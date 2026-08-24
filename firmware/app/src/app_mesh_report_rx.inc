#line 14825 __BASE_FILE__
static void mesh_rx_handler_lock_note_owner(const char *owner)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_rx_handler_lock_owner = owner == NULL ? "rx" : owner;
    mesh_rx_handler_lock_since_ms = k_uptime_get_32();
}

static void mesh_rx_handler_lock_clear_owner(void)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_rx_handler_lock_owner = NULL;
    mesh_rx_handler_lock_since_ms = 0u;
}

/* Keep the scan-handoff state and the physical lease in the same transaction. */
static int mesh_rx_radio_claim(const char *owner,
                               struct radio_guard_uwb_lease *lease_out)
{
    k_spinlock_key_t key;
    bool accepted;
    int ret;

    if (lease_out == NULL || !mesh_rx_handoff_scan_rearm_allowed()) {
        return -ECANCELED;
    }
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                     owner,
                                     lease_out);
    if (ret < 0) {
        return ret;
    }

    key = k_spin_lock(&mesh_rx_handoff_lock);
    accepted = app_mesh_rx_handoff_try_begin_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    if (accepted) {
        return 0;
    }

    ret = mesh_transport_radio_finish(lease_out, 0);
    return ret < 0 ? ret : -ECANCELED;
}

static int mesh_rx_radio_finish(struct radio_guard_uwb_lease *lease,
                                int parking_ret)
{
    k_spinlock_key_t key;
    int ret;

    ret = mesh_transport_radio_finish(lease, parking_ret);
    if (ret < 0) {
        return ret;
    }
    key = k_spin_lock(&mesh_rx_handoff_lock);
    app_mesh_rx_handoff_end_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return 0;
}

static void mesh_rx_work_handler(struct k_work *work)
{
    uint32_t handled_count;
    int lock_ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_WAIT q=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    lock_ret = k_mutex_lock(&mesh_rx_handler_lock, K_NO_WAIT);
    if (lock_ret != 0) {
        /* The direct lock owner drains the queue and schedules any remainder.
         * Requeueing this worker here can monopolize its workqueue while that
         * owner is waiting for a different work item to make progress.
         */
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_WORK_LOCK_BUSY ret=%d q=%u owner=%s since=%u\n",
                                lock_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                mesh_rx_handler_lock_owner == NULL ?
                                "unknown" : mesh_rx_handler_lock_owner,
                                mesh_rx_handler_lock_since_ms);
        }
        return;
    }
    atomic_set(&mesh_rx_handler_active_state, 1);
    mesh_rx_handler_lock_note_owner("work");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_ENTER q=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    handled_count = mesh_drain_rx_queue_locked("work");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_EXIT handled=%u q=%u\n",
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    atomic_set(&mesh_rx_handler_active_state, 0);
    mesh_rx_handler_lock_clear_owner();
    k_mutex_unlock(&mesh_rx_handler_lock);
}

static bool mesh_process_queued_rx_now_internal(const char *reason,
                                                bool gateway_rx_boundary)
{
    uint32_t pending_count = k_msgq_num_used_get(&mesh_rx_msgq);
    uint32_t handled_count;
    struct fw_radio_activity_decision coordinator_decision;
    int lock_ret;
    int submit_ret;

    if (pending_count == 0u) {
        return false;
    }
    if (gateway_rx_boundary &&
        (DEVICE_ROLE != ROLE_GATEWAY || radio_guard_uwb_busy())) {
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && radio_guard_uwb_busy()) {
        submit_ret = mesh_submit_owned_work(&mesh_rx_work,
                                            "rx-radio-busy");
        status_debug_printf("DBG_MESH_RX_DIRECT_RF_BUSY submit=%d q=%u reason=%s\n",
                            submit_ret,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
        return false;
    }
    mesh_coordinator_decide_now(reason == NULL ? "queued-rx" : reason,
                                &coordinator_decision);
    if (!coordinator_decision.mesh_work_allowed &&
        !(gateway_rx_boundary &&
          coordinator_decision.state == FW_RADIO_ACTIVITY_GATEWAY_RX)) {
        submit_ret = mesh_submit_owned_work(&mesh_rx_work,
                                            "rx-coordinator-defer");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_DIRECT_COORD_DEFER submit=%d q=%u state=%s reason=%s\n",
                                submit_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                fw_radio_activity_state_name(
                                    coordinator_decision.state),
                                reason == NULL ? "direct" : reason);
        }
        return false;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_WAIT q=%u reason=%s\n",
                            pending_count,
                            reason == NULL ? "direct" : reason);
    }
    lock_ret = k_mutex_lock(&mesh_rx_handler_lock, K_NO_WAIT);
    if (lock_ret != 0) {
        submit_ret = mesh_submit_owned_work(&mesh_rx_work,
                                            "rx-direct-lock-retry");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_DIRECT_BUSY lock=%d submit=%d q=%u owner=%s since=%u reason=%s\n",
                                lock_ret,
                                submit_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                mesh_rx_handler_lock_owner == NULL ?
                                "unknown" : mesh_rx_handler_lock_owner,
                                mesh_rx_handler_lock_since_ms,
                                reason == NULL ? "direct" : reason);
        }
        return false;
    }

    atomic_set(&mesh_rx_handler_active_state, 1);
    mesh_rx_handler_lock_note_owner(reason == NULL ? "direct" : reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_ENTER q=%u reason=%s\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
    }
    handled_count = mesh_drain_rx_queue_locked(reason == NULL ? "direct" : reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_EXIT handled=%u q=%u reason=%s\n",
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
    }
    atomic_set(&mesh_rx_handler_active_state, 0);
    mesh_rx_handler_lock_clear_owner();
    k_mutex_unlock(&mesh_rx_handler_lock);

    if (k_msgq_num_used_get(&mesh_rx_msgq) > 0u) {
        (void)mesh_submit_owned_work(&mesh_rx_work,
                                     "rx-drain-remainder");
    }
    return handled_count > 0u;
}

bool mesh_process_queued_rx_now(const char *reason)
{
    return mesh_process_queued_rx_now_internal(reason, false);
}

static bool mesh_process_queued_rx_at_gateway_rx_boundary(void)
{
    return mesh_process_queued_rx_now_internal(
        "gateway-ch9-continuous-rx", true);
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_handle_ch9_gateway_ack_timeout_route_failure(
    const struct mesh_outbound *failed,
    uint64_t timed_out_next_hop_id,
    uint8_t gateway_unacked,
    uint8_t requeued,
    uint32_t now_ms)
{
    const struct route_candidate *selected;
    enum route_delivery_action action;
    enum app_mesh_ch9_timeout_pressure_action pressure_action;
    uint64_t next_hop_id = 0u;
    uint32_t random_value = sys_rand32_get();
    uint32_t delay_ms = 0u;
    uint8_t failure_count = 0u;
    int select_ret = PROTO_ERR_NOT_FOUND;
    int route_ret = 0;

    if (failed == NULL || gateway_unacked == 0u) {
        return;
    }

    if (failed->packet.msg_type == MSG_COMMAND_RESULT) {
        selected = route_selected(&mesh_runtime.upstream);
        failure_count = selected == NULL ? 1u : selected->failure_count;
        delay_ms = mesh_identity_backoff_ms(DEVICE_ID, timed_out_next_hop_id, failure_count);
        report_tx_schedule_backoff(delay_ms, "ch9-operation-route-timeout");
        return;
    }

    action = route_record_failure_at(&mesh_runtime.upstream,
                                     ROUTE_FAILURE_GATEWAY_ACK,
                                     now_ms);
    pressure_action = app_mesh_ch9_timeout_pressure_decide(
        failed,
        DEVICE_ROLE == ROLE_ANCHOR,
        mesh_runtime_has_transit_reservation(),
        false,
        DEVICE_ID);
    if (action != ROUTE_DELIVERY_DISCOVER) {
        select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                failed->packet.dst_id,
                                                &next_hop_id);
    }
    if (pressure_action == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
        if (action != ROUTE_DELIVERY_RETRY_CURRENT) {
            mesh_relay_clear_channel9_timing(&mesh_runtime,
                                             timed_out_next_hop_id);
            mesh_event_owner_abandon_peer(timed_out_next_hop_id);
        }
        if (action == ROUTE_DELIVERY_DISCOVER || select_ret != PROTO_OK) {
            mesh_relay_invalidate_upstream_route(&mesh_runtime);
        }
    } else if (action == ROUTE_DELIVERY_RETRY_CURRENT &&
               select_ret == PROTO_OK) {
        selected = route_selected(&mesh_runtime.upstream);
        failure_count = selected == NULL ? 1u : selected->failure_count;
        delay_ms = mesh_relay_retry_backoff_ms(failure_count, random_value);
        report_tx_schedule_backoff(delay_ms, "ch9-gateway-ack-timeout");
    } else if (action == ROUTE_DELIVERY_TRY_ALTERNATE && select_ret == PROTO_OK) {
        report_tx_schedule(0u);
    } else if (pressure_action == APP_MESH_CH9_TIMEOUT_DEFER_LOCAL) {
        delay_ms = ROUTE_PARENT_HOLDDOWN_MS;
        route_ret = -EAGAIN;
        report_tx_schedule_backoff(delay_ms,
                                   "ch9-gateway-ack-downstream-reserved");
    } else {
        mesh_relay_invalidate_upstream_route(&mesh_runtime);
        if (timed_out_next_hop_id == GATEWAY_ID) {
            mesh_direct_gateway_bulk_probe_suppress(now_ms,
                                                    "ch9-gateway-ack-timeout");
        }
        route_ret = mesh_schedule_route_request(
            failed->packet.dst_id,
            "ch9-gateway-ack-timeout");
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_ROUTE_FAIL action=%s pressure=%u src=0x%llx type=0x%02x gw_unacked=%u requeued=%u fail=%u delay=%u route_ret=%d select=%d next=0x%llx timed=0x%llx\n",
                            route_delivery_action_name(action),
                            pressure_action,
                            (unsigned long long)failed->packet.src_id,
                            failed->packet.msg_type,
                            gateway_unacked,
                            requeued,
                            failure_count,
                            delay_ms,
                            route_ret,
                            select_ret,
                            (unsigned long long)next_hop_id,
                            (unsigned long long)timed_out_next_hop_id);
    }
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_ch9_tx_pending_handle_timeout(uint32_t now_ms)
{
    struct mesh_outbound route_failure_packet = {0};
    uint64_t timed_out_next_hop_id = 0u;
    uint8_t requeued = 0u;
    uint8_t acked = 0u;
    uint8_t gateway_unacked = 0u;
    uint8_t transit_dropped = 0u;
    bool route_failure_packet_valid = false;

    if (!mesh_ch9_tx_pending.active ||
        !uptime_deadline_reached(now_ms, mesh_ch9_tx_pending.deadline_ms)) {
        return;
    }

    timed_out_next_hop_id = mesh_ch9_tx_pending.next_hop_id;
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        struct mesh_ch9_tx_pending_entry *entry = &mesh_ch9_tx_pending.entries[i];

        if (entry->acked) {
            acked++;
            continue;
        }
        struct mesh_outbound clean = entry->outbound;
        int strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
        const struct mesh_outbound *retry =
            strip_ret == PROTO_OK ? &clean : &entry->outbound;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && strip_ret != PROTO_OK) {
            status_debug_printf("DBG_CH9_TX_ACK_TIMEOUT_STRIP_FAIL ret=%d seq=%u\n",
                                strip_ret,
                                entry->outbound.packet.seq);
        }
        if (app_mesh_ch9_tx_timeout_counts_route_failure(retry,
                                                         timed_out_next_hop_id,
                                                         GATEWAY_ID)) {
            gateway_unacked++;
            if (!route_failure_packet_valid) {
                route_failure_packet = *retry;
                route_failure_packet_valid = true;
            }
        }
        if (app_mesh_ch9_timeout_pressure_decide(
                retry,
                DEVICE_ROLE == ROLE_ANCHOR,
                mesh_runtime_has_transit_reservation(),
                false,
                DEVICE_ID) == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
            entry->acked = true;
            transit_dropped++;
            status_debug_printf("DBG_TRANSIT_CUSTODY_DROP src=0x%llx seq=%u type=0x%02x reason=gateway-ack-timeout\n",
                                (unsigned long long)retry->packet.src_id,
                                retry->packet.seq,
                                retry->packet.msg_type);
            continue;
        }
    }

    requeued = mesh_ch9_tx_pending_requeue_unacked(now_ms);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_TX_ACK_TIMEOUT\n");
        status_debug_printf("DBG_CH9_TX_ACK_TIMEOUT n=%u acked=%u dropped=%u requeued=%u retained=%u now=%u deadline=%u\n",
                            (uint8_t)(acked + transit_dropped + requeued +
                                      mesh_ch9_tx_pending.count),
                            acked,
                            transit_dropped,
                            requeued,
                            mesh_ch9_tx_pending.count,
                            now_ms,
                            mesh_ch9_tx_pending.deadline_ms);
    }
    LOG_WRN("mesh channel-9 TX batch ACK timeout: acked=%u dropped=%u requeued=%u retained=%u",
            acked,
            transit_dropped,
            requeued,
            mesh_ch9_tx_pending.count);
    if (route_failure_packet_valid) {
        mesh_handle_ch9_gateway_ack_timeout_route_failure(&route_failure_packet,
                                                          timed_out_next_hop_id,
                                                          gateway_unacked,
                                                          requeued,
                                                          now_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint8_t skipped;

        status_debug_note("DBG_CH9_TX_ACK_TIMEOUT_REQUEUE\n");
        skipped = mesh_advance_channel9_timing_past(timed_out_next_hop_id,
                                                    now_ms,
                                                    "ack-timeout");
        (void)mesh_expire_channel9_timings(now_ms, "ch9-ack-timeout");
        status_debug_printf("DBG_CH9_TX_ACK_REQUEUE next=0x%llx skip=%u\n",
                            (unsigned long long)timed_out_next_hop_id,
                            skipped);
    }
    if (!mesh_ch9_tx_pending.active) {
        mesh_ch9_tx_pending_clear();
    } else {
        mesh_schedule_tx_timeout();
    }
    if (requeued > 0u || report_tx_queue_used() > 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_ACK_REQUEUE_SCHED requeued=%u q=%u\n",
                                requeued,
                                report_tx_queue_used());
        }
        report_tx_schedule(0u);
    }
}
#else
static void mesh_ch9_tx_pending_handle_timeout(uint32_t now_ms)
{
    ARG_UNUSED(now_ms);
}
#endif

