#line 12150 __BASE_FILE__
#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_try_send_report_tx_ch9_batch(void)
{
    struct mesh_outbound *first = &report_tx_worker_scratch;
    struct mesh_outbound *queued = &report_tx_worker_scratch;
    struct mesh_outbound *tx = &report_tx_worker_scratch;
    struct mesh_ch9_slot_tx_context slot_tx = {
        .uwb_window_start_ms = -1,
    };
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    struct app_mesh_coordinator_decision coordinator_decision;
    uint64_t next_hop_id = 0u;
    uint32_t now_ms;
    uint32_t deadline_ms;
    uint32_t send_failure_retry_delay_ms = 0u;
    uint8_t sent_count = 0u;
    uint8_t max_sent;
    bool ack_wait_started = false;
    bool core_ack_wait_started = false;
    bool direct_gateway_batch = false;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        !mesh_ch9_tx_pending_can_start()) {
        return -ENOTSUP;
    }

    ret = k_msgq_peek(&report_tx_msgq, first);
    if (ret != 0 || !mesh_packet_prefers_channel9(&first->packet)) {
        return -ENOTSUP;
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(first, now_ms)) {
        return -EAGAIN;
    }

    ret = mesh_relay_select_next_hop(&mesh_runtime, first->packet.dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return -ENOTSUP;
    }
    max_sent = app_mesh_ch9_tx_max_in_flight(&first->packet,
                                             next_hop_id,
                                             MESH_CH9_TX_BATCH_MAX);
    if (app_mesh_ch9_tx_requires_tracked_single(&first->packet,
                                                next_hop_id,
                                                MESH_CH9_TX_BATCH_MAX)) {
        return -ENOTSUP;
    }
    direct_gateway_batch =
        next_hop_id == GATEWAY_ID &&
        first->packet.dst_id == GATEWAY_ID &&
        (first->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
    if (direct_gateway_batch) {
        mesh_init_direct_gateway_batch_plan(&plan, now_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_IMMEDIATE now=%u start=%u end=%u q=%u max=%u reserve=%u\n",
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                report_tx_queue_used(),
                                max_sent,
                                MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS);
        }
    } else {
        (void)mesh_advance_channel9_timing_past(next_hop_id, now_ms, "tx-batch-select");
        mesh_fill_channel5_requirements(&requirements);
        (void)mesh_expire_channel9_timings(now_ms, "queued-ch9-batch");
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   next_hop_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("tx-batch",
                                       "queued-ch9-batch",
                                       next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note(ret == PROTO_ERR_STALE ?
                                  "DBG_CH9_TX_BATCH_STALE\n" :
                                  "DBG_CH9_TX_BATCH_WAIT\n");
                status_debug_printf("DBG_CH9_TX_BATCH_WAIT ret=%d act=%u now=%u start=%u q=%u\n",
                                    ret,
                                    (unsigned int)plan.action,
                                    now_ms,
                                    plan.start_ms,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            if (mesh_event_plan_debugs_channel5(plan.action)) {
                mesh_ch9_event_set(CH9_EVENT_PREEMPTED_BY_C5,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch");
            }
            if (ret == PROTO_ERR_BUSY &&
                mesh_event_plan_is_policy_deferral(plan.action)) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &first->packet,
                        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
                uint32_t retry_delay_ms;

                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                    &mesh_report_rf_retry_bank,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "report-batch-channel5-deferral");
                app_mesh_test_note_report_tx_backoff(first->packet.seq,
                                                     -EBUSY,
                                                     retry_delay_ms);
                report_tx_schedule_backoff(
                    retry_delay_ms,
                    "report-batch-channel5-deferral");
                return -EALREADY;
            }
            if (ret == PROTO_ERR_BUSY &&
                plan.action == MESH_EVENT_PLAN_WAIT) {
                uint32_t retry_delay_ms =
                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;

                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    &retry_delay_ms);
                report_tx_schedule(retry_delay_ms);
                return -EALREADY;
            }
            return ret == PROTO_ERR_BUSY ? -EBUSY : -ENOTSUP;
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t event_counter = 0u;

        if (!direct_gateway_batch) {
            (void)mesh_debug_channel9_state(next_hop_id, &event_counter, NULL);
        }
        status_debug_printf("DBG_CH9_TX_BATCH_SLOT cnt=%u now=%u start=%u end=%u q=%u max=%u\n",
                            event_counter,
                            now_ms,
                            plan.start_ms,
                            plan.end_ms,
                            k_msgq_num_used_get(&report_tx_msgq),
                            max_sent);
        if (direct_gateway_batch) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_SELECT cnt=%u reserve=%u rx=%u guard=%u cfg=%u\n",
                                event_counter,
                                MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS,
                                MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS,
                                MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                MESH_CH9_TX_CONFIG_GUARD_MS);
        }
    }
    mesh_ch9_event_set(CH9_EVENT_GRANTED,
                       next_hop_id,
                       &plan,
                       "queued-ch9-batch");

    if (direct_gateway_batch) {
        return mesh_try_send_report_tx_ch9_direct_gateway_batch(&plan,
                                                                next_hop_id,
                                                                max_sent,
                                                                now_ms);
    }

    deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    while (sent_count < max_sent) {
        uint64_t queued_next_hop_id = 0u;
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;
        size_t sent_frame_len = 0u;
        bool fits;

        if (k_msgq_peek(&report_tx_msgq, queued) != 0) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=empty sent=%u now=%u q=%u\n",
                                    sent_count,
                                    k_uptime_get_32(),
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        mesh_coordinator_decide_now("ch9-batch", &coordinator_decision);
        if (!coordinator_decision.mesh_work_allowed) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=coord sent=%u state=%s seq=%u q=%u\n",
                                    sent_count,
                                    app_mesh_coordinator_state_name(
                                        coordinator_decision.state),
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        now_ms = k_uptime_get_32();
        if (!mesh_outbound_ready_for_tx(queued, now_ms) ||
            !mesh_packet_prefers_channel9(&queued->packet)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=blocked sent=%u now=%u ready=%u ch9=%u seq=%u earliest=%u q=%u\n",
                                    sent_count,
                                    now_ms,
                                    mesh_outbound_ready_for_tx(queued, now_ms) ? 1u : 0u,
                                    mesh_packet_prefers_channel9(&queued->packet) ? 1u : 0u,
                                    queued->packet.seq,
                                    queued->earliest_tx_ms,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        ret = mesh_relay_select_next_hop(&mesh_runtime,
                                         queued->packet.dst_id,
                                         &queued_next_hop_id);
        if (ret != PROTO_OK || queued_next_hop_id != next_hop_id) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=next-hop sent=%u now=%u ret=%d next=0x%llx want=0x%llx seq=%u q=%u\n",
                                    sent_count,
                                    now_ms,
                                    ret,
                                    (unsigned long long)queued_next_hop_id,
                                    (unsigned long long)next_hop_id,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }

        *tx = *queued;
        mesh_outbound_refresh_age(tx, now_ms);
        tx->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        tx->next_hop_id = next_hop_id;
        if (sent_count == 0u) {
            tx->earliest_tx_ms = mesh_ch9_slot_send_start_ms(tx, &plan, now_ms);
        } else {
            tx->earliest_tx_ms = now_ms + MESH_CH9_TX_FRAME_GAP_MS;
        }
        send_start_ms = tx->earliest_tx_ms;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_TARGET now=%u start=%u txstart=%u offset=%u seq=%u\n",
                                now_ms,
                                plan.start_ms,
                                tx->earliest_tx_ms,
                                tx->earliest_tx_ms - plan.start_ms,
                                tx->packet.seq);
        }
        fits = slot_tx.active ?
               mesh_ch9_tx_fits_configured_slot(tx,
                                                &plan,
                                                now_ms,
                                                send_start_ms,
                                                &required_ms) :
               mesh_ch9_tx_fits_plan(tx, &plan, now_ms, &required_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_FIT sent=%u now=%u start=%u end=%u txstart=%u req=%u fit=%u seq=%u\n",
                                sent_count,
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                send_start_ms,
                                required_ms,
                                fits ? 1u : 0u,
                                tx->packet.seq);
        }
        if (!fits) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_SLOT_FULL\n");
                if (sent_count == 0u) {
                    status_debug_note("DBG_CH9_TX_BATCH_MISSED_SLOT\n");
                }
                status_debug_printf("DBG_CH9_TX_BATCH_FULL sent=%u now=%u end=%u req=%u seq=%u\n",
                                    sent_count,
                                    now_ms,
                                    plan.end_ms,
                                    required_ms,
                                    tx->packet.seq);
            }
            if (sent_count == 0u) {
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-fit");
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
            break;
        }

        if (!slot_tx.active) {
            ret = mesh_ch9_slot_tx_begin(&slot_tx);
            if (ret < 0) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_BATCH_CONFIG_FAIL\n");
                    status_debug_printf("DBG_CH9_TX_BATCH_CONFIG_FAIL ret=%d sent=%u seq=%u\n",
                                        ret,
                                        sent_count,
                                        tx->packet.seq);
                }
                k_mutex_unlock(&mesh_send_scratch_lock);
                return ret;
            }

            now_ms = k_uptime_get_32();
            if (!mesh_ch9_tx_fits_configured_slot(tx,
                                                  &plan,
                                                  now_ms,
                                                  send_start_ms,
                                                  &required_ms)) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_BATCH_SLOT_FULL\n");
                    status_debug_note("DBG_CH9_TX_BATCH_MISSED_SLOT\n");
                    status_debug_printf("DBG_CH9_TX_BATCH_FULL sent=%u now=%u end=%u req=%u seq=%u\n",
                                        sent_count,
                                        now_ms,
                                        plan.end_ms,
                                        required_ms,
                                        tx->packet.seq);
                }
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-configured-fit");
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
        }

        if (sent_count == 0u) {
            mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                               next_hop_id,
                               &plan,
                               "queued-ch9-batch");
        }
        ret = mesh_send_outbound_preconfigured_ch9_locked(tx,
                                                          "queued-ch9-batch",
                                                          &sent_frame_len);
        if (ret < 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &tx->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            send_failure_retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "queued-ch9-batch-send");
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_SEND_FAIL\n");
                status_debug_printf("DBG_CH9_TX_BATCH_FAIL ret=%d sent=%u seq=%u delay=%u\n",
                                    ret,
                                    sent_count,
                                    tx->packet.seq,
                                    send_failure_retry_delay_ms);
            }
            if (sent_count == 0u) {
                mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-send");
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                           "queued-ch9-batch-send");
                return -EALREADY;
            }
            break;
        }
        {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &tx->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                                &retry_key);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t done_ms = k_uptime_get_32();

            status_debug_printf("DBG_CH9_TX_BATCH_FRAME sent=%u done=%u start=%u end=%u rem=%u len=%u seq=%u\n",
                                sent_count + 1u,
                                done_ms,
                                plan.start_ms,
                                plan.end_ms,
                                uptime_ms_until_deadline(done_ms, plan.end_ms),
                                (unsigned int)sent_frame_len,
                                tx->packet.seq);
        }

        (void)report_tx_queue_discard_head_locked();
        mesh_relay_note_tx_sent(&mesh_runtime, tx, k_uptime_get_32());
        app_mesh_paused_delivery_note_sent(&mesh_paused_delivery, tx);
        if (mesh_ch9_tx_pending_track_sent(tx, deadline_ms)) {
            ack_wait_started = true;
        }
        if (app_mesh_ch9_core_ack_wait_active(
                &mesh_runtime.pending,
                mesh_relay_tx_active(&mesh_runtime))) {
            core_ack_wait_started = true;
        }
        sent_count++;
    }
    mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);

    if (send_failure_retry_delay_ms > 0u) {
        report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                   "queued-ch9-batch-partial-send");
    }

    if (sent_count == 0u) {
        return -EBUSY;
    }

    mesh_note_channel9_local_tx(next_hop_id, plan.start_ms);
    mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                       next_hop_id,
                       &plan,
                       "queued-ch9-batch");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_SENT sent=%u batch_ack=%u core_ack=%u deadline=%u\n",
                            sent_count,
                            ack_wait_started ? 1u : 0u,
                            core_ack_wait_started ? 1u : 0u,
                            deadline_ms);
    }
    if (ack_wait_started || core_ack_wait_started) {
        uint32_t ack_deadline_ms = ack_wait_started ?
            mesh_ch9_tx_pending.deadline_ms :
            mesh_runtime.pending.gateway_ack_deadline_ms;

        mesh_ch9_event_note_persistent_ack_wait(next_hop_id,
                                                ack_deadline_ms,
                                                "queued-ch9-batch");
        mesh_schedule_tx_timeout();
        mesh_schedule_uwb_rx(0u);
    } else {
        report_tx_schedule(0u);
    }
    return 0;
}
#endif

