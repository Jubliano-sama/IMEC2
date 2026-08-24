#ifndef APP_MESH_C5_PRIORITY_H
#define APP_MESH_C5_PRIORITY_H

#include "mesh_relay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_c5_flood_priority_state {
    bool response_priority;
    bool anchor_busy;
    bool gateway_ch5_preempt;
};

struct app_mesh_c5_route_capture_state {
    uint8_t msg_type;
    uint32_t session_id;
    uint32_t flood_epoch_id;
    uint16_t reply_nonce;
    uint64_t src_id;
    uint64_t dst_id;
    uint64_t previous_hop_id;
    uint64_t target_id;
    uint64_t route_request_target_id;
    uint64_t local_id;
    uint64_t control_origin_id;
    uint32_t expected_session_id;
    uint32_t expected_flood_epoch_id;
    uint16_t expected_reply_nonce;
    bool route_identity_required;
    bool control_followup;
    bool gateway_control_priority;
    bool require_relayed_gateway_control;
    bool targeted_control_relay;
};

struct app_mesh_c5_route_adv_timing {
    uint32_t wake_to_route_delay_ms;
    uint32_t request_flood_burst_ms;
    uint32_t embedded_reply_guard_ms;
    uint32_t route_adv_reply_guard_ms;
};

struct app_mesh_c5_route_reply_window_timing {
    uint32_t base_reply_window_ms;
    uint32_t wake_train_ms;
    uint32_t post_wake_route_rx_ms;
    uint32_t wake_to_route_delay_ms;
    uint32_t request_flood_burst_ms;
    uint32_t flood_forward_wave_ms;
    uint32_t route_reply_exchange_ms;
    uint32_t direct_gateway_probe_ms;
    uint32_t guard_ms;
};

struct app_mesh_c5_connected_gap_timing {
    uint32_t next_channel9_delay_ms;
    uint32_t scan_cap_ms;
    uint32_t min_scan_ms;
    uint32_t retune_margin_ms;
};

#define APP_MESH_C5_CONTROL_ROUTE_HISTORY_SIZE 4u

struct app_mesh_c5_control_route_identity {
    /* Physical previous hop is deliberately excluded: echoes keep identity. */
    uint32_t route_epoch;
    uint32_t session_id;
    uint16_t seq;
    uint8_t msg_type;
};

struct app_mesh_c5_control_route_history {
    struct app_mesh_c5_control_route_identity
        entries[APP_MESH_C5_CONTROL_ROUTE_HISTORY_SIZE];
    uint8_t valid_mask;
    uint8_t next_index;
};

enum app_mesh_c5_connected_gap_rx_action {
    APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE = 0,
    APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK,
    APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL,
    APP_MESH_C5_CONNECTED_GAP_RX_COMPLETE,
};

bool app_mesh_c5_flood_should_defer(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_gateway_rx_should_yield_to_response(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_gateway_route_adv_allowed(bool mesh_route_test_enabled);
bool app_mesh_c5_contact_expired(
    const struct c5_contact_context *contact,
    uint32_t now_ms);
bool app_mesh_c5_contact_accepted(
    const struct c5_contact_context *contact,
    uint64_t peer_id,
    uint8_t purpose,
    uint32_t now_ms);
bool app_mesh_c5_route_capture_relevant(
    const struct app_mesh_c5_route_capture_state *state);
bool app_mesh_c5_route_capture_completes_discovery(uint8_t msg_type);
bool app_mesh_c5_route_capture_yields_to_competing_request(uint8_t msg_type);
bool app_mesh_c5_route_capture_requires_ack_hold(uint8_t msg_type);
bool app_mesh_c5_route_capture_requires_inline_timing_install(
    uint8_t msg_type,
    bool timing_negotiation_active);
bool app_mesh_c5_route_capture_requires_post_rx_response(uint8_t msg_type);
bool app_mesh_c5_route_capture_receive_aborted(int receive_ret);
bool app_mesh_c5_gateway_control_origin_ttl(uint8_t msg_type,
                                            uint16_t command_id,
                                            uint8_t *origin_ttl);
bool app_mesh_c5_gateway_operation_outranks_unaccepted_click(
    uint8_t msg_type,
    const uint8_t *payload,
    size_t payload_len);
bool app_mesh_c5_event_accept_reservation(
    const struct mesh_event_timing *accepted,
    uint16_t realign_slop_ms,
    struct mesh_event_timing *reservation);
bool app_mesh_c5_control_uses_extended_phr(uint8_t msg_type,
                                           size_t frame_len,
                                           size_t standard_frame_max_len);
bool app_mesh_c5_wake_claim_preempts_mesh(uint8_t claim_flags);
bool app_mesh_c5_wake_claim_requires_anchor_handoff(uint8_t claim_flags,
                                                    bool local_can_range_clicks);
bool app_mesh_c5_route_wake_claim_allowed(
    uint64_t source_id,
    uint64_t gateway_id,
    uint8_t claim_flags,
    bool require_relayed_route_req,
    bool require_relayed_gateway_control);
bool app_mesh_c5_route_wake_should_listen(
    uint64_t source_id,
    uint64_t gateway_id,
    uint8_t claim_flags,
    bool require_relayed_route_req,
    bool require_relayed_gateway_control);
bool app_mesh_c5_gateway_control_copy_allowed(
    uint64_t source_id,
    uint64_t previous_hop_id,
    uint64_t gateway_id,
    bool require_relayed_gateway_control);
bool app_mesh_c5_gateway_control_rx_allowed(
    uint8_t msg_type,
    uint8_t packet_ttl,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t source_id,
    uint64_t previous_hop_id,
    uint64_t gateway_id,
    bool require_relayed_gateway_control,
    uint8_t required_gateway_relay_hops);
bool app_mesh_c5_control_route_hint_is_first(
    struct app_mesh_c5_control_route_history *history,
    const struct app_mesh_c5_control_route_identity *identity);
bool app_mesh_c5_wake_followup_uses_extended_phr(uint8_t claim_flags);
bool app_mesh_c5_wake_followup_is_control(uint8_t claim_flags);
bool app_mesh_c5_control_followup_yields_to_ack(uint8_t claim_flags,
                                                bool ch9_ack_wait_active);
bool app_mesh_c5_command_followup_holds_same_train_wake(
    uint8_t contact_purpose,
    bool click_priority);
uint32_t app_mesh_c5_route_reply_listen_window_ms(
    uint8_t route_ttl,
    const struct app_mesh_c5_route_reply_window_timing *timing);
uint32_t app_mesh_c5_route_adv_response_delay_ms(
    uint16_t wake_train_ends_in_ms,
    bool embedded_route_frame,
    const struct app_mesh_c5_route_adv_timing *timing);
uint32_t app_mesh_c5_connected_gap_window_ms(
    const struct app_mesh_c5_connected_gap_timing *timing);
uint32_t app_mesh_c5_connected_gap_reschedule_ms(
    uint32_t next_channel9_delay_ms,
    uint32_t min_scan_ms,
    uint32_t retune_margin_ms);
enum app_mesh_c5_connected_gap_rx_action
app_mesh_c5_connected_gap_rx_action(bool click_claim,
                                    bool route_control_claim,
                                    bool deadline_reached);

#endif
