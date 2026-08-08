#line 1 "app_anchor_gateway_survey_round.inc"
#if DEVICE_ROLE == ROLE_GATEWAY

static bool gateway_survey_round_active(void)
{
    return gateway_survey_round.phase != APP_GATEWAY_SURVEY_ROUND_INACTIVE &&
           gateway_survey_round.phase != APP_GATEWAY_SURVEY_ROUND_COMPLETE;
}

static void gateway_survey_round_reset(void)
{
    memset(&gateway_survey_round, 0, sizeof(gateway_survey_round));
    memset(&gateway_survey_start_release,
           0,
           sizeof(gateway_survey_start_release));
    app_gateway_survey_event_runtime_init(
        &gateway_survey_event_runtime);
    gateway_survey_round_observation_started_at_ms = 0u;
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

    /*
     * The route-owner worker must freeze and validate the final plan before
     * entering the concurrent driver.  This is also the boundary at which
     * this event seam begins: discovery and report collection have already
     * completed in their existing owner, so no historical events are replayed
     * here.  Failing closed prevents a caller from creating remote PREPARE
     * state through the old planner shortcut.
     */
    if (!gateway_survey_context.pairs_planned ||
        !gateway_survey_topology_accounted) {
        return PROTO_ERR_MALFORMED;
    }
    if (!app_gateway_survey_event_runtime_active(
            &gateway_survey_event_runtime)) {
        ret = gateway_survey_state_result_to_proto(
            app_gateway_survey_event_runtime_begin_selection(
                &gateway_survey_event_runtime,
                gateway_survey_context.operation_generation,
                (uint64_t)k_uptime_get()));
        if (ret != PROTO_OK) {
            return ret;
        }
    } else if (app_gateway_survey_event_runtime_state(
                   &gateway_survey_event_runtime) != FW_SURVEY_SELECT_PAIRS) {
        return PROTO_ERR_STALE;
    }
    if (gateway_survey_context.pair_count == 0u) {
        ret = gateway_survey_state_event(FW_EVENT_NO_PAIR_AVAILABLE, 0u);
        if (ret != PROTO_OK) {
            return ret;
        }
        return PROTO_ERR_NOT_FOUND;
    }
    ret = gateway_survey_state_event(FW_EVENT_PAIR_AVAILABLE, 0u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = app_gateway_survey_round_begin(&gateway_survey_round,
                                         &gateway_survey_context,
                                         gateway_survey_max_parallel_pairs,
                                         gateway_survey_max_pair_reruns);
    if (ret != PROTO_OK) {
        return ret;
    }
    gateway_survey_round_sync_auto();
    GATEWAY_SURVEY_VERBOSE_LOG("gateway concurrent survey starting: survey=%u reports=%u pairs=%u planned_rounds=%u first_batch=%u parallel_cap=%u reruns=%u",
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

        if (lane == NULL) {
            continue;
        }
        run_ms = gateway_survey_pair_run_delay_ms(&lane->pair);
        if (run_ms > longest_ms) {
            longest_ms = run_ms;
        }
    }
    return longest_ms;
}

static int gateway_survey_round_commitment(
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct survey_round_plan_entry
        entries[SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES];
    const size_t lane_count =
        app_gateway_survey_round_lane_count(&gateway_survey_round);
    const uint32_t execute_delay_ms =
        SURVEY_ROUND_START_EXECUTE_DELAY_MS;
    const uint32_t longest_run_ms =
        gateway_survey_round_longest_run_ms();
    struct survey_round_plan_identity identity = {
        .operation_generation =
            gateway_survey_context.operation_generation,
        .survey_id = gateway_survey_context.survey_id,
        .operation_session_id = survey_operation_session_id(
            gateway_survey_context.operation_generation),
        .execute_delay_ms = execute_delay_ms,
        .observation_window_ms =
            longest_run_ms + SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS,
        .round_id = gateway_survey_round.runtime.batch_sequence,
        .max_parallel_pairs = gateway_survey_max_parallel_pairs,
        .max_reruns = gateway_survey_max_pair_reruns,
    };

    if (commitment == NULL || lane_count == 0u ||
        lane_count > ARRAY_SIZE(entries) ||
        UINT32_MAX - longest_run_ms <
            SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS) {
        return PROTO_ERR_MALFORMED;
    }
    memset(entries, 0, sizeof(entries));
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, i);

        if (lane == NULL) {
            return PROTO_ERR_MALFORMED;
        }
        entries[i] = (struct survey_round_plan_entry) {
            .pair = lane->pair,
            .lane_index = (uint8_t)i,
            .plan_pair_index = lane->plan_pair_index,
            .reruns_started = lane->reruns_started,
        };
    }
    return survey_round_commitment_compute(&identity,
                                           entries,
                                           lane_count,
                                           commitment);
}

