#include "mesh.h"
#include "survey.h"

#include <assert.h>
#include <string.h>

static void test_rf_channel_admission_is_exhaustive_and_fail_closed(void)
{
    static const uint8_t channel5_only[] = {
        MSG_ROUTE_REQ,
        MSG_ROUTE_REPLY,
        MSG_ROUTE_REPLY_ACK,
        MSG_GATEWAY_ROUTE_ADV,
        MSG_MESH_EVENT_PROPOSE,
        MSG_MESH_EVENT_ACCEPT,
        MSG_MESH_EVENT_UPDATE,
        MSG_MESH_EVENT_END,
        MSG_RELAY_BUSY,
        MSG_RESULT_BUSY,
        MSG_RESULT_OFFER,
        MSG_RESULT_GRANT,
        MSG_COMMAND,
        MSG_SURVEY_PAIR_PREPARE,
        MSG_SURVEY_DISCOVERY_START,
    };
    static const uint8_t channel9_only[] = {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_ANCHOR_HEARTBEAT,
        MSG_MESH_HOP_ACK,
        MSG_GATEWAY_ACK,
        MSG_GATEWAY_ROUTE_REQ,
        MSG_COMMAND_RESULT,
        MSG_RESULT_BUNDLE,
        MSG_SURVEY_PAIR_RESULT,
        MSG_SURVEY_DISCOVERY_REPORT,
    };
    static const uint8_t rejected[] = {
        MSG_SURVEY_REACH_REQ,
        MSG_SURVEY_REACH_REPORT,
        MSG_GATEWAY_COMMAND_EVENT,
        MSG_GATEWAY_HOST_RECEIPT,
        MSG_ERROR,
    };

    for (size_t i = 0u; i < sizeof(channel5_only); i++) {
        assert(mesh_packet_rf_channel_allowed(
            channel5_only[i], UWB_CHANNEL_WAKE_CONTACT, false));
        assert(!mesh_packet_rf_channel_allowed(
            channel5_only[i], UWB_CHANNEL_MESH_PAYLOAD, false));
    }
    for (size_t i = 0u; i < sizeof(channel9_only); i++) {
        assert(!mesh_packet_rf_channel_allowed(
            channel9_only[i], UWB_CHANNEL_WAKE_CONTACT, false));
        assert(mesh_packet_rf_channel_allowed(
            channel9_only[i], UWB_CHANNEL_MESH_PAYLOAD, false));
    }
    for (size_t i = 0u; i < sizeof(rejected); i++) {
        assert(!mesh_packet_rf_channel_allowed(
            rejected[i], UWB_CHANNEL_WAKE_CONTACT, true));
        assert(!mesh_packet_rf_channel_allowed(
            rejected[i], UWB_CHANNEL_MESH_PAYLOAD, true));
    }

    assert(mesh_packet_rf_channel_allowed(
        MSG_GATEWAY_COLLECTION_EACK, UWB_CHANNEL_WAKE_CONTACT, false));
    assert(mesh_packet_rf_channel_allowed(
        MSG_GATEWAY_COLLECTION_EACK, UWB_CHANNEL_MESH_PAYLOAD, false));
    assert(!mesh_packet_rf_channel_allowed(
        MSG_MESH_DATA, UWB_CHANNEL_MESH_PAYLOAD, false));
    assert(mesh_packet_rf_channel_allowed(
        MSG_MESH_DATA, UWB_CHANNEL_MESH_PAYLOAD, true));
    assert(!mesh_packet_rf_channel_allowed(
        MSG_MESH_DATA, UWB_CHANNEL_WAKE_CONTACT, true));
    assert(!mesh_packet_rf_channel_allowed(MSG_COMMAND, 7u, false));
    assert(!mesh_packet_rf_channel_allowed(0xFFu,
                                           UWB_CHANNEL_MESH_PAYLOAD,
                                           true));
}

static void test_non_rf_types_fail_semantic_ingress(void)
{
    static const uint8_t rejected[] = {
        MSG_SURVEY_REACH_REQ,
        MSG_SURVEY_REACH_REPORT,
        MSG_GATEWAY_COMMAND_EVENT,
        MSG_GATEWAY_HOST_RECEIPT,
        MSG_ERROR,
    };
    const uint64_t source_id = UINT64_C(0x1000000000000001);
    const uint64_t local_id = UINT64_C(0x2000000000000002);
    const uint64_t gateway_id = UINT64_C(0x3000000000000003);

    for (size_t i = 0u; i < sizeof(rejected); i++) {
        struct proto_packet packet = {
            .msg_type = rejected[i],
            .src_id = source_id,
            .dst_id = local_id,
            .session_id = 1u,
            .seq = 1u,
            .ttl = 1u,
        };

        assert(mesh_packet_rx_semantics_validate(&packet,
                                                 NULL,
                                                 0u,
                                                 source_id,
                                                 local_id,
                                                 gateway_id) ==
               PROTO_ERR_MALFORMED);
    }
}

static void test_gateway_ack_is_end_to_end(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};
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

