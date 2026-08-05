#line 6112 __BASE_FILE__
static int outbox_snapshot_validate(const struct mesh_relay *relay,
                                    const struct mesh_relay_outbox_snapshot *snapshot)
{
    const struct persistent_outbox_record *record;
    const struct mesh_pending_tx *pending;
    struct command_result_id result_id;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t collection_epoch_id = 0u;

    if (relay == NULL || snapshot == NULL || !snapshot->valid ||
        snapshot->version != MESH_RELAY_OUTBOX_SNAPSHOT_VERSION) {
        return PROTO_ERR_ARG;
    }

    record = &snapshot->record;
    pending = &snapshot->pending;
    if (!relay_semantic_packet_digest(&pending->packet,
                                      pending->payload,
                                      pending->payload_len,
                                      semantic_digest)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!record->valid ||
        record->gateway_acked ||
        record->delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED ||
        record->delivery_state == MESH_RELAY_DELIVERY_EXPIRED ||
        record->delivery_state == MESH_RELAY_DELIVERY_COLLECTION_CLOSED ||
        record->delivery_state ==
            MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED ||
        record->expiry_s == 0u ||
        (record->age_ms_saturating / 1000u) >= record->expiry_s ||
        snapshot->role != relay->role ||
        snapshot->local_id != relay->local_id ||
        snapshot->gateway_id != relay->gateway_id ||
        record->gateway_id != relay->gateway_id ||
        snapshot->route_epoch != relay->upstream.current_epoch ||
        snapshot->route_epoch == 0u ||
        (uint16_t)snapshot->route_epoch == 0u ||
        pending->state == MESH_RELAY_TX_IDLE ||
        (pending->gateway_ack_forward_pending &&
         pending->packet.src_id == relay->local_id) ||
        pending->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        pending->packet.payload_len != pending->payload_len ||
        pending->packet.dst_id != relay->gateway_id ||
        (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        record->packet_id != outbox_packet_id_for(&pending->packet) ||
        record->session_id != pending->packet.session_id ||
        record->seq != pending->packet.seq ||
        record->packet_class != pending->packet.msg_type ||
        record->payload_len != pending->payload_len ||
        !semantic_digest_equal(record->semantic_digest,
                               semantic_digest,
                               sizeof(semantic_digest))) {
        return PROTO_ERR_MALFORMED;
    }
    if (pending->result_offer_active &&
        ((pending->state != MESH_RELAY_TX_WAIT_RESULT_GRANT &&
          pending->state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) ||
         record->delivery_state != MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK ||
         record->retry_round > MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS ||
         record->expiry_s > MESH_RELAY_RESULT_OFFER_EXPIRY_S)) {
        return PROTO_ERR_MALFORMED;
    }
    if (pending_is_result_bundle(relay, pending)) {
        return PROTO_OK;
    }
    if (pending_is_gateway_ack_confirm(relay, pending)) {
        return PROTO_OK;
    }
    if (pending_is_local_gateway_host_packet(relay, pending)) {
        return PROTO_OK;
    }
    if (pending_is_generic_transit_gateway_packet(relay, pending)) {
        return PROTO_OK;
    }
    if (!pending_has_valid_command_result_id(relay, pending) ||
        command_result_id_from_tlvs(pending->payload,
                                    pending->payload_len,
                                    &result_id) != PROTO_OK ||
        result_id.gateway_id != relay->gateway_id ||
        result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id.node_id != pending->packet.src_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (collection_epoch_id_from_payload(pending->payload,
                                         pending->payload_len,
                                         &collection_epoch_id) == PROTO_OK &&
        collection_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    return PROTO_OK;
}

static uint32_t outbox_snapshot_restore_retry_delay_ms(
    const struct mesh_relay_outbox_snapshot *snapshot)
{
    if (snapshot == NULL ||
        snapshot->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return RELAY_BUSY_RETRY_MIN_MS;
    }
    if (deadline_reached(snapshot->snapshot_at_ms, snapshot->pending.retry_after_ms)) {
        return 1u;
    }
    return snapshot->pending.retry_after_ms - snapshot->snapshot_at_ms;
}

int mesh_relay_export_outbox_snapshot(struct mesh_relay *relay,
                                      uint32_t now_ms,
                                      struct mesh_relay_outbox_snapshot *snapshot)
{
    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_relay_tx_active(relay) ||
        (!pending_has_valid_command_result_id(relay, &relay->pending) &&
         !pending_is_result_bundle(relay, &relay->pending) &&
         !pending_is_gateway_ack_confirm(relay, &relay->pending) &&
         !pending_is_local_gateway_host_packet(relay, &relay->pending) &&
         !pending_is_generic_transit_gateway_packet(relay, &relay->pending)) ||
        !relay->outbox_record.valid) {
        return PROTO_ERR_NOT_FOUND;
    }

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    snapshot->version = MESH_RELAY_OUTBOX_SNAPSHOT_VERSION;
    snapshot->role = relay->role;
    snapshot->local_id = relay->local_id;
    snapshot->gateway_id = relay->gateway_id;
    snapshot->record = relay->outbox_record;
    snapshot->pending = relay->pending;
    snapshot->route_epoch = relay->upstream.current_epoch;
    snapshot->snapshot_at_ms = now_ms;
    snapshot->valid = true;
    return PROTO_OK;
}

int mesh_relay_restore_outbox_snapshot(struct mesh_relay *relay,
                                       const struct mesh_relay_outbox_snapshot *snapshot,
                                       uint32_t now_ms)
{
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = outbox_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }

    relay->pending = snapshot->pending;
    relay->outbox_record = snapshot->record;
    relay->pending.packet.message_age_ms = relay->outbox_record.age_ms_saturating;
    relay->pending.queued_at_ms = now_ms;
    if (relay->pending.result_offer_active) {
        /*
         * Re-enter the normal missed-grant timeout path so the next RF offer
         * receives fresh caller randomness. The one-millisecond wake is a
         * non-transmitting service poll and does not consume an opportunity.
         */
        relay->pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
        relay->pending.retry_after_ms = 0u;
        relay->pending.gateway_ack_deadline_ms = now_ms + 1u;
    } else {
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms =
            now_ms + outbox_snapshot_restore_retry_delay_ms(snapshot);
        relay->pending.gateway_ack_deadline_ms = 0u;
    }
    relay->pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = 0u;
    relay->outbox_record.gateway_acked = false;
    relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
    relay->outbox_record.age_ms_saturating = relay->pending.packet.message_age_ms;
    return PROTO_OK;
}

static int child_custody_bundle_validate(
    const struct mesh_relay *relay,
    const struct mesh_result_bundle_queue *queue)
{
    if (relay == NULL || queue == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!queue->active) {
        return queue->record_count == 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
    }
    if (queue->gateway_id != relay->gateway_id ||
        queue->gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        queue->command_seq == 0u ||
        queue->collection_epoch_id == 0u ||
        queue->record_count == 0u ||
        queue->record_count > MESH_RELAY_RESULT_BUNDLE_RECORDS) {
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_RESULT_BUNDLE_RECORDS; i++) {
        const struct mesh_result_bundle_entry *entry = &queue->records[i];
        struct command_result_id result_id;
        uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
        uint32_t collection_epoch_id = 0u;

        if (i >= queue->record_count) {
            if (entry->valid) {
                return PROTO_ERR_MALFORMED;
            }
            continue;
        }
        if (!entry->valid ||
            entry->payload_len == 0u ||
            entry->payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN ||
            !semantic_digest_sha256(entry->payload,
                                    entry->payload_len,
                                    semantic_digest) ||
            !semantic_digest_equal(entry->semantic_digest,
                                   semantic_digest,
                                   sizeof(semantic_digest)) ||
            entry->payload_crc != proto_crc16_ccitt_false(entry->payload,
                                                          entry->payload_len) ||
            command_result_id_from_tlvs(entry->payload,
                                        entry->payload_len,
                                        &result_id) != PROTO_OK ||
            collection_epoch_id_from_payload(entry->payload,
                                             entry->payload_len,
                                             &collection_epoch_id) != PROTO_OK ||
            collection_epoch_id != queue->collection_epoch_id ||
            !command_result_id_matches(&result_id, &entry->result_id) ||
            entry->result_id.gateway_id != queue->gateway_id ||
            entry->result_id.gateway_epoch != queue->gateway_epoch ||
            entry->result_id.command_seq != queue->command_seq ||
            !id_is_unicast(entry->result_id.node_id)) {
            return PROTO_ERR_MALFORMED;
        }
    }

    return PROTO_OK;
}

static int child_custody_reservation_validate(
    const struct mesh_relay *relay,
    const struct mesh_result_offer_reservation *reservation)
{
    if (relay == NULL || reservation == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!reservation->valid) {
        return PROTO_OK;
    }
    if (!command_result_id_is_well_formed(&reservation->result_id) ||
        !id_is_unicast(reservation->child_id) ||
        reservation->result_len == 0u ||
        reservation->result_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        reservation->result_id.gateway_id != relay->gateway_id ||
        reservation->result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        reservation->result_id.node_id != reservation->child_id ||
        !id_is_unicast(reservation->result_id.node_id)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int child_custody_snapshot_validate(
    const struct mesh_relay *relay,
    const struct mesh_relay_child_custody_snapshot *snapshot)
{
    int ret;

    if (relay == NULL || snapshot == NULL || !snapshot->valid ||
        snapshot->version != MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION ||
        snapshot->role != relay->role ||
        snapshot->local_id != relay->local_id ||
        snapshot->gateway_id != relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    ret = child_custody_bundle_validate(relay, &snapshot->result_bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = child_custody_reservation_validate(relay,
                                             &snapshot->result_offer_reservation);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!snapshot->result_bundle.active &&
        !snapshot->result_offer_reservation.valid) {
        return PROTO_ERR_NOT_FOUND;
    }
    return PROTO_OK;
}

int mesh_relay_export_child_custody_snapshot(
    const struct mesh_relay *relay,
    uint32_t now_ms,
    struct mesh_relay_child_custody_snapshot *snapshot)
{
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_relay_result_bundle_pending(relay) &&
        !relay->result_offer_reservation.valid) {
        return PROTO_ERR_NOT_FOUND;
    }

    snapshot->version = MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION;
    snapshot->role = relay->role;
    snapshot->local_id = relay->local_id;
    snapshot->gateway_id = relay->gateway_id;
    snapshot->snapshot_at_ms = now_ms;
    snapshot->result_bundle = relay->result_bundle;
    snapshot->result_offer_reservation = relay->result_offer_reservation;
    snapshot->valid = true;

    ret = child_custody_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        return ret;
    }
    return PROTO_OK;
}

int mesh_relay_restore_child_custody_snapshot(
    struct mesh_relay *relay,
    const struct mesh_relay_child_custody_snapshot *snapshot,
    uint32_t now_ms)
{
    uint32_t exported_at_ms;
    uint32_t remaining_hold_ms = 1u;
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_result_bundle_pending(relay) ||
        relay->result_offer_reservation.valid) {
        return PROTO_ERR_MALFORMED;
    }

    ret = child_custody_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }

    exported_at_ms = snapshot->snapshot_at_ms;
    relay->result_bundle = snapshot->result_bundle;
    relay->result_offer_reservation = snapshot->result_offer_reservation;
    if (relay->result_offer_reservation.valid) {
        relay->result_offer_reservation_deadline_ms =
            now_ms + MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u;
    }

    if (relay->result_bundle.active) {
        if (!deadline_reached(exported_at_ms, relay->result_bundle.due_ms)) {
            remaining_hold_ms = relay->result_bundle.due_ms - exported_at_ms;
        }
        relay->result_bundle.due_ms = now_ms + remaining_hold_ms;
        for (uint8_t i = 0u; i < relay->result_bundle.record_count; i++) {
            struct mesh_result_bundle_entry *entry = &relay->result_bundle.records[i];
            uint32_t elapsed_ms = exported_at_ms - entry->queued_at_ms;

            entry->message_age_ms = packet_age_add(entry->message_age_ms, elapsed_ms);
            entry->queued_at_ms = now_ms;
        }
    }
    return PROTO_OK;
}

