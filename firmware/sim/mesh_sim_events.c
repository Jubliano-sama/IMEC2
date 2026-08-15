#include "mesh_sim_internal.h"

#include <limits.h>
#include <string.h>

_Static_assert(MESH_SIM_MAX_ROLES <= UINT8_MAX,
               "simulator role indices must fit radio/watchdog APIs");
_Static_assert(MESH_SIM_MAX_ROLES <= UINT16_MAX,
               "simulator role indices must fit scheduler events");
_Static_assert(MESH_SIM_MAX_RX_WINDOWS <= UINT32_MAX,
               "simulator RX-window indices must fit trace detail");

static void watchdog_worker_started(struct mesh_sim_world *world,
                                    uint8_t node_index);

int mesh_sim_events_runtime_schedule_cb(enum mesh_runtime_work_kind kind,
                               uint64_t token,
                               uint64_t at_us,
                               void *ctx)
{
    struct mesh_sim_role_instance *node = ctx;

    (void)kind;
    (void)token;
    if (node == NULL || node->world == NULL) {
        return MESH_RUNTIME_ERR_ARG;
    }
    return mesh_sim_scheduler_schedule(node->world,
                          SIM_EVENT_RUNTIME_BOUNDARY,
                          at_us,
                          node->node_index);
}

void mesh_sim_events_runtime_trace_cb(enum mesh_runtime_action_kind action,
                             uint64_t token,
                             uint64_t at_us,
                             void *ctx)
{
    struct mesh_sim_role_instance *node = ctx;
    enum mesh_sim_transition_kind transition;

    if (node == NULL || node->world == NULL) {
        return;
    }
    switch (action) {
    case MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND:
        transition = MESH_SIM_TRANSITION_RUNTIME_GATEWAY_COMMAND;
        break;
    case MESH_RUNTIME_ACTION_START_LOCAL_CLICK:
        transition = MESH_SIM_TRANSITION_RUNTIME_LOCAL_CLICK;
        break;
    case MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT:
        transition = MESH_SIM_TRANSITION_RUNTIME_EVENT_REPAIR;
        break;
    case MESH_RUNTIME_ACTION_RUN_TRANSIT:
        transition = MESH_SIM_TRANSITION_RUNTIME_TRANSIT;
        break;
    case MESH_RUNTIME_ACTION_NONE:
    case MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY:
    default:
        return;
    }
    (void)mesh_sim_trace_add(node->world,
                         at_us,
                         node->id,
                         0u,
                         transition,
                         0u,
                         token > UINT32_MAX ? UINT32_MAX : (uint32_t)token);
}

int mesh_sim_runtime_claim_radio(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 enum mesh_runtime_radio_owner owner,
                                 uint64_t start_us,
                                 uint64_t end_us)
{
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index) || start_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_runtime_claim_radio(&world->roles[node_index].runtime,
                                   owner,
                                   start_us,
                                   end_us);
    if (ret != MESH_RUNTIME_OK) {
        return ret;
    }
    watchdog_worker_started(world, node_index);
    return mesh_sim_scheduler_schedule(world,
                          SIM_EVENT_RUNTIME_RADIO_RELEASE,
                          end_us,
                          node_index);
}

int mesh_sim_watchdog_arm(struct mesh_sim_world *world,
                          uint8_t node_index,
                          uint64_t timeout_us,
                          enum mesh_sim_watchdog_action action)
{
    struct mesh_sim_role_instance *node;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index) || timeout_us == 0u ||
        action > MESH_SIM_WATCHDOG_RESET_ROLE ||
        world->now_us > UINT64_MAX - timeout_us) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    memset(&node->watchdog, 0, sizeof(node->watchdog));
    node->watchdog.timeout_us = timeout_us;
    node->watchdog.last_feed_us = world->now_us;
    node->watchdog.deadline_us = world->now_us + timeout_us;
    node->watchdog.expiry_generation = 1u;
    node->watchdog.action = action;
    node->watchdog.armed = true;
    ret = mesh_sim_trace_add(world, world->now_us, node->id, 0u,
                         MESH_SIM_TRANSITION_WATCHDOG_ARMED, 0u,
                         timeout_us > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout_us);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_scheduler_reschedule_watchdog(world,
                                                 node_index,
                                                 node->watchdog.deadline_us,
                                                 node->watchdog.expiry_generation);
    if (ret == MESH_SIM_OK) {
        node->watchdog.expiry_event_pending = true;
        return MESH_SIM_OK;
    }
    return mesh_sim_fail(world, ret);
}

