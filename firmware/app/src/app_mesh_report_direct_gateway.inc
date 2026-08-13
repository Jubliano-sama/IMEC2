#line 7877 __BASE_FILE__
struct mesh_direct_gateway_ack_debug {
    uint64_t matched_at_ms;
    enum dwm3000_rx_failure rx_failure;
    uint64_t previous_hop_id;
    uint16_t frame_len;
    uint8_t timeout_slices;
    bool saw_frame;
    bool matched;
    bool functional_rx_outcome;
    bool queued_frame;
    bool valid_frame;
};

static void mesh_debug_log_direct_gateway_ack_result(
    const char *reason,
    int ret,
    const struct mesh_direct_gateway_ack_debug *debug,
    uint8_t attempt)
{
    if (debug == NULL) {
        return;
    }

    status_debug_printf("DBG_DIRECT_GW_ACK_DONE ret=%d m=%u f=%u p=%u q=%u v=%u fail=%u sl=%u prev=0x%llx len=%u att=%u r=%s\n",
                        ret,
                        debug->matched ? 1u : 0u,
                        debug->saw_frame ? 1u : 0u,
                        debug->functional_rx_outcome ? 1u : 0u,
                        debug->queued_frame ? 1u : 0u,
                        debug->valid_frame ? 1u : 0u,
                        (unsigned int)debug->rx_failure,
                        debug->timeout_slices,
                        (unsigned long long)debug->previous_hop_id,
                        debug->frame_len,
                        attempt,
                        reason == NULL ? "route" : reason);
}

static bool mesh_direct_gateway_ack_matches_packet(const struct mesh_outbound *probe,
                                                   const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len,
                                                   uint64_t previous_hop_id,
                                                   uint8_t quality)
{
    ARG_UNUSED(quality);

    if (probe == NULL || packet == NULL) {
        return false;
    }

    return app_mesh_direct_gateway_ack_matches(probe,
                                               packet,
                                               payload,
                                               payload_len,
                                               previous_hop_id,
                                               GATEWAY_ID);
}

static int mesh_wait_for_direct_gateway_ack_configured(const struct mesh_outbound *probe,
                                                       const char *reason,
                                                       bool apply_tracked_ack,
                                                       struct mesh_direct_gateway_ack_debug *debug)
{
    struct proto_packet ack_packet;
    uint8_t ack_payload[MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP];
    uint8_t *frame = mesh_uwb_rx_frame;
    uint32_t deadline_ms;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    uint8_t timeout_slices = 0u;
    int result_ret = -ETIMEDOUT;

    if (probe == NULL) {
        return -EINVAL;
    }
    if (debug != NULL) {
        memset(debug, 0, sizeof(*debug));
    }

    deadline_ms = k_uptime_get_32() + MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
    ARG_UNUSED(reason);
    while (true) {
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        bool valid_mesh_frame = false;
        uint64_t previous_hop_id = 0u;
        struct proto_packet *packet = &ack_packet;
        uint8_t *payload = ack_payload;
        size_t payload_len = 0u;
        uint32_t now_ms = k_uptime_get_32();
        uint32_t remaining_ms;
        uint32_t slice_ms;
        int rx_ret;

        if (uptime_deadline_reached(now_ms, deadline_ms)) {
            break;
        }
        remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
        memset(packet, 0, sizeof(*packet));
        slice_ms = MIN(remaining_ms, MESH_GATEWAY_DIRECT_PROBE_ACK_RX_SLICE_MS);
        rx_failure = DWM3000_RX_FAILURE_NONE;
        rx_ret = dwm3000_driver_receive_frame_detailed_quiet(
            slice_ms,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len,
            &quality,
            NULL,
            &rx_failure);
        if (rx_ret == -ETIMEDOUT) {
            if (timeout_slices < UINT8_MAX) {
                timeout_slices++;
            }
            if (debug != NULL) {
                debug->timeout_slices = timeout_slices;
                debug->rx_failure = rx_failure;
                debug->functional_rx_outcome = true;
            }
            continue;
        }
        if (rx_ret < 0) {
            if (debug != NULL) {
                debug->rx_failure = rx_failure;
                debug->functional_rx_outcome =
                    rx_failure != DWM3000_RX_FAILURE_NONE;
            }
            result_ret = rx_ret;
            break;
        }
        const uint64_t frame_received_at_ms = (uint64_t)k_uptime_get();

        status_debug_tx_gateway_ack_rx_pulse();
        if (debug != NULL) {
            debug->saw_frame = true;
            debug->frame_len = (uint16_t)MIN(frame_len, (size_t)UINT16_MAX);
            debug->functional_rx_outcome = true;
        }
        if (uwb_mesh_frame_decode(frame,
                                  frame_len,
                                  NETWORK_ID,
                                  DEVICE_ID,
                                  &previous_hop_id,
                                  packet,
                                  payload,
                                  MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP,
                                  &payload_len) == PROTO_OK &&
            mesh_direct_gateway_ack_matches_packet(probe,
                                                   packet,
                                                   payload,
                                                   payload_len,
                                                   previous_hop_id,
                                                   quality)) {
            if (debug != NULL) {
                debug->matched = true;
                debug->matched_at_ms = frame_received_at_ms;
                debug->previous_hop_id = previous_hop_id;
            }
            if (apply_tracked_ack) {
                (void)mesh_ch9_tx_pending_handle_ack(packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id,
                                                     UWB_CHANNEL_MESH_PAYLOAD);
            }
            if (!apply_tracked_ack &&
                probe->packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
                /*
                 * A direct route probe consumes this exact ACK as its
                 * synchronous contact proof.  The probe is intentionally not
                 * retained in relay custody, so queueing the same ACK gives
                 * RX no owner to advance and can block the relay C5 request
                 * that must follow a forced-hop contact-only success.
                 */
                if (debug != NULL) {
                    debug->queued_frame = false;
                    debug->valid_frame = true;
                    debug->previous_hop_id = previous_hop_id;
                }
                result_ret = 0;
                break;
            }
            {
                bool queued = mesh_queue_from_frame_at(frame,
                                                       frame_len,
                                                       quality,
                                                       UWB_CHANNEL_MESH_PAYLOAD,
                                                       k_uptime_get_32(),
                                                       0u,
                                                       NULL,
                                                       0u,
                                                       &valid_mesh_frame,
                                                       &previous_hop_id);

                if (debug != NULL) {
                    debug->queued_frame = queued;
                    debug->valid_frame = valid_mesh_frame;
                    debug->previous_hop_id = previous_hop_id;
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_DIRECT_GW_ACK_TRANSIT queued=%u valid=%u dst=0x%llx seq=%u\n",
                                        queued ? 1u : 0u,
                                        valid_mesh_frame ? 1u : 0u,
                                        (unsigned long long)packet->dst_id,
                                        packet->seq);
                }
                if (!queued) {
                    result_ret = -ENOSPC;
                    break;
                }
            }
            result_ret = 0;
            break;
        }

