#line 4287 __BASE_FILE__
#define GATEWAY_CONTROL_VERBOSE_LOG(...) LOG_INF(__VA_ARGS__)

#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_discovery_assignment_next_generation(void)
{
    gateway_discovery_assignment_generation++;
    if (gateway_discovery_assignment_generation == 0u) {
        gateway_discovery_assignment_generation = 1u;
    }
    return gateway_discovery_assignment_generation;
}

static int gateway_discovery_assignment_reschedule(k_timeout_t delay,
                                                    const char *source)
{
    int ret = mesh_route_owner_work_reschedule_timeout(
        &gateway_discovery_assignment_finalize_work, delay);

    if (ret < 0) {
        LOG_ERR("gateway assignment owner scheduling failed closed: source=%s ret=%d",
                source == NULL ? "unknown" : source,
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static int gateway_discovery_assignment_wake_now(const char *source)
{
    int ret;

    /*
     * ACK-confirm is a causal boundary, so it must replace a barrier's
     * operation-deadline timer rather than merely submit an already-pending
     * delayable item. Both owners run on mesh_route_work_q, which makes this
     * cancellation and immediate rearm ordered with the confirmation RX.
     */
    (void)k_work_cancel_delayable(
        &gateway_discovery_assignment_finalize_work);
    ret = gateway_discovery_assignment_reschedule(K_NO_WAIT, source);
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_ACK_CONFIRM_WAKE ret=%d now=%u\n",
        ret,
        k_uptime_get_32());
    return ret;
}

static int gateway_build_discovery_assignment_command(
    struct mesh_outbound *outbound,
    enum discovery_assignment_phase phase,
    uint32_t epoch,
    uint32_t command_seq,
    uint16_t packet_seq)
{
    uint32_t flood_attempt_id;
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || epoch == 0u || command_seq == 0u ||
        packet_seq == 0u) {
        return -EINVAL;
    }
    /*
     * command_seq is the immutable assignment-generation identity consumed by
     * anchors.  Every bounded flood attempt needs a distinct transport
     * identity so an intermediary that forwarded an earlier CLAIM/TABLE round
     * will forward this one as well.  All redundant RF copies produced by one
     * node-comm submission retain this same flood_attempt_id.
     */
    flood_attempt_id = gateway_next_broadcast_command_seq();
    if (flood_attempt_id == 0u) {
        return -EIO;
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
                             flood_attempt_id);
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
    outbound->packet.seq = packet_seq;
    outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = gateway_command_append_default_flood_controls(outbound);
    return ret == PROTO_OK ? 0 : mesh_errno_from_proto(ret);
}

static bool gateway_discovery_assignment_abandon_active_delivery_locked(void)
{
    uint32_t handle = gateway_discovery_assignment_state.delivery_handle;
    int ret;

    if (handle == 0u) {
        return true;
    }
    ret = app_node_comm_abandon_delivery(handle);
    if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
        LOG_ERR("discovery assignment delivery cleanup failed: handle=%u ret=%d",
                handle,
                ret);
        app_watchdog_stop_feeding();
        return false;
    }
    gateway_discovery_assignment_state.delivery_handle = 0u;
    gateway_discovery_assignment_state.delivery_kind =
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE;
    gateway_discovery_assignment_state.table_delivery_is_redrive = false;
    return true;
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

static bool gateway_discovery_assignment_ack_confirm_blocks_locked(
    uint32_t session_id,
    const char *phase)
{
    uint64_t now_ms;
    uint64_t remaining_64;
    uint32_t remaining_ms;

    if (session_id == 0u ||
        !app_mesh_report_gateway_operation_confirmation_pending(
            MSG_COMMAND_RESULT, session_id, k_uptime_get_32())) {
        return false;
    }
    now_ms = (uint64_t)k_uptime_get();
    remaining_64 = gateway_discovery_assignment_state.operation_deadline_ms >
                           now_ms ?
                       gateway_discovery_assignment_state
                               .operation_deadline_ms -
                           now_ms :
                       0u;
    remaining_ms = remaining_64 > UINT32_MAX ?
                       UINT32_MAX :
                       (uint32_t)remaining_64;
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(remaining_ms), "ack-confirm-barrier");
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_ACK_CONFIRM_BARRIER phase=%s session=%u remaining_ms=%u\n",
        phase == NULL ? "unknown" : phase,
        session_id,
        remaining_ms);
    return true;
}

static void gateway_discovery_assignment_complete_success_locked(void)
{
    uint64_t committed_anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t committed_slots[UWB_DISCOVERY_SLOT_COUNT];
    struct gateway_membership_publication publication = {0};
    struct gateway_command_event table_event;
    struct gateway_command_event event;
    size_t committed_count = 0u;
    uint64_t committed_slot_mask = 0u;
    uint64_t published_slot_mask = 0u;
    uint8_t claimed_slot_span = 0u;
    uint8_t published_count;
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
    if (gateway_discovery_assignment_ack_confirm_blocks_locked(
            gateway_discovery_assignment_state.table_command_seq,
            "table-complete")) {
        return;
    }
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        uint8_t slot =
            gateway_discovery_assignment_state.anchor_slots[i];

        /*
         * TABLE has no aggregate acknowledgement.  Once the immutable table
         * has completed its planned control flood, every listed anchor owns
         * the published mapping.  Newer per-anchor responses remain useful
         * transport evidence, but an older anchor that implements the base
         * no-overall-ACK contract cannot be excluded from the durable roster.
         */
        committed_anchor_ids[committed_count] =
            gateway_discovery_assignment_state.anchor_ids[i];
        committed_slots[committed_count] = slot;
        committed_slot_mask |= UINT64_C(1) << slot;
        published_slot_mask |= UINT64_C(1) << slot;
        committed_count++;
    }
    published_count =
        (uint8_t)__builtin_popcountll(published_slot_mask);
    if (published_count != gateway_discovery_assignment_state.claim_count) {
        gateway_discovery_assignment_fail_locked(COMMAND_INTERNAL_ERROR,
                                                 EPROTO);
        return;
    }
    membership_epoch = discovery_assignment_membership_epoch(
        gateway_discovery_assignment_state.epoch);
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        uint8_t slot =
            gateway_discovery_assignment_state.anchor_slots[i];

        publication.claimed_node_ids[slot] =
            gateway_discovery_assignment_state.anchor_ids[i];
        if ((uint8_t)(slot + 1u) > claimed_slot_span) {
            claimed_slot_span = (uint8_t)(slot + 1u);
        }
    }
    publication.host_command =
        gateway_discovery_assignment_state.host_command;
    publication.committed_mask = committed_slot_mask;
    publication.acknowledged_mask = published_slot_mask;
    publication.command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication.event_gateway_epoch = event.gateway_epoch;
    publication.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    publication.claimed_count =
        (uint8_t)gateway_discovery_assignment_state.claim_count;
    publication.claimed_slot_span = claimed_slot_span;
    publication.table_round =
        gateway_discovery_assignment_state.table_round;
    publication.publish_pending = 1u;
    /*
     * Reserve the entire host-visible mapping before the irreversible roster
     * proof.  Nothing is emitted while prepared, so persistence failure can
     * still abort cleanly.  Once the proof commits, publishing this exact
     * batch is allocation-free and the operation can never report failure
     * for a roster that anchors are already allowed to activate.
     */
    publish_ret = app_gateway_assignment_publisher_prepare_table(
        &event,
        gateway_discovery_assignment_state.anchor_ids,
        gateway_discovery_assignment_state.anchor_slots,
        gateway_discovery_assignment_state.anchor_hop_counts,
        gateway_discovery_assignment_state.claim_count,
        published_slot_mask,
        gateway_discovery_assignment_state.duplicate_count);
    if (publish_ret < 0) {
        LOG_ERR("discovery assignment final telemetry staging failed: epoch=%u ret=%d",
                gateway_discovery_assignment_state.epoch,
                publish_ret);
        gateway_discovery_assignment_fail_locked(COMMAND_INTERNAL_ERROR,
                                                 (uint8_t)(-publish_ret));
        return;
    }
    membership_ret = gateway_set_registered_membership_roster(
        membership_epoch,
        committed_anchor_ids,
        committed_slots,
        committed_count,
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.table_command_seq,
        &gateway_discovery_assignment_state.table_commitment,
        &publication);
    if (membership_ret == GATEWAY_MEMBERSHIP_COMMIT_ADOPTION_PENDING) {
        /* The first durable write may have landed before its readback failed.
         * Keep this prepared batch and its operation lease intact, emit no
         * failure, and let the persistence owner adopt/replay this exact
         * semantic identity. */
        gateway_discovery_assignment_state.active = false;
        gateway_discovery_assignment_state.round_open = false;
        (void)k_work_cancel_delayable(
            &gateway_discovery_assignment_finalize_work);
        return;
    }
    if (membership_ret < 0) {
        (void)app_gateway_assignment_publisher_abort_prepared_batch(&event);
        LOG_ERR("discovery assignment membership persistence failed: assignment_epoch=%u membership_epoch=%u ret=%d",
                gateway_discovery_assignment_state.epoch,
                membership_epoch,
                membership_ret);
        gateway_discovery_assignment_fail_locked(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-membership_ret));
        return;
    }
    publish_ret =
        app_gateway_assignment_publisher_commit_prepared_batch(&event);
    if (publish_ret < 0) {
        /*
         * The durable proof is authoritative now.  Retain the active
         * operation and retry publication; emitting a failure would lie to
         * the host while ACK retries are already eligible for success.
         */
        LOG_ERR("discovery assignment committed telemetry activation failed: epoch=%u ret=%d",
                gateway_discovery_assignment_state.epoch,
                publish_ret);
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_VALIDATION_RECHECK_MS),
            "publisher-activation-retry");
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
        published_count;
    table_event.total_count =
        (uint16_t)gateway_discovery_assignment_state.claim_count;
    table_event.success_count = published_count;
    table_event.failure_count = 0u;
    table_event.status = COMMAND_OK;
    table_event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    table_event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    app_gateway_assignment_publisher_stage_table_ready(&table_event);
    gateway_emit_host_command_result_reserved(
        gateway_discovery_assignment_state.result_reservation_token,
        &gateway_discovery_assignment_state.host_command,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        COMMAND_OK,
        published_count);
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    event.progress_count =
        published_count;
    event.total_count =
        (uint16_t)gateway_discovery_assignment_state.claim_count;
    event.success_count = published_count;
    event.failure_count = 0u;
    event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    (void)gateway_observe_command_event(&event, true);
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    /* The publisher retains this exact operation lease through host send. */
    (void)k_work_cancel_delayable(
        &gateway_discovery_assignment_finalize_work);
}

static bool gateway_discovery_assignment_adoption_identity_matches_locked(
    const struct app_durable_state_gateway_assignment_identity *identity)
{
    struct gateway_command_event expected;

    if (identity == NULL ||
        gateway_discovery_assignment_state.active ||
        gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS ||
        !gateway_discovery_assignment_state.table_delivery_succeeded) {
        return false;
    }
    expected = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    return identity->correlation_id == expected.correlation_id &&
           identity->gateway_sequence == expected.gateway_sequence &&
           identity->host_session_id == expected.host_session_id &&
           identity->gateway_epoch == expected.gateway_epoch &&
           identity->host_seq == expected.host_seq;
}

int app_anchor_gateway_assignment_adopted_result_commit(
    const struct app_durable_state_gateway_assignment_identity *identity,
    uint16_t published_count)
{
    uint32_t token;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || identity == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (!gateway_discovery_assignment_adoption_identity_matches_locked(
            identity)) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -ESTALE;
    }
    token = gateway_discovery_assignment_state.result_reservation_token;
    if (token == 0u) {
        /* A previous call has already converted this same RAM reservation.
         * The durable owner may safely retry its post-readback activation. */
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return 0;
    }
    if (published_count == 0u ||
        published_count != gateway_discovery_assignment_state.claim_count) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -EPROTO;
    }

    /* Keep the exact token in the owner until queue custody succeeds.  The
     * result commit verifies the original host envelope as well, so a stale
     * adoption cannot consume another command's reservation. */
    ret = gateway_commit_host_command_result_reserved(
        token,
        &gateway_discovery_assignment_state.host_command,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        COMMAND_OK,
        (uint8_t)published_count);
    if (ret == 0) {
        gateway_discovery_assignment_state.result_reservation_token = 0u;
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret;
}

int app_anchor_gateway_assignment_publication_complete(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.active) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -EBUSY;
    }
    ret = gateway_operation_owner_release(
        &gateway_assignment_operation_lease);
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret;
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
    gateway_discovery_assignment_state.response_window_deadline_ms =
        (uint32_t)gateway_discovery_assignment_state.operation_deadline_ms;
    gateway_discovery_assignment_state.response_window_deadline_valid = true;
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
        gateway_discovery_assignment_state.claim_command_seq,
        gateway_next_command_seq());
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

static int gateway_discovery_assignment_assign_new_slots_locked(void)
{
    bool used[UWB_DISCOVERY_SLOT_COUNT] = {false};

    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.prior_anchor_count;
         i++) {
        uint8_t slot = gateway_discovery_assignment_state.anchor_slots[i];

        if (slot >= UWB_DISCOVERY_SLOT_COUNT || used[slot]) {
            return PROTO_ERR_MALFORMED;
        }
        used[slot] = true;
    }
    for (size_t i = gateway_discovery_assignment_state.prior_anchor_count;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        uint8_t slot = 0u;

        while (slot < UWB_DISCOVERY_SLOT_COUNT && used[slot]) {
            slot++;
        }
        if (slot >= UWB_DISCOVERY_SLOT_COUNT) {
            return PROTO_ERR_NO_SPACE;
        }
        gateway_discovery_assignment_state.anchor_slots[i] = slot;
        used[slot] = true;
    }
    return PROTO_OK;
}

static size_t gateway_discovery_assignment_current_claim_count_locked(void)
{
    return (size_t)__builtin_popcountll(
        gateway_discovery_assignment_state.claim_response_mask);
}

