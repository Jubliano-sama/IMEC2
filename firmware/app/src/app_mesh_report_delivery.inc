#line 12150 __BASE_FILE__

void report_tx_schedule(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms = k_uptime_get_32();

    delay_ms = app_mesh_schedule_preserve_deadline(
        &report_tx_backoff_active,
        report_tx_backoff_until_ms,
        now_ms,
        delay_ms);
    (void)mesh_reschedule_owned_work(&report_tx_work,
                                     delay_ms,
                                     "anchor-report-queue");
#else
    ARG_UNUSED(delay_ms);
#endif
}


#if DEVICE_ROLE == ROLE_ANCHOR
static void report_tx_schedule_backoff(uint32_t delay_ms)
{
    report_tx_backoff_until_ms = k_uptime_get_32() + delay_ms;
    report_tx_backoff_active = true;
    report_tx_schedule(delay_ms);
}
#endif

static void mesh_c5_flood_work_handler(struct k_work *work)
{
    struct mesh_c5_flood_deferred_entry *entry;
    struct mesh_outbound outbound;
    uint32_t generation;
    uint32_t queued_at_ms;
    uint8_t purpose;
    uint8_t retry_count;
    const char *reason;
    bool current_generation;
    bool pending_after;
    bool response_priority;
    bool route_adv_lane;
    bool send_wake_train;
    uint32_t pending_delay_ms = 0u;
    struct app_mesh_flood_result result = {0};
    int schedule_ret;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        /* Pause cancellation deliberately transfers every valid deferred
         * generation to mesh_transport_resume(). Scheduling here would only
         * produce pause-owned -ESHUTDOWN and no independent worker. */
        return;
    }

    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    entry = mesh_c5_flood_deferred.valid ?
        &mesh_c5_flood_deferred : &mesh_route_adv_deferred;
    if (!entry->valid) {
        k_mutex_unlock(&mesh_c5_flood_deferred_lock);
        return;
    }

    route_adv_lane = entry == &mesh_route_adv_deferred;
    if (!mesh_c5_flood_deferred_entry_due(entry, k_uptime_get_32())) {
        pending_delay_ms = mesh_c5_flood_next_delay_locked(k_uptime_get_32());
        schedule_ret = mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                                  pending_delay_ms,
                                                  "c5-flood-not-due");
        k_mutex_unlock(&mesh_c5_flood_deferred_lock);
        if (schedule_ret < 0) {
            mesh_c5_flood_schedule_loss_fail_stop(
                schedule_ret, "c5-flood-not-due");
        }
        return;
    }
    generation = entry->generation;
    outbound = entry->outbound;
    purpose = entry->purpose;
    reason = entry->reason;
    response_priority = entry->response_priority;
    send_wake_train = true;
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);

    ret = mesh_send_c5_flood_now_intent(&outbound,
                                        purpose,
                                        reason,
                                        send_wake_train,
                                        response_priority,
                                        false,
                                        NULL,
                                        &result,
                                        NULL,
                                        mesh_c5_forwarded_control_intent(
                                            &outbound));
    if (result.sent_count == 0u && mesh_send_failure_retryable(ret)) {
        uint32_t retry_ms;
        bool local_deferral = ret == -EAGAIN || ret == -EBUSY;

        k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
        entry = route_adv_lane ? &mesh_route_adv_deferred :
                                 &mesh_c5_flood_deferred;
        current_generation = entry->valid &&
                             entry->generation == generation;
        if (current_generation && local_deferral &&
            k_uptime_get_32() - entry->queued_at_ms <
                MESH_C5_LOCAL_DEFER_MAX_AGE_MS) {
            mesh_c5_flood_deferred_schedule_after_locked(
                entry,
                k_uptime_get_32(),
                MESH_C5_LOCAL_DEFER_RETRY_MS);
            schedule_ret = mesh_reschedule_owned_work(
                &mesh_c5_flood_work,
                mesh_c5_flood_next_delay_locked(k_uptime_get_32()),
                "c5-flood-local-defer");
            k_mutex_unlock(&mesh_c5_flood_deferred_lock);
            if (schedule_ret < 0) {
                mesh_c5_flood_schedule_loss_fail_stop(
                    schedule_ret, "c5-flood-local-defer");
            }
            return;
        }
        if (current_generation && !local_deferral &&
            entry->retry_count < MESH_C5_DEFERRED_MAX_RETRIES) {
            retry_ms = mesh_c5_flood_deferred_retry_ms(
                &outbound, &entry->rf_retry);
            entry->retry_count++;
            mesh_c5_flood_deferred_schedule_after_locked(
                entry, k_uptime_get_32(), retry_ms);
            schedule_ret = mesh_reschedule_owned_work(
                &mesh_c5_flood_work,
                mesh_c5_flood_next_delay_locked(k_uptime_get_32()),
                "c5-flood-retry");
            k_mutex_unlock(&mesh_c5_flood_deferred_lock);
            if (schedule_ret < 0) {
                mesh_c5_flood_schedule_loss_fail_stop(
                    schedule_ret, "c5-flood-retry");
            }
            return;
        }
        k_mutex_unlock(&mesh_c5_flood_deferred_lock);
        if (!current_generation) {
            return;
        }
    }

    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    entry = route_adv_lane ? &mesh_route_adv_deferred :
                             &mesh_c5_flood_deferred;
    current_generation = entry->valid &&
                         entry->generation == generation;
    retry_count = entry->retry_count;
    queued_at_ms = entry->queued_at_ms;
    if (current_generation) {
        entry->valid = false;
        app_mesh_rf_retry_reset(&entry->rf_retry);
    }
    pending_after = mesh_c5_flood_deferred.valid ||
                    mesh_route_adv_deferred.valid;
    if (pending_after) {
        pending_delay_ms = mesh_c5_flood_next_delay_locked(
            k_uptime_get_32());
    }
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);

    if (ret == 0 && result.sent_count > 0u) {
        mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
    } else if (ret < 0 && current_generation) {
        LOG_ERR("deferred C5 flood exhausted: msg=0x%02x ret=%d attempts=%u age_ms=%u",
                outbound.packet.msg_type,
                ret,
                retry_count,
                k_uptime_get_32() - queued_at_ms);
    }
    if (pending_after) {
        schedule_ret = mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                                  pending_delay_ms,
                                                  "c5-flood-next-lane");
        if (schedule_ret < 0) {
            mesh_c5_flood_schedule_loss_fail_stop(
                schedule_ret, "c5-flood-next-lane");
        }
    }
}

uint32_t report_tx_queue_used(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t used;

    if (k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER) != 0) {
        return (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    }
    if (!anchor_range_report_batch_reservation.active) {
        (void)report_tx_queue_restore_recovery_locked();
    }
    used = (uint32_t)k_msgq_num_used_get(&report_tx_msgq) +
           (report_tx_queue_recovery_valid ? 1u : 0u);
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return used;
#else
    return 0u;
#endif
}

static bool mesh_packet_delivery_identity_matches(
    const struct proto_packet *left,
    const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->payload_len == right->payload_len;
}

int mesh_report_active_owner_matches_outbound(
    const struct mesh_outbound *outbound)
{
    const struct mesh_pending_tx *pending;
    struct proto_packet confirmed_original;
    uint8_t active_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t outbound_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (outbound == NULL ||
        outbound->packet.payload_len != outbound->payload_len ||
        !mesh_packet_semantic_digest(&outbound->packet,
                                     outbound->payload,
                                     outbound->payload_len,
                                     outbound_digest)) {
        return -EINVAL;
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return 0;
    }
    pending = &mesh_runtime.pending;
    if (pending->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        ret = mesh_gateway_ack_confirm_identity_packet(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            &confirmed_original,
            active_digest);
        if (ret != PROTO_OK) {
            return -EBADMSG;
        }
        if (!mesh_packet_delivery_identity_matches(&confirmed_original,
                                                   &outbound->packet)) {
            return 0;
        }
        return semantic_digest_equal(active_digest,
                                     outbound_digest,
                                     sizeof(active_digest)) ?
                   1 : -EBADMSG;
    }
    if (!mesh_packet_delivery_identity_matches(&pending->packet,
                                               &outbound->packet)) {
        return 0;
    }
    if (!mesh_packet_semantic_digest(&pending->packet,
                                     pending->payload,
                                     pending->payload_len,
                                     active_digest)) {
        return -EBADMSG;
    }
    return semantic_digest_equal(active_digest,
                                 outbound_digest,
                                 sizeof(active_digest)) ?
               1 : -EBADMSG;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool anchor_range_report_batch_matches_locked(uint64_t clicker_id,
                                                     uint32_t event_seq,
                                                     uint8_t attempt_index)
{
    return anchor_range_report_batch_reservation.active &&
           anchor_range_report_batch_reservation.clicker_id == clicker_id &&
           anchor_range_report_batch_reservation.event_seq == event_seq &&
           anchor_range_report_batch_reservation.attempt_index ==
               attempt_index;
}

static uint16_t anchor_range_report_all_acknowledged_mask(uint8_t count)
{
    return count == 0u || count > RANGE_REPORT_MAX_PACKET_FRAGMENTS ?
           0u : (uint16_t)((UINT16_C(1) << count) - 1u);
}

static bool anchor_range_report_packet_identity_matches(
    const struct anchor_range_report_control *control,
    uint8_t fragment_index,
    const struct proto_packet *packet)
{
    return control != NULL && packet != NULL &&
           fragment_index < control->fragment_count &&
           packet->msg_type == MSG_CLICK_REPORT &&
           (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           packet->src_id == control->anchor_id &&
           packet->dst_id == control->gateway_id &&
           packet->session_id == proto_click_report_session_id(
               control->clicker_id, control->event_seq) &&
           packet->seq == control->fragments[fragment_index].seq;
}

static int anchor_range_report_fragment_validate_locked(
    const struct mesh_outbound *outbound,
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index)
{
    const struct proto_packet *packet;
    const uint8_t *payload_attempt_index = NULL;
    uint16_t expected_seq;
    uint8_t payload_attempt_index_len = 0u;
    uint8_t mode_flags;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    packet = &outbound->packet;
    mode_flags = packet->flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC);
    if (packet->msg_type != MSG_CLICK_REPORT ||
        packet->flags != (uint8_t)(FLAG_GATEWAY_ACK_REQUIRED | mode_flags) ||
        (mode_flags != FLAG_COUNT_AS_CLICK &&
         mode_flags != FLAG_DIAGNOSTIC) ||
        packet->src_id != DEVICE_ID ||
        packet->dst_id != GATEWAY_ID ||
        packet->session_id == 0u ||
        packet->session_id !=
            proto_click_report_session_id(clicker_id, event_seq) ||
        packet->seq == 0u ||
        packet->ttl != REPORT_DEFAULT_TTL ||
        packet->payload_len == 0u ||
        packet->payload_len != outbound->payload_len ||
        report_validate_click_payload(packet,
                                      outbound->payload,
                                      outbound->payload_len) != PROTO_OK) {
        return -EBADMSG;
    }
    ret = tlv_find_unique(outbound->payload,
                          outbound->payload_len,
                          TLV_ATTEMPT_INDEX,
                          &payload_attempt_index,
                          &payload_attempt_index_len);
    if (ret != PROTO_OK || payload_attempt_index == NULL ||
        payload_attempt_index_len != sizeof(attempt_index) ||
        payload_attempt_index[0] != attempt_index) {
        return -EBADMSG;
    }
    if (anchor_range_report_batch_reservation.control.fragment_count !=
        anchor_range_report_batch_reservation.queued_fragment_count) {
        return -ESTALE;
    }
    for (uint8_t i = 0u;
         i < anchor_range_report_batch_reservation.control.fragment_count;
         i++) {
        if (anchor_range_report_batch_reservation.control.fragments[i].seq ==
            packet->seq) {
            return -EALREADY;
        }
    }
    ret = report_range_transport_seq(
        attempt_index,
        anchor_range_report_batch_reservation.control.fragment_count,
        &expected_seq);
    if (ret != PROTO_OK || packet->seq != expected_seq) {
        return -EBADMSG;
    }
    return 0;
}

static bool anchor_range_report_fragment_matches_control_locked(
    const struct anchor_range_report_control *control,
    uint8_t fragment_index,
    const struct mesh_outbound *outbound)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    return outbound != NULL &&
           anchor_range_report_packet_identity_matches(
               control, fragment_index, &outbound->packet) &&
           mesh_packet_semantic_digest(&outbound->packet,
                                       outbound->payload,
                                       outbound->payload_len,
                                       digest) &&
           semantic_digest_equal(control->fragments[fragment_index].digest,
                                 digest,
                                 sizeof(digest));
}

#if defined(CONFIG_ZTEST)
static bool anchor_range_report_control_matches(
    const struct anchor_range_report_control *left,
    const struct anchor_range_report_control *right)
{
    if (left == NULL || right == NULL ||
        left->clicker_id != right->clicker_id ||
        left->anchor_id != right->anchor_id ||
        left->gateway_id != right->gateway_id ||
        left->event_seq != right->event_seq ||
        left->fragment_count != right->fragment_count ||
        left->attempt_index != right->attempt_index) {
        return false;
    }
    for (uint8_t i = 0u; i < left->fragment_count; i++) {
        if (left->fragments[i].seq != right->fragments[i].seq ||
            !semantic_digest_equal(left->fragments[i].digest,
                                   right->fragments[i].digest,
                                   SEMANTIC_DIGEST_SHA256_LEN)) {
            return false;
        }
    }
    return true;
}
#endif

static bool anchor_range_report_ack_generation_matches(
    const struct anchor_range_report_ack_runtime *runtime,
    uint64_t generation)
{
    return runtime != NULL && runtime->active && generation != 0u &&
           runtime->generation == generation;
}

/*
 * Resolve a producer callback against the exact immutable fragment held in
 * RAM by the range ACK ledger. Packet identity alone is insufficient because a
 * wrapped session/sequence can name different bytes; ACK_CONFIRM carries the
 * original semantic digest specifically to close that collision. The fragment
 * digest is captured in RAM at admission time since there is no flash copy.
 */
static int anchor_range_report_fragment_digest_match_locked(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint8_t *fragment_index)
{
    if (packet == NULL || semantic_digest == NULL ||
        fragment_index == NULL) {
        return -EINVAL;
    }
    for (uint8_t i = 0u;
         i < anchor_range_report_ack_runtime.control.fragment_count;
         i++) {
        if (!anchor_range_report_packet_identity_matches(
                &anchor_range_report_ack_runtime.control, i, packet)) {
            continue;
        }
        if (!semantic_digest_equal(
                anchor_range_report_ack_runtime.control.fragments[i].digest,
                semantic_digest,
                SEMANTIC_DIGEST_SHA256_LEN)) {
            return -EBADMSG;
        }
        *fragment_index = i;
        return 1;
    }
    return 0;
}
#endif

int mesh_range_report_batch_reserve_capacity(uint64_t clicker_id,
                                             uint32_t event_seq,
                                             uint8_t attempt_index,
                                             uint8_t fragment_capacity)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct anchor_range_report_control control;
    uint32_t queue_count;
    int ret;

    if (clicker_id == 0u || event_seq == 0u || fragment_capacity == 0u ||
        fragment_capacity > RANGE_REPORT_MAX_PACKET_FRAGMENTS) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_matches_locked(clicker_id,
                                                 event_seq,
                                                 attempt_index)) {
        if (anchor_range_report_batch_reservation
                .queue_admission_fail_closed) {
            ret = anchor_range_report_batch_reservation.queue_error != 0 ?
                      anchor_range_report_batch_reservation.queue_error :
                      -EIO;
            goto out;
        }
        ret = fragment_capacity <=
                      anchor_range_report_batch_reservation.fragment_capacity ?
                  (anchor_range_report_batch_reservation
                           .queued_fragment_count == 0u ?
                       0 : -EALREADY) :
                  -ENOSPC;
        goto out;
    }
    if (anchor_range_report_batch_reservation.active) {
        ret = -EBUSY;
        goto out;
    }
    if (anchor_range_report_ack_runtime.active) {
        uint16_t all_acknowledged =
            anchor_range_report_all_acknowledged_mask(
                anchor_range_report_ack_runtime.control.fragment_count);

        if (all_acknowledged == 0u ||
            anchor_range_report_ack_runtime.acknowledged_mask !=
                all_acknowledged) {
            if (app_anchor_click_event_runtime_result_owned()) {
                ret = -EBUSY;
                goto out;
            }
        }
        /*
         * The old packets remain owned by the report queue or relay.  Once
         * their phase owner has detached, this lifecycle-only ACK ledger may
         * be replaced; exact per-packet transport custody is unaffected.
         */
        memset(&anchor_range_report_ack_runtime, 0,
               sizeof(anchor_range_report_ack_runtime));
    }
    (void)report_tx_queue_restore_recovery_locked();
    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    if (app_mesh_queue_head_owned(&report_tx_queue_head_owner) ||
        report_tx_queue_recovery_valid ||
        queue_count > UINT8_MAX ||
        k_msgq_num_free_get(&report_tx_msgq) < fragment_capacity) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_RANGE_BATCH_RESERVE_BUSY evt=%u attempt=%u q=%u free=%u cap=%u head=%u recovery=%u cir=%u ackwait=%u\n",
                                event_seq,
                                attempt_index,
                                queue_count,
                                k_msgq_num_free_get(&report_tx_msgq),
                                fragment_capacity,
                                app_mesh_queue_head_owned(
                                    &report_tx_queue_head_owner) ? 1u : 0u,
                                report_tx_queue_recovery_valid ? 1u : 0u,
                                app_mesh_report_encode_cir_pending() ? 1u : 0u,
                                0u);
        }
        ret = -EAGAIN;
        goto out;
    }
    memset(&control, 0, sizeof(control));
    control.clicker_id = clicker_id;
    control.event_seq = event_seq;
    control.attempt_index = attempt_index;
    control.anchor_id = DEVICE_ID;
    control.gateway_id = GATEWAY_ID;
    anchor_range_report_batch_reservation =
        (struct anchor_range_report_batch_reservation) {
            .control = control,
            .clicker_id = clicker_id,
            .event_seq = event_seq,
            .attempt_index = attempt_index,
            .fragment_capacity = fragment_capacity,
            .queue_prefix_count = (uint8_t)queue_count,
            .active = true,
        };
    ret = 0;

out:
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
#else
    ARG_UNUSED(clicker_id);
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(fragment_capacity);
    return 0;
#endif
}

int mesh_range_report_batch_reserve(uint64_t clicker_id,
                                    uint32_t event_seq,
                                    uint8_t attempt_index)
{
    return mesh_range_report_batch_reserve_capacity(
        clicker_id, event_seq, attempt_index,
        RANGE_REPORT_MAX_PACKET_FRAGMENTS);
}

int mesh_range_report_batch_rebind(uint64_t current_clicker_id,
                                   uint32_t current_event_seq,
                                   uint8_t current_attempt_index,
                                   uint64_t replacement_clicker_id,
                                   uint32_t replacement_event_seq,
                                   uint8_t replacement_attempt_index)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    int ret;

    if (current_clicker_id == 0u || current_event_seq == 0u ||
        replacement_clicker_id == 0u || replacement_event_seq == 0u) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (!anchor_range_report_batch_matches_locked(current_clicker_id,
                                                  current_event_seq,
                                                  current_attempt_index)) {
        ret = -ESTALE;
        goto out;
    }
    if (anchor_range_report_batch_reservation.queue_admission_fail_closed ||
        anchor_range_report_batch_reservation.rollback_scratch_owned ||
        anchor_range_report_batch_reservation.queued_fragment_count != 0u ||
        anchor_range_report_batch_reservation.control.fragment_count != 0u) {
        ret = -EBUSY;
        goto out;
    }

    anchor_range_report_batch_reservation.clicker_id =
        replacement_clicker_id;
    anchor_range_report_batch_reservation.event_seq = replacement_event_seq;
    anchor_range_report_batch_reservation.attempt_index =
        replacement_attempt_index;
    anchor_range_report_batch_reservation.control.clicker_id =
        replacement_clicker_id;
    anchor_range_report_batch_reservation.control.event_seq =
        replacement_event_seq;
    anchor_range_report_batch_reservation.control.attempt_index =
        replacement_attempt_index;
    ret = 0;

out:
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
#else
    ARG_UNUSED(current_clicker_id);
    ARG_UNUSED(current_event_seq);
    ARG_UNUSED(current_attempt_index);
    ARG_UNUSED(replacement_clicker_id);
    ARG_UNUSED(replacement_event_seq);
    ARG_UNUSED(replacement_attempt_index);
    return 0;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_ZTEST)
enum mesh_range_report_batch_abort_queue_operation {
    MESH_RANGE_REPORT_BATCH_ABORT_PREFLIGHT_PEEK = 0,
    MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_GET,
    MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_PUT,
    MESH_RANGE_REPORT_BATCH_ABORT_REMOVE_GET,
    MESH_RANGE_REPORT_BATCH_ABORT_POST_GET,
};

static int (*mesh_range_report_batch_abort_test_queue_fault)(
    enum mesh_range_report_batch_abort_queue_operation operation,
    uint32_t index);
