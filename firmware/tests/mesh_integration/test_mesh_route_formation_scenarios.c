#include "mesh_sim.h"
#include "mesh_sim_internal.h"

#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORIGIN_ID UINT64_C(0xA100)
#define RELAY_1_ID UINT64_C(0xA101)
#define RELAY_2_ID UINT64_C(0xA102)
#define GATEWAY_ID UINT64_C(0x9000)
#define ROUTE_EPOCH UINT32_C(23)
#define SCENARIO_SEED UINT32_C(0x7a11c0de)
#define RX_GUARD_US UINT64_C(100)
#define BETWEEN_FRAMES_US UINT64_C(250)
#define DISCOVERY_START_MS UINT32_C(10)
#define RESPONDER_REPLY_DELAY_MS UINT16_C(20)
/* Production keeps at least this TTL-1 origin reply window open. */
#define ORIGIN_REPLY_BUDGET_MS UINT32_C(1000)
#define RESPONDER_GROUP_MAX 8u
#define TTL_LADDER_ATTEMPT_COUNT 4u
#define TTL_LADDER_RELAY_COUNT 6u
#define TTL_LADDER_HOP_COUNT (TTL_LADDER_RELAY_COUNT + 1u)
#define TTL_LADDER_DATA_PHASE_STEP_MS 100u
#define TTL_LADDER_ACK_BASE_DELAY_MS 2000u
#define DISCOVERY_BUDGET_MS                                                \
    (2u * MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS + FLOOD_WAVE_MS +    \
     RREP_RESPONDER_JITTER_MAX_MS + 250u)

struct route_reception {
    struct mesh_sim_reception radio;
    uint16_t window_index;
};

struct discovery_identity {
    uint64_t origin_id;
    uint64_t target_id;
    uint32_t session_id;
    uint32_t flood_epoch_id;
    uint16_t reply_nonce;
};

static const char *test_phase = "setup";

#define CHECK(expression) do {                                             \
    if (!(expression)) {                                                   \
        fprintf(stderr,                                                    \
                "scenario=two_relay_route_formation phase=%s line=%d "    \
                "assertion=%s\n",                                         \
                test_phase, __LINE__, #expression);                        \
        return 1;                                                          \
    }                                                                      \
} while (0)

static uint64_t max_u64(uint64_t first, uint64_t second)
{
    return first > second ? first : second;
}

