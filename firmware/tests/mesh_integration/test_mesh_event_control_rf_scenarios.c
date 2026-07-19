#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include "mesh.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LEAF_ID UINT64_C(0x4100000000000001)
#define RELAY_A_ID UINT64_C(0x4100000000000002)
#define RELAY_B_ID UINT64_C(0x4100000000000003)
#define GATEWAY_ID UINT64_C(0x4100000000000004)
#define ROUTE_EPOCH UINT32_C(73)
#define RX_GUARD_US UINT64_C(100)
#define CONTROL_GAP_US UINT64_C(500)
#define CONTROL_PAYLOAD_CAPACITY 128u
#define EXPECTED_SCENARIO_TRANSMISSIONS 26u
#define MAX_SCENARIO_TRANSITIONS 256u

#define CHECK(expression) do {                                               \
    if (!(expression)) {                                                     \
        fprintf(stderr, "event-control phase=%s line=%d assertion=%s\n",  \
                phase, __LINE__, #expression);                               \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char *phase = "setup";

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .first_event_time_ms = first_event_ms,
        .event_interval_ms = 250u,
        .event_window_ms = 80u,
        .guard_ms = 20u,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 1500u,
    };
}

static uint16_t sequence_after(uint16_t sequence, uint16_t delta)
{
    uint16_t next = (uint16_t)(sequence + delta);

    return next == 0u ? 1u : next;
}

static uint64_t endpoint_ready_us(const struct mesh_sim_role_instance *node)
{
    uint64_t ready_us = node->runtime.radio_busy_until_us;

    if (node->dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = node->dwm3000.cpu_busy_until_us;
    }
    if (node->dwm3000.spi_busy_until_us > ready_us) {
        ready_us = node->dwm3000.spi_busy_until_us;
    }
    if (node->dwm3000.radio_busy_until_us > ready_us) {
        ready_us = node->dwm3000.radio_busy_until_us;
    }
    return ready_us;
}

static bool timing_equal(const struct mesh_event_timing *a,
                         const struct mesh_event_timing *b)
{
    return a->mesh_channel == b->mesh_channel &&
           a->event_interval_ms == b->event_interval_ms &&
           a->event_window_ms == b->event_window_ms &&
           a->next_event_time_ms == b->next_event_time_ms &&
           a->event_counter == b->event_counter &&
           a->guard_ms == b->guard_ms &&
           a->peer_clock_skew_estimate_ppm ==
               b->peer_clock_skew_estimate_ppm &&
           a->max_missed_events == b->max_missed_events &&
           a->missed_event_count == b->missed_event_count &&
           a->supervision_timeout_ms == b->supervision_timeout_ms &&
           a->last_successful_ch9_event_ms ==
               b->last_successful_ch9_event_ms &&
           a->local_tx_on_even_events == b->local_tx_on_even_events &&
           a->route_fresh == b->route_fresh &&
           a->timing_fresh == b->timing_fresh &&
           a->fallback_required == b->fallback_required;
}

static int run_scheduled_connection_exchange(struct mesh_sim_world *world,
                                             uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world, connection_index,
                                              &action);

    if (ret != MESH_SIM_OK ||
        action.kind != MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR ||
        !action.already_scheduled) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int append_update_payload(const struct mesh_sim_world *world,
                                 const struct mesh_event_timing *base,
                                 uint32_t event_counter,
                                 uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *payload_len)
{
    struct mesh_event_timing timing = *base;
    uint32_t now_ms = (uint32_t)(world->now_us / 1000u);

    timing.event_counter = event_counter;
    timing.next_event_time_ms = now_ms + 1000u + event_counter;
    timing.missed_event_count = 0u;
    timing.last_successful_ch9_event_ms = now_ms;
    timing.route_fresh = true;
    timing.timing_fresh = true;
    timing.fallback_required = false;
    *payload_len = 0u;
    return mesh_append_event_timing_tlvs_at(payload, payload_cap, payload_len,
                                            &timing, now_ms);
}