static int (*mesh_range_report_fragment_test_put_fault)(uint8_t fragment_index);
static void (*mesh_range_report_test_after_lifecycle_release_hook)(void);
#endif

void mesh_range_report_batch_abort(uint64_t clicker_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t queue_count;
    uint32_t queue_depth;
    uint8_t fragments_to_remove;
    bool recovery_pending;
    int ret;

    if (k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER) != 0) {
        LOG_ERR("range report batch abort could not lock report queue");
        return;
    }
    if (!anchor_range_report_batch_matches_locked(clicker_id,
                                                  event_seq,
                                                  attempt_index)) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }
    if (anchor_range_report_batch_reservation.queue_admission_fail_closed) {
        /*
         * The reservation plus its queue/scratch record is the only exact
         * owner after rollback became uncertain. Keep it immutable until the
         * watchdog resets the process; retrying could overwrite owned bytes.
         */
        LOG_ERR("range report batch abort preserved fail-closed queue admission: event=%u fragment=%u error=%d",
                event_seq,
                anchor_range_report_batch_reservation
                    .queued_fragment_count,
                anchor_range_report_batch_reservation.queue_error);
        app_watchdog_stop_feeding();
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }

    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    if (app_mesh_queue_head_owned(&report_tx_queue_head_owner) ||
        anchor_range_report_batch_reservation.rollback_scratch_owned ||
        anchor_range_report_batch_reservation.control.fragment_count !=
            anchor_range_report_batch_reservation.queued_fragment_count ||
        queue_count !=
            (uint32_t)anchor_range_report_batch_reservation.queue_prefix_count +
                anchor_range_report_batch_reservation.queued_fragment_count) {
        LOG_ERR("range report batch queue ownership corrupted: queued=%u prefix=%u expected=%u recovery=%u head=%u",
                queue_count,
                anchor_range_report_batch_reservation.queue_prefix_count,
                anchor_range_report_batch_reservation.queued_fragment_count,
                report_tx_queue_recovery_valid ? 1u : 0u,
                app_mesh_queue_head_owned(&report_tx_queue_head_owner) ? 1u : 0u);
        anchor_range_report_batch_reservation.queue_error = -ESTALE;
        anchor_range_report_batch_reservation.queue_admission_fail_closed = true;
        app_watchdog_stop_feeding();
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }

    fragments_to_remove =
        anchor_range_report_batch_reservation.queued_fragment_count;
    /* Prove the entire ordered batch before removing its first byte. */
    for (uint8_t i = 0u; i < fragments_to_remove; i++) {
#if defined(CONFIG_ZTEST)
        ret = mesh_range_report_batch_abort_test_queue_fault != NULL ?
                  mesh_range_report_batch_abort_test_queue_fault(
                      MESH_RANGE_REPORT_BATCH_ABORT_PREFLIGHT_PEEK, i) :
                  0;
        if (ret == 0)
#endif
        {
            ret = k_msgq_peek_at(&report_tx_msgq,
                                 &report_tx_queue_rotation_scratch,
                                 anchor_range_report_batch_reservation
                                         .queue_prefix_count +
                                     i);
        }
        if (ret != 0 ||
            !anchor_range_report_fragment_matches_control_locked(
                &anchor_range_report_batch_reservation.control,
                i,
                &report_tx_queue_rotation_scratch)) {
            if (ret == 0) {
                ret = -ESTALE;
            }
            LOG_ERR("range report batch abort preflight rejected fragment %u: %d",
                    i, ret);
            anchor_range_report_batch_reservation.queue_error = ret;
            anchor_range_report_batch_reservation.queue_admission_fail_closed = true;
            app_watchdog_stop_feeding();
            k_mutex_unlock(&report_tx_queue_overflow_lock);
            return;
        }
    }
    /* Move the proven pre-existing prefix behind this batch. Removing the
     * batch from the head then restores the prefix byte-for-byte and in order.
     * The queue mutex prevents either owner from changing during the rotation. */
    if (fragments_to_remove > 0u) {
        for (uint8_t i = 0u;
             i < anchor_range_report_batch_reservation.queue_prefix_count;
             i++) {
#if defined(CONFIG_ZTEST)
            ret = mesh_range_report_batch_abort_test_queue_fault != NULL ?
                      mesh_range_report_batch_abort_test_queue_fault(
                          MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_GET, i) :
                      0;
            if (ret == 0)
#endif
            {
                ret = k_msgq_get(&report_tx_msgq,
                                 &report_tx_queue_rotation_scratch,
                                 K_NO_WAIT);
            }
            if (ret != 0) {
                LOG_ERR("range report batch abort could not rotate prefix %u: %d",
                        i, ret);
                anchor_range_report_batch_reservation.queue_error = ret;
                anchor_range_report_batch_reservation
                    .queue_admission_fail_closed = true;
                app_watchdog_stop_feeding();
                k_mutex_unlock(&report_tx_queue_overflow_lock);
                return;
            }
            anchor_range_report_batch_reservation.rollback_scratch_owned = true;
#if defined(CONFIG_ZTEST)
            ret = mesh_range_report_batch_abort_test_queue_fault != NULL ?
                      mesh_range_report_batch_abort_test_queue_fault(
                          MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_PUT, i) :
                      0;
            if (ret == 0)
#endif
            {
                ret = k_msgq_put(&report_tx_msgq,
                                 &report_tx_queue_rotation_scratch,
                                 K_NO_WAIT);
            }
            if (ret != 0) {
                LOG_ERR("range report batch abort could not restore prefix %u: %d",
                        i, ret);
                anchor_range_report_batch_reservation.queue_error = ret;
                anchor_range_report_batch_reservation
                    .queue_admission_fail_closed = true;
                app_watchdog_stop_feeding();
                k_mutex_unlock(&report_tx_queue_overflow_lock);
                return;
            }
            anchor_range_report_batch_reservation.rollback_scratch_owned = false;
        }
    }
    for (uint8_t i = 0u; i < fragments_to_remove; i++) {
#if defined(CONFIG_ZTEST)
        ret = mesh_range_report_batch_abort_test_queue_fault != NULL ?
                  mesh_range_report_batch_abort_test_queue_fault(
                      MESH_RANGE_REPORT_BATCH_ABORT_REMOVE_GET, i) :
                  0;
        if (ret == 0)
#endif
        {
            ret = k_msgq_get(&report_tx_msgq,
                             &report_tx_queue_rotation_scratch,
                             K_NO_WAIT);
        }
        if (ret != 0) {
            LOG_ERR("range report batch abort could not remove fragment %u: %d",
                    i, ret);
            anchor_range_report_batch_reservation.queue_error = ret;
            anchor_range_report_batch_reservation.queue_admission_fail_closed = true;
            app_watchdog_stop_feeding();
            k_mutex_unlock(&report_tx_queue_overflow_lock);
            return;
        }
        anchor_range_report_batch_reservation.rollback_scratch_owned = true;
#if defined(CONFIG_ZTEST)
        if (mesh_range_report_batch_abort_test_queue_fault != NULL) {
            (void)mesh_range_report_batch_abort_test_queue_fault(
                MESH_RANGE_REPORT_BATCH_ABORT_POST_GET, i);
        }
#endif
        if (!anchor_range_report_fragment_matches_control_locked(
                &anchor_range_report_batch_reservation.control,
                0u,
                &report_tx_queue_rotation_scratch)) {
            LOG_ERR("range report batch abort dequeued an unproven fragment %u",
                    i);
            anchor_range_report_batch_reservation.queue_error = -ESTALE;
            anchor_range_report_batch_reservation.queue_admission_fail_closed = true;
            app_watchdog_stop_feeding();
            k_mutex_unlock(&report_tx_queue_overflow_lock);
            return;
        }
        queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
        app_stack_workload_diag_click_release(
            &report_tx_queue_rotation_scratch.packet, 0,
            (uint16_t)queue_depth, 0u);
        app_stack_workload_diag_cir_release(
            &report_tx_queue_rotation_scratch.packet, 0,
            (uint16_t)queue_depth, 0u);
        if (anchor_range_report_batch_reservation.control.fragment_count > 1u) {
            memmove(
                &anchor_range_report_batch_reservation.control.fragments[0],
                &anchor_range_report_batch_reservation.control.fragments[1],
                (anchor_range_report_batch_reservation.control.fragment_count -
                 1u) *
                    sizeof(anchor_range_report_batch_reservation.control
                               .fragments[0]));
        }
        anchor_range_report_batch_reservation.control.fragment_count--;
        anchor_range_report_batch_reservation.queued_fragment_count--;
        memset(&anchor_range_report_batch_reservation.control.fragments[
                   anchor_range_report_batch_reservation.control.fragment_count],
               0,
               sizeof(anchor_range_report_batch_reservation.control
                          .fragments[0]));
        anchor_range_report_batch_reservation.rollback_scratch_owned = false;
    }
    /* Reconcile any independent recovery owner before resuming the frozen
     * prefix. Normally none can appear while the reservation is active. */
    ret = report_tx_queue_restore_recovery_locked();
    if (ret == 0) {
        ret = report_tx_queue_promote_local_locked();
        if (ret == -ENOENT) {
            ret = 0;
        }
    }
    if (ret != 0) {
        LOG_ERR("range report batch abort could not restore queue ownership: %d",
                ret);
        anchor_range_report_batch_reservation.queue_error = ret;
        anchor_range_report_batch_reservation.queue_admission_fail_closed = true;
        app_watchdog_stop_feeding();
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }
    memset(&anchor_range_report_batch_reservation, 0,
           sizeof(anchor_range_report_batch_reservation));
    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    recovery_pending = report_tx_queue_recovery_valid;
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (queue_count > 0u || recovery_pending) {
        report_tx_schedule(0u);
    }
#else
    ARG_UNUSED(clicker_id);
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
#endif
}

int queue_anchor_range_report_fragment(
    const struct mesh_outbound *outbound,
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    bool final_fragment)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint8_t fragment_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t queue_depth;
    int ret;

    if (outbound == NULL || clicker_id == 0u || event_seq == 0u) {
        return -EINVAL;
    }

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (!anchor_range_report_batch_matches_locked(clicker_id,
                                                  event_seq,
                                                  attempt_index) ||
        anchor_range_report_batch_reservation.queued_fragment_count >=
            anchor_range_report_batch_reservation.fragment_capacity) {
        ret = -ESTALE;
        goto out;
    }
    if (anchor_range_report_batch_reservation.queue_admission_fail_closed ||
        anchor_range_report_batch_reservation.rollback_scratch_owned) {
        ret = anchor_range_report_batch_reservation.queue_error != 0 ?
                  anchor_range_report_batch_reservation.queue_error :
                  -EIO;
        goto out;
    }
    ret = anchor_range_report_fragment_validate_locked(outbound,
                                                       clicker_id,
                                                       event_seq,
                                                       attempt_index);
    if (ret != 0) {
        goto out;
    }
    /*
     * This scratch is owned by the same queue mutex for abort preflight and
     * admission. Reusing it avoids nesting another 1 KiB mesh_outbound on the
     * range-builder workqueue stack.
     */
    report_tx_queue_rotation_scratch = *outbound;
    mesh_outbound_refresh_age(&report_tx_queue_rotation_scratch,
                              k_uptime_get_32());
    mesh_attach_paused_delivery_loss(
        &report_tx_queue_rotation_scratch, "queue-range-report-batch");
    if (!mesh_packet_semantic_digest(&report_tx_queue_rotation_scratch.packet,
                                     report_tx_queue_rotation_scratch.payload,
                                     report_tx_queue_rotation_scratch.payload_len,
                                     fragment_digest)) {
        ret = -EBADMSG;
        goto out;
    }
#if defined(CONFIG_ZTEST)
    ret = mesh_range_report_fragment_test_put_fault != NULL ?
              mesh_range_report_fragment_test_put_fault(
                  anchor_range_report_batch_reservation
                      .queued_fragment_count) :
              0;
    if (ret == 0)
#endif
    {
        ret = k_msgq_put(&report_tx_msgq,
                         &report_tx_queue_rotation_scratch,
                         K_NO_WAIT);
    }
    if (ret != 0) {
        /* K_NO_WAIT failure is a definite non-transfer: the caller still
         * owns this fragment and will abort the already-visible prefix. */
        ret = -EIO;
        anchor_range_report_batch_reservation.queue_error = ret;
        memset(&report_tx_queue_rotation_scratch, 0,
               sizeof(report_tx_queue_rotation_scratch));
        LOG_ERR("anchor range fragment queue admission failed before transfer: event=%u fragment=%u",
                event_seq,
                anchor_range_report_batch_reservation
                    .queued_fragment_count);
        goto out;
    }
    anchor_range_report_batch_reservation.control.fragments[
        anchor_range_report_batch_reservation.queued_fragment_count].seq =
        report_tx_queue_rotation_scratch.packet.seq;
    memcpy(anchor_range_report_batch_reservation.control.fragments[
               anchor_range_report_batch_reservation.queued_fragment_count]
               .digest,
           fragment_digest,
           SEMANTIC_DIGEST_SHA256_LEN);
    anchor_range_report_batch_reservation.control.fragment_count++;
    anchor_range_report_batch_reservation.queued_fragment_count++;
    queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    if (final_fragment) {
        anchor_range_report_ack_generation++;
        if (anchor_range_report_ack_generation == 0u) {
            anchor_range_report_ack_generation = 1u;
        }
        anchor_range_report_ack_runtime =
            (struct anchor_range_report_ack_runtime) {
                .control =
                    anchor_range_report_batch_reservation.control,
                .generation = anchor_range_report_ack_generation,
                .active = true,
            };
        memset(&anchor_range_report_batch_reservation, 0,
               sizeof(anchor_range_report_batch_reservation));
        (void)report_tx_queue_promote_local_locked();
    }
    ret = 0;

out:
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (ret != 0) {
        return ret;
    }

    if (final_fragment && !anchor_uwb_window_active()) {
        report_tx_schedule(0u);
    }
    return ret;
#else
    ARG_UNUSED(outbound);
    ARG_UNUSED(clicker_id);
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(final_fragment);
    return 0;
#endif
}

int mesh_anchor_range_report_note_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint64_t retired_clicker_id = 0u;
    uint64_t retired_generation = 0u;
    uint32_t retired_event_seq = 0u;
    uint16_t all_acknowledged;
    uint8_t fragment_index = 0u;
    uint8_t retired_attempt_index = 0u;
    bool retire = false;
    int ret;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (!anchor_range_report_ack_runtime.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return 0;
    }
    ret = anchor_range_report_fragment_digest_match_locked(
        packet, semantic_digest, &fragment_index);
    if (ret <= 0) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return ret;
    }
    anchor_range_report_ack_runtime.acknowledged_mask |=
        (uint16_t)(UINT16_C(1) << fragment_index);
    all_acknowledged = anchor_range_report_all_acknowledged_mask(
        anchor_range_report_ack_runtime.control.fragment_count);
    if (all_acknowledged != 0u &&
        anchor_range_report_ack_runtime.acknowledged_mask ==
            all_acknowledged) {
        LOG_INF("anchor range ACK ledger retired after all gateway ACKs: event=%u fragments=%u",
                anchor_range_report_ack_runtime.control.event_seq,
                anchor_range_report_ack_runtime.control.fragment_count);
        retired_clicker_id =
            anchor_range_report_ack_runtime.control.clicker_id;
        retired_event_seq =
            anchor_range_report_ack_runtime.control.event_seq;
        retired_attempt_index =
            anchor_range_report_ack_runtime.control.attempt_index;
        retired_generation = anchor_range_report_ack_runtime.generation;
        retire = true;
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (!retire) {
        return 0;
    }
    ret = app_anchor_click_event_runtime_custody_released(
        retired_clicker_id,
        retired_event_seq,
        retired_attempt_index,
        k_uptime_get_32(),
        NULL);
    if (ret < 0) {
        LOG_ERR("anchor range lifecycle rejected gateway-confirmed custody release: event=%u attempt=%u ret=%d",
                retired_event_seq,
                retired_attempt_index,
                ret);
        return ret;
    }
#if defined(CONFIG_ZTEST)
    if (mesh_range_report_test_after_lifecycle_release_hook != NULL) {
        mesh_range_report_test_after_lifecycle_release_hook();
    }
#endif
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_ack_generation_matches(
            &anchor_range_report_ack_runtime, retired_generation)) {
        memset(&anchor_range_report_ack_runtime, 0,
               sizeof(anchor_range_report_ack_runtime));
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(semantic_digest);
    return 0;
#endif
}

int mesh_anchor_range_report_note_terminal_release(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t fragment_index = 0u;
    uint64_t retired_clicker_id;
    uint64_t retired_generation;
    uint32_t retired_event_seq;
    uint8_t retired_attempt_index;
    int ret;

    if (packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->payload_len != payload_len ||
        !mesh_packet_semantic_digest(packet,
                                     payload,
                                     payload_len,
                                     semantic_digest)) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (!anchor_range_report_ack_runtime.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return 0;
    }
    ret = anchor_range_report_fragment_digest_match_locked(
        packet, semantic_digest, &fragment_index);
    if (ret <= 0) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return ret;
    }

    /*
     * The fragments form one range-report batch. Once any exact fragment
     * reaches a terminal transport outcome, the batch can no longer promise
     * complete replay, so retire it in RAM instead of letting the expired
     * fragment resurrect (there is no flash copy anymore).
     */
    LOG_WRN("anchor range ACK ledger retired after terminal fragment: event=%u index=%u",
            anchor_range_report_ack_runtime.control.event_seq,
            fragment_index);
    anchor_range_report_ack_runtime.acknowledged_mask =
        anchor_range_report_all_acknowledged_mask(
            anchor_range_report_ack_runtime.control.fragment_count);
    retired_clicker_id = anchor_range_report_ack_runtime.control.clicker_id;
    retired_event_seq = anchor_range_report_ack_runtime.control.event_seq;
    retired_attempt_index =
        anchor_range_report_ack_runtime.control.attempt_index;
    retired_generation = anchor_range_report_ack_runtime.generation;
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    ret = app_anchor_click_event_runtime_custody_released(
        retired_clicker_id,
        retired_event_seq,
        retired_attempt_index,
        k_uptime_get_32(),
        NULL);
    if (ret < 0) {
        LOG_ERR("anchor range lifecycle rejected terminal custody release: event=%u attempt=%u ret=%d",
                retired_event_seq,
                retired_attempt_index,
                ret);
        return ret;
    }
#if defined(CONFIG_ZTEST)
    if (mesh_range_report_test_after_lifecycle_release_hook != NULL) {
        mesh_range_report_test_after_lifecycle_release_hook();
    }
#endif
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_ack_generation_matches(
            &anchor_range_report_ack_runtime, retired_generation)) {
        memset(&anchor_range_report_ack_runtime, 0,
               sizeof(anchor_range_report_ack_runtime));
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return 0;
#endif
}

bool mesh_report_tx_backlog_active(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    bool range_batch_active = false;

    if (k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER) == 0) {
        range_batch_active = anchor_range_report_batch_reservation.active;
        k_mutex_unlock(&report_tx_queue_overflow_lock);
    }
    return range_batch_active || mesh_report_delivery_active() ||
           app_mesh_report_encode_cir_pending() ||
           report_tx_queue_used() > 0u;
#else
    return false;
#endif
}

bool mesh_report_ch9_ack_wait_active(void)
{
    bool relay_tx_active = mesh_relay_tx_active(&mesh_runtime);

    return app_mesh_ch9_core_ack_wait_active(&mesh_runtime.pending,
                                             relay_tx_active);
}

