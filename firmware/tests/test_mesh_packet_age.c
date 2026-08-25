#include "dwm3000_timing.h"
#include "mesh_packet_age.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_age_includes_preparation_and_ceil_rounded_airtime(void)
{
    assert(mesh_packet_age_at_air_arrival(100u,
                                          1000u,
                                          true,
                                          1029u,
                                          UINT64_C(4551)) == 134u);
    assert(mesh_packet_age_at_air_arrival(7u,
                                          0u,
                                          false,
                                          9000u,
                                          UINT64_C(1)) == 8u);
    assert(mesh_packet_age_at_air_arrival(7u,
                                          0u,
                                          false,
                                          9000u,
                                          UINT64_C(1000)) == 8u);
    assert(mesh_packet_age_at_air_arrival(7u,
                                          0u,
                                          false,
                                          9000u,
                                          UINT64_C(1001)) == 9u);
}

static void test_queue_residence_is_rollover_safe_and_saturating(void)
{
    assert(mesh_packet_age_at_air_arrival(3u,
                                          UINT32_MAX - 10u,
                                          true,
                                          9u,
                                          0u) == 23u);
    assert(mesh_packet_age_at_air_arrival(UINT32_MAX - 2u,
                                          100u,
                                          true,
                                          101u,
                                          UINT64_C(1500)) == UINT32_MAX);
    assert(mesh_packet_age_at_air_arrival(UINT32_MAX,
                                          100u,
                                          true,
                                          200u,
                                          UINT64_MAX) == UINT32_MAX);
    assert(mesh_packet_age_at_air_arrival(0u,
                                          0u,
                                          false,
                                          0u,
                                          UINT64_MAX) == UINT32_MAX);
}

static void test_two_hops_preserve_the_original_schedule_epoch(void)
{
    const uint32_t origin_ms = 1000u;
    const uint32_t first_snapshot_ms = 1029u;
    const uint32_t first_airtime_ms = 5u;
    const uint32_t first_arrival_ms = first_snapshot_ms + first_airtime_ms;
    const uint32_t second_snapshot_ms = first_arrival_ms + 27u;
    const uint32_t second_airtime_ms = 2u;
    const uint32_t second_arrival_ms = second_snapshot_ms + second_airtime_ms;
    uint32_t first_age_ms;
    uint32_t second_age_ms;

    first_age_ms = mesh_packet_age_at_air_arrival(
        0u,
        origin_ms,
        true,
        first_snapshot_ms,
        UINT64_C(4551));
    assert(first_age_ms == first_arrival_ms - origin_ms);
    assert(first_arrival_ms - first_age_ms == origin_ms);

    second_age_ms = mesh_packet_age_at_air_arrival(
        first_age_ms,
        first_arrival_ms,
        true,
        second_snapshot_ms,
        UINT64_C(1901));
    assert(second_age_ms == second_arrival_ms - origin_ms);
    assert(second_arrival_ms - second_age_ms == origin_ms);
}

static void test_production_phy_airtime_is_accounted_in_full(void)
{
    const size_t frame_len = 965u;
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH9_MESH, frame_len);
    const uint32_t age_ms = mesh_packet_age_at_air_arrival(
        41u, 500u, true, 529u, airtime_us);

    assert(airtime_us == UINT64_C(10188));
    assert(age_ms == 41u + 29u + 11u);
}

int main(void)
{
    test_age_includes_preparation_and_ceil_rounded_airtime();
    test_queue_residence_is_rollover_safe_and_saturating();
    test_two_hops_preserve_the_original_schedule_epoch();
    test_production_phy_airtime_is_accounted_in_full();
    puts("mesh packet age tests passed");
    return 0;
}
