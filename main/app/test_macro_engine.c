/*
 * Host self-test for the pure macro-engine core (app/macro_core.h).
 * No RMT / IDF / FreeRTOS dependency — the replay sequencer runs on injected
 * callbacks (fake `send` recording the emitted keys, fake `sleep_ms` able to
 * inject an abort at a chosen point).
 *
 * Compile with plain cc and run:
 *     cc -Wall -I main -o /tmp/macro_test main/app/test_macro_engine.c
 *     /tmp/macro_test
 *
 * Standard: prints MACRO_SELFTEST_PASS (and exits 0) on success, or
 * MACRO_SELFTEST_FAIL:<detail> (exit 1) on failure.
 *
 * This unit intentionally lists in NO SRCS of main/CMakeLists.txt — it is
 * host-only, exactly like test_ir_encoder.c and test_web_logic.c.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "app/macro_core.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("MACRO_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

/* ---- injected-callback fakes ---- */

#define MAX_SENT 16u

static ir_key_t sent_keys[MAX_SENT];
static unsigned sent_count;
static unsigned sleep_count;

/* Abort injection knobs (0 = disabled). */
static unsigned abort_after_sends;   /* set flag once this many sends landed */
static unsigned abort_on_sleep_call; /* set flag on this sleep_ms call */
static volatile uint8_t abort_flag;

static void fake_send(ir_key_t key)
{
    if (sent_count < MAX_SENT) {
        sent_keys[sent_count++] = key;
    }
    if (abort_after_sends != 0 && sent_count >= abort_after_sends) {
        abort_flag = 1;   /* inject a stop right after the chosen send */
    }
}

static void fake_sleep(uint32_t ms)
{
    (void)ms;
    sleep_count++;
    if (abort_on_sleep_call != 0 && sleep_count >= abort_on_sleep_call) {
        abort_flag = 1;   /* inject a stop mid-wait */
    }
}

static void reset_fakes(void)
{
    sent_count      = 0;
    sleep_count     = 0;
    abort_after_sends   = 0;
    abort_on_sleep_call = 0;
    abort_flag          = 0;
}