void mesh_relay_cancel_tx(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->pending, 0, sizeof(relay->pending));
        outbox_record_clear(relay);
    }
}

bool mesh_relay_defer_tx(struct mesh_relay *relay,
                         uint32_t now_ms,
                         uint32_t random_value)
{
    return preserve_pending_gateway_result(relay, now_ms, random_value);
}

bool mesh_relay_can_defer_tx(const struct mesh_relay *relay)
{
    return relay != NULL &&
           (pending_is_local_collection_result(relay, &relay->pending) ||
            pending_is_forwarded_command_result(relay, &relay->pending) ||
            pending_is_result_bundle(relay, &relay->pending) ||
            pending_is_gateway_ack_confirm(relay, &relay->pending) ||
            pending_is_local_gateway_host_packet(relay, &relay->pending) ||
            pending_is_generic_transit_gateway_packet(relay, &relay->pending));
}

static bool pending_matches_outbound(const struct mesh_relay *relay,
                                     const struct mesh_pending_tx *pending,
                                     const struct mesh_outbound *out)
{
    bool logical_packet_matches;

    if (relay == NULL || pending == NULL || out == NULL ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.session_id != out->packet.session_id ||
        pending->packet.seq != out->packet.seq) {
        return false;
    }

    logical_packet_matches =
        pending->packet.msg_type == out->packet.msg_type &&
        pending->packet.flags == out->packet.flags &&
        pending->packet.src_id == out->packet.src_id &&
        pending->packet.dst_id == out->packet.dst_id &&
        pending->packet.ttl == out->packet.ttl &&
        pending->packet.payload_len == pending->payload_len &&
        out->packet.payload_len == out->payload_len &&
        pending->payload_len == out->payload_len &&
        pending->next_hop_id == out->next_hop_id &&
        (pending->payload_len == 0u ||
         memcmp(pending->payload, out->payload, pending->payload_len) == 0);
    if (logical_packet_matches) {
        return true;
    }
    return pending->result_offer_active &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           out->packet.msg_type == MSG_RESULT_OFFER &&
           out->packet.src_id == relay->local_id &&
           out->packet.dst_id == pending->next_hop_id &&
           out->next_hop_id == pending->next_hop_id;
}

