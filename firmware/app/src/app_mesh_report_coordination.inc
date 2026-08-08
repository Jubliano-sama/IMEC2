#line 673 __BASE_FILE__
static bool mesh_report_anchor_survey_discovery_is_pending(void)
{
    return mesh_report_callbacks != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending();
}

static uint8_t mesh_channel9_connection_count(void)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (mesh_runtime.event_timings[i].valid) {
            count++;
        }
    }
    return count;
}

static bool mesh_gateway_route_test_role(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY;
}

static bool mesh_gateway_route_test_peer_active(uint64_t peer_id)
{
    if (!mesh_gateway_route_test_role() || !mesh_id_is_unicast(peer_id)) {
        return false;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (entry->valid && entry->next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static bool mesh_gateway_route_test_slots_full_for(uint64_t peer_id)
{
    return mesh_gateway_route_test_role() &&
           !mesh_gateway_route_test_peer_active(peer_id) &&
           mesh_channel9_connection_count() >= MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS;
}

static bool mesh_gateway_route_test_ch5_scan_has_capacity(void)
{
    return !mesh_gateway_route_test_role() ||
           mesh_channel9_connection_count() < MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS;
}

static void mesh_gateway_route_test_clear_preempt(uint64_t peer_id, const char *reason)
{
    if (!mesh_gateway_route_test_role() ||
        mesh_gateway_route_preempt_peer_id == 0u ||
        (mesh_id_is_unicast(peer_id) && mesh_gateway_route_preempt_peer_id != peer_id)) {
        return;
    }

    status_debug_printf("DBG_GATEWAY_CH5_PREEMPT_CLEAR peer=0x%llx reason=%s\n",
                        (unsigned long long)mesh_gateway_route_preempt_peer_id,
                        reason == NULL ? "clear" : reason);
    mesh_gateway_route_preempt_peer_id = 0u;
    mesh_gateway_route_preempt_deadline_ms = 0u;
    app_node_comm_gateway_route_refresh_response_priority_clear();
}

static bool mesh_gateway_route_test_preempt_active(uint32_t now_ms)
{
    if (!mesh_gateway_route_test_role() || mesh_gateway_route_preempt_peer_id == 0u) {
        return false;
    }
    if (uptime_deadline_reached(now_ms, mesh_gateway_route_preempt_deadline_ms)) {
        mesh_gateway_route_test_clear_preempt(0u, "timeout");
        status_debug_note("DBG_GATEWAY_CH5_PREEMPT_TIMEOUT\n");
        return false;
    }
    return true;
}

static uint32_t mesh_gateway_route_test_preempt_window_ms(uint32_t now_ms)
{
    if (!mesh_gateway_route_test_preempt_active(now_ms)) {
        return 0u;
    }

    return uptime_ms_until_deadline(
        now_ms, mesh_gateway_route_preempt_deadline_ms);
}

static const char *mesh_c5_contact_purpose_name(uint8_t purpose)
{
    switch (purpose) {
    case C5_CONTACT_PURPOSE_ROUTE_SOLICIT:
        return "route_solicit";
    case C5_CONTACT_PURPOSE_ROUTE_REPLY:
        return "route_reply";
    case C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH:
        return "route_contact_refresh";
    case C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD:
        return "gateway_command_flood";
    case C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD:
        return "collection_eack_flood";
    case C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT:
        return "result_offer_grant";
    case C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION:
        return "channel9_timing_negotiation";
    default:
        return "unknown";
    }
}

static void mesh_c5_contact_log(const char *phase, const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    status_debug_printf("DBG_C5_CONTACT phase=%s state=%u peer=0x%016llx purpose=%s reason=%s\n",
                        phase == NULL ? "state" : phase,
                        (unsigned int)mesh_c5_contact.state,
                        (unsigned long long)mesh_c5_contact.peer_id,
                        mesh_c5_contact_purpose_name(mesh_c5_contact.purpose),
                        reason == NULL ? "contact" : reason);
}

static void mesh_c5_contact_clear(const char *reason)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE) {
        return;
    }

    mesh_c5_contact.state = C5_CONTACT_CLOSING;
    mesh_c5_contact_log("closing", reason);
    memset(&mesh_c5_contact, 0, sizeof(mesh_c5_contact));
}

static void mesh_c5_contact_clear_matching(uint64_t peer_id,
                                           uint8_t purpose,
                                           const char *reason)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE ||
        mesh_c5_contact.peer_id != peer_id ||
        mesh_c5_contact.purpose != purpose) {
        return;
    }

    mesh_c5_contact_clear(reason);
}

static bool mesh_c5_contact_expire_if_due(uint32_t now_ms)
{
    if (!app_mesh_c5_contact_expired(&mesh_c5_contact, now_ms)) {
        return false;
    }

    mesh_c5_contact_clear("expired");
    return true;
}

static bool mesh_c5_contact_active(uint64_t peer_id,
                                   uint8_t purpose,
                                   uint32_t now_ms)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE ||
        mesh_c5_contact.peer_id != peer_id ||
        mesh_c5_contact.purpose != purpose) {
        return false;
    }
    if (mesh_c5_contact_expire_if_due(now_ms)) {
        return false;
    }
    return app_mesh_c5_contact_accepted(&mesh_c5_contact,
                                        peer_id,
                                        purpose,
                                        now_ms);
}

static void mesh_c5_contact_open(uint64_t peer_id,
                                 uint8_t purpose,
                                 uint32_t contact_id,
                                 bool peer_was_woken,
                                 uint32_t expires_at_ms,
                                 const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return;
    }

    if (mesh_c5_contact.state != C5_CONTACT_NONE &&
        (mesh_c5_contact.peer_id != peer_id ||
         mesh_c5_contact.purpose != purpose)) {
        mesh_c5_contact_clear("replace");
    }

    mesh_c5_contact.peer_id = peer_id;
    mesh_c5_contact.contact_id = contact_id;
    mesh_c5_contact.purpose = purpose;
    mesh_c5_contact.peer_was_woken = peer_was_woken;
    mesh_c5_contact.accepted = false;
    mesh_c5_contact.opened_at_ms = now_ms;
    mesh_c5_contact.last_frame_at_ms = now_ms;
    mesh_c5_contact.expires_at_ms = expires_at_ms;
    mesh_c5_contact.state = peer_was_woken ?
                            C5_CONTACT_WAKE_PENDING :
                            C5_CONTACT_AWAKE_ACCEPTED;
    mesh_c5_contact_log("open", reason);
}

static void mesh_c5_contact_accept(uint64_t peer_id,
                                   uint8_t purpose,
                                   uint32_t expires_at_ms,
                                   const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    bool matching_contact;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return;
    }
    matching_contact = mesh_c5_contact.state != C5_CONTACT_NONE &&
                       mesh_c5_contact.peer_id == peer_id &&
                       mesh_c5_contact.purpose == purpose;
    if (matching_contact && mesh_c5_contact_expire_if_due(now_ms)) {
        matching_contact = false;
    }
    if (!matching_contact) {
        mesh_c5_contact_open(peer_id,
                             purpose,
                             now_ms,
                             false,
                             expires_at_ms,
                             reason);
    }
    mesh_c5_contact.accepted = true;
    mesh_c5_contact.last_frame_at_ms = now_ms;
    mesh_c5_contact.expires_at_ms = expires_at_ms;
    mesh_c5_contact.state = C5_CONTACT_AWAKE_ACCEPTED;
    mesh_c5_contact_log("accepted", reason);
}

static void mesh_c5_contact_exchange(uint64_t peer_id,
                                     uint8_t purpose,
                                     uint32_t expires_at_ms,
                                     const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();

    if (!mesh_c5_contact_active(peer_id, purpose, now_ms)) {
        mesh_c5_contact_accept(peer_id, purpose, expires_at_ms, reason);
    }
    if (mesh_c5_contact.peer_id == peer_id &&
        mesh_c5_contact.purpose == purpose) {
        mesh_c5_contact.accepted = true;
        mesh_c5_contact.last_frame_at_ms = now_ms;
        mesh_c5_contact.expires_at_ms = expires_at_ms;
        mesh_c5_contact.state = C5_CONTACT_EXCHANGE_ACTIVE;
        mesh_c5_contact_log("exchange", reason);
    }
}

static bool mesh_c5_contact_peer_active(uint64_t peer_id, uint32_t now_ms)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE ||
        mesh_c5_contact.peer_id != peer_id) {
        return false;
    }
    if (mesh_c5_contact_expire_if_due(now_ms)) {
        return false;
    }
    return mesh_c5_contact.accepted;
}

static uint32_t mesh_c5_exchange_duration_ms(uint8_t purpose)
{
    switch (purpose) {
    case C5_CONTACT_PURPOSE_ROUTE_REPLY:
        return MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS;
    case C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD:
    case C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD:
        return FLOOD_WAVE_MS + FLOOD_RELAY_BURST_MS + FLOOD_POST_ROOT_GUARD_MS;
    case C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT:
    case C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION:
    case C5_CONTACT_PURPOSE_ROUTE_SOLICIT:
    case C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH:
    default:
        return MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
}

static uint32_t mesh_route_reply_listen_window_ms(uint8_t route_ttl)
{
    const struct app_mesh_c5_route_reply_window_timing timing = {
        .base_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
        .wake_train_ms = WAKE_ADV_MS,
        .post_wake_route_rx_ms = MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS,
        .wake_to_route_delay_ms = MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
        .request_flood_burst_ms = FLOOD_RELAY_BURST_MS,
        .flood_forward_wave_ms = FLOOD_WAVE_MS,
        .route_reply_exchange_ms = MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS,
        .direct_gateway_probe_ms = MESH_GATEWAY_DIRECT_PROBE_ROUND_MS,
        .guard_ms = MESH_ROUTE_TEST_REPLY_WINDOW_GUARD_MS,
    };

    return app_mesh_c5_route_reply_listen_window_ms(route_ttl, &timing);
}

static bool mesh_direct_gateway_bulk_probe_install_suppressed(uint64_t target_id,
                                                              uint32_t now_ms)
{
    if (target_id != GATEWAY_ID ||
        !mesh_direct_gateway_bulk_suppressed) {
        return false;
    }
    if (uptime_deadline_reached(now_ms,
                                mesh_direct_gateway_bulk_suppressed_until_ms)) {
        mesh_direct_gateway_bulk_suppressed_until_ms = 0u;
        mesh_direct_gateway_bulk_suppressed = false;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS_EXPIRED now=%u\n",
                                now_ms);
        }
        return false;
    }
    return true;
}

static void __attribute__((unused))
mesh_direct_gateway_bulk_probe_suppress(uint32_t now_ms,
                                        const char *reason)
{
    uint32_t until_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY) {
        return;
    }

    until_ms = now_ms + ROUTE_PARENT_HOLDDOWN_MS;
    mesh_direct_gateway_bulk_suppressed_until_ms = until_ms;
    mesh_direct_gateway_bulk_suppressed = true;
    status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS now=%u until=%u reason=%s\n",
                        now_ms,
                        until_ms,
                        reason == NULL ? "direct-bulk-ack-timeout" : reason);
}

static void mesh_direct_gateway_bulk_probe_clear(const char *reason)
{
    if (!mesh_direct_gateway_bulk_suppressed) {
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS_CLEAR until=%u reason=%s\n",
                            mesh_direct_gateway_bulk_suppressed_until_ms,
                            reason == NULL ? "direct-bulk-ack" : reason);
    }
    mesh_direct_gateway_bulk_suppressed_until_ms = 0u;
    mesh_direct_gateway_bulk_suppressed = false;
}

static uint32_t mesh_c5_exchange_expires_at(uint8_t purpose)
{
    return k_uptime_get_32() + mesh_c5_exchange_duration_ms(purpose);
}

static bool mesh_click_priority_active(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR && anchor_click_window_active();
}

