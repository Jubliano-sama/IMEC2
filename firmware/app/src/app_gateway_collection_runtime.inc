#line 1209 __BASE_FILE__
static int gateway_persist_collection_state(const char *reason)
{
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_membership_restore_pending ||
        gateway_collection_restore_pending ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending) {
        return -ENOENT;
    }

    ret = app_mesh_persistence_save_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        gateway_collection_persistence_dirty = true;
        gateway_schedule_persistence_retry(reason);
        LOG_WRN("gateway collection snapshot save failed: ret=%d reason=%s",
                ret,
                reason == NULL ? "" : reason);
        return ret;
    } else {
        gateway_collection_persistence_dirty = false;
        gateway_persistence_retry_round = 0u;
    }
    return 0;
}

static bool gateway_collection_tracking_active(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY &&
           gateway_collection_state.gateway_id == DEVICE_ID &&
           gateway_collection_state.command_seq != 0u &&
           gateway_collection_state.collection_epoch_id != 0u;
}

static bool gateway_collection_work_pending(void)
{
    return gateway_collection_recovery_eack_active ||
           gateway_eack_custody_clear_pending ||
           gateway_membership_restore_pending ||
           gateway_collection_restore_pending ||
           gateway_membership_persistence_dirty ||
           gateway_membership_clear_pending ||
           gateway_collection_clear_pending ||
           (gateway_collection_tracking_active() &&
           (gateway_collection_state.collection_open ||
            gateway_collection_state.eack_pending ||
            gateway_collection_eack_retry_state.active ||
            gateway_collection_eack_round_dirty ||
            gateway_collection_persistence_dirty));
}

static void gateway_collection_rollback_failed_start(void)
{
    int ret;

    gateway_collection_persistence_dirty = false;
    gateway_collection_eack_round_dirty = false;
    gateway_persistence_retry_round = 0u;
    ret = app_mesh_persistence_restore_gateway_collection(
        &gateway_collection_state);
    if (ret < 0) {
        gateway_collection_clear(&gateway_collection_state);
        gateway_collection_restore_pending = true;
        gateway_schedule_persistence_retry("collection-start-rollback-restore");
        LOG_WRN("gateway collection start rollback found no durable snapshot: ret=%d",
                ret);
    }
}

static void gateway_schedule_collection_eack_round(void)
{
    int ret;

    if (gateway_eack_custody_clear_pending ||
        !gateway_collection_tracking_active() ||
        !gateway_collection_state.eack_pending) {
        atomic_clear(&gateway_collection_eack_schedule_deferred);
        atomic_clear(&gateway_collection_eack_deferred_delay_ms);
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        return;
    }

    ret = mesh_route_work_reschedule(
        &gateway_collection_eack_work,
        gateway_collection_state.next_retry_spread_ms);
    if (ret < 0) {
        atomic_set(&gateway_collection_eack_deferred_delay_ms,
                   (atomic_val_t)gateway_collection_state.next_retry_spread_ms);
        atomic_set(&gateway_collection_eack_schedule_deferred, 1);
        if (ret != -ESHUTDOWN) {
            LOG_ERR("gateway collection EACK round lost its route owner: ret=%d",
                    ret);
            app_watchdog_stop_feeding();
        }
    } else {
        atomic_clear(&gateway_collection_eack_schedule_deferred);
    }
    if (ret < 0) {
        LOG_WRN("gateway collection EACK round scheduling deferred: ret=%d",
                ret);
    }
}

static void gateway_schedule_collection_eack_retry(const char *reason,
                                                   int ret,
                                                   uint64_t failed_next_hop_id)
{
    uint32_t retry_delay_ms;
    int schedule_ret;

    if (gateway_eack_custody_clear_pending) {
        gateway_schedule_persistence_retry(
            "eack-custody-clear-before-retry");
        return;
    }

    retry_delay_ms = app_gateway_eack_retry_note_failure(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        sys_rand32_get());
    app_gateway_eack_retry_note_failed_channel9_target(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        failed_next_hop_id);
    if (retry_delay_ms == 0u) {
        LOG_ERR("gateway collection EACK retry state invalid: command_seq=%u collection=%u ret=%d reason=%s",
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                ret,
                reason == NULL ? "" : reason);
        app_watchdog_stop_feeding();
        return;
    }

    schedule_ret = mesh_route_work_reschedule(
        &gateway_collection_eack_work, retry_delay_ms);
    if (schedule_ret < 0) {
        atomic_set(&gateway_collection_eack_deferred_delay_ms,
                   (atomic_val_t)retry_delay_ms);
        atomic_set(&gateway_collection_eack_schedule_deferred, 1);
        if (schedule_ret != -ESHUTDOWN) {
            LOG_ERR("gateway collection EACK retry lost its route owner: ret=%d",
                    schedule_ret);
            app_watchdog_stop_feeding();
        }
    } else {
        atomic_clear(&gateway_collection_eack_schedule_deferred);
    }
    LOG_WRN("gateway collection EACK send failure retry: command_seq=%u collection=%u eack_round=%u rf_retry_round=%u delay_ms=%u ret=%d schedule_ret=%d reason=%s",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.retry_round,
            gateway_collection_eack_retry_state.rf_retry.retry_round,
            retry_delay_ms,
            ret,
            schedule_ret,
            reason == NULL ? "" : reason);
}

