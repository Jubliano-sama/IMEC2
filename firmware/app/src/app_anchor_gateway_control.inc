#line 4287 __BASE_FILE__
#define GATEWAY_CONTROL_VERBOSE_LOG(...) LOG_INF(__VA_ARGS__)

#if DEVICE_ROLE == ROLE_GATEWAY
#define GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS \
    (ROUTE_GATEWAY_ACK_TIMEOUT_MS + \
     ROUTE_RETRY_BACKOFF_FIRST_MS + \
     (ROUTE_RETRY_BACKOFF_FIRST_MS / 2u) + \
     DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS)
BUILD_ASSERT(GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS == 4700u,
             "TABLE collection must cover one lost-response retry turn");

/* Exact active delivery whose physical RF-start edge was already published. */
static uint32_t gateway_discovery_assignment_rf_event_handle;
static uint8_t gateway_discovery_assignment_adaptive_observed_hop_count;

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
    status_debug_printf("DBG_ENUM_CONFIRM ret=%d\n", ret);
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
    if (ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        ret = tlv_append_u8(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            TLV_HOP_COUNT,
            gateway_discovery_assignment_state.response_lane_max_hop_count);
    }
    if (ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_DISCOVERY_START_DELAY_MS,
                             ENUMERATION_RESPONSE_START_DELAY_MS);
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
                .ram_only_iteration =
                    gateway_discovery_assignment_state.ram_only_iteration,
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
static size_t gateway_discovery_assignment_current_claim_count_locked(void);
static uint32_t gateway_discovery_assignment_window_ms_locked(void);
static uint32_t gateway_discovery_assignment_set_response_deadline_locked(
    uint64_t window_origin_ms,
    uint32_t requested_wait_ms);
static uint64_t gateway_discovery_assignment_expand_uptime32(
    uint32_t uptime_ms);
static void gateway_discovery_assignment_fail_locked(
    enum command_status status,
    uint8_t reason);
static void gateway_discovery_assignment_finish_failure_locked(
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

static void gateway_discovery_assignment_observe_rf_start_locked(void)
{
    struct gateway_command_event event;
    uint32_t handle = gateway_discovery_assignment_state.delivery_handle;
    uint8_t attempts_started = 0u;

    if (handle == 0u ||
        handle == gateway_discovery_assignment_rf_event_handle ||
        gateway_discovery_assignment_state.delivery_kind !=
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM ||
        app_node_comm_delivery_attempts_started(handle,
                                                &attempts_started) < 0 ||
        attempts_started == 0u) {
        return;
    }

    gateway_discovery_assignment_rf_event_handle = handle;
    gateway_discovery_assignment_state.claim_rf_started = true;
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    event.attempt = gateway_discovery_assignment_state.claim_round + 1u;
    event.progress_count =
        (uint16_t)gateway_discovery_assignment_current_claim_count_locked();
    event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    (void)gateway_observe_command_event(&event, false);
    status_debug_printf(
        "DBG_ENUM_RF_BEGIN kind=%u handle=%u attempt=%u tries=%u\n",
        (unsigned int)gateway_discovery_assignment_state.delivery_kind,
        handle,
        event.attempt,
        attempts_started);
}

static int gateway_discovery_assignment_prepare_durable_table_locked(void)
{
    uint64_t committed_anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t committed_slots[UWB_DISCOVERY_SLOT_COUNT];
    struct gateway_membership_publication publication = {0};
    struct gateway_command_event event;
    size_t committed_count = 0u;
    uint64_t committed_slot_mask = 0u;
    uint64_t responder_slot_mask = 0u;
    uint64_t published_slot_mask = 0u;
    uint8_t claimed_slot_span = 0u;
    uint16_t membership_epoch;
    int membership_ret;
    int publish_ret;

    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        uint8_t slot =
            gateway_discovery_assignment_state.anchor_slots[i];

        committed_anchor_ids[committed_count] =
            gateway_discovery_assignment_state.anchor_ids[i];
        committed_slots[committed_count] = slot;
        committed_slot_mask |= UINT64_C(1) << slot;
        published_slot_mask |= UINT64_C(1) << slot;
        if ((gateway_discovery_assignment_state.claim_response_mask &
             (UINT64_C(1) << i)) != 0u) {
            responder_slot_mask |= UINT64_C(1) << slot;
        }
        committed_count++;
        publication.claimed_node_ids[slot] =
            gateway_discovery_assignment_state.anchor_ids[i];
        claimed_slot_span = survey_slot_span_include(claimed_slot_span,
                                                     slot);
    }
    if (committed_count == 0u || responder_slot_mask == 0u ||
        (uint8_t)__builtin_popcountll(published_slot_mask) !=
            gateway_discovery_assignment_state.claim_count) {
        return -EPROTO;
    }
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    publish_ret = app_gateway_assignment_publisher_prepare_table(
        &event,
        gateway_discovery_assignment_state.anchor_ids,
        gateway_discovery_assignment_state.anchor_slots,
        gateway_discovery_assignment_state.anchor_hop_counts,
        gateway_discovery_assignment_state.claim_count,
        published_slot_mask,
        gateway_discovery_assignment_state.duplicate_count);
    if (publish_ret < 0) {
        return publish_ret;
    }
    if (gateway_discovery_assignment_state.replay && publish_ret == 0) {
        publish_ret = app_gateway_assignment_publisher_mark_prepared_replay(
            &event);
        if (publish_ret < 0) {
            return publish_ret;
        }
    }
    {
        size_t first_responder =
            (size_t)__builtin_ctzll(
                gateway_discovery_assignment_state.claim_response_mask);

        if (first_responder < gateway_discovery_assignment_state.claim_count &&
            gateway_registered_membership_proves_assignment_ack(
                gateway_discovery_assignment_state.epoch,
                gateway_discovery_assignment_state.table_command_seq,
                &gateway_discovery_assignment_state.table_commitment,
                gateway_discovery_assignment_state.anchor_ids[first_responder]) ==
                1) {
            return 0;
        }
    }
    membership_epoch = discovery_assignment_membership_epoch(
        gateway_discovery_assignment_state.epoch);
    publication.host_command =
        gateway_discovery_assignment_state.host_command;
    publication.committed_mask = committed_slot_mask;
    /* Before host publication this field durably records which roster
     * members answered CLAIM and therefore owe an exact ACK_CONFIRM. */
    publication.acknowledged_mask = responder_slot_mask;
    publication.command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication.event_gateway_epoch = event.gateway_epoch;
    publication.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    publication.claimed_count =
        (uint8_t)gateway_discovery_assignment_state.claim_count;
    publication.claimed_slot_span = claimed_slot_span;
    publication.table_round =
        GATEWAY_MEMBERSHIP_TABLE_CONFIRM_PENDING | 1u;
    publication.publish_pending = 1u;
    membership_ret = gateway_discovery_assignment_state.ram_only_iteration ?
        gateway_set_registered_membership_roster_ram_only(
            membership_epoch,
            committed_anchor_ids,
            committed_slots,
            committed_count,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq,
            &gateway_discovery_assignment_state.table_commitment,
            &publication) :
        gateway_set_registered_membership_roster(
            membership_epoch,
            committed_anchor_ids,
            committed_slots,
            committed_count,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq,
            &gateway_discovery_assignment_state.table_commitment,
            &publication);
    if (membership_ret == GATEWAY_MEMBERSHIP_COMMIT_ADOPTION_PENDING) {
        return -EAGAIN;
    }
    if (membership_ret < 0) {
        return membership_ret;
    }
    return 0;
}

static void gateway_discovery_assignment_complete_success_locked(void)
{
    struct gateway_command_event table_event;
    struct gateway_command_event event;
    uint8_t published_count;
    uint16_t expected_count;
    uint16_t missing_count;
    int publish_ret;

    if (!gateway_discovery_assignment_state.active ||
        gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_END_DELIVERY ||
        !gateway_discovery_assignment_state.table_delivery_succeeded) {
        return;
    }
    published_count =
        (uint8_t)gateway_discovery_assignment_state.claim_count;
    expected_count =
        gateway_discovery_assignment_state.expected_claim_count != 0u ?
            gateway_discovery_assignment_state.expected_claim_count :
            published_count;
    missing_count = expected_count > published_count ?
        (uint16_t)(expected_count - published_count) : 0u;
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
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
        expected_count;
    table_event.success_count = published_count;
    table_event.failure_count = missing_count;
    table_event.status = COMMAND_OK;
    table_event.reason = missing_count == 0u ?
        GATEWAY_COMMAND_EVENT_REASON_NONE :
        GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    table_event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    app_gateway_assignment_publisher_stage_table_ready(&table_event);
    if (gateway_discovery_assignment_state.result_reservation_token != 0u) {
        gateway_emit_host_command_result_reserved(
            gateway_discovery_assignment_state.result_reservation_token,
            &gateway_discovery_assignment_state.host_command,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            COMMAND_OK,
            published_count);
    }
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        &gateway_discovery_assignment_state.host_command,
        gateway_discovery_assignment_state.epoch);
    event.progress_count =
        published_count;
    event.total_count =
        expected_count;
    event.success_count = published_count;
    event.failure_count = missing_count;
    event.reason = table_event.reason;
    event.duplicate_count =
        gateway_discovery_assignment_state.duplicate_count;
    (void)gateway_observe_command_event(&event, true);
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    /* The publisher retains this exact operation lease through host send. */
    if (gateway_discovery_assignment_state.replay) {
        (void)gateway_operation_owner_release(
            &gateway_assignment_operation_lease);
    }
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
    struct mesh_outbound *outbound,
    enum gateway_discovery_assignment_delivery_kind kind)
{
    enum node_comm_delivery_profile profile;
    uint32_t remaining_ms;
    uint32_t handle;
    uint64_t absolute_deadline_ms;
    int ret;

