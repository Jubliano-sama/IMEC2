#include "mesh_relay.h"

#include "mesh.h"
#include "report.h"

#include <assert.h>
#include <string.h>

#define ANCHOR_A 0x1111222233334444ull
#define ANCHOR_B 0x5555666677778888ull
#define GATEWAY  0x9999888877776666ull

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

static size_t route_status_payload(uint8_t *payload,
                                   size_t payload_cap,
                                   uint64_t anchor_id,
                                   uint64_t gateway_id,
                                   uint64_t next_hop_id,
                                   uint32_t epoch,
                                   uint8_t hop_count,
                                   uint8_t quality)
{
    size_t offset = 0u;

    assert(tlv_append_u64(payload, payload_cap, &offset, TLV_ANCHOR_ID, anchor_id) == PROTO_OK);
    assert(tlv_append_u64(payload, payload_cap, &offset, TLV_GATEWAY_ID, gateway_id) == PROTO_OK);
    assert(tlv_append_u64(payload, payload_cap, &offset, TLV_NEXT_HOP_ID, next_hop_id) == PROTO_OK);
    assert(tlv_append_u32(payload, payload_cap, &offset, TLV_ROUTE_EPOCH, epoch) == PROTO_OK);
    assert(tlv_append_u8(payload, payload_cap, &offset, TLV_HOP_COUNT, hop_count) == PROTO_OK);
    assert(tlv_append_u8(payload, payload_cap, &offset, TLV_QUALITY, quality) == PROTO_OK);
    assert(tlv_append_u8(payload, payload_cap, &offset, TLV_RETRY_COUNT, 0u) == PROTO_OK);
    return offset;
}

static void test_route_adv_forms_upstream_route_and_readvertises(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound route_adv;
    struct mesh_relay_result result;
    const struct route_candidate *selected;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 7u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    assert(mesh_relay_build_route_adv(&gateway, &route_adv, 1000u) == PROTO_OK);
    assert(route_adv.packet.msg_type == MSG_ROUTE_ADV);
    assert(route_adv.packet.dst_id == MESH_BROADCAST_ID);

    assert(mesh_relay_handle_rx(&anchor,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                80u,
                                1100u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_ADV));

    selected = route_selected(&anchor.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == GATEWAY);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->route_epoch == 7u);
    assert(selected->hop_count == 0u);
    assert(selected->link_quality == 80u);

    assert(result.route_status.packet.msg_type == MSG_ROUTE_STATUS);
    assert(result.route_status.packet.src_id == ANCHOR_A);
    assert(result.route_status.packet.dst_id == GATEWAY);
    assert(result.route_status.next_hop_id == GATEWAY);
    assert((result.route_status.packet.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((result.route_status.packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);

    assert(tlv_find(result.route_adv.payload,
                    result.route_adv.payload_len,
                    TLV_HOP_COUNT,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 1u);
}

static void test_route_refresh_is_throttled_until_due(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound route_adv;
    struct mesh_relay_result result;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 7u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    assert(mesh_relay_build_route_adv(&gateway, &route_adv, 1000u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                80u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_ADV));

    assert(mesh_relay_build_route_adv(&gateway, &route_adv, 2000u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                80u,
                                2000u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_ADV));

    assert(mesh_relay_build_route_adv(&gateway,
                                      &route_adv,
                                      1000u + MESH_RELAY_ROUTE_REFRESH_MS) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                80u,
                                1000u + MESH_RELAY_ROUTE_REFRESH_MS,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_ADV));
}

static void test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0xAAu};
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

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
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(result.hop_ack.packet.dst_id == ANCHOR_A);
    assert(tlv_find(result.hop_ack.payload,
                    result.hop_ack.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == report.seq);

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
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
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
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_HOP_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_HOP_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);

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

static void test_gateway_caches_downlink_and_returns_gateway_ack(void)
{
    struct mesh_relay gateway;
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 12u,
        .seq = 3u,
        .ttl = 3u,
    };
    uint8_t status_payload[96];
    struct mesh_relay_result result;
    const struct mesh_downlink_entry *downlink;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 12u);
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              12u,
                                                              2u,
                                                              75u);

    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                70u,
                                3000u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_B);
    assert(downlink->gateway_id == GATEWAY);
    assert(downlink->route_epoch == 12u);
    assert(downlink->hop_count == 2u);
    assert(downlink->quality == 70u);

    assert(result.gateway_ack.packet.msg_type == MSG_GATEWAY_ACK);
    assert((result.gateway_ack.packet.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((result.gateway_ack.packet.flags & FLAG_GATEWAY_ACK) != 0u);
    assert(result.gateway_ack.next_hop_id == ANCHOR_B);
    assert(result.gateway_ack.packet.dst_id == ANCHOR_A);
    assert(tlv_find(result.gateway_ack.payload,
                    result.gateway_ack.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == status_packet.seq);

    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                70u,
                                3001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

static void test_downlink_routes_expire_when_not_refreshed(void)
{
    struct mesh_relay gateway;
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 30u,
        .seq = 1u,
        .ttl = 3u,
    };
    uint8_t status_payload[96];
    struct mesh_relay_result result;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 30u);
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              30u,
                                                              2u,
                                                              75u);

    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                70u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(mesh_relay_expire_routes(&gateway, 31001u) == 1u);
    assert(mesh_relay_find_downlink(&gateway, ANCHOR_A) == NULL);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_ERR_NOT_FOUND);
}