static uint32_t gateway_discovery_assignment_window_ms_locked(void)
{
    uint8_t collection_hop_count =
        app_discovery_assignment_collection_hop_count(
            gateway_discovery_assignment_state.expected_claim_count,
            gateway_discovery_assignment_current_claim_count_locked(),
            gateway_discovery_assignment_state.max_hop_count);
    uint32_t natural_window_ms;
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

    natural_window_ms = collecting_claims ?
        discovery_assignment_collection_window_ms(
            gateway_discovery_assignment_state.response_spread_ms,
            collection_hop_count) :
        discovery_assignment_table_collection_window_ms(
            gateway_discovery_assignment_state.response_spread_ms,
            collection_hop_count);

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

static uint32_t gateway_discovery_assignment_operation_origin_ms_locked(void)
{
    return (uint32_t)(
        gateway_discovery_assignment_state.operation_deadline_ms -
        gateway_discovery_assignment_state.command_budget_ms);
}

static uint64_t gateway_discovery_assignment_expand_uptime32(uint32_t uptime_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t expanded =
        (now_ms & ~UINT64_C(0xffffffff)) | uptime_ms;

    if (expanded > now_ms &&
        expanded - now_ms > UINT32_C(0x7fffffff)) {
        expanded -= UINT64_C(0x100000000);
    } else if (now_ms > expanded &&
               now_ms - expanded > UINT32_C(0x7fffffff)) {
        expanded += UINT64_C(0x100000000);
    }
    return expanded;
}

static uint32_t gateway_discovery_assignment_set_response_deadline_locked(
    uint64_t window_origin_ms,
    uint32_t requested_wait_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t deadline_ms =
        UINT64_MAX - window_origin_ms < requested_wait_ms ?
            UINT64_MAX : window_origin_ms + requested_wait_ms;
    uint64_t remaining_64;

    if (deadline_ms >
        gateway_discovery_assignment_state.operation_deadline_ms) {
        deadline_ms =
            gateway_discovery_assignment_state.operation_deadline_ms;
    }
    gateway_discovery_assignment_state.response_window_deadline_ms =
        (uint32_t)deadline_ms;
    gateway_discovery_assignment_state.response_window_deadline_valid = true;
    remaining_64 = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;
    return remaining_64 > UINT32_MAX ? UINT32_MAX :
                                      (uint32_t)remaining_64;
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
           gateway_discovery_assignment_current_claim_count_locked() >=
               gateway_discovery_assignment_state.expected_claim_count;
}

int gateway_discovery_assignment_admit_nonassignment_source(uint64_t src_id)
{
    int admitted = 0;

    if (DEVICE_ROLE != ROLE_GATEWAY || !mesh_id_is_unicast(src_id) ||
        src_id == DEVICE_ID) {
        return -EINVAL;
    }

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (!gateway_discovery_assignment_state.active) {
        admitted = 1;
    } else {
        /*
         * The prior roster was loaded durably before the operation became
         * active. Newly claimed anchors remain assignment-control-only until
         * their TABLE proof commits, so ordinary survey/bench traffic cannot
         * consume another candidate's origin partition mid-enumeration.
         */
        for (size_t i = 0u;
             i < gateway_discovery_assignment_state.prior_anchor_count;
             i++) {
            if (gateway_discovery_assignment_state.anchor_ids[i] == src_id) {
                admitted = 1;
                break;
            }
        }
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return admitted;
}

static uint32_t gateway_discovery_assignment_arm_claim_ack_settle_locked(
    uint32_t observed_at_ms)
{
    uint64_t now_ms =
        gateway_discovery_assignment_expand_uptime32(observed_at_ms);
    uint64_t latest_deadline =
        gateway_discovery_assignment_state.operation_deadline_ms;
    uint64_t candidate_deadline;
    uint64_t remaining_64;

    if (gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS ||
        !gateway_discovery_assignment_expected_claims_complete_locked()) {
        return 0u;
    }
    if (gateway_discovery_assignment_state.claim_collection_deadline_ms != 0u &&
        gateway_discovery_assignment_state.claim_collection_deadline_ms <
            latest_deadline) {
        latest_deadline =
            gateway_discovery_assignment_state.claim_collection_deadline_ms;
    }
    candidate_deadline =
        discovery_assignment_claim_ack_settle_deadline_ms(
            now_ms,
            gateway_discovery_assignment_state.max_hop_count);
    if (candidate_deadline > latest_deadline) {
        candidate_deadline = latest_deadline;
    }
    if (gateway_discovery_assignment_state.claim_ack_settle_armed &&
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms >
            candidate_deadline) {
        candidate_deadline =
            gateway_discovery_assignment_state.claim_ack_settle_deadline_ms;
        if (candidate_deadline > latest_deadline) {
            candidate_deadline = latest_deadline;
        }
    }
    gateway_discovery_assignment_state.claim_ack_settle_deadline_ms =
        candidate_deadline;
    gateway_discovery_assignment_state.claim_ack_settle_armed = true;
    if (gateway_discovery_assignment_state.response_window_deadline_valid) {
        uint64_t response_deadline =
            gateway_discovery_assignment_expand_uptime32(
                gateway_discovery_assignment_state
                    .response_window_deadline_ms);

        if (candidate_deadline > response_deadline) {
            gateway_discovery_assignment_state.response_window_deadline_ms =
                (uint32_t)candidate_deadline;
        }
    }
    if (gateway_discovery_assignment_state.claim_ack_settle_deadline_ms <=
        now_ms) {
        return 0u;
    }
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
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(wait_ms), "claim-delivery-or-admission");
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
            (uint16_t)
                gateway_discovery_assignment_current_claim_count_locked();
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
    if (!gateway_discovery_assignment_abandon_active_delivery_locked()) {
        return;
    }
    gateway_emit_host_command_result_reserved(
        gateway_discovery_assignment_state.result_reservation_token,
        &gateway_discovery_assignment_state.host_command,
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
    (void)gateway_operation_owner_release(
        &gateway_assignment_operation_lease);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_finalize_work);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_publish_work);
}

static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_gateway_assignment_publisher_diagnostics publisher_diag;
    struct app_operation_policy_candidate policy_candidate;
    struct gateway_command_options host_options;
    uint64_t prior_anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t prior_anchor_slots[UWB_DISCOVERY_SLOT_COUNT];
    uint32_t budget_ms = DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    uint32_t claim_command_seq = 0u;
    uint32_t reserved_epoch = 0u;
    uint32_t required_budget_ms = 0u;
    uint16_t expected_claim_count = 0u;
    uint16_t prior_membership_epoch = 0u;
    size_t prior_anchor_count = 0u;
    bool budget_explicit = false;
    bool expected_count_present = false;
    bool operation_claimed = false;
    int ret;

    if (host_command == NULL || payload == NULL || DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
#define GATEWAY_ASSIGNMENT_START_RETURN(value) do {                          \
        if (operation_claimed) {                                             \
            (void)gateway_operation_owner_release(                           \
                &gateway_assignment_operation_lease);                        \
        }                                                                    \
        k_mutex_unlock(&gateway_discovery_assignment_mutex);                 \
        return (value);                                                      \
    } while (false)
    ret = gateway_command_extract_options(payload, payload_len, &host_options);
    if (ret != PROTO_OK ||
        host_options.scope != CMD_SCOPE_SINGLE_NODE ||
        host_options.response_mode != CMD_RESPONSE_SMALL_RESULT ||
        host_options.flood_required ||
        host_options.collection_required) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         EINVAL);
        GATEWAY_ASSIGNMENT_START_RETURN(
            ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret));
    }
    ret = app_operation_policy_prepare_payload(payload,
                                               payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
                                               &policy_candidate);
    if (ret < 0) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         EBADMSG);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBADMSG);
    }
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
    if (policy_candidate.updates.assignment_present) {
        if ((budget_explicit &&
             budget_ms !=
                 policy_candidate.updates.assignment.operation_budget_ms) ||
            (expected_count_present &&
             expected_claim_count !=
                policy_candidate.updates.assignment.expected_anchor_count)) {
            gateway_emit_host_command_result(host_command,
                                             CMD_ASSIGN_DISCOVERY_SLOTS,
                                             COMMAND_MALFORMED_PAYLOAD,
                                             EINVAL);
            GATEWAY_ASSIGNMENT_START_RETURN(-EINVAL);
        }
        budget_ms =
            policy_candidate.updates.assignment.operation_budget_ms;
        expected_claim_count =
            policy_candidate.updates.assignment.expected_anchor_count;
        budget_explicit = true;
        expected_count_present = expected_claim_count != 0u;
    }
    ret = operation_policy_assignment_required_budget_ms(
        &policy_candidate.resolved.assignment, &required_budget_ms);
    if (ret != PROTO_OK || budget_ms < required_budget_ms) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         EINVAL);
        GATEWAY_ASSIGNMENT_START_RETURN(-EINVAL);
    }
    if (!app_gateway_control_sequence_admission_available(
            APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET)) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         3u);
        GATEWAY_ASSIGNMENT_START_RETURN(-ENOSPC);
    }
    /* A restored assignment may be semantically complete on RF yet still owe
     * its exact host publication. That is ordinary admission backpressure,
     * not a leaked live operation: reject it before claiming any new owner or
     * consuming a control identity. */
    if (gateway_assignment_publication_pending()) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         5u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    app_gateway_assignment_publisher_get_diagnostics(&publisher_diag);
    if (gateway_discovery_assignment_state.active || publisher_diag.active) {
        LOG_ERR("assignment state survived without publication debt: active=%u publisher=%u",
                gateway_discovery_assignment_state.active ? 1u : 0u,
                publisher_diag.active ? 1u : 0u);
        app_watchdog_stop_feeding();
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_INTERNAL_ERROR,
                                         EIO);
        GATEWAY_ASSIGNMENT_START_RETURN(-EIO);
    }
    if (gateway_operation_owner_claim(
            APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
            &gateway_assignment_operation_lease) < 0) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         4u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    operation_claimed = true;
    ret = gateway_get_registered_membership_roster_with_slots(
        prior_anchor_ids,
        prior_anchor_slots,
        ARRAY_SIZE(prior_anchor_ids),
        &prior_anchor_count,
        &prior_membership_epoch);
    if (ret != 0 && ret != -ENOENT) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        GATEWAY_ASSIGNMENT_START_RETURN(ret);
    }
    /*
     * The inactive-state, publisher, and durable-publication checks above prove
     * the prior assignment owner terminal. The exact durable roster can now
     * retain member histories and retire every absent origin, guaranteeing
     * P + (50 - P) partitions for this append-only enumeration.
     */
    ret = mesh_relay_reconcile_gateway_ack_membership(
        &mesh_runtime, prior_anchor_ids, prior_anchor_count);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        GATEWAY_ASSIGNMENT_START_RETURN(mesh_errno_from_proto(ret));
    }
    claim_command_seq = gateway_next_broadcast_command_seq();
    if (claim_command_seq == 0u) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_INTERNAL_ERROR,
                                         EIO);
        GATEWAY_ASSIGNMENT_START_RETURN(-EIO);
    }
    /*
     * CLAIM and TABLE belong to one semantic operation. Reusing the exact
     * durable command identity as its RFC 1982 epoch prevents an independent
     * RAM cursor from rolling an anchor back after a gateway reboot.
     */
    reserved_epoch = claim_command_seq;
    app_operation_policy_commit_prepared(&policy_candidate);
    memset(&gateway_discovery_assignment_state,
           0,
           sizeof(gateway_discovery_assignment_state));
    gateway_discovery_assignment_state.host_command = *host_command;
    gateway_discovery_assignment_state.result_reservation_token =
        gateway_command_result_get_dispatch_token();
    gateway_discovery_assignment_state.epoch = reserved_epoch;
    gateway_discovery_assignment_state.operation_deadline_ms =
        (uint64_t)k_uptime_get() + budget_ms;
    gateway_discovery_assignment_state.claim_collection_deadline_ms =
        gateway_discovery_assignment_state.operation_deadline_ms;
    gateway_discovery_assignment_state.command_budget_ms = budget_ms;
    gateway_discovery_assignment_state.response_spread_ms =
        policy_candidate.resolved.assignment.response_spread_ms;
    gateway_discovery_assignment_state.budget_explicit = budget_explicit;
    gateway_discovery_assignment_state.expected_claim_count =
        expected_count_present ? expected_claim_count : 0u;
    gateway_discovery_assignment_state.claim_count =
        prior_anchor_count;
    gateway_discovery_assignment_state.prior_anchor_count =
        (uint8_t)prior_anchor_count;
    if (prior_anchor_count != 0u) {
        memcpy(gateway_discovery_assignment_state.anchor_ids,
               prior_anchor_ids,
               prior_anchor_count * sizeof(prior_anchor_ids[0]));
        memcpy(gateway_discovery_assignment_state.anchor_slots,
               prior_anchor_slots,
               prior_anchor_count * sizeof(prior_anchor_slots[0]));
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_PRIOR_ROSTER_RETAIN epoch=%u nodes=%u membership_epoch=%u\n",
            gateway_discovery_assignment_state.epoch,
            (unsigned int)prior_anchor_count,
            prior_membership_epoch);
    }
    gateway_discovery_assignment_state.generation =
        gateway_discovery_assignment_next_generation();
    /*
     * Every CLAIM retry is an exact operation-bound replay.  Keeping one
     * identity lets an anchor retain in-flight response custody across a
     * gateway retry without its eventual result becoming stale.
     */
    gateway_discovery_assignment_state.claim_command_seq =
        claim_command_seq;
    gateway_discovery_assignment_state.stage =
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;
    gateway_discovery_assignment_state.active = true;
    operation_claimed = false;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            host_command,
            gateway_discovery_assignment_state.epoch);

        (void)gateway_observe_command_event(&event, false);
    }

    /*
     * The host-command worker has a deliberately small stack.  Publish the
     * first CLAIM through the existing assignment work boundary so packet
     * construction, flooding, and node-communication custody do not remain
     * chained to BLE command ingress.
     */
    ret = gateway_discovery_assignment_reschedule(
        K_NO_WAIT, "assignment-start");
    if (ret < 0) {
        gateway_discovery_assignment_fail_locked(
            COMMAND_INTERNAL_ERROR, (uint8_t)(-ret));
        GATEWAY_CONTROL_VERBOSE_LOG(
            "gateway discovery-slot collection handoff failed: epoch=%u ret=%d",
            reserved_epoch,
            ret);
        GATEWAY_ASSIGNMENT_START_RETURN(0);
    }
    GATEWAY_CONTROL_VERBOSE_LOG(
        "gateway discovery-slot collection started: epoch=%u expected=%u window_ms=%u",
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.expected_claim_count,
        gateway_discovery_assignment_window_ms_locked());
    GATEWAY_ASSIGNMENT_START_RETURN(0);
#undef GATEWAY_ASSIGNMENT_START_RETURN
}

static bool gateway_discovery_assignment_live_proof(
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id)
{
    if (node_id == 0u || table_commitment == NULL ||
        epoch == 0u || table_seq == 0u) {
        return false;
    }
    if (epoch != gateway_discovery_assignment_state.epoch ||
        table_seq != gateway_discovery_assignment_state.table_command_seq ||
        !discovery_assignment_table_commitment_equal(
            table_commitment,
            &gateway_discovery_assignment_state.table_commitment)) {
        return false;
    }
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        if (gateway_discovery_assignment_state.anchor_ids[i] == node_id) {
            return true;
        }
    }
    return false;
}

