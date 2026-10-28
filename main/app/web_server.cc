#include "app/web_server.h"
#include "app/web_logic.h"
#include "app/remote_state.h"
#include "app/macro_engine.h"
#include "app/wizard_core.h"
#include "ir/ir_db.h"
#include "ir/ir_encoder.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

static const char *TAG = "web_server";

/* Embedded index.html (EMBED_FILES "web/index.html" in main/CMakeLists.txt). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ---- Minimal JSON field extraction (fixed-shape POST bodies only) ----
 * Accepts a `{"field":value}`-shaped object and pulls one field by name.
 * Strict and allocation-free: malformed input returns 0, which the caller
 * turns into a 400 (never crashes, never OOMs). Supports hex `0x04` style
 * numbers as sent by the raw-NEC debug endpoint. */

static int jis_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Point at the raw value (and its length) of the named field, or NULL. */
static const char *jfield(const char *body, size_t len, const char *name, size_t *vlen)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen + 2 < len; i++) {
        if (body[i] == '"' && memcmp(&body[i + 1], name, nlen) == 0 &&
            body[i + nlen + 1] == '"') {
            size_t j = i + nlen + 2;
            while (j < len && jis_space(body[j])) {
                j++;
            }
            if (j >= len || body[j] != ':') {
                return NULL;
            }
            j++;
            while (j < len && jis_space(body[j])) {
                j++;
            }
            *vlen = len - j;
            return &body[j];
        }
    }
    return NULL;
}

/* Parse a leading unsigned decimal or 0x-hex number from [s, s+n). */
static int jparse_u32(const char *s, size_t n, uint32_t *out)
{
    size_t i = 0;
    unsigned base = 10;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i >= n) {
        return 0;
    }
    uint32_t val = 0;
    int any = 0;
    for (; i < n; i++) {
        unsigned d;
        char c = s[i];
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            break;
        }
        if (val > (0xFFFFFFFFu - d) / base) {
            return 0; /* overflow */
        }
        val = val * base + d;
        any = 1;
    }
    if (!any) {
        return 0;
    }
    *out = val;
    return 1;
}

static int jget_uint(const char *body, size_t len, const char *name, uint32_t *out)
{
    size_t vlen = 0;
    const char *v = jfield(body, len, name, &vlen);
    if (v == NULL) {
        return 0;
    }
    return jparse_u32(v, vlen, out);
}

/* Copy an unquoted string value into `out`. No JSON escapes are expected
 * (key names are plain ASCII). */
static int jget_string(const char *body, size_t len, const char *name,
                       char *out, size_t cap)
{
    size_t vlen = 0;
    const char *v = jfield(body, len, name, &vlen);
    if (v == NULL || vlen == 0 || v[0] != '"') {
        return 0;
    }
    size_t i = 1;
    size_t o = 0;
    while (i < vlen) {
        if (v[i] == '"') {
            out[o] = '\0';
            return 1;
        }
        if (o + 1 >= cap) {
            return 0; /* need room for this char + NUL */
        }
        out[o++] = v[i++];
    }
    return 0; /* unterminated string */
}

/* Parse a JSON boolean literal (`true` / `false`) for `name`. Returns 1 and
 * sets *out (0/1) on success. Unlike jget_uint this deliberately does NOT
 * accept numeric values — the pair-answer contract is `{"reacted":true|false}`
 * and the frontend sends exactly those literals. */
