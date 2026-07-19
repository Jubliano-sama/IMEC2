#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORIGIN_ID UINT64_C(0x000000000000e100)
#define CHILD_ID UINT64_C(0x000000000000e101)
#define PARENT_ID UINT64_C(0x000000000000e102)
#define GATEWAY_ID UINT64_C(0x000000000000e1ff)
#define ROUTE_EPOCH UINT32_C(53)
#define REQUEST_TX_US UINT64_C(10000)
#define RX_GUARD_US UINT64_C(100)

enum response_window_case {
    RESPONSE_WINDOW_VALID = 0,
    RESPONSE_WINDOW_SHIFTED,
    RESPONSE_WINDOW_SHORT,
};

struct fixture {
    struct mesh_sim_world world;
    uint8_t origin;
    uint8_t child;
    uint8_t parent;
    uint8_t gateway;
};

static int failures;
static const char *phase;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__, phase); \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
            failures++;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

static int setup_fixture(struct fixture *fixture)
{
    int ret;

    mesh_sim_init(&fixture->world, UINT32_C(0xe1000053));
    ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->origin);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            CHILD_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->child);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            PARENT_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->parent);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->gateway);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_set_link(&fixture->world, fixture->origin, fixture->child,
                            94u, 2u);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->child, fixture->parent,
                                96u, 2u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->parent,
                                fixture->gateway, 98u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_route(&fixture->world, fixture->origin,
                                     fixture->child, 2u, ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_route(&fixture->world, fixture->child,
                                     fixture->parent, 1u, ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_route(&fixture->world, fixture->parent,
                                     fixture->gateway, 0u, ROUTE_EPOCH);
    }
    return ret;
}

static int make_parent_busy(struct fixture *fixture)
{
    struct mesh_sim_role_instance *parent =
        mesh_sim_role(&fixture->world, fixture->parent);
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = PARENT_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0xe1020001),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    struct mesh_outbound outbound;
    int ret = mesh_relay_start_tx(&parent->relay, &packet, NULL, 0u, 1u,
                                  &outbound);

    if (ret == PROTO_OK) {
        mesh_relay_note_tx_sent(&parent->relay, &outbound, 1u);
    }
    return ret;
}

static int build_result_payload(struct proto_packet *packet,
                                uint8_t *payload,
                                size_t payload_capacity,
                                size_t *payload_len)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = (uint16_t)ROUTE_EPOCH,
        .command_seq = UINT32_C(0xe1010053),
        .node_id = CHILD_ID,
        .node_boot_counter = 7u,
        .result_seq = 8u,
    };
    uint8_t padding[24];
    int ret;

    *payload_len = 0u;
    ret = command_result_id_append_tlvs(payload, payload_capacity,
                                        payload_len, &result_id);
    if (ret == PROTO_OK) {
        ret = mesh_append_command_result(payload, payload_capacity,
                                         payload_len, CMD_GET_STATUS,
                                         COMMAND_OK, 0u);
    }
    memset(padding, 0xa5, sizeof(padding));
    while (ret == PROTO_OK && *payload_len < 64u) {
        size_t remaining = 64u - *payload_len;
        uint8_t chunk_len = remaining > sizeof(padding) + 2u ?
                            (uint8_t)sizeof(padding) :
                            (uint8_t)(remaining - 2u);

        ret = tlv_append_bytes(payload, payload_capacity, payload_len,
                               TLV_MESH_TEST_PADDING, padding, chunk_len);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_init_command_result(packet, CHILD_ID, GATEWAY_ID,
                                    UINT32_C(0xe1010053), 8u,
                                    (uint8_t)*payload_len, false);
}

static int apply_window_case(struct fixture *fixture,
                             enum response_window_case window_case)
{
    struct mesh_sim_contact_response_timing timing;

    if (window_case == RESPONSE_WINDOW_VALID) {
        return MESH_SIM_OK;
    }
    timing = (struct mesh_sim_contact_response_timing) {
        .rx_delay_us = window_case == RESPONSE_WINDOW_SHIFTED ? 8000u : 900u,
        .rx_window_us = window_case == RESPONSE_WINDOW_SHIFTED ? 2000u : 200u,
        .tx_delay_us = MESH_SIM_C5_RESPONSE_TX_DELAY_US,
    };
    return mesh_sim_override_next_contact_response_timing(&fixture->world,
                                                           &timing);
}

