#include "mesh_sim.h"

#include "report.h"
#include "route.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ROUTE_EPOCH UINT32_C(41)
#define GATEWAY_ID UINT64_C(0x9000)
#define ORIGIN_ID_BASE UINT64_C(0xB100)
#define RELAY_ID_BASE UINT64_C(0xA100)

#define NETWORK_WATCHDOG_US MESH_SIM_WATCHDOG_PRODUCTION_TIMEOUT_US
#define MAX_NETWORK_STEPS 900u
#define MULTI_ORIGIN_BURSTS 2u
#define MULTI_ORIGIN_COUNT 3u
#define TIMING_REPAIR_CYCLES 3u
#define SIX_HOP_COUNT 6u
#define SIX_HOP_RELAY_COUNT (SIX_HOP_COUNT - 1u)
#define SIX_HOP_PACKET_COUNT 48u
#define LOCAL_CLICK_COUNT (MESH_RUNTIME_WORK_CAPACITY - 1u)
#define LOCAL_CLICK_DURATION_US UINT64_C(1000)
#define SHARED_RELAY_ORIGIN_COUNT 4u
#define SHARED_RELAY_INTERVAL_MS 2400u
#define SHARED_RELAY_UPSTREAM_PHASE_MS 5000u
#define SHARED_RELAY_DOWNSTREAM_PHASE_MS 100u
#define SHARED_RELAY_DOWNSTREAM_INTERVAL_MS 200u
#define SHARED_RELAY_MAX_STEPS 240u
#define SHARED_RELAY_MAX_DURATION_US UINT64_C(90000000)
#define SHARED_RELAY_MAX_QUEUE_DEPTH 6u
#define ACK_AIRTIME_CHILD_PHASE_MS 100u
#define ACK_AIRTIME_CHILD_INTERVAL_MS 1000u
#define ACK_AIRTIME_UPSTREAM_PHASE_MS 500u
#define ACK_AIRTIME_UPSTREAM_INTERVAL_MS 2000u
#define ACK_AIRTIME_MAX_DURATION_US UINT64_C(15000000)

static const uint32_t multi_origin_seeds[] = {
    UINT32_C(0x5EED1001),
    UINT32_C(0x5EED1002),
    UINT32_C(0x5EED10F1),
};

static const uint32_t queue_pressure_seeds[] = {
    UINT32_C(0x5EED2001),
    UINT32_C(0x5EED20A5),
};

static const uint32_t partition_seeds[] = {
    UINT32_C(0x5EED3001),
    UINT32_C(0x5EED30C3),
};

static const uint32_t timing_repair_seeds[] = {
    UINT32_C(0x5EED4001),
    UINT32_C(0x5EED4047),
    UINT32_C(0x5EED40D3),
    UINT32_C(0x5EED40FF),
};

static const uint32_t six_hop_seeds[] = {
    UINT32_C(0x5EED6001),
    UINT32_C(0x5EED60B7),
};

static const uint32_t shared_relay_seeds[] = {
    UINT32_C(0x5EED7001),
    UINT32_C(0x5EED70A7),
};

struct test_context {
    const char *scenario;
    const char *phase;
    uint32_t seed;
    struct mesh_sim_world *world;
};

static struct mesh_sim_world world;
static struct test_context test_ctx;
static char phase_buffer[96];

static size_t runtime_work_count(const struct mesh_runtime *runtime)
{
    size_t count = 0u;

    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        if (runtime->work[i].valid) {
            count++;
        }
    }
    return count;
}

static void set_phase(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    (void)vsnprintf(phase_buffer, sizeof(phase_buffer), format, args);
    va_end(args);
    test_ctx.phase = phase_buffer;
}

static void begin_case(const char *scenario, uint32_t seed)
{
    mesh_sim_init(&world, seed);
    test_ctx = (struct test_context) {
        .scenario = scenario,
        .phase = "setup",
        .seed = seed,
        .world = &world,
    };
}

static void dump_world_state(const struct mesh_sim_world *sim)
{
    fprintf(stderr,
            "state now_us=%llu last_error=%d roles=%zu connections=%zu "
            "conn_events=%zu events=%zu tx=%zu rx=%zu transitions=%zu\n",
            (unsigned long long)sim->now_us,
            sim->last_error,
            sim->role_count,
            sim->connection_count,
            sim->connection_event_count,
            sim->event_count,
            sim->transmission_count,
            sim->reception_count,
            sim->transition_count);

    for (size_t i = 0u; i < sim->role_count; i++) {
        const struct mesh_sim_role_instance *node = &sim->roles[i];
        const struct route_candidate *selected =
            node->relay_initialized ? route_selected(&node->relay.upstream) : NULL;

        fprintf(stderr,
                "role[%zu] id=%llx kind=%d radio=%d queue=%zu deliveries=%zu "
                "pending=%d pending_seq=%u pending_next=%llx runtime_owner=%d "
                "runtime_until=%llu work=%zu transit_reserved=%u "
                "transit_abandoned=%u route_requests=%u selected=%llx "
                "route_epoch=%u watchdog_expired=%u\n",
                i,
                (unsigned long long)node->id,
                node->role,
                node->radio_state,
                node->tx_queue_count,
                node->delivery_count,
                node->relay.pending.state,
                node->relay.pending.packet.seq,
                (unsigned long long)node->relay.pending.next_hop_id,
                node->runtime.radio_owner,
                (unsigned long long)node->runtime.radio_busy_until_us,
                runtime_work_count(&node->runtime),
                node->runtime.transit_reserved,
                node->runtime.transit_abandon_count,
                node->route_discovery_requests,
                (unsigned long long)(selected == NULL ? 0u :
                                     selected->next_hop_id),
                selected == NULL ? 0u : selected->route_epoch,
                node->watchdog.expired);
    }

    for (size_t i = 0u; i < sim->connection_count; i++) {
        const struct mesh_sim_connection *connection = &sim->connections[i];
        struct mesh_sim_connection_action action = {0};
        int action_ret = mesh_sim_connection_next_action(sim,
                                                         (uint16_t)i,
                                                         &action);

        fprintf(stderr,
                "connection[%zu] nodes=%u/%u reachable=%u valid=%u "
                "fresh=%u/%u fallback=%u/%u missed=%u/%u diagnostics=%u/%u "
                "repair_pending=%u repairs=%u action_ret=%d action=%d "
                "scheduled=%u start_us=%llu end_us=%llu skipped=%u\n",
                i,
                connection->node_a,
                connection->node_b,
                sim->reachable[connection->node_a][connection->node_b],
                connection->valid,
                connection->timing_a.timing_fresh,
                connection->timing_b.timing_fresh,
                connection->timing_a.fallback_required,
                connection->timing_b.fallback_required,
                connection->timing_a.missed_event_count,
                connection->timing_b.missed_event_count,
                connection->diagnostics_a.ch9_event_misses,
                connection->diagnostics_b.ch9_event_misses,
                connection->repair_pending,
                connection->completed_repairs,
                action_ret,
                action_ret == MESH_SIM_OK ? (int)action.kind : -1,
                action_ret == MESH_SIM_OK ? action.already_scheduled : false,
                (unsigned long long)(action_ret == MESH_SIM_OK ?
                                     action.start_us : 0u),
                (unsigned long long)(action_ret == MESH_SIM_OK ?
                                     action.end_us : 0u),
                action_ret == MESH_SIM_OK ? action.skipped_events : 0u);
    }

    if (sim->transition_count > 0u) {
        const struct mesh_sim_transition *transition =
            &sim->transitions[sim->transition_count - 1u];

        fprintf(stderr,
                "last_transition time_us=%llu kind=%d node=%llx peer=%llx "
                "msg=%u detail=%u\n",
                (unsigned long long)transition->time_us,
                transition->kind,
                (unsigned long long)transition->node_id,
                (unsigned long long)transition->peer_id,
                transition->msg_type,
                transition->detail);
    }
}

static int fail_requirement(int line, const char *expression)
{
    fprintf(stderr,
            "scenario=%s seed=0x%08x phase=%s line=%d assertion=%s\n",
            test_ctx.scenario,
            (unsigned int)test_ctx.seed,
            test_ctx.phase,
            line,
            expression);
    if (test_ctx.world != NULL) {
        dump_world_state(test_ctx.world);
    }
    return 1;
}

#define CHECK(expression) do { \
    if (!(expression)) { \
        return fail_requirement(__LINE__, #expression); \
    } \
} while (0)

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

static int build_data_packet(uint64_t source_id,
                             uint16_t seq,
                             uint32_t packet_id,
                             uint8_t ttl,
                             struct proto_packet *packet,
                             uint8_t *payload,
                             size_t payload_capacity,
                             size_t *payload_len)
{
    size_t length = 0u;
    int ret;

    ret = tlv_append_u32(payload,
                         payload_capacity,
                         &length,
                         TLV_MESH_TEST_PACKET_ID,
                         packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_capacity,
                        &length,
                        TLV_MESH_TEST_ATTEMPT,
                        1u);
    if (ret != PROTO_OK || length > UINT16_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    *packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = source_id,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x61000000) ^ (uint32_t)source_id ^ packet_id,
        .seq = seq,
        .ttl = ttl,
        .payload_len = (uint16_t)length,
    };
    *payload_len = length;
    return PROTO_OK;
}

