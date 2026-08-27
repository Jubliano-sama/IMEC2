#line 3284 __BASE_FILE__
static bool mesh_pending_tx_blocks_uwb_rx(void)
{
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return false;
    }

    return !(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
             app_mesh_ch9_core_pending_allows_rx(
                 &mesh_runtime.pending,
                 mesh_relay_tx_active(&mesh_runtime)));
}

static uint32_t mesh_uwb_rx_window_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_WINDOW_MS :
           UWB_MESH_ANCHOR_RX_WINDOW_MS;
}

static uint32_t mesh_uwb_rx_idle_delay_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_IDLE_MS :
           UWB_MESH_ANCHOR_RX_INTERVAL_MS;
}

static size_t mesh_node_comm_reliable_tx_peers(uint64_t *peer_ids,
                                               size_t peer_cap)
{
    uint64_t target_ids[APP_NODE_COMM_MAX_DELIVERIES];
    size_t peer_count = 0u;
    const size_t target_count = app_node_comm_reliable_delivery_targets(
        target_ids,
        sizeof(target_ids) / sizeof(target_ids[0]));

    if (peer_ids == NULL || peer_cap == 0u) {
        return 0u;
    }
    for (size_t i = 0u; i < target_count; i++) {
        uint64_t next_hop_id = 0u;
        bool duplicate = false;

        if (mesh_relay_select_next_hop(
                &mesh_runtime, target_ids[i], &next_hop_id) != PROTO_OK ||
            !mesh_id_is_unicast(next_hop_id)) {
            continue;
        }
        for (size_t j = 0u; j < peer_count; j++) {
            if (peer_ids[j] == next_hop_id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            peer_ids[peer_count++] = next_hop_id;
            if (peer_count == peer_cap) {
                break;
            }
        }
    }
    return peer_count;
}

static bool mesh_node_comm_reliable_tx_pending_for_peer(
    uint64_t peer_id,
    const uint64_t *peer_ids,
    size_t peer_count)
{
    for (size_t i = 0u; i < peer_count; i++) {
        if (peer_ids[i] == peer_id) {
            return true;
        }
    }
    return false;
}

static bool mesh_next_channel9_activity_delay_ms(uint32_t now_ms,
                                                 uint32_t *delay_ms)
{
    uint64_t local_reliable_tx_peers[APP_NODE_COMM_MAX_DELIVERIES];
    const size_t local_reliable_tx_peer_count =
        mesh_node_comm_reliable_tx_peers(
            local_reliable_tx_peers,
            sizeof(local_reliable_tx_peers) /
                sizeof(local_reliable_tx_peers[0]));
    bool found = false;
    uint32_t selected_delay_ms = 0u;

    (void)mesh_advance_all_channel9_timings_past(now_ms, "rx-delay");
    (void)mesh_expire_channel9_timings(now_ms, "rx-delay");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_timing timing = {0};
        uint32_t candidate_delay_ms;

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

            candidate_delay_ms =
                uptime_deadline_reached(now_ms, prepare_ms) ?
                0u :
                uptime_ms_until_deadline(now_ms, prepare_ms);
        }
        if (!found || candidate_delay_ms < selected_delay_ms) {
            selected_delay_ms = candidate_delay_ms;
            found = true;
        }
    }
    if (found && delay_ms != NULL) {
        *delay_ms = selected_delay_ms;
    }
    return found;
}

bool mesh_report_next_channel9_activity_prepare_delay_ms(uint32_t now_ms,
                                                         uint32_t *delay_ms)
{
    return mesh_next_channel9_activity_delay_ms(now_ms, delay_ms);
}

/*
 * A long channel-5 control listener may span several connection turns.  It
 * must still release the radio before the next downstream child transmit
 * turn, even when there is no local packet or ACK waiting.  An upstream RX
 * turn must not cut short the same parent's command-followup listener before
 * the relayed control payload arrives.  Keep this RX-only helper separate from
 * mesh_next_channel9_activity_delay_ms(), which also treats ordinary local
 * reliable traffic as required activity.
 */
static bool mesh_next_channel9_receive_prepare_delay_ms(uint32_t now_ms,
                                                        uint32_t *delay_ms)
{
    bool found = false;
    uint32_t selected_delay_ms = 0u;

    (void)mesh_advance_all_channel9_timings_past(now_ms, "c5-listener-rx");
    (void)mesh_expire_channel9_timings(now_ms, "c5-listener-rx");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];
        struct mesh_event_timing timing;
        uint32_t prepare_ms;
        uint32_t candidate_delay_ms;

        if (!entry->valid ||
            entry->direction != MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
            continue;
        }
        timing = entry->timing;
        if (!mesh_event_timing_local_rx_slot(&timing)) {
            timing.next_event_time_ms += timing.event_interval_ms;
            timing.event_counter++;
        }
        if (!mesh_event_timing_local_rx_slot(&timing) ||
            !mesh_event_timing_usable(&timing, now_ms)) {
            continue;
        }

        prepare_ms = mesh_channel9_prepare_start_ms(&timing);
        candidate_delay_ms = uptime_deadline_reached(now_ms, prepare_ms) ?
            0u : uptime_ms_until_deadline(now_ms, prepare_ms);
        if (!found || candidate_delay_ms < selected_delay_ms) {
            selected_delay_ms = candidate_delay_ms;
            found = true;
        }
    }
    if (found && delay_ms != NULL) {
        *delay_ms = selected_delay_ms;
    }
    return found;
}

static bool mesh_select_channel9_rx_event(uint32_t now_ms,
                                          struct mesh_event_plan *selected_plan,
                                          uint64_t *selected_peer,
                                          uint8_t *selected_index)
{
    struct mesh_channel5_requirements requirements;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL || selected_index == NULL) {
        return false;
    }

    (void)mesh_advance_all_channel9_timings_past(now_ms, "rx-select");
    (void)mesh_expire_channel9_timings(now_ms, "rx-select");
    mesh_fill_channel5_requirements(&requirements);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_plan plan = {0};
        int ret;

        if (!entry->valid) {
            continue;
        }
        if (!mesh_event_timing_local_rx_slot(&entry->timing)) {
            continue;
        }
        ret = mesh_event_plan_channel9(&entry->timing, &requirements, now_ms, &plan);
        if (ret != PROTO_OK) {
            continue;
        }
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        mesh_debug_channel5_preemption("rx",
                                       "channel9-rx-select",
                                       entry->next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (plan.action != MESH_EVENT_PLAN_START &&
            plan.action != MESH_EVENT_PLAN_CLIP) {
            continue;
        }
        if (!selected || uptime_deadline_reached(selected_plan->start_ms, plan.start_ms)) {
            *selected_plan = plan;
            *selected_peer = entry->next_hop_id;
            *selected_index = i;
            selected = true;
        }
	    }
	    if (selected && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        const struct mesh_relay_event_timing_entry *entry =
	            &mesh_runtime.event_timings[*selected_index];

	        status_debug_printf("DBG_CH9_RX_SLOT idx=%u cnt=%u now=%u start=%u end=%u\n",
	                            *selected_index,
	                            entry->timing.event_counter,
	                            now_ms,
	                            selected_plan->start_ms,
	                            selected_plan->end_ms);
	    }
	    return selected;
}

static bool mesh_select_direct_gateway_ack_rx(
    uint32_t now_ms,
    struct mesh_event_plan *selected_plan,
    uint64_t *selected_peer)
{
    const struct mesh_pending_tx *pending = &mesh_runtime.pending;
    struct mesh_event_timing timing;
    uint32_t remaining_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR || selected_plan == NULL ||
        selected_peer == NULL || !mesh_relay_tx_active(&mesh_runtime) ||
        pending->state != MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
        pending->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !mesh_id_is_unicast(pending->next_hop_id) ||
        pending->gateway_ack_deadline_ms == 0u ||
        uptime_deadline_reached(now_ms, pending->gateway_ack_deadline_ms) ||
        mesh_find_active_channel9_timing(pending->next_hop_id,
                                         now_ms,
                                         &timing)) {
        return false;
    }

    remaining_ms = uptime_ms_until_deadline(
        now_ms, pending->gateway_ack_deadline_ms);
    *selected_plan = (struct mesh_event_plan) {
        .action = MESH_EVENT_PLAN_START,
        .start_ms = now_ms,
        .end_ms = pending->gateway_ack_deadline_ms,
        .window_ms = (uint16_t)MIN(remaining_ms, (uint32_t)UINT16_MAX),
    };
    *selected_peer = pending->next_hop_id;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_DIRECT_GW_ACK_RX_TURN now=%u end=%u deadline=%u seq=%u\n",
            now_ms,
            selected_plan->end_ms,
            pending->gateway_ack_deadline_ms,
            pending->packet.seq);
    }
    return true;
}

static bool mesh_select_channel9_ack_tx_event(uint32_t now_ms,
                                               struct mesh_event_plan *selected_plan,
                                               uint64_t *selected_peer)
{
    struct mesh_channel5_requirements requirements;
    uint32_t selected_delay_ms = UINT32_MAX;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL ||
        !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
        return false;
    }

    mesh_fill_channel5_requirements(&requirements);
    (void)mesh_expire_channel9_timings(now_ms, "ack-tx-select");
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        struct mesh_event_plan plan = {0};
        uint32_t delay_ms;
        uint64_t peer_id;
        int ret;

        if (!batch->valid || batch->count == 0u) {
            continue;
        }
        peer_id = batch->peer_id;
        (void)mesh_advance_channel9_timing_past(peer_id,
                                                now_ms,
                                                "ack-tx-select");
        if (!app_mesh_ch9_ack_table_pending_for_peer(&mesh_ch9_ack_table,
                                                      peer_id)) {
            continue;
        }
        if (!app_mesh_ch9_ack_table_retry_ready(&mesh_ch9_ack_table,
                                                 peer_id,
                                                 now_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf(
                    "DBG_CH9_ACK_BACKOFF peer=0x%llx wait=%u round=%u n=%u\n",
                    (unsigned long long)peer_id,
                    app_mesh_ch9_ack_table_retry_wait_ms(
                        &mesh_ch9_ack_table, peer_id, now_ms),
                    batch->retry_round,
                    batch->count);
            }
            continue;
        }
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   peer_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("ack-tx",
                                       "ch9-ack-batch-slot",
                                       peer_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note(ret == PROTO_ERR_STALE ?
                                  "DBG_CH9_ACK_TX_STALE\n" :
                                  "DBG_CH9_ACK_TX_WAIT\n");
                status_debug_printf("DBG_CH9_ACK_WAIT peer=0x%llx ret=%d act=%u now=%u start=%u n=%u\n",
                                    (unsigned long long)peer_id,
                                    ret,
                                    (unsigned int)plan.action,
                                    now_ms,
                                    plan.start_ms,
                                    batch->count);
            }
            if (ret == PROTO_ERR_STALE) {
                bool transit_core_owned =
                    mesh_ch9_ack_batch_requires_physical_commit(peer_id);
                bool late_terminal_forward =
                    mesh_ch9_ack_batch_is_late_terminal_forward(peer_id);

                if (transit_core_owned || late_terminal_forward) {
                    struct app_mesh_c5_tx_authorization_token authorization;
                    enum app_mesh_c5_tx_authorization authorization_kind =
                        transit_core_owned ?
                            APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR :
                            APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR;
                    int repair_ret;

                    if (mesh_local_delivery_blocks_background_event_repair()) {
                        uint32_t retry_delay_ms = 0u;

                        (void)app_mesh_ch9_ack_table_note_send_failure(
                            &mesh_ch9_ack_table,
                            peer_id,
                            now_ms,
                            sys_rand32_get(),
                            &retry_delay_ms);
                        status_debug_printf(
                            "DBG_FORWARD_GATEWAY_ACK_EVENT_DEFER peer=0x%llx delay=%u owner=local\n",
                            (unsigned long long)peer_id,
                            retry_delay_ms);
                        report_tx_schedule(retry_delay_ms == 0u ? 1u :
                                           retry_delay_ms);
                        continue;
                    }

                    if (!app_mesh_ch9_c5_repair_authorization_capture(
                            &authorization,
                            authorization_kind,
                            &mesh_runtime.pending,
                            mesh_relay_tx_active(&mesh_runtime),
                            batch,
                            peer_id)) {
                        LOG_ERR("stale forwarded gateway ACK could not mint owner-specific event repair: peer=0x%016llx owner=%u",
                                (unsigned long long)peer_id,
                                (unsigned int)authorization_kind);
                        app_watchdog_stop_feeding();
                        repair_ret = -ESTALE;
                    } else {
                        repair_ret =
                            mesh_propose_event_after_channel5_contact_authorized(
                                peer_id,
                                "forwarded-gateway-ack-event-repair",
                                &authorization,
                                false,
                                false);
                    }

                    status_debug_printf(
                        "DBG_FORWARD_GATEWAY_ACK_EVENT_REPAIR peer=0x%llx owner=%s ret=%d\n",
                        (unsigned long long)peer_id,
                        transit_core_owned ? "transit" : "late",
                        repair_ret);
                    if (repair_ret < 0) {
                        report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
                    }
                } else {
                    (void)mesh_ch9_ack_batch_discard_if_safe(
                        peer_id, "ack-tx-stale");
                }
            } else if (ret == PROTO_ERR_BUSY &&
                       mesh_event_plan_is_policy_deferral(plan.action)) {
                uint32_t retry_delay_ms = 0u;

                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                peer_id,
                                                &mesh_event_stats);
                (void)app_mesh_ch9_ack_table_note_send_failure(
                    &mesh_ch9_ack_table,
                    peer_id,
                    now_ms,
                    sys_rand32_get(),
                    &retry_delay_ms);
                status_debug_printf(
                    "DBG_CH9_ACK_POLICY_DEFER peer=0x%llx delay=%u act=%u\n",
                    (unsigned long long)peer_id,
                    retry_delay_ms,
                    (unsigned int)plan.action);
            }
            continue;
        }

        if (mesh_forwarded_ack_event_repair_authorization.valid &&
            mesh_forwarded_ack_event_repair_authorization.peer_id == peer_id) {
            memset(&mesh_forwarded_ack_event_repair_authorization, 0,
                   sizeof(mesh_forwarded_ack_event_repair_authorization));
        }

        delay_ms = uptime_deadline_reached(now_ms, plan.start_ms) ?
                   0u :
                   uptime_ms_until_deadline(now_ms, plan.start_ms);
        if (!selected || delay_ms < selected_delay_ms) {
            *selected_plan = plan;
            *selected_peer = peer_id;
            selected_delay_ms = delay_ms;
            selected = true;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t event_counter = 0u;

            (void)mesh_debug_channel9_state(peer_id, &event_counter, NULL);
            status_debug_printf("DBG_CH9_ACK_SLOT peer=0x%llx cnt=%u now=%u start=%u end=%u n=%u\n",
                                (unsigned long long)peer_id,
                                event_counter,
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                batch->count);
        }
    }
    return selected;
}

