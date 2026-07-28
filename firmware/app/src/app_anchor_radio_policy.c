#include "app_anchor_radio_policy.h"

#include "uwb.h"

uint32_t app_anchor_radio_blocked_retry_ms(bool route_test,
                                           uint32_t busy_retry_ms,
                                           uint32_t normal_interval_ms)
{
    return route_test ? busy_retry_ms : normal_interval_ms;
}

bool app_anchor_radio_pair_schedule_matches_epoch(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    const struct uwb_anchor_epoch *epoch)
{
    return schedule != NULL && epoch != NULL && epoch->active &&
           schedule->network_id == epoch->network_id &&
           schedule->clicker_id == epoch->clicker_id &&
           schedule->survey_id == epoch->click_event_id &&
           schedule->attempt_index == epoch->attempt_index &&
           schedule->nonce == epoch->nonce &&
           schedule->flags == epoch->flags;
}
