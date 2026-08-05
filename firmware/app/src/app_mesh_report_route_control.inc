#line 5154 __BASE_FILE__
static int mesh_send_c5_flood_response(const struct mesh_outbound *out,
                                       uint8_t purpose,
                                       const char *reason,
                                       bool *forward_admission_retained)
{
    struct app_mesh_flood_result result = {0};
    int store_ret;
    int ret;

    if (forward_admission_retained != NULL) {
        *forward_admission_retained = false;
    }
    ret = mesh_send_c5_flood_now(out, purpose, reason, true, true, false, NULL,
                                 &result, NULL);
    if (result.sent_count > 0u) {
        if (forward_admission_retained != NULL) {
            *forward_admission_retained = true;
        }
        return ret;
    }
    if (ret == 0) {
        ret = -EIO;
    }
    if (mesh_send_failure_retryable(ret)) {
        store_ret =
            mesh_c5_flood_store_deferred(out, purpose, reason, true);
        if (store_ret == 0) {
            if (forward_admission_retained != NULL) {
                *forward_admission_retained = true;
            }
        } else {
            ret = store_ret;
        }
    }
    return ret;
}

static void mesh_rollback_c5_forward_admission(
    const struct mesh_relay_result *result,
    const struct mesh_rx_pending *rx,
    const char *reason)
{
    int ret;

    if (result == NULL || rx == NULL) {
        return;
    }
    ret = mesh_relay_rollback_forward_admission(
        &mesh_runtime,
        &rx->packet,
        rx->payload,
        rx->payload_len,
        result);
    if (ret != PROTO_OK) {
        LOG_ERR("mesh C5 forward admission rollback failed: msg=0x%02x seq=%u ret=%d reason=%s",
                rx->packet.msg_type,
                rx->packet.seq,
                ret,
                reason == NULL ? "c5-forward" : reason);
    } else {
        LOG_WRN("mesh C5 forward left retryable after admission failure: msg=0x%02x seq=%u reason=%s",
                rx->packet.msg_type,
                rx->packet.seq,
                reason == NULL ? "c5-forward" : reason);
    }
}

/*
 * A route-reply listener normally owns the extended-PHR control PHY.  A
 * standard wake claim cannot be decoded in that mode, but the DW3000 still
 * reports the preamble/PHY error as activity.  Use that activity as a bounded
 * probe trigger: retune to the standard wake PHY long enough to catch the
 * clicker's next repeated claim, then restore the route-control PHY before
 * returning to the continuous listener.
 *
 * The radio remains owned by the caller throughout this helper.  A captured
 * claim is returned to the caller while the standard PHY is still selected so
 * the caller can release its guard and hand the claim to the normal click
 * sequence exactly once.
 */
static void mesh_log_standard_click_probe_done(uint32_t started_ms,
                                               const char *outcome,
                                               int ret)
{
    status_debug_printf("DBG_ROUTE_REPLY_CLICK_PROBE_DONE duration_ms=%u outcome=%s ret=%d\n",
                        k_uptime_get_32() - started_ms,
                        outcome == NULL ? "fail" : outcome,
                        ret);
}

static int mesh_probe_standard_click_claim(
    uint8_t *frame,
    size_t frame_cap,
    struct uwb_wake_claim_frame *click_claim,
    uint8_t *click_quality,
    uint32_t *click_observed_ms)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint32_t started_ms;
    int ret;

    if (frame == NULL || frame_cap == 0u || click_claim == NULL ||
        click_quality == NULL || click_observed_ms == NULL) {
        return -EINVAL;
    }

    started_ms = k_uptime_get_32();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        mesh_log_standard_click_probe_done(started_ms, "fail", ret);
        return ret;
    }

    ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
        MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS,
        ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
        frame,
        frame_cap,
        &frame_len,
        &quality,
        NULL,
        &rx_failure);
    if (ret == 0 &&
        mesh_frame_requires_anchor_click_handoff(frame,
                                                 frame_len,
                                                 click_claim)) {
        *click_quality = quality;
        *click_observed_ms = k_uptime_get_32();
        status_debug_printf("DBG_ROUTE_REPLY_CLICK_PROBE_CAPTURE evt=%u attempt=%u q=%u\n",
                            click_claim->click_event_id,
                            click_claim->attempt_index,
                            quality);
        mesh_log_standard_click_probe_done(started_ms, "capture", 0);
        return 1;
    }

    /* A timeout, malformed standard frame, or another PHY's activity all
     * consume this bounded opportunity but leave no claim to hand off. */
    ret = app_wake_train_politeness_rx_activity(ret, rx_failure) ?
          0 : ret;
    if (ret != 0 && ret != -ETIMEDOUT) {
        mesh_log_standard_click_probe_done(started_ms, "fail", ret);
        return ret;
    }

    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (ret < 0) {
        mesh_log_standard_click_probe_done(started_ms, "fail", ret);
        return ret;
    }
    mesh_log_standard_click_probe_done(started_ms, "restore", 0);
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_route_reply_ack_matches(const struct mesh_outbound *route_reply,
                                         const struct mesh_frame_parse_context *parsed)
{
    if (route_reply == NULL ||
        parsed == NULL ||
        parsed->packet.msg_type != MSG_ROUTE_REPLY_ACK ||
        parsed->packet.src_id != route_reply->next_hop_id ||
        parsed->packet.dst_id != DEVICE_ID ||
        parsed->previous_hop_id != route_reply->next_hop_id ||
        parsed->packet.session_id != route_reply->packet.session_id) {
        return false;
    }

    return mesh_relay_accept_route_reply_ack(&mesh_runtime,
                                             &parsed->packet,
                                             parsed->payload,
                                             parsed->payload_len,
                                             parsed->previous_hop_id) ==
           PROTO_OK;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_listen_for_route_reply_ack(const struct mesh_outbound *route_reply,
                                           uint8_t attempt)
{
    uint8_t *frame = mesh_route_reply_ack_frame;
    struct mesh_frame_parse_context *parsed = &mesh_route_reply_ack_parsed;
    int64_t uwb_window_start_ms = -1;
    uint32_t deadline_ms;
    uint32_t latest_deadline_ms = 0u;
    bool captured = false;
    bool click_captured = false;
    struct uwb_wake_claim_frame click_claim = {0};
    uint8_t click_quality = 0u;
    uint32_t click_observed_ms = 0u;
    uint8_t capture_quality = 0u;
    int last_ret = -ETIMEDOUT;
    int release_ret;
    int ret;

    if (route_reply == NULL ||
        route_reply->packet.msg_type != MSG_ROUTE_REPLY ||
        !mesh_id_is_unicast(route_reply->next_hop_id)) {
        return -EINVAL;
    }
    if (!mesh_coordinator_mesh_work_allowed("route-reply-ack-listen")) {
        return -EBUSY;
    }

    ret = k_mutex_lock(&mesh_route_reply_ack_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    mesh_c5_contact_exchange(route_reply->next_hop_id,
                             C5_CONTACT_PURPOSE_ROUTE_REPLY,
                             k_uptime_get_32() + RREP_ACK_TIMEOUT_MS,
                             "route-reply-ack-listen");
    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh route reply ACK RX");
    if (ret < 0) {
        mesh_restart_role_scan();
        goto out_unlock;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=start next=0x%016llx seq=%u attempt=%u timeout_ms=%u",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         attempt,
                         RREP_ACK_TIMEOUT_MS);
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        uint32_t listen_start_ms = k_uptime_get_32();

        deadline_ms = listen_start_ms + RREP_ACK_TIMEOUT_MS;
        latest_deadline_ms =
            listen_start_ms + RREP_ACK_ATTEMPT_MAX_MS;
        while (true) {
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms;
            size_t frame_len = 0u;
            uint8_t quality = 0u;

            if (uptime_deadline_reached(now_ms, deadline_ms)) {
                break;
            }
            remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
            memset(parsed, 0, sizeof(*parsed));

            ret = dwm3000_driver_receive_frame_continuous(remaining_ms,
                                                          frame,
                                                          UWB_MESH_MAX_FRAME_LEN,
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
            last_ret = ret;
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                     "phase=rx-fail next=0x%016llx seq=%u attempt=%u ret=%d rx_failure=%u",
                                     (unsigned long long)route_reply->next_hop_id,
                                     route_reply->packet.seq,
                                     attempt,
                                     ret,
                                     (unsigned int)rx_failure);
                continue;
            }

            {
                bool embedded_route_frame = false;
                bool click_priority = false;

                if (mesh_frame_requires_anchor_click_handoff(frame,
                                                             frame_len,
                                                             &click_claim)) {
                    click_captured = true;
                    click_quality = quality;
                    click_observed_ms = k_uptime_get_32();
                    last_ret = -EAGAIN;
                    status_debug_note("DBG_ROUTE_REPLY_ACK_CLICK_CAPTURE\n");
                    break;
                }

                if (mesh_handle_channel5_wake_claim(frame,
                                                    frame_len,
                                                    quality,
                                                    &embedded_route_frame,
                                                    &click_priority)) {
                    uint32_t preempted_at_ms = k_uptime_get_32();

                    if (DEVICE_ROLE == ROLE_ANCHOR && click_priority) {
                        last_ret = -EAGAIN;
                        status_debug_note("DBG_ROUTE_REPLY_ACK_CLICK_PREEMPT\n");
                        high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                             "phase=click-preempt next=0x%016llx seq=%u attempt=%u",
                                             (unsigned long long)route_reply->next_hop_id,
                                             route_reply->packet.seq,
                                             attempt);
                        break;
                    }
                    deadline_ms = app_mesh_route_reply_ack_deadline_after_preemption(
                        preempted_at_ms,
                        RREP_ACK_TIMEOUT_MS,
                        latest_deadline_ms,
                        true);
                    status_debug_note("DBG_ROUTE_REPLY_ACK_PREEMPT_EXTEND\n");
                    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                         "phase=c5-preempt-click next=0x%016llx seq=%u attempt=%u extended_deadline=%u latest_deadline=%u",
                                         (unsigned long long)route_reply->next_hop_id,
                                         route_reply->packet.seq,
                                         attempt,
                                         deadline_ms,
                                         latest_deadline_ms);
                    continue;
                }
            }

            ret = uwb_mesh_frame_decode(frame,
                                        frame_len,
                                        NETWORK_ID,
                                        DEVICE_ID,
                                        &parsed->previous_hop_id,
                                        &parsed->packet,
                                        parsed->payload,
                                        sizeof(parsed->payload),
                                        &parsed->payload_len);
            if (ret != PROTO_OK || parsed->payload_len > UINT8_MAX) {
                high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                     "phase=reject next=0x%016llx seq=%u attempt=%u len=%u quality=%u decode_ret=%d",
                                     (unsigned long long)route_reply->next_hop_id,
                                     route_reply->packet.seq,
                                     attempt,
                                     (unsigned int)frame_len,
                                     quality,
                                     ret);
                continue;
            }

            if (!mesh_route_reply_ack_matches(route_reply, parsed)) {
                LOG_INF("mesh route reply ACK listen ignored unrelated mesh frame: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                        parsed->packet.msg_type,
                        (unsigned long long)parsed->packet.src_id,
                        (unsigned long long)parsed->packet.dst_id,
                        (unsigned long long)parsed->previous_hop_id,
                        parsed->packet.seq,
                        quality);
                continue;
            }

            capture_quality = quality;
            captured = true;
            status_debug_note("DBG_ROUTE_REPLY_ACK_RX\n");
            high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                 "phase=capture next=0x%016llx seq=%u ack_seq=%u attempt=%u quality=%u",
                                 (unsigned long long)route_reply->next_hop_id,
                                 route_reply->packet.seq,
                                 parsed->packet.seq,
                                 attempt,
                                 quality);
            break;
        }
    } else {
        last_ret = ret;
        high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                             "phase=config-fail next=0x%016llx seq=%u attempt=%u ret=%d",
                             (unsigned long long)route_reply->next_hop_id,
                             route_reply->packet.seq,
                             attempt,
                             ret);
    }

    release_ret = mesh_radio_standby_with_bounded_recovery(
        "route-reply-ack-listen");
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    if (release_ret < 0) {
        LOG_ERR("route-reply ACK listen radio release failed: %d",
                release_ret);
        mesh_c5_contact_clear(click_captured ?
                              "click-preempt-release-fail" :
                              "route-reply-ack-release-fail");
        k_mutex_unlock(&mesh_route_reply_ack_scratch_lock);
        return release_ret;
    }
    radio_guard_uwb_stop();

    if (click_captured) {
        bool handled;

        k_mutex_unlock(&mesh_route_reply_ack_scratch_lock);
        mesh_c5_contact_clear("click-preempt");
        handled = mesh_handoff_anchor_click_claim(&click_claim,
                                                  click_quality,
                                                  click_observed_ms);
        if (!handled) {
            mesh_restart_role_scan();
            return -EIO;
        }
        return -EAGAIN;
    }

    mesh_restart_role_scan();

    if (captured) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_CONSUMED seq=%u quality=%u\n",
                                route_reply->packet.seq,
                                capture_quality);
        }
        ret = 0;
        goto out_unlock;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=timeout next=0x%016llx seq=%u attempt=%u last_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         attempt,
                         last_ret);
    ret = last_ret;