static void test_channel9_timing_rejects_duplicate_singletons_without_mutation(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing parsed = {
        .mesh_channel = 7u,
        .event_interval_ms = 0xA5A5u,
        .event_counter = 0x12345678u,
        .route_fresh = true,
    };
    struct mesh_event_timing before = parsed;
    struct mesh_event_params params = event_params();
    uint8_t payload[128];
    size_t payload_len = 0u;

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    timing.event_counter = 9u;
    assert(mesh_append_event_timing_tlvs(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         &timing) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_EVENT_INTERVAL_MS,
                          timing.event_interval_ms + 1u) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs(&parsed,
                                       payload,
                                       payload_len,
                                       true) == PROTO_ERR_MALFORMED);
    assert(memcmp(&parsed, &before, sizeof(parsed)) == 0);

    payload_len = 0u;
    assert(mesh_append_event_timing_tlvs(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         &timing) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_EVENT_COUNTER,
                          timing.event_counter + 1u) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs(&parsed,
                                       payload,
                                       payload_len,
                                       true) == PROTO_ERR_MALFORMED);
    assert(memcmp(&parsed, &before, sizeof(parsed)) == 0);
}

static void test_event_update_requires_explicit_sender_parity(void)
{
    const uint64_t local_id = UINT64_C(0x1111111111111111);
    const uint64_t peer_id = UINT64_C(0x2222222222222222);
    struct mesh_event_params params = event_params();
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing parsed = {0};
    struct proto_packet packet = {0};
    uint8_t payload[96];
    size_t payload_len;

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    timing.event_counter = UINT32_C(0x12345678);
    for (uint8_t parity = 0u; parity <= 1u; parity++) {
        payload_len = 0u;
        timing.local_tx_on_even_events = parity != 0u;
        assert(mesh_append_event_update_tlvs_at(payload,
                                                sizeof(payload),
                                                &payload_len,
                                                &timing,
                                                900u) == PROTO_OK);
        assert(mesh_init_event_control(&packet,
                                       MSG_MESH_EVENT_UPDATE,
                                       peer_id,
                                       local_id,
                                       UINT32_C(0x80000001),
                                       (uint16_t)(20u + parity),
                                       (uint8_t)payload_len) == PROTO_OK);
        assert(mesh_packet_rx_envelope_validate(
                   &packet, payload, payload_len, peer_id, local_id, local_id,
                   UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_OK);
        memset(&parsed, 0, sizeof(parsed));
        assert(mesh_event_timing_from_tlvs_at(&parsed,
                                              payload,
                                              payload_len,
                                              900u,
                                              true) == PROTO_OK);
        assert(parsed.local_tx_on_even_events == (parity != 0u));
        /* The receiver installs the complement of this decoded sender phase. */
        parsed.local_tx_on_even_events = !parsed.local_tx_on_even_events;
        assert(parsed.local_tx_on_even_events == (parity == 0u));
    }

    payload_len = 0u;
    assert(mesh_append_event_timing_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            900u) == PROTO_OK);
    assert(mesh_init_event_control(&packet,
                                   MSG_MESH_EVENT_UPDATE,
                                   peer_id,
                                   local_id,
                                   UINT32_C(0x80000001),
                                   30u,
                                   (uint8_t)payload_len) == PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_EVENT_TX_ON_EVEN,
                         2u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    timing.local_tx_on_even_events = true;
    assert(mesh_append_event_update_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            900u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_EVENT_TX_ON_EVEN,
                         1u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(mesh_append_event_update_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            900u) == PROTO_OK);
    assert(mesh_init_event_control(&packet,
                                   MSG_MESH_EVENT_ACCEPT,
                                   peer_id,
                                   local_id,
                                   timing.event_counter,
                                   31u,
                                   (uint8_t)payload_len) == PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);
}

static void test_event_accept_wire_allows_legacy_header_identity(void)
{
    const uint64_t local_id = UINT64_C(0x1111111111111111);
    const uint64_t peer_id = UINT64_C(0x2222222222222222);
    const uint32_t proposal_session = UINT32_C(0x2468ace0);
    struct mesh_event_params params = event_params();
    struct mesh_event_timing timing = {0};
    struct proto_packet packet = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_timing_bind_proposal_session(&timing, proposal_session));
    assert(mesh_append_event_timing_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            900u) == PROTO_OK);

    assert(mesh_init_event_control(&packet,
                                   MSG_MESH_EVENT_ACCEPT,
                                   peer_id,
                                   local_id,
                                   proposal_session,
                                   40u,
                                   (uint8_t)payload_len) == PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_OK);

    packet.session_id = UINT32_C(0x87654321);
    packet.seq = 77u;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, peer_id, local_id, local_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_OK);
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

