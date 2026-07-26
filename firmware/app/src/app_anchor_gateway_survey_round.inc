#line 1 "app_anchor_gateway_survey_round.inc"
#if DEVICE_ROLE == ROLE_GATEWAY

static bool gateway_survey_round_active(void)
{
    return gateway_survey_round.phase != APP_GATEWAY_SURVEY_ROUND_INACTIVE &&
           gateway_survey_round.phase != APP_GATEWAY_SURVEY_ROUND_COMPLETE;
}

static void gateway_survey_round_release_go_delivery(void)
{
    if (gateway_survey_round_go_delivery_handle == 0u) {
        return;
    }
    (void)app_node_comm_abandon_delivery(
        gateway_survey_round_go_delivery_handle);
    gateway_survey_round_go_delivery_handle = 0u;
}

static void gateway_survey_round_reset(void)
{
    gateway_survey_round_release_go_delivery();
    memset(&gateway_survey_round, 0, sizeof(gateway_survey_round));
    gateway_survey_round_observation_deadline_ms = 0u;
    gateway_survey_round_cleanup_lane_index = 0u;
    gateway_survey_round_cleanup_lane_valid = false;
}

static void gateway_survey_round_sync_auto(void)
{
    struct app_gateway_survey_round_control control;
    const struct survey_pair_round_lane *lane;

    (void)survey_gateway_auto_begin(&gateway_survey_auto);
    if (app_gateway_survey_round_current_control(&gateway_survey_round,
                                                 &control) != PROTO_OK) {
        return;
    }
    lane = app_gateway_survey_round_lane(&gateway_survey_round,
                                         control.lane_index);
    gateway_survey_auto.running = true;
    gateway_survey_auto.waiting = false;
    gateway_survey_auto.pair = control.pair;
    gateway_survey_auto.stage = control.stage;
    gateway_survey_auto.pair_reruns_started =
        lane == NULL ? 0u : lane->reruns_started;
}

static int gateway_survey_round_start(void)
{
    int ret;

    if (!gateway_survey_context.pairs_planned) {
        ret = survey_gateway_plan_pairs(&gateway_survey_context);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (gateway_survey_context.pair_count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = app_gateway_survey_round_begin(&gateway_survey_round,
                                         &gateway_survey_context,
                                         gateway_survey_max_parallel_pairs,
                                         gateway_survey_max_pair_reruns);
    if (ret != PROTO_OK) {
        return ret;
    }
    gateway_survey_round_sync_auto();
    LOG_INF("gateway concurrent survey starting: survey=%u reports=%u pairs=%u planned_rounds=%u first_batch=%u parallel_cap=%u reruns=%u",
            gateway_survey_context.survey_id,
            (unsigned int)gateway_survey_context.report_count,
            (unsigned int)gateway_survey_context.pair_count,
            (unsigned int)gateway_survey_round.planned_round_count,
            (unsigned int)app_gateway_survey_round_lane_count(
                &gateway_survey_round),
            gateway_survey_max_parallel_pairs,
            gateway_survey_max_pair_reruns);
    return PROTO_OK;
}

static uint32_t gateway_survey_round_longest_run_ms(void)
{
    uint32_t longest_ms = 0u;

    for (size_t i = 0u;
         i < app_gateway_survey_round_lane_count(&gateway_survey_round);
         i++) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, i);
        uint32_t run_ms;

        if (lane == NULL || !survey_pair_round_lane_armed(lane)) {
            continue;
        }
        run_ms = gateway_survey_pair_run_delay_ms(&lane->pair);
        if (run_ms > longest_ms) {
            longest_ms = run_ms;
        }
    }
    return longest_ms;
}