void report_tx_schedule(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms = k_uptime_get_32();

    if (report_tx_backoff_until_ms != 0u) {
        if (uptime_deadline_reached(now_ms, report_tx_backoff_until_ms)) {
            report_tx_backoff_until_ms = 0u;
        } else {
            uint32_t remaining_ms =
                uptime_ms_until_deadline(now_ms, report_tx_backoff_until_ms);

            if (delay_ms < remaining_ms) {
                delay_ms = remaining_ms;
            }
        }
    }
    (void)mesh_reschedule_delayable(&report_tx_work, delay_ms);
#else
    ARG_UNUSED(delay_ms);
#endif
}

static void report_tx_schedule_backoff(uint32_t delay_ms, const char *reason)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms;

    now_ms = k_uptime_get_32();
    report_tx_backoff_until_ms = now_ms + delay_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_REPORT_TX_BACKOFF reason=%s delay=%u until=%u\n",
                            reason == NULL ? "report-tx" : reason,
                            delay_ms,
                            report_tx_backoff_until_ms);
    }
    report_tx_schedule(delay_ms);
#else
    ARG_UNUSED(delay_ms);
    ARG_UNUSED(reason);
#endif
}

static void mesh_c5_flood_work_handler(struct k_work *work)
{
    struct mesh_outbound outbound;
    uint8_t purpose;
    const char *reason;
    bool response_priority;
    struct app_mesh_flood_result result = {0};
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (!mesh_c5_flood_deferred.valid) {
        return;
    }

    outbound = mesh_c5_flood_deferred.outbound;
    purpose = mesh_c5_flood_deferred.purpose;
    reason = mesh_c5_flood_deferred.reason;
    response_priority = mesh_c5_flood_deferred.response_priority;

    ret = mesh_send_c5_flood_now(&outbound,
                                 purpose,
                                 reason,
                                 true,
                                 response_priority,
                                 false,
                                 NULL,
                                 &result,
                                 NULL);
    if (result.sent_count == 0u && mesh_send_failure_retryable(ret) &&
        mesh_c5_flood_deferred.retry_count < MESH_C5_DEFERRED_MAX_RETRIES) {
        uint32_t retry_ms = mesh_c5_flood_deferred_retry_ms(&outbound);

        mesh_c5_flood_deferred.retry_count++;
        (void)mesh_reschedule_delayable(&mesh_c5_flood_work,
                                        retry_ms);
        return;
    }

    mesh_c5_flood_deferred.valid = false;
    if (ret == 0 && result.sent_count > 0u) {
        mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
    } else if (ret < 0) {
        LOG_ERR("deferred C5 flood exhausted: msg=0x%02x ret=%d attempts=%u age_ms=%u",
                outbound.packet.msg_type,
                ret,
                mesh_c5_flood_deferred.retry_count,
                k_uptime_get_32() - mesh_c5_flood_deferred.queued_at_ms);
    }
    app_mesh_rf_retry_reset(&mesh_c5_flood_deferred.rf_retry);
}

