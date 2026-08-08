#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_A_ID UINT64_C(0x000000000000a101)
#define SOURCE_B_ID UINT64_C(0x000000000000a102)
#define ALT_PARENT_ID UINT64_C(0x000000000000a103)
#define GATEWAY_ID UINT64_C(0x000000000000a1ff)

static int failures;
static const char *phase;

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__, phase); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            failures++; \
            return; \
        } \
    } while (0)

static struct proto_packet data_packet(uint64_t source_id, uint16_t seq)
{
    return (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .src_id = source_id,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x51a7f011),
        .seq = seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
}

static int build_gateway_ack_for_packet(const struct proto_packet *packet,
                                        const uint8_t *packet_payload,
                                        size_t packet_payload_len,
                                        uint16_t ack_seq,
                                        struct proto_packet *ack,
                                        uint8_t *ack_payload,
                                        size_t ack_payload_cap,
                                        size_t *ack_payload_len)
{
    int ret;

    if (packet == NULL || ack == NULL || ack_payload == NULL ||
        ack_payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    *ack_payload_len = 0u;
    ret = mesh_append_requested_seq(ack_payload,
                                    ack_payload_cap,
                                    ack_payload_len,
                                    packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_ack_semantic_identity(ack_payload,
                                            ack_payload_cap,
                                            ack_payload_len,
                                            packet,
                                            packet_payload,
                                            packet_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_init_gateway_ack(ack,
                                 GATEWAY_ID,
                                 packet->src_id,
                                 packet->session_id,
                                 ack_seq,
                                 (uint8_t)*ack_payload_len);
}

static bool pending_identity_unchanged(const struct mesh_pending_tx *actual,
                                       const struct mesh_pending_tx *expected)
{
    return actual != NULL && expected != NULL &&
           actual->state == expected->state &&
           actual->packet.msg_type == expected->packet.msg_type &&
           actual->packet.src_id == expected->packet.src_id &&
           actual->packet.dst_id == expected->packet.dst_id &&
           actual->packet.session_id == expected->packet.session_id &&
           actual->packet.seq == expected->packet.seq &&
           actual->payload_len == expected->payload_len &&
           memcmp(actual->payload,
                  expected->payload,
                  expected->payload_len) == 0 &&
           actual->next_hop_id == expected->next_hop_id &&
           actual->gateway_ack_deadline_ms ==
               expected->gateway_ack_deadline_ms &&
           actual->retry_after_ms == expected->retry_after_ms &&
           actual->gateway_ack_forward_pending ==
               expected->gateway_ack_forward_pending;
}

static void setup_direct_pair(struct mesh_sim_world *world,
                              uint32_t seed,
                              uint8_t *source,
                              uint8_t *gateway)
{
    mesh_sim_init(world, seed);
    CHECK(mesh_sim_add_role(world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            SOURCE_A_ID,
                            GATEWAY_ID,
                            1u,
                            source) == MESH_SIM_OK,
          "source setup failed");
    CHECK(mesh_sim_add_role(world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            1u,
                            gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_set_link(world, *source, *gateway, 100u, 0u) == MESH_SIM_OK,
          "direct link setup failed");
}

static void test_broadcast_queue_identity_is_valid(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_invariant_report invariant = {0};
    struct mesh_sim_role_instance *node;
    uint8_t source;
    uint8_t gateway;

    phase = "broadcast-queue-identity";
    setup_direct_pair(&world, UINT32_C(0xb40adc45), &source, &gateway);
    node = &world.roles[source];
    node->tx_queue[0] = (struct mesh_sim_queued_tx) {
        .outbound = {
            .packet = {
                .msg_type = MSG_COMMAND,
                .src_id = SOURCE_A_ID,
                .dst_id = MESH_BROADCAST_ID,
                .session_id = UINT32_C(0xb40adc45),
                .seq = 1u,
                .ttl = MESH_DEFAULT_TTL,
                .payload_len = 0u,
            },
            .next_hop_id = MESH_BROADCAST_ID,
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        },
        .enqueue_order = 1u,
        .priority = 1u,
        .valid = true,
    };
    node->tx_queue_count = 1u;

    CHECK(mesh_sim_check_invariants(&world, &invariant) == MESH_SIM_OK,
          "valid broadcast queue entry failed invariant code=%d detail=%llu",
          invariant.code, (unsigned long long)invariant.detail);
}

static void test_complete_frame_loss_is_explicit(void)
{
    static struct mesh_sim_world world;
    const struct mesh_sim_fault_config config = {
        .seed = UINT32_C(0x1001),
        .frame_loss_permyriad = MESH_SIM_FAULT_RATE_SCALE,
    };
    struct mesh_sim_fault_stats stats;
    struct proto_packet packet = data_packet(SOURCE_A_ID, 1u);
    uint8_t source;
    uint8_t gateway;
    uint16_t ignored;

    phase = "complete-frame-loss";
    setup_direct_pair(&world, UINT32_C(0x2001), &source, &gateway);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_sim_set_fault_config(&world, &config) == MESH_SIM_OK,
          "fault configuration failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               gateway,
                               0u,
                               UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "gateway RX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(&world,
                                      source,
                                      UINT64_C(1000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &packet,
                                      NULL,
                                      0u,
                                      &ignored) == MESH_SIM_OK,
          "packet TX setup failed");
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK, "simulation failed");
    CHECK(world.reception_count == 1u,
          "loss produced %zu receptions instead of one explicit outcome",
          world.reception_count);
    CHECK(world.receptions[0].outcome == MESH_SIM_RX_DECODE_ERROR,
          "100%% loss outcome=%d", world.receptions[0].outcome);
    CHECK(world.roles[gateway].delivery_count == 0u,
          "lost frame reached the protocol delivery owner");
    CHECK(mesh_sim_get_fault_stats(&world, &stats) == MESH_SIM_OK,
          "fault statistics unavailable");
    CHECK(stats.receiver_decisions == 1u && stats.frame_losses == 1u &&
              stats.ack_losses == 0u,
          "unexpected loss stats decisions=%llu frame=%llu ack=%llu",
          (unsigned long long)stats.receiver_decisions,
          (unsigned long long)stats.frame_losses,
          (unsigned long long)stats.ack_losses);
    CHECK(mesh_sim_count_transitions(
              &world, MESH_SIM_TRANSITION_FAULT_FRAME_DROPPED, GATEWAY_ID) == 1u,
          "frame-loss decision was not traced exactly once");
}

static void test_ack_loss_does_not_drop_data(void)
{
    static struct mesh_sim_world world;
    const struct mesh_sim_fault_config config = {
        .seed = UINT32_C(0x1002),
        .ack_loss_permyriad = MESH_SIM_FAULT_RATE_SCALE,
    };
    struct mesh_sim_fault_stats stats;
    struct proto_packet data = data_packet(SOURCE_A_ID, 2u);
    const uint8_t data_payload[] = {0x42u};
    struct proto_packet ack;
    uint8_t source;
    uint8_t gateway;
    uint16_t ignored;

    phase = "ack-only-loss";
    data.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    data.payload_len = sizeof(data_payload);
    setup_direct_pair(&world, UINT32_C(0x2002), &source, &gateway);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_init_gateway_ack(&ack,
                                GATEWAY_ID,
                                SOURCE_A_ID,
                                data.session_id,
                                data.seq,
                                0u) == PROTO_OK,
          "gateway ACK setup failed");
    CHECK(mesh_sim_set_fault_config(&world, &config) == MESH_SIM_OK,
          "fault configuration failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               gateway,
                               0u,
                               UINT64_C(6000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "gateway RX setup failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               source,
                               UINT64_C(10000),
                               UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "source ACK RX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(&world,
                                      source,
                                      UINT64_C(1000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &data,
                                      data_payload,
                                      sizeof(data_payload),
                                      &ignored) == MESH_SIM_OK,
          "data TX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(&world,
                                      gateway,
                                      UINT64_C(11000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &ack,
                                      NULL,
                                      0u,
                                      &ignored) == MESH_SIM_OK,
          "ACK TX setup failed");
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK, "simulation failed");
    CHECK(world.reception_count == 2u,
          "ACK scenario produced %zu receptions", world.reception_count);
    CHECK(world.receptions[0].packet.msg_type == MSG_MESH_DATA &&
              world.receptions[0].outcome == MESH_SIM_RX_DECODED,
          "ACK-only loss altered the data frame outcome");
    CHECK(world.receptions[1].source_id == GATEWAY_ID &&
              world.receptions[1].receiver_id == SOURCE_A_ID &&
              world.receptions[1].outcome == MESH_SIM_RX_DECODE_ERROR,
          "gateway ACK was not classified as the dropped frame "
          "rx0=0x%02x/%d rx1=0x%02x/%d",
          world.receptions[0].packet.msg_type,
          world.receptions[0].outcome,
          world.receptions[1].packet.msg_type,
          world.receptions[1].outcome);
    /* A decoded RF frame is not a committed gateway delivery.  The gateway
     * now retains semantic custody until the GUI accepts the exact stream
     * record, and this radio-only scenario deliberately supplies no receipt. */
    CHECK(world.roles[gateway].delivery_count == 0u &&
              world.roles[gateway].gateway_semantic_commit_count == 0u,
          "ACK loss caused an unreceipted semantic commit deliveries=%zu "
          "commits=%u",
          world.roles[gateway].delivery_count,
          world.roles[gateway].gateway_semantic_commit_count);
    CHECK(mesh_sim_get_fault_stats(&world, &stats) == MESH_SIM_OK,
          "fault statistics unavailable");
    CHECK(stats.receiver_decisions == 2u && stats.ack_losses == 1u &&
              stats.frame_losses == 0u,
          "unexpected ACK stats decisions=%llu ack=%llu frame=%llu",
          (unsigned long long)stats.receiver_decisions,
          (unsigned long long)stats.ack_losses,
          (unsigned long long)stats.frame_losses);
    CHECK(mesh_sim_count_transitions(
              &world, MESH_SIM_TRANSITION_FAULT_ACK_DROPPED, SOURCE_A_ID) == 1u,
          "ACK-loss decision was not traced exactly once");
}

static void test_duplicate_delivery_is_idempotent(void)
{
    static struct mesh_sim_world world;
    const struct mesh_sim_fault_config config = {
        .seed = UINT32_C(0x1003),
        .duplicate_permyriad = MESH_SIM_FAULT_RATE_SCALE,
    };
    struct mesh_sim_fault_stats stats;
    struct proto_packet packet = data_packet(SOURCE_A_ID, 3u);
    uint8_t source;
    uint8_t gateway;
    uint16_t ignored;

    phase = "duplicate-idempotence";
    setup_direct_pair(&world, UINT32_C(0x2003), &source, &gateway);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_sim_set_fault_config(&world, &config) == MESH_SIM_OK,
          "fault configuration failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               gateway,
                               0u,
                               UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "gateway RX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(&world,
                                      source,
                                      UINT64_C(1000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &packet,
                                      NULL,
                                      0u,
                                      &ignored) == MESH_SIM_OK,
          "packet TX setup failed");
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK, "simulation failed");
    CHECK(world.reception_count == 2u,
          "duplicate injection produced %zu decoded dispatches",
          world.reception_count);
    CHECK(world.receptions[0].outcome == MESH_SIM_RX_DECODED &&
              world.receptions[1].outcome == MESH_SIM_RX_DECODED,
          "duplicate injection changed the radio decode outcome");
    /* Duplicate RF delivery is still decoded twice, but neither copy may
     * commit gateway semantics before the GUI receipt boundary. */
    CHECK(world.roles[gateway].delivery_count == 0u &&
              world.roles[gateway].gateway_semantic_commit_count == 0u,
          "duplicate frame committed unreceipted gateway semantics: "
          "deliveries=%zu commits=%u",
          world.roles[gateway].delivery_count,
          world.roles[gateway].gateway_semantic_commit_count);
    CHECK(mesh_sim_get_fault_stats(&world, &stats) == MESH_SIM_OK,
          "fault statistics unavailable");
    CHECK(stats.duplicates == 1u,
          "duplicate statistics=%llu", (unsigned long long)stats.duplicates);
    CHECK(mesh_sim_count_transitions(
              &world, MESH_SIM_TRANSITION_FAULT_DUPLICATED, GATEWAY_ID) == 1u,
          "duplicate decision was not traced exactly once");
}

