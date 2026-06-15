#include "mesh_relay.h"

#include "mesh.h"
#include "report.h"
#include "uwb.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0x1111222233334444ull
#define ANCHOR_B 0x5555666677778888ull
#define GATEWAY  0x9999888877776666ull
#define NETWORK_ID 0x494D4543u

static bool has_action(const struct mesh_relay_result *result, enum mesh_relay_action action)
{
    return (result->actions & action) != 0u;
}

static struct route_candidate direct_gateway_route(uint64_t next_hop_id,
                                                   uint32_t epoch,
                                                   uint8_t quality)
{
    struct route_candidate candidate = {
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY,
        .route_epoch = epoch,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = quality,
        .failure_count = 0u,
        .valid = true,
    };
    return candidate;
}

static struct mesh_event_params channel9_params(uint32_t first_event_time_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = 100u,
        .event_window_ms = 20u,
        .first_event_time_ms = first_event_time_ms,
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 2u,
        .supervision_timeout_ms = 500u,
    };
    return params;
}

static struct mesh_channel5_requirements clear_channel5_requirements(void)
{
    const struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 0u,
        .active_until_ms = 0u,
        .retune_guard_ms = 5u,
        .click_epoch_active = false,
        .discovery_active = false,
        .ranging_active = false,
    };
    return requirements;
}

static void seed_downlink(struct mesh_relay *relay,
                          uint64_t target_id,
                          uint64_t next_hop_id,
                          uint32_t epoch,
                          uint8_t hop_count,
                          uint8_t quality,
                          uint32_t now_ms)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid) {
            continue;
        }
        entry->target_id = target_id;
        entry->next_hop_id = next_hop_id;
        entry->gateway_id = GATEWAY;
        entry->route_epoch = epoch;
        entry->last_seen_ms = now_ms;
        entry->hop_count = hop_count;
        entry->quality = quality;
        entry->valid = true;
        return;
    }

    assert(false);
}

static void decode_outbound_over_uwb(const struct mesh_outbound *out,
                                     uint64_t sender_id,
                                     uint64_t receiver_id,
                                     struct proto_packet *packet,
                                     uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     uint64_t *previous_hop_id)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    assert(out != NULL);
    assert(packet != NULL);
    assert(payload != NULL);
    assert(payload_len != NULL);
    assert(previous_hop_id != NULL);
    assert(uwb_mesh_frame_encode(NETWORK_ID,
                                 sender_id,
                                 out->next_hop_id,
                                 &out->packet,
                                 out->payload,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    assert(uwb_mesh_frame_decode(frame,
                                 frame_len,
                                 NETWORK_ID,
                                 receiver_id,
                                 previous_hop_id,
                                 packet,
                                 payload,
                                 payload_cap,
                                 payload_len) == PROTO_OK);
    assert(*previous_hop_id == sender_id);
    assert(packet->payload_len == *payload_len);
}

static void test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0xAAu};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 42u, 9u, sizeof(payload)) == PROTO_OK);
    report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                95u,
                                2000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.forward.packet.src_id == ANCHOR_A);
    assert(result.forward.packet.dst_id == GATEWAY);
    assert(result.forward.packet.session_id == report.session_id);
    assert(result.forward.packet.seq == report.seq);
    assert(result.forward.packet.ttl == 3u);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                95u,
                                2001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.forward.packet.ttl == 3u);
    assert(result.forward.packet.seq == report.seq);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);
}

static void test_duplicate_cache_expires_by_time_window(void)
{
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = 55u,
        .seq = 7u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    struct mesh_relay_result result;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                1001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                61001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
}

