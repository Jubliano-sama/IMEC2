#include "gateway_command.h"
#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHILD_A_ID UINT64_C(0xc501000000000001)
#define CHILD_B_ID UINT64_C(0xc501000000000002)
#define COLLECTOR_ID UINT64_C(0xc501000000000010)
#define UPSTREAM_ID UINT64_C(0xc501000000000020)
#define GATEWAY_ID UINT64_C(0xc5010000000000ff)
#define UNUSED_ROSTER_ID UINT64_C(0xc501000000000003)
#define ROUTE_EPOCH UINT32_C(71)
#define COMMAND_SEQ UINT32_C(0xc5010047)
#define COLLECTION_EPOCH UINT32_C(0xc5011047)
#define MEMBERSHIP_EPOCH UINT16_C(47)
#define TURN_GUARD_US UINT64_C(20000)
#define RX_GUARD_US UINT64_C(250)
#define SAME_TURN_ACK_BOUND_US UINT64_C(50000)
#define MAX_IDENTICAL_RESULT_BUNDLE_TX 4u
#define MAX_IDENTICAL_RESULT_BUNDLE_TX_PER_RADIO 4u
#define MAX_IDENTICAL_EACK_TX_PER_RADIO 2u
#define MAX_TOTAL_EACK_TX 9u
#define EXPECTED_LEAF_EACK_FORWARDS 1u

struct fixture {
    struct mesh_sim_world world;
    struct gateway_collection_state collection;
    struct command_result_id child_result_ids[2];
    uint8_t child_a;
    uint8_t child_b;
    uint8_t collector;
    uint8_t upstream;
    uint8_t gateway;
};

static struct fixture fixture;
static const char *phase;
static unsigned int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr,                                                   \
                    "FAIL %s:%d [%s] now=%llu error=%d (%s): %s\n",         \
                    __FILE__, __LINE__, phase,                                \
                    (unsigned long long)fixture.world.now_us,                 \
                    fixture.world.last_error,                                 \
                    fixture.world.last_error_file == NULL ? "none" :        \
                        fixture.world.last_error_file,                        \
                    #expression);                                             \
            failures++;                                                       \
            return false;                                                     \
        }                                                                     \
    } while (0)

static uint64_t role_ready_us(const struct mesh_sim_role_instance *role)
{
    uint64_t ready_us = role->dwm3000.cpu_busy_until_us;

    if (role->dwm3000.spi_busy_until_us > ready_us) {
        ready_us = role->dwm3000.spi_busy_until_us;
    }
    if (role->dwm3000.radio_busy_until_us > ready_us) {
        ready_us = role->dwm3000.radio_busy_until_us;
    }
    return ready_us;
}

static uint64_t safe_turn_start_us(const struct fixture *f,
                                   uint8_t sender,
                                   const uint8_t *receivers,
                                   size_t receiver_count)
{
    uint64_t start_us = f->world.now_us;
    uint64_t ready_us = role_ready_us(&f->world.roles[sender]);

    if (ready_us > start_us) {
        start_us = ready_us;
    }
    for (size_t i = 0u; i < receiver_count; i++) {
        ready_us = role_ready_us(&f->world.roles[receivers[i]]);
        if (ready_us > start_us) {
            start_us = ready_us;
        }
    }
    return start_us + TURN_GUARD_US;
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
    return tx->end_us + maximum_propagation_us + RX_GUARD_US;
}

static int transmit_outbound(struct fixture *f,
                             uint8_t sender,
                             const uint8_t *receivers,
                             size_t receiver_count,
                             struct mesh_outbound *outbound,
                             uint16_t *transmission_index)
{
    enum mesh_sim_phy phy;
    uint64_t start_us;
    uint64_t rx_end_us;
    uint32_t duration_us;
    uint16_t tx_index;
    uint8_t channel;
    int ret;

    if (outbound == NULL || (receivers == NULL && receiver_count != 0u)) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_sim_outbound_radio(outbound, &channel, &phy);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "transmit_outbound radio mapping failed: %d\n", ret);
        return ret;
    }
    duration_us = mesh_sim_frame_duration_us(
        phy, proto_packet_encoded_len(outbound->payload_len));
    if (duration_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }
    start_us = safe_turn_start_us(f, sender, receivers, receiver_count);
    if (start_us < (uint64_t)outbound->earliest_tx_ms * 1000u) {
        start_us = (uint64_t)outbound->earliest_tx_ms * 1000u;
    }
    rx_end_us = start_us + duration_us + RX_GUARD_US;
    for (size_t i = 0u; i < receiver_count; i++) {
        uint64_t receiver_end_us = start_us + duration_us +
            f->world.propagation_us[sender][receivers[i]] + RX_GUARD_US;

        ret = mesh_sim_schedule_rx(&f->world,
                                   receivers[i],
                                   start_us - RX_GUARD_US,
                                   receiver_end_us,
                                   channel,
                                   phy,
                                   NULL);
        if (ret != MESH_SIM_OK) {
            fprintf(stderr, "transmit_outbound RX scheduling failed: %d\n", ret);
            return ret;
        }
        if (receiver_end_us > rx_end_us) {
            rx_end_us = receiver_end_us;
        }
    }
    ret = mesh_sim_schedule_outbound_tx(&f->world,
                                        sender,
                                        start_us,
                                        outbound,
                                        &tx_index);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "transmit_outbound TX scheduling failed: %d\n", ret);
        return ret;
    }
    ret = mesh_sim_run_until(&f->world, rx_end_us);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "transmit_outbound run failed: %d\n", ret);
        return ret;
    }
    if (transmission_index != NULL) {
        *transmission_index = tx_index;
    }
    return MESH_SIM_OK;
}

