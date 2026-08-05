#include "app_mesh_c5_priority.h"
#include "gateway_command.h"
#include "survey.h"

#include <assert.h>
#include <stdbool.h>

static void test_passive_gateway_preempt_defers_background_flood(void)
{
    const struct app_mesh_c5_flood_priority_state state = {
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_flood_should_defer(&state));
}

static void test_priority_response_bypasses_passive_gateway_preempt(void)
{
    const struct app_mesh_c5_flood_priority_state state = {
        .response_priority = true,
        .gateway_ch5_preempt = true,
    };

    assert(!app_mesh_c5_flood_should_defer(&state));
}

static void test_gateway_rx_yields_to_priority_response(void)
{
    const struct app_mesh_c5_flood_priority_state priority_state = {
        .response_priority = true,
        .gateway_ch5_preempt = true,
    };
    const struct app_mesh_c5_flood_priority_state background_state = {
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_gateway_rx_should_yield_to_response(&priority_state));
    assert(!app_mesh_c5_gateway_rx_should_yield_to_response(&background_state));
}

static void test_mesh_route_test_gateway_does_not_advertise_on_channel5(void)
{
    assert(!app_mesh_c5_gateway_route_adv_allowed(true));
    assert(app_mesh_c5_gateway_route_adv_allowed(false));
}

static void test_protected_anchor_work_still_defers_priority_response(void)
{
    const struct app_mesh_c5_flood_priority_state anchor_state = {
        .response_priority = true,
        .anchor_busy = true,
        .gateway_ch5_preempt = true,
    };
    const struct app_mesh_c5_flood_priority_state survey_state = {
        .response_priority = true,
        .survey_busy = true,
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_flood_should_defer(&anchor_state));
    assert(app_mesh_c5_flood_should_defer(&survey_state));
}

static void test_idle_state_does_not_defer(void)
{
    const struct app_mesh_c5_flood_priority_state state = {0};

    assert(!app_mesh_c5_flood_should_defer(&state));
}

static void test_gateway_route_adv_counts_as_route_capture(void)
{
    const struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_GATEWAY_ROUTE_ADV,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };

    assert(app_mesh_c5_route_capture_relevant(&state));
    assert(app_mesh_c5_route_capture_completes_discovery(state.msg_type));
    assert(!app_mesh_c5_route_capture_requires_ack_hold(state.msg_type));
}

static void test_unrelated_gateway_route_adv_is_ignored(void)
{
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_GATEWAY_ROUTE_ADV,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x8888888877776666ull,
        .local_id = 0x3333333333333301ull,
    };

    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.src_id = state.target_id;
    state.previous_hop_id = state.local_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_route_reply_and_event_control_capture_rules(void)
{
    const struct app_mesh_c5_route_capture_state route_reply = {
        .msg_type = MSG_ROUTE_REPLY,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x3333333333333301ull,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };
    const struct app_mesh_c5_route_capture_state event_control = {
        .msg_type = MSG_MESH_EVENT_PROPOSE,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x3333333333333301ull,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };
    const struct app_mesh_c5_route_capture_state route_request = {
        .msg_type = MSG_ROUTE_REQ,
        .src_id = 0x3333333333333301ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x3333333333333301ull,
        .target_id = 0x3333333333333301ull,
        .route_request_target_id = 0x3333333333333301ull,
        .local_id = 0x2222222222222301ull,
    };

    assert(app_mesh_c5_route_capture_relevant(&route_reply));
    assert(app_mesh_c5_route_capture_completes_discovery(route_reply.msg_type));
    assert(app_mesh_c5_route_capture_requires_ack_hold(route_reply.msg_type));
    assert(app_mesh_c5_route_capture_relevant(&event_control));
    assert(!app_mesh_c5_route_capture_completes_discovery(event_control.msg_type));
    assert(!app_mesh_c5_route_capture_requires_ack_hold(event_control.msg_type));
    assert(app_mesh_c5_route_capture_requires_inline_timing_install(
        MSG_MESH_EVENT_ACCEPT,
        true));
    assert(!app_mesh_c5_route_capture_requires_inline_timing_install(
        MSG_MESH_EVENT_PROPOSE,
        true));
    assert(!app_mesh_c5_route_capture_requires_inline_timing_install(
        MSG_MESH_EVENT_PROPOSE,
        false));
    assert(app_mesh_c5_route_capture_requires_post_rx_response(
        MSG_MESH_EVENT_PROPOSE));
    assert(!app_mesh_c5_route_capture_requires_post_rx_response(
        MSG_MESH_EVENT_ACCEPT));
    assert(!app_mesh_c5_route_capture_requires_inline_timing_install(
        MSG_MESH_EVENT_ACCEPT,
        false));
    assert(app_mesh_c5_route_capture_relevant(&route_request));
    assert(!app_mesh_c5_route_capture_completes_discovery(
        route_request.msg_type));
    assert(!app_mesh_c5_route_capture_requires_ack_hold(
        route_request.msg_type));
}

