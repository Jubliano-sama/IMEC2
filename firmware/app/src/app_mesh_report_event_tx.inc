#line 9366 __BASE_FILE__
int mesh_request_route(uint64_t target_id, const char *reason)
{
    return mesh_request_route_owned(
        target_id, reason,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        NULL, 0u);
}

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet)
{
    return mesh_relay_packet_requires_channel9_payload_event(packet);
}

static bool mesh_outbound_needs_result_offer(const struct mesh_outbound *out)
{
    struct command_result_id result_id;

    return out != NULL &&
           out->packet.msg_type == MSG_COMMAND_RESULT &&
           out->payload_len > COLLECTION_RESULT_INLINE_C5_MAX_BYTES &&
           command_result_id_from_tlvs(out->payload, out->payload_len, &result_id) == PROTO_OK;
}

void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements)
{
    uint32_t now_ms;

    if (requirements == NULL) {
        return;
    }

    memset(requirements, 0, sizeof(*requirements));
    requirements->retune_guard_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
                                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS :
                                    MESH_EVENT_DEFAULT_GUARD_MS;
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        now_ms = k_uptime_get_32();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            mesh_channel9_connection_count() > 0u) {
            requirements->next_required_scan_start_ms = 0u;
        } else {
            requirements->next_required_scan_start_ms = now_ms + anchor_uwb_scan_interval_ms;
        }
        requirements->click_epoch_active = anchor_uwb_window_active();
        requirements->discovery_active = requirements->click_epoch_active;
        requirements->ranging_active = requirements->click_epoch_active;
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        now_ms = k_uptime_get_32();
        if (mesh_gateway_route_test_preempt_active(now_ms)) {
            requirements->active_until_ms = mesh_gateway_route_preempt_deadline_ms;
            requirements->discovery_active = true;
        }
    }
}

static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_EVENT_DEFAULT_INTERVAL_MS,
        .event_window_ms = MESH_EVENT_DEFAULT_WINDOW_MS,
        .first_event_time_ms = mesh_route_test_first_event_time_ms(now_ms),
        .guard_ms = MESH_EVENT_DEFAULT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_EVENT_DEFAULT_MAX_MISSED,
        .supervision_timeout_ms = MESH_EVENT_DEFAULT_SUPERVISION_MS,
    };

    return mesh_event_timing_negotiate(timing, &params, true);
}

static uint32_t mesh_event_control_tx_reference_ms(uint8_t msg_type, uint32_t now_ms)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        msg_type == MSG_MESH_EVENT_PROPOSE) {
        return now_ms + MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
    }
    return now_ms;
}

static uint32_t mesh_event_control_rx_reference_ms(uint32_t received_at_ms)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        MESH_EVENT_CONTROL_CH5_AIRTIME_MS == 0u) {
        return received_at_ms;
    }
    if (received_at_ms <= MESH_EVENT_CONTROL_CH5_AIRTIME_MS) {
        return 1u;
    }
    return received_at_ms - MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
}

static uint32_t mesh_route_test_first_event_time_ms(uint32_t now_ms)
{
    uint32_t default_first_ms = now_ms + MESH_EVENT_DEFAULT_FIRST_DELAY_MS;
    uint32_t selected_ms = default_first_ms;
    bool selected = false;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS == 0u) {
        return default_first_ms;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        uint32_t candidate_ms;

        if (!entry->valid ||
            entry->timing.event_interval_ms != MESH_EVENT_DEFAULT_INTERVAL_MS) {
            continue;
        }

        candidate_ms = entry->timing.next_event_time_ms +
                       MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS;
        while (!uptime_deadline_reached(candidate_ms, default_first_ms)) {
            candidate_ms += entry->timing.event_interval_ms;
        }
        if (!selected || uptime_deadline_reached(selected_ms, candidate_ms)) {
            selected_ms = candidate_ms;
            selected = true;
        }
    }

    if (selected) {
        high_debug_log_event("MESH_EVENT_TIMING",
                             "phase=second-slot first_ms=%u default_ms=%u offset_ms=%u",
                             selected_ms,
                             default_first_ms,
                             MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS);
    }
    return selected_ms;
}

static struct app_mesh_event_request_identity mesh_event_request_identity(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return (struct app_mesh_event_request_identity) {
        .source_id = packet == NULL ? 0u : packet->src_id,
        .session_id = packet == NULL ? 0u : packet->session_id,
        .sequence = packet == NULL ? 0u : packet->seq,
        .payload_fingerprint = app_mesh_event_payload_fingerprint(payload,
                                                                  payload_len),
        .payload_len = payload_len > UINT16_MAX ? UINT16_MAX :
                       (uint16_t)payload_len,
        .message_type = packet == NULL ? 0u : packet->msg_type,
    };
}

static bool mesh_event_accept_timing_compatible(
    const struct mesh_event_timing *accepted,
    const struct mesh_event_control_record *proposal)
{
    const struct mesh_event_timing *proposed;

    if (accepted == NULL || proposal == NULL || !proposal->valid) {
        return false;
    }
    proposed = &proposal->timing;
    return accepted->mesh_channel == proposed->mesh_channel &&
           accepted->event_interval_ms == proposed->event_interval_ms &&
           accepted->event_window_ms == proposed->event_window_ms &&
           accepted->event_counter == proposed->event_counter &&
           accepted->guard_ms == proposed->guard_ms &&
           accepted->peer_clock_skew_estimate_ppm ==
               proposed->peer_clock_skew_estimate_ppm &&
           accepted->max_missed_events == proposed->max_missed_events &&
           accepted->supervision_timeout_ms ==
               proposed->supervision_timeout_ms;
}

static struct mesh_event_owner *mesh_event_owner_for_peer(uint64_t peer_id)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_owners); i++) {
        if (mesh_event_owners[i].generation != 0u &&
            mesh_event_owners[i].peer_id == peer_id) {
            return &mesh_event_owners[i];
        }
    }
    return NULL;
}

static int mesh_event_owner_begin_peer(uint64_t peer_id,
                                       uint32_t session_id,
                                       uint16_t proposal_sequence,
                                       bool proposal_from_peer)
{
    struct mesh_event_owner *owner = mesh_event_owner_for_peer(peer_id);

    if (owner == NULL) {
        for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_owners); i++) {
            if (!mesh_event_owners[i].active) {
                owner = &mesh_event_owners[i];
                break;
            }
        }
    }
    if (owner == NULL) {
        return PROTO_ERR_NO_SPACE;
    }
    return mesh_event_owner_begin(owner, peer_id, session_id,
                                  proposal_sequence, proposal_from_peer);
}

static void mesh_event_owner_abandon_peer(uint64_t peer_id)
{
    struct mesh_event_owner *owner = mesh_event_owner_for_peer(peer_id);

    if (owner != NULL) {
        mesh_event_owner_abandon(owner);
    }
}

static bool mesh_event_session_retained(uint32_t session_id)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_owners); i++) {
        if (mesh_event_owner_retains_session(&mesh_event_owners[i],
                                             session_id)) {
            return true;
        }
    }
    return false;
}

static uint32_t mesh_event_new_operation_session(void)
{
    uint32_t seed;

    if (mesh_event_operation_session_next == 0u) {
        seed = sys_rand32_get() ^ k_uptime_get_32() ^
               (uint32_t)(uint64_t)DEVICE_ID ^
               (uint32_t)(((uint64_t)DEVICE_ID) >> 32);
        mesh_event_operation_session_next = seed == 0u ? 1u : seed;
    }

    for (size_t attempt = 0u;
         attempt <= ARRAY_SIZE(mesh_event_owners);
         attempt++) {
        mesh_event_operation_session_next++;
        if (mesh_event_operation_session_next == 0u) {
            mesh_event_operation_session_next = 1u;
        }
        if (!mesh_event_session_retained(mesh_event_operation_session_next)) {
            return mesh_event_operation_session_next;
        }
    }
    return 0u;
}