static void mesh_coordinator_decide_now(
    const char *owner,
    struct fw_radio_activity_decision *decision)
{
    const uint32_t rx_queue_used = k_msgq_num_used_get(&mesh_rx_msgq);
    const uint32_t report_queue_used = report_tx_queue_used();
    const bool relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    const struct fw_radio_activity_capture capture = {
        .click_active = mesh_click_priority_active(),
        .survey_pending = DEVICE_ROLE == ROLE_ANCHOR &&
                          mesh_report_anchor_survey_discovery_is_pending(),
        .rx_queue_used = rx_queue_used,
        .report_queue_used = report_queue_used,
        .relay_tx_active = relay_tx_active,
        .route_waiting_tx_active = mesh_route_waiting_tx_valid,
        .ch9_ack_wait_active = mesh_ch9_tx_pending_is_active() ||
            app_mesh_ch9_core_ack_wait_active(&mesh_runtime.pending,
                                              relay_tx_active),
        .ch9_ack_send_pending =
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table),
        .gateway_continuous_ch9 = mesh_gateway_route_test_role(),
    };
    bool state_changed;
    int ret;

    ret = app_mesh_command_orchestrator_decide(
        app_node_comm_gateway_control_context(),
        &capture,
        decision,
        &state_changed);
    if (ret < 0 || decision == NULL) {
        LOG_ERR("mesh coordinator capture rejected: %d", ret);
        return;
    }

    if (state_changed) {
        const char *state_name =
            fw_radio_activity_state_name(decision->state);

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_COORD state=%s owner=%s reason=%s rx=%u tx=%u relay=%u route=%u ack=%u qack=%u\n",
                                state_name,
                                owner == NULL ? "mesh" : owner,
                                decision->reason == NULL ? "none" :
                                decision->reason,
                                capture.rx_queue_used,
                                capture.report_queue_used,
                                capture.relay_tx_active ? 1u : 0u,
                                capture.route_waiting_tx_active ? 1u : 0u,
                                capture.ch9_ack_wait_active ? 1u : 0u,
                                capture.ch9_ack_send_pending ? 1u : 0u);
        }
    }
}

static bool mesh_coordinator_mesh_work_allowed(const char *owner)
{
    struct fw_radio_activity_decision decision;

    mesh_coordinator_decide_now(owner, &decision);
    if (decision.mesh_work_allowed) {
        return true;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_COORD_MESH_DEFER owner=%s state=%s reason=%s\n",
                            owner == NULL ? "mesh" : owner,
                            fw_radio_activity_state_name(decision.state),
                            decision.reason == NULL ? "none" :
                            decision.reason);
    }
    return false;
}

static bool mesh_outbound_reports_local_delivery_loss(const struct mesh_outbound *out)
{
    if (out == NULL || out->packet.src_id != DEVICE_ID ||
        out->packet.dst_id == MESH_BROADCAST_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static void mesh_attach_paused_delivery_loss(struct mesh_outbound *out,
                                             const char *reason)
{
    struct fw_delivery_loss_attach_result result;
    int ret;

    if (!mesh_outbound_reports_local_delivery_loss(out) ||
        fw_delivery_loss_count(&mesh_delivery_loss) == 0u) {
        return;
    }

    ret = fw_delivery_loss_attach(&mesh_delivery_loss, out, &result);
    if (ret == PROTO_OK) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_LOSS_TLV msg=0x%02x seq=%u lost=%u attach=%u update=%u reason=%s\n",
                                out->packet.msg_type,
                                out->packet.seq,
                                result.lost_count,
                                result.tlv_attached ? 1u : 0u,
                                result.tlv_updated ? 1u : 0u,
                                reason == NULL ? "mesh" : reason);
        }
        return;
    }

    LOG_WRN("mesh paused-delivery loss TLV not attached: msg=0x%02x seq=%u lost=%u ret=%d reason=%s",
            out->packet.msg_type,
            out->packet.seq,
            result.lost_count,
            ret,
            reason == NULL ? "mesh" : reason);
}

static uint8_t mesh_c5_purpose_for_packet(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return 0u;
    }

    switch (packet->msg_type) {
    case MSG_ROUTE_REQ:
        return C5_CONTACT_PURPOSE_ROUTE_SOLICIT;
    case MSG_ROUTE_REPLY:
    case MSG_ROUTE_REPLY_ACK:
        return C5_CONTACT_PURPOSE_ROUTE_REPLY;
    case MSG_GATEWAY_ROUTE_ADV:
    case MSG_GATEWAY_ACK:
    case MSG_RELAY_BUSY:
        return C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH;
    case MSG_COMMAND:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_DISCOVERY_START:
        return C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD;
    case MSG_GATEWAY_COLLECTION_EACK:
        return C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD;
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_OFFER:
    case MSG_RESULT_GRANT:
    case MSG_RESULT_BUSY:
        return C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT;
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
        return C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION;
    default:
        return 0u;
    }
}

static void mesh_note_c5_control_rx(const struct proto_packet *packet,
                                    uint64_t previous_hop_id,
                                    uint8_t radio_channel,
                                    const char *reason)
{
    uint8_t purpose;

    if (radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        !mesh_id_is_unicast(previous_hop_id) ||
        previous_hop_id == DEVICE_ID) {
        return;
    }

    purpose = mesh_c5_purpose_for_packet(packet);
    if (purpose == 0u) {
        return;
    }

    mesh_c5_contact_accept(previous_hop_id,
                           purpose,
                           mesh_c5_exchange_expires_at(purpose),
                           reason == NULL ? "c5-control-rx" : reason);
}

static bool mesh_c5_control_rx_semantically_admitted(
    const struct proto_packet *packet,
    const struct mesh_relay_result *result,
    bool admitted_event_control)
{
    if (packet == NULL || result == NULL ||
        mesh_c5_purpose_for_packet(packet) == 0u ||
        result->status == PROTO_ERR_MALFORMED) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
        return admitted_event_control;
    default:
        return true;
    }
}

static const char *mesh_ch9_event_state_name(enum ch9_event_state state)
{
    switch (state) {
    case CH9_EVENT_NONE:
        return "none";
    case CH9_EVENT_GRANTED:
        return "granted";
    case CH9_EVENT_TX_PAYLOAD:
        return "tx_payload";
    case CH9_EVENT_WAIT_CUSTODY_ACK:
        return "wait_custody_ack";
    case CH9_EVENT_COMPLETE:
        return "complete";
    case CH9_EVENT_BUSY_RETRY_LATER:
        return "busy_retry_later";
    case CH9_EVENT_WINDOW_EXPIRED:
        return "window_expired";
    case CH9_EVENT_PREEMPTED_BY_C5:
        return "preempted_by_c5";
    default:
        return "unknown";
    }
}

static void mesh_ch9_event_set(enum ch9_event_state state,
                               uint64_t peer_id,
                               const struct mesh_event_plan *plan,
                               const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_ch9_event_state = state;
    mesh_ch9_event_peer_id = peer_id;
    mesh_ch9_event_start_ms = plan == NULL ? 0u : plan->start_ms;
    mesh_ch9_event_end_ms = plan == NULL ? 0u : plan->end_ms;
    status_debug_printf("DBG_CH9_EVENT_STATE state=%s peer=0x%016llx start=%u end=%u reason=%s\n",
                        mesh_ch9_event_state_name(state),
                        (unsigned long long)mesh_ch9_event_peer_id,
                        mesh_ch9_event_start_ms,
                        mesh_ch9_event_end_ms,
                        reason == NULL ? "ch9-event" : reason);
}

static void mesh_ch9_event_note_persistent_ack_wait(uint64_t peer_id,
                                                    uint32_t deadline_ms,
                                                    const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    status_debug_printf("DBG_CH9_DELIVERY_WAIT peer=0x%016llx deadline=%u reason=%s\n",
                        (unsigned long long)peer_id,
                        deadline_ms,
                        reason == NULL ? "gateway-ack" : reason);
}

static void mesh_gateway_route_test_note_channel5_contact(uint64_t peer_id,
                                                          const char *reason)
{
    if (!mesh_gateway_route_test_role() || !mesh_id_is_unicast(peer_id)) {
        return;
    }
    if (mesh_gateway_route_test_slots_full_for(peer_id)) {
        status_debug_note("DBG_GATEWAY_CH5_FULL_IGNORE\n");
        status_debug_printf("DBG_GATEWAY_CH5_FULL peer=0x%llx active=%u reason=%s\n",
                            (unsigned long long)peer_id,
                            mesh_channel9_connection_count(),
                            reason == NULL ? "ch5" : reason);
        return;
    }

    mesh_gateway_route_preempt_peer_id = peer_id;
    mesh_gateway_route_preempt_deadline_ms =
        k_uptime_get_32() + MESH_GATEWAY_ROUTE_PREEMPT_MS;
    status_debug_printf("DBG_GATEWAY_CH5_PREEMPT peer=0x%llx until=%u reason=%s\n",
                        (unsigned long long)peer_id,
                        mesh_gateway_route_preempt_deadline_ms,
                        reason == NULL ? "ch5" : reason);
}

static bool mesh_gateway_route_test_should_reject_route_request(
    const struct mesh_rx_pending *pending)
{
    if (!mesh_gateway_route_test_role() ||
        pending == NULL ||
        pending->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        pending->packet.msg_type != MSG_ROUTE_REQ) {
        return false;
    }
    if (!mesh_gateway_route_test_slots_full_for(pending->previous_hop_id)) {
        mesh_gateway_route_test_note_channel5_contact(pending->previous_hop_id,
                                                      "route-req");
        return false;
    }

    status_debug_note("DBG_GATEWAY_ROUTE_REQ_REJECT_FULL\n");
    status_debug_printf("DBG_GATEWAY_ROUTE_REQ_REJECT prev=0x%llx src=0x%llx active=%u\n",
                        (unsigned long long)pending->previous_hop_id,
                        (unsigned long long)pending->packet.src_id,
                        mesh_channel9_connection_count());
    mesh_gateway_route_test_clear_preempt(pending->previous_hop_id,
                                          "route-req-full");
    return true;
}

static void mesh_restore_anchor_low_duty_if_no_ch9(const char *reason)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        mesh_channel9_connection_count() != 0u) {
        return;
    }

    if (anchor_uwb_scan_interval_ms != CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS) {
        anchor_uwb_scan_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS;
        status_debug_note("DBG_CH9_IDLE_CH5_LOW_DUTY\n");
        LOG_INF("mesh channel-9 idle; restored low-duty channel-5 scan: interval_ms=%u reason=%s",
                anchor_uwb_scan_interval_ms,
                reason == NULL ? "idle" : reason);
    }
#else
    ARG_UNUSED(reason);
#endif
}

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
static const char *mesh_ch9_direction_name(enum mesh_relay_channel9_direction direction)
{
    switch (direction) {
    case MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM:
        return "upstream";
    case MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM:
        return "downstream";
    case MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS:
        return "ambiguous";
    case MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *mesh_ch9_guard_reason_name(enum mesh_relay_channel9_guard_reason reason)
{
    switch (reason) {
    case MESH_RELAY_CHANNEL9_GUARD_OK:
        return "ok";
    case MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER:
        return "replaced-peer";
    case MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER:
        return "ambiguous-new-peer";
    case MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_ACTIVE_PEER:
        return "ambiguous-active-peer";
    case MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS:
        return "too-many-peers";
    case MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY:
        return "direction-busy";
    default:
        return "unknown";
    }
}
#endif

static int mesh_install_channel9_timing_direction(
    uint64_t peer_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    const char *reason)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        if (DEVICE_ROLE == ROLE_GATEWAY) {
            if (!mesh_event_timing_local_rx_slot(timing)) {
                status_debug_note("DBG_GATEWAY_CH9_REJECT_TX_FIRST\n");
                status_debug_printf("DBG_GATEWAY_CH9_REJECT peer=0x%llx active=%u reason=tx-first\n",
                                    (unsigned long long)peer_id,
                                    mesh_channel9_connection_count());
                LOG_WRN("mesh gateway rejected channel-9 timing that does not start RX: peer=0x%016llx reason=%s",
                        (unsigned long long)peer_id,
                        reason == NULL ? "install" : reason);
                return PROTO_ERR_MALFORMED;
            }
            status_debug_note("DBG_GATEWAY_CH9_CONTINUOUS_RX_NO_SLOT\n");
            status_debug_printf("DBG_GATEWAY_CH9_NO_SLOT peer=0x%llx cnt=%u next=%u reason=%s\n",
                                (unsigned long long)peer_id,
                                timing->event_counter,
                                timing->next_event_time_ms,
                                reason == NULL ? "install" : reason);
            return PROTO_OK;
        }
        struct mesh_relay_channel9_guard_status guard = {0};
        int ret;

        if (direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM ||
            direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
            ret = mesh_relay_set_channel9_timing_guarded_direction(
                &mesh_runtime,
                peer_id,
                timing,
                direction,
                MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                &guard);
        } else {
            ret = mesh_relay_set_channel9_timing_guarded(
                &mesh_runtime,
                peer_id,
                timing,
                MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                &guard);
        }

        if (ret != PROTO_OK) {
            const char *guard_reason = mesh_ch9_guard_reason_name(guard.reason);
            const char *direction = mesh_ch9_direction_name(guard.direction);
            const char *conflict_direction =
                mesh_ch9_direction_name(guard.conflict_direction);

            status_debug_note("DBG_CH9_GUARD_REJECT\n");
            status_debug_printf("DBG_CH9_GUARD peer=0x%llx ret=%d reason=%u dir=%u active=%u conflict=0x%llx cdir=%u\n",
                                (unsigned long long)peer_id,
                                ret,
                                (unsigned int)guard.reason,
                                (unsigned int)guard.direction,
                                guard.active_peer_count,
                                (unsigned long long)guard.conflict_peer_id,
                                (unsigned int)guard.conflict_direction);
            LOG_WRN("mesh route-test channel-9 timing rejected: peer=0x%016llx reason=%s direction=%s active=%u conflict=0x%016llx conflict_direction=%s ret=%d",
                    (unsigned long long)peer_id,
                    guard_reason,
                    direction,
                    guard.active_peer_count,
                    (unsigned long long)guard.conflict_peer_id,
                    conflict_direction,
                    ret);
        }
        return ret;
    }
#else
    ARG_UNUSED(direction);
    ARG_UNUSED(reason);
#endif

    return mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, timing);
}