static int build_click_report(uint64_t anchor_id,
                              uint16_t seq,
                              struct proto_packet *packet,
                              uint8_t *payload,
                              size_t payload_capacity,
                              size_t *payload_len)
{
    const struct range_report_fields fields = {
        .clicker_id = UINT64_C(0xC000) + seq,
        .anchor_id = anchor_id,
        .event_seq = seq,
        .timestamp_ms = (uint32_t)seq * 10u,
        .distance_mm = 1000 + (int32_t)seq,
        .quality = 100u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t length = 0u;
    int ret;

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
                                   UINT32_C(0x62000000) + seq,
                                   seq,
                                   (uint8_t)length);
    if (ret == PROTO_OK) {
        *payload_len = length;
    }
    return ret;
}

static int arm_all_watchdogs(struct mesh_sim_world *sim)
{
    for (size_t i = 0u; i < sim->role_count; i++) {
        int ret = mesh_sim_watchdog_arm(sim,
                                        (uint8_t)i,
                                        NETWORK_WATCHDOG_US,
                                        MESH_SIM_WATCHDOG_FAIL);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_OK;
}

static bool no_watchdog_expired(const struct mesh_sim_world *sim)
{
    for (size_t i = 0u; i < sim->role_count; i++) {
        if (sim->roles[i].watchdog.expired ||
            sim->roles[i].watchdog.expirations != 0u) {
            return false;
        }
    }
    return mesh_sim_count_transitions(sim,
                                      MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                      0u) == 0u;
}

static bool network_idle(const struct mesh_sim_world *sim)
{
    for (size_t i = 0u; i < sim->role_count; i++) {
        if (sim->roles[i].tx_queue_count != 0u ||
            sim->roles[i].relay.pending.state != MESH_RELAY_TX_IDLE) {
            return false;
        }
    }
    for (size_t i = 0u; i < sim->connection_count; i++) {
        if (sim->connections[i].repair_pending) {
            return false;
        }
    }
    return true;
}

static const struct mesh_sim_queued_tx *best_queued_tx_for_peer(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id)
{
    const struct mesh_sim_queued_tx *best = NULL;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &node->tx_queue[i];

        if (!candidate->valid || candidate->outbound.next_hop_id != peer_id) {
            continue;
        }
        if (best == NULL || candidate->priority > best->priority ||
            (candidate->priority == best->priority &&
             candidate->enqueue_order < best->enqueue_order)) {
            best = candidate;
        }
    }
    return best;
}

static bool connection_action_runnable(
    const struct mesh_sim_world *sim,
    uint16_t connection_index,
    const struct mesh_sim_connection_action *action)
{
    const struct mesh_sim_connection *connection =
        &sim->connections[connection_index];
    const struct mesh_sim_role_instance *sender;
    const struct mesh_sim_role_instance *receiver;
    const struct mesh_sim_queued_tx *queued;
    bool node_a_tx;

    if (action->kind != MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT) {
        return true;
    }
    node_a_tx = mesh_event_timing_local_tx_slot(&connection->timing_a);
    if ((action->skipped_events & 1u) != 0u) {
        node_a_tx = !node_a_tx;
    }
    sender = &sim->roles[node_a_tx ? connection->node_a : connection->node_b];
    receiver = &sim->roles[node_a_tx ? connection->node_b : connection->node_a];
    queued = best_queued_tx_for_peer(sender, receiver->id);

    /* A fresh local/forwarded packet cannot replace an owned transaction. */
    return queued == NULL || !queued->needs_relay_start ||
           sender->relay.pending.state == MESH_RELAY_TX_IDLE;
}

static int run_connection(struct mesh_sim_world *sim,
                          uint16_t connection_index,
                          bool receiver_preempted)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(sim,
                                              connection_index,
                                              &action);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (action.kind == MESH_SIM_CONNECTION_ACTION_NONE) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    if (!action.already_scheduled) {
        ret = mesh_sim_schedule_next_connection_event(sim,
                                                      connection_index,
                                                      receiver_preempted);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return mesh_sim_run_until(sim, action.end_us);
}

static int run_earliest_connection(struct mesh_sim_world *sim,
                                   const uint16_t *connections,
                                   size_t connection_count)
{
    size_t selected = SIZE_MAX;
    uint64_t earliest_us = UINT64_MAX;
    int first_error = MESH_SIM_OK;

    for (size_t i = 0u; i < connection_count; i++) {
        struct mesh_sim_connection_action action;
        int ret = mesh_sim_connection_next_action(sim,
                                                  connections[i],
                                                  &action);

        if (ret != MESH_SIM_OK) {
            if (first_error == MESH_SIM_OK) {
                first_error = ret;
            }
            continue;
        }
        if (action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
            connection_action_runnable(sim, connections[i], &action) &&
            action.start_us >= sim->now_us &&
            action.start_us < earliest_us) {
            selected = i;
            earliest_us = action.start_us;
        }
    }
    if (selected == SIZE_MAX) {
        return first_error == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER :
                                            first_error;
    }
    return run_connection(sim, connections[selected], false);
}

static int drive_until_quiescent(struct mesh_sim_world *sim,
                                 const uint16_t *connections,
                                 size_t connection_count,
                                 uint8_t gateway,
                                 size_t expected_deliveries,
                                 unsigned int max_steps,
                                 const char *phase_prefix)
{
    for (unsigned int step = 0u; step < max_steps; step++) {
        int ret;

        if (sim->roles[gateway].delivery_count >= expected_deliveries &&
            network_idle(sim)) {
            return MESH_SIM_OK;
        }
        set_phase("%s-step-%u", phase_prefix, step + 1u);
        ret = run_earliest_connection(sim, connections, connection_count);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static size_t delivery_count_for(const struct mesh_sim_world *sim,
                                 uint8_t gateway,
                                 uint64_t source_id,
                                 uint16_t seq)
{
    size_t count = 0u;

    for (size_t i = 0u; i < sim->roles[gateway].delivery_count; i++) {
        const struct mesh_sim_delivery *delivery =
            &sim->roles[gateway].deliveries[i];

        if (delivery->packet.src_id == source_id &&
            delivery->packet.seq == seq) {
            count++;
        }
    }
    return count;
}

static const struct mesh_sim_delivery *delivery_for_identity(
    const struct mesh_sim_world *sim,
    uint8_t gateway,
    uint64_t source_id,
    uint32_t session_id,
    uint16_t seq)
{
    const struct mesh_sim_delivery *match = NULL;

    for (size_t i = 0u; i < sim->roles[gateway].delivery_count; i++) {
        const struct mesh_sim_delivery *delivery =
            &sim->roles[gateway].deliveries[i];

        if (delivery->packet.src_id == source_id &&
            delivery->packet.session_id == session_id &&
            delivery->packet.seq == seq) {
            if (match != NULL) {
                return NULL;
            }
            match = delivery;
        }
    }
    return match;
}

static size_t queued_packet_count_for(
    const struct mesh_sim_role_instance *node,
    uint64_t source_id,
    uint32_t session_id,
    uint16_t seq)
{
    size_t count = 0u;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &node->tx_queue[i];

        if (queued->valid && queued->outbound.packet.src_id == source_id &&
            queued->outbound.packet.session_id == session_id &&
            queued->outbound.packet.seq == seq) {
            count++;
        }
    }
    return count;
}

static bool packet_identity_matches(const struct proto_packet *packet,
                                    const struct proto_packet *expected)
{
    return packet != NULL && expected != NULL &&
           packet->src_id == expected->src_id &&
           packet->dst_id == expected->dst_id &&
           packet->session_id == expected->session_id &&
           packet->seq == expected->seq;
}

static bool outbound_ack_matches(const struct mesh_outbound *outbound,
                                 uint8_t msg_type,
                                 uint64_t source_id,
                                 uint64_t destination_id,
                                 uint64_t next_hop_id,
                                 uint32_t session_id,
                                 uint16_t requested_seq)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (outbound == NULL || outbound->packet.msg_type != msg_type ||
        outbound->packet.src_id != source_id ||
        outbound->packet.dst_id != destination_id ||
        outbound->packet.session_id != session_id ||
        outbound->next_hop_id != next_hop_id ||
        tlv_find(outbound->payload,
                 outbound->payload_len,
                 TLV_REQUESTED_MSG_SEQ,
                 &encoded,
                 &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(uint16_t)) {
        return false;
    }
    return proto_get_u16_le(encoded) == requested_seq;
}

static size_t active_channel9_timing_count(
    const struct mesh_relay *relay,
    enum mesh_relay_channel9_direction direction)
{
    size_t count = 0u;

    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].direction == (uint8_t)direction) {
            count++;
        }
    }
    return count;
}

static bool shared_relay_reservations_bounded(
    const struct mesh_sim_role_instance *relay)
{
    size_t upstream = active_channel9_timing_count(
        &relay->relay, MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    size_t downstream = active_channel9_timing_count(
        &relay->relay, MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);

    return upstream == 1u && downstream <= 1u &&
           upstream + downstream <= 2u;
}

static uint32_t next_connection_phase_ms(uint64_t now_us,
                                         uint32_t phase_ms,
                                         uint32_t interval_ms)
{
    uint64_t candidate_ms = phase_ms;

    if (candidate_ms * 1000u <= now_us) {
        uint64_t elapsed_ms = now_us / 1000u - phase_ms;
        uint64_t intervals = elapsed_ms / interval_ms + 1u;

        candidate_ms += intervals * interval_ms;
    }
    return candidate_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)candidate_ms;
}

