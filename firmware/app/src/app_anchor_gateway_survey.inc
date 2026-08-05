#line 2118 __BASE_FILE__
#if defined(CONFIG_IMEC_HIGH_DEBUG)
#define GATEWAY_SURVEY_VERBOSE_LOG(...) LOG_INF(__VA_ARGS__)
#else
/*
 * Production survey progress is already exported through retained BLE events
 * and typed stack-evidence records. Keep verbose success-path text in the
 * explicit high-debug preset so the release gateway retains upgrade margin;
 * warnings and errors remain unconditional below.
 */
#define GATEWAY_SURVEY_VERBOSE_LOG(...) do { } while (0)
#endif

static int gateway_survey_work_reschedule(uint32_t delay_ms)
{
    int ret = mesh_route_owner_work_reschedule(
        &gateway_survey_work, delay_ms);

    if (ret < 0) {
        /*
         * Every active survey transition is owned by this single delayed
         * worker.  A rejected reschedule has no independent timer that can
         * restore progress, so retain the transaction for diagnostics and
         * force watchdog recovery instead of silently stranding it.
         */
        LOG_ERR("gateway survey liveness scheduling failed: delay_ms=%u ret=%d",
                delay_ms,
                ret);
        status_debug_printf(
            "DBG_SURVEY_SCHEDULE_FAILED delay_ms=%u ret=%d\n",
            delay_ms,
            ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

#if DEVICE_ROLE == ROLE_GATEWAY
static void gateway_manual_survey_pair_reset(void)
{
    bool owned = gateway_manual_survey_pair_state.valid;

    if (gateway_manual_survey_pair_state.go_delivery_handle != 0u) {
        int ret = app_node_comm_abandon_delivery(
            gateway_manual_survey_pair_state.go_delivery_handle);

        if (ret < 0) {
            LOG_ERR("manual survey GO abandonment failed: handle=%u ret=%d",
                    gateway_manual_survey_pair_state.go_delivery_handle,
                    ret);
            app_watchdog_stop_feeding();
            return;
        }
    }
    memset(&gateway_manual_survey_pair_state,
           0,
           sizeof(gateway_manual_survey_pair_state));
    if (owned) {
        gateway_operation_owner_release(
            GATEWAY_OPERATION_OWNER_MANUAL_SURVEY);
    }
}
#endif

/*
 * Survey admission uses the radio-reported first receive time, never the
 * route-owner worker's processing time.  These intervals are all shorter than
 * the signed 32-bit uptime horizon, so the ordinary wrap-safe deadline
 * comparison preserves the closed-open [start, deadline) contract.
 */
static uint32_t gateway_survey_operation_started_at_ms;
static uint32_t gateway_survey_collection_started_at_ms;
static uint32_t gateway_survey_collection_finalize_cutoff_ms;
static bool gateway_survey_collection_finalize_cutoff_valid;
static uint32_t gateway_survey_pair_observation_started_at_ms;
static uint32_t gateway_survey_pair_observation_deadline_ms;
#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_survey_round_observation_started_at_ms;
#endif
static void gateway_survey_pair_identity_reset(void);

static bool gateway_survey_receive_in_interval(uint64_t received_at_ms,
                                               uint32_t started_at_ms,
                                               uint32_t deadline_ms)
{
    uint32_t received_at_32 = (uint32_t)received_at_ms;

    return deadline_ms != started_at_ms &&
           uptime_deadline_reached(deadline_ms, started_at_ms) &&
           uptime_deadline_reached(received_at_32, started_at_ms) &&
           !uptime_deadline_reached(received_at_32, deadline_ms);
}

static uint32_t gateway_survey_collection_receive_deadline_ms(void)
{
    return gateway_survey_collection_finalize_cutoff_valid ?
           gateway_survey_collection_finalize_cutoff_ms :
           gateway_survey_collection_deadline_ms;
}

static void gateway_survey_collection_freeze_finalize_cutoff(
    uint64_t received_at_ms)
{
    uint32_t cutoff_ms = (uint32_t)received_at_ms + 1u;

    if (uptime_deadline_reached(cutoff_ms,
                                gateway_survey_collection_deadline_ms)) {
        cutoff_ms = gateway_survey_collection_deadline_ms;
    }
    gateway_survey_collection_finalize_cutoff_ms = cutoff_ms;
    gateway_survey_collection_finalize_cutoff_valid = true;
}

static bool gateway_command_uses_survey_mesh(enum command_id command_id)
{
    return command_id == CMD_SURVEY_REACHABILITY ||
           command_id == CMD_SURVEY_PREPARE_PAIR ||
           command_id == CMD_SURVEY_START_PAIR ||
           command_id == CMD_SURVEY_ABORT;
}

static int gateway_extract_survey_sample_count(const uint8_t *payload,
                                                size_t payload_len,
                                                uint16_t *sample_count)
{
    uint16_t value = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
    int ret;

    if (sample_count == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = command_find_u16_tlv(payload, payload_len, TLV_SAMPLE_COUNT, &value);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (!survey_sample_count_valid(value)) {
        return PROTO_ERR_MALFORMED;
    }

    *sample_count = value;
    return PROTO_OK;
}

static enum gateway_command_event_kind gateway_observability_kind(
    enum command_id command_id)
{
    switch (command_id) {
    case CMD_ASSIGN_DISCOVERY_SLOTS:
        return GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
    case CMD_SURVEY_REACHABILITY:
    case CMD_SURVEY_PREPARE_PAIR:
    case CMD_SURVEY_START_PAIR:
    case CMD_SURVEY_ABORT:
        return GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY;
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

        gateway_route_refresh_result_token = 0u;
        if (refresh->result == 0) {
            event.status = COMMAND_OK;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
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
        gateway_emit_host_command_result_reserved(
            result_reservation_token,
            &refresh->correlation,
            CMD_FORCE_REDISCOVERY,
            event.status,
            (uint8_t)event.reason);
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
    event = gateway_observability_event(kind,
                                        stage,
                                        command_id,
                                        host_command,
                                        0u);
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

    if (kind == 0 || host_command == NULL) {
        return;
    }
    event = gateway_observability_event(kind,
                                        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
                                        command_id,
                                        host_command,
                                        0u);
    event.status = status;
    event.reason = reason;
    (void)gateway_observe_command_event(&event, true);
}

static void gateway_observe_survey_terminal(
    enum command_status status,
    enum gateway_command_event_reason reason)
{
    struct gateway_command_event event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        CMD_SURVEY_REACHABILITY,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);

    event.status = status;
    event.reason = reason;
    event.progress_count = (uint16_t)gateway_survey_context.next_pair_index;
    event.total_count = (uint16_t)gateway_survey_context.pair_count;
    event.success_count = gateway_survey_pair_success_count;
    event.failure_count = (uint16_t)MIN(
        (uint32_t)gateway_survey_pair_failure_count +
            gateway_survey_discovery_failure_count,
        UINT16_MAX);
    event.duplicate_count = gateway_survey_duplicate_count;
    (void)gateway_observe_command_event(&event, true);
}

static int gateway_survey_observe_with_custody(
    struct gateway_command_event *event,
    bool terminal)
{
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
    return gateway_observe_command_event_if_available(event, terminal, NULL);
#else
    return gateway_observe_command_event(event, terminal);
#endif
}

static int gateway_survey_observe_callback(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx)
{
    ARG_UNUSED(ctx);
    return gateway_survey_observe_with_custody(event, terminal);
}

static const struct app_gateway_survey_observability_ops
    gateway_survey_observability_ops = {
        .emit_if_available = gateway_survey_observe_callback,
        .ctx = NULL,
    };

static bool gateway_survey_flush_boundary_event(void)
{
    int ret = app_gateway_survey_observability_flush_boundary(
        &gateway_survey_observability, &gateway_survey_observability_ops);

    if (ret < 0) {
        (void)gateway_survey_work_reschedule(GATEWAY_BLE_TX_RETRY_MS);
        return false;
    }
    return true;
}

static bool gateway_survey_emit_collection_telemetry(void)
{
    struct gateway_command_event base = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_SURVEY_REACHABILITY,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);
    int ret = app_gateway_survey_observability_emit_collection_next(
        &gateway_survey_observability, &gateway_survey_observability_ops,
        &gateway_survey_context, &base, gateway_survey_duplicate_count);

    if (ret <= 0) {
        (void)gateway_survey_work_reschedule(
            ret < 0 ? GATEWAY_BLE_TX_RETRY_MS : 0u);
        return false;
    }
    return true;
}

static int gateway_reject_survey_request(
    const struct proto_packet *host_packet,
    enum command_status status,
    uint8_t protocol_reason,
    enum gateway_command_event_reason event_reason,
    int error)
{
    gateway_emit_host_command_result(host_packet,
                                     CMD_SURVEY_REACHABILITY,
                                     status,
                                     protocol_reason);
    if (host_packet != NULL) {
        gateway_observe_host_terminal(host_packet,
                                      CMD_SURVEY_REACHABILITY,
                                      status,
                                      event_reason);
    }
    return error;
}

static uint8_t gateway_survey_known_max_report_hops(void)
{
    uint8_t max_hops = 0u;

    /*
     * The downlink cache is intentionally smaller than the survey roster, so
     * it cannot be the sole timing authority once discovery reports have been
     * frozen.  Every accepted report retains its reverse-path hop count.
     */
    for (size_t i = 0u; i < gateway_survey_context.report_count; i++) {
        struct survey_gateway_reverse_hint reverse_hint;
        enum command_status report_status;
        uint64_t anchor_id;
        size_t entry_count;
        int ret = survey_gateway_report_info_at(
            &gateway_survey_context,
            i,
            &anchor_id,
            &entry_count,
            &report_status);

        if (ret != PROTO_OK) {
            return SURVEY_DEFAULT_TTL;
        }
        ret = survey_gateway_reverse_hint_for_target(
            &gateway_survey_context, anchor_id, &reverse_hint);
        if (ret == PROTO_OK && reverse_hint.hop_count > max_hops) {
            max_hops = reverse_hint.hop_count;
        } else if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
            return SURVEY_DEFAULT_TTL;
        }
    }
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(&mesh_runtime); i++) {
        const struct mesh_downlink_entry *route =
            mesh_relay_downlink_at(&mesh_runtime, i);

        if (route != NULL && route->valid &&
            route->gateway_id == DEVICE_ID &&
            route->target_id != DEVICE_ID &&
            route->hop_count > max_hops) {
            max_hops = route->hop_count;
        }
    }
    return max_hops;
}

static int gateway_route_survey_reachability(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len)
{
    const struct survey_discovery_config discovery_config = {
        .survey_id = 0u,
        .start_delay_ms = SURVEY_DISCOVERY_START_DELAY_MS,
        .slot_ms = SURVEY_DISCOVERY_SLOT_MS,
        .slot_count = SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
        .round_count = OPERATION_POLICY_DISCOVERY_DEFAULT_ROUND_COUNT,
    };
    struct app_operation_policy_candidate policy_candidate;
    struct operation_policy_discovery admitted_discovery;
    struct operation_policy_discovery_budget_terms budget_terms;
    struct survey_discovery_config config = discovery_config;
    struct mesh_outbound outbound = {0};
    uint32_t survey_id = 0u;
    uint64_t operation_generation = 0u;
    uint32_t duration_ms = 0u;
    uint32_t discovery_duration_ms = 0u;
    uint32_t report_mesh_duration_ms = 0u;
    uint32_t report_custody_ms;
    uint32_t emission_horizon_delay_ms = 0u;
    uint32_t collection_delay_ms = 0u;
    uint32_t command_budget_ms = SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS;
    uint32_t required_budget_ms = 0u;
    uint32_t command_origin_ms;
    uint64_t command_origin_uptime_ms;
    uint32_t flood_delivery_handle = 0u;
    uint16_t sample_count = UWB_SAMPLES_PER_ANCHOR;
    uint16_t expected_node_count = 0u;
    bool budget_explicit = false;
    bool expected_node_count_present = false;
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (host_packet->dst_id != MESH_BROADCAST_ID && host_packet->dst_id != DEVICE_ID)) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_DENIED, 1u,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
    }
    if (gateway_survey_active ||
        gateway_assignment_publication_pending()
#if DEVICE_ROLE == ROLE_GATEWAY
        || gateway_survey_cleanup_pending()
#endif
    ) {
        LOG_WRN("gateway survey reachability rejected while survey active or cleaning up: current=%u requested_dst=0x%016llx",
                gateway_survey_context.survey_id,
                (unsigned long long)host_packet->dst_id);
        return gateway_reject_survey_request(
            host_packet, COMMAND_BUSY, 3u,
            GATEWAY_COMMAND_EVENT_REASON_BUSY, -EBUSY);
    }

    ret = app_operation_policy_prepare_payload(host_payload,
                                               host_payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_DISCOVERY_MASK |
                                                   APP_OPERATION_POLICY_PAIR_MASK,
                                               &policy_candidate);
    if (ret < 0) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, EBADMSG,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EBADMSG);
    }

    ret = survey_extract_reach_request_tlvs(host_payload,
                                            host_payload_len,
                                            &survey_id,
                                            &duration_ms);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    ret = survey_extract_expected_node_count_tlv(
        host_payload, host_payload_len, &expected_node_count,
        &expected_node_count_present);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    ret = gateway_command_extract_budget_ms(host_payload,
                                            host_payload_len,
                                            SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
                                            &command_budget_ms,
                                            &budget_explicit);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }

    ret = gateway_extract_survey_sample_count(host_payload,
                                              host_payload_len,
                                              &sample_count);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    {
        enum command_status admission_status;
        enum gateway_command_event_reason admission_reason;

        if (!gateway_command_survey_sample_admission(sample_count,
                                                     &admission_status,
                                                     &admission_reason)) {
        LOG_WRN("gateway survey reachability rejected: samples=%u runtime_max=%u",
                sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
            return gateway_reject_survey_request(host_packet,
                                                 admission_status,
                                                 4u,
                                                 admission_reason,
                                                 -EINVAL);
        }
    }
    ret = survey_extract_discovery_slot_count_tlv(host_payload,
                                                  host_payload_len,
                                                  SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
                                                  &config.slot_count);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    if (policy_candidate.updates.discovery_present) {
        if (duration_ms !=
                policy_candidate.updates.discovery.report_grace_ms ||
            config.slot_count !=
                policy_candidate.updates.discovery.slot_count ||
            (budget_explicit &&
             command_budget_ms !=
                policy_candidate.updates.discovery.operation_budget_ms)) {
            return gateway_reject_survey_request(
                host_packet, COMMAND_MALFORMED_PAYLOAD, EINVAL,
                GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
        }
        config.start_delay_ms =
            policy_candidate.updates.discovery.start_delay_ms;
        config.slot_ms = policy_candidate.updates.discovery.slot_ms;
        config.slot_count = policy_candidate.updates.discovery.slot_count;
        config.round_count = policy_candidate.updates.discovery.round_count;
        duration_ms =
            policy_candidate.updates.discovery.report_grace_ms;
        command_budget_ms =
            policy_candidate.updates.discovery.operation_budget_ms;
        budget_explicit = true;
    }
    config.survey_id = survey_id;
    discovery_duration_ms = survey_discovery_duration_ms(&config);
    report_custody_ms = survey_discovery_report_custody_ms(
        gateway_survey_known_max_report_hops());
    ret = survey_discovery_report_delay_ms(&config,
                                           config.slot_count - 1u,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_mesh_duration_ms);
    if (ret == PROTO_OK && UINT32_MAX - report_mesh_duration_ms >=
        SURVEY_RESULT_MESH_SLOT_MS) {
        report_mesh_duration_ms += SURVEY_RESULT_MESH_SLOT_MS;
    } else {
        ret = PROTO_ERR_NO_SPACE;
    }
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    if (discovery_duration_ms == 0u ||
        UINT32_MAX - config.start_delay_ms < report_mesh_duration_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms < duration_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms - duration_ms <
            report_custody_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms - duration_ms -
            report_custody_ms <
            SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, 2u,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
    }
    emission_horizon_delay_ms = config.start_delay_ms +
                                report_mesh_duration_ms + duration_ms;
    collection_delay_ms = emission_horizon_delay_ms +
                          report_custody_ms +
                          SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS;
    admitted_discovery = (struct operation_policy_discovery) {
        .start_delay_ms = config.start_delay_ms,
        .slot_ms = config.slot_ms,
        .slot_count = config.slot_count,
        .round_count = config.round_count,
        .report_grace_ms = duration_ms,
        .operation_budget_ms = command_budget_ms,
    };
    budget_terms = (struct operation_policy_discovery_budget_terms) {
        .report_slot_ms = SURVEY_RESULT_MESH_SLOT_MS,
        .report_custody_ms = report_custody_ms,
        .report_delivery_tail_ms =
            SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS,
        .terminal_scheduling_guard_ms =
            GATEWAY_SURVEY_OPERATION_TERMINAL_SCHEDULING_GUARD_MS,
    };
    ret = operation_policy_discovery_required_budget_ms(
        &admitted_discovery, &budget_terms, &required_budget_ms);
    if (ret != PROTO_OK || command_budget_ms < required_budget_ms) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, EINVAL,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
    }
    if (budget_explicit) {
        /*
         * Collection is one indivisible phase.  If the host deadline is
         * shorter than the natural collection horizon, wake at that deadline
         * so the worker reports an operation timeout; never close collection
         * at an arbitrary fair-share point and mislabel later valid reports as
         * absent.
         */
        collection_delay_ms = gateway_command_budget_window_ms(
            true, command_budget_ms, 1u, collection_delay_ms);
    }

    ret = app_mesh_persistence_reserve_gateway_survey_generation(
        DEVICE_ID, &operation_generation);
    if (ret < 0) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL, ret);
    }
    config.operation_generation = operation_generation;
    ret = survey_append_discovery_start_tlvs(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             &config);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
    if (ret == PROTO_OK) {
        const struct operation_policy discovery_policy = {
            .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
            .value.discovery = policy_candidate.resolved.discovery,
        };

        ret = operation_policy_append_tlv(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          &discovery_policy);
    }
    if (ret == PROTO_OK) {
        const struct operation_policy pair_policy = {
            .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
            .value.pair = policy_candidate.resolved.pair,
        };

        ret = operation_policy_append_tlv(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          &pair_policy);
    }
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    ret = survey_init_discovery_start_packet(&outbound.packet,
                                             DEVICE_ID,
                                             &config,
                                             seq,
                                             (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = MESH_BROADCAST_ID;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    command_origin_uptime_ms = (uint64_t)k_uptime_get();
    command_origin_ms = (uint32_t)command_origin_uptime_ms;
    outbound.queued_at_ms = command_origin_ms;
    outbound.queued_at_valid = true;

#if DEVICE_ROLE == ROLE_GATEWAY
    if (!gateway_operation_owner_claim(
            GATEWAY_OPERATION_OWNER_AUTO_SURVEY)) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_BUSY, 4u,
            GATEWAY_COMMAND_EVENT_REASON_BUSY, -EBUSY);
    }