static int send_control_over_radio(struct mesh_sim_world *world,
                                   uint16_t connection_index,
                                   uint8_t sender_index,
                                   uint8_t message_type,
                                   uint32_t session_id,
                                   uint16_t sequence,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    const struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    struct proto_packet packet;
    uint8_t receiver_index;
    uint64_t tx_start_us;
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    uint32_t airtime_us;
    uint16_t transmission_index;
    int ret;

    if (sender_index == connection->node_a) {
        receiver_index = connection->node_b;
    } else if (sender_index == connection->node_b) {
        receiver_index = connection->node_a;
    } else {
        return MESH_SIM_ERR_ARG;
    }
    if (payload_len > UINT8_MAX) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_init_event_control(&packet, message_type,
                                  world->roles[sender_index].id,
                                  world->roles[receiver_index].id,
                                  session_id, sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proto_packet_encoded_len(payload_len));
    if (airtime_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }
    tx_start_us = endpoint_ready_us(&world->roles[sender_index]);
    if (endpoint_ready_us(&world->roles[receiver_index]) > tx_start_us) {
        tx_start_us = endpoint_ready_us(&world->roles[receiver_index]);
    }
    if (world->now_us > tx_start_us) {
        tx_start_us = world->now_us;
    }
    tx_start_us += CONTROL_GAP_US;
    arrival_start_us = tx_start_us +
        world->propagation_us[sender_index][receiver_index];
    arrival_end_us = arrival_start_us + airtime_us;
    ret = mesh_sim_schedule_rx(world, receiver_index,
                               arrival_start_us - RX_GUARD_US,
                               arrival_end_us + RX_GUARD_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, NULL);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "control RX schedule ret=%d last=%d now=%llu\n",
                ret, world->last_error,
                (unsigned long long)world->now_us);
        return ret;
    }
    ret = mesh_sim_schedule_packet_tx(world, sender_index, tx_start_us,
                                      UWB_CHANNEL_WAKE_CONTACT,
                                      MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                      &packet, payload, payload_len,
                                      &transmission_index);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "control TX schedule ret=%d last=%d now=%llu msg=%u\n",
                ret, world->last_error,
                (unsigned long long)world->now_us, message_type);
        return ret;
    }
    ret = mesh_sim_run_until(
        world, world->transmissions[transmission_index].end_us +
                   world->propagation_us[sender_index][receiver_index] +
                   RX_GUARD_US + 1u);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "control RF run ret=%d last=%d file=%s line=%u msg=%u\n",
                ret, world->last_error,
                world->last_error_file == NULL ? "?" : world->last_error_file,
                world->last_error_line, message_type);
    }
    return ret;
}

static bool connection_owners_active(const struct mesh_sim_connection *connection)
{
    return connection->owner_a.active && connection->owner_b.active &&
           connection->owner_a.session_id == connection->owner_b.session_id;
}

static bool connection_owners_inactive(const struct mesh_sim_connection *connection)
{
    return !connection->owner_a.active && !connection->owner_b.active;
}

