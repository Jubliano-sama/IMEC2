#include "mesh_sim.h"

#include "report.h"
#include "route.h"
#include "uwb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_ID UINT32_C(0x10203040)
#define ROUTE_EPOCH UINT32_C(7)
#define GATEWAY_ID UINT64_C(0x9000)
#define TRANSMITTER_ID UINT64_C(0xB001)
#define CLICKER_ID UINT64_C(0xC001)
#define ANCHOR_1_ID UINT64_C(0xA001)
#define ANCHOR_2_ID UINT64_C(0xA002)
#define ANCHOR_3_ID UINT64_C(0xA003)
#define ANCHOR_4_ID UINT64_C(0xA004)

static struct mesh_sim_world world;

static struct mesh_event_params connection_params(uint32_t first_event_ms,
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

static struct proto_packet mesh_data_packet(uint16_t seq,
                                            uint32_t session_id,
                                            uint8_t ttl,
                                            uint16_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = TRANSMITTER_ID,
        .dst_id = GATEWAY_ID,
        .session_id = session_id,
        .seq = seq,
        .ttl = ttl,
        .payload_len = payload_len,
    };
}

static size_t mesh_data_payload(uint8_t *payload,
                                size_t payload_cap,
                                uint32_t packet_id)
{
    size_t payload_len = 0u;

    assert(tlv_append_u32(payload,
                          payload_cap,
                          &payload_len,
                          TLV_MESH_TEST_PACKET_ID,
                          packet_id) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         payload_cap,
                         &payload_len,
                         TLV_MESH_TEST_ATTEMPT,
                         1u) == PROTO_OK);
    return payload_len;
}

static uint64_t run_next_connection(struct mesh_sim_world *sim,
                                    uint16_t connection_index,
                                    bool receiver_preempted)
{
    struct mesh_sim_connection_action action;
    int ret;

    ret = mesh_sim_connection_next_action(sim, connection_index, &action);
    assert(ret == MESH_SIM_OK);
    ret = action.already_scheduled ? MESH_SIM_OK :
          mesh_sim_schedule_next_connection_event(sim,
                                                  connection_index,
                                                  receiver_preempted);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "connection schedule failed: conn=%u start_ms=%u now_us=%llu ret=%d last=%d\n",
                connection_index,
                (unsigned int)(action.start_us / 1000u),
                (unsigned long long)sim->now_us,
                ret,
                sim->last_error);
    }
    assert(ret == MESH_SIM_OK);
    assert(mesh_sim_run_until(sim, action.end_us) == MESH_SIM_OK);
    return action.end_us;
}

static uint16_t add_connection(struct mesh_sim_world *sim,
                               uint8_t node_a,
                               uint8_t node_b,
                               uint32_t first_event_ms,
                               uint32_t interval_ms,
                               bool node_a_transmits_first)
{
    struct mesh_event_params params = connection_params(first_event_ms, interval_ms);
    uint16_t connection_index = UINT16_MAX;

    assert(mesh_sim_add_connection(sim,
                                   node_a,
                                   node_b,
                                   &params,
                                   node_a_transmits_first,
                                   &connection_index) == MESH_SIM_OK);
    assert(connection_index != UINT16_MAX);
    return connection_index;
}

static void assert_no_route_fallback(const struct mesh_sim_world *sim)
{
    assert(sim->last_error == MESH_SIM_OK);
    for (size_t i = 0u; i < sim->role_count; i++) {
        assert(sim->roles[i].route_discovery_requests == 0u);
    }
    assert(mesh_sim_count_transitions(sim,
                                      MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                      0u) == 0u);
}

static size_t count_transitions_for_message(
    const struct mesh_sim_world *sim,
    enum mesh_sim_transition_kind kind,
    uint64_t node_id,
    uint8_t msg_type)
{
    size_t count = 0u;

    for (size_t i = 0u; i < sim->transition_count; i++) {
        const struct mesh_sim_transition *transition = &sim->transitions[i];

        if (transition->kind == kind && transition->node_id == node_id &&
            transition->msg_type == msg_type) {
            count++;
        }
    }
    return count;
}

static void run_connections_until_confirmed(struct mesh_sim_world *sim,
                                             const uint16_t *connections,
                                             size_t connection_count,
                                             uint8_t transmitter_index,
                                             size_t max_events);