static void test_competing_route_request_yields_without_false_route_success(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t local_anchor_id = 0x2222222222222301ull;
    const uint64_t direct_origin_id = 0x3333333333333301ull;
    const uint64_t forwarded_origin_id = 0x4444444444444401ull;
    const uint64_t forwarding_anchor_id = 0x5555555555555501ull;
    const uint64_t wrong_target_id = 0x8888888877776666ull;
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_ROUTE_REQ,
        .src_id = direct_origin_id,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = direct_origin_id,
        .target_id = gateway_id,
        .route_request_target_id = gateway_id,
        .local_id = local_anchor_id,
    };

    /* A neighbor competing for the same gateway must release RX ownership. */
    assert(app_mesh_c5_route_capture_relevant(&state));
    assert(!app_mesh_c5_route_capture_completes_discovery(state.msg_type));
    assert(app_mesh_c5_route_capture_yields_to_competing_request(
        state.msg_type));
    assert(!app_mesh_c5_route_capture_yields_to_competing_request(
        MSG_ROUTE_REPLY));

    /* Several-hop requests retain the origin while the previous hop changes. */
    state.src_id = forwarded_origin_id;
    state.previous_hop_id = forwarding_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));

    /*
     * A child's route-solicit wake makes the listener target the child, while
     * the decoded request still asks for the gateway or this local responder.
     */
    state.src_id = direct_origin_id;
    state.previous_hop_id = direct_origin_id;
    state.target_id = direct_origin_id;
    state.control_origin_id = gateway_id;
    state.route_request_target_id = gateway_id;
    assert(app_mesh_c5_route_capture_relevant(&state));
    state.route_request_target_id = local_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));

    state.route_request_target_id = wrong_target_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.route_request_target_id = gateway_id;
    state.target_id = gateway_id;
    state.control_origin_id = 0u;

    state.src_id = local_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.src_id = forwarded_origin_id;

    state.previous_hop_id = local_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = 0u;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = MESH_BROADCAST_ID;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = forwarding_anchor_id;

    state.dst_id = local_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_control_wake_captures_gateway_broadcast_command(void)
{
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_COMMAND,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
        .control_followup = true,
    };

    assert(app_mesh_c5_route_capture_relevant(&state));
    state.control_followup = false;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.gateway_control_priority = true;
    assert(app_mesh_c5_route_capture_relevant(&state));
    state.gateway_control_priority = false;
    state.control_followup = true;
    state.previous_hop_id = state.local_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_control_wake_captures_survey_discovery_like_enumeration(void)
{
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
        .control_followup = true,
    };

    assert(app_mesh_c5_route_capture_relevant(&state));
    state.control_followup = false;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.control_followup = true;
    state.dst_id = state.local_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_forced_hop_anchor_requires_relayed_gateway_control(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t local_anchor_id = 0x3333333333333301ull;
    const uint64_t relay_anchor_id = 0x2222222222222301ull;
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .src_id = gateway_id,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = gateway_id,
        .target_id = gateway_id,
        .local_id = local_anchor_id,
        .control_followup = true,
        .require_relayed_gateway_control = true,
    };

    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = relay_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));

    state.msg_type = MSG_SURVEY_PAIR_PREPARE;
    state.dst_id = local_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = gateway_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_control_wake_captures_targeted_survey_pair_prepare(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t local_anchor_id = 0x3333333333333301ull;
    const uint64_t relay_anchor_id = 0x2222222222222301ull;
    const uint64_t wrong_anchor_id = 0x1111111111111101ull;
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_SURVEY_PAIR_PREPARE,
        .src_id = gateway_id,
        .dst_id = local_anchor_id,
        .previous_hop_id = gateway_id,
        .target_id = gateway_id,
        .local_id = local_anchor_id,
        .control_origin_id = gateway_id,
        .control_followup = true,
    };

    /* Direct delivery: this anchor is the final survey-pair target. */
    assert(app_mesh_c5_route_capture_relevant(&state));

    /* Relayed delivery keeps the gateway source and local final destination. */
    state.previous_hop_id = relay_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));

    /* A relay-owned wake contact still carries the gateway's control origin. */
    state.target_id = relay_anchor_id;
    assert(app_mesh_c5_route_capture_relevant(&state));
    state.src_id = relay_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.src_id = gateway_id;
    state.target_id = gateway_id;
    state.previous_hop_id = gateway_id;

    state.control_followup = false;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.control_followup = true;

    state.dst_id = wrong_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.dst_id = MESH_BROADCAST_ID;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.dst_id = local_anchor_id;

    state.src_id = local_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.src_id = gateway_id;

    state.previous_hop_id = local_anchor_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = gateway_id;

    state.msg_type = MSG_SURVEY_PAIR_RESULT;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_targeted_gateway_control_requires_validated_downlink_to_relay(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t local_relay_id = 0x2222222222222301ull;
    const uint64_t downstream_target_id = 0x3333333333333301ull;
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_SURVEY_PAIR_PREPARE,
        .src_id = gateway_id,
        .dst_id = downstream_target_id,
        .previous_hop_id = gateway_id,
        .target_id = gateway_id,
        .local_id = local_relay_id,
        .gateway_control_priority = true,
    };

    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.targeted_control_relay = true;
    assert(app_mesh_c5_route_capture_relevant(&state));

    state.msg_type = MSG_COMMAND;
    assert(app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = local_relay_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.previous_hop_id = MESH_BROADCAST_ID;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_route_reply_capture_requires_exact_discovery_identity(void)
{
    struct app_mesh_c5_route_capture_state route_reply = {
        .msg_type = MSG_ROUTE_REPLY,
        .session_id = 0x10203040u,
        .flood_epoch_id = 0x50607080u,
        .reply_nonce = 0x3344u,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x3333333333333301ull,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
        .expected_session_id = 0x10203040u,
        .expected_flood_epoch_id = 0x50607080u,
        .expected_reply_nonce = 0x3344u,
        .route_identity_required = true,
    };

    assert(app_mesh_c5_route_capture_relevant(&route_reply));

    route_reply.session_id--;
    assert(!app_mesh_c5_route_capture_relevant(&route_reply));
    route_reply.session_id = route_reply.expected_session_id;

    route_reply.flood_epoch_id--;
    assert(!app_mesh_c5_route_capture_relevant(&route_reply));
    route_reply.flood_epoch_id = route_reply.expected_flood_epoch_id;

    route_reply.reply_nonce--;
    assert(!app_mesh_c5_route_capture_relevant(&route_reply));
    route_reply.reply_nonce = route_reply.expected_reply_nonce;

    route_reply.expected_session_id = 0u;
    assert(!app_mesh_c5_route_capture_relevant(&route_reply));
}

static void test_event_accept_reservation_covers_bounded_realign(void)
{
    const struct mesh_event_timing accepted = {
        .event_interval_ms = 440u,
        .event_window_ms = 120u,
        .next_event_time_ms = UINT32_MAX - 4u,
        .guard_ms = 30u,
    };
    struct mesh_event_timing reservation;
    struct mesh_event_timing realigned;

    assert(app_mesh_c5_event_accept_reservation(&accepted, 20u, &reservation));
    assert(reservation.guard_ms == 50u);
    assert(reservation.next_event_time_ms == accepted.next_event_time_ms);

    realigned = accepted;
    realigned.next_event_time_ms += 17u;
    assert(app_mesh_c5_event_accept_realign_is_reserved(&reservation,
                                                         &realigned,
                                                         20u));
    realigned.next_event_time_ms++;
    assert(app_mesh_c5_event_accept_realign_is_reserved(&reservation,
                                                         &realigned,
                                                         20u));

    realigned = accepted;
    realigned.next_event_time_ms -= 20u;
    assert(app_mesh_c5_event_accept_realign_is_reserved(&reservation,
                                                         &realigned,
                                                         20u));
    realigned = accepted;
    realigned.next_event_time_ms += 21u;
    assert(!app_mesh_c5_event_accept_realign_is_reserved(&reservation,
                                                          &realigned,
                                                          20u));
    assert(!app_mesh_c5_event_accept_reservation(&accepted, 200u, &reservation));
}

static void test_channel5_control_phr_policy(void)
{
    assert(app_mesh_c5_control_uses_extended_phr(MSG_COMMAND, 117u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_SURVEY_DISCOVERY_START,
                                                 117u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_SURVEY_PAIR_PREPARE,
                                                 91u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REQ, 146u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REQ, 95u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REPLY, 95u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_GATEWAY_ROUTE_ADV,
                                                 114u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_MESH_EVENT_PROPOSE,
                                                 103u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_MESH_EVENT_ACCEPT,
                                                 103u,
                                                 125u));
    assert(!app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REPLY_ACK,
                                                  95u,
                                                  125u));
}