static bool mesh_defer_due_retry_to_channel9_tx_slot(uint32_t now_ms)
{
    uint64_t peer_id;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        mesh_runtime.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
        mesh_runtime.pending.radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !uptime_deadline_reached(now_ms, mesh_runtime.pending.retry_after_ms)) {
        return false;
    }

    peer_id = mesh_runtime.pending.next_hop_id;
    (void)mesh_advance_channel9_timing_past(peer_id,
                                            now_ms,
                                            "retry-slot-preflight");
    (void)mesh_expire_channel9_timings(now_ms, "retry-slot-preflight");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];
        uint32_t prepare_ms;
        uint32_t delay_ms;

        if (!entry->valid || entry->next_hop_id != peer_id ||
            !mesh_event_timing_usable(&entry->timing, now_ms) ||
            !app_mesh_ch9_retry_next_local_tx_prepare_ms(
                &entry->timing,
                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                &prepare_ms)) {
            continue;
        }

        delay_ms = uptime_deadline_reached(now_ms, prepare_ms) ?
                   1u :
                   uptime_ms_until_deadline(now_ms, prepare_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_RETRY_DEFER_RX_SLOT peer=0x%llx cnt=%u now=%u prepare=%u delay=%u\n",
                                (unsigned long long)peer_id,
                                entry->timing.event_counter,
                                now_ms,
                                prepare_ms,
                                delay_ms);
        }
        (void)mesh_reschedule_owned_work(&mesh_tx_timeout_work,
                                         delay_ms,
                                         "tx-timeout-channel9-slot");
        mesh_schedule_uwb_rx(0u);
        return true;
    }
    return false;
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result *result = &mesh_work_result;
    struct mesh_outbound *pending_waiting = &mesh_tx_timeout_pending_waiting;
    enum app_mesh_parent_loss_custody parent_loss_custody;
    bool pending_route_waiting = false;
    uint32_t now_ms;

    ARG_UNUSED(work);
    if (mesh_transport_paused()) {
        (void)mesh_reschedule_owned_work(&mesh_tx_timeout_work,
                                         REPORT_TX_RETRY_DELAY_MS,
                                         "paused-tx-timeout");
        return;
    }
    now_ms = k_uptime_get_32();
    memset(result, 0, sizeof(*result));
    memset(pending_waiting, 0, sizeof(*pending_waiting));

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_TX_TIMEOUT_ENTER relay=%u route=%u ch9pend=%u bundle=%u now=%u\n",
                            mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            mesh_ch9_tx_pending_is_active() ? 1u : 0u,
                            mesh_relay_result_bundle_pending(&mesh_runtime) ? 1u : 0u,
                            now_ms);
    }

    mesh_ch9_tx_pending_handle_timeout(now_ms);
    if (mesh_defer_due_retry_to_channel9_tx_slot(now_ms)) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        pending_waiting->packet = mesh_runtime.pending.packet;
        pending_waiting->payload_len = mesh_runtime.pending.payload_len;
        pending_waiting->radio_channel = mesh_runtime.pending.radio_channel;
        pending_waiting->next_hop_id = mesh_runtime.pending.next_hop_id;
        if (pending_waiting->payload_len > 0u) {
            memcpy(pending_waiting->payload,
                   mesh_runtime.pending.payload,
                   pending_waiting->payload_len);
        }
        pending_route_waiting = mesh_tx_can_wait_for_route(pending_waiting);
    }

    if (mesh_relay_tick_with_random(&mesh_runtime,
                                    now_ms,
                                    sys_rand32_get(),
                                    result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(result,
                               UWB_CHANNEL_WAKE_CONTACT,
                               NULL,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               NULL);
    if (mesh_relay_tx_active(&mesh_runtime) ||
        mesh_relay_result_bundle_pending(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }

    parent_loss_custody = app_mesh_parent_loss_custody_decide(
        (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u,
        mesh_relay_tx_active(&mesh_runtime),
        pending_route_waiting);
    if (parent_loss_custody != APP_MESH_PARENT_LOSS_CUSTODY_NONE) {
        if (parent_loss_custody == APP_MESH_PARENT_LOSS_CUSTODY_ROUTE_WAIT) {
            (void)mesh_store_route_waiting_tx_owned(
                pending_waiting, APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC);
        }
        (void)mesh_schedule_route_request(pending_waiting->packet.dst_id,
                                          "pending-tx-timeout");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_TX_TIMEOUT_ROUTE_REQ_ASYNC relay=%u route=%u custody=%u dst=0x%llx\n",
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                (unsigned int)parent_loss_custody,
                                (unsigned long long)pending_waiting->packet.dst_id);
        }
    }
}

static uint64_t mesh_expand_uptime32(uint32_t timestamp_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int32_t offset_ms = (int32_t)(timestamp_ms - (uint32_t)now_ms);

    return (uint64_t)((int64_t)now_ms + offset_ms);
}

static int mesh_gateway_protocol_validation_arm(uint32_t receive_timeout_ms,
                                                uint32_t *token)
{
    if (token == NULL) {
        return -EINVAL;
    }
    *token = 0u;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    return gateway_protocol_validation_arm(receive_timeout_ms, token);
}

static int mesh_gateway_protocol_validation_complete(
    uint32_t token,
    uint32_t *received_at_ms)
{
    int ret;

    if (received_at_ms == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_GATEWAY || token == 0u) {
        *received_at_ms = k_uptime_get_32();
        return 0;
    }
    ret = dwm3000_driver_last_rx_host_uptime(received_at_ms);
    if (ret < 0) {
        return ret;
    }
    return gateway_protocol_validation_complete(
        token, mesh_expand_uptime32(*received_at_ms));
}

static bool mesh_queue_from_frame_at_internal(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t link_quality,
    uint8_t radio_channel,
    uint32_t received_at_ms,
    uint32_t protocol_validation_token,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t current_channel9_peer_id,
    bool submit_work,
    bool *valid_mesh_frame,
    uint64_t *previous_hop_id)
{
    struct mesh_frame_parse_context context = {0};
    struct mesh_rx_pending pending = {0};
    uint64_t received_uptime_ms;
    int ret;

    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = false;
    }
    if (frame == NULL || frame_len == 0u) {
        return false;
    }
    if (uwb_mesh_frame_decode(frame,
                              frame_len,
                              NETWORK_ID,
                              DEVICE_ID,
                              &context.previous_hop_id,
                              &context.packet,
                              context.payload,
                              sizeof(context.payload),
                              &context.payload_len) != PROTO_OK) {
        if (mesh_gateway_route_test_role()) {
            status_debug_printf("DBG_GATEWAY_RX_DECODE_FAIL ch=%u len=%u q=%u\n",
                                radio_channel,
                                (unsigned int)frame_len,
                                link_quality);
        }
        return false;
    }
    if (mesh_packet_rx_envelope_validate(
            &context.packet,
            context.payload,
            context.payload_len,
            context.previous_hop_id,
            DEVICE_ID,
            GATEWAY_ID,
            radio_channel,
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) != PROTO_OK) {
        if (mesh_gateway_route_test_role()) {
            status_debug_printf(
                "DBG_GATEWAY_RX_ENVELOPE_REJECT ch=%u msg=0x%02x src=0x%llx prev=0x%llx\n",
                radio_channel,
                context.packet.msg_type,
                (unsigned long long)context.packet.src_id,
                (unsigned long long)context.previous_hop_id);
        }
        return false;
    }
    received_uptime_ms = mesh_expand_uptime32(received_at_ms);
    if (!app_mesh_rx_policy_postboot_route_adv_fresh(
            context.packet.msg_type,
            context.packet.message_age_ms,
            received_uptime_ms)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_ROUTE_ADV_PREBOOT_REJECT age=%u received_uptime=%llu seq=%u\n",
                context.packet.message_age_ms,
                (unsigned long long)received_uptime_ms,
                context.packet.seq);
        }
        return false;
    }
#if DEVICE_ROLE == ROLE_CLICKER
    if (context.packet.dst_id != DEVICE_ID ||
        (context.packet.msg_type != MSG_ROUTE_REPLY &&
         context.packet.msg_type != MSG_GATEWAY_ACK &&
         context.packet.msg_type != MSG_MESH_HOP_ACK &&
         context.packet.msg_type != MSG_RELAY_BUSY &&
         context.packet.msg_type != MSG_MESH_EVENT_PROPOSE &&
         context.packet.msg_type != MSG_MESH_EVENT_ACCEPT &&
         context.packet.msg_type != MSG_MESH_EVENT_UPDATE &&
         context.packet.msg_type != MSG_MESH_EVENT_END)) {
        /*
         * A clicker is an uplink leaf. Reject broadcasts, transit traffic,
         * commands, reports, and custody negotiation before they can consume
         * the bounded RX queue or schedule application work.
         */
        return false;
    }
