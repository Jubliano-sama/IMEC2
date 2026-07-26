#line 4287 __BASE_FILE__
#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_discovery_assignment_new_epoch(void)
{
    uint32_t epoch = sys_rand32_get() ^ k_uptime_get_32();

    return epoch == 0u ? 1u : epoch;
}

static uint32_t gateway_discovery_assignment_next_generation(void)
{
    gateway_discovery_assignment_generation++;
    if (gateway_discovery_assignment_generation == 0u) {
        gateway_discovery_assignment_generation = 1u;
    }
    return gateway_discovery_assignment_generation;
}

static int gateway_build_discovery_assignment_command(
    struct mesh_outbound *outbound,
    enum discovery_assignment_phase phase,
    uint32_t epoch,
    uint32_t command_seq)
{
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || epoch == 0u || command_seq == 0u) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = tlv_append_u16(outbound->payload,
                         sizeof(outbound->payload),
                         &payload_len,
                         TLV_COMMAND_ID,
                         CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_SEQ,
                             command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_EXPIRY_S,
                             DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(outbound->payload,
                                                       sizeof(outbound->payload),
                                                       &payload_len,
                                                       phase,
                                                       epoch);
    }
    if (ret == PROTO_OK) {
        const struct operation_policy policy = {
            .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
            .value.assignment = {
                .expected_anchor_count =
                    gateway_discovery_assignment_state.expected_claim_count,
                .operation_budget_ms =
                    gateway_discovery_assignment_state.command_budget_ms,
                .response_spread_ms =
                    gateway_discovery_assignment_state.response_spread_ms,
            },
        };

        ret = operation_policy_append_tlv(outbound->payload,
                                          sizeof(outbound->payload),
                                          &payload_len,
                                          &policy);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    outbound->packet.msg_type = MSG_COMMAND;
    outbound->packet.flags = FLAG_DIAGNOSTIC;
    outbound->packet.src_id = DEVICE_ID;
    outbound->packet.dst_id = MESH_BROADCAST_ID;
    outbound->packet.session_id = command_seq;
    outbound->packet.seq = gateway_next_command_seq();
    outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = gateway_command_append_default_flood_controls(outbound);
    return ret == PROTO_OK ? 0 : mesh_errno_from_proto(ret);
}

static void gateway_discovery_assignment_abandon_active_delivery_locked(void)
{
    uint32_t handle = gateway_discovery_assignment_state.delivery_handle;

    gateway_discovery_assignment_state.delivery_handle = 0u;
    gateway_discovery_assignment_state.delivery_kind =
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE;
    if (handle != 0u && app_node_comm_abandon_delivery(handle) < 0) {
        LOG_WRN("discovery assignment delivery cleanup failed: handle=%u",
                handle);
    }
}

static uint8_t gateway_discovery_assignment_missing_ack_count_locked(void);
static void gateway_discovery_assignment_fail_locked(
    enum command_status status,
    uint8_t reason);

static bool gateway_discovery_assignment_rf_started_locked(
    enum gateway_discovery_assignment_delivery_kind kind)
{
    uint8_t attempts_started = 0u;

    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM &&
        gateway_discovery_assignment_state.claim_delivery_succeeded) {
        return true;
    }
    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE &&
        gateway_discovery_assignment_state.table_delivery_succeeded) {
        return true;
    }
    return gateway_discovery_assignment_state.delivery_kind == kind &&
           gateway_discovery_assignment_state.delivery_handle != 0u &&
           app_node_comm_delivery_attempts_started(
               gateway_discovery_assignment_state.delivery_handle,
               &attempts_started) == 0 &&
           attempts_started > 0u;
}

static void gateway_discovery_assignment_complete_success_locked(void)
{
    struct discovery_assignment_entry
        committed_entries[UWB_DISCOVERY_SLOT_COUNT];
    uint64_t committed_anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    struct gateway_command_event table_event;
    struct gateway_command_event event;
    size_t committed_count = 0u;
    uint8_t failure_count;
    uint16_t membership_epoch;
    int membership_ret;
    int publish_ret;

    if (!gateway_discovery_assignment_state.active ||
        gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS ||
        !gateway_discovery_assignment_state.table_delivery_succeeded ||
        (gateway_discovery_assignment_state.response_ack_settle_armed &&
         discovery_assignment_response_ack_settle_pending(
             (uint64_t)k_uptime_get(),
             gateway_discovery_assignment_state.response_ack_settle_deadline_ms))) {
        return;
    }
    failure_count = gateway_discovery_assignment_missing_ack_count_locked();
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        if ((gateway_discovery_assignment_state.ack_mask &
             (UINT64_C(1) << i)) == 0u) {
            continue;
        }
        committed_anchor_ids[committed_count] =
            gateway_discovery_assignment_state.anchor_ids[i];
        committed_entries[committed_count] =
            (struct discovery_assignment_entry) {
                .anchor_id = gateway_discovery_assignment_state.anchor_ids[i],
                .hash = discovery_assignment_hash(
                    gateway_discovery_assignment_state.anchor_ids[i]),
                .slot = (uint8_t)i,
            };
        committed_count++;
    }
    if (committed_count == 0u) {
        gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT,
                                                 failure_count);
        return;
    }
    membership_epoch = discovery_assignment_membership_epoch(
        gateway_discovery_assignment_state.epoch);
    membership_ret = gateway_set_registered_membership_roster(
        membership_epoch,
        committed_anchor_ids,
        committed_count);
    if (membership_ret < 0) {
        LOG_ERR("discovery assignment membership persistence failed: assignment_epoch=%u membership_epoch=%u ret=%d",
                gateway_discovery_assignment_state.epoch,
                membership_epoch,
                membership_ret);
        gateway_discovery_assignment_fail_locked(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-membership_ret));
        return;
    }
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    publish_ret = app_gateway_assignment_publisher_stage_batch(
        &event,
        committed_entries,
        committed_count,
        gateway_discovery_assignment_state.duplicate_count);
    if (publish_ret < 0) {
        LOG_ERR("discovery assignment final telemetry staging failed: epoch=%u ret=%d",
                gateway_discovery_assignment_state.epoch,
                publish_ret);
        gateway_discovery_assignment_fail_locked(COMMAND_INTERNAL_ERROR,
                                                 (uint8_t)(-publish_ret));
        return;
    }
    table_event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    table_event.attempt = gateway_discovery_assignment_state.table_round;
    table_event.progress_count =
        (uint16_t)committed_count;
    table_event.total_count =
        (uint16_t)gateway_discovery_assignment_state.claim_count;
    table_event.success_count = (uint16_t)committed_count;
    table_event.failure_count = failure_count;
    table_event.status = COMMAND_OK;
    table_event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    table_event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    app_gateway_assignment_publisher_stage_table_ready(&table_event);
    gateway_emit_host_command_result(
        &gateway_discovery_assignment_state.host_command,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        COMMAND_OK,
        (uint8_t)committed_count);
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    event.progress_count =
        (uint16_t)committed_count;
    event.total_count =
        (uint16_t)gateway_discovery_assignment_state.claim_count;
    event.success_count = (uint16_t)committed_count;
    event.failure_count = failure_count;
    event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    (void)gateway_observe_command_event(&event, true);
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    (void)k_work_cancel_delayable(
        &gateway_discovery_assignment_finalize_work);
}

static int gateway_discovery_assignment_submit_control_flood_locked(
    const struct mesh_outbound *outbound,
    enum gateway_discovery_assignment_delivery_kind kind,
    const char *reason)
{
    uint32_t remaining_ms;
    uint32_t handle;
    uint64_t absolute_deadline_ms;
    int ret;

    if (outbound == NULL || reason == NULL ||
        kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE) {
        return -EINVAL;
    }
    if (gateway_discovery_assignment_state.delivery_handle != 0u) {
        return -EBUSY;
    }
    {
        uint64_t now_ms = (uint64_t)k_uptime_get();
        uint64_t remaining_64 =
            gateway_discovery_assignment_state.operation_deadline_ms > now_ms ?
            gateway_discovery_assignment_state.operation_deadline_ms - now_ms :
            0u;

        remaining_ms = remaining_64 > UINT32_MAX ?
                       UINT32_MAX : (uint32_t)remaining_64;
    }
    if (remaining_ms == 0u) {
        return -ETIMEDOUT;
    }
    absolute_deadline_ms = discovery_assignment_control_flood_deadline_ms(
        (uint64_t)k_uptime_get(),
        gateway_discovery_assignment_state.operation_deadline_ms);
    ret = app_node_comm_submit_delivery(
        outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        absolute_deadline_ms,
        ((uint32_t)CMD_ASSIGN_DISCOVERY_SLOTS << 16) ^ outbound->packet.seq,
        &handle);
    if (ret < 0) {
        return ret;
    }
    gateway_discovery_assignment_state.delivery_handle = handle;
    gateway_discovery_assignment_state.delivery_kind = kind;
    status_debug_printf("DBG_DISCOVERY_SLOT_FLOOD_SUBMIT kind=%u handle=%u seq=%u deadline=%llu reason=%s\n",
                        (unsigned int)kind,
                        handle,
                        outbound->packet.seq,
                        (unsigned long long)absolute_deadline_ms,
                        reason);
    return 0;
}

static int gateway_send_discovery_assignment_claim_request_locked(void)
{
    struct mesh_outbound outbound;
    int ret;

    ret = gateway_build_discovery_assignment_command(
        &outbound,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.claim_command_seq);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_discovery_assignment_submit_control_flood_locked(
        &outbound,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM,
        "discovery-slot-claim-request");
    status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REQUEST epoch=%u round=%u command_seq=%u queued=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.claim_round + 1u,
                        gateway_discovery_assignment_state.claim_command_seq,
                        ret == 0 ? 1u : 0u,
                        ret);
    return ret;
}

static uint8_t gateway_discovery_assignment_table_round_limit_locked(void);