static void test_status_tlvs_report_selected_route(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 44u, 77u);
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 44u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);

    assert(mesh_relay_append_status_tlvs(&relay, payload, sizeof(payload), &payload_len) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == GATEWAY);

    assert(tlv_find(payload, payload_len, TLV_NEXT_HOP_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == ANCHOR_B);

    assert(tlv_find(payload, payload_len, TLV_ROUTE_EPOCH, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    assert(proto_get_u32_le(value) == 44u);

    assert(tlv_find(payload, payload_len, TLV_HOP_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);

    assert(tlv_find(payload, payload_len, TLV_QUALITY, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 77u);

    assert(tlv_find(payload, payload_len, TLV_RETRY_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);
}

static void test_status_tlvs_report_missing_route_reason(void)
{
    struct mesh_relay relay;
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 44u);

    assert(mesh_relay_append_status_tlvs(&relay, payload, sizeof(payload), &payload_len) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == GATEWAY);

    assert(tlv_find(payload, payload_len, TLV_REASON, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == (uint8_t)(-PROTO_ERR_NOT_FOUND));
    assert(tlv_find(payload, payload_len, TLV_NEXT_HOP_ID, &value, &value_len) == PROTO_ERR_NOT_FOUND);
}

static void test_legacy_route_beacons_are_dropped(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = 0x33u,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 12u,
        .seq = 3u,
        .ttl = 1u,
        .payload_len = 0u,
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                GATEWAY,
                                80u,
                                3000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(route_selected(&relay.upstream) == NULL);

    packet.msg_type = 0x34u;
    packet.dst_id = relay.local_id;
    packet.seq = 4u;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                GATEWAY,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

static void test_survey_discovery_broadcast_delivers_and_floods(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    const uint8_t payload[] = {0x15u, 0x04u, 0x78u, 0x56u, 0x34u, 0x12u};
    struct proto_packet packet = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345678u,
        .seq = 9u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.msg_type == MSG_SURVEY_DISCOVERY_START);
    assert(result.forward.packet.dst_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == 2u);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_broadcast_command_delivers_without_flooding(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    uint8_t payload[8];
    size_t payload_len = 0u;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345679u,
        .seq = 12u,
        .ttl = 3u,
    };

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_PING) == PROTO_OK);
    packet.payload_len = (uint8_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_survey_discovery_broadcast_still_forwards(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 14u, 90u);
    const uint8_t payload[] = {0x1Au, 0x04u, 0xE8u, 0x03u, 0x00u, 0x00u};
    const uint8_t report_payload[] = {0x42u};
    struct proto_packet report;
    struct mesh_outbound tx;
    struct proto_packet packet = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 55u,
        .seq = 10u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 14u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    56u,
                                    11u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               3100u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == 2u);
}

static void test_downlink_routes_survive_age_until_delivery_failure(void)
{
    struct mesh_relay gateway;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 30u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 30u, 2u, 75u, 1000u);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(mesh_relay_expire_routes(&gateway, 31001u) == 0u);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    mesh_relay_note_delivery_failure(&gateway, ANCHOR_A);
    assert(mesh_relay_find_downlink(&gateway, ANCHOR_A) == NULL);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_ERR_NOT_FOUND);
}

static void test_downlink_route_selection_uses_weighted_quality(void)
{
    struct mesh_relay gateway;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 31u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_A, 31u, 1u, 0u, 1000u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 31u, 2u, 100u, 1100u);

    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
}

static void test_start_tx_accepts_aged_upstream_route_until_failures(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 32u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    uint8_t payload[1] = {0x5Au};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 32u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 91u, 1u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               31001u,
                               &tx) == PROTO_OK);
    assert(route_selected(&relay.upstream) != NULL);

    mesh_relay_cancel_tx(&relay);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    assert(route_selected(&relay.upstream) != NULL);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    assert(route_selected(&relay.upstream) == NULL);
}

static void test_downlink_next_hop_send_completes_immediately(void)
{
    struct mesh_relay gateway;
    struct proto_packet command;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 33u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 33u, 2u, 100u, 1000u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_A, 33u, 1u, 0u, 1010u);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             400u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               2000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    assert(!mesh_relay_tx_active(&gateway));

    assert(mesh_relay_tick(&gateway, 10000u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_downlink_send_refreshes_route_age(void)
{
    struct mesh_relay gateway;
    struct proto_packet command;
    struct mesh_outbound tx;
    const struct mesh_downlink_entry *downlink;
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 34u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 34u, 2u, 80u, 1000u);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             401u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               7400u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    assert(!mesh_relay_tx_active(&gateway));

    mesh_relay_note_tx_sent(&gateway, &tx, 7450u);

    assert(mesh_relay_expire_routes(&gateway, 8001u) == 0u);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->last_seen_ms == 7450u);
}

static void test_ttl_zero_packet_is_dropped_without_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x55u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 77u, 1u, sizeof(payload)) == PROTO_OK);
    report.ttl = 0u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4000u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_zero_session_or_sequence_mesh_packets_are_rejected(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x56u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 78u, 1u, sizeof(payload)) ==
           PROTO_OK);

    report.session_id = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4010u,
                                &result) == PROTO_ERR_ARG);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               4010u,
                               &tx) == PROTO_ERR_ARG);

    report.session_id = 78u;
    report.seq = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4020u,
                                &result) == PROTO_ERR_ARG);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               4020u,
                               &tx) == PROTO_ERR_ARG);
}