static void test_downlink_route_selection_uses_weighted_quality(void)
{
    struct mesh_relay gateway;
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 31u,
        .seq = 1u,
        .ttl = 3u,
    };
    uint8_t status_payload[96];
    struct mesh_relay_result result;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 31u);
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              GATEWAY,
                                                              31u,
                                                              1u,
                                                              0u);
    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_A,
                                0u,
                                1000u,
                                &result) == PROTO_OK);

    status_packet.seq = 2u;
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              31u,
                                                              2u,
                                                              100u);
    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                100u,
                                1100u,
                                &result) == PROTO_OK);

    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
}

static void test_start_tx_rejects_stale_upstream_route(void)
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
                               &tx) == PROTO_ERR_NOT_FOUND);
    assert(route_selected(&relay.upstream) == NULL);
}

static void test_downlink_failure_retries_alternate_route(void)
{
    struct mesh_relay gateway;
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 33u,
        .seq = 1u,
        .ttl = 3u,
    };
    struct proto_packet command;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t status_payload[96];
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 33u);

    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              33u,
                                                              2u,
                                                              100u);
    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                100u,
                                1000u,
                                &result) == PROTO_OK);

    status_packet.seq = 2u;
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              GATEWAY,
                                                              33u,
                                                              1u,
                                                              0u);
    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_A,
                                0u,
                                1010u,
                                &result) == PROTO_OK);

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

    uint32_t now_ms = 2000u + ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(0u) + 1u;

    assert(mesh_relay_tick(&gateway, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == ANCHOR_B);

    now_ms += ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(1u) + 1u;
    assert(mesh_relay_tick(&gateway, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == ANCHOR_B);

    now_ms += ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(2u) + 1u;
    assert(mesh_relay_tick(&gateway, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(result.retransmit.next_hop_id == ANCHOR_A);
    assert(mesh_relay_tx_active(&gateway));
}

static void test_downlink_hop_ack_refreshes_route_age(void)
{
    struct mesh_relay gateway;
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 34u,
        .seq = 1u,
        .ttl = 3u,
    };
    struct proto_packet command;
    struct proto_packet ack;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    const struct mesh_downlink_entry *downlink;
    uint8_t status_payload[96];
    uint8_t command_payload[16];
    uint8_t ack_payload[16];
    size_t command_payload_len = 0u;
    size_t ack_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 34u);
    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              34u,
                                                              2u,
                                                              80u);
    assert(mesh_relay_handle_rx(&gateway,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_B,
                                80u,
                                1000u,
                                &result) == PROTO_OK);

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

    assert(mesh_append_requested_seq(ack_payload, sizeof(ack_payload), &ack_payload_len, command.seq) ==
           PROTO_OK);
    assert(mesh_init_hop_ack(&ack,
                             ANCHOR_B,
                             GATEWAY,
                             command.session_id,
                             1u,
                             (uint8_t)ack_payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                7450u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));

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
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
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
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
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
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_relay_does_not_ack_duplicate_forward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_report;
    struct mesh_outbound tracked_forward;
    struct mesh_relay_result result;
    uint8_t incoming_payload[1] = {0x45u};

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
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               4201u,
                               &tracked_forward) == PROTO_OK);
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
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_local_gateway_bound_tx_waits_for_hop_then_gateway_ack(void)
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
    assert(mesh_init_hop_ack(&ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             report.session_id,
                             1u,
                             (uint8_t)ack_payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5050u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));
    assert(mesh_relay_tx_active(&relay));

    ack_payload_len = 0u;
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

