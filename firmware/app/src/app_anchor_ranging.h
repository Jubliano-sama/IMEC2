#ifndef APP_ANCHOR_RANGING_H
#define APP_ANCHOR_RANGING_H

#include "dwm3000_driver.h"
#include "uwb_session.h"

#include <stdbool.h>

struct uwb_range_schedule_frame;

enum uwb_wake_decode_failure app_anchor_wake_failure_from_rx(
    enum dwm3000_rx_failure failure);
enum uwb_wake_decode_failure app_anchor_wake_failure_from_proto_ret(int ret);
const char *app_anchor_wake_decode_failure_name(
    enum uwb_wake_decode_failure failure);
const char *app_anchor_rx_failure_name(enum dwm3000_rx_failure failure);
bool app_anchor_rx_failure_detected_preamble(
    enum dwm3000_rx_failure failure);

void app_anchor_log_range_schedule(
    const char *role,
    const struct uwb_range_schedule_frame *schedule);

#endif