static bool mesh_channel9_ack_pending_for_peer(uint64_t peer_id)
{
    return app_mesh_ch9_ack_table_pending_for_peer(&mesh_ch9_ack_table,
                                                    peer_id);
}

static bool mesh_channel9_core_tx_pending_for_peer(uint64_t peer_id)
{
    return mesh_id_is_unicast(peer_id) &&
           mesh_runtime.pending.next_hop_id == peer_id &&
           app_mesh_ch9_core_pending_allows_rx(
               &mesh_runtime.pending,
               mesh_relay_tx_active(&mesh_runtime));
}

static bool mesh_channel9_next_required_activity(
    const struct mesh_relay_event_timing_entry *entry,
    const uint64_t *local_reliable_tx_peers,
    size_t local_reliable_tx_peer_count,
    struct mesh_event_timing *timing)
{
    if (entry == NULL || !entry->valid || timing == NULL) {
        return false;
    }

    *timing = entry->timing;
    if (mesh_event_timing_local_rx_slot(timing) ||
        (mesh_event_timing_local_tx_slot(timing) &&
         (mesh_node_comm_reliable_tx_pending_for_peer(
              entry->next_hop_id,
              local_reliable_tx_peers,
              local_reliable_tx_peer_count) ||
          mesh_channel9_core_tx_pending_for_peer(entry->next_hop_id) ||
          mesh_channel9_ack_pending_for_peer(entry->next_hop_id) ||
          mesh_channel9_close_intent_ready_for_peer(
              entry->next_hop_id)))) {
        return true;
    }

    timing->next_event_time_ms += timing->event_interval_ms;
    timing->event_counter++;
    return mesh_event_timing_local_rx_slot(timing);
}

static uint32_t mesh_channel9_prepare_start_ms(const struct mesh_event_timing *timing)
{
    uint32_t retune_guard_ms;

    if (timing == NULL) {
        return 0u;
    }

    retune_guard_ms = timing->guard_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        retune_guard_ms < MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS) {
        retune_guard_ms = MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS;
    }

    if (timing->next_event_time_ms <= retune_guard_ms) {
        return 1u;
    }
    return timing->next_event_time_ms - retune_guard_ms;
}

static uint32_t mesh_next_channel9_rx_delay_ms(uint32_t now_ms)
{
    uint32_t delay_ms = mesh_uwb_rx_idle_delay_ms();

    if (mesh_next_channel9_activity_delay_ms(now_ms, &delay_ms)) {
        return delay_ms;
    }
    return delay_ms;
}

static uint32_t mesh_active_channel9_ch5_gap_window_ms(uint32_t now_ms)
{
    uint32_t delay_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        mesh_channel9_connection_count() == 0u ||
        !mesh_gateway_route_test_ch5_scan_has_capacity()) {
        return 0u;
    }

    delay_ms = mesh_next_channel9_rx_delay_ms(now_ms);
    return app_mesh_c5_connected_gap_window_ms(
        &(const struct app_mesh_c5_connected_gap_timing) {
            .next_channel9_delay_ms = delay_ms,
            .scan_cap_ms = DEVICE_ROLE == ROLE_GATEWAY ? UINT32_MAX :
                           MESH_ROUTE_TEST_CH5_GAP_SCAN_MS,
            .min_scan_ms = MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
            .retune_margin_ms = MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS,
        });
}

static void mesh_anchor_yield_idle_ch5_to_low_duty_scan(const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t next_delay_ms = 0u;
    bool channel9_pending =
        mesh_next_channel9_activity_delay_ms(now_ms, &next_delay_ms);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (mesh_anchor_rx_yield_log_next_ms == 0u ||
         uptime_deadline_reached(now_ms, mesh_anchor_rx_yield_log_next_ms) ||
         channel9_pending)) {
        mesh_anchor_rx_yield_log_next_ms = now_ms + MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS;
        status_debug_printf("DBG_ANCHOR_MESH_RX_YIELD reason=%s ch9=%u delay=%u scan_int=%u scan_win=%u wait=%u q=%u\n",
                            reason == NULL ? "idle" : reason,
                            channel9_pending ? 1u : 0u,
                            next_delay_ms,
                            anchor_uwb_scan_interval_ms,
                            ANCHOR_UWB_SCAN_RX_MS,
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }

    if (channel9_pending) {
        mesh_schedule_uwb_rx(next_delay_ms);
    } else {
        mesh_uwb_rx_active = false;
    }
}

static int mesh_defer_uwb_rx_rearm(uint32_t delay_ms)
{
    int ret;

    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    mesh_uwb_rx_rearm_generation++;
    if (mesh_uwb_rx_rearm_generation == 0u) {
        mesh_uwb_rx_rearm_generation = 1u;
    }
    mesh_uwb_rx_rearm_delay_ms = delay_ms;
    mesh_uwb_rx_rearm_pending = true;
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);

    ret = mesh_submit_owned_work(&mesh_uwb_rx_rearm_work,
                                 "role-scan-rearm");
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_REARM_DEFER delay=%u ret=%d active=%u\n",
                            delay_ms,
                            ret,
                            mesh_uwb_rx_active ? 1u : 0u);
    }
    return ret;
}

static bool mesh_take_deferred_uwb_rx_rearm(uint32_t *delay_ms,
                                            uint32_t *generation)
{
    bool pending;

    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    pending = mesh_uwb_rx_rearm_pending;
    if (pending) {
        if (delay_ms != NULL) {
            *delay_ms = mesh_uwb_rx_rearm_delay_ms;
        }
        if (generation != NULL) {
            *generation = mesh_uwb_rx_rearm_generation;
        }
        mesh_uwb_rx_rearm_pending = false;
    }
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);
    return pending;
}

static void mesh_clear_deferred_uwb_rx_rearm(void)
{
    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    mesh_uwb_rx_rearm_generation++;
    if (mesh_uwb_rx_rearm_generation == 0u) {
        mesh_uwb_rx_rearm_generation = 1u;
    }
    mesh_uwb_rx_rearm_pending = false;
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);
    (void)k_work_cancel(&mesh_uwb_rx_rearm_work);
}

static void mesh_uwb_rx_rearm_work_handler(struct k_work *work)
{
    uint32_t delay_ms = 0u;
    uint32_t generation = 0u;
    uint32_t current_generation = 0u;
    bool stale = false;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        return;
    }

    if (!mesh_take_deferred_uwb_rx_rearm(&delay_ms, &generation)) {
        return;
    }

    /*
     * Immediate causal ACK ownership clears the deferred rearm and then
     * cancels the role scan.  Serialize the final generation check with the
     * actual reschedule so a rearm worker that was already running cannot
     * publish RX after that clear/cancel boundary.
     */
    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    stale = generation != mesh_uwb_rx_rearm_generation ||
            mesh_transport_paused() ||
            !mesh_rx_handoff_scan_rearm_allowed() ||
            !mesh_uwb_rx_active || !mesh_role_uses_uwb_rx();
    current_generation = mesh_uwb_rx_rearm_generation;
    if (stale) {
        ret = -ECANCELED;
    } else {
        ret = mesh_reschedule_owned_work_with_busy_handoff(
            &mesh_uwb_rx_work,
            delay_ms,
            "role-scan-rearm");
    }
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);
    if (stale) {
        if (mesh_gateway_route_test_role()) {
            status_debug_printf(
                "DBG_GATEWAY_RX_REARM_STALE generation=%u current=%u\n",
                generation,
                current_generation);
        }
        return;
    }
    if (ret == -EBUSY) {
        (void)mesh_defer_uwb_rx_rearm(delay_ms);
        return;
    }
    if (ret < 0) {
        mesh_uwb_rx_active = false;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_REARM_RUN delay=%u ret=%d active=%u\n",
                            delay_ms,
                            ret,
                            mesh_uwb_rx_active ? 1u : 0u);
    }
}

static int mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    int ret;
    int defer_ret = 0;

    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }
    if (!mesh_rx_handoff_scan_rearm_allowed()) {
        mesh_uwb_rx_active = false;
        return -EAGAIN;
    }

    mesh_uwb_rx_active = true;
    ret = mesh_reschedule_owned_work_with_busy_handoff(
        &mesh_uwb_rx_work,
        delay_ms,
        "role-scan");
    if (ret == -EBUSY) {
        defer_ret = mesh_defer_uwb_rx_rearm(delay_ms);
    }
    if (ret < 0 && !(ret == -EBUSY && defer_ret >= 0)) {
        mesh_uwb_rx_active = false;
    }
    if (mesh_gateway_route_test_role() && ret < 0 &&
        !(ret == -EBUSY && defer_ret >= 0)) {
        status_debug_printf("DBG_GATEWAY_RX_SCHEDULE_FAIL ret=%d defer=%d\n",
                            ret, defer_ret);
    }
    return ret == -EBUSY && defer_ret >= 0 ? 0 : ret;
}

static bool mesh_route_test_keeps_radio_idle_between_channel9_turns(void)
{
    bool exact_ack_exchange =
        mesh_ch9_tx_pending_is_active() ||
        app_mesh_ch9_core_ack_wait_active(
            &mesh_runtime.pending,
            mesh_relay_tx_active(&mesh_runtime)) ||
        app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table);

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return false;
    }

    /*
     * A production clicker is a scheduled leaf, not an always-awake relay.
     * Retaining a peer timing alone must not pin its DWM3000 in IDLE between
     * events.  Keep the radio awake only while exact ACK custody still needs
     * the immediate receive/send horizon; otherwise the scheduled worker
     * wakes it again for the next owned turn.
     */
    if (DEVICE_ROLE == ROLE_CLICKER) {
        return exact_ack_exchange;
    }

    return mesh_channel9_connection_count() > 0u ||
           exact_ack_exchange;
}

bool mesh_anchor_connected_radio_active(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           mesh_route_test_keeps_radio_idle_between_channel9_turns();
}

static int mesh_release_radio_after_mesh_turn(bool channel9_turn,
                                              const char *reason)
{
    int ret;

    ARG_UNUSED(channel9_turn);

    if (mesh_route_test_keeps_radio_idle_between_channel9_turns()) {
        ret = mesh_radio_idle_with_bounded_recovery(reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_KEEP_IDLE reason=%s ch9=%u ack=%u qack=%u ret=%d\n",
                                reason == NULL ? "mesh" : reason,
                                mesh_channel9_connection_count(),
                                mesh_ch9_tx_pending_is_active() ? 1u : 0u,
                                app_mesh_ch9_ack_table_any_pending(
                                    &mesh_ch9_ack_table) ? 1u : 0u,
                                ret);
        }
        return ret;
    }

    return mesh_radio_standby_with_bounded_recovery(reason);
}

static int mesh_radio_standby_with_bounded_recovery(const char *reason)
{
    int first_ret;
    int recovery_ret;
    int retry_ret;

    first_ret = dwm3000_driver_standby();
    if (first_ret >= 0) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    if (recovery_ret >= 0) {
        retry_ret = dwm3000_driver_standby();
        if (retry_ret >= 0) {
            LOG_WRN("mesh radio standby recovered: reason=%s first_ret=%d",
                    reason == NULL ? "mesh" : reason,
                    first_ret);
            return 0;
        }
    } else {
        retry_ret = recovery_ret;
    }

    LOG_ERR("mesh radio standby failed after bounded recovery: reason=%s first_ret=%d recovery_ret=%d retry_ret=%d",
            reason == NULL ? "mesh" : reason,
            first_ret,
            recovery_ret,
            retry_ret);
    app_watchdog_stop_feeding();
    return recovery_ret < 0 ? recovery_ret : retry_ret;
}

static int mesh_radio_idle_with_bounded_recovery(const char *reason)
{
    int first_ret;
    int recovery_ret;
    int retry_ret;

    first_ret = dwm3000_driver_idle();
    if (first_ret >= 0) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    if (recovery_ret >= 0) {
        retry_ret = dwm3000_driver_idle();
        if (retry_ret >= 0) {
            LOG_WRN("mesh radio idle recovered: reason=%s first_ret=%d",
                    reason == NULL ? "mesh" : reason,
                    first_ret);
            return 0;
        }
    } else {
        retry_ret = recovery_ret;
    }

    LOG_ERR("mesh radio idle failed after bounded recovery: reason=%s first_ret=%d recovery_ret=%d retry_ret=%d",
            reason == NULL ? "mesh" : reason,
            first_ret,
            recovery_ret,
            retry_ret);
    app_watchdog_stop_feeding();
    return recovery_ret < 0 ? recovery_ret : retry_ret;
}

/*
 * A transport lease spans exactly one DWM3000 operation.  The old guard
 * wrapper cannot identify a delayed cleanup, so the active RX/TX adapters
 * retain this token through physical parking.
 */
static int mesh_transport_radio_claim(
    enum radio_guard_uwb_client client,
    const char *owner,
    struct radio_guard_uwb_lease *lease_out)
{
    int ret;

    if (lease_out == NULL) {
        return -EINVAL;
    }
    if (mesh_transport_paused()) {
        return -ESHUTDOWN;
    }
    ret = radio_guard_uwb_claim(client, owner, lease_out);
    if (ret < 0) {
        return ret;
    }
    if (mesh_transport_paused()) {
        ret = radio_guard_uwb_release_begin(lease_out);
        if (ret == 0) {
            ret = radio_guard_uwb_release_finish(lease_out, 0);
        }
        return ret < 0 ? ret : -ESHUTDOWN;
    }
    return 0;
}

static int mesh_transport_radio_finish(
    struct radio_guard_uwb_lease *lease,
    int parking_ret)
{
    int ret;

    ret = radio_guard_uwb_release_begin(lease);
    if (ret < 0) {
        return ret;
    }
    return radio_guard_uwb_release_finish(lease, parking_ret);
}