bool mesh_report_local_protocol_response_active(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    const struct mesh_pending_tx *pending = &mesh_runtime.pending;

    if (!mesh_relay_tx_active(&mesh_runtime) ||
        pending->packet.src_id != DEVICE_ID ||
        pending->packet.dst_id != GATEWAY_ID ||
        (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    return pending->packet.msg_type == MSG_COMMAND_RESULT;
#else
    return false;
#endif
}

static int mesh_queue_anchor_cir_fragment(struct mesh_outbound *outbound,
                                          uint32_t *queue_depth)
{
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    mesh_outbound_refresh_age(outbound, k_uptime_get_32());
    mesh_attach_paused_delivery_loss(outbound, "queue-anchor-cir-fragment");
    ret = report_tx_queue_append(outbound);
    if (ret != 0) {
        return -ENOSPC;
    }
    if (queue_depth != NULL) {
        *queue_depth = report_tx_queue_used();
    }
    return 0;
#else
    ARG_UNUSED(outbound);
    ARG_UNUSED(queue_depth);
    return -ENOTSUP;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void report_tx_work_handler(struct k_work *work)
{
    struct fw_radio_activity_decision coordinator_decision;
    bool anchor_busy;
    bool ch9_ack_send_pending;
    bool report_queue_pending;
    bool rx_queue_pending;
    bool route_handoff_active;
    struct app_mesh_tx_handoff_result handoff_result;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER) == 0) {
        bool range_batch_active;

        range_batch_active =
            anchor_range_report_batch_reservation.active;
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        if (range_batch_active) {
            report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            return;
        }
    }
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    ret = app_mesh_report_encode_queue_next_cir();
    if (ret < 0 && ret != -ENOENT && ret != -ENOSPC) {
        LOG_WRN("anchor partial CIR report refill failed: %d", ret);
    }
#endif
    anchor_busy = anchor_uwb_window_active();
    ch9_ack_send_pending =
        app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table);
    report_queue_pending = report_tx_queue_used() > 0u || mesh_report_delivery_active();
    rx_queue_pending = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                        k_msgq_num_used_get(&mesh_rx_msgq) > 0u;
    route_handoff_active = mesh_route_reply_handoff_active();
    mesh_coordinator_decide_now("report-tx", &coordinator_decision);
    if (anchor_busy || !coordinator_decision.report_tx_allowed ||
        route_handoff_active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            report_queue_pending) {
            /* Each underlying owner logs its own lifecycle. Repeating the
             * selected busy reason here can starve the useful packet trace. */
            if (mesh_tx_handoff_gate_yields(
                    APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
                    report_queue_pending,
                    route_handoff_active,
                    rx_queue_pending,
                    &handoff_result)) {
                return;
            }
            if (ch9_ack_send_pending) {
                (void)mesh_schedule_uwb_rx(0u);
            }
            report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        }
        return;
    }

    ret = mesh_report_delivery_step();
    if (ret == -EBUSY) {
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
    } else if (ret < 0 && ret != -ENOENT) {
        /* Valid report bytes remain owned even if their adapter fails. */
        LOG_ERR("report delivery deferred: %d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
    }

}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
/* Caller holds report_tx_queue_overflow_lock. */
static int report_tx_queue_contains_semantic_locked(
    const struct mesh_outbound *outbound,
    bool *contains)
{
    uint32_t queue_count;

    if (outbound == NULL || contains == NULL) {
        return -EINVAL;
    }
    *contains = mesh_report_delivery_contains(outbound);
    if (*contains) {
        return 0;
    }
    if (report_tx_queue_recovery_valid &&
        mesh_preempt_outbound_matches(
            &report_tx_queue_overflow_dropped, outbound)) {
        *contains = true;
        return 0;
    }

    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    for (uint32_t i = 0u; i < queue_count; i++) {
        if (k_msgq_peek_at(&report_tx_msgq,
                           &report_tx_queue_rotation_scratch,
                           i) != 0) {
            return -EIO;
        }
        if (mesh_preempt_outbound_matches(
                &report_tx_queue_rotation_scratch, outbound)) {
            *contains = true;
            return 0;
        }
    }
    return 0;
}
#endif

int queue_anchor_report(const struct mesh_outbound *outbound)
{
    if (outbound == NULL) {
        return -EINVAL;
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound queued;
    struct fw_delivery_loss_store_result drop_result = {0};
    uint32_t queue_depth = 0u;
    bool admitted = false;
    bool duplicate_owned = false;
    bool owner_active_at_failure = false;
    bool queued_local_priority;
    int ret;

    queued = *outbound;
    /* A relay starts a new physical attempt, with its own backlog/credit. */
    if (mesh_outbound_clear_ch9_batch_metadata(&queued) != PROTO_OK) {
        return -EBADMSG;
    }
    mesh_outbound_refresh_age(&queued, k_uptime_get_32());
    mesh_attach_paused_delivery_loss(&queued, "queue-anchor-report");
    queued_local_priority = mesh_outbound_is_local_origin_priority(&queued);

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        fw_delivery_loss_note_drop(&mesh_delivery_loss, &drop_result);
        return -ENOSPC;
    }

    if (anchor_range_report_batch_reservation.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return -EAGAIN;
    }
    (void)report_tx_queue_restore_recovery_locked();
    ret = report_tx_queue_contains_semantic_locked(&queued,
                                                   &duplicate_owned);
    if (ret != 0) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        app_watchdog_stop_feeding();
        return -EIO;
    }
    if (duplicate_owned) {
        admitted = true;
    } else if (!report_tx_queue_recovery_valid &&
        k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT) == 0) {
        admitted = true;
        if (queued_local_priority) {
            (void)report_tx_queue_promote_local_locked();
        }
    } else if (report_tx_queue_recovery_valid ||
               app_mesh_queue_head_owned(&report_tx_queue_head_owner)) {
        enum app_mesh_queue_reserve_action reserve_action =
            app_mesh_queue_reserve_decide(
                report_tx_queue_recovery_valid,
                report_tx_queue_recovery_valid &&
                    mesh_outbound_is_local_origin_priority(
                        &report_tx_queue_overflow_dropped),
                queued_local_priority);

        /* The immutable head and recovery slot each remain single owners. */
        if (reserve_action == APP_MESH_QUEUE_RESERVE_ADMIT) {
            report_tx_queue_overflow_dropped = queued;
            report_tx_queue_recovery_valid = true;
            admitted = true;
        }
    } else if (queued_local_priority) {
        /*
         * Make room for local work by transferring one queued transit packet
         * to the recovery owner.  No packet is released or counted lost.
         */
        ret = -ENOSPC;
        for (uint32_t i = 0u;
             i < REPORT_TX_QUEUE_DEPTH &&
             k_msgq_get(&report_tx_msgq,
                        &report_tx_queue_overflow_dropped,
                        K_NO_WAIT) == 0;
             i++) {
            if (!mesh_outbound_is_local_origin_priority(
                    &report_tx_queue_overflow_dropped)) {
                ret = 0;
                break;
            }
            ret = k_msgq_put(&report_tx_msgq,
                             &report_tx_queue_overflow_dropped,
                             K_NO_WAIT);
            if (ret != 0) {
                (void)mesh_preempt_report_queue_recover(
                    &report_tx_queue_overflow_dropped,
                    &report_tx_msgq);
                break;
            }
            ret = -ENOSPC;
        }
        if (ret == 0) {
            ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
            if (ret == 0) {
                report_tx_queue_recovery_valid = true;
                admitted = true;
                (void)report_tx_queue_promote_local_locked();
            } else {
                (void)mesh_preempt_report_queue_recover(
                    &report_tx_queue_overflow_dropped,
                    &report_tx_msgq);
            }
        }
    }

    queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq) +
                  (report_tx_queue_recovery_valid ? 1u : 0u);
    owner_active_at_failure =
        app_mesh_queue_head_owned(&report_tx_queue_head_owner);
    k_mutex_unlock(&report_tx_queue_overflow_lock);

    if (!admitted) {
        fw_delivery_loss_note_drop(&mesh_delivery_loss, &drop_result);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_REPORT_QUEUE_FULL lost=%u ret=%d owner=%u\n",
                                drop_result.lost_count,
                                ret,
                                owner_active_at_failure ? 1u : 0u);
        }
        LOG_WRN("anchor report queue full; gateway-bound report dropped: lost=%u",
                drop_result.lost_count);
        return -ENOSPC;
    }

    if (queued.packet.dst_id == GATEWAY_ID &&
        queued.packet.src_id != DEVICE_ID &&
        (queued.packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
        int reverse_route_ret =
            mesh_relay_commit_queued_transit_reverse_route(
                &mesh_runtime,
                &queued,
                queued.ingress_previous_hop_id,
                k_uptime_get_32());

        if (reverse_route_ret != PROTO_OK) {
            LOG_ERR("queued transit reverse route commit failed: src=0x%016llx prev=0x%016llx ret=%d",
                    (unsigned long long)queued.packet.src_id,
                    (unsigned long long)queued.ingress_previous_hop_id,
                    reverse_route_ret);
            app_watchdog_stop_feeding();
        } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_TRANSIT_REVERSE_ROUTE_COMMIT src=0x%llx session=%u seq=%u prev=0x%llx owner=queue admission=%s\n",
                (unsigned long long)queued.packet.src_id,
                queued.packet.session_id,
                queued.packet.seq,
                (unsigned long long)queued.ingress_previous_hop_id,
                duplicate_owned ? "existing" : "new");
        }
    }

    if ((queued.packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
        uint16_t click_queue_depth = (uint16_t)queue_depth;

        app_stack_workload_diag_click_admit(&queued.packet, click_queue_depth,
                                            click_queue_depth);
        /* Admission retains the new click beside any earlier report custody. */
        app_stack_workload_diag_click_sample(&queued.packet, click_queue_depth,
                                             click_queue_depth);
    }
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x src=0x%016llx session=%u seq=%u admission=%s earliest_tx_ms=%u queue_depth=%u",
            queued.packet.msg_type,
            (unsigned long long)queued.packet.src_id,
            queued.packet.session_id,
            queued.packet.seq,
            duplicate_owned ? "existing" : "new",
            queued.earliest_tx_ms,
            queue_depth);
    if (!anchor_uwb_window_active()) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t delay_ms = mesh_outbound_ready_for_tx(&queued, now_ms) ?
                            0u :
                            uptime_ms_until_deadline(now_ms, queued.earliest_tx_ms);

        report_tx_schedule(delay_ms);
    }
    return 0;
#else
    return 0;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
struct mesh_preempt_queue_admission {
    uint8_t displaced_after_count;
    bool transit_displaced;
};

/* Caller holds report_tx_queue_overflow_lock. */
static int mesh_preempt_remove_appended_click_locked(
    const struct mesh_outbound *outbound)
{
    uint32_t queue_count;

    if (outbound == NULL) {
        return -EINVAL;
    }

    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    for (uint32_t i = 0u; i < queue_count; i++) {
        int ret = k_msgq_get(&report_tx_msgq,
                             &report_tx_queue_rotation_scratch,
                             K_NO_WAIT);

        if (ret != 0) {
            return ret;
        }
        if (i + 1u == queue_count) {
            if (mesh_preempt_click_handoff_matches(
                    &report_tx_queue_rotation_scratch, outbound)) {
                return 0;
            }
            ret = k_msgq_put(&report_tx_msgq,
                             &report_tx_queue_rotation_scratch,
                             K_NO_WAIT);
            return ret == 0 ? -ESTALE : ret;
        }
        ret = k_msgq_put(&report_tx_msgq,
                         &report_tx_queue_rotation_scratch,
                         K_NO_WAIT);
        if (ret != 0) {
            /* A slot was just removed while this mutex was held. */
            return ret;
        }
    }
    return -ESTALE;
}

/* Caller holds report_tx_queue_overflow_lock. */
static int mesh_preempt_rotate_queue_left_locked(uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        int ret = k_msgq_get(&report_tx_msgq,
                             &report_tx_queue_rotation_scratch,
                             K_NO_WAIT);

        if (ret != 0) {
            return ret;
        }
        ret = k_msgq_put(&report_tx_msgq,
                         &report_tx_queue_rotation_scratch,
                         K_NO_WAIT);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

/* Caller holds report_tx_queue_overflow_lock. */
static int mesh_preempt_restore_displaced_transit_locked(
    const struct mesh_preempt_queue_admission *admission)
{
    int ret;

    if (admission == NULL || !admission->transit_displaced) {
        return 0;
    }
    ret = report_tx_queue_restore_recovery_locked();
    if (ret != 0) {
        return ret;
    }
    /* Admission rotates the prefix before the displaced transit to the tail.
     * After appending that transit again, rotate the original suffix back to
     * the tail so a defensive cancel mismatch restores byte-for-byte FIFO. */
    return mesh_preempt_rotate_queue_left_locked(
        admission->displaced_after_count);
}

/* Caller holds report_tx_queue_overflow_lock. */
static int mesh_preempt_rollback_admission_locked(
    const struct mesh_outbound *outbound,
    const struct mesh_preempt_queue_admission *admission)
{
    int ret;

    ret = mesh_preempt_remove_appended_click_locked(outbound);
    if (ret != 0) {
        return ret;
    }
    return mesh_preempt_restore_displaced_transit_locked(admission);
}

/* Caller holds report_tx_queue_overflow_lock. */
static int mesh_preempt_admit_click_locked(
    const struct mesh_outbound *outbound,
    struct mesh_preempt_queue_admission *admission)
{
    int ret;

    if (outbound == NULL || admission == NULL) {
        return -EINVAL;
    }
    memset(admission, 0, sizeof(*admission));

    if (report_tx_queue_recovery_valid ||
        app_mesh_queue_head_owned(&report_tx_queue_head_owner)) {
        return -EBUSY;
    }

    ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
    if (ret == 0) {
        return 0;
    }
    if (k_msgq_num_used_get(&report_tx_msgq) != REPORT_TX_QUEUE_DEPTH) {
        return ret;
    }

    /*
     * The full queue can make room only by retaining one transit record in
     * the existing recovery owner.  Do not overwrite an occupied recovery
     * owner, and do not rotate around an in-flight immutable head.
     */
    for (uint32_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        ret = k_msgq_get(&report_tx_msgq,
                         &report_tx_queue_overflow_dropped,
                         K_NO_WAIT);
        if (ret != 0) {
            return ret;
        }
        if (!mesh_outbound_is_local_origin_priority(
                &report_tx_queue_overflow_dropped)) {
            report_tx_queue_recovery_valid = true;
            admission->transit_displaced = true;
            admission->displaced_after_count =
                (uint8_t)(REPORT_TX_QUEUE_DEPTH - i - 1u);
            ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
            if (ret == 0) {
                return 0;
            }
            /* The active relay still owns the click; restore transit first. */
            {
                int restore_ret =
                    mesh_preempt_restore_displaced_transit_locked(admission);

                return restore_ret == 0 ? ret : restore_ret;
            }
        }
        ret = k_msgq_put(&report_tx_msgq,
                         &report_tx_queue_overflow_dropped,
                         K_NO_WAIT);
        if (ret != 0) {
            return ret;
        }
    }
    return -ENOSPC;
}

/*
 * This is the one production local-click transfer point.  It intentionally
 * does not use queue_anchor_report(): that producer reports an ordinary
 * capacity rejection as delivery loss, while this operation must leave the
 * live relay record authoritative on every failed handoff.
 */
static int mesh_preempt_transfer_click_report_atomic(
    void *ctx,
    const struct mesh_outbound *outbound)
{
    struct mesh_preempt_queue_admission admission = {0};
    int ret;

    ARG_UNUSED(ctx);
    if (outbound == NULL || outbound->packet.msg_type != MSG_CLICK_REPORT ||
        outbound->packet.src_id != DEVICE_ID) {
        return -EINVAL;
    }

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT);
    if (ret != 0) {
        return -EBUSY;
    }
    if (anchor_range_report_batch_reservation.active) {
        ret = -EAGAIN;
        goto unlock;
    }
    if (report_tx_queue_recovery_valid ||
        app_mesh_queue_head_owned(&report_tx_queue_head_owner)) {
        ret = -EBUSY;
        goto unlock;
    }
    if (!mesh_preempt_live_click_matches(outbound)) {
        ret = -ESTALE;
        goto unlock;
    }

    ret = mesh_preempt_admit_click_locked(outbound, &admission);
    if (ret != 0) {
        goto unlock;
    }

    /* Revalidate after admission; this is the sole release of relay custody. */
#if defined(CONFIG_ZTEST)
    if (mesh_preempt_test_before_cancel_hook != NULL) {
        mesh_preempt_test_before_cancel_hook();
    }
#endif
    if (mesh_relay_cancel_tx_if_matches(&mesh_runtime, outbound) != PROTO_OK) {
        int undo_ret = mesh_preempt_rollback_admission_locked(outbound,
                                                               &admission);
        if (undo_ret != 0) {
            LOG_ERR("click preemption queue rollback failed after stale relay: %d",
                    undo_ret);
            app_watchdog_stop_feeding();
            ret = undo_ret;
        } else {
            ret = -ESTALE;
        }
        goto unlock;
    }

unlock:
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (ret == 0) {
        int timeout_ret = mesh_schedule_tx_timeout();

        if (timeout_ret < 0) {
            /* The queue now owns the click, but a different shared timeout
             * owner may be asleep. Never undo this committed handoff; fail
             * closed instead of blindly canceling the multiplexed work item. */
            LOG_ERR("click preemption could not reconcile shared timeout: %d",
                    timeout_ret);
            app_watchdog_stop_feeding();
        }
        if (!anchor_uwb_window_active() && !anchor_click_window_active()) {
            report_tx_schedule(0u);
        }
    }
    return ret;
}
#endif

static bool mesh_send_route_reply_outbound_action(const struct mesh_outbound *route_reply,
                                                  bool backup_valid,
                                                  uint64_t backup_next_hop_id,
                                                  const char *reason)
{
    uint64_t route_reply_acked_next_hop = 0u;
    int ret;

    if (route_reply == NULL) {
        return false;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_ACTION reason=%s next=0x%llx seq=%u backup=%u\n",
                            reason == NULL ? "route-reply" : reason,
                            (unsigned long long)route_reply->next_hop_id,
                            route_reply->packet.seq,
                            backup_valid ? 1u : 0u);
    }
    mesh_route_embedded_wait_before_reply(route_reply);
    if (route_reply->earliest_tx_valid) {
        uint32_t now_ms = k_uptime_get_32();

        if (!uptime_deadline_reached(now_ms,
                                     route_reply->earliest_tx_ms)) {
            uint32_t delay_ms = uptime_ms_until_deadline(
                now_ms, route_reply->earliest_tx_ms);

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REPLY_ETA_WAIT delay=%u earliest=%u now=%u\n",
                                    delay_ms,
                                    route_reply->earliest_tx_ms,
                                    now_ms);
            }
            mesh_wait_until_ms(route_reply->earliest_tx_ms);
        }
    }
    if (mesh_id_is_unicast(route_reply->next_hop_id)) {
        mesh_c5_contact_exchange(route_reply->next_hop_id,
                                 C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                 k_uptime_get_32() +
                                 MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS,
                                 "route-reply-action");
    }
    ret = mesh_send_route_reply_train(route_reply,
                                      backup_valid,
                                      backup_next_hop_id,
                                      &route_reply_acked_next_hop);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_ACTION_DONE ret=%d ack=0x%llx reason=%s\n",
                            ret,
                            (unsigned long long)route_reply_acked_next_hop,
                            reason == NULL ? "route-reply" : reason);
    }
    if (ret == 0 && mesh_id_is_unicast(route_reply_acked_next_hop)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            LOG_INF("mesh route reply sent; waiting for route origin to propose channel-9 event: next=0x%016llx",
                    (unsigned long long)route_reply_acked_next_hop);
        } else {
            (void)mesh_propose_event_after_channel5_contact(route_reply_acked_next_hop,
                                                            "route-reply-event-propose");
        }
        return true;
    }

    return false;
}