int main(void)
{
    /* === Test 1: record 3 steps -> snapshot -> serialize -> reload === */

    ASSERT(macro_core_record_active() == 0, "not recording before start");
    ASSERT(macro_core_record_add(IR_KEY_POWER, 100) == -1,
           "add while not recording must fail");
    macro_core_record_start();
    ASSERT(macro_core_record_active() == 1, "recording active after start");
    ASSERT(macro_core_record_step_count() == 0, "count starts at 0");
    ASSERT(macro_core_record_first_key() == IR_KEY_COUNT,
           "first_key is 'none' (IR_KEY_COUNT) before any step");

    ASSERT(macro_core_record_add(IR_KEY_POWER, 500) == 0, "add step 0 POWER/500ms");
    ASSERT(macro_core_record_add(IR_KEY_VOL_UP, 150) == 0, "add step 1 VOL_UP/150ms");
    ASSERT(macro_core_record_add(IR_KEY_MUTE, 300) == 0, "add step 2 MUTE/300ms");
    ASSERT(macro_core_record_step_count() == 3, "count == 3 after adds");
    ASSERT(macro_core_record_first_key() == IR_KEY_POWER, "first_key tracked");
    ASSERT(macro_core_record_dirty() != 0, "dirty set after adds");

    /* macro_save snapshot: copy the temp buffer, then clear the dirty flag. */
    uint16_t n = macro_core_record_step_count();
    macro_step_t saved[MACRO_MAX_STEPS];
    memcpy(saved, macro_core_record_steps(), (size_t)n * sizeof(macro_step_t));
    macro_core_record_clear_dirty();
    ASSERT(macro_core_record_dirty() == 0, "dirty cleared after save snapshot");

    /* round-trip through the NVS blob layout */
    uint8_t blob[256];
    size_t wrote = macro_core_serialize(saved, n, blob, sizeof(blob));
    ASSERT(wrote == (size_t)n * MACRO_STEP_ENCODED_BYTES, "serialize writes n*5 bytes");
    ASSERT(macro_core_serialize(saved, n, blob, 4) == 0,
           "undersized serialize must return 0");
    ASSERT(macro_core_serialize(saved, n, NULL, sizeof(blob)) == 0,
           "NULL out serialize must return 0");

    macro_step_t reloaded[MACRO_MAX_STEPS];
    uint16_t rn = macro_core_deserialize(blob, wrote, reloaded, MACRO_MAX_STEPS);
    ASSERT(rn == n, "deserialize recovers the step count");
    for (uint16_t i = 0; i < rn; i++) {
        if (reloaded[i].key != saved[i].key) {
            ASSERT(0, "reloaded key must equal saved key");
        }
        if (reloaded[i].wait_ms != saved[i].wait_ms) {
            ASSERT(0, "reloaded wait_ms must equal saved wait_ms");
        }
    }
    ASSERT(macro_core_deserialize(blob, wrote, reloaded, 2) == 0,
           "deserialize over `max` must return 0");
    ASSERT(macro_core_deserialize(NULL, wrote, reloaded, MACRO_MAX_STEPS) == 0,
           "NULL data deserialize must return 0");
    ASSERT(macro_core_deserialize(blob, 8, reloaded, MACRO_MAX_STEPS) == 1,
           "truncated blob decodes whole steps only (8/5 => 1)");

    /* === Test 2: replay order / wait-before-send / abort semantics === */

    reset_fakes();
    int r = macro_core_replay(saved, n, fake_send, fake_sleep, &abort_flag);
    ASSERT(r == (int)n, "full replay returns n (completed)");
    ASSERT(sent_count == 3 && sleep_count > 0,
           "all 3 keys sent and sleep happened before sends");
    ASSERT(sent_keys[0] == IR_KEY_POWER && sent_keys[1] == IR_KEY_VOL_UP
           && sent_keys[2] == IR_KEY_MUTE, "send order follows the recorded steps");
    ASSERT(sent_count == 3, "replay sent exactly the 3 macro keys");

    /* inject a stop right after the 2nd send: replay must abort at step 2,
     * with keys 0..1 sent and step 2 NOT sent. */
    reset_fakes();
    abort_after_sends = 2;
    r = macro_core_replay(saved, n, fake_send, fake_sleep, &abort_flag);
    ASSERT(r == 2, "abort after 2nd send returns 2 (step 2 not reached)");
    ASSERT(sent_count == 2, "exactly keys 0..1 sent before abort");
    ASSERT(sent_keys[sent_count - 1] == IR_KEY_VOL_UP,
           "last sent key is step 1's key (VOL_UP), the interrupting key pattern");

    /* a long wait must be promptly interruptible: chunked sleep checks the
     * flag between pieces, so aborting mid-wait sends nothing from that step. */
    reset_fakes();
    macro_step_t long_wait = { IR_KEY_MENU, 400 };
    abort_on_sleep_call = 3;
    r = macro_core_replay(&long_wait, 1, fake_send, fake_sleep, &abort_flag);
    ASSERT(r == 0, "abort mid-wait returns the interrupted step (0)");
    ASSERT(sent_count == 0, "no key sent when aborted during the wait");
    ASSERT(abort_flag != 0, "the abort flag was consumed as set");

    /* chunk granularity: 500ms wait == ten 50ms sleep pieces. */
    reset_fakes();
    macro_step_t chunk_probe = { IR_KEY_OK, 500 };
    r = macro_core_replay(&chunk_probe, 1, fake_send, fake_sleep, &abort_flag);
    ASSERT(r == 1, "chunk probe completes");
    ASSERT(sleep_count == 10, "500ms wait is chopped into ten 50ms pieces");

    /* NULL-arg safety of the sequencer. */
    reset_fakes();
    ASSERT(macro_core_replay(NULL, n, fake_send, fake_sleep, &abort_flag) == -1,
           "NULL steps -> -1");
    ASSERT(macro_core_replay(saved, n, NULL, fake_sleep, &abort_flag) == -1,
           "NULL send -> -1");
    ASSERT(macro_core_replay(saved, n, fake_send, NULL, &abort_flag) == -1,
           "NULL sleep_ms -> -1");
    ASSERT(macro_core_replay(saved, n, fake_send, fake_sleep, NULL) == -1,
           "NULL abort_flag -> -1");
    ASSERT(macro_core_replay(saved, 0, fake_send, fake_sleep, &abort_flag) == 0,
           "zero-step replay completes (returns n == 0)");

    /* === Test 3: nonexistent / invalid id path === */

    uint8_t reg_id = 0;
    ASSERT(macro_core_index_get_count() == 0, "index starts empty");
    ASSERT(macro_core_index_add("TestMacro", n, saved[0].key, &reg_id) == 0,
           "index add succeeds");
    ASSERT(macro_core_index_get_count() == 1, "index count == 1");
    ASSERT(macro_core_index_find(reg_id) >= 0, "registered id is found");
    ASSERT(macro_core_index_get_by_id(reg_id) != NULL, "registered id resolves");
    {
        const macro_info_t *info = macro_core_index_get_by_id(reg_id);
        ASSERT(info->step_count == n, "index records step_count");
        ASSERT(info->first_key == IR_KEY_POWER, "index records first_key");
        ASSERT(strcmp(info->name, "TestMacro") == 0, "index records the name");
    }
    ASSERT(macro_core_index_get_by_id(0xFF) == NULL,
           "nonexistent id 0xFF returns NULL");
    ASSERT(macro_core_index_find(0xFE) == -1, "nonexistent id find returns -1");
    ASSERT(macro_core_index_remove(0xFF) == -1, "removing nonexistent id fails");
    ASSERT(macro_core_index_add("", 1, IR_KEY_OK, &reg_id) == -1,
           "empty name rejected");
    ASSERT(macro_core_index_remove(reg_id) == 0, "removing registered id succeeds");
    ASSERT(macro_core_index_get_count() == 0, "index count back to 0 after remove");

    /* id auto-increment: the freed id is NOT recycled. */
    macro_core_index_reset();
    uint8_t id_a = 0xFF, id_b = 0xFF;
    ASSERT(macro_core_index_add("A", 1, IR_KEY_OK, &id_a) == 0 && id_a == 0,
           "first macro gets id 0");
    ASSERT(macro_core_index_remove(id_a) == 0, "remove macro A");
    ASSERT(macro_core_index_add("B", 1, IR_KEY_OK, &id_b) == 0 && id_b == 1,
           "deleted id 0 not recycled; B gets id 1");

    /* index capacity. */
    macro_core_index_reset();
    uint8_t id_tmp = 0;
    int added = 0;
    while (macro_core_index_add("F", 1, IR_KEY_OK, &id_tmp) == 0) {
        added++;
    }
    ASSERT(added == (int)MACRO_INDEX_MAX, "index accepts MACRO_INDEX_MAX entries");
    ASSERT(macro_core_index_get_count() == MACRO_INDEX_MAX,
           "index holds all 16 after the fill attempt");
    ASSERT(macro_core_index_get_by_id((uint8_t)added) == NULL,
           "17th add failed: id 16 was never allocated/inserted");

    /* === Test 4: recording cap (100 steps) and reset === */

    macro_core_record_start();
    int k = 0;
    while (macro_core_record_add(IR_KEY_OK, 10) == 0) {
        k++;
    }
    ASSERT(k == (int)MACRO_MAX_STEPS, "100 steps accepted, 101st rejected");
    ASSERT(macro_core_record_step_count() == MACRO_MAX_STEPS,
           "step count pins at the 100-step cap");
    macro_core_record_reset();
    ASSERT(macro_core_record_active() == 0, "reset stops recording");
    ASSERT(macro_core_record_step_count() == 0, "reset clears the buffer");
    ASSERT(macro_core_record_first_key() == IR_KEY_COUNT,
           "reset clears first_key back to 'none'");

    printf("MACRO_SELFTEST_PASS\n");
    return 0;
}