        {
            bool queued = mesh_queue_from_frame_at(
                frame,
                frame_len,
                quality,
                UWB_CHANNEL_MESH_PAYLOAD,
                k_uptime_get_32(),
                0u,
                NULL,
                0u,
                &valid_mesh_frame,
                &previous_hop_id);

            if (!queued && valid_mesh_frame) {
                result_ret = -ENOSPC;
            }
            if (debug != NULL) {
                debug->queued_frame = queued;
                debug->valid_frame = valid_mesh_frame;
                debug->previous_hop_id = previous_hop_id;
            }
            if (queued && valid_mesh_frame) {
                /*
                 * This valid frame belongs to another retained owner.  End
                 * the synchronous direct-ACK wait now; its queued RX work
                 * cannot run until the current radio turn yields.
                 */
                result_ret = -EAGAIN;
                break;
            }
        }
        if (result_ret == -ENOSPC) {
            break;
        }
    }
    return result_ret;
}

static int mesh_send_direct_gateway_payload_and_wait_ack(
    const struct mesh_outbound *out,
    const char *reason,
    const struct mesh_event_plan *plan,
    uint32_t channel9_event_start_ms,
    uint32_t report_latency_reference_ms,
    bool channel9_report_latency_pending,
    bool *rf_sent,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation)
{
    int64_t uwb_window_start_ms = -1;
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    struct radio_guard_uwb_lease radio_lease = {0};
    uint32_t ack_deadline_ms;
    bool gateway_ack_required = false;
    bool ack_wait_started = false;
    bool keep_channel9_awake = false;
    bool functional_tx_completed = false;
    int ack_ret = -ENOTCONN;
    int parking_ret;
    int release_ret;
    int ret;

    if (rf_sent != NULL) {
        *rf_sent = false;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (out == NULL ||
        out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        out->next_hop_id != GATEWAY_ID) {
        return -EINVAL;
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                     "mesh direct gateway payload",
                                     &radio_lease);
    if (ret < 0) {
        mesh_restart_role_scan();
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }

    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_mesh_payload_mode();
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_TX_CONFIG ret=%d now=%u seq=%u\n",
                            ret,
                            k_uptime_get_32(),
                            out->packet.seq);
    }
    if (ret == 0) {
        ret = mesh_send_outbound_preconfigured_ch9_locked_until(
            out,
            reason == NULL ? "direct-gateway-tx" : reason,
            NULL,
            absolute_deadline_ms,
            observation);
    }
    if (ret == 0) {
        functional_tx_completed = true;
        if (rf_sent != NULL) {
            *rf_sent = observation == NULL || observation->rf_started;
        }
        mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
        fw_delivery_loss_note_sent(&mesh_delivery_loss, out);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_OK\n");
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                               out->next_hop_id,
                               plan,
                               reason);
        }
        mesh_note_channel9_local_tx(out->next_hop_id, channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms >
                                           report_latency_reference_ms ?
                                           channel9_event_start_ms -
                                           report_latency_reference_ms : 0u);
        }

        gateway_ack_required =
            (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
        if (gateway_ack_required) {
            ack_deadline_ms = k_uptime_get_32() + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
            ack_wait_started = mesh_ch9_tx_pending_track_sent(out, ack_deadline_ms);
            if (ack_wait_started) {
                mesh_ch9_event_note_persistent_ack_wait(out->next_hop_id,
                                                        mesh_ch9_tx_pending_ack_deadline_ms(),
                                                        reason);
            } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DIRECT_GW_TX_ACK_TRANSIT_WAIT dst=0x%llx src=0x%llx seq=%u\n",
                                    (unsigned long long)out->packet.dst_id,
                                    (unsigned long long)out->packet.src_id,
                                    out->packet.seq);
            }
            ack_ret = mesh_wait_for_direct_gateway_ack_configured(out,
                                                                  reason,
                                                                  ack_wait_started,
                                                                  &ack_debug);
            status_debug_printf("DBG_DIRECT_GW_TX_ACK_WAIT ret=%d seq=%u\n",
                                ack_ret,
                                out->packet.seq);
            app_mesh_test_note_direct_gateway_ack(out->packet.seq,
                                                  ack_ret,
                                                  report_tx_queue_used());
            keep_channel9_awake =
                (ack_ret == 0 || ack_ret == -ETIMEDOUT) &&
                report_tx_queue_used() > 0u;
            if (ack_ret != 0) {
                if (ack_wait_started) {
                    mesh_ch9_tx_pending_clear();
                }
                ret = ack_ret == -ETIMEDOUT ? -EAGAIN : ack_ret;
            } else {
                route_record_success_at(&mesh_runtime.upstream, k_uptime_get_32());
                mesh_direct_gateway_bulk_probe_clear("direct-gateway-ack");
            }
        }
    }

    if (keep_channel9_awake) {
        parking_ret = mesh_radio_idle_with_bounded_recovery(
            "direct-gateway-payload");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_KEEP_CH9_AWAKE q=%u seq=%u ack_ret=%d release_ret=%d\n",
                                report_tx_queue_used(),
                                out->packet.seq,
                                ack_ret,
                                parking_ret);
        }
    } else {
        parking_ret = mesh_release_radio_after_mesh_turn(
            true, "direct-gateway-payload");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    release_ret = mesh_transport_radio_finish(&radio_lease, parking_ret);
    if (release_ret < 0) {
        LOG_ERR("direct-gateway payload radio release failed: seq=%u ret=%d",
                out->packet.seq,
                release_ret);
        k_mutex_unlock(&mesh_send_scratch_lock);
        return release_ret;
    }
    mesh_restart_role_scan();
    k_mutex_unlock(&mesh_send_scratch_lock);
    if (functional_tx_completed &&
               (!gateway_ack_required ||
                ack_debug.functional_rx_outcome)) {
        app_watchdog_note_radio_progress();
    }

    if (ack_wait_started) {
        mesh_debug_log_direct_gateway_ack_result(reason, ack_ret, &ack_debug, 0u);
    }
    if (observation != NULL) {
        observation->result_at_ms = (uint64_t)k_uptime_get();
    }
    if (ret < 0) {
        return ret;
    }
    mesh_schedule_tx_timeout();
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_init_direct_gateway_batch_plan(struct mesh_event_plan *plan,
                                                uint32_t now_ms)
{
    if (plan == NULL) {
        return;
    }

    plan->action = MESH_EVENT_PLAN_START;
    plan->start_ms = now_ms;
    plan->window_ms = MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS;
    plan->end_ms = now_ms + MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS;
}

static int mesh_try_send_report_tx_ch9_direct_gateway_batch(
    const struct mesh_event_plan *plan,
    uint64_t next_hop_id,
    uint8_t max_sent,
    uint32_t selected_at_ms)
{
    struct mesh_event_plan tx_plan;
    struct mesh_ch9_slot_tx_context slot_tx = {
        .uwb_window_start_ms = -1,
    };
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    struct mesh_outbound *queued = &report_tx_worker_scratch;
    struct fw_radio_activity_decision coordinator_decision;
    uint32_t batch_id;
    uint32_t reserve_ms = MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS;
    uint32_t slot_ms;
    uint32_t now_ms;
    uint32_t deadline_ms;
    uint32_t projected_done_ms = 0u;
    uint64_t batch_src_id = 0u;
    uint8_t candidate_count = 0u;
    uint8_t sent_count = 0u;
    bool ack_wait_started = false;
    int ack_ret = 0;
    int release_ret;
    int ret;

    /*
     * Direct batching has no independent per-fragment ACK owner yet. It also
     * shares candidate storage with the mutually exclusive pending-ACK bank,
     * so entering this path would overwrite unsent candidates as soon as the
     * first ACK wait starts. Keep the implementation compile-visible for its
     * timing model, but fail closed until both ownership problems are solved.
     */
    if (!MESH_DIRECT_GATEWAY_BATCHING_ENABLED) {
        return -ENOTSUP;
    }
    if (plan == NULL || next_hop_id != GATEWAY_ID || max_sent == 0u) {
        return -EINVAL;
    }

    tx_plan = *plan;
    slot_ms = uptime_ms_until_deadline(plan->start_ms, plan->end_ms);
    if (slot_ms <= reserve_ms) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_BATCH_DIRECT_NO_RESERVE\n");
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_RESERVE_FAIL now=%u start=%u end=%u slot=%u reserve=%u\n",
                                k_uptime_get_32(),
                                plan->start_ms,
                                plan->end_ms,
                                slot_ms,
                                reserve_ms);
        }
        report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return -EBUSY;
    }
    tx_plan.end_ms = plan->end_ms - reserve_ms;
    batch_id = mesh_ch9_next_batch_id();
    now_ms = k_uptime_get_32();
    deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_RESERVE batch=%u now=%u start=%u end=%u tx_end=%u reserve=%u cfg=%u guard=%u rx=%u q=%u max=%u\n",
                            batch_id,
                            now_ms,
                            plan->start_ms,
                            plan->end_ms,
                            tx_plan.end_ms,
                            reserve_ms,
                            MESH_CH9_TX_CONFIG_GUARD_MS,
                            MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                            MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS,
                            k_msgq_num_used_get(&report_tx_msgq),
                            max_sent);
    }

    while (candidate_count < max_sent) {
        struct app_mesh_queue_head_token head_token;
        uint64_t queued_next_hop_id = 0u;
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;
        bool fits;

        if (report_tx_queue_begin_head(queued, &head_token) != 0) {
            break;
        }
        mesh_coordinator_decide_now("ch9-direct-batch-collect",
                                    &coordinator_decision);
        if (!coordinator_decision.mesh_work_allowed) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=coord n=%u state=%s q=%u\n",
                                    candidate_count,
                                    fw_radio_activity_state_name(
                                        coordinator_decision.state),
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }

        now_ms = k_uptime_get_32();
        if (!mesh_outbound_ready_for_tx(queued, now_ms) ||
            !mesh_packet_prefers_channel9(&queued->packet)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=blocked n=%u ready=%u ch9=%u seq=%u earliest=%u q=%u\n",
                                    candidate_count,
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
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=next-hop n=%u ret=%d next=0x%llx want=0x%llx seq=%u q=%u\n",
                                    candidate_count,
                                    ret,
                                    (unsigned long long)queued_next_hop_id,
                                    (unsigned long long)next_hop_id,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }
        if (queued->packet.dst_id != GATEWAY_ID ||
            (queued->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
            (candidate_count > 0u && queued->packet.src_id != batch_src_id)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=batch-key n=%u src=0x%llx first=0x%llx dst=0x%llx flags=0x%02x seq=%u q=%u\n",
                                    candidate_count,
                                    (unsigned long long)queued->packet.src_id,
                                    (unsigned long long)batch_src_id,
                                    (unsigned long long)queued->packet.dst_id,
                                    queued->packet.flags,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }
        if (candidate_count == 0u) {
            batch_src_id = queued->packet.src_id;
        }

        report_tx_batch_candidates[candidate_count] = *queued;
        mesh_outbound_refresh_age(&report_tx_batch_candidates[candidate_count],
                                  now_ms);
        report_tx_batch_candidates[candidate_count].radio_channel =
            UWB_CHANNEL_MESH_PAYLOAD;
        report_tx_batch_candidates[candidate_count].next_hop_id = next_hop_id;
        ret = mesh_outbound_set_ch9_batch_metadata(
            &report_tx_batch_candidates[candidate_count],
            batch_id,
            false);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_META_FAIL ret=%d n=%u seq=%u\n",
                                    ret,
                                    candidate_count,
                                    queued->packet.seq);
            }
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }

        if (candidate_count == 0u) {
            send_start_ms = mesh_ch9_slot_send_start_ms(
                &report_tx_batch_candidates[candidate_count],
                &tx_plan,
                now_ms);
        } else {
            send_start_ms = projected_done_ms +
                MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS;
        }
        report_tx_batch_candidates[candidate_count].earliest_tx_ms =
            send_start_ms;
        report_tx_batch_candidates[candidate_count].earliest_tx_valid = true;
        fits = mesh_ch9_tx_fits_configured_slot(&report_tx_batch_candidates[candidate_count],
                                                &tx_plan,
                                                now_ms,
                                                send_start_ms,
                                                &required_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FIT batch=%u n=%u now=%u start=%u tx_end=%u txstart=%u req=%u fit=%u seq=%u len=%u\n",
                                batch_id,
                                candidate_count,
                                now_ms,
                                tx_plan.start_ms,
                                tx_plan.end_ms,
                                send_start_ms,
                                required_ms,
                                fits ? 1u : 0u,
                                report_tx_batch_candidates[candidate_count].packet.seq,
                                (unsigned int)mesh_outbound_encoded_frame_len(
                                    &report_tx_batch_candidates[candidate_count]));
        }
        if (!fits) {
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }

        ret = report_tx_queue_commit_head(&head_token, queued);
        if (ret != 0) {
            LOG_ERR("direct report queue collection commit failed: %d", ret);
            (void)report_tx_queue_abort_head(&head_token);
            break;
        }
        projected_done_ms = send_start_ms +
            mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(
                &report_tx_batch_candidates[candidate_count]));
        candidate_count++;
    }

    if (candidate_count == 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_EMPTY batch=%u q=%u\n",
                                batch_id,
                                k_msgq_num_used_get(&report_tx_msgq));
        }
        return -EBUSY;
    }

    ret = mesh_outbound_set_ch9_batch_metadata(
        &report_tx_batch_candidates[candidate_count - 1u],
        batch_id,
        true);
    if (ret != PROTO_OK) {
        (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                0u,
                                                candidate_count,
                                                "direct-final-meta-fail");
        return mesh_errno_from_proto(ret);
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    ret = mesh_ch9_slot_tx_begin(&slot_tx);
    if (ret < 0) {
        k_mutex_unlock(&mesh_send_scratch_lock);
        (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                0u,
                                                candidate_count,
                                                "direct-config-fail");
        return ret;
    }

    projected_done_ms = 0u;
    for (uint8_t i = 0u; i < candidate_count; i++) {
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;

        now_ms = k_uptime_get_32();
        send_start_ms = (i == 0u) ?
            mesh_ch9_slot_send_start_ms(&report_tx_batch_candidates[i],
                                        &tx_plan,
                                        now_ms) :
            projected_done_ms + MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS;
        report_tx_batch_candidates[i].earliest_tx_ms = send_start_ms;
        report_tx_batch_candidates[i].earliest_tx_valid = true;
        if (!mesh_ch9_tx_fits_configured_slot(&report_tx_batch_candidates[i],
                                              &tx_plan,
                                              now_ms,
                                              send_start_ms,
                                              &required_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_DIRECT_CONFIGURED_TRIM\n");
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_TRIM batch=%u keep=%u old=%u now=%u tx_end=%u req=%u seq=%u\n",
                                    batch_id,
                                    i,
                                    candidate_count,
                                    now_ms,
                                    tx_plan.end_ms,
                                    required_ms,
                                    report_tx_batch_candidates[i].packet.seq);
            }
            (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                    i,
                                                    candidate_count,
                                                    "direct-configured-trim");
            candidate_count = i;
            if (candidate_count > 0u) {
                (void)mesh_outbound_set_ch9_batch_metadata(
                    &report_tx_batch_candidates[candidate_count - 1u],
                    batch_id,
                    true);
            }
            break;
        }
        projected_done_ms = send_start_ms +
            mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(
                &report_tx_batch_candidates[i]));
    }

    if (candidate_count == 0u) {
        mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                           next_hop_id,
                           plan,
                           "queued-direct-gateway-batch-fit");
        mesh_relay_note_channel9_missed(&mesh_runtime,
                                        next_hop_id,
                                        &mesh_event_stats);
        app_mesh_test_note_ch9_missed();
        report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        release_ret = mesh_ch9_slot_tx_end(&slot_tx);
        k_mutex_unlock(&mesh_send_scratch_lock);
        return release_ret < 0 ? release_ret : -EALREADY;
    }

    mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                       next_hop_id,
                       plan,
                       "queued-direct-gateway-batch");

    uint32_t previous_frame_done_ms = 0u;
    for (uint8_t i = 0u; i < candidate_count; i++) {
        size_t sent_frame_len = 0u;
        uint32_t done_ms;

        ret = mesh_send_outbound_preconfigured_ch9_locked(
            &report_tx_batch_candidates[i],
            "queued-direct-gateway-batch",
            &sent_frame_len);
        if (ret < 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &report_tx_batch_candidates[i].packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
            uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "direct-gateway-batch-send");

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_DIRECT_SEND_FAIL\n");
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FAIL batch=%u ret=%d sent=%u n=%u seq=%u final=%u delay=%u\n",
                                    batch_id,
                                    ret,
                                    sent_count,
                                    candidate_count,
                                    report_tx_batch_candidates[i].packet.seq,
                                    i == candidate_count - 1u ? 1u : 0u,
                                    retry_delay_ms);
            }
            (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                    i,
                                                    candidate_count,
                                                    "direct-send-fail");
            if (mesh_ch9_tx_pending.active) {
                (void)mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
                if (!mesh_ch9_tx_pending.active) {
                    mesh_ch9_tx_pending_clear();
                }
            }
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               next_hop_id,
                               plan,
                               "queued-direct-gateway-batch-send");
            release_ret = mesh_ch9_slot_tx_end(&slot_tx);
            k_mutex_unlock(&mesh_send_scratch_lock);
            report_tx_schedule_backoff(retry_delay_ms,
                                       "direct-gateway-batch-send");
            return release_ret < 0 ? release_ret : -EALREADY;
        }
        slot_tx.functional_tx_completed = true;

        {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &report_tx_batch_candidates[i].packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                                &retry_key);
        }

        mesh_relay_note_tx_sent(&mesh_runtime,
                                &report_tx_batch_candidates[i],
                                k_uptime_get_32());
        fw_delivery_loss_note_sent(&mesh_delivery_loss,
                                           &report_tx_batch_candidates[i]);
        if (mesh_ch9_tx_pending_track_sent(&report_tx_batch_candidates[i],
                                           deadline_ms)) {
            ack_wait_started = true;
        }
        sent_count++;
        done_ms = k_uptime_get_32();

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t actual_gap_ms =
                previous_frame_done_ms == 0u ? 0u :
                uptime_ms_until_deadline(previous_frame_done_ms, done_ms);

            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FRAME batch=%u sent=%u n=%u done=%u tx_end=%u rem=%u len=%u seq=%u final=%u gap=%u\n",
                                batch_id,
                                sent_count,
                                candidate_count,
                                done_ms,
                                tx_plan.end_ms,
                                uptime_ms_until_deadline(done_ms, tx_plan.end_ms),
                                (unsigned int)sent_frame_len,
                                report_tx_batch_candidates[i].packet.seq,
                                i == candidate_count - 1u ? 1u : 0u,
                                actual_gap_ms);
        }
        previous_frame_done_ms = done_ms;
    }


    if (ack_wait_started) {
        mesh_ch9_event_note_persistent_ack_wait(next_hop_id,
                                                deadline_ms,
                                                "queued-direct-gateway-batch");
        ack_ret = mesh_wait_for_direct_gateway_ack_configured(
            &report_tx_batch_candidates[candidate_count - 1u],
            "queued-direct-gateway-batch",
            true,
            &ack_debug);
        mesh_debug_log_direct_gateway_ack_result("queued-direct-gateway-batch",
                                                 ack_ret,
                                                 &ack_debug,
                                                 0u);
        app_mesh_test_note_direct_gateway_ack(
            report_tx_batch_candidates[candidate_count - 1u].packet.seq,
            ack_ret,
            report_tx_queue_used());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_ACK batch=%u ret=%d sent=%u deadline=%u active=%u\n",
                                batch_id,
                                ack_ret,
                                sent_count,
                                deadline_ms,
                                mesh_ch9_tx_pending.active ? 1u : 0u);
        }
        if (ack_ret == 0) {
            route_record_success_at(&mesh_runtime.upstream, k_uptime_get_32());
            mesh_direct_gateway_bulk_probe_clear("direct-gateway-batch-ack");
        } else {
            mesh_schedule_tx_timeout();
        }
        if (!ack_debug.functional_rx_outcome) {
            slot_tx.functional_tx_completed = false;
        }
    } else {
        report_tx_schedule(0u);
    }

    release_ret = mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);
    if (release_ret < 0) {
        return release_ret;
    }
    if (ack_ret < 0 && ack_ret != -ETIMEDOUT) {
        return ack_ret;
    }

    mesh_note_channel9_local_tx(next_hop_id, plan->start_ms);
    mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                       next_hop_id,
                       plan,
                       "queued-direct-gateway-batch");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_SENT batch=%u selected_at=%u sent=%u ack=%u deadline=%u q=%u\n",
                            batch_id,
                            selected_at_ms,
                            sent_count,
                            ack_wait_started ? 1u : 0u,
                            deadline_ms,
                            report_tx_queue_used());
    }
    if (ack_wait_started) {
        mesh_schedule_tx_timeout();
    }
    return 0;
}
#endif

