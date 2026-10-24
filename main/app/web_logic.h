#pragma once

/*
 * Pure HTTP-dispatch logic for the web layer (Task 3).
 *
 * Everything here is `static inline` and depends only on the host-compilable
 * IR headers (ir_encoder.h / ir_db.h) — no IDF / httpd dependency. Both
 * web_server.cc (firmware) and test_web_logic.c (host self-test) include it,
 * so the routing/parsing logic is testable with plain `cc`.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "ir/ir_encoder.h"   /* ir_key_t, IR_KEY_COUNT */
#include "ir/ir_db.h"        /* ir_profile_t */

/*
 * Map a wire key-name string (exactly what index.html sends) to an ir_key_t.
 *
 * Returns the enum value, or -1 for any unknown / NULL string (never crashes).
 * The names are pinned by the plan: "POWER"->IR_KEY_POWER, "UP"->IR_KEY_UP,
 * "VOL+"->IR_KEY_VOL_UP, etc.
 */
static inline int web_parse_key_name(const char *name)
{
    static const char *const WIRE_NAMES[IR_KEY_COUNT] = {
        "POWER", "VOL+", "VOL-", "CH+", "CH-", "MUTE", "MENU", "EXIT", "OK",
        "UP", "DOWN", "LEFT", "RIGHT",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    };
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < IR_KEY_COUNT; i++) {
        if (strcmp(name, WIRE_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Count the defined (non-0xFF) keys in a profile. Drives the per-profile
 * `keys_known` flag in GET /api/profiles (profile is "complete" when the
 * count == IR_KEY_COUNT). NULL profile -> 0.
 */
static inline int web_profile_known_keys(const ir_profile_t *p)
{
    if (p == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < IR_KEY_COUNT; i++) {
        if (p->cmds[i] != 0xFF) {
            n++;
        }
    }
    return n;
}

/*
 * Format the JSON payload of GET /api/status:
 *   {"paired_profile":N,"paired_name":"...","macros":[]}
 *
 * (The macros array stays empty until Task 4 fills it.)
 *
 * Returns the number of bytes written (excluding the NUL terminator), or -1
 * if the buffer is too small. `paired_name` is the built-in profile's display
 * name (safe chars only — no quotes), empty string when unpaired.
 */
static inline int web_format_status(uint8_t paired_profile, const char *paired_name,
                                    char *buf, size_t cap)
{
    if (paired_name == NULL) {
        paired_name = "";
    }
    int n = snprintf(buf, cap,
                     "{\"paired_profile\":%u,\"paired_name\":\"%s\",\"macros\":[]}",
                     (unsigned)paired_profile, paired_name);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}