static int mesh_prepare_event_control_record(
    struct mesh_event_control_record *record,
    uint64_t peer_id,
    uint8_t msg_type,
    const struct mesh_event_timing *accepted_timing,
    uint32_t session_id,
    uint16_t sequence)
{
    struct mesh_event_timing timing = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = mesh_event_control_tx_reference_ms(msg_type,
                                                                      now_ms);
    int ret;

    if (record == NULL || !mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID ||
        (msg_type != MSG_MESH_EVENT_PROPOSE &&
         msg_type != MSG_MESH_EVENT_ACCEPT)) {
        return -EINVAL;
    }

    memset(record, 0, sizeof(*record));
    if (accepted_timing != NULL) {
        timing = *accepted_timing;
    } else {
        ret = mesh_prepare_event_timing(&timing, timing_reference_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    if (msg_type == MSG_MESH_EVENT_PROPOSE) {
        mesh_event_timing_set_local_first_slot_tx(&timing, true);
    }
    ret = mesh_append_event_timing_tlvs_at(
        record->payload,
        sizeof(record->payload),
        &payload_len,
        &timing,
        now_ms);
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return -EMSGSIZE;
    }
    ret = mesh_init_event_control(&record->packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  session_id == 0u ?
                                      mesh_event_new_operation_session() :
                                      session_id,
                                  sequence == 0u ?
                                      mesh_next_event_control_seq() : sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    record->timing = timing;
    record->peer_id = peer_id;
    record->encoded_delay_ms = uptime_ms_until_deadline(
        now_ms, timing.next_event_time_ms);
    record->payload_len = (uint8_t)payload_len;
    record->valid = true;
    return 0;
}

static int mesh_send_event_control_record(
    const struct mesh_event_control_record *record,
    enum mesh_c5_control_send_mode mode,
    const char *reason,
    bool *rf_started,
    struct mesh_event_timing *transmitted_timing)
{
    struct mesh_outbound *outbound = &mesh_event_control_retry_scratch;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (record == NULL || !record->valid) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_event_control_retry_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    memset(outbound, 0, sizeof(*outbound));
    outbound->packet = record->packet;
    memcpy(outbound->payload, record->payload, record->payload_len);
    outbound->payload_len = record->payload_len;
    outbound->next_hop_id = record->peer_id;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = mesh_send_c5_control_attempt(
        outbound,
        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
        mode,
        reason == NULL ? "mesh-event-control" : reason,
        rf_started);
    if (ret == 0 && transmitted_timing != NULL) {
        uint32_t tx_done_ms = k_uptime_get_32();

        *transmitted_timing = record->timing;
        mesh_event_timing_reanchor_after_control_tx(
            transmitted_timing,
            tx_done_ms,
            record->encoded_delay_ms,
            MESH_EVENT_CONTROL_CH5_AIRTIME_MS);
    }
    k_mutex_unlock(&mesh_event_control_retry_scratch_lock);
    return ret;
}

static int mesh_send_event_control(uint64_t peer_id,
                                   uint8_t msg_type,
                                   const struct mesh_event_timing *accepted_timing,
                                   bool install_local,
                                   const char *reason,
                                   struct mesh_event_timing *transmitted_timing)
{
    struct mesh_event_timing timing = {0};
    struct mesh_outbound outbound = {0};
    struct mesh_event_owner *owner = NULL;
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = mesh_event_control_tx_reference_ms(msg_type, now_ms);
    uint32_t encoded_delay_ms = 0u;
    uint32_t operation_session_id;
    bool close_event = msg_type == MSG_MESH_EVENT_END;
    bool owner_scoped = close_event || msg_type == MSG_MESH_EVENT_UPDATE;
    int ret;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return -EINVAL;
    }
    if (owner_scoped) {
        owner = mesh_event_owner_for_peer(peer_id);
        if (owner == NULL || !owner->active) {
            return -ESTALE;
        }
        operation_session_id = owner->session_id;
    } else {
        operation_session_id = mesh_event_new_operation_session();
    }

    if (!close_event) {
        if (accepted_timing != NULL) {
            timing = *accepted_timing;
        } else {
            ret = mesh_prepare_event_timing(&timing, timing_reference_ms);
            if (ret != PROTO_OK) {
                return -EINVAL;
            }
        }
        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            mesh_event_timing_set_local_first_slot_tx(&timing, true);
        }

        ret = mesh_append_event_timing_tlvs_at(outbound.payload,
                                               sizeof(outbound.payload),
                                               &payload_len,
                                               &timing,
                                               now_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        encoded_delay_ms = uptime_ms_until_deadline(now_ms,
                                                    timing.next_event_time_ms);
    }
    ret = mesh_init_event_control(&outbound.packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  operation_session_id,
                                  mesh_next_event_control_seq(),
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = peer_id;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && !close_event) {
        status_debug_printf("DBG_EVENT_TX msg=%02x now=%u ref=%u next=%u cnt=%u txeven=%u\n",
                            msg_type,
                            now_ms,
                            timing_reference_ms,
                            timing.next_event_time_ms,
                            timing.event_counter,
                            timing.local_tx_on_even_events ? 1u : 0u);
    }
    ret = close_event ?
          mesh_send_outbound(&outbound, reason == NULL ? "mesh-event-control" : reason) :
          mesh_send_c5_control(&outbound,
                               C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
                               msg_type == MSG_MESH_EVENT_ACCEPT ?
                                   MESH_C5_CONTROL_ACCEPTED_EXCHANGE :
                                   MESH_C5_CONTROL_WAKE_IF_NEEDED,
                               reason == NULL ? "mesh-event-control" : reason);
    if (ret < 0) {
        return ret;
    }
    if (!close_event) {
        uint32_t tx_done_ms = k_uptime_get_32();

        mesh_event_timing_reanchor_after_control_tx(
            &timing,
            tx_done_ms,
            encoded_delay_ms,
            MESH_EVENT_CONTROL_CH5_AIRTIME_MS);
        if (transmitted_timing != NULL) {
            *transmitted_timing = timing;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_EVENT_TX_ALIGNED msg=%02x done=%u encoded=%u rxoff=%u next=%u\n",
                                msg_type,
                                tx_done_ms,
                                encoded_delay_ms,
                                MESH_EVENT_CONTROL_CH5_AIRTIME_MS,
                                timing.next_event_time_ms);
        }
    }
    if (install_local) {
        struct mesh_event_timing local_timing = timing;

        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            mesh_event_timing_set_local_first_slot_tx(&local_timing, true);
        }
        ret = mesh_install_channel9_timing(peer_id,
                                           &local_timing,
                                           reason == NULL ? "mesh-event-control" : reason);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_LOCAL_TIMING_INSTALLED\n");
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                      mesh_channel9_prepare_start_ms(&local_timing)));
    }
    if (owner_scoped &&
        mesh_event_owner_commit_local(owner, DEVICE_ID, &outbound.packet,
                                      outbound.payload,
                                      outbound.payload_len) !=
            PROTO_OK) {
        return -ESTALE;
    }
    return 0;
}

static void mesh_event_propose_clear(void)
{
    uint64_t peer_id = mesh_event_propose_retry.peer_id;

    app_mesh_event_retry_clear(&mesh_event_propose_retry);
    memset(&mesh_event_propose_record, 0, sizeof(mesh_event_propose_record));
    if (mesh_route_ready_event_peer_id == peer_id) {
        mesh_route_ready_event_peer_id = 0u;
    }
}

static void mesh_event_accept_rx_clear_peer(uint64_t peer_id)
{
    if (mesh_event_accept_rx_cache.active &&
        mesh_event_accept_rx_cache.peer_id == peer_id) {
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
    }
}

static int mesh_event_negotiation_schedule_next(void)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t delay_ms = UINT32_MAX;
    const struct app_mesh_event_retry_state *states[] = {
        &mesh_event_accept_retry.retry,
        &mesh_event_propose_retry,
    };

    for (size_t i = 0u; i < ARRAY_SIZE(states); i++) {
        const struct app_mesh_event_retry_state *state = states[i];
        uint32_t candidate_ms;

        if (!state->active) {
            continue;
        }
        if (app_mesh_event_retry_expired(state, now_ms)) {
            delay_ms = 0u;
            break;
        }
        candidate_ms = state->retry_due_ms != 0u ?
                       state->retry_due_ms : state->deadline_ms;
        candidate_ms = uptime_ms_until_deadline(now_ms, candidate_ms);
        if (candidate_ms < delay_ms) {
            delay_ms = candidate_ms;
        }
    }
    if (delay_ms == UINT32_MAX) {
        return mesh_cancel_delayable(&mesh_event_negotiation_retry_work);
    }
    return mesh_reschedule_delayable(&mesh_event_negotiation_retry_work,
                                     delay_ms);
}

static bool mesh_event_retry_after_failure(
    struct app_mesh_event_retry_state *state,
    enum app_mesh_rf_retry_policy policy,
    bool rf_started,
    const char *reason)
{
    uint32_t delay_ms = 0u;
    bool scheduled = app_mesh_event_retry_note_failure(
        state,
        policy,
        k_uptime_get_32(),
        sys_rand32_get(),
        rf_started,
        &delay_ms);

    if (scheduled) {
        int schedule_ret = mesh_event_negotiation_schedule_next();

        status_debug_printf("DBG_EVENT_RETRY_SCHED msg=%02x peer=0x%llx delay=%u rf=%u round=%u ret=%d reason=%s\n",
                            state->retry_key.message_type,
                            (unsigned long long)state->peer_id,
                            delay_ms,
                            rf_started ? 1u : 0u,
                            state->retry.retry_round,
                            schedule_ret,
                            reason == NULL ? "event-retry" : reason);
    }
    return scheduled;
}

static int mesh_propose_event_after_channel5_contact(uint64_t peer_id, const char *reason)
{
    bool require_accept = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                          DEVICE_ROLE != ROLE_GATEWAY;
    struct mesh_event_timing transmitted_timing = {0};
    bool rf_started = false;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (app_mesh_event_retry_expired(&mesh_event_propose_retry, now_ms)) {
        mesh_event_propose_clear();
    }
    if (!mesh_event_propose_retry.active) {
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;

        /* A completed ACCEPT belongs only to the proposal that created it. */
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
        ret = mesh_prepare_event_control_record(&mesh_event_propose_record,
                                                peer_id,
                                                MSG_MESH_EVENT_PROPOSE,
                                                NULL,
                                                0u,
                                                0u);
        if (ret < 0) {
            return ret;
        }
        request = mesh_event_request_identity(
            &mesh_event_propose_record.packet,
            mesh_event_propose_record.payload,
            mesh_event_propose_record.payload_len);
        retry_key = mesh_rf_retry_packet_key(
            &mesh_event_propose_record.packet,
            APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
        ret = app_mesh_event_retry_begin(
            &mesh_event_propose_retry,
            peer_id,
            &request,
            &retry_key,
            now_ms,
            now_ms + MESH_EVENT_PROPOSE_RETRY_DEADLINE_MS,
            mesh_event_propose_record.timing.event_interval_ms,
            0u);
        if (ret < 0) {
            memset(&mesh_event_propose_record, 0,
                   sizeof(mesh_event_propose_record));
            return ret;
        }
    } else if (mesh_event_propose_retry.peer_id != peer_id ||
               !mesh_event_propose_record.valid) {
        return -EBUSY;
    } else if (mesh_event_propose_retry.retry_due_ms != 0u &&
               !app_mesh_event_retry_due(&mesh_event_propose_retry, now_ms)) {
        return -EAGAIN;
    }

    ret = mesh_send_event_control_record(
        &mesh_event_propose_record,
        MESH_C5_CONTROL_WAKE_IF_NEEDED,
        reason,
        &rf_started,
        &transmitted_timing);
    if (ret < 0) {
        LOG_WRN("mesh channel-9 event proposal failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-propose" : reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_PROPOSE_FAIL\n");
        }
        if (!mesh_event_retry_after_failure(
                &mesh_event_propose_retry,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                rf_started,
                "event-propose-send")) {
            mesh_event_propose_clear();
            return -ETIMEDOUT;
        }
        return ret;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_EVENT_PROPOSE_SENT\n");
    }
    /*
     * The immutable wire record carries a relative delay.  Retain the phase
     * that this successful RF transmission actually proposed so ACCEPT can
     * confirm it without moving the connection by the response-queue latency.
     */
    mesh_event_propose_record.timing = transmitted_timing;

    if (require_accept) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_ACCEPT_LISTEN\n");
        }
        ret = mesh_listen_for_route_reply(peer_id,
                                          "event-accept",
                                          MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
                                          NULL,
                                          NULL);
        if (ret == 0 && mesh_event_propose_retry.active) {
            ret = -EPROTO;
        }
        if (ret < 0) {
            (void)mesh_ch9_ack_batch_clear_for_peer(peer_id, "event-accept-timeout");
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
            mesh_event_owner_abandon_peer(peer_id);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_EVENT_ACCEPT_TIMEOUT\n");
            }
            LOG_WRN("mesh channel-9 event ACCEPT not received: peer=0x%016llx ret=%d",
                    (unsigned long long)peer_id,
                    ret);
            if (!mesh_event_retry_after_failure(
                    &mesh_event_propose_retry,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    true,
                    "event-accept-missed")) {
                mesh_event_propose_clear();
                return -ETIMEDOUT;
            }
            return ret;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_ACCEPT_RX\n");
        }
    } else if (app_mesh_event_retry_claim_timing_install(
                   &mesh_event_propose_retry)) {
        ret = mesh_install_channel9_timing(peer_id,
                                           &transmitted_timing,
                                           reason == NULL ?
                                               "event-propose" : reason);
        if (ret != PROTO_OK) {
            mesh_event_propose_clear();
            return mesh_errno_from_proto(ret);
        }
        ret = mesh_event_owner_begin_peer(
            peer_id,
            mesh_event_propose_record.packet.session_id,
            mesh_event_propose_record.packet.seq,
            false);
        if (ret != PROTO_OK) {
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
            mesh_event_propose_clear();
            return mesh_errno_from_proto(ret);
        }
    }
    mesh_event_propose_clear();
    return 0;
}