static int mesh_install_channel9_timing(uint64_t peer_id,
                                        const struct mesh_event_timing *timing,
                                        const char *reason)
{
    return mesh_install_channel9_timing_direction(
        peer_id,
        timing,
        MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN,
        reason);
}

static bool mesh_ch9_ack_batch_clear_for_peer(uint64_t peer_id,
                                              const char *reason)
{
    const struct app_mesh_ch9_ack_batch *batch;
    uint8_t count;
    uint16_t first_seq;

    if (!mesh_id_is_unicast(peer_id)) {
        return false;
    }
    batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    if (batch == NULL) {
        return false;
    }

    count = batch->count;
    first_seq = batch->preserve_payload ? batch->template_ack.packet.seq :
                count == 0u ? 0u : batch->entries[0].seq;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_ACK_CLEAR peer=0x%llx n=%u first=%u reason=%s\n",
                            (unsigned long long)peer_id,
                            count,
                            first_seq,
                            reason == NULL ? "clear" : reason);
    }
    return app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer_id);
}

static uint8_t mesh_ch9_ack_batch_count_for_peer(uint64_t peer_id)
{
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);

    return batch == NULL ? 0u : batch->count;
}

static uint8_t mesh_expire_channel9_timings(uint32_t now_ms, const char *reason)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (entry->valid &&
            !mesh_event_timing_usable(&entry->timing, now_ms)) {
            mesh_event_owner_abandon_peer(entry->next_hop_id);
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
            const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

            if (!entry->valid || mesh_event_timing_usable(&entry->timing, now_ms)) {
                continue;
            }
            status_debug_note("DBG_CH9_TIMING_EXPIRE_DETAIL\n");
            (void)mesh_ch9_ack_batch_clear_for_peer(entry->next_hop_id,
                                                    reason == NULL ? "timing-expire" : reason);
        }
    }

    uint8_t expired = mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);

    if (expired > 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TIMING_EXPIRED\n");
        }
        mesh_restore_anchor_low_duty_if_no_ch9(reason);
    }
    return expired;
}

static bool mesh_find_active_channel9_timing(uint64_t peer_id,
                                             uint32_t now_ms,
                                             struct mesh_event_timing *timing)
{
    if (!mesh_id_is_unicast(peer_id) || timing == NULL) {
        return false;
    }

    (void)mesh_expire_channel9_timings(now_ms, "find-active");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (!entry->valid ||
            entry->next_hop_id != peer_id ||
            !mesh_event_timing_usable(&entry->timing, now_ms)) {
            continue;
        }
        *timing = entry->timing;
        return true;
    }

    return false;
}

static uint32_t mesh_channel9_skip_reference_ms(uint32_t now_ms)
{
    return now_ms - MESH_EVENT_RX_LATE_GUARD_MS;
}

static uint8_t mesh_advance_channel9_entry_past(struct mesh_relay_event_timing_entry *entry,
                                                uint32_t now_ms,
                                                const char *reason)
{
    uint8_t skipped;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || entry == NULL || !entry->valid) {
        return 0u;
    }

    skipped = mesh_event_skip_elapsed(&entry->timing,
                                      mesh_channel9_skip_reference_ms(now_ms),
                                      &mesh_event_stats);
    if (skipped > 0u) {
        status_debug_printf("DBG_CH9_SKIP_STALE n=%u now=%u next=%u cnt=%u reason=%s\n",
                            skipped,
                            now_ms,
                            entry->timing.next_event_time_ms,
                            entry->timing.event_counter,
                            reason == NULL ? "skip" : reason);
    }
    return skipped;
}

static uint8_t mesh_advance_channel9_timing_past(uint64_t peer_id,
                                                 uint32_t now_ms,
                                                 const char *reason)
{
    if (!mesh_id_is_unicast(peer_id)) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (entry->valid && entry->next_hop_id == peer_id) {
            return mesh_advance_channel9_entry_past(entry, now_ms, reason);
        }
    }

    return 0u;
}

static uint8_t mesh_advance_all_channel9_timings_past(uint32_t now_ms,
                                                      const char *reason)
{
    uint8_t skipped_total = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        uint8_t skipped = mesh_advance_channel9_entry_past(&mesh_runtime.event_timings[i],
                                                           now_ms,
                                                           reason);

        if ((uint8_t)(UINT8_MAX - skipped_total) < skipped) {
            skipped_total = UINT8_MAX;
        } else {
            skipped_total += skipped;
        }
    }

    return skipped_total;
}

static void mesh_note_channel9_local_tx(uint64_t next_hop_id, uint32_t event_start_ms)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_relay_note_channel9_tx(&mesh_runtime, next_hop_id, event_start_ms);
    } else {
        mesh_relay_note_channel9_success(&mesh_runtime, next_hop_id, event_start_ms);
    }
}

static void mesh_report_note_anchor_uwb_awake_since(int64_t start_ms,
                                                    uint32_t already_counted_us)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_note_uwb_awake_since != NULL) {
        mesh_report_callbacks->anchor_note_uwb_awake_since(start_ms, already_counted_us);
    }
}

static int mesh_report_anchor_handle_local_command(const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_local_command != NULL) {
        return mesh_report_callbacks->anchor_handle_local_command(
            packet, payload, payload_len);
    }
    return 0;
}

static void mesh_report_anchor_handle_survey_discovery_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_discovery_start != NULL) {
        mesh_report_callbacks->anchor_handle_survey_discovery_start(packet, payload, payload_len);
    }
}

static void mesh_report_anchor_handle_survey_pair_prepare(const struct proto_packet *packet,
                                                          const uint8_t *payload,
                                                          size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_pair_prepare != NULL) {
        mesh_report_callbacks->anchor_handle_survey_pair_prepare(packet, payload, payload_len);
    }
}

static int mesh_report_gateway_handle_survey_discovery_report(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t received_at_ms,
    uint64_t previous_hop_id,
    uint8_t radio_channel,
    uint8_t link_quality)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_handle_survey_discovery_report != NULL) {
        return mesh_report_callbacks->gateway_handle_survey_discovery_report(
            packet,
            payload,
            payload_len,
            received_at_ms,
            previous_hop_id,
            radio_channel,
            link_quality);
    }

    return -ENOTSUP;
}