static void test_single_relay_delivery(void)
{
    uint8_t transmitter;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t child_connection;
    uint16_t gateway_connection;
    uint16_t connections[2];
    uint8_t payload[32];
    size_t payload_len;
    struct proto_packet packet;
    const struct mesh_sim_transition *delivery_transition;

    mesh_sim_init(&world, 0x11111111u);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_TRANSMITTER,
                             TRANSMITTER_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &transmitter) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_GATEWAY,
                             GATEWAY_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &gateway) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, transmitter, anchor, 96u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, anchor, gateway, 98u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_install_route(&world,
                                  transmitter,
                                  anchor,
                                  1u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_route(&world,
                                  anchor,
                                  gateway,
                                  0u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_downlink(&world,
                                     anchor,
                                     TRANSMITTER_ID,
                                     transmitter,
                                     1u,
                                     ROUTE_EPOCH) == MESH_SIM_OK);

    child_connection = add_connection(&world,
                                      transmitter,
                                      anchor,
                                      100u,
                                      150u,
                                      true);
    gateway_connection = add_connection(&world,
                                        anchor,
                                        gateway,
                                        200u,
                                        150u,
                                        true);
    connections[0] = child_connection;
    connections[1] = gateway_connection;
    payload_len = mesh_data_payload(payload, sizeof(payload), 1u);
    packet = mesh_data_packet(1u, 0x1001u, 4u, (uint16_t)payload_len);
    assert(mesh_sim_queue_originated(&world,
                                     transmitter,
                                     &packet,
                                     payload,
                                     payload_len) == MESH_SIM_OK);

    assert(mesh_sim_schedule_next_connection_event(&world,
                                                   child_connection,
                                                   false) == MESH_SIM_OK);
    assert(mesh_sim_run(&world) == MESH_SIM_OK);
    assert(world.now_us == 125000u);
    assert(world.roles[anchor].tx_queue_count == 2u);
    run_next_connection(&world, gateway_connection, false);
    assert(world.roles[gateway].delivery_count == 1u);
    assert(world.roles[gateway].deliveries[0].packet.msg_type == MSG_MESH_DATA);
    assert(world.roles[gateway].deliveries[0].packet.ttl == 3u);
    assert(world.roles[gateway].deliveries[0].previous_hop_id == ANCHOR_1_ID);

    run_connections_until_confirmed(&world,
                                    connections,
                                    2u,
                                    transmitter,
                                    24u);

    assert(world.roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE);
    assert(world.roles[gateway].delivery_count == 1u);
    assert(count_transitions_for_message(&world,
                                         MESH_SIM_TRANSITION_TX_START,
                                         TRANSMITTER_ID,
                                         MSG_GATEWAY_ACK_CONFIRM) >= 1u);
    assert(mesh_sim_count_transitions(&world,
                                      MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                      TRANSMITTER_ID) == 1u);
    delivery_transition = mesh_sim_find_transition(
        &world,
        MESH_SIM_TRANSITION_PACKET_DELIVERED,
        GATEWAY_ID,
        0u);
    assert(delivery_transition != NULL);
    assert(delivery_transition->time_us > 200000u);
    assert(delivery_transition->time_us < 225000u);
    assert(world.connections[child_connection].completed_events >= 4u);
    assert(world.connections[gateway_connection].completed_events >= 2u);
    assert_no_route_fallback(&world);
}