int mesh_sim_watchdog_feed(struct mesh_sim_world *world, uint8_t node_index)
{
    struct mesh_sim_role_instance *node;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (!node->watchdog.armed || node->watchdog.expired) {
        return MESH_SIM_OK;
    }
    if (world->now_us > UINT64_MAX - node->watchdog.timeout_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    node->watchdog.last_feed_us = world->now_us;
    node->watchdog.deadline_us = world->now_us + node->watchdog.timeout_us;
    node->watchdog.expiry_generation++;
    if (node->watchdog.expiry_generation == 0u) {
        node->watchdog.expiry_generation = 1u;
    }
    node->watchdog.feeds++;
    ret = mesh_sim_trace_add(world, world->now_us, node->id, 0u,
                         MESH_SIM_TRANSITION_WATCHDOG_FED, 0u,
                         node->watchdog.feeds);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = mesh_sim_scheduler_reschedule_watchdog(world,
                                                 node_index,
                                                 node->watchdog.deadline_us,
                                                 node->watchdog.expiry_generation);
    if (ret == MESH_SIM_OK) {
        node->watchdog.expiry_event_pending = true;
        return MESH_SIM_OK;
    }
    return mesh_sim_fail(world, ret);
}

static void watchdog_worker_started(struct mesh_sim_world *world,
                                    uint8_t node_index)
{
    struct mesh_sim_watchdog *watchdog = &world->roles[node_index].watchdog;

    if (!watchdog->armed || watchdog->expired) {
        return;
    }
    watchdog->workers_started++;
    watchdog->workers_active++;
    watchdog->radio_lease_state = MESH_SIM_RADIO_LEASE_WORKER_STARTED;
}

static int watchdog_worker_completed(struct mesh_sim_world *world,
                                     uint8_t node_index,
                                     bool recoverable)
{
    struct mesh_sim_watchdog *watchdog = &world->roles[node_index].watchdog;

    if (!watchdog->armed || watchdog->expired) {
        return MESH_SIM_OK;
    }
    if (watchdog->workers_active == 0u) {
        return MESH_SIM_OK;
    }
    watchdog->workers_active--;
    watchdog->workers_completed++;
    if (recoverable) {
        watchdog->recoverable_completions++;
    }
    watchdog->radio_lease_state = MESH_SIM_RADIO_LEASE_COMPLETED_RECOVERABLE;
    return mesh_sim_watchdog_feed(world, node_index);
}

static int watchdog_abort_workers(struct mesh_sim_world *world,
                                  uint8_t node_index,
                                  uint32_t worker_count)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_watchdog *watchdog = &node->watchdog;
    uint32_t aborted_workers = worker_count < watchdog->workers_active ?
                               worker_count : watchdog->workers_active;

    watchdog->workers_active -= aborted_workers;
    while (aborted_workers-- > 0u) {
        int ret;

        watchdog->workers_aborted++;
        ret = mesh_sim_trace_add(world,
                             world->now_us,
                             node->id,
                             0u,
                             MESH_SIM_TRANSITION_WATCHDOG_WORKER_ABORTED,
                             0u,
                             watchdog->workers_aborted);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    return MESH_SIM_OK;
}

static int watchdog_abort_active_workers(struct mesh_sim_world *world,
                                         uint8_t node_index)
{
    return watchdog_abort_workers(world,
                                  node_index,
                                  world->roles[node_index].watchdog.workers_active);
}

static void advance_work_epoch(struct mesh_sim_role_instance *node)
{
    node->work_epoch++;
    if (node->work_epoch == 0u) {
        node->work_epoch = 1u;
    }
}

static int reset_relay_generation(struct mesh_sim_world *world,
                                  struct mesh_sim_role_instance *node)
{
    struct mesh_relay *relay = &node->relay;

    if (!node->relay_initialized) {
        return MESH_SIM_OK;
    }

    /*
     * Routes and downlinks are simulator-provisioned topology, so they remain
     * attached across this volatile reboot boundary. The resetting endpoint's
     * connection timing is invalidated before this reset; a connected peer's
     * timing remains under its own supervision deadline. Every
     * operation-owned relay field returns to relay-init state; no
     * outbox/custody transaction is implicitly restored from persistence.
     */
    memset(relay->duplicates, 0, sizeof(relay->duplicates));
    memset(relay->flood_seen, 0, sizeof(relay->flood_seen));
    memset(&relay->pending, 0, sizeof(relay->pending));
    memset(&relay->outbox_record, 0, sizeof(relay->outbox_record));
    memset(&relay->route_discovery, 0, sizeof(relay->route_discovery));
    memset(&relay->result_bundle, 0, sizeof(relay->result_bundle));
    memset(&relay->result_offer_reservation,
           0,
           sizeof(relay->result_offer_reservation));
    memset(&relay->diagnostics, 0, sizeof(relay->diagnostics));
    relay->duplicate_next = 0u;
    relay->flood_seen_next = 0u;
    relay->next_seq = 1u;

    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        struct mesh_gateway_ack_store *store = &world->gateway_ack_store;

        mesh_gateway_ack_store_init(store);
        relay->gateway_ack_store = store;
    } else {
        struct mesh_anchor_downlink_store *store = &node->anchor_route_store;

        /* The anchor sidecar contains provisioned route topology only. */
        relay->anchor_downlink_store = store;
    }
    return MESH_SIM_OK;
}