static int mesh_send_direct_gateway_probe_and_wait(const struct mesh_outbound *probe,
                                                   const char *reason,
                                                   bool apply_tracked_ack,
                                                   uint8_t attempt,
                                                   bool bounded_scratch_acquire)
{
    int64_t uwb_window_start_ms = -1;
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    struct radio_guard_uwb_lease radio_lease = {0};
    bool c5_activity = false;
    bool ack_wait_ran = false;
    bool functional_tx_completed = false;
    int parking_ret;
    int release_ret;
    int ret;

    if (probe == NULL ||
        probe->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        probe->next_hop_id != GATEWAY_ID) {
        return -EINVAL;
    }

    app_mesh_direct_probe_breadcrumb_note(APP_MESH_DIRECT_PROBE_PHASE_SCRATCH_ACQUIRE,
                                          attempt,
                                          probe->packet.seq);
    ret = k_mutex_lock(&mesh_send_scratch_lock,
                       bounded_scratch_acquire ?
                           K_MSEC(APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS) :
                           K_FOREVER);
    if (ret < 0) {
        return -EBUSY;
    }
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_SCAN_GUARD_TRANSITION,
        attempt,
        probe->packet.seq);
    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                     "mesh direct gateway route probe",
                                     &radio_lease);
    if (ret < 0) {
        mesh_restart_role_scan();
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }

    /*
     * Direct route contact is background maintenance. Check the control lane
     * at every safe retry boundary before leaving channel 5 so a gateway wake
     * train that started during the previous ACK wait can preempt the next
     * channel-9 attempt.
     */
    ret = mesh_route_wake_sniff_activity("direct-gateway-pre",
                                         GATEWAY_ID,
                                         reason,
                                         attempt > 0u ? attempt - 1u : 0u,
                                         &c5_activity);
    if (ret < 0 || c5_activity) {
        if (c5_activity) {
            ret = -EBUSY;
        }
        status_debug_printf("DBG_DIRECT_GW_PROBE_C5_YIELD ret=%d activity=%u attempt=%u\n",
                            ret,
                            c5_activity ? 1u : 0u,
                            attempt);
        parking_ret = mesh_release_radio_after_mesh_turn(
            true, "direct-gateway-probe-c5-yield");
        release_ret = mesh_transport_radio_finish(&radio_lease, parking_ret);
        if (release_ret < 0) {
            LOG_ERR("direct-gateway probe C5-yield radio release failed: seq=%u ret=%d",
                    probe->packet.seq,
                    release_ret);
            ret = release_ret;
        } else {
            mesh_restart_role_scan();
        }
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }

    uwb_window_start_ms = k_uptime_get();
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_CH9_CONFIGURE_RECOVERY,
        attempt,
        probe->packet.seq);
    ret = dwm3000_driver_configure_mesh_payload_mode();
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_PAYLOAD_MODE_COMPLETE,
        attempt,
        probe->packet.seq);
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_CONFIG ret=%d now=%u attempt=%u\n",
                            ret,
                            k_uptime_get_32(),
                            attempt);
    }
    if (ret == 0) {
        ret = mesh_send_outbound_preconfigured_ch9_locked(
            probe,
            "gateway-direct-route-probe",
            NULL);
    }
    if (ret == 0) {
        functional_tx_completed = true;
        ack_wait_ran = true;
        ret = mesh_wait_for_direct_gateway_ack_configured(probe,
                                                          reason,
                                                          apply_tracked_ack,
                                                          &ack_debug);
    } else {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_FAIL ret=%d attempt=%u\n",
                            ret,
                            attempt);
    }

    if (ret == 0) {
        parking_ret = mesh_radio_idle_with_bounded_recovery(
            "direct-gateway-probe");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_PROBE_KEEP_CH9_AWAKE seq=%u attempt=%u release_ret=%d\n",
                                probe->packet.seq,
                                attempt,
                                parking_ret);
        }
    } else {
        parking_ret = mesh_release_radio_after_mesh_turn(
            true, "direct-gateway-probe");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    release_ret = mesh_transport_radio_finish(&radio_lease, parking_ret);
    if (release_ret < 0) {
        LOG_ERR("direct-gateway probe radio release failed: seq=%u ret=%d",
                probe->packet.seq,
                release_ret);
        k_mutex_unlock(&mesh_send_scratch_lock);
        return release_ret;
    }
    mesh_restart_role_scan();
    k_mutex_unlock(&mesh_send_scratch_lock);
    if (functional_tx_completed &&
               (!ack_wait_ran ||
                ack_debug.functional_rx_outcome)) {
        app_watchdog_note_radio_progress();
    }
    if (ack_wait_ran) {
        mesh_debug_log_direct_gateway_ack_result(reason, ret, &ack_debug, attempt);
    }
    return ret;
}

