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

static void test_relay_forwards_gateway_bound_packet_and_suppresses_duplicate(void)
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
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
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
}

static void test_ttl_zero_packet_is_acknowledged_but_not_forwarded(void)
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
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
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
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 90u, 9u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 7000u, &tx) == PROTO_OK);

    assert(mesh_relay_tick(&relay, 7251u, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&relay, 7651u, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&relay, 8301u, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(!mesh_relay_tx_active(&relay));
    assert(route_selected(&relay.upstream) == NULL);
}

int main(void)
{
    test_route_adv_forms_upstream_route_and_readvertises();
    test_relay_forwards_gateway_bound_packet_and_suppresses_duplicate();
    test_gateway_caches_downlink_and_returns_gateway_ack();
    test_ttl_zero_packet_is_acknowledged_but_not_forwarded();
    test_local_gateway_bound_tx_waits_for_hop_then_gateway_ack();
    test_relayed_tx_completes_after_hop_ack_only();
    test_hop_ack_timeout_retries_then_requests_route_discovery();
    return 0;
}
