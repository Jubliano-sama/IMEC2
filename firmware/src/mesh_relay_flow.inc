#line 6808 __BASE_FILE__
/*
 * Channel-5 delivery protocol: depth, credit, explicit backpressure, batch
 * sequencing and local route repair.  See
 * Documentation/Channel 5 Delivery Protocol.md sections 2, 3, 4 and 6.
 *
 * Everything here is pure relay-core state.  Radio timing (when the follower
 * frame is actually keyed, how long the receiver stays armed) belongs to the
 * application; the core only says what is owed and to whom.
 */

/* --------------------------------------------------------------------------
 * Local depth and poisoning
 * ----------------------------------------------------------------------- */

static struct route_candidate *relay_upstream_candidate_mutable(
    struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return NULL;
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->next_hop_id == next_hop_id &&
            candidate->gateway_id == relay->gateway_id) {
            return candidate;
        }
    }
    return NULL;
}

uint8_t mesh_relay_local_depth(const struct mesh_relay *relay, uint32_t now_ms)
{
    const struct route_candidate *selected;

    if (relay == NULL) {
        return MESH_ROUTE_DEPTH_UNREACHABLE;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        /* The gateway is the root: it is never unreachable from itself. */
        return 0u;
    }
    if (relay->upstream_poisoned) {
        return MESH_ROUTE_DEPTH_UNREACHABLE;
    }
    selected = route_selected(&relay->upstream);
    if (!route_candidate_eligible_at(&relay->upstream, selected, now_ms) ||
        selected->gateway_id != relay->gateway_id ||
        selected->hop_count >= MESH_ROUTE_DEPTH_UNREACHABLE - 1u) {
        return MESH_ROUTE_DEPTH_UNREACHABLE;
    }
    return (uint8_t)(selected->hop_count + 1u);
}

void mesh_relay_poison_upstream(struct mesh_relay *relay)
{
    if (relay == NULL || relay->role == MESH_RELAY_ROLE_GATEWAY) {
        return;
    }
    relay->upstream_poisoned = true;
}

bool mesh_relay_upstream_poisoned(const struct mesh_relay *relay)
{
    return relay != NULL && relay->upstream_poisoned;
}

/* Fresh physical proof of an upstream lifts the poison. */
static void relay_clear_upstream_poison(struct mesh_relay *relay)
{
    if (relay != NULL) {
        relay->upstream_poisoned = false;
    }
}

bool mesh_relay_depth_is_forwardable(const struct mesh_relay *relay,
                                     uint8_t candidate_depth,
                                     uint32_t now_ms)
{
    uint8_t local_depth;

    if (relay == NULL || candidate_depth == MESH_ROUTE_DEPTH_UNREACHABLE) {
        return false;
    }
    local_depth = mesh_relay_local_depth(relay, now_ms);
    if (local_depth == MESH_ROUTE_DEPTH_UNREACHABLE) {
        /*
         * A node that has lost its own route may hand a packet to any
         * neighbour that still has one - including the child that gave the
         * packet to it.  Depth ordering cannot loop when this node advertises
         * no depth at all, because nobody may select it.
         */
        return true;
    }
    return candidate_depth < local_depth;
}

/* --------------------------------------------------------------------------
 * Admission credit
 * ----------------------------------------------------------------------- */

void mesh_relay_set_local_free_slots(struct mesh_relay *relay,
                                     uint16_t free_slots)
{
    if (relay == NULL) {
        return;
    }
    relay->batch.local_free_slots =
        free_slots >= MESH_RELAY_FREE_SLOTS_UNKNOWN ?
            (uint8_t)(MESH_RELAY_FREE_SLOTS_UNKNOWN - 1u) :
            (uint8_t)free_slots;
}

uint8_t mesh_relay_local_credit(const struct mesh_relay *relay)
{
    uint16_t free_slots;

    if (relay == NULL) {
        return 0u;
    }
    if (relay->batch.local_free_slots != MESH_RELAY_FREE_SLOTS_UNKNOWN) {
        free_slots = relay->batch.local_free_slots;
    } else {
        /*
         * No application hook installed: one custody slot exists and the
         * own-report reserve consumes it, so an unconfigured node grants no
         * batch credit.  That is the safe default, not a silent promise.
         */
        free_slots = relay->pending.state == MESH_RELAY_TX_IDLE ? 1u : 0u;
    }
    return mesh_batch_credit_from_free_slots(free_slots);
}

