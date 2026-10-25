/*
 * Host self-test for the pure HTTP-dispatch logic in app/web_logic.h
 * (key-name parsing + status JSON formatting). No httpd / IDF dependency.
 *
 * Compile with plain cc and run:
 *     cc -DWEB_LOGIC_SELFTEST -I<repo>/main -o /tmp/web_logic_selftest \
 *        <repo>/main/app/test_web_logic.c
 *     /tmp/web_logic_selftest
 *
 * Standard: prints WEB_LOGIC_SELFTEST_PASS (and exits 0) on success, or
 * WEB_LOGIC_SELFTEST_FAIL:<detail> (exit 1) on failure.
 *
 * This unit intentionally lists in no SRCS of main/CMakeLists.txt — it is
 * host-only, exactly like the Task 2 ir encoder self-test.
 */
#ifdef WEB_LOGIC_SELFTEST

#include <stdio.h>
#include <string.h>

#include "app/web_logic.h"
#include "app/pair_state.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("WEB_LOGIC_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

/* ---- pair 纯函数段(host stub:测试表自造,与固件 ir_db.cc 无关) ---- */
static const ir_profile_t T_PROF0 = { "T0", 0x04,
    { 0x5A, 0x5B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
const ir_profile_t *ir_db_get_profile(uint8_t id)
{
    static const ir_profile_t *const T_PROFS[] = { &T_PROF0 };
    return id < sizeof(T_PROFS) / sizeof(T_PROFS[0]) ? T_PROFS[id] : NULL;
}

int main(void)
{
    /* Test 1: key-name -> ir_key_t mapping. The plan pins these exact pins:
     * "POWER"->IR_KEY_POWER, "UP"->IR_KEY_UP, "VOL+"->IR_KEY_VOL_UP. */
    ASSERT(web_parse_key_name("POWER") == IR_KEY_POWER,
           "\"POWER\" must map to IR_KEY_POWER");
    ASSERT(web_parse_key_name("UP") == IR_KEY_UP,
           "\"UP\" must map to IR_KEY_UP");
    ASSERT(web_parse_key_name("VOL+") == IR_KEY_VOL_UP,
           "\"VOL+\" must map to IR_KEY_VOL_UP");

    /* Every wire name must round-trip onto the ir_key_t enum in order. */
    ASSERT(web_parse_key_name("VOL-") == IR_KEY_VOL_DOWN, "\"VOL-\"");
    ASSERT(web_parse_key_name("CH+") == IR_KEY_CH_UP, "\"CH+\"");
    ASSERT(web_parse_key_name("CH-") == IR_KEY_CH_DOWN, "\"CH-\"");
    ASSERT(web_parse_key_name("MUTE") == IR_KEY_MUTE, "\"MUTE\"");
    ASSERT(web_parse_key_name("MENU") == IR_KEY_MENU, "\"MENU\"");
    ASSERT(web_parse_key_name("EXIT") == IR_KEY_EXIT, "\"EXIT\"");
    ASSERT(web_parse_key_name("OK") == IR_KEY_OK, "\"OK\"");
    ASSERT(web_parse_key_name("DOWN") == IR_KEY_DOWN, "\"DOWN\"");
    ASSERT(web_parse_key_name("LEFT") == IR_KEY_LEFT, "\"LEFT\"");
    ASSERT(web_parse_key_name("RIGHT") == IR_KEY_RIGHT, "\"RIGHT\"");
    ASSERT(web_parse_key_name("0") == IR_KEY_0, "\"0\"");
    ASSERT(web_parse_key_name("9") == IR_KEY_9, "\"9\"");

    /* Test 2: unknown / malformed key names return -1 without crashing. */
    ASSERT(web_parse_key_name("PWR") == -1, "unknown key \"PWR\" must return -1");
    ASSERT(web_parse_key_name("") == -1, "empty string must return -1");
    ASSERT(web_parse_key_name("Vol+") == -1, "case-sensitive: \"Vol+\" must return -1");
    ASSERT(web_parse_key_name("VOL+ ") == -1, "trailing space must return -1");
    ASSERT(web_parse_key_name("POWERUP") == -1, "\"POWERUP\" must return -1");
    ASSERT(web_parse_key_name(NULL) == -1, "NULL must not crash, must return -1");

    /* Test 3: unpaired /api/status payload -> paired_profile=0xFF, empty name,
     * empty macros array, idle playback. */
    char buf[256];
    int n = web_format_status(0xFF, "", NULL, 0, "idle", 0, buf, sizeof(buf));
    ASSERT(n > 0, "unpaired status format must succeed");
    ASSERT(strstr(buf, "\"paired_profile\":255") != NULL,
           "unpaired status must carry paired_profile=255");
    ASSERT(strstr(buf, "\"paired_name\":\"\"") != NULL,
           "unpaired status must carry empty paired_name");
    ASSERT(strstr(buf, "\"macros\":[]") != NULL,
           "status must carry the (empty) macros array");
    ASSERT(strstr(buf, "\"playback\":{\"state\":\"idle\",\"current_step\":0}") != NULL,
           "idle status must carry the idle playback block");

    /* Test 4: paired status carries the profile id + display name. */
    n = web_format_status(0, "Hisense-A (NEC 0x04)", NULL, 0, "idle", 0,
                          buf, sizeof(buf));
    ASSERT(n > 0, "paired status format must succeed");
    ASSERT(strstr(buf, "\"paired_profile\":0") != NULL,
           "paired status must carry paired_profile=0");
    ASSERT(strstr(buf, "\"paired_name\":\"Hisense-A (NEC 0x04)\"") != NULL,
           "paired status must carry the profile name");

    /* Test 5: the macros array renders real index entries; first_key uses the
     * same wire names as web_parse_key_name. */
    macro_info_t macros[2];
    macros[0].id = 3;
    strcpy(macros[0].name, "PowerOff");
    macros[0].step_count = 2;
    macros[0].first_key = IR_KEY_POWER;
    macros[1].id = 7;
    strcpy(macros[1].name, "VolUp");
    macros[1].step_count = 1;
    macros[1].first_key = IR_KEY_VOL_UP;
    n = web_format_status(1, "Sony-B", macros, 2, "playing", 2, buf, sizeof(buf));
    ASSERT(n > 0, "status with macros must format");
    ASSERT(strstr(buf, "\"id\":3,\"name\":\"PowerOff\",\"step_count\":2,\"first_key\":\"POWER\"") != NULL,
           "macro 0 rendered with wire name POWER");
    ASSERT(strstr(buf, "\"id\":7,\"name\":\"VolUp\",\"step_count\":1,\"first_key\":\"VOL+\"") != NULL,
           "macro 1 rendered with wire name VOL+");
    ASSERT(strstr(buf, "\"playback\":{\"state\":\"playing\",\"current_step\":2}") != NULL,
           "playing status carries state + current_step");
    ASSERT(strstr(buf, "\"paired_profile\":1") != NULL,
           "macro-bearing status still carries paired_profile");

    /* Test 6: undersized buffer -> -1, no buffer overflow. */
    char tiny[8];
    ASSERT(web_format_status(0xFF, "", NULL, 0, "idle", 0, tiny, sizeof(tiny)) == -1,
           "truncated status write must return -1");
    ASSERT(web_format_status(0xFF, "", NULL, 0, "idle", 0, NULL, 0) == -1,
           "NULL/zero-cap status write must return -1");
    ASSERT(web_parse_key_name("POWER") == IR_KEY_POWER,
           "parse is stateless across calls after error cases");

    /* Test 7: profile_id must be range-checked as uint32 BEFORE any cast to
     * uint8_t. 256/512/65536 with 4 profiles all wrap to 0 on a uint8 cast —
     * they must be rejected (review finding on web_server.cc pairing path). */
    ASSERT(web_profile_id_valid(0, 4) != 0, "profile 0 of 4 must be valid");
    ASSERT(web_profile_id_valid(3, 4) != 0, "profile 3 of 4 must be valid");
    ASSERT(web_profile_id_valid(4, 4) == 0, "profile 4 of 4 must be invalid (>= count)");
    ASSERT(web_profile_id_valid(256, 4) == 0, "256 must not wrap to profile 0");
    ASSERT(web_profile_id_valid(512, 4) == 0, "512 must not wrap to profile 0");
    ASSERT(web_profile_id_valid(65536, 4) == 0, "65536 must not wrap to profile 0");
    ASSERT(web_profile_id_valid(0xFF000000u, 4) == 0, "huge uint32 id must be invalid");
    ASSERT(web_profile_id_valid(3, 0) == 0, "empty library: any id invalid");

    /* Test 8: macro names with quotes/backslashes/control chars must be
     * JSON-escaped in the status payload (fix round 1 — an unescaped name
     * would make GET /api/status emit INVALID JSON and kill the macro UI). */
    macro_info_t hostile[1];
    hostile[0].id = 4;
    strcpy(hostile[0].name, "say \"hi\\\"\n");   /* say "hi\"<LF> */
    hostile[0].step_count = 1;
    hostile[0].first_key = IR_KEY_OK;
    n = web_format_status(0xFF, "", hostile, 1, "idle", 0, buf, sizeof(buf));
    ASSERT(n > 0, "status with a hostile macro name must format");
    ASSERT(strstr(buf, "\"name\":\"say \\\"hi\\\\\\\"\\n\"") != NULL,
           "\" \\ and LF must escape as \\\" \\\\ \\n");
    ASSERT(strstr(buf, "\"name\":\"say \"hi") == NULL,
           "no raw (unescaped) quote may appear inside the name value");

    macro_info_t ctrl[1];
    ctrl[0].id = 5;
    strcpy(ctrl[0].name, "a\x01z\t");   /* a, SOH, z, TAB */
    ctrl[0].step_count = 1;
    ctrl[0].first_key = IR_KEY_OK;
    n = web_format_status(0xFF, "", ctrl, 1, "idle", 0, buf, sizeof(buf));
    ASSERT(n > 0, "status with control chars in macro name must format");
    ASSERT(strstr(buf, "\"name\":\"a\\u0001z\\t\"") != NULL,
           "SOH escapes as \\u0001 and TAB as \\t");

    static uint8_t T_CALIB[IR_KEY_COUNT];
    for (int i = 0; i < IR_KEY_COUNT; i++) T_CALIB[i] = 0xFF;
    T_CALIB[IR_KEY_POWER] = 0xA1; T_CALIB[IR_KEY_VOL_UP] = 0xA2;

    uint16_t addr; uint8_t cmd;
    ASSERT(pair_lookup(T_CALIB, IR_KEY_COUNT, IR_KEY_POWER) == 0xA1, "lookup calib 有码");
    ASSERT(pair_lookup(T_CALIB, IR_KEY_COUNT, IR_KEY_MUTE) == 0xFF, "lookup calib 无码 → 0xFF");
    ASSERT(pair_lookup(NULL, 0, IR_KEY_POWER) == 0xFF, "lookup NULL calib → 0xFF");
    ASSERT(pair_lookup(T_CALIB, IR_KEY_COUNT, IR_KEY_COUNT) == 0xFF, "lookup 越界 → 0xFF");
    ASSERT(pair_resolve_key(T_CALIB, IR_KEY_COUNT, 0, IR_KEY_POWER, &addr, &cmd) == 1
           && addr == 0x04 && cmd == 0xA1, "calib 有码 → addr=来源组 nec_address, cmd=calib[key]");
    ASSERT(pair_resolve_key(T_CALIB, IR_KEY_COUNT, 0, IR_KEY_MUTE, &addr, &cmd) == 0,
           "calib 键 0xFF → 0 不发送");
    ASSERT(pair_resolve_key(NULL, 0, 0, IR_KEY_POWER, &addr, &cmd) == 1
           && addr == 0x04 && cmd == 0x5A, "NULL calib → legacy 原生表");
    ASSERT(pair_resolve_key(NULL, 0, 0, IR_KEY_MUTE, &addr, &cmd) == 0, "legacy 键 0xFF → 0");
    ASSERT(pair_resolve_key(NULL, 0, 1, IR_KEY_POWER, &addr, &cmd) == 0,
           "profile 越界(NULL)→ 0 不崩");
    ASSERT(pair_resolve_key(T_CALIB, IR_KEY_COUNT, 0, IR_KEY_COUNT, &addr, &cmd) == 0,
           "key 越界 → 0 不崩");
    ASSERT(pair_resolve_key(T_CALIB, IR_KEY_COUNT, 0, IR_KEY_POWER, NULL, &cmd) == 0,
           "NULL addr → 0 不崩");
    ASSERT(pair_resolve_key(T_CALIB, IR_KEY_COUNT - 1, 0, IR_KEY_POWER, &addr, &cmd) == 1
           && cmd == 0x5A, "calib len!=23 → 视为 legacy");

    /* ---- web_format_wizard 段(Step 4:各 state 文案、keys 0/1 渲染、NULL keys 省略 calib_group) ---- */
    char wbuf[512];
    int wn;

    /* idle: state/group_id/key 文案 + NULL keys 省略 calib_group。 */
    strcpy(wbuf, "{\"paired_profile\":255,\"playback\":{\"state\":\"idle\"}");
    wn = web_format_wizard("idle", 0, "", 4, "", NULL, 0, wbuf, sizeof(wbuf));
    ASSERT(wn > 0, "idle wizard append must succeed");
    ASSERT(strstr(wbuf, "\"wizard\":{\"state\":\"idle\",\"group_id\":0,\"key\":\"\",\"group_count\":4,\"group_name\":\"\"}") != NULL,
           "idle wizard block text");
    ASSERT(strstr(wbuf, "calib_group") == NULL, "NULL keys omits calib_group");
    ASSERT(wbuf[wn] == '\0', "returned offset is the NUL position");

    /* calibrate with a real key name, keys still NULL. */
    strcpy(wbuf, "{\"paired_profile\":255,\"playback\":{\"state\":\"idle\"}");
    wn = web_format_wizard("calibrate", 0, "VOL+", 4, "", NULL, 0, wbuf, sizeof(wbuf));
    ASSERT(wn > 0, "calibrate wizard append must succeed");
    ASSERT(strstr(wbuf, "\"wizard\":{\"state\":\"calibrate\",\"group_id\":0,\"key\":\"VOL+\",\"group_count\":4,\"group_name\":\"\"}") != NULL,
           "calibrate wizard block text");
    ASSERT(strstr(wbuf, "calib_group") == NULL, "NULL keys omits calib_group again");

    /* no_match state rendering. */
    strcpy(wbuf, "{\"ok\":true");
    wn = web_format_wizard("no_match", 3, "", 4, "", NULL, 0, wbuf, sizeof(wbuf));
    ASSERT(wn > 0, "no_match wizard append must succeed");
    ASSERT(strstr(wbuf, "\"wizard\":{\"state\":\"no_match\",\"group_id\":3,\"key\":\"\",\"group_count\":4,\"group_name\":\"\"}") != NULL,
           "no_match wizard block text");

    /* keys: raw keymap bytes normalize to 0/1; POWER(idx 0) + VOL+(idx 1) set. */
    static uint8_t WKEYS[IR_KEY_COUNT];
    for (int i = 0; i < IR_KEY_COUNT; i++) WKEYS[i] = 0;
    WKEYS[IR_KEY_POWER] = 0xA1; WKEYS[IR_KEY_VOL_UP] = 0xA2;   /* !=0 → rendered 1 */
    strcpy(wbuf, "{\"paired_profile\":255,\"playback\":{\"state\":\"idle\"}");
    wn = web_format_wizard("try_power", 2, "POWER", 4, "Generic NEC 0x20 (LG/Vizio)",
                           WKEYS, 3, wbuf, sizeof(wbuf));
    ASSERT(wn > 0, "try_power + keys append must succeed");
    ASSERT(strstr(wbuf, "\"wizard\":{\"state\":\"try_power\",\"group_id\":2,\"key\":\"POWER\",\"group_count\":4,\"group_name\":\"Generic NEC 0x20 (LG/Vizio)\"}") != NULL,
           "try_power wizard block text");
    ASSERT(strstr(wbuf, "\"calib_group\":3,\"keys\":[1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]") != NULL,
           "keys array renders 1/1/0... and calib_group 3");

    /* Capacity: too-small buffer → -1 and the existing prefix is untouched. */
    char wtiny[16];
    strcpy(wtiny, "{\"ok\":true}");
    ASSERT(web_format_wizard("idle", 0, "", 4, "", NULL, 0, wtiny, sizeof(wtiny)) == -1,
           "too-small wizard append must return -1");
    ASSERT(strcmp(wtiny, "{\"ok\":true}") == 0, "overflow must not disturb the prefix");
    ASSERT(web_format_wizard("idle", 0, "", 4, "", NULL, 0, NULL, 0) == -1,
           "NULL/zero-cap wizard append must return -1");

    printf("WEB_LOGIC_SELFTEST_PASS\n");
    return 0;
}

#endif /* WEB_LOGIC_SELFTEST */