static int gateway_eack_plan_channel9(uint64_t next_hop_id,
                                      struct mesh_event_plan *plan,
                                      void *ctx)
{
    struct mesh_channel5_requirements requirements;

    ARG_UNUSED(ctx);

    mesh_fill_channel5_requirements(&requirements);
    return mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                next_hop_id,
                                                &requirements,
                                                k_uptime_get_32(),
                                                plan);
}

static int gateway_eack_send_channel9(const struct mesh_outbound *out, void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_send_outbound(out, "collection-eack-channel9");
}

static int gateway_eack_prepare_channel9(struct mesh_outbound *out,
                                         const struct mesh_event_plan *plan,
                                         void *ctx)
{
    uint32_t required_ms = 0u;
    int ret;

    ARG_UNUSED(ctx);

    ret = mesh_prepare_channel9_outbound(out, plan, k_uptime_get_32(), &required_ms);
    if (ret < 0) {
        LOG_DBG("gateway collection EACK channel9 slot unavailable: ret=%d req=%u start=%u end=%u",
                ret,
                required_ms,
                plan == NULL ? 0u : plan->start_ms,
                plan == NULL ? 0u : plan->end_ms);
    }
    return ret;
}

struct gateway_eack_send_context {
    bool c5_rf_started;
};

static int gateway_eack_send_c5_flood(const struct mesh_outbound *out, void *ctx)
{
    struct gateway_eack_send_context *send_context = ctx;
    struct app_mesh_flood_result flood_result = {0};
    struct app_mesh_flood_progress *progress =
        &gateway_collection_eack_retry_state.c5_flood_progress;
    int ret;

    if (send_context == NULL) {
        return -EINVAL;
    }
    send_context->c5_rf_started = false;

    /*
     * EACK retry state is the sole custody owner.  Retain resumable progress
     * across busy periods and release the frozen datagram only after all four
     * real EACK transmissions complete.
     */
    ret = mesh_try_send_c5_flood_resume(
        out,
        C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD,
        "collection-eack-c5",
        progress,
        &flood_result,
        &send_context->c5_rf_started);
    if (progress->initialized && !progress->complete) {
        gateway_collection_eack_retry_state.force_c5_recovery = true;
    }
    if (ret == 0 &&
        (!progress->complete ||
         progress->next_opportunity != app_mesh_flood_repeat_limit() ||
         flood_result.sent_count != app_mesh_flood_repeat_limit())) {
        return -EAGAIN;
    }
    return ret;
}

static void gateway_eack_note_tx_sent(const struct mesh_outbound *out, void *ctx)
{
    const struct gateway_eack_send_context *send_context = ctx;

    if (out != NULL && out->radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        (send_context == NULL || !send_context->c5_rf_started)) {
        return;
    }

    mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
}

static void gateway_eack_note_channel9_tx(uint64_t next_hop_id,
                                          uint32_t event_start_ms,
                                          void *ctx)
{
    ARG_UNUSED(ctx);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_relay_note_channel9_tx(&mesh_runtime,
                                    next_hop_id,
                                    event_start_ms);
    } else {
        mesh_relay_note_channel9_success(&mesh_runtime,
                                         next_hop_id,
                                         event_start_ms);
    }
}

static uint32_t gateway_eack_now_ms(void *ctx)
{
    ARG_UNUSED(ctx);

    return k_uptime_get_32();
}

