/* Channel-5 credit/burst transport. The existing pending bank owns all bytes
 * from queue removal through exact ACK cleanup or successful queue return. */
#define MESH_C5_BURST_TURNAROUND_MS 30u
#define MESH_C5_BURST_FRAME_GAP_MS 8u
#define MESH_C5_BURST_ACK_WAIT_MS 250u
/* Requested sequence, three diagnostic lists, identities, and all flow TLVs. */
#define MESH_C5_ACK_PAYLOAD_CAP \
    (7u * PROTO_TLV_HEADER_LEN + 6u + \
     MESH_CH9_TX_BATCH_MAX * (10u + MESH_ACK_SEMANTIC_IDENTITY_TLV_LEN))
BUILD_ASSERT(MESH_C5_ACK_PAYLOAD_CAP <= UWB_MESH_MAX_PAYLOAD_LEN,
             "the largest credited ACK must fit the transport");
BUILD_ASSERT(MESH_CH9_TX_BATCH_MAX <= MESH_RX_QUEUE_DEPTH,
             "a granted burst must fit the physical RX queue");

static void mesh_c5_update_credit(void)
{
    uint32_t free_slots = 0u;
#if DEVICE_ROLE == ROLE_GATEWAY
    free_slots = gateway_ble_stream_free_slots();
#elif DEVICE_ROLE == ROLE_ANCHOR
    uint32_t used = report_tx_queue_used();
    free_slots = used < REPORT_TX_QUEUE_DEPTH ? REPORT_TX_QUEUE_DEPTH - used : 0u;
#endif
    free_slots = free_slots > 0u ? free_slots - 1u : 0u;
    uint32_t rx_used = k_msgq_num_used_get(&mesh_rx_msgq);
    uint32_t rx_free = rx_used < MESH_RX_QUEUE_DEPTH ? MESH_RX_QUEUE_DEPTH - rx_used : 0u;
    /* The relay subtracts the own-report reserve. Cap the grant to one bank. */
    free_slots = MIN(free_slots, MIN(rx_free + MESH_CUSTODY_OWN_RESERVE,
                                   MESH_CH9_TX_BATCH_MAX));
    mesh_relay_set_local_free_slots(&mesh_runtime, (uint16_t)free_slots);
}

/* Called after the first positive ACK, on the existing RX-drain owner.
 * The peer waits for our turnaround, then sends the granted followers. */
static void mesh_c5_capture_followers(const struct mesh_outbound *ack,
                                      const struct mesh_rx_pending *rx)
{
    struct mesh_ack_flow_control flow;
    struct radio_guard_uwb_lease lease = {0};
    uint8_t pending = 0u;
    uint8_t granted;
    int ret;

    if (rx == NULL || mesh_c5_rx_burst_peer != 0u ||
        mesh_batch_pending_from_payload(rx->payload, rx->payload_len, &pending) != PROTO_OK ||
        pending == 0u ||
        mesh_ack_flow_control_parse(&ack->packet, ack->payload, ack->payload_len, &flow) != PROTO_OK ||
        flow.identity_count == 0u || !flow.credit_valid || flow.credit == 0u) {
        return;
    }
    granted = MIN(pending, MIN(flow.credit, MESH_CH9_TX_BATCH_MAX - 1u));
    mesh_c5_rx_burst_peer = rx->previous_hop_id;
    /* The gateway scanner runs on another owner. Use its existing control
     * handoff before borrowing RX, just as route-control exchanges do. */
    ret = mesh_route_refresh_begin_radio_control(NULL);
    if (ret < 0) {
        return;
    }
    mesh_stop_role_scan();
    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                     "c5-burst-followers", &lease);
    if (ret == 0) {
        ret = dwm3000_driver_configure_wake_mesh_control_mode();
        for (uint8_t i = 0u; ret == 0 && i < granted; i++) {
            size_t len = 0u;
            uint8_t quality = 0u;
            bool valid = false;
            uint64_t previous = 0u;
            enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_NONE;
            ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
                i == 0u ? MESH_C5_BURST_TURNAROUND_MS + 20u : 20u,
                UWB_MESH_TX_TIMEOUT_MS, mesh_uwb_rx_frame,
                UWB_MESH_MAX_FRAME_LEN, &len, &quality, NULL, &failure);
            if (ret != 0) {
                break;
            }
            if (!mesh_queue_from_frame_at_internal(mesh_uwb_rx_frame, len,
                    quality, 0, false, UWB_CHANNEL_WAKE_CONTACT,
                    k_uptime_get_32(), 0u, NULL, 0u, false, &valid,
                    &previous, NULL) || previous != mesh_c5_rx_burst_peer) {
                break;
            }
        }
        int park = mesh_radio_idle_with_bounded_recovery("c5-burst-followers");
        int release = mesh_transport_radio_finish(&lease, park);
        if (release < 0) {
            app_watchdog_stop_feeding();
        } else if (ret == 0 || ret == -ETIMEDOUT) {
            app_watchdog_note_radio_progress();
        }
    }
    k_mutex_unlock(&mesh_send_scratch_lock);
    mesh_route_refresh_end_radio_control(NULL);
    /* ACK aggregation remains owned until the same drain empties the queue. */
    status_debug_printf("DBG_C5_BURST_RX peer=0x%llx grant=%u queued=%u ret=%d\n",
        (unsigned long long)mesh_c5_rx_burst_peer, granted,
        k_msgq_num_used_get(&mesh_rx_msgq), ret);
}