static bool mesh_packet_is_event_control_type(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static void mesh_event_accept_clear(void)
{
    memset(&mesh_event_accept_retry, 0, sizeof(mesh_event_accept_retry));
}

static void mesh_event_accept_completed_prune(uint32_t now_ms)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (entry->completion.valid &&
            uptime_deadline_reached(now_ms,
                                    entry->completion.expires_at_ms)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void mesh_event_accept_completed_clear_peer(uint64_t peer_id)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (entry->completion.valid &&
            entry->completion.peer_id == peer_id) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void mesh_event_accept_completed_store(
    const struct mesh_event_control_record *response,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint32_t expires_at_ms,
    uint16_t retry_round)
{
    struct mesh_event_accept_completed *selected = NULL;

    if (response == NULL || !response->valid || request == NULL) {
        return;
    }
    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];
        enum app_mesh_event_request_match match =
            app_mesh_event_completion_match(&entry->completion,
                                            peer_id,
                                            request,
                                            now_ms);

        if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
            selected = entry;
            break;
        }
        if (selected == NULL && !entry->completion.valid) {
            selected = entry;
        }
    }
    if (selected == NULL) {
        selected = &mesh_event_accept_completed[
            mesh_event_accept_completed_cursor %
            ARRAY_SIZE(mesh_event_accept_completed)];
        mesh_event_accept_completed_cursor++;
    }

    memset(selected, 0, sizeof(*selected));
    selected->response = *response;
    if (app_mesh_event_completion_store(&selected->completion,
                                        peer_id,
                                        request,
                                        now_ms,
                                        expires_at_ms) < 0) {
        memset(selected, 0, sizeof(*selected));
    } else {
        selected->retry_round = retry_round;
    }
}

static void mesh_event_accept_completed_note_retry_round(
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint16_t retry_round)
{
    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (app_mesh_event_completion_match(&entry->completion,
                                            peer_id,
                                            request,
                                            now_ms) ==
            APP_MESH_EVENT_REQUEST_DUPLICATE) {
            entry->retry_round = retry_round;
            return;
        }
    }
}

static int mesh_event_accept_finish_send(
    const struct mesh_event_timing *transmitted_timing,
    const char *reason)
{
    struct app_mesh_event_retry_state *retry =
        &mesh_event_accept_retry.retry;
    const struct mesh_event_timing *negotiated_timing =
        &mesh_event_accept_retry.response.timing;
    uint64_t peer_id = retry->peer_id;
    uint16_t retry_round;
    int ret;

    if (transmitted_timing == NULL || !retry->active) {
        return -EINVAL;
    }
    /*
     * RF has completed, so the bounded accepted exchange has served its only
     * purpose.  Release only this peer and control class; a newer or unrelated
     * contact must survive even if timing validation below fails.
     */
    mesh_c5_contact_clear_matching(
        peer_id,
        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
        "event-accept-sent");
    if (!mesh_event_accept_retry.replay_existing_response &&
        app_mesh_event_retry_claim_timing_install(retry)) {
        if (!app_mesh_c5_event_accept_realign_is_reserved(
                &mesh_event_accept_retry.reservation_timing,
                negotiated_timing,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS)) {
            status_debug_printf("DBG_EVENT_ACCEPT_REALIGN_OUT_OF_BOUNDS prev=0x%llx reserved=%u actual=%u slop=%u\n",
                                (unsigned long long)peer_id,
                                mesh_event_accept_retry.reservation_timing.next_event_time_ms,
                                negotiated_timing->next_event_time_ms,
                                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS);
            retry->timing_installed = false;
            mesh_event_accept_clear();
            return -ERANGE;
        }
        ret = mesh_install_channel9_timing_direction(
            peer_id,
            negotiated_timing,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
            reason == NULL ? "event-accept-tx-aligned" : reason);
        if (ret != PROTO_OK) {
            retry->timing_installed = false;
            mesh_event_accept_clear();
            return mesh_errno_from_proto(ret);
        }
        ret = mesh_event_owner_begin_peer(peer_id,
                                          retry->request.session_id,
                                          retry->request.sequence,
                                          true);
        if (ret != PROTO_OK) {
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
            retry->timing_installed = false;
            mesh_event_accept_clear();
            return mesh_errno_from_proto(ret);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_EVENT_ACCEPT_REALIGNED prev=0x%llx next=%u\n",
                                (unsigned long long)peer_id,
                                negotiated_timing->next_event_time_ms);
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(
            k_uptime_get_32(),
            mesh_channel9_prepare_start_ms(negotiated_timing)));
        if (mesh_gateway_route_test_role()) {
            mesh_gateway_route_test_clear_preempt(peer_id,
                                                  "event-accept-installed");
        }
    }
    retry_round = retry->retry.retry_round;
    app_mesh_event_retry_note_send_success(retry);
    mesh_event_accept_completed_store(
        &mesh_event_accept_retry.response,
        peer_id,
        &retry->request,
        k_uptime_get_32(),
        retry->deadline_ms,
        retry_round);
    mesh_event_accept_clear();
    (void)mesh_event_negotiation_schedule_next();
    return 0;
}

static int mesh_event_accept_attempt(const char *reason)
{
    struct mesh_event_timing transmitted_timing = {0};
    struct app_mesh_event_retry_state *retry =
        &mesh_event_accept_retry.retry;
    bool rf_started = false;
    int ret;

    if (!retry->active || !mesh_event_accept_retry.response.valid) {
        return -ENOENT;
    }
    atomic_set(&mesh_rx_response_active_state, 1);
    ret = mesh_send_event_control_record(
        &mesh_event_accept_retry.response,
        MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
        reason == NULL ? "mesh-event-accept" : reason,
        &rf_started,
        &transmitted_timing);
    atomic_set(&mesh_rx_response_active_state, 0);
    if (ret == 0) {
        return mesh_event_accept_finish_send(&transmitted_timing, reason);
    }

    LOG_WRN("mesh channel-9 event ACCEPT failed: next=0x%016llx ret=%d",
            (unsigned long long)retry->peer_id,
            ret);
    if (!mesh_event_retry_after_failure(
            retry,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            rf_started,
            "event-accept-send")) {
        mesh_c5_contact_clear_matching(
            retry->peer_id,
            C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
            "event-accept-failed");
        mesh_event_accept_clear();
        return -ETIMEDOUT;
    }
    return ret;
}

static bool mesh_event_accept_duplicate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct app_mesh_event_request_identity request =
        mesh_event_request_identity(packet, payload, payload_len);
    enum app_mesh_event_request_match match;
    uint32_t now_ms = k_uptime_get_32();

    if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry, now_ms)) {
        mesh_c5_contact_clear_matching(
            mesh_event_accept_retry.retry.peer_id,
            C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
            "event-accept-expired");
        mesh_event_accept_clear();
    }
    match = app_mesh_event_retry_match(&mesh_event_accept_retry.retry,
                                       previous_hop_id,
                                       &request);
    if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
        if (mesh_event_accept_retry.retry.response_sent &&
            mesh_event_accept_retry.retry.retry_due_ms == 0u) {
            (void)mesh_event_retry_after_failure(
                &mesh_event_accept_retry.retry,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                true,
                "event-propose-duplicate");
        }
        status_debug_note("DBG_EVENT_PROPOSE_DUPLICATE\n");
        return true;
    }
    if (match == APP_MESH_EVENT_REQUEST_BUSY &&
        mesh_event_accept_retry.replay_existing_response) {
        /*
         * A completed response remains in the completion cache, so its
         * best-effort replay must never reserve the singleton ahead of new
         * work from another peer. The duplicate can recreate this replay on
         * its next proposal without losing response identity or retry round.
         */
        mesh_event_accept_completed_note_retry_round(
            mesh_event_accept_retry.retry.peer_id,
            &mesh_event_accept_retry.retry.request,
            now_ms,
            mesh_event_accept_retry.retry.retry.retry_round);
        mesh_event_accept_clear();
        match = APP_MESH_EVENT_REQUEST_NEW;
    }
    if (match == APP_MESH_EVENT_REQUEST_CONFLICT ||
        match == APP_MESH_EVENT_REQUEST_BUSY) {
        status_debug_note(match == APP_MESH_EVENT_REQUEST_CONFLICT ?
                          "DBG_EVENT_PROPOSE_CONFLICT\n" :
                          "DBG_EVENT_PROPOSE_BUSY\n");
        return true;
    }

    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        match = app_mesh_event_completion_match(&entry->completion,
                                                previous_hop_id,
                                                &request,
                                                now_ms);
        if (match == APP_MESH_EVENT_REQUEST_CONFLICT) {
            status_debug_note("DBG_EVENT_PROPOSE_CONFLICT\n");
            return true;
        }
        if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
            struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &entry->response.packet,
                    APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
            int ret;

            memset(&mesh_event_accept_retry, 0,
                   sizeof(mesh_event_accept_retry));
            mesh_event_accept_retry.response = entry->response;
            ret = app_mesh_event_retry_begin(
                &mesh_event_accept_retry.retry,
                previous_hop_id,
                &entry->completion.request,
                &retry_key,
                now_ms,
                entry->completion.expires_at_ms,
                entry->response.timing.event_interval_ms,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS / 2u);
            if (ret < 0) {
                mesh_event_accept_clear();
                return true;
            }
            mesh_event_accept_retry.replay_existing_response = true;
            if (app_mesh_event_retry_resume_backoff(
                    &mesh_event_accept_retry.retry,
                    entry->retry_round) < 0) {
                mesh_event_accept_clear();
                return true;
            }
            if (!mesh_event_retry_after_failure(
                    &mesh_event_accept_retry.retry,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    false,
                    "event-propose-duplicate")) {
                mesh_event_accept_clear();
            }
            status_debug_note("DBG_EVENT_PROPOSE_DUPLICATE\n");
            return true;
        }
    }
    return false;
}

static enum app_mesh_event_request_match mesh_event_accept_rx_match(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct app_mesh_event_request_identity request =
        mesh_event_request_identity(packet, payload, payload_len);
    enum app_mesh_event_request_match match;

    if (app_mesh_event_retry_expired(&mesh_event_accept_rx_cache,
                                     k_uptime_get_32())) {
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
    }
    match = app_mesh_event_retry_match(&mesh_event_accept_rx_cache,
                                       previous_hop_id,
                                       &request);
    if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
        status_debug_note("DBG_EVENT_ACCEPT_DUPLICATE\n");
        return match;
    }
    if (match == APP_MESH_EVENT_REQUEST_CONFLICT ||
        match == APP_MESH_EVENT_REQUEST_BUSY) {
        status_debug_note("DBG_EVENT_ACCEPT_UNEXPECTED\n");
        return match;
    }
    return APP_MESH_EVENT_REQUEST_NEW;
}