#endif
    if (previous_hop_id != NULL) {
        *previous_hop_id = context.previous_hop_id;
    }
    if (radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        DEVICE_ROLE == ROLE_ANCHOR &&
        context.packet.src_id == GATEWAY_ID &&
        context.packet.msg_type == MSG_COMMAND &&
        !app_mesh_c5_gateway_control_rx_allowed(
            context.packet.msg_type,
            context.packet.ttl,
            context.payload,
            context.payload_len,
            context.packet.src_id,
            context.previous_hop_id,
            GATEWAY_ID,
            IS_ENABLED(
                CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL),
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS)
            CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS
#else
            0u
#endif
            )) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DIRECT_GATEWAY_CONTROL_IGNORED msg=0x%02x seq=%u ttl=%u prev=0x%llx\n",
                                context.packet.msg_type,
                                context.packet.seq,
                                context.packet.ttl,
                                (unsigned long long)context.previous_hop_id);
        }
        return false;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_DECODE ch=%u msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx seq=%u flags=0x%02x plen=%u q=%u\n",
                            radio_channel,
                            context.packet.msg_type,
                            (unsigned long long)context.packet.src_id,
                            (unsigned long long)context.packet.dst_id,
                            (unsigned long long)context.previous_hop_id,
                            context.packet.seq,
                            context.packet.flags,
                            (unsigned int)context.payload_len,
                            link_quality);
    }

    pending.packet = context.packet;
    if (context.payload_len > 0u) {
        memcpy(pending.payload, context.payload, context.payload_len);
    }
    pending.payload_len = (uint16_t)context.payload_len;
    pending.previous_hop_id = context.previous_hop_id;
    pending.link_quality = link_quality;
    pending.radio_channel = radio_channel;
    pending.first_received_at_ms = received_uptime_ms;
    pending.received_at_ms = received_at_ms;
    pending.received_at_valid = true;
    pending.result_validation_token = protocol_validation_token;
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
        current_channel9_plan != NULL &&
        context.previous_hop_id == current_channel9_peer_id &&
        !uptime_deadline_reached(received_at_ms, current_channel9_plan->end_ms)) {
        pending.current_channel9_plan_valid = true;
        pending.current_channel9_plan = *current_channel9_plan;
    }
    if (app_mesh_rx_policy_should_drop(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
                                       pending.packet.msg_type)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_IGNORED_TX prev=0x%llx seq=%u\n",
                                (unsigned long long)pending.previous_hop_id,
                                pending.packet.seq);
        }
        return false;
    }
    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = true;
    }
    app_mesh_test_note_wake_event(&pending.packet,
                                  pending.previous_hop_id,
                                  pending.link_quality,
                                  radio_channel);
    status_debug_uwb_rx_channel_pulse(radio_channel);

    if (DEVICE_ROLE == ROLE_GATEWAY &&
        pending.result_validation_token == 0u &&
        pending.packet.msg_type == MSG_COMMAND_RESULT) {
        ret = gateway_command_result_validation_reserve(
            &pending.packet,
            pending.first_received_at_ms,
            &pending.result_validation_token);
        if (ret < 0 && ret != -ENOENT) {
            LOG_ERR("gateway result validation lease unavailable; dropped src=0x%016llx session=%u seq=%u ret=%d",
                    (unsigned long long)pending.packet.src_id,
                    pending.packet.session_id,
                    pending.packet.seq,
                    ret);
            return false;
        }
    }

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        gateway_command_result_validation_release_reserved(
            pending.result_validation_token);
        LOG_WRN("mesh UWB RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return false;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        uwb_anchor_note_mesh_packet(&anchor_uwb_session);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_QUEUED ch=%u msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx seq=%u q=%u\n",
                            radio_channel,
                            pending.packet.msg_type,
                            (unsigned long long)pending.packet.src_id,
                            (unsigned long long)pending.packet.dst_id,
                            (unsigned long long)pending.previous_hop_id,
                            pending.packet.seq,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        pending.packet.msg_type == MSG_GATEWAY_ROUTE_ADV) {
        status_debug_printf("DBG_ROUTE_ADV_QUEUED q=%u prev=0x%llx seq=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            (unsigned long long)pending.previous_hop_id,
                            pending.packet.seq);
    }
    if (pending.packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_tx_gateway_ack_rx_pulse();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_ACK_RX\n");
        }
    }
    LOG_INF("mesh UWB RX queued: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u queue_depth=%u role=%s",
            pending.packet.msg_type,
            (unsigned long long)pending.packet.src_id,
            (unsigned long long)pending.packet.dst_id,
            (unsigned long long)pending.previous_hop_id,
            pending.link_quality,
            k_msgq_num_used_get(&mesh_rx_msgq),
            role_name());

    if (submit_work) {
        (void)mesh_submit_owned_work(&mesh_rx_work,
                                     "rx-queue-admission");
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DEFERRED q=%u msg=0x%02x\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            pending.packet.msg_type);
    }
    return true;
}

static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
                                     uint32_t protocol_validation_token,
                                     const struct mesh_event_plan *current_channel9_plan,
                                     uint64_t current_channel9_peer_id,
                                     bool *valid_mesh_frame,
                                     uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at_internal(frame,
                                             frame_len,
                                             link_quality,
                                             radio_channel,
                                             received_at_ms,
                                             protocol_validation_token,
                                             current_channel9_plan,
                                             current_channel9_peer_id,
                                             true,
                                             valid_mesh_frame,
                                             previous_hop_id);
}

bool mesh_queue_from_frame(const uint8_t *frame,
                           size_t frame_len,
                           uint8_t link_quality,
                           uint8_t radio_channel,
                           bool *valid_mesh_frame,
                           uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at(frame,
                                    frame_len,
                                    link_quality,
                                    radio_channel,
                                    k_uptime_get_32(),
                                    0u,
                                    NULL,
                                    0u,
                                    valid_mesh_frame,
                                    previous_hop_id);
}

bool mesh_queue_from_frame_deferred(const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t link_quality,
                                    uint8_t radio_channel,
                                    bool *valid_mesh_frame,
                                    uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at_internal(frame,
                                             frame_len,
                                             link_quality,
                                             radio_channel,
                                             k_uptime_get_32(),
                                             0u,
                                             NULL,
                                             0u,
                                             false,
                                             valid_mesh_frame,
                                             previous_hop_id);
}

void mesh_submit_queued_rx(void)
{
    int ret = mesh_submit_owned_work(&mesh_rx_work,
                                     "queued-rx-submit");

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_SUBMIT ret=%d q=%u routeq=1\n",
                            ret,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
}

uint32_t mesh_rx_pending_count(void)
{
    return k_msgq_num_used_get(&mesh_rx_msgq);
}

bool mesh_rx_response_active(void)
{
    return atomic_get(&mesh_rx_response_active_state) != 0 ||
           mesh_rx_handoff_control_active() ||
           (mesh_event_accept_retry.retry.active &&
            !mesh_event_accept_retry.retry.response_sent);
}

bool mesh_anchor_low_duty_scan_should_defer(uint32_t *retry_ms)
{
    uint64_t local_reliable_tx_peers[APP_NODE_COMM_MAX_DELIVERIES];
    const size_t local_reliable_tx_peer_count =
        mesh_node_comm_reliable_tx_peers(
            local_reliable_tx_peers,
            sizeof(local_reliable_tx_peers) /
                sizeof(local_reliable_tx_peers[0]));
    uint32_t now_ms = k_uptime_get_32();
    uint32_t selected_delay_ms = 0u;
    uint32_t min_gap_ms;
    int rearm_ret;
    bool found = false;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    min_gap_ms = ANCHOR_UWB_SCAN_RX_MS + MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS;
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_timing timing = {0};
        uint32_t delay_ms;

        if (!mesh_channel9_next_required_activity(
                entry,
                local_reliable_tx_peers,
                local_reliable_tx_peer_count,
                &timing) ||
            !mesh_event_timing_usable(&timing, now_ms)) {
            continue;
        }
        {
            uint32_t prepare_ms =
                mesh_channel9_prepare_start_ms(&timing);

            delay_ms = uptime_deadline_reached(now_ms, prepare_ms) ?
                       0u :
                       uptime_ms_until_deadline(now_ms, prepare_ms);
        }
        if (!found || delay_ms < selected_delay_ms) {
            selected_delay_ms = delay_ms;
            found = true;
        }
    }
    if (!found || selected_delay_ms > min_gap_ms) {
        return false;
    }

    /*
     * Deferring the low-duty scanner is safe only if the channel-9 owner is
     * actually armed.  The ACK may have been queued while this worker was
     * inactive, so merely waiting past the event can skip every TX slot.
     */
    rearm_ret = mesh_schedule_uwb_rx(selected_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_ANCHOR_CH9_REARM delay=%u ret=%d ack=%u\n",
            selected_delay_ms,
            rearm_ret,
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table) ? 1u : 0u);
    }

    if (retry_ms != NULL) {
        *retry_ms = selected_delay_ms +
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS +
                    MESH_EVENT_DEFAULT_WINDOW_MS +
                    MESH_EVENT_RX_LATE_GUARD_MS +
                    ANCHOR_UWB_SCAN_BUSY_RETRY_MS;
    }
    return true;
}

static bool mesh_decode_channel5_wake_claim(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim,
    bool *embedded_route_frame_out)
{
    bool embedded_route_frame = false;
    int ret;

    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = false;
    }
    if (frame == NULL || frame_len == 0u || claim == NULL) {
        return false;
    }

    if (frame_len == UWB_WAKE_CLAIM_LEN) {
        ret = uwb_decode_wake_claim(frame, frame_len, claim);
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
               frame_len > UWB_WAKE_CLAIM_LEN) {
        ret = uwb_decode_wake_claim(frame, UWB_WAKE_CLAIM_LEN, claim);
        embedded_route_frame = ret == PROTO_OK;
    } else {
        ret = PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return false;
    }
    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = embedded_route_frame;
    }
    return true;
}

static bool mesh_frame_requires_anchor_click_handoff(const uint8_t *frame,
                                                     size_t frame_len,
                                                     struct uwb_wake_claim_frame *claim)
{
    bool local_can_range_clicks =
        DEVICE_ROLE == ROLE_ANCHOR &&
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);

    return mesh_decode_channel5_wake_claim(frame, frame_len, claim, NULL) &&
           app_mesh_c5_wake_claim_requires_anchor_handoff(
               claim->flags, local_can_range_clicks);
}

static bool mesh_handoff_anchor_click_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t quality,
    uint32_t observed_packet_ms)
{
    bool handled = false;
    uint32_t preempt_deadline_ms;

    if (claim == NULL) {
        return false;
    }

    anchor_click_window_set_active(true);
    /* `observed_packet_ms` anchors the received countdown; pass the actual
     * discovery start and let the route-owner bridge reserve its one guard. */
    preempt_deadline_ms = observed_packet_ms + claim->discovery_starts_in_ms;
    if (mesh_preempt_for_click_event_until(preempt_deadline_ms) < 0) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click wake handoff deferred because mesh custody preemption failed");
        return false;
    }
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_click_wake_claim != NULL) {
        handled = mesh_report_callbacks->anchor_handle_click_wake_claim(
            claim,
            quality,
            observed_packet_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF evt=%u attempt=%u handled=%u q=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            handled ? 1u : 0u,
                            quality);
    }
    if (!handled) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click wake handoff failed: clicker=0x%016llx event_seq=%u attempt=%u; claim not reclassified as route traffic",
                (unsigned long long)claim->clicker_id,
                claim->click_event_id,
                claim->attempt_index);
    }
    return handled;
}

static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t link_quality,
                                            bool *embedded_route_frame_out,
                                            bool *click_priority_out)
{
    struct uwb_wake_claim_frame claim;
    uint32_t contact_expires_at_ms;
    bool embedded_route_candidate = false;
    bool embedded_route_frame = false;

    if (click_priority_out != NULL) {
        *click_priority_out = false;
    }
    if (!mesh_decode_channel5_wake_claim(frame,
                                         frame_len,
                                         &claim,
                                         &embedded_route_candidate)) {
        return false;
    }

    if (app_mesh_c5_wake_claim_preempts_mesh(claim.flags) &&
        !app_mesh_c5_wake_claim_requires_anchor_handoff(
            claim.flags,
            DEVICE_ROLE == ROLE_ANCHOR &&
            !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER))) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
            status_debug_printf("DBG_TX_CLICK_CLAIM_IGNORED src=0x%llx evt=%u attempt=%u\n",
                                (unsigned long long)claim.clicker_id,
                                claim.click_event_id,
                                claim.attempt_index);
        }
        return false;
    }

    app_mesh_test_note_wake_claim(claim.clicker_id,
                                  claim.click_event_id,
                                  claim.attempt_index,
                                  link_quality);
    if (click_priority_out != NULL) {
        *click_priority_out =
            app_mesh_c5_wake_claim_preempts_mesh(claim.flags);
    }
    mesh_gateway_route_test_note_channel5_contact(claim.clicker_id, "wake-claim");
    contact_expires_at_ms = k_uptime_get_32() + claim.claimed_duration_ms;
    if (DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        contact_expires_at_ms += MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
    if (embedded_route_candidate) {
        embedded_route_frame = mesh_queue_embedded_route_request(
            &claim,
            &frame[UWB_WAKE_CLAIM_LEN],
            frame_len - UWB_WAKE_CLAIM_LEN,
            link_quality);
    }
    mesh_c5_contact_accept(claim.clicker_id,
                           C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                           contact_expires_at_ms,
                           embedded_route_frame ? "embedded-wake-claim" :
                                                  "wake-claim");
    if (DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        const struct app_mesh_c5_route_adv_timing timing = {
            .wake_to_route_delay_ms = MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
            .request_flood_burst_ms = FLOOD_RELAY_BURST_MS,
            .embedded_reply_guard_ms = MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS,
            .route_adv_reply_guard_ms = MESH_ROUTE_TEST_ROUTE_ADV_REPLY_GUARD_MS,
        };
        uint32_t route_adv_delay_ms =
            app_mesh_c5_route_adv_response_delay_ms(
                claim.wake_train_ends_in_ms,
                embedded_route_frame,
                &timing);

        status_debug_printf("DBG_GW_ADV_WAKE_DELAY wake=%u claimed=%u delay=%u embed=%u exp=%u\n",
                            claim.wake_train_ends_in_ms,
                            claim.claimed_duration_ms,
                            route_adv_delay_ms,
                            embedded_route_frame ? 1u : 0u,
                            contact_expires_at_ms);
        (void)app_node_comm_request_route_refresh(
            route_adv_delay_ms, "route-solicit-wake-claim", false);
    }
    LOG_INF("mesh channel-5 wake claim RX: src=0x%016llx event_seq=%u attempt=%u quality=%u role=%s",
            (unsigned long long)claim.clicker_id,
            claim.click_event_id,
            claim.attempt_index,
            link_quality,
            role_name());
    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = embedded_route_frame;
    }
    return true;
}