    if (outbound == NULL ||
        kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE) {
        return -EINVAL;
    }
    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE ||
        kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END) {
        outbound->flood_retry_count =
            MESH_ENUMERATION_RELAY_COPY_COUNT - 1u;
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
    absolute_deadline_ms = kind ==
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT ?
        (uint64_t)k_uptime_get() +
            GATEWAY_DISCOVERY_ASSIGNMENT_ABORT_DELIVERY_BUDGET_MS :
        discovery_assignment_control_flood_deadline_ms(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms);
    /*
     * The activation train establishes each anchor's response parent before
     * the command arrives.  Send a few real gateway CLAIM copies after that
     * train so one damaged extended frame does not make a direct anchor rely
     * on a sibling's relay. TABLE and END remain one logical
     * gateway-originated delivery each, but carry three quick physical copies
     * before anchor relays begin. ABORT stays a single-copy origin.
     */
    profile = kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM ?
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD :
        (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE ||
               kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END ||
               kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT) ?
        NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN :
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD;
    ret = app_node_comm_submit_delivery(
        outbound,
        profile,
        absolute_deadline_ms,
        ((uint32_t)CMD_ASSIGN_DISCOVERY_SLOTS << 16) ^ outbound->packet.seq,
        &handle);
    if (ret < 0) {
        return ret;
    }
    gateway_discovery_assignment_state.delivery_handle = handle;
    gateway_discovery_assignment_state.delivery_kind = kind;
    gateway_discovery_assignment_rf_event_handle = 0u;
    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM ||
        kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE) {
        gateway_discovery_assignment_adaptive_observed_hop_count = 0u;
    }
    gateway_discovery_assignment_state.response_window_deadline_ms =
        (uint32_t)gateway_discovery_assignment_state.operation_deadline_ms;
    gateway_discovery_assignment_state.response_window_deadline_valid = true;
    status_debug_printf("DBG_ENUM_FLOOD kind=%u handle=%u\n",
                        (unsigned int)kind,
                        handle);
    return 0;
}

static int gateway_send_discovery_assignment_claim_request_locked(void)
{
    struct mesh_outbound outbound;
    uint64_t claim_origin_ms;
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
    /*
     * Freeze the same origin used by the gateway lane before node-comm begins
     * its wake train. The packet header then accumulates the ordinary gateway
     * and relay delays, so the advertised countdown may legitimately be
     * negative at a deeper anchor without losing the shared start edge.
     */
    claim_origin_ms = (uint64_t)k_uptime_get();
    outbound.queued_at_ms = (uint32_t)claim_origin_ms;
    outbound.queued_at_valid = true;
    gateway_discovery_assignment_state.response_lane_start_ms =
        claim_origin_ms + ENUMERATION_RESPONSE_START_DELAY_MS;
    gateway_discovery_assignment_state.claim_collection_deadline_ms =
        gateway_discovery_assignment_state.response_lane_start_ms +
        ENUMERATION_RESPONSE_DEPTH_MS;
    if (gateway_discovery_assignment_state.claim_collection_deadline_ms >
        gateway_discovery_assignment_state.operation_deadline_ms) {
        gateway_discovery_assignment_state.claim_collection_deadline_ms =
            gateway_discovery_assignment_state.operation_deadline_ms;
    }
    gateway_discovery_assignment_state.response_lane_active = true;
    ret = gateway_discovery_assignment_submit_control_flood_locked(
        &outbound,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM);
    status_debug_printf("DBG_ENUM_CLAIM_TX epoch=%u round=%u max_hop=%u first_wait_ms=%u ceiling_ms=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.claim_round + 1u,
                        gateway_discovery_assignment_state
                            .response_lane_max_hop_count,
                        ENUMERATION_RESPONSE_DEPTH_MS,
                        enumeration_response_duration_ms(
                            gateway_discovery_assignment_state
                                .response_lane_max_hop_count),
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

static uint8_t gateway_discovery_assignment_adaptive_max_hop_count_locked(void)
{
    if (gateway_discovery_assignment_state.expected_claim_count == 0u) {
        return DISCOVERY_ASSIGNMENT_MAX_HOPS;
    }
    return (uint8_t)MIN(
        gateway_discovery_assignment_state.expected_claim_count,
        (uint16_t)DISCOVERY_ASSIGNMENT_MAX_HOPS);
}

static uint32_t gateway_discovery_assignment_propagation_hold_ms_locked(void)
{
    uint8_t hop_count = gateway_discovery_assignment_state.max_hop_count;

    if (hop_count == 0u) {
        hop_count = gateway_discovery_assignment_state
            .response_lane_max_hop_count;
    }
    hop_count = CLAMP(hop_count, 1u, UWB_ENUM_MAX_HOPS);
    return discovery_assignment_control_propagation_hold_ms(hop_count);
}

static uint8_t gateway_discovery_assignment_adaptive_slot_count_locked(void)
{
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS) {
        return gateway_discovery_assignment_state.expected_claim_count == 0u ?
            UWB_DISCOVERY_SLOT_COUNT :
            (uint8_t)gateway_discovery_assignment_state.expected_claim_count;
    }

    uint8_t slot_span = 0u;

    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        uint8_t slot = gateway_discovery_assignment_state.anchor_slots[i];

        if (slot < UWB_DISCOVERY_SLOT_COUNT && slot + 1u > slot_span) {
            slot_span = slot + 1u;
        }
    }
    return slot_span == 0u ? 1u : slot_span;
}

static uint32_t
gateway_discovery_assignment_adaptive_deadline_offset_ms_locked(
    uint8_t observed_hop_count,
    uint8_t max_hop_count)
{
    uint32_t deadline_offset_ms = 0u;

    if (discovery_assignment_adaptive_depth_deadline_offset_ms(
            gateway_discovery_assignment_state.response_spread_ms,
            gateway_discovery_assignment_adaptive_slot_count_locked(),
            observed_hop_count,
            max_hop_count,
            &deadline_offset_ms) != PROTO_OK) {
        return gateway_discovery_assignment_window_ms_locked();
    }
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
        deadline_offset_ms = MAX(
            deadline_offset_ms,
            GATEWAY_DISCOVERY_ASSIGNMENT_TABLE_RETRY_RECOVERY_MS);
    }
    return deadline_offset_ms;
}

static void gateway_discovery_assignment_refresh_adaptive_deadline_locked(
    uint64_t received_at_ms,
    uint8_t observed_hop_count)
{
    uint8_t max_hop_count =
        gateway_discovery_assignment_adaptive_max_hop_count_locked();
    uint32_t deadline_offset_ms;
    uint32_t remaining_ms;

    if (observed_hop_count == 0u ||
        observed_hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS) {
        return;
    }
    if (observed_hop_count >
        gateway_discovery_assignment_adaptive_observed_hop_count) {
        gateway_discovery_assignment_adaptive_observed_hop_count =
            observed_hop_count;
    }
    if (gateway_discovery_assignment_adaptive_observed_hop_count >
        max_hop_count) {
        gateway_discovery_assignment_adaptive_observed_hop_count =
            max_hop_count;
    }
    deadline_offset_ms =
        gateway_discovery_assignment_adaptive_deadline_offset_ms_locked(
        gateway_discovery_assignment_adaptive_observed_hop_count,
        max_hop_count);
    remaining_ms = gateway_discovery_assignment_set_response_deadline_locked(
        gateway_discovery_assignment_state.response_window_origin_ms != 0u ?
            gateway_discovery_assignment_state.response_window_origin_ms :
            gateway_discovery_assignment_expand_uptime32(
                (uint32_t)received_at_ms),
        deadline_offset_ms);
    status_debug_printf(
        "DBG_ENUM_ADAPTIVE_DEADLINE stage=%u observed_hop=%u next_hop=%u offset=%u remaining=%u deadline=%u\n",
        (unsigned int)gateway_discovery_assignment_state.stage,
        observed_hop_count,
        gateway_discovery_assignment_adaptive_observed_hop_count <
                max_hop_count ?
            gateway_discovery_assignment_adaptive_observed_hop_count + 1u :
            max_hop_count,
        deadline_offset_ms,
        remaining_ms,
        gateway_discovery_assignment_state.response_window_deadline_ms);
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(remaining_ms), "adaptive-response-deadline");
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
    uint8_t first_contact_slot_count = UWB_DISCOVERY_SLOT_COUNT;

    if (collecting_claims) {
        if (gateway_discovery_assignment_state.expected_claim_count != 0u) {
            first_contact_slot_count =
                (uint8_t)gateway_discovery_assignment_state
                    .expected_claim_count;
        }
    } else if (gateway_discovery_assignment_state.claim_count != 0u) {
        uint8_t slot_span = 0u;

        for (size_t i = 0u;
             i < gateway_discovery_assignment_state.claim_count;
             i++) {
            uint8_t slot =
                gateway_discovery_assignment_state.anchor_slots[i];

            if (slot < UWB_DISCOVERY_SLOT_COUNT && slot + 1u > slot_span) {
                slot_span = slot + 1u;
            }
        }
        if (slot_span != 0u) {
            first_contact_slot_count = slot_span;
        }
    }

    natural_window_ms = collecting_claims ?
        discovery_assignment_collection_window_for_topology_ms(
            gateway_discovery_assignment_state.response_spread_ms,
            first_contact_slot_count,
            collection_hop_count) :
        discovery_assignment_table_collection_window_for_topology_ms(
            gateway_discovery_assignment_state.response_spread_ms,
            first_contact_slot_count,
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
    return gateway_discovery_assignment_state.claim_response_mask;
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

static bool gateway_discovery_assignment_expected_claims_missing_locked(void)
{
    return gateway_discovery_assignment_state.expected_claim_count != 0u &&
           gateway_discovery_assignment_current_claim_count_locked() != 0u &&
           gateway_discovery_assignment_current_claim_count_locked() <
               gateway_discovery_assignment_state.expected_claim_count;
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
    candidate_deadline = discovery_assignment_claim_ack_settle_deadline_ms(
        now_ms, gateway_discovery_assignment_state.max_hop_count);
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
    if (candidate_deadline <= now_ms) {
        return 0u;
    }
    remaining_64 = candidate_deadline - now_ms;
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
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms -
        now_ms;
    return remaining_64 > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_64;
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
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return admitted;
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
    if (ret != 0) {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.attempt = gateway_discovery_assignment_state.claim_round + 1u;
        event.progress_count =
            (uint16_t)
                gateway_discovery_assignment_current_claim_count_locked();
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        event.status = COMMAND_RADIO_ERROR;
        event.reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
        (void)gateway_observe_command_event(&event, false);
    }
    return ret;
}

static int gateway_discovery_assignment_publish_abort_locked(void)
{
    struct discovery_assignment_abort_identity identity;
    struct mesh_outbound outbound;
    size_t payload_len;
    int ret;

    if (!gateway_discovery_assignment_state.active ||
        !gateway_discovery_assignment_state.claim_rf_started ||
        gateway_discovery_assignment_state.claim_command_seq == 0u) {
        return -EINVAL;
    }
    if (gateway_discovery_assignment_state.abort_command_seq == 0u) {
        gateway_discovery_assignment_state.abort_command_seq =
            gateway_next_broadcast_command_seq();
    }
    if (gateway_discovery_assignment_state.abort_packet_seq == 0u) {
        gateway_discovery_assignment_state.abort_packet_seq =
            gateway_next_command_seq();
    }
    if (gateway_discovery_assignment_state.abort_command_seq == 0u ||
        gateway_discovery_assignment_state.abort_packet_seq == 0u) {
        return -EIO;
    }
    ret = gateway_build_discovery_assignment_command(
        &outbound,
        DISCOVERY_ASSIGNMENT_PHASE_ABORT,
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.abort_command_seq,
        gateway_discovery_assignment_state.abort_packet_seq);
    if (ret < 0) {
        return ret;
    }
    identity = (struct discovery_assignment_abort_identity) {
        .epoch = gateway_discovery_assignment_state.epoch,
        .claim_session_id =
            gateway_discovery_assignment_state.claim_command_seq,
        .claim_command_seq =
            gateway_discovery_assignment_state.claim_command_seq,
    };
    payload_len = outbound.payload_len;
    ret = discovery_assignment_append_abort_identity(
        outbound.payload,
        sizeof(outbound.payload),
        &payload_len,
        &identity);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint16_t)payload_len;
    outbound.packet.payload_len = (uint16_t)payload_len;
    ret = gateway_discovery_assignment_submit_control_flood_locked(
        &outbound,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT);
    status_debug_printf(
        "DBG_ENUM_ABORT_TX epoch=%u claim_session=%u claim_seq=%u ret=%d\n",
        identity.epoch,
        identity.claim_session_id,
        identity.claim_command_seq,
        ret);
    return ret;
}

