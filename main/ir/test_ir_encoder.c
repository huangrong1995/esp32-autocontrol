/*
 * Host self-test for the pure NEC encoder (no RMT/hardware dependency).
 *
 * Compile with plain cc and run:
 *     cc -DIR_SELFTEST -I<repo>/main -o /tmp/ir_selftest \
 *        <repo>/main/ir/test_ir_encoder.c
 *     /tmp/ir_selftest
 *
 * Standard: prints IR_SELFTEST_PASS (and exits 0) on success, or
 * IR_SELFTEST_FAIL:<detail> (exit 1) on failure.
 *
 * It exercises ir_encode_nec() / ir_symbol_make() only — the pure symbol
 * builder that ir_send_raw_nec() feeds into the RMT copy encoder. The RMT
 * transmit path (channel init, carrier, rmt_transmit) is intentionally NOT
 * linked here; the brief scopes the host test to the pure encoder layer.
 * Test 4 additionally covers ir_encode_nec_var() with the short-header
 * (Samsung-style 4.5ms) leader used by the W9 Mi-DB candidate profiles.
 */
#ifdef IR_SELFTEST

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "ir_encoder.h"

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("IR_SELFTEST_FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    /* Known-true frame for ir_encode_nec(0x04, 0x1F).
     * On-wire bytes (LSB-first): 0x04 0xFB 0x1F 0xE0.
     * Each row is a (mark_us, space_us) symbol pair. */
    static const uint32_t EXPECTED[34][2] = {
        /* leader */
        { 9000, 4500 },
        /* addr 0x04 = b00000100, LSB-first: 0,0,1,0,0,0,0,0 */
        { 562, 562 }, { 562, 562 }, { 562, 1690 }, { 562, 562 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* ~addr 0xFB = b11111011, LSB-first: 1,1,0,1,1,1,1,1 */
        { 562, 1690 }, { 562, 1690 }, { 562, 562 }, { 562, 1690 },
        { 562, 1690 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        /* cmd 0x1F = b00011111, LSB-first: 1,1,1,1,1,0,0,0 */
        { 562, 1690 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        { 562, 1690 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* ~cmd 0xE0 = b11100000, LSB-first: 0,0,0,0,0,1,1,1 */
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        { 562, 562 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        /* ending: trailing 562us mark + idle */
        { 562, IR_NEC_TRAIL_IDLE_US },
    };

    /* Known-true NECx frame for ir_encode_nec(0xBF00, 0x0D).
     * On-wire bytes (LSB-first): 0x00 0xBF 0x0D 0xF2 (no complement on addr). */
    static const uint32_t EXPECTED2[34][2] = {
        /* leader */
        { 9000, 4500 },
        /* addr_low 0x00 = b00000000, LSB-first: 0,0,0,0,0,0,0,0 */
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* addr_high 0xBF = b10111111, LSB-first: 1,1,1,1,1,1,0,1 */
        { 562, 1690 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        { 562, 1690 }, { 562, 1690 }, { 562, 562 }, { 562, 1690 },
        /* cmd 0x0D = b00001101, LSB-first: 1,0,1,1,0,0,0,0 */
        { 562, 1690 }, { 562, 562 }, { 562, 1690 }, { 562, 1690 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* ~cmd 0xF2 = b11110010, LSB-first: 0,1,0,0,1,1,1,1 */
        { 562, 562 }, { 562, 1690 }, { 562, 562 }, { 562, 562 },
        { 562, 1690 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        /* ending: trailing 562us mark + idle */
        { 562, IR_NEC_TRAIL_IDLE_US },
    };

    /* Known-true NECx short-header (Samsung-style 4.5ms) frame for
     * ir_encode_nec_var(0x0D0D, 0x0B, 4500, 4521) — the xm_1_124 Hisense
     * candidate POWER frame (freq 37910 Hz).  On-wire bytes (LSB-first):
     * 0x0D 0x0D 0x0B 0xF4. */
    static const uint32_t EXPECTED3[34][2] = {
        /* short leader (4500/4521, IR_NEC_SHORT_*) */
        { IR_NEC_SHORT_LEADER_MARK_US, IR_NEC_SHORT_LEADER_SPACE_US },
        /* addr_low 0x0D = b00001101, LSB-first: 1,0,1,1,0,0,0,0 */
        { 562, 1690 }, { 562, 562 }, { 562, 1690 }, { 562, 1690 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* addr_high 0x0D = b00001101, LSB-first: 1,0,1,1,0,0,0,0 */
        { 562, 1690 }, { 562, 562 }, { 562, 1690 }, { 562, 1690 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* cmd 0x0B = b00001011, LSB-first: 1,1,0,1,0,0,0,0 */
        { 562, 1690 }, { 562, 1690 }, { 562, 562 }, { 562, 1690 },
        { 562, 562 }, { 562, 562 }, { 562, 562 }, { 562, 562 },
        /* ~cmd 0xF4 = b11110100, LSB-first: 0,0,1,0,1,1,1,1 */
        { 562, 562 }, { 562, 562 }, { 562, 1690 }, { 562, 562 },
        { 562, 1690 }, { 562, 1690 }, { 562, 1690 }, { 562, 1690 },
        /* ending: trailing 562us mark + idle */
        { 562, IR_NEC_TRAIL_IDLE_US },
    };

    ir_symbol_t syms[40];
    size_t n = ir_encode_nec(0x04, 0x1F, syms, sizeof(syms) / sizeof(syms[0]));

    /* Test 1: full symbol sequence equals the known-true frame. */
    ASSERT(n == 34, "expected exactly 34 symbols (leader + 32 bits + ending)");
    for (size_t i = 0; i < 34; i++) {
        ir_symbol_t want = ir_symbol_make(EXPECTED[i][0], EXPECTED[i][1]);
        if (syms[i] != want) {
            printf("IR_SELFTEST_FAIL: symbol %u = 0x%08X, want 0x%08X\n",
                   (unsigned)i, (unsigned)syms[i], (unsigned)want);
            return 1;
        }
    }

    /* Test 2: complement fields on the wire.
     * Rebuild the 32 data bits from the symbol spaces (LSB-first) and check
     * the four bytes are 0x04, ~0x04&0xFF, 0x1F, ~0x1F&0xFF. */
    uint32_t frame = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t space = (syms[1 + i] >> 16) & 0x7FFFu;
        if (space == IR_NEC_BIT1_SPACE_US) {
            frame |= (1u << i);
        }
    }
    ASSERT((frame & 0xFFu) == 0x04u, "byte0 (address) must be 0x04");
    ASSERT(((frame >> 8) & 0xFFu) == 0xFBu, "byte1 (~address) must be ~0x04 & 0xFF");
    ASSERT(((frame >> 16) & 0xFFu) == 0x1Fu, "byte2 (command) must be 0x1F");
    ASSERT(((frame >> 24) & 0xFFu) == 0xE0u, "byte3 (~command) must be ~0x1F & 0xFF");

    /* Test 3: NECx frame — 0xBF00 0x0D → 00 BF 0D F2, 34 symbols. */
    size_t n2 = ir_encode_nec(0xBF00, 0x0D, syms, sizeof(syms) / sizeof(syms[0]));
    ASSERT(n2 == 34, "NECx: expected exactly 34 symbols");
    for (size_t i = 0; i < 34; i++) {
        ir_symbol_t want = ir_symbol_make(EXPECTED2[i][0], EXPECTED2[i][1]);
        if (syms[i] != want) {
            printf("IR_SELFTEST_FAIL: NECx symbol %u = 0x%08X, want 0x%08X\n",
                   (unsigned)i, (unsigned)syms[i], (unsigned)want);
            return 1;
        }
    }

    /* Test 4: NECx short-header (Samsung-style 4.5ms) frame via
     * ir_encode_nec_var — 0x0D0D 0x0B with 4500/4521 leader → 34 symbols,
     * leader exactly the short values, data bytes 0D 0D 0B F4. */
    size_t n3 = ir_encode_nec_var(0x0D0D, 0x0B,
                                  IR_NEC_SHORT_LEADER_MARK_US,
                                  IR_NEC_SHORT_LEADER_SPACE_US,
                                  syms, sizeof(syms) / sizeof(syms[0]));
    ASSERT(n3 == 34, "short-header: expected exactly 34 symbols");
    ASSERT((syms[0] == (ir_symbol_t)ir_symbol_make(
            IR_NEC_SHORT_LEADER_MARK_US, IR_NEC_SHORT_LEADER_SPACE_US)),
           "short-header: symbol 0 must be the 4.5ms leader");
    for (size_t i = 0; i < 34; i++) {
        ir_symbol_t want = ir_symbol_make(EXPECTED3[i][0], EXPECTED3[i][1]);
        if (syms[i] != want) {
            printf("IR_SELFTEST_FAIL: short-header symbol %u = 0x%08X, want 0x%08X\n",
                   (unsigned)i, (unsigned)syms[i], (unsigned)want);
            return 1;
        }
    }
    /* Rebuild the 32 data bits (skipping the leader at syms[0]) and confirm
     * the four bytes are 0x0D, 0x0D, 0x0B, 0xF4. */
    frame = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t space = (syms[1 + i] >> 16) & 0x7FFFu;
        if (space == IR_NEC_BIT1_SPACE_US) {
            frame |= (1u << i);
        }
    }
    ASSERT((frame & 0xFFu) == 0x0Du, "short-header byte0 (addr_low) must be 0x0D");
    ASSERT(((frame >> 8) & 0xFFu) == 0x0Du, "short-header byte1 (addr_high) must be 0x0D");
    ASSERT(((frame >> 16) & 0xFFu) == 0x0Bu, "short-header byte2 (command) must be 0x0B");
    ASSERT(((frame >> 24) & 0xFFu) == 0xF4u, "short-header byte3 (~command) must be ~0x0B & 0xFF");

    printf("IR_SELFTEST_PASS\n");
    return 0;
}

#endif /* IR_SELFTEST */
