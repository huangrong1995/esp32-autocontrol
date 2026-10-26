#include "ir/ir_db.h"

#include <stddef.h>

/* ---- Code sourcing -------------------------------------------------------
 * profile[0] codes are the REAL discrete IR command values from the official
 * "Hisense Discrete IR Commands for copy paste" PDF and the "RS232 Commands
 * & IR Protocol" PDF (Hisense-USA):
 *   https://assets.hisense-usa.com/assets/ProductDownloads/16/c995389f20/Hisense-Discrete-IR-Commands-for-copy-paste_0.pdf
 * NEC1 protocol, 38 kHz, LOW CUSTOM=0x04, HIGH CUSTOM=0xFB, LSB-first.
 * On-wire byte order for every key is: 0x04, 0xFB, <cmd>, <~cmd>.
 *
 * MUTE: the official discrete-IR table has NO IR code for mute (only an
 * RS-232 serial "MUTE0001" command / remote-button simulator BTTN1031), so it
 * remains 0xFF (undefined) rather than inventing a value.
 *
 * profile[1] ("Generic NEC 0x20") is the shared NEC addr-0x20 family — the
 * codeset publicly documented as LG/Vizio (probonopd/lirc-remotes, ZeroDroid
 * IrRemoteDatabase.kt, OMOTE-Firmware device_lgTV.cpp). It is NOT a Hisense
 * table: no Hisense-branded NEC addr-0x20 key map is citable, so only
 * high-confidence keys consistent across the LG/Vizio sources are filled and
 * the rest stay 0xFF (never fabricated / never mislabelled).
 *
 * profile[2] ("Hisense-C") carries the REAL Hisense NEC-extended family,
 * captured live from an 85A6NTUK by WillEastbury/PICO_UNIVERSAL_REMOTE:
 *   https://github.com/WillEastbury/PICO_UNIVERSAL_REMOTE/blob/main/server/hisense_codes.py
 * NECx protocol, 38 kHz, extended address 0xBF00. On-wire byte order for
 * every key is: 00, BF, <cmd>, <~cmd>. All 23 keys of the layout are filled
 * with the captured <cmd> values. The 0xBA00 special keys in that source
 * (vidaa/nowtv/freely/kid) are NOT part of this 23-key layout and are
 * intentionally not filled.
 *
 * profile[3] reuses the official NEC1 addr-0x04 keymap as a generic NEC TV
 * layout (the only fully verified NEC1 TV table available).
 *
 * profiles[4..13] (W9) are REAL Hisense codesets from the Xiaomi Mi Remote
 * production database — see the "W9" block comment before them for the full
 * sourcing (ysard/mi_remote_database dump decrypted per
 * halifox/android_xiaomi_ir_lab).  They are appended only (never reordered/
 * deleted/renamed) because PROFILES order === NVS profile-id semantics.
 * ------------------------------------------------------------------------ */

