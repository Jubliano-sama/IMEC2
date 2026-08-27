#include "battery_status.h"

#include <assert.h>

static void test_exact_anchor_boundaries(void)
{
    assert(battery_status_anchor_band(3529u) == BATTERY_STATUS_LOW);
    assert(battery_status_anchor_band(3530u) == BATTERY_STATUS_MIDDLE);
    assert(battery_status_anchor_band(3860u) == BATTERY_STATUS_MIDDLE);
    assert(battery_status_anchor_band(3861u) == BATTERY_STATUS_HIGH);
}

static void test_anchor_range_clamps(void)
{
    assert(battery_status_anchor_band(0u) == BATTERY_STATUS_LOW);
    assert(battery_status_anchor_band(BATTERY_STATUS_EMPTY_MV) ==
           BATTERY_STATUS_LOW);
    assert(battery_status_anchor_band(BATTERY_STATUS_FULL_MV) ==
           BATTERY_STATUS_HIGH);
    assert(battery_status_anchor_band(UINT16_MAX) == BATTERY_STATUS_HIGH);
}

static void test_clicker_exact_boundaries(void)
{
    assert(battery_status_clicker_band(3399u) == BATTERY_STATUS_LOW);
    assert(battery_status_clicker_band(3400u) == BATTERY_STATUS_MIDDLE);
    assert(battery_status_clicker_band(3800u) == BATTERY_STATUS_MIDDLE);
    assert(battery_status_clicker_band(3801u) == BATTERY_STATUS_HIGH);
    assert(battery_status_clicker_band(4200u) == BATTERY_STATUS_HIGH);
}

int main(void)
{
    test_exact_anchor_boundaries();
    test_anchor_range_clamps();
    test_clicker_exact_boundaries();
    return 0;
}