static uint32_t gateway_discovery_assignment_window_ms_locked(void)
{
    uint8_t collection_hop_count =
        app_discovery_assignment_collection_hop_count(
            gateway_discovery_assignment_state.expected_claim_count,
            gateway_discovery_assignment_state.claim_count,
            gateway_discovery_assignment_state.max_hop_count);
    uint32_t natural_window_ms = discovery_assignment_collection_window_ms(
        gateway_discovery_assignment_state.response_spread_ms,
        collection_hop_count);
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t remaining_64 =
        gateway_discovery_assignment_state.operation_deadline_ms > now_ms ?
        gateway_discovery_assignment_state.operation_deadline_ms - now_ms :
        0u;
    uint32_t remaining_ms = remaining_64 > UINT32_MAX ?
                            UINT32_MAX : (uint32_t)remaining_64;
    bool collecting_claims =
        gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;

    if (collecting_claims) {
        return gateway_command_budget_weighted_window_ms(
            gateway_discovery_assignment_state.budget_explicit,
            remaining_ms,
            3u,
            5u,
            natural_window_ms);
    }
    uint8_t windows_remaining =
        app_discovery_assignment_table_windows_remaining(
            gateway_discovery_assignment_state.table_round,
            gateway_discovery_assignment_table_round_limit_locked());

    return gateway_command_budget_window_ms(
        gateway_discovery_assignment_state.budget_explicit,
        remaining_ms,
        windows_remaining,
        natural_window_ms);
}

static uint8_t gateway_discovery_assignment_claim_round_limit_locked(void)
{
    uint8_t budget_round_limit = gateway_command_budget_retry_limit(
        gateway_discovery_assignment_state.budget_explicit,
        gateway_discovery_assignment_state.command_budget_ms,
        DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);

    return app_discovery_assignment_claim_round_limit(
        gateway_discovery_assignment_state.budget_explicit,
        budget_round_limit,
        DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);
}

static uint8_t gateway_discovery_assignment_table_round_limit_locked(void)
{
    return gateway_command_budget_retry_limit(
        gateway_discovery_assignment_state.budget_explicit,
        gateway_discovery_assignment_state.command_budget_ms,
        DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS);
}

static uint32_t gateway_discovery_assignment_next_command_seq(uint32_t current)
{
    current++;
    return current == 0u ? 1u : current;
}

static uint64_t gateway_discovery_assignment_expected_ack_mask_locked(void)
{
    size_t count = gateway_discovery_assignment_state.claim_count;

    return count >= 64u ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
}

static uint8_t gateway_discovery_assignment_missing_ack_count_locked(void)
{
    uint64_t missing =
        gateway_discovery_assignment_expected_ack_mask_locked() &
                       ~gateway_discovery_assignment_state.ack_mask;
    uint8_t count = 0u;

    while (missing != 0u) {
        count += (uint8_t)(missing & 1u);
        missing >>= 1;
    }
    return count;
}

static bool gateway_discovery_assignment_expected_claims_complete_locked(void)
{
    return gateway_discovery_assignment_state.expected_claim_count != 0u &&
           gateway_discovery_assignment_state.claim_count >=
               gateway_discovery_assignment_state.expected_claim_count;
}

static uint32_t gateway_discovery_assignment_arm_claim_ack_settle_locked(void)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t remaining_64;

    if (gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS ||
        !gateway_discovery_assignment_expected_claims_complete_locked()) {
        return 0u;
    }
    gateway_discovery_assignment_state.claim_ack_settle_deadline_ms =
        discovery_assignment_claim_ack_settle_deadline_ms(
            now_ms,
            gateway_discovery_assignment_state.max_hop_count);
    gateway_discovery_assignment_state.claim_ack_settle_armed = true;
    remaining_64 =
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms - now_ms;
    return remaining_64 > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_64;
}

static uint32_t gateway_discovery_assignment_claim_ack_settle_remaining_locked(
    uint64_t now_ms)
{
    uint64_t remaining_64;

    if (!gateway_discovery_assignment_state.claim_ack_settle_armed ||
        !discovery_assignment_response_ack_settle_pending(
            now_ms,
            gateway_discovery_assignment_state.claim_ack_settle_deadline_ms)) {
        return 0u;
    }
    remaining_64 =
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms - now_ms;
    return remaining_64 > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_64;
}

static int gateway_discovery_assignment_open_claim_round_locked(void)
{
    uint32_t wait_ms;
    int ret;

    if (gateway_discovery_assignment_state.claim_round >=
        gateway_discovery_assignment_claim_round_limit_locked()) {
        return -ETIMEDOUT;
    }
    gateway_discovery_assignment_state.claim_command_seq =
        gateway_discovery_assignment_next_command_seq(
            gateway_discovery_assignment_state.claim_command_seq);
    ret = gateway_send_discovery_assignment_claim_request_locked();
    gateway_discovery_assignment_state.round_open = false;
    if (ret == 0) {
        gateway_discovery_assignment_state.claim_admission_retry_round = 0u;
        wait_ms = GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS;
    } else {
        uint8_t retry_round =
            gateway_discovery_assignment_state.claim_admission_retry_round;

        if (gateway_discovery_assignment_state.claim_admission_retry_round <
            UINT8_MAX) {
            gateway_discovery_assignment_state.claim_admission_retry_round++;
        }
        wait_ms = discovery_assignment_retry_backoff_ms(
            retry_round, sys_rand32_get());
    }
    (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                            K_MSEC(wait_ms));
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            ret == 0 ? GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT :
                       GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.attempt = gateway_discovery_assignment_state.claim_round + 1u;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        event.status = ret == 0 ? COMMAND_OK : COMMAND_RADIO_ERROR;
        event.reason = ret == 0 ? GATEWAY_COMMAND_EVENT_REASON_NONE :
                                 GATEWAY_COMMAND_EVENT_REASON_RADIO;
        (void)gateway_observe_command_event(&event, false);
    }
    return ret;
}

static void gateway_discovery_assignment_fail_locked(
    enum command_status status,
    uint8_t reason)
{
    enum gateway_command_event_reason event_reason;
    uint16_t failure_count = 0u;

    if (!gateway_discovery_assignment_state.active) {
        return;
    }
    if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    } else if (status == COMMAND_TIMEOUT &&
               gateway_discovery_assignment_state.claim_count == 0u &&
               gateway_discovery_assignment_state.claim_delivery_succeeded) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS;
    } else if (status == COMMAND_TIMEOUT &&
               gateway_discovery_assignment_state.table_delivery_succeeded) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    } else {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
    }
    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
        failure_count =
            gateway_discovery_assignment_missing_ack_count_locked();
    } else if (status != COMMAND_OK &&
               gateway_discovery_assignment_state.claim_count > 0u) {
        failure_count = (uint16_t)
            gateway_discovery_assignment_state.claim_count;
    }
    gateway_discovery_assignment_abandon_active_delivery_locked();
    gateway_emit_host_command_result(&gateway_discovery_assignment_state.host_command,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     status,
                                     reason);
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.status = status;
        event.reason = event_reason;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.total_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.failure_count = failure_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, true);
    }
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_finalize_work);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_publish_work);
}

static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_gateway_assignment_publisher_diagnostics publisher_diag;
    struct operation_policy_set provided_policy;
    struct operation_policy_set operation_policy;
    uint32_t budget_ms = DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    uint16_t expected_claim_count = 0u;
    bool budget_explicit = false;
    bool expected_count_present = false;
    int ret;

    if (host_command == NULL || payload == NULL || DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
#define GATEWAY_ASSIGNMENT_START_RETURN(value) do {                          \
        k_mutex_unlock(&gateway_discovery_assignment_mutex);                 \
        return (value);                                                      \
    } while (false)
    if (gateway_discovery_assignment_state.active) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         1u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    app_gateway_assignment_publisher_get_diagnostics(&publisher_diag);
    if (publisher_diag.active) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         2u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    ret = operation_policy_set_from_tlvs(payload,
                                         payload_len,
                                         &provided_policy);
    if (ret != PROTO_OK ||
        app_operation_policy_install(&provided_policy, 0u) < 0) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         EBADMSG);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBADMSG);
    }
    app_operation_policy_snapshot(&operation_policy);
    ret = gateway_command_extract_budget_ms(
        payload,
        payload_len,
        DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
        &budget_ms,
        &budget_explicit);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        GATEWAY_ASSIGNMENT_START_RETURN(mesh_errno_from_proto(ret));
    }
    ret = discovery_assignment_extract_expected_count(
        payload,
        payload_len,
        &expected_claim_count,
        &expected_count_present);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        GATEWAY_ASSIGNMENT_START_RETURN(mesh_errno_from_proto(ret));
    }
    if (provided_policy.assignment_present) {
        if ((budget_explicit &&
             budget_ms != provided_policy.assignment.operation_budget_ms) ||
            (expected_count_present &&
             expected_claim_count !=
                provided_policy.assignment.expected_anchor_count)) {
            gateway_emit_host_command_result(host_command,
                                             CMD_ASSIGN_DISCOVERY_SLOTS,
                                             COMMAND_MALFORMED_PAYLOAD,
                                             EINVAL);
            GATEWAY_ASSIGNMENT_START_RETURN(-EINVAL);
        }
        budget_ms = provided_policy.assignment.operation_budget_ms;
        expected_claim_count =
            provided_policy.assignment.expected_anchor_count;
        budget_explicit = true;
        expected_count_present = expected_claim_count != 0u;
    }
    memset(&gateway_discovery_assignment_state,
           0,
           sizeof(gateway_discovery_assignment_state));
    gateway_discovery_assignment_state.host_command = *host_command;
    gateway_discovery_assignment_state.epoch =
        gateway_discovery_assignment_new_epoch();
    gateway_discovery_assignment_state.operation_deadline_ms =
        (uint64_t)k_uptime_get() + budget_ms;
    gateway_discovery_assignment_state.command_budget_ms = budget_ms;
    gateway_discovery_assignment_state.response_spread_ms =
        operation_policy.assignment.response_spread_ms;
    gateway_discovery_assignment_state.budget_explicit = budget_explicit;
    gateway_discovery_assignment_state.expected_claim_count =
        expected_count_present ? expected_claim_count : 0u;
    gateway_discovery_assignment_state.generation =
        gateway_discovery_assignment_next_generation();
    gateway_discovery_assignment_state.claim_command_seq =
        gateway_discovery_assignment_state.epoch - 1u;
    gateway_discovery_assignment_state.stage =
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;
    gateway_discovery_assignment_state.active = true;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            host_command,
            gateway_discovery_assignment_state.epoch);

        (void)gateway_observe_command_event(&event, false);
    }

    ret = gateway_discovery_assignment_open_claim_round_locked();
    LOG_INF("gateway discovery-slot collection started: epoch=%u expected=%u window_ms=%u ret=%d",
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.expected_claim_count,
            gateway_discovery_assignment_window_ms_locked(),
            ret);
    GATEWAY_ASSIGNMENT_START_RETURN(0);