static int jget_bool(const char *body, size_t len, const char *name, int *out)
{
    size_t vlen = 0;
    const char *v = jfield(body, len, name, &vlen);
    if (v == NULL) {
        return 0;
    }
    size_t i = 0;
    while (i < vlen && jis_space(v[i])) {
        i++;
    }
    if (vlen - i >= 4 && memcmp(v + i, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (vlen - i >= 5 && memcmp(v + i, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* ---- HTTP response helpers ---- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Bounded append for building small JSON responses. Stops writing at `cap`
 * and never overruns; returns the new offset (== cap when full). */
static size_t buf_append(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off >= cap) {
        return off;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    if ((size_t)n >= cap - off) {
        return cap; /* truncated; caller must stop appending */
    }
    return off + (size_t)n;
}

/* Read the full request body into `buf` (at most cap-1 bytes + NUL). The
 * entire body is always drained before returning, so the httpd can send the
 * response even when the body is oversized (we just refuse to parse it). */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t len = req->content_len;
    if (len == 0 || cap < 2) {
        return -1;
    }
    int too_big = (len >= cap);
    size_t to_store = too_big ? (cap - 1) : len;
    size_t got = 0;
    while (got < len) {
        size_t want = len - got;
        char *dst;
        if (got < to_store) {
            dst = buf + got;
            if (want > to_store - got) {
                want = to_store - got;
            }
        } else {
            dst = buf; /* discard excess, bounds-safe */
            if (want > cap - 1) {
                want = cap - 1;
            }
        }
        int r = httpd_req_recv(req, dst, want);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    buf[to_store] = '\0';
    if (too_big) {
        return -1; /* body fully drained but too large to parse */
    }
    return (int)to_store;
}

/* ---- Handlers ---- */

static esp_err_t handle_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (ssize_t)(index_html_end - index_html_start));
}

static esp_err_t handle_api_status(httpd_req_t *req)
{
    uint8_t pid = remote_state_get_profile();
    const ir_profile_t *p = ir_db_get_profile(pid);
    if (p == NULL) {
        pid = 0xFF; /* stale/invalid stored id -> treat as unpaired */
    }
    char buf[2048];
    const char *state = MACRO_STATE_STR_IDLE;
    switch (macro_get_state()) {
    case MACRO_STATE_RECORDING: state = MACRO_STATE_STR_RECORDING; break;
    case MACRO_STATE_PLAYING:   state = MACRO_STATE_STR_PLAYING;   break;
    default: break;
    }
    macro_info_t macros[MACRO_INDEX_MAX];
    uint8_t mac_count = 0;
    macro_macros_snapshot(macros, MACRO_INDEX_MAX, &mac_count);
    int n = web_format_status(pid, (p != NULL) ? p->name : "",
                              macros, mac_count, state,
                              (unsigned)macro_get_playback_step(),
                              macro_get_onekey(), macro_get_selfstart(),
                              buf, sizeof(buf));
    if (n < 0) {
        n = (int)strlen(buf);   /* truncated; send what we have below */
    }
    if (n <= 0 || buf[n - 1] != '}') {
        return send_json(req, "200 OK", buf);   /* nothing to splice onto */
    }
    const int root_at = n;
    buf[n - 1] = '\0';   /* drop the root object's closing '}' */

    /* Wizard block: derive state from the per-TU wizard ctx. */
    const wizard_ctx_t *ctx = &wizard_core_ctx;
    const char *wstate;
    switch (ctx->phase) {
    case WIZARD_PHASE_TRY_POWER:  wstate = "try_power"; break;
    case WIZARD_PHASE_CALIBRATE:  wstate = "calibrate"; break;
    case WIZARD_PHASE_DONE:
        wstate = (ctx->source_group == WIZARD_NOMATCH) ? "no_match" : "done";
        break;
    default:                      wstate = "idle";      break;
    }
    uint8_t wgroup = (ctx->phase == WIZARD_PHASE_IDLE) ? 0 : ctx->group_idx;
    const char *wkey = web_key_name(wizard_current_key(ctx));

    /* Calibration block: 0/1 per key, present only when a valid blob exists. */
    uint8_t calib[IR_KEY_COUNT];
    uint8_t keys[IR_KEY_COUNT];
    const uint8_t *keyp = NULL;
    uint8_t calib_group = 0;
    if (pair_calib_read_nvs(calib)) {
        for (int i = 0; i < IR_KEY_COUNT; i++) {
            keys[i] = (calib[i] != 0xFF) ? 1u : 0u;
        }
        keyp = keys;
        calib_group = pair_calib_group();
    }

    /* Group count + current group name from the wizard ctx. */
    const char *group_name = "";
    uint8_t group_count = ctx->group_count;
    if (ctx->groups != NULL && ctx->group_idx < ctx->group_count) {
        group_name = ctx->groups[ctx->group_idx]->name;
    }

    int wn = web_format_wizard(wstate, wgroup, wkey, group_count, group_name,
                               keyp, calib_group,
                               buf, sizeof(buf));
    if (wn < 0 || (size_t)wn + 2 > sizeof(buf)) {
        buf[root_at - 1] = '}';   /* restore the base status verbatim */
        buf[root_at] = '\0';
        return send_json(req, "200 OK", buf);
    }
    buf[wn] = '}';
    buf[wn + 1] = '\0';
    return send_json(req, "200 OK", buf);
}

static esp_err_t handle_api_profiles(httpd_req_t *req)
{
    /* 按 30+ 组预算:~70B/组,NECx 码表会继续增长 */
    char buf[2048];
    size_t off = buf_append(buf, sizeof(buf), 0, "[");
    uint8_t count = ir_db_get_profile_count();
    for (uint8_t i = 0; i < count && off < sizeof(buf) - 4u; i++) {
        const ir_profile_t *p = ir_db_get_profile(i);
        const char *name = (p != NULL) ? p->name : "";
        int known = web_profile_known_keys(p);
        off = buf_append(buf, sizeof(buf), off,
                         "%s{\"id\":%u,\"name\":\"%s\",\"keys_known\":%s}",
                         (i ? "," : ""), (unsigned)i, name,
                         (known == IR_KEY_COUNT) ? "true" : "false");
    }
    buf_append(buf, sizeof(buf), off, "]");
    return send_json(req, "200 OK", buf);
}

/* 旧资产兼容路径(deprecated):legacy POST /api/pair {profile_id} 仅保留注册,避免老页面 404。
 * 语义仍是直接写回 legacy paired_profile;但校准档存在(pair_calib_active() 为真)时,
 * ir_encoder/ir_send_key 已优先使用校准档作为 NEC 地址来源,故旧配对无法干扰新向导。
 * 新页面请走 /api/pair/start|answer|cancel|resend。 */
static esp_err_t handle_api_pair(httpd_req_t *req)
{
    char body[128];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    uint32_t pid = 0;
    if (!jget_uint(body, (size_t)len, "profile_id", &pid)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid body\"}");
    }
    if (!web_profile_id_valid(pid, ir_db_get_profile_count())) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid profile\"}");
    }
    remote_state_set_profile((uint8_t)pid);
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"paired_profile\":%u}", (unsigned)pid);
    return send_json(req, "200 OK", out);
}