/* Depth + credit every positive ACK this node emits must carry. */
static void relay_ack_flow_control(const struct mesh_relay *relay,
                                   uint32_t now_ms,
                                   struct mesh_ack_flow_control *flow)
{
    memset(flow, 0, sizeof(*flow));
    flow->depth = mesh_relay_local_depth(relay, now_ms);
    flow->depth_valid = true;
    flow->credit = mesh_relay_local_credit(relay);
    flow->credit_valid = true;
}

/* --------------------------------------------------------------------------
 * Explicit backpressure
 * ----------------------------------------------------------------------- */

static uint32_t relay_jittered_retry_after_ms(uint16_t retry_after_ms,
                                              uint32_t random_value)
{
    uint32_t base = retry_after_ms != 0u ?
                        retry_after_ms :
                        MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS;
    uint32_t span = (base * MESH_RELAY_BACKPRESSURE_JITTER_PERCENT) / 100u;
    uint32_t offset;

    if (span == 0u) {
        return base;
    }
    offset = random_value % ((2u * span) + 1u);
    return (base + offset) - span;
}

int mesh_relay_build_backpressure_ack(struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint16_t retry_after_ms,
                                      uint32_t now_ms,
                                      struct mesh_relay_result *result)
{
    struct mesh_ack_flow_control flow;
    struct mesh_outbound *out;
    size_t ack_payload_len = 0u;
    int ret;

    if (result == NULL) {
        return PROTO_ERR_ARG;
    }
    result_reset(result);
    if (relay == NULL || packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u || packet->seq == 0u ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        result->status = PROTO_ERR_ARG;
        return PROTO_ERR_ARG;
    }

    out = relay->role == MESH_RELAY_ROLE_GATEWAY ? &result->gateway_ack :
                                                   &result->hop_ack;
    memset(out, 0, sizeof(*out));

    ret = mesh_append_requested_seq(out->payload,
                                    sizeof(out->payload),
                                    &ack_payload_len,
                                    packet->seq);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }
    /*
     * A full receiver says so.  No accepted identity, the responder's real
     * depth, credit zero and an explicit retry-after: the sender keeps custody
     * and reschedules without penalising the route.
     */
    relay_ack_flow_control(relay, now_ms, &flow);
    flow.credit = 0u;
    flow.retry_after_ms = retry_after_ms != 0u ?
                              retry_after_ms :
                              MESH_RELAY_BACKPRESSURE_RETRY_AFTER_MS;
    flow.retry_after_valid = true;
    ret = mesh_append_ack_flow_control(out->payload,
                                       sizeof(out->payload),
                                       &ack_payload_len,
                                       &flow);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }

    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        ret = mesh_init_gateway_ack(&out->packet,
                                    relay->local_id,
                                    packet->src_id,
                                    packet->session_id,
                                    relay_next_seq(relay),
                                    (uint8_t)ack_payload_len);
        if (ret != PROTO_OK) {
            result->status = ret;
            return ret;
        }
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ACK;
    } else {
        out->packet.msg_type = MSG_MESH_HOP_ACK;
        out->packet.flags = 0u;
        out->packet.src_id = relay->local_id;
        out->packet.dst_id = previous_hop_id;
        out->packet.session_id = packet->session_id;
        out->packet.seq = relay_next_seq(relay);
        out->packet.ttl = MESH_GATEWAY_ACK_TTL;
        out->packet.payload_len = (uint16_t)ack_payload_len;
        result->actions |= MESH_RELAY_ACTION_SEND_HOP_ACK;
    }
    out->payload_len = (uint16_t)ack_payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
    return PROTO_OK;
}

/* --------------------------------------------------------------------------
 * Sender-side reaction to a depth/credit ACK
 * ----------------------------------------------------------------------- */

static bool relay_flow_ack_answers_pending(const struct mesh_relay *relay,
                                           const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    const uint8_t *requested = NULL;
    uint8_t requested_len = 0u;

    if (relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return false;
    }
    if (packet->dst_id != relay->local_id ||
        previous_hop_id != relay->pending.next_hop_id ||
        packet->session_id != relay->pending.packet.session_id) {
        return false;
    }
    if (packet->msg_type == MSG_MESH_HOP_ACK &&
        packet->src_id != previous_hop_id) {
        return false;
    }
    if (tlv_find_unique(payload,
                        payload_len,
                        TLV_REQUESTED_MSG_SEQ,
                        &requested,
                        &requested_len) != PROTO_OK ||
        requested_len != sizeof(uint16_t)) {
        return false;
    }
    return proto_get_u16_le(requested) == relay->pending.packet.seq;
}