static bool mesh_gateway_delivery_requires_semantic_acceptance(
    const struct proto_packet *packet)
{
    if (packet == NULL || DEVICE_ROLE != ROLE_GATEWAY ||
        packet->dst_id != DEVICE_ID ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_GATEWAY_ACK_CONFIRM:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static bool mesh_gateway_delivery_is_internal_control(
    const struct mesh_rx_pending *pending)
{
    const uint8_t *command_id_raw = NULL;
    uint8_t command_id_len = 0u;

    if (pending == NULL) {
        return false;
    }
    if (pending->packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        return true;
    }
    if (pending->packet.msg_type != MSG_COMMAND_RESULT) {
        return false;
    }

    /*
     * The command ID owns the namespace before the phase-specific parser
     * decides whether the assignment control is valid.  Otherwise a
     * well-framed assignment result with an unsupported phase/scheme can fall
     * through the generic command-result path after enumeration is terminal,
     * acquire host-output custody, and receive a gateway ACK even though the
     * assignment coordinator rejected it.
     */
    return tlv_find_unique(pending->payload,
                           pending->payload_len,
                           TLV_COMMAND_ID,
                           &command_id_raw,
                           &command_id_len) == PROTO_OK &&
           command_id_len == sizeof(uint16_t) &&
           proto_get_u16_le(command_id_raw) ==
               CMD_ASSIGN_DISCOVERY_SLOTS;
}

static int mesh_gateway_accept_semantic_delivery(
    const struct mesh_rx_pending *pending)
{
    const struct mesh_event_plan *current_channel9_plan;
    int assignment_admission;

    if (pending == NULL ||
        !mesh_gateway_delivery_requires_semantic_acceptance(&pending->packet)) {
        return -EINVAL;
    }
    if (!mesh_gateway_delivery_is_internal_control(pending)) {
        assignment_admission =
            gateway_discovery_assignment_admit_nonassignment_source(
                pending->packet.src_id);
        if (assignment_admission < 0) {
            return assignment_admission;
        }
        if (assignment_admission == 0) {
            /*
             * Keep the packet in sender custody. Once enumeration is terminal,
             * an unprovisioned survey anchor or synthetic bench source may
             * retry through the ordinary semantic path without having consumed
             * a candidate origin partition while the roster was being built.
             */
            return -EAGAIN;
        }
    }
    current_channel9_plan = pending->current_channel9_plan_valid ?
                            &pending->current_channel9_plan : NULL;

    switch (pending->packet.msg_type) {
    case MSG_GATEWAY_ACK_CONFIRM: {
        /* ACK_CONFIRM is an RF proof that the source received a previous
         * gateway ACK. GUI RAM owns host history, so the proof is validated
         * structurally and then handled idempotently without a firmware
         * receipt ledger. */
        return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }
    case MSG_CLICK_REPORT:
        if (report_validate_click_payload(&pending->packet,
                                          pending->payload,
                                          pending->payload_len) != PROTO_OK) {
            return -EINVAL;
        }
        /*
         * Host-delivery duplicate/retry gating is classified before this
         * application dispatcher. A fresh click reaches this case only after
         * validation capacity and bounded host-item custody are reserved;
         * transport identity is committed at the later semantic boundary.
         */
        return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    case MSG_SELF_TEST_REPORT:
        return proto_self_test_report_validate(
                   &pending->packet,
                   pending->payload,
                   pending->payload_len) == PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_ANCHOR_HEARTBEAT:
        return proto_anchor_heartbeat_validate(
                   &pending->packet,
                   pending->payload,
                   pending->payload_len) == PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_MESH_DATA:
        return pending->packet.flags ==
                       (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC) &&
                       pending->payload_len > 0u ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_COMMAND_RESULT:
        return gateway_note_command_result(&pending->packet,
                                           pending->payload,
                                           pending->payload_len,
                                           pending->first_received_at_ms,
                                           pending->result_validation_token,
                                           pending->previous_hop_id,
                                           pending->radio_channel,
                                           current_channel9_plan);
    case MSG_RESULT_BUNDLE:
        return gateway_note_command_result_bundle(&pending->packet,
                                                  pending->payload,
                                                  pending->payload_len,
                                                  pending->previous_hop_id,
                                                  pending->radio_channel,
                                                  current_channel9_plan);
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return mesh_report_gateway_handle_survey_discovery_report(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->first_received_at_ms,
            pending->previous_hop_id,
            pending->radio_channel,
            pending->link_quality);
    default:
        return -EINVAL;
    }
}

/*
 * Read-only semantic admission pass. The mesh RX worker owns all relevant
 * gateway protocol state across this call and the later host-custody and
 * semantic commit; collection checkpoints, when used, are separate state.
 */
static int mesh_gateway_preflight_semantic_delivery(
    const struct mesh_rx_pending *pending)
{
    int assignment_admission;

    if (pending == NULL ||
        !mesh_gateway_delivery_requires_semantic_acceptance(
            &pending->packet)) {
        return -EINVAL;
    }
    if (!mesh_gateway_delivery_is_internal_control(pending)) {
        assignment_admission =
            gateway_discovery_assignment_admit_nonassignment_source(
                pending->packet.src_id);
        if (assignment_admission < 0) {
            return assignment_admission;
        }
        if (assignment_admission == 0) {
            return -EAGAIN;
        }
    }

    switch (pending->packet.msg_type) {
    case MSG_GATEWAY_ACK_CONFIRM: {
        struct mesh_gateway_ack_confirm_identity identity;

        return mesh_gateway_ack_confirm_payload_parse(
                   &pending->packet,
                   pending->payload,
                   pending->payload_len,
                   &identity) == PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    }
    case MSG_CLICK_REPORT:
        return report_validate_click_payload(&pending->packet,
                                             pending->payload,
                                             pending->payload_len) ==
                   PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_SELF_TEST_REPORT:
        return proto_self_test_report_validate(
                   &pending->packet,
                   pending->payload,
                   pending->payload_len) == PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_ANCHOR_HEARTBEAT:
        return proto_anchor_heartbeat_validate(
                   &pending->packet,
                   pending->payload,
                   pending->payload_len) == PROTO_OK ?
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_MESH_DATA:
        return pending->packet.flags ==
                       (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC) &&
                       pending->payload_len > 0u ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_NEW : -EINVAL;
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
        return gateway_preflight_result_semantic_delivery(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->first_received_at_ms,
            pending->result_validation_token,
            pending->previous_hop_id);
    case MSG_SURVEY_DISCOVERY_REPORT: {
        int ret = gateway_survey_preflight_semantic_delivery(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->first_received_at_ms,
            pending->previous_hop_id,
            pending->radio_channel,
            pending->link_quality);

        return ret == -ESTALE ?
               APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW : ret;
    }
    case MSG_SURVEY_PAIR_RESULT:
        /*
         * A stale pair result still belongs to the sender's bounded result
         * delivery owner. Do not project it as recovered host data or commit a
         * gateway ACK: the live round must remain untouched and the sender
         * retires this obsolete custody only at its existing deadline.
         */
        return gateway_survey_preflight_semantic_delivery(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->first_received_at_ms,
            pending->previous_hop_id,
            pending->radio_channel,
            pending->link_quality);
    default:
        return -EINVAL;
    }
}

static void mesh_rx_pending_refresh_age(struct mesh_rx_pending *pending, uint32_t now_ms)
{
    if (pending == NULL) {
        return;
    }

    if (pending->received_at_valid) {
        packet_age_add_elapsed(&pending->packet, now_ms - pending->received_at_ms);
    }
    pending->received_at_ms = now_ms;
    pending->received_at_valid = true;
}

int mesh_preempt_for_click_event(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_click_preempt_plan plan;
    struct app_mesh_click_preempt_ops ops = {
        .save_deferred_outbox = mesh_preempt_save_outbox,
        .clear_outbox = mesh_preempt_clear_outbox,
        .stage_click_handoff = mesh_preempt_stage_click_handoff,
        .commit_click_handoff = mesh_preempt_commit_click_handoff,
        .rollback_click_handoff = mesh_preempt_rollback_click_handoff,
        .cancel_timeout = mesh_preempt_cancel_timeout,
        .schedule_timeout = mesh_preempt_schedule_timeout,
        .requeue_click_report = mesh_preempt_requeue_click_report,
        .discard_requeued_click_report = mesh_preempt_discard_requeued_click_report,
        .cancel_active_tx = mesh_preempt_cancel_active_tx,
    };
    struct app_mesh_click_preempt_result result;
    int ret;

    if (mesh_prepare_click_preemption(&mesh_runtime,
                                      DEVICE_ID,
                                      k_uptime_get_32(),
                                      &plan) != PROTO_OK) {
        return -EINVAL;
    }

    ret = app_mesh_apply_click_preempt_plan(&plan, &ops, &result);

    if (result.outbox_saved) {
        LOG_INF("mesh pending collection result deferred: reason=click-preempt");
    }
    if (result.outbox_saved || result.outbox_cleared) {
        LOG_INF("anchor click discovery preempted active mesh TX");
    }
    if (result.click_report_requeue_failed) {
        LOG_ERR("click preemption retained the committed handoff because click-report queue admission failed: ret=%d",
                result.click_report_requeue_ret);
    }
    if (ret < 0) {
        LOG_ERR("click preemption custody operation failed: ret=%d save=%d stage=%d commit=%d rollback=%d schedule=%d requeue=%d cancel=%d clear=%d owner=%u",
                ret,
                result.save_outbox_ret,
                result.stage_click_handoff_ret,
                result.commit_click_handoff_ret,
                result.rollback_click_handoff_ret,
                result.schedule_timeout_ret,
                result.click_report_requeue_ret,
                result.cancel_timeout_ret,
                result.clear_outbox_ret,
                (unsigned int)result.custody_owner);
        return ret;
    }
#endif
    return 0;
}

static uint32_t mesh_outbox_persistence_generation;
static uint32_t mesh_child_custody_persistence_generation;

static uint32_t mesh_persistence_generation_begin(uint32_t *generation)
{
    uint32_t next_generation;

    if (generation == NULL) {
        return 0u;
    }

    next_generation = *generation + 1u;
    if (next_generation == 0u) {
        next_generation = 1u;
    }
    *generation = next_generation;
    return next_generation;
}

static bool mesh_persistence_generation_is_current(uint32_t current_generation,
                                                   uint32_t generation)
{
    return generation != 0u && current_generation == generation;
}

static void mesh_persistence_retry_round_reset_if_clean(void)
{
    if (!mesh_outbox_persistence_dirty &&
        !mesh_child_custody_persistence_dirty) {
        mesh_persistence_retry_round = 0u;
    }
}

static void mesh_schedule_persistence_retry(const char *reason)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        mesh_persistence_retry_round,
        sys_rand32_get());
    int ret;

    if (mesh_persistence_retry_round < UINT8_MAX) {
        mesh_persistence_retry_round++;
    }
    ret = mesh_route_owner_work_reschedule(&mesh_persistence_retry_work,
                                           delay_ms);
    if (ret < 0) {
        LOG_ERR("mesh persistence retry owner scheduling failed closed: reason=%s ret=%d",
                reason == NULL ? "unknown" : reason,
                ret);
        app_watchdog_stop_feeding();
        return;
    }
    LOG_WRN("mesh persistence retry scheduled: reason=%s round=%u delay_ms=%u",
            reason == NULL ? "unknown" : reason,
            mesh_persistence_retry_round,
            delay_ms);
}

static int mesh_save_outbox_durable(const char *reason)
{
    uint32_t generation = mesh_persistence_generation_begin(
        &mesh_outbox_persistence_generation);
    int ret;

    (void)reason;
    /* Outbox flash persistence is removed: the in-RAM relay/queue is the
     * sole custody owner, so the durable save is a no-op that always
     * succeeds and never leaves a dirty slot behind. */
    mesh_outbox_persistence_dirty = false;
    ret = 0;
    if (ret < 0) {
        if (mesh_persistence_generation_is_current(
                mesh_outbox_persistence_generation, generation)) {
            mesh_schedule_persistence_retry(reason);
        }
        return ret;
    }
    if (!mesh_persistence_generation_is_current(
            mesh_outbox_persistence_generation, generation)) {
        return 0;
    }

    /* Reconcile the in-RAM deferred owner after every primary save so a
     * completed transaction is never left as a pending deferred slot. */
    mesh_deferred_outbox_pending = false;
    if (!mesh_persistence_generation_is_current(
            mesh_outbox_persistence_generation, generation)) {
        return 0;
    }

    if (mesh_persistence_generation_is_current(
            mesh_outbox_persistence_generation, generation)) {
        mesh_outbox_persistence_dirty = false;
        mesh_persistence_retry_round_reset_if_clean();
    }
    return ret;
}

static int mesh_save_child_custody_durable(const char *reason)
{
    uint32_t generation = mesh_persistence_generation_begin(
        &mesh_child_custody_persistence_generation);
    int ret;

    (void)reason;
    /* Child-custody flash persistence is removed; the in-RAM relay owns the
     * custody, so this save is a no-op that always succeeds. */
    mesh_child_custody_persistence_dirty = false;
    ret = 0;
    if (ret < 0) {
        if (mesh_persistence_generation_is_current(
                mesh_child_custody_persistence_generation, generation)) {
            mesh_schedule_persistence_retry(reason);
        }
    } else if (mesh_persistence_generation_is_current(
                   mesh_child_custody_persistence_generation, generation)) {
        mesh_child_custody_persistence_dirty = false;
        mesh_persistence_retry_round_reset_if_clean();
    }
    return ret;
}

static void mesh_persistence_retry_work_handler(struct k_work *work)
{
    bool outbox_retry_pending;

    ARG_UNUSED(work);

    outbox_retry_pending = mesh_outbox_persistence_dirty;
    if (outbox_retry_pending) {
        if (mesh_save_outbox_durable("outbox-dirty") < 0) {
            return;
        }
        if (mesh_relay_tx_active(&mesh_runtime) &&
            mesh_runtime.pending.packet.msg_type ==
                MSG_GATEWAY_ACK_CONFIRM) {
            (void)mesh_schedule_tx_timeout();
        }
    }
    if (mesh_child_custody_persistence_dirty &&
        mesh_save_child_custody_durable("child-custody-dirty") < 0) {
        return;
    }
    mesh_persistence_retry_round_reset_if_clean();
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_preempt_save_outbox(void *ctx)
{
    ARG_UNUSED(ctx);

    /* Deferred-outbox flash persistence is removed. The in-RAM deferred
     * custody owner is recorded by the pending flag alone. */
    mesh_deferred_outbox_pending = true;
    return 0;
}

static int mesh_preempt_clear_outbox(void *ctx)
{
    const struct proto_packet *released_packet =
        mesh_relay_tx_active(&mesh_runtime) ?
        &mesh_runtime.pending.packet : NULL;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    ARG_UNUSED(ctx);

    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_survey_delivery_transport_released != NULL) {
        if (released_packet == NULL ||
            !mesh_packet_semantic_digest(
                released_packet,
                mesh_runtime.pending.payload,
                mesh_runtime.pending.payload_len,
                semantic_digest)) {
            return -EBADMSG;
        }
        ret =
            mesh_report_callbacks->anchor_survey_delivery_transport_released(
                released_packet, semantic_digest, true);
        if (ret < 0 && ret != -ENOENT && ret != -ESTALE &&
            ret != -ENOTSUP) {
            return ret;
        }
    }
    return 0;
}

static int mesh_preempt_stage_click_handoff(void *ctx,
                                            const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return 0;
}

static int mesh_preempt_commit_click_handoff(void *ctx,
                                             const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return 0;
}

static int mesh_preempt_rollback_click_handoff(void *ctx,
                                               const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return 0;
}

static int mesh_preempt_cancel_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_cancel_delayable(&mesh_tx_timeout_work);
}

static int mesh_preempt_schedule_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_schedule_tx_timeout();
}

static int mesh_preempt_requeue_click_report(void *ctx,
                                              const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);

    return queue_anchor_report(outbound);
}

static bool mesh_preempt_outbound_matches(const struct mesh_outbound *left,
                                          const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == right->packet.msg_type &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq &&
           left->payload_len == right->payload_len &&
           (left->payload_len == 0u ||
            memcmp(left->payload, right->payload, left->payload_len) == 0);
}

static uint32_t mesh_preempt_report_queue_count(void *ctx)
{
    return k_msgq_num_used_get((struct k_msgq *)ctx);
}

static int mesh_preempt_report_queue_peek(struct mesh_outbound *outbound,
                                          void *ctx)
{
    return k_msgq_peek((struct k_msgq *)ctx, outbound);
}

static int mesh_preempt_report_queue_get(struct mesh_outbound *outbound,
                                         void *ctx)
{
    return k_msgq_get((struct k_msgq *)ctx, outbound, K_NO_WAIT);
}