static void test_duplicate_relay_admission_is_coalesced(void)
{
    static struct mesh_sim_world world;
    const struct mesh_sim_fault_config config = {
        .seed = UINT32_C(0x1004),
        .duplicate_permyriad = MESH_SIM_FAULT_RATE_SCALE,
    };
    struct proto_packet packet = data_packet(SOURCE_A_ID, 4u);
    const uint8_t data_payload[] = {0x43u};
    uint8_t source;
    uint8_t relay;
    uint8_t gateway;
    uint16_t ignored;

    phase = "duplicate-relay-admission";
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet.payload_len = sizeof(data_payload);
    mesh_sim_init(&world, UINT32_C(0x2004));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                            SOURCE_A_ID, GATEWAY_ID, 1u, &source) ==
              MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                SOURCE_B_ID, GATEWAY_ID, 1u, &relay) ==
              MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID, GATEWAY_ID, 1u, &gateway) ==
              MESH_SIM_OK,
          "relay roles setup failed");
    CHECK(mesh_sim_set_link(&world, source, relay, 100u, 0u) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, relay, gateway, 100u, 0u) ==
                  MESH_SIM_OK &&
              mesh_sim_install_route(&world, relay, gateway, 0u, 1u) ==
                  PROTO_OK,
          "relay topology setup failed");
    CHECK(mesh_sim_set_fault_config(&world, &config) == MESH_SIM_OK,
          "fault configuration failed");
    CHECK(mesh_sim_schedule_rx(&world, relay, 0u, UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK &&
              mesh_sim_schedule_packet_tx(&world, source, UINT64_C(1000),
                                          UWB_CHANNEL_MESH_PAYLOAD,
                                          MESH_SIM_PHY_CHANNEL9_MESH,
                                          &packet,
                                          data_payload,
                                          sizeof(data_payload),
                                          &ignored) ==
                  MESH_SIM_OK,
          "duplicate relay radio setup failed");
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK, "simulation failed");
    CHECK(world.roles[relay].tx_queue_count == 2u,
          "one duplicated DATA frame admitted %zu relay actions instead of "
          "one forward and one semantic hop ACK",
          world.roles[relay].tx_queue_count);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_PACKET_COALESCED,
                                     SOURCE_B_ID) == 2u,
          "duplicate forward/ACK admission was not diagnosed twice");
}