static int gateway_collection_recovery_input_from_result(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct app_gateway_collection_recovery_eack_input *input)
{
    if (packet == NULL || payload == NULL || input == NULL ||
        packet->payload_len != payload_len ||
        packet->src_id == 0u ||
        packet->seq == 0u ||
        payload_len == 0u ||
        payload_len > UINT16_MAX) {
        return -EINVAL;
    }
    memset(input, 0, sizeof(*input));
    input->packet_src_id = packet->src_id;
    input->packet_seq = packet->seq;
    input->payload_len = (uint16_t)payload_len;
    if (!semantic_digest_sha256(payload,
                                payload_len,
                                input->payload_digest)) {
        return -EIO;
    }

    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        struct result_bundle_header bundle = {0};
        int ret = result_bundle_header_from_tlvs(
            payload, payload_len, &bundle);

        if (ret != PROTO_OK ||
            bundle.gateway_id == 0u ||
            bundle.command_seq == 0u ||
            bundle.collection_epoch_id == 0u ||
            packet->session_id != bundle.command_seq) {
            return -EBADMSG;
        }
        input->gateway_id = bundle.gateway_id;
        input->gateway_epoch = bundle.gateway_epoch;
        input->command_seq = bundle.command_seq;
        input->collection_epoch_id = bundle.collection_epoch_id;
        return 0;
    }
    if (packet->msg_type == MSG_COMMAND_RESULT) {
        struct command_result_id result_id = {0};
        const uint8_t *collection_epoch_raw = NULL;
        uint8_t collection_epoch_len = 0u;
        int ret = command_result_id_from_tlvs(
            payload, payload_len, &result_id);

        if (ret != PROTO_OK) {
            return -EBADMSG;
        }
        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              &collection_epoch_raw,
                              &collection_epoch_len);
        if (ret != PROTO_OK ||
            collection_epoch_len != sizeof(uint32_t) ||
            result_id.gateway_id == 0u ||
            result_id.command_seq == 0u ||
            packet->session_id != result_id.command_seq) {
            return -EBADMSG;
        }
        input->gateway_id = result_id.gateway_id;
        input->gateway_epoch = result_id.gateway_epoch;
        input->command_seq = result_id.command_seq;
        input->collection_epoch_id =
            proto_get_u32_le(collection_epoch_raw);
        return input->collection_epoch_id == 0u ? -EBADMSG : 0;
    }
    return -EINVAL;
}