static int run_event_control_scenario(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_invariant_report report;
    struct mesh_sim_fault_config faults = {0};
    struct mesh_sim_fault_stats fault_stats;
    struct mesh_event_params params;
    struct mesh_event_timing snapshot_a;
    struct mesh_event_timing snapshot_b;
    uint8_t valid_payload[CONTROL_PAYLOAD_CAPACITY];
    const uint8_t malformed_payload[] = {
        TLV_MESH_EVENT_INTERVAL_MS, 4u, 0x01u,
    };
    size_t valid_payload_len;
    uint64_t selected_next_hop = 0u;
    uint64_t expiry_us;
    uint32_t session_n;
    uint32_t session_n1;
    uint32_t session_before_reset;
    uint32_t generation_n1_a;
    uint32_t generation_n1_b;
    uint16_t seq_n;
    uint16_t seq_n1;
    uint16_t seq_bc;
    uint16_t leaf_proposal_seq;
    uint16_t relay_proposal_seq;
    uint16_t connection_ab;
    uint16_t connection_bc;
    uint8_t expired_endpoints = 0u;
    uint8_t leaf;
    uint8_t relay_a;
    uint8_t relay_b;
    uint8_t gateway;

    mesh_sim_init(&world, UINT32_C(0x7ea4d91b));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &leaf) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay_a) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_B_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay_b) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, leaf, relay_a, 96u, 4u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_a, relay_b, 95u, 6u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_b, gateway, 97u, 3u) == MESH_SIM_OK);
    CHECK(!world.reachable[leaf][relay_b]);
    CHECK(!world.reachable[leaf][gateway]);
    CHECK(!world.reachable[relay_a][gateway]);
    CHECK(mesh_sim_install_route(&world, leaf, relay_a, 2u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world, relay_a, relay_b, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world, relay_b, gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);

    phase = "forced_multihop_radio_negotiation";
    params = connection_params(5000u);
    CHECK(mesh_sim_add_connection_over_radio(&world, leaf, relay_a, &params,
                                             true, &connection_ab) ==
          MESH_SIM_OK);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    leaf_proposal_seq = world.connections[connection_ab].repair_seq;
    CHECK(leaf_proposal_seq != 0u);
    CHECK(world.roles[leaf].event_control_seq == leaf_proposal_seq);
    CHECK(world.transmission_count == 2u);
    params = connection_params((uint32_t)(world.now_us / 1000u) + 5000u);
    CHECK(mesh_sim_add_connection_over_radio(&world, relay_a, relay_b, &params,
                                             true, &connection_bc) ==
          MESH_SIM_OK);
    CHECK(run_scheduled_connection_exchange(&world, connection_bc) ==
          MESH_SIM_OK);
    relay_proposal_seq = world.connections[connection_bc].repair_seq;
    CHECK(relay_proposal_seq == leaf_proposal_seq);
    CHECK(world.roles[relay_a].event_control_seq == relay_proposal_seq);
    CHECK(world.transmission_count == 4u);
    CHECK(connection_owners_active(&world.connections[connection_ab]));
    CHECK(connection_owners_active(&world.connections[connection_bc]));
    CHECK(world.connections[connection_ab].repair_propose_decoded);
    CHECK(world.connections[connection_ab].repair_accept_decoded);
    CHECK(world.connections[connection_bc].repair_propose_decoded);
    CHECK(world.connections[connection_bc].repair_accept_decoded);

    phase = "independent_sequence_domains_and_duplicate";
    session_n = world.connections[connection_ab].owner_a.session_id;
    seq_n = sequence_after(world.connections[connection_ab].repair_seq,
                           UINT16_C(0x4000));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                101u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n, seq_n,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_control_seen);
    CHECK(world.connections[connection_ab].owner_b.remote_control_seen);
    CHECK(world.connections[connection_ab].owner_a.local_sequence == seq_n);
    CHECK(world.connections[connection_ab].owner_b.remote_sequence == seq_n);
    CHECK(mesh_event_timing_local_tx_slot(
              &world.connections[connection_ab].timing_a) !=
          mesh_event_timing_local_tx_slot(
              &world.connections[connection_ab].timing_b));
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n, seq_n,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));

    phase = "reordered_and_malformed_controls";
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                103u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 3u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 2u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 4u),
                                  malformed_payload,
                                  sizeof(malformed_payload)) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_sequence ==
          sequence_after(seq_n, 3u));
    CHECK(world.connections[connection_ab].owner_b.remote_sequence ==
          sequence_after(seq_n, 3u));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                104u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 4u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_sequence ==
          sequence_after(seq_n, 4u));
    CHECK(world.connections[connection_ab].owner_b.remote_sequence ==
          sequence_after(seq_n, 4u));

    phase = "route_change_and_operation_n_plus_one";
    CHECK(mesh_sim_set_link(&world, leaf, relay_b, 99u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world, leaf, relay_b, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_relay_select_next_hop(&world.roles[leaf].relay, GATEWAY_ID,
                                     &selected_next_hop) == PROTO_OK);
    CHECK(selected_next_hop == RELAY_B_ID);
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    leaf_proposal_seq = sequence_after(leaf_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == leaf_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    session_n1 = world.connections[connection_ab].owner_a.session_id;
    CHECK(session_n1 != session_n);
    CHECK(world.connections[connection_ab].owner_b.session_id == session_n1);
    generation_n1_a = world.connections[connection_ab].owner_a.generation;
    generation_n1_b = world.connections[connection_ab].owner_b.generation;
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 5u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n,
                                  sequence_after(seq_n, 6u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(connection_owners_active(&world.connections[connection_ab]));
    CHECK(world.connections[connection_ab].owner_a.generation ==
          generation_n1_a);
    CHECK(world.connections[connection_ab].owner_b.generation ==
          generation_n1_b);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));

    phase = "matching_end_is_single_terminal_transition";
    seq_n1 = sequence_after(world.connections[connection_ab].repair_seq,
                            UINT16_C(0x4000));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                201u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n1, seq_n1,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1,
                                  sequence_after(seq_n1, 1u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    CHECK(world.connections[connection_ab].owner_a.terminal);
    CHECK(world.connections[connection_ab].owner_b.terminal);
    generation_n1_a = world.connections[connection_ab].owner_a.generation;
    generation_n1_b = world.connections[connection_ab].owner_b.generation;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1,
                                  sequence_after(seq_n1, 1u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.generation ==
          generation_n1_a);
    CHECK(world.connections[connection_ab].owner_b.generation ==
          generation_n1_b);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));

    phase = "opposite_requester_uses_its_own_sequence_domain";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, relay_a) == MESH_SIM_OK);
    relay_proposal_seq = sequence_after(relay_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == relay_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.proposal_from_peer);
    CHECK(!world.connections[connection_ab].owner_b.proposal_from_peer);
    CHECK(connection_owners_active(&world.connections[connection_ab]));

    phase = "reset_recovery_rejects_old_owner";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    leaf_proposal_seq = sequence_after(leaf_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == leaf_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    session_before_reset = world.connections[connection_ab].owner_a.session_id;
    CHECK(mesh_sim_reset_role(&world, leaf) == MESH_SIM_OK);
    CHECK(world.roles[leaf].event_control_seq == 0u);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    CHECK(!mesh_event_timing_usable(
              &world.connections[connection_ab].timing_a,
              (uint32_t)(world.now_us / 1000u)));
    CHECK(!mesh_event_timing_usable(
              &world.connections[connection_ab].timing_b,
              (uint32_t)(world.now_us / 1000u)));
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_before_reset,
                                  UINT16_C(0x7111), NULL, 0u) == MESH_SIM_OK);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].repair_seq == 1u);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.session_id !=
          session_before_reset);

    phase = "lost_end_expires_orphan_within_supervision_bound";
    faults.seed = UINT32_C(0x51a1e0d5);
    faults.frame_loss_permyriad = MESH_SIM_FAULT_RATE_SCALE;
    CHECK(mesh_sim_set_fault_config(&world, &faults) == MESH_SIM_OK);
    session_n1 = world.connections[connection_ab].owner_a.session_id;
    seq_n1 = sequence_after(world.connections[connection_ab].repair_seq,
                            UINT16_C(0x4000));
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1, seq_n1,
                                  NULL, 0u) == MESH_SIM_OK);
    CHECK(!world.connections[connection_ab].owner_a.active);
    CHECK(world.connections[connection_ab].owner_a.terminal);
    CHECK(world.connections[connection_ab].owner_b.active);
    if (mesh_sim_check_invariants(&world, &report) != MESH_SIM_OK) {
        fprintf(stderr, "lost-END invariant=%s detail=%llu description=%s\n",
                mesh_sim_invariant_name(report.code),
                (unsigned long long)report.detail,
                report.description == NULL ? "?" : report.description);
        return 1;
    }
    CHECK(mesh_sim_get_fault_stats(&world, &fault_stats) == MESH_SIM_OK);
    CHECK(fault_stats.frame_losses == 1u);
    expiry_us = ((uint64_t)
        world.connections[connection_ab].timing_b.last_successful_ch9_event_ms +
        world.connections[connection_ab].timing_b.supervision_timeout_ms + 1u) *
        1000u;
    if (expiry_us > world.now_us) {
        CHECK(mesh_sim_run_until(&world, expiry_us) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_expire_connection_ownership(
              &world, connection_ab, &expired_endpoints) == MESH_SIM_OK);
    CHECK(expired_endpoints == 1u);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    memset(&faults, 0, sizeof(faults));
    CHECK(mesh_sim_set_fault_config(&world, &faults) == MESH_SIM_OK);

    phase = "second_hop_teardown_and_bounded_idle";
    seq_bc = sequence_after(world.connections[connection_bc].repair_seq,
                            UINT16_C(0x4000));
    CHECK(send_control_over_radio(
              &world, connection_bc, relay_a, MSG_MESH_EVENT_END,
              world.connections[connection_bc].owner_a.session_id,
              seq_bc, NULL, 0u) == MESH_SIM_OK);
    CHECK(connection_owners_inactive(&world.connections[connection_bc]));
    CHECK(world.transmission_count == EXPECTED_SCENARIO_TRANSMISSIONS);
    CHECK(world.transition_count <= MAX_SCENARIO_TRANSITIONS);
    CHECK(mesh_sim_count_transitions(&world,
              MESH_SIM_TRANSITION_ROUTE_REQUIRED, 0u) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
              MESH_SIM_TRANSITION_RETRY_READY, 0u) == 0u);
    for (size_t i = 0u; i < world.role_count; i++) {
        CHECK(world.roles[i].tx_queue_count == 0u);
        CHECK(world.roles[i].watchdog.workers_active == 0u);
        CHECK(world.roles[i].radio_state == MESH_SIM_RADIO_SLEEP);
    }
    CHECK(mesh_sim_check_settled(&world, &report) == MESH_SIM_OK);
    CHECK(report.code == MESH_SIM_INVARIANT_NONE);
    return 0;
}