static void test_malformed_control_is_inert(void)
{
    static struct mesh_sim_world world;
    struct proto_packet pending_packet = data_packet(SOURCE_A_ID, 33u);
    struct proto_packet malformed_ack;
    struct mesh_pending_tx pending_before;
    struct mesh_outbound outbound;
    const uint8_t malformed_payload[] = {0xffu};
    uint8_t source;
    uint8_t gateway;
    uint16_t ignored;

    phase = "malformed-control-inert";
    pending_packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    setup_direct_pair(&world, UINT32_C(0x2033), &source, &gateway);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_relay_note_direct_gateway_route(
              &world.roles[source].relay, 0u) == PROTO_OK,
          "direct route setup failed");
    CHECK(mesh_relay_start_tx(&world.roles[source].relay,
                              &pending_packet,
                              NULL,
                              0u,
                              0u,
                              &outbound) == PROTO_OK,
          "pending operation setup failed");
    mesh_relay_note_tx_sent(&world.roles[source].relay, &outbound, 0u);
    pending_before = world.roles[source].relay.pending;
    CHECK(mesh_init_gateway_ack(&malformed_ack,
                                GATEWAY_ID,
                                SOURCE_A_ID,
                                pending_packet.session_id,
                                99u,
                                sizeof(malformed_payload)) == PROTO_OK,
          "malformed ACK envelope setup failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               source,
                               0u,
                               UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "source RX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(&world,
                                      gateway,
                                      UINT64_C(1000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &malformed_ack,
                                      malformed_payload,
                                      sizeof(malformed_payload),
                                      &ignored) == MESH_SIM_OK,
          "malformed ACK TX setup failed");
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK,
          "malformed control escaped as a simulator failure");
    CHECK(world.reception_count == 1u &&
              world.receptions[0].outcome == MESH_SIM_RX_DECODED,
          "malformed control did not exercise the production parser");
    CHECK(world.roles[source].relay.pending.state == pending_before.state &&
              world.roles[source].relay.pending.packet.session_id ==
                  pending_before.packet.session_id &&
              world.roles[source].relay.pending.packet.seq ==
                  pending_before.packet.seq &&
              world.roles[source].relay.pending.gateway_ack_deadline_ms ==
                  pending_before.gateway_ack_deadline_ms &&
              world.roles[source].tx_queue_count == 0u,
          "malformed ACK mutated or completed the pending operation");
}

