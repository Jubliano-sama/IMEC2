#ifndef APP_ANCHOR_RADIO_POLICY_H
#define APP_ANCHOR_RADIO_POLICY_H

#include <stdbool.h>
#include <stdint.h>

struct uwb_anchor_epoch;
struct uwb_anchor_pair_schedule_frame;

uint32_t app_anchor_radio_blocked_retry_ms(bool route_test,
                                           uint32_t busy_retry_ms,
                                           uint32_t normal_interval_ms);
bool app_anchor_radio_pair_schedule_matches_epoch(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    const struct uwb_anchor_epoch *epoch);

#endif