bool mesh_anchor_handoff_route_wake_frame(const uint8_t *frame,
                                          size_t frame_len,
                                          uint8_t link_quality)
{
    struct uwb_wake_claim_frame claim;
    bool embedded_route_frame = false;
    bool captured_route_reply = false;
    bool control_followup;
    uint32_t listen_ms;
    int listen_ret = 0;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ||
        !mesh_decode_channel5_wake_claim(frame,
                                         frame_len,
                                         &claim,
                                         &embedded_route_frame) ||
        app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags, true) ||
        !app_mesh_c5_route_wake_should_listen(
            claim.clicker_id,
            GATEWAY_ID,
            claim.flags,
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ),
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL))) {
        return false;
    }

    control_followup = app_mesh_c5_wake_followup_is_control(claim.flags);
    if (!embedded_route_frame && control_followup) {
        for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
            const struct mesh_relay_event_timing_entry *entry =
                &mesh_runtime.event_timings[i];

            if (entry->valid && entry->next_hop_id == claim.clicker_id &&
                entry->direction ==
                    MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
                status_debug_printf(
                    "DBG_ANCHOR_DOWNSTREAM_CONTROL_WAKE_IGNORED src=0x%llx evt=%u\n",
                    (unsigned long long)claim.clicker_id,
                    claim.click_event_id);
                return true;
            }
        }
    }

    if (!mesh_handle_channel5_wake_claim(frame,
                                         frame_len,
                                         link_quality,
                                         &embedded_route_frame,
                                         NULL)) {
        return false;
    }

    listen_ms = claim.claimed_duration_ms;
    if (listen_ms < MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS) {
        listen_ms = MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS;
    }
    if (listen_ms > WAKE_ADV_MS + MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS) {
        listen_ms = WAKE_ADV_MS + MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS;
    }
    if (control_followup) {
        /*
         * A deeper relay starts in a later flood wave.  The sender's
         * remaining claim only covers its own payload; keep the receiver
         * through the next bounded wave so a forced-depth child can hear the
         * next relay wake and payload.  The listener still probes clicks
         * before accepting another control wake.
         */
        listen_ms = MESH_GATEWAY_CONTROL_DEEP_RELAY_LISTEN_MS;
    }

    status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_HANDOFF src=0x%llx evt=%u attempt=%u flags=0x%02x embedded=%u listen=%u\n",
                        (unsigned long long)claim.clicker_id,
                        claim.click_event_id,
                        claim.attempt_index,
                        claim.flags,
                        embedded_route_frame ? 1u : 0u,
                        listen_ms);

    if (!embedded_route_frame) {
        const char *followup_reason =
            control_followup ?
            "gateway-command-wake-followup" :
            "anchor-route-wake-followup";

        listen_ret = mesh_listen_for_route_reply(claim.clicker_id,
                                                 followup_reason,
                                                 listen_ms,
                                                 NULL,
                                                 &captured_route_reply);
    }
    if (mesh_rx_pending_count() > 0u) {
        (void)mesh_process_queued_rx_now("anchor-route-wake-followup");
    }
    status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_HANDOFF_DONE src=0x%llx embedded=%u ret=%d pending=%u\n",
                        (unsigned long long)claim.clicker_id,
                        embedded_route_frame ? 1u : 0u,
                        listen_ret,
                        mesh_rx_pending_count());
    return true;
}

static bool mesh_process_received_frame(const uint8_t *frame,
                                        size_t frame_len,
                                        uint8_t quality,
                                        bool channel9_event,
                                        uint64_t channel9_peer_id,
                                        const struct mesh_event_plan *channel9_plan,
                                        uint32_t observed_packet_ms,
                                        uint32_t protocol_validation_token,
                                        bool *channel9_peer_observed)
{
    bool valid_mesh_frame = false;
    uint64_t rx_previous_hop_id = 0u;

    if (channel9_peer_observed != NULL) {
        *channel9_peer_observed = false;
    }

    if (!channel9_event) {
        struct uwb_wake_claim_frame claim;

        if (mesh_frame_requires_anchor_click_handoff(frame,
                                                     frame_len,
                                                     &claim)) {
            (void)mesh_handoff_anchor_click_claim(&claim,
                                                  quality,
                                                  observed_packet_ms);
            gateway_command_result_validation_release_reserved(
                protocol_validation_token);
            return true;
        }
        if (mesh_handle_channel5_wake_claim(frame,
                                            frame_len,
                                            quality,
                                            NULL,
                                            NULL)) {
            LOG_DBG("mesh UWB RX accepted channel-5 wake claim: len=%u",
                    (unsigned int)frame_len);
            gateway_command_result_validation_release_reserved(
                protocol_validation_token);
            return true;
        }
    }

    if (mesh_queue_from_frame_at(frame,
                                 frame_len,
                                 quality,
                                 channel9_event ? UWB_CHANNEL_MESH_PAYLOAD :
                                                  UWB_CHANNEL_WAKE_CONTACT,
                                 observed_packet_ms,
                                 protocol_validation_token,
                                 channel9_event ? channel9_plan : NULL,
                                 channel9_peer_id,
                                 &valid_mesh_frame,
                                 &rx_previous_hop_id)) {
        if (channel9_event &&
            channel9_plan != NULL &&
            rx_previous_hop_id == channel9_peer_id) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                int32_t delta_ms = (int32_t)(observed_packet_ms - channel9_plan->start_ms);

                status_debug_printf("DBG_CH9_RX_EXPECTED delta=%d q=%u\n",
                                    delta_ms,
                                    quality);
            }
            mesh_relay_note_channel9_rx(&mesh_runtime,
                                        rx_previous_hop_id,
                                        channel9_plan->start_ms,
                                        observed_packet_ms);
            if (channel9_peer_observed != NULL) {
                *channel9_peer_observed = true;
            }
        } else if (channel9_event &&
                   channel9_plan != NULL &&
                   IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_RX_UNEXPECTED_PEER\n");
            status_debug_printf("DBG_CH9_RX_UNEXPECTED prev=0x%llx exp=0x%llx\n",
                                (unsigned long long)rx_previous_hop_id,
                                (unsigned long long)channel9_peer_id);
        }
        LOG_DBG("mesh UWB RX frame accepted: len=%u", (unsigned int)frame_len);
        return true;
    }

    if (!valid_mesh_frame) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
            status_debug_printf("DBG_RX_REJECT mode=%u len=%u q=%u\n",
                                channel9_event ? 9u : 5u,
                                (unsigned int)frame_len,
                                quality);
        }
        LOG_DBG("mesh UWB RX ignored non-mesh frame: len=%u quality=%u",
                (unsigned int)frame_len,
                quality);
    }

    gateway_command_result_validation_release_reserved(
        protocol_validation_token);
    return false;
}

static bool mesh_gateway_run_enumeration_response_slice(void)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    struct radio_guard_uwb_lease lease = {0};
    uint64_t round_deadline_ms = 0u;
    uint64_t first_receive_start_ms;
    uint16_t timeout_count = 0u;
    uint16_t raw_frame_count = 0u;
    uint16_t bundle_count = 0u;
    uint16_t error_count = 0u;
    bool functional_rx_outcome = false;
    int parking_ret;
    int release_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !app_gateway_enumeration_response_window(
            (uint64_t)k_uptime_get(), &round_deadline_ms)) {
        return false;
    }
    ret = mesh_rx_radio_claim("gateway compact enumeration RX", &lease);
    if (ret < 0) {
        mesh_schedule_uwb_rx(0u);
        return true;
    }
    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (ret < 0) {
        int configure_ret = ret;
        int recovery_ret = dwm3000_driver_force_recovery();

        ret = recovery_ret < 0 ? recovery_ret :
            dwm3000_driver_configure_wake_mesh_control_mode();
        status_debug_printf(
            "DBG_ENUM_COMPACT_GATEWAY_RECOVER stage=config first=%d recovery=%d ret=%d\n",
            configure_ret,
            recovery_ret,
            ret);
    }
    first_receive_start_ms = (uint64_t)k_uptime_get();
    status_debug_printf(
        "DBG_ENUM_COMPACT_GATEWAY_BEGIN now=%u first_window=%u ret=%d\n",
        (uint32_t)first_receive_start_ms,
        round_deadline_ms > first_receive_start_ms ?
            (uint32_t)(round_deadline_ms - first_receive_start_ms) : 0u,
        ret);
    while (ret == 0 &&
           app_gateway_enumeration_response_window(
               (uint64_t)k_uptime_get(), &round_deadline_ms)) {
        struct uwb_enumeration_bundle_frame bundle;
        struct uwb_enumeration_hop_ack_frame ack;
        enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_NONE;
        uint8_t encoded[UWB_ENUM_HOP_ACK_LEN];
        size_t frame_len = 0u;
        size_t encoded_len = 0u;
        uint8_t quality = 0u;
        uint64_t receive_start_ms = (uint64_t)k_uptime_get();
        uint32_t window_ms;
        uint64_t received_at_ms;

        if (receive_start_ms >= round_deadline_ms) {
            continue;
        }
        window_ms = round_deadline_ms - receive_start_ms > UINT32_MAX ?
            UINT32_MAX : (uint32_t)(round_deadline_ms - receive_start_ms);

        ret = dwm3000_driver_receive_frame_continuous(
            window_ms,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len,
            &quality,
            NULL,
            &failure);
        received_at_ms = (uint64_t)k_uptime_get();
        functional_rx_outcome =
            app_mesh_rx_policy_dwm_attempt_made_progress(ret, failure);
        if (ret == -ETIMEDOUT) {
            if (timeout_count < UINT16_MAX) {
                timeout_count++;
            }
            ret = 0;
            continue;
        }
        if (ret == -ECANCELED) {
            break;
        }
        if (ret < 0) {
            int receive_ret = ret;
            int recovery_ret = dwm3000_driver_force_recovery();

            if (error_count < UINT16_MAX) {
                error_count++;
            }

            ret = recovery_ret < 0 ? recovery_ret :
                dwm3000_driver_configure_wake_mesh_control_mode();
            status_debug_printf(
                "DBG_ENUM_COMPACT_GATEWAY_RECOVER stage=rx first=%d failure=%u recovery=%d ret=%d\n",
                receive_ret,
                (unsigned int)failure,
                recovery_ret,
                ret);
            continue;
        }
        if (uwb_decode_enumeration_bundle(
                frame, frame_len, &bundle) != PROTO_OK) {
            /* The compact lane owns extended-PHR Channel 5 exclusively.
             * Unrelated raw frames are harmless and must not escape into the
             * normal mesh decoder. */
            if (raw_frame_count < UINT16_MAX) {
                raw_frame_count++;
            }
            status_debug_printf(
                "DBG_ENUM_COMPACT_GATEWAY_RAW len=%u b0=%02x b1=%02x b2=%02x\n",
                (unsigned int)frame_len,
                frame_len > 0u ? frame[0] : 0u,
                frame_len > 1u ? frame[1] : 0u,
                frame_len > 2u ? frame[2] : 0u);
            ret = 0;
            continue;
        }
        if (bundle_count < UINT16_MAX) {
            bundle_count++;
        }
        ret = app_gateway_enumeration_response_handle_bundle(
            &bundle, received_at_ms, &ack);
        if (ret == 0) {
            int encode_ret = uwb_encode_enumeration_hop_ack(
                &ack, encoded, sizeof(encoded), &encoded_len);

            if (encode_ret != PROTO_OK) {
                status_debug_printf(
                    "DBG_ENUM_COMPACT_GATEWAY_ACK_ENCODE_FAIL epoch=%u child=0x%016llx seq=%u ret=%d\n",
                    ack.epoch,
                    (unsigned long long)ack.child_id,
                    ack.sequence,
                    encode_ret);
                ret = mesh_errno_from_proto(encode_ret);
                functional_rx_outcome = false;
            }
        }
        if (ret == 0) {
            uint64_t ack_start_ms = (uint64_t)k_uptime_get();
            int ack_ret = dwm3000_driver_send_frame_tracked_until(
                encoded,
                encoded_len,
                UWB_CONTROL_TX_TIMEOUT_MS,
                round_deadline_ms,
                NULL);

            if (ack_ret == -ETIMEDOUT) {
                status_debug_printf(
                    "DBG_ENUM_COMPACT_GATEWAY_ACK_SKIP epoch=%u child=0x%016llx seq=%u remaining=%u\n",
                    ack.epoch,
                    (unsigned long long)ack.child_id,
                    ack.sequence,
                    round_deadline_ms > ack_start_ms ?
                        (uint32_t)(round_deadline_ms - ack_start_ms) : 0u);
                /* The child retains custody and retries in another random
                 * round. A late ACK is local contention, not a reason to tear
                 * down the gateway's continuous receiver. */
                ret = 0;
            } else {
                status_debug_printf(
                    "DBG_ENUM_COMPACT_GATEWAY_ACK_TX epoch=%u child=0x%016llx seq=%u remaining=%u ret=%d\n",
                    ack.epoch,
                    (unsigned long long)ack.child_id,
                    ack.sequence,
                    round_deadline_ms > ack_start_ms ?
                        (uint32_t)(round_deadline_ms - ack_start_ms) : 0u,
                    ack_ret);
                ret = ack_ret;
                if (ack_ret < 0) {
                    functional_rx_outcome = false;
                }
            }
        }
        status_debug_printf(
            "DBG_ENUM_COMPACT_GATEWAY_RX epoch=%u child=0x%016llx seq=%u records=%u ret=%d\n",
            bundle.epoch,
            (unsigned long long)bundle.sender_id,
            bundle.sequence,
            bundle.record_count,
            ret);
        if (ret >= 0) {
            ret = 0;
        }
    }
    status_debug_printf(
        "DBG_ENUM_COMPACT_GATEWAY_END now=%u timeouts=%u raw=%u bundles=%u errors=%u ret=%d\n",
        k_uptime_get_32(),
        timeout_count,
        raw_frame_count,
        bundle_count,
        error_count,
        ret);

    parking_ret = mesh_radio_idle_with_bounded_recovery(
        "gateway-enumeration-lane");
    release_ret = mesh_rx_radio_finish(&lease, parking_ret);
    app_gateway_enumeration_response_publish_pending();
    if (release_ret < 0) {
        mesh_uwb_rx_active = false;
        return true;
    } else if (functional_rx_outcome) {
        app_watchdog_note_radio_progress();
    }
    if (ret == -ECANCELED) {
        (void)app_node_comm_gateway_delivery_safe_boundary();
    }
    mesh_schedule_uwb_rx(0u);
    return true;
}

