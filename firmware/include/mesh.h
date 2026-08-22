#ifndef MESH_H
#define MESH_H

#include "mesh_event_timing.h"
#include "protocol.h"
#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_NETWORK_MAX_HOPS 8u
#define MESH_DEFAULT_TTL MESH_NETWORK_MAX_HOPS
#define MESH_GATEWAY_ACK_TTL MESH_NETWORK_MAX_HOPS
#define MESH_EVENT_CHANNEL UWB_CHANNEL_MESH_PAYLOAD
#define MESH_CH9_BATCH_FLAG_FINAL 0x01u
#define MESH_ACK_SEMANTIC_IDENTITY_MAX 8u
#define MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN \
    (sizeof(uint32_t) + sizeof(uint16_t) + SEMANTIC_DIGEST_SHA256_LEN)
#define MESH_ACK_SEMANTIC_IDENTITY_TLV_LEN \
    (PROTO_TLV_HEADER_LEN + MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN)
#define MESH_ACK_SINGLE_PAYLOAD_LEN \
    ((PROTO_TLV_HEADER_LEN + sizeof(uint16_t)) + \
     MESH_ACK_SEMANTIC_IDENTITY_TLV_LEN)
#define MESH_ACK_BATCH_MAX_PAYLOAD_LEN \
    ((PROTO_TLV_HEADER_LEN + sizeof(uint16_t)) + \
     (PROTO_TLV_HEADER_LEN + \
      MESH_ACK_SEMANTIC_IDENTITY_MAX * sizeof(uint32_t)) + \
     (PROTO_TLV_HEADER_LEN + \
      MESH_ACK_SEMANTIC_IDENTITY_MAX * sizeof(uint16_t)) + \
     (PROTO_TLV_HEADER_LEN + \
      MESH_ACK_SEMANTIC_IDENTITY_MAX * sizeof(uint32_t)) + \
     MESH_ACK_SEMANTIC_IDENTITY_MAX * \
      MESH_ACK_SEMANTIC_IDENTITY_TLV_LEN)
#define MESH_GATEWAY_ACK_CONFIRM_IDENTITY_VALUE_LEN \
    (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t) + \
     sizeof(uint16_t) + sizeof(uint16_t) + \
     SEMANTIC_DIGEST_SHA256_LEN)
#define MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN \
    (PROTO_TLV_HEADER_LEN + MESH_GATEWAY_ACK_CONFIRM_IDENTITY_VALUE_LEN)

struct mesh_ack_semantic_identity {
    uint32_t session_id;
    uint16_t seq;
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
};

struct mesh_gateway_ack_confirm_identity {
    uint8_t msg_type;
    uint8_t flags;
    uint32_t session_id;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
};

struct mesh_ch9_batch_metadata {
    uint32_t batch_id;
    uint8_t flags;
    bool present;
    bool final_packet;
};

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

struct mesh_channel5_requirements {
    uint32_t next_required_scan_start_ms;
    uint32_t active_until_ms;
    uint16_t retune_guard_ms;
    bool click_epoch_active;
    bool discovery_active;
    bool ranging_active;
    bool next_required_scan_start_valid;
    bool active_until_valid;
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
void mesh_event_timing_set_local_first_slot_tx(struct mesh_event_timing *timing,
                                               bool local_first_slot_tx);
bool mesh_event_timing_bind_proposal_session(
    struct mesh_event_timing *timing,
    uint32_t operation_session_id);
bool mesh_event_timing_local_tx_slot(const struct mesh_event_timing *timing);
bool mesh_event_timing_local_rx_slot(const struct mesh_event_timing *timing);
uint32_t mesh_event_guard_start_ms(const struct mesh_event_timing *timing);
void mesh_event_timing_reanchor_after_control_tx(
    struct mesh_event_timing *timing,
    uint32_t tx_done_ms,
    uint32_t encoded_delay_ms,
    uint32_t rx_reference_offset_ms);
int mesh_event_plan_channel9(const struct mesh_event_timing *timing,
                             const struct mesh_channel5_requirements *requirements,
                             uint32_t now_ms,
                             struct mesh_event_plan *plan);
bool mesh_event_plan_is_policy_deferral(enum mesh_event_plan_action action);
void mesh_event_note_success(struct mesh_event_timing *timing,
                             uint32_t event_start_ms);
void mesh_event_note_unobserved_turn(struct mesh_event_timing *timing,
                                     uint32_t event_start_ms);
void mesh_event_note_observed_packet(struct mesh_event_timing *timing,
                                     uint32_t planned_event_start_ms,
                                     uint32_t observed_packet_ms);
void mesh_event_note_missed(struct mesh_event_timing *timing,
                            struct mesh_event_diagnostics *diagnostics);
uint8_t mesh_event_skip_elapsed(struct mesh_event_timing *timing,
                                uint32_t now_ms,
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
int mesh_append_event_update_tlvs_at(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct mesh_event_timing *timing,
                                     uint32_t now_ms);
int mesh_append_compact_event_timing_tlvs_at(uint8_t *payload,
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
int mesh_ch9_batch_metadata_parse(const uint8_t *payload,
                                  size_t payload_len,
                                  struct mesh_ch9_batch_metadata *metadata);
int mesh_init_event_control(struct proto_packet *packet,
                            uint8_t msg_type,
                            uint64_t local_id,
                            uint64_t peer_id,
                            uint32_t session_id,
                            uint16_t seq,
                            uint8_t payload_len);
bool mesh_packet_rf_channel_allowed(uint8_t msg_type,
                                    uint8_t radio_channel,
                                    bool synthetic_mesh_data_enabled);
int mesh_packet_rx_semantics_validate(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint64_t local_id,
                                      uint64_t gateway_id);
int mesh_packet_rx_envelope_validate(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t previous_hop_id,
                                     uint64_t local_id,
                                     uint64_t gateway_id,
                                     uint8_t radio_channel,
                                     bool synthetic_mesh_data_enabled);

int mesh_append_requested_seq(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   uint16_t requested_seq);
bool mesh_packet_semantic_digest(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);
int mesh_append_ack_semantic_identity(
    uint8_t *ack_payload,
    size_t ack_payload_cap,
    size_t *offset,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len);
int mesh_ack_semantic_identity_at(
    const uint8_t *ack_payload,
    size_t ack_payload_len,
    uint8_t index,
    struct mesh_ack_semantic_identity *identity);
int mesh_ack_payload_contains_packet(
    const struct proto_packet *ack_packet,
    const uint8_t *ack_payload,
    size_t ack_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    bool *contains);
/*
 * Legacy session/sequence parsing remains available for wire diagnostics.
 * Custody release must use mesh_ack_payload_contains_packet().
 */
int mesh_ack_payload_contains(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t requested_session_id,
                              uint16_t requested_seq,
                              bool *contains);
bool mesh_gateway_ack_confirmed_type(uint8_t msg_type);
bool mesh_gateway_ack_confirmed_flags_valid(uint8_t msg_type,
                                            uint8_t flags);
int mesh_gateway_ack_confirm_payload_build(
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    uint8_t *confirm_payload,
    size_t confirm_payload_cap,
    size_t *confirm_payload_len);
int mesh_gateway_ack_confirm_payload_parse(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct mesh_gateway_ack_confirm_identity *identity);
int mesh_gateway_ack_confirm_identity_packet(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct proto_packet *acknowledged_packet,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);
int mesh_gateway_ack_confirm_matches_packet(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    bool *matches);
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
int mesh_init_gateway_ack_confirm(struct proto_packet *packet,
                                  uint64_t source_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t confirm_seq);
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