static int mesh_preempt_report_queue_put(const struct mesh_outbound *outbound,
                                         void *ctx)
{
    return k_msgq_put((struct k_msgq *)ctx, outbound, K_NO_WAIT);
}

static int mesh_preempt_report_queue_recover(
    const struct mesh_outbound *outbound,
    void *ctx)
{
    ARG_UNUSED(ctx);
    if (report_tx_queue_recovery_valid) {
        return -ENOSPC;
    }
    report_tx_queue_overflow_dropped = *outbound;
    report_tx_queue_recovery_valid = true;
    report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
    return 0;
}

static bool mesh_preempt_report_queue_matches(
    const struct mesh_outbound *candidate,
    const struct mesh_outbound *target,
    void *ctx)
{
    ARG_UNUSED(ctx);
    return mesh_preempt_outbound_matches(candidate, target);
}

static const struct app_mesh_queue_remove_ops report_tx_queue_remove_ops = {
    .count = mesh_preempt_report_queue_count,
    .get = mesh_preempt_report_queue_get,
    .put = mesh_preempt_report_queue_put,
    .recover = mesh_preempt_report_queue_recover,
    .matches = mesh_preempt_report_queue_matches,
    .ctx = &report_tx_msgq,
};

static const struct app_mesh_queue_head_ops report_tx_queue_head_ops = {
    .peek = mesh_preempt_report_queue_peek,
    .get = mesh_preempt_report_queue_get,
    .recover = mesh_preempt_report_queue_recover,
    .matches = mesh_preempt_report_queue_matches,
    .ctx = &report_tx_msgq,
};

/* Caller holds report_tx_queue_overflow_lock. */
static int report_tx_queue_restore_recovery_locked(void)
{
    int ret;

    if (!report_tx_queue_recovery_valid) {
        return 0;
    }
    ret = k_msgq_put(&report_tx_msgq,
                     &report_tx_queue_overflow_dropped,
                     K_NO_WAIT);
    if (ret == 0) {
        report_tx_queue_recovery_valid = false;
    }
    return ret;
}

/* Caller holds report_tx_queue_overflow_lock. */
static int report_tx_queue_promote_local_locked(void)
{
    uint32_t queue_count;

    if (app_mesh_queue_head_owned(&report_tx_queue_head_owner)) {
        return -EBUSY;
    }

    /*
     * A full queue can leave the next local item in the explicit recovery
     * owner.  Swap it with the current head before rotating, so the local
     * item runs next while the displaced transit packet remains owned.
     */
    if (report_tx_queue_recovery_valid &&
        mesh_outbound_is_local_origin_priority(
            &report_tx_queue_overflow_dropped)) {
        if (k_msgq_get(&report_tx_msgq,
                       &report_tx_queue_rotation_scratch,
                       K_NO_WAIT) != 0) {
            return -EIO;
        }
        if (k_msgq_put(&report_tx_msgq,
                       &report_tx_queue_overflow_dropped,
                       K_NO_WAIT) != 0) {
            if (k_msgq_put(&report_tx_msgq,
                           &report_tx_queue_rotation_scratch,
                           K_NO_WAIT) != 0) {
                (void)mesh_preempt_report_queue_recover(
                    &report_tx_queue_rotation_scratch,
                    &report_tx_msgq);
            }
            return -EIO;
        }
        report_tx_queue_overflow_dropped =
            report_tx_queue_rotation_scratch;
    }

    queue_count = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    for (uint32_t i = 0u;
         i < queue_count && i < REPORT_TX_QUEUE_DEPTH;
         i++) {
        if (k_msgq_peek(&report_tx_msgq,
                        &report_tx_queue_rotation_scratch) != 0) {
            return -EIO;
        }
        if (mesh_outbound_is_local_origin_priority(
                &report_tx_queue_rotation_scratch)) {
            return 0;
        }
        if (k_msgq_get(&report_tx_msgq,
                       &report_tx_queue_rotation_scratch,
                       K_NO_WAIT) != 0) {
            return -EIO;
        }
        if (k_msgq_put(&report_tx_msgq,
                       &report_tx_queue_rotation_scratch,
                       K_NO_WAIT) != 0) {
            (void)mesh_preempt_report_queue_recover(
                &report_tx_queue_rotation_scratch,
                &report_tx_msgq);
            return -EIO;
        }
    }
    return -ENOENT;
}

static int report_tx_queue_peek(struct mesh_outbound *outbound)
{
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_reservation.active) {
        ret = -EBUSY;
    } else {
        (void)report_tx_queue_restore_recovery_locked();
        (void)report_tx_queue_promote_local_locked();
        ret = k_msgq_peek(&report_tx_msgq, outbound);
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
}

static int report_tx_queue_append(const struct mesh_outbound *outbound)
{
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_reservation.active) {
        /*
         * The batch owns every nominal queue slot until its final fragment is
         * visible.  Letting a retry append here would interleave unrelated
         * custody with a partial batch, and abort could then remove that retry
         * while rotating away the batch prefix.
         */
        ret = -EAGAIN;
    } else {
        (void)report_tx_queue_restore_recovery_locked();
    }
    if (ret == -EAGAIN) {
        /* Keep the caller's existing retry owner unchanged. */
    } else if (report_tx_queue_recovery_valid) {
        ret = -ENOSPC;
    } else {
        ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
        if (ret == 0 &&
            mesh_outbound_is_local_origin_priority(outbound)) {
            (void)report_tx_queue_promote_local_locked();
        }
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
}

static int report_tx_queue_begin_head(
    struct mesh_outbound *outbound,
    struct app_mesh_queue_head_token *token)
{
    int ret;

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    if (anchor_range_report_batch_reservation.active) {
        ret = -EBUSY;
    } else {
        (void)report_tx_queue_restore_recovery_locked();
        (void)report_tx_queue_promote_local_locked();
        ret = app_mesh_queue_head_begin(&report_tx_queue_head_owner,
                                        &report_tx_queue_head_ops,
                                        outbound,
                                        token);
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
}

static int report_tx_queue_abort_head(
    const struct app_mesh_queue_head_token *token)
{
    int ret;

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    ret = app_mesh_queue_head_abort(&report_tx_queue_head_owner, token);
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
}

/* Caller holds mesh_send_scratch_lock, which owns mesh_send_scratch_tx. */
static int report_tx_queue_commit_head_locked(
    const struct app_mesh_queue_head_token *token,
    const struct mesh_outbound *expected)
{
    int ret;

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret != 0) {
        return ret;
    }
    ret = app_mesh_queue_head_commit(&report_tx_queue_head_owner,
                                     &report_tx_queue_head_ops,
                                     token,
                                     expected,
                                     &mesh_send_scratch_tx);
    if (ret == 0) {
        (void)report_tx_queue_restore_recovery_locked();
        (void)report_tx_queue_promote_local_locked();
    }
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    if (ret == 0) {
        uint16_t queue_depth = (uint16_t)report_tx_queue_used();

        app_stack_workload_diag_click_release(&mesh_send_scratch_tx.packet,
                                              0, queue_depth, 0u);
        app_stack_workload_diag_cir_release(&mesh_send_scratch_tx.packet,
                                            0, queue_depth, 0u);
    }
    return ret;
}

static int report_tx_queue_commit_head(
    const struct app_mesh_queue_head_token *token,
    const struct mesh_outbound *expected)
{
    int ret = k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);

    if (ret != 0) {
        return ret;
    }
    ret = report_tx_queue_commit_head_locked(token, expected);
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

static int mesh_preempt_discard_requeued_click_report(
    void *ctx,
    const struct mesh_outbound *outbound)
{
    bool removed = false;
    int ret;

    ARG_UNUSED(ctx);
    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_send_scratch_lock, K_NO_WAIT);
    if (ret != 0) {
        return ret;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT);
    if (ret != 0) {
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }
    if (anchor_range_report_batch_reservation.active) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        k_mutex_unlock(&mesh_send_scratch_lock);
        return -EBUSY;
    }
    (void)report_tx_queue_restore_recovery_locked();
    ret = app_mesh_queue_remove_first_owned(&report_tx_queue_head_owner,
                                            &report_tx_queue_remove_ops,
                                            outbound,
                                            &mesh_send_scratch_tx,
                                            &removed);
    (void)report_tx_queue_restore_recovery_locked();
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

static int mesh_preempt_cancel_active_tx(void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_cancel_tx(&mesh_runtime);
    return 0;
}
#endif

static int mesh_collection_deferral_save_outbox(struct mesh_relay *relay,
                                                uint32_t now_ms,
                                                void *ctx)
{
    ARG_UNUSED(ctx);

    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);

    return mesh_save_outbox_durable("collection-deferral");
}

static int mesh_collection_deferral_schedule_retry(void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_schedule_tx_timeout();
    return 0;
}

static int mesh_handoff_save_child_custody(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_save_child_custody_durable("handoff");
}

static void mesh_handoff_note_result_bundle_forwarded(const struct mesh_outbound *out,
                                                      void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_result_bundle_note_forwarded(&mesh_runtime, out);
}

static int mesh_handoff_send_result_grant(const struct mesh_outbound *out,
                                          void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_send_c5_control(out,
                                C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
                                MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
                                "result-grant");
}

static void mesh_handoff_note_tx_sent(const struct mesh_outbound *out,
                                      void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
}

static int mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;
    bool deferred_pending;

    /* Refresh only when no in-RAM owner exists; the in-RAM deferred flag is
     * the explicit owner that keeps this work item alive after a local ACK. */
    if (!mesh_relay_tx_active(&mesh_runtime) &&
        !mesh_ch9_tx_pending_is_active() &&
        !mesh_relay_result_bundle_pending(&mesh_runtime)) {
        int deferred_ret = mesh_deferred_outbox_pending ? 1 : 0;

        if (deferred_ret >= 0) {
            mesh_deferred_outbox_pending = deferred_ret > 0;
        } else if (deferred_ret != -ENOTSUP) {
            mesh_deferred_outbox_pending = true;
        }
    }
    deferred_pending = mesh_deferred_outbox_pending;

    if (!app_mesh_tx_timeout_work_needed(
            mesh_relay_tx_active(&mesh_runtime),
            mesh_ch9_tx_pending_is_active(),
            mesh_relay_result_bundle_pending(&mesh_runtime),
            deferred_pending)) {
        return mesh_cancel_delayable(&mesh_tx_timeout_work);
    }

    deadline = UINT32_MAX;
    if (mesh_relay_tx_active(&mesh_runtime)) {
        deadline = mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ?
                   mesh_runtime.pending.retry_after_ms :
                   mesh_runtime.pending.gateway_ack_deadline_ms;
    }
    if (mesh_ch9_tx_pending_is_active() &&
        (deadline == UINT32_MAX ||
         uptime_deadline_reached(deadline,
                                 mesh_ch9_tx_pending_ack_deadline_ms()))) {
        deadline = mesh_ch9_tx_pending_ack_deadline_ms();
    }
    if (mesh_relay_result_bundle_pending(&mesh_runtime) &&
        (deadline == UINT32_MAX ||
         uptime_deadline_reached(deadline,
                                 mesh_relay_result_bundle_due_ms(&mesh_runtime)))) {
        deadline = mesh_relay_result_bundle_due_ms(&mesh_runtime);
    }
    if (deferred_pending && deadline == UINT32_MAX) {
        /* Retained custody has no relay deadline after a local ACK; poll the
         * promotion path on the normal retry cadence instead of cancelling. */
        deadline = now + REPORT_TX_RETRY_DELAY_MS;
    }
    delay_ms = uptime_deadline_reached(now, deadline) ?
               1u :
               uptime_ms_until_deadline(now, deadline);
    return mesh_reschedule_owned_work(&mesh_tx_timeout_work,
                                      delay_ms,
                                      "tx-timeout");
}

