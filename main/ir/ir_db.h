#pragma once
#include <stdint.h>

#define IR_PROFILE_COUNT 4   /* 占位;任务 2 扩展 */
const char *ir_db_get_profile_name(uint8_t profile_id);
uint8_t ir_db_get_profile_count(void);