/* Address-0x04 NEC1 TV keymap (official Hisense-USA discrete IR values). */
static const ir_profile_t PROFILE_HISENSE_A = {
    .name = "Hisense-A (NEC 0x04)",
    .nec_address = 0x04,
    /* Positional: IR_KEY_POWER, VOL_UP, VOL_DOWN, CH_UP, CH_DOWN, MUTE,
     *             MENU, EXIT, OK, UP, DOWN, LEFT, RIGHT,
     *             0, 1, 2, 3, 4, 5, 6, 7, 8, 9
     * Comment shows <cmd>/<~cmd> of frame 04 FB xx yy. */
    .cmds = {
        /* POWER=0x70/0x8F  VOL+=0xA6/0x59  VOL-=0xA5/0x5A  CH+=0xA8/0x57  CH-=0xA7/0x58 */
        0x70, 0xA6, 0xA5, 0xA8, 0xA7,
        /* MUTE: no official IR code                          MENU=0x8D/0x72  EXIT=0xA3/0x5C */
        0xFF, 0x8D, 0xA3,
        /* OK=0xA1/0x5E  UP=0x9C/0x63  DOWN=0x9D/0x62  LEFT=0x9E/0x61  RIGHT=0x9F/0x60 */
        0xA1, 0x9C, 0x9D, 0x9E, 0x9F,
        /* digits 0..9 = 0x90..0x99 */
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* Generic NEC 0x20: the shared NEC addr-0x20 ("0x20DF") family that public
 * NEC1 databases document as the LG/Vizio TV codeset:
 *   probonopd/lirc-remotes (lg/VF28.xml, lg/EC970W.xml, vizio/VUR8.xml),
 *   ZeroDroid IrRemoteDatabase.kt, OMOTE-Firmware device_lgTV.cpp.
 * This is NOT a Hisense table (no citable Hisense addr-0x20 key map exists);
 * it fills only high-confidence keys consistent across those sources.
 * On-wire frames are 20 DF <cmd> <~cmd>:
 *   power 0x10 (20DF10EF)  vol+ 0x40 (20DF40BF)  vol- 0xC0 (20DFC03F)
 *   mute  0x90 (20DF906F)  ch+  0x00 (20DF00FF)  ch-  0x80 (20DF807F)
 *   menu  0xC2 (20DFC23D)
 * Arrows/OK/exit/digits have no unambiguous, citable per-key semantics for
 * this family in the sources above, so they stay 0xFF (never fabricated).
 * NOTE: exactly 23 positional initializers (IR_KEY_COUNT). */
static const ir_profile_t PROFILE_GENERIC_NEC_0X20 = {
    .name = "Generic NEC 0x20 (LG/Vizio)",
    .nec_address = 0x20,
    .cmds = {
        /* POWER=0x10  VOL+=0x40  VOL-=0xC0  CH+=0x00  CH-=0x80 */
        0x10, 0x40, 0xC0, 0x00, 0x80,
        /* MUTE=0x90  MENU=0xC2  EXIT: undefined */
        0x90, 0xC2, 0xFF,
        /* OK/UP/DOWN/LEFT/RIGHT: undefined (no citable per-key semantics) */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        /* digits 0-9: undefined */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* Hisense-C: the REAL Hisense NEC-extended (NECx) family, captured live from
 * an 85A6NTUK by WillEastbury/PICO_UNIVERSAL_REMOTE
 * (server/hisense_codes.py). Extended address 0xBF00, NECx 38 kHz.
 * On-wire frame: 00 BF <cmd> <~cmd>.
 * Positional: IR_KEY_POWER, VOL_UP, VOL_DOWN, CH_UP, CH_DOWN, MUTE,
 *             MENU, EXIT, OK, UP, DOWN, LEFT, RIGHT,
 *             0, 1, 2, 3, 4, 5, 6, 7, 8, 9
 * Comment shows <cmd>/<~cmd> of frame 00 BF xx yy. */
static const ir_profile_t PROFILE_HISENSE_C = {
    .name = "Hisense NECx 0xBF00",
    .nec_address = 0xBF00,
    .cmds = {
        /* POWER=0x0D/0xF2  VOL+=0x44/0xBB  VOL-=0x43/0xBC  CH+=0x4A/0xB5  CH-=0x4B/0xB4 */
        0x0D, 0x44, 0x43, 0x4A, 0x4B,
        /* MUTE=0x0E/0xF1  MENU=0x14/0xEB  EXIT=0x5C/0xA3 */
        0x0E, 0x14, 0x5C,
        /* OK=0x15/0xEA  UP=0x16/0xE9  DOWN=0x17/0xE8  LEFT=0x19/0xE6  RIGHT=0x18/0xE7 */
        0x15, 0x16, 0x17, 0x19, 0x18,
        /* digits 0..9 = 0x00..0x09 (~cmd 0xFF..0xF6) */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* Generic NEC TV: addr 0x04, shares the verified official NEC1 0x04 keymap. */
static const ir_profile_t PROFILE_GENERIC_NEC_TV = {
    .name = "Generic NEC TV (addr 0x04)",
    .nec_address = 0x04,
    .cmds = {
        0x70, 0xA6, 0xA5, 0xA8, 0xA7,
        0xFF, 0x8D, 0xA3,
        0xA1, 0x9C, 0x9D, 0x9E, 0x9F,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* ---- W9: real Hisense candidate codesets from the Xiaomi Mi Remote DB -------
 * Source: ysard/mi_remote_database v2.1 release ("database_dump"),
 *   https://github.com/ysard/mi_remote_database
 *   (production Xiaomi universal-remote code database, device=1 TV, brand=75
 *    Hisense/海信).  Decryption & layout per
 *   https://github.com/halifox/android_xiaomi_ir_lab
 * Pipeline: Base64 -> AES-256-ECB(no pad, key "fd7e...0788") -> strip 0x20 ->
 * GZIP -> int[] mark/space us.  Every key below is a byte-exact, ~cmd-verified
 * NEC frame decoded from the DB's own waveform — nothing fabricated.
 *
 * Six standard-header (9 ms NEC1/NECx) models + four short-header
 * (Samsung-style 4.5 ms leader, IR_NEC_SHORT_* 4500/4521) models.
 * On-wire bytes are LSB-first; NEC1 addr <= 0xFF emits <a> <~a> <c> <~c>,
 * NECx addr > 0xFF emits <a_lo> <a_hi> <c> <~c> (matches the decoder).
 * Frequencies in the DB are 37800-38460 Hz; the firmware uses a single
 * 38 kHz carrier, which is within receiver tolerance for all of them.
 *
 * Mi-DB key-reuse is preserved verbatim (e.g. xm_1_4524 vol+/right both
 * 0x5B): these are the DB's own key mappings, not fabrication.  Keys absent
 * from a model's DB table stay 0xFF (undefined).
 * ---------------------------------------------------------------------------- */

/* xm_1_4438: NEC1 addr 0x04 (frame 04 FB), freq 38340 Hz.
 * NOTE: shares PROFILE_HISENSE_A's address 0x04 but a different keymap. */
static const ir_profile_t PROFILE_MIDB_HISENSE_4438 = {
    .name = "MiDB Hisense 4438 (NEC 0x04)",
    .nec_address = 0x04,
    .cmds = {
        0x08, 0x02, 0x03, 0x00, 0x01, 0x09,
        0x43, 0x4D, 0x43, 0x56, 0x57, 0x58, 0x59,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_4524: NECx addr 0xBF00 (frame 00 BF), freq 38460 Hz.
 * DB reuses commands: vol+/right=0x5B, vol-/left=0x1E, ch+/up=0x14,
 * ch-/down=0x0F, menu/exit/ok=0x5E.  Shares PROFILE_HISENSE_C's 0xBF00
 * address with a different keymap. */
static const ir_profile_t PROFILE_MIDB_HISENSE_4524 = {
    .name = "MiDB Hisense 4524 (NECx 0xBF00)",
    .nec_address = 0xBF00,
    .cmds = {
        0x10, 0x5B, 0x1E, 0x14, 0x0F, 0x0B,
        0x5E, 0x5E, 0x5E, 0x14, 0x0F, 0x1E, 0x5B,
        0x0E, 0x0D, 0x19, 0x1B, 0x11, 0x15, 0x17, 0x12, 0x16, 0x03,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_3674: NEC1 addr 0x40 (frame 40 BF), freq 38150 Hz. */
static const ir_profile_t PROFILE_MIDB_HISENSE_3674 = {
    .name = "MiDB Hisense 3674 (NEC 0x40)",
    .nec_address = 0x40,
    .cmds = {
        0x12, 0x1A, 0x1E, 0x1B, 0x1F, 0x10,
        0x5B, 0x3C, 0x20, 0x18, 0x1C, 0x1D, 0x19,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_120: NEC1 addr 0x50 (frame 50 AF), freq 37910 Hz.
 * DB reuses menu/ok=0x53. */
static const ir_profile_t PROFILE_MIDB_HISENSE_120 = {
    .name = "MiDB Hisense 120 (NEC 0x50)",
    .nec_address = 0x50,
    .cmds = {
        0x17, 0x12, 0x15, 0x19, 0x18, 0x0B,
        0x53, 0x54, 0x53, 0x70, 0x71, 0x73, 0x72,
        0x0C, 0x0D, 0x0E, 0x0F, 0x1C, 0x1D, 0x1E, 0x1F, 0x04, 0x05,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_3907: NEC1 addr 0x18 (frame 18 E7), freq 37800 Hz.
 * DB reuses: ch+/up=0x00, ch-/down=0x01, vol-/left=0x03, vol+/right=0x02,
 * exit/ok=0x06. */
static const ir_profile_t PROFILE_MIDB_HISENSE_3907 = {
    .name = "MiDB Hisense 3907 (NEC 0x18)",
    .nec_address = 0x18,
    .cmds = {
        0x08, 0x02, 0x03, 0x00, 0x01, 0x09,
        0x04, 0x06, 0x06, 0x00, 0x01, 0x03, 0x02,
        0x19, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_3104: NEC1 addr 0x38 (frame 38 C7), freq 38000 Hz.
 * DB reuses: menu/exit/ok=0x0A, ch+/up=0x0C, ch-/down=0x0D, vol+/left=0x16,
 * vol-/right=0x17. */
static const ir_profile_t PROFILE_MIDB_HISENSE_3104 = {
    .name = "MiDB Hisense 3104 (NEC 0x38)",
    .nec_address = 0x38,
    .cmds = {
        0x1C, 0x16, 0x17, 0x0C, 0x0D, 0x15,
        0x0A, 0x0A, 0x0A, 0x0C, 0x0D, 0x16, 0x17,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    },
    .leader_mark_us  = 0,   /* standard NEC 9000/4500 (no override) */
    .leader_space_us = 0,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* ---- Short-header (Samsung-style 4.5 ms) family ----------------------------
 * NECx addresses, leader 4500/4521 (IR_NEC_SHORT_*), data bits identical to
 * NEC (562/1690).  The most common family in the Mi DB Hisense tree (6 of 14
 * remotes).  EXIT/MUTE absent from some models' DB tables -> 0xFF. */

/* xm_1_124: NECx addr 0x0D0D, freq 37910 Hz, no EXIT key in DB. */
static const ir_profile_t PROFILE_MIDB_HISENSE_124_SHORT = {
    .name = "MiDB Hisense 124 (NECx 0x0D0D short)",
    .nec_address = 0x0D0D,
    .cmds = {
        0x0B, 0x12, 0x13, 0x10, 0x11, 0x14,
        0x1E, 0xFF, 0x1E, 0x18, 0x19, 0x1A, 0x1B,
        0x09, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    },
    .leader_mark_us  = IR_NEC_SHORT_LEADER_MARK_US,
    .leader_space_us = IR_NEC_SHORT_LEADER_SPACE_US,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_109: NECx addr 0x0D0D, freq 37910 Hz, POWER=0x14, no MUTE/EXIT in DB. */
static const ir_profile_t PROFILE_MIDB_HISENSE_109_SHORT = {
    .name = "MiDB Hisense 109 (NECx 0x0D0D short)",
    .nec_address = 0x0D0D,
    .cmds = {
        0x14, 0x12, 0x13, 0x10, 0x11, 0xFF,
        0x1E, 0xFF, 0x1E, 0x10, 0x11, 0x13, 0x12,
        0x09, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    },
    .leader_mark_us  = IR_NEC_SHORT_LEADER_MARK_US,
    .leader_space_us = IR_NEC_SHORT_LEADER_SPACE_US,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_99: NECx addr 0x0909, freq 37910 Hz, no EXIT key in DB. */
static const ir_profile_t PROFILE_MIDB_HISENSE_99_SHORT = {
    .name = "MiDB Hisense 99 (NECx 0x0909 short)",
    .nec_address = 0x0909,
    .cmds = {
        0x0B, 0x12, 0x13, 0x10, 0x11, 0x14,
        0x19, 0xFF, 0x19, 0x10, 0x11, 0x13, 0x12,
        0x09, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    },
    .leader_mark_us  = IR_NEC_SHORT_LEADER_MARK_US,
    .leader_space_us = IR_NEC_SHORT_LEADER_SPACE_US,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* xm_1_102: NECx addr 0x0E0E, freq 37910 Hz. */
static const ir_profile_t PROFILE_MIDB_HISENSE_102_SHORT = {
    .name = "MiDB Hisense 102 (NECx 0x0E0E short)",
    .nec_address = 0x0E0E,
    .cmds = {
        0x0B, 0x12, 0x13, 0x10, 0x11, 0x19,
        0x1C, 0x1E, 0x1C, 0x10, 0x11, 0x13, 0x12,
        0x09, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    },
    .leader_mark_us  = IR_NEC_SHORT_LEADER_MARK_US,
    .leader_space_us = IR_NEC_SHORT_LEADER_SPACE_US,
    .raw_frames = NULL,     /* NEC-only profile: no raw waveform */
};

/* ---- W13 raw-frame profiles (tail-appended at PROFILES indices 14-17) ----
 * The 4 untested Hisense keysets from the Xiaomi Mi Remote DB whose frames are
 * NOT 32-bit NEC: xm_1_3359/3513/4070 are 42-bit NEC-timing with a fixed
 * 3-byte header; xm_1_110 is an 833/881 us short-leader family.  Their exact
 * waveforms live in ir_raw_db.cc (generated).  Every defined key i has
 * cmds[i] = IR_RAW_CMD_MARKER so the wizard/send path knows it is defined;
 * the actual timing is p->raw_frames[i].  Keys missing from the DB (only
 * 3513 EXIT/OK) stay 0xFF with {0, NULL} in raw_frames.  nec_address/leader
 * are unused (0) — raw frames carry their own timing. */

#define MIDB_RAW_CMDS_3359  { 0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE, \
                              0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE }
#define MIDB_RAW_CMDS_3513  { 0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFF,0xFF,0xFE,0xFE,0xFE,0xFE, \
                              0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE }
#define MIDB_RAW_CMDS_4070  { 0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE, \
                              0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE }
#define MIDB_RAW_CMDS_110   { 0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE, \
                              0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE }

static const ir_profile_t PROFILE_MIDB_RAW_3359 = {
    .name = "MiDB Hisense 3359 (42-bit 19C1DC)",
    .nec_address = 0,                       /* unused: raw-frame profile */
    .cmds = MIDB_RAW_CMDS_3359,
    .leader_mark_us  = 0,                   /* raw frames carry own timing */
    .leader_space_us = 0,
    .raw_frames = g_midb_3359_frames,
};

static const ir_profile_t PROFILE_MIDB_RAW_3513 = {
    .name = "MiDB Hisense 3513 (42-bit 1C61DC)",
    .nec_address = 0,
    .cmds = MIDB_RAW_CMDS_3513,             /* EXIT/OK undefined in DB */
    .leader_mark_us  = 0,                   /* raw frames carry own timing */
    .leader_space_us = 0,
    .raw_frames = g_midb_3513_frames,
};

static const ir_profile_t PROFILE_MIDB_RAW_4070 = {
    .name = "MiDB Hisense 4070 (42-bit 1D41DC)",
    .nec_address = 0,
    .cmds = MIDB_RAW_CMDS_4070,
    .leader_mark_us  = 0,                   /* raw frames carry own timing */
    .leader_space_us = 0,
    .raw_frames = g_midb_4070_frames,
};

static const ir_profile_t PROFILE_MIDB_RAW_110 = {
    .name = "MiDB Hisense 110 (short-leader)",
    .nec_address = 0,
    .cmds = MIDB_RAW_CMDS_110,
    .leader_mark_us  = 0,                   /* raw frames carry own timing */
    .leader_space_us = 0,
    .raw_frames = g_midb_110_frames,
};

static const ir_profile_t *const PROFILES[] = {
    &PROFILE_HISENSE_A,
    &PROFILE_GENERIC_NEC_0X20,
    &PROFILE_HISENSE_C,
    &PROFILE_GENERIC_NEC_TV,
    &PROFILE_MIDB_HISENSE_4438,
    &PROFILE_MIDB_HISENSE_4524,
    &PROFILE_MIDB_HISENSE_3674,
    &PROFILE_MIDB_HISENSE_120,
    &PROFILE_MIDB_HISENSE_3907,
    &PROFILE_MIDB_HISENSE_3104,
    &PROFILE_MIDB_HISENSE_124_SHORT,
    &PROFILE_MIDB_HISENSE_109_SHORT,
    &PROFILE_MIDB_HISENSE_99_SHORT,
    &PROFILE_MIDB_HISENSE_102_SHORT,
    /* W13 raw-frame profiles (indices 14-17, tail-appended). These keysets'
     * frames are non-32-bit (42-bit fixed-header NEC-timing; 833/881 us
     * short-leader for 110), so they cannot go through the NEC encoder.
     * cmds[i] = IR_RAW_CMD_MARKER (0xFE) marks a defined key whose waveform
     * lives in raw_frames[i]; it is NEVER emitted as a NEC command byte.
     * ir_db_find_key() rejects 0xFE, so the NEC path stays safe; every send
     * path checks ir_profile_raw_frame() first. */
    &PROFILE_MIDB_RAW_3359,
    &PROFILE_MIDB_RAW_3513,
    &PROFILE_MIDB_RAW_4070,
    &PROFILE_MIDB_RAW_110,
};

const ir_profile_t *ir_db_get_profile(uint8_t profile_id)
{
    if (profile_id >= sizeof(PROFILES) / sizeof(PROFILES[0])) {
        return NULL;
    }
    return PROFILES[profile_id];
}

uint8_t ir_db_get_profile_count(void)
{
    return (uint8_t)(sizeof(PROFILES) / sizeof(PROFILES[0]));
}

const ir_profile_t *const *ir_db_profile_table(void)
{
    return PROFILES;
}

/* W13 wizard candidate order: the 4 raw-frame keysets first, then the
 * original NEC/NECx profiles.  Build lazily once from PROFILES[] so the two
 * arrays can never drift — ids[] always mirrors the PROFILES index of each
 * order slot, and PROFILES[] itself keeps the NVS profile-id semantics.
 * Sizes are compile-time fixed (IR_PROFILE_COUNT_MAX) and asserted below. */
static const ir_profile_t *s_wizard_order[IR_PROFILE_COUNT_MAX];
static uint8_t s_wizard_ids[IR_PROFILE_COUNT_MAX];
static uint8_t s_wizard_n = 0;

const ir_profile_t *const *ir_db_wizard_order(const uint8_t **ids)
{
    static_assert(IR_PROFILE_COUNT_MAX >=
                      (sizeof(PROFILES) / sizeof(PROFILES[0])),
                  "wizard order buffer must hold every profile");
    if (s_wizard_n == 0) {
        uint8_t n = (uint8_t)(sizeof(PROFILES) / sizeof(PROFILES[0]));
        uint8_t k = 0;
        for (uint8_t i = 0; i < n; i++) {   /* raw-frame keysets first */
            if (PROFILES[i]->raw_frames != NULL) {
                s_wizard_order[k] = PROFILES[i];
                s_wizard_ids[k] = i;
                k++;
            }
        }
        for (uint8_t i = 0; i < n; i++) {   /* then NEC/NECx in PROFILES order */
            if (PROFILES[i]->raw_frames == NULL) {
                s_wizard_order[k] = PROFILES[i];
                s_wizard_ids[k] = i;
                k++;
            }
        }
        s_wizard_n = k;
    }
    if (ids != NULL) {
        *ids = s_wizard_ids;
    }
    return s_wizard_order;
}

uint8_t ir_db_find_key(const ir_profile_t *p, ir_key_t key,
                       uint16_t *addr, uint8_t *cmd)
{
    if (p == NULL || addr == NULL || cmd == NULL || (unsigned)key >= IR_KEY_COUNT) {
        return 0;
    }
    uint8_t c = p->cmds[key];
    if (c == 0xFF) {
        return 0; /* undefined key */
    }
    if (c == IR_RAW_CMD_MARKER) {
        /* Raw-frame key: waveform in p->raw_frames[key], NOT a NEC command.
         * ir_db_find_key is the pure NEC resolver — 0xFE is never emitted */
        return 0;
    }
    *addr = p->nec_address;
    *cmd = c;
    return 1;
}