out_unlock:
    k_mutex_unlock(&mesh_route_reply_ack_scratch_lock);
    return ret;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_send_route_reply_burst(const struct mesh_outbound *route_reply,
                                       uint8_t attempt,
                                       bool apply_turnaround_delay)
{
    uint8_t repeat_count = 1u;
    uint8_t sent_count = 0u;
    int last_ret = -EINVAL;

    if (route_reply == NULL) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        DEVICE_ROLE == ROLE_GATEWAY &&
        apply_turnaround_delay &&
        mesh_id_is_unicast(route_reply->next_hop_id)) {
        repeat_count = MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_COUNT;
        LOG_INF("mesh route-reply turnaround delay: next=0x%016llx delay_ms=%u repeats=%u gap_ms=%u attempt=%u",
                (unsigned long long)route_reply->next_hop_id,
                MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS,
                repeat_count,
                MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS,
                attempt);
        k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS);
    }

    for (uint8_t i = 0u; i < repeat_count; i++) {
        if (i > 0u) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS);
        }

        last_ret = mesh_send_c5_control(route_reply,
                                        C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                        MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                        i == 0u ? "route-reply" :
                                                  "route-reply-repeat");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_TX_REPEAT attempt=%u idx=%u ret=%d sent=%u\n",
                                attempt,
                                i,
                                last_ret,
                                sent_count + (last_ret == 0 ? 1u : 0u));
        }
        if (last_ret == 0) {
            sent_count++;
            mesh_relay_note_tx_sent(&mesh_runtime,
                                    route_reply,
                                    k_uptime_get_32());
        }
    }

    return sent_count > 0u ? 0 : last_ret;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_send_route_reply_train_to_hop(const struct mesh_outbound *route_reply)
{
    int last_ret = -EINVAL;

    if (route_reply == NULL) {
        return -EINVAL;
    }

    for (uint8_t attempt = 0u; attempt <= RREP_RETRY_COUNT_PER_HOP; attempt++) {
        struct app_mesh_route_reply_ack_attempt_state attempt_state = {
            .attempt = attempt,
            .max_retries = RREP_RETRY_COUNT_PER_HOP,
        };
        struct app_mesh_route_reply_ack_attempt_result attempt_result;

        last_ret = mesh_send_route_reply_burst(route_reply, attempt, attempt == 0u);
        attempt_state.send_ret = last_ret;
        if (last_ret == 0) {
            last_ret = mesh_listen_for_route_reply_ack(route_reply, attempt);
            attempt_state.listen_attempted = true;
            attempt_state.listen_ret = last_ret;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ATTEMPT attempt=%u send=%d listen=%d listened=%u\n",
                                attempt,
                                attempt_state.send_ret,
                                attempt_state.listen_ret,
                                attempt_state.listen_attempted ? 1u : 0u);
        }

        app_mesh_route_reply_ack_decide_attempt(&attempt_state, &attempt_result);
        if (attempt_result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS) {
            return attempt_result.return_ret;
        }
        last_ret = attempt_result.return_ret;
        if (attempt_result.note_retry) {
            mesh_relay_note_route_reply_retry(&mesh_runtime);
        }
        if (attempt_result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY) {
            continue;
        }
        break;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=failed next=0x%016llx seq=%u attempts=%u last_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         (unsigned int)(RREP_RETRY_COUNT_PER_HOP + 1u),
                         last_ret);
    return last_ret;
}
#endif

static int mesh_send_route_reply_train(const struct mesh_outbound *route_reply,
                                       bool backup_valid,
                                       uint64_t backup_next_hop_id,
                                       uint64_t *acked_next_hop_id)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound *backup_route_reply = &mesh_route_reply_backup_scratch;
    struct app_mesh_route_reply_ack_backup_state backup_state;
    struct app_mesh_route_reply_ack_backup_result backup_result;
    int primary_ret;
    int ret;

    if (acked_next_hop_id != NULL) {
        *acked_next_hop_id = 0u;
    }
    if (route_reply == NULL) {
        return -EINVAL;
    }

    ret = mesh_send_route_reply_train_to_hop(route_reply);
    if (ret == 0) {
        if (acked_next_hop_id != NULL) {
            *acked_next_hop_id = route_reply->next_hop_id;
        }
        mesh_c5_contact_clear("route-reply-acked");
        return 0;
    }

    primary_ret = ret;
    mesh_relay_abandon_route_reply_ack(&mesh_runtime, route_reply);
    backup_state.primary_ret = primary_ret;
    backup_state.backup_valid = backup_valid;
    backup_state.primary_next_hop_id = route_reply->next_hop_id;
    backup_state.backup_next_hop_id = backup_next_hop_id;
    app_mesh_route_reply_ack_decide_backup(&backup_state, &backup_result);
    if (!backup_result.try_backup) {
        mesh_c5_contact_clear(backup_result.clear_reason);
        return ret;
    }

    ret = k_mutex_lock(&mesh_route_reply_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    *backup_route_reply = *route_reply;
    backup_route_reply->next_hop_id = backup_result.backup_next_hop_id;
    if (backup_result.note_retry) {
        mesh_relay_note_route_reply_retry(&mesh_runtime);
    }
    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=backup primary=0x%016llx backup=0x%016llx seq=%u primary_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         (unsigned long long)backup_result.backup_next_hop_id,
                         route_reply->packet.seq,
                         primary_ret);

    ret = mesh_send_route_reply_train_to_hop(backup_route_reply);
    if (ret == 0 && acked_next_hop_id != NULL) {
        *acked_next_hop_id = backup_route_reply->next_hop_id;
    } else if (ret != 0) {
        mesh_relay_abandon_route_reply_ack(&mesh_runtime,
                                           backup_route_reply);
    }
    k_mutex_unlock(&mesh_route_reply_scratch_lock);
    mesh_c5_contact_clear(ret == 0 ? "route-reply-backup-acked" :
                                   "route-reply-backup-failed");
    return ret;
#else
    ARG_UNUSED(route_reply);
    ARG_UNUSED(backup_valid);
    ARG_UNUSED(backup_next_hop_id);
    ARG_UNUSED(acked_next_hop_id);
    return -ENOTSUP;
#endif
}