#undef GATEWAY_ASSIGNMENT_START_RETURN
}

int gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t previous_hop_id)
{
    enum discovery_assignment_phase phase = 0;
    enum command_id command_id = CMD_VENDOR_BASE;
    const uint8_t *hop_raw = NULL;
    const uint8_t *status_raw = NULL;
    uint8_t hop_len = 0u;
    uint8_t status_len = 0u;
    uint8_t hop_count = 0u;
    uint64_t hash = 0u;
    uint32_t epoch = 0u;
    size_t anchor_index = SIZE_MAX;
    bool duplicate_ack;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT) {
        return -ENOENT;
    }
    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    if (command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return -ENOENT;
    }
    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    if (ret != PROTO_OK) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_STATE_REJECT reason=control ret=%d src=0x%016llx\n",
                            ret,
                            (unsigned long long)packet->src_id);
        return -EBADMSG;
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_STATE_REJECT reason=phase phase=%u epoch=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            (unsigned long long)packet->src_id);
        return -EPROTO;
    }
    ret = tlv_find(payload,
                   payload_len,
                   TLV_COMMAND_STATUS,
                   &status_raw,
                   &status_len);
    if (ret != PROTO_OK || status_len != sizeof(uint16_t) ||
        proto_get_u16_le(status_raw) != COMMAND_OK ||
        discovery_assignment_extract_claim_hash(payload,
                                                payload_len,
                                                &hash) != PROTO_OK ||
        packet->src_id == 0u ||
        hash != discovery_assignment_hash(packet->src_id)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u ret=%d\n",
                            (unsigned long long)packet->src_id,
                            epoch,
                            ret);
        return -EBADMSG;
    }
    ret = tlv_find(payload, payload_len, TLV_HOP_COUNT, &hop_raw, &hop_len);
    if (ret == PROTO_OK) {
        if (hop_len != sizeof(uint8_t)) {
            return -EBADMSG;
        }
        hop_count = hop_raw[0];
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return -EBADMSG;
    }

    /*
     * A valid result that arrives after its assignment transaction has
     * become terminal must still receive the transport ACK.  Otherwise every
     * relay retains that obsolete result in custody forever and it can block
     * the next assignment operation.  Validation stays ahead of this branch
     * so malformed or forged identities remain unacknowledged and retryable.
     */
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
#define GATEWAY_ASSIGNMENT_RETURN(value) do {                                \
        k_mutex_unlock(&gateway_discovery_assignment_mutex);                 \
        return (value);                                                      \
    } while (false)
    if (!gateway_discovery_assignment_state.active) {
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=inactive phase=%u epoch=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (epoch != gateway_discovery_assignment_state.epoch) {
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=epoch phase=%u received=%u expected=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            gateway_discovery_assignment_state.epoch,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=deadline phase=%u epoch=%u now=%u deadline=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            k_uptime_get_32(),
                            (uint32_t)gateway_discovery_assignment_state.operation_deadline_ms,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (!gateway_discovery_assignment_rf_started_locked(
            phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ?
                GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE :
                GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_STATE_REJECT reason=rf-start phase=%u epoch=%u kind=%u handle=%u claim_ok=%u table_ok=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            (unsigned int)gateway_discovery_assignment_state.delivery_kind,
                            gateway_discovery_assignment_state.delivery_handle,
                            gateway_discovery_assignment_state.claim_delivery_succeeded ? 1u : 0u,
                            gateway_discovery_assignment_state.table_delivery_succeeded ? 1u : 0u,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(-EAGAIN);
    }
    if (hop_count > gateway_discovery_assignment_state.max_hop_count) {
        gateway_discovery_assignment_state.max_hop_count = hop_count;
    }
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        if (gateway_discovery_assignment_state.anchor_ids[i] == packet->src_id) {
            anchor_index = i;
            break;
        }
    }

    if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        if (gateway_discovery_assignment_state.stage !=
                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS ||
            packet->session_id !=
                gateway_discovery_assignment_state.table_command_seq ||
            anchor_index == SIZE_MAX) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=ack-state src=0x%016llx epoch=%u session=%u expected=%u index=%d\n",
                                (unsigned long long)packet->src_id,
                                epoch,
                                packet->session_id,
                                gateway_discovery_assignment_state.table_command_seq,
                                anchor_index == SIZE_MAX ? -1 : (int)anchor_index);
            GATEWAY_ASSIGNMENT_RETURN(
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
        }
        duplicate_ack = (gateway_discovery_assignment_state.ack_mask &
                         (UINT64_C(1) << anchor_index)) != 0u;
        gateway_discovery_assignment_state.ack_mask |=
            UINT64_C(1) << anchor_index;
        status_debug_printf("DBG_DISCOVERY_SLOT_ACK_RX epoch=%u anchor=0x%016llx hop=%u acked=%u missing=%u\n",
                            epoch,
                            (unsigned long long)packet->src_id,
                            hop_count,
                            (unsigned int)__builtin_popcountll(
                                gateway_discovery_assignment_state.ack_mask),
                            gateway_discovery_assignment_missing_ack_count_locked());
        if (gateway_discovery_assignment_missing_ack_count_locked() == 0u) {
            gateway_discovery_assignment_state.response_ack_settle_armed = true;
            gateway_discovery_assignment_state.response_ack_settle_deadline_ms =
                discovery_assignment_response_ack_settle_deadline_ms(
                    (uint64_t)k_uptime_get());
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_NO_WAIT);
        }
        GATEWAY_ASSIGNMENT_RETURN(
            duplicate_ack ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                            APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
    }

    if (anchor_index != SIZE_MAX) {
        uint32_t settle_ms =
            gateway_discovery_assignment_arm_claim_ack_settle_locked();

        if (gateway_discovery_assignment_state.duplicate_count < UINT16_MAX) {
            gateway_discovery_assignment_state.duplicate_count++;
        }
        if (settle_ms != 0u && gateway_discovery_assignment_state.round_open) {
            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_SETTLE epoch=%u hop=%u delay=%u duplicate=1\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.max_hop_count,
                                settle_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(settle_ms));
        }
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (gateway_discovery_assignment_state.claim_count >=
        ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u reason=capacity\n",
                            (unsigned long long)packet->src_id,
                            epoch);
        GATEWAY_ASSIGNMENT_RETURN(-ENOSPC);
    }
    gateway_discovery_assignment_state.anchor_ids[
        gateway_discovery_assignment_state.claim_count] = packet->src_id;
    gateway_discovery_assignment_state.claim_count++;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.anchor_id = packet->src_id;
        event.previous_hop_id = previous_hop_id;
        event.hop_count = hop_count;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, false);
    }
    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
        gateway_discovery_assignment_abandon_active_delivery_locked();
        gateway_discovery_assignment_state.table_command_seq =
            gateway_discovery_assignment_next_command_seq(
                gateway_discovery_assignment_state.table_command_seq);
        gateway_discovery_assignment_state.stage =
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;
        gateway_discovery_assignment_state.ack_mask = 0u;
        gateway_discovery_assignment_state.table_round = 0u;
        gateway_discovery_assignment_state.table_delivery_succeeded = false;
        gateway_discovery_assignment_state.claim_ack_settle_armed = false;
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms = 0u;
        gateway_discovery_assignment_state.response_ack_settle_armed = false;
        gateway_discovery_assignment_state.response_ack_settle_deadline_ms = 0u;
        gateway_discovery_assignment_state.claim_round = 0u;
        gateway_discovery_assignment_state.round_open = false;
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_NO_WAIT);
    }
    if (gateway_discovery_assignment_expected_claims_complete_locked()) {
        uint32_t settle_ms =
            gateway_discovery_assignment_arm_claim_ack_settle_locked();

        if (settle_ms != 0u && gateway_discovery_assignment_state.round_open) {
            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_SETTLE epoch=%u hop=%u delay=%u duplicate=0\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.max_hop_count,
                                settle_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(settle_ms));
        }
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_RX epoch=%u anchor=0x%016llx hash=0x%016llx hop=%u count=%u\n",
                        epoch,
                        (unsigned long long)packet->src_id,
                        (unsigned long long)hash,
                        hop_count,
                        (unsigned int)gateway_discovery_assignment_state.claim_count);
    GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
#undef GATEWAY_ASSIGNMENT_RETURN
}

