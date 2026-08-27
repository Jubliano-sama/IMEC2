#include "mesh_sim.h"
#include "mesh_sim_internal.h"

#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORIGIN_ID UINT64_C(0xA100)
#define RELAY_1_ID UINT64_C(0xA101)
#define RELAY_2_ID UINT64_C(0xA102)
#define RETAINED_PARENT_BASE_ID UINT64_C(0xB100)
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
#define TTL_LADDER_ATTEMPT_COUNT 5u
#define TTL_LADDER_RELAY_COUNT (MESH_DEFAULT_TTL - 1u)
#define TTL_LADDER_HOP_COUNT (TTL_LADDER_RELAY_COUNT + 1u)
#define TTL_LADDER_DATA_PHASE_STEP_MS 100u
#define TTL_LADDER_ACK_BASE_DELAY_MS 2000u
#define DISCOVERY_BUDGET_MS                                                \
    (2u * MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS + FLOOD_WAVE_MS +    \
     RREP_RESPONDER_JITTER_MAX_MS + 250u)
#define UNSEEDED_CLICKER_ID UINT64_C(0xA1f0)
#define UNSEEDED_CLICK_SESSION UINT32_C(0x5a110001)
#define DIRECT_PROBE_SESSION UINT32_C(0x5a110002)
#define DIRECT_PROBE_TX_PREPARE_US UINT64_C(2000)
#define DIRECT_PROBE_ACK_GUARD_US UINT64_C(50000)
#define DIRECT_PROBE_ACK_SERVICE_US UINT64_C(80000)
#define DIRECT_CLICK_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_CLICK_TX_SERVICE_US UINT64_C(80000)
#define DIRECT_CLICK_RX_GUARD_US UINT64_C(100)
#define DIRECT_CLICK_ACK_GUARD_US UINT64_C(50000)
#define DIRECT_CLICK_ACK_SERVICE_US UINT64_C(80000)

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

static char test_phase[96] = "setup";

static void set_test_phase(const char *phase)
{
    snprintf(test_phase, sizeof(test_phase), "%s", phase);
}

#define CHECK(expression) do {                                             \
    if (!(expression)) {                                                   \
        fprintf(stderr,                                                    \
                "scenario=two_relay_route_formation phase=%s line=%d "    \
                "assertion=%s\n",                                         \
                test_phase, __LINE__, #expression);                        \
        return 1;                                                          \
    }                                                                      \
} while (0)

static int pending_gateway_ack_confirm_packet(
    const struct mesh_sim_world *world,
    uint8_t node_index,
    struct proto_packet *packet)
{
    uint8_t payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t payload_len = 0u;

    return mesh_relay_pending_gateway_ack_confirm_wire(
        &world->roles[node_index].relay,
        mesh_sim_time_ms(world->now_us),
        packet,
        payload,
        sizeof(payload),
        &payload_len);
}

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

    /* A discovery TX can be made ready at the current scheduler timestamp
     * after a preceding direct-probe exchange.  In that case the test cannot
     * pre-arm the guard portion of the window; arm at the current time while
     * preserving the requested complete or partial end boundary. */
    if (window_start_us < world->now_us) {
        window_start_us = world->now_us;
    }

    /* Immediate unicast C5 responses may already own the paired peer turn. */
    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world->rx_windows[i];

        if (window->valid && window->node_index == receiver_index &&
            window->channel == channel && window->phy == phy &&
            window->start_us <= window_start_us &&
            window->end_us >= window_end_us) {
            if (window_index != NULL) {
                *window_index = (uint16_t)i;
            }
            return MESH_SIM_OK;
        }
    }

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
    const struct mesh_sim_transmission *tx;
    uint64_t expected_start_us;
    size_t reception_index;
    size_t matching_reception = SIZE_MAX;
    int ret;

    if (world == NULL || received == NULL ||
        transmission_index >= world->transmission_count ||
        receiver_index >= world->role_count) {
        return MESH_SIM_ERR_ARG;
    }
    tx = &world->transmissions[transmission_index];
    expected_start_us = dwm3000_timing_rctu_to_us_floor(
        tx->start_rctu +
        world->propagation_rctu[tx->node_index][receiver_index]);
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
    for (size_t i = reception_index; i < world->reception_count; i++) {
        const struct mesh_sim_reception *candidate = &world->receptions[i];

        if (candidate->source_id == world->roles[tx->node_index].id &&
            candidate->receiver_id == world->roles[receiver_index].id &&
            candidate->start_us == expected_start_us) {
            if (matching_reception != SIZE_MAX) {
                return MESH_SIM_ERR_EVENT_ORDER;
            }
            matching_reception = i;
        }
    }
    if (matching_reception == SIZE_MAX) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }

    received->radio = world->receptions[matching_reception];
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

