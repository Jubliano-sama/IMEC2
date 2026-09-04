#include "app_mesh_c5_priority.h"
#include "app_mesh_route_request_policy.h"
#include "gateway_command.h"

#include <errno.h>
#include <stddef.h>

static bool event_control_type(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static bool targeted_control_followup_type(uint8_t msg_type)
{
    return msg_type == MSG_COMMAND;
}

static bool targeted_control_previous_hop_valid(
    const struct app_mesh_c5_route_capture_state *state)
{
    return state->previous_hop_id != 0u &&
           state->previous_hop_id != MESH_BROADCAST_ID &&
           state->previous_hop_id != state->local_id;
}

bool app_mesh_c5_flood_should_defer(
    const struct app_mesh_c5_flood_priority_state *state)
{
    if (state == NULL) {
        return true;
    }

    if (state->anchor_busy) {
        return true;
    }

    if (state->gateway_ch5_preempt && !state->response_priority) {
        return true;
    }

    return false;
}

bool app_mesh_c5_gateway_rx_should_yield_to_response(
    const struct app_mesh_c5_flood_priority_state *state)
{
    if (state == NULL) {
        return false;
    }

    return state->response_priority && state->gateway_ch5_preempt &&
           !state->anchor_busy;
}

bool app_mesh_c5_gateway_route_adv_allowed(bool mesh_route_test_enabled)
{
    return !mesh_route_test_enabled;
}

bool app_mesh_c5_contact_expired(
    const struct c5_contact_context *contact,
    uint32_t now_ms)
{
    if (contact == NULL || contact->state == C5_CONTACT_NONE) {
        return false;
    }

    return (int32_t)(now_ms - contact->expires_at_ms) >= 0;
}

bool app_mesh_c5_contact_accepted(
    const struct c5_contact_context *contact,
    uint64_t peer_id,
    uint8_t purpose,
    uint32_t now_ms)
{
    return contact != NULL &&
           contact->state != C5_CONTACT_NONE &&
           contact->peer_id == peer_id &&
           contact->purpose == purpose &&
           contact->accepted &&
           !app_mesh_c5_contact_expired(contact, now_ms);
}

bool app_mesh_c5_route_capture_relevant(
    const struct app_mesh_c5_route_capture_state *state)
{
    uint64_t control_origin_id;

    if (state == NULL) {
        return false;
    }

    control_origin_id = state->control_origin_id != 0u ?
        state->control_origin_id : state->target_id;

    if (state->msg_type == MSG_ROUTE_REPLY) {
        /* The route validator owns logical source and path identity.  This
         * layer only correlates the reply to the active receive handoff. */
        return state->dst_id == state->local_id &&
               (!state->route_identity_required ||
                (state->expected_session_id != 0u &&
                 state->expected_flood_epoch_id != 0u &&
                 state->expected_reply_nonce != 0u &&
                 state->session_id == state->expected_session_id &&
                 state->flood_epoch_id == state->expected_flood_epoch_id &&
                 state->reply_nonce == state->expected_reply_nonce));
    }

    if (state->msg_type == MSG_GATEWAY_ROUTE_ADV) {
        /* A relayed control wake names the physical relay as target_id while
         * the advertisement correctly retains the gateway as src_id.  The
         * route validator checks gateway identity, ancestry, TTL, and loops. */
        return state->dst_id == MESH_BROADCAST_ID &&
               state->previous_hop_id != 0u &&
               state->previous_hop_id != MESH_BROADCAST_ID &&
               state->previous_hop_id != state->local_id;
    }

    if (state->msg_type == MSG_ROUTE_REQ) {
        return state->src_id != 0u &&
               state->src_id != MESH_BROADCAST_ID &&
               state->src_id != state->local_id &&
               state->dst_id == MESH_BROADCAST_ID &&
               state->previous_hop_id != 0u &&
               state->previous_hop_id != MESH_BROADCAST_ID &&
               state->previous_hop_id != state->local_id &&
               state->route_request_target_id != 0u &&
               state->route_request_target_id != MESH_BROADCAST_ID &&
               (state->route_request_target_id == state->target_id ||
                state->route_request_target_id == control_origin_id ||
                state->route_request_target_id == state->local_id);
    }

    if (event_control_type(state->msg_type)) {
        return state->dst_id == state->local_id;
    }

    /*
     * Uplink custody handed to this parent (Channel 5 Delivery Protocol
     * section 3/4).  Without this the listener a wake train opened dropped
     * the very frame the train was sent to collect as "unrelated", so the
     * child never saw MSG_MESH_HOP_ACK and retrained forever.
     */
    if (state->uplink_capture_allowed &&
        app_mesh_c5_route_capture_completes_uplink(state->msg_type)) {
        return targeted_control_previous_hop_valid(state) &&
               state->previous_hop_id == state->target_id &&
               state->src_id != state->local_id &&
               state->dst_id != MESH_BROADCAST_ID;
    }

    if ((state->control_followup || state->gateway_control_priority) &&
        control_origin_id != MESH_BROADCAST_ID &&
        state->src_id == control_origin_id) {
        if (!app_mesh_c5_gateway_control_copy_allowed(
                state->src_id,
                state->previous_hop_id,
                control_origin_id,
                state->require_relayed_gateway_control)) {
            return false;
        }

        if (targeted_control_followup_type(state->msg_type) &&
            state->dst_id != MESH_BROADCAST_ID) {
            if (state->dst_id == state->local_id) {
                return targeted_control_previous_hop_valid(state);
            }
            if (state->targeted_control_relay &&
                state->dst_id != MESH_BROADCAST_ID &&
                state->dst_id != state->local_id) {
                return targeted_control_previous_hop_valid(state);
            }
            return false;
        }

        if (state->msg_type == MSG_COMMAND) {
            return state->dst_id == MESH_BROADCAST_ID &&
                   targeted_control_previous_hop_valid(state);
        }
    }

    return false;
}

bool app_mesh_c5_route_capture_completes_discovery(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REPLY ||
           msg_type == MSG_GATEWAY_ROUTE_ADV;
}

bool app_mesh_c5_route_capture_completes_uplink(uint8_t msg_type)
{
    switch (msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_MESH_DATA:
    case MSG_ROUTE_SOLICIT:
        return true;
    default:
        return false;
    }
}

bool app_mesh_c5_route_capture_yields_to_competing_request(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REQ;
}

bool app_mesh_c5_route_capture_requires_ack_hold(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REPLY;
}

bool app_mesh_c5_route_capture_requires_inline_timing_install(
    uint8_t msg_type,
    bool timing_negotiation_active)
{
    return timing_negotiation_active && msg_type == MSG_MESH_EVENT_ACCEPT;
}

bool app_mesh_c5_route_capture_requires_post_rx_response(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE;
}

bool app_mesh_c5_route_capture_receive_aborted(int receive_ret)
{
    return receive_ret == -ECANCELED;
}

bool app_mesh_c5_gateway_control_origin_ttl(uint8_t msg_type,
                                            uint16_t command_id,
                                            uint8_t *origin_ttl)
{
    uint8_t ttl;

    if (origin_ttl == NULL) {
        return false;
    }

    switch (msg_type) {
    case MSG_COMMAND:
        ttl = gateway_command_origin_ttl((enum command_id)command_id);
        break;
    default:
        return false;
    }

    *origin_ttl = ttl;
    return true;
}

bool app_mesh_c5_gateway_operation_outranks_unaccepted_click(
    uint8_t msg_type,
    const uint8_t *payload,
    size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;

    /* This classifies which control must be durably queued before the listener
     * changes radio owners. It does not authorize discarding a valid click
     * claim; once queued, these operations can resume after click handoff. */
    if (msg_type == MSG_GATEWAY_ROUTE_ADV) {
        return true;
    }
    if (msg_type != MSG_COMMAND ||
        gateway_command_extract_id(payload, payload_len, &command_id) !=
            PROTO_OK) {
        return false;
    }

    switch (command_id) {
    case CMD_ASSIGN_DISCOVERY_SLOTS:
    case CMD_SURVEY_START:
    case CMD_SURVEY_PLAN:
    case CMD_SURVEY_CANCEL:
        return true;
    default:
        return false;
    }
}

bool app_mesh_c5_event_accept_reservation(
    const struct mesh_event_timing *accepted,
    uint16_t realign_slop_ms,
    struct mesh_event_timing *reservation)
{
    uint32_t expanded_guard_ms;
    uint32_t reserved_ms;

    if (accepted == NULL || reservation == NULL ||
        accepted->event_interval_ms == 0u ||
        accepted->event_window_ms == 0u || accepted->guard_ms == 0u) {
        return false;
    }

    expanded_guard_ms = (uint32_t)accepted->guard_ms + realign_slop_ms;
    reserved_ms = (uint32_t)accepted->event_window_ms +
                  (2u * expanded_guard_ms);
    if (expanded_guard_ms > UINT16_MAX ||
        reserved_ms >= accepted->event_interval_ms) {
        return false;
    }

    *reservation = *accepted;
    reservation->guard_ms = (uint16_t)expanded_guard_ms;
    return true;
}

bool app_mesh_c5_control_uses_extended_phr(uint8_t msg_type,
                                           size_t frame_len,
                                           size_t standard_frame_max_len)
{
    /*
     * Every mesh packet on channel 5 uses the one extended-PHR control PHY.
     * Only the clicker-facing UWB frames (wake claim, discovery, schedule,
     * DS-TWR) use the standard-PHR wake PHY, so a receiver waiting for a
     * hop or gateway ACK never has to guess which PHY the reply will use.
     */
    (void)msg_type;
    (void)frame_len;
    (void)standard_frame_max_len;
    return true;
}

bool app_mesh_c5_wake_claim_preempts_mesh(uint8_t claim_flags)
{
    return (claim_flags & FLAG_COUNT_AS_CLICK) != 0u ||
           (claim_flags & FLAG_RANGE_ONLY) == 0u;
}

bool app_mesh_c5_wake_claim_requires_anchor_handoff(uint8_t claim_flags,
                                                    bool local_can_range_clicks)
{
    return local_can_range_clicks &&
           app_mesh_c5_wake_claim_preempts_mesh(claim_flags);
}

bool app_mesh_c5_route_wake_claim_allowed(
    uint64_t source_id,
    uint64_t gateway_id,
    uint8_t claim_flags,
    bool require_relayed_route_req,
    bool require_relayed_gateway_control)
{
    if (app_mesh_c5_wake_claim_preempts_mesh(claim_flags)) {
        return true;
    }

    return source_id != gateway_id ||
           (!require_relayed_route_req &&
            !require_relayed_gateway_control);
}

bool app_mesh_c5_route_wake_should_listen(
    uint64_t source_id,
    uint64_t gateway_id,
    uint8_t claim_flags,
    bool require_relayed_route_req,
    bool require_relayed_gateway_control)
{
    if (app_mesh_c5_route_wake_claim_allowed(source_id,
                                            gateway_id,
                                            claim_flags,
                                            require_relayed_route_req,
                                            require_relayed_gateway_control)) {
        return true;
    }

    /*
     * A forced child must still enter the follow-up listener after a
     * rejected direct gateway control wake. The listener rejects the
     * hop-0 payload and waits for the relayed copy; leaving Channel 5
     * after the gateway train expires before that copy can arrive.
     */
    return require_relayed_gateway_control &&
           source_id == gateway_id &&
           app_mesh_c5_wake_followup_is_control(claim_flags);
}

bool app_mesh_c5_connected_gap_route_handoff_required(
    uint64_t source_id,
    uint64_t gateway_id,
    uint8_t claim_flags,
    bool require_relayed_route_req,
    bool require_relayed_gateway_control)
{
    /*
     * The connected-gap receiver uses the standard wake PHY.  Every allowed
     * non-click wake announces an extended-PHR payload next, including
     * EVENT_PROPOSE timing repair wakes which deliberately do not carry the
     * CONTROL_FOLLOWUP bit.  Hand those wakes to the extended listener before
     * continuing the standard-PHR scan.
     */
    return app_mesh_c5_wake_followup_uses_extended_phr(claim_flags) &&
           app_mesh_c5_route_wake_claim_allowed(
               source_id,
               gateway_id,
               claim_flags,
               require_relayed_route_req,
               require_relayed_gateway_control);
}

bool app_mesh_c5_gateway_control_copy_allowed(
    uint64_t source_id,
    uint64_t previous_hop_id,
    uint64_t gateway_id,
    bool require_relayed_gateway_control)
{
    return !require_relayed_gateway_control ||
           source_id != gateway_id ||
           previous_hop_id != gateway_id;
}

bool app_mesh_c5_gateway_control_rx_allowed(
    uint8_t msg_type,
    uint8_t packet_ttl,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t source_id,
    uint64_t previous_hop_id,
    uint64_t gateway_id,
    bool require_relayed_gateway_control,
    uint8_t required_gateway_relay_hops)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    uint8_t origin_ttl = 0u;

    if (!app_mesh_c5_gateway_control_copy_allowed(source_id,
                                                  previous_hop_id,
                                                  gateway_id,
                                                  require_relayed_gateway_control)) {
        return false;
    }
    if (required_gateway_relay_hops == 0u) {
        return true;
    }
    if (msg_type == MSG_COMMAND &&
        gateway_command_extract_id(payload, payload_len, &command_id) !=
            PROTO_OK) {
        return false;
    }
    if (!app_mesh_c5_gateway_control_origin_ttl(msg_type,
                                               (uint16_t)command_id,
                                               &origin_ttl)) {
        return false;
    }
    return app_mesh_gateway_control_relay_hops_allowed(
        origin_ttl, packet_ttl, required_gateway_relay_hops);
}