static bool mesh_send_route_reply_action(const struct mesh_relay_result *result,
                                         const char *reason)
{
    if (result == NULL ||
        (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) == 0u) {
        return false;
    }

    return mesh_send_route_reply_outbound_action(
        &result->route_reply,
        result->route_reply_backup_valid,
        result->route_reply_backup_next_hop_id,
        reason);
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_execute_route_request_action(uint64_t previous_hop_id,
                                              uint32_t reply_deadline_ms)
{
    struct mesh_outbound *route_req = &mesh_route_request_action_tx;
    struct mesh_outbound *route_reply = &mesh_route_request_reply_tx;
    struct mesh_route_capture_identity route_identity = {0};
    uint64_t target_id = GATEWAY_ID;
    uint32_t route_reply_window_ms;
    bool embedded_route_sent = false;
    bool handled = false;
    int ret;

    if (!mesh_payload_find_u64(route_req->payload,
                               route_req->payload_len,
                               TLV_RESPONDER_ID,
                               &target_id)) {
        target_id = GATEWAY_ID;
    }
    if (!mesh_route_capture_identity_from_request(route_req,
                                                  target_id,
                                                  &route_identity)) {
        LOG_ERR("mesh route-request action lacks capture identity: src=0x%016llx target=0x%016llx session=%u",
                (unsigned long long)route_req->packet.src_id,
                (unsigned long long)target_id,
                route_req->packet.session_id);
        goto out;
    }

    {
        uint32_t now_ms = k_uptime_get_32();

        if (uptime_deadline_reached(now_ms, reply_deadline_ms)) {
            LOG_WRN("mesh route-request action expired before forwarding: src=0x%016llx target=0x%016llx session=%u",
                    (unsigned long long)route_req->packet.src_id,
                    (unsigned long long)target_id,
                    route_req->packet.session_id);
            goto out;
        }
        route_reply_window_ms =
            uptime_ms_until_deadline(now_ms, reply_deadline_ms);
    }

    ret = mesh_try_direct_gateway_route_probe(target_id,
                                              "route-request-rebroadcast-probe",
                                              true,
                                              APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,
                                              0u);
    if (ret != 0 && k_msgq_num_used_get(&mesh_rx_msgq) != 0u) {
        /*
         * The direct probe captured another owner's causal frame.  This
         * worker cleared its pending bit before entering here, so restore the
         * exact route-request action before yielding; otherwise the queued RX
         * would drain but the rebroadcast owner would be lost.
         */
        mesh_route_request_action_pending = true;
        (void)mesh_reschedule_owned_work(
            &mesh_route_request_action_work,
            REPORT_TX_RETRY_DELAY_MS,
            "route-request-action-rx-yield");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_ROUTE_REQ_REBROADCAST_RX_YIELD q=%u seq=%u\n",
                k_msgq_num_used_get(&mesh_rx_msgq),
                route_req->packet.seq);
        }
        goto out;
    }
    if (ret == 0 && mesh_id_is_unicast(previous_hop_id)) {
        memset(route_reply, 0, sizeof(*route_reply));

        ret = mesh_relay_build_route_reply_for_request(&mesh_runtime,
                                                       &route_req->packet,
                                                       route_req->payload,
                                                       route_req->payload_len,
                                                       previous_hop_id,
                                                       k_uptime_get_32(),
                                                       sys_rand32_get(),
                                                       route_reply);
        if (ret == PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_DIRECT_REPLY next=0x%llx seq=%u\n",
                                    (unsigned long long)route_reply->next_hop_id,
                                    route_reply->packet.seq);
            }
            handled = mesh_send_route_reply_outbound_action(route_reply,
                                                            false,
                                                            0u,
                                                            "route-request-direct-probe-reply");
            goto out;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_DIRECT_REPLY_FAIL ret=%d\n",
                                ret);
        }
    }

    ret = mesh_send_route_wake_train(MESH_BROADCAST_ID,
                                     route_req,
                                     &embedded_route_sent,
                                     C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                     "route-request-rebroadcast",
                                     route_req,
                                     NULL,
                                     FW_C5_TX_INTENT_BACKGROUND,
                                     NULL,
                                     NULL);
    if (ret < 0) {
        LOG_WRN("mesh route-request rebroadcast wake train failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        mesh_restart_role_scan();
        goto out;
    }

    if (embedded_route_sent) {
        bool route_reply_captured = false;
        int listen_ret;
        uint32_t now_ms = k_uptime_get_32();

        if (uptime_deadline_reached(now_ms, reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                now_ms, reply_deadline_ms);
            listen_ret = mesh_listen_for_route_reply(
                target_id,
                "embedded-route-request-rebroadcast",
                route_reply_window_ms,
                &route_identity,
                &route_reply_captured);
        }

        if (route_reply_captured) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_REQ_REBROADCAST_EMBEDDED_REPLY\n");
            }
            handled = true;
            goto out;
        }
        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh embedded rebroadcast route-reply listen failed: target=0x%016llx ret=%d",
                    (unsigned long long)target_id,
                    listen_ret);
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_TURNAROUND delay=%u\n",
                            MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
        k_msleep(MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
    }

    ret = mesh_send_outbound(route_req, "route-request-rebroadcast-control");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_TX ret=%d seq=%u ttl=%u\n",
                            ret,
                            route_req->packet.seq,
                            route_req->packet.ttl);
    }
    if (ret == 0) {
        bool route_reply_captured = false;
        int listen_ret;
        uint32_t now_ms = k_uptime_get_32();

        if (uptime_deadline_reached(now_ms, reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                now_ms, reply_deadline_ms);
            listen_ret = mesh_listen_for_route_reply(
                target_id,
                "route-request-rebroadcast",
                route_reply_window_ms,
                &route_identity,
                &route_reply_captured);
        }

        if (route_reply_captured && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REQ_REBROADCAST_REPLY_CAPTURED\n");
        } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_REPLY_MISS listen_ret=%d win=%u\n",
                                listen_ret,
                                route_reply_window_ms);
        }
        handled = route_reply_captured || listen_ret == 0;
        goto out;
    }

    LOG_WRN("mesh route-request rebroadcast control TX failed: target=0x%016llx ret=%d",
            (unsigned long long)target_id,
            ret);

out:
    return handled;
}

static bool mesh_route_request_action_same_identity(
    const struct mesh_outbound *left,
    const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == MSG_ROUTE_REQ &&
           right->packet.msg_type == MSG_ROUTE_REQ &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq;
}

static bool mesh_schedule_route_request_action(
    const struct mesh_relay_result *result,
    const struct mesh_rx_pending *rx,
    bool update_only)
{
    const struct mesh_outbound *next;
    struct app_mesh_route_request_defer_decision defer_decision;
    struct app_mesh_route_request_defer_state defer_state;
    uint32_t delay_ms;
    uint32_t now_ms;
    bool same_identity;
    int ret;

    if (result == NULL || rx == NULL ||
        !mesh_id_is_unicast(rx->previous_hop_id)) {
        return false;
    }
    if (update_only) {
        if ((result->actions & MESH_RELAY_ACTION_UPDATE_ROUTE_REQ) == 0u) {
            return false;
        }
    } else if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) == 0u) {
        return false;
    }
    next = &result->route_request;

    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    same_identity = mesh_route_request_action_pending &&
        mesh_route_request_action_same_identity(&mesh_route_request_action_tx,
                                                next);
    defer_state = (struct app_mesh_route_request_defer_state) {
        .pending_due_ms = mesh_route_request_action_tx.earliest_tx_ms,
        .requested_due_ms = next->earliest_tx_ms,
        .pending_reply_deadline_ms =
            mesh_route_request_action_reply_deadline_ms,
        .requested_reply_deadline_ms =
            rx->received_at_ms + mesh_route_reply_listen_window_ms(next->packet.ttl),
        .pending = mesh_route_request_action_pending,
        .same_identity = same_identity,
        .update_only = update_only,
    };
    app_mesh_route_request_defer_decide(&defer_state, &defer_decision);
    if (defer_decision.action == APP_MESH_ROUTE_REQUEST_DEFER_REJECT) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_DEFER_REJECT update=%u same=%u src=0x%llx seq=%u\n",
                                update_only ? 1u : 0u,
                                same_identity ? 1u : 0u,
                                (unsigned long long)next->packet.src_id,
                                next->packet.seq);
            status_debug_printf("DBG_ROUTE_REQ_DEFER_PENDING src=0x%llx seq=%u session=%u due=%u deadline=%u prev=0x%llx\n",
                                (unsigned long long)mesh_route_request_action_tx.packet.src_id,
                                mesh_route_request_action_tx.packet.seq,
                                mesh_route_request_action_tx.packet.session_id,
                                mesh_route_request_action_tx.earliest_tx_ms,
                                mesh_route_request_action_reply_deadline_ms,
                                (unsigned long long)mesh_route_request_action_previous_hop_id);
            status_debug_printf("DBG_ROUTE_REQ_DEFER_REQUEST due=%u deadline=%u now=%u\n",
                                next->earliest_tx_ms,
                                defer_state.requested_reply_deadline_ms,
                                k_uptime_get_32());
        }
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return false;
    }

    mesh_route_request_action_tx = *next;
    mesh_route_request_action_tx.earliest_tx_ms = defer_decision.due_ms;
    mesh_route_request_action_tx.earliest_tx_valid = true;
    mesh_route_request_action_previous_hop_id = rx->previous_hop_id;
    mesh_route_request_action_reply_deadline_ms =
        defer_decision.reply_deadline_ms;
    mesh_route_request_action_pending = true;
    now_ms = k_uptime_get_32();
    delay_ms = app_mesh_route_request_defer_delay_ms(
        now_ms,
        mesh_route_request_action_tx.earliest_tx_ms);
    ret = mesh_reschedule_owned_work(&mesh_route_request_action_work,
                                     delay_ms,
                                     "route-request-action");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DEFER update=%u delay=%u earliest=%u now=%u ret=%d src=0x%llx seq=%u\n",
                            update_only ? 1u : 0u,
                            delay_ms,
                            mesh_route_request_action_tx.earliest_tx_ms,
                            now_ms,
                            ret,
                            (unsigned long long)next->packet.src_id,
                            next->packet.seq);
    }
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
    return mesh_route_request_action_pending;
}

static void mesh_route_request_action_work_handler(struct k_work *work)
{
    uint32_t delay_ms;
    uint32_t now_ms;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DEFER_WORK pending=%u seq=%u due=%u deadline=%u now=%u\n",
                            mesh_route_request_action_pending ? 1u : 0u,
                            mesh_route_request_action_tx.packet.seq,
                            mesh_route_request_action_tx.earliest_tx_ms,
                            mesh_route_request_action_reply_deadline_ms,
                            k_uptime_get_32());
    }
    if (!mesh_route_request_action_pending) {
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return;
    }
    now_ms = k_uptime_get_32();
    delay_ms = app_mesh_route_request_defer_delay_ms(
        now_ms,
        mesh_route_request_action_tx.earliest_tx_ms);
    if (delay_ms > 0u) {
        (void)mesh_reschedule_owned_work(&mesh_route_request_action_work,
                                         delay_ms,
                                         "route-request-action-delay");
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return;
    }

    mesh_route_request_action_pending = false;
    (void)mesh_execute_route_request_action(
        mesh_route_request_action_previous_hop_id,
        mesh_route_request_action_reply_deadline_ms);
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
}
#else
static bool mesh_schedule_route_request_action(
    const struct mesh_relay_result *result,
    const struct mesh_rx_pending *rx,
    bool update_only)
{
    ARG_UNUSED(result);
    ARG_UNUSED(rx);
    ARG_UNUSED(update_only);
    return false;
}
#endif

/* Every forwarded report enters the same custody engine, including an idle
 * relay's first packet to another anchor and a packet awaiting a route. */
static bool mesh_forward_uses_gateway_batch_queue(const struct mesh_outbound *out)
{
    return out != NULL &&
           mesh_relay_packet_can_queue_gateway_report(&mesh_runtime, &out->packet);
}

static bool mesh_forward_uses_child_gateway_ack_queue(
    const struct mesh_outbound *out,
    uint8_t received_radio_channel)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           DEVICE_ROLE == ROLE_ANCHOR &&
           received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           out != NULL &&
           out->packet.msg_type == MSG_GATEWAY_ACK &&
           out->packet.src_id == GATEWAY_ID &&
           out->packet.dst_id != DEVICE_ID &&
           out->packet.dst_id != GATEWAY_ID &&
           mesh_id_is_unicast(out->next_hop_id);
}

static bool mesh_terminal_cleanup_idempotent_status(int ret)
{
    return ret == 0 || ret == -ENOENT || ret == -ESTALE ||
           ret == -ENOTSUP;
}

static int mesh_complete_gateway_ack_confirm(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint64_t confirmed_at_ms)
{
    struct proto_packet original_packet;
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool locally_originated;
    int ret;

    ret = mesh_gateway_ack_confirm_identity_packet(
        confirm_packet,
        confirm_payload,
        confirm_payload_len,
        &original_packet,
        original_digest);
    if (ret != PROTO_OK || original_packet.dst_id != GATEWAY_ID) {
        status_debug_printf(
            "DBG_ACK_CONFIRM_TERMINAL stage=identity ret=%d msg=%u seq=%u\n",
            ret,
            confirm_packet == NULL ? 0u : confirm_packet->msg_type,
            confirm_packet == NULL ? 0u : confirm_packet->seq);
        return -EBADMSG;
    }
    locally_originated = original_packet.src_id == DEVICE_ID;

    if (!locally_originated) {
        /* The child already retired its original on this relay's hop ACK.
         * There is no local producer record to consume here; exact relay-core
         * custody is the only remaining owner. */
        ret = mesh_relay_commit_gateway_ack_confirm_terminal(
            &mesh_runtime,
            confirm_packet,
            confirm_payload,
            confirm_payload_len,
            k_uptime_get_32());
        status_debug_printf(
            "DBG_RELAY_ACK_CONFIRM_TERMINAL ret=%d src=0x%llx type=0x%02x seq=%u\n",
            ret,
            (unsigned long long)original_packet.src_id,
            original_packet.msg_type,
            original_packet.seq);
        return ret == PROTO_OK ? 0 : -EBADMSG;
    }

    /*
     * Validate and consume every digest-aware producer before touching the
     * shared relay owner.  The relay commit must then precede node_comm: a
     * node_comm confirmation immediately retires its transport backend, and
     * that exact backend is the relay owner being confirmed here.  Running
     * node_comm first therefore clears gateway_ack_confirm_pending before its
     * exact commit and strands the source in confirmation recovery.
     */
    ret = mesh_anchor_range_report_note_gateway_confirmed(
        &original_packet, original_digest);
    if (ret < 0) {
        status_debug_printf(
            "DBG_ACK_CONFIRM_TERMINAL stage=range ret=%d original=%u seq=%u\n",
            ret,
            original_packet.msg_type,
            original_packet.seq);
        LOG_WRN("gateway ACK-confirm range cleanup deferred: ret=%d",
                ret);
        return ret;
    }
    ret = mesh_relay_commit_gateway_ack_confirm_terminal(
        &mesh_runtime,
        confirm_packet,
        confirm_payload,
        confirm_payload_len,
        k_uptime_get_32());
    if (ret != PROTO_OK) {
        status_debug_printf(
            "DBG_ACK_CONFIRM_TERMINAL stage=relay-commit ret=%d original=%u seq=%u\n",
            ret,
            original_packet.msg_type,
            original_packet.seq);
        return -EBADMSG;
    }
    ret = app_node_comm_note_gateway_confirmed_digest_at(
        &original_packet, original_digest, confirmed_at_ms);
    if (!mesh_terminal_cleanup_idempotent_status(ret)) {
        status_debug_printf(
            "DBG_ACK_CONFIRM_TERMINAL stage=node-comm ret=%d original=%u seq=%u\n",
            ret,
            original_packet.msg_type,
            original_packet.seq);
        LOG_ERR("gateway ACK-confirm node communication cleanup failed after exact relay commit: ret=%d",
                ret);
        app_watchdog_stop_feeding();
        return ret;
    }
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_delivery_gateway_confirmed != NULL) {
        ret = mesh_report_callbacks->anchor_delivery_gateway_confirmed(
            &original_packet, original_digest);
        if (!mesh_terminal_cleanup_idempotent_status(ret)) {
            status_debug_printf(
                "DBG_ACK_CONFIRM_TERMINAL stage=producer ret=%d original=%u seq=%u\n",
                ret,
                original_packet.msg_type,
                original_packet.seq);
            LOG_ERR("gateway ACK-confirm producer cleanup failed after exact relay and node communication commit: ret=%d",
                    ret);
            app_watchdog_stop_feeding();
            return ret;
        }
    }
    status_debug_printf(
        "DBG_ACK_CONFIRM_TERMINAL stage=complete ret=0 original=%u seq=%u\n",
        original_packet.msg_type,
        original_packet.seq);
    if (app_node_comm_pending_delivery_count() == 0u &&
        report_tx_queue_used() == 0u &&
        !mesh_route_waiting_tx_valid &&
        !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
        const struct route_candidate *selected =
            route_selected(&mesh_runtime.upstream);
        uint64_t parent_id =
            selected != NULL ? selected->next_hop_id : 0u;

        if (mesh_id_is_unicast(parent_id) &&
            parent_id != GATEWAY_ID) {
            mesh_close_channel9_connection(
                parent_id, "ack-confirm-terminal-confirmed");
        }
    }
    return 0;
}

static int mesh_complete_terminal_release(
    const struct mesh_outbound *terminal,
    enum node_comm_terminal_reason reason)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (terminal == NULL ||
        terminal->packet.dst_id != GATEWAY_ID ||
        terminal->packet.payload_len != terminal->payload_len ||
        !mesh_packet_semantic_digest(&terminal->packet,
                                     terminal->payload,
                                     terminal->payload_len,
                                     semantic_digest)) {
        return -EBADMSG;
    }

    if (terminal->packet.src_id != DEVICE_ID) {
        /*
         * A relay owns only transport custody for a transit packet.  Local
         * producer and node-communication cleanup belongs to the source
         * anchor and would reject this exact terminal identity here.  Commit
         * the relay's digest-bound terminal record directly so expired
         * transit custody cannot pin the relay forever.
         */
        ret = mesh_relay_commit_terminal_release(
            &mesh_runtime,
            &terminal->packet,
            terminal->payload,
            terminal->payload_len);
        if (ret != PROTO_OK) {
            return -EBADMSG;
        }
        return 0;
    }

    /*
     * Retire every exact producer owner before releasing the relay's final
     * in-RAM copy. Each operation is digest-bound and idempotent, so a
     * replay cannot terminalize a same-header/different-payload operation.
     */
    ret = mesh_anchor_range_report_note_terminal_release(
        &terminal->packet, terminal->payload, terminal->payload_len);
    if (!mesh_terminal_cleanup_idempotent_status(ret)) {
        return ret;
    }
    ret = app_node_comm_note_gateway_failed_digest(
        &terminal->packet, semantic_digest, reason);
    if (!mesh_terminal_cleanup_idempotent_status(ret)) {
        return ret;
    }
    ret = mesh_relay_commit_terminal_release(
        &mesh_runtime,
        &terminal->packet,
        terminal->payload,
        terminal->payload_len);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    return 0;
}

static bool mesh_gateway_ack_route_waiting_owns(
    const struct mesh_outbound *ack)
{
    const struct mesh_outbound *waiting = &mesh_route_waiting_tx;

    if (ack == NULL || !mesh_route_waiting_tx_valid ||
        ack->next_hop_id != waiting->next_hop_id ||
        ack->radio_channel != waiting->radio_channel ||
        ack->payload_len != waiting->payload_len ||
        ack->packet.msg_type != waiting->packet.msg_type ||
        ack->packet.flags != waiting->packet.flags ||
        ack->packet.src_id != waiting->packet.src_id ||
        ack->packet.dst_id != waiting->packet.dst_id ||
        ack->packet.session_id != waiting->packet.session_id ||
        ack->packet.seq != waiting->packet.seq) {
        return false;
    }
    return ack->payload_len == 0u ||
           memcmp(ack->payload, waiting->payload, ack->payload_len) == 0;
}


static inline bool mesh_selected_relay_parent_needs_channel9_repair(
    int select_ret,
    uint64_t selected_next_hop_id)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    (void)select_ret;
    (void)selected_next_hop_id;
    return false;
#else
    const struct route_candidate *selected;

    if (select_ret != PROTO_OK ||
        !mesh_id_is_unicast(selected_next_hop_id) ||
        selected_next_hop_id == DEVICE_ID ||
        selected_next_hop_id == GATEWAY_ID) {
        return false;
    }

    selected = route_selected(&mesh_runtime.upstream);
    return selected != NULL &&
           selected->next_hop_id == selected_next_hop_id &&
           selected->gateway_id == GATEWAY_ID &&
           !selected->channel9_timing_valid;
#endif
}

/*
 * End of the synchronous first-hop ACK window this sender actually listened
 * through, measured from the physical send rather than from the moment the
 * receive turn ended: an ACK slice cut short by another owner's frame must
 * still leave the complete APP_MESH_DIRECT_GATEWAY_ACK_RX_MS window intact.
 */
static uint32_t mesh_direct_gateway_ack_window_end_ms(
    const struct mesh_outbound *out,
    const struct app_mesh_tx_observation *observation)
{
    uint32_t sent_ms = k_uptime_get_32();
    /* A parent anchor answers with MSG_MESH_HOP_ACK only after its deferred
     * mesh RX handoff; the gateway answers within a few ms.  Use the same
     * per-hop window the ACK receive loop uses so the relay core's deadline
     * matches how long the sender actually listened. */
    uint32_t window_ms = (out != NULL && out->next_hop_id != GATEWAY_ID) ?
                         APP_MESH_PARENT_HOP_ACK_RX_MS :
                         MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;

    if (observation != NULL && observation->tx_completed) {
        sent_ms = (uint32_t)observation->tx_completed_at_ms;
    } else if (observation != NULL && observation->rf_started) {
        sent_ms = (uint32_t)observation->rf_started_at_ms;
    }

    return sent_ms + window_ms + MESH_GATEWAY_DIRECT_PROBE_ACK_GUARD_MS;
}