static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t received_at_ms)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_owner *owner = NULL;
    enum mesh_event_owner_decision owner_decision = MESH_EVENT_OWNER_INVALID;
    bool replayed_event_accept = false;
    uint32_t timing_reference_ms;
    int ret;

    if (packet == NULL) {
        return false;
    }

    if (!mesh_packet_is_event_control_type(packet->msg_type)) {
        return false;
    }
    if (!mesh_id_is_unicast(previous_hop_id)) {
        LOG_WRN("mesh event control ignored without unicast previous hop");
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_UPDATE ||
        packet->msg_type == MSG_MESH_EVENT_END) {
        owner = mesh_event_owner_for_peer(previous_hop_id);
        owner_decision = mesh_event_owner_classify(
            owner, DEVICE_ID, previous_hop_id, packet, payload, payload_len);
        if (owner_decision != MESH_EVENT_OWNER_APPLY) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_EVENT_OWNER_REJECT msg=%02x prev=0x%llx session=%u seq=%u decision=%u\n",
                                    packet->msg_type,
                                    (unsigned long long)previous_hop_id,
                                    packet->session_id,
                                    packet->seq,
                                    (unsigned int)owner_decision);
            }
            return true;
        }
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        if (mesh_event_owner_commit(owner, DEVICE_ID, previous_hop_id,
                                    packet, payload, payload_len) != PROTO_OK) {
            return true;
        }
        (void)mesh_ch9_ack_batch_clear_for_peer(previous_hop_id, "event-end-rx");
        mesh_event_accept_rx_clear_peer(previous_hop_id);
        mesh_event_accept_completed_clear_peer(previous_hop_id);
        mesh_relay_clear_channel9_timing(&mesh_runtime, previous_hop_id);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_EVENT_END_RX\n");
        }
        mesh_restore_anchor_low_duty_if_no_ch9("event-end-rx");
        return true;
    }

    timing_reference_ms = mesh_event_control_rx_reference_ms(received_at_ms);
    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         timing_reference_ms,
                                         true);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing parse failed: msg=0x%02x ret=%d",
                packet->msg_type,
                ret);
        return true;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_EVENT_RX msg=%02x recv=%u ref=%u next=%u cnt=%u txeven=%u\n",
                            packet->msg_type,
                            received_at_ms,
                            timing_reference_ms,
                            timing.next_event_time_ms,
                            timing.event_counter,
                            timing.local_tx_on_even_events ? 1u : 0u);
    }
    if (packet->msg_type == MSG_MESH_EVENT_UPDATE) {
        /* UPDATE carries the sender's phase; the immediate peer owns the
         * complementary TX/RX direction for that same event counter. */
        mesh_event_timing_set_local_first_slot_tx(
            &timing, !timing.local_tx_on_even_events);
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        struct mesh_event_timing active_timing = {0};
        struct mesh_event_timing reservation_timing = {0};
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;
        uint32_t now_ms = k_uptime_get_32();
        bool had_active_timing;

        if (mesh_event_accept_duplicate(packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id)) {
            return true;
        }
        owner = mesh_event_owner_for_peer(previous_hop_id);
        owner_decision = mesh_event_owner_classify_proposal(
            owner, DEVICE_ID, previous_hop_id, packet, payload, payload_len);
        if (owner_decision != MESH_EVENT_OWNER_APPLY) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_EVENT_PROPOSE_OWNER_REJECT prev=0x%llx session=%u seq=%u decision=%u\n",
                                    (unsigned long long)previous_hop_id,
                                    packet->session_id,
                                    packet->seq,
                                    (unsigned int)owner_decision);
            }
            return true;
        }
        mesh_event_timing_set_local_first_slot_tx(&timing, false);
        had_active_timing = mesh_find_active_channel9_timing(previous_hop_id,
                                                             now_ms,
                                                             &active_timing);
        if (had_active_timing) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_EVENT_PROPOSE_REPLACE_ACTIVE now=%u old=%u new=%u old_cnt=%u new_cnt=%u\n",
                                    now_ms,
                                    active_timing.next_event_time_ms,
                                    timing.next_event_time_ms,
                                    active_timing.event_counter,
                                    timing.event_counter);
            }
        }
        if (mesh_gateway_route_test_slots_full_for(previous_hop_id)) {
            status_debug_note("DBG_GATEWAY_EVENT_REJECT_FULL\n");
            status_debug_printf("DBG_GATEWAY_EVENT_REJECT prev=0x%llx active=%u\n",
                                (unsigned long long)previous_hop_id,
                                mesh_channel9_connection_count());
            mesh_gateway_route_test_clear_preempt(previous_hop_id,
                                                  "event-propose-full");
            return true;
        }
        if (!app_mesh_c5_event_accept_reservation(
                &timing,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS,
                &reservation_timing)) {
            status_debug_note("DBG_EVENT_PROPOSE_REJECT_RESERVATION\n");
            return true;
        }
        ret = mesh_prepare_event_control_record(
            &mesh_event_accept_retry.response,
            previous_hop_id,
            MSG_MESH_EVENT_ACCEPT,
            &timing,
            packet->session_id,
            packet->seq);
        if (ret < 0) {
            return true;
        }
        request = mesh_event_request_identity(packet, payload, payload_len);
        retry_key = mesh_rf_retry_packet_key(
            &mesh_event_accept_retry.response.packet,
            APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
        ret = app_mesh_event_retry_begin(
            &mesh_event_accept_retry.retry,
            previous_hop_id,
            &request,
            &retry_key,
            now_ms,
            now_ms + MESH_EVENT_ACCEPT_RETRY_DEADLINE_MS,
            timing.event_interval_ms,
            MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS / 2u);
        if (ret < 0) {
            memset(&mesh_event_accept_retry, 0,
                   sizeof(mesh_event_accept_retry));
            return true;
        }
        mesh_event_accept_retry.reservation_timing = reservation_timing;
        atomic_set(&mesh_rx_response_active_state, 1);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
        }
        atomic_set(&mesh_rx_response_active_state, 0);
        (void)mesh_event_accept_attempt("mesh-event-accept");
        return true;
    } else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;
        enum app_mesh_event_accept_correlation correlation;
        enum app_mesh_event_request_match accept_match;
        uint32_t now_ms = k_uptime_get_32();

        accept_match = mesh_event_accept_rx_match(packet,
                                                  payload,
                                                  payload_len,
                                                  previous_hop_id);
        if (accept_match == APP_MESH_EVENT_REQUEST_CONFLICT ||
            accept_match == APP_MESH_EVENT_REQUEST_BUSY) {
            return true;
        }
        replayed_event_accept =
            accept_match == APP_MESH_EVENT_REQUEST_DUPLICATE;
        if (!replayed_event_accept) {
            correlation = app_mesh_event_accept_classify(
                &mesh_event_propose_retry,
                packet->src_id,
                packet->dst_id,
                previous_hop_id,
                packet->session_id,
                packet->seq,
                mesh_event_accept_timing_compatible(
                    &timing, &mesh_event_propose_record));
            if (correlation == APP_MESH_EVENT_ACCEPT_REJECT) {
                status_debug_note("DBG_EVENT_ACCEPT_UNCORRELATED\n");
                return true;
            }
            if (correlation == APP_MESH_EVENT_ACCEPT_LEGACY) {
                status_debug_note("DBG_EVENT_ACCEPT_LEGACY_IDENTITY\n");
            }
            request = mesh_event_request_identity(packet, payload, payload_len);
            retry_key = mesh_rf_retry_packet_key(
                packet, APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
            ret = app_mesh_event_retry_begin(
                &mesh_event_accept_rx_cache,
                previous_hop_id,
                &request,
                &retry_key,
                now_ms,
                now_ms + MESH_EVENT_DEFAULT_SUPERVISION_MS,
                timing.event_interval_ms,
                0u);
            if (ret < 0 ||
                !app_mesh_event_retry_claim_timing_install(
                    &mesh_event_accept_rx_cache)) {
                app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
                return true;
            }
        }
        if (replayed_event_accept) {
            /* A duplicate confirmation cannot move an established schedule. */
            status_debug_note("DBG_EVENT_ACCEPT_REPLAY_NO_TIMING_CHANGE\n");
            return true;
        }
        /* ACCEPT confirms the phase from our successful PROPOSE RF send. */
        timing = mesh_event_propose_record.timing;
        mesh_event_timing_set_local_first_slot_tx(&timing, true);
    }

    if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        ret = mesh_install_channel9_timing_direction(
            previous_hop_id,
            &timing,
            MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
            "event-control-rx");
    } else {
        ret = mesh_install_channel9_timing(previous_hop_id,
                                           &timing,
                                           "event-control-rx");
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing install failed: next=0x%016llx ret=%d",
                (unsigned long long)previous_hop_id,
                ret);
        if (packet->msg_type == MSG_MESH_EVENT_ACCEPT &&
            !replayed_event_accept) {
            app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
        }
        return true;
    }

    if (packet->msg_type == MSG_MESH_EVENT_UPDATE &&
        mesh_event_owner_commit(owner, DEVICE_ID, previous_hop_id, packet,
                                payload, payload_len) != PROTO_OK) {
        mesh_relay_clear_channel9_timing(&mesh_runtime, previous_hop_id);
        mesh_event_owner_abandon_peer(previous_hop_id);
        return true;
    }

    if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        if (!replayed_event_accept) {
            ret = mesh_event_owner_begin_peer(
                previous_hop_id,
                mesh_event_propose_record.packet.session_id,
                mesh_event_propose_record.packet.seq,
                false);
            if (ret != PROTO_OK) {
                mesh_relay_clear_channel9_timing(&mesh_runtime,
                                                 previous_hop_id);
                app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
                mesh_event_propose_clear();
                return true;
            }
            app_mesh_event_retry_note_send_success(&mesh_event_accept_rx_cache);
            mesh_event_propose_clear();
        }
    }

    mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                  mesh_channel9_prepare_start_ms(&timing)));
    if (mesh_gateway_route_test_role() &&
        packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        mesh_gateway_route_test_clear_preempt(previous_hop_id,
                                              "event-accept-installed");
    }
    return true;
}

static void mesh_event_negotiation_retry_work_handler(struct k_work *work)
{
    uint32_t now_ms = k_uptime_get_32();

    ARG_UNUSED(work);

    if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry, now_ms)) {
        mesh_event_accept_clear();
    } else if (app_mesh_event_retry_due(&mesh_event_accept_retry.retry,
                                        now_ms)) {
        (void)mesh_event_accept_attempt("mesh-event-accept-retry");
    }

    now_ms = k_uptime_get_32();
    if (app_mesh_event_retry_expired(&mesh_event_propose_retry, now_ms)) {
        mesh_event_propose_clear();
        if (mesh_route_waiting_tx_valid) {
            mesh_schedule_route_waiting_retry_after(
                "event-propose-expired", 1u);
        }
    } else if (app_mesh_event_retry_due(&mesh_event_propose_retry, now_ms)) {
        uint64_t peer_id = mesh_event_propose_retry.peer_id;
        int ret = mesh_propose_event_after_channel5_contact(
            peer_id, "mesh-event-propose-retry");

        if (ret == 0) {
            if (mesh_route_waiting_tx_valid) {
                mesh_schedule_route_waiting_retry_after(
                    "event-propose-complete", 1u);
            }
            if (mesh_relay_tx_active(&mesh_runtime)) {
                (void)mesh_relay_defer_pending_retry(
                    &mesh_runtime, k_uptime_get_32() + 1u);
                mesh_schedule_tx_timeout();
            }
        }
    }
    (void)mesh_event_negotiation_schedule_next();
}