static void test_two_relay_delivery(void)
{
    uint8_t transmitter;
    uint8_t anchor_1;
    uint8_t anchor_2;
    uint8_t gateway;
    uint16_t connection_0;
    uint16_t connection_1;
    uint16_t connection_2;
    uint16_t connections[3];
    uint8_t payload[32];
    size_t payload_len;
    struct proto_packet packet;

    mesh_sim_init(&world, 0x22222222u);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                             TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &transmitter) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &anchor_1) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_2_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &anchor_2) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                             GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &gateway) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, transmitter, anchor_1, 95u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, anchor_1, anchor_2, 94u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, anchor_2, gateway, 97u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_install_route(&world, transmitter, anchor_1, 2u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_route(&world, anchor_1, anchor_2, 1u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_route(&world, anchor_2, gateway, 0u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_downlink(&world, anchor_1, TRANSMITTER_ID,
                                     transmitter, 1u,
                                     ROUTE_EPOCH) == MESH_SIM_OK);
    assert(mesh_sim_install_downlink(&world, anchor_2, TRANSMITTER_ID,
                                     anchor_1, 2u,
                                     ROUTE_EPOCH) == MESH_SIM_OK);

    connection_0 = add_connection(&world, transmitter, anchor_1,
                                  100u, 300u, true);
    connection_1 = add_connection(&world, anchor_1, anchor_2,
                                  200u, 300u, true);
    connection_2 = add_connection(&world, anchor_2, gateway,
                                  300u, 300u, true);
    connections[0] = connection_0;
    connections[1] = connection_1;
    connections[2] = connection_2;
    payload_len = mesh_data_payload(payload, sizeof(payload), 2u);
    packet = mesh_data_packet(2u, 0x1002u, MESH_DEFAULT_TTL,
                              (uint16_t)payload_len);
    assert(mesh_sim_queue_originated(&world, transmitter, &packet,
                                     payload, payload_len) == MESH_SIM_OK);

    run_next_connection(&world, connection_0, false);
    run_next_connection(&world, connection_1, false);
    run_next_connection(&world, connection_2, false);

    run_connections_until_confirmed(&world,
                                    connections,
                                    3u,
                                    transmitter,
                                    48u);

    assert(world.roles[transmitter].relay.pending.state == MESH_RELAY_TX_IDLE);
    assert(world.roles[gateway].delivery_count == 1u);
    assert(world.roles[gateway].deliveries[0].packet.ttl == 2u);
    assert(world.roles[gateway].deliveries[0].previous_hop_id == ANCHOR_2_ID);
    assert(count_transitions_for_message(&world,
                                         MESH_SIM_TRANSITION_TX_START,
                                         TRANSMITTER_ID,
                                         MSG_GATEWAY_ACK_CONFIRM) >= 1u);
    assert(mesh_sim_count_transitions(&world,
                                      MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                      TRANSMITTER_ID) == 1u);
    assert(world.roles[gateway].deliveries[0].delivered_at_us < 325000u);
    assert_no_route_fallback(&world);
}

static void complete_normal_click(struct mesh_sim_role_instance *clicker,
                                  const uint64_t *anchor_ids,
                                  size_t anchor_count)
{
    struct uwb_wake_claim_frame claim;
    struct uwb_discover_frame discover;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    size_t completed = 0u;

    assert(clicker->clicker_session.state == UWB_CLICKER_POLITENESS);
    assert(uwb_clicker_build_wake_claim(&clicker->clicker_session,
                                        CLICKER_ID,
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_OK);
    assert(clicker->clicker_session.state == UWB_CLICKER_WAKE);
    assert(uwb_clicker_build_discover(&clicker->clicker_session,
                                      &discover) == PROTO_OK);
    assert(clicker->clicker_session.state == UWB_CLICKER_DISCOVERY);
    for (size_t i = 0u; i < anchor_count; i++) {
        assert(uwb_clicker_seed_discovered_anchor(&clicker->clicker_session,
                                                  anchor_ids[i],
                                                  (uint8_t)i,
                                                  (uint8_t)(100u - i)) == PROTO_OK);
    }
    assert(uwb_clicker_build_range_schedule(&clicker->clicker_session,
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            5u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == clicker->clicker_session.config.min_anchor_count);
    while (uwb_clicker_next_range_step(&clicker->clicker_session, &step) ==
           PROTO_OK) {
        assert(uwb_clicker_record_range_result(&clicker->clicker_session,
                                               &step,
                                               RANGE_OK) == PROTO_OK);
        completed++;
    }
    assert(completed == schedule.selected_count);
    assert(clicker->clicker_session.state == UWB_CLICKER_SUCCEEDED);
    assert(clicker->clicker_session.successful_unique_count == schedule.selected_count);
}

static size_t build_click_report(uint8_t *payload,
                                 size_t payload_cap,
                                 struct proto_packet *packet)
{
    const int32_t distance_samples_mm[] = {1234};
    const uint8_t range_round_indices[] = {0u};
    const uint64_t sequence_start_timestamps_ms[] = {100u};
    const struct range_report_fields fields = {
        .clicker_id = CLICKER_ID,
        .anchor_id = ANCHOR_1_ID,
        .event_seq = UINT32_C(0x2001),
        .timestamp_ms = 100u,
        .distance_mm = 1234,
        .quality = 99u,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples_mm,
        .range_round_indices = range_round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 1u,
        .distance_sample_count = 1u,
        .burst_id = UINT32_C(0x2001),
        .omit_rsl = true,
        .omit_cir = true,
        .burst_id_present = true,
    };
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload,
                                    payload_cap,
                                    &payload_len,
                                    &fields) == PROTO_OK);
    assert(payload_len <= UINT8_MAX);
    assert(report_init_click_packet(packet,
                                    ANCHOR_1_ID,
                                    GATEWAY_ID,
                                    proto_click_report_session_id(
                                        fields.clicker_id,
                                        fields.event_seq),
                                    1u,
                                    (uint8_t)payload_len) == PROTO_OK);
    assert(report_validate_click_payload(packet, payload, payload_len) ==
           PROTO_OK);
    return payload_len;
}

static uint64_t earliest_connection_start(const struct mesh_sim_world *sim,
                                          const uint16_t *connections,
                                          size_t connection_count,
                                          size_t *which)
{
    uint64_t earliest = UINT64_MAX;

    *which = SIZE_MAX;
    for (size_t i = 0u; i < connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(sim,
                                                  connections[i],
                                                  &action);

        if (ret != MESH_SIM_OK) {
            fprintf(stderr,
                    "connection action failed: conn=%u now_us=%llu ret=%d\n",
                    connections[i],
                    (unsigned long long)sim->now_us,
                    ret);
        }
        assert(ret == MESH_SIM_OK);
        if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
            continue;
        }
        if (action.start_us < earliest) {
            earliest = action.start_us;
            *which = i;
        }
    }
    return earliest;
}

static void run_connections_before(struct mesh_sim_world *sim,
                                   const uint16_t *connections,
                                   size_t connection_count,
                                   uint64_t deadline_us)
{
    while (true) {
        size_t which;
        uint64_t next_us = earliest_connection_start(sim,
                                                     connections,
                                                     connection_count,
                                                     &which);
        struct mesh_sim_connection_action action;

        if (which == SIZE_MAX || next_us >= deadline_us) {
            break;
        }
        assert(mesh_sim_connection_next_action(sim,
                                               connections[which],
                                               &action) == MESH_SIM_OK);
        if (!action.already_scheduled) {
            assert(mesh_sim_schedule_next_connection_event(sim,
                                                           connections[which],
                                                           false) == MESH_SIM_OK);
        }
        if (action.end_us > deadline_us) {
            assert(mesh_sim_run_until(sim, deadline_us) == MESH_SIM_OK);
            return;
        }
        assert(mesh_sim_run_until(sim, action.end_us) == MESH_SIM_OK);
    }
    assert(mesh_sim_run_until(sim, deadline_us) == MESH_SIM_OK);
}