static void test_channel9_first_slot_direction_honors_counter_parity(void)
{
    struct mesh_event_timing timing = {0};

    timing.event_counter = UINT32_C(0x2468ace0);
    mesh_event_timing_set_local_first_slot_tx(&timing, true);
    assert(mesh_event_timing_local_tx_slot(&timing));
    timing.event_counter++;
    assert(mesh_event_timing_local_rx_slot(&timing));

    timing.event_counter = UINT32_C(0x13579bdf);
    mesh_event_timing_set_local_first_slot_tx(&timing, true);
    assert(mesh_event_timing_local_tx_slot(&timing));
    timing.event_counter++;
    assert(mesh_event_timing_local_rx_slot(&timing));

    timing.event_counter = UINT32_C(0x2468ace0);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    assert(mesh_event_timing_local_rx_slot(&timing));
    timing.event_counter++;
    assert(mesh_event_timing_local_tx_slot(&timing));

    timing.event_counter = UINT32_C(0x13579bdf);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    assert(mesh_event_timing_local_rx_slot(&timing));
    timing.event_counter++;
    assert(mesh_event_timing_local_tx_slot(&timing));
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

static void test_channel9_event_start_zero_survives_uptime_wrap(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing parsed = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    const struct mesh_channel5_requirements requirements = {
        .retune_guard_ms = 5u,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    params.first_event_time_ms = 0u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_guard_start_ms(&timing) == UINT32_MAX - 3u);
    assert(mesh_event_plan_channel9(&timing,
                                    &requirements,
                                    UINT32_MAX - 10u,
                                    &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(mesh_event_plan_channel9(&timing,
                                    &requirements,
                                    UINT32_MAX - 4u,
                                    &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 0u);

    assert(mesh_append_event_timing_tlvs_at(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &timing,
                                            UINT32_MAX) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs_at(&parsed,
                                         payload,
                                         payload_len,
                                         UINT32_MAX,
                                         true) == PROTO_OK);
    assert(parsed.next_event_time_ms == 0u);
}

static void test_channel9_accept_reanchors_after_wake_train_delay(void)
{
    struct mesh_event_timing proposal = {0};
    struct mesh_event_timing accepted_by_peer = {0};
    struct mesh_event_timing accepted_by_origin = {0};
    struct mesh_event_params params = event_params();
    uint8_t proposal_payload[96];
    uint8_t accept_payload[96];
    size_t proposal_len = 0u;
    size_t accept_len = 0u;

    params.first_event_time_ms = 1600u;
    assert(mesh_event_timing_negotiate(&proposal, &params, true) == PROTO_OK);
    assert(mesh_append_event_timing_tlvs_at(proposal_payload,
                                            sizeof(proposal_payload),
                                            &proposal_len,
                                            &proposal,
                                            1000u) == PROTO_OK);

    /* The wake/contact sequence delays proposal reception into another uptime domain. */
    assert(mesh_event_timing_from_tlvs_at(&accepted_by_peer,
                                          proposal_payload,
                                          proposal_len,
                                          5000u,
                                          true) == PROTO_OK);
    assert(accepted_by_peer.next_event_time_ms == 5600u);

    assert(mesh_append_event_timing_tlvs_at(accept_payload,
                                            sizeof(accept_payload),
                                            &accept_len,
                                            &accepted_by_peer,
                                            5100u) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs_at(&accepted_by_origin,
                                          accept_payload,
                                          accept_len,
                                          2200u,
                                          true) == PROTO_OK);
    assert(accepted_by_origin.next_event_time_ms == 2700u);
    assert(accepted_by_origin.next_event_time_ms != proposal.next_event_time_ms);
}

static void test_channel9_sender_reanchors_to_control_tx_completion(void)
{
    struct mesh_event_timing timing = {
        .mesh_channel = MESH_EVENT_CHANNEL,
        .event_interval_ms = 300u,
        .local_tx_on_even_events = false,
    };
    uint32_t peer_start_ms;

    mesh_event_timing_reanchor_after_control_tx(&timing, 1720u, 610u, 10u);
    peer_start_ms = (1720u - 10u) + 610u;
    assert(timing.next_event_time_ms == 2320u);
    assert(timing.next_event_time_ms == peer_start_ms);
    assert(timing.mesh_channel == MESH_EVENT_CHANNEL);
    assert(timing.event_interval_ms == 300u);
    assert(!timing.local_tx_on_even_events);

    mesh_event_timing_reanchor_after_control_tx(&timing, 108040u, 519u, 10u);
    peer_start_ms = (108040u - 10u) + 519u;
    assert(timing.next_event_time_ms == 108549u);
    assert(timing.next_event_time_ms == peer_start_ms);

    mesh_event_timing_reanchor_after_control_tx(&timing, 500u, 5u, 10u);
    assert(timing.next_event_time_ms == 501u);
}

static void test_channel9_exact_accept_replay_realigns_both_peers(void)
{
    struct mesh_event_timing encoded_timing = {0};
    struct mesh_event_timing first_sender = {0};
    struct mesh_event_timing first_receiver = {0};
    struct mesh_event_timing replay_sender = {0};
    struct mesh_event_timing replay_receiver = {0};
    struct mesh_event_params params = event_params();
    uint8_t accept_payload[96];
    size_t accept_len = 0u;
    const uint32_t encoded_at_ms = 1000u;
    const uint32_t encoded_delay_ms = 600u;
    const uint32_t airtime_ms = 10u;

    params.first_event_time_ms = encoded_at_ms + encoded_delay_ms;
    assert(mesh_event_timing_negotiate(&encoded_timing, &params, true) ==
           PROTO_OK);
    assert(mesh_append_event_timing_tlvs_at(accept_payload,
                                            sizeof(accept_payload),
                                            &accept_len,
                                            &encoded_timing,
                                            encoded_at_ms) == PROTO_OK);

    first_sender = encoded_timing;
    mesh_event_timing_reanchor_after_control_tx(&first_sender,
                                                1100u,
                                                encoded_delay_ms,
                                                airtime_ms);
    assert(mesh_event_timing_from_tlvs_at(&first_receiver,
                                          accept_payload,
                                          accept_len,
                                          1100u - airtime_ms,
                                          true) == PROTO_OK);
    assert(first_sender.next_event_time_ms ==
           first_receiver.next_event_time_ms);

    /* Reuse the exact payload later; both peers must move to the new phase. */
    replay_sender = encoded_timing;
    mesh_event_timing_reanchor_after_control_tx(&replay_sender,
                                                2100u,
                                                encoded_delay_ms,
                                                airtime_ms);
    assert(mesh_event_timing_from_tlvs_at(&replay_receiver,
                                          accept_payload,
                                          accept_len,
                                          2100u - airtime_ms,
                                          true) == PROTO_OK);
    assert(replay_sender.next_event_time_ms ==
           replay_receiver.next_event_time_ms);
    assert(replay_sender.next_event_time_ms != first_sender.next_event_time_ms);
}

static void test_channel9_event_planner_reserves_channel5_scan(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 1015u,
        .next_required_scan_start_valid = true,
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

static void test_channel9_observed_rx_duplicate_is_inert_across_counter_wrap(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    const uint32_t event_start_ms = params.first_event_time_ms;

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    timing.event_counter = UINT32_MAX;

    mesh_event_note_observed_packet(&timing,
                                    event_start_ms,
                                    event_start_ms + 1u);
    assert(timing.event_counter == 0u);
    assert(timing.last_successful_ch9_event_ms == event_start_ms);
    assert(timing.next_event_time_ms ==
           event_start_ms + timing.event_interval_ms);

    mesh_event_note_observed_packet(&timing,
                                    event_start_ms,
                                    event_start_ms + 2u);
    assert(timing.event_counter == 0u);
    assert(timing.last_successful_ch9_event_ms == event_start_ms);
    assert(timing.next_event_time_ms ==
           event_start_ms + timing.event_interval_ms);
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
        .next_required_scan_start_valid = true,
        .active_until_valid = true,
        .retune_guard_ms = 5u,
        .click_epoch_active = true,
    };

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(&timing, &click_requirements, 1000u, &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE);
    assert(mesh_event_plan_is_policy_deferral(plan.action));
    mesh_event_note_plan_action(&diagnostics, plan.action);
    assert(diagnostics.mesh_deferrals == 1u);
    assert(diagnostics.channel5_preemptions == 1u);

    mesh_event_note_channel_switch(&diagnostics, false, true);
    assert(diagnostics.channel_switches == 1u);
    assert(diagnostics.pll_ready_failures == 1u);
    assert(diagnostics.late_channel5_returns == 1u);
}

static void test_channel9_policy_deferral_classification(void)
{
    assert(mesh_event_plan_is_policy_deferral(
        MESH_EVENT_PLAN_DEFER_CH5_ACTIVE));
    assert(mesh_event_plan_is_policy_deferral(
        MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD));
    assert(!mesh_event_plan_is_policy_deferral(MESH_EVENT_PLAN_START));
    assert(!mesh_event_plan_is_policy_deferral(MESH_EVENT_PLAN_CLIP));
    assert(!mesh_event_plan_is_policy_deferral(MESH_EVENT_PLAN_WAIT));
    assert(!mesh_event_plan_is_policy_deferral(
        MESH_EVENT_PLAN_REFRESH_CONTACT_CH5));
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

static void test_channel5_active_deadline_zero_preempts_across_uptime_wrap(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    const struct mesh_channel5_requirements requirements = {
        .active_until_ms = 0u,
        .active_until_valid = true,
        .retune_guard_ms = 5u,
    };

    params.first_event_time_ms = UINT32_MAX - 16u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(
               &timing, &requirements, params.first_event_time_ms, &plan) ==
           PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE);
}

static void test_channel5_scan_deadline_zero_clips_across_uptime_wrap(void)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = event_params();
    struct mesh_event_plan plan = {0};
    const struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 0u,
        .next_required_scan_start_valid = true,
        .retune_guard_ms = 5u,
    };

    params.first_event_time_ms = UINT32_MAX - 20u;
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_event_plan_channel9(
               &timing, &requirements, params.first_event_time_ms, &plan) ==
           PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_CLIP);
    assert(plan.start_ms == params.first_event_time_ms);
    assert(plan.end_ms == UINT32_MAX - 4u);
    assert(plan.window_ms == 16u);
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
    assert(timing.missed_event_count == 1u);
    assert(diagnostics.ch9_event_misses == 1u);
    assert(timing.timing_fresh);
    assert(!timing.fallback_required);

    assert(mesh_event_skip_elapsed(&timing, 1419u, &diagnostics) == 1u);
    assert(timing.next_event_time_ms == 1400u);
    assert(timing.event_counter == 4u);
    assert(timing.missed_event_count == 2u);
    assert(diagnostics.ch9_event_misses == 2u);
    assert(!timing.timing_fresh);
    assert(timing.fallback_required);
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

static void test_channel9_batch_metadata_requires_exact_pair(void)
{
    struct mesh_ch9_batch_metadata metadata;
    uint8_t payload[32];
    size_t payload_len = 0u;

    assert(mesh_ch9_batch_metadata_parse(NULL, 0u, &metadata) == PROTO_OK);
    assert(!metadata.present);
    assert(mesh_ch9_batch_metadata_parse(payload, 1u, NULL) == PROTO_ERR_ARG);
    assert(mesh_ch9_batch_metadata_parse(NULL, 1u, &metadata) == PROTO_ERR_ARG);

    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          UINT32_C(0x12345678)) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         MESH_CH9_BATCH_FLAG_FINAL) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          UINT32_C(0x12345678)) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         MESH_CH9_BATCH_FLAG_FINAL) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_OK);
    assert(metadata.present);
    assert(metadata.batch_id == UINT32_C(0x12345678));
    assert(metadata.flags == MESH_CH9_BATCH_FLAG_FINAL);
    assert(metadata.final_packet);

    payload_len = 0u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS, 0u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 1u) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_OK);
    assert(metadata.present);
    assert(!metadata.final_packet);
}