static void mesh_handle_result_actions(struct mesh_relay_result *result,
                                       uint8_t received_radio_channel,
                                       const struct mesh_rx_pending *rx,
                                       const struct proto_packet *confirmed_packet,
                                       const uint8_t *confirmed_payload,
                                       size_t confirmed_payload_len,
                                       const uint8_t confirmed_digest[
                                           SEMANTIC_DIGEST_SHA256_LEN],
                                       bool *gateway_ack_handed_off)
{
    bool forward_sent = false;
    bool forward_queued_gateway_ack = false;
    bool route_reply_downstream_handoff_required =
        (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u;
    bool route_reply_downstream_handoff_acked = false;
    struct app_mesh_result_handoff_ops handoff_ops = {
#if DEVICE_ROLE == ROLE_ANCHOR
        .note_result_bundle_forwarded = mesh_handoff_note_result_bundle_forwarded,
#endif
        .send_result_grant = mesh_handoff_send_result_grant,
        .note_tx_sent = mesh_handoff_note_tx_sent,
    };
    struct app_mesh_result_handoff_status handoff_status;

    if (gateway_ack_handed_off != NULL) {
        *gateway_ack_handed_off =
            (result == NULL ||
             (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) == 0u);
    }

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        /*
         * mesh_work_result is shared scratch.  Freeze the ACK before the
         * guard sleep or RF handoff so unrelated work cannot replace the
         * packet while this receive turn still owns it.
         */
        struct mesh_outbound gateway_ack_snapshot = result->gateway_ack;
        struct mesh_outbound *gateway_ack = &gateway_ack_snapshot;
        struct mesh_channel5_requirements requirements;
        struct mesh_event_plan plan = {0};
        struct app_mesh_gateway_ack_state ack_state = {
            .route_test_enabled = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST),
            .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
            .received_on_channel9 = received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD,
            .channel9_require_ret = PROTO_ERR_BUSY,
            .channel9_retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,
        };
        struct app_mesh_gateway_ack_decision ack_decision;
        bool gateway_ack_policy_deferred = false;
        uint32_t now_ms;
        /* The route-probe retry-owner branch below can report the policy
         * outcome without attempting a same-turn send. */
        int ret = -EAGAIN;

#if DEVICE_ROLE == ROLE_GATEWAY
        if (received_radio_channel == UWB_CHANNEL_WAKE_CONTACT) {
            const uint64_t ack_deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;

            gateway_ack->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
            gateway_ack->earliest_tx_ms = 0u;
            gateway_ack->earliest_tx_valid = false;
            /* Causal reply: publishes the RX-to-control handoff and may
             * transmit while later RX items are still queued, because the
             * sender's ACK window is far shorter than draining a backlog. */
            if (mesh_c5_collect_ack(gateway_ack, rx)) {
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off = true;
                }
                goto after_gateway_ack;
            }
            ret = mesh_send_causal_channel9_response(
                gateway_ack, "gateway-ack-c5", NULL);
            /* dms is the receive-to-ACK latency this path exists to shrink:
             * the frame's arrival stamp to the moment its ACK left the RF. */
            status_debug_printf("DBG_GATEWAY_ACK_SEND ret=%d seq=%u dst=0x%llx next=0x%llx rx=%u dms=%u\n",
                                ret,
                                gateway_ack->packet.seq,
                                (unsigned long long)gateway_ack->packet.dst_id,
                                (unsigned long long)gateway_ack->next_hop_id,
                                rx == NULL ? 0u : rx->received_at_ms,
                                rx == NULL ? 0u :
                                    k_uptime_get_32() - rx->received_at_ms);
            if (ret == 0) {
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off = true;
                }
                mesh_relay_note_tx_sent(&mesh_runtime,
                                        gateway_ack,
                                        k_uptime_get_32());
                mesh_c5_capture_followers(gateway_ack, rx);
                goto after_gateway_ack;
            }
            /* The radio was busy in this turn; node communication owns the
             * bounded retry while the sender keeps its own retry clock. */
            ret = app_node_comm_submit_control_response(
                gateway_ack,
                ack_deadline_ms,
                gateway_ack->packet.seq);
            status_debug_printf("DBG_GATEWAY_ACK_QUEUE ret=%d seq=%u dst=0x%llx next=0x%llx\n",
                                ret,
                                gateway_ack->packet.seq,
                                (unsigned long long)gateway_ack->packet.dst_id,
                                (unsigned long long)gateway_ack->next_hop_id);
            if (ret < 0) {
                LOG_WRN("gateway ACK communication admission failed; sender retains custody: ret=%d dst=0x%016llx seq=%u",
                        ret,
                        (unsigned long long)gateway_ack->packet.dst_id,
                        gateway_ack->packet.seq);
            } else if (gateway_ack_handed_off != NULL) {
                *gateway_ack_handed_off = true;
            }
            goto after_gateway_ack;
        }
#endif
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            rx != NULL) {
            struct mesh_ch9_batch_metadata batch_metadata;
            int meta_ret = mesh_ch9_batch_metadata_parse(rx->payload,
                                                         rx->payload_len,
                                                         &batch_metadata);

            status_debug_printf("DBG_GATEWAY_ACK_INPUT type=%u src=0x%llx dst=0x%llx rxseq=%u ackseq=%u len=%u flags=0x%02x meta=%d\n",
                                rx->packet.msg_type,
                                (unsigned long long)rx->packet.src_id,
                                (unsigned long long)rx->packet.dst_id,
                                rx->packet.seq,
                                gateway_ack->packet.seq,
                                rx->payload_len,
                                rx->packet.flags,
                                meta_ret);

            if (meta_ret == PROTO_OK && batch_metadata.present) {
                gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
                ret = mesh_ch9_ack_batch_queue(gateway_ack, rx);
                if (ret == 0 && gateway_ack_handed_off != NULL) {
                    /* Route-test batch admission is the radio owner for this
                     * ACK. The queue helper validates the exact packet and
                     * keeps duplicate entries idempotent. */
                    *gateway_ack_handed_off = true;
                }
                status_debug_printf("DBG_GATEWAY_BATCH_ACK_QUEUE batch=%u final=%u n=%u rxseq=%u ackseq=%u next=0x%llx\n",
                                    batch_metadata.batch_id,
                                    batch_metadata.final_packet ? 1u : 0u,
                                    mesh_ch9_ack_batch_count_for_peer(
                                        gateway_ack->next_hop_id),
                                    rx->packet.seq,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->next_hop_id);
                if (ret == 0 && batch_metadata.final_packet) {
                    ret = mesh_send_current_ch9_ack_batch(
                        gateway_ack->next_hop_id,
                        "gateway-batch-ack-final");
                    status_debug_printf("DBG_GATEWAY_BATCH_ACK_FINAL batch=%u ret=%d n=%u\n",
                                        batch_metadata.batch_id,
                                        ret,
                                        mesh_ch9_ack_batch_count_for_peer(
                                            gateway_ack->next_hop_id));
                }
                goto after_gateway_ack;
            }
            if (meta_ret != PROTO_OK) {
                status_debug_printf("DBG_GATEWAY_BATCH_ACK_META_BAD ret=%d rxseq=%u len=%u\n",
                                    meta_ret,
                                    rx->packet.seq,
                                    rx->payload_len);
                /*
                 * Core relay pre-admission rejects malformed batch metadata,
                 * so this is a defensive fail-closed boundary.  Never turn an
                 * invalid batch member into an immediate single-packet ACK:
                 * that would let the sender retire custody for data whose
                 * batch identity was never accepted.
                 */
                goto after_gateway_ack;
            }
        }
        if (DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            const uint64_t ack_deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;

            app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
            if (ack_decision.action ==
                APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
                ret = mesh_send_causal_channel9_response(
                    gateway_ack, ack_decision.reason, NULL);
                status_debug_printf("DBG_GATEWAY_ACK_SEND ret=%d seq=%u dst=0x%llx next=0x%llx\n",
                                    ret,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->packet.dst_id,
                                    (unsigned long long)gateway_ack->next_hop_id);
                if (ret == 0) {
                    if (gateway_ack_handed_off != NULL) {
                        *gateway_ack_handed_off = true;
                    }
                    app_mesh_rf_retry_note_success(
                        &mesh_deferred_gateway_ack_rf_retry,
                        &retry_key);
                    mesh_relay_note_tx_sent(&mesh_runtime,
                                            gateway_ack,
                                            k_uptime_get_32());
                    mesh_note_channel9_local_tx(gateway_ack->next_hop_id,
                                                k_uptime_get_32());
                    goto after_gateway_ack;
                }
            }

            if (rx != NULL &&
                rx->packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
                /*
                 * A direct route probe is idempotent contact discovery.  If
                 * its same-turn ACK could not acquire the radio, the sender
                 * already owns the bounded retry; retaining a reply in RAM
                 * for every fresh probe sequence only starves real control
                 * and result traffic.
                 */
                status_debug_printf(
                    "DBG_GATEWAY_ROUTE_PROBE_ACK_RETRY_OWNED ret=%d rxseq=%u ackseq=%u\n",
                    ret,
                    rx->packet.seq,
                    gateway_ack->packet.seq);
                goto after_gateway_ack;
            }

            gateway_ack->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
            gateway_ack->earliest_tx_ms = 0u;
            gateway_ack->earliest_tx_valid = false;
            ret = app_node_comm_submit_control_response(
                gateway_ack,
                ack_deadline_ms,
                gateway_ack->packet.seq);
            status_debug_printf("DBG_GATEWAY_ACK_QUEUE ret=%d seq=%u dst=0x%llx next=0x%llx deadline=%llu\n",
                                ret,
                                gateway_ack->packet.seq,
                                (unsigned long long)gateway_ack->packet.dst_id,
                                (unsigned long long)gateway_ack->next_hop_id,
                                (unsigned long long)ack_deadline_ms);
            if (ret < 0) {
                LOG_WRN("gateway ACK communication admission failed; sender retains custody: ret=%d dst=0x%016llx seq=%u",
                        ret,
                        (unsigned long long)gateway_ack->packet.dst_id,
                        gateway_ack->packet.seq);
            } else if (gateway_ack_handed_off != NULL) {
                *gateway_ack_handed_off = true;
            }
            goto after_gateway_ack;
        }
        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_QUEUE_ROUTE_TEST_ACK) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_ch9_ack_batch_queue(gateway_ack, rx);
            if (gateway_ack_handed_off != NULL) {
                *gateway_ack_handed_off = ret == 0;
            }
            goto after_gateway_ack;
        }

        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_send_causal_channel9_response(
                gateway_ack, ack_decision.reason, NULL);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE == ROLE_GATEWAY) {
                status_debug_printf("DBG_GATEWAY_ACK_SEND ret=%d seq=%u dst=0x%llx next=0x%llx\n",
                                    ret,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->packet.dst_id,
                                    (unsigned long long)gateway_ack->next_hop_id);
            }
            if (ret == 0) {
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off = true;
                }
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                app_mesh_rf_retry_note_success(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                mesh_note_channel9_local_tx(gateway_ack->next_hop_id, k_uptime_get_32());
                LOG_INF("gateway ACK sent on current channel-9 event: dst=0x%016llx next=0x%016llx seq=%u",
                        (unsigned long long)gateway_ack->packet.dst_id,
                        (unsigned long long)gateway_ack->next_hop_id,
                        gateway_ack->packet.seq);
                goto after_gateway_ack;
            } else {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-current-channel9");

                LOG_WRN("gateway ACK current channel-9 send failed; will retry on channel-9 event: next=0x%016llx",
                        (unsigned long long)gateway_ack->next_hop_id);
                ack_state.current_channel9_attempted = true;
                ack_state.current_channel9_ret = ret;
                app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
                (void)mesh_store_route_waiting_tx_owned(
                    gateway_ack,
                    APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK);
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off =
                        mesh_gateway_ack_route_waiting_owns(gateway_ack);
                }
                mesh_schedule_route_waiting_retry_after(ack_decision.reason,
                                                        retry_delay_ms);
                goto after_gateway_ack;
            }
        }

        mesh_fill_channel5_requirements(&requirements);
        now_ms = k_uptime_get_32();
        (void)mesh_expire_channel9_timings(now_ms, "gateway-ack");
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   gateway_ack->next_hop_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        ack_state.channel9_require_ret = ret;
        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("gateway-ack",
                                       "gateway-ack",
                                       gateway_ack->next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        gateway_ack_policy_deferred =
            ret == PROTO_ERR_BUSY &&
            mesh_event_plan_is_policy_deferral(plan.action);
        if (gateway_ack_policy_deferred) {
            mesh_relay_note_channel9_missed(&mesh_runtime,
                                            gateway_ack->next_hop_id,
                                            &mesh_event_stats);
        }
        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &gateway_ack->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_send_outbound(gateway_ack, ack_decision.reason);
            if (ret == 0) {
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off = true;
                }
                app_mesh_rf_retry_note_success(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                mesh_note_channel9_local_tx(gateway_ack->next_hop_id, plan.start_ms);
            } else {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-planned-channel9");

                (void)mesh_store_route_waiting_tx_owned(
                    gateway_ack,
                    APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK);
                if (gateway_ack_handed_off != NULL) {
                    *gateway_ack_handed_off =
                        mesh_gateway_ack_route_waiting_owns(gateway_ack);
                }
                mesh_schedule_route_waiting_retry_after(
                    "gateway-ack-planned-channel9",
                    retry_delay_ms);
            }
        } else if (ack_decision.action ==
                   APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9) {
            LOG_WRN("gateway ACK deferred until channel-9 timing refresh: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack->next_hop_id,
                    ret);
            (void)mesh_store_route_waiting_tx_owned(
                gateway_ack,
                APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK);
            if (gateway_ack_handed_off != NULL) {
                *gateway_ack_handed_off =
                    mesh_gateway_ack_route_waiting_owns(gateway_ack);
            }
            (void)mesh_propose_event_after_channel5_contact(gateway_ack->next_hop_id,
                                                            ack_decision.reason);
            mesh_schedule_route_waiting_retry_after(ack_decision.reason,
                                                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        } else if (ack_decision.action ==
                   APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY) {
            uint32_t retry_delay_ms = ack_decision.delay_ms;
            const char *retry_reason = ack_decision.reason;

            if (gateway_ack_policy_deferred) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-channel5-deferral");
                retry_reason = "gateway-ack-channel5-deferral";
            } else if (ret == PROTO_ERR_BUSY &&
                       plan.action == MESH_EVENT_PLAN_WAIT) {
                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    &retry_delay_ms);
            }
            if (retry_delay_ms == 0u) {
                retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
            }
            LOG_WRN("gateway ACK waiting for channel-9 event: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack->next_hop_id,
                    ret);
            (void)mesh_store_route_waiting_tx_owned(
                gateway_ack,
                APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK);
            if (gateway_ack_handed_off != NULL) {
                *gateway_ack_handed_off =
                    mesh_gateway_ack_route_waiting_owns(gateway_ack);
            }
            mesh_schedule_route_waiting_retry_after(retry_reason,
                                                    retry_delay_ms);
        }
    }
after_gateway_ack:
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        bool c5_forward_admission_retained = true;
        int ret;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            result->forward.packet.msg_type == MSG_GATEWAY_ACK) {
            status_debug_printf("DBG_FORWARD_GATEWAY_ACK_CAND ch=%u src=0x%llx dst=0x%llx next=0x%llx local=0x%llx gw=0x%llx\n",
                                received_radio_channel,
                                (unsigned long long)result->forward.packet.src_id,
                                (unsigned long long)result->forward.packet.dst_id,
                                (unsigned long long)result->forward.next_hop_id,
                                (unsigned long long)DEVICE_ID,
                                (unsigned long long)GATEWAY_ID);
        }
        if (result->forward.packet.dst_id == MESH_BROADCAST_ID) {
            ret = mesh_send_c5_flood_response(
                &result->forward,
                mesh_c5_purpose_for_packet(&result->forward.packet),
                "broadcast-forward",
                &c5_forward_admission_retained);
        } else if (result->forward.packet.src_id == GATEWAY_ID &&
                   mesh_id_is_unicast(result->forward.packet.dst_id) &&
                   result->forward.packet.msg_type == MSG_COMMAND) {
            struct mesh_outbound control_forward = result->forward;

            control_forward.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
            ret = mesh_send_c5_flood_response(
                &control_forward,
                C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                "targeted-gateway-control-forward",
                &c5_forward_admission_retained);
        } else if (mesh_forward_uses_child_gateway_ack_queue(
                       &result->forward,
                       received_radio_channel)) {
            /*
             * Gateway-ACK handling above is complete before forwarding. Its
             * result slot is the transient mutable copy, while the original
             * forward member remains intact for the custody handoff below.
             */
            struct mesh_outbound *forward_ack = &result->gateway_ack;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_FORWARD_GATEWAY_ACK_MATCH\n");
            }
            *forward_ack = result->forward;
            forward_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_ch9_ack_batch_queue_forwarded_gateway_ack(
                forward_ack,
                (result->actions &
                 MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING) != 0u,
                "forward-gateway-ack");
            forward_queued_gateway_ack = ret == 0;
            if (ret == 0 &&
                (result->actions &
                 MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING) != 0u) {
                const struct app_mesh_ch9_ack_batch *batch =
                    app_mesh_ch9_ack_table_get_peer(
                        &mesh_ch9_ack_table, forward_ack->next_hop_id);
                struct app_mesh_c5_tx_authorization_token authorization;
                int promote_ret;

                if (!app_mesh_ch9_c5_repair_authorization_capture(
                        &authorization,
                        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
                        &mesh_runtime.pending,
                        mesh_relay_tx_active(&mesh_runtime),
                        batch,
                        forward_ack->next_hop_id)) {
                    LOG_ERR("forwarded gateway ACK could not capture reciprocal ACCEPT repair owner: peer=0x%016llx",
                            (unsigned long long)forward_ack->next_hop_id);
                    app_watchdog_stop_feeding();
                } else {
                    promote_ret =
                        mesh_propose_event_after_channel5_contact_authorized(
                            forward_ack->next_hop_id,
                            "forwarded-gateway-ack-event-repair",
                            &authorization,
                            false,
                            false);
                    if (promote_ret != 0 && promote_ret != -EAGAIN &&
                        promote_ret != -EBUSY) {
                        LOG_ERR("forwarded gateway ACK could not promote reciprocal ACCEPT owner: peer=0x%016llx ret=%d",
                                (unsigned long long)forward_ack->next_hop_id,
                                promote_ret);
                        app_watchdog_stop_feeding();
                    }
                }
            }
        } else if (mesh_forward_uses_gateway_batch_queue(&result->forward)) {
            ret = queue_anchor_report(&result->forward);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_FORWARD_GATEWAY_BATCH_QUEUE ret=%d src=0x%llx session=%u seq=%u q=%u\n",
                                    ret,
                                    (unsigned long long)
                                        result->forward.packet.src_id,
                                    result->forward.packet.session_id,
                                    result->forward.packet.seq,
                                    report_tx_queue_used());
            }
        } else {
            ret = mesh_start_tracked_tx(&result->forward, "forward");
#if DEVICE_ROLE == ROLE_ANCHOR
            if (result->forward.packet.dst_id == GATEWAY_ID &&
                result->forward.packet.src_id != DEVICE_ID &&
                (result->forward.packet.flags &
                 FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
                int reverse_route_ret =
                    mesh_relay_commit_transit_reverse_route(
                        &mesh_runtime,
                        &result->forward,
                        k_uptime_get_32());

                if (reverse_route_ret != PROTO_OK &&
                    reverse_route_ret != PROTO_ERR_NOT_FOUND) {
                    LOG_ERR("retained transit reverse route commit failed: src=0x%016llx prev=0x%016llx ret=%d",
                            (unsigned long long)result->forward.packet.src_id,
                            (unsigned long long)
                                result->forward.ingress_previous_hop_id,
                            reverse_route_ret);
                    app_watchdog_stop_feeding();
                    return;
                } else if (reverse_route_ret == PROTO_OK &&
                           IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf(
                        "DBG_TRANSIT_REVERSE_ROUTE_COMMIT src=0x%llx session=%u seq=%u prev=0x%llx owner=relay\n",
                        (unsigned long long)result->forward.packet.src_id,
                        result->forward.packet.session_id,
                        result->forward.packet.seq,
                        (unsigned long long)
                            result->forward.ingress_previous_hop_id);
                }
            }
#endif
        }
        if (!c5_forward_admission_retained) {
            if (result->route_state_changed) {
                LOG_ERR("committed route-state forward could not retain retry: msg=0x%02x seq=%u ret=%d",
                        result->forward.packet.msg_type,
                        result->forward.packet.seq,
                        ret);
                (void)mesh_transport_pause_preserving_queued();
                app_watchdog_stop_feeding();
                return;
            } else {
                mesh_rollback_c5_forward_admission(
                    result, rx, "forward-response-capacity");
            }
        }
        forward_sent = ret == 0 && !forward_queued_gateway_ack;
        app_mesh_result_handoff_after_forward(result,
                                              forward_sent,
                                              &handoff_ops,
                                              &handoff_status);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        app_mesh_result_handoff_prepare_hop_ack(result,
                                                forward_sent,
                                                &handoff_ops,
                                                &handoff_status);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) &&
        handoff_status.hop_ack_allowed) {
        struct mesh_outbound *hop_ack = &result->hop_ack;

        if (mesh_c5_collect_ack(hop_ack, rx)) {
            goto after_hop_ack;
        }
        if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            (void)mesh_ch9_ack_batch_queue(hop_ack, rx);
        } else if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(hop_ack, "hop-ack") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, hop_ack, k_uptime_get_32());
            }
        } else {
            uint32_t hop_ack_at_ms = k_uptime_get_32();
            uint32_t hop_ack_dms = rx == NULL ? 0u :
                hop_ack_at_ms - rx->received_at_ms;
            int hop_ack_ret;

            /*
             * Same fixed channel-5 turnaround the gateway uses: with a
             * 1024-symbol ACK preamble the responder must not transmit before
             * the sender's receiver is provably armed.  Only the remainder of
             * the guard is ever slept, so a drain that already took longer
             * than MESH_GATEWAY_C5_ACK_GUARD_MS pays nothing.
             */
            if (rx != NULL && hop_ack_dms < MESH_GATEWAY_C5_ACK_GUARD_MS) {
                k_msleep(MESH_GATEWAY_C5_ACK_GUARD_MS - hop_ack_dms);
                hop_ack_at_ms = k_uptime_get_32();
                hop_ack_dms = hop_ack_at_ms - rx->received_at_ms;
            }
            hop_ack_ret = mesh_send_outbound_causal_response(hop_ack,
                                                             "hop-ack");
            status_debug_printf("DBG_HOP_ACK_TX at=%u dms=%u src=0x%llx seq=%u ret=%d\n",
                                hop_ack_at_ms,
                                hop_ack_dms,
                                (unsigned long long)(rx == NULL ?
                                    hop_ack->packet.dst_id :
                                    rx->packet.src_id),
                                hop_ack->packet.seq,
                                hop_ack_ret);
            if (hop_ack_ret == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, hop_ack,
                                        k_uptime_get_32());
                mesh_c5_capture_followers(hop_ack, rx);
            }
        }
    }
