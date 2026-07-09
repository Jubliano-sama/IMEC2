#include "app_wake_train_politeness.h"

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
        if (base_ms >= APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS / 2u) {
            base_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS;
            break;
        }
        base_ms *= 2u;
    }

    if (base_ms > APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS) {
        base_ms = APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS;
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