static int gateway_discovery_assignment_publish_table(void)
{
    struct mesh_outbound outbound;
    size_t payload_len;
    int ret;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.claim_count == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -ENOENT;
    }

    ret = discovery_assignment_sort_anchor_ids(
        gateway_discovery_assignment_state.anchor_ids,
        gateway_discovery_assignment_state.claim_count);
    if (gateway_discovery_assignment_state.table_command_seq == 0u) {
        gateway_discovery_assignment_state.table_command_seq =
            gateway_discovery_assignment_state.epoch ^ UINT32_C(0x80000000);
        if (gateway_discovery_assignment_state.table_command_seq == 0u) {
            gateway_discovery_assignment_state.table_command_seq = 1u;
        }
    }
    if (ret == PROTO_OK) {
        ret = gateway_build_discovery_assignment_command(
            &outbound,
            DISCOVERY_ASSIGNMENT_PHASE_TABLE,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq);
    }
    payload_len = ret == 0 ? outbound.payload_len : 0u;
    if (ret == 0) {
        ret = discovery_assignment_append_table_from_anchor_ids(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            gateway_discovery_assignment_state.anchor_ids,
            gateway_discovery_assignment_state.claim_count);
    }
    gateway_discovery_assignment_state.round_open = false;
    if (ret == PROTO_OK || ret == 0) {
        outbound.payload_len = (uint16_t)payload_len;
        outbound.packet.payload_len = (uint16_t)payload_len;
        ret = gateway_discovery_assignment_submit_control_flood_locked(
            &outbound,
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE,
            "discovery-slot-assignment-table");
        if (ret == 0) {
            gateway_discovery_assignment_state.stage =
                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS;
        }
    } else {
        ret = mesh_errno_from_proto(ret);
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_TABLE_TX epoch=%u generation=%u count=%u bytes=%u round=%u queued=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.table_command_seq,
                        (unsigned int)gateway_discovery_assignment_state.claim_count,
                        (unsigned int)payload_len,
                        gateway_discovery_assignment_state.table_round + 1u,
                        ret == 0 ? 1u : 0u,
                        ret);
    if (ret == 0) {
        gateway_discovery_assignment_state.table_admission_retry_round = 0u;
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS));
    } else if (gateway_discovery_assignment_state.table_round <
               gateway_discovery_assignment_table_round_limit_locked()) {
        uint8_t retry_round =
            gateway_discovery_assignment_state.table_admission_retry_round;

        if (gateway_discovery_assignment_state.table_admission_retry_round <
            UINT8_MAX) {
            gateway_discovery_assignment_state.table_admission_retry_round++;
        }
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(discovery_assignment_retry_backoff_ms(
                retry_round,
                sys_rand32_get())));
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret;
}

static bool gateway_discovery_assignment_service_delivery(void)
{
    struct node_comm_terminal_event event;
    enum gateway_discovery_assignment_delivery_kind kind;
    bool effective_delivered;
    bool semantic_terminal_success;
    uint32_t handle;
    uint32_t retry_ms;
    uint8_t round;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    kind = gateway_discovery_assignment_state.delivery_kind;
    handle = gateway_discovery_assignment_state.delivery_handle;
    if (handle == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return false;
    }
    if (!app_node_comm_take_delivery_event_for(handle, &event)) {
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS));
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    gateway_discovery_assignment_state.delivery_handle = 0u;
    gateway_discovery_assignment_state.delivery_kind =
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE;
    status_debug_printf("DBG_DISCOVERY_SLOT_FLOOD_TERMINAL kind=%u handle=%u reason=%u attempts=%u\n",
                        (unsigned int)kind,
                        handle,
                        (unsigned int)event.reason,
                        event.attempts_started);

    semantic_terminal_success =
        app_discovery_assignment_semantic_terminal_success(
            kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE ?
                APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE :
                APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
            gateway_discovery_assignment_state.expected_claim_count,
            gateway_discovery_assignment_state.claim_count,
            gateway_discovery_assignment_missing_ack_count_locked(),
            event.attempts_started,
            app_discovery_assignment_operation_expired(
                (uint64_t)k_uptime_get(),
                gateway_discovery_assignment_state.operation_deadline_ms),
            event.reason == NODE_COMM_TERMINAL_CANCELLED);
    effective_delivered = event.reason == NODE_COMM_TERMINAL_DELIVERED ||
                          semantic_terminal_success;
    if (semantic_terminal_success &&
        event.reason != NODE_COMM_TERMINAL_DELIVERED) {
        status_debug_printf("DBG_DISCOVERY_SLOT_FLOOD_SEMANTIC_OVERRIDE kind=%u handle=%u reason=%u attempts=%u claims=%u expected=%u missing=%u\n",
                            (unsigned int)kind,
                            handle,
                            (unsigned int)event.reason,
                            event.attempts_started,
                            (unsigned int)gateway_discovery_assignment_state.claim_count,
                            gateway_discovery_assignment_state.expected_claim_count,
                            gateway_discovery_assignment_missing_ack_count_locked());
    }

    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM) {
        if (event.attempts_started > 0u &&
            gateway_discovery_assignment_state.claim_round < UINT8_MAX) {
            gateway_discovery_assignment_state.claim_round++;
        }
        if (effective_delivered) {
            gateway_discovery_assignment_state.claim_delivery_succeeded = true;
        }
    } else if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE) {
        if (event.attempts_started > 0u &&
            gateway_discovery_assignment_state.table_round < UINT8_MAX) {
            gateway_discovery_assignment_state.table_round++;
        }
        if (effective_delivered) {
            gateway_discovery_assignment_state.table_delivery_succeeded = true;
        }
    } else {
        gateway_discovery_assignment_fail_locked(COMMAND_INTERNAL_ERROR,
                                                 EPROTO);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }

    if (effective_delivered) {
        uint32_t wait_ms;

        gateway_discovery_assignment_state.round_open = true;
        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM &&
            gateway_discovery_assignment_expected_claims_complete_locked()) {
            if (!gateway_discovery_assignment_state.claim_ack_settle_armed) {
                (void)gateway_discovery_assignment_arm_claim_ack_settle_locked();
            }
            wait_ms =
                gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                    (uint64_t)k_uptime_get());
        } else if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE &&
                   gateway_discovery_assignment_missing_ack_count_locked() == 0u) {
            wait_ms = 0u;
        } else {
            wait_ms = gateway_discovery_assignment_window_ms_locked();
        }
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(wait_ms));
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }

    gateway_discovery_assignment_state.round_open = false;
    if (event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED) {
        gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT,
                                                 (uint8_t)event.reason);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    if (event.reason == NODE_COMM_TERMINAL_PERMANENT_FAILURE ||
        event.reason == NODE_COMM_TERMINAL_CANCELLED ||
        (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM &&
         gateway_discovery_assignment_state.claim_round >=
             gateway_discovery_assignment_claim_round_limit_locked()) ||
        (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE &&
         gateway_discovery_assignment_state.table_round >=
             gateway_discovery_assignment_table_round_limit_locked())) {
        gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                                 (uint8_t)event.reason);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    round = kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE ?
                gateway_discovery_assignment_state.table_round :
                gateway_discovery_assignment_state.claim_round;
    retry_ms = discovery_assignment_retry_backoff_ms(
        round == 0u ? 0u : round - 1u,
        sys_rand32_get());
    (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                            K_MSEC(retry_ms));
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return true;
}

static void gateway_discovery_assignment_publish_work_handler(struct k_work *work)
{
    bool current_generation;
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
#define GATEWAY_ASSIGNMENT_PUBLISH_RETURN() do {                             \
        k_mutex_unlock(&gateway_discovery_assignment_mutex);                 \
        return;                                                              \
    } while (false)
    current_generation = app_discovery_assignment_work_guard_begin(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation);
    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !gateway_discovery_assignment_state.active ||
        !current_generation) {
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }

    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS) {
        if (!gateway_discovery_assignment_state.round_open) {
            if (gateway_discovery_assignment_state.claim_round <
                gateway_discovery_assignment_claim_round_limit_locked()) {
                (void)gateway_discovery_assignment_open_claim_round_locked();
                GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
            }
            if (gateway_discovery_assignment_state.claim_count == 0u) {
                gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
                GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
            }
        } else {
            gateway_discovery_assignment_state.round_open = false;
        }

        if (gateway_discovery_assignment_state.claim_round <
            gateway_discovery_assignment_claim_round_limit_locked()) {
            uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
                gateway_discovery_assignment_state.claim_round - 1u,
                sys_rand32_get());

            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_BACKOFF epoch=%u round=%u count=%u delay=%u\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.claim_round,
                                (unsigned int)gateway_discovery_assignment_state.claim_count,
                                retry_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(retry_ms));
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        if (gateway_discovery_assignment_state.claim_count == 0u) {
            gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        {
            uint32_t settle_ms =
                gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                    (uint64_t)k_uptime_get());

            if (settle_ms != 0u) {
                (void)k_work_reschedule(
                    &gateway_discovery_assignment_finalize_work,
                    K_MSEC(settle_ms));
                GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
            }
        }
        gateway_discovery_assignment_state.claim_ack_settle_armed = false;
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms = 0u;

        gateway_discovery_assignment_state.table_round = 0u;
        ret = gateway_discovery_assignment_publish_table();
        if (ret < 0 && gateway_discovery_assignment_state.table_round >=
            gateway_discovery_assignment_table_round_limit_locked()) {
            gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                                     (uint8_t)(-ret));
        }
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }

    if (gateway_discovery_assignment_missing_ack_count_locked() == 0u) {
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }
    if (gateway_discovery_assignment_state.table_round >=
        gateway_discovery_assignment_table_round_limit_locked()) {
        gateway_discovery_assignment_complete_success_locked();
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }
    ret = gateway_discovery_assignment_publish_table();
    if (ret < 0 && gateway_discovery_assignment_state.table_round >=
        gateway_discovery_assignment_table_round_limit_locked()) {
        gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                                 (uint8_t)(-ret));
    }
    GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
#undef GATEWAY_ASSIGNMENT_PUBLISH_RETURN
}