int mesh_relay_defer_pending_retry(struct mesh_relay *relay,
                                   uint32_t retry_at_ms)
{
    if (relay == NULL) {
        return PROTO_ERR_ARG;
    }
    if (relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        relay->pending.state != MESH_RELAY_TX_WAIT_RESULT_GRANT &&
        relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return PROTO_ERR_MALFORMED;
    }

    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms = result_offer_clip_retry_at_ms(relay,
                                                                 retry_at_ms);
    relay->pending.gateway_ack_deadline_ms = 0u;
    if (relay->outbox_record.valid) {
        relay->outbox_record.age_ms_saturating =
            relay->pending.packet.message_age_ms;
        relay->outbox_record.delivery_state =
            outbox_delivery_state_for(&relay->pending);
        relay->outbox_record.gateway_acked = false;
    }
    return PROTO_OK;
}

int mesh_relay_note_retransmit_deferred(struct mesh_relay *relay,
                                        const struct mesh_outbound *out,
                                        uint32_t retry_at_ms)
{
    if (relay == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!pending_matches_outbound(relay, &relay->pending, out) ||
        (relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
         relay->pending.state != MESH_RELAY_TX_WAIT_RESULT_GRANT &&
         relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF)) {
        return PROTO_ERR_MALFORMED;
    }
    return mesh_relay_defer_pending_retry(relay, retry_at_ms);
}

int mesh_relay_start_tx(struct mesh_relay *relay,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        !id_is_unicast(packet->src_id) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (relay->role == MESH_RELAY_ROLE_CLICKER &&
        (packet->msg_type != MSG_SELF_TEST_REPORT ||
         packet->src_id != relay->local_id ||
         packet->dst_id != relay->gateway_id)) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&relay->pending, 0, sizeof(relay->pending));
    relay->pending.packet = *packet;
    if (payload_len > 0u) {
        memcpy(relay->pending.payload, payload, payload_len);
    }
    relay->pending.payload_len = (uint16_t)payload_len;
    relay->pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.queued_at_ms = now_ms;
    relay->pending.state = outbox_should_track_pending(relay, &relay->pending) ?
                           MESH_RELAY_TX_WAIT_GATEWAY_ACK :
                           MESH_RELAY_TX_IDLE;
    pending_set_deadlines(&relay->pending, now_ms);
    outbox_record_track_pending(relay, now_ms);
    return outbound_from_pending(relay, &relay->pending, now_ms, out);
}

int mesh_relay_start_result_offer(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint32_t now_ms,
                                  struct mesh_outbound *out)
{
    struct command_result_id result_id;
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        payload_len <= COLLECTION_RESULT_INLINE_C5_MAX_BYTES ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        !id_is_unicast(packet->src_id) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id != relay->gateway_id ||
        relay->role == MESH_RELAY_ROLE_CLICKER) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, &result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!command_result_id_is_well_formed(&result_id) ||
        result_id.gateway_id != relay->gateway_id ||
        result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id.node_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&relay->pending, 0, sizeof(relay->pending));
    relay->pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
    relay->pending.packet = *packet;
    memcpy(relay->pending.payload, payload, payload_len);
    relay->pending.payload_len = (uint16_t)payload_len;
    relay->pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.queued_at_ms = now_ms;
    relay->pending.gateway_ack_deadline_ms = now_ms + RREP_ACK_TIMEOUT_MS;
    relay->pending.result_offer_active = true;
    outbox_record_track_pending(relay, now_ms);
    return outbound_from_pending(relay, &relay->pending, now_ms, out);
}

