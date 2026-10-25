/*
 * Host self-test for the pure pairing-wizard state machine in
 * app/wizard_core.h. No RMT / IDF / NVS dependency — the wizard core only
 * advances a state machine; the web layer (later task) does the actual IR.
 *
 * Compile with plain cc and run:
 *     cc -Wall -Wextra -I main -DWIZARD_SELFTEST -o /tmp/wizard_test \
 *        main/app/test_wizard_core.c
 *     /tmp/wizard_test
 *
 * Standard: prints WIZARD_SELFTEST_PASS (and exits 0) on success, or
 * WIZARD_SELFTEST_FAIL:<detail> (exit 1) on failure.
 *
 * This unit intentionally lists in NO SRCS of main/CMakeLists.txt — it is
 * host-only, exactly like test_web_logic.c / test_macro_engine.c.
 */
#ifdef WIZARD_SELFTEST

#include <stdio.h>

#include "app/wizard_core.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("WIZARD_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    /* 测试夹具(命令码自造,仅测状态机,与真实码无关):
       P_A:POWER=0xAB VOL+=0xAC VOL-=0xAD CH+=0xAE CH-=0xAF MUTE=0xFF(故意无码)
           MENU=0xB1 EXIT=0xB2 OK=0xB3 UP=0xB4 DOWN=0xB5 LEFT=0xB6 RIGHT=0xB7
           0-9=0xC0..0xC9(不与 0xB1..0xB7 重复,断言不易混淆)
       P_B:电源=0x10, 其余 22 键全有码(0x11..0x26 顺排)
       P_C:电源=0xFF(无码,用于"跳过无码电源组"), 其余自造 */
    static const ir_profile_t P_A = { "P_A", 0x04,
        { 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xFF,
          0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
          0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9 } };
    static const ir_profile_t P_B = { "P_B", 0x20,
        { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
          0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 } };
    static const ir_profile_t P_C = { "P_C", 0x00,
        { 0xFF, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
          0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45 } };
    static const ir_profile_t *const G_TWO_VALID[]    = { &P_A, &P_B };    /* 用例 1/2/3/5 */
    static const ir_profile_t *const POWERLESS_FIRST[] = { &P_C, &P_A };   /* 用例 6 */
    static const ir_profile_t *const G_SINGLE[]        = { &P_C };         /* 用例 7 */
    wizard_ctx_t *ctx = NULL;

    /* 用例 1:锁定 A→校准顺序 = WIZARD_FUNC_ORDER 的可发子集(从 VOL+ 开始,POWER 不重验,
       原生 0xFF 的 MUTE 自动跳过);逐键有反应→DONE;keymap 数字键=继承的原生码、
       POWER/MUTE=原生码/0xFF 各就位。
       P_A 里 MUTE=0xFF,故当前键序列 = WIZARD_FUNC_ORDER 去掉 MUTE 的 11 个键。 */
    static const ir_key_t EXPECTED_SEND[] = {
        IR_KEY_VOL_UP, IR_KEY_VOL_DOWN, IR_KEY_CH_UP, IR_KEY_CH_DOWN,
        IR_KEY_MENU, IR_KEY_EXIT, IR_KEY_OK,
        IR_KEY_UP, IR_KEY_DOWN, IR_KEY_LEFT, IR_KEY_RIGHT,
    };
    wizard_cancel();
    ctx = wizard_start(G_TWO_VALID, 2);           /* 组0=A → 锁 A */
    ctx = wizard_answer_power(ctx, 1);            /* 有反应 → 锁定组0 → CALIBRATE */
    ASSERT(ctx->phase == WIZARD_PHASE_CALIBRATE, "POWER 有反应 → CALIBRATE");
    ASSERT(ctx->source_group == 0, "锁定组 0");
    for (int i = 0; i < (int)(sizeof(EXPECTED_SEND) / sizeof(EXPECTED_SEND[0])); i++) {
        ASSERT(wizard_current_key(ctx) == EXPECTED_SEND[i], "校准顺序从 VOL+ 开始,(跳过 MUTE)");
        ctx = wizard_answer_key(ctx, 1);          /* 全有反应 */
    }
    ASSERT(ctx->phase == WIZARD_PHASE_DONE, "11 个可发键走完 → DONE");
    ASSERT(ctx->keymap[IR_KEY_POWER] == 0xAB, "POWER keymap = 组码");
    ASSERT(ctx->keymap[IR_KEY_MUTE] == 0xFF, "无码键 MUTE keymap = 0xFF");
    ASSERT(ctx->keymap[IR_KEY_VOL_UP] == 0xAC, "功能键 keymap = 组码");
    ASSERT(ctx->keymap[IR_KEY_0] == 0xC0 && ctx->keymap[IR_KEY_9] == 0xC9,
           "数字键继承组原生码");
    ASSERT(wizard_answer_key(ctx, 1) == ctx, "DONE 后再 answer 是安全 no-op(状态不变)");

    /* 用例 2:POWER 无反应→前进;全组无反应→no_match 终止 */
    wizard_cancel();
    ctx = wizard_start(G_TWO_VALID, 2);           /* 组0=A、组1=B 都电源有码,停在组0 */
    ASSERT(ctx->phase == WIZARD_PHASE_TRY_POWER && ctx->group_idx == 0, "初始 TRY_POWER 组0");
    ctx = wizard_answer_power(ctx, 0);            /* A 无反应 → 组1(B) */
    ASSERT(ctx->phase == WIZARD_PHASE_TRY_POWER && ctx->group_idx == 1,
           "A 无反应 → 前进到组1,仍在 TRY_POWER");
    ASSERT(wizard_current_key(ctx) == IR_KEY_POWER, "当前键仍为 POWER");
    ctx = wizard_answer_power(ctx, 0);            /* B 无反应 → 无组可试 → no_match */
    ASSERT(ctx->source_group == WIZARD_NOMATCH && ctx->phase == WIZARD_PHASE_DONE,
           "全组无反应 → no_match 终止");

    /* 用例 2b:前进路径跳过电源无码组(C=组1)→ 直接 no_match */
    wizard_cancel();
    ctx = wizard_start((const ir_profile_t *const[]){ &P_A, &P_C }, 2);
    ctx = wizard_answer_power(ctx, 0);            /* A 无反应 → 跳过 C 电源无码组 → no_match */
    ASSERT(ctx->source_group == WIZARD_NOMATCH && ctx->phase == WIZARD_PHASE_DONE,
           "前进路径跳过电源无码组");

    /* 用例 3:功能键无反应 → 该键 0xFF → 继续下一个(不重试) */
    wizard_cancel();
    ctx = wizard_start(G_TWO_VALID, 2);
    ctx = wizard_answer_power(ctx, 1);
    ctx = wizard_answer_key(ctx, 0);              /* VOL+ 无反应 */
    ASSERT(ctx->keymap[IR_KEY_VOL_UP] == 0xFF, "无反应 → keymap 0xFF");
    ASSERT(wizard_current_key(ctx) == IR_KEY_VOL_DOWN, "无反应后继续下一个键(不重试)");
    ctx = wizard_answer_key(ctx, 1);              /* VOL- 有反应 */
    ASSERT(ctx->keymap[IR_KEY_VOL_DOWN] == 0xAD, "有反应 → keymap 组码");

    /* 用例 4:中途 cancel → 回 IDLE;cancel 后 answer 是安全 no-op(不崩、不前进) */
    wizard_start(G_TWO_VALID, 2);
    wizard_cancel();
    ASSERT(ctx->phase == WIZARD_PHASE_IDLE, "cancel → IDLE");
    wizard_answer_power(ctx, 1);
    ASSERT(ctx->phase == WIZARD_PHASE_IDLE, "cancel 后 answer 不前进");

    /* 用例 5:重新 start 覆盖旧结果(配对 A 全对 → 配对 B 全无反应 → 无残留) */
    wizard_cancel();
    ctx = wizard_start(G_TWO_VALID, 2);
    ctx = wizard_answer_power(ctx, 1);
    for (int i = 0; i < (int)(sizeof(EXPECTED_SEND) / sizeof(EXPECTED_SEND[0])); i++)
        ctx = wizard_answer_key(ctx, 1);          /* 配对 A:全键真码 */
    wizard_cancel();
    ctx = wizard_start(G_TWO_VALID, 2);
    ctx = wizard_answer_power(ctx, 0);            /* 配对 B:A 无反应 */
    ctx = wizard_answer_power(ctx, 0);            /* 配对 B:B 无反应 → no_match */
    ASSERT(ctx->phase == WIZARD_PHASE_DONE && ctx->source_group == WIZARD_NOMATCH,
           "重配对 → 覆盖,A 结果不残留");

    /* 用例 6:电源键无码的组被 start 跳过(不进入功能键遍历) */
    wizard_cancel();
    ctx = wizard_start(POWERLESS_FIRST, 2);       /* 组0=C 电源无码 → 自动跳到组1(A) */
    ASSERT(ctx->group_idx == 1, "电源无码组被 start 自动跳过");
    ASSERT(wizard_current_key(ctx) == IR_KEY_POWER, "跳到的组发 POWER");

    /* 用例 7:start 时全组电源无码 → 立即 no_match(非 NULL ctx) */
    wizard_cancel();
    ctx = wizard_start(G_SINGLE, 1);
    ASSERT(ctx->phase == WIZARD_PHASE_DONE && ctx->source_group == WIZARD_NOMATCH,
           "单组电源无码 → 立即 no_match");

    printf("WIZARD_SELFTEST_PASS\n");
    return 0;
}

#endif /* WIZARD_SELFTEST */