static bool gateway_collection_recovery_snapshot_matches(
    const struct app_gateway_collection_recovery_eack_input *input)
{
    struct gateway_collection_eack decoded = {0};
    struct gateway_collection_recovery_identity recovery_identity = {0};
    const struct gateway_collection_eack_custody_snapshot *snapshot =
        &gateway_collection_eack_retry_state.snapshot;

    if (input == NULL ||
        !gateway_collection_recovery_eack_active ||
        !gateway_collection_eack_retry_state.active ||
        gateway_collection_eack_custody_validate(snapshot) != PROTO_OK ||
        gateway_collection_eack_packet_validate(
            &snapshot->packet,
            snapshot->payload,
            snapshot->payload_len,
            &decoded) != PROTO_OK ||
        gateway_collection_eack_recovery_identity(
            snapshot->payload,
            snapshot->payload_len,
            &recovery_identity) != PROTO_OK) {
        return false;
    }
    return decoded.gateway_id == input->gateway_id &&
           decoded.gateway_epoch == input->gateway_epoch &&
           decoded.command_seq == input->command_seq &&
           decoded.collection_epoch_id == input->collection_epoch_id &&
           recovery_identity.packet_src_id == input->packet_src_id &&
           recovery_identity.packet_seq == input->packet_seq &&
           recovery_identity.payload_len == input->payload_len &&
           semantic_digest_equal(recovery_identity.payload_digest,
                                 input->payload_digest,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}

static int gateway_import_collection_recovery_eack_custody(void)
{
    struct gateway_collection_eack decoded = {0};
    uint32_t recovery_attempt_id = 0u;
    struct app_gateway_eack_retry_state *state =
        &gateway_collection_eack_retry_state;
    int marker_ret;

    if (!state->snapshot.valid ||
        gateway_collection_eack_custody_validate(
            &state->snapshot) != PROTO_OK ||
        gateway_collection_eack_packet_validate(
            &state->snapshot.packet,
            state->snapshot.payload,
            state->snapshot.payload_len,
            &decoded) != PROTO_OK) {
        return -EBADMSG;
    }
    marker_ret = gateway_collection_eack_recovery_attempt_id(
        state->snapshot.payload,
        state->snapshot.payload_len,
        &recovery_attempt_id);
    if (marker_ret == PROTO_ERR_NOT_FOUND) {
        return -ENOENT;
    }
    if (marker_ret != PROTO_OK ||
        decoded.collection_open) {
        return -EBADMSG;
    }

    /*
     * Preserve the already-restored frozen snapshot while resetting every
     * volatile retry lane. Reset recovery restarts the complete bounded flood
     * with the same immutable attempt; it never rebuilds semantic bytes.
     */
    memset(&state->rf_retry, 0, sizeof(state->rf_retry));
    memset(&state->c5_flood_progress, 0, sizeof(state->c5_flood_progress));
    memset(&state->identity, 0, sizeof(state->identity));
    memset(state->failed_channel9_next_hop_ids,
           0,
           sizeof(state->failed_channel9_next_hop_ids));
    state->failed_channel9_next_hop_count = 0u;
    state->force_c5_recovery = true;
    state->identity = (struct app_gateway_eack_retry_identity) {
        .gateway_id = decoded.gateway_id,
        .command_seq = decoded.command_seq,
        .collection_epoch_id = decoded.collection_epoch_id,
        .eack_round = decoded.retry_round,
        .eack_sequence = decoded.packet_sequence,
    };
    state->active = true;
    gateway_collection_recovery_eack_active = true;
    return 0;
}

static struct app_mesh_rf_retry_key
gateway_collection_recovery_retry_key(void)
{
    uint32_t recovery_attempt_id = 0u;
    const struct gateway_collection_eack_custody_snapshot *snapshot =
        &gateway_collection_eack_retry_state.snapshot;

    (void)gateway_collection_eack_recovery_attempt_id(
        snapshot->payload,
        snapshot->payload_len,
        &recovery_attempt_id);
    return (struct app_mesh_rf_retry_key) {
        .source_id = snapshot->packet.src_id,
        .destination_id = MESH_BROADCAST_ID,
        .session_id = snapshot->packet.session_id,
        .sequence = recovery_attempt_id,
        .message_type = MSG_GATEWAY_COLLECTION_EACK,
        .operation = APP_MESH_RF_RETRY_OPERATION_COLLECTION_EACK,
    };
}

static void gateway_complete_collection_recovery_eack(void)
{
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_recovery_eack_active = false;
    if (gateway_clear_eack_custody_or_defer(
            "collection-recovery-eack-custody-clear") < 0) {
        return;
    }
    if (!gateway_collection_clear_pending &&
        gateway_collection_tracking_active() &&
        gateway_collection_state.eack_pending) {
        gateway_schedule_collection_eack_round();
    }
}

static void gateway_schedule_collection_recovery_eack_retry(
    const char *reason,
    int failure)
{
    struct app_mesh_rf_retry_key key =
        gateway_collection_recovery_retry_key();
    uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
        &gateway_collection_eack_retry_state.rf_retry,
        &key,
        APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
        sys_rand32_get() ^ key.sequence);
    int schedule_ret;

    if (delay_ms == 0u) {
        LOG_ERR("gateway collection recovery EACK retry state invalid: command_seq=%u attempt=%u ret=%d reason=%s",
                key.session_id,
                key.sequence,
                failure,
                reason == NULL ? "" : reason);
        app_watchdog_stop_feeding();
        return;
    }
    schedule_ret = mesh_route_work_reschedule(
        &gateway_collection_eack_work, delay_ms);
    if (schedule_ret < 0) {
        atomic_set(&gateway_collection_eack_deferred_delay_ms,
                   (atomic_val_t)delay_ms);
        atomic_set(&gateway_collection_eack_schedule_deferred, 1);
        if (schedule_ret != -ESHUTDOWN) {
            LOG_ERR("gateway collection recovery EACK lost its route owner: ret=%d",
                    schedule_ret);
            app_watchdog_stop_feeding();
        }
    } else {
        atomic_clear(&gateway_collection_eack_schedule_deferred);
    }
    LOG_WRN("gateway collection recovery EACK retry scheduled: command_seq=%u attempt=%u delay_ms=%u ret=%d schedule_ret=%d reason=%s",
            key.session_id,
            key.sequence,
            delay_ms,
            failure,
            schedule_ret,
            reason == NULL ? "" : reason);
}

static int gateway_send_collection_recovery_eack(
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan)
{
    struct mesh_outbound eack = {0};
    struct gateway_eack_send_context send_context = {
        .c5_rf_started = false,
    };
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = gateway_eack_plan_channel9,
        .prepare_channel9 = gateway_eack_prepare_channel9,
        .send_channel9 = gateway_eack_send_channel9,
        .send_c5_flood = gateway_eack_send_c5_flood,
        .note_tx_sent = gateway_eack_note_tx_sent,
        .note_channel9_tx = gateway_eack_note_channel9_tx,
        .now_ms = gateway_eack_now_ms,
        .ctx = &send_context,
    };
    struct app_gateway_eack_policy_result result = {0};
    uint64_t current_return_hop;
    uint64_t return_hops[1];
    size_t return_hop_count = 0u;
    int ret;

    if (gateway_eack_custody_clear_pending) {
        return -EAGAIN;
    }
    if (!gateway_collection_recovery_eack_active ||
        !gateway_collection_eack_retry_state.active ||
        gateway_collection_eack_custody_validate(
            &gateway_collection_eack_retry_state.snapshot) != PROTO_OK) {
        return -ENOENT;
    }
    eack.packet = gateway_collection_eack_retry_state.snapshot.packet;
    memcpy(eack.payload,
           gateway_collection_eack_retry_state.snapshot.payload,
           gateway_collection_eack_retry_state.snapshot.payload_len);
    eack.payload_len =
        gateway_collection_eack_retry_state.snapshot.payload_len;
    eack.next_hop_id = MESH_BROADCAST_ID;
    eack.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    current_return_hop =
        app_gateway_collection_eack_current_channel9_return_hop(
            previous_hop_id,
            received_radio_channel,
            current_channel9_plan,
            DEVICE_ID);
    if (previous_hop_id != 0u &&
        previous_hop_id != MESH_BROADCAST_ID &&
        previous_hop_id != DEVICE_ID) {
        return_hops[0] = previous_hop_id;
        return_hop_count = 1u;
    }
    ret = app_gateway_eack_send_to_candidates_with_current_channel9(
        &eack,
        current_return_hop,
        current_channel9_plan,
        return_hops,
        return_hop_count,
        &ops,
        &result);
    if (ret < 0) {
        LOG_WRN("gateway collection recovery EACK send failed: ret=%d mode=%u c9_attempts=%u c5=%d",
                ret,
                (unsigned int)result.mode,
                (unsigned int)result.channel9_attempt_count,
                result.c5_send_ret);
    }
    return ret;
}