/*
 * Fold an ACK's advertised depth and credit into the candidate that sent it.
 * A missing TLV means "unknown": old firmware behaviour is unchanged.
 */
void mesh_relay_note_ack_route_feedback(struct mesh_relay *relay,
                                         uint64_t previous_hop_id,
                                         const struct mesh_ack_flow_control *flow,
                                         uint32_t now_ms)
{
    struct route_candidate *candidate;

    if (relay == NULL || flow == NULL) {
        return;
    }
    candidate = relay_upstream_candidate_mutable(relay, previous_hop_id);
    if (candidate == NULL ||
        candidate->route_epoch != relay->upstream.current_epoch) {
        return;
    }
    if (flow->depth_valid) {
        candidate->hop_count = flow->depth;
    }
    if (flow->credit_valid) {
        candidate->queue_free_hint = flow->credit;
    }
    if (mesh_ack_flow_control_is_dead_end(flow)) {
        candidate->hold_down_until_ms = now_ms + MESH_PARENT_DEAD_END_HOLD_MS;
        candidate->hold_down_valid = true;
    }
    (void)route_select_best_at(&relay->upstream, now_ms);
}

/*
 * A dead end is not an RF failure.  Park the candidate, re-select, keep the
 * bytes.  The candidate's failure_count is deliberately untouched so a routing
 * problem two hops away never exhausts this node's bounded RF retries.
 */
static void relay_handle_dead_end_ack(struct mesh_relay *relay,
                                      uint64_t previous_hop_id,
                                      uint32_t now_ms,
                                      struct mesh_relay_result *result)
{
    uint64_t next_hop_id = 0u;
    relay->pending.gateway_ack_deadline_ms = 0u;
    relay->pending.hop_ack_observed_since_send = false;
    mesh_relay_reset_batch(relay);

    if (mesh_relay_select_next_hop(relay,
                                   relay->pending.packet.dst_id,
                                   &next_hop_id) == PROTO_OK &&
        next_hop_id != previous_hop_id) {
        relay->pending.next_hop_id = next_hop_id;
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        /* Re-selection is immediate: the frame was heard, not lost. */
        relay->pending.retry_after_ms = now_ms;
        relay_clear_upstream_poison(relay);
    } else {
        /*
         * Nothing left upstream.  Poison the local depth so children stop
         * choosing this node, keep custody, and let the application solicit.
         */
        mesh_relay_poison_upstream(relay);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = now_ms + MESH_PARENT_DEAD_END_HOLD_MS;
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
    }
    result->actions |= MESH_RELAY_ACTION_TX_HOP_DEFERRED;
}

static void relay_handle_backpressure_ack(struct mesh_relay *relay,
                                          const struct mesh_ack_flow_control *flow,
                                          uint32_t now_ms,
                                          uint32_t random_value,
                                          struct mesh_relay_result *result)
{
    /*
     * The receiver is alive, routed and simply out of room.  Keep custody,
     * keep the parent, retry when it said to.  No hold-down, no failure count,
     * no route penalty of any kind.
     */
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.gateway_ack_deadline_ms = 0u;
    relay->pending.hop_ack_observed_since_send = false;
    relay->pending.parent_hop_ack_miss_count = 0u;
    relay->pending.retry_after_ms =
        now_ms + relay_jittered_retry_after_ms(flow->retry_after_ms,
                                               random_value);
    mesh_relay_reset_batch(relay);
    (void)route_refresh_candidate_at(&relay->upstream,
                                     relay->pending.next_hop_id,
                                     relay->gateway_id,
                                     now_ms);
    result->actions |= MESH_RELAY_ACTION_TX_HOP_DEFERRED;
}