static void gateway_discovery_assignment_finalize_work_handler(struct k_work *work)
{
    enum app_discovery_assignment_work_request request;
    uint8_t missing_ack_count;
    uint8_t table_round_limit;
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
#define GATEWAY_ASSIGNMENT_FINALIZE_RETURN() do {                            \
        k_mutex_unlock(&gateway_discovery_assignment_mutex);                 \
        return;                                                              \
    } while (false)
    if (!gateway_discovery_assignment_state.active) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (gateway_discovery_assignment_service_delivery()) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    missing_ack_count =
        gateway_discovery_assignment_missing_ack_count_locked();
    table_round_limit =
        gateway_discovery_assignment_table_round_limit_locked();
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
        gateway_discovery_assignment_state.round_open) {
        if (missing_ack_count == 0u) {
            uint64_t now_ms = (uint64_t)k_uptime_get();

            if (gateway_discovery_assignment_state.response_ack_settle_armed &&
                discovery_assignment_response_ack_settle_pending(
                    now_ms,
                    gateway_discovery_assignment_state.response_ack_settle_deadline_ms)) {
                uint64_t remaining_64 =
                    gateway_discovery_assignment_state.response_ack_settle_deadline_ms -
                    now_ms;
                uint32_t remaining_ms = remaining_64 > UINT32_MAX ?
                                        UINT32_MAX : (uint32_t)remaining_64;

                (void)k_work_reschedule(
                    &gateway_discovery_assignment_finalize_work,
                    K_MSEC(remaining_ms));
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
            gateway_discovery_assignment_state.response_ack_settle_armed = false;
            gateway_discovery_assignment_state.response_ack_settle_deadline_ms = 0u;
            gateway_discovery_assignment_state.round_open = false;
            gateway_discovery_assignment_complete_success_locked();
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        gateway_discovery_assignment_state.round_open = false;
        if (app_discovery_assignment_table_retry_backoff_required(
                true,
                missing_ack_count,
                gateway_discovery_assignment_state.table_round,
                table_round_limit)) {
            uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
                gateway_discovery_assignment_state.table_round - 1u,
                sys_rand32_get());

            status_debug_printf("DBG_DISCOVERY_SLOT_TABLE_BACKOFF epoch=%u round=%u missing=%u delay=%u\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.table_round,
                                missing_ack_count,
                                retry_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(retry_ms));
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
    }
    request = app_discovery_assignment_work_guard_request(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation);
    if (request == APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (request != APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT) {
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_MSEC(100u));
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    ret = mesh_gateway_command_priority_submit(
        &gateway_discovery_assignment_publish_work);
    app_discovery_assignment_work_guard_note_submit_result(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation,
        ret);
    if (ret < 0) {
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_MSEC(100u));
    }
    GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
#undef GATEWAY_ASSIGNMENT_FINALIZE_RETURN
}
#else
static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(host_command);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t previous_hop_id)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    return -ENOTSUP;
}
#endif

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id)
{
    if (command == NULL) {
        return;
    }

    mesh_clear_route_waiting_tx(command);
    gateway_survey_auto_note_command_timeout(command, command_id);
    if (command_id == CMD_FORCE_REDISCOVERY) {
        (void)app_node_comm_request_route_refresh(
            0u, "force-rediscovery-timeout", false);
    }
}

void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason)
{
    gateway_survey_auto_note_command_result(command, command_id, status, reason);
    if (command_id == CMD_FORCE_REDISCOVERY && status == COMMAND_OK) {
        (void)app_node_comm_request_route_refresh(
            0u, "force-rediscovery-result", false);
    }
}

#if DEVICE_ROLE == ROLE_GATEWAY
static void gateway_survey_service_active_delivery(void)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    struct node_comm_terminal_event event;
    enum node_transaction_action action;
    int ret;

    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        transaction->request_delivery_terminal) {
        return;
    }
    if (!app_node_comm_take_delivery_event_for(
            transaction->request_delivery_handle, &event)) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
        return;
    }
    ret = survey_gateway_transaction_note_delivery_terminal(
        &gateway_survey_transaction, &event, (uint64_t)k_uptime_get(),
        &action);
    if (ret < 0) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "delivery-terminal-mismatch");
        return;
    }
    app_stack_workload_diag_gateway_control_sample(
        &gateway_survey_pending_command,
        event.attempts_started,
        event.reason == NODE_COMM_TERMINAL_DELIVERED ? 1u : 0u);
    if (transaction->state == NODE_TRANSACTION_SUCCEEDED &&
        gateway_survey_result_preflight.valid &&
        gateway_survey_pending_command_valid) {
        gateway_survey_auto_note_command_result(
            &gateway_survey_pending_command,
            gateway_survey_transaction.active_command_id,
            gateway_survey_result_preflight.status,
            gateway_survey_result_preflight.reason);
        return;
    }
    if (action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED ||
        action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON) {
        gateway_survey_abandon_current(
            event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
            (uint8_t)event.reason,
            event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                GATEWAY_COMMAND_EVENT_REASON_RADIO,
            "control-delivery-failed");
    }
}
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_survey_wait_for_discovery_collection(void)
{
    struct node_comm_terminal_event event;
    enum survey_gateway_collection_decision decision;
    enum gateway_command_event_reason failure_reason;
    uint32_t now_ms;
    uint32_t operation_wait_ms;
    uint32_t wait_ms;
    bool emission_horizon_elapsed;
    bool safety_deadline_elapsed;

    if (!gateway_survey_collection_pending) {
        return false;
    }
    if (gateway_survey_discovery_delivery_handle != 0u) {
        uint32_t handle = gateway_survey_discovery_delivery_handle;

        if (!app_node_comm_take_delivery_event_for(handle, &event)) {
            (void)k_work_reschedule(
                &gateway_survey_work,
                K_MSEC(GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS));
            return true;
        }
        gateway_survey_discovery_delivery_handle = 0u;
        status_debug_printf("DBG_SURVEY_DISCOVERY_FLOOD_TERMINAL handle=%u reason=%u attempts=%u\n",
                            handle,
                            (unsigned int)event.reason,
                            event.attempts_started);
        if (event.reason != NODE_COMM_TERMINAL_DELIVERED) {
            gateway_survey_collection_pending = false;
            gateway_survey_collection_deadline_ms = 0u;
            gateway_survey_collection_duration_ms = 0u;
            gateway_survey_collection_window_armed = false;
            failure_reason =
                event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                    GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED ?
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                    GATEWAY_COMMAND_EVENT_REASON_RADIO;
            gateway_survey_auto_finish_status(
                event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                    COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
                failure_reason);
            return true;
        }
        gateway_survey_collection_deadline_ms =
            k_uptime_get_32() + gateway_survey_collection_duration_ms;
        gateway_survey_collection_window_armed = true;
    }

    now_ms = k_uptime_get_32();
    if (!gateway_survey_collection_window_armed) {
        gateway_survey_collection_pending = false;
        gateway_survey_collection_duration_ms = 0u;
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    emission_horizon_elapsed = uptime_deadline_reached(
        now_ms, gateway_survey_collection_emission_deadline_ms);
    safety_deadline_elapsed = uptime_deadline_reached(
        now_ms, gateway_survey_collection_deadline_ms);
    decision = survey_gateway_collection_decide(
        emission_horizon_elapsed,
        safety_deadline_elapsed,
        gateway_survey_context.report_count,
        gateway_survey_expected_node_count,
        gateway_survey_expected_node_count_present);
    if (decision == SURVEY_GATEWAY_COLLECTION_WAIT) {
        wait_ms = uptime_ms_until_deadline(
            now_ms,
            emission_horizon_elapsed ?
                gateway_survey_collection_deadline_ms :
                gateway_survey_collection_emission_deadline_ms);
        operation_wait_ms = uptime_ms_until_deadline(
            now_ms, gateway_survey_operation_deadline_ms);
        if (operation_wait_ms < wait_ms) {
            wait_ms = operation_wait_ms;
        }
        (void)k_work_reschedule(
            &gateway_survey_work,
            K_MSEC(wait_ms));
        return true;
    }
    gateway_survey_collection_pending = false;
    gateway_survey_collection_deadline_ms = 0u;
    gateway_survey_collection_emission_deadline_ms = 0u;
    gateway_survey_collection_duration_ms = 0u;
    gateway_survey_collection_window_armed = false;
    if (decision == SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH) {
        bool missing = gateway_survey_context.report_count <
                       gateway_survey_expected_node_count;

        status_debug_printf(
            "DBG_SURVEY_COLLECTION_COUNT_MISMATCH expected=%u observed=%u missing=%u\n",
            gateway_survey_expected_node_count,
            (unsigned int)gateway_survey_context.report_count,
            missing ? 1u : 0u);
        gateway_survey_auto_finish_status(
            missing ? COMMAND_TIMEOUT : COMMAND_INVALID_STATE,
            gateway_survey_context.report_count == 0u ?
                GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS :
            missing ?
                GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                GATEWAY_COMMAND_EVENT_REASON_CAPACITY);
        return true;
    }
    return false;
}
#endif

static void gateway_survey_work_handler(struct k_work *work)
{
    struct survey_gateway_auto_action action = {0};
    enum gateway_survey_pair_finalize_result finalize_result;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_service_active_delivery();
    if (gateway_survey_finish_pending &&
        (gateway_survey_transaction.active.state == NODE_TRANSACTION_EMPTY ||
         gateway_survey_transaction.active.request_delivery_terminal)) {
        enum command_status finish_status =
            gateway_survey_finish_pending_status;
        enum gateway_command_event_reason finish_reason =
            gateway_survey_finish_pending_reason;

        gateway_survey_finish_pending = false;
        gateway_survey_auto_finish_status(finish_status, finish_reason);
        return;
    }
    gateway_survey_service_cleanup();
    if (!gateway_survey_active) {
        goto out;
    }
    if (uptime_deadline_reached(k_uptime_get_32(),
                                gateway_survey_operation_deadline_ms)) {
        gateway_survey_auto_finish_status(
            COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
        goto out;
    }
    if (!gateway_survey_flush_boundary_event()) {
        goto out;
    }
    if (gateway_survey_wait_for_discovery_collection()) {
        return;
    }
    if (gateway_survey_cleanup_pending() &&
        (gateway_survey_pair_observation_active ||
         !survey_gateway_auto_no_unstarted_pairs(
             &gateway_survey_auto, &gateway_survey_context))) {
        goto out;
    }
    if (survey_gateway_response_ack_settle_pending(
            &gateway_survey_response_ack_settle,
            (uint64_t)k_uptime_get())) {
        goto out;
    }
#endif
    if (gateway_survey_auto.waiting) {
        goto out;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_round_drive()) {
        goto out;
    }
#endif
    finalize_result = gateway_survey_finalize_pair_observation();
    if (finalize_result == GATEWAY_SURVEY_PAIR_FINALIZE_TERMINAL) {
        gateway_survey_auto_finish();
        goto out;
    }
    if (finalize_result == GATEWAY_SURVEY_PAIR_FINALIZE_RETRY) {
        goto out;
    }
    if (!gateway_survey_auto.running) {
        if (!gateway_survey_context.pairs_planned) {
            ret = survey_gateway_plan_pairs(&gateway_survey_context);
            if (ret != PROTO_OK) {
                LOG_WRN("gateway survey final pair planning failed: survey=%u ret=%d",
                        gateway_survey_context.survey_id,
                        ret);
                gateway_survey_auto_finish();
                goto out;
            }
        }
        if (!gateway_survey_emit_collection_telemetry()) {
            goto out;
        }
        LOG_INF("gateway survey orchestration starting: survey=%u reports=%u pairs=%u",
                gateway_survey_context.survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count);
    }

    ret = survey_gateway_auto_next_action(&gateway_survey_auto,
                                          &gateway_survey_context,
                                          &action);
    if (ret == PROTO_OK && action.complete) {
        gateway_survey_auto_finish();
        goto out;
    }
    if (ret == PROTO_ERR_BUSY) {
        goto out;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey next action failed: survey=%u ret=%d",
                gateway_survey_context.survey_id,
                ret);
        gateway_survey_auto_finish();
        goto out;
    }

    ret = gateway_survey_auto_send_action(&action);
    if (ret < 0) {
        LOG_WRN("gateway survey auto send failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)action.command_id,
                (unsigned long long)action.target_id,
                ret);
        if (ret == -ETIMEDOUT) {
            gateway_survey_auto_finish_status(
                COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
            goto out;
        }
#if DEVICE_ROLE == ROLE_GATEWAY
        if (gateway_survey_transaction.active.state == NODE_TRANSACTION_ACTIVE) {
            (void)app_node_comm_cancel_delivery(
                gateway_survey_transaction.active.request_delivery_handle);
        }
        survey_gateway_transaction_require_cleanup(
            &gateway_survey_transaction, false, (uint64_t)k_uptime_get());
#endif
        gateway_survey_auto.waiting = false;
        gateway_survey_auto.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
        gateway_survey_auto_log_skipped_pair("send-failed",
            ret == -EHOSTUNREACH ? COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
            (uint8_t)(-ret),
            ret == -EHOSTUNREACH || ret == -ENOTCONN ?
                GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE :
                ret == -ETIMEDOUT ?
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                    GATEWAY_COMMAND_EVENT_REASON_RADIO,
            action.command_id,
            action.target_id);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    }

out:
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_schedule_drive();
#endif
}

static int gateway_route_survey_pair_control(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len,
                                             enum command_id command_id)
{
    struct mesh_outbound outbound = {0};
    struct survey_pair pair = {0};
    size_t payload_len = 0u;
    uint64_t absolute_deadline_ms;
    uint32_t delivery_handle = 0u;
    uint32_t client_token;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }
    if (gateway_survey_active
#if DEVICE_ROLE == ROLE_GATEWAY
        || gateway_survey_cleanup_pending()
#endif
    ) {
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_BUSY,
                                         5u);
        return -EBUSY;
    }

    ret = survey_extract_pair_tlvs(host_payload, host_payload_len, &pair);
    if (ret != PROTO_OK ||
        (host_packet->dst_id != pair.initiator_id &&
         host_packet->dst_id != pair.responder_id)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == PROTO_OK ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(ret == PROTO_OK ? 2u : -ret));
        return ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret);
    }
    if (pair.sample_count > SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           4u);
        LOG_WRN("gateway survey pair control rejected: cmd=0x%04x survey=%u samples=%u runtime_max=%u",
                (unsigned int)command_id,
                pair.survey_id,
                pair.sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
        return -EINVAL;
    }

    if (command_id == CMD_SURVEY_START_PAIR) {
        if (host_payload_len > sizeof(outbound.payload)) {
            ret = PROTO_ERR_NO_SPACE;
        } else {
            memcpy(outbound.payload, host_payload, host_payload_len);
            payload_len = host_payload_len;
            ret = PROTO_OK;
        }
    } else {
        ret = survey_append_pair_tlvs(outbound.payload,
                                      sizeof(outbound.payload),
                                      &payload_len,
                                      &pair);
    }
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        ret = survey_init_pair_prepare_packet(&outbound.packet,
                                              &pair,
                                              DEVICE_ID,
                                              seq,
                                              (uint8_t)payload_len);
    } else {
        outbound.packet.msg_type = MSG_COMMAND;
        outbound.packet.flags = host_packet->flags & FLAG_DIAGNOSTIC;
        outbound.packet.src_id = DEVICE_ID;
        outbound.packet.dst_id = host_packet->dst_id;
        outbound.packet.session_id = pair.survey_id;
        outbound.packet.seq = seq;
        outbound.packet.ttl = host_packet->ttl != 0u ?
                              host_packet->ttl : MESH_DEFAULT_TTL;
        outbound.packet.payload_len = (uint8_t)payload_len;
        ret = PROTO_OK;
    }
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = host_packet->dst_id;
    outbound.payload_len = (uint8_t)payload_len;
    {
        uint32_t control_timeout_ms =
            gateway_survey_natural_control_timeout_ms(
                outbound.packet.dst_id);

        absolute_deadline_ms = (uint64_t)k_uptime_get() +
                               control_timeout_ms;
        ret = gateway_begin_command_result_wait_for(
            &outbound.packet,
            command_id,
            control_timeout_ms);
    }
    client_token = ((uint32_t)command_id << 16) | seq;
    if (ret < 0) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = gateway_survey_send_pair_control(
        &outbound,
        absolute_deadline_ms,
        client_token,
        &delivery_handle);
    if (ret < 0) {
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == -EHOSTUNREACH ? COMMAND_TIMEOUT :
                                           ret == -EBUSY ? COMMAND_BUSY :
                                           COMMAND_RADIO_ERROR,
                                           (uint8_t)(-ret));
        return ret;
    }
    ret = app_node_comm_auto_reap_delivery(delivery_handle);
    if (ret < 0) {
        (void)app_node_comm_abandon_delivery(delivery_handle);
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway survey pair control submitted: cmd=0x%04x survey=%u initiator=0x%016llx responder=0x%016llx samples=%u seq=%u handle=%u",
            (unsigned int)command_id,
            pair.survey_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            seq,
            delivery_handle);
    return 0;
}

