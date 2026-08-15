#line 1846 __BASE_FILE__
#define GATEWAY_ACK_RECOVERY_AUTHORIZED 0x01u
#define GATEWAY_ACK_RECOVERY_QUARANTINE 0x02u

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
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          &value,
                          &value_len);
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

uint32_t mesh_relay_outbox_expiry_s_for_packet(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint32_t expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S;
    uint32_t requested_s = 0u;

    /* A source no longer replaces its original with ACK_CONFIRM. Any confirm
     * that enters this generic outbox is therefore transit-only and cannot
     * reconstruct the original after a gateway reset; keep that limitation
     * explicit and bounded by the ordinary one-day terminal horizon. */
    if (packet->msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        return expiry_s;
    }
    if (packet->msg_type == MSG_SURVEY_DISCOVERY_REPORT ||
        packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        return SURVEY_MESH_RESULT_OUTBOX_EXPIRY_S;
    }
    if (packet->msg_type != MSG_COMMAND_RESULT) {
        return expiry_s;
    }
    if (command_result_has_command_id(payload,
                                      payload_len,
                                      CMD_SURVEY_PREPARE_PAIR) ||
        command_result_has_command_id(payload,
                                      payload_len,
                                      CMD_SURVEY_START_PAIR)) {
        return SURVEY_PAIR_CONTROL_RESULT_OUTBOX_EXPIRY_S;
    }
    if (command_result_has_command_id(payload,
                                      payload_len,
                                      CMD_ASSIGN_DISCOVERY_SLOTS)) {
        return DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S;
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
    if (pending->packet.msg_type == MSG_RESULT_BUNDLE ||
        (pending->packet.msg_type == MSG_COMMAND_RESULT &&
         collection_epoch_id_from_payload(pending->payload,
                                          pending->payload_len,
                                          &collection_epoch_id) == PROTO_OK)) {
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

static bool pending_is_gateway_ack_confirm(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending)
{
    struct mesh_gateway_ack_confirm_identity identity;

    return outbox_should_track_pending(relay, pending) &&
           pending->state != MESH_RELAY_TX_IDLE &&
           pending->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM &&
           mesh_gateway_ack_confirm_payload_parse(
               &pending->packet,
               pending->payload,
               pending->payload_len,
               &identity) == PROTO_OK;
}

static bool pending_is_local_gateway_host_packet(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending)
{
    if (!outbox_should_track_pending(relay, pending) ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.src_id != relay->local_id ||
        pending->result_offer_active ||
        pending->packet.msg_type == MSG_COMMAND_RESULT ||
        pending->packet.msg_type == MSG_RESULT_BUNDLE ||
        pending->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        return false;
    }
    return mesh_gateway_ack_confirmed_type(pending->packet.msg_type) &&
           mesh_gateway_ack_confirmed_flags_valid(
               pending->packet.msg_type,
               pending->packet.flags);
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
    const size_t header_len = RESULT_BUNDLE_HEADER_TLV_BYTES;

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
    struct mesh_outbox_record *record;
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
    if (!relay_semantic_packet_digest(&pending->packet,
                                      pending->payload,
                                      pending->payload_len,
                                      record->semantic_digest)) {
        return;
    }
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
    record->expiry_s = mesh_relay_outbox_expiry_s_for_packet(
        &pending->packet,
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
        relay->outbox_record.expiry_s == 0u ||
        relay->outbox_record.expiry_s == UINT32_MAX ||
        !mesh_relay_tx_active(relay)) {
        return false;
    }

    age_ms = outbox_age_from_pending(&relay->pending, now_ms);
    return (age_ms / 1000u) >= relay->outbox_record.expiry_s;
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
    result->terminal.queued_at_valid = true;
    relay->outbox_record.delivery_state = delivery_state;
    relay->outbox_record.gateway_acked = false;
    relay->pending.state = MESH_RELAY_TX_WAIT_TERMINAL_COMMIT;
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

/*
 * A retry transition keeps the exact live relay record authoritative.  A
 * terminal record instead belongs to its exact cleanup owner and must never
 * be converted back into RF retry custody by an unrelated preemption or
 * parent-loss callback.
 */
static bool pending_state_retryable_for_deferral(
    const struct mesh_pending_tx *pending)
{
    return pending != NULL &&
           (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
            pending->state == MESH_RELAY_TX_WAIT_RESULT_GRANT ||
            pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
}

static bool pending_expired_for_deferral(const struct mesh_relay *relay,
                                         uint32_t now_ms)
{
    uint64_t expiry_ms;

    if (relay == NULL || !mesh_relay_tx_active(relay)) {
        return true;
    }

    expiry_ms = (uint64_t)mesh_relay_outbox_expiry_s_for_packet(
        &relay->pending.packet,
        relay->pending.payload,
        relay->pending.payload_len) * 1000u;
    return (uint64_t)outbox_age_from_pending(&relay->pending, now_ms) >=
           expiry_ms;
}

static bool preserve_pending_gateway_result(struct mesh_relay *relay,
                                            uint32_t now_ms,
                                            uint32_t random_value)
{
    uint32_t retry_delay_ms;

    if (relay == NULL ||
        !pending_state_retryable_for_deferral(&relay->pending) ||
        pending_expired_for_deferral(relay, now_ms) ||
        (!pending_is_local_collection_result(relay, &relay->pending) &&
         !pending_is_forwarded_command_result(relay, &relay->pending) &&
         !pending_is_result_bundle(relay, &relay->pending) &&
         !pending_is_gateway_ack_confirm(relay, &relay->pending) &&
         !pending_is_local_gateway_host_packet(relay, &relay->pending) &&
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

    if (relay == NULL ||
        !pending_state_retryable_for_deferral(&relay->pending)) {
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

static int result_bundle_entry_classify(
    const struct mesh_result_bundle_queue *queue,
    const struct command_result_id *result_id,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (queue == NULL || result_id == NULL || semantic_digest == NULL) {
        return PROTO_ERR_ARG;
    }
    for (uint8_t i = 0u; i < queue->record_count; i++) {
        if (queue->records[i].valid &&
            command_result_id_matches(&queue->records[i].result_id, result_id)) {
            return semantic_digest_equal(queue->records[i].semantic_digest,
                                         semantic_digest,
                                         SEMANTIC_DIGEST_SHA256_LEN) ?
                       PROTO_ERR_STALE :
                       PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_ERR_NOT_FOUND;
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
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
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
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || result_id == NULL ||
        payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }
    if (!semantic_digest_sha256(payload, payload_len, semantic_digest)) {
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

    ret = result_bundle_entry_classify(queue, result_id, semantic_digest);
    if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (queue->record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS) {
        return PROTO_ERR_BUSY;
    }

    entry = &queue->records[queue->record_count];
    memset(entry, 0, sizeof(*entry));
    entry->result_id = *result_id;
    memcpy(entry->semantic_digest,
           semantic_digest,
           sizeof(entry->semantic_digest));
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
        uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
        uint32_t age_ms;

        if (!entry->valid ||
            !semantic_digest_sha256(entry->payload,
                                    entry->payload_len,
                                    semantic_digest) ||
            !semantic_digest_equal(entry->semantic_digest,
                                   semantic_digest,
                                   sizeof(semantic_digest))) {
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
    out->queued_at_valid = true;

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
    struct proto_packet transient_confirm;
    uint8_t transient_confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    const struct proto_packet *acknowledged_packet = &pending->packet;
    const uint8_t *acknowledged_payload = pending->payload;
    size_t acknowledged_payload_len = pending->payload_len;
    size_t transient_confirm_payload_len = 0u;
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
        previous_hop_id != pending->next_hop_id ||
        (pending->gateway_ack_recovery_flags &
         GATEWAY_ACK_RECOVERY_QUARANTINE) != 0u) {
        return false;
    }
    if (packet->msg_type == MSG_MESH_HOP_ACK &&
        packet->src_id != previous_hop_id) {
        if (status != NULL) {
            *status = PROTO_ERR_MALFORMED;
        }
        return false;
    }

    if (pending->gateway_ack_confirm_pending) {
        ret = mesh_relay_pending_gateway_ack_confirm_wire(
            relay,
            pending->queued_at_ms,
            &transient_confirm,
            transient_confirm_payload,
            sizeof(transient_confirm_payload),
            &transient_confirm_payload_len);
        if (ret != PROTO_OK) {
            if (status != NULL) {
                *status = ret;
            }
            return false;
        }
        acknowledged_packet = &transient_confirm;
        acknowledged_payload = transient_confirm_payload;
        acknowledged_payload_len = transient_confirm_payload_len;
    }

    ret = mesh_ack_payload_contains_packet(packet,
                                           payload,
                                           payload_len,
                                           acknowledged_packet,
                                           acknowledged_payload,
                                           acknowledged_payload_len,
                                           &contains);
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

#if !IMEC_MESH_RELAY_GATEWAY_ONLY
static bool gateway_ack_exactly_targets_pending_transit_origin(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    int status = PROTO_OK;

    return gateway_ack_targets_pending_transit_origin(relay, packet) &&
           pending_ack_matches(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               &status) &&
           status == PROTO_OK;
}

static bool gateway_ack_exactly_targets_pending_transit_confirm(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct proto_packet confirm;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t confirm_payload_len = 0u;
    bool contains = false;

    if (!gateway_ack_targets_pending_transit_origin(relay, packet) ||
        previous_hop_id != relay->pending.next_hop_id ||
        packet->session_id != relay->pending.packet.session_id ||
        mesh_gateway_ack_confirm_payload_build(
            &relay->pending.packet,
            relay->pending.payload,
            relay->pending.payload_len,
            confirm_payload,
            sizeof(confirm_payload),
            &confirm_payload_len) != PROTO_OK ||
        mesh_init_gateway_ack_confirm(&confirm,
                                      relay->pending.packet.src_id,
                                      relay->gateway_id,
                                      relay->pending.packet.session_id,
                                      relay->pending.packet.seq) != PROTO_OK ||
        mesh_ack_payload_contains_packet(packet,
                                         payload,
                                         payload_len,
                                         &confirm,
                                         confirm_payload,
                                         confirm_payload_len,
                                         &contains) != PROTO_OK) {
        return false;
    }

    return contains;
}
#endif

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
                             const uint8_t *payload,
                             size_t acknowledged_payload_len,
                             uint64_t previous_hop_id,
                             struct mesh_outbound *out)
{
    size_t ack_payload_len = 0u;
    int ret;

    ret = mesh_append_requested_seq(out->payload,
                                    sizeof(out->payload),
                                    &ack_payload_len,
                                    packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_ack_semantic_identity(out->payload,
                                            sizeof(out->payload),
                                            &ack_payload_len,
                                            packet,
                                            payload,
                                            acknowledged_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_init_gateway_ack(&out->packet,
                                relay->local_id,
                                packet->src_id,
                                packet->session_id,
                                relay_next_seq(relay),
                                (uint8_t)ack_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    out->payload_len = (uint16_t)ack_payload_len;
    /*
     * Return acceptance over the exact live reverse edge that delivered the
     * packet.  A cached downlink can describe an older topology; preferring it
     * here can strand the upstream relay's retained custody even though that
     * relay has just proved a working path to the gateway.  The ACK's logical
     * destination remains the original source while the physical next hop is
     * the ingress peer that owns the corresponding transit record.
     */
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int build_hop_ack(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t acknowledged_payload_len,
                         uint64_t previous_hop_id,
                         struct mesh_outbound *out)
{
    size_t ack_payload_len = 0u;
    int ret;

    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->src_id == relay->local_id ||
        packet->dst_id == relay->local_id ||
        packet->dst_id == MESH_BROADCAST_ID) {
        return PROTO_ERR_STALE;
    }

    ret = mesh_append_requested_seq(out->payload,
                                    sizeof(out->payload),
                                    &ack_payload_len,
                                    packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_ack_semantic_identity(out->payload,
                                            sizeof(out->payload),
                                            &ack_payload_len,
                                            packet,
                                            payload,
                                            acknowledged_payload_len);
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
    out->packet.payload_len = (uint16_t)ack_payload_len;
    out->payload_len = (uint16_t)ack_payload_len;
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
    /*
     * BUSY is a one-hop custody response.  Address the physical sender whose
     * pending state it correlates; targeting the end-to-end origin with TTL=1
     * strands transit BUSY at the first relay and leaves that sender stalled.
     */
    out->packet.dst_id = previous_hop_id;
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
                               uint32_t now_ms,
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
    if (!command_result_id_is_well_formed(&offer.result_id) ||
        offer.result_id.gateway_id != relay->gateway_id ||
        offer.result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        offer.result_id.node_id != packet->src_id ||
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

    ret = result_offer_reserve(relay, previous_hop_id, &offer, now_ms);
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
    if (!id_is_unicast(relay->pending.next_hop_id) ||
        previous_hop_id != relay->pending.next_hop_id) {
        /*
         * BUSY is physical-hop flow control, so matching the end-to-end
         * packet identity is insufficient. A different peer must not defer
         * this attempt or poison the selected parent's capacity hint.
         */
        return PROTO_ERR_MALFORMED;
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
    /* BUSY is scoped to the exact packet and the parent's current custody
     * slot.  It is not evidence that the proven route or negotiated radio
     * timing failed.  Persisting the advertised transient capacity here made
     * a RED response unselect the current parent, so every ordinary retry
     * restarted route discovery and Channel-9 negotiation.  Keep the route
     * byte-for-byte intact and retry this packet after the bounded backoff;
     * actual RF/ACK failure remains the sole route-repair authority. */
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
                         uint64_t ingress_previous_hop_id,
                         bool exact_return_edge_required,
                         uint32_t now_ms,
                         struct mesh_outbound *out);

static uint32_t next_gateway_ack_forward_owner_generation(
    struct mesh_relay *relay)
{
    if (relay == NULL) {
        return 0u;
    }

    relay->next_handoff_owner_generation++;
    if (relay->next_handoff_owner_generation == 0u) {
        relay->next_handoff_owner_generation = 1u;
    }
    return relay->next_handoff_owner_generation;
}

static bool pending_can_generate_gateway_ack_confirm(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending)
{
    if (relay == NULL || pending == NULL ||
        pending->packet.src_id != relay->local_id ||
        pending->packet.dst_id != relay->gateway_id ||
        !mesh_gateway_ack_confirmed_type(pending->packet.msg_type) ||
        pending->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        return false;
    }

    /*
     * Collection traffic has its own same-boot RAM end-to-end EACK. Its ordinary
     * gateway ACK means only "gateway collection state accepted", so converting on
     * that intermediate ACK would bypass roster/retry completion.
     */
    if (pending->packet.msg_type == MSG_RESULT_BUNDLE ||
        relay->outbox_record.delivery_state ==
            MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK) {
        return false;
    }
    return pending->packet.msg_type != MSG_MESH_DATA ||
           pending->packet.flags ==
               (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC);
}

static bool pending_requires_gateway_ack_confirm(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending)
{
    return pending != NULL && !pending->gateway_ack_confirm_pending &&
           pending_can_generate_gateway_ack_confirm(relay, pending);
}

int mesh_relay_pending_gateway_ack_confirm_wire(
    const struct mesh_relay *relay,
    uint32_t now_ms,
    struct proto_packet *confirm_packet,
    uint8_t *confirm_payload,
    size_t confirm_payload_capacity,
    size_t *confirm_payload_len)
{
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || confirm_packet == NULL || confirm_payload == NULL ||
        confirm_payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    *confirm_payload_len = 0u;
    if (!relay->pending.gateway_ack_confirm_pending ||
        relay->pending.state == MESH_RELAY_TX_IDLE ||
        !pending_can_generate_gateway_ack_confirm(relay, &relay->pending)) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (confirm_payload_capacity < MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = mesh_gateway_ack_confirm_payload_build(
        &relay->pending.packet,
        relay->pending.payload,
        relay->pending.payload_len,
        confirm_payload,
        confirm_payload_capacity,
        &payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_init_gateway_ack_confirm(confirm_packet,
                                        relay->local_id,
                                        relay->gateway_id,
                                        relay->pending.packet.session_id,
                                        relay->pending.packet.seq);
    if (ret != PROTO_OK ||
        payload_len != MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    confirm_packet->message_age_ms = packet_age_add(
        0u, now_ms - relay->pending.gateway_ack_confirm_started_ms);
    *confirm_payload_len = payload_len;
    return PROTO_OK;
}

static bool pending_gateway_ack_confirm_recovery_due(
    const struct mesh_relay *relay,
    uint32_t now_ms)
{
    return relay != NULL && relay->pending.gateway_ack_confirm_pending &&
           (relay->pending.gateway_ack_recovery_flags &
            GATEWAY_ACK_RECOVERY_AUTHORIZED) != 0u &&
           deadline_reached(
               now_ms,
               relay->pending.gateway_ack_confirm_started_ms +
                   MESH_RELAY_GATEWAY_ACK_CONFIRM_REPLAY_MS);
}

static void pending_recover_original_after_gateway_ack_confirm(
    struct mesh_relay *relay,
    uint32_t now_ms)
{
    if (relay == NULL || !relay->pending.gateway_ack_confirm_pending) {
        return;
    }

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->pending.gateway_ack_confirm_pending = false;
    relay->pending.gateway_ack_recovery_flags |=
        GATEWAY_ACK_RECOVERY_AUTHORIZED |
        GATEWAY_ACK_RECOVERY_QUARANTINE;
    relay->pending.gateway_ack_confirm_started_ms = 0u;
    relay->pending.gateway_ack_deadline_ms = 0u;
    relay->pending.retry_after_ms =
        now_ms + MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS;
    relay->pending.next_hop_id = 0u;
    relay->pending.radio_channel =
        mesh_relay_packet_requires_channel9_payload_event(
            &relay->pending.packet) ?
            UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
}

static bool pending_recover_original_after_new_gateway_epoch(
    struct mesh_relay *relay,
    uint32_t now_ms)
{
    if (relay == NULL || !relay->pending.gateway_ack_confirm_pending ||
        !route_epoch_strictly_newer(
            relay->upstream.current_epoch,
            relay->pending.gateway_ack_confirm_route_epoch)) {
        return false;
    }

    relay->pending.gateway_ack_recovery_flags |=
        GATEWAY_ACK_RECOVERY_AUTHORIZED;
    pending_recover_original_after_gateway_ack_confirm(relay, now_ms);
    return true;
}

static int pending_transition_to_gateway_ack_confirm(
    struct mesh_relay *relay,
    uint32_t now_ms)
{
    uint64_t next_hop_id = 0u;

    if (relay == NULL ||
        !pending_requires_gateway_ack_confirm(relay, &relay->pending)) {
        return PROTO_ERR_MALFORMED;
    }

    /*
     * A route can disappear at the ACK boundary. Keep the immutable original
     * bytes as the sole source owner while a compact confirmation wire image
     * is generated on demand. If the gateway lost its RAM history, bounded
     * recovery replays these exact original bytes through host receipt again.
     */
    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    (void)mesh_relay_expire_routes(relay, now_ms);
    if (mesh_relay_select_next_hop(relay,
                                   relay->gateway_id,
                                   &next_hop_id) != PROTO_OK) {
        next_hop_id = 0u;
    }

    relay->pending.gateway_ack_confirm_pending = true;
    relay->pending.gateway_ack_recovery_flags &=
        GATEWAY_ACK_RECOVERY_AUTHORIZED;
    relay->pending.gateway_ack_confirm_started_ms = now_ms;
    relay->pending.gateway_ack_confirm_route_epoch =
        relay->upstream.current_epoch;
    relay->pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.retry_after_ms = now_ms + 1u;
    relay->pending.gateway_ack_deadline_ms = 0u;
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    if (!relay->outbox_record.valid) {
        relay->pending.gateway_ack_confirm_pending = false;
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

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
                                relay->pending.transit_previous_hop_id,
                                true,
                                now_ms,
                                &result->forward);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
                if (ret == PROTO_ERR_NOT_FOUND) {
                    result->actions |=
                        MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_ROUTE_REPAIR;
                }
                return ret;
            }
            /*
             * This is only phase one.  Keep the original transit packet and
             * retained outbox live until the app proves that this exact ACK
             * physically reached its child-directed transmit path.
             */
            pending_refresh_age(&relay->pending, now_ms);
            outbox_record_sync_age_from_pending(relay, now_ms);
            if (!relay->pending.gateway_ack_forward_pending) {
                relay->pending.gateway_ack_forward_previous_hop_id =
                    previous_hop_id;
                relay->pending.gateway_ack_forward_next_hop_id =
                    result->forward.next_hop_id;
                relay->pending.gateway_ack_forward_owner_generation =
                    next_gateway_ack_forward_owner_generation(relay);
            } else {
                /* The first app admission may still own the old ACK bytes.
                 * Retry with that handoff's identity so either the retained
                 * first ACK or this rebuilt copy can complete it. */
                result->forward.next_hop_id =
                    relay->pending.gateway_ack_forward_next_hop_id;
            }
            result->forward.handoff_owner_generation =
                relay->pending.gateway_ack_forward_owner_generation;
            relay->pending.state =
                MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
            relay->pending.gateway_ack_forward_pending = true;
            relay->pending.gateway_ack_deadline_ms =
                now_ms + MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;
            result->actions |= MESH_RELAY_ACTION_FORWARD |
                MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING;
            return PROTO_OK;
        }
        if (relay->pending.gateway_ack_confirm_pending) {
            /* The app must retire every exact producer-owned raw record before
             * committing this compact proof. Keep the immutable original intact
             * until mesh_relay_commit_gateway_ack_confirm_terminal(). */
            result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
            return PROTO_OK;
        }
        if (pending_requires_gateway_ack_confirm(relay, &relay->pending)) {
            ret = pending_transition_to_gateway_ack_confirm(relay, now_ms);
            if (ret != PROTO_OK) {
                result->status = ret;
                return ret;
            }
            result->actions |=
                MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING;
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
        if (relay->pending.hop_ack_observed_since_send) {
            /*
             * An exact duplicate ACK proves no new RF progress. Let the
             * existing gateway-ACK deadline or retry backoff continue so an
             * ACK replay storm cannot retain custody indefinitely.
             */
            return PROTO_OK;
        }
        relay->pending.hop_ack_observed_since_send = true;
        relay->pending.gateway_ack_deadline_ms =
            now_ms + ROUTE_GATEWAY_ACK_PROGRESS_TIMEOUT_MS;
        relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
        route_refresh_selected_at(&relay->upstream, now_ms);
        result->actions |= MESH_RELAY_ACTION_TX_HOP_PROGRESS;
    }

    return PROTO_OK;
}

int mesh_relay_commit_gateway_ack_confirm_terminal(
    struct mesh_relay *relay,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint32_t now_ms)
{
    struct mesh_gateway_ack_confirm_identity identity;
    struct proto_packet expected_confirm;
    uint8_t expected_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t expected_payload_len = 0u;
    int ret;

    if (relay == NULL || confirm_packet == NULL ||
        confirm_payload == NULL ||
        mesh_gateway_ack_confirm_payload_parse(confirm_packet,
                                               confirm_payload,
                                               confirm_payload_len,
                                               &identity) != PROTO_OK ||
        !relay->pending.gateway_ack_confirm_pending) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_pending_gateway_ack_confirm_wire(
        relay,
        now_ms,
        &expected_confirm,
        expected_payload,
        sizeof(expected_payload),
        &expected_payload_len);
    if (ret != PROTO_OK ||
        expected_confirm.msg_type != confirm_packet->msg_type ||
        expected_confirm.flags != confirm_packet->flags ||
        expected_confirm.src_id != confirm_packet->src_id ||
        expected_confirm.dst_id != confirm_packet->dst_id ||
        expected_confirm.session_id != confirm_packet->session_id ||
        expected_confirm.seq != confirm_packet->seq ||
        expected_payload_len != confirm_payload_len ||
        !semantic_digest_equal(expected_payload,
                               confirm_payload,
                               confirm_payload_len)) {
        return PROTO_ERR_MALFORMED;
    }

    mesh_relay_note_route_discovery_ready(relay,
                                          relay->pending.packet.dst_id);
    outbox_record_mark_gateway_acked(relay, now_ms);
    relay->pending.gateway_ack_confirm_pending = false;
    relay->pending.gateway_ack_recovery_flags = 0u;
    relay->pending.gateway_ack_confirm_started_ms = 0u;
    relay->pending.gateway_ack_confirm_route_epoch = 0u;
    relay->pending.state = MESH_RELAY_TX_IDLE;
    route_record_success_at(&relay->upstream, now_ms);
    return PROTO_OK;
}

int mesh_relay_commit_terminal_release(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t pending_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t provided_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (relay == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u) ||
        relay->pending.state != MESH_RELAY_TX_WAIT_TERMINAL_COMMIT ||
        !relay->outbox_record.valid ||
        (relay->outbox_record.delivery_state !=
             MESH_RELAY_DELIVERY_EXPIRED &&
         relay->outbox_record.delivery_state !=
             MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED) ||
        relay->pending.packet.msg_type != packet->msg_type ||
        relay->pending.packet.flags != packet->flags ||
        relay->pending.packet.src_id != packet->src_id ||
        relay->pending.packet.dst_id != packet->dst_id ||
        relay->pending.packet.session_id != packet->session_id ||
        relay->pending.packet.seq != packet->seq ||
        relay->pending.packet.payload_len != packet->payload_len ||
        relay->pending.payload_len != payload_len ||
        packet->payload_len != payload_len ||
        !relay_semantic_packet_digest(&relay->pending.packet,
                                      relay->pending.payload,
                                      relay->pending.payload_len,
                                      pending_digest) ||
        !relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      provided_digest) ||
        !semantic_digest_equal(pending_digest,
                               provided_digest,
                               sizeof(pending_digest)) ||
        !semantic_digest_equal(relay->outbox_record.semantic_digest,
                               pending_digest,
                               sizeof(pending_digest))) {
        return PROTO_ERR_MALFORMED;
    }

    relay->outbox_record.valid = false;
    relay->pending.result_offer_active = false;
    relay->pending.state = MESH_RELAY_TX_IDLE;
    return PROTO_OK;
}

int mesh_relay_commit_transit_gateway_ack_forward(
    struct mesh_relay *relay,
    const struct mesh_outbound *forwarded_ack,
    uint32_t now_ms,
    uint32_t *actions)
{
    bool contains = false;
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
        forwarded_ack->packet.payload_len != forwarded_ack->payload_len ||
        forwarded_ack->payload_len == 0u ||
        forwarded_ack->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !id_is_unicast(forwarded_ack->next_hop_id) ||
        forwarded_ack->next_hop_id == relay->local_id ||
        !id_is_unicast(
            relay->pending.gateway_ack_forward_previous_hop_id) ||
        relay->pending.gateway_ack_forward_previous_hop_id ==
            relay->local_id ||
        !id_is_unicast(relay->pending.gateway_ack_forward_next_hop_id) ||
        relay->pending.gateway_ack_forward_next_hop_id == relay->local_id ||
        relay->pending.gateway_ack_forward_owner_generation == 0u ||
        forwarded_ack->handoff_owner_generation !=
            relay->pending.gateway_ack_forward_owner_generation ||
        forwarded_ack->next_hop_id !=
            relay->pending.gateway_ack_forward_next_hop_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_ack_payload_contains_packet(
        &forwarded_ack->packet,
        forwarded_ack->payload,
        forwarded_ack->payload_len,
        &relay->pending.packet,
        relay->pending.payload,
        relay->pending.payload_len,
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
                         uint64_t ingress_previous_hop_id,
                         bool exact_return_edge_required,
                         uint32_t now_ms,
                         struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (packet->ttl <= 1u) {
        return PROTO_ERR_STALE;
    }
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
    if (exact_return_edge_required) {
        if (packet->msg_type != MSG_GATEWAY_ACK ||
            packet->src_id != relay->gateway_id ||
            packet->dst_id == relay->local_id) {
            return PROTO_ERR_MALFORMED;
        }
        if (!id_is_unicast(ingress_previous_hop_id) ||
            ingress_previous_hop_id == relay->local_id) {
            return PROTO_ERR_NOT_FOUND;
        }
        next_hop_id = ingress_previous_hop_id;
    } else {
        ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
#else
    (void)ingress_previous_hop_id;
    (void)exact_return_edge_required;
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
#endif

    out->packet = *packet;
    out->packet.ttl = (uint8_t)(packet->ttl - 1u);
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    /*
     * The received packet already includes age through this relay's ingress
     * boundary. Start a new residence interval here so the physical sender
     * also accounts for route/control handoff and radio wait time. Without
     * this timestamp, every forwarded hop silently loses its local delay and
     * separately delivered controls derive different "shared" deadlines.
     */
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
    out->ingress_previous_hop_id = ingress_previous_hop_id;
#else
    (void)ingress_previous_hop_id;
#endif
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
    case MSG_GATEWAY_ACK_CONFIRM:
    case MSG_GATEWAY_COLLECTION_EACK:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

bool mesh_relay_packet_can_queue_gateway_report(
    const struct mesh_relay *relay,
    const struct proto_packet *packet)
{
    if (relay == NULL || packet == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        packet->dst_id != relay->gateway_id ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_MESH_DATA:
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

    return tlv_find_unique(payload,
                           payload_len,
                           type,
                           &value,
                           &value_len) == PROTO_OK &&
           value_len == sizeof(uint32_t) &&
           proto_get_u32_le(value) != 0u;
}

static bool tlv_u16_nonzero(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find_unique(payload,
                           payload_len,
                           type,
                           &value,
                           &value_len) == PROTO_OK &&
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

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_SCOPE,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint8_t)) {
        return false;
    }
    scope = value[0];
    if (scope == CMD_SCOPE_SINGLE_NODE ||
        (scope != CMD_SCOPE_ALL_REGISTERED &&
         scope != CMD_SCOPE_ALL_HEARD)) {
        return false;
    }

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_RESPONSE_MODE,
                          &value,
                          &value_len);
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

/*
 * A relayed command keeps the gateway as its immutable logical source while
 * each physical relay decrements the gateway-owned origin TTL. Reject the
 * envelope before replay, routing, or forwarding state can consume it.
 */
static bool command_packet_envelope_valid(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct gateway_command_options options;
    enum command_id command_id;
    uint8_t origin_ttl;

    if (relay == NULL || packet == NULL ||
        packet->msg_type != MSG_COMMAND ||
        !id_is_unicast(relay->gateway_id) ||
        packet->src_id != relay->gateway_id ||
        (packet->flags & (uint8_t)~FLAG_DIAGNOSTIC) != 0u ||
        packet->payload_len != payload_len ||
        payload == NULL ||
        packet->ttl == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        (packet->dst_id != MESH_BROADCAST_ID &&
         (packet->dst_id == 0u || packet->dst_id == relay->gateway_id)) ||
        gateway_command_extract_id(payload,
                                   payload_len,
                                   &command_id) != PROTO_OK ||
        gateway_command_extract_options(payload,
                                        payload_len,
                                        &options) != PROTO_OK) {
        return false;
    }

    origin_ttl = gateway_command_origin_ttl(command_id);
    if (packet->ttl > origin_ttl ||
        ((packet->ttl == origin_ttl) !=
         (previous_hop_id == relay->gateway_id))) {
        return false;
    }
    if (packet->dst_id == MESH_BROADCAST_ID) {
        /* Broadcast commands carry their application identity in the command
         * sequence TLV, so their header session is not required to match it. */
        return options.flood_required &&
               command_flood_broadcast_valid(payload, payload_len);
    }
    return !options.flood_required;
}

/*
 * Gateway BLE prioritization consults FLAG_COUNT_AS_CLICK before msg_type.
 * Enforce every gateway-bound producer's exact flag shape before a forged
 * control/status packet can enter duplicate or reliable delivery state as a
 * highest-priority click.
 */
static bool gateway_bound_packet_flags_valid(
    const struct mesh_relay *relay,
    const struct proto_packet *packet)
{
    const uint8_t ack = FLAG_GATEWAY_ACK_REQUIRED;
    const uint8_t diagnostic = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;

    if (relay == NULL || packet == NULL ||
        packet->dst_id != relay->gateway_id) {
        return true;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
        return packet->flags == (ack | FLAG_COUNT_AS_CLICK) ||
               packet->flags == diagnostic;
    case MSG_SELF_TEST_REPORT:
        return packet->flags == diagnostic;
    case MSG_ANCHOR_HEARTBEAT:
        return packet->flags == 0u;
    case MSG_RESULT_BUNDLE:
        return packet->flags == ack;
    case MSG_COMMAND_RESULT:
        return packet->flags == ack || packet->flags == diagnostic;
    case MSG_MESH_DATA:
        return packet->flags == diagnostic;
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return packet->flags == diagnostic;
    default:
        return true;
    }
}

static bool collection_eack_broadcast_valid(const struct mesh_relay *relay,
                                            const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len)
{
    struct gateway_collection_eack eack;
    uint32_t recovery_attempt_id = 0u;
    int recovery_ret;

    if (relay == NULL || packet == NULL ||
        packet->src_id != relay->gateway_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        packet->ttl > FLOOD_EPOCH_GLOBAL_TTL ||
        gateway_collection_eack_packet_validate(packet,
                                                payload,
                                                payload_len,
                                                &eack) != PROTO_OK) {
        return false;
    }
    recovery_ret = gateway_collection_eack_recovery_attempt_id(
        payload, payload_len, &recovery_attempt_id);
    if (recovery_ret != PROTO_OK &&
        recovery_ret != PROTO_ERR_NOT_FOUND) {
        return false;
    }
    if (eack.gateway_id != relay->gateway_id ||
        (eack.gateway_epoch != (uint16_t)relay->upstream.current_epoch &&
         recovery_ret != PROTO_OK) ||
        eack.command_seq == 0u ||
        eack.collection_epoch_id == 0u ||
        eack.received_count > eack.expected_count) {
        return false;
    }
    return true;
}

static bool collection_eack_rx_envelope_valid(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    if (!collection_eack_broadcast_valid(relay,
                                          packet,
                                          payload,
                                          payload_len) ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return false;
    }

    /*
     * The gateway originates the broadcast at the network-wide TTL.  Every
     * relay copy must have consumed at least one hop.  This relationship is
     * checked before duplicate admission so a shortened forged copy cannot
     * suppress the later canonical flood.
     */
    return previous_hop_id == relay->gateway_id ?
           packet->ttl == FLOOD_EPOCH_GLOBAL_TTL :
           packet->ttl < FLOOD_EPOCH_GLOBAL_TTL;
}

#if !IMEC_MESH_RELAY_GATEWAY_ONLY
static int handle_collection_eack_for_pending(struct mesh_relay *relay,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t now_ms,
                                              uint32_t random_value,
                                              struct mesh_relay_result *result)
{
    struct gateway_collection_eack eack;
    struct gateway_collection_recovery_identity recovery_identity;
    struct command_result_id pending_id;
    uint8_t pending_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t collection_epoch_id = 0u;
    uint32_t recovery_attempt_id = 0u;
    bool listed = false;
    int ret;

    if (relay == NULL || result == NULL || payload == NULL ||
        relay->pending.state == MESH_RELAY_TX_IDLE ||
        (relay->pending.packet.msg_type != MSG_COMMAND_RESULT &&
         relay->pending.packet.msg_type != MSG_RESULT_BUNDLE) ||
        relay->pending.packet.dst_id != relay->gateway_id) {
        return PROTO_OK;
    }

    ret = gateway_collection_eack_from_tlvs(payload, payload_len, &eack);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = gateway_collection_eack_recovery_attempt_id(
        payload, payload_len, &recovery_attempt_id);
    if (ret == PROTO_OK) {
        uint64_t pending_gateway_id = 0u;
        uint16_t pending_gateway_epoch = 0u;
        uint32_t pending_command_seq = 0u;

        ret = gateway_collection_eack_recovery_identity(
            payload, payload_len, &recovery_identity);
        if (ret != PROTO_OK ||
            !semantic_digest_sha256(relay->pending.payload,
                                    relay->pending.payload_len,
                                    pending_payload_digest)) {
            return PROTO_ERR_MALFORMED;
        }
        if (relay->pending.packet.msg_type == MSG_COMMAND_RESULT) {
            ret = command_result_id_from_tlvs(relay->pending.payload,
                                              relay->pending.payload_len,
                                              &pending_id);
            if (ret != PROTO_OK) {
                return PROTO_ERR_MALFORMED;
            }
            ret = collection_epoch_id_from_payload(relay->pending.payload,
                                                   relay->pending.payload_len,
                                                   &collection_epoch_id);
            if (ret != PROTO_OK) {
                return PROTO_ERR_MALFORMED;
            }
            pending_gateway_id = pending_id.gateway_id;
            pending_gateway_epoch = pending_id.gateway_epoch;
            pending_command_seq = pending_id.command_seq;
        } else {
            struct result_bundle_header bundle = {0};

            ret = result_bundle_header_from_tlvs(relay->pending.payload,
                                                 relay->pending.payload_len,
                                                 &bundle);
            if (ret != PROTO_OK) {
                return PROTO_ERR_MALFORMED;
            }
            pending_gateway_id = bundle.gateway_id;
            pending_gateway_epoch = bundle.gateway_epoch;
            pending_command_seq = bundle.command_seq;
            collection_epoch_id = bundle.collection_epoch_id;
        }

        /*
         * A frozen same-boot collection-EACK receipt proves one immutable redriven
         * packet, not every sender sharing its collection. Match the complete
         * packet owner and payload identity before releasing custody. This also
         * gives bundled relay packets an exact terminal path after the volatile
         * transaction has disappeared at the gateway.
         */
        if (eack.gateway_id != pending_gateway_id ||
            eack.gateway_epoch != pending_gateway_epoch ||
            eack.command_seq != pending_command_seq ||
            eack.collection_epoch_id != collection_epoch_id ||
            recovery_identity.packet_src_id !=
                relay->pending.packet.src_id ||
            recovery_identity.packet_seq != relay->pending.packet.seq ||
            recovery_identity.payload_len != relay->pending.payload_len ||
            !semantic_digest_equal(recovery_identity.payload_digest,
                                   pending_payload_digest,
                                   sizeof(pending_payload_digest))) {
            return PROTO_OK;
        }

        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(
            relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }
    if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (relay->pending.packet.msg_type != MSG_COMMAND_RESULT) {
        return PROTO_OK;
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

    if (eack.eack_format != EACK_FORMAT_EXPLICIT_RECEIVED_LIST &&
        eack.eack_format != EACK_FORMAT_EXPLICIT_MISSING_LIST) {
        if (!eack.collection_open) {
            result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_CLOSED;
            outbox_record_mark_collection_closed(relay, &eack, now_ms);
            relay->pending.state = MESH_RELAY_TX_IDLE;
        }
        return PROTO_OK;
    }

    ret = gateway_collection_eack_contains_node_id(payload,
                                                   payload_len,
                                                   pending_id.node_id,
                                                   &listed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST && listed) ||
        (eack.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST && !listed)) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }

    if (!eack.collection_open) {
        result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_CLOSED;
        outbox_record_mark_collection_closed(relay, &eack, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
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
#endif

/*
 * Dedicated survey controls are gateway-owned protocol messages, not generic
 * application datagrams. Validate their complete immutable envelope before
 * duplicate history, local delivery, reverse-route learning, or forwarding
 * can consume the packet.
 */
static bool survey_gateway_control_semantics_valid(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct operation_policy_set policies;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t round_id = SURVEY_LEGACY_ROUND_ID;
    int ret;

    if (packet == NULL ||
        (packet->msg_type != MSG_SURVEY_DISCOVERY_START &&
         packet->msg_type != MSG_SURVEY_PAIR_PREPARE)) {
        return true;
    }
    if (relay == NULL || relay->role != MESH_RELAY_ROLE_ANCHOR ||
        !id_is_unicast(relay->gateway_id) ||
        packet->src_id != relay->gateway_id ||
        packet->flags != FLAG_DIAGNOSTIC ||
        packet->payload_len != payload_len ||
        payload == NULL ||
        packet->ttl == 0u || packet->ttl > SURVEY_DEFAULT_TTL ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        ((packet->ttl == SURVEY_DEFAULT_TTL) !=
         (previous_hop_id == relay->gateway_id))) {
        return false;
    }

    if (packet->msg_type == MSG_SURVEY_DISCOVERY_START) {
        struct survey_discovery_config config;
        struct survey_discovery_timing timing;

        if (packet->dst_id != MESH_BROADCAST_ID) {
            return false;
        }
        ret = survey_extract_discovery_start_tlvs(
            payload, payload_len, &config);
        if (ret != PROTO_OK || config.operation_generation == 0u ||
            packet->session_id !=
                survey_operation_session_id(config.operation_generation) ||
            operation_policy_set_from_tlvs(
                payload, payload_len, &policies) != PROTO_OK ||
            policies.assignment_present) {
            return false;
        }
        ret = survey_discovery_timing_from_age(
            &config, packet->message_age_ms, &timing);
        return ret == PROTO_OK && !timing.expired;
    }

    {
        struct survey_pair pair;

        if (!id_is_unicast(packet->dst_id) ||
            packet->dst_id == relay->gateway_id ||
            survey_extract_pair_tlvs(payload, payload_len, &pair) !=
                PROTO_OK ||
            pair.operation_generation == 0u ||
            packet->session_id !=
                survey_operation_session_id(pair.operation_generation) ||
            (packet->dst_id != pair.initiator_id &&
             packet->dst_id != pair.responder_id) ||
            operation_policy_set_from_tlvs(
                payload, payload_len, &policies) != PROTO_OK ||
            policies.assignment_present ||
            policies.discovery_present ||
            survey_round_id_extract_tlv(
                payload, payload_len, &round_id) != PROTO_OK ||
            round_id == SURVEY_LEGACY_ROUND_ID ||
            survey_round_commitment_extract_tlv(
                payload, payload_len, round_commitment) != PROTO_OK) {
            return false;
        }
    }
    return true;
}

static bool broadcast_packet_needs_forward(const struct mesh_relay *relay,
                                           const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    if (packet->dst_id != MESH_BROADCAST_ID || packet->ttl <= 1u) {
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
    out->packet.ttl = (uint8_t)(packet->ttl - 1u);
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
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
    out->earliest_tx_valid = true;
    return PROTO_OK;
}

static int add_gateway_ack_action(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    int ret;

    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return PROTO_OK;
    }

    ret = build_gateway_ack(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            &result->gateway_ack);
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
    case MSG_SELF_TEST_REPORT:
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
           tlv_find_unique(payload,
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
    struct mesh_ch9_batch_metadata batch_metadata;
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
    /*
     * Validate batch framing before allocating an ACK sequence. The history
     * store repeats this check as a fail-closed boundary, but a malformed
     * direct commit must not mutate even transport sequencing first.
     */
    ret = mesh_ch9_batch_metadata_parse(payload,
                                        payload_len,
                                        &batch_metadata);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }

    ret = build_gateway_ack(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            &result->gateway_ack);
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

int mesh_relay_commit_anchor_command_delivery(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    if (relay == NULL || packet == NULL ||
        (payload_len != 0u && payload == NULL) ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        packet->msg_type != MSG_COMMAND ||
        packet->src_id != relay->gateway_id ||
        packet->dst_id != relay->local_id ||
        packet->payload_len != payload_len ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }
    duplicate_store_payload_identity(
        relay, packet, payload, payload_len, now_ms);
    return PROTO_OK;
}

static void add_hop_ack_action(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint64_t previous_hop_id,
                               struct mesh_relay_result *result)
{
    int ret;

    ret = build_hop_ack(relay,
                        packet,
                        payload,
                        payload_len,
                        previous_hop_id,
                        &result->hop_ack);
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
            .capacity_hint_valid =
                capacity_validity_interval_ms != 0u,
            .valid = true,
        };

        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_OK;
        }
        if (rollback != NULL) {
            rollback->kind = REACTIVE_ROUTE_ROLLBACK_UPSTREAM;
            rollback->gateway_route_adv_seq =
                relay->gateway_route_adv_seq;
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
        relay->gateway_route_adv_seq =
            rollback->gateway_route_adv_seq;
    } else if (rollback->kind == REACTIVE_ROUTE_ROLLBACK_DOWNLINK &&
               rollback->state.downlink.valid &&
               rollback->state.downlink.index <
                   mesh_relay_downlink_capacity(relay)) {
        *downlink_at_mutable(relay, rollback->state.downlink.index) =
             rollback->state.downlink.previous;
    }
    rollback->kind = REACTIVE_ROUTE_ROLLBACK_NONE;
}