static int64_t s_rec_step_last_us;   /* last recorded step's us timestamp */

static esp_err_t handle_api_key(httpd_req_t *req)
{
    char body[128];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    char key_name[16];
    if (!jget_string(body, (size_t)len, "key", key_name, sizeof(key_name))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"missing key\"}");
    }
    int key = web_parse_key_name(key_name);
    if (key < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"unknown key\"}");
    }

    /* Recording hook: any single key press while RECORDING appends a macro
     * step (wait_ms = time since the previous step, or since start for the
     * first). The key is sent below regardless, so the user sees live
     * feedback. The step is recorded even when the key is undefined in the
     * paired profile — a macro may target another profile's layout later and
     * the core treats keys as opaque. */
    if (macro_is_recording()) {
        int64_t now = esp_timer_get_time();
        uint32_t wait_ms = (uint32_t)((now - s_rec_step_last_us) / 1000ll);
        s_rec_step_last_us = now;
        (void)macro_record_step((ir_key_t)key, wait_ms);
    }

    /* Abort hook (急停): ANY recognized key press interrupts active macro
     * playback. macro_interrupt() is a safe no-op when not playing and does
     * NOT disturb a recording (unlike macro_stop); it fires before the key is
     * sent so the pressed key is emitted exactly once, below — the playback
     * task never sends the interrupting key. */
    (void)macro_interrupt();

    /* Which profile to send through:
     *  - optional "profile_id" in the body -> that profile (pairing power test);
     *  - otherwise -> the currently paired source (normal remote use), resolved
     *    through the unified calib-aware path. */
    const ir_profile_t *p = NULL;
    uint32_t explicit_pid = 0;
    bool sent = false;
    if (jget_uint(body, (size_t)len, "profile_id", &explicit_pid)) {
        /* Range-check the uint32_t BEFORE the uint8_t cast: a value >= count
         * (e.g. 256 with 4 profiles) must 400, not wrap to profile 0. */
        if (!web_profile_id_valid(explicit_pid, ir_db_get_profile_count())) {
            return send_json(req, "400 Bad Request", "{\"error\":\"invalid profile\"}");
        }
        p = ir_db_get_profile((uint8_t)explicit_pid);
        if (p == NULL) {
            return send_json(req, "400 Bad Request", "{\"error\":\"invalid profile\"}");
        }
        /* W13: raw-frame keysets transmit the stored waveform (their frames
         * are not 32-bit NEC).  Check the raw table FIRST — cmds[i] is only
         * a definedness marker (0xFE) and must never go out as an NEC byte. */
        const ir_raw_frame_t *rf = ir_profile_raw_frame(p, (ir_key_t)key);
        if (rf != NULL) {
            ir_send_raw_us(rf->frame, rf->n_pairs);
            sent = true;
        } else {
            uint16_t addr = 0;
            uint8_t cmd = 0;
            sent = ir_db_find_key(p, (ir_key_t)key, &addr, &cmd);
            if (sent) {
                /* The explicit-profile_id branch tests the candidate group's own
                 * frame: short-header (Samsung-style 4.5ms) groups must send with
                 * their profile leader, not the standard 9 ms one. */
                uint16_t lm, ls;
                ir_profile_leader(p, &lm, &ls);
                ir_send_raw_nec_var(addr, cmd, lm, ls);
            }
        }
    } else {
        /* Normal remote use: resolve through the unified calib-aware path
         * (calibration blob active -> its keymap; else legacy paired profile).
         * Never sends 0xFF/undefined keys. */
        if (!ir_send_key((ir_key_t)key)) {
            /* Preserve the pre-wizard error contract: "not paired" when nothing
             * is resolvable at all; "key undefined in profile" when a paired
             * source exists but this key is not in it. */
            if (!pair_calib_active() &&
                remote_state_get_profile() >= ir_db_get_profile_count()) {
                return send_json(req, "400 Bad Request", "{\"error\":\"not paired\"}");
            }
            return send_json(req, "400 Bad Request",
                             "{\"error\":\"key undefined in profile\"}");
        }
        sent = true;
    }

    if (!sent) {
        /* only reachable from the explicit-profile_id branch (the else-branch
         * already returned its own error above) */
        return send_json(req, "400 Bad Request", "{\"error\":\"key undefined in profile\"}");
    }

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"key\":\"%s\"}", key_name);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_key_raw(httpd_req_t *req)
{
    char body[128];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    uint32_t addr = 0;
    uint32_t cmd = 0;
    if (!jget_uint(body, (size_t)len, "address", &addr) ||
        !jget_uint(body, (size_t)len, "command", &cmd)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid body\"}");
    }
    if (addr > 0xFFFFu || cmd > 0xFFu) {
        return send_json(req, "400 Bad Request", "{\"error\":\"out of range\"}");
    }
    ir_send_raw_nec((uint16_t)addr, (uint8_t)cmd);

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"address\":%u,\"command\":%u}",
             (unsigned)addr, (unsigned)cmd);
    return send_json(req, "200 OK", out);
}