static int mesh_try_direct_gateway_route_probe(uint64_t target_id,
                                               const char *reason,
                                               bool install_direct_route,
                                               enum app_mesh_direct_gateway_retry_mode retry_mode,
                                               uint32_t survey_id)
{
    struct mesh_outbound *probe = &mesh_direct_gateway_probe_scratch;
    struct app_mesh_direct_gateway_retry_state retry_state;
    struct app_mesh_direct_gateway_retry_decision retry_decision;
    uint32_t policy_deadline_ms = 0u;
    int last_ret = -ETIMEDOUT;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY ||
        target_id != GATEWAY_ID) {
        return -ENOTSUP;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ)) {
        install_direct_route = false;
    }
    if (app_mesh_direct_gateway_retry_init(&retry_state, retry_mode,
                                           DEVICE_ID, survey_id) != 0) {
        return -EINVAL;
    }
    if (retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
        policy_deadline_ms = k_uptime_get_32() +
            app_mesh_direct_gateway_retry_policy_horizon_ms(
                retry_mode, SURVEY_DIRECT_GATEWAY_PROBE_ATTEMPT_MS,
                APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS);
    }

    while ((ret = k_mutex_lock(&mesh_direct_gateway_probe_scratch_lock,
                               K_NO_WAIT)) < 0) {
        if (retry_mode != APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
            return -EBUSY;
        }
        (void)app_mesh_direct_gateway_retry_note(
            &retry_state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY, 0u,
            &retry_decision);
        if (!retry_decision.retry) {
            status_debug_printf("DBG_DIRECT_GW_PROBE_BUSY_TERMINAL attempts=%u deferrals=%u phase=outer-scratch\n",
                                retry_state.attempts,
                                retry_state.busy_deferrals);
            return -EBUSY;
        }
        if (retry_decision.delay_ms >
            app_mesh_direct_gateway_retry_deadline_remaining_ms(
                k_uptime_get_32(), policy_deadline_ms)) {
            return -ETIMEDOUT;
        }
        k_msleep(retry_decision.delay_ms);
    }

    while (true) {
        uint32_t now_ms = k_uptime_get_32();
        uint8_t attempt = retry_state.attempts + 1u;

        /*
         * Route probing is background work.  A prior probe may have received
         * and queued a valid frame that was not the probe's own direct ACK
         * (for example, the forwarded gateway ACK that terminates retained
         * ACK_CONFIRM custody).  Yield before another RF attempt so the
         * causal RX record can drain and retire the owner that requested
         * route repair.
         */
        if (k_msgq_num_used_get(&mesh_rx_msgq) != 0u) {
            status_debug_printf(
                "DBG_DIRECT_GW_PROBE_RX_YIELD q=%u attempt=%u phase=pre\n",
                k_msgq_num_used_get(&mesh_rx_msgq),
                (unsigned int)attempt);
            last_ret = -EAGAIN;
            break;
        }

        memset(probe, 0, sizeof(*probe));
        probe->packet.msg_type = MSG_GATEWAY_ROUTE_REQ;
        probe->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
        probe->packet.src_id = DEVICE_ID;
        probe->packet.dst_id = GATEWAY_ID;
        probe->packet.session_id = nonzero_uptime_session_id();
        probe->packet.seq = mesh_next_event_control_seq();
        probe->packet.ttl = MESH_DEFAULT_TTL;
        probe->packet.payload_len = 0u;
        probe->payload_len = 0u;
        probe->next_hop_id = GATEWAY_ID;
        probe->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        probe->queued_at_ms = now_ms;
        probe->earliest_tx_ms = now_ms;
        probe->queued_at_valid = true;
        probe->earliest_tx_valid = true;

        status_debug_printf("DBG_DIRECT_GW_PROBE_TX seq=%u now=%u attempt=%u/%u reason=%s\n",
                            probe->packet.seq,
                            now_ms,
                            (unsigned int)attempt,
                            (unsigned int)(retry_mode ==
                                APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY ?
                                APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS :
                                APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS),
                            reason == NULL ? "route" : reason);
        ret = mesh_send_direct_gateway_probe_and_wait(probe,
                                                      reason,
                                                      false,
                                                      attempt,
                                                      retry_mode ==
                                                          APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY);
        if (ret != 0 && k_msgq_num_used_get(&mesh_rx_msgq) != 0u) {
            status_debug_printf(
                "DBG_DIRECT_GW_PROBE_RX_YIELD q=%u attempt=%u phase=post\n",
                k_msgq_num_used_get(&mesh_rx_msgq),
                (unsigned int)attempt);
            last_ret = -EAGAIN;
            break;
        }
        if (ret == 0) {
            (void)app_mesh_direct_gateway_retry_note(
                &retry_state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS, 0u,
                &retry_decision);
            if (!install_direct_route) {
                status_debug_note("DBG_DIRECT_GW_PROBE_CONTACT_ONLY\n");
                last_ret = 0;
                goto out_unlock;
            }

            ret = mesh_relay_note_direct_gateway_route(&mesh_runtime,
                                                       k_uptime_get_32());
            if (ret != PROTO_OK) {
                status_debug_printf("DBG_DIRECT_GW_ROUTE_INSTALL_FAIL ret=%d\n", ret);
                last_ret = mesh_errno_from_proto(ret);
                goto out_unlock;
            }

            status_debug_note("DBG_DIRECT_GW_ROUTE_READY\n");
            last_ret = 0;
            goto out_unlock;
        }
        last_ret = ret;
        status_debug_printf("DBG_DIRECT_GW_PROBE_MISS ret=%d attempt=%u\n",
                            ret,
                            (unsigned int)attempt);
        (void)app_mesh_direct_gateway_retry_note(
            &retry_state,
            ret == -EBUSY ? APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY :
                            APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED,
            retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE &&
                retry_state.attempts + 1u <
                    APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS ?
                sys_rand32_get() : 0u,
            &retry_decision);
        if (!retry_decision.retry) {
            if (retry_decision.busy_exhausted) {
                status_debug_printf("DBG_DIRECT_GW_PROBE_BUSY_TERMINAL attempts=%u deferrals=%u phase=radio\n",
                                    retry_state.attempts,
                                    retry_state.busy_deferrals);
            }
            break;
        }
        status_debug_printf("DBG_DIRECT_GW_PROBE_BACKOFF ms=%u next_attempt=%u busy_defer=%u\n",
                            retry_decision.delay_ms,
                            (unsigned int)(retry_state.attempts + 1u),
                            retry_decision.attempt_consumed ? 0u : 1u);
        if (retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY &&
            retry_decision.delay_ms >
                app_mesh_direct_gateway_retry_deadline_remaining_ms(
                    k_uptime_get_32(), policy_deadline_ms)) {
            last_ret = -ETIMEDOUT;
            break;
        }
        k_msleep(retry_decision.delay_ms);
    }

