#pragma once

/*
 * Pure pairing-resolution logic (Task 2 of the guided-pairing plan).
 *
 * Everything here is `static inline` and depends only on the host-compilable
 * IR headers (ir_encoder.h / ir_db.h) — no IDF / NVS dependency. Both
 * ir_encoder.cc (firmware) and test_web_logic.c (host self-test) include it,
 * so the calibrated-keymap resolution layer is testable with plain `cc`.
 *
 * Data model:
 *   - A "calibration" is an IR_KEY_COUNT (23) byte raw keymap captured from a
 *     learned/copied remote: `calib[key]` is the NEC command byte for that
 *     key; 0xFF marks an undefined/unlearned key (never fabricated).
 *   - A calibration belongs to a "source group" (profile_id) whose built-in
 *     profile supplies the NEC *address* (nec_address) — the calibration only
 *     stores command bytes, not addresses.
 *   - When no calibration exists, resolution falls back to the built-in
 *     profile table exactly as before (legacy path).
 */

#include <stddef.h>
#include <stdint.h>

#include "ir/ir_db.h"       /* ir_profile_t, ir_db_get_profile */
#include "ir/ir_encoder.h"  /* ir_key_t, IR_KEY_COUNT */

/*
 * Return the calibrated command byte for `key`, or 0xFF when there is no
 * calibration / bad length / key out of range.
 */
static inline int pair_lookup(const uint8_t *calib, size_t calib_len, ir_key_t key)
{
    if (calib == NULL || calib_len != IR_KEY_COUNT || (unsigned)key >= IR_KEY_COUNT) {
        return 0xFF;
    }
    return calib[key];
}

/*
 * Resolve `key` to a sendable NEC (addr, cmd):
 *   - invalid args (NULL addr/cmd, key out of range)       -> 0 (never crash)
 *   - profile_id out of range (ir_db_get_profile == NULL)  -> 0
 *   - cmd byte == 0xFF (undefined, not fabricated)         -> 0 (don't send)
 *   - otherwise fills *addr=profile->nec_address, *cmd=c;  -> 1.
 * `calib` wins when present AND calib_len == IR_KEY_COUNT (len != 23 is
 * treated as legacy); otherwise the built-in profile table supplies `cmd`.
 */
static inline int pair_resolve_key(const uint8_t *calib, size_t calib_len,
                                   uint8_t profile_id, ir_key_t key,
                                   uint16_t *addr, uint8_t *cmd)
{
    if (addr == NULL || cmd == NULL || (unsigned)key >= IR_KEY_COUNT) {
        return 0;
    }
    const ir_profile_t *p = ir_db_get_profile(profile_id);
    if (p == NULL) {
        return 0;
    }
    uint8_t c = (calib != NULL && calib_len == IR_KEY_COUNT) ? calib[key] : p->cmds[key];
    if (c == 0xFF) {
        return 0;
    }
    *addr = p->nec_address;
    *cmd = c;
    return 1;
}