/* ---- Macro REST routes (Task 4b) ---- */

/* Body parser helper: read a bounded JSON body into `buf`; returns length (>=0)
 * or -1 on empty/oversized bodies. */
static int macro_read_json_body(httpd_req_t *req, char *buf, size_t cap)
{
    int len = read_body(req, buf, cap);
    if (len <= 0) {
        return -1;
    }
    return len;
}

static esp_err_t handle_api_macro_start(httpd_req_t *req)
{
    char body[64];
    if (macro_read_json_body(req, body, sizeof(body)) < 0) {
        /* tolerant: accept empty or arbitrary body */
    }
    esp_err_t e = macro_start_recording();
    if (e != ESP_OK) {
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"cannot start recording\"}");
    }
    s_rec_step_last_us = esp_timer_get_time();   /* first-step wait baseline */
    return send_json(req, "200 OK", "{\"ok\":true,\"recording\":true}");
}

/* Record one step + send it live (mirror of the handle_api_key recording
 * hook). Requires recording to be active. */
static esp_err_t handle_api_macro_step(httpd_req_t *req)
{
    char body[128];
    int len = macro_read_json_body(req, body, sizeof(body));
    if (len < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    char key_name[16];
    if (!jget_string(body, (size_t)len, "key", key_name, sizeof(key_name))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"missing key\"}");
    }
    int key = web_parse_key_name(key_name);
    if (key < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"unknown key\"}");
    }
    if (!macro_is_recording()) {
        return send_json(req, "400 Bad Request", "{\"error\":\"not recording\"}");
    }
    int64_t now = esp_timer_get_time();
    uint32_t wait_ms = (uint32_t)((now - s_rec_step_last_us) / 1000ll);
    s_rec_step_last_us = now;
    esp_err_t e = macro_record_step((ir_key_t)key, wait_ms);
    if (e != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"record step failed\"}");
    }

    /* Live feedback: send the key through the currently paired source exactly
     * once (calib-aware unified resolution). Silently skips when the key is
     * undefined or nothing is paired — never sends 0xFF. */
    (void)ir_send_key((ir_key_t)key);

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"key\":\"%s\",\"wait_ms\":%lu}",
             key_name, (unsigned long)wait_ms);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_macro_save(httpd_req_t *req)
{
    char body[128];
    int len = macro_read_json_body(req, body, sizeof(body));
    if (len < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    char name[MACRO_NAME_CAP];   /* 24 bytes incl NUL; engine requires <= 23 */
    if (!jget_string(body, (size_t)len, "name", name, sizeof(name))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"missing name\"}");
    }
    if (name[0] == '\0' || strlen(name) >= MACRO_NAME_CAP) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid name\"}");
    }
    esp_err_t e = macro_save(name);
    if (e == ESP_ERR_INVALID_STATE) {
        return send_json(req, "400 Bad Request", "{\"error\":\"not recording\"}");
    }
    if (e != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"save failed\"}");
    }
    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"name\":\"%s\"}", name);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_macro_play(httpd_req_t *req)
{
    char body[128];
    int len = macro_read_json_body(req, body, sizeof(body));
    if (len < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    uint32_t id = 0;
    if (!jget_uint(body, (size_t)len, "id", &id)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"missing id\"}");
    }
    if (id > 0xFFu) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid id\"}");
    }
    /* Optional one_key toggle: {"id":N,"one_key":true/false} binds (or clears
     * a binding of) macro N as the physical-button macro WITHOUT consuming the
     * request — the call then still runs the plain play path below for
     * immediate feedback. jfield distinguishes "field absent" (plain play)
     * from "field present but not a bool" (400). */
    size_t vlen = 0;
    if (jfield(body, (size_t)len, "one_key", &vlen) != NULL) {
        int one_key = 0;
        if (!jget_bool(body, (size_t)len, "one_key", &one_key)) {
            return send_json(req, "400 Bad Request", "{\"error\":\"invalid body\"}");
        }
        esp_err_t be = one_key ? macro_set_onekey((uint8_t)id)
                               : macro_clear_onekey((uint8_t)id);
        if (be == ESP_ERR_NOT_FOUND) {
            return send_json(req, "404 Not Found", "{\"error\":\"macro not found\"}");
        }
        if (be != ESP_OK) {
            return send_json(req, "400 Bad Request", "{\"error\":\"play failed\"}");
        }
    }
    /* Optional selfstart toggle: {"id":N,"selfstart":true/false} enables (or
     * disables) power-on auto-replay of the one-key macro. Independent of the
     * plain play path below — same no-new-route contract as `one_key`. */
    if (jfield(body, (size_t)len, "selfstart", &vlen) != NULL) {
        int ss = 0;
        if (!jget_bool(body, (size_t)len, "selfstart", &ss)) {
            return send_json(req, "400 Bad Request", "{\"error\":\"invalid body\"}");
        }
        esp_err_t se = macro_set_selfstart(ss ? 1u : 0u);
        if (se != ESP_OK) {
            return send_json(req, "400 Bad Request", "{\"error\":\"play failed\"}");
        }
    }
    esp_err_t e = macro_play((uint8_t)id);
    if (e == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"error\":\"macro not found\"}");
    }
    if (e == ESP_ERR_INVALID_STATE) {
        /* macro_play returns INVALID_STATE for "unpaired" and "engine busy"
         * alike; key off the live engine state for an accurate message. */
        switch (macro_get_state()) {
        case MACRO_STATE_PLAYING:
            return send_json(req, "400 Bad Request", "{\"error\":\"already playing\"}");
        case MACRO_STATE_RECORDING:
            return send_json(req, "400 Bad Request", "{\"error\":\"recording in progress\"}");
        default:
            return send_json(req, "400 Bad Request", "{\"error\":\"not paired\"}");
        }
    }
    if (e != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"play failed\"}");
    }
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%lu}", (unsigned long)id);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_macro_stop(httpd_req_t *req)
{
    char body[64];
    if (macro_read_json_body(req, body, sizeof(body)) < 0) {
        /* tolerant: accept empty or arbitrary body */
    }
    (void)macro_stop();   /* safe when idle/playing/recording */
    return send_json(req, "200 OK", "{\"ok\":true,\"state\":\"idle\"}");
}