static bool mesh_payload_find_u32(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t type,
                                  uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    if (value == NULL ||
        tlv_find_unique(payload,
                        payload_len,
                        type,
                        &tlv_value,
                        &tlv_len) != PROTO_OK ||
        tlv_len != sizeof(uint32_t)) {
        return false;
    }

    *value = proto_get_u32_le(tlv_value);
    return true;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_payload_find_u64(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t type,
                                  uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    if (value == NULL ||
        tlv_find_unique(payload,
                        payload_len,
                        type,
                        &tlv_value,
                        &tlv_len) != PROTO_OK ||
        tlv_len != sizeof(uint64_t)) {
        return false;
    }

    *value = proto_get_u64_le(tlv_value);
    return true;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t mesh_ch9_next_batch_id(void)
{
    if (mesh_ch9_batch_next_id == 0u) {
        mesh_ch9_batch_next_id =
            sys_rand32_get() ^ k_uptime_get_32() ^ (uint32_t)DEVICE_ID;
    }
    mesh_ch9_batch_next_id++;
    if (mesh_ch9_batch_next_id == 0u) {
        mesh_ch9_batch_next_id = 1u;
    }
    return mesh_ch9_batch_next_id;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_payload_remove_ch9_batch_metadata(uint8_t *payload,
                                                  size_t payload_cap,
                                                  size_t append_reserve,
                                                  size_t *payload_len)
{
    size_t read_offset = 0u;
    size_t stripped_len = 0u;
    size_t original_len;

    if (payload == NULL || payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    original_len = *payload_len;
    if (original_len > payload_cap) {
        return PROTO_ERR_NO_SPACE;
    }

    /* Validate and size the complete TLV stream before modifying any byte. */
    while (read_offset < original_len) {
        uint8_t type;
        uint8_t len;
        size_t tlv_len;

        if (original_len - read_offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[read_offset];
        len = payload[read_offset + 1u];
        tlv_len = (size_t)len + 2u;
        if (original_len - read_offset < tlv_len) {
            return PROTO_ERR_MALFORMED;
        }

        if (type != TLV_MESH_CH9_BATCH_ID &&
            type != TLV_MESH_CH9_BATCH_FLAGS) {
            stripped_len += tlv_len;
        }
        read_offset += tlv_len;
    }
    if (append_reserve > payload_cap - stripped_len) {
        return PROTO_ERR_NO_SPACE;
    }

    read_offset = 0u;
    stripped_len = 0u;
    while (read_offset < original_len) {
        uint8_t type = payload[read_offset];
        size_t tlv_len = (size_t)payload[read_offset + 1u] + 2u;

        if (type != TLV_MESH_CH9_BATCH_ID &&
            type != TLV_MESH_CH9_BATCH_FLAGS) {
            if (stripped_len != read_offset) {
                memmove(&payload[stripped_len],
                        &payload[read_offset],
                        tlv_len);
            }
            stripped_len += tlv_len;
        }
        read_offset += tlv_len;
    }

    *payload_len = stripped_len;
    return PROTO_OK;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_outbound_set_ch9_batch_metadata(struct mesh_outbound *out,
                                                uint32_t batch_id,
                                                bool final_packet)
{
    size_t payload_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    payload_len = out->payload_len;
    ret = mesh_payload_remove_ch9_batch_metadata(
        out->payload,
        sizeof(out->payload),
        (sizeof(uint32_t) + 2u) + (sizeof(uint8_t) + 2u),
        &payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_append_u32(out->payload,
                         sizeof(out->payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_ID,
                         batch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(out->payload,
                        sizeof(out->payload),
                        &payload_len,
                        TLV_MESH_CH9_BATCH_FLAGS,
                        final_packet ? MESH_CH9_BATCH_FLAG_FINAL : 0u);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->payload_len = (uint16_t)payload_len;
    out->packet.payload_len = (uint16_t)payload_len;
    return PROTO_OK;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_outbound_clear_ch9_batch_metadata(struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    payload_len = out->payload_len;
    ret = mesh_payload_remove_ch9_batch_metadata(out->payload,
                                                 sizeof(out->payload),
                                                 0u,
                                                 &payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->payload_len = (uint16_t)payload_len;
    out->packet.payload_len = (uint16_t)payload_len;
    return PROTO_OK;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static uint8_t mesh_requeue_ch9_batch_candidates(struct mesh_outbound *candidates,
                                                 uint8_t start_index,
                                                 uint8_t count,
                                                 const char *reason)
{
    uint8_t requeued = 0u;

    if (candidates == NULL) {
        return 0u;
    }

    for (uint8_t i = start_index; i < count; i++) {
        struct mesh_outbound clean = candidates[i];
        int strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
        int ret;

        ret = queue_anchor_report(strip_ret == PROTO_OK ? &clean : &candidates[i]);
        if (ret == 0) {
            requeued++;
        } else {
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_REQUEUE idx=%u count=%u ret=%d strip=%d seq=%u r=%s\n",
                                i,
                                count,
                                ret,
                                strip_ret,
                                candidates[i].packet.seq,
                                reason == NULL ? "batch" : reason);
        }
    }
    return requeued;
}
#endif

static int mesh_ack_payload_packet_id_summary(const uint8_t *payload,
                                              size_t payload_len,
                                              uint8_t *count,
                                              uint32_t *first_id,
                                              uint32_t *last_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t id_count;
    int ret;

    if (count == NULL || first_id == NULL || last_id == NULL) {
        return PROTO_ERR_ARG;
    }
    *count = 0u;
    *first_id = 0u;
    *last_id = 0u;

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_MESH_ACK_PACKET_ID_LIST,
                          &value,
                          &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((value_len % sizeof(uint32_t)) != 0u) {
        return PROTO_ERR_MALFORMED;
    }

    id_count = value_len / sizeof(uint32_t);
    *count = id_count;
    if (id_count > 0u) {
        *first_id = proto_get_u32_le(value);
        *last_id = proto_get_u32_le(&value[(id_count - 1u) * sizeof(uint32_t)]);
    }
    return PROTO_OK;
}

static void mesh_ch9_ack_batch_queue(const struct mesh_outbound *ack,
                                     const struct mesh_rx_pending *rx)
{
    struct app_mesh_ch9_ack_batch_entry entry = {0};
    enum app_mesh_ch9_ack_queue_result queue_result =
        APP_MESH_CH9_ACK_QUEUE_ADDED;
    uint32_t packet_id = 0u;
    bool semantic_match = false;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        ack == NULL ||
        rx == NULL ||
        rx->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        (ack->packet.msg_type != MSG_GATEWAY_ACK &&
         ack->packet.msg_type != MSG_MESH_HOP_ACK)) {
        return;
    }

    ret = mesh_ack_payload_contains_packet(&ack->packet,
                                           ack->payload,
                                           ack->payload_len,
                                           &rx->packet,
                                           rx->payload,
                                           rx->payload_len,
                                           &semantic_match);
    if (ret != PROTO_OK || !semantic_match) {
        LOG_ERR("refusing channel-9 ACK without exact received-packet commitment: type=0x%02x seq=%u ret=%d match=%u",
                ack->packet.msg_type,
                rx->packet.seq,
                ret,
                semantic_match ? 1u : 0u);
        return;
    }

    entry.seq = rx->packet.seq;
    entry.session_id = rx->packet.session_id;
    entry.has_packet_id = mesh_payload_find_u32(rx->payload,
                                                rx->payload_len,
                                                TLV_MESH_TEST_PACKET_ID,
                                                &packet_id);
    entry.packet_id = entry.has_packet_id ? packet_id : 0u;
    ret = app_mesh_ch9_ack_table_queue(&mesh_ch9_ack_table,
                                       ack,
                                       &entry,
                                       &queue_result);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        switch (queue_result) {
        case APP_MESH_CH9_ACK_QUEUE_ADDED:
            status_debug_note("DBG_CH9_ACK_QUEUE_ADD\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_DUPLICATE:
            status_debug_note("DBG_CH9_ACK_QUEUE_DUP\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_REPLACED:
            status_debug_note("DBG_CH9_ACK_QUEUE_REPLACE\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_SUPPRESSED_BY_FORWARDED_ACK:
            status_debug_note("DBG_CH9_ACK_QUEUE_KEEP_GATEWAY\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_FORWARDED_BUSY:
            status_debug_note("DBG_CH9_ACK_QUEUE_FORWARDED_BUSY\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_BATCH_FULL:
        case APP_MESH_CH9_ACK_QUEUE_TABLE_FULL:
            status_debug_note("DBG_CH9_ACK_QUEUE_FULL\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_SEMANTIC_CONFLICT:
            status_debug_note("DBG_CH9_ACK_QUEUE_SEMANTIC_CONFLICT\n");
            break;
        }
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh channel-9 ACK table full: next=0x%016llx seq=%u ret=%d result=%u",
                (unsigned long long)ack->next_hop_id,
                rx->packet.seq,
                ret,
                (unsigned int)queue_result);
        return;
    }
    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
}

static int mesh_ch9_ack_batch_queue_forwarded_gateway_ack(
    const struct mesh_outbound *ack,
    const char *reason)
{
    enum app_mesh_ch9_ack_queue_result queue_result =
        APP_MESH_CH9_ACK_QUEUE_ADDED;
    uint8_t packet_id_count = 0u;
    uint32_t first_packet_id = 0u;
    uint32_t last_packet_id = 0u;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        ack == NULL ||
        ack->packet.msg_type != MSG_GATEWAY_ACK ||
        !mesh_id_is_unicast(ack->next_hop_id)) {
        return -EINVAL;
    }

    ret = app_mesh_ch9_ack_table_queue_forwarded(&mesh_ch9_ack_table,
                                                  ack,
                                                  &queue_result);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh forwarded gateway ACK table full: next=0x%016llx seq=%u ret=%d",
                (unsigned long long)ack->next_hop_id,
                ack->packet.seq,
                ret);
        return mesh_errno_from_proto(ret);
    }
    if (queue_result == APP_MESH_CH9_ACK_QUEUE_REPLACED &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_ACK_QUEUE_REPLACE_GATEWAY\n");
    }

    ret = mesh_ack_payload_packet_id_summary(ack->payload,
                                             ack->payload_len,
                                             &packet_id_count,
                                             &first_packet_id,
                                             &last_packet_id);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_FORWARD_GATEWAY_ACK_QUEUE seq=%u next=0x%llx len=%u ids=%u first=%u last=%u ret=%d reason=%s\n",
                            ack->packet.seq,
                            (unsigned long long)ack->next_hop_id,
                            ack->payload_len,
                            packet_id_count,
                            first_packet_id,
                            last_packet_id,
                            ret,
                            reason == NULL ? "forward-gateway-ack" : reason);
    }
    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    return 0;
}

static int mesh_ch9_ack_batch_build(uint64_t peer_id,
                                    struct mesh_outbound *out)
{
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    int ret;

    if (batch == NULL || out == NULL) {
        return -ENOENT;
    }
    ret = app_mesh_ch9_ack_table_build_peer(&mesh_ch9_ack_table,
                                             peer_id,
                                             out);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_ACK_BUILD peer=0x%llx n=%u forwarded=%u seq=%u len=%u\n",
                            (unsigned long long)peer_id,
                            batch->count,
                            batch->preserve_payload ? 1u : 0u,
                            out->packet.seq,
                            out->payload_len);
    }
    return 0;
}

static int mesh_commit_forwarded_gateway_ack_sent(
    const struct mesh_outbound *ack)
{
    uint32_t actions = MESH_RELAY_ACTION_NONE;
    int clear_ret;
    int ret;

    ret = mesh_relay_commit_transit_gateway_ack_forward(
        &mesh_runtime, ack, k_uptime_get_32(), &actions);
    if (ret != PROTO_OK ||
        (actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) == 0u) {
        LOG_WRN("forwarded gateway ACK send did not commit transit custody: ret=%d actions=0x%08x seq=%u",
                ret, actions, ack == NULL ? 0u : ack->packet.seq);
        return ret == PROTO_OK ? -EIO : mesh_errno_from_proto(ret);
    }

    clear_ret = mesh_save_outbox_durable("forwarded-gateway-ack");
    if (clear_ret < 0) {
        LOG_WRN("forwarded gateway ACK committed with stale durable snapshot retained for safe replay: ret=%d seq=%u",
                clear_ret, ack->packet.seq);
    }
    HIGH_DEBUG_COUNTER_INC(mesh_ack);
    status_debug_printf("DBG_FORWARD_GATEWAY_ACK_COMMIT seq=%u dst=0x%llx next=0x%llx\n",
                        ack->packet.seq,
                        (unsigned long long)ack->packet.dst_id,
                        (unsigned long long)ack->next_hop_id);
    (void)mesh_schedule_tx_timeout();
    report_tx_schedule(0u);
    return 0;
}

static int mesh_send_pending_ch9_ack_batch(const struct mesh_event_plan *plan,
                                           uint64_t peer_id,
                                           const char *reason)
{
    struct mesh_outbound ack = {0};
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    bool forwarded_gateway_ack;
    uint8_t count;
    int ret;

    if (batch == NULL || batch->count == 0u) {
        return -ENOENT;
    }

    forwarded_gateway_ack = batch->preserve_payload &&
        batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK;
    count = batch->count;
    ret = mesh_ch9_ack_batch_build(peer_id, &ack);
    if (ret < 0) {
        return ret;
    }
	    if (plan != NULL) {
	        uint32_t required_ms = 0u;
	        uint32_t now_ms = k_uptime_get_32();

	        if (!mesh_ch9_tx_fits_plan(&ack, plan, now_ms, &required_ms)) {
	            uint32_t retry_delay_ms = 0u;

	            (void)app_mesh_ch9_ack_table_note_send_failure(
	                &mesh_ch9_ack_table,
	                peer_id,
	                now_ms,
	                sys_rand32_get(),
	                &retry_delay_ms);
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                status_debug_note("DBG_CH9_ACK_BATCH_SLOT_FULL\n");
	                status_debug_printf("DBG_CH9_ACK_SLOT_FULL now=%u end=%u req=%u n=%u retry=%u\n",
	                                    now_ms,
	                                    plan->end_ms,
	                                    required_ms,
	                                    count,
	                                    retry_delay_ms);
	            }
            return -EBUSY;
        }
    }

    if (plan != NULL) {
        ack.earliest_tx_ms = mesh_ch9_slot_send_start_ms(&ack,
                                                         plan,
                                                         k_uptime_get_32());
        ack.earliest_tx_valid = true;
        mesh_ch9_event_set(CH9_EVENT_GRANTED, peer_id, plan, reason);
    }
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        status_debug_note("DBG_CH9_ACK_BATCH_TRY\n");
	    }
    if (plan != NULL) {
        mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD, peer_id, plan, reason);
    }
    ret = mesh_send_outbound(&ack, reason == NULL ? "gateway-ack-batch" : reason);
    if (ret == 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_ACK_BATCH_SENT\n");
        }
        mesh_relay_note_tx_sent(&mesh_runtime, &ack, k_uptime_get_32());
        if (plan != NULL) {
            mesh_note_channel9_local_tx(ack.next_hop_id, plan->start_ms);
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE, peer_id, plan, reason);
        }

        if (forwarded_gateway_ack) {
            ret = mesh_commit_forwarded_gateway_ack_sent(&ack);
            if (ret < 0) {
                uint32_t retry_delay_ms = 0u;

                (void)app_mesh_ch9_ack_table_note_send_failure(
                    &mesh_ch9_ack_table,
                    peer_id,
                    k_uptime_get_32(),
                    sys_rand32_get(),
                    &retry_delay_ms);
                status_debug_printf("DBG_FORWARD_GATEWAY_ACK_COMMIT_WAIT ret=%d retry=%u seq=%u\n",
                                    ret,
                                    retry_delay_ms,
                                    ack.packet.seq);
            }
        }
        if (ret == 0) {
            (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table,
                                                     peer_id);
        }
	    } else {
	        uint32_t retry_delay_ms = 0u;

	        (void)app_mesh_ch9_ack_table_note_send_failure(
	            &mesh_ch9_ack_table,
	            peer_id,
	            k_uptime_get_32(),
	            sys_rand32_get(),
	            &retry_delay_ms);
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_ACK_BATCH_FAIL\n");
	            status_debug_printf("DBG_CH9_ACK_SEND_FAIL ret=%d n=%u retry=%u\n",
	                                ret,
	                                count,
	                                retry_delay_ms);
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER, peer_id, plan, reason);
        }
	    }
    return ret;
}

static int mesh_send_current_ch9_ack_batch(uint64_t peer_id,
                                           const char *reason)
{
    struct mesh_outbound ack = {0};
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    uint8_t count;
    int ret;

    if (batch == NULL || batch->count == 0u) {
        return -ENOENT;
    }

    count = batch->count;
    ret = mesh_ch9_ack_batch_build(peer_id, &ack);
    if (ret < 0) {
        return ret;
    }

    ack.radio_channel = MESH_EVENT_CHANNEL;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_GATEWAY_BATCH_ACK_GUARD ms=%u n=%u peer=0x%llx reason=%s\n",
                            MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                            count,
                            (unsigned long long)peer_id,
                            reason == NULL ? "batch-ack" : reason);
    }
    k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
    ret = mesh_send_outbound_keep_channel9_awake(
        &ack,
        reason == NULL ? "gateway-batch-ack-current-channel9" : reason,
        NULL);
    if (ret == 0) {
        mesh_relay_note_tx_sent(&mesh_runtime, &ack, k_uptime_get_32());
        mesh_note_channel9_local_tx(ack.next_hop_id, k_uptime_get_32());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_BATCH_ACK_SENT\n");
            status_debug_printf("DBG_GATEWAY_BATCH_ACK_SENT n=%u dst=0x%llx next=0x%llx seq=%u\n",
                                count,
                                (unsigned long long)ack.packet.dst_id,
                                (unsigned long long)ack.next_hop_id,
                                ack.packet.seq);
        }
        (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer_id);
    } else {
        uint32_t retry_delay_ms = 0u;

        /* Keep table custody until the randomized not-before expires. */
        (void)app_mesh_ch9_ack_table_note_send_failure(
            &mesh_ch9_ack_table,
            peer_id,
            k_uptime_get_32(),
            sys_rand32_get(),
            &retry_delay_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_BATCH_ACK_FAIL\n");
            status_debug_printf("DBG_GATEWAY_BATCH_ACK_FAIL ret=%d retry=%u n=%u peer=0x%llx\n",
                                ret,
                                retry_delay_ms,
                                count,
                                (unsigned long long)peer_id);
        }
    }
    return ret;
}

static bool mesh_gateway_batch_ack_retry_wait_ms(uint32_t now_ms,
                                                  uint32_t *wait_ms_out)
{
    uint32_t selected_wait_ms = UINT32_MAX;
    bool found = false;

    if (DEVICE_ROLE != ROLE_GATEWAY || wait_ms_out == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        uint32_t wait_ms;

        if (!batch->valid || batch->count == 0u ||
            !batch->retry_deferred || batch->preserve_payload ||
            batch->template_ack.packet.msg_type != MSG_GATEWAY_ACK) {
            continue;
        }
        wait_ms = app_mesh_ch9_ack_table_retry_wait_ms(
            &mesh_ch9_ack_table, batch->peer_id, now_ms);
        if (!found || wait_ms < selected_wait_ms) {
            selected_wait_ms = wait_ms;
            found = true;
        }
    }
    if (found) {
        *wait_ms_out = selected_wait_ms;
    }
    return found;
}