static uint64_t transmission_evaluation_us(
    const struct mesh_sim_world *world,
    uint16_t transmission_index)
{
    const struct mesh_sim_transmission *tx =
        &world->transmissions[transmission_index];
    uint16_t maximum_propagation_us = 0u;

    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->reachable[tx->node_index][i] &&
            world->propagation_us[tx->node_index][i] >
                maximum_propagation_us) {
            maximum_propagation_us =
                world->propagation_us[tx->node_index][i];
        }
    }
    return tx->end_us + maximum_propagation_us;
}

static int transmit_request_until_response(struct fixture *fixture,
                                           const struct mesh_outbound *request,
                                           uint16_t *response_index)
{
    enum mesh_sim_phy phy;
    uint32_t duration_us;
    uint64_t arrival_end_us;
    size_t transmission_count_before;
    uint16_t request_index;
    uint8_t channel;
    int ret;

    if (response_index == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_sim_outbound_radio(request, &channel, &phy);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    duration_us = mesh_sim_frame_duration_us(
        phy, proto_packet_encoded_len(request->payload_len));
    if (duration_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }
    arrival_end_us = REQUEST_TX_US + duration_us +
        fixture->world.propagation_us[fixture->child][fixture->parent];
    transmission_count_before = fixture->world.transmission_count;
    ret = mesh_sim_schedule_rx(&fixture->world,
                               fixture->parent,
                               REQUEST_TX_US - RX_GUARD_US,
                               arrival_end_us + RX_GUARD_US,
                               channel,
                               phy,
                               NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_schedule_outbound_tx(&fixture->world,
                                            fixture->child,
                                            REQUEST_TX_US,
                                            request,
                                            &request_index);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(
            &fixture->world,
            transmission_evaluation_us(&fixture->world, request_index));
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    for (size_t i = transmission_count_before;
         i < fixture->world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx =
            &fixture->world.transmissions[i];

        if (tx->has_outbound && tx->node_index == fixture->parent &&
            tx->outbound.next_hop_id == CHILD_ID) {
            *response_index = (uint16_t)i;
            return MESH_SIM_OK;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int receive_response(struct fixture *fixture, uint16_t response_index)
{
    return mesh_sim_run_until(
        &fixture->world,
        transmission_evaluation_us(&fixture->world, response_index));
}

static int transmit_route_reply_until_ack(
    struct fixture *fixture,
    const struct mesh_outbound *reply,
    uint16_t *ack_index)
{
    enum mesh_sim_phy phy;
    uint64_t tx_start_us;
    uint64_t arrival_end_us;
    uint32_t duration_us;
    uint16_t reply_index;
    uint8_t channel;
    int ret;

    if (ack_index == NULL ||
        mesh_sim_outbound_radio(reply, &channel, &phy) != MESH_SIM_OK) {
        return MESH_SIM_ERR_ARG;
    }
    duration_us = mesh_sim_frame_duration_us(
        phy, proto_packet_encoded_len(reply->payload_len));
    tx_start_us = REQUEST_TX_US;
    if (tx_start_us < (uint64_t)reply->earliest_tx_ms * 1000u) {
        tx_start_us = (uint64_t)reply->earliest_tx_ms * 1000u;
    }
    arrival_end_us = tx_start_us + duration_us +
        fixture->world.propagation_us[fixture->parent][fixture->child];
    ret = mesh_sim_schedule_rx(&fixture->world,
                               fixture->child,
                               tx_start_us - RX_GUARD_US,
                               arrival_end_us + RX_GUARD_US,
                               channel,
                               phy,
                               NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_schedule_outbound_tx(&fixture->world,
                                            fixture->parent,
                                            tx_start_us,
                                            reply,
                                            &reply_index);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(
            &fixture->world,
            transmission_evaluation_us(&fixture->world, reply_index));
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    for (size_t i = (size_t)reply_index + 1u;
         i < fixture->world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx =
            &fixture->world.transmissions[i];

        if (tx->has_outbound && tx->node_index == fixture->child &&
            tx->outbound.packet.msg_type == MSG_ROUTE_REPLY_ACK &&
            tx->outbound.next_hop_id == PARENT_ID) {
            *ack_index = (uint16_t)i;
            return MESH_SIM_OK;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static size_t response_reception_count(const struct fixture *fixture,
                                       uint8_t msg_type,
                                       enum mesh_sim_rx_outcome outcome)
{
    size_t count = 0u;

    for (size_t i = 0u; i < fixture->world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &fixture->world.receptions[i];

        if (rx->source_id == PARENT_ID && rx->receiver_id == CHILD_ID &&
            rx->packet.msg_type == msg_type && rx->outcome == outcome) {
            count++;
        }
    }
    return count;
}

static size_t route_ack_reception_count(const struct fixture *fixture,
                                        enum mesh_sim_rx_outcome outcome)
{
    size_t count = 0u;

    for (size_t i = 0u; i < fixture->world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &fixture->world.receptions[i];

        if (rx->source_id == CHILD_ID && rx->receiver_id == PARENT_ID &&
            rx->packet.msg_type == MSG_ROUTE_REPLY_ACK &&
            rx->outcome == outcome) {
            count++;
        }
    }
    return count;
}

static uint8_t response_transmission_type(const struct fixture *fixture)
{
    for (size_t i = 0u; i < fixture->world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx =
            &fixture->world.transmissions[i];

        if (tx->has_outbound && tx->outbound.packet.src_id == PARENT_ID &&
            tx->outbound.packet.dst_id == CHILD_ID) {
            return tx->outbound.packet.msg_type;
        }
    }
    return 0u;
}

static void run_relay_busy_case(enum response_window_case window_case)
{
    static struct fixture fixture;
    struct mesh_sim_role_instance *child;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = CHILD_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0xe1010001),
        .seq = 2u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    struct mesh_outbound request;
    struct mesh_pending_tx before;
    uint16_t response_index;
    int ret;

    phase = window_case == RESPONSE_WINDOW_VALID ? "relay-busy-valid" :
            window_case == RESPONSE_WINDOW_SHIFTED ? "relay-busy-shifted" :
                                                    "relay-busy-short";
    CHECK(setup_fixture(&fixture) == MESH_SIM_OK, "fixture setup failed");
    CHECK(make_parent_busy(&fixture) == PROTO_OK, "parent busy setup failed");
    child = mesh_sim_role(&fixture.world, fixture.child);
    CHECK(mesh_relay_start_tx(&child->relay, &packet, NULL, 0u, 9u,
                              &request) == PROTO_OK,
          "child request setup failed");
    CHECK(apply_window_case(&fixture, window_case) == MESH_SIM_OK,
          "response timing setup failed");
    ret = transmit_request_until_response(&fixture, &request,
                                          &response_index);
    CHECK(ret == MESH_SIM_OK, "request exchange failed: %d", ret);
    CHECK(response_transmission_type(&fixture) == MSG_RELAY_BUSY,
          "relay did not transmit RELAY_BUSY");
    before = child->relay.pending;
    ret = receive_response(&fixture, response_index);
    CHECK(ret == MESH_SIM_OK, "response exchange failed: %d", ret);

    if (window_case == RESPONSE_WINDOW_VALID) {
        CHECK(response_reception_count(&fixture, MSG_RELAY_BUSY,
                                       MESH_SIM_RX_DECODED) == 1u,
              "valid RELAY_BUSY was not decoded exactly once");
        CHECK(child->relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF &&
                  child->relay.pending.retry_after_ms != 0u,
              "decoded RELAY_BUSY did not apply bounded backoff");
    } else {
        CHECK(response_reception_count(&fixture, MSG_RELAY_BUSY,
                                       MESH_SIM_RX_DECODED) == 0u,
              "invalid response window decoded RELAY_BUSY");
        CHECK(memcmp(&child->relay.pending, &before, sizeof(before)) == 0,
              "failed RELAY_BUSY decode mutated child state");
        if (window_case == RESPONSE_WINDOW_SHORT) {
            CHECK(fixture.world.roles[fixture.child].partial_frames > 0u,
                  "short response window did not produce an explicit partial");
        }
    }
}

static void run_multihop_transit_busy_addressing_case(void)
{
    static struct fixture fixture;
    const struct mesh_sim_transmission *busy_tx;
    const struct mesh_sim_reception *busy_rx = NULL;
    struct mesh_sim_invariant_report report;
    struct mesh_outbound request = {
        .packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = ORIGIN_ID,
            .dst_id = GATEWAY_ID,
            .session_id = UINT32_C(0xe1000002),
            .seq = 3u,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 0u,
        },
        .payload_len = 0u,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = PARENT_ID,
    };
    uint16_t response_index;
    int ret;

    phase = "multihop-transit-busy-addressing";
    CHECK(setup_fixture(&fixture) == MESH_SIM_OK, "fixture setup failed");
    CHECK(!fixture.world.reachable[fixture.parent][fixture.origin],
          "busy relay unexpectedly has a direct RF link to the origin");
    CHECK(make_parent_busy(&fixture) == PROTO_OK, "parent busy setup failed");

    ret = transmit_request_until_response(&fixture, &request,
                                          &response_index);
    CHECK(ret == MESH_SIM_OK, "transit request exchange failed: %d", ret);
    busy_tx = &fixture.world.transmissions[response_index];
    CHECK(busy_tx->node_index == fixture.parent && busy_tx->has_outbound,
          "BUSY response was not transmitted by the busy relay");
    CHECK(busy_tx->outbound.packet.msg_type == MSG_RELAY_BUSY,
          "busy relay transmitted message type %u",
          busy_tx->outbound.packet.msg_type);
    CHECK(busy_tx->outbound.packet.dst_id == ORIGIN_ID,
          "BUSY semantic destination was not the original source");
    CHECK(busy_tx->outbound.next_hop_id == CHILD_ID,
          "BUSY physical next hop was not the previous relay");

    ret = receive_response(&fixture, response_index);
    CHECK(ret == MESH_SIM_OK, "BUSY response exchange failed: %d", ret);
    for (size_t i = 0u; i < fixture.world.reception_count; i++) {
        const struct mesh_sim_reception *candidate =
            &fixture.world.receptions[i];

        if (candidate->source_id == PARENT_ID &&
            candidate->receiver_id == CHILD_ID &&
            candidate->packet.msg_type == MSG_RELAY_BUSY &&
            candidate->outcome == MESH_SIM_RX_DECODED) {
            CHECK(busy_rx == NULL, "BUSY response decoded more than once");
            busy_rx = candidate;
        }
    }
    CHECK(busy_rx != NULL, "previous relay did not decode the BUSY response");
    CHECK(busy_rx->packet.dst_id == ORIGIN_ID,
          "decoded BUSY lost its end-to-end destination");

    mesh_relay_cancel_tx(
        &mesh_sim_role(&fixture.world, fixture.parent)->relay);
    ret = mesh_sim_run_until(
        &fixture.world,
        fixture.world.now_us + 2u * MESH_SIM_C5_RESPONSE_RX_WINDOW_US);
    CHECK(ret == MESH_SIM_OK, "bounded cleanup run failed: %d", ret);
    CHECK(fixture.world.transmission_count == 2u,
          "transit BUSY exchange emitted %zu transmissions instead of two",
          fixture.world.transmission_count);
    CHECK(mesh_sim_check_settled(&fixture.world, &report) == MESH_SIM_OK,
          "transit BUSY exchange did not settle: %s node=%zu detail=%llu",
          report.description != NULL ? report.description : "unknown",
          report.node_index,
          (unsigned long long)report.detail);
}

static void run_busy_response_reserves_future_connection_case(void)
{
    static struct fixture fixture;
    const struct mesh_event_params params = {
        .first_event_time_ms = 15u,
        .event_interval_ms = 250u,
        .event_window_ms = 80u,
        .guard_ms = 20u,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 1500u,
    };
    struct mesh_sim_connection_action action;
    struct mesh_sim_role_instance *child;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = CHILD_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0xe1010003),
        .seq = 4u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    struct mesh_outbound request;
    uint16_t connection;
    uint16_t response_index;
    int ret;

    phase = "busy-response-reserves-future-connection";
    CHECK(setup_fixture(&fixture) == MESH_SIM_OK, "fixture setup failed");
    CHECK(mesh_sim_add_connection(&fixture.world,
                                  fixture.parent,
                                  fixture.gateway,
                                  &params,
                                  true,
                                  &connection) == MESH_SIM_OK,
          "future connection setup failed");
    CHECK(make_parent_busy(&fixture) == PROTO_OK, "parent busy setup failed");
    child = mesh_sim_role(&fixture.world, fixture.child);
    CHECK(mesh_relay_start_tx(&child->relay, &packet, NULL, 0u, 9u,
                              &request) == PROTO_OK,
          "child request setup failed");
    ret = transmit_request_until_response(&fixture, &request, &response_index);
    CHECK(ret == MESH_SIM_OK, "request exchange failed: %d", ret);
    CHECK(mesh_sim_connection_next_action(&fixture.world, connection,
                                          &action) == MESH_SIM_OK,
          "future connection action unavailable");
    CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT,
          "future connection unexpectedly requires repair");
    CHECK(fixture.world.transmissions[response_index].start_us >= action.end_us,
          "BUSY response at %llu us overlapped planned connection ending %llu us",
          (unsigned long long)fixture.world.transmissions[response_index].start_us,
          (unsigned long long)action.end_us);
    CHECK(mesh_sim_schedule_next_connection_event(&fixture.world,
                                                  connection,
                                                  false) == MESH_SIM_OK,
          "future connection scheduling failed");
    CHECK(mesh_sim_run_until(&fixture.world, action.end_us) == MESH_SIM_OK,
          "future connection conflicted with paired BUSY response");
    CHECK(receive_response(&fixture, response_index) == MESH_SIM_OK,
          "deferred BUSY response exchange failed");
    CHECK(response_reception_count(&fixture, MSG_RELAY_BUSY,
                                   MESH_SIM_RX_DECODED) == 1u,
          "deferred BUSY response was not decoded exactly once");
}

static void run_result_offer_case(bool parent_busy,
                                  enum response_window_case window_case)
{
    static struct fixture fixture;
    struct mesh_sim_role_instance *child;
    struct proto_packet result_packet;
    struct mesh_outbound offer;
    struct mesh_pending_tx before;
    uint8_t payload[96];
    size_t payload_len;
    uint16_t response_index;
    uint8_t expected_response = parent_busy ? MSG_RESULT_BUSY : MSG_RESULT_GRANT;
    int ret;

    phase = parent_busy ? "result-busy-paired" :
            window_case == RESPONSE_WINDOW_VALID ? "result-grant-valid" :
                                                    "result-grant-short";
    CHECK(setup_fixture(&fixture) == MESH_SIM_OK, "fixture setup failed");
    if (parent_busy) {
        CHECK(make_parent_busy(&fixture) == PROTO_OK,
              "parent busy setup failed");
    }
    CHECK(build_result_payload(&result_packet, payload, sizeof(payload),
                               &payload_len) == PROTO_OK,
          "result payload setup failed");
    child = mesh_sim_role(&fixture.world, fixture.child);
    CHECK(mesh_relay_start_result_offer(&child->relay,
                                        &result_packet,
                                        payload,
                                        payload_len,
                                        9u,
                                        &offer) == PROTO_OK,
          "result offer setup failed");
    CHECK(apply_window_case(&fixture, window_case) == MESH_SIM_OK,
          "response timing setup failed");
    ret = transmit_request_until_response(&fixture, &offer, &response_index);
    CHECK(ret == MESH_SIM_OK, "request exchange failed: %d", ret);
    CHECK(response_transmission_type(&fixture) == expected_response,
          "wrong result-offer response type");
    before = child->relay.pending;
    ret = receive_response(&fixture, response_index);
    CHECK(ret == MESH_SIM_OK, "response exchange failed: %d", ret);

    if (window_case == RESPONSE_WINDOW_VALID) {
        CHECK(response_reception_count(&fixture, expected_response,
                                       MESH_SIM_RX_DECODED) == 1u,
              "valid result-offer response was not decoded");
        CHECK(parent_busy ?
                  child->relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF :
                  (child->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
                   !child->relay.pending.result_offer_active),
              "decoded result-offer response did not advance production state");
    } else {
        CHECK(response_reception_count(&fixture, expected_response,
                                       MESH_SIM_RX_DECODED) == 0u,
              "short result-offer response was decoded");
        CHECK(memcmp(&child->relay.pending, &before, sizeof(before)) == 0,
              "failed result-offer response mutated child state");
        CHECK(fixture.world.roles[fixture.child].partial_frames > 0u,
              "short result-offer response was not an explicit partial");
    }
}

static void run_route_reply_ack_case(enum response_window_case window_case)
{
    static struct fixture fixture;
    static struct mesh_relay parent_before_ack;
    struct mesh_sim_role_instance *child;
    struct mesh_sim_role_instance *parent;
    struct mesh_outbound request;
    struct mesh_relay_result request_result;
    uint16_t ack_index;
    int ret;

    phase = window_case == RESPONSE_WINDOW_VALID ? "route-ack-valid" :
            window_case == RESPONSE_WINDOW_SHIFTED ? "route-ack-shifted" :
                                                    "route-ack-short";
    CHECK(setup_fixture(&fixture) == MESH_SIM_OK, "fixture setup failed");
    child = mesh_sim_role(&fixture.world, fixture.child);
    parent = mesh_sim_role(&fixture.world, fixture.parent);
    CHECK(mesh_relay_prepare_route_request(&child->relay, GATEWAY_ID,
                                           9u, 0u, &request) == PROTO_OK,
          "route request setup failed");
    memset(&request_result, 0, sizeof(request_result));
    CHECK(mesh_relay_handle_rx_with_random(&parent->relay,
                                           &request.packet,
                                           request.payload,
                                           request.payload_len,
                                           CHILD_ID,
                                           96u,
                                           9u,
                                           0u,
                                           &request_result) == PROTO_OK,
          "route reply setup failed");
    CHECK((request_result.actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u,
          "parent did not build ROUTE_REPLY");
    CHECK(apply_window_case(&fixture, window_case) == MESH_SIM_OK,
          "response timing setup failed");
    ret = transmit_route_reply_until_ack(&fixture,
                                         &request_result.route_reply,
                                         &ack_index);
    CHECK(ret == MESH_SIM_OK, "route reply exchange failed: %d", ret);
    CHECK(fixture.world.transmissions[ack_index].outbound.packet.msg_type ==
              MSG_ROUTE_REPLY_ACK,
          "child did not transmit ROUTE_REPLY_ACK");
    parent_before_ack = parent->relay;
    ret = receive_response(&fixture, ack_index);
    CHECK(ret == MESH_SIM_OK, "route ACK exchange failed: %d", ret);

    if (window_case == RESPONSE_WINDOW_VALID) {
        CHECK(route_ack_reception_count(&fixture,
                                        MESH_SIM_RX_DECODED) == 1u,
              "valid ROUTE_REPLY_ACK was not decoded exactly once");
    } else {
        CHECK(route_ack_reception_count(&fixture,
                                        MESH_SIM_RX_DECODED) == 0u,
              "failed ROUTE_REPLY_ACK unexpectedly decoded");
        CHECK(memcmp(&parent->relay, &parent_before_ack,
                     sizeof(parent_before_ack)) == 0,
              "failed ROUTE_REPLY_ACK mutated parent relay state");
        if (window_case == RESPONSE_WINDOW_SHORT) {
            CHECK(parent->partial_frames > 0u,
                  "short ROUTE_REPLY_ACK was not an explicit partial");
        }
    }
}

int main(void)
{
    run_relay_busy_case(RESPONSE_WINDOW_VALID);
    run_relay_busy_case(RESPONSE_WINDOW_SHIFTED);
    run_relay_busy_case(RESPONSE_WINDOW_SHORT);
    run_multihop_transit_busy_addressing_case();
    run_busy_response_reserves_future_connection_case();
    run_result_offer_case(true, RESPONSE_WINDOW_VALID);
    run_result_offer_case(false, RESPONSE_WINDOW_VALID);
    run_result_offer_case(false, RESPONSE_WINDOW_SHORT);
    run_route_reply_ack_case(RESPONSE_WINDOW_VALID);
    run_route_reply_ack_case(RESPONSE_WINDOW_SHIFTED);
    run_route_reply_ack_case(RESPONSE_WINDOW_SHORT);

    if (failures != 0) {
        fprintf(stderr, "%d mesh contact response scenario(s) failed\n",
                failures);
        return 1;
    }
    puts("mesh contact response scenarios passed");
    return 0;
}