out_unlock:
    k_mutex_unlock(&mesh_direct_gateway_probe_scratch_lock);
    return last_ret;
}

static bool mesh_route_request_defer_rf_busy(
    enum app_mesh_route_request_rf_phase phase,
    const char *phase_name,
    int ret,
    bool embedded_route_sent,
    const char *reason,
    const struct mesh_outbound *route_request,
    uint64_t target_id,
    const struct mesh_route_discovery_state *route_discovery_before_prepare)
{
    struct app_mesh_route_request_rf_failure_decision decision;
    struct app_mesh_rf_retry_key retry_key;
    uint32_t retry_delay_ms;

    app_mesh_route_request_rf_failure_decide(phase,
                                             ret,
                                             embedded_route_sent,
                                             -EBUSY,
                                             &decision);
    if (!decision.defer_retry) {
        return false;
    }

    if (decision.restore_prepared_attempt &&
        route_discovery_before_prepare != NULL) {
        mesh_runtime.route_discovery = *route_discovery_before_prepare;
    }
    retry_key = mesh_route_request_rf_retry_key(route_request, target_id,
                                                phase);
    retry_delay_ms = mesh_rf_retry_next_delay_ms(
        mesh_route_request_rf_retry_state(phase),
        &retry_key,
        APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
        phase_name);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RF_BUSY_DEFER phase=%s ret=%d restored=%u delay=%u active=%u attempts=%u next=%u\n",
                            phase_name == NULL ? "route" : phase_name,
                            ret,
                            decision.restore_prepared_attempt ? 1u : 0u,
                            retry_delay_ms,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }
    mesh_schedule_route_waiting_retry_after(reason == NULL ? "route-rf-busy" :
                                           reason,
                                           retry_delay_ms);
    return true;
}