static bool mesh_defer_active_collection_result(const char *reason)
{
    const uint32_t now_ms = k_uptime_get_32();
    const struct app_mesh_collection_deferral_ops ops = {
        .save_outbox = mesh_collection_deferral_save_outbox,
        .schedule_retry = mesh_collection_deferral_schedule_retry,
    };
    struct app_mesh_collection_deferral_result result;

    if (!app_mesh_collection_defer_active_result(&mesh_runtime,
                                                now_ms,
                                                sys_rand32_get(),
                                                &ops,
                                                &result)) {
        return false;
    }

    if (!result.outbox_saved) {
        LOG_WRN("mesh pending collection result deferred without persisted outbox: ret=%d reason=%s",
                result.save_ret,
                reason == NULL ? "defer" : reason);
    }
    if (!result.retry_scheduled) {
        LOG_WRN("mesh pending collection result deferred without retry schedule: ret=%d reason=%s",
                result.schedule_ret,
                reason == NULL ? "defer" : reason);
    }
    LOG_INF("mesh pending collection result deferred: reason=%s",
            reason == NULL ? "defer" : reason);
    return true;
}

static bool mesh_send_failure_retryable(int ret)
{
    return ret == -EIO || ret == -EBUSY || ret == -EAGAIN;
}

static struct app_mesh_rf_retry_key mesh_rf_retry_packet_key(
    const struct proto_packet *packet,
    enum app_mesh_rf_retry_operation operation)
{
    return (struct app_mesh_rf_retry_key) {
        .source_id = packet->src_id,
        .destination_id = packet->dst_id,
        .session_id = packet->session_id,
        .sequence = packet->seq,
        .message_type = packet->msg_type,
        .operation = (uint8_t)operation,
    };
}

static struct app_mesh_rf_retry_state *mesh_route_request_rf_retry_state(
    enum app_mesh_route_request_rf_phase phase)
{
    return phase == APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX ?
           &mesh_route_request_control_rf_retry :
           &mesh_route_request_wake_rf_retry;
}

static struct app_mesh_rf_retry_key mesh_route_request_rf_retry_key(
    const struct mesh_outbound *route_request,
    uint64_t target_id,
    enum app_mesh_route_request_rf_phase phase)
{
    struct app_mesh_rf_retry_key key = mesh_rf_retry_packet_key(
        &route_request->packet,
        phase == APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX ?
            APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_CONTROL :
            APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE);

    /* The discovery session survives a pre-RF restore; the packet seq does not. */
    key.destination_id = target_id;
    key.sequence = 0u;
    return key;
}

static uint32_t mesh_rf_retry_next_delay_ms(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    const char *reason)
{
    uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
        state, key, policy, sys_rand32_get());

    if (delay_ms == 0u) {
        uint32_t random_value = sys_rand32_get();

        LOG_ERR("mesh RF retry identity rejected: reason=%s src=0x%016llx session=%u seq=%u op=%u",
                reason == NULL ? "rf-retry" : reason,
                (unsigned long long)(key == NULL ? 0u : key->source_id),
                key == NULL ? 0u : key->session_id,
                key == NULL ? 0u : key->sequence,
                key == NULL ? 0u : key->operation);
        delay_ms = policy == APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN ?
            app_wake_train_politeness_backoff_ms(0u, random_value) :
            25u + (random_value % 51u);
    }
    return delay_ms;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t mesh_rf_retry_bank_next_delay_ms(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    const char *reason)
{
    uint32_t delay_ms = app_mesh_rf_retry_bank_next_delay_ms(
        bank, key, policy, sys_rand32_get());

    if (delay_ms == 0u) {
        uint32_t random_value = sys_rand32_get();

        LOG_ERR("mesh RF retry bank rejected identity: reason=%s src=0x%016llx session=%u seq=%u op=%u",
                reason == NULL ? "rf-retry" : reason,
                (unsigned long long)(key == NULL ? 0u : key->source_id),
                key == NULL ? 0u : key->session_id,
                key == NULL ? 0u : key->sequence,
                key == NULL ? 0u : key->operation);
        delay_ms = policy == APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN ?
            app_wake_train_politeness_backoff_ms(0u, random_value) :
            25u + (random_value % 51u);
    }
    return delay_ms;
}
#endif

static __attribute__((unused)) const char *
route_delivery_action_name(enum route_delivery_action action)
{
    switch (action) {
    case ROUTE_DELIVERY_RETRY_CURRENT:
        return "retry-current";
    case ROUTE_DELIVERY_TRY_ALTERNATE:
        return "try-alternate";
    case ROUTE_DELIVERY_DISCOVER:
        return "discover";
    default:
        return "unknown";
    }
}

static void __attribute__((unused))
report_tx_set_retry_delay_override(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    report_tx_retry_delay_override_ms = delay_ms;
    report_tx_retry_delay_override_valid = true;
#else
    ARG_UNUSED(delay_ms);
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool report_tx_consume_retry_delay_override(uint32_t *delay_ms)
{
    if (!report_tx_retry_delay_override_valid || delay_ms == NULL) {
        return false;
    }
    *delay_ms = report_tx_retry_delay_override_ms;
    report_tx_retry_delay_override_valid = false;
    return true;
}
#endif

static bool mesh_route_reply_handoff_applies(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE != ROLE_GATEWAY;
}

static void mesh_route_reply_handoff_begin(void)
{
    if (!mesh_route_reply_handoff_applies()) {
        return;
    }

    mesh_route_reply_handoff_pending = true;
    mesh_route_reply_handoff_deadline_ms =
        k_uptime_get_32() + MESH_ROUTE_TEST_REPLY_HANDOFF_WAIT_MS;
    status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_WAIT until=%u\n",
                        mesh_route_reply_handoff_deadline_ms);
}

static void mesh_route_reply_handoff_clear(const char *reason)
{
    if (!mesh_route_reply_handoff_applies() ||
        !mesh_route_reply_handoff_pending) {
        return;
    }

    status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_CLEAR reason=%s\n",
                        reason == NULL ? "clear" : reason);
    mesh_route_reply_handoff_pending = false;
    mesh_route_reply_handoff_deadline_ms = 0u;
}

static bool mesh_route_reply_handoff_active(void)
{
    if (!mesh_route_reply_handoff_applies() ||
        !mesh_route_reply_handoff_pending) {
        return false;
    }
    if (uptime_deadline_reached(k_uptime_get_32(),
                                mesh_route_reply_handoff_deadline_ms)) {
        status_debug_note("DBG_ROUTE_REPLY_HANDOFF_EXPIRE\n");
        mesh_route_reply_handoff_pending = false;
        mesh_route_reply_handoff_deadline_ms = 0u;
        return false;
    }
    return true;
}

static void mesh_route_reply_handoff_after_capture(uint64_t target_id,
                                                   const char *reason)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_ROUTE_REPLY_HANDOFF\n");
        status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_STATE q=%u wait=%u relay=%u ch9pend=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                            mesh_ch9_tx_pending_is_active() ? 1u : 0u);
    }
    mesh_route_reply_handoff_begin();
    if (!mesh_ch9_tx_pending_is_active() &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
        (void)mesh_cancel_delayable(&mesh_route_waiting_work);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REPLY_CANCEL_RETRY\n");
        }
    }
    LOG_INF("mesh route reply captured; waiting for queued route processing: target=0x%016llx reason=%s",
            (unsigned long long)target_id,
            reason == NULL ? "route" : reason);
}

static int mesh_route_wake_encode_suffix(const struct mesh_outbound *route_req,
                                         uint8_t *out,
                                         size_t out_cap,
                                         size_t *written)
{
    size_t payload_len;
    size_t total_len;
    uint16_t crc;

    if (route_req == NULL || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        route_req->packet.msg_type != MSG_ROUTE_REQ ||
        route_req->packet.src_id != DEVICE_ID ||
        route_req->packet.dst_id != MESH_BROADCAST_ID ||
        route_req->next_hop_id != MESH_BROADCAST_ID ||
        route_req->payload_len != route_req->packet.payload_len) {
        return -EINVAL;
    }

    payload_len = route_req->payload_len;
    if (payload_len == 0u ||
        payload_len > MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES ||
        payload_len > UINT8_MAX) {
        return -EMSGSIZE;
    }

    total_len = MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + payload_len +
                MESH_ROUTE_WAKE_ROUTE_CRC_LEN;
    if (out_cap < total_len) {
        return -ENOSPC;
    }

    out[0] = MESH_ROUTE_WAKE_ROUTE_MAGIC0;
    out[1] = MESH_ROUTE_WAKE_ROUTE_MAGIC1;
    out[2] = MESH_ROUTE_WAKE_ROUTE_VERSION;
    out[3] = MSG_ROUTE_REQ;
    out[4] = route_req->packet.flags;
    out[5] = route_req->packet.ttl;
    proto_put_u32_le(&out[6], route_req->packet.session_id);
    proto_put_u16_le(&out[10], route_req->packet.seq);
    out[12] = (uint8_t)payload_len;
    memcpy(&out[MESH_ROUTE_WAKE_ROUTE_HEADER_LEN], route_req->payload, payload_len);

    crc = proto_crc16_ccitt_false(out, total_len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN);
    proto_put_u16_le(&out[total_len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN], crc);
    *written = total_len;
    return 0;
}

static int mesh_route_wake_decode_suffix(const uint8_t *data,
                                         size_t len,
                                         uint64_t source_id,
                                         struct proto_packet *packet,
                                         uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *payload_len)
{
    uint8_t declared_payload_len;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (data == NULL || packet == NULL || payload == NULL ||
        payload_len == NULL || !mesh_id_is_unicast(source_id)) {
        return -EINVAL;
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        len < MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + MESH_ROUTE_WAKE_ROUTE_CRC_LEN) {
        return -EINVAL;
    }
    if (data[0] != MESH_ROUTE_WAKE_ROUTE_MAGIC0 ||
        data[1] != MESH_ROUTE_WAKE_ROUTE_MAGIC1 ||
        data[2] != MESH_ROUTE_WAKE_ROUTE_VERSION ||
        data[3] != MSG_ROUTE_REQ) {
        return -EBADMSG;
    }

    declared_payload_len = data[12];
    if (declared_payload_len == 0u ||
        declared_payload_len > MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES ||
        declared_payload_len > payload_cap ||
        len != MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + declared_payload_len +
               MESH_ROUTE_WAKE_ROUTE_CRC_LEN) {
        return -EMSGSIZE;
    }

    expected_crc = proto_get_u16_le(&data[len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN]);
    actual_crc = proto_crc16_ccitt_false(data, len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN);
    if (expected_crc != actual_crc) {
        return -EBADMSG;
    }

    memset(packet, 0, sizeof(*packet));
    packet->msg_type = MSG_ROUTE_REQ;
    packet->flags = data[4];
    packet->src_id = source_id;
    packet->dst_id = MESH_BROADCAST_ID;
    packet->session_id = proto_get_u32_le(&data[6]);
    packet->seq = proto_get_u16_le(&data[10]);
    packet->ttl = data[5];
    packet->payload_len = declared_payload_len;
    memcpy(payload, &data[MESH_ROUTE_WAKE_ROUTE_HEADER_LEN], declared_payload_len);
    *payload_len = declared_payload_len;
    return 0;
}

static bool mesh_route_embedded_duplicate(const struct proto_packet *packet,
                                          uint32_t now_ms)
{
    uint32_t suppress_deadline_ms;

    if (!mesh_route_embedded_rx.valid || packet == NULL) {
        return false;
    }

    suppress_deadline_ms = mesh_route_embedded_rx.received_at_ms +
                           MESH_ROUTE_TEST_EMBEDDED_ROUTE_SUPPRESS_MS;
    if (uptime_deadline_reached(now_ms, suppress_deadline_ms)) {
        mesh_route_embedded_rx.valid = false;
        return false;
    }

    return mesh_route_embedded_rx.packet.msg_type == packet->msg_type &&
           mesh_route_embedded_rx.packet.src_id == packet->src_id &&
           mesh_route_embedded_rx.packet.dst_id == packet->dst_id &&
           mesh_route_embedded_rx.packet.session_id == packet->session_id &&
           mesh_route_embedded_rx.packet.seq == packet->seq;
}