static void mesh_close_channel9_connection(uint64_t peer_id, const char *reason)
{
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !mesh_id_is_unicast(peer_id) ||
        peer_id == DEVICE_ID) {
        return;
    }

    ret = mesh_send_event_control(peer_id,
                                  MSG_MESH_EVENT_END,
                                  NULL,
                                  false,
                                  reason == NULL ? "mesh-event-end" : reason,
                                  NULL);
    if (ret == 0) {
        status_debug_note("DBG_CH9_EVENT_END_TX\n");
    } else {
        mesh_event_owner_abandon_peer(peer_id);
        status_debug_note("DBG_CH9_EVENT_END_FAIL\n");
        LOG_WRN("mesh channel-9 event close TX failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-end" : reason);
    }

    (void)mesh_ch9_ack_batch_clear_for_peer(peer_id,
                                            reason == NULL ? "event-end-tx" : reason);
    mesh_event_accept_rx_clear_peer(peer_id);
    mesh_event_accept_completed_clear_peer(peer_id);
    mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
    mesh_restore_anchor_low_duty_if_no_ch9("event-end-tx");
}

static uint8_t mesh_capture_downstream_channel9_peers(
    uint64_t *peer_ids,
    uint8_t capacity)
{
    uint8_t count = 0u;

    if (peer_ids == NULL || capacity == 0u) {
        return 0u;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS && count < capacity; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (entry->valid &&
            entry->direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
            peer_ids[count++] = entry->next_hop_id;
        }
    }
    return count;
}

static bool mesh_channel9_peer_timing_exists(uint64_t peer_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (mesh_runtime.event_timings[i].valid &&
            mesh_runtime.event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static void mesh_notify_removed_downstream_peers(const uint64_t *peer_ids,
                                                 uint8_t count,
                                                 const char *reason)
{
    if (peer_ids == NULL) {
        return;
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (!mesh_channel9_peer_timing_exists(peer_ids[i])) {
            mesh_close_channel9_connection(peer_ids[i], reason);
        }
    }
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_packet_is_local_origin_priority(const struct proto_packet *packet)
{
    return packet != NULL &&
           packet->src_id == DEVICE_ID &&
           (packet->msg_type == MSG_CLICK_REPORT ||
            packet->msg_type == MSG_COMMAND_RESULT);
}

static bool mesh_outbound_is_local_origin_priority(const struct mesh_outbound *out)
{
    return out != NULL && mesh_packet_is_local_origin_priority(&out->packet);
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_runtime_has_transit_reservation(void)
{
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(&mesh_runtime); i++) {
        const struct mesh_downlink_entry *downlink =
            mesh_relay_downlink_at(&mesh_runtime, i);

        if (!downlink->valid) {
            continue;
        }
        for (uint8_t j = 0u; j < MESH_RELAY_EVENT_TIMINGS; j++) {
            if (mesh_runtime.event_timings[j].valid &&
                mesh_runtime.event_timings[j].next_hop_id ==
                    downlink->next_hop_id) {
                return true;
            }
        }
    }
    return false;
}
#endif

static bool mesh_tx_can_wait_for_route(const struct mesh_outbound *out)
{
    if (out == NULL ||
        out->packet.dst_id == MESH_BROADCAST_ID ||
        out->packet.dst_id == DEVICE_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_GATEWAY_ACK:
    case MSG_MESH_DATA:
    case MSG_SELF_TEST_REPORT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out)
{
    if (out == NULL) {
        return 0u;
    }

    return UWB_MESH_FRAME_HEADER_LEN +
           proto_packet_encoded_len(out->packet.payload_len) +
           UWB_FRAME_CRC_LEN;
}

static uint32_t mesh_ch9_estimated_airtime_ms(size_t frame_len)
{
    const uint64_t frame_bits = ((uint64_t)frame_len + UWB_PHY_FCS_LEN) * 8ull;
    const uint64_t payload_us =
        ((frame_bits * 1000000ull) + MESH_CH9_DATA_RATE_BPS - 1ull) /
        MESH_CH9_DATA_RATE_BPS;
    const uint64_t total_us = payload_us + MESH_CH9_PHY_OVERHEAD_US;

    return (uint32_t)((total_us + 999ull) / 1000ull);
}

static uint32_t mesh_ch9_estimated_tx_ms(size_t frame_len)
{
    return mesh_ch9_estimated_airtime_ms(frame_len) + MESH_CH9_TX_FRAME_GAP_MS;
}

static uint32_t mesh_ch9_tx_offset_ms(const struct mesh_outbound *out)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || out == NULL) {
        return 0u;
    }
    if (out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return 0u;
    }

    return MESH_ROUTE_TEST_CH9_TX_OFFSET_MS;
}

static uint32_t mesh_ch9_slot_send_start_ms(const struct mesh_outbound *out,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms)
{
    uint32_t start_ms;

    if (plan == NULL) {
        return now_ms;
    }

    start_ms = plan->start_ms + mesh_ch9_tx_offset_ms(out);
    if (!uptime_deadline_reached(now_ms, start_ms)) {
        return start_ms;
    }
    return now_ms;
}

static uint32_t mesh_ch9_effective_send_start_ms(const struct mesh_outbound *out,
                                                 const struct mesh_event_plan *plan,
                                                 uint32_t now_ms)
{
    uint32_t send_start_ms = mesh_ch9_slot_send_start_ms(out, plan, now_ms);
    uint32_t config_ready_ms = now_ms + MESH_CH9_TX_CONFIG_GUARD_MS;

    return uptime_deadline_reached(send_start_ms, config_ready_ms) ?
           send_start_ms : config_ready_ms;
}

static bool mesh_ch9_tx_fits_plan(const struct mesh_outbound *out,
                                  const struct mesh_event_plan *plan,
                                  uint32_t now_ms,
                                  uint32_t *required_ms)
{
    uint32_t needed_ms;
    uint32_t available_ms;
    uint32_t effective_send_start_ms;
    uint32_t wait_ms;

    if (out == NULL || plan == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return true;
    }

    needed_ms = mesh_ch9_estimated_tx_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    effective_send_start_ms = mesh_ch9_effective_send_start_ms(out, plan, now_ms);
    wait_ms = uptime_deadline_reached(now_ms, effective_send_start_ms) ?
              0u : effective_send_start_ms - now_ms;
    if (uptime_deadline_reached(effective_send_start_ms, plan->end_ms)) {
        if (required_ms != NULL) {
            *required_ms = wait_ms + needed_ms;
        }
        return false;
    }
    available_ms = uptime_ms_until_deadline(effective_send_start_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = wait_ms + needed_ms;
    }
    return available_ms >= needed_ms;
}

int mesh_prepare_channel9_outbound(struct mesh_outbound *out,
                                   const struct mesh_event_plan *plan,
                                   uint32_t now_ms,
                                   uint32_t *required_ms)
{
    if (out == NULL || plan == NULL) {
        return -EINVAL;
    }

    out->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    out->earliest_tx_ms = mesh_ch9_slot_send_start_ms(out, plan, now_ms);
    if (!mesh_ch9_tx_fits_plan(out, plan, now_ms, required_ms)) {
        return -EBUSY;
    }
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_ch9_tx_fits_configured_slot(const struct mesh_outbound *out,
                                             const struct mesh_event_plan *plan,
                                             uint32_t now_ms,
                                             uint32_t send_start_ms,
                                             uint32_t *required_ms)
{
    uint32_t needed_ms;
    uint32_t available_ms;
    uint32_t wait_ms;

    if (out == NULL || plan == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return true;
    }

    wait_ms = uptime_deadline_reached(now_ms, send_start_ms) ?
              0u : send_start_ms - now_ms;
    needed_ms = wait_ms +
                mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    if (uptime_deadline_reached(now_ms, plan->end_ms) ||
        uptime_deadline_reached(send_start_ms, plan->end_ms)) {
        if (required_ms != NULL) {
            *required_ms = needed_ms;
        }
        return false;
    }
    available_ms = uptime_ms_until_deadline(now_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = needed_ms;
    }
    return available_ms >= needed_ms;
}
#endif

static void mesh_wait_until_ms(uint32_t target_ms)
{
    while (!uptime_deadline_reached(k_uptime_get_32(), target_ms)) {
        uint32_t delay_ms = uptime_ms_until_deadline(k_uptime_get_32(), target_ms);

        k_msleep(delay_ms > 5u ? 5u : delay_ms);
    }
}

static void mesh_store_route_waiting_tx(const struct mesh_outbound *out)
{
    struct mesh_outbound waiting;
    struct app_mesh_paused_delivery_store_result store_result;

    if (!mesh_tx_can_wait_for_route(out)) {
        return;
    }

    waiting = *out;
    if (waiting.queued_at_ms == 0u) {
        waiting.queued_at_ms = k_uptime_get_32();
    }
    app_mesh_paused_delivery_note_store(&mesh_paused_delivery,
                                        mesh_route_waiting_tx_valid,
                                        &mesh_route_waiting_tx,
                                        &waiting,
                                        &store_result);
    if (store_result.replaced_existing) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        high_debug_log_event("MESH_PAUSED_DELIVERY",
                             "phase=replace old_msg=0x%02x old_dst=0x%016llx old_seq=%u new_msg=0x%02x new_dst=0x%016llx new_seq=%u lost=%u",
                             mesh_route_waiting_tx.packet.msg_type,
                             (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                             mesh_route_waiting_tx.packet.seq,
                             waiting.packet.msg_type,
                             (unsigned long long)waiting.packet.dst_id,
                             waiting.packet.seq,
                             store_result.lost_count);
        LOG_WRN("mesh paused delivery replaced older packet: old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u",
                mesh_route_waiting_tx.packet.msg_type,
                mesh_route_waiting_tx.packet.seq,
                waiting.packet.msg_type,
                waiting.packet.seq,
                store_result.lost_count);
    }
    mesh_attach_paused_delivery_loss(&waiting, "route-wait-store");
    mesh_route_waiting_tx = waiting;
    mesh_route_waiting_tx_valid = true;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_STORE msg=0x%02x dst=0x%llx next=0x%llx seq=%u ch=%u queued=%u\n",
                            waiting.packet.msg_type,
                            (unsigned long long)waiting.packet.dst_id,
                            (unsigned long long)waiting.next_hop_id,
                            waiting.packet.seq,
                            waiting.radio_channel,
                            waiting.queued_at_ms);
    }
    mesh_schedule_route_waiting_retry_after("route-wait-store", 0u);
}

static void mesh_defer_route_for_outbound(const struct mesh_outbound *out,
                                           const char *reason,
                                           bool store_route_wait)
{
    if (out == NULL || !mesh_id_is_unicast(out->packet.dst_id) ||
        out->packet.dst_id == DEVICE_ID) {
        return;
    }
    if (store_route_wait) {
        mesh_store_route_waiting_tx(out);
    } else {
        (void)mesh_schedule_route_request(out->packet.dst_id, reason);
    }
}

static bool mesh_parent_contact_failure_is_hard(int ret)
{
    return ret == -ETIMEDOUT || ret == -EHOSTUNREACH ||
           ret == -ENOTCONN || ret == -ECONNRESET;
}

static bool mesh_try_repair_selected_parent_event(
    const struct mesh_outbound *out,
    const char *reason,
    int select_ret,
    uint64_t selected_next_hop,
    bool store_route_wait,
    uint32_t *wait_retry_delay_ms)
{
    uint32_t retry_delay_ms = RELAY_BUSY_RETRY_MIN_MS;
    bool repair_active;
    bool hard_failure;
    int repair_ret;

    if (out == NULL || select_ret != PROTO_OK ||
        !mesh_id_is_unicast(selected_next_hop) ||
        selected_next_hop == DEVICE_ID || selected_next_hop == GATEWAY_ID) {
        return false;
    }

    if (store_route_wait) {
        mesh_store_route_waiting_tx(out);
    }
    repair_ret = mesh_propose_event_after_channel5_contact(
        selected_next_hop, "initial-tx-event-repair");
    repair_active = mesh_event_propose_retry.active;
    hard_failure = !repair_active && repair_ret < 0 &&
        mesh_parent_contact_failure_is_hard(repair_ret);

    if (repair_active && mesh_event_propose_retry.retry_due_ms != 0u) {
        retry_delay_ms = uptime_ms_until_deadline(
            k_uptime_get_32(), mesh_event_propose_retry.retry_due_ms);
        if (retry_delay_ms == 0u) {
            retry_delay_ms = 1u;
        }
    } else if (repair_ret == 0) {
        retry_delay_ms = 1u;
    }
    if (wait_retry_delay_ms != NULL) {
        *wait_retry_delay_ms = retry_delay_ms;
    }

    if (hard_failure) {
        mesh_relay_invalidate_upstream_route(&mesh_runtime);
        if (store_route_wait) {
            (void)mesh_schedule_route_request(out->packet.dst_id,
                                              "initial-tx-event-repair-failed");
        } else {
            mesh_defer_route_for_outbound(out,
                                           "initial-tx-event-repair-failed",
                                           false);
        }
    }
    status_debug_printf(
        "DBG_INITIAL_TX_EVENT_REPAIR peer=0x%llx repair=%d active=%u hard=%u delay=%u seq=%u reason=%s\n",
        (unsigned long long)selected_next_hop,
        repair_ret,
        repair_active ? 1u : 0u,
        hard_failure ? 1u : 0u,
        retry_delay_ms,
        out->packet.seq,
        reason == NULL ? "mesh-tx" : reason);
    return true;
}

static void mesh_drop_route_waiting_tx(const char *reason)
{
    struct app_mesh_paused_delivery_store_result drop_result;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);

    high_debug_log_event("MESH_ROUTE_WAIT_DROP",
                         "reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u lost=%u",
                         reason == NULL ? "drop" : reason,
                         mesh_route_waiting_tx.packet.msg_type,
                         (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                         mesh_route_waiting_tx.packet.seq,
                         mesh_runtime.route_discovery.attempts,
                         drop_result.lost_count);
    LOG_WRN("mesh route waiting packet dropped: reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u lost=%u",
            reason == NULL ? "drop" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            mesh_runtime.route_discovery.attempts,
            drop_result.lost_count);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_DROP_LOST lost=%u reason=%s\n",
                            drop_result.lost_count,
                            reason == NULL ? "drop" : reason);
    }
    mesh_route_reply_handoff_clear("route-wait-drop");
    mesh_route_waiting_tx_valid = false;
}