static void reset_runtime_generation(struct mesh_sim_role_instance *node,
                                     uint64_t now_us)
{
    const struct mesh_runtime_ops ops = node->runtime.ops;

    /*
     * A watchdog reboot is an operation-generation boundary.  Reinitializing
     * the complete runtime, rather than just releasing its radio owner, drops
     * queued callbacks and transit reservations from the expired generation.
     * The freshly reset relay and its configured topology remain attached;
     * persistence restore is deliberately outside this simulator boundary.
     */
    mesh_runtime_init(&node->runtime,
                      node->relay_initialized ? &node->relay : NULL,
                      node->id,
                      &ops);
    node->runtime.radio_busy_until_us = now_us;
}

static bool abort_repair_endpoint_radio(struct mesh_sim_world *world,
                                        uint8_t node_index,
                                        uint64_t repair_end_us)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];

    if (node->runtime.radio_owner != MESH_RUNTIME_RADIO_CHANNEL9_EVENT ||
        node->runtime.radio_busy_until_us != repair_end_us) {
        return false;
    }
    node->runtime.radio_owner = MESH_RUNTIME_RADIO_NONE;
    node->runtime.radio_busy_until_us = world->now_us;
    node->radio_state = MESH_SIM_RADIO_SLEEP;
    dwm3000_runtime_init(&node->dwm3000);
    return true;
}