static bool mesh_c5_enumeration_relay_burst_active(void)
{
    return atomic_get(&mesh_c5_enumeration_relay_burst_count) != 0;
}

static bool mesh_c5_gateway_enumeration_quick_copy_burst(
    const struct mesh_outbound *out)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    enum command_id command_id;
    enum discovery_assignment_phase phase;
    uint32_t epoch;

    return out != NULL &&
           out->packet.msg_type == MSG_COMMAND &&
           gateway_command_extract_id(out->payload,
                                      out->payload_len,
                                      &command_id) == PROTO_OK &&
           command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           discovery_assignment_extract_control_tlvs(out->payload,
                                                     out->payload_len,
                                                     &phase,
                                                     &epoch) == PROTO_OK &&
           epoch != 0u &&
           (phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE ||
            phase == DISCOVERY_ASSIGNMENT_PHASE_END);
#else
    ARG_UNUSED(out);
    return false;
#endif
}

void mesh_stop_role_scan(void)
{
    mesh_clear_deferred_uwb_rx_rearm();
    if (mesh_uwb_rx_active) {
        (void)mesh_cancel_delayable(&mesh_uwb_rx_work);
        mesh_uwb_rx_active = false;
    }
}

void mesh_restart_role_scan(void)
{
    int ret;

    /* A completed wake train normally restarts RX before its caller returns.
     * The one exception is the exact retained stale-collection receipt: its
     * frozen C5 EACK still owns the radio until semantic completion retires
     * the BLE head, at which point delivery explicitly restarts the scan. */
    if (mesh_gateway_collection_recovery_receipt_active()) {
        return;
    }
    /* One logical enumeration control burst owns the radio through every
     * physical copy. A per-copy release must not reopen the role scanner. */
    if (mesh_c5_enumeration_relay_burst_active()) {
        return;
    }
    if (!radio_guard_uwb_rearm_allowed()) {
        LOG_ERR("mesh RX rearm suppressed while radio parking is poisoned: %d",
                radio_guard_uwb_poison_error());
        return;
    }
    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        return;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy) {
        ret = mesh_start_uwb_rx("anchor mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart anchor UWB mesh RX: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = mesh_start_uwb_rx("gateway mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart gateway UWB mesh RX: %d", ret);
        }
    }
}

int mesh_transport_pause_preserving_queued(void)
{
    radio_guard_uwb_admission_pause();
    atomic_set(&mesh_transport_paused_state, 1);
    mesh_stop_role_scan();
    (void)k_work_cancel(&mesh_rx_work);
    (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
    (void)mesh_cancel_delayable(&mesh_route_waiting_work);
    (void)mesh_cancel_delayable(&mesh_c5_flood_work);
    (void)mesh_cancel_delayable(&mesh_route_discovery_work);
    (void)mesh_cancel_delayable(&mesh_event_negotiation_retry_work);
#if DEVICE_ROLE == ROLE_ANCHOR
    (void)mesh_cancel_delayable(&report_tx_work);
    (void)mesh_cancel_delayable(&mesh_route_request_action_work);
#endif
    return 0;
}

bool mesh_transport_pause_active(void)
{
    return mesh_transport_paused();
}

bool mesh_transport_quiesced(void)
{
    return mesh_transport_paused() &&
           !radio_guard_uwb_busy() &&
           /* A paused EVENT_ACCEPT retry is retained RAM custody, not live
            * RF.  Keep it parked for resume, but wait for an attempt that is
            * physically inside the response send before lending the radio. */
           atomic_get(&mesh_rx_response_active_state) == 0 &&
           atomic_get(&mesh_rx_handler_active_state) == 0;
}

void mesh_transport_resume(void)
{
    bool c5_flood_pending_now;
    uint32_t route_discovery_delay_ms;
    bool route_discovery_pending_now;

    if (!mesh_transport_paused()) {
        return;
    }
    atomic_set(&mesh_transport_paused_state, 0);
    radio_guard_uwb_admission_resume();

#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_command_result_tracking_resume();
#endif
    if (k_msgq_num_used_get(&mesh_rx_msgq) > 0u) {
        (void)mesh_submit_owned_work(&mesh_rx_work,
                                     "transport-resume-rx");
    }
    if (mesh_relay_tx_active(&mesh_runtime) ||
        mesh_ch9_tx_pending_is_active() ||
        mesh_relay_result_bundle_pending(&mesh_runtime)) {
        (void)mesh_schedule_tx_timeout();
    }
    if (mesh_route_waiting_tx_valid) {
        mesh_schedule_route_waiting_retry_after("transport-resume", 0u);
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    if (report_tx_queue_used() > 0u) {
        report_tx_schedule(0u);
    }
    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    if (mesh_route_request_action_pending) {
        uint32_t delay_ms = app_mesh_route_request_defer_delay_ms(
            k_uptime_get_32(),
            mesh_route_request_action_tx.earliest_tx_ms);

        (void)mesh_reschedule_owned_work(
            &mesh_route_request_action_work,
            delay_ms,
            "transport-resume-route-action");
    }
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
#endif
    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    c5_flood_pending_now = mesh_c5_flood_deferred.valid ||
                           mesh_route_adv_deferred.valid;
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);
    if (c5_flood_pending_now) {
        (void)mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                         0u,
                                         "transport-resume-c5-flood");
    }
    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    route_discovery_pending_now =
        app_mesh_async_route_request_retry_delay_ms(
            &mesh_route_discovery_request,
            k_uptime_get_32(),
            &route_discovery_delay_ms);
    if (route_discovery_pending_now) {
        (void)mesh_reschedule_owned_work(&mesh_route_discovery_work,
                                         route_discovery_delay_ms,
                                         "transport-resume-route-discovery");
    }
    k_mutex_unlock(&mesh_route_discovery_lock);
    (void)mesh_event_negotiation_schedule_next();
    if (DEVICE_ROLE == ROLE_CLICKER &&
        mesh_clicker_on_demand_rx_required()) {
        (void)mesh_schedule_uwb_rx(0u);
    }
    mesh_restart_role_scan();
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_ch9_slot_tx_begin(struct mesh_ch9_slot_tx_context *ctx)
{
    int parking_ret;
    int release_ret;
    int ret;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->active) {
        return 0;
    }

    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                     "mesh channel9 batch TX",
                                     &ctx->radio_lease);
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }

    ctx->uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_mesh_payload_mode();
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_SLOT_CONFIG ret=%d now=%u\n",
                            ret,
                            k_uptime_get_32());
    }
    if (ret < 0) {
        release_ret = radio_guard_uwb_release_begin(&ctx->radio_lease);
        if (release_ret == 0) {
            parking_ret = mesh_radio_standby_with_bounded_recovery(
                "ch9-slot-config-fail");
            release_ret = radio_guard_uwb_release_finish(
                &ctx->radio_lease, parking_ret);
        }
        mesh_report_note_anchor_uwb_awake_since(ctx->uwb_window_start_ms, 0u);
        if (release_ret < 0) {
            LOG_ERR("mesh channel-9 configuration cleanup failed: %d",
                    release_ret);
            ctx->uwb_window_start_ms = -1;
            return release_ret;
        }
        mesh_restart_role_scan();
        ctx->uwb_window_start_ms = -1;
        return ret;
    }

    ctx->active = true;
    return 0;
}

static int mesh_ch9_slot_tx_end(struct mesh_ch9_slot_tx_context *ctx)
{
    bool functional_tx_completed;
    int parking_ret;
    int release_ret;

    if (ctx == NULL || !ctx->active) {
        return 0;
    }

    functional_tx_completed = ctx->functional_tx_completed;
    release_ret = radio_guard_uwb_release_begin(&ctx->radio_lease);
    if (release_ret == 0) {
        parking_ret = mesh_release_radio_after_mesh_turn(true, "ch9-slot-tx");
        release_ret = radio_guard_uwb_release_finish(&ctx->radio_lease,
                                                      parking_ret);
    }
    mesh_report_note_anchor_uwb_awake_since(ctx->uwb_window_start_ms, 0u);
    if (release_ret < 0) {
        LOG_ERR("mesh channel-9 slot radio release failed: %d",
                release_ret);
        return release_ret;
    }
    mesh_restart_role_scan();
    ctx->active = false;
    ctx->functional_tx_completed = false;
    ctx->uwb_window_start_ms = -1;
    if (functional_tx_completed) {
        app_watchdog_note_radio_progress();
    }
    return 0;
}
#endif

/*
 * Encode from the synchronous caller's immutable payload while refreshing
 * only the transmitted packet snapshot. The old gateway/clicker path copied
 * an entire 958-byte-capacity mesh_outbound into BSS just to change packet
 * age. If the optional flood-age TLV is present, update the encoded frame and
 * repair both nested CRCs; the caller-owned payload remains untouched.
 *
 * Every entry point is synchronous: direct callers retain an immutable
 * envelope through return, while queue-backed paths first copy into their
 * worker-owned scratch. mesh_send_scratch_lock protects the shared encoded
 * frame, and no borrowed payload pointer survives this encode call.
 */
static int mesh_encode_outbound_tx_snapshot(
    const struct mesh_outbound *out,
    uint64_t frame_airtime_us,
    struct proto_packet *tx_packet,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len)
{
    uint32_t now_ms;
    int ret;

    if (out == NULL || tx_packet == NULL || frame == NULL ||
        frame_len == NULL ||
        out->payload_len != out->packet.payload_len ||
        out->payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }

    now_ms = k_uptime_get_32();
    *tx_packet = out->packet;
    tx_packet->message_age_ms = mesh_packet_age_at_air_arrival(
        out->packet.message_age_ms,
        out->queued_at_ms,
        out->queued_at_valid,
        now_ms,
        frame_airtime_us);
    ret = uwb_mesh_frame_encode(NETWORK_ID,
                                DEVICE_ID,
                                out->next_hop_id,
                                tx_packet,
                                out->payload,
                                frame,
                                frame_cap,
                                frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    return PROTO_OK;
}

static uint64_t mesh_transmitted_frame_airtime_us(
    uint8_t radio_channel,
    bool channel5_extended_control,
    size_t frame_len)
{
    enum dwm3000_timing_phy phy;
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        phy = DWM3000_TIMING_PHY_CH9_MESH;
    } else if (radio_channel == UWB_CHANNEL_WAKE_CONTACT) {
        phy = channel5_extended_control ?
            DWM3000_TIMING_PHY_CH5_MESH_CONTROL :
            DWM3000_TIMING_PHY_CH5_WAKE;
    } else {
        return 0u;
    }
    return dwm3000_timing_airtime_us_ceil(phy, frame_len);
}

static int mesh_send_outbound_preconfigured_ch9_locked_until(
    const struct mesh_outbound *out,
    const char *reason,
    size_t *frame_len_out,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation)
{
    struct proto_packet tx_packet;
    struct dwm3000_tx_observation driver_observation = {0};
    uint8_t *frame = mesh_send_scratch_frame;
    size_t frame_len = 0u;
    int ret;

    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }

    ret = mesh_encode_outbound_tx_snapshot(out,
                                           0u,
                                           &tx_packet,
                                           frame,
                                           UWB_MESH_MAX_FRAME_LEN,
                                           &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }

    if (out->earliest_tx_valid) {
        mesh_wait_until_ms(out->earliest_tx_ms);
    }
    ret = mesh_encode_outbound_tx_snapshot(
        out,
        mesh_transmitted_frame_airtime_us(
            UWB_CHANNEL_MESH_PAYLOAD, false, frame_len),
        &tx_packet,
        frame,
        UWB_MESH_MAX_FRAME_LEN,
        &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh final frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_SEND_AT now=%u earliest=%u msg=%02x len=%u\n",
                            k_uptime_get_32(),
                            out->earliest_tx_ms,
                            tx_packet.msg_type,
                            (unsigned int)frame_len);
    }

    uint32_t send_begin_ms = k_uptime_get_32();

    ret = dwm3000_driver_send_frame_tracked_until(
        frame,
        frame_len,
        UWB_MESH_TX_TIMEOUT_MS,
        absolute_deadline_ms,
        &driver_observation);
    if (observation != NULL) {
        observation->rf_started = driver_observation.rf_started;
        observation->rf_started_at_ms =
            driver_observation.rf_started_at_ms;
        observation->tx_completed = driver_observation.tx_completed;
        observation->tx_completed_at_ms =
            driver_observation.tx_completed_at_ms;
    }
    if (ret < 0) {
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                tx_packet.msg_type,
                (unsigned long long)out->next_hop_id,
                (unsigned int)frame_len,
                ret);
        return ret;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t send_done_ms = k_uptime_get_32();

        status_debug_printf("DBG_CH9_TX_DONE now=%u dur=%u len=%u msg=%02x\n",
                            send_done_ms,
                            uptime_ms_until_deadline(send_begin_ms, send_done_ms),
                            (unsigned int)frame_len,
                            tx_packet.msg_type);
    }

    status_debug_tx_mesh_frame_sent_pulse();
    status_debug_uwb_tx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
    if (tx_packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_gateway_ack_tx_pulse();
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u age_ms=%u channel=%u frame_len=%u",
                reason,
                tx_packet.msg_type,
                (unsigned long long)tx_packet.src_id,
                (unsigned long long)tx_packet.dst_id,
                (unsigned long long)out->next_hop_id,
                tx_packet.seq,
                tx_packet.ttl,
                tx_packet.message_age_ms,
                UWB_CHANNEL_MESH_PAYLOAD,
                (unsigned int)frame_len);
    }
    if (frame_len_out != NULL) {
        *frame_len_out = frame_len;
    }
    return 0;
}

static int mesh_send_outbound_preconfigured_ch9_locked(
    const struct mesh_outbound *out,
    const char *reason,
    size_t *frame_len_out)
{
    return mesh_send_outbound_preconfigured_ch9_locked_until(
        out, reason, frame_len_out, 0u, NULL);
}