static int repair_connection_before_payload_turn(
    struct mesh_sim_world *world,
    uint16_t connection_index)
{
    for (size_t attempt = 0u; attempt < 3u; attempt++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(world,
                                                  connection_index,
                                                  &action);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
        if (action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
            return MESH_SIM_OK;
        }
        if (action.kind != MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR) {
            return MESH_SIM_ERR_EVENT_ORDER;
        }
        if (!action.already_scheduled) {
            ret = mesh_sim_schedule_next_connection_event(world,
                                                          connection_index,
                                                          false);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
        ret = mesh_sim_run_until(world, action.end_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int run_connection_until_message(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    uint8_t sender_index,
    uint64_t next_hop_id,
    uint8_t msg_type,
    uint16_t *transmission_index)
{
    for (size_t turn = 0u; turn < MESH_RADIO_EVENT_MAX_MISSES; turn++) {
        size_t transmission_count_before;
        size_t reception_count_before;
        const struct mesh_sim_transmission *tx;
        int ret = repair_connection_before_payload_turn(world,
                                                        connection_index);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
        transmission_count_before = world->transmission_count;
        reception_count_before = world->reception_count;
        ret = run_connection_event(world, connection_index);
        if (ret != MESH_SIM_OK ||
            world->transmission_count > transmission_count_before + 1u ||
            world->reception_count > reception_count_before + 1u) {
            return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
        }
        if (world->transmission_count == transmission_count_before) {
            if (world->reception_count != reception_count_before) {
                return MESH_SIM_ERR_PROTOCOL;
            }
            continue;
        }
        if (world->reception_count != reception_count_before + 1u) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        tx = &world->transmissions[transmission_count_before];
        if (!tx->has_outbound ||
            world->receptions[reception_count_before].outcome !=
                MESH_SIM_RX_DECODED) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        if (tx->node_index == sender_index &&
            tx->outbound.next_hop_id == next_hop_id &&
            tx->outbound.packet.msg_type == msg_type) {
            if (transmission_index != NULL) {
                *transmission_index = (uint16_t)transmission_count_before;
            }
            return MESH_SIM_OK;
        }
        if (tx->outbound.packet.msg_type != MSG_MESH_HOP_ACK) {
            return MESH_SIM_ERR_PROTOCOL;
        }
    }
    return MESH_SIM_ERR_PROTOCOL;
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

static bool hop_ack_matches(
    const struct mesh_outbound *outbound,
    uint64_t ack_sender_id,
    uint64_t previous_hop_id,
    const struct proto_packet *data_packet,
    const uint8_t *data_payload,
    size_t data_payload_len)
{
    bool contains = false;

    if (outbound == NULL || data_packet == NULL ||
        (data_payload_len > 0u && data_payload == NULL)) {
        return false;
    }
    return outbound->packet.msg_type == MSG_MESH_HOP_ACK &&
           outbound->packet.src_id == ack_sender_id &&
           outbound->packet.dst_id == previous_hop_id &&
           outbound->packet.session_id == data_packet->session_id &&
           outbound->next_hop_id == previous_hop_id &&
           mesh_ack_payload_contains_packet(&outbound->packet,
                                            outbound->payload,
                                            outbound->payload_len,
                                            data_packet,
                                            data_payload,
                                            data_payload_len,
                                            &contains) == PROTO_OK &&
           contains;
}

static bool queued_hop_ack_matches(
    const struct mesh_sim_role_instance *node,
    uint64_t ack_sender_id,
    uint64_t previous_hop_id,
    const struct proto_packet *data_packet,
    const uint8_t *data_payload,
    size_t data_payload_len)
{
    size_t match_count = 0u;

    if (node == NULL) {
        return false;
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &node->tx_queue[i];

        if (!queued->valid || queued->needs_relay_start ||
            !hop_ack_matches(&queued->outbound,
                             ack_sender_id,
                             previous_hop_id,
                             data_packet,
                             data_payload,
                             data_payload_len)) {
            continue;
        }
        match_count++;
    }
    return match_count == 1u;
}

static void remove_queued_entry_for_test(struct mesh_sim_role_instance *node,
                                         size_t index)
{
    memset(&node->tx_queue[index], 0, sizeof(node->tx_queue[index]));
    if (node->tx_queue_count > 0u) {
        node->tx_queue_count--;
    }
}

static void make_direct_probe(struct mesh_outbound *probe, uint64_t source_id)
{
    memset(probe, 0, sizeof(*probe));
    probe->packet.msg_type = MSG_GATEWAY_ROUTE_REQ;
    probe->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    probe->packet.src_id = source_id;
    probe->packet.dst_id = GATEWAY_ID;
    probe->packet.session_id = DIRECT_PROBE_SESSION;
    probe->packet.seq = 1u;
    probe->packet.ttl = MESH_DEFAULT_TTL;
    probe->next_hop_id = GATEWAY_ID;
    probe->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
}

static int run_direct_probe_for_route(struct mesh_sim_world *world,
                                      uint8_t sender,
                                      uint8_t gateway)
{
    struct mesh_outbound probe;
    struct mesh_outbound gateway_ack;
    struct route_reception reception;
    struct route_reception ack_reception;
    uint16_t probe_tx;
    uint16_t ack_tx;
    int ack_queue_index = -1;
    uint64_t start_us;
    int ret;

    make_direct_probe(&probe, world->roles[sender].id);
    start_us = world->now_us + DIRECT_PROBE_TX_PREPARE_US;
    ret = transmit_route_outbound(world,
                                  sender,
                                  gateway,
                                  &probe,
                                  start_us,
                                  true,
                                  &reception,
                                  &probe_tx);
    if (ret != MESH_SIM_OK || reception.radio.outcome != MESH_SIM_RX_DECODED ||
        reception.radio.packet.msg_type != MSG_GATEWAY_ROUTE_REQ) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued =
            &world->roles[gateway].tx_queue[i];

        if (queued->valid && !queued->needs_relay_start &&
            queued->outbound.packet.msg_type == MSG_GATEWAY_ACK &&
            queued->outbound.next_hop_id == world->roles[sender].id) {
            ack_queue_index = (int)i;
            break;
        }
    }
    if (ack_queue_index < 0) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    gateway_ack = world->roles[gateway].tx_queue[ack_queue_index].outbound;
    remove_queued_entry_for_test(&world->roles[gateway],
                                 (size_t)ack_queue_index);
    gateway_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    start_us = world->now_us + DIRECT_PROBE_ACK_GUARD_US;
    ret = transmit_route_outbound(world,
                                  gateway,
                                  sender,
                                  &gateway_ack,
                                  start_us,
                                  true,
                                  &ack_reception,
                                  &ack_tx);
    if (ret != MESH_SIM_OK || ack_reception.radio.outcome != MESH_SIM_RX_DECODED ||
        ack_reception.radio.packet.msg_type != MSG_GATEWAY_ACK) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    return mesh_relay_note_direct_gateway_route(
        &world->roles[sender].relay,
        mesh_sim_time_ms(world->now_us));
}

static int build_unseeded_click(struct proto_packet *packet,
                                uint8_t *payload,
                                size_t payload_capacity,
                                size_t *payload_len,
                                uint64_t anchor_id)
{
    const uint64_t participant_anchor_ids[] = {
        anchor_id,
        UINT64_C(0xefffffffffffffff),
    };
    const int32_t distance_samples[] = {120};
    const uint8_t range_round_indices[] = {0u};
    const uint64_t sequence_start_timestamps_ms[] = {1u};
    const struct range_report_fields fields = {
        .clicker_id = UNSEEDED_CLICKER_ID,
        .anchor_id = anchor_id,
        .event_seq = UNSEEDED_CLICK_SESSION,
        .timestamp_ms = 1u,
        .distance_mm = 120,
        .quality = 90u,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = range_round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 1u,
        .participant_anchor_ids = participant_anchor_ids,
        .participant_anchor_count = 2u,
        .burst_id = UNSEEDED_CLICK_SESSION,
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t length = 0u;
    int ret;

    if (packet == NULL || payload == NULL || payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = report_append_range_tlvs(payload,
                                   payload_capacity,
                                   &length,
                                   &fields);
    if (ret != PROTO_OK || length > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = report_init_click_packet(packet,
                                   anchor_id,
                                   GATEWAY_ID,
                                   proto_click_report_session_id(
                                       fields.clicker_id,
                                       fields.event_seq),
                                   1u,
                                   (uint8_t)length);
    if (ret != PROTO_OK) {
        return ret;
    }
    *payload_len = length;
    return report_validate_click_payload(packet, payload, length);
}

static int build_unseeded_self_test(struct proto_packet *packet,
                                    uint8_t *payload,
                                    size_t payload_capacity,
                                    size_t *payload_len,
                                    uint64_t clicker_id)
{
    const struct self_test_report_fields fields = {
        .clicker_id = clicker_id,
        .event_seq = UNSEEDED_CLICK_SESSION,
        .failure_code = 0u,
        .battery_mv = 3000u,
    };
    size_t length = 0u;
    int ret;

    if (packet == NULL || payload == NULL || payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = report_append_self_test_tlvs(payload,
                                        payload_capacity,
                                        &length,
                                        &fields);
    if (ret != PROTO_OK || length > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = report_init_self_test_packet(packet,
                                       clicker_id,
                                       GATEWAY_ID,
                                       fields.event_seq,
                                       (uint16_t)fields.event_seq,
                                       (uint8_t)length);
    if (ret != PROTO_OK) {
        return ret;
    }
    *payload_len = length;
    return proto_self_test_report_validate(packet, payload, length);
}

static bool click_identity_matches(const struct mesh_outbound *outbound,
                                   const struct proto_packet *packet,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    return outbound != NULL && packet != NULL &&
           outbound->packet.msg_type == packet->msg_type &&
           outbound->packet.src_id == packet->src_id &&
           outbound->packet.dst_id == packet->dst_id &&
           outbound->packet.session_id == packet->session_id &&
           outbound->packet.seq == packet->seq &&
           outbound->payload_len == payload_len &&
           memcmp(outbound->payload, payload, payload_len) == 0;
}

static bool queue_has_click(const struct mesh_sim_role_instance *node,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint64_t next_hop_id)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &node->tx_queue[i];

        if (queued->valid && queued->outbound.next_hop_id == next_hop_id &&
            queued->needs_relay_start &&
            click_identity_matches(&queued->outbound,
                                   packet,
                                   payload,
                                   payload_len)) {
            return true;
        }
    }
    return false;
}

static int run_scheduled_connection_repair(struct mesh_sim_world *world,
                                           uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world, connection_index, &action);

    if (ret != MESH_SIM_OK ||
        action.kind != MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR ||
        !action.already_scheduled) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int run_direct_click_turn(struct mesh_sim_world *world,
                                 uint8_t sender,
                                 uint8_t gateway)
{
    uint64_t ready_us = world->now_us;
    uint64_t air_start_us;
    uint64_t tx_deadline_us;
    uint64_t arrival_end_us;
    uint64_t rx_end_us;
    uint64_t ack_start_us;
    uint64_t ack_end_us;
    uint16_t payload_tx;
    uint16_t ack_tx;
    int ret;

    if (world->roles[sender].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[sender].dwm3000.cpu_busy_until_us;
    }
    if (world->roles[gateway].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[gateway].dwm3000.cpu_busy_until_us;
    }
    if (ready_us > world->now_us) {
        ret = mesh_sim_run_until(world, ready_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    air_start_us = world->now_us + DIRECT_CLICK_TX_PREPARE_US;
    tx_deadline_us = air_start_us + DIRECT_CLICK_TX_SERVICE_US;
    ret = mesh_sim_direct_gateway_start_queued_tx(world,
                                                  sender,
                                                  air_start_us,
                                                  tx_deadline_us,
                                                  &payload_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    arrival_end_us = world->transmissions[payload_tx].end_us +
                     world->propagation_us[sender][gateway];
    rx_end_us = arrival_end_us + DIRECT_CLICK_RX_GUARD_US;
    ret = mesh_sim_direct_gateway_arm_rx(world,
                                         gateway,
                                         air_start_us,
                                         rx_end_us);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, rx_end_us);
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->roles[gateway].dwm3000.cpu_busy_until_us > world->now_us) {
        ret = mesh_sim_run_until(world,
                                 world->roles[gateway].dwm3000.cpu_busy_until_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    ack_start_us = world->now_us + DIRECT_CLICK_ACK_GUARD_US;
    ack_end_us = ack_start_us + DIRECT_CLICK_ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(world,
                                               gateway,
                                               sender,
                                               ack_start_us,
                                               ack_end_us,
                                               &ack_tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_run_until(world, ack_end_us);
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
    set_test_phase(phase);
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
        set_test_phase(phase);
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
    set_test_phase(phase);
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

static int run_exact_hop_multi_responder_case(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound gateway_adv;
    struct mesh_outbound request;
    struct mesh_relay_result parent_adv_result = {0};
    struct mesh_relay_result exact_adv_result = {0};
    struct route_reception reply_reception;
    struct route_reception ack_reception;
    const struct route_candidate *selected;
    const uint8_t exact_two_hop_flags =
        MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED |
        MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(2u);
    const uint64_t scenario_origin_id = ORIGIN_ID + UINT64_C(0x7000);
    const uint64_t wrong_responder_id = RELAY_1_ID + UINT64_C(0x7000);
    const uint64_t route_parent_id = RELAY_2_ID + UINT64_C(0x7000);
    const uint64_t exact_responder_id = RELAY_2_ID + UINT64_C(0x7100);
    uint8_t origin;
    uint8_t wrong_responder;
    uint8_t route_parent;
    uint8_t exact_responder;
    uint8_t gateway;
    uint8_t request_flags = 0u;
    uint8_t reply_hop_count = 0u;
    uint16_t request_tx;
    uint16_t reply_tx;
    uint16_t ack_tx;
    size_t reception_count_before;
    size_t transmission_count_before;

    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0xe2ac7002));
    set_test_phase("exact_hop_multi_responder_setup");
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            scenario_origin_id,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            wrong_responder_id,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &wrong_responder) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            route_parent_id,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &route_parent) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            exact_responder_id,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &exact_responder) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, origin, wrong_responder, 99u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, origin, exact_responder, 96u, 7u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, wrong_responder, gateway, 99u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, exact_responder, route_parent, 96u, 6u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, route_parent, gateway, 98u, 5u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world,
                                 wrong_responder,
                                 gateway,
                                 0u,
                                 ROUTE_EPOCH) == PROTO_OK);

    /* Install a complete two-relay gateway path, including ancestry, through
     * the same route-advertisement validation used by production RX. */
    CHECK(mesh_relay_build_gateway_route_adv(&world.roles[gateway].relay,
                                              77u,
                                              1000u,
                                              &gateway_adv) == PROTO_OK);
    CHECK(mesh_relay_handle_rx(&world.roles[route_parent].relay,
                               &gateway_adv.packet,
                               gateway_adv.payload,
                               gateway_adv.payload_len,
                               GATEWAY_ID,
                               98u,
                               1010u,
                               &parent_adv_result) == PROTO_OK);
    CHECK((parent_adv_result.actions &
           MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) != 0u);
    CHECK(mesh_relay_handle_rx(&world.roles[exact_responder].relay,
                               &parent_adv_result.gateway_route_adv.packet,
                               parent_adv_result.gateway_route_adv.payload,
                               parent_adv_result.gateway_route_adv.payload_len,
                               route_parent_id,
                               96u,
                               1020u,
                               &exact_adv_result) == PROTO_OK);
    selected = route_selected(&world.roles[wrong_responder].relay.upstream);
    CHECK(selected != NULL && selected->hop_count == 0u);
    selected = route_selected(&world.roles[exact_responder].relay.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == route_parent_id);
    CHECK(selected->hop_count == 1u);

    set_test_phase("exact_hop_multi_responder_request");
    CHECK(mesh_relay_prepare_route_request_with_timing_flags(
              &world.roles[origin].relay,
              GATEWAY_ID,
              NULL,
              0u,
              exact_two_hop_flags,
              RESPONDER_REPLY_DELAY_MS,
              DISCOVERY_START_MS,
              0u,
              &request) == PROTO_OK);
    CHECK(request.packet.ttl == 1u);
    CHECK(find_tlv_u8(request.payload,
                      request.payload_len,
                      TLV_ROUTE_REQUEST_FLAGS,
                      &request_flags) == PROTO_OK);
    CHECK(request_flags == exact_two_hop_flags);
    CHECK(mesh_sim_schedule_outbound_tx(
              &world,
              origin,
              (uint64_t)DISCOVERY_START_MS * 1000u,
              &request,
              &request_tx) == MESH_SIM_OK);
    CHECK(mesh_sim_override_next_relay_random(
              &world, wrong_responder, 0u) == MESH_SIM_OK);
    CHECK(mesh_sim_override_next_relay_random(
              &world, exact_responder, 0u) == MESH_SIM_OK);
    reception_count_before = world.reception_count;
    transmission_count_before = world.transmission_count;
    CHECK(schedule_outbound_rx(&world,
                               request_tx,
                               wrong_responder,
                               true,
                               NULL) == MESH_SIM_OK);
    CHECK(schedule_outbound_rx(&world,
                               request_tx,
                               exact_responder,
                               true,
                               NULL) == MESH_SIM_OK);
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world, request_tx)) == MESH_SIM_OK);
    CHECK(world.reception_count == reception_count_before + 2u);
    CHECK(world.receptions[reception_count_before].receiver_id ==
          wrong_responder_id);
    CHECK(world.receptions[reception_count_before].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(world.receptions[reception_count_before + 1u].receiver_id ==
          exact_responder_id);
    CHECK(world.receptions[reception_count_before + 1u].outcome ==
          MESH_SIM_RX_DECODED);
    CHECK(world.receptions[reception_count_before].end_us <
          world.receptions[reception_count_before + 1u].end_us);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    reply_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[reply_tx].node_index == exact_responder);
    CHECK(world.transmissions[reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[reply_tx].outbound.next_hop_id ==
          scenario_origin_id);
    CHECK(find_tlv_u8(world.transmissions[reply_tx].outbound.payload,
                      world.transmissions[reply_tx].outbound.payload_len,
                      TLV_HOP_COUNT,
                      &reply_hop_count) == PROTO_OK);
    CHECK(reply_hop_count == 2u);

    set_test_phase("exact_hop_multi_responder_install");
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     reply_tx,
                                     origin,
                                     true,
                                     &reply_reception) == MESH_SIM_OK);
    CHECK(reply_reception.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    ack_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    selected = route_selected(&world.roles[origin].relay.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == exact_responder_id);
    CHECK(selected->hop_count == 2u);
    CHECK(receive_scheduled_outbound(&world,
                                     ack_tx,
                                     exact_responder,
                                     true,
                                     &ack_reception) == MESH_SIM_OK);
    CHECK(ack_reception.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(!world.roles[origin].relay.route_discovery.active);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int run_ttl_ladder_data_case(void)
{
    static const uint8_t expected_attempt_ttls[TTL_LADDER_ATTEMPT_COUNT] = {
        1u, 2u, 4u, 6u, MESH_DEFAULT_TTL,
    };
    static struct mesh_sim_world world;
    struct discovery_identity attempt_identity;
    struct discovery_identity route_identity;
    struct discovery_identity reply_identity;
    struct proto_packet data_packet;
    struct proto_packet confirm_packet;
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
    char phase[96];

    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0x64006400));
    set_test_phase("ttl_ladder_setup");
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
        set_test_phase(phase);
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
            set_test_phase(phase);
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
        uint16_t current_ack_tx = UINT16_MAX;

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
            set_test_phase(phase);
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
                uint16_t forwarded_tx =
                    (uint16_t)transmission_count_before;

                CHECK(world.transmission_count ==
                      transmission_count_before + 1u);
                CHECK(world.transmissions[forwarded_tx].outbound.packet
                          .msg_type == MSG_ROUTE_REPLY);
                CHECK(world.transmissions[forwarded_tx].outbound.next_hop_id ==
                      world.roles[path[receiver_path_index - 1u]].id);
                CHECK(world.roles[receiver].route_reply_upstream_ack_valid);
                CHECK(world.roles[receiver].route_reply_upstream_ack
                          .next_hop_id == world.roles[sender].id);
                current_reply_tx = forwarded_tx;
            } else {
                CHECK(world.transmission_count ==
                      transmission_count_before + 1u);
                CHECK(world.transmissions[transmission_count_before].outbound
                          .packet.msg_type == MSG_ROUTE_REPLY_ACK);
                current_ack_tx = (uint16_t)transmission_count_before;
            }
        }

        CHECK(current_ack_tx != UINT16_MAX);
        for (size_t receiver_path_index = 1u;
             receiver_path_index < TTL_LADDER_RELAY_COUNT;
             receiver_path_index++) {
            uint8_t receiver = path[receiver_path_index];
            size_t transmission_count_before = world.transmission_count;

            snprintf(phase,
                     sizeof(phase),
                     "ttl_ladder_reply_ack_hop_%zu",
                     receiver_path_index);
            set_test_phase(phase);
            CHECK(mesh_sim_run_until(
                      &world,
                      transmission_evaluation_us(&world,
                                                 current_ack_tx)) ==
                  MESH_SIM_OK);
            CHECK(!world.roles[receiver].route_reply_upstream_ack_valid);
            CHECK(world.transmission_count ==
                  transmission_count_before + 1u);
            current_ack_tx = (uint16_t)transmission_count_before;
            CHECK(world.transmissions[current_ack_tx].outbound.packet.msg_type ==
                  MSG_ROUTE_REPLY_ACK);
            CHECK(world.transmissions[current_ack_tx].outbound.next_hop_id ==
                  world.roles[path[receiver_path_index + 1u]].id);
        }

        {
            size_t transmission_count_before = world.transmission_count;

            set_test_phase("ttl_ladder_reply_ack_responder");
            CHECK(mesh_sim_run_until(
                      &world,
                      transmission_evaluation_us(&world,
                                                 current_ack_tx)) ==
                  MESH_SIM_OK);
            CHECK(world.transmission_count == transmission_count_before);
        }
        CHECK(!world.roles[relays[TTL_LADDER_RELAY_COUNT - 1u]]
                   .route_reply_upstream_ack_valid);
        CHECK(!world.roles[relays[TTL_LADDER_RELAY_COUNT - 1u]]
                   .relay.route_reply_ack_expectation.active);
    }
    CHECK(!world.roles[origin].relay.route_discovery.active);

    set_test_phase("ttl_ladder_installed_route");
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

    set_test_phase("ttl_ladder_queue_data");
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
        set_test_phase(phase);
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

    /* The gateway receipt terminates at its immediate relay. Every earlier
     * edge closes independently with the hop ACK already queued when that
     * parent accepted the exact report. */
    {
        const size_t gateway_connection = TTL_LADDER_HOP_COUNT - 1u;
        const uint8_t gateway_relay = path[gateway_connection];
        uint16_t transmission_index = UINT16_MAX;
        const struct mesh_sim_transmission *tx;

        set_test_phase("ttl_ladder_gateway_receipt_to_adjacent_relay");
        CHECK(run_connection_until_message(
                  &world,
                  connections[gateway_connection],
                  gateway,
                  world.roles[gateway_relay].id,
                  MSG_GATEWAY_ACK,
                  &transmission_index) == MESH_SIM_OK);
        tx = &world.transmissions[transmission_index];
        CHECK(route_ack_matches(&tx->outbound,
                                world.roles[gateway_relay].id,
                                &data_packet));
        CHECK(world.roles[gateway_relay].relay.pending
                  .gateway_ack_confirm_pending);
    }

    for (size_t reverse_step = 0u;
         reverse_step + 1u < TTL_LADDER_HOP_COUNT;
         reverse_step++) {
        size_t connection = TTL_LADDER_HOP_COUNT - reverse_step - 2u;
        uint8_t child = path[connection];
        uint8_t parent = path[connection + 1u];
        uint16_t transmission_index = UINT16_MAX;
        const struct mesh_sim_transmission *tx;
        uint16_t requested_seq = 0u;

        snprintf(phase,
                 sizeof(phase),
                 "ttl_ladder_hop_custody_%zu",
                 reverse_step + 1u);
        set_test_phase(phase);
        CHECK(run_connection_until_message(
                  &world,
                  connections[connection],
                  parent,
                  world.roles[child].id,
                  MSG_MESH_HOP_ACK,
                  &transmission_index) == MESH_SIM_OK);
        tx = &world.transmissions[transmission_index];
        CHECK(tx->outbound.packet.src_id == world.roles[parent].id);
        CHECK(tx->outbound.packet.dst_id == world.roles[child].id);
        CHECK(tx->outbound.packet.session_id == data_packet.session_id);
        CHECK(find_tlv_u16(tx->outbound.payload,
                           tx->outbound.payload_len,
                           TLV_REQUESTED_MSG_SEQ,
                           &requested_seq) == PROTO_OK);
        CHECK(requested_seq == data_packet.seq);
        CHECK(world.roles[child].relay.pending.state == MESH_RELAY_TX_IDLE);
        if (connection == 0u) {
            CHECK(world.roles[child].tx_queue_count == 0u);
        } else {
            /* This relay has relinquished its upstream report custody, but
             * it still owes the exact hop ACK to its own child.  That edge is
             * serviced by the next reverse-walk iteration. */
            CHECK(world.roles[child].tx_queue_count == 1u);
            CHECK(queued_hop_ack_matches(
                      &world.roles[child],
                      world.roles[child].id,
                      world.roles[path[connection - 1u]].id,
                      &data_packet,
                      data_payload,
                      data_payload_len));
        }
    }
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(pending_gateway_ack_confirm_packet(&world,
                                             path[TTL_LADDER_HOP_COUNT - 1u],
                                             &confirm_packet) == PROTO_OK);
    CHECK(confirm_packet.src_id == data_packet.src_id);
    CHECK(confirm_packet.dst_id == data_packet.dst_id);
    CHECK(confirm_packet.session_id == data_packet.session_id);
    CHECK(confirm_packet.seq == data_packet.seq);
    {
        const size_t gateway_connection = TTL_LADDER_HOP_COUNT - 1u;
        const uint8_t gateway_relay = path[gateway_connection];
        uint16_t transmission_index = UINT16_MAX;
        const struct mesh_sim_transmission *tx;

        set_test_phase("ttl_ladder_adjacent_relay_confirm");
        CHECK(run_connection_until_message(
                  &world,
                  connections[gateway_connection],
                  gateway_relay,
                  GATEWAY_ID,
                  MSG_GATEWAY_ACK_CONFIRM,
                  &transmission_index) == MESH_SIM_OK);
        tx = &world.transmissions[transmission_index];
        CHECK(tx->outbound.packet.src_id == confirm_packet.src_id);
        CHECK(tx->outbound.packet.dst_id == confirm_packet.dst_id);
        CHECK(tx->outbound.packet.session_id == confirm_packet.session_id);
        CHECK(tx->outbound.packet.seq == confirm_packet.seq);
        CHECK(tx->outbound.next_hop_id == GATEWAY_ID);
    }

    {
        const size_t gateway_connection = TTL_LADDER_HOP_COUNT - 1u;
        const uint8_t gateway_relay = path[gateway_connection];
        uint16_t transmission_index = UINT16_MAX;
        const struct mesh_sim_transmission *tx;

        set_test_phase("ttl_ladder_adjacent_relay_confirm_receipt");
        CHECK(run_connection_until_message(
                  &world,
                  connections[gateway_connection],
                  gateway,
                  world.roles[gateway_relay].id,
                  MSG_GATEWAY_ACK,
                  &transmission_index) == MESH_SIM_OK);
        tx = &world.transmissions[transmission_index];
        CHECK(route_ack_matches(&tx->outbound,
                                world.roles[gateway_relay].id,
                                &confirm_packet));
        CHECK(world.roles[gateway_relay].relay.pending.state ==
              MESH_RELAY_TX_IDLE);
    }
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 0u);
    CHECK(mesh_sim_count_transitions(
              &world,
              MESH_SIM_TRANSITION_GATEWAY_ACKED,
              world.roles[path[TTL_LADDER_HOP_COUNT - 1u]].id) == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                     0u) == 0u);
    for (size_t i = 0u; i < TTL_LADDER_HOP_COUNT; i++) {
        CHECK(world.connections[connections[i]].completed_events >=
              (i + 1u == TTL_LADDER_HOP_COUNT ? 4u : 2u));
        CHECK(world.connections[connections[i]].completed_repairs <= 1u);
        CHECK(world.connections[connections[i]].timing_a.timing_fresh);
        CHECK(world.connections[connections[i]].timing_b.timing_fresh);
        CHECK(!world.connections[connections[i]].timing_a.fallback_required);
        CHECK(!world.connections[connections[i]].timing_b.fallback_required);
        CHECK(world.roles[path[i]].tx_queue_count == 0u);
    }
    CHECK(world.roles[gateway].tx_queue_count == 0u);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int run_blank_anchor_retains_local_click_until_route_case(void)
{
    static struct mesh_sim_world world;
    struct proto_packet click = {0};
    uint8_t click_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t click_payload_len = 0u;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t connection;
    size_t click_tx_count = 0u;

    mesh_sim_init(&world, SCENARIO_SEED ^ UINT32_C(0xb1a0c001));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID, GATEWAY_ID, ROUTE_EPOCH, &anchor) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH, &gateway) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, anchor, gateway, 100u, 1u) == MESH_SIM_OK);

    CHECK(build_unseeded_click(&click,
                               click_payload,
                               sizeof(click_payload),
                               &click_payload_len,
                               ORIGIN_ID) == PROTO_OK);
    CHECK(mesh_sim_queue_originated(&world, anchor, &click,
                                    click_payload, click_payload_len) ==
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

    set_test_phase("route_collision_setup");
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

    set_test_phase("route_collision_delivery");
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

static int run_unseeded_report_route_custody_case(bool self_test_report)
{
    static struct mesh_sim_world world;
    struct proto_packet click_packet;
    struct proto_packet confirm_packet;
    struct route_reception partial;
    struct route_reception request_at_relay_1;
    struct route_reception request_at_relay_2;
    struct route_reception reply_at_relay_1;
    struct route_reception reply_at_origin;
    uint8_t click_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t click_payload_len = 0u;
    uint8_t origin;
    uint8_t relay_1;
    uint8_t relay_2;
    uint8_t gateway;
    uint16_t first_request_tx;
    uint16_t retry_request_tx;
    uint16_t forwarded_request_tx;
    uint16_t route_reply_tx;
    uint16_t forwarded_reply_tx;
    uint16_t downstream_reply_ack_tx;
    uint16_t upstream_reply_ack_tx;
    uint16_t phase_tx;
    uint16_t connection_origin_relay;
    uint16_t connection_relay_relay;
    uint32_t retry_ms;
    size_t transmission_count_before;
    size_t gateway_delivery_before;
    uint64_t latest_tx_end_us;
    struct mesh_event_params params;
    int ret;

    set_test_phase(self_test_report ? "unseeded_self_test_setup" :
                                      "unseeded_click_setup");
    mesh_sim_init(&world,
                  SCENARIO_SEED ^
                      (self_test_report ? UINT32_C(0x5e1f7e57) :
                                          UINT32_C(0xc11c7e01)));
    CHECK(mesh_sim_add_role(&world,
                            self_test_report ? MESH_SIM_ROLE_CLICKER :
                                               MESH_SIM_ROLE_ANCHOR,
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
    CHECK(mesh_sim_set_link(&world, origin, relay_1, 97u, 7u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_1, relay_2, 94u, 11u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_2, gateway, 99u, 5u) ==
          MESH_SIM_OK);

    /* Only the last relay has direct gateway contact, learned from a real
     * probe/ACK exchange rather than a simulator route fixture. */
    set_test_phase("unseeded_click_direct_probe");
    CHECK(run_direct_probe_for_route(&world, relay_2, gateway) == MESH_SIM_OK);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              GATEWAY_ID, GATEWAY_ID) == 0);
    CHECK(route_selected(&world.roles[relay_1].relay.upstream) == NULL);
    CHECK(route_selected(&world.roles[origin].relay.upstream) == NULL);
    gateway_delivery_before = world.roles[gateway].delivery_count;

    if (self_test_report) {
        CHECK(build_unseeded_self_test(&click_packet,
                                       click_payload,
                                       sizeof(click_payload),
                                       &click_payload_len,
                                       ORIGIN_ID) == PROTO_OK);
        CHECK(click_packet.msg_type == MSG_SELF_TEST_REPORT);
        CHECK(click_packet.flags ==
              (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC));
    } else {
        CHECK(build_unseeded_click(&click_packet,
                                   click_payload,
                                   sizeof(click_payload),
                                   &click_payload_len,
                                   ORIGIN_ID) == PROTO_OK);
        CHECK(click_packet.msg_type == MSG_CLICK_REPORT);
    }
    set_test_phase("unseeded_click_queued_without_route");
    CHECK(mesh_sim_queue_originated(&world,
                                    origin,
                                    &click_packet,
                                    click_payload,
                                    click_payload_len) == MESH_SIM_OK);
    CHECK(world.roles[origin].route_waiting_valid);
    CHECK(world.roles[origin].tx_queue_count == 0u);
    CHECK(click_identity_matches(&world.roles[origin].route_waiting_outbound,
                                 &click_packet,
                                 click_payload,
                                 click_payload_len));
    CHECK(world.roles[origin].route_discovery_requests == 1u);
    first_request_tx = (uint16_t)(world.transmission_count - 1u);

    /* The first TTL-1 request is deliberately truncated.  The original
     * click remains the sole route-wait owner while discovery retries. */
    set_test_phase("unseeded_click_first_route_attempt_partial");
    ret = schedule_outbound_rx(&world,
                               first_request_tx,
                               relay_1,
                               false,
                               &partial.window_index);
    CHECK(ret == MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&world,
                             transmission_evaluation_us(&world,
                                                        first_request_tx)) ==
          MESH_SIM_OK);
    partial.radio = world.receptions[world.reception_count - 1u];
    CHECK(partial.radio.outcome == MESH_SIM_RX_FRAME_TIMEOUT);
    CHECK(world.roles[origin].route_waiting_valid);
    CHECK(click_identity_matches(&world.roles[origin].route_waiting_outbound,
                                 &click_packet,
                                 click_payload,
                                 click_payload_len));

    retry_ms = world.roles[origin].relay.route_discovery.next_request_ms;
    CHECK(retry_ms > mesh_sim_time_ms(world.now_us));
    set_test_phase("unseeded_click_route_retry");
    CHECK(mesh_sim_run_until(&world, (uint64_t)retry_ms * 1000u) ==
          MESH_SIM_OK);
    CHECK(world.transmission_count >= 2u);
    retry_request_tx = (uint16_t)(world.transmission_count - 1u);
    CHECK(world.transmissions[retry_request_tx].has_outbound);
    CHECK(world.transmissions[retry_request_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REQ);
    CHECK(world.transmissions[retry_request_tx].outbound.packet.ttl == 2u);
    CHECK(world.transmissions[retry_request_tx].outbound.packet.session_id !=
          world.transmissions[first_request_tx].outbound.packet.session_id);
    CHECK(world.roles[origin].route_waiting_valid);
    CHECK(click_identity_matches(&world.roles[origin].route_waiting_outbound,
                                 &click_packet,
                                 click_payload,
                                 click_payload_len));

    set_test_phase("unseeded_click_route_request_relay_1");
    CHECK(mesh_sim_override_next_relay_random(&world, relay_1, 0u) ==
          MESH_SIM_OK);
    CHECK(receive_scheduled_outbound(&world,
                                     retry_request_tx,
                                     relay_1,
                                     true,
                                     &request_at_relay_1) == MESH_SIM_OK);
    CHECK(request_at_relay_1.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(request_at_relay_1.radio.packet.msg_type == MSG_ROUTE_REQ);
    transmission_count_before = world.transmission_count;
    CHECK(transmission_count_before > retry_request_tx + 1u);
    forwarded_request_tx = (uint16_t)(transmission_count_before - 1u);
    CHECK(world.transmissions[forwarded_request_tx].has_outbound);
    CHECK(world.transmissions[forwarded_request_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REQ);
    CHECK(world.transmissions[forwarded_request_tx].outbound.packet.ttl == 1u);

    set_test_phase("unseeded_click_route_request_relay_2");
    CHECK(mesh_sim_override_next_relay_random(&world, relay_2, 0u) ==
          MESH_SIM_OK);
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     forwarded_request_tx,
                                     relay_2,
                                     true,
                                     &request_at_relay_2) == MESH_SIM_OK);
    CHECK(request_at_relay_2.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(request_at_relay_2.radio.packet.msg_type == MSG_ROUTE_REQ);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    route_reply_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[route_reply_tx].has_outbound);
    CHECK(world.transmissions[route_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[route_reply_tx].outbound.next_hop_id ==
          world.roles[relay_1].id);

    set_test_phase("unseeded_click_route_reply_relay_1");
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     route_reply_tx,
                                     relay_1,
                                     true,
                                     &reply_at_relay_1) == MESH_SIM_OK);
    CHECK(reply_at_relay_1.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(reply_at_relay_1.radio.packet.msg_type == MSG_ROUTE_REPLY);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    forwarded_reply_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[forwarded_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[forwarded_reply_tx].outbound.next_hop_id ==
          world.roles[origin].id);
    CHECK(world.roles[relay_1].route_reply_upstream_ack_valid);

    set_test_phase("unseeded_click_route_reply_origin");
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     forwarded_reply_tx,
                                     origin,
                                     true,
                                     &reply_at_origin) == MESH_SIM_OK);
    CHECK(reply_at_origin.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(reply_at_origin.radio.packet.msg_type == MSG_ROUTE_REPLY);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    CHECK(world.transmissions[transmission_count_before].outbound.packet
              .msg_type == MSG_ROUTE_REPLY_ACK);
    downstream_reply_ack_tx = (uint16_t)transmission_count_before;
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world,
                                         downstream_reply_ack_tx)) ==
          MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    upstream_reply_ack_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[upstream_reply_ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    CHECK(world.transmissions[upstream_reply_ack_tx].outbound.next_hop_id ==
          world.roles[relay_2].id);
    CHECK(!world.roles[relay_1].route_reply_upstream_ack_valid);
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world,
                                         upstream_reply_ack_tx)) ==
          MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before);
    CHECK(!world.roles[origin].route_waiting_valid);
    CHECK(queue_has_click(&world.roles[origin],
                          &click_packet,
                          click_payload,
                          click_payload_len,
                          world.roles[relay_1].id));
    CHECK(assert_selected_hop(&world.roles[origin].relay,
                              GATEWAY_ID, RELAY_1_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_1].relay,
                              GATEWAY_ID, RELAY_2_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              GATEWAY_ID, GATEWAY_ID) == 0);

    latest_tx_end_us = world.now_us;
    for (size_t i = 0u; i < world.transmission_count; i++) {
        if (world.transmissions[i].has_outbound) {
            latest_tx_end_us = max_u64(latest_tx_end_us,
                                       transmission_evaluation_us(
                                           &world, (uint16_t)i));
        }
    }
    CHECK(mesh_sim_run_until(&world, latest_tx_end_us) == MESH_SIM_OK);
    CHECK(world.roles[origin].tx_queue_count == 1u);
    CHECK(world.roles[relay_1].tx_queue_count == 0u);
    CHECK(world.roles[relay_2].tx_queue_count == 0u);

    params = route_connection_params(
        mesh_sim_time_ms(world.now_us) + 500u, 250u);
    params.max_missed_events = 20u;
    CHECK(mesh_sim_add_connection_over_radio(&world,
                                             origin,
                                             relay_1,
                                             &params,
                                             true,
                                             &connection_origin_relay) ==
          MESH_SIM_OK);
    CHECK(run_scheduled_connection_repair(&world,
                                          connection_origin_relay) ==
          MESH_SIM_OK);
    params = route_connection_params(
        mesh_sim_time_ms(world.now_us) + 500u, 250u);
    params.max_missed_events = 20u;
    CHECK(mesh_sim_add_connection_over_radio(&world,
                                             relay_1,
                                             relay_2,
                                             &params,
                                             true,
                                             &connection_relay_relay) ==
          MESH_SIM_OK);
    CHECK(run_scheduled_connection_repair(&world,
                                          connection_relay_relay) ==
          MESH_SIM_OK);

    set_test_phase("unseeded_click_origin_to_relay");
    ret = run_connection_until_message(&world,
                                       connection_origin_relay,
                                       origin,
                                       world.roles[relay_1].id,
                                       click_packet.msg_type,
                                       NULL);
    CHECK(ret == MESH_SIM_OK);
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(queue_has_click(&world.roles[relay_1],
                          &click_packet,
                          click_payload,
                          click_payload_len,
                          world.roles[relay_2].id));
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 0u);
    CHECK(run_connection_until_message(
              &world,
              connection_origin_relay,
              relay_1,
              world.roles[origin].id,
              MSG_MESH_HOP_ACK,
              &phase_tx) == MESH_SIM_OK);
    CHECK(hop_ack_matches(&world.transmissions[phase_tx].outbound,
                          world.roles[relay_1].id,
                          world.roles[origin].id,
                          &click_packet,
                          click_payload,
                          click_payload_len));
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[origin].tx_queue_count == 0u);

    set_test_phase("unseeded_click_relay_to_relay");
    CHECK(run_connection_until_message(&world,
                                       connection_relay_relay,
                                       relay_1,
                                       world.roles[relay_2].id,
                                       click_packet.msg_type,
                                       NULL) == MESH_SIM_OK);
    CHECK(world.roles[relay_1].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(world.roles[relay_1].relay.outbox_record.valid);
    CHECK(queue_has_click(&world.roles[relay_2],
                          &click_packet,
                          click_payload,
                          click_payload_len,
                          GATEWAY_ID));
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_1].id) == 0u);
    CHECK(run_connection_until_message(
              &world,
              connection_relay_relay,
              relay_2,
              world.roles[relay_1].id,
              MSG_MESH_HOP_ACK,
              &phase_tx) == MESH_SIM_OK);
    CHECK(hop_ack_matches(&world.transmissions[phase_tx].outbound,
                          world.roles[relay_2].id,
                          world.roles[relay_1].id,
                          &click_packet,
                          click_payload,
                          click_payload_len));
    CHECK(world.roles[relay_1].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[relay_1].tx_queue_count == 0u);

    set_test_phase("unseeded_click_direct_gateway_ack");
    CHECK(run_direct_click_turn(&world, relay_2, gateway) == MESH_SIM_OK);
    CHECK(world.roles[gateway].delivery_count == gateway_delivery_before + 1u);
    {
        const struct mesh_sim_delivery *delivery =
            &world.roles[gateway]
                 .deliveries[world.roles[gateway].delivery_count - 1u];

        CHECK(delivery->packet.msg_type == click_packet.msg_type);
        CHECK(delivery->packet.src_id == click_packet.src_id);
        CHECK(delivery->packet.dst_id == click_packet.dst_id);
        CHECK(delivery->packet.session_id == click_packet.session_id);
        CHECK(delivery->packet.seq == click_packet.seq);
        CHECK(delivery->payload_len == click_payload_len);
        CHECK(memcmp(delivery->payload,
                     click_payload,
                     click_payload_len) == 0);
    }
    CHECK(world.roles[relay_2].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_2].id) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_1].id) == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 0u);
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[relay_1].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[relay_2].relay.pending.gateway_ack_confirm_pending);
    CHECK(pending_gateway_ack_confirm_packet(&world,
                                             relay_2,
                                             &confirm_packet) == PROTO_OK);
    CHECK(confirm_packet.src_id == click_packet.src_id);
    CHECK(confirm_packet.dst_id == click_packet.dst_id);
    CHECK(confirm_packet.session_id == click_packet.session_id);
    CHECK(confirm_packet.seq == click_packet.seq);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_1].id) == 1u);
    CHECK(world.roles[gateway].gateway_semantic_commit_count == 1u);
    CHECK(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);

    set_test_phase("unseeded_click_adjacent_relay_confirm");
    CHECK(run_direct_click_turn(&world, relay_2, gateway) == MESH_SIM_OK);
    CHECK(world.roles[gateway].delivery_count == gateway_delivery_before + 1u);
    CHECK(world.roles[relay_2].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[relay_1].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(world.roles[origin].tx_queue_count == 0u);
    CHECK(world.roles[relay_1].tx_queue_count == 0u);
    CHECK(world.roles[relay_2].tx_queue_count == 0u);
    CHECK(world.roles[gateway].tx_queue_count == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_1].id) == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[relay_2].id) == 1u);
    CHECK(world.roles[gateway].gateway_semantic_commit_count == 1u);
    CHECK(world.roles[gateway].gateway_semantic_duplicate_ack_count == 0u);

    {
        size_t click_transmissions = 0u;
        size_t confirm_transmissions = 0u;

        for (size_t i = 0u; i < world.transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world.transmissions[i];

            if (tx->has_outbound &&
                click_identity_matches(&tx->outbound,
                                        &click_packet,
                                        click_payload,
                                        click_payload_len)) {
                click_transmissions++;
            }
            if (tx->has_outbound &&
                tx->outbound.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM &&
                tx->outbound.packet.src_id == click_packet.src_id &&
                tx->outbound.packet.session_id == click_packet.session_id &&
                tx->outbound.packet.seq == click_packet.seq) {
                confirm_transmissions++;
            }
        }
        CHECK(click_transmissions == 3u);
        CHECK(confirm_transmissions == 1u);
    }
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                     world.roles[origin].id) >= 2u);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int run_reset_after_established_events_case(uint32_t seed)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params;
    struct mesh_sim_connection_action action;
    uint8_t anchor;
    uint8_t peer;
    uint8_t gateway;
    uint16_t connection;
    uint32_t events_before_reset;
    uint32_t repairs_before_reset;
    uint32_t work_epoch_before;
    uint32_t peer_session_before_reset;
    uint64_t peer_expiry_us;
    uint8_t expired_endpoints = 0u;
    struct mesh_event_timing peer_timing_before_reset;

    set_test_phase("reset_after_established_setup");
    mesh_sim_init(&world, seed);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID + UINT64_C(0x30), GATEWAY_ID,
                            ROUTE_EPOCH, &anchor) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_1_ID + UINT64_C(0x30), GATEWAY_ID,
                            ROUTE_EPOCH, &peer) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, anchor, peer, 98u, 6u) == MESH_SIM_OK);
    params = route_connection_params(500u, 250u);
    params.max_missed_events = 20u;
    CHECK(mesh_sim_add_connection_over_radio(&world,
                                             anchor,
                                             peer,
                                             &params,
                                             true,
                                             &connection) == MESH_SIM_OK);
    CHECK(run_scheduled_connection_repair(&world, connection) == MESH_SIM_OK);
    CHECK(world.connections[connection].completed_repairs == 1u);

    set_test_phase("reset_after_established_completed_events");
    for (uint8_t i = 0u; i < 6u; i++) {
        CHECK(run_connection_event(&world, connection) == MESH_SIM_OK);
    }
    events_before_reset = world.connections[connection].completed_events;
    repairs_before_reset = world.connections[connection].completed_repairs;
    work_epoch_before = world.roles[anchor].work_epoch;
    CHECK(events_before_reset >= 6u);
    CHECK(world.connections[connection].timing_a.timing_fresh);
    CHECK(world.connections[connection].timing_b.timing_fresh);
    peer_session_before_reset = world.connections[connection].owner_b.session_id;
    peer_timing_before_reset = world.connections[connection].timing_b;

    set_test_phase("reset_after_established_reset");
    CHECK(mesh_sim_reset_role(&world, anchor) == MESH_SIM_OK);
    CHECK(world.roles[anchor].work_epoch == work_epoch_before + 1u);
    CHECK(world.connections[connection].completed_events == events_before_reset);
    CHECK(world.connections[connection].completed_repairs == repairs_before_reset);
    CHECK(!world.connections[connection].repair_pending);
    CHECK(!world.connections[connection].owner_a.active);
    CHECK(world.connections[connection].owner_a.terminal);
    CHECK(world.connections[connection].owner_b.active);
    CHECK(world.connections[connection].owner_b.session_id ==
          peer_session_before_reset);
    CHECK(!mesh_event_timing_usable(&world.connections[connection].timing_a,
                                    mesh_sim_time_ms(world.now_us)));
    CHECK(mesh_event_timing_usable(&world.connections[connection].timing_b,
                                   mesh_sim_time_ms(world.now_us)));
    CHECK(world.connections[connection].timing_b.last_successful_ch9_event_ms ==
          peer_timing_before_reset.last_successful_ch9_event_ms);
    CHECK(world.connections[connection].timing_b.next_event_time_ms ==
          peer_timing_before_reset.next_event_time_ms);
    CHECK(world.roles[anchor].event_control_seq == 0u);

    set_test_phase("reset_after_established_peer_supervision_expiry");
    peer_expiry_us =
        ((uint64_t)peer_timing_before_reset.last_successful_ch9_event_ms +
         peer_timing_before_reset.supervision_timeout_ms + 1u) * 1000u;
    if (peer_expiry_us > world.now_us) {
        CHECK(mesh_sim_run_until(&world, peer_expiry_us) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_expire_connection_ownership(
              &world, connection, &expired_endpoints) == MESH_SIM_OK);
    CHECK(expired_endpoints == 1u);
    CHECK(!world.connections[connection].owner_b.active);

    set_test_phase("reset_after_established_repair");
    CHECK(mesh_sim_renegotiate_connection_over_radio(&world,
                                                     connection,
                                                     anchor) == MESH_SIM_OK);
    CHECK(run_scheduled_connection_repair(&world, connection) == MESH_SIM_OK);
    CHECK(world.connections[connection].completed_repairs ==
          repairs_before_reset + 1u);
    CHECK(world.connections[connection].repair_seq == 1u);
    CHECK(world.connections[connection].repair_propose_decoded);
    CHECK(world.connections[connection].repair_accept_decoded);
    CHECK(world.connections[connection].timing_a.timing_fresh);
    CHECK(world.connections[connection].timing_b.timing_fresh);

    set_test_phase("reset_after_established_post_recovery_events");
    for (uint8_t i = 0u; i < 4u; i++) {
        CHECK(mesh_sim_connection_next_action(&world, connection, &action) ==
              MESH_SIM_OK);
        CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT);
        CHECK(run_connection_event(&world, connection) == MESH_SIM_OK);
    }
    CHECK(world.connections[connection].completed_events >=
          events_before_reset + 4u);
    CHECK(world.roles[anchor].watchdog.resets == 1u);
    CHECK(world.roles[peer].watchdog.resets == 0u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_WATCHDOG_RESET,
                                     world.roles[anchor].id) == 1u);
    CHECK(world.last_error == MESH_SIM_OK);
    (void)gateway;
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
    struct route_reception reply_at_relay_1_retry;
    struct route_reception reply_at_origin;
    struct route_reception reply_at_origin_retry;
    struct discovery_identity request_identity;
    struct discovery_identity initial_reply_identity;
    struct discovery_identity forwarded_reply_identity;
    struct route_candidate retained_candidate;
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
    uint16_t initial_reply_retry_tx;
    uint16_t downstream_reply_ack_tx;
    uint16_t route_reply_ack_tx;
    uint16_t forwarded_reply_tx;
    uint16_t forwarded_reply_retry_tx;
    uint64_t next_start_us;
    size_t reception_count_before;
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

    set_test_phase("truncated_first_attempt");
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

    set_test_phase("retry_request_to_first_relay");
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

    set_test_phase("request_to_second_relay");
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

    /*
     * The reverse request state already exists, so filling relay 1's bounded
     * upstream table now must not prevent this valid reply from continuing to
     * the origin.  All retained candidates are deliberately better than the
     * reply learned through relay 2, forcing route_upsert_candidate() to
     * return PROTO_ERR_NO_SPACE at the transit relay.
     */
    set_test_phase("reply_transit_candidate_table_full");
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        retained_candidate = (struct route_candidate) {
            .next_hop_id = RETAINED_PARENT_BASE_ID + i,
            .gateway_id = GATEWAY_ID,
            .route_epoch = ROUTE_EPOCH,
            .last_seen_ms = mesh_sim_time_ms(world.now_us),
            .hop_count = 1u,
            .link_quality = (uint8_t)(100u - i),
            .valid = true,
        };
        CHECK(route_upsert_candidate(&world.roles[relay_1].relay.upstream,
                                     &retained_candidate) == PROTO_OK);
    }

    set_test_phase("reply_to_first_relay");
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     initial_reply_tx,
                                     relay_1,
                                     true,
                                     &reply_at_relay_1) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    forwarded_reply_tx = (uint16_t)transmission_count_before;
    CHECK(reply_at_relay_1.radio.packet.msg_type == MSG_ROUTE_REPLY);
    CHECK(world.transmissions[forwarded_reply_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY);
    CHECK(world.transmissions[forwarded_reply_tx].outbound.next_hop_id ==
          ORIGIN_ID);
    CHECK(world.roles[relay_1].route_reply_upstream_ack_valid);
    CHECK(world.roles[relay_1].route_reply_upstream_ack.next_hop_id ==
          RELAY_2_ID);
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

    set_test_phase("reply_to_origin_downstream_ack_timeout");
    {
        const struct mesh_sim_contact_response_timing short_ack_window = {
            .rx_delay_us = 900u,
            .rx_window_us = 200u,
            .tx_delay_us = MESH_SIM_C5_RESPONSE_TX_DELAY_US,
        };

        CHECK(mesh_sim_override_next_contact_response_timing(
                  &world, &short_ack_window) == MESH_SIM_OK);
    }
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(&world,
                                     forwarded_reply_tx,
                                     origin,
                                     true,
                                     &reply_at_origin) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    downstream_reply_ack_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[downstream_reply_ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    reception_count_before = world.reception_count;
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world,
                                         downstream_reply_ack_tx)) ==
          MESH_SIM_OK);
    CHECK(world.reception_count == reception_count_before + 1u);
    CHECK(world.receptions[reception_count_before].outcome !=
          MESH_SIM_RX_DECODED);
    CHECK(world.roles[relay_1].route_reply_upstream_ack_valid);
    CHECK(world.transmission_count == transmission_count_before + 1u);

    set_test_phase("reply_to_origin_retry_releases_chained_custody");
    transmission_count_before = world.transmission_count;
    mesh_relay_note_route_reply_retry(&world.roles[relay_2].relay);
    CHECK(transmit_route_outbound(
              &world,
              relay_2,
              relay_1,
              &world.transmissions[initial_reply_tx].outbound,
              world.now_us + BETWEEN_FRAMES_US,
              true,
              &reply_at_relay_1_retry,
              &initial_reply_retry_tx) == MESH_SIM_OK);
    CHECK(initial_reply_retry_tx == transmission_count_before);
    CHECK(reply_at_relay_1_retry.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(world.transmission_count == transmission_count_before + 2u);
    forwarded_reply_retry_tx =
        (uint16_t)(transmission_count_before + 1u);
    CHECK(world.transmissions[forwarded_reply_retry_tx].outbound.packet
              .msg_type == MSG_ROUTE_REPLY);
    CHECK(world.transmissions[forwarded_reply_retry_tx].outbound.next_hop_id ==
          ORIGIN_ID);
    CHECK(world.roles[relay_1].route_reply_upstream_ack_valid);
    transmission_count_before = world.transmission_count;
    CHECK(receive_scheduled_outbound(
              &world,
              forwarded_reply_retry_tx,
              origin,
              true,
              &reply_at_origin_retry) == MESH_SIM_OK);
    CHECK(reply_at_origin_retry.radio.outcome == MESH_SIM_RX_DECODED);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    downstream_reply_ack_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[downstream_reply_ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world,
                                         downstream_reply_ack_tx)) ==
          MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    route_reply_ack_tx = (uint16_t)transmission_count_before;
    CHECK(world.transmissions[route_reply_ack_tx].outbound.packet.msg_type ==
          MSG_ROUTE_REPLY_ACK);
    CHECK(world.transmissions[route_reply_ack_tx].outbound.next_hop_id ==
          RELAY_2_ID);
    CHECK(!world.roles[relay_1].route_reply_upstream_ack_valid);
    transmission_count_before = world.transmission_count;
    CHECK(mesh_sim_run_until(
              &world,
              transmission_evaluation_us(&world, route_reply_ack_tx)) ==
          MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before);
    CHECK(capture_discovery_identity(&reply_at_origin.radio,
                                     true,
                                     &forwarded_reply_identity) == PROTO_OK);
    CHECK(same_discovery(&initial_reply_identity,
                         &forwarded_reply_identity));
    CHECK(!world.roles[origin].relay.route_discovery.active);

    set_test_phase("usable_forward_and_reverse_routes");
    CHECK(assert_selected_hop(&world.roles[origin].relay,
                              GATEWAY_ID, RELAY_1_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_1].relay,
                              GATEWAY_ID, RETAINED_PARENT_BASE_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_1].relay,
                              ORIGIN_ID, ORIGIN_ID) == 0);
    CHECK(assert_selected_hop(&world.roles[relay_2].relay,
                              ORIGIN_ID, RELAY_1_ID) == 0);
    CHECK(route_selected(&world.roles[origin].relay.upstream)->hop_count == 2u);
    CHECK(route_selected(&world.roles[relay_1].relay.upstream)->hop_count == 1u);
    CHECK(route_selected(&world.roles[relay_2].relay.upstream)->hop_count == 0u);

    set_test_phase("bounded_timing");
    completed_ms = (uint32_t)(reply_at_origin.radio.end_us / 1000u);
    CHECK(completed_ms >= DISCOVERY_START_MS);
    CHECK(completed_ms - DISCOVERY_START_MS <= DISCOVERY_BUDGET_MS);
    CHECK(world.last_error == MESH_SIM_OK);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_PARTIAL,
                                     RELAY_1_ID) == 2u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RX_DECODED,
                                     0u) == 8u);

    if (run_exact_hop_multi_responder_case() != 0 ||
        run_ttl_ladder_data_case() != 0 ||
        run_blank_anchor_retains_local_click_until_route_case() != 0 ||
        run_route_collision_case() != 0 ||
        run_unseeded_report_route_custody_case(false) != 0 ||
        run_unseeded_report_route_custody_case(true) != 0 ||
        run_reset_after_established_events_case(
            SCENARIO_SEED ^ UINT32_C(0x052e7a11)) != 0 ||
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