static esp_err_t handle_api_macro_delete(httpd_req_t *req)
{
    char body[128];
    int len = macro_read_json_body(req, body, sizeof(body));
    if (len < 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    uint32_t id = 0;
    if (!jget_uint(body, (size_t)len, "id", &id)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"missing id\"}");
    }
    if (id > 0xFFu) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid id\"}");
    }
    esp_err_t e = macro_delete((uint8_t)id);
    if (e == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"error\":\"macro not found\"}");
    }
    if (e != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"delete failed\"}");
    }
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%lu}", (unsigned long)id);
    return send_json(req, "200 OK", out);
}

/* ---- Guided-pairing REST routes (Task 3) ---- */

/*
 * Send the wizard's current key through the CANDIDATE group, not the paired
 * profile. During a pairing pass no calibration exists and the device is
 * usually not paired, so ir_send_key() would resolve against the legacy / NVS
 * profile (and skip entirely when unpaired); the wizard must test the
 * candidate group's own native code instead. Only sends when the machine is in
 * an active phase (TRY_POWER / CALIBRATE) and the current key is defined in the
 * group. Returns 1 when a frame was actually transmitted.
 */
static int wizard_send_current(void)
{
    const wizard_ctx_t *ctx = &wizard_core_ctx;
    if (ctx->phase != WIZARD_PHASE_TRY_POWER &&
        ctx->phase != WIZARD_PHASE_CALIBRATE) {
        return 0;
    }
    ir_key_t key = wizard_current_key(ctx);
    if (key == IR_KEY_COUNT || ctx->group_idx >= ctx->group_count) {
        return 0;
    }
    const ir_profile_t *g = ctx->groups[ctx->group_idx];
    if (g == NULL) {
        return 0;
    }
    /* W13: raw-frame candidate keysets are transmitted from their stored
     * waveform; the NEC path below only serves real NEC/NECx groups. */
    const ir_raw_frame_t *rf = ir_profile_raw_frame(g, key);
    if (rf != NULL) {
        ir_send_raw_us(rf->frame, rf->n_pairs);
        return 1;
    }
    uint16_t addr = 0;
    uint8_t cmd = 0;
    if (!ir_db_find_key(g, key, &addr, &cmd)) {
        return 0;   /* undefined in the candidate group: never fabricate */
    }
    uint16_t lm, ls;
    ir_profile_leader(g, &lm, &ls);
    ir_send_raw_nec_var(addr, cmd, lm, ls);
    return 1;
}