static int mesh_request_route_owned(
    uint64_t target_id,
    const char *reason,
    enum app_mesh_route_wait_tx_owner owner,
    const struct proto_packet *owner_packet,
    uint32_t generation,
    const struct app_mesh_c5_tx_authorization_token *authorization)
{
    struct mesh_outbound route_req;
    struct app_mesh_route_request_policy_state route_policy_state = {0};
    struct app_mesh_route_request_policy_decision route_policy;
    struct mesh_route_capture_identity route_identity = {0};
    struct app_mesh_route_retry_identity retry_identity;
    struct mesh_route_discovery_state route_discovery_before_prepare;
    struct mesh_event_timing proposed_timing = {0};
    const struct mesh_event_timing *proposed_timing_ptr = NULL;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t route_backoff_remaining_ms = 0u;
    uint32_t timing_reference_ms = now_ms;
    uint32_t route_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    uint32_t route_random_value;
    bool embedded_route_sent = false;
    bool relay_required_route_req =
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ) &&
        target_id == GATEWAY_ID;
    uint8_t route_request_flags;
    uint16_t route_reply_rx_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
                                       MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS : 0u;
    int ret;

    if (owner == APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC) {
        owner_packet = mesh_route_waiting_tx_valid ?
                       &mesh_route_waiting_tx.packet : NULL;
        generation = owner_packet == NULL ? 0u : owner_packet->session_id;
    }
    app_mesh_route_retry_identity_select(owner, owner_packet, generation,
                                         &retry_identity);

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_CALL target=0x%llx reason=%s active=%u attempts=%u next=%u\n",
                            (unsigned long long)target_id,
                            reason == NULL ? "route" : reason,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }

    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-request" :
                                            reason)) {
        mesh_schedule_route_waiting_retry(reason);
        return -EBUSY;
    }

    /*
     * The direct-gateway probe is part of a route-discovery attempt, not a
     * free liveness poll.  Enforce the core's same-target backoff before any
     * radio side effect; timeout service may submit the exact retained owner
     * repeatedly while this deadline is in the future.
     */
    if (mesh_relay_route_discovery_backoff_pending(
            &mesh_runtime,
            target_id,
            now_ms,
            &route_backoff_remaining_ms)) {
        mesh_schedule_route_waiting_retry_after(
            reason == NULL ? "route-backoff" : reason,
            route_backoff_remaining_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_ROUTE_REQ_PREPROBE_BACKOFF target=0x%llx delay=%u reason=%s\n",
                (unsigned long long)target_id,
                route_backoff_remaining_ms,
                reason == NULL ? "route" : reason);
        }
        return -EAGAIN;
    }

    route_policy_state.relay_required = relay_required_route_req;
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS)
    if (relay_required_route_req) {
        route_policy_state.required_gateway_relay_hops =
            CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS;
    }
