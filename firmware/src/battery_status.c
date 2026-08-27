#include "battery_status.h"

int battery_status_compare_percent(uint16_t battery_mv,
                                   uint8_t threshold_percent)
{
    const uint32_t span_mv = BATTERY_STATUS_FULL_MV -
                             BATTERY_STATUS_EMPTY_MV;
    uint32_t level_scaled;
    uint32_t threshold_scaled;

    if (battery_mv <= BATTERY_STATUS_EMPTY_MV) {
        level_scaled = 0u;
    } else if (battery_mv >= BATTERY_STATUS_FULL_MV) {
        level_scaled = span_mv * 100u;
    } else {
        level_scaled =
            ((uint32_t)battery_mv - BATTERY_STATUS_EMPTY_MV) * 100u;
    }
    threshold_scaled = (uint32_t)threshold_percent * span_mv;

    if (level_scaled < threshold_scaled) {
        return -1;
    }
    if (level_scaled > threshold_scaled) {
        return 1;
    }
    return 0;
}

enum battery_status_band battery_status_anchor_band(uint16_t battery_mv)
{
    if (battery_status_compare_percent(
            battery_mv, BATTERY_STATUS_ANCHOR_LOW_PERCENT) < 0) {
        return BATTERY_STATUS_LOW;
    }
    if (battery_status_compare_percent(
            battery_mv, BATTERY_STATUS_ANCHOR_HIGH_PERCENT) > 0) {
        return BATTERY_STATUS_HIGH;
    }
    return BATTERY_STATUS_MIDDLE;
}

enum battery_status_band battery_status_clicker_band(uint16_t battery_mv)
{
    if (battery_status_compare_percent(
            battery_mv, BATTERY_STATUS_CLICKER_LOW_PERCENT) < 0) {
        return BATTERY_STATUS_LOW;
    }
    if (battery_status_compare_percent(
            battery_mv, BATTERY_STATUS_CLICKER_HIGH_PERCENT) > 0) {
        return BATTERY_STATUS_HIGH;
    }
    return BATTERY_STATUS_MIDDLE;
}