#endif
    ret = survey_gateway_begin_operation(&gateway_survey_context,
                                         survey_id,
                                         operation_generation,
                                         sample_count);
    if (ret != PROTO_OK) {
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_operation_owner_release(
            GATEWAY_OPERATION_OWNER_AUTO_SURVEY);
#endif
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
    ret = survey_gateway_auto_begin(&gateway_survey_auto);
    if (ret != PROTO_OK) {
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_operation_owner_release(
            GATEWAY_OPERATION_OWNER_AUTO_SURVEY);
#endif
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    survey_gateway_transaction_init(&gateway_survey_transaction);
    gateway_survey_round_reset();
    memset(&gateway_survey_sequential_run, 0,
           sizeof(gateway_survey_sequential_run));
    memset(&gateway_survey_cleanup, 0, sizeof(gateway_survey_cleanup));
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
    gateway_survey_finish_pending = false;
#endif
    gateway_survey_active = true;
    gateway_survey_operation_started_at_ms = command_origin_ms;
    gateway_survey_operation_deadline_ms = command_origin_ms + command_budget_ms;
    gateway_survey_host_command = *host_packet;
    gateway_survey_duplicate_count = 0u;
    gateway_survey_pair_success_count = 0u;
    gateway_survey_pair_failure_count = 0u;
    gateway_survey_discovery_failure_count = 0u;
    gateway_survey_topology_accounted = false;
    gateway_survey_terminal_failure_reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    gateway_survey_pair_result_mask = 0u;
    gateway_survey_pair_responder_usable_mask = 0u;
    gateway_survey_pair_initiator_unusable_mask = 0u;
    gateway_survey_pair_responder_unusable_mask = 0u;
    gateway_survey_pair_identity_reset();
    gateway_survey_pair_observation_active = false;
    gateway_survey_max_pair_reruns =
        policy_candidate.resolved.pair.max_reruns;
    gateway_survey_max_parallel_pairs =
        policy_candidate.resolved.pair.max_parallel_pairs;
    survey_gateway_response_ack_settle_init(
        &gateway_survey_response_ack_settle);
    app_gateway_survey_observability_reset(&gateway_survey_observability);
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING,
            CMD_SURVEY_REACHABILITY,
            host_packet,
            survey_id);

        event.total_count = config.slot_count;
        (void)gateway_observe_command_event(&event, false);
    }

    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        command_origin_uptime_ms + config.start_delay_ms,
        ((uint32_t)CMD_SURVEY_REACHABILITY << 16) | seq,
        &flood_delivery_handle);
    if (ret < 0) {
        gateway_survey_active = false;
        gateway_survey_operation_started_at_ms = 0u;
        gateway_survey_operation_deadline_ms = 0u;
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_operation_owner_release(
            GATEWAY_OPERATION_OWNER_AUTO_SURVEY);
#endif
        (void)survey_gateway_auto_begin(&gateway_survey_auto);
        (void)k_work_cancel_delayable(&gateway_survey_work);
        return gateway_reject_survey_request(
            host_packet,
            ret == -EBUSY ? COMMAND_BUSY : COMMAND_RADIO_ERROR,
            (uint8_t)(-ret),
            ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                            GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION,
            ret);
    }
    app_operation_policy_commit_prepared(&policy_candidate);
    gateway_survey_discovery_delivery_handle = flood_delivery_handle;
    /*
     * The validated collection budget is measured from command admission.
     * Freeze that absolute horizon before the flood starts; arming collection
     * after the flood terminal must not restart the same budget.
     */
    gateway_survey_collection_deadline_ms =
        command_origin_ms + collection_delay_ms;
    gateway_survey_collection_emission_deadline_ms =
        command_origin_ms + emission_horizon_delay_ms;
    gateway_survey_collection_started_at_ms = command_origin_ms;
    gateway_survey_collection_finalize_cutoff_ms = 0u;
    gateway_survey_collection_finalize_cutoff_valid = false;
    gateway_survey_collection_duration_ms = collection_delay_ms;
    gateway_survey_collection_window_armed = false;
    gateway_survey_collection_pending = true;
    gateway_survey_expected_node_count = expected_node_count;
    gateway_survey_expected_node_count_present =
        expected_node_count_present;

    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT,
            CMD_SURVEY_REACHABILITY,
            host_packet,
            survey_id);

        event.attempt = 1u;
        event.total_count = config.slot_count;
        (void)gateway_observe_command_event(&event, false);
    }

    (void)gateway_survey_work_reschedule(
        GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS);
    gateway_emit_host_command_result(host_packet, CMD_SURVEY_REACHABILITY, COMMAND_OK, 0u);
    GATEWAY_SURVEY_VERBOSE_LOG("gateway survey discovery broadcast: survey=%u start_delay_ms=%u slot_ms=%u slots=%u rounds=%u discovery_ms=%u report_train_end_ms=%u report_grace_ms=%u report_custody_ms=%u expected=%u expected_present=%u samples=%u seq=%u",
            survey_id,
            config.start_delay_ms,
            config.slot_ms,
            config.slot_count,
            config.round_count,
            discovery_duration_ms,
            report_mesh_duration_ms,
            duration_ms,
            report_custody_ms,
            expected_node_count,
            expected_node_count_present ? 1u : 0u,
            sample_count,
            seq);
    return 0;
}

static void gateway_survey_pair_identity_reset(void)
{
    memset(gateway_survey_pair_initiator_identities,
           SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID,
           sizeof(gateway_survey_pair_initiator_identities));
    memset(gateway_survey_pair_responder_identities,
           SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID,
           sizeof(gateway_survey_pair_responder_identities));
}

static int gateway_survey_pair_sample_identity(
    const struct survey_sample *sample,
    uint64_t reporter_id,
    bool apply,
    bool *duplicate)
{
    struct survey_sample_observation_identity *identities;
    struct survey_sample_observation_identity identity;
    struct survey_sample_observation_identity *existing_identity;
    int ret;

    if (sample == NULL || duplicate == NULL ||
        sample->pair.sample_count >
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT ||
        sample->sample_index >=
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT ||
        (reporter_id != sample->pair.initiator_id &&
         reporter_id != sample->pair.responder_id)) {
        return PROTO_ERR_MALFORMED;
    }
    identities = reporter_id == sample->pair.responder_id ?
        gateway_survey_pair_responder_identities :
        gateway_survey_pair_initiator_identities;
    ret = survey_sample_observation_identity_capture(sample, &identity);
    if (ret != PROTO_OK) {
        return ret;
    }
    existing_identity = &identities[sample->sample_index];
    if (survey_sample_observation_identity_valid(existing_identity) &&
        !survey_sample_observation_identity_equal(
            existing_identity, &identity)) {
        return PROTO_ERR_MALFORMED;
    }
    *duplicate =
        survey_sample_observation_identity_valid(existing_identity);
    if (apply && !*duplicate) {
        *existing_identity = identity;
    }
    return PROTO_OK;
}

#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_manual_survey_pair_matches_sample(
    const struct survey_sample *sample,
    uint64_t reporter_id)
{
    const struct gateway_manual_survey_pair_state *manual =
        &gateway_manual_survey_pair_state;

    return manual->valid && !manual->cleanup_requested &&
           manual->go_rf_started &&
           manual->round_id != SURVEY_LEGACY_ROUND_ID &&
           sample != NULL &&
           reporter_id != 0u &&
           (manual->prepare_possible_mask &
            (reporter_id == manual->pair.initiator_id ?
                 SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK :
             reporter_id == manual->pair.responder_id ?
                 SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK : 0u)) != 0u &&
           survey_sample_matches_pair_run(
               sample, &manual->pair, manual->round_id);
}