static int relay_handle_ack_flow_control(struct mesh_relay *relay,
                                         const struct proto_packet *packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t previous_hop_id,
                                         uint32_t now_ms,
                                         uint32_t random_value,
                                         struct mesh_relay_result *result,
                                         bool *handled)
{
    struct mesh_ack_flow_control flow;

    *handled = false;
    if (relay == NULL || packet == NULL || result == NULL ||
        (payload_len > 0u && payload == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (mesh_ack_flow_control_parse(packet, payload, payload_len, &flow) !=
        PROTO_OK) {
        /* Malformed or unrecognised: leave it to the ordinary ACK path. */
        return PROTO_OK;
    }
    if (!flow.depth_valid && !flow.credit_valid) {
        /* Old firmware: unknown depth and credit, unchanged behaviour. */
        return PROTO_OK;
    }
    if (!relay_flow_ack_answers_pending(relay,
                                        packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id)) {
        return PROTO_OK;
    }

    mesh_relay_note_ack_route_feedback(relay, previous_hop_id, &flow, now_ms);

    if (mesh_ack_flow_control_is_dead_end(&flow)) {
        relay_handle_dead_end_ack(relay, previous_hop_id, now_ms, result);
        *handled = true;
        return PROTO_OK;
    }
    if (mesh_ack_flow_control_is_backpressure(&flow)) {
        relay_handle_backpressure_ack(relay,
                                      &flow,
                                      now_ms,
                                      random_value,
                                      result);
        *handled = true;
        return PROTO_OK;
    }
    if (flow.identity_count == 0u) {
        /*
         * Nothing accepted, but the responder named neither a dead end nor an
         * explicit refusal.  Treat it as a refusal with the default delay
         * rather than as silence: an answer is always better than a timeout.
         */
        relay_handle_backpressure_ack(relay,
                                      &flow,
                                      now_ms,
                                      random_value,
                                      result);
        *handled = true;
        return PROTO_OK;
    }

    /* Positive ACK: the credit sizes the burst the sender may now send. */
    if (flow.credit_valid) {
        relay->batch.tx_credit =
            flow.credit < relay->batch.tx_pending ? flow.credit :
                                                    relay->batch.tx_pending;
    }
    relay_clear_upstream_poison(relay);
    return PROTO_OK;
}

/* --------------------------------------------------------------------------
 * Batch sequencing
 * ----------------------------------------------------------------------- */

void mesh_relay_reset_batch(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }
    relay->batch.tx_pending = 0u;
    relay->batch.tx_credit = 0u;
}

int mesh_relay_note_batch_pending(struct mesh_relay *relay,
                                  uint8_t frames_for_next_hop)
{
    if (relay == NULL) {
        return PROTO_ERR_ARG;
    }
    relay->batch.tx_pending = frames_for_next_hop;
    if (relay->batch.tx_credit > frames_for_next_hop) {
        relay->batch.tx_credit = frames_for_next_hop;
    }
    return PROTO_OK;
}

uint8_t mesh_relay_batch_pending(const struct mesh_relay *relay)
{
    return relay == NULL ? 0u : relay->batch.tx_pending;
}

uint8_t mesh_relay_batch_credit(const struct mesh_relay *relay)
{
    return relay == NULL ? 0u : relay->batch.tx_credit;
}

int mesh_relay_append_batch_pending(const struct mesh_relay *relay,
                                    uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset)
{
    if (relay == NULL) {
        return PROTO_ERR_ARG;
    }
    return mesh_append_batch_pending(payload,
                                     payload_cap,
                                     offset,
                                     relay->batch.tx_pending);
}

int mesh_relay_next_burst_frame(struct mesh_relay *relay,
                                uint8_t *remaining_out)
{
    if (relay == NULL || remaining_out == NULL) {
        return PROTO_ERR_ARG;
    }
    if (relay->batch.tx_credit == 0u || relay->batch.tx_pending == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }
    relay->batch.tx_credit--;
    relay->batch.tx_pending--;
    /*
     * REMAINING counts the frames of this burst that still follow.  The burst
     * ends when either the credit or the backlog runs out, whichever is
     * smaller, so the last frame always carries zero.
     */
    *remaining_out = relay->batch.tx_credit < relay->batch.tx_pending ?
                         relay->batch.tx_credit :
                         relay->batch.tx_pending;
    return PROTO_OK;
}

int mesh_relay_note_rx_batch_frame(struct mesh_relay *relay,
                                   uint64_t previous_hop_id,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms)
{
    uint8_t pending = 0u;
    uint8_t remaining = 0u;
    uint8_t expected;
    int ret;

    if (relay == NULL || !id_is_unicast(previous_hop_id) ||
        (payload_len > 0u && payload == NULL)) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_batch_pending_from_payload(payload, payload_len, &pending);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_batch_remaining_from_payload(payload, payload_len, &remaining);
    if (ret != PROTO_OK) {
        return ret;
    }
    /*
     * The first frame of a burst announces the backlog; every follower states
     * how many are still to come.  Either way the receiver keeps its radio
     * armed and defers the single ACK it owes until the burst ends or the
     * application's gap timer fires.
     */
    expected = pending > remaining ? pending : remaining;
    if (expected == 0u) {
        mesh_relay_clear_rx_batch(relay);
        return PROTO_OK;
    }
    relay->batch.rx_peer_id = previous_hop_id;
    relay->batch.rx_remaining = expected;
    relay->batch.rx_deadline_ms = now_ms + MESH_BATCH_FOLLOWER_GAP_MS;
    return PROTO_OK;
}

bool mesh_relay_rx_expects_more(const struct mesh_relay *relay,
                                uint32_t now_ms)
{
    return relay != NULL &&
           relay->batch.rx_remaining > 0u &&
           id_is_unicast(relay->batch.rx_peer_id) &&
           !deadline_reached(now_ms, relay->batch.rx_deadline_ms);
}

uint32_t mesh_relay_rx_batch_deadline_ms(const struct mesh_relay *relay)
{
    if (relay == NULL || relay->batch.rx_remaining == 0u) {
        return 0u;
    }
    return relay->batch.rx_deadline_ms;
}

void mesh_relay_clear_rx_batch(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }
    relay->batch.rx_peer_id = 0u;
    relay->batch.rx_remaining = 0u;
    relay->batch.rx_deadline_ms = 0u;
}

