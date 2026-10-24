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

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("WEB_LOGIC_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

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

    /* Test 3: unpaired /api/status payload -> paired_profile=0xFF, empty name. */
    char buf[192];
    int n = web_format_status(0xFF, "", buf, sizeof(buf));
    ASSERT(n > 0, "unpaired status format must succeed");
    ASSERT(strstr(buf, "\"paired_profile\":255") != NULL,
           "unpaired status must carry paired_profile=255");
    ASSERT(strstr(buf, "\"paired_name\":\"\"") != NULL,
           "unpaired status must carry empty paired_name");
    ASSERT(strstr(buf, "\"macros\":[]") != NULL,
           "status must carry the empty macros array (Task 4 fills it)");

    /* Test 4: paired status carries the profile id + display name. */
    n = web_format_status(0, "Hisense-A (NEC 0x04)", buf, sizeof(buf));
    ASSERT(n > 0, "paired status format must succeed");
    ASSERT(strstr(buf, "\"paired_profile\":0") != NULL,
           "paired status must carry paired_profile=0");
    ASSERT(strstr(buf, "\"paired_name\":\"Hisense-A (NEC 0x04)\"") != NULL,
           "paired status must carry the profile name");

    /* Test 5: undersized buffer -> -1, no buffer overflow. */
    char tiny[8];
    ASSERT(web_format_status(0xFF, "", tiny, sizeof(tiny)) == -1,
           "truncated status write must return -1");
    ASSERT(web_format_status(0xFF, "", NULL, 0) == -1,
           "NULL/zero-cap status write must return -1");
    ASSERT(web_parse_key_name("POWER") == IR_KEY_POWER,
           "parse is stateless across calls after error cases");

    printf("WEB_LOGIC_SELFTEST_PASS\n");
    return 0;
}

#endif /* WEB_LOGIC_SELFTEST */
