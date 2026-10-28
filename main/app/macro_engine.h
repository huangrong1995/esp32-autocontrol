#pragma once

/*
 * Macro engine public API — NVS persistence + FreeRTOS playback task + state
 * machine wrapping the pure core (app/macro_core.h). Task 4b.
 *
 * All functions are callable from C (web_server.cc REST handlers); the
 * implementation is C++ (macro_engine.cc).
 *
 * State machine: IDLE / RECORDING / PLAYING. All non-init functions are safe
 * no-ops or return an error when called in the wrong state (never crash).
 */

#include <stdint.h>
#include "esp_err.h"
#include "app/macro_types.h"   /* macro_info_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Playback / recording state as exposed to GET /api/status. */
typedef enum {
    MACRO_STATE_IDLE = 0,
    MACRO_STATE_RECORDING,
    MACRO_STATE_PLAYING,
} macro_state_t;

#define MACRO_STATE_STR_IDLE      "idle"
#define MACRO_STATE_STR_RECORDING "recording"
#define MACRO_STATE_STR_PLAYING   "playing"

void macro_engine_init(void);

/* Begin recording a new macro. Errors when already recording / playing. */
esp_err_t macro_start_recording(void);

/* Append one step while recording (wait_ms computed by the caller from
 * esp_timer since the previous step). Safe no-op-ish error when not recording
 * or the 100-step cap is reached. */
esp_err_t macro_record_step(ir_key_t key, uint32_t wait_ms);

/* Persist the in-progress recording under `name` (<= 23 chars) + add it to
 * the NVS macro index. Errors on a 0-step recording / index full / NVS
 * failure. Leaves the engine IDLE afterwards. */
esp_err_t macro_save(const char *name);

/* Start playback of the stored macro `id`. Creates the FreeRTOS playback task
 * which splays through the currently paired profile via ir_send_key().
 * Errors when unpaired, id not found, or already playing. */
esp_err_t macro_play(uint8_t id);

/* Abort playback (also a safe no-op when idle/recording) and, when recording,
 * abandon the recording and return to IDLE. */
esp_err_t macro_stop(void);

/* Abort active playback ONLY (急停 hook, called unconditionally before the
 * httpd sends an interrupting key). Safe no-op when not playing; leaves a
 * recording untouched. */
esp_err_t macro_interrupt(void);

/* Delete the stored macro `id` (index entry + steps blob + NVS). */
esp_err_t macro_delete(uint8_t id);

/* Number of stored macros in the in-memory index. */
uint8_t macro_get_count(void);

/* Pointer to the in-memory macro index array (macro_get_count() entries).
 * Valid until the next macro_save/delete/engine-init. */
const macro_info_t *macro_get_list(void);

/* Copy up to `max` macro_info_t entries into `out` while holding the engine
 * lock, and store the copied count into *count (or 0 on NULL args). Safe for
 * callers that build a response without racing a concurrent save/delete. */
void macro_macros_snapshot(macro_info_t *out, uint8_t max, uint8_t *count);

/* Current engine state (for GET /api/status). */
macro_state_t macro_get_state(void);

/* Current playback step number, 1-based (0 = not playing / between steps).
 * Read by GET /api/status to render replay progress. */
uint16_t macro_get_playback_step(void);

/* Whether a replay is active right now. */
int macro_is_playing(void);

/* Whether recording is active right now. */
int macro_is_recording(void);

/* Bind macro `id` as the one-key (physical button) macro, replacing any prior
 * binding. ESP_ERR_NOT_FOUND when `id` is not a stored macro. The binding
 * persists in NVS ("macros"/"one_key") so it survives reboot. */
esp_err_t macro_set_onekey(uint8_t id);

/* Clear the one-key binding if it currently equals `id` (no-op otherwise;
 * always ESP_OK for a non-bound id). */
esp_err_t macro_clear_onekey(uint8_t id);

/* Current one-key binding (macro id), or MACRO_ONEKEY_NONE (0xFF) when none.
 * Read by GET /api/status and the button task; a single atomic byte read. */
uint8_t macro_get_onekey(void);

/* Power-on auto-replay flag ("macros"/"auto_play", u8 0/1, default off).
 * When enabled, macro_engine_selfstart() replays the current one-key macro
 * once after boot. Switch `on`: 1 = enable, 0 = disable; NVS is written only
 * on an actual change. */
esp_err_t macro_set_selfstart(uint8_t on);

/* Current power-on auto-replay flag (0 or 1). */
uint8_t macro_get_selfstart(void);

/* Power-on auto-replay hook: when the selfstart flag is on AND a one-key macro
 * is bound AND the device is paired, starts playback of the one-key macro once.
 * Otherwise a logged no-op (also when unpaired). Call ONCE from app_main after
 * macro_engine_init() (RMT TX init is lazy, so playback may begin immediately). */
void macro_engine_selfstart(void);

#ifdef __cplusplus
}
#endif
