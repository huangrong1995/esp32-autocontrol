/*
 * Macro engine implementation — NVS persistence + FreeRTOS playback task +
 * state machine over the pure core (app/macro_core.h). Task 4b.
 *
 * macro_core.h keeps its static recording buffer + index in THIS TU only
 * (macro_engine.cc is the single firmware TU that includes it; the host
 * self-test gets its own independent copy).
 *
 * Lifecycle / concurrency:
 *  - State transitions happen with the FreeRTOS mutex held (brief: no
 *    deadlock — the httpd thread takes the mutex to start/stop/record; the
 *    playback task ALSO takes it, so the task never holds it while an httpd
 *    thread waits on it).
 *  - The recording buffer is only ever touched while holding the mutex.
 *  - The playback task runs macro_core_replay() on a snapshot taken while
 *    holding the mutex; it reads only the volatile stop flag + step counter
 *    between mutex acquisitions. The task never holds the mutex while
 *    sleeping or sending.
 */

#include "app/macro_engine.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app/macro_core.h"    /* pure core (static state lives in THIS TU) */
#include "app/remote_state.h"  /* remote_state_get_profile() */
#include "ir/ir_encoder.h"     /* ir_send_key() */
#include "ir/ir_db.h"          /* ir_db_get_profile_count() */

static const char *TAG = "macro_engine";

#define MACRO_NVS_NAMESPACE   "macros"
#define MACRO_NVS_INDEX_KEY   "macro_index"
#define MACRO_NVS_NEXT_KEY    "macro_next"
#define MACRO_NVS_ONEKEY_KEY  "one_key"
#define MACRO_NVS_SELFSTART_KEY "auto_play"
#define MACRO_STACK_BYTES     4096
#define MACRO_STACK_WORDS     (MACRO_STACK_BYTES / sizeof(StackType_t))
#define MACRO_INDEX_BLOB_BYTES (sizeof(macro_info_t) * MACRO_INDEX_MAX)
#define MACRO_STEPS_BLOB_BYTES (MACRO_STEP_ENCODED_BYTES * MACRO_MAX_STEPS)

/* ---- engine state ---- */

static macro_state_t  s_state;
static volatile uint8_t  s_play_stop;          /* abort flag for macro_core_replay */
static volatile uint16_t s_play_step;          /* 1-based current playback step */
static volatile uint8_t  s_one_key;            /* one-key macro binding, 0xFF = none */
static volatile uint8_t  s_selfstart;          /* power-on auto-replay flag, 0/1 */
static uint8_t         s_snapshot_step_count;
static macro_step_t    s_snapshot[MACRO_MAX_STEPS];
static TaskHandle_t    s_play_task;
static SemaphoreHandle_t s_mutex;

/* ---- helpers ---- */

static const char *state_str(macro_state_t st)
{
    switch (st) {
    case MACRO_STATE_IDLE:      return MACRO_STATE_STR_IDLE;
    case MACRO_STATE_RECORDING: return MACRO_STATE_STR_RECORDING;
    case MACRO_STATE_PLAYING:   return MACRO_STATE_STR_PLAYING;
    }
    return MACRO_STATE_STR_IDLE;
}

/* ---- NVS persistence ---- */

/* (Re)build the in-memory macro index from NVS. Safe to call anytime;
 * on read failure the index is emptied + logged. */