static void test_channel9_batch_metadata_rejects_ambiguous_values(void)
{
    struct mesh_ch9_batch_metadata metadata;
    uint8_t payload[32];
    size_t payload_len = 0u;

    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 0u) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS, 0u) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 1u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 2u) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS, 0u) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 1u) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS, 0x02u) == PROTO_OK);
    assert(mesh_ch9_batch_metadata_parse(payload, payload_len, &metadata) ==
           PROTO_ERR_MALFORMED);

    payload[0] = TLV_MESH_CH9_BATCH_ID;
    payload[1] = 4u;
    payload[2] = 1u;
    assert(mesh_ch9_batch_metadata_parse(payload, 3u, &metadata) ==
           PROTO_ERR_MALFORMED);
}

static void test_ack_payload_requires_one_consistent_encoding(void)
{
    struct proto_packet ack = {
        .msg_type = MSG_GATEWAY_ACK,
        .session_id = 100u,
    };
    uint8_t payload[64];
    uint8_t seqs[2u * sizeof(uint16_t)];
    uint8_t sessions[2u * sizeof(uint32_t)];
    uint8_t packet_ids[sizeof(uint32_t)];
    size_t payload_len = 0u;
    bool contains = false;

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     7u) == PROTO_OK);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     100u,
                                     7u,
                                     &contains) == PROTO_OK);
    assert(contains);

    payload_len = 0u;
    proto_put_u16_le(&seqs[0], 9u);
    proto_put_u16_le(&seqs[sizeof(uint16_t)], 9u);
    proto_put_u32_le(&sessions[0], 101u);
    proto_put_u32_le(&sessions[sizeof(uint32_t)], 102u);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            sessions,
                            sizeof(sessions)) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seqs,
                            sizeof(seqs)) == PROTO_OK);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     102u,
                                     9u,
                                     &contains) == PROTO_OK);
    assert(contains);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     103u,
                                     9u,
                                     &contains) == PROTO_OK);
    assert(!contains);

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     10u) == PROTO_OK);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     102u,
                                     9u,
                                     &contains) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            sessions,
                            sizeof(uint32_t)) == PROTO_OK);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     101u,
                                     9u,
                                     &contains) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seqs,
                            sizeof(seqs)) == PROTO_OK);
    proto_put_u32_le(packet_ids, 1u);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_PACKET_ID_LIST,
                            packet_ids,
                            sizeof(packet_ids)) == PROTO_OK);
    assert(mesh_ack_payload_contains(&ack,
                                     payload,
                                     payload_len,
                                     101u,
                                     9u,
                                     &contains) == PROTO_ERR_MALFORMED);
}

