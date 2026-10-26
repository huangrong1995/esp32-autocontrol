#include "ir/ir_encoder.h"
#include "ir/ir_db.h"
#include "app/remote_state.h"
#include "app/pair_state.h"

#include <stdio.h>
#include <stddef.h>
#include "esp_log.h"
#include "driver/rmt_tx.h"

static const char *TAG = "ir_encoder";

#define IR_RMT_RESOLUTION_HZ 1000000u /* 1 tick = 1 us */
#define IR_NEC_SYMBOLS        34u     /* leader + 32 data bits + ending */
#define IR_RMT_MAX_SYMBOLS    64u     /* mem_block_symbols (also raw-frame cap) */
#define IR_RMT_TIMEOUT_MS     200u

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;

/* RMT TX channel: create once, then apply the 38 kHz carrier and enable. */
static void rmt_tx_init(void)
{
    if (s_tx_chan != NULL) {
        return;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (gpio_num_t)CONFIG_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64, /* enough for a 34-symbol NEC frame */
        .trans_queue_depth = 4,
        .intr_priority = 0,      /* driver picks a low interrupt priority */
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .allow_pd = false,
            .init_level = 0,
        },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_tx_chan));

    /* IDF 6.0: carrier is applied separately via rmt_apply_carrier()
     * (rmt_transmit_config_t has no carrier field anymore).
     * duty_cycle is taken as a 0.0~1.0 factor by the driver
     * (high_ticks = total_ticks * duty_cycle), so 33% is 0.33f. */
    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = CONFIG_IR_CARRIER_HZ,
        .duty_cycle = 0.33f,
        .flags = {
            .polarity_active_low = false,
            .always_on = false,
        },
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(s_tx_chan, &carrier_cfg));

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_cfg, &s_copy_enc));

    ESP_ERROR_CHECK(rmt_enable(s_tx_chan));
    ESP_LOGI(TAG, "RMT TX ready: gpio=%d carrier=%dHz", CONFIG_IR_TX_GPIO, CONFIG_IR_CARRIER_HZ);
}

void ir_send_raw_nec_var(uint16_t address, uint8_t command,
                         uint16_t leader_mark_us, uint16_t leader_space_us)
{
    rmt_tx_init();

    ir_symbol_t packed[IR_NEC_SYMBOLS];
    size_t n = ir_encode_nec_var(address, command, leader_mark_us, leader_space_us,
                                 packed, IR_NEC_SYMBOLS);
    if (n == 0) {
        ESP_LOGE(TAG, "NEC encode failed (addr=0x%02X cmd=0x%02X)", address, command);
        return;
    }

    rmt_symbol_word_t words[IR_NEC_SYMBOLS];
    for (size_t i = 0; i < n; i++) {
        words[i].val = packed[i];
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0, /* one frame per press, no NEC repeat code */
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };
    ESP_ERROR_CHECK(rmt_transmit(s_tx_chan, s_copy_enc, words, sizeof(rmt_symbol_word_t) * n, &tx_cfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_tx_chan, IR_RMT_TIMEOUT_MS));
    ESP_LOGI(TAG, "sent NEC addr=0x%02X cmd=0x%02X (%u symbols, leader %u/%u)",
             address, command, (unsigned)n,
             (unsigned)leader_mark_us, (unsigned)leader_space_us);
}

void ir_send_raw_nec(uint16_t address, uint8_t command)
{
    ir_send_raw_nec_var(address, command,
                        IR_NEC_LEADER_MARK_US, IR_NEC_LEADER_SPACE_US);
}

