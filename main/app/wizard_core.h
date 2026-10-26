#pragma once

/*
 * Pure pairing-wizard state machine (Task 1 of the guided-pairing plan).
 *
 * This is PURE LOGIC: it never touches IR hardware, NVS, or any IDF API — it
 * only advances a pairing state machine. Sending IR is the web layer's job
 * (a later task), not this file's.
 *
 * The global single-instance context lives HERE as a `static` per-TU variable,
 * the same pattern as app/macro_core.h / app/web_logic.h: every TU that
 * includes this header gets an independent copy of the pure logic. The
 * firmware includes it from exactly ONE TU (web_server.cc, from a later task).
 * The host self-test test_wizard_core.c gets its own independent copy.
 *
 * Hard project constraint (preserved verbatim): 不伪造红外码 — unknown keys
 * stay 0xFF (undefined), never guessed. The keymap holds either the locked
 * group's native code or 0xFF.
 *
 * Flow:
 *   wizard_start(groups, count)  -> skip power-less groups, latch the first
 *                                    power-capable group in TRY_POWER.
 *   wizard_answer_power(ptr, r)  -> reacted: lock that group, prefill keymap,
 *                                    enter CALIBRATE; not reacted: advance to
 *                                    the next untested power-capable group,
 *                                    or DONE/NOMATCH when none are left.
 *   wizard_answer_key(ptr, r)    -> CALIBRATE: verify each WIZARD_FUNC_ORDER
 *                                    key in turn (native 0xFF keys auto-skip);
 *                                    no reaction sets that keymap slot to 0xFF.
 *   wizard_current_key(ptr)      -> what the web layer should send next.
 *   wizard_cancel()              -> back to IDLE.
 */

#include <stdint.h>
#include <stddef.h>

#include "ir/ir_db.h"        /* ir_profile_t */
#include "ir/ir_encoder.h"   /* ir_key_t, IR_KEY_COUNT */

typedef enum {
    WIZARD_PHASE_IDLE = 0,
    WIZARD_PHASE_TRY_POWER,
    WIZARD_PHASE_CALIBRATE,
    WIZARD_PHASE_DONE
} wizard_phase_t;

#define WIZARD_POWER_KEYS 1u   /* 锁定位第一个被验证的功能键下标 */
#define WIZARD_NOMATCH 0xFFu   /* source_group 的"无匹配"标记 */
#define WIZARD_FUNC_KEYS 12u

/* Calibration order: 12 function keys, POWER not re-verified after lock. */
static const ir_key_t WIZARD_FUNC_ORDER[WIZARD_FUNC_KEYS] = {
    IR_KEY_VOL_UP, IR_KEY_VOL_DOWN, IR_KEY_CH_UP, IR_KEY_CH_DOWN,
    IR_KEY_MUTE, IR_KEY_MENU, IR_KEY_EXIT, IR_KEY_OK,
    IR_KEY_UP, IR_KEY_DOWN, IR_KEY_LEFT, IR_KEY_RIGHT,
};

/* Minimum contract from the plan, plus the two trailing extension fields (the
 * wizard must remember the `groups` array / `count` passed to wizard_start so
 * later answer_* calls can read groups[group_idx].cmds[...]). The listed
 * fields keep their exact names/types — the listed order too. */
typedef struct {
    uint8_t group_idx;                 /* current TRY_POWER / locked group */
    uint8_t key_idx;                   /* index into WIZARD_FUNC_ORDER */
    uint8_t keymap[IR_KEY_COUNT];      /* locked group's codes; 0xFF = no code */
    uint8_t source_group;              /* locked group, or WIZARD_NOMATCH */
    uint8_t phase;                     /* wizard_phase_t */
    const ir_profile_t *const *groups; /* extension: candidate profiles */
    uint8_t group_count;               /* extension: number of candidates */
    /* W13 (tail-appended): parallel mapping from a wizard group index to the
     * REAL PROFILES index (NVS profile-id semantics).  When non-NULL,
     * source_group indexes this array; when NULL it IS the real index
     * (identity — the pure-logic self-test keeps passing unchanged). */
    const uint8_t *profile_ids;
} wizard_ctx_t;

