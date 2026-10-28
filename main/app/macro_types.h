#pragma once

/*
 * Shared macro data structures + limits (no state, host-compilable).
 *
 * Task 4b extracted the pure structs/limits out of macro_core.h so that
 * app/web_logic.h (host-compilable HTTP-dispatch logic) can reference
 * macro_info_t WITHOUT pulling in macro_core.h's per-TU static state. This
 * header depends only on stdint/stddef + ir_encoder.h (for ir_key_t).
 */

#include <stdint.h>
#include <stddef.h>

#include "ir/ir_encoder.h"   /* ir_key_t */

/* ---- data structures (brief Step 1) ---- */

typedef struct {
    ir_key_t  key;
    uint32_t  wait_ms;   /* ms to wait BEFORE sending `key` */
} macro_step_t;

typedef struct {
    uint8_t   id;
    char      name[24];
    uint16_t  step_count;
    ir_key_t  first_key;
} macro_info_t;

/* ---- limits ---- */
#define MACRO_MAX_STEPS      100u   /* brief Step 2: temp steps array cap */
#define MACRO_INDEX_MAX       16u   /* max macros in the NVS index */
#define MACRO_MAX_ID         255u   /* exclusive: ids 0..254, full-wipe at 255 */
#define MACRO_STEP_ENCODED_BYTES 5u /* per-step blob: u32le wait + u8 key */
#define MACRO_REPLAY_SLEEP_CHUNK_MS 50u /* interruptible wait granularity */
#define MACRO_NAME_CAP       24u    /* macro_info_t::name bytes incl NUL */

/* One-key (physical button) macro binding: a macro id, or 0xFF when none.
 * Macro ids are 0..254 (see MACRO_MAX_ID), so 0xFF can never collide — safe
 * NVS sentinel and unsafe-default. Kept here (state-free) so the button engine
 * and status formatter can reference it WITHOUT including macro_core.h's
 * per-TU static state. */
#define MACRO_ONEKEY_NONE    0xFFu