static int gateway_route_survey_command(const struct proto_packet *host_packet,
                                        const uint8_t *host_payload,
                                        size_t host_payload_len,
                                        enum command_id command_id)
{
    switch (command_id) {
    case CMD_SURVEY_REACHABILITY:
        return gateway_route_survey_reachability(host_packet, host_payload, host_payload_len);
    case CMD_SURVEY_PREPARE_PAIR:
    case CMD_SURVEY_START_PAIR:
        return gateway_route_survey_pair_control(host_packet,
                                                 host_payload,
                                                 host_payload_len,
                                                 command_id);
    default:
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_UNSUPPORTED_COMMAND,
                                         1u);
        return -ENOTSUP;
    }
}

static bool gateway_host_command_send_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -ENOSPC ||
           ret == -EIO || ret == -ETIMEDOUT || ret == -ENOTCONN ||
           ret == -EHOSTUNREACH;
}

static int gateway_route_mesh_host_payload(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct mesh_outbound outbound = {0};
    uint64_t absolute_deadline_ms;

    if (packet == NULL || payload == NULL ||
        payload_len > sizeof(outbound.payload)) {
        return -EINVAL;
    }
    outbound.packet = *packet;
    outbound.packet.payload_len = (uint16_t)payload_len;
    outbound.payload_len = (uint16_t)payload_len;
    memcpy(outbound.payload, payload, payload_len);
    absolute_deadline_ms = (uint64_t)k_uptime_get() +
                           GATEWAY_COMMAND_RESULT_TIMEOUT_MS;
    return app_node_comm_submit_reliable_uplink(
        &outbound,
        absolute_deadline_ms,
        ((uint32_t)packet->msg_type << 16) | packet->seq,
        NULL);
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_route_mesh_host_packet(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_command_orchestrator *orchestrator;
    const struct mesh_outbound *outbound;
    const struct gateway_command_options *command_options;
    enum command_id command_id;
    enum gateway_command_tracking_mode tracking_mode;
    bool sent_now = false;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    if (packet == NULL || payload == NULL ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }
    if (packet->msg_type != MSG_COMMAND) {
        return gateway_route_mesh_host_payload(packet, payload, payload_len);
    }

    orchestrator = mesh_gateway_command_orchestrator_context();
    ret = app_mesh_command_orchestrator_prepare_flood(
        orchestrator,
        DEVICE_ID,
        k_uptime_get_32(),
        packet->seq == 0u ? gateway_next_command_seq() : 0u);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected BLE host command: %d", ret);
        gateway_emit_host_command_result(packet,
                                         CMD_VENDOR_BASE,
                                         ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound = &orchestrator->gateway_flow.outbound;
    command_options = &orchestrator->gateway_flow.options;
    command_id = orchestrator->gateway_flow.command_id;
    tracking_mode = orchestrator->gateway_flow.tracking_mode;
    if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
        ret = gateway_begin_command_result_wait(&outbound->packet, command_id);
        if (ret < 0) {
            LOG_WRN("gateway command result tracker busy: cmd=0x%04x dst=0x%016llx ret=%d",
                    (unsigned int)command_id,
                    (unsigned long long)outbound->packet.dst_id,
                    ret);
            gateway_emit_host_command_result(&outbound->packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
        ret = gateway_begin_command_collection(command_options);
        if (ret < 0) {
            LOG_WRN("gateway command collection tracker busy: cmd=0x%04x command_seq=%u ret=%d",
                    (unsigned int)command_id,
                    command_options->command_seq,
                    ret);
            gateway_emit_host_command_result(&outbound->packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    }

    if (gateway_command_transport_mode_from_outbound(outbound) ==
        GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST) {
        ret = mesh_send_gateway_command_flood(orchestrator,
                                              "ble-command-broadcast",
                                              &sent_now);
        if (ret == 0 && sent_now) {
            mesh_relay_note_tx_sent(&mesh_runtime, outbound, k_uptime_get_32());
        }
    } else {
        ret = app_node_comm_submit_protocol_response(
            outbound,
            (uint64_t)k_uptime_get() + GATEWAY_COMMAND_RESULT_TIMEOUT_MS,
            ((uint32_t)command_id << 16) | outbound->packet.seq,
            NULL);
    }
    if (ret < 0) {
        LOG_WRN("gateway BLE command route failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound->packet.dst_id,
                ret);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound->packet.dst_id);
            return 0;
        }
        if (gateway_host_command_send_retryable(ret)) {
            if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
                gateway_clear_pending_command_result(&outbound->packet);
            } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
                gateway_clear_command_collection(command_options);
            }
            LOG_INF("gateway command retained for send retry: cmd=0x%04x dst=0x%016llx ret=%d",
                    (unsigned int)command_id,
                    (unsigned long long)outbound->packet.dst_id,
                    ret);
            return -EAGAIN;
        }
        if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
            gateway_clear_pending_command_result(&outbound->packet);
        } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
            gateway_clear_command_collection(command_options);
        }
        gateway_emit_host_command_result(&outbound->packet,
                                         command_id,
                                         ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                         (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway BLE command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
            (unsigned int)command_id,
            (unsigned long long)outbound->packet.dst_id,
            outbound->packet.session_id,
            outbound->packet.seq,
            outbound->packet.ttl);
    return 0;
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_route_host_packet(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    if (packet != NULL && packet->msg_type == MSG_COMMAND) {
        ret = gateway_command_extract_id(payload, payload_len, &command_id);
        if (ret == PROTO_OK &&
            command_id == CMD_FORCE_REDISCOVERY &&
            packet->dst_id == DEVICE_ID) {
            uint32_t command_budget_ms;
            bool budget_explicit;

            ret = gateway_command_extract_budget_ms(
                payload,
                payload_len,
                APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS,
                &command_budget_ms,
                &budget_explicit);
            if (ret != PROTO_OK) {
                gateway_emit_host_command_result(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    (uint8_t)(-ret));
                gateway_observe_host_terminal(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST);
                return mesh_errno_from_proto(ret);
            }
            ret = app_operation_policy_apply_payload(payload,
                                                     payload_len,
                                                     0u,
                                                     NULL);
            if (ret < 0) {
                gateway_emit_host_command_result(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    EBADMSG);
                gateway_observe_host_terminal(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST);
                return ret;
            }
            ret = app_node_comm_request_route_refresh_correlated_bounded(
                0u,
                "force-rediscovery-ble",
                packet,
                command_budget_ms);
            if (ret < 0) {
                enum command_status status = ret == -EBUSY ?
                    COMMAND_BUSY : COMMAND_INTERNAL_ERROR;
                enum gateway_command_event_reason event_reason =
                    ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL;

                gateway_emit_host_command_result(packet,
                                                 command_id,
                                                 status,
                                                 (uint8_t)event_reason);
                gateway_observe_host_terminal(packet,
                                              command_id,
                                              status,
                                              event_reason);
            }
            return ret;
        }
        if (ret == PROTO_OK &&
            command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
            packet->dst_id == DEVICE_ID) {
            return gateway_start_discovery_assignment(packet,
                                                      payload,
                                                      payload_len);
        }
        if (ret == PROTO_OK && gateway_command_uses_survey_mesh(command_id)) {
            return gateway_route_survey_command(packet, payload, payload_len, command_id);
        }
        if (ret == PROTO_OK &&
            command_id == CMD_SURVEY_ABORT &&
            packet->dst_id == DEVICE_ID) {
            if (gateway_survey_active) {
                LOG_INF("gateway survey orchestration aborted by host: survey=%u",
                        gateway_survey_context.survey_id);
                gateway_survey_auto_finish_status(
                    COMMAND_DENIED, GATEWAY_COMMAND_EVENT_REASON_ABORTED);
            }
            gateway_emit_host_command_result(
                packet, command_id, COMMAND_OK, 0u);
            gateway_observe_host_terminal(
                packet,
                command_id,
                COMMAND_OK,
                GATEWAY_COMMAND_EVENT_REASON_NONE);
            return 0;
        }
    }

    return gateway_route_mesh_host_packet(packet, payload, payload_len);
}

#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
static int gateway_host_command_admit(
    void *ctx,
    struct app_gateway_command_ingress_item *item)
{
    int ret;

    ARG_UNUSED(ctx);

    if (item == NULL) {
        return -EINVAL;
    }
    gateway_host_command_next_admission_id++;
    if (gateway_host_command_next_admission_id == 0u) {
        gateway_host_command_next_admission_id = 1u;
    }
    item->admission_id = gateway_host_command_next_admission_id;
    ret = app_gateway_command_lifecycle_admit(&gateway_host_command_lifecycle,
                                               item);
    if (ret < 0) {
        return ret;
    }
    if (k_msgq_num_free_get(&gateway_host_command_msgq) == 0u) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return -ENOSPC;
    }
    ret = gateway_observe_host_acceptance(&item->packet, item->command_id);
    if (ret < 0) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return ret;
    }
    ret = k_msgq_put(&gateway_host_command_msgq, item, K_NO_WAIT);
    if (ret < 0) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return ret;
    }
    return 0;
}