static int gateway_discovery_assignment_ack_proof_admission(
    const struct proto_packet *packet,
    uint32_t epoch,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    uint64_t member_ids[MESH_CONNECTED_MAX_ANCHORS];
    uint8_t member_slots[MESH_CONNECTED_MAX_ANCHORS];
    size_t member_count = 0u;
    uint16_t membership_epoch = 0u;
    int proof_ret;
    int ret;

    proof_ret = gateway_discovery_assignment_live_proof(
                    epoch,
                    packet->session_id,
                    table_commitment,
                    packet->src_id) ?
                1 : 0;
    if (proof_ret == 1) {
        /*
         * The first TABLE response is intentionally left in sender custody
         * until this proof exists. Reconcile only now, while gateway RX owns
         * mesh_runtime: the newly durable roster is authoritative, retained
         * members keep their ACK identities, and a removed member cannot hold
         * the transition partition needed by this response retry.
         */
        ret = gateway_get_registered_membership_roster_with_slots(
            member_ids,
            member_slots,
            ARRAY_SIZE(member_ids),
            &member_count,
            &membership_epoch);
        if (ret != 0) {
            return ret == -ENOENT ? -EAGAIN : ret;
        }
        ret = mesh_relay_reconcile_gateway_ack_membership(
            &mesh_runtime, member_ids, member_count);
        if (ret != PROTO_OK) {
            LOG_ERR("gateway ACK membership reconcile failed: epoch=%u members=%u ret=%d",
                    membership_epoch,
                    (unsigned int)member_count,
                    ret);
            return mesh_errno_from_proto(ret);
        }
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    return proof_ret < 0 ? proof_ret : -EAGAIN;
}

int gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t received_at_ms,
                                            uint64_t previous_hop_id)
{
    struct discovery_assignment_result assignment_result;
    enum discovery_assignment_phase phase = 0;
    uint8_t hop_count = 0u;
    uint8_t observed_hop_count = 0u;
    uint64_t hash = 0u;
    struct discovery_assignment_table_commitment table_commitment = {0};
    uint32_t epoch = 0u;
    size_t anchor_index = SIZE_MAX;
    bool ack_already_recorded = false;
    bool late_table_recovery_claim = false;
    uint8_t missing_ack_count;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT) {
        return -ENOENT;
    }
    ret = discovery_assignment_parse_result_tlvs(
        payload, payload_len, &assignment_result);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return -ENOENT;
    }
    if (ret != PROTO_OK) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_STATE_REJECT reason=envelope ret=%d src=0x%016llx\n",
                            ret,
                            (unsigned long long)packet->src_id);
        return -EBADMSG;
    }
    phase = assignment_result.phase;
    epoch = assignment_result.epoch;
    hash = assignment_result.hash;
    table_commitment = assignment_result.table_commitment;
    hop_count = assignment_result.hop_count_present ?
                assignment_result.hop_count : 0u;
    if (hop_count != 0u && hop_count <= DISCOVERY_ASSIGNMENT_MAX_HOPS) {
        observed_hop_count = hop_count;
    }
    if (packet->src_id == 0u ||
        hash != discovery_assignment_hash(packet->src_id)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u ret=%d\n",
                            (unsigned long long)packet->src_id,
                            epoch,
                            ret);
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
        if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
            ret = gateway_discovery_assignment_ack_proof_admission(
                packet, epoch, &table_commitment);
            if (ret != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE) {
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_ACK_PROOF_REJECT reason=inactive epoch=%u session=%u src=0x%016llx ret=%d\n",
                    epoch,
                    packet->session_id,
                    (unsigned long long)packet->src_id,
                    ret);
                GATEWAY_ASSIGNMENT_RETURN(ret);
            }
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=inactive phase=%u epoch=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (epoch != gateway_discovery_assignment_state.epoch) {
        if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
            ret = gateway_discovery_assignment_ack_proof_admission(
                packet, epoch, &table_commitment);
            if (ret != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE) {
                GATEWAY_ASSIGNMENT_RETURN(ret);
            }
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=epoch phase=%u received=%u expected=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            gateway_discovery_assignment_state.epoch,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (!uptime_deadline_reached(
            (uint32_t)received_at_ms,
            gateway_discovery_assignment_operation_origin_ms_locked()) ||
        uptime_deadline_reached(
            (uint32_t)received_at_ms,
            (uint32_t)gateway_discovery_assignment_state
                .operation_deadline_ms)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=deadline phase=%u epoch=%u now=%u deadline=%u src=0x%016llx\n",
                            (unsigned int)phase,
                            epoch,
                            k_uptime_get_32(),
                            (uint32_t)gateway_discovery_assignment_state.operation_deadline_ms,
                            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(
            phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ?
                gateway_discovery_assignment_ack_proof_admission(
                    packet, epoch, &table_commitment) :
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
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
            !discovery_assignment_table_commitment_equal(
                &table_commitment,
                &gateway_discovery_assignment_state.table_commitment) ||
            anchor_index == SIZE_MAX) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=ack-state src=0x%016llx epoch=%u session=%u expected=%u index=%d\n",
                                (unsigned long long)packet->src_id,
                                epoch,
                                packet->session_id,
                                gateway_discovery_assignment_state.table_command_seq,
                                anchor_index == SIZE_MAX ? -1 : (int)anchor_index);
            GATEWAY_ASSIGNMENT_RETURN(
                gateway_discovery_assignment_ack_proof_admission(
                    packet, epoch, &table_commitment));
        }
    } else {
        late_table_recovery_claim =
            gateway_discovery_assignment_state.stage ==
                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
            gateway_discovery_assignment_rf_started_locked(
                GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE) &&
            gateway_discovery_assignment_state.table_command_seq != 0u &&
            packet->session_id ==
                gateway_discovery_assignment_state.table_command_seq &&
            anchor_index != SIZE_MAX &&
            (gateway_discovery_assignment_state.ack_mask &
             (UINT64_C(1) << anchor_index)) == 0u;
        if (!late_table_recovery_claim &&
            (gateway_discovery_assignment_state.stage !=
                 GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS ||
             packet->session_id !=
                 gateway_discovery_assignment_state.claim_command_seq)) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=claim-state src=0x%016llx epoch=%u session=%u expected=%u stage=%u\n",
                                (unsigned long long)packet->src_id,
                                epoch,
                                packet->session_id,
                                gateway_discovery_assignment_state.claim_command_seq,
                                (unsigned int)gateway_discovery_assignment_state.stage);
            GATEWAY_ASSIGNMENT_RETURN(
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
        }
    }
    if (late_table_recovery_claim) {
        /*
         * A listed anchor that first heard the final TABLE has no way to know
         * the gateway's earlier CLAIM sequence. Its TABLE-correlated CLAIM is
         * an explicit request for the same immutable table. Keep the CLAIM in
         * sender custody until that table is received: ACKing it here could
         * strand the anchor if the redrive flood were also lost.
         */
        if (!gateway_discovery_assignment_state.late_table_redrive_pending) {
            gateway_discovery_assignment_state.late_table_redrive_pending =
                true;
            (void)gateway_discovery_assignment_reschedule(
                K_NO_WAIT, "late-claim-table-redrive");
        }
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_LATE_CLAIM_REDRIVE epoch=%u table_seq=%u src=0x%016llx pending=%u\n",
            epoch,
            gateway_discovery_assignment_state.table_command_seq,
            (unsigned long long)packet->src_id,
            gateway_discovery_assignment_state.late_table_redrive_pending ?
                1u : 0u);
        GATEWAY_ASSIGNMENT_RETURN(-EAGAIN);
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
    if (gateway_discovery_assignment_state
            .response_window_deadline_valid &&
        uptime_deadline_reached(
            (uint32_t)received_at_ms,
            gateway_discovery_assignment_state
                .response_window_deadline_ms)) {
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_RESULT_RETIRED reason=response-deadline phase=%u epoch=%u received=%u deadline=%u src=0x%016llx\n",
            (unsigned int)phase,
            epoch,
            (uint32_t)received_at_ms,
            gateway_discovery_assignment_state.response_window_deadline_ms,
            (unsigned long long)packet->src_id);
        GATEWAY_ASSIGNMENT_RETURN(
            phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ?
                gateway_discovery_assignment_ack_proof_admission(
                    packet, epoch, &table_commitment) :
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        anchor_index == SIZE_MAX &&
        gateway_discovery_assignment_state.claim_count >=
            ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u reason=capacity\n",
                            (unsigned long long)packet->src_id,
                            epoch);
        GATEWAY_ASSIGNMENT_RETURN(-ENOSPC);
    }
    /*
     * Bind ACK-history capacity only after every phase, epoch, session, hash,
     * membership, RF-start, and deadline check above succeeds, but before any
     * hop-depth, roster, ACK-mask, duplicate, or timer mutation. Prior roster
     * members need the same reservation as new candidates: their prior
     * assignment marker was retired when that roster became authoritative,
     * and an accepted TABLE response must never mutate semantic state only to
     * discover that its terminal gateway ACK cannot be retained.
     */
    ret = mesh_relay_reserve_gateway_ack_candidate(
        &mesh_runtime, packet->src_id, (uint32_t)received_at_ms);
    if (ret != PROTO_OK) {
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_CANDIDATE_REJECT reason=ack-capacity phase=%u epoch=%u src=0x%016llx ret=%d\n",
            (unsigned int)phase,
            epoch,
            (unsigned long long)packet->src_id,
            ret);
        GATEWAY_ASSIGNMENT_RETURN(mesh_errno_from_proto(ret));
    }
    if (hop_count == 0u ||
        hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS) {
        hop_count = DISCOVERY_ASSIGNMENT_MAX_HOPS;
    }
    if (hop_count > gateway_discovery_assignment_state.max_hop_count) {
        gateway_discovery_assignment_state.max_hop_count = hop_count;
    }

    if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        ack_already_recorded =
            (gateway_discovery_assignment_state.ack_mask &
             (UINT64_C(1) << anchor_index)) != 0u;
        /*
         * Prior roster members begin a new assignment with stable IDs and
         * slots but no borrowed route sidecar.  Their exact TABLE ACK is the
         * first reliable, operation-bound proof of the route depth that
         * actually delivered this table.  Preserve that first nonzero depth
         * for the durable mapping publisher; otherwise a successful retained
         * roster is reported to the host with hop_count=0 whenever the
         * best-effort CLAIM telemetry is delayed or dropped.
         */
        if (!ack_already_recorded && observed_hop_count != 0u) {
            gateway_discovery_assignment_state.anchor_hop_counts[
                anchor_index] = observed_hop_count;
        }
        gateway_discovery_assignment_state.ack_mask |=
            UINT64_C(1) << anchor_index;
        missing_ack_count =
            gateway_discovery_assignment_missing_ack_count_locked();
        status_debug_printf("DBG_DISCOVERY_SLOT_ACK_RX epoch=%u anchor=0x%016llx hop=%u acked=%u missing=%u\n",
                            epoch,
                            (unsigned long long)packet->src_id,
                            hop_count,
                            (unsigned int)__builtin_popcountll(
                                gateway_discovery_assignment_state.ack_mask),
                            missing_ack_count);
        if (discovery_assignment_ack_quorum_settle_should_arm(
                gateway_discovery_assignment_state
                    .response_ack_settle_armed,
                missing_ack_count)) {
            gateway_discovery_assignment_state.response_ack_settle_armed = true;
            gateway_discovery_assignment_state.response_ack_settle_deadline_ms =
                discovery_assignment_response_ack_settle_deadline_ms(
                    gateway_discovery_assignment_expand_uptime32(
                        (uint32_t)received_at_ms));
            if (gateway_discovery_assignment_state
                    .response_ack_settle_deadline_ms >
                gateway_discovery_assignment_state.operation_deadline_ms) {
                gateway_discovery_assignment_state
                    .response_ack_settle_deadline_ms =
                    gateway_discovery_assignment_state
                        .operation_deadline_ms;
            }
            if (gateway_discovery_assignment_state
                    .response_window_deadline_valid) {
                uint64_t response_deadline =
                    gateway_discovery_assignment_expand_uptime32(
                        gateway_discovery_assignment_state
                            .response_window_deadline_ms);

                if (gateway_discovery_assignment_state
                        .response_ack_settle_deadline_ms >
                    response_deadline) {
                    gateway_discovery_assignment_state
                        .response_ack_settle_deadline_ms =
                        response_deadline;
                }
            }
            (void)gateway_discovery_assignment_reschedule(
                K_NO_WAIT, "proof-validated-response");
        }
        /*
         * Acceptance into the live assignment transaction is the terminal
         * transport boundary for this TABLE response. Holding every gateway
         * ACK until full quorum deadlocks a multi-hop branch: the relay must
         * retain either its child's response or its own response in the one
         * reliable slot, so the other response can never reach the gateway.
         * Publication remains gated on the complete ACK mask and its own
         * prepared roster proof below; an exact RF retry is idempotent.
         */
        GATEWAY_ASSIGNMENT_RETURN(
            ack_already_recorded ?
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
    }

    if (anchor_index != SIZE_MAX) {
        uint64_t response_bit = UINT64_C(1) << anchor_index;
        uint32_t settle_ms =
            0u;

        if ((gateway_discovery_assignment_state.claim_response_mask &
             response_bit) == 0u) {
            struct gateway_command_event event;

            gateway_discovery_assignment_state.claim_response_mask |=
                response_bit;
            gateway_discovery_assignment_state.anchor_hop_counts[
                anchor_index] = observed_hop_count;
            event = gateway_observability_event(
                GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
                GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
                CMD_ASSIGN_DISCOVERY_SLOTS,
                &gateway_discovery_assignment_state.host_command,
                gateway_discovery_assignment_state.epoch);
            event.anchor_id = packet->src_id;
            event.previous_hop_id = observed_hop_count == 0u ?
                0u : previous_hop_id;
            event.hop_count = observed_hop_count;
            event.progress_count =
                (uint16_t)
                    gateway_discovery_assignment_current_claim_count_locked();
            event.duplicate_count =
                gateway_discovery_assignment_state.duplicate_count;
            (void)gateway_observe_command_event(&event, false);
            if (gateway_discovery_assignment_expected_claims_complete_locked()) {
                settle_ms =
                    gateway_discovery_assignment_arm_claim_ack_settle_locked(
                        (uint32_t)received_at_ms);
            }
            if (settle_ms != 0u &&
                gateway_discovery_assignment_state.round_open) {
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_CLAIM_SETTLE epoch=%u hop=%u delay=%u duplicate=0\n",
                    gateway_discovery_assignment_state.epoch,
                    gateway_discovery_assignment_state.max_hop_count,
                    settle_ms);
                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(settle_ms), "retained-claim-settle");
            }
            status_debug_printf(
                "DBG_DISCOVERY_SLOT_CLAIM_RX epoch=%u anchor=0x%016llx hash=0x%016llx hop=%u responders=%u roster=%u retained=1\n",
                epoch,
                (unsigned long long)packet->src_id,
                (unsigned long long)hash,
                hop_count,
                (unsigned int)
                    gateway_discovery_assignment_current_claim_count_locked(),
                (unsigned int)gateway_discovery_assignment_state.claim_count);
            GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
        }
        settle_ms =
            gateway_discovery_assignment_arm_claim_ack_settle_locked(
                (uint32_t)received_at_ms);

        if (gateway_discovery_assignment_state.duplicate_count < UINT16_MAX) {
            gateway_discovery_assignment_state.duplicate_count++;
        }
        if (settle_ms != 0u && gateway_discovery_assignment_state.round_open) {
            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_SETTLE epoch=%u hop=%u delay=%u duplicate=1\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.max_hop_count,
                                settle_ms);
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(settle_ms), "duplicate-claim-settle");
        }
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    gateway_discovery_assignment_state.anchor_ids[
        gateway_discovery_assignment_state.claim_count] = packet->src_id;
    gateway_discovery_assignment_state.anchor_slots[
        gateway_discovery_assignment_state.claim_count] =
            GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
    gateway_discovery_assignment_state.anchor_hop_counts[
        gateway_discovery_assignment_state.claim_count] = observed_hop_count;
    gateway_discovery_assignment_state.claim_response_mask |=
        UINT64_C(1) << gateway_discovery_assignment_state.claim_count;
    gateway_discovery_assignment_state.claim_count++;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.anchor_id = packet->src_id;
        event.previous_hop_id = observed_hop_count == 0u ?
            0u : previous_hop_id;
        event.hop_count = observed_hop_count;
        event.progress_count =
            (uint16_t)
                gateway_discovery_assignment_current_claim_count_locked();
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, false);
    }
    if (gateway_discovery_assignment_expected_claims_complete_locked()) {
        uint32_t settle_ms =
            gateway_discovery_assignment_arm_claim_ack_settle_locked(
                (uint32_t)received_at_ms);

        if (settle_ms != 0u && gateway_discovery_assignment_state.round_open) {
            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_SETTLE epoch=%u hop=%u delay=%u duplicate=0\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.max_hop_count,
                                settle_ms);
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(settle_ms), "claim-settle");
        }
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_RX epoch=%u anchor=0x%016llx hash=0x%016llx hop=%u responders=%u roster=%u retained=0\n",
                        epoch,
                        (unsigned long long)packet->src_id,
                        (unsigned long long)hash,
                        hop_count,
                        (unsigned int)
                            gateway_discovery_assignment_current_claim_count_locked(),
                        (unsigned int)gateway_discovery_assignment_state.claim_count);
    GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
#undef GATEWAY_ASSIGNMENT_RETURN
}

static int gateway_discovery_assignment_publish_table(void)
{
    struct mesh_outbound outbound;
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    size_t entry_count = 0u;
    size_t payload_len;
    uint8_t slot_count = 0u;
    bool immutable_redrive;
    int ret;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.claim_count == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -ENOENT;
    }
    immutable_redrive =
        gateway_discovery_assignment_state.late_table_redrive_pending;

    /*
     * A prior durable roster is already in slot order.  Keep that prefix
     * fixed and sort only newly claimed anchors before appending them.  This
     * prevents both transient old/new slot collisions and the prior-ACK mask
     * from referring to different nodes after an in-place whole-table sort.
     */
    ret = discovery_assignment_order_roster_extension(
        gateway_discovery_assignment_state.anchor_ids,
        gateway_discovery_assignment_state.anchor_hop_counts,
        gateway_discovery_assignment_state.claim_count,
        gateway_discovery_assignment_state.prior_anchor_count);
    if (ret == PROTO_OK) {
        ret = gateway_discovery_assignment_assign_new_slots_locked();
    }
    if (gateway_discovery_assignment_state.table_command_seq == 0u) {
        gateway_discovery_assignment_state.table_command_seq =
            gateway_next_broadcast_command_seq();
        if (gateway_discovery_assignment_state.table_command_seq == 0u) {
            ret = -EIO;
        }
    }
    if (ret == PROTO_OK &&
        gateway_discovery_assignment_state.table_packet_seq == 0u) {
        gateway_discovery_assignment_state.table_packet_seq =
            gateway_next_command_seq();
        if (gateway_discovery_assignment_state.table_packet_seq == 0u) {
            ret = -EIO;
        }
    }
    if (ret == PROTO_OK) {
        ret = gateway_build_discovery_assignment_command(
            &outbound,
            DISCOVERY_ASSIGNMENT_PHASE_TABLE,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq,
            gateway_discovery_assignment_state.table_packet_seq);
    }
    payload_len = ret == 0 ? outbound.payload_len : 0u;
    if (ret == 0) {
        for (size_t i = 0u;
             i < gateway_discovery_assignment_state.claim_count;
             i++) {
            entries[i] = (struct discovery_assignment_entry) {
                .anchor_id =
                    gateway_discovery_assignment_state.anchor_ids[i],
                .hash = discovery_assignment_hash(
                    gateway_discovery_assignment_state.anchor_ids[i]),
                .slot =
                    gateway_discovery_assignment_state.anchor_slots[i],
            };
        }
        ret = discovery_assignment_append_table_tlvs(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            entries,
            gateway_discovery_assignment_state.claim_count);
    }
    gateway_discovery_assignment_state.round_open = false;
    if (ret == PROTO_OK || ret == 0) {
        outbound.payload_len = (uint16_t)payload_len;
        outbound.packet.payload_len = (uint16_t)payload_len;
        ret = discovery_assignment_parse_table_tlvs(
            outbound.payload,
            outbound.payload_len,
            entries,
            ARRAY_SIZE(entries),
            &entry_count,
            &slot_count);
        if (ret == PROTO_OK) {
            ret = discovery_assignment_table_commitment(
                      entries,
                      entry_count,
                      slot_count,
                      &gateway_discovery_assignment_state.table_commitment) ?
                  PROTO_OK : PROTO_ERR_MALFORMED;
        }
    }
    if (ret == PROTO_OK || ret == 0) {
        ret = gateway_discovery_assignment_submit_control_flood_locked(
            &outbound,
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE,
            "discovery-slot-assignment-table");
        if (ret == 0) {
            gateway_discovery_assignment_state.table_delivery_is_redrive =
                immutable_redrive;
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
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "table-delivery-poll");
    } else if (gateway_discovery_assignment_state.late_table_redrive_pending ||
               gateway_discovery_assignment_state.table_round <
                   gateway_discovery_assignment_table_round_limit_locked()) {
        uint8_t retry_round =
            gateway_discovery_assignment_state.table_admission_retry_round;

        if (gateway_discovery_assignment_state.table_admission_retry_round <
            UINT8_MAX) {
            gateway_discovery_assignment_state.table_admission_retry_round++;
        }
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(discovery_assignment_retry_backoff_ms(
                retry_round,
                sys_rand32_get())),
            "table-admission-retry");
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret;
}