static void run_connections_until_confirmed(struct mesh_sim_world *sim,
                                             const uint16_t *connections,
                                             size_t connection_count,
                                             uint8_t transmitter_index,
                                             size_t max_events)
{
    for (size_t i = 0u; i < max_events; i++) {
        size_t which;

        if (sim->roles[transmitter_index].relay.pending.state ==
            MESH_RELAY_TX_IDLE) {
            return;
        }
        assert(earliest_connection_start(sim,
                                         connections,
                                         connection_count,
                                         &which) != UINT64_MAX);
        assert(which != SIZE_MAX);
        run_next_connection(sim, connections[which], false);
    }
    fprintf(stderr,
            "confirmation timeout: node=%u now_us=%llu pending=%u msg=%u "
            "queue=%zu transitions=%zu\n",
            transmitter_index,
            (unsigned long long)sim->now_us,
            sim->roles[transmitter_index].relay.pending.state,
            sim->roles[transmitter_index].relay.pending.packet.msg_type,
            sim->roles[transmitter_index].tx_queue_count,
            sim->transition_count);
    for (size_t i = 0u; i < sim->role_count; i++) {
        fprintf(stderr,
                "  role=%zu id=%llx pending=%u msg=%u queue=%zu deliveries=%zu\n",
                i,
                (unsigned long long)sim->roles[i].id,
                sim->roles[i].relay.pending.state,
                sim->roles[i].relay.pending.packet.msg_type,
                sim->roles[i].tx_queue_count,
                sim->roles[i].delivery_count);
    }
    for (size_t i = sim->transition_count > 24u ?
                    sim->transition_count - 24u : 0u;
         i < sim->transition_count;
         i++) {
        const struct mesh_sim_transition *transition = &sim->transitions[i];

        fprintf(stderr,
                "  trace=%zu time=%llu kind=%u node=%llx peer=%llx "
                "msg=%u src=%llx dst=%llx session=%u seq=%u detail=%u\n",
                i,
                (unsigned long long)transition->time_us,
                transition->kind,
                (unsigned long long)transition->node_id,
                (unsigned long long)transition->peer_id,
                transition->msg_type,
                (unsigned long long)transition->packet_src_id,
                (unsigned long long)transition->packet_dst_id,
                transition->packet_session_id,
                transition->packet_seq,
                transition->detail);
    }
    for (size_t i = 0u; i < sim->transition_count; i++) {
        const struct mesh_sim_transition *transition = &sim->transitions[i];

        if (transition->msg_type != MSG_GATEWAY_ACK_CONFIRM &&
            transition->msg_type != MSG_GATEWAY_ACK &&
            transition->msg_type != MSG_MESH_DATA) {
            continue;
        }
        fprintf(stderr,
                "  ack-trace=%zu time=%llu kind=%u node=%llx peer=%llx "
                "msg=%u src=%llx dst=%llx session=%u seq=%u detail=%u\n",
                i,
                (unsigned long long)transition->time_us,
                transition->kind,
                (unsigned long long)transition->node_id,
                (unsigned long long)transition->peer_id,
                transition->msg_type,
                (unsigned long long)transition->packet_src_id,
                (unsigned long long)transition->packet_dst_id,
                transition->packet_session_id,
                transition->packet_seq,
                transition->detail);
    }
    assert(!"transmitter was not confirmed within the bounded event budget");
}