static void mesh_uwb_rx_work_handler(struct k_work *work)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint64_t channel9_peer_id = 0u;
    struct mesh_event_plan channel9_plan = {0};
    uint32_t window_ms;
    uint32_t observed_packet_ms = 0u;
    uint32_t protocol_validation_token = 0u;
    uint32_t channel9_rx_armed_ms = 0u;
    uint8_t channel9_timing_index = 0u;
    uint8_t channel9_frames_seen = 0u;
    int64_t uwb_window_start_ms = -1;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    bool channel9_event = false;
    bool channel5_gap_scan = false;
    bool gateway_route_preempt = false;
    bool gateway_continuous_ch5 = false;
    bool frame_processed_inline = false;
    bool route_control_handoff = false;
    bool channel9_peer_observed = false;
    bool functional_rx_outcome = false;
    bool hard_rx_error = false;
    uint16_t gateway_ch9_recoverable_rx_errors = 0u;
    uint32_t channel5_gap_window_ms = 0u;
    uint32_t channel5_gap_deadline_ms = 0u;
    uint16_t channel5_gap_frames_seen = 0u;
    int channel5_gap_last_error = 0;
    struct radio_guard_uwb_lease radio_lease = {0};
    int radio_release_ret;
    uint32_t route_adv_wait_ms = 0u;
    struct fw_radio_activity_decision coordinator_decision;
    int ret;

    ARG_UNUSED(work);
    if (radio_guard_uwb_poisoned()) {
        LOG_ERR("mesh RX worker suppressed while radio parking is poisoned: %d",
                radio_guard_uwb_poison_error());
        mesh_uwb_rx_active = false;
        return;
    }
    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        mesh_uwb_rx_active = false;
        return;
    }

    if (!mesh_role_uses_uwb_rx()) {
        mesh_uwb_rx_active = false;
        return;
    }
    mesh_coordinator_decide_now("uwb-rx", &coordinator_decision);
    if (!coordinator_decision.uwb_rx_allowed) {
        if (coordinator_decision.route_wait_allowed &&
            DEVICE_ROLE == ROLE_ANCHOR &&
            mesh_route_waiting_tx_valid) {
            mesh_schedule_route_waiting_retry_after("rx-coordinator", 0u);
        }
        if (coordinator_decision.report_tx_allowed) {
            report_tx_schedule(0u);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_UWB_RX_COORD_DEFER state=%s reason=%s route=%u q=%u\n",
                                fw_radio_activity_state_name(
                                    coordinator_decision.state),
                                coordinator_decision.reason == NULL ? "none" :
                                coordinator_decision.reason,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                report_tx_queue_used());
        }
        mesh_schedule_uwb_rx(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if (mesh_gateway_run_enumeration_response_slice()) {
        return;
    }
    if (mesh_rx_response_active()) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_UWB_RX_YIELD_EVENT_RESPONSE\n");
        }
        mesh_schedule_uwb_rx(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if ((DEVICE_ROLE == ROLE_ANCHOR && anchor_uwb_window_active()) ||
        mesh_pending_tx_blocks_uwb_rx()) {
        mesh_schedule_uwb_rx(mesh_uwb_rx_idle_delay_ms());
        return;
    }
    if (mesh_gateway_route_test_role()) {
        uint32_t now_ms = k_uptime_get_32();
        bool route_adv_pending;

        if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_YIELD_ADV now=%u due=%u\n",
                                k_uptime_get_32(),
                                app_node_comm_gateway_route_refresh_due_ms());
            (void)app_node_comm_gateway_route_refresh_schedule_now();
            mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
            return;
        }

        route_adv_pending = app_node_comm_gateway_route_refresh_pending_wait_ms(
            now_ms,
            &route_adv_wait_ms);
        window_ms = app_mesh_rx_policy_gateway_ch9_window_ms(
            UWB_MESH_GATEWAY_RX_WINDOW_MS,
            route_adv_pending,
            route_adv_wait_ms);
        if (route_adv_pending &&
            window_ms < UWB_MESH_GATEWAY_RX_WINDOW_MS) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_CAP now=%u win=%u adv_wait=%u due=%u\n",
                                k_uptime_get_32(),
                                window_ms,
                                route_adv_wait_ms,
                                app_node_comm_gateway_route_refresh_due_ms());
        }
        uint32_t gateway_rx_deadline_ms = k_uptime_get_32() + window_ms;
        uint32_t gateway_rx_slice_deadline_ms =
            k_uptime_get_32() + APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS;
        uint16_t recoverable_errors_in_slice = 0u;

        ret = -ETIMEDOUT;
        while (true) {
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms;
            uint32_t slice_remaining_ms;
            uint32_t ack_retry_wait_ms = 0u;
            uint32_t enumeration_wait_ms = 0u;
            uint32_t protocol_validation_token = 0u;
            bool functional_rx_outcome = false;
            struct radio_guard_uwb_lease radio_lease = {0};
            int parking_ret;
            int release_ret;

            if (uptime_deadline_reached(now_ms, gateway_rx_deadline_ms) ||
                uptime_deadline_reached(now_ms,
                                        gateway_rx_slice_deadline_ms)) {
                break;
            }
            if (app_gateway_enumeration_response_window(
                    (uint64_t)k_uptime_get(), NULL)) {
                status_debug_printf(
                    "DBG_GATEWAY_CH9_RX_YIELD_ENUM now=%u\n", now_ms);
                mesh_schedule_uwb_rx(0u);
                return;
            }
            remaining_ms =
                uptime_ms_until_deadline(now_ms, gateway_rx_deadline_ms);
            slice_remaining_ms = uptime_ms_until_deadline(
                now_ms, gateway_rx_slice_deadline_ms);
            remaining_ms = app_mesh_rx_policy_gateway_ch9_work_slice_ms(
                MIN(remaining_ms, slice_remaining_ms));
            if (mesh_gateway_handoff_due_batch_acks(k_uptime_get_32())) {
                /* Let the node-communication worker run the frozen ACK. */
                mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
                return;
            }
            if (mesh_gateway_batch_ack_retry_wait_ms(
                    k_uptime_get_32(), &ack_retry_wait_ms) &&
                ack_retry_wait_ms < remaining_ms) {
                remaining_ms = ack_retry_wait_ms;
            }
            if (app_gateway_enumeration_response_pending_wait_ms(
                    (uint64_t)k_uptime_get(), &enumeration_wait_ms) &&
                enumeration_wait_ms < remaining_ms) {
                remaining_ms = enumeration_wait_ms;
                status_debug_printf(
                    "DBG_GATEWAY_CH9_RX_CAP_ENUM now=%u wait=%u\n",
                    now_ms,
                    enumeration_wait_ms);
            }

            if (remaining_ms == 0u) {
                continue;
            }
            if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
                break;
            }

            ret = mesh_rx_radio_claim("mesh gateway continuous channel9 RX",
                                      &radio_lease);
            if (ret < 0) {
                if (radio_guard_uwb_poisoned()) {
                    LOG_ERR("gateway channel-9 RX rearm suppressed after radio parking failure: %d",
                            radio_guard_uwb_poison_error());
                    mesh_uwb_rx_active = false;
                    return;
                }
                if (ret == -ECANCELED) {
                    int priority_ret =
                        app_node_comm_gateway_delivery_safe_boundary();

                    status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_START_HANDOFF ret=%d\n",
                                        priority_ret);
                    if (priority_ret < 0) {
                        LOG_ERR("gateway command safe-boundary scheduling failed before RX start: %d",
                                priority_ret);
                    }
                    return;
                }
                status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_GUARD_FAIL ret=%d\n", ret);
                mesh_schedule_uwb_rx(0u);
                return;
            }
            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            uwb_window_start_ms = k_uptime_get();
            ret = dwm3000_driver_configure_mesh_payload_mode();
            mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
            if (ret == 0) {
                ret = mesh_gateway_protocol_validation_arm(
                    remaining_ms, &protocol_validation_token);
            }
            if (ret == 0) {
                int rx_ret;
                uint32_t rx_started_ms = k_uptime_get_32();

                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf(
                        "DBG_GATEWAY_CH9_RX_CONT_ARM now=%u rem=%u frames=%u errors=%u\n",
                        rx_started_ms,
                        remaining_ms,
                        channel9_frames_seen,
                        gateway_ch9_recoverable_rx_errors);
                }

                rx_ret = dwm3000_driver_receive_frame_continuous(
                    remaining_ms,
                    frame,
                    UWB_MESH_MAX_FRAME_LEN,
                    &frame_len,
                    &quality,
                    NULL,
                    &rx_failure);
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    struct dwm3000_rx_debug_snapshot dbg_snap;

                    dwm3000_driver_last_rx_debug_get(&dbg_snap);
                    status_debug_printf(
                        "DBG_GATEWAY_CH9_RX_CONT_RESULT start=%u end=%u ret=%d fail=%u status=0x%08x len=%u frames=%u\n",
                        rx_started_ms,
                        k_uptime_get_32(),
                        rx_ret,
                        (unsigned int)rx_failure,
                        dbg_snap.status,
                        (unsigned int)frame_len,
                        channel9_frames_seen);
                }
                functional_rx_outcome =
                    app_mesh_rx_policy_dwm_attempt_made_progress(
                        rx_ret, rx_failure);
                ret = rx_ret;
            }

            if (ret == 0) {
                ret = mesh_gateway_protocol_validation_complete(
                    protocol_validation_token, &observed_packet_ms);
            }
            if (ret == 0) {
                channel9_frames_seen++;
                status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
                parking_ret = mesh_radio_idle_with_bounded_recovery(
                    "gateway-ch9-frame");
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_GATEWAY_CH9_RX_RELEASE_IDLE\n");
                }
            } else if (ret == -ECANCELED) {
                parking_ret = mesh_radio_idle_with_bounded_recovery(
                    "gateway-ch9-command-preempt");
                status_debug_printf("DBG_GATEWAY_CH9_RX_COMMAND_PREEMPT now=%u rem=%u frames=%u\n",
                                    k_uptime_get_32(),
                                    remaining_ms,
                                    channel9_frames_seen);
            } else if (ret == -ETIMEDOUT) {
                /*
                 * An empty bounded slice is normal receiver housekeeping,
                 * not a reason to put the DWM3000 to sleep.  Keep the PHY
                 * configured and awake so the next cooperative slice only
                 * pays the short idle-to-RX transition.
                 */
                parking_ret = mesh_radio_idle_with_bounded_recovery(
                    "gateway-ch9-empty-slice");
            } else if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
                           ret, rx_failure)) {
                if (gateway_ch9_recoverable_rx_errors < UINT16_MAX) {
                    gateway_ch9_recoverable_rx_errors++;
                }
                if (recoverable_errors_in_slice < UINT16_MAX) {
                    recoverable_errors_in_slice++;
                }
                /* The driver already forces RX off and clears terminal status. */
                parking_ret = mesh_radio_idle_with_bounded_recovery(
                    "gateway-ch9-recoverable-rx");
            } else {
                parking_ret = mesh_radio_standby_with_bounded_recovery(
                    "gateway-ch9-hard-rx");
            }
            mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
            release_ret = mesh_rx_radio_finish(&radio_lease, parking_ret);
            if (release_ret < 0) {
                status_debug_printf(
                    "DBG_GATEWAY_CH9_RX_CONT_RELEASE_FAIL ret=%d rx_ret=%d\n",
                    release_ret,
                    ret);
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                mesh_uwb_rx_active = false;
                return;
            } else if (functional_rx_outcome) {
                app_watchdog_note_radio_progress();
            }

            if (ret != 0) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                if (ret == -ECANCELED) {
                    int priority_ret =
                        app_node_comm_gateway_delivery_safe_boundary();

                    if (priority_ret < 0) {
                        LOG_ERR("gateway command safe-boundary scheduling failed after RX abort: %d",
                                priority_ret);
                    }
                    break;
                }
                if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
                        ret, rx_failure)) {
                    static bool storm_reinit_attempted;

                    if (gateway_ch9_recoverable_rx_errors <= 3u ||
                        (gateway_ch9_recoverable_rx_errors % 64u) == 0u) {
                        struct dwm3000_rx_debug_snapshot dbg_snap;

                        dwm3000_driver_last_rx_debug_get(&dbg_snap);
                        status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_RECOVER ret=%d fail=%u rem=%u errors=%u frames=%u reinit=%u status=0x%08x phy=%u pac=%u sfd=%u phr=%u\n",
                                            ret,
                                            (unsigned int)rx_failure,
                                            remaining_ms,
                                            gateway_ch9_recoverable_rx_errors,
                                            channel9_frames_seen,
                                            storm_reinit_attempted ? 1u : 0u,
                                            dbg_snap.status,
                                            dbg_snap.phy_mode,
                                            dbg_snap.pac,
                                            dbg_snap.sfd_type,
                                            dbg_snap.phr_mode);
                    }

                    if (!storm_reinit_attempted && channel9_frames_seen == 0u &&
                        recoverable_errors_in_slice >= 2u) {
                        storm_reinit_attempted = true;
                        /* A storm this early with zero decoded frames means
                         * the receiver never locked; one full re-init either
                         * clears stale radio state or proves the cause is not
                         * driver state at all. */
                        status_debug_printf("DBG_GATEWAY_CH9_RX_STORM_REINIT errors=%u\n",
                                            gateway_ch9_recoverable_rx_errors);
                        (void)dwm3000_driver_force_recovery();
                        (void)dwm3000_driver_configure_mesh_payload_mode();
                    }
                    if (app_mesh_rx_policy_gateway_ch9_should_yield_recovery(
                            recoverable_errors_in_slice)) {
                        status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_RECOVERY_YIELD errors=%u frames=%u\n",
                                            recoverable_errors_in_slice,
                                            channel9_frames_seen);
                        break;
                    }
                    continue;
                }
                if (ret != -ETIMEDOUT) {
                    status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_FAIL ret=%d fail=%u\n",
                                        ret,
                                        (unsigned int)rx_failure);
                    LOG_WRN("mesh gateway continuous channel-9 RX failed: ret=%d", ret);
                }
                break;
            }

            if (dwm3000_driver_receive_abort_pending()) {
                bool valid_mesh_frame = false;
                bool queued;

                queued = mesh_queue_from_frame_at_internal(
                    frame,
                    frame_len,
                    quality,
                    UWB_CHANNEL_MESH_PAYLOAD,
                    observed_packet_ms,
                    protocol_validation_token,
                    NULL,
                    0u,
                    false,
                    &valid_mesh_frame,
                    NULL);
                if (!queued) {
                    gateway_command_result_validation_release_reserved(
                        protocol_validation_token);
                }
                protocol_validation_token = 0u;
                status_debug_printf("DBG_GATEWAY_CH9_RX_FRAME_DEFER_COMMAND now=%u valid=%u frames=%u\n",
                                    k_uptime_get_32(),
                                    valid_mesh_frame ? 1u : 0u,
                                    channel9_frames_seen);
                /*
                 * The frame completed before the abort boundary, so its
                 * receive custody is older than the control which requested
                 * that boundary.  Drain it while the radio is parked before
                 * scheduling another control attempt.  In particular, a
                 * command result must be able to cancel its own request;
                 * leaving it queued makes the request repeatedly preempt the
                 * only proof that can stop those retries.
                 */
                (void)mesh_process_queued_rx_at_gateway_rx_boundary();
                {
                    int priority_ret =
                        app_node_comm_gateway_delivery_safe_boundary();

                    if (priority_ret < 0) {
                        LOG_ERR("gateway command safe-boundary scheduling failed after RX frame: %d",
                                priority_ret);
                    }
                }
                break;
            }

            {
                (void)mesh_process_received_frame(frame,
                                                  frame_len,
                                                  quality,
                                                  true,
                                                  0u,
                                                  NULL,
                                                  observed_packet_ms,
                                                  protocol_validation_token,
                                                  NULL);
                protocol_validation_token = 0u;
                /* The receiver has parked and released the physical radio.
                 * Drain this exact frame before rearming so its bounded
                 * validation lease cannot expire behind the receiver's own
                 * coordinator state. */
                (void)mesh_process_queued_rx_at_gateway_rx_boundary();
            }
        }

        if (ret != -ETIMEDOUT) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_DONE ret=%d len=%u fail=%u recover=%u\n",
                                ret,
                                (unsigned int)frame_len,
                                (unsigned int)rx_failure,
                                gateway_ch9_recoverable_rx_errors);
        }
        if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_DONE_YIELD_ADV now=%u due=%u\n",
                                k_uptime_get_32(),
                                app_node_comm_gateway_route_refresh_due_ms());
            (void)app_node_comm_gateway_route_refresh_schedule_now();
            mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
            return;
        }
        mesh_schedule_uwb_rx(
            app_mesh_rx_policy_gateway_ch9_rearm_delay_ms());
        return;
    }

    gateway_route_preempt = mesh_gateway_route_test_preempt_active(k_uptime_get_32());
    gateway_continuous_ch5 = mesh_gateway_route_test_role() &&
                             mesh_channel9_connection_count() == 0u;
    if (app_mesh_c5_gateway_rx_should_yield_to_response(
            &(const struct app_mesh_c5_flood_priority_state) {
                .response_priority =
                    app_node_comm_gateway_route_refresh_response_priority_due(
                        k_uptime_get_32()),
                .gateway_ch5_preempt = gateway_route_preempt,
            })) {
        (void)app_node_comm_gateway_route_refresh_schedule_now();
        mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
        return;
    }
    channel9_event = !gateway_route_preempt &&
                     mesh_select_channel9_ack_tx_event(k_uptime_get_32(),
                                                       &channel9_plan,
                                                       &channel9_peer_id);
    if (channel9_event) {
        (void)mesh_send_pending_ch9_ack_batch(&channel9_plan,
                                              channel9_peer_id,
                                              "ch9-ack-batch-slot");
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    channel9_event = !gateway_route_preempt &&
                     mesh_select_channel9_rx_event(k_uptime_get_32(),
                                                   &channel9_plan,
                                                   &channel9_peer_id,
                                                   &channel9_timing_index);
    if (DEVICE_ROLE == ROLE_CLICKER && !channel9_event) {
        /*
         * A clicker is a scheduled leaf, not a passive receiver. Keep the
         * worker alive for its next negotiated TX/RX turn without opening a
         * channel-5 gap scan between events.
         */
        mesh_schedule_uwb_rx(
            mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
        !channel9_event) {
        uint32_t next_delay_ms =
            mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

        status_debug_printf("DBG_TX_CH9_RX_WAIT next_delay=%u conn=%u ack=%u\n",
                            next_delay_ms,
                            mesh_channel9_connection_count(),
                            mesh_ch9_tx_pending_is_active() ? 1u : 0u);
        mesh_schedule_uwb_rx(next_delay_ms);
        return;
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !channel9_event &&
        mesh_channel9_connection_count() == 0u) {
        mesh_anchor_yield_idle_ch5_to_low_duty_scan("anchor-low-duty-ch5-owner");
        return;
    }
    if (gateway_route_preempt && !channel9_event) {
        status_debug_printf("DBG_GATEWAY_CH5_PREEMPT_SCAN peer=0x%llx now=%u until=%u\n",
                            (unsigned long long)mesh_gateway_route_preempt_peer_id,
                            k_uptime_get_32(),
                            mesh_gateway_route_preempt_deadline_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_channel9_connection_count() > 0u &&
        !channel9_event) {
        channel5_gap_window_ms = mesh_active_channel9_ch5_gap_window_ms(k_uptime_get_32());
        if (channel5_gap_window_ms == 0u) {
            uint32_t next_delay_ms = mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                mesh_channel9_connection_count() > 0u &&
                next_delay_ms > MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS) {
                status_debug_printf("DBG_CH5_GAP_SCAN_SKIP now=%u next_delay=%u\n",
                                    k_uptime_get_32(),
                                    next_delay_ms);
            }
            next_delay_ms =
                app_mesh_c5_connected_gap_reschedule_ms(
                    next_delay_ms,
                    MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
                    MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS);
            mesh_schedule_uwb_rx(next_delay_ms);
            return;
        }
        channel5_gap_scan = true;
    }

    ret = mesh_rx_radio_claim(channel9_event ?
                              "mesh channel9 UWB RX" :
                              "mesh UWB RX",
                              &radio_lease);
    if (ret < 0) {
        if (radio_guard_uwb_poisoned()) {
            LOG_ERR("mesh RX rearm suppressed after radio parking failure: %d",
                    radio_guard_uwb_poison_error());
            mesh_uwb_rx_active = false;
            return;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
            status_debug_printf("DBG_RX_GUARD_FAIL mode=%u ret=%d\n",
                                channel9_event ? 9u : 5u,
                                ret);
            LOG_WRN("mesh gateway RX window guard failed: mode=%s ret=%d",
                    channel9_event ? "ch9" : "ch5",
                    ret);
        }
        mesh_schedule_uwb_rx(mesh_gateway_route_test_preempt_active(k_uptime_get_32()) ?
                             0u : mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }
    window_ms = channel9_event ? channel9_plan.window_ms :
                (gateway_route_preempt && !channel5_gap_scan) ?
                    mesh_gateway_route_test_preempt_window_ms(k_uptime_get_32()) :
                channel5_gap_scan ? channel5_gap_window_ms :
                gateway_continuous_ch5 ? MESH_GATEWAY_CH5_CONTINUOUS_RX_MS :
                mesh_uwb_rx_window_ms();
    if (gateway_route_preempt && !channel5_gap_scan) {
        uint32_t response_wait_ms =
            app_node_comm_gateway_route_refresh_response_priority_wait_ms(
                k_uptime_get_32());

        if (response_wait_ms > 0u && response_wait_ms < window_ms) {
            window_ms = response_wait_ms;
        }
    }
    if (channel5_gap_scan) {
        channel5_gap_deadline_ms = k_uptime_get_32() + window_ms;
    }
    uwb_window_start_ms = k_uptime_get();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
        mesh_ch9_event_set(CH9_EVENT_GRANTED,
                           channel9_peer_id,
                           &channel9_plan,
                           "rx-slot");
        status_debug_printf("DBG_CH9_RX_START idx=%u now=%u start=%u end=%u tail=%u\n",
                            channel9_timing_index,
                            k_uptime_get_32(),
                            channel9_plan.start_ms,
                            channel9_plan.end_ms,
                            MESH_EVENT_RX_LATE_GUARD_MS);
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
               (channel5_gap_scan || gateway_route_preempt ||
                gateway_continuous_ch5)) {
        status_debug_printf("DBG_CH5_GAP_SCAN now=%u win=%u next_delay=%u\n",
                            k_uptime_get_32(),
                            window_ms,
                            mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    }
    ret = channel9_event ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_wake_mode();
    if (channel9_event) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0 && channel9_event) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_RX_CONFIG_OK\n");
            status_debug_printf("DBG_CH9_RX_EARLY_ARM now=%u start=%u guard=%u\n",
                                k_uptime_get_32(),
                                channel9_plan.start_ms,
                                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS);
        } else {
            mesh_wait_until_ms(channel9_plan.start_ms);
        }
    } else if (channel9_event && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_RX_CONFIG_FAIL\n");
        status_debug_printf("DBG_CH9_RX_CONFIG_FAIL ret=%d now=%u start=%u\n",
                            ret,
                            k_uptime_get_32(),
                            channel9_plan.start_ms);
    }
    if (ret == 0 && channel9_event && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        const uint32_t deadline_ms = channel9_plan.end_ms + MESH_EVENT_RX_LATE_GUARD_MS;

        frame_processed_inline = true;
        ret = -ETIMEDOUT;
        channel9_rx_armed_ms = k_uptime_get_32();
        status_debug_printf("DBG_CH9_RX_ARMED now=%u start=%u end=%u deadline=%u\n",
                            channel9_rx_armed_ms,
                            channel9_plan.start_ms,
                            channel9_plan.end_ms,
                            deadline_ms);
        while (true) {
            struct dwm3000_rx_frame_timing rx_timing = {0};
            bool peer_observed_this_frame = false;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms;

            if (uptime_deadline_reached(now_ms, deadline_ms)) {
                break;
            }
            remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            ret = mesh_gateway_protocol_validation_arm(
                remaining_ms, &protocol_validation_token);
            if (ret == 0) {
                int rx_ret = dwm3000_driver_receive_frame_continuous_timed(
                    remaining_ms,
                    frame,
                    UWB_MESH_MAX_FRAME_LEN,
                    &frame_len,
                    &quality,
                    NULL,
                    &rx_failure,
                    &rx_timing);
                functional_rx_outcome =
                    app_mesh_rx_policy_dwm_attempt_made_progress(
                        rx_ret, rx_failure);
                ret = rx_ret;
            }
            if (ret == -ETIMEDOUT) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                break;
            }
            if (ret != 0) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                break;
            }

            ret = mesh_gateway_protocol_validation_complete(
                protocol_validation_token, &observed_packet_ms);
            if (ret < 0) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                break;
            }
            channel9_frames_seen++;
            status_debug_note("DBG_CH9_RX_FRAME\n");
            status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
            if (rx_timing.valid) {
                int32_t host_delta_ms = (int32_t)(observed_packet_ms - channel9_plan.start_ms);
                int32_t arm_delta_ms = (int32_t)(channel9_rx_armed_ms - channel9_plan.start_ms);
                int32_t hw_slot_delta_us =
                    (arm_delta_ms * 1000) + (int32_t)rx_timing.rx_since_enable_uus;

                status_debug_printf("DBG_CH9_RX_HW since_us=%u host_delta=%d en32=0x%08x rx32=0x%08x\n",
                                    rx_timing.rx_since_enable_uus,
                                    host_delta_ms,
                                    rx_timing.rx_enable_time32,
                                    rx_timing.rx_timestamp_time32);
                status_debug_printf("DBG_CH9_RX_TIMING arm_delta=%d hw_delta_us=%d host_delta=%d len=%u q=%u\n",
                                    arm_delta_ms,
                                    hw_slot_delta_us,
                                    host_delta_ms,
                                    (unsigned int)frame_len,
                                    quality);
            }
            (void)mesh_process_received_frame(frame,
                                              frame_len,
                                              quality,
                                              true,
                                              channel9_peer_id,
                                              &channel9_plan,
                                              observed_packet_ms,
                                              protocol_validation_token,
                                              &peer_observed_this_frame);
            protocol_validation_token = 0u;
            channel9_peer_observed = channel9_peer_observed || peer_observed_this_frame;
        }
    } else if (ret == 0 && channel9_event) {
        ret = mesh_gateway_protocol_validation_arm(
            window_ms, &protocol_validation_token);
        if (ret == 0) {
            int rx_ret = dwm3000_driver_receive_frame(
                window_ms,
                frame,
                UWB_MESH_MAX_FRAME_LEN,
                &frame_len,
                &quality,
                NULL);

            functional_rx_outcome =
                app_mesh_rx_policy_dwm_attempt_made_progress(
                    rx_ret, DWM3000_RX_FAILURE_NONE);
            ret = rx_ret;
        }
        if (ret == 0) {
            ret = mesh_gateway_protocol_validation_complete(
                protocol_validation_token, &observed_packet_ms);
        }
    } else if (ret == 0 && channel5_gap_scan) {
        frame_processed_inline = true;
        ret = -ETIMEDOUT;
        while (true) {
            struct uwb_wake_claim_frame claim;
            enum app_mesh_c5_connected_gap_rx_action action;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms;
            bool click_claim;
            bool route_control_claim = false;

            if (uptime_deadline_reached(now_ms,
                                        channel5_gap_deadline_ms)) {
                break;
            }
            remaining_ms = uptime_ms_until_deadline(
                now_ms, channel5_gap_deadline_ms);
            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            ret = mesh_gateway_protocol_validation_arm(
                remaining_ms, &protocol_validation_token);
            if (ret == 0) {
                int rx_ret = dwm3000_driver_receive_frame_continuous(
                    remaining_ms,
                    frame,
                    UWB_MESH_MAX_FRAME_LEN,
                    &frame_len,
                    &quality,
                    NULL,
                    &rx_failure);
                functional_rx_outcome =
                    app_mesh_rx_policy_dwm_attempt_made_progress(
                        rx_ret, rx_failure);
                ret = rx_ret;
            }
            if (ret == -ETIMEDOUT) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                break;
            }
            if (ret < 0) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                channel5_gap_last_error = ret;
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_CH5_GAP_REARM_ERROR ret=%d fail=%u rem=%u\n",
                                        ret,
                                        (unsigned int)rx_failure,
                                        remaining_ms);
                }
                if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
                        ret, rx_failure)) {
                    continue;
                }
                hard_rx_error = true;
                break;
            }

            ret = mesh_gateway_protocol_validation_complete(
                protocol_validation_token, &observed_packet_ms);
            if (ret < 0) {
                gateway_command_result_validation_release_reserved(
                    protocol_validation_token);
                protocol_validation_token = 0u;
                channel5_gap_last_error = ret;
                continue;
            }
            status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
            click_claim = mesh_frame_requires_anchor_click_handoff(frame,
                                                                   frame_len,
                                                                   &claim);
            if (!click_claim &&
                mesh_decode_channel5_wake_claim(frame,
                                                frame_len,
                                                &claim,
                                                NULL) &&
                app_mesh_c5_wake_followup_is_control(claim.flags)) {
                route_control_claim = app_mesh_c5_route_wake_claim_allowed(
                    claim.clicker_id,
                    GATEWAY_ID,
                    claim.flags,
                    IS_ENABLED(
                        CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ),
                    IS_ENABLED(
                        CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL));
            }
            action = app_mesh_c5_connected_gap_rx_action(
                click_claim,
                route_control_claim,
                uptime_deadline_reached(k_uptime_get_32(),
                                        channel5_gap_deadline_ms));
            if (action == APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK) {
                frame_processed_inline = false;
                ret = 0;
                break;
            }
            if (action ==
                APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL) {
                route_control_handoff = true;
                frame_processed_inline = false;
                ret = 0;
                break;
            }

            channel5_gap_frames_seen++;
            (void)mesh_process_received_frame(frame,
                                              frame_len,
                                              quality,
                                              false,
                                              0u,
                                              NULL,
                                              observed_packet_ms,
                                              protocol_validation_token,
                                              NULL);
            protocol_validation_token = 0u;
            if (action == APP_MESH_C5_CONNECTED_GAP_RX_COMPLETE) {
                break;
            }
        }
        if (frame_processed_inline && !hard_rx_error) {
            ret = -ETIMEDOUT;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH5_GAP_SCAN_DONE frames=%u last_error=%d now=%u deadline=%u click=%u\n",
                                channel5_gap_frames_seen,
                                channel5_gap_last_error,
                                k_uptime_get_32(),
                                channel5_gap_deadline_ms,
                                frame_processed_inline ? 0u : 1u);
        }

    } else if (ret == 0) {
        ret = mesh_gateway_protocol_validation_arm(
            window_ms, &protocol_validation_token);
        if (ret == 0) {
            int rx_ret = dwm3000_driver_receive_frame_continuous(
                window_ms,
                frame,
                UWB_MESH_MAX_FRAME_LEN,
                &frame_len,
                &quality,
                NULL,
                &rx_failure);
            functional_rx_outcome =
                app_mesh_rx_policy_dwm_attempt_made_progress(
                    rx_ret, rx_failure);
            ret = rx_ret;
        }
        if (ret == 0) {
            ret = mesh_gateway_protocol_validation_complete(
                protocol_validation_token, &observed_packet_ms);
        }
    }
    if (ret == 0 && !frame_processed_inline) {
        status_debug_gateway_uwb_rx_channel_pulse(channel9_event ? UWB_CHANNEL_MESH_PAYLOAD :
                                                                  UWB_CHANNEL_WAKE_CONTACT);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        DEVICE_ROLE == ROLE_GATEWAY && !channel9_event) {
        radio_release_ret = mesh_radio_idle_with_bounded_recovery(
            "gateway-channel5-rx");
    } else {
        radio_release_ret = mesh_release_radio_after_mesh_turn(
            channel9_event,
            channel9_event ? "ch9-rx" : "mesh-rx");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_release_ret = mesh_rx_radio_finish(&radio_lease, radio_release_ret);
    if (radio_release_ret < 0) {
        status_debug_printf("DBG_MESH_RX_RELEASE_FAIL ret=%d rx_ret=%d\n",
                            radio_release_ret,
                            ret);
        gateway_command_result_validation_release_reserved(
            protocol_validation_token);
        mesh_uwb_rx_active = false;
        return;
    } else if (functional_rx_outcome) {
        app_watchdog_note_radio_progress();
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    if (channel9_event) {
        /* The low-duty worker may not run until this owner is already gone. */
        app_anchor_note_channel9_window_released();
    }
#endif

    if (route_control_handoff) {
        frame_processed_inline =
            mesh_anchor_handoff_route_wake_frame(frame,
                                                 frame_len,
                                                 quality);
        ret = frame_processed_inline ? -ETIMEDOUT : 0;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
        uint32_t now_ms = k_uptime_get_32();
        bool log_window = ret == 0 || ret != -ETIMEDOUT || channel9_event ||
                          mesh_rx_window_log_next_ms == 0u ||
                          now_ms >= mesh_rx_window_log_next_ms;

        if (log_window) {
            mesh_rx_window_log_next_ms = now_ms + MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS;
            if (channel9_event || ret != -ETIMEDOUT) {
                status_debug_printf("DBG_RX_DONE mode=%u ret=%d len=%u fail=%u frames=%u\n",
                                    channel9_event ? 9u : 5u,
                                    ret,
                                    (unsigned int)frame_len,
                                    (unsigned int)rx_failure,
                                    channel9_frames_seen);
            }
        }
    }

    if (ret == 0 && !frame_processed_inline) {
        (void)mesh_process_received_frame(frame,
                                          frame_len,
                                          quality,
                                          channel9_event,
                                          channel9_peer_id,
                                          &channel9_plan,
                                          observed_packet_ms,
                                          protocol_validation_token,
                                          &channel9_peer_observed);
        protocol_validation_token = 0u;
    } else if (ret != -ETIMEDOUT) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
            status_debug_note("DBG_CH9_RX_FAIL\n");
        }
        LOG_WRN("mesh UWB RX failed: ret=%d role=%s", ret, role_name());
	    } else if (channel9_event && !channel9_peer_observed &&
	               channel9_timing_index < MESH_RELAY_EVENT_TIMINGS &&
	               mesh_runtime.event_timings[channel9_timing_index].valid) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note(channel9_frames_seen == 0u ?
	                              "DBG_CH9_RX_EMPTY\n" :
	                              "DBG_CH9_RX_NO_EXPECTED_PEER\n");
	            status_debug_note("DBG_CH9_RX_TIMEOUT\n");
	            status_debug_printf("DBG_CH9_RX_MISS frames=%u now=%u start=%u end=%u fail=%u\n",
	                                channel9_frames_seen,
	                                k_uptime_get_32(),
	                                channel9_plan.start_ms,
	                                channel9_plan.end_ms,
	                                (unsigned int)rx_failure);
	        }
	        /* The peer is allowed to skip an empty transmit turn. Advance
	         * parity, but reserve the miss budget for an attempted transfer
	         * that actually failed. Silent-peer cleanup remains bounded by
	         * the negotiated supervision timeout. */
	        mesh_relay_note_channel9_unobserved_turn(
	            &mesh_runtime, channel9_peer_id, channel9_plan.start_ms);
		    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
		        status_debug_note(channel9_peer_observed ?
		                          "DBG_CH9_RX_DONE_WITH_PEER\n" :
		                          "DBG_CH9_RX_TIMEOUT\n");
		    }

    if (channel9_event) {
        if (ret != 0 && ret != -ETIMEDOUT) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot-fail");
        } else if (channel9_peer_observed) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot");
        } else {
            mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot");
        }
    }
    gateway_command_result_validation_release_reserved(
        protocol_validation_token);

    if (channel5_gap_scan) {
        uint32_t next_delay_ms = mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

        mesh_schedule_uwb_rx(
            app_mesh_c5_connected_gap_reschedule_ms(
                next_delay_ms,
                MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
                MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS));
    } else if (app_mesh_c5_gateway_rx_should_yield_to_response(
            &(const struct app_mesh_c5_flood_priority_state) {
                .response_priority =
                    app_node_comm_gateway_route_refresh_response_priority_due(
                        k_uptime_get_32()),
                .gateway_ch5_preempt =
                    mesh_gateway_route_test_preempt_active(k_uptime_get_32()),
            })) {
        (void)app_node_comm_gateway_route_refresh_schedule_now();
        mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
    } else if (mesh_gateway_route_test_preempt_active(k_uptime_get_32()) ||
               (mesh_gateway_route_test_role() &&
                mesh_channel9_connection_count() == 0u)) {
        mesh_schedule_uwb_rx(0u);
    } else {
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    }
}