static bool gateway_discovery_assignment_boundary_ready_locked(
    uint32_t deadline_ms);

static bool gateway_discovery_assignment_service_delivery(void)
{
    struct node_comm_terminal_event event;
    enum gateway_discovery_assignment_delivery_kind kind;
    bool effective_delivered;
    bool table_delivery_is_redrive;
    bool semantic_terminal_success;
    uint32_t handle;
    uint32_t retry_ms;
    uint8_t round;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    kind = gateway_discovery_assignment_state.delivery_kind;
    table_delivery_is_redrive =
        gateway_discovery_assignment_state.table_delivery_is_redrive;
    handle = gateway_discovery_assignment_state.delivery_handle;
    if (handle == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return false;
    }
    if (!app_node_comm_peek_delivery_event_for(handle, &event)) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "delivery-terminal-poll");
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    if (event.reason != NODE_COMM_TERMINAL_DELIVERED &&
        !gateway_discovery_assignment_boundary_ready_locked(
            gateway_discovery_assignment_state
                    .response_window_deadline_valid ?
                gateway_discovery_assignment_state
                    .response_window_deadline_ms :
                (uint32_t)gateway_discovery_assignment_state
                    .operation_deadline_ms)) {
        /*
         * A D-1 CLAIM/ACK may already be physically received while the
         * control delivery terminal wins the route-owner scheduling race.
         * Leave the terminal in custody until semantic RX validation either
         * commits that packet or expires; consuming it first can retire the
         * operation and make the on-time response irrecoverably late.
         */
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    if (!app_node_comm_take_delivery_event_for(handle, &event)) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "delivery-terminal-race");
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    gateway_discovery_assignment_state.delivery_handle = 0u;
    gateway_discovery_assignment_state.delivery_kind =
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE;
    gateway_discovery_assignment_state.table_delivery_is_redrive = false;
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
            gateway_discovery_assignment_current_claim_count_locked(),
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
                            (unsigned int)
                                gateway_discovery_assignment_current_claim_count_locked(),
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
        gateway_discovery_assignment_state.table_round =
            app_discovery_assignment_table_round_after_delivery(
                gateway_discovery_assignment_state.table_round,
                event.attempts_started,
                table_delivery_is_redrive);
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
        uint64_t response_window_origin_ms = event.terminal_at_ms;
        uint32_t wait_ms;

        gateway_discovery_assignment_state.round_open = true;
        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM) {
            uint32_t full_window_ms =
                gateway_discovery_assignment_window_ms_locked();
            uint64_t collection_deadline_ms =
                UINT64_MAX - event.terminal_at_ms < full_window_ms ?
                    UINT64_MAX : event.terminal_at_ms + full_window_ms;

            if (collection_deadline_ms >
                gateway_discovery_assignment_state.operation_deadline_ms) {
                collection_deadline_ms =
                    gateway_discovery_assignment_state.operation_deadline_ms;
            }
            gateway_discovery_assignment_state.claim_collection_deadline_ms =
                collection_deadline_ms;
            if (gateway_discovery_assignment_state.claim_ack_settle_armed &&
                gateway_discovery_assignment_state
                        .claim_ack_settle_deadline_ms >
                    collection_deadline_ms) {
                gateway_discovery_assignment_state
                    .claim_ack_settle_deadline_ms = collection_deadline_ms;
            }
        }
        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM &&
            gateway_discovery_assignment_expected_claims_complete_locked()) {
            if (!gateway_discovery_assignment_state.claim_ack_settle_armed) {
                (void)gateway_discovery_assignment_arm_claim_ack_settle_locked(
                    k_uptime_get_32());
            }
            wait_ms =
                gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                    (uint64_t)k_uptime_get());
            response_window_origin_ms = (uint64_t)k_uptime_get();
        } else if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE) {
            /* TABLE completes after its planned flood. Per-anchor responses
             * are an optional extension and never gate publication. */
            wait_ms = 0u;
        } else {
            wait_ms = gateway_discovery_assignment_window_ms_locked();
        }
        wait_ms = gateway_discovery_assignment_set_response_deadline_locked(
            response_window_origin_ms, wait_ms);
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(wait_ms), "response-window");
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
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(retry_ms), "delivery-terminal-retry");
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return true;
}

static void gateway_discovery_assignment_publish_work_handler(struct k_work *work)
{
    bool current_generation;
    int ret;

    ARG_UNUSED(work);

    /*
     * This work is admitted only after the gateway-priority arbiter observes
     * the receive boundary.  Release its level-triggered abort owner before
     * the work submits the node-communication flood; otherwise every radio
     * attempt sees the stale priority abort and defers before RF starts.
     */
    dwm3000_driver_clear_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);

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
    if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        if (!gateway_discovery_assignment_boundary_ready_locked(
                (uint32_t)gateway_discovery_assignment_state
                    .operation_deadline_ms)) {
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
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
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(retry_ms), "claim-round-backoff");
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
                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(settle_ms), "claim-ack-settle");
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
        gateway_discovery_assignment_state.late_table_redrive_pending = false;
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }
    if (gateway_discovery_assignment_state.late_table_redrive_pending) {
        ret = gateway_discovery_assignment_publish_table();
        if (ret == 0) {
            gateway_discovery_assignment_state.late_table_redrive_pending =
                false;
        }
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

static bool gateway_discovery_assignment_boundary_ready_locked(
    uint32_t deadline_ms)
{
    enum gateway_command_result_validation_check validation_check =
        gateway_protocol_validation_check_interval(
            gateway_discovery_assignment_operation_origin_ms_locked(),
            deadline_ms);

    if (validation_check ==
        GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(
                GATEWAY_DISCOVERY_ASSIGNMENT_VALIDATION_RECHECK_MS),
            "validation-boundary");
        return false;
    }
    if (validation_check ==
        GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
        gateway_discovery_assignment_fail_locked(
            COMMAND_INTERNAL_ERROR, ETIMEDOUT);
        return false;
    }
    return true;
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
    if (gateway_discovery_assignment_service_delivery()) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    missing_ack_count =
        gateway_discovery_assignment_missing_ack_count_locked();
    table_round_limit =
        gateway_discovery_assignment_table_round_limit_locked();
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
        gateway_discovery_assignment_state.round_open &&
        gateway_discovery_assignment_state.table_delivery_succeeded) {
        uint32_t completion_deadline =
            gateway_discovery_assignment_state
                    .response_window_deadline_valid ?
            gateway_discovery_assignment_state.response_window_deadline_ms :
            (uint32_t)gateway_discovery_assignment_state
                .operation_deadline_ms;

        if (!gateway_discovery_assignment_boundary_ready_locked(
                completion_deadline)) {
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        {
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

                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(remaining_ms), "response-ack-settle");
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
            gateway_discovery_assignment_state.response_ack_settle_armed = false;
            gateway_discovery_assignment_state.response_ack_settle_deadline_ms = 0u;
            /*
             * Keep the TABLE round owned until completion actually commits.
             * The exact ACK-confirm barrier below may return after scheduling
             * a causal wake; clearing round_open here would make that wake
             * skip this only completion path forever.
             */
            gateway_discovery_assignment_complete_success_locked();
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
    }
    if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        if (!gateway_discovery_assignment_boundary_ready_locked(
                (uint32_t)gateway_discovery_assignment_state
                    .operation_deadline_ms)) {
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        if (gateway_discovery_assignment_state.active) {
            gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
        }
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (gateway_discovery_assignment_state.round_open &&
        gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
        gateway_discovery_assignment_state.late_table_redrive_pending) {
        /*
         * A TABLE-correlated late CLAIM is a live receiver proving that the
         * listed anchor still does not own the table. Do not strand that
         * explicit recovery request behind the original response window;
         * close this round and let the publisher immediately redrive the
         * same immutable table.
         */
        gateway_discovery_assignment_state.round_open = false;
    } else if (gateway_discovery_assignment_state.round_open) {
        bool expected_claims_settling =
            gateway_discovery_assignment_state.stage ==
                GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
            gateway_discovery_assignment_expected_claims_complete_locked();

        if (expected_claims_settling) {
            uint32_t settle_ms =
                gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                    (uint64_t)k_uptime_get());

            if (settle_ms != 0u) {
                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(settle_ms), "expected-claim-settle");
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
            if (!gateway_discovery_assignment_boundary_ready_locked(
                    (uint32_t)gateway_discovery_assignment_state
                        .claim_ack_settle_deadline_ms)) {
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
        } else if (gateway_discovery_assignment_state
                       .response_window_deadline_valid) {
            uint32_t now_ms = k_uptime_get_32();
            uint32_t deadline_ms =
                gateway_discovery_assignment_state
                    .response_window_deadline_ms;

            if (!uptime_deadline_reached(now_ms, deadline_ms)) {
                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(deadline_ms - now_ms),
                    "response-window-deadline");
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
            if (!gateway_discovery_assignment_boundary_ready_locked(
                    deadline_ms)) {
                GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
            }
        }
        if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
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
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(retry_ms), "table-round-backoff");
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        }
    }
    request = app_discovery_assignment_work_guard_request(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation);
    if (request == APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (request != APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(100u), "publish-guard-retry");
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    ret = mesh_gateway_command_priority_submit(
        &gateway_discovery_assignment_publish_work);
    app_discovery_assignment_work_guard_note_submit_result(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation,
        ret);
    if (ret < 0) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(100u), "publish-submit-retry");
    }
    GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
#undef GATEWAY_ASSIGNMENT_FINALIZE_RETURN
}

int gateway_resume_pending_assignment_publication(void)
{
    struct app_gateway_assignment_publisher_diagnostics diagnostics;

    if (!gateway_assignment_publication_pending()) {
        return 0;
    }

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.active) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -EBUSY;
    }
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    if (diagnostics.active) {
        app_gateway_assignment_publisher_pump();
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return 1;
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    /* The only replay input is the durable sparse roster proof. It must
     * reconstruct the full host-visible batch before a successor assignment
     * can pass this publication-debt gate. */
    return gateway_replay_pending_assignment_publication();
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
                                            uint64_t received_at_ms,
                                            uint64_t previous_hop_id)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(previous_hop_id);
    return -ENOTSUP;
}

int gateway_discovery_assignment_admit_nonassignment_source(uint64_t src_id)
{
    ARG_UNUSED(src_id);
    return -ENOTSUP;
}

int gateway_resume_pending_assignment_publication(void)
{
    return -ENOTSUP;
}
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
static uint8_t gateway_manual_survey_pair_target_mask(uint64_t target_id)
{
    if (!gateway_manual_survey_pair_state.valid || target_id == 0u) {
        return 0u;
    }
    if (target_id ==
        gateway_manual_survey_pair_state.pair.initiator_id) {
        return SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK;
    }
    if (target_id ==
        gateway_manual_survey_pair_state.pair.responder_id) {
        return SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    }
    return 0u;
}

static void gateway_manual_survey_pair_expire(uint32_t now_ms)
{
    if (gateway_manual_survey_pair_state.valid &&
        !gateway_manual_survey_pair_state.cleanup_requested &&
        uptime_deadline_reached(
            now_ms, gateway_manual_survey_pair_state.deadline_ms)) {
        int ret = gateway_manual_survey_pair_begin_cleanup();

        if (ret < 0) {
            LOG_ERR("manual survey expiry cleanup admission failed: ret=%d",
                    ret);
            app_watchdog_stop_feeding();
        }
    }
}

static void gateway_manual_survey_pair_arm_start_release(void)
{
    const uint8_t both_endpoints =
        SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
        SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    if (!gateway_manual_survey_pair_state.valid ||
        gateway_manual_survey_pair_state.cleanup_requested ||
        gateway_manual_survey_pair_state.started_mask != both_endpoints ||
        gateway_manual_survey_pair_state.round_id ==
            SURVEY_LEGACY_ROUND_ID ||
        gateway_manual_survey_pair_state.start_release_armed) {
        return;
    }
    if (!gateway_survey_start_release.valid) {
        return;
    }
    gateway_manual_survey_pair_state.start_release_started_at_ms =
        gateway_survey_start_release.started_at_ms;
    gateway_manual_survey_pair_state.start_release_armed = true;
}

static void gateway_manual_survey_pair_note_terminal(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status)
{
    uint8_t target_mask;

    if (command == NULL || !gateway_manual_survey_pair_state.valid ||
        gateway_manual_survey_pair_state.cleanup_requested ||
        command->session_id != survey_operation_session_id(
            gateway_manual_survey_pair_state.pair.operation_generation)) {
        return;
    }
    target_mask =
        gateway_manual_survey_pair_target_mask(command->dst_id);
    if (target_mask == 0u) {
        return;
    }

    switch (command_id) {
    case CMD_SURVEY_PREPARE_PAIR:
        gateway_manual_survey_pair_state.prepare_submitted_mask &=
            (uint8_t)~target_mask;
        if (status == COMMAND_OK) {
            gateway_manual_survey_pair_state.prepared_mask |= target_mask;
        }
        break;
    case CMD_SURVEY_START_PAIR:
        gateway_manual_survey_pair_state.start_submitted_mask &=
            (uint8_t)~target_mask;
        if (status == COMMAND_OK) {
            gateway_manual_survey_pair_state.started_mask |= target_mask;
            gateway_manual_survey_pair_arm_start_release();
        }
        break;
    case CMD_SURVEY_ABORT:
        gateway_manual_survey_pair_state.abort_submitted_mask &=
            (uint8_t)~target_mask;
        if (status == COMMAND_OK) {
            gateway_manual_survey_pair_state.aborted_mask |= target_mask;
            gateway_manual_survey_pair_state.prepare_possible_mask &=
                (uint8_t)~target_mask;
            gateway_manual_survey_pair_state.prepared_mask &=
                (uint8_t)~target_mask;
            gateway_manual_survey_pair_state.started_mask &=
                (uint8_t)~target_mask;
        }
        if (gateway_manual_survey_pair_state.prepare_possible_mask == 0u &&
            gateway_manual_survey_pair_state.abort_submitted_mask == 0u) {
            gateway_manual_survey_pair_reset();
        }
        break;
    default:
        break;
    }
}
#endif

void gateway_command_timeout_side_effects(
    const struct proto_packet *command,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum command_id command_id)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    int manual_ret;
#endif

    if (command == NULL) {
        return;
    }

    mesh_clear_route_waiting_tx(
        command,
        semantic_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC);
#if DEVICE_ROLE == ROLE_GATEWAY
    manual_ret = gateway_manual_survey_control_complete(command, command_id);
    if (manual_ret < 0) {
        return;
    }
    gateway_manual_survey_pair_note_terminal(
        command, command_id, COMMAND_TIMEOUT);
    gateway_survey_cleanup_note_command_terminal(
        command, command_id, COMMAND_TIMEOUT);