static void test_gateway_control_origin_ttl_matches_command_profile(void)
{
    uint8_t origin_ttl = 0u;

    assert(app_mesh_c5_gateway_control_origin_ttl(
        MSG_SURVEY_PAIR_PREPARE, CMD_VENDOR_BASE, &origin_ttl));
    assert(origin_ttl == SURVEY_DEFAULT_TTL);
    assert(app_mesh_c5_gateway_control_origin_ttl(
        MSG_SURVEY_DISCOVERY_START, CMD_VENDOR_BASE, &origin_ttl));
    assert(origin_ttl == SURVEY_DEFAULT_TTL);

    assert(app_mesh_c5_gateway_control_origin_ttl(
        MSG_COMMAND, CMD_SURVEY_START_PAIR, &origin_ttl));
    assert(origin_ttl == MESH_DEFAULT_TTL);
    assert(app_mesh_c5_gateway_control_origin_ttl(
        MSG_COMMAND, CMD_SURVEY_ABORT, &origin_ttl));
    assert(origin_ttl == MESH_DEFAULT_TTL);
    assert(app_mesh_c5_gateway_control_origin_ttl(
        MSG_COMMAND, CMD_ASSIGN_DISCOVERY_SLOTS, &origin_ttl));
    assert(origin_ttl == FLOOD_EPOCH_GLOBAL_TTL);

    assert(!app_mesh_c5_gateway_control_origin_ttl(
        MSG_SURVEY_PAIR_RESULT, CMD_VENDOR_BASE, &origin_ttl));
    assert(!app_mesh_c5_gateway_control_origin_ttl(
        MSG_COMMAND, CMD_VENDOR_BASE, NULL));
}

