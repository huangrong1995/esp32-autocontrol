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
 * JSON-escape `src` into `dst` (capacity `cap`). Escapes `"` -> `\"` and
 * `\` -> `\\`; control chars (< 0x20) use the JSON short forms where they
 * exist (`\b \f \n \r \t`) and `\u00XX` otherwise. Bounded: never writes past
 * cap-1 (a NUL is always left), and never splits an escape sequence — when
 * fewer than `need` bytes remain for the current character, it stops BEFORE
 * the backslash so truncation cannot produce a dangling `\`. NULL `src` is
 * treated as "". NULL/zero-cap `dst` is a no-op. Host-compilable.
 */
static inline void web_escape_json(const char *src, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    static const char HEX[] = "0123456789ABCDEF";
    char *o = dst;
    char *const limit = dst + cap - 1;   /* one byte reserved for the NUL */
    for (const char *p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        const char *esc = NULL;          /* short escape: `\` + 1 char */
        char hex[7];                     /* `\u00XX` (6 chars + NUL) */
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                hex[0] = '\\'; hex[1] = 'u'; hex[2] = '0'; hex[3] = '0';
                hex[4] = HEX[(c >> 4) & 0xF];
                hex[5] = HEX[c & 0xF];
                hex[6] = '\0';   /* NUL-terminate so strlen(hex) == 6 is safe */
                esc = hex;
            }
            break;
        }
        size_t need = (esc != NULL) ? strlen(esc) : 1u;
        if ((size_t)(limit - o) < need) {
            break;   /* no room for the whole sequence: stop before `\` */
        }
        if (esc != NULL) {
            memcpy(o, esc, need);
            o += need;
        } else {
            *o++ = (char)c;
        }
    }
    *o = '\0';
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
    char esc_name[MACRO_NAME_CAP * 6 + 1];   /* worst case: every char \u00XX */
    for (unsigned i = 0; i < shown; i++) {
        const macro_info_t *m = &macros_array[i];
        web_escape_json(m->name, esc_name, sizeof(esc_name));
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"step_count\":%u,\"first_key\":\"%s\"}",
               (i ? "," : ""), (unsigned)m->id, esc_name,
               (unsigned)m->step_count, web_key_name(m->first_key));
    }
    APPEND("],\"playback\":{\"state\":\"%s\",\"current_step\":%u}}",
           playback_state, current_step);
#undef APPEND
    return (off < cap) ? (int)off : -1;
}

/*
 * Append the wizard + calibration block to an existing NUL-terminated JSON
 * prefix already stored in `buf` (the caller keeps the leading part of a
 * document; this function closes nothing). Appends:
 *   ,"wizard":{"state":"...","group_id":N,"key":"...","group_count":M,"group_name":"..."}
 * and, when `keys` != NULL, additionally appends:
 *   ,"calib_group":N,"keys":[0,1,...]
 * where each entry is rendered 1 when `keys[i] != 0` and 0 otherwise (the
 * caller passes raw keymap bytes; this helper normalizes to 0/1). `state` and
 * `key` are fixed wire values ("idle"/"try_power"/"calibrate"/"done"/"no_match"
 * and web_key_name() output — plain ASCII, no JSON escaping needed).
 * `group_name` is likewise a plain-ASCII profile name (may be ""); non-ASCII
 * names would require JSON escaping — future work, not this round.
 *
 * Returns the new offset (index of the NUL terminator), or -1 when the buffer
 * is too small. Capacity is checked UP FRONT — on overflow nothing is written,
 * so the caller's existing prefix (and its terminating NUL) is never disturbed
 * (mirrors web_format_status's APPEND "do not corrupt" contract). `keys == NULL`
 * omits the `calib_group` field entirely.
 */
static inline int web_format_wizard(const char *state, uint8_t group, const char *key,
                                    uint8_t group_count, const char *group_name,
                                    const uint8_t *keys,
                                    uint8_t calib_group,
                                    char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return -1;
    }
    if (state == NULL) {
        state = "";
    }
    if (key == NULL) {
        key = "";
    }
    if (group_name == NULL) {
        group_name = "";
    }
    size_t off = strlen(buf);
    if (off >= cap) {
        return -1;
    }
    size_t room = cap - off;   /* includes room for the final NUL */

    /* Up-front sizing: base wizard block + optional calib block. */
    int n = snprintf(NULL, 0,
                     ",\"wizard\":{\"state\":\"%s\",\"group_id\":%u,\"key\":\"%s\",\"group_count\":%u,\"group_name\":\"%s\"}",
                     state, (unsigned)group, key, (unsigned)group_count, group_name);
    if (n < 0) {
        return -1;
    }
    size_t need = (size_t)n;
    if (keys != NULL) {
        int m = snprintf(NULL, 0, ",\"calib_group\":%u,\"keys\":[", (unsigned)calib_group);
        if (m < 0) {
            return -1;
        }
        need += (size_t)m;
        for (int i = 0; i < IR_KEY_COUNT; i++) {
            need += (i ? 2u : 1u);   /* ",0" or "0" */
        }
        need += 1u;                  /* closing ']' */
    }
    if (need + 1u > room) {          /* +1 for the NUL terminator */
        return -1;                   /* nothing written: prefix untouched */
    }

#define APPEND(...) do { \
        if (off >= cap) return -1; \
        n = snprintf(buf + off, cap - off, __VA_ARGS__); \
        if (n < 0) return -1; \
        size_t nn = (size_t)n; \
        if (nn >= cap - off) return -1; \
        off += nn; \
    } while (0)

    APPEND(",\"wizard\":{\"state\":\"%s\",\"group_id\":%u,\"key\":\"%s\",\"group_count\":%u,\"group_name\":\"%s\"}",
           state, (unsigned)group, key, (unsigned)group_count, group_name);
    if (keys != NULL) {
        APPEND(",\"calib_group\":%u,\"keys\":[", (unsigned)calib_group);
        for (int i = 0; i < IR_KEY_COUNT; i++) {
            APPEND("%s%u", (i ? "," : ""),
                   (unsigned)((keys[i] != 0u) ? 1u : 0u));
        }
        APPEND("]");
    }
#undef APPEND
    return (off < cap) ? (int)off : -1;
}