static int mesh_send_outbound_with_release_on_channel_until(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    bool *rf_started_out,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation,
    uint8_t forced_radio_channel,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent c5_tx_intent)
{
    struct proto_packet tx_packet;
    struct dwm3000_tx_observation driver_observation = {0};
    struct radio_guard_uwb_lease radio_lease = {0};
    uint8_t *frame = mesh_send_scratch_frame;
    size_t frame_len = 0u;
    uint32_t encoded_at_ms = 0u;
    int64_t uwb_window_start_ms = -1;
    bool channel5_extended_control = false;
    bool gateway_enumeration_quick_burst_owner;
    uint8_t radio_channel;
    int parking_ret = 0;
    int release_ret = 0;
    int ret;
    bool collection_recovery_receipt_owner;

    if (rf_started_out != NULL) {
        *rf_started_out = false;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (out == NULL) {
        return -EINVAL;
    }
    radio_channel = forced_radio_channel == 0u ? out->radio_channel :
                    forced_radio_channel;
    if (radio_channel != UWB_CHANNEL_WAKE_CONTACT &&
        radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        LOG_ERR("mesh TX rejected invalid radio channel: channel=%u reason=%s",
                radio_channel,
                reason == NULL ? "unknown" : reason);
        return -EINVAL;
    }
    collection_recovery_receipt_owner =
        radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        mesh_gateway_collection_recovery_receipt_c5_allowed(out);
    gateway_enumeration_quick_burst_owner =
        radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        mesh_c5_enumeration_relay_burst_active() &&
        mesh_c5_gateway_enumeration_quick_copy_burst(out);
    if (!collection_recovery_receipt_owner &&
        !gateway_enumeration_quick_burst_owner &&
        !mesh_coordinator_mesh_work_allowed(reason)) {
        return -EBUSY;
    }
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
        out->earliest_tx_valid) {
        uint32_t guard_start_ms =
            out->earliest_tx_ms - MESH_CH9_TX_CONFIG_GUARD_MS;
        if (!uptime_deadline_reached(k_uptime_get_32(), guard_start_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_PRE_GUARD_WAIT now=%u guard=%u earliest=%u msg=%02x\n",
                                    k_uptime_get_32(),
                                    guard_start_ms,
                                    out->earliest_tx_ms,
                                    out->packet.msg_type);
            }
            mesh_wait_until_ms(guard_start_ms);
        }
        if (!mesh_coordinator_mesh_work_allowed(reason)) {
            return -EBUSY;
        }
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    ret = mesh_encode_outbound_tx_snapshot(out,
                                           0u,
                                           &tx_packet,
                                           frame,
                                           UWB_MESH_MAX_FRAME_LEN,
                                           &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        ret = -EINVAL;
        goto out_unlock;
    }
    channel5_extended_control =
        radio_channel != UWB_CHANNEL_MESH_PAYLOAD &&
        app_mesh_c5_control_uses_extended_phr(
            tx_packet.msg_type,
            frame_len,
            MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN);

    if (radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        !gateway_enumeration_quick_burst_owner &&
        !mesh_coordinator_c5_tx_allowed_authorized_intent(
            reason, out, authorization, c5_tx_intent)) {
        ret = -EBUSY;
        goto out_unlock;
    }

    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                     "mesh UWB TX",
                                     &radio_lease);
    if (ret < 0) {
        mesh_restart_role_scan();
        goto out_unlock;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
          dwm3000_driver_configure_mesh_payload_mode() :
          channel5_extended_control ?
          dwm3000_driver_configure_wake_mesh_control_mode() :
          dwm3000_driver_configure_wake_mode();
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_CONFIG ret=%d now=%u earliest=%u msg=%02x\n",
                                ret,
                                k_uptime_get_32(),
                                out->earliest_tx_ms,
                                tx_packet.msg_type);
        }
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_C5_TX_CONFIG ret=%d ext=%u len=%u msg=%02x reason=%s\n",
                            ret,
                            channel5_extended_control ? 1u : 0u,
                            (unsigned int)frame_len,
                            tx_packet.msg_type,
                            reason == NULL ? "mesh" : reason);
    }
    if (ret == 0) {
        if (radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
            !gateway_enumeration_quick_burst_owner &&
            !mesh_coordinator_c5_tx_allowed_authorized_intent(
                reason, out, authorization, c5_tx_intent)) {
            ret = -EBUSY;
        }
    }
    if (ret == 0) {
        if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            out->earliest_tx_valid) {
            mesh_wait_until_ms(out->earliest_tx_ms);
        }
        ret = mesh_encode_outbound_tx_snapshot(
            out,
            mesh_transmitted_frame_airtime_us(
                radio_channel, channel5_extended_control, frame_len),
            &tx_packet,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len);
        encoded_at_ms = k_uptime_get_32();
        if (ret != PROTO_OK) {
            LOG_WRN("mesh final frame encode failed for %s: %d", reason, ret);
            ret = -EINVAL;
        }
    }
    if (ret == 0) {
        if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_SEND_AT now=%u earliest=%u msg=%02x len=%u\n",
                                k_uptime_get_32(),
                                out->earliest_tx_ms,
                                tx_packet.msg_type,
                                (unsigned int)frame_len);
        }
        ret = dwm3000_driver_send_frame_tracked_until(
            frame,
            frame_len,
            UWB_MESH_TX_TIMEOUT_MS,
            absolute_deadline_ms,
            &driver_observation);
        if (rf_started_out != NULL) {
            *rf_started_out = driver_observation.rf_started;
        }
        if (observation != NULL) {
            observation->rf_started =
                driver_observation.rf_started;
            observation->rf_started_at_ms =
                driver_observation.rf_started_at_ms;
            observation->tx_completed =
                driver_observation.tx_completed;
            observation->tx_completed_at_ms =
                driver_observation.tx_completed_at_ms;
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_MESH_TX_AGE msg=%02x seq=%u age=%u enc=%u rf=%llu gap=%lld air=%u\n",
            tx_packet.msg_type,
            tx_packet.seq,
            tx_packet.message_age_ms,
            encoded_at_ms,
            (unsigned long long)driver_observation.rf_started_at_ms,
            driver_observation.rf_started ?
                (long long)(driver_observation.rf_started_at_ms -
                            encoded_at_ms) : -1ll,
            (unsigned int)((mesh_transmitted_frame_airtime_us(
                radio_channel, channel5_extended_control, frame_len) + 999u) /
                1000u));
        status_debug_printf(
            "DBG_MESH_TX_STAGE stage=start msg=%02x seq=%u ch=%u seen=%u at=%llu\n",
            tx_packet.msg_type,
            tx_packet.seq,
            radio_channel,
            driver_observation.rf_started ? 1u : 0u,
            (unsigned long long)driver_observation.rf_started_at_ms);
        status_debug_printf(
            "DBG_MESH_TX_STAGE stage=complete msg=%02x seq=%u ch=%u seen=%u at=%llu ret=%d\n",
            tx_packet.msg_type,
            tx_packet.seq,
            radio_channel,
            driver_observation.tx_completed ? 1u : 0u,
            (unsigned long long)driver_observation.tx_completed_at_ms,
            ret);
    }
    release_ret = radio_guard_uwb_release_begin(&radio_lease);
    if (release_ret == 0) {
        if (ret == 0 && radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            if (release_policy == MESH_RADIO_RELEASE_IDLE) {
                parking_ret = mesh_radio_idle_with_bounded_recovery(
                    reason == NULL ? "mesh-send-idle" : reason);
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_CH9_TX_RELEASE_IDLE msg=%02x seq=%u reason=%s ret=%d\n",
                                        tx_packet.msg_type,
                                        tx_packet.seq,
                                        reason == NULL ? "mesh" : reason,
                                        parking_ret);
                }
            } else {
                parking_ret = mesh_release_radio_after_mesh_turn(true, reason);
            }
        } else {
            parking_ret = mesh_radio_standby_with_bounded_recovery(
                reason == NULL ? "mesh-send-release" : reason);
        }
        release_ret = radio_guard_uwb_release_finish(&radio_lease, parking_ret);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_MESH_TX_STAGE stage=park msg=%02x seq=%u ch=%u park_ret=%d release_ret=%d\n",
            tx_packet.msg_type,
            tx_packet.seq,
            radio_channel,
            parking_ret,
            release_ret);
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    if (release_ret < 0) {
        LOG_ERR("mesh UWB TX radio release failed for %s: msg=0x%02x next=0x%016llx ret=%d",
                reason,
                tx_packet.msg_type,
                (unsigned long long)out->next_hop_id,
                release_ret);
        ret = release_ret;
        goto out_unlock;
    }
    mesh_restart_role_scan();
    if (ret < 0) {
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                tx_packet.msg_type,
                (unsigned long long)out->next_hop_id,
                (unsigned int)frame_len,
                ret);
        goto out_unlock;
    }

    app_watchdog_note_radio_progress();
    status_debug_tx_mesh_frame_sent_pulse();
    status_debug_uwb_tx_channel_pulse(radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                                      UWB_CHANNEL_MESH_PAYLOAD :
                                      UWB_CHANNEL_WAKE_CONTACT);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        tx_packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_SENT now=%u seq=%u\n",
                            k_uptime_get_32(),
                            tx_packet.seq);
    }
    if (tx_packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_gateway_ack_tx_pulse();
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        tx_packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_RETURN now=%u seq=%u\n",
                            k_uptime_get_32(),
                            tx_packet.seq);
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u age_ms=%u channel=%u frame_len=%u",
                reason,
                tx_packet.msg_type,
                (unsigned long long)tx_packet.src_id,
                (unsigned long long)tx_packet.dst_id,
                (unsigned long long)out->next_hop_id,
                tx_packet.seq,
                tx_packet.ttl,
                tx_packet.message_age_ms,
                radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
                (unsigned int)frame_len);
    }
    ret = 0;

out_unlock:
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

static int mesh_send_outbound_with_release_on_channel(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    bool *rf_started_out,
    uint8_t forced_radio_channel)
{
    return mesh_send_outbound_with_release_on_channel_until(
        out,
        reason,
        release_policy,
        rf_started_out,
        0u,
        NULL,
        forced_radio_channel,
        NULL,
        FW_C5_TX_INTENT_BACKGROUND);
}

static int mesh_send_outbound_with_release(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    bool *rf_started_out)
{
    return mesh_send_outbound_with_release_on_channel(out,
                                                      reason,
                                                      release_policy,
                                                      rf_started_out,
                                                      0u);
}

static int mesh_send_outbound_with_release_until(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation)
{
    return mesh_send_outbound_with_release_on_channel_until(
        out,
        reason,
        release_policy,
        NULL,
        absolute_deadline_ms,
        observation,
        0u,
        NULL,
        FW_C5_TX_INTENT_BACKGROUND);
}

int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    return mesh_send_outbound_with_release(out,
                                           reason,
                                           MESH_RADIO_RELEASE_STANDBY,
                                           NULL);
}

static int mesh_send_outbound_authorized(
    const struct mesh_outbound *out,
    const char *reason,
    const struct app_mesh_c5_tx_authorization_token *authorization)
{
    return mesh_send_outbound_with_release_on_channel_until(
        out,
        reason,
        MESH_RADIO_RELEASE_STANDBY,
        NULL,
        0u,
        NULL,
        0u,
        authorization,
        FW_C5_TX_INTENT_BACKGROUND);
}

static int mesh_send_outbound_causal_response(
    const struct mesh_outbound *out,
    const char *reason)
{
    return mesh_send_outbound_with_release_on_channel_until(
        out,
        reason,
        MESH_RADIO_RELEASE_STANDBY,
        NULL,
        0u,
        NULL,
        UWB_CHANNEL_WAKE_CONTACT,
        NULL,
        FW_C5_TX_INTENT_CAUSAL_RESPONSE);
}

static int mesh_send_outbound_keep_channel9_awake(const struct mesh_outbound *out,
                                                  const char *reason,
                                                  bool *rf_started_out)
{
    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }

    return mesh_send_outbound_with_release(out,
                                           reason,
                                           MESH_RADIO_RELEASE_IDLE,
                                           rf_started_out);
}

/*
 * A gateway normally advertises continuous Channel-9 RX ownership even in
 * the short interval after its RX worker has released the driver.  A causal
 * ACK produced from that worker must therefore publish the same explicit
 * RX-to-control handoff used by deferred node-communication responses before
 * it enters the generic TX path.  Without the handoff, the coordinator
 * rejects every immediate ACK as gateway-rx and the fallback queue can fill
 * with replies to repeated direct route probes.
 */
static int mesh_send_causal_channel9_response(
    const struct mesh_outbound *out,
    const char *reason,
    bool *rf_started_out)
{
    bool control_handoff_started = false;
    bool abort_scan = false;
    int ret;

    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        control_handoff_started = mesh_rx_handoff_begin_control(&abort_scan);
        if (!control_handoff_started) {
            return -EBUSY;
        }
        mesh_stop_role_scan();
        if (abort_scan) {
            dwm3000_driver_request_receive_abort(
                DWM3000_RECEIVE_ABORT_MESH_CONTROL);
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            goto out;
        }
        if (out->packet.msg_type == MSG_GATEWAY_ACK) {
            /*
             * Hold the RX-to-control handoff across the peer retune guard.
             * Sleeping before this reservation lets a later receive turn
             * overtake the causal ACK that owns the current response.
             */
            status_debug_printf("DBG_GATEWAY_ACK_GUARD ms=%u seq=%u owner=control\n",
                                MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                out->packet.seq);
            k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
        }
    }

    ret = mesh_send_outbound_keep_channel9_awake(out, reason,
                                                 rf_started_out);

out:
    if (control_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
    return ret;
}

static int mesh_send_outbound_keep_channel9_awake_until(
    const struct mesh_outbound *out,
    const char *reason,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation)
{
    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }

    return mesh_send_outbound_with_release_until(
        out,
        reason,
        MESH_RADIO_RELEASE_IDLE,
        absolute_deadline_ms,
        observation);
}

static void mesh_wait_for_c5_control_followup_turnaround(
    uint8_t msg_type,
    const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    status_debug_printf(
        "DBG_C5_CONTROL_FOLLOWUP_TURNAROUND delay=%u msg=%02x reason=%s\n",
        MESH_CONTROL_FOLLOWUP_TURNAROUND_MS,
        msg_type,
        reason == NULL ? "c5-control" : reason);
    k_msleep(MESH_CONTROL_FOLLOWUP_TURNAROUND_MS);
}