static int gateway_manual_survey_pair_preflight_sample(
    uint64_t reporter_id,
    const struct survey_sample *sample,
    uint64_t received_at_ms)
{
    bool duplicate = false;
    int ret;

    if (!gateway_manual_survey_pair_matches_sample(sample, reporter_id) ||
        uptime_deadline_reached(
            (uint32_t)received_at_ms,
            gateway_manual_survey_pair_state.deadline_ms)) {
        return -ESTALE;
    }
    ret = gateway_survey_pair_sample_identity(
        sample, reporter_id, false, &duplicate);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    return duplicate ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                       APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static int gateway_manual_survey_pair_note_sample(
    uint64_t reporter_id,
    const struct survey_sample *sample,
    uint64_t received_at_ms)
{
    const uint16_t expected_mask =
        sample != NULL && sample->pair.sample_count == 16u ?
            UINT16_MAX :
        sample == NULL || sample->pair.sample_count == 0u ?
            0u :
            (uint16_t)((1u << sample->pair.sample_count) - 1u);
    uint16_t *result_mask;
    bool duplicate = false;
    int ret;

    ret = gateway_manual_survey_pair_preflight_sample(
        reporter_id, sample, received_at_ms);
    if (ret != APP_GATEWAY_SEMANTIC_ACCEPT_NEW) {
        return ret;
    }
    ret = gateway_survey_pair_sample_identity(
        sample, reporter_id, true, &duplicate);
    if (ret != PROTO_OK || duplicate) {
        return -EBADMSG;
    }
    result_mask =
        reporter_id == gateway_manual_survey_pair_state.pair.responder_id ?
            &gateway_manual_survey_pair_state.responder_result_mask :
            &gateway_manual_survey_pair_state.initiator_result_mask;
    *result_mask |= (uint16_t)(1u << sample->sample_index);
    if (expected_mask != 0u &&
        gateway_manual_survey_pair_state.initiator_result_mask ==
            expected_mask &&
        gateway_manual_survey_pair_state.responder_result_mask ==
            expected_mask) {
        gateway_manual_survey_pair_reset();
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_survey_round_preflight_sample(
    uint64_t reporter_id,
    const struct survey_sample *sample,
    uint64_t received_at_ms)
{
    enum survey_pair_round_lane_state admissible_lane_state;
    uint8_t attempts_started = 0u;
    bool duplicate = false;
    int ret;

    if (!gateway_survey_round_active() || sample == NULL) {
        return -ESTALE;
    }
    if (gateway_survey_round.phase ==
        APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        admissible_lane_state = SURVEY_PAIR_ROUND_LANE_OBSERVING;
    } else if (gateway_survey_round.phase ==
                   APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED &&
               app_gateway_survey_round_go_needed(&gateway_survey_round) &&
               gateway_survey_round_go_delivery_handle != 0u &&
               app_node_comm_peek_delivery_attempts_started(
                   gateway_survey_round_go_delivery_handle,
                   &attempts_started) == 0 &&
               attempts_started != 0u) {
        /*
         * The commit path will perform the same deterministic ARMED to
         * OBSERVING transition. Preview it by admitting only currently armed
         * lanes; do not consume the delivery terminal or mutate the round.
         */
        admissible_lane_state = SURVEY_PAIR_ROUND_LANE_ARMED;
    } else {
        return -ESTALE;
    }
    if (!gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_round_observation_started_at_ms,
            gateway_survey_round_observation_deadline_ms)) {
        return -ESTALE;
    }
    ret = app_gateway_survey_round_preflight_sample(
        &gateway_survey_round,
        admissible_lane_state,
        reporter_id,
        sample,
        NULL,
        &duplicate);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_STALE ? -ESTALE : -EBADMSG;
    }
    return duplicate ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                       APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}
#endif

static int gateway_survey_preflight_pair_result(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t received_at_ms)
{
    struct survey_sample sample = {0};
    uint16_t expected_transport_sequence;
    bool duplicate = false;
    int ret;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -EINVAL;
    }
    if (survey_extract_sample_tlvs(payload, payload_len, &sample) !=
        PROTO_OK) {
        return -EBADMSG;
    }
    if (sample.pair.operation_generation == 0u ||
        packet->session_id != survey_operation_session_id(
            sample.pair.operation_generation) ||
        (packet->src_id != sample.pair.initiator_id &&
         packet->src_id != sample.pair.responder_id)) {
        return -EPROTO;
    }
    ret = survey_pair_result_transport_sequence(
        sample.round_id,
        sample.sample_index,
        &expected_transport_sequence);
    if (ret != PROTO_OK ||
        packet->seq != expected_transport_sequence) {
        return -EPROTO;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_round_active()) {
        return gateway_survey_round_preflight_sample(
            packet->src_id, &sample, received_at_ms);
    }
    if (gateway_manual_survey_pair_state.valid) {
        return gateway_manual_survey_pair_preflight_sample(
            packet->src_id, &sample, received_at_ms);
    }
#endif
    if (!gateway_survey_active || !gateway_survey_auto.running ||
        !gateway_survey_pair_observation_active ||
        sample.pair.operation_generation !=
            gateway_survey_auto.pair.operation_generation ||
        sample.pair.survey_id != gateway_survey_auto.pair.survey_id ||
        sample.pair.initiator_id != gateway_survey_auto.pair.initiator_id ||
        sample.pair.responder_id != gateway_survey_auto.pair.responder_id ||
        sample.pair.sample_count != gateway_survey_auto.pair.sample_count ||
        !gateway_survey_sequential_run.valid ||
        !survey_sample_matches_pair_run(
            &sample,
            &gateway_survey_sequential_run.pair,
            gateway_survey_sequential_run.round_id) ||
        sample.sample_index >= 16u ||
        !gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_pair_observation_started_at_ms,
            gateway_survey_pair_observation_deadline_ms)) {
        return -ESTALE;
    }

    ret = gateway_survey_pair_sample_identity(
        &sample, packet->src_id, false, &duplicate);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    return duplicate ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                       APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static int gateway_survey_preflight_discovery_report(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t received_at_ms,
    uint64_t previous_hop_id,
    uint8_t link_quality)
{
    struct survey_reachability_entry
        entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    const uint8_t *status_raw = NULL;
    uint32_t survey_id = 0u;
    uint64_t operation_generation = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    uint8_t flood_attempts_started = 0u;
    uint8_t status_len = 0u;
    enum command_status report_status = COMMAND_OK;
    uint32_t receive_deadline_ms;
    int ret;

    ret = survey_extract_reach_report_tlvs(
        payload,
        payload_len,
        &survey_id,
        &anchor_id,
        entries,
        ARRAY_SIZE(entries),
        &entry_count);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    ret = survey_operation_generation_extract_tlv(
        payload, payload_len, &operation_generation);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_STATUS,
                          &status_raw,
                          &status_len);
    if (ret != PROTO_OK ||
        status_len != sizeof(uint16_t) ||
        proto_get_u16_le(status_raw) > COMMAND_INTERNAL_ERROR) {
        return -EBADMSG;
    }
    report_status =
        (enum command_status)proto_get_u16_le(status_raw);
    if (packet->session_id !=
            survey_operation_session_id(operation_generation) ||
        packet->src_id != anchor_id) {
        return -EPROTO;
    }
    ret = survey_reachability_report_endpoints_validate(
        anchor_id, DEVICE_ID, entries, entry_count);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    if (!gateway_survey_active ||
        gateway_survey_context.operation_generation !=
            operation_generation ||
        gateway_survey_context.survey_id != survey_id) {
        return -ESTALE;
    }
    if (!gateway_survey_collection_window_armed &&
        (gateway_survey_discovery_delivery_handle == 0u ||
         app_node_comm_peek_delivery_attempts_started(
             gateway_survey_discovery_delivery_handle,
             &flood_attempts_started) < 0 ||
         flood_attempts_started == 0u)) {
        return -EAGAIN;
    }
    receive_deadline_ms = gateway_survey_collection_receive_deadline_ms();
    if (!gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_collection_started_at_ms,
            receive_deadline_ms)) {
        return -ESTALE;
    }
    ret = survey_gateway_reach_report_compare(
        &gateway_survey_context,
        anchor_id,
        entries,
        entry_count,
        report_status);
    if (ret == PROTO_OK) {
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    if (ret != PROTO_ERR_NOT_FOUND) {
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    if (gateway_survey_auto.running) {
        return -ESTALE;
    }
    if (gateway_survey_context.report_count >=
        SURVEY_GATEWAY_MAX_REPORTS) {
        return -ENOSPC;
    }
    if (!mesh_id_is_unicast(previous_hop_id) ||
        previous_hop_id == DEVICE_ID || link_quality > 100u) {
        return -EPROTO;
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

int gateway_survey_preflight_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t received_at_ms,
    uint64_t previous_hop_id,
    uint8_t radio_channel,
    uint8_t link_quality)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL || payload == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT &&
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -ENOMSG;
    }
    if (packet->dst_id != DEVICE_ID ||
        packet->payload_len != payload_len ||
        !mesh_id_is_unicast(packet->src_id) ||
        packet->src_id == DEVICE_ID ||
        radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        packet->flags !=
            (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC) ||
        !mesh_id_is_unicast(previous_hop_id) ||
        previous_hop_id == DEVICE_ID ||
        link_quality > 100u) {
        return -EPROTO;
    }
    if (packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        return gateway_survey_preflight_pair_result(
            packet, payload, payload_len, received_at_ms);
    }
    return gateway_survey_preflight_discovery_report(
        packet,
        payload,
        payload_len,
        received_at_ms,
        previous_hop_id,
        link_quality);
}

static int gateway_note_survey_pair_result(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t received_at_ms)
{
    struct survey_sample sample = {0};
    bool changed = false;
    bool duplicate = false;
    int ret;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -EINVAL;
    }
    if (survey_extract_sample_tlvs(payload, payload_len, &sample) !=
        PROTO_OK) {
        return -EBADMSG;
    }
    if (sample.pair.operation_generation == 0u ||
        packet->session_id != survey_operation_session_id(
            sample.pair.operation_generation) ||
        (packet->src_id != sample.pair.initiator_id &&
         packet->src_id != sample.pair.responder_id)) {
        return -EPROTO;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_round_active()) {
        return gateway_survey_round_note_sample(packet->src_id,
                                                &sample,
                                                received_at_ms);
    }
    if (gateway_manual_survey_pair_state.valid) {
        return gateway_manual_survey_pair_note_sample(
            packet->src_id, &sample, received_at_ms);
    }
