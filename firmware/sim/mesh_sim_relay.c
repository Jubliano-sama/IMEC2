#include "mesh_sim_internal.h"
#include "mesh_sim_interval.h"
#include "report.h"

#include <limits.h>
#include <string.h>

static int schedule_relay_action_outbound(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound);
static int schedule_contact_response(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound);
static int start_route_discovery_for_waiting(struct mesh_sim_world *world,
                                             uint8_t node_index);
static int schedule_pending_runtime_tick(struct mesh_sim_world *world,
                                         uint8_t node_index);
int mesh_sim_relay_queue_index_for_peer(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id);
static int queue_index_for_peer_during_pending(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id);
void mesh_sim_relay_remove_queue_entry(struct mesh_sim_role_instance *node,
                                       size_t index);

static bool packet_identity_matches(const struct proto_packet *left,
                                    const struct proto_packet *right)
{
    return left->msg_type == right->msg_type &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq;
}

static bool route_reply_ack_custody_matches(
    const struct mesh_outbound *left,
    const struct mesh_outbound *right)
{
    return left->packet.msg_type == MSG_ROUTE_REPLY_ACK &&
           right->packet.msg_type == MSG_ROUTE_REPLY_ACK &&
           left->packet.flags == right->packet.flags &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.ttl == right->packet.ttl &&
           left->next_hop_id == right->next_hop_id &&
           left->payload_len == right->payload_len &&
           memcmp(left->payload, right->payload, right->payload_len) == 0;
}

static bool packet_identity_matches_pending(
    const struct proto_packet *packet,
    const struct mesh_pending_tx *pending)
{
    return packet_identity_matches(packet, &pending->packet);
}

static bool queued_can_run_during_pending(
    const struct mesh_sim_queued_tx *queued,
    const struct mesh_pending_tx *pending)
{
    if (queued->needs_relay_start) {
        return false;
    }
    switch (queued->outbound.packet.msg_type) {
    case MSG_MESH_HOP_ACK:
    case MSG_GATEWAY_ACK:
    case MSG_ROUTE_REPLY_ACK:
    case MSG_GATEWAY_COLLECTION_EACK:
        return true;
    default:
        return packet_identity_matches_pending(&queued->outbound.packet,
                                               pending);
    }
}

static void cancel_completed_pending_tick(struct mesh_sim_world *world,
                                          uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];

    if (node->relay.pending.state == MESH_RELAY_TX_IDLE &&
        node->relay_timer_guard.valid &&
        node->relay_timer_guard.pending_owned) {
        mesh_sim_scheduler_cancel_relay_tick(world, node_index);
        node->relay_timer_guard.valid = false;
    }
    if (!node->route_waiting_valid &&
        !node->relay.route_discovery.active) {
        mesh_sim_scheduler_cancel_route_discovery(world, node_index);
    }
}

static int schedule_pending_runtime_tick(struct mesh_sim_world *world,
                                         uint8_t node_index)
{
    const struct mesh_pending_tx *pending;
    uint32_t due_ms;
    uint64_t due_us;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    pending = &world->roles[node_index].relay.pending;
    switch (pending->state) {
    case MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD:
        due_ms = pending->gateway_ack_deadline_ms;
        break;
    case MESH_RELAY_TX_WAIT_RETRY_BACKOFF:
        due_ms = pending->retry_after_ms;
        break;
    default:
        return MESH_SIM_OK;
    }
    if (due_ms == 0u) {
        return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
    }
    due_us = (uint64_t)due_ms * 1000u;
    if (due_us < world->now_us) {
        due_us = world->now_us;
    }
    return mesh_sim_schedule_relay_tick(world, node_index, due_us);
}

static uint8_t outbound_priority(const struct mesh_sim_role_instance *node,
                                 const struct mesh_outbound *outbound)
{
    if (outbound->packet.src_id == node->gateway_id &&
        outbound->packet.msg_type == MSG_COMMAND) {
        return 250u;
    }
    if (outbound->packet.src_id == node->id &&
        outbound->packet.msg_type == MSG_CLICK_REPORT) {
        return 240u;
    }
    if (outbound->packet.msg_type == MSG_GATEWAY_ACK) {
        return 225u;
    }
    if (outbound->packet.msg_type == MSG_MESH_HOP_ACK) {
        return 210u;
    }
    if (outbound->packet.src_id == node->id) {
        return 180u;
    }
    return 100u;
}

static bool generated_ack_requested_seq(const struct mesh_outbound *outbound,
                                        uint16_t *requested_seq)
{
    const uint8_t *value;
    uint8_t value_len;

    if (outbound == NULL || requested_seq == NULL ||
        (outbound->packet.msg_type != MSG_MESH_HOP_ACK &&
         outbound->packet.msg_type != MSG_GATEWAY_ACK) ||
        tlv_find(outbound->payload,
                 outbound->payload_len,
                 TLV_REQUESTED_MSG_SEQ,
                 &value,
                 &value_len) != PROTO_OK ||
        value_len != sizeof(uint16_t)) {
        return false;
    }
    *requested_seq = proto_get_u16_le(value);
    return true;
}

static bool queued_outbound_matches(const struct mesh_sim_queued_tx *queued,
                                    const struct mesh_outbound *candidate,
                                    bool needs_relay_start)
{
    const struct proto_packet *left;
    const struct proto_packet *right;
    uint16_t left_requested_seq;
    uint16_t right_requested_seq;

    if (queued == NULL || candidate == NULL || !queued->valid ||
        queued->needs_relay_start != needs_relay_start) {
        return false;
    }
    left = &queued->outbound.packet;
    right = &candidate->packet;

    /*
     * The production Channel-9 ACK table coalesces generated ACKs by the
     * operation they acknowledge, not by the ACK packet's freshly allocated
     * sequence.  Mirror that admission rule so two copies decoded before the
     * next peer turn cannot manufacture an ACK train in the simulator queue.
     */
    if (generated_ack_requested_seq(&queued->outbound, &left_requested_seq) &&
        generated_ack_requested_seq(candidate, &right_requested_seq) &&
        left->msg_type == right->msg_type &&
        left->dst_id == right->dst_id &&
        left->session_id == right->session_id &&
        queued->outbound.next_hop_id == candidate->next_hop_id &&
        left_requested_seq == right_requested_seq) {
        return true;
    }

    return left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len &&
           queued->outbound.payload_len == candidate->payload_len &&
           queued->outbound.radio_channel == candidate->radio_channel &&
           queued->outbound.next_hop_id == candidate->next_hop_id &&
           memcmp(queued->outbound.payload,
                  candidate->payload,
                  candidate->payload_len) == 0;
}

static int queue_outbound(struct mesh_sim_world *world,
                          uint8_t node_index,
                          const struct mesh_outbound *outbound,
                          bool needs_relay_start)
{
    struct mesh_sim_role_instance *node;
    struct mesh_sim_queued_tx *slot = NULL;

    if (!mesh_sim_node_index_valid(world, node_index) || outbound == NULL ||
        outbound->payload_len != outbound->packet.payload_len ||
        outbound->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        (outbound->next_hop_id == 0u &&
         outbound->packet.dst_id != MESH_BROADCAST_ID)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (queued_outbound_matches(&node->tx_queue[i],
                                    outbound,
                                    needs_relay_start)) {
            return mesh_sim_trace_add_packet(
                world,
                world->now_us,
                node->id,
                outbound->next_hop_id,
                MESH_SIM_TRANSITION_PACKET_COALESCED,
                &outbound->packet,
                node->tx_queue[i].priority);
        }
    }
    if (node->tx_queue_capacity == 0u ||
        node->tx_queue_count >= node->tx_queue_capacity) {
        /* Queue pressure is an explicit admission result, not world failure. */
        return MESH_SIM_ERR_CAPACITY;
    }
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (!node->tx_queue[i].valid) {
            slot = &node->tx_queue[i];
            break;
        }
    }
    if (slot == NULL) {
        return MESH_SIM_ERR_CAPACITY;
    }
    *slot = (struct mesh_sim_queued_tx) {
        .outbound = *outbound,
        .enqueue_order = world->next_enqueue_order++,
        .priority = outbound_priority(node, outbound),
        .needs_relay_start = needs_relay_start,
        .valid = true,
    };
    node->tx_queue_count++;
    return mesh_sim_trace_add_packet(world,
                                     world->now_us,
                                     node->id,
                                     outbound->next_hop_id,
                                     MESH_SIM_TRANSITION_PACKET_QUEUED,
                                     &outbound->packet,
                                     slot->priority);
}