static void record_queue_depths(const struct mesh_sim_world *sim,
                                size_t *max_depths)
{
    for (size_t i = 0u; i < sim->role_count; i++) {
        if (sim->roles[i].tx_queue_count > max_depths[i]) {
            max_depths[i] = sim->roles[i].tx_queue_count;
        }
    }
}

static int expire_connection_timing(struct mesh_sim_world *sim,
                                    uint16_t connection_index,
                                    const char *phase_prefix)
{
    const unsigned int max_events =
        (unsigned int)sim->connections[connection_index].timing_a.max_missed_events *
        2u + 4u;

    for (unsigned int event = 0u; event < max_events; event++) {
        const struct mesh_sim_connection *connection =
            &sim->connections[connection_index];

        if (!connection->timing_a.timing_fresh ||
            !connection->timing_b.timing_fresh) {
            return MESH_SIM_OK;
        }
        set_phase("%s-event-%u", phase_prefix, event + 1u);
        {
            int ret = run_connection(sim, connection_index, false);

            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    return MESH_SIM_ERR_EVENT_ORDER;
}

static int queue_pending_retry(struct mesh_sim_world *sim,
                               uint8_t node_index,
                               const char *phase_prefix)
{
    struct mesh_sim_role_instance *node = &sim->roles[node_index];
    uint64_t at_us;
    int ret;

    if (node->relay.pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    at_us = (uint64_t)node->relay.pending.gateway_ack_deadline_ms * 1000u;
    if (at_us < sim->now_us) {
        at_us = sim->now_us;
    }
    set_phase("%s-ack-timeout", phase_prefix);
    ret = mesh_sim_schedule_relay_tick(sim, node_index, at_us);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_run_until(sim, at_us);
    if (ret != MESH_SIM_OK ||
        node->relay.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }

    at_us = (uint64_t)node->relay.pending.retry_after_ms * 1000u;
    if (at_us < sim->now_us) {
        at_us = sim->now_us;
    }
    set_phase("%s-backoff-expiry", phase_prefix);
    ret = mesh_sim_schedule_relay_tick(sim, node_index, at_us);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_run_until(sim, at_us);
}

static int add_line_connection(struct mesh_sim_world *sim,
                               uint8_t node_a,
                               uint8_t node_b,
                               uint32_t first_event_ms,
                               uint32_t interval_ms,
                               uint16_t *connection_index)
{
    struct mesh_event_params params = connection_params(first_event_ms,
                                                        interval_ms);
    int ret = mesh_sim_set_link(sim, node_a, node_b, 96u, 1u);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_add_connection(sim,
                                   node_a,
                                   node_b,
                                   &params,
                                   true,
                                   connection_index);
}

static int test_multi_origin_bursts(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(multi_origin_seeds) / sizeof(multi_origin_seeds[0]);
         seed_index++) {
        uint8_t nodes[MULTI_ORIGIN_COUNT + 1u];
        uint16_t connections[MULTI_ORIGIN_COUNT];
        uint8_t gateway;
        uint32_t seed = multi_origin_seeds[seed_index];

        begin_case("multi-origin-bursts", seed);
        set_phase("add-roles");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_TRANSMITTER,
                                ORIGIN_ID_BASE,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &nodes[0]) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID_BASE,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &nodes[1]) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID_BASE + 1u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &nodes[2]) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);
        nodes[3] = gateway;

        for (size_t i = 0u; i < MULTI_ORIGIN_COUNT; i++) {
            set_phase("add-link-%zu", i);
            CHECK(add_line_connection(&world,
                                      nodes[i],
                                      nodes[i + 1u],
                                      100u + (uint32_t)i * 45u,
                                      360u,
                                      &connections[i]) == MESH_SIM_OK);
            CHECK(mesh_sim_install_route(&world,
                                         nodes[i],
                                         nodes[i + 1u],
                                         (uint8_t)(MULTI_ORIGIN_COUNT - i),
                                         ROUTE_EPOCH) == PROTO_OK);
        }
        set_phase("install-downlinks");
        CHECK(mesh_sim_install_downlink(&world,
                                        nodes[1],
                                        world.roles[nodes[0]].id,
                                        nodes[0],
                                        1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                        nodes[2],
                                        world.roles[nodes[0]].id,
                                        nodes[1],
                                        2u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                        nodes[2],
                                        world.roles[nodes[1]].id,
                                        nodes[1],
                                        1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);

        for (uint16_t burst = 1u; burst <= MULTI_ORIGIN_BURSTS; burst++) {
            size_t expected_deliveries =
                (size_t)burst * MULTI_ORIGIN_COUNT;

            for (size_t origin = 0u; origin < MULTI_ORIGIN_COUNT; origin++) {
                struct proto_packet packet;
                uint8_t payload[32];
                size_t payload_len = 0u;
                uint16_t seq = (uint16_t)(burst * 16u + origin);
                uint32_t packet_id = ((uint32_t)burst << 8) |
                                     (uint32_t)origin;

                set_phase("burst-%u-origin-%zu-queue", burst, origin);
                CHECK(build_data_packet(world.roles[nodes[origin]].id,
                                        seq,
                                        packet_id,
                                        12u,
                                        &packet,
                                        payload,
                                        sizeof(payload),
                                        &payload_len) == PROTO_OK);
                CHECK(mesh_sim_queue_originated(&world,
                                                nodes[origin],
                                                &packet,
                                                payload,
                                                payload_len) == MESH_SIM_OK);
            }

            set_phase("burst-%u-drain", burst);
            CHECK(drive_until_quiescent(&world,
                                        connections,
                                        MULTI_ORIGIN_COUNT,
                                        gateway,
                                        expected_deliveries,
                                        MAX_NETWORK_STEPS,
                                        "multi-origin-drain") == MESH_SIM_OK);
            CHECK(world.roles[gateway].delivery_count == expected_deliveries);
            for (size_t origin = 0u; origin < MULTI_ORIGIN_COUNT; origin++) {
                uint16_t seq = (uint16_t)(burst * 16u + origin);

                set_phase("burst-%u-origin-%zu-verify", burst, origin);
                CHECK(delivery_count_for(&world,
                                         gateway,
                                         world.roles[nodes[origin]].id,
                                         seq) == 1u);
            }
        }

        set_phase("multi-origin-final-state");
        for (size_t origin = 0u; origin < MULTI_ORIGIN_COUNT; origin++) {
            CHECK(mesh_sim_count_transitions(
                      &world,
                      MESH_SIM_TRANSITION_GATEWAY_ACKED,
                      world.roles[nodes[origin]].id) == MULTI_ORIGIN_BURSTS);
        }
        CHECK(world.last_error == MESH_SIM_OK);
        CHECK(no_watchdog_expired(&world));
    }
    return 0;
}

static int test_queue_pressure_preserves_local_clicks(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(queue_pressure_seeds) /
                      sizeof(queue_pressure_seeds[0]);
         seed_index++) {
        uint8_t anchor;
        uint8_t gateway;
        uint16_t connection;
        uint16_t connections[1];
        struct mesh_runtime *runtime;
        struct mesh_outbound transit = {0};
        uint64_t now_us = 0u;
        uint32_t seed = queue_pressure_seeds[seed_index];

        begin_case("queue-pressure-local-clicks", seed);
        set_phase("queue-add-roles");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID_BASE + 0x20u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &anchor) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);
        CHECK(add_line_connection(&world,
                                  anchor,
                                  gateway,
                                  100u,
                                  260u,
                                  &connection) == MESH_SIM_OK);
        connections[0] = connection;
        CHECK(mesh_sim_install_route(&world,
                                     anchor,
                                     gateway,
                                     1u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);

        runtime = &world.roles[anchor].runtime;
        mesh_runtime_init(runtime,
                          &world.roles[anchor].relay,
                          world.roles[anchor].id,
                          NULL);
        transit.packet = (struct proto_packet) {
            .msg_type = MSG_MESH_DATA,
            .src_id = ORIGIN_ID_BASE + 0x20u,
            .dst_id = GATEWAY_ID,
            .seq = UINT16_C(0x500),
        };
        transit.next_hop_id = GATEWAY_ID;

        set_phase("reserve-droppable-transit");
        CHECK(mesh_runtime_reserve_transit(runtime, &transit, 0u) ==
              MESH_RUNTIME_OK);
        for (uint16_t click = 1u; click <= LOCAL_CLICK_COUNT; click++) {
            set_phase("queue-local-click-%u", click);
            CHECK(mesh_runtime_submit(runtime,
                                      MESH_RUNTIME_WORK_LOCAL_CLICK,
                                      click,
                                      0u) == MESH_RUNTIME_OK);
        }
        set_phase("reject-extra-transit-at-capacity");
        CHECK(runtime_work_count(runtime) == MESH_RUNTIME_WORK_CAPACITY);
        CHECK(mesh_runtime_submit(runtime,
                                  MESH_RUNTIME_WORK_TRANSIT,
                                  UINT64_C(0x501),
                                  0u) == MESH_RUNTIME_ERR_CAPACITY);
        CHECK(runtime_work_count(runtime) == MESH_RUNTIME_WORK_CAPACITY);

        for (uint16_t click = 1u; click <= LOCAL_CLICK_COUNT; click++) {
            struct mesh_runtime_action action;

            set_phase("run-local-click-%u", click);
            CHECK(mesh_runtime_run_boundary(runtime, now_us, &action) ==
                  MESH_RUNTIME_OK);
            CHECK(action.kind == MESH_RUNTIME_ACTION_START_LOCAL_CLICK);
            CHECK(action.token == click);
            CHECK(action.transit_reservation_abandoned == (click == 1u));
            CHECK(mesh_runtime_claim_radio(runtime,
                                           MESH_RUNTIME_RADIO_DS_TWR,
                                           now_us,
                                           now_us + LOCAL_CLICK_DURATION_US) ==
                  MESH_RUNTIME_OK);
            now_us += LOCAL_CLICK_DURATION_US;
            CHECK(mesh_runtime_release_radio(runtime,
                                             MESH_RUNTIME_RADIO_DS_TWR,
                                             now_us) == MESH_RUNTIME_OK);
        }
        set_phase("verify-runtime-pressure-result");
        {
            struct mesh_runtime_action action;

            CHECK(mesh_runtime_run_boundary(runtime, now_us, &action) ==
                  MESH_RUNTIME_OK);
            CHECK(action.kind == MESH_RUNTIME_ACTION_NONE);
        }
        CHECK(runtime_work_count(runtime) == 0u);
        CHECK(!runtime->transit_reserved);
        CHECK(runtime->transit_abandon_count == 1u);

        for (uint16_t click = 1u; click <= LOCAL_CLICK_COUNT; click++) {
            struct proto_packet packet;
            uint8_t payload[160];
            size_t payload_len = 0u;

            set_phase("queue-click-report-%u", click);
            CHECK(build_click_report(world.roles[anchor].id,
                                     click,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len) == PROTO_OK);
            CHECK(mesh_sim_queue_originated(&world,
                                            anchor,
                                            &packet,
                                            payload,
                                            payload_len) == MESH_SIM_OK);
        }
        set_phase("drain-click-reports");
        CHECK(drive_until_quiescent(&world,
                                    connections,
                                    1u,
                                    gateway,
                                    LOCAL_CLICK_COUNT,
                                    MAX_NETWORK_STEPS,
                                    "queue-pressure-drain") == MESH_SIM_OK);
        CHECK(world.roles[gateway].delivery_count == LOCAL_CLICK_COUNT);
        for (uint16_t click = 1u; click <= LOCAL_CLICK_COUNT; click++) {
            set_phase("verify-click-report-%u", click);
            CHECK(delivery_count_for(&world,
                                     gateway,
                                     world.roles[anchor].id,
                                     click) == 1u);
            CHECK(world.roles[gateway].deliveries[click - 1u].packet.msg_type ==
                  MSG_CLICK_REPORT);
        }
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                         world.roles[anchor].id) ==
              LOCAL_CLICK_COUNT);
        CHECK(no_watchdog_expired(&world));
    }
    return 0;
}