void ir_send_raw_us(const uint16_t *frame, size_t n_pairs)
{
    if (frame == NULL || n_pairs == 0) {
        ESP_LOGW(TAG, "raw send: empty frame (n_pairs=%u)", (unsigned)n_pairs);
        return;
    }
    if (n_pairs > IR_RMT_MAX_SYMBOLS) {
        ESP_LOGW(TAG, "raw send: %u pairs exceeds %u-symbol RMT block; truncating",
                 (unsigned)n_pairs, IR_RMT_MAX_SYMBOLS);
        n_pairs = IR_RMT_MAX_SYMBOLS;
    }

    rmt_tx_init();

    rmt_symbol_word_t words[IR_RMT_MAX_SYMBOLS];
    for (size_t i = 0; i < n_pairs; i++) {
        /* frame is [mark,space,...,mark,space]; the last space is the
         * trailing idle, clamped to 0x7FFF by ir_symbol_make(). */
        words[i].val = ir_symbol_make(frame[2 * i], frame[2 * i + 1]);
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0, /* one frame per press */
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };
    ESP_ERROR_CHECK(rmt_transmit(s_tx_chan, s_copy_enc, words,
                                 sizeof(rmt_symbol_word_t) * n_pairs, &tx_cfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_tx_chan, IR_RMT_TIMEOUT_MS));
    ESP_LOGI(TAG, "sent raw frame of %u (mark,space) pairs", (unsigned)n_pairs);
}

bool ir_send_key(ir_key_t key)
{
    uint8_t calib_buf[IR_KEY_COUNT];
    size_t calib_len = 0;
    uint8_t profile_id;
    if (pair_calib_active()) {                 /* 校准档存在 → 来源组提供 NEC 地址 */
        calib_len = IR_KEY_COUNT;
        pair_calib_read_nvs(calib_buf);
        profile_id = pair_calib_group();
    } else {                                   /* legacy → 原 profile_id 语义不变 */
        profile_id = remote_state_get_profile();
    }
    if (profile_id >= ir_db_get_profile_count()) { /* 同现状:log skip;return; */
        ESP_LOGW(TAG, "no profile paired (id=0x%02X); skip key=%d", profile_id, (int)key);
        return false;
    }

    uint16_t addr = 0;
    uint8_t cmd = 0;
    const ir_profile_t *p = ir_db_get_profile(profile_id);

    /* W13: raw-frame keysets send the stored waveform directly (their frames
     * are not 32-bit NEC — cmds[] only holds a definedness marker).  Intercept
     * BEFORE pair_resolve_key, which is the pure NEC resolver.  Definedness
     * still follows the calib/legacy rule: a 0xFF slot (calib no-reaction, or
     * cmds 0xFF) is never sent, exactly like the NEC path. */
    const ir_raw_frame_t *rf = ir_profile_raw_frame(p, key);
    if (rf != NULL) {
        if (calib_len == IR_KEY_COUNT && calib_buf[key] == 0xFF) {
            ESP_LOGW(TAG, "profile '%s' key=%d calib-undefined; skip",
                     p ? p->name : "?", (int)key);
            return false;
        }
        ESP_LOGI(TAG, "key=%d -> profile_id=%u raw frame (%u pairs)",
                 (int)key, (unsigned)profile_id, (unsigned)rf->n_pairs);
        ir_send_raw_us(rf->frame, rf->n_pairs);
        return true;
    }

    if (!pair_resolve_key(calib_len ? calib_buf : NULL, calib_len, profile_id,
                          key, &addr, &cmd)) {     /* 未定义键:log skip;return; */
        ESP_LOGW(TAG, "profile '%s' key=%d undefined; skip",
                 p ? p->name : "?", (int)key);
        return false;
    }

    uint16_t lm, ls;
    ir_profile_leader(p, &lm, &ls);   /* calib 档沿用同 profile 的 leader */

    ESP_LOGI(TAG, "key=%d -> profile_id=%u NEC addr=0x%02X cmd=0x%02X",
             (int)key, (unsigned)profile_id, addr, cmd);
    ir_send_raw_nec_var(addr, cmd, lm, ls);
    return true;
}

void ir_set_profile(uint8_t profile_id)
{
    if (profile_id >= ir_db_get_profile_count()) {
        ESP_LOGW(TAG, "invalid profile id=%u", profile_id);
        return;
    }
    remote_state_set_profile(profile_id);
    const ir_profile_t *p = ir_db_get_profile(profile_id);
    ESP_LOGI(TAG, "send profile set to %u ('%s')", profile_id, p ? p->name : "?");
}