static bool mesh_c5_collect_ack(const struct mesh_outbound *ack,
                                const struct mesh_rx_pending *rx)
{
    if (rx == NULL || mesh_c5_rx_burst_peer == 0u ||
        rx->previous_hop_id != mesh_c5_rx_burst_peer ||
        rx->radio_channel != UWB_CHANNEL_WAKE_CONTACT) {
        return false;
    }
    if (mesh_ch9_ack_batch_queue(ack, rx) != 0) {
        /* Do not claim custody through an ACK we could not retain. The data
         * itself is still admitted and an exact sender retry is safe. */
        return false;
    }
    return true;
}

static void mesh_c5_flush_batch_ack(void)
{
    uint64_t peer = mesh_c5_rx_burst_peer;
    if (peer == 0u) {
        return;
    }
    if (mesh_send_current_ch9_ack_batch(peer, "c5-burst-ack") < 0) {
        /* Accepted data retains custody. An exact sender retry regenerates
         * this terminal ACK, with no scheduled channel-9 ACK owner. */
        (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer);
    }
    mesh_c5_rx_burst_peer = 0u;
    mesh_restart_role_scan();
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_c5_batch_packet(const struct mesh_outbound *out)
{
    return out->packet.dst_id == GATEWAY_ID &&
        (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
        (out->packet.msg_type == MSG_CLICK_REPORT ||
         out->packet.msg_type == MSG_SELF_TEST_REPORT ||
         out->packet.msg_type == MSG_ANCHOR_HEARTBEAT ||
         out->packet.msg_type == MSG_MESH_DATA);
}

static int mesh_c5_wait_batch_ack(uint8_t sent_mask, uint8_t *acked_mask,
                                 struct mesh_ack_flow_control *flow)
{
    struct proto_packet packet;
    uint8_t payload[MESH_C5_ACK_PAYLOAD_CAP];
    const uint64_t peer = mesh_ch9_tx_pending.next_hop_id;
    const uint32_t deadline = k_uptime_get_32() + MESH_C5_BURST_ACK_WAIT_MS;
    while (!uptime_deadline_reached(k_uptime_get_32(), deadline)) {
        size_t len = 0u, payload_len = 0u;
        uint8_t quality = 0u;
        uint64_t previous = 0u;
        enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_NONE;
        int ret = dwm3000_driver_receive_frame_continuous(
            uptime_ms_until_deadline(k_uptime_get_32(), deadline),
            mesh_uwb_rx_frame, UWB_MESH_MAX_FRAME_LEN, &len, &quality,
            NULL, &failure);
        if (ret != 0) {
            return ret;
        }
        ret = uwb_mesh_frame_decode(mesh_uwb_rx_frame, len, NETWORK_ID,
            DEVICE_ID, &previous, &packet, payload, sizeof(payload), &payload_len);
        if (ret != PROTO_OK || previous != peer || packet.src_id != peer ||
            (packet.dst_id != mesh_ch9_tx_pending.entries[0].outbound.packet.src_id &&
             packet.dst_id != DEVICE_ID) ||
            packet.msg_type != (peer == GATEWAY_ID ? MSG_GATEWAY_ACK : MSG_MESH_HOP_ACK) ||
            mesh_packet_rx_semantics_validate(&packet, payload, payload_len,
                previous, DEVICE_ID, GATEWAY_ID) != PROTO_OK ||
            mesh_ack_flow_control_parse(&packet, payload, payload_len, flow) != PROTO_OK) {
            continue;
        }
        uint8_t matched = 0u;
        for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
            const struct mesh_outbound *out = &mesh_ch9_tx_pending.entries[i].outbound;
            bool contains = false;
            ret = mesh_ack_payload_contains_packet(&packet, payload, payload_len,
                &out->packet, out->payload, out->payload_len, &contains);
            if (ret != PROTO_OK) {
                matched = 0u;
                break;
            }
            if (contains && (sent_mask & BIT(i)) != 0u) {
                matched |= BIT(i);
            }
        }
        matched &= (uint8_t)~*acked_mask;
        if (matched != 0u) {
            *acked_mask |= matched;
            return 0;
        }
        if (flow->identity_count == 0u && flow->credit_valid) {
            const uint8_t *seq = NULL;
            uint8_t seq_len = 0u;
            if (tlv_find_unique(payload, payload_len, TLV_REQUESTED_MSG_SEQ,
                    &seq, &seq_len) == PROTO_OK && seq_len == 2u) {
                for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
                    const struct proto_packet *sent = &mesh_ch9_tx_pending.entries[i].outbound.packet;
                    if ((sent_mask & (uint8_t)~*acked_mask & BIT(i)) != 0u &&
                        packet.session_id == sent->session_id &&
                        proto_get_u16_le(seq) == sent->seq) {
                        return -EAGAIN;
                    }
                }
            }
        }
    }
    return -ETIMEDOUT;
}

