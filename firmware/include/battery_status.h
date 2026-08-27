#ifndef BATTERY_STATUS_H
#define BATTERY_STATUS_H

#include <stdint.h>

#define BATTERY_STATUS_EMPTY_MV 3200u
#define BATTERY_STATUS_FULL_MV 4200u
#define BATTERY_STATUS_ANCHOR_LOW_PERCENT 33u
#define BATTERY_STATUS_ANCHOR_HIGH_PERCENT 66u
#define BATTERY_STATUS_CLICKER_LOW_PERCENT 20u
#define BATTERY_STATUS_CLICKER_HIGH_PERCENT 60u

enum battery_status_band {
    BATTERY_STATUS_LOW = 0,
    BATTERY_STATUS_MIDDLE,
    BATTERY_STATUS_HIGH,
};

/*
 * Compare a cell voltage against an exact percentage of the documented
 * 3.2-4.2 V operating span. The result is negative below the threshold, zero
 * on it, and positive above it; no integer-percentage rounding is involved.
 */
int battery_status_compare_percent(uint16_t battery_mv,
                                   uint8_t threshold_percent);

enum battery_status_band battery_status_anchor_band(uint16_t battery_mv);
enum battery_status_band battery_status_clicker_band(uint16_t battery_mv);

#endif