/* Global single-instance context (per-TU static, same as macro_core.h). */
static wizard_ctx_t wizard_core_ctx;

/* Internal assistant: reset ctx to IDLE defaults (keymap fully 0xFF). */
static inline void wizard_core_reset(wizard_ctx_t *ctx)
{
    ctx->group_idx    = 0;
    ctx->key_idx      = 0;
    for (int i = 0; i < IR_KEY_COUNT; i++) {
        ctx->keymap[i] = 0xFF;
    }
    ctx->source_group = WIZARD_NOMATCH;
    ctx->phase        = WIZARD_PHASE_IDLE;
    ctx->groups       = NULL;
    ctx->group_count  = 0;
    ctx->profile_ids  = NULL;
}

/* The REAL PROFILES index of the locked source group — the value to store in
 * NVS (source_group is a wizard-order index when profile_ids is set). */
static inline uint8_t wizard_core_real_group_id(const wizard_ctx_t *ctx)
{
    if (ctx == NULL || ctx->source_group == WIZARD_NOMATCH) {
        return WIZARD_NOMATCH;
    }
    return (ctx->profile_ids != NULL) ? ctx->profile_ids[ctx->source_group]
                                      : ctx->source_group;
}

/* W13: hand the wizard its group-index -> real-PROFILES-index mapping.  Must
 * be called right after wizard_start().  NULL restores identity (self-test). */
static inline void wizard_set_profile_ids(wizard_ctx_t *ctx,
                                          const uint8_t *profile_ids)
{
    if (ctx != NULL) {
        ctx->profile_ids = profile_ids;
    }
}

/*
 * Internal settle loop (rule 4): while the current WIZARD_FUNC_ORDER slot is a
 * native 0xFF key in the locked group, mark that keymap slot 0xFF and advance
 * key_idx. Guarantees wizard_current_key() always stands on the next native
 * non-0xFF function key (or the machine is finished). Must only be called when
 * source_group is valid (CALIBRATE or just before locking). The caller decides
 * DONE when key_idx reaches WIZARD_FUNC_KEYS.
 */
static inline void wizard_core_settle(wizard_ctx_t *ctx)
{
    while (ctx->key_idx < WIZARD_FUNC_KEYS &&
           ctx->groups[ctx->source_group]
               ->cmds[WIZARD_FUNC_ORDER[ctx->key_idx]] == 0xFF) {
        ctx->keymap[WIZARD_FUNC_ORDER[ctx->key_idx]] = 0xFF;
        ctx->key_idx++;
    }
}

/* Start a pairing pass over `count` candidate groups. Skips power-less groups
 * (native POWER == 0xFF) from group 0, latches the first power-capable group in
 * TRY_POWER. keymap is NOT prefilled until a group is locked. When every group
 * is power-less: phase = DONE, source_group = WIZARD_NOMATCH (still returns a
 * valid ctx, never NULL). */
static inline wizard_ctx_t *wizard_start(const ir_profile_t *const *groups,
                                         uint8_t count)
{
    wizard_core_reset(&wizard_core_ctx);
    wizard_core_ctx.groups      = groups;
    wizard_core_ctx.group_count = count;
    for (uint8_t i = 0; i < count; i++) {
        if (groups[i] != NULL && groups[i]->cmds[IR_KEY_POWER] != 0xFF) {
            wizard_core_ctx.group_idx = i;
            wizard_core_ctx.phase     = WIZARD_PHASE_TRY_POWER;
            return &wizard_core_ctx;
        }
    }
    wizard_core_ctx.phase        = WIZARD_PHASE_DONE;
    wizard_core_ctx.source_group = WIZARD_NOMATCH;
    return &wizard_core_ctx;
}

/*
 * Answer the current TRY_POWER key (IR_KEY_POWER). Not in TRY_POWER -> no-op
 * (never crashes, never advances). Reacted: lock the current group — prefill
 * keymap with its native cmds (digits inherit), set source_group, key_idx=0,
 * run the settle loop, enter CALIBRATE (or DONE when the group has no native
 * function key at all). Not reacted: advance to the next power-capable group
 * (staying TRY_POWER), or DONE/NOMATCH when none remain.
 */
