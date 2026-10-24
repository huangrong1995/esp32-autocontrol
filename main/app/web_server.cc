#include "app/web_server.h"
#include "app/web_logic.h"
#include "app/remote_state.h"
#include "app/macro_engine.h"
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
    char buf[1600];
    const char *state = MACRO_STATE_STR_IDLE;
    switch (macro_get_state()) {
    case MACRO_STATE_RECORDING: state = MACRO_STATE_STR_RECORDING; break;
    case MACRO_STATE_PLAYING:   state = MACRO_STATE_STR_PLAYING;   break;
    default: break;
    }
    macro_info_t macros[MACRO_INDEX_MAX];
    uint8_t mac_count = 0;
    macro_macros_snapshot(macros, MACRO_INDEX_MAX, &mac_count);
    web_format_status(pid, (p != NULL) ? p->name : "",
                      macros, mac_count, state,
                      (unsigned)macro_get_playback_step(), buf, sizeof(buf));
    return send_json(req, "200 OK", buf);
}

static esp_err_t handle_api_profiles(httpd_req_t *req)
{
    char buf[640];
    size_t off = buf_append(buf, sizeof(buf), 0, "[");
    uint8_t count = ir_db_get_profile_count();
    for (uint8_t i = 0; i < count && off < sizeof(buf); i++) {
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
     *  - otherwise -> the currently paired profile (normal remote use). */
    const ir_profile_t *p = NULL;
    uint32_t explicit_pid = 0;
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
    } else {
        p = ir_db_get_profile(remote_state_get_profile());
        if (p == NULL) {
            return send_json(req, "400 Bad Request", "{\"error\":\"not paired\"}");
        }
    }

    uint16_t addr = 0;
    uint8_t cmd = 0;
    if (!ir_db_find_key(p, (ir_key_t)key, &addr, &cmd)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"key undefined in profile\"}");
    }

    ir_send_raw_nec(addr, cmd);

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

    /* Live feedback: send the key through the paired profile exactly once. */
    const ir_profile_t *p = ir_db_get_profile(remote_state_get_profile());
    if (p != NULL) {
        uint16_t addr = 0;
        uint8_t cmd = 0;
        if (ir_db_find_key(p, (ir_key_t)key, &addr, &cmd)) {
            ir_send_raw_nec(addr, cmd);
        }
    }

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
    esp_err_t e = macro_play((uint8_t)id);
    if (e == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"error\":\"macro not found\"}");
    }
    if (e == ESP_ERR_INVALID_STATE) {
        /* macro_play returns INVALID_STATE for both "unpaired" and "already
         * playing"; distinguish for a clear client message. */
        if (macro_is_playing()) {
            return send_json(req, "400 Bad Request", "{\"error\":\"already playing\"}");
        }
        return send_json(req, "400 Bad Request", "{\"error\":\"not paired\"}");
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

/* ---- Server start ---- */

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
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
