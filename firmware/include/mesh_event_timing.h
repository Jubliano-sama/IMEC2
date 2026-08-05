#ifndef MESH_EVENT_TIMING_H
#define MESH_EVENT_TIMING_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure event-control timing value shared by the protocol policy and the
 * application retry owner.  It deliberately carries no relay, route, or
 * transport state.
 */
struct mesh_event_timing {
    uint8_t mesh_channel;
    uint32_t event_interval_ms;
    uint16_t event_window_ms;
    uint32_t next_event_time_ms;
    uint32_t event_counter;
    uint16_t guard_ms;
    int16_t peer_clock_skew_estimate_ppm;
    uint8_t max_missed_events;
    uint8_t missed_event_count;
    uint32_t supervision_timeout_ms;
    uint32_t last_successful_ch9_event_ms;
    bool local_tx_on_even_events;
    bool route_fresh;
    bool timing_fresh;
    bool fallback_required;
};

#endif