static int run_event_control_sequence_wrap_scenario(void)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params = connection_params(5000u);
    uint16_t connection;
    uint8_t leaf;
    uint8_t relay;

    phase = "event_control_sequence_wrap_setup";
    mesh_sim_init(&world, UINT32_C(0x5e91a7e1));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &leaf) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, leaf, relay, 100u, 4u) == MESH_SIM_OK);

    /* add_connection_over_radio first builds, then replaces, its local model. */
    world.roles[leaf].event_control_seq = UINT16_MAX - 2u;
    CHECK(mesh_sim_add_connection_over_radio(&world, leaf, relay, &params,
                                             true, &connection) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection].repair_seq == UINT16_MAX);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection].owner_b.remote_proposal_sequence ==
          UINT16_MAX);

    phase = "event_control_sequence_wrap_skips_zero";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection, leaf) == MESH_SIM_OK);
    CHECK(world.connections[connection].repair_seq == 1u);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    CHECK(world.roles[leaf].event_control_seq == 1u);
    CHECK(world.connections[connection].owner_b.remote_proposal_sequence == 1u);
    CHECK(connection_owners_active(&world.connections[connection]));
    return 0;
}

int main(void)
{
    int ret = run_event_control_scenario();

    return ret == 0 ? run_event_control_sequence_wrap_scenario() : ret;
}
