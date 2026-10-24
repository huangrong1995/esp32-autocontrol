#pragma once
#include <stdint.h>

typedef enum {
    IR_KEY_POWER, IR_KEY_VOL_UP, IR_KEY_VOL_DOWN, IR_KEY_CH_UP, IR_KEY_CH_DOWN,
    IR_KEY_MUTE, IR_KEY_MENU, IR_KEY_EXIT, IR_KEY_OK,
    IR_KEY_UP, IR_KEY_DOWN, IR_KEY_LEFT, IR_KEY_RIGHT,
    IR_KEY_0, IR_KEY_1, IR_KEY_2, IR_KEY_3, IR_KEY_4,
    IR_KEY_5, IR_KEY_6, IR_KEY_7, IR_KEY_8, IR_KEY_9,
    IR_KEY_COUNT
} ir_key_t;

void ir_send_key(ir_key_t key);