static void test_busy_relay_does_not_ack_or_cache_new_forward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet local_report;
    struct proto_packet incoming_report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t local_payload[1] = {0x33u};
    uint8_t incoming_payload[1] = {0x44u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    78u,
                                    2u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4100u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(report_init_click_packet(&incoming_report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    79u,
                                    3u,
                                    sizeof(incoming_payload)) == PROTO_OK);
    incoming_report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    mesh_relay_cancel_tx(&relay);
    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4120u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_relay_drops_duplicate_forward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_report;
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    uint8_t incoming_payload[1] = {0x45u};
    uint8_t local_payload[1] = {0x46u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&incoming_report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    80u,
                                    4u,
                                    sizeof(incoming_payload)) == PROTO_OK);
    incoming_report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4200u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    81u,
                                    5u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4201u,
                               &tracked_report) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_local_gateway_bound_tx_waits_for_gateway_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet ack;
    uint8_t ack_payload[16];
    size_t ack_payload_len = 0u;
    uint8_t payload[1] = {0x42u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 88u, 7u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 5000u, &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(tx.next_hop_id == ANCHOR_B);
    assert(tx.packet.seq == report.seq);

    assert(mesh_append_requested_seq(ack_payload, sizeof(ack_payload), &ack_payload_len, report.seq) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 2u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));
}

static void test_relayed_tx_completes_after_next_hop_send(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x43u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 89u, 8u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 6000u, &tx) == PROTO_OK);
    assert(tx.next_hop_id == GATEWAY);
    assert(!mesh_relay_tx_active(&relay));

    mesh_relay_note_tx_sent(&relay, &tx, 6050u);
    assert(mesh_relay_tick(&relay, 7000u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_gateway_ack_timeout_retries_then_requests_route_discovery(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 9u, 70u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x44u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 9u);
    route.last_seen_ms = 7000u;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 90u, 9u, sizeof(payload)) == PROTO_OK);
    report.message_age_ms = 123u;

    uint32_t now_ms = 7000u;

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), now_ms, &tx) == PROTO_OK);
    assert(tx.packet.message_age_ms == 123u);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.packet.message_age_ms ==
           123u + ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u);
    assert(mesh_relay_tx_active(&relay));
    mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.message_age_ms ==
           123u + ((ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u) * 2u));
    assert(mesh_relay_tx_active(&relay));
    mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(!mesh_relay_tx_active(&relay));
    assert(route_selected(&relay.upstream) == NULL);
}

static void test_gateway_ack_timeout_handles_ms_wrap(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 10u, 70u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x45u};
    const uint32_t start_ms = 0xffffff00u;
    const uint32_t before_wrap_ms = start_ms + 10u;
    const uint32_t timed_out_ms = start_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 10u);
    route.last_seen_ms = start_ms;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 91u, 10u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), start_ms, &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, start_ms);
    assert(relay.pending.gateway_ack_deadline_ms < start_ms);

    assert(mesh_relay_tick(&relay, before_wrap_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&relay, timed_out_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&relay));
}