static void gateway_discovery_assignment_fail_locked(
    enum command_status status,
    uint8_t reason)
{
    int ret;

    if (!gateway_discovery_assignment_state.active) {
        return;
    }
    if (!gateway_discovery_assignment_state.claim_rf_started &&
        gateway_discovery_assignment_rf_started_locked(
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM)) {
        gateway_discovery_assignment_state.claim_rf_started = true;
    }
    if (!gateway_discovery_assignment_state.claim_rf_started) {
        gateway_discovery_assignment_finish_failure_locked(status, reason);
        return;
    }
    if (gateway_discovery_assignment_state.stage !=
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY) {
        if (!gateway_discovery_assignment_abandon_active_delivery_locked()) {
            return;
        }
        gateway_discovery_assignment_state.pending_failure_status = status;
        gateway_discovery_assignment_state.pending_failure_reason = reason;
        gateway_discovery_assignment_state.stage =
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY;
        gateway_discovery_assignment_state.round_open = false;
    }
    if (gateway_discovery_assignment_state.delivery_handle != 0u) {
        return;
    }
    ret = gateway_discovery_assignment_publish_abort_locked();
    if (ret == 0) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "abort-delivery-poll");
        return;
    }
    gateway_discovery_assignment_state.abort_admission_retry_count++;
    if (gateway_discovery_assignment_state.abort_admission_retry_count >= 10u) {
        gateway_discovery_assignment_finish_failure_locked(status, reason);
        return;
    }
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(100u), "abort-admission-retry");
}

static void gateway_discovery_assignment_finish_failure_locked(
    enum command_status status,
    uint8_t reason)
{
    enum gateway_command_event_reason event_reason;
    uint16_t failure_count = 0u;

    if (!gateway_discovery_assignment_state.active) {
        return;
    }
    if (!gateway_discovery_assignment_state.claim_rf_started) {
        /*
         * A response timeout describes anchors only after the gateway proved
         * a physical CLAIM attempt.  Pre-RF admission/deadline failure is a
         * local radio failure, even if the operation budget also expired.
         */
        status = COMMAND_RADIO_ERROR;
        event_reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
    } else if (app_discovery_assignment_operation_expired(
            (uint64_t)k_uptime_get(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    } else if (status == COMMAND_TIMEOUT &&
               gateway_discovery_assignment_state.claim_count == 0u &&
               gateway_discovery_assignment_state.claim_rf_started &&
               gateway_discovery_assignment_state.claim_delivery_succeeded) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS;
    } else if (status == COMMAND_TIMEOUT &&
               gateway_discovery_assignment_state.table_delivery_succeeded) {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    } else {
        event_reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
    }
    if (gateway_discovery_assignment_state.table_delivery_succeeded) {
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
    if (gateway_discovery_assignment_state.result_reservation_token != 0u) {
        gateway_emit_host_command_result_reserved(
            gateway_discovery_assignment_state.result_reservation_token,
            &gateway_discovery_assignment_state.host_command,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            status,
            reason);
    }
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.status = status;
        event.reason = event_reason;
        event.attempt = gateway_discovery_assignment_state.claim_round;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.total_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.failure_count = failure_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, true);
        if (app_gateway_assignment_publisher_abort_prepared_batch(&event)) {
            int rollback_ret =
                gateway_abort_pending_assignment_publication_ram_only(&event);

            if (rollback_ret < 0 && rollback_ret != -ENOTSUP) {
                LOG_ERR("assignment RAM-only publication rollback failed: ret=%d",
                        rollback_ret);
                app_watchdog_stop_feeding();
            }
        }
    }
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    (void)gateway_operation_owner_release(
        &gateway_assignment_operation_lease);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_finalize_work);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_publish_work);
}

static int gateway_discovery_assignment_validate_host_options(
    const uint8_t *payload,
    size_t payload_len)
{
    struct gateway_command_options host_options;
    int ret;

    ret = gateway_command_extract_options(payload, payload_len, &host_options);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (host_options.scope != CMD_SCOPE_SINGLE_NODE ||
        host_options.response_mode != CMD_RESPONSE_SMALL_RESULT ||
        host_options.flood_required ||
        host_options.collection_required) {
        return -EINVAL;
    }
    return 0;
}