int mesh_start_uwb_rx(const char *reason)
{
    int ret;

    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }

    ret = mesh_schedule_uwb_rx(0u);
    if (ret < 0) {
        LOG_ERR("mesh UWB RX schedule failed: role=%s reason=%s ret=%d",
                role_name(),
                reason == NULL ? "start" : reason,
                ret);
        return ret;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_START reason=%s active=%u qrx=%u qtx=%u adv_due=%u\n",
                            reason == NULL ? "start" : reason,
                            mesh_uwb_rx_active ? 1u : 0u,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            report_tx_queue_used(),
                            app_node_comm_gateway_route_refresh_due_ms());
    }
    LOG_INF("mesh UWB RX scheduled: role=%s window_ms=%u idle_ms=%u reason=%s",
            role_name(),
            mesh_uwb_rx_window_ms(),
            mesh_uwb_rx_idle_delay_ms(),
            reason == NULL ? "start" : reason);
    return 0;
}


static bool mesh_route_refresh_allowed(void *ctx)
{
    ARG_UNUSED(ctx);
    return app_mesh_c5_gateway_route_adv_allowed(
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST));
}

static bool mesh_route_refresh_policy_running(void *ctx)
{
    ARG_UNUSED(ctx);
    return app_node_comm_policy_running();
}