static bool mesh_gateway_handoff_due_batch_acks(uint32_t now_ms)
{
    struct mesh_outbound ack = {0};
    bool admitted_any = false;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return false;
    }
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        uint64_t peer_id;
        uint64_t deadline_ms;
        int ret;

        if (!batch->valid || batch->count == 0u ||
            !batch->retry_deferred || batch->preserve_payload ||
            batch->template_ack.packet.msg_type != MSG_GATEWAY_ACK ||
            !app_mesh_ch9_ack_table_retry_ready(&mesh_ch9_ack_table,
                                                 batch->peer_id,
                                                 now_ms)) {
            continue;
        }

        peer_id = batch->peer_id;
        ret = mesh_ch9_ack_batch_build(peer_id, &ack);
        if (ret == 0) {
            deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;
            ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
            ack.earliest_tx_ms = 0u;
            ack.earliest_tx_valid = false;
            ret = app_node_comm_submit_control_response(
                &ack, deadline_ms, ack.packet.seq);
        }
        if (ret == 0) {
            (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table,
                                                     peer_id);
            admitted_any = true;
            status_debug_printf(
                "DBG_GATEWAY_BATCH_ACK_HANDOFF peer=0x%llx seq=%u\n",
                (unsigned long long)peer_id,
                ack.packet.seq);
        } else {
            uint32_t retry_delay_ms = 0u;

            (void)app_mesh_ch9_ack_table_note_send_failure(
                &mesh_ch9_ack_table,
                peer_id,
                now_ms,
                sys_rand32_get(),
                &retry_delay_ms);
            status_debug_printf(
                "DBG_GATEWAY_BATCH_ACK_HANDOFF_WAIT peer=0x%llx ret=%d retry=%u\n",
                (unsigned long long)peer_id,
                ret,
                retry_delay_ms);
        }
    }
    return admitted_any;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_ch9_tx_pending_clear(void)
{
    memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_ch9_tx_retry_queue_put(const struct mesh_outbound *outbound,
                                       void *ctx)
{
    struct mesh_outbound clean;
    int strip_ret;
    int ret;

    ARG_UNUSED(ctx);
    if (outbound == NULL) {
        return -EINVAL;
    }

    clean = *outbound;
    strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && strip_ret != PROTO_OK) {
        status_debug_printf("DBG_CH9_TX_RETRY_STRIP_FAIL ret=%d seq=%u\n",
                            strip_ret,
                            outbound->packet.seq);
    }
    ret = report_tx_queue_append(strip_ret == PROTO_OK ? &clean : outbound);
    if (ret == 0) {
        const struct mesh_outbound *retained =
            strip_ret == PROTO_OK ? &clean : outbound;
        uint16_t queue_depth = (uint16_t)report_tx_queue_used();

        app_stack_workload_diag_relay_admit(&retained->packet, queue_depth,
                                            (uint16_t)mesh_ch9_tx_pending.count);
        app_stack_workload_diag_relay_sample(&retained->packet, queue_depth,
                                             (uint16_t)mesh_ch9_tx_pending.count);
    }
    return ret;
}

static uint8_t mesh_ch9_tx_retry_queue_used(void *ctx)
{
    uint32_t used;

    ARG_UNUSED(ctx);
    used = report_tx_queue_used();
    return used > UINT8_MAX ? UINT8_MAX : (uint8_t)used;
}

static void mesh_ch9_tx_retry_note_drop(void *ctx)
{
    ARG_UNUSED(ctx);
    HIGH_DEBUG_COUNTER_INC(mesh_drop);
}
#endif

static uint8_t mesh_ch9_tx_pending_requeue_unacked(uint32_t now_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct app_mesh_ch9_tx_retry_entry retry_entries[MESH_CH9_TX_BATCH_MAX];
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = mesh_ch9_tx_retry_queue_put,
        .queue_used = mesh_ch9_tx_retry_queue_used,
        .note_drop = mesh_ch9_tx_retry_note_drop,
        .ctx = NULL,
    };
    struct app_mesh_ch9_tx_retry_result result;
    int ret;

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        retry_entries[i].outbound = &mesh_ch9_tx_pending.entries[i].outbound;
        retry_entries[i].acked = &mesh_ch9_tx_pending.entries[i].acked;
    }

    ret = app_mesh_ch9_tx_requeue_unacked(retry_entries,
                                          mesh_ch9_tx_pending.count,
                                          now_ms,
                                          &ops,
                                          &result);
    if (ret != PROTO_OK) {
        memset(&result, 0, sizeof(result));
        result.retained = mesh_ch9_tx_pending.count;
        mesh_ch9_tx_pending.deadline_ms = now_ms + REPORT_TX_RETRY_DELAY_MS;
    } else {
        uint8_t retained_count = 0u;

        for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
            if (!mesh_ch9_tx_pending.entries[i].acked) {
                if (retained_count != i) {
                    mesh_ch9_tx_pending.entries[retained_count] =
                        mesh_ch9_tx_pending.entries[i];
                }
                retained_count++;
            }
        }
        mesh_ch9_tx_pending.count = retained_count;
        mesh_ch9_tx_pending.active = retained_count > 0u;
        if (mesh_ch9_tx_pending.active) {
            mesh_ch9_tx_pending.deadline_ms = now_ms + REPORT_TX_RETRY_DELAY_MS;
        }
    }

    if (result.retained > 0u) {
        uint32_t oldest_age_ms = 0u;

        if (UINT32_MAX - mesh_delivery_health.ack_retry_admission_failures <
            result.retained) {
            mesh_delivery_health.ack_retry_admission_failures = UINT32_MAX;
        } else {
            mesh_delivery_health.ack_retry_admission_failures += result.retained;
        }
        for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
            uint32_t age_ms = now_ms -
                              mesh_ch9_tx_pending.entries[i].outbound.queued_at_ms;

            if (age_ms > oldest_age_ms) {
                oldest_age_ms = age_ms;
            }
        }
        if (oldest_age_ms > mesh_delivery_health.oldest_ack_pending_age_ms) {
            mesh_delivery_health.oldest_ack_pending_age_ms = oldest_age_ms;
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_REQUEUE_PARTIAL requeued=%u retained=%u dropped=%u q_before=%u q_after=%u\n",
                            result.requeued,
                            result.retained,
                            result.dropped,
                            result.queued_before,
                            result.queued_after);
    }
    return result.requeued;
#else
    ARG_UNUSED(now_ms);
    return 0u;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_ch9_tx_pending_can_start(void)
{
    return !mesh_ch9_tx_pending.active || mesh_ch9_tx_pending.count == 0u;
}
#endif

static bool mesh_ch9_tx_pending_add(const struct mesh_outbound *sent,
                                    uint32_t deadline_ms)
{
    struct mesh_ch9_tx_pending_entry *entry;
    uint32_t packet_id = 0u;

    if (!app_mesh_ch9_tx_should_track_sent(sent, DEVICE_ID)) {
        return false;
    }

    if (!mesh_ch9_tx_pending.active) {
        memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
        mesh_ch9_tx_pending.active = true;
        mesh_ch9_tx_pending.next_hop_id = sent->next_hop_id;
        mesh_ch9_tx_pending.deadline_ms = deadline_ms;
    }
    if (mesh_ch9_tx_pending.count >= MESH_CH9_TX_BATCH_MAX ||
        mesh_ch9_tx_pending.next_hop_id != sent->next_hop_id) {
        return false;
    }

    entry = &mesh_ch9_tx_pending.entries[mesh_ch9_tx_pending.count++];
    entry->outbound = *sent;
    entry->has_packet_id = mesh_payload_find_u32(sent->payload,
                                                 sent->payload_len,
                                                 TLV_MESH_TEST_PACKET_ID,
                                                 &packet_id);
    entry->packet_id = entry->has_packet_id ? packet_id : 0u;
    entry->acked = false;
    return true;
}

static bool mesh_ch9_tx_pending_track_sent(const struct mesh_outbound *sent,
                                           uint32_t deadline_ms)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !app_mesh_ch9_tx_should_track_sent(sent, DEVICE_ID)) {
        return false;
    }

    if (!app_mesh_ch9_tx_should_track_ack(
            &sent->packet,
            mesh_relay_tx_active_local_collection_result(&mesh_runtime))) {
        return false;
    }

    if (app_mesh_ch9_core_ack_wait_active(&mesh_runtime.pending,
                                          mesh_relay_tx_active(&mesh_runtime))) {
        status_debug_printf("DBG_CH9_TX_ACK_CORE_OWNER src=0x%llx seq=%u next=0x%llx deadline=%u\n",
                            (unsigned long long)sent->packet.src_id,
                            sent->packet.seq,
                            (unsigned long long)sent->next_hop_id,
                            mesh_runtime.pending.gateway_ack_deadline_ms);
        return false;
    }
    if (sent->packet.src_id == DEVICE_ID) {
        LOG_ERR("local gateway-ack TX has no durable relay-core owner: seq=%u next=0x%016llx",
                sent->packet.seq,
                (unsigned long long)sent->next_hop_id);
        return false;
    }

    if (!mesh_ch9_tx_pending_add(sent, deadline_ms)) {
        return false;
    }

    status_debug_printf("DBG_CH9_TX_ACK_TRACKED seq=%u pid=%u n=%u deadline=%u\n",
                        sent->packet.seq,
                        mesh_ch9_tx_pending.entries[mesh_ch9_tx_pending.count - 1u].packet_id,
                        mesh_ch9_tx_pending.count,
                        deadline_ms);
    mesh_relay_cancel_tx(&mesh_runtime);
    return true;
}

static bool mesh_ch9_tx_pending_handle_ack(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id,
                                           uint8_t received_radio_channel)
{
    struct app_mesh_ch9_tx_ack_entry ack_entries[MESH_CH9_TX_BATCH_MAX];
    struct app_mesh_ch9_tx_ack_result ack_result;
    uint64_t ack_peer_id = 0u;
    bool dst_matches_pending_source = false;
    int ret;

    if (packet == NULL ||
        received_radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return false;
    }
    if (packet->msg_type != MSG_GATEWAY_ACK) {
        return false;
    }
    if (!mesh_ch9_tx_pending.active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_IDLE\n");
        }
        return false;
    }
    if (payload == NULL) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_NULL_PAYLOAD\n");
        }
        return false;
    }
    if (previous_hop_id != mesh_ch9_tx_pending.next_hop_id ||
        packet->src_id != mesh_ch9_tx_pending.next_hop_id) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_ACK_PEER_MISMATCH src=0x%llx prev=0x%llx expect=0x%llx\n",
                                (unsigned long long)packet->src_id,
                                (unsigned long long)previous_hop_id,
                                (unsigned long long)mesh_ch9_tx_pending.next_hop_id);
        }
        return false;
    }
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        if (packet->dst_id == mesh_ch9_tx_pending.entries[i].outbound.packet.src_id) {
            dst_matches_pending_source = true;
            break;
        }
    }
    if (packet->dst_id != DEVICE_ID &&
        !(packet->msg_type == MSG_GATEWAY_ACK &&
          previous_hop_id == mesh_ch9_tx_pending.next_hop_id &&
          dst_matches_pending_source)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_DST_MISMATCH\n");
            status_debug_printf("DBG_CH9_TX_ACK_DST dst=0x%llx expect=0x%llx pending_src=%u prev=0x%llx next=0x%llx\n",
                                (unsigned long long)packet->dst_id,
                                (unsigned long long)DEVICE_ID,
                                dst_matches_pending_source ? 1u : 0u,
                                (unsigned long long)previous_hop_id,
                                (unsigned long long)mesh_ch9_tx_pending.next_hop_id);
        }
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint8_t id_count = 0u;
        uint32_t first_id = 0u;
        uint32_t last_id = 0u;
        int id_ret;

        status_debug_note("DBG_CH9_TX_ACK_CANDIDATE\n");
        id_ret = mesh_ack_payload_packet_id_summary(payload,
                                                    payload_len,
                                                    &id_count,
                                                    &first_id,
                                                    &last_id);
        if (id_ret == PROTO_OK) {
            status_debug_printf("DBG_CH9_TX_ACK_PACKET_IDS n=%u first=%u last=%u\n",
                                id_count,
                                first_id,
                                last_id);
        } else {
            status_debug_printf("DBG_CH9_TX_ACK_PACKET_IDS_BAD ret=%d\n", id_ret);
        }
    }

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        ack_entries[i].outbound =
            &mesh_ch9_tx_pending.entries[i].outbound;
        ack_entries[i].acked = mesh_ch9_tx_pending.entries[i].acked;
    }
    ret = app_mesh_ch9_tx_ack_apply(packet,
                                    payload,
                                    payload_len,
                                    ack_entries,
                                    mesh_ch9_tx_pending.count,
                                    &ack_result);
    if (ret != PROTO_OK) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_MALFORMED\n");
            status_debug_printf("DBG_CH9_TX_ACK_BAD_PAYLOAD ret=%d seq=%u\n",
                                ret,
                                mesh_ch9_tx_pending.count == 0u ? 0u :
                                mesh_ch9_tx_pending.entries[0].outbound.packet.seq);
        }
        return false;
    }
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        mesh_ch9_tx_pending.entries[i].acked = ack_entries[i].acked;
    }

	    if (!ack_result.any_match) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_TX_ACK_NO_MATCH\n");
	            status_debug_printf("DBG_CH9_TX_ACK_NO_MATCH n=%u first=%u ackseq=%u\n",
	                                mesh_ch9_tx_pending.count,
	                                mesh_ch9_tx_pending.count == 0u ? 0u :
	                                mesh_ch9_tx_pending.entries[0].outbound.packet.seq,
	                                packet->seq);
	        }
	        return false;
	    }

    if (!ack_result.all_acked) {
            uint8_t pending_count = mesh_ch9_tx_pending.count;
            uint8_t requeued;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_ACK_PARTIAL\n");
                status_debug_printf("DBG_CH9_TX_ACK_PARTIAL acked=%u n=%u\n",
                                    ack_result.acked_now,
                                    pending_count);
            }
            requeued = mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
            if (!mesh_ch9_tx_pending.active) {
                mesh_ch9_tx_pending_clear();
            }
            mesh_schedule_tx_timeout();
            if (requeued > 0u || report_tx_queue_used() > 0u) {
                report_tx_schedule(0u);
            }
            return true;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_COMPLETE acked=%u n=%u\n",
                            ack_result.acked_now,
                            mesh_ch9_tx_pending.count);
        if (report_tx_queue_used() > 0u) {
            status_debug_printf("DBG_CH9_TX_ACK_QUEUE_PARTIAL acked=%u queued=%u\n",
                                ack_result.acked_now,
                                report_tx_queue_used());
        }
    }
    ack_peer_id = mesh_ch9_tx_pending.next_hop_id;
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        app_stack_workload_diag_relay_release(
            &mesh_ch9_tx_pending.entries[i].outbound.packet, 0,
            (uint16_t)report_tx_queue_used(), 0u);
    }
    mesh_ch9_tx_pending_clear();
    (void)mesh_save_outbox_durable("channel9-ack-complete");
    mesh_schedule_tx_timeout();
    report_tx_schedule(0u);
    const struct app_mesh_ch9_ack_complete_state complete_state = {
        .route_test_enabled = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST),
        .transmitter_role = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
        .report_tx_queue_used = report_tx_queue_used(),
        .route_waiting_tx_valid = mesh_route_waiting_tx_valid,
        .ack_batch_valid =
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table),
    };

    if (app_mesh_ch9_ack_complete_should_close_timing(&complete_state)) {
        mesh_close_channel9_connection(ack_peer_id, "ch9-idle-ack-complete");
    }
    return true;
}
#else
static void mesh_ch9_tx_pending_clear(void)
{
}