static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_gateway_assignment_publisher_diagnostics publisher_diag;
    struct app_operation_policy_candidate policy_candidate;
    uint32_t budget_ms = DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    uint32_t claim_command_seq = 0u;
    uint32_t reserved_epoch = 0u;
    uint32_t required_budget_ms = 0u;
    uint16_t expected_claim_count = 0u;
    bool budget_explicit = false;
    bool expected_count_present = false;
    bool ram_only_iteration = false;
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
    ret = gateway_discovery_assignment_validate_host_options(
        payload, payload_len);
    if (ret < 0) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         EINVAL);
        GATEWAY_ASSIGNMENT_START_RETURN(ret);
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
    ram_only_iteration =
        policy_candidate.resolved.assignment.ram_only_iteration;
    /* An explicit RAM-only run is a clean-slate request. Discard an older
     * restored publication before ordinary publication/state admission, so
     * reset replay cannot permanently return BUSY without reaching CLAIM. */
    if (ram_only_iteration) {
        ret = gateway_clear_registered_membership_roster_ram_only();
        if (ret < 0) {
            status_debug_printf(
                "DBG_ASSIGN_START_BLOCK gate=ram-clear ret=%d\n", ret);
            gateway_emit_host_command_result(host_command,
                                             CMD_ASSIGN_DISCOVERY_SLOTS,
                                             COMMAND_BUSY,
                                             (uint8_t)(-ret));
            GATEWAY_ASSIGNMENT_START_RETURN(ret);
        }
    }
    if (!app_gateway_control_sequence_admission_available(
            APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET)) {
        status_debug_note("DBG_ASSIGN_START_BLOCK gate=sequence\n");
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
        status_debug_note("DBG_ASSIGN_START_BLOCK gate=publication\n");
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         5u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    app_gateway_assignment_publisher_get_diagnostics(&publisher_diag);
    if (gateway_discovery_assignment_state.active || publisher_diag.active) {
        status_debug_printf(
            "DBG_ASSIGN_START_BLOCK gate=state assignment=%u publisher=%u\n",
            gateway_discovery_assignment_state.active ? 1u : 0u,
            publisher_diag.active ? 1u : 0u);
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
        status_debug_note("DBG_ASSIGN_START_BLOCK gate=owner\n");
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         4u);
        GATEWAY_ASSIGNMENT_START_RETURN(-EBUSY);
    }
    operation_claimed = true;
    /*
     * A new assignment is a clean-slate operation. Retire the previous active
     * roster before CLAIM so neither old IDs nor their slots can influence
     * admission, ordering, timing, or a zero-response outcome. The checked
     * read below makes an asynchronous/failed durable clear ordinary start
     * backpressure instead of silently running with stale membership.
     */
    if (!ram_only_iteration) {
        gateway_clear_registered_membership_roster();
        {
            uint16_t stale_membership_epoch = 0u;
            size_t stale_count = 0u;

            ret = gateway_get_registered_membership_roster_with_slots(
                gateway_discovery_assignment_state.anchor_ids,
                gateway_discovery_assignment_state.anchor_slots,
                ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids),
                &stale_count,
                &stale_membership_epoch);
            if (ret != -ENOENT) {
                gateway_emit_host_command_result(
                    host_command,
                    CMD_ASSIGN_DISCOVERY_SLOTS,
                    ret == -EAGAIN ? COMMAND_BUSY :
                                     COMMAND_INTERNAL_ERROR,
                    (uint8_t)(ret < 0 ? -ret : EBUSY));
                GATEWAY_ASSIGNMENT_START_RETURN(ret < 0 ? ret : -EBUSY);
            }
        }
    }
    ret = mesh_relay_reconcile_gateway_ack_membership(
        &mesh_runtime, NULL, 0u);
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
    gateway_discovery_assignment_adaptive_observed_hop_count = 0u;
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
    gateway_discovery_assignment_state.ram_only_iteration =
        ram_only_iteration;
    gateway_discovery_assignment_state.expected_claim_count =
        expected_count_present ? expected_claim_count : 0u;
    /* The current operation starts without a trustworthy roster. Anchors get
     * the full supported ceiling, while the gateway expands its own receive
     * deadline only when a newly observed hop proves another band is needed. */
    gateway_discovery_assignment_state.response_lane_max_hop_count =
        UWB_ENUM_MAX_HOPS;
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
        gateway_discovery_assignment_finish_failure_locked(
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

    proof_ret = gateway_registered_membership_proves_assignment_ack(
        epoch,
        packet->session_id,
        table_commitment,
        packet->src_id);
    if (proof_ret == 1) {
        /*
         * The first TABLE response is intentionally left in sender custody
         * until an exact durable proof, or a newer durable roster that makes
         * this response obsolete, exists. Reconcile only now, while gateway
         * RX owns mesh_runtime: the durable roster is authoritative, retained
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

static int gateway_discovery_assignment_note_compact_record_locked(
    const struct uwb_enumeration_record *record,
    uint64_t received_at_ms,
    uint64_t previous_hop_id)
{
    size_t anchor_index = SIZE_MAX;

    if (record == NULL || record->anchor_id == 0u ||
        record->hop_count == 0u || record->hop_count > UWB_ENUM_MAX_HOPS ||
        !gateway_discovery_assignment_state.active ||
        !gateway_discovery_assignment_state.response_lane_active ||
        gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS ||
        received_at_ms <
            gateway_discovery_assignment_state.response_lane_start_ms ||
        received_at_ms >=
            gateway_discovery_assignment_state.response_lane_start_ms +
                enumeration_response_duration_ms(
                    gateway_discovery_assignment_state
                        .response_lane_max_hop_count)) {
        return -ESTALE;
    }
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        if (gateway_discovery_assignment_state.anchor_ids[i] ==
            record->anchor_id) {
            anchor_index = i;
            break;
        }
    }
    if (anchor_index == SIZE_MAX) {
        if (gateway_discovery_assignment_state.claim_count >=
            ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)) {
            return -ENOSPC;
        }
        anchor_index = gateway_discovery_assignment_state.claim_count++;
        gateway_discovery_assignment_state.anchor_ids[anchor_index] =
            record->anchor_id;
        gateway_discovery_assignment_state.anchor_slots[anchor_index] =
            GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
    } else if ((gateway_discovery_assignment_state.claim_response_mask &
                (UINT64_C(1) << anchor_index)) != 0u) {
        if (gateway_discovery_assignment_state.anchor_hop_counts[anchor_index] !=
            record->hop_count) {
            return -EBADMSG;
        }
        if (gateway_discovery_assignment_state.duplicate_count < UINT16_MAX) {
            gateway_discovery_assignment_state.duplicate_count++;
        }
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }

    gateway_discovery_assignment_state.anchor_hop_counts[anchor_index] =
        record->hop_count;
    gateway_discovery_assignment_state.anchor_previous_hop_ids[anchor_index] =
        previous_hop_id;
    gateway_discovery_assignment_state.claim_response_mask |=
        UINT64_C(1) << anchor_index;
    gateway_discovery_assignment_state.compact_observability_pending_mask |=
        UINT64_C(1) << anchor_index;
    if (record->hop_count >
        gateway_discovery_assignment_state.max_hop_count) {
        gateway_discovery_assignment_state.max_hop_count =
            record->hop_count;
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

bool app_gateway_enumeration_response_pending_wait_ms(
    uint64_t now_ms,
    uint32_t *wait_ms)
{
    uint64_t prepare_start_ms;
    uint64_t remaining_ms;
    bool pending = false;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return false;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    prepare_start_ms = gateway_discovery_assignment_state
            .response_lane_start_ms > ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS ?
        gateway_discovery_assignment_state.response_lane_start_ms -
            ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS : 0u;
    if (gateway_discovery_assignment_state.active &&
        gateway_discovery_assignment_state.response_lane_active &&
        gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
        now_ms < prepare_start_ms) {
        remaining_ms = prepare_start_ms - now_ms;
        if (wait_ms != NULL) {
            *wait_ms = remaining_ms > UINT32_MAX ?
                UINT32_MAX : (uint32_t)remaining_ms;
        }
        pending = true;
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return pending;
}

bool app_gateway_enumeration_response_window(
    uint64_t now_ms,
    uint64_t *round_deadline_ms)
{
    uint64_t elapsed_ms;
    uint64_t deadline_ms;
    uint64_t prepare_start_ms;
    bool active = false;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return false;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    prepare_start_ms = gateway_discovery_assignment_state
            .response_lane_start_ms > ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS ?
        gateway_discovery_assignment_state.response_lane_start_ms -
            ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS : 0u;
    if (gateway_discovery_assignment_state.active &&
        gateway_discovery_assignment_state.response_lane_active &&
        gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
        now_ms >= prepare_start_ms &&
        now_ms < gateway_discovery_assignment_state.claim_collection_deadline_ms) {
        if (now_ms < gateway_discovery_assignment_state
                         .response_lane_start_ms) {
            deadline_ms = gateway_discovery_assignment_state
                .response_lane_start_ms + ENUMERATION_RESPONSE_ROUND_MS;
        } else {
            elapsed_ms = now_ms -
                gateway_discovery_assignment_state.response_lane_start_ms;
            deadline_ms = now_ms -
                (elapsed_ms % ENUMERATION_RESPONSE_ROUND_MS) +
                ENUMERATION_RESPONSE_ROUND_MS;
        }
        if (deadline_ms >
            gateway_discovery_assignment_state.claim_collection_deadline_ms) {
            deadline_ms =
                gateway_discovery_assignment_state.claim_collection_deadline_ms;
        }
        if (round_deadline_ms != NULL) {
            *round_deadline_ms = deadline_ms;
        }
        active = true;
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return active;
}

int app_gateway_enumeration_response_handle_bundle(
    const struct uwb_enumeration_bundle_frame *bundle,
    uint64_t received_at_ms,
    struct uwb_enumeration_hop_ack_frame *ack)
{
    struct enumeration_response_timing timing;
    bool valid_bundle_activity = false;
    int ret = 0;

    if (DEVICE_ROLE != ROLE_GATEWAY || bundle == NULL || ack == NULL ||
        bundle->network_id != NETWORK_ID || bundle->parent_id != DEVICE_ID) {
        return -EINVAL;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (!gateway_discovery_assignment_state.active ||
        !gateway_discovery_assignment_state.response_lane_active ||
        bundle->epoch != gateway_discovery_assignment_state.epoch ||
        !enumeration_response_timing_at_depth(
            gateway_discovery_assignment_state.response_lane_start_ms,
            received_at_ms,
            gateway_discovery_assignment_state.response_lane_max_hop_count,
            &timing)) {
        ret = -ESTALE;
        goto out;
    }
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        int note_ret = gateway_discovery_assignment_note_compact_record_locked(
            &bundle->records[i], received_at_ms, bundle->sender_id);

        if (note_ret < 0) {
            ret = note_ret;
            goto out;
        }
        valid_bundle_activity = true;
    }
    if (valid_bundle_activity) {
        uint8_t observed_hop_count =
            gateway_discovery_assignment_state.max_hop_count;
        uint8_t active_hop_count = MAX(observed_hop_count, timing.depth);
        uint8_t wait_through_hop_count = active_hop_count <
                    gateway_discovery_assignment_state
                        .response_lane_max_hop_count ?
            (uint8_t)(active_hop_count + 1u) : active_hop_count;
        uint64_t depth_deadline =
            gateway_discovery_assignment_state.response_lane_start_ms +
            enumeration_response_duration_ms(wait_through_hop_count);

        if (depth_deadline >
            gateway_discovery_assignment_state.claim_collection_deadline_ms) {
            if (depth_deadline >
                gateway_discovery_assignment_state.operation_deadline_ms) {
                depth_deadline =
                    gateway_discovery_assignment_state.operation_deadline_ms;
            }
            gateway_discovery_assignment_state.claim_collection_deadline_ms =
                depth_deadline;
            gateway_discovery_assignment_state.response_window_deadline_ms =
                (uint32_t)depth_deadline;
            gateway_discovery_assignment_state.response_window_deadline_valid =
                true;
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(depth_deadline > received_at_ms ?
                    (uint32_t)(depth_deadline - received_at_ms) : 0u),
                "compact-next-depth");
            status_debug_printf(
                "DBG_ENUM_COMPACT_DEPTH_WAIT observed_hop=%u through_hop=%u deadline=%u\n",
                observed_hop_count,
                wait_through_hop_count,
                (uint32_t)depth_deadline);
        }
    }
    *ack = (struct uwb_enumeration_hop_ack_frame) {
        .network_id = NETWORK_ID,
        .epoch = gateway_discovery_assignment_state.epoch,
        .parent_id = DEVICE_ID,
        .child_id = bundle->sender_id,
        .sequence = bundle->sequence,
    };
out:
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret;
}

void app_gateway_enumeration_response_publish_pending(void)
{
    while (DEVICE_ROLE == ROLE_GATEWAY) {
        struct gateway_command_event event;
        uint64_t pending_mask;
        size_t anchor_index;
        uint32_t assignment_epoch;

        k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
        pending_mask = gateway_discovery_assignment_state
            .compact_observability_pending_mask;
        if (!gateway_discovery_assignment_state.active || pending_mask == 0u) {
            k_mutex_unlock(&gateway_discovery_assignment_mutex);
            return;
        }
        anchor_index = (size_t)__builtin_ctzll(pending_mask);
        gateway_discovery_assignment_state.compact_observability_pending_mask &=
            ~(UINT64_C(1) << anchor_index);
        event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);
        event.anchor_id =
            gateway_discovery_assignment_state.anchor_ids[anchor_index];
        event.previous_hop_id = gateway_discovery_assignment_state
            .anchor_previous_hop_ids[anchor_index];
        event.hop_count = gateway_discovery_assignment_state
            .anchor_hop_counts[anchor_index];
        event.progress_count = (uint16_t)(anchor_index + 1u);
        event.duplicate_count =
            gateway_discovery_assignment_state.duplicate_count;
        assignment_epoch = gateway_discovery_assignment_state.epoch;
        k_mutex_unlock(&gateway_discovery_assignment_mutex);

        (void)gateway_observe_command_event(&event, false);
        status_debug_printf(
            "DBG_ENUM_COMPACT_RECORD_PUBLISHED epoch=%u anchor=0x%016llx hop=%u via=0x%016llx progress=%u\n",
            assignment_epoch,
            (unsigned long long)event.anchor_id,
            event.hop_count,
            (unsigned long long)event.previous_hop_id,
            event.progress_count);
    }
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
        status_debug_printf("DBG_ENUM_REJECT reason=envelope ret=%d\n", ret);
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
        status_debug_printf("DBG_ENUM_REJECT reason=identity epoch=%u\n",
                            epoch);
        return -EBADMSG;
    }
    /*
     * Enumeration claims and table receipts now use the compact raw-UWB
     * lane.  Retire a valid legacy result so an old queued frame cannot hold
     * mesh custody, but never let that obsolete path mutate the new roster.
     */
    return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
#if 0
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
                status_debug_printf("DBG_ENUM_REJECT reason=proof ret=%d\n",
                                    ret);
                GATEWAY_ASSIGNMENT_RETURN(ret);
            }
        }
        status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=inactive\n",
                            (unsigned int)phase);
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
        status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=epoch\n",
                            (unsigned int)phase);
        GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (!uptime_deadline_reached(
            (uint32_t)received_at_ms,
            gateway_discovery_assignment_operation_origin_ms_locked()) ||
        uptime_deadline_reached(
            (uint32_t)received_at_ms,
            (uint32_t)gateway_discovery_assignment_state
                .operation_deadline_ms)) {
        status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=deadline\n",
                            (unsigned int)phase);
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
            status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=state\n",
                                (unsigned int)phase);
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
            status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=state\n",
                                (unsigned int)phase);
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
        status_debug_printf("DBG_ENUM_REDRIVE epoch=%u\n", epoch);
        GATEWAY_ASSIGNMENT_RETURN(-EAGAIN);
    }
    if (!gateway_discovery_assignment_rf_started_locked(
            phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ?
                GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE :
                GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM)) {
        status_debug_printf("DBG_ENUM_REJECT phase=%u reason=rf\n",
                            (unsigned int)phase);
        GATEWAY_ASSIGNMENT_RETURN(-EAGAIN);
    }
    if (gateway_discovery_assignment_state
            .response_window_deadline_valid &&
        uptime_deadline_reached(
            (uint32_t)received_at_ms,
            gateway_discovery_assignment_state
                .response_window_deadline_ms)) {
        status_debug_printf("DBG_ENUM_RETIRE phase=%u reason=response-deadline\n",
                            (unsigned int)phase);
        GATEWAY_ASSIGNMENT_RETURN(
            phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ?
                gateway_discovery_assignment_ack_proof_admission(
                    packet, epoch, &table_commitment) :
                APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE);
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        anchor_index == SIZE_MAX &&
        gateway_discovery_assignment_expected_claims_complete_locked()) {
        status_debug_printf("DBG_ENUM_REJECT reason=expected-count-closed\n");
        GATEWAY_ASSIGNMENT_RETURN(-ENOSPC);
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        anchor_index == SIZE_MAX &&
        gateway_discovery_assignment_state.claim_count >=
            ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)) {
        status_debug_printf("DBG_ENUM_REJECT reason=capacity\n");
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
        status_debug_printf("DBG_ENUM_REJECT reason=ack-capacity ret=%d\n",
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
        if (gateway_registered_membership_proves_assignment_ack(
                epoch,
                packet->session_id,
                &table_commitment,
                packet->src_id) != 1) {
            GATEWAY_ASSIGNMENT_RETURN(-EAGAIN);
        }
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
        if (!ack_already_recorded) {
            gateway_discovery_assignment_refresh_adaptive_deadline_locked(
                received_at_ms,
                gateway_discovery_assignment_state.anchor_hop_counts[
                    anchor_index]);
        }
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
            gateway_discovery_assignment_refresh_adaptive_deadline_locked(
                received_at_ms, observed_hop_count);
            if (gateway_discovery_assignment_expected_claims_complete_locked()) {
                (void)gateway_discovery_assignment_arm_claim_ack_settle_locked(
                    (uint32_t)received_at_ms);
                (void)gateway_discovery_assignment_wake_now(
                    "expected-count-closed");
            }
            status_debug_printf(
                "DBG_ENUM_CLAIM_RX epoch=%u anchor=0x%016llx hop=%u responders=%u roster=%u retained=1\n",
                epoch,
                (unsigned long long)packet->src_id,
                hop_count,
                (unsigned int)
                    gateway_discovery_assignment_current_claim_count_locked(),
                (unsigned int)gateway_discovery_assignment_state.claim_count);
            GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
        }
        if (gateway_discovery_assignment_state.duplicate_count < UINT16_MAX) {
            gateway_discovery_assignment_state.duplicate_count++;
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
        gateway_discovery_assignment_refresh_adaptive_deadline_locked(
            received_at_ms, observed_hop_count);
    }
    if (gateway_discovery_assignment_expected_claims_complete_locked()) {
        (void)gateway_discovery_assignment_arm_claim_ack_settle_locked(
            (uint32_t)received_at_ms);
        (void)gateway_discovery_assignment_wake_now(
            "expected-count-closed");
    }
    status_debug_printf("DBG_ENUM_CLAIM_RX epoch=%u anchor=0x%016llx hop=%u responders=%u roster=%u retained=0\n",
                        epoch,
                        (unsigned long long)packet->src_id,
                        hop_count,
                        (unsigned int)
                            gateway_discovery_assignment_current_claim_count_locked(),
                        (unsigned int)gateway_discovery_assignment_state.claim_count);
    GATEWAY_ASSIGNMENT_RETURN(APP_GATEWAY_SEMANTIC_ACCEPT_NEW);
#undef GATEWAY_ASSIGNMENT_RETURN
#endif
}