static void macro_index_load_from_nvs(void)
{
    macro_core_index_reset();
    nvs_handle_t h;
    if (nvs_open(MACRO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = MACRO_INDEX_BLOB_BYTES;
    esp_err_t err = nvs_get_blob(h, MACRO_NVS_INDEX_KEY,
                                 macro_core_index, &len);
    uint8_t v = 0;
    if (nvs_get_u8(h, MACRO_NVS_NEXT_KEY, &v) == ESP_OK) {
        macro_core_index_set_next_id(v);
    }
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;   /* no macros stored yet: keep empty index */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read macro index failed: %s", esp_err_to_name(err));
        macro_core_index_reset();
        return;
    }
    macro_core_index_count = (uint16_t)(len / sizeof(macro_info_t));
    if (macro_core_index_count > MACRO_INDEX_MAX) {
        macro_core_index_count = MACRO_INDEX_MAX;
    }
}

/* Load the persisted one-key macro binding (u8, 0xFF sentinel = none). On a
 * missing key or NVS error the binding starts cleared. */
static void macro_onekey_load_from_nvs(void)
{
    s_one_key = MACRO_ONEKEY_NONE;
    nvs_handle_t h;
    if (nvs_open(MACRO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = MACRO_ONEKEY_NONE;
    esp_err_t err = nvs_get_u8(h, MACRO_NVS_ONEKEY_KEY, &v);
    nvs_close(h);
    if (err == ESP_OK) {
        s_one_key = v;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read one-key binding failed: %s", esp_err_to_name(err));
    }
}

/* Persist the in-memory macro index blob to NVS. Returns ESP_OK on success. */
static esp_err_t macro_index_store_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, MACRO_NVS_INDEX_KEY, macro_core_index,
                       macro_core_index_count * sizeof(macro_info_t));
    if (err == ESP_OK) {
        err = nvs_set_u8(h, MACRO_NVS_NEXT_KEY,
                         macro_core_index_get_next_id());
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Persist the one-key macro binding (u8, 0xFF sentinel = none). */
static esp_err_t macro_onekey_store(uint8_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, MACRO_NVS_ONEKEY_KEY, id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Load the persisted power-on auto-replay flag (u8, 0/1). On a missing key or
 * NVS error the flag starts off (0) — auto-replay is always opt-in. */
static void macro_selfstart_load_from_nvs(void)
{
    s_selfstart = 0;
    nvs_handle_t h;
    if (nvs_open(MACRO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, MACRO_NVS_SELFSTART_KEY, &v);
    nvs_close(h);
    if (err == ESP_OK) {
        s_selfstart = (v != 0u) ? 1u : 0u;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read auto-replay flag failed: %s", esp_err_to_name(err));
    }
}

/* Persist the power-on auto-replay flag (u8, 0/1). */
static esp_err_t macro_selfstart_store(uint8_t on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, MACRO_NVS_SELFSTART_KEY, on ? 1u : 0u);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Steps blob key for macro id: "macro_<id>" (NVS keys cap at 15 chars;
 * "macro_" is 6 + up to 3 decimal digits == 9 chars, safely under). */
static void macro_steps_key(char *out, size_t cap, uint8_t id)
{
    snprintf(out, cap, "macro_%u", (unsigned)id);
}

/* Save `steps`/`n` as the steps blob for `info`. */
static esp_err_t macro_steps_store(const macro_info_t *info,
                                   const macro_step_t *steps, uint16_t n)
{
    uint8_t blob[MACRO_STEPS_BLOB_BYTES];
    size_t blen = macro_core_serialize(steps, n, blob, sizeof(blob));
    if (blen == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char key[16];
    macro_steps_key(key, sizeof(key), info->id);
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, blob, blen);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Load the steps blob for macro `info` into `out` (capacity `max`). Returns
 * the step count on success, else 0. */
static uint16_t macro_steps_load(const macro_info_t *info,
                                 macro_step_t *out, uint16_t max)
{
    char key[16];
    macro_steps_key(key, sizeof(key), info->id);
    uint8_t blob[MACRO_STEPS_BLOB_BYTES];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    if (nvs_open(MACRO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    esp_err_t err = nvs_get_blob(h, key, blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read steps for macro id=%u failed: %s",
                 (unsigned)info->id, esp_err_to_name(err));
        return 0;
    }
    return macro_core_deserialize(blob, len, out, max);
}

/* Erase the steps blob for `id` from NVS (delete path). */
static esp_err_t macro_steps_erase(uint8_t id)
{
    char key[16];
    macro_steps_key(key, sizeof(key), id);
    nvs_handle_t h;
    esp_err_t err = nvs_open(MACRO_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- playback task ---- */

/* Interruptible sleep: yield in <=50ms chunks while polling the stop flag,
 * so a long wait_ms aborts promptly (brief Step 3). */
static void macro_task_sleep_ms(uint32_t ms)
{
    while (ms > 0 && !s_play_stop) {
        uint32_t piece = (ms > MACRO_REPLAY_SLEEP_CHUNK_MS)
                       ? MACRO_REPLAY_SLEEP_CHUNK_MS : ms;
        vTaskDelay(pdMS_TO_TICKS(piece));
        ms -= piece;
    }
}

/* Send wrapper: bump the 1-based progress counter, then replay the key via
 * the paired profile. GET /api/status reads macro_get_playback_step(). */
static void macro_task_send(ir_key_t key)
{
    s_play_step = (uint16_t)(s_play_step + 1);
    ir_send_key(key);
}

/* Task teardown: if this task is still the registered playback task, clear
 * the handle/state/counters. Guards against a dying task clobbering a newer
 * play (macro_stop/interrupt set the flag but leave state=PLAYING, so the
 * engine stays busy until the task actually exits — no zombie double-send). */
static void macro_playback_task_exit(void)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_play_task == NULL || s_play_task == xTaskGetCurrentTaskHandle()) {
            s_play_task = NULL;
            s_state = MACRO_STATE_IDLE;
            s_play_step = 0;
            s_play_stop = 0;
        }
        xSemaphoreGive(s_mutex);
    }
    vTaskDelete(NULL);
}

static void macro_playback_task(void *arg)
{
    (void)arg;

    /* Snapshot the recorded steps under the mutex (macro_play already filled
     * the snapshot; copy it at task scope so the task is self-contained). */
    uint16_t n = 0;
    macro_step_t steps[MACRO_MAX_STEPS];
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        n = s_snapshot_step_count;
        memcpy(steps, s_snapshot, (size_t)n * sizeof(macro_step_t));
        xSemaphoreGive(s_mutex);
    }
    if (n == 0) {
        /* Snapshot unavailable (only possible if the mutex take failed):
         * release the PLAYING state before exiting, so the engine recovers. */
        macro_playback_task_exit();
        return;
    }

    s_play_step = 0;
    ESP_LOGI(TAG, "playback started (%u steps)", (unsigned)n);

    /* macro_core_replay sends via macro_task_send (paired-profile send that
     * also bumps the progress counter) and sleeps via macro_task_sleep_ms
     * (abort-interruptible chunks). NOTE: s_play_stop is NOT cleared here —
     * macro_play_locked zeroed it before creating this task, and a stop
     * requested in the window before we run must not be swallowed. */
    int sent = macro_core_replay(steps, n, macro_task_send,
                                 macro_task_sleep_ms, &s_play_stop);

    ESP_LOGI(TAG, "playback %s (%d steps completed)",
             (s_play_stop ? "aborted" : "finished"), sent);

    macro_playback_task_exit();
}

/* ---- public API ---- */

void macro_engine_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }
    s_state = MACRO_STATE_IDLE;
    s_play_stop = 0;
    s_play_step = 0;
    s_play_task = NULL;
    macro_core_record_reset();
    macro_index_load_from_nvs();
    macro_onekey_load_from_nvs();
    macro_selfstart_load_from_nvs();
    ESP_LOGI(TAG, "macro engine init: %u stored macro(s), one-key=%u, auto_play=%u",
             (unsigned)macro_core_index_count, (unsigned)s_one_key,
             (unsigned)s_selfstart);
}

esp_err_t macro_start_recording(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ret;
    }
    if (s_state == MACRO_STATE_IDLE) {
        macro_core_record_start();
        s_state = MACRO_STATE_RECORDING;
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "cannot start recording in state %s", state_str(s_state));
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t macro_record_step(ir_key_t key, uint32_t wait_ms)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ret;
    }
    if (s_state == MACRO_STATE_RECORDING) {
        ret = (macro_core_record_add(key, wait_ms) == 0) ? ESP_OK
                                                         : ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

/* macro_save body, assumes the engine mutex is HELD. */
static esp_err_t macro_save_locked(const char *name)
{
    if (s_state != MACRO_STATE_RECORDING) {
        ESP_LOGW(TAG, "macro_save: not recording");
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t n = macro_core_record_step_count();
    if (n == 0) {
        ESP_LOGW(TAG, "macro_save: refusing to save a 0-step macro");
        macro_core_record_reset();
        s_state = MACRO_STATE_IDLE;
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t id = 0;
    ir_key_t first_key = macro_core_record_first_key();
    if (macro_core_index_add(name, n, first_key, &id) != 0) {
        ESP_LOGE(TAG, "macro_save: index full / invalid name");
        macro_core_record_reset();
        s_state = MACRO_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    const macro_info_t *info = macro_core_index_get_by_id(id);
    if (info == NULL) {
        macro_core_index_remove(id);
        macro_core_record_reset();
        s_state = MACRO_STATE_IDLE;
        return ESP_FAIL;
    }
    esp_err_t ret = macro_steps_store(info, macro_core_record_steps(), n);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "macro_save: steps blob store failed: %s",
                 esp_err_to_name(ret));
        macro_core_index_remove(id);
        macro_core_record_reset();
        s_state = MACRO_STATE_IDLE;
        return ret;
    }
    ret = macro_index_store_to_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "macro_save: index blob store failed: %s",
                 esp_err_to_name(ret));
        esp_err_t e = macro_steps_erase(id);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "macro_save rollback: steps erase for id=%u failed: %s",
                     (unsigned)id, esp_err_to_name(e));
        }
        macro_core_index_remove(id);
        macro_core_record_reset();
        s_state = MACRO_STATE_IDLE;
        return ret;
    }
    macro_core_record_clear_dirty();
    macro_index_load_from_nvs();   /* resync count from authoritative blob */
    macro_core_record_reset();
    s_state = MACRO_STATE_IDLE;
    ESP_LOGI(TAG, "saved macro id=%u name='%s' steps=%u", (unsigned)id, name, (unsigned)n);
    return ESP_OK;
}