static bool mesh_ch9_tx_pending_track_sent(const struct mesh_outbound *sent,
                                           uint32_t deadline_ms)
{
    ARG_UNUSED(sent);
    ARG_UNUSED(deadline_ms);
    return false;
}

static bool mesh_ch9_tx_pending_handle_ack(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id,
                                           uint8_t received_radio_channel)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    return false;
}
#endif

static uint16_t mesh_route_wake_claimed_duration_ms(
    uint16_t wake_train_ends_in_ms,
    const struct app_clicker_wake_train_config *config)
{
    uint32_t claimed_ms;

    if (config == NULL) {
        return wake_train_ends_in_ms;
    }

    claimed_ms = (uint32_t)wake_train_ends_in_ms +
                 config->post_wake_claimed_duration_ms;
    return claimed_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)claimed_ms;
}

static int mesh_route_wake_sniff_activity(const char *phase,
                                          uint64_t target_id,
                                          const char *reason,
                                          uint8_t retry_index,
                                          bool *activity)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int release_ret;
    int ret;

    if (activity == NULL) {
        return -EINVAL;
    }
    *activity = false;

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_sniff_activity(APP_WAKE_TRAIN_POLITE_SNIFF_MS,
                                        &rx_failure);
    release_ret = mesh_radio_standby_with_bounded_recovery(
        "route-wake-politeness");
    if (release_ret < 0) {
        LOG_ERR("mesh route wake sniff radio release failed: phase=%s ret=%d",
                phase == NULL ? "unknown" : phase,
                release_ret);
        return release_ret;
    }

    *activity = app_wake_train_politeness_rx_activity(ret, rx_failure);
    if (*activity) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_WAKE_POLITE_ACTIVITY phase=%s target=0x%llx retry=%u ret=%d fail=%u reason=%s\n",
                                phase == NULL ? "unknown" : phase,
                                (unsigned long long)target_id,
                                retry_index,
                                ret,
                                (unsigned int)rx_failure,
                                reason == NULL ? "route" : reason);
        }
        LOG_INF("mesh route wake train C5 activity during %s sniff: target=0x%016llx retry=%u ret=%d failure=%u reason=%s",
                phase == NULL ? "unknown" : phase,
                (unsigned long long)target_id,
                retry_index,
                ret,
                (unsigned int)rx_failure,
                reason == NULL ? "route" : reason);
        return 0;
    }

    return ret == -ETIMEDOUT ? 0 : ret;
}

static void mesh_route_wake_backoff(uint64_t target_id,
                                    const char *phase,
                                    const char *reason,
                                    uint8_t retry_index)
{
    uint32_t delay_ms = app_wake_train_politeness_backoff_ms(
        retry_index,
        sys_rand32_get());

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_WAKE_POLITE_BACKOFF phase=%s target=0x%llx retry=%u/%u delay=%u reason=%s\n",
                            phase == NULL ? "unknown" : phase,
                            (unsigned long long)target_id,
                            (uint32_t)retry_index + 1u,
                            APP_WAKE_TRAIN_POLITE_MAX_RETRIES,
                            delay_ms,
                            reason == NULL ? "route" : reason);
    }
    LOG_INF("mesh route wake train deferred after C5 activity: target=0x%016llx retry=%u/%u phase=%s backoff_ms=%u reason=%s",
            (unsigned long long)target_id,
            (uint32_t)retry_index + 1u,
            APP_WAKE_TRAIN_POLITE_MAX_RETRIES,
            phase == NULL ? "unknown" : phase,
            delay_ms,
            reason == NULL ? "route" : reason);
    k_msleep(delay_ms);
}

static int mesh_route_wake_listen_for_click(
    uint8_t *frame,
    struct uwb_wake_claim_frame *click_claim,
    uint8_t *click_quality,
    uint32_t *click_observed_ms,
    bool *activity)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    int ret;

    if (frame == NULL || click_claim == NULL || click_quality == NULL ||
        click_observed_ms == NULL || activity == NULL) {
        return -EINVAL;
    }

    *activity = false;
    ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
        ANCHOR_UWB_SCAN_RX_MS,
        ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
        frame,
        MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
        &frame_len,
        &quality,
        NULL,
        &rx_failure);
    *activity = app_wake_train_politeness_rx_activity(ret, rx_failure);
    if (ret == 0 &&
        mesh_frame_requires_anchor_click_handoff(frame,
                                                 frame_len,
                                                 click_claim)) {
        *click_quality = quality;
        *click_observed_ms = k_uptime_get_32();
        status_debug_printf("DBG_ROUTE_WAKE_CLICK_CAPTURE evt=%u attempt=%u q=%u\n",
                            click_claim->click_event_id,
                            click_claim->attempt_index,
                            quality);
        return 1;
    }

    if (*activity) {
        status_debug_printf("DBG_ROUTE_WAKE_RX_ACTIVITY ret=%d fail=%u len=%u click=0\n",
                            ret,
                            (unsigned int)rx_failure,
                            (unsigned int)frame_len);
        return 0;
    }

    return ret == -ETIMEDOUT ? 0 : ret;
}

static int mesh_send_route_wake_train(uint64_t target_id,
                                      const struct mesh_outbound *embedded_route_req,
                                      bool *embedded_sent,
                                      uint8_t purpose,
                                      const char *reason)
{
    struct uwb_clicker_session *session = &mesh_route_wake_session_scratch;
    struct uwb_clicker_config *config = &mesh_route_wake_config_scratch;
    const struct app_clicker_wake_train_config wake_train_config = {
        .wake_adv_ms = WAKE_ADV_MS,
        .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
        .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    };
    uint8_t *route_suffix = mesh_route_wake_suffix_scratch;
    uint8_t *frame = mesh_route_wake_frame_scratch;
    size_t route_suffix_len = 0u;
    bool embed_route = false;
    uint32_t route_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    uint32_t event_seq;
    uint32_t next_click_rx_ms = 0u;
    uint32_t click_observed_ms = 0u;
    int64_t close_ms;
    uint16_t sent_count = 0u;
    uint16_t embedded_count = 0u;
    uint8_t polite_retry = 0u;
    uint8_t click_quality = 0u;
    bool c5_activity = false;
    bool click_captured = false;
    bool local_can_range_clicks =
        DEVICE_ROLE == ROLE_ANCHOR &&
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);
    struct uwb_wake_claim_frame click_claim = {0};
    const char *activity_phase = NULL;
    int release_ret;
    int ret;

    if (embedded_sent != NULL) {
        *embedded_sent = false;
    }
    if (target_id != MESH_BROADCAST_ID &&
        (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID)) {
        return -EINVAL;
    }
    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-wake-train" : reason)) {
        return -EBUSY;
    }

    ret = k_mutex_lock(&mesh_route_wake_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    event_seq = nonzero_uptime_session_id();
    memset(config, 0, sizeof(*config));
    config->network_id = NETWORK_ID;
    config->clicker_id = DEVICE_ID;
    config->click_event_id = event_seq;
    config->nonce = clicker_nonce(event_seq);
    config->min_anchor_count = 1u;
    config->max_anchor_count = 1u;
    config->max_attempts = 1u;
    config->samples_per_anchor = 1u;
    config->wake_channel = UWB_CHANNEL_WAKE_CONTACT;
    config->ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
    config->flags = FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    if (purpose != C5_CONTACT_PURPOSE_ROUTE_SOLICIT) {
        config->flags |= FLAG_CONTROL_FOLLOWUP;
    }

    ret = uwb_clicker_session_start(session, config);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh channel-5 wake-train session rejected: target=0x%016llx ret=%d reason=%s",
                (unsigned long long)target_id,
                ret,
                reason == NULL ? "route" : reason);
        ret = -EINVAL;
        goto out_unlock;
    }

    /*
     * The mandatory route ancestry makes every current request larger than
     * the standard-PHR wake suffix. Preserve the encoder for a future compact
     * schema, but do not attempt or warn on the expected standalone path.
     */
    if (embedded_route_req != NULL &&
        embedded_route_req->payload_len <= MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        route_reply_window_ms =
            mesh_route_reply_listen_window_ms(embedded_route_req->packet.ttl);
        ret = mesh_route_wake_encode_suffix(embedded_route_req,
                                            route_suffix,
                                            MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN,
                                            &route_suffix_len);
        if (ret == 0) {
            embed_route = true;
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_SUFFIX payload=%u suffix=%u cap=%u\n",
                                embedded_route_req->payload_len,
                                (unsigned int)route_suffix_len,
                                (unsigned int)MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN);
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_WINDOW ttl=%u win=%u\n",
                                embedded_route_req->packet.ttl,
                                route_reply_window_ms);
        } else {
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_DISABLED ret=%d payload=%u cap=%u\n",
                                ret,
                                embedded_route_req->payload_len,
                                (unsigned int)MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN);
            LOG_WRN("mesh channel-5 wake-train embedded request disabled: target=0x%016llx ret=%d reason=%s",
                    (unsigned long long)target_id,
                    ret,
                    reason == NULL ? "route" : reason);
        }
    }