#endif
    route_policy_state.direct_bulk_suppressed =
        mesh_direct_gateway_bulk_probe_install_suppressed(target_id, now_ms);
    route_policy_state.direct_probe_ret = -ENOTCONN;
    app_mesh_route_request_policy_decide(&route_policy_state, &route_policy);
    route_request_flags = route_policy.route_request_flags;
    if (route_policy_state.direct_bulk_suppressed &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DIRECT_BULK_SUPPRESSED target=0x%llx until=%u reason=%s\n",
                            (unsigned long long)target_id,
                            mesh_direct_gateway_bulk_suppressed_until_ms,
                            reason == NULL ? "route" : reason);
    }

    ret = mesh_try_direct_gateway_route_probe(
        target_id, reason, route_policy.install_direct_route_from_probe,
        retry_identity.mode,
        retry_identity.survey_id);
    if (ret != 0 && k_msgq_num_used_get(&mesh_rx_msgq) != 0u) {
        /* The direct probe queued causal RX.  Do not continue into a relay
         * wake train in the same route-work turn; let RX clear or advance the
         * exact retained owner first, then re-evaluate route need. */
        mesh_schedule_route_waiting_retry_after(
            reason == NULL ? "route-rx-yield" : reason,
            REPORT_TX_RETRY_DELAY_MS);
        return -EBUSY;
    }
    route_policy_state.direct_probe_ret = ret;
    app_mesh_route_request_policy_decide(&route_policy_state, &route_policy);
    app_mesh_test_note_direct_gateway_route_probe(target_id, ret);
    if (route_policy.direct_probe_satisfies_request) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REQ_DIRECT_READY\n");
        }
        app_mesh_test_note_route_ready(target_id, GATEWAY_ID, 0);
        mesh_schedule_route_waiting_retry_after("gateway-direct-route", 0u);
        return 0;
    }
    if (ret == 0 && !route_policy.install_direct_route_from_probe &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note(relay_required_route_req ?
                          "DBG_ROUTE_REQ_DIRECT_CONTACT_RELAY_REQUIRED\n" :
                          "DBG_ROUTE_REQ_DIRECT_CONTACT_BULK_SUPPRESSED\n");
    }
    if (relay_required_route_req && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RELAY_REQUIRED target=0x%llx hops=%u reason=%s\n",
                            (unsigned long long)target_id,
                            route_policy_state.required_gateway_relay_hops,
                            reason == NULL ? "route" : reason);
    }
    now_ms = k_uptime_get_32();
    timing_reference_ms = now_ms;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE != ROLE_GATEWAY) {
        ret = mesh_prepare_event_timing(&proposed_timing, timing_reference_ms);
        if (ret == PROTO_OK) {
            mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
            proposed_timing_ptr = &proposed_timing;
            status_debug_printf("DBG_ROUTE_REQ_CH9_TIMING ref=%u next=%u guard=%u win=%u\n",
                                timing_reference_ms,
                                proposed_timing.next_event_time_ms,
                                proposed_timing.guard_ms,
                                proposed_timing.event_window_ms);
        } else {
            status_debug_printf("DBG_ROUTE_REQ_CH9_TIMING_FAIL ret=%d\n", ret);
        }
    }

    route_random_value = sys_rand32_get();
    route_discovery_before_prepare = mesh_runtime.route_discovery;
    ret = mesh_relay_prepare_route_request_with_timing_flags(&mesh_runtime,
                                                             target_id,
                                                             proposed_timing_ptr,
                                                             timing_reference_ms,
                                                             route_request_flags,
                                                             route_reply_rx_delay_ms,
                                                             now_ms,
                                                             route_random_value,
                                                             &route_req);
    if (ret != PROTO_OK) {
        app_mesh_test_note_route_request_prepare_result(target_id, ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_PREP_FAIL ret=%d target=0x%llx active=%u attempts=%u next=%u\n",
                                ret,
                                (unsigned long long)target_id,
                                mesh_runtime.route_discovery.active ? 1u : 0u,
                                mesh_runtime.route_discovery.attempts,
                                mesh_runtime.route_discovery.next_request_ms);
        }
        if (ret == PROTO_ERR_BUSY) {
            LOG_INF("mesh route discovery backoff active: target=0x%016llx attempts=%u next_ms=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    mesh_runtime.route_discovery.next_request_ms,
                    reason);
            mesh_schedule_route_waiting_retry(reason);
            return -EAGAIN;
        }
        if (ret == PROTO_ERR_STALE) {
            LOG_WRN("mesh route discovery exhausted: target=0x%016llx attempts=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    reason);
            mesh_relay_reset_route_discovery(&mesh_runtime);
            return -ETIMEDOUT;
        }
        return mesh_errno_from_proto(ret);
    }
    if (!mesh_route_capture_identity_from_request(&route_req,
                                                  target_id,
                                                  &route_identity)) {
        LOG_ERR("mesh route request identity missing: target=0x%016llx session=%u",
                (unsigned long long)target_id,
                route_req.packet.session_id);
        return -EINVAL;
    }
    app_mesh_test_note_route_request_attempt(target_id,
                                             mesh_runtime.route_discovery.attempts,
                                             route_req.packet.ttl);
    route_reply_window_ms = mesh_route_reply_listen_window_ms(route_req.packet.ttl);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t route_backoff_ms =
            uptime_ms_until_deadline(now_ms,
                                     mesh_runtime.route_discovery.next_request_ms);

        status_debug_printf("DBG_ROUTE_REQ_BACKOFF attempt=%u ttl=%u rand=%u delay=%u next=%u\n",
                            mesh_runtime.route_discovery.attempts,
                            route_req.packet.ttl,
                            route_random_value,
                            route_backoff_ms,
                            mesh_runtime.route_discovery.next_request_ms);
        status_debug_printf("DBG_ROUTE_REPLY_WINDOW ttl=%u win=%u base=%u attempt=%u\n",
                            route_req.packet.ttl,
                            route_reply_window_ms,
                            MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
                            mesh_runtime.route_discovery.attempts);
    }

    LOG_INF("mesh route discovery request: target=0x%016llx attempt=%u next_ms=%u reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            mesh_runtime.route_discovery.next_request_ms,
            reason);
    ret = mesh_send_route_wake_train(target_id,
                                     &route_req,
                                     &embedded_route_sent,
                                     C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                     reason,
                                     &route_req,
                                     authorization,
                                     FW_C5_TX_INTENT_BACKGROUND,
                                     NULL,
                                     NULL);
    if (ret < 0) {
        LOG_WRN("mesh route discovery wake train failed: target=0x%016llx attempt=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                ret,
                reason);
        if (mesh_route_request_defer_rf_busy(
                APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN,
                "wake",
                ret,
                embedded_route_sent,
                reason,
                &route_req,
                target_id,
                &route_discovery_before_prepare)) {
            return ret;
        }
        app_mesh_rf_retry_reset(&mesh_route_request_wake_rf_retry);
        mesh_restart_role_scan();
        mesh_schedule_route_waiting_retry(reason);
        return ret;
    }
    {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_route_request_rf_retry_key(
                &route_req,
                target_id,
                APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN);

        app_mesh_rf_retry_note_success(&mesh_route_request_wake_rf_retry,
                                       &retry_key);
    }
    if (embedded_route_sent) {
        bool route_reply_captured = false;
        int listen_ret;

        listen_ret = mesh_listen_for_route_reply(target_id,
                                                 "embedded-route-request",
                                                 route_reply_window_ms,
                                                 &route_identity,
                                                 &route_reply_captured);
        if (route_reply_captured) {
            status_debug_note("DBG_EMBEDDED_ROUTE_REQ_SKIP_FALLBACK\n");
            mesh_route_reply_handoff_after_capture(target_id, reason);
            return 0;
        }
        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh embedded route reply listen failed: target=0x%016llx attempt=%u ret=%d reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    listen_ret,
                    reason == NULL ? "route" : reason);
        }
        status_debug_note("DBG_EMBEDDED_ROUTE_REQ_FALLBACK\n");
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        LOG_INF("mesh route channel-5 turnaround delay: target=0x%016llx delay_ms=%u reason=%s",
                (unsigned long long)target_id,
                MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
                reason == NULL ? "route" : reason);
        k_msleep(MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
    }
    mesh_c5_contact_exchange(target_id,
                             C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                             k_uptime_get_32() + route_reply_window_ms,
                             reason);
    ret = mesh_send_outbound_authorized(
        &route_req,
        "route-request-control",
        authorization);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_CONTROL_TX ret=%d seq=%u ttl=%u\n",
                            ret,
                            route_req.packet.seq,
                            route_req.packet.ttl);
    }
    LOG_INF("mesh route discovery request TX done: target=0x%016llx attempt=%u ret=%d reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            ret,
            reason == NULL ? "route" : reason);
    if (ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_route_request_rf_retry_key(
                &route_req,
                target_id,
                APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX);
        bool route_reply_captured = false;
        int listen_ret = mesh_listen_for_route_reply(target_id,
                                                     reason,
                                                     route_reply_window_ms,
                                                     &route_identity,
                                                     &route_reply_captured);

        app_mesh_rf_retry_note_success(&mesh_route_request_control_rf_retry,
                                       &retry_key);

        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh route reply listen failed after request TX: target=0x%016llx attempt=%u ret=%d reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    listen_ret,
                    reason == NULL ? "route" : reason);
        }
        if (route_reply_captured) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_REQ_REPLY_CAPTURED\n");
            }
            mesh_route_reply_handoff_after_capture(target_id, reason);
        } else {
            app_mesh_test_note_route_reply_miss(target_id, listen_ret);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REQ_REPLY_MISS listen_ret=%d win=%u\n",
                                    listen_ret,
                                    route_reply_window_ms);
            }
            mesh_schedule_route_waiting_retry(reason);
        }
    } else {
        LOG_WRN("mesh route discovery request TX failed: target=0x%016llx attempt=%u next_ms=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                mesh_runtime.route_discovery.next_request_ms,
                ret,
                reason);
        if (!mesh_route_request_defer_rf_busy(
                APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
                "control",
                ret,
                embedded_route_sent,
                reason,
                &route_req,
                target_id,
                &route_discovery_before_prepare)) {
            app_mesh_rf_retry_reset(&mesh_route_request_control_rf_retry);
            mesh_schedule_route_waiting_retry(reason);
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RET ret=%d active=%u attempts=%u next=%u\n",
                            ret,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }
    return ret;
}