static inline wizard_ctx_t *wizard_answer_power(wizard_ctx_t *ctx, int reacted)
{
    if (ctx == NULL || ctx->phase != WIZARD_PHASE_TRY_POWER) {
        return ctx;   /* no-op */
    }
    if (reacted) {
        const ir_profile_t *p = ctx->groups[ctx->group_idx];
        ctx->source_group = ctx->group_idx;
        for (int i = 0; i < IR_KEY_COUNT; i++) {
            ctx->keymap[i] = p->cmds[i];   /* digits inherit native codes */
        }
        ctx->key_idx = 0;
        wizard_core_settle(ctx);           /* settle on lock (rule 4) */
        ctx->phase = (ctx->key_idx >= WIZARD_FUNC_KEYS)
                   ? WIZARD_PHASE_DONE : WIZARD_PHASE_CALIBRATE;
        return ctx;
    }
    /* 状态机单调前移,不回访,故无需 tested_mask;from group_idx+1 起保证不会重测 */
    for (uint8_t i = (uint8_t)(ctx->group_idx + 1u); i < ctx->group_count; i++) {
        if (ctx->groups[i] != NULL && ctx->groups[i]->cmds[IR_KEY_POWER] != 0xFF) {
            ctx->group_idx = i;            /* stay TRY_POWER, new group */
            return ctx;
        }
    }
    ctx->phase        = WIZARD_PHASE_DONE;
    ctx->source_group = WIZARD_NOMATCH;
    return ctx;
}

/*
 * Answer the current calibration key (wizard_current_key()). Not in CALIBRATE
 * -> no-op. Runs the settle loop first (auto-skip native 0xFF keys); key_idx
 * past the end -> DONE. Reacted: keymap keeps the prefilled native code;
 * not reacted: that keymap slot is forced to 0xFF. Advances key_idx, settles
 * again, and DONE when the order is exhausted.
 */
static inline wizard_ctx_t *wizard_answer_key(wizard_ctx_t *ctx, int reacted)
{
    if (ctx == NULL || ctx->phase != WIZARD_PHASE_CALIBRATE) {
        return ctx;   /* no-op */
    }
    wizard_core_settle(ctx);               /* settle before every advance */
    if (ctx->key_idx >= WIZARD_FUNC_KEYS) {
        ctx->phase = WIZARD_PHASE_DONE;
        return ctx;
    }
    ir_key_t k = WIZARD_FUNC_ORDER[ctx->key_idx];
    if (!reacted) {
        ctx->keymap[k] = 0xFF;             /* 不伪造红外码: no guess */
    }
    ctx->key_idx++;
    wizard_core_settle(ctx);               /* settle after every advance */
    if (ctx->key_idx >= WIZARD_FUNC_KEYS) {
        ctx->phase = WIZARD_PHASE_DONE;
    }
    return ctx;
}

/*
 * The key the web layer should send/verify next: TRY_POWER -> IR_KEY_POWER;
 * CALIBRATE -> WIZARD_FUNC_ORDER[key_idx] (settle guarantees it is a valid,
 * native non-0xFF key); DONE/IDLE -> IR_KEY_COUNT ("none"). Pure read — never
 * touches IR or NVS.
 */
static inline ir_key_t wizard_current_key(const wizard_ctx_t *ctx)
{
    if (ctx == NULL) {
        return IR_KEY_COUNT;
    }
    if (ctx->phase == WIZARD_PHASE_TRY_POWER) {
        return IR_KEY_POWER;
    }
    if (ctx->phase == WIZARD_PHASE_CALIBRATE) {
        return WIZARD_FUNC_ORDER[ctx->key_idx];
    }
    return IR_KEY_COUNT;
}

/* Cancel the current pass: the global ctx returns to IDLE (keymap all 0xFF,
 * source_group = WIZARD_NOMATCH). Any later answer_* is a safe no-op. */
static inline void wizard_cancel(void)
{
    wizard_core_reset(&wizard_core_ctx);
}