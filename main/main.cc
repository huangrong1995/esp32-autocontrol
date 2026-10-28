#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/wifi_ap.h"
#include "app/web_server.h"
#include "app/macro_engine.h"
#include "app/button_engine.h"

extern "C" void app_main(void)
{
    printf("esp32-ir-remote boot OK\n");

    /* SoftAP first (initialises NVS + netif + event loop), then the HTTP
     * server that phones can reach at http://192.168.4.1, then the macro
     * engine (NVS persistence + playback task). */
    wifi_ap_start();
    macro_engine_init();
    button_engine_init();
    /* Power-on auto-replay hook: replays the one-key macro once when the
     * selfstart flag is on AND a one-key macro is bound AND the device is
     * paired. Idempotent no-op otherwise, so a normal boot is unaffected. */
    macro_engine_selfstart();
    web_server_start();

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