static void test_click_preempts_transit_and_origin_retries(void)
{
    uint8_t transmitter;
    uint8_t anchors[4];
    uint8_t gateway;
    uint8_t clicker;
    uint16_t child_connection;
    uint16_t gateway_connection;
    uint16_t connections[2];
    uint8_t transit_payload[32];
    uint8_t click_payload[160];
    size_t transit_payload_len;
    size_t click_payload_len;
    struct proto_packet transit_packet;
    struct proto_packet click_packet;
    const uint64_t anchor_ids[] = {
        ANCHOR_1_ID,
        ANCHOR_2_ID,
        ANCHOR_3_ID,
        ANCHOR_4_ID,
    };
    struct uwb_clicker_config clicker_config = {
        .network_id = NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = 77u,
        .nonce = UINT64_C(0x1122334455667788),
        .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .max_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    const struct mesh_sim_transition *preempted;
    const struct mesh_sim_transition *retry_ready;
    uint32_t timeout_ms;
    uint32_t retry_at_ms;

    mesh_sim_init(&world, 0x33333333u);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER,
                             TRANSMITTER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &transmitter) == MESH_SIM_OK);
    for (size_t i = 0u; i < 4u; i++) {
        assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                 anchor_ids[i], GATEWAY_ID, ROUTE_EPOCH,
                                 &anchors[i]) == MESH_SIM_OK);
    }
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                             GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &gateway) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &clicker) == MESH_SIM_OK);
    assert(mesh_sim_init_clicker_session(&world,
                                         clicker,
                                         &clicker_config) == PROTO_OK);
    complete_normal_click(&world.roles[clicker], anchor_ids, 4u);

    assert(mesh_sim_set_link(&world, transmitter, anchors[0], 96u, 0u) ==
           MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, anchors[0], gateway, 98u, 0u) ==
           MESH_SIM_OK);
    assert(mesh_sim_install_route(&world, transmitter, anchors[0], 1u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_route(&world, anchors[0], gateway, 0u,
                                  ROUTE_EPOCH) == PROTO_OK);
    assert(mesh_sim_install_downlink(&world, anchors[0], TRANSMITTER_ID,
                                     transmitter, 1u,
                                     ROUTE_EPOCH) == MESH_SIM_OK);
    child_connection = add_connection(&world, transmitter, anchors[0],
                                      100u, 900u, true);
    gateway_connection = add_connection(&world, anchors[0], gateway,
                                        100u, 850u, true);
    connections[0] = child_connection;
    connections[1] = gateway_connection;

    transit_payload_len = mesh_data_payload(transit_payload,
                                            sizeof(transit_payload),
                                            3u);
    transit_packet = mesh_data_packet(3u,
                                      0x1003u,
                                      4u,
                                      (uint16_t)transit_payload_len);
    click_payload_len = build_click_report(click_payload,
                                           sizeof(click_payload),
                                           &click_packet);
    assert(mesh_sim_queue_originated(&world, transmitter, &transit_packet,
                                     transit_payload,
                                     transit_payload_len) == MESH_SIM_OK);
    assert(mesh_sim_queue_originated(&world, anchors[0], &click_packet,
                                     click_payload,
                                     click_payload_len) == MESH_SIM_OK);

    assert(mesh_sim_schedule_next_connection_event(&world,
                                                   child_connection,
                                                   true) == MESH_SIM_OK);
    assert(mesh_sim_schedule_next_connection_event(&world,
                                                   gateway_connection,
                                                   false) == MESH_SIM_OK);
    assert(mesh_sim_run_until(&world, 125000u) == MESH_SIM_OK);
    assert(world.roles[gateway].delivery_count == 1u);
    assert(world.roles[gateway].deliveries[0].packet.msg_type == MSG_CLICK_REPORT);
    assert(world.roles[transmitter].relay.pending.state ==
           MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(world.connections[child_connection].timing_b.missed_event_count == 1u);
    assert(world.connections[child_connection].timing_b.timing_fresh);
    assert(!world.connections[child_connection].timing_b.fallback_required);
    preempted = mesh_sim_find_transition(
        &world,
        MESH_SIM_TRANSITION_CONNECTION_PREEMPTED,
        ANCHOR_1_ID,
        0u);
    assert(preempted != NULL);
    assert(preempted->time_us == 100000u);

    run_next_connection(&world, gateway_connection, false);
    assert(world.roles[anchors[0]].relay.pending.state !=
           MESH_RELAY_TX_IDLE);
    assert(world.roles[anchors[0]].relay.pending.gateway_ack_confirm_pending);
    timeout_ms = world.roles[transmitter].relay.pending.gateway_ack_deadline_ms;
    assert(timeout_ms > 100u);
    assert(mesh_sim_schedule_relay_tick(&world,
                                        transmitter,
                                        (uint64_t)timeout_ms * 1000u) ==
           MESH_SIM_OK);
    run_connections_before(&world,
                           connections,
                           2u,
                           (uint64_t)timeout_ms * 1000u);
    assert(world.roles[transmitter].relay.pending.state ==
           MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(world.roles[transmitter].route_discovery_requests == 0u);
    retry_at_ms = world.roles[transmitter].relay.pending.retry_after_ms;
    assert(retry_at_ms > timeout_ms);
    assert(mesh_sim_schedule_relay_tick(&world,
                                        transmitter,
                                        (uint64_t)retry_at_ms * 1000u) ==
           MESH_SIM_OK);
    run_connections_before(&world,
                           connections,
                           2u,
                           (uint64_t)retry_at_ms * 1000u);
    assert(world.roles[transmitter].relay.pending.state ==
           MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    retry_ready = mesh_sim_find_transition(&world,
                                           MESH_SIM_TRANSITION_RETRY_READY,
                                           TRANSMITTER_ID,
                                           0u);
    assert(retry_ready != NULL);
    assert(retry_ready->time_us == (uint64_t)retry_at_ms * 1000u);

    run_connections_until_confirmed(&world,
                                    connections,
                                    2u,
                                    transmitter,
                                    24u);
    assert(world.roles[gateway].delivery_count == 2u);
    assert(world.roles[gateway].deliveries[0].packet.msg_type == MSG_CLICK_REPORT);
    assert(world.roles[gateway].deliveries[1].packet.msg_type == MSG_MESH_DATA);
    assert(world.roles[gateway].deliveries[1].delivered_at_us >
           world.roles[gateway].deliveries[0].delivered_at_us);
    assert(count_transitions_for_message(&world,
                                         MESH_SIM_TRANSITION_TX_START,
                                         TRANSMITTER_ID,
                                         MSG_MESH_DATA) == 2u);
    assert(world.connections[child_connection].completed_repairs == 0u);
    assert(mesh_sim_count_transitions(
               &world,
               MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
               TRANSMITTER_ID) ==
           0u);
    assert(mesh_sim_count_transitions(
               &world,
               MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
               TRANSMITTER_ID) ==
           0u);
    assert(world.connections[child_connection].timing_b.missed_event_count <
           world.connections[child_connection].timing_b.max_missed_events);
    assert(world.connections[child_connection].timing_b.timing_fresh);
    assert(!world.connections[child_connection].timing_b.fallback_required);
    assert(route_selected(&world.roles[transmitter].relay.upstream) != NULL);
    assert(route_selected(&world.roles[transmitter].relay.upstream)->next_hop_id ==
           ANCHOR_1_ID);
    assert_no_route_fallback(&world);
}

static void test_radio_window_semantics(void)
{
    uint8_t source;
    uint8_t anchor;
    uint8_t raw_frame[UWB_WAKE_CLAIM_LEN];
    size_t raw_frame_len = 0u;
    struct uwb_wake_claim_frame claim;
    struct uwb_clicker_config clicker_config = {
        .network_id = NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = 88u,
        .nonce = UINT64_C(0x8877665544332211),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
    };
    struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = ANCHOR_1_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    uint32_t random_0;
    uint32_t random_1;

    mesh_sim_init(&world, 0x44444444u);
    random_0 = mesh_sim_random(&world);
    random_1 = mesh_sim_random(&world);
    mesh_sim_init(&world, 0x44444444u);
    assert(mesh_sim_random(&world) == random_0);
    assert(mesh_sim_random(&world) == random_1);
    mesh_sim_init(&world, 0x44444444u);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &source) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_init_clicker_session(&world,
                                         source,
                                         &clicker_config) == PROTO_OK);
    assert(mesh_sim_init_anchor_session(&world, anchor, &anchor_config) == PROTO_OK);
    assert(mesh_sim_set_link(&world, source, anchor, 100u, 0u) == MESH_SIM_OK);
    assert(uwb_clicker_build_wake_claim(&world.roles[source].clicker_session,
                                        CLICKER_ID,
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_OK);
    assert(uwb_encode_wake_claim(&claim,
                                 raw_frame,
                                 sizeof(raw_frame),
                                 &raw_frame_len) == PROTO_OK);
    assert(raw_frame_len == UWB_WAKE_CLAIM_LEN);
    assert(mesh_sim_schedule_rx(&world,
                                anchor,
                                1000u,
                                4000u,
                                UWB_CHANNEL_WAKE_CONTACT,
                                MESH_SIM_PHY_CHANNEL5_WAKE,
                                NULL) == MESH_SIM_OK);
    assert(mesh_sim_schedule_rx(&world,
                                anchor,
                                7000u,
                                8000u,
                                UWB_CHANNEL_WAKE_CONTACT,
                                MESH_SIM_PHY_CHANNEL5_WAKE,
                                NULL) == MESH_SIM_OK);
    assert(mesh_sim_schedule_raw_tx(&world,
                                    source,
                                    1000u,
                                    UWB_CHANNEL_WAKE_CONTACT,
                                    MESH_SIM_PHY_CHANNEL5_WAKE,
                                    raw_frame,
                                    raw_frame_len,
                                    false,
                                    NULL) == MESH_SIM_OK);
    assert(mesh_sim_run(&world) == MESH_SIM_OK);
    assert(world.reception_count == 1u);
    assert(world.receptions[0].outcome == MESH_SIM_RX_SFD_TIMEOUT);
    assert(world.roles[anchor].decoded_frames == 0u);
    assert(world.roles[anchor].partial_frames == 1u);
    assert(world.roles[anchor].anchor_session.diagnostics.scans == 2u);
    assert(world.roles[anchor].anchor_session.diagnostics.preambles == 1u);
    assert(world.roles[anchor].anchor_session.diagnostics.sfd_timeouts == 1u);
    assert(mesh_sim_count_transitions(&world,
                                      MESH_SIM_TRANSITION_RX_DECODED,
                                      ANCHOR_1_ID) == 0u);
}

static void test_full_frame_and_collision_semantics(void)
{
    uint8_t source_1;
    uint8_t source_2;
    uint8_t anchor;
    uint8_t payload[8];
    size_t payload_len = 0u;
    struct proto_packet packet;
    uint32_t duration_us;

    mesh_sim_init(&world, 0x55555555u);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &source_1) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID + 1u, GATEWAY_ID, ROUTE_EPOCH,
                             &source_2) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                             ANCHOR_1_ID, ANCHOR_1_ID, ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, source_1, anchor, 100u, 7u) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, source_2, anchor, 100u, 0u) == MESH_SIM_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_REASON,
                         1u) == PROTO_OK);
    packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = CLICKER_ID,
        .dst_id = ANCHOR_1_ID,
        .session_id = 0x3001u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    duration_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL9_MESH,
        proto_packet_encoded_len(payload_len));
    assert(mesh_sim_schedule_rx(&world,
                                anchor,
                                900u,
                                1100u + duration_us,
                                UWB_CHANNEL_MESH_PAYLOAD,
                                MESH_SIM_PHY_CHANNEL9_MESH,
                                NULL) == MESH_SIM_OK);
    assert(mesh_sim_schedule_packet_tx(&world,
                                       source_1,
                                       1000u,
                                       UWB_CHANNEL_MESH_PAYLOAD,
                                       MESH_SIM_PHY_CHANNEL9_MESH,
                                       &packet,
                                       payload,
                                       payload_len,
                                       NULL) == MESH_SIM_OK);
    assert(mesh_sim_run(&world) == MESH_SIM_OK);
    assert(world.reception_count == 1u);
    assert(world.receptions[0].outcome == MESH_SIM_RX_DECODED);
    assert(world.receptions[0].protocol_status == PROTO_OK);
    assert(world.receptions[0].start_us == 1007u);
    assert(world.receptions[0].end_us == 1007u + duration_us);
    assert(world.roles[anchor].delivery_count == 1u);
    assert(world.roles[anchor].deliveries[0].packet.seq == 1u);
    assert(world.roles[anchor].deliveries[0].delivered_at_us ==
           1007u + duration_us);

    mesh_sim_init(&world, 0x66666666u);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID, GATEWAY_ID, ROUTE_EPOCH,
                             &source_1) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_CLICKER,
                             CLICKER_ID + 1u, GATEWAY_ID, ROUTE_EPOCH,
                             &source_2) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                             ANCHOR_1_ID, ANCHOR_1_ID, ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, source_1, anchor, 100u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, source_2, anchor, 100u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_schedule_rx(&world,
                                anchor,
                                900u,
                                1100u + duration_us,
                                UWB_CHANNEL_MESH_PAYLOAD,
                                MESH_SIM_PHY_CHANNEL9_MESH,
                                NULL) == MESH_SIM_OK);
    assert(mesh_sim_schedule_packet_tx(&world,
                                       source_1,
                                       1000u,
                                       UWB_CHANNEL_MESH_PAYLOAD,
                                       MESH_SIM_PHY_CHANNEL9_MESH,
                                       &packet,
                                       payload,
                                       payload_len,
                                       NULL) == MESH_SIM_OK);
    packet.src_id = CLICKER_ID + 1u;
    packet.seq = 2u;
    assert(mesh_sim_schedule_packet_tx(&world,
                                       source_2,
                                       1000u,
                                       UWB_CHANNEL_MESH_PAYLOAD,
                                       MESH_SIM_PHY_CHANNEL9_MESH,
                                       &packet,
                                       payload,
                                       payload_len,
                                       NULL) == MESH_SIM_OK);
    assert(mesh_sim_run(&world) == MESH_SIM_OK);
    assert(world.reception_count == 2u);
    assert(world.receptions[0].outcome == MESH_SIM_RX_COLLISION);
    assert(world.receptions[1].outcome == MESH_SIM_RX_COLLISION);
    assert(world.roles[anchor].delivery_count == 0u);
    assert(world.roles[anchor].collision_frames == 2u);
}