static int test_partition_route_and_event_recovery(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(partition_seeds) / sizeof(partition_seeds[0]);
         seed_index++) {
        uint8_t source;
        uint8_t relay;
        uint8_t gateway;
        uint16_t child_connection;
        uint16_t upstream_connection;
        uint16_t connections[2];
        struct proto_packet packet;
        uint8_t payload[32];
        size_t payload_len = 0u;
        struct mesh_sim_connection_action action;
        const struct route_candidate *selected;
        uint32_t seed = partition_seeds[seed_index];

        begin_case("physical-partition-recovery", seed);
        set_phase("partition-add-roles");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_TRANSMITTER,
                                ORIGIN_ID_BASE + 0x30u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &source) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID_BASE + 0x30u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &relay) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);
        CHECK(add_line_connection(&world,
                                  source,
                                  relay,
                                  100u,
                                  300u,
                                  &child_connection) == MESH_SIM_OK);
        CHECK(add_line_connection(&world,
                                  relay,
                                  gateway,
                                  140u,
                                  300u,
                                  &upstream_connection) == MESH_SIM_OK);
        connections[0] = child_connection;
        connections[1] = upstream_connection;
        CHECK(mesh_sim_install_route(&world,
                                     source,
                                     relay,
                                     2u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(mesh_sim_install_route(&world,
                                     relay,
                                     gateway,
                                     1u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                        relay,
                                        world.roles[source].id,
                                        source,
                                        1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);
        CHECK(build_data_packet(world.roles[source].id,
                                1u,
                                UINT32_C(0x3001),
                                8u,
                                &packet,
                                payload,
                                sizeof(payload),
                                &payload_len) == PROTO_OK);
        CHECK(mesh_sim_queue_originated(&world,
                                        source,
                                        &packet,
                                        payload,
                                        payload_len) == MESH_SIM_OK);

        /* The public reachability matrix is the simulator's physical link state. */
        set_phase("partition-link-down");
        world.reachable[source][relay] = false;
        world.reachable[relay][source] = false;
        CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
        CHECK(world.roles[source].relay.pending.state ==
              MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        CHECK(world.roles[gateway].delivery_count == 0u);

        CHECK(expire_connection_timing(&world,
                                       child_connection,
                                       "partition-expiry") == MESH_SIM_OK);
        set_phase("partition-repair-blocked-by-physical-link");
        CHECK(mesh_sim_connection_next_action(&world,
                                              child_connection,
                                              &action) == MESH_SIM_OK);
        CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR);
        CHECK(!world.reachable[source][relay]);
        CHECK(world.connections[child_connection].completed_repairs == 0u);
        CHECK(world.roles[gateway].delivery_count == 0u);

        set_phase("restore-link-and-route");
        CHECK(mesh_sim_set_link(&world, source, relay, 96u, 1u) == MESH_SIM_OK);
        CHECK(mesh_sim_install_route(&world,
                                     relay,
                                     gateway,
                                     1u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(mesh_sim_install_route(&world,
                                     source,
                                     relay,
                                     2u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                        relay,
                                        world.roles[source].id,
                                        source,
                                        1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        selected = route_selected(&world.roles[source].relay.upstream);
        CHECK(selected != NULL);
        CHECK(selected->next_hop_id == world.roles[relay].id);
        CHECK(selected->route_epoch == ROUTE_EPOCH);
        CHECK(selected->last_seen_ms == (uint32_t)(world.now_us / 1000u));

        set_phase("run-explicit-event-repair");
        CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
        CHECK(world.connections[child_connection].completed_repairs == 1u);
        CHECK(world.connections[child_connection].timing_a.timing_fresh);
        CHECK(world.connections[child_connection].timing_b.timing_fresh);
        CHECK(mesh_sim_count_transitions(
                  &world,
                  MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
                  world.roles[source].id) == 1u);
        CHECK(mesh_sim_count_transitions(
                  &world,
                  MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
                  world.roles[source].id) == 1u);

        CHECK(queue_pending_retry(&world,
                                  source,
                                  "partition-origin-retry") == MESH_SIM_OK);
        set_phase("partition-post-recovery-delivery");
        CHECK(drive_until_quiescent(&world,
                                    connections,
                                    2u,
                                    gateway,
                                    1u,
                                    MAX_NETWORK_STEPS,
                                    "partition-recovery-drain") == MESH_SIM_OK);
        CHECK(world.roles[gateway].delivery_count == 1u);
        CHECK(delivery_count_for(&world,
                                 gateway,
                                 world.roles[source].id,
                                 1u) == 1u);
        CHECK(world.roles[source].route_discovery_requests == 0u);
        CHECK(world.roles[relay].route_discovery_requests == 0u);
        CHECK(no_watchdog_expired(&world));
    }
    return 0;
}

static int test_repeated_timing_expiry_and_repair(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(timing_repair_seeds) /
                      sizeof(timing_repair_seeds[0]);
         seed_index++) {
        uint8_t source;
        uint8_t gateway;
        uint16_t connection;
        uint16_t connections[1];
        struct proto_packet packet;
        uint8_t payload[32];
        size_t payload_len = 0u;
        uint32_t seed = timing_repair_seeds[seed_index];

        begin_case("repeated-channel9-expiry-repair", seed);
        set_phase("timing-add-roles");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_TRANSMITTER,
                                ORIGIN_ID_BASE + 0x40u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &source) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);
        CHECK(add_line_connection(&world,
                                  source,
                                  gateway,
                                  100u,
                                  260u,
                                  &connection) == MESH_SIM_OK);
        connections[0] = connection;
        CHECK(mesh_sim_install_route(&world,
                                     source,
                                     gateway,
                                     1u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);
        CHECK(build_data_packet(world.roles[source].id,
                                1u,
                                UINT32_C(0x4001),
                                6u,
                                &packet,
                                payload,
                                sizeof(payload),
                                &payload_len) == PROTO_OK);
        CHECK(mesh_sim_queue_originated(&world,
                                        source,
                                        &packet,
                                        payload,
                                        payload_len) == MESH_SIM_OK);

        set_phase("drop-first-payload-at-receiver");
        CHECK(run_connection(&world, connection, true) == MESH_SIM_OK);
        CHECK(world.roles[gateway].delivery_count == 0u);
        CHECK(world.roles[source].relay.pending.state ==
              MESH_RELAY_TX_WAIT_GATEWAY_ACK);

        for (unsigned int cycle = 1u; cycle <= TIMING_REPAIR_CYCLES; cycle++) {
            set_phase("repair-cycle-%u-expire", cycle);
            CHECK(expire_connection_timing(&world,
                                           connection,
                                           "timing-expiry") == MESH_SIM_OK);
            CHECK(!world.connections[connection].timing_a.timing_fresh ||
                  !world.connections[connection].timing_b.timing_fresh);

            set_phase("repair-cycle-%u-run", cycle);
            CHECK(run_connection(&world, connection, false) == MESH_SIM_OK);
            CHECK(world.connections[connection].completed_repairs == cycle);
            CHECK(world.connections[connection].timing_a.timing_fresh);
            CHECK(world.connections[connection].timing_b.timing_fresh);
            CHECK(!world.connections[connection].timing_a.fallback_required);
            CHECK(!world.connections[connection].timing_b.fallback_required);
            CHECK(world.roles[source].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        }

        set_phase("verify-repair-cycle-accounting");
        CHECK(world.connections[connection].completed_repairs ==
              TIMING_REPAIR_CYCLES);
        CHECK(mesh_sim_count_transitions(
                  &world,
                  MESH_SIM_TRANSITION_CONNECTION_REPAIRED,
                  world.roles[source].id) == TIMING_REPAIR_CYCLES);
        CHECK(queue_pending_retry(&world,
                                  source,
                                  "timing-final-retry") == MESH_SIM_OK);
        set_phase("timing-final-delivery");
        CHECK(drive_until_quiescent(&world,
                                    connections,
                                    1u,
                                    gateway,
                                    1u,
                                    MAX_NETWORK_STEPS,
                                    "timing-final-drain") == MESH_SIM_OK);
        CHECK(world.roles[gateway].delivery_count == 1u);
        CHECK(delivery_count_for(&world,
                                 gateway,
                                 world.roles[source].id,
                                 1u) == 1u);
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                         world.roles[source].id) == 1u);
        CHECK(world.roles[source].route_discovery_requests == 0u);
        CHECK(no_watchdog_expired(&world));
    }
    return 0;
}

/* Fixture-installed routes exercise forwarding capacity, not route formation. */
static int test_six_hop_fixture_routed_forwarding_capacity(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(six_hop_seeds) / sizeof(six_hop_seeds[0]);
         seed_index++) {
        uint8_t nodes[SIX_HOP_COUNT + 1u];
        uint16_t connections[SIX_HOP_COUNT];
        uint8_t gateway;
        uint32_t seed = six_hop_seeds[seed_index];

        begin_case("six-hop-fixture-routed-forwarding-capacity", seed);
        set_phase("six-hop-add-origin");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_TRANSMITTER,
                                ORIGIN_ID_BASE + 0x60u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &nodes[0]) == MESH_SIM_OK);
        for (size_t relay = 0u; relay < SIX_HOP_RELAY_COUNT; relay++) {
            set_phase("six-hop-add-relay-%zu", relay + 1u);
            CHECK(mesh_sim_add_role(&world,
                                    MESH_SIM_ROLE_ANCHOR,
                                    RELAY_ID_BASE + 0x60u + relay,
                                    GATEWAY_ID,
                                    ROUTE_EPOCH,
                                    &nodes[relay + 1u]) == MESH_SIM_OK);
        }
        set_phase("six-hop-add-gateway");
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);
        nodes[SIX_HOP_COUNT] = gateway;

        for (size_t hop = 0u; hop < SIX_HOP_COUNT; hop++) {
            set_phase("six-hop-add-link-%zu", hop + 1u);
            CHECK(add_line_connection(&world,
                                      nodes[hop],
                                      nodes[hop + 1u],
                                      100u + (uint32_t)hop * 32u,
                                      MESH_RADIO_EVENT_INTERVAL_MS,
                                      &connections[hop]) == MESH_SIM_OK);
            CHECK(mesh_sim_install_route(&world,
                                         nodes[hop],
                                         nodes[hop + 1u],
                                         (uint8_t)(SIX_HOP_COUNT - hop),
                                         ROUTE_EPOCH) == PROTO_OK);
            if (hop > 0u) {
                CHECK(mesh_sim_install_downlink(&world,
                                                nodes[hop],
                                                world.roles[nodes[0]].id,
                                                nodes[hop - 1u],
                                                (uint8_t)hop,
                                                ROUTE_EPOCH) == MESH_SIM_OK);
            }
        }
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);

        for (uint16_t seq = 1u; seq <= SIX_HOP_PACKET_COUNT; seq++) {
            struct proto_packet packet;
            uint8_t payload[32];
            size_t payload_len = 0u;

            set_phase("six-hop-queue-packet-%u", seq);
            CHECK(build_data_packet(world.roles[nodes[0]].id,
                                    seq,
                                    UINT32_C(0x6000) + seq,
                                    16u,
                                    &packet,
                                    payload,
                                    sizeof(payload),
                                    &payload_len) == PROTO_OK);
            CHECK(mesh_sim_queue_originated(&world,
                                            nodes[0],
                                            &packet,
                                            payload,
                                            payload_len) == MESH_SIM_OK);
            CHECK(world.roles[nodes[0]].tx_queue_count == 1u);
            set_phase("six-hop-drain-packet-%u", seq);
            CHECK(drive_until_quiescent(&world,
                                        connections,
                                        SIX_HOP_COUNT,
                                        gateway,
                                        seq,
                                        MAX_NETWORK_STEPS,
                                        "six-hop-drain") == MESH_SIM_OK);
            CHECK(world.roles[gateway].delivery_count == seq);
        }
        CHECK(world.roles[gateway].delivery_count == SIX_HOP_PACKET_COUNT);
        for (uint16_t seq = 1u; seq <= SIX_HOP_PACKET_COUNT; seq++) {
            const struct mesh_sim_delivery *delivery =
                &world.roles[gateway].deliveries[seq - 1u];

            set_phase("six-hop-verify-packet-%u", seq);
            CHECK(delivery_count_for(&world,
                                     gateway,
                                     world.roles[nodes[0]].id,
                                     seq) == 1u);
            CHECK(delivery->packet.seq == seq);
            CHECK(delivery->packet.src_id == world.roles[nodes[0]].id);
            CHECK(delivery->packet.ttl > 0u);
        }
        set_phase("six-hop-final-state");
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                         world.roles[nodes[0]].id) ==
              SIX_HOP_PACKET_COUNT);
        CHECK(mesh_sim_trace_total_count(&world) > MESH_SIM_MAX_TRANSITIONS);
        CHECK(mesh_sim_trace_is_truncated(&world));
        CHECK(world.transition_count == MESH_SIM_MAX_TRANSITIONS);
        CHECK(mesh_sim_trace_dropped_count(&world) ==
              mesh_sim_trace_total_count(&world) - world.transition_count);
        CHECK(world.transmission_total_count > MESH_SIM_MAX_TRANSMISSIONS);
        CHECK(world.reception_total_count > MESH_SIM_MAX_TRANSMISSIONS);
        CHECK(mesh_sim_telemetry_is_truncated(
            &world, MESH_SIM_TELEMETRY_TRANSMISSION));
        CHECK(mesh_sim_telemetry_is_truncated(
            &world, MESH_SIM_TELEMETRY_RX_WINDOW));
        {
            struct mesh_sim_transmission retained_tx;

            CHECK(mesh_sim_transmission_snapshot(
                      &world, 0u, &retained_tx) == MESH_SIM_SNAPSHOT_TRUNCATED);
            CHECK(mesh_sim_transmission_snapshot(
                      &world,
                      world.transmission_total_count - 1u,
                      &retained_tx) == MESH_SIM_SNAPSHOT_OK);
        }
        CHECK(world.event_count < MESH_SIM_MAX_EVENTS);
        CHECK(world.last_error == MESH_SIM_OK);
        CHECK(no_watchdog_expired(&world));
    }
    return 0;
}