static void test_gateway_reaches_anchor_behind_relay_and_receives_result(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound gateway_command_tx;
    struct mesh_outbound anchor_result_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result anchor_result;
    struct mesh_relay_result gateway_result;
    struct route_candidate relay_route = direct_gateway_route(GATEWAY, 21u, 85u);
    struct route_candidate anchor_route = direct_gateway_route(ANCHOR_B, 21u, 70u);
    struct proto_packet command = {0};
    struct proto_packet command_result = {0};
    uint8_t command_payload[16];
    uint8_t result_payload[32];
    size_t command_payload_len = 0u;
    size_t result_payload_len = 0u;
    const struct mesh_downlink_entry *downlink;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    anchor_route.hop_count = 1u;
    assert(route_upsert_candidate(&relay.upstream, &relay_route) == PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &anchor_route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 21u, 2u, 70u, 1040u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 21u, 2u, 70u, 1050u);

    downlink = mesh_relay_find_downlink(&relay, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_A);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_B);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             300u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               2000u,
                               &gateway_command_tx) == PROTO_OK);
    assert(gateway_command_tx.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_command_tx.packet,
                                gateway_command_tx.payload,
                                gateway_command_tx.payload_len,
                                GATEWAY,
                                85u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == ANCHOR_A);
    assert(!mesh_relay_tx_active(&gateway));

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                70u,
                                2020u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&anchor_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_append_command_result(result_payload,
                                      sizeof(result_payload),
                                      &result_payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(mesh_init_command_result(&command_result,
                                    ANCHOR_A,
                                    GATEWAY,
                                    300u,
                                    2u,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(mesh_relay_start_tx(&anchor,
                               &command_result,
                               result_payload,
                               result_payload_len,
                               3000u,
                               &anchor_result_tx) == PROTO_OK);
    assert(anchor_result_tx.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &anchor_result_tx.packet,
                                anchor_result_tx.payload,
                                anchor_result_tx.payload_len,
                                ANCHOR_A,
                                70u,
                                3010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == GATEWAY);

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                85u,
                                3020u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
    assert(gateway_result.gateway_ack.packet.dst_id == ANCHOR_A);
}

static void test_gateway_delivers_direct_clicker_self_test_report_and_acks(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 710u,
        .failure_code = 5u,
        .battery_mv = 0u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 71u);

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        9u,
                                        (uint8_t)payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                fields.clicker_id,
                                95u,
                                3000u,
                                &result) == PROTO_OK);

    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.next_hop_id == fields.clicker_id);
    assert(result.gateway_ack.packet.msg_type == MSG_GATEWAY_ACK);
    assert(result.gateway_ack.packet.src_id == GATEWAY);
    assert(result.gateway_ack.packet.dst_id == fields.clicker_id);
    assert(result.gateway_ack.packet.session_id == fields.event_seq);
    assert((result.gateway_ack.packet.flags & FLAG_GATEWAY_ACK) != 0u);
    assert(tlv_find(result.gateway_ack.payload,
                    result.gateway_ack.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == packet.seq);
}

static void test_gateway_ack_previous_hop_fallback_must_not_be_rerouted(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    struct mesh_outbound rerouted;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 711u,
        .failure_code = 0u,
        .battery_mv = 0u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 72u);

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        10u,
                                        (uint8_t)payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                fields.clicker_id,
                                95u,
                                3100u,
                                &result) == PROTO_OK);

    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.next_hop_id == fields.clicker_id);
    assert(mesh_relay_start_tx(&gateway,
                               &result.gateway_ack.packet,
                               result.gateway_ack.payload,
                               result.gateway_ack.payload_len,
                               3110u,
                               &rerouted) == PROTO_ERR_NOT_FOUND);
}