static int mesh_send_c5_control_attempt(
    const struct mesh_outbound *out,
    uint8_t purpose,
    enum mesh_c5_control_send_mode mode,
    const char *reason,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent c5_tx_intent,
    bool assignment_parent_protocol_awake)
{
    const struct mesh_outbound *tx = out;
    uint64_t peer_id;
    uint64_t wake_target_id;
    uint64_t wake_rf_started_at_ms = 0u;
    bool active_exchange = false;
    bool assignment_parent_awake = false;
    bool wake_rf_started = false;
    uint32_t now_ms;
    int ret;

    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (out == NULL || purpose == 0u ||
        (mode != MESH_C5_CONTROL_WAKE_IF_NEEDED &&
         mode != MESH_C5_CONTROL_ACCEPTED_EXCHANGE)) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    /* The synchronous caller retains its immutable envelope through return. */
    peer_id = mesh_id_is_unicast(tx->next_hop_id) ? tx->next_hop_id : 0u;
    now_ms = k_uptime_get_32();
    if (peer_id != 0u) {
        active_exchange = mesh_c5_contact_active(peer_id,
                                                  purpose,
                                                  now_ms);
        assignment_parent_awake =
            purpose == C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION &&
            assignment_parent_protocol_awake &&
            tx->packet.msg_type == MSG_MESH_EVENT_PROPOSE;
    }

    if (mode == MESH_C5_CONTROL_ACCEPTED_EXCHANGE && !active_exchange) {
        LOG_WRN("mesh C5 control send rejected without accepted exchange: msg=0x%02x next=0x%016llx purpose=%s reason=%s",
                tx->packet.msg_type,
                (unsigned long long)tx->next_hop_id,
                mesh_c5_contact_purpose_name(purpose),
                reason == NULL ? "c5-control" : reason);
        ret = -ENOTCONN;
        goto out_unlock;
    }

    if (mode == MESH_C5_CONTROL_WAKE_IF_NEEDED && !active_exchange &&
        !assignment_parent_awake) {
        wake_target_id = peer_id != 0u ? peer_id : MESH_BROADCAST_ID;
        ret = mesh_send_route_wake_train(wake_target_id, NULL, NULL,
                                         purpose, reason, tx,
                                         authorization,
                                         c5_tx_intent,
                                         &wake_rf_started,
                                         &wake_rf_started_at_ms);
        if (ret < 0) {
            mesh_restart_role_scan();
            LOG_WRN("mesh C5 control wake train failed: msg=0x%02x next=0x%016llx ret=%d reason=%s",
                    tx->packet.msg_type,
                    (unsigned long long)tx->next_hop_id,
                    ret,
                    reason == NULL ? "c5-control" : reason);
            goto out_unlock;
        }
        mesh_wait_for_c5_control_followup_turnaround(tx->packet.msg_type,
                                                     reason);
    } else if (assignment_parent_awake) {
        status_debug_printf(
            "DBG_ENUM_EVENT_PROPOSE_DIRECT peer=0x%llx reason=%s\n",
            (unsigned long long)peer_id,
            reason == NULL ? "assignment-parent-awake" : reason);
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    if (tx->packet.msg_type == MSG_MESH_EVENT_PROPOSE) {
        ret = mesh_event_propose_prepare_immediate_send(tx);
        if (ret < 0) {
            goto out_unlock;
        }
    }
#endif

    if (peer_id != 0u) {
        mesh_c5_contact_exchange(peer_id,
                                 purpose,
                                 mesh_c5_exchange_expires_at(purpose),
                                 reason == NULL ? "c5-control" : reason);
    }

    ret = mesh_send_outbound_with_release_on_channel_until(
        tx,
        reason == NULL ? "c5-control" : reason,
        MESH_RADIO_RELEASE_STANDBY,
        NULL,
        absolute_deadline_ms,
        observation,
        UWB_CHANNEL_WAKE_CONTACT,
        authorization,
        c5_tx_intent);
    if (ret == 0 && peer_id != 0u) {
        mesh_c5_contact_exchange(peer_id,
                                 purpose,
                                 mesh_c5_exchange_expires_at(purpose),
                                 reason == NULL ? "c5-control" : reason);
    }
out_unlock:
    if (observation != NULL) {
        if (wake_rf_started) {
            observation->rf_started = true;
            observation->rf_started_at_ms = wake_rf_started_at_ms;
        }
        if (observation->result_at_ms == 0u) {
            observation->result_at_ms = (uint64_t)k_uptime_get();
        }
    }
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason)
{
    return mesh_send_c5_control_attempt(out, purpose, mode, reason, 0u, NULL,
                                        NULL,
                                        FW_C5_TX_INTENT_BACKGROUND,
                                        false);
}

static int mesh_send_c5_causal_response(
    const struct mesh_outbound *out,
    uint8_t purpose,
    enum mesh_c5_control_send_mode mode,
    const char *reason)
{
    return mesh_send_c5_control_attempt(
        out, purpose, mode, reason, 0u, NULL, NULL,
        FW_C5_TX_INTENT_CAUSAL_RESPONSE, false);
}

static uint32_t mesh_c5_flood_now_ms(void *ctx)
{
    ARG_UNUSED(ctx);

    return k_uptime_get_32();
}

static void mesh_c5_flood_sleep_until_ms(uint32_t due_ms, void *ctx)
{
    const struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    int64_t uwb_window_start_ms;

    /* A gateway-command relay burst is itself the highest-priority Channel-5
     * work.  Listening between its closely spaced copies lets an echo of the
     * same command abort the remaining redundancy, so keep that short gap as
     * a plain wait.  Background floods retain their polite RX yield. */
    if (flood_ctx != NULL && flood_ctx->response_priority) {
        mesh_wait_until_ms(due_ms);
        return;
    }

    while (true) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t remaining_ms;
        uint32_t listen_ms;
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        bool valid_mesh_frame = false;
        struct radio_guard_uwb_lease radio_lease = {0};
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        int release_ret;
        int ret;

        if (k_msgq_num_used_get(&mesh_rx_msgq) != 0u) {
            return;
        }

        if (uptime_deadline_reached(now_ms, due_ms)) {
            break;
        }
        remaining_ms = uptime_ms_until_deadline(now_ms, due_ms);
        listen_ms = MIN(remaining_ms, MESH_C5_FLOOD_DELAY_LISTEN_SLICE_MS);

        mesh_stop_role_scan();
        ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                         "mesh C5 flood delay listen",
                                         &radio_lease);
        if (ret < 0) {
            mesh_restart_role_scan();
            mesh_wait_until_ms(due_ms);
            return;
        }
        uwb_window_start_ms = k_uptime_get();
        ret = dwm3000_driver_configure_wake_mesh_control_mode();
        if (ret == 0) {
            ret = dwm3000_driver_receive_frame_continuous(listen_ms,
                                                          mesh_uwb_rx_frame,
                                                          sizeof(mesh_uwb_rx_frame),
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
        }
        release_ret = radio_guard_uwb_release_begin(&radio_lease);
        if (release_ret == 0) {
            int parking_ret = mesh_radio_standby_with_bounded_recovery(
                "c5-flood-delay-listen");

            release_ret = radio_guard_uwb_release_finish(&radio_lease,
                                                          parking_ret);
        }
        mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
        if (release_ret < 0) {
            LOG_ERR("mesh C5 flood delay radio release failed: %d",
                    release_ret);
            return;
        }
        mesh_restart_role_scan();

        if (ret == 0) {
            bool queued = mesh_queue_from_frame_at_internal(
                mesh_uwb_rx_frame,
                frame_len,
                quality,
                UWB_CHANNEL_WAKE_CONTACT,
                k_uptime_get_32(),
                0u,
                NULL,
                0u,
                false,
                &valid_mesh_frame,
                NULL);

            if (queued) {
                (void)mesh_submit_owned_work(&mesh_rx_work,
                                             "c5-delay-rx");
            }
            if (queued ||
                (valid_mesh_frame &&
                 k_msgq_num_used_get(&mesh_rx_msgq) != 0u)) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf(
                        "DBG_C5_FLOOD_DELAY_RX_YIELD queued=%u q=%u rem=%u\n",
                        queued ? 1u : 0u,
                        k_msgq_num_used_get(&mesh_rx_msgq),
                        remaining_ms);
                }
                return;
            }
        } else if (ret < 0 && ret != -ETIMEDOUT &&
                   IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_C5_FLOOD_DELAY_RX_CONTINUE ret=%d fail=%u rem=%u\n",
                                ret,
                                (unsigned int)rx_failure,
                                remaining_ms);
        }
    }
}

static bool mesh_c5_flood_defer_active_cb(void *ctx)
{
    const struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    struct fw_radio_activity_decision coordinator_decision;
    struct app_mesh_c5_flood_priority_state state = {
        .response_priority = flood_ctx != NULL && flood_ctx->response_priority,
    };
    bool collection_recovery_receipt_owner;
    uint32_t now_ms;

    /* An admitted gateway command is the Channel-5 priority owner. Its
     * bounded hop wave must not surrender its remaining copies to a sibling
     * relay carrying that same command. Physical send authorization is still
     * rechecked at every wake claim and data frame boundary. */
    if (flood_ctx != NULL && flood_ctx->atomic_gateway_control) {
        return false;
    }

    /* A recovered stale collection packet owns the host-receipt completion
     * transaction. Its C5 CLOSED EACK must run before the next queued frame
     * is admitted, even though the coordinator still observes the gateway
     * RX owner that is executing this exact completion. */
    collection_recovery_receipt_owner = flood_ctx != NULL &&
        mesh_gateway_collection_recovery_receipt_c5_allowed(
            flood_ctx->candidate);

    /* Once the gateway starts a compact TABLE/END burst, queued receive work
     * cannot split its three physical copies into a partial logical send. */
    if (flood_ctx != NULL &&
        mesh_c5_enumeration_relay_burst_active() &&
        mesh_c5_gateway_enumeration_quick_copy_burst(
            flood_ctx->candidate)) {
        return false;
    }

    if (mesh_rx_pending_count() > 0u && !collection_recovery_receipt_owner) {
        return true;
    }
    if (flood_ctx != NULL &&
        flood_ctx->c5_tx_intent != FW_C5_TX_INTENT_BACKGROUND) {
        if (!mesh_coordinator_c5_tx_allowed_authorized_intent(
                "c5-flood",
                flood_ctx->candidate,
                NULL,
                (enum fw_c5_tx_intent)flood_ctx->c5_tx_intent)) {
            return true;
        }
    } else {
        mesh_coordinator_decide_now("c5-flood", &coordinator_decision);
        if (!coordinator_decision.c5_tx_allowed &&
            !collection_recovery_receipt_owner) {
            return true;
        }
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        state.anchor_busy = anchor_uwb_window_active();
    }

    now_ms = k_uptime_get_32();
    state.gateway_ch5_preempt = mesh_gateway_route_test_preempt_active(now_ms);

    return app_mesh_c5_flood_should_defer(&state);
}

static bool mesh_c5_flood_quiet_cb(uint32_t sniff_ms, void *ctx)
{
    const struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    struct radio_guard_uwb_lease radio_lease = {0};
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    int release_ret;
    int ret;

    /* Gateway-command relay waves are already hop-scheduled and primary.
     * Listening here lets another relay's copy cancel the admitted sender's
     * redundancy, which is exactly the opposite of the flood contract. */
    if (flood_ctx != NULL && flood_ctx->atomic_gateway_control) {
        return true;
    }

    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                     "mesh C5 flood politeness",
                                     &radio_lease);
    if (ret < 0) {
        mesh_restart_role_scan();
        return false;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame(sniff_ms,
                                           mesh_uwb_rx_frame,
                                           sizeof(mesh_uwb_rx_frame),
                                           &frame_len,
                                           NULL,
                                           NULL);
    }
    release_ret = radio_guard_uwb_release_begin(&radio_lease);
    if (release_ret == 0) {
        int parking_ret = mesh_radio_standby_with_bounded_recovery(
            "c5-flood-politeness");

        release_ret = radio_guard_uwb_release_finish(&radio_lease,
                                                      parking_ret);
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);

    if (release_ret < 0) {
        LOG_ERR("mesh C5 flood politeness radio release failed: %d",
                release_ret);
        return false;
    }
    mesh_restart_role_scan();
    return ret == -ETIMEDOUT;
}

static uint32_t mesh_c5_flood_random_u32(void *ctx)
{
    ARG_UNUSED(ctx);

    return sys_rand32_get();
}

static int mesh_c5_flood_send_cb(const struct mesh_outbound *out, void *ctx)
{
    struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    bool rf_started = false;
    int ret;

    ret = mesh_send_outbound_with_release_on_channel_until(
        out,
        "bounded-c5-flood",
        MESH_RADIO_RELEASE_STANDBY,
        &rf_started,
        flood_ctx == NULL ? 0u : flood_ctx->absolute_deadline_ms,
        flood_ctx == NULL ? NULL : flood_ctx->observation,
        0u,
        NULL,
        flood_ctx == NULL ? FW_C5_TX_INTENT_BACKGROUND :
            (enum fw_c5_tx_intent)flood_ctx->c5_tx_intent);
    if (rf_started && flood_ctx != NULL &&
        flood_ctx->rf_started_out != NULL) {
        *flood_ctx->rf_started_out = true;
    }
    return ret;
}

static bool mesh_c5_flood_same_packet(const struct mesh_outbound *left,
                                      const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == right->packet.msg_type &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq;
}

static uint32_t mesh_c5_flood_deferred_retry_ms(
    const struct mesh_outbound *out,
    struct app_mesh_rf_retry_state *retry_state)
{
    const struct app_mesh_rf_retry_key retry_key =
        mesh_rf_retry_packet_key(
            &out->packet,
            APP_MESH_RF_RETRY_OPERATION_CONTROL_FLOOD);

    return mesh_rf_retry_next_delay_ms(
        retry_state,
        &retry_key,
        APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
        "c5-flood-deferred");
}

bool mesh_c5_flood_work_pending(void)
{
    bool pending;

    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    pending = mesh_c5_flood_deferred.valid ||
              mesh_route_adv_deferred.valid;
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);
    return pending;
}