/*
 * Keep TABLE materialization off the delivery call chain.  The entry scratch
 * is dead before app_node_comm_submit_delivery() computes the semantic digest,
 * but one monolithic function makes GCC reserve both frames at once on the
 * system workqueue.  This explicit non-inline boundary preserves that real
 * lifetime split without moving transient scratch into permanent RAM.
 * gateway_discovery_assignment_mutex must be held by the caller.
 */
static __attribute__((noinline)) int
gateway_discovery_assignment_build_table_locked(struct mesh_outbound *outbound)
{
    size_t payload_len;
    int ret;

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
            outbound,
            DISCOVERY_ASSIGNMENT_PHASE_TABLE,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq,
            gateway_discovery_assignment_state.table_packet_seq);
    }
    payload_len = ret == 0 ? outbound->payload_len : 0u;
    if (ret == 0) {
        ret = discovery_assignment_append_table_from_roster(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            gateway_discovery_assignment_state.anchor_ids,
            gateway_discovery_assignment_state.anchor_slots,
            gateway_discovery_assignment_state.claim_count);
    }
    gateway_discovery_assignment_state.round_open = false;
    if (ret == PROTO_OK || ret == 0) {
        outbound->payload_len = (uint16_t)payload_len;
        outbound->packet.payload_len = (uint16_t)payload_len;
        ret = discovery_assignment_table_commitment_from_roster(
                  gateway_discovery_assignment_state.anchor_ids,
                  gateway_discovery_assignment_state.anchor_slots,
                  gateway_discovery_assignment_state.claim_count,
                  UWB_DISCOVERY_SLOT_COUNT,
                  &gateway_discovery_assignment_state.table_commitment) ?
              PROTO_OK : PROTO_ERR_MALFORMED;
    }
    return ret;
}

static int gateway_discovery_assignment_publish_table(void)
{
    struct mesh_outbound outbound;
    int ret;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.claim_count == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -ENOENT;
    }
    ret = gateway_discovery_assignment_build_table_locked(&outbound);
    if (ret == PROTO_OK || ret == 0) {
        ret = gateway_discovery_assignment_prepare_durable_table_locked();
        if (ret < 0) {
            ret = PROTO_ERR_BUSY;
        }
    }
    if (ret == PROTO_OK || ret == 0) {
        ret = gateway_discovery_assignment_submit_control_flood_locked(
            &outbound,
            GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE);
        if (ret == 0) {
            gateway_discovery_assignment_state.table_delivery_is_redrive =
                false;
            gateway_discovery_assignment_state.stage =
                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS;
        }
    } else {
        ret = mesh_errno_from_proto(ret);
    }
    status_debug_printf("DBG_ENUM_TABLE_TX epoch=%u count=%u round=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        (unsigned int)gateway_discovery_assignment_state.claim_count,
                        gateway_discovery_assignment_state.table_round + 1u,
                        ret);
    if (ret == 0) {
        gateway_discovery_assignment_state.table_admission_retry_round = 0u;
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "table-delivery-poll");
    } else if (gateway_discovery_assignment_state.table_round <
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

static int gateway_discovery_assignment_publish_end(void)
{
    struct discovery_assignment_end_identity identity;
    struct mesh_outbound outbound;
    size_t payload_len;
    int ret;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.stage !=
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS ||
        !gateway_discovery_assignment_state.table_delivery_succeeded) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -EAGAIN;
    }
    if (gateway_discovery_assignment_state.end_command_seq == 0u) {
        gateway_discovery_assignment_state.end_command_seq =
            gateway_next_broadcast_command_seq();
    }
    if (gateway_discovery_assignment_state.end_packet_seq == 0u) {
        gateway_discovery_assignment_state.end_packet_seq =
            gateway_next_command_seq();
    }
    if (gateway_discovery_assignment_state.end_command_seq == 0u ||
        gateway_discovery_assignment_state.end_packet_seq == 0u) {
        ret = -EIO;
        goto out;
    }
    ret = gateway_build_discovery_assignment_command(
        &outbound,
        DISCOVERY_ASSIGNMENT_PHASE_END,
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.end_command_seq,
        gateway_discovery_assignment_state.end_packet_seq);
    if (ret < 0) {
        goto out;
    }
    identity = (struct discovery_assignment_end_identity) {
        .epoch = gateway_discovery_assignment_state.epoch,
        .table_command_seq =
            gateway_discovery_assignment_state.table_command_seq,
        .table_commitment =
            gateway_discovery_assignment_state.table_commitment,
    };
    payload_len = outbound.payload_len;
    ret = discovery_assignment_append_end_identity(
        outbound.payload,
        sizeof(outbound.payload),
        &payload_len,
        &identity);
    if (ret != PROTO_OK) {
        ret = mesh_errno_from_proto(ret);
        goto out;
    }
    outbound.payload_len = (uint16_t)payload_len;
    outbound.packet.payload_len = (uint16_t)payload_len;
    ret = gateway_discovery_assignment_submit_control_flood_locked(
        &outbound,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END);
    if (ret == 0) {
        gateway_discovery_assignment_state.stage =
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_END_DELIVERY;
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "end-delivery-poll");
    } else {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(100u), "end-admission-retry");
    }