/* --------------------------------------------------------------------------
 * Solicit reply
 * ----------------------------------------------------------------------- */

int mesh_relay_build_solicit_reply(const struct mesh_relay *relay,
                                   const struct proto_packet *solicit,
                                   uint64_t previous_hop_id,
                                   uint32_t now_ms,
                                   struct mesh_outbound *out)
{
    struct gateway_route_adv_fields fields;
    const struct route_candidate *selected;
    const struct mesh_route_path *path;
    size_t payload_len = 0u;
    uint32_t gateway_route_seq;
    uint8_t depth;
    int ret;

    if (relay == NULL || solicit == NULL || out == NULL ||
        solicit->msg_type != MSG_ROUTE_SOLICIT ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        !id_is_unicast(relay->gateway_id)) {
        return PROTO_ERR_ARG;
    }

    depth = mesh_relay_local_depth(relay, now_ms);
    if (depth == MESH_ROUTE_DEPTH_UNREACHABLE ||
        depth >= MESH_ROUTE_PATH_MAX_NODES ||
        depth > MESH_GATEWAY_ROUTE_MAX_DEPTH) {
        /* Only a node with a finite depth answers a solicit. */
        return PROTO_ERR_NOT_FOUND;
    }

    memset(&fields, 0, sizeof(fields));
    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        gateway_route_seq = relay->gateway_route_adv_seq != 0u ?
                                relay->gateway_route_adv_seq :
                                solicit->session_id;
        fields.path.node_ids[0] = relay->gateway_id;
        fields.path.count = 1u;
        fields.path_quality_min = 100u;
        fields.gateway_epoch = (uint16_t)relay->upstream.current_epoch;
    } else {
        selected = route_selected(&relay->upstream);
        path = upstream_candidate_ancestry(relay, selected);
        gateway_route_seq = relay->gateway_route_adv_seq;
        if (path == NULL || path->count != (uint8_t)(depth + 1u) ||
            path->node_ids[path->count - 1u] != relay->local_id) {
            return PROTO_ERR_NOT_FOUND;
        }
        fields.path = *path;
        fields.path_quality_min = selected->link_quality > 100u ?
                                      100u : selected->link_quality;
        fields.gateway_epoch = (uint16_t)selected->route_epoch;
    }
    if (gateway_route_seq == 0u || fields.gateway_epoch == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    fields.gateway_id = relay->gateway_id;
    fields.gateway_route_seq = gateway_route_seq;
    fields.flood_epoch_id = gateway_route_seq;
    fields.hop_count = depth;
    fields.route_cost = gateway_route_cost(fields.hop_count,
                                           fields.path_quality_min);
    fields.gateway_capacity_state = relay_current_capacity_state(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    fields.slot_seed = gateway_route_adv_slot_seed(fields.gateway_id,
                                                   fields.gateway_route_seq,
                                                   fields.gateway_epoch);
    fields.random_backoff_max_ms =
        MESH_GATEWAY_ROUTE_ADV_FIRST_COPY_JITTER_MAX_MS;
    fields.random_backoff_slot_ms = 1u;
    fields.flood_retry_count = FLOOD_DEFAULT_RETRY_COUNT;

    memset(out, 0, sizeof(*out));
    ret = append_gateway_route_adv_tlvs(out->payload,
                                        sizeof(out->payload),
                                        &payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    /*
     * The advert keeps its ordinary wire shape - src_id is always the gateway
     * and dst_id is always broadcast, which is what every receiver validates -
     * while the radio next hop is the single node that asked.  Nobody forwards
     * a solicit and nobody re-floods this reply: its TTL is exactly the depth
     * complement the receiver checks, and the reply is not a wave.
     */
    out->packet.msg_type = MSG_GATEWAY_ROUTE_ADV;
    out->packet.flags = 0u;
    out->packet.src_id = relay->gateway_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = gateway_route_seq;
    out->packet.seq = solicit->seq != 0u ? solicit->seq : 1u;
    out->packet.ttl = (uint8_t)(MESH_NETWORK_MAX_HOPS - fields.hop_count);
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
    out->earliest_tx_ms = now_ms;
    out->earliest_tx_valid = true;
    out->flood_retry_count = 0u;
    return PROTO_OK;
}

/* --------------------------------------------------------------------------
 * Re-adoption of a packet handed back by a former parent
 * ----------------------------------------------------------------------- */

void mesh_relay_note_parent_handoff(struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    uint64_t parent_id)
{
    struct mesh_relay_handoff_memo *slot;

    if (relay == NULL || packet == NULL || !id_is_unicast(parent_id) ||
        packet->src_id != relay->local_id ||
        packet->session_id == 0u || packet->seq == 0u) {
        return;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_HANDOFF_MEMO_SLOTS; i++) {
        slot = &relay->handoff_memo[i];
        if ((slot->flags & MESH_RELAY_HANDOFF_VALID) != 0u &&
            slot->session_id == packet->session_id &&
            slot->seq == packet->seq) {
            slot->parent_id = parent_id;
            slot->flags = MESH_RELAY_HANDOFF_VALID;
            return;
        }
    }
    slot = &relay->handoff_memo[relay->handoff_memo_next];
    relay->handoff_memo_next = (uint8_t)((relay->handoff_memo_next + 1u) %
                                         MESH_RELAY_HANDOFF_MEMO_SLOTS);
    slot->parent_id = parent_id;
    slot->session_id = packet->session_id;
    slot->seq = packet->seq;
    slot->flags = MESH_RELAY_HANDOFF_VALID;
}

void mesh_relay_note_handoff_gateway_acked(struct mesh_relay *relay,
                                           uint32_t session_id,
                                           uint16_t seq)
{
    if (relay == NULL) {
        return;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_HANDOFF_MEMO_SLOTS; i++) {
        struct mesh_relay_handoff_memo *slot = &relay->handoff_memo[i];

        if ((slot->flags & MESH_RELAY_HANDOFF_VALID) != 0u &&
            slot->session_id == session_id && slot->seq == seq) {
            slot->flags |= MESH_RELAY_HANDOFF_GATEWAY_ACKED;
            return;
        }
    }
}

bool mesh_relay_should_readopt_returned_packet(const struct mesh_relay *relay,
                                               const struct proto_packet *packet,
                                               uint64_t previous_hop_id)
{
    if (relay == NULL || packet == NULL ||
        packet->src_id != relay->local_id ||
        packet->session_id == 0u || packet->seq == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return false;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_HANDOFF_MEMO_SLOTS; i++) {
        const struct mesh_relay_handoff_memo *slot = &relay->handoff_memo[i];

        if ((slot->flags & MESH_RELAY_HANDOFF_VALID) != 0u &&
            (slot->flags & MESH_RELAY_HANDOFF_GATEWAY_ACKED) == 0u &&
            slot->session_id == packet->session_id &&
            slot->seq == packet->seq &&
            slot->parent_id == previous_hop_id) {
            return true;
        }
    }
    return false;
}
