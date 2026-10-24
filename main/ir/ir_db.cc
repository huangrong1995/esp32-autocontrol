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
 * profile[1] ("Hisense-B") is the NEC addr-0x20 family. Its POWER command
 * 0x40 is verified (on-wire frame 20 DF 40 BF). Public NEC1 databases
 * document the 0x20DF family as the LG/Vizio TV codeset (probonopd/lirc-remotes,
 * ZeroDroid IrRemoteDatabase.kt, OMOTE-Firmware device_lgTV.cpp) and no
 * Hisense-branded NEC addr-0x20 key table is citable, so every other key
 * stays 0xFF rather than borrowing LG's assignments for a Hisense profile.
 *
 * profile[2] ("Hisense-C") stays an all-0xFF documented placeholder:
 * its nec_address records 0xBF00 — the REAL Hisense NEC-extended family
 * captured by WillEastbury/PICO_UNIVERSAL_REMOTE — but those frames need an
 * NEC-extended encoder (not the NEC1 encoder of this task), so every key
 * stays undefined here.
 *
 * profile[3] reuses the official NEC1 addr-0x04 keymap as a generic NEC TV
 * layout (the only fully verified NEC1 TV table available).
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
};

/* Hisense-B: NEC1 address 0x20 family. POWER command 0x40 is verified
 * (on-wire frame 20 DF 40 BF — address byte 0x20, complement 0xDF, power
 * command 0x40). Every other key stays 0xFF: public NEC1 databases
 * (probonopd/lirc-remotes, ZeroDroid IrRemoteDatabase.kt, OMOTE-Firmware
 * device_lgTV.cpp) document the 0x20DF family as the LG/Vizio TV codeset and
 * no Hisense-branded NEC addr-0x20 key table could be cited, so reusing LG's
 * button assignments for a Hisense profile would be fabrication.
 * NOTE: exactly 23 positional initializers (IR_KEY_COUNT). */
static const ir_profile_t PROFILE_HISENSE_B = {
    .name = "Hisense-B (NEC 0x20)",
    .nec_address = 0x20,
    .cmds = {
        /* POWER=0x40/0xBF (frame 20 DF 40 BF)               rest undefined */
        0x40, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
};

/* Hisense-C: "byte-swapped" family. The real Hisense NEC-extended variant
 * (captured by WillEastbury/PICO_UNIVERSAL_REMOTE, e.g. 85A6NTUK) uses the
 * 16-bit extended address 0xBF00 with, e.g., power=0x0D, mute=0x0E,
 * vol-=0x43, vol+=0x44, ch-=0x4B, ch+=0x4A, ok=0x15, up=0x16, down=0x17,
 * right=0x18, left=0x19, home=0x20, menu=0x14, back=0x48, exit=0x5C,
 * digits 0-9=0x00-0x09. Those frames require an NEC-extended encoder, which
 * this task's NEC1 encoder cannot emit, so all keys stay 0xFF here. */
static const ir_profile_t PROFILE_HISENSE_C = {
    .name = "Hisense-C (NEC swapped)",
    .nec_address = 0xBF00,
    .cmds = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    },
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
};

static const ir_profile_t *const PROFILES[] = {
    &PROFILE_HISENSE_A,
    &PROFILE_HISENSE_B,
    &PROFILE_HISENSE_C,
    &PROFILE_GENERIC_NEC_TV,
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
    *addr = p->nec_address;
    *cmd = c;
    return 1;
}