static void test_channel9_runtime_timing_and_deadline(void)
{
    struct mesh_event_params params = connection_params(100u, 440u);
    struct mesh_sim_connection_action action;
    struct proto_packet packet;
    uint8_t payload[32];
    uint8_t transmitter;
    uint8_t anchor;
    uint16_t connection;
    size_t payload_len;

    mesh_sim_init(&world, 0x66666666u);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_TRANSMITTER,
                             TRANSMITTER_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &transmitter) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, transmitter, anchor, 96u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_install_route(&world,
                                  transmitter,
                                  anchor,
                                  1u,
                                  ROUTE_EPOCH) == PROTO_OK);
    connection = add_connection(&world,
                                transmitter,
                                anchor,
                                params.first_event_time_ms,
                                params.event_interval_ms,
                                true);
    payload_len = mesh_data_payload(payload, sizeof(payload), 0x7001u);
    packet = mesh_data_packet(0x701u, 0x70000001u, 4u,
                              (uint16_t)payload_len);
    assert(mesh_sim_queue_originated(&world,
                                     transmitter,
                                     &packet,
                                     payload,
                                     payload_len) == MESH_SIM_OK);
    run_next_connection(&world, connection, false);

    assert(world.transmission_count == 1u);
    assert(world.transmissions[0].dwm_runtime_owned);
    assert(world.rx_window_count == 1u);
    assert(world.rx_windows[0].dwm_runtime_owned);
    assert(world.transmissions[0].start_us ==
           (uint64_t)params.first_event_time_ms * 1000u +
               MESH_SIM_SLOT_TX_OFFSET_US);
    assert(world.roles[transmitter].dwm3000.operation_count >= 7u);
    assert(world.roles[anchor].dwm3000.operation_count >= 9u);
    assert(world.roles[transmitter].dwm3000.configured_phy ==
           DWM3000_TIMING_PHY_CH9_MESH);
    assert(world.roles[anchor].dwm3000.configured_phy ==
           DWM3000_TIMING_PHY_CH9_MESH);
    assert(world.roles[transmitter].dwm3000.radio_state ==
           DWM3000_RUNTIME_RADIO_IDLE);
    assert(world.roles[anchor].dwm3000.radio_state ==
           DWM3000_RUNTIME_RADIO_IDLE);

    mesh_sim_init(&world, 0x77777777u);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_TRANSMITTER,
                             TRANSMITTER_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &transmitter) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             &anchor) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, transmitter, anchor, 96u, 0u) == MESH_SIM_OK);
    assert(mesh_sim_install_route(&world,
                                  transmitter,
                                  anchor,
                                  1u,
                                  ROUTE_EPOCH) == PROTO_OK);
    connection = add_connection(&world,
                                transmitter,
                                anchor,
                                params.first_event_time_ms,
                                params.event_interval_ms,
                                true);
    assert(mesh_sim_set_channel9_tx_offset_us(&world, 50u) == MESH_SIM_OK);
    assert(mesh_sim_queue_originated(&world,
                                     transmitter,
                                     &packet,
                                     payload,
                                     payload_len) == MESH_SIM_OK);
    assert(mesh_sim_connection_next_action(&world,
                                           connection,
                                           &action) == MESH_SIM_OK);
    assert(mesh_sim_schedule_next_connection_event(&world,
                                                   connection,
                                                   false) == MESH_SIM_OK);
    assert(mesh_sim_run_until(&world, action.end_us) ==
           MESH_SIM_ERR_RADIO_DEADLINE);
    assert(world.last_error == MESH_SIM_ERR_RADIO_DEADLINE);
    assert(world.roles[transmitter].tx_queue_count == 1u);
}