static void test_ack_payload_requires_exact_semantic_identity(void)
{
    struct proto_packet first = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = UINT64_C(0x1111222233334444),
        .dst_id = UINT64_C(0x9999888877776666),
        .session_id = UINT32_C(0x12345678),
        .seq = UINT16_C(0x2345),
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 1u,
        .message_age_ms = 10u,
    };
    struct proto_packet second = first;
    struct proto_packet retry = first;
    struct proto_packet ack = {
        .msg_type = MSG_GATEWAY_ACK,
        .flags = FLAG_GATEWAY_ACK,
        .src_id = UINT64_C(0x9999888877776666),
        .dst_id = UINT64_C(0x1111222233334444),
        .session_id = first.session_id,
        .seq = UINT16_C(0x9001),
        .ttl = MESH_GATEWAY_ACK_TTL,
    };
    const uint8_t first_payload[1] = {0x41u};
    const uint8_t conflicting_payload[1] = {0x42u};
    const uint8_t second_payload[1] = {0x43u};
    uint8_t payload[192];
    uint8_t seq_list[2u * sizeof(uint16_t)];
    uint8_t session_list[2u * sizeof(uint32_t)];
    size_t payload_len = 0u;
    bool contains = false;

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     first.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &first,
                                             first_payload,
                                             sizeof(first_payload)) ==
           PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    assert(payload_len == MESH_ACK_SINGLE_PAYLOAD_LEN);
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &first,
                                            first_payload,
                                            sizeof(first_payload),
                                            &contains) == PROTO_OK);
    assert(contains);

    retry.ttl--;
    retry.message_age_ms += 500u;
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &retry,
                                            first_payload,
                                            sizeof(first_payload),
                                            &contains) == PROTO_OK);
    assert(contains);
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &first,
                                            conflicting_payload,
                                            sizeof(conflicting_payload),
                                            &contains) == PROTO_OK);
    assert(!contains);

    payload_len = 0u;
    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     first.seq) == PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &first,
                                            first_payload,
                                            sizeof(first_payload),
                                            &contains) ==
           PROTO_ERR_MALFORMED);

    second.session_id++;
    second.seq++;
    proto_put_u32_le(&session_list[0], first.session_id);
    proto_put_u32_le(&session_list[sizeof(uint32_t)], second.session_id);
    proto_put_u16_le(&seq_list[0], first.seq);
    proto_put_u16_le(&seq_list[sizeof(uint16_t)], second.seq);
    payload_len = 0u;
    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     first.seq) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            session_list,
                            sizeof(session_list)) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seq_list,
                            sizeof(seq_list)) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &first,
                                             first_payload,
                                             sizeof(first_payload)) ==
           PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &second,
                                             second_payload,
                                             sizeof(second_payload)) ==
           PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &second,
                                            second_payload,
                                            sizeof(second_payload),
                                            &contains) == PROTO_OK);
    assert(contains);

    proto_put_u16_le(&seq_list[sizeof(uint16_t)], first.seq);
    payload_len = 0u;
    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     first.seq) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            session_list,
                            sizeof(session_list)) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seq_list,
                            sizeof(seq_list)) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &first,
                                             first_payload,
                                             sizeof(first_payload)) ==
           PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &second,
                                             second_payload,
                                             sizeof(second_payload)) ==
           PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    assert(mesh_ack_payload_contains_packet(&ack,
                                            payload,
                                            payload_len,
                                            &second,
                                            second_payload,
                                            sizeof(second_payload),
                                            &contains) ==
           PROTO_ERR_MALFORMED);
}