int gateway_accept_collection_receipt_redrive(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan)
{
    struct app_gateway_collection_recovery_eack_input input = {0};
    struct mesh_outbound eack = {0};
    struct gateway_collection_eack decoded = {0};
    uint32_t recovery_attempt_id;
    int ret;

    if (gateway_eack_custody_clear_pending ||
        gateway_collection_restore_pending ||
        gateway_collection_clear_pending) {
        return -EAGAIN;
    }
    if (received_radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EBADMSG;
    }
    ret = gateway_collection_recovery_input_from_result(
        packet, payload, payload_len, &input);
    if (ret < 0) {
        return ret;
    }
    if (input.gateway_id != DEVICE_ID) {
        return -ESTALE;
    }
    if (gateway_collection_eack_retry_state.active) {
        return gateway_collection_recovery_snapshot_matches(&input) ?
               0 : -EAGAIN;
    }

    recovery_attempt_id = gateway_next_broadcast_command_seq();
    if (recovery_attempt_id == 0u) {
        return -EAGAIN;
    }
    input.recovery_attempt_id = recovery_attempt_id;
    ret = app_gateway_collection_recovery_eack_prepare(&eack, &input);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_collection_eack_custody_capture(
        &gateway_collection_eack_retry_state.snapshot,
        &eack.packet,
        eack.payload,
        eack.payload_len);
    if (ret != PROTO_OK ||
        gateway_collection_eack_packet_validate(
            &eack.packet,
            eack.payload,
            eack.payload_len,
            &decoded) != PROTO_OK) {
        app_gateway_eack_retry_reset(
            &gateway_collection_eack_retry_state);
        return -EBADMSG;
    }
    gateway_collection_eack_retry_state.identity =
        (struct app_gateway_eack_retry_identity) {
            .gateway_id = decoded.gateway_id,
            .command_seq = decoded.command_seq,
            .collection_epoch_id = decoded.collection_epoch_id,
            .eack_round = decoded.retry_round,
            .eack_sequence = decoded.packet_sequence,
        };
    gateway_collection_eack_retry_state.active = true;
    gateway_collection_recovery_eack_active = true;

    ret = app_mesh_persistence_save_gateway_eack_custody(
        &gateway_collection_eack_retry_state.snapshot);
    if (ret < 0) {
        app_gateway_eack_retry_reset(
            &gateway_collection_eack_retry_state);
        gateway_collection_recovery_eack_active = false;
        return ret;
    }
    ret = gateway_send_collection_recovery_eack(
        previous_hop_id,
        received_radio_channel,
        current_channel9_plan);
    if (ret < 0) {
        gateway_schedule_collection_recovery_eack_retry(
            "collection-receipt-redrive", ret);
        /*
         * Exact frozen RF custody is durable. The original sender keeps its
         * collection outbox despite the generic transport ACK, and the route
         * owner will continue the CLOSED EACK until a full attempt completes.
         */
        return 0;
    }
    gateway_complete_collection_recovery_eack();
    return 0;
}