int mesh_sim_queue_originated(struct mesh_sim_world *world,
                              uint8_t node_index,
                              const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len)
{
    struct mesh_sim_role_instance *node;
    struct mesh_outbound outbound = {0};
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index) || packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized || packet->src_id != node->id) {
        return MESH_SIM_ERR_ARG;
    }
    outbound.packet = *packet;
    if (payload_len > 0u) {
        memcpy(outbound.payload, payload, payload_len);
    }
    outbound.payload_len = (uint16_t)payload_len;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.queued_at_ms = mesh_sim_time_ms(world->now_us);
    outbound.queued_at_valid = true;
    ret = mesh_relay_select_next_hop(&node->relay,
                                     packet->dst_id,
                                     &outbound.next_hop_id);
    if (ret != PROTO_OK) {
        if (node->route_waiting_valid) {
            return MESH_SIM_ERR_CAPACITY;
        }
        node->route_waiting_outbound = outbound;
        node->route_waiting_valid = true;
        ret = start_route_discovery_for_waiting(world, node_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        return MESH_SIM_OK;
    }
    return queue_outbound(world, node_index, &outbound, true);
}

int mesh_sim_override_next_relay_random(struct mesh_sim_world *world,
                                        uint8_t node_index,
                                        uint32_t random_value)
{
    struct mesh_sim_role_instance *node;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    node->next_relay_random = random_value;
    node->next_relay_random_valid = true;
    return MESH_SIM_OK;
}

int mesh_sim_direct_gateway_start_queued_tx(struct mesh_sim_world *world,
                                            uint8_t sender_index,
                                            uint64_t air_start_us,
                                            uint64_t tx_deadline_us,
                                            uint16_t *transmission_index)
{
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_queued_tx queued;
    struct mesh_outbound outbound;
    int queue_index;
    int ret;

    if (world == NULL || !mesh_sim_node_index_valid(world, sender_index) ||
        air_start_us < world->now_us || tx_deadline_us <= air_start_us) {
        return MESH_SIM_ERR_ARG;
    }
    sender = &world->roles[sender_index];
    queue_index = mesh_relay_tx_active(&sender->relay) ?
        queue_index_for_peer_during_pending(sender, sender->gateway_id) :
        mesh_sim_relay_queue_index_for_peer(sender, sender->gateway_id);
    if (queue_index < 0) {
        return MESH_SIM_ERR_ROUTE_REQUIRED;
    }
    queued = sender->tx_queue[queue_index];
    if (queued.needs_relay_start) {
        ret = mesh_relay_start_tx(&sender->relay,
                                  &queued.outbound.packet,
                                  queued.outbound.payload,
                                  queued.outbound.payload_len,
                                  mesh_sim_time_ms(air_start_us),
                                  &outbound);
        if (ret != PROTO_OK) {
            return mesh_sim_fail(world, ret);
        }
    } else {
        if (!mesh_relay_tx_active(&sender->relay) ||
            queued.outbound.packet.msg_type == MSG_GATEWAY_ACK ||
            queued.outbound.next_hop_id != sender->gateway_id ||
            sender->relay.pending.packet.src_id !=
                queued.outbound.packet.src_id ||
            sender->relay.pending.packet.dst_id !=
                queued.outbound.packet.dst_id ||
            sender->relay.pending.packet.session_id !=
                queued.outbound.packet.session_id ||
            sender->relay.pending.packet.seq != queued.outbound.packet.seq) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        outbound = queued.outbound;
    }
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    sender->relay.pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       sender_index,
                                                       air_start_us,
                                                       tx_deadline_us,
                                                       &outbound,
                                                       UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        if (queued.needs_relay_start) {
            mesh_relay_cancel_tx(&sender->relay);
        }
        return ret;
    }
    if (transmission_index != NULL) {
        *transmission_index = (uint16_t)(world->transmission_count - 1u);
    }
    mesh_sim_relay_remove_queue_entry(sender, (size_t)queue_index);
    /* An unscheduled direct turn has no connection-event owner to complete
     * the worker lease in mesh_sim_events.c.  Feed the sender at the actual
     * RF admission boundary so a live direct turn cannot let its watchdog
     * expire while the gateway ACK is being collected. */
    return mesh_sim_watchdog_feed(world, sender_index);
}

int mesh_sim_direct_gateway_schedule_ack(struct mesh_sim_world *world,
                                         uint8_t gateway_index,
                                         uint8_t sender_index,
                                         uint64_t air_start_us,
                                         uint64_t ack_window_end_us,
                                         uint16_t *transmission_index)
{
    struct mesh_sim_role_instance *gateway;
    struct mesh_sim_role_instance *sender;
    struct mesh_sim_queued_tx queued;
    struct dwm3000_runtime sender_runtime_before;
    struct dwm3000_runtime gateway_runtime_before;
    size_t event_count_before;
    size_t rx_window_count_before;
    size_t transmission_count_before;
    int queue_index;
    int ret;

    if (world == NULL || !mesh_sim_node_index_valid(world, gateway_index) ||
        !mesh_sim_node_index_valid(world, sender_index) ||
        world->roles[gateway_index].role != MESH_SIM_ROLE_GATEWAY ||
        air_start_us < world->now_us || ack_window_end_us <= air_start_us) {
        return MESH_SIM_ERR_ARG;
    }
    gateway = &world->roles[gateway_index];
    sender = &world->roles[sender_index];
    queue_index = -1;
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *candidate = &gateway->tx_queue[i];

        if (!candidate->valid || candidate->needs_relay_start ||
            candidate->outbound.next_hop_id != sender->id ||
            candidate->outbound.packet.msg_type != MSG_GATEWAY_ACK) {
            continue;
        }
        if (queue_index < 0 || candidate->enqueue_order <
                gateway->tx_queue[queue_index].enqueue_order) {
            queue_index = (int)i;
        }
    }
    if (queue_index < 0) {
        return MESH_SIM_ERR_ROUTE_REQUIRED;
    }
    queued = gateway->tx_queue[queue_index];
    sender_runtime_before = sender->dwm3000;
    gateway_runtime_before = gateway->dwm3000;
    event_count_before = world->event_count;
    rx_window_count_before = world->rx_window_count;
    transmission_count_before = world->transmission_count;
    ret = mesh_sim_radio_schedule_channel9_runtime_rx(
        world, sender_index, ack_window_end_us, air_start_us, UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        sender->dwm3000 = sender_runtime_before;
        gateway->dwm3000 = gateway_runtime_before;
        while (world->event_count > event_count_before) {
            world->event_count--;
            memset(&world->events[world->event_count], 0,
                   sizeof(world->events[world->event_count]));
        }
        while (world->rx_window_count > rx_window_count_before) {
            world->rx_window_count--;
            memset(&world->rx_windows[world->rx_window_count], 0,
                   sizeof(world->rx_windows[world->rx_window_count]));
        }
        while (world->transmission_count > transmission_count_before) {
            world->transmission_count--;
            memset(&world->transmissions[world->transmission_count], 0,
                   sizeof(world->transmissions[world->transmission_count]));
        }
        return ret;
    }
    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       gateway_index,
                                                       air_start_us,
                                                       ack_window_end_us,
                                                       &queued.outbound,
                                                       UINT16_MAX);
    if (ret != MESH_SIM_OK) {
        sender->dwm3000 = sender_runtime_before;
        gateway->dwm3000 = gateway_runtime_before;
        while (world->event_count > event_count_before) {
            world->event_count--;
            memset(&world->events[world->event_count], 0,
                   sizeof(world->events[world->event_count]));
        }
        while (world->rx_window_count > rx_window_count_before) {
            world->rx_window_count--;
            memset(&world->rx_windows[world->rx_window_count], 0,
                   sizeof(world->rx_windows[world->rx_window_count]));
        }
        while (world->transmission_count > transmission_count_before) {
            world->transmission_count--;
            memset(&world->transmissions[world->transmission_count], 0,
                   sizeof(world->transmissions[world->transmission_count]));
        }
        return ret;
    }
    if (transmission_index != NULL) {
        *transmission_index = (uint16_t)(world->transmission_count - 1u);
    }
    mesh_sim_relay_remove_queue_entry(gateway, (size_t)queue_index);
    /* The direct gateway RX/TX pair uses the unscheduled runtime seam, so it
     * has no connection event to feed either endpoint's worker lease. */
    ret = mesh_sim_watchdog_feed(world, gateway_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_watchdog_feed(world, sender_index);
}