int mesh_relay_start_channel9_tx(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const struct mesh_channel5_requirements *requirements,
                                 uint32_t now_ms,
                                 struct mesh_event_plan *plan,
                                 struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!mesh_relay_packet_requires_channel9_payload_event(packet)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    (void)mesh_relay_expire_channel9_timings(relay, now_ms);
    ret = mesh_relay_require_channel9_tx_event(relay,
                                               next_hop_id,
                                               requirements,
                                               now_ms,
                                               plan);
    if (ret != PROTO_OK) {
        if (ret == PROTO_ERR_BUSY &&
            relay_channel9_plan_misses_event(plan->action)) {
            mesh_relay_note_channel9_missed(relay, next_hop_id, NULL);
        }
        return ret;
    }

    ret = mesh_relay_start_tx(relay,
                              packet,
                              payload,
                              payload_len,
                              now_ms,
                              out);
    if (ret == PROTO_OK) {
        out->radio_channel = MESH_EVENT_CHANNEL;
        if (mesh_relay_tx_active(relay) &&
            relay->pending.packet.session_id == packet->session_id &&
            relay->pending.packet.seq == packet->seq) {
            relay->pending.radio_channel = MESH_EVENT_CHANNEL;
        }
    }
    return ret;
}

void mesh_relay_note_channel9_success(struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_success(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_tx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_local_tx(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_rx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t planned_event_start_ms,
                                 uint32_t observed_packet_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_observed_packet(&relay->event_timings[index].timing,
                                        planned_event_start_ms,
                                        observed_packet_ms);
    }
}

void mesh_relay_note_channel9_missed(struct mesh_relay *relay,
                                     uint64_t next_hop_id,
                                     struct mesh_event_diagnostics *diagnostics)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_missed(&relay->event_timings[index].timing, diagnostics);
    }
}

void mesh_relay_note_tx_sent(struct mesh_relay *relay,
                             const struct mesh_outbound *out,
                             uint32_t now_ms)
{
    if (relay == NULL || out == NULL || !id_is_unicast(out->next_hop_id)) {
        return;
    }

    if (out->packet.msg_type == MSG_ROUTE_REPLY) {
        arm_route_reply_ack_expectation(relay, out);
    }

    if (pending_matches_outbound(relay, &relay->pending, out)) {
        if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK) {
            /*
             * A hop ACK is progress only once for each physical packet
             * transmission. Re-arm that progress gate only after the platform
             * confirms that this retry actually reached the radio.
             */
            relay->pending.hop_ack_observed_since_send = false;
            pending_set_deadlines(&relay->pending, now_ms);
        } else if (relay->pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT) {
            outbox_record_note_result_offer_sent(relay, now_ms);
            relay->pending.gateway_ack_deadline_ms = now_ms + RREP_ACK_TIMEOUT_MS;
        }
    }

    if (out->packet.dst_id == relay->gateway_id) {
        route_refresh_selected_at(&relay->upstream, now_ms);
    } else if (out->packet.dst_id != MESH_BROADCAST_ID) {
        refresh_downlink(relay, out->packet.dst_id, out->next_hop_id, now_ms);
    }
}

void mesh_relay_note_delivery_failure_at(struct mesh_relay *relay,
                                         uint64_t dst_id,
                                         uint32_t now_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(dst_id) || dst_id == relay->local_id) {
        return;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role != MESH_RELAY_ROLE_GATEWAY) {
            (void)route_record_failure_at(&relay->upstream,
                                          ROUTE_FAILURE_GATEWAY_ACK,
                                          now_ms);
        }
        return;
    }

    index = downlink_index(relay, dst_id);
    if (index >= 0) {
        struct mesh_downlink_entry *downlink =
            downlink_at_mutable(relay, (size_t)index);

        if (downlink->failure_count < UINT8_MAX) {
            downlink->failure_count++;
        }
        if (downlink->failure_count >= MESH_RELAY_DOWNLINK_MAX_FAILURES) {
            downlink->valid = false;
        }
    }
}

static int schedule_pending_parent_failure(struct mesh_relay *relay,
                                           uint32_t now_ms,
                                           uint32_t random_value,
                                           uint32_t *actions,
                                           int *status)
{
    enum route_delivery_action action;
    const struct route_candidate *selected;
    uint64_t failed_parent_id = 0u;
    uint32_t failed_route_epoch = 0u;
    uint64_t next_hop_id = 0u;
    uint32_t delay_ms;
    uint8_t failure_count;

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    selected = route_selected(&relay->upstream);
    if (selected != NULL) {
        failed_parent_id = selected->next_hop_id;
        failed_route_epoch = selected->route_epoch;
    }
    action = route_record_failure_at(&relay->upstream,
                                     ROUTE_FAILURE_GATEWAY_ACK,
                                     now_ms);
    if ((action == ROUTE_DELIVERY_TRY_ALTERNATE ||
         action == ROUTE_DELIVERY_DISCOVER) &&
        failed_parent_id != 0u &&
        id_is_unicast(failed_parent_id)) {
        invalidate_failed_upstream_dependents(relay,
                                              failed_parent_id,
                                              failed_route_epoch);
    }
    if (action == ROUTE_DELIVERY_DISCOVER ||
        mesh_relay_select_next_hop(relay,
                                   relay->pending.packet.dst_id,
                                   &next_hop_id) != PROTO_OK) {
        mesh_relay_invalidate_upstream_route(relay);
        if (preserve_local_pending_after_parent_loss(relay,
                                                     now_ms,
                                                     random_value)) {
            *actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
            *status = PROTO_ERR_NOT_FOUND;
            return PROTO_OK;
        }
        relay->pending.state = MESH_RELAY_TX_IDLE;
        outbox_record_clear(relay);
        *actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
        *status = PROTO_ERR_NOT_FOUND;
        return PROTO_OK;
    }

    selected = route_selected(&relay->upstream);
    if (action == ROUTE_DELIVERY_TRY_ALTERNATE) {
        /* Preserve the origin sender's immediate alternate-parent opportunity. */
        delay_ms = 0u;
    } else {
        failure_count = selected == NULL ? ROUTE_RETRIES_PER_CANDIDATE :
                        selected->failure_count;
        delay_ms = mesh_relay_retry_backoff_ms(failure_count, random_value);
    }
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.retry_after_ms = now_ms + delay_ms;
    relay->pending.gateway_ack_deadline_ms = 0u;
    return PROTO_OK;
}

