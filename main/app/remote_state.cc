#include "app/remote_state.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "ir/ir_encoder.h"   /* IR_KEY_COUNT (calibration blob length) */

static const char *TAG = "remote_state";

/* Pairing state lives in NVS namespace "pair", key "pair_profile" (u8).
 * Default 0xFF = not paired. NVS is initialised exactly once, inside
 * wifi_ap_start(); these functions tolerate NVS being unavailable and fall
 * back to sane defaults instead of crashing (relevant until startup wiring
 * exists, and as a defensive guard). */
#define NVS_PAIR_NAMESPACE   "pair"
#define NVS_KEY_PAIR_PROFILE "pair_profile"
#define NVS_KEY_PAIR_CALIB   "pair_calib"
#define NVS_KEY_PAIR_GROUP   "pair_group"
#define NVS_PAIR_CALIB_LEN   IR_KEY_COUNT /* 23 bytes */

uint8_t remote_state_get_profile(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PAIR_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0xFF; /* unpaired */
    }
    uint8_t v = 0xFF;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_PAIR_PROFILE, &v);
    nvs_close(h);
    if (err != ESP_OK) {
        return 0xFF; /* key missing / read error -> unpaired */
    }
    return v;
}

void remote_state_set_profile(uint8_t profile_id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PAIR_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, NVS_KEY_PAIR_PROFILE, profile_id) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---- NVS calibrated-keymap API (Task 2) -------------------------------- */

int pair_calib_read_nvs(uint8_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PAIR_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    size_t len = NVS_PAIR_CALIB_LEN;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_PAIR_CALIB, out, &len);
    nvs_close(h);
    return (err == ESP_OK && len == NVS_PAIR_CALIB_LEN) ? 1 : 0;
}

/*
 * Write point for the guided-pairing completion path (the only pair-calibration
 * writer; NVS pairing state is written EXCLUSIVELY when the wizard completes —
 * cancel / reboot / no-match must never write). Storing a calibration
 * atomically writes the 23-byte blob (pair_calib) + its source group
 * (pair_group), then ERASES the legacy pair_profile so a stale profile cannot
 * resurrect as the fallback after calibration. On any failure the whole
 * transaction is left uncommitted and the error is logged.
 */
void pair_calib_store_nvs(const uint8_t *calib, uint8_t source_group)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PAIR_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "pair_calib_store_nvs: nvs_open failed");
        return;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY_PAIR_CALIB, calib, NVS_PAIR_CALIB_LEN);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_PAIR_GROUP, source_group);
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY_PAIR_PROFILE);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* legacy pair_profile never written → not an error */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pair_calib_store_nvs failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

int pair_calib_active(void)
{
    uint8_t probe[NVS_PAIR_CALIB_LEN];
    return pair_calib_read_nvs(probe);
}

uint8_t pair_calib_group(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PAIR_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0xFF;
    }
    uint8_t v = 0xFF;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_PAIR_GROUP, &v);
    nvs_close(h);
    if (err != ESP_OK) {
        return 0xFF;
    }
    return v;
}