static void test_gateway_control_followup_tx_rx_phr_symmetry(void)
{
    static const uint8_t control_types[] = {
        MSG_COMMAND,
        MSG_SURVEY_DISCOVERY_START,
        MSG_SURVEY_PAIR_PREPARE,
    };
    static const size_t frame_lengths[] = {91u, 124u, 125u, 126u};
    const uint8_t wake_claim_flags =
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    const bool receiver_uses_extended_phr =
        app_mesh_c5_wake_followup_uses_extended_phr(wake_claim_flags);

    assert(receiver_uses_extended_phr);
    for (size_t type = 0u;
         type < sizeof(control_types) / sizeof(control_types[0]);
         type++) {
        for (size_t length = 0u;
             length < sizeof(frame_lengths) / sizeof(frame_lengths[0]);
             length++) {
            assert(app_mesh_c5_control_uses_extended_phr(
                       control_types[type], frame_lengths[length], 125u) ==
                   receiver_uses_extended_phr);
        }
    }
}

static void test_wake_claim_click_priority_policy(void)
{
    assert(!app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC |
                                                 FLAG_RANGE_ONLY));
    assert(!app_mesh_c5_wake_claim_preempts_mesh(FLAG_ROUTE_SETUP |
                                                 FLAG_DIAGNOSTIC |
                                                 FLAG_RANGE_ONLY));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC));
    assert(app_mesh_c5_wake_claim_preempts_mesh(0u));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_COUNT_AS_CLICK));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC |
                                                FLAG_COUNT_AS_CLICK));

    assert(app_mesh_c5_wake_claim_requires_anchor_handoff(0u, true));
    assert(app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_COUNT_AS_CLICK, true));
    assert(!app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY, true));
    assert(!app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY, true));
    assert(!app_mesh_c5_wake_claim_requires_anchor_handoff(0u, false));
    assert(!app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_COUNT_AS_CLICK, false));

    assert(!app_mesh_c5_wake_followup_uses_extended_phr(0u));
    assert(!app_mesh_c5_wake_followup_uses_extended_phr(FLAG_COUNT_AS_CLICK));
    assert(app_mesh_c5_wake_followup_uses_extended_phr(
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY));
    assert(!app_mesh_c5_wake_followup_is_control(
        FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY));
    assert(app_mesh_c5_wake_followup_is_control(
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY));
}