uint32_t report_tx_queue_used(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    return (uint32_t)k_msgq_num_used_get(&report_tx_msgq) +
           (report_tx_queue_recovery_valid ? 1u : 0u);
#else
    return 0u;
#endif
}

bool mesh_report_tx_backlog_active(void)
{
    return report_tx_queue_used() > 0u || mesh_ch9_tx_pending.active;
}

bool mesh_report_ch9_ack_wait_active(void)
{
    return mesh_ch9_tx_pending.active;
}

static int mesh_queue_anchor_cir_fragment(struct mesh_outbound *outbound,
                                          uint32_t *queue_depth)
{
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    outbound->queued_at_ms = k_uptime_get_32();
    mesh_attach_paused_delivery_loss(outbound, "queue-anchor-cir-fragment");
    mesh_reclaim_for_local_origin_priority(outbound,
                                           "local-origin-cir-ready",
                                           false);
    ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
    if (ret != 0) {
        return -ENOSPC;
    }
    if (queue_depth != NULL) {
        *queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
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
    struct mesh_outbound *outbound = &report_tx_worker_scratch;
    struct app_mesh_coordinator_decision coordinator_decision;
    bool anchor_busy;
    bool survey_busy;
    bool relay_tx_active;
    bool ch9_ack_wait_active;
    bool ch9_ack_send_pending;
    bool route_waiting_active;
    bool report_queue_pending;
    bool rx_queue_pending;
    bool route_handoff_active;
    bool report_policy_deferred = false;
    struct app_mesh_tx_handoff_result handoff_result;
    uint32_t now_ms;
    uint32_t channel9_wait_retry_delay_ms = 0u;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (report_tx_queue_recovery_valid &&
        k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT) == 0) {
        if (k_msgq_put(&report_tx_msgq,
                       &report_tx_queue_overflow_dropped,
                       K_NO_WAIT) == 0) {
            report_tx_queue_recovery_valid = false;
        }
        k_mutex_unlock(&report_tx_queue_overflow_lock);
    }
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (k_msgq_num_used_get(&report_tx_msgq) < 2u) {
        ret = app_mesh_report_encode_queue_next_cir();
        if (ret < 0 && ret != -ENOENT) {
            LOG_WRN("anchor partial CIR report refill failed: %d", ret);
        }
    }
#endif
    anchor_busy = anchor_uwb_window_active();
    survey_busy = mesh_report_anchor_survey_discovery_is_pending();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    ch9_ack_wait_active = mesh_ch9_tx_pending.active;
    ch9_ack_send_pending =
        app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table);
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    report_queue_pending = k_msgq_num_used_get(&report_tx_msgq) > 0;
    if (report_queue_pending &&
        k_msgq_peek(&report_tx_msgq, outbound) == 0 &&
        mesh_outbound_is_local_origin_priority(outbound)) {
        mesh_reclaim_for_local_origin_priority(outbound,
                                               "local-origin-report-ready",
                                               true);
        relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
        ch9_ack_wait_active = mesh_ch9_tx_pending.active;
        route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                               mesh_route_waiting_tx_active();
    }
    rx_queue_pending = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                        k_msgq_num_used_get(&mesh_rx_msgq) > 0u;
    route_handoff_active = mesh_route_reply_handoff_active();
    mesh_coordinator_decide_now("report-tx", &coordinator_decision);
    if (anchor_busy || !coordinator_decision.report_tx_allowed ||
        route_handoff_active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            report_queue_pending) {
            if (anchor_busy) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ANCHOR\n");
            } else if (rx_queue_pending) {
                status_debug_note("DBG_REPORT_WORK_BUSY_RX\n");
            } else if (route_handoff_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_HANDOFF\n");
            } else if (ch9_ack_send_pending) {
                status_debug_note("DBG_REPORT_WORK_BUSY_QACK\n");
            } else if (route_waiting_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ROUTE\n");
            } else if (ch9_ack_wait_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ACK\n");
            } else if (relay_tx_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_TX\n");
            } else {
                status_debug_note("DBG_REPORT_WORK_BUSY_SURVEY\n");
            }
            if (mesh_tx_handoff_gate_yields(
                    APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
                    report_queue_pending,
                    route_handoff_active,
                    rx_queue_pending,
                    &handoff_result)) {
                return;
            }
            if (ch9_ack_send_pending) {
                mesh_schedule_uwb_rx(0u);
            }
            report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        }
        return;
    }

    ret = mesh_try_send_report_tx_ch9_batch();
    if (ret == 0) {
        return;
    }
    if (ret == -EBUSY) {
        report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if (ret == -EALREADY) {
        return;
    }
    if (mesh_send_failure_retryable(ret) &&
        k_msgq_peek(&report_tx_msgq, outbound) == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
            &mesh_report_rf_retry_bank,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "report-batch-send");

        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms, "report-batch-send");
        return;
    }

    ret = k_msgq_peek(&report_tx_msgq, outbound);
    if (ret != 0) {
        report_tx_backoff_until_ms = 0u;
        return;
    }

    {
        uint16_t queue_depth = (uint16_t)k_msgq_num_used_get(&report_tx_msgq);
        uint16_t custody_depth = (uint16_t)(mesh_ch9_tx_pending.count +
            (mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u));

        if ((outbound->packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
            app_stack_workload_diag_click_sample(&outbound->packet,
                                                 queue_depth, custody_depth);
        }
        app_stack_workload_diag_cir_sample(&outbound->packet, queue_depth,
                                           custody_depth);
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(outbound, now_ms)) {
        report_tx_schedule(uptime_ms_until_deadline(now_ms, outbound->earliest_tx_ms));
        return;
    }

    {
        uint64_t selected_next_hop_id = 0u;
        int select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                    outbound->packet.dst_id,
                                                    &selected_next_hop_id);

        if (select_ret != PROTO_OK) {
            enum app_mesh_ch9_timeout_pressure_action pressure_action =
                app_mesh_ch9_timeout_pressure_decide(
                    outbound,
                    true,
                    mesh_runtime_has_transit_reservation(),
                    false,
                    DEVICE_ID);

            if (pressure_action == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
                (void)report_tx_queue_discard_head();
                HIGH_DEBUG_COUNTER_INC(mesh_drop);
                status_debug_printf("DBG_TRANSIT_QUEUE_DROP_NO_UPSTREAM src=0x%llx seq=%u type=0x%02x select=%d q=%u\n",
                                    (unsigned long long)outbound->packet.src_id,
                                    outbound->packet.seq,
                                    outbound->packet.msg_type,
                                    select_ret,
                                    report_tx_queue_used());
                if (report_tx_queue_used() > 0u) {
                    report_tx_schedule(0u);
                }
                return;
            }
            if (pressure_action == APP_MESH_CH9_TIMEOUT_DEFER_LOCAL) {
                status_debug_printf("DBG_LOCAL_QUEUE_DEFER_DOWNSTREAM seq=%u type=0x%02x delay=%u select=%d\n",
                                    outbound->packet.seq,
                                    outbound->packet.msg_type,
                                    ROUTE_PARENT_HOLDDOWN_MS,
                                    select_ret);
                report_tx_schedule_backoff(ROUTE_PARENT_HOLDDOWN_MS,
                                           "downstream-reservation");
                return;
            }
        }
    }

    ret = mesh_start_tracked_tx_with_retry(outbound,
                                           "queued-click-report",
                                           &channel9_wait_retry_delay_ms,
                                           app_mesh_route_wait_tx_may_store(
                                               APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
                                           NULL,
                                           NULL,
                                           &report_policy_deferred);
    if (ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

        app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                            &retry_key);
        report_tx_backoff_until_ms = 0u;
        (void)report_tx_queue_discard_head();
        return;
    }

    if (ret == -EAGAIN) {
        uint32_t retry_delay_ms = 0u;

        if (!report_tx_consume_retry_delay_override(&retry_delay_ms)) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &outbound->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "report-tx-transient");
        }

        LOG_WRN("queued gateway-bound report retrying after transient mesh TX failure");
        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms,
                                   "report-tx-transient");
        return;
    }

    if (ret == -EHOSTUNREACH &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_route_waiting_tx_active()) {
        status_debug_note("DBG_REPORT_QUEUE_TO_ROUTE_WAIT\n");
        (void)report_tx_queue_discard_head();
        return;
    }

    if (ret == -EBUSY && report_policy_deferred) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
            &mesh_report_rf_retry_bank,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "report-channel5-deferral");

        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms,
                                   "report-channel5-deferral");
        return;
    }

    if (ret == -EBUSY) {
        uint32_t delay_ms = channel9_wait_retry_delay_ms > 0u ?
                            channel9_wait_retry_delay_ms :
                            REPORT_TX_RETRY_DELAY_MS;

        status_debug_printf("DBG_REPORT_TX_WAIT_CH9_SLOT now=%u delay=%u\n",
                            k_uptime_get_32(),
                            delay_ms);
        report_tx_schedule(delay_ms);
        return;
    }

    if (ret == -EHOSTUNREACH) {
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)report_tx_queue_discard_head();
    {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

        app_mesh_rf_retry_bank_forget(&mesh_report_rf_retry_bank, &retry_key);
    }
    LOG_WRN("queued gateway-bound report dropped after permanent TX error: ret=%d", ret);
    if (mesh_delivery_health.permanent_report_failures < UINT32_MAX) {
        mesh_delivery_health.permanent_report_failures++;
    }
    mesh_delivery_health.last_permanent_error = (uint32_t)(-ret);
    mesh_delivery_health.last_permanent_session_id = outbound->packet.session_id;
    mesh_delivery_health.last_permanent_seq = outbound->packet.seq;
    mesh_delivery_health.last_permanent_msg_type = outbound->packet.msg_type;
    HIGH_DEBUG_COUNTER_INC(mesh_drop);
    if (report_tx_queue_used() > 0u) {
        report_tx_schedule(0u);
    }
}
#endif