static int gateway_survey_round_build_go(struct mesh_outbound *outbound,
                                         uint32_t execute_delay_ms,
                                         uint32_t command_seq)
{
    const struct survey_round_go go = {
        .survey_id = gateway_survey_context.survey_id,
        .round_id = gateway_survey_round.runtime.batch_sequence,
    };
    uint32_t expiry_ms;
    uint32_t expiry_s;
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || execute_delay_ms == 0u || command_seq == 0u) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = survey_round_go_append_tlvs(outbound->payload,
                                      sizeof(outbound->payload),
                                      &payload_len,
                                      &go);
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
                             TLV_EXECUTE_DELAY_MS,
                             execute_delay_ms);
    }
    expiry_ms = execute_delay_ms +
                gateway_survey_round_longest_run_ms() +
                SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS + 1000u;
    expiry_s = (expiry_ms + 999u) / 1000u;
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_EXPIRY_S,
                             expiry_s);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_round_go_init_packet(&outbound->packet,
                                      DEVICE_ID,
                                      go.survey_id,
                                      (uint16_t)command_seq,
                                      (uint16_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound->packet.dst_id = MESH_BROADCAST_ID;
    outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = gateway_command_append_default_flood_controls(outbound);
    return ret == PROTO_OK ? 0 : mesh_errno_from_proto(ret);
}

static int gateway_survey_round_submit_go(void)
{
    struct mesh_outbound outbound;
    uint32_t execute_delay_ms = survey_round_go_execute_delay_ms(
        gateway_survey_known_max_report_hops());
    uint32_t longest_run_ms = gateway_survey_round_longest_run_ms();
    uint32_t now_ms = k_uptime_get_32();
    uint32_t command_seq = gateway_next_command_seq();
    uint32_t required_ms;
    int ret;

    required_ms = execute_delay_ms + longest_run_ms +
                  SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS;
    if (uptime_ms_until_deadline(now_ms,
                                 gateway_survey_operation_deadline_ms) <
            required_ms) {
        return -ETIMEDOUT;
    }
    ret = gateway_survey_round_build_go(&outbound,
                                        execute_delay_ms,
                                        command_seq);
    if (ret < 0) {
        return ret;
    }
    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        (uint64_t)now_ms + execute_delay_ms,
        ((uint32_t)CMD_SURVEY_GO << 16) | (uint16_t)command_seq,
        &gateway_survey_round_go_delivery_handle);
    if (ret < 0) {
        gateway_survey_round_go_delivery_handle = 0u;
        return ret;
    }
    gateway_survey_round_observation_deadline_ms = now_ms + required_ms;
    LOG_INF("gateway survey round GO submitted: survey=%u round=%u lanes=%u max_hops=%u execute_delay_ms=%u run_ms=%u handle=%u",
            gateway_survey_context.survey_id,
            gateway_survey_round.runtime.batch_sequence,
            (unsigned int)app_gateway_survey_round_lane_count(
                &gateway_survey_round),
            gateway_survey_known_max_report_hops(),
            execute_delay_ms,
            longest_run_ms,
            gateway_survey_round_go_delivery_handle);
    return 0;
}

static bool gateway_survey_round_ensure_observing(void)
{
    uint8_t attempts_started = 0u;

    if (gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        return true;
    }
    if (!app_gateway_survey_round_go_needed(&gateway_survey_round) ||
        gateway_survey_round_go_delivery_handle == 0u ||
        app_node_comm_delivery_attempts_started(
            gateway_survey_round_go_delivery_handle,
            &attempts_started) < 0 || attempts_started == 0u) {
        return false;
    }
    if (app_gateway_survey_round_mark_observing_after_go(
            &gateway_survey_round) != PROTO_OK) {
        return false;
    }
    LOG_INF("gateway survey round RF started: survey=%u round=%u attempts=%u",
            gateway_survey_context.survey_id,
            gateway_survey_round.runtime.batch_sequence,
            attempts_started);
    return true;
}

static int gateway_survey_round_note_sample(uint64_t reporter_id,
                                            const struct survey_sample *sample)
{
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    int ret;

    if (!gateway_survey_round_active() ||
        !gateway_survey_round_ensure_observing()) {
        return -ESTALE;
    }
    ret = app_gateway_survey_round_note_sample(&gateway_survey_round,
                                                reporter_id,
                                                sample,
                                                &lane_index,
                                                &accepted_new);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_STALE ? -ESTALE : -EBADMSG;
    }
    if (!accepted_new) {
        if (gateway_survey_duplicate_count < UINT16_MAX) {
            gateway_survey_duplicate_count++;
        }
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, lane_index);

        if (survey_pair_round_lane_results_complete(lane) ||
            survey_pair_round_lane_missing_samples_all_unusable(lane)) {
            (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
        }
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static bool gateway_survey_round_emit_lane_terminal(
    const struct survey_pair_round_lane *lane,
    bool success)
{
    struct gateway_command_event event;
    uint16_t observed_count;

    if (lane == NULL) {
        return false;
    }
    observed_count = (uint16_t)__builtin_popcount(lane->usable_result_mask);
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        success ? GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS :
                  GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        CMD_SURVEY_START_PAIR,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);
    event.status = success ? COMMAND_OK : COMMAND_TIMEOUT;
    event.reason = success ? GATEWAY_COMMAND_EVENT_REASON_NONE :
        (lane->initiator_unusable_mask != 0u ||
         lane->responder_unusable_mask != 0u) ?
            GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED :
            GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE;
    event.attempt = (uint8_t)(lane->reruns_started + 1u);
    event.pair_initiator_id = lane->pair.initiator_id;
    event.pair_responder_id = lane->pair.responder_id;
    event.progress_count = observed_count;
    event.total_count = lane->pair.sample_count;
    event.success_count = (uint16_t)(gateway_survey_pair_success_count +
                                    (success ? 1u : 0u));
    event.failure_count = (uint16_t)(gateway_survey_pair_failure_count +
                                    (success ? 0u : 1u));
    event.duplicate_count = gateway_survey_duplicate_count;
    if (gateway_survey_observe_with_custody(&event, false) < 0) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_BLE_TX_RETRY_MS));
        return false;
    }
    if (success) {
        gateway_survey_pair_success_count++;
    } else {
        gateway_survey_pair_failure_count++;
        gateway_survey_terminal_failure_reason =
            gateway_command_survey_failure_reason_merge(
                gateway_survey_terminal_failure_reason,
                event.reason);
    }
    return true;
}

