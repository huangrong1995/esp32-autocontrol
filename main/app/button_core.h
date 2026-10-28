#pragma once

/*
 * Pure button debounce core — no IDF / FreeRTOS dependency, host-compilable
 * with plain `cc` (exactly like app/macro_core.h and app/web_logic.h). The
 * firmware glue (app/button_engine.cc) polls the GPIO, feeds each 20 ms level
 * sample through btn_debounce_sample(), and treats a return of 1 as "one press
 * edge completed — fire the action".
 *
 * Design:
 *  - Stable-count debounce: a level change must hold for BTN_DEBOUNCE_SAMPLES
 *    consecutive samples before it counts (3 x 20 ms == 60 ms total).
 *  - `seen_release` gates the very first press after init: a button HELD at
 *    power-on (or held when init runs) must NOT fire until it is released and
 *    pressed again — the seeding init sees the held-low level as "stable", so
 *    without the gate an already-pressed button would fire 60 ms into boot.
 *  - `fired` is set when the press edge completes (debounced low -> sample
 *    returns 1 once) and is cleared on the next debounced release edge, so one
 *    press yields exactly one 1 no matter how long it is held.
 *  - `prev_level` is the last DEBOUNCED level, not the raw sample; each raw
 *    sample is compared against it so bounce on a held button re-starts the
 *    stable counter without flipping the debounced output.
 */

#include <stdint.h>

#define BTN_DEBOUNCE_SAMPLES 3u   /* consecutive samples for a level change */
#define BTN_ACTIVE_LEVEL     0u   /* button pressed == GPIO low (gnd, pull-up) */

typedef struct {
    uint8_t prev_level;      /* last debounced level (0/1) */
    uint8_t stable;          /* consecutive samples matching prev_level */
    uint8_t fired;           /* press edge reported, pending release re-arm */
    uint8_t seen_release;    /* set once a debounced release is seen post-init */
} btn_debounce_t;

/* Seed the state from the CURRENT (possibly held at power-on) level: the
 * debounced level starts at the current sample, no press may fire until a
 * release is debounced first. Call once after gpio_set_direction. */
static inline void btn_debounce_init(btn_debounce_t *b, uint8_t current_level)
{
    b->prev_level    = current_level ? 1u : 0u;
    b->stable        = BTN_DEBOUNCE_SAMPLES;   /* current level is trusted */
    b->fired         = 0u;
    b->seen_release  = (b->prev_level != BTN_ACTIVE_LEVEL);  /* already released? */
}

/* Feed one raw level sample (taken every ~20 ms). Returns 1 exactly once per
 * debounced press edge (a complete press+release cycle), else 0. A press that
 * starts before a release (power-on held) is withheld by seen_release. */
static inline unsigned btn_debounce_sample(btn_debounce_t *b, uint8_t raw_level)
{
    uint8_t level = raw_level ? 1u : 0u;
    if (level != b->prev_level) {
        /* Possible change: require BTN_DEBOUNCE_SAMPLES consecutive samples. */
        b->stable = 0u;
        b->prev_level = level;    /* probe the new level */
    }
    /* Count samples matching the current debounced (probed) level. */
    if (level == b->prev_level) {
        if (b->stable < BTN_DEBOUNCE_SAMPLES) {
            b->stable++;
        }
        if (b->stable >= BTN_DEBOUNCE_SAMPLES) {
            /* The probed level is confirmed. */
            if (level == BTN_ACTIVE_LEVEL) {
                if (!b->seen_release) {
                    return 0u;    /* power-on held press: wait for a release */
                }
                if (!b->fired) {
                    b->fired = 1u;
                    return 1u;    /* press edge completed -> fire */
                }
                return 0u;        /* still held: no repeat */
            } else {              /* confirmed release edge */
                b->seen_release = 1u;
                b->fired = 0u;    /* re-arm for the next press */
            }
        }
        return 0u;
    }
    return 0u;
}