after_hop_ack:
    if (route_reply_downstream_handoff_required) {
        route_reply_downstream_handoff_acked =
            mesh_send_route_reply_action(result, "route-reply-action");
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) &&
        app_mesh_route_reply_upstream_ack_allowed(
            route_reply_downstream_handoff_required,
            route_reply_downstream_handoff_acked)) {
        struct mesh_outbound *route_reply_ack = &result->route_reply_ack;
        bool active_exchange;
        int ret;

        /*
         * A forwarded reply temporarily uses the single Channel-5 contact
         * context for its downstream peer.  Successful downstream ACK
         * reception transfers custody to that peer, so restore the accepted
         * upstream exchange before returning its ACK.  A failed downstream
         * train deliberately leaves the upstream reply unacknowledged and
         * retryable.
         */
        if (route_reply_downstream_handoff_required) {
            mesh_c5_contact_accept(route_reply_ack->next_hop_id,
                                   C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                   mesh_c5_exchange_expires_at(
                                       C5_CONTACT_PURPOSE_ROUTE_REPLY),
                                   "route-reply-upstream-handoff");
        }
        active_exchange = mesh_c5_contact_peer_active(route_reply_ack->next_hop_id,
                                                      k_uptime_get_32());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_TX_BEGIN next=0x%llx seq=%u active=%u\n",
                                (unsigned long long)route_reply_ack->next_hop_id,
                                route_reply_ack->packet.seq,
                                active_exchange ? 1u : 0u);
        }
        ret = mesh_send_c5_causal_response(
            route_reply_ack,
            C5_CONTACT_PURPOSE_ROUTE_REPLY,
            MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
            "route-reply-ack");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_TX_DONE ret=%d next=0x%llx seq=%u\n",
                                ret,
                                (unsigned long long)route_reply_ack->next_hop_id,
                                route_reply_ack->packet.seq);
        }
        if (ret == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime,
                                    route_reply_ack,
                                    k_uptime_get_32());
        }
        mesh_c5_contact_clear(ret == 0 ? "route-reply-ack-sent" :
                                       "route-reply-ack-failed");
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) &&
        !app_mesh_route_reply_upstream_ack_allowed(
            route_reply_downstream_handoff_required,
            route_reply_downstream_handoff_acked)) {
        LOG_WRN("mesh route reply upstream ACK withheld: downstream custody not confirmed");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, false);
    }
    if (result->actions & MESH_RELAY_ACTION_UPDATE_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, true);
    }
    if (result->actions & MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY) {
        int ret = app_operation_policy_install(
            &result->operation_policy,
            APP_OPERATION_POLICY_ALL_MASK);

        if (ret < 0) {
            LOG_WRN("mesh route policy install failed: %d", ret);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_ENUMERATION_PREARM) {
        int ret = mesh_report_callbacks == NULL ||
                  mesh_report_callbacks->anchor_enumeration_prearm == NULL ?
            -ENOTSUP :
            mesh_report_callbacks->anchor_enumeration_prearm(
                result->enumeration_prearm_epoch,
                result->enumeration_prearm_hold_ms,
                result->operation_policy.assignment.operation_budget_ms,
                result->enumeration_survey_follows,
                result->gateway_route_adv.route_wave_start_ms,
                (uint8_t)(FLOOD_EPOCH_GLOBAL_TTL -
                          result->gateway_route_adv.packet.ttl));

        if (ret < 0) {
            LOG_WRN("enumeration Here-I-Am prearm rejected: epoch=%u ret=%d",
                    result->enumeration_prearm_epoch,
                    ret);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) {
        bool forward_admission_retained = false;
        uint8_t forward_purpose =
            (result->actions & MESH_RELAY_ACTION_ENUMERATION_PREARM) != 0u ?
                C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD :
                C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH;
        int ret;

        if (mesh_route_activation_apply_forward_timing(
                &result->gateway_route_adv, rx)) {
            result->gateway_route_adv.earliest_tx_ms =
                result->gateway_route_adv.route_wave_start_ms +
                MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS +
                mesh_gateway_route_adv_copy_offset_ms(
                    DEVICE_ID,
                    result->gateway_route_adv.packet.session_id,
                    (uint8_t)(FLOOD_EPOCH_GLOBAL_TTL -
                              result->gateway_route_adv.packet.ttl),
                    0u);
            result->gateway_route_adv.earliest_tx_valid = true;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_BEGIN ttl=%u seq=%u earliest=%u\n",
                                result->gateway_route_adv.packet.ttl,
                                result->gateway_route_adv.packet.seq,
                                result->gateway_route_adv.earliest_tx_ms);
        }
        ret = mesh_send_c5_flood_response(&result->gateway_route_adv,
                                          forward_purpose,
                                          "gateway-route-adv-forward",
                                          &forward_admission_retained);
        if (!forward_admission_retained) {
            /*
             * Freshness was committed before action processing. Rolling it
             * back here would let an intermediate sequence overwrite the live
             * path while the in-RAM route state still names the newer one.
             * Retryable failures retain the exact advertisement in the
             * dedicated superseding lane; any other failure stops the
             * transport instead of regressing ordering state.
             */
            LOG_ERR("committed gateway route advertisement could not retain retry: seq=%u ret=%d",
                    result->gateway_route_adv.packet.session_id,
                    ret);
            (void)mesh_transport_pause_preserving_queued();
            app_watchdog_stop_feeding();
            return;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_DONE ret=%d seq=%u\n",
                                ret,
                                result->gateway_route_adv.packet.seq);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) {
        if (mesh_send_c5_causal_response(
                &result->busy,
                C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                MESH_C5_CONTROL_WAKE_IF_NEEDED,
                "relay-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_BUSY) {
        if (mesh_send_c5_causal_response(
                &result->busy,
                C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
                MESH_C5_CONTROL_WAKE_IF_NEEDED,
                "result-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT) {
        app_mesh_result_handoff_result_grant(result,
                                             &handoff_ops,
                                             &handoff_status);
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        /* RF handoff may synchronously drain a received control frame.  That
         * path reuses mesh_work_result, so never retain a pointer into the
         * shared result scratch across the handoff. */
        struct mesh_outbound retransmit_snapshot = result->retransmit;
        struct mesh_outbound *retransmit = &retransmit_snapshot;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
        bool direct_gateway_retransmit = false;
        uint32_t retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
        int ret = PROTO_OK;

        if (mesh_packet_prefers_channel9(&retransmit->packet)) {
            struct mesh_channel5_requirements requirements;
            uint32_t now_ms = k_uptime_get_32();
            uint64_t debug_next_hop = 0u;
            int debug_select_ret;

            debug_next_hop = retransmit->next_hop_id;
            debug_select_ret = mesh_id_is_unicast(debug_next_hop) ?
                               PROTO_OK : PROTO_ERR_NOT_FOUND;
            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_expire_channel9_timings(now_ms, "retransmit");
            /* Every gateway-bound retransmit uses the channel-5 uplink: one
             * frame plus an immediate ACK, preceded by a wake train when the
             * next hop is a low-duty parent anchor. */
            direct_gateway_retransmit =
                IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE != ROLE_GATEWAY &&
                retransmit->packet.dst_id == GATEWAY_ID &&
                debug_select_ret == PROTO_OK;
            if (direct_gateway_retransmit) {
                plan.action = MESH_EVENT_PLAN_START;
                plan.start_ms = now_ms;
                plan.window_ms = MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
                plan.end_ms = now_ms + plan.window_ms;
                ret = PROTO_OK;
                status_debug_printf(
                    "DBG_RETRANSMIT_DIRECT_GW seq=%u now=%u\n",
                    retransmit->packet.seq,
                    now_ms);
            } else {
                ret = mesh_relay_require_channel9_tx_event(
                    &mesh_runtime,
                    retransmit->next_hop_id,
                    &requirements,
                    now_ms,
                    &plan);
            }
            if (ret != PROTO_ERR_STALE) {
                mesh_event_note_plan_action(&mesh_event_stats, plan.action);
            }
            mesh_debug_channel5_preemption("retransmit",
                                           "retransmit",
                                           retransmit->next_hop_id,
                                           &requirements,
                                           &plan,
                                           now_ms);
            if (ret == PROTO_OK) {
                if (direct_gateway_retransmit) {
                    retransmit->radio_channel = MESH_EVENT_CHANNEL;
                } else {
                    uint32_t required_ms = 0u;
                    uint32_t prepare_now_ms = k_uptime_get_32();
                    int prepare_ret = mesh_prepare_channel9_outbound(
                        retransmit,
                        &plan,
                        prepare_now_ms,
                        &required_ms);

                    if (prepare_ret < 0) {
                        uint32_t retry_due_ms = plan.end_ms + 1u;
                        int defer_ret;

                        retry_delay_ms = uptime_deadline_reached(
                                             prepare_now_ms,
                                             retry_due_ms) ?
                            1u : uptime_ms_until_deadline(prepare_now_ms,
                                                          retry_due_ms);
                        mesh_relay_note_channel9_missed(
                            &mesh_runtime,
                            retransmit->next_hop_id,
                            &mesh_event_stats);
                        app_mesh_test_note_ch9_missed();
                        defer_ret = mesh_relay_note_retransmit_deferred(
                            &mesh_runtime,
                            retransmit,
                            prepare_now_ms + retry_delay_ms);
                        status_debug_printf(
                            "DBG_RETRANSMIT_DEFER reason=ch9-slot-fit ret=%d prepare=%d delay=%u req=%u seq=%u state=%u\n",
                            defer_ret,
                            prepare_ret,
                            retry_delay_ms,
                            required_ms,
                            retransmit->packet.seq,
                            mesh_runtime.pending.state);
                        ret = PROTO_ERR_BUSY;
                    } else {
                        channel9_replanned = true;
                    }
                }
            } else if (ret == PROTO_ERR_BUSY) {
                bool policy_deferred =
                    mesh_event_plan_is_policy_deferral(plan.action);
                int defer_ret;

                if (policy_deferred) {
                    const struct app_mesh_rf_retry_key retry_key =
                        mesh_rf_retry_packet_key(
                            &retransmit->packet,
                            APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);

                    mesh_relay_note_channel9_missed(&mesh_runtime,
                                                    retransmit->next_hop_id,
                                                    &mesh_event_stats);
                    retry_delay_ms = mesh_rf_retry_next_delay_ms(
                        &mesh_retransmit_rf_retry,
                        &retry_key,
                        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                        "retransmit-channel5-deferral");
                } else if (plan.action == MESH_EVENT_PLAN_WAIT) {
                    (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                        now_ms,
                        plan.start_ms,
                        MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                        &retry_delay_ms);
                }
                LOG_INF("mesh retransmit waiting for channel-9 event: msg=0x%02x dst=0x%016llx start_ms=%u window_ms=%u",
                        retransmit->packet.msg_type,
                        (unsigned long long)retransmit->packet.dst_id,
                        plan.start_ms,
                        plan.window_ms);
                defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    now_ms + retry_delay_ms);
                status_debug_printf("DBG_RETRANSMIT_DEFER reason=%s ret=%d delay=%u seq=%u state=%u\n",
                                    policy_deferred ?
                                        "channel5-deferral" : "ch9-wait",
                                    defer_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
            } else {
                int defer_ret;

                mesh_debug_channel9_unavailable("retransmit",
                                                "retransmit",
                                                &retransmit->packet,
                                                ret,
                                                debug_select_ret,
                                                debug_next_hop,
                                                &plan,
                                                now_ms);
                LOG_WRN("mesh retransmit deferred until channel-9 timing is refreshed: msg=0x%02x dst=0x%016llx ret=%d",
                        retransmit->packet.msg_type,
                        (unsigned long long)retransmit->packet.dst_id,
                        ret);
                retry_delay_ms = RELAY_BUSY_RETRY_MIN_MS;
                defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    now_ms + retry_delay_ms);
                status_debug_printf("DBG_RETRANSMIT_DEFER reason=ch9-refresh ret=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
                if (mesh_selected_relay_parent_needs_channel9_repair(
                        debug_select_ret, debug_next_hop)) {
                        uint32_t failure_actions = MESH_RELAY_ACTION_NONE;
                        int failure_status = PROTO_OK;
                        struct app_mesh_rf_retry_key contact_key =
                            mesh_rf_retry_packet_key(
                                &retransmit->packet,
                                APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
                        int repair_ret;
                        int failure_ret = PROTO_OK;
                        uint32_t repair_generation =
                            (uint32_t)atomic_get(&mesh_route_ready_generation);

                        if (repair_generation == 0u) {
                            repair_generation =
                                (uint32_t)atomic_inc(
                                    &mesh_route_ready_generation) + 1u;
                        }
                        mesh_route_ready_event_owner_set(
                            &mesh_route_ready_event_owner,
                            debug_next_hop,
                            repair_generation);
                        repair_ret = mesh_propose_event_for_outbound(
                            debug_next_hop,
                            "retransmit-event-repair",
                            retransmit);
                        if (repair_ret == 0) {
                            app_mesh_known_parent_contact_note_success(
                                &mesh_known_parent_contact_retry,
                                debug_next_hop,
                                &contact_key);
                            if (mesh_route_ready_event_owner_matches(
                                    &mesh_route_ready_event_owner,
                                    debug_next_hop,
                                    repair_generation)) {
                                mesh_route_ready_event_owner_clear();
                            }
                            defer_ret = mesh_relay_note_retransmit_deferred(
                                &mesh_runtime,
                                retransmit,
                                k_uptime_get_32() + RELAY_BUSY_RETRY_MIN_MS);
                        } else if (mesh_parent_contact_failure_is_hard(
                                       repair_ret)) {
                            uint32_t failure_now_ms = k_uptime_get_32();
                            uint64_t downstream_peers[
                                MESH_RELAY_EVENT_TIMINGS];
                            uint8_t downstream_peer_count = 0u;
                            uint64_t selected_after_repair = 0u;
                            int select_after_repair =
                                mesh_relay_select_next_hop(
                                    &mesh_runtime,
                                    retransmit->packet.dst_id,
                                    &selected_after_repair);
                            bool selected_route_changed =
                                select_after_repair != PROTO_OK ||
                                selected_after_repair != debug_next_hop;
                            bool retain_known_parent = false;

                            if (selected_route_changed) {
                                defer_ret = mesh_relay_note_retransmit_deferred(
                                    &mesh_runtime,
                                    retransmit,
                                    failure_now_ms + 25u +
                                        (sys_rand32_get() % 51u));
                                if (select_after_repair != PROTO_OK) {
                                    (void)mesh_schedule_route_request(
                                        retransmit->packet.dst_id,
                                        "retransmit-topology-route-exhausted");
                                }
                            } else {
                                if (!mesh_outbound_is_topology_operation_uplink(
                                        retransmit)) {
                                    uint8_t hard_failure_count = 0u;
                                    enum app_mesh_known_parent_contact_action
                                        contact_action =
                                            app_mesh_known_parent_contact_note_hard_failure(
                                                &mesh_known_parent_contact_retry,
                                                debug_next_hop,
                                                &contact_key,
                                                MESH_KNOWN_PARENT_CONTACT_RETRIES,
                                                &hard_failure_count);

                                    retain_known_parent =
                                        contact_action ==
                                        APP_MESH_KNOWN_PARENT_CONTACT_RETRY;
                                    if (retain_known_parent) {
                                        uint32_t contact_retry_delay_ms =
                                            mesh_parent_contact_retry_delay_ms(
                                                debug_next_hop,
                                                hard_failure_count);

                                        defer_ret =
                                            mesh_relay_note_retransmit_deferred(
                                                &mesh_runtime,
                                                retransmit,
                                                failure_now_ms +
                                                    contact_retry_delay_ms);
                                        status_debug_printf(
                                            "DBG_KNOWN_PARENT_CONTACT_RETRY peer=0x%llx failure=%u/%u delay=%u seq=%u phase=retransmit\n",
                                            (unsigned long long)debug_next_hop,
                                            hard_failure_count,
                                            MESH_KNOWN_PARENT_CONTACT_RETRIES,
                                            contact_retry_delay_ms,
                                            retransmit->packet.seq);
                                    }
                                }
                            }
                            if (!selected_route_changed &&
                                !retain_known_parent) {
                                downstream_peer_count =
                                    mesh_capture_downstream_channel9_peers(
                                        downstream_peers,
                                        ARRAY_SIZE(downstream_peers));

                                failure_ret =
                                    mesh_relay_note_pending_parent_failure_status(
                                        &mesh_runtime,
                                        failure_now_ms,
                                        sys_rand32_get(),
                                        &failure_actions,
                                        &failure_status);
                                defer_ret = failure_ret;
                                if (failure_ret == PROTO_OK) {
                                    mesh_notify_removed_downstream_peers(
                                        downstream_peers,
                                        downstream_peer_count,
                                        "parent-route-failed");
                                }
                            }
                            if (!selected_route_changed &&
                                !retain_known_parent &&
                                failure_ret == PROTO_OK &&
                                (failure_actions &
                                 MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
                                (void)mesh_schedule_route_request(
                                    retransmit->packet.dst_id,
                                    "retransmit-event-repair-failed");
                            } else if (!selected_route_changed &&
                                       !retain_known_parent &&
                                       failure_ret != PROTO_OK) {
                                status_debug_note(
                                    "DBG_RETRANSMIT_EVENT_REPAIR_ACCOUNT_FAIL\n");
                                mesh_relay_invalidate_upstream_route(
                                    &mesh_runtime);
                                mesh_notify_removed_downstream_peers(
                                    downstream_peers,
                                    downstream_peer_count,
                                    "parent-route-account-failed");
                                (void)mesh_schedule_route_request(
                                    retransmit->packet.dst_id,
                                    "retransmit-event-repair-account-failed");
                            }
                        } else {
                            defer_ret = mesh_relay_note_retransmit_deferred(
                                &mesh_runtime,
                                retransmit,
                                k_uptime_get_32() + RELAY_BUSY_RETRY_MIN_MS);
                        }
                        status_debug_printf("DBG_RETRANSMIT_EVENT_REPAIR peer=0x%llx repair=%d account=%d actions=0x%08x defer=%d retry=%u\n",
                                            (unsigned long long)debug_next_hop,
                                            repair_ret,
                                            failure_ret,
                                            failure_ret == PROTO_OK &&
                                                mesh_parent_contact_failure_is_hard(
                                                    repair_ret) ?
                                                failure_actions : 0u,
                                            defer_ret,
                                            mesh_runtime.pending.retry_after_ms);
                } else if (mesh_channel9_connection_count() == 0u) {
                    (void)mesh_schedule_route_request(
                        retransmit->packet.dst_id,
                        "retransmit-channel9-refresh");
                }
            }
        }
        if (ret == PROTO_OK) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &retransmit->packet,
                    APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);
            struct app_mesh_tx_observation backend_observation = {0};
            uint64_t backend_deadline_ms = 0u;
            uint32_t backend_delivery_generation = 0u;
            bool backend_rf_started = false;
            bool backend_terminal = false;
            bool direct_gateway_ack_wait_owned = false;
            int backend_ret =
                app_node_comm_backend_retry_preflight_with_generation(
                    &retransmit->packet,
                    &backend_delivery_generation,
                    &backend_deadline_ms);
            int account_ret;
            int send_ret;

            if (backend_ret == -ETIMEDOUT || backend_ret == -ECANCELED) {
                backend_terminal = true;
                status_debug_printf(
                    "DBG_NODE_COMM_RETRANSMIT_TERMINAL ret=%d seq=%u\n",
                    backend_ret,
                    retransmit->packet.seq);
                send_ret = backend_ret;
            } else if (backend_ret < 0) {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_retransmit_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "retransmit-facade-preflight");
                int defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    k_uptime_get_32() + retry_delay_ms);

                status_debug_printf("DBG_RETRANSMIT_DEFER reason=facade-preflight ret=%d check=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    backend_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
                send_ret = backend_ret;
            } else {
                if (direct_gateway_retransmit) {
                    send_ret = 0;
                    if (retransmit->next_hop_id != GATEWAY_ID) {
                        bool wake_rf_started = false;
                        uint64_t wake_rf_started_at_ms = 0u;

                        send_ret = mesh_send_route_wake_train(
                            retransmit->next_hop_id,
                            NULL,
                            NULL,
                            C5_CONTACT_PURPOSE_UPLINK,
                            "retransmit-uplink-wake",
                            retransmit,
                            NULL,
                            FW_C5_TX_INTENT_BACKGROUND,
                            &wake_rf_started,
                            &wake_rf_started_at_ms);
                        status_debug_printf("DBG_UPLINK_WAKE next=0x%llx ret=%d seq=%u phase=retransmit\n",
                                            (unsigned long long)retransmit->next_hop_id,
                                            send_ret,
                                            retransmit->packet.seq);
                        if (send_ret < 0) {
                            mesh_restart_role_scan();
                        } else {
                            mesh_wait_for_c5_control_followup_turnaround(
                                retransmit->packet.msg_type,
                                "retransmit-uplink-wake");
                        }
                    }
                    if (send_ret == 0) {
                        send_ret = mesh_send_direct_gateway_payload_and_wait_ack(
                            retransmit,
                            "retransmit-direct-gateway",
                            &plan,
                            plan.start_ms,
                            plan.start_ms,
                            false,
                            &backend_rf_started,
                            backend_deadline_ms,
                            &backend_observation);
                    }
                } else {
                    send_ret = mesh_send_outbound_with_release_until(
                        retransmit,
                        "retransmit",
                        MESH_RADIO_RELEASE_STANDBY,
                        backend_deadline_ms,
                        &backend_observation);
                    backend_rf_started =
                        backend_observation.rf_started;
                }
                /*
                 * A facade-owned retry returns the generation captured by
                 * preflight. Unowned mesh retransmits deliberately have no
                 * app record to account, so preserve their old -ENOENT path.
                 */
                account_ret = backend_delivery_generation != 0u ?
                    app_node_comm_complete_backend_attempt_at_for_generation(
                        &retransmit->packet,
                        backend_delivery_generation,
                        backend_rf_started,
                        backend_observation.rf_started_at_ms) : -ENOENT;
                if (account_ret == 0 &&
                    backend_observation.gateway_confirmed) {
                    uint8_t semantic_digest[
                        SEMANTIC_DIGEST_SHA256_LEN];

                    account_ret = mesh_packet_semantic_digest(
                                      &retransmit->packet,
                                      retransmit->payload,
                                      retransmit->payload_len,
                                      semantic_digest) ?
                        app_node_comm_note_gateway_confirmed_digest_at(
                            &retransmit->packet,
                            semantic_digest,
                            backend_observation.gateway_confirmed_at_ms) :
                        -EBADMSG;
                }

                if (account_ret < 0 && account_ret != -ENOENT) {
                    status_debug_printf(
                        "DBG_NODE_COMM_BACKEND_RF_ACCOUNT ret=%d rf=%u seq=%u\n",
                        account_ret,
                        backend_rf_started ? 1u : 0u,
                        retransmit->packet.seq);
                }
                if (account_ret == -ETIMEDOUT ||
                    account_ret == -ECANCELED) {
                    backend_terminal = true;
                    send_ret = account_ret;
                }
                /* The frame physically went out: the relay core already
                 * armed its ordinary ACK deadline for this retransmit, so a
                 * missed short ACK slice must not be reclassified as an RF
                 * send failure with its sub-second retry bank. */
                direct_gateway_ack_wait_owned =
                    !backend_terminal && direct_gateway_retransmit &&
                    backend_rf_started && send_ret == -EAGAIN &&
                    mesh_relay_tx_active(&mesh_runtime) &&
                    mesh_packet_delivery_identity_matches(
                        &mesh_runtime.pending.packet,
                        &retransmit->packet);
            }

            if (send_ret == 0) {
                app_mesh_rf_retry_note_success(&mesh_retransmit_rf_retry,
                                               &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime,
                                        retransmit,
                                        k_uptime_get_32());
                if (channel9_replanned) {
                    mesh_note_channel9_local_tx(retransmit->next_hop_id,
                                                plan.start_ms);
                }
            } else if (direct_gateway_ack_wait_owned) {
                /* The payload RF send succeeded. A timeout, RX error, or full
                 * RX queue in the fast synchronous ACK slice cannot turn that
                 * completed send into a generic RF failure: doing so erases
                 * gateway-failure accounting and creates an unbounded
                 * sub-second retransmission loop. The relay core keeps this
                 * exact packet and all retry policy; it is only told that the
                 * ACK window this sender listened through has elapsed, so its
                 * ordinary miss handling starts at the end of that window
                 * instead of ROUTE_GATEWAY_ACK_TIMEOUT_MS after the frame. */
                int ack_window_ret =
                    mesh_relay_note_gateway_ack_window_elapsed(
                        &mesh_runtime,
                        retransmit,
                        mesh_direct_gateway_ack_window_end_ms(
                            retransmit,
                            &backend_observation));

                app_mesh_rf_retry_note_success(&mesh_retransmit_rf_retry,
                                               &retry_key);
                status_debug_printf(
                    "DBG_RETRANSMIT_DIRECT_GW_ACK_WINDOW seq=%u deadline=%u window=%d\n",
                    retransmit->packet.seq,
                    mesh_runtime.pending.gateway_ack_deadline_ms,
                    ack_window_ret);
                mesh_schedule_tx_timeout();
            } else if (!backend_terminal &&
                       backend_ret >= 0) {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_retransmit_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "retransmit-send-failed");
                int defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    k_uptime_get_32() + retry_delay_ms);

                status_debug_printf("DBG_RETRANSMIT_DEFER reason=send-failed ret=%d send=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    send_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        if (mesh_id_is_unicast(result->route_discovery_target_id) &&
            result->route_discovery_target_id != DEVICE_ID) {
            status_debug_printf("DBG_ROUTE_REQ_UPSTREAM_REPAIR target=0x%llx\n",
                                (unsigned long long)result->route_discovery_target_id);
            if (mesh_channel9_connection_count() == 0u) {
                (void)mesh_schedule_route_request(
                    result->route_discovery_target_id,
                    "rx-route-request-upstream");
            } else {
                mesh_schedule_route_waiting_retry_after("rx-route-request-ch9-wait",
                                                        MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            }
        } else {
            LOG_WRN("mesh route discovery needed after delivery failure");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        const uint64_t route_ready_target_id =
            result->route_discovery_target_id;
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);
        uint64_t route_ready_next_hop_id = selected != NULL ? selected->next_hop_id : 0u;
        uint32_t previous_route_ready_generation =
            (uint32_t)atomic_get(&mesh_route_ready_generation);
        bool deferred_peer_valid =
            mesh_route_ready_event_owner_matches(
                &mesh_route_ready_event_owner,
                mesh_route_ready_event_owner.peer_id,
                previous_route_ready_generation);
        uint64_t deferred_peer_id = deferred_peer_valid ?
            mesh_route_ready_event_owner.peer_id : 0u;
        uint32_t route_ready_generation =
            (uint32_t)atomic_inc(&mesh_route_ready_generation) + 1u;
        const struct app_mesh_route_ready_handoff_state route_ready_state = {
            .selected_route_valid = selected != NULL,
            .selected_timing_valid =
                selected != NULL && selected->channel9_timing_valid,
            .selected_is_unscheduled_gateway =
                route_ready_next_hop_id == GATEWAY_ID,
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = deferred_peer_valid,
            .selected_peer_id = route_ready_next_hop_id,
            .deferred_peer_id = deferred_peer_id,
        };
        struct app_mesh_route_ready_handoff_result route_ready_result;

        (void)mesh_complete_async_route_request_for_ready_target(
            route_ready_target_id);

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_READY\n");
        }
        app_mesh_test_note_route_ready(route_ready_target_id,
                                       route_ready_next_hop_id,
                                       result->status);
        app_mesh_route_ready_handoff_on_ready(&route_ready_state,
                                              &route_ready_result);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_READY_DECISION sel=%u timing=%u next=0x%llx rxq=%u def=0x%llx now=%u drain=%u waittx=%u prop=%u clear=%u\n",
                                route_ready_state.selected_route_valid ? 1u : 0u,
                                route_ready_state.selected_timing_valid ? 1u : 0u,
                                (unsigned long long)route_ready_state.selected_peer_id,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                (unsigned long long)deferred_peer_id,
                                route_ready_result.propose_now ? 1u : 0u,
                                route_ready_result.schedule_rx_drain ? 1u : 0u,
                                route_ready_result.try_waiting_tx ? 1u : 0u,
                                route_ready_result.propose_deferred ? 1u : 0u,
                                route_ready_result.clear_route_reply_handoff ? 1u : 0u);
        }
        if (route_ready_result.clear_route_reply_handoff) {
            mesh_route_reply_handoff_clear("route-ready");
        }
        if (route_ready_result.clear_deferred_peer) {
            mesh_route_ready_event_owner_clear();
        }
        LOG_INF("mesh reactive route ready");
        if (route_ready_result.propose_now) {
            int propose_ret;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_PROPOSE_NOW\n");
                LOG_INF("mesh route-ready to event-control gap: next=0x%016llx delay_ms=%u",
                        (unsigned long long)route_ready_result.peer_id,
                        MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
                k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            }
            propose_ret = mesh_propose_event_after_channel5_contact(route_ready_result.peer_id,
                                                                    "route-ready-event-propose");
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                app_mesh_route_ready_handoff_after_proposal(propose_ret,
                                                            &route_ready_result);
                if (route_ready_result.schedule_event_accept_wait) {
                    mesh_schedule_route_ready_event_retry(
                        &route_ready_result,
                        "route-ready-event-accept-wait");
                    return;
                }
            }
        }
        if (route_ready_result.schedule_rx_drain) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_RX_PENDING\n");
                mesh_route_ready_event_owner_set(
                    &mesh_route_ready_event_owner,
                    route_ready_result.peer_id,
                    route_ready_generation);
            }
            LOG_INF("mesh route ready deferred until queued RX control frames drain");
            mesh_schedule_route_waiting_retry("route-ready-rx-drain");
        } else if (route_ready_result.try_waiting_tx) {
            mesh_schedule_route_waiting_retry_after("route-ready", 0u);
            (void)mesh_schedule_tx_timeout();
        }
    }