static int gateway_survey_round_note_sample(uint64_t reporter_id,
                                            const struct survey_sample *sample,
                                            uint64_t received_at_ms)
{
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    int ret;

    if (!gateway_survey_round_active()) {
        return -ESTALE;
    }
    if (!gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_round_observation_started_at_ms,
            gateway_survey_round_observation_deadline_ms)) {
        LOG_WRN("gateway survey round sample rejected outside receive interval: survey=%u reporter=0x%016llx received=%llu start=%u deadline=%u",
                sample == NULL ? 0u : sample->pair.survey_id,
                (unsigned long long)reporter_id,
                (unsigned long long)received_at_ms,
                gateway_survey_round_observation_started_at_ms,
                gateway_survey_round_observation_deadline_ms);
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

        if (survey_pair_round_lane_preferred_results_complete(lane)) {
            (void)gateway_survey_work_reschedule(0u);
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
        (void)gateway_survey_work_reschedule(GATEWAY_BLE_TX_RETRY_MS);
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

/* Retire every endpoint lease whose ordered START did not yield a result. */
static int gateway_survey_round_start_next_observation_cleanup(void)
{
    const size_t lane_count = app_gateway_survey_round_lane_count(
        &gateway_survey_round);
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int ret;

    if (gateway_survey_round_cleanup_lane_valid) {
        return 1;
    }
    if (app_gateway_survey_round_terminating(&gateway_survey_round)) {
        return 0;
    }
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, i);

        if (lane == NULL ||
            lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP ||
            lane->cleanup_mask == 0u) {
            continue;
        }
        survey_gateway_transaction_init(&gateway_survey_transaction);
        ret = survey_gateway_transaction_load_pair(
            &gateway_survey_transaction, &lane->pair);
        if (ret < 0) {
            return ret;
        }
        gateway_survey_transaction.possible_prepare_mask =
            lane->cleanup_mask;
        survey_gateway_transaction_require_cleanup(
            &gateway_survey_transaction, false, now_ms);
        if (survey_gateway_transaction_cleanup_mask(
                &gateway_survey_transaction) != lane->cleanup_mask) {
            return -EIO;
        }
        gateway_survey_round_cleanup_lane_index = i;
        gateway_survey_round_cleanup_lane_valid = true;
        gateway_survey_begin_cleanup();
        return 1;
    }
    return 0;
}

static bool gateway_survey_round_retire_failed_control_lane(void)
{
    const struct survey_pair_round_lane *lane;

    if (!gateway_survey_round_cleanup_lane_valid) {
        return true;
    }
    lane = app_gateway_survey_round_lane(
        &gateway_survey_round,
        gateway_survey_round_cleanup_lane_index);
    if (lane == NULL) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return false;
    }
    if (lane->state == SURVEY_PAIR_ROUND_LANE_CLEANUP) {
        return true;
    }
    if (lane->state == SURVEY_PAIR_ROUND_LANE_FAILED &&
        !gateway_survey_round_emit_lane_terminal(lane, false)) {
        return false;
    }
    gateway_survey_round_cleanup_lane_valid = false;
    return true;
}