static void mesh_c5_enumeration_relay_burst_begin(
    const struct mesh_outbound *out)
{
    atomic_inc(&mesh_c5_enumeration_relay_burst_count);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_ENUM_CONTROL_BURST state=begin msg=%02x seq=%u active=%ld\n",
            out == NULL ? 0u : out->packet.msg_type,
            out == NULL ? 0u : out->packet.seq,
            (long)atomic_get(&mesh_c5_enumeration_relay_burst_count));
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    if (radio_guard_uwb_owner_client() ==
        RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_ENUM_RELAY_SCAN_PREEMPT msg=%02x seq=%u active=%ld\n",
                out == NULL ? 0u : out->packet.msg_type,
                out == NULL ? 0u : out->packet.seq,
                (long)atomic_get(&mesh_c5_enumeration_relay_burst_count));
        }
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    }
#endif
}

static void mesh_c5_enumeration_relay_burst_end(void)
{
    atomic_dec(&mesh_c5_enumeration_relay_burst_count);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf(
            "DBG_ENUM_CONTROL_BURST state=end active=%ld\n",
            (long)atomic_get(&mesh_c5_enumeration_relay_burst_count));
    }
}

bool mesh_c5_protocol_flood_work_pending(void)
{
    bool pending;

    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    pending = mesh_c5_enumeration_relay_burst_active() ||
        mesh_c5_flood_deferred.valid ||
        mesh_route_adv_deferred.valid;
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);
    return pending;
}

static int mesh_c5_flood_store_deferred(const struct mesh_outbound *out,
                                        uint8_t purpose,
                                        const char *reason,
                                        bool response_priority,
                                        bool local_deferral)
{
    struct mesh_c5_flood_deferred_entry *entry =
        &mesh_c5_flood_deferred;
    bool abort_background_scan = false;
    uint32_t retry_ms;

    if (out == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&mesh_c5_flood_deferred_lock, K_FOREVER);
    if (out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV &&
        mesh_route_adv_deferred.valid) {
        entry = &mesh_route_adv_deferred;
        if (mesh_c5_flood_same_packet(&entry->outbound, out)) {
            k_mutex_unlock(&mesh_c5_flood_deferred_lock);
            return 0;
        }
        LOG_WRN("mesh route advertisement defer superseded: old_seq=%u new_seq=%u",
                entry->outbound.packet.session_id,
                out->packet.session_id);
    }
    if (mesh_c5_flood_deferred.valid) {
        if (mesh_c5_flood_same_packet(&mesh_c5_flood_deferred.outbound, out)) {
            k_mutex_unlock(&mesh_c5_flood_deferred_lock);
            return 0;
        }
        if (out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV &&
            entry == &mesh_c5_flood_deferred) {
            /*
             * Route freshness is already committed in RAM before this admission. It
             * may not be undone when another response-priority flood owns the
             * general slot, so retain the newest exact advertisement in its
             * dedicated superseding lane.
             */
            entry = &mesh_route_adv_deferred;
            if (entry->valid &&
                mesh_c5_flood_same_packet(&entry->outbound, out)) {
                k_mutex_unlock(&mesh_c5_flood_deferred_lock);
                return 0;
            }
            if (entry->valid) {
                LOG_WRN("mesh route advertisement defer superseded: old_seq=%u new_seq=%u",
                        entry->outbound.packet.session_id,
                        out->packet.session_id);
            }
        } else {
            if (!response_priority ||
                mesh_c5_flood_deferred.response_priority) {
                LOG_WRN("mesh C5 flood defer slot busy: queued=0x%02x rejected=0x%02x priority=%u",
                        mesh_c5_flood_deferred.outbound.packet.msg_type,
                        out->packet.msg_type,
                        response_priority ? 1u : 0u);
                k_mutex_unlock(&mesh_c5_flood_deferred_lock);
                return -ENOSPC;
            }
            LOG_WRN("mesh C5 priority flood displaced non-priority work: queued=0x%02x priority=0x%02x",
                    mesh_c5_flood_deferred.outbound.packet.msg_type,
                    out->packet.msg_type);
        }
    }
    entry->outbound = *out;
    entry->purpose = purpose;
    entry->reason = reason;
    entry->response_priority = response_priority;
    entry->retry_count = 0u;
    app_mesh_rf_retry_reset(&entry->rf_retry);
    entry->queued_at_ms = k_uptime_get_32();
    entry->generation++;
    if (entry->generation == 0u) {
        entry->generation = 1u;
    }
    entry->valid = true;
    retry_ms = local_deferral ? MESH_C5_LOCAL_DEFER_RETRY_MS :
        mesh_c5_flood_deferred_retry_ms(out, &entry->rf_retry);
    entry->retry_count = local_deferral ? 0u : 1u;
    (void)mesh_reschedule_owned_work(&mesh_c5_flood_work,
                                     retry_ms,
                                     "c5-flood-admission");
    if (DEVICE_ROLE == ROLE_ANCHOR && response_priority &&
        radio_guard_uwb_owner_client() ==
            RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN &&
        !anchor_uwb_window_active() &&
        !anchor_click_window_active()) {
        abort_background_scan = true;
    }
    k_mutex_unlock(&mesh_c5_flood_deferred_lock);
    if (abort_background_scan) {
        status_debug_printf(
            "DBG_C5_FLOOD_SCAN_PREEMPT msg=%02x seq=%u\n",
            out->packet.msg_type,
            out->packet.seq);
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    }
    return 0;
}

static bool mesh_c5_flood_purpose_is_priority(uint8_t purpose)
{
    return purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD ||
           purpose == C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH ||
           purpose == C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD;
}

static bool mesh_c5_flood_destination_valid(const struct mesh_outbound *out)
{
    bool broadcast;
    bool targeted_command;

    if (out == NULL) {
        return false;
    }

    broadcast = out->packet.dst_id == MESH_BROADCAST_ID &&
                out->next_hop_id == MESH_BROADCAST_ID;
    targeted_command = mesh_id_is_unicast(out->packet.dst_id) &&
                       mesh_id_is_unicast(out->next_hop_id) &&
                       out->packet.msg_type == MSG_COMMAND;
    return broadcast || targeted_command;
}

static bool mesh_c5_flood_enumeration_identity(
    const struct mesh_outbound *out,
    enum discovery_assignment_phase *phase_out,
    uint32_t *epoch_out,
    uint32_t *operation_budget_ms_out)
{
    enum command_id command_id;
    enum discovery_assignment_phase phase;
    struct operation_policy_set operation_policy;
    uint32_t epoch;
    uint32_t operation_budget_ms;

    if (out == NULL || phase_out == NULL || epoch_out == NULL ||
        operation_budget_ms_out == NULL ||
        DEVICE_ROLE != ROLE_ANCHOR || out->packet.msg_type != MSG_COMMAND ||
        gateway_command_extract_id(out->payload,
                                   out->payload_len,
                                   &command_id) != PROTO_OK ||
        command_id != CMD_ASSIGN_DISCOVERY_SLOTS ||
        discovery_assignment_extract_control_tlvs(out->payload,
                                                  out->payload_len,
                                                  &phase,
                                                  &epoch) != PROTO_OK ||
        (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_END &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_ABORT)) {
        return false;
    }
    operation_policy_set_defaults(&operation_policy);
    if (operation_policy_set_from_tlvs(out->payload,
                                       out->payload_len,
                                       &operation_policy) != PROTO_OK) {
        return false;
    }
    operation_budget_ms = operation_policy.assignment.operation_budget_ms;
    *phase_out = phase;
    *epoch_out = epoch;
    *operation_budget_ms_out = operation_budget_ms;
    return true;
}

static bool mesh_c5_gateway_enumeration_claim(
    const struct mesh_outbound *out)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    enum command_id command_id;
    enum discovery_assignment_phase phase;
    uint32_t epoch;

    return out != NULL &&
           out->packet.msg_type == MSG_COMMAND &&
           gateway_command_extract_id(out->payload,
                                      out->payload_len,
                                      &command_id) == PROTO_OK &&
           command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           discovery_assignment_extract_control_tlvs(out->payload,
                                                     out->payload_len,
                                                     &phase,
                                                     &epoch) == PROTO_OK &&
           phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM && epoch != 0u;
#else
    ARG_UNUSED(out);
    return false;
#endif
}

static bool mesh_c5_compact_scheduled_control(
    const struct mesh_outbound *out)
{
    return out != NULL && out->packet.msg_type == MSG_COMMAND &&
           gateway_command_uses_compact_scheduled_flood(out->payload,
                                                        out->payload_len);
}

static int mesh_send_c5_flood_now_until(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    bool send_wake_train,
    bool response_priority,
    bool single_opportunity,
    const struct app_mesh_command_orchestrator *command_orchestrator,
    struct app_mesh_flood_result *result,
    bool *rf_started_out,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation,
    enum fw_c5_tx_intent c5_tx_intent)
{
    struct mesh_outbound tx;
    struct app_mesh_flood_result aggregate_result = {0};
    struct mesh_c5_flood_tx_context flood_ctx = {
        .rf_started_out = rf_started_out,
        .observation = observation,
        .absolute_deadline_ms = absolute_deadline_ms,
        .response_priority = response_priority,
        .c5_tx_intent = (uint8_t)c5_tx_intent,
    };
    struct app_mesh_flood_ops ops = {
        .now_ms = mesh_c5_flood_now_ms,
        .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
        .defer_active = mesh_c5_flood_defer_active_cb,
        .c5_quiet = mesh_c5_flood_quiet_cb,
        .random_u32 = mesh_c5_flood_random_u32,
        .send = mesh_c5_flood_send_cb,
        .ctx = &flood_ctx,
    };
    const char *log_reason = reason == NULL ? "c5-flood" : reason;
    enum discovery_assignment_phase enumeration_phase =
        (enum discovery_assignment_phase)0;
    uint32_t enumeration_epoch = 0u;
    uint32_t enumeration_operation_budget_ms = 0u;
    uint32_t enumeration_now_ms = 0u;
    bool enumeration_control;
    bool compact_scheduled_control;
    bool compact_primary_control;
    bool atomic_gateway_control;
    bool enumeration_needs_wake = false;
    bool enumeration_burst_owned = false;
    bool gateway_enumeration_quick_copies;
    bool gateway_enumeration_claim;
    uint16_t attempt_count;
    int ret = 0;

    if (rf_started_out != NULL) {
        *rf_started_out = false;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (out == NULL || purpose == 0u ||
        !mesh_c5_flood_destination_valid(out) ||
        out->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        out->packet.msg_type == MSG_ROUTE_REQ) {
        return -EINVAL;
    }

    tx = *out;
    tx.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    flood_ctx.candidate = &tx;
    attempt_count = 1u + tx.flood_retry_count;
    gateway_enumeration_quick_copies =
        mesh_c5_gateway_enumeration_quick_copy_burst(&tx);
    enumeration_control = mesh_c5_flood_enumeration_identity(
        &tx,
        &enumeration_phase,
        &enumeration_epoch,
        &enumeration_operation_budget_ms);
    compact_scheduled_control = mesh_c5_compact_scheduled_control(&tx);
    compact_primary_control = enumeration_control ||
                              compact_scheduled_control;
    if ((gateway_enumeration_quick_copies || compact_primary_control) &&
        absolute_deadline_ms != 0u &&
        (uint64_t)k_uptime_get() >= absolute_deadline_ms) {
        return -ETIMEDOUT;
    }
    if (gateway_enumeration_quick_copies || compact_primary_control) {
        /* Admission observes the logical deadline. Once admitted, the tiny
         * three-copy burst completes atomically instead of expiring midway. */
        flood_ctx.absolute_deadline_ms = 0u;
    }
    atomic_gateway_control =
        purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD;
    flood_ctx.atomic_gateway_control = atomic_gateway_control;
    if (enumeration_control) {
        enumeration_now_ms = k_uptime_get_32();
        (void)protocol_rx_downstream_activation_expire(
            &mesh_enumeration_downstream_activation,
            enumeration_now_ms);
        if (mesh_enumeration_downstream_activation.activated &&
            mesh_enumeration_downstream_activation.generation !=
                enumeration_epoch) {
            if (!discovery_assignment_epoch_strictly_newer(
                    enumeration_epoch,
                    (uint32_t)mesh_enumeration_downstream_activation
                        .generation)) {
                return -ESTALE;
            }
            (void)protocol_rx_downstream_activation_clear(
                &mesh_enumeration_downstream_activation,
                PROTOCOL_RX_OPERATION_ENUMERATION,
                mesh_enumeration_downstream_activation.generation);
        }
        enumeration_needs_wake = protocol_rx_downstream_activation_needs_wake(
            &mesh_enumeration_downstream_activation,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            enumeration_epoch,
            enumeration_now_ms);
        /* The gateway's activation train already placed direct and forced-hop
         * anchors in the gateway-control listener. Relays send only their
         * bounded control copies; another wake train would add latency and
         * violate the single activation-owner contract. */
        send_wake_train = false;
    }
    if (compact_scheduled_control && DEVICE_ROLE == ROLE_ANCHOR) {
        /* START and PLAN share enumeration's one activation owner: the
         * gateway wakes the complete route, then relays only the compact
         * control burst while every anchor is already listening. */
        send_wake_train = false;
    }
    gateway_enumeration_claim =
        mesh_c5_gateway_enumeration_claim(&tx);

    if (compact_primary_control || gateway_enumeration_quick_copies) {
        mesh_c5_enumeration_relay_burst_begin(&tx);
        enumeration_burst_owned = true;
    }

    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        ret = -EAGAIN;
        goto out;
    }