static uint64_t transmission_evaluation_us(const struct mesh_sim_world *world,
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

static int find_tlv_u16(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint16_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &encoded, &encoded_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (encoded_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(encoded);
    return PROTO_OK;
}

static int find_tlv_u8(const uint8_t *payload,
                       size_t payload_len,
                       uint8_t type,
                       uint8_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &encoded, &encoded_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (encoded_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = encoded[0];
    return PROTO_OK;
}

static int find_tlv_u32(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint32_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &encoded, &encoded_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (encoded_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(encoded);
    return PROTO_OK;
}

static int find_tlv_u64(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint64_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &encoded, &encoded_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (encoded_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u64_le(encoded);
    return PROTO_OK;
}

static int capture_discovery_identity(const struct mesh_sim_reception *reception,
                                      bool require_reply_nonce,
                                      struct discovery_identity *identity)
{
    int ret;

    if (reception == NULL || identity == NULL ||
        reception->outcome != MESH_SIM_RX_DECODED ||
        reception->protocol_status != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    memset(identity, 0, sizeof(*identity));
    identity->session_id = reception->packet.session_id;
    ret = find_tlv_u64(reception->payload,
                       reception->payload_len,
                       TLV_INITIATOR_ID,
                       &identity->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_tlv_u64(reception->payload,
                       reception->payload_len,
                       TLV_RESPONDER_ID,
                       &identity->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_tlv_u32(reception->payload,
                       reception->payload_len,
                       TLV_FLOOD_EPOCH_ID,
                       &identity->flood_epoch_id);
    if (ret != PROTO_OK || !require_reply_nonce) {
        return ret;
    }
    return find_tlv_u16(reception->payload,
                        reception->payload_len,
                        TLV_REPLY_NONCE,
                        &identity->reply_nonce);
}

static int schedule_outbound_rx(struct mesh_sim_world *world,
                                uint16_t transmission_index,
                                uint8_t receiver_index,
                                bool complete_window,
                                uint16_t *window_index)
{
    const struct mesh_sim_transmission *tx;
    enum mesh_sim_phy phy;
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    uint64_t window_start_us;
    uint64_t window_end_us;
    uint8_t channel;

    if (world == NULL ||
        transmission_index >= world->transmission_count ||
        receiver_index >= world->role_count) {
        return MESH_SIM_ERR_ARG;
    }
    tx = &world->transmissions[transmission_index];
    if (!tx->has_outbound ||
        !world->reachable[tx->node_index][receiver_index] ||
        mesh_sim_outbound_radio(&tx->outbound, &channel, &phy) != MESH_SIM_OK) {
        return MESH_SIM_ERR_ARG;
    }
    arrival_start_us = tx->start_us +
                       world->propagation_us[tx->node_index][receiver_index];
    arrival_end_us = tx->end_us +
                     world->propagation_us[tx->node_index][receiver_index];
    window_start_us = arrival_start_us - RX_GUARD_US;
    window_end_us = complete_window ? arrival_end_us + RX_GUARD_US :
                                      arrival_end_us - 1u;

    return mesh_sim_schedule_rx(world,
                                receiver_index,
                                window_start_us,
                                window_end_us,
                                channel,
                                phy,
                                window_index);
}

static int receive_scheduled_outbound(struct mesh_sim_world *world,
                                      uint16_t transmission_index,
                                      uint8_t receiver_index,
                                      bool complete_window,
                                      struct route_reception *received)
{
    size_t reception_index;
    int ret;

    if (world == NULL || received == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    memset(received, 0, sizeof(*received));
    reception_index = world->reception_count;
    ret = schedule_outbound_rx(world,
                               transmission_index,
                               receiver_index,
                               complete_window,
                               &received->window_index);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(
            world, transmission_evaluation_us(world, transmission_index));
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->reception_count != reception_index + 1u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }

    received->radio = world->receptions[reception_index];
    return MESH_SIM_OK;
}

static int receive_prearmed_outbound(struct mesh_sim_world *world,
                                     uint16_t transmission_index,
                                     uint8_t receiver_index,
                                     struct route_reception *received)
{
    const struct mesh_sim_transmission *tx;
    size_t reception_index;
    int ret;

    if (world == NULL || received == NULL ||
        transmission_index >= world->transmission_count ||
        receiver_index >= world->role_count) {
        return MESH_SIM_ERR_ARG;
    }
    tx = &world->transmissions[transmission_index];
    reception_index = world->reception_count;
    ret = mesh_sim_run_until(
        world, transmission_evaluation_us(world, transmission_index));
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->reception_count != reception_index + 1u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    memset(received, 0, sizeof(*received));
    received->radio = world->receptions[reception_index];
    if (received->radio.source_id != world->roles[tx->node_index].id ||
        received->radio.receiver_id != world->roles[receiver_index].id) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    return MESH_SIM_OK;
}

static int transmit_route_outbound(struct mesh_sim_world *world,
                                   uint8_t sender_index,
                                   uint8_t receiver_index,
                                   const struct mesh_outbound *outbound,
                                   uint64_t requested_start_us,
                                   bool complete_window,
                                   struct route_reception *received,
                                   uint16_t *transmission_index)
{
    uint64_t start_us;
    uint16_t tx_index;
    int ret;

    if (world == NULL || outbound == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    start_us = max_u64(requested_start_us,
                       world->now_us + RX_GUARD_US + 1u);
    start_us = max_u64(start_us,
                       (uint64_t)outbound->earliest_tx_ms * 1000u);
    ret = mesh_sim_schedule_outbound_tx(world,
                                        sender_index,
                                        start_us,
                                        outbound,
                                        &tx_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = receive_scheduled_outbound(world,
                                     tx_index,
                                     receiver_index,
                                     complete_window,
                                     received);
    if (ret == MESH_SIM_OK && transmission_index != NULL) {
        *transmission_index = tx_index;
    }
    return ret;
}

static bool same_discovery(const struct discovery_identity *first,
                           const struct discovery_identity *second)
{
    return first->origin_id == second->origin_id &&
           first->target_id == second->target_id &&
           first->session_id == second->session_id &&
           first->flood_epoch_id == second->flood_epoch_id &&
           first->reply_nonce == second->reply_nonce;
}

static int assert_selected_hop(const struct mesh_relay *relay,
                               uint64_t target_id,
                               uint64_t expected_next_hop)
{
    uint64_t next_hop = 0u;

    if (mesh_relay_select_next_hop(relay, target_id, &next_hop) != PROTO_OK) {
        return 1;
    }
    return next_hop == expected_next_hop ? 0 : 1;
}

static struct mesh_event_params route_connection_params(uint32_t first_event_ms,
                                                        uint32_t interval_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = interval_ms,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static int run_connection_event(struct mesh_sim_world *world,
                                uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world,
                                              connection_index,
                                              &action);

    if (ret != MESH_SIM_OK ||
        action.kind != MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(world,
                                                      connection_index,
                                                      false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int build_ttl_ladder_data(uint64_t source_id,
                                 struct proto_packet *packet,
                                 uint8_t *payload,
                                 size_t payload_capacity,
                                 size_t *payload_len)
{
    size_t length = 0u;
    int ret = tlv_append_u32(payload,
                             payload_capacity,
                             &length,
                             TLV_MESH_TEST_PACKET_ID,
                             UINT32_C(0x640001));

    if (ret != PROTO_OK || length > UINT16_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    *packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = source_id,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x64000001),
        .seq = UINT16_C(0x6401),
        .ttl = TTL_LADDER_HOP_COUNT,
        .payload_len = (uint16_t)length,
    };
    *payload_len = length;
    return PROTO_OK;
}

static bool route_ack_matches(const struct mesh_outbound *outbound,
                              uint64_t next_hop_id,
                              const struct proto_packet *data_packet)
{
    uint16_t requested_seq = 0u;

    return outbound != NULL && data_packet != NULL &&
           outbound->packet.msg_type == MSG_GATEWAY_ACK &&
           outbound->packet.src_id == GATEWAY_ID &&
           outbound->packet.dst_id == data_packet->src_id &&
           outbound->packet.session_id == data_packet->session_id &&
           outbound->next_hop_id == next_hop_id &&
           find_tlv_u16(outbound->payload,
                        outbound->payload_len,
                        TLV_REQUESTED_MSG_SEQ,
                        &requested_seq) == PROTO_OK &&
           requested_seq == data_packet->seq;
}

static int run_responder_slot_case(uint8_t responder_count)
{
    static struct mesh_sim_world world;
    struct mesh_outbound request;
    struct route_reception reply_reception;
    struct route_reception ack_reception;
    uint8_t responders[RESPONDER_GROUP_MAX];
    uint16_t reply_transmissions[RESPONDER_GROUP_MAX];
    uint16_t request_transmission;
    uint16_t advertised_reply_delay_ms;
    uint8_t origin;
    uint8_t gateway;
    uint64_t request_evaluation_us;
    uint64_t reply_slot_base_us;
    uint64_t budget_deadline_us;
    size_t request_reception_base;
    size_t reply_transmission_base;
    char phase[64];

    if (responder_count == 0u || responder_count > RESPONDER_GROUP_MAX) {
        return 1;
    }

    mesh_sim_init(&world, SCENARIO_SEED + responder_count);
    snprintf(phase, sizeof(phase), "responder_slots_%u_setup", responder_count);
    test_phase = phase;
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID + UINT64_C(0x1000),
                            GATEWAY_ID, ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    for (uint8_t i = 0u; i < responder_count; i++) {
        uint64_t responder_id = RELAY_1_ID + UINT64_C(0x1000) + i;

        CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                responder_id, GATEWAY_ID, ROUTE_EPOCH,
                                &responders[i]) == MESH_SIM_OK);
        CHECK(mesh_sim_set_link(&world, origin, responders[i],
                                (uint8_t)(96u - i),
                                (uint16_t)(5u + i)) == MESH_SIM_OK);
        CHECK(mesh_sim_set_link(&world, responders[i], gateway,
                                98u, (uint16_t)(4u + i)) == MESH_SIM_OK);
        CHECK(mesh_sim_install_route(&world,
                                     responders[i],
                                     gateway,
                                     0u,
                                     ROUTE_EPOCH) == PROTO_OK);
    }

    CHECK(mesh_relay_prepare_route_request_with_timing_flags(
              &world.roles[origin].relay,
              GATEWAY_ID,
              NULL,
              0u,
              0u,
              RESPONDER_REPLY_DELAY_MS,
              DISCOVERY_START_MS,
              0u,
              &request) == PROTO_OK);
    CHECK(request.packet.ttl == 1u);
    CHECK(mesh_route_request_reply_rx_delay_ms(
              &request, &advertised_reply_delay_ms));
    CHECK(advertised_reply_delay_ms == RESPONDER_REPLY_DELAY_MS);
    CHECK(mesh_sim_schedule_outbound_tx(
              &world,
              origin,
              (uint64_t)DISCOVERY_START_MS * 1000u,
              &request,
              &request_transmission) == MESH_SIM_OK);
    CHECK(request_transmission == 0u);

    request_reception_base = world.reception_count;
    reply_transmission_base = world.transmission_count;
    for (uint8_t i = 0u; i < responder_count; i++) {
        CHECK(mesh_sim_override_next_relay_random(&world,
                                                  responders[i],
                                                  i) == MESH_SIM_OK);
        CHECK(schedule_outbound_rx(&world,
                                   request_transmission,
                                   responders[i],
                                   true,
                                   NULL) == MESH_SIM_OK);
    }
    request_evaluation_us =
        transmission_evaluation_us(&world, request_transmission);
    CHECK(mesh_sim_run_until(&world, request_evaluation_us) == MESH_SIM_OK);
    CHECK(world.reception_count == request_reception_base + responder_count);
    CHECK(world.transmission_count ==
          reply_transmission_base + responder_count);

    reply_slot_base_us =
        ((request_evaluation_us / 1000u) + RESPONDER_REPLY_DELAY_MS) * 1000u;
    budget_deadline_us =
        world.transmissions[request_transmission].start_us +
        (uint64_t)ORIGIN_REPLY_BUDGET_MS * 1000u;
    for (uint8_t i = 0u; i < responder_count; i++) {
        const struct mesh_sim_reception *request_rx =
            &world.receptions[request_reception_base + i];
        const struct mesh_sim_transmission *reply_tx;
        uint64_t expected_start_us =
            reply_slot_base_us +
            (uint64_t)i * RREP_RESPONDER_SLOT_MS * 1000u;

        reply_transmissions[i] =
            (uint16_t)(reply_transmission_base + i);
        reply_tx = &world.transmissions[reply_transmissions[i]];
        CHECK(request_rx->receiver_id == world.roles[responders[i]].id);
        CHECK(request_rx->outcome == MESH_SIM_RX_DECODED);
        CHECK(request_rx->packet.msg_type == MSG_ROUTE_REQ);
        CHECK(reply_tx->node_index == responders[i]);
        CHECK(reply_tx->has_outbound);
        CHECK(reply_tx->outbound.packet.msg_type == MSG_ROUTE_REPLY);
        CHECK(reply_tx->outbound.next_hop_id == world.roles[origin].id);
        CHECK(reply_tx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL);
        CHECK(reply_tx->start_us == expected_start_us);
        CHECK(reply_tx->start_us >= reply_slot_base_us);
        CHECK(reply_tx->start_us <=
              reply_slot_base_us +
                  (uint64_t)RREP_RESPONDER_JITTER_MAX_MS * 1000u);
        if (i > 0u) {
            const struct mesh_sim_transmission *previous =
                &world.transmissions[reply_transmissions[i - 1u]];

            CHECK(previous->end_us + 2u * RX_GUARD_US < reply_tx->start_us);
        }
        CHECK(schedule_outbound_rx(&world,
                                   reply_transmissions[i],
                                   origin,
                                   true,
                                   NULL) == MESH_SIM_OK);
    }

    for (uint8_t i = 0u; i < responder_count; i++) {
        size_t transmission_count_before = world.transmission_count;
        uint16_t ack_transmission;

        snprintf(phase, sizeof(phase), "responder_slots_%u_exchange_%u",
                 responder_count, i);
        test_phase = phase;
        CHECK(receive_prearmed_outbound(&world,
                                        reply_transmissions[i],
                                        origin,
                                        &reply_reception) == MESH_SIM_OK);
        CHECK(reply_reception.radio.outcome == MESH_SIM_RX_DECODED);
        CHECK(reply_reception.radio.packet.msg_type == MSG_ROUTE_REPLY);
        CHECK(world.transmission_count == transmission_count_before + 1u);
        ack_transmission = (uint16_t)transmission_count_before;
        CHECK(world.transmissions[ack_transmission].has_outbound);
        CHECK(world.transmissions[ack_transmission].outbound.packet.msg_type ==
              MSG_ROUTE_REPLY_ACK);
        CHECK(world.transmissions[ack_transmission].outbound.next_hop_id ==
              world.roles[responders[i]].id);
        CHECK(receive_scheduled_outbound(&world,
                                         ack_transmission,
                                         responders[i],
                                         true,
                                         &ack_reception) == MESH_SIM_OK);
        CHECK(ack_reception.radio.outcome == MESH_SIM_RX_DECODED);
        CHECK(ack_reception.radio.packet.msg_type == MSG_ROUTE_REPLY_ACK);
        CHECK(ack_reception.radio.receiver_id == world.roles[responders[i]].id);
        CHECK(ack_reception.radio.end_us <= budget_deadline_us);
        if (i + 1u < responder_count) {
            CHECK(ack_reception.radio.end_us <
                  world.transmissions[reply_transmissions[i + 1u]].start_us);
        }
    }

    snprintf(phase, sizeof(phase), "responder_slots_%u_complete",
             responder_count);
    test_phase = phase;
    CHECK(world.roles[origin].collision_frames == 0u);
    for (uint8_t i = 0u; i < responder_count; i++) {
        CHECK(world.roles[responders[i]].collision_frames == 0u);
        CHECK(!world.roles[responders[i]].next_relay_random_valid);
    }
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_COLLISION,
                                     0u) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_DECODED,
                                     0u) == (size_t)responder_count * 3u);
    CHECK(world.now_us <= budget_deadline_us);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int run_ttl_ladder_data_case(void)
{
    static const uint8_t expected_attempt_ttls[TTL_LADDER_ATTEMPT_COUNT] = {
        1u, 2u, 4u, 6u,
    };
    static struct mesh_sim_world world;
    struct discovery_identity attempt_identity;
    struct discovery_identity route_identity;
    struct discovery_identity reply_identity;
    struct proto_packet data_packet;
    uint8_t data_payload[32];
    size_t data_payload_len = 0u;
    uint8_t relays[TTL_LADDER_RELAY_COUNT];
    uint8_t path[TTL_LADDER_HOP_COUNT + 1u];
    uint16_t connections[TTL_LADDER_HOP_COUNT];
    uint32_t attempt_sessions[TTL_LADDER_ATTEMPT_COUNT] = {0};
    uint32_t request_start_ms = DISCOVERY_START_MS;
    uint16_t route_reply_tx = UINT16_MAX;
    uint16_t reply_nonce = 0u;
    uint8_t origin;
    uint8_t gateway;
    uint8_t first_gateway_ack_ttl = 0u;
    uint16_t gateway_ack_seq = 0u;
    char phase[96];

    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0x64006400));
    test_phase = "ttl_ladder_setup";
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID + UINT64_C(0x2000),
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    path[0] = origin;
    for (size_t i = 0u; i < TTL_LADDER_RELAY_COUNT; i++) {
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_1_ID + UINT64_C(0x2000) + i,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &relays[i]) == MESH_SIM_OK);
        path[i + 1u] = relays[i];
    }
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    path[TTL_LADDER_HOP_COUNT] = gateway;
    for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
        CHECK(mesh_sim_set_link(&world,
                                path[i],
                                path[i + 1u],
                                (uint8_t)(98u - i),
                                (uint16_t)(4u + i)) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_install_route(
              &world,
              relays[TTL_LADDER_RELAY_COUNT - 1u],
              gateway,
              0u,
              ROUTE_EPOCH) == PROTO_OK);
    CHECK(assert_selected_hop(&world.roles[origin].relay,
                              GATEWAY_ID,
                              world.roles[relays[0]].id) != 0);

    for (size_t attempt = 0u;
         attempt < TTL_LADDER_ATTEMPT_COUNT;
         attempt++) {
        struct mesh_outbound request;
        uint16_t current_tx;
        uint8_t attempt_ttl = expected_attempt_ttls[attempt];
        uint8_t request_hops =
            attempt == TTL_LADDER_ATTEMPT_COUNT - 1u ?
                TTL_LADDER_RELAY_COUNT : attempt_ttl;
        uint64_t requested_start_us;

        if (attempt > 0u) {
            request_start_ms =
                world.roles[origin].relay.route_discovery.next_request_ms;
        }
        snprintf(phase, sizeof(phase), "ttl_ladder_attempt_%zu_prepare",
                 attempt + 1u);
        test_phase = phase;
        CHECK(mesh_relay_prepare_route_request(
                  &world.roles[origin].relay,
                  GATEWAY_ID,
                  request_start_ms,
                  0u,
                  &request) == PROTO_OK);
        CHECK(request.packet.msg_type == MSG_ROUTE_REQ);
        CHECK(request.packet.ttl == attempt_ttl);
        CHECK(world.roles[origin].relay.route_discovery.attempts ==
              attempt + 1u);
        attempt_sessions[attempt] = request.packet.session_id;
        for (size_t previous = 0u; previous < attempt; previous++) {
            CHECK(attempt_sessions[previous] != attempt_sessions[attempt]);
        }

        requested_start_us = (uint64_t)request_start_ms * 1000u;
        if (requested_start_us <= world.now_us + RX_GUARD_US) {
            requested_start_us = world.now_us + RX_GUARD_US + 1u;
        }
        CHECK(mesh_sim_schedule_outbound_tx(&world,
                                            origin,
                                            requested_start_us,
                                            &request,
                                            &current_tx) == MESH_SIM_OK);

        for (uint8_t hop = 0u; hop < request_hops; hop++) {
            struct route_reception reception;
            const struct mesh_sim_reception *radio;
            uint8_t hop_count = UINT8_MAX;
            uint8_t receiver = path[hop + 1u];
            size_t transmission_count_before = world.transmission_count;

            snprintf(phase,
                     sizeof(phase),
                     "ttl_ladder_attempt_%zu_hop_%u",
                     attempt + 1u,
                     hop + 1u);
            test_phase = phase;
            CHECK(mesh_sim_override_next_relay_random(&world,
                                                      receiver,
                                                      0u) == MESH_SIM_OK);
            CHECK(receive_scheduled_outbound(&world,
                                             current_tx,
                                             receiver,
                                             true,
                                             &reception) == MESH_SIM_OK);
            radio = &reception.radio;
            CHECK(radio->outcome == MESH_SIM_RX_DECODED);
            CHECK(radio->receiver_id == world.roles[receiver].id);
            CHECK(radio->source_id == world.roles[path[hop]].id);
            CHECK(radio->packet.msg_type == MSG_ROUTE_REQ);
            CHECK(radio->packet.ttl == (uint8_t)(attempt_ttl - hop));
            CHECK(find_tlv_u8(radio->payload,
                              radio->payload_len,
                              TLV_HOP_COUNT,
                              &hop_count) == PROTO_OK);
            CHECK(hop_count == hop);
            CHECK(capture_discovery_identity(radio,
                                             false,
                                             &attempt_identity) == PROTO_OK);
            CHECK(attempt_identity.origin_id == world.roles[origin].id);
            CHECK(attempt_identity.target_id == GATEWAY_ID);
            CHECK(attempt_identity.session_id == request.packet.session_id);
            CHECK(attempt_identity.flood_epoch_id == request.packet.session_id);

            if (attempt == TTL_LADDER_ATTEMPT_COUNT - 1u &&
                hop + 1u == TTL_LADDER_RELAY_COUNT) {
                CHECK(receiver == relays[TTL_LADDER_RELAY_COUNT - 1u]);
                CHECK(world.transmission_count ==
                      transmission_count_before + 1u);
                route_reply_tx = (uint16_t)transmission_count_before;
                CHECK(world.transmissions[route_reply_tx].has_outbound);
                CHECK(world.transmissions[route_reply_tx].outbound.packet
                          .msg_type == MSG_ROUTE_REPLY);
                CHECK(world.transmissions[route_reply_tx].outbound.packet.ttl ==
                      MESH_NETWORK_MAX_HOPS);
                CHECK(world.transmissions[route_reply_tx].outbound.next_hop_id ==
                      world.roles[relays[TTL_LADDER_RELAY_COUNT - 2u]].id);
                route_identity = attempt_identity;
            } else if (hop + 1u < request_hops) {
                CHECK(world.transmission_count ==
                      transmission_count_before + 1u);
                current_tx = (uint16_t)transmission_count_before;
                CHECK(world.transmissions[current_tx].has_outbound);
                CHECK(world.transmissions[current_tx].outbound.packet.msg_type ==
                      MSG_ROUTE_REQ);
                CHECK(world.transmissions[current_tx].outbound.packet.ttl ==
                      (uint8_t)(attempt_ttl - hop - 1u));
            } else {
                CHECK(world.transmission_count == transmission_count_before);
            }
        }

        if (attempt + 1u < TTL_LADDER_ATTEMPT_COUNT) {
            uint64_t unused_next_hop = 0u;

            CHECK(world.roles[origin].relay.route_discovery.active);
            CHECK(mesh_relay_select_next_hop(&world.roles[origin].relay,
                                             GATEWAY_ID,
                                             &unused_next_hop) ==
                  PROTO_ERR_NOT_FOUND);
        }
    }
    CHECK(route_reply_tx != UINT16_MAX);
    CHECK(world.roles[origin].relay.route_discovery.attempts ==
          TTL_LADDER_ATTEMPT_COUNT);

    {
        uint16_t current_reply_tx = route_reply_tx;

        for (size_t reverse_step = 0u;
             reverse_step < TTL_LADDER_RELAY_COUNT;
             reverse_step++) {
            struct route_reception reception;
            size_t receiver_path_index =
                TTL_LADDER_RELAY_COUNT - reverse_step - 1u;
            uint8_t receiver = path[receiver_path_index];
            uint8_t sender = path[receiver_path_index + 1u];
            size_t transmission_count_before = world.transmission_count;

            snprintf(phase,
                     sizeof(phase),
                     "ttl_ladder_reply_hop_%zu",
                     reverse_step + 1u);
            test_phase = phase;
            CHECK(receive_scheduled_outbound(&world,
                                             current_reply_tx,
                                             receiver,
                                             true,
                                             &reception) == MESH_SIM_OK);
            CHECK(reception.radio.outcome == MESH_SIM_RX_DECODED);
            CHECK(reception.radio.source_id == world.roles[sender].id);
            CHECK(reception.radio.receiver_id == world.roles[receiver].id);
            CHECK(reception.radio.packet.msg_type == MSG_ROUTE_REPLY);
            CHECK(reception.radio.packet.ttl ==
                  (uint8_t)(MESH_NETWORK_MAX_HOPS - reverse_step));
            CHECK(capture_discovery_identity(&reception.radio,
                                             true,
                                             &reply_identity) == PROTO_OK);
            reply_nonce = mesh_route_reply_nonce(route_identity.origin_id,
                                                 route_identity.target_id,
                                                 route_identity.session_id,
                                                 route_identity.flood_epoch_id);
            CHECK(reply_identity.origin_id == route_identity.origin_id);
            CHECK(reply_identity.target_id == route_identity.target_id);
            CHECK(reply_identity.session_id == route_identity.session_id);
            CHECK(reply_identity.flood_epoch_id ==
                  route_identity.flood_epoch_id);
            CHECK(reply_identity.reply_nonce == reply_nonce);

            if (receiver != origin) {
                uint16_t ack_tx = (uint16_t)transmission_count_before;
                uint16_t forwarded_tx =
                    (uint16_t)(transmission_count_before + 1u);

                CHECK(world.transmission_count ==
                      transmission_count_before + 2u);
                CHECK(world.transmissions[ack_tx].outbound.packet.msg_type ==
                      MSG_ROUTE_REPLY_ACK);
                CHECK(world.transmissions[ack_tx].outbound.next_hop_id ==
                      world.roles[sender].id);
                CHECK(world.transmissions[forwarded_tx].outbound.packet
                          .msg_type == MSG_ROUTE_REPLY);
                CHECK(world.transmissions[forwarded_tx].outbound.next_hop_id ==
                      world.roles[path[receiver_path_index - 1u]].id);
                current_reply_tx = forwarded_tx;
            } else {
                CHECK(world.transmission_count ==
                      transmission_count_before + 1u);
                CHECK(world.transmissions[transmission_count_before].outbound
                          .packet.msg_type == MSG_ROUTE_REPLY_ACK);
            }
        }
    }
    CHECK(!world.roles[origin].relay.route_discovery.active);

    test_phase = "ttl_ladder_installed_route";
    for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
        const struct route_candidate *selected =
            route_selected(&world.roles[path[i]].relay.upstream);

        CHECK(selected != NULL);
        CHECK(selected->next_hop_id == world.roles[path[i + 1u]].id);
        CHECK(selected->hop_count == TTL_LADDER_HOP_COUNT - i - 1u);
        CHECK(selected->route_epoch == ROUTE_EPOCH);
    }

    {
        uint32_t base_ms =
            (uint32_t)((world.now_us + 999u) / 1000u) + 100u;

        for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
            uint32_t first_event_ms =
                base_ms + (uint32_t)i * TTL_LADDER_DATA_PHASE_STEP_MS;
            uint32_t ack_event_ms =
                base_ms + TTL_LADDER_ACK_BASE_DELAY_MS +
                (uint32_t)(TTL_LADDER_HOP_COUNT - i - 1u) *
                    TTL_LADDER_DATA_PHASE_STEP_MS;
            struct mesh_event_params params = route_connection_params(
                first_event_ms,
                ack_event_ms - first_event_ms);

            CHECK(mesh_sim_add_connection(&world,
                                          path[i],
                                          path[i + 1u],
                                          &params,
                                          true,
                                          &connections[i]) == MESH_SIM_OK);
        }
    }

    test_phase = "ttl_ladder_queue_data";
    CHECK(build_ttl_ladder_data(world.roles[origin].id,
                                &data_packet,
                                data_payload,
                                sizeof(data_payload),
                                &data_payload_len) == PROTO_OK);
    CHECK(mesh_sim_queue_originated(&world,
                                    origin,
                                    &data_packet,
                                    data_payload,
                                    data_payload_len) == MESH_SIM_OK);

    for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
        const struct mesh_sim_transmission *tx;
        const struct mesh_sim_reception *rx;
        size_t transmission_count_before = world.transmission_count;
        size_t reception_count_before = world.reception_count;

        snprintf(phase, sizeof(phase), "ttl_ladder_data_hop_%zu", i + 1u);
        test_phase = phase;
        CHECK(run_connection_event(&world, connections[i]) == MESH_SIM_OK);
        CHECK(world.transmission_count == transmission_count_before + 1u);
        CHECK(world.reception_count == reception_count_before + 1u);
        tx = &world.transmissions[transmission_count_before];
        rx = &world.receptions[reception_count_before];
        CHECK(tx->node_index == path[i]);
        CHECK(tx->has_outbound);
        CHECK(tx->outbound.packet.msg_type == MSG_MESH_DATA);
        CHECK(tx->outbound.packet.src_id == data_packet.src_id);
        CHECK(tx->outbound.packet.dst_id == data_packet.dst_id);
        CHECK(tx->outbound.packet.session_id == data_packet.session_id);
        CHECK(tx->outbound.packet.seq == data_packet.seq);
        CHECK(tx->outbound.packet.ttl == TTL_LADDER_HOP_COUNT - i);
        CHECK(tx->outbound.next_hop_id == world.roles[path[i + 1u]].id);
        CHECK(rx->outcome == MESH_SIM_RX_DECODED);
        CHECK(rx->receiver_id == world.roles[path[i + 1u]].id);
        CHECK(rx->packet.ttl == TTL_LADDER_HOP_COUNT - i);
    }
    CHECK(world.roles[gateway].delivery_count == 1u);
    CHECK(world.roles[gateway].deliveries[0].packet.session_id ==
          data_packet.session_id);
    CHECK(world.roles[gateway].deliveries[0].packet.seq == data_packet.seq);
    CHECK(world.roles[gateway].deliveries[0].packet.ttl == 1u);
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    for (size_t reverse_step = 0u;
         reverse_step < TTL_LADDER_HOP_COUNT;
         reverse_step++) {
        size_t connection = TTL_LADDER_HOP_COUNT - reverse_step - 1u;
        const struct mesh_sim_transmission *tx;
        const struct mesh_sim_reception *rx;
        size_t transmission_count_before = world.transmission_count;
        size_t reception_count_before = world.reception_count;

        snprintf(phase,
                 sizeof(phase),
                 "ttl_ladder_gateway_ack_hop_%zu",
                 reverse_step + 1u);
        test_phase = phase;
        CHECK(run_connection_event(&world, connections[connection]) ==
              MESH_SIM_OK);
        CHECK(world.transmission_count == transmission_count_before + 1u);
        CHECK(world.reception_count == reception_count_before + 1u);
        tx = &world.transmissions[transmission_count_before];
        rx = &world.receptions[reception_count_before];
        CHECK(tx->node_index == path[connection + 1u]);
        CHECK(tx->has_outbound);
        CHECK(route_ack_matches(&tx->outbound,
                                world.roles[path[connection]].id,
                                &data_packet));
        if (reverse_step == 0u) {
            first_gateway_ack_ttl = tx->outbound.packet.ttl;
            gateway_ack_seq = tx->outbound.packet.seq;
        } else {
            CHECK(tx->outbound.packet.ttl ==
                  (uint8_t)(first_gateway_ack_ttl - reverse_step));
            CHECK(tx->outbound.packet.seq == gateway_ack_seq);
        }
        CHECK(rx->outcome == MESH_SIM_RX_DECODED);
        CHECK(rx->packet.msg_type == MSG_GATEWAY_ACK);
        CHECK(rx->packet.ttl == tx->outbound.packet.ttl);
        CHECK(world.roles[gateway].delivery_count == 1u);
        if (connection > 0u) {
            CHECK(world.roles[origin].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        }
    }
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                     0u) == 0u);
    for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
        CHECK(world.connections[connections[i]].completed_events == 2u);
        CHECK(world.connections[connections[i]].completed_repairs == 0u);
        CHECK(world.connections[connections[i]].timing_a.timing_fresh);
        CHECK(world.connections[connections[i]].timing_b.timing_fresh);
        CHECK(!world.connections[connections[i]].timing_a.fallback_required);
        CHECK(!world.connections[connections[i]].timing_b.fallback_required);
    }
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int run_blank_anchor_retains_local_click_until_route_case(void)
{
    static struct mesh_sim_world world;
    struct proto_packet click = {0};
    uint8_t anchor;
    uint8_t gateway;
    uint16_t connection;
    uint16_t click_tx = UINT16_MAX;
    size_t click_tx_count = 0u;

    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0xb1a0c001));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID, GATEWAY_ID, ROUTE_EPOCH, &anchor) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH, &gateway) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, anchor, gateway, 100u, 1u) == MESH_SIM_OK);

    click.msg_type = MSG_CLICK_REPORT;
    click.src_id = ORIGIN_ID;
    click.dst_id = GATEWAY_ID;
    click.session_id = UINT32_C(0x11223344);
    click.seq = UINT16_C(0x3344);
    click.ttl = MESH_DEFAULT_TTL;
    click.payload_len = 0u;
    CHECK(mesh_sim_queue_originated(&world, anchor, &click, NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(world.roles[anchor].route_waiting_valid);

    for (uint8_t retry = 0u; retry < 2u; retry++) {
        world.now_us =
            (uint64_t)world.roles[anchor].relay.route_discovery.next_request_ms *
            1000u;
        CHECK(mesh_sim_relay_process_route_discovery_retry(&world, anchor) ==
              MESH_SIM_OK);
        CHECK(world.roles[anchor].route_waiting_valid);
    }
    CHECK(world.roles[anchor].route_discovery_requests >= 3u);
    CHECK(world.roles[anchor].relay.route_discovery.attempts >= 3u);

    CHECK(mesh_sim_install_route(&world, anchor, gateway, 0u, ROUTE_EPOCH) ==
          MESH_SIM_OK);
    world.now_us =
        (uint64_t)world.roles[anchor].relay.route_discovery.next_request_ms *
        1000u;
    {
        uint32_t first_event_ms =
            (uint32_t)((world.now_us + 999u) / 1000u) + 100u;
        struct mesh_event_params params =
            route_connection_params(first_event_ms, 1000u);

        CHECK(mesh_sim_add_connection(&world, anchor, gateway, &params,
                                      true, &connection) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_relay_process_route_discovery_retry(&world, anchor) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_schedule_next_connection_event(&world, connection, false) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK);
    CHECK(!world.roles[anchor].route_waiting_valid);

    for (size_t i = 0u; i < world.transmission_count; i++) {
        if (world.transmissions[i].has_outbound &&
            world.transmissions[i].outbound.packet.msg_type == MSG_CLICK_REPORT &&
            world.transmissions[i].outbound.packet.session_id == click.session_id &&
            world.transmissions[i].outbound.packet.seq == click.seq) {
            click_tx = (uint16_t)i;
            click_tx_count++;
        }
    }
    if (click_tx_count != 1u) {
        fprintf(stderr,
                "blank-anchor retained click transmissions=%u total=%u retries=%u\n",
                (unsigned int)click_tx_count,
                (unsigned int)world.transmission_count,
                world.roles[anchor].route_discovery_requests);
        fprintf(stderr, "queue=%u deliveries=%u now=%llu\n",
                (unsigned int)world.roles[anchor].tx_queue_count,
                (unsigned int)world.roles[gateway].delivery_count,
                (unsigned long long)world.now_us);
    }
    CHECK(click_tx_count == 1u);
    CHECK(world.roles[gateway].delivery_count == 1u);
    CHECK(world.roles[gateway].deliveries[0].packet.session_id == click.session_id);
    CHECK(world.roles[gateway].deliveries[0].packet.seq == click.seq);
    return 0;
}

static int run_route_collision_case(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound request;
    uint8_t origin;
    uint8_t interferer;
    uint8_t receiver;
    uint16_t origin_tx;
    uint16_t interferer_tx;
    uint64_t evaluation_us;

    test_phase = "route_collision_setup";
    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0x55aa55aa));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_2_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &interferer) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_1_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &receiver) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, origin, receiver, 96u, 7u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, interferer, receiver, 93u, 7u) ==
          MESH_SIM_OK);
    CHECK(mesh_relay_prepare_route_request(&world.roles[origin].relay,
                                           GATEWAY_ID,
                                           DISCOVERY_START_MS,
                                           0u,
                                           &request) == PROTO_OK);
    CHECK(mesh_sim_schedule_outbound_tx(
              &world,
              origin,
              (uint64_t)DISCOVERY_START_MS * 1000u,
              &request,
              &origin_tx) == MESH_SIM_OK);
    CHECK(mesh_sim_schedule_outbound_tx(
              &world,
              interferer,
              (uint64_t)DISCOVERY_START_MS * 1000u,
              &request,
              &interferer_tx) == MESH_SIM_OK);
    CHECK(schedule_outbound_rx(&world,
                               origin_tx,
                               receiver,
                               true,
                               NULL) == MESH_SIM_OK);
    CHECK(mesh_sim_override_next_relay_random(&world, receiver, 0u) ==
          MESH_SIM_OK);

    test_phase = "route_collision_delivery";
    evaluation_us = max_u64(transmission_evaluation_us(&world, origin_tx),
                            transmission_evaluation_us(&world, interferer_tx));
    CHECK(mesh_sim_run_until(&world, evaluation_us) == MESH_SIM_OK);
    CHECK(world.reception_count == 2u);
    CHECK(world.receptions[0].outcome == MESH_SIM_RX_COLLISION);
    CHECK(world.receptions[1].outcome == MESH_SIM_RX_COLLISION);
    CHECK(world.roles[receiver].collision_frames == 2u);
    CHECK(world.roles[receiver].decoded_frames == 0u);
    CHECK(world.roles[receiver].next_relay_random_valid);
    CHECK(mesh_relay_find_downlink(&world.roles[receiver].relay,
                                   ORIGIN_ID) == NULL);
    CHECK(world.transmission_count == 2u);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

