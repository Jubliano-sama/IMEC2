#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_A_ID UINT64_C(0x000000000000a101)
#define SOURCE_B_ID UINT64_C(0x000000000000a102)
#define ALT_PARENT_ID UINT64_C(0x000000000000a103)
#define GATEWAY_ID UINT64_C(0x000000000000a1ff)

#define ROUTE_CHANGE_EPOCH UINT32_C(1)
#define DIRECT_GATEWAY_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_GATEWAY_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_GATEWAY_ACK_GUARD_US UINT64_C(10000)
#define DIRECT_GATEWAY_ACK_SERVICE_US UINT64_C(40000)
#define DIRECT_GATEWAY_RX_COMPLETION_GUARD_US UINT64_C(1)

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
    struct proto_packet ack;
    uint8_t source;
    uint8_t gateway;
    uint16_t ignored;

    phase = "ack-only-loss";
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
                                      NULL,
                                      0u,
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
    CHECK(world.roles[gateway].delivery_count == 1u,
          "data frame did not reach the gateway exactly once");
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
    CHECK(world.roles[gateway].delivery_count == 1u,
          "duplicate frame completed the semantic delivery %zu times",
          world.roles[gateway].delivery_count);
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
    uint8_t source;
    uint8_t relay;
    uint8_t gateway;
    uint16_t ignored;

    phase = "duplicate-relay-admission";
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
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
                                          &packet, NULL, 0u, &ignored) ==
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

static struct mesh_event_params route_change_connection_params(
    uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 500u,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static int run_connection_turn(struct mesh_sim_world *world,
                               uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world,
                                              connection_index,
                                              &action);

    if (ret != MESH_SIM_OK ||
        action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(world,
                                                      connection_index,
                                                      false);
    }
    return ret == MESH_SIM_OK ? mesh_sim_run_until(world, action.end_us) : ret;
}

static uint64_t transmission_arrival_end_us(
    const struct mesh_sim_world *world,
    uint16_t transmission_index,
    uint8_t receiver_index)
{
    const struct mesh_sim_transmission *transmission =
        &world->transmissions[transmission_index];
    uint64_t delay_us =
        world->propagation_us[transmission->node_index][receiver_index] +
        transmission->fault_extra_delay_us[receiver_index];

    return transmission->end_us > UINT64_MAX - delay_us ?
           UINT64_MAX : transmission->end_us + delay_us;
}