static esp_err_t handle_api_pair_start(httpd_req_t *req)
{
    char body[64];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    const uint8_t *wizard_ids = NULL;
    const ir_profile_t *const *wizard_groups = ir_db_wizard_order(&wizard_ids);
    wizard_ctx_t *ctx = wizard_start(wizard_groups, ir_db_get_profile_count());
    /* W13: the wizard tests the raw-frame keysets first; source_group is an
     * index into that reordered list, so hand the wizard its group-index ->
     * real-PROFILES-index mapping before any answer arrives. */
    wizard_set_profile_ids(ctx, wizard_ids);
    if (ctx->phase == WIZARD_PHASE_DONE &&
        ctx->source_group == WIZARD_NOMATCH) {
        /* Every candidate group is power-less: terminal, no NVS write. */
        return send_json(req, "200 OK",
                         "{\"ok\":true,\"wizard\":{\"state\":\"done\",\"no_match\":true}}");
    }
    /* Auto-send the current group's POWER. */
    (void)wizard_send_current();
    char out[192];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"wizard\":{\"state\":\"try_power\",\"group_id\":%u,\"key\":\"%s\",\"group_count\":%u,\"group_name\":\"%s\"},\"sent\":true}",
             (unsigned)ctx->group_idx, web_key_name(wizard_current_key(ctx)),
             (unsigned)ctx->group_count, ctx->groups[ctx->group_idx]->name);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_pair_answer(httpd_req_t *req)
{
    char body[128];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    int reacted = 0;
    if (!jget_bool(body, (size_t)len, "reacted", &reacted)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid body\"}");
    }

    wizard_ctx_t *ctx = &wizard_core_ctx;
    if (ctx->phase == WIZARD_PHASE_IDLE) {
        return send_json(req, "400 Bad Request", "{\"error\":\"invalid state\"}");
    }
    if (ctx->phase == WIZARD_PHASE_DONE) {
        /* Already terminal: safe no-op echoing the terminal state. */
        int nm = (ctx->source_group == WIZARD_NOMATCH);
        char out[96];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"wizard\":{\"state\":\"done\",\"no_match\":%s}}",
                 nm ? "true" : "false");
        return send_json(req, "200 OK", out);
    }

    if (ctx->phase == WIZARD_PHASE_TRY_POWER) {
        ctx = wizard_answer_power(ctx, reacted);
    } else {   /* CALIBRATE (IDLE/DONE already handled above) */
        ctx = wizard_answer_key(ctx, reacted);
    }

    if (ctx->phase == WIZARD_PHASE_DONE) {
        if (ctx->source_group != WIZARD_NOMATCH) {
            /* Completion with a locked group: write the calibration (the ONLY
             * wizard write point — never called on cancel / reboot / no-match),
             * then drop back to IDLE so status reports idle + calib present.
             * source_group is a WIZARD-ORDER index; map it to the REAL PROFILES
             * index (NVS profile-id semantics) before storing. */
            uint8_t real_id = wizard_core_real_group_id(ctx);
            pair_calib_store_nvs(ctx->keymap, real_id);
            wizard_cancel();
            return send_json(req, "200 OK",
                             "{\"ok\":true,\"wizard\":{\"state\":\"done\"},\"done\":true}");
        }
        return send_json(req, "200 OK",
                         "{\"ok\":true,\"wizard\":{\"state\":\"done\",\"no_match\":true}}");
    }

    /* Advanced and still active: auto-send the new current key. */
    (void)wizard_send_current();
    const char *wstate = (ctx->phase == WIZARD_PHASE_TRY_POWER) ? "try_power" : "calibrate";
    char out[192];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"wizard\":{\"state\":\"%s\",\"group_id\":%u,\"key\":\"%s\",\"group_count\":%u,\"group_name\":\"%s\"},\"sent\":true}",
             wstate, (unsigned)ctx->group_idx, web_key_name(wizard_current_key(ctx)),
             (unsigned)ctx->group_count, ctx->groups[ctx->group_idx]->name);
    return send_json(req, "200 OK", out);
}