static int gateway_send_collection_eack(const char *reason,
                                        uint64_t previous_hop_id,
                                        uint8_t received_radio_channel,
                                        const struct mesh_event_plan *current_channel9_plan,
                                        uint64_t *failed_next_hop_id,
                                        bool *used_frozen_snapshot)
{
    struct mesh_outbound eack = {0};
    struct gateway_eack_send_context send_context = {
        .c5_rf_started = false,
    };
    struct app_gateway_collection_eack_input input = {
        .collection = &gateway_collection_state,
        .expected_node_ids = gateway_collection_state.expected_node_ids,
        .expected_node_id_count =
            gateway_collection_state.expected_node_id_count,
        .previous_hop_id = previous_hop_id,
        .received_radio_channel = received_radio_channel,
        .current_channel9_plan = current_channel9_plan,
        .self_id = DEVICE_ID,
        .excluded_channel9_next_hop_ids =
            gateway_collection_eack_retry_state.failed_channel9_next_hop_ids,
        .excluded_channel9_next_hop_count =
            gateway_collection_eack_retry_state.failed_channel9_next_hop_count,
        .force_c5_recovery =
            gateway_collection_eack_retry_state.force_c5_recovery,
    };
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = gateway_eack_plan_channel9,
        .prepare_channel9 = gateway_eack_prepare_channel9,
        .send_channel9 = gateway_eack_send_channel9,
        .send_c5_flood = gateway_eack_send_c5_flood,
        .note_tx_sent = gateway_eack_note_tx_sent,
        .note_channel9_tx = gateway_eack_note_channel9_tx,
        .now_ms = gateway_eack_now_ms,
        .ctx = &send_context,
    };
    struct app_gateway_collection_eack_result result;
    int ret;

    if (failed_next_hop_id != NULL) {
        *failed_next_hop_id = 0u;
    }
    if (used_frozen_snapshot != NULL) {
        *used_frozen_snapshot = false;
    }
    if (!gateway_collection_tracking_active()) {
        return -ENOENT;
    }
    if (gateway_eack_custody_clear_pending) {
        return -EAGAIN;
    }
    if (!gateway_collection_state.eack_pending) {
        return -EALREADY;
    }
    if (gateway_collection_persistence_dirty) {
        /*
         * An EACK is a durable acceptance statement.  Never put it on RF
         * while the collection update it describes is still RAM-only.
         */
        return -EAGAIN;
    }

    ret = app_gateway_eack_retry_restore(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        &eack);
    if (ret == PROTO_OK) {
        input.use_prebuilt_eack = true;
        if (used_frozen_snapshot != NULL) {
            *used_frozen_snapshot = true;
        }
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        LOG_ERR("gateway collection EACK snapshot restore failed: ret=%d command_seq=%u collection=%u round=%u",
                ret,
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_state.retry_round);
        return -EBADMSG;
    }

    if (!input.use_prebuilt_eack) {
        ret = app_gateway_collection_eack_prepare(&eack, &input, &result);
        if (ret < 0) {
            LOG_WRN("gateway collection EACK build failed before custody: ret=%d command_seq=%u collection=%u round=%u",
                    ret,
                    gateway_collection_state.command_seq,
                    gateway_collection_state.collection_epoch_id,
                    gateway_collection_state.retry_round);
            return ret;
        }
        ret = app_gateway_eack_retry_freeze(
            &gateway_collection_eack_retry_state,
            &gateway_collection_state,
            &eack);
        if (ret != PROTO_OK) {
            LOG_ERR("gateway collection EACK snapshot freeze failed: ret=%d command_seq=%u collection=%u round=%u",
                    ret,
                    gateway_collection_state.command_seq,
                    gateway_collection_state.collection_epoch_id,
                    gateway_collection_state.retry_round);
            return -EBADMSG;
        }
        input.use_prebuilt_eack = true;
    }

    /*
     * Store the exact datagram before every RF attempt.  Repeating an
     * identical NVS write is harmless, and it closes the reset window after
     * an earlier persistence failure without rebuilding mutable payload.
     */
    ret = app_mesh_persistence_save_gateway_eack_custody(
        &gateway_collection_eack_retry_state.snapshot);
    if (ret < 0) {
        LOG_WRN("gateway collection EACK custody save failed before RF: ret=%d command_seq=%u collection=%u round=%u",
                ret,
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_state.retry_round);
        return ret;
    }

    ret = app_gateway_collection_eack_send(
        &eack,
        &input,
        &ops,
        &result);
    if (ret < 0) {
        if (failed_next_hop_id != NULL &&
            result.policy.channel9_attempt_count != 0u) {
            *failed_next_hop_id = result.policy.channel9_next_hop_id;
        }
        LOG_WRN("gateway collection EACK build/send failed: ret=%d candidates=%u mode=%u c9_candidates=%u c9_attempts=%u c9_plan=%d c9_prepare=%d c9_send=%d c5_send=%d",
                ret,
                (unsigned int)result.return_target_count,
                (unsigned int)result.policy.mode,
                (unsigned int)result.policy.channel9_candidate_count,
                (unsigned int)result.policy.channel9_attempt_count,
                result.policy.channel9_plan_ret,
                result.policy.channel9_prepare_ret,
                result.policy.channel9_send_ret,
                result.policy.c5_send_ret);
        return ret;
    }

    LOG_INF("gateway collection EACK sent: command_seq=%u received=%u expected=%u missing=%u open=%u candidates=%u mode=%u c9_next=0x%016llx reason=%s",
            gateway_collection_state.command_seq,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            result.missing_count,
            gateway_collection_state.collection_open ? 1u : 0u,
            (unsigned int)result.return_target_count,
            (unsigned int)result.policy.mode,
            (unsigned long long)result.policy.channel9_next_hop_id,
            reason == NULL ? "" : reason);
    return 0;
}

