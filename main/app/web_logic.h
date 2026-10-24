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
#include "app/macro_types.h" /* macro_info_t */

/*
 * Wire key-name table (exactly what index.html sends / GET status emits).
 * The names are pinned by the plan: "POWER"->IR_KEY_POWER, "UP"->IR_KEY_UP,
 * "VOL+"->IR_KEY_VOL_UP, etc. Index i holds the wire name for ir_key_t i.
 */
static inline const char *web_key_name(ir_key_t key)
{
    static const char *const WIRE_NAMES[IR_KEY_COUNT] = {
        "POWER", "VOL+", "VOL-", "CH+", "CH-", "MUTE", "MENU", "EXIT", "OK",
        "UP", "DOWN", "LEFT", "RIGHT",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    };
    return ((unsigned)key < (unsigned)IR_KEY_COUNT) ? WIRE_NAMES[key] : "";
}

/*
 * Map a wire key-name string (exactly what index.html sends) to an ir_key_t.
 *
 * Returns the enum value, or -1 for any unknown / NULL string (never crashes).
 */
static inline int web_parse_key_name(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < IR_KEY_COUNT; i++) {
        if (strcmp(name, web_key_name((ir_key_t)i)) == 0) {
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
 * Validate a profile id from a client request BEFORE truncating it to the
 * uint8_t stored in NVS / indexing ir_db. `count` is ir_db_get_profile_count()
 * at the call site. A uint32_t id >= count (e.g. 256 when there are 4 profiles)
 * must be rejected: casting it to uint8_t first would silently wrap it to
 * 0..count-1 and send the WRONG profile's key. Returns non-zero when valid.
 */
static inline int web_profile_id_valid(uint32_t id, uint32_t count)
{
    return (id < count);
}

/*
 * Format the JSON payload of GET /api/status:
 *   {"paired_profile":N,"paired_name":"...","macros":[
 *       {"id":..,"name":"..","step_count":..,"first_key":"POWER"},
 *       ...
 *    ],"playback":{"state":"idle|recording|playing","current_step":N}}
 *
 * `macros` is the engine's real list (`macros_array`/`macros_count`, may be
 * NULL/0). `first_key` uses the same wire names as web_parse_key_name
 * ("POWER"), "" when the macro has no first key.
 *
 * Returns the number of bytes written (excluding the NUL terminator), or -1
 * if the buffer is too small. `paired_name` is the built-in profile's display
 * name (safe chars only — no quotes), empty string when unpaired.
 */
static inline int web_format_status(uint8_t paired_profile, const char *paired_name,
                                    const macro_info_t *macros_array, uint8_t macros_count,
                                    const char *playback_state, unsigned current_step,
                                    char *buf, size_t cap)
{
    if (paired_name == NULL) {
        paired_name = "";
    }
    if (playback_state == NULL) {
        playback_state = "idle";
    }

    size_t off = 0;
    int n;
#define APPEND(...) do { \
        if (off >= cap) return -1; \
        n = snprintf(buf + off, cap - off, __VA_ARGS__); \
        if (n < 0) return -1; \
        size_t nn = (size_t)n; \
        if (nn >= cap - off) return -1; \
        off += nn; \
    } while (0)

    APPEND("{\"paired_profile\":%u,\"paired_name\":\"%s\",\"macros\":[",
           (unsigned)paired_profile, paired_name);
    unsigned shown = (macros_array != NULL) ? (unsigned)macros_count : 0u;
    for (unsigned i = 0; i < shown; i++) {
        const macro_info_t *m = &macros_array[i];
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"step_count\":%u,\"first_key\":\"%s\"}",
               (i ? "," : ""), (unsigned)m->id, m->name,
               (unsigned)m->step_count, web_key_name(m->first_key));
    }
    APPEND("],\"playback\":{\"state\":\"%s\",\"current_step\":%u}}",
           playback_state, current_step);
#undef APPEND
    return (off < cap) ? (int)off : -1;
}