static void test_duplicate_retry_repairs_lost_gateway_ack(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_relay gateway;
    struct route_candidate origin_route = direct_gateway_route(ANCHOR_B, 52u, 85u);
    struct route_candidate relay_route = direct_gateway_route(GATEWAY, 52u, 95u);
    struct proto_packet report;
    struct mesh_outbound origin_tx;
    struct mesh_outbound relay_tx;
    struct mesh_outbound relay_retry_tx;
    struct mesh_relay_result origin_result;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    uint8_t report_payload[1] = {0x46u};
    uint32_t gateway_ack_timeout_ms;

    origin_route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 52u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 52u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 52u);
    assert(route_upsert_candidate(&origin.upstream, &origin_route) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &relay_route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 52u, 2u, 80u, 1000u);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) != NULL);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    520u,
                                    6u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               2000u,
                               &origin_tx) == PROTO_OK);
    assert(origin_tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &origin_tx, 2000u);
    gateway_ack_timeout_ms = origin.pending.gateway_ack_deadline_ms + 1u;

    assert(mesh_relay_handle_rx(&relay,
                                &origin_tx.packet,
                                origin_tx.payload,
                                origin_tx.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_tx_active(&origin));

    assert(mesh_relay_start_tx(&relay,
                               &relay_result.forward.packet,
                               relay_result.forward.payload,
                               relay_result.forward.payload_len,
                               2021u,
                               &relay_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &relay_tx.packet,
                                relay_tx.payload,
                                relay_tx.payload_len,
                                ANCHOR_B,
                                90u,
                                2030u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&origin, gateway_ack_timeout_ms, &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(origin_result.retransmit.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &origin_result.retransmit, gateway_ack_timeout_ms);

    assert(mesh_relay_handle_rx(&relay,
                                &origin_result.retransmit.packet,
                                origin_result.retransmit.payload,
                                origin_result.retransmit.payload_len,
                                ANCHOR_A,
                                80u,
                                gateway_ack_timeout_ms + 10u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_ERR_STALE);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_start_tx(&relay,
                               &relay_result.forward.packet,
                               relay_result.forward.payload,
                               relay_result.forward.payload_len,
                               gateway_ack_timeout_ms + 21u,
                               &relay_retry_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &relay_retry_tx.packet,
                                relay_retry_tx.payload,
                                relay_retry_tx.payload_len,
                                ANCHOR_B,
                                90u,
                                gateway_ack_timeout_ms + 30u,
                                &gateway_result) == PROTO_OK);
    assert(gateway_result.status == PROTO_ERR_STALE);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                gateway_ack_timeout_ms + 50u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&origin,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                80u,
                                gateway_ack_timeout_ms + 60u,
                                &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
}

static void test_reactive_gateway_route_request_and_reply(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound report_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result anchor_result;
    struct proto_packet report;
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);
    uint8_t payload[1] = {0x66u};
    const struct route_candidate *selected;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 50u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 500u, 1u, sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1000u, &report_tx) ==
           PROTO_ERR_NOT_FOUND);

    assert(mesh_relay_build_route_request(&anchor, GATEWAY, &route_req, 1000u) == PROTO_OK);
    assert(route_req.packet.msg_type == MSG_ROUTE_REQ);
    assert(route_req.packet.dst_id == MESH_BROADCAST_ID);
    assert(route_req.next_hop_id == MESH_BROADCAST_ID);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1010u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.route_request.packet,
                                relay_result.route_request.payload,
                                relay_result.route_request.payload_len,
                                ANCHOR_B,
                                90u,
                                1020u,
                                &gateway_result) == PROTO_OK);
    assert(gateway_result.status == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(gateway_result.route_reply.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.route_reply.packet,
                                gateway_result.route_reply.payload,
                                gateway_result.route_reply.payload_len,
                                GATEWAY,
                                90u,
                                1030u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(relay_result.route_reply.next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.route_reply.packet,
                                relay_result.route_reply.payload,
                                relay_result.route_reply.payload_len,
                                ANCHOR_B,
                                70u,
                                1040u,
                                &anchor_result) == PROTO_OK);
    assert(anchor_result.status == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));

    selected = route_selected(&anchor.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->hop_count == 1u);
    assert(selected->link_quality == 70u);

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1050u, &report_tx) ==
           PROTO_OK);
    assert(report_tx.next_hop_id == ANCHOR_B);
}

static void test_malformed_route_request_does_not_poison_downlink_route(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 60u, 90u);
    const uint8_t *hop_value = NULL;
    uint8_t hop_value_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 60u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_build_route_request(&relay, GATEWAY, &route_req, 1000u) ==
           PROTO_OK);

    route_req.packet.src_id = ANCHOR_A;
    assert(tlv_find(route_req.payload,
                    route_req.payload_len,
                    TLV_HOP_COUNT,
                    &hop_value,
                    &hop_value_len) == PROTO_OK);
    assert(hop_value_len == 1u);
    route_req.payload[(size_t)(hop_value - route_req.payload) - 1u] = 2u;

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) == NULL);
}