int queue_anchor_report(const struct mesh_outbound *outbound)
{
    if (outbound == NULL) {
        return -EINVAL;
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound queued;
    int ret;

    queued = *outbound;
    queued.queued_at_ms = k_uptime_get_32();
    mesh_attach_paused_delivery_loss(&queued, "queue-anchor-report");
    mesh_reclaim_for_local_origin_priority(&queued,
                                           "local-origin-report-queued",
                                           false);

    ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
    if (ret != 0) {
        struct app_mesh_paused_delivery_store_result drop_result;
        int lock_ret;

        lock_ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT);
        if (lock_ret == 0) {
            bool queued_local_priority =
                mesh_outbound_is_local_origin_priority(&queued);

            if (report_tx_queue_recovery_valid) {
                k_mutex_unlock(&report_tx_queue_overflow_lock);
                return -ENOSPC;
            }
            ret = -ENOSPC;
            for (uint32_t i = 0u;
                 i < REPORT_TX_QUEUE_DEPTH &&
                 k_msgq_get(&report_tx_msgq,
                            &report_tx_queue_overflow_dropped,
                            K_NO_WAIT) == 0;
                 i++) {
                bool dropped_candidate = false;

                if (queued_local_priority) {
                    dropped_candidate =
                        !mesh_outbound_is_local_origin_priority(
                            &report_tx_queue_overflow_dropped);
                } else {
                    dropped_candidate =
                        !mesh_outbound_is_local_origin_priority(
                            &report_tx_queue_overflow_dropped);
                }
                if (dropped_candidate) {
                    ret = 0;
                    break;
                }
                (void)k_msgq_put(&report_tx_msgq,
                                 &report_tx_queue_overflow_dropped,
                                 K_NO_WAIT);
            }
            if (ret != 0 && queued_local_priority) {
                ret = k_msgq_get(&report_tx_msgq,
                                 &report_tx_queue_overflow_dropped,
                                 K_NO_WAIT);
            }
            if (ret == 0) {
                app_mesh_paused_delivery_note_drop(&mesh_paused_delivery,
                                                   &drop_result);
                mesh_attach_paused_delivery_loss(&queued,
                                                 "queue-anchor-report-replace");
                ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
                if (ret == 0) {
                    HIGH_DEBUG_COUNTER_INC(mesh_drop);
                    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                                         "phase=replace-oldest old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u queue_depth=%u",
                                         report_tx_queue_overflow_dropped.packet.msg_type,
                                         report_tx_queue_overflow_dropped.packet.seq,
                                         queued.packet.msg_type,
                                         queued.packet.seq,
                                         drop_result.lost_count,
                                         k_msgq_num_used_get(&report_tx_msgq));
                    LOG_WRN("anchor report queue full; replaced oldest report: old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u",
                            report_tx_queue_overflow_dropped.packet.msg_type,
                            report_tx_queue_overflow_dropped.packet.seq,
                            queued.packet.msg_type,
                            queued.packet.seq,
                            drop_result.lost_count);
                    k_mutex_unlock(&report_tx_queue_overflow_lock);
                    ret = 0;
                    goto queued;
                }
            }
            k_mutex_unlock(&report_tx_queue_overflow_lock);
        }

        app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_REPORT_QUEUE_FULL lost=%u lock=%d ret=%d\n",
                                drop_result.lost_count,
                                lock_ret,
                                ret);
        }
        LOG_WRN("anchor report queue full; gateway-bound report dropped: lost=%u",
                drop_result.lost_count);
        return -ENOSPC;
    }