#endif
    if (!gateway_survey_active || !gateway_survey_auto.running ||
        !gateway_survey_pair_observation_active ||
        sample.pair.operation_generation !=
            gateway_survey_auto.pair.operation_generation ||
        sample.pair.survey_id != gateway_survey_auto.pair.survey_id ||
        sample.pair.initiator_id != gateway_survey_auto.pair.initiator_id ||
        sample.pair.responder_id != gateway_survey_auto.pair.responder_id ||
        sample.pair.sample_count != gateway_survey_auto.pair.sample_count ||
        !gateway_survey_sequential_run.valid ||
        !survey_sample_matches_pair_run(
            &sample,
            &gateway_survey_sequential_run.pair,
            gateway_survey_sequential_run.round_id) ||
        sample.sample_index >= 16u) {
        return -ESTALE;
    }
    if (!gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_pair_observation_started_at_ms,
            gateway_survey_pair_observation_deadline_ms)) {
        LOG_WRN("gateway survey pair result rejected outside receive interval: survey=%u src=0x%016llx received=%llu start=%u deadline=%u",
                sample.pair.survey_id,
                (unsigned long long)packet->src_id,
                (unsigned long long)received_at_ms,
                gateway_survey_pair_observation_started_at_ms,
                gateway_survey_pair_observation_deadline_ms);
        return -ESTALE;
    }

    ret = gateway_survey_pair_sample_identity(
        &sample, packet->src_id, false, &duplicate);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    if (duplicate) {
        if (gateway_survey_duplicate_count < UINT16_MAX) {
            gateway_survey_duplicate_count++;
        }
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    ret = survey_pair_note_sample_masks(
        &sample,
        packet->src_id,
        &gateway_survey_pair_result_mask,
        &gateway_survey_pair_responder_usable_mask,
        &gateway_survey_pair_initiator_unusable_mask,
        &gateway_survey_pair_responder_unusable_mask,
        &changed);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ret = gateway_survey_pair_sample_identity(
        &sample, packet->src_id, true, &duplicate);
    if (ret != PROTO_OK || duplicate) {
        return -EBADMSG;
    }
    if ((uint16_t)__builtin_popcount(
            gateway_survey_pair_responder_usable_mask) ==
        gateway_survey_auto.pair.sample_count) {
        (void)gateway_survey_work_reschedule(0u);
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static int gateway_handle_survey_discovery_report(const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len,
                                                   uint64_t received_at_ms,
                                                   uint64_t previous_hop_id,
                                                   uint8_t radio_channel,
                                                   uint8_t link_quality)
{
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct survey_gateway_reverse_hint reverse_hint;
    uint32_t survey_id = 0u;
    uint64_t operation_generation = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    uint8_t flood_attempts_started = 0u;
    uint32_t receive_deadline_ms;
    enum command_status report_status = COMMAND_OK;
    bool duplicate_report = false;
    int ret;

    ret = gateway_survey_preflight_semantic_delivery(
        packet,
        payload,
        payload_len,
        received_at_ms,
        previous_hop_id,
        radio_channel,
        link_quality);
    if (ret < 0) {
        LOG_WRN("gateway survey discovery report rejected transport: src=0x%016llx prev=0x%016llx channel=%u flags=0x%02x quality=%u",
                (unsigned long long)(packet == NULL ? 0u : packet->src_id),
                (unsigned long long)previous_hop_id,
                radio_channel,
                packet == NULL ? 0u : packet->flags,
                link_quality);
        return ret;
    }
    if (packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        return gateway_note_survey_pair_result(
            packet, payload, payload_len, received_at_ms);
    }

    ret = survey_extract_reach_report_tlvs(payload,
                                           payload_len,
                                           &survey_id,
                                           &anchor_id,
                                           entries,
                                           ARRAY_SIZE(entries),
                                           &entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report rejected: ret=%d src=0x%016llx session=%u",
                ret,
                (unsigned long long)packet->src_id,
                packet->session_id);
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    ret = survey_operation_generation_extract_tlv(
        payload, payload_len, &operation_generation);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    {
        const uint8_t *status_raw = NULL;
        uint8_t status_len = 0u;

        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_COMMAND_STATUS,
                              &status_raw,
                              &status_len);
        if (ret != PROTO_OK ||
            status_len != sizeof(uint16_t)) {
            return -EBADMSG;
        }
        uint16_t status_value = proto_get_u16_le(status_raw);

        if (status_value > COMMAND_INTERNAL_ERROR) {
            return -EBADMSG;
        }
        report_status = (enum command_status)status_value;
    }
    if (packet->session_id !=
            survey_operation_session_id(operation_generation) ||
        packet->src_id != anchor_id) {
        LOG_WRN("gateway survey discovery report identity mismatch: pkt_session=%u survey=%u src=0x%016llx anchor=0x%016llx",
                packet->session_id,
                survey_id,
                (unsigned long long)packet->src_id,
                (unsigned long long)anchor_id);
        return -EPROTO;
    }
    ret = survey_reachability_report_endpoints_validate(
        anchor_id, DEVICE_ID, entries, entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report has invalid endpoint: survey=%u anchor=0x%016llx entries=%u ret=%d",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                ret);
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    if (!gateway_survey_active ||
        gateway_survey_context.operation_generation !=
            operation_generation ||
        gateway_survey_context.survey_id != survey_id) {
        LOG_WRN("gateway survey discovery report stale: survey=%u active=%u current=%u anchor=0x%016llx",
                survey_id,
                gateway_survey_active ? 1u : 0u,
                gateway_survey_context.survey_id,
                (unsigned long long)anchor_id);
        return -ESTALE;
    }
    if (!gateway_survey_collection_window_armed &&
        (gateway_survey_discovery_delivery_handle == 0u ||
         app_node_comm_delivery_attempts_started(
             gateway_survey_discovery_delivery_handle,
             &flood_attempts_started) < 0 ||
         flood_attempts_started == 0u)) {
        return -EAGAIN;
    }
    receive_deadline_ms = gateway_survey_collection_receive_deadline_ms();
    if (!gateway_survey_receive_in_interval(
            received_at_ms,
            gateway_survey_collection_started_at_ms,
            receive_deadline_ms)) {
        LOG_WRN("gateway survey discovery report rejected outside receive interval: survey=%u anchor=0x%016llx received=%llu start=%u deadline=%u",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned long long)received_at_ms,
                gateway_survey_collection_started_at_ms,
                receive_deadline_ms);
        return -ESTALE;
    }
    ret = survey_gateway_reach_report_compare(
        &gateway_survey_context,
        anchor_id,
        entries,
        entry_count,
        report_status);
    if (ret == PROTO_OK) {
        if (gateway_survey_duplicate_count < UINT16_MAX) {
            gateway_survey_duplicate_count++;
        }
        duplicate_report = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret == PROTO_ERR_NO_SPACE ? -EMSGSIZE : -EBADMSG;
    }
    if (duplicate_report) {
        GATEWAY_SURVEY_VERBOSE_LOG("gateway survey duplicate report ignored: survey=%u anchor=0x%016llx entries=%u prev=0x%016llx",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                (unsigned long long)previous_hop_id);
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    if (gateway_survey_auto.running) {
        LOG_WRN("gateway survey discovery report ignored after orchestration start: survey=%u anchor=0x%016llx entries=%u",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count);
        return -ESTALE;
    }

    reverse_hint = (struct survey_gateway_reverse_hint) {
        .target_id = anchor_id,
        .next_hop_id = previous_hop_id,
        .quality = link_quality,
        .hop_count = survey_gateway_hop_count_from_report_ttl(packet->ttl),
        .valid = true,
    };
    ret = survey_gateway_note_reach_report_with_reverse_hint_status(
        &gateway_survey_context,
        survey_id,
        anchor_id,
        entries,
        entry_count,
        report_status,
        &reverse_hint);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report not recorded: survey=%u anchor=0x%016llx entries=%u ret=%d",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                ret);
        if (ret == PROTO_ERR_NO_SPACE) {
            return -ENOSPC;
        }
        return ret == PROTO_ERR_STALE ? -ESTALE : -EBADMSG;
    }
    if (gateway_survey_expected_node_count_present &&
        !gateway_survey_collection_finalize_cutoff_valid &&
        gateway_survey_context.report_count ==
            gateway_survey_expected_node_count) {
        /*
         * Expected-count completion is caused by this physical receipt.  One
         * tick makes the strict upper bound include that causal millisecond;
         * the frozen safety deadline remains the outer cap.
         */
        gateway_survey_collection_freeze_finalize_cutoff(received_at_ms);
        (void)gateway_survey_work_reschedule(0u);
    }
    app_stack_workload_diag_gateway_report_cycle(
        packet, (uint16_t)gateway_survey_context.report_count, 1u);
    if (report_status != COMMAND_OK) {
        if (gateway_survey_discovery_failure_count < UINT16_MAX) {
            gateway_survey_discovery_failure_count++;
        }
        gateway_survey_terminal_failure_reason =
            gateway_command_survey_failure_reason_merge(
                gateway_survey_terminal_failure_reason,
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
        status_debug_printf(
            "DBG_SURVEY_REPORT_INCOMPLETE survey=%u anchor=0x%016llx status=%u\n",
            survey_id,
            (unsigned long long)anchor_id,
            report_status);
    }

    ret = survey_gateway_plan_pairs(&gateway_survey_context);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey pair planning failed: survey=%u reports=%u ret=%d",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                ret);
        return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }

    if (gateway_survey_context.pair_count > 0u) {
        struct survey_pair pair;

        ret = survey_gateway_pair_at(&gateway_survey_context, 0u, &pair);
        if (ret != PROTO_OK) {
            LOG_WRN("gateway survey first pair unavailable: survey=%u ret=%d",
                    survey_id,
                    ret);
            return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
        }

        GATEWAY_SURVEY_VERBOSE_LOG("gateway survey report recorded: survey=%u reports=%u pairs=%u first=0x%016llx->0x%016llx samples=%u",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count,
                (unsigned long long)pair.initiator_id,
                (unsigned long long)pair.responder_id,
                pair.sample_count);
    } else {
        GATEWAY_SURVEY_VERBOSE_LOG("gateway survey report recorded: survey=%u reports=%u pairs=0",
                survey_id,
                (unsigned int)gateway_survey_context.report_count);
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static uint32_t gateway_survey_pair_run_delay_ms(const struct survey_pair *pair)
{
    uint32_t per_sample_ms = SURVEY_PAIR_RESPONDER_WINDOW_MS + SURVEY_PAIR_SAMPLE_GAP_MS;

    if (pair == NULL || pair->sample_count == 0u) {
        return GATEWAY_SURVEY_AUTO_RETRY_MS;
    }
    if (pair->sample_count > (UINT32_MAX - GATEWAY_SURVEY_PAIR_SETTLE_MS) / per_sample_ms) {
        return UINT32_MAX;
    }
    return ((uint32_t)pair->sample_count * per_sample_ms) + GATEWAY_SURVEY_PAIR_SETTLE_MS;
}

enum gateway_survey_pair_finalize_result {
    GATEWAY_SURVEY_PAIR_FINALIZE_CONTINUE = 0,
    GATEWAY_SURVEY_PAIR_FINALIZE_RETRY,
    GATEWAY_SURVEY_PAIR_FINALIZE_TERMINAL,
    GATEWAY_SURVEY_PAIR_FINALIZE_VALIDATION_EXPIRED,
};

static enum gateway_survey_pair_finalize_result
gateway_survey_finalize_pair_observation(void)
{
    struct gateway_command_event event;
    enum gateway_command_result_validation_check validation_check;
    uint16_t observed_count;
    bool deadline;
    bool success;

    if (!gateway_survey_pair_observation_active) {
        return GATEWAY_SURVEY_PAIR_FINALIZE_CONTINUE;
    }
    observed_count = (uint16_t)__builtin_popcount(
        gateway_survey_pair_result_mask);
    deadline = uptime_deadline_reached(
        k_uptime_get_32(),
        gateway_survey_pair_observation_deadline_ms);
    if ((uint16_t)__builtin_popcount(
            gateway_survey_pair_responder_usable_mask) !=
            gateway_survey_auto.pair.sample_count &&
        !deadline) {
        return GATEWAY_SURVEY_PAIR_FINALIZE_CONTINUE;
    }
    success = observed_count == gateway_survey_auto.pair.sample_count;
    if (deadline) {
        validation_check = gateway_protocol_validation_check_interval(
            gateway_survey_pair_observation_started_at_ms,
            gateway_survey_pair_observation_deadline_ms);
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            (void)gateway_survey_work_reschedule(
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return GATEWAY_SURVEY_PAIR_FINALIZE_RETRY;
        }
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            return GATEWAY_SURVEY_PAIR_FINALIZE_VALIDATION_EXPIRED;
        }
    }
    if (!success &&
        (gateway_survey_pair_initiator_unusable_mask != 0u ||
         gateway_survey_pair_responder_unusable_mask != 0u) &&
        gateway_survey_auto.pair_reruns_started <
            gateway_survey_max_pair_reruns &&
        survey_gateway_auto_rerun_pair(&gateway_survey_auto) == PROTO_OK) {
        uint8_t run_number =
            (uint8_t)(gateway_survey_auto.pair_reruns_started + 1u);

        gateway_survey_pair_observation_active = false;
        gateway_survey_pair_observation_started_at_ms = 0u;
        gateway_survey_pair_observation_deadline_ms = 0u;
        gateway_survey_pair_result_mask = 0u;
        gateway_survey_pair_responder_usable_mask = 0u;
        gateway_survey_pair_initiator_unusable_mask = 0u;
        gateway_survey_pair_responder_unusable_mask = 0u;
        gateway_survey_pair_identity_reset();
#if DEVICE_ROLE == ROLE_GATEWAY
        survey_gateway_transaction_pair_complete(
            &gateway_survey_transaction, false, (uint64_t)k_uptime_get());
        gateway_survey_begin_cleanup();
#endif
        LOG_WRN("gateway survey pair unusable; rerunning full pair: survey=%u initiator=0x%016llx responder=0x%016llx run=%u/%u",
                gateway_survey_auto.pair.survey_id,
                (unsigned long long)gateway_survey_auto.pair.initiator_id,
                (unsigned long long)gateway_survey_auto.pair.responder_id,
                run_number,
                gateway_survey_max_pair_reruns + 1u);
        return GATEWAY_SURVEY_PAIR_FINALIZE_RETRY;
    }
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        success ? GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS :
                  GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        CMD_SURVEY_START_PAIR,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);
    event.status = success ? COMMAND_OK : COMMAND_TIMEOUT;
    event.reason = success ? GATEWAY_COMMAND_EVENT_REASON_NONE :
                   (gateway_survey_pair_initiator_unusable_mask != 0u ||
                    gateway_survey_pair_responder_unusable_mask != 0u) ?
                   GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED :
                   GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE;
    event.attempt =
        (uint8_t)(gateway_survey_auto.pair_reruns_started + 1u);
    event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
    event.pair_responder_id = gateway_survey_auto.pair.responder_id;
    event.progress_count = observed_count;
    event.total_count = gateway_survey_auto.pair.sample_count;
    event.success_count = (uint16_t)(gateway_survey_pair_success_count +
                                    (success ? 1u : 0u));
    event.failure_count = (uint16_t)(gateway_survey_pair_failure_count +
                                    (success ? 0u : 1u));
    event.duplicate_count = gateway_survey_duplicate_count;
    if (gateway_survey_observe_with_custody(&event, false) < 0) {
        (void)gateway_survey_work_reschedule(GATEWAY_BLE_TX_RETRY_MS);
        return GATEWAY_SURVEY_PAIR_FINALIZE_RETRY;
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
    gateway_survey_pair_observation_active = false;
    gateway_survey_pair_observation_started_at_ms = 0u;
    gateway_survey_pair_observation_deadline_ms = 0u;
    gateway_survey_pair_result_mask = 0u;
    gateway_survey_pair_responder_usable_mask = 0u;
    gateway_survey_pair_initiator_unusable_mask = 0u;
    gateway_survey_pair_responder_unusable_mask = 0u;
    gateway_survey_pair_identity_reset();
#if DEVICE_ROLE == ROLE_GATEWAY
    survey_gateway_transaction_pair_complete(&gateway_survey_transaction,
                                             success,
                                             (uint64_t)k_uptime_get());
    if (!success) {
        gateway_survey_begin_cleanup();
        if (survey_gateway_auto_no_unstarted_pairs(
                &gateway_survey_auto, &gateway_survey_context)) {
            return GATEWAY_SURVEY_PAIR_FINALIZE_TERMINAL;
        }
        return GATEWAY_SURVEY_PAIR_FINALIZE_RETRY;
    }
#endif
    return GATEWAY_SURVEY_PAIR_FINALIZE_CONTINUE;
}

#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_survey_round_release_go_delivery(void);
static void gateway_survey_finish_cleanup_if_complete(uint64_t now_ms);
#endif

/*
 * Returns zero once terminal cleanup custody is installed (or no survey is
 * active). A negative result means a caller such as host SURVEY_ABORT must not
 * acknowledge success because an exact transport owner could not be retained.
 */
static int gateway_survey_auto_finish_status(
    enum command_status status,
    enum gateway_command_event_reason reason)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    const struct survey_pair *active_pair = NULL;
    enum node_transaction_action action;
    uint8_t active_cleanup_mask;
    size_t active_lane_index = SIZE_MAX;
    uint64_t now_ms;
    int ret;
#endif

    if (!gateway_survey_active) {
        return 0;
    }
    GATEWAY_SURVEY_VERBOSE_LOG("gateway survey orchestration complete: survey=%u planned_pairs=%u",
            gateway_survey_context.survey_id,
            (unsigned int)gateway_survey_context.pair_count);
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_finish_pending_status = status;
    gateway_survey_finish_pending_reason = reason;
    gateway_survey_finish_pending = true;
    now_ms = (uint64_t)k_uptime_get();
    survey_gateway_transaction_require_cleanup(
        &gateway_survey_transaction, false, now_ms);
    active_cleanup_mask = survey_gateway_transaction_cleanup_mask(
        &gateway_survey_transaction);
    if (active_cleanup_mask != 0u) {
        if (!gateway_survey_transaction.pair_loaded) {
            LOG_ERR("gateway survey finish has cleanup without pair custody");
            app_watchdog_stop_feeding();
            return -EIO;
        }
        active_pair = &gateway_survey_transaction.pair;
    }
    if (gateway_survey_round_active() &&
        !app_gateway_survey_round_terminating(&gateway_survey_round)) {
        ret = app_gateway_survey_round_begin_termination(
            &gateway_survey_round,
            active_pair,
            active_cleanup_mask,
            &active_lane_index);
        if (ret != PROTO_OK) {
            LOG_ERR("gateway survey round termination custody rejected: ret=%d mask=0x%02x",
                    ret,
                    active_cleanup_mask);
            app_watchdog_stop_feeding();
            return mesh_errno_from_proto(ret);
        }
        if (active_cleanup_mask != 0u) {
            gateway_survey_round_cleanup_lane_index = active_lane_index;
            gateway_survey_round_cleanup_lane_valid = true;
        }
    }
    if (!gateway_survey_round_release_go_delivery()) {
        return -EIO;
    }
    if (gateway_survey_discovery_delivery_handle != 0u) {
        ret = app_node_comm_abandon_delivery(
            gateway_survey_discovery_delivery_handle);
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            LOG_ERR("gateway survey discovery abandonment failed: handle=%u ret=%d",
                    gateway_survey_discovery_delivery_handle,
                    ret);
            app_watchdog_stop_feeding();
            return ret;
        }
        gateway_survey_discovery_delivery_handle = 0u;
    }
    if (gateway_survey_transaction.active.state != NODE_TRANSACTION_EMPTY &&
        !gateway_survey_transaction.active.request_delivery_terminal) {
        ret = gateway_survey_cancel_take_active_delivery(&action);

        if (ret < 0) {
            LOG_WRN("gateway survey finish delivery cancel/take pending: ret=%d",
                    ret);
            (void)gateway_survey_work_reschedule(
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return 0;
        }
    }
#endif

    /*
     * A terminal outcome cancels the live observation. Running normal pair
     * finalization here can schedule a rerun or block on telemetry before
     * ABORT custody is established.
     */
    gateway_survey_pair_observation_active = false;
    gateway_survey_pair_result_mask = 0u;
    gateway_survey_pair_responder_usable_mask = 0u;
    gateway_survey_pair_initiator_unusable_mask = 0u;
    gateway_survey_pair_responder_unusable_mask = 0u;
    gateway_survey_pair_identity_reset();
    gateway_survey_collection_pending = false;
    gateway_survey_collection_started_at_ms = 0u;
    gateway_survey_collection_deadline_ms = 0u;
    gateway_survey_collection_emission_deadline_ms = 0u;
    gateway_survey_collection_finalize_cutoff_ms = 0u;
    gateway_survey_collection_finalize_cutoff_valid = false;
    gateway_survey_collection_duration_ms = 0u;
    gateway_survey_collection_window_armed = false;
    gateway_survey_expected_node_count = 0u;
    gateway_survey_expected_node_count_present = false;
    gateway_survey_active = false;
    gateway_survey_operation_started_at_ms = 0u;
    gateway_survey_operation_deadline_ms = 0u;
    gateway_survey_pair_observation_started_at_ms = 0u;
    gateway_survey_pair_observation_deadline_ms = 0u;
#if DEVICE_ROLE == ROLE_GATEWAY
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
#endif
    survey_gateway_response_ack_settle_init(
        &gateway_survey_response_ack_settle);
    if (gateway_survey_pending_command_valid) {
        app_stack_workload_diag_gateway_control_release(
            &gateway_survey_pending_command, -ECANCELED, 0u, 0u);
        gateway_clear_pending_command_result(&gateway_survey_pending_command);
        gateway_survey_pending_command_valid = false;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_begin_cleanup();
    if (gateway_survey_cleanup_pending()) {
        (void)gateway_survey_work_reschedule(
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
    } else {
        /*
         * The no-cleanup case retires through the same finalizer so terminal
         * observability, round reset, and owner release retain one ordering.
         */
        gateway_survey_finish_cleanup_if_complete(
            (uint64_t)k_uptime_get());
    }
#else
    gateway_observe_survey_terminal(status, reason);
    (void)k_work_cancel_delayable(&gateway_survey_work);
#endif
    (void)survey_gateway_auto_begin(&gateway_survey_auto);
    return 0;
}

static void gateway_survey_auto_finish(void)
{
    enum command_status status;
    enum gateway_command_event_reason reason;

    gateway_command_survey_terminal_outcome(
        gateway_survey_context.report_count,
        gateway_survey_context.pair_count,
        gateway_survey_context.pairs_planned,
        gateway_survey_context.topology_complete,
        (uint16_t)MIN(
            (uint32_t)gateway_survey_pair_failure_count +
                gateway_survey_discovery_failure_count,
            UINT16_MAX),
        gateway_survey_terminal_failure_reason,
        &status,
        &reason);
    gateway_survey_auto_finish_status(status, reason);
}

static int gateway_survey_prepare_pair_control(struct mesh_outbound *outbound,
                                               uint8_t *hop_count_out)
{
    const struct mesh_downlink_entry *downlink;
    struct survey_gateway_reverse_hint reverse_hint;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = survey_gateway_reverse_hint_for_target(&gateway_survey_context,
                                                 outbound->packet.dst_id,
                                                 &reverse_hint);
    if (ret == PROTO_OK) {
        ret = mesh_relay_note_gateway_survey_reverse_route(
            &mesh_runtime,
            reverse_hint.target_id,
            reverse_hint.next_hop_id,
            reverse_hint.quality,
            k_uptime_get_32());
        if (ret != PROTO_OK) {
            LOG_WRN("gateway survey reverse route install failed: dst=0x%016llx next=0x%016llx ret=%d",
                    (unsigned long long)reverse_hint.target_id,
                    (unsigned long long)reverse_hint.next_hop_id,
                    ret);
            return mesh_errno_from_proto(ret);
        }
        outbound->next_hop_id = reverse_hint.next_hop_id;
        if (hop_count_out != NULL) {
            *hop_count_out = reverse_hint.hop_count;
        }
    } else if (ret == PROTO_ERR_NOT_FOUND) {
        downlink = mesh_relay_find_current_downlink(
            &mesh_runtime, outbound->packet.dst_id);
        if (downlink == NULL) {
            return -EHOSTUNREACH;
        }
        outbound->next_hop_id = downlink->next_hop_id;
        if (hop_count_out != NULL) {
            *hop_count_out = downlink->hop_count;
        }
    } else {
        return mesh_errno_from_proto(ret);
    }
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    return 0;
}

static int gateway_survey_send_pair_control(struct mesh_outbound *outbound,
                                            uint64_t absolute_deadline_ms,
                                            uint32_t client_token,
                                            uint32_t *delivery_handle_out)
{
    int ret;

    if (absolute_deadline_ms == 0u || delivery_handle_out == NULL) {
        return -EINVAL;
    }
    ret = gateway_survey_prepare_pair_control(outbound, NULL);
    if (ret < 0) {
        return ret;
    }

    return app_node_comm_submit_delivery(
        outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        absolute_deadline_ms,
        client_token,
        delivery_handle_out);
}

#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_survey_next_transaction_token(void)
{
    gateway_survey_transaction_client_token++;
    if (gateway_survey_transaction_client_token == 0u) {
        gateway_survey_transaction_client_token = 1u;
    }
    return gateway_survey_transaction_client_token;
}

static uint32_t gateway_survey_natural_control_timeout_ms(uint64_t target_id)
{
    const struct mesh_downlink_entry *downlink;
    struct survey_gateway_reverse_hint reverse_hint;

    if (survey_gateway_reverse_hint_for_target(&gateway_survey_context,
                                               target_id,
                                               &reverse_hint) != PROTO_OK) {
        downlink = mesh_relay_find_current_downlink(&mesh_runtime, target_id);
        return downlink == NULL ?
               SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS :
               survey_pair_control_timeout_ms(downlink->hop_count);
    }
    return survey_pair_control_timeout_ms(reverse_hint.hop_count);
}

static uint32_t gateway_survey_control_timeout_ms(uint64_t target_id)
{
    uint32_t natural_timeout_ms =
        gateway_survey_natural_control_timeout_ms(target_id);
    uint32_t remaining_ms = uptime_ms_until_deadline(
        k_uptime_get_32(), gateway_survey_operation_deadline_ms);
    /*
     * The stored default or host-selected budget is one absolute operation
     * deadline. A control result that arrives before it must not become late
     * merely because future phases exist.
     */
    return gateway_command_budget_window_ms(
        true,
        remaining_ms,
        1u,
        natural_timeout_ms);
}

static int gateway_survey_outbound_digest(
    const struct mesh_outbound *outbound,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (outbound == NULL || digest == NULL ||
        outbound->packet.payload_len != outbound->payload_len) {
        return -EINVAL;
    }
    return node_transaction_digest_packet(&outbound->packet,
                                          outbound->payload,
                                          outbound->payload_len,
                                          digest) ? 0 : -EINVAL;
}

static bool gateway_survey_pair_matches_transaction(
    const struct survey_pair *pair)
{
    return pair != NULL && gateway_survey_transaction.pair_loaded &&
           gateway_survey_transaction.pair.operation_generation ==
               pair->operation_generation &&
           gateway_survey_transaction.pair.survey_id == pair->survey_id &&
           gateway_survey_transaction.pair.initiator_id == pair->initiator_id &&
           gateway_survey_transaction.pair.responder_id == pair->responder_id &&
           gateway_survey_transaction.pair.sample_count == pair->sample_count;
}

static bool gateway_manual_survey_cleanup_matches_transaction(void)
{
    return gateway_manual_survey_pair_state.valid &&
           gateway_manual_survey_pair_state.cleanup_requested &&
           gateway_survey_pair_matches_transaction(
               &gateway_manual_survey_pair_state.pair);
}

/*
 * A submitted PREPARE may have changed remote state even when its command
 * result was lost.  Keep the manual-operation owner until the same bounded
 * cleanup machinery used by automatic surveys has attempted ABORT for every
 * endpoint whose PREPARE could have reached the radio.
 */
static int gateway_manual_survey_pair_begin_cleanup(void)
{
    uint8_t possible_prepare_mask;
    int ret;

    if (!gateway_manual_survey_pair_state.valid) {
        return 0;
    }
    if (gateway_manual_survey_pair_state.cleanup_requested) {
        return gateway_survey_work_reschedule(0u);
    }
    possible_prepare_mask =
        gateway_manual_survey_pair_state.prepare_possible_mask;
    if (possible_prepare_mask == 0u) {
        gateway_manual_survey_pair_reset();
        return 0;
    }
    if (gateway_survey_active || gateway_survey_cleanup_pending()) {
        return -EBUSY;
    }
    if (gateway_manual_survey_pair_state.go_delivery_handle != 0u) {
        uint8_t attempts_started = 0u;

        if (app_node_comm_delivery_attempts_started(
                gateway_manual_survey_pair_state.go_delivery_handle,
                &attempts_started) == 0 &&
            attempts_started != 0u) {
            gateway_manual_survey_pair_state.go_rf_started = true;
        }
        ret = app_node_comm_abandon_delivery(
            gateway_manual_survey_pair_state.go_delivery_handle);
        if (ret < 0) {
            return ret;
        }
        gateway_manual_survey_pair_state.go_delivery_handle = 0u;
    }

    survey_gateway_transaction_init(&gateway_survey_transaction);
    memset(&gateway_survey_cleanup, 0, sizeof(gateway_survey_cleanup));
    ret = survey_gateway_transaction_load_pair(
        &gateway_survey_transaction,
        &gateway_manual_survey_pair_state.pair);
    if (ret < 0) {
        return ret;
    }
    gateway_survey_transaction.possible_prepare_mask =
        possible_prepare_mask;
    survey_gateway_transaction_require_cleanup(
        &gateway_survey_transaction, false, (uint64_t)k_uptime_get());
    if (survey_gateway_transaction_cleanup_mask(
            &gateway_survey_transaction) != possible_prepare_mask) {
        survey_gateway_transaction_init(&gateway_survey_transaction);
        return -EIO;
    }
    gateway_manual_survey_pair_state.cleanup_requested = true;
    return gateway_survey_work_reschedule(0u);
}

static int gateway_survey_transaction_load_action_pair(
    const struct survey_pair *pair)
{
    if (gateway_survey_pair_matches_transaction(pair)) {
        return 0;
    }
    if (gateway_survey_transaction.pair_loaded) {
        return -ESTALE;
    }
    return survey_gateway_transaction_load_pair(&gateway_survey_transaction,
                                                pair);
}

static bool gateway_survey_sequential_run_matches(
    const struct survey_gateway_auto_action *action)
{
    return action != NULL && gateway_survey_sequential_run.valid &&
           gateway_survey_sequential_run.pair.operation_generation ==
               action->pair.operation_generation &&
           gateway_survey_sequential_run.pair.survey_id ==
               action->pair.survey_id &&
           gateway_survey_sequential_run.pair.initiator_id ==
               action->pair.initiator_id &&
           gateway_survey_sequential_run.pair.responder_id ==
               action->pair.responder_id &&
           gateway_survey_sequential_run.pair.sample_count ==
               action->pair.sample_count &&
           gateway_survey_sequential_run.reruns_started ==
               gateway_survey_auto.pair_reruns_started;
}

static int gateway_survey_action_round_id(
    const struct survey_gateway_auto_action *action,
    uint16_t *round_id)
{
    if (action == NULL || round_id == NULL) {
        return -EINVAL;
    }
    if (gateway_survey_round_active()) {
        if (gateway_survey_round.runtime.batch_sequence == 0u) {
            return -ESTALE;
        }
        *round_id = gateway_survey_round.runtime.batch_sequence;
        return 0;
    }
    if (!gateway_survey_sequential_run_matches(action)) {
        uint16_t next_round_id;
        int ret;

        if (action->stage != SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR) {
            return -ESTALE;
        }
        ret = survey_pair_result_next_round_id(
            gateway_survey_sequential_run.generation_cursor,
            &next_round_id);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        gateway_survey_sequential_run = (struct gateway_survey_sequential_run) {
            .pair = action->pair,
            .round_id = next_round_id,
            .generation_cursor = next_round_id,
            .reruns_started = gateway_survey_auto.pair_reruns_started,
            .valid = true,
        };
    }
    *round_id = gateway_survey_sequential_run.round_id;
    return 0;
}
#endif

#if DEVICE_ROLE != ROLE_GATEWAY
static uint32_t gateway_survey_natural_control_timeout_ms(uint64_t target_id)
{
    (void)target_id;
    return SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
}
#endif

static int gateway_survey_auto_send_outbound(struct mesh_outbound *outbound,
                                             enum command_id command_id,
                                             const char *reason)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct node_transaction_key key;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t now_ms;
    uint64_t absolute_deadline_ms;
    uint32_t control_timeout_ms;
    uint32_t client_token;
    uint32_t delivery_handle;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = gateway_survey_outbound_digest(outbound, request_digest);
    if (ret < 0) {
        return ret;
    }
    now_ms = (uint64_t)k_uptime_get();
    control_timeout_ms = gateway_survey_control_timeout_ms(
        outbound->packet.dst_id);
    if (control_timeout_ms == 0u) {
        return -ETIMEDOUT;
    }
    absolute_deadline_ms = now_ms + control_timeout_ms;
    client_token = gateway_survey_next_transaction_token();
    ret = gateway_survey_send_pair_control(
        outbound,
        absolute_deadline_ms,
        client_token,
        &delivery_handle);
    if (ret < 0) {
        return ret;
    }

    key = (struct node_transaction_key) {
        .requester_id = outbound->packet.src_id,
        .responder_id = outbound->packet.dst_id,
        .session_id = outbound->packet.session_id,
        .transaction_id = outbound->packet.seq,
        .operation_id = (uint16_t)command_id,
    };
    ret = survey_gateway_transaction_begin(&gateway_survey_transaction,
                                           &key,
                                           command_id,
                                           request_digest,
                                           client_token,
                                           delivery_handle,
                                           absolute_deadline_ms,
                                           now_ms);
    if (ret < 0) {
        (void)app_node_comm_abandon_delivery(delivery_handle);
        return ret;
    }
    ret = gateway_begin_command_result_wait_until(
        &outbound->packet,
        command_id,
        (uint32_t)absolute_deadline_ms);
    if (ret < 0) {
        enum node_transaction_action action;

        (void)app_node_comm_abandon_delivery(delivery_handle);
        (void)node_transaction_cancel(&gateway_survey_transaction.active,
                                      now_ms, &action);
        return ret;
    }
    ret = survey_gateway_auto_mark_waiting(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        enum node_transaction_action action;

        (void)app_node_comm_abandon_delivery(delivery_handle);
        (void)node_transaction_cancel(&gateway_survey_transaction.active,
                                      now_ms, &action);
        gateway_clear_pending_command_result(&outbound->packet);
        return mesh_errno_from_proto(ret);
    }

    gateway_survey_pending_command = outbound->packet;
    gateway_survey_pending_command_valid = true;
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
    app_stack_workload_diag_gateway_control_admit(&outbound->packet, 1u, 1u);
    GATEWAY_SURVEY_VERBOSE_LOG("gateway survey transaction submitted: cmd=0x%04x dst=0x%016llx seq=%u handle=%u deadline=%llu reason=%s",
            (unsigned int)command_id,
            (unsigned long long)outbound->packet.dst_id,
            outbound->packet.seq,
            delivery_handle,
            (unsigned long long)absolute_deadline_ms,
            reason == NULL ? "survey-control" : reason);
    (void)gateway_survey_work_reschedule(
        GATEWAY_SURVEY_TRANSACTION_POLL_MS);
    return 0;
#else
    ARG_UNUSED(outbound);
    ARG_UNUSED(command_id);
    ARG_UNUSED(reason);
    return -ENOTSUP;
#endif
}

static int gateway_survey_auto_send_prepare(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
#if DEVICE_ROLE == ROLE_GATEWAY
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t round_id;
#endif
    int ret;

#if DEVICE_ROLE == ROLE_GATEWAY
    ret = gateway_survey_action_round_id(action, &round_id);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_survey_round_commitment(round_commitment);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
#endif
    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret == PROTO_OK) {
        const struct operation_policy policy = {
            .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
            .value.pair = {
                .max_reruns = gateway_survey_max_pair_reruns,
                .max_parallel_pairs = gateway_survey_max_parallel_pairs,
            },
        };

        ret = operation_policy_append_tlv(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          &policy);
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            round_commitment);
    }
#endif
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    ret = survey_init_pair_prepare_packet(&outbound.packet,
                                          &action->pair,
                                          DEVICE_ID,
                                          action->target_id,
                                          seq,
                                          (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;

    GATEWAY_SURVEY_VERBOSE_LOG("gateway survey auto prepare: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_PREPARE_PAIR,
                                             "survey-auto-prepare");
}

static int gateway_survey_auto_send_start(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
#if DEVICE_ROLE == ROLE_GATEWAY
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t round_id;
#endif
    int ret;

#if DEVICE_ROLE == ROLE_GATEWAY
    ret = gateway_survey_action_round_id(action, &round_id);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_survey_round_commitment(round_commitment);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
#endif
    ret = mesh_append_command_id(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 CMD_SURVEY_START_PAIR);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret == PROTO_OK) {
        const struct operation_policy policy = {
            .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
            .value.pair = {
                .max_reruns = gateway_survey_max_pair_reruns,
                .max_parallel_pairs = gateway_survey_max_parallel_pairs,
            },
        };

        ret = operation_policy_append_tlv(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          &policy);
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_append_tlv(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            round_commitment);
    }
#endif
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    outbound.packet.msg_type = MSG_COMMAND;
    outbound.packet.src_id = DEVICE_ID;
    outbound.packet.dst_id = action->target_id;
    outbound.packet.session_id =
        action->pair.operation_generation == 0u ?
            action->pair.survey_id :
            survey_operation_session_id(
                action->pair.operation_generation);
    outbound.packet.seq = seq;
    outbound.packet.ttl = MESH_DEFAULT_TTL;
    outbound.packet.payload_len = (uint8_t)payload_len;
    outbound.payload_len = (uint8_t)payload_len;

    GATEWAY_SURVEY_VERBOSE_LOG("gateway survey auto start: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_START_PAIR,
                                             "survey-auto-start");
}

static int gateway_survey_auto_send_action(const struct survey_gateway_auto_action *action)
{
    int ret;

    if (action == NULL) {
        return -EINVAL;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    ret = gateway_survey_transaction_load_action_pair(&action->pair);
    if (ret < 0) {
        return ret;
    }
#else
    ARG_UNUSED(ret);
#endif
    switch (action->command_id) {
    case CMD_SURVEY_PREPARE_PAIR:
        return gateway_survey_auto_send_prepare(action);
    case CMD_SURVEY_START_PAIR:
        return gateway_survey_auto_send_start(action);
    default:
        return -EINVAL;
    }
}

static void gateway_survey_auto_log_skipped_pair(const char *reason,
                                                 enum command_status status,
                                                 uint8_t detail,
                                                 enum gateway_command_event_reason event_reason,
                                                 enum command_id failed_command_id,
                                                 uint64_t failed_target_id)
{
    struct gateway_command_event event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        failed_command_id,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);

    gateway_survey_pair_failure_count++;
    event.status = status;
    event.reason = event_reason;
    event.anchor_id = failed_target_id;
    event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
    event.pair_responder_id = gateway_survey_auto.pair.responder_id;
    event.progress_count =
        (uint16_t)gateway_survey_context.next_pair_index;
    event.total_count = (uint16_t)gateway_survey_context.pair_count;
#if DEVICE_ROLE == ROLE_GATEWAY
    event.attempt = gateway_survey_transaction.active.request_attempts_started;
#endif
    event.success_count = gateway_survey_pair_success_count;
    event.failure_count = gateway_survey_pair_failure_count;
    event.duplicate_count = gateway_survey_duplicate_count;
    gateway_survey_terminal_failure_reason =
        gateway_command_survey_failure_reason_merge(
            gateway_survey_terminal_failure_reason,
            event.reason);
    (void)app_gateway_survey_observability_submit_boundary(
        &gateway_survey_observability, &gateway_survey_observability_ops,
        &event);
    LOG_WRN("gateway survey auto pair skipped: survey=%u initiator=0x%016llx responder=0x%016llx reason=%s status=%u detail=%u",
            gateway_survey_auto.pair.survey_id,
            (unsigned long long)gateway_survey_auto.pair.initiator_id,
            (unsigned long long)gateway_survey_auto.pair.responder_id,
            reason,
            status,
            detail);
    (void)gateway_survey_work_reschedule(GATEWAY_SURVEY_AUTO_RETRY_MS);
}

#if DEVICE_ROLE == ROLE_GATEWAY
static uint64_t gateway_survey_cleanup_target(uint8_t peer_mask)
{
    if (peer_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK) {
        return gateway_survey_transaction.pair.initiator_id;
    }
    if (peer_mask == SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK) {
        return gateway_survey_transaction.pair.responder_id;
    }
    return 0u;
}

static uint16_t gateway_survey_cleanup_round_id(void)
{
    if (gateway_manual_survey_cleanup_matches_transaction()) {
        return gateway_manual_survey_pair_state.round_id;
    }
    if (gateway_survey_round_active() &&
        gateway_survey_round.runtime.batch_sequence !=
            SURVEY_LEGACY_ROUND_ID) {
        return gateway_survey_round.runtime.batch_sequence;
    }
    if (gateway_survey_sequential_run.valid &&
        gateway_survey_pair_matches_transaction(
            &gateway_survey_sequential_run.pair)) {
        return gateway_survey_sequential_run.round_id;
    }
    return SURVEY_LEGACY_ROUND_ID;
}

static int gateway_survey_prepare_cleanup_delivery(
    struct gateway_survey_cleanup_delivery *cleanup,
    uint8_t peer_mask,
    uint64_t now_ms)
{
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t target_id = gateway_survey_cleanup_target(peer_mask);
    uint64_t cleanup_deadline_ms =
        survey_gateway_transaction_cleanup_deadline(
            &gateway_survey_transaction);
    uint16_t round_id = gateway_survey_cleanup_round_id();
    struct mesh_outbound route = {
        .packet = {
            .dst_id = target_id,
        },
    };
    uint8_t hop_count = 0u;
    int ret;

    if (cleanup == NULL || target_id == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        cleanup_deadline_ms == 0u) {
        return -EINVAL;
    }
    if (gateway_manual_survey_cleanup_matches_transaction()) {
        memcpy(round_commitment,
               gateway_manual_survey_pair_state.round_commitment,
               sizeof(round_commitment));
    } else {
        ret = gateway_survey_round_commitment(round_commitment);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
    }
    if (!cleanup->active) {
        *cleanup = (struct gateway_survey_cleanup_delivery) {
            .pair = gateway_survey_transaction.pair,
            .target_id = target_id,
            .absolute_deadline_ms = cleanup_deadline_ms,
            .client_token = gateway_survey_next_transaction_token(),
            .sequence = gateway_next_command_seq(),
            .round_id = round_id,
            .peer_mask = peer_mask,
            .active = true,
        };
        memcpy(cleanup->round_commitment,
               round_commitment,
               sizeof(cleanup->round_commitment));
    } else if (cleanup->peer_mask != peer_mask ||
               cleanup->target_id != target_id ||
               cleanup->pair.operation_generation !=
                   gateway_survey_transaction.pair.operation_generation ||
               cleanup->pair.survey_id !=
                   gateway_survey_transaction.pair.survey_id ||
               cleanup->pair.initiator_id !=
                   gateway_survey_transaction.pair.initiator_id ||
               cleanup->pair.responder_id !=
                   gateway_survey_transaction.pair.responder_id ||
               cleanup->pair.sample_count !=
                   gateway_survey_transaction.pair.sample_count ||
               cleanup->round_id != round_id ||
               !semantic_digest_equal(cleanup->round_commitment,
                                      round_commitment,
                                      sizeof(round_commitment))) {
        return -ESTALE;
    }
    if (now_ms >= cleanup->absolute_deadline_ms) {
        return -ETIMEDOUT;
    }
    ret = gateway_survey_prepare_pair_control(&route, &hop_count);
    if (ret < 0) {
        return ret;
    }
    cleanup->next_hop_id = route.next_hop_id;
    cleanup->hop_count = hop_count;
    cleanup->prepared = true;
    return 0;
}

static int gateway_survey_build_cleanup_outbound(
    const struct gateway_survey_cleanup_delivery *cleanup,
    struct mesh_outbound *outbound)
{
    size_t payload_len = 0u;
    int ret;

    if (cleanup == NULL || outbound == NULL || !cleanup->prepared) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = mesh_append_command_id(outbound->payload,
                                 sizeof(outbound->payload),
                                 &payload_len,
                                 CMD_SURVEY_ABORT);
    if (ret == PROTO_OK) {
        ret = survey_append_pair_tlvs(outbound->payload,
                                      sizeof(outbound->payload),
                                      &payload_len,
                                      &cleanup->pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(outbound->payload,
                                         sizeof(outbound->payload),
                                         &payload_len,
                                         cleanup->round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_append_tlv(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            cleanup->round_commitment);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound->packet.msg_type = MSG_COMMAND;
    outbound->packet.src_id = DEVICE_ID;
    outbound->packet.dst_id = cleanup->target_id;
    outbound->packet.session_id = survey_operation_session_id(
        cleanup->pair.operation_generation);
    outbound->packet.seq = cleanup->sequence;
    outbound->packet.ttl = MESH_DEFAULT_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint8_t)payload_len;
    outbound->next_hop_id = cleanup->next_hop_id;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return 0;
}

static bool gateway_survey_cleanup_slots_active(void)
{
    return gateway_survey_cleanup.active;
}

static bool gateway_survey_cleanup_pending(void)
{
    return survey_gateway_transaction_cleanup_pending(
               &gateway_survey_transaction) ||
           gateway_survey_cleanup_slots_active();
}

static struct survey_gateway_drive_state gateway_survey_drive_state(
    uint32_t now_ms)
{
    bool response_ack_settle_pending =
        survey_gateway_response_ack_settle_pending(
            &gateway_survey_response_ack_settle,
            now_ms);

    return (struct survey_gateway_drive_state) {
        .survey_active = gateway_survey_active,
        .auto_running = gateway_survey_auto.running,
        .auto_waiting = gateway_survey_auto.waiting,
        .pair_observation_active =
            gateway_survey_pair_observation_active,
        .round_drive_ready =
            gateway_survey_round.phase ==
                APP_GATEWAY_SURVEY_ROUND_DISPATCHING ||
            gateway_survey_round.phase ==
                APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED ||
            gateway_survey_round.phase ==
                APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE,
        .round_go_delivery_pending =
            gateway_survey_round.phase ==
                APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED &&
            gateway_survey_round_go_delivery_handle != 0u,
        .cleanup_pending = gateway_survey_cleanup_pending(),
        .boundary_pending = gateway_survey_observability.boundary_pending,
        .response_ack_settle_pending = response_ack_settle_pending,
    };
}

static bool gateway_survey_response_ack_settle_blocks_progress(
    uint32_t now_ms)
{
    if (!gateway_survey_response_ack_settle.active) {
        return false;
    }
    if (!survey_gateway_response_ack_settle_deadline_reached(
            &gateway_survey_response_ack_settle, now_ms)) {
        (void)gateway_survey_work_reschedule(
            gateway_survey_response_ack_settle.deadline_ms - now_ms);
        return true;
    }
    {
        enum gateway_command_result_validation_check validation_check =
            gateway_protocol_validation_check_interval(
                gateway_survey_response_ack_settle.started_at_ms,
                gateway_survey_response_ack_settle.deadline_ms);

        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            /*
             * A duplicate physically received in [start, deadline) owns the
             * boundary until semantic validation commits its ACK re-arm.
             */
            (void)gateway_survey_work_reschedule(
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return true;
        }
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            gateway_survey_auto_finish_status(
                COMMAND_INTERNAL_ERROR,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            return true;
        }
        survey_gateway_response_ack_settle_complete(
            &gateway_survey_response_ack_settle);
    }
    return false;
}

static void gateway_survey_schedule_drive(void)
{
    uint32_t now_ms = k_uptime_get_32();
    struct survey_gateway_drive_state state;
    enum survey_gateway_drive_action action;

    if (gateway_survey_response_ack_settle_blocks_progress(now_ms)) {
        return;
    }
    state = gateway_survey_drive_state(now_ms);
    action = survey_gateway_drive_action(&state);

    if (action == SURVEY_GATEWAY_DRIVE_POLL_CLEANUP) {
        (void)gateway_survey_work_reschedule(
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
    } else if (action == SURVEY_GATEWAY_DRIVE_POLL_WAIT) {
        (void)gateway_survey_work_reschedule(
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
    } else if (action == SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY) {
        (void)gateway_survey_work_reschedule(GATEWAY_BLE_TX_RETRY_MS);
    } else if (action == SURVEY_GATEWAY_DRIVE_RUN_NOW) {
        status_debug_printf("DBG_SURVEY_DRIVE_RESUME survey=%u stage=%u\n",
                            gateway_survey_context.survey_id,
                            (unsigned int)gateway_survey_auto.stage);
        (void)gateway_survey_work_reschedule(0u);
    }
}

/*
 * Reuse the single bounded cleanup transaction for one retained round lane at
 * a time. The round stays in TERMINATING, so every ABORT uses the original
 * round ID and commitment even after the public survey operation is inactive.
 */
static int gateway_survey_start_next_round_termination_cleanup(
    uint64_t now_ms)
{
    struct survey_pair pair;
    uint8_t cleanup_mask;
    size_t lane_index;
    int ret;

    if (!app_gateway_survey_round_terminating(&gateway_survey_round)) {
        return 0;
    }
    ret = app_gateway_survey_round_next_termination_cleanup(
        &gateway_survey_round,
        &lane_index,
        &pair,
        &cleanup_mask);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    survey_gateway_transaction_init(&gateway_survey_transaction);
    ret = survey_gateway_transaction_load_pair(&gateway_survey_transaction,
                                               &pair);
    if (ret < 0) {
        return ret;
    }
    gateway_survey_transaction.possible_prepare_mask = cleanup_mask;
    survey_gateway_transaction_require_cleanup(&gateway_survey_transaction,
                                               false,
                                               now_ms);
    if (survey_gateway_transaction_cleanup_mask(
            &gateway_survey_transaction) != cleanup_mask) {
        return -EIO;
    }
    gateway_survey_round_cleanup_lane_index = lane_index;
    gateway_survey_round_cleanup_lane_valid = true;
    gateway_survey_begin_cleanup();
    return 1;
}

static void gateway_survey_finish_cleanup_if_complete(uint64_t now_ms)
{
    bool manual_cleanup =
        gateway_manual_survey_cleanup_matches_transaction();
    int ret;

    if (survey_gateway_transaction_cleanup_mask(
            &gateway_survey_transaction) != 0u ||
        gateway_survey_cleanup_slots_active()) {
        return;
    }
    if (gateway_survey_transaction.abandoning) {
        ret = survey_gateway_transaction_note_cleanup_complete(
            &gateway_survey_transaction, 0u, now_ms);
        if (ret == -EINPROGRESS) {
            (void)gateway_survey_work_reschedule(
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return;
        }
        if (ret < 0) {
            LOG_ERR("gateway survey cleanup retirement rejected: ret=%d",
                    ret);
            return;
        }
    }
    survey_gateway_transaction_pair_complete(&gateway_survey_transaction,
                                             true, now_ms);
    if (manual_cleanup) {
        gateway_manual_survey_pair_reset();
        return;
    }
    if (app_gateway_survey_round_terminating(&gateway_survey_round)) {
        ret = gateway_survey_start_next_round_termination_cleanup(now_ms);
        if (ret > 0) {
            return;
        }
        if (ret < 0) {
            LOG_ERR("gateway survey next round cleanup rejected: ret=%d",
                    ret);
            app_watchdog_stop_feeding();
            return;
        }
    }
    if (!gateway_survey_active) {
        gateway_survey_round_reset();
        memset(&gateway_survey_sequential_run, 0,
               sizeof(gateway_survey_sequential_run));
        if (gateway_survey_finish_pending) {
            gateway_observe_survey_terminal(
                gateway_survey_finish_pending_status,
                gateway_survey_finish_pending_reason);
            gateway_survey_finish_pending = false;
        }
        gateway_operation_owner_release(
            GATEWAY_OPERATION_OWNER_AUTO_SURVEY);
        (void)k_work_cancel_delayable(&gateway_survey_work);
    }
}

static void gateway_survey_begin_cleanup(void)
{
    uint8_t cleanup_mask = survey_gateway_transaction_cleanup_mask(
        &gateway_survey_transaction);
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint8_t peer_mask;

    if (cleanup_mask == 0u) {
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (gateway_survey_cleanup.active) {
        if (gateway_survey_cleanup.prepared ||
            gateway_survey_cleanup.completion_ready) {
            return;
        }
        peer_mask = gateway_survey_cleanup.peer_mask;
    } else {
        peer_mask = (cleanup_mask &
                     SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK) != 0u ?
                    SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK :
                    SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    }
    {
        int ret = gateway_survey_prepare_cleanup_delivery(
            &gateway_survey_cleanup, peer_mask, now_ms);

        if (ret == -ETIMEDOUT) {
            gateway_survey_cleanup.peer_unavailable = true;
            gateway_survey_cleanup.completion_ready = true;
            status_debug_printf(
                "DBG_SURVEY_CLEANUP_PEER_UNAVAILABLE dst=0x%016llx peer_mask=0x%02x deadline=%llu reason=route\n",
                (unsigned long long)gateway_survey_cleanup.target_id,
                gateway_survey_cleanup.peer_mask,
                (unsigned long long)
                    gateway_survey_cleanup.absolute_deadline_ms);
        } else if (ret < 0) {
            LOG_WRN("gateway survey cleanup route unavailable; retrying within frozen deadline: peer_mask=0x%02x ret=%d deadline=%llu",
                    peer_mask,
                    ret,
                    (unsigned long long)
                        gateway_survey_cleanup.absolute_deadline_ms);
        }
    }
    (void)gateway_survey_work_reschedule(
        GATEWAY_SURVEY_TRANSACTION_POLL_MS);
}

static void gateway_survey_service_cleanup(void)
{
    struct gateway_survey_cleanup_delivery *cleanup =
        &gateway_survey_cleanup;
    struct mesh_outbound outbound;
    struct node_comm_terminal_event event;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int ret;

    if (!cleanup->active) {
        gateway_survey_begin_cleanup();
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (cleanup->completion_ready) {
        if (gateway_survey_round_cleanup_lane_valid) {
            const struct survey_pair_round_lane *lane =
                app_gateway_survey_round_lane(
                    &gateway_survey_round,
                    gateway_survey_round_cleanup_lane_index);

            if (lane == NULL ||
                lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP ||
                (cleanup->peer_mask &
                 (uint8_t)~lane->cleanup_mask) != 0u) {
                LOG_ERR("gateway survey cleanup lane custody mismatch: lane=%zu peer_mask=0x%02x",
                        gateway_survey_round_cleanup_lane_index,
                        cleanup->peer_mask);
                app_watchdog_stop_feeding();
                return;
            }
        }
        if (cleanup->peer_unavailable) {
            /*
             * The anchor's prepared lease can outlive this cleanup attempt by
             * several minutes. Treating an unconfirmed ABORT as completion
             * could load a new round while the old commitment still owns
             * START_PENDING. Freeze the cleanup debt, publish a terminal
             * failure through the normal survey owner, and force bounded
             * watchdog recovery instead of advancing the lane.
             */
            LOG_ERR("gateway survey cleanup failed without delivery: dst=0x%016llx peer_mask=0x%02x",
                    (unsigned long long)cleanup->target_id,
                    cleanup->peer_mask);
            status_debug_printf(
                "DBG_SURVEY_CLEANUP_FAILED dst=0x%016llx peer_mask=0x%02x deadline=%llu\n",
                (unsigned long long)cleanup->target_id,
                cleanup->peer_mask,
                (unsigned long long)cleanup->absolute_deadline_ms);
            if (gateway_survey_active) {
                ret = gateway_survey_auto_finish_status(
                    COMMAND_RADIO_ERROR,
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
                if (ret < 0) {
                    LOG_ERR("gateway survey cleanup failure retirement rejected: ret=%d",
                            ret);
                }
            } else if (gateway_survey_finish_pending) {
                gateway_survey_finish_pending_status =
                    COMMAND_RADIO_ERROR;
                gateway_survey_finish_pending_reason =
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED;
            }
            app_watchdog_stop_feeding();
            return;
        }
        ret = survey_gateway_transaction_note_cleanup_complete(
            &gateway_survey_transaction, cleanup->peer_mask, now_ms);
        if (ret == -EINPROGRESS) {
            (void)gateway_survey_work_reschedule(
                GATEWAY_SURVEY_TRANSACTION_POLL_MS);
            return;
        }
        if (ret < 0) {
            LOG_ERR("gateway survey cleanup completion rejected: peer_mask=0x%02x ret=%d",
                    cleanup->peer_mask,
                    ret);
            return;
        }
        gateway_survey_round_note_cleanup_peer(cleanup->peer_mask);
        memset(cleanup, 0, sizeof(*cleanup));
        gateway_survey_begin_cleanup();
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (!cleanup->prepared) {
        gateway_survey_begin_cleanup();
        return;
    }
    if (cleanup->submitted) {
        if (!app_node_comm_take_delivery_event_for(cleanup->handle, &event)) {
            if (now_ms < cleanup->absolute_deadline_ms) {
                return;
            }
            ret = app_node_comm_abandon_delivery(cleanup->handle);
            status_debug_printf(
                "DBG_SURVEY_CLEANUP_DEADLINE dst=0x%016llx handle=%u ret=%d\n",
                (unsigned long long)cleanup->target_id,
                cleanup->handle,
                ret);
            if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
                /*
                 * The transport may still own this exact ABORT.  Keep the
                 * submitted slot and handle intact so a live delivery cannot
                 * outlast the cleanup transaction that identifies it.  There
                 * is no safe post-deadline retry owner, so fail closed.
                 */
                LOG_ERR("gateway survey cleanup abandonment failed: dst=0x%016llx handle=%u ret=%d",
                        (unsigned long long)cleanup->target_id,
                        cleanup->handle,
                        ret);
                app_watchdog_stop_feeding();
                return;
            }
            cleanup->peer_unavailable = true;
            cleanup->completion_ready = true;
            (void)gateway_survey_work_reschedule(0u);
            return;
        }
        status_debug_printf("DBG_SURVEY_CLEANUP_TERMINAL dst=0x%016llx handle=%u reason=%u attempts=%u\n",
                            (unsigned long long)cleanup->target_id,
                            cleanup->handle,
                            (unsigned int)event.reason,
                            event.attempts_started);
        cleanup->peer_unavailable =
            event.reason != NODE_COMM_TERMINAL_DELIVERED ||
            event.attempts_started == 0u;
        cleanup->completion_ready = true;
        (void)gateway_survey_work_reschedule(0u);
        return;
    }
    if (now_ms >= cleanup->absolute_deadline_ms) {
        LOG_ERR("gateway survey cleanup submission expired: dst=0x%016llx",
                (unsigned long long)cleanup->target_id);
        cleanup->peer_unavailable = true;
        cleanup->completion_ready = true;
        (void)gateway_survey_work_reschedule(0u);
        return;
    }
    ret = gateway_survey_build_cleanup_outbound(cleanup, &outbound);
    if (ret < 0) {
        return;
    }
    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        cleanup->absolute_deadline_ms,
        cleanup->client_token,
        &cleanup->handle);
    if (ret < 0) {
        return;
    }
    cleanup->submitted = true;
    (void)survey_gateway_transaction_note_cleanup_started(
        &gateway_survey_transaction, cleanup->peer_mask);
    status_debug_printf("DBG_SURVEY_CLEANUP_SUBMIT dst=0x%016llx seq=%u handle=%u deadline=%llu\n",
                        (unsigned long long)cleanup->target_id,
                        cleanup->sequence,
                        cleanup->handle,
                        (unsigned long long)cleanup->absolute_deadline_ms);
}

static int gateway_survey_cancel_take_active_delivery(
    enum node_transaction_action *action)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    struct node_comm_terminal_event event;
    uint32_t handle;
    int ret;

    if (action == NULL || transaction->state == NODE_TRANSACTION_EMPTY) {
        return -EINVAL;
    }
    if (transaction->request_delivery_terminal) {
        *action = transaction->state == NODE_TRANSACTION_SUCCEEDED ?
                      NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS :
                  transaction->state == NODE_TRANSACTION_ABANDONING ?
                      NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED :
                  transaction->state == NODE_TRANSACTION_ABANDONED ?
                      NODE_TRANSACTION_ACTION_TERMINAL_ABANDON :
                      NODE_TRANSACTION_ACTION_WAIT_RESULT;
        return 0;
    }

    handle = transaction->request_delivery_handle;
    ret = app_node_comm_cancel_delivery(handle);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
    if (!app_node_comm_take_delivery_event_for(handle, &event)) {
        return -EAGAIN;
    }
    ret = survey_gateway_transaction_note_delivery_terminal(
        &gateway_survey_transaction, &event, (uint64_t)k_uptime_get(),
        action);
    if (ret < 0) {
        return ret;
    }
    status_debug_printf("DBG_SURVEY_CONTROL_CANCEL_TAKE handle=%u attempts=%u state=%u\n",
                        event.handle,
                        event.attempts_started,
                        (unsigned int)transaction->state);
    return 0;
}

static void gateway_survey_abandon_current(
    enum command_status status,
    uint8_t reason,
    enum gateway_command_event_reason event_reason,
    const char *log_reason)
{
    bool pair_launched = false;
    bool pair_skipped = false;
    enum command_id command_id =
        gateway_survey_transaction.active_command_id;
    enum node_transaction_action action;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int ret;

    if (gateway_survey_transaction.active.state != NODE_TRANSACTION_EMPTY &&
        !gateway_survey_transaction.active.request_delivery_terminal) {
        ret = gateway_survey_cancel_take_active_delivery(&action);
        if (ret < 0) {
            LOG_ERR("gateway survey active delivery cancel/take failed: ret=%d",
                    ret);
        }
    }
    survey_gateway_transaction_require_cleanup(&gateway_survey_transaction,
                                               false, now_ms);
    if (gateway_survey_pending_command_valid) {
        app_stack_workload_diag_gateway_control_release(
            &gateway_survey_pending_command, -ECANCELED, 0u, 0u);
        gateway_clear_pending_command_result(&gateway_survey_pending_command);
        gateway_survey_pending_command_valid = false;
    }
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
    if (gateway_survey_round_active()) {
        gateway_survey_round_fail_current_control(command_id,
                                                  gateway_survey_transaction.active_target_id,
                                                  event_reason);
        gateway_survey_begin_cleanup();
        return;
    }
    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          gateway_survey_transaction.active_target_id,
                                          survey_operation_session_id(
                                              gateway_survey_transaction.pair
                                                  .operation_generation),
                                          status,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_OK && pair_skipped) {
        gateway_survey_auto_log_skipped_pair(log_reason,
                                             status,
                                             reason,
                                             event_reason,
                                             command_id,
                                             gateway_survey_transaction.active_target_id);
    }
    gateway_survey_begin_cleanup();
}
#endif

enum gateway_survey_result_preflight_outcome
gateway_survey_auto_preflight_result(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t received_at_ms)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct survey_gateway_transaction transaction_candidate;
    struct node_transaction_key key;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action action;
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    uint32_t result_token;
    uint8_t reason = 0u;
    int ret;

    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
    if (!gateway_survey_active || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        gateway_command_extract_id(payload, payload_len, &command_id) !=
            PROTO_OK ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    }
    key = (struct node_transaction_key) {
        .requester_id = packet->dst_id,
        .responder_id = packet->src_id,
        .session_id = packet->session_id,
        .transaction_id = packet->seq,
        .operation_id = (uint16_t)command_id,
    };
    if (!survey_gateway_transaction_request_digest(
            &gateway_survey_transaction, &key, request_digest)) {
        return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    }
    if (!node_transaction_digest_bytes(payload,
                                       payload_len,
                                       result_digest)) {
        return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    }
    result_token = packet->seq;
    ret = app_mesh_gateway_command_flow_decode_result(command_id,
                                                      payload,
                                                      payload_len,
                                                      &status,
                                                      &reason);
    if (ret != PROTO_OK) {
        status_debug_printf("DBG_SURVEY_RESULT_MALFORMED src=0x%016llx session=%u seq=%u command=0x%04x ret=%d\n",
                            (unsigned long long)packet->src_id,
                            packet->session_id,
                            packet->seq,
                            command_id,
                            ret);
        return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    }
    transaction_candidate = gateway_survey_transaction;
    ret = survey_gateway_transaction_reconcile_result(
        &transaction_candidate,
        &key,
        request_digest,
        result_digest,
        result_token,
        status,
        received_at_ms,
        &transaction_result,
        &action);
    if (ret < 0) {
        return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    }
    gateway_survey_result_preparation.key = key;
    gateway_survey_result_preparation.result = transaction_result;
    gateway_survey_result_preparation.received_at_ms = received_at_ms;
    memcpy(gateway_survey_result_preparation.result_digest,
           result_digest,
           sizeof(gateway_survey_result_preparation.result_digest));
    gateway_survey_result_preparation.result_token = result_token;
    gateway_survey_result_preparation.command_id = command_id;
    gateway_survey_result_preparation.status = status;
    gateway_survey_result_preparation.reason = reason;
    gateway_survey_result_preparation.valid = true;
    return transaction_result ==
                   SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK ||
               transaction_result ==
                   SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE ?
           GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED :
           GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    return GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
#endif
}

void gateway_survey_auto_discard_preflight_result(void)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
#endif
}

void gateway_survey_auto_commit_preflight_result(void)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_survey_result_preparation preparation;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action action;
    int ret;

    if (!gateway_survey_result_preparation.valid) {
        return;
    }
    preparation = gateway_survey_result_preparation;
    memset(&gateway_survey_result_preparation, 0,
           sizeof(gateway_survey_result_preparation));
    if (!survey_gateway_transaction_request_digest(
            &gateway_survey_transaction,
            &preparation.key,
            request_digest)) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            1u,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "result-preparation-request-missing");
        return;
    }
    ret = survey_gateway_transaction_reconcile_result(
        &gateway_survey_transaction,
        &preparation.key,
        request_digest,
        preparation.result_digest,
        preparation.result_token,
        preparation.status,
        preparation.received_at_ms,
        &transaction_result,
        &action);
    if (ret < 0 || transaction_result != preparation.result) {
        LOG_ERR("gateway survey result preparation changed before commit: ret=%d expected=%u actual=%u",
                ret,
                (unsigned int)preparation.result,
                ret < 0 ? (unsigned int)UINT32_MAX :
                          (unsigned int)transaction_result);
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            ret < 0 ? (uint8_t)(-ret) : 1u,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "result-preparation-changed");
        return;
    }

    if (preparation.result ==
            SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK ||
        preparation.result ==
            SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE) {
        if (!gateway_survey_result_preflight.valid) {
            gateway_survey_result_preflight =
                (struct gateway_survey_result_preflight) {
                .key = preparation.key,
                .result = preparation.result,
                .status = preparation.status,
                .reason = preparation.reason,
                .valid = true,
            };
        }
        return;
    }

    if (preparation.result ==
        SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT) {
        if (gateway_survey_auto.waiting) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                preparation.reason,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "conflicting-command-result");
        } else {
            gateway_survey_auto.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
            gateway_survey_auto.waiting = false;
            gateway_survey_auto_log_skipped_pair(
                "conflicting-command-result",
                COMMAND_INTERNAL_ERROR,
                preparation.reason,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                preparation.command_id,
                preparation.key.responder_id);
            gateway_survey_begin_cleanup();
        }
    } else if (preparation.result ==
               SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE) {
        gateway_survey_duplicate_count++;
        survey_gateway_response_ack_settle_note_duplicate(
            &gateway_survey_response_ack_settle,
            preparation.received_at_ms,
            gateway_survey_operation_deadline_ms);
        gateway_survey_schedule_drive();
    }