static int abort_connection_repairs_for_reset(struct mesh_sim_world *world,
                                              uint8_t reset_node_index)
{
    for (size_t i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection *connection = &world->connections[i];
        uint8_t peer_index;
        bool reset_owned_radio;
        bool peer_owned_radio;
        int ret;

        if (!connection->repair_pending ||
            (connection->node_a != reset_node_index &&
             connection->node_b != reset_node_index)) {
            continue;
        }
        peer_index = connection->node_a == reset_node_index ?
                     connection->node_b : connection->node_a;
        mesh_sim_scheduler_cancel_connection_repair(world, (uint16_t)i);
        reset_owned_radio = abort_repair_endpoint_radio(
            world,
            reset_node_index,
            connection->repair_end_us);
        peer_owned_radio = abort_repair_endpoint_radio(
            world,
            peer_index,
            connection->repair_end_us);
        if (peer_owned_radio) {
            ret = watchdog_abort_workers(world, peer_index, 1u);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
        (void)reset_owned_radio;
        connection->repair_pending = false;
        connection->repair_propose_decoded = false;
        connection->repair_accept_decoded = false;
        connection->repair_session_id = 0u;
        connection->repair_seq = 0u;
    }
    return MESH_SIM_OK;
}

static bool transmission_work_current(const struct mesh_sim_world *world,
                                      const struct mesh_sim_transmission *tx)
{
    return tx->valid && tx->node_index < world->role_count &&
           tx->work_epoch == world->roles[tx->node_index].work_epoch;
}

static bool rx_window_work_current(const struct mesh_sim_world *world,
                                   const struct mesh_sim_rx_window *window)
{
    return window->valid && window->node_index < world->role_count &&
           window->work_epoch == world->roles[window->node_index].work_epoch;
}

static int reset_role_state(struct mesh_sim_world *world, uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_watchdog *watchdog = &node->watchdog;
    int ret;

    advance_work_epoch(node);
    for (size_t i = 0u; i < world->connection_count; i++) {
        struct mesh_sim_connection *connection = &world->connections[i];
        uint8_t peer_index;
        struct mesh_event_owner *owner;
        struct mesh_event_timing *timing;

        if (connection->node_a == node_index ||
            connection->node_b == node_index) {
            peer_index = connection->node_a == node_index ?
                         connection->node_b : connection->node_a;
            owner = connection->node_a == node_index ?
                    &connection->owner_a : &connection->owner_b;
            timing = connection->node_a == node_index ?
                     &connection->timing_a : &connection->timing_b;

            /* A reset erases only the resetting endpoint's volatile event
             * owner and timing.  The peer keeps its live owner and timing
             * until its normal supervision bound expires; clearing those
             * fields here would make reset recovery pass without exercising
             * the stale-peer protocol path. */
            mesh_event_owner_abandon(owner);
            owner->terminal = true;
            timing->route_fresh = false;
            timing->timing_fresh = false;
            timing->fallback_required = true;
            mesh_relay_clear_channel9_timing(
                &node->relay,
                world->roles[peer_index].id);
        }
    }
    ret = abort_connection_repairs_for_reset(world, node_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    mesh_sim_scheduler_cancel_role_work(world, node_index);
    ret = watchdog_abort_active_workers(world, node_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    node->radio_state = MESH_SIM_RADIO_SLEEP;
    dwm3000_runtime_init(&node->dwm3000);
    ret = reset_relay_generation(world, node);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    reset_runtime_generation(node, world->now_us);
    memset(node->tx_queue, 0, sizeof(node->tx_queue));
    node->tx_queue_count = 0u;
    node->route_waiting_valid = false;
    memset(&node->route_reply_upstream_ack,
           0,
           sizeof(node->route_reply_upstream_ack));
    node->route_reply_upstream_ack_valid = false;
    node->resume_low_duty_after_ds_twr = false;
    node->event_control_seq = 0u;
    node->event_operation_session_next = 0u;
    {
        uint64_t previous_boot_nonce = node->event_boot_nonce;

        do {
            node->event_boot_nonce =
                ((uint64_t)mesh_sim_random(world) << 32) |
                (uint64_t)mesh_sim_random(world);
        } while (node->event_boot_nonce == 0u ||
                 node->event_boot_nonce == previous_boot_nonce);
    }
    node->relay_timer_guard.generation++;
    if (node->relay_timer_guard.generation == 0u) {
        node->relay_timer_guard.generation = 1u;
    }
    node->relay_timer_guard.valid = false;
    watchdog->armed = false;
    watchdog->expiry_event_pending = false;
    watchdog->resets++;
    watchdog->radio_lease_state = MESH_SIM_RADIO_LEASE_RESET;
    return mesh_sim_trace_add(world, world->now_us, node->id, 0u,
                          MESH_SIM_TRANSITION_WATCHDOG_RESET, 0u,
                          watchdog->resets);
}

int mesh_sim_reset_role(struct mesh_sim_world *world, uint8_t node_index)
{
    if (!mesh_sim_node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    return reset_role_state(world, node_index);
}

static int process_watchdog_expiry(struct mesh_sim_world *world,
                                   uint8_t node_index,
                                   uint32_t generation)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_watchdog *watchdog = &node->watchdog;
    uint32_t detail;
    int ret;

    watchdog->expiry_event_pending = false;
    if (!watchdog->armed || watchdog->expired ||
        generation != watchdog->expiry_generation ||
        world->now_us != watchdog->deadline_us) {
        return MESH_SIM_OK;
    }
    watchdog->expired = true;
    watchdog->feeds_stopped = true;
    watchdog->radio_lease_state = MESH_SIM_RADIO_LEASE_EXPIRED;
    watchdog->expirations++;
    watchdog->expired_radio_owner = node->runtime.radio_owner;
    watchdog->expired_radio_state = node->radio_state;
    watchdog->expired_pending_state = node->relay.pending.state;
    watchdog->expired_queue_count = node->tx_queue_count > UINT16_MAX ?
                                    UINT16_MAX : (uint16_t)node->tx_queue_count;
    detail = ((uint32_t)watchdog->expired_queue_count << 16) |
             ((uint32_t)watchdog->expired_pending_state << 8) |
             (uint32_t)watchdog->expired_radio_state;
    ret = mesh_sim_trace_add(world, world->now_us, node->id,
                         (uint64_t)watchdog->expired_radio_owner,
                         MESH_SIM_TRANSITION_WATCHDOG_EXPIRED, 0u, detail);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    watchdog->armed = false;
    if (watchdog->action == MESH_SIM_WATCHDOG_FAIL) {
        return mesh_sim_fail(world, MESH_SIM_ERR_WATCHDOG);
    }

    return reset_role_state(world, node_index);
}

static int dispatch_received_packet(struct mesh_sim_world *world,
                                    uint8_t receiver_index,
                                    uint8_t sender_index,
                                    const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    return mesh_sim_relay_dispatch_packet(world,
                                          receiver_index,
                                          sender_index,
                                          packet,
                                          payload,
                                          payload_len);
}

static const struct mesh_sim_connection_callbacks connection_callbacks = {
    .worker_started = watchdog_worker_started,
    .worker_completed = watchdog_worker_completed,
};

static int claim_radio_from_event_dispatch(
    void *context,
    uint8_t node_index,
    enum mesh_runtime_radio_owner owner,
    uint64_t start_us,
    uint64_t end_us)
{
    return mesh_sim_runtime_claim_radio(context,
                                        node_index,
                                        owner,
                                        start_us,
                                        end_us);
}

static int process_tx_end(struct mesh_sim_world *world, size_t tx_index)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    struct mesh_sim_role_instance *node;
    int ret;

    if (!transmission_work_current(world, tx)) {
        return MESH_SIM_OK;
    }
    node = &world->roles[tx->node_index];
    if (node->radio_state != MESH_SIM_RADIO_TX) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (tx->dwm_runtime_owned) {
        ret = dwm3000_runtime_finish_tx(&node->dwm3000, world->now_us);
        if (ret != DWM3000_RUNTIME_OK) {
            return mesh_sim_fail(world, ret);
        }
    }
    node->radio_state = mesh_sim_radio_post_operation_state(node, world->now_us);
    if (tx->has_outbound) {
        mesh_relay_note_tx_sent(&node->relay,
                                &tx->outbound,
                                mesh_sim_time_ms(world->now_us));
        if (tx->connection_event_index != UINT16_MAX &&
            tx->connection_event_index < world->connection_event_count) {
            struct mesh_sim_connection_event *connection_event =
                &world->connection_events[tx->connection_event_index];

            mesh_relay_note_channel9_unobserved_turn(
                &node->relay,
                world->roles[connection_event->receiver_index].id,
                mesh_sim_time_ms(connection_event->start_us));
        }
    }
    ret = mesh_sim_trace_add(world,
                             world->now_us,
                             node->id,
                             tx->has_outbound ? tx->outbound.next_hop_id : 0u,
                             MESH_SIM_TRANSITION_TX_END,
                             tx->has_outbound ? tx->outbound.packet.msg_type :
                                                tx->protocol_msg_type,
                             tx->frame_len);
    if (ret != MESH_SIM_OK || tx->connection_event_index == UINT16_MAX) {
        return ret;
    }
    if (tx->connection_event_index >= world->connection_event_count) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    world->connection_events[tx->connection_event_index].sender_worker_completed = true;
    return watchdog_worker_completed(world, tx->node_index, true);
}

static int process_rx_end(struct mesh_sim_world *world, size_t window_index)
{
    struct mesh_sim_rx_window *window = &world->rx_windows[window_index];
    struct mesh_sim_role_instance *node;
    struct dwm3000_runtime_interval status_interval;
    struct dwm3000_runtime_interval read_interval;
    struct dwm3000_runtime_interval sleep_interval;
    uint64_t runtime_cursor = world->now_us;
    int ret;

    if (!rx_window_work_current(world, window)) {
        return MESH_SIM_OK;
    }
    if (world->now_us < window->end_us) {
        return MESH_SIM_OK;
    }
    node = &world->roles[window->node_index];
    if (node->radio_state != MESH_SIM_RADIO_RX) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (window->dwm_runtime_owned) {
        ret = dwm3000_runtime_finish_rx(&node->dwm3000, world->now_us);
        if (ret != DWM3000_RUNTIME_OK) {
            return mesh_sim_fail(world, ret);
        }
        if (window->decoded_frame_len > 0u) {
            ret = dwm3000_runtime_status_poll(&node->dwm3000,
                                              runtime_cursor,
                                              &status_interval);
            if (ret != DWM3000_RUNTIME_OK) {
                return mesh_sim_fail(world, ret);
            }
            runtime_cursor = status_interval.end_us;
            ret = dwm3000_runtime_read_frame(&node->dwm3000,
                                             window->decoded_frame_len,
                                             runtime_cursor,
                                             &read_interval);
            if (ret != DWM3000_RUNTIME_OK) {
                return mesh_sim_fail(world, ret);
            }
            runtime_cursor = read_interval.end_us;
        }
    }
    node->radio_state = mesh_sim_radio_post_operation_state(node, world->now_us);
    if (node->anchor_initialized && window->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
        !window->wake_claim_handoff) {
        uint64_t duration = window->end_us - window->start_us;

        uwb_anchor_note_idle_scan(&node->anchor_session,
                                  0u,
                                  0u,
                                  duration > UINT32_MAX ? UINT32_MAX :
                                  (uint32_t)duration,
                                  window->preamble_detected);
    }
    if (window->periodic_low_duty) {
        if (window->wake_claim_handoff) {
            return mesh_sim_trace_add(world,
                                      world->now_us,
                                      node->id,
                                      0u,
                                      MESH_SIM_TRANSITION_RX_END,
                                      0u,
                                      (uint32_t)window_index);
        }
        ret = dwm3000_runtime_enter_retained_sleep(&node->dwm3000,
                                                   runtime_cursor,
                                                   &sleep_interval);
        if (ret != DWM3000_RUNTIME_OK) {
            return mesh_sim_fail(world, ret);
        }
    }
    ret = mesh_sim_trace_add(world,
                             world->now_us,
                             node->id,
                             0u,
                             MESH_SIM_TRANSITION_RX_END,
                             0u,
                             (uint32_t)window_index);
    if (ret != MESH_SIM_OK || !window->periodic_low_duty) {
        if (ret == MESH_SIM_OK &&
            window->connection_event_index != UINT16_MAX) {
            if (window->connection_event_index >= world->connection_event_count) {
                return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
            }
            world->connection_events[window->connection_event_index]
                .receiver_worker_completed = true;
            return watchdog_worker_completed(world, window->node_index, true);
        }
        return ret;
    }
    ret = mesh_sim_scheduler_schedule(
        world,
        SIM_EVENT_LOW_DUTY_START,
        sleep_interval.end_us +
            (uint64_t)MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS * 1000u,
        window->node_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_trace_add(
        world,
        sleep_interval.end_us,
        node->id,
        0u,
        MESH_SIM_TRANSITION_LOW_DUTY_RESCHEDULED,
        0u,
        MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS);
}

static int process_rx_start(struct mesh_sim_world *world, size_t window_index)
{
    struct mesh_sim_rx_window *window = &world->rx_windows[window_index];
    struct mesh_sim_role_instance *node;

    if (!rx_window_work_current(world, window)) {
        return MESH_SIM_OK;
    }
    node = &world->roles[window->node_index];
    if (node->radio_state != MESH_SIM_RADIO_SLEEP &&
        node->radio_state != MESH_SIM_RADIO_IDLE) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }
    node->radio_state = MESH_SIM_RADIO_RX;
    return mesh_sim_trace_add(world,
                              world->now_us,
                              node->id,
                              0u,
                              MESH_SIM_TRANSITION_RX_START,
                              0u,
                              (uint32_t)window_index);
}

static int process_tx_start(struct mesh_sim_world *world, size_t tx_index)
{
    struct mesh_sim_transmission *tx = &world->transmissions[tx_index];
    struct mesh_sim_role_instance *node;
    int ret;

    if (!transmission_work_current(world, tx)) {
        return MESH_SIM_OK;
    }
    node = &world->roles[tx->node_index];
    if (node->radio_state != MESH_SIM_RADIO_SLEEP &&
        node->radio_state != MESH_SIM_RADIO_IDLE) {
        return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
    }
    node->radio_state = MESH_SIM_RADIO_TX;
    ret = mesh_sim_radio_note_preamble_at_tx_start(world, tx);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_trace_add(world,
                              world->now_us,
                              node->id,
                              tx->has_outbound ? tx->outbound.next_hop_id : 0u,
                              MESH_SIM_TRANSITION_TX_START,
                              tx->has_outbound ? tx->outbound.packet.msg_type :
                                                 tx->protocol_msg_type,
                              tx->frame_len);
}

static int process_runtime_radio_release(struct mesh_sim_world *world,
                                         size_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    enum mesh_runtime_radio_owner owner;
    struct dwm3000_runtime_interval sleep_interval;
    int ret;

    owner = node->runtime.radio_owner;
    if (owner == MESH_RUNTIME_RADIO_NONE ||
        world->now_us < node->runtime.radio_busy_until_us) {
        return MESH_SIM_OK;
    }
    ret = mesh_runtime_release_radio(&node->runtime, owner, world->now_us);
    if (ret != MESH_RUNTIME_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = mesh_sim_trace_add(world,
                             world->now_us,
                             node->id,
                             0u,
                             MESH_SIM_TRANSITION_RUNTIME_RADIO_RELEASED,
                             0u,
                             (uint32_t)owner);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (owner == MESH_RUNTIME_RADIO_DS_TWR && node->resume_low_duty_after_ds_twr) {
        if (node->radio_state != MESH_SIM_RADIO_IDLE &&
            node->radio_state != MESH_SIM_RADIO_SLEEP) {
            return mesh_sim_fail(world, MESH_SIM_ERR_RADIO_CONFLICT);
        }
        node->radio_state = MESH_SIM_RADIO_SLEEP;
        ret = dwm3000_runtime_enter_retained_sleep(&node->dwm3000,
                                                   world->now_us,
                                                   &sleep_interval);
        if (ret != DWM3000_RUNTIME_OK) {
            return mesh_sim_fail(world, ret);
        }
        node->resume_low_duty_after_ds_twr = false;
        ret = mesh_sim_scheduler_schedule(
            world,
            SIM_EVENT_LOW_DUTY_START,
            sleep_interval.end_us +
                (uint64_t)MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS * 1000u,
            (uint16_t)node_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = mesh_sim_trace_add(world,
                                 world->now_us,
                                 node->id,
                                 0u,
                                 MESH_SIM_TRANSITION_DS_TWR_RELEASED,
                                 0u,
                                 0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    ret = watchdog_worker_completed(world, (uint8_t)node_index, true);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_scheduler_schedule(world,
                                       SIM_EVENT_RUNTIME_BOUNDARY,
                                       world->now_us,
                                       (uint16_t)node_index);
}

int mesh_sim_process_event(struct mesh_sim_world *world,
                           const struct mesh_sim_event *event)
{
    switch ((enum mesh_sim_event_type)event->type) {
    case SIM_EVENT_TX_END:
        return process_tx_end(world, event->object_index);
    case SIM_EVENT_TX_EVALUATE:
        return mesh_sim_radio_evaluate_transmission(world,
                                                    event->object_index,
                                                    dispatch_received_packet,
                                                    claim_radio_from_event_dispatch,
                                                    world);
    case SIM_EVENT_RX_END:
        return process_rx_end(world, event->object_index);
    case SIM_EVENT_CONNECTION_END:
        return mesh_sim_connection_process_end(world, event->object_index);
    case SIM_EVENT_CONNECTION_REPAIR_END:
        return mesh_sim_connection_process_repair_end(world,
                                                       event->object_index,
                                                       &connection_callbacks);
    case SIM_EVENT_RELAY_TICK:
        return mesh_sim_relay_process_tick(world,
                                           (uint8_t)event->object_index,
                                           event->token);
    case SIM_EVENT_CONNECTION_START:
        return mesh_sim_connection_process_start(world,
                                                 event->object_index,
                                                 &connection_callbacks,
                                                 mesh_sim_relay_start_connection_tx);
    case SIM_EVENT_CONNECTION_REPAIR_START:
        return mesh_sim_connection_process_repair_start(world,
                                                         event->object_index,
                                                         &connection_callbacks);
    case SIM_EVENT_RX_START:
        return process_rx_start(world, event->object_index);
    case SIM_EVENT_TX_START:
        return process_tx_start(world, event->object_index);
    case SIM_EVENT_LOW_DUTY_START:
        return mesh_sim_radio_process_anchor_low_duty_start(
            world,
            (uint8_t)event->object_index);
    case SIM_EVENT_RUNTIME_BOUNDARY:
        return mesh_sim_radio_process_runtime_boundary(world,
                                        (uint8_t)event->object_index);
    case SIM_EVENT_RUNTIME_RADIO_RELEASE:
        return process_runtime_radio_release(world, event->object_index);
    case SIM_EVENT_ROUTE_DISCOVERY_RETRY:
        return mesh_sim_relay_process_route_discovery_retry(
            world,
            (uint8_t)event->object_index);
    case SIM_EVENT_WATCHDOG_EXPIRE:
        return process_watchdog_expiry(world,
                                       (uint8_t)event->object_index,
                                       event->token);
    case SIM_EVENT_TRACE_MARKER:
        return mesh_sim_trace_add(world,
                              world->now_us,
                              0u,
                              0u,
                              MESH_SIM_TRANSITION_SCHEDULER_MARKER,
                              0u,
                              event->object_index);
    default:
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
}