int mesh_sim_relay_queue_index_for_peer(const struct mesh_sim_role_instance *node,
                                        uint64_t peer_id)
{
    size_t best = SIZE_MAX;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *entry = &node->tx_queue[i];

        if (!entry->valid || entry->outbound.next_hop_id != peer_id) {
            continue;
        }
        if (best == SIZE_MAX || entry->priority > node->tx_queue[best].priority ||
            (entry->priority == node->tx_queue[best].priority &&
             entry->enqueue_order < node->tx_queue[best].enqueue_order)) {
            best = i;
        }
    }
    return best == SIZE_MAX ? -1 : (int)best;
}

static int queue_index_for_peer_during_pending(
    const struct mesh_sim_role_instance *node,
    uint64_t peer_id)
{
    size_t best = SIZE_MAX;

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        const struct mesh_sim_queued_tx *entry = &node->tx_queue[i];

        if (!entry->valid || entry->outbound.next_hop_id != peer_id ||
            !queued_can_run_during_pending(entry, &node->relay.pending)) {
            continue;
        }
        if (best == SIZE_MAX || entry->priority > node->tx_queue[best].priority ||
            (entry->priority == node->tx_queue[best].priority &&
             entry->enqueue_order < node->tx_queue[best].enqueue_order)) {
            best = i;
        }
    }
    return best == SIZE_MAX ? -1 : (int)best;
}

void mesh_sim_relay_remove_queue_entry(struct mesh_sim_role_instance *node,
                                       size_t index)
{
    memset(&node->tx_queue[index], 0, sizeof(node->tx_queue[index]));
    if (node->tx_queue_count > 0u) {
        node->tx_queue_count--;
    }
}

static void remove_queued_packet_identity(
    struct mesh_sim_role_instance *node,
    const struct proto_packet *packet)
{
    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        if (node->tx_queue[i].valid &&
            packet_identity_matches(&node->tx_queue[i].outbound.packet,
                                    packet)) {
            mesh_sim_relay_remove_queue_entry(node, i);
        }
    }
}

static int append_delivery(struct mesh_sim_world *world,
                           uint8_t node_index,
                           uint64_t previous_hop_id,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_sim_delivery *delivery;

    if (node->delivery_count >= MESH_SIM_DELIVERY_CAPACITY) {
        return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
    }
    delivery = &node->deliveries[node->delivery_count++];
    memset(delivery, 0, sizeof(*delivery));
    delivery->delivered_at_us = world->now_us;
    delivery->previous_hop_id = previous_hop_id;
    delivery->packet = *packet;
    delivery->payload_len = (uint16_t)payload_len;
    if (payload_len > 0u) {
        memcpy(delivery->payload, payload, payload_len);
    }
    return mesh_sim_trace_add_packet(world,
                                     world->now_us,
                                     node->id,
                                     previous_hop_id,
                                     MESH_SIM_TRANSITION_PACKET_DELIVERED,
                                     packet,
                                     packet->ttl);
}

enum semantic_delivery_match {
    SEMANTIC_DELIVERY_NEW = 0,
    SEMANTIC_DELIVERY_EXACT_DUPLICATE,
    SEMANTIC_DELIVERY_CONFLICT,
};

static bool semantic_delivery_identity_matches(
    const struct mesh_sim_delivery *delivery,
    const struct proto_packet *packet)
{
    return delivery->packet.msg_type == packet->msg_type &&
           delivery->packet.src_id == packet->src_id &&
           delivery->packet.dst_id == packet->dst_id &&
           delivery->packet.session_id == packet->session_id &&
           delivery->packet.seq == packet->seq;
}

static enum semantic_delivery_match semantic_delivery_classify(
    const struct mesh_sim_role_instance *node,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    for (size_t i = 0u; i < node->delivery_count; i++) {
        const struct mesh_sim_delivery *delivery = &node->deliveries[i];

        if (!semantic_delivery_identity_matches(delivery, packet)) {
            continue;
        }
        if (delivery->payload_len == payload_len &&
            (payload_len == 0u ||
             memcmp(delivery->payload, payload, payload_len) == 0)) {
            return SEMANTIC_DELIVERY_EXACT_DUPLICATE;
        }
        return SEMANTIC_DELIVERY_CONFLICT;
    }
    return SEMANTIC_DELIVERY_NEW;
}

static bool gateway_ack_confirm_matches_delivery(
    const struct mesh_sim_role_instance *gateway,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len)
{
    bool found = false;

    for (size_t i = 0u; i < gateway->delivery_count; i++) {
        bool matches = false;

        if (mesh_gateway_ack_confirm_matches_packet(
                confirm_packet,
                confirm_payload,
                confirm_payload_len,
                &gateway->deliveries[i].packet,
                gateway->deliveries[i].payload,
                gateway->deliveries[i].payload_len,
                &matches) != PROTO_OK) {
            return false;
        }
        if (matches) {
            /*
             * The simulator's durable gateway abstraction is the unique
             * committed semantic-delivery record. A confirm may retire that
             * record's transport journal, but it never creates another host
             * delivery and must not ambiguously name two records.
             */
            if (found) {
                return false;
            }
            found = true;
        }
    }
    return found;
}