static void gateway_complete_collection_eack_round(
    const char *reason,
    bool used_frozen_snapshot)
{
    bool send_updated_round =
        used_frozen_snapshot && gateway_collection_eack_round_dirty;
    uint32_t previous_retry_spread_ms =
        gateway_collection_state.next_retry_spread_ms;
    uint16_t previous_eack_sequence = gateway_collection_state.eack_sequence;
    uint8_t previous_retry_round = gateway_collection_state.retry_round;
    bool previous_eack_pending = gateway_collection_state.eack_pending;
    int ret;

    if (!used_frozen_snapshot) {
        gateway_collection_eack_round_dirty = false;
    }

    ret = gateway_collection_advance_retry_round(&gateway_collection_state);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection retry round advance failed: ret=%d", ret);
        gateway_schedule_collection_eack_retry(reason, -EBADMSG, 0u);
        return;
    }
    gateway_collection_state.eack_pending =
        gateway_collection_state.collection_open || send_updated_round;
    ret = gateway_persist_collection_state(reason);
    if (ret < 0) {
        /*
         * The previously persisted collection identity still owns the
         * frozen EACK.  Roll RAM back to that identity and retry the exact
         * datagram instead of exposing a half-committed next round.
         */
        gateway_collection_state.retry_round = previous_retry_round;
        gateway_collection_state.eack_sequence = previous_eack_sequence;
        gateway_collection_state.next_retry_spread_ms =
            previous_retry_spread_ms;
        gateway_collection_state.eack_pending = previous_eack_pending;
        gateway_collection_persistence_dirty = false;
        gateway_schedule_collection_eack_retry(reason, ret, 0u);
        return;
    }

    ret = app_gateway_eack_retry_commit_success(
        &gateway_collection_eack_retry_state);
    if (ret != PROTO_OK) {
        LOG_ERR("gateway collection EACK RAM custody commit failed after durable round advance: ret=%d",
                ret);
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    }
    if (gateway_clear_eack_custody_or_defer(
            "collection-eack-custody-clear") < 0) {
        return;
    }

    if (send_updated_round) {
        gateway_collection_eack_round_dirty = false;
        ret = mesh_route_work_reschedule(&gateway_collection_eack_work, 0u);
        if (ret < 0) {
            atomic_clear(&gateway_collection_eack_deferred_delay_ms);
            atomic_set(&gateway_collection_eack_schedule_deferred, 1);
            if (ret != -ESHUTDOWN) {
                LOG_ERR("gateway updated EACK round lost its route owner: ret=%d",
                        ret);
                app_watchdog_stop_feeding();
            }
        } else {
            atomic_clear(&gateway_collection_eack_schedule_deferred);
        }
        if (ret < 0) {
            LOG_WRN("gateway updated EACK round scheduling deferred: ret=%d",
                    ret);
        }
        return;
    }

    gateway_collection_eack_round_dirty = false;
    gateway_schedule_collection_eack_round();
}

static void gateway_collection_eack_work_handler(struct k_work *work)
{
    uint64_t failed_next_hop_id = 0u;
    bool used_frozen_snapshot = false;
    int ret;

    ARG_UNUSED(work);
    atomic_clear(&gateway_collection_eack_schedule_deferred);
    atomic_clear(&gateway_collection_eack_deferred_delay_ms);

    if (gateway_eack_custody_clear_pending) {
        gateway_schedule_persistence_retry(
            "eack-custody-clear-before-work");
        return;
    }
    if (gateway_collection_recovery_eack_active) {
        ret = gateway_send_collection_recovery_eack(
            0u, 0u, NULL);
        if (ret < 0) {
            gateway_schedule_collection_recovery_eack_retry(
                "collection-receipt-recovery-round", ret);
        } else {
            gateway_complete_collection_recovery_eack();
        }
        return;
    }
    if (!gateway_collection_tracking_active()) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        (void)gateway_clear_eack_custody_or_defer(
            "inactive-collection-eack-custody-clear");
        return;
    }
    if (!gateway_collection_state.eack_pending) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        (void)gateway_clear_eack_custody_or_defer(
            "settled-collection-eack-custody-clear");
        return;
    }

    ret = gateway_send_collection_eack("collection-eack-round",
                                       0u,
                                       0u,
                                       NULL,
                                       &failed_next_hop_id,
                                       &used_frozen_snapshot);
    if (ret < 0) {
        gateway_schedule_collection_eack_retry("collection-eack-round",
                                               ret,
                                               failed_next_hop_id);
        return;
    }
    gateway_complete_collection_eack_round("collection-eack-round",
                                           used_frozen_snapshot);
}