bool app_mesh_c5_gateway_route_adv_rx_allowed(
    uint8_t packet_ttl,
    uint8_t required_gateway_relay_hops)
{
    if (required_gateway_relay_hops == 0u) {
        return true;
    }

    return app_mesh_gateway_control_relay_hops_allowed(
        FLOOD_EPOCH_GLOBAL_TTL,
        packet_ttl,
        required_gateway_relay_hops);
}

bool app_mesh_c5_control_route_hint_is_first(
    struct app_mesh_c5_control_route_history *history,
    const struct app_mesh_c5_control_route_identity *identity)
{
    uint8_t index;

    if (history == NULL || identity == NULL) {
        return false;
    }

    for (index = 0u; index < APP_MESH_C5_CONTROL_ROUTE_HISTORY_SIZE; index++) {
        const struct app_mesh_c5_control_route_identity *entry =
            &history->entries[index];

        if ((history->valid_mask & (1u << index)) != 0u &&
            entry->route_epoch == identity->route_epoch &&
            entry->session_id == identity->session_id &&
            entry->seq == identity->seq &&
            entry->msg_type == identity->msg_type) {
            return false;
        }
    }

    index = history->next_index;
    history->entries[index] = *identity;
    history->valid_mask |= (uint8_t)(1u << index);
    history->next_index =
        (uint8_t)((index + 1u) % APP_MESH_C5_CONTROL_ROUTE_HISTORY_SIZE);
    return true;
}