static int remove_queued_gateway_ack_confirm_predecessor(
    struct mesh_sim_role_instance *node)
{
    const struct mesh_pending_tx *confirm = &node->relay.pending;
    struct proto_packet acknowledged_packet;
    uint8_t acknowledged_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (confirm->state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
        confirm->packet.msg_type != MSG_GATEWAY_ACK_CONFIRM ||
        !node->relay.outbox_record.valid ||
        node->relay.outbox_record.packet_class != MSG_GATEWAY_ACK_CONFIRM ||
        node->relay.outbox_record.delivery_state !=
            MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    ret = mesh_gateway_ack_confirm_identity_packet(
        &confirm->packet,
        confirm->payload,
        confirm->payload_len,
        &acknowledged_packet,
        acknowledged_digest);
    if (ret != PROTO_OK) {
        return MESH_SIM_ERR_PROTOCOL;
    }

    for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
        struct mesh_sim_queued_tx *queued = &node->tx_queue[i];
        bool matches = false;

        if (!queued->valid ||
            !packet_identity_matches(&queued->outbound.packet,
                                     &acknowledged_packet)) {
            continue;
        }
        ret = mesh_gateway_ack_confirm_matches_packet(
            &confirm->packet,
            confirm->payload,
            confirm->payload_len,
            &queued->outbound.packet,
            queued->outbound.payload,
            queued->outbound.payload_len,
            &matches);
        if (ret != PROTO_OK || !matches) {
            /*
             * A same-identity payload conflict cannot remain runnable after
             * the durable successor took ownership.
             */
            return MESH_SIM_ERR_PROTOCOL;
        }
        mesh_sim_relay_remove_queue_entry(node, i);
    }
    return MESH_SIM_OK;
}

static uint64_t scheduled_connection_conflict_end_us(
    const struct mesh_sim_world *world,
    uint8_t node_index,
    uint64_t start_us,
    uint64_t end_us)
{
    uint64_t conflict_end_us = start_us;

    for (size_t i = 0u; i < world->connection_event_count; i++) {
        const struct mesh_sim_connection_event *event =
            &world->connection_events[i];
        const struct mesh_sim_connection *connection;

        if (!event->valid || event->connection_index >= world->connection_count) {
            continue;
        }
        connection = &world->connections[event->connection_index];
        if ((connection->node_a != node_index &&
             connection->node_b != node_index) ||
            event->node_a_work_epoch !=
                world->roles[connection->node_a].work_epoch ||
            event->node_b_work_epoch !=
                world->roles[connection->node_b].work_epoch ||
            !mesh_sim_interval_overlaps(start_us, end_us,
                                        event->start_us, event->end_us)) {
            continue;
        }
        if (event->end_us > conflict_end_us) {
            conflict_end_us = event->end_us;
        }
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        const struct mesh_sim_connection *connection = &world->connections[i];

        if (!connection->repair_pending ||
            (connection->node_a != node_index &&
             connection->node_b != node_index) ||
            !mesh_sim_interval_overlaps(start_us, end_us,
                                        connection->repair_start_us,
                                        connection->repair_end_us)) {
            continue;
        }
        if (connection->repair_end_us > conflict_end_us) {
            conflict_end_us = connection->repair_end_us;
        }
    }
    for (size_t i = 0u; i < world->connection_count; i++) {
        const struct mesh_sim_connection *connection = &world->connections[i];
        struct mesh_sim_connection_action action;

        if (!connection->valid ||
            (connection->node_a != node_index &&
             connection->node_b != node_index) ||
            mesh_sim_connection_next_action(world, (uint16_t)i, &action) !=
                MESH_SIM_OK ||
            action.kind == MESH_SIM_CONNECTION_ACTION_NONE ||
            !mesh_sim_interval_overlaps(start_us, end_us,
                                        action.start_us, action.end_us)) {
            continue;
        }
        if (action.end_us > conflict_end_us) {
            conflict_end_us = action.end_us;
        }
    }
    return conflict_end_us;
}

static uint64_t relay_action_safe_start_us(
    const struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    const struct mesh_sim_role_instance *node = &world->roles[node_index];
    enum mesh_sim_phy phy;
    uint64_t start_us;
    uint32_t duration_us;
    uint8_t channel;
    bool moved;

    if (mesh_sim_outbound_radio(outbound, &channel, &phy) != MESH_SIM_OK) {
        return UINT64_MAX;
    }
    (void)channel;
    duration_us = mesh_sim_frame_duration_us(
        phy, proto_packet_encoded_len(outbound->payload_len));
    if (duration_us == 0u) {
        return UINT64_MAX;
    }
    start_us = outbound->earliest_tx_valid ?
               (uint64_t)outbound->earliest_tx_ms * 1000u :
               world->now_us;
    if (start_us < world->now_us) {
        start_us = world->now_us;
    }

    do {
        uint64_t end_us;
        uint64_t connection_end_us;

        moved = false;
        if (node->runtime.radio_busy_until_us > start_us) {
            start_us = node->runtime.radio_busy_until_us;
            moved = true;
        }
        if (start_us > UINT64_MAX - duration_us) {
            return UINT64_MAX;
        }
        end_us = start_us + duration_us;
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            const struct mesh_sim_rx_window *window = &world->rx_windows[i];

            if (window->valid && window->node_index == node_index &&
                mesh_sim_interval_overlaps(start_us, end_us,
                                           window->start_us, window->end_us) &&
                window->end_us > start_us) {
                start_us = window->end_us;
                moved = true;
            }
        }
        for (size_t i = 0u; i < world->transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world->transmissions[i];

            if (tx->valid && tx->node_index == node_index &&
                mesh_sim_interval_overlaps(start_us, end_us,
                                           tx->start_us, tx->end_us) &&
                tx->end_us > start_us) {
                start_us = tx->end_us;
                moved = true;
            }
        }
        connection_end_us = scheduled_connection_conflict_end_us(
            world, node_index, start_us, end_us);
        if (connection_end_us > start_us) {
            start_us = connection_end_us;
            moved = true;
        }
    } while (moved);

    return start_us;
}

static int schedule_relay_action_outbound(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    uint64_t start_us;
    int ret;

    start_us = relay_action_safe_start_us(world, node_index, outbound);
    if (start_us == UINT64_MAX) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    ret = mesh_sim_schedule_outbound_tx(world,
                                        node_index,
                                        start_us,
                                        outbound,
                                        NULL);
    if (ret != MESH_SIM_OK) {
        return mesh_sim_fail(world, ret);
    }
    return mesh_sim_trace_add_packet(
        world,
        world->now_us,
        world->roles[node_index].id,
        outbound->next_hop_id,
        MESH_SIM_TRANSITION_PACKET_QUEUED,
        &outbound->packet,
        outbound_priority(&world->roles[node_index], outbound));
}

static bool contact_response_type(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REPLY_ACK || msg_type == MSG_RELAY_BUSY ||
           msg_type == MSG_RESULT_BUSY || msg_type == MSG_RESULT_GRANT;
}

static bool contact_response_destination_valid(
    const struct mesh_outbound *outbound)
{
    if (outbound->next_hop_id == MESH_BROADCAST_ID ||
        outbound->packet.dst_id == MESH_BROADCAST_ID) {
        return false;
    }

    /*
     * BUSY is an end-to-end deferral addressed to the original source, but
     * the immediate Channel-5 response is physically returned to the previous
     * hop.  The two IDs intentionally differ for transit traffic.
     */
    if (outbound->packet.msg_type == MSG_RELAY_BUSY ||
        outbound->packet.msg_type == MSG_RESULT_BUSY) {
        return true;
    }
    return outbound->packet.dst_id == outbound->next_hop_id;
}

static int role_index_for_id(const struct mesh_sim_world *world,
                             uint64_t node_id)
{
    for (size_t i = 0u; i < world->role_count; i++) {
        if (world->roles[i].id == node_id) {
            return (int)i;
        }
    }
    return -1;
}

