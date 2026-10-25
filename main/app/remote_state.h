#pragma once
#include <stdint.h>

uint8_t remote_state_get_profile(void);
void remote_state_set_profile(uint8_t profile_id);

/* NVS calibrated-keymap API (Task 2). A calibration is an IR_KEY_COUNT-byte
 * raw NEC command map plus the source-group profile id that supplies the NEC
 * address. See app/pair_state.h for the resolution semantics.
 *   pair_calib_read_nvs(out)  : reads the stored blob; returns 1 iff a valid
 *                               23-byte blob exists (then *out is filled).
 *   pair_calib_store_nvs(...) : THE ONLY write entry for pairing state.
 *   pair_calib_active()       : 1 iff a valid calibration is stored.
 *   pair_calib_group()        : stored source-group id (0xFF when none). */
int  pair_calib_read_nvs(uint8_t *out);
void pair_calib_store_nvs(const uint8_t *calib, uint8_t source_group);
int  pair_calib_active(void);
uint8_t pair_calib_group(void);
