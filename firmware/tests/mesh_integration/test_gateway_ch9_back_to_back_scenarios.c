#include "mesh_sim.h"

#include "report.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xd900000000000001)
#define ANCHOR_A_ID UINT64_C(0xda00000000000002)
#define ANCHOR_B_ID UINT64_C(0xdb00000000000003)
#define CLICKER_ID UINT64_C(0xdc00000000000004)
#define CLICK_EVENT_ID UINT32_C(0x26082601)
#define ROUTE_EPOCH UINT32_C(91)

#define FIRST_REPORT_START_US UINT64_C(50000)
#define REPORT_TX_SERVICE_US UINT64_C(50000)
#define OLD_POST_FRAME_RX_HOLE_US UINT64_C(8000)
#define SECOND_REPORT_IN_HOLE_US UINT64_C(4000)
#define ACK_PREPARE_US UINT64_C(50000)
#define ACK_SERVICE_US UINT64_C(40000)

_Static_assert(SECOND_REPORT_IN_HOLE_US < OLD_POST_FRAME_RX_HOLE_US,
               "second report must begin inside the former RX hole");

struct report_wire {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

static struct mesh_sim_world world;
static int failures;

#define CHECK(expression, ...) do {                                          \
    if (!(expression)) {                                                     \
        fprintf(stderr, "FAIL line=%d now_us=%" PRIu64 " error=%d ",       \
                __LINE__, world.now_us, world.last_error);                   \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        failures++;                                                          \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool packet_identity_equal(const struct proto_packet *left,
                                  const struct proto_packet *right)
{
    return left->msg_type == right->msg_type &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq;
}

static size_t packet_transmission_count(
    const struct mesh_sim_world *sim,
    uint64_t node_id,
    const struct proto_packet *packet)
{
    size_t count = 0u;

    for (size_t i = 0u; i < sim->transmission_count; i++) {
        const struct mesh_sim_transmission *transmission =
            &sim->transmissions[i];

        if (transmission->valid && transmission->has_outbound &&
            sim->roles[transmission->node_index].id == node_id &&
            packet_identity_equal(&transmission->outbound.packet, packet)) {
            count++;
        }
    }
    return count;
}

static size_t decoded_report_receptions(const struct mesh_sim_world *sim,
                                        uint64_t source_id)
{
    size_t count = 0u;

    for (size_t i = 0u; i < sim->reception_count; i++) {
        const struct mesh_sim_reception *rx = &sim->receptions[i];

        if (rx->receiver_id == GATEWAY_ID &&
            rx->source_id == source_id &&
            rx->packet.msg_type == MSG_CLICK_REPORT &&
            rx->outcome == MESH_SIM_RX_DECODED &&
            rx->protocol_status == PROTO_OK) {
            count++;
        }
    }
    return count;
}

static size_t gateway_deliveries_for(const struct mesh_sim_role_instance *gateway,
                                     const struct proto_packet *packet)
{
    size_t count = 0u;

    for (size_t i = 0u; i < gateway->delivery_count; i++) {
        if (packet_identity_equal(&gateway->deliveries[i].packet, packet)) {
            count++;
        }
    }
    return count;
}

static bool build_click_report(uint64_t anchor_id,
                               uint16_t seq,
                               struct report_wire *wire)
{
    static const uint64_t participants[] = {
        ANCHOR_A_ID,
        ANCHOR_B_ID,
    };
    const int32_t samples[] = {730};
    const uint8_t rounds[] = {0u};
    const uint64_t starts[] = {47u};
    const struct range_report_fields fields = {
        .clicker_id = CLICKER_ID,
        .anchor_id = anchor_id,
        .event_seq = CLICK_EVENT_ID,
        .timestamp_ms = 47u,
        .distance_mm = 730,
        .quality = 93u,
        .range_status = RANGE_OK,
        .distance_samples_mm = samples,
        .range_round_indices = rounds,
        .sequence_start_timestamps_ms = starts,
        .sample_count = 1u,
        .participant_anchor_ids = participants,
        .participant_anchor_count = 2u,
        .burst_id = CLICK_EVENT_ID,
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
    };
    int ret;

    memset(wire, 0, sizeof(*wire));
    ret = report_append_range_tlvs(wire->payload,
                                   sizeof(wire->payload),
                                   &wire->payload_len,
                                   &fields);
    if (ret != PROTO_OK || wire->payload_len > UINT8_MAX) {
        return false;
    }
    ret = report_init_click_packet(
        &wire->packet,
        anchor_id,
        GATEWAY_ID,
        proto_click_report_session_id(CLICKER_ID, CLICK_EVENT_ID),
        seq,
        (uint8_t)wire->payload_len);
    return ret == PROTO_OK &&
           report_validate_click_payload(&wire->packet,
                                         wire->payload,
                                         wire->payload_len) == PROTO_OK;
}

static bool gateway_ack_sender(uint8_t gateway_index, uint8_t sender_index)
{
    uint64_t ready_us = world.now_us;
    uint64_t ack_start_us;
    uint64_t ack_end_us;
    int ret;

    if (world.roles[gateway_index].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world.roles[gateway_index].dwm3000.cpu_busy_until_us;
    }
    if (world.roles[sender_index].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world.roles[sender_index].dwm3000.cpu_busy_until_us;
    }
    ret = mesh_sim_run_until(&world, ready_us);
    if (ret != MESH_SIM_OK) {
        return false;
    }
    ack_start_us = world.now_us + ACK_PREPARE_US;
    ack_end_us = ack_start_us + ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(&world,
                                                gateway_index,
                                                sender_index,
                                                ack_start_us,
                                                ack_end_us,
                                                NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(&world, ack_end_us);
    }
    return ret == MESH_SIM_OK;
}

/*
 * MSG_GATEWAY_ACK_CONFIRM is retired: the gateway ACK is sent on RAM/BLE
 * admission and is itself terminal proof. A sender that received it must
 * therefore hold no custody at all - no pending report, no confirm debt and
 * no confirm frame it could still build.
 */
static bool sender_custody_retired_by_gateway_ack(
    const struct mesh_sim_role_instance *sender,
    const struct report_wire *report)
{
    const struct mesh_pending_tx *pending = &sender->relay.pending;
    struct proto_packet confirm;
    uint8_t payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t payload_len = 0u;

    (void)report;
    if (pending->state != MESH_RELAY_TX_IDLE ||
        pending->gateway_ack_confirm_pending) {
        return false;
    }
    return mesh_relay_pending_gateway_ack_confirm_wire(
               &sender->relay,
               (uint32_t)(world.now_us / 1000u),
               &confirm,
               payload,
               sizeof(payload),
               &payload_len) == PROTO_ERR_NOT_FOUND &&
           payload_len == 0u;
}

static bool test_back_to_back_reports_cross_former_rearm_hole(void)
{
    struct report_wire report_a;
    struct report_wire report_b;
    uint8_t anchor_a;
    uint8_t anchor_b;
    uint8_t gateway;
    uint16_t tx_a;
    uint16_t tx_b;
    uint64_t first_end_us;
    uint64_t second_start_us;
    uint64_t second_end_us;
    uint64_t gateway_rx_end_us;
    int ret;

    mesh_sim_init(&world, UINT32_C(0xbac27009));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ANCHOR_A_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &anchor_a) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                ANCHOR_B_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &anchor_b) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK,
          "role setup failed");
    CHECK(mesh_sim_set_link(&world, anchor_a, gateway, 100u, 1u) ==
              MESH_SIM_OK &&
              mesh_sim_set_link(&world, anchor_b, gateway, 100u, 1u) ==
              MESH_SIM_OK,
          "direct links failed");
    CHECK(mesh_relay_note_direct_gateway_route(
              &world.roles[anchor_a].relay, 0u) == PROTO_OK &&
              mesh_relay_note_direct_gateway_route(
                  &world.roles[anchor_b].relay, 0u) == PROTO_OK,
          "direct routes failed");
    CHECK(build_click_report(ANCHOR_A_ID, 11u, &report_a) &&
              build_click_report(ANCHOR_B_ID, 12u, &report_b),
          "click report build failed");
    CHECK(mesh_sim_queue_originated(&world, anchor_a,
                                    &report_a.packet,
                                    report_a.payload,
                                    report_a.payload_len) == MESH_SIM_OK &&
              mesh_sim_queue_originated(&world, anchor_b,
                                        &report_b.packet,
                                        report_b.payload,
                                        report_b.payload_len) == MESH_SIM_OK,
          "report queueing failed");

    ret = mesh_sim_direct_gateway_start_queued_tx(
        &world,
        anchor_a,
        FIRST_REPORT_START_US,
        FIRST_REPORT_START_US + REPORT_TX_SERVICE_US,
        &tx_a);
    CHECK(ret == MESH_SIM_OK, "first report TX failed ret=%d", ret);
    first_end_us = world.transmissions[tx_a].end_us;
    second_start_us = first_end_us + SECOND_REPORT_IN_HOLE_US;
    CHECK(second_start_us < first_end_us + OLD_POST_FRAME_RX_HOLE_US,
          "second report escaped former RX hole");
    ret = mesh_sim_direct_gateway_start_queued_tx(
        &world,
        anchor_b,
        second_start_us,
        second_start_us + REPORT_TX_SERVICE_US,
        &tx_b);
    CHECK(ret == MESH_SIM_OK, "second report TX failed ret=%d", ret);
    second_end_us = world.transmissions[tx_b].end_us;
    gateway_rx_end_us = second_end_us + 1u;
    CHECK(mesh_sim_direct_gateway_arm_rx(&world,
                                         gateway,
                                         FIRST_REPORT_START_US,
                                         gateway_rx_end_us) == MESH_SIM_OK &&
              mesh_sim_run_until(&world, gateway_rx_end_us) == MESH_SIM_OK,
          "continuous gateway RX failed");

    CHECK(decoded_report_receptions(&world, ANCHOR_A_ID) == 1u &&
              decoded_report_receptions(&world, ANCHOR_B_ID) == 1u,
          "gateway did not decode both complete reports exactly once");
    CHECK(world.roles[gateway].delivery_count == 2u &&
              world.roles[gateway].gateway_semantic_commit_count == 2u &&
              world.roles[gateway].gateway_semantic_rejection_count == 0u &&
              world.roles[gateway].gateway_semantic_duplicate_redelivery_count == 0u &&
              gateway_deliveries_for(&world.roles[gateway],
                                     &report_a.packet) == 1u &&
              gateway_deliveries_for(&world.roles[gateway],
                                     &report_b.packet) == 1u,
          "gateway did not retain both reports exactly once");
    {
        size_t tx_a_count = packet_transmission_count(
            &world, ANCHOR_A_ID, &report_a.packet);
        size_t tx_b_count = packet_transmission_count(
            &world, ANCHOR_B_ID, &report_b.packet);

        CHECK(tx_a_count == 1u && tx_b_count == 1u,
              "report TX identity count disagreed before gateway ACK "
              "anchor_a=%zu anchor_b=%zu",
              tx_a_count, tx_b_count);
    }

    CHECK(gateway_ack_sender(gateway, anchor_a),
          "gateway did not ACK anchor A");
    CHECK(gateway_ack_sender(gateway, anchor_b),
          "gateway did not ACK anchor B");
    {
        size_t tx_a_count = packet_transmission_count(
            &world, ANCHOR_A_ID, &report_a.packet);
        size_t tx_b_count = packet_transmission_count(
            &world, ANCHOR_B_ID, &report_b.packet);

        CHECK(tx_a_count == 1u && tx_b_count == 1u,
              "report TX identity count disagreed after gateway ACK "
              "anchor_a=%zu anchor_b=%zu",
              tx_a_count, tx_b_count);
    }
    CHECK(sender_custody_retired_by_gateway_ack(&world.roles[anchor_a],
                                                &report_a),
          "anchor A custody was not retired by the terminal gateway ACK "
          "pending_state=%u pending_type=0x%02x confirm=%u "
          "outbox_valid=%u outbox_acked=%u",
          (unsigned int)world.roles[anchor_a].relay.pending.state,
          world.roles[anchor_a].relay.pending.packet.msg_type,
          world.roles[anchor_a].relay.pending.gateway_ack_confirm_pending ?
              1u : 0u,
          world.roles[anchor_a].relay.outbox_record.valid ? 1u : 0u,
          world.roles[anchor_a].relay.outbox_record.gateway_acked ? 1u : 0u);
    CHECK(sender_custody_retired_by_gateway_ack(&world.roles[anchor_b],
                                                &report_b),
          "anchor B custody was not retired by the terminal gateway ACK "
          "pending_state=%u pending_type=0x%02x confirm=%u "
          "outbox_valid=%u outbox_acked=%u",
          (unsigned int)world.roles[anchor_b].relay.pending.state,
          world.roles[anchor_b].relay.pending.packet.msg_type,
          world.roles[anchor_b].relay.pending.gateway_ack_confirm_pending ?
              1u : 0u,
          world.roles[anchor_b].relay.outbox_record.valid ? 1u : 0u,
          world.roles[anchor_b].relay.outbox_record.gateway_acked ? 1u : 0u);
    CHECK(world.roles[gateway].delivery_count == 2u,
          "ACK processing created an extra host delivery");

    puts("PASS gateway back-to-back reports cross the former 8 ms post-frame "
         "hole with exactly-once retention and terminal gateway ACK custody");
    return true;
}

int main(void)
{
    bool ok = test_back_to_back_reports_cross_former_rearm_hole();

    return ok && failures == 0 ? 0 : 1;
}
