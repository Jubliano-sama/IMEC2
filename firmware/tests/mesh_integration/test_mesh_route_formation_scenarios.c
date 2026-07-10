#include "mesh_sim.h"

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
                                     1u,
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
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              GATEWAY_ID, GATEWAY_ID) == 0);
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

    if (run_route_collision_case() != 0 ||
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