static void setup_reordering_world(struct mesh_sim_world *world)
{
    const struct mesh_sim_fault_config config = {
        .seed = 3u,
        .max_extra_delay_us = 10000u,
        .delay_permyriad = MESH_SIM_FAULT_RATE_SCALE,
    };
    struct proto_packet first = data_packet(SOURCE_A_ID, 4u);
    struct proto_packet second = data_packet(SOURCE_B_ID, 5u);
    uint8_t source_a;
    uint8_t source_b;
    uint8_t gateway;
    uint16_t ignored;

    mesh_sim_init(world, UINT32_C(0x2004));
    CHECK(mesh_sim_add_role(world, MESH_SIM_ROLE_TRANSMITTER, SOURCE_A_ID,
                            GATEWAY_ID, 1u, &source_a) == MESH_SIM_OK,
          "first source setup failed");
    CHECK(mesh_sim_add_role(world, MESH_SIM_ROLE_TRANSMITTER, SOURCE_B_ID,
                            GATEWAY_ID, 1u, &source_b) == MESH_SIM_OK,
          "second source setup failed");
    CHECK(mesh_sim_add_role(world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, 1u, &gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_set_link(world, source_a, gateway, 100u, 0u) == MESH_SIM_OK &&
              mesh_sim_set_link(world, source_b, gateway, 100u, 0u) == MESH_SIM_OK,
          "reordering links setup failed");
    CHECK(mesh_sim_set_fault_config(world, &config) == MESH_SIM_OK,
          "fault configuration failed");
    CHECK(mesh_sim_schedule_rx(world,
                               gateway,
                               0u,
                               UINT64_C(20000),
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               &ignored) == MESH_SIM_OK,
          "gateway RX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(world, source_a, UINT64_C(1000),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &first, NULL, 0u, &ignored) == MESH_SIM_OK,
          "first packet TX setup failed");
    CHECK(mesh_sim_schedule_packet_tx(world, source_b, UINT64_C(3500),
                                      UWB_CHANNEL_MESH_PAYLOAD,
                                      MESH_SIM_PHY_CHANNEL9_MESH,
                                      &second, NULL, 0u, &ignored) == MESH_SIM_OK,
          "second packet TX setup failed");
    CHECK(mesh_sim_run(world) == MESH_SIM_OK, "simulation failed");
}

static void test_same_seed_replays_delay_and_reordering(void)
{
    static struct mesh_sim_world first;
    static struct mesh_sim_world replay;
    struct mesh_sim_fault_stats first_stats;
    struct mesh_sim_fault_stats replay_stats;

    phase = "deterministic-delay-reordering";
    setup_reordering_world(&first);
    if (failures != 0) {
        return;
    }
    setup_reordering_world(&replay);
    if (failures != 0) {
        return;
    }
    CHECK(first.transmission_count == 2u && replay.transmission_count == 2u,
          "expected exactly two transmissions");
    for (size_t i = 0u; i < first.transmission_count; i++) {
        CHECK(first.transmissions[i].fault_decision_ordinal[2] ==
                  replay.transmissions[i].fault_decision_ordinal[2] &&
                  first.transmissions[i].fault_extra_delay_us[2] ==
                  replay.transmissions[i].fault_extra_delay_us[2] &&
                  first.transmissions[i].fault_frame_loss_receivers ==
                  replay.transmissions[i].fault_frame_loss_receivers &&
                  first.transmissions[i].fault_ack_loss_receivers ==
                  replay.transmissions[i].fault_ack_loss_receivers &&
                  first.transmissions[i].fault_duplicate_receivers ==
                  replay.transmissions[i].fault_duplicate_receivers,
              "transmission %zu did not replay the same fault decision", i);
    }
    CHECK(first.transmissions[0].fault_extra_delay_us[2] == 7004u &&
              first.transmissions[1].fault_extra_delay_us[2] == 656u,
          "seed 3 delay vector changed (%u, %u)",
          first.transmissions[0].fault_extra_delay_us[2],
          first.transmissions[1].fault_extra_delay_us[2]);
    CHECK(first.reception_count == 2u && replay.reception_count == 2u,
          "expected two non-colliding receptions");
    CHECK(first.receptions[0].packet.seq == 5u &&
              first.receptions[1].packet.seq == 4u,
          "delay did not reorder the frames (%u, %u)",
          first.receptions[0].packet.seq,
          first.receptions[1].packet.seq);
    for (size_t i = 0u; i < first.reception_count; i++) {
        CHECK(first.receptions[i].packet.seq == replay.receptions[i].packet.seq &&
                  first.receptions[i].start_us == replay.receptions[i].start_us &&
                  first.receptions[i].end_us == replay.receptions[i].end_us &&
                  first.receptions[i].outcome == replay.receptions[i].outcome,
              "reception %zu did not replay exactly", i);
    }
    CHECK(mesh_sim_get_fault_stats(&first, &first_stats) == MESH_SIM_OK &&
              mesh_sim_get_fault_stats(&replay, &replay_stats) == MESH_SIM_OK,
          "fault statistics unavailable");
    CHECK(memcmp(&first_stats, &replay_stats, sizeof(first_stats)) == 0 &&
              first_stats.receiver_decisions == 2u &&
              first_stats.delay_injections == 2u,
          "same seed produced different fault statistics");
}

static void test_stale_relay_tick_cannot_mutate_new_operation(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_role_instance *node;
    struct mesh_pending_tx pending_before;
    struct mesh_outbound outbound;
    struct proto_packet operation_n = data_packet(SOURCE_A_ID, 6u);
    struct proto_packet operation_n_plus_one = data_packet(SOURCE_A_ID, 7u);
    uint64_t stale_due_us;
    uint32_t stale_generation;
    uint8_t source;

    phase = "stale-relay-timer-generation";
    operation_n.flags = FLAG_GATEWAY_ACK_REQUIRED;
    operation_n_plus_one.flags = FLAG_GATEWAY_ACK_REQUIRED;
    mesh_sim_init(&world, UINT32_C(0x2005));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            SOURCE_A_ID,
                            GATEWAY_ID,
                            1u,
                            &source) == MESH_SIM_OK,
          "source setup failed");
    node = mesh_sim_role(&world, source);
    CHECK(node != NULL &&
              mesh_relay_note_direct_gateway_route(&node->relay, 0u) == PROTO_OK,
          "direct gateway route setup failed");
    CHECK(mesh_relay_start_tx(&node->relay,
                              &operation_n,
                              NULL,
                              0u,
                              0u,
                              &outbound) == PROTO_OK,
          "operation N setup failed");
    mesh_relay_note_tx_sent(&node->relay, &outbound, 0u);
    CHECK(node->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
              node->relay.pending.gateway_ack_deadline_ms != 0u,
          "operation N did not arm an ACK deadline state=%d deadline=%u "
          "next=0x%llx",
          node->relay.pending.state,
          node->relay.pending.gateway_ack_deadline_ms,
          (unsigned long long)node->relay.pending.next_hop_id);
    stale_due_us = (uint64_t)node->relay.pending.gateway_ack_deadline_ms * 1000u;
    CHECK(mesh_sim_schedule_relay_tick(&world, source, stale_due_us) ==
              MESH_SIM_OK,
          "operation N timer setup failed");
    stale_generation = node->relay_timer_guard.generation;
    CHECK(stale_generation != 0u && node->relay_timer_guard.valid,
          "operation N timer was not generation guarded");

    mesh_relay_cancel_tx(&node->relay);
    CHECK(mesh_relay_start_tx(&node->relay,
                              &operation_n_plus_one,
                              NULL,
                              0u,
                              0u,
                              &outbound) == PROTO_OK,
          "operation N+1 setup failed");
    mesh_relay_note_tx_sent(&node->relay, &outbound, 0u);
    CHECK(node->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
              node->relay.pending.packet.seq == operation_n_plus_one.seq,
          "operation N+1 did not own relay state");
    pending_before = node->relay.pending;
    CHECK(mesh_sim_run_until(&world, stale_due_us) == MESH_SIM_OK,
          "stale timer execution failed");
    CHECK(node->relay.pending.state == pending_before.state &&
              node->relay.pending.packet.msg_type ==
                  pending_before.packet.msg_type &&
              node->relay.pending.packet.src_id == pending_before.packet.src_id &&
              node->relay.pending.packet.dst_id == pending_before.packet.dst_id &&
              node->relay.pending.packet.session_id ==
                  pending_before.packet.session_id &&
              node->relay.pending.packet.seq == pending_before.packet.seq &&
              node->relay.pending.gateway_ack_deadline_ms ==
                  pending_before.gateway_ack_deadline_ms &&
              node->relay.pending.retry_after_ms == pending_before.retry_after_ms &&
              node->relay.pending.busy_retry_round ==
                  pending_before.busy_retry_round,
          "operation N timer mutated operation N+1 relay state");
    CHECK(node->relay_timer_guard.generation == stale_generation,
          "ignored stale timer unexpectedly advanced its generation");
    CHECK(mesh_sim_count_transitions(
              &world, MESH_SIM_TRANSITION_RETRY_READY, SOURCE_A_ID) == 0u,
          "stale timer made operation N+1 retry-ready");
}