static bool mesh_route_refresh_response_active(uint32_t now_ms, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!mesh_gateway_route_test_role() ||
        mesh_gateway_route_preempt_peer_id == 0u ||
        !mesh_gateway_route_test_preempt_active(now_ms)) {
        return false;
    }
    return mesh_c5_contact_active(mesh_gateway_route_preempt_peer_id,
                                  C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                  now_ms);
}

static uint32_t mesh_route_refresh_now(void *ctx)
{
    ARG_UNUSED(ctx);
    return k_uptime_get_32();
}

static int mesh_route_refresh_build(void *ctx,
                                    uint32_t sequence,
                                    uint32_t now_ms,
                                    struct mesh_gateway_route_adv_snapshot *snapshot,
                                    struct mesh_outbound *out)
{
    struct operation_policy_set operation_policy;
    int ret;

    ARG_UNUSED(ctx);
    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!snapshot->valid) {
        app_operation_policy_snapshot(&operation_policy);
        ret = mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
            &mesh_runtime,
            sequence,
            now_ms,
            &operation_policy,
            snapshot);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return mesh_relay_build_gateway_route_adv_from_snapshot(&mesh_runtime,
                                                            snapshot,
                                                            out);
}

static int mesh_route_refresh_send_wake(void *ctx, const char *reason)
{
    ARG_UNUSED(ctx);
    return mesh_send_route_wake_train(
        MESH_BROADCAST_ID, NULL, NULL,
        C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH, reason, NULL, NULL,
        FW_C5_TX_INTENT_BACKGROUND, NULL, NULL);
}