static int take_queued_outbound(struct mesh_sim_role_instance *node,
                                uint8_t msg_type,
                                uint64_t next_hop_id,
                                bool require_relay_start,
                                struct mesh_outbound *outbound)
{
    size_t selected = SIZE_MAX;

    if (node == NULL || outbound == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &node->tx_queue[i];

        if (!candidate->valid ||
            candidate->outbound.packet.msg_type != msg_type ||
            candidate->outbound.next_hop_id != next_hop_id ||
            candidate->needs_relay_start != require_relay_start) {
            continue;
        }
        if (selected == SIZE_MAX ||
            candidate->enqueue_order < node->tx_queue[selected].enqueue_order) {
            selected = i;
        }
    }
    if (selected == SIZE_MAX) {
        return MESH_SIM_ERR_ROUTE_REQUIRED;
    }
    *outbound = node->tx_queue[selected].outbound;
    memset(&node->tx_queue[selected], 0, sizeof(node->tx_queue[selected]));
    if (node->tx_queue_count == 0u) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    node->tx_queue_count--;
    return MESH_SIM_OK;
}

static size_t queued_type_count(const struct mesh_sim_role_instance *node,
                                uint8_t msg_type)
{
    size_t count = 0u;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.packet.msg_type == msg_type) {
            count++;
        }
    }
    return count;
}

