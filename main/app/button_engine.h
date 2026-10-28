#pragma once

/*
 * One-key (physical) button engine — polls a GPIO button and plays the macro
 * bound via POST /api/macro/play {"id":N,"one_key":true} (see
 * app/macro_engine.h macro_set_onekey / macro_get_onekey). Wiring: the button
 * shorts the configured GPIO to GND; internal pull-up makes a press read LOW.
 * Presses are debounced (app/button_core.h) and ignored while the engine is
 * busy playing/recording — the page's 停止回放 stays the stop path.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Configure + spawn the polling task. Safe no-op (logged) when the configured
 * GPIO collides with the IR TX pin or the GPIO layer fails. */
void button_engine_init(void);

#ifdef __cplusplus
}
#endif