static int run_direct_gateway_turn(struct mesh_sim_world *world,
                                   uint8_t sender_index,
                                   uint8_t gateway_index)
{
    uint64_t ready_us = world->now_us;
    uint64_t air_start_us;
    uint64_t tx_deadline_us;
    uint64_t arrival_end_us;
    uint64_t rx_end_us;
    uint64_t ack_start_us;
    uint64_t ack_end_us;
    uint16_t transmission_index;
    int ret;

    if (world->roles[sender_index].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[sender_index].dwm3000.cpu_busy_until_us;
    }
    if (world->roles[gateway_index].dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = world->roles[gateway_index].dwm3000.cpu_busy_until_us;
    }
    if (ready_us > world->now_us) {
        ret = mesh_sim_run_until(world, ready_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    air_start_us = world->now_us + DIRECT_GATEWAY_TX_PREPARE_US;
    tx_deadline_us = air_start_us + DIRECT_GATEWAY_PAYLOAD_SERVICE_US;
    ret = mesh_sim_direct_gateway_start_queued_tx(
        world, sender_index, air_start_us, tx_deadline_us,
        &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    arrival_end_us = transmission_arrival_end_us(
        world, transmission_index, gateway_index);
    if (arrival_end_us == UINT64_MAX) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    rx_end_us = arrival_end_us + DIRECT_GATEWAY_RX_COMPLETION_GUARD_US;
    ret = mesh_sim_direct_gateway_arm_rx(world, gateway_index,
                                         air_start_us, rx_end_us);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, rx_end_us);
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->roles[gateway_index].dwm3000.cpu_busy_until_us >
            world->now_us) {
        ret = mesh_sim_run_until(
            world, world->roles[gateway_index].dwm3000.cpu_busy_until_us);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    ack_start_us = world->now_us + DIRECT_GATEWAY_ACK_GUARD_US;
    ack_end_us = ack_start_us + DIRECT_GATEWAY_ACK_SERVICE_US;
    ret = mesh_sim_direct_gateway_schedule_ack(
        world, gateway_index, sender_index, ack_start_us, ack_end_us, NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, ack_end_us);
    }
    return ret;
}

static bool queued_control_for_peer(
    const struct mesh_sim_role_instance *node,
    uint8_t msg_type,
    uint64_t peer_id)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            node->tx_queue[i].outbound.packet.msg_type == msg_type &&
            node->tx_queue[i].outbound.next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static int drain_connection_queue(struct mesh_sim_world *world,
                                  uint16_t connection_index,
                                  uint8_t queued_node_index)
{
    for (size_t turn = 0u; turn < 8u; turn++) {
        if (world->roles[queued_node_index].tx_queue_count == 0u) {
            return MESH_SIM_OK;
        }
        {
            int ret = run_connection_turn(world, connection_index);

            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static void test_route_change_rejects_stale_parent_ack(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_invariant_report invariant;
    struct mesh_pending_tx pending_before_stale_ack;
    struct persistent_outbox_record outbox_before_stale_ack;
    struct mesh_event_params params;
    struct proto_packet packet = data_packet(SOURCE_A_ID, 8u);
    const struct route_candidate *selected;
    uint64_t ack_deadline_us;
    uint64_t retry_ready_us;
    uint8_t retry_round_before_stale;
    uint8_t source;
    uint8_t parent_a;
    uint8_t parent_b;
    uint8_t gateway;
    uint16_t stale_parent_connection;
    uint16_t alternate_parent_connection;

    phase = "route-change-stale-parent-ack";
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    mesh_sim_init(&world, UINT32_C(0x5eedc057));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            SOURCE_A_ID, GATEWAY_ID, ROUTE_CHANGE_EPOCH,
                            &source) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                SOURCE_B_ID, GATEWAY_ID,
                                ROUTE_CHANGE_EPOCH, &parent_a) ==
                  MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                ALT_PARENT_ID, GATEWAY_ID,
                                ROUTE_CHANGE_EPOCH, &parent_b) ==
                  MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID, GATEWAY_ID,
                                ROUTE_CHANGE_EPOCH, &gateway) ==
                  MESH_SIM_OK,
          "route-change role setup failed");
    CHECK(mesh_sim_set_link(&world, source, parent_a, 100u, 1u) ==
              MESH_SIM_OK &&
              mesh_sim_set_link(&world, source, parent_b, 90u, 1u) ==
                  MESH_SIM_OK &&
              mesh_sim_set_link(&world, parent_a, gateway, 100u, 1u) ==
                  MESH_SIM_OK &&
              mesh_sim_set_link(&world, parent_b, gateway, 100u, 1u) ==
                  MESH_SIM_OK,
          "route-change links setup failed");
    CHECK(mesh_sim_install_route(&world, source, parent_a, 1u,
                                 ROUTE_CHANGE_EPOCH) == PROTO_OK &&
              mesh_sim_install_route(&world, source, parent_b, 1u,
                                     ROUTE_CHANGE_EPOCH) == PROTO_OK &&
              mesh_sim_install_route(&world, parent_a, gateway, 0u,
                                     ROUTE_CHANGE_EPOCH) == PROTO_OK &&
              mesh_sim_install_route(&world, parent_b, gateway, 0u,
                                     ROUTE_CHANGE_EPOCH) == PROTO_OK &&
              mesh_sim_install_downlink(&world, parent_a, SOURCE_A_ID,
                                        source, 1u,
                                        ROUTE_CHANGE_EPOCH) == MESH_SIM_OK &&
              mesh_sim_install_downlink(&world, parent_b, SOURCE_A_ID,
                                        source, 1u,
                                        ROUTE_CHANGE_EPOCH) == MESH_SIM_OK,
          "route-change production routes setup failed");
    selected = route_selected(&world.roles[source].relay.upstream);
    CHECK(selected != NULL && selected->next_hop_id == SOURCE_B_ID,
          "parent A was not the initial production route");

    params = route_change_connection_params(100u);
    params.event_interval_ms = 6000u;
    CHECK(mesh_sim_add_connection(&world, source, parent_a, &params, true,
                                  &stale_parent_connection) == MESH_SIM_OK &&
              mesh_sim_queue_originated(&world, source, &packet, NULL, 0u) ==
                  MESH_SIM_OK &&
              run_connection_turn(&world, stale_parent_connection) ==
                  MESH_SIM_OK,
          "initial parent-A radio delivery failed");
    CHECK(world.roles[source].relay.pending.state ==
              MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
              world.roles[source].relay.pending.next_hop_id == SOURCE_B_ID &&
              world.roles[parent_a].tx_queue_count == 2u,
          "initial parent-A custody was not retained state=%d next=0x%llx "
          "parent_queue=%zu parent_pending=%d deliveries=%zu rx=%zu "
          "outcome=%d tx=%zu",
          world.roles[source].relay.pending.state,
          (unsigned long long)world.roles[source].relay.pending.next_hop_id,
          world.roles[parent_a].tx_queue_count,
          world.roles[parent_a].relay.pending.state,
          world.roles[parent_a].delivery_count,
          world.reception_count,
          world.reception_count == 0u ? -1 :
              (int)world.receptions[world.reception_count - 1u].outcome,
          world.transmission_count);
    CHECK(run_direct_gateway_turn(&world, parent_a, gateway) == MESH_SIM_OK,
          "parent-A direct gateway turn failed");
    CHECK(world.roles[gateway].delivery_count == 1u &&
              queued_control_for_peer(&world.roles[parent_a],
                                      MSG_GATEWAY_ACK, SOURCE_A_ID),
          "gateway acceptance was not retained behind parent-A ACK custody "
          "deliveries=%zu parent_queue=%zu gateway_queue=%zu",
          world.roles[gateway].delivery_count,
          world.roles[parent_a].tx_queue_count,
          world.roles[gateway].tx_queue_count);

    ack_deadline_us =
        (uint64_t)world.roles[source].relay.pending.gateway_ack_deadline_ms *
        1000u;
    CHECK(mesh_sim_schedule_relay_tick(&world, source, ack_deadline_us) ==
              MESH_SIM_OK &&
              mesh_sim_run_until(&world, ack_deadline_us) == MESH_SIM_OK &&
              world.roles[source].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_RETRY_BACKOFF &&
              world.roles[source].relay.pending.next_hop_id == SOURCE_B_ID,
          "parent-A ACK loss did not enter bounded retry backoff");

    /*
     * Model a route-maintenance boundary invalidating the failing parent while
     * this exact packet remains in custody.  All four failure updates and the
     * alternate selection execute through the production route table.
     */
    for (size_t failure = 1u; failure < ROUTE_MAX_FAILURES; failure++) {
        mesh_relay_note_delivery_failure_at(
            &world.roles[source].relay, GATEWAY_ID,
            (uint32_t)(world.now_us / 1000u));
    }
    selected = route_selected(&world.roles[source].relay.upstream);
    CHECK(selected != NULL && selected->next_hop_id == ALT_PARENT_ID &&
              world.roles[source].relay.pending.next_hop_id == SOURCE_B_ID,
          "production route did not move from pending parent A to parent B");

    retry_ready_us =
        (uint64_t)world.roles[source].relay.pending.retry_after_ms * 1000u;
    CHECK(mesh_sim_schedule_relay_tick(&world, source, retry_ready_us) ==
              MESH_SIM_OK &&
              mesh_sim_run_until(&world, retry_ready_us) == MESH_SIM_OK &&
              world.roles[source].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
              world.roles[source].relay.pending.next_hop_id == ALT_PARENT_ID &&
              world.roles[source].tx_queue_count == 1u,
          "retry did not transfer the exact custody to parent B");
    pending_before_stale_ack = world.roles[source].relay.pending;
    outbox_before_stale_ack = world.roles[source].relay.outbox_record;
    retry_round_before_stale =
        world.roles[source].relay.outbox_record.retry_round;

    CHECK(run_connection_turn(&world, stale_parent_connection) == MESH_SIM_OK,
          "delayed parent-A gateway ACK was not injected");
    CHECK(world.roles[source].relay.pending.state ==
              pending_before_stale_ack.state &&
              world.roles[source].relay.pending.packet.session_id ==
                  pending_before_stale_ack.packet.session_id &&
              world.roles[source].relay.pending.packet.seq ==
                  pending_before_stale_ack.packet.seq &&
              world.roles[source].relay.pending.next_hop_id ==
                  pending_before_stale_ack.next_hop_id &&
              world.roles[source].relay.pending.gateway_ack_deadline_ms ==
                  pending_before_stale_ack.gateway_ack_deadline_ms &&
              world.roles[source].relay.outbox_record.retry_round ==
                  retry_round_before_stale &&
              memcmp(&world.roles[source].relay.outbox_record,
                     &outbox_before_stale_ack,
                     sizeof(outbox_before_stale_ack)) == 0 &&
              route_selected(&world.roles[source].relay.upstream) != NULL &&
              route_selected(&world.roles[source].relay.upstream)
                      ->next_hop_id == ALT_PARENT_ID &&
              mesh_sim_count_transitions(
                  &world, MESH_SIM_TRANSITION_GATEWAY_ACKED,
                  SOURCE_A_ID) == 0u,
          "stale parent-A ACK completed or mutated parent-B custody "
          "state=%d/%d session=%u/%u seq=%u/%u next=0x%llx/0x%llx "
          "deadline=%u/%u retry_round=%u/%u acked=%zu",
          world.roles[source].relay.pending.state,
          pending_before_stale_ack.state,
          world.roles[source].relay.pending.packet.session_id,
          pending_before_stale_ack.packet.session_id,
          world.roles[source].relay.pending.packet.seq,
          pending_before_stale_ack.packet.seq,
          (unsigned long long)world.roles[source].relay.pending.next_hop_id,
          (unsigned long long)pending_before_stale_ack.next_hop_id,
          world.roles[source].relay.pending.gateway_ack_deadline_ms,
          pending_before_stale_ack.gateway_ack_deadline_ms,
          world.roles[source].relay.outbox_record.retry_round,
          retry_round_before_stale,
          mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     SOURCE_A_ID));
    CHECK(world.roles[parent_a].relay.pending.state == MESH_RELAY_TX_IDLE &&
              !queued_control_for_peer(&world.roles[parent_a],
                                       MSG_GATEWAY_ACK, SOURCE_A_ID),
          "old parent did not release its delayed forwarded ACK");

    params = route_change_connection_params(
        (uint32_t)(world.now_us / 1000u) + 50u);
    CHECK(mesh_sim_add_connection(&world, source, parent_b, &params, true,
                                  &alternate_parent_connection) ==
              MESH_SIM_OK &&
              run_connection_turn(&world, alternate_parent_connection) ==
                  MESH_SIM_OK,
          "parent-B retry radio delivery failed");
    CHECK(world.roles[parent_b].tx_queue_count == 2u,
          "parent B did not retain one forward and one hop ACK");
    CHECK(run_direct_gateway_turn(&world, parent_b, gateway) == MESH_SIM_OK,
          "parent-B direct gateway turn failed");
    CHECK(world.roles[gateway].delivery_count == 1u,
          "alternate route duplicated the semantic gateway delivery");
    CHECK(run_connection_turn(&world, alternate_parent_connection) ==
              MESH_SIM_OK &&
              world.roles[source].relay.pending.state ==
                  MESH_RELAY_TX_IDLE &&
              mesh_sim_count_transitions(
                  &world, MESH_SIM_TRANSITION_GATEWAY_ACKED,
                  SOURCE_A_ID) == 1u,
          "parent-B gateway ACK did not complete exactly once");

    CHECK(drain_connection_queue(&world, stale_parent_connection,
                                 parent_a) == MESH_SIM_OK &&
              drain_connection_queue(&world, alternate_parent_connection,
                                     parent_b) == MESH_SIM_OK,
          "post-operation ACK queues did not drain within eight turns");
    CHECK(mesh_sim_count_transitions(
              &world, MESH_SIM_TRANSITION_RETRY_READY, SOURCE_A_ID) == 1u,
          "route change produced an unbounded or missing retry");
    CHECK(mesh_sim_check_settled(&world, &invariant) == MESH_SIM_OK,
          "route-change world did not settle code=%d node=%zu detail=%llu",
          invariant.code, invariant.node_index,
          (unsigned long long)invariant.detail);
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