static void test_relayed_tx_completes_after_hop_ack_only(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet ack;
    uint8_t ack_payload[16];
    size_t ack_payload_len = 0u;
    uint8_t payload[1] = {0x43u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 89u, 8u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 6000u, &tx) == PROTO_OK);
    assert(tx.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_append_requested_seq(ack_payload, sizeof(ack_payload), &ack_payload_len, report.seq) == PROTO_OK);
    assert(mesh_init_hop_ack(&ack,
                             GATEWAY,
                             ANCHOR_B,
                             report.session_id,
                             3u,
                             (uint8_t)ack_payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                95u,
                                6050u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));
}

static void test_hop_ack_timeout_retries_then_requests_route_discovery(void)
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

    uint32_t now_ms = 7000u;

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), now_ms, &tx) == PROTO_OK);

    now_ms += ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(0u) + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&relay));

    now_ms += ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(1u) + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(mesh_relay_tx_active(&relay));

    now_ms += ROUTE_HOP_ACK_TIMEOUT_MS + route_retry_backoff_ms(2u) + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(!mesh_relay_tx_active(&relay));
    assert(route_selected(&relay.upstream) == NULL);
}

static void test_gateway_reaches_anchor_behind_relay_and_receives_result(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound gateway_adv;
    struct mesh_outbound gateway_command_tx;
    struct mesh_outbound anchor_result_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result anchor_result;
    struct mesh_relay_result gateway_result;
    struct proto_packet command = {0};
    struct proto_packet command_result = {0};
    struct proto_packet gateway_hop_ack = {0};
    uint8_t command_payload[16];
    uint8_t gateway_hop_ack_payload[16];
    uint8_t result_payload[32];
    size_t command_payload_len = 0u;
    size_t gateway_hop_ack_payload_len = 0u;
    size_t result_payload_len = 0u;
    const struct mesh_downlink_entry *downlink;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    assert(mesh_relay_build_route_adv(&gateway, &gateway_adv, 1000u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &gateway_adv.packet,
                                gateway_adv.payload,
                                gateway_adv.payload_len,
                                GATEWAY,
                                85u,
                                1010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_ADV));

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.route_status.packet,
                                relay_result.route_status.payload,
                                relay_result.route_status.payload_len,
                                ANCHOR_B,
                                85u,
                                1020u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.route_adv.packet,
                                relay_result.route_adv.payload,
                                relay_result.route_adv.payload_len,
                                ANCHOR_B,
                                70u,
                                1030u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_SEND_ROUTE_STATUS));

    assert(mesh_relay_handle_rx(&relay,
                                &anchor_result.route_status.packet,
                                anchor_result.route_status.payload,
                                anchor_result.route_status.payload_len,
                                ANCHOR_A,
                                70u,
                                1040u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    downlink = mesh_relay_find_downlink(&relay, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                85u,
                                1050u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
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
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == ANCHOR_A);

    assert(mesh_append_requested_seq(gateway_hop_ack_payload,
                                     sizeof(gateway_hop_ack_payload),
                                     &gateway_hop_ack_payload_len,
                                     command.seq) == PROTO_OK);
    assert(mesh_init_hop_ack(&gateway_hop_ack,
                             ANCHOR_B,
                             GATEWAY,
                             command.session_id,
                             9u,
                             (uint8_t)gateway_hop_ack_payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &gateway_hop_ack,
                                gateway_hop_ack_payload,
                                gateway_hop_ack_payload_len,
                                ANCHOR_B,
                                85u,
                                2015u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));
    assert(!mesh_relay_tx_active(&gateway));

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                70u,
                                2020u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
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
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
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
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
    assert(gateway_result.gateway_ack.packet.dst_id == ANCHOR_A);
}

static void test_duplicate_retry_repairs_lost_gateway_ack(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_relay gateway;
    struct route_candidate origin_route = direct_gateway_route(ANCHOR_B, 52u, 85u);
    struct route_candidate relay_route = direct_gateway_route(GATEWAY, 52u, 95u);
    struct proto_packet status_packet = {
        .msg_type = MSG_ROUTE_STATUS,
        .flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 52u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet report;
    struct mesh_outbound origin_tx;
    struct mesh_outbound relay_tx;
    struct mesh_outbound relay_retry_tx;
    struct mesh_relay_result origin_result;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    uint8_t status_payload[96];
    uint8_t report_payload[1] = {0x46u};
    uint32_t gateway_ack_timeout_ms;

    origin_route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 52u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 52u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 52u);
    assert(route_upsert_candidate(&origin.upstream, &origin_route) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &relay_route) == PROTO_OK);

    status_packet.payload_len = (uint8_t)route_status_payload(status_payload,
                                                              sizeof(status_payload),
                                                              ANCHOR_A,
                                                              GATEWAY,
                                                              ANCHOR_B,
                                                              52u,
                                                              2u,
                                                              80u);
    assert(mesh_relay_handle_rx(&relay,
                                &status_packet,
                                status_payload,
                                status_packet.payload_len,
                                ANCHOR_A,
                                80u,
                                1000u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
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

    assert(mesh_relay_handle_rx(&relay,
                                &origin_tx.packet,
                                origin_tx.payload,
                                origin_tx.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_handle_rx(&origin,
                                &relay_result.hop_ack.packet,
                                relay_result.hop_ack.payload,
                                relay_result.hop_ack.payload_len,
                                ANCHOR_B,
                                80u,
                                2020u,
                                &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));
    assert(mesh_relay_tx_active(&origin));
    gateway_ack_timeout_ms = origin.pending.gateway_ack_deadline_ms + 1u;

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

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.hop_ack.packet,
                                gateway_result.hop_ack.payload,
                                gateway_result.hop_ack.payload_len,
                                GATEWAY,
                                90u,
                                2040u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&origin, gateway_ack_timeout_ms, &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(origin_result.retransmit.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &origin_result.retransmit.packet,
                                origin_result.retransmit.payload,
                                origin_result.retransmit.payload_len,
                                ANCHOR_A,
                                80u,
                                gateway_ack_timeout_ms + 10u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_ERR_STALE);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_handle_rx(&origin,
                                &relay_result.hop_ack.packet,
                                relay_result.hop_ack.payload,
                                relay_result.hop_ack.payload_len,
                                ANCHOR_B,
                                80u,
                                gateway_ack_timeout_ms + 20u,
                                &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));

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
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.hop_ack.packet,
                                gateway_result.hop_ack.payload,
                                gateway_result.hop_ack.payload_len,
                                GATEWAY,
                                90u,
                                gateway_ack_timeout_ms + 40u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_TX_HOP_CONFIRMED));

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                gateway_ack_timeout_ms + 50u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
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

int main(void)
{
    test_route_adv_forms_upstream_route_and_readvertises();
    test_route_refresh_is_throttled_until_due();
    test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate();
    test_duplicate_cache_expires_by_time_window();
    test_status_tlvs_report_selected_route();
    test_status_tlvs_report_missing_route_reason();
    test_gateway_caches_downlink_and_returns_gateway_ack();
    test_downlink_routes_expire_when_not_refreshed();
    test_downlink_route_selection_uses_weighted_quality();
    test_start_tx_rejects_stale_upstream_route();
    test_downlink_failure_retries_alternate_route();
    test_downlink_hop_ack_refreshes_route_age();
    test_ttl_zero_packet_is_dropped_without_ack();
    test_busy_relay_does_not_ack_or_cache_new_forward();
    test_busy_relay_does_not_ack_duplicate_forward();
    test_local_gateway_bound_tx_waits_for_hop_then_gateway_ack();
    test_relayed_tx_completes_after_hop_ack_only();
    test_hop_ack_timeout_retries_then_requests_route_discovery();
    test_gateway_reaches_anchor_behind_relay_and_receives_result();
    test_duplicate_retry_repairs_lost_gateway_ack();
    return 0;
}