#if DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_CLICKER
    if (result->actions &
        MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED) {
        struct discovery_assignment_result assignment = {0};
        const uint64_t accepted_at_ms =
            rx == NULL ? (uint64_t)k_uptime_get() : rx->first_received_at_ms;
        /* Normal assignment responses are retained in relay pending state;
         * confirmed_payload is populated only for the separate gateway-ACK-
         * confirm wire image.  Classify this exact response before terminal
         * commit clears it. */
        const bool table_response_custody =
            confirmed_packet != NULL &&
            confirmed_packet->msg_type == MSG_COMMAND_RESULT &&
            discovery_assignment_parse_result_tlvs(
                mesh_runtime.pending.payload,
                mesh_runtime.pending.payload_len,
                &assignment) == PROTO_OK &&
            assignment.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK;
        const uint64_t custody_parent =
            mesh_runtime.outbox_record.custody_parent;
        int range_ret;
        int producer_ret = -ENOENT;
        int commit_ret;
        int comm_ret;

        range_ret = mesh_anchor_range_report_note_gateway_confirmed(
            confirmed_packet, confirmed_digest);
        if (!mesh_terminal_cleanup_idempotent_status(range_ret)) {
            LOG_ERR("next-hop custody range cleanup failed: ret=%d",
                    range_ret);
            app_watchdog_stop_feeding();
            mesh_schedule_tx_timeout();
            goto gateway_confirmation_done;
        }
        if (confirmed_packet != NULL && confirmed_digest != NULL &&
            confirmed_packet->src_id == DEVICE_ID &&
            mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_delivery_gateway_confirmed != NULL) {
            /* This callback records only the exact response as transport-
             * terminal. END remains responsible for promoting its pending
             * slot, while ABORT can still clear the provisional snapshot. */
            producer_ret =
                mesh_report_callbacks->anchor_delivery_gateway_confirmed(
                    confirmed_packet, confirmed_digest);
        }
        if (!mesh_terminal_cleanup_idempotent_status(producer_ret)) {
            LOG_ERR("next-hop custody producer cleanup failed: ret=%d",
                    producer_ret);
            app_watchdog_stop_feeding();
            mesh_schedule_tx_timeout();
            goto gateway_confirmation_done;
        }
        commit_ret = mesh_relay_commit_next_hop_custody_terminal(
            &mesh_runtime,
            confirmed_packet,
            confirmed_digest);
        if (commit_ret != PROTO_OK) {
            LOG_ERR("next-hop custody core commit failed: ret=%d",
                    commit_ret);
            app_watchdog_stop_feeding();
            mesh_schedule_tx_timeout();
            goto gateway_confirmation_done;
        }
        comm_ret = app_node_comm_note_gateway_confirmed_digest_at(
            confirmed_packet, confirmed_digest, accepted_at_ms);
        if (!mesh_terminal_cleanup_idempotent_status(comm_ret)) {
            LOG_ERR("next-hop custody node communication cleanup failed: ret=%d",
                    comm_ret);
            app_watchdog_stop_feeding();
        }
        status_debug_printf(
            "DBG_NEXT_HOP_CUSTODY type=0x%02x src=0x%llx session=%u seq=%u parent=0x%llx accepted=%llu now=%u\n",
            confirmed_packet == NULL ? 0u : confirmed_packet->msg_type,
            (unsigned long long)(confirmed_packet == NULL ? 0u :
                                 confirmed_packet->src_id),
            confirmed_packet == NULL ? 0u : confirmed_packet->session_id,
            confirmed_packet == NULL ? 0u : confirmed_packet->seq,
            (unsigned long long)(confirmed_packet == NULL ? 0u :
                                 custody_parent),
            (unsigned long long)accepted_at_ms,
            k_uptime_get_32());
        if (table_response_custody) {
            status_debug_printf(
                "DBG_ENUM_NEXT_HOP_CUSTODY seq=%u parent=0x%llx\n",
                confirmed_packet == NULL ? 0u : confirmed_packet->seq,
                (unsigned long long)(confirmed_packet == NULL ? 0u :
                                     custody_parent));
        }
        if (table_response_custody) {
            if (mesh_id_is_unicast(custody_parent) &&
                custody_parent != GATEWAY_ID) {
                mesh_retire_assignment_channel9_peer(
                    custody_parent, "assignment-table-custody");
            }
        }
        if (!table_response_custody &&
            app_node_comm_pending_delivery_count() == 0u &&
            report_tx_queue_used() == 0u &&
            !mesh_route_waiting_tx_valid &&
            !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
            const struct route_candidate *selected =
                route_selected(&mesh_runtime.upstream);
            uint64_t parent_id =
                selected != NULL ? selected->next_hop_id : custody_parent;

            if (mesh_id_is_unicast(parent_id) &&
                parent_id != GATEWAY_ID) {
                mesh_close_channel9_connection(
                    parent_id, "next-hop-custody-complete");
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions &
        MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) {
        int accepted_ret = -ENOENT;

        if (confirmed_packet != NULL && confirmed_digest != NULL &&
            confirmed_packet->src_id == DEVICE_ID &&
            mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_delivery_gateway_accepted != NULL) {
            accepted_ret =
                mesh_report_callbacks->anchor_delivery_gateway_accepted(
                    confirmed_packet, confirmed_digest);
        }
        if (accepted_ret < 0 && accepted_ret != -ENOENT) {
            LOG_ERR("ACK-confirm blocked by durable acceptance: type=%u session=%u seq=%u ret=%d",
                    confirmed_packet == NULL ? 0u :
                        confirmed_packet->msg_type,
                    confirmed_packet == NULL ? 0u :
                        confirmed_packet->session_id,
                    confirmed_packet == NULL ? 0u :
                        confirmed_packet->seq,
                    accepted_ret);
            app_watchdog_stop_feeding();
        } else {
            LOG_INF("gateway ACK-confirm RAM custody retained");
            mesh_schedule_tx_timeout();
        }
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        const bool locally_originated =
            confirmed_packet != NULL && confirmed_packet->src_id == DEVICE_ID;
        int comm_ret;

        if (confirmed_packet != NULL &&
            confirmed_packet->msg_type == MSG_GATEWAY_ACK_CONFIRM) {
            int complete_ret = mesh_complete_gateway_ack_confirm(
                confirmed_packet,
                confirmed_payload,
                confirmed_payload_len,
                rx == NULL ? (uint64_t)k_uptime_get() :
                             rx->first_received_at_ms);

            if (complete_ret < 0) {
                LOG_WRN("gateway ACK-confirm terminal cleanup retained: ret=%d",
                        complete_ret);
                mesh_schedule_tx_timeout();
                goto gateway_confirmation_done;
            }
            /* This source owns teardown. Keep one parent cadence across a
             * burst of queued results, and close it only after the final
             * result's ACK_CONFIRM has itself reached the gateway. */
            if (app_node_comm_pending_delivery_count() == 0u &&
                report_tx_queue_used() == 0u &&
                !mesh_route_waiting_tx_valid &&
                !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
                const struct route_candidate *selected =
                    route_selected(&mesh_runtime.upstream);
                uint64_t parent_id =
                    selected != NULL ? selected->next_hop_id : 0u;

                if (mesh_id_is_unicast(parent_id) &&
                    parent_id != GATEWAY_ID) {
                    mesh_close_channel9_connection(
                        parent_id, "operation-burst-confirmed");
                }
            }
            LOG_INF("mesh pending TX gateway confirmation committed");
            mesh_schedule_tx_timeout();
            goto gateway_confirmation_done;
        }

        comm_ret = locally_originated && confirmed_digest != NULL ?
            app_node_comm_note_gateway_confirmed_digest_at(
                confirmed_packet,
                confirmed_digest,
                rx == NULL ? (uint64_t)k_uptime_get() :
                             rx->first_received_at_ms) :
            locally_originated ? -EBADMSG : -ENOENT;
        if (!mesh_terminal_cleanup_idempotent_status(comm_ret)) {
            LOG_WRN("node communication gateway confirmation rejected: ret=%d",
                    comm_ret);
            mesh_schedule_tx_timeout();
            goto gateway_confirmation_done;
        }
        LOG_INF("mesh pending legacy TX gateway acknowledged");
        if (app_node_comm_pending_delivery_count() == 0u &&
            report_tx_queue_used() == 0u &&
            !mesh_route_waiting_tx_valid &&
            !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
            const struct route_candidate *selected =
                route_selected(&mesh_runtime.upstream);
            uint64_t parent_id =
                selected != NULL ? selected->next_hop_id : 0u;

            if (mesh_id_is_unicast(parent_id) &&
                parent_id != GATEWAY_ID) {
                mesh_close_channel9_connection(
                    parent_id, "uplink-burst-confirmed");
            }
        }
        mesh_schedule_tx_timeout();
    }
gateway_confirmation_done:
#endif
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) {
        LOG_INF("mesh pending TX hop progress acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RELAY_BUSY) {
        LOG_INF("mesh pending TX deferred by relay busy hint");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_DEFERRED) {
        /* A refusal is an answered turn, not a failed parent. The relay
         * retained custody and selected its exact retry_after_ms deadline. */
        mesh_schedule_tx_timeout();
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_RETRY) {
        LOG_INF("mesh pending collection result scheduled for EACK retry");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_CLOSED) {
        LOG_INF("mesh pending collection result stopped after collection close");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL) {
        enum node_comm_terminal_reason reason =
            (result->status ==
                 MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED ||
             result->status == MESH_RELAY_ERR_OUTBOX_EXPIRED) ?
                NODE_COMM_TERMINAL_DEADLINE_EXPIRED :
                result->status ==
                    MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED ?
                NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED :
                NODE_COMM_TERMINAL_PERMANENT_FAILURE;
        int complete_ret;

        LOG_WRN("mesh result grant wait ended: status=%d reason=%u src=0x%016llx session=%u seq=%u",
                result->status,
                (unsigned int)reason,
                (unsigned long long)result->terminal.packet.src_id,
                result->terminal.packet.session_id,
                result->terminal.packet.seq);
        complete_ret = mesh_complete_terminal_release(
            &result->terminal, reason);
        if (complete_ret < 0) {
            LOG_WRN("mesh terminal RAM cleanup retained: ret=%d status=%d",
                    complete_ret, result->status);
        }
        mesh_schedule_tx_timeout();
    }
#endif
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    if (mesh_relay_result_bundle_pending(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }
#endif
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_route_waiting_retry_after("result-actions-complete", 0u);
    }
}


static uint32_t mesh_drain_rx_queue_locked(const char *owner)
{
    struct mesh_rx_pending *pending = &mesh_rx_work_pending;
    struct mesh_relay_result *result = &mesh_work_result;
    uint32_t handled_count = 0u;
    int ret;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_ENTER owner=%s q=%u\n",
                            owner == NULL ? "rx" : owner,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    while (k_msgq_get(&mesh_rx_msgq, pending, K_NO_WAIT) == 0) {
        uint32_t now_ms = k_uptime_get_32();
        bool delivered_event_control = false;
        bool admitted_event_control = false;
#if DEVICE_ROLE == ROLE_GATEWAY
        bool gateway_semantic_delivery_processed = false;
#endif
        struct proto_packet confirmed_packet = {0};
        uint8_t confirmed_payload[
            MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN] = {0};
        uint8_t confirmed_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
        size_t confirmed_payload_len = 0u;
        bool confirmed_packet_valid = false;
        bool confirmed_digest_valid = false;

        if (mesh_relay_tx_active(&mesh_runtime) &&
            mesh_runtime.pending.gateway_ack_confirm_pending) {
            int snapshot_ret =
                mesh_relay_pending_gateway_ack_confirm_wire(
                    &mesh_runtime,
                    now_ms,
                    &confirmed_packet,
                    confirmed_payload,
                    sizeof(confirmed_payload),
                    &confirmed_payload_len);

            if (snapshot_ret == PROTO_OK) {
                confirmed_packet_valid = true;
            } else {
                /* Do not fall back to the original packet here: a matching
                 * ACK is terminal proof for the transient confirm wire image,
                 * while producer cleanup must still name the immutable
                 * original encoded inside that image. */
                LOG_ERR("gateway ACK-confirm snapshot failed: ret=%d",
                        snapshot_ret);
            }
        } else if (mesh_relay_tx_active(&mesh_runtime)) {
            confirmed_packet_valid = true;
            confirmed_packet = mesh_runtime.pending.packet;
            confirmed_digest_valid = mesh_packet_semantic_digest(
                &mesh_runtime.pending.packet,
                mesh_runtime.pending.payload,
                mesh_runtime.pending.payload_len,
                confirmed_digest);
            if (confirmed_packet.msg_type == MSG_GATEWAY_ACK_CONFIRM &&
                mesh_runtime.pending.payload_len <=
                    sizeof(confirmed_payload)) {
                confirmed_payload_len =
                    mesh_runtime.pending.payload_len;
                memcpy(confirmed_payload,
                       mesh_runtime.pending.payload,
                       confirmed_payload_len);
            }
        }

        memset(result, 0, sizeof(*result));
        mesh_rx_pending_refresh_age(pending, now_ms);
        /* A peer TX slot can carry ACKs, telemetry, or control; ACK matching is opportunistic. */

        if (pending->packet.msg_type == MSG_ROUTE_REQ) {
            ret = mesh_relay_validate_route_request(
                &mesh_runtime,
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->previous_hop_id,
                now_ms);
            if (ret != PROTO_OK) {
                status_debug_printf(
                    "DBG_ROUTE_REQ_SEMANTIC_REJECT prev=0x%llx seq=%u ret=%d\n",
                    (unsigned long long)pending->previous_hop_id,
                    pending->packet.seq,
                    ret);
                goto mesh_rx_item_cleanup;
            }
        }
        if (mesh_gateway_route_test_should_reject_route_request(pending)) {
            goto mesh_rx_item_cleanup;
        }
#if DEVICE_ROLE == ROLE_GATEWAY
        /* Queue admission already validated the full mesh semantics. Observe
         * a source reboot before ACK-history capacity admission, otherwise
         * four obsolete confirmation debts can reject their own boot proof. */
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->gateway_note_anchor_boot_observation !=
                NULL &&
            pending->packet.msg_type == MSG_ANCHOR_HEARTBEAT) {
            mesh_report_callbacks->gateway_note_anchor_boot_observation(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->first_received_at_ms);
        }
#endif
        mesh_c5_update_credit();
        ret = mesh_relay_handle_rx_with_random_radio(
            &mesh_runtime,
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->previous_hop_id,
            pending->link_quality,
            pending->link_rsl_dbm,
            pending->link_rsl_valid,
            now_ms,
            sys_rand32_get(),
            result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            goto mesh_rx_item_cleanup;
        }
#if DEVICE_ROLE == ROLE_CLICKER
        {
            const uint32_t allowed_actions =
                MESH_RELAY_ACTION_DROP |
                MESH_RELAY_ACTION_DELIVER_LOCAL |
                MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED |
                MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK |
                MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY |
                MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING |
                MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED |
                MESH_RELAY_ACTION_TX_HOP_PROGRESS |
                MESH_RELAY_ACTION_TX_RELAY_BUSY |
                MESH_RELAY_ACTION_TX_HOP_DEFERRED;

            if ((result->actions & ~allowed_actions) != 0u ||
                ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                 !mesh_packet_is_event_control_type(
                     pending->packet.msg_type))) {
                LOG_ERR("clicker leaf rejected unexpected mesh action set: msg=0x%02x actions=0x%08x",
                        pending->packet.msg_type,
                        result->actions);
                goto mesh_rx_item_cleanup;
            }
        }
#endif

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx session=%u seq=%u actions=0x%08x status=%d",
                pending->packet.msg_type,
                (unsigned long long)pending->packet.src_id,
                (unsigned long long)pending->packet.dst_id,
                (unsigned long long)pending->previous_hop_id,
                pending->packet.session_id,
                pending->packet.seq,
                result->actions,
                result->status);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            (pending->packet.msg_type == MSG_ROUTE_REQ ||
             pending->packet.msg_type == MSG_GATEWAY_ROUTE_ADV ||
             pending->packet.msg_type == MSG_ROUTE_REPLY ||
             pending->packet.msg_type == MSG_ROUTE_REPLY_ACK ||
             pending->packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
             pending->packet.msg_type == MSG_MESH_EVENT_ACCEPT)) {
            const struct route_candidate *selected =
                route_selected(&mesh_runtime.upstream);

            status_debug_printf("DBG_RX_HANDLE msg=0x%02x act=0x%08x st=%d q=%u sel=%u next=0x%llx hop=%u\n",
                                pending->packet.msg_type,
                                result->actions,
                                result->status,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                selected != NULL ? 1u : 0u,
                                (unsigned long long)(selected != NULL ?
                                    selected->next_hop_id : 0u),
                                selected != NULL ? selected->hop_count : 0u);
        }
        if (result->status == PROTO_ERR_NOT_FOUND &&
            pending->packet.dst_id != DEVICE_ID &&
            pending->packet.dst_id != MESH_BROADCAST_ID) {
            if ((result->actions &
                 MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_ROUTE_REPAIR) != 0u) {
                struct app_mesh_c5_tx_authorization_token authorization;
                int schedule_ret;

                if (!app_mesh_ch9_c5_repair_authorization_capture(
                        &authorization,
                        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
                        &mesh_runtime.pending,
                        mesh_relay_tx_active(&mesh_runtime),
                        NULL,
                        pending->packet.dst_id)) {
                    LOG_ERR("matching transit gateway ACK could not mint child-route repair owner: peer=0x%016llx",
                            (unsigned long long)pending->packet.dst_id);
                    app_watchdog_stop_feeding();
                } else {
                    schedule_ret = mesh_schedule_route_request_authorized(
                        pending->packet.dst_id,
                        "transit-gateway-ack-route-repair",
                        NULL,
                        &authorization);
                    if (schedule_ret < 0) {
                        LOG_ERR("transit gateway ACK child-route repair could not retain owner: peer=0x%016llx ret=%d",
                                (unsigned long long)pending->packet.dst_id,
                                schedule_ret);
                        app_watchdog_stop_feeding();
                    }
                }
            } else {
                (void)mesh_schedule_route_request(pending->packet.dst_id,
                                                  "rx-forward-miss");
            }
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            pending->packet.msg_type == MSG_ROUTE_SOLICIT) {
            mesh_gateway_note_route_solicit(pending->packet.src_id);
            goto mesh_rx_item_cleanup;
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_packet_is_event_control_type(pending->packet.msg_type)) {
            delivered_event_control = true;
            admitted_event_control = mesh_handle_event_control(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->previous_hop_id,
                mesh_rx_pending_physical_received_at_ms(pending));
        }
        if (mesh_c5_control_rx_semantically_admitted(
                &pending->packet,
                result,
                admitted_event_control) &&
            pending->packet.msg_type != MSG_MESH_EVENT_PROPOSE) {
            mesh_note_c5_control_rx(&pending->packet,
                                    pending->previous_hop_id,
                                    pending->radio_channel,
                                    "c5-control-rx");
        }