int main(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound first_request;
    struct mesh_outbound retry_request;
    struct route_reception partial;
    struct route_reception request_at_relay_1;
    struct route_reception request_at_relay_2;
    struct route_reception reply_at_relay_1;
    struct route_reception reply_at_origin;
    struct discovery_identity request_identity;
    struct discovery_identity initial_reply_identity;
    struct discovery_identity forwarded_reply_identity;
    const struct mesh_downlink_entry *relay_1_reverse;
    const struct mesh_downlink_entry *relay_2_reverse;
    uint8_t origin;
    uint8_t relay_1;
    uint8_t relay_2;
    uint8_t gateway;
    uint32_t retry_ms;
    uint32_t completed_ms;
    uint16_t expected_nonce;
    uint16_t first_request_tx;
    uint16_t retry_request_tx;
    uint16_t forwarded_request_tx;
    uint16_t initial_reply_tx;
    uint16_t route_reply_ack_tx;
    uint16_t forwarded_reply_tx;
    uint64_t next_start_us;
    size_t transmission_count_before;

    mesh_sim_init(&world, SCENARIO_SEED);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_1_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &relay_1) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_2_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &relay_2) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, origin, relay_1, 96u, 7u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_1, relay_2, 93u, 11u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_2, gateway, 98u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world, relay_2, gateway, 1u,
                                 ROUTE_EPOCH) == MESH_SIM_ERR_ARG);
    CHECK(mesh_sim_install_route(&world, relay_1, relay_2, 0u,
                                 ROUTE_EPOCH) == MESH_SIM_ERR_ARG);
    CHECK(mesh_sim_install_route(&world, relay_2, gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              GATEWAY_ID, GATEWAY_ID) == 0);
    CHECK(route_selected(&world.roles[relay_2].relay.upstream)->hop_count == 0u);
    CHECK(mesh_relay_select_next_hop(&world.roles[origin].relay,
                                     GATEWAY_ID,
                                     &next_start_us) == PROTO_ERR_NOT_FOUND);

    test_phase = "truncated_first_attempt";
    CHECK(mesh_relay_prepare_route_request(&world.roles[origin].relay,
                                           GATEWAY_ID,
                                           DISCOVERY_START_MS,
                                           0u,
                                           &first_request) == PROTO_OK);
    CHECK(first_request.packet.msg_type == MSG_ROUTE_REQ);
    CHECK(first_request.packet.ttl == 1u);
    transmission_count_before = world.transmission_count;
    CHECK(transmit_route_outbound(&world,
                                  origin,
                                  relay_1,
                                  &first_request,
                                  (uint64_t)DISCOVERY_START_MS * 1000u,
                                  false,
                                  &partial,
                                  &first_request_tx) == MESH_SIM_OK);
    CHECK(first_request_tx == transmission_count_before);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    CHECK(partial.radio.outcome == MESH_SIM_RX_FRAME_TIMEOUT);
    CHECK(partial.radio.end_rctu >
          world.rx_windows[partial.window_index].end_rctu);
    CHECK(world.roles[relay_1].decoded_frames == 0u);
    CHECK(mesh_relay_find_downlink(&world.roles[relay_1].relay,
                                    ORIGIN_ID) == NULL);

    test_phase = "retry_request_to_first_relay";
    retry_ms = world.roles[origin].relay.route_discovery.next_request_ms;
    CHECK(retry_ms >= DISCOVERY_START_MS +
                      MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS);
    CHECK(retry_ms <= DISCOVERY_START_MS +
                      2u * MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS);
    CHECK(mesh_relay_prepare_route_request(&world.roles[origin].relay,
                                           GATEWAY_ID,
                                           retry_ms,
                                           0u,
                                           &retry_request) == PROTO_OK);
    CHECK(retry_request.packet.ttl == 2u);
    CHECK(retry_request.packet.session_id != first_request.packet.session_id);
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_override_next_relay_random(&world, relay_1, 0u) ==
          MESH_SIM_OK);
    CHECK(transmit_route_outbound(&world,
                                  origin,
                                  relay_1,
                                  &retry_request,
                                  (uint64_t)retry_ms * 1000u,
                                  true,
                                  &request_at_relay_1,
                                  &retry_request_tx) == MESH_SIM_OK);
    CHECK(retry_request_tx == transmission_count_before);
    CHECK(world.transmission_count == transmission_count_before + 2u);
    forwarded_request_tx = (uint16_t)(transmission_count_before + 1u);
    CHECK(request_at_relay_1.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(request_at_relay_1.radio.protocol_status == PROTO_OK);
    CHECK(request_at_relay_1.radio.start_rctu >=
          world.rx_windows[request_at_relay_1.window_index].start_rctu);
    CHECK(request_at_relay_1.radio.end_rctu <=
          world.rx_windows[request_at_relay_1.window_index].end_rctu);
    CHECK(world.transmissions[forwarded_request_tx].has_outbound);
    CHECK(world.transmissions[forwarded_request_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REQ);
    CHECK(capture_discovery_identity(&request_at_relay_1.radio,
                                     false,
                                     &request_identity) == PROTO_OK);
    CHECK(request_identity.origin_id == ORIGIN_ID);
    CHECK(request_identity.target_id == GATEWAY_ID);
    CHECK(request_identity.session_id == retry_request.packet.session_id);
    CHECK(request_identity.flood_epoch_id == request_identity.session_id);

    test_phase = "request_to_second_relay";
    CHECK(world.transmissions[forwarded_request_tx].outbound.packet.ttl == 1u);
    CHECK(world.transmissions[forwarded_request_tx].outbound.packet.session_id ==
          request_identity.session_id);
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_override_next_relay_random(&world, relay_2, 0u) ==
          MESH_SIM_OK);
    CHECK(receive_scheduled_outbound(&world,
                                     forwarded_request_tx,
                                     relay_2,
                                     true,
                                     &request_at_relay_2) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    initial_reply_tx = (uint16_t)transmission_count_before;
    CHECK(request_at_relay_2.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(world.transmissions[initial_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[initial_reply_tx].outbound.next_hop_id ==
          RELAY_1_ID);

    relay_1_reverse = mesh_relay_find_downlink(
        &world.roles[relay_1].relay, ORIGIN_ID);
    relay_2_reverse = mesh_relay_find_downlink(
        &world.roles[relay_2].relay, ORIGIN_ID);
    CHECK(relay_1_reverse != NULL);
    CHECK(relay_2_reverse != NULL);
    CHECK(relay_1_reverse->next_hop_id == ORIGIN_ID);
    CHECK(relay_2_reverse->next_hop_id == RELAY_1_ID);
    CHECK(relay_1_reverse->discovery_session_id == request_identity.session_id);
    CHECK(relay_2_reverse->discovery_session_id == request_identity.session_id);
    CHECK(relay_1_reverse->discovery_flood_epoch_id ==
          request_identity.flood_epoch_id);
    CHECK(relay_2_reverse->discovery_flood_epoch_id ==
          request_identity.flood_epoch_id);

    test_phase = "reply_to_first_relay";
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     initial_reply_tx,
                                     relay_1,
                                     true,
                                     &reply_at_relay_1) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 2u);
    route_reply_ack_tx = (uint16_t)transmission_count_before;
    forwarded_reply_tx = (uint16_t)(transmission_count_before + 1u);
    CHECK(reply_at_relay_1.radio.packet.msg_type == MSG_ROUTE_REPLY);
    CHECK(world.transmissions[route_reply_ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    CHECK(world.transmissions[route_reply_ack_tx].outbound.next_hop_id ==
          RELAY_2_ID);
    CHECK(world.transmissions[forwarded_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[forwarded_reply_tx].outbound.next_hop_id ==
          ORIGIN_ID);
    CHECK(capture_discovery_identity(&reply_at_relay_1.radio,
                                     true,
                                     &initial_reply_identity) == PROTO_OK);
    expected_nonce = mesh_route_reply_nonce(request_identity.origin_id,
                                            request_identity.target_id,
                                            request_identity.session_id,
                                            request_identity.flood_epoch_id);
    CHECK(initial_reply_identity.origin_id == request_identity.origin_id);
    CHECK(initial_reply_identity.target_id == request_identity.target_id);
    CHECK(initial_reply_identity.session_id == request_identity.session_id);
    CHECK(initial_reply_identity.flood_epoch_id ==
          request_identity.flood_epoch_id);
    CHECK(initial_reply_identity.reply_nonce == expected_nonce);

    test_phase = "reply_to_origin";
    CHECK(receive_scheduled_outbound(&world,
                                     forwarded_reply_tx,
                                     origin,
                                     true,
                                     &reply_at_origin) == MESH_SIM_OK);
    CHECK(capture_discovery_identity(&reply_at_origin.radio,
                                     true,
                                     &forwarded_reply_identity) == PROTO_OK);
    CHECK(same_discovery(&initial_reply_identity,
                         &forwarded_reply_identity));
    CHECK(!world.roles[origin].relay.route_discovery.active);

    test_phase = "usable_forward_and_reverse_routes";
    CHECK(assert_selected_hop(&world.roles[origin].relay,
                              GATEWAY_ID, RELAY_1_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_1].relay,
                              GATEWAY_ID, RELAY_2_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_1].relay,
                              ORIGIN_ID, ORIGIN_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              ORIGIN_ID, RELAY_1_ID) == 0);
    CHECK(route_selected(&world.roles[origin].relay.upstream)->hop_count == 2u);
    CHECK(route_selected(&world.roles[relay_1].relay.upstream)->hop_count == 1u);
    CHECK(route_selected(&world.roles[relay_2].relay.upstream)->hop_count == 0u);

    test_phase = "bounded_timing";
    completed_ms = (uint32_t)(reply_at_origin.radio.end_us / 1000u);
    CHECK(completed_ms >= DISCOVERY_START_MS);
    CHECK(completed_ms - DISCOVERY_START_MS <= DISCOVERY_BUDGET_MS);
    CHECK(world.last_error == MESH_SIM_OK);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_PARTIAL,
                                     RELAY_1_ID) == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_DECODED,
                                     0u) == 4u);

    if (run_ttl_ladder_data_case() != 0 ||
        run_blank_anchor_retains_local_click_until_route_case() != 0 ||
        run_route_collision_case() != 0 ||
        run_responder_slot_case(3u) != 0 ||
        run_responder_slot_case(8u) != 0) {
        return 1;
    }

    printf("mesh route formation scenarios passed "
           "(session=%u flood_epoch=%u nonce=0x%04x elapsed_ms=%u "
           "responder_groups=3,8)\n",
           request_identity.session_id,
           request_identity.flood_epoch_id,
           expected_nonce,
           completed_ms - DISCOVERY_START_MS);
    return 0;
}
