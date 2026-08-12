#include "mesh_sim_invariants.h"

#include "mesh_sim_internal.h"

#include <stdbool.h>
#include <string.h>

static int fail_invariant(struct mesh_sim_invariant_report *report,
                          enum mesh_sim_invariant_code code,
                          size_t node_index,
                          size_t object_index,
                          uint64_t detail,
                          const char *description)
{
    if (report != NULL) {
        *report = (struct mesh_sim_invariant_report) {
            .code = code,
            .node_index = node_index,
            .object_index = object_index,
            .detail = detail,
            .description = description,
        };
    }
    return MESH_SIM_ERR_PROTOCOL;
}

static bool delivery_identity_matches(const struct mesh_sim_delivery *a,
                                      const struct mesh_sim_delivery *b)
{
    return a->packet.msg_type == b->packet.msg_type &&
           a->packet.src_id == b->packet.src_id &&
           a->packet.dst_id == b->packet.dst_id &&
           a->packet.session_id == b->packet.session_id &&
           a->packet.seq == b->packet.seq;
}

static bool outbox_matches_pending(const struct mesh_relay *relay)
{
    const struct mesh_outbox_record *record = &relay->outbox_record;
    const struct mesh_pending_tx *pending = &relay->pending;
    enum mesh_relay_delivery_state state = record->delivery_state;

    if (!record->valid) {
        return true;
    }
    if (pending->state == MESH_RELAY_TX_IDLE || record->gateway_acked ||
        record->gateway_id != relay->gateway_id ||
        record->session_id != pending->packet.session_id ||
        record->seq != pending->packet.seq ||
        record->packet_class != pending->packet.msg_type ||
        record->payload_len != pending->payload_len ||
        record->payload_crc !=
            proto_crc16_ccitt_false(pending->payload, pending->payload_len) ||
        (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD &&
         !pending->gateway_ack_forward_pending) ||
        (pending->gateway_ack_forward_pending &&
         pending->packet.src_id == relay->local_id)) {
        return false;
    }
    if (pending->result_offer_active) {
        return state == MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK &&
               (pending->state == MESH_RELAY_TX_WAIT_RESULT_GRANT ||
                pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    }
    return state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK ||
           state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
}

static bool relay_has_connection_timing(const struct mesh_relay *relay,
                                        uint64_t peer_id)
{
    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static int check_connection_owner(
    const struct mesh_sim_world *world,
    size_t connection_index,
    struct mesh_sim_invariant_report *report)
{
    const struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    const struct mesh_event_owner *owner_a = &connection->owner_a;
    const struct mesh_event_owner *owner_b = &connection->owner_b;
    uint64_t node_a_id;
    uint64_t node_b_id;
    uint32_t now_ms;

    if (!connection->valid) {
        return MESH_SIM_OK;
    }
    if (connection->node_a >= world->role_count ||
        connection->node_b >= world->role_count ||
        connection->node_a == connection->node_b) {
        return fail_invariant(report, MESH_SIM_INVARIANT_CONNECTION_OWNER,
                              connection->node_a, connection_index, 0u,
                              "connection endpoints are invalid");
    }
    node_a_id = world->roles[connection->node_a].id;
    node_b_id = world->roles[connection->node_b].id;
    if ((owner_a->terminal && owner_a->active) ||
        (owner_b->terminal && owner_b->active)) {
        return fail_invariant(report, MESH_SIM_INVARIANT_CONNECTION_OWNER,
                              connection->node_a, connection_index,
                              owner_a->session_id,
                              "terminal connection ownership is still active");
    }
    if (owner_a->active != owner_b->active) {
        const struct mesh_event_owner *active_owner =
            owner_a->active ? owner_a : owner_b;
        const struct mesh_event_owner *terminal_owner =
            owner_a->active ? owner_b : owner_a;
        const struct mesh_event_timing *active_timing =
            owner_a->active ? &connection->timing_a : &connection->timing_b;
        const struct mesh_relay *active_relay = owner_a->active ?
            &world->roles[connection->node_a].relay :
            &world->roles[connection->node_b].relay;
        uint64_t active_peer_id = owner_a->active ? node_b_id : node_a_id;

        now_ms = mesh_sim_time_ms(world->now_us);
        if (terminal_owner->terminal &&
            terminal_owner->session_id == active_owner->session_id &&
            active_owner->session_id != 0u &&
            active_owner->peer_id == active_peer_id &&
            mesh_event_timing_usable(active_timing, now_ms) &&
            relay_has_connection_timing(active_relay, active_peer_id)) {
            /* A successfully transmitted END may be lost.  The peer retains
             * bounded ownership until supervision expires. */
            return MESH_SIM_OK;
        }
        return fail_invariant(report, MESH_SIM_INVARIANT_CONNECTION_OWNER,
                              connection->node_a, connection_index,
                              active_owner->session_id,
                              "connection ownership is orphaned past its bound");
    }
    if (!owner_a->active) {
        return MESH_SIM_OK;
    }
    if (connection->repair_pending || owner_a->session_id == 0u ||
        owner_a->session_id != owner_b->session_id ||
        owner_a->peer_id != node_b_id || owner_b->peer_id != node_a_id) {
        return fail_invariant(report, MESH_SIM_INVARIANT_CONNECTION_OWNER,
                              connection->node_a, connection_index,
                              owner_a->session_id,
                              "paired connection ownership identity is mismatched");
    }
    /* Both endpoints may retain the same old owner while a bounded fallback
     * or Channel-5 repair is being planned.  Settled-state validation below
     * requires usable paired timing; incremental validation only requires
     * that the ownership identity cannot split or alias another operation. */
    return MESH_SIM_OK;
}

static int check_role(const struct mesh_sim_world *world,
                      size_t node_index,
                      struct mesh_sim_invariant_report *report)
{
    const struct mesh_sim_role_instance *node = &world->roles[node_index];
    size_t queue_count = 0u;
    size_t transit_work_count = 0u;

    if (node->id == 0u || node->work_epoch == 0u ||
        node->tx_queue_capacity == 0u ||
        node->tx_queue_capacity > MESH_SIM_TX_QUEUE_CAPACITY ||
        node->tx_queue_count > node->tx_queue_capacity) {
        return fail_invariant(report, MESH_SIM_INVARIANT_CAPACITY,
                              node_index, 0u, node->tx_queue_count,
                              "role identity, epoch, or queue capacity is invalid");
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *queued = &node->tx_queue[i];

        if (!queued->valid) {
            continue;
        }
        queue_count++;
        if ((queued->outbound.next_hop_id == 0u &&
             queued->outbound.packet.dst_id != MESH_BROADCAST_ID) ||
            queued->outbound.payload_len != queued->outbound.packet.payload_len ||
            queued->outbound.payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
            return fail_invariant(report, MESH_SIM_INVARIANT_QUEUE_ENTRY,
                                  node_index, i,
                                  queued->outbound.packet.seq,
                                  "queued frame ownership or payload length is invalid");
        }
    }
    if (queue_count != node->tx_queue_count) {
        return fail_invariant(report, MESH_SIM_INVARIANT_QUEUE_COUNT,
                              node_index, queue_count, node->tx_queue_count,
                              "queue count disagrees with valid queue slots");
    }
    if ((uint64_t)node->watchdog.workers_started !=
            (uint64_t)node->watchdog.workers_completed +
                node->watchdog.workers_aborted +
                node->watchdog.workers_active ||
        node->watchdog.recoverable_completions >
            node->watchdog.workers_completed) {
        return fail_invariant(report, MESH_SIM_INVARIANT_WORKER_ACCOUNTING,
                              node_index, 0u,
                              node->watchdog.workers_started,
                              "watchdog worker lifecycle accounting is unbalanced");
    }
    if (!node->relay_initialized) {
        return MESH_SIM_OK;
    }
    if (node->relay.pending.state < MESH_RELAY_TX_IDLE ||
        node->relay.pending.state > MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD ||
        (node->relay.pending.state != MESH_RELAY_TX_IDLE &&
         ((node->relay.pending.next_hop_id == 0u &&
           node->relay.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) ||
          node->relay.pending.payload_len != node->relay.pending.packet.payload_len ||
          node->relay.pending.payload_len > UWB_MESH_MAX_PAYLOAD_LEN))) {
        return fail_invariant(report, MESH_SIM_INVARIANT_RELAY_STATE,
                              node_index, 0u, node->relay.pending.state,
                              "relay transaction state or owned frame is invalid");
    }
    if (!outbox_matches_pending(&node->relay)) {
        return fail_invariant(report, MESH_SIM_INVARIANT_RELAY_STATE,
                              node_index, 0u,
                              node->relay.outbox_record.delivery_state,
                              "valid outbox no longer matches pending custody ownership");
    }
    if (node->runtime.relay != &node->relay ||
        node->runtime.local_id != node->id ||
        node->runtime.ops.ctx != node ||
        node->runtime.radio_owner < MESH_RUNTIME_RADIO_NONE ||
        node->runtime.radio_owner > MESH_RUNTIME_RADIO_TRANSIT) {
        return fail_invariant(report, MESH_SIM_INVARIANT_RUNTIME_STATE,
                              node_index, 0u, node->runtime.radio_owner,
                              "runtime owner or relay attachment is invalid");
    }
    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        const struct mesh_runtime_work *work = &node->runtime.work[i];

        if (!work->valid) {
            continue;
        }
        if (work->kind < MESH_RUNTIME_WORK_GATEWAY_COMMAND ||
            work->kind > MESH_RUNTIME_WORK_TRANSIT) {
            return fail_invariant(report, MESH_SIM_INVARIANT_RUNTIME_STATE,
                                  node_index, i, work->kind,
                                  "runtime work kind is outside the production state space");
        }
        if (work->kind == MESH_RUNTIME_WORK_TRANSIT) {
            transit_work_count++;
            if (work->token != node->runtime.transit.packet.seq) {
                return fail_invariant(report, MESH_SIM_INVARIANT_RUNTIME_STATE,
                                      node_index, i, work->token,
                                      "transit work no longer owns its reserved packet");
            }
        }
        for (size_t j = i + 1u; j < MESH_RUNTIME_WORK_CAPACITY; j++) {
            const struct mesh_runtime_work *other = &node->runtime.work[j];

            if (other->valid && other->kind == work->kind &&
                other->token == work->token) {
                return fail_invariant(report, MESH_SIM_INVARIANT_RUNTIME_STATE,
                                      node_index, j, work->token,
                                      "runtime contains a re-entrant duplicate work identity");
            }
        }
    }
    if ((node->runtime.transit_reserved && transit_work_count != 1u) ||
        (!node->runtime.transit_reserved && transit_work_count != 0u)) {
        return fail_invariant(report, MESH_SIM_INVARIANT_RUNTIME_STATE,
                              node_index, transit_work_count,
                              node->runtime.transit.packet.seq,
                              "transit reservation and scheduled work disagree");
    }
    for (size_t i = 0u; i < node->delivery_count; i++) {
        const struct mesh_sim_delivery *delivery = &node->deliveries[i];

        if (delivery->payload_len != delivery->packet.payload_len ||
            delivery->payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
            return fail_invariant(report, MESH_SIM_INVARIANT_SEMANTIC_COUNT,
                                  node_index, i, delivery->payload_len,
                                  "delivered frame payload is inconsistent");
        }
        if (node->role == MESH_SIM_ROLE_GATEWAY &&
            delivery->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
            return fail_invariant(
                report,
                MESH_SIM_INVARIANT_SEMANTIC_COUNT,
                node_index,
                i,
                delivery->packet.seq,
                "gateway ACK-confirm leaked into host semantic delivery");
        }
        for (size_t j = i + 1u; j < node->delivery_count; j++) {
            if (delivery_identity_matches(delivery, &node->deliveries[j])) {
                return fail_invariant(report,
                                      MESH_SIM_INVARIANT_DUPLICATE_DELIVERY,
                                      node_index, j, delivery->packet.seq,
                                      "one transaction produced two semantic deliveries");
            }
        }
    }
    if (node->role == MESH_SIM_ROLE_GATEWAY &&
        node->gateway_semantic_commit_count > node->delivery_count) {
        return fail_invariant(report, MESH_SIM_INVARIANT_SEMANTIC_COUNT,
                              node_index, node->delivery_count,
                              node->gateway_semantic_commit_count,
                              "gateway committed more transactions than it delivered");
    }
    return MESH_SIM_OK;
}

int mesh_sim_check_invariants(const struct mesh_sim_world *world,
                              struct mesh_sim_invariant_report *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    if (world == NULL) {
        return fail_invariant(report, MESH_SIM_INVARIANT_WORLD_ERROR,
                              0u, 0u, 0u, "world is null");
    }
    if (world->last_error != MESH_SIM_OK) {
        return fail_invariant(report, MESH_SIM_INVARIANT_WORLD_ERROR,
                              0u, 0u, (uint32_t)world->last_error,
                              "simulator recorded a terminal error");
    }
    if (world->role_count > MESH_SIM_MAX_ROLES ||
        world->connection_count > MESH_SIM_MAX_CONNECTIONS ||
        world->event_count > MESH_SIM_MAX_EVENTS ||
        world->rx_window_count > MESH_SIM_MAX_RX_WINDOWS ||
        world->transmission_count > MESH_SIM_MAX_TRANSMISSIONS ||
        world->reception_count > MESH_SIM_MAX_RECEPTIONS ||
        world->transition_count > MESH_SIM_MAX_TRANSITIONS) {
        return fail_invariant(report, MESH_SIM_INVARIANT_CAPACITY,
                              0u, 0u, world->event_count,
                              "world telemetry or scheduler capacity is invalid");
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        int ret = check_role(world, i, report);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        int ret = check_connection_owner(world, i, report);

        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    /*
     * Transmissions and receive windows are retained as trace telemetry after
     * they finish.  Only a pending scheduler event makes one live work that
     * could cross an operation-generation boundary.
     */
    for (size_t i = 0u; i < world->event_count; i++) {
        const struct mesh_sim_event *event = &world->events[i];

        if (!event->pending) {
            continue;
        }
        if (event->type == SIM_EVENT_TX_START ||
            event->type == SIM_EVENT_TX_END ||
            event->type == SIM_EVENT_TX_EVALUATE) {
            const struct mesh_sim_transmission *tx;

            if (event->object_index >= world->transmission_count) {
                return fail_invariant(report, MESH_SIM_INVARIANT_STALE_WORK,
                                      0u, i, event->object_index,
                                      "pending transmission event has no object");
            }
            tx = &world->transmissions[event->object_index];
            if (!tx->valid || tx->node_index >= world->role_count ||
                tx->work_epoch != world->roles[tx->node_index].work_epoch) {
                return fail_invariant(report, MESH_SIM_INVARIANT_STALE_WORK,
                                      tx->node_index, event->object_index,
                                      tx->work_epoch,
                                      "stale transmission event crossed a generation boundary");
            }
        } else if (event->type == SIM_EVENT_RX_START ||
                   event->type == SIM_EVENT_RX_END) {
            const struct mesh_sim_rx_window *window;

            if (event->object_index >= world->rx_window_count) {
                return fail_invariant(report, MESH_SIM_INVARIANT_STALE_WORK,
                                      0u, i, event->object_index,
                                      "pending receive event has no object");
            }
            window = &world->rx_windows[event->object_index];
            if (!window->valid || window->node_index >= world->role_count ||
                window->work_epoch !=
                    world->roles[window->node_index].work_epoch) {
                return fail_invariant(report, MESH_SIM_INVARIANT_STALE_WORK,
                                      window->node_index, event->object_index,
                                      window->work_epoch,
                                      "stale receive event crossed a generation boundary");
            }
        } else if (event->type == SIM_EVENT_RELAY_TICK) {
            if (event->object_index >= world->role_count ||
                event->token == 0u) {
                return fail_invariant(report, MESH_SIM_INVARIANT_STALE_WORK,
                                      0u, i, event->token,
                                      "pending relay callback has no operation generation");
            }
        }
    }
    return MESH_SIM_OK;
}

int mesh_sim_check_settled(const struct mesh_sim_world *world,
                           struct mesh_sim_invariant_report *report)
{
    int ret = mesh_sim_check_invariants(world, report);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    for (size_t i = 0u; i < world->event_count; i++) {
        const struct mesh_sim_event *event = &world->events[i];

        if (event->pending && event->type != SIM_EVENT_WATCHDOG_EXPIRE) {
            return fail_invariant(report, MESH_SIM_INVARIANT_PENDING_EVENT,
                                  0u, i, event->type,
                                  "finite scenario ended with scheduled non-watchdog work");
        }
    }
    for (size_t i = 0u; i < world->role_count; i++) {
        const struct mesh_sim_role_instance *node = &world->roles[i];

        if (node->tx_queue_count != 0u || node->route_waiting_valid ||
            (node->relay_initialized &&
             (node->relay.pending.state != MESH_RELAY_TX_IDLE ||
              node->relay.route_discovery.active ||
              node->relay.result_bundle.active ||
              node->relay.result_bundle.record_count != 0u ||
              node->relay.result_offer_reservation.valid)) ||
            node->relay_timer_guard.valid ||
            node->runtime.transit_reserved || node->runtime.event_repair_pending ||
            node->runtime.radio_owner != MESH_RUNTIME_RADIO_NONE ||
            node->radio_state != MESH_SIM_RADIO_SLEEP ||
            node->watchdog.workers_active != 0u) {
            return fail_invariant(report, MESH_SIM_INVARIANT_NOT_SETTLED,
                                  i, 0u, node->relay.pending.state,
                                  "role still owns queue, timer, discovery, custody, or radio work");
        }
        for (size_t j = 0u; j < MESH_RUNTIME_WORK_CAPACITY; j++) {
            if (node->runtime.work[j].valid) {
                return fail_invariant(report, MESH_SIM_INVARIANT_NOT_SETTLED,
                                      i, j, node->runtime.work[j].token,
                                      "role still owns deferred runtime work");
            }
        }
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        const struct mesh_sim_connection *connection = &world->connections[i];

        if (connection->valid &&
            connection->owner_a.active != connection->owner_b.active) {
            return fail_invariant(report, MESH_SIM_INVARIANT_NOT_SETTLED,
                                  connection->node_a, i,
                                  connection->owner_a.active ?
                                      connection->owner_a.session_id :
                                      connection->owner_b.session_id,
                                  "finite scenario ended during lost-END supervision");
        }
        if (connection->valid && connection->owner_a.active &&
            connection->owner_b.active &&
            (!mesh_event_timing_usable(
                 &connection->timing_a, mesh_sim_time_ms(world->now_us)) ||
             !mesh_event_timing_usable(
                 &connection->timing_b, mesh_sim_time_ms(world->now_us)) ||
             !relay_has_connection_timing(
                 &world->roles[connection->node_a].relay,
                 world->roles[connection->node_b].id) ||
             !relay_has_connection_timing(
                 &world->roles[connection->node_b].relay,
                 world->roles[connection->node_a].id))) {
            return fail_invariant(report, MESH_SIM_INVARIANT_NOT_SETTLED,
                                  connection->node_a, i,
                                  connection->owner_a.session_id,
                                  "active connection owner has no settled paired timing");
        }
    }
    return MESH_SIM_OK;
}

const char *mesh_sim_invariant_name(enum mesh_sim_invariant_code code)
{
    switch (code) {
    case MESH_SIM_INVARIANT_NONE:
        return "none";
    case MESH_SIM_INVARIANT_WORLD_ERROR:
        return "world-error";
    case MESH_SIM_INVARIANT_CAPACITY:
        return "capacity";
    case MESH_SIM_INVARIANT_QUEUE_COUNT:
        return "queue-count";
    case MESH_SIM_INVARIANT_QUEUE_ENTRY:
        return "queue-entry";
    case MESH_SIM_INVARIANT_RELAY_STATE:
        return "relay-state";
    case MESH_SIM_INVARIANT_RUNTIME_STATE:
        return "runtime-state";
    case MESH_SIM_INVARIANT_CONNECTION_OWNER:
        return "connection-owner";
    case MESH_SIM_INVARIANT_WORKER_ACCOUNTING:
        return "worker-accounting";
    case MESH_SIM_INVARIANT_STALE_WORK:
        return "stale-work";
    case MESH_SIM_INVARIANT_DUPLICATE_DELIVERY:
        return "duplicate-delivery";
    case MESH_SIM_INVARIANT_SEMANTIC_COUNT:
        return "semantic-count";
    case MESH_SIM_INVARIANT_PENDING_EVENT:
        return "pending-event";
    case MESH_SIM_INVARIANT_NOT_SETTLED:
        return "not-settled";
    }
    return "unknown";
}