#endif
}

bool gateway_survey_auto_owns_pending_command(
    const struct proto_packet *command,
    enum command_id command_id)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    if (command == NULL || !gateway_survey_active ||
        !gateway_survey_pending_command_valid ||
        !gateway_survey_auto.waiting ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        return false;
    }
    if (command->msg_type != gateway_survey_pending_command.msg_type ||
        command->src_id != gateway_survey_pending_command.src_id ||
        command->dst_id != gateway_survey_pending_command.dst_id ||
        command->session_id != gateway_survey_pending_command.session_id ||
        command->seq != gateway_survey_pending_command.seq) {
        return false;
    }
    return survey_gateway_auto_command_matches(&gateway_survey_auto,
                                                command_id,
                                                command->dst_id,
                                                command->session_id);
#else
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return false;
#endif
}

#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_survey_complete_accepted_delivery(void)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    enum node_transaction_action action;
    int ret;

    if (transaction->state != NODE_TRANSACTION_SUCCEEDED) {
        return -EINVAL;
    }
    if (transaction->request_delivery_terminal) {
        return 0;
    }

    ret = gateway_survey_cancel_take_active_delivery(&action);
    if (ret < 0) {
        return ret;
    }
    if (action != NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS ||
        !transaction->request_delivery_terminal) {
        return -EPROTO;
    }
    return 0;
}
#endif