#endif
    gateway_survey_note_command_timeout(command, command_id);
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
#if DEVICE_ROLE == ROLE_GATEWAY
    int manual_ret = gateway_manual_survey_control_complete(
        command, command_id);

    if (manual_ret < 0) {
        return;
    }
    gateway_manual_survey_pair_note_terminal(command, command_id, status);
    gateway_survey_cleanup_note_command_terminal(
        command, command_id, status);
#endif
    gateway_survey_note_command_result(command, command_id, status, reason);
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
    enum gateway_command_result_validation_check validation_check =
        GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;
    enum survey_gateway_transaction_close_intent close_intent;
    bool validation_expired = false;
    uint64_t now_ms;
    uint64_t redrive_due_ms;
    uint32_t redrive_horizon_ms;
    int ret;

    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        transaction->request_delivery_terminal) {
        return;
    }
    if (!app_node_comm_peek_delivery_event_for(
            transaction->request_delivery_handle, &event)) {
        (void)gateway_survey_work_schedule(
            SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
        return;
    }
    now_ms = (uint64_t)k_uptime_get();
    close_intent = survey_gateway_transaction_close_requested(
        &gateway_survey_transaction);
    redrive_horizon_ms = gateway_survey_natural_request_timeout_ms(
        gateway_survey_transaction.active_target_id);
    redrive_due_ms = UINT64_MAX - now_ms < redrive_horizon_ms ?
        UINT64_MAX : now_ms + redrive_horizon_ms;
    if (close_intent == SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE &&
        event.reason == NODE_COMM_TERMINAL_DELIVERED &&
        !gateway_survey_result_preflight.valid &&
        redrive_horizon_ms != 0u &&
        redrive_due_ms < transaction->spec.absolute_deadline_ms) {
        ret = app_node_comm_redrive_delivered_control(
            transaction->request_delivery_handle,
            redrive_due_ms,
            transaction->spec.absolute_deadline_ms,
            &event);
        if (ret == 0) {
            ret = survey_gateway_transaction_note_delivery_redrive(
                &gateway_survey_transaction, &event, now_ms, &action);
            if (ret == 0) {
                app_stack_workload_diag_gateway_control_sample(
                    &gateway_survey_pending_command,
                    event.attempts_started,
                    1u);
                (void)gateway_survey_work_schedule(
                    SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
                    GATEWAY_SURVEY_TRANSACTION_POLL_MS);
                return;
            }
            (void)app_node_comm_cancel_delivery(
                transaction->request_delivery_handle);
        }
        if (ret != -EAGAIN && ret != -ESTALE) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                (uint8_t)(-ret),
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return;
        }
    }
    if (close_intent == SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE &&
        event.reason != NODE_COMM_TERMINAL_DELIVERED) {
        validation_check = gateway_protocol_validation_check_interval(
            gateway_survey_transaction.active_started_at_ms,
            (uint32_t)transaction->spec.absolute_deadline_ms);
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            /*
             * Keep the failed delivery terminal in communication custody.
             * A matching result physically reserved in [start,D) must decide
             * the phase before transport failure can make it late.
             */
            (void)gateway_survey_work_schedule(
                SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return;
        }
        validation_expired =
            validation_check ==
                GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED;
    }
    if (!app_node_comm_take_delivery_event_for(
            transaction->request_delivery_handle, &event)) {
        (void)gateway_survey_work_schedule(
            SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
        return;
    }
    gateway_survey_work_cancel_owner(
        SURVEY_GATEWAY_DUE_CONTROL_DELIVERY);
    ret = survey_gateway_transaction_note_delivery_terminal(
        &gateway_survey_transaction, &event, now_ms,
        &action);
    if (ret < 0) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return;
    }
    if (survey_gateway_transaction_close_requested(
            &gateway_survey_transaction) !=
            SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE) {
        if (!gateway_survey_complete_close_request()) {
            gateway_survey_close_fault(
                GATEWAY_SURVEY_CLOSE_FAULT_SERVICE, -EPROTO);
        }
        return;
    }
    if (validation_expired) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return;
    }
    app_stack_workload_diag_gateway_control_sample(
        &gateway_survey_pending_command,
        event.attempts_started,
        event.reason == NODE_COMM_TERMINAL_DELIVERED ? 1u : 0u);
    if (transaction->state == NODE_TRANSACTION_SUCCEEDED &&
        gateway_survey_result_preflight.valid &&
        gateway_survey_pending_command_valid) {
        gateway_survey_note_command_result(
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
                GATEWAY_COMMAND_EVENT_REASON_RADIO);
    }
}
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_survey_wait_for_discovery_collection(void)
{
    struct node_comm_terminal_event event;
    enum survey_gateway_collection_decision decision;
    enum gateway_command_event_reason failure_reason;
    enum gateway_command_result_validation_check validation_check;
    uint32_t now_ms;
    uint32_t operation_wait_ms;
    uint32_t receive_deadline_ms;
    uint32_t wait_ms;
    bool emission_horizon_elapsed;
    bool safety_deadline_elapsed;

    if (!gateway_survey_collection_pending) {
        return false;
    }
    if (gateway_survey_discovery_delivery_handle != 0u) {
        uint32_t handle = gateway_survey_discovery_delivery_handle;
        uint64_t now_uptime_ms;
        uint64_t redrive_due_ms;
        uint64_t latest_gateway_start_ms;
        uint64_t path_guard_ms;
        int redrive_ret;

        if (!app_node_comm_peek_delivery_event_for(handle, &event)) {
            (void)gateway_survey_work_schedule(
                SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
                GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS);
            return true;
        }
        now_uptime_ms = (uint64_t)k_uptime_get();
        path_guard_ms = (uint64_t)SURVEY_DEFAULT_TTL *
            SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS;
        latest_gateway_start_ms =
            gateway_survey_discovery_start_deadline_ms > path_guard_ms ?
                gateway_survey_discovery_start_deadline_ms - path_guard_ms :
                0u;
        redrive_due_ms = gateway_survey_discovery_origin_uptime_ms +
            SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS;
        if (redrive_due_ms < now_uptime_ms) {
            redrive_due_ms = now_uptime_ms;
        }
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED &&
            !gateway_survey_discovery_redrive_started &&
            gateway_survey_discovery_start_deadline_ms != 0u &&
            redrive_due_ms <=
                UINT64_MAX - SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS &&
            redrive_due_ms + SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS <
                latest_gateway_start_ms) {
            redrive_ret = app_node_comm_redrive_delivered_control(
                handle,
                redrive_due_ms,
                latest_gateway_start_ms,
                &event);
            if (redrive_ret == 0) {
                gateway_survey_discovery_redrive_started = true;
                status_debug_printf(
                    "DBG_SURVEY_DISCOVERY_FLOOD_REDRIVE handle=%u due=%llu latest=%llu execute=%llu attempts=%u\n",
                    handle,
                    (unsigned long long)redrive_due_ms,
                    (unsigned long long)latest_gateway_start_ms,
                    (unsigned long long)
                        gateway_survey_discovery_start_deadline_ms,
                    event.attempts_started);
                (void)gateway_survey_work_schedule(
                    SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
                    GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS);
                return true;
            }
            if (redrive_ret != -EAGAIN && redrive_ret != -ESTALE) {
                LOG_ERR("gateway survey discovery redrive failed: handle=%u ret=%d",
                        handle,
                        redrive_ret);
                app_watchdog_stop_feeding();
                return true;
            }
        }
        if (!app_node_comm_take_delivery_event_for(handle, &event)) {
            (void)gateway_survey_work_schedule(
                SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
                GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS);
            return true;
        }
        gateway_survey_discovery_delivery_handle = 0u;
        gateway_survey_discovery_origin_uptime_ms = 0u;
        gateway_survey_discovery_start_deadline_ms = 0u;
        gateway_survey_discovery_redrive_started = false;
        status_debug_printf("DBG_SURVEY_DISCOVERY_FLOOD_TERMINAL handle=%u reason=%u attempts=%u\n",
                            handle,
                            (unsigned int)event.reason,
                            event.attempts_started);
        if (!survey_gateway_discovery_collection_survives_terminal(
                event.reason == NODE_COMM_TERMINAL_DELIVERED,
                event.attempts_started)) {
            gateway_survey_collection_pending = false;
            gateway_survey_collection_started_at_ms = 0u;
            gateway_survey_discovery_origin_uptime_ms = 0u;
            gateway_survey_discovery_start_deadline_ms = 0u;
            gateway_survey_discovery_redrive_started = false;
            gateway_survey_collection_deadline_ms = 0u;
            gateway_survey_collection_finalize_cutoff_ms = 0u;
            gateway_survey_collection_finalize_cutoff_valid = false;
            gateway_survey_collection_duration_ms = 0u;
            gateway_survey_collection_window_armed = false;
            failure_reason =
                event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                    GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED ?
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                    GATEWAY_COMMAND_EVENT_REASON_RADIO;
            gateway_survey_finish_status(
                event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                    COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
                failure_reason);
            return true;
        }
        if (event.reason != NODE_COMM_TERMINAL_DELIVERED) {
            status_debug_printf(
                "DBG_SURVEY_DISCOVERY_POST_RF_TERMINAL_OVERRIDE handle=%u reason=%u attempts=%u reports=%u\n",
                handle,
                (unsigned int)event.reason,
                event.attempts_started,
                (unsigned int)gateway_survey_context.report_count);
        }
        /*
         * The absolute collection deadline was frozen at command admission.
         * Delivery completion only arms evaluation of that deadline; it
         * cannot grant a second full collection horizon.
         */
        gateway_survey_collection_window_armed = true;
    }

    now_ms = k_uptime_get_32();
    if (!gateway_survey_collection_window_armed) {
        gateway_survey_collection_pending = false;
        gateway_survey_collection_duration_ms = 0u;
        gateway_survey_finish_status(
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
        (void)gateway_survey_work_schedule(
            SURVEY_GATEWAY_DUE_BOUNDARY_POLL, wait_ms);
        return true;
    }
    receive_deadline_ms = gateway_survey_collection_receive_deadline_ms();
    validation_check = gateway_protocol_validation_check_interval(
        gateway_survey_collection_started_at_ms,
        receive_deadline_ms);
    if (validation_check == GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
        (void)gateway_survey_work_schedule(
            SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
        return true;
    }
    if (validation_check == GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
        gateway_survey_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    gateway_survey_collection_pending = false;
    gateway_survey_collection_started_at_ms = 0u;
    gateway_survey_discovery_origin_uptime_ms = 0u;
    gateway_survey_discovery_start_deadline_ms = 0u;
    gateway_survey_discovery_redrive_started = false;
    gateway_survey_collection_deadline_ms = 0u;
    gateway_survey_collection_emission_deadline_ms = 0u;
    gateway_survey_collection_finalize_cutoff_ms = 0u;
    gateway_survey_collection_finalize_cutoff_valid = false;
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
        gateway_survey_finish_status(
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
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    /* Retire only the due slots which actually woke this shared worker. */
    gateway_survey_work_consume_due();
    if (gateway_survey_consume_incarnation_event()) {
        goto out;
    }
    gateway_manual_survey_control_service();
    gateway_survey_service_active_delivery();
    if (gateway_survey_active && gateway_survey_finish_pending &&
        (gateway_survey_transaction.active.state == NODE_TRANSACTION_EMPTY ||
         gateway_survey_transaction.active.request_delivery_terminal)) {
        enum command_status finish_status =
            gateway_survey_finish_pending_status;
        enum gateway_command_event_reason finish_reason =
            gateway_survey_finish_pending_reason;

        gateway_survey_finish_pending = false;
        gateway_survey_finish_status(finish_status, finish_reason);
        goto out;
    }
    if (!gateway_survey_active) {
        if (gateway_survey_cleanup_pending() &&
            gateway_survey_cleanup_custody_owned()) {
            gateway_survey_service_cleanup();
        }
        goto out;
    }
    if (uptime_deadline_reached(k_uptime_get_32(),
                                gateway_survey_operation_deadline_ms)) {
        /*
         * Mesh RX may record exact ACK-confirm proof before D while this
         * lower-priority survey worker runs after D. Apply one such semantic
         * transition using the immutable physical receive time, then yield;
         * the already-armed immediate drive will enforce the operation
         * deadline before any successor RF work can start.
         */
        if (gateway_survey_round_active() &&
            app_gateway_survey_round_control_confirmation_received_in_interval(
                &gateway_survey_round,
                gateway_survey_operation_started_at_ms,
                gateway_survey_operation_deadline_ms)) {
            if (!gateway_survey_round_apply_control_confirmation()) {
                gateway_survey_finish_status(
                    COMMAND_INTERNAL_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            }
            goto out;
        }
        enum gateway_command_result_validation_check validation_check =
            gateway_protocol_validation_check_interval(
                gateway_survey_operation_started_at_ms,
                gateway_survey_operation_deadline_ms);

        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            (void)gateway_survey_work_schedule(
                SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            goto out;
        }
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            gateway_survey_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        } else {
            gateway_survey_finish_status(
                COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
        }
        goto out;
    }
    gateway_survey_service_cleanup();
    if (gateway_survey_wait_for_discovery_collection()) {
        goto out;
    }
    if (gateway_survey_cleanup_pending()) {
        goto out;
    }
    if (!gateway_survey_flush_boundary_event()) {
        goto out;
    }
    /*
     * A round-owned control may be waiting on the exact ACK_CONFIRM of an
     * already retired result. Let that owner consume the proof before any
     * successor control is exposed.
     */
    if (gateway_survey_round_active() && gateway_survey_round_drive()) {
        goto out;
    }
#endif
#if DEVICE_ROLE == ROLE_GATEWAY
    if (!gateway_survey_round_active()) {
        if (!gateway_survey_context.pairs_planned) {
            ret = survey_gateway_plan_pairs(&gateway_survey_context);
            if (ret != PROTO_OK) {
                LOG_WRN("gateway survey final pair planning failed: survey=%u ret=%d",
                        gateway_survey_context.survey_id,
                        ret);
                if (gateway_survey_discovery_failure_count < UINT16_MAX) {
                    gateway_survey_discovery_failure_count++;
                }
                gateway_survey_terminal_failure_reason =
                    gateway_command_survey_failure_reason_merge(
                        gateway_survey_terminal_failure_reason,
                        GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
                gateway_survey_finish();
                goto out;
            }
        }
        {
            uint32_t remaining_ms = uptime_ms_until_deadline(
                k_uptime_get_32(),
                gateway_survey_operation_deadline_ms);

            /*
             * Every pair reserves four bounded PREPARE/START custody
             * windows, even when ranging itself runs in concurrent lanes.
             * Reject an impossible plan before the first PREPARE can create
             * remote state; observation, RF retry, and cleanup time only
             * increase the required budget.
             */
            if (!survey_gateway_transaction_pair_plan_fits_minimum_budget(
                    gateway_survey_context.pair_count,
                    remaining_ms)) {
                status_debug_printf(
                    "DBG_SURVEY_PLAN_BUDGET_REJECT survey=%u pairs=%u remaining_ms=%u minimum_per_pair_ms=%u\n",
                    gateway_survey_context.survey_id,
                    (unsigned int)gateway_survey_context.pair_count,
                    remaining_ms,
                    SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS);
                gateway_survey_finish_status(
                    COMMAND_TIMEOUT,
                    GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
                goto out;
            }
        }
        /*
         * Discovery reports eagerly re-plan after every accepted report, so
         * pairs_planned is normally already true when collection closes.
         * Account graph completeness at this phase boundary, independently
         * of whether this invocation had to run the planner.  Telemetry
         * backpressure can revisit the boundary, hence the one-shot latch.
         */
        if (!gateway_survey_topology_accounted) {
            gateway_survey_topology_accounted = true;
            if (!gateway_survey_context.topology_complete) {
                if (gateway_survey_discovery_failure_count < UINT16_MAX) {
                    gateway_survey_discovery_failure_count++;
                }
                gateway_survey_terminal_failure_reason =
                    gateway_command_survey_failure_reason_merge(
                        gateway_survey_terminal_failure_reason,
                        GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE);
            }
        }
        if (!gateway_survey_emit_collection_telemetry()) {
            goto out;
        }
        GATEWAY_CONTROL_VERBOSE_LOG(
            "gateway survey orchestration starting: survey=%u reports=%u pairs=%u",
            gateway_survey_context.survey_id,
            (unsigned int)gateway_survey_context.report_count,
            (unsigned int)gateway_survey_context.pair_count);
    }
    if (gateway_survey_round_drive()) {
        goto out;
    }
#endif

out:
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_schedule_drive();
    gateway_survey_work_rearm_due();
#endif
}

static int gateway_route_survey_pair_control(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len,
                                             enum command_id command_id)
{
#if DEVICE_ROLE != ROLE_GATEWAY
    ARG_UNUSED(host_packet);
    ARG_UNUSED(host_payload);
    ARG_UNUSED(host_payload_len);
    ARG_UNUSED(command_id);
    return -ENOTSUP;
#else
    struct mesh_outbound outbound = {0};
    struct app_node_comm_reservation_lease control_reservation = {0};
    struct survey_pair pair = {0};
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t target_mask = 0u;
    size_t payload_len = 0u;
    uint64_t request_deadline_ms = 0u;
    uint64_t transaction_deadline_ms = 0u;
    uint32_t delivery_handle = 0u;
    uint32_t client_token;
    uint32_t pending_wait_reservation_token = 0u;
    uint16_t seq;
    bool control_reserved = false;
    bool created_state = false;
    bool pending_wait_reserved = false;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR &&
         command_id != CMD_SURVEY_ABORT)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }
    gateway_manual_survey_pair_expire(k_uptime_get_32());
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
    if (ret != PROTO_OK || pair.operation_generation != 0u ||
        (host_packet->dst_id != pair.initiator_id &&
         host_packet->dst_id != pair.responder_id)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == PROTO_OK ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(ret == PROTO_OK ? 2u : -ret));
        return ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret);
    }
    if (pair.sample_count != SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           4u);
        LOG_WRN("gateway survey pair control rejected: cmd=0x%04x survey=%u samples=%u required=%u",
                (unsigned int)command_id,
                pair.survey_id,
                pair.sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
        return -EINVAL;
    }

    target_mask = host_packet->dst_id == pair.initiator_id ?
        SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK :
        SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        if (!gateway_manual_survey_pair_state.valid) {
            uint64_t operation_generation = 0u;

            if (!app_gateway_control_sequence_admission_available(
                    APP_GATEWAY_CONTROL_SEQUENCE_SURVEY_ADMISSION_BUDGET)) {
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    COMMAND_BUSY,
                    9u);
                return -ENOSPC;
            }
            if (gateway_operation_owner_claim(
                    APP_GATEWAY_OPERATION_OWNER_MANUAL_SURVEY,
                    &gateway_manual_survey_operation_lease) < 0) {
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    COMMAND_BUSY,
                    8u);
                return -EBUSY;
            }
            ret = app_node_comm_reserve_bounded_control(
                gateway_manual_survey_operation_lease.generation,
                &control_reservation);
            if (ret < 0) {
                (void)gateway_operation_owner_release(
                    &gateway_manual_survey_operation_lease);
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    COMMAND_BUSY,
                    (uint8_t)(-ret));
                return ret;
            }
            control_reserved = true;
            {
                uint64_t now_ms = (uint64_t)k_uptime_get();
                uint32_t request_timeout_ms =
                    gateway_survey_natural_request_timeout_ms(
                        host_packet->dst_id);
                uint32_t result_timeout_ms =
                    SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
                uint32_t transaction_timeout_ms =
                    request_timeout_ms > UINT32_MAX - result_timeout_ms ?
                        UINT32_MAX :
                        request_timeout_ms + result_timeout_ms;

                request_deadline_ms =
                    UINT64_MAX - now_ms < request_timeout_ms ?
                        UINT64_MAX : now_ms + request_timeout_ms;
                transaction_deadline_ms =
                    UINT64_MAX - now_ms < transaction_timeout_ms ?
                        UINT64_MAX : now_ms + transaction_timeout_ms;
                ret = gateway_command_result_wait_reserve(
                    &pending_wait_reservation_token);
            }
            if (ret < 0) {
                gateway_survey_cancel_control_reservation(
                    &control_reservation);
                (void)gateway_operation_owner_release(
                    &gateway_manual_survey_operation_lease);
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    ret == -EBUSY ? COMMAND_BUSY :
                                     COMMAND_INVALID_STATE,
                    (uint8_t)(-ret));
                return ret;
            }
            pending_wait_reserved = true;
            outbound.packet.dst_id = host_packet->dst_id;
            ret = gateway_survey_prepare_pair_control(&outbound, NULL);
            if (ret < 0) {
                (void)gateway_command_result_wait_cancel(
                    pending_wait_reservation_token);
                gateway_survey_cancel_control_reservation(
                    &control_reservation);
                (void)gateway_operation_owner_release(
                    &gateway_manual_survey_operation_lease);
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    ret == -EHOSTUNREACH ? COMMAND_TIMEOUT :
                                           COMMAND_RADIO_ERROR,
                    (uint8_t)(-ret));
                return ret;
            }
            ret = gateway_durable_reserve_survey_generation(
                DEVICE_ID, &operation_generation);
            if (ret < 0) {
                (void)gateway_command_result_wait_cancel(
                    pending_wait_reservation_token);
                gateway_survey_cancel_control_reservation(
                    &control_reservation);
                (void)gateway_operation_owner_release(
                    &gateway_manual_survey_operation_lease);
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    COMMAND_INTERNAL_ERROR,
                    (uint8_t)(-ret));
                return ret;
            }
            pair.operation_generation = operation_generation;
            ret = survey_pair_control_commitment_compute(
                &pair, round_commitment);
            if (ret != PROTO_OK) {
                (void)gateway_command_result_wait_cancel(
                    pending_wait_reservation_token);
                gateway_survey_cancel_control_reservation(
                    &control_reservation);
                (void)gateway_operation_owner_release(
                    &gateway_manual_survey_operation_lease);
                gateway_emit_host_command_result(
                    host_packet,
                    command_id,
                    COMMAND_INTERNAL_ERROR,
                    (uint8_t)(-ret));
                return mesh_errno_from_proto(ret);
            }
            gateway_survey_pair_identity_reset();
            gateway_manual_survey_pair_state =
                (struct gateway_manual_survey_pair_state) {
                    .pair = pair,
                    .deadline_ms = k_uptime_get_32() +
                        SURVEY_PAIR_PREPARED_LEASE_MS,
                    .round_id = 1u,
                    .valid = true,
                };
            created_state = true;
            memcpy(
                gateway_manual_survey_pair_state.round_commitment,
                round_commitment,
                sizeof(round_commitment));
        } else if (
            gateway_manual_survey_pair_state.pair.survey_id !=
                pair.survey_id ||
            gateway_manual_survey_pair_state.pair.initiator_id !=
                pair.initiator_id ||
            gateway_manual_survey_pair_state.pair.responder_id !=
                pair.responder_id ||
            gateway_manual_survey_pair_state.pair.sample_count !=
                pair.sample_count) {
            gateway_emit_host_command_result(host_packet,
                                             command_id,
                                             COMMAND_BUSY,
                                             6u);
            return -EBUSY;
        } else if (
            (gateway_manual_survey_pair_state.prepare_submitted_mask &
             target_mask) != 0u ||
            (gateway_manual_survey_pair_state.prepared_mask &
             target_mask) != 0u) {
            gateway_emit_host_command_result(host_packet,
                                             command_id,
                                             COMMAND_BUSY,
                                             7u);
            return -EBUSY;
        } else {
            pair = gateway_manual_survey_pair_state.pair;
        }
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        if (!gateway_manual_survey_pair_state.valid ||
            gateway_manual_survey_pair_state.pair.survey_id !=
                pair.survey_id ||
            gateway_manual_survey_pair_state.pair.initiator_id !=
                pair.initiator_id ||
            gateway_manual_survey_pair_state.pair.responder_id !=
                pair.responder_id ||
            gateway_manual_survey_pair_state.pair.sample_count !=
                pair.sample_count ||
            gateway_manual_survey_pair_state.prepared_mask !=
                (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
                 SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK) ||
            (target_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK &&
             (gateway_manual_survey_pair_state.started_mask &
              SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK) == 0u) ||
            (gateway_manual_survey_pair_state.start_submitted_mask &
             target_mask) != 0u ||
            (gateway_manual_survey_pair_state.started_mask &
             target_mask) != 0u) {
            gateway_emit_host_command_result(host_packet,
                                             command_id,
                                             COMMAND_INVALID_STATE,
                                             6u);
            return -ESTALE;
        }
        pair = gateway_manual_survey_pair_state.pair;
    } else {
        if (!gateway_manual_survey_pair_state.valid ||
            gateway_manual_survey_pair_state.pair.survey_id !=
                pair.survey_id ||
            gateway_manual_survey_pair_state.pair.initiator_id !=
                pair.initiator_id ||
            gateway_manual_survey_pair_state.pair.responder_id !=
                pair.responder_id ||
            gateway_manual_survey_pair_state.pair.sample_count !=
                pair.sample_count ||
            (gateway_manual_survey_pair_state.prepare_possible_mask &
             target_mask) == 0u ||
            (gateway_manual_survey_pair_state.abort_submitted_mask &
             target_mask) != 0u) {
            gateway_emit_host_command_result(host_packet,
                                             command_id,
                                             COMMAND_INVALID_STATE,
                                             6u);
            return -ESTALE;
        }
        pair = gateway_manual_survey_pair_state.pair;
    }
    memcpy(round_commitment,
           gateway_manual_survey_pair_state.round_commitment,
           sizeof(round_commitment));

    if (command_id != CMD_SURVEY_PREPARE_PAIR) {
        ret = mesh_append_command_id(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     command_id);
    } else {
        ret = PROTO_OK;
    }
    if (ret == PROTO_OK) {
        ret = survey_append_pair_tlvs(outbound.payload,
                                      sizeof(outbound.payload),
                                      &payload_len,
                                      &pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            gateway_manual_survey_pair_state.round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            round_commitment);
    }
    if (ret != PROTO_OK) {
        if (pending_wait_reserved) {
            (void)gateway_command_result_wait_cancel(
                pending_wait_reservation_token);
        }
        if (control_reserved) {
            gateway_survey_cancel_control_reservation(
                &control_reservation);
        }
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        if (created_state) {
            gateway_manual_survey_pair_reset();
        }
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        ret = survey_init_pair_prepare_packet(&outbound.packet,
                                              &pair,
                                              DEVICE_ID,
                                              host_packet->dst_id,
                                              seq,
                                              (uint8_t)payload_len);
    } else {
        outbound.packet.msg_type = MSG_COMMAND;
        outbound.packet.flags = host_packet->flags & FLAG_DIAGNOSTIC;
        outbound.packet.src_id = DEVICE_ID;
        outbound.packet.dst_id = host_packet->dst_id;
        outbound.packet.session_id = survey_operation_session_id(
            pair.operation_generation);
        outbound.packet.seq = seq;
        outbound.packet.ttl = gateway_command_origin_ttl(command_id);
        outbound.packet.payload_len = (uint8_t)payload_len;
        ret = PROTO_OK;
    }
    if (ret != PROTO_OK) {
        if (pending_wait_reserved) {
            (void)gateway_command_result_wait_cancel(
                pending_wait_reservation_token);
        }
        if (control_reserved) {
            gateway_survey_cancel_control_reservation(
                &control_reservation);
        }
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        if (created_state) {
            gateway_manual_survey_pair_reset();
        }
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    ret = gateway_command_result_rebind_command(
        host_packet, command_id, &outbound.packet);
    if (ret < 0) {
        LOG_ERR("gateway survey result custody rebind failed: cmd=0x%04x ret=%d",
                (unsigned int)command_id,
                ret);
        gateway_emit_host_command_result(
            host_packet,
            command_id,
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret));
        if (pending_wait_reserved) {
            (void)gateway_command_result_wait_cancel(
                pending_wait_reservation_token);
        }
        if (control_reserved) {
            gateway_survey_cancel_control_reservation(
                &control_reservation);
        }
        if (created_state) {
            gateway_manual_survey_pair_reset();
        }
        return ret;
    }
    if (pending_wait_reserved) {
        ret = gateway_command_result_wait_commit(
            pending_wait_reservation_token,
            &outbound,
            command_id,
            (uint32_t)transaction_deadline_ms);
        if (ret == 0) {
            pending_wait_reserved = false;
            pending_wait_reservation_token = 0u;
        }
    } else {
        uint64_t now_ms = (uint64_t)k_uptime_get();
        uint32_t request_timeout_ms =
            gateway_survey_natural_request_timeout_ms(
                outbound.packet.dst_id);
        uint32_t result_timeout_ms =
            command_id == CMD_SURVEY_ABORT ?
                SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS :
                SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
        uint32_t transaction_timeout_ms =
            request_timeout_ms > UINT32_MAX - result_timeout_ms ?
                UINT32_MAX : request_timeout_ms + result_timeout_ms;

        request_deadline_ms =
            UINT64_MAX - now_ms < request_timeout_ms ?
                UINT64_MAX : now_ms + request_timeout_ms;
        transaction_deadline_ms =
            UINT64_MAX - now_ms < transaction_timeout_ms ?
                UINT64_MAX : now_ms + transaction_timeout_ms;
        ret = gateway_begin_command_result_wait_for(
            &outbound,
            command_id,
            transaction_timeout_ms);
    }
    client_token = ((uint32_t)command_id << 16) | seq;
    if (ret < 0) {
        if (pending_wait_reservation_token != 0u) {
            (void)gateway_command_result_wait_cancel(
                pending_wait_reservation_token);
        }
        if (control_reserved) {
            gateway_survey_cancel_control_reservation(
                &control_reservation);
        }
        gateway_emit_host_command_result(
            &outbound.packet,
            command_id,
            ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
            (uint8_t)(-ret));
        if (created_state) {
            gateway_manual_survey_pair_reset();
        }
        return ret;
    }

    if (control_reserved) {
        ret = app_node_comm_commit_bounded_control_reservation(
            &control_reservation,
            &outbound,
            request_deadline_ms,
            client_token,
            &delivery_handle);
    } else {
        ret = gateway_survey_send_pair_control(
            &outbound,
            request_deadline_ms,
            client_token,
            &delivery_handle);
    }
    if (ret < 0) {
        if (control_reserved) {
            gateway_survey_cancel_control_reservation(
                &control_reservation);
        }
        if (!gateway_clear_pending_command_result(&outbound.packet)) {
            if (created_state) {
                gateway_manual_survey_pair_reset();
            }
            return 0;
        }
        gateway_emit_host_command_result(
            &outbound.packet,
            command_id,
            ret == -EHOSTUNREACH ? COMMAND_TIMEOUT :
            ret == -EBUSY ? COMMAND_BUSY :
            COMMAND_RADIO_ERROR,
            (uint8_t)(-ret));
        if (created_state) {
            gateway_manual_survey_pair_reset();
        }
        return ret;
    }
    memset(&control_reservation, 0, sizeof(control_reservation));
    control_reserved = false;
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        gateway_manual_survey_pair_state.prepare_submitted_mask |=
            target_mask;
        gateway_manual_survey_pair_state.prepare_possible_mask |=
            target_mask;
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        gateway_manual_survey_pair_state.start_submitted_mask |=
            target_mask;
    } else {
        gateway_manual_survey_pair_state.abort_submitted_mask |=
            target_mask;
    }
    gateway_manual_survey_pair_state.control_command = outbound.packet;
    gateway_manual_survey_pair_state.control_transaction_deadline_ms =
        transaction_deadline_ms;
    gateway_manual_survey_pair_state.control_redrive_horizon_ms =
        gateway_survey_natural_request_timeout_ms(outbound.packet.dst_id);
    gateway_manual_survey_pair_state.control_delivery_handle = delivery_handle;
    gateway_manual_survey_pair_state.control_command_id = command_id;
    gateway_manual_survey_pair_state.control_target_mask = target_mask;
    gateway_manual_survey_pair_state.control_delivery_active = true;
    ret = gateway_survey_work_schedule(
        SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
        GATEWAY_SURVEY_TRANSACTION_POLL_MS);
    if (ret < 0) {
        int cleanup_ret;

        cleanup_ret = gateway_manual_survey_control_release();
        if (cleanup_ret < 0) {
            return cleanup_ret;
        }
        cleanup_ret = gateway_manual_survey_pair_begin_cleanup();
        if (cleanup_ret < 0) {
            LOG_ERR("manual survey auto-reap cleanup admission failed: ret=%d",
                    cleanup_ret);
            app_watchdog_stop_feeding();
        }
        if (!gateway_clear_pending_command_result(&outbound.packet)) {
            return 0;
        }
        gateway_emit_host_command_result(
            &outbound.packet,
            command_id,
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret));
        return ret;
    }
    return 0;