static void test_route_change_rejects_stale_parent_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate primary = {
        .next_hop_id = SOURCE_B_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 1u,
        .last_seen_ms = 1000u,
        .last_success_ms = 1000u,
        .hop_count = 1u,
        .link_quality = 100u,
        .valid = true,
    };
    struct route_candidate alternate = {
        .next_hop_id = ALT_PARENT_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 1u,
        .last_seen_ms = 1000u,
        .last_success_ms = 900u,
        .hop_count = 1u,
        .link_quality = 90u,
        .valid = true,
    };
    struct proto_packet packet = data_packet(SOURCE_A_ID, 8u);
    struct proto_packet gateway_ack;
    struct mesh_outbound outbound;
    struct mesh_pending_tx pending_before_stale_ack;
    struct persistent_outbox_record outbox_before_stale_ack;
    struct mesh_relay_result result = {0};
    uint8_t packet_payload[] = {0x44u};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;
    const struct route_candidate *selected;

    phase = "route-change-stale-parent-ack";
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet.payload_len = sizeof(packet_payload);
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    SOURCE_A_ID,
                    GATEWAY_ID,
                    1u);
    CHECK(route_upsert_candidate(&relay.upstream, &primary) == PROTO_OK &&
              route_upsert_candidate(&relay.upstream, &alternate) == PROTO_OK,
          "route candidates setup failed");
    selected = route_selected(&relay.upstream);
    CHECK(selected != NULL && selected->next_hop_id == SOURCE_B_ID,
          "primary parent was not selected");
    CHECK(mesh_relay_start_tx(&relay,
                              &packet,
                              packet_payload,
                              sizeof(packet_payload),
                              1000u,
                              &outbound) == PROTO_OK &&
              outbound.next_hop_id == SOURCE_B_ID,
          "primary-parent custody setup failed");
    mesh_relay_note_tx_sent(&relay, &outbound, 1000u);

    /* Four bounded parent-failure observations hold the old parent down and
     * move this exact pending packet to the alternate parent. */
    for (uint32_t failure = 1u; failure <= ROUTE_MAX_FAILURES; failure++) {
        CHECK(mesh_relay_note_pending_parent_failure(
                  &relay,
                  1100u + failure,
                  failure,
                  &result) == PROTO_OK,
              "parent failure %u was not admitted", failure);
    }
    selected = route_selected(&relay.upstream);
    CHECK(selected != NULL && selected->next_hop_id == ALT_PARENT_ID &&
              relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF &&
              relay.pending.next_hop_id == ALT_PARENT_ID,
          "route change did not transfer pending custody to alternate parent");
    pending_before_stale_ack = relay.pending;
    outbox_before_stale_ack = relay.outbox_record;

    CHECK(build_gateway_ack_for_packet(&packet,
                                       packet_payload,
                                       sizeof(packet_payload),
                                       80u,
                                       &gateway_ack,
                                       ack_payload,
                                       sizeof(ack_payload),
                                       &ack_payload_len) == PROTO_OK,
          "stale gateway ACK setup failed");
    CHECK(mesh_relay_handle_rx(&relay,
                               &gateway_ack,
                               ack_payload,
                               ack_payload_len,
                               SOURCE_B_ID,
                               95u,
                               1200u,
                               &result) == PROTO_OK,
          "stale parent ACK was not classified at the relay boundary");
    CHECK(result.actions == MESH_RELAY_ACTION_NONE &&
              pending_identity_unchanged(&relay.pending,
                                         &pending_before_stale_ack) &&
              relay.outbox_record.valid == outbox_before_stale_ack.valid &&
              relay.outbox_record.gateway_acked ==
                  outbox_before_stale_ack.gateway_acked &&
              relay.outbox_record.retry_round ==
                  outbox_before_stale_ack.retry_round &&
              relay.outbox_record.packet_id ==
                  outbox_before_stale_ack.packet_id &&
              memcmp(relay.outbox_record.semantic_digest,
                     outbox_before_stale_ack.semantic_digest,
                     sizeof(relay.outbox_record.semantic_digest)) == 0,
          "stale primary-parent ACK completed or mutated alternate-parent "
          "custody state=%d/%d next=0x%llx/0x%llx actions=0x%x",
          relay.pending.state,
          pending_before_stale_ack.state,
          (unsigned long long)relay.pending.next_hop_id,
          (unsigned long long)pending_before_stale_ack.next_hop_id,
          result.actions);
    CHECK(route_selected(&relay.upstream) != NULL &&
              route_selected(&relay.upstream)->next_hop_id == ALT_PARENT_ID,
          "stale primary-parent ACK changed the selected route");
}

int main(void)
{
    test_broadcast_queue_identity_is_valid();
    test_complete_frame_loss_is_explicit();
    test_ack_loss_does_not_drop_data();
    test_duplicate_delivery_is_idempotent();
    test_duplicate_relay_admission_is_coalesced();
    test_malformed_control_is_inert();
    test_same_seed_replays_delay_and_reordering();
    test_stale_relay_tick_cannot_mutate_new_operation();
    test_route_change_rejects_stale_parent_ack();

    if (failures != 0) {
        fprintf(stderr, "%d mesh simulator fault scenario(s) failed\n", failures);
        return 1;
    }
    puts("mesh simulator fault scenarios passed");
    return 0;
}
