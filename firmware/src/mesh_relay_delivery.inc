#line 6112 __BASE_FILE__
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
static int outbox_snapshot_validate(const struct mesh_relay *relay,
                                    const struct mesh_relay_outbox_snapshot *snapshot)
{
    const struct mesh_outbox_record *record;
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
    if (!relay_semantic_packet_digest(relay,
                                      &pending->packet,
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
#endif

int mesh_relay_export_outbox_snapshot(struct mesh_relay *relay,
                                      uint32_t now_ms,
                                      struct mesh_relay_outbox_snapshot *snapshot)
{
    /* Serialization-neutral RAM-model helper. Production does not write this
     * snapshot to storage or promise restoration across reset. */
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
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    /* A gateway never owns upstream outbox custody, so accepting a restored
     * pending record would invalidate the specialization's idle invariant. */
    (void)now_ms;
    return PROTO_ERR_MALFORMED;
#else
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
    } else if (relay->pending.gateway_ack_confirm_pending ||
               relay->pending.gateway_ack_recovery_flags != 0u) {
        /* A source reset cannot prove which gateway incarnation saw the
         * transient confirm. Replay the retained original only after a fresh
         * same-wire ACK quarantine instead of restoring ambiguous proof. */
        relay->pending.gateway_ack_confirm_pending = false;
        relay->pending.gateway_ack_recovery_flags =
            GATEWAY_ACK_RECOVERY_AUTHORIZED |
            GATEWAY_ACK_RECOVERY_QUARANTINE;
        relay->pending.gateway_ack_confirm_started_ms = 0u;
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms =
            now_ms + MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS;
        relay->pending.gateway_ack_deadline_ms = 0u;
    } else {
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms =
            now_ms + outbox_snapshot_restore_retry_delay_ms(snapshot);
        relay->pending.gateway_ack_deadline_ms = 0u;
    }
    relay->pending.radio_channel =
        mesh_relay_packet_requires_channel9_payload_event(
            &relay->pending.packet) ?
            UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = 0u;
    relay->outbox_record.gateway_acked = false;
    relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
    relay->outbox_record.age_ms_saturating = relay->pending.packet.message_age_ms;
    return PROTO_OK;
#endif
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
    /* Serialization-neutral RAM-model helper. Production child custody is
     * volatile and an all-node reboot requires the operation to be rerun. */
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
    if (relay == NULL || !mesh_relay_can_defer_tx(relay) ||
        pending_expired_for_deferral(relay, now_ms)) {
        return false;
    }
    /* A preemption cannot make an already-paused owner more deferred. */
    if (relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return true;
    }
    return preserve_pending_gateway_result(relay, now_ms, random_value);
}

bool mesh_relay_can_defer_tx(const struct mesh_relay *relay)
{
    return relay != NULL &&
           pending_state_retryable_for_deferral(&relay->pending) &&
           (pending_is_local_collection_result(relay, &relay->pending) ||
            pending_is_forwarded_command_result(relay, &relay->pending) ||
            pending_is_result_bundle(relay, &relay->pending) ||
            pending_is_gateway_ack_confirm(relay, &relay->pending) ||
            pending_is_local_gateway_host_packet(relay, &relay->pending) ||
            pending_is_generic_transit_gateway_packet(relay, &relay->pending));
}

static bool pending_original_exactly_matches_outbound(
    const struct mesh_pending_tx *pending,
    const struct mesh_outbound *out)
{
    if (pending == NULL || out == NULL ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.session_id != out->packet.session_id ||
        pending->packet.seq != out->packet.seq) {
        return false;
    }

    return pending->packet.msg_type == out->packet.msg_type &&
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
}

static bool pending_transient_confirm_matches_outbound(
    const struct mesh_relay *relay,
    const struct mesh_pending_tx *pending,
    const struct mesh_outbound *out)
{
    struct proto_packet confirm_packet;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t confirm_payload_len = 0u;

    if (relay == NULL || pending == NULL || out == NULL ||
        !pending->gateway_ack_confirm_pending ||
        mesh_relay_pending_gateway_ack_confirm_wire(
            relay,
            out->queued_at_ms,
            &confirm_packet,
            confirm_payload,
            sizeof(confirm_payload),
            &confirm_payload_len) != PROTO_OK) {
        return false;
    }
    return confirm_packet.msg_type == out->packet.msg_type &&
           confirm_packet.flags == out->packet.flags &&
           confirm_packet.src_id == out->packet.src_id &&
           confirm_packet.dst_id == out->packet.dst_id &&
           confirm_packet.session_id == out->packet.session_id &&
           confirm_packet.seq == out->packet.seq &&
           confirm_packet.payload_len == out->packet.payload_len &&
           confirm_payload_len == out->payload_len &&
           pending->next_hop_id == out->next_hop_id &&
           semantic_digest_equal(confirm_payload,
                                 out->payload,
                                 confirm_payload_len);
}

static bool pending_matches_outbound(const struct mesh_relay *relay,
                                     const struct mesh_pending_tx *pending,
                                     const struct mesh_outbound *out)
{
    if (relay == NULL || pending == NULL || out == NULL ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.session_id != out->packet.session_id ||
        pending->packet.seq != out->packet.seq) {
        return false;
    }
    if (pending_transient_confirm_matches_outbound(relay, pending, out)) {
        return true;
    }
    if (pending_original_exactly_matches_outbound(pending, out)) {
        return true;
    }
    return pending->result_offer_active &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           out->packet.msg_type == MSG_RESULT_OFFER &&
           out->packet.src_id == relay->local_id &&
           out->packet.dst_id == pending->next_hop_id &&
           out->next_hop_id == pending->next_hop_id;
}

int mesh_relay_cancel_tx_if_matches(struct mesh_relay *relay,
                                    const struct mesh_outbound *out)
{
    if (relay == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!pending_matches_outbound(relay, &relay->pending, out)) {
        return PROTO_ERR_NOT_FOUND;
    }
    mesh_relay_cancel_tx(relay);
    return PROTO_OK;
}

int mesh_relay_bind_transit_previous_hop(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint64_t previous_hop_id)
{
    if (relay == NULL || out == NULL ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        out->packet.src_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (!pending_matches_outbound(relay, &relay->pending, out)) {
        return PROTO_ERR_NOT_FOUND;
    }

    /*
     * The retained original and its gateway ACK form one custody chain.
     * Bind the physical child before RF starts; route discovery and later
     * downlink improvements cannot redirect the ACK away from that owner.
     */
    relay->pending.transit_previous_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int commit_retained_transit_reverse_route(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint64_t previous_hop_id,
    uint32_t now_ms)
{
    struct mesh_downlink_entry entry;
    int keep_index;
    int ret;

    if (relay == NULL || out == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        relay->local_id == relay->gateway_id ||
        out->packet.dst_id != relay->gateway_id ||
        out->packet.src_id == relay->gateway_id ||
        (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        relay->upstream.current_epoch == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        previous_hop_id == relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    entry = (struct mesh_downlink_entry) {
        .target_id = out->packet.src_id,
        .next_hop_id = previous_hop_id,
        .gateway_id = relay->gateway_id,
        .route_epoch = relay->upstream.current_epoch,
        .last_seen_ms = now_ms,
        .hop_count = out->packet.src_id == previous_hop_id ?
                     1u : MESH_NETWORK_MAX_HOPS,
        .quality = 0u,
        .valid = true,
    };
    ret = upsert_required_gateway_downlink(relay, &entry);
    if (ret != PROTO_OK) {
        return ret;
    }
    keep_index = downlink_exact_index(relay, &entry);
    if (keep_index < 0) {
        return PROTO_ERR_NOT_FOUND;
    }

    /* There is one authoritative same-boot ingress edge per descendant.
     * The source has already completed route discovery and transferred exact
     * packet custody, so its older physical hints must not redirect a later
     * targeted control around the selected parent. */
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        struct mesh_downlink_entry *candidate =
            downlink_at_mutable(relay, i);

        if ((int)i != keep_index && candidate->valid &&
            candidate->target_id == entry.target_id &&
            candidate->gateway_id == entry.gateway_id) {
            memset(candidate, 0, sizeof(*candidate));
        }
    }
    return PROTO_OK;
}

static bool transit_reverse_route_outbound_valid(
    const struct mesh_relay *relay,
    const struct mesh_outbound *out)
{
    return relay != NULL && out != NULL &&
           relay->role == MESH_RELAY_ROLE_ANCHOR &&
           relay->local_id != relay->gateway_id &&
           out->packet.dst_id == relay->gateway_id &&
           out->packet.src_id != relay->gateway_id &&
           (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           relay->upstream.current_epoch != 0u;
}

int mesh_relay_commit_transit_reverse_route(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint32_t now_ms)
{
    if (!transit_reverse_route_outbound_valid(relay, out)) {
        return PROTO_ERR_ARG;
    }
    if (!pending_matches_outbound(relay, &relay->pending, out)) {
        return PROTO_ERR_NOT_FOUND;
    }
    return commit_retained_transit_reverse_route(
        relay, out, relay->pending.transit_previous_hop_id, now_ms);
}

int mesh_relay_commit_queued_transit_reverse_route(
    struct mesh_relay *relay,
    const struct mesh_outbound *out,
    uint64_t previous_hop_id,
    uint32_t now_ms)
{
    return commit_retained_transit_reverse_route(
        relay, out, previous_hop_id, now_ms);
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
    ret = mesh_relay_select_next_hop_for_packet(relay,
                                                packet,
                                                payload,
                                                payload_len,
                                                now_ms,
                                                &next_hop_id);
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
    ret = mesh_relay_select_next_hop_for_packet(relay,
                                                packet,
                                                payload,
                                                payload_len,
                                                now_ms,
                                                &next_hop_id);
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

int mesh_relay_retain_channel9_tx_wait(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint32_t now_ms,
                                       uint32_t retry_at_ms,
                                       struct mesh_outbound *out)
{
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        !mesh_relay_packet_requires_channel9_payload_event(packet)) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_relay_start_tx(relay,
                              packet,
                              payload,
                              payload_len,
                              now_ms,
                              out);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!mesh_relay_tx_active(relay)) {
        mesh_relay_cancel_tx(relay);
        return PROTO_ERR_MALFORMED;
    }

    out->radio_channel = MESH_EVENT_CHANNEL;
    relay->pending.radio_channel = MESH_EVENT_CHANNEL;
    ret = mesh_relay_note_retransmit_deferred(relay, out, retry_at_ms);
    if (ret != PROTO_OK) {
        (void)mesh_relay_cancel_tx_if_matches(relay, out);
    }
    return ret;
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

    ret = mesh_relay_select_next_hop_for_packet(relay,
                                                packet,
                                                payload,
                                                payload_len,
                                                now_ms,
                                                &next_hop_id);
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
        /*
         * A TX plan may intentionally target the local-TX event following a
         * retained peer-RX event. Advance any fully elapsed intervening
         * events before committing success so the alternating owner parity
         * remains aligned with the plan that was actually executed.
         */
        (void)mesh_event_skip_elapsed(
            &relay->event_timings[index].timing, event_start_ms, NULL);
        mesh_event_note_success(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_unobserved_turn(struct mesh_relay *relay,
                                              uint64_t next_hop_id,
                                              uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = relay_channel9_timing_index(relay, next_hop_id);
    if (index >= 0) {
        (void)mesh_event_skip_elapsed(
            &relay->event_timings[index].timing, event_start_ms, NULL);
        mesh_event_note_unobserved_turn(
            &relay->event_timings[index].timing, event_start_ms);
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
    bool recovery_original_sent;

    if (relay == NULL || out == NULL || !id_is_unicast(out->next_hop_id)) {
        return;
    }

    if (out->packet.msg_type == MSG_ROUTE_REPLY) {
        arm_route_reply_ack_expectation(relay, out);
    }

    recovery_original_sent =
        (relay->pending.gateway_ack_recovery_flags &
         GATEWAY_ACK_RECOVERY_QUARANTINE) != 0u &&
        pending_original_exactly_matches_outbound(&relay->pending, out);
    if (pending_matches_outbound(relay, &relay->pending, out)) {
        if (recovery_original_sent) {
            /* Only a proven physical replay opens the ACK gate. A queued or
             * failed replay leaves delayed pre-reset ACKs quarantined. */
            relay->pending.gateway_ack_recovery_flags &=
                (uint8_t)~GATEWAY_ACK_RECOVERY_QUARANTINE;
        }
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
        (void)route_refresh_candidate_at(&relay->upstream,
                                         out->next_hop_id,
                                         relay->gateway_id,
                                         now_ms);
    } else if (out->packet.dst_id != MESH_BROADCAST_ID) {
        refresh_downlink(relay, out->packet.dst_id, out->next_hop_id, now_ms);
    }
}

int mesh_relay_note_gateway_ack_window_elapsed(struct mesh_relay *relay,
                                               const struct mesh_outbound *sent,
                                               uint32_t deadline_ms)
{
    if (relay == NULL || sent == NULL) {
        return PROTO_ERR_ARG;
    }
    if (relay->pending.state != MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
        relay->pending.result_offer_active ||
        relay->pending.gateway_ack_confirm_pending) {
        return PROTO_ERR_MALFORMED;
    }
    if (!pending_matches_outbound(relay, &relay->pending, sent)) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->pending.hop_ack_observed_since_send) {
        /*
         * First-hop custody already advanced this window to the multi-hop
         * gateway-ACK return budget. A short local receive turn proves
         * nothing about that longer path, so never shorten it here.
         */
        return PROTO_ERR_STALE;
    }
    if (!deadline_reached(deadline_ms,
                          relay->pending.gateway_ack_deadline_ms)) {
        relay->pending.gateway_ack_deadline_ms = deadline_ms;
    }
    return PROTO_OK;
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
    const bool bounded_report_hop_retry =
        packet_uses_report_hop_custody(relay, &relay->pending.packet) &&
        id_is_unicast(relay->pending.next_hop_id) &&
        relay->pending.next_hop_id != relay->gateway_id;

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    selected = route_selected(&relay->upstream);
    if (bounded_report_hop_retry) {
        failed_parent_id = relay->pending.next_hop_id;
        failed_route_epoch = relay->upstream.current_epoch;
    } else if (selected != NULL) {
        failed_parent_id = selected->next_hop_id;
        failed_route_epoch = selected->route_epoch;
    }
    if (bounded_report_hop_retry &&
        relay->pending.parent_hop_ack_miss_count <
            ROUTE_RETRIES_PER_CANDIDATE) {
        relay->pending.parent_hop_ack_miss_count++;
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.next_hop_id = failed_parent_id;
        relay->pending.retry_after_ms = now_ms +
            mesh_relay_retry_backoff_ms(
                relay->pending.parent_hop_ack_miss_count,
                random_value);
        relay->pending.gateway_ack_deadline_ms = 0u;
        return PROTO_OK;
    }
    if (bounded_report_hop_retry) {
        action = route_abandon_candidate_at(&relay->upstream,
                                            failed_parent_id,
                                            relay->gateway_id,
                                            failed_route_epoch,
                                            now_ms);
        relay->pending.parent_hop_ack_miss_count = 0u;
    } else {
        action = route_record_failure_at(&relay->upstream,
                                         ROUTE_FAILURE_GATEWAY_ACK,
                                         now_ms);
    }
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
        /* Every candidate is exhausted: advertise UNREACHABLE until repair. */
        mesh_relay_poison_upstream(relay);
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
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        relay->pending.state != MESH_RELAY_TX_IDLE) {
        return PROTO_ERR_MALFORMED;
    }
#endif
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

    if (pending_gateway_ack_confirm_recovery_due(relay, now_ms)) {
        pending_recover_original_after_gateway_ack_confirm(relay, now_ms);
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
            if (packet_uses_report_hop_custody(
                    relay, &relay->pending.packet) &&
                mesh_relay_frozen_upstream_hop_usable(
                    relay,
                    relay->pending.next_hop_id,
                    now_ms)) {
                next_hop_id = relay->pending.next_hop_id;
                ret = PROTO_OK;
            } else {
                ret = mesh_relay_select_next_hop_for_packet(
                    relay,
                    &relay->pending.packet,
                    relay->pending.payload,
                    relay->pending.payload_len,
                    now_ms,
                    &next_hop_id);
            }
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
        if (!relay->pending.gateway_ack_confirm_pending) {
            relay->pending.packet.message_age_ms =
                result->retransmit.packet.message_age_ms;
            relay->pending.queued_at_ms = now_ms;
        }
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

static bool late_gateway_ack_can_forward_independently(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id)
{
    const struct mesh_downlink_entry *downlink;

    if (relay == NULL || packet == NULL ||
        packet->msg_type != MSG_GATEWAY_ACK ||
        packet->src_id != relay->gateway_id ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id == relay->local_id ||
        packet->dst_id == relay->gateway_id || packet->ttl <= 1u ||
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

#if !IMEC_MESH_RELAY_GATEWAY_ONLY
static bool gateway_ack_confirm_can_forward_independently(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    uint64_t previous_hop_id)
{
    uint64_t next_hop_id = 0u;

    if (relay == NULL || packet == NULL ||
        packet->msg_type != MSG_GATEWAY_ACK_CONFIRM ||
        packet->src_id == relay->local_id ||
        packet->src_id == relay->gateway_id ||
        packet->dst_id != relay->gateway_id ||
        packet->ttl <= 1u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        mesh_relay_select_next_hop(relay,
                                   relay->gateway_id,
                                   &next_hop_id) != PROTO_OK) {
        return false;
    }

    /* ACK_CONFIRM has its own bounded gateway-bound queue owner in the app.
     * It can therefore pass an unrelated core TX owner without replacing it.
     * This is also the terminal proof that may overtake a duplicate gateway
     * ACK still queued toward the source, so requiring the core owner to go
     * idle first would deadlock that source behind redundant ACK custody. */
    return next_hop_id != previous_hop_id;
}
#endif

static int handle_retained_pending_retry(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint32_t now_ms,
    struct mesh_relay_result *result)
{
    bool same_identity;
    bool same_payload;
    int ret;

    same_identity = relay->pending.packet.msg_type == packet->msg_type &&
                    relay->pending.packet.src_id == packet->src_id &&
                    relay->pending.packet.dst_id == packet->dst_id &&
                    relay->pending.packet.session_id == packet->session_id &&
                    relay->pending.packet.seq == packet->seq;
    if (!same_identity) {
        return PROTO_ERR_NOT_FOUND;
    }
    same_payload = relay->pending.packet.payload_len ==
                       relay->pending.payload_len &&
                   relay->pending.payload_len == payload_len &&
                   packet->payload_len == payload_len &&
                   (payload_len == 0u ||
                    memcmp(relay->pending.payload, payload, payload_len) == 0);
    if (!same_payload) {
        result->actions |= MESH_RELAY_ACTION_DROP;
        result->status = PROTO_ERR_MALFORMED;
        return PROTO_ERR_MALFORMED;
    }

    /* Live custody outlasts the short duplicate cache. Re-forward the
     * immutable retained bytes and re-ACK the exact child retry. */
    ret = outbound_from_pending(relay,
                                &relay->pending,
                                now_ms,
                                &result->forward);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        result->status = ret;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return ret;
    }
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_FORWARD;
    }
    add_hop_ack_action(relay,
                       packet,
                       payload,
                       payload_len,
                       previous_hop_id,
                       now_ms,
                       result);
    result->status = PROTO_ERR_STALE;
    return PROTO_OK;
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
    return mesh_relay_handle_rx_with_random_radio(relay,
                                                  packet,
                                                  payload,
                                                  payload_len,
                                                  previous_hop_id,
                                                  link_quality,
                                                  0,
                                                  false,
                                                  now_ms,
                                                  random_value,
                                                  result);
}

int mesh_relay_handle_rx_with_random_radio(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t link_quality,
    int8_t link_rsl_dbm,
    bool link_rsl_valid,
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
        !id_is_unicast(packet->src_id)) {
        return PROTO_ERR_ARG;
    }
    if (packet->src_id == relay->local_id) {
        /*
         * A parent that lost its route may hand a packet back to whoever gave
         * it, including the node that originated it.  If this relay handed
         * exactly these bytes to exactly that parent and never saw a gateway
         * ACK for them, they are ours again: re-adopt custody rather than
         * dropping our own packet as a self-addressed duplicate.
         */
        if (!mesh_relay_should_readopt_returned_packet(relay,
                                                        packet,
                                                        previous_hop_id)) {
            return PROTO_ERR_ARG;
        }
        result_reset(result);
        if (mesh_relay_tx_active(relay)) {
            /* No free custody slot: say so instead of staying silent. */
            return mesh_relay_build_backpressure_ack(relay,
                                                     packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id,
                                                     0u,
                                                     now_ms,
                                                     result);
        }
        ret = mesh_relay_start_tx(relay,
                                  packet,
                                  payload,
                                  payload_len,
                                  now_ms,
                                  &result->retransmit);
        if (ret != PROTO_OK) {
            return mesh_relay_build_backpressure_ack(relay,
                                                     packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id,
                                                     0u,
                                                     now_ms,
                                                     result);
        }
        result->actions |= MESH_RELAY_ACTION_RETRANSMIT |
                           MESH_RELAY_ACTION_CUSTODY_ACCEPTED;
        add_hop_ack_action(relay,
                           packet,
                           payload,
                           payload_len,
                           previous_hop_id,
                           now_ms,
                           result);
        return PROTO_OK;
    }

    result_reset(result);
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    (void)link_rsl_dbm;
    (void)link_rsl_valid;
    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        relay->pending.state != MESH_RELAY_TX_IDLE) {
        return PROTO_ERR_MALFORMED;
    }
#endif
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
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
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
            relay_semantic_packet_digest(relay,
                                         packet,
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
#endif
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
        replay = broadcast_command_replay_classify(relay,
                                                   &relay->command_replay,
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
        (packet->dst_id == relay->local_id
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
         ||
         gateway_ack_exactly_targets_pending_transit_origin(
             relay,
             packet,
             payload,
             payload_len,
             previous_hop_id)
#endif
        )) {
        (void)handle_local_ack(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               now_ms,
                               random_value,
                               result);
        return PROTO_OK;
    }
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
    if (gateway_ack_targets_pending_transit_origin(relay, packet) &&
        !gateway_ack_exactly_targets_pending_transit_confirm(
            relay,
            packet,
            payload,
            payload_len,
            previous_hop_id)) {
        /* A gateway ACK addressed to this transit origin is either the exact
         * ACK for its retained original, handled above, or the exact terminal
         * ACK for that original's ACK_CONFIRM, allowed to continue below.
         * Any other same-destination ACK must not escape through the late-ACK
         * forwarding lane while this core owner is live. */
        return PROTO_OK;
    }
#endif

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
                                         now_ms,
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
            relay_diag_inc_u8(&relay->diagnostics.flood_suppression_count);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        if (mesh_relay_packet_can_queue_gateway_report(relay, packet)) {
            /*
             * DUPLICATE_CLASS_EXACT is returned only for a digest-bound
             * entry whose first delivery was accepted.  Per-hop custody is
             * therefore already at this relay even if the child missed the
             * original HOP_ACK.  Replay that terminal proof without creating
             * a second upstream owner.  CUSTODY_ACCEPTED tells the platform
             * handoff gate that this ACK is backed by existing custody rather
             * than by a new forward in this receive turn.
             */
            add_hop_ack_action(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               now_ms,
                               result);
            result->actions |= MESH_RELAY_ACTION_CUSTODY_ACCEPTED |
                               MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        if (mesh_relay_tx_active(relay) &&
            !late_gateway_ack_can_forward_independently(
                relay, packet, previous_hop_id) &&
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
            !gateway_ack_confirm_can_forward_independently(
                relay, packet, previous_hop_id) &&
#endif
            !gateway_targeted_control_can_forward_independently(
                relay, packet, previous_hop_id)) {
            /* A retry of the exact packet this relay already owns is a
             * progress probe, not a competing transaction.  Identity alone
             * is insufficient: a same-header packet with different bytes
             * must never replace or extend custody. */
            ret = handle_retained_pending_retry(relay,
                                                packet,
                                                payload,
                                                payload_len,
                                                previous_hop_id,
                                                now_ms,
                                                result);
            if (ret != PROTO_ERR_NOT_FOUND) {
                return PROTO_OK;
            }
            if (mesh_relay_packet_can_queue_gateway_report(relay, packet)) {
                /* The first copy transferred into the bounded app queue.
                 * Repair only the child's hop ACK; the queue remains the one
                 * immutable owner of the report. */
                add_hop_ack_action(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   now_ms,
                                   result);
                result->actions |= MESH_RELAY_ACTION_DROP;
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

        ret = build_forward(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            false,
                            now_ms,
                            &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
            add_hop_ack_action(relay,
                               packet,
                               payload,
                               payload_len,
                               previous_hop_id,
                               now_ms,
                               result);
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        result->actions |= MESH_RELAY_ACTION_DROP;
        result->status = ret;
        return PROTO_OK;
    }

    if (packet_needs_forward(relay, packet) &&
        packet->msg_type == MSG_ANCHOR_HEARTBEAT &&
        packet->flags == 0u && mesh_relay_tx_active(relay)) {
        /* Heartbeat is disposable telemetry. It cannot displace retained
         * custody or create a RELAY_BUSY response on Channel 5. */
        result->status = PROTO_ERR_BUSY;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type != MSG_ROUTE_REQ &&
        packet->msg_type != MSG_ROUTE_REPLY &&
        packet_needs_forward(relay, packet) &&
        !gateway_delivery_requires_commit(relay, packet) &&
        !late_gateway_ack_can_forward_independently(
            relay, packet, previous_hop_id) &&
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
        !gateway_ack_confirm_can_forward_independently(
            relay, packet, previous_hop_id) &&
#endif
        !gateway_targeted_control_can_forward_independently(
            relay, packet, previous_hop_id) &&
        !mesh_relay_packet_can_queue_gateway_report(relay, packet) &&
        mesh_relay_tx_active(relay)) {
        ret = handle_retained_pending_retry(relay,
                                            packet,
                                            payload,
                                            payload_len,
                                            previous_hop_id,
                                            now_ms,
                                            result);
        if (ret != PROTO_ERR_NOT_FOUND) {
            return PROTO_OK;
        }
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
#if IMEC_MESH_RELAY_GATEWAY_ONLY
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
#else
        ret = handle_gateway_route_adv(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       link_quality,
                                       link_rsl_dbm,
                                       link_rsl_valid,
                                       now_ms,
                                       random_value,
                                       result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, payload, payload_len, now_ms);
        }
#endif
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
        packet->dst_id == MESH_BROADCAST_ID) {
#if IMEC_MESH_RELAY_GATEWAY_ONLY
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
#else
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
#endif
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
                                     now_ms,
                                     result);
        return PROTO_OK;
    }

    if (packet->dst_id == MESH_BROADCAST_ID) {
        if (packet->msg_type == MSG_ROUTE_SOLICIT) {
            /*
             * A solicit is consumed locally: the gateway answers with a
             * Here-I-Am wave and an anchor re-originates it upstream.  It is
             * never re-flooded, so a solicit storm cannot propagate.
             */
            duplicate_store(relay, packet, payload, payload_len, now_ms);
            result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
            return PROTO_OK;
        }
        if (packet->msg_type == MSG_COMMAND) {
            ret = broadcast_command_replay_store(
                    relay,
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
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
        struct command_result_id result_id;
        uint32_t collection_epoch_id = 0u;
        bool reservation_applies =
            relay->result_offer_reservation.valid &&
            relay->result_offer_reservation.child_id == packet->src_id;
#endif

        ret = result_offer_reservation_matches_payload(relay, packet, payload, payload_len);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
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
                                   now_ms,
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
                 * its exact retained result and retries this or an alternate
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
#endif
    }

    ret = build_forward(relay,
                        packet,
                        payload,
                        payload_len,
                        previous_hop_id,
                        false,
                        now_ms,
                        &result->forward);
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
                           now_ms,
                           result);
        return PROTO_OK;
    }

    result->status = ret;
    result->actions |= MESH_RELAY_ACTION_DROP;
    return PROTO_OK;
}