#endif
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
    case CMD_SURVEY_ABORT:
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
    enum command_id command_id = CMD_VENDOR_BASE;
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
    (void)gateway_command_extract_id(payload, payload_len, &command_id);

    orchestrator = mesh_gateway_command_orchestrator_context();
    ret = app_mesh_command_orchestrator_prepare_flood(
        orchestrator,
        DEVICE_ID,
        k_uptime_get_32(),
        packet->seq == 0u ? gateway_next_command_seq() : 0u);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected BLE host command: %d", ret);
        gateway_emit_host_command_result(packet,
                                         command_id,
                                         ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound = &orchestrator->gateway_flow.outbound;
    command_options = &orchestrator->gateway_flow.options;
    command_id = orchestrator->gateway_flow.command_id;
    tracking_mode = orchestrator->gateway_flow.tracking_mode;
    ret = gateway_command_result_rebind_command(
        packet, command_id, &outbound->packet);
    if (ret < 0) {
        LOG_ERR("gateway command result custody rebind failed: cmd=0x%04x ret=%d",
                (unsigned int)command_id,
                ret);
        gateway_emit_host_command_result(
            packet, command_id, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret));
        return ret;
    }
    if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
        ret = gateway_begin_command_result_wait_for(
            outbound,
            command_id,
            GATEWAY_COMMAND_RESULT_ROUND_TRIP_TIMEOUT_MS);
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
            GATEWAY_CONTROL_VERBOSE_LOG(
                "gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                (unsigned int)command_id,
                (unsigned long long)outbound->packet.dst_id);
            return 0;
        }
        if (gateway_host_command_send_retryable(ret)) {
            if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
                if (!gateway_clear_pending_command_result(
                        &outbound->packet)) {
                    return 0;
                }
            } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
                gateway_clear_command_collection(command_options);
            }
            GATEWAY_CONTROL_VERBOSE_LOG(
                "gateway command retained for send retry: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound->packet.dst_id,
                ret);
            return -EAGAIN;
        }
        if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
            if (!gateway_clear_pending_command_result(&outbound->packet)) {
                return 0;
            }
        } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
            gateway_clear_command_collection(command_options);
        }
        gateway_emit_host_command_result(&outbound->packet,
                                         command_id,
                                         ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                         (uint8_t)(-ret));
        return ret;
    }

    GATEWAY_CONTROL_VERBOSE_LOG(
        "gateway BLE command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
        (unsigned int)command_id,
        (unsigned long long)outbound->packet.dst_id,
        outbound->packet.session_id,
        outbound->packet.seq,
        outbound->packet.ttl);
    return 0;
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_handle_local_survey_abort(
    const struct proto_packet *packet,
    enum command_id command_id,
    uint32_t result_reservation_token)
{
    int ret;

    if (packet == NULL || packet->msg_type != MSG_COMMAND ||
        packet->dst_id != DEVICE_ID || command_id != CMD_SURVEY_ABORT) {
        return -EINVAL;
    }
    if (gateway_survey_active) {
        GATEWAY_CONTROL_VERBOSE_LOG(
            "gateway survey orchestration aborted by host: survey=%u",
            gateway_survey_context.survey_id);
        ret = gateway_survey_finish_status(
            COMMAND_DENIED, GATEWAY_COMMAND_EVENT_REASON_ABORTED);
        if (ret < 0) {
            gateway_emit_host_command_result_reserved(
                result_reservation_token,
                packet,
                command_id,
                COMMAND_INTERNAL_ERROR,
                (uint8_t)(-ret));
            gateway_observe_host_terminal(
                packet,
                command_id,
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return ret;
        }
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_manual_survey_pair_state.valid) {
        ret = gateway_manual_survey_pair_begin_cleanup();

        if (ret < 0) {
            gateway_emit_host_command_result_reserved(
                result_reservation_token,
                packet,
                command_id,
                COMMAND_INTERNAL_ERROR,
                (uint8_t)(-ret));
            gateway_observe_host_terminal(
                packet,
                command_id,
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return ret;
        }
    }
#endif
    gateway_emit_host_command_result_reserved(
        result_reservation_token,
        packet,
        command_id,
        COMMAND_OK,
        0u);
    gateway_observe_host_terminal(
        packet,
        command_id,
        COMMAND_OK,
        GATEWAY_COMMAND_EVENT_REASON_NONE);
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
            struct app_operation_policy_candidate policy_candidate;
            uint32_t command_budget_ms;
            uint32_t result_reservation_token;
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
            ret = app_operation_policy_prepare_payload(payload,
                                                       payload_len,
                                                       0u,
                                                       APP_OPERATION_POLICY_ALL_MASK,
                                                       &policy_candidate);
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
            result_reservation_token =
                gateway_command_result_get_dispatch_token();
            if (result_reservation_token == 0u) {
                ret = -EINVAL;
            } else if (gateway_route_refresh_result_token != 0u) {
                ret = -EBUSY;
            } else {
                /*
                 * The route worker may preempt this caller as soon as it is
                 * scheduled. Publish its exact terminal-result owner first,
                 * so even an inline completion can convert the reservation.
                 */
                gateway_route_refresh_result_token =
                    result_reservation_token;
                ret = app_node_comm_request_route_refresh_correlated_bounded(
                    0u,
                    "force-rediscovery-ble",
                    packet,
                    command_budget_ms);
                if (ret < 0 &&
                    gateway_route_refresh_result_token ==
                        result_reservation_token) {
                    gateway_route_refresh_result_token = 0u;
                }
            }
            if (ret == 0) {
                app_operation_policy_commit_prepared(&policy_candidate);
            }
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
        if (ret == PROTO_OK &&
            command_id == CMD_SURVEY_ABORT &&
            packet->dst_id == DEVICE_ID) {
            return gateway_handle_local_survey_abort(
                packet,
                command_id,
                gateway_command_result_get_dispatch_token());
        }
        if (ret == PROTO_OK && gateway_command_uses_survey_mesh(command_id)) {
            return gateway_route_survey_command(packet, payload, payload_len, command_id);
        }
    }

    return gateway_route_mesh_host_packet(packet, payload, payload_len);
}

#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
struct gateway_host_command_ingress_context {
    uint32_t result_reservation_token;
};

struct gateway_host_command_priority_retry_state {
    bool active;
    uint8_t attempts;
    uint32_t generation;
    uint32_t admission_cutoff;
    uint32_t started_ms;
};

static struct gateway_host_command_priority_retry_state
    gateway_host_command_priority_retry;
static uint32_t gateway_host_command_priority_retry_next_generation;

static int gateway_host_command_schedule_priority_retry(
    uint32_t admission_cutoff,
    int error);
static void gateway_host_command_submit_next_queued(void);

/*
 * Bluetooth ingress runs on the system workqueue while dispatch, retry, and
 * survey-abort cleanup run on the route-owned workqueue.  Keep the compact
 * lifecycle table serialized across those owners, but never retain this
 * spinlock across result custody, observability, message queues, or RF work.
 */
static int gateway_host_command_lifecycle_admit_locked(
    struct app_gateway_command_ingress_item *item)
{
    k_spinlock_key_t key;
    int ret;

    if (item == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&gateway_host_command_lifecycle_lock);
    gateway_host_command_next_admission_id++;
    if (gateway_host_command_next_admission_id == 0u) {
        gateway_host_command_next_admission_id = 1u;
    }
    item->admission_id = gateway_host_command_next_admission_id;
    ret = app_gateway_command_lifecycle_admit(
        &gateway_host_command_lifecycle, item);
    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_lifecycle_cancel_locked(
    const struct app_gateway_command_identity *identity)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_lifecycle_lock);
    int ret = app_gateway_command_lifecycle_cancel(
        &gateway_host_command_lifecycle,
        identity,
        NULL,
        NULL,
        NULL);

    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_lifecycle_begin_locked(
    const struct app_gateway_command_ingress_item *item,
    enum app_gateway_command_lifecycle_dispatch *dispatch)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_lifecycle_lock);
    int ret = app_gateway_command_lifecycle_begin_dispatch(
        &gateway_host_command_lifecycle, item, dispatch);

    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_lifecycle_requeue_locked(
    const struct app_gateway_command_ingress_item *item)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_lifecycle_lock);
    int ret = app_gateway_command_lifecycle_requeue_retry(
        &gateway_host_command_lifecycle, item);

    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_lifecycle_finish_locked(
    const struct app_gateway_command_ingress_item *item)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_lifecycle_lock);
    int ret = app_gateway_command_lifecycle_finish(
        &gateway_host_command_lifecycle, item);

    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_lifecycle_discard_locked(
    const struct app_gateway_command_ingress_item *item)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_lifecycle_lock);
    int ret = app_gateway_command_lifecycle_discard(
        &gateway_host_command_lifecycle, item);

    k_spin_unlock(&gateway_host_command_lifecycle_lock, key);
    return ret;
}

