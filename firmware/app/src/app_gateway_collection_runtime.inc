#line 1209 __BASE_FILE__
static int gateway_persist_collection_state(const char *reason)
{
    ARG_UNUSED(reason);

    if (!gateway_collection_tracking_active() ||
        gateway_membership_restore_pending ||
        gateway_collection_restore_pending ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending) {
        return -ENOENT;
    }

    /* RAM-only build: the in-memory collection is always already authoritative,
     * so the save is a no-op that merely records the accepted dirty round. */
    gateway_collection_persistence_dirty = false;
    gateway_persistence_retry_round = 0u;
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
    return gateway_eack_custody_clear_pending ||
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
    gateway_collection_persistence_dirty = false;
    gateway_collection_eack_round_dirty = false;
    gateway_persistence_retry_round = 0u;
    /* RAM-only build: there is no durable snapshot to fall back to, so a
     * failed start reverts the in-memory collection to its idle state. */
    gateway_collection_clear(&gateway_collection_state);
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
                /* RAM-only build: no durable rollback exists, just drop the
                 * uncommitted in-memory update. */
                gateway_collection_persistence_dirty = false;
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