out:
    status_debug_printf("DBG_ENUM_END_TX epoch=%u table_seq=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.table_command_seq,
                        ret);
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
    uint32_t handle;

    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    kind = gateway_discovery_assignment_state.delivery_kind;
    table_delivery_is_redrive =
        gateway_discovery_assignment_state.table_delivery_is_redrive;
    handle = gateway_discovery_assignment_state.delivery_handle;
    if (handle == 0u) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return false;
    }
    gateway_discovery_assignment_observe_rf_start_locked();
    if (!app_node_comm_peek_delivery_event_for(handle, &event)) {
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
            "delivery-terminal-poll");
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    gateway_discovery_assignment_observe_rf_start_locked();
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
    gateway_discovery_assignment_rf_event_handle = 0u;
    status_debug_printf("DBG_ENUM_RF_RESULT kind=%u handle=%u success=%u reason=%u tries=%u\n",
                        (unsigned int)kind,
                        handle,
                        event.reason == NODE_COMM_TERMINAL_DELIVERED ? 1u : 0u,
                        (unsigned int)event.reason,
                        event.attempts_started);

    effective_delivered = event.reason == NODE_COMM_TERMINAL_DELIVERED;
    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM) {
        /*
         * A bounded redundant CLAIM flood can reach anchors even when a later
         * local copy fails and the transport terminal is ATTEMPTS_EXHAUSTED.
         * Once any RF attempt began, preserve the already-advertised compact
         * response horizon and let the response window prove delivery.
         */
        effective_delivered =
            effective_delivered ||
            app_discovery_assignment_semantic_terminal_success(
                APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
                gateway_discovery_assignment_state.expected_claim_count,
                gateway_discovery_assignment_current_claim_count_locked(),
                gateway_discovery_assignment_missing_ack_count_locked(),
                event.attempts_started,
                app_discovery_assignment_operation_expired(
                    (uint64_t)k_uptime_get(),
                    gateway_discovery_assignment_state.operation_deadline_ms),
                !gateway_discovery_assignment_state.active);
    }

    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM) {
        if (event.attempts_started > 0u) {
            gateway_discovery_assignment_state.claim_rf_started = true;
        }
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
    } else if (kind != GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END &&
               kind != GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT) {
        gateway_discovery_assignment_fail_locked(COMMAND_INTERNAL_ERROR,
                                                 EPROTO);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }

    if (effective_delivered) {
        uint64_t response_window_origin_ms = event.terminal_at_ms;
        uint32_t wait_ms;

        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_END) {
            wait_ms =
                gateway_discovery_assignment_propagation_hold_ms_locked();
            gateway_discovery_assignment_state.phase_propagation_deadline_ms =
                response_window_origin_ms + wait_ms;
            gateway_discovery_assignment_state.end_propagation_pending = true;
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(wait_ms), "end-propagation-hold");
            k_mutex_unlock(&gateway_discovery_assignment_mutex);
            return true;
        }
        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT) {
            gateway_discovery_assignment_finish_failure_locked(
                gateway_discovery_assignment_state.pending_failure_status,
                gateway_discovery_assignment_state.pending_failure_reason);
            k_mutex_unlock(&gateway_discovery_assignment_mutex);
            return true;
        }

        gateway_discovery_assignment_state.round_open = true;
        gateway_discovery_assignment_state.response_window_origin_ms =
            response_window_origin_ms;
        if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM) {
            uint64_t collection_deadline_ms =
                gateway_discovery_assignment_state
                    .claim_collection_deadline_ms;

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
            wait_ms = collection_deadline_ms > (uint64_t)k_uptime_get() ?
                (uint32_t)(collection_deadline_ms -
                           (uint64_t)k_uptime_get()) : 0u;
            gateway_discovery_assignment_state.response_window_deadline_ms =
                (uint32_t)collection_deadline_ms;
            gateway_discovery_assignment_state.response_window_deadline_valid =
                true;
            status_debug_printf(
                "DBG_ENUM_COMPACT_GATEWAY_SCHEDULE now=%u start=%u deadline=%u wait=%u\n",
                k_uptime_get_32(),
                (uint32_t)gateway_discovery_assignment_state
                    .response_lane_start_ms,
                (uint32_t)collection_deadline_ms,
                wait_ms);
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(wait_ms), "compact-response-lane");
            (void)mesh_start_uwb_rx("compact-enumeration");
            k_mutex_unlock(&gateway_discovery_assignment_mutex);
            return true;
        } else if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE) {
            wait_ms =
                gateway_discovery_assignment_propagation_hold_ms_locked();
            gateway_discovery_assignment_state.phase_propagation_deadline_ms =
                response_window_origin_ms + wait_ms;
        } else {
            wait_ms =
                gateway_discovery_assignment_adaptive_deadline_offset_ms_locked(
                    1u, 1u);
        }
        wait_ms = gateway_discovery_assignment_set_response_deadline_locked(
            response_window_origin_ms, wait_ms);
        (void)gateway_discovery_assignment_reschedule(
            K_MSEC(wait_ms), "response-window");
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }

    gateway_discovery_assignment_state.round_open = false;
    if (kind == GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT) {
        gateway_discovery_assignment_finish_failure_locked(
            gateway_discovery_assignment_state.pending_failure_status,
            gateway_discovery_assignment_state.pending_failure_reason);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return true;
    }
    /*
     * Node communication owns the complete bounded RF attempt and recovery.
     * Any non-delivered terminal here is that physical boundary's final
     * failure, so retrying it as another logical CLAIM can only hide a broken
     * gateway radio behind a later no-response timeout.
     */
    gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                             (uint8_t)event.reason);
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
        uint64_t lane_end_ms =
            gateway_discovery_assignment_state.claim_collection_deadline_ms;
        uint64_t now_ms = (uint64_t)k_uptime_get();

        if (gateway_discovery_assignment_state.response_lane_active &&
            now_ms < lane_end_ms) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC((uint32_t)(lane_end_ms - now_ms)),
                "compact-response-lane-open");
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        gateway_discovery_assignment_state.response_lane_active = false;
        if (gateway_discovery_assignment_expected_claims_missing_locked()) {
            status_debug_printf(
                "DBG_ENUM_EXPECTED_MISSING expected=%u received=%u\n",
                gateway_discovery_assignment_state.expected_claim_count,
                (unsigned int)
                    gateway_discovery_assignment_current_claim_count_locked());
            gateway_discovery_assignment_fail_locked(
                COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        if (gateway_discovery_assignment_expected_claims_complete_locked()) {
            uint32_t settle_ms =
                gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                    (uint64_t)k_uptime_get());

            if (settle_ms != 0u) {
                (void)gateway_discovery_assignment_reschedule(
                    K_MSEC(settle_ms), "expected-claim-settle");
                GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
            }
            gateway_discovery_assignment_state.round_open = false;
            gateway_discovery_assignment_state.claim_ack_settle_armed = false;
            gateway_discovery_assignment_state.claim_ack_settle_deadline_ms =
                0u;
            gateway_discovery_assignment_state.table_round = 0u;
            ret = gateway_discovery_assignment_publish_table();
            if (ret < 0 && gateway_discovery_assignment_state.table_round >=
                gateway_discovery_assignment_table_round_limit_locked()) {
                gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                                         (uint8_t)(-ret));
            }
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
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

            status_debug_printf("DBG_ENUM_BACKOFF phase=claim delay=%u\n",
                                retry_ms);
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(retry_ms), "claim-round-backoff");
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        if (gateway_discovery_assignment_state.claim_count == 0u) {
            gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        gateway_discovery_assignment_state.claim_ack_settle_armed = false;
        gateway_discovery_assignment_state.claim_ack_settle_deadline_ms = 0u;

        if (gateway_discovery_assignment_current_claim_count_locked() == 0u) {
            gateway_discovery_assignment_fail_locked(COMMAND_TIMEOUT, 0u);
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }

        gateway_discovery_assignment_state.table_round = 0u;
        ret = gateway_discovery_assignment_publish_table();
        if (ret < 0 && gateway_discovery_assignment_state.table_round >=
            gateway_discovery_assignment_table_round_limit_locked()) {
            gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,
                                                     (uint8_t)(-ret));
        }
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }

    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_END_DELIVERY &&
        gateway_discovery_assignment_state.end_propagation_pending) {
        uint64_t now_ms = (uint64_t)k_uptime_get();

        if (now_ms < gateway_discovery_assignment_state
                .phase_propagation_deadline_ms) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC((uint32_t)(gateway_discovery_assignment_state
                    .phase_propagation_deadline_ms - now_ms)),
                "end-propagation-hold");
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        gateway_discovery_assignment_state.end_propagation_pending = false;
        gateway_discovery_assignment_complete_success_locked();
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }

    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
        gateway_discovery_assignment_state.table_delivery_succeeded) {
        uint64_t now_ms = (uint64_t)k_uptime_get();

        if (now_ms < gateway_discovery_assignment_state
                .phase_propagation_deadline_ms) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC((uint32_t)(gateway_discovery_assignment_state
                    .phase_propagation_deadline_ms - now_ms)),
                "table-propagation-hold");
            GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
        }
        gateway_discovery_assignment_state.late_table_redrive_pending = false;
        ret = gateway_discovery_assignment_publish_end();
        if (ret < 0) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(100u), "end-submit-retry");
        }
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }
    if (gateway_discovery_assignment_state.replay &&
        gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
        ret = gateway_discovery_assignment_publish_table();
        if (ret < 0) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(100u), "table-resume-retry");
        }
        GATEWAY_ASSIGNMENT_PUBLISH_RETURN();
    }
    (void)gateway_discovery_assignment_reschedule(
        K_MSEC(GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS),
        "table-delivery-await");
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
    app_stack_workload_diag_gateway_control_admit(
        &gateway_discovery_assignment_state.host_command, 1u, 1u);
    app_stack_workload_diag_gateway_control_sample(
        &gateway_discovery_assignment_state.host_command, 1u, 1u);
    app_stack_workload_diag_gateway_control_release(
        &gateway_discovery_assignment_state.host_command, 0, 1u, 1u);
    if (gateway_discovery_assignment_service_delivery()) {
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
        gateway_discovery_assignment_state.response_lane_active) {
        uint64_t lane_end_ms =
            gateway_discovery_assignment_state.claim_collection_deadline_ms;
        uint64_t now_ms = (uint64_t)k_uptime_get();

        if (now_ms < lane_end_ms) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC((uint32_t)(lane_end_ms - now_ms)),
                "compact-response-lane-finalize");
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        gateway_discovery_assignment_state.response_lane_active = false;
    }
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
        gateway_discovery_assignment_expected_claims_missing_locked()) {
        status_debug_printf(
            "DBG_ENUM_EXPECTED_MISSING expected=%u received=%u\n",
            gateway_discovery_assignment_state.expected_claim_count,
            (unsigned int)
                gateway_discovery_assignment_current_claim_count_locked());
        gateway_discovery_assignment_fail_locked(
            COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
    /*
     * CLAIM delivery can finish before the expected responders arrive.  Their
     * final semantic receipt wakes this owner, but the response deadline was
     * frozen while topology depth was still unknown.  Close that obsolete
     * window at the level-triggered quorum and continue through the existing
     * gateway-priority submission path.
     */
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS &&
        gateway_discovery_assignment_expected_claims_complete_locked()) {
        uint32_t settle_ms =
            gateway_discovery_assignment_claim_ack_settle_remaining_locked(
                (uint64_t)k_uptime_get());

        if (settle_ms != 0u) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(settle_ms), "expected-claim-settle");
            GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
        }
        gateway_discovery_assignment_state.round_open = false;
        gateway_discovery_assignment_state.response_window_deadline_valid =
            false;
    }
    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY) {
        gateway_discovery_assignment_fail_locked(
            gateway_discovery_assignment_state.pending_failure_status,
            gateway_discovery_assignment_state.pending_failure_reason);
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
    }
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
        gateway_discovery_assignment_state.response_ack_settle_armed = false;
        gateway_discovery_assignment_state.response_ack_settle_deadline_ms = 0u;
        gateway_discovery_assignment_state.round_open = false;
        ret = gateway_discovery_assignment_publish_end();
        if (ret < 0) {
            (void)gateway_discovery_assignment_reschedule(
                K_MSEC(100u), "end-submit-retry");
        }
        GATEWAY_ASSIGNMENT_FINALIZE_RETURN();
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
    if (gateway_discovery_assignment_state.round_open) {
        if (gateway_discovery_assignment_state
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

int app_anchor_gateway_assignment_resume_pending_table(
    const struct gateway_membership_snapshot *snapshot,
    const struct gateway_membership_publication *publication)
{
    uint64_t responder_mask = 0u;
    size_t cursor = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || snapshot == NULL ||
        publication == NULL ||
        (snapshot->publication_table_round &
         GATEWAY_MEMBERSHIP_TABLE_CONFIRM_PENDING) == 0u ||
        snapshot->node_count == 0u ||
        publication->acknowledged_mask == 0u) {
        return -EINVAL;
    }
    k_mutex_lock(&gateway_discovery_assignment_mutex, K_FOREVER);
    if (gateway_discovery_assignment_state.active) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return 1;
    }
    ret = gateway_operation_owner_claim(
        APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
        &gateway_assignment_operation_lease);
    if (ret < 0) {
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return ret;
    }
    memset(&gateway_discovery_assignment_state,
           0,
           sizeof(gateway_discovery_assignment_state));
    for (uint8_t slot = 0u; slot < snapshot->slot_span; slot++) {
        uint64_t node_id = snapshot->node_ids[slot];

        if (node_id == 0u) {
            continue;
        }
        gateway_discovery_assignment_state.anchor_ids[cursor] = node_id;
        gateway_discovery_assignment_state.anchor_slots[cursor] = slot;
        if ((publication->acknowledged_mask &
             (UINT64_C(1) << slot)) != 0u) {
            responder_mask |= UINT64_C(1) << cursor;
        }
        cursor++;
    }
    if (cursor != snapshot->node_count || responder_mask == 0u) {
        (void)gateway_operation_owner_release(
            &gateway_assignment_operation_lease);
        k_mutex_unlock(&gateway_discovery_assignment_mutex);
        return -EPROTO;
    }
    gateway_discovery_assignment_state.host_command =
        publication->host_command;
    gateway_discovery_assignment_state.epoch = snapshot->assignment_epoch;
    gateway_discovery_assignment_state.table_command_seq =
        snapshot->assignment_table_seq;
    gateway_discovery_assignment_state.table_commitment =
        snapshot->assignment_table_commitment;
    /* The durable publication does not store the host-selected spread.  A
     * replay must therefore rebuild the TABLE with the conservative allowed
     * maximum, never the zero left by the state reset above. */
    gateway_discovery_assignment_state.response_spread_ms =
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS;
    gateway_discovery_assignment_state.operation_deadline_ms =
        (uint64_t)k_uptime_get() +
        DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    gateway_discovery_assignment_state.command_budget_ms =
        DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    gateway_discovery_assignment_state.claim_count = cursor;
    gateway_discovery_assignment_state.prior_anchor_count = (uint8_t)cursor;
    gateway_discovery_assignment_state.claim_response_mask = responder_mask;
    gateway_discovery_assignment_state.expected_claim_count =
        (uint16_t)__builtin_popcountll(responder_mask);
    gateway_discovery_assignment_state.max_hop_count =
        DISCOVERY_ASSIGNMENT_MAX_HOPS;
    gateway_discovery_assignment_state.stage =
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS;
    gateway_discovery_assignment_state.generation =
        gateway_discovery_assignment_next_generation();
    gateway_discovery_assignment_state.replay = true;
    gateway_discovery_assignment_state.active = true;
    ret = gateway_discovery_assignment_reschedule(
        K_NO_WAIT, "table-resume");
    if (ret < 0) {
        gateway_discovery_assignment_state.active = false;
        (void)gateway_operation_owner_release(
            &gateway_assignment_operation_lease);
    }
    k_mutex_unlock(&gateway_discovery_assignment_mutex);
    return ret < 0 ? ret : 1;
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

bool app_gateway_enumeration_response_window(
    uint64_t now_ms,
    uint64_t *round_deadline_ms)
{
    ARG_UNUSED(now_ms);
    ARG_UNUSED(round_deadline_ms);
    return false;
}

bool app_gateway_enumeration_response_pending_wait_ms(
    uint64_t now_ms,
    uint32_t *wait_ms)
{
    ARG_UNUSED(now_ms);
    ARG_UNUSED(wait_ms);
    return false;
}

int app_gateway_enumeration_response_handle_bundle(
    const struct uwb_enumeration_bundle_frame *bundle,
    uint64_t received_at_ms,
    struct uwb_enumeration_hop_ack_frame *ack)
{
    ARG_UNUSED(bundle);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(ack);
    return -ENOTSUP;
}

void app_gateway_enumeration_response_publish_pending(void)
{
}

int gateway_discovery_assignment_admit_nonassignment_source(uint64_t src_id)
{
    ARG_UNUSED(src_id);
    return -ENOTSUP;
}

int app_anchor_gateway_assignment_resume_pending_table(
    const struct gateway_membership_snapshot *snapshot,
    const struct gateway_membership_publication *publication)
{
    ARG_UNUSED(snapshot);
    ARG_UNUSED(publication);
    return -ENOTSUP;
}

int gateway_resume_pending_assignment_publication(void)
{
    return -ENOTSUP;
}
#endif

#if DEVICE_ROLE == ROLE_GATEWAY

static uint16_t gateway_survey_event_seq;

static int gateway_start_survey(const struct proto_packet *packet);
static int gateway_submit_survey_plan(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len);
static int gateway_cancel_survey(const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len);
static int gateway_get_survey_status(const struct proto_packet *packet);

static int gateway_survey_emit_event(const struct survey_event *event)
{
    uint8_t payload[SURVEY_EVENT_MAX_WIRE_LEN];
    struct proto_packet packet = {0};
    size_t payload_len;

    if (event == NULL) {
        return -EINVAL;
    }
    payload_len = survey_event_encode(event, payload, sizeof(payload));
    if (payload_len == 0u) {
        return -EBADMSG;
    }
    gateway_survey_event_seq++;
    if (gateway_survey_event_seq == 0u) {
        gateway_survey_event_seq = 1u;
    }
    packet.msg_type = MSG_SURVEY_EVENT;
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet.src_id = DEVICE_ID;
    packet.dst_id = DEVICE_ID;
    packet.session_id = event->identity.generation;
    packet.seq = gateway_survey_event_seq;
    packet.ttl = 1u;
    packet.payload_len = (uint16_t)payload_len;
    return gateway_ble_stream_packet(&packet, payload, payload_len,
                                     k_uptime_get_32());
}

static int gateway_survey_send_control(
    const struct survey_control *control,
    uint64_t *origin_ms)
{
    struct mesh_outbound outbound = {0};
    uint32_t command_seq;
    uint32_t flood_epoch;
    uint32_t handle = 0u;
    size_t payload_len = 0u;
    enum command_id command_id;
    uint64_t origin;
    int ret;

    if (control == NULL || origin_ms == NULL) {
        return -EINVAL;
    }
    command_id = control->phase == SURVEY_PHASE_PLAN ?
        CMD_SURVEY_PLAN :
        control->phase == SURVEY_PHASE_ABORT ? CMD_SURVEY_CANCEL :
        CMD_SURVEY_START;
    command_seq = gateway_next_broadcast_command_seq();
    flood_epoch = gateway_next_broadcast_command_seq();
    if (command_seq == 0u || flood_epoch == 0u) {
        return -EIO;
    }
    ret = tlv_append_u16(outbound.payload, sizeof(outbound.payload),
                         &payload_len, TLV_COMMAND_ID, command_id);
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound.payload, sizeof(outbound.payload),
                            &payload_len, TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound.payload, sizeof(outbound.payload),
                            &payload_len, TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound.payload, sizeof(outbound.payload),
                             &payload_len, TLV_COMMAND_SEQ, command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound.payload, sizeof(outbound.payload),
                             &payload_len, TLV_FLOOD_EPOCH_ID,
                             flood_epoch);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound.payload, sizeof(outbound.payload),
                             &payload_len, TLV_COMMAND_EXPIRY_S,
                             SURVEY_HARD_CAP_MS / 1000u);
    }
    if (ret == PROTO_OK) {
        ret = survey_control_append_tlvs(outbound.payload,
                                         sizeof(outbound.payload),
                                         &payload_len, control);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.msg_type = MSG_COMMAND;
    outbound.packet.flags = FLAG_DIAGNOSTIC;
    outbound.packet.src_id = DEVICE_ID;
    outbound.packet.dst_id = MESH_BROADCAST_ID;
    outbound.packet.session_id = command_seq;
    outbound.packet.seq = gateway_next_command_seq();
    outbound.packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    outbound.packet.payload_len = (uint16_t)payload_len;
    outbound.payload_len = (uint16_t)payload_len;
    outbound.next_hop_id = MESH_BROADCAST_ID;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    outbound.flood_retry_count = MESH_ENUMERATION_RELAY_COPY_COUNT - 1u;
    ret = gateway_command_append_default_flood_controls(&outbound);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    origin = (uint64_t)k_uptime_get();
    outbound.queued_at_ms = (uint32_t)origin;
    outbound.queued_at_valid = true;
    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        origin + NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS,
        ((uint32_t)command_id << 16) ^ outbound.packet.seq,
        &handle);
    if (ret < 0) {
        return ret;
    }
    ret = app_node_comm_auto_reap_delivery(handle);
    if (ret < 0) {
        (void)app_node_comm_abandon_delivery(handle);
        return ret;
    }
    *origin_ms = origin;
    status_debug_printf(
        "DBG_SURVEY_CONTROL phase=%u gen=%u handle=%u delay=%u bytes=%u\n",
        (unsigned int)control->phase, control->identity.generation,
        handle, control->start_delay_present ? control->start_delay_ms : 0u,
        (unsigned int)payload_len);
    return 0;
}