#if DEVICE_ROLE == ROLE_GATEWAY
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_gateway_delivery_requires_semantic_acceptance(
                &pending->packet)) {
            bool internal_control =
                mesh_gateway_delivery_is_internal_control(pending);
            bool host_custody_ready = false;
            bool semantic_preflight_complete = false;
            bool stream_reservation_acquired = false;
            uint8_t host_projection_mask = 0u;
            int reservation_ret = 0;
            int semantic_ret = -EINVAL;

            /*
             * The GUI is the external RAM for host history.  Firmware only
             * admits a semantic packet after it owns the retained BLE item;
             * collection state remains in RAM for EACK retry, but no flash
             * journal or receipt ledger participates in this decision.
             */
            mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
            gateway_semantic_delivery_processed = true;
            status_debug_printf("DBG_GWLAT_DRAIN_ENTER at=%u rx=%u msg=0x%02x ctl=%u\n",
                                k_uptime_get_32(),
                                pending->received_at_ms,
                                pending->packet.msg_type,
                                internal_control ? 1u : 0u);
            semantic_ret = mesh_gateway_preflight_semantic_delivery(pending);
            semantic_preflight_complete = true;
            if (internal_control) {
                host_custody_ready = true;
                /* Assignment control results do not need host projection,
                 * but they still need the same pure admission pass as every
                 * other command result. A reboot can leave an anchor retrying
                 * an exact old assignment result after the gateway's volatile
                 * operation owner is gone. Preflight classifies that packet
                 * as RECOVERED_RAW, allowing only the transport ACK that
                 * retires source custody; a live matching assignment still
                 * reaches the mutating dispatcher below. */
            } else {
                if (semantic_ret == APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                    pending->packet.msg_type == MSG_RESULT_BUNDLE) {
                    int projection_ret = gateway_result_bundle_host_projection_mask(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->previous_hop_id,
                        &host_projection_mask);

                    if (projection_ret < 0) {
                        semantic_ret = projection_ret;
                    }
                }
                if (semantic_ret < 0 ||
                    semantic_ret == APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE) {
                    /* A negative preflight and an active collection redrive
                     * don't need another host record. */
                    host_custody_ready = true;
                } else {
                    reservation_ret = gateway_ble_reserve_stream_packet(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->received_at_ms);
                    stream_reservation_acquired =
                        reservation_ret ==
                            GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED;
                    host_custody_ready = stream_reservation_acquired;
                    status_debug_printf("DBG_GWLAT_RESV at=%u ret=%d ok=%u\n",
                                        k_uptime_get_32(),
                                        reservation_ret,
                                        stream_reservation_acquired ? 1u : 0u);
                }
            }
            if (!host_custody_ready) {
                                status_debug_printf("DBG_GATEWAY_NO_ACK reason=deferred-ble-custody msg=0x%02x src=0x%llx ret=%d\n",
                                    pending->packet.msg_type,
                                    (unsigned long long)pending->packet.src_id,
                                    reservation_ret);
LOG_WRN("gateway semantic delivery deferred for BLE custody: msg=0x%02x src=0x%016llx ret=%d",
                        pending->packet.msg_type,
                        (unsigned long long)pending->packet.src_id,
                        reservation_ret);
            } else {
                /*
                 * A completed negative pure preflight must reach rejection
                 * before PREPARE or the mutating semantic dispatcher.
                 */
                if (semantic_preflight_complete && semantic_ret < 0) {
                    goto gateway_semantic_result_ready;
                }
                if (!stream_reservation_acquired &&
                    semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE &&
                    semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                    semantic_ret =
                        mesh_gateway_commit_preflighted_semantic_delivery(
                            pending);
                    if (semantic_ret >= 0 &&
                        semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                        semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
                        semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW &&
                        semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE &&
                        semantic_ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                        semantic_ret = -EINVAL;
                    }
                }
gateway_semantic_result_ready:
                if (semantic_ret < 0) {
                    if (stream_reservation_acquired) {
                        gateway_ble_cancel_stream_reservation();
                    }
                                        status_debug_printf("DBG_GATEWAY_NO_ACK reason=rejected msg=0x%02x src=0x%llx ret=%d\n",
                                        pending->packet.msg_type,
                                        (unsigned long long)pending->packet.src_id,
                                        semantic_ret);
LOG_WRN("gateway semantic delivery rejected without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                    goto mesh_rx_item_cleanup;
                } else if (stream_reservation_acquired) {
                    /*
                     * RAM admission is the ACK boundary.  The retained BLE
                     * stream record is the host's copy; publication and the
                     * GUI receipt only retire that record and never gate
                     * mesh custody, so one absent host can never stall the
                     * radio behind a single report.
                     */
                    if (semantic_ret ==
                        APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                        ret = gateway_collection_recovery_reserve_host_custody(
                            &pending->packet,
                            pending->payload,
                            pending->payload_len);
                        if (ret < 0) {
                            gateway_ble_cancel_stream_reservation();
                            LOG_WRN("gateway stale collection recovery reservation retained: src=0x%016llx seq=%u ret=%d",
                                    (unsigned long long)pending->packet.src_id,
                                    pending->packet.seq,
                                    ret);
                            goto mesh_rx_item_cleanup;
                        }
                    }
                    ret = gateway_ble_commit_stream_reservation_projection(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        host_projection_mask);
                    if (ret != 1) {
                        if (semantic_ret ==
                            APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                            int cancel_ret =
                                gateway_collection_recovery_cancel_host_custody(
                                    &pending->packet,
                                    pending->payload,
                                    pending->payload_len);

                            if (cancel_ret < 0) {
                                LOG_ERR("gateway stale collection recovery reservation could not roll back: src=0x%016llx seq=%u ret=%d",
                                        (unsigned long long)pending->packet.src_id,
                                        pending->packet.seq,
                                        cancel_ret);
                                app_watchdog_stop_feeding();
                            }
                        }
                        gateway_ble_cancel_stream_reservation();
                                                status_debug_printf("DBG_GATEWAY_NO_ACK reason=ble-commit-failed msg=0x%02x src=0x%llx ret=%d\n",
                                            pending->packet.msg_type,
                                            (unsigned long long)pending->packet.src_id,
                                            ret);
LOG_ERR("gateway semantic BLE custody commit failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                        goto mesh_rx_item_cleanup;
                    }
                    if (semantic_ret !=
                            APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW &&
                        semantic_ret !=
                            APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                        uint32_t validation_token =
                            pending->result_validation_token;

                        ret = mesh_gateway_commit_preflighted_semantic_delivery(
                            pending);
                        gateway_command_result_validation_release_reserved(
                            validation_token);
                        pending->result_validation_token = 0u;
                        if (ret < 0 ||
                            (ret != APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                             ret != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
                             ret != APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW &&
                             ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE &&
                             ret != APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY)) {
                            /* The host already owns the raw record; the
                             * sender keeps custody and its retry classifies
                             * as a duplicate on the next pass. */
                            LOG_WRN("gateway semantic acceptance failed after host custody; no ACK: msg=0x%02x src=0x%016llx ret=%d",
                                    pending->packet.msg_type,
                                    (unsigned long long)pending->packet.src_id,
                                    ret);
                            goto mesh_rx_item_cleanup;
                        }
                        semantic_ret = ret;
                    } else {
                        gateway_command_result_validation_release_reserved(
                            pending->result_validation_token);
                        pending->result_validation_token = 0u;
                    }
                    if (pending->packet.src_id != DEVICE_ID) {
                        app_stack_workload_diag_gateway_report_cycle(
                            &pending->packet,
                            (uint16_t)k_msgq_num_used_get(&mesh_rx_msgq),
                            1u);
                    }
                    if (semantic_ret == APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                        pending->packet.msg_type == MSG_MESH_DATA) {
                        app_mesh_test_note_gateway_delivery(
                            &pending->packet,
                            pending->payload,
                            pending->payload_len,
                            pending->received_at_ms,
                            k_msgq_num_used_get(&mesh_rx_msgq));
                    }
                } else if (semantic_ret ==
                               APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
                           !internal_control) {
                    /* The same-boot semantic owner already has a host item;
                     * only the retry ACK/EACK needs to be emitted. */
                } else if (!internal_control) {
                    if (stream_reservation_acquired) {
                        gateway_ble_cancel_stream_reservation();
                    }
                                        status_debug_printf("DBG_GATEWAY_NO_ACK reason=invalid-acceptance msg=0x%02x src=0x%llx ret=%d\n",
                                        pending->packet.msg_type,
                                        (unsigned long long)pending->packet.src_id,
                                        semantic_ret);
LOG_ERR("gateway semantic delivery returned invalid acceptance without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                    goto mesh_rx_item_cleanup;
                }

                if (semantic_ret ==
                    APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY) {
                    ret = gateway_collection_recovery_after_host_receipt(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    if (ret == 0) {
                        ret = gateway_collection_recovery_finish_host_delivery(
                            &pending->packet,
                            pending->payload,
                            pending->payload_len);
                    }
                    if (ret < 0) {
                        LOG_ERR("gateway stale collection recovery failed closed: src=0x%016llx seq=%u ret=%d",
                                (unsigned long long)pending->packet.src_id,
                                pending->packet.seq,
                                ret);
                        app_watchdog_stop_feeding();
                        goto mesh_rx_item_cleanup;
                    }
                    mesh_restart_role_scan();
                } else {
                    ret = semantic_ret ==
                              APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW ?
                          0 :
                          gateway_finalize_semantic_delivery(
                              &pending->packet,
                              pending->payload,
                              pending->payload_len,
                              pending->previous_hop_id,
                              pending->radio_channel,
                              pending->current_channel9_plan_valid ?
                              &pending->current_channel9_plan : NULL,
                              semantic_ret);
                    if (ret < 0) {
                                                status_debug_printf("DBG_GATEWAY_NO_ACK reason=finalize-failed msg=0x%02x src=0x%llx ret=%d\n",
                                            pending->packet.msg_type,
                                            (unsigned long long)pending->packet.src_id,
                                            ret);
LOG_WRN("gateway semantic finalization failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                        goto mesh_rx_item_cleanup;
                    }
                    ret = mesh_relay_commit_gateway_delivery(
                        &mesh_runtime,
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->previous_hop_id,
                        k_uptime_get_32(),
                        result);
                    status_debug_printf("DBG_GWLAT_COMMIT at=%u ret=%d sem=%d\n",
                                        k_uptime_get_32(),
                                        ret,
                                        semantic_ret);
                    if (ret != PROTO_OK) {
                        LOG_WRN("gateway semantic delivery commit failed: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                    }
                }
            }
        }
#endif
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            pending->packet.msg_type == MSG_GATEWAY_ACK) {
            if ((result->actions &
                 (MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED |
                  MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED)) != 0u) {
                status_debug_note("DBG_LEGACY_ACK_CONFIRMED\n");
            } else if (mesh_relay_tx_active(&mesh_runtime)) {
                status_debug_note("DBG_LEGACY_ACK_NO_MATCH\n");
            } else {
                status_debug_note("DBG_LEGACY_ACK_IDLE\n");
            }
        }
            mesh_handle_result_actions(result, pending->radio_channel, pending,
                                       confirmed_packet_valid ?
                                       &confirmed_packet : NULL,
                                       confirmed_payload_len > 0u ?
                                           confirmed_payload : NULL,
                                       confirmed_payload_len,
                                       confirmed_digest_valid ?
                                           confirmed_digest : NULL,
                                       NULL);
        if (mesh_transport_paused()) {
            goto mesh_rx_item_cleanup;
        }
        if (delivered_event_control) {
            goto mesh_rx_item_cleanup;
        }
        /*
         * Forwarding a broadcast can take long enough to materially advance
         * its protocol age. Keep the delivery timestamp and message age at
         * the same instant so local timing reconstruction does not restart
         * the operation after a relay operation.
         */
        mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
#if DEVICE_ROLE == ROLE_GATEWAY
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            !gateway_semantic_delivery_processed) {
            if (pending->packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending->packet,
                                            pending->payload,
                                            pending->payload_len,
                                            pending->first_received_at_ms,
                                            pending->result_validation_token,
                                            pending->previous_hop_id,
                                            pending->radio_channel,
                                            pending->current_channel9_plan_valid ?
                                            &pending->current_channel9_plan :
                                            NULL);
            } else if (pending->packet.msg_type == MSG_RESULT_BUNDLE) {
                gateway_note_command_result_bundle(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len,
                                                   pending->previous_hop_id,
                                                   pending->radio_channel,
                                                   pending->current_channel9_plan_valid ?
                                                   &pending->current_channel9_plan :
                                                   NULL);
            }
            app_mesh_test_note_gateway_delivery(&pending->packet,
                                                pending->payload,
                                                pending->payload_len,
                                                pending->received_at_ms,
                                                k_msgq_num_used_get(&mesh_rx_msgq));
            ret = gateway_ble_stream_packet(&pending->packet,
                                            pending->payload,
                                            pending->payload_len,
                                            pending->received_at_ms);
            if (ret < 0) {
                LOG_DBG("gateway BLE stream packet not queued: %d", ret);
            }
        }
#endif
#if DEVICE_ROLE == ROLE_ANCHOR
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u) {
            ret = mesh_report_anchor_handle_local_command(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->previous_hop_id);
            if (ret > 0 &&
                pending->packet.msg_type == MSG_COMMAND &&
                pending->packet.dst_id == DEVICE_ID) {
                ret = mesh_relay_commit_anchor_command_delivery(
                    &mesh_runtime,
                    &pending->packet,
                    pending->payload,
                    pending->payload_len,
                    k_uptime_get_32());
                if (ret != PROTO_OK) {
                    LOG_ERR("anchor command semantic commit failed: ret=%d",
                            ret);
                }
            }
        }
#endif
        handled_count++;
mesh_rx_item_cleanup:
        gateway_command_result_validation_release_reserved(
            pending->result_validation_token);
        if (mesh_transport_paused()) {
            break;
        }
    }
    mesh_c5_flush_batch_ack();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_EXIT owner=%s handled=%u q=%u\n",
                            owner == NULL ? "rx" : owner,
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    return handled_count;
}