static void test_rx_envelope_rejects_noncanonical_gateway_route_and_event(void)
{
    const uint64_t gateway_id = UINT64_C(0x1000000000000001);
    const uint64_t anchor_id = UINT64_C(0x2000000000000002);
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_ROUTE_REQ,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = anchor_id,
        .dst_id = gateway_id,
        .session_id = 11u,
        .seq = 12u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };

    assert(mesh_packet_rx_envelope_validate(
               &packet, NULL, 0u, anchor_id, gateway_id, gateway_id,
               UWB_CHANNEL_MESH_PAYLOAD, false) == PROTO_OK);
    packet.ttl--;
    assert(mesh_packet_rx_envelope_validate(
               &packet, NULL, 0u, anchor_id, gateway_id, gateway_id,
               UWB_CHANNEL_MESH_PAYLOAD, false) == PROTO_ERR_MALFORMED);
    packet.ttl = MESH_DEFAULT_TTL;
    assert(mesh_packet_rx_envelope_validate(
               &packet, NULL, 0u, UINT64_C(0x3000000000000003),
               gateway_id, gateway_id, UWB_CHANNEL_MESH_PAYLOAD, false) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_packet_rx_envelope_validate(
               &packet, NULL, 0u, anchor_id, anchor_id, gateway_id,
               UWB_CHANNEL_MESH_PAYLOAD, false) == PROTO_ERR_MALFORMED);

    {
        struct mesh_event_timing timing = {
            .mesh_channel = MESH_EVENT_CHANNEL,
            .event_interval_ms = 100u,
            .event_window_ms = 20u,
            .next_event_time_ms = 300u,
            .event_counter = 55u,
            .guard_ms = 5u,
            .peer_clock_skew_estimate_ppm = 0,
            .max_missed_events = 3u,
            .supervision_timeout_ms = 1200u,
        };
        uint8_t payload[96];
        size_t payload_len = 0u;

        assert(mesh_append_event_timing_tlvs_at(payload,
                                                sizeof(payload),
                                                &payload_len,
                                                &timing,
                                                100u) == PROTO_OK);
        assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                              TLV_MESH_EVENT_BOOT_NONCE,
                              UINT64_C(0x123456789abcdef0)) == PROTO_OK);
        assert(mesh_init_event_control(&packet,
                                       MSG_MESH_EVENT_PROPOSE,
                                       anchor_id,
                                       gateway_id,
                                       timing.event_counter,
                                       13u,
                                       (uint8_t)payload_len) == PROTO_OK);
        assert(mesh_packet_rx_envelope_validate(
                   &packet, payload, payload_len, anchor_id, gateway_id,
                   gateway_id, UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_OK);
        assert(mesh_packet_rx_envelope_validate(
                   &packet, payload, payload_len, anchor_id, gateway_id,
                   gateway_id, UWB_CHANNEL_MESH_PAYLOAD, false) ==
               PROTO_ERR_MALFORMED);
        packet.flags = FLAG_CONTROL_FOLLOWUP;
        assert(mesh_packet_rx_envelope_validate(
                   &packet, payload, payload_len, anchor_id, gateway_id,
                   gateway_id, UWB_CHANNEL_WAKE_CONTACT, false) ==
               PROTO_ERR_MALFORMED);
        packet.flags = 0u;
        assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                             TLV_REASON, 1u) == PROTO_OK);
        packet.payload_len = (uint16_t)payload_len;
        assert(mesh_packet_rx_envelope_validate(
                   &packet, payload, payload_len, anchor_id, gateway_id,
                   gateway_id, UWB_CHANNEL_WAKE_CONTACT, false) ==
               PROTO_ERR_MALFORMED);
    }
}

static void test_rx_envelope_rejects_result_control_schema_and_addressing(void)
{
    const uint64_t gateway_id = UINT64_C(0x1000000000000001);
    const uint64_t child_id = UINT64_C(0x2000000000000002);
    const uint64_t parent_id = UINT64_C(0x3000000000000003);
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = gateway_id,
            .gateway_epoch = 4u,
            .command_seq = 5u,
            .node_id = child_id,
            .node_boot_counter = 6u,
            .result_seq = 7u,
        },
        .result_len = 100u,
        .result_crc = 0x1234u,
        .result_digest = {0x9au},
        .priority = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = child_id,
        .dst_id = parent_id,
        .session_id = 5u,
        .seq = 7u,
        .ttl = 1u,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload, sizeof(payload), &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, child_id, parent_id, gateway_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_OK);
    packet.dst_id = gateway_id;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, child_id, parent_id, gateway_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);
    packet.dst_id = parent_id;
    packet.ttl = 2u;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, child_id, parent_id, gateway_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);
    packet.ttl = 1u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_REASON, 0u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_envelope_validate(
               &packet, payload, payload_len, child_id, parent_id, gateway_id,
               UWB_CHANNEL_WAKE_CONTACT, false) == PROTO_ERR_MALFORMED);
}

