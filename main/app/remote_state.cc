#include "app/remote_state.h"

uint8_t remote_state_get_profile(void)
{
    return 0xFF; /* 未配对 */
}

void remote_state_set_profile(uint8_t profile_id)
{
    (void)profile_id;
}