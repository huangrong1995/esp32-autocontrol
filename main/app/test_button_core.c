/*
 * Host self-test for the pure button debounce core (app/button_core.h).
 * No RMT / IDF / FreeRTOS dependency — the debounce state machine runs on
 * injected level sequences.
 *
 * Compile with plain cc and run:
 *     cc -Wall -I main -o /tmp/bt main/app/test_button_core.c
 *     /tmp/bt
 *
 * Standard: prints BTN_SELFTEST_PASS (and exits 0) on success, or
 * BTN_SELFTEST_FAIL:<detail> (exit 1) on failure.
 *
 * This unit intentionally lists in NO SRCS of main/CMakeLists.txt — it is
 * host-only, exactly like test_macro_engine.c and test_web_logic.c.
 */

#include <stdio.h>
#include <string.h>

#include "app/button_core.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("BTN_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

/* Feed a whole level sequence, counting press-edge events. */
static unsigned feed(btn_debounce_t *b, const uint8_t *seq, unsigned n)
{
    unsigned fires = 0;
    for (unsigned i = 0; i < n; i++) {
        fires += btn_debounce_sample(b, seq[i]);
    }
    return fires;
}

int main(void)
{
    /* === Test 1: clean press+release fires exactly once, on the 3rd low === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 1);            /* idle: released (high) */
        static const uint8_t s[] = { 1,1, 0,0,0,0, 1,1,1,1 };
        /* lows: samples 3-6; the 3rd low (index 4) must be the single fire. */
        unsigned f0 = btn_debounce_sample(&b, s[0]);            /* 1 */
        unsigned f1 = btn_debounce_sample(&b, s[1]);            /* 1 */
        ASSERT(f0 == 0 && f1 == 0, "released level yields no event");
        unsigned f2 = btn_debounce_sample(&b, s[2]);            /* 0 -> probe */
        ASSERT(f2 == 0, "1st low is only probing, not fired");
        unsigned f3 = btn_debounce_sample(&b, s[3]);            /* 0 */
        ASSERT(f3 == 0, "2nd low still not fired");
        unsigned f4 = btn_debounce_sample(&b, s[4]);            /* 0 -> 3rd */
        ASSERT(f4 == 1, "3rd consecutive low fires exactly once");
        unsigned f5 = btn_debounce_sample(&b, s[5]);            /* 0 (held) */
        ASSERT(f5 == 0, "held low does not repeat-fire");
        unsigned f6 = btn_debounce_sample(&b, s[6]);            /* 0 (held) */
        ASSERT(f6 == 0, "still held, still no repeat");
        unsigned f7 = btn_debounce_sample(&b, s[7]);            /* 1 */
        ASSERT(f7 == 0, "release edge itself yields no event");
        unsigned f8 = btn_debounce_sample(&b, s[8]);            /* 1 */
        unsigned f9 = btn_debounce_sample(&b, s[9]);            /* 1 (3rd) */
        ASSERT(f8 == 0 && f9 == 0, "release re-arms but fires nothing");
    }

    /* === Test 2: long press fires once (no repeat) === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 1);
        unsigned fires = 0;
        for (int i = 0; i < 50; i++) {       /* 50 lows = 1 s held */
            fires += btn_debounce_sample(&b, 0);
        }
        ASSERT(fires == 1, "1-second hold fires exactly once");
    }

    /* === Test 3: release for 3 highs re-arms; next press fires again === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 1);
        feed(&b, (const uint8_t[]){ 0,0,0 }, 3);          /* press (fires) */
        unsigned after = 0;
        for (int i = 0; i < 5; i++) {
            after += btn_debounce_sample(&b, 1);          /* release */
        }
        ASSERT(after == 0, "release sequence fires nothing");
        unsigned f = btn_debounce_sample(&b, 0);          /* 1st low */
        unsigned g = btn_debounce_sample(&b, 0);
        unsigned h = btn_debounce_sample(&b, 0);          /* 3rd low */
        ASSERT(f == 0 && g == 0 && h == 1,
               "after a debounced release the next press fires again");
    }

    /* === Test 4: bounce (low-high-low-low-low) fires only after 3 stable lows
     * after the last bounce === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 1);
        static const uint8_t s[] = { 0,1,0,0,0,0 };
        unsigned f0 = btn_debounce_sample(&b, s[0]);       /* bounce low */
        ASSERT(f0 == 0, "bounce low does not fire");
        unsigned f1 = btn_debounce_sample(&b, s[1]);       /* back high */
        ASSERT(f1 == 0, "bounce high resets the count, no fire");
        unsigned f2 = btn_debounce_sample(&b, s[2]);       /* low */
        unsigned f3 = btn_debounce_sample(&b, s[3]);       /* low */
        unsigned f4 = btn_debounce_sample(&b, s[4]);       /* 3rd stable low */
        unsigned f5 = btn_debounce_sample(&b, s[5]);       /* 4th (held) */
        ASSERT(f2 == 0 && f3 == 0, "stable lows 1-2 do not fire");
        ASSERT(f4 == 1, "3rd stable low after the bounce fires");
        ASSERT(f5 == 0, "held after bounce does not repeat");
    }

    /* === Test 5: 2 lows + high == no event (never reached 3 stable) === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 1);
        static const uint8_t s[] = { 0,0,1,1,1,1 };
        unsigned fires = feed(&b, s, sizeof(s));
        ASSERT(fires == 0, "2 lows then release never reaches the 3-sample hold");
    }

    /* === Test 6: button HELD at power-on never fires; after release it does === */
    {
        btn_debounce_t b;
        btn_debounce_init(&b, 0);              /* init sees the button held low */
        unsigned fires = 0;
        for (int i = 0; i < 50; i++) {         /* 50 held-low samples */
            fires += btn_debounce_sample(&b, 0);
        }
        ASSERT(fires == 0, "power-on held press must not fire");
        for (int i = 0; i < 5; i++) {          /* release... */
            (void)btn_debounce_sample(&b, 1);
        }
        unsigned f = 0;
        for (int i = 0; i < 4; i++) {          /* ...then press again */
            f += btn_debounce_sample(&b, 0);
        }
        ASSERT(f == 1, "a real press after the release does fire");
    }

    printf("BTN_SELFTEST_PASS\n");
    return 0;
}