static void gateway_survey_wake_rx(void)
{
    mesh_restart_role_scan();
}

static void gateway_survey_terminal(void)
{
    if (gateway_operation_owner_matches(
            APP_GATEWAY_OPERATION_OWNER_SURVEY,
            &gateway_survey_operation_lease)) {
        (void)gateway_operation_owner_release(
            &gateway_survey_operation_lease);
    }
}

static int gateway_survey_roster(
    struct app_survey_gateway_roster *roster)
{
    struct discovery_assignment_table_commitment commitment;
    uint16_t membership_epoch = 0u;
    uint32_t assignment_epoch = 0u;
    uint32_t table_seq = 0u;
    uint8_t slot_span = 0u;
    uint8_t max_hop_count;
    int ret;

    if (roster == NULL) {
        return -EINVAL;
    }
    memset(roster, 0, sizeof(*roster));
    ret = gateway_get_registered_membership_roster_with_slots(
        roster->node_ids, roster->slots, ARRAY_SIZE(roster->node_ids),
        &roster->node_count, &membership_epoch);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_get_registered_membership_assignment_identity(
        membership_epoch, roster->node_count, &assignment_epoch,
        &table_seq, &commitment, &slot_span);
    if (ret < 0) {
        return ret;
    }
    if (gateway_discovery_assignment_state.epoch != assignment_epoch ||
        gateway_discovery_assignment_state.table_command_seq != table_seq ||
        !discovery_assignment_table_commitment_equal(
            &gateway_discovery_assignment_state.table_commitment,
            &commitment)) {
        return -ESTALE;
    }
    max_hop_count = gateway_discovery_assignment_state.max_hop_count;
    if (max_hop_count == 0u || max_hop_count > UWB_ENUM_MAX_HOPS) {
        return -ESTALE;
    }
    roster->assignment = (struct survey_assignment_identity) {
        .assignment_epoch = assignment_epoch,
        .table_command_seq = table_seq,
        .table_commitment = commitment,
        .slot_span = slot_span,
        .max_hop_count = max_hop_count,
    };
    for (size_t member = 0u; member < roster->node_count; member++) {
        for (size_t observed = 0u;
             observed < gateway_discovery_assignment_state.claim_count;
             observed++) {
            if (gateway_discovery_assignment_state.anchor_ids[observed] ==
                roster->node_ids[member]) {
                roster->hop_counts[member] =
                    gateway_discovery_assignment_state
                        .anchor_hop_counts[observed];
                break;
            }
        }
    }
    return survey_assignment_identity_valid(&roster->assignment) ?
        0 : -ESTALE;
}