static int gateway_host_command_submit_priority(void *ctx)
{
    ARG_UNUSED(ctx);

    app_mesh_command_orchestrator_clear_safe_boundary(
        mesh_gateway_command_orchestrator_context());
    return mesh_gateway_command_priority_submit(&gateway_host_command_work);
}

static bool gateway_host_command_is_preemptive(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    ARG_UNUSED(ctx);

    return item != NULL &&
           item->command_id == CMD_SURVEY_ABORT &&
           item->packet.dst_id == DEVICE_ID;
}

static int gateway_host_command_submit_preemptive(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    int ret;

    ARG_UNUSED(ctx);
    if (!gateway_host_command_is_preemptive(NULL, item)) {
        return -EINVAL;
    }
    if (k_msgq_num_free_get(&gateway_host_abort_msgq) == 0u) {
        return -ENOSPC;
    }
    ret = k_msgq_put(&gateway_host_abort_msgq, item, K_NO_WAIT);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_observe_host_acceptance(&item->packet, item->command_id);
    if (ret < 0) {
        struct app_gateway_command_ingress_item abandoned;

        (void)k_msgq_get(&gateway_host_abort_msgq, &abandoned, K_NO_WAIT);
        return ret;
    }
    ret = k_work_submit(&gateway_host_abort_work);
    if (ret < 0) {
        struct app_gateway_command_ingress_item abandoned;

        (void)k_msgq_get(&gateway_host_abort_msgq, &abandoned, K_NO_WAIT);
        return ret;
    }
    return 0;
}

static int gateway_host_command_cancel_admitted(
    void *ctx,
    const struct app_gateway_command_identity *identity)
{
    ARG_UNUSED(ctx);

    return app_gateway_command_lifecycle_cancel(&gateway_host_command_lifecycle,
                                                identity,
                                                NULL,
                                                NULL,
                                                NULL);
}

static void gateway_host_command_emit_result(
    void *ctx,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);

    ARG_UNUSED(ctx);

    gateway_emit_host_command_result(command, command_id, status, reason);
    if (kind != 0 && command != NULL) {
        struct gateway_command_event event = gateway_observability_event(
            kind,
            GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
            command_id,
            command,
            0u);

        event.status = status;
        event.reason = status == COMMAND_BUSY ?
                       GATEWAY_COMMAND_EVENT_REASON_BUSY :
                       GATEWAY_COMMAND_EVENT_REASON_INTERNAL;
        (void)gateway_observe_command_event(&event, true);
    }
}

static void gateway_host_command_note_decoded(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    ARG_UNUSED(ctx);

    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_ble msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         item->packet.msg_type,
                         (unsigned long long)item->packet.src_id,
                         (unsigned long long)item->packet.dst_id,
                         item->packet.seq,
                         (unsigned int)item->payload_len);
}

static void gateway_host_command_schedule_failed(void *ctx, int error)
{
    struct app_gateway_command_ingress_item item;

    ARG_UNUSED(ctx);
    gateway_host_command_retry_pending = false;
    gateway_host_command_retry_round = 0u;
    gateway_host_command_retry_started_ms = 0u;
    while (k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT) == 0) {
        enum app_gateway_command_lifecycle_dispatch dispatch;
        int ret;

        ret = app_gateway_command_lifecycle_begin_dispatch(
            &gateway_host_command_lifecycle, &item, &dispatch);
        if (ret == 0 &&
            dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED) {
            continue;
        }
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-error));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_finish(
            &gateway_host_command_lifecycle, &item);
    }
    LOG_ERR("gateway command priority safe-boundary scheduling failed: ret=%d; admitted commands cancelled",
            error);
}

static void gateway_host_command_retry_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    gateway_host_command_retry_pending = false;
    ret = gateway_host_command_submit_priority(NULL);
    if (ret < 0) {
        gateway_host_command_schedule_failed(NULL, ret);
    }
}

static void gateway_host_command_submit_next_queued(void)
{
    struct app_gateway_command_ingress_item item;
    int ret;

    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    ret = gateway_host_command_submit_priority(NULL);
    if (ret < 0) {
        gateway_host_command_schedule_failed(NULL, ret);
    }
}

