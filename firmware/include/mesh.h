#ifndef MESH_H
#define MESH_H

#include "protocol.h"
#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_DEFAULT_TTL 4u
#define MESH_GATEWAY_ACK_TTL 4u
#define MESH_EVENT_CHANNEL UWB_CHANNEL_MESH_PAYLOAD

enum mesh_event_plan_action {
    MESH_EVENT_PLAN_START = 0,
    MESH_EVENT_PLAN_CLIP = 1,
    MESH_EVENT_PLAN_WAIT = 2,
    MESH_EVENT_PLAN_DEFER_CH5_ACTIVE = 3,
    MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD = 4,
    MESH_EVENT_PLAN_REFRESH_CONTACT_CH5 = 5,
};

struct mesh_event_params {
    uint32_t event_interval_ms;
    uint16_t event_window_ms;
    uint32_t first_event_time_ms;
    uint16_t guard_ms;
    int16_t peer_clock_skew_estimate_ppm;
    uint8_t max_missed_events;
    uint32_t supervision_timeout_ms;
};

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
    bool route_fresh;
    bool timing_fresh;
    bool fallback_required;
};

struct mesh_channel5_requirements {
    uint32_t next_required_scan_start_ms;
    uint32_t active_until_ms;
    uint16_t retune_guard_ms;
    bool click_epoch_active;
    bool discovery_active;
    bool ranging_active;
};

struct mesh_event_plan {
    enum mesh_event_plan_action action;
    uint32_t start_ms;
    uint32_t end_ms;
    uint16_t window_ms;
};

struct mesh_event_diagnostics {
    uint32_t channel_switches;
    uint32_t pll_ready_failures;
    uint32_t late_channel5_returns;
    uint32_t mesh_deferrals;
    uint32_t ch9_event_misses;
    uint32_t channel5_preemptions;
    uint32_t ch9_report_latency_ms;
};

int mesh_event_timing_negotiate(struct mesh_event_timing *timing,
                                const struct mesh_event_params *params,
                                bool channel5_contact_refreshed);
bool mesh_event_timing_usable(const struct mesh_event_timing *timing,
                              uint32_t now_ms);
int mesh_event_plan_channel9(const struct mesh_event_timing *timing,
                             const struct mesh_channel5_requirements *requirements,
                             uint32_t now_ms,
                             struct mesh_event_plan *plan);
void mesh_event_note_success(struct mesh_event_timing *timing,
                             uint32_t event_start_ms);
void mesh_event_note_observed_packet(struct mesh_event_timing *timing,
                                     uint32_t planned_event_start_ms,
                                     uint32_t observed_packet_ms);
void mesh_event_note_missed(struct mesh_event_timing *timing,
                            struct mesh_event_diagnostics *diagnostics);
void mesh_event_note_channel_switch(struct mesh_event_diagnostics *diagnostics,
                                    bool pll_ready,
                                    bool late_channel5_return);
void mesh_event_note_plan_action(struct mesh_event_diagnostics *diagnostics,
                                 enum mesh_event_plan_action action);
void mesh_event_note_report_latency(struct mesh_event_diagnostics *diagnostics,
                                    uint32_t latency_ms);
int mesh_append_event_timing_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct mesh_event_timing *timing);
int mesh_append_event_timing_tlvs_at(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct mesh_event_timing *timing,
                                     uint32_t now_ms);
int mesh_event_timing_from_tlvs(struct mesh_event_timing *timing,
                                const uint8_t *payload,
                                size_t payload_len,
                                bool channel5_contact_refreshed);
int mesh_event_timing_from_tlvs_at(struct mesh_event_timing *timing,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms,
                                   bool channel5_contact_refreshed);
int mesh_init_event_control(struct proto_packet *packet,
                            uint8_t msg_type,
                            uint64_t local_id,
                            uint64_t peer_id,
                            uint32_t session_id,
                            uint16_t seq,
                            uint8_t payload_len);

int mesh_append_requested_seq(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   uint16_t requested_seq);
int mesh_append_command_id(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                enum command_id command_id);
int mesh_append_command_result(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    enum command_id command_id,
                                    enum command_status status,
                                    uint8_t reason);

int mesh_init_gateway_ack(struct proto_packet *packet,
                               uint64_t gateway_id,
                               uint64_t original_src_id,
                               uint32_t session_id,
                               uint16_t ack_seq,
                               uint8_t payload_len);
int mesh_init_command(struct proto_packet *packet,
                           uint64_t gateway_id,
                           uint64_t target_id,
                           uint32_t session_id,
                           uint16_t seq,
                           uint8_t payload_len);
int mesh_init_command_result(struct proto_packet *packet,
                                  uint64_t target_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len,
                                  bool diagnostic);

#ifdef __cplusplus
}
#endif

#endif