static bool mesh_queue_embedded_route_request(
    const struct uwb_wake_claim_frame *claim,
    const uint8_t *suffix,
    size_t suffix_len,
    uint8_t link_quality)
{
    struct mesh_rx_pending pending = {0};
    size_t payload_len = 0u;
    uint32_t now_ms;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        claim == NULL || suffix == NULL || suffix_len == 0u ||
        claim->clicker_id == DEVICE_ID) {
        return false;
    }

    ret = mesh_route_wake_decode_suffix(suffix,
                                        suffix_len,
                                        claim->clicker_id,
                                        &pending.packet,
                                        pending.payload,
                                        sizeof(pending.payload),
                                        &payload_len);
    if (ret < 0) {
        return false;
    }

    now_ms = k_uptime_get_32();
    if (mesh_route_embedded_duplicate(&pending.packet, now_ms)) {
        status_debug_note("DBG_EMBEDDED_ROUTE_REQ_DUP\n");
        return true;
    }

    pending.payload_len = (uint16_t)payload_len;
    pending.previous_hop_id = claim->clicker_id;
    pending.link_quality = link_quality;
    pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    pending.first_received_at_ms = mesh_expand_uptime32(now_ms);
    pending.received_at_ms = now_ms;
    pending.received_at_valid = true;
    app_mesh_test_note_wake_event(&pending.packet,
                                  pending.previous_hop_id,
                                  pending.link_quality,
                                  pending.radio_channel);

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("embedded route request RX queue full; dropped src=0x%016llx seq=%u ret=%d",
                (unsigned long long)pending.packet.src_id,
                pending.packet.seq,
                ret);
        return false;
    }

    mesh_route_embedded_rx.valid = true;
    mesh_route_embedded_rx.packet = pending.packet;
    mesh_route_embedded_rx.received_at_ms = now_ms;
    mesh_route_embedded_reply_peer_id = claim->clicker_id;
    mesh_route_embedded_reply_hold_until_ms =
        now_ms + claim->wake_train_ends_in_ms + MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS;

    status_debug_note("DBG_EMBEDDED_ROUTE_REQ_RX\n");
    status_debug_printf("DBG_EMBEDDED_ROUTE_REQ src=0x%llx seq=%u hold_until=%u\n",
                        (unsigned long long)pending.packet.src_id,
                        pending.packet.seq,
                        mesh_route_embedded_reply_hold_until_ms);
    (void)mesh_submit_owned_work(&mesh_rx_work,
                                 "embedded-route-rx");
    return true;
}

static void mesh_route_embedded_wait_before_reply(const struct mesh_outbound *route_reply)
{
    uint32_t now_ms;
    uint32_t delay_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        route_reply == NULL ||
        route_reply->packet.msg_type != MSG_ROUTE_REPLY ||
        mesh_route_embedded_reply_peer_id == 0u ||
        route_reply->next_hop_id != mesh_route_embedded_reply_peer_id) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (!uptime_deadline_reached(
            now_ms, mesh_route_embedded_reply_hold_until_ms)) {
        delay_ms = uptime_ms_until_deadline(
            now_ms, mesh_route_embedded_reply_hold_until_ms);
        status_debug_printf("DBG_EMBEDDED_ROUTE_REPLY_HOLD delay=%u peer=0x%llx\n",
                            delay_ms,
                            (unsigned long long)mesh_route_embedded_reply_peer_id);
        k_msleep(delay_ms);
    }

    mesh_route_embedded_reply_peer_id = 0u;
    mesh_route_embedded_reply_hold_until_ms = 0u;
}

static void mesh_schedule_route_waiting_retry_after(const char *reason, uint32_t delay_ms)
{
    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    (void)mesh_reschedule_owned_work(&mesh_route_waiting_work,
                                     delay_ms,
                                     "route-wait");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_RETRY reason=%s delay=%u msg=0x%02x dst=0x%llx seq=%u active=%u\n",
                            reason == NULL ? "route-wait" : reason,
                            delay_ms,
                            mesh_route_waiting_tx.packet.msg_type,
                            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                            mesh_route_waiting_tx.packet.seq,
                            mesh_runtime.route_discovery.active ? 1u : 0u);
    }
    LOG_INF("mesh route waiting: reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u retry_in_ms=%u",
            reason == NULL ? "route-wait" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            mesh_runtime.route_discovery.attempts,
            delay_ms);
}

static void mesh_schedule_route_waiting_retry(const char *reason)
{
    uint32_t now;
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }
    if (mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
        return;
    }

    now = k_uptime_get_32();
    deadline = mesh_runtime.route_discovery.active ?
               mesh_runtime.route_discovery.next_request_ms :
               now + REPORT_TX_RETRY_DELAY_MS;
    delay_ms = uptime_deadline_reached(now, deadline) ?
               1u :
               uptime_ms_until_deadline(now, deadline);
    mesh_schedule_route_waiting_retry_after(reason, delay_ms);
}

static uint32_t mesh_route_exhausted_retry_delay_ms(uint32_t random_value)
{
    return MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS +
           (random_value % (MESH_ROUTE_EXHAUSTED_RETRY_JITTER_MS + 1u));
}

static void mesh_schedule_route_waiting_exhausted_retry(const char *reason)
{
    uint32_t now_ms;
    uint32_t random_value;
    uint32_t delay_ms;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    mesh_relay_reset_route_discovery(&mesh_runtime);
    now_ms = k_uptime_get_32();
    random_value = sys_rand32_get();
    delay_ms = mesh_route_exhausted_retry_delay_ms(random_value);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_EXHAUSTED_RETRY reason=%s base=%u jitter=%u rand=%u delay=%u next=%u msg=0x%02x dst=0x%llx seq=%u lost=%u\n",
                            reason == NULL ? "route-exhausted" : reason,
                            MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS,
                            MESH_ROUTE_EXHAUSTED_RETRY_JITTER_MS,
                            random_value,
                            delay_ms,
                            now_ms + delay_ms,
                            mesh_route_waiting_tx.packet.msg_type,
                            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                            mesh_route_waiting_tx.packet.seq,
                            fw_delivery_loss_count(
                                &mesh_delivery_loss));
    }
    LOG_WRN("mesh route discovery exhausted; retained packet will retry: reason=%s msg=0x%02x dst=0x%016llx seq=%u retry_in_ms=%u",
            reason == NULL ? "route-exhausted" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            delay_ms);
    mesh_schedule_route_waiting_retry_after(reason == NULL ?
                                            "route-exhausted" : reason,
                                            delay_ms);
}

void mesh_report_resume_restored_outbox(const char *reason)
{
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        int deferred_ret = mesh_deferred_outbox_pending ? 1 : 0;

        if (deferred_ret >= 0) {
            mesh_deferred_outbox_pending = deferred_ret > 0;
        } else if (deferred_ret != -ENOTSUP) {
            mesh_deferred_outbox_pending = true;
        }
    }
    if (!mesh_relay_tx_active(&mesh_runtime) &&
        !mesh_deferred_outbox_pending) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_OUTBOX_RESTORE_RESUME reason=%s state=%u msg=0x%02x dst=0x%llx seq=%u retry=%u ack=%u now=%u\n",
                            reason == NULL ? "restore" : reason,
                            (unsigned int)mesh_runtime.pending.state,
                            mesh_runtime.pending.packet.msg_type,
                            (unsigned long long)mesh_runtime.pending.packet.dst_id,
                            mesh_runtime.pending.packet.seq,
                            mesh_runtime.pending.retry_after_ms,
                            mesh_runtime.pending.gateway_ack_deadline_ms,
                            k_uptime_get_32());
    }
    mesh_schedule_tx_timeout();
}

static const char *mesh_tx_handoff_reason_name(
    enum app_mesh_tx_handoff_reason reason)
{
    switch (reason) {
    case APP_MESH_TX_HANDOFF_REASON_RX_CONTROL:
        return "rx-control-handoff";
    case APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY:
        return "route-reply-handoff";
    case APP_MESH_TX_HANDOFF_REASON_NONE:
    default:
        return "handoff";
    }
}

static int mesh_tx_handoff_schedule_retry(
    enum app_mesh_tx_handoff_work work,
    enum app_mesh_tx_handoff_reason reason,
    uint32_t delay_ms,
    void *ctx)
{
    ARG_UNUSED(ctx);

    switch (work) {
    case APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING:
        mesh_schedule_route_waiting_retry_after(
            mesh_tx_handoff_reason_name(reason), delay_ms);
        return 0;
    case APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE:
        report_tx_schedule(delay_ms);
        return 0;
    default:
        return -EINVAL;
    }
}

static bool mesh_tx_handoff_gate_yields(
    enum app_mesh_tx_handoff_work work,
    bool queued_gateway_tx_pending,
    bool route_reply_handoff_active,
    bool rx_control_handoff_active,
    struct app_mesh_tx_handoff_result *result)
{
    const struct app_mesh_tx_handoff_ops ops = {
        .schedule_retry = mesh_tx_handoff_schedule_retry,
    };
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = queued_gateway_tx_pending,
        .route_reply_handoff_active = route_reply_handoff_active,
        .rx_control_handoff_active = rx_control_handoff_active,
        .work = work,
        .retry_delay_ms = MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS,
    };

    return app_mesh_tx_handoff_gate_yield(&state, &ops, result);
}

static bool mesh_event_plan_debugs_channel5(enum mesh_event_plan_action action)
{
    return action == MESH_EVENT_PLAN_CLIP ||
           action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
           action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD ||
           action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5;
}

static bool mesh_debug_channel9_state(uint64_t peer_id,
                                      uint32_t *event_counter,
                                      bool *local_tx_slot)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return false;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (!entry->valid || entry->next_hop_id != peer_id) {
            continue;
        }
        if (event_counter != NULL) {
            *event_counter = entry->timing.event_counter;
        }
        if (local_tx_slot != NULL) {
            *local_tx_slot = mesh_event_timing_local_tx_slot(&entry->timing);
        }
        return true;
    }
    return false;
}

static void mesh_debug_channel5_preemption(const char *context,
                                           const char *reason,
                                           uint64_t peer_id,
                                           const struct mesh_channel5_requirements *requirements,
                                           const struct mesh_event_plan *plan,
                                           uint32_t now_ms)
{
    ARG_UNUSED(context);
    ARG_UNUSED(reason);
    ARG_UNUSED(peer_id);

    if (requirements == NULL || plan == NULL ||
        !mesh_event_plan_debugs_channel5(plan->action)) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH5_PREEMPT act=%u now=%u ch5=%u start=%u win=%u\n",
                            (unsigned int)plan->action,
                            now_ms,
                            requirements->next_required_scan_start_ms,
                            plan->start_ms,
                            plan->window_ms);
    }
}

static void mesh_debug_channel9_unavailable(const char *context,
                                            const char *reason,
                                            const struct proto_packet *packet,
                                            int ret,
                                            int select_ret,
                                            uint64_t selected_next_hop,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms)
{
    ARG_UNUSED(context);
    ARG_UNUSED(reason);
    ARG_UNUSED(selected_next_hop);

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || packet == NULL || plan == NULL) {
        return;
    }

    status_debug_printf("DBG_CH9_UNAVAIL ret=%d sel=%d act=%u now=%u start=%u win=%u\n",
                        ret,
                        select_ret,
                        (unsigned int)plan->action,
                        now_ms,
                        plan->start_ms,
                        plan->window_ms);

    status_debug_printf("DBG_CH9_ROUTE_STATE epoch=%u selected=%u\n",
                        mesh_runtime.upstream.current_epoch,
                        mesh_runtime.upstream.selected_index);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate =
            &mesh_runtime.upstream.candidates[i];

        if (!candidate->valid) {
            continue;
        }
        status_debug_printf("DBG_CH9_ROUTE_CAND i=%u epoch=%u next=0x%llx hop=%u timing=%u fail=%u hold=%u\n",
                            i,
                            candidate->route_epoch,
                            (unsigned long long)candidate->next_hop_id,
                            candidate->hop_count,
                            candidate->channel9_timing_valid ? 1u : 0u,
                            candidate->failure_count,
                            candidate->hold_down_until_ms);
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (!entry->valid) {
            continue;
        }
        status_debug_printf("DBG_CH9_ENTRY i=%u usable=%u cnt=%u next=%u last=%u tx=%u\n",
                            i,
                            mesh_event_timing_usable(&entry->timing, now_ms) ? 1u : 0u,
                            entry->timing.event_counter,
                            entry->timing.next_event_time_ms,
                            entry->timing.last_successful_ch9_event_ms,
                            mesh_event_timing_local_tx_slot(&entry->timing) ? 1u : 0u);
    }
}