static int gateway_host_command_bind_result_reservation(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    const struct gateway_host_command_ingress_context *ingress = ctx;

    if (ingress == NULL || item == NULL ||
        ingress->result_reservation_token == 0u) {
        return -EINVAL;
    }
    return gateway_command_result_bind_ingress(
        ingress->result_reservation_token,
        &item->packet,
        item->command_id);
}

static int gateway_host_command_admit(
    void *ctx,
    struct app_gateway_command_ingress_item *item)
{
    const struct gateway_host_command_ingress_context *ingress = ctx;
    struct gateway_command_options options;
    uint32_t broadcast_command_seq;
    int ret;

    if (item == NULL || ingress == NULL) {
        return -EINVAL;
    }
    ret = gateway_command_extract_options(item->payload,
                                          item->payload_len,
                                          &options);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (ret == PROTO_OK && options.flood_required) {
        broadcast_command_seq = gateway_next_broadcast_command_seq();
        ret = broadcast_command_seq == 0u ? PROTO_ERR_BUSY :
            gateway_command_rebind_broadcast_sequence(
                item->payload,
                item->payload_len,
                broadcast_command_seq);
        if (ret != PROTO_OK) {
            /*
             * Ingress owns the one rejection result after admit() fails. Keep
             * its pre-reserved slot unbound so gateway_host_command_emit_result()
             * can bind the original host envelope and return COMMAND_BUSY.
             */
            return mesh_errno_from_proto(ret);
        }
    }
    ret = gateway_host_command_bind_result_reservation(ctx, item);
    if (ret < 0) {
        /*
         * Do not retire the token here. The common ingress rejection path owns
         * terminal conversion or, if the token itself is invalid, the bounded
         * fail-closed release attempt and diagnostic.
         */
        return ret;
    }
    item->result_reservation_token =
        ((const struct gateway_host_command_ingress_context *)ctx)
            ->result_reservation_token;
    ret = gateway_host_command_lifecycle_admit_locked(item);
    if (ret < 0) {
        return ret;
    }
    if (k_msgq_num_free_get(&gateway_host_command_msgq) == 0u) {
        (void)gateway_host_command_lifecycle_discard_locked(item);
        return -ENOSPC;
    }
    ret = gateway_observe_host_acceptance(&item->packet, item->command_id);
    if (ret < 0) {
        (void)gateway_host_command_lifecycle_discard_locked(item);
        return ret;
    }
    ret = k_msgq_put(&gateway_host_command_msgq, item, K_NO_WAIT);
    if (ret < 0) {
        (void)gateway_host_command_lifecycle_discard_locked(item);
        return ret;
    }
    return 0;
}

static int gateway_host_command_submit_priority(void *ctx,
                                                uint32_t admission_cutoff)
{
    int ret;

    ARG_UNUSED(ctx);

    ret = app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
        &gateway_host_command_work, admission_cutoff);
    if (ret < 0) {
        return app_gateway_command_ingress_contention_retryable(ret) ?
            gateway_host_command_schedule_priority_retry(admission_cutoff,
                                                         ret) :
            ret;
    }
    ret = mesh_gateway_command_priority_submit(&gateway_host_command_work);
    if (app_gateway_command_ingress_contention_retryable(ret)) {
        return gateway_host_command_schedule_priority_retry(admission_cutoff,
                                                           ret);
    }
    return ret;
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
    struct gateway_host_abort_item queued;
    int ret;

    if (!gateway_host_command_is_preemptive(NULL, item)) {
        return -EINVAL;
    }
    queued.packet = item->packet;
    queued.command_id = item->command_id;
    ret = gateway_host_command_bind_result_reservation(ctx, item);
    if (ret < 0) {
        /*
         * The common ingress rejection path owns the terminal conversion.
         * Retain the pre-reserved slot so it can still bind the original host
         * envelope and return COMMAND_BUSY.
         */
        return ret;
    }
    queued.result_reservation_token =
        ((const struct gateway_host_command_ingress_context *)ctx)
            ->result_reservation_token;
    if (k_msgq_num_free_get(&gateway_host_abort_msgq) == 0u) {
        return -ENOSPC;
    }
    /*
     * This is the only abort-queue producer and it runs on the serialized
     * Bluetooth RX worker.  Reserve the host-visible acceptance before queue
     * custody, so a durable event-identity failure cannot leave a runnable
     * command whose acceptance record was silently lost.  The route worker is
     * the only concurrent queue user and can only create more free capacity.
     */
    ret = gateway_observe_host_acceptance(&item->packet, item->command_id);
    if (ret < 0) {
        return ret;
    }
    ret = k_msgq_put(&gateway_host_abort_msgq, &queued, K_NO_WAIT);
    if (ret < 0) {
        LOG_ERR("gateway survey abort queue changed after serialized admission: ret=%d",
                ret);
        return ret;
    }
    ret = k_work_reschedule(&gateway_host_abort_work, K_NO_WAIT);
    if (ret < 0) {
        int fallback_ret = mesh_gateway_command_priority_submit(
            &gateway_host_abort_route_work);

        if (fallback_ret < 0) {
            LOG_ERR("gateway survey abort lost both work owners after custody: schedule=%d fallback=%d",
                    ret,
                    fallback_ret);
            app_watchdog_stop_feeding();
        }
    }
    return 0;
}

static int gateway_host_command_cancel_admitted(
    void *ctx,
    const struct app_gateway_command_identity *identity)
{
    ARG_UNUSED(ctx);

    return gateway_host_command_lifecycle_cancel_locked(identity);
}

static void gateway_host_command_emit_result(
    void *ctx,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);

    if (ctx != NULL) {
        const struct gateway_host_command_ingress_context *ingress = ctx;
        int bind_ret;

        bind_ret = gateway_command_result_bind_ingress(
            ingress->result_reservation_token,
            command,
            command_id);
        if (bind_ret == 0 || bind_ret == -EALREADY) {
            gateway_emit_host_command_result_reserved(
                ingress->result_reservation_token,
                command,
                command_id,
                status,
                reason);
        } else {
            LOG_ERR("gateway command rejection could not bind result custody: cmd=0x%04x ret=%d",
                    (unsigned int)command_id,
                    bind_ret);
            gateway_command_result_release_ingress(
                ingress->result_reservation_token);
        }
    }
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
                       status == COMMAND_MALFORMED_PAYLOAD ?
                       GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST :
                       GATEWAY_COMMAND_EVENT_REASON_INTERNAL;
        (void)gateway_observe_command_event(&event, true);
    }
}

static void gateway_host_command_note_decoded(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    ARG_UNUSED(ctx);

}

static uint32_t gateway_host_command_priority_retry_next(void)
{
    gateway_host_command_priority_retry_next_generation++;
    if (gateway_host_command_priority_retry_next_generation == 0u) {
        gateway_host_command_priority_retry_next_generation = 1u;
    }
    return gateway_host_command_priority_retry_next_generation;
}

static void gateway_host_command_priority_retry_reset(void)
{
    memset(&gateway_host_command_priority_retry, 0,
           sizeof(gateway_host_command_priority_retry));
}