void mesh_clear_route_waiting_tx(const struct proto_packet *packet)
{
    if (!mesh_route_waiting_tx_valid || packet == NULL) {
        return;
    }
    if (mesh_route_waiting_tx.packet.dst_id == packet->dst_id &&
        mesh_route_waiting_tx.packet.session_id == packet->session_id &&
        mesh_route_waiting_tx.packet.seq == packet->seq) {
        mesh_route_reply_handoff_clear("route-wait-clear");
        mesh_route_waiting_tx_valid = false;
    }
}

bool mesh_route_waiting_tx_active(void)
{
    return mesh_route_waiting_tx_valid;
}

static int mesh_handle_direct_gateway_retry_policy(const struct mesh_outbound *tx,
                                                   const char *reason,
                                                   int tx_ret)
{
    const struct route_candidate *selected;
    enum route_delivery_action action;
    uint64_t next_hop_id = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t random_value = sys_rand32_get();
    uint32_t delay_ms = 0u;
    uint32_t base_delay_ms = 0u;
    uint8_t failure_count = 0u;
    int select_ret = PROTO_ERR_NOT_FOUND;
    int route_ret;

    if (tx == NULL || tx->packet.dst_id != GATEWAY_ID) {
        return tx_ret;
    }

    action = route_record_failure_at(&mesh_runtime.upstream,
                                     ROUTE_FAILURE_GATEWAY_ACK,
                                     now_ms);
    if (action != ROUTE_DELIVERY_DISCOVER) {
        select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                tx->packet.dst_id,
                                                &next_hop_id);
    }
    if (action != ROUTE_DELIVERY_DISCOVER && select_ret == PROTO_OK) {
        if (action == ROUTE_DELIVERY_RETRY_CURRENT) {
            selected = route_selected(&mesh_runtime.upstream);
            failure_count = selected == NULL ? 1u : selected->failure_count;
            base_delay_ms = route_retry_backoff_ms(failure_count);
            delay_ms = mesh_relay_retry_backoff_ms(failure_count,
                                                   random_value);
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        app_mesh_persistence_clear_outbox();
        report_tx_set_retry_delay_override(delay_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_ROUTE_RETRY action=%s seq=%u ret=%d fail=%u base=%u rand=%u delay=%u now=%u next=%u current=0x%llx selected=0x%llx reason=%s\n",
                                route_delivery_action_name(action),
                                tx->packet.seq,
                                tx_ret,
                                failure_count,
                                base_delay_ms,
                                random_value,
                                delay_ms,
                                now_ms,
                                now_ms + delay_ms,
                                (unsigned long long)tx->next_hop_id,
                                (unsigned long long)next_hop_id,
                                reason == NULL ? "direct-gateway" : reason);
        }
        return -EAGAIN;
    }

    mesh_relay_cancel_tx(&mesh_runtime);
    app_mesh_persistence_clear_outbox();
    mesh_store_route_waiting_tx(tx);
    mesh_direct_gateway_bulk_probe_suppress(now_ms, reason);
    route_ret = -EINPROGRESS;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_ROUTE_REPAIR action=%s seq=%u ret=%d route_ret=%d now=%u current=0x%llx reason=%s\n",
                            route_delivery_action_name(ROUTE_DELIVERY_DISCOVER),
                            tx->packet.seq,
                            tx_ret,
                            route_ret,
                            now_ms,
                            (unsigned long long)tx->next_hop_id,
                            reason == NULL ? "direct-gateway" : reason);
    }
    return -EHOSTUNREACH;
}

static int mesh_try_deferred_gateway_ack_on_channel9(
    const struct mesh_outbound *pending,
    const char *reason,
    bool *send_attempted,
    bool *policy_deferred,
    uint32_t *wait_retry_delay_ms)
{
    struct mesh_outbound *ack = &mesh_deferred_gateway_ack_scratch;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    int ret;

    if (send_attempted != NULL) {
        *send_attempted = false;
    }
    if (policy_deferred != NULL) {
        *policy_deferred = false;
    }
    if (wait_retry_delay_ms != NULL) {
        *wait_retry_delay_ms = 0u;
    }
    if (pending == NULL ||
        pending->packet.msg_type != MSG_GATEWAY_ACK ||
        !mesh_id_is_unicast(pending->next_hop_id)) {
        return -EINVAL;
    }

    *ack = *pending;
    mesh_fill_channel5_requirements(&requirements);
    now_ms = k_uptime_get_32();
    (void)mesh_expire_channel9_timings(now_ms, reason);
    ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                               ack->next_hop_id,
                                               &requirements,
                                               now_ms,
                                               &plan);
    if (ret != PROTO_ERR_STALE) {
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
    }
    mesh_debug_channel5_preemption("gateway-ack",
                                   reason,
                                   ack->next_hop_id,
                                   &requirements,
                                   &plan,
                                   now_ms);
    if (ret == PROTO_ERR_STALE || ret == PROTO_ERR_NOT_FOUND) {
        return -EHOSTUNREACH;
    }
    if (ret == PROTO_ERR_BUSY) {
        if (mesh_event_plan_is_policy_deferral(plan.action)) {
            mesh_relay_note_channel9_missed(&mesh_runtime,
                                            ack->next_hop_id,
                                            &mesh_event_stats);
            if (policy_deferred != NULL) {
                *policy_deferred = true;
            }
        } else if (plan.action == MESH_EVENT_PLAN_WAIT &&
                   wait_retry_delay_ms != NULL) {
            (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                now_ms,
                plan.start_ms,
                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                wait_retry_delay_ms);
        }
        return -EBUSY;
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    ack->radio_channel = MESH_EVENT_CHANNEL;
    if (send_attempted != NULL) {
        *send_attempted = true;
    }
    ret = mesh_send_outbound(ack, reason);
    if (ret == 0) {
        mesh_relay_note_tx_sent(&mesh_runtime, ack, k_uptime_get_32());
        mesh_note_channel9_local_tx(ack->next_hop_id, plan.start_ms);
    }
    return ret;
}