static int mesh_c5_retire_batch_member(struct mesh_outbound *out)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    if (!mesh_packet_semantic_digest(&out->packet, out->payload, out->payload_len, digest)) {
        return -EBADMSG;
    }
    if (out->packet.src_id == DEVICE_ID) {
        int ret = mesh_anchor_range_report_note_gateway_confirmed(&out->packet, digest);
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            return ret;
        }
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_delivery_gateway_confirmed != NULL) {
            ret = mesh_report_callbacks->anchor_delivery_gateway_confirmed(&out->packet, digest);
            if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
                return ret;
            }
        }
        ret = app_node_comm_note_gateway_confirmed_digest_at(&out->packet,
                    digest, (uint64_t)k_uptime_get());
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            return ret;
        }
        if (out->next_hop_id != GATEWAY_ID) {
            mesh_relay_note_parent_handoff(&mesh_runtime, &out->packet, out->next_hop_id);
        }
    }
    app_stack_workload_diag_relay_release(&out->packet, 0,
                                        report_tx_queue_used(), 0u);
    return 0;
}

static int mesh_c5_try_batch(void)
{
    struct mesh_outbound *scratch = &report_tx_worker_scratch;
    struct radio_guard_uwb_lease lease = {0};
    struct mesh_ack_flow_control flow = {0};
    uint64_t peer = 0u, source = 0u;
    uint8_t sent_mask = 0u, acked_mask = 0u;
    uint32_t retry_ms = MESH_RELAY_ACK_RETRY_BACKOFF_BASE_MS;
    bool leased = false;
    int ret = 0;

    if (mesh_c5_tx_bank_owned) {
        (void)mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
        if (!mesh_ch9_tx_pending.active) {
            mesh_c5_tx_bank_owned = false;
        }
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        /* Drain a normal queue head so retained custody can be returned. */
        return -ENOTSUP;
    }
    if (!mesh_ch9_tx_pending_can_start() || mesh_relay_tx_active(&mesh_runtime) ||
        mesh_route_waiting_tx_valid || report_tx_queue_used() < 2u) {
        return -ENOTSUP;
    }
    mesh_ch9_tx_pending_clear();
    while (mesh_ch9_tx_pending.count < MESH_CH9_TX_BATCH_MAX) {
        struct app_mesh_queue_head_token token;
        uint64_t candidate_peer = 0u;
        uint32_t now = k_uptime_get_32();
        if (report_tx_queue_begin_head(scratch, &token) != 0) {
            break;
        }
        if (!mesh_c5_batch_packet(scratch) || !mesh_outbound_ready_for_tx(scratch, now) ||
            mesh_relay_select_next_hop_for_packet(&mesh_runtime, &scratch->packet,
                scratch->payload, scratch->payload_len, now, &candidate_peer) != PROTO_OK ||
            (mesh_ch9_tx_pending.count != 0u &&
             (candidate_peer != peer || scratch->packet.src_id != source)) ||
            scratch->payload_len + 3u > sizeof(scratch->payload)) {
            (void)report_tx_queue_abort_head(&token);
            break;
        }
        peer = candidate_peer;
        source = scratch->packet.src_id;
        struct mesh_ch9_tx_pending_entry *entry =
            &mesh_ch9_tx_pending.entries[mesh_ch9_tx_pending.count];
        entry->outbound = *scratch;
        entry->outbound.next_hop_id = peer;
        entry->outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
        entry->acked = false;
        if (report_tx_queue_commit_head(&token, scratch) != 0) {
            (void)report_tx_queue_abort_head(&token);
            break;
        }
        mesh_ch9_tx_pending.count++;
    }
    if (mesh_ch9_tx_pending.count == 0u) {
        return -ENOTSUP;
    }
    mesh_ch9_tx_pending.active = true;
    mesh_ch9_tx_pending.next_hop_id = peer;
    mesh_c5_tx_bank_owned = true;
    if (peer != GATEWAY_ID &&
        !(app_mesh_report_click_participant(peer, k_uptime_get_32()) &&
          app_mesh_report_click_listen_active(k_uptime_get_32(), NULL))) {
        ret = mesh_send_route_wake_train_with_duration(peer, NULL, NULL,
            C5_CONTACT_PURPOSE_UPLINK, "c5-batch-wake",
            &mesh_ch9_tx_pending.entries[0].outbound, NULL,
            FW_C5_TX_INTENT_BACKGROUND, mesh_uplink_wake_train_ms(), NULL, NULL);
        if (ret < 0) {
            goto finish;
        }
        mesh_wait_for_c5_control_followup_turnaround(MSG_CLICK_REPORT, "c5-batch");
    }
    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    mesh_stop_role_scan();
    ret = mesh_transport_radio_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX, "c5-batch", &lease);
    if (ret == 0) {
        leased = true;
        ret = dwm3000_driver_configure_wake_mesh_control_mode();
    }
    uint8_t granted = 0u;
    for (uint8_t i = 0u; ret == 0 && i < mesh_ch9_tx_pending.count; i++) {
        if (i > granted && i != 0u) {
            break;
        }
        *scratch = mesh_ch9_tx_pending.entries[i].outbound;
        if (i > 0u) {
            uint8_t remaining;
            if (mesh_relay_next_burst_frame(&mesh_runtime, &remaining) != PROTO_OK ||
                remaining != granted - i) {
                ret = -EBADMSG;
                break;
            }
        }
        mesh_outbound_refresh_age(scratch, k_uptime_get_32());
        size_t len = scratch->payload_len;
        ret = i == 0u ?
            mesh_append_batch_pending(scratch->payload, sizeof(scratch->payload), &len,
                                      mesh_ch9_tx_pending.count - 1u) :
            mesh_append_batch_remaining(scratch->payload, sizeof(scratch->payload), &len,
                                        granted - i);
        scratch->payload_len = (uint16_t)len;
        scratch->packet.payload_len = (uint16_t)len;
        if (ret == PROTO_OK) {
            ret = mesh_send_outbound_preconfigured_ch9_locked(scratch, "c5-batch", NULL);
        }
        if (ret != 0) {
            break;
        }
        sent_mask |= BIT(i);
        fw_delivery_loss_note_sent(&mesh_delivery_loss, scratch);
        if (i == 0u) {
            ret = mesh_c5_wait_batch_ack(sent_mask, &acked_mask, &flow);
            if (ret != 0 || (acked_mask & 1u) == 0u) {
                break;
            }
            (void)mesh_relay_note_batch_pending(&mesh_runtime, mesh_ch9_tx_pending.count - 1u);
            mesh_runtime.batch.tx_credit = flow.credit_valid ?
                MIN(flow.credit, mesh_ch9_tx_pending.count - 1u) : 0u;
            granted = mesh_relay_batch_credit(&mesh_runtime);
            if (granted == 0u) {
                break;
            }
            k_msleep(MESH_C5_BURST_TURNAROUND_MS);
        } else if (i < granted) {
            k_msleep(MESH_C5_BURST_FRAME_GAP_MS);
        } else {
            ret = mesh_c5_wait_batch_ack(sent_mask, &acked_mask, &flow);
        }
    }
    if (leased) {
        int park = mesh_radio_idle_with_bounded_recovery("c5-batch");
        int release = mesh_transport_radio_finish(&lease, park);
        if (release < 0) {
            ret = release;
            app_watchdog_stop_feeding();
        } else if (sent_mask != 0u && (ret == 0 || ret == -ETIMEDOUT || ret == -EAGAIN)) {
            app_watchdog_note_radio_progress();
        }
    }
    k_mutex_unlock(&mesh_send_scratch_lock);
    mesh_restart_role_scan();