static void test_malformed_route_reply_does_not_poison_upstream_route(void)
{
    struct mesh_relay anchor;
    struct proto_packet route_reply = {
        .msg_type = MSG_ROUTE_REPLY,
        .flags = 0u,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_A,
        .session_id = 1000u,
        .seq = 42u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *quality_value = NULL;
    uint8_t quality_value_len = 0u;

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 60u);
    assert(route_selected(&anchor.upstream) == NULL);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len, TLV_INITIATOR_ID, ANCHOR_A) ==
           PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len, TLV_RESPONDER_ID, GATEWAY) ==
           PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len, TLV_ROUTE_EPOCH, 60u) ==
           PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len, TLV_HOP_COUNT, 0u) ==
           PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len, TLV_QUALITY, 100u) ==
           PROTO_OK);
    route_reply.payload_len = (uint8_t)payload_len;

    assert(tlv_find(payload,
                    payload_len,
                    TLV_QUALITY,
                    &quality_value,
                    &quality_value_len) == PROTO_OK);
    assert(quality_value_len == 1u);
    payload[(size_t)(quality_value - payload) - 1u] = 2u;

    assert(mesh_relay_handle_rx(&anchor,
                                &route_reply,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                70u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(route_selected(&anchor.upstream) == NULL);
}

static void test_reactive_route_and_report_flow_over_uwb_mesh_frames(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound report_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result anchor_result;
    struct proto_packet decoded_packet;
    struct proto_packet report;
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);
    uint8_t decoded_payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload[1] = {0x66u};
    size_t decoded_payload_len = 0u;
    uint64_t previous_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 50u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 500u, 1u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1000u, &report_tx) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_build_route_request(&anchor, GATEWAY, &route_req, 1000u) == PROTO_OK);

    decode_outbound_over_uwb(&route_req,
                             ANCHOR_A,
                             ANCHOR_B,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&relay,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    decode_outbound_over_uwb(&relay_result.route_request,
                             ANCHOR_B,
                             GATEWAY,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&gateway,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                90u,
                                1020u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    decode_outbound_over_uwb(&gateway_result.route_reply,
                             GATEWAY,
                             ANCHOR_B,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&relay,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                90u,
                                1030u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    decode_outbound_over_uwb(&relay_result.route_reply,
                             ANCHOR_B,
                             ANCHOR_A,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&anchor,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1040u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1050u, &report_tx) ==
           PROTO_OK);
    assert(report_tx.next_hop_id == ANCHOR_B);
    decode_outbound_over_uwb(&report_tx,
                             ANCHOR_A,
                             ANCHOR_B,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&relay,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1060u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == GATEWAY);

    decode_outbound_over_uwb(&relay_result.forward,
                             ANCHOR_B,
                             GATEWAY,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&gateway,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                90u,
                                1070u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
}

static void test_reactive_gateway_discovers_downlink_anchor(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound command_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result anchor_result;
    struct mesh_relay_result gateway_result;
    struct proto_packet command;
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;
    const struct mesh_downlink_entry *downlink;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 60u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 60u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 60u);

    assert(mesh_relay_build_route_request(&gateway, ANCHOR_A, &route_req, 2000u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                GATEWAY,
                                85u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(route_selected(&relay.upstream) != NULL);

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.route_request.packet,
                                relay_result.route_request.payload,
                                relay_result.route_request.payload_len,
                                ANCHOR_B,
                                65u,
                                2020u,
                                &anchor_result) == PROTO_OK);
    assert(anchor_result.status == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(anchor_result.route_reply.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &anchor_result.route_reply.packet,
                                anchor_result.route_reply.payload,
                                anchor_result.route_reply.payload_len,
                                ANCHOR_A,
                                65u,
                                2030u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(relay_result.route_reply.next_hop_id == GATEWAY);
    downlink = mesh_relay_find_downlink(&relay, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.route_reply.packet,
                                relay_result.route_reply.payload,
                                relay_result.route_reply.payload_len,
                                ANCHOR_B,
                                85u,
                                2040u,
                                &gateway_result) == PROTO_OK);
    assert(gateway_result.status == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_B);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             600u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               2050u,
                               &command_tx) == PROTO_OK);
    assert(command_tx.next_hop_id == ANCHOR_B);
}

static void test_channel9_report_tx_requires_negotiated_event(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 70u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(2000u);
    uint8_t payload[1] = {0x77u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 70u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    700u,
                                    1u,
                                    sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        2000u,
                                        &plan,
                                        &tx) == PROTO_ERR_STALE);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        1999u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        2000u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(tx.next_hop_id == GATEWAY);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    assert(mesh_relay_tx_active(&relay));
}

