#pragma once
#include <stdint.h>
#include "ir_encoder.h"   /* IR_KEY_COUNT, ir_key_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in IR profile table: one entry per remote/device family.
 * `cmds[index]` maps ir_key_t -> NEC command byte; 0xFF marks an
 * undefined/unknown key (never fabricated). */
typedef struct {
    const char *name;
    uint16_t nec_address;
    uint8_t cmds[IR_KEY_COUNT];   /* 未定义键 = 0xFF */
} ir_profile_t;

/* Return the profile at `profile_id`, or NULL if out of range. */
const ir_profile_t *ir_db_get_profile(uint8_t profile_id);

/* Number of profiles in the built-in table. */
uint8_t ir_db_get_profile_count(void);

/* Look up `key` in profile `p`. Returns 1 on success and fills `*addr`/`*cmd`;
 * returns 0 when the key is undefined (cmds[key]==0xFF) or arguments invalid. */
uint8_t ir_db_find_key(const ir_profile_t *p, ir_key_t key,
                       uint16_t *addr, uint8_t *cmd);

#ifdef __cplusplus
}
#endif
