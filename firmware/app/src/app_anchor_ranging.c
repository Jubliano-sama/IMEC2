#include "app_anchor_ranging.h"

#include "protocol.h"
#include "report.h"
#include "uwb.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

enum uwb_wake_decode_failure app_anchor_wake_failure_from_rx(
    enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
        return UWB_WAKE_DECODE_SFD_TIMEOUT;
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_NONE:
        return UWB_WAKE_DECODE_FRAME_TIMEOUT;
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return UWB_WAKE_DECODE_CRC_FAILURE;
    default:
        return UWB_WAKE_DECODE_FRAME_TIMEOUT;
    }
}

enum uwb_wake_decode_failure app_anchor_wake_failure_from_proto_ret(int ret)
{
    return ret == PROTO_ERR_BAD_CRC ? UWB_WAKE_DECODE_CRC_FAILURE :
                                      UWB_WAKE_DECODE_FRAME_TIMEOUT;
}

const char *app_anchor_wake_decode_failure_name(
    enum uwb_wake_decode_failure failure)
{
    switch (failure) {
    case UWB_WAKE_DECODE_PREAMBLE_ONLY:
        return "preamble_only";
    case UWB_WAKE_DECODE_SFD_TIMEOUT:
        return "sfd_timeout";
    case UWB_WAKE_DECODE_FRAME_TIMEOUT:
        return "frame_timeout";
    case UWB_WAKE_DECODE_CRC_FAILURE:
        return "crc_failure";
    default:
        return "unknown";
    }
}

const char *app_anchor_rx_failure_name(enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_NONE:
        return "none";
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
        return "no_preamble_timeout";
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
        return "sfd_timeout";
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
        return "frame_timeout";
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
        return "crc_or_phy";
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return "bad_frame";
    default:
        return "unknown";
    }
}

bool app_anchor_rx_failure_detected_preamble(
    enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return true;
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    case DWM3000_RX_FAILURE_NONE:
    default:
        return false;
    }
}

void app_anchor_log_range_schedule(
    const char *role,
    const struct uwb_range_schedule_frame *schedule)
{
    if (role == NULL || schedule == NULL) {
        return;
    }

    for (uint8_t i = 0u; i < schedule->selected_count; i++) {
        const struct uwb_range_schedule_entry *entry = &schedule->entries[i];

        LOG_INF("%s UWB RANGE_SCHEDULE entry: order=%u/%u anchor=0x%016llx seq_base=%u samples=%u first_poll_ms=%u stride_us=%u burst_ms=%u",
                role,
                (unsigned int)(i + 1u),
                schedule->selected_count,
                (unsigned long long)entry->anchor_id,
                entry->seq,
                entry->sample_count,
                schedule->first_poll_delay_ms,
                schedule->exchange_stride_us,
                schedule->burst_window_ms);
    }
}
