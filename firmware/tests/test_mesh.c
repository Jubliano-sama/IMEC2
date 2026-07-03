#include "mesh.h"

#include <assert.h>

static void test_gateway_ack_is_end_to_end(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};
    struct mesh_event_timing parsed = {0};

    assert(mesh_append_requested_seq(payload, sizeof(payload), &payload_len, 101u) == PROTO_OK);
    assert(mesh_init_gateway_ack(&packet,
                                      0x9999888877776666ull,
                                      0x1111222233334444ull,
                                      0x12345678u,
                                      12u,
                                      (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_GATEWAY_ACK);
    assert((packet.flags & FLAG_GATEWAY_ACK) != 0u);
    assert(packet.src_id == 0x9999888877776666ull);
    assert(packet.dst_id == 0x1111222233334444ull);
    assert(packet.ttl == MESH_GATEWAY_ACK_TTL);
}

static void test_command_and_result_are_acknowledged_not_clicks(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet command = {0};
    struct proto_packet result = {0};

    assert(mesh_append_command_id(payload, sizeof(payload), &payload_len, CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                                  0x9999888877776666ull,
                                  0x1111222233334444ull,
                                  0x12345678u,
                                  1u,
                                  (uint8_t)payload_len) == PROTO_OK);

    assert(command.msg_type == MSG_COMMAND);
    assert(command.flags == 0u);
    assert((command.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u);
    assert((command.flags & FLAG_COUNT_AS_CLICK) == 0u);

    payload_len = 0u;
    assert(mesh_append_command_result(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           CMD_GET_STATUS,
                                           COMMAND_OK,
                                           0u) == PROTO_OK);
    assert(mesh_init_command_result(&result,
                                         0x1111222233334444ull,
                                         0x9999888877776666ull,
                                         0x12345678u,
                                         2u,
                                         (uint8_t)payload_len,
                                         true) == PROTO_OK);

    assert(result.msg_type == MSG_COMMAND_RESULT);
    assert((result.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((result.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((result.flags & FLAG_COUNT_AS_CLICK) == 0u);

    assert(tlv_find(payload, payload_len, TLV_COMMAND_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == CMD_GET_STATUS);

    assert(tlv_find(payload, payload_len, TLV_COMMAND_STATUS, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == COMMAND_OK);
}

static void test_rejects_invalid_ids(void)
{
    struct proto_packet packet = {0};

    assert(mesh_init_command(&packet, 0u, 1u, 1u, 1u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_command(&packet, 1u, 1u, 1u, 1u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_gateway_ack(&packet, 1u, 2u, 0u, 1u, 0u) == PROTO_ERR_MALFORMED);
}

static void test_rejects_zero_sequence_numbers(void)
{
    struct proto_packet packet = {0};

    assert(mesh_init_gateway_ack(&packet, 1u, 2u, 1u, 0u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_command(&packet, 1u, 2u, 1u, 0u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_command_result(&packet, 1u, 2u, 1u, 0u, 0u, false) ==
           PROTO_ERR_MALFORMED);
}

static struct mesh_event_params event_params(void)
{
    const struct mesh_event_params params = {
        .event_interval_ms = 100u,
        .event_window_ms = 20u,
        .first_event_time_ms = 1000u,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 30,
        .max_missed_events = 2u,
        .supervision_timeout_ms = 300u,
    };
    return params;
}

static void test_channel9_timing_requires_channel5_contact(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing parsed = {0};
    struct mesh_event_params params = event_params();
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(mesh_event_timing_negotiate(&timing, &params, false) == PROTO_ERR_BUSY);
    assert(!mesh_event_timing_usable(&timing, 1000u));

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(timing.mesh_channel == MESH_EVENT_CHANNEL);
    assert(timing.mesh_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(timing.route_fresh);
    assert(timing.timing_fresh);
    assert(mesh_event_timing_usable(&timing, 1000u));

    assert(mesh_append_event_timing_tlvs(payload, sizeof(payload), &payload_len, &timing) ==
           PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_MESH_CHANNEL, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == UWB_CHANNEL_MESH_PAYLOAD);
    assert(tlv_find(payload, payload_len, TLV_MESH_EVENT_WINDOW_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == params.event_window_ms);
    assert(mesh_event_timing_from_tlvs(&parsed, payload, payload_len, false) ==
           PROTO_ERR_BUSY);
    assert(mesh_event_timing_from_tlvs(&parsed, payload, payload_len, true) == PROTO_OK);
    assert(parsed.mesh_channel == MESH_EVENT_CHANNEL);
    assert(parsed.event_interval_ms == timing.event_interval_ms);
    assert(parsed.event_window_ms == timing.event_window_ms);
    assert(parsed.next_event_time_ms == timing.next_event_time_ms);
    assert(parsed.guard_ms == timing.guard_ms);
    assert(parsed.peer_clock_skew_estimate_ppm == timing.peer_clock_skew_estimate_ppm);
    assert(parsed.max_missed_events == timing.max_missed_events);
    assert(parsed.supervision_timeout_ms == timing.supervision_timeout_ms);
    assert(mesh_event_timing_local_tx_slot(&timing));
    assert(!mesh_event_timing_local_rx_slot(&timing));

    assert(mesh_init_event_control(&packet,
                                   MSG_MESH_EVENT_PROPOSE,
                                   0x1111u,
                                   0x2222u,
                                   0x1234u,
                                   1u,
                                   (uint8_t)payload_len) == PROTO_OK);
    assert(packet.msg_type == MSG_MESH_EVENT_PROPOSE);
    assert(packet.flags == 0u);
    assert(packet.ttl == MESH_DEFAULT_TTL);
    assert(mesh_init_event_control(&packet,
                                   MSG_MESH_DATA,
                                   0x1111u,
                                   0x2222u,
                                   0x1234u,
                                   1u,
                                   0u) == PROTO_ERR_MALFORMED);
}

static void test_channel9_first_slot_direction_follows_initiator(void)
{
    struct mesh_event_timing initiator = {0};
    struct mesh_event_timing downstream = {0};
    struct mesh_event_params params = event_params();

    assert(mesh_event_timing_negotiate(&initiator, &params, true) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&downstream, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&initiator, true);
    mesh_event_timing_set_local_first_slot_tx(&downstream, false);

    assert(mesh_event_timing_local_tx_slot(&initiator));
    assert(mesh_event_timing_local_rx_slot(&downstream));
    assert(!mesh_event_timing_local_rx_slot(&initiator));
    assert(!mesh_event_timing_local_tx_slot(&downstream));

    mesh_event_note_success(&initiator, params.first_event_time_ms);
    mesh_event_note_observed_packet(&downstream,
                                    params.first_event_time_ms,
                                    params.first_event_time_ms + 1u);

    assert(mesh_event_timing_local_rx_slot(&initiator));
    assert(mesh_event_timing_local_tx_slot(&downstream));
}

static void test_channel9_timing_crosses_uptime_domains_as_relative_delay(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing parsed = {0};
    struct mesh_event_params params = event_params();
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    params.first_event_time_ms = 1010u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_append_event_timing_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            1000u) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_MESH_NEXT_EVENT_TIME_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == 10u);

    assert(mesh_event_timing_from_tlvs_at(&parsed, payload, payload_len, 4000u, true) ==
           PROTO_OK);
    assert(parsed.next_event_time_ms == 4010u);
    assert(parsed.event_interval_ms == timing.event_interval_ms);
    assert(parsed.event_window_ms == timing.event_window_ms);
}

static void test_channel9_event_planner_reserves_channel5_scan(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 1015u,
        .retune_guard_ms = 5u,
    };

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(&timing, &requirements, 1000u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_CLIP);
    assert(plan.start_ms == 1000u);
    assert(plan.end_ms == 1010u);
    assert(plan.window_ms == 10u);
    mesh_event_note_plan_action(&diagnostics, plan.action);
    assert(diagnostics.mesh_deferrals == 1u);
    assert(diagnostics.channel5_preemptions == 0u);

    requirements.next_required_scan_start_ms = 1003u;
    assert(mesh_event_plan_channel9(&timing, &requirements, 1000u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD);
    assert(plan.window_ms == 0u);
    mesh_event_note_plan_action(&diagnostics, plan.action);
    assert(diagnostics.mesh_deferrals == 2u);
    assert(diagnostics.channel5_preemptions == 1u);
}

static void test_channel9_event_planner_keeps_negotiated_window_when_late(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    const struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 0u,
        .retune_guard_ms = 5u,
    };

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(&timing, &requirements, 1012u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 1000u);
    assert(plan.end_ms == 1020u);
    assert(plan.window_ms == 20u);
}

static void test_channel9_observed_rx_keeps_negotiated_cadence(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_note_observed_packet(&timing, 1000u, 1005u);
    assert(timing.next_event_time_ms == 1100u);
    assert(timing.event_counter == 1u);
    assert(timing.missed_event_count == 0u);
    assert(mesh_event_timing_usable(&timing, 1005u));

    mesh_event_note_observed_packet(&timing, 1000u, 1010u);
    assert(timing.next_event_time_ms == 1100u);
    assert(timing.event_counter == 1u);
    assert(timing.missed_event_count == 0u);

    mesh_event_note_observed_packet(&timing, 1100u, 1118u);
    assert(timing.next_event_time_ms == 1200u);
    assert(timing.event_counter == 2u);
    assert(!timing.fallback_required);
    assert(mesh_event_timing_usable(&timing, 1118u));
}

static void test_channel5_activity_preempts_channel9_mesh(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    const struct mesh_channel5_requirements click_requirements = {
        .next_required_scan_start_ms = 1100u,
        .active_until_ms = 1030u,
        .retune_guard_ms = 5u,
        .click_epoch_active = true,
    };

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(&timing, &click_requirements, 1000u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE);
    mesh_event_note_plan_action(&diagnostics, plan.action);
    assert(diagnostics.mesh_deferrals == 1u);
    assert(diagnostics.channel5_preemptions == 1u);

    mesh_event_note_channel_switch(&diagnostics, false, true);
    assert(diagnostics.channel_switches == 1u);
    assert(diagnostics.pll_ready_failures == 1u);
    assert(diagnostics.late_channel5_returns == 1u);
}

static void test_channel5_active_until_zero_is_idle_across_uptime_wrap(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    const struct mesh_channel5_requirements requirements = {
        .active_until_ms = 0u,
        .retune_guard_ms = 5u,
    };

    params.first_event_time_ms = 0xfffffff0u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(&timing, &requirements, 0xfffffff0u, &plan) ==
           PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
}

static void test_channel9_missed_events_refresh_contact_at_configured_limit(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    const struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 0u,
        .retune_guard_ms = 5u,
    };

    params.supervision_timeout_ms = 500u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_note_success(&timing, 1000u);
    assert(timing.event_counter == 1u);
    assert(timing.next_event_time_ms == 1100u);
    assert(mesh_event_timing_usable(&timing, 1100u));

    mesh_event_note_missed(&timing, &diagnostics);
    assert(timing.missed_event_count == 1u);
    assert(diagnostics.ch9_event_misses == 1u);
    assert(!timing.fallback_required);
    assert(timing.timing_fresh);
    assert(mesh_event_timing_usable(&timing, 1100u));

    mesh_event_note_missed(&timing, &diagnostics);
    assert(timing.missed_event_count == 2u);
    assert(diagnostics.ch9_event_misses == 2u);
    assert(timing.fallback_required);
    assert(!timing.timing_fresh);
    assert(!mesh_event_timing_usable(&timing, 1200u));

    assert(mesh_event_plan_channel9(&timing, &requirements, 1200u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5);

    mesh_event_note_report_latency(&diagnostics, 42u);
    mesh_event_note_report_latency(&diagnostics, 8u);
    assert(diagnostics.ch9_report_latency_ms == 50u);
}

static void test_channel9_skip_elapsed_advances_to_next_live_slot(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_diagnostics diagnostics = {0};

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);

    assert(mesh_event_skip_elapsed(&timing, 1019u, &diagnostics) == 0u);
    assert(timing.next_event_time_ms == 1000u);
    assert(timing.event_counter == 0u);
    assert(diagnostics.ch9_event_misses == 0u);

    assert(mesh_event_skip_elapsed(&timing, 1319u, &diagnostics) == 3u);
    assert(timing.next_event_time_ms == 1300u);
    assert(timing.event_counter == 3u);
    assert(timing.missed_event_count == 3u);
    assert(diagnostics.ch9_event_misses == 3u);
}

static void test_channel9_traffic_refreshes_supervision_timeout(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();

    params.supervision_timeout_ms = 500u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_timing_usable(&timing, 1499u));
    assert(!mesh_event_timing_usable(&timing, 1500u));

    mesh_event_note_success(&timing, 1400u);
    assert(mesh_event_timing_usable(&timing, 1899u));
    assert(!mesh_event_timing_usable(&timing, 1900u));

    mesh_event_note_observed_packet(&timing, 1800u, 1805u);
    assert(mesh_event_timing_usable(&timing, 2299u));
    assert(!mesh_event_timing_usable(&timing, 2300u));
}

static void test_channel9_local_tx_does_not_refresh_supervision_timeout(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();

    params.supervision_timeout_ms = 500u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_timing_local_tx_slot(&timing));

    mesh_event_note_local_tx(&timing, 1400u);
    assert(mesh_event_timing_local_rx_slot(&timing));
    assert(timing.next_event_time_ms == 1500u);
    assert(mesh_event_timing_usable(&timing, 1499u));
    assert(!mesh_event_timing_usable(&timing, 1500u));
}

int main(void)
{
    test_gateway_ack_is_end_to_end();
    test_command_and_result_are_acknowledged_not_clicks();
    test_rejects_invalid_ids();
    test_rejects_zero_sequence_numbers();
    test_channel9_timing_requires_channel5_contact();
    test_channel9_first_slot_direction_follows_initiator();
    test_channel9_timing_crosses_uptime_domains_as_relative_delay();
    test_channel9_event_planner_reserves_channel5_scan();
    test_channel9_event_planner_keeps_negotiated_window_when_late();
    test_channel9_observed_rx_keeps_negotiated_cadence();
    test_channel5_activity_preempts_channel9_mesh();
    test_channel5_active_until_zero_is_idle_across_uptime_wrap();
    test_channel9_missed_events_refresh_contact_at_configured_limit();
    test_channel9_skip_elapsed_advances_to_next_live_slot();
    test_channel9_traffic_refreshes_supervision_timeout();
    test_channel9_local_tx_does_not_refresh_supervision_timeout();
    return 0;
}
