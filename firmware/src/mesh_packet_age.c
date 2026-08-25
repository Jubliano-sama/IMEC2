#include "mesh_packet_age.h"

#include <limits.h>

static uint32_t saturating_add_ms(uint32_t value, uint64_t elapsed_ms)
{
    return elapsed_ms >= UINT32_MAX || value > UINT32_MAX - elapsed_ms ?
        UINT32_MAX : value + (uint32_t)elapsed_ms;
}

uint32_t mesh_packet_age_at_air_arrival(uint32_t base_age_ms,
                                        uint32_t queued_at_ms,
                                        bool queued_at_valid,
                                        uint32_t tx_snapshot_ms,
                                        uint64_t frame_airtime_us)
{
    uint32_t age_ms = base_age_ms;
    uint64_t airtime_ms = frame_airtime_us / 1000u +
        (frame_airtime_us % 1000u != 0u ? 1u : 0u);

    if (queued_at_valid) {
        age_ms = saturating_add_ms(age_ms, tx_snapshot_ms - queued_at_ms);
    }
    return saturating_add_ms(age_ms, airtime_ms);
}