esp_err_t macro_save(const char *name)
{
    if (s_mutex == NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = macro_save_locked(name);
    xSemaphoreGive(s_mutex);
    return ret;
}

/* macro_play body, assumes the engine mutex is HELD. */
static esp_err_t macro_play_locked(uint8_t id)
{
    if (s_state != MACRO_STATE_IDLE) {
        ESP_LOGW(TAG, "macro_play: busy in state %s", state_str(s_state));
        return ESP_ERR_INVALID_STATE;
    }
    const macro_info_t *info = macro_core_index_get_by_id(id);
    if (info == NULL || info->step_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t n = macro_steps_load(info, s_snapshot, MACRO_MAX_STEPS);
    if (n == 0 || n != info->step_count) {
        ESP_LOGE(TAG, "macro_play: steps blob for id=%u unreadable/mismatch",
                 (unsigned)id);
        return ESP_ERR_NOT_FOUND;
    }
    s_snapshot_step_count = n;
    s_state = MACRO_STATE_PLAYING;
    s_play_stop = 0;
    s_play_step = 0;
    s_play_task = NULL;
    if (xTaskCreate(macro_playback_task, "macro_play", MACRO_STACK_WORDS,
                    NULL, tskIDLE_PRIORITY + 2, &s_play_task) != pdPASS) {
        ESP_LOGE(TAG, "macro_play: playback task create failed");
        s_state = MACRO_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t macro_play(uint8_t id)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A valid wizard calibration counts as paired even when the legacy
     * profile was erased (calib pairing stores the source group in NVS and
     * clears pair_profile).  Mirror the unified test in handle_api_key so
     * macro playback works for calib-only pairings (raw keysets included). */
    if (!pair_calib_active() &&
        remote_state_get_profile() >= ir_db_get_profile_count()) {
        return ESP_ERR_INVALID_STATE;   /* unpaired -> 400 "not paired" */
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = macro_play_locked(id);
    xSemaphoreGive(s_mutex);
    return ret;
}

/* Abort active playback by setting the stop flag; the playback task alone
 * owns the PLAYING->IDLE transition, so while it winds down (<50ms) the
 * engine still reports PLAYING and refuses a new play/delete (no zombie
 * double-send, no state clobber). When recording, abandons it immediately. */
esp_err_t macro_stop(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == MACRO_STATE_PLAYING) {
        s_play_stop = 1;   /* task notices within <=50ms, then resets state */
        ESP_LOGI(TAG, "playback stop requested");
    } else if (s_state == MACRO_STATE_RECORDING) {
        macro_core_record_reset();   /* abandon the in-progress recording */
        s_state = MACRO_STATE_IDLE;
        ESP_LOGI(TAG, "recording abandoned (macro_stop)");
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t macro_interrupt(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == MACRO_STATE_PLAYING) {
        s_play_stop = 1;   /* task notices within <=50ms, then resets state */
        ESP_LOGI(TAG, "playback interrupted by key press");
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

/* macro_delete body, assumes the engine mutex is HELD. */
static esp_err_t macro_delete_locked(uint8_t id)
{
    if (s_state == MACRO_STATE_PLAYING) {
        ESP_LOGW(TAG, "macro_delete: refuse while playing");
        return ESP_ERR_INVALID_STATE;
    }
    if (macro_core_index_find(id) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (macro_core_index_remove(id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)macro_steps_erase(id);   /* non-fatal if the steps blob is absent */
    /* A one-key binding pointing at the deleted macro must not dangle; clear
     * it best-effort (a surviving binding would still be harmless — the
     * button path macro_play() returns NOT_FOUND for a vanished id). */
    uint8_t prior_one_key = (uint8_t)s_one_key;
    s_one_key = macro_core_onekey_after_delete(prior_one_key, id);
    if (s_one_key != prior_one_key) {
        esp_err_t e = macro_onekey_store(MACRO_ONEKEY_NONE);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "macro_delete: clearing one-key binding failed: %s",
                     esp_err_to_name(e));
        }
    }
    esp_err_t ret = macro_index_store_to_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "macro_delete: index store failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    macro_index_load_from_nvs();
    ESP_LOGI(TAG, "deleted macro id=%u", (unsigned)id);
    return ESP_OK;
}

esp_err_t macro_delete(uint8_t id)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = macro_delete_locked(id);
    xSemaphoreGive(s_mutex);
    return ret;
}

uint8_t macro_get_count(void)
{
    if (s_mutex == NULL) {
        return 0;
    }
    uint8_t c = 0;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    c = (uint8_t)macro_core_index_count;
    xSemaphoreGive(s_mutex);
    return c;
}

const macro_info_t *macro_get_list(void)
{
    /* Returns the core's static array; only valid while holding the engine
     * mutex. Prefer macro_macros_snapshot() from any thread that builds a
     * response (e.g. the httpd status handler). */
    return macro_core_index_get_all();
}

void macro_macros_snapshot(macro_info_t *out, uint8_t max, uint8_t *count)
{
    if (out == NULL || count == NULL) {
        return;
    }
    *count = 0;
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint8_t n = (uint8_t)macro_core_index_count;
    if (n > max) {
        n = max;
    }
    uint8_t i;
    for (i = 0; i < n; i++) {
        out[i] = macro_core_index[i];
    }
    *count = n;
    xSemaphoreGive(s_mutex);
}

macro_state_t macro_get_state(void)
{
    if (s_mutex == NULL) {
        return MACRO_STATE_IDLE;
    }
    macro_state_t st = MACRO_STATE_IDLE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return MACRO_STATE_IDLE;
    }
    st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

uint16_t macro_get_playback_step(void)
{
    return (uint16_t)s_play_step;
}

int macro_is_playing(void)
{
    return (macro_get_state() == MACRO_STATE_PLAYING);
}

int macro_is_recording(void)
{
    return (macro_get_state() == MACRO_STATE_RECORDING);
}

esp_err_t macro_set_onekey(uint8_t id)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (macro_core_index_find(id) < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = macro_onekey_store(id);
    if (err == ESP_OK) {
        s_one_key = id;
        ESP_LOGI(TAG, "one-key macro bound to id=%u", (unsigned)id);
    } else {
        ESP_LOGE(TAG, "macro_set_onekey: NVS store failed: %s",
                 esp_err_to_name(err));
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t macro_clear_onekey(uint8_t id)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_one_key == id) {
        esp_err_t err = macro_onekey_store(MACRO_ONEKEY_NONE);
        if (err == ESP_OK) {
            s_one_key = MACRO_ONEKEY_NONE;
            ESP_LOGI(TAG, "one-key macro cleared (was id=%u)", (unsigned)id);
        } else {
            ESP_LOGE(TAG, "macro_clear_onekey: NVS store failed: %s",
                     esp_err_to_name(err));
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

uint8_t macro_get_onekey(void)
{
    return (uint8_t)s_one_key;
}

/* Set or clear the power-on auto-replay flag (0/1). NVS is written only on an
 * actual change, so a flag that never flips causes no flash wear. */
esp_err_t macro_set_selfstart(uint8_t on)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    on = (on != 0u) ? 1u : 0u;
    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_selfstart == on) {
        xSemaphoreGive(s_mutex);
        return ret;
    }
    esp_err_t err = macro_selfstart_store(on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "macro_set_selfstart: NVS store failed: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }
    s_selfstart = on;
    ESP_LOGI(TAG, "power-on auto-replay %s", on ? "enabled" : "disabled");
    xSemaphoreGive(s_mutex);
    return ret;
}

uint8_t macro_get_selfstart(void)
{
    return (uint8_t)s_selfstart;
}

/* Power-on auto-replay hook. Mirrors the pairing gate of macro_play() (which
 * also carries its own busy gate), so boot replay behaves exactly like a
 * page/button-triggered playback — no IR when unpaired, no IR before the
 * engine is ready. Call ONCE from app_main after macro_engine_init(). */
void macro_engine_selfstart(void)
{
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "selfstart: engine not initialized");
        return;
    }
    if (!s_selfstart) {
        ESP_LOGI(TAG, "selfstart: power-on auto-replay disabled");
        return;
    }
    uint8_t one_key = (uint8_t)s_one_key;
    if (one_key == MACRO_ONEKEY_NONE) {
        ESP_LOGW(TAG, "selfstart: no one-key macro bound");
        return;
    }
    if (!pair_calib_active() &&
        remote_state_get_profile() >= ir_db_get_profile_count()) {
        ESP_LOGW(TAG, "selfstart: not paired, skipping auto-replay");
        return;
    }
    ESP_LOGI(TAG, "selfstart: playing one-key macro id=%u", (unsigned)one_key);
    esp_err_t err = macro_play(one_key);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "selfstart: replay returned %s", esp_err_to_name(err));
    }
}