queued:
    if ((queued.packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
        uint16_t queue_depth = (uint16_t)k_msgq_num_used_get(&report_tx_msgq);

        app_stack_workload_diag_click_admit(&queued.packet, queue_depth,
                                            queue_depth);
        /* Admission retains the new click beside any earlier report custody. */
        app_stack_workload_diag_click_sample(&queued.packet, queue_depth,
                                             queue_depth);
    }
    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                         "msg=0x%02x dst=0x%016llx seq=%u earliest_tx_ms=%u queue_depth=%u",
                         queued.packet.msg_type,
                         (unsigned long long)queued.packet.dst_id,
                         queued.packet.seq,
                         queued.earliest_tx_ms,
                         k_msgq_num_used_get(&report_tx_msgq));
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x earliest_tx_ms=%u queue_depth=%u",
            queued.packet.msg_type,
            queued.earliest_tx_ms,
            k_msgq_num_used_get(&report_tx_msgq));
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
    if (route_reply->earliest_tx_ms != 0u) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t delay_ms = uptime_ms_until_deadline(now_ms,
                                                     route_reply->earliest_tx_ms);

        if (delay_ms > 0u) {
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

    if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
        LOG_WRN("mesh route-request action expired before forwarding: src=0x%016llx target=0x%016llx session=%u",
                (unsigned long long)route_req->packet.src_id,
                (unsigned long long)target_id,
                route_req->packet.session_id);
        goto out;
    }
    route_reply_window_ms = uptime_ms_until_deadline(k_uptime_get_32(),
                                                     reply_deadline_ms);

    ret = mesh_try_direct_gateway_route_probe(target_id,
                                              "route-request-rebroadcast-probe",
                                              true,
                                              APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,
                                              0u);
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
                                     "route-request-rebroadcast");
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

        if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                k_uptime_get_32(),
                reply_deadline_ms);
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

        if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                k_uptime_get_32(),
                reply_deadline_ms);
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
    mesh_route_request_action_previous_hop_id = rx->previous_hop_id;
    mesh_route_request_action_reply_deadline_ms =
        defer_decision.reply_deadline_ms;
    mesh_route_request_action_pending = true;
    now_ms = k_uptime_get_32();
    delay_ms = app_mesh_route_request_defer_delay_ms(
        now_ms,
        mesh_route_request_action_tx.earliest_tx_ms);
    ret = mesh_reschedule_delayable(&mesh_route_request_action_work, delay_ms);
    if (ret < 0) {
        mesh_route_request_action_pending = false;
    }
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
    return ret >= 0;
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
        (void)mesh_reschedule_delayable(&mesh_route_request_action_work,
                                        delay_ms);
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