int mesh_relay_note_pending_parent_failure(struct mesh_relay *relay,
                                           uint32_t now_ms,
                                           uint32_t random_value,
                                           struct mesh_relay_result *result)
{
    if (relay == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    return mesh_relay_note_pending_parent_failure_status(relay,
                                                         now_ms,
                                                         random_value,
                                                         &result->actions,
                                                         &result->status);
}

int mesh_relay_note_pending_parent_failure_status(struct mesh_relay *relay,
                                                  uint32_t now_ms,
                                                  uint32_t random_value,
                                                  uint32_t *actions,
                                                  int *status)
{
    if (relay == NULL || actions == NULL || status == NULL) {
        return PROTO_ERR_ARG;
    }

    *actions = MESH_RELAY_ACTION_NONE;
    *status = PROTO_OK;
    if (relay->pending.packet.dst_id != relay->gateway_id ||
        pending_is_local_collection_result(relay, &relay->pending) ||
        (relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
         relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF)) {
        return PROTO_ERR_MALFORMED;
    }

    return schedule_pending_parent_failure(relay,
                                           now_ms,
                                           random_value,
                                           actions,
                                           status);
}

int mesh_relay_tick_with_random(struct mesh_relay *relay,
                                uint32_t now_ms,
                                uint32_t random_value,
                                struct mesh_relay_result *result)
{
    uint64_t next_hop_id = 0u;
    uint32_t retry_delay_ms;
    int ret;

    if (relay == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    if (result_offer_reservation_expire(relay, now_ms)) {
        result->actions |= MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED;
    }
    (void)mesh_relay_expire_routes(relay, now_ms);
    if (!mesh_relay_tx_active(relay)) {
        if (result_bundle_ready_to_flush(&relay->result_bundle, now_ms)) {
            ret = result_bundle_build_outbound(relay, now_ms, &result->forward);
            if (ret == PROTO_OK) {
                result->actions |= MESH_RELAY_ACTION_FORWARD;
                return PROTO_OK;
            }
            relay->result_bundle.due_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
            result->status = ret;
            if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
                result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
            }
        }
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT) {
        int terminal_status =
            relay->outbox_record.delivery_state ==
                    MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED ?
                MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED :
            relay->pending.result_offer_active ?
                MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED :
                MESH_RELAY_ERR_OUTBOX_EXPIRED;

        result_offer_finish_terminal(
            relay,
            now_ms,
            relay->outbox_record.delivery_state,
            terminal_status,
            result);
        return PROTO_OK;
    }

    if (outbox_record_is_expired(relay, now_ms)) {
        if (relay->pending.result_offer_active) {
            result_offer_finish_terminal(
                relay,
                now_ms,
                MESH_RELAY_DELIVERY_EXPIRED,
                MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED,
                result);
            return PROTO_OK;
        }
        result_offer_finish_terminal(
            relay,
            now_ms,
            MESH_RELAY_DELIVERY_EXPIRED,
            MESH_RELAY_ERR_OUTBOX_EXPIRED,
            result);
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        if (!deadline_reached(now_ms, relay->pending.retry_after_ms)) {
            return PROTO_OK;
        }
        if (relay->pending.result_offer_active &&
            relay->outbox_record.retry_round >=
                MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS) {
            result_offer_finish_terminal(
                relay,
                now_ms,
                MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED,
                MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED,
                result);
            return PROTO_OK;
        }
        if (relay->pending.packet.dst_id == relay->gateway_id &&
            relay->outbox_record.valid) {
            pending_refresh_age(&relay->pending, now_ms);
            outbox_record_sync_age_from_pending(relay, now_ms);
            ret = mesh_relay_select_next_hop(relay,
                                             relay->pending.packet.dst_id,
                                             &next_hop_id);
            if (ret != PROTO_OK) {
                relay->pending.retry_after_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
                result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
                result->status = ret;
                return PROTO_OK;
            }
            relay->pending.next_hop_id = next_hop_id;
        }
        ret = outbound_from_pending(relay, &relay->pending, now_ms, &result->retransmit);
        if (ret != PROTO_OK) {
            result->status = ret;
            return ret;
        }
        relay->pending.packet.message_age_ms = result->retransmit.packet.message_age_ms;
        relay->pending.queued_at_ms = now_ms;
        outbox_record_sync_age_from_pending(relay, now_ms);
        if (relay->pending.result_offer_active) {
            relay->pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
            relay->pending.gateway_ack_deadline_ms = now_ms + RREP_ACK_TIMEOUT_MS;
        } else {
            relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
            pending_set_deadlines(&relay->pending, now_ms);
        }
        result->actions |= MESH_RELAY_ACTION_RETRANSMIT;
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
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
        ret = result_offer_retry_delay_ms(relay,
                                          random_value,
                                          &retry_delay_ms);
        if (ret != PROTO_OK) {
            result_offer_finish_terminal(relay,
                                         now_ms,
                                         MESH_RELAY_DELIVERY_EXPIRED,
                                         ret,
                                         result);
            return PROTO_OK;
        }
        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = result_offer_clip_retry_at_ms(
            relay,
            now_ms + retry_delay_ms);
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
        /*
         * The gateway already accepted this transit packet, so an expired
         * child-ACK handoff must retry the immutable original without
         * penalizing or invalidating the successful upstream parent.
         */
        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = now_ms +
            pending_busy_retry_delay_ms(relay,
                                        RELAY_BUSY_RETRY_MIN_MS,
                                        random_value);
        relay->pending.gateway_ack_deadline_ms = 0u;
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
        if (pending_is_local_collection_result(relay, &relay->pending)) {
            ret = schedule_pending_collection_retry(relay,
                                                    now_ms,
                                                    random_value,
                                                    result);
            if (ret != PROTO_OK) {
                result->status = ret;
                return ret;
            }
            return PROTO_OK;
        }

        return schedule_pending_parent_failure(relay,
                                               now_ms,
                                               random_value,
                                               &result->actions,
                                               &result->status);
    }