static uint64_t current_radio_operation_end_us(
    const struct mesh_sim_world *world,
    uint8_t node_index)
{
    const struct mesh_sim_role_instance *node = &world->roles[node_index];
    uint64_t end_us = world->now_us;

    if (node->dwm3000.radio_busy_until_us > end_us) {
        end_us = node->dwm3000.radio_busy_until_us;
    }
    for (size_t i = 0u; i < world->rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world->rx_windows[i];

        if (window->valid && window->node_index == node_index &&
            window->start_us <= world->now_us && window->end_us > end_us) {
            end_us = window->end_us;
        }
    }
    for (size_t i = 0u; i < world->transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &world->transmissions[i];

        if (tx->valid && tx->node_index == node_index &&
            tx->start_us <= world->now_us && tx->end_us > end_us) {
            end_us = tx->end_us;
        }
    }
    return end_us;
}

static uint64_t contact_response_safe_base_us(
    const struct mesh_sim_world *world,
    uint8_t sender_index,
    uint8_t receiver_index,
    const struct mesh_outbound *outbound,
    const struct mesh_sim_contact_response_timing *timing)
{
    uint64_t base_us = current_radio_operation_end_us(world, sender_index);
    uint64_t receiver_ready_us = current_radio_operation_end_us(
        world, receiver_index);
    uint32_t tx_duration_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proto_packet_encoded_len(outbound->payload_len));
    bool moved;

    if (receiver_ready_us > base_us) {
        base_us = receiver_ready_us;
    }
    if (tx_duration_us == 0u) {
        return UINT64_MAX;
    }
    do {
        uint64_t rx_start_us;
        uint64_t rx_end_us;
        uint64_t tx_start_us;
        uint64_t tx_end_us;
        uint64_t sender_connection_end_us;
        uint64_t receiver_connection_end_us;

        moved = false;
        if (base_us > UINT64_MAX - timing->rx_delay_us ||
            base_us > UINT64_MAX - timing->tx_delay_us) {
            return UINT64_MAX;
        }
        rx_start_us = base_us + timing->rx_delay_us;
        tx_start_us = base_us + timing->tx_delay_us;
        if (rx_start_us > UINT64_MAX - timing->rx_window_us ||
            tx_start_us > UINT64_MAX - tx_duration_us) {
            return UINT64_MAX;
        }
        rx_end_us = rx_start_us + timing->rx_window_us;
        tx_end_us = tx_start_us + tx_duration_us;
        for (size_t i = 0u; i < world->rx_window_count; i++) {
            const struct mesh_sim_rx_window *window = &world->rx_windows[i];

            if (window->valid && window->node_index == sender_index &&
                mesh_sim_interval_overlaps(tx_start_us, tx_end_us,
                                           window->start_us, window->end_us) &&
                window->end_us > base_us) {
                base_us = window->end_us;
                moved = true;
            } else if (window->valid &&
                       window->node_index == receiver_index &&
                       mesh_sim_interval_overlaps(rx_start_us, rx_end_us,
                                                  window->start_us,
                                                  window->end_us) &&
                       window->end_us > base_us) {
                base_us = window->end_us;
                moved = true;
            }
        }
        for (size_t i = 0u; i < world->transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world->transmissions[i];

            if (tx->valid && tx->node_index == sender_index &&
                mesh_sim_interval_overlaps(tx_start_us, tx_end_us,
                                           tx->start_us, tx->end_us) &&
                tx->end_us > base_us) {
                base_us = tx->end_us;
                moved = true;
            } else if (tx->valid && tx->node_index == receiver_index &&
                       mesh_sim_interval_overlaps(rx_start_us, rx_end_us,
                                                  tx->start_us, tx->end_us) &&
                       tx->end_us > base_us) {
                base_us = tx->end_us;
                moved = true;
            }
        }
        sender_connection_end_us = scheduled_connection_conflict_end_us(
            world, sender_index, tx_start_us, tx_end_us);
        receiver_connection_end_us = scheduled_connection_conflict_end_us(
            world, receiver_index, rx_start_us, rx_end_us);
        if (sender_connection_end_us > tx_start_us) {
            base_us = sender_connection_end_us;
            moved = true;
        }
        if (receiver_connection_end_us > rx_start_us) {
            base_us = receiver_connection_end_us;
            moved = true;
        }
    } while (moved);
    return base_us;
}

static int schedule_contact_response(
    struct mesh_sim_world *world,
    uint8_t node_index,
    const struct mesh_outbound *outbound)
{
    const struct mesh_sim_contact_response_timing defaults = {
        .rx_delay_us = MESH_SIM_C5_RESPONSE_RX_DELAY_US,
        .rx_window_us = MESH_SIM_C5_RESPONSE_RX_WINDOW_US,
        .tx_delay_us = MESH_SIM_C5_RESPONSE_TX_DELAY_US,
    };
    struct mesh_sim_contact_response_timing timing;
    uint64_t rx_start_us;
    uint64_t rx_end_us;
    uint64_t tx_start_us;
    uint64_t response_base_us;
    size_t event_count_before;
    size_t rx_window_count_before;
    size_t transmission_count_before;
    int peer_index;
    int ret;

    if (world == NULL || outbound == NULL ||
        !mesh_sim_node_index_valid(world, node_index) ||
        !contact_response_type(outbound->packet.msg_type) ||
        outbound->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        !contact_response_destination_valid(outbound)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    peer_index = role_index_for_id(world, outbound->next_hop_id);
    if (peer_index < 0 ||
        !world->reachable[node_index][(uint8_t)peer_index]) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ROUTE_REQUIRED);
    }
    timing = world->next_contact_response_timing_valid ?
             world->next_contact_response_timing : defaults;
    world->next_contact_response_timing_valid = false;
    memset(&world->next_contact_response_timing, 0,
           sizeof(world->next_contact_response_timing));
    response_base_us = contact_response_safe_base_us(
        world, node_index, (uint8_t)peer_index, outbound, &timing);
    if (response_base_us == UINT64_MAX) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    if (response_base_us > UINT64_MAX - timing.rx_delay_us ||
        response_base_us > UINT64_MAX - timing.tx_delay_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    rx_start_us = response_base_us + timing.rx_delay_us;
    tx_start_us = response_base_us + timing.tx_delay_us;
    if (rx_start_us > UINT64_MAX - timing.rx_window_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    rx_end_us = rx_start_us + timing.rx_window_us;
    if (outbound->earliest_tx_valid &&
        tx_start_us < (uint64_t)outbound->earliest_tx_ms * 1000u) {
        tx_start_us = (uint64_t)outbound->earliest_tx_ms * 1000u;
    }

    event_count_before = world->event_count;
    rx_window_count_before = world->rx_window_count;
    transmission_count_before = world->transmission_count;
    ret = mesh_sim_schedule_rx(world,
                               (uint8_t)peer_index,
                               rx_start_us,
                               rx_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_schedule_outbound_tx(world,
                                            node_index,
                                            tx_start_us,
                                            outbound,
                                            NULL);
    }
    if (ret != MESH_SIM_OK) {
        while (world->event_count > event_count_before) {
            world->event_count--;
            memset(&world->events[world->event_count], 0,
                   sizeof(world->events[world->event_count]));
        }
        while (world->rx_window_count > rx_window_count_before) {
            world->rx_window_count--;
            memset(&world->rx_windows[world->rx_window_count], 0,
                   sizeof(world->rx_windows[world->rx_window_count]));
        }
        while (world->transmission_count > transmission_count_before) {
            world->transmission_count--;
            memset(&world->transmissions[world->transmission_count], 0,
                   sizeof(world->transmissions[world->transmission_count]));
        }
        return mesh_sim_fail(world, ret);
    }
    return mesh_sim_trace_add_packet(
        world,
        world->now_us,
        world->roles[node_index].id,
        outbound->next_hop_id,
        MESH_SIM_TRANSITION_PACKET_QUEUED,
        &outbound->packet,
        outbound_priority(&world->roles[node_index], outbound));
}

static int start_route_discovery(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t target_id,
                                 uint8_t triggering_msg_type)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_outbound route_request;
    uint32_t now_ms = mesh_sim_time_ms(world->now_us);
    uint64_t retry_at_us;
    int ret;

    if (target_id == 0u || target_id == node->id ||
        target_id == MESH_BROADCAST_ID) {
        return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
    }
    ret = mesh_relay_prepare_route_request_with_timing_flags(
        &node->relay,
        target_id,
        NULL,
        0u,
        node->route_request_flags,
        0u,
        now_ms,
        mesh_sim_random(world),
        &route_request);
    if (ret == PROTO_ERR_BUSY) {
        retry_at_us = (uint64_t)node->relay.route_discovery.next_request_ms *
                      1000u;
        if (retry_at_us < world->now_us) {
            retry_at_us = world->now_us;
        }
        return mesh_sim_scheduler_reschedule_route_discovery(world,
                                                             node_index,
                                                             retry_at_us);
    }
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    node->route_discovery_requests++;
    ret = mesh_sim_trace_add(world,
                         world->now_us,
                         node->id,
                         target_id,
                         MESH_SIM_TRANSITION_ROUTE_REQUIRED,
                         triggering_msg_type,
                         route_request.packet.ttl);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    ret = schedule_relay_action_outbound(world, node_index, &route_request);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    retry_at_us = (uint64_t)node->relay.route_discovery.next_request_ms * 1000u;
    if (retry_at_us <= world->now_us) {
        return mesh_sim_fail(world, MESH_SIM_ERR_EVENT_ORDER);
    }
    return mesh_sim_scheduler_reschedule_route_discovery(world,
                                                         node_index,
                                                         retry_at_us);
}