    for (uint16_t attempt = 0u; attempt < attempt_count; attempt++) {
        struct app_mesh_flood_result attempt_result = {0};
        struct app_mesh_flood_progress attempt_progress = {0};
        uint32_t wake_train_ms = send_wake_train ?
            (gateway_enumeration_claim || compact_scheduled_control) ?
                (attempt == 0u ?
                    MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS : 0u) :
                WAKE_ADV_MS : 0u;
        uint32_t first_tx_ms = tx.earliest_tx_valid ? tx.earliest_tx_ms :
                               k_uptime_get_32();
        uint32_t wake_start_ms = wake_train_ms != 0u ?
            first_tx_ms - wake_train_ms : first_tx_ms;
        if (!uptime_deadline_reached(k_uptime_get_32(), wake_start_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_C5_FLOOD_WAIT now=%u wake=%u first=%u msg=%02x attempt=%u/%u reason=%s\n",
                                    k_uptime_get_32(),
                                    wake_start_ms,
                                    first_tx_ms,
                                    tx.packet.msg_type,
                                    attempt + 1u,
                                    attempt_count,
                                    log_reason);
            }
            mesh_wait_until_ms(wake_start_ms);
        }
        if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
            ret = -EAGAIN;
            break;
        }

        if (wake_train_ms != 0u) {
            ret = mesh_send_route_wake_train_with_duration(
                MESH_BROADCAST_ID, NULL, NULL,
                purpose, reason, &tx, NULL,
                c5_tx_intent, wake_train_ms,
                NULL, NULL);
            if (ret < 0) {
                mesh_restart_role_scan();
                LOG_WRN("mesh C5 flood wake train failed: msg=0x%02x ret=%d reason=%s",
                        tx.packet.msg_type,
                        ret,
                        log_reason);
                goto out;
            }
            mesh_wait_for_c5_control_followup_turnaround(tx.packet.msg_type,
                                                         log_reason);
        }

        ret = (single_opportunity || compact_primary_control) ?
            app_mesh_flood_send_opportunity(&tx, &ops, &attempt_result) :
            atomic_gateway_control ?
            app_mesh_flood_send_bounded_resume(&tx,
                                                &ops,
                                                &attempt_progress,
                                                &attempt_result) :
            command_orchestrator == NULL ?
            app_mesh_command_orchestrator_serialize_flood(&tx,
                                                          &ops,
                                                          &attempt_result) :
            app_mesh_command_orchestrator_send_flood(command_orchestrator,
                                                      &ops,
                                                      &attempt_result);
        aggregate_result.sent_count =
            UINT8_MAX - aggregate_result.sent_count < attempt_result.sent_count ?
            UINT8_MAX : aggregate_result.sent_count + attempt_result.sent_count;
        aggregate_result.busy_skip_count =
            UINT8_MAX - aggregate_result.busy_skip_count < attempt_result.busy_skip_count ?
            UINT8_MAX : aggregate_result.busy_skip_count + attempt_result.busy_skip_count;
        aggregate_result.deferred_count =
            UINT8_MAX - aggregate_result.deferred_count < attempt_result.deferred_count ?
            UINT8_MAX : aggregate_result.deferred_count + attempt_result.deferred_count;
        if (attempt == 0u) {
            aggregate_result.first_due_ms = attempt_result.first_due_ms;
        }
        aggregate_result.last_due_ms = attempt_result.last_due_ms;
        if (ret != 0) {
            if (!atomic_gateway_control &&
                !gateway_enumeration_quick_copies &&
                aggregate_result.sent_count > 0u) {
                ret = 0;
            }
            break;
        }
        if (attempt + 1u < attempt_count) {
            uint32_t diversification_ms =
                mesh_flood_copy_diversification_ms(
                    DEVICE_ID, &tx.packet, (uint8_t)(attempt + 1u));
            uint32_t copy_gap_ms;

            if (tx.packet.msg_type == MSG_GATEWAY_ROUTE_ADV ||
                gateway_enumeration_quick_copies) {
                copy_gap_ms = diversification_ms %
                    (MESH_GATEWAY_ROUTE_ADV_COPY_GAP_MAX_MS + 1u);
            } else {
                copy_gap_ms =
                    (compact_primary_control ?
                         MESH_ENUMERATION_RELAY_COPY_GUARD_MS :
                         FLOOD_POST_ROOT_GUARD_MS) +
                    diversification_ms;
            }
            tx.earliest_tx_ms = k_uptime_get_32() + copy_gap_ms;
            tx.earliest_tx_valid = true;
        }
    }

    if (gateway_enumeration_quick_copies && ret == 0 &&
        aggregate_result.sent_count != attempt_count) {
        ret = -EIO;
    }
    if (atomic_gateway_control && ret == 0) {
        uint16_t copies_per_attempt =
            (single_opportunity || compact_primary_control) ? 1u :
            app_mesh_flood_repeat_limit();
        uint16_t expected_copies = attempt_count * copies_per_attempt;

        if (aggregate_result.sent_count != expected_copies) {
            ret = -EIO;
        }
    }

out:
    if (result != NULL) {
        *result = aggregate_result;
    }
    if (enumeration_control && enumeration_needs_wake &&
        aggregate_result.sent_count > 0u) {
        (void)protocol_rx_downstream_activation_mark(
            &mesh_enumeration_downstream_activation,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            enumeration_epoch,
            enumeration_now_ms,
            enumeration_now_ms + enumeration_operation_budget_ms);
    }
    if (enumeration_control &&
        (enumeration_phase == DISCOVERY_ASSIGNMENT_PHASE_END ||
         enumeration_phase == DISCOVERY_ASSIGNMENT_PHASE_ABORT) && ret == 0 &&
        aggregate_result.sent_count == attempt_count) {
        (void)protocol_rx_downstream_activation_clear(
            &mesh_enumeration_downstream_activation,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            enumeration_epoch);
    }
    if (ret == 0) {
        LOG_DBG("mesh bounded C5 flood sent: msg=0x%02x sent=%u busy_skip=%u defer=%u retry=%u reason=%s",
                tx.packet.msg_type,
                aggregate_result.sent_count,
                aggregate_result.busy_skip_count,
                aggregate_result.deferred_count,
                out->flood_retry_count,
                log_reason);
    }
    if (enumeration_burst_owned) {
        mesh_c5_enumeration_relay_burst_end();
    }
    return ret;
}

static int mesh_send_c5_flood_now(const struct mesh_outbound *out,
                                  uint8_t purpose,
                                  const char *reason,
                                  bool send_wake_train,
                                  bool response_priority,
                                  bool single_opportunity,
                                  const struct app_mesh_command_orchestrator *command_orchestrator,
                                  struct app_mesh_flood_result *result,
                                  bool *rf_started_out)
{
    return mesh_send_c5_flood_now_intent(
        out, purpose, reason, send_wake_train, response_priority,
        single_opportunity, command_orchestrator, result, rf_started_out,
        FW_C5_TX_INTENT_BACKGROUND);
}

static int mesh_send_c5_flood_now_intent(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    bool send_wake_train,
    bool response_priority,
    bool single_opportunity,
    const struct app_mesh_command_orchestrator *command_orchestrator,
    struct app_mesh_flood_result *result,
    bool *rf_started_out,
    enum fw_c5_tx_intent c5_tx_intent)
{
    return mesh_send_c5_flood_now_until(
        out,
        purpose,
        reason,
        send_wake_train,
        response_priority,
        single_opportunity,
        command_orchestrator,
        result,
        rf_started_out,
        0u,
        NULL,
        c5_tx_intent);
}

int mesh_try_send_c5_flood(const struct mesh_outbound *out,
                           uint8_t purpose,
                           const char *reason,
                           bool *rf_started)
{
    struct app_mesh_flood_result result = {0};
    bool priority = mesh_c5_flood_purpose_is_priority(purpose);

    if (rf_started != NULL) {
        *rf_started = false;
    }
    return mesh_send_c5_flood_now(out, purpose, reason, true, priority,
                                  false, NULL, &result, rf_started);
}

int mesh_try_send_c5_flood_resume(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result,
    bool *rf_started)
{
    struct mesh_outbound tx;
    struct mesh_c5_flood_tx_context flood_ctx = {
        .rf_started_out = rf_started,
        .response_priority = mesh_c5_flood_purpose_is_priority(purpose),
        .atomic_gateway_control =
            purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
    };
    struct app_mesh_flood_ops ops = {
        .now_ms = mesh_c5_flood_now_ms,
        .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
        .defer_active = mesh_c5_flood_defer_active_cb,
        .c5_quiet = mesh_c5_flood_quiet_cb,
        .random_u32 = mesh_c5_flood_random_u32,
        .send = mesh_c5_flood_send_cb,
        .ctx = &flood_ctx,
    };
    uint32_t first_tx_ms;
    uint32_t wake_start_ms;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (out == NULL || progress == NULL || result == NULL || purpose == 0u ||
        !mesh_c5_flood_destination_valid(out) ||
        out->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        out->packet.msg_type == MSG_ROUTE_REQ || out->flood_retry_count != 0u) {
        return -EINVAL;
    }

    tx = *out;
    tx.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    flood_ctx.candidate = &tx;
    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        *result = progress->result;
        return -EAGAIN;
    }

    first_tx_ms = progress->initialized ? progress->due_ms :
                  tx.earliest_tx_valid ? tx.earliest_tx_ms :
                  k_uptime_get_32();
    wake_start_ms = first_tx_ms - WAKE_ADV_MS;
    if (!uptime_deadline_reached(k_uptime_get_32(), wake_start_ms)) {
        mesh_wait_until_ms(wake_start_ms);
    }
    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        *result = progress->result;
        return -EAGAIN;
    }

    ret = mesh_send_route_wake_train(MESH_BROADCAST_ID, NULL, NULL,
                                     purpose, reason, &tx, NULL,
                                     FW_C5_TX_INTENT_BACKGROUND,
                                     NULL,
                                     NULL);
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }
    mesh_wait_for_c5_control_followup_turnaround(
        tx.packet.msg_type,
        reason == NULL ? "c5-flood-resume" : reason);
    ret = app_mesh_flood_send_bounded_resume(&tx, &ops, progress, result);
    if (ret == 0 &&
        (!progress->complete ||
         progress->next_opportunity != app_mesh_flood_repeat_limit() ||
         result->sent_count != app_mesh_flood_repeat_limit())) {
        return -EAGAIN;
    }
    return ret;
}

int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool send_wake_train,
                                struct app_mesh_tx_observation *observation,
                                uint32_t *scheduled_retry_delay_ms)
{
    struct mesh_outbound out_storage;
    struct mesh_outbound *out = &out_storage;
    uint32_t tx_span_ms = MESH_CONTROL_RX_HANDOFF_TIMEOUT_MS +
                          MESH_RADIO_EVENT_RETUNE_GUARD_MS +
                          C5_POLITE_SNIFF_MS + UWB_CONTROL_TX_TIMEOUT_MS;
    bool control_handoff_started = false;
    bool abort_scan = false;
    uint32_t wake_train_ms = WAKE_ADV_MS;
    int ret;

    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = 0u;
    }
    if (view == NULL || view->packet == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > sizeof(out->payload) ||
        view->packet->payload_len != view->payload_len ||
        scheduled_retry_delay_ms == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out->payload, view->payload, view->payload_len);
    }
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
    out->flood_retry_count = view->flood_retry_count;
    out->queued_at_valid = view->queued_at_valid;
    out->earliest_tx_valid = view->earliest_tx_valid;
    if (send_wake_train &&
        (mesh_c5_gateway_enumeration_claim(out) ||
         mesh_c5_compact_scheduled_control(out))) {
        wake_train_ms =
            MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS;
    }
    if (send_wake_train) {
        tx_span_ms += wake_train_ms + MESH_CONTROL_FOLLOWUP_TURNAROUND_MS;
    }
    *scheduled_retry_delay_ms = 0u;
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        control_handoff_started = mesh_rx_handoff_begin_control(&abort_scan);
        if (!control_handoff_started) {
            ret = -EBUSY;
            goto out_unlock;
        }
        mesh_stop_role_scan();
        if (abort_scan) {
            dwm3000_driver_request_receive_abort(
                DWM3000_RECEIVE_ABORT_MESH_CONTROL);
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            status_debug_printf("DBG_CONTROL_RX_HANDOFF_TIMEOUT abort=%u busy=%u\n",
                                abort_scan ? 1u : 0u,
                                radio_guard_uwb_busy() ? 1u : 0u);
            goto out_unlock;
        }
        status_debug_printf("DBG_CONTROL_RX_HANDOFF_READY abort=%u\n",
                            abort_scan ? 1u : 0u);
    }
    ret = mesh_send_c5_flood_now_until(
        out,
        purpose,
        reason,
        send_wake_train,
        mesh_c5_flood_purpose_is_priority(purpose),
        true,
        NULL,
        NULL,
        NULL,
        view->absolute_deadline_ms,
        observation,
        FW_C5_TX_INTENT_BACKGROUND);

out_unlock:
    if (observation != NULL) {
        observation->result_at_ms = (uint64_t)k_uptime_get();
    }
    if (control_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation)
{
    struct mesh_outbound out;
    bool control_handoff_started = false;
    bool abort_scan = false;
    int ret;

    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (view == NULL || view->packet == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > sizeof(out.payload) ||
        view->packet->payload_len != view->payload_len ||
        view->packet->msg_type != MSG_GATEWAY_ACK ||
        view->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !mesh_id_is_unicast(view->next_hop_id)) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        control_handoff_started = mesh_rx_handoff_begin_control(&abort_scan);
        if (!control_handoff_started) {
            ret = -EBUSY;
            goto out_unlock;
        }
        mesh_stop_role_scan();
        if (abort_scan) {
            dwm3000_driver_request_receive_abort(
                DWM3000_RECEIVE_ABORT_MESH_CONTROL);
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            status_debug_printf("DBG_RESPONSE_RX_HANDOFF_TIMEOUT abort=%u busy=%u\n",
                                abort_scan ? 1u : 0u,
                                radio_guard_uwb_busy() ? 1u : 0u);
            goto out_unlock;
        }
        status_debug_printf("DBG_RESPONSE_RX_HANDOFF_READY abort=%u\n",
                            abort_scan ? 1u : 0u);
    }

    memset(&out, 0, sizeof(out));
    out.packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out.payload, view->payload, view->payload_len);
    }
    out.payload_len = view->payload_len;
    out.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    out.next_hop_id = view->next_hop_id;
    out.queued_at_ms = view->queued_at_ms;
    out.queued_at_valid = view->queued_at_valid;
    /* A response is unscheduled; stale event timing must never delay it. */
    out.earliest_tx_ms = 0u;
    out.earliest_tx_valid = false;
    ret = mesh_send_outbound_keep_channel9_awake_until(
        &out,
        reason == NULL ? "control-response" : reason,
        view->absolute_deadline_ms,
        observation);