finish:
    status_debug_printf("DBG_C5_BATCH_TX peer=0x%llx count=%u sent=0x%x acked=0x%x ret=%d\n",
        (unsigned long long)peer, mesh_ch9_tx_pending.count, sent_mask, acked_mask, ret);
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        if ((acked_mask & BIT(i)) != 0u &&
            mesh_c5_retire_batch_member(&mesh_ch9_tx_pending.entries[i].outbound) == 0) {
            mesh_ch9_tx_pending.entries[i].acked = true;
        }
    }
    if (sent_mask != 0u && (ret == 0 || ret == -EAGAIN)) {
        for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
            struct route_candidate *candidate = &mesh_runtime.upstream.candidates[i];
            if (!candidate->valid || candidate->next_hop_id != peer) {
                continue;
            }
            if (flow.depth_valid) { candidate->hop_count = flow.depth; }
            if (flow.credit_valid) { candidate->queue_free_hint = flow.credit; }
            if (mesh_ack_flow_control_is_dead_end(&flow)) {
                candidate->hold_down_until_ms = k_uptime_get_32() + MESH_PARENT_DEAD_END_HOLD_MS;
                candidate->hold_down_valid = true;
            }
        }
        (void)route_select_best_at(&mesh_runtime.upstream, k_uptime_get_32());
        uint64_t next_peer = 0u;
        if (mesh_ack_flow_control_is_dead_end(&flow) &&
            mesh_relay_select_next_hop(&mesh_runtime, GATEWAY_ID, &next_peer) != PROTO_OK) {
            mesh_relay_poison_upstream(&mesh_runtime);
        }
    }
    if (acked_mask != 0u && !mesh_ack_flow_control_is_dead_end(&flow)) {
        mesh_relay_note_route_discovery_ready(&mesh_runtime, GATEWAY_ID);
        (void)route_record_candidate_success_at(&mesh_runtime.upstream, peer,
                                               GATEWAY_ID, k_uptime_get_32());
    } else if (ret == -ETIMEDOUT) {
        (void)route_record_candidate_failure_at(&mesh_runtime.upstream, peer,
            GATEWAY_ID, ROUTE_FAILURE_GATEWAY_ACK, k_uptime_get_32());
    }
    if (acked_mask != sent_mask || sent_mask == 0u) {
        const struct app_mesh_rf_retry_key retry_key = mesh_rf_retry_packet_key(
            &mesh_ch9_tx_pending.entries[0].outbound.packet,
            APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        retry_ms = mesh_rf_retry_bank_next_delay_ms(&mesh_report_rf_retry_bank,
            &retry_key, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA, "c5-batch");
    }
    if (ret == -EAGAIN && flow.retry_after_valid) {
        retry_ms = flow.retry_after_ms + (sys_rand32_get() % 31u);
    }
    mesh_relay_reset_batch(&mesh_runtime);
    (void)mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
    if (!mesh_ch9_tx_pending.active) {
        mesh_c5_tx_bank_owned = false;
    }
    report_tx_schedule(acked_mask == sent_mask && sent_mask != 0u ? 0u : retry_ms);
    return 0;
}
#else
static int mesh_c5_try_batch(void) { return -ENOTSUP; }
#endif