static void append_survey_pair_result_extensions(uint8_t *payload,
                                                 size_t payload_cap,
                                                 size_t *payload_len,
                                                 bool include_timing)
{
    uint8_t raw_timestamps[6u * sizeof(uint32_t)];

    assert(tlv_append_u64(payload,
                          payload_cap,
                          payload_len,
                          TLV_TIMESTAMP_MS,
                          UINT64_C(0x0102030405060708)) == PROTO_OK);
    if (!include_timing) {
        return;
    }
    assert(tlv_append_i8(payload, payload_cap, payload_len,
                         TLV_UWB_RSL_DBM, -73) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, payload_len,
                          TLV_UWB_CLOCK_OFFSET_RAW,
                          UINT16_C(0xfedc)) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, payload_len,
                          TLV_CLICKER_CLOCK_OFFSET_RAW,
                          UINT16_C(0x8123)) == PROTO_OK);
    assert(tlv_append_i32(payload, payload_cap, payload_len,
                          TLV_UWB_CARRIER_INTEGRATOR,
                          INT32_C(-1234567)) == PROTO_OK);
    for (size_t i = 0u; i < 6u; i++) {
        proto_put_u32_le(&raw_timestamps[i * sizeof(uint32_t)],
                         UINT32_C(0x10203040) + (uint32_t)i);
    }
    assert(tlv_append_bytes(payload,
                            payload_cap,
                            payload_len,
                            TLV_UWB_RAW_TIMESTAMPS,
                            raw_timestamps,
                            sizeof(raw_timestamps)) == PROTO_OK);
}

static struct survey_sample mesh_survey_pair_sample(uint64_t initiator_id)
{
    const struct survey_sample sample = {
        .pair = {
            .operation_generation = UINT64_C(0x1122334400001234),
            .survey_id = UINT32_C(0x55667788),
            .initiator_id = initiator_id,
            .responder_id = UINT64_C(0x3000000000000003),
            .sample_count = 3u,
        },
        .sample_index = 0u,
        .distance_mm = 1234,
        .quality = 90u,
        .range_status = RANGE_OK,
    };

    return sample;
}

static void init_mesh_survey_pair_packet(struct proto_packet *packet,
                                         const struct survey_sample *sample,
                                         uint64_t gateway_id,
                                         size_t payload_len)
{
    assert(survey_init_result_packet_from_reporter(
               packet,
               sample,
               sample->pair.responder_id,
               gateway_id,
               1u,
               (uint8_t)payload_len) == PROTO_OK);
}

static void test_survey_pair_result_rejects_reserved_range_status(void)
{
    const uint64_t gateway_id = UINT64_C(0x1000000000000001);
    const uint64_t initiator_id = UINT64_C(0x2000000000000002);
    struct survey_sample sample = mesh_survey_pair_sample(initiator_id);
    struct proto_packet packet;
    const uint8_t *range_status_raw = NULL;
    uint8_t range_status_len = 0u;
    uint8_t payload[192];
    size_t payload_len = 0u;

    assert(survey_append_sample_tlvs(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     &sample) == PROTO_OK);
    append_survey_pair_result_extensions(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         false);
    init_mesh_survey_pair_packet(&packet, &sample, gateway_id, payload_len);
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) == PROTO_OK);
    assert(tlv_find_unique(payload,
                           payload_len,
                           TLV_RANGE_STATUS,
                           &range_status_raw,
                           &range_status_len) == PROTO_OK);
    assert(range_status_len == sizeof(uint8_t));
    payload[(size_t)(range_status_raw - payload)] =
        (uint8_t)RANGE_STS_QUALITY_FAIL;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);
}

static void test_survey_pair_result_extension_schema_is_closed_at_ingress(void)
{
    const uint64_t gateway_id = UINT64_C(0x1000000000000001);
    const uint64_t initiator_id = UINT64_C(0x2000000000000002);
    const struct survey_sample sample = mesh_survey_pair_sample(initiator_id);
    struct proto_packet packet;
    uint8_t payload[192];
    size_t core_len;
    size_t payload_len = 0u;

    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &sample) == PROTO_OK);
    core_len = payload_len;
    append_survey_pair_result_extensions(payload, sizeof(payload),
                                         &payload_len, true);
    init_mesh_survey_pair_packet(&packet, &sample, gateway_id, payload_len);
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) == PROTO_OK);

    payload_len = core_len;
    append_survey_pair_result_extensions(payload, sizeof(payload),
                                         &payload_len, false);
    init_mesh_survey_pair_packet(&packet, &sample, gateway_id, payload_len);
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) == PROTO_OK);

    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         UINT8_C(0x27), 1u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);

    payload_len = core_len;
    append_survey_pair_result_extensions(payload, sizeof(payload),
                                         &payload_len, false);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_TIMESTAMP_MS,
                          UINT64_C(0x1112131415161718)) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);

    payload_len = core_len;
    append_survey_pair_result_extensions(payload, sizeof(payload),
                                         &payload_len, false);
    assert(tlv_append_i32(payload, sizeof(payload), &payload_len,
                          TLV_UWB_CARRIER_INTEGRATOR, -91) == PROTO_OK);
    assert(tlv_append_i8(payload, sizeof(payload), &payload_len,
                         TLV_UWB_RSL_DBM, -81) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);

    payload_len = core_len;
    append_survey_pair_result_extensions(payload, sizeof(payload),
                                         &payload_len, false);
    payload[core_len + 1u] = sizeof(uint64_t) - 1u;
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             sample.pair.responder_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);
}

