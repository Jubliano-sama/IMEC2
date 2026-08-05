#line 12150 __BASE_FILE__
#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_semantic_commit_owner_identity_matches(
    const struct gateway_semantic_commit_owner *owner,
    const struct proto_packet *packet,
    size_t payload_len,
    const uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return owner != NULL && owner->active && packet != NULL &&
           packet->payload_len == payload_len &&
           owner->msg_type == packet->msg_type &&
           owner->flags == packet->flags &&
           owner->src_id == packet->src_id &&
           owner->dst_id == packet->dst_id &&
           owner->session_id == packet->session_id &&
           owner->seq == packet->seq &&
           owner->payload_len == payload_len &&
           semantic_digest_equal(owner->payload_digest,
                                 payload_digest,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}
#endif

static bool gateway_semantic_commit_owner_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    k_spinlock_key_t key;
    bool matches;

    if (packet == NULL || payload == NULL ||
        packet->payload_len != payload_len || payload_len == 0u ||
        payload_len > UINT16_MAX) {
        return false;
    }
    if (!semantic_digest_sha256(payload, payload_len, payload_digest)) {
        return false;
    }
    key = k_spin_lock(&gateway_semantic_commit_owner_lock);
    matches = gateway_semantic_commit_owner_identity_matches(
        &gateway_semantic_commit_owner,
        packet,
        payload_len,
        payload_digest);
    k_spin_unlock(&gateway_semantic_commit_owner_lock, key);
    return matches;
#else
    (void)packet;
    (void)payload;
    (void)payload_len;
    return false;
#endif
}

static void gateway_semantic_commit_owner_set(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_semantic_commit_owner next = {0};
    k_spinlock_key_t key;

    if (packet == NULL || payload == NULL ||
        packet->payload_len != payload_len || payload_len == 0u ||
        payload_len > UINT16_MAX) {
        mesh_gateway_semantic_commit_owner_clear();
        return;
    }
    next.msg_type = packet->msg_type;
    next.flags = packet->flags;
    next.src_id = packet->src_id;
    next.dst_id = packet->dst_id;
    next.session_id = packet->session_id;
    next.seq = packet->seq;
    next.payload_len = (uint16_t)payload_len;
    if (!semantic_digest_sha256(payload,
                                payload_len,
                                next.payload_digest)) {
        mesh_gateway_semantic_commit_owner_clear();
        return;
    }
    next.active = true;

    key = k_spin_lock(&gateway_semantic_commit_owner_lock);
    gateway_semantic_commit_owner = next;
    k_spin_unlock(&gateway_semantic_commit_owner_lock, key);
#else
    (void)packet;
    (void)payload;
    (void)payload_len;
#endif
}

static void gateway_semantic_commit_owner_clear_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    k_spinlock_key_t key;

    if (packet == NULL || payload == NULL ||
        packet->payload_len != payload_len || payload_len == 0u ||
        payload_len > UINT16_MAX) {
        return;
    }
    if (!semantic_digest_sha256(payload, payload_len, payload_digest)) {
        return;
    }
    key = k_spin_lock(&gateway_semantic_commit_owner_lock);
    if (gateway_semantic_commit_owner_identity_matches(
            &gateway_semantic_commit_owner,
            packet,
            payload_len,
            payload_digest)) {
        memset(&gateway_semantic_commit_owner,
               0,
               sizeof(gateway_semantic_commit_owner));
    }
    k_spin_unlock(&gateway_semantic_commit_owner_lock, key);
#else
    (void)packet;
    (void)payload;
    (void)payload_len;
#endif
}

void mesh_gateway_semantic_commit_owner_clear(void)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    k_spinlock_key_t key =
        k_spin_lock(&gateway_semantic_commit_owner_lock);

    memset(&gateway_semantic_commit_owner,
           0,
           sizeof(gateway_semantic_commit_owner));
    k_spin_unlock(&gateway_semantic_commit_owner_lock, key);
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_try_send_report_tx_ch9_batch(void)
{
    struct mesh_outbound *first = &report_tx_worker_scratch;
    struct mesh_outbound *queued = &report_tx_batch_candidates[0];
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
    int release_ret;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        !mesh_ch9_tx_pending_can_start()) {
        return -ENOTSUP;
    }

    ret = report_tx_queue_peek(first);
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
        struct app_mesh_queue_head_token head_token;
        uint64_t queued_next_hop_id = 0u;
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;
        size_t sent_frame_len = 0u;
        bool fits;

        if (report_tx_queue_begin_head(queued, &head_token) != 0) {
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
            (void)report_tx_queue_abort_head(&head_token);
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
            (void)report_tx_queue_abort_head(&head_token);
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
            (void)report_tx_queue_abort_head(&head_token);
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
        tx->earliest_tx_valid = true;
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
                (void)report_tx_queue_abort_head(&head_token);
                (void)mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
            (void)report_tx_queue_abort_head(&head_token);
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
                (void)report_tx_queue_abort_head(&head_token);
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
                (void)report_tx_queue_abort_head(&head_token);
                release_ret = mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return release_ret < 0 ? release_ret : -EALREADY;
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
                (void)report_tx_queue_abort_head(&head_token);
                release_ret = mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                           "queued-ch9-batch-send");
                return release_ret < 0 ? release_ret : -EALREADY;
            }
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }
        slot_tx.functional_tx_completed = true;
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

        ret = report_tx_queue_commit_head_locked(&head_token, queued);
        if (ret != 0) {
            LOG_ERR("report queue batch commit failed: %d", ret);
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }
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
    release_ret = mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);

    if (send_failure_retry_delay_ms > 0u) {
        report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                   "queued-ch9-batch-partial-send");
    }

    if (sent_count == 0u) {
        return release_ret < 0 ? release_ret : -EBUSY;
    }
    if (release_ret < 0) {
        return release_ret;
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
static void report_tx_schedule_backoff(uint32_t delay_ms, const char *reason)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms;

    now_ms = k_uptime_get_32();
    report_tx_backoff_until_ms = now_ms + delay_ms;
    report_tx_backoff_active = true;
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
    struct app_mesh_flood_result result = {0};
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        /*
         * The transport can pause after a caller decides to defer a flood but
         * before this worker runs.  Transport resume normally submits the
         * worker, but it may have completed just before the deferred record was
         * stored.  Keep an independent delayed owner so that race cannot strand
         * a gateway command indefinitely.
         */
        k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
        current_generation = mesh_c5_flood_deferred.valid ||
                             mesh_route_adv_deferred.valid;
        k_mutex_unlock(&mesh_c5_flood_deferred_lock);
        if (current_generation) {
            (void)mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                             0u,
                                             "paused-c5-flood");
        }
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
    generation = entry->generation;
    outbound = entry->outbound;
    purpose = entry->purpose;
    reason = entry->reason;
    response_priority = entry->response_priority;
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);

    ret = mesh_send_c5_flood_now(&outbound,
                                 purpose,
                                 reason,
                                 true,
                                 response_priority,
                                 false,
                                 NULL,
                                 &result,
                                 NULL);
    if (result.sent_count == 0u && mesh_send_failure_retryable(ret)) {
        uint32_t retry_ms;

        k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
        entry = route_adv_lane ? &mesh_route_adv_deferred :
                                 &mesh_c5_flood_deferred;
        current_generation = entry->valid &&
                             entry->generation == generation;
        if (current_generation &&
            entry->retry_count < MESH_C5_DEFERRED_MAX_RETRIES) {
            retry_ms = mesh_c5_flood_deferred_retry_ms(
                &outbound, &entry->rf_retry);
            entry->retry_count++;
            (void)mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                             retry_ms,
                                             "c5-flood-retry");
            k_mutex_unlock(&mesh_c5_flood_deferred_lock);
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
        (void)mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                         0u,
                                         "c5-flood-next-lane");
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
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    const struct proto_packet *packet)
{
    return control != NULL && packet != NULL &&
           fragment_index < control->fragment_count &&
           packet->msg_type == MSG_CLICK_REPORT &&
           (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           packet->src_id == control->anchor_id &&
           packet->dst_id == control->gateway_id &&
           packet->session_id == control->event_seq &&
           packet->seq == control->fragments[fragment_index].seq;
}

static int anchor_range_report_active_owner_matches(
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    const struct mesh_outbound *restored)
{
    bool matches = false;
    int ret;

    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return 0;
    }
    if (anchor_range_report_packet_identity_matches(
            control, fragment_index, &mesh_runtime.pending.packet)) {
        return 1;
    }
    if (restored == NULL ||
        mesh_runtime.pending.packet.msg_type != MSG_GATEWAY_ACK_CONFIRM) {
        return 0;
    }

    ret = mesh_gateway_ack_confirm_matches_packet(
        &mesh_runtime.pending.packet,
        mesh_runtime.pending.payload,
        mesh_runtime.pending.payload_len,
        &restored->packet,
        restored->payload,
        restored->payload_len,
        &matches);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    return matches ? 1 : 0;
}