bool app_mesh_c5_wake_followup_uses_extended_phr(uint8_t claim_flags)
{
    return !app_mesh_c5_wake_claim_preempts_mesh(claim_flags);
}

bool app_mesh_c5_wake_followup_is_control(uint8_t claim_flags)
{
    return (claim_flags & FLAG_CONTROL_FOLLOWUP) != 0u;
}

bool app_mesh_c5_control_followup_yields_to_ack(uint8_t claim_flags,
                                                bool ch9_ack_wait_active)
{
    return ch9_ack_wait_active &&
           app_mesh_c5_wake_followup_is_control(claim_flags) &&
           !app_mesh_c5_wake_claim_preempts_mesh(claim_flags);
}

bool app_mesh_c5_command_followup_holds_same_train_wake(
    uint8_t contact_purpose,
    bool click_priority)
{
    return contact_purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD &&
           !click_priority;
}

uint32_t app_mesh_c5_route_reply_listen_window_ms(
    uint8_t route_ttl,
    const struct app_mesh_c5_route_reply_window_timing *timing)
{
    uint64_t per_forward_hop_ms;
    uint64_t total_ms;
    uint32_t post_wake_route_ms;
    uint8_t bounded_ttl;

    if (timing == NULL) {
        return 0u;
    }

    post_wake_route_ms = timing->post_wake_route_rx_ms;
    if (post_wake_route_ms < timing->wake_to_route_delay_ms) {
        post_wake_route_ms = timing->wake_to_route_delay_ms;
    }

    bounded_ttl = route_ttl == 0u ? 1u : route_ttl;
    total_ms = (uint64_t)post_wake_route_ms +
               RREP_RESPONDER_JITTER_MAX_MS +
               timing->route_reply_exchange_ms +
               timing->guard_ms;
    per_forward_hop_ms = (uint64_t)timing->wake_train_ms +
                         post_wake_route_ms +
                         timing->request_flood_burst_ms +
                         timing->flood_forward_wave_ms +
                         timing->route_reply_exchange_ms +
                         timing->guard_ms;
    total_ms += (uint64_t)(bounded_ttl - 1u) * per_forward_hop_ms;
    if (total_ms < timing->base_reply_window_ms) {
        return timing->base_reply_window_ms;
    }
    return total_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)total_ms;
}