static int test_completed_event_reclamation(void)
{
    uint8_t node;

    begin_case("completed-event-reclamation", UINT32_C(0x5EEDCA01));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            ORIGIN_ID_BASE + 0xCAu,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &node) == MESH_SIM_OK);
    for (size_t i = 0u; i < MESH_SIM_MAX_EVENTS + 32u; i++) {
        set_phase("event-reclamation-%zu", i);
        CHECK(mesh_sim_schedule_relay_tick(&world, node, world.now_us) ==
              MESH_SIM_OK);
        CHECK(mesh_sim_run_until(&world, world.now_us) == MESH_SIM_OK);
        CHECK(world.event_count == 0u);
    }
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

static int test_watchdog_requires_rx_worker_completion(void)
{
    uint8_t sender;
    uint8_t receiver;
    uint16_t connection;
    struct proto_packet packet;
    uint8_t payload[32];
    size_t payload_len = 0u;

    begin_case("watchdog-rx-worker-completion", UINT32_C(0x5EEDAD01));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            ORIGIN_ID_BASE + 0xD0u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &sender) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID_BASE + 0xD0u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &receiver) == MESH_SIM_OK);
    CHECK(add_line_connection(&world, sender, receiver, 1u, 1000u,
                              &connection) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world, sender, receiver, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(build_data_packet(world.roles[sender].id,
                            1u,
                            UINT32_C(0xD001),
                            4u,
                            &packet,
                            payload,
                            sizeof(payload),
                            &payload_len) == PROTO_OK);
    CHECK(mesh_sim_queue_originated(&world, sender, &packet, payload,
                                    payload_len) == MESH_SIM_OK);
    CHECK(mesh_sim_set_directed_rx_failures(&world,
                                            sender,
                                            receiver,
                                            1u,
                                            MESH_SIM_RX_SFD_TIMEOUT) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_watchdog_arm(&world,
                                receiver,
                                UINT64_C(35000),
                                MESH_SIM_WATCHDOG_FAIL) == MESH_SIM_OK);
    CHECK(mesh_sim_schedule_next_connection_event(&world, connection, false) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&world, UINT64_C(35000)) == MESH_SIM_OK);
    CHECK(world.connection_events[0].receiver_worker_completed);
    CHECK(!world.connection_events[0].decoded);
    CHECK(world.roles[receiver].watchdog.feeds == 1u);
    CHECK(!world.roles[receiver].watchdog.expired);

    begin_case("watchdog-preempted-event", UINT32_C(0x5EEDAD02));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            ORIGIN_ID_BASE + 0xD1u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &sender) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID_BASE + 0xD1u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &receiver) == MESH_SIM_OK);
    CHECK(add_line_connection(&world, sender, receiver, 1u, 1000u,
                              &connection) == MESH_SIM_OK);
    CHECK(mesh_sim_watchdog_arm(&world,
                                receiver,
                                UINT64_C(35000),
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK);
    CHECK(mesh_sim_schedule_next_connection_event(&world, connection, true) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&world, UINT64_C(35000)) == MESH_SIM_OK);
    CHECK(!world.connection_events[0].receiver_worker_completed);
    CHECK(world.roles[receiver].watchdog.feeds == 0u);
    CHECK(world.roles[receiver].watchdog.expired);
    CHECK(world.roles[receiver].watchdog.resets == 1u);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                     world.roles[receiver].id) == 1u);
    return 0;
}