/*
 * Resolve a producer callback against the exact immutable fragment stored by
 * the range journal. Packet identity alone is insufficient because a wrapped
 * session/sequence can name different bytes; ACK_CONFIRM carries the
 * original semantic digest specifically to close that collision.
 */
static int anchor_range_report_fragment_digest_match_locked(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint8_t *fragment_index)
{
    struct mesh_outbound restored;
    uint8_t restored_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (packet == NULL || semantic_digest == NULL ||
        fragment_index == NULL) {
        return -EINVAL;
    }
    for (uint8_t i = 0u;
         i < anchor_range_report_journal_runtime.control.fragment_count;
         i++) {
        if (!anchor_range_report_packet_identity_matches(
                &anchor_range_report_journal_runtime.control, i, packet)) {
            continue;
        }
        ret = app_mesh_persistence_restore_anchor_range_fragment(
            &anchor_range_report_journal_runtime.control, i, &restored);
        if (ret < 0) {
            return ret;
        }
        if (restored.packet.msg_type != packet->msg_type ||
            restored.packet.flags != packet->flags ||
            restored.packet.src_id != packet->src_id ||
            restored.packet.dst_id != packet->dst_id ||
            restored.packet.session_id != packet->session_id ||
            restored.packet.seq != packet->seq ||
            restored.packet.payload_len != packet->payload_len ||
            !mesh_packet_semantic_digest(&restored.packet,
                                         restored.payload,
                                         restored.payload_len,
                                         restored_digest) ||
            !semantic_digest_equal(restored_digest,
                                   semantic_digest,
                                   sizeof(restored_digest))) {
            return -EBADMSG;
        }
        *fragment_index = i;
        return 1;
    }
    return 0;
}
#endif

int mesh_range_report_batch_reserve(uint64_t clicker_id,
                                    uint32_t event_seq,
                                    uint8_t attempt_index)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct anchor_range_journal_control journal;
    uint32_t queue_count;
    int ret;

    if (clicker_id == 0u || event_seq == 0u) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_matches_locked(clicker_id,
                                                 event_seq,
                                                 attempt_index)) {
        ret = anchor_range_report_batch_reservation.queued_fragment_count == 0u ?
              0 : -EALREADY;
        goto out;
    }
    if (anchor_range_report_batch_reservation.active) {
        ret = -EBUSY;
        goto out;
    }
    if (anchor_range_report_journal_runtime.active) {
        uint16_t all_acknowledged =
            anchor_range_report_all_acknowledged_mask(
                anchor_range_report_journal_runtime.control.fragment_count);

        if (all_acknowledged == 0u ||
            anchor_range_report_journal_runtime.acknowledged_mask !=
                all_acknowledged) {
            ret = -EBUSY;
            goto out;
        }
        ret = app_mesh_persistence_clear_anchor_range_journal(
            &anchor_range_report_journal_runtime.control);
        if (ret < 0) {
            goto out;
        }
        memset(&anchor_range_report_journal_runtime, 0,
               sizeof(anchor_range_report_journal_runtime));
    }
    if (report_tx_queue_recovery_valid &&
        mesh_outbound_is_local_origin_priority(
            &report_tx_queue_overflow_dropped)) {
        ret = -EAGAIN;
        goto out;
    }
    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    if (!report_tx_queue_recovery_valid &&
        !app_mesh_queue_head_owned(&report_tx_queue_head_owner) &&
        queue_count == 1u &&
        k_msgq_num_free_get(&report_tx_msgq) + 1u >=
            RANGE_REPORT_MAX_PACKET_FRAGMENTS &&
        k_msgq_get(&report_tx_msgq,
                   &report_tx_queue_overflow_dropped,
                   K_NO_WAIT) == 0) {
        if (mesh_outbound_is_local_origin_priority(
                &report_tx_queue_overflow_dropped)) {
            (void)k_msgq_put(&report_tx_msgq,
                             &report_tx_queue_overflow_dropped,
                             K_NO_WAIT);
        } else {
            report_tx_queue_recovery_valid = true;
            queue_count = 0u;
        }
    }
    if (app_mesh_queue_head_owned(&report_tx_queue_head_owner) ||
        k_msgq_num_free_get(&report_tx_msgq) <
            RANGE_REPORT_MAX_PACKET_FRAGMENTS) {
        ret = -EAGAIN;
        goto out;
    }
    ret = app_mesh_persistence_prepare_anchor_range_journal(
        clicker_id,
        event_seq,
        attempt_index,
        DEVICE_ID,
        GATEWAY_ID,
        &journal);
    if (ret < 0) {
        goto out;
    }
    anchor_range_report_batch_reservation =
        (struct anchor_range_report_batch_reservation) {
            .journal = journal,
            .clicker_id = clicker_id,
            .event_seq = event_seq,
            .queue_count_before_batch = queue_count,
            .attempt_index = attempt_index,
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
    return 0;
#endif
}