static bool gateway_survey_round_finalize_observation(void)
{
    const bool deadline = uptime_deadline_reached(
        k_uptime_get_32(), gateway_survey_round_observation_deadline_ms);
    const size_t lane_count = app_gateway_survey_round_lane_count(
        &gateway_survey_round);

    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, i);
        enum survey_pair_round_cleanup_outcome outcome;
        bool final_failure;

        if (lane == NULL ||
            lane->state != SURVEY_PAIR_ROUND_LANE_OBSERVING) {
            continue;
        }
        if (survey_pair_round_lane_results_complete(lane)) {
            if (!gateway_survey_round_emit_lane_terminal(lane, true)) {
                return false;
            }
            (void)app_gateway_survey_round_finalize_lane(
                &gateway_survey_round,
                i,
                0u,
                SURVEY_PAIR_ROUND_CLEANUP_SUCCESS);
            continue;
        }
        if (!deadline &&
            !survey_pair_round_lane_missing_samples_all_unusable(lane)) {
            continue;
        }
        final_failure = lane->reruns_started >=
                        gateway_survey_max_pair_reruns;
        outcome = final_failure ? SURVEY_PAIR_ROUND_CLEANUP_FAIL :
                                  SURVEY_PAIR_ROUND_CLEANUP_RETRY;
        if (final_failure &&
            !gateway_survey_round_emit_lane_terminal(lane, false)) {
            return false;
        }
        (void)app_gateway_survey_round_finalize_lane(&gateway_survey_round,
                                                     i,
                                                     0u,
                                                     outcome);
    }
    if (!app_gateway_survey_round_batch_complete(&gateway_survey_round)) {
        return false;
    }
    {
        bool complete = false;
        int ret = app_gateway_survey_round_advance_batch(
            &gateway_survey_round,
            &complete);

        gateway_survey_round_release_go_delivery();
        gateway_survey_round_observation_deadline_ms = 0u;
        if (ret != PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
        if (complete) {
            gateway_survey_auto_finish();
            return true;
        }
    }
    gateway_survey_round_sync_auto();
    (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
    return true;
}

static void gateway_survey_round_fail_current_control(
    enum command_id command_id,
    uint64_t target_id,
    enum gateway_command_event_reason failure_reason)
{
    uint8_t cleanup_mask = survey_gateway_transaction_cleanup_mask(
        &gateway_survey_transaction);
    size_t lane_index = SIZE_MAX;
    int ret;

    ret = app_gateway_survey_round_note_control_failure(
        &gateway_survey_round,
        command_id,
        target_id,
        gateway_survey_context.survey_id,
        cleanup_mask,
        SURVEY_PAIR_ROUND_CLEANUP_RETRY,
        &lane_index);
    if (ret != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return;
    }
    gateway_survey_terminal_failure_reason =
        gateway_command_survey_failure_reason_merge(
            gateway_survey_terminal_failure_reason,
            failure_reason);
    if (cleanup_mask != 0u) {
        gateway_survey_round_cleanup_lane_index = lane_index;
        gateway_survey_round_cleanup_lane_valid = true;
    } else {
        survey_gateway_transaction_pair_complete(
            &gateway_survey_transaction,
            true,
            (uint64_t)k_uptime_get());
    }
    gateway_survey_auto.waiting = false;
    gateway_survey_round_sync_auto();
}

static bool gateway_survey_round_note_control_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    struct app_gateway_survey_round_control control;
    int ret;

    ARG_UNUSED(reason);
    if (!gateway_survey_round_active() || command == NULL ||
        app_gateway_survey_round_current_control(&gateway_survey_round,
                                                 &control) != PROTO_OK ||
        control.command_id != command_id ||
        control.target_id != command->dst_id ||
        control.pair.survey_id != command->session_id) {
        return false;
    }
    if (status != COMMAND_OK) {
        gateway_survey_round_fail_current_control(
            command_id,
            command->dst_id,
            status == COMMAND_TIMEOUT ?
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                GATEWAY_COMMAND_EVENT_REASON_RADIO);
        gateway_survey_begin_cleanup();
        return true;
    }
    ret = app_gateway_survey_round_note_control_success(
        &gateway_survey_round,
        command_id,
        command->dst_id,
        command->session_id);
    if (ret != PROTO_OK ||
        survey_gateway_transaction_phase_complete(
            &gateway_survey_transaction) < 0) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    if (control.stage == SURVEY_GATEWAY_AUTO_START_INITIATOR) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round,
                                          control.lane_index);
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_PAIR_START,
            CMD_SURVEY_START_PAIR,
            &gateway_survey_host_command,
            gateway_survey_context.survey_id);

        survey_gateway_transaction_pair_complete(
            &gateway_survey_transaction,
            true,
            (uint64_t)k_uptime_get());
        event.pair_initiator_id = control.pair.initiator_id;
        event.pair_responder_id = control.pair.responder_id;
        event.attempt = lane == NULL ?
            1u : (uint8_t)(lane->reruns_started + 1u);
        event.progress_count = lane == NULL ?
            0u : (uint16_t)(lane->plan_pair_index + 1u);
        event.total_count = (uint16_t)gateway_survey_context.pair_count;
        ret = app_gateway_survey_observability_submit_boundary(
            &gateway_survey_observability,
            &gateway_survey_observability_ops,
            &event);
        status_debug_printf("DBG_SURVEY_ROUND_PAIR_START survey=%u round=%u initiator=0x%016llx responder=0x%016llx attempt=%u ret=%d pending=%u\n",
                            control.pair.survey_id,
                            gateway_survey_round.runtime.batch_sequence,
                            (unsigned long long)control.pair.initiator_id,
                            (unsigned long long)control.pair.responder_id,
                            event.attempt,
                            ret,
                            gateway_survey_observability.boundary_pending ?
                                1u : 0u);
        if (ret == -EBUSY) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
    }
    gateway_survey_auto.waiting = false;
    gateway_survey_round_sync_auto();
    /*
     * A successful command result is followed by a gateway ACK on Channel 9.
     * Keep the next PREPARE, START, or GO wake train out of that exchange just
     * as the serial survey path does. Without this ownership boundary the
     * parallel-round driver can start the next control a few milliseconds
     * after the result was decoded, preempting the ACK and leaving the prior
     * target unable to receive the new control.
     */
    survey_gateway_response_ack_settle_note_result(
        &gateway_survey_response_ack_settle,
        (uint64_t)k_uptime_get());
    gateway_survey_schedule_drive();
    return true;
}

