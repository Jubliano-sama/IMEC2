#line 1846 __BASE_FILE__
static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void result_bundle_clear(struct mesh_result_bundle_queue *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

static int collection_epoch_id_from_payload(const uint8_t *payload,
                                            size_t payload_len,
                                            uint32_t *collection_epoch_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (collection_epoch_id == NULL) {
        return PROTO_ERR_ARG;
    }
    *collection_epoch_id = 0u;
    ret = tlv_find(payload, payload_len, TLV_COLLECTION_EPOCH_ID, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *collection_epoch_id = proto_get_u32_le(value);
    return *collection_epoch_id == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

static uint32_t outbox_packet_id_for(const struct proto_packet *packet)
{
    uint8_t identity[sizeof(uint32_t) + sizeof(uint16_t)];
    uint16_t crc;

    proto_put_u32_le(&identity[0], packet->session_id);
    proto_put_u16_le(&identity[sizeof(uint32_t)], packet->seq);
    crc = proto_crc16_ccitt_false(identity, sizeof(identity));
    return ((uint32_t)crc << 16) | packet->seq;
}

static uint32_t outbox_age_from_pending(const struct mesh_pending_tx *pending,
                                        uint32_t now_ms)
{
    return packet_age_add(pending->packet.message_age_ms, now_ms - pending->queued_at_ms);
}

static uint8_t outbox_selected_parent_index(const struct mesh_relay *relay,
                                            uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == relay->upstream.current_epoch &&
            candidate->next_hop_id == next_hop_id) {
            return i;
        }
    }
    return UINT8_MAX;
}

static uint32_t outbox_expiry_s_for(const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    uint32_t expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S;
    uint32_t requested_s = 0u;

    if (packet->msg_type != MSG_COMMAND_RESULT) {
        return expiry_s;
    }
    if (find_u32_tlv(payload, payload_len, TLV_COMMAND_EXPIRY_S, &requested_s) == PROTO_OK &&
        requested_s != 0u &&
        requested_s < expiry_s) {
        expiry_s = requested_s;
    }
    return expiry_s;
}

static enum mesh_relay_delivery_state outbox_delivery_state_for(
    const struct mesh_pending_tx *pending)
{
    uint32_t collection_epoch_id = 0u;

    if (pending->packet.msg_type == MSG_COMMAND_RESULT && pending->result_offer_active) {
        return MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK;
    }
    if (pending->packet.msg_type == MSG_COMMAND_RESULT &&
        collection_epoch_id_from_payload(pending->payload,
                                         pending->payload_len,
                                         &collection_epoch_id) == PROTO_OK) {
        return MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    }
    return MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK;
}

static bool pending_has_valid_command_result_id(const struct mesh_relay *relay,
                                                const struct mesh_pending_tx *pending)
{
    struct command_result_id result_id;

    return relay != NULL &&
           pending != NULL &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           pending->packet.dst_id == relay->gateway_id &&
           (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           command_result_id_from_tlvs(pending->payload,
                                       pending->payload_len,
                                       &result_id) == PROTO_OK &&
           result_id.gateway_id == relay->gateway_id &&
           result_id.gateway_epoch == (uint16_t)relay->upstream.current_epoch &&
           result_id.node_id == pending->packet.src_id;
}

static bool outbox_should_track_pending(const struct mesh_relay *relay,
                                        const struct mesh_pending_tx *pending)
{
    /* Gateway-ACK custody belongs to every gateway-bound packet, including
     * a transit packet whose producer is a child.  Restricting this to local
     * producers leaves the transit relay with no pending identity when the
     * child-directed ACK returns, so it can neither validate nor hand off the
     * exact ACK before releasing the original packet. */
    return relay != NULL &&
           pending != NULL &&
           pending->packet.dst_id == relay->gateway_id &&
           (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

static bool pending_is_generic_transit_gateway_packet(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending)
{
    if (relay == NULL || pending == NULL ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.src_id == relay->local_id ||
        pending->packet.dst_id != relay->gateway_id ||
        (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        pending->result_offer_active) {
        return false;
    }

    /* Forwarded command results retain their existing identity contract; the
     * generic branch is for ordinary transit classes such as click reports
     * and mesh data, whose end-to-end identity is already packet-bound. */
    return pending->packet.msg_type != MSG_COMMAND_RESULT;
}

static bool pending_is_local_collection_result(const struct mesh_relay *relay,
                                               const struct mesh_pending_tx *pending)
{
    uint32_t collection_epoch_id = 0u;

    return outbox_should_track_pending(relay, pending) &&
           pending->packet.src_id == relay->local_id &&
           pending->state != MESH_RELAY_TX_IDLE &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           collection_epoch_id_from_payload(pending->payload,
                                            pending->payload_len,
                                            &collection_epoch_id) == PROTO_OK;
}

static bool pending_is_forwarded_command_result(const struct mesh_relay *relay,
                                                const struct mesh_pending_tx *pending)
{
    return outbox_should_track_pending(relay, pending) &&
           pending->packet.src_id != relay->local_id &&
           pending->state != MESH_RELAY_TX_IDLE &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           pending_has_valid_command_result_id(relay, pending);
}

static bool pending_is_result_bundle(const struct mesh_relay *relay,
                                     const struct mesh_pending_tx *pending)
{
    struct result_bundle_header bundle;
    uint8_t header[64];
    size_t header_len = 0u;

    if (!outbox_should_track_pending(relay, pending) ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.msg_type != MSG_RESULT_BUNDLE ||
        result_bundle_header_from_tlvs(pending->payload,
                                       pending->payload_len,
                                       &bundle) != PROTO_OK ||
        bundle.gateway_id != relay->gateway_id ||
        bundle.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        bundle.command_seq == 0u ||
        bundle.collection_epoch_id == 0u ||
        bundle.record_count == 0u ||
        result_bundle_header_append_tlvs(header,
                                         sizeof(header),
                                         &header_len,
                                         &bundle) != PROTO_OK ||
        header_len >= pending->payload_len) {
        return false;
    }
    return bundle.bundle_crc == proto_crc16_ccitt_false(
        &pending->payload[header_len],
        pending->payload_len - header_len);
}

static void outbox_record_clear(struct mesh_relay *relay)
{
    memset(&relay->outbox_record, 0, sizeof(relay->outbox_record));
}

static void outbox_record_sync_age_from_pending(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    if (relay->outbox_record.valid && mesh_relay_tx_active(relay)) {
        relay->outbox_record.age_ms_saturating =
            outbox_age_from_pending(&relay->pending, now_ms);
    }
}

static void outbox_record_track_pending(struct mesh_relay *relay,
                                        uint32_t now_ms)
{
    struct persistent_outbox_record *record;
    const struct mesh_pending_tx *pending;

    if (relay == NULL) {
        return;
    }
    if (!mesh_relay_tx_active(relay)) {
        outbox_record_clear(relay);
        return;
    }

    pending = &relay->pending;
    if (!outbox_should_track_pending(relay, pending)) {
        outbox_record_clear(relay);
        return;
    }

    record = &relay->outbox_record;
    memset(record, 0, sizeof(*record));
    record->valid = true;
    record->delivery_state = outbox_delivery_state_for(pending);
    record->packet_id = outbox_packet_id_for(&pending->packet);
    record->session_id = pending->packet.session_id;
    record->seq = pending->packet.seq;
    record->gateway_id = relay->gateway_id;
    record->packet_class = pending->packet.msg_type;
    record->created_uptime_ms = now_ms;
    record->age_ms_saturating = pending->packet.message_age_ms;
    record->selected_parent_index = outbox_selected_parent_index(relay, pending->next_hop_id);
    record->expiry_s = outbox_expiry_s_for(&pending->packet,
                                           pending->payload,
                                           pending->payload_len);
    if (pending->result_offer_active &&
        record->expiry_s > MESH_RELAY_RESULT_OFFER_EXPIRY_S) {
        record->expiry_s = MESH_RELAY_RESULT_OFFER_EXPIRY_S;
    }
    record->payload_crc = proto_crc16_ccitt_false(pending->payload, pending->payload_len);
    record->payload_len = pending->payload_len;
}

static void outbox_record_mark_gateway_acked(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_GATEWAY_ACKED;
        relay->outbox_record.gateway_acked = true;
    }
}

static void outbox_record_mark_collection_retry(struct mesh_relay *relay,
                                                const struct gateway_collection_eack *eack,
                                                uint32_t now_ms)
{
    if (!relay->outbox_record.valid || eack == NULL) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    relay->outbox_record.retry_round = eack->retry_round;
    relay->outbox_record.gateway_acked = false;
}

static void outbox_record_mark_collection_retry_round(struct mesh_relay *relay,
                                                      uint8_t retry_round,
                                                      uint32_t now_ms)
{
    if (!relay->outbox_record.valid) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    relay->outbox_record.retry_round = retry_round;
    relay->outbox_record.gateway_acked = false;
}

static void outbox_record_mark_collection_closed(struct mesh_relay *relay,
                                                 const struct gateway_collection_eack *eack,
                                                 uint32_t now_ms)
{
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_COLLECTION_CLOSED;
        relay->outbox_record.retry_round = eack != NULL ? eack->retry_round : 0u;
        relay->outbox_record.gateway_acked = false;
    }
}

static bool outbox_record_is_expired(struct mesh_relay *relay, uint32_t now_ms)
{
    uint32_t age_ms;

    if (relay == NULL ||
        !relay->outbox_record.valid ||
        relay->outbox_record.packet_class != MSG_COMMAND_RESULT ||
        relay->outbox_record.expiry_s == 0u ||
        !mesh_relay_tx_active(relay)) {
        return false;
    }

    age_ms = outbox_age_from_pending(&relay->pending, now_ms);
    return (age_ms / 1000u) >= relay->outbox_record.expiry_s;
}

static void outbox_record_mark_expired(struct mesh_relay *relay, uint32_t now_ms)
{
    if (relay == NULL) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_EXPIRED;
        relay->outbox_record.gateway_acked = false;
    }
}

static void outbox_record_note_result_offer_sent(struct mesh_relay *relay,
                                                 uint32_t now_ms)
{
    if (relay == NULL ||
        !relay->pending.result_offer_active ||
        !relay->outbox_record.valid) {
        return;
    }

    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.retry_round < UINT8_MAX) {
        relay->outbox_record.retry_round++;
    }
    relay->outbox_record.delivery_state =
        MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK;
    relay->outbox_record.gateway_acked = false;
}

static bool result_offer_absolute_expiry_ms(const struct mesh_relay *relay,
                                            uint32_t *deadline_ms)
{
    uint32_t expiry_ms;
    uint32_t age_at_queued_ms;

    if (relay == NULL || deadline_ms == NULL ||
        !relay->pending.result_offer_active ||
        !relay->outbox_record.valid ||
        relay->outbox_record.expiry_s == 0u) {
        return false;
    }

    expiry_ms = relay->outbox_record.expiry_s * 1000u;
    age_at_queued_ms = relay->pending.packet.message_age_ms;
    *deadline_ms = age_at_queued_ms >= expiry_ms ?
                   relay->pending.queued_at_ms :
                   relay->pending.queued_at_ms + (expiry_ms - age_at_queued_ms);
    return true;
}

static uint32_t result_offer_clip_retry_at_ms(const struct mesh_relay *relay,
                                              uint32_t retry_at_ms)
{
    uint32_t deadline_ms;

    if (!result_offer_absolute_expiry_ms(relay, &deadline_ms) ||
        !deadline_reached(retry_at_ms, deadline_ms)) {
        return retry_at_ms;
    }
    return deadline_ms;
}

static void result_offer_finish_terminal(
    struct mesh_relay *relay,
    uint32_t now_ms,
    enum mesh_relay_delivery_state delivery_state,
    int status,
    struct mesh_relay_result *result)
{
    const struct proto_packet *packet = &relay->pending.packet;

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    result->terminal.packet = *packet;
    if (relay->pending.payload_len > 0u) {
        memcpy(result->terminal.payload,
               relay->pending.payload,
               relay->pending.payload_len);
    }
    result->terminal.payload_len = relay->pending.payload_len;
    result->terminal.radio_channel = relay->pending.radio_channel;
    result->terminal.next_hop_id = relay->pending.next_hop_id;
    result->terminal.queued_at_ms = now_ms;
    relay->outbox_record.valid = false;
    relay->outbox_record.delivery_state = delivery_state;
    relay->outbox_record.gateway_acked = false;
    relay->pending.state = MESH_RELAY_TX_IDLE;
    result->actions |= MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL;
    result->status = status;
}

static void outbox_record_mark_custody_accepted(struct mesh_relay *relay,
                                                uint64_t custody_parent,
                                                uint32_t now_ms)
{
    if (!relay->outbox_record.valid) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.custody_accepted = true;
    relay->outbox_record.custody_parent = custody_parent;
    relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
}

static bool preserve_pending_gateway_result(struct mesh_relay *relay,
                                            uint32_t now_ms,
                                            uint32_t random_value)
{
    uint32_t retry_delay_ms;

    if (relay == NULL ||
        (!pending_is_local_collection_result(relay, &relay->pending) &&
         !pending_is_forwarded_command_result(relay, &relay->pending) &&
         !pending_is_result_bundle(relay, &relay->pending) &&
         !pending_is_generic_transit_gateway_packet(relay, &relay->pending))) {
        return false;
    }

    retry_delay_ms = pending_busy_retry_delay_ms(relay,
                                                 RELAY_BUSY_RETRY_MIN_MS,
                                                 random_value);
    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms = now_ms + retry_delay_ms;
    if (relay->outbox_record.valid) {
        relay->outbox_record.age_ms_saturating = relay->pending.packet.message_age_ms;
        relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
        relay->outbox_record.gateway_acked = false;
    }
    return true;
}

static bool preserve_local_pending_after_parent_loss(struct mesh_relay *relay,
                                                     uint32_t now_ms,
                                                     uint32_t random_value)
{
    uint32_t retry_delay_ms;

    if (relay == NULL) {
        return false;
    }
    if (preserve_pending_gateway_result(relay, now_ms, random_value)) {
        relay->pending.next_hop_id = 0u;
        relay->pending.gateway_ack_deadline_ms = 0u;
        return true;
    }
    if (relay->pending.packet.src_id != relay->local_id) {
        return false;
    }

    retry_delay_ms = pending_busy_retry_delay_ms(relay,
                                                 RELAY_BUSY_RETRY_MIN_MS,
                                                 random_value);
    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.next_hop_id = 0u;
    relay->pending.retry_after_ms = now_ms + retry_delay_ms;
    relay->pending.gateway_ack_deadline_ms = 0u;
    outbox_record_sync_age_from_pending(relay, now_ms);
    return true;
}

static uint32_t collection_retry_base_for_round(uint8_t retry_round)
{
    switch (retry_round) {
    case 0u:
    case 1u:
        return COLLECTION_RETRY_ROUND_0_MS;
    case 2u:
        return COLLECTION_RETRY_ROUND_1_MS;
    case 3u:
        return COLLECTION_RETRY_ROUND_2_MS;
    case 4u:
        return COLLECTION_RETRY_ROUND_3_MS;
    default:
        return COLLECTION_RETRY_ROUND_STEADY_MS;
    }
}

static int schedule_pending_collection_retry(struct mesh_relay *relay,
                                             uint32_t now_ms,
                                             uint32_t random_value,
                                             struct mesh_relay_result *result)
{
    struct command_result_id pending_id;
    uint32_t collection_epoch_id = 0u;
    uint8_t retry_round;
    uint32_t retry_base_ms;
    uint32_t retry_seed;
    int ret;

    if (relay == NULL || result == NULL ||
        !pending_is_local_collection_result(relay, &relay->pending)) {
        return PROTO_ERR_ARG;
    }

    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = collection_epoch_id_from_payload(relay->pending.payload,
                                           relay->pending.payload_len,
                                           &collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    retry_round = relay->outbox_record.retry_round;
    if (retry_round < UINT8_MAX) {
        retry_round++;
    }
    retry_base_ms = collection_retry_base_for_round(retry_round);
    retry_seed = collection_retry_seed(pending_id.node_id,
                                       pending_id.command_seq,
                                       collection_epoch_id,
                                       retry_round,
                                       random_value);

    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms =
        now_ms + mesh_relay_collection_retry_delay_ms(retry_base_ms, retry_seed);
    outbox_record_mark_collection_retry_round(relay, retry_round, now_ms);
    result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_RETRY;
    return PROTO_OK;
}

static bool result_bundle_key_matches(const struct mesh_result_bundle_queue *queue,
                                      const struct command_result_id *result_id,
                                      uint32_t collection_epoch_id)
{
    return queue != NULL && result_id != NULL &&
           queue->gateway_id == result_id->gateway_id &&
           queue->gateway_epoch == result_id->gateway_epoch &&
           queue->command_seq == result_id->command_seq &&
           queue->collection_epoch_id == collection_epoch_id;
}

static bool result_bundle_entry_exists(const struct mesh_result_bundle_queue *queue,
                                       const struct command_result_id *result_id)
{
    if (queue == NULL || result_id == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < queue->record_count; i++) {
        if (queue->records[i].valid &&
            command_result_id_matches(&queue->records[i].result_id, result_id)) {
            return true;
        }
    }
    return false;
}

static bool command_result_can_bundle(const struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      struct command_result_id *result_id,
                                      uint32_t *collection_epoch_id)
{
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL ||
        result_id == NULL || collection_epoch_id == NULL ||
        relay->role == MESH_RELAY_ROLE_GATEWAY ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->dst_id != relay->gateway_id ||
        packet->ttl == 0u ||
        payload_len == 0u ||
        payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return false;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, result_id);
    if (ret != PROTO_OK) {
        return false;
    }
    if (result_id->gateway_id != relay->gateway_id ||
        result_id->gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id->node_id != packet->src_id) {
        return false;
    }

    ret = collection_epoch_id_from_payload(payload, payload_len, collection_epoch_id);
    return ret == PROTO_OK;
}

static bool result_bundle_ready_to_flush(const struct mesh_result_bundle_queue *queue,
                                         uint32_t now_ms)
{
    if (queue == NULL || !queue->active || queue->record_count == 0u) {
        return false;
    }
    return queue->record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS ||
           deadline_reached(now_ms, queue->due_ms);
}

static int result_bundle_enqueue_command_result(struct mesh_relay *relay,
                                                const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                const struct command_result_id *result_id,
                                                uint32_t collection_epoch_id,
                                                uint32_t now_ms)
{
    struct mesh_result_bundle_queue *queue;
    struct mesh_result_bundle_entry *entry;
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || result_id == NULL ||
        payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }

    ret = mesh_relay_select_next_hop(relay, relay->gateway_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    queue = &relay->result_bundle;
    if (!queue->active) {
        memset(queue, 0, sizeof(*queue));
        queue->active = true;
        queue->gateway_id = result_id->gateway_id;
        queue->gateway_epoch = result_id->gateway_epoch;
        queue->command_seq = result_id->command_seq;
        queue->collection_epoch_id = collection_epoch_id;
        queue->due_ms = now_ms + MESH_RELAY_RESULT_BUNDLE_HOLD_MS;
    } else if (!result_bundle_key_matches(queue, result_id, collection_epoch_id)) {
        return PROTO_ERR_BUSY;
    }

    if (result_bundle_entry_exists(queue, result_id)) {
        return PROTO_ERR_STALE;
    }
    if (queue->record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS) {
        return PROTO_ERR_BUSY;
    }

    entry = &queue->records[queue->record_count];
    memset(entry, 0, sizeof(*entry));
    entry->result_id = *result_id;
    entry->payload_len = (uint16_t)payload_len;
    entry->payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    entry->message_age_ms = packet->message_age_ms;
    entry->queued_at_ms = now_ms;
    memcpy(entry->payload, payload, payload_len);
    entry->valid = true;
    queue->record_count++;
    (void)next_hop_id;
    return PROTO_OK;
}

static int result_bundle_build_outbound(struct mesh_relay *relay,
                                        uint32_t now_ms,
                                        struct mesh_outbound *out)
{
    struct mesh_result_bundle_queue *queue;
    struct result_bundle_header bundle;
    uint8_t header[64];
    size_t header_len = 0u;
    size_t records_len = 0u;
    size_t payload_len;
    uint64_t next_hop_id = 0u;
    uint32_t max_age_ms = 0u;
    uint16_t seq;
    int ret;

    if (relay == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    queue = &relay->result_bundle;
    if (!queue->active || queue->record_count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    ret = mesh_relay_select_next_hop(relay, relay->gateway_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0u; i < queue->record_count; i++) {
        struct mesh_result_bundle_entry *entry = &queue->records[i];
        struct result_bundle_record record;
        uint32_t age_ms;

        if (!entry->valid) {
            return PROTO_ERR_MALFORMED;
        }
        memset(&record, 0, sizeof(record));
        record.result_id = entry->result_id;
        record.payload_len = entry->payload_len;
        record.payload_crc = entry->payload_crc;
        record.payload = entry->payload;
        ret = result_bundle_record_append_tlv(out->payload,
                                              sizeof(out->payload),
                                              &records_len,
                                              &record);
        if (ret != PROTO_OK) {
            return ret;
        }
        age_ms = packet_age_add(entry->message_age_ms, now_ms - entry->queued_at_ms);
        if (age_ms > max_age_ms) {
            max_age_ms = age_ms;
        }
    }

    seq = relay_next_seq(relay);
    memset(&bundle, 0, sizeof(bundle));
    bundle.gateway_id = queue->gateway_id;
    bundle.gateway_epoch = queue->gateway_epoch;
    bundle.command_seq = queue->command_seq;
    bundle.collection_epoch_id = queue->collection_epoch_id;
    bundle.bundle_id = seq;
    bundle.record_count = queue->record_count;
    bundle.bundle_crc = proto_crc16_ccitt_false(out->payload, records_len);

    ret = result_bundle_header_append_tlvs(header,
                                           sizeof(header),
                                           &header_len,
                                           &bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (header_len + records_len > sizeof(out->payload)) {
        return PROTO_ERR_NO_SPACE;
    }

    memmove(&out->payload[header_len], out->payload, records_len);
    memcpy(out->payload, header, header_len);
    payload_len = header_len + records_len;

    out->packet.msg_type = MSG_RESULT_BUNDLE;
    out->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = relay->gateway_id;
    out->packet.session_id = queue->command_seq;
    out->packet.seq = seq;
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->packet.message_age_ms = max_age_ms;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = next_hop_id;
    out->queued_at_ms = now_ms;

    return PROTO_OK;
}

static bool pending_ack_matches(const struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t previous_hop_id,
                                int *status)
{
    const struct mesh_pending_tx *pending = &relay->pending;
    bool ack_targets_local;
    bool ack_targets_transit_origin;
    bool contains = false;
    int ret;

    if (status != NULL) {
        *status = PROTO_OK;
    }
    ack_targets_local = packet->dst_id == relay->local_id;
    ack_targets_transit_origin =
        packet->msg_type == MSG_GATEWAY_ACK &&
        pending->packet.src_id != relay->local_id &&
        packet->dst_id == pending->packet.src_id;
    if (pending->state == MESH_RELAY_TX_IDLE ||
        (!ack_targets_local && !ack_targets_transit_origin) ||
        packet->session_id != pending->packet.session_id ||
        previous_hop_id != pending->next_hop_id) {
        return false;
    }

    ret = ack_payload_contains_seq(payload, payload_len, pending->packet.seq, &contains);
    if (ret != PROTO_OK) {
        if (status != NULL) {
            *status = ret;
        }
        return false;
    }
    return contains;
}

static bool gateway_ack_targets_pending_transit_origin(
    const struct mesh_relay *relay,
    const struct proto_packet *packet)
{
    return relay != NULL && packet != NULL &&
           packet->msg_type == MSG_GATEWAY_ACK &&
           relay->pending.state != MESH_RELAY_TX_IDLE &&
           relay->pending.packet.src_id != relay->local_id &&
           packet->dst_id == relay->pending.packet.src_id;
}

static void refresh_downlink(struct mesh_relay *relay,
                             uint64_t target_id,
                             uint64_t next_hop_id,
                             uint32_t now_ms)
{
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        struct mesh_downlink_entry *entry = downlink_at_mutable(relay, i);

        if (entry->valid &&
            entry->target_id == target_id &&
            entry->next_hop_id == next_hop_id) {
            if (entry->discovery_flood_epoch_id == 0u) {
                entry->last_seen_ms = now_ms;
            }
            return;
        }
    }
}

static int build_gateway_ack(struct mesh_relay *relay,
                             const struct proto_packet *packet,
                             uint64_t previous_hop_id,
                             struct mesh_outbound *out)
{
    uint64_t next_hop_id = previous_hop_id;
    size_t payload_len = 0u;
    int ret;

    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_init_gateway_ack(&out->packet,
                                relay->local_id,
                                packet->src_id,
                                packet->session_id,
                                relay_next_seq(relay),
                                (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (mesh_relay_select_next_hop(relay, packet->src_id, &next_hop_id) != PROTO_OK) {
        next_hop_id = previous_hop_id;
    }
    if (!id_is_unicast(next_hop_id)) {
        return PROTO_ERR_NOT_FOUND;
    }

    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int build_hop_ack(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         uint64_t previous_hop_id,
                         struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->src_id == relay->local_id ||
        packet->dst_id == relay->local_id ||
        packet->dst_id == MESH_BROADCAST_ID) {
        return PROTO_ERR_STALE;
    }

    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_MESH_HOP_ACK;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = previous_hop_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_GATEWAY_ACK_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int parse_relay_busy_tlvs(const uint8_t *payload,
                                 size_t payload_len,
                                 struct relay_busy_fields *fields)
{
    int ret;

    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_REQUESTED_MSG_SESSION_ID,
                       &fields->requested_session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REQUESTED_MSG_SEQ, &fields->requested_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_RETRY_AFTER_MS, &fields->retry_after_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_RELAY_CAPACITY_STATE, &fields->capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->capacity_state > RELAY_CAP_BLACK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = fields->retry_after_ms;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload,
                       payload_len,
                       TLV_ALTERNATE_PARENT_ID,
                       &fields->alternate_parent_id);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->has_alternate_parent = false;
        fields->alternate_parent_id = 0u;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    fields->has_alternate_parent = id_is_unicast(fields->alternate_parent_id);
    return fields->has_alternate_parent ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static int build_busy_response(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t incoming_payload_len,
                               uint64_t previous_hop_id,
                               uint8_t msg_type,
                               struct mesh_outbound *out)
{
    const struct route_candidate *alternate;
    size_t payload_len = 0u;
    uint16_t retry_after_ms;
    uint8_t capacity_state;
    uint64_t alternate_parent_id = 0u;
    bool has_alternate_parent = false;
    bool appended_busy_fields = false;
    int ret;

    if (relay == NULL ||
        packet == NULL ||
        out == NULL ||
        (msg_type != MSG_RELAY_BUSY && msg_type != MSG_RESULT_BUSY) ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    retry_after_ms = relay_busy_retry_after_ms(relay);
    capacity_state = relay_current_capacity_state(relay);

    ret = tlv_append_u32(out->payload,
                         sizeof(out->payload),
                         &payload_len,
                         TLV_REQUESTED_MSG_SESSION_ID,
                         packet->session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    alternate = route_selected(&relay->upstream);
    if (alternate != NULL &&
        alternate->next_hop_id != previous_hop_id &&
        alternate->next_hop_id != packet->src_id) {
        alternate_parent_id = alternate->next_hop_id;
        has_alternate_parent = true;
    }

    if (msg_type == MSG_RESULT_BUSY && payload != NULL && incoming_payload_len > 0u) {
        struct command_result_id result_id;
        ret = command_result_id_from_tlvs(payload, incoming_payload_len, &result_id);
        if (ret == PROTO_OK) {
            const struct result_busy busy = {
                .result_id = result_id,
                .retry_after_ms = retry_after_ms,
                .capacity_state = capacity_state,
                .capacity_validity_interval_ms = retry_after_ms,
                .optional_alternate_parent = alternate_parent_id,
                .has_optional_alternate_parent = has_alternate_parent,
            };

            ret = result_busy_append_tlvs(out->payload,
                                          sizeof(out->payload),
                                          &payload_len,
                                          &busy);
            if (ret != PROTO_OK) {
                return ret;
            }
            appended_busy_fields = true;
        }
    }

    if (!appended_busy_fields) {
        ret = tlv_append_u16(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_RETRY_AFTER_MS,
                             retry_after_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(out->payload,
                            sizeof(out->payload),
                            &payload_len,
                            TLV_RELAY_CAPACITY_STATE,
                            capacity_state);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u16(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                             retry_after_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (has_alternate_parent) {
            ret = tlv_append_u64(out->payload,
                                 sizeof(out->payload),
                                 &payload_len,
                                 TLV_ALTERNATE_PARENT_ID,
                                 alternate_parent_id);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
    }

    out->packet.msg_type = msg_type;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static void add_busy_action(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint64_t previous_hop_id,
                            struct mesh_relay_result *result)
{
    uint8_t busy_msg_type = packet->msg_type == MSG_COMMAND_RESULT ?
                            MSG_RESULT_BUSY :
                            MSG_RELAY_BUSY;
    struct mesh_outbound *out = &result->busy;

    if (build_busy_response(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            busy_msg_type,
                            out) != PROTO_OK) {
        return;
    }
    result->actions |= busy_msg_type == MSG_RESULT_BUSY ?
                       MESH_RELAY_ACTION_SEND_RESULT_BUSY :
                       MESH_RELAY_ACTION_SEND_RELAY_BUSY;
    relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
}

static int build_result_offer_busy_response(struct mesh_relay *relay,
                                            const struct proto_packet *packet,
                                            const struct result_offer *offer,
                                            uint64_t previous_hop_id,
                                            struct mesh_outbound *out)
{
    const struct route_candidate *alternate;
    struct result_busy busy;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || packet == NULL || offer == NULL || out == NULL ||
        !id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    memset(&busy, 0, sizeof(busy));
    busy.result_id = offer->result_id;
    busy.retry_after_ms = relay_busy_retry_after_ms(relay);
    busy.capacity_state = relay_current_capacity_state(relay);
    busy.capacity_validity_interval_ms = busy.retry_after_ms;

    ret = tlv_append_u32(out->payload,
                         sizeof(out->payload),
                         &payload_len,
                         TLV_REQUESTED_MSG_SESSION_ID,
                         packet->session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    alternate = route_selected(&relay->upstream);
    if (alternate != NULL &&
        alternate->next_hop_id != previous_hop_id &&
        alternate->next_hop_id != packet->src_id) {
        busy.optional_alternate_parent = alternate->next_hop_id;
        busy.has_optional_alternate_parent = true;
    }

    ret = result_busy_append_tlvs(out->payload, sizeof(out->payload), &payload_len, &busy);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_RESULT_BUSY;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int build_result_grant_response(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const struct result_offer *offer,
                                       uint64_t previous_hop_id,
                                       struct mesh_outbound *out)
{
    struct result_grant grant;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || packet == NULL || offer == NULL || out == NULL ||
        !id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (offer->result_len == 0u || offer->result_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    memset(&grant, 0, sizeof(grant));
    grant.result_id = offer->result_id;
    grant.granted_channel = UWB_CHANNEL_MESH_PAYLOAD;
    grant.max_bytes = offer->result_len;
    grant.event_offset_hint = 0u;

    ret = result_grant_append_tlvs(out->payload, sizeof(out->payload), &payload_len, &grant);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_RESULT_GRANT;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int handle_result_offer(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint64_t previous_hop_id,
                               struct mesh_relay_result *result)
{
    struct result_offer offer;
    int ret;

    if (packet->dst_id != relay->local_id || packet->msg_type != MSG_RESULT_OFFER) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) || previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = result_offer_from_tlvs(payload, payload_len, &offer);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (offer.result_id.gateway_id != relay->gateway_id ||
        offer.result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        offer.result_len == 0u ||
        offer.result_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    if (mesh_relay_tx_active(relay) ||
        (relay_current_capacity_state(relay) >= RELAY_CAP_RED &&
         !result_offer_reservation_matches_offer(relay, previous_hop_id, &offer))) {
        ret = build_result_offer_busy_response(relay,
                                               packet,
                                               &offer,
                                               previous_hop_id,
                                               &result->busy);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                               MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
        }
        return ret;
    }

    ret = result_offer_reserve(relay, previous_hop_id, &offer);
    if (ret == PROTO_ERR_BUSY) {
        ret = build_result_offer_busy_response(relay,
                                               packet,
                                               &offer,
                                               previous_hop_id,
                                               &result->busy);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                               MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
        }
        return ret;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = build_result_grant_response(relay,
                                      packet,
                                      &offer,
                                      previous_hop_id,
                                      &result->result_grant);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_RESULT_GRANT;
    }
    return ret;
}

static int handle_result_grant(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint64_t previous_hop_id,
                               uint32_t now_ms,
                               uint32_t random_value,
                               struct mesh_relay_result *result)
{
    struct result_grant grant;
    struct command_result_id pending_id;
    int ret;

    if (packet->dst_id != relay->local_id || packet->msg_type != MSG_RESULT_GRANT) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->pending.state != MESH_RELAY_TX_WAIT_RESULT_GRANT ||
        !relay->pending.result_offer_active ||
        relay->pending.next_hop_id != previous_hop_id) {
        return PROTO_OK;
    }

    ret = result_grant_from_tlvs(payload, payload_len, &grant);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!command_result_id_matches(&grant.result_id, &pending_id) ||
        grant.granted_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        grant.max_bytes == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->pending.payload_len > grant.max_bytes) {
        uint32_t retry_delay_ms = pending_busy_retry_delay_ms(
            relay,
            RELAY_BUSY_RETRY_MIN_MS,
            random_value);

        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = result_offer_clip_retry_at_ms(
            relay,
            now_ms + retry_delay_ms);
        result->status = PROTO_ERR_NO_SPACE;
        return PROTO_OK;
    }

    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.result_offer_active = false;
    relay->outbox_record.retry_round = 0u;
    relay->pending.radio_channel = grant.granted_channel;
    relay->pending.next_hop_id = previous_hop_id;
    relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    pending_set_deadlines(&relay->pending, now_ms);
    outbox_record_mark_custody_accepted(relay, previous_hop_id, now_ms);
    ret = outbound_from_pending(relay, &relay->pending, now_ms, &result->retransmit);
    if (ret != PROTO_OK) {
        return ret;
    }
    result->actions |= MESH_RELAY_ACTION_RETRANSMIT;
    return PROTO_OK;
}

static int handle_local_busy(struct mesh_relay *relay,
                             const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len,
                             uint64_t previous_hop_id,
                             uint32_t now_ms,
                             uint32_t random_value,
                             struct mesh_relay_result *result)
{
    struct relay_busy_fields fields = {0};
    uint32_t retry_after_ms;
    int ret;

    if (packet->dst_id != relay->local_id ||
        (packet->msg_type != MSG_RELAY_BUSY && packet->msg_type != MSG_RESULT_BUSY)) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_relay_busy_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (relay->pending.state == MESH_RELAY_TX_IDLE ||
        fields.requested_session_id != relay->pending.packet.session_id ||
        fields.requested_seq != relay->pending.packet.seq) {
        return PROTO_OK;
    }
    if (packet->msg_type == MSG_RESULT_BUSY &&
        relay->pending.packet.msg_type == MSG_COMMAND_RESULT &&
        relay->pending.result_offer_active) {
        struct result_busy busy;
        struct command_result_id pending_id;

        ret = result_busy_from_tlvs(payload, payload_len, &busy);
        if (ret != PROTO_OK) {
            return PROTO_OK;
        }
        ret = command_result_id_from_tlvs(relay->pending.payload,
                                          relay->pending.payload_len,
                                          &pending_id);
        if (ret != PROTO_OK ||
            !command_result_id_matches(&busy.result_id, &pending_id)) {
            return PROTO_OK;
        }
        if (relay->outbox_record.retry_round >=
            MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS) {
            result_offer_finish_terminal(
                relay,
                now_ms,
                MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED,
                MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED,
                result);
            return PROTO_OK;
        }
    }

    retry_after_ms = fields.retry_after_ms;
    if (retry_after_ms < RELAY_BUSY_RETRY_MIN_MS) {
        retry_after_ms = RELAY_BUSY_RETRY_MIN_MS;
    } else if (retry_after_ms > RELAY_BUSY_RETRY_MAX_MS) {
        retry_after_ms = RELAY_BUSY_RETRY_MAX_MS;
    }
    retry_after_ms = pending_busy_retry_delay_ms(relay,
                                                 retry_after_ms,
                                                 random_value);
    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    route_update_capacity_hint(&relay->upstream,
                               previous_hop_id,
                               relay->gateway_id,
                               fields.capacity_state,
                               0u,
                               0u,
                               now_ms,
                               capacity_valid_until_ms(
                                   now_ms,
                                   fields.capacity_validity_interval_ms),
                               now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms = result_offer_clip_retry_at_ms(
        relay,
        now_ms + retry_after_ms);
    result->actions |= MESH_RELAY_ACTION_TX_RELAY_BUSY;
    return PROTO_OK;
}

static int build_forward(const struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         struct mesh_outbound *out);

static int handle_local_ack(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint64_t previous_hop_id,
                            uint32_t now_ms,
                            struct mesh_relay_result *result)
{
    bool transit_gateway_ack;
    int ret;

    transit_gateway_ack = gateway_ack_targets_pending_transit_origin(relay,
                                                                     packet);
    if ((packet->dst_id != relay->local_id && !transit_gateway_ack) ||
        (packet->msg_type != MSG_GATEWAY_ACK &&
         packet->msg_type != MSG_MESH_HOP_ACK)) {
        return PROTO_OK;
    }

    if (!pending_ack_matches(relay,
                             packet,
                             payload,
                             payload_len,
                             previous_hop_id,
                             &ret)) {
        if (ret != PROTO_OK) {
            result->status = ret;
            return ret;
        }
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }

    if (packet->msg_type == MSG_GATEWAY_ACK &&
        (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
         relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
         relay->pending.state ==
             MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD) &&
        packet->src_id == relay->gateway_id) {
        if (relay->outbox_record.delivery_state ==
            MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK) {
            return PROTO_OK;
        }
        if (transit_gateway_ack) {
            /*
             * The first matching gateway ACK creates exactly one physical
             * child-directed handoff.  Further copies can arrive while that
             * handoff is queued or on air; rebuilding it here both refreshes
             * the deadline and lets an ACK storm keep the relay awake after
             * the operation has otherwise completed.
             */
            if (relay->pending.state ==
                    MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD &&
                relay->pending.gateway_ack_forward_pending) {
                return PROTO_OK;
            }
            ret = build_forward(relay,
                                packet,
                                payload,
                                payload_len,
                                &result->forward);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
                return ret;
            }
            /*
             * This is only phase one.  Keep the original transit packet and
             * persistent outbox live until the app proves that this exact ACK
             * physically reached its child-directed transmit path.
             */
            pending_refresh_age(&relay->pending, now_ms);
            outbox_record_sync_age_from_pending(relay, now_ms);
            relay->pending.state =
                MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
            relay->pending.gateway_ack_forward_pending = true;
            relay->pending.gateway_ack_deadline_ms =
                now_ms + MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;
            result->actions |= MESH_RELAY_ACTION_FORWARD |
                MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING;
            return PROTO_OK;
        }
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
    } else if (packet->msg_type == MSG_MESH_HOP_ACK &&
               (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
                relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF)) {
        pending_set_deadlines(&relay->pending, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
        route_refresh_selected_at(&relay->upstream, now_ms);
        result->actions |= MESH_RELAY_ACTION_TX_HOP_PROGRESS;
    }

    return PROTO_OK;
}

int mesh_relay_commit_transit_gateway_ack_forward(
    struct mesh_relay *relay,
    const struct mesh_outbound *forwarded_ack,
    uint32_t now_ms,
    uint32_t *actions)
{
    bool contains = false;
    uint64_t expected_next_hop_id = 0u;
    int ret;

    if (relay == NULL || forwarded_ack == NULL || actions == NULL) {
        return PROTO_ERR_ARG;
    }
    *actions = MESH_RELAY_ACTION_NONE;
    if (relay->pending.state == MESH_RELAY_TX_IDLE ||
        !relay->pending.gateway_ack_forward_pending ||
        relay->pending.packet.src_id == relay->local_id ||
        relay->outbox_record.delivery_state ==
            MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK ||
        forwarded_ack->packet.msg_type != MSG_GATEWAY_ACK ||
        forwarded_ack->packet.src_id != relay->gateway_id ||
        forwarded_ack->packet.dst_id != relay->pending.packet.src_id ||
        forwarded_ack->packet.session_id !=
            relay->pending.packet.session_id ||
        forwarded_ack->packet.payload_len != forwarded_ack->payload_len ||
        forwarded_ack->payload_len == 0u ||
        forwarded_ack->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !id_is_unicast(forwarded_ack->next_hop_id) ||
        forwarded_ack->next_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_select_next_hop(relay,
                                     relay->pending.packet.src_id,
                                     &expected_next_hop_id);
    if (ret != PROTO_OK ||
        forwarded_ack->next_hop_id != expected_next_hop_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = ack_payload_contains_seq(forwarded_ack->payload,
                                   forwarded_ack->payload_len,
                                   relay->pending.packet.seq,
                                   &contains);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!contains) {
        return PROTO_ERR_NOT_FOUND;
    }

    mesh_relay_note_route_discovery_ready(relay,
                                          relay->pending.packet.dst_id);
    outbox_record_mark_gateway_acked(relay, now_ms);
    relay->pending.gateway_ack_forward_pending = false;
    relay->pending.state = MESH_RELAY_TX_IDLE;
    route_record_success_at(&relay->upstream, now_ms);
    *actions = MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
    return PROTO_OK;
}

static int build_forward(const struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (packet->ttl <= 1u) {
        return PROTO_ERR_STALE;
    }
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static bool packet_needs_forward(const struct mesh_relay *relay, const struct proto_packet *packet)
{
    return packet->dst_id != relay->local_id && packet->dst_id != MESH_BROADCAST_ID;
}

static bool gateway_targeted_control_can_forward_independently(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id)
{
    const struct mesh_downlink_entry *downlink;

    if (relay == NULL || packet == NULL ||
        packet->src_id != relay->gateway_id ||
        (packet->msg_type != MSG_COMMAND &&
         packet->msg_type != MSG_SURVEY_PAIR_PREPARE) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id == relay->local_id ||
        packet->ttl <= 1u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return false;
    }

    downlink = mesh_relay_find_downlink(relay, packet->dst_id);
    return downlink != NULL && downlink->valid &&
           downlink->route_epoch == relay->upstream.current_epoch &&
           downlink->gateway_id == relay->gateway_id &&
           id_is_unicast(downlink->next_hop_id) &&
           downlink->next_hop_id != relay->local_id &&
           downlink->next_hop_id != previous_hop_id;
}

bool mesh_relay_packet_requires_channel9_payload_event(
    const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_GATEWAY_COLLECTION_EACK:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static bool tlv_u32_nonzero(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK &&
           value_len == sizeof(uint32_t) &&
           proto_get_u32_le(value) != 0u;
}

static bool tlv_u16_nonzero(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK &&
           value_len == sizeof(uint16_t) &&
           proto_get_u16_le(value) != 0u;
}

static bool command_flood_broadcast_valid(const uint8_t *payload, size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t scope;
    uint8_t response_mode = CMD_RESPONSE_SMALL_RESULT;
    int ret;

    ret = tlv_find(payload, payload_len, TLV_COMMAND_SCOPE, &value, &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint8_t)) {
        return false;
    }
    scope = value[0];
    if (scope == CMD_SCOPE_SINGLE_NODE ||
        (scope != CMD_SCOPE_GROUP &&
         scope != CMD_SCOPE_ALL_REGISTERED &&
         scope != CMD_SCOPE_ALL_HEARD)) {
        return false;
    }

    ret = tlv_find(payload, payload_len, TLV_COMMAND_RESPONSE_MODE, &value, &value_len);
    if (ret == PROTO_OK) {
        if (value_len != sizeof(uint8_t)) {
            return false;
        }
        response_mode = value[0];
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return false;
    }
    if (response_mode != CMD_RESPONSE_NONE &&
        response_mode != CMD_RESPONSE_ACK_ONLY &&
        response_mode != CMD_RESPONSE_SMALL_RESULT &&
        response_mode != CMD_RESPONSE_LARGE_RESULT) {
        return false;
    }

    if (!tlv_u32_nonzero(payload, payload_len, TLV_COMMAND_SEQ) ||
        !tlv_u32_nonzero(payload, payload_len, TLV_FLOOD_EPOCH_ID)) {
        return false;
    }
    if (scope == CMD_SCOPE_ALL_REGISTERED &&
        (!tlv_u16_nonzero(payload, payload_len, TLV_MEMBERSHIP_EPOCH) ||
         !tlv_u16_nonzero(payload, payload_len, TLV_EXPECTED_NODE_COUNT))) {
        return false;
    }
    if (response_mode != CMD_RESPONSE_NONE &&
        (!tlv_u32_nonzero(payload, payload_len, TLV_COLLECTION_EPOCH_ID) ||
         !tlv_u32_nonzero(payload, payload_len, TLV_COLLECTION_SLOT_SEED))) {
        return false;
    }

    return true;
}

static bool collection_eack_broadcast_valid(const struct mesh_relay *relay,
                                            const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len)
{
    struct gateway_collection_eack eack;

    if (relay == NULL || packet == NULL ||
        packet->src_id != relay->gateway_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        gateway_collection_eack_packet_validate(packet,
                                                payload,
                                                payload_len,
                                                &eack) != PROTO_OK) {
        return false;
    }
    if (eack.gateway_id != relay->gateway_id ||
        eack.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        eack.command_seq == 0u ||
        eack.collection_epoch_id == 0u ||
        eack.received_count > eack.expected_count) {
        return false;
    }
    return true;
}

static int handle_collection_eack_for_pending(struct mesh_relay *relay,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t now_ms,
                                              uint32_t random_value,
                                              struct mesh_relay_result *result)
{
    struct gateway_collection_eack eack;
    struct command_result_id pending_id;
    uint32_t collection_epoch_id = 0u;
    bool listed = false;
    int ret;

    if (relay == NULL || result == NULL || payload == NULL ||
        relay->pending.state == MESH_RELAY_TX_IDLE ||
        relay->pending.packet.msg_type != MSG_COMMAND_RESULT ||
        relay->pending.packet.dst_id != relay->gateway_id) {
        return PROTO_OK;
    }

    ret = gateway_collection_eack_from_tlvs(payload, payload_len, &eack);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return PROTO_OK;
    }
    ret = collection_epoch_id_from_payload(relay->pending.payload,
                                           relay->pending.payload_len,
                                           &collection_epoch_id);
    if (ret != PROTO_OK) {
        return PROTO_OK;
    }

    if (eack.gateway_id != pending_id.gateway_id ||
        eack.gateway_epoch != pending_id.gateway_epoch ||
        eack.command_seq != pending_id.command_seq ||
        eack.collection_epoch_id != collection_epoch_id ||
        pending_id.node_id != relay->local_id) {
        return PROTO_OK;
    }

    if (!eack.collection_open) {
        result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_CLOSED;
        outbox_record_mark_collection_closed(relay, &eack, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        return PROTO_OK;
    }

    if (eack.eack_format != EACK_FORMAT_EXPLICIT_RECEIVED_LIST &&
        eack.eack_format != EACK_FORMAT_EXPLICIT_MISSING_LIST) {
        return PROTO_OK;
    }

    ret = gateway_collection_eack_contains_node_id(payload,
                                                   payload_len,
                                                   pending_id.node_id,
                                                   &listed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST && listed) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }
    if (eack.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST && !listed) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }

    if ((eack.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST && listed) ||
        (eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST && !listed)) {
        uint32_t retry_base_ms = eack.next_retry_spread_ms == 0u ?
                                 COLLECTION_RETRY_ROUND_0_MS :
                                 eack.next_retry_spread_ms;
        uint32_t retry_seed = collection_retry_seed(pending_id.node_id,
                                                    pending_id.command_seq,
                                                    collection_epoch_id,
                                                    eack.retry_round,
                                                    random_value);

        pending_refresh_age(&relay->pending, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms =
            now_ms + mesh_relay_collection_retry_delay_ms(retry_base_ms, retry_seed);
        outbox_record_mark_collection_retry(relay, &eack, now_ms);
        result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_RETRY;
    }
    return PROTO_OK;
}

static bool broadcast_packet_needs_forward(const struct mesh_relay *relay,
                                           const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    if (packet->dst_id != MESH_BROADCAST_ID || packet->ttl == 0u) {
        return false;
    }
    if (packet->msg_type == MSG_SURVEY_DISCOVERY_START) {
        return true;
    }
    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK) {
        return collection_eack_broadcast_valid(relay, packet, payload, payload_len);
    }
    return packet->msg_type == MSG_COMMAND &&
           command_flood_broadcast_valid(payload, payload_len);
}

static int build_broadcast_forward(const struct mesh_relay *relay,
                                   const struct proto_packet *packet,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms,
                                   uint32_t random_value,
                                   struct mesh_outbound *out)
{
    struct flood_control_fields flood_control;
    uint32_t slot_seed;
    int ret;

    if (!broadcast_packet_needs_forward(relay, packet, payload, payload_len)) {
        return PROTO_ERR_STALE;
    }

    flood_control_defaults(&flood_control);
    if (packet->msg_type == MSG_COMMAND) {
        ret = parse_flood_control_tlvs(payload, payload_len, &flood_control);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    /*
     * Survey-start forwarding already owns four randomized RF opportunities
     * inside the bounded flood.  Inheriting the generic outer retry count
     * multiplies those opportunities and can delay local survey acceptance
     * until the shared discovery epoch has passed.
     */
    out->flood_retry_count = packet->msg_type == MSG_SURVEY_DISCOVERY_START ?
                                 0u : flood_control.retry_count;
    if (packet->msg_type == MSG_COMMAND) {
        ret = ensure_flood_control_tlvs(out,
                                        &flood_control,
                                        packet->message_age_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    slot_seed = flood_identity_seed(packet, relay->local_id);
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(
        relay->local_id,
        slot_seed,
        packet->ttl,
        packet->msg_type == MSG_COMMAND ? &flood_control : NULL,
        random_value);
    return PROTO_OK;
}

static bool local_delivery_needs_response(const struct mesh_relay *relay,
                                          const struct proto_packet *packet)
{
    if (packet->dst_id != relay->local_id) {
        return false;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY &&
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
        return true;
    }
    return relay->role == MESH_RELAY_ROLE_ANCHOR && packet->msg_type == MSG_COMMAND;
}

static int add_gateway_ack_action(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    int ret;

    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return PROTO_OK;
    }

    ret = build_gateway_ack(relay, packet, previous_hop_id, &result->gateway_ack);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ACK;
    } else if (result->status == PROTO_OK) {
        result->status = ret;
    }
    return ret;
}

static bool gateway_delivery_requires_commit(
    const struct mesh_relay *relay,
    const struct proto_packet *packet)
{
    if (relay == NULL || packet == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        packet->dst_id != relay->local_id ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static bool gateway_collection_duplicate_requires_redelivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *collection_epoch = NULL;
    uint8_t collection_epoch_len = 0u;

    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        return true;
    }
    return packet->msg_type == MSG_COMMAND_RESULT &&
           tlv_find(payload,
                    payload_len,
                    TLV_COLLECTION_EPOCH_ID,
                    &collection_epoch,
                    &collection_epoch_len) == PROTO_OK &&
           collection_epoch_len == sizeof(uint32_t);
}

int mesh_relay_commit_gateway_delivery(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint32_t now_ms,
                                       struct mesh_relay_result *result)
{
    int ret;

    if (result == NULL) {
        return PROTO_ERR_ARG;
    }
    result_reset(result);

    if (relay == NULL || packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u || packet->seq == 0u ||
        !id_is_unicast(packet->src_id) || packet->src_id == 0u ||
        packet->src_id == relay->local_id ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        packet->dst_id != relay->local_id ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !id_is_unicast(previous_hop_id) || previous_hop_id == 0u ||
        previous_hop_id == relay->local_id) {
        result->status = PROTO_ERR_ARG;
        return PROTO_ERR_ARG;
    }
    if (!gateway_delivery_requires_commit(relay, packet)) {
        result->status = PROTO_ERR_MALFORMED;
        return PROTO_ERR_MALFORMED;
    }

    ret = build_gateway_ack(relay, packet, previous_hop_id, &result->gateway_ack);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }

    ret = gateway_ack_history_store(relay,
                                    packet,
                                    payload,
                                    payload_len,
                                    now_ms,
                                    true);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }
    duplicate_store_payload_identity(relay,
                                     packet,
                                     payload,
                                     payload_len,
                                     now_ms);
    result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ACK;
    return PROTO_OK;
}

static void add_hop_ack_action(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               uint64_t previous_hop_id,
                               struct mesh_relay_result *result)
{
    int ret;

    ret = build_hop_ack(relay, packet, previous_hop_id, &result->hop_ack);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_HOP_ACK;
    }
}

static void add_route_reply_backup(struct mesh_relay *relay,
                                   const struct route_discovery_fields *fields,
                                   uint32_t session_id,
                                   uint64_t primary_next_hop,
                                   struct mesh_relay_result *result)
{
    const struct mesh_downlink_entry *backup;
    uint16_t reply_nonce;

    if (relay == NULL ||
        fields == NULL ||
        result == NULL ||
        !id_is_unicast(fields->origin_id) ||
        !id_is_unicast(fields->target_id) ||
        session_id == 0u ||
        fields->flood_epoch_id == 0u ||
        !id_is_unicast(primary_next_hop)) {
        return;
    }

    reply_nonce = fields->reply_nonce != 0u ? fields->reply_nonce :
        mesh_route_reply_nonce(fields->origin_id,
                               fields->target_id,
                               session_id,
                               fields->flood_epoch_id);
    backup = downlink_backup_for_discovery(relay,
                                           fields->origin_id,
                                           fields->target_id,
                                           session_id,
                                           fields->flood_epoch_id,
                                           reply_nonce,
                                           primary_next_hop);
    if (backup == NULL || !id_is_unicast(backup->next_hop_id)) {
        return;
    }

    result->route_reply_backup_next_hop_id = backup->next_hop_id;
    result->route_reply_backup_valid = true;
}

static int upsert_reactive_route(struct mesh_relay *relay,
                                 uint64_t target_id,
                                 uint64_t next_hop_id,
                                 uint32_t route_epoch,
                                 uint8_t advertised_hop_count,
                                 uint8_t quality,
                                 uint8_t relay_capacity_state,
                                 uint16_t queue_free_hint,
                                 uint8_t channel9_busy_hint,
                                 uint16_t capacity_validity_interval_ms,
                                 uint32_t now_ms,
                                 const struct route_discovery_fields *discovery,
                                 uint32_t discovery_session_id,
                                 struct reactive_route_rollback *rollback)
{
    int ret;

    if (rollback != NULL) {
        memset(rollback, 0, sizeof(*rollback));
    }
    if (!id_is_unicast(target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        advertised_hop_count == UINT8_MAX ||
        quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    if (discovery != NULL &&
        (discovery->origin_id != target_id ||
         !id_is_unicast(discovery->target_id) ||
         discovery->target_id == discovery->origin_id ||
         discovery_session_id == 0u ||
         discovery->flood_epoch_id == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    if (target_id == relay->local_id) {
        return PROTO_OK;
    }

    if (target_id == relay->gateway_id) {
        struct route_candidate candidate = {
            .next_hop_id = next_hop_id,
            .gateway_id = target_id,
            .route_epoch = route_epoch,
            .last_seen_ms = now_ms,
            .hop_count = advertised_hop_count,
            .link_quality = quality,
            .relay_capacity_state = relay_capacity_state,
            .queue_free_hint = queue_free_hint,
            .channel9_busy_hint = channel9_busy_hint,
            .capacity_observed_at_ms = capacity_validity_interval_ms != 0u ? now_ms : 0u,
            .capacity_valid_until_ms = capacity_valid_until_ms(
                now_ms,
                capacity_validity_interval_ms),
            .valid = true,
        };

        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_OK;
        }
        if (rollback != NULL) {
            rollback->kind = REACTIVE_ROUTE_ROLLBACK_UPSTREAM;
            rollback->state.upstream = relay->upstream;
        }
        ret = route_upsert_candidate(&relay->upstream, &candidate);
        if (ret != PROTO_OK && rollback != NULL) {
            rollback->kind = REACTIVE_ROUTE_ROLLBACK_NONE;
        }
        return ret;
    }

    if (advertised_hop_count + 1u == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    struct mesh_downlink_entry entry = {
        .target_id = target_id,
        .next_hop_id = next_hop_id,
        .gateway_id = discovery != NULL ? discovery->target_id : relay->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = discovery != NULL ? discovery_session_id : now_ms,
        .discovery_flood_epoch_id = discovery != NULL ?
            discovery->flood_epoch_id : 0u,
        .hop_count = advertised_hop_count + 1u,
        .quality = quality,
        .valid = true,
    };

    if (rollback != NULL) {
        rollback->kind = REACTIVE_ROUTE_ROLLBACK_DOWNLINK;
    }
    ret = upsert_downlink(relay,
                          &entry,
                          rollback != NULL ? &rollback->state.downlink : NULL);
    if (ret != PROTO_OK && rollback != NULL) {
        rollback->kind = REACTIVE_ROUTE_ROLLBACK_NONE;
    }
    return ret;
}

static void rollback_reactive_route(struct mesh_relay *relay,
                                    struct reactive_route_rollback *rollback)
{
    if (relay == NULL || rollback == NULL) {
        return;
    }

    if (rollback->kind == REACTIVE_ROUTE_ROLLBACK_UPSTREAM) {
        relay->upstream = rollback->state.upstream;
    } else if (rollback->kind == REACTIVE_ROUTE_ROLLBACK_DOWNLINK &&
               rollback->state.downlink.valid &&
               rollback->state.downlink.index <
                   mesh_relay_downlink_capacity(relay)) {
        *downlink_at_mutable(relay, rollback->state.downlink.index) =
             rollback->state.downlink.previous;
    }
    rollback->kind = REACTIVE_ROUTE_ROLLBACK_NONE;
}