static void test_forced_hop_anchor_ignores_direct_gateway_route_wake(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t relay_anchor_id = 0x2222222222222301ull;
    const uint8_t control_wake_flags =
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;

    assert(app_mesh_c5_route_wake_claim_allowed(gateway_id,
                                                 gateway_id,
                                                 control_wake_flags,
                                                 false,
                                                 false));
    assert(!app_mesh_c5_route_wake_claim_allowed(gateway_id,
                                                  gateway_id,
                                                  control_wake_flags,
                                                  true,
                                                  false));
    assert(!app_mesh_c5_route_wake_claim_allowed(gateway_id,
                                                  gateway_id,
                                                  control_wake_flags,
                                                  false,
                                                  true));

    assert(app_mesh_c5_route_wake_claim_allowed(relay_anchor_id,
                                                 gateway_id,
                                                 control_wake_flags,
                                                 true,
                                                 true));
    assert(app_mesh_c5_route_wake_claim_allowed(gateway_id,
                                                 gateway_id,
                                                 FLAG_COUNT_AS_CLICK,
                                                 true,
                                                 true));
}

static void test_gateway_control_route_hint_uses_first_transport_copy(void)
{
    struct app_mesh_c5_control_route_history history = {0};
    const struct app_mesh_c5_control_route_identity direct_first = {
        .route_epoch = 0x10203040u,
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .session_id = 0x50607080u,
        .seq = 0x1122u,
    };
    /*
     * The physical previous hop is deliberately absent from the identity.
     * A relayed echo of the same gateway packet must retain the route learned
     * from the direct first copy instead of creating an alternate parent.
     */
    const struct app_mesh_c5_control_route_identity relayed_echo = direct_first;
    struct app_mesh_c5_control_route_identity new_sequence = direct_first;
    struct app_mesh_c5_control_route_identity new_epoch = direct_first;
    const struct app_mesh_c5_control_route_identity relayed_first = {
        .route_epoch = 0x10203041u,
        .msg_type = MSG_COMMAND,
        .session_id = 0x90a0b0c0u,
        .seq = 0x3344u,
    };

    assert(app_mesh_c5_control_route_hint_is_first(&history, &direct_first));
    assert(!app_mesh_c5_control_route_hint_is_first(&history, &relayed_echo));

    new_sequence.seq++;
    assert(app_mesh_c5_control_route_hint_is_first(&history, &new_sequence));

    new_epoch.route_epoch++;
    assert(app_mesh_c5_control_route_hint_is_first(&history, &new_epoch));

    assert(app_mesh_c5_control_route_hint_is_first(&history, &relayed_first));
    assert(!app_mesh_c5_control_route_hint_is_first(&history, &relayed_first));
}

