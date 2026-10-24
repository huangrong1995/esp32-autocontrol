#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/wifi_ap.h"
#include "app/web_server.h"

extern "C" void app_main(void)
{
    printf("esp32-ir-remote boot OK\n");

    /* SoftAP first (initialises NVS + netif + event loop), then the HTTP
     * server that phones can reach at http://192.168.4.1. */
    wifi_ap_start();
    web_server_start();

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