static bool gateway_survey_round_advance_completed_batch(void)
{
    bool complete = false;
    int ret;

    if (!app_gateway_survey_round_batch_complete(&gateway_survey_round)) {
        return false;
    }
    ret = gateway_survey_state_event(
        app_gateway_survey_round_batch_has_result_evidence(
            &gateway_survey_round) ? FW_EVENT_PAIR_RESULT_RECEIVED :
                                      FW_EVENT_TIMER_EXPIRED,
        0u);
    if (ret == PROTO_OK) {
        ret = gateway_survey_state_event(FW_EVENT_GRAPH_BUILT, 0u);
    }
    if (ret != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    ret = app_gateway_survey_round_advance_batch(&gateway_survey_round,
                                                 &complete);
    gateway_survey_round_observation_started_at_ms = 0u;
    gateway_survey_round_observation_deadline_ms = 0u;
    if (ret != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    if (complete) {
        ret = gateway_survey_state_event(FW_EVENT_NO_PAIR_AVAILABLE, 0u);
        if (ret != PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
        gateway_survey_auto_finish();
        return true;
    }
    ret = gateway_survey_state_event(FW_EVENT_PAIR_AVAILABLE, 0u);
    if (ret != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    gateway_survey_round_sync_auto();
    (void)gateway_survey_work_reschedule(0u);
    return true;
}

static bool gateway_survey_round_finalize_observation(void)
{
    const bool deadline = uptime_deadline_reached(
        k_uptime_get_32(), gateway_survey_round_observation_deadline_ms);
    const size_t lane_count = app_gateway_survey_round_lane_count(
        &gateway_survey_round);
    int ret;

    if (deadline) {
        enum gateway_command_result_validation_check validation_check =
            gateway_protocol_validation_check_interval(
                gateway_survey_round_observation_started_at_ms,
                gateway_survey_round_observation_deadline_ms);

        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            return false;
        }
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
    }
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&gateway_survey_round, i);
        enum survey_pair_round_cleanup_outcome outcome;
        bool final_failure;

        if (lane == NULL ||
            lane->state != SURVEY_PAIR_ROUND_LANE_OBSERVING) {
            continue;
        }
        if (survey_pair_round_lane_preferred_results_complete(lane)) {
            if (!gateway_survey_round_emit_lane_terminal(lane, true)) {
                return false;
            }
            ret = app_gateway_survey_round_finalize_lane(
                &gateway_survey_round,
                i,
                0u,
                SURVEY_PAIR_ROUND_CLEANUP_SUCCESS);
            if (ret != PROTO_OK) {
                gateway_survey_auto_finish_status(
                    COMMAND_INTERNAL_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
                return true;
            }
            continue;
        }
        if (!deadline) {
            continue;
        }
        if (survey_pair_round_lane_results_complete(lane)) {
            if (!gateway_survey_round_emit_lane_terminal(lane, true)) {
                return false;
            }
            ret = app_gateway_survey_round_finalize_lane(
                &gateway_survey_round,
                i,
                0u,
                SURVEY_PAIR_ROUND_CLEANUP_SUCCESS);
            if (ret != PROTO_OK) {
                gateway_survey_auto_finish_status(
                    COMMAND_INTERNAL_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
                return true;
            }
            continue;
        }
        final_failure = lane->reruns_started >=
                        gateway_survey_max_pair_reruns;
        outcome = final_failure ? SURVEY_PAIR_ROUND_CLEANUP_FAIL :
                                  SURVEY_PAIR_ROUND_CLEANUP_RETRY;
        ret = app_gateway_survey_round_finalize_lane(
            &gateway_survey_round,
            i,
            SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
            outcome);
        if (ret != PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
    }
    ret = gateway_survey_round_start_next_observation_cleanup();
    if (ret < 0) {
        LOG_ERR("gateway survey observation cleanup admission failed: ret=%d",
                ret);
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return true;
    }
    if (ret > 0) {
        return true;
    }
    return gateway_survey_round_advance_completed_batch();
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
    gateway_survey_round_cleanup_lane_index = lane_index;
    gateway_survey_round_cleanup_lane_valid = true;
    if (cleanup_mask == 0u) {
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
        survey_operation_session_id(
            control.pair.operation_generation) != command->session_id) {
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
     * Keep the next PREPARE or START wake train out of that exchange just
     * as the serial survey path does. Without this ownership boundary the
     * parallel-round driver can start the next control a few milliseconds
     * after the result was decoded, preempting the ACK and leaving the prior
     * target unable to receive the new control.
     */
    survey_gateway_response_ack_settle_note_result(
        &gateway_survey_response_ack_settle,
        (uint64_t)k_uptime_get(),
        gateway_survey_operation_deadline_ms);
    gateway_survey_schedule_drive();
    return true;
}

static void gateway_survey_round_note_cleanup_peer(uint8_t peer_mask)
{
    const struct survey_pair_round_lane *lane;
    int ret;

    if (!gateway_survey_round_cleanup_lane_valid) {
        return;
    }
    if (app_gateway_survey_round_terminating(&gateway_survey_round)) {
        ret = app_gateway_survey_round_note_termination_cleanup_complete(
            &gateway_survey_round,
            gateway_survey_round_cleanup_lane_index,
            peer_mask);
    } else {
        ret = app_gateway_survey_round_note_cleanup_complete(
            &gateway_survey_round,
            gateway_survey_round_cleanup_lane_index,
            peer_mask);
    }
    if (ret != PROTO_OK) {
        gateway_survey_auto_finish_status(
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        return;
    }
    if (app_gateway_survey_round_terminating(&gateway_survey_round)) {
        lane = app_gateway_survey_round_lane(
            &gateway_survey_round,
            gateway_survey_round_cleanup_lane_index);
        if (lane == NULL ||
            lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP) {
            gateway_survey_round_cleanup_lane_valid = false;
        }
        return;
    }
    if (!gateway_survey_round_retire_failed_control_lane()) {
        (void)gateway_survey_work_reschedule(GATEWAY_BLE_TX_RETRY_MS);
    }
}

static bool gateway_survey_round_drive(void)
{
    struct app_gateway_survey_round_control control;
    struct survey_gateway_auto_action action;
    int ret;

    if (!gateway_survey_round_retire_failed_control_lane()) {
        return true;
    }
    if (!gateway_survey_round_active()) {
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
    if (gateway_survey_round.phase == APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        uint32_t remaining_ms;

        if (app_gateway_survey_event_runtime_state(
                &gateway_survey_event_runtime) == FW_SURVEY_ARM_PAIRS &&
            gateway_survey_state_event(FW_EVENT_PAIR_ARMED, 0u) !=
                PROTO_OK) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
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
        (void)gateway_survey_work_reschedule(remaining_ms);
        return true;
    }
    if (gateway_survey_round.phase ==
            APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE) {
        return gateway_survey_round_advance_completed_batch();
    }
    return true;
}

#endif