static bool setup_topology(struct fixture *f)
{
    const uint64_t roster[] = {
        CHILD_A_ID,
        CHILD_B_ID,
        UNUSED_ROSTER_ID,
    };

    mesh_sim_init(&f->world, UINT32_C(0xc5010047));
    CHECK(mesh_sim_add_role(&f->world, MESH_SIM_ROLE_ANCHOR,
                            CHILD_A_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &f->child_a) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&f->world, MESH_SIM_ROLE_ANCHOR,
                            CHILD_B_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &f->child_b) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&f->world, MESH_SIM_ROLE_ANCHOR,
                            COLLECTOR_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &f->collector) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&f->world, MESH_SIM_ROLE_ANCHOR,
                            UPSTREAM_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &f->upstream) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&f->world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &f->gateway) == MESH_SIM_OK);

    CHECK(mesh_sim_set_link(&f->world, f->child_a, f->collector, 95u, 2u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&f->world, f->child_b, f->collector, 94u, 2u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&f->world, f->collector, f->gateway, 97u, 1u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&f->world, f->collector, f->upstream, 96u, 2u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&f->world, f->upstream, f->gateway, 98u, 1u) ==
          MESH_SIM_OK);

    CHECK(mesh_sim_install_route(&f->world, f->child_a, f->collector, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&f->world, f->child_b, f->collector, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&f->world, f->collector, f->gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&f->world, f->upstream, f->gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);

    gateway_collection_clear(&f->collection);
    CHECK(gateway_collection_start(&f->collection,
                                   GATEWAY_ID,
                                   (uint16_t)ROUTE_EPOCH,
                                   COMMAND_SEQ,
                                   COLLECTION_EPOCH,
                                   MEMBERSHIP_EPOCH,
                                   3u,
                                   0u,
                                   COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    CHECK(gateway_collection_set_expected_roster(&f->collection,
                                                  roster,
                                                  3u) == PROTO_OK);
    return true;
}

static bool build_child_result(struct fixture *f,
                               uint8_t child_number,
                               struct proto_packet *packet,
                               uint8_t *payload,
                               size_t payload_capacity,
                               size_t *payload_len)
{
    static const uint8_t padding[24] = {
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
    };
    const uint64_t child_id = child_number == 0u ? CHILD_A_ID : CHILD_B_ID;
    struct command_result_id *result_id = &f->child_result_ids[child_number];
    int ret;

    *result_id = (struct command_result_id) {
        .gateway_id = GATEWAY_ID,
        .gateway_epoch = (uint16_t)ROUTE_EPOCH,
        .command_seq = COMMAND_SEQ,
        .node_id = child_id,
        .node_boot_counter = UINT32_C(0x4700) + child_number,
        .result_seq = (uint16_t)child_number + 1u,
    };
    *payload_len = 0u;
    ret = command_result_id_append_tlvs(payload,
                                        payload_capacity,
                                        payload_len,
                                        result_id);
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(payload,
                             payload_capacity,
                             payload_len,
                             TLV_COLLECTION_EPOCH_ID,
                             COLLECTION_EPOCH);
    }
    if (ret == PROTO_OK) {
        ret = mesh_append_command_result(payload,
                                         payload_capacity,
                                         payload_len,
                                         CMD_GET_STATUS,
                                         COMMAND_OK,
                                         0u);
    }
    while (ret == PROTO_OK && *payload_len < 64u) {
        size_t remaining = 64u - *payload_len;
        uint8_t chunk_len = remaining > sizeof(padding) + 2u ?
                            (uint8_t)sizeof(padding) :
                            (uint8_t)(remaining - 2u);

        ret = tlv_append_bytes(payload,
                               payload_capacity,
                               payload_len,
                               TLV_MESH_TEST_PADDING,
                               padding,
                               chunk_len);
    }
    if (ret != PROTO_OK) {
        return false;
    }
    CHECK(*payload_len == 64u);
    CHECK(mesh_init_command_result(packet,
                                   child_id,
                                   GATEWAY_ID,
                                   COMMAND_SEQ,
                                   result_id->result_seq,
                                   (uint8_t)*payload_len,
                                   false) == PROTO_OK);
    return true;
}

static bool send_offer_and_result(struct fixture *f,
                                  uint8_t child_number,
                                  uint8_t child_index)
{
    struct mesh_sim_role_instance *child = &f->world.roles[child_index];
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct proto_packet result_packet;
    struct mesh_outbound offer;
    struct mesh_outbound result_tx;
    struct mesh_outbound hop_ack;
    uint8_t payload[96];
    size_t payload_len;
    uint8_t receiver = f->collector;
    uint8_t child_receiver = child_index;
    uint16_t request_tx_index;
    size_t transmission_count_before;
    uint16_t grant_tx_index = UINT16_MAX;

    CHECK(build_child_result(f, child_number, &result_packet, payload,
                             sizeof(payload), &payload_len));
    CHECK(mesh_relay_start_result_offer(&child->relay,
                                        &result_packet,
                                        payload,
                                        payload_len,
                                        (uint32_t)(f->world.now_us / 1000u),
                                        &offer) == PROTO_OK);

    transmission_count_before = f->world.transmission_count;
    CHECK(transmit_outbound(f, child_index, &receiver, 1u, &offer,
                            &request_tx_index) == MESH_SIM_OK);
    for (size_t i = transmission_count_before;
         i < f->world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &f->world.transmissions[i];

        if (tx->has_outbound && tx->node_index == f->collector &&
            tx->outbound.packet.msg_type == MSG_RESULT_GRANT &&
            tx->outbound.next_hop_id == child->id) {
            grant_tx_index = (uint16_t)i;
            break;
        }
    }
    CHECK(request_tx_index < f->world.transmission_count);
    CHECK(grant_tx_index != UINT16_MAX);
    CHECK(mesh_sim_run_until(&f->world,
                             transmission_evaluation_us(&f->world,
                                                        grant_tx_index)) ==
          MESH_SIM_OK);
    CHECK(child->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(!child->relay.pending.result_offer_active);
    CHECK(collector->relay.result_offer_reservation.valid);
    CHECK(take_queued_outbound(child,
                               MSG_COMMAND_RESULT,
                               COLLECTOR_ID,
                               false,
                               &result_tx) == MESH_SIM_OK);
    result_tx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(transmit_outbound(f, child_index, &receiver, 1u, &result_tx, NULL) ==
          MESH_SIM_OK);
    CHECK(!collector->relay.result_offer_reservation.valid);
    CHECK(take_queued_outbound(collector,
                               MSG_MESH_HOP_ACK,
                               child->id,
                               false,
                               &hop_ack) == MESH_SIM_OK);
    hop_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(transmit_outbound(f, f->collector, &child_receiver, 1u, &hop_ack,
                            NULL) == MESH_SIM_OK);
    CHECK(child->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(child->relay.outbox_record.delivery_state ==
          MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    return true;
}

static bool run_queue_pressure_gate(void)
{
    struct proto_packet first = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = CHILD_A_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0xc5010001),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    struct proto_packet second = first;

    phase = "explicit queue-pressure admission";
    CHECK(setup_topology(&fixture));
    CHECK(mesh_sim_set_tx_queue_capacity(&fixture.world,
                                         fixture.child_a,
                                         1u) == MESH_SIM_OK);
    CHECK(mesh_sim_queue_originated(&fixture.world,
                                    fixture.child_a,
                                    &first,
                                    NULL,
                                    0u) == MESH_SIM_OK);
    second.seq = 2u;
    CHECK(mesh_sim_queue_originated(&fixture.world,
                                    fixture.child_a,
                                    &second,
                                    NULL,
                                    0u) == MESH_SIM_ERR_CAPACITY);
    CHECK(fixture.world.last_error == MESH_SIM_OK);
    CHECK(fixture.world.roles[fixture.child_a].tx_queue_count == 1u);
    CHECK(mesh_sim_reset_role(&fixture.world, fixture.child_a) == MESH_SIM_OK);
    CHECK(fixture.world.roles[fixture.child_a].tx_queue_count == 0u);
    return true;
}

static bool start_bundle_custody(struct fixture *f,
                                 struct mesh_outbound *bundle)
{
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct mesh_relay_child_custody_snapshot child_snapshot;

    CHECK(collector->relay.result_bundle.active);
    CHECK(collector->relay.result_bundle.record_count == 2u);
    CHECK(mesh_relay_export_child_custody_snapshot(
              &collector->relay,
              (uint32_t)(f->world.now_us / 1000u),
              &child_snapshot) == PROTO_OK);
    CHECK(child_snapshot.result_bundle.record_count == 2u);
    CHECK(take_queued_outbound(collector,
                               MSG_RESULT_BUNDLE,
                               GATEWAY_ID,
                               true,
                               bundle) == MESH_SIM_OK);
    mesh_relay_result_bundle_note_forwarded(&collector->relay, bundle);
    CHECK(!mesh_relay_result_bundle_pending(&collector->relay));
    CHECK(mesh_relay_export_child_custody_snapshot(
              &collector->relay,
              (uint32_t)(f->world.now_us / 1000u),
              &child_snapshot) == PROTO_ERR_NOT_FOUND);
    return true;
}

static bool reject_mutated_bundle_then_restore(
    struct fixture *f,
    const struct mesh_outbound *queued_bundle)
{
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct mesh_sim_role_instance *gateway = &f->world.roles[f->gateway];
    struct mesh_relay_outbox_snapshot snapshot;
    struct mesh_outbound bundle_tx;
    struct mesh_outbound mutated;
    uint8_t gateway_receiver = f->gateway;
    uint64_t retry_at_us;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    CHECK(mesh_relay_start_tx(&collector->relay,
                              &queued_bundle->packet,
                              queued_bundle->payload,
                              queued_bundle->payload_len,
                              (uint32_t)(f->world.now_us / 1000u),
                              &bundle_tx) == PROTO_OK);
    bundle_tx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    mutated = bundle_tx;
    CHECK(mutated.payload_len > 0u);
    mutated.payload[mutated.payload_len - 1u] ^= 0x01u;
    CHECK(transmit_outbound(f, f->collector, &gateway_receiver, 1u, &mutated,
                            NULL) == MESH_SIM_OK);
    CHECK(gateway->delivery_count == 0u);
    CHECK(gateway->gateway_semantic_rejection_count == 0u);
    CHECK(gateway_collection_record_bundle_from_hop(
              &f->collection,
              &mutated.packet,
              mutated.payload,
              mutated.payload_len,
              COLLECTOR_ID,
              &accepted_count,
              &duplicate_count) == PROTO_ERR_BAD_CRC);
    CHECK(accepted_count == 0u);
    CHECK(duplicate_count == 0u);
    CHECK(f->collection.received_count == 0u);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(collector->relay.outbox_record.valid);
    CHECK(memcmp(collector->relay.pending.payload,
                 queued_bundle->payload,
                 queued_bundle->payload_len) == 0);
    CHECK(mesh_relay_export_outbox_snapshot(
              &collector->relay,
              (uint32_t)(f->world.now_us / 1000u),
              &snapshot) == PROTO_OK);
    CHECK(mesh_sim_reset_role(&f->world, f->collector) == MESH_SIM_OK);
    CHECK(mesh_relay_restore_outbox_snapshot(
              &collector->relay,
              &snapshot,
              (uint32_t)(f->world.now_us / 1000u)) == PROTO_OK);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_at_us = (uint64_t)collector->relay.pending.retry_after_ms * 1000u;
    CHECK(mesh_sim_schedule_relay_tick(&f->world, f->collector, retry_at_us) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&f->world, retry_at_us) == MESH_SIM_OK);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(queued_type_count(collector, MSG_RESULT_BUNDLE) == 1u);
    CHECK(queued_type_count(gateway, MSG_GATEWAY_ACK) == 0u);
    return true;
}

static bool gateway_accepts_exact_retry(
    struct fixture *f,
    const struct mesh_outbound *original_bundle,
    struct mesh_outbound *accepted_bundle,
    uint16_t *accepted_bundle_tx_index)
{
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct mesh_sim_role_instance *gateway = &f->world.roles[f->gateway];
    struct mesh_outbound retry;
    struct mesh_outbound conflict;
    uint8_t gateway_receiver = f->gateway;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    CHECK(take_queued_outbound(collector,
                               MSG_RESULT_BUNDLE,
                               GATEWAY_ID,
                               false,
                               &retry) == MESH_SIM_OK);
    retry.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(retry.payload_len == original_bundle->payload_len);
    CHECK(memcmp(retry.payload,
                 original_bundle->payload,
                 retry.payload_len) == 0);
    CHECK(transmit_outbound(f, f->collector, &gateway_receiver, 1u, &retry,
                            accepted_bundle_tx_index) == MESH_SIM_OK);
    CHECK(gateway->delivery_count == 1u);
    CHECK(gateway->gateway_semantic_commit_count == 1u);
    CHECK(gateway->gateway_semantic_duplicate_ack_count == 0u);
    CHECK(gateway_collection_record_bundle_from_hop(
              &f->collection,
              &gateway->deliveries[0].packet,
              gateway->deliveries[0].payload,
              gateway->deliveries[0].payload_len,
              COLLECTOR_ID,
              &accepted_count,
              &duplicate_count) == PROTO_OK);
    CHECK(accepted_count == 2u);
    CHECK(duplicate_count == 0u);
    CHECK(f->collection.received_count == 2u);
    CHECK(f->collection.collection_open);
    conflict = retry;
    conflict.payload[conflict.payload_len - 1u] ^= 0x01u;
    CHECK(transmit_outbound(f, f->collector, &gateway_receiver, 1u, &conflict,
                            NULL) == MESH_SIM_OK);
    CHECK(gateway->delivery_count == 1u);
    CHECK(gateway->gateway_semantic_commit_count == 1u);
    /* The immutable transport identity rejects the same-header mutation
     * before the application semantic-admission boundary. */
    CHECK(gateway->gateway_semantic_rejection_count == 0u);
    CHECK(queued_type_count(gateway, MSG_GATEWAY_ACK) == 1u);
    accepted_count = 0u;
    duplicate_count = 0u;
    CHECK(gateway_collection_record_bundle_from_hop(
              &f->collection,
              &conflict.packet,
              conflict.payload,
              conflict.payload_len,
              COLLECTOR_ID,
              &accepted_count,
              &duplicate_count) == PROTO_ERR_BAD_CRC);
    CHECK(accepted_count == 0u);
    CHECK(duplicate_count == 0u);
    CHECK(f->collection.received_count == 2u);
    *accepted_bundle = retry;
    return true;
}

static bool lose_gateway_ack_then_accept_duplicate_retry(
    struct fixture *f,
    const struct mesh_outbound *accepted_bundle,
    uint16_t bundle_tx_index)
{
    struct mesh_sim_role_instance *gateway = &f->world.roles[f->gateway];
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct mesh_outbound gateway_ack;
    struct mesh_outbound retry;
    uint8_t collector_receiver = f->collector;
    uint8_t gateway_receiver = f->gateway;
    uint64_t retry_at_us;
    uint16_t ack_tx_index;
    uint16_t retry_tx_index;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    CHECK(take_queued_outbound(gateway,
                               MSG_GATEWAY_ACK,
                               COLLECTOR_ID,
                               false,
                               &gateway_ack) == MESH_SIM_OK);
    gateway_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(mesh_sim_set_directed_rx_failures(&f->world,
                                            f->gateway,
                                            f->collector,
                                            1u,
                                            MESH_SIM_RX_DECODE_ERROR) ==
          MESH_SIM_OK);
    CHECK(transmit_outbound(f, f->gateway, &collector_receiver, 1u,
                            &gateway_ack, &ack_tx_index) == MESH_SIM_OK);
    CHECK(f->world.transmissions[ack_tx_index].start_us -
              f->world.transmissions[bundle_tx_index].end_us <=
          SAME_TURN_ACK_BOUND_US);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    retry_at_us = (uint64_t)collector->relay.pending.gateway_ack_deadline_ms *
        1000u;
    CHECK(mesh_sim_schedule_relay_tick(&f->world, f->collector, retry_at_us) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&f->world, retry_at_us) == MESH_SIM_OK);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_at_us = (uint64_t)collector->relay.pending.retry_after_ms * 1000u;
    CHECK(mesh_sim_schedule_relay_tick(&f->world, f->collector, retry_at_us) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&f->world, retry_at_us) == MESH_SIM_OK);
    CHECK(take_queued_outbound(collector,
                               MSG_RESULT_BUNDLE,
                               GATEWAY_ID,
                               false,
                               &retry) == MESH_SIM_OK);
    retry.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(retry.payload_len == accepted_bundle->payload_len);
    CHECK(memcmp(retry.payload,
                 accepted_bundle->payload,
                 retry.payload_len) == 0);
    CHECK(retry.packet.msg_type == accepted_bundle->packet.msg_type);
    CHECK(retry.packet.flags == accepted_bundle->packet.flags);
    CHECK(retry.packet.src_id == accepted_bundle->packet.src_id);
    CHECK(retry.packet.dst_id == accepted_bundle->packet.dst_id);
    CHECK(retry.packet.session_id == accepted_bundle->packet.session_id);
    CHECK(retry.packet.seq == accepted_bundle->packet.seq);
    CHECK(retry.packet.ttl == accepted_bundle->packet.ttl);
    CHECK(retry.packet.message_age_ms > accepted_bundle->packet.message_age_ms);
    CHECK(transmit_outbound(f, f->collector, &gateway_receiver, 1u, &retry,
                            &retry_tx_index) == MESH_SIM_OK);
    /* Collection duplicates are deliberately redelivered to the app so its
     * durable semantic store can classify them before another ACK is sent. */
    CHECK(gateway->delivery_count == 1u);
    CHECK(gateway->gateway_semantic_commit_count == 1u);
    CHECK(gateway->gateway_semantic_duplicate_redelivery_count == 1u);
    CHECK(gateway->gateway_semantic_duplicate_ack_count == 0u);
    CHECK(gateway_collection_record_bundle_from_hop(
              &f->collection,
              &retry.packet,
              retry.payload,
              retry.payload_len,
              COLLECTOR_ID,
              &accepted_count,
              &duplicate_count) == PROTO_OK);
    CHECK(accepted_count == 0u);
    CHECK(duplicate_count == 2u);
    CHECK(f->collection.received_count == 2u);
    CHECK(take_queued_outbound(gateway,
                               MSG_GATEWAY_ACK,
                               COLLECTOR_ID,
                               false,
                               &gateway_ack) == MESH_SIM_OK);
    gateway_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(transmit_outbound(f, f->gateway, &collector_receiver, 1u,
                            &gateway_ack, &ack_tx_index) == MESH_SIM_OK);
    CHECK(f->world.transmissions[ack_tx_index].start_us -
              f->world.transmissions[retry_tx_index].end_us <=
          SAME_TURN_ACK_BOUND_US);
    CHECK(collector->relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(!collector->relay.outbox_record.valid);
    return true;
}

static bool build_stale_eack(const struct mesh_outbound *correct,
                             struct mesh_outbound *stale)
{
    struct gateway_collection_eack eack;
    size_t payload_len = 0u;

    CHECK(gateway_collection_eack_from_tlvs(correct->payload,
                                             correct->payload_len,
                                             &eack) == PROTO_OK);
    eack.collection_epoch_id++;
    eack.packet_sequence = (uint16_t)(eack.packet_sequence + 10u);
    *stale = *correct;
    memset(stale->payload, 0, sizeof(stale->payload));
    CHECK(gateway_collection_eack_append_tlvs(stale->payload,
                                               sizeof(stale->payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    CHECK(tlv_append_u64(stale->payload,
                         sizeof(stale->payload),
                         &payload_len,
                         TLV_NODE_ID,
                         CHILD_A_ID) == PROTO_OK);
    CHECK(tlv_append_u64(stale->payload,
                         sizeof(stale->payload),
                         &payload_len,
                         TLV_NODE_ID,
                         CHILD_B_ID) == PROTO_OK);
    stale->packet.seq = eack.packet_sequence;
    stale->packet.ttl = 1u;
    stale->packet.payload_len = (uint16_t)payload_len;
    stale->payload_len = (uint16_t)payload_len;
    stale->earliest_tx_ms = 0u;
    return true;
}

static bool drain_leaf_eack_forwards(struct fixture *f)
{
    const uint8_t children[] = {f->child_a, f->child_b};

    for (size_t i = 0u; i < sizeof(children); i++) {
        struct mesh_sim_role_instance *child = &f->world.roles[children[i]];
        CHECK(queued_type_count(child, MSG_GATEWAY_COLLECTION_EACK) ==
              EXPECTED_LEAF_EACK_FORWARDS);
        for (size_t forward_index = 0u;
             forward_index < EXPECTED_LEAF_EACK_FORWARDS;
             forward_index++) {
            struct mesh_outbound forward;

            CHECK(take_queued_outbound(child,
                                       MSG_GATEWAY_COLLECTION_EACK,
                                       MESH_BROADCAST_ID,
                                       true,
                                       &forward) == MESH_SIM_OK);
            CHECK(transmit_outbound(f, children[i], NULL, 0u, &forward,
                                    NULL) == MESH_SIM_OK);
        }
        CHECK(queued_type_count(child, MSG_GATEWAY_COLLECTION_EACK) == 0u);
    }
    return true;
}

static bool deliver_eack_with_loss_stale_and_duplicate(struct fixture *f)
{
    struct mesh_sim_role_instance *upstream = &f->world.roles[f->upstream];
    struct mesh_sim_role_instance *collector = &f->world.roles[f->collector];
    struct mesh_outbound eack;
    struct mesh_outbound upstream_forward;
    struct mesh_outbound collector_forward;
    struct mesh_outbound stale;
    struct mesh_outbound malformed;
    const uint8_t upstream_receiver = f->upstream;
    const uint8_t collector_receiver = f->collector;
    const uint8_t children[] = {f->child_a, f->child_b};

    CHECK(gateway_collection_prepare_eack_outbound(
              &f->collection,
              EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
              &eack) == PROTO_OK);
    CHECK(mesh_sim_set_directed_rx_failures(&f->world,
                                            f->gateway,
                                            f->upstream,
                                            1u,
                                            MESH_SIM_RX_DECODE_ERROR) ==
          MESH_SIM_OK);
    CHECK(transmit_outbound(f, f->gateway, &upstream_receiver, 1u, &eack,
                            NULL) == MESH_SIM_OK);
    CHECK(queued_type_count(upstream, MSG_GATEWAY_COLLECTION_EACK) == 0u);
    CHECK(transmit_outbound(f, f->gateway, &upstream_receiver, 1u, &eack,
                            NULL) == MESH_SIM_OK);
    CHECK(take_queued_outbound(upstream,
                               MSG_GATEWAY_COLLECTION_EACK,
                               MESH_BROADCAST_ID,
                               true,
                               &upstream_forward) == MESH_SIM_OK);
    CHECK(transmit_outbound(f, f->upstream, &collector_receiver, 1u,
                            &upstream_forward, NULL) == MESH_SIM_OK);
    CHECK(take_queued_outbound(collector,
                               MSG_GATEWAY_COLLECTION_EACK,
                               MESH_BROADCAST_ID,
                               true,
                               &collector_forward) == MESH_SIM_OK);

    CHECK(build_stale_eack(&collector_forward, &stale));
    CHECK(transmit_outbound(f, f->collector, children, 2u, &stale, NULL) ==
          MESH_SIM_OK);
    CHECK(f->world.roles[f->child_a].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(f->world.roles[f->child_b].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    malformed = stale;
    malformed.packet.seq++;
    CHECK(transmit_outbound(f, f->collector, children, 2u, &malformed, NULL) ==
          MESH_SIM_OK);
    CHECK(f->world.roles[f->child_a].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(f->world.roles[f->child_b].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    /*
     * Both adversarial copies arrive at TTL one. They may be inspected
     * locally, but neither may create an invalid zero-TTL forward. Each child
     * therefore queues only its first decodable copy of the real EACK below.
     */
    CHECK(mesh_sim_set_directed_rx_failures(&f->world,
                                            f->collector,
                                            f->child_a,
                                            1u,
                                            MESH_SIM_RX_DECODE_ERROR) ==
          MESH_SIM_OK);
    CHECK(transmit_outbound(f, f->collector, children, 2u,
                            &collector_forward, NULL) == MESH_SIM_OK);
    CHECK(f->world.roles[f->child_a].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(f->world.roles[f->child_b].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(transmit_outbound(f, f->collector, children, 2u,
                            &collector_forward, NULL) == MESH_SIM_OK);
    CHECK(f->world.roles[f->child_a].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(f->world.roles[f->child_b].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(!f->world.roles[f->child_a].relay.outbox_record.valid);
    CHECK(!f->world.roles[f->child_b].relay.outbox_record.valid);
    CHECK(mesh_sim_count_transitions(&f->world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     CHILD_A_ID) == 1u);
    CHECK(mesh_sim_count_transitions(&f->world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     CHILD_B_ID) == 1u);
    CHECK(drain_leaf_eack_forwards(f));
    return true;
}

static size_t matching_transmission_count(const struct mesh_sim_world *world,
                                          uint8_t physical_sender,
                                          uint8_t msg_type,
                                          uint64_t packet_src_id,
                                          uint32_t session_id,
                                          uint16_t seq)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &world->transmissions[i];

        if (tx->valid && tx->has_outbound &&
            tx->node_index == physical_sender &&
            tx->outbound.packet.msg_type == msg_type &&
            tx->outbound.packet.src_id == packet_src_id &&
            tx->outbound.packet.session_id == session_id &&
            tx->outbound.packet.seq == seq) {
            count++;
        }
    }
    return count;
}

static bool assert_bounded_traffic_and_settled(
    struct fixture *f,
    const struct mesh_outbound *accepted_bundle)
{
    struct mesh_sim_invariant_report report;
    struct gateway_collection_eack eack;
    size_t bundle_tx_count = 0u;
    size_t eack_tx_count = 0u;

    for (size_t i = 0u; i < f->world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &f->world.transmissions[i];

        if (!tx->valid || !tx->has_outbound) {
            continue;
        }
        if (tx->outbound.packet.msg_type == MSG_RESULT_BUNDLE &&
            tx->outbound.packet.src_id == accepted_bundle->packet.src_id &&
            tx->outbound.packet.session_id == accepted_bundle->packet.session_id &&
            tx->outbound.packet.seq == accepted_bundle->packet.seq) {
            bundle_tx_count++;
        }
        if (tx->outbound.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK) {
            eack_tx_count++;
        }
    }
    CHECK(bundle_tx_count <= MAX_IDENTICAL_RESULT_BUNDLE_TX);
    CHECK(bundle_tx_count == MAX_IDENTICAL_RESULT_BUNDLE_TX);
    CHECK(matching_transmission_count(&f->world,
                                      f->collector,
                                      MSG_RESULT_BUNDLE,
                                      accepted_bundle->packet.src_id,
                                      accepted_bundle->packet.session_id,
                                      accepted_bundle->packet.seq) <=
          MAX_IDENTICAL_RESULT_BUNDLE_TX_PER_RADIO);
    CHECK(matching_transmission_count(&f->world,
                                      f->upstream,
                                      MSG_RESULT_BUNDLE,
                                      accepted_bundle->packet.src_id,
                                      accepted_bundle->packet.session_id,
                                      accepted_bundle->packet.seq) == 0u);
    CHECK(eack_tx_count <= MAX_TOTAL_EACK_TX);
    CHECK(eack_tx_count == MAX_TOTAL_EACK_TX);
    CHECK(gateway_collection_build_eack(&f->collection,
                                        EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                                        &eack) == PROTO_OK);
    CHECK(matching_transmission_count(&f->world,
                                      f->gateway,
                                      MSG_GATEWAY_COLLECTION_EACK,
                                      GATEWAY_ID,
                                      COMMAND_SEQ,
                                      eack.packet_sequence) <=
          MAX_IDENTICAL_EACK_TX_PER_RADIO);
    CHECK(matching_transmission_count(&f->world,
                                      f->collector,
                                      MSG_GATEWAY_COLLECTION_EACK,
                                      GATEWAY_ID,
                                      COMMAND_SEQ,
                                      eack.packet_sequence) <=
          MAX_IDENTICAL_EACK_TX_PER_RADIO);
    CHECK(f->world.roles[f->gateway].delivery_count == 1u);
    CHECK(f->world.roles[f->gateway].gateway_semantic_commit_count == 1u);
    CHECK(f->world.roles[f->gateway]
              .gateway_semantic_duplicate_redelivery_count == 1u);
    CHECK(f->world.roles[f->gateway].gateway_semantic_duplicate_ack_count ==
          0u);
    CHECK(f->world.roles[f->gateway].gateway_semantic_rejection_count == 0u);
    for (size_t i = 0u; i < f->world.role_count; i++) {
        CHECK(f->world.roles[i].tx_queue_count == 0u);
        CHECK(f->world.roles[i].relay.pending.state == MESH_RELAY_TX_IDLE);
        CHECK(!f->world.roles[i].relay.outbox_record.valid);
        CHECK(!f->world.roles[i].relay.result_bundle.active);
        CHECK(!f->world.roles[i].relay.result_offer_reservation.valid);
    }
    gateway_collection_clear(&f->collection);
    CHECK(mesh_sim_check_settled(&f->world, &report) == MESH_SIM_OK);
    return true;
}

static bool run_forced_multihop_result_custody(void)
{
    struct mesh_outbound queued_bundle;
    struct mesh_outbound accepted_bundle;
    uint16_t bundle_tx_index;

    phase = "forced multihop result custody setup";
    CHECK(setup_topology(&fixture));

    phase = "child A offer grant result and custody ACK";
    CHECK(send_offer_and_result(&fixture, 0u, fixture.child_a));
    CHECK(fixture.world.roles[fixture.collector].relay.result_bundle.record_count ==
          1u);

    phase = "child B offer grant result and two-record bundle";
    CHECK(send_offer_and_result(&fixture, 1u, fixture.child_b));
    CHECK(start_bundle_custody(&fixture, &queued_bundle));

    phase = "mutated bundle rejection and persisted collector reset recovery";
    CHECK(reject_mutated_bundle_then_restore(&fixture, &queued_bundle));

    phase = "gateway accepts exact restored bundle once";
    CHECK(gateway_accepts_exact_retry(
              &fixture, &queued_bundle, &accepted_bundle, &bundle_tx_index));

    phase = "lost gateway ACK duplicate retry and owner cleanup";
    CHECK(lose_gateway_ack_then_accept_duplicate_retry(
              &fixture, &accepted_bundle, bundle_tx_index));

    phase = "lost stale malformed and duplicate collection EACK";
    CHECK(deliver_eack_with_loss_stale_and_duplicate(&fixture));

    phase = "bounded traffic exact once and settled ownership";
    CHECK(assert_bounded_traffic_and_settled(&fixture, &accepted_bundle));
    return true;
}

int main(void)
{
    if (!run_queue_pressure_gate() ||
        !run_forced_multihop_result_custody()) {
        return EXIT_FAILURE;
    }
    if (failures != 0u) {
        return EXIT_FAILURE;
    }
    puts("PASS mesh_result_custody_rf_scenarios");
    return EXIT_SUCCESS;
}