static void mesh_route_refresh_note_sent(const struct mesh_outbound *out,
                                         uint32_t now_ms,
                                         void *ctx)
{
    ARG_UNUSED(ctx);
    mesh_relay_note_tx_sent(&mesh_runtime, out, now_ms);
}

static int mesh_route_refresh_begin_radio_control(void *ctx)
{
    bool abort_scan = false;
    int ret;

    ARG_UNUSED(ctx);
    if (!mesh_gateway_route_test_role()) {
        return 0;
    }
    if (!mesh_rx_handoff_begin_control(&abort_scan)) {
        return -EBUSY;
    }
    mesh_stop_role_scan();
    if (abort_scan) {
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    }
    ret = mesh_rx_handoff_wait_for_control();
    if (ret < 0) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
        return ret;
    }
    status_debug_printf("DBG_ROUTE_REFRESH_RX_HANDOFF_READY abort=%u\n",
                        abort_scan ? 1u : 0u);
    return 0;
}

static void mesh_route_refresh_end_radio_control(void *ctx)
{
    ARG_UNUSED(ctx);
    if (mesh_gateway_route_test_role()) {
        mesh_rx_handoff_end_control();
    }
}

static void mesh_route_refresh_stop_scan(void *ctx)
{
    ARG_UNUSED(ctx);
    if (mesh_gateway_route_test_role()) {
        mesh_stop_role_scan();
    }
}

static void mesh_route_refresh_restart_scan(void *ctx)
{
    ARG_UNUSED(ctx);
    if (mesh_gateway_route_test_role()) {
        mesh_restart_role_scan();
    }
}

static void mesh_route_refresh_clear_response(void *ctx)
{
    ARG_UNUSED(ctx);
    mesh_gateway_route_test_clear_preempt(0u, "route-adv-sent");
}

static int mesh_route_refresh_schedule(void *ctx,
                                       struct k_work_delayable *work,
                                       uint32_t delay_ms)
{
    ARG_UNUSED(ctx);
    return mesh_reschedule_owned_work(work,
                                      delay_ms,
                                      "gateway-route-refresh");
}

static int mesh_route_refresh_next_sequence(void *ctx, uint32_t *sequence)
{
    ARG_UNUSED(ctx);
    if (sequence == NULL) {
        return -EINVAL;
    }
    if (!app_gateway_control_sequence_admission_available(
            APP_GATEWAY_CONTROL_SEQUENCE_FORCED_ROUTE_REFRESH_BUDGET)) {
        return -EAGAIN;
    }
    return app_gateway_control_sequence_next(sequence);
}

static bool mesh_route_refresh_defer(void *ctx)
{
    struct mesh_c5_flood_tx_context flood_ctx = {
        .response_priority =
            app_node_comm_gateway_route_refresh_response_priority_due(
                k_uptime_get_32()),
    };

    ARG_UNUSED(ctx);
    return mesh_c5_flood_defer_active_cb(&flood_ctx);
}

static void mesh_route_refresh_observe(
    void *ctx, const struct app_node_comm_route_refresh_event *event)
{
    ARG_UNUSED(ctx);
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_route_refresh_event != NULL) {
        mesh_report_callbacks->gateway_route_refresh_event(event);
    }
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    struct app_node_comm_gateway_control_config gateway_control_config = {
        .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
        .send_flood = mesh_gateway_control_send_flood,
        .priority_observer = mesh_gateway_control_priority_observed,
    };
    const struct app_mesh_report_encode_ops report_encode_config = {
        .queue_cir_fragment = mesh_queue_anchor_cir_fragment,
    };
    static const struct app_node_comm_gateway_route_refresh_config
        route_refresh_config = {
            .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
            .wake_train_ms = WAKE_ADV_MS,
            .allowed = mesh_route_refresh_allowed,
            .policy_running = mesh_route_refresh_policy_running,
            .response_priority_active = mesh_route_refresh_response_active,
            .now_ms = mesh_route_refresh_now,
            .random_u32 = mesh_c5_flood_random_u32,
            .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
            .defer_active = mesh_route_refresh_defer,
            .c5_quiet = mesh_c5_flood_quiet_cb,
            .send = mesh_c5_flood_send_cb,
            .build = mesh_route_refresh_build,
            .send_wake = mesh_route_refresh_send_wake,
            .note_sent = mesh_route_refresh_note_sent,
            .begin_radio_control = mesh_route_refresh_begin_radio_control,
            .end_radio_control = mesh_route_refresh_end_radio_control,
            .stop_role_scan = mesh_route_refresh_stop_scan,
            .restart_role_scan = mesh_route_refresh_restart_scan,
            .clear_response_priority = mesh_route_refresh_clear_response,
            .schedule = mesh_route_refresh_schedule,
            .next_sequence = mesh_route_refresh_next_sequence,
            .observe = mesh_route_refresh_observe,
    };

    mesh_report_callbacks = callbacks;
    protocol_rx_downstream_activation_init(
        &mesh_enumeration_downstream_activation);
#if DEVICE_ROLE == ROLE_CLICKER
    /*
     * Clickers normally originate ranging rather than mesh traffic, but
     * physical self-test and future diagnostics still use the reliable
     * uplink path. Give that leaf path its own routing identity without
     * enabling anchor relay, capacity, command, or custody behavior.
     */
    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_CLICKER,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
#endif
    app_mesh_report_encode_init(&report_encode_config);
    atomic_set(&mesh_transport_paused_state, 0);
    app_mesh_rx_handoff_reset(&mesh_rx_handoff);
    fw_delivery_loss_init(&mesh_delivery_loss);
#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    k_work_queue_start(&mesh_route_work_q,
                       mesh_route_work_q_stack,
                       K_THREAD_STACK_SIZEOF(mesh_route_work_q_stack),
                       MESH_ROUTE_WORKQUEUE_PRIORITY,
                       &mesh_route_work_q_config);
    gateway_control_config.priority_work_queue = &mesh_route_work_q;
#endif
    app_node_comm_gateway_control_init(&gateway_control_config);
    app_node_comm_gateway_route_refresh_init(&route_refresh_config,
                                             (uint32_t)DEVICE_ID);
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
#if DEVICE_ROLE == ROLE_GATEWAY
    atomic_clear(&mesh_gateway_host_delivery_pending_state);
    atomic_clear(&mesh_gateway_host_receipt_received_state);
    atomic_clear(&mesh_gateway_host_delivery_semantic_accepted_state);
    atomic_clear(&mesh_gateway_host_delivery_semantic_finalized_state);
    atomic_clear(&mesh_gateway_host_delivery_relay_committed_state);
    atomic_clear(&mesh_gateway_host_delivery_ack_handoff_state);
    mesh_gateway_host_delivery_semantic_acceptance = -EINVAL;
    k_work_init_delayable(&mesh_gateway_host_delivery_retry_work,
                          mesh_gateway_host_delivery_retry_work_handler);
#endif
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init(&mesh_uwb_rx_rearm_work, mesh_uwb_rx_rearm_work_handler);
    k_work_init_delayable(&mesh_node_comm_cancel_work,
                          mesh_node_comm_cancel_work_handler);
    {
        k_spinlock_key_t cancel_key =
            k_spin_lock(&mesh_node_comm_cancel_lock);

        memset(&mesh_node_comm_cancel_request, 0,
               sizeof(mesh_node_comm_cancel_request));
        k_spin_unlock(&mesh_node_comm_cancel_lock, cancel_key);
    }
    memset(&mesh_delivery_health, 0, sizeof(mesh_delivery_health));
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
#if DEVICE_ROLE == ROLE_GATEWAY
    mesh_gateway_ack_store_init(&mesh_gateway_ack_store);
    mesh_gateway_ack_store_initialized = true;
    mesh_gateway_ack_store_attached = false;
#endif
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&mesh_route_waiting_work,
                          mesh_route_waiting_work_handler);
#if DEVICE_ROLE == ROLE_ANCHOR
    mesh_click_preempt_request_init();
    app_mesh_queue_head_owner_init(&report_tx_queue_head_owner);
    report_tx_queue_recovery_valid = false;
    report_tx_backoff_until_ms = 0u;
    report_tx_backoff_active = false;
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
#endif
    k_work_init_delayable(&mesh_c5_flood_work, mesh_c5_flood_work_handler);
    k_work_init_delayable(&mesh_route_discovery_work, mesh_route_discovery_work_handler);
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    k_work_init_delayable(&mesh_event_negotiation_retry_work,
                          mesh_event_negotiation_retry_work_handler);
    mesh_event_propose_clear();
    memset(&mesh_event_accept_retry, 0, sizeof(mesh_event_accept_retry));
    mesh_event_accept_rx_clear();
    mesh_event_owner_registry_reset(&mesh_event_owner_registry);
#if DEVICE_ROLE != ROLE_GATEWAY
    memset(mesh_ch9_close_intents, 0, sizeof(mesh_ch9_close_intents));
#endif
    memset(&mesh_event_local_proposal_windows, 0,
           sizeof(mesh_event_local_proposal_windows));
    mesh_event_operation_session_next = 0u;
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_init_delayable(&mesh_route_request_action_work,
                          mesh_route_request_action_work_handler);
    mesh_route_request_action_pending = false;
#endif
    return 0;
}

void mesh_delivery_health_get(struct mesh_delivery_health *health)
{
    if (health != NULL) {
        *health = mesh_delivery_health;
    }
}

int mesh_gateway_command_priority_submit(struct k_work_delayable *work)
{
    return app_node_comm_gateway_control_priority_submit(work);
}

int mesh_gateway_command_priority_safe_boundary(void)
{
    return app_mesh_command_orchestrator_safe_boundary(
        app_node_comm_gateway_control_context(),
        radio_guard_uwb_busy(),
        app_node_comm_gateway_control_safe_boundary_schedule,
        NULL);
}

static void mesh_gateway_control_priority_observed(void *ctx, int result)
{
    ARG_UNUSED(ctx);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_GATEWAY_COMMAND_PRIORITY_SUBMIT ret=%d now=%u\n",
                            result,
                            k_uptime_get_32());
    }
}