static void test_connected_gap_stays_armed_until_deadline_or_click(void)
{
    assert(app_mesh_c5_connected_gap_rx_action(false, false, false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE);
    assert(app_mesh_c5_connected_gap_rx_action(false, false, true) ==
           APP_MESH_C5_CONNECTED_GAP_RX_COMPLETE);
    assert(app_mesh_c5_connected_gap_rx_action(true, false, false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK);
    assert(app_mesh_c5_connected_gap_rx_action(true, false, true) ==
           APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK);
}

static void test_connected_gap_hands_allowed_control_to_extended_follower(void)
{
    const uint64_t gateway_id = 0x9999888877776666ull;
    const uint64_t relay_anchor_id = 0x2222222222222301ull;
    const uint8_t control_followup_flags =
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    bool route_control_claim;

    assert(app_mesh_c5_wake_followup_is_control(control_followup_flags));

    route_control_claim = app_mesh_c5_route_wake_claim_allowed(
        gateway_id,
        gateway_id,
        control_followup_flags,
        false,
        false);
    assert(route_control_claim);
    assert(app_mesh_c5_connected_gap_rx_action(false,
                                                route_control_claim,
                                                false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL);

    /* The forced-hop bench ignores direct gateway control and keeps RX armed. */
    route_control_claim = app_mesh_c5_route_wake_claim_allowed(
        gateway_id,
        gateway_id,
        control_followup_flags,
        false,
        true);
    assert(!route_control_claim);
    assert(app_mesh_c5_connected_gap_rx_action(false,
                                                route_control_claim,
                                                false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE);

    /* The same control arriving through a relay owns the extended follower. */
    route_control_claim = app_mesh_c5_route_wake_claim_allowed(
        relay_anchor_id,
        gateway_id,
        control_followup_flags,
        false,
        true);
    assert(route_control_claim);
    assert(app_mesh_c5_connected_gap_rx_action(false,
                                                route_control_claim,
                                                false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL);
}

static void test_route_adv_delay_targets_requester_reply_window(void)
{
    const struct app_mesh_c5_route_adv_timing timing = {
        .wake_to_route_delay_ms = 40u,
        .request_flood_burst_ms = 600u,
        .embedded_reply_guard_ms = 5u,
        .route_adv_reply_guard_ms = 20u,
    };

    assert(app_mesh_c5_route_adv_response_delay_ms(315u, false, &timing) ==
           375u);
    assert(app_mesh_c5_route_adv_response_delay_ms(315u, true, &timing) ==
           320u);
}

static void test_route_reply_window_covers_direct_probe_and_reply_exchange(void)
{
    const struct app_mesh_c5_route_reply_window_timing timing = {
        .base_reply_window_ms = 1000u,
        .wake_train_ms = 400u,
        .post_wake_route_rx_ms = 1250u,
        .wake_to_route_delay_ms = 40u,
        .request_flood_burst_ms = 600u,
        .flood_forward_wave_ms = 1400u,
        .route_reply_exchange_ms = 1375u,
        .direct_gateway_probe_ms = 200u,
        .guard_ms = 250u,
    };
    uint32_t ttl1_window =
        app_mesh_c5_route_reply_listen_window_ms(1u, &timing);
    uint32_t ttl2_window =
        app_mesh_c5_route_reply_listen_window_ms(2u, &timing);
    uint32_t ttl4_window =
        app_mesh_c5_route_reply_listen_window_ms(4u, &timing);
    uint32_t ttl8_window =
        app_mesh_c5_route_reply_listen_window_ms(8u, &timing);

    assert(ttl1_window == 3050u);
    assert(ttl2_window == 8325u);
    assert(ttl4_window == 18875u);
    assert(ttl8_window == 39975u);
    assert(ttl1_window > timing.base_reply_window_ms);
    assert(ttl1_window < ttl2_window);
    assert(ttl2_window < ttl4_window);
    assert(ttl4_window < ttl8_window);
}

static void test_connected_gap_window_uses_channel5_until_retune_guard(void)
{
    const struct app_mesh_c5_connected_gap_timing long_gap = {
        .next_channel9_delay_ms = 220u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };
    const struct app_mesh_c5_connected_gap_timing short_gap = {
        .next_channel9_delay_ms = 45u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };
    const struct app_mesh_c5_connected_gap_timing capped_gap = {
        .next_channel9_delay_ms = 75u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };

    assert(app_mesh_c5_connected_gap_window_ms(&long_gap) == 100u);
    assert(app_mesh_c5_connected_gap_window_ms(&short_gap) == 0u);
    assert(app_mesh_c5_connected_gap_window_ms(&capped_gap) == 45u);
}

static void test_connected_gap_reschedules_immediate_c5_until_ch9_is_close(void)
{
    assert(app_mesh_c5_connected_gap_reschedule_ms(220u, 20u, 30u) == 0u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(45u, 20u, 30u) == 15u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(30u, 20u, 30u) == 30u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(0u, 20u, 30u) == 0u);
}

static void test_contact_expiry_uses_state_across_uptime_wrap(void)
{
    struct c5_contact_context contact = {
        .peer_id = 0x1111222233334444ull,
        .purpose = C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
        .accepted = true,
        .expires_at_ms = 0u,
        .state = C5_CONTACT_EXCHANGE_ACTIVE,
    };

    assert(!app_mesh_c5_contact_expired(&contact, UINT32_MAX));
    assert(app_mesh_c5_contact_accepted(
        &contact,
        contact.peer_id,
        C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
        UINT32_MAX));
    assert(!app_mesh_c5_contact_accepted(
        &contact,
        contact.peer_id,
        C5_CONTACT_PURPOSE_ROUTE_REPLY,
        UINT32_MAX));

    assert(app_mesh_c5_contact_expired(&contact, 0u));
    assert(!app_mesh_c5_contact_accepted(
        &contact,
        contact.peer_id,
        C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
        0u));

    contact.state = C5_CONTACT_NONE;
    assert(!app_mesh_c5_contact_expired(&contact, 0u));
    contact.state = C5_CONTACT_AWAKE_ACCEPTED;
    contact.accepted = false;
    assert(!app_mesh_c5_contact_accepted(
        &contact,
        contact.peer_id,
        C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
        UINT32_MAX));
}

int main(void)
{
    test_passive_gateway_preempt_defers_background_flood();
    test_priority_response_bypasses_passive_gateway_preempt();
    test_gateway_rx_yields_to_priority_response();
    test_mesh_route_test_gateway_does_not_advertise_on_channel5();
    test_protected_anchor_work_still_defers_priority_response();
    test_idle_state_does_not_defer();
    test_gateway_route_adv_counts_as_route_capture();
    test_unrelated_gateway_route_adv_is_ignored();
    test_route_reply_and_event_control_capture_rules();
    test_competing_route_request_yields_without_false_route_success();
    test_control_wake_captures_gateway_broadcast_command();
    test_control_wake_captures_survey_discovery_like_enumeration();
    test_forced_hop_anchor_requires_relayed_gateway_control();
    test_control_wake_captures_targeted_survey_pair_prepare();
    test_targeted_gateway_control_requires_validated_downlink_to_relay();
    test_route_reply_capture_requires_exact_discovery_identity();
    test_event_accept_reservation_covers_bounded_realign();
    test_channel5_control_phr_policy();
    test_gateway_control_origin_ttl_matches_command_profile();
    test_gateway_control_followup_tx_rx_phr_symmetry();
    test_wake_claim_click_priority_policy();
    test_forced_hop_anchor_ignores_direct_gateway_route_wake();
    test_gateway_control_route_hint_uses_first_transport_copy();
    test_connected_gap_stays_armed_until_deadline_or_click();
    test_connected_gap_hands_allowed_control_to_extended_follower();
    test_route_adv_delay_targets_requester_reply_window();
    test_route_reply_window_covers_direct_probe_and_reply_exchange();
    test_connected_gap_window_uses_channel5_until_retune_guard();
    test_connected_gap_reschedules_immediate_c5_until_ch9_is_close();
    test_contact_expiry_uses_state_across_uptime_wrap();
    return 0;
}
