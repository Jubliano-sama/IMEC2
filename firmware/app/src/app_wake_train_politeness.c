#include "app_wake_train_politeness.h"

#include <limits.h>

bool app_wake_train_politeness_rx_activity(int rx_ret,
                                           enum dwm3000_rx_failure failure)
{
    if (rx_ret == 0) {
        return true;
    }

    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return true;
    case DWM3000_RX_FAILURE_NONE:
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    default:
        return false;
    }
}

uint32_t app_wake_train_politeness_backoff_ms(uint8_t retry_index,
                                              uint32_t random_value)
{
    uint32_t base_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_MIN_MS;
    uint32_t jitter_window_ms;

    for (uint8_t i = 0u; i < retry_index; i++) {
        if (base_ms >= APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS / 2u) {
            base_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS;
            break;
        }
        base_ms *= 2u;
    }

    if (base_ms > APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS) {
        base_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS;
    }
    jitter_window_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS - base_ms;
    if (jitter_window_ms >= base_ms) {
        jitter_window_ms = base_ms - 1u;
    }
    if (jitter_window_ms == 0u) {
        return base_ms;
    }

    return base_ms + (random_value % (jitter_window_ms + 1u));
}

bool app_wake_train_deadline_fits(int64_t now_ms,
                                  int64_t deadline_ms,
                                  uint32_t required_ms)
{
    if (deadline_ms == INT64_MAX) {
        return true;
    }
    if (now_ms < 0 || now_ms > deadline_ms) {
        return false;
    }
    return (uint64_t)required_ms <= (uint64_t)(deadline_ms - now_ms);
}

bool app_wake_train_deadline_clip_delay(int64_t now_ms,
                                        int64_t deadline_ms,
                                        uint32_t requested_delay_ms,
                                        uint32_t required_tail_ms,
                                        uint32_t *delay_ms)
{
    uint64_t available_ms;

    if (delay_ms == NULL) {
        return false;
    }
    *delay_ms = 0u;

    if (deadline_ms == INT64_MAX) {
        *delay_ms = requested_delay_ms;
        return true;
    }
    if (!app_wake_train_deadline_fits(now_ms,
                                      deadline_ms,
                                      required_tail_ms)) {
        return false;
    }

    available_ms = (uint64_t)(deadline_ms - now_ms) - required_tail_ms;
    *delay_ms = available_ms < requested_delay_ms ?
                (uint32_t)available_ms : requested_delay_ms;
    return true;
}