static int open_shared_relay_downstream(struct mesh_sim_world *sim,
                                        uint8_t origin,
                                        uint8_t relay,
                                        uint16_t *connection_index)
{
    struct mesh_relay_channel9_guard_status guard_status;
    uint32_t first_event_ms;
    int ret;

    if (!shared_relay_reservations_bounded(&sim->roles[relay]) ||
        active_channel9_timing_count(
            &sim->roles[relay].relay,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) != 0u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    first_event_ms = next_connection_phase_ms(
        sim->now_us,
        SHARED_RELAY_DOWNSTREAM_PHASE_MS,
        SHARED_RELAY_DOWNSTREAM_INTERVAL_MS);
    ret = add_line_connection(sim,
                              origin,
                              relay,
                              first_event_ms,
                              SHARED_RELAY_DOWNSTREAM_INTERVAL_MS,
                              connection_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_relay_set_channel9_timing_guarded_direction(
        &sim->roles[relay].relay,
        sim->roles[origin].id,
        &sim->connections[*connection_index].timing_b,
        MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
        2u,
        &guard_status);
    if (ret != PROTO_OK ||
        guard_status.reason != MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER ||
        !shared_relay_reservations_bounded(&sim->roles[relay]) ||
        active_channel9_timing_count(
            &sim->roles[relay].relay,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) != 1u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    return MESH_SIM_OK;
}

static int close_shared_relay_downstream(struct mesh_sim_world *sim,
                                         uint8_t origin,
                                         uint8_t relay)
{
    mesh_relay_clear_channel9_timing(&sim->roles[origin].relay,
                                     sim->roles[relay].id);
    mesh_relay_clear_channel9_timing(&sim->roles[relay].relay,
                                     sim->roles[origin].id);
    if (!shared_relay_reservations_bounded(&sim->roles[relay]) ||
        active_channel9_timing_count(
            &sim->roles[relay].relay,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) != 0u) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    return MESH_SIM_OK;
}

static int test_four_origins_share_one_relay(void)
{
    for (size_t seed_index = 0u;
         seed_index < sizeof(shared_relay_seeds) /
                      sizeof(shared_relay_seeds[0]);
         seed_index++) {
        uint8_t origins[SHARED_RELAY_ORIGIN_COUNT];
        struct proto_packet origin_packets[SHARED_RELAY_ORIGIN_COUNT];
        uint8_t origin_payloads[SHARED_RELAY_ORIGIN_COUNT][160];
        size_t origin_payload_lens[SHARED_RELAY_ORIGIN_COUNT] = {0};
        size_t max_queue_depths[MESH_SIM_MAX_ROLES] = {0};
        struct proto_packet relay_click_packet;
        uint8_t relay_click_payload[160];
        size_t relay_click_payload_len = 0u;
        uint8_t relay;
        uint8_t gateway;
        uint16_t upstream_connection;
        uint64_t custody_ready_us;
        bool upstream_drained = false;
        uint32_t seed = shared_relay_seeds[seed_index];

        begin_case("four-origins-one-relay", seed);
        set_phase("shared-relay-add-roles");
        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            CHECK(mesh_sim_add_role(&world,
                                    MESH_SIM_ROLE_ANCHOR,
                                    ORIGIN_ID_BASE + 0x70u + i,
                                    GATEWAY_ID,
                                    ROUTE_EPOCH,
                                    &origins[i]) == MESH_SIM_OK);
        }
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID_BASE + 0x70u,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &relay) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world,
                                MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID,
                                GATEWAY_ID,
                                ROUTE_EPOCH,
                                &gateway) == MESH_SIM_OK);

        set_phase("shared-relay-install-upstream");
        CHECK(mesh_sim_set_link(&world, relay, gateway, 96u, 1u) ==
              MESH_SIM_OK);
        CHECK(mesh_sim_install_route(&world,
                                     relay,
                                     gateway,
                                     1u,
                                     ROUTE_EPOCH) == PROTO_OK);
        CHECK(add_line_connection(&world,
                                  relay,
                                  gateway,
                                  SHARED_RELAY_UPSTREAM_PHASE_MS,
                                  SHARED_RELAY_INTERVAL_MS,
                                  &upstream_connection) == MESH_SIM_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                        gateway,
                                        world.roles[relay].id,
                                        relay,
                                        1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK);
        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            set_phase("shared-relay-install-origin-%zu", i);
            CHECK(mesh_sim_set_link(&world,
                                    origins[i],
                                    relay,
                                    96u,
                                    1u) == MESH_SIM_OK);
            CHECK(mesh_sim_install_route(&world,
                                         origins[i],
                                         relay,
                                         2u,
                                         ROUTE_EPOCH) == PROTO_OK);
            CHECK(mesh_sim_install_downlink(&world,
                                            relay,
                                            world.roles[origins[i]].id,
                                            origins[i],
                                            1u,
                                            ROUTE_EPOCH) == MESH_SIM_OK);
            CHECK(mesh_sim_install_downlink(&world,
                                            gateway,
                                            world.roles[origins[i]].id,
                                            relay,
                                            2u,
                                            ROUTE_EPOCH) == MESH_SIM_OK);
        }
        CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);
        CHECK(shared_relay_reservations_bounded(&world.roles[relay]));

        set_phase("shared-relay-queue-simultaneous-reports");
        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            uint16_t seq = (uint16_t)(UINT16_C(0x700) + i);

            CHECK(build_click_report(world.roles[origins[i]].id,
                                     seq,
                                     &origin_packets[i],
                                     origin_payloads[i],
                                     sizeof(origin_payloads[i]),
                                     &origin_payload_lens[i]) == PROTO_OK);
            CHECK(mesh_sim_queue_originated(&world,
                                            origins[i],
                                            &origin_packets[i],
                                            origin_payloads[i],
                                            origin_payload_lens[i]) ==
                  MESH_SIM_OK);
            CHECK(world.roles[origins[i]].tx_queue_count == 1u);
        }
        record_queue_depths(&world, max_queue_depths);

        set_phase("shared-relay-build-local-click");
        CHECK(build_click_report(world.roles[relay].id,
                                 UINT16_C(0x7F0),
                                 &relay_click_packet,
                                 relay_click_payload,
                                 sizeof(relay_click_payload),
                                 &relay_click_payload_len) == PROTO_OK);

        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            const struct mesh_sim_transmission *hop_ack_tx;
            size_t transmission_count_before;
            uint16_t downstream_connection;

            set_phase("shared-relay-admit-origin-%zu", i);
            CHECK(open_shared_relay_downstream(&world,
                                               origins[i],
                                               relay,
                                               &downstream_connection) ==
                  MESH_SIM_OK);
            CHECK(run_connection(&world, downstream_connection, false) ==
                  MESH_SIM_OK);
            record_queue_depths(&world, max_queue_depths);
            CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
            CHECK(queued_packet_count_for(&world.roles[relay],
                                          origin_packets[i].src_id,
                                          origin_packets[i].session_id,
                                          origin_packets[i].seq) == 1u);
            CHECK(world.roles[origins[i]].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
            CHECK(world.connections[upstream_connection].completed_events == 0u);
            CHECK(world.roles[gateway].delivery_count == 0u);

            transmission_count_before = world.transmission_count;
            CHECK(run_connection(&world, downstream_connection, false) ==
                  MESH_SIM_OK);
            record_queue_depths(&world, max_queue_depths);
            CHECK(world.transmission_count == transmission_count_before + 1u);
            hop_ack_tx = &world.transmissions[transmission_count_before];
            CHECK(hop_ack_tx->node_index == relay);
            CHECK(hop_ack_tx->has_outbound);
            CHECK(outbound_ack_matches(&hop_ack_tx->outbound,
                                       MSG_MESH_HOP_ACK,
                                       world.roles[relay].id,
                                       origin_packets[i].src_id,
                                       origin_packets[i].src_id,
                                       origin_packets[i].session_id,
                                       origin_packets[i].seq));
            CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
            CHECK(mesh_sim_count_transitions(
                      &world,
                      MESH_SIM_TRANSITION_HOP_PROGRESS,
                      origin_packets[i].src_id) == 1u);
            CHECK(world.roles[origins[i]].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
            CHECK(close_shared_relay_downstream(&world,
                                                origins[i],
                                                relay) == MESH_SIM_OK);
        }

        set_phase("shared-relay-simultaneous-custody");
        custody_ready_us = world.now_us;
        CHECK(world.connections[upstream_connection].completed_events == 0u);
        CHECK(world.roles[gateway].delivery_count == 0u);
        CHECK(world.roles[relay].tx_queue_count == SHARED_RELAY_ORIGIN_COUNT);
        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            CHECK(queued_packet_count_for(&world.roles[relay],
                                          origin_packets[i].src_id,
                                          origin_packets[i].session_id,
                                          origin_packets[i].seq) == 1u);
            for (size_t j = i + 1u; j < SHARED_RELAY_ORIGIN_COUNT; j++) {
                CHECK(!packet_identity_matches(&origin_packets[i],
                                               &origin_packets[j]));
            }
        }

        set_phase("shared-relay-local-click-priority");
        CHECK(mesh_sim_queue_originated(&world,
                                        relay,
                                        &relay_click_packet,
                                        relay_click_payload,
                                        relay_click_payload_len) ==
              MESH_SIM_OK);
        record_queue_depths(&world, max_queue_depths);
        CHECK(world.roles[relay].tx_queue_count ==
              SHARED_RELAY_ORIGIN_COUNT + 1u);

        for (unsigned int step = 0u;
             step < SHARED_RELAY_MAX_STEPS && !upstream_drained;
             step++) {
            upstream_drained =
                world.roles[gateway].delivery_count ==
                    SHARED_RELAY_ORIGIN_COUNT + 1u &&
                world.roles[gateway].tx_queue_count == 0u &&
                world.roles[relay].relay.pending.state == MESH_RELAY_TX_IDLE &&
                world.roles[relay].tx_queue_count == SHARED_RELAY_ORIGIN_COUNT;
            if (upstream_drained) {
                break;
            }
            set_phase("shared-relay-upstream-drain-%u", step);
            CHECK(run_connection(&world, upstream_connection, false) ==
                  MESH_SIM_OK);
            record_queue_depths(&world, max_queue_depths);
            CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
        }
        CHECK(upstream_drained);
        CHECK(world.roles[gateway].deliveries[0].packet.src_id ==
              world.roles[relay].id);
        CHECK(world.roles[gateway].deliveries[0].packet.seq ==
              relay_click_packet.seq);
        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            const struct mesh_sim_delivery *delivery =
                &world.roles[gateway].deliveries[i + 1u];

            CHECK(packet_identity_matches(&delivery->packet,
                                          &origin_packets[i]));
            CHECK(delivery->previous_hop_id == world.roles[relay].id);
            CHECK(delivery->payload_len == origin_payload_lens[i]);
            CHECK(memcmp(delivery->payload,
                         origin_payloads[i],
                         origin_payload_lens[i]) == 0);
            CHECK(delivery->delivered_at_us >= custody_ready_us);
            CHECK(delivery->delivered_at_us <= SHARED_RELAY_MAX_DURATION_US);
            CHECK(delivery_count_for(&world,
                                     gateway,
                                     origin_packets[i].src_id,
                                     origin_packets[i].seq) == 1u);
            CHECK(world.roles[origins[i]].relay.pending.state ==
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        }

        for (size_t i = 0u; i < SHARED_RELAY_ORIGIN_COUNT; i++) {
            const struct mesh_sim_transmission *gateway_ack_tx;
            size_t transmission_count_before;
            uint16_t downstream_connection;

            set_phase("shared-relay-confirm-origin-%zu", i);
            CHECK(open_shared_relay_downstream(&world,
                                               origins[i],
                                               relay,
                                               &downstream_connection) ==
                  MESH_SIM_OK);
            CHECK(run_connection(&world, downstream_connection, false) ==
                  MESH_SIM_OK);
            CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
            transmission_count_before = world.transmission_count;
            CHECK(run_connection(&world, downstream_connection, false) ==
                  MESH_SIM_OK);
            CHECK(world.transmission_count == transmission_count_before + 1u);
            gateway_ack_tx = &world.transmissions[transmission_count_before];
            CHECK(gateway_ack_tx->node_index == relay);
            CHECK(gateway_ack_tx->has_outbound);
            CHECK(outbound_ack_matches(&gateway_ack_tx->outbound,
                                       MSG_GATEWAY_ACK,
                                       GATEWAY_ID,
                                       origin_packets[i].src_id,
                                       origin_packets[i].src_id,
                                       origin_packets[i].session_id,
                                       origin_packets[i].seq));
            CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
            CHECK(world.roles[origins[i]].relay.pending.state ==
                  MESH_RELAY_TX_IDLE);
            CHECK(mesh_sim_count_transitions(
                      &world,
                      MESH_SIM_TRANSITION_GATEWAY_ACKED,
                      origin_packets[i].src_id) == 1u);
            CHECK(close_shared_relay_downstream(&world,
                                                origins[i],
                                                relay) == MESH_SIM_OK);
            record_queue_depths(&world, max_queue_depths);
        }

        set_phase("shared-relay-final-health");
        CHECK(mesh_sim_count_transitions(
                  &world,
                  MESH_SIM_TRANSITION_GATEWAY_ACKED,
                  world.roles[relay].id) == 1u);
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_RETRY_READY,
                                         0u) == 0u);
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                         0u) == 0u);
        CHECK(mesh_sim_count_transitions(
                  &world,
                  MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
                  world.roles[relay].id) == 0u);
        CHECK(world.connections[upstream_connection].valid);
        CHECK(world.connections[upstream_connection].completed_repairs == 0u);
        CHECK(world.connections[upstream_connection].timing_a.timing_fresh);
        CHECK(world.connections[upstream_connection].timing_b.timing_fresh);
        CHECK(!world.connections[upstream_connection].timing_a.fallback_required);
        CHECK(!world.connections[upstream_connection].timing_b.fallback_required);
        CHECK(world.connections[upstream_connection].completed_events > 0u);
        {
            const struct route_candidate *upstream =
                route_selected(&world.roles[relay].relay.upstream);

            CHECK(upstream != NULL);
            CHECK(upstream->next_hop_id == GATEWAY_ID);
            CHECK(upstream->channel9_timing_valid);
        }
        for (size_t i = 0u; i < world.connection_count; i++) {
            CHECK(world.connections[i].completed_repairs == 0u);
            CHECK(world.connections[i].timing_a.timing_fresh);
            CHECK(world.connections[i].timing_b.timing_fresh);
            CHECK(!world.connections[i].timing_a.fallback_required);
            CHECK(!world.connections[i].timing_b.fallback_required);
        }
        CHECK(max_queue_depths[relay] == SHARED_RELAY_ORIGIN_COUNT + 1u);
        for (size_t i = 0u; i < world.role_count; i++) {
            CHECK(world.roles[i].route_discovery_requests == 0u);
            CHECK(max_queue_depths[i] <= SHARED_RELAY_MAX_QUEUE_DEPTH);
            CHECK(world.roles[i].watchdog.expirations == 0u);
            CHECK(world.roles[i].watchdog.resets == 0u);
        }
        CHECK(shared_relay_reservations_bounded(&world.roles[relay]));
        CHECK(network_idle(&world));
        CHECK(no_watchdog_expired(&world));
        CHECK(world.now_us <= SHARED_RELAY_MAX_DURATION_US);
        CHECK(world.last_error == MESH_SIM_OK);
    }
    return 0;
}