wake_train_attempt:
    sent_count = 0u;
    embedded_count = 0u;
    c5_activity = false;
    activity_phase = NULL;

    mesh_stop_role_scan();
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=start target=0x%016llx event_seq=%u embed=%u retry=%u reason=%s",
                         (unsigned long long)target_id,
                         event_seq,
                         embed_route ? 1u : 0u,
                         polite_retry,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train start: target=0x%016llx event_seq=%u embed=%u retry=%u reason=%s",
            (unsigned long long)target_id,
            event_seq,
            embed_route ? 1u : 0u,
            polite_retry,
            reason == NULL ? "route" : reason);
    mesh_c5_contact_open(target_id,
                         purpose,
                         event_seq,
                         true,
                         k_uptime_get_32() + wake_train_config.wake_adv_ms +
                         wake_train_config.post_wake_claimed_duration_ms,
                         reason);

    ret = mesh_transport_radio_start("mesh route WAKE_CLAIM train");
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_GUARD_FAIL\n");
        LOG_WRN("mesh route UWB WAKE_CLAIM guard failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        mesh_c5_contact_clear("wake-train-guard-fail");
        mesh_restart_role_scan();
        goto out_unlock;
    }
    status_debug_note("DBG_WAKE_TRAIN_GUARD_OK\n");

    ret = mesh_route_wake_sniff_activity("pre",
                                         target_id,
                                         reason,
                                         polite_retry,
                                         &c5_activity);
    if (ret < 0) {
        goto out;
    }
    if (c5_activity) {
        activity_phase = "pre";
        ret = -EAGAIN;
        goto out;
    }

    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    status_debug_note("DBG_WAKE_TRAIN_CONFIG_BEGIN\n");
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_CONFIG_FAIL\n");
        LOG_WRN("mesh route UWB WAKE_CLAIM wake-mode config failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        goto out;
    }
    status_debug_note("DBG_WAKE_TRAIN_CONFIG_OK\n");

    close_ms = k_uptime_get() + wake_train_config.wake_adv_ms;
    next_click_rx_ms = k_uptime_get_32() + MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS;
    while (k_uptime_get() < close_ms) {
        struct uwb_wake_claim_frame claim;
        size_t frame_len = 0u;
        int64_t remaining_ms = close_ms - k_uptime_get();
        uint16_t remaining_u16 = delay_ms_to_u16(remaining_ms);

        ret = uwb_clicker_build_wake_claim(session,
                                           clicker_priority_id(event_seq,
                                                               session->attempt_index),
                                           remaining_u16,
                                           remaining_u16,
                                           mesh_route_wake_claimed_duration_ms(
                                               remaining_u16,
                                               &wake_train_config),
                                           &claim);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_BUILD_FAIL\n");
            LOG_WRN("mesh route UWB WAKE_CLAIM build failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }

        ret = uwb_encode_wake_claim(&claim,
                                    frame,
                                    MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
                                    &frame_len);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_ENCODE_FAIL\n");
            LOG_WRN("mesh route UWB WAKE_CLAIM encode failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }

        if (embed_route &&
            frame_len + route_suffix_len <= MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN &&
            k_uptime_get() < close_ms) {
            memcpy(&frame[frame_len], route_suffix, route_suffix_len);
            frame_len += route_suffix_len;
            ret = dwm3000_driver_send_frame(frame,
                                            frame_len,
                                            wake_train_config.control_tx_timeout_ms);
            if (ret < 0) {
                status_debug_note("DBG_EMBEDDED_ROUTE_REQ_TX_FAIL\n");
                LOG_WRN("mesh route embedded WAKE_CLAIM send failed: target=0x%016llx sent=%u embedded=%u ret=%d frame_len=%u",
                        (unsigned long long)target_id,
                        sent_count,
                        embedded_count,
                        ret,
                        (unsigned int)frame_len);
                break;
            }
            status_debug_note("DBG_EMBEDDED_ROUTE_REQ_TX\n");
            status_debug_tx_wake_claim_sent_pulse();
            sent_count++;
            embedded_count++;
            HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
            uwb_clicker_note_wake_claim_tx(session, 1u);
        }

        if (k_uptime_get() >= close_ms) {
            continue;
        }

        ret = uwb_encode_wake_claim(&claim,
                                    frame,
                                    MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
                                    &frame_len);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_ENCODE_FAIL\n");
            LOG_WRN("mesh route plain WAKE_CLAIM encode failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }
        if (sent_count == 0u) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_BEGIN\n");
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        wake_train_config.control_tx_timeout_ms);
        if (ret < 0) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_FAIL\n");
            LOG_WRN("mesh route plain WAKE_CLAIM send failed: target=0x%016llx sent=%u ret=%d frame_len=%u",
                    (unsigned long long)target_id,
                    sent_count,
                    ret,
                    (unsigned int)frame_len);
            break;
        }
        if (sent_count == 0u) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_OK\n");
        }
        status_debug_tx_wake_claim_sent_pulse();
        sent_count++;
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
        uwb_clicker_note_wake_claim_tx(session, 1u);

        if (local_can_range_clicks &&
            uptime_deadline_reached(k_uptime_get_32(), next_click_rx_ms)) {
            bool rx_activity = false;
            int listen_ret;

            next_click_rx_ms = k_uptime_get_32() +
                               MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS;
            listen_ret = mesh_route_wake_listen_for_click(frame,
                                                          &click_claim,
                                                          &click_quality,
                                                          &click_observed_ms,
                                                          &rx_activity);
            if (listen_ret > 0) {
                click_captured = true;
                ret = -ECANCELED;
                goto out;
            }
            if (rx_activity) {
                c5_activity = true;
                activity_phase = "train";
                ret = -EAGAIN;
                goto out;
            }
            if (listen_ret < 0) {
                status_debug_printf("DBG_ROUTE_WAKE_CLICK_RX_FAIL ret=%d sent=%u\n",
                                    listen_ret,
                                    sent_count);
                ret = listen_ret;
                goto out;
            }
            ret = 0;
        }

        if (k_uptime_get() < close_ms) {
            uint32_t jitter_us = uwb_clicker_wake_claim_jitter_us(sys_rand32_get());
            int64_t remaining_after_tx_ms = close_ms - k_uptime_get();

            if (jitter_us > 0u && remaining_after_tx_ms > 0) {
                k_busy_wait(jitter_us);
            }
        }
    }

    if (ret >= 0 && sent_count > 0u) {
        ret = mesh_route_wake_sniff_activity("post",
                                             target_id,
                                             reason,
                                             polite_retry,
                                             &c5_activity);
        if (ret < 0) {
            goto out;
        }
        if (c5_activity) {
            activity_phase = "post";
            ret = -EAGAIN;
            goto out;
        }
    }

out:
    release_ret = mesh_radio_standby_with_bounded_recovery(
        "route-wake-train");
    if (release_ret < 0) {
        LOG_ERR("mesh route wake radio release failed: %d",
                release_ret);
        mesh_c5_contact_clear(click_captured ?
                              "click-preempt-release-fail" :
                              "wake-train-release-fail");
        click_captured = false;
        ret = release_ret;
        goto out_unlock;
    }
    radio_guard_uwb_stop();

    if (click_captured) {
        mesh_c5_contact_clear("click-preempt");
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
        high_debug_log_event("MESH_CH5_WAKE_TX",
                             "phase=click-preempt target=0x%016llx event_seq=%u click_event=%u attempt=%u wake_claims=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             click_claim.click_event_id,
                             click_claim.attempt_index,
                             sent_count,
                             reason == NULL ? "route" : reason);
        goto out_unlock;
    }
    if (ret < 0) {
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        mesh_c5_contact_clear("wake-train-fail");
        if (ret == -EAGAIN && c5_activity &&
            polite_retry < APP_WAKE_TRAIN_POLITE_MAX_RETRIES) {
            mesh_restart_role_scan();
            mesh_route_wake_backoff(target_id,
                                    activity_phase,
                                    reason,
                                    polite_retry);
            polite_retry++;
            goto wake_train_attempt;
        }
        high_debug_log_event("MESH_CH5_WAKE_TX",
                             "phase=fail target=0x%016llx event_seq=%u ret=%d wake_claims=%u embedded=%u retry=%u activity=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             ret,
                             sent_count,
                             embedded_count,
                             polite_retry,
                             c5_activity ? 1u : 0u,
                             reason == NULL ? "route" : reason);
        LOG_WRN("mesh route channel-5 wake train failed: target=0x%016llx ret=%d wake_claims=%u embedded=%u retry=%u activity=%u phase=%s reason=%s",
                (unsigned long long)target_id,
                ret,
                sent_count,
                embedded_count,
                polite_retry,
                c5_activity ? 1u : 0u,
                activity_phase == NULL ? "none" : activity_phase,
                reason == NULL ? "route" : reason);
        goto out_unlock;
    }
    if (embedded_sent != NULL) {
        *embedded_sent = embedded_count > 0u;
    }
    if (sent_count > 0u) {
        mesh_c5_contact_accept(target_id,
                               C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                               k_uptime_get_32() +
                               wake_train_config.post_wake_claimed_duration_ms +
                               route_reply_window_ms,
                               reason);
    } else {
        mesh_c5_contact_clear("wake-train-timeout");
    }
    stage1_led_result(sent_count == 0u ?
                      STAGE1_LED_RESULT_TIMEOUT :
                      STAGE1_LED_RESULT_OK);
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=done target=0x%016llx event_seq=%u ret=%d wake_claims=%u embedded=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             sent_count == 0u ? -ETIMEDOUT : 0,
                             session->diagnostics.wake_claim_tx_count,
                             embedded_count,
                             reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train complete: target=0x%016llx ret=%d wake_claims=%u embedded=%u reason=%s",
            (unsigned long long)target_id,
            sent_count == 0u ? -ETIMEDOUT : 0,
            sent_count,
            embedded_count,
            reason == NULL ? "route" : reason);
    ret = sent_count == 0u ? -ETIMEDOUT : 0;

out_unlock:
    k_mutex_unlock(&mesh_route_wake_scratch_lock);
    if (click_captured) {
        bool handled = mesh_handoff_anchor_click_claim(&click_claim,
                                                       click_quality,
                                                       click_observed_ms);

        return handled ? -ECANCELED : -EIO;
    }
    return ret;
}

static uint8_t mesh_c5_listener_purpose(const char *reason)
{
    if (reason != NULL && strcmp(reason, "event-accept") == 0) {
        return C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION;
    }
    if (reason != NULL && strcmp(reason, "gateway-command-wake-followup") == 0) {
        return C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD;
    }
    return C5_CONTACT_PURPOSE_ROUTE_SOLICIT;
}