static void gateway_survey_round_note_cleanup_peer(uint8_t peer_mask)
{
    if (!gateway_survey_round_cleanup_lane_valid) {
        return;
    }
    if (app_gateway_survey_round_note_cleanup_complete(
            &gateway_survey_round,
            gateway_survey_round_cleanup_lane_index,
            peer_mask) != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return;
    }
    {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(
                &gateway_survey_round,
                gateway_survey_round_cleanup_lane_index);

        if (lane != NULL && lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP) {
            gateway_survey_round_cleanup_lane_valid = false;
        }
    }
}

static bool gateway_survey_round_drive(void)
{
    struct app_gateway_survey_round_control control;
    struct survey_gateway_auto_action action;
    struct node_comm_terminal_event event;
    int ret;

    if (!gateway_survey_round_active()) {
        if (!gateway_survey_emit_collection_telemetry()) {
            return true;
        }
        ret = gateway_survey_round_start();
        if (ret == PROTO_ERR_NOT_FOUND) {
            gateway_survey_auto_finish();
            return true;
        }
        if (ret != PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
    }
    if (gateway_survey_cleanup_pending() ||
        gateway_survey_observability.boundary_pending) {
        return true;
    }
    if (gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_DISPATCHING) {
        ret = app_gateway_survey_round_current_control(&gateway_survey_round,
                                                       &control);
        if (ret != PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
        gateway_survey_round_sync_auto();
        action = (struct survey_gateway_auto_action) {
            .pair = control.pair,
            .stage = control.stage,
            .command_id = control.command_id,
            .target_id = control.target_id,
        };
        ret = gateway_survey_auto_send_action(&action);
        if (ret < 0) {
            survey_gateway_transaction_require_cleanup(
                &gateway_survey_transaction,
                false,
                (uint64_t)k_uptime_get());
            gateway_survey_round_fail_current_control(
                control.command_id,
                control.target_id,
                ret == -EHOSTUNREACH || ret == -ENOTCONN ?
                    GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE :
                    GATEWAY_COMMAND_EVENT_REASON_RADIO);
            gateway_survey_begin_cleanup();
        }
        return true;
    }
    if (app_gateway_survey_round_go_needed(&gateway_survey_round)) {
        if (gateway_survey_round_go_delivery_handle == 0u) {
            ret = gateway_survey_round_submit_go();
            if (ret < 0) {
                if (app_gateway_survey_round_go_submit_retryable(ret)) {
                    status_debug_printf(
                        "DBG_SURVEY_ROUND_GO_ADMISSION_RETRY survey=%u round=%u ret=%d\n",
                        gateway_survey_context.survey_id,
                        gateway_survey_round.runtime.batch_sequence,
                        ret);
                    (void)k_work_reschedule(
                        &gateway_survey_work,
                        K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
                } else {
                    gateway_survey_auto_finish_status(
                        ret == -ETIMEDOUT ? COMMAND_TIMEOUT :
                                            COMMAND_RADIO_ERROR,
                        ret == -ETIMEDOUT ?
                            GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                            GATEWAY_COMMAND_EVENT_REASON_RADIO);
                }
            } else {
                (void)k_work_reschedule(
                    &gateway_survey_work,
                    K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
            }
            return true;
        }
        (void)gateway_survey_round_ensure_observing();
        if (!app_node_comm_take_delivery_event_for(
                gateway_survey_round_go_delivery_handle,
                &event)) {
            (void)k_work_reschedule(
                &gateway_survey_work,
                K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
            return true;
        }
        LOG_INF("gateway survey round GO terminal: survey=%u round=%u reason=%u attempts=%u",
                gateway_survey_context.survey_id,
                gateway_survey_round.runtime.batch_sequence,
                (unsigned int)event.reason,
                event.attempts_started);
        if (app_gateway_survey_round_go_terminal_retryable(
                event.reason, event.attempts_started)) {
            gateway_survey_round_go_delivery_handle = 0u;
            status_debug_printf(
                "DBG_SURVEY_ROUND_GO_ZERO_ATTEMPT_RETRY survey=%u round=%u reason=%u\n",
                gateway_survey_context.survey_id,
                gateway_survey_round.runtime.batch_sequence,
                (unsigned int)event.reason);
            (void)k_work_reschedule(
                &gateway_survey_work,
                K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
            return true;
        }
        if (event.attempts_started == 0u) {
            gateway_survey_round_go_delivery_handle = 0u;
            gateway_survey_auto_finish_status(
                COMMAND_RADIO_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_RADIO);
            return true;
        }
        (void)gateway_survey_round_ensure_observing();
        gateway_survey_round_go_delivery_handle = 0u;
    }
    if (gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        uint32_t remaining_ms;

        if (gateway_survey_round_go_delivery_handle != 0u &&
            app_node_comm_take_delivery_event_for(
                gateway_survey_round_go_delivery_handle,
                &event)) {
            LOG_INF("gateway survey round GO terminal during observation: survey=%u round=%u reason=%u attempts=%u",
                    gateway_survey_context.survey_id,
                    gateway_survey_round.runtime.batch_sequence,
                    (unsigned int)event.reason,
                    event.attempts_started);
            gateway_survey_round_go_delivery_handle = 0u;
        }
        if (gateway_survey_round_finalize_observation()) {
            return true;
        }
        remaining_ms = uptime_ms_until_deadline(
            k_uptime_get_32(),
            gateway_survey_round_observation_deadline_ms);
        if (remaining_ms == 0u || remaining_ms > GATEWAY_BLE_TX_RETRY_MS) {
            remaining_ms = GATEWAY_BLE_TX_RETRY_MS;
        }
        (void)k_work_reschedule(
            &gateway_survey_work,
            K_MSEC(remaining_ms));
        return true;
    }
    return true;
}

#endif