static bool mesh_clicker_on_demand_rx_required(void)
{
    const struct route_candidate *parent;
    struct mesh_event_owner *owner;
    struct mesh_event_timing timing;
    bool exact_ack_wait;

    if (DEVICE_ROLE != ROLE_CLICKER) {
        return false;
    }

    exact_ack_wait =
        mesh_ch9_tx_pending_is_active() ||
        app_mesh_ch9_core_ack_wait_active(
            &mesh_runtime.pending,
            mesh_relay_tx_active(&mesh_runtime));
    if (exact_ack_wait) {
        return true;
    }

    parent = route_selected(&mesh_runtime.upstream);
    if (parent == NULL || !parent->channel9_timing_valid) {
        return false;
    }
    owner = mesh_event_owner_registry_find(&mesh_event_owner_registry,
                                           parent->next_hop_id);
    if (owner == NULL || !owner->active) {
        return false;
    }
    return mesh_find_active_channel9_timing(parent->next_hop_id,
                                            k_uptime_get_32(),
                                            &timing);
}

static bool mesh_role_uses_uwb_rx(void)
{
    if (mesh_clicker_on_demand_rx_required()) {
        return true;
    }
    return app_mesh_rx_policy_role_uses_uwb_rx(
        DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY,
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
        mesh_channel9_connection_count() > 0u);
}

static bool mesh_transport_paused(void)
{
    return atomic_get(&mesh_transport_paused_state) != 0;
}

static int mesh_transport_radio_start(const char *owner)
{
    int ret;

    if (mesh_transport_paused()) {
        return -ESHUTDOWN;
    }
    ret = radio_guard_uwb_start(owner);
    if (ret == 0 && mesh_transport_paused()) {
        /* Pause won the admission race before any PHY operation began. */
        radio_guard_uwb_stop();
        return -ESHUTDOWN;
    }
    return ret;
}

static bool mesh_rx_handoff_scan_rearm_allowed(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool allowed = app_mesh_rx_handoff_scan_rearm_allowed(&mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return allowed;
}

int mesh_node_comm_gateway_delivery_due_begin(bool *wait_for_scan_boundary)
{
    k_spinlock_key_t key;
    bool abort_scan = false;
    bool accepted;

    if (wait_for_scan_boundary == NULL) {
        return -EINVAL;
    }
    *wait_for_scan_boundary = false;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }

    key = k_spin_lock(&mesh_rx_handoff_lock);
    accepted = app_mesh_rx_handoff_request_scheduled_control(
        &mesh_rx_handoff, &abort_scan);
    if (accepted && abort_scan) {
        /*
         * Request abort only while this lock proves a scan owns the radio.
         * The pending gate prevents rearm; this path never clears the shared
         * abort bit, so it cannot consume a concurrent host-command request.
         */
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    }
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    if (!accepted) {
        return -EINVAL;
    }
    *wait_for_scan_boundary = abort_scan;
    return 0;
}

bool mesh_node_comm_gateway_delivery_due_pending(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool pending = app_mesh_rx_handoff_scheduled_control_pending(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return pending;
}

bool mesh_node_comm_gateway_delivery_due_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool ready = app_mesh_rx_handoff_scheduled_control_ready(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return ready;
}

bool mesh_node_comm_gateway_delivery_due_end(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool was_pending = app_mesh_rx_handoff_end_scheduled_control(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return was_pending;
}

static bool mesh_rx_handoff_begin_control(bool *abort_scan)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool started = app_mesh_rx_handoff_begin_control(&mesh_rx_handoff,
                                                      abort_scan);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return started;
}

static bool mesh_rx_handoff_control_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool ready = app_mesh_rx_handoff_control_ready(&mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return ready;
}

static void mesh_rx_handoff_end_control(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);

    app_mesh_rx_handoff_end_control(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
}

static int mesh_rx_radio_start(const char *owner)
{
    k_spinlock_key_t key;
    bool accepted;
    int ret;

    if (!mesh_rx_handoff_scan_rearm_allowed()) {
        return -ECANCELED;
    }
    ret = mesh_transport_radio_start(owner);
    if (ret < 0) {
        return ret;
    }

    key = k_spin_lock(&mesh_rx_handoff_lock);
    accepted = app_mesh_rx_handoff_try_begin_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    if (!accepted) {
        radio_guard_uwb_stop();
        return -ECANCELED;
    }
    return 0;
}

static void mesh_rx_radio_stop(void)
{
    k_spinlock_key_t key;

    radio_guard_uwb_stop();
    key = k_spin_lock(&mesh_rx_handoff_lock);
    app_mesh_rx_handoff_end_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
}

static int mesh_rx_handoff_wait_for_control(void)
{
    uint32_t deadline_ms = k_uptime_get_32() +
                           MESH_CONTROL_RX_HANDOFF_TIMEOUT_MS;

    while (!mesh_rx_handoff_control_ready() || radio_guard_uwb_busy()) {
        if (uptime_deadline_reached(k_uptime_get_32(), deadline_ms)) {
            return -EBUSY;
        }
        k_msleep(1);
    }
    return 0;
}

int mesh_route_owner_work_reschedule_timeout(struct k_work_delayable *work,
                                             k_timeout_t delay)
{
    if (work == NULL) {
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    return k_work_reschedule_for_queue(&mesh_route_work_q, work, delay);
#else
    /*
     * Legacy and ML presets retain the system workqueue. Production mesh
     * presets enable the dedicated communication queue explicitly.
     */
    return k_work_reschedule(work, delay);
#endif
}

int mesh_route_owner_work_reschedule(struct k_work_delayable *work,
                                     uint32_t delay_ms)
{
    return mesh_route_owner_work_reschedule_timeout(work,
                                                    K_MSEC(delay_ms));
}

static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms)
{
    if (mesh_transport_paused() && work != &mesh_persistence_retry_work) {
        return -ESHUTDOWN;
    }
    return mesh_route_owner_work_reschedule(work, delay_ms);
}

int mesh_route_work_reschedule(struct k_work_delayable *work, uint32_t delay_ms)
{
    if (work == NULL) {
        return -EINVAL;
    }

    return mesh_reschedule_delayable(work, delay_ms);
}

static int mesh_owned_schedule_result(int ret,
                                      const char *owner,
                                      bool pause_owned_rejection)
{
    enum app_mesh_schedule_recovery recovery =
        app_mesh_schedule_recovery_decide(ret, pause_owned_rejection);

    if (recovery == APP_MESH_SCHEDULE_RECOVERY_WATCHDOG) {
        LOG_ERR("mesh accepted state lost its work owner: owner=%s ret=%d",
                owner == NULL ? "unknown" : owner,
                ret);
        app_watchdog_stop_feeding();
    } else if (recovery == APP_MESH_SCHEDULE_RECOVERY_ON_RESUME &&
               IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_WORK_DEFER owner=%s ret=%d\n",
                            owner == NULL ? "unknown" : owner,
                            ret);
    }
    return ret;
}

static int mesh_reschedule_owned_work(struct k_work_delayable *work,
                                      uint32_t delay_ms,
                                      const char *owner)
{
    if (mesh_transport_paused()) {
        return mesh_owned_schedule_result(-ESHUTDOWN, owner, true);
    }
    return mesh_owned_schedule_result(
        mesh_route_owner_work_reschedule(work, delay_ms), owner, false);
}

static int mesh_submit_owned_work(struct k_work *work, const char *owner)
{
    int ret;

    if (mesh_transport_paused()) {
        return mesh_owned_schedule_result(-ESHUTDOWN, owner, true);
    }
#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    ret = k_work_submit_to_queue(&mesh_route_work_q, work);
#else
    ret = k_work_submit(work);
#endif
    return mesh_owned_schedule_result(ret, owner, false);
}

int mesh_schedule_route_request(uint64_t target_id, const char *reason)
{
    const struct app_mesh_async_route_transfer_identity transfer = {
        .target_id = mesh_route_waiting_tx_valid ?
                     mesh_route_waiting_tx.packet.dst_id : 0u,
        .owner_generation = mesh_route_waiting_tx_valid ?
                            mesh_route_waiting_tx.packet.session_id : 0u,
        .packet_seq = mesh_route_waiting_tx_valid ?
                      mesh_route_waiting_tx.packet.seq : 0u,
        .msg_type = mesh_route_waiting_tx_valid ?
                    mesh_route_waiting_tx.packet.msg_type : 0u,
        .valid = mesh_route_waiting_tx_valid &&
                 mesh_route_waiting_tx.packet.dst_id == target_id,
    };
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    if (!app_mesh_async_route_request_submit(
            &mesh_route_discovery_request,
            target_id,
            reason,
            k_uptime_get_32(),
            &transfer)) {
        k_mutex_unlock(&mesh_route_discovery_lock);
        return -EINVAL;
    }
    ret = mesh_reschedule_owned_work(&mesh_route_discovery_work,
                                     0u,
                                     "route-discovery");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_SCHED target=0x%llx generation=%u ret=%d reason=%s\n",
                            (unsigned long long)target_id,
                            mesh_route_discovery_request.generation,
                            ret,
                            mesh_route_discovery_request.reason);
    }
    k_mutex_unlock(&mesh_route_discovery_lock);
    return ret;
}

static uint32_t mesh_route_discovery_async_retry_delay_ms(int ret)
{
    uint32_t now_ms = k_uptime_get_32();

    if (ret == -ETIMEDOUT) {
        return mesh_route_exhausted_retry_delay_ms(sys_rand32_get());
    }
    if (ret == -EBUSY) {
        return MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
    }
    if (mesh_runtime.route_discovery.active &&
        !uptime_deadline_reached(
            now_ms, mesh_runtime.route_discovery.next_request_ms)) {
        return uptime_ms_until_deadline(
            now_ms, mesh_runtime.route_discovery.next_request_ms);
    }
    return REPORT_TX_RETRY_DELAY_MS;
}

static void mesh_route_discovery_work_handler(struct k_work *work)
{
    struct app_mesh_async_route_attempt attempt;
    uint32_t retry_delay_ms = 0u;
    bool terminal_transferred = false;
    bool retry_owned = false;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    if (!app_mesh_async_route_request_snapshot(
            &mesh_route_discovery_request, &attempt)) {
        k_mutex_unlock(&mesh_route_discovery_lock);
        return;
    }
    k_mutex_unlock(&mesh_route_discovery_lock);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_RUN target=0x%llx generation=%u reason=%s\n",
                            (unsigned long long)attempt.target_id,
                            attempt.generation,
                            attempt.reason);
    }
    ret = mesh_request_route(attempt.target_id, attempt.reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_DONE target=0x%llx generation=%u ret=%d reason=%s\n",
                            (unsigned long long)attempt.target_id,
                            attempt.generation,
                            ret,
                            attempt.reason);
    }
    if (ret == -ETIMEDOUT &&
        app_mesh_async_route_request_transfer_matches(
            &attempt,
            mesh_route_waiting_tx_valid,
            mesh_route_waiting_tx_valid ?
                mesh_route_waiting_tx.packet.dst_id : 0u,
            mesh_route_waiting_tx_valid ?
                mesh_route_waiting_tx.packet.session_id : 0u,
            mesh_route_waiting_tx_valid ?
                mesh_route_waiting_tx.packet.seq : 0u,
            mesh_route_waiting_tx_valid ?
                mesh_route_waiting_tx.packet.msg_type : 0u) &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_route_waiting_exhausted_retry(
            "async-route-exhausted");
        terminal_transferred = true;
    } else if (ret < 0) {
        retry_delay_ms = mesh_route_discovery_async_retry_delay_ms(ret);
    }

    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    if (ret == 0 || terminal_transferred) {
        (void)app_mesh_async_route_request_complete(
            &mesh_route_discovery_request, &attempt);
    } else if (app_mesh_async_route_request_defer(
                   &mesh_route_discovery_request,
                   &attempt,
                   k_uptime_get_32(),
                   retry_delay_ms)) {
        retry_owned = true;
        (void)mesh_reschedule_owned_work(
            &mesh_route_discovery_work,
            retry_delay_ms,
            "route-discovery-retry");
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret < 0) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_RETAIN target=0x%llx generation=%u ret=%d retry=%u delay=%u transferred=%u\n",
                            (unsigned long long)attempt.target_id,
                            attempt.generation,
                            ret,
                            retry_owned ? 1u : 0u,
                            retry_delay_ms,
                            terminal_transferred ? 1u : 0u);
    }
    k_mutex_unlock(&mesh_route_discovery_lock);
}

static int mesh_cancel_delayable(struct k_work_delayable *work)
{
    return k_work_cancel_delayable(work);
}