static size_t gateway_host_command_cancel_pending_surveys(void)
{
    size_t queued = k_msgq_num_used_get(&gateway_host_command_msgq);
    size_t cancelled = 0u;

    for (size_t i = 0u; i < queued; i++) {
        struct app_gateway_command_ingress_item item;

        if (k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT) != 0) {
            break;
        }
        if (!gateway_command_uses_survey_mesh(item.command_id)) {
            if (k_msgq_put(&gateway_host_command_msgq,
                           &item,
                           K_NO_WAIT) != 0) {
                gateway_emit_host_command_result(
                    &item.packet,
                    item.command_id,
                    COMMAND_INTERNAL_ERROR,
                    ENOSPC);
                gateway_observe_host_terminal(
                    &item.packet,
                    item.command_id,
                    COMMAND_INTERNAL_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
                (void)app_gateway_command_lifecycle_discard(
                    &gateway_host_command_lifecycle, &item);
            }
            continue;
        }
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, &item);
        gateway_emit_host_command_result(
            &item.packet,
            item.command_id,
            COMMAND_DENIED,
            GATEWAY_COMMAND_EVENT_REASON_ABORTED);
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_DENIED,
            GATEWAY_COMMAND_EVENT_REASON_ABORTED);
        cancelled++;
    }
    if (cancelled > 0u) {
        (void)k_work_cancel_delayable(&gateway_host_command_retry_work);
        gateway_host_command_retry_pending = false;
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
    }
    return cancelled;
}

static void gateway_host_abort_work_handler(struct k_work *work)
{
    struct app_gateway_command_ingress_item item;

    ARG_UNUSED(work);
    while (k_msgq_get(&gateway_host_abort_msgq, &item, K_NO_WAIT) == 0) {
        int ret;

        gateway_observe_host_stage(&item.packet,
                                   item.command_id,
                                   GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING);
        (void)gateway_host_command_cancel_pending_surveys();
        ret = gateway_route_host_packet(
            &item.packet, item.payload, item.payload_len);
        if (ret < 0) {
            gateway_emit_host_command_result(
                &item.packet,
                item.command_id,
                COMMAND_INTERNAL_ERROR,
                (uint8_t)(-ret));
            gateway_observe_host_terminal(
                &item.packet,
                item.command_id,
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        }
    }
    gateway_host_command_submit_next_queued();
}

static void gateway_host_command_work_handler(struct k_work *work)
{
    struct app_gateway_command_ingress_item item;
    enum app_gateway_command_lifecycle_dispatch dispatch;
    int ret;

    ARG_UNUSED(work);
    gateway_host_command_retry_pending = false;

    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    ret = app_gateway_command_lifecycle_begin_dispatch(
        &gateway_host_command_lifecycle, &item, &dispatch);
    if (ret < 0) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, &item);
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
        gateway_host_command_submit_next_queued();
        return;
    }
    if (dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
        gateway_host_command_submit_next_queued();
        return;
    }
    ret = app_mesh_command_orchestrator_activate(
        mesh_gateway_command_orchestrator_context(), &item);
    if (ret < 0) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_finish(
            &gateway_host_command_lifecycle, &item);
        gateway_host_command_submit_next_queued();
        return;
    }
    dwm3000_driver_clear_receive_abort();
    gateway_observe_host_stage(&item.packet,
                               item.command_id,
                               GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING);
    ret = gateway_route_host_packet(&item.packet, item.payload, item.payload_len);
    if (ret == -EAGAIN) {
        uint32_t delay_ms;

        if (gateway_host_command_retry_round == 0u) {
            gateway_host_command_retry_started_ms = k_uptime_get_32();
        }
        if (gateway_host_command_retry_round >=
            GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS) {
            gateway_emit_host_command_result(&item.packet,
                                             item.command_id,
                                             COMMAND_TIMEOUT,
                                             ETIMEDOUT);
            gateway_observe_host_terminal(
                &item.packet,
                item.command_id,
                COMMAND_TIMEOUT,
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
            LOG_ERR("gateway BLE command send retries exhausted: cmd=0x%04x attempts=%u age_ms=%u",
                    (unsigned int)item.command_id,
                    gateway_host_command_retry_round,
                    k_uptime_get_32() - gateway_host_command_retry_started_ms);
            (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
            (void)app_gateway_command_lifecycle_finish(
                &gateway_host_command_lifecycle, &item);
            gateway_host_command_retry_round = 0u;
            gateway_host_command_retry_started_ms = 0u;
            gateway_host_command_submit_next_queued();
            return;
        }
        delay_ms = discovery_assignment_retry_backoff_ms(
            gateway_host_command_retry_round,
            sys_rand32_get());
        gateway_host_command_retry_round++;
        {
            enum gateway_command_event_kind kind =
                gateway_observability_kind(item.command_id);

            if (kind != 0) {
                struct gateway_command_event event = gateway_observability_event(
                    kind,
                    GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
                    item.command_id,
                    &item.packet,
                    0u);

                event.attempt = gateway_host_command_retry_round;
                event.status = COMMAND_BUSY;
                event.reason = GATEWAY_COMMAND_EVENT_REASON_BUSY;
                (void)gateway_observe_command_event(&event, false);
            }
        }
        ret = app_gateway_command_lifecycle_requeue_retry(
            &gateway_host_command_lifecycle, &item);
        if (ret < 0) {
            (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
            gateway_emit_host_command_result(&item.packet, item.command_id,
                                             COMMAND_INTERNAL_ERROR,
                                             (uint8_t)(-ret));
            (void)app_gateway_command_lifecycle_finish(
                &gateway_host_command_lifecycle, &item);
            gateway_host_command_retry_round = 0u;
            gateway_host_command_retry_started_ms = 0u;
            gateway_host_command_submit_next_queued();
            return;
        }
        gateway_host_command_retry_pending = true;
        ret = k_work_reschedule(&gateway_host_command_retry_work,
                                K_MSEC(delay_ms));
        if (ret < 0) {
            gateway_host_command_retry_pending = false;
            gateway_host_command_schedule_failed(NULL, ret);
            return;
        }
        LOG_WRN("gateway BLE command send deferred: attempt=%u/%u delay_ms=%u",
                gateway_host_command_retry_round,
                GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS,
                delay_ms);
        return;
    }
    (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
    (void)app_gateway_command_lifecycle_finish(&gateway_host_command_lifecycle,
                                                &item);
    gateway_host_command_retry_round = 0u;
    gateway_host_command_retry_started_ms = 0u;
    if (ret < 0) {
        enum gateway_command_event_kind kind =
            gateway_observability_kind(item.command_id);

        /* Reachability owns every immediate terminal outcome at its source. */
        if (kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION ||
            (kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY &&
             item.command_id != CMD_SURVEY_REACHABILITY)) {
            struct gateway_command_event event = gateway_observability_event(
                kind,
                GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
                item.command_id,
                &item.packet,
                0u);

            event.status = ret == -EBUSY ? COMMAND_BUSY :
                           ret == -ETIMEDOUT || ret == -EHOSTUNREACH ?
                           COMMAND_TIMEOUT :
                           ret == -EINVAL || ret == -EMSGSIZE ?
                           COMMAND_MALFORMED_PAYLOAD : COMMAND_RADIO_ERROR;
            event.reason = item.command_id == CMD_SURVEY_REACHABILITY && ret == -EINVAL ?
                           GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION :
                           ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                           ret == -ETIMEDOUT || ret == -EHOSTUNREACH ?
                           GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                           ret == -EINVAL || ret == -EMSGSIZE ?
                           GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST :
                           GATEWAY_COMMAND_EVENT_REASON_RADIO;
            (void)gateway_observe_command_event(&event, true);
        }
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                item.packet.msg_type,
                (unsigned long long)item.packet.dst_id,
                ret);
    }
    gateway_host_command_submit_next_queued();
}
#endif

#if defined(CONFIG_IMEC_GATEWAY_BLE)
void gateway_handle_ble_frame(const uint8_t *frame, size_t frame_len)
{
#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    int ret;

    ret = gateway_ble_send_packet_frame(frame, frame_len);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(gateway_ble_notify_failures);
        LOG_WRN("gateway BLE connectivity test echo failed: frame_len=%u ret=%d",
                (unsigned int)frame_len,
                ret);
        return;
    }
    LOG_INF("gateway BLE connectivity test echoed packet frame: frame_len=%u",
            (unsigned int)frame_len);
#elif defined(CONFIG_IMEC_ML_CLICKER)
    ml_clicker_handle_ble_frame(frame, frame_len);
#elif defined(CONFIG_IMEC_ML_ANCHOR)
    LOG_WRN("ML anchor BLE packet RX ignored: frame_len=%u",
            (unsigned int)frame_len);
#else
#if DEVICE_ROLE == ROLE_GATEWAY
    struct app_mesh_command_orchestrator *orchestrator =
        mesh_gateway_command_orchestrator_context();
    const struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .is_preemptive = gateway_host_command_is_preemptive,
        .submit_preemptive = gateway_host_command_submit_preemptive,
        .admit = gateway_host_command_admit,
        .submit_priority = gateway_host_command_submit_priority,
        .cancel_admitted = gateway_host_command_cancel_admitted,
        .emit_result = gateway_host_command_emit_result,
        .note_decoded = gateway_host_command_note_decoded,
    };
    bool command_handled;
    int ret;

    ret = app_mesh_command_orchestrator_gateway_ingress(
        orchestrator,
        &ingress_ops,
        frame,
        frame_len,
        &command_handled);
    if (ret != 0) {
        if (command_handled) {
            LOG_WRN("gateway BLE command ingress failed: %d", ret);
        } else {
            LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        }
        return;
    }
    if (command_handled) {
        return;
    }

    /* Ingress always decodes into the shared context, including non-commands. */
    ret = gateway_route_host_packet(&orchestrator->admitted.packet,
                                    orchestrator->admitted.payload,
                                    orchestrator->admitted.payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                orchestrator->admitted.packet.msg_type,
                (unsigned long long)orchestrator->admitted.packet.dst_id,
                ret);
    }
#else
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        return;
    }
    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_ble msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         packet.msg_type,
                         (unsigned long long)packet.src_id,
                         (unsigned long long)packet.dst_id,
                         packet.seq,
                         (unsigned int)payload_len);

    ret = gateway_route_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
#endif
#endif
}
#endif