static int start_route_discovery_for_waiting(struct mesh_sim_world *world,
                                             uint8_t node_index)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    int ret;

    if (!node->route_waiting_valid) {
        return MESH_SIM_OK;
    }
    ret = mesh_relay_select_next_hop(
        &node->relay,
        node->route_waiting_outbound.packet.dst_id,
        &node->route_waiting_outbound.next_hop_id);
    if (ret == PROTO_OK) {
        mesh_relay_note_route_discovery_ready(
            &node->relay, node->route_waiting_outbound.packet.dst_id);
        mesh_sim_scheduler_cancel_route_discovery(world, node_index);
        ret = queue_outbound(world,
                             node_index,
                             &node->route_waiting_outbound,
                             true);
        if (ret == MESH_SIM_OK) {
            node->route_waiting_valid = false;
            memset(&node->route_waiting_outbound,
                   0,
                   sizeof(node->route_waiting_outbound));
        }
        return ret;
    }
    return start_route_discovery(
        world,
        node_index,
        node->route_waiting_outbound.packet.dst_id,
        node->route_waiting_outbound.packet.msg_type);
}

static bool gateway_semantic_delivery_requires_commit(
    const struct mesh_sim_role_instance *node,
    const struct proto_packet *packet)
{
    if (node == NULL || packet == NULL ||
        node->role != MESH_SIM_ROLE_GATEWAY ||
        packet->dst_id != node->id ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_GATEWAY_ACK_CONFIRM:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static int process_relay_actions(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t previous_hop_id,
                                 const struct proto_packet *received_packet,
                                 const uint8_t *received_payload,
                                 size_t received_payload_len,
                                 const struct mesh_relay_result *result)
{
    struct mesh_sim_role_instance *node = &world->roles[node_index];
    struct mesh_relay_result semantic_commit = {0};
    uint32_t unsupported;
    bool gateway_commit_required = gateway_semantic_delivery_requires_commit(
        node, received_packet);
    bool gateway_ack_confirm =
        gateway_commit_required && received_packet != NULL &&
        received_packet->msg_type == MSG_GATEWAY_ACK_CONFIRM;
    bool semantic_delivery = gateway_commit_required && !gateway_ack_confirm;
    bool semantic_committed = false;
    bool semantic_duplicate_redelivery = false;
    bool semantic_rejected = false;
    int ret;

    if ((result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        if (node->route_waiting_valid) {
            ret = start_route_discovery_for_waiting(world, node_index);
        } else {
            uint64_t target_id = result->route_discovery_target_id;
            uint8_t triggering_msg_type = received_packet == NULL ?
                node->relay.pending.packet.msg_type : received_packet->msg_type;

            if (target_id == 0u &&
                node->relay.pending.state != MESH_RELAY_TX_IDLE) {
                target_id = node->relay.pending.packet.dst_id;
            }
            ret = target_id == 0u ? MESH_SIM_OK :
                start_route_discovery(world,
                                      node_index,
                                      target_id,
                                      triggering_msg_type);
        }
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
        received_packet != NULL) {
        if (gateway_commit_required &&
            node->gateway_semantic_rejections_remaining != 0u) {
            node->gateway_semantic_rejections_remaining--;
            node->gateway_semantic_rejection_count++;
            semantic_rejected = true;
        } else if (gateway_commit_required) {
            enum semantic_delivery_match delivery_match =
                gateway_ack_confirm ? SEMANTIC_DELIVERY_NEW :
                semantic_delivery_classify(node,
                                           received_packet,
                                           received_payload,
                                           received_payload_len);

            if (gateway_ack_confirm &&
                !gateway_ack_confirm_matches_delivery(
                    node,
                    received_packet,
                    received_payload,
                    received_payload_len)) {
                node->gateway_semantic_rejection_count++;
                semantic_rejected = true;
            } else if (received_packet->msg_type == MSG_CLICK_REPORT &&
                report_validate_click_payload(received_packet,
                                              received_payload,
                                              received_payload_len) !=
                    PROTO_OK) {
                node->gateway_semantic_rejection_count++;
                semantic_rejected = true;
            } else if (delivery_match == SEMANTIC_DELIVERY_CONFLICT) {
                node->gateway_semantic_rejection_count++;
                semantic_rejected = true;
            } else if (delivery_match == SEMANTIC_DELIVERY_NEW &&
                       node->delivery_count >= MESH_SIM_DELIVERY_CAPACITY) {
                return mesh_sim_fail(world, MESH_SIM_ERR_CAPACITY);
            } else if (delivery_match == SEMANTIC_DELIVERY_NEW &&
                       node->gateway_admit != NULL &&
                       node->gateway_admit(
                           received_packet,
                           received_payload,
                           received_payload_len,
                           mesh_sim_time_ms(world->now_us),
                           node->gateway_admit_context) != PROTO_OK) {
                node->gateway_semantic_rejection_count++;
                semantic_rejected = true;
            } else {
                ret = mesh_relay_commit_gateway_delivery(
                    &node->relay,
                    received_packet,
                    received_payload,
                    received_payload_len,
                    previous_hop_id,
                    mesh_sim_time_ms(world->now_us),
                    &semantic_commit);
                if (ret != PROTO_OK ||
                    semantic_commit.status != PROTO_OK ||
                    semantic_commit.actions !=
                        MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
                    return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
                }
                semantic_committed = true;
                if (semantic_delivery &&
                    delivery_match == SEMANTIC_DELIVERY_EXACT_DUPLICATE) {
                    node->gateway_semantic_duplicate_redelivery_count++;
                    semantic_duplicate_redelivery = true;
                } else if (semantic_delivery) {
                    node->gateway_semantic_commit_count++;
                }
            }
        }

        if (!semantic_rejected && !semantic_duplicate_redelivery &&
            (!gateway_commit_required || semantic_delivery)) {
            ret = append_delivery(world,
                                  node_index,
                                  previous_hop_id,
                                  received_packet,
                                  received_payload,
                                  received_payload_len);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_FORWARD) != 0u) {
        struct mesh_outbound forward = result->forward;
        /* A gateway ACK produced for transit custody is already a complete
         * child-directed packet.  Keep it independently schedulable while
         * the original pending packet remains live; treating it as a fresh
         * relay-start payload would exclude it from the pending turn and let
         * a lower-priority hop ACK consume the opportunity first. */
        bool needs_relay_start =
            (result->actions &
             MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING) == 0u;
        if (needs_relay_start &&
            node->relay.pending.state != MESH_RELAY_TX_IDLE &&
            packet_identity_matches(&forward.packet,
                                    &node->relay.pending.packet)) {
            /* A duplicate retry of the packet already tracked by this relay
             * is an immutable custody retransmission.  It must be admitted
             * during the pending turn, just like the generated ACKs, while
             * still using the exact pending identity. */
            needs_relay_start = false;
        }
        if (!needs_relay_start) {
            forward.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        }
        ret = queue_outbound(world,
                             node_index,
                             &forward,
                             needs_relay_start);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        if ((result->actions &
             MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING) != 0u) {
            /*
             * Gateway acceptance makes any already-queued upstream retry of
             * the immutable transit packet obsolete.  Keep the core pending
             * packet/outbox until the child ACK handoff commits, but do not
             * let a stale upstream retry repeatedly win the direct-gateway
             * lane and starve that handoff.
             */
            remove_queued_packet_identity(node, &node->relay.pending.packet);
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) != 0u) {
        if (semantic_rejected || semantic_committed) {
            return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
        }
        if (semantic_delivery &&
            (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) == 0u) {
            node->gateway_semantic_duplicate_ack_count++;
        }
        ret = queue_outbound(world, node_index, &result->gateway_ack, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if (semantic_committed) {
        ret = queue_outbound(world,
                             node_index,
                             &semantic_commit.gateway_ack,
                             false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions &
         MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) != 0u) {
        /*
         * Core has atomically replaced the raw pending packet and outbox with
         * its compact successor. Treat the validated in-memory outbox as the
         * simulator's durable save/readback boundary, retire any runnable raw
         * retry, and let the pending retry timer emit the exact confirm.
         */
        ret = remove_queued_gateway_ack_confirm_predecessor(node);
        if (ret != MESH_SIM_OK) {
            return mesh_sim_fail(world, ret);
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        ret = queue_outbound(world, node_index, &result->hop_ack, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) != 0u) {
        if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
            /*
             * A transit reply remains in upstream custody until the next hop
             * has accepted the forwarded reply.  The Zephyr runtime performs
             * the same chained handoff synchronously; retain the child-facing
             * ACK here so the asynchronous simulator cannot release custody
             * before a downstream ROUTE_REPLY_ACK is decoded.
             */
            if (node->route_reply_upstream_ack_valid &&
                !route_reply_ack_custody_matches(
                    &node->route_reply_upstream_ack,
                    &result->route_reply_ack)) {
                return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
            }
            node->route_reply_upstream_ack = result->route_reply_ack;
            node->route_reply_upstream_ack_valid = true;
        } else {
            ret = schedule_contact_response(world,
                                            node_index,
                                            &result->route_reply_ack);
            if (ret != MESH_SIM_OK) {
                return ret;
            }
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) != 0u) {
        ret = schedule_relay_action_outbound(world,
                                             node_index,
                                             &result->route_request);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
        ret = schedule_relay_action_outbound(world,
                                             node_index,
                                             &result->route_reply);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_ROUTE_REPLY_ACKED) != 0u &&
        node->route_reply_upstream_ack_valid) {
        ret = schedule_contact_response(world,
                                        node_index,
                                        &node->route_reply_upstream_ack);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        memset(&node->route_reply_upstream_ack,
               0,
               sizeof(node->route_reply_upstream_ack));
        node->route_reply_upstream_ack_valid = false;
    }
    if ((result->actions & (MESH_RELAY_ACTION_SEND_RELAY_BUSY |
                            MESH_RELAY_ACTION_SEND_RESULT_BUSY)) != 0u) {
        ret = schedule_contact_response(world,
                                        node_index,
                                        &result->busy);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT) != 0u) {
        ret = schedule_contact_response(world,
                                        node_index,
                                        &result->result_grant);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_RETRANSMIT) != 0u) {
        ret = queue_outbound(world, node_index, &result->retransmit, false);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        ret = mesh_sim_trace_add_packet(world,
                                        world->now_us,
                                        node->id,
                                        result->retransmit.next_hop_id,
                                        MESH_SIM_TRANSITION_RETRY_READY,
                                        &result->retransmit.packet,
                                        result->retransmit.packet.message_age_ms);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) != 0u) {
        struct proto_packet confirmed_packet = node->relay.pending.packet;

        remove_queued_packet_identity(node, &confirmed_packet);
        if (confirmed_packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
            ret = mesh_relay_commit_gateway_ack_confirm_terminal(
                &node->relay,
                &confirmed_packet,
                node->relay.pending.payload,
                node->relay.pending.payload_len,
                mesh_sim_time_ms(world->now_us));
            if (ret != PROTO_OK) {
                return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
            }
        }
        ret = mesh_sim_trace_add_packet(world,
                                        world->now_us,
                                        node->id,
                                        node->gateway_id,
                                        MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                        &confirmed_packet,
                                        0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) != 0u) {
        ret = mesh_sim_trace_add_packet(world,
                                        world->now_us,
                                        node->id,
                                        previous_hop_id,
                                        MESH_SIM_TRANSITION_HOP_PROGRESS,
                                        &node->relay.pending.packet,
                                        0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
    }

    unsupported = result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV;
    if (unsupported != 0u) {
        return mesh_sim_fail(world, MESH_SIM_ERR_UNSUPPORTED_ACTION);
    }
    cancel_completed_pending_tick(world, node_index);
    return schedule_pending_runtime_tick(world, node_index);
}

int mesh_sim_relay_dispatch_packet(struct mesh_sim_world *world,
                           uint8_t receiver_index,
                           uint8_t sender_index,
                           const struct proto_packet *packet,
                           const uint8_t *payload,
                           size_t payload_len)
{
    struct mesh_sim_role_instance *receiver = &world->roles[receiver_index];
    struct mesh_relay_result result;
    uint32_t random_value;
    bool event_control_handled;
    int ret;

    ret = mesh_sim_connection_process_control_packet(world,
                                            receiver_index,
                                            sender_index,
                                            packet,
                                            payload,
                                            payload_len,
                                            &event_control_handled);
    if (ret != MESH_SIM_OK || event_control_handled) {
        return ret;
    }
    if (!receiver->relay_initialized) {
        return MESH_SIM_OK;
    }
    if (receiver->next_relay_random_valid) {
        random_value = receiver->next_relay_random;
        receiver->next_relay_random_valid = false;
    } else {
        random_value = mesh_sim_random(world);
    }
    ret = mesh_relay_handle_rx_with_random(
        &receiver->relay,
        packet,
        payload,
        payload_len,
        world->roles[sender_index].id,
        world->link_quality[sender_index][receiver_index],
        mesh_sim_time_ms(world->now_us),
        random_value,
        &result);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = process_relay_actions(world,
                                receiver_index,
                                world->roles[sender_index].id,
                                packet,
                                payload,
                                payload_len,
                                &result);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return start_route_discovery_for_waiting(world, receiver_index);
}

static bool relay_timer_guard_matches_state(
    const struct mesh_sim_relay_timer_guard *guard,
    const struct mesh_pending_tx *pending)
{
    if (!guard->valid) {
        return false;
    }
    if (pending->state == MESH_RELAY_TX_IDLE) {
        return !guard->pending_owned;
    }
    return guard->pending_owned &&
           guard->msg_type == pending->packet.msg_type &&
           guard->src_id == pending->packet.src_id &&
           guard->dst_id == pending->packet.dst_id &&
           guard->session_id == pending->packet.session_id &&
           guard->seq == pending->packet.seq;
}

int mesh_sim_relay_process_tick(struct mesh_sim_world *world,
                                uint8_t node_index,
                                uint32_t timer_generation)
{
    struct mesh_sim_role_instance *node;
    struct mesh_relay_result result;
    int ret;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    node = &world->roles[node_index];
    if (!node->relay_initialized) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    if (timer_generation == 0u ||
        timer_generation != node->relay_timer_guard.generation) {
        return MESH_SIM_OK;
    }
    if (!relay_timer_guard_matches_state(&node->relay_timer_guard,
                                         &node->relay.pending)) {
        node->relay_timer_guard.valid = false;
        return MESH_SIM_OK;
    }
    node->relay_timer_guard.valid = false;
    ret = mesh_relay_tick_with_random(&node->relay,
                                      mesh_sim_time_ms(world->now_us),
                                      mesh_sim_random(world),
                                      &result);
    if (ret != PROTO_OK) {
        return mesh_sim_fail(world, ret);
    }
    ret = process_relay_actions(world,
                                node_index,
                                0u,
                                NULL,
                                NULL,
                                0u,
                                &result);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return MESH_SIM_OK;
}

int mesh_sim_schedule_relay_tick(struct mesh_sim_world *world,
                                 uint8_t node_index,
                                 uint64_t at_us)
{
    struct mesh_sim_role_instance *node;
    struct mesh_sim_relay_timer_guard *guard;
    bool pending_owned;

    if (!mesh_sim_node_index_valid(world, node_index) || at_us < world->now_us) {
        return MESH_SIM_ERR_ARG;
    }
    node = &world->roles[node_index];
    guard = &node->relay_timer_guard;
    pending_owned = node->relay.pending.state != MESH_RELAY_TX_IDLE;
    if (!relay_timer_guard_matches_state(guard, &node->relay.pending)) {
        guard->generation++;
        if (guard->generation == 0u) {
            guard->generation = 1u;
        }
        guard->src_id = 0u;
        guard->dst_id = 0u;
        guard->session_id = 0u;
        guard->seq = 0u;
        guard->msg_type = 0u;
        if (pending_owned) {
            guard->src_id = node->relay.pending.packet.src_id;
            guard->dst_id = node->relay.pending.packet.dst_id;
            guard->session_id = node->relay.pending.packet.session_id;
            guard->seq = node->relay.pending.packet.seq;
            guard->msg_type = node->relay.pending.packet.msg_type;
        }
        guard->pending_owned = pending_owned;
        guard->valid = true;
    }
    for (size_t i = 0u; i < world->event_count; i++) {
        struct mesh_sim_event *event = &world->events[i];

        if (event->pending && event->type == SIM_EVENT_RELAY_TICK &&
            event->object_index == node_index) {
            event->time_us = at_us;
            event->token = guard->generation;
            return MESH_SIM_OK;
        }
    }
    return mesh_sim_scheduler_schedule_priority(world,
                                                SIM_EVENT_RELAY_TICK,
                                                at_us,
                                                node_index,
                                                SIM_EVENT_RELAY_TICK,
                                                guard->generation);
}

int mesh_sim_relay_process_route_discovery_retry(
    struct mesh_sim_world *world,
    uint8_t node_index)
{
    struct mesh_sim_role_instance *node;

    if (!mesh_sim_node_index_valid(world, node_index)) {
        return mesh_sim_fail(world, MESH_SIM_ERR_ARG);
    }
    node = &world->roles[node_index];
    if (node->route_waiting_valid) {
        return start_route_discovery_for_waiting(world, node_index);
    }
    if (!node->relay.route_discovery.active) {
        return MESH_SIM_OK;
    }
    return start_route_discovery(
        world,
        node_index,
        node->relay.route_discovery.target_id,
        node->relay.pending.packet.msg_type);
}

int mesh_sim_relay_start_connection_tx(
    struct mesh_sim_world *world,
    struct mesh_sim_connection_event *event,
    const struct mesh_channel5_requirements *requirements,
    const struct mesh_event_plan *plan,
    uint64_t tx_start_us,
    struct mesh_sim_connection_tx_result *result)
{
    struct mesh_sim_role_instance *sender = &world->roles[event->sender_index];
    struct mesh_sim_role_instance *receiver =
        &world->roles[event->receiver_index];
    struct mesh_sim_queued_tx queued;
    struct mesh_event_plan tx_plan;
    struct mesh_outbound outbound;
    uint16_t connection_event_index;
    int queue_index;
    int ret;

    *result = (struct mesh_sim_connection_tx_result) {0};
    connection_event_index = (uint16_t)(event - world->connection_events);
    queue_index = mesh_relay_tx_active(&sender->relay) ?
        queue_index_for_peer_during_pending(sender, receiver->id) :
        mesh_sim_relay_queue_index_for_peer(sender, receiver->id);
    if (queue_index < 0) {
        return MESH_SIM_OK;
    }

    queued = sender->tx_queue[queue_index];
    outbound = queued.outbound;
    if (queued.needs_relay_start) {
        ret = mesh_relay_start_channel9_tx(&sender->relay,
                                            &queued.outbound.packet,
                                            queued.outbound.payload,
                                            queued.outbound.payload_len,
                                            requirements,
                                            mesh_sim_time_ms(event->start_us),
                                            &tx_plan,
                                            &outbound);
        if (ret != PROTO_OK) {
            return mesh_sim_fail(world, ret);
        }
        if ((tx_plan.action != MESH_EVENT_PLAN_START &&
             tx_plan.action != MESH_EVENT_PLAN_CLIP) ||
            tx_plan.start_ms != plan->start_ms ||
            tx_plan.end_ms != plan->end_ms) {
            return mesh_sim_fail(world, MESH_SIM_ERR_SENDER_PLAN);
        }
    }

    ret = mesh_sim_radio_schedule_channel9_runtime_tx(world,
                                                       event->sender_index,
                                                       tx_start_us,
                                                       event->end_us,
                                                       &outbound,
                                                       connection_event_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (sender->relay.pending.state ==
            MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD &&
        outbound.packet.msg_type == MSG_GATEWAY_ACK) {
        struct proto_packet confirmed_packet = sender->relay.pending.packet;
        uint32_t actions = MESH_RELAY_ACTION_NONE;

        ret = mesh_relay_commit_transit_gateway_ack_forward(
            &sender->relay,
            &outbound,
            mesh_sim_time_ms(tx_start_us),
            &actions);
        if (ret != PROTO_OK ||
            actions != MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
            return mesh_sim_fail(world, MESH_SIM_ERR_PROTOCOL);
        }
        remove_queued_packet_identity(sender, &confirmed_packet);
        ret = mesh_sim_trace_add_packet(world,
                                        tx_start_us,
                                        sender->id,
                                        sender->gateway_id,
                                        MESH_SIM_TRANSITION_GATEWAY_ACKED,
                                        &confirmed_packet,
                                        0u);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        cancel_completed_pending_tick(world, event->sender_index);
    }
    mesh_sim_relay_remove_queue_entry(sender, (size_t)queue_index);
    result->had_packet = true;
    result->msg_type = outbound.packet.msg_type;
    return MESH_SIM_OK;
}
