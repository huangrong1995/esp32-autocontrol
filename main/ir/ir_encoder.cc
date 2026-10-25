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

void ir_send_raw_nec(uint16_t address, uint8_t command)
{
    rmt_tx_init();

    ir_symbol_t packed[IR_NEC_SYMBOLS];
    size_t n = ir_encode_nec(address, command, packed, IR_NEC_SYMBOLS);
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
    ESP_LOGI(TAG, "sent NEC addr=0x%02X cmd=0x%02X (%u symbols)", address, command, (unsigned)n);
}

void ir_send_key(ir_key_t key)
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
        return;
    }

    uint16_t addr = 0;
    uint8_t cmd = 0;
    if (!pair_resolve_key(calib_len ? calib_buf : NULL, calib_len, profile_id,
                          key, &addr, &cmd)) {     /* 未定义键:log skip;return; */
        const ir_profile_t *p = ir_db_get_profile(profile_id);
        ESP_LOGW(TAG, "profile '%s' key=%d undefined; skip",
                 p ? p->name : "?", (int)key);
        return;
    }

    ESP_LOGI(TAG, "key=%d -> profile_id=%u NEC addr=0x%02X cmd=0x%02X",
             (int)key, (unsigned)profile_id, addr, cmd);
    ir_send_raw_nec(addr, cmd);
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