static bool mesh_route_capture_find_u32(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint32_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (value == NULL ||
        tlv_find_unique(payload,
                        payload_len,
                        type,
                        &encoded,
                        &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(*value)) {
        return false;
    }

    *value = proto_get_u32_le(encoded);
    return true;
}

static bool mesh_route_capture_find_u16(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint16_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (value == NULL ||
        tlv_find_unique(payload,
                        payload_len,
                        type,
                        &encoded,
                        &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(*value)) {
        return false;
    }

    *value = proto_get_u16_le(encoded);
    return true;
}

static bool mesh_route_capture_find_u64(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint64_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (value == NULL ||
        tlv_find_unique(payload,
                        payload_len,
                        type,
                        &encoded,
                        &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(*value)) {
        return false;
    }

    *value = proto_get_u64_le(encoded);
    return true;
}

static bool mesh_route_capture_identity_from_request(
    const struct mesh_outbound *request,
    uint64_t target_id,
    struct mesh_route_capture_identity *identity)
{
    uint32_t flood_epoch_id = 0u;

    if (request == NULL || identity == NULL ||
        request->packet.msg_type != MSG_ROUTE_REQ ||
        request->packet.session_id == 0u ||
        !mesh_id_is_unicast(target_id) ||
        !mesh_route_capture_find_u32(request->payload,
                                     request->payload_len,
                                     TLV_FLOOD_EPOCH_ID,
                                     &flood_epoch_id) ||
        flood_epoch_id == 0u) {
        return false;
    }

    *identity = (struct mesh_route_capture_identity) {
        .session_id = request->packet.session_id,
        .flood_epoch_id = flood_epoch_id,
        .reply_nonce = mesh_route_reply_nonce(request->packet.src_id,
                                              target_id,
                                              request->packet.session_id,
                                              flood_epoch_id),
    };
    return identity->reply_nonce != 0u;
}

static int mesh_listen_for_route_reply(uint64_t target_id,
                                       const char *reason,
                                       uint32_t window_ms,
                                       const struct mesh_route_capture_identity *identity,
                                       bool *route_reply_captured)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    size_t capture_count = 0u;
    bool captured_route_reply = false;
    bool yielded_to_competing_route_request = false;
    bool hold_post_rx_response_contact = false;
    bool click_captured = false;
    struct uwb_wake_claim_frame click_claim = {0};
    uint8_t click_quality = 0u;
    uint32_t click_observed_ms = 0u;
    bool rx_failure_diagnostic_logged = false;
    atomic_val_t route_ready_generation;
    int64_t uwb_window_start_ms = -1;
    uint32_t deadline_ms;
    uint8_t contact_purpose;
    int last_ret = -ETIMEDOUT;
    int release_ret;
    int ret;

    if (route_reply_captured != NULL) {
        *route_reply_captured = false;
    }

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY) {
        return 0;
    }
    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }
    if (window_ms == 0u) {
        window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
    route_ready_generation = atomic_get(&mesh_route_ready_generation);
    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-reply-listen" : reason)) {
        return -EBUSY;
    }

    contact_purpose = mesh_c5_listener_purpose(reason);
    mesh_c5_contact_exchange(target_id,
                             contact_purpose,
                             k_uptime_get_32() + window_ms,
                             reason);

    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh route reply RX");
    if (ret < 0) {
        mesh_c5_contact_clear("reply-listen-guard-fail");
        mesh_restart_role_scan();
        return ret;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=start target=0x%016llx window_ms=%u reason=%s",
                         (unsigned long long)target_id,
                         window_ms,
                         reason == NULL ? "route" : reason);
    status_debug_note("DBG_ROUTE_REPLY_LISTEN_START\n");
    LOG_INF("mesh route reply listen start: target=0x%016llx window_ms=%u reason=%s",
            (unsigned long long)target_id,
            window_ms,
            reason == NULL ? "route" : reason);

    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_LISTEN_CONFIG ret=%d ext=1\n", ret);
    }
    if (ret == 0) {
        deadline_ms = k_uptime_get_32() + window_ms;
        while (capture_count < MESH_ROUTE_TEST_REPLY_CAPTURE_MAX) {
            struct mesh_frame_parse_context parsed = {0};
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint32_t captured_flood_epoch_id = 0u;
            uint16_t captured_reply_nonce = 0u;
            uint64_t captured_route_request_target_id = 0u;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms;
            size_t frame_len = 0u;
            uint8_t quality = 0u;

            if (uptime_deadline_reached(now_ms, deadline_ms)) {
                break;
            }
            remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);

            if (atomic_get(&mesh_route_ready_generation) !=
                route_ready_generation) {
                captured_route_reply = true;
                last_ret = 0;
                if (route_reply_captured != NULL) {
                    *route_reply_captured = true;
                }
                status_debug_note("DBG_ROUTE_REPLY_LISTEN_ROUTE_READY\n");
                break;
            }

            ret = dwm3000_driver_receive_frame_continuous(
                                                          MIN(remaining_ms,
                                                              MESH_ROUTE_REPLY_READY_POLL_MS),
                                                          frame,
                                                          UWB_MESH_MAX_FRAME_LEN,
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
            last_ret = ret;
            if (ret == -ETIMEDOUT) {
                continue;
            }
            if (ret < 0) {
                if (DEVICE_ROLE == ROLE_ANCHOR &&
                    app_wake_train_politeness_rx_activity(ret, rx_failure) &&
                    contact_purpose !=
                        C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD) {
                    int probe_ret = mesh_probe_standard_click_claim(
                        frame,
                        UWB_MESH_MAX_FRAME_LEN,
                        &click_claim,
                        &click_quality,
                        &click_observed_ms);

                    if (probe_ret > 0) {
                        click_captured = true;
                        last_ret = -EAGAIN;
                        status_debug_note("DBG_ROUTE_REPLY_CLICK_PREEMPT_PROBE\n");
                        break;
                    }
                    if (probe_ret < 0) {
                        last_ret = probe_ret;
                        status_debug_printf("DBG_ROUTE_REPLY_CLICK_PROBE_FAIL ret=%d\n",
                                            probe_ret);
                        break;
                    }
                } else if (DEVICE_ROLE == ROLE_ANCHOR &&
                           app_wake_train_politeness_rx_activity(
                               ret, rx_failure) &&
                           contact_purpose ==
                               C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD) {
                    /*
                     * The decoded CONTROL_FOLLOWUP wake already transferred
                     * this bounded exchange to the gateway-control listener.
                     * Repeated standard-PHR claims belong to that same wake
                     * train; stay on extended PHR so a zero-turnaround older
                     * sender cannot make the following control frame partial.
                     */
                    status_debug_note(
                        "DBG_C5_CONTROL_LISTENER_HOLD_EXTENDED\n");
                }
                if (!rx_failure_diagnostic_logged &&
                    IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    struct dwm3000_rx_debug_snapshot snapshot = {0};

                    dwm3000_driver_last_rx_debug_get(&snapshot);
                    status_debug_printf(
                        "DBG_C5_RX_FAIL status=0x%08x finfo=0x%08x fail=%u phy=%u ch=%u plen=%u pac=%u txc=%u rxc=%u sfd=%u rate=%u phr=%u phrr=%u sfdto=%u\n",
                        snapshot.status,
                        snapshot.rx_finfo,
                        (unsigned int)rx_failure,
                        snapshot.phy_mode,
                        snapshot.channel,
                        snapshot.preamble_length,
                        snapshot.pac,
                        snapshot.tx_code,
                        snapshot.rx_code,
                        snapshot.sfd_type,
                        snapshot.data_rate,
                        snapshot.phr_mode,
                        snapshot.phr_rate,
                        snapshot.sfd_timeout);
                    rx_failure_diagnostic_logged = true;
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=rx-fail target=0x%016llx ret=%d rx_failure=%u reason=%s",
                                     (unsigned long long)target_id,
                                     ret,
                                     (unsigned int)rx_failure,
                                     reason == NULL ? "route" : reason);
                LOG_WRN("mesh route reply RX failed: target=0x%016llx ret=%d rx_failure=%u reason=%s",
                        (unsigned long long)target_id,
                        ret,
                        (unsigned int)rx_failure,
                        reason == NULL ? "route" : reason);
                continue;
            }

            status_debug_note("DBG_ROUTE_REPLY_RX_FRAME\n");
            {
                bool embedded_route_frame = false;
                bool click_priority = false;

                if (mesh_frame_requires_anchor_click_handoff(frame,
                                                             frame_len,
                                                             &click_claim)) {
                    click_captured = true;
                    click_quality = quality;
                    click_observed_ms = k_uptime_get_32();
                    last_ret = -EAGAIN;
                    status_debug_note("DBG_ROUTE_REPLY_CLICK_CAPTURE\n");
                    break;
                }

                if (mesh_handle_channel5_wake_claim(frame,
                                                    frame_len,
                                                    quality,
                                                    &embedded_route_frame,
                                                    &click_priority)) {
                    uint32_t preempted_at_ms = k_uptime_get_32();

                    if (DEVICE_ROLE == ROLE_ANCHOR && click_priority) {
                        last_ret = -EAGAIN;
                        status_debug_note("DBG_ROUTE_REPLY_CLICK_PREEMPT\n");
                        high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                             "phase=click-preempt target=0x%016llx reason=%s",
                                             (unsigned long long)target_id,
                                             reason == NULL ? "route" : reason);
                        break;
                    }
                    deadline_ms = app_mesh_route_reply_ack_deadline_after_preemption(
                        preempted_at_ms,
                        window_ms,
                        deadline_ms,
                        true);
                    status_debug_note("DBG_ROUTE_REPLY_RX_PREEMPT_BOUNDED\n");
                    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                         "phase=c5-preempt target=0x%016llx cumulative_deadline=%u reason=%s",
                                         (unsigned long long)target_id,
                                         deadline_ms,
                                         reason == NULL ? "route" : reason);
                    continue;
                }
            }
            ret = uwb_mesh_frame_decode(frame,
                                        frame_len,
                                        NETWORK_ID,
                                        DEVICE_ID,
                                        &parsed.previous_hop_id,
                                        &parsed.packet,
                                        parsed.payload,
                                        sizeof(parsed.payload),
                                        &parsed.payload_len);
            if (ret != PROTO_OK || parsed.payload_len > UINT8_MAX) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_DECODE_FAIL ret=%d len=%u q=%u\n",
                                        ret,
                                        (unsigned int)frame_len,
                                        quality);
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=reject target=0x%016llx len=%u quality=%u decode_ret=%d reason=%s",
                                     (unsigned long long)target_id,
                                     (unsigned int)frame_len,
                                     quality,
                                     ret,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen ignored non-mesh frame: target=0x%016llx len=%u quality=%u ret=%d",
                        (unsigned long long)target_id,
                        (unsigned int)frame_len,
                        quality,
                        ret);
                continue;
            }
            ret = mesh_packet_rx_envelope_validate(
                &parsed.packet,
                parsed.payload,
                parsed.payload_len,
                parsed.previous_hop_id,
                DEVICE_ID,
                GATEWAY_ID,
                UWB_CHANNEL_WAKE_CONTACT,
                IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST));
            if (ret != PROTO_OK) {
                status_debug_printf(
                    "DBG_ROUTE_REPLY_ENVELOPE_REJECT msg=0x%02x src=0x%llx prev=0x%llx ret=%d\n",
                    parsed.packet.msg_type,
                    (unsigned long long)parsed.packet.src_id,
                    (unsigned long long)parsed.previous_hop_id,
                    ret);
                continue;
            }

            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                status_debug_note("DBG_ROUTE_REPLY_RX\n");
            } else if (parsed.packet.msg_type == MSG_GATEWAY_ROUTE_ADV) {
                status_debug_note("DBG_GATEWAY_ROUTE_ADV_RX\n");
            } else if (parsed.packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_ACCEPT ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_UPDATE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_END) {
                status_debug_note("DBG_EVENT_CTRL_RX\n");
            }

            if (app_mesh_rx_policy_should_drop(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
                                               parsed.packet.msg_type)) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_POLICY_DROP msg=0x%02x prev=0x%llx seq=%u\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.previous_hop_id,
                                        parsed.packet.seq);
                }
                continue;
            }
            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                int validate_ret = mesh_relay_validate_route_reply(
                    &mesh_runtime,
                    &parsed.packet,
                    parsed.payload,
                    parsed.payload_len,
                    parsed.previous_hop_id,
                    k_uptime_get_32());

                if (validate_ret != PROTO_OK) {
                    status_debug_printf(
                        "DBG_ROUTE_REPLY_IDENTITY_REJECT prev=0x%llx seq=%u ret=%d\n",
                        (unsigned long long)parsed.previous_hop_id,
                        parsed.packet.seq,
                        validate_ret);
                    continue;
                }
            } else if (parsed.packet.msg_type ==
                       MSG_GATEWAY_ROUTE_ADV) {
                int validate_ret = mesh_relay_validate_gateway_route_adv(
                    &mesh_runtime,
                    &parsed.packet,
                    parsed.payload,
                    parsed.payload_len,
                    parsed.previous_hop_id);

                if (validate_ret != PROTO_OK) {
                    status_debug_printf(
                        "DBG_GATEWAY_ROUTE_ADV_IDENTITY_REJECT prev=0x%llx seq=%u ret=%d\n",
                        (unsigned long long)parsed.previous_hop_id,
                        parsed.packet.seq,
                        validate_ret);
                    continue;
                }
            }

            bool gateway_priority_control =
                DEVICE_ROLE == ROLE_ANCHOR &&
                parsed.packet.src_id == GATEWAY_ID &&
                (parsed.packet.msg_type == MSG_COMMAND ||
                 parsed.packet.msg_type == MSG_SURVEY_PAIR_PREPARE ||
                 parsed.packet.msg_type == MSG_SURVEY_DISCOVERY_START);
            bool targeted_control_relay = false;

            if (gateway_priority_control &&
                (parsed.packet.msg_type == MSG_COMMAND ||
                 parsed.packet.msg_type == MSG_SURVEY_PAIR_PREPARE) &&
                mesh_id_is_unicast(parsed.packet.dst_id) &&
                parsed.packet.dst_id != DEVICE_ID &&
                parsed.packet.ttl > 1u) {
                const struct mesh_downlink_entry *downlink =
                    mesh_relay_find_downlink(&mesh_runtime,
                                             parsed.packet.dst_id);

                targeted_control_relay =
                    downlink != NULL && downlink->valid &&
                    downlink->route_epoch ==
                        mesh_runtime.upstream.current_epoch &&
                    downlink->gateway_id == GATEWAY_ID &&
                    mesh_id_is_unicast(downlink->next_hop_id) &&
                    downlink->next_hop_id != DEVICE_ID &&
                    downlink->next_hop_id != parsed.previous_hop_id;
            }

            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                (void)mesh_route_capture_find_u32(parsed.payload,
                                                  parsed.payload_len,
                                                  TLV_FLOOD_EPOCH_ID,
                                                  &captured_flood_epoch_id);
                (void)mesh_route_capture_find_u16(parsed.payload,
                                                  parsed.payload_len,
                                                  TLV_REPLY_NONCE,
                                                  &captured_reply_nonce);
            } else if (parsed.packet.msg_type == MSG_ROUTE_REQ) {
                (void)mesh_route_capture_find_u64(parsed.payload,
                                                  parsed.payload_len,
                                                  TLV_RESPONDER_ID,
                                                  &captured_route_request_target_id);
            }

#if DEVICE_ROLE == ROLE_CLICKER
            if (mesh_packet_is_event_control_type(
                    parsed.packet.msg_type)) {
                const struct route_candidate *parent =
                    route_selected(&mesh_runtime.upstream);

                if (parent == NULL ||
                    parsed.previous_hop_id != parent->next_hop_id ||
                    parsed.packet.src_id != parsed.previous_hop_id) {
                    continue;
                }
            }