static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason)
{
    bool pair_launched = false;
    bool pair_skipped = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    bool matched_preflight = false;
    enum survey_gateway_transaction_result transaction_result =
        SURVEY_GATEWAY_TRANSACTION_RESULT_STALE;
#endif
    int ret;

    if (!gateway_survey_auto_owns_pending_command(command, command_id)) {
        return;
    }

#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_result_preflight.valid &&
        gateway_survey_result_preflight.key.requester_id == command->src_id &&
        gateway_survey_result_preflight.key.responder_id == command->dst_id &&
        gateway_survey_result_preflight.key.session_id == command->session_id &&
        gateway_survey_result_preflight.key.transaction_id == command->seq &&
        gateway_survey_result_preflight.key.operation_id == (uint16_t)command_id) {
        transaction_result = gateway_survey_result_preflight.result;
        status = gateway_survey_result_preflight.status;
        reason = gateway_survey_result_preflight.reason;
        matched_preflight = true;
    }
    if (transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_LATE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_STALE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT) {
        if (matched_preflight) {
            memset(&gateway_survey_result_preflight, 0,
                   sizeof(gateway_survey_result_preflight));
        }
        return;
    }
    ret = gateway_survey_complete_accepted_delivery();
    if (ret == -EAGAIN) {
        (void)gateway_survey_work_reschedule(
            GATEWAY_SURVEY_TRANSACTION_POLL_MS);
        return;
    }
    if (ret < 0) {
        if (matched_preflight) {
            memset(&gateway_survey_result_preflight, 0,
                   sizeof(gateway_survey_result_preflight));
        }
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "accepted-delivery-close-failed");
        return;
    }
    if (matched_preflight) {
        memset(&gateway_survey_result_preflight, 0,
               sizeof(gateway_survey_result_preflight));
    }