out_unlock:
    if (observation != NULL) {
        observation->result_at_ms = (uint64_t)k_uptime_get();
    }
    if (control_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

static bool mesh_node_comm_cancel_request_matches(
    const struct mesh_node_comm_cancel_request *request,
    uint32_t delivery_handle,
    uint32_t request_token)
{
    return request != NULL &&
           request->delivery_handle == delivery_handle &&
           request->request_token == request_token;
}

static bool mesh_node_comm_packet_matches_cancel(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_node_comm_cancel_request *request)
{
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN];

    return packet != NULL && request != NULL &&
           packet->msg_type == request->packet.msg_type &&
           packet->flags == request->packet.flags &&
           packet->src_id == request->packet.src_id &&
           packet->dst_id == request->packet.dst_id &&
           packet->session_id == request->packet.session_id &&
           packet->seq == request->packet.seq &&
           packet->payload_len == request->packet.payload_len &&
           mesh_packet_semantic_digest(packet,
                                       payload,
                                       payload_len,
                                       packet_digest) &&
           semantic_digest_equal(packet_digest,
                                 request->semantic_digest,
                                 sizeof(packet_digest));
}

int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation,
    uint32_t *scheduled_retry_delay_ms)
{
    /*
     * The communication facade keeps production records bounded to 192
     * payload bytes. Expansion reuses the route-wait worker scratch under
     * that scratch's existing mutex. Both paths hand their complete envelope
     * synchronously to mesh_start_tracked_tx_with_retry(), which copies it to
     * function-local working state before it can schedule route-wait work.
     * The worker therefore cannot observe or overwrite this expansion, and a
     * transient worker collision remains a pre-RF resource deferral.
     */
    struct mesh_outbound *out = &mesh_route_waiting_tx_scratch;
    struct app_mesh_async_route_transfer_identity route_transfer;
    bool policy_deferred = false;
    bool rf_started = false;
#if DEVICE_ROLE == ROLE_ANCHOR
    bool protocol_response_handoff_started = false;
    bool abort_scan = false;
    enum radio_guard_uwb_client protocol_response_radio_owner =
        RADIO_GUARD_UWB_CLIENT_NONE;
#endif
    int ret;

    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = 0u;
    }
    if (view == NULL || view->packet == NULL || observation == NULL ||
        scheduled_retry_delay_ms == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        view->packet->payload_len != view->payload_len ||
        view->delivery_generation == 0u ||
        !mesh_id_is_unicast(view->packet->dst_id)) {
        return -EINVAL;
    }

    ret = k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    /*
     * A command result is the causal reply to a frame the anchor has already
     * accepted.  Repeated Channel-5 command copies can otherwise keep the
     * role scan continuously armed, so the result never reaches the event
     * proposal that establishes its return Channel-9 cadence.  Claim one
     * exact RX-to-control boundary for this response.  Queued mesh work and
     * the coordinator's click/ACK priorities still apply after the
     * handoff; this only prevents a redundant receive rearm from winning
     * forever.
     */
    if (view->packet->msg_type == MSG_COMMAND_RESULT) {
        protocol_response_handoff_started =
            mesh_rx_handoff_begin_control(&abort_scan);
        if (!protocol_response_handoff_started) {
            ret = -EBUSY;
            goto out_unlock;
        }
        mesh_stop_role_scan();
        protocol_response_radio_owner = radio_guard_uwb_owner_client();
        if (!abort_scan && radio_guard_uwb_busy() &&
            ((protocol_response_radio_owner ==
                  RADIO_GUARD_UWB_CLIENT_MESH_RX &&
              !anchor_uwb_window_active()) ||
            protocol_response_radio_owner ==
                 RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN) &&
            !anchor_click_window_active()) {
            /*
             * Gateway-command delivery can own a separate bounded C5
             * follow-up listener rather than the ordinary role scan.  That
             * receive lease is deliberately abortable, but it is outside
             * mesh_rx_handoff.scan_radio_active.  A locally accepted command
             * result must end that obsolete listener before it can establish
             * its return cadence; never preempt click ranging.
             */
            abort_scan = true;
        }
        if (abort_scan) {
            dwm3000_driver_request_receive_abort(
                DWM3000_RECEIVE_ABORT_MESH_CONTROL);
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            status_debug_printf(
                "DBG_PROTOCOL_RESPONSE_RX_HANDOFF_TIMEOUT abort=%u busy=%u owner=%u\n",
                abort_scan ? 1u : 0u,
                radio_guard_uwb_busy() ? 1u : 0u,
                (unsigned int)protocol_response_radio_owner);
            goto out_unlock;
        }
        status_debug_printf(
            "DBG_PROTOCOL_RESPONSE_RX_HANDOFF_READY abort=%u owner=%u seq=%u\n",
            abort_scan ? 1u : 0u,
            (unsigned int)protocol_response_radio_owner,
            view->packet->seq);
    }
#endif
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out->payload, view->payload, view->payload_len);
    }
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
    out->flood_retry_count = view->flood_retry_count;
    out->queued_at_valid = view->queued_at_valid;
    out->earliest_tx_valid = view->earliest_tx_valid;
    route_transfer = (struct app_mesh_async_route_transfer_identity) {
        .target_id = out->packet.dst_id,
        .owner_generation = view->delivery_generation,
        .packet_seq = out->packet.seq,
        .msg_type = out->packet.msg_type,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
    };

    ret = mesh_start_tracked_tx_with_retry(
        out,
        reason == NULL ? "node-comm-reliable-uplink" : reason,
        scheduled_retry_delay_ms,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_RETAINED_LOCAL),
        &route_transfer,
        NULL,
        &rf_started,
        &policy_deferred,
        false,
        view->absolute_deadline_ms,
        observation);

#if DEVICE_ROLE == ROLE_ANCHOR
out_unlock:
    if (protocol_response_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
#endif
    k_mutex_unlock(&mesh_route_wait_scratch_lock);
    if (policy_deferred) {
        /* Channel-5 contention remains a randomized exponential retry. */
        *scheduled_retry_delay_ms = 0u;
    }
    /*
     * Direct gateway ACK waits populate gateway_confirmed_at_ms at frame
     * receipt; asynchronous ACKs are published through app_node_comm
     * separately.
     */
    observation->result_at_ms = (uint64_t)k_uptime_get();
    return ret;
}

static int mesh_cancel_reliable_uplink_route_owned(
    const struct mesh_node_comm_cancel_request *request)
{
    struct app_mesh_async_route_attempt route_attempt;
    bool cancelled = false;

    if (request == NULL || request->delivery_handle == 0u ||
        request->delivery_generation == 0u || request->request_token == 0u) {
        return -EINVAL;
    }
    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    if (app_mesh_async_route_request_snapshot(
            &mesh_route_discovery_request, &route_attempt) &&
        app_mesh_async_route_request_transfer_matches(
            &route_attempt,
            APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
            true,
            request->packet.dst_id,
            request->delivery_generation,
            request->packet.seq,
            request->packet.msg_type) &&
        app_mesh_async_route_request_complete(
            &mesh_route_discovery_request, &route_attempt)) {
        cancelled = true;
    }
    k_mutex_unlock(&mesh_route_discovery_lock);
    if (mesh_relay_tx_active(&mesh_runtime) &&
        mesh_node_comm_packet_matches_cancel(
            &mesh_runtime.pending.packet,
            mesh_runtime.pending.payload,
            mesh_runtime.pending.payload_len,
            request)) {
        mesh_relay_cancel_tx(&mesh_runtime);
        cancelled = true;
    }
    if (mesh_route_waiting_tx_valid &&
        mesh_node_comm_packet_matches_cancel(
            &mesh_route_waiting_tx.packet,
            mesh_route_waiting_tx.payload,
            mesh_route_waiting_tx.payload_len,
            request)) {
        mesh_route_waiting_tx_valid = false;
        mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
        cancelled = true;
    }

    if (cancelled) {
    }
    (void)mesh_schedule_tx_timeout();
    return cancelled ? 0 : -ENOENT;
}

static bool mesh_node_comm_cancel_complete(
    const struct mesh_node_comm_cancel_request *request,
    int result)
{
    k_spinlock_key_t key;
    bool completed = false;

    if (request == NULL) {
        return false;
    }

    key = k_spin_lock(&mesh_node_comm_cancel_lock);
    if (mesh_node_comm_cancel_request.pending &&
        !mesh_node_comm_cancel_request.complete &&
        mesh_node_comm_cancel_request_matches(
            &mesh_node_comm_cancel_request,
            request->delivery_handle,
            request->request_token)) {
        mesh_node_comm_cancel_request.result = result;
        mesh_node_comm_cancel_request.complete = true;
        completed = true;
    }
    k_spin_unlock(&mesh_node_comm_cancel_lock, key);
    return completed;
}

static void mesh_node_comm_cancel_work_handler(struct k_work *work)
{
    struct mesh_node_comm_cancel_request request;
    k_spinlock_key_t key;
    bool publish = false;
    int ret;

    ARG_UNUSED(work);

    key = k_spin_lock(&mesh_node_comm_cancel_lock);
    if (!mesh_node_comm_cancel_request.pending ||
        mesh_node_comm_cancel_request.complete) {
        k_spin_unlock(&mesh_node_comm_cancel_lock, key);
        return;
    }
    request = mesh_node_comm_cancel_request;
    k_spin_unlock(&mesh_node_comm_cancel_lock, key);

    ret = mesh_cancel_reliable_uplink_route_owned(&request);

    publish = mesh_node_comm_cancel_complete(&request, ret);

    if (publish) {
        app_node_comm_backend_release_ready(request.delivery_handle,
                                            request.request_token);
    }
}

int mesh_request_reliable_uplink_cancel(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t delivery_handle,
    uint32_t delivery_generation,
    uint32_t request_token)
{
    struct mesh_node_comm_cancel_request request;
    k_spinlock_key_t key;
    bool exact_existing;
    int ret;

    if (packet == NULL || semantic_digest == NULL ||
        delivery_handle == 0u || delivery_generation == 0u ||
        request_token == 0u) {
        return -EINVAL;
    }

    key = k_spin_lock(&mesh_node_comm_cancel_lock);
    exact_existing =
        (mesh_node_comm_cancel_request.pending ||
         mesh_node_comm_cancel_request.complete) &&
        mesh_node_comm_cancel_request_matches(
            &mesh_node_comm_cancel_request,
            delivery_handle,
            request_token);
    if (exact_existing) {
        k_spin_unlock(&mesh_node_comm_cancel_lock, key);
        return 0;
    }
    if (mesh_node_comm_cancel_request.pending ||
        mesh_node_comm_cancel_request.complete) {
        k_spin_unlock(&mesh_node_comm_cancel_lock, key);
        return -EBUSY;
    }
    mesh_node_comm_cancel_request =
        (struct mesh_node_comm_cancel_request) {
            .packet = *packet,
            .delivery_handle = delivery_handle,
            .delivery_generation = delivery_generation,
            .request_token = request_token,
            .pending = true,
        };
    memcpy(mesh_node_comm_cancel_request.semantic_digest,
           semantic_digest,
           sizeof(mesh_node_comm_cancel_request.semantic_digest));
    request = mesh_node_comm_cancel_request;
    k_spin_unlock(&mesh_node_comm_cancel_lock, key);

#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    if (k_current_get() == k_work_queue_thread_get(&mesh_route_work_q)) {
        /*
         * Gateway confirmation normally terminalizes the facade record from
         * this exact route-owner queue.  Deferring its release back onto the
         * same queue leaves the caller waiting on work that cannot run until
         * the confirmation handler returns.  Complete the route-owned part
         * inline; the caller still owns the exact terminal record and polls
         * this result immediately while holding its communication lock, so a
         * callback here would recurse into that lock.
         */
        ret = mesh_cancel_reliable_uplink_route_owned(&request);
        return mesh_node_comm_cancel_complete(&request, ret) ? 0 : -ESTALE;
    }
#endif

    ret = mesh_route_owner_work_reschedule(&mesh_node_comm_cancel_work, 0u);
    if (ret >= 0) {
        return 0;
    }

    key = k_spin_lock(&mesh_node_comm_cancel_lock);
    if (mesh_node_comm_cancel_request.pending &&
        !mesh_node_comm_cancel_request.complete &&
        mesh_node_comm_cancel_request_matches(
            &mesh_node_comm_cancel_request,
            delivery_handle,
            request_token)) {
        memset(&mesh_node_comm_cancel_request, 0,
               sizeof(mesh_node_comm_cancel_request));
    }
    k_spin_unlock(&mesh_node_comm_cancel_lock, key);
    return ret;
}

int mesh_take_reliable_uplink_cancel_result(uint32_t delivery_handle,
                                            uint32_t request_token,
                                            int *cancel_result)
{
    k_spinlock_key_t key;
    int ret;

    if (delivery_handle == 0u || request_token == 0u ||
        cancel_result == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&mesh_node_comm_cancel_lock);
    if (!mesh_node_comm_cancel_request_matches(
            &mesh_node_comm_cancel_request,
            delivery_handle,
            request_token)) {
        ret = -ENOENT;
    } else if (!mesh_node_comm_cancel_request.complete) {
        ret = 0;
    } else {
        *cancel_result = mesh_node_comm_cancel_request.result;
        memset(&mesh_node_comm_cancel_request, 0,
               sizeof(mesh_node_comm_cancel_request));
        ret = 1;
    }
    k_spin_unlock(&mesh_node_comm_cancel_lock, key);
    return ret;
}

int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason,
                       bool *sent_now)
{
    bool attempted = false;
    bool priority = mesh_c5_flood_purpose_is_priority(purpose);
    int ret = mesh_try_send_c5_flood(out, purpose, reason, &attempted);

    if (sent_now != NULL) {
        *sent_now = attempted;
    }
    if (!attempted && mesh_send_failure_retryable(ret)) {
        ret = mesh_c5_flood_store_deferred(
            out, purpose, reason, priority,
            ret == -EAGAIN || ret == -EBUSY);
    }
    return ret;
}

static int mesh_gateway_control_send_flood(
    void *ctx,
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    struct app_mesh_flood_result *result)
{
    ARG_UNUSED(ctx);
    return mesh_send_c5_flood_now(&orchestrator->gateway_flow.outbound,
                                  C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                                  reason,
                                  true,
                                  true,
                                  false,
                                  orchestrator,
                                  result,
                                  NULL);
}

int mesh_send_gateway_command_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now)
{
    bool control_handoff_started = false;
    int ret = mesh_gateway_rx_control_begin(&control_handoff_started);

    if (ret == 0) {
        ret = app_node_comm_gateway_control_send(orchestrator,
                                                 reason,
                                                 sent_now);
    }
    mesh_gateway_rx_control_end(control_handoff_started);
    return ret;
}
