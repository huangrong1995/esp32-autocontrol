#pragma once
#include <stdint.h>
#include "ir_encoder.h"   /* IR_KEY_COUNT, ir_key_t */

/* Cap for profile-table and wizard-order arrays (NECx 码表会继续增长; the
 * /api/profiles JSON buffer is budgeted for 30+ groups). */
#define IR_PROFILE_COUNT_MAX 32u

#ifdef __cplusplus
extern "C" {
#endif

/* W13: one row of a non-32-bit keyset as literal mark/space timings.
 * `frame` is interleaved [mark,space,...,mark,space] in us; the LAST space
 * is the trailing idle (clamped to 0x7FFF).  `n_pairs` is the (mark,space)
 * pair count.  {0, NULL} marks an undefined key (never sent). */
typedef struct {
    uint16_t n_pairs;          /* number of (mark,space) pairs; 0 = undefined */
    const uint16_t *frame;     /* [mark,space,...,mark,space] us; NULL = undefined */
} ir_raw_frame_t;

/* cmds[] value for a defined raw-frame key: "defined, but the waveform lives
 * in raw_frames[i]".  NEVER emitted as a NEC command byte — ir_db_find_key
 * rejects it, and every send path checks ir_profile_raw_frame() first. */
#define IR_RAW_CMD_MARKER 0xFEu

/* Built-in IR profile table: one entry per remote/device family.
 * `cmds[index]` maps ir_key_t -> NEC command byte; 0xFF marks an
 * undefined/unknown key (never fabricated). */
typedef struct {
    const char *name;
    uint16_t nec_address;
    uint8_t cmds[IR_KEY_COUNT];   /* 未定义键 = 0xFF; raw 键 = IR_RAW_CMD_MARKER */
    /* W9 (tail-appended): per-profile leader override for short-header
     * (Samsung-style 4.5ms) candidates.  Both 0  => standard NEC
     * 9000/4500 leader (existing profiles keep the old behavior). */
    uint16_t leader_mark_us;   /* 0 = IR_NEC_LEADER_MARK_US  (9000) */
    uint16_t leader_space_us;  /* 0 = IR_NEC_LEADER_SPACE_US (4500) */
    /* W13 (tail-appended): per-key raw frames for non-NEC keysets, or NULL
     * for a NEC-only profile.  When non-NULL, each defined key i holds the
     * marker IR_RAW_CMD_MARKER in cmds[i]. */
    const ir_raw_frame_t *raw_frames;
} ir_profile_t;

/* Resolve a profile's effective leader timing (0 fields mean standard NEC). */
static inline void ir_profile_leader(const ir_profile_t *p,
                                     uint16_t *mark_us, uint16_t *space_us)
{
    if (p->leader_mark_us != 0) {
        *mark_us  = p->leader_mark_us;
        *space_us = p->leader_space_us;
    } else {
        *mark_us  = IR_NEC_LEADER_MARK_US;
        *space_us = IR_NEC_LEADER_SPACE_US;
    }
}

/* Return the profile at `profile_id`, or NULL if out of range. */
const ir_profile_t *ir_db_get_profile(uint8_t profile_id);

/* Number of profiles in the built-in table. */
uint8_t ir_db_get_profile_count(void);

/* Pointer to the built-in profile table (array of `const ir_profile_t *`).
 * Never NULL; its length is ir_db_get_profile_count(). */
const ir_profile_t *const *ir_db_profile_table(void);

/* W13 wizard candidate order: the 4 raw-frame Hisense keysets FIRST (they are
 * the strongest remaining candidates for the TV — the other 14 are NEC32 that
 * already failed), then the original 14 NEC/NECx profiles in PROFILES order.
 * Fills `*ids` with a parallel array mapping each order slot back to its REAL
 * PROFILES index (NVS profile-id semantics — PROFILES[] itself is untouched).
 * Returns the ordered table; length is ir_db_get_profile_count(). */
const ir_profile_t *const *ir_db_wizard_order(const uint8_t **ids);

/* Look up `key` in profile `p`. Returns 1 on success and fills `*addr`/`*cmd`;
 * returns 0 when the key is undefined (cmds[key]==0xFF) or arguments invalid.
 * For raw-frame profiles this mirrors wizard_send_current/ir_send_key: the
 * caller checks ir_profile_raw_frame() FIRST; ir_db_find_key keeps working as
 * the pure NEC resolver (the marker 0xFE is NOT a NEC command byte). */
uint8_t ir_db_find_key(const ir_profile_t *p, ir_key_t key,
                       uint16_t *addr, uint8_t *cmd);

/* W13 raw-frame tables, one per untested-keyeset profile, indexed by ir_key_t.
 * Defined keys hold {n_pairs>0, F_..}; undefined keys hold {0, NULL}.  Built
 * from the Xiaomi Mi Remote DB waveforms (see ir_raw_db.cc).  External C
 * linkage so the ir_db.cc TU (compiled as C++) can reference them. */
extern const ir_raw_frame_t g_midb_3359_frames[IR_KEY_COUNT];
extern const ir_raw_frame_t g_midb_3513_frames[IR_KEY_COUNT];
extern const ir_raw_frame_t g_midb_4070_frames[IR_KEY_COUNT];
extern const ir_raw_frame_t g_midb_110_frames[IR_KEY_COUNT];

/* Raw waveform for `key` in profile `p`, or NULL when the profile/key has no
 * raw frame (NEC-only profile, undefined key, or already consumed).  Every
 * transmit path checks this BEFORE ir_db_find_key/ir_send_raw_nec_var. */
static inline const ir_raw_frame_t *ir_profile_raw_frame(const ir_profile_t *p,
                                                         ir_key_t key)
{
    if (p == NULL || p->raw_frames == NULL || (unsigned)key >= IR_KEY_COUNT) {
        return NULL;
    }
    const ir_raw_frame_t *f = &p->raw_frames[key];
    if (f->n_pairs == 0 || f->frame == NULL) {
        return NULL;
    }
    return f;
}

#ifdef __cplusplus
}
#endif