#endif

    app_stack_workload_diag_gateway_control_release(
        command, status == COMMAND_OK ? 0 : -EIO, 0u, 0u);
    gateway_survey_pending_command_valid = false;

#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_round_note_control_result(command,
                                                 command_id,
                                                 status,
                                                 reason)) {
        return;
    }
#endif

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          status,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto result transition failed: cmd=0x%04x dst=0x%016llx status=%u ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                status,
                ret);
        gateway_survey_auto_finish();
        return;
    }

    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair(
            "command-result",
            status,
            reason,
            status == COMMAND_TIMEOUT ?
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                GATEWAY_COMMAND_EVENT_REASON_RADIO,
            command_id,
            command->dst_id);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    } else if (pair_launched) {
        uint32_t observation_started_at_ms;
        uint32_t observation_duration_ms;

#if DEVICE_ROLE == ROLE_GATEWAY
        if (survey_gateway_transaction_phase_complete(
                &gateway_survey_transaction) < 0) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                1u,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "transaction-retire-failed");
            return;
        }
#endif
        observation_started_at_ms = k_uptime_get_32();
        observation_duration_ms =
            gateway_survey_pair_run_delay_ms(&gateway_survey_auto.pair);
        if (uptime_ms_until_deadline(
                observation_started_at_ms,
                gateway_survey_operation_deadline_ms) <
            observation_duration_ms) {
            observation_duration_ms = uptime_ms_until_deadline(
                observation_started_at_ms,
                gateway_survey_operation_deadline_ms);
        }
        gateway_survey_pair_observation_active = true;
        gateway_survey_pair_observation_started_at_ms =
            observation_started_at_ms;
        gateway_survey_pair_observation_deadline_ms =
            observation_started_at_ms + observation_duration_ms;
        gateway_survey_pair_result_mask = 0u;
        gateway_survey_pair_responder_usable_mask = 0u;
        gateway_survey_pair_initiator_unusable_mask = 0u;
        gateway_survey_pair_responder_unusable_mask = 0u;
        gateway_survey_pair_identity_reset();
        {
            struct gateway_command_event event = gateway_observability_event(
                GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
                GATEWAY_COMMAND_EVENT_STAGE_PAIR_START,
                CMD_SURVEY_START_PAIR,
                &gateway_survey_host_command,
                gateway_survey_context.survey_id);

            event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
            event.pair_responder_id = gateway_survey_auto.pair.responder_id;
            event.attempt =
                (uint8_t)(gateway_survey_auto.pair_reruns_started + 1u);
            event.progress_count =
                (uint16_t)gateway_survey_context.next_pair_index;
            event.total_count = (uint16_t)gateway_survey_context.pair_count;
            (void)app_gateway_survey_observability_submit_boundary(
                &gateway_survey_observability,
                &gateway_survey_observability_ops, &event);
        }
        GATEWAY_SURVEY_VERBOSE_LOG("gateway survey pair launched: survey=%u initiator=0x%016llx responder=0x%016llx samples=%u",
                gateway_survey_auto.pair.survey_id,
                (unsigned long long)gateway_survey_auto.pair.initiator_id,
                (unsigned long long)gateway_survey_auto.pair.responder_id,
                gateway_survey_auto.pair.sample_count);
        (void)gateway_survey_work_reschedule(
            observation_duration_ms);
    } else {
#if DEVICE_ROLE == ROLE_GATEWAY
        if (survey_gateway_transaction_phase_complete(
                &gateway_survey_transaction) < 0) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                1u,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "transaction-retire-failed");
            return;
        }
        survey_gateway_response_ack_settle_note_result(
            &gateway_survey_response_ack_settle,
            (uint64_t)k_uptime_get(),
            gateway_survey_operation_deadline_ms);
        gateway_survey_schedule_drive();