uint32_t app_mesh_c5_route_adv_response_delay_ms(
    uint16_t wake_train_ends_in_ms,
    bool embedded_route_frame,
    const struct app_mesh_c5_route_adv_timing *timing)
{
    if (timing == NULL) {
        return wake_train_ends_in_ms;
    }

    if (embedded_route_frame) {
        return (uint32_t)wake_train_ends_in_ms +
               timing->embedded_reply_guard_ms;
    }

    return (uint32_t)wake_train_ends_in_ms +
           timing->wake_to_route_delay_ms +
           timing->route_adv_reply_guard_ms;
}

uint32_t app_mesh_c5_connected_gap_window_ms(
    const struct app_mesh_c5_connected_gap_timing *timing)
{
    uint32_t available_ms;

    if (timing == NULL ||
        timing->next_channel9_delay_ms <= timing->retune_margin_ms) {
        return 0u;
    }

    available_ms = timing->next_channel9_delay_ms - timing->retune_margin_ms;
    if (available_ms < timing->min_scan_ms) {
        return 0u;
    }
    if (available_ms > timing->scan_cap_ms) {
        return timing->scan_cap_ms;
    }
    return available_ms;
}

uint32_t app_mesh_c5_connected_gap_reschedule_ms(
    uint32_t next_channel9_delay_ms,
    uint32_t min_scan_ms,
    uint32_t retune_margin_ms)
{
    uint32_t available_ms;

    if (next_channel9_delay_ms <= retune_margin_ms) {
        return next_channel9_delay_ms;
    }

    available_ms = next_channel9_delay_ms - retune_margin_ms;
    return available_ms >= min_scan_ms ? 0u : available_ms;
}

enum app_mesh_c5_connected_gap_rx_action
app_mesh_c5_connected_gap_rx_action(bool click_claim,
                                    bool route_control_claim,
                                    bool deadline_reached)
{
    if (click_claim) {
        return APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK;
    }
    if (route_control_claim) {
        return APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL;
    }
    if (deadline_reached) {
        return APP_MESH_C5_CONNECTED_GAP_RX_COMPLETE;
    }
    return APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE;
}
