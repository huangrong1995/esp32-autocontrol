#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Task 1 enum — keep unchanged. Order must stay stable: ir_db.cc fills
 * cmds[] positionally in this order. */
typedef enum {
    IR_KEY_POWER, IR_KEY_VOL_UP, IR_KEY_VOL_DOWN, IR_KEY_CH_UP, IR_KEY_CH_DOWN,
    IR_KEY_MUTE, IR_KEY_MENU, IR_KEY_EXIT, IR_KEY_OK,
    IR_KEY_UP, IR_KEY_DOWN, IR_KEY_LEFT, IR_KEY_RIGHT,
    IR_KEY_0, IR_KEY_1, IR_KEY_2, IR_KEY_3, IR_KEY_4,
    IR_KEY_5, IR_KEY_6, IR_KEY_7, IR_KEY_8, IR_KEY_9,
    IR_KEY_COUNT
} ir_key_t;

/* Packed RMT symbol word — bit-compatible with esp-idf `rmt_symbol_word_t`
 * (components/esp_hal_rmt/include/hal/rmt_types.h):
 *   bits 14..0   -> duration0 (ticks @ 1 MHz RMT resolution => microseconds)
 *   bit  15      -> level0
 *   bits 30..16  -> duration1
 *   bit  31      -> level1
 * A pure `uint32_t` alias is used here so ir_encode_nec() stays host-compilable
 * without IDF headers — this is what the IR_SELFTEST unit test compiles. */
typedef uint32_t ir_symbol_t;

/* NEC1 timing, classic rounded values (1 tick = 1 us @ 1 MHz resolution). */
#define IR_NEC_LEADER_MARK_US   9000u
#define IR_NEC_LEADER_SPACE_US  4500u
#define IR_NEC_BIT_MARK_US       562u
#define IR_NEC_BIT0_SPACE_US     562u
#define IR_NEC_BIT1_SPACE_US    1690u
#define IR_NEC_TRAIL_MARK_US     562u
#define IR_NEC_TRAIL_IDLE_US   0x7FFFu  /* ending symbol idle (max representable) */

/* Short-header (Samsung-style 4.5ms) leader, from the Xiaomi Mi Remote DB
 * Hisense short-header family (xm_1_99/102/109/124/125/4369): measured
 * 4498-4505 mark / 4515-4522 space.  Same 562/1690 data bits as NEC. */
#define IR_NEC_SHORT_LEADER_MARK_US   4500u
#define IR_NEC_SHORT_LEADER_SPACE_US  4521u

/* Pack one (mark,space) pair into an ir_symbol_t word. */
static inline ir_symbol_t ir_symbol_make(uint32_t mark_us, uint32_t idle_us)
{
    return (ir_symbol_t)((mark_us & 0x7FFFu) | (1u << 15) | ((idle_us & 0x7FFFu) << 16));
}

/* Encode one full NEC frame into `out` (capacity `cap` symbols).
 *
 * On-wire frame (LSB-first, 32 bits in this order):
 *   address(8)  ~address(8)  command(8)  ~command(8)
 *   leader: 9 ms mark / 4.5 ms space
 *   bit "0": 562us mark / 562us space    bit "1": 562us mark / 1690us space
 *   trailing: 562us mark + idle space (stop bit)
 * When `address > 0xFF`, encodes as NEC-extended (NECx): the frame is
 *   addr_low(8)  addr_high(8)  command(8)  ~command(8)
 *   i.e. byte1 is the address HIGH byte, not the bitwise complement
 *   (PICO-universal remote convention).
 * `ir_encode_nec_var` takes an explicit leader (mark/space) so short-header
 * (Samsung-style 4.5ms) profiles can be encoded; `ir_encode_nec` is the
 * standard 9 ms wrapper.  Returns the number of symbols written (0 if `cap`
 * too small / NULL).  Pure function — no hardware access, host-testable. */
static inline size_t ir_encode_nec_var(uint16_t address, uint8_t command,
                                       uint16_t leader_mark_us,
                                       uint16_t leader_space_us,
                                       ir_symbol_t *out, size_t cap)
{
    const size_t need = 1 + 32 + 1; /* leader + 32 data bits + ending */
    if (out == NULL || cap < need) {
        return 0;
    }

    size_t n = 0;
    out[n++] = ir_symbol_make(leader_mark_us, leader_space_us);

    /* 32-bit stream read LSB-first.  NEC1: addr(0..7) ~addr(8..15) cmd(16..23) ~cmd(24..31).
     * NECx (address > 0xFF): addr_low(0..7) addr_high(8..15) cmd(16..23) ~cmd(24..31) —
     * byte1 is the address HIGH byte, not the bitwise complement (PICO-universal convention). */
    uint32_t frame;
    if (address > 0xFFu) {
        frame = (uint32_t)(address & 0xFFu)
              | ((uint32_t)((address >> 8) & 0xFFu) << 8)
              | ((uint32_t)command << 16)
              | ((uint32_t)(~command & 0xFFu) << 24);
    } else {
        frame = (uint32_t)(address & 0xFFu)
              | ((uint32_t)(~address & 0xFFu) << 8)
              | ((uint32_t)command << 16)
              | ((uint32_t)(~command & 0xFFu) << 24);
    }

    for (int i = 0; i < 32; i++) {
        uint32_t bit = (frame >> i) & 1u;
        uint32_t space = bit ? IR_NEC_BIT1_SPACE_US : IR_NEC_BIT0_SPACE_US;
        out[n++] = ir_symbol_make(IR_NEC_BIT_MARK_US, space);
    }

    out[n++] = ir_symbol_make(IR_NEC_TRAIL_MARK_US, IR_NEC_TRAIL_IDLE_US);
    return n;
}

/* Standard 9 ms leader NEC wrapper (all pre-W9 profiles/callers). */
static inline size_t ir_encode_nec(uint16_t address, uint8_t command,
                                   ir_symbol_t *out, size_t cap)
{
    return ir_encode_nec_var(address, command,
                             IR_NEC_LEADER_MARK_US, IR_NEC_LEADER_SPACE_US,
                             out, cap);
}

#ifdef __cplusplus
extern "C" {
#endif

/* Select the profile id used for ir_send_key() (delegates to remote_state). */
void ir_set_profile(uint8_t profile_id);

/* Send a logical key via the currently paired source (calibration blob if
 * active, else the paired profile). Returns true iff an NEC frame was
 * actually transmitted; never sends 0xFF/undefined keys. */
bool ir_send_key(ir_key_t key);

/* Send a raw NEC frame (address + command) directly. Debug/raw entry point;
 * also the real transmitter behind ir_send_key(). Standard 9 ms leader. */
void ir_send_raw_nec(uint16_t address, uint8_t command);

/* Send a raw NEC frame with an explicit leader (mark/space us) — used by
 * short-header (Samsung-style 4.5ms) profiles. */
void ir_send_raw_nec_var(uint16_t address, uint8_t command,
                         uint16_t leader_mark_us, uint16_t leader_space_us);

/* Send a raw mark/space timing frame directly (non-NEC protocols that the
 * 32-bit encoder cannot express — e.g. the 42-bit fixed-header Hisense
 * family and the 833/881 us short-leader keyst).  `frame` is interleaved
 * [mark,space,...,mark,space] in microseconds; the LAST space is the
 * trailing idle and is clamped to 0x7FFF (RMT 15-bit duration) by
 * ir_symbol_make().  Each (mark,space) pair becomes one RMT symbol.
 * Calls rmt_tx_init() on first use, like the NEC path. */
void ir_send_raw_us(const uint16_t *frame, size_t n_pairs);

#ifdef __cplusplus
}
#endif