static int gateway_start_survey(
    const struct proto_packet *host_command)
{
    struct app_survey_gateway_roster roster;
    struct survey_identity identity;
    bool claimed = false;
    int ret;

    if (host_command == NULL || host_command->dst_id != DEVICE_ID) {
        return -EINVAL;
    }
    ret = gateway_survey_roster(&roster);
    if (ret == 0) {
        ret = gateway_operation_owner_claim(
            APP_GATEWAY_OPERATION_OWNER_SURVEY,
            &gateway_survey_operation_lease);
        claimed = ret == 0;
    }
    if (ret == 0) {
        ret = app_survey_gateway_start(&roster, &identity);
    }
    if (ret < 0 && claimed) {
        (void)gateway_operation_owner_release(
            &gateway_survey_operation_lease);
    }
    gateway_emit_host_command_result(
        host_command, CMD_SURVEY_START,
        ret == 0 ? COMMAND_OK :
        ret == -EBUSY ? COMMAND_BUSY :
        ret == -ESTALE || ret == -ENOENT ? COMMAND_INVALID_STATE :
        ret == -EINVAL ? COMMAND_MALFORMED_PAYLOAD :
        COMMAND_INTERNAL_ERROR,
        ret == 0 ? 0u : (uint8_t)(-ret));
    return ret;
}

static int gateway_submit_survey_plan(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    struct survey_host_plan_request request;
    int ret = survey_host_plan_request_extract_tlvs(
        payload, payload_len, &request);

    if (ret == PROTO_OK) {
        ret = app_survey_gateway_submit_plan(&request, NULL);
    } else {
        ret = mesh_errno_from_proto(ret);
    }
    gateway_emit_host_command_result(
        host_command, CMD_SURVEY_PLAN,
        ret == 0 ? COMMAND_OK :
        ret == -EBUSY ? COMMAND_BUSY :
        ret == -ESTALE ? COMMAND_INVALID_STATE :
        ret == -EINVAL || ret == -EBADMSG ? COMMAND_MALFORMED_PAYLOAD :
        COMMAND_INTERNAL_ERROR,
        ret == 0 ? 0u : (uint8_t)(-ret));
    return ret;
}

static int gateway_cancel_survey(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct survey_event current;
    int ret = tlv_find_unique(payload, payload_len, TLV_SURVEY_GENERATION,
                              &value, &value_len);

    if (ret == PROTO_OK && value_len == sizeof(uint32_t)) {
        ret = app_survey_gateway_status(&current);
        if (ret == 0 && current.identity.generation !=
                proto_get_u32_le(value)) {
            ret = -ESTALE;
        }
        if (ret == 0) {
            ret = app_survey_gateway_abort(&current.identity);
        }
    } else {
        ret = -EINVAL;
    }
    gateway_emit_host_command_result(
        host_command, CMD_SURVEY_CANCEL,
        ret == 0 ? COMMAND_OK :
        ret == -ESTALE || ret == -ENOENT ? COMMAND_INVALID_STATE :
        COMMAND_MALFORMED_PAYLOAD,
        ret == 0 ? 0u : (uint8_t)(-ret));
    return ret;
}

static int gateway_get_survey_status(
    const struct proto_packet *host_command)
{
    struct survey_event event;
    int ret = app_survey_gateway_status(&event);

    if (ret == 0) {
        ret = gateway_survey_emit_event(&event);
    }
    gateway_emit_host_command_result(
        host_command, CMD_SURVEY_GET_STATUS,
        ret == 0 ? COMMAND_OK :
        ret == -ENOENT ? COMMAND_INVALID_STATE : COMMAND_INTERNAL_ERROR,
        ret == 0 ? 0u : (uint8_t)(-ret));
    return ret;
}

#endif

void gateway_command_timeout_side_effects(
    const struct proto_packet *command,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum command_id command_id)
{
    if (command == NULL) {
        return;
    }

    mesh_clear_route_waiting_tx(
        command,
        semantic_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC);
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
    if (command_id == CMD_FORCE_REDISCOVERY && status == COMMAND_OK) {
        (void)app_node_comm_request_route_refresh(
            0u, "force-rediscovery-result", false);
    }
}

#if DEVICE_ROLE == ROLE_GATEWAY

#endif

#if DEVICE_ROLE == ROLE_GATEWAY

#endif







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
        if (ret == PROTO_OK && packet->dst_id == DEVICE_ID) {
#if DEVICE_ROLE == ROLE_GATEWAY
            switch (command_id) {
            case CMD_SURVEY_START:
                return gateway_start_survey(packet);
            case CMD_SURVEY_PLAN:
                return gateway_submit_survey_plan(packet, payload,
                                                  payload_len);
            case CMD_SURVEY_CANCEL:
                return gateway_cancel_survey(packet, payload, payload_len);
            case CMD_SURVEY_GET_STATUS:
                return gateway_get_survey_status(packet);
            default:
                break;
            }
#endif
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
 * route-owned dispatch runs on the route workqueue. Keep the compact
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

        if (kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION) {
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
            event.reason = ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
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
static enum gateway_command_event_kind gateway_observability_kind(
    enum command_id command_id)
{
    switch (command_id) {
    case CMD_ASSIGN_DISCOVERY_SLOTS:
        return GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
    case CMD_FORCE_REDISCOVERY:
        return GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH;
    default:
        return 0;
    }
}

static struct gateway_command_event gateway_observability_event(
    enum gateway_command_event_kind kind,
    enum gateway_command_event_stage stage,
    enum command_id command_id,
    const struct proto_packet *host_command,
    uint32_t gateway_sequence)
{
    struct gateway_command_event event = {
        .kind = kind,
        .stage = stage,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = command_id,
        .gateway_epoch = (uint16_t)mesh_runtime.upstream.current_epoch,
        .gateway_sequence = gateway_sequence,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };

    if (host_command != NULL) {
        event.host_session_id = host_command->session_id;
        event.host_seq = host_command->seq;
        event.correlation_id = host_command->session_id != 0u ?
                               host_command->session_id : host_command->seq;
    }
    return event;
}

static void gateway_route_refresh_observe(
    const struct app_node_comm_route_refresh_event *refresh)
{
    struct gateway_command_event event;
    enum gateway_command_event_stage stage;

    if (refresh == NULL || !refresh->correlated ||
        DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
    switch (refresh->kind) {
    case APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT:
        stage = GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT;
        break;
    case APP_NODE_COMM_ROUTE_REFRESH_BACKOFF:
        stage = GATEWAY_COMMAND_EVENT_STAGE_BACKOFF;
        break;
    case APP_NODE_COMM_ROUTE_REFRESH_COMPLETE:
        stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
        break;
    default:
        return;
    }

    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
        stage,
        CMD_FORCE_REDISCOVERY,
        &refresh->correlation,
        refresh->gateway_sequence);
    event.attempt = refresh->attempt;
    event.progress_count = refresh->sent_count;

    if (refresh->kind == APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) {
        uint32_t result_reservation_token =
            gateway_route_refresh_result_token;
        int result_ret;

        gateway_route_refresh_result_token = 0u;
        if (refresh->result == 0) {
            event.status = COMMAND_OK;
        } else if (refresh->result == -ETIMEDOUT) {
            event.status = COMMAND_TIMEOUT;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
        } else if (refresh->result == -EIO ||
                   refresh->result == -ECANCELED) {
            event.status = COMMAND_RADIO_ERROR;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
        } else {
            event.status = COMMAND_INTERNAL_ERROR;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_INTERNAL;
        }
        result_ret = gateway_commit_host_command_result_reserved(
            result_reservation_token,
            &refresh->correlation,
            CMD_FORCE_REDISCOVERY,
            event.status,
            (uint8_t)event.reason);
        if (result_ret < 0) {
            LOG_ERR("gateway route-refresh result custody unavailable: ret=%d",
                    result_ret);
        }
        (void)gateway_observe_command_event(&event, true);
        return;
    }
    if (refresh->kind == APP_NODE_COMM_ROUTE_REFRESH_BACKOFF) {
        event.reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
    }
    (void)gateway_observe_command_event(&event, false);
}

#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observe_host_stage(const struct proto_packet *host_command,
                                       enum command_id command_id,
                                       enum gateway_command_event_stage stage)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event event;

    if (kind == 0 || host_command == NULL) {
        return;
    }
    event = gateway_observability_event(kind, stage, command_id,
                                        host_command, 0u);
    (void)gateway_observe_command_event(&event, false);
}

static int gateway_observe_host_acceptance(
    const struct proto_packet *host_command,
    enum command_id command_id)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event queued;

    if (kind == 0 || host_command == NULL) {
        return 0;
    }
    queued = gateway_observability_event(
        kind, GATEWAY_COMMAND_EVENT_STAGE_QUEUED, command_id,
        host_command, 0u);
    return gateway_observe_command_acceptance_if_available(&queued);
}
#endif

static void gateway_observe_host_terminal(
    const struct proto_packet *host_command,
    enum command_id command_id,
    enum command_status status,
    enum gateway_command_event_reason reason)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event event;

    if (kind == 0 || host_command == NULL ||
        (status == COMMAND_BUSY &&
         gateway_command_result_get_dispatch_token() != 0u)) {
        return;
    }
    event = gateway_observability_event(
        kind, GATEWAY_COMMAND_EVENT_STAGE_COMPLETE, command_id,
        host_command, 0u);
    event.status = status;
    event.reason = reason;
    (void)gateway_observe_command_event(&event, true);
}