static int gateway_accept_collection_record(
    const char *reason,
    bool collection_changed)
{
    bool reopened = !gateway_collection_state.eack_pending;
    int ret;

    if (reopened) {
        gateway_collection_state.eack_pending = true;
    }
    if (collection_changed || reopened || gateway_collection_persistence_dirty) {
        ret = gateway_persist_collection_state(reason);
        if (ret < 0) {
            if (collection_changed) {
                int rollback_ret =
                    app_mesh_persistence_rollback_gateway_collection(
                        &gateway_collection_state);

                gateway_collection_persistence_dirty = false;
                if (rollback_ret < 0) {
                    LOG_ERR("gateway collection result rollback failed: ret=%d persist_ret=%d",
                            rollback_ret,
                            ret);
                }
            } else if (reopened) {
                gateway_collection_state.eack_pending = false;
                gateway_collection_persistence_dirty = false;
            }
            return ret;
        }
    }

    if (gateway_collection_eack_retry_state.active) {
        if (collection_changed) {
            gateway_collection_eack_round_dirty = true;
        }
        LOG_DBG("gateway collection EACK retry already pending after record: command_seq=%u collection=%u rf_retry_round=%u",
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_eack_retry_state.rf_retry.retry_round);
    }

    return 0;
}

static int gateway_drive_collection_eack_after_record(
    const char *reason,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan)
{
    bool used_frozen_snapshot = false;
    uint64_t failed_next_hop_id = 0u;
    int ret;

    if (!gateway_collection_tracking_active() ||
        !gateway_collection_state.eack_pending ||
        gateway_eack_custody_clear_pending) {
        return -ESTALE;
    }
    if (gateway_collection_eack_retry_state.active) {
        return 0;
    }

    ret = gateway_send_collection_eack(reason,
                                       previous_hop_id,
                                       received_radio_channel,
                                       current_channel9_plan,
                                       &failed_next_hop_id,
                                       &used_frozen_snapshot);
    if (ret < 0) {
        gateway_schedule_collection_eack_retry(reason,
                                               ret,
                                               failed_next_hop_id);
    } else {
        gateway_complete_collection_eack_round(reason,
                                               used_frozen_snapshot);
    }

    /* The result itself is safely accepted once its collection is durable. */
    return 0;
}

static int gateway_note_collection_result(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    bool duplicate = false;
    int accept_ret;
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT) {
        return -ENOENT;
    }

    ret = gateway_collection_record_result_from_hop(&gateway_collection_state,
                                                    packet,
                                                    payload,
                                                    payload_len,
                                                    previous_hop_id,
                                                    &duplicate);
    if (ret != PROTO_OK) {
        LOG_DBG("gateway collection result ignored: src=0x%016llx ret=%d",
                packet == NULL ? 0ull : (unsigned long long)packet->src_id,
                ret);
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection result recorded: src=0x%016llx command_seq=%u received=%u expected=%u duplicate=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            duplicate ? 1u : 0u);
    accept_ret = gateway_accept_collection_record(
        duplicate ? "collection-eack-result-duplicate" :
                    "collection-eack-result",
        !duplicate);
    if (accept_ret < 0) {
        return accept_ret;
    }
    return duplicate ? APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE :
                       APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static int gateway_note_collection_bundle(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;
    int accept_ret;
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending ||
        packet == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE) {
        return -ENOENT;
    }

    ret = gateway_collection_record_bundle_from_hop(&gateway_collection_state,
                                                    packet,
                                                    payload,
                                                    payload_len,
                                                    previous_hop_id,
                                                    &accepted_count,
                                                    &duplicate_count);
    if (ret != PROTO_OK) {
        LOG_DBG("gateway collection bundle ignored: src=0x%016llx ret=%d",
                packet == NULL ? 0ull : (unsigned long long)packet->src_id,
                ret);
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection bundle recorded: src=0x%016llx command_seq=%u accepted=%u duplicate=%u received=%u expected=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            (unsigned int)accepted_count,
            (unsigned int)duplicate_count,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count);
    if (accepted_count == 0u && duplicate_count == 0u) {
        return -EBADMSG;
    }
    accept_ret = gateway_accept_collection_record(
        accepted_count == 0u ? "collection-eack-bundle-duplicate" :
                               "collection-eack-bundle",
        accepted_count != 0u);
    if (accept_ret < 0) {
        return accept_ret;
    }
    return accepted_count == 0u ?
           APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE :
           APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}