#endif
            if (!app_mesh_c5_route_capture_relevant(
                    &(const struct app_mesh_c5_route_capture_state) {
                        .msg_type = parsed.packet.msg_type,
                        .session_id = parsed.packet.session_id,
                        .flood_epoch_id = captured_flood_epoch_id,
                        .reply_nonce = captured_reply_nonce,
                        .src_id = parsed.packet.src_id,
                        .dst_id = parsed.packet.dst_id,
                        .previous_hop_id = parsed.previous_hop_id,
                        .target_id = target_id,
                        .route_request_target_id =
                            captured_route_request_target_id,
                        .local_id = DEVICE_ID,
                        .control_origin_id = GATEWAY_ID,
                        .expected_session_id =
                            identity == NULL ? 0u : identity->session_id,
                        .expected_flood_epoch_id =
                            identity == NULL ? 0u : identity->flood_epoch_id,
                        .expected_reply_nonce =
                            identity == NULL ? 0u : identity->reply_nonce,
                        .route_identity_required = identity != NULL,
                        .control_followup =
                            contact_purpose ==
                                C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                        .gateway_control_priority = gateway_priority_control,
                        .require_relayed_gateway_control =
                            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL),
                        .targeted_control_relay = targeted_control_relay,
                    })) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_UNRELATED msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx target=0x%llx session=%u/%u flood=%u/%u nonce=%u/%u\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.packet.src_id,
                                        (unsigned long long)parsed.packet.dst_id,
                                        (unsigned long long)parsed.previous_hop_id,
                                        (unsigned long long)target_id,
                                        parsed.packet.session_id,
                                        identity == NULL ? 0u : identity->session_id,
                                        captured_flood_epoch_id,
                                        identity == NULL ? 0u : identity->flood_epoch_id,
                                        captured_reply_nonce,
                                        identity == NULL ? 0u : identity->reply_nonce);
                }
                LOG_INF("mesh route reply listen ignored unrelated mesh frame: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                        parsed.packet.msg_type,
                        (unsigned long long)parsed.packet.src_id,
                        (unsigned long long)parsed.packet.dst_id,
                        (unsigned long long)parsed.previous_hop_id,
                        parsed.packet.seq,
                        quality);
                continue;
            }

            if (gateway_priority_control) {
                enum command_id control_command_id = CMD_VENDOR_BASE;
                uint8_t control_origin_ttl = 0u;
                bool reverse_route_hint_first = false;
                int reverse_route_ret;

                if (parsed.packet.msg_type == MSG_COMMAND &&
                    gateway_command_extract_id(parsed.payload,
                                               parsed.payload_len,
                                               &control_command_id) != PROTO_OK) {
                    status_debug_note("DBG_C5_CONTROL_COMMAND_ID_FAIL\n");
                    continue;
                }
                if (!app_mesh_c5_gateway_control_origin_ttl(
                        parsed.packet.msg_type,
                        (uint16_t)control_command_id,
                        &control_origin_ttl)) {
                    status_debug_printf("DBG_C5_CONTROL_ROUTE_TTL_FAIL msg=0x%02x\n",
                                        parsed.packet.msg_type);
                    continue;
                }

                if (!targeted_control_relay) {
#if DEVICE_ROLE == ROLE_ANCHOR
                    const struct app_mesh_c5_control_route_identity
                        route_identity = {
                            .route_epoch = mesh_runtime.upstream.current_epoch,
                            .session_id = parsed.packet.session_id,
                            .seq = parsed.packet.seq,
                            .msg_type = parsed.packet.msg_type,
                        };

                    reverse_route_hint_first =
                        app_mesh_c5_control_route_hint_is_first(
                            &mesh_c5_control_route_history,
                            &route_identity);
#else
                    reverse_route_hint_first = true;
#endif
                }
                if (reverse_route_hint_first &&
                    IS_ENABLED(
                        CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL) &&
                    parsed.previous_hop_id != GATEWAY_ID) {
                    mesh_relay_remove_direct_gateway_route(&mesh_runtime);
                }
                reverse_route_ret = reverse_route_hint_first ?
                    mesh_relay_note_gateway_control_reverse_route(
                        &mesh_runtime,
                        &parsed.packet,
                        parsed.previous_hop_id,
                        quality,
                        control_origin_ttl,
                        k_uptime_get_32()) : PROTO_OK;
                if (reverse_route_ret != PROTO_OK &&
                    reverse_route_ret != PROTO_ERR_NO_SPACE) {
                    status_debug_printf("DBG_C5_CONTROL_ROUTE_REJECT msg=0x%02x src=0x%llx prev=0x%llx ttl=%u origin_ttl=%u ret=%d\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.packet.src_id,
                                        (unsigned long long)parsed.previous_hop_id,
                                        parsed.packet.ttl,
                                        control_origin_ttl,
                                        reverse_route_ret);
                    continue;
                }
                if (!targeted_control_relay && !reverse_route_hint_first) {
                    status_debug_printf("DBG_C5_CONTROL_ROUTE_HINT_RETAIN msg=0x%02x session=%u seq=%u prev=0x%llx epoch=%u\n",
                                        parsed.packet.msg_type,
                                        parsed.packet.session_id,
                                        parsed.packet.seq,
                                        (unsigned long long)parsed.previous_hop_id,
                                        mesh_runtime.upstream.current_epoch);
                }
                status_debug_printf("DBG_C5_CONTROL_ROUTE_READY msg=0x%02x prev=0x%llx ttl=%u origin_ttl=%u relay=%u ret=%d\n",
                                    parsed.packet.msg_type,
                                    (unsigned long long)parsed.previous_hop_id,
                                    parsed.packet.ttl,
                                    control_origin_ttl,
                                    targeted_control_relay ? 1u : 0u,
                                    reverse_route_ret);
                if (!targeted_control_relay &&
                    IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    const struct route_candidate *selected =
                        route_selected(&mesh_runtime.upstream);

                    status_debug_printf("DBG_C5_CONTROL_ROUTE_STATE epoch=%u sel=%u next=0x%llx hop=%u timing=%u\n",
                                        mesh_runtime.upstream.current_epoch,
                                        mesh_runtime.upstream.selected_index,
                                        (unsigned long long)(selected == NULL ?
                                            0u : selected->next_hop_id),
                                        selected == NULL ? 0u : selected->hop_count,
                                        selected != NULL &&
                                            selected->channel9_timing_valid ? 1u : 0u);
                }
            }

            if (app_mesh_c5_route_capture_requires_inline_timing_install(
                    parsed.packet.msg_type,
                    contact_purpose ==
                        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION)) {
                uint32_t received_at_ms = k_uptime_get_32();

                if (!mesh_handle_event_control(&parsed.packet,
                                               parsed.payload,
                                               parsed.payload_len,
                                               parsed.previous_hop_id,
                                               received_at_ms)) {
                    status_debug_printf("DBG_EVENT_CTRL_INLINE_FAIL msg=0x%02x\n",
                                        parsed.packet.msg_type);
                    continue;
                }
                capture_count++;
                last_ret = 0;
                status_debug_printf("DBG_EVENT_CTRL_INLINE msg=0x%02x recv=%u next=0x%llx seq=%u\n",
                                    parsed.packet.msg_type,
                                    received_at_ms,
                                    (unsigned long long)parsed.previous_hop_id,
                                    parsed.packet.seq);
                break;
            }

            if (!mesh_queue_from_frame_at_internal(frame,
                                                   frame_len,
                                                   quality,
                                                   UWB_CHANNEL_WAKE_CONTACT,
                                                   k_uptime_get_32(),
                                                   0u,
                                                   NULL,
                                                   0u,
                                                   false,
                                                   NULL,
                                                   NULL)) {
                status_debug_printf("DBG_ROUTE_REPLY_QUEUE_FAIL msg=0x%02x seq=%u\n",
                                    parsed.packet.msg_type,
                                    parsed.packet.seq);
                continue;
            }
            capture_count++;
            if (app_mesh_c5_route_capture_yields_to_competing_request(
                    parsed.packet.msg_type)) {
                yielded_to_competing_route_request = true;
                last_ret = -EAGAIN;
                status_debug_printf("DBG_ROUTE_REPLY_LISTEN_YIELD_REQUEST src=0x%llx prev=0x%llx target=0x%llx\n",
                                    (unsigned long long)parsed.packet.src_id,
                                    (unsigned long long)parsed.previous_hop_id,
                                    (unsigned long long)captured_route_request_target_id);
                break;
            }
            if (app_mesh_c5_route_capture_requires_post_rx_response(
                    parsed.packet.msg_type)) {
                hold_post_rx_response_contact = true;
                status_debug_note("DBG_EVENT_CTRL_POST_RX_QUEUED\n");
            }
            if (app_mesh_c5_route_capture_requires_ack_hold(parsed.packet.msg_type)) {
                captured_route_reply = true;
            }
            high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                 "phase=capture target=0x%016llx msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u captured=%u reason=%s",
                                 (unsigned long long)target_id,
                                 parsed.packet.msg_type,
                                 (unsigned long long)parsed.packet.src_id,
                                 (unsigned long long)parsed.packet.dst_id,
                                 (unsigned long long)parsed.previous_hop_id,
                                 parsed.packet.seq,
                                 quality,
                                 (unsigned int)capture_count,
                                 reason == NULL ? "route" : reason);
            LOG_INF("mesh route reply listen captured: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u captured=%u",
                    parsed.packet.msg_type,
                    (unsigned long long)parsed.packet.src_id,
                    (unsigned long long)parsed.packet.dst_id,
                    (unsigned long long)parsed.previous_hop_id,
                    parsed.packet.seq,
                    quality,
                    (unsigned int)capture_count);
            if (app_mesh_c5_route_capture_completes_discovery(parsed.packet.msg_type)) {
                if (route_reply_captured != NULL) {
                    *route_reply_captured = true;
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=route-captured target=0x%016llx msg=0x%02x seq=%u captured=%u reason=%s",
                                     (unsigned long long)target_id,
                                     parsed.packet.msg_type,
                                     parsed.packet.seq,
                                     (unsigned int)capture_count,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen exiting after route capture: target=0x%016llx msg=0x%02x seq=%u captured=%u",
                        (unsigned long long)target_id,
                        parsed.packet.msg_type,
                        parsed.packet.seq,
                        (unsigned int)capture_count);
                break;
            }
            if (mesh_packet_is_event_control_type(parsed.packet.msg_type)) {
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=event-control-captured target=0x%016llx msg=0x%02x seq=%u captured=%u reason=%s",
                                     (unsigned long long)target_id,
                                     parsed.packet.msg_type,
                                     parsed.packet.seq,
                                     (unsigned int)capture_count,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen exiting after event control: target=0x%016llx msg=0x%02x seq=%u captured=%u",
                        (unsigned long long)target_id,
                        parsed.packet.msg_type,
                        parsed.packet.seq,
                        (unsigned int)capture_count);
                break;
            }
            if (gateway_priority_control) {
                break;
            }
        }
    } else {
        last_ret = ret;
        high_debug_log_event("MESH_ROUTE_REPLY_RX",
                             "phase=config-fail target=0x%016llx ret=%d reason=%s",
                             (unsigned long long)target_id,
                             ret,
                             reason == NULL ? "route" : reason);
        LOG_WRN("mesh route reply wake-mode config failed: target=0x%016llx ret=%d reason=%s",
                (unsigned long long)target_id,
                ret,
                reason == NULL ? "route" : reason);
    }

    release_ret = mesh_radio_standby_with_bounded_recovery(
        "route-reply-listen");
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    if (release_ret < 0) {
        LOG_ERR("route-reply listen radio release failed: %d",
                release_ret);
        mesh_c5_contact_clear(click_captured ?
                              "click-preempt-release-fail" :
                              "route-reply-release-fail");
        if (route_reply_captured != NULL) {
            *route_reply_captured = false;
        }
        return release_ret;
    }
    radio_guard_uwb_stop();

    if (click_captured) {
        bool handled;

        mesh_c5_contact_clear("click-preempt");
        handled = mesh_handoff_anchor_click_claim(&click_claim,
                                                  click_quality,
                                                  click_observed_ms);
        if (!handled) {
            mesh_restart_role_scan();
            if (mesh_rx_pending_count() > 0u) {
                (void)mesh_submit_owned_work(&mesh_rx_work,
                                             "route-listener-rx");
            }
            return -EIO;
        }
        return -EAGAIN;
    }

    mesh_restart_role_scan();

    if (mesh_rx_pending_count() > 0u) {
        (void)mesh_submit_owned_work(&mesh_rx_work,
                                     "route-listener-click-rx");
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=done target=0x%016llx captured=%u last_ret=%d reason=%s",
                         (unsigned long long)target_id,
                         (unsigned int)capture_count,
                         last_ret,
                         reason == NULL ? "route" : reason);
    status_debug_printf("DBG_ROUTE_REPLY_LISTEN_DONE captured=%u ret=%d\n",
                        (unsigned int)capture_count,
                        last_ret);
    LOG_INF("mesh route reply listen done: target=0x%016llx captured=%u last_ret=%d reason=%s",
            (unsigned long long)target_id,
            (unsigned int)capture_count,
            last_ret,
            reason == NULL ? "route" : reason);
    if (captured_route_reply || hold_post_rx_response_contact) {
        status_debug_note(captured_route_reply ?
                          "DBG_ROUTE_REPLY_CONTACT_KEEP_ACK\n" :
                          "DBG_EVENT_CTRL_CONTACT_KEEP_RESPONSE\n");
    } else {
        mesh_c5_contact_clear(capture_count > 0u ? "reply-listen-done" :
                              "reply-listen-timeout");
    }
    if (yielded_to_competing_route_request) {
        return -EAGAIN;
    }
    return capture_count > 0u ? 0 : last_ret;
}