static bool mesh_forward_uses_gateway_batch_queue(const struct mesh_outbound *out)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           DEVICE_ROLE == ROLE_ANCHOR &&
           out != NULL &&
           out->packet.dst_id == GATEWAY_ID &&
           out->next_hop_id == GATEWAY_ID &&
           mesh_packet_prefers_channel9(&out->packet) &&
           (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
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

static bool mesh_parent_contact_failure_is_hard(int ret)
{
    return ret == -ETIMEDOUT || ret == -EHOSTUNREACH ||
           ret == -ENOTCONN || ret == -ECONNRESET;
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result,
                                       uint8_t received_radio_channel,
                                       const struct mesh_rx_pending *rx,
                                       const struct proto_packet *confirmed_packet)
{
    bool forward_sent = false;
    bool forward_queued_gateway_ack = false;
    bool child_custody_ready = true;
    struct app_mesh_result_handoff_ops handoff_ops = {
        .save_child_custody = mesh_handoff_save_child_custody,
        .note_result_bundle_forwarded = mesh_handoff_note_result_bundle_forwarded,
        .send_result_grant = mesh_handoff_send_result_grant,
        .note_tx_sent = mesh_handoff_note_tx_sent,
    };
    struct app_mesh_result_handoff_status handoff_status;

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound *gateway_ack = &mesh_result_action_tx;
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
        int ret;

        *gateway_ack = result->gateway_ack;
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
                mesh_ch9_ack_batch_queue(gateway_ack, rx);
                status_debug_printf("DBG_GATEWAY_BATCH_ACK_QUEUE batch=%u final=%u n=%u rxseq=%u ackseq=%u next=0x%llx\n",
                                    batch_metadata.batch_id,
                                    batch_metadata.final_packet ? 1u : 0u,
                                    mesh_ch9_ack_batch_count_for_peer(
                                        gateway_ack->next_hop_id),
                                    rx->packet.seq,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->next_hop_id);
                if (batch_metadata.final_packet) {
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
            }
        }
        if (DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            const uint64_t ack_deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;

            gateway_ack->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
            gateway_ack->earliest_tx_ms = 0u;
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
            }
            goto after_gateway_ack;
        }
        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_QUEUE_ROUTE_TEST_ACK) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            mesh_ch9_ack_batch_queue(gateway_ack, rx);
            goto after_gateway_ack;
        }

        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE == ROLE_GATEWAY &&
                received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
                status_debug_printf("DBG_GATEWAY_ACK_GUARD ms=%u seq=%u\n",
                                    MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                    gateway_ack->packet.seq);
                k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
            }
            ret = mesh_send_outbound_keep_channel9_awake(
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
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                app_mesh_rf_retry_note_success(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                mesh_note_channel9_local_tx(gateway_ack->next_hop_id, k_uptime_get_32());
                high_debug_log_event("GATEWAY_ACK_TX",
                                     "mode=current-channel9 dst=0x%016llx next=0x%016llx seq=%u",
                                     (unsigned long long)gateway_ack->packet.dst_id,
                                     (unsigned long long)gateway_ack->next_hop_id,
                                     gateway_ack->packet.seq);
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
                mesh_store_route_waiting_tx(gateway_ack);
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

                mesh_store_route_waiting_tx(gateway_ack);
                mesh_schedule_route_waiting_retry_after(
                    "gateway-ack-planned-channel9",
                    retry_delay_ms);
            }
        } else if (ack_decision.action ==
                   APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9) {
            LOG_WRN("gateway ACK deferred until channel-9 timing refresh: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack->next_hop_id,
                    ret);
            mesh_store_route_waiting_tx(gateway_ack);
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
            mesh_store_route_waiting_tx(gateway_ack);
            mesh_schedule_route_waiting_retry_after(retry_reason,
                                                    retry_delay_ms);
        }
    }