static size_t gateway_host_command_retire_failed_generation(
    uint32_t generation,
    uint32_t admission_cutoff,
    int error)
{
    struct app_gateway_command_ingress_item item;
    size_t retired = 0u;
    enum command_status status =
        app_gateway_command_ingress_contention_retryable(error) ?
            COMMAND_TIMEOUT : COMMAND_INTERNAL_ERROR;
    enum gateway_command_event_reason event_reason =
        app_gateway_command_ingress_contention_retryable(error) ?
            GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL;

    while (k_msgq_peek(&gateway_host_command_msgq, &item) == 0 &&
           app_gateway_command_admission_within_cutoff(
               item.admission_id, admission_cutoff)) {
        enum app_gateway_command_lifecycle_dispatch dispatch;
        int ret;

        if (k_msgq_get(&gateway_host_command_msgq,
                       &item,
                       K_NO_WAIT) != 0) {
            break;
        }
        ret = gateway_host_command_lifecycle_begin_locked(&item, &dispatch);
        if (ret == 0 &&
            dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED) {
            continue;
        }
        gateway_emit_host_command_result_reserved(
            item.result_reservation_token,
            &item.packet,
            item.command_id,
            status,
            status == COMMAND_TIMEOUT ? ETIMEDOUT : (uint8_t)(-error));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            status,
            event_reason);
        if (ret == 0) {
            (void)gateway_host_command_lifecycle_finish_locked(&item);
        } else {
            (void)gateway_host_command_lifecycle_discard_locked(&item);
        }
        retired++;
    }
    if (retired > 0u) {
        gateway_host_command_retry_pending = false;
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
    }
    LOG_ERR("gateway command priority generation failed: generation=%u admission_cutoff=%u ret=%d retired=%u queued=%u",
            generation,
            admission_cutoff,
            error,
            (unsigned int)retired,
            k_msgq_num_used_get(&gateway_host_command_msgq));
    return retired;
}

static int gateway_host_command_schedule_priority_retry(
    uint32_t admission_cutoff,
    int error)
{
    uint32_t delay_ms;
    int ret;

    if (!app_gateway_command_ingress_contention_retryable(error) ||
        admission_cutoff == 0u) {
        return error;
    }
    if (!gateway_host_command_priority_retry.active) {
        gateway_host_command_priority_retry =
            (struct gateway_host_command_priority_retry_state) {
                .active = true,
                .generation =
                    gateway_host_command_priority_retry_next(),
                .admission_cutoff = admission_cutoff,
                .started_ms = k_uptime_get_32(),
            };
    }
    /*
     * A newer command remains behind the frozen cutoff. The existing retry
     * owner will either schedule the FIFO head or retire only that generation
     * before explicitly handing off to the newer head.
     */
    if (!app_gateway_command_admission_within_cutoff(
            admission_cutoff,
            gateway_host_command_priority_retry.admission_cutoff)) {
        return 0;
    }
    if (gateway_host_command_priority_retry.attempts >=
        GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS) {
        uint32_t generation =
            gateway_host_command_priority_retry.generation;
        uint32_t cutoff =
            gateway_host_command_priority_retry.admission_cutoff;

        gateway_host_command_priority_retry_reset();
        (void)gateway_host_command_retire_failed_generation(
            generation, cutoff, error);
        gateway_host_command_submit_next_queued();
        return 0;
    }
    delay_ms = discovery_assignment_retry_backoff_ms(
        gateway_host_command_priority_retry.attempts,
        sys_rand32_get());
    gateway_host_command_priority_retry.attempts++;
    gateway_host_command_retry_pending = true;
    ret = k_work_reschedule(&gateway_host_command_retry_work,
                            K_MSEC(delay_ms));
    if (ret < 0 &&
        !app_gateway_command_ingress_contention_retryable(ret)) {
        gateway_host_command_retry_pending = false;
        gateway_host_command_priority_retry_reset();
        return ret;
    }
    return 0;
}

static void gateway_host_command_schedule_failed(void *ctx,
                                                 int error,
                                                 uint32_t generation,
                                                 uint32_t admission_cutoff)
{

    ARG_UNUSED(ctx);
    (void)gateway_host_command_retire_failed_generation(
        generation, admission_cutoff, error);
    gateway_host_command_submit_next_queued();
}

static void gateway_host_command_retry_work_handler(struct k_work *work)
{
    struct app_gateway_command_ingress_item item;
    int ret;

    ARG_UNUSED(work);

    gateway_host_command_retry_pending = false;
    if (gateway_host_command_priority_retry.active) {
        uint32_t cutoff =
            gateway_host_command_priority_retry.admission_cutoff;

        if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0 ||
            !app_gateway_command_admission_within_cutoff(
                item.admission_id, cutoff)) {
            gateway_host_command_priority_retry_reset();
            gateway_host_command_submit_next_queued();
            return;
        }
        ret = app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
            &gateway_host_command_work, cutoff);
        if (ret == 0) {
            ret = mesh_gateway_command_priority_submit(
                &gateway_host_command_work);
        }
        if (ret >= 0) {
            gateway_host_command_priority_retry_reset();
            return;
        }
        if (app_gateway_command_ingress_contention_retryable(ret)) {
            (void)gateway_host_command_schedule_priority_retry(cutoff, ret);
            return;
        }
        {
            uint32_t generation =
                gateway_host_command_priority_retry.generation;

            gateway_host_command_priority_retry_reset();
            (void)gateway_host_command_retire_failed_generation(
                generation, cutoff, ret);
            gateway_host_command_submit_next_queued();
        }
        return;
    }
    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    ret = gateway_host_command_submit_priority(NULL, item.admission_id);
    if (ret < 0) {
        (void)gateway_host_command_retire_failed_generation(
            item.admission_id,
            item.admission_id,
            ret);
        gateway_host_command_submit_next_queued();
    }
}

static void gateway_host_command_submit_next_queued(void)
{
    struct app_gateway_command_ingress_item item;
    int ret;

    if (gateway_host_command_retry_pending ||
        k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    /*
     * Every terminal path can ask for the FIFO head to run.  Cross one
     * delayable-work boundary before attempting priority admission so a
     * permanent admission failure cannot recurse through
     * submit -> retry -> retire -> submit on the system workqueue stack.
     */
    gateway_host_command_retry_pending = true;
    ret = k_work_reschedule(&gateway_host_command_retry_work, K_NO_WAIT);
    if (ret < 0) {
        gateway_host_command_retry_pending = false;
        LOG_ERR("gateway command FIFO kick scheduling failed closed: admission=%u ret=%d",
                item.admission_id,
                ret);
        app_watchdog_stop_feeding();
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
                gateway_emit_host_command_result_reserved(
                    item.result_reservation_token,
                    &item.packet,
                    item.command_id,
                    COMMAND_INTERNAL_ERROR,
                    ENOSPC);
                gateway_observe_host_terminal(
                    &item.packet,
                    item.command_id,
                    COMMAND_INTERNAL_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
                (void)gateway_host_command_lifecycle_discard_locked(&item);
            }
            continue;
        }
        (void)gateway_host_command_lifecycle_discard_locked(&item);
        gateway_emit_host_command_result_reserved(
            item.result_reservation_token,
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
    int ret;

    ARG_UNUSED(work);
    ret = mesh_gateway_command_priority_submit(
        &gateway_host_abort_route_work);
    if (ret < 0) {
        int schedule_ret;

        LOG_WRN("gateway survey abort owner submission deferred: ret=%d",
                ret);
        schedule_ret = k_work_reschedule(
            &gateway_host_abort_work, K_MSEC(10u));
        if (schedule_ret < 0) {
            LOG_ERR("gateway survey abort retry owner scheduling failed closed: submit=%d schedule=%d",
                    ret,
                    schedule_ret);
            app_watchdog_stop_feeding();
        }
    }
}

static void gateway_host_abort_route_work_handler(struct k_work *work)
{
    struct gateway_host_abort_item item;

    ARG_UNUSED(work);
    dwm3000_driver_clear_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    (void)gateway_host_command_cancel_pending_surveys();
    while (k_msgq_get(&gateway_host_abort_msgq, &item, K_NO_WAIT) == 0) {
        int ret;

        gateway_observe_host_stage(&item.packet,
                                   item.command_id,
                                   GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING);
        ret = gateway_handle_local_survey_abort(
            &item.packet,
            item.command_id,
            item.result_reservation_token);
        if (ret < 0) {
            /*
             * The handler owns the exact reservation and emits its one
             * terminal outcome on every accepted path.  Re-emitting here
             * would consume a second event identity and can fill the retained
             * terminal backlog during a disconnect.
             */
            LOG_ERR("gateway survey abort completed with failure: ret=%d",
                    ret);
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
    dwm3000_driver_clear_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    gateway_host_command_retry_pending = false;

    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    if (mesh_id_is_unicast(item.packet.dst_id) &&
        item.packet.dst_id != DEVICE_ID &&
        app_mesh_report_gateway_origin_confirmation_pending(
            item.packet.dst_id, k_uptime_get_32())) {
        uint32_t now_ms = k_uptime_get_32();

        if (gateway_host_command_ack_confirm_wait_admission_id !=
                item.admission_id) {
            gateway_host_command_ack_confirm_wait_admission_id =
                item.admission_id;
            gateway_host_command_ack_confirm_wait_deadline_ms =
                now_ms + GATEWAY_COMMAND_BUDGET_MAX_MS;
        }
        if (!uptime_deadline_reached(
                now_ms,
                gateway_host_command_ack_confirm_wait_deadline_ms)) {
            uint32_t remaining_ms = uptime_ms_until_deadline(
                now_ms,
                gateway_host_command_ack_confirm_wait_deadline_ms);
            uint32_t delay_ms = MIN(
                remaining_ms,
                GATEWAY_HOST_COMMAND_ACK_CONFIRM_POLL_MS);

            gateway_host_command_retry_pending = true;
            ret = k_work_reschedule(
                &gateway_host_command_retry_work, K_MSEC(delay_ms));
            if (ret < 0) {
                gateway_host_command_retry_pending = false;
                LOG_ERR("gateway command ACK-confirm barrier scheduling failed closed: admission=%u ret=%d",
                        item.admission_id,
                        ret);
                app_watchdog_stop_feeding();
            }
            status_debug_printf(
                "DBG_GATEWAY_COMMAND_ACK_CONFIRM_BARRIER dst=0x%016llx cmd=0x%04x remaining_ms=%u\n",
                (unsigned long long)item.packet.dst_id,
                (unsigned int)item.command_id,
                remaining_ms);
            return;
        }

        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result_reserved(
            item.result_reservation_token,
            &item.packet,
            item.command_id,
            COMMAND_TIMEOUT,
            ETIMEDOUT);
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_TIMEOUT,
            GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
        (void)gateway_host_command_lifecycle_discard_locked(&item);
        gateway_host_command_ack_confirm_wait_admission_id = 0u;
        gateway_host_command_ack_confirm_wait_deadline_ms = 0u;
        gateway_host_command_submit_next_queued();
        return;
    }
    gateway_host_command_ack_confirm_wait_admission_id = 0u;
    gateway_host_command_ack_confirm_wait_deadline_ms = 0u;
    ret = gateway_host_command_lifecycle_begin_locked(&item, &dispatch);
    if (ret < 0) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result_reserved(
            item.result_reservation_token,
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)gateway_host_command_lifecycle_discard_locked(&item);
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
        gateway_emit_host_command_result_reserved(
            item.result_reservation_token,
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)gateway_host_command_lifecycle_finish_locked(&item);
        gateway_host_command_submit_next_queued();
        return;
    }
    /*
     * This handler can run only after the gateway-priority arbiter has either
     * scheduled it from an idle radio or observed the receive-abort boundary.
     * Bind that proof to this exact route-owned dispatch after activation.
     */
    app_mesh_command_orchestrator_mark_safe_boundary(
        mesh_gateway_command_orchestrator_context());
    gateway_observe_host_stage(&item.packet,
                               item.command_id,
                               GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING);
    gateway_command_result_set_dispatch_token(
        item.result_reservation_token);
    ret = gateway_route_host_packet(
        &item.packet, item.payload, item.payload_len);
    gateway_command_result_set_dispatch_token(0u);
    if (app_gateway_command_ingress_contention_retryable(ret)) {
        uint32_t delay_ms;

        if (gateway_host_command_retry_round == 0u) {
            gateway_host_command_retry_started_ms = k_uptime_get_32();
        }
        if (gateway_host_command_retry_round >=
            GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS) {
            gateway_emit_host_command_result_reserved(
                item.result_reservation_token,
                &item.packet,
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
            (void)gateway_host_command_lifecycle_finish_locked(&item);
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
        ret = gateway_host_command_lifecycle_requeue_locked(&item);
        if (ret < 0) {
            (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
            gateway_emit_host_command_result_reserved(
                item.result_reservation_token,
                &item.packet,
                item.command_id,
                COMMAND_INTERNAL_ERROR,
                (uint8_t)(-ret));
            (void)gateway_host_command_lifecycle_finish_locked(&item);
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
            if (app_gateway_command_ingress_contention_retryable(ret)) {
                (void)gateway_host_command_schedule_priority_retry(
                    item.admission_id, ret);
            } else {
                (void)gateway_host_command_retire_failed_generation(
                    item.admission_id,
                    item.admission_id,
                    ret);
                gateway_host_command_submit_next_queued();
            }
            return;
        }
        LOG_WRN("gateway BLE command send deferred: attempt=%u/%u delay_ms=%u",
                gateway_host_command_retry_round,
                GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS,
                delay_ms);
        return;
    }
    (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
    if (ret == 0) {
        struct gateway_command_options options;

        if (gateway_command_extract_options(item.payload,
                                            item.payload_len,
                                            &options) == PROTO_OK &&
            gateway_command_tracking_mode_from_options(&options) !=
                GATEWAY_COMMAND_TRACK_LEGACY_RESULT &&
            item.command_id != CMD_ASSIGN_DISCOVERY_SLOTS &&
            item.command_id != CMD_FORCE_REDISCOVERY) {
            /*
             * Collection and no-response commands have no later local
             * MSG_COMMAND_RESULT.  Their compact admission credit can be
             * released once routing has accepted the command.
             *
             * Assignment and route refresh are local asynchronous operations.
             * Their internally generated floods intentionally use no
             * response, but the host reservation remains owned until their
             * local terminal callback converts it into MSG_COMMAND_RESULT.
             */
            gateway_command_result_release_reserved(
                item.result_reservation_token,
                &item.packet,
                item.command_id);
        }
    }
    (void)gateway_host_command_lifecycle_finish_locked(&item);
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
bool gateway_handle_ble_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t result_reservation_token)
{
#if defined(CONFIG_IMEC_ML_CLICKER)
    ARG_UNUSED(result_reservation_token);
    ml_clicker_handle_ble_frame(frame, frame_len);
    return false;
#elif defined(CONFIG_IMEC_ML_ANCHOR)
    ARG_UNUSED(result_reservation_token);
    LOG_WRN("ML anchor BLE packet RX ignored: frame_len=%u",
            (unsigned int)frame_len);
    return false;
#else
#if DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_host_command_ingress_context ingress_context = {
        .result_reservation_token = result_reservation_token,
    };
    struct app_gateway_command_ingress_item decoded;
    const struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .gateway_id = DEVICE_ID,
        .is_preemptive = gateway_host_command_is_preemptive,
        .submit_preemptive = gateway_host_command_submit_preemptive,
        .admit = gateway_host_command_admit,
        .submit_priority = gateway_host_command_submit_priority,
        .cancel_admitted = gateway_host_command_cancel_admitted,
        .emit_result = gateway_host_command_emit_result,
        .note_decoded = gateway_host_command_note_decoded,
        .ctx = &ingress_context,
    };
    bool command_handled;
    int ret;

    /* A GUI receipt is a host-only custody boundary, never a mesh command. */
    ret = gateway_ble_accept_host_receipt_frame(frame, frame_len);
    if (ret != 0) {
        if (ret < 0) {
            LOG_WRN("gateway BLE host receipt rejected: %d", ret);
        }
        return false;
    }

    ret = app_mesh_command_orchestrator_gateway_ingress(
        &ingress_ops,
        frame,
        frame_len,
        &decoded,
        &command_handled);
    if (ret != 0) {
        if (command_handled) {
            LOG_WRN("gateway BLE command ingress failed: %d", ret);
        } else {
            LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        }
        return command_handled;
    }
    if (command_handled) {
        return true;
    }

    ret = gateway_route_host_packet(&decoded.packet,
                                    decoded.payload,
                                    decoded.payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                decoded.packet.msg_type,
                (unsigned long long)decoded.packet.dst_id,
                ret);
    }
    return false;
#else
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    ARG_UNUSED(result_reservation_token);
    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        return false;
    }

    ret = gateway_route_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
    return false;
#endif
#endif
}
#endif

#undef GATEWAY_CONTROL_VERBOSE_LOG