static size_t build_survey_discovery_report_payload(
    uint8_t *payload,
    size_t payload_cap,
    uint64_t anchor_id,
    uint64_t operation_generation,
    bool include_boot_incarnation,
    uint32_t boot_incarnation)
{
    const struct survey_reachability_entry entry = {
        .peer_id = UINT64_C(0x3030303030303030),
        .rssi_dbm = -71,
        .quality = 83u,
    };
    size_t payload_len = 0u;

    assert(survey_append_reach_report_tlvs(
               payload,
               payload_cap,
               &payload_len,
               17u,
               anchor_id,
               &entry,
               1u) == PROTO_OK);
    assert(survey_operation_generation_append_tlv(
               payload,
               payload_cap,
               &payload_len,
               operation_generation) == PROTO_OK);
    if (include_boot_incarnation) {
        assert(tlv_append_u32(payload,
                              payload_cap,
                              &payload_len,
                              TLV_NODE_BOOT_COUNTER,
                              boot_incarnation) == PROTO_OK);
    }
    assert(tlv_append_u16(payload,
                          payload_cap,
                          &payload_len,
                          TLV_COMMAND_STATUS,
                          COMMAND_OK) == PROTO_OK);
    return payload_len;
}

static void test_survey_discovery_report_requires_one_nonzero_boot_incarnation(void)
{
    const uint64_t anchor_id = UINT64_C(0x1010101010101010);
    const uint64_t gateway_id = UINT64_C(0x2020202020202020);
    const uint64_t operation_generation = UINT64_C(0x0000000100000011);
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN] = {0};
    struct proto_packet packet = {
        .msg_type = MSG_SURVEY_DISCOVERY_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .ttl = MESH_DEFAULT_TTL,
        .src_id = anchor_id,
        .dst_id = gateway_id,
        .session_id = 7u,
        .seq = 1u,
    };
    size_t payload_len;

    payload_len = build_survey_discovery_report_payload(
        payload,
        sizeof(payload),
        anchor_id,
        operation_generation,
        true,
        7u);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             anchor_id,
                                             gateway_id,
                                             gateway_id) == PROTO_OK);

    packet.session_id = 8u;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             anchor_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);
    packet.session_id = 7u;

    payload_len = build_survey_discovery_report_payload(
        payload,
        sizeof(payload),
        anchor_id,
        operation_generation,
        false,
        0u);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             anchor_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);

    payload_len = build_survey_discovery_report_payload(
        payload,
        sizeof(payload),
        anchor_id,
        operation_generation,
        true,
        0u);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             anchor_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);

    payload_len = build_survey_discovery_report_payload(
        payload,
        sizeof(payload),
        anchor_id,
        operation_generation,
        true,
        7u);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_BOOT_COUNTER,
                          8u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_packet_rx_semantics_validate(&packet,
                                             payload,
                                             payload_len,
                                             anchor_id,
                                             gateway_id,
                                             gateway_id) ==
           PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_rf_channel_admission_is_exhaustive_and_fail_closed();
    test_non_rf_types_fail_semantic_ingress();
    test_gateway_ack_is_end_to_end();
    test_command_and_result_are_acknowledged_not_clicks();
    test_rejects_invalid_ids();
    test_rejects_zero_sequence_numbers();
    test_channel9_timing_requires_channel5_contact();
    test_channel9_timing_rejects_duplicate_singletons_without_mutation();
    test_event_update_requires_explicit_sender_parity();
    test_event_accept_wire_allows_legacy_header_identity();
    test_channel9_first_slot_direction_follows_initiator();
    test_channel9_first_slot_direction_honors_counter_parity();
    test_channel9_timing_crosses_uptime_domains_as_relative_delay();
    test_channel9_event_start_zero_survives_uptime_wrap();
    test_channel9_accept_reanchors_after_wake_train_delay();
    test_channel9_sender_reanchors_to_control_tx_completion();
    test_channel9_exact_accept_replay_realigns_both_peers();
    test_channel9_event_planner_reserves_channel5_scan();
    test_channel9_event_planner_keeps_negotiated_window_when_late();
    test_channel9_observed_rx_keeps_negotiated_cadence();
    test_channel9_observed_rx_duplicate_is_inert_across_counter_wrap();
    test_channel5_activity_preempts_channel9_mesh();
    test_channel9_policy_deferral_classification();
    test_channel5_active_until_zero_is_idle_across_uptime_wrap();
    test_channel5_active_deadline_zero_preempts_across_uptime_wrap();
    test_channel5_scan_deadline_zero_clips_across_uptime_wrap();
    test_channel9_missed_events_refresh_contact_at_configured_limit();
    test_channel9_skip_elapsed_advances_to_next_live_slot();
    test_channel9_traffic_refreshes_supervision_timeout();
    test_channel9_local_tx_does_not_refresh_supervision_timeout();
    test_channel9_batch_metadata_requires_exact_pair();
    test_channel9_batch_metadata_rejects_ambiguous_values();
    test_ack_payload_requires_one_consistent_encoding();
    test_ack_payload_requires_exact_semantic_identity();
    test_rx_envelope_rejects_noncanonical_gateway_route_and_event();
    test_rx_envelope_rejects_result_control_schema_and_addressing();
    test_survey_pair_result_rejects_reserved_range_status();
    test_survey_pair_result_extension_schema_is_closed_at_ingress();
    test_survey_discovery_report_requires_one_nonzero_boot_incarnation();
    return 0;
}
