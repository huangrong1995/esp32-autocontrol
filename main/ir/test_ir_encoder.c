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

    printf("IR_SELFTEST_PASS\n");
    return 0;
}

#endif /* IR_SELFTEST */