static void test_channel9_report_delivery_and_gateway_ack_require_events(void)
{
    struct mesh_relay origin;
    struct mesh_relay gateway;
    struct route_candidate route = direct_gateway_route(GATEWAY, 71u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result origin_result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(3000u);
    uint8_t payload[1] = {0x78u};

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 71u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 71u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&origin, GATEWAY, &timing) == PROTO_OK);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    710u,
                                    2u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&origin,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        3000u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    mesh_relay_note_channel9_success(&origin, tx.next_hop_id, plan.start_ms);
    mesh_relay_note_tx_sent(&origin, &tx, 3000u);

    assert(mesh_relay_handle_rx(&gateway,
                                &tx.packet,
                                tx.payload,
                                tx.payload_len,
                                ANCHOR_A,
                                90u,
                                3010u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_A);

    assert(mesh_relay_require_channel9_event(&gateway,
                                             gateway_result.gateway_ack.next_hop_id,
                                             &requirements,
                                             3010u,
                                             &plan) == PROTO_ERR_STALE);
    params = channel9_params(3010u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&gateway, ANCHOR_A, &timing) == PROTO_OK);
    assert(mesh_relay_require_channel9_event(&gateway,
                                             gateway_result.gateway_ack.next_hop_id,
                                             &requirements,
                                             3010u,
                                             &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);

    assert(mesh_relay_handle_rx(&origin,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                3020u,
                                &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
}

static void test_channel9_rx_observation_self_heals_event_timing(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(4000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 80u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);

    mesh_relay_note_channel9_rx(&relay, GATEWAY, 4000u, 4018u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             4108u,
                                             &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 4108u);
}

static void test_channel9_sender_skips_channel5_preempted_event_without_refresh(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 82u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(6000u);
    uint8_t payload[1] = {0x79u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 82u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    720u,
                                    1u,
                                    sizeof(payload)) == PROTO_OK);

    requirements.next_required_scan_start_ms = 6003u;
    requirements.retune_guard_ms = 5u;
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6000u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);
    assert(relay.event_timings[0].timing.next_event_time_ms == 6100u);

    requirements = clear_channel5_requirements();
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6050u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(plan.start_ms == 6100u);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6100u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
}

static void test_channel9_receiver_miss_advances_timing_and_diagnostics(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    struct mesh_event_params params = channel9_params(7000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 83u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_A, &timing) == PROTO_OK);

    mesh_relay_note_channel9_missed(&relay, ANCHOR_A, &diagnostics);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);
    assert(relay.event_timings[0].timing.next_event_time_ms == 7100u);
    assert(diagnostics.ch9_event_misses == 1u);
}

static void test_channel9_timing_expires_idle_connection_state(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(5000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 81u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);

    assert(mesh_relay_expire_channel9_timings(&relay, 5200u) == 0u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             5200u,
                                             &plan) == PROTO_OK);
    assert(mesh_relay_expire_channel9_timings(&relay, 5500u) == 1u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             5500u,
                                             &plan) == PROTO_ERR_STALE);
}

int main(void)
{
    test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate();
    test_duplicate_cache_expires_by_time_window();
    test_status_tlvs_report_selected_route();
    test_status_tlvs_report_missing_route_reason();
    test_legacy_route_beacons_are_dropped();
    test_survey_discovery_broadcast_delivers_and_floods();
    test_broadcast_command_delivers_without_flooding();
    test_busy_survey_discovery_broadcast_still_forwards();
    test_downlink_routes_survive_age_until_delivery_failure();
    test_downlink_route_selection_uses_weighted_quality();
    test_start_tx_accepts_aged_upstream_route_until_failures();
    test_downlink_next_hop_send_completes_immediately();
    test_downlink_send_refreshes_route_age();
    test_ttl_zero_packet_is_dropped_without_ack();
    test_zero_session_or_sequence_mesh_packets_are_rejected();
    test_busy_relay_does_not_ack_or_cache_new_forward();
    test_busy_relay_drops_duplicate_forward();
    test_local_gateway_bound_tx_waits_for_gateway_ack();
    test_relayed_tx_completes_after_next_hop_send();
    test_gateway_ack_timeout_retries_then_requests_route_discovery();
    test_gateway_ack_timeout_handles_ms_wrap();
    test_gateway_reaches_anchor_behind_relay_and_receives_result();
    test_gateway_delivers_direct_clicker_self_test_report_and_acks();
    test_gateway_ack_previous_hop_fallback_must_not_be_rerouted();
    test_duplicate_retry_repairs_lost_gateway_ack();
    test_reactive_gateway_route_request_and_reply();
    test_malformed_route_request_does_not_poison_downlink_route();
    test_malformed_route_reply_does_not_poison_upstream_route();
    test_reactive_route_and_report_flow_over_uwb_mesh_frames();
    test_reactive_gateway_discovers_downlink_anchor();
    test_channel9_report_tx_requires_negotiated_event();
    test_channel9_report_delivery_and_gateway_ack_require_events();
    test_channel9_rx_observation_self_heals_event_timing();
    test_channel9_sender_skips_channel5_preempted_event_without_refresh();
    test_channel9_receiver_miss_advances_timing_and_diagnostics();
    test_channel9_timing_expires_idle_connection_state();
    return 0;
}