static void add_radio_negotiation_roles(uint8_t *initiator,
                                        uint8_t *peer,
                                        uint8_t *interferer)
{
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_1_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             initiator) == MESH_SIM_OK);
    assert(mesh_sim_add_role(&world,
                             MESH_SIM_ROLE_ANCHOR,
                             ANCHOR_2_ID,
                             GATEWAY_ID,
                             ROUTE_EPOCH,
                             peer) == MESH_SIM_OK);
    assert(mesh_sim_set_link(&world, *initiator, *peer, 96u, 5u) ==
           MESH_SIM_OK);
    if (interferer != NULL) {
        assert(mesh_sim_add_role(&world,
                                 MESH_SIM_ROLE_ANCHOR,
                                 ANCHOR_3_ID,
                                 GATEWAY_ID,
                                 ROUTE_EPOCH,
                                 interferer) == MESH_SIM_OK);
        assert(mesh_sim_set_link(&world, *interferer, *peer, 96u, 7u) ==
               MESH_SIM_OK);
    }
}

static void test_connection_negotiation_uses_radio_exchange(void)
{
    const struct mesh_event_params params = connection_params(1000u, 440u);
    uint8_t collision_frame[24] = {0x5au};
    uint8_t initiator;
    uint8_t interferer;
    uint8_t peer;
    uint16_t connection;
    uint64_t proposal_us;

    mesh_sim_init(&world, 0x88888888u);
    add_radio_negotiation_roles(&initiator, &peer, NULL);
    assert(mesh_sim_add_connection_over_radio(&world,
                                              initiator,
                                              peer,
                                              &params,
                                              true,
                                              &connection) == MESH_SIM_OK);
    assert(world.connections[connection].repair_pending);
    assert(world.connections[connection].completed_repairs == 0u);
    assert(mesh_sim_run_until(&world,
                              world.connections[connection].repair_end_us) ==
           MESH_SIM_OK);
    assert(world.connections[connection].completed_repairs == 1u);
    assert(world.connections[connection].repair_propose_decoded);
    assert(world.connections[connection].repair_accept_decoded);
    assert(!world.connections[connection].repair_pending);
    assert(mesh_event_timing_usable(&world.connections[connection].timing_a,
                                    (uint32_t)(world.now_us / 1000u)));
    assert(mesh_event_timing_usable(&world.connections[connection].timing_b,
                                    (uint32_t)(world.now_us / 1000u)));
    assert(count_transitions_for_message(
               &world,
               MESH_SIM_TRANSITION_TX_START,
               ANCHOR_1_ID,
               MSG_MESH_EVENT_PROPOSE) == 1u);
    assert(count_transitions_for_message(
               &world,
               MESH_SIM_TRANSITION_TX_START,
               ANCHOR_2_ID,
               MSG_MESH_EVENT_ACCEPT) == 1u);

    mesh_sim_init(&world, 0x99999999u);
    add_radio_negotiation_roles(&initiator, &peer, &interferer);
    assert(mesh_sim_add_connection_over_radio(&world,
                                              initiator,
                                              peer,
                                              &params,
                                              true,
                                              &connection) == MESH_SIM_OK);
    proposal_us = world.connections[connection].repair_start_us +
                  (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u;
    assert(mesh_sim_schedule_raw_tx(&world,
                                    interferer,
                                    proposal_us,
                                    UWB_CHANNEL_WAKE_CONTACT,
                                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                    collision_frame,
                                    sizeof(collision_frame),
                                    false,
                                    NULL) == MESH_SIM_OK);
    assert(mesh_sim_run_until(&world,
                              world.connections[connection].repair_end_us) ==
           MESH_SIM_OK);
    assert(world.connections[connection].completed_repairs == 0u);
    assert(!world.connections[connection].repair_propose_decoded);
    assert(!world.connections[connection].repair_accept_decoded);
    assert(!mesh_event_timing_usable(&world.connections[connection].timing_a,
                                     (uint32_t)(world.now_us / 1000u)));
    assert(mesh_sim_count_transitions(
               &world,
               MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
               ANCHOR_1_ID) == 0u);
    assert(world.roles[peer].collision_frames >= 1u);

    mesh_sim_init(&world, 0xaaaaaaaau);
    add_radio_negotiation_roles(&initiator, &peer, NULL);
    assert(mesh_sim_add_connection_over_radio(&world,
                                              initiator,
                                              peer,
                                              &params,
                                              true,
                                              &connection) == MESH_SIM_OK);
    assert(mesh_sim_set_directed_rx_failures(&world,
                                             peer,
                                             initiator,
                                             1u,
                                             MESH_SIM_RX_FRAME_TIMEOUT) ==
           MESH_SIM_OK);
    assert(mesh_sim_run_until(&world,
                              world.connections[connection].repair_end_us) ==
           MESH_SIM_OK);
    assert(world.connections[connection].repair_propose_decoded);
    assert(!world.connections[connection].repair_accept_decoded);
    assert(world.connections[connection].completed_repairs == 0u);
    assert(world.roles[initiator].partial_frames >= 1u);
}

int main(void)
{
    test_single_relay_delivery();
    test_two_relay_delivery();
    test_click_preempts_transit_and_origin_retries();
    test_radio_window_semantics();
    test_full_frame_and_collision_semantics();
    test_channel9_runtime_timing_and_deadline();
    test_connection_negotiation_uses_radio_exchange();
    puts("mesh integration simulator tests passed");
    return 0;
}