    return PROTO_OK;
}

int mesh_relay_tick(struct mesh_relay *relay,
                    uint32_t now_ms,
                    struct mesh_relay_result *result)
{
    return mesh_relay_tick_with_random(relay, now_ms, 0u, result);
}

int mesh_relay_handle_rx(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t previous_hop_id,
                         uint8_t link_quality,
                         uint32_t now_ms,
                         struct mesh_relay_result *result)
{
    return mesh_relay_handle_rx_with_random(relay,
                                            packet,
                                            payload,
                                            payload_len,
                                            previous_hop_id,
                                            link_quality,
                                            now_ms,
                                            0u,
                                            result);
}

int mesh_relay_handle_rx_with_random(struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t previous_hop_id,
                                     uint8_t link_quality,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_relay_result *result)
{
    enum duplicate_classification duplicate = DUPLICATE_CLASS_NEW;
    int ret;

    if (relay == NULL || packet == NULL || result == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(packet->src_id) ||
        packet->src_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    if (mesh_packet_rx_semantics_validate(packet,
                                          payload,
                                          payload_len,
                                          previous_hop_id,
                                          relay->local_id,
                                          relay->gateway_id) != PROTO_OK ||
        (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
         !collection_eack_rx_envelope_valid(relay,
                                             packet,
                                             payload,
                                             payload_len,
                                             previous_hop_id)) ||
        !survey_gateway_control_semantics_valid(relay,
                                                packet,
                                                payload,
                                                payload_len,
                                                previous_hop_id) ||
        (packet->msg_type == MSG_COMMAND &&
         !command_packet_envelope_valid(relay,
                                        packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id)) ||
        !gateway_bound_packet_flags_valid(relay, packet)) {
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }
    if (relay->role == MESH_RELAY_ROLE_CLICKER) {
        const struct route_candidate *parent =
            route_selected(&relay->upstream);
        uint8_t reply_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
        bool completed_route_reply_allowed =
            packet->msg_type == MSG_ROUTE_REPLY &&
            packet->src_id == relay->gateway_id &&
            relay->route_discovery.completed_request_id != 0u &&
            packet->session_id ==
                relay->route_discovery.completed_request_id &&
            previous_hop_id ==
                relay->route_discovery.completed_reply_previous_hop_id &&
            !deadline_reached(
                now_ms,
                relay->route_discovery.completed_request_until_ms) &&
            relay_semantic_packet_digest(packet,
                                         payload,
                                         payload_len,
                                         reply_digest) &&
            semantic_digest_equal(
                reply_digest,
                relay->route_discovery.completed_reply_digest,
                sizeof(reply_digest));
        bool route_reply_allowed =
            packet->msg_type == MSG_ROUTE_REPLY &&
            packet->src_id == relay->gateway_id &&
            ((relay->route_discovery.active &&
              relay->route_discovery.target_id == relay->gateway_id &&
              relay->route_discovery.current_request_id != 0u &&
              packet->session_id ==
                  relay->route_discovery.current_request_id) ||
             completed_route_reply_allowed);
        bool event_control =
            packet->msg_type == MSG_MESH_EVENT_PROPOSE ||
            packet->msg_type == MSG_MESH_EVENT_ACCEPT ||
            packet->msg_type == MSG_MESH_EVENT_UPDATE ||
            packet->msg_type == MSG_MESH_EVENT_END;
        bool event_control_allowed =
            event_control && parent != NULL &&
            previous_hop_id == parent->next_hop_id &&
            packet->src_id == previous_hop_id;
        bool pending_control =
            packet->msg_type == MSG_GATEWAY_ACK ||
            packet->msg_type == MSG_MESH_HOP_ACK ||
            packet->msg_type == MSG_RELAY_BUSY;

        if (packet->dst_id == relay->local_id &&
            (route_reply_allowed || event_control_allowed ||
             pending_control)) {
            goto clicker_leaf_rx_allowed;
        }
        /*
         * A clicker owns reliable uplinks but is never a relay or command
         * endpoint. Reject every broadcast, transit packet, route request,
         * result offer, and directed application packet before duplicate,
         * route, custody, or delivery state can change.
         */
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }
clicker_leaf_rx_allowed:
    if (result_offer_reservation_expire(relay, now_ms)) {
        result->actions |= MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED;
    }
    (void)mesh_relay_expire_routes(relay, now_ms);
    (void)mesh_relay_expire_channel9_timings(relay, now_ms);

    if (packet->msg_type == MSG_COMMAND &&
        packet->dst_id == MESH_BROADCAST_ID) {
        enum broadcast_command_replay_classification replay;
        uint32_t command_seq;

        if (!command_flood_broadcast_valid(payload, payload_len)) {
            result->status = PROTO_ERR_MALFORMED;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        duplicate = duplicate_classify(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       now_ms);
        if (duplicate == DUPLICATE_CLASS_CONFLICT) {
            result->status = PROTO_ERR_MALFORMED;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        command_seq = duplicate_session_identity(packet, payload, payload_len);
        replay = broadcast_command_replay_classify(&relay->command_replay,
                                                   command_seq,
                                                   packet,
                                                   payload,
                                                   payload_len);
        if (replay == BROADCAST_COMMAND_REPLAY_CONFLICT) {
            result->status = PROTO_ERR_MALFORMED;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        if (replay == BROADCAST_COMMAND_REPLAY_EXACT) {
            /*
             * Forwarding is transport-committed on the first valid copy, but
             * the anchor application may still be waiting for result or
             * execution capacity. Redeliver only the exact current attempt
             * locally without reflooding; the application replay window makes
             * an accepted command inert.
             */
            if (relay->role == MESH_RELAY_ROLE_ANCHOR) {
                result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
                result->status = PROTO_ERR_STALE;
                return PROTO_OK;
            }
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (replay == BROADCAST_COMMAND_REPLAY_STALE) {
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
    }

    if ((packet->msg_type == MSG_GATEWAY_ACK ||
         packet->msg_type == MSG_MESH_HOP_ACK) &&
        (packet->dst_id == relay->local_id ||
         gateway_ack_targets_pending_transit_origin(relay, packet))) {
        (void)handle_local_ack(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               now_ms,
                               result);
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REPLY_ACK) {
        ret = handle_route_reply_ack(relay,
                                     packet,
                                     payload,
                                     payload_len,
                                     previous_hop_id,
                                     result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if ((packet->msg_type == MSG_RELAY_BUSY || packet->msg_type == MSG_RESULT_BUSY) &&
        packet->dst_id == relay->local_id) {
        ret = handle_local_busy(relay,
                                packet,
                                payload,
                                payload_len,
                                previous_hop_id,
                                now_ms,
                                random_value,
                                result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_RESULT_OFFER && packet->dst_id == relay->local_id) {
        ret = handle_result_offer(relay,
                                  packet,
                                  payload,
                                  payload_len,
                                  previous_hop_id,
                                  now_ms,
                                  result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_RESULT_GRANT && packet->dst_id == relay->local_id) {
        ret = handle_result_grant(relay,
                                  packet,
                                  payload,
                                  payload_len,
                                  previous_hop_id,
                                  now_ms,
                                  random_value,
                                  result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if (packet->msg_type != MSG_COMMAND ||
        packet->dst_id != MESH_BROADCAST_ID) {
        duplicate = duplicate_classify(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       now_ms);
    }
    if (duplicate == DUPLICATE_CLASS_CONFLICT) {
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }
    if (duplicate == DUPLICATE_CLASS_EXACT) {
        if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {
            add_route_reply_ack_action(relay, packet, payload, payload_len, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (packet->dst_id == relay->local_id) {
            if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
                packet->msg_type == MSG_SURVEY_PAIR_PREPARE) {
                /*
                 * Relay acceptance precedes the application's durable
                 * command-result admission.  Redeliver an exact retry so a
                 * transient result-capacity failure can make progress.  The
                 * pair lease and command-result owners make an already
                 * accepted prepare idempotent; never re-ACK or reforward it
                 * from this final endpoint.
                 */
                result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
                result->status = PROTO_ERR_STALE;
                return PROTO_OK;
            }
            if (gateway_delivery_requires_commit(relay, packet) &&
                gateway_collection_duplicate_requires_redelivery(packet,
                                                                 payload,
                                                                 payload_len)) {
                result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
                return PROTO_OK;
            }
            (void)add_gateway_ack_action(relay,
                                         packet,
                                         payload,
                                         payload_len,
                                         previous_hop_id,
                                         result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (packet->msg_type == MSG_ROUTE_REQ &&
            packet->dst_id == MESH_BROADCAST_ID) {
            ret = handle_route_request(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       link_quality,
                                       now_ms,
                                       random_value,
                                       result,
                                       true);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
            }
            return PROTO_OK;
        }
        if (packet->dst_id == MESH_BROADCAST_ID) {
            if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
                packet->msg_type == MSG_SURVEY_DISCOVERY_START) {
                /*
                 * The first copy may have committed flood suppression before
                 * the local scheduler could own the discovery.  Redeliver an
                 * exact retry locally, while suppressing another flood.  An
                 * active generation is absorbed by application admission.
                 */
                result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
                result->status = PROTO_ERR_STALE;
                return PROTO_OK;
            }
            relay_diag_inc_u8(&relay->diagnostics.flood_suppression_count);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        if (mesh_relay_tx_active(relay) &&
            !gateway_targeted_control_can_forward_independently(
                relay, packet, previous_hop_id)) {
            bool same_pending_identity =
                relay->pending.packet.msg_type == packet->msg_type &&
                relay->pending.packet.src_id == packet->src_id &&
                relay->pending.packet.dst_id == packet->dst_id &&
                relay->pending.packet.session_id == packet->session_id &&
                relay->pending.packet.seq == packet->seq;

            /* A retry of the exact packet this relay already owns is a
             * progress probe, not a competing transaction.  Identity alone
             * is insufficient: a same-header packet with different bytes
             * must never replace or extend custody. */
            if (same_pending_identity) {
                bool same_pending_payload =
                    relay->pending.packet.payload_len ==
                        relay->pending.payload_len &&
                    relay->pending.payload_len == payload_len &&
                    packet->payload_len == payload_len &&
                    (payload_len == 0u ||
                     memcmp(relay->pending.payload, payload, payload_len) == 0);

                if (!same_pending_payload) {
                    result->actions |= MESH_RELAY_ACTION_DROP;
                    result->status = PROTO_ERR_MALFORMED;
                    return PROTO_OK;
                }

                /* Re-forward immutable custody bytes.  In particular, do
                 * not build from the received retry, whose payload may have
                 * been altered after duplicate identity matched. */
                ret = outbound_from_pending(relay,
                                            &relay->pending,
                                            now_ms,
                                            &result->forward);
                if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
                    result->status = ret;
                    result->actions |= MESH_RELAY_ACTION_DROP;
                    return PROTO_OK;
                }
                if (ret == PROTO_OK) {
                    result->actions |= MESH_RELAY_ACTION_FORWARD;
                }
                add_hop_ack_action(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   result);
                result->status = PROTO_ERR_STALE;
                return PROTO_OK;
            }
            if (duplicate_busy_response_due(relay, packet, payload,
                                            payload_len, now_ms)) {
                add_busy_action(relay, packet, payload, payload_len,
                                previous_hop_id, result);
            }
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            return PROTO_OK;
        }

        ret = build_forward(relay, packet, payload, payload_len, &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
            add_hop_ack_action(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               result);
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        result->actions |= MESH_RELAY_ACTION_DROP;
        result->status = ret;
        return PROTO_OK;
    }

    if (packet->msg_type != MSG_ROUTE_REQ &&
        packet->msg_type != MSG_ROUTE_REPLY &&
        (packet_needs_forward(relay, packet) || local_delivery_needs_response(relay, packet)) &&
        !gateway_delivery_requires_commit(relay, packet) &&
        !gateway_targeted_control_can_forward_independently(
            relay, packet, previous_hop_id) &&
        mesh_relay_tx_active(relay)) {
        if (duplicate_busy_response_due(relay, packet, payload,
                                        payload_len, now_ms)) {
            add_busy_action(relay, packet, payload, payload_len,
                            previous_hop_id, result);
        }
        result->status = PROTO_ERR_BUSY;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == LEGACY_MSG_ROUTE_STATUS) {
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REQ && packet->dst_id == MESH_BROADCAST_ID) {
        ret = handle_route_request(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   link_quality,
                                   now_ms,
                                   random_value,
                                   result,
                                   false);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, payload, payload_len, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {
        ret = handle_route_reply(relay,
                                 packet,
                                 payload,
                                 payload_len,
                                 previous_hop_id,
                                 link_quality,
                                 now_ms,
                                 result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else if (packet->dst_id == relay->local_id) {
            /*
             * A final destination owns the accepted reply and can make exact
             * duplicate ACKs sticky.  An intermediate does not commit the
             * duplicate here: if its downstream handoff later fails, an exact
             * upstream retry must rebuild and retry that handoff rather than
             * being ACKed as already consumed.
             */
            duplicate_store(relay, packet, payload, payload_len, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV && packet->dst_id == MESH_BROADCAST_ID) {
        ret = handle_gateway_route_adv(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       link_quality,
                                       now_ms,
                                       random_value,
                                       result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, payload, payload_len, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
        packet->dst_id == MESH_BROADCAST_ID) {
        if (!collection_eack_broadcast_valid(relay, packet, payload, payload_len)) {
            result->status = PROTO_ERR_MALFORMED;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        ret = handle_collection_eack_for_pending(relay,
                                                 payload,
                                                 payload_len,
                                                 now_ms,
                                                 random_value,
                                                 result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
    }

    if (packet->dst_id == relay->local_id) {
        if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
            packet->msg_type == MSG_COMMAND) {
            /*
             * The application still has to reserve result custody before it
             * mutates state. Do not commit duplicate suppression here; an
             * admission failure must leave the exact command retryable.
             */
            result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
            return PROTO_OK;
        }
        if (gateway_delivery_requires_commit(relay, packet)) {
            ret = gateway_ack_history_can_accept(relay,
                                                 packet,
                                                 payload,
                                                 payload_len,
                                                 now_ms);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
                return PROTO_OK;
            }
            result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
            return PROTO_OK;
        }
        ret = gateway_ack_history_accept_generic(relay,
                                                 packet,
                                                 payload,
                                                 payload_len,
                                                 now_ms);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        duplicate_store(relay, packet, payload, payload_len, now_ms);
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        (void)add_gateway_ack_action(relay,
                                     packet,
                                     payload,
                                     payload_len,
                                     previous_hop_id,
                                     result);
        return PROTO_OK;
    }

    if (packet->dst_id == MESH_BROADCAST_ID) {
        if (packet->msg_type == MSG_COMMAND) {
            ret = broadcast_command_replay_store(
                    &relay->command_replay,
                    duplicate_session_identity(packet, payload, payload_len),
                    packet,
                    payload,
                    payload_len);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
                return PROTO_OK;
            }
        } else {
            duplicate_store(relay, packet, payload, payload_len, now_ms);
        }
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        ret = build_broadcast_forward(relay,
                                      packet,
                                      payload,
                                      payload_len,
                                      now_ms,
                                      random_value,
                                      &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_COMMAND_RESULT) {
        struct command_result_id result_id;
        uint32_t collection_epoch_id = 0u;
        bool reservation_applies =
            relay->result_offer_reservation.valid &&
            relay->result_offer_reservation.child_id == packet->src_id;

        ret = result_offer_reservation_matches_payload(relay, packet, payload, payload_len);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        if (command_result_can_bundle(relay,
                                      packet,
                                      payload,
                                      payload_len,
                                      &result_id,
                                      &collection_epoch_id)) {
            ret = result_bundle_enqueue_command_result(relay,
                                                       packet,
                                                       payload,
                                                       payload_len,
                                                       &result_id,
                                                       collection_epoch_id,
                                                       now_ms);
            if (ret == PROTO_OK || ret == PROTO_ERR_STALE) {
                duplicate_store(relay, packet, payload, payload_len, now_ms);
                if (reservation_applies) {
                    result_offer_reservation_clear(relay);
                }
                add_hop_ack_action(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   result);
                result->actions |= MESH_RELAY_ACTION_CUSTODY_ACCEPTED;
                result->status = ret;
                if (result_bundle_ready_to_flush(&relay->result_bundle, now_ms)) {
                    ret = result_bundle_build_outbound(relay, now_ms, &result->forward);
                    if (ret == PROTO_OK) {
                        result->actions |= MESH_RELAY_ACTION_FORWARD;
                        result->status = PROTO_OK;
                    } else {
                        result->status = ret;
                    }
                }
                return PROTO_OK;
            }
            if (ret == PROTO_ERR_MALFORMED) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
                return PROTO_OK;
            }
            if (ret == PROTO_ERR_BUSY ||
                ret == PROTO_ERR_NO_SPACE ||
                ret == PROTO_ERR_NOT_FOUND) {
                /*
                 * A collection result cannot fall through to the ordinary
                 * single-packet forward path after bundle admission failed.
                 * That path installs WAIT_COLLECTION_EACK at a transit relay,
                 * while ordinary EACK ownership is local to the producer, so
                 * the relay would retain the child record forever after
                 * already returning a hop ACK. Withhold custody instead and
                 * return a correlated one-hop RESULT_BUSY; the child keeps
                 * its exact durable result and retries this or an alternate
                 * parent.
                 */
                add_busy_action(relay,
                                packet,
                                payload,
                                payload_len,
                                previous_hop_id,
                                result);
                result->status = PROTO_ERR_BUSY;
                result->actions |= MESH_RELAY_ACTION_DROP;
                if (reservation_applies) {
                    result_offer_reservation_clear(relay);
                    result->actions |=
                        MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED;
                }
                return PROTO_OK;
            }
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
    }

    ret = build_forward(relay, packet, payload, payload_len, &result->forward);
    if (ret == PROTO_OK) {
        duplicate_store(relay, packet, payload, payload_len, now_ms);
        if (relay->result_offer_reservation.valid &&
            relay->result_offer_reservation.child_id == packet->src_id) {
            result_offer_reservation_clear(relay);
        }
        result->actions |= MESH_RELAY_ACTION_FORWARD;
        add_hop_ack_action(relay,
                           packet,
                           payload,
                           payload_len,
                           previous_hop_id,
                           result);
        return PROTO_OK;
    }

    result->status = ret;
    result->actions |= MESH_RELAY_ACTION_DROP;
    return PROTO_OK;
}