after_gateway_ack:
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
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
                "broadcast-forward");
        } else if (mesh_forward_uses_child_gateway_ack_queue(
                       &result->forward,
                       received_radio_channel)) {
            struct mesh_outbound *forward_ack = &mesh_result_action_tx;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_FORWARD_GATEWAY_ACK_MATCH\n");
            }
            *forward_ack = result->forward;
            forward_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_ch9_ack_batch_queue_forwarded_gateway_ack(
                forward_ack,
                "forward-gateway-ack");
            forward_queued_gateway_ack = ret == 0;
        } else if (mesh_forward_uses_gateway_batch_queue(&result->forward)) {
            ret = queue_anchor_report(&result->forward);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_FORWARD_GATEWAY_BATCH_QUEUE ret=%d seq=%u q=%u\n",
                                    ret,
                                    result->forward.packet.seq,
                                    report_tx_queue_used());
            }
        } else {
            ret = mesh_start_tracked_tx(&result->forward, "forward");
        }
        forward_sent = ret == 0 && !forward_queued_gateway_ack;
        app_mesh_result_handoff_after_forward(result,
                                              forward_sent,
                                              DEVICE_ROLE == ROLE_ANCHOR,
                                              &handoff_ops,
                                              &handoff_status);
        if (handoff_status.child_custody_save_failed) {
            LOG_WRN("mesh child custody snapshot update failed after forward");
        }
    }
    if (child_custody_ready &&
        (result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        app_mesh_result_handoff_prepare_hop_ack(result,
                                                forward_sent,
                                                DEVICE_ROLE == ROLE_ANCHOR,
                                                &handoff_ops,
                                                &handoff_status);
        child_custody_ready = handoff_status.child_custody_ready;
        if (handoff_status.child_custody_save_failed) {
            LOG_WRN("mesh hop ACK skipped: child custody snapshot unavailable");
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) &&
        handoff_status.hop_ack_allowed) {
        struct mesh_outbound *hop_ack = &mesh_result_action_tx;

        *hop_ack = result->hop_ack;
        if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            mesh_ch9_ack_batch_queue(hop_ack, rx);
        } else if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(hop_ack, "hop-ack") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, hop_ack, k_uptime_get_32());
            }
        } else if (mesh_send_outbound(hop_ack, "hop-ack") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, hop_ack, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) {
        struct mesh_outbound *route_reply_ack = &mesh_result_action_tx;
        bool active_exchange;
        int ret;

        *route_reply_ack = result->route_reply_ack;
        active_exchange = mesh_c5_contact_peer_active(route_reply_ack->next_hop_id,
                                                      k_uptime_get_32());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_TX_BEGIN next=0x%llx seq=%u active=%u\n",
                                (unsigned long long)route_reply_ack->next_hop_id,
                                route_reply_ack->packet.seq,
                                active_exchange ? 1u : 0u);
        }
        ret = mesh_send_c5_control(route_reply_ack,
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
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        (void)mesh_send_route_reply_action(result, "route-reply-action");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, false);
    }
    if (result->actions & MESH_RELAY_ACTION_UPDATE_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, true);
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) {
        int ret;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_BEGIN ttl=%u seq=%u earliest=%u\n",
                                result->gateway_route_adv.packet.ttl,
                                result->gateway_route_adv.packet.seq,
                                result->gateway_route_adv.earliest_tx_ms);
        }
        ret = mesh_send_c5_flood_response(&result->gateway_route_adv,
                                          C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                                          "gateway-route-adv-forward");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_DONE ret=%d seq=%u\n",
                                ret,
                                result->gateway_route_adv.packet.seq);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) {
        if (mesh_send_c5_control(&result->busy,
                                 C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                                 MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                 "relay-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_BUSY) {
        if (mesh_send_c5_control(&result->busy,
                                 C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
                                 MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                 "result-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (child_custody_ready &&
        (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT)) {
        app_mesh_result_handoff_result_grant(result,
                                             DEVICE_ROLE == ROLE_ANCHOR,
                                             &handoff_ops,
                                             &handoff_status);
        if (handoff_status.result_grant_suppressed) {
            LOG_WRN("mesh result grant skipped: child custody snapshot unavailable");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        struct mesh_outbound *retransmit = &mesh_result_action_tx;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
        uint32_t retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
        int ret = PROTO_OK;

        *retransmit = result->retransmit;
        if (mesh_packet_prefers_channel9(&retransmit->packet)) {
            struct mesh_channel5_requirements requirements;
            uint32_t now_ms = k_uptime_get_32();
            uint64_t debug_next_hop = 0u;
            int debug_select_ret;

            debug_select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                          retransmit->packet.dst_id,
                                                          &debug_next_hop);
            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_expire_channel9_timings(now_ms, "retransmit");
            ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                       retransmit->next_hop_id,
                                                       &requirements,
                                                       now_ms,
                                                       &plan);
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
                retransmit->radio_channel = MESH_EVENT_CHANNEL;
                channel9_replanned = true;
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
                if (mesh_channel9_connection_count() == 0u) {
                    if (debug_select_ret == PROTO_OK &&
                        mesh_id_is_unicast(debug_next_hop) &&
                        debug_next_hop != GATEWAY_ID) {
                        uint32_t failure_actions = MESH_RELAY_ACTION_NONE;
                        int failure_status = PROTO_OK;
                        int repair_ret;
                        int failure_ret = PROTO_OK;

                        mesh_route_ready_event_peer_id = debug_next_hop;
                        repair_ret = mesh_propose_event_after_channel5_contact(
                            debug_next_hop,
                            "retransmit-event-repair");
                        if (repair_ret == 0) {
                            mesh_route_ready_event_peer_id = 0u;
                            defer_ret = mesh_relay_note_retransmit_deferred(
                                &mesh_runtime,
                                retransmit,
                                k_uptime_get_32() + RELAY_BUSY_RETRY_MIN_MS);
                        } else if (mesh_parent_contact_failure_is_hard(
                                       repair_ret)) {
                            uint64_t downstream_peers[MESH_RELAY_EVENT_TIMINGS];
                            uint32_t failure_now_ms = k_uptime_get_32();
                            uint8_t downstream_peer_count =
                                mesh_capture_downstream_channel9_peers(
                                    downstream_peers,
                                    ARRAY_SIZE(downstream_peers));

                            failure_ret = mesh_relay_note_pending_parent_failure_status(
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
                            if (failure_ret == PROTO_OK &&
                                (failure_actions &
                                 MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
                                (void)mesh_schedule_route_request(
                                    retransmit->packet.dst_id,
                                    "retransmit-event-repair-failed");
                            } else if (failure_ret != PROTO_OK) {
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
                    } else {
                        (void)mesh_schedule_route_request(
                            retransmit->packet.dst_id,
                            "retransmit-channel9-refresh");
                    }
                }
            }
        }
        if (ret == PROTO_OK) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &retransmit->packet,
                    APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);
            bool backend_rf_started = false;
            bool backend_terminal = false;
            int backend_ret = app_node_comm_backend_retry_preflight(
                &retransmit->packet);
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
                send_ret = mesh_send_outbound_with_release(
                    retransmit,
                    "retransmit",
                    MESH_RADIO_RELEASE_STANDBY,
                    &backend_rf_started);
                account_ret = app_node_comm_complete_backend_attempt(
                    &retransmit->packet, backend_rf_started);

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
            }

            if (send_ret == 0) {
                app_mesh_rf_retry_note_success(&mesh_retransmit_rf_retry,
                                               &retry_key);
                HIGH_DEBUG_COUNTER_INC(mesh_retry);
                mesh_relay_note_tx_sent(&mesh_runtime,
                                        retransmit,
                                        k_uptime_get_32());
                if (channel9_replanned) {
                    mesh_note_channel9_local_tx(retransmit->next_hop_id,
                                                plan.start_ms);
                }
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
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);
        uint64_t route_ready_target_id = selected != NULL ? selected->gateway_id : GATEWAY_ID;
        uint64_t route_ready_next_hop_id = selected != NULL ? selected->next_hop_id : 0u;
        const struct app_mesh_route_ready_handoff_state route_ready_state = {
            .selected_route_valid = selected != NULL,
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = mesh_id_is_unicast(mesh_route_ready_event_peer_id),
            .selected_peer_id = route_ready_next_hop_id,
            .deferred_peer_id = mesh_route_ready_event_peer_id,
        };
        struct app_mesh_route_ready_handoff_result route_ready_result;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_READY\n");
        }
        app_mesh_test_note_route_ready(route_ready_target_id,
                                       route_ready_next_hop_id,
                                       result->status);
        app_mesh_route_ready_handoff_on_ready(&route_ready_state,
                                              &route_ready_result);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_READY_DECISION sel=%u next=0x%llx rxq=%u def=0x%llx now=%u drain=%u waittx=%u prop=%u clear=%u\n",
                                route_ready_state.selected_route_valid ? 1u : 0u,
                                (unsigned long long)route_ready_state.selected_peer_id,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                (unsigned long long)mesh_route_ready_event_peer_id,
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
            mesh_route_ready_event_peer_id = 0u;
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
                mesh_route_ready_event_peer_id = route_ready_result.peer_id;
            }
            LOG_INF("mesh route ready deferred until queued RX control frames drain");
            mesh_schedule_route_waiting_retry("route-ready-rx-drain");
        } else if (route_ready_result.try_waiting_tx) {
            mesh_schedule_route_waiting_retry_after("route-ready", 0u);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        int comm_ret = app_node_comm_note_gateway_confirmed(confirmed_packet);

        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        LOG_INF("mesh pending TX gateway acknowledged");
        if (comm_ret < 0 && comm_ret != -ENOENT && comm_ret != -ESTALE) {
            LOG_WRN("node communication gateway confirmation rejected: ret=%d",
                    comm_ret);
        }
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed != NULL) {
            mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed(
                confirmed_packet);
        }
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) {
        LOG_INF("mesh pending TX hop progress acknowledged");
        (void)mesh_save_outbox_durable("hop-progress");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RELAY_BUSY) {
        LOG_INF("mesh pending TX deferred by relay busy hint");
        (void)mesh_save_outbox_durable("relay-busy");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_RETRY) {
        LOG_INF("mesh pending collection result scheduled for EACK retry");
        (void)mesh_save_outbox_durable("collection-retry");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_CLOSED) {
        LOG_INF("mesh pending collection result stopped after collection close");
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL) {
        enum node_comm_terminal_reason reason =
            result->status == MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED ?
                NODE_COMM_TERMINAL_DEADLINE_EXPIRED :
                result->status ==
                    MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED ?
                NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED :
                NODE_COMM_TERMINAL_PERMANENT_FAILURE;
        int comm_ret = app_node_comm_note_gateway_failed(
            &result->terminal.packet, reason);

        LOG_WRN("mesh result grant wait ended: status=%d reason=%u src=0x%016llx session=%u seq=%u",
                result->status,
                (unsigned int)reason,
                (unsigned long long)result->terminal.packet.src_id,
                result->terminal.packet.session_id,
                result->terminal.packet.seq);
        if (comm_ret < 0 && comm_ret != -ENOENT &&
            comm_ret != -EALREADY) {
            LOG_WRN("node communication result terminal rejected: ret=%d status=%d",
                    comm_ret, result->status);
        }
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_survey_delivery_transport_released !=
                NULL) {
            mesh_report_callbacks->anchor_survey_delivery_transport_released(
                &result->terminal.packet, false);
        }
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (mesh_relay_result_bundle_pending(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }
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
        bool handled_event_control = false;
        bool gateway_semantic_delivery_processed = false;
        struct proto_packet confirmed_packet = {0};
        bool confirmed_packet_valid = mesh_relay_tx_active(&mesh_runtime);

        if (confirmed_packet_valid) {
            confirmed_packet = mesh_runtime.pending.packet;
        }

        memset(result, 0, sizeof(*result));
        mesh_rx_pending_refresh_age(pending, now_ms);
        /* A peer TX slot can carry ACKs, telemetry, or control; ACK matching is opportunistic. */
        (void)mesh_ch9_tx_pending_handle_ack(&pending->packet,
                                             pending->payload,
                                             pending->payload_len,
                                             pending->previous_hop_id);
        if (mesh_gateway_route_test_should_reject_route_request(pending)) {
            continue;
        }
        ret = mesh_relay_handle_rx_with_random(&mesh_runtime,
                                               &pending->packet,
                                               pending->payload,
                                               pending->payload_len,
                                               pending->previous_hop_id,
                                               pending->link_quality,
                                               now_ms,
                                               sys_rand32_get(),
                                               result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            continue;
        }

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
                pending->packet.msg_type,
                (unsigned long long)pending->packet.src_id,
                (unsigned long long)pending->packet.dst_id,
                (unsigned long long)pending->previous_hop_id,
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
            (void)mesh_schedule_route_request(pending->packet.dst_id,
                                              "rx-forward-miss");
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_packet_is_event_control_type(pending->packet.msg_type)) {
            handled_event_control = mesh_handle_event_control(&pending->packet,
                                                              pending->payload,
                                                              pending->payload_len,
                                                              pending->previous_hop_id,
                                                              pending->received_at_ms);
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_gateway_delivery_requires_semantic_acceptance(
                &pending->packet)) {
            int reservation_ret;
            int semantic_ret;

            /*
             * Reserve downstream host-output custody before mutating protocol
             * state.  A full BLE queue therefore leaves the radio packet
             * unaccepted and retryable instead of ACKing data that the host
             * can never observe.
             */
            mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
            gateway_semantic_delivery_processed = true;
            reservation_ret = gateway_ble_reserve_stream_packet(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->received_at_ms);
            if (reservation_ret != 1) {
                LOG_WRN("gateway semantic delivery deferred for BLE custody: msg=0x%02x src=0x%016llx ret=%d",
                        pending->packet.msg_type,
                        (unsigned long long)pending->packet.src_id,
                        reservation_ret);
            } else {
                semantic_ret = mesh_gateway_accept_semantic_delivery(pending);
                if (semantic_ret < 0) {
                    gateway_ble_cancel_stream_reservation();
                    LOG_WRN("gateway semantic delivery rejected without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                } else if (semantic_ret ==
                           APP_GATEWAY_SEMANTIC_ACCEPT_NEW) {
                    ret = gateway_ble_commit_stream_reservation(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    if (ret != 1) {
                        gateway_ble_cancel_stream_reservation();
                        LOG_ERR("gateway semantic BLE custody commit failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                        continue;
                    }
                    app_mesh_test_note_gateway_delivery(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->received_at_ms,
                        k_msgq_num_used_get(&mesh_rx_msgq));
                } else {
                    gateway_ble_cancel_stream_reservation();
                }

                ret = gateway_finalize_semantic_delivery(
                    &pending->packet,
                    pending->payload,
                    pending->payload_len,
                    pending->previous_hop_id,
                    pending->radio_channel,
                    pending->current_channel9_plan_valid ?
                    &pending->current_channel9_plan : NULL,
                    semantic_ret);
                if (ret < 0) {
                    LOG_WRN("gateway semantic post-custody finalization failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            ret);
                    continue;
                }
                ret = mesh_relay_commit_gateway_delivery(
                    &mesh_runtime,
                    &pending->packet,
                    pending->payload,
                    pending->payload_len,
                    pending->previous_hop_id,
                    k_uptime_get_32(),
                    result);
                if (ret != PROTO_OK) {
                    LOG_WRN("gateway semantic delivery commit failed: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            ret);
                }
            }
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            pending->packet.msg_type == MSG_GATEWAY_ACK) {
            if ((result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) != 0u) {
                status_debug_note("DBG_LEGACY_ACK_CONFIRMED\n");
            } else if (mesh_relay_tx_active(&mesh_runtime)) {
                status_debug_note("DBG_LEGACY_ACK_NO_MATCH\n");
            } else {
                status_debug_note("DBG_LEGACY_ACK_IDLE\n");
            }
        }
        mesh_handle_result_actions(result, pending->radio_channel, pending,
                                   confirmed_packet_valid ?
                                   &confirmed_packet : NULL);
        if (handled_event_control) {
            continue;
        }
        /*
         * Forwarding a broadcast can take long enough to materially advance
         * its protocol age. Keep the delivery timestamp and message age at
         * the same instant so local timing reconstruction does not restart
         * the survey after a relay operation.
         */
        mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY &&
            !gateway_semantic_delivery_processed) {
            if (pending->packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending->packet,
                                            pending->payload,
                                            pending->payload_len,
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
            mesh_report_gateway_handle_survey_discovery_report(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len,
                                                   pending->previous_hop_id,
                                                   pending->radio_channel,
                                                   pending->link_quality);
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
        } else if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                   DEVICE_ROLE == ROLE_ANCHOR) {
            mesh_report_anchor_handle_local_command(&pending->packet,
                                                    pending->payload,
                                                    pending->payload_len);
            mesh_report_anchor_handle_survey_discovery_start(&pending->packet,
                                                 pending->payload,
                                                 pending->payload_len);
            mesh_report_anchor_handle_survey_pair_prepare(&pending->packet,
                                              pending->payload,
                                                    pending->payload_len);
        }
        handled_count++;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_EXIT owner=%s handled=%u q=%u\n",
                            owner == NULL ? "rx" : owner,
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    return handled_count;
}