static int test_airtime_ack_loss_and_custody(void)
{
    struct proto_packet packet;
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint8_t origin;
    uint8_t relay;
    uint8_t gateway;
    uint16_t child_connection;
    uint16_t upstream_connection;
    uint32_t initial_deadline_ms;
    uint32_t deadline_before_progress_ms;
    uint32_t deadline_after_progress_ms;
    size_t transmission_count_before;
    size_t reception_count_before;
    const struct mesh_sim_transmission *tx;

    begin_case("airtime-ack-loss-custody", UINT32_C(0x5EED8001));
    set_phase("ack-airtime-add-roles");
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            ORIGIN_ID_BASE + 0x80u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &origin) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID_BASE + 0x80u,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &relay) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, origin, relay, 96u, 1u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay, gateway, 98u, 1u) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world,
                                 origin,
                                 relay,
                                 2u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world,
                                 relay,
                                 gateway,
                                 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_downlink(&world,
                                    relay,
                                    world.roles[origin].id,
                                    origin,
                                    1u,
                                    ROUTE_EPOCH) == MESH_SIM_OK);
    CHECK(mesh_sim_install_downlink(&world,
                                    gateway,
                                    world.roles[origin].id,
                                    relay,
                                    2u,
                                    ROUTE_EPOCH) == MESH_SIM_OK);
    CHECK(add_line_connection(&world,
                              origin,
                              relay,
                              ACK_AIRTIME_CHILD_PHASE_MS,
                              ACK_AIRTIME_CHILD_INTERVAL_MS,
                              &child_connection) == MESH_SIM_OK);
    CHECK(add_line_connection(&world,
                              relay,
                              gateway,
                              ACK_AIRTIME_UPSTREAM_PHASE_MS,
                              ACK_AIRTIME_UPSTREAM_INTERVAL_MS,
                              &upstream_connection) == MESH_SIM_OK);
    CHECK(arm_all_watchdogs(&world) == MESH_SIM_OK);

    set_phase("ack-airtime-queue-origin");
    CHECK(build_data_packet(world.roles[origin].id,
                            UINT16_C(0x810),
                            UINT32_C(0x810),
                            4u,
                            &packet,
                            payload,
                            sizeof(payload),
                            &payload_len) == PROTO_OK);
    CHECK(mesh_sim_queue_originated(&world,
                                    origin,
                                    &packet,
                                    payload,
                                    payload_len) == MESH_SIM_OK);

    set_phase("ack-airtime-first-hop-data");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == origin && tx->has_outbound);
    CHECK(packet_identity_matches(&tx->outbound.packet, &packet));
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    initial_deadline_ms =
        world.roles[origin].relay.pending.gateway_ack_deadline_ms;
    CHECK(initial_deadline_ms > world.now_us / 1000u);
    CHECK(queued_packet_count_for(&world.roles[relay],
                                  packet.src_id,
                                  packet.session_id,
                                  packet.seq) == 1u);

    set_phase("ack-airtime-original-gateway-delivery");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, upstream_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == relay && tx->has_outbound);
    CHECK(packet_identity_matches(&tx->outbound.packet, &packet));
    CHECK(world.roles[gateway].delivery_count == 1u);
    CHECK(packet_identity_matches(
              &world.roles[gateway].deliveries[0].packet, &packet));

    set_phase("ack-airtime-drop-first-hop-ack");
    transmission_count_before = world.transmission_count;
    reception_count_before = world.reception_count;
    CHECK(run_connection(&world, child_connection, true) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    CHECK(world.reception_count == reception_count_before);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == relay && tx->has_outbound);
    CHECK(outbound_ack_matches(&tx->outbound,
                               MSG_MESH_HOP_ACK,
                               world.roles[relay].id,
                               world.roles[origin].id,
                               world.roles[origin].id,
                               packet.session_id,
                               packet.seq));
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(world.roles[origin].relay.pending.gateway_ack_deadline_ms ==
          initial_deadline_ms);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_HOP_PROGRESS,
                                     world.roles[origin].id) == 0u);

    set_phase("ack-airtime-timeout-first-ownership");
    CHECK(mesh_sim_override_next_relay_random(&world, origin, 0u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_schedule_relay_tick(
              &world,
              origin,
              (uint64_t)initial_deadline_ms * 1000u) == MESH_SIM_OK);
    CHECK(mesh_sim_run_until(&world,
                             (uint64_t)initial_deadline_ms * 1000u) ==
          MESH_SIM_OK);
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_RETRY_BACKOFF);

    set_phase("ack-airtime-delay-first-gateway-ack");
    transmission_count_before = world.transmission_count;
    reception_count_before = world.reception_count;
    CHECK(run_connection(&world, upstream_connection, true) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    CHECK(world.reception_count == reception_count_before);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == gateway && tx->has_outbound);
    CHECK(outbound_ack_matches(&tx->outbound,
                               MSG_GATEWAY_ACK,
                               GATEWAY_ID,
                               packet.src_id,
                               world.roles[relay].id,
                               packet.session_id,
                               packet.seq));
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_RETRY_BACKOFF);

    set_phase("ack-airtime-retry-ready");
    CHECK(mesh_sim_schedule_relay_tick(
              &world,
              origin,
              (uint64_t)world.roles[origin].relay.pending.retry_after_ms *
                  1000u) == MESH_SIM_OK);
    CHECK(mesh_sim_run_until(
              &world,
              (uint64_t)world.roles[origin].relay.pending.retry_after_ms *
                  1000u) == MESH_SIM_OK);
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_RETRY_READY,
                                     world.roles[origin].id) == 1u);

    set_phase("ack-airtime-retransmit-same-identity");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == origin && tx->has_outbound);
    CHECK(packet_identity_matches(&tx->outbound.packet, &packet));
    CHECK(tx->outbound.payload_len == payload_len);
    CHECK(memcmp(tx->outbound.payload, payload, payload_len) == 0);
    deadline_before_progress_ms =
        world.roles[origin].relay.pending.gateway_ack_deadline_ms;
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    set_phase("ack-airtime-duplicate-gateway-reception");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, upstream_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == relay && tx->has_outbound);
    CHECK(packet_identity_matches(&tx->outbound.packet, &packet));
    CHECK(world.roles[gateway].delivery_count == 1u);
    CHECK(delivery_count_for(&world, gateway, packet.src_id, packet.seq) == 1u);

    set_phase("ack-airtime-hop-progress");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == relay && tx->has_outbound);
    CHECK(outbound_ack_matches(&tx->outbound,
                               MSG_MESH_HOP_ACK,
                               world.roles[relay].id,
                               world.roles[origin].id,
                               world.roles[origin].id,
                               packet.session_id,
                               packet.seq));
    deadline_after_progress_ms =
        world.roles[origin].relay.pending.gateway_ack_deadline_ms;
    CHECK(deadline_after_progress_ms > deadline_before_progress_ms);
    CHECK(deadline_after_progress_ms > world.now_us / 1000u);
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_HOP_PROGRESS,
                                     world.roles[origin].id) == 1u);

    set_phase("ack-airtime-gateway-ack-to-relay");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, upstream_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == gateway && tx->has_outbound);
    CHECK(outbound_ack_matches(&tx->outbound,
                               MSG_GATEWAY_ACK,
                               GATEWAY_ID,
                               packet.src_id,
                               world.roles[relay].id,
                               packet.session_id,
                               packet.seq));
    CHECK(world.roles[origin].relay.pending.state ==
          MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    set_phase("ack-airtime-final-gateway-ack");
    transmission_count_before = world.transmission_count;
    CHECK(run_connection(&world, child_connection, false) == MESH_SIM_OK);
    CHECK(world.transmission_count == transmission_count_before + 1u);
    tx = &world.transmissions[transmission_count_before];
    CHECK(tx->node_index == relay && tx->has_outbound);
    CHECK(outbound_ack_matches(&tx->outbound,
                               MSG_GATEWAY_ACK,
                               GATEWAY_ID,
                               packet.src_id,
                               packet.src_id,
                               packet.session_id,
                               packet.seq));
    CHECK(world.roles[origin].relay.pending.state == MESH_RELAY_TX_IDLE);
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                     world.roles[origin].id) == 1u);

    set_phase("ack-airtime-final-health");
    CHECK(world.roles[gateway].delivery_count == 1u);
    CHECK(network_idle(&world));
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                                     0u) == 0u);
    CHECK(mesh_sim_count_transitions(
              &world,
              MESH_SIM_TRANSITION_CONNECTION_REPAIR_STARTED,
              0u) == 0u);
    CHECK(world.connections[child_connection].completed_repairs == 0u);
    CHECK(world.connections[upstream_connection].completed_repairs == 0u);
    CHECK(world.connections[child_connection].timing_a.timing_fresh);
    CHECK(world.connections[child_connection].timing_b.timing_fresh);
    CHECK(world.connections[upstream_connection].timing_a.timing_fresh);
    CHECK(world.connections[upstream_connection].timing_b.timing_fresh);
    CHECK(!world.connections[child_connection].timing_a.fallback_required);
    CHECK(!world.connections[child_connection].timing_b.fallback_required);
    CHECK(!world.connections[upstream_connection].timing_a.fallback_required);
    CHECK(!world.connections[upstream_connection].timing_b.fallback_required);
    for (size_t i = 0u; i < world.role_count; i++) {
        CHECK(world.roles[i].route_discovery_requests == 0u);
        CHECK(world.roles[i].watchdog.expirations == 0u);
        CHECK(world.roles[i].watchdog.resets == 0u);
    }
    CHECK(no_watchdog_expired(&world));
    CHECK(world.now_us <= ACK_AIRTIME_MAX_DURATION_US);
    CHECK(world.last_error == MESH_SIM_OK);
    return 0;
}