void mesh_range_report_batch_abort(uint64_t clicker_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t queue_count;
    uint32_t expected_count;
    uint32_t queue_depth;
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
    if (anchor_range_report_batch_reservation.persistence_fail_closed ||
        anchor_range_report_batch_reservation.fragment_pending) {
        /*
         * The scratch record is the only exact owner after an uncertain
         * fragment write/readback. Keep it immutable until the watchdog
         * resets the process; aborting here would turn uncertainty into
         * certain data loss.
         */
        LOG_ERR("range report batch abort preserved fail-closed fragment: event=%u fragment=%u error=%d",
                event_seq,
                anchor_range_report_batch_reservation
                    .queued_fragment_count,
                anchor_range_report_batch_reservation.persistence_error);
        app_watchdog_stop_feeding();
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }
    if (anchor_range_report_batch_reservation.final_fragment_staged) {
        /*
         * Every exact fragment is already staged and invisible. Preserve
         * that complete batch while the control-record commit/readback is
         * retried; discarding it here would strand a possibly committed NVS
         * owner until reset.
         */
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }

    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    expected_count =
        anchor_range_report_batch_reservation.queue_count_before_batch +
        anchor_range_report_batch_reservation.queued_fragment_count;
    if (app_mesh_queue_head_owned(&report_tx_queue_head_owner) ||
        queue_count != expected_count) {
        LOG_ERR("range report batch queue ownership corrupted: queued=%u expected=%u recovery=%u head=%u",
                queue_count,
                expected_count,
                report_tx_queue_recovery_valid ? 1u : 0u,
                app_mesh_queue_head_owned(&report_tx_queue_head_owner) ? 1u : 0u);
        memset(&anchor_range_report_batch_reservation, 0,
               sizeof(anchor_range_report_batch_reservation));
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return;
    }

    for (uint32_t i = 0u;
         i < anchor_range_report_batch_reservation.queue_count_before_batch;
         i++) {
        ret = k_msgq_get(&report_tx_msgq,
                         &report_tx_queue_rotation_scratch,
                         K_NO_WAIT);
        if (ret != 0 ||
            k_msgq_put(&report_tx_msgq,
                       &report_tx_queue_rotation_scratch,
                       K_NO_WAIT) != 0) {
            LOG_ERR("range report batch abort could not preserve prior queue item");
            memset(&anchor_range_report_batch_reservation, 0,
                   sizeof(anchor_range_report_batch_reservation));
            k_mutex_unlock(&report_tx_queue_overflow_lock);
            return;
        }
    }
    for (uint8_t i = 0u;
         i < anchor_range_report_batch_reservation.queued_fragment_count;
         i++) {
        if (k_msgq_get(&report_tx_msgq,
                       &report_tx_queue_rotation_scratch,
                       K_NO_WAIT) != 0) {
            LOG_ERR("range report batch abort could not remove fragment %u", i);
            break;
        }
        queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
        app_stack_workload_diag_click_release(
            &report_tx_queue_rotation_scratch.packet, 0,
            (uint16_t)queue_depth, 0u);
        app_stack_workload_diag_cir_release(
            &report_tx_queue_rotation_scratch.packet, 0,
            (uint16_t)queue_depth, 0u);
    }
    memset(&anchor_range_report_batch_reservation, 0,
           sizeof(anchor_range_report_batch_reservation));
    /*
     * Reservation may have moved the only pre-existing transit record into
     * the explicit recovery owner.  Restore it before deciding whether work
     * remains, including the zero-fragment work-submit failure path.
     */
    (void)report_tx_queue_restore_recovery_locked();
    (void)report_tx_queue_promote_local_locked();
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
    bool final_fragment,
    int64_t persistence_deadline_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct anchor_range_fragment_policy persistence_policy;
    enum anchor_range_fragment_persistence_observation observation;
    enum anchor_range_fragment_policy_action policy_action;
    int64_t now_ms;
    int64_t remaining_ms;
    uint32_t queue_depth;
    uint32_t retry_delay_ms;
    int save_ret;
    int ret;

    if (outbound == NULL || clicker_id == 0u || event_seq == 0u ||
        persistence_deadline_ms <= 0) {
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
            RANGE_REPORT_MAX_PACKET_FRAGMENTS) {
        ret = -ESTALE;
        goto out;
    }
    /*
     * This scratch is owned by the same queue mutex for abort rotation and
     * admission. Reusing it avoids nesting another 1 KiB mesh_outbound on the
     * range-builder workqueue stack.
     */
    report_tx_queue_rotation_scratch = *outbound;
    mesh_outbound_refresh_age(&report_tx_queue_rotation_scratch,
                              k_uptime_get_32());
    mesh_attach_paused_delivery_loss(
        &report_tx_queue_rotation_scratch, "queue-range-report-batch");
    ret = anchor_range_fragment_policy_begin(&persistence_policy,
                                             persistence_deadline_ms);
    if (ret < 0) {
        goto out;
    }
    anchor_range_report_batch_reservation.fragment_pending = true;

    for (;;) {
        now_ms = k_uptime_get();
        observation =
            ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE;
        if (now_ms >= persistence_deadline_ms) {
            save_ret = -ETIMEDOUT;
        } else {
            save_ret = app_mesh_persistence_save_anchor_range_fragment(
                &anchor_range_report_batch_reservation.journal,
                anchor_range_report_batch_reservation.queued_fragment_count,
                &report_tx_queue_rotation_scratch,
                &observation);
            if ((save_ret == 0) !=
                (observation ==
                 ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED)) {
                save_ret = -EIO;
                observation =
                    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS;
            }
        }
        now_ms = k_uptime_get();
        policy_action = anchor_range_fragment_policy_observe(
            &persistence_policy, observation, now_ms);
        if (policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE) {
            break;
        }
        if (policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY) {
            remaining_ms = persistence_deadline_ms - now_ms;
            retry_delay_ms = (uint32_t)MIN(
                remaining_ms,
                (int64_t)ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRY_MS);
            k_sleep(K_MSEC(MAX(retry_delay_ms, 1u)));
            continue;
        }

        if (observation ==
            ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE) {
            ret = -ETIMEDOUT;
        } else {
            ret = save_ret < 0 ? save_ret : -EIO;
        }
        anchor_range_report_batch_reservation.persistence_error = ret;
        anchor_range_report_batch_reservation.persistence_fail_closed = true;
        LOG_ERR("anchor range fragment persistence failed closed: event=%u fragment=%u outcome=%u ret=%d",
                event_seq,
                anchor_range_report_batch_reservation
                    .queued_fragment_count,
                (unsigned int)observation,
                ret);
        app_watchdog_stop_feeding();
        goto out;
    }
    ret = k_msgq_put(&report_tx_msgq,
                     &report_tx_queue_rotation_scratch,
                     K_NO_WAIT);
    if (ret != 0) {
        ret = -EIO;
        anchor_range_report_batch_reservation.persistence_error = ret;
        anchor_range_report_batch_reservation.persistence_fail_closed = true;
        LOG_ERR("anchor range fragment durable but queue visibility failed closed: event=%u fragment=%u",
                event_seq,
                anchor_range_report_batch_reservation
                    .queued_fragment_count);
        app_watchdog_stop_feeding();
        goto out;
    }
    anchor_range_report_batch_reservation.fragment_pending = false;
    anchor_range_report_batch_reservation.queued_fragment_count++;
    queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    if (final_fragment) {
        anchor_range_report_batch_reservation.final_fragment_staged = true;
        ret = anchor_range_fragment_policy_begin(&persistence_policy,
                                                 persistence_deadline_ms);
        if (ret < 0) {
            anchor_range_report_batch_reservation.persistence_error = ret;
            anchor_range_report_batch_reservation.persistence_fail_closed = true;
            app_watchdog_stop_feeding();
            goto out;
        }
        for (;;) {
            now_ms = k_uptime_get();
            observation =
                ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT;
            if (now_ms >= persistence_deadline_ms) {
                save_ret = -ETIMEDOUT;
            } else {
                save_ret = app_mesh_persistence_commit_anchor_range_journal(
                    &anchor_range_report_batch_reservation.journal);
                if (save_ret == 0) {
                    observation =
                        ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED;
                } else if (save_ret == -EINVAL ||
                           save_ret == -ENOTSUP ||
                           save_ret == -EBADMSG ||
                           save_ret == -ESTALE) {
                    observation =
                        ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
                }
            }
            now_ms = k_uptime_get();
            policy_action = anchor_range_fragment_policy_observe(
                &persistence_policy, observation, now_ms);
            if (policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE) {
                break;
            }
            if (policy_action == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY) {
                remaining_ms = persistence_deadline_ms - now_ms;
                retry_delay_ms = (uint32_t)MIN(
                    remaining_ms,
                    (int64_t)ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRY_MS);
                k_sleep(K_MSEC(MAX(retry_delay_ms, 1u)));
                continue;
            }

            if (observation ==
                    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED &&
                save_ret < 0) {
                ret = save_ret;
            } else {
                ret = -ETIMEDOUT;
            }
            anchor_range_report_batch_reservation.persistence_error = ret;
            anchor_range_report_batch_reservation.persistence_fail_closed = true;
            LOG_ERR("anchor range journal commit failed closed: event=%u fragments=%u outcome=%u ret=%d",
                    event_seq,
                    anchor_range_report_batch_reservation
                        .queued_fragment_count,
                    (unsigned int)observation,
                    ret);
            app_watchdog_stop_feeding();
            goto out;
        }
        anchor_range_report_journal_runtime =
            (struct anchor_range_report_journal_runtime) {
                .control =
                    anchor_range_report_batch_reservation.journal,
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
    ARG_UNUSED(persistence_deadline_ms);
    return 0;
#endif
}

int mesh_restore_anchor_range_report_journal(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct anchor_range_journal_control control;
    uint32_t queue_count_before;
    uint32_t queue_depth;
    bool skipped_active_outbox = false;
    int ret;

    ret = app_mesh_persistence_restore_anchor_range_journal(&control);
    if (ret <= 0) {
        return ret;
    }
    if (control.anchor_id != DEVICE_ID || control.gateway_id != GATEWAY_ID) {
        return -EBADMSG;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_reservation.active ||
        anchor_range_report_journal_runtime.active ||
        app_mesh_queue_head_owned(&report_tx_queue_head_owner) ||
        k_msgq_num_free_get(&report_tx_msgq) < control.fragment_count) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return -EBUSY;
    }
    queue_count_before = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    anchor_range_report_batch_reservation =
        (struct anchor_range_report_batch_reservation) {
            .journal = control,
            .clicker_id = control.clicker_id,
            .event_seq = control.event_seq,
            .queue_count_before_batch = queue_count_before,
            .attempt_index = control.attempt_index,
            .active = true,
        };
    for (uint8_t i = 0u; i < control.fragment_count; i++) {
        struct mesh_outbound restored;
        int active_owner_match;

        ret = app_mesh_persistence_restore_anchor_range_fragment(
            &control, i, &restored);
        if (ret < 0) {
            break;
        }
        active_owner_match = skipped_active_outbox ?
            0 : anchor_range_report_active_owner_matches(
                    &control, i, &restored);
        if (active_owner_match < 0) {
            ret = active_owner_match;
            break;
        }
        if (active_owner_match > 0) {
            skipped_active_outbox = true;
            continue;
        }
        mesh_outbound_refresh_age(&restored, k_uptime_get_32());
        mesh_attach_paused_delivery_loss(
            &restored, "restore-range-report-journal");
        ret = k_msgq_put(&report_tx_msgq, &restored, K_NO_WAIT);
        if (ret != 0) {
            ret = -EIO;
            break;
        }
        anchor_range_report_batch_reservation.queued_fragment_count++;
    }
    if (ret >= 0) {
        anchor_range_report_journal_runtime =
            (struct anchor_range_report_journal_runtime) {
                .control = control,
                .active = true,
            };
        memset(&anchor_range_report_batch_reservation, 0,
               sizeof(anchor_range_report_batch_reservation));
        (void)report_tx_queue_promote_local_locked();
        queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (ret < 0) {
        mesh_range_report_batch_abort(
            control.clicker_id, control.event_seq, control.attempt_index);
        return ret;
    }

    LOG_INF("anchor range journal restored: event=%u fragments=%u queued=%u active_outbox=%u",
            control.event_seq,
            control.fragment_count,
            queue_depth - queue_count_before,
            skipped_active_outbox ? 1u : 0u);
    if (queue_depth > 0u && !anchor_uwb_window_active()) {
        report_tx_schedule(0u);
    }
    return 1;
#else
    return -ENOTSUP;
#endif
}

int mesh_anchor_range_report_note_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint16_t all_acknowledged;
    uint8_t fragment_index = 0u;
    int ret;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (!anchor_range_report_journal_runtime.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return 0;
    }
    ret = anchor_range_report_fragment_digest_match_locked(
        packet, semantic_digest, &fragment_index);
    if (ret <= 0) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return ret;
    }
    anchor_range_report_journal_runtime.acknowledged_mask |=
        (uint16_t)(UINT16_C(1) << fragment_index);
    all_acknowledged = anchor_range_report_all_acknowledged_mask(
        anchor_range_report_journal_runtime.control.fragment_count);
    if (all_acknowledged != 0u &&
        anchor_range_report_journal_runtime.acknowledged_mask ==
            all_acknowledged) {
        ret = app_mesh_persistence_clear_anchor_range_journal(
            &anchor_range_report_journal_runtime.control);
        if (ret == 0) {
            LOG_INF("anchor range journal retired after all gateway ACKs: event=%u fragments=%u",
                    anchor_range_report_journal_runtime.control.event_seq,
                    anchor_range_report_journal_runtime.control.fragment_count);
            memset(&anchor_range_report_journal_runtime, 0,
                   sizeof(anchor_range_report_journal_runtime));
        } else {
            LOG_WRN("anchor range journal retirement deferred: event=%u ret=%d",
                    anchor_range_report_journal_runtime.control.event_seq,
                    ret);
            k_mutex_unlock(&report_tx_queue_overflow_lock);
            return ret;
        }
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
    if (!anchor_range_report_journal_runtime.active) {
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
     * The fragments form one durable range-report batch. Once any exact
     * fragment reaches a terminal transport outcome, the batch can no longer
     * promise complete replay, so retire its commit marker atomically instead
     * of letting the expired fragment resurrect on every reset.
     */
    ret = app_mesh_persistence_clear_anchor_range_journal(
        &anchor_range_report_journal_runtime.control);
    if (ret == 0) {
        LOG_WRN("anchor range journal retired after terminal fragment: event=%u index=%u",
                anchor_range_report_journal_runtime.control.event_seq,
                fragment_index);
        memset(&anchor_range_report_journal_runtime, 0,
               sizeof(anchor_range_report_journal_runtime));
    } else {
        LOG_WRN("anchor range terminal journal retirement deferred: event=%u ret=%d",
                anchor_range_report_journal_runtime.control.event_seq,
                ret);
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
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
    return range_batch_active || report_tx_queue_used() > 0u ||
           mesh_ch9_tx_pending_is_active();
#else
    return mesh_ch9_tx_pending_is_active();
#endif
}

bool mesh_report_ch9_ack_wait_active(void)
{
    return mesh_ch9_tx_pending_is_active();
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
    struct app_mesh_queue_head_token head_token;
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
    if (k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER) == 0) {
        bool range_batch_active;

        if (anchor_range_report_batch_reservation.active &&
            !anchor_range_report_batch_reservation.persistence_fail_closed &&
            anchor_range_report_batch_reservation.final_fragment_staged) {
            ret = app_mesh_persistence_commit_anchor_range_journal(
                &anchor_range_report_batch_reservation.journal);
            if (ret == 0) {
                anchor_range_report_journal_runtime =
                    (struct anchor_range_report_journal_runtime) {
                        .control =
                            anchor_range_report_batch_reservation.journal,
                        .active = true,
                    };
                memset(&anchor_range_report_batch_reservation, 0,
                       sizeof(anchor_range_report_batch_reservation));
                (void)report_tx_queue_promote_local_locked();
            }
        }
        range_batch_active =
            anchor_range_report_batch_reservation.active;
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        if (range_batch_active) {
            report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            return;
        }
    }
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (report_tx_queue_used() < 2u) {
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
    report_queue_pending = report_tx_queue_used() > 0u;
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
        report_tx_queue_peek(outbound) == 0) {
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

    ret = report_tx_queue_begin_head(outbound, &head_token);
    if (ret != 0) {
        return;
    }

    {
        uint16_t queue_depth = (uint16_t)report_tx_queue_used();
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
        (void)report_tx_queue_abort_head(&head_token);
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
                ret = report_tx_queue_commit_head(&head_token, outbound);
                if (ret != 0) {
                    LOG_ERR("report queue transit-drop commit failed: %d", ret);
                    (void)report_tx_queue_abort_head(&head_token);
                    report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
                    return;
                }
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
                (void)report_tx_queue_abort_head(&head_token);
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
                                           &report_policy_deferred,
                                           0u,
                                           NULL);
    if (ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

        app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                            &retry_key);
        ret = report_tx_queue_commit_head(&head_token, outbound);
        if (ret != 0) {
            LOG_ERR("report queue tracked-TX commit failed: %d", ret);
            (void)report_tx_queue_abort_head(&head_token);
            report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        }
        return;
    }

    if (ret == -EAGAIN) {
        uint32_t retry_delay_ms = 0u;

        (void)report_tx_queue_abort_head(&head_token);
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
        ret = report_tx_queue_commit_head(&head_token, outbound);
        if (ret != 0) {
            LOG_ERR("report queue route-wait transfer commit failed: %d", ret);
            (void)report_tx_queue_abort_head(&head_token);
            report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        }
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

        (void)report_tx_queue_abort_head(&head_token);
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

        (void)report_tx_queue_abort_head(&head_token);
        status_debug_printf("DBG_REPORT_TX_WAIT_CH9_SLOT now=%u delay=%u\n",
                            k_uptime_get_32(),
                            delay_ms);
        report_tx_schedule(delay_ms);
        return;
    }

    if (ret == -EHOSTUNREACH) {
        (void)report_tx_queue_abort_head(&head_token);
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    {
        int commit_ret = report_tx_queue_commit_head(&head_token, outbound);

        if (commit_ret != 0) {
            LOG_ERR("report queue permanent-drop commit failed: %d", commit_ret);
            (void)report_tx_queue_abort_head(&head_token);
            report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
            return;
        }
    }
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
    struct app_mesh_paused_delivery_store_result drop_result = {0};
    uint32_t queue_depth = 0u;
    bool admitted = false;
    bool owner_active_at_failure = false;
    bool queued_local_priority;
    int ret;

    queued = *outbound;
    mesh_outbound_refresh_age(&queued, k_uptime_get_32());
    mesh_attach_paused_delivery_loss(&queued, "queue-anchor-report");
    queued_local_priority = mesh_outbound_is_local_origin_priority(&queued);

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);
        return -ENOSPC;
    }

    if (anchor_range_report_batch_reservation.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return -EAGAIN;
    }
    (void)report_tx_queue_restore_recovery_locked();
    if (!report_tx_queue_recovery_valid &&
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
        app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);
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

    if ((queued.packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
        uint16_t click_queue_depth = (uint16_t)queue_depth;

        app_stack_workload_diag_click_admit(&queued.packet, click_queue_depth,
                                            click_queue_depth);
        /* Admission retains the new click beside any earlier report custody. */
        app_stack_workload_diag_click_sample(&queued.packet, click_queue_depth,
                                             click_queue_depth);
    }
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x earliest_tx_ms=%u queue_depth=%u",
            queued.packet.msg_type,
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
    int ret;

    ret = mesh_gateway_ack_confirm_identity_packet(
        confirm_packet,
        confirm_payload,
        confirm_payload_len,
        &original_packet,
        original_digest);
    if (ret != PROTO_OK || original_packet.src_id != DEVICE_ID ||
        original_packet.dst_id != GATEWAY_ID) {
        return -EBADMSG;
    }

    /*
     * Validate and durably consume every digest-aware producer before
     * touching node_comm, whose public key is only the compact packet
     * identity. This ordering prevents a delayed same-header/different-bytes
     * confirmation from terminalizing a newer operation.
     */
    ret = app_mesh_persistence_complete_confirmed_producer(
        &original_packet, original_digest);
    if (ret < 0 && ret != -ENOTSUP) {
        LOG_WRN("gateway ACK-confirm producer cleanup deferred: ret=%d",
                ret);
        return ret;
    }
    ret = mesh_anchor_range_report_note_gateway_confirmed(
        &original_packet, original_digest);
    if (ret < 0) {
        LOG_WRN("gateway ACK-confirm range cleanup deferred: ret=%d",
                ret);
        return ret;
    }
    if ((original_packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT ||
         original_packet.msg_type == MSG_SURVEY_PAIR_RESULT) &&
        mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed !=
            NULL) {
        ret =
            mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed(
                &original_packet, original_digest);
        if (!mesh_terminal_cleanup_idempotent_status(ret)) {
            LOG_WRN("gateway ACK-confirm survey cleanup deferred: ret=%d",
                    ret);
            return ret;
        }
    }

    ret = app_mesh_persistence_clear_deferred_outbox_if_matches(
        &original_packet, original_digest);
    if (ret < 0 && ret != -ENOTSUP) {
        mesh_deferred_outbox_pending = true;
        LOG_WRN("gateway ACK-confirm deferred outbox cleanup failed: %d",
                ret);
        return ret;
    }
    if (ret == 0) {
        int present_ret = app_mesh_persistence_deferred_outbox_present();

        if (present_ret >= 0) {
            mesh_deferred_outbox_pending = present_ret > 0;
        } else if (present_ret != -ENOTSUP && present_ret != -ENODEV &&
                   present_ret != -EAGAIN) {
            mesh_deferred_outbox_pending = true;
            return present_ret;
        }
    } else {
        mesh_deferred_outbox_pending = false;
    }

    ret = app_node_comm_note_gateway_confirmed_digest_at(
        &original_packet, original_digest, confirmed_at_ms);
    if (!mesh_terminal_cleanup_idempotent_status(ret)) {
        LOG_WRN("gateway ACK-confirm node communication cleanup deferred: ret=%d",
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
        return -EBADMSG;
    }
    ret = mesh_save_outbox_durable("gateway-ack-confirm-terminal");
    if (ret < 0) {
        /*
         * The in-RAM owner is already terminal, while the last proven NVS
         * state is still the compact confirm. The persistence retry clears
         * it this boot; a reset safely replays the same idempotent cleanup.
         */
        return ret;
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
        terminal->packet.src_id != DEVICE_ID ||
        terminal->packet.dst_id != GATEWAY_ID ||
        terminal->packet.payload_len != terminal->payload_len ||
        !mesh_packet_semantic_digest(&terminal->packet,
                                     terminal->payload,
                                     terminal->payload_len,
                                     semantic_digest)) {
        return -EBADMSG;
    }

    /*
     * Retire every exact producer owner before releasing the relay's final
     * durable copy. Each operation is digest-bound and idempotent, so a
     * storage failure can leave the relay in WAIT_TERMINAL_COMMIT and retry
     * this sequence without terminalizing a same-header/different-payload
     * operation.
     */
    ret = app_mesh_persistence_complete_terminal_producer(
        &terminal->packet, terminal->payload, terminal->payload_len);
    if (ret < 0 && ret != -ENOTSUP) {
        return ret;
    }
    ret = mesh_anchor_range_report_note_terminal_release(
        &terminal->packet, terminal->payload, terminal->payload_len);
    if (!mesh_terminal_cleanup_idempotent_status(ret)) {
        return ret;
    }
    if ((terminal->packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT ||
         terminal->packet.msg_type == MSG_SURVEY_PAIR_RESULT) &&
        mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_survey_delivery_transport_released !=
            NULL) {
        ret =
            mesh_report_callbacks->anchor_survey_delivery_transport_released(
                &terminal->packet, semantic_digest, false);
        if (!mesh_terminal_cleanup_idempotent_status(ret)) {
            return ret;
        }
    }

    ret = app_mesh_persistence_clear_deferred_outbox_if_matches(
        &terminal->packet, semantic_digest);
    if (ret < 0 && ret != -ENOTSUP) {
        mesh_deferred_outbox_pending = true;
        return ret;
    }
    if (ret == 0) {
        int present_ret = app_mesh_persistence_deferred_outbox_present();

        if (present_ret >= 0) {
            mesh_deferred_outbox_pending = present_ret > 0;
        } else if (present_ret != -ENOTSUP && present_ret != -ENODEV &&
                   present_ret != -EAGAIN) {
            mesh_deferred_outbox_pending = true;
            return present_ret;
        }
    } else {
        mesh_deferred_outbox_pending = false;
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
    return mesh_save_outbox_durable("result-grant-terminal");
}

static void mesh_handle_result_actions(struct mesh_relay_result *result,
                                       uint8_t received_radio_channel,
                                       const struct mesh_rx_pending *rx,
                                       const struct proto_packet *confirmed_packet,
                                       const uint8_t *confirmed_payload,
                                       size_t confirmed_payload_len,
                                       const uint8_t confirmed_digest[
                                           SEMANTIC_DIGEST_SHA256_LEN])
{
    bool forward_sent = false;
    bool forward_queued_gateway_ack = false;
    bool child_custody_ready = true;
    bool route_reply_downstream_handoff_required =
        (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u;
    bool route_reply_downstream_handoff_acked = false;
    struct app_mesh_result_handoff_ops handoff_ops = {
        .save_child_custody = mesh_handoff_save_child_custody,
        .note_result_bundle_forwarded = mesh_handoff_note_result_bundle_forwarded,
        .send_result_grant = mesh_handoff_send_result_grant,
        .note_tx_sent = mesh_handoff_note_tx_sent,
    };
    struct app_mesh_result_handoff_status handoff_status;

    if (DEVICE_ROLE == ROLE_ANCHOR &&
        (result->actions & MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED) != 0u &&
        mesh_save_child_custody_durable("child-custody-expired") < 0) {
        LOG_WRN("mesh expired child custody snapshot cleanup deferred");
    }

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound *gateway_ack = &result->gateway_ack;
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
                status_debug_printf("DBG_GATEWAY_ACK_GUARD ms=%u seq=%u\n",
                                    MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                    gateway_ack->packet.seq);
                k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
                ret = mesh_send_outbound_keep_channel9_awake(
                    gateway_ack, ack_decision.reason, NULL);
                status_debug_printf("DBG_GATEWAY_ACK_SEND ret=%d seq=%u dst=0x%llx next=0x%llx\n",
                                    ret,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->packet.dst_id,
                                    (unsigned long long)gateway_ack->next_hop_id);
                if (ret == 0) {
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
                   (result->forward.packet.msg_type == MSG_COMMAND ||
                    result->forward.packet.msg_type ==
                        MSG_SURVEY_PAIR_PREPARE)) {
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
        struct mesh_outbound *hop_ack = &result->hop_ack;

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
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) {
        bool forward_admission_retained = false;
        int ret;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_BEGIN ttl=%u seq=%u earliest=%u\n",
                                result->gateway_route_adv.packet.ttl,
                                result->gateway_route_adv.packet.seq,
                                result->gateway_route_adv.earliest_tx_ms);
        }
        ret = mesh_send_c5_flood_response(&result->gateway_route_adv,
                                          C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                                          "gateway-route-adv-forward",
                                          &forward_admission_retained);
        if (!forward_admission_retained) {
            /*
             * Freshness was read-back durable before action processing.
             * Rolling it back here would let an intermediate sequence
             * overwrite the live path while NVS still names the newer one.
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
        struct mesh_outbound *retransmit = &result->retransmit;
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

            debug_select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                          retransmit->packet.dst_id,
                                                          &debug_next_hop);
            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_expire_channel9_timings(now_ms, "retransmit");
            direct_gateway_retransmit =
                IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE != ROLE_GATEWAY &&
                retransmit->packet.dst_id == GATEWAY_ID &&
                debug_select_ret == PROTO_OK &&
                debug_next_hop == GATEWAY_ID;
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
                retransmit->radio_channel = MESH_EVENT_CHANNEL;
                channel9_replanned = !direct_gateway_retransmit;
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
            struct app_mesh_tx_observation backend_observation = {0};
            uint64_t backend_deadline_ms = 0u;
            bool backend_rf_started = false;
            bool backend_terminal = false;
            int backend_ret = app_node_comm_backend_retry_preflight_until(
                &retransmit->packet, &backend_deadline_ms);
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
                account_ret = app_node_comm_complete_backend_attempt_at(
                    &retransmit->packet,
                    backend_rf_started,
                    backend_observation.rf_started_at_ms);
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
            .selected_timing_valid =
                selected != NULL && selected->channel9_timing_valid,
            .selected_is_unscheduled_gateway =
                route_ready_next_hop_id == GATEWAY_ID,
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = mesh_id_is_unicast(mesh_route_ready_event_peer_id),
            .selected_peer_id = route_ready_next_hop_id,
            .deferred_peer_id = mesh_route_ready_event_peer_id,
        };
        struct app_mesh_route_ready_handoff_result route_ready_result;

        atomic_inc(&mesh_route_ready_generation);

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
    if (result->actions &
        MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) {
        int save_ret = mesh_save_outbox_durable(
            "gateway-ack-confirm-pending");

        if (save_ret < 0) {
            /*
             * The last durable primary snapshot is still the original raw
             * report.  Do not transmit the in-RAM confirm until its exact
             * replacement has passed write/readback; a reset safely replays
             * the original and obtains another ACK.
             */
            LOG_WRN("gateway ACK-confirm persistence deferred: ret=%d",
                    save_ret);
        } else {
            LOG_INF("gateway ACK-confirm custody persisted");
            mesh_schedule_tx_timeout();
        }
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        const bool locally_originated =
            confirmed_packet != NULL && confirmed_packet->src_id == DEVICE_ID;
        bool deferred_cleanup_safe = true;
        int comm_ret;

        if (locally_originated &&
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
        if (confirmed_packet != NULL && confirmed_digest != NULL) {
            int deferred_clear_ret =
                app_mesh_persistence_clear_deferred_outbox_if_matches(
                    confirmed_packet, confirmed_digest);

            if (deferred_clear_ret < 0 && deferred_clear_ret != -ENOTSUP) {
                deferred_cleanup_safe = false;
                mesh_deferred_outbox_pending = true;
                LOG_WRN("mesh deferred outbox ACK cleanup failed: %d",
                        deferred_clear_ret);
            } else if (deferred_clear_ret == 0) {
                /* The ACK may belong to a different active packet; refresh
                 * instead of assuming the deferred owner was consumed. */
                int deferred_present_ret =
                    app_mesh_persistence_deferred_outbox_present();

                if (deferred_present_ret >= 0) {
                    mesh_deferred_outbox_pending = deferred_present_ret > 0;
                } else if (deferred_present_ret != -ENOTSUP &&
                           deferred_present_ret != -ENODEV &&
                           deferred_present_ret != -EAGAIN) {
                    deferred_cleanup_safe = false;
                    mesh_deferred_outbox_pending = true;
                }
            }
        }
        if (deferred_cleanup_safe) {
            (void)mesh_save_outbox_durable("gateway-confirmed");
        } else {
            /* Keep the primary snapshot so the next timeout can reconcile
             * both durable copies before retrying this already-ACKed packet. */
            LOG_WRN("mesh primary outbox retained until deferred cleanup succeeds");
        }
        mesh_schedule_tx_timeout();
    }
gateway_confirmation_done:
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
        (void)mesh_save_outbox_durable("collection-closed");
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
            LOG_WRN("mesh terminal durable cleanup retained: ret=%d status=%d",
                    complete_ret, result->status);
        }
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
        bool delivered_event_control = false;
        bool admitted_event_control = false;
        bool gateway_semantic_delivery_processed = false;
        struct proto_packet confirmed_packet = {0};
        uint8_t confirmed_payload[
            MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN] = {0};
        uint8_t confirmed_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
        size_t confirmed_payload_len = 0u;
        bool confirmed_packet_valid = mesh_relay_tx_active(&mesh_runtime);
        bool confirmed_digest_valid = false;

        if (confirmed_packet_valid) {
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
        (void)mesh_ch9_tx_pending_handle_ack(&pending->packet,
                                             pending->payload,
                                             pending->payload_len,
                                             pending->previous_hop_id,
                                             pending->radio_channel);
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
            goto mesh_rx_item_cleanup;
        }
        if (result->route_state_changed) {
            ret = app_mesh_route_state_save(&mesh_runtime);
            if (ret < 0) {
                /*
                 * The relay has admitted the transition, but no forwarding,
                 * policy install, or local delivery may escape until its
                 * ordering state has survived a read-back. Keep queued work
                 * intact and let the watchdog reboot into either the prior
                 * complete record or the newly completed one.
                 */
                LOG_ERR("mesh route state durability failed closed: epoch=%u adv_seq=%u ret=%d",
                        mesh_runtime.upstream.current_epoch,
                        mesh_runtime.gateway_route_adv_seq,
                        ret);
                (void)mesh_transport_pause_preserving_queued();
                app_watchdog_stop_feeding();
                goto mesh_rx_item_cleanup;
            }
            ret = mesh_relay_mark_route_state_durable(&mesh_runtime, result);
            if (ret != PROTO_OK) {
                LOG_ERR("mesh route state durable-boundary mismatch: epoch=%u adv_seq=%u ret=%d",
                        mesh_runtime.upstream.current_epoch,
                        mesh_runtime.gateway_route_adv_seq,
                        ret);
                (void)mesh_transport_pause_preserving_queued();
                app_watchdog_stop_feeding();
                goto mesh_rx_item_cleanup;
            }
        }
#if DEVICE_ROLE == ROLE_CLICKER
        {
            const uint32_t allowed_actions =
                MESH_RELAY_ACTION_DROP |
                MESH_RELAY_ACTION_DELIVER_LOCAL |
                MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK |
                MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY |
                MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING |
                MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED |
                MESH_RELAY_ACTION_TX_HOP_PROGRESS |
                MESH_RELAY_ACTION_TX_RELAY_BUSY;

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
            delivered_event_control = true;
            admitted_event_control = mesh_handle_event_control(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->previous_hop_id,
                pending->received_at_ms);
        }
        if (mesh_c5_control_rx_semantically_admitted(
                &pending->packet,
                result,
                admitted_event_control)) {
            mesh_note_c5_control_rx(&pending->packet,
                                    pending->previous_hop_id,
                                    pending->radio_channel,
                                    "c5-control-rx");
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_gateway_delivery_requires_semantic_acceptance(
                &pending->packet)) {
            bool internal_control =
                mesh_gateway_delivery_is_internal_control(pending);
            bool collection_redrive_without_host = false;
            bool host_custody_ready = false;
            bool journal_owned_for_packet = false;
            bool semantic_preflight_complete = false;
            bool stream_reservation_acquired = false;
            bool stream_terminal_journal_match = false;
            bool stream_terminal_receipt_match = false;
            uint8_t host_projection_mask = 0u;
            int journal_ret = 0;
            int reservation_ret = 0;
            int semantic_ret = -EINVAL;

            /*
             * Classify a durable collection duplicate before BLE admission.
             * Once its original host journal is gone, the retry exists only
             * to re-arm a lost collection EACK and must not be blocked by, or
             * consume, host queue capacity. Every other record still reserves
             * downstream host-output custody before protocol mutation.
             */
            mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
            gateway_semantic_delivery_processed = true;
            if (internal_control) {
                host_custody_ready = true;
            } else {
                journal_ret =
                    app_mesh_persistence_gateway_host_journal_matches_with_projection(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        &host_projection_mask);
                if (journal_ret == 0) {
                    semantic_ret =
                        mesh_gateway_preflight_semantic_delivery(pending);
                    semantic_preflight_complete = true;
                    if (semantic_ret ==
                            APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                        pending->packet.msg_type == MSG_RESULT_BUNDLE) {
                        int projection_ret =
                            gateway_result_bundle_host_projection_mask(
                                &pending->packet,
                                pending->payload,
                                pending->payload_len,
                                pending->previous_hop_id,
                                &host_projection_mask);

                        if (projection_ret < 0) {
                            semantic_ret = projection_ret;
                        }
                    }
                    if (semantic_ret < 0) {
                        /*
                         * A completed negative pure preflight owns no host
                         * journal and needs no BLE capacity. Preserve that
                         * rejection through the result gate below.
                         */
                        host_custody_ready = true;
                    } else if (semantic_ret ==
                                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE ||
                               semantic_ret ==
                                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE) {
                        collection_redrive_without_host = true;
                        host_custody_ready = true;
                    }
                } else if (journal_ret < 0) {
                    semantic_ret = journal_ret;
                    host_custody_ready = true;
                } else {
                    journal_owned_for_packet = true;
                }
                if (!host_custody_ready) {
                    reservation_ret = gateway_ble_reserve_stream_packet(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->received_at_ms);
                    stream_reservation_acquired =
                        reservation_ret ==
                            GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED;
                    stream_terminal_journal_match =
                        reservation_ret ==
                            GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL;
                    stream_terminal_receipt_match =
                        reservation_ret ==
                            GATEWAY_BLE_STREAM_RESERVATION_RECEIPT_TERMINAL;
                    host_custody_ready =
                        stream_reservation_acquired ||
                        stream_terminal_journal_match ||
                        stream_terminal_receipt_match;
                }
            }
            if (internal_control &&
                IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_GATEWAY_INTERNAL_CONTROL_ACCEPT src=0x%llx seq=%u\n",
                                    (unsigned long long)pending->packet.src_id,
                                    pending->packet.seq);
            }
            if (!host_custody_ready) {
                LOG_WRN("gateway semantic delivery deferred for BLE custody: msg=0x%02x src=0x%016llx ret=%d",
                        pending->packet.msg_type,
                        (unsigned long long)pending->packet.src_id,
                        reservation_ret);
            } else {
                if (stream_terminal_journal_match &&
                    journal_ret !=
                        APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED &&
                    journal_ret !=
                        APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
                    gateway_ble_require_host_journal_restore(
                        "host-output-reservation-phase-race");
                    goto mesh_rx_item_cleanup;
                }
                if (stream_terminal_receipt_match) {
                    /*
                     * Terminal host proof has already moved out of the
                     * singleton journal. Suppress semantic mutation and BLE
                     * delivery, then commit only relay duplicate/ACK state.
                     */
                    gateway_semantic_commit_owner_clear_if_matches(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    semantic_ret =
                        APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
                    semantic_preflight_complete = true;
                }
                /*
                 * A completed negative pure preflight must reach rejection
                 * before PREPARE or the mutating semantic dispatcher.
                 */
                if (semantic_preflight_complete && semantic_ret < 0) {
                    goto gateway_semantic_result_ready;
                }
                if (stream_terminal_receipt_match) {
                    /* The durable receipt selected the duplicate outcome. */
                } else if (journal_ret < 0) {
                    semantic_ret = journal_ret;
                } else if (
                    journal_ret ==
                        APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED ||
                    journal_ret ==
                        APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
                    /*
                     * A prior marker write may have succeeded while its
                     * readback failed. Exact terminal identity is the durable
                     * authority, so retire the same-boot mutation hint before
                     * any later journal owner can be admitted.
                     */
                    if (!stream_terminal_journal_match) {
                        if (stream_reservation_acquired) {
                            gateway_ble_cancel_stream_reservation();
                        }
                        gateway_ble_require_host_journal_restore(
                            "host-output-terminal-race");
                        goto mesh_rx_item_cleanup;
                    }
                    gateway_semantic_commit_owner_clear_if_matches(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    semantic_ret =
                        APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
                } else {
                    if (!internal_control &&
                        !semantic_preflight_complete &&
                        !(journal_ret ==
                              APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED &&
                          gateway_semantic_commit_owner_matches(
                              &pending->packet,
                              pending->payload,
                              pending->payload_len))) {
                        /*
                         * Validate the complete semantic transition while RX
                         * still owns every relevant state machine. Invalid or
                         * stale input must never create a PREPARED owner that
                         * blocks later host records.
                         */
                        semantic_ret =
                            mesh_gateway_preflight_semantic_delivery(
                                pending);
                        if (semantic_ret < 0) {
                            goto gateway_semantic_result_ready;
                        }
                    }
                    if (!internal_control && journal_ret == 0 &&
                        !collection_redrive_without_host) {
                        /*
                         * Persist the exact raw record only after the pure
                         * semantic admission pass and before any command,
                         * collection, or survey mutation.
                         */
                        ret =
                            app_mesh_persistence_prepare_gateway_host_journal_projection(
                                &pending->packet,
                                pending->payload,
                                pending->payload_len,
                                pending->received_at_ms,
                                host_projection_mask);
                        if (ret < 0) {
                            if (stream_reservation_acquired) {
                                gateway_ble_cancel_stream_reservation();
                            }
                            /*
                             * The PREPARED marker write may have reached NVS
                             * even when its verification failed. Force exact
                             * restore before another packet can claim the
                             * singleton.
                             */
                            gateway_ble_require_host_journal_restore(
                                "host-output-prepare-uncertain");
                            LOG_WRN("gateway host journal prepare failed without protocol mutation or ACK: msg=0x%02x src=0x%016llx ret=%d",
                                    pending->packet.msg_type,
                                    (unsigned long long)
                                        pending->packet.src_id,
                                    ret);
                            goto mesh_rx_item_cleanup;
                        }
                        journal_owned_for_packet = true;
                    }
                    if (!internal_control &&
                        journal_ret ==
                            APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED &&
                        gateway_semantic_commit_owner_matches(
                            &pending->packet,
                            pending->payload,
                            pending->payload_len)) {
                        /*
                         * The prior semantic call succeeded in this boot and
                         * only durable phase promotion failed. Do not apply
                         * the mutation twice; retry the marker transition.
                         */
                        semantic_ret =
                            APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
                    } else {
                        if (semantic_ret ==
                            APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW) {
                            /* Pure preflight proved a valid terminal raw
                             * record; no volatile semantic owner remains. */
                        } else if (semantic_ret ==
                            APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE) {
                            semantic_ret =
                                gateway_accept_collection_receipt_redrive(
                                    &pending->packet,
                                    pending->payload,
                                    pending->payload_len,
                                    pending->previous_hop_id,
                                    pending->radio_channel,
                                    pending->current_channel9_plan_valid ?
                                    &pending->current_channel9_plan : NULL);
                            if (semantic_ret == 0) {
                                semantic_ret =
                                    APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE;
                            }
                        } else {
                            semantic_ret =
                                mesh_gateway_accept_semantic_delivery(
                                    pending);
                            if (!internal_control && semantic_ret >= 0 &&
                                semantic_ret !=
                                    APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
                                semantic_ret !=
                                    APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
                                semantic_ret !=
                                    APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW &&
                                semantic_ret !=
                                    APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE) {
                                semantic_ret = -EINVAL;
                            }
                            if (!internal_control && semantic_ret >= 0) {
                                gateway_semantic_commit_owner_set(
                                    &pending->packet,
                                    pending->payload,
                                    pending->payload_len);
                            }
                        }
                    }
                }
gateway_semantic_result_ready:
                if (semantic_ret < 0) {
                    if (!internal_control) {
                        int rollback_ret = 0;

                        if (journal_owned_for_packet &&
                            journal_ret !=
                                APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED &&
                            journal_ret !=
                                APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
                            rollback_ret =
                                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                                    &pending->packet,
                                    pending->payload,
                                    pending->payload_len);
                        }
                        if (stream_reservation_acquired) {
                            gateway_ble_cancel_stream_reservation();
                        }
                        if (rollback_ret < 0) {
                            gateway_ble_require_host_journal_restore(
                                "host-output-rejected-rollback");
                            LOG_ERR("gateway rejected host journal rollback failed; durable owner remains fail-closed: msg=0x%02x src=0x%016llx ret=%d",
                                    pending->packet.msg_type,
                                    (unsigned long long)
                                        pending->packet.src_id,
                                    rollback_ret);
                        } else if (journal_owned_for_packet) {
                            gateway_semantic_commit_owner_clear_if_matches(
                                &pending->packet,
                                pending->payload,
                                pending->payload_len);
                        }
                    }
                    LOG_WRN("gateway semantic delivery rejected without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                    goto mesh_rx_item_cleanup;
                } else if (!internal_control &&
                           !collection_redrive_without_host &&
                           journal_ret !=
                               APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED &&
                           journal_ret !=
                               APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW &&
                           (semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_NEW ||
                            semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE ||
                            semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW ||
                            semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE)) {
                    ret = semantic_ret ==
                              APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW ?
                          app_mesh_persistence_recover_raw_gateway_host_journal(
                              &pending->packet,
                              pending->payload,
                              pending->payload_len) :
                          app_mesh_persistence_commit_gateway_host_journal(
                              &pending->packet,
                              pending->payload,
                              pending->payload_len);
                    if (ret < 0) {
                        if (stream_reservation_acquired) {
                            gateway_ble_cancel_stream_reservation();
                        }
                        gateway_ble_require_host_journal_restore(
                            "host-output-marker-uncertain");
                        LOG_WRN("gateway semantic commit marker failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)
                                    pending->packet.src_id,
                                ret);
                        goto mesh_rx_item_cleanup;
                    }
                    gateway_semantic_commit_owner_clear_if_matches(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    /*
                     * Semantic state and the write-ahead host journal are now
                     * committed. Commit the reserved RAM stream entry before
                     * exposing relay duplicate identity or gateway ACK.
                     */
                    if (!stream_reservation_acquired) {
                        gateway_ble_require_host_journal_restore(
                            "host-output-reservation-owner");
                        LOG_ERR("gateway semantic journal committed without owned BLE reservation: msg=0x%02x src=0x%016llx",
                                pending->packet.msg_type,
                                (unsigned long long)
                                    pending->packet.src_id);
                        goto mesh_rx_item_cleanup;
                    }
                    ret = gateway_ble_commit_stream_reservation_projection(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        host_projection_mask);
                    if (ret != 1) {
                        if (stream_reservation_acquired) {
                            gateway_ble_cancel_stream_reservation();
                        }
                        LOG_ERR("gateway semantic BLE custody commit failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                        goto mesh_rx_item_cleanup;
                    }
                    if (semantic_ret ==
                        APP_GATEWAY_SEMANTIC_ACCEPT_NEW) {
                        app_mesh_test_note_gateway_delivery(
                            &pending->packet,
                            pending->payload,
                            pending->payload_len,
                            pending->received_at_ms,
                            k_msgq_num_used_get(&mesh_rx_msgq));
                    }
                } else if (collection_redrive_without_host &&
                           (semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE ||
                            semantic_ret ==
                                APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE)) {
                    gateway_semantic_commit_owner_clear_if_matches(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                } else if (semantic_ret ==
                               APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
                           !internal_control) {
                    if (stream_reservation_acquired) {
                        gateway_ble_cancel_stream_reservation();
                    }
                } else if (!internal_control) {
                    if (stream_reservation_acquired) {
                        gateway_ble_cancel_stream_reservation();
                    }
                    LOG_ERR("gateway semantic delivery returned invalid acceptance without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                    goto mesh_rx_item_cleanup;
                }

                ret = journal_ret ==
                          APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW ||
                      semantic_ret ==
                          APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW ||
                      semantic_ret ==
                          APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE ?
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
                    LOG_WRN("gateway semantic post-custody finalization failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
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
                                   &confirmed_packet : NULL,
                                   confirmed_payload_len > 0u ?
                                       confirmed_payload : NULL,
                                   confirmed_payload_len,
                                   confirmed_digest_valid ?
                                       confirmed_digest : NULL);
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
            mesh_report_gateway_handle_survey_discovery_report(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len,
                                                   pending->first_received_at_ms,
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
            ret = mesh_report_anchor_handle_local_command(
                &pending->packet,
                pending->payload,
                pending->payload_len);
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
            mesh_report_anchor_handle_survey_discovery_start(&pending->packet,
                                                 pending->payload,
                                                 pending->payload_len);
            mesh_report_anchor_handle_survey_pair_prepare(&pending->packet,
                                              pending->payload,
                                                    pending->payload_len);
        }
        handled_count++;
mesh_rx_item_cleanup:
        gateway_command_result_validation_release_reserved(
            pending->result_validation_token);
        if (mesh_transport_paused()) {
            break;
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_EXIT owner=%s handled=%u q=%u\n",
                            owner == NULL ? "rx" : owner,
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    return handled_count;
}