static esp_err_t handle_api_pair_cancel(httpd_req_t *req)
{
    char body[64];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    wizard_cancel();   /* never touches NVS; old pairing stays intact */
    return send_json(req, "200 OK", "{\"ok\":true,\"state\":\"idle\"}");
}

static esp_err_t handle_api_pair_resend(httpd_req_t *req)
{
    char body[64];
    int len = read_body(req, body, sizeof(body));
    if (len <= 0) {
        return send_json(req, "400 Bad Request", "{\"error\":\"empty body\"}");
    }
    /* Idempotent convenience: re-send the current wizard key (page-refresh
     * recovery). With no active wizard (IDLE/DONE) the send is skipped but the
     * endpoint still answers 200. `sent` reflects what actually dispatched. */
    int sent = wizard_send_current();
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"sent\":%s}", sent ? "true" : "false");
    return send_json(req, "200 OK", out);
}

/* ---- Server start ---- */

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16; /* 16 routes registered below — keep in sync */
    cfg.stack_size = 8192; /* headroom for serving the embedded page + JSON */
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

/* Parameter names must not collide with the struct members (.uri/.method/
 * .handler) or the preprocessor would rewrite the designators. */
#define REGISTER(uri_s, method_s, handler_fn)                                \
    do {                                                                     \
        httpd_uri_t reg = { .uri = uri_s, .method = method_s,                \
                            .handler = handler_fn, .user_ctx = NULL };       \
        esp_err_t err = httpd_register_uri_handler(server, &reg);            \
        if (err != ESP_OK) {                                                 \
            ESP_LOGE(TAG, "register handler %s failed: %s", uri_s,           \
                     esp_err_to_name(err));                                  \
        }                                                                    \
    } while (0)

    REGISTER("/", HTTP_GET, handle_get_root);
    REGISTER("/api/status", HTTP_GET, handle_api_status);
    REGISTER("/api/profiles", HTTP_GET, handle_api_profiles);
    REGISTER("/api/pair", HTTP_POST, handle_api_pair);
    REGISTER("/api/pair/start", HTTP_POST, handle_api_pair_start);
    REGISTER("/api/pair/answer", HTTP_POST, handle_api_pair_answer);
    REGISTER("/api/pair/cancel", HTTP_POST, handle_api_pair_cancel);
    REGISTER("/api/pair/resend", HTTP_POST, handle_api_pair_resend);
    REGISTER("/api/key", HTTP_POST, handle_api_key);
    REGISTER("/api/key/raw", HTTP_POST, handle_api_key_raw);
    REGISTER("/api/macro/start", HTTP_POST, handle_api_macro_start);
    REGISTER("/api/macro/step", HTTP_POST, handle_api_macro_step);
    REGISTER("/api/macro/save", HTTP_POST, handle_api_macro_save);
    REGISTER("/api/macro/play", HTTP_POST, handle_api_macro_play);
    REGISTER("/api/macro/stop", HTTP_POST, handle_api_macro_stop);
    REGISTER("/api/macro/delete", HTTP_POST, handle_api_macro_delete);
#undef REGISTER

    ESP_LOGI(TAG, "web server started on 192.168.4.1");
}