static void mesh_schedule_route_ready_event_retry(
    const struct app_mesh_route_ready_handoff_result *result,
    const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t retry_delay_ms;
    int defer_ret = PROTO_OK;

    if (result == NULL || !result->schedule_event_accept_wait ||
        !mesh_id_is_unicast(result->peer_id)) {
        return;
    }

    if (result->remember_deferred_peer) {
        mesh_route_ready_event_peer_id = result->peer_id;
    }
    retry_delay_ms = mesh_event_propose_retry.active &&
                     mesh_event_propose_retry.retry_due_ms != 0u ?
        uptime_ms_until_deadline(now_ms,
                                 mesh_event_propose_retry.retry_due_ms) :
        25u + (sys_rand32_get() % 51u);
    if (mesh_route_waiting_tx_valid) {
        mesh_schedule_route_waiting_retry_after(
            reason,
            retry_delay_ms);
        return;
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    defer_ret = mesh_relay_defer_pending_retry(
        &mesh_runtime,
        now_ms + retry_delay_ms);
    status_debug_printf("DBG_ROUTE_READY_EVENT_RETRY peer=0x%llx ret=%d delay=%u state=%u\n",
                        (unsigned long long)result->peer_id,
                        defer_ret,
                        retry_delay_ms,
                        mesh_runtime.pending.state);
    mesh_schedule_tx_timeout();
}

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound *pending = &mesh_route_waiting_tx_scratch;
    struct app_mesh_tx_handoff_result handoff_result;
    struct app_mesh_route_ready_handoff_result route_ready_result;
    struct app_mesh_route_wait_tx_decision wait_decision;
    struct app_mesh_coordinator_decision coordinator_decision;
    struct app_mesh_route_wait_tx_state wait_state = {
        .channel9_retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,
        .busy_retry_delay_ms = REPORT_TX_RETRY_DELAY_MS,
    };
    uint32_t now_ms;
    uint32_t gateway_ack_wait_retry_delay_ms = 0u;
    bool gateway_ack_send_attempted = false;
    bool gateway_ack_policy_deferred = false;
    bool route_wait_send_attempted = false;
    bool route_wait_policy_deferred = false;
    int ret;

    if (!mesh_route_waiting_tx_valid ||
        (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }
    mesh_coordinator_decide_now("route-wait", &coordinator_decision);
    if (!coordinator_decision.route_wait_allowed) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_WAIT_COORD_DEFER state=%s reason=%s q=%u\n",
                                app_mesh_coordinator_state_name(
                                    coordinator_decision.state),
                                coordinator_decision.reason == NULL ? "none" :
                                coordinator_decision.reason,
                                k_msgq_num_used_get(&mesh_rx_msgq));
        }
        mesh_schedule_route_waiting_retry_after("route-wait-coordinator",
                                                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_tx_handoff_gate_yields(
            APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING,
            mesh_route_waiting_tx_valid,
            mesh_route_reply_handoff_active(),
            k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            &handoff_result)) {
        status_debug_note("DBG_ROUTE_WAIT_RX_HANDOFF\n");
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_id_is_unicast(mesh_route_ready_event_peer_id)) {
        const struct app_mesh_route_ready_handoff_state route_ready_state = {
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = true,
            .deferred_peer_id = mesh_route_ready_event_peer_id,
        };

        app_mesh_route_ready_handoff_on_waiting_tx(&route_ready_state,
                                                   &route_ready_result);
        status_debug_printf("DBG_ROUTE_READY_WAIT_DECISION rxq=%u def=0x%llx prop=%u waitrx=%u allow=%u evwait=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            (unsigned long long)mesh_route_ready_event_peer_id,
                            route_ready_result.propose_deferred ? 1u : 0u,
                            route_ready_result.schedule_propose_wait_rx ? 1u : 0u,
                            route_ready_result.allow_waiting_tx ? 1u : 0u,
                            route_ready_result.schedule_event_accept_wait ? 1u : 0u);
        if (route_ready_result.schedule_propose_wait_rx) {
            status_debug_note("DBG_ROUTE_READY_PROPOSE_WAIT_RX\n");
            mesh_schedule_route_waiting_retry("route-ready-propose-wait-rx");
            return;
        }

        if (route_ready_result.propose_deferred) {
            int propose_ret;

            status_debug_note("DBG_ROUTE_READY_PROPOSE_DRAINED\n");
            LOG_INF("mesh route-ready deferred proposal after RX drain: next=0x%016llx delay_ms=%u",
                    (unsigned long long)route_ready_result.peer_id,
                    MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            if (route_ready_result.clear_deferred_peer) {
                mesh_route_ready_event_peer_id = 0u;
            }
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            propose_ret = mesh_propose_event_after_channel5_contact(
                route_ready_result.peer_id,
                "route-ready-drained-event-propose");
            app_mesh_route_ready_handoff_after_proposal(propose_ret,
                                                        &route_ready_result);
        }
        if (route_ready_result.schedule_event_accept_wait) {
            mesh_schedule_route_ready_event_retry(
                &route_ready_result,
                "route-ready-event-accept-wait");
            return;
        }
        if (!route_ready_result.allow_waiting_tx) {
            return;
        }
    }

    ret = k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        mesh_schedule_route_waiting_retry_after("route-wait-scratch-busy",
                                                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }

    *pending = mesh_route_waiting_tx;
    now_ms = k_uptime_get_32();
    wait_state.outbound_ready = mesh_outbound_ready_for_tx(pending, now_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_TRY msg=0x%02x dst=0x%llx ready=%u now=%u early=%u q=%u\n",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.dst_id,
                            wait_state.outbound_ready ? 1u : 0u,
                            now_ms,
                            pending->earliest_tx_ms,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    if (pending->packet.msg_type == MSG_GATEWAY_ACK &&
        mesh_id_is_unicast(pending->next_hop_id)) {
        if (!wait_state.outbound_ready) {
            mesh_schedule_route_waiting_retry("gateway-ack-not-ready");
            goto out_unlock;
        }

        ret = mesh_try_deferred_gateway_ack_on_channel9(
            pending,
            "gateway-ack-deferred",
            &gateway_ack_send_attempted,
            &gateway_ack_policy_deferred,
            &gateway_ack_wait_retry_delay_ms);
        if (ret == 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &pending->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

            app_mesh_rf_retry_note_success(
                &mesh_deferred_gateway_ack_rf_retry,
                &retry_key);
            mesh_route_waiting_tx_valid = false;
            goto out_unlock;
        }
        if (gateway_ack_send_attempted) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &pending->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);
            uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                &mesh_deferred_gateway_ack_rf_retry,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "gateway-ack-send-retry");

            mesh_schedule_route_waiting_retry_after(
                "gateway-ack-send-retry",
                retry_delay_ms);
            goto out_unlock;
        }
        if (ret == -EHOSTUNREACH) {
            (void)mesh_propose_event_after_channel5_contact(
                pending->next_hop_id,
                "gateway-ack-channel9-refresh");
            mesh_schedule_route_waiting_retry_after("gateway-ack-channel9-refresh",
                                                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            goto out_unlock;
        }
        if (ret == -EBUSY) {
            uint32_t retry_delay_ms = gateway_ack_wait_retry_delay_ms;

            if (gateway_ack_policy_deferred) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &pending->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-channel5-deferral");
            }
            if (retry_delay_ms == 0u) {
                retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
            }
            mesh_schedule_route_waiting_retry_after(
                gateway_ack_policy_deferred ?
                    "gateway-ack-channel5-deferral" :
                    "gateway-ack-channel9-wait",
                retry_delay_ms);
            goto out_unlock;
        }

        mesh_schedule_route_waiting_retry_after("gateway-ack-send-retry",
                                                REPORT_TX_RETRY_DELAY_MS);
        goto out_unlock;
    }

    if (wait_state.outbound_ready) {
        wait_state.tx_ret = mesh_start_tracked_tx_with_retry(
            pending,
            "route-discovered-packet",
            NULL,
            app_mesh_route_wait_tx_may_store(
                APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
            &route_wait_send_attempted,
            NULL,
            &route_wait_policy_deferred);
    }
    if (route_wait_send_attempted && wait_state.tx_ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);

        app_mesh_rf_retry_note_success(&mesh_route_wait_delivery_rf_retry,
                                       &retry_key);
    } else if (route_wait_send_attempted) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);
        uint32_t retry_delay_ms = 0u;
        bool policy_override = false;

#if DEVICE_ROLE == ROLE_ANCHOR
        if (wait_state.tx_ret == -EAGAIN) {
            policy_override = report_tx_consume_retry_delay_override(
                &retry_delay_ms);
        }
#endif
        if (!policy_override) {
            retry_delay_ms = mesh_rf_retry_next_delay_ms(
                &mesh_route_wait_delivery_rf_retry,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "route-waiting-send-retry");
        }
        mesh_schedule_route_waiting_retry_after(
            "route-waiting-send-retry",
            retry_delay_ms);
        goto out_unlock;
    }
    if (route_wait_policy_deferred) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
            &mesh_route_wait_delivery_rf_retry,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "route-waiting-channel5-deferral");

        mesh_schedule_route_waiting_retry_after(
            "route-waiting-channel5-deferral",
            retry_delay_ms);
        goto out_unlock;
    }
    app_mesh_route_wait_tx_decide(&wait_state, &wait_decision);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_DECISION tx=%d action=%u delay=%u req_attempt=%u req_ret=%d\n",
                            wait_state.tx_ret,
                            (unsigned int)wait_decision.action,
                            wait_decision.delay_ms,
                            wait_state.route_request_attempted ? 1u : 0u,
                            wait_state.route_request_ret);
    }
    switch (wait_decision.action) {
    case APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID:
        mesh_route_waiting_tx_valid = false;
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE:
        ret = mesh_request_route(pending->packet.dst_id, wait_decision.reason);
        wait_state.route_request_attempted = true;
        wait_state.route_request_ret = ret;
        app_mesh_route_wait_tx_decide(&wait_state, &wait_decision);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_WAIT_AFTER_REQ req_ret=%d action=%u delay=%u\n",
                                ret,
                                (unsigned int)wait_decision.action,
                                wait_decision.delay_ms);
        }
        if (wait_decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_DROP) {
            mesh_drop_route_waiting_tx(wait_decision.reason);
        }
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_DROP:
        mesh_drop_route_waiting_tx(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY:
        mesh_schedule_route_waiting_retry(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY:
        mesh_schedule_route_waiting_retry_after(wait_decision.reason,
                                                wait_decision.delay_ms);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY:
        mesh_schedule_route_waiting_exhausted_retry(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_NONE:
    default:
        break;
    }

out_unlock:
    k_mutex_unlock(&mesh_route_wait_scratch_lock);
}

static void mesh_route_waiting_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }
    mesh_try_route_waiting_tx();
}

