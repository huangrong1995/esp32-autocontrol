#include "app/wifi_ap.h"

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_ap";

static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != WIFI_EVENT) {
        return;
    }
    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station connected");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station disconnected");
        break;
    default:
        break;
    }
}

void wifi_ap_start(void)
{
    /* NVS init happens exactly once, here — remote_state_* and anything else
     * using NVS assume it is ready after wifi_ap_start() returns. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap(); /* default SoftAP IP: 192.168.4.1 */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_ap_event_handler,
                                                        NULL, NULL));

    /* {} value-initialises the whole union (zeroes the first member, .ap);
     * field-by-field assignment below avoids C++ designated-initializer
     * ordering + missing-field warnings for the many SoftAP fields we leave
     * at their defaults. */
    wifi_config_t wifi_config = {};
    wifi_config.ap.ssid_len = (uint8_t)strlen(CONFIG_AP_SSID);
    memcpy(wifi_config.ap.ssid, CONFIG_AP_SSID, wifi_config.ap.ssid_len);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN; /* default; PSK when password set */
    if (strlen(CONFIG_AP_PASSWORD) > 0) {
        memcpy(wifi_config.ap.password, CONFIG_AP_PASSWORD,
               strlen(CONFIG_AP_PASSWORD)); /* array is pre-zeroed -> NUL terminated */
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=\"%s\" password=\"%s\" IP=192.168.4.1",
             CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
}
