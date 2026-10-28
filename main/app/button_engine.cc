/*
 * One-key (physical) button engine implementation. Polls the configured GPIO
 * on a low-priority task, runs each sample through the pure debounce core
 * (app/button_core.h), and starts macro playback on a debounced press.
 *
 * Concurrency: the only shared state is the macro engine's one-key binding,
 * read through the atomic macro_get_onekey() and played through macro_play()
 * (which carries its own pairing + busy gates), so this task never holds any
 * engine lock while sending IR. A press landing mid-playback/recoording is
 * simply ignored (logged) — the page's 停止回放 remains the stop path, and the
 * playback task owns the PLAYING->IDLE transition.
 */

#include "app/button_engine.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/button_core.h"     /* pure debounce state machine */
#include "app/macro_types.h"     /* MACRO_ONEKEY_NONE sentinel */
#include "app/macro_engine.h"    /* macro_get_onekey, macro_play */

static const char *TAG = "onekey_btn";

#define BTN_GPIO       CONFIG_ONE_KEY_BTN_GPIO
#define BTN_POLL_MS    20
#define BTN_STACK_BYTES 2048
#define BTN_STACK_WORDS (BTN_STACK_BYTES / sizeof(StackType_t))

static void button_task(void *arg)
{
    (void)arg;
    btn_debounce_t db;
    btn_debounce_init(&db, (uint8_t)gpio_get_level((gpio_num_t)BTN_GPIO));

    for (;;) {
        uint8_t level = (uint8_t)gpio_get_level((gpio_num_t)BTN_GPIO);
        if (btn_debounce_sample(&db, level) == 1u) {
            uint8_t one_key = macro_get_onekey();
            if (one_key == MACRO_ONEKEY_NONE) {
                ESP_LOGI(TAG, "button press: no one-key macro bound");
                continue;
            }
            ESP_LOGI(TAG, "button press: playing one-key macro id=%u",
                     (unsigned)one_key);
            esp_err_t err = macro_play(one_key);
            switch (err) {
            case ESP_OK:
                break;
            case ESP_ERR_NOT_FOUND:
                ESP_LOGW(TAG, "one-key macro id=%u gone (deleted); binding is stale",
                         (unsigned)one_key);
                break;
            case ESP_ERR_INVALID_STATE:
                /* macro_play reports INVALID_STATE for both "unpaired" and
                 * "engine busy"; key off the live engine state for the log. */
                if (macro_is_playing() || macro_is_recording()) {
                    ESP_LOGI(TAG, "button press ignored: playback/recording in progress");
                } else {
                    ESP_LOGW(TAG, "button press ignored: not paired");
                }
                break;
            default:
                ESP_LOGW(TAG, "button press play failed: %s", esp_err_to_name(err));
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

void button_engine_init(void)
{
#if (CONFIG_ONE_KEY_BTN_GPIO == CONFIG_IR_TX_GPIO)
    ESP_LOGE(TAG, "button disabled: ONE_KEY_BTN_GPIO == IR_TX_GPIO == %d",
             BTN_GPIO);
    return;
#else
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed (GPIO%d): %s", BTN_GPIO,
                 esp_err_to_name(err));
        return;
    }
    BaseType_t ok = xTaskCreate(button_task, "onekey_btn", BTN_STACK_WORDS,
                                NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "button task create failed");
        return;
    }
    ESP_LOGI(TAG, "one-key button init: GPIO%d active-low", BTN_GPIO);
#endif
}