struct scenario_entry {
    const char *name;
    int (*run)(void);
};

static const struct scenario_entry scenarios[] = {
    {"multi-origin", test_multi_origin_bursts},
    {"queue-pressure", test_queue_pressure_preserves_local_clicks},
    {"partition", test_partition_route_and_event_recovery},
    {"timing-repair", test_repeated_timing_expiry_and_repair},
    {"six-hop-fixture-routed", test_six_hop_fixture_routed_forwarding_capacity},
    {"event-reclamation", test_completed_event_reclamation},
    {"watchdog-worker", test_watchdog_requires_rx_worker_completion},
    {"shared-relay", test_four_origins_share_one_relay},
    {"ack-airtime", test_airtime_ack_loss_and_custody},
};

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr,
                "usage: %s [multi-origin|queue-pressure|partition|timing-repair|six-hop-fixture-routed|event-reclamation|watchdog-worker|shared-relay|ack-airtime]\n",
                argv[0]);
        return 2;
    }
    if (argc == 2) {
        for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
            if (strcmp(argv[1], scenarios[i].name) == 0) {
                return scenarios[i].run();
            }
        }
        fprintf(stderr, "unknown scenario selector: %s\n", argv[1]);
        return 2;
    }

    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (scenarios[i].run() != 0) {
            return 1;
        }
    }
    puts("mesh network stress scenarios passed");
    return 0;
}