#endif
    }
}

static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id)
{
    bool pair_launched = false;
    bool pair_skipped = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    enum node_transaction_action action;
    int delivery_ret;
#endif
    int ret;

    if (!gateway_survey_auto_owns_pending_command(command, command_id)) {
        return;
    }

    app_stack_workload_diag_gateway_control_release(command, -ETIMEDOUT,
                                                    0u, 0u);
    gateway_survey_pending_command_valid = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    delivery_ret = gateway_survey_cancel_take_active_delivery(&action);
    if (delivery_ret < 0 && delivery_ret != -EAGAIN) {
        LOG_ERR("gateway survey timed-out delivery cancel failed: ret=%d",
                delivery_ret);
    }
    (void)survey_gateway_transaction_service(&gateway_survey_transaction,
                                             (uint64_t)k_uptime_get(),
                                             &action);
    survey_gateway_transaction_require_cleanup(&gateway_survey_transaction,
                                               false,
                                               (uint64_t)k_uptime_get());
    if (gateway_survey_round_note_control_result(command,
                                                 command_id,
                                                 COMMAND_TIMEOUT,
                                                 0u)) {
        gateway_survey_begin_cleanup();
        return;
    }
#endif

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          COMMAND_TIMEOUT,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto timeout transition failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                ret);
        gateway_survey_auto_finish();
        return;
    }
    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair(
            "command-timeout",
            COMMAND_TIMEOUT,
            0u,
            GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED,
            command_id,
            command->dst_id);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    }
}