static int mesh_start_tracked_tx_with_retry(const struct mesh_outbound *out,
                                            const char *reason,
                                            uint32_t *wait_retry_delay_ms,
                                            bool store_route_wait,
                                            bool *send_attempted,
                                            bool *rf_sent,
                                            bool *policy_deferred)
{
    struct mesh_outbound aged_out;
    struct mesh_outbound tx;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    uint32_t channel9_event_start_ms = 0u;
    uint64_t channel9_next_hop_id = 0u;
    bool channel9_success_pending = false;
    bool channel9_report_latency_pending = false;
    bool direct_gateway_tx_pending = false;
    bool send_prepared_c5_control = false;
    uint8_t send_prepared_c5_purpose = 0u;
    enum mesh_c5_control_send_mode send_prepared_c5_mode =
        MESH_C5_CONTROL_WAKE_IF_NEEDED;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }
    if (rf_sent != NULL) {
        *rf_sent = false;
    }
    if (send_attempted != NULL) {
        *send_attempted = false;
    }
    if (policy_deferred != NULL) {
        *policy_deferred = false;
    }
    if (wait_retry_delay_ms != NULL) {
        *wait_retry_delay_ms = 0u;
    }

    aged_out = *out;
    now_ms = k_uptime_get_32();
    mesh_outbound_refresh_age(&aged_out, now_ms);
    mesh_attach_paused_delivery_loss(&aged_out, reason);

    if (mesh_outbound_needs_result_offer(&aged_out)) {
        ret = mesh_relay_start_result_offer(&mesh_runtime,
                                            &aged_out.packet,
                                            aged_out.payload,
                                            aged_out.payload_len,
                                            now_ms,
                                            &tx);
        if (ret == PROTO_OK) {
            send_prepared_c5_control = true;
            send_prepared_c5_purpose = C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT;
            send_prepared_c5_mode = MESH_C5_CONTROL_WAKE_IF_NEEDED;
            goto send_prepared;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            mesh_defer_route_for_outbound(&aged_out, reason,
                                           store_route_wait);
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh result offer rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    if (mesh_packet_prefers_channel9(&aged_out.packet)) {
        uint64_t debug_next_hop = 0u;
        int debug_select_ret;

        debug_select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                      aged_out.packet.dst_id,
                                                      &debug_next_hop);
        if (debug_select_ret == PROTO_OK) {
            (void)mesh_advance_channel9_timing_past(debug_next_hop,
                                                    now_ms,
                                                    "tx-select");
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            DEVICE_ROLE != ROLE_GATEWAY &&
            aged_out.packet.dst_id == GATEWAY_ID &&
            debug_select_ret == PROTO_OK &&
            debug_next_hop == GATEWAY_ID) {
            ret = mesh_relay_start_tx(&mesh_runtime,
                                      &aged_out.packet,
                                      aged_out.payload,
                                      aged_out.payload_len,
                                      now_ms,
                                      &tx);
            if (ret == PROTO_OK) {
                tx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
                if (mesh_relay_tx_active(&mesh_runtime) &&
                    mesh_runtime.pending.packet.session_id == tx.packet.session_id &&
                    mesh_runtime.pending.packet.seq == tx.packet.seq) {
                    mesh_runtime.pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
                }
                channel9_success_pending = true;
                direct_gateway_tx_pending = true;
                channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
                channel9_event_start_ms = now_ms;
                channel9_next_hop_id = tx.next_hop_id;
                plan.start_ms = now_ms;
                plan.end_ms = now_ms + MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
                plan.window_ms = MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
                status_debug_printf("DBG_DIRECT_GW_TX seq=%u now=%u reason=%s\n",
                                    tx.packet.seq,
                                    now_ms,
                                    reason == NULL ? "mesh-tx" : reason);
                goto send_prepared;
            }
            if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
                mesh_defer_route_for_outbound(&aged_out, reason,
                                               store_route_wait);
                return -EHOSTUNREACH;
            }
            return mesh_errno_from_proto(ret);
        }
        mesh_fill_channel5_requirements(&requirements);
        ret = mesh_relay_start_channel9_tx(&mesh_runtime,
                                           &aged_out.packet,
                                           aged_out.payload,
                                           aged_out.payload_len,
                                           &requirements,
                                           now_ms,
                                           &plan,
                                           &tx);
        if (ret != PROTO_ERR_NOT_FOUND) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("tx",
                                       reason,
                                       aged_out.packet.dst_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
	        if (ret == PROTO_OK) {
	            uint32_t required_ms = 0u;
	            uint32_t fit_now_ms = k_uptime_get_32();
	            uint32_t event_counter = 0u;

	            tx.earliest_tx_ms = mesh_ch9_slot_send_start_ms(&tx, &plan, fit_now_ms);
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                (void)mesh_debug_channel9_state(tx.next_hop_id, &event_counter, NULL);
	                status_debug_printf("DBG_CH9_TX_SINGLE_SLOT cnt=%u now=%u start=%u end=%u txstart=%u seq=%u\n",
	                                    event_counter,
	                                    fit_now_ms,
	                                    plan.start_ms,
	                                    plan.end_ms,
	                                    tx.earliest_tx_ms,
	                                    tx.packet.seq);
	            }
            if (!mesh_ch9_tx_fits_plan(&tx, &plan, fit_now_ms, &required_ms)) {
                bool deferred = mesh_defer_active_collection_result("channel9-slot-full");

                if (!deferred) {
                    mesh_relay_cancel_tx(&mesh_runtime);
                    app_mesh_persistence_clear_outbox();
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_SINGLE_SLOT_FULL\n");
                    status_debug_note("DBG_CH9_TX_SINGLE_MISSED_SLOT\n");
                    status_debug_printf("DBG_CH9_TX_SINGLE_FULL now=%u end=%u req=%u seq=%u\n",
                                        fit_now_ms,
                                        plan.end_ms,
                                        required_ms,
                                        tx.packet.seq);
                }
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   tx.next_hop_id,
                                   &plan,
                                   reason);
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                tx.next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                return deferred ? 0 : -EBUSY;
            }
            channel9_success_pending = true;
            channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
            channel9_event_start_ms = plan.start_ms;
            channel9_next_hop_id = tx.next_hop_id;
            mesh_ch9_event_set(CH9_EVENT_GRANTED,
                               channel9_next_hop_id,
                               &plan,
                               reason);
            goto send_prepared;
        }
        if (ret == PROTO_ERR_BUSY) {
            if (policy_deferred != NULL) {
                *policy_deferred =
                    mesh_event_plan_is_policy_deferral(plan.action);
            }
            if (wait_retry_delay_ms != NULL &&
                plan.action == MESH_EVENT_PLAN_WAIT) {
                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    wait_retry_delay_ms);
            }
            mesh_debug_channel9_unavailable("tx",
                                            reason,
                                            &aged_out.packet,
                                            ret,
                                            debug_select_ret,
                                            debug_next_hop,
                                            &plan,
                                            now_ms);
            if (mesh_event_plan_debugs_channel5(plan.action)) {
                mesh_ch9_event_set(CH9_EVENT_PREEMPTED_BY_C5,
                                   debug_next_hop,
                                   &plan,
                                   reason);
            }
            return -EBUSY;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            mesh_debug_channel9_unavailable("tx",
                                            reason,
                                            &aged_out.packet,
                                            ret,
                                            debug_select_ret,
                                            debug_next_hop,
                                            &plan,
                                            now_ms);
            LOG_WRN("mesh channel-9 timing unavailable for %s; refreshing channel-5 contact: ret=%d",
                    reason,
                    ret);
            if (mesh_try_repair_selected_parent_event(
                    &aged_out,
                    reason,
                    debug_select_ret,
                    debug_next_hop,
                    store_route_wait,
                    wait_retry_delay_ms)) {
                return -EHOSTUNREACH;
            }
            mesh_defer_route_for_outbound(&aged_out, reason,
                                           store_route_wait);
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh channel-9 TX rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    ret = mesh_relay_start_tx(&mesh_runtime,
                              &aged_out.packet,
                              aged_out.payload,
                              aged_out.payload_len,
                              now_ms,
                              &tx);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh could not start tracked TX for %s: %d", reason, ret);
        if (ret == PROTO_ERR_NOT_FOUND) {
            mesh_defer_route_for_outbound(&aged_out, reason,
                                           store_route_wait);
        }
        return mesh_errno_from_proto(ret);
    }

send_prepared:
    if (channel9_success_pending) {
        mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                           channel9_next_hop_id,
                           &plan,
                           reason);
    }
    if (send_attempted != NULL) {
        *send_attempted = true;
    }
    if (send_prepared_c5_control) {
        ret = mesh_send_c5_control(&tx,
                                   send_prepared_c5_purpose,
                                   send_prepared_c5_mode,
                                   reason);
    } else if (direct_gateway_tx_pending) {
        ret = mesh_send_direct_gateway_payload_and_wait_ack(
            &tx,
            reason,
            &plan,
            channel9_event_start_ms,
            now_ms,
            channel9_report_latency_pending,
            rf_sent);
        if (ret == 0) {
            return 0;
        }
    } else {
        ret = mesh_send_outbound(&tx, reason);
        if (ret == 0 && rf_sent != NULL) {
            *rf_sent = true;
        }
    }
    if (ret < 0) {
        if (channel9_success_pending && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_FAIL\n");
            status_debug_printf("DBG_CH9_TX_SINGLE_FAIL ret=%d seq=%u\n",
                                ret,
                                tx.packet.seq);
        }
        if (channel9_success_pending) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               channel9_next_hop_id,
                               &plan,
                               reason);
        }
        if (direct_gateway_tx_pending && ret == -EAGAIN) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_SINGLE_RETRYABLE ret=%d seq=%u\n",
                                    ret,
                                    tx.packet.seq);
            }
            app_mesh_test_note_report_tx_retryable(tx.packet.seq, ret);
            return mesh_handle_direct_gateway_retry_policy(&tx, reason, ret);
        }
        if (channel9_success_pending && mesh_send_failure_retryable(ret)) {
            mesh_relay_cancel_tx(&mesh_runtime);
            app_mesh_persistence_clear_outbox();
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_SINGLE_RETRYABLE ret=%d seq=%u\n",
                                    ret,
                                    tx.packet.seq);
            }
            app_mesh_test_note_report_tx_retryable(tx.packet.seq, ret);
            return -EAGAIN;
        }
        if (mesh_defer_active_collection_result("send-failure")) {
            if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
                (void)mesh_schedule_route_request(
                    mesh_runtime.pending.packet.dst_id, reason);
            }
            return 0;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        app_mesh_persistence_clear_outbox();
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            /* No RF send completed, so no gateway-ACK cycle failed here. */
            mesh_defer_route_for_outbound(&tx, reason, store_route_wait);
        }
        return ret;
    }

    mesh_relay_note_tx_sent(&mesh_runtime, &tx, k_uptime_get_32());
    app_mesh_paused_delivery_note_sent(&mesh_paused_delivery, &tx);
    (void)mesh_save_outbox_durable("tracked-send");
    if (channel9_success_pending) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_OK\n");
        }
        mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                           channel9_next_hop_id,
                           &plan,
                           reason);
        mesh_note_channel9_local_tx(channel9_next_hop_id, channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms > now_ms ?
                                           channel9_event_start_ms - now_ms : 0u);
        }
        {
            bool batch_ack_wait = mesh_ch9_tx_pending_track_sent(
                &tx,
                k_uptime_get_32() + ROUTE_GATEWAY_ACK_TIMEOUT_MS);
            bool core_ack_wait = app_mesh_ch9_core_ack_wait_active(
                &mesh_runtime.pending,
                mesh_relay_tx_active(&mesh_runtime));

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_ACK_WAIT_SELECT batch=%u core=%u state=%u ch=%u next=0x%llx\n",
                                    batch_ack_wait ? 1u : 0u,
                                    core_ack_wait ? 1u : 0u,
                                    mesh_runtime.pending.state,
                                    mesh_runtime.pending.radio_channel,
                                    (unsigned long long)mesh_runtime.pending.next_hop_id);
            }
            if (batch_ack_wait || core_ack_wait) {
                uint32_t ack_deadline_ms = batch_ack_wait ?
                    mesh_ch9_tx_pending_ack_deadline_ms() :
                    mesh_runtime.pending.gateway_ack_deadline_ms;

                mesh_ch9_event_note_persistent_ack_wait(
                    channel9_next_hop_id,
                    ack_deadline_ms,
                    reason);
                mesh_schedule_tx_timeout();
                mesh_schedule_uwb_rx(0u);
                return 0;
            }
        }
    }
    mesh_schedule_tx_timeout();
    return 0;
}

int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    return mesh_start_tracked_tx_with_retry(
        out, reason, NULL,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
        NULL,
        NULL,
        NULL);
}

int mesh_start_owned_tracked_tx(const struct mesh_outbound *out,
                                const char *reason,
                                bool *rf_sent)
{
    return mesh_start_tracked_tx_with_retry(
        out, reason, NULL,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL),
        NULL,
        rf_sent,
        NULL);
}

int mesh_owned_tracked_tx_preflight(const struct mesh_outbound *out,
                                    const char *reason,
                                    enum app_mesh_route_wait_tx_owner owner,
                                    uint32_t generation)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (out == NULL ||
        owner != APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL ||
        generation == 0u || generation != out->packet.session_id) {
        return -EINVAL;
    }
    if (mesh_relay_tx_active(&mesh_runtime)) {
        return -EBUSY;
    }
    ret = mesh_relay_select_next_hop(&mesh_runtime,
                                     out->packet.dst_id,
                                     &next_hop_id);
    if (ret == PROTO_OK) {
        return 0;
    }
    if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
        (void)mesh_request_route_owned(out->packet.dst_id, reason, owner,
                                       &out->packet, generation);
        return -EHOSTUNREACH;
    }
    return mesh_errno_from_proto(ret);
}
