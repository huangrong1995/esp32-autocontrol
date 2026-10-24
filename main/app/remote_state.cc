#include "app/remote_state.h"

#include "nvs_flash.h"
#include "nvs.h"

/* Pairing state lives in NVS namespace "pair", key "pair_profile" (u8).
 * Default 0xFF = not paired. NVS is initialised exactly once, inside
 * wifi_ap_start(); these functions tolerate NVS being unavailable and fall
 * back to sane defaults instead of crashing (relevant until startup wiring
 * exists, and as a defensive guard). */
#define NVS_PAIR_NAMESPACE   "pair"
#define NVS_KEY_PAIR_PROFILE "pair_profile"

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
