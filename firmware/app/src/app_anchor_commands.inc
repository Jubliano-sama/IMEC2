#line 338 __BASE_FILE__
#if DEVICE_ROLE == ROLE_ANCHOR
static struct k_work_delayable anchor_discovery_ack_liveness_work;
static uint32_t anchor_discovery_ack_liveness_generation;
static bool anchor_discovery_ack_route_wait_pending;
#endif

static struct app_mesh_discovery_assignment_snapshot
    anchor_ram_discovery_assignment_snapshot;
static bool anchor_discovery_assignment_restore_complete;
static bool anchor_discovery_assignment_ram_only_iteration;

static int anchor_restore_live_discovery_assignment_snapshot(
    const struct app_mesh_discovery_assignment_snapshot *snapshot);

#if defined(CONFIG_IMEC_DURABLE_STATE)
static void anchor_assignment_snapshot_to_durable(
    const struct app_mesh_discovery_assignment_snapshot *snapshot,
    struct app_durable_state_anchor_assignment *assignment)
{
    memset(assignment, 0, sizeof(*assignment));
    assignment->table_commitment = snapshot->table_commitment;
    assignment->pending_table_commitment =
        snapshot->pending_table_commitment;
    assignment->epoch = snapshot->epoch;
    assignment->table_command_seq = snapshot->table_command_seq;
    assignment->pending_epoch = snapshot->pending_epoch;
    assignment->pending_table_command_seq =
        snapshot->pending_table_command_seq;
    memcpy(assignment->retired_epochs,
           snapshot->retired_epochs,
           sizeof(assignment->retired_epochs));
    assignment->table_packet_seq = snapshot->table_packet_seq;
    assignment->response_spread_ms = snapshot->response_spread_ms;
    assignment->slot = snapshot->slot;
    assignment->slot_count = snapshot->slot_count;
    assignment->provisioned = snapshot->provisioned;
    assignment->retired_epoch_count = snapshot->retired_epoch_count;
    assignment->ordered_epoch_valid = snapshot->ordered_epoch_valid;
    assignment->ack_pending = snapshot->ack_pending;
    assignment->pending_slot = snapshot->pending_slot;
    assignment->pending_slot_count = snapshot->pending_slot_count;
    assignment->pending_response_lane = snapshot->pending_response_lane;
    assignment->pending_response_lane_count =
        snapshot->pending_response_lane_count;
    assignment->pending_valid = snapshot->pending_valid;
}

static void anchor_assignment_durable_to_snapshot(
    const struct app_durable_state_anchor_assignment *assignment,
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->local_id = DEVICE_ID;
    snapshot->gateway_id = GATEWAY_ID;
    snapshot->table_commitment = assignment->table_commitment;
    snapshot->pending_table_commitment =
        assignment->pending_table_commitment;
    snapshot->epoch = assignment->epoch;
    snapshot->table_command_seq = assignment->table_command_seq;
    snapshot->pending_epoch = assignment->pending_epoch;
    snapshot->pending_table_command_seq =
        assignment->pending_table_command_seq;
    memcpy(snapshot->retired_epochs,
           assignment->retired_epochs,
           sizeof(snapshot->retired_epochs));
    snapshot->table_packet_seq = assignment->table_packet_seq;
    snapshot->response_spread_ms = assignment->response_spread_ms;
    snapshot->size = sizeof(*snapshot);
    snapshot->version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION;
    snapshot->slot = assignment->slot;
    snapshot->slot_count = assignment->slot_count;
    snapshot->provisioned = assignment->provisioned;
    snapshot->valid = 1u;
    snapshot->retired_epoch_count = assignment->retired_epoch_count;
    snapshot->ordered_epoch_valid = assignment->ordered_epoch_valid;
    snapshot->ack_pending = assignment->ack_pending;
    snapshot->pending_slot = assignment->pending_slot;
    snapshot->pending_slot_count = assignment->pending_slot_count;
    snapshot->pending_response_lane = assignment->pending_response_lane;
    snapshot->pending_response_lane_count =
        assignment->pending_response_lane_count;
    snapshot->pending_valid = assignment->pending_valid;
    snapshot->ack_retry_round = 0u;
}
#endif

static int anchor_restore_discovery_assignment_ram(
    struct app_mesh_discovery_assignment_snapshot *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    *out = anchor_ram_discovery_assignment_snapshot;
    return 0;
}

static int anchor_save_discovery_assignment_semantic(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    struct app_durable_state_anchor_assignment assignment;
    int ret;

    if (snapshot == NULL) {
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_DURABLE_STATE)
    if (anchor_discovery_assignment_ram_only_iteration) {
        anchor_ram_discovery_assignment_snapshot = *snapshot;
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_SAVE mode=ram epoch=%u pending_epoch=%u slot=%u slot_count=%u provisioned=%u pending=%u\n",
            snapshot->epoch,
            snapshot->pending_epoch,
            snapshot->slot,
            snapshot->slot_count,
            snapshot->provisioned,
            snapshot->pending_valid);
        return 0;
    }
    anchor_assignment_snapshot_to_durable(snapshot, &assignment);
    ret = app_durable_state_save_anchor_assignment(GATEWAY_ID, &assignment);
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_SAVE mode=nvs ret=%d epoch=%u pending_epoch=%u slot=%u slot_count=%u provisioned=%u pending=%u\n",
        ret,
        snapshot->epoch,
        snapshot->pending_epoch,
        snapshot->slot,
        snapshot->slot_count,
        snapshot->provisioned,
        snapshot->pending_valid);
    if (ret < 0) {
        return ret;
    }
#else
    ARG_UNUSED(assignment);
    ret = 0;
#endif
    anchor_ram_discovery_assignment_snapshot = *snapshot;
    return ret;
}

static int anchor_save_discovery_assignment_retry_ram(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return -EINVAL;
    }
    anchor_ram_discovery_assignment_snapshot = *snapshot;
    return 0;
}

static int anchor_clear_discovery_assignment_semantic(void)
{
#if defined(CONFIG_IMEC_DURABLE_STATE)
    int ret = 0;

    if (!anchor_discovery_assignment_ram_only_iteration) {
        ret = app_durable_state_delete_anchor_assignment(GATEWAY_ID);
        if (ret < 0) {
            return ret;
        }
    }
#endif
    memset(&anchor_ram_discovery_assignment_snapshot, 0,
           sizeof(anchor_ram_discovery_assignment_snapshot));
    return 0;
}

static int anchor_restore_discovery_assignment_durable(void)
{
    struct app_durable_state_anchor_assignment assignment;
    struct app_mesh_discovery_assignment_snapshot snapshot;
    int ret;

    if (anchor_discovery_assignment_restore_complete) {
        return 0;
    }
    local_anchor_reset_discovery_assignment();
#if defined(CONFIG_IMEC_DURABLE_STATE)
    ret = app_durable_state_restore_anchor_assignment(GATEWAY_ID,
                                                      &assignment);
    if (ret < 0) {
        return ret;
    }
    if (ret == 0) {
        memset(&anchor_ram_discovery_assignment_snapshot,
               0,
               sizeof(anchor_ram_discovery_assignment_snapshot));
        anchor_discovery_assignment_restore_complete = true;
        return 0;
    }
    anchor_assignment_durable_to_snapshot(&assignment, &snapshot);
    ret = anchor_restore_live_discovery_assignment_snapshot(&snapshot);
    if (ret != PROTO_OK) {
        return -EILSEQ;
    }
    anchor_ram_discovery_assignment_snapshot = snapshot;
    anchor_discovery_assignment_restore_complete = true;
    LOG_INF("anchor discovery assignment restored: state=%s epoch=%u generation=%u slot=%u slot_count=%u pending_epoch=%u pending_generation=%u pending_slot=%u",
            snapshot.ack_pending != 0u ? "ACK_PENDING" :
            snapshot.provisioned != 0u ? "PROVISIONED" : "UNPROVISIONED",
            snapshot.epoch,
            snapshot.table_command_seq,
            snapshot.slot,
            snapshot.slot_count,
            snapshot.pending_epoch,
            snapshot.pending_table_command_seq,
            snapshot.pending_slot);
    return 0;
#else
    ARG_UNUSED(assignment);
    ARG_UNUSED(snapshot);
    anchor_discovery_assignment_restore_complete = true;
    return 0;
#endif
}

static int anchor_checked_work_reschedule(struct k_work_delayable *work,
                                          k_timeout_t delay,
                                          const char *source)
{
    int ret = k_work_reschedule(work, delay);

    if (ret < 0) {
        LOG_ERR("anchor state-machine owner scheduling failed closed: source=%s ret=%d",
                source == NULL ? "unknown" : source,
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static int anchor_ram_owner_work_reschedule(struct k_work_delayable *work,
                                            k_timeout_t delay,
                                            const char *source)
{
    int system_ret = k_work_reschedule(work, delay);
    int owner_ret;

    if (system_ret >= 0) {
        return system_ret;
    }
    owner_ret = mesh_route_owner_work_reschedule_timeout(work, delay);
    if (owner_ret < 0) {
        LOG_ERR("anchor RAM owner scheduling failed on both queues: source=%s system_ret=%d owner_ret=%d",
                source == NULL ? "unknown" : source,
                system_ret,
                owner_ret);
        app_watchdog_stop_feeding();
    }
    return owner_ret;
}

static bool anchor_discovery_assignment_required(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           !IS_ENABLED(CONFIG_IMEC_ML_ANCHOR) &&
           !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);
}

static int anchor_append_discovery_assignment_status(uint8_t *payload,
                                                     size_t payload_cap,
                                                     size_t *payload_len)
{
    static const uint8_t unprovisioned[] = "UNPROVISIONED";
    uint32_t epoch = 0u;
    uint8_t slot = 0u;
    uint8_t slot_count = 0u;

    if (!anchor_discovery_assignment_required()) {
        return PROTO_OK;
    }
    if (local_anchor_discovery_assignment_get(&epoch, &slot, &slot_count)) {
        return tlv_append_u32(payload,
                              payload_cap,
                              payload_len,
                              TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                              epoch);
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            payload_len,
                            TLV_ERROR_DETAIL,
                            unprovisioned,
                            (uint8_t)(sizeof(unprovisioned) - 1u));
}

static int anchor_enter_low_power(enum app_radio_low_power_mode mode,
                                  const char *reason)
{
    struct app_radio_low_power_policy policy;
    enum app_radio_low_power_action action;
    const char *mode_name = app_radio_low_power_mode_name(mode);
    int first_ret;
    int recovery_ret;
    int retry_ret = 0;
    int final_ret;
    bool retry_attempted = false;

    app_radio_low_power_policy_init(&policy, mode);
    first_ret = mode == APP_RADIO_LOW_POWER_IDLE ?
                dwm3000_driver_idle() : dwm3000_driver_standby();
    action = app_radio_low_power_policy_note_transition(&policy, first_ret);
    if (action == APP_RADIO_LOW_POWER_COMPLETE) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    action = app_radio_low_power_policy_note_recovery(&policy, recovery_ret);
    if (action == APP_RADIO_LOW_POWER_RETRY) {
        retry_attempted = true;
        retry_ret = mode == APP_RADIO_LOW_POWER_IDLE ?
                    dwm3000_driver_idle() : dwm3000_driver_standby();
        action = app_radio_low_power_policy_note_transition(&policy, retry_ret);
        if (action == APP_RADIO_LOW_POWER_COMPLETE) {
            LOG_WRN("anchor DWM3000 %s recovered after transition failure: reason=%s first_ret=%d",
                    mode_name,
                    reason,
                    first_ret);
            status_debug_printf("DBG_ANCHOR_LOW_POWER_RECOVERED mode=%s first=%d\n",
                                mode_name,
                                first_ret);
            return 0;
        }
    }

    final_ret = recovery_ret < 0 ? recovery_ret : retry_ret;
    if (anchor_low_power_transition_failures != UINT32_MAX) {
        anchor_low_power_transition_failures++;
    }
    LOG_ERR("anchor DWM3000 %s failed after bounded recovery: reason=%s first_ret=%d recovery_ret=%d retry_attempted=%u retry_ret=%d failures=%u",
            mode_name,
            reason,
            first_ret,
            recovery_ret,
            retry_attempted ? 1u : 0u,
            retry_ret,
            anchor_low_power_transition_failures);
    status_debug_printf("DBG_ANCHOR_LOW_POWER_FAILED mode=%s ret=%d count=%u\n",
                        mode_name,
                        final_ret,
                        anchor_low_power_transition_failures);
    return final_ret;
}

static int anchor_collection_node_boot_id(uint32_t *boot_id)
{
    if (boot_id == NULL) {
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_DURABLE_STATE)
    return app_durable_state_boot_incarnation(boot_id);
#else
    *boot_id = 0u;
    return -ENOTSUP;
#endif
}

static uint16_t anchor_next_collection_result_seq(void)
{
    anchor_collection_result_seq =
        app_anchor_command_completion_next_result_seq(
            anchor_collection_result_seq);
    return anchor_collection_result_seq;
}

static int anchor_commit_broadcast_command_replay(
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    uint32_t now_ms)
{
    struct app_mesh_command_orchestrator *orchestrator =
        mesh_gateway_command_orchestrator_context();
    struct app_mesh_gateway_command_anchor_state candidate;

    if (packet == NULL || options == NULL ||
        packet->dst_id != MESH_BROADCAST_ID ||
        options->command_seq == 0u) {
        return -EINVAL;
    }
    candidate = orchestrator->anchor;
    app_mesh_gateway_command_flow_anchor_remember(
        &candidate, packet, options, now_ms);
    orchestrator->anchor = candidate;
    return 0;
}

static int anchor_commit_pending_collection_command_replay(void)
{
    struct gateway_command_options options = {0};

    if (!anchor_collection_result_pending.active ||
        anchor_collection_result_pending.command.dst_id !=
            MESH_BROADCAST_ID) {
        return 0;
    }
    options.command_seq =
        anchor_collection_result_pending.result_id.command_seq;
    return anchor_commit_broadcast_command_replay(
        &anchor_collection_result_pending.command,
        &options,
        k_uptime_get_32());
}

static int anchor_submit_command_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    const struct command_result_id *collection_result_id,
    uint32_t collection_epoch_id,
    const struct app_node_comm_reservation_lease *protocol_reservation,
    uint32_t *delivery_handle_out)
{
    struct mesh_outbound outbound = {0};
    uint32_t delivery_handle = 0u;
    uint32_t client_token;
    uint32_t result_timeout_ms;
    uint64_t absolute_deadline_ms;
    uint64_t now_ms;
    size_t payload_len = 0u;
    uint32_t session_id;
    uint16_t seq;
    bool auto_reap_with_handle;
    bool diagnostic;
    int ret;

    if (command == NULL) {
        return -EINVAL;
    }

    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (status == COMMAND_OK && command_id == CMD_GET_STATUS) {
        ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
        if (ret == PROTO_OK) {
            ret = anchor_append_discovery_assignment_status(outbound.payload,
                                                            sizeof(outbound.payload),
                                                            &payload_len);
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    if (collection_result_id != NULL) {
        ret = gateway_command_append_collection_result_identity(outbound.payload,
                                                               sizeof(outbound.payload),
                                                               &payload_len,
                                                               collection_result_id,
                                                               collection_epoch_id);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        session_id = collection_result_id->command_seq;
        seq = collection_result_id->result_seq;
    } else {
        session_id = command->session_id;
        seq = command->seq;
    }

    diagnostic = (command->flags & FLAG_DIAGNOSTIC) != 0u;
    outbound.payload_len = (uint16_t)payload_len;
    ret = app_mesh_command_orchestrator_anchor_result(command,
                                                       DEVICE_ID,
                                                       GATEWAY_ID,
                                                       diagnostic,
                                                       &outbound);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.packet.session_id = session_id;
    outbound.packet.seq = seq;

    now_ms = (uint64_t)k_uptime_get();
    result_timeout_ms = GATEWAY_COMMAND_RESULT_TIMEOUT_MS;
    absolute_deadline_ms = UINT64_MAX - now_ms <
                           result_timeout_ms ?
                           UINT64_MAX :
                           now_ms + result_timeout_ms;
    client_token = ((uint32_t)command_id << 16) | seq;
    auto_reap_with_handle = false;
    if (protocol_reservation == NULL) {
        ret = auto_reap_with_handle ?
            app_node_comm_submit_protocol_response_auto_reap(
                &outbound,
                absolute_deadline_ms,
                client_token,
                &delivery_handle) :
            app_node_comm_submit_protocol_response(
                &outbound,
                absolute_deadline_ms,
                client_token,
                delivery_handle_out == NULL ? NULL : &delivery_handle);
    } else {
        ret = auto_reap_with_handle ?
            app_node_comm_commit_protocol_response_auto_reap(
                protocol_reservation,
                &outbound,
                absolute_deadline_ms,
                client_token,
                &delivery_handle) :
            app_node_comm_commit_protocol_response(
                protocol_reservation,
                &outbound,
                absolute_deadline_ms,
                client_token,
                delivery_handle_out == NULL ? NULL : &delivery_handle);
    }
    if (ret == 0) {
        if (delivery_handle_out != NULL) {
            *delivery_handle_out = delivery_handle;
        }
    }
    return ret;
}

static int anchor_send_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason,
                                      const struct command_result_id *collection_result_id,
                                      uint32_t collection_epoch_id)
{
    return anchor_submit_command_result(command,
                                        command_id,
                                        status,
                                        reason,
                                        collection_result_id,
                                        collection_epoch_id,
                                        NULL,
                                        NULL);
}

static uint8_t anchor_command_completion_actions(
    enum command_status status,
    bool force_rediscovery_after_result,
    bool reboot_after_result)
{
    uint8_t actions = 0u;

    if (status != COMMAND_OK) {
        return 0u;
    }
    if (force_rediscovery_after_result) {
        actions |= APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY;
    }
    if (reboot_after_result) {
        actions |= APP_ANCHOR_COMMAND_COMPLETION_REBOOT;
    }
    return actions;
}

static uint8_t anchor_discovery_gateway_hop_count(void)
{
    const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);

    if (selected == NULL || selected->hop_count == UINT8_MAX) {
        return 0u;
    }
    /* Route candidates count intermediate relays; assignment reports RF hops. */
    return selected->hop_count + 1u;
}

static int anchor_discovery_claim_reschedule_locked(
    uint32_t generation,
    uint64_t not_before_ms);

static uint32_t anchor_discovery_response_delay_ms(
    const struct anchor_discovery_claim_pending *pending)
{
    uint32_t delay_ms = DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;

    if (pending == NULL ||
        discovery_assignment_response_delay_ms(pending->response_lane,
                                               pending->response_lane_count,
                                               pending->hop_count,
                                               pending->response_spread_ms,
                                               pending->attempt,
                                               pending->first_contact_random,
                                               &delay_ms) != PROTO_OK) {
        return DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;
    }
    return delay_ms;
}

static int anchor_discovery_postpone_for_deeper_route(
    struct anchor_discovery_claim_pending *snapshot)
{
    struct anchor_discovery_claim_pending candidate;
    uint64_t deadline_ms;
    uint64_t not_before_ms;
    uint32_t delay_ms = 0u;
    uint8_t hop_count;
    int schedule_ret = 0;

    if (snapshot == NULL || snapshot->delivery_handle != 0u ||
        snapshot->terminal_retry_count != 0u || snapshot->ack_delivered) {
        return 0;
    }
    hop_count = anchor_discovery_gateway_hop_count();
    if (hop_count == 0u || hop_count <= snapshot->hop_count) {
        return 0;
    }
    candidate = *snapshot;
    candidate.hop_count = hop_count;
    candidate.attempt = 0u;
    if (discovery_assignment_response_delay_ms(
            candidate.response_lane,
            candidate.response_lane_count,
            candidate.hop_count,
            candidate.response_spread_ms,
            0u,
            candidate.first_contact_random,
            &delay_ms) != PROTO_OK) {
        return -EINVAL;
    }
    not_before_ms = candidate.first_contact_origin_ms + delay_ms;
    deadline_ms = discovery_assignment_response_deadline_ms(
        candidate.first_contact_origin_ms, delay_ms, hop_count);

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.generation == snapshot->generation &&
        anchor_discovery_claim_pending.delivery_handle == 0u &&
        anchor_discovery_claim_pending.terminal_retry_count == 0u &&
        !anchor_discovery_claim_pending.ack_delivered &&
        hop_count > anchor_discovery_claim_pending.hop_count) {
        anchor_discovery_claim_pending.hop_count = hop_count;
        if (not_before_ms >
            anchor_discovery_claim_pending.next_attempt_not_before_ms) {
            anchor_discovery_claim_pending.next_attempt_not_before_ms =
                not_before_ms;
            if (deadline_ms >
                anchor_discovery_claim_pending.absolute_deadline_ms) {
                anchor_discovery_claim_pending.absolute_deadline_ms =
                    deadline_ms;
            }
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                snapshot->generation, not_before_ms);
        }
        *snapshot = anchor_discovery_claim_pending;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return schedule_ret < 0 ? schedule_ret : 1;
}

static uint32_t anchor_discovery_ack_low_duty_retry_delay_ms(
    uint8_t terminal_retry_count,
    uint32_t random_value)
{
    uint32_t delay_ms = ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS;
    uint8_t low_duty_round =
        terminal_retry_count > DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES ?
        terminal_retry_count - DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES :
        0u;

    for (uint8_t i = 0u; i < low_duty_round; i++) {
        if (delay_ms >=
            ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS / 2u) {
            delay_ms = ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS;
            break;
        }
        delay_ms *= 2u;
    }
    if (delay_ms < ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS) {
        uint32_t jitter_cap = delay_ms / 4u;
        uint32_t jitter =
            jitter_cap == 0u ? 0u : random_value % (jitter_cap + 1u);

        delay_ms = MIN(
            delay_ms + jitter,
            ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS);
    }
    return delay_ms;
}

static int anchor_build_discovery_response(
    const struct anchor_discovery_claim_pending *pending,
    struct mesh_outbound *outbound)
{
    uint64_t hash;
    size_t payload_len = 0u;
    int ret;

    if (pending == NULL || outbound == NULL || pending->epoch == 0u) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    hash = discovery_assignment_hash(DEVICE_ID);
    ret = mesh_append_command_result(outbound->payload,
                                     sizeof(outbound->payload),
                                     &payload_len,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     COMMAND_OK,
                                     0u);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
             outbound->payload,
             sizeof(outbound->payload),
             &payload_len,
             pending->phase,
             pending->epoch);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(outbound->payload,
                                                     sizeof(outbound->payload),
                                                     &payload_len,
                                                     hash);
    }
    if (ret == PROTO_OK &&
        pending->phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        ret = discovery_assignment_append_table_commitment(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            &pending->table_commitment);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_HOP_COUNT,
                            pending->hop_count);
    }
    if (ret == PROTO_OK) {
        ret = mesh_init_command_result(&outbound->packet,
                                       DEVICE_ID,
                                       GATEWAY_ID,
                                       pending->command.session_id,
                                       pending->command.seq,
                                       (uint8_t)payload_len,
                                       true);
    }
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound->payload_len = (uint16_t)payload_len;
    return 0;
}

static int anchor_send_discovery_response(
    const struct anchor_discovery_claim_pending *pending,
    uint32_t *delivery_handle_out)
{
    struct mesh_outbound outbound = {0};
    uint64_t hash;
    int ret;

    ret = anchor_build_discovery_response(pending, &outbound);
    if (ret < 0) {
        return ret;
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    ret = app_node_comm_submit_protocol_response(
        &outbound,
        pending->absolute_deadline_ms,
        ((uint32_t)CMD_ASSIGN_DISCOVERY_SLOTS << 16) |
            pending->command.seq,
        delivery_handle_out);
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE phase=%u epoch=%u hash=0x%016llx hop=%u slot=%u attempt=%u ret=%d\n",
                        pending->phase,
                        pending->epoch,
                        (unsigned long long)hash,
                        pending->hop_count,
                        pending->slot,
                        pending->attempt + 1u,
                        ret);
    if (ret == 0) {
    }
    return ret;
}

static uint32_t anchor_discovery_claim_next_generation(uint32_t generation)
{
    generation++;
    return generation == 0u ? 1u : generation;
}

static bool anchor_discovery_claim_snapshot(
    struct anchor_discovery_claim_pending *snapshot)
{
    bool active;

    if (snapshot == NULL) {
        return false;
    }
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    active = anchor_discovery_claim_pending.active;
    if (active) {
        *snapshot = anchor_discovery_claim_pending;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return active;
}

static int anchor_discovery_claim_reschedule_locked(
    uint32_t generation,
    uint64_t not_before_ms)
{
    uint64_t delay_ms;
    uint64_t now_ms;

    if (!anchor_discovery_claim_pending.active ||
        anchor_discovery_claim_pending.generation != generation) {
        return -ESTALE;
    }
    anchor_discovery_claim_pending.next_attempt_not_before_ms = not_before_ms;
    now_ms = (uint64_t)k_uptime_get();
    delay_ms = not_before_ms > now_ms ? not_before_ms - now_ms : 0u;
    if (delay_ms > UINT32_MAX) {
        delay_ms = UINT32_MAX;
    }
    /*
     * Keep the generation check and timer replacement in one critical
     * section.  Otherwise an old handler can overwrite a newer response's
     * randomized timer after the newer generation has taken custody.
     * Rescheduling never invokes the handler synchronously.
     */
    {
        int ret = mesh_route_work_reschedule(
            &anchor_discovery_claim_work, (uint32_t)delay_ms);

        /*
         * A durable ACK may be scheduled while node communication is paused
         * for another serialized radio owner. Keep its timer on the same
         * communication workqueue; the worker will retry admission after the
         * pause instead of requiring a reset or TABLE replay.
         */
        if (ret < 0 &&
            anchor_discovery_claim_pending.phase ==
                DISCOVERY_ASSIGNMENT_PHASE_ACK) {
            ret = mesh_route_owner_work_reschedule(
                &anchor_discovery_claim_work, (uint32_t)delay_ms);
#if DEVICE_ROLE == ROLE_ANCHOR
            if (ret < 0) {
                /*
                 * This system-work owner never performs radio work. It only
                 * retries publication onto the serialized communication
                 * queue, so a transient queue/scheduling failure cannot
                 * strand the durable ACK.
                 */
                anchor_discovery_ack_liveness_generation = generation;
                ret = anchor_checked_work_reschedule(
                    &anchor_discovery_ack_liveness_work,
                    K_MSEC(MAX((uint32_t)delay_ms, 1u)),
                    "assignment-ack-liveness");
                if (ret < 0 &&
                    anchor_discovery_ack_liveness_generation ==
                        generation) {
                    anchor_discovery_ack_liveness_generation = 0u;
                }
            }
#endif
        }
        return ret;
    }
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void anchor_discovery_ack_liveness_work_handler(struct k_work *work)
{
    uint32_t generation;
    bool route_wait_pending;
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    route_wait_pending = anchor_discovery_ack_route_wait_pending;
    anchor_discovery_ack_route_wait_pending = false;
    if (route_wait_pending) {
        anchor_discovery_ack_liveness_generation = 0u;
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        ret = anchor_resume_pending_discovery_assignment_ack(false);
        if (ret < 0) {
            LOG_ERR("anchor pending TABLE ACK route-wait resume failed: %d",
                    ret);
            app_watchdog_stop_feeding();
        }
        return;
    }
    if (!anchor_discovery_claim_pending.active ||
        anchor_discovery_claim_pending.phase !=
            DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        return;
    }
    generation = anchor_discovery_ack_liveness_generation;
    anchor_discovery_ack_liveness_generation = 0u;
    if (generation == 0u ||
        anchor_discovery_claim_pending.generation != generation) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        return;
    }
    ret = anchor_discovery_claim_reschedule_locked(
        generation,
        anchor_discovery_claim_pending.next_attempt_not_before_ms);
    if (ret < 0 &&
        anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.generation == generation) {
        anchor_discovery_ack_liveness_generation = generation;
        (void)anchor_checked_work_reschedule(
            &anchor_discovery_ack_liveness_work,
            K_MSEC(DISCOVERY_ASSIGNMENT_RETRY_BASE_MS),
            "assignment-ack-liveness-retry");
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
}
#endif

static void anchor_discovery_ack_refresh_attempt_deadline_locked(
    uint64_t not_before_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t delay_ms = not_before_ms > now_ms ?
                        not_before_ms - now_ms : 0u;

    if (anchor_discovery_claim_pending.phase !=
            DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return;
    }
    if (delay_ms > UINT32_MAX) {
        delay_ms = UINT32_MAX;
    }
    anchor_discovery_claim_pending.absolute_deadline_ms =
        discovery_assignment_response_deadline_ms(
            now_ms,
            (uint32_t)delay_ms,
        anchor_discovery_claim_pending.hop_count);
}

static void anchor_discovery_terminal_retry_refresh_deadline_locked(
    uint64_t not_before_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t delay_ms = not_before_ms > now_ms ?
                        not_before_ms - now_ms : 0u;

    if (anchor_discovery_claim_pending.phase !=
            DISCOVERY_ASSIGNMENT_PHASE_ACK &&
        (anchor_discovery_claim_pending.phase !=
             DISCOVERY_ASSIGNMENT_PHASE_CLAIM ||
         anchor_discovery_claim_pending.terminal_retry_count >
             DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES)) {
        return;
    }
    if (delay_ms > UINT32_MAX) {
        delay_ms = UINT32_MAX;
    }
    anchor_discovery_claim_pending.absolute_deadline_ms =
        discovery_assignment_response_deadline_ms(
            now_ms,
            (uint32_t)delay_ms,
            anchor_discovery_claim_pending.hop_count);
}

static bool anchor_discovery_claim_ready_for_submit(
    uint32_t generation,
    uint64_t not_before_ms)
{
    bool ready;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    ready = anchor_discovery_claim_pending.active &&
            anchor_discovery_claim_pending.generation == generation &&
            anchor_discovery_claim_pending.delivery_handle == 0u &&
            anchor_discovery_claim_failed_abandon_handle == 0u &&
            anchor_discovery_claim_pending.next_attempt_not_before_ms ==
                not_before_ms &&
            (uint64_t)k_uptime_get() >= not_before_ms;
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return ready;
}

static int anchor_discovery_claim_service_failed_abandon(void)
{
    uint32_t handle;
    int ret;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    handle = anchor_discovery_claim_failed_abandon_handle;
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    if (handle == 0u) {
        return 0;
    }

    ret = app_node_comm_abandon_delivery(handle);
    if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
        LOG_ERR("anchor discovery response orphan cleanup retry failed: handle=%u ret=%d",
                handle,
                ret);
        app_watchdog_stop_feeding();
        return ret;
    }
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_failed_abandon_handle == handle) {
        anchor_discovery_claim_failed_abandon_handle = 0u;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return 0;
}

static int anchor_promote_discovery_assignment_locked_impl(
    const struct anchor_discovery_claim_pending *pending,
    bool require_table_ack)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    int ret;

    if (pending == NULL ||
        pending->phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return -EINVAL;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid ||
        snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID) {
        return -ESTALE;
    }
    if (snapshot.ack_pending == 0u &&
        snapshot.provisioned != 0u &&
        snapshot.epoch == pending->epoch &&
        snapshot.table_command_seq == pending->command.session_id &&
        discovery_assignment_table_commitment_equal(
            &snapshot.table_commitment,
            &pending->table_commitment) &&
        snapshot.slot == pending->slot &&
        snapshot.slot_count == pending->slot_count) {
        return 0;
    }
    if ((require_table_ack && snapshot.ack_pending == 0u) ||
        snapshot.pending_valid == 0u ||
        snapshot.pending_epoch != pending->epoch ||
        snapshot.pending_table_command_seq != pending->command.session_id ||
        !discovery_assignment_table_commitment_equal(
            &snapshot.pending_table_commitment,
            &pending->table_commitment) ||
        snapshot.pending_slot != pending->slot ||
        snapshot.pending_slot_count != pending->slot_count ||
        (snapshot.pending_response_lane_count != 0u &&
         (snapshot.pending_response_lane != pending->response_lane ||
          snapshot.pending_response_lane_count !=
              pending->response_lane_count)) ||
        snapshot.table_packet_seq != pending->command.seq ||
        snapshot.response_spread_ms != pending->response_spread_ms) {
        return -ESTALE;
    }

    /*
     * END commits the exact pending TABLE. Legacy ACK recovery reaches this
     * same point only after its physical TABLE response was delivered.
     */
    if (!local_anchor_discovery_assignment_project_pending_commit(
            snapshot.pending_epoch,
            snapshot.pending_table_command_seq,
            &snapshot.pending_table_commitment,
            snapshot.retired_epochs,
            &snapshot.retired_epoch_count)) {
        return -ESTALE;
    }
    snapshot.epoch = snapshot.pending_epoch;
    snapshot.table_command_seq = snapshot.pending_table_command_seq;
    snapshot.table_commitment = snapshot.pending_table_commitment;
    snapshot.slot = snapshot.pending_slot;
    snapshot.slot_count = snapshot.pending_slot_count;
    snapshot.provisioned = 1u;
    snapshot.ack_pending = 0u;
    snapshot.table_packet_seq = 0u;
    snapshot.response_spread_ms = 0u;
    snapshot.pending_epoch = 0u;
    snapshot.pending_table_command_seq = 0u;
    memset(&snapshot.pending_table_commitment,
           0,
           sizeof(snapshot.pending_table_commitment));
    snapshot.pending_slot = 0u;
    snapshot.pending_slot_count = 0u;
    snapshot.pending_response_lane = 0u;
    snapshot.pending_response_lane_count = 0u;
    snapshot.pending_valid = 0u;
    snapshot.ack_retry_round = 0u;
    ret = anchor_save_discovery_assignment_semantic(&snapshot);
    if (ret < 0) {
        return ret;
    }
    ret = local_anchor_commit_discovery_assignment(
        snapshot.epoch,
        snapshot.table_command_seq,
        &snapshot.table_commitment,
        snapshot.slot,
        snapshot.slot_count);
    if (ret != PROTO_OK) {
        /*
         * Persistence is authoritative after ACK delivery.  Rebuild the live
         * policy from that exact committed record instead of leaving RAM
         * unprovisioned until the next reset.
         */
        ret = local_anchor_restore_discovery_assignment(
            snapshot.epoch,
            snapshot.table_command_seq,
            &snapshot.table_commitment,
            snapshot.slot,
            snapshot.slot_count,
            true,
            snapshot.ordered_epoch_valid,
            snapshot.retired_epochs,
            snapshot.retired_epoch_count,
            0u,
            0u,
            NULL,
            false);
        if (ret != PROTO_OK) {
            return -ESTALE;
        }
    }
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_ASSIGNED state=PROVISIONED epoch=%u slot=%u slot_count=%u proof=%s\n",
        snapshot.epoch,
        snapshot.slot,
        snapshot.slot_count,
        require_table_ack ? "table-ack" : "end");
    LOG_INF("anchor discovery slot committed after %s: epoch=%u slot=%u slot_count=%u",
            require_table_ack ? "TABLE ACK" : "END",
            snapshot.epoch,
            snapshot.slot,
            snapshot.slot_count);
    return 0;
}

static int anchor_promote_discovery_assignment_after_ack_locked(
    const struct anchor_discovery_claim_pending *pending)
{
    return anchor_promote_discovery_assignment_locked_impl(pending, true);
}

static int anchor_commit_discovery_table_locked(
    struct app_mesh_discovery_assignment_snapshot *snapshot,
    const struct app_mesh_discovery_assignment_snapshot *original_snapshot,
    uint32_t epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    bool provisioned,
    uint8_t slot,
    uint8_t slot_count)
{
    struct discovery_assignment_table_commitment committed_table;
    int ret;

    if (snapshot == NULL || original_snapshot == NULL || epoch == 0u ||
        table_command_seq == 0u || table_commitment == NULL ||
        (provisioned && (slot_count == 0u || slot >= slot_count))) {
        return -EINVAL;
    }
    committed_table = *table_commitment;
    if (!local_anchor_discovery_assignment_project_pending_commit(
            epoch,
            table_command_seq,
            &committed_table,
            snapshot->retired_epochs,
            &snapshot->retired_epoch_count)) {
        return -ESTALE;
    }

    snapshot->local_id = DEVICE_ID;
    snapshot->gateway_id = GATEWAY_ID;
    snapshot->size = sizeof(*snapshot);
    snapshot->version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION;
    snapshot->valid = true;
    snapshot->ordered_epoch_valid = true;
    snapshot->epoch = epoch;
    snapshot->table_command_seq = table_command_seq;
    snapshot->table_commitment = committed_table;
    snapshot->slot = provisioned ? slot : 0u;
    snapshot->slot_count = provisioned ? slot_count : 0u;
    snapshot->provisioned = provisioned;
    snapshot->ack_pending = false;
    snapshot->table_packet_seq = 0u;
    snapshot->response_spread_ms = 0u;
    snapshot->pending_epoch = 0u;
    snapshot->pending_table_command_seq = 0u;
    memset(&snapshot->pending_table_commitment,
           0,
           sizeof(snapshot->pending_table_commitment));
    snapshot->pending_slot = 0u;
    snapshot->pending_slot_count = 0u;
    snapshot->pending_response_lane = 0u;
    snapshot->pending_response_lane_count = 0u;
    snapshot->pending_valid = false;
    snapshot->ack_retry_round = 0u;

    ret = anchor_save_discovery_assignment_semantic(snapshot);
    if (ret < 0) {
        (void)anchor_restore_live_discovery_assignment_snapshot(
            original_snapshot);
        return ret;
    }
    ret = provisioned ?
        local_anchor_commit_discovery_assignment(
            epoch, table_command_seq, &committed_table, slot, slot_count) :
        local_anchor_mark_discovery_assignment_unprovisioned(
            epoch, table_command_seq, &committed_table);
    if (ret != PROTO_OK &&
        anchor_restore_live_discovery_assignment_snapshot(snapshot) !=
            PROTO_OK) {
        app_watchdog_stop_feeding();
        return -EILSEQ;
    }
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_TABLE_COMMIT epoch=%u table_seq=%u provisioned=%u slot=%u slot_count=%u\n",
        epoch,
        table_command_seq,
        provisioned ? 1u : 0u,
        provisioned ? slot : 0u,
        provisioned ? slot_count : 0u);
    return 0;
}

static int anchor_promote_discovery_assignment_after_ack(
    const struct anchor_discovery_claim_pending *pending)
{
    int ret;

    /*
     * TABLE handling and delivered-ACK promotion both update the same
     * snapshot and live policy. Keep their complete read/modify/write
     * transitions under one owner so neither can overwrite the other's
     * newer durable state with a stale local copy.
     */
    k_mutex_lock(&anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    ret = anchor_promote_discovery_assignment_after_ack_locked(pending);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    return ret;
}

static int anchor_discovery_assignment_gateway_accepted(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct anchor_discovery_claim_pending pending;
    struct mesh_outbound expected = {0};
    uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (packet == NULL || semantic_digest == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->src_id != DEVICE_ID || packet->dst_id != GATEWAY_ID) {
        return -ENOENT;
    }
    k_mutex_lock(
        &anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (!anchor_discovery_claim_pending.active ||
        anchor_discovery_claim_pending.phase !=
            DISCOVERY_ASSIGNMENT_PHASE_ACK ||
        packet->session_id !=
            anchor_discovery_claim_pending.command.session_id ||
        packet->seq != anchor_discovery_claim_pending.command.seq) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return -ENOENT;
    }
    pending = anchor_discovery_claim_pending;
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    ret = anchor_build_discovery_response(&pending, &expected);
    if (ret < 0 ||
        !mesh_packet_semantic_digest(&expected.packet,
                                     expected.payload,
                                     expected.payload_len,
                                     expected_digest) ||
        memcmp(expected_digest,
               semantic_digest,
               sizeof(expected_digest)) != 0) {
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return ret < 0 ? ret : -EBADMSG;
    }
    ret = anchor_promote_discovery_assignment_after_ack_locked(&pending);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    if (ret == 0) {
        status_debug_printf("DBG_ENUM_ACCEPT epoch=%u slot=%u\n",
                            pending.epoch, pending.slot);
    }
    return ret;
}

static int anchor_discovery_assignment_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct anchor_discovery_claim_pending pending;
    struct mesh_outbound expected = {0};
    uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (packet == NULL || semantic_digest == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->src_id != DEVICE_ID || packet->dst_id != GATEWAY_ID) {
        return -ENOENT;
    }

    k_mutex_lock(
        &anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (!anchor_discovery_claim_pending.active ||
        packet->session_id !=
            anchor_discovery_claim_pending.command.session_id ||
        packet->seq != anchor_discovery_claim_pending.command.seq) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return -ENOENT;
    }
    pending = anchor_discovery_claim_pending;
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    ret = anchor_build_discovery_response(&pending, &expected);
    if (ret < 0 ||
        !mesh_packet_semantic_digest(&expected.packet,
                                     expected.payload,
                                     expected.payload_len,
                                     expected_digest) ||
        memcmp(expected_digest,
               semantic_digest,
               sizeof(expected_digest)) != 0) {
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return ret < 0 ? ret : -EBADMSG;
    }

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    anchor_discovery_response_terminal.epoch = pending.epoch;
    anchor_discovery_response_terminal.command_session_id =
        pending.command.session_id;
    anchor_discovery_response_terminal.command_packet_seq =
        pending.command.seq;
    anchor_discovery_response_terminal.phase = pending.phase;
    anchor_discovery_response_terminal.table_commitment =
        pending.table_commitment;
    anchor_discovery_response_terminal.valid = true;
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    k_mutex_unlock(
        &anchor_discovery_assignment_transaction_mutex);

    status_debug_printf(
        "DBG_DISCOVERY_SLOT_RESPONSE_TERMINAL phase=%u epoch=%u generation=%u\n",
        pending.phase,
        pending.epoch,
        pending.command.session_id);
    return 0;
}

static bool anchor_discovery_response_terminal_matches(
    uint32_t epoch,
    enum discovery_assignment_phase response_phase,
    uint32_t command_session_id,
    uint16_t command_packet_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    bool matches;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    matches = anchor_discovery_response_terminal.valid &&
        anchor_discovery_response_terminal.epoch == epoch &&
        anchor_discovery_response_terminal.phase == response_phase &&
        anchor_discovery_response_terminal.command_session_id ==
            command_session_id &&
        anchor_discovery_response_terminal.command_packet_seq ==
            command_packet_seq &&
        ((response_phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
          table_commitment == NULL) ||
         (response_phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
          table_commitment != NULL &&
          discovery_assignment_table_commitment_equal(
              &anchor_discovery_response_terminal.table_commitment,
              table_commitment)));
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return matches;
}

static int anchor_persist_discovery_assignment_ack_retry_round(
    const struct anchor_discovery_claim_pending *pending,
    uint8_t retry_round)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    int ret;

    if (pending == NULL ||
        pending->phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return -EINVAL;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid || snapshot.pending_valid == 0u ||
        snapshot.ack_pending == 0u ||
        snapshot.pending_epoch != pending->epoch ||
        snapshot.pending_table_command_seq != pending->command.session_id ||
        !discovery_assignment_table_commitment_equal(
            &snapshot.pending_table_commitment,
            &pending->table_commitment) ||
        snapshot.pending_slot != pending->slot ||
        snapshot.pending_slot_count != pending->slot_count ||
        (snapshot.pending_response_lane_count != 0u &&
         (snapshot.pending_response_lane != pending->response_lane ||
          snapshot.pending_response_lane_count !=
              pending->response_lane_count)) ||
        snapshot.table_packet_seq != pending->command.seq) {
        return -ESTALE;
    }
    if (snapshot.ack_retry_round == retry_round) {
        return 0;
    }
    snapshot.ack_retry_round = retry_round;
    return anchor_save_discovery_assignment_retry_ram(&snapshot);
}

static void anchor_discovery_assignment_service_ack_promotion(
    const struct anchor_discovery_claim_pending *pending)
{
    uint32_t retry_ms = 0u;
    uint8_t attempt = 0u;
    bool fail_closed = false;
    bool retryable;
    int schedule_ret = 0;
    int ret;

    if (pending == NULL || !pending->ack_delivered) {
        return;
    }
    ret = anchor_promote_discovery_assignment_after_ack(pending);
    retryable = ret == -EIO || ret == -ETIMEDOUT || ret == -EINTR ||
                ret == -EAGAIN || ret == -ENOSPC || ret == -EBUSY ||
                ret == -ENODEV || ret == -ENOMEM;
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.generation == pending->generation &&
        anchor_discovery_claim_pending.ack_delivered) {
        if (ret == 0) {
            anchor_discovery_claim_pending.active = false;
        } else if (retryable) {
            retry_ms = discovery_assignment_retry_backoff_ms(
                anchor_discovery_claim_pending.attempt,
                sys_rand32_get());
            if (anchor_discovery_claim_pending.attempt < UINT8_MAX) {
                anchor_discovery_claim_pending.attempt++;
            }
            attempt = anchor_discovery_claim_pending.attempt;
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                pending->generation,
                (uint64_t)k_uptime_get() + retry_ms);
            if (schedule_ret < 0) {
                fail_closed = true;
            }
        } else {
            /*
             * Corrupt, stale-schema, or identity-mismatched evidence cannot
             * be repaired by another write. Retain exact promotion custody
             * and force reset/recovery instead of silently exposing the old
             * live slot after its TABLE ACK was already delivered.
             */
            fail_closed = true;
        }
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    if (fail_closed) {
        app_watchdog_stop_feeding();
    }
    if (ret < 0) {
        LOG_ERR("anchor TABLE ACK durable promotion %s: epoch=%u ret=%d delay=%u attempt=%u schedule_ret=%d",
                retryable ? "retry" : "rejected",
                pending->epoch,
                ret,
                retry_ms,
                attempt,
                schedule_ret);
    }
}

static int anchor_restore_live_discovery_assignment_snapshot(
    const struct app_mesh_discovery_assignment_snapshot *snapshot);
static int anchor_resume_pending_discovery_assignment_ack(
    bool explicit_table_replay);

static int anchor_retire_superseded_discovery_ack(
    const struct anchor_discovery_claim_pending *pending)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    int rearm_ret;
    int ret;

    if (pending == NULL ||
        pending->phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return -EINVAL;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid || snapshot.pending_valid == 0u ||
        snapshot.ack_pending == 0u ||
        snapshot.pending_epoch != pending->epoch ||
        snapshot.pending_table_command_seq != pending->command.session_id ||
        !discovery_assignment_table_commitment_equal(
            &snapshot.pending_table_commitment,
            &pending->table_commitment) ||
        snapshot.pending_slot != pending->slot ||
        snapshot.pending_slot_count != pending->slot_count ||
        (snapshot.pending_response_lane_count != 0u &&
         (snapshot.pending_response_lane != pending->response_lane ||
          snapshot.pending_response_lane_count !=
              pending->response_lane_count)) ||
        snapshot.table_packet_seq != pending->command.seq) {
        return -ESTALE;
    }

    if (snapshot.provisioned != 0u) {
        /*
         * The v7 history orders retired finalized assignments behind the
         * current committed epoch; an uncommitted newer candidate cannot be
         * inserted there. Clear only its exact response envelope and keep the
         * committed slot/history unchanged. A delayed old TABLE may be parsed
         * again, but it cannot obtain gateway completion without the matching
         * durable gateway proof.
         */
        snapshot.ack_pending = 0u;
        snapshot.table_packet_seq = 0u;
        snapshot.response_spread_ms = 0u;
        snapshot.pending_epoch = 0u;
        snapshot.pending_table_command_seq = 0u;
        memset(&snapshot.pending_table_commitment,
               0,
               sizeof(snapshot.pending_table_commitment));
        snapshot.pending_slot = 0u;
        snapshot.pending_slot_count = 0u;
        snapshot.pending_response_lane = 0u;
        snapshot.pending_response_lane_count = 0u;
        snapshot.pending_valid = 0u;
        snapshot.ack_retry_round = 0u;
        ret = anchor_save_discovery_assignment_semantic(&snapshot);
    } else {
        /*
         * A first-assignment candidate has no finalized identity, so a
         * pending-less v7 record is invalid. Delete that exact obsolete record
         * through the checked path instead of pretending an empty snapshot was
         * committed.
         */
        ret = anchor_clear_discovery_assignment_semantic();
        if (ret == 0) {
            memset(&snapshot, 0, sizeof(snapshot));
        }
    }
    if (ret < 0) {
        rearm_ret = anchor_resume_pending_discovery_assignment_ack(false);
        if (rearm_ret < 0) {
            LOG_ERR("superseded discovery ACK clear failed and rearm failed: epoch=%u clear_ret=%d rearm_ret=%d",
                    pending->epoch,
                    ret,
                    rearm_ret);
        }
        return ret;
    }
    ret = anchor_restore_live_discovery_assignment_snapshot(&snapshot);
    if (ret != PROTO_OK) {
        LOG_ERR("superseded discovery ACK durable clear could not restore live policy: epoch=%u ret=%d",
                pending->epoch,
                ret);
        app_watchdog_stop_feeding();
        return -ESTALE;
    }
    return 0;
}

static void anchor_discovery_claim_work_handler(struct k_work *work)
{
    struct anchor_discovery_claim_pending pending;
    struct node_comm_terminal_event event;
    uint32_t delivery_handle = 0u;
    uint32_t retry_ms = 0u;
    uint64_t now_ms;
    uint64_t retry_not_before_ms = 0u;
    uint8_t attempt = 0u;
    bool installed = false;
    bool low_duty_retry = false;
    bool retry = false;
    bool transient;
    int persistence_ret = 0;
    int schedule_ret = 0;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    ret = anchor_discovery_claim_service_failed_abandon();
    if (ret < 0 || !anchor_discovery_claim_snapshot(&pending)) {
        return;
    }
    ret = anchor_discovery_postpone_for_deeper_route(&pending);
    if (ret < 0) {
        LOG_ERR("anchor discovery route-depth postponement failed: phase=%u epoch=%u ret=%d",
                pending.phase, pending.epoch, ret);
        app_watchdog_stop_feeding();
        return;
    }
    now_ms = (uint64_t)k_uptime_get();
    if (now_ms < pending.next_attempt_not_before_ms) {
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        schedule_ret = anchor_discovery_claim_reschedule_locked(
            pending.generation,
            pending.next_attempt_not_before_ms);
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        if (schedule_ret < 0 && schedule_ret != -ESTALE) {
            LOG_ERR("anchor discovery response early-work reschedule failed: phase=%u epoch=%u ret=%d",
                    pending.phase,
                    pending.epoch,
                    schedule_ret);
            app_watchdog_stop_feeding();
        }
        return;
    }
    if (pending.ack_delivered) {
        anchor_discovery_assignment_service_ack_promotion(&pending);
        return;
    }
    if (pending.delivery_handle != 0u) {
        bool have_event = false;
        bool next_hop_custody_terminal = false;

        /*
         * Delivery-event consumption and assignment supersession use the
         * transaction owner. The node-communication backend stays outside
         * the response-state mutex, while a newer TABLE cannot replace this
         * handle between terminal-event consumption and state publication.
         */
        k_mutex_lock(
            &anchor_discovery_assignment_transaction_mutex, K_FOREVER);
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        if (anchor_discovery_claim_pending.active &&
            anchor_discovery_claim_pending.generation ==
                pending.generation &&
            anchor_discovery_claim_pending.delivery_handle ==
                pending.delivery_handle) {
            have_event = true;
        }
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        if (have_event) {
            have_event = app_node_comm_take_delivery_event_for(
                pending.delivery_handle, &event);
            next_hop_custody_terminal =
                have_event &&
                event.reason == NODE_COMM_TERMINAL_DELIVERED &&
                pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
                anchor_discovery_response_terminal_matches(
                    pending.epoch,
                    pending.phase,
                    pending.command.session_id,
                    pending.command.seq,
                    &pending.table_commitment);
        }
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        if (!have_event) {
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                pending.generation,
                (uint64_t)k_uptime_get() +
                    ANCHOR_COMMAND_DELIVERY_POLL_MS);
            k_mutex_unlock(&anchor_discovery_claim_mutex);
            k_mutex_unlock(
                &anchor_discovery_assignment_transaction_mutex);
            if (schedule_ret < 0 && schedule_ret != -ESTALE) {
                LOG_ERR("anchor discovery response terminal poll reschedule failed: phase=%u epoch=%u handle=%u ret=%d",
                        pending.phase,
                        pending.epoch,
                        pending.delivery_handle,
                        schedule_ret);
                app_watchdog_stop_feeding();
            }
            return;
        }

        /*
         * Each node-communication handle is bounded. A CLAIM gets two fresh
         * handles so the finite route/timing/forwarded-ACK repair horizon
         * cannot outlive its exact local owner. TABLE ACK
         * custody keeps its existing durable retry policy. Supersession and
         * explicit cancellation always win the generation race.
         */
        retry = event.reason != NODE_COMM_TERMINAL_CANCELLED &&
                event.reason != NODE_COMM_TERMINAL_DELIVERED &&
                (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ||
                 (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
                  pending.terminal_retry_count <
                      DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES));
        if (retry) {
            low_duty_retry = pending.phase ==
                                 DISCOVERY_ASSIGNMENT_PHASE_ACK &&
                pending.terminal_retry_count >=
                    DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES;
            retry_ms = low_duty_retry ?
                anchor_discovery_ack_low_duty_retry_delay_ms(
                    pending.terminal_retry_count, sys_rand32_get()) :
                discovery_assignment_retry_backoff_ms(
                    pending.attempt, sys_rand32_get());
        }
        anchor_discovery_claim_pending.delivery_handle = 0u;
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED &&
            pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
            !next_hop_custody_terminal) {
            anchor_discovery_claim_pending.ack_delivered = true;
            anchor_discovery_claim_pending.next_attempt_not_before_ms =
                (uint64_t)k_uptime_get();
        } else if (next_hop_custody_terminal) {
            anchor_discovery_claim_pending.active = false;
        } else if (retry) {
            if (anchor_discovery_claim_pending.terminal_retry_count <
                UINT8_MAX) {
                anchor_discovery_claim_pending.terminal_retry_count++;
            }
            pending.terminal_retry_count =
                anchor_discovery_claim_pending.terminal_retry_count;
            if (anchor_discovery_claim_pending.attempt < UINT8_MAX) {
                anchor_discovery_claim_pending.attempt++;
            }
            attempt = anchor_discovery_claim_pending.attempt;
            retry_not_before_ms =
                (uint64_t)k_uptime_get() + retry_ms;
            anchor_discovery_terminal_retry_refresh_deadline_locked(
                retry_not_before_ms);
            if (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
                persistence_ret =
                    anchor_persist_discovery_assignment_ack_retry_round(
                        &pending,
                        anchor_discovery_claim_pending.terminal_retry_count);
            }
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                pending.generation,
                retry_not_before_ms);
            if (schedule_ret < 0) {
                anchor_discovery_claim_pending.active = false;
            }
        } else {
            anchor_discovery_claim_pending.active = false;
        }
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);

        if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_DELIVERED phase=%u epoch=%u attempts=%u\n",
                                pending.phase,
                                pending.epoch,
                                event.attempts_started);
            if (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
                !next_hop_custody_terminal) {
                pending.ack_delivered = true;
                anchor_discovery_assignment_service_ack_promotion(&pending);
            } else if (next_hop_custody_terminal) {
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_PROVISIONAL epoch=%u slot=%u state=WAIT_END\n",
                    pending.epoch,
                    pending.slot);
            }
            return;
        }
        if (retry && schedule_ret >= 0) {
            if (low_duty_retry) {
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_ACK_LOW_DUTY epoch=%u terminal_retries=%u delay=%u reason=%u state=ACK_PENDING\n",
                    pending.epoch,
                    pending.terminal_retry_count,
                    retry_ms,
                    (unsigned int)event.reason);
            } else {
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_RESPONSE_TERMINAL_RETRY phase=%u epoch=%u reason=%u delay=%u attempt=%u\n",
                    pending.phase,
                    pending.epoch,
                    (unsigned int)event.reason,
                    retry_ms,
                    attempt);
            }
            if (persistence_ret < 0) {
                LOG_ERR("anchor TABLE ACK retry-round persistence failed: epoch=%u round=%u ret=%d",
                        pending.epoch,
                        pending.terminal_retry_count,
                        persistence_ret);
            }
            return;
        }
        LOG_ERR("anchor discovery-slot response failed: phase=%u epoch=%u reason=%u attempts=%u",
                pending.phase,
                pending.epoch,
                (unsigned int)event.reason,
                event.attempts_started);
        status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_FAILED phase=%u epoch=%u attempts=%u reason=%u\n",
                            pending.phase,
                            pending.epoch,
                            event.attempts_started,
                            (unsigned int)event.reason);
        return;
    }

    if (!anchor_discovery_claim_ready_for_submit(
            pending.generation,
            pending.next_attempt_not_before_ms)) {
        return;
    }
    ret = anchor_send_discovery_response(&pending, &delivery_handle);
    if (ret == 0) {
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        if (anchor_discovery_claim_pending.active &&
            anchor_discovery_claim_pending.generation == pending.generation &&
            anchor_discovery_claim_pending.delivery_handle == 0u) {
            anchor_discovery_claim_pending.delivery_handle = delivery_handle;
            installed = true;
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                pending.generation,
                (uint64_t)k_uptime_get() +
                    ANCHOR_COMMAND_DELIVERY_POLL_MS);
            if (schedule_ret < 0) {
                /*
                 * Retain exact custody for watchdog recovery. Clearing this
                 * handle would orphan a live or terminal node-communication
                 * record precisely when no poll owner remains.
                 */
                app_watchdog_stop_feeding();
            }
        }
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        if (!installed && delivery_handle != 0u) {
            ret = app_node_comm_abandon_delivery(delivery_handle);
            if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
                uint32_t retained_handle;

                k_mutex_lock(
                    &anchor_discovery_claim_mutex, K_FOREVER);
                if (anchor_discovery_claim_failed_abandon_handle == 0u ||
                    anchor_discovery_claim_failed_abandon_handle ==
                        delivery_handle) {
                    anchor_discovery_claim_failed_abandon_handle =
                        delivery_handle;
                }
                retained_handle =
                    anchor_discovery_claim_failed_abandon_handle;
                k_mutex_unlock(&anchor_discovery_claim_mutex);
                LOG_ERR("anchor discovery stale response abandonment failed: handle=%u ret=%d retained=%u",
                        delivery_handle,
                        ret,
                        retained_handle);
                app_watchdog_stop_feeding();
            }
        }
        if (schedule_ret < 0) {
            LOG_ERR("anchor discovery-slot response poll scheduling failed: phase=%u epoch=%u ret=%d",
                    pending.phase,
                    pending.epoch,
                    schedule_ret);
        }
        return;
    }

    transient = ret == -EBUSY || ret == -EAGAIN || ret == -ENOSPC;
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.generation == pending.generation) {
        if ((transient ||
             pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) &&
            (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK ||
             (uint64_t)k_uptime_get() <
                 anchor_discovery_claim_pending.absolute_deadline_ms)) {
            retry_ms = discovery_assignment_retry_backoff_ms(
                anchor_discovery_claim_pending.attempt,
                sys_rand32_get());
            if (anchor_discovery_claim_pending.attempt < UINT8_MAX) {
                anchor_discovery_claim_pending.attempt++;
            }
            attempt = anchor_discovery_claim_pending.attempt;
            retry_not_before_ms =
                (uint64_t)k_uptime_get() + retry_ms;
            anchor_discovery_ack_refresh_attempt_deadline_locked(
                retry_not_before_ms);
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                pending.generation,
                retry_not_before_ms);
            retry = schedule_ret >= 0;
            if (!retry) {
                anchor_discovery_claim_pending.active = false;
            }
        } else {
            anchor_discovery_claim_pending.active = false;
        }
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    if (retry) {
        status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_ADMISSION_RETRY phase=%u epoch=%u ret=%d delay=%u attempt=%u\n",
                            pending.phase,
                            pending.epoch,
                            ret,
                            retry_ms,
                            attempt);
        return;
    }
    if (schedule_ret < 0) {
        LOG_ERR("anchor discovery-slot response retry scheduling failed: phase=%u epoch=%u ret=%d schedule_ret=%d",
                pending.phase,
                pending.epoch,
                    ret,
                    schedule_ret);
        return;
    }
    LOG_ERR("anchor discovery-slot response admission failed: phase=%u epoch=%u ret=%d transient=%u",
            pending.phase,
            pending.epoch,
            ret,
            transient ? 1u : 0u);
}

static int anchor_settle_ack_before_replacement_assignment(
    uint32_t incoming_epoch)
{
    struct anchor_discovery_claim_pending pending;
    struct node_comm_terminal_event event = {0};
    bool have_handle = false;
    bool delivered = false;
    int ret = 0;

    if (incoming_epoch == 0u) {
        return -EINVAL;
    }

    k_mutex_lock(
        &anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (!anchor_discovery_claim_pending.active ||
        anchor_discovery_claim_pending.phase !=
            DISCOVERY_ASSIGNMENT_PHASE_ACK ||
        incoming_epoch == anchor_discovery_claim_pending.epoch) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return 0;
    }
    pending = anchor_discovery_claim_pending;
    if (pending.ack_delivered) {
        delivered = true;
    } else if (pending.delivery_handle != 0u) {
        have_handle = true;
    } else {
        anchor_discovery_claim_pending.active = false;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    if (have_handle) {
        /*
         * The assignment transaction owner prevents both the ACK worker and
         * response replacement from consuming this terminal outcome. Keep
         * node-communication outside the response-state mutex to preserve
         * lock ordering.
         */
        ret = app_node_comm_cancel_delivery(pending.delivery_handle);
        if (ret < 0 && ret != -EALREADY) {
            k_mutex_unlock(
                &anchor_discovery_assignment_transaction_mutex);
            return ret;
        }
        if (!app_node_comm_take_delivery_event_for(
                pending.delivery_handle, &event)) {
            k_mutex_unlock(
                &anchor_discovery_assignment_transaction_mutex);
            return -EAGAIN;
        }
        delivered = event.reason == NODE_COMM_TERMINAL_DELIVERED;
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        if (anchor_discovery_claim_pending.active &&
            anchor_discovery_claim_pending.generation ==
                pending.generation &&
            anchor_discovery_claim_pending.delivery_handle ==
                pending.delivery_handle) {
            anchor_discovery_claim_pending.delivery_handle = 0u;
            anchor_discovery_claim_pending.ack_delivered = delivered;
            if (!delivered) {
                anchor_discovery_claim_pending.active = false;
            }
        }
        k_mutex_unlock(&anchor_discovery_claim_mutex);
    }

    if (!delivered) {
        ret = anchor_retire_superseded_discovery_ack(&pending);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return ret;
    }
    pending.ack_delivered = true;
    ret = anchor_promote_discovery_assignment_after_ack(&pending);
    if (ret < 0) {
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return ret;
    }
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.generation == pending.generation &&
        anchor_discovery_claim_pending.ack_delivered) {
        anchor_discovery_claim_pending.active = false;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    return 0;
}

static int anchor_clear_committed_assignment_for_new_claim(
    uint32_t incoming_epoch)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    int ret;

    if (incoming_epoch == 0u) {
        return -EINVAL;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0 || !snapshot.valid || snapshot.provisioned == 0u) {
        return ret;
    }
    if (incoming_epoch == snapshot.epoch) {
        return 0;
    }

    /*
     * CLAIM starts a clean-slate run. Keep the ordered epoch proof only as a
     * stale-packet barrier, but remove the old slot from both durable and live
     * assignment state before this run can send a response.
     */
    snapshot.slot = 0u;
    snapshot.slot_count = 0u;
    snapshot.provisioned = 0u;
    ret = anchor_save_discovery_assignment_semantic(&snapshot);
    if (ret < 0) {
        return ret;
    }
    ret = anchor_restore_live_discovery_assignment_snapshot(&snapshot);
    if (ret != PROTO_OK) {
        app_watchdog_stop_feeding();
        return -ESTALE;
    }
    status_debug_printf(
        "DBG_DISCOVERY_SLOT_CLEARED epoch=%u prior_epoch=%u\n",
        incoming_epoch,
        snapshot.epoch);
    return 0;
}

static int anchor_schedule_discovery_response(
    const struct proto_packet *command,
    uint32_t epoch,
    enum discovery_assignment_phase phase,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint8_t slot,
    uint8_t slot_count,
    uint8_t response_lane,
    uint8_t response_lane_count,
    uint16_t response_spread_ms,
    uint8_t restored_ack_retry_round,
    bool wait_for_route_before_admission)
{
    struct anchor_discovery_claim_pending replaced_pending;
    uint64_t hash;
    uint64_t not_before_ms;
    uint32_t replaced_delivery_handle;
    uint32_t generation;
    uint32_t delay_ms;
    uint8_t hop_count;
    bool matching_custody;
    int abandon_ret;
    int schedule_ret;

    if (command == NULL || epoch == 0u ||
        (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) ||
        (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         table_commitment != NULL) ||
        (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
         table_commitment == NULL) ||
        (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         restored_ack_retry_round != 0u) ||
        slot_count == 0u || slot >= slot_count ||
        response_lane_count == 0u ||
        response_lane >= response_lane_count ||
        response_spread_ms < DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        response_spread_ms > DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return -EINVAL;
    }
    k_mutex_lock(
        &anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    hop_count = anchor_discovery_gateway_hop_count();
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    matching_custody =
        discovery_assignment_response_custody_matches(
            anchor_discovery_claim_pending.active,
            anchor_discovery_claim_pending.epoch,
            anchor_discovery_claim_pending.phase,
            epoch,
            phase);

#if DEVICE_ROLE == ROLE_ANCHOR
    if (wait_for_route_before_admission &&
        phase == DISCOVERY_ASSIGNMENT_PHASE_ACK && hop_count == 0u &&
        !anchor_discovery_claim_pending.active) {
        anchor_discovery_ack_route_wait_pending = true;
        anchor_discovery_ack_liveness_generation = 0u;
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        schedule_ret = anchor_checked_work_reschedule(
            &anchor_discovery_ack_liveness_work,
            K_MSEC(ANCHOR_DISCOVERY_ACK_ROUTE_WAIT_RETRY_MS),
            "assignment-ack-route-wait");
        if (schedule_ret < 0) {
            k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
            anchor_discovery_ack_route_wait_pending = false;
            k_mutex_unlock(&anchor_discovery_claim_mutex);
        }
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_ACK_ROUTE_WAIT epoch=%u generation=%u slot=%u delay=%u ret=%d\n",
            epoch,
            command->session_id,
            slot,
            ANCHOR_DISCOVERY_ACK_ROUTE_WAIT_RETRY_MS,
            schedule_ret);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return schedule_ret < 0 ? schedule_ret : 0;
    }
#else
    ARG_UNUSED(wait_for_route_before_admission);
#endif

    if (matching_custody &&
        !app_discovery_assignment_response_identity_matches(
            phase,
            anchor_discovery_claim_pending.command.session_id,
            &anchor_discovery_claim_pending.table_commitment,
            command->session_id,
            table_commitment)) {
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return -ESTALE;
    }
    if (matching_custody &&
        !(phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
          anchor_discovery_claim_pending.delivery_handle == 0u &&
          !anchor_discovery_claim_pending.ack_delivered &&
          restored_ack_retry_round <
              anchor_discovery_claim_pending.terminal_retry_count)) {
        uint32_t retained_handle =
            anchor_discovery_claim_pending.delivery_handle;

        if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
            restored_ack_retry_round <
                anchor_discovery_claim_pending.terminal_retry_count) {
            anchor_discovery_claim_pending.terminal_retry_count =
                restored_ack_retry_round;
            anchor_discovery_claim_pending.attempt =
                restored_ack_retry_round;
        }
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_RETAIN phase=%u epoch=%u handle=%u\n",
                            phase,
                            epoch,
                            retained_handle);
        return 0;
    }
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.phase ==
            DISCOVERY_ASSIGNMENT_PHASE_ACK &&
        phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        epoch == anchor_discovery_claim_pending.epoch) {
        uint32_t retained_handle =
            anchor_discovery_claim_pending.delivery_handle;

        k_mutex_unlock(&anchor_discovery_claim_mutex);
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_RESPONSE_RETAIN phase=%u epoch=%u handle=%u owner=ack\n",
            DISCOVERY_ASSIGNMENT_PHASE_ACK,
            epoch,
            retained_handle);
        return 0;
    }
    replaced_pending = anchor_discovery_claim_pending;
    replaced_delivery_handle = replaced_pending.delivery_handle;
    generation = anchor_discovery_claim_next_generation(
        replaced_pending.generation);
    if (replaced_delivery_handle != 0u) {
        /*
         * Invalidate queued work before leaving the response mutex, but keep
         * the complete old owner available for rollback.  A replacement must
         * never forget an exact node-communication handle merely because its
         * cancellation path is temporarily unavailable.
         */
        anchor_discovery_claim_pending.active = false;
        anchor_discovery_claim_pending.delivery_handle = 0u;
        anchor_discovery_claim_pending.generation = generation;
        k_mutex_unlock(&anchor_discovery_claim_mutex);
        abandon_ret =
            app_node_comm_abandon_delivery(replaced_delivery_handle);
        k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
        if (abandon_ret < 0 && abandon_ret != -ENOENT &&
            abandon_ret != -EALREADY) {
            anchor_discovery_claim_pending = replaced_pending;
            schedule_ret = anchor_discovery_claim_reschedule_locked(
                replaced_pending.generation,
                (uint64_t)k_uptime_get() +
                    ANCHOR_COMMAND_DELIVERY_POLL_MS);
            k_mutex_unlock(&anchor_discovery_claim_mutex);
            k_mutex_unlock(
                &anchor_discovery_assignment_transaction_mutex);
            LOG_ERR("anchor discovery response replacement abandonment failed: handle=%u ret=%d reschedule_ret=%d",
                    replaced_delivery_handle,
                    abandon_ret,
                    schedule_ret);
            if (schedule_ret < 0) {
                app_watchdog_stop_feeding();
            }
            return abandon_ret;
        }
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    anchor_discovery_claim_pending.generation = generation;
    anchor_discovery_claim_pending.command = *command;
    anchor_discovery_claim_pending.epoch = epoch;
    memset(&anchor_discovery_claim_pending.table_commitment,
           0,
           sizeof(anchor_discovery_claim_pending.table_commitment));
    if (table_commitment != NULL) {
        anchor_discovery_claim_pending.table_commitment = *table_commitment;
    }
    anchor_discovery_claim_pending.delivery_handle = 0u;
    anchor_discovery_claim_pending.phase = phase;
    anchor_discovery_claim_pending.slot = slot;
    anchor_discovery_claim_pending.slot_count = slot_count;
    anchor_discovery_claim_pending.response_lane = response_lane;
    anchor_discovery_claim_pending.response_lane_count =
        response_lane_count;
    anchor_discovery_claim_pending.response_spread_ms = response_spread_ms;
    anchor_discovery_claim_pending.hop_count = hop_count;
    anchor_discovery_claim_pending.first_contact_origin_ms =
        (uint64_t)k_uptime_get();
    anchor_discovery_claim_pending.first_contact_random = sys_rand32_get();
    anchor_discovery_claim_pending.attempt = restored_ack_retry_round;
    anchor_discovery_claim_pending.terminal_retry_count =
        restored_ack_retry_round;
    anchor_discovery_claim_pending.ack_delivered = false;
    anchor_discovery_claim_pending.active = true;
    delay_ms =
        phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
        restored_ack_retry_round >=
            DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES ?
        anchor_discovery_ack_low_duty_retry_delay_ms(
            restored_ack_retry_round, sys_rand32_get()) :
        anchor_discovery_response_delay_ms(
            &anchor_discovery_claim_pending);
    not_before_ms =
        anchor_discovery_claim_pending.first_contact_origin_ms + delay_ms;
    anchor_discovery_claim_pending.next_attempt_not_before_ms = not_before_ms;
    anchor_discovery_claim_pending.absolute_deadline_ms =
        discovery_assignment_response_deadline_ms(
            anchor_discovery_claim_pending.first_contact_origin_ms,
            delay_ms,
            anchor_discovery_claim_pending.hop_count);
    generation = anchor_discovery_claim_pending.generation;
    hop_count = anchor_discovery_claim_pending.hop_count;
    schedule_ret = anchor_discovery_claim_reschedule_locked(
        generation,
        not_before_ms);
    if (schedule_ret < 0) {
        anchor_discovery_claim_pending.active = false;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    if (schedule_ret < 0) {
        LOG_ERR("anchor discovery-slot response scheduling failed: phase=%u epoch=%u ret=%d",
                phase,
                epoch,
                schedule_ret);
        app_watchdog_stop_feeding();
        k_mutex_unlock(
            &anchor_discovery_assignment_transaction_mutex);
        return schedule_ret;
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_SCHEDULED phase=%u epoch=%u delay=%u slot=%u lane=%u/%u hop=%u hash=0x%016llx\n",
                        phase,
                        epoch,
                        delay_ms,
                        slot,
                        response_lane,
                        response_lane_count,
                        hop_count,
                        (unsigned long long)hash);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    return 0;
}

static int anchor_schedule_discovery_claim(const struct proto_packet *command,
                                           uint32_t epoch,
                                           uint16_t expected_anchor_count,
                                           uint16_t response_spread_ms)
{
    uint8_t slot_count = expected_anchor_count == 0u ?
        UWB_DISCOVERY_SLOT_COUNT : (uint8_t)expected_anchor_count;
    uint8_t slot = (uint8_t)(discovery_assignment_hash(DEVICE_ID) %
                             slot_count);

    return anchor_schedule_discovery_response(command,
                                              epoch,
                                              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                              NULL,
                                              slot,
                                              slot_count,
                                              slot,
                                              slot_count,
                                              response_spread_ms,
                                              0u,
                                              false);
}

static int anchor_start_compact_enumeration_response_lane(
    const struct proto_packet *command,
    uint32_t epoch,
    uint8_t max_hop_count,
    uint32_t advertised_start_delay_ms)
{
    const struct route_candidate *selected;
    uint64_t now_ms;
    uint64_t start_ms;
    int64_t starts_in_ms;
    uint8_t hop_count;
    int ret;

    if (command == NULL || epoch == 0u || max_hop_count == 0u ||
        max_hop_count > UWB_ENUM_MAX_HOPS) {
        return -EINVAL;
    }
    selected = route_selected(&mesh_runtime.upstream);
    if (selected == NULL || selected->next_hop_id == 0u ||
        selected->hop_count == UINT8_MAX) {
        return -EHOSTUNREACH;
    }
    hop_count = (uint8_t)(selected->hop_count + 1u);
    if (hop_count == 0u || hop_count > max_hop_count) {
        status_debug_printf(
            "DBG_ENUM_COMPACT_REJECT epoch=%u hop=%u max=%u\n",
            epoch,
            hop_count,
            max_hop_count);
        return -E2BIG;
    }

    now_ms = (uint64_t)k_uptime_get();
    if (!enumeration_response_claim_start(
            now_ms,
            command->message_age_ms,
            advertised_start_delay_ms,
            &start_ms,
            &starts_in_ms)) {
        return -ESTALE;
    }
    if (enumeration_response_lane_complete_depth(
            start_ms, now_ms, max_hop_count)) {
        return -ESTALE;
    }

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_enumeration_response_config.active &&
        anchor_enumeration_response_config.epoch == epoch) {
        ret = anchor_enumeration_response_config.hop_count == hop_count &&
              anchor_enumeration_response_config.max_hop_count ==
                  max_hop_count &&
              anchor_enumeration_response_config.start_ms == start_ms &&
              anchor_enumeration_response_config.parent_id ==
                  selected->next_hop_id ?
            PROTO_OK : PROTO_ERR_STALE;
    } else {
        anchor_enumeration_response_config =
            (struct anchor_enumeration_response_config) {
                .start_ms = start_ms,
                .parent_id = selected->next_hop_id,
                .epoch = epoch,
                .hop_count = hop_count,
                .max_hop_count = max_hop_count,
                .active = true,
            };
        ret = PROTO_OK;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    status_debug_printf(
        "DBG_ENUM_COMPACT_BEGIN epoch=%u parent=0x%016llx hop=%u start=%llu starts_in=%lld age=%u duration=%u\n",
        epoch,
        (unsigned long long)selected->next_hop_id,
        hop_count,
        (unsigned long long)start_ms,
        (long long)starts_in_ms,
        command->message_age_ms,
        enumeration_response_duration_ms(max_hop_count));
    (void)anchor_uwb_scan_schedule_ms(
        enumeration_response_ms_until_lane(start_ms, now_ms));
    return 0;
}

static int anchor_survey_upstream(uint64_t *parent_id, uint8_t *hop_count)
{
    const struct route_candidate *selected;

    if (parent_id == NULL || hop_count == NULL) {
        return -EINVAL;
    }
    selected = route_selected(&mesh_runtime.upstream);
    if (selected == NULL || selected->next_hop_id == 0u ||
        selected->hop_count == UINT8_MAX || selected->hop_count == UINT8_MAX - 1u) {
        return -EHOSTUNREACH;
    }
    *parent_id = selected->next_hop_id;
    *hop_count = (uint8_t)(selected->hop_count + 1u);
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
static int anchor_survey_work_reschedule(struct k_work_delayable *work,
                                         uint32_t delay_ms)
{
    if (work == NULL) {
        return -EINVAL;
    }
    return k_work_reschedule_for_queue(&anchor_uwb_scan_work_q, work,
                                       K_MSEC(delay_ms));
}
#endif

static int anchor_schedule_late_discovery_claim(
    const struct proto_packet *table_command,
    uint32_t epoch,
    uint8_t table_entry_count,
    uint16_t response_spread_ms)
{
    struct proto_packet claim_command;

    if (table_command == NULL || epoch == 0u || table_entry_count == 0u ||
        table_entry_count > UWB_DISCOVERY_SLOT_COUNT) {
        return -EINVAL;
    }
    claim_command = *table_command;
    /*
     * This CLAIM requests a replay of the exact TABLE that exposed the missed
     * CLAIM phase. Keep the TABLE session and outer sequence so the gateway
     * can distinguish it from both an ordinary CLAIM and a stale recovery
     * request without guessing the gateway's private CLAIM sequence.
     */
    return anchor_schedule_discovery_claim(
        &claim_command, epoch, table_entry_count, response_spread_ms);
}

static int anchor_resume_pending_discovery_assignment_ack(
    bool explicit_table_replay)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    struct app_mesh_discovery_assignment_snapshot original_snapshot;
    int ret;

    ARG_UNUSED(explicit_table_replay);
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }
    ret = anchor_restore_discovery_assignment_durable();
    if (ret < 0) {
        return ret;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid || snapshot.pending_valid == 0u ||
        snapshot.ack_pending == 0u) {
        return 0;
    }
    if (snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID) {
        return -ESTALE;
    }
    original_snapshot = snapshot;
    ret = anchor_commit_discovery_table_locked(
        &snapshot,
        &original_snapshot,
        snapshot.pending_epoch,
        snapshot.pending_table_command_seq,
        &snapshot.pending_table_commitment,
        true,
        snapshot.pending_slot,
        snapshot.pending_slot_count);
    if (ret == 0) {
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_PENDING_MIGRATED state=PROVISIONED epoch=%u generation=%u slot=%u\n",
            snapshot.epoch,
            snapshot.table_command_seq,
            snapshot.slot);
        return 1;
    }
    return ret;
}

static void anchor_collection_result_clear(void)
{
    memset(&anchor_collection_result_pending, 0, sizeof(anchor_collection_result_pending));
}

static void anchor_collection_result_start_ram_owner(void)
{
    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    anchor_collection_result_clear();
    anchor_collection_result_seq = 0u;
    k_mutex_unlock(&anchor_command_result_mutex);
}

static bool anchor_collection_result_has_collection_identity(
    const struct anchor_collection_result_pending *pending)
{
    return pending != NULL &&
           pending->result_id.gateway_id != 0u &&
           pending->result_id.command_seq != 0u &&
           pending->result_id.node_id != 0u &&
           pending->result_id.node_boot_counter != 0u &&
           pending->result_id.result_seq != 0u;
}

static bool anchor_collection_result_take_terminal(
    uint32_t delivery_handle,
    struct node_comm_terminal_event *event_out,
    void *context)
{
    ARG_UNUSED(context);
    return app_node_comm_take_delivery_event_for(delivery_handle, event_out);
}

static int anchor_collection_result_apply_actions(
    const struct anchor_collection_result_pending *pending)
{
    uint8_t actions;
    int ret;

    if (pending == NULL) {
        return -EINVAL;
    }
    actions = anchor_command_completion_actions(
        pending->status,
        pending->force_rediscovery_after_result,
        pending->reboot_after_result);
    if ((actions &
         APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY) != 0u) {
        ret = anchor_force_rediscovery_from_command();
        if (ret < 0) {
            return ret;
        }
    }
    if ((actions & APP_ANCHOR_COMMAND_COMPLETION_REBOOT) != 0u) {
        ret = anchor_schedule_reboot_after_command_result();
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int anchor_collection_result_retry(const char *source)
{
    uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
        anchor_collection_result_pending.retry_round,
        sys_rand32_get());

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.retry_round < UINT8_MAX) {
        anchor_collection_result_pending.retry_round++;
    }
    return anchor_ram_owner_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(retry_ms),
        source);
}

static void anchor_collection_result_work_handler_locked(struct k_work *work)
{
    struct anchor_collection_result_pending pending;
    struct node_comm_terminal_event event;
    struct node_comm_terminal_event taken_event;
    const struct command_result_id *collection_result_id;
    uint32_t delivery_handle = 0u;
    int schedule_ret;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return;
    }

    if (anchor_collection_result_pending.result_confirmed) {
        pending = anchor_collection_result_pending;
        ret = anchor_collection_result_apply_actions(&pending);
        if (ret < 0) {
            LOG_ERR("anchor confirmed command-result action transfer deferred: cmd=0x%04x ret=%d round=%u",
                    (unsigned int)pending.command_id,
                    ret,
                    anchor_collection_result_pending.retry_round);
            (void)anchor_collection_result_retry(
                "command-result-action-transfer-retry");
            return;
        }
        anchor_collection_result_clear();
        LOG_INF("anchor confirmed command-result action ownership transferred: cmd=0x%04x",
                (unsigned int)pending.command_id);
        return;
    }

    ret = anchor_commit_pending_collection_command_replay();
    if (ret < 0) {
        LOG_WRN("anchor collection result replay commit retry: ret=%d round=%u",
                ret,
                anchor_collection_result_pending.retry_round);
        (void)anchor_collection_result_retry(
                "collection-result-replay-retry");
        return;
    }

    if (anchor_collection_result_pending.delivery_handle != 0u) {
        delivery_handle = anchor_collection_result_pending.delivery_handle;

        if (!app_node_comm_peek_delivery_event_for(delivery_handle, &event)) {
            (void)anchor_ram_owner_work_reschedule(
                &anchor_collection_result_work,
                K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
                "command-result-terminal-poll");
            return;
        }
        ret = app_anchor_command_completion_take_terminal_exact(
            delivery_handle,
            &event,
            anchor_collection_result_take_terminal,
            NULL,
            &taken_event);
        if (ret < 0) {
            if (ret == -EPROTO) {
                /*
                 * The exact terminal was consumed before its immutable
                 * metadata mismatch was detected. The RAM owner remains
                 * authoritative, but this handle can never become terminal
                 * again.
                 */
                anchor_collection_result_pending.delivery_handle = 0u;
            }
            LOG_WRN("anchor command result exact terminal take deferred: cmd=0x%04x handle=%u ret=%d",
                    (unsigned int)anchor_collection_result_pending.command_id,
                    delivery_handle,
                    ret);
            (void)anchor_collection_result_retry(
                "command-result-terminal-take-retry");
            return;
        }
        if (ret == 0) {
            LOG_WRN("anchor command result delivery failed; retaining exact RAM result: cmd=0x%04x handle=%u reason=%u attempts=%u",
                    (unsigned int)anchor_collection_result_pending.command_id,
                    delivery_handle,
                    (unsigned int)taken_event.reason,
                    taken_event.attempts_started);
            anchor_collection_result_pending.delivery_handle = 0u;
            (void)anchor_collection_result_retry(
                "command-result-terminal-resubmit");
            return;
        }

        pending = anchor_collection_result_pending;
        anchor_collection_result_pending.delivery_handle = 0u;
        anchor_collection_result_pending.result_confirmed = true;
        ret = anchor_collection_result_apply_actions(&pending);
        if (ret < 0) {
            LOG_ERR("anchor command result confirmed but action transfer failed: cmd=0x%04x handle=%u ret=%d",
                    (unsigned int)pending.command_id,
                    delivery_handle,
                    ret);
            (void)anchor_collection_result_retry(
                "command-result-action-transfer-retry");
            return;
        }
        anchor_collection_result_clear();
        LOG_INF("anchor command result gateway-confirmed and action transferred: cmd=0x%04x handle=%u attempts=%u",
                (unsigned int)pending.command_id,
                delivery_handle,
                taken_event.attempts_started);
        return;
    }

    pending = anchor_collection_result_pending;
    collection_result_id =
        anchor_collection_result_has_collection_identity(&pending) ?
            &pending.result_id : NULL;
    ret = anchor_submit_command_result(
        &pending.command,
        pending.command_id,
        pending.status,
        pending.reason,
        collection_result_id,
        pending.collection_epoch_id,
        NULL,
        &delivery_handle);
    if (ret < 0) {
        LOG_WRN("anchor collection command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)pending.command_id,
                pending.status,
                ret);
        (void)anchor_collection_result_retry("command-result-submit-retry");
        return;
    }
    anchor_collection_result_pending.delivery_handle = delivery_handle;
    anchor_collection_result_pending.retry_round = 0u;
    schedule_ret = anchor_ram_owner_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
        "command-result-terminal-owner");
    if (schedule_ret < 0) {
        LOG_ERR("anchor command result terminal polling admission failed: cmd=0x%04x handle=%u ret=%d",
                (unsigned int)pending.command_id,
                delivery_handle,
                schedule_ret);
        return;
    }
    LOG_INF("anchor command result submitted with RAM terminal custody: cmd=0x%04x status=%u reason=%u handle=%u",
            (unsigned int)pending.command_id,
            pending.status,
            pending.reason,
            delivery_handle);
}

static void anchor_collection_result_work_handler(struct k_work *work)
{
    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    anchor_collection_result_work_handler_locked(work);
    k_mutex_unlock(&anchor_command_result_mutex);
}

static int anchor_schedule_collection_command_result(
    const struct proto_packet *command,
    const struct gateway_command_options *options,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    bool force_rediscovery_after_result,
    bool reboot_after_result)
{
    uint32_t node_boot_counter = 0u;
    uint32_t now_ms;
    uint32_t due_ms;
    uint32_t delay_ms;
    int schedule_ret;
    int ret;

    if (command == NULL || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.active) {
        LOG_WRN("anchor collection command result already pending: cmd=0x%04x new=0x%04x",
                (unsigned int)anchor_collection_result_pending.command_id,
                (unsigned int)command_id);
        return -EBUSY;
    }
    ret = anchor_collection_node_boot_id(&node_boot_counter);
    if (ret < 0) {
        return ret;
    }

    now_ms = k_uptime_get_32();
    due_ms = gateway_command_collection_initial_due_ms(now_ms,
                                                       DEVICE_ID,
                                                       options->command_seq,
                                                       options->collection_slot_seed,
                                                       options->expected_node_count);
    delay_ms = due_ms - now_ms;
    if (delay_ms == 0u) {
        delay_ms = 1u;
    }

    memset(&anchor_collection_result_pending,
           0,
           sizeof(anchor_collection_result_pending));
    anchor_collection_result_pending.command = *command;
    anchor_collection_result_pending.collection_epoch_id = options->collection_epoch_id;
    anchor_collection_result_pending.result_id.gateway_id = GATEWAY_ID;
    anchor_collection_result_pending.result_id.gateway_epoch =
        (uint16_t)mesh_runtime.upstream.current_epoch;
    anchor_collection_result_pending.result_id.command_seq = options->command_seq;
    anchor_collection_result_pending.result_id.node_id = DEVICE_ID;
    anchor_collection_result_pending.result_id.node_boot_counter =
        node_boot_counter;
    anchor_collection_result_pending.result_id.result_seq =
        anchor_next_collection_result_seq();
    anchor_collection_result_pending.delivery_handle = 0u;
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result = reboot_after_result;
    anchor_collection_result_pending.active = true;
    schedule_ret = anchor_ram_owner_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(delay_ms),
        "collection-result-admission");
    if (schedule_ret < 0) {
        /*
         * Keep the exact RAM owner so an immediate replay can rearm it, but do
         * not report acceptance without a live work owner. The checked helper
         * also stops watchdog feeding if no replay arrives.
         */
        return schedule_ret;
    }

    LOG_INF("anchor collection command result scheduled: cmd=0x%04x due_ms=%u delay_ms=%u",
            (unsigned int)command_id,
            due_ms,
            delay_ms);
    return 0;
}

static bool anchor_direct_action_result_matches(
    const struct proto_packet *command,
    enum command_id command_id)
{
    return command != NULL &&
           anchor_collection_result_pending.active &&
           !anchor_collection_result_has_collection_identity(
               &anchor_collection_result_pending) &&
           anchor_collection_result_pending.command_id == command_id &&
           anchor_collection_result_pending.command.msg_type ==
               command->msg_type &&
           anchor_collection_result_pending.command.flags == command->flags &&
           anchor_collection_result_pending.command.src_id == command->src_id &&
           anchor_collection_result_pending.command.dst_id == command->dst_id &&
           anchor_collection_result_pending.command.session_id ==
               command->session_id &&
           anchor_collection_result_pending.command.seq == command->seq;
}

static int anchor_admit_direct_action_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    uint8_t completion_actions,
    const struct app_node_comm_reservation_lease *protocol_reservation)
{
    uint32_t delivery_handle = 0u;
    int schedule_ret;
    int ret;

    if (command == NULL || completion_actions == 0u ||
        protocol_reservation == NULL || protocol_reservation->token == 0u) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.active) {
        return -EBUSY;
    }

    memset(&anchor_collection_result_pending,
           0,
           sizeof(anchor_collection_result_pending));
    anchor_collection_result_pending.command = *command;
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        (completion_actions &
         APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY) != 0u;
    anchor_collection_result_pending.reboot_after_result =
        (completion_actions & APP_ANCHOR_COMMAND_COMPLETION_REBOOT) != 0u;
    anchor_collection_result_pending.active = true;

    ret = anchor_submit_command_result(
        command,
        command_id,
        status,
        reason,
        NULL,
        0u,
        protocol_reservation,
        &delivery_handle);
    if (ret < 0) {
        (void)app_node_comm_cancel_protocol_response_reservation(
            protocol_reservation);
        (void)anchor_collection_result_retry(
            "direct-command-result-submit-retry");
        /*
         * The RAM owner allows an exact replay to reconcile this command, but
         * failed transport admission is not safe acceptance.
         */
        return ret;
    }

    anchor_collection_result_pending.delivery_handle = delivery_handle;
    schedule_ret = anchor_ram_owner_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
        "direct-command-result-terminal-owner");
    return schedule_ret < 0 ? schedule_ret : 0;
}

static void anchor_reboot_work_handler(struct k_work *work)
{
    uint32_t now_ms;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_reboot_pending) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (mesh_relay_tx_active(&mesh_runtime) &&
        !uptime_deadline_reached(now_ms, anchor_reboot_deadline_ms)) {
        (void)anchor_ram_owner_work_reschedule(
            &anchor_reboot_work,
            K_MSEC(ANCHOR_REBOOT_RESULT_POLL_MS),
            "post-result-reboot-poll");
        return;
    }

    LOG_INF("anchor rebooting after command-result drain");
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static int anchor_schedule_reboot_after_command_result(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }

    anchor_reboot_pending = true;
    anchor_reboot_deadline_ms = k_uptime_get_32() + ANCHOR_REBOOT_RESULT_MAX_WAIT_MS;
    return anchor_ram_owner_work_reschedule(
        &anchor_reboot_work,
        K_MSEC(ANCHOR_REBOOT_RESULT_DRAIN_MS),
        "post-result-reboot");
}

static uint16_t anchor_next_heartbeat_seq(void)
{
    anchor_heartbeat_seq++;
    if (anchor_heartbeat_seq == 0u) {
        anchor_heartbeat_seq = 1u;
    }
    return anchor_heartbeat_seq;
}

static int anchor_send_heartbeat(void)
{
    struct mesh_outbound outbound = {0};
    uint64_t absolute_deadline_ms;
    uint64_t now_ms;
    uint32_t boot_incarnation = 0u;
    size_t payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

#if defined(CONFIG_IMEC_DURABLE_STATE)
    ret = app_durable_state_boot_incarnation(&boot_incarnation);
    if (ret < 0) {
        return ret;
    }
#else
    boot_incarnation = nonzero_uptime_session_id();
#endif

    ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
    if (ret == PROTO_OK) {
        ret = anchor_append_discovery_assignment_status(outbound.payload,
                                                        sizeof(outbound.payload),
                                                        &payload_len);
    }
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = report_init_anchor_heartbeat_packet(&outbound.packet,
                                              DEVICE_ID,
                                              GATEWAY_ID,
                                              boot_incarnation,
                                              anchor_next_heartbeat_seq(),
                                              (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint8_t)payload_len;
    now_ms = (uint64_t)k_uptime_get();
    absolute_deadline_ms = UINT64_MAX - now_ms <
                           ANCHOR_HEARTBEAT_DELIVERY_TIMEOUT_MS ?
                           UINT64_MAX :
                           now_ms + ANCHOR_HEARTBEAT_DELIVERY_TIMEOUT_MS;
    return app_node_comm_submit_best_effort_uplink(
        &outbound,
        absolute_deadline_ms,
        ((uint32_t)MSG_ANCHOR_HEARTBEAT << 16) | outbound.packet.seq);
}

static int anchor_heartbeat_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_heartbeat_enabled) {
        return -EALREADY;
    }
    return anchor_ram_owner_work_reschedule(
        &anchor_heartbeat_work,
        K_MSEC(delay_ms),
        "heartbeat");
}

static void anchor_heartbeat_request_startup(void)
{
    /* Heartbeats are operator-requested diagnostics, never boot traffic. */
}

static void anchor_heartbeat_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_heartbeat_enabled) {
        return;
    }
    if (anchor_uwb_window_active() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        (void)anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
        return;
    }

    ret = anchor_send_heartbeat();
    if (ret < 0) {
        LOG_WRN("anchor heartbeat TX failed: ret=%d interval_ms=%u",
                ret, anchor_heartbeat_interval_ms);
        (void)anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
        return;
    }

    LOG_INF("anchor heartbeat queued: interval_ms=%u seq=%u",
            anchor_heartbeat_interval_ms, anchor_heartbeat_seq);
    if (anchor_heartbeat_enabled) {
        (void)anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
    }
}

static int anchor_start_heartbeat_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_HEARTBEAT_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_HEARTBEAT_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_heartbeat_interval_ms = interval_ms;
    anchor_heartbeat_enabled = true;
    (void)anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
    return 0;
}

static void anchor_stop_heartbeat(void)
{
    anchor_heartbeat_enabled = false;
    (void)k_work_cancel_delayable(&anchor_heartbeat_work);
}

static int command_find_u8_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = tlv_value[0];
    return PROTO_OK;
}

static int command_find_u32_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u16_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u64_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

static int anchor_set_led_pattern_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint8_t pattern_id = 0u;
    int ret;

    ret = command_find_u8_tlv(payload, payload_len, TLV_LED_PATTERN_ID, &pattern_id);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }

    switch ((enum status_pattern)pattern_id) {
    case STATUS_PATTERN_OFF:
        status_leds_set(false, false, false);
        break;
    case STATUS_PATTERN_BLUE_PULSE:
    case STATUS_PATTERN_BLUE_CHASE:
        status_leds_set(false, false, true);
        break;
    case STATUS_PATTERN_GREEN_SOLID:
    case STATUS_PATTERN_GREEN_SLOW_BLINK:
        status_leds_set(false, true, false);
        break;
    case STATUS_PATTERN_AMBER_BLINK_ONCE:
    case STATUS_PATTERN_AMBER_SLOW_BLINK:
        status_leds_set(true, true, false);
        break;
    case STATUS_PATTERN_RED_BLINK_CODE:
        status_leds_set(true, false, false);
        break;
    default:
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    LOG_INF("anchor LED pattern command applied: pattern=%u", pattern_id);
    return 0;
}

static int anchor_set_route_from_command(const uint8_t *payload,
                                          size_t payload_len,
                                          uint8_t *reason)
{
    struct route_candidate candidate = {0};
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    bool epoch_changed = false;
    int ret;

    ret = command_find_u64_tlv(payload, payload_len, TLV_NEXT_HOP_ID, &candidate.next_hop_id);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, &candidate.route_epoch);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &hop_count);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_QUALITY, &quality);
    if (ret != PROTO_OK) {
        goto malformed;
    }

    candidate.gateway_id = GATEWAY_ID;
    candidate.hop_count = hop_count;
    candidate.link_quality = quality;
    candidate.last_seen_ms = k_uptime_get_32();
    candidate.valid = true;

    ret = mesh_relay_validate_configured_gateway_route(
        &mesh_runtime, &candidate, &epoch_changed);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return ret == PROTO_ERR_NO_SPACE ? -ENOSPC :
               ret == PROTO_ERR_STALE ? -ESTALE : -EINVAL;
    }
    ret = mesh_relay_upsert_configured_gateway_route(&mesh_runtime,
                                                     &candidate);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        if (epoch_changed) {
            /*
             * Mutation-free validation and serialized command execution make
             * this unreachable unless the runtime contract is broken. The
             * durable epoch is already newer, so reboot rather than continue
             * with a mixed live generation.
             */
            app_watchdog_stop_feeding();
            return -EIO;
        }
        return ret == PROTO_ERR_NO_SPACE ? -ENOSPC :
               ret == PROTO_ERR_STALE ? -ESTALE : -EINVAL;
    }
    mesh_relay_note_route_discovery_ready(&mesh_runtime, GATEWAY_ID);

    LOG_INF("anchor route command applied: next=0x%016llx epoch=%u hop_count=%u quality=%u",
            (unsigned long long)candidate.next_hop_id,
            candidate.route_epoch,
            hop_count,
            quality);
    return 0;

malformed:
    if (reason != NULL) {
        *reason = (uint8_t)(-ret);
    }
    return -EINVAL;
}

static void anchor_clear_route_from_command(void)
{
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    LOG_INF("anchor route command cleared upstream route state");
}

static int anchor_force_rediscovery_from_command(void)
{
    int ret;

    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    ret = app_node_comm_schedule_path_refresh(GATEWAY_ID,
                                              "forced-rediscovery");
    if (ret < 0) {
        LOG_WRN("anchor forced rediscovery request failed: ret=%d", ret);
        return ret;
    }
    LOG_INF("anchor forced rediscovery requested");
    return 0;
}

static int anchor_set_scan_duty_from_command(const uint8_t *payload,
                                              size_t payload_len,
                                              uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              anchor_uwb_scan_interval_ms,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_UWB_SCAN_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_UWB_SCAN_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_uwb_scan_interval_ms = interval_ms;
    if (!anchor_uwb_window_active()) {
        ret = anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
        if (ret < 0) {
            if (reason != NULL) {
                *reason = (uint8_t)(-PROTO_ERR_BUSY);
            }
            return ret;
        }
    }
    LOG_INF("anchor UWB scan duty command applied: interval_ms=%u min_ms=%u max_ms=%u awake_us=%u",
            anchor_uwb_scan_interval_ms,
            (unsigned int)ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
            ANCHOR_UWB_IDLE_SCAN_AWAKE_US);
    return 0;
}

static void anchor_pending_options_to_gateway(
    const struct anchor_pending_command_options *pending,
    struct gateway_command_options *options)
{
    memset(options, 0, sizeof(*options));
    options->scope = CMD_SCOPE_ALL_REGISTERED;
    options->response_mode = pending->response_mode;
    options->command_seq = pending->command_seq;
    options->collection_epoch_id = pending->collection_epoch_id;
    options->collection_slot_seed = pending->collection_slot_seed;
    options->command_expiry_s = pending->command_expiry_s;
    options->execute_delay_ms = pending->execute_delay_ms;
    options->expected_node_count = pending->expected_node_count;
    options->collection_required = pending->collection_required;
    options->flood_required = true;
}

static int anchor_restore_live_discovery_assignment_snapshot(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!snapshot->valid) {
        local_anchor_reset_discovery_assignment();
        return PROTO_OK;
    }
    return local_anchor_restore_discovery_assignment(
        snapshot->epoch,
        snapshot->table_command_seq,
        snapshot->epoch != 0u ? &snapshot->table_commitment : NULL,
        snapshot->slot,
        snapshot->slot_count,
        snapshot->provisioned != 0u,
        snapshot->ordered_epoch_valid != 0u,
        snapshot->retired_epochs,
        snapshot->retired_epoch_count,
        snapshot->pending_epoch,
        snapshot->pending_table_command_seq,
        snapshot->pending_valid != 0u ?
            &snapshot->pending_table_commitment : NULL,
        snapshot->pending_valid != 0u);
}

static void anchor_retire_discovery_response_for_terminal_locked(
    uint32_t epoch,
    const char *reason)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct anchor_discovery_claim_pending retired = {0};
    uint32_t handle = 0u;
    bool matched = false;
    bool retry_abandon = false;
    int ret;

    if (epoch == 0u) {
        return;
    }

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.epoch == epoch) {
        retired = anchor_discovery_claim_pending;
        handle = retired.delivery_handle;
        anchor_discovery_claim_pending.active = false;
        anchor_discovery_claim_pending.delivery_handle = 0u;
        anchor_discovery_claim_pending.generation =
            anchor_discovery_claim_next_generation(retired.generation);
        if (anchor_discovery_ack_liveness_generation == retired.generation) {
            anchor_discovery_ack_liveness_generation = 0u;
        }
        anchor_discovery_ack_route_wait_pending = false;
        matched = true;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);

    (void)k_work_cancel_delayable(&anchor_discovery_claim_work);
    (void)k_work_cancel_delayable(&anchor_discovery_ack_liveness_work);
    if (handle != 0u) {
        ret = app_node_comm_abandon_delivery(handle);
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
            if (anchor_discovery_claim_failed_abandon_handle == 0u ||
                anchor_discovery_claim_failed_abandon_handle == handle) {
                anchor_discovery_claim_failed_abandon_handle = handle;
            }
            k_mutex_unlock(&anchor_discovery_claim_mutex);
            LOG_ERR("anchor terminal enumeration response abandonment failed: epoch=%u handle=%u ret=%d",
                    epoch,
                    handle,
                    ret);
            retry_abandon = true;
            app_watchdog_stop_feeding();
        }
    }
    if (retry_abandon) {
        ret = anchor_checked_work_reschedule(
            &anchor_discovery_claim_work,
            K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
            "terminal-enumeration-response-abandon-retry");
        if (ret < 0) {
            LOG_ERR("anchor terminal enumeration response cleanup retry scheduling failed: epoch=%u handle=%u ret=%d",
                    epoch,
                    handle,
                    ret);
            app_watchdog_stop_feeding();
        }
    }
    if (matched) {
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_RESPONSE_RETIRED phase=%u epoch=%u handle=%u reason=%s\n",
            retired.phase,
            epoch,
            handle,
            reason == NULL ? "terminal" : reason);
    }
#else
    ARG_UNUSED(epoch);
    ARG_UNUSED(reason);
#endif
}

static bool anchor_discovery_pending_from_snapshot(
    const struct app_mesh_discovery_assignment_snapshot *snapshot,
    struct anchor_discovery_claim_pending *pending)
{
    if (snapshot == NULL || pending == NULL || !snapshot->valid ||
        snapshot->pending_valid == 0u ||
        snapshot->pending_epoch == 0u ||
        snapshot->pending_table_command_seq == 0u ||
        snapshot->table_packet_seq == 0u ||
        snapshot->pending_slot_count == 0u ||
        snapshot->pending_slot >= snapshot->pending_slot_count) {
        return false;
    }

    memset(pending, 0, sizeof(*pending));
    pending->command.msg_type = MSG_COMMAND;
    pending->command.src_id = GATEWAY_ID;
    pending->command.dst_id = MESH_BROADCAST_ID;
    pending->command.session_id = snapshot->pending_table_command_seq;
    pending->command.seq = snapshot->table_packet_seq;
    pending->epoch = snapshot->pending_epoch;
    pending->table_commitment = snapshot->pending_table_commitment;
    pending->response_spread_ms = snapshot->response_spread_ms;
    pending->phase = DISCOVERY_ASSIGNMENT_PHASE_ACK;
    pending->slot = snapshot->pending_slot;
    pending->slot_count = snapshot->pending_slot_count;
    pending->response_lane = snapshot->pending_response_lane;
    pending->response_lane_count =
        snapshot->pending_response_lane_count == 0u ?
            snapshot->pending_slot_count :
            snapshot->pending_response_lane_count;
    if (snapshot->pending_response_lane_count == 0u) {
        pending->response_lane = snapshot->pending_slot;
    }
    return pending->response_lane < pending->response_lane_count;
}

static int anchor_apply_discovery_assignment_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t ingress_hop_id)
{
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    struct app_operation_policy_candidate policy_candidate;
    struct operation_policy_set operation_policy;
    struct gateway_command_options assignment_options = {0};
    enum discovery_assignment_phase phase = 0;
    enum app_discovery_assignment_table_decision table_decision;
    uint32_t epoch = 0u;
    uint32_t response_lane_start_delay_ms = 0u;
    struct discovery_assignment_table_commitment table_commitment = {0};
    size_t entry_count = 0u;
    uint8_t slot_count = 0u;
    uint8_t response_lane_max_hop_count = 0u;
    int ret;

    ARG_UNUSED(ingress_hop_id);

    ret = app_operation_policy_prepare_payload(payload,
                                               payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
                                               &policy_candidate);
    if (ret < 0) {
        return ret;
    }
    operation_policy = policy_candidate.resolved;
    anchor_discovery_assignment_ram_only_iteration =
        operation_policy.assignment.ram_only_iteration;

    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    status_debug_printf(
        "DBG_DISCOVERY_POLICY phase=%u epoch=%u ram_only=%u\n",
        phase,
        epoch,
        anchor_discovery_assignment_ram_only_iteration ? 1u : 0u);
    ret = gateway_command_extract_options(payload,
                                          payload_len,
                                          &assignment_options);
    if (ret != PROTO_OK || !assignment_options.flood_required ||
        assignment_options.command_seq == 0u ||
        assignment_options.command_seq != packet->session_id ||
        assignment_options.flood_epoch_id == 0u) {
        return -EINVAL;
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE &&
        phase != DISCOVERY_ASSIGNMENT_PHASE_ABORT) {
        return -EINVAL;
    }
    /*
     * The preceding here-I-am activation establishes the response parent.
     * A later command copy may arrive through any sibling relay, so its
     * ingress is command-delivery evidence only and must not rewrite routing.
     */
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_ABORT) {
        struct discovery_assignment_abort_identity abort_identity = {0};
        struct anchor_discovery_claim_pending pending;

        ret = discovery_assignment_extract_abort_identity(
            payload, payload_len, &abort_identity);
        if (ret != PROTO_OK || abort_identity.epoch != epoch) {
            return ret == PROTO_OK ? -ESTALE : mesh_errno_from_proto(ret);
        }
        if (!anchor_enumeration_rx_matches_claim(
                epoch,
                abort_identity.claim_session_id,
                abort_identity.claim_command_seq)) {
            status_debug_printf(
                "DBG_ENUM_ABORT_REJECT epoch=%u claim_session=%u claim_seq=%u reason=identity\n",
                epoch,
                abort_identity.claim_session_id,
                abort_identity.claim_command_seq);
            return -ESTALE;
        }
        ret = anchor_restore_discovery_assignment_ram(&snapshot);
        if (ret < 0) {
            return ret;
        }
        if (snapshot.valid && snapshot.pending_valid != 0u &&
            snapshot.pending_epoch == epoch) {
            if (!anchor_discovery_pending_from_snapshot(&snapshot,
                                                        &pending)) {
                return -ESTALE;
            }
            ret = anchor_retire_superseded_discovery_ack(&pending);
            if (ret < 0) {
                return ret;
            }
        }
        if (!anchor_enumeration_rx_terminate_claim(
                epoch,
                abort_identity.claim_session_id,
                abort_identity.claim_command_seq,
                "abort")) {
            status_debug_printf(
                "DBG_ENUM_ABORT_REJECT epoch=%u claim_session=%u claim_seq=%u reason=identity\n",
                epoch,
                abort_identity.claim_session_id,
                abort_identity.claim_command_seq);
            return -ESTALE;
        }
        anchor_retire_discovery_response_for_terminal_locked(epoch, "abort");
        app_operation_policy_commit_prepared(&policy_candidate);
        status_debug_printf(
            "DBG_ENUM_ABORT_ACCEPT epoch=%u claim_session=%u claim_seq=%u\n",
            epoch,
            abort_identity.claim_session_id,
            abort_identity.claim_command_seq);
        return 0;
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        if (anchor_discovery_assignment_ram_only_iteration) {
            app_survey_anchor_clear_ram_roster();
        }
        ret = anchor_restore_discovery_assignment_ram(&snapshot);
        if (ret < 0) {
            return ret;
        }
        if (snapshot.valid &&
            (snapshot.local_id != DEVICE_ID ||
             snapshot.gateway_id != GATEWAY_ID)) {
            return -ESTALE;
        }
        if (anchor_discovery_response_terminal_matches(
                epoch,
                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                packet->session_id,
                packet->seq,
                NULL)) {
            status_debug_printf(
                "DBG_DISCOVERY_SLOT_RESPONSE_TERMINAL_REPLAY phase=%u epoch=%u generation=%u\n",
                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                epoch,
                packet->session_id);
            app_operation_policy_commit_prepared(&policy_candidate);
            return 0;
        }
        if (snapshot.valid && snapshot.pending_valid != 0u &&
            snapshot.ack_pending != 0u &&
            epoch == snapshot.pending_epoch) {
            ret = anchor_enumeration_rx_begin(
                epoch,
                operation_policy.assignment.operation_budget_ms,
                false);
            if (ret < 0) {
                return ret;
            }
            if (!anchor_enumeration_rx_bind_claim(
                    epoch,
                    packet->session_id,
                    assignment_options.command_seq)) {
                return -ESTALE;
            }
            ret = anchor_resume_pending_discovery_assignment_ack(false);
            if (ret < 0) {
                return ret;
            }
            status_debug_printf(
                "DBG_DISCOVERY_SLOT_CLAIM_DEFERRED epoch=%u pending_epoch=%u owner=ack\n",
                epoch,
                snapshot.pending_epoch);
            return 0;
        }
        if (snapshot.valid && snapshot.pending_valid != 0u &&
            snapshot.ack_pending != 0u &&
            epoch != snapshot.pending_epoch) {
            /*
             * An ACK from the prior enumeration cannot suppress a replacement
             * CLAIM. Drain its exact handle first: a queued DELIVERED
             * terminal still promotes the proven candidate, while every
             * other classified terminal retires only the obsolete pending
             * response and preserves any already committed slot before the
             * replacement CLAIM is scheduled.
             */
            ret = anchor_settle_ack_before_replacement_assignment(epoch);
            if (ret < 0) {
                return ret;
            }
        }
        ret = anchor_clear_committed_assignment_for_new_claim(epoch);
        if (ret < 0) {
            return ret;
        }
        enum app_discovery_assignment_claim_decision claim_decision =
            local_anchor_discovery_assignment_note_claim(epoch);

        if (claim_decision == APP_DISCOVERY_ASSIGNMENT_CLAIM_INVALID) {
            return -EINVAL;
        }
        if (claim_decision == APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE) {
            status_debug_printf("DBG_DISCOVERY_SLOT_STALE phase=claim epoch=%u state=%s\n",
                                epoch,
                                app_discovery_assignment_provisioning_state_name(
                                    local_anchor_discovery_assignment_provisioning_state()));
            LOG_WRN("anchor ignored stale discovery assignment claim: epoch=%u",
                    epoch);
            return 0;
        }
        ret = anchor_enumeration_rx_begin(
            epoch,
            operation_policy.assignment.operation_budget_ms,
            true);
        if (ret < 0) {
            return ret;
        }
        if (!anchor_enumeration_rx_bind_claim(
                epoch,
                packet->session_id,
                assignment_options.command_seq)) {
            return -ESTALE;
        }
        ret = command_find_u8_tlv(payload,
                                  payload_len,
                                  TLV_HOP_COUNT,
                                  &response_lane_max_hop_count);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        ret = command_find_u32_tlv(payload,
                                   payload_len,
                                   TLV_DISCOVERY_START_DELAY_MS,
                                   &response_lane_start_delay_ms);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        ret = anchor_start_compact_enumeration_response_lane(
            packet,
            epoch,
            response_lane_max_hop_count,
            response_lane_start_delay_ms);
        if (ret < 0) {
            return ret;
        }
        app_operation_policy_commit_prepared(&policy_candidate);
        return 0;
    }
    ret = discovery_assignment_parse_table_tlvs(payload,
                                                payload_len,
                                                entries,
                                                ARRAY_SIZE(entries),
                                                &entry_count,
                                                &slot_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (!discovery_assignment_table_commitment(
            entries, entry_count, slot_count, &table_commitment) ||
        packet->session_id == 0u) {
        return -EINVAL;
    }
    ret = anchor_restore_discovery_assignment_ram(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (snapshot.valid &&
        (snapshot.local_id != DEVICE_ID ||
         snapshot.gateway_id != GATEWAY_ID)) {
        return -ESTALE;
    }
    {
        struct app_mesh_discovery_assignment_snapshot original_snapshot =
            snapshot;
        bool listed = false;
        uint8_t assigned_slot = 0u;

        table_decision = local_anchor_discovery_assignment_preview_table(
            epoch, packet->session_id, &table_commitment);
        if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID) {
            return -EINVAL;
        }
        if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE) {
            status_debug_printf(
                "DBG_DISCOVERY_SLOT_STALE phase=table epoch=%u state=%s\n",
                epoch,
                app_discovery_assignment_provisioning_state_name(
                    local_anchor_discovery_assignment_provisioning_state()));
            return -ESTALE;
        }
        if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM &&
            local_anchor_discovery_assignment_note_claim(epoch) !=
                APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND) {
            return -ESTALE;
        }
        for (size_t i = 0u; i < entry_count; i++) {
            if (entries[i].anchor_id == DEVICE_ID) {
                listed = true;
                assigned_slot = entries[i].slot;
                break;
            }
        }

        if (anchor_discovery_assignment_ram_only_iteration) {
            ret = app_survey_anchor_note_ram_roster(
                entries, entry_count, slot_count, epoch, packet->session_id,
                &table_commitment);
            if (ret < 0) {
                return ret;
            }
        }

        if (table_decision != APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY) {
            table_decision = local_anchor_discovery_assignment_note_table(
                epoch, packet->session_id, &table_commitment);
            if (table_decision != APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY) {
                (void)anchor_restore_live_discovery_assignment_snapshot(
                    &original_snapshot);
                return -ESTALE;
            }
            ret = anchor_commit_discovery_table_locked(
                &snapshot,
                &original_snapshot,
                epoch,
                packet->session_id,
                &table_commitment,
                listed,
                assigned_slot,
                slot_count);
            if (ret < 0) {
                return ret;
            }
        }

        ret = anchor_enumeration_rx_begin_table(
            epoch,
            operation_policy.assignment.operation_budget_ms,
            packet->session_id,
            &table_commitment,
            table_decision != APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
        if (ret < 0 || !anchor_enumeration_rx_finish_table(
                epoch,
                packet->session_id,
                &table_commitment,
                "table-committed")) {
            return ret < 0 ? ret : -ESTALE;
        }
        anchor_retire_discovery_response_for_terminal_locked(
            epoch, "table-committed");
        app_operation_policy_commit_prepared(&policy_candidate);
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_TABLE_ACCEPT epoch=%u table_seq=%u listed=%u slot=%u anchors=%u replay=%u\n",
            epoch,
            packet->session_id,
            listed ? 1u : 0u,
            listed ? assigned_slot : 0u,
            (unsigned int)entry_count,
            table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY ? 1u : 0u);
        return 0;
    }
}

static int anchor_apply_discovery_assignment_command_serialized(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t ingress_hop_id)
{
    int ret;

    k_mutex_lock(&anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    ret = anchor_apply_discovery_assignment_command(
        packet, payload, payload_len, ingress_hop_id);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    return ret;
}

static int anchor_execute_command_side_effects(const struct proto_packet *packet,
                                               const uint8_t *payload,
                                               size_t payload_len,
                                               enum command_id command_id,
                                               uint32_t execution_deadline_ms,
                                               enum command_status *status,
                                               uint8_t *reason,
                                               bool *force_rediscovery_after_result,
                                               bool *reboot_after_result,
                                               uint64_t ingress_hop_id)
{
    enum device_role requested_role = ROLE_ANCHOR;
    int ret = 0;

    ARG_UNUSED(execution_deadline_ms);

    if (command_id == CMD_REBOOT) {
        *reboot_after_result = true;
    } else if (command_id == CMD_SET_ROLE) {
        ret = gateway_command_extract_role(payload, payload_len, &requested_role);
        if (ret != PROTO_OK) {
            *status = COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        } else if ((uint8_t)requested_role != (uint8_t)DEVICE_ROLE) {
            *status = COMMAND_DENIED;
            *reason = 1u;
        }
    } else if (command_id == CMD_START_HEARTBEAT) {
        ret = anchor_start_heartbeat_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_STOP_HEARTBEAT) {
        anchor_stop_heartbeat();
    } else if (command_id == CMD_SET_LED_PATTERN) {
        ret = anchor_set_led_pattern_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SET_ROUTE) {
        ret = anchor_set_route_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = ret == -ENOSPC ? COMMAND_BUSY :
                      ret == -ESTALE ? COMMAND_INVALID_STATE :
                      ret == -EINVAL ? COMMAND_MALFORMED_PAYLOAD :
                      COMMAND_INTERNAL_ERROR;
        }
    } else if (command_id == CMD_CLEAR_ROUTE) {
        anchor_clear_route_from_command();
    } else if (command_id == CMD_FORCE_REDISCOVERY) {
        *force_rediscovery_after_result = true;
    } else if (command_id == CMD_SET_SCAN_DUTY) {
        ret = anchor_set_scan_duty_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_ASSIGN_DISCOVERY_SLOTS) {
        ret = anchor_apply_discovery_assignment_command_serialized(
            packet, payload, payload_len, ingress_hop_id);
        if (ret < 0) {
            *status = ret == -ESTALE ? COMMAND_INVALID_STATE :
                      COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_START ||
               command_id == CMD_SURVEY_PLAN ||
               command_id == CMD_SURVEY_CANCEL) {
        struct survey_control control;

        ret = survey_control_extract_tlvs(payload, payload_len, &control);
        if (ret == PROTO_OK) {
            ret = app_survey_anchor_apply_control(packet, &control);
        } else {
            ret = mesh_errno_from_proto(ret);
        }
        if (ret < 0) {
            *status = ret == -ESTALE || ret == -EHOSTUNREACH ?
                COMMAND_INVALID_STATE : COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        *status = COMMAND_UNSUPPORTED_COMMAND;
        *reason = 1u;
    }
    return ret;
}

static int anchor_finish_broadcast_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_id command_id,
    const struct gateway_command_options *command_options,
    uint32_t execution_deadline_ms,
    uint64_t ingress_hop_id)
{
    enum command_status status = COMMAND_OK;
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    uint8_t reason = 0u;
    int ret;

    ret = anchor_execute_command_side_effects(packet,
                                              payload,
                                              payload_len,
                                              command_id,
                                              execution_deadline_ms,
                                              &status,
                                              &reason,
                                              &force_rediscovery_after_result,
                                              &reboot_after_result,
                                              ingress_hop_id);
    if (command_options->response_mode == CMD_RESPONSE_NONE) {
        LOG_INF("anchor broadcast command handled without result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            ret = anchor_force_rediscovery_from_command();
            if (ret < 0) {
                return ret;
            }
        }
        if (reboot_after_result && status == COMMAND_OK) {
            ret = anchor_schedule_reboot_after_command_result();
            if (ret < 0) {
                return ret;
            }
        }
        return 0;
    }

    ret = anchor_schedule_collection_command_result(packet,
                                                    command_options,
                                                    command_id,
                                                    status,
                                                    reason,
                                                    force_rediscovery_after_result,
                                                    reboot_after_result);
    if (ret < 0) {
        LOG_WRN("anchor collection command result schedule failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
    } else {
        LOG_INF("anchor broadcast command handled with collection result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
    }
    return ret;
}

static int anchor_command_execute_reschedule(uint32_t delay_ms);

static uint32_t anchor_broadcast_command_schedule_delay_ms(
    enum command_id command_id,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t execution_remaining_ms)
{
    ARG_UNUSED(command_id);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return execution_remaining_ms;
}

static int anchor_schedule_broadcast_command_execution(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_id command_id,
    const struct gateway_command_options *options,
    uint32_t delay_ms)
{
    uint32_t schedule_delay_ms;
    int ret;

    if (packet == NULL || payload == NULL || options == NULL ||
        payload_len > sizeof(anchor_pending_command_execution.payload)) {
        return -EINVAL;
    }
    schedule_delay_ms = anchor_broadcast_command_schedule_delay_ms(
        command_id, payload, payload_len, delay_ms);
    if (anchor_pending_command_execution.active) {
        return -EBUSY;
    }

    anchor_pending_command_execution.command = *packet;
    anchor_pending_command_execution.options.response_mode = options->response_mode;
    anchor_pending_command_execution.options.command_seq = options->command_seq;
    anchor_pending_command_execution.options.collection_epoch_id = options->collection_epoch_id;
    anchor_pending_command_execution.options.collection_slot_seed = options->collection_slot_seed;
    anchor_pending_command_execution.options.command_expiry_s = options->command_expiry_s;
    anchor_pending_command_execution.options.execute_delay_ms = options->execute_delay_ms;
    anchor_pending_command_execution.options.expected_node_count = options->expected_node_count;
    anchor_pending_command_execution.options.collection_required = options->collection_required;
    memcpy(anchor_pending_command_execution.payload, payload, payload_len);
    anchor_pending_command_execution.payload_len = (uint16_t)payload_len;
    anchor_pending_command_execution.received_at_ms = k_uptime_get_32();
    anchor_pending_command_execution.execute_at_ms =
        anchor_pending_command_execution.received_at_ms + schedule_delay_ms;
    anchor_pending_command_execution.execute_deadline_ms =
        anchor_pending_command_execution.received_at_ms + delay_ms;
    anchor_pending_command_execution.active = true;
    ret = mesh_route_work_reschedule(
        &anchor_command_execute_work,
        schedule_delay_ms == 0u ? 1u : schedule_delay_ms);
    if (ret < 0) {
        memset(&anchor_pending_command_execution,
               0,
               sizeof(anchor_pending_command_execution));
        return ret;
    }
    LOG_INF("anchor broadcast command execution scheduled: command_seq=%u delay_ms=%u execute_remaining_ms=%u",
            options->command_seq,
            schedule_delay_ms,
            delay_ms);
    return 0;
}

static bool anchor_pending_command_execution_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_command_options *options)
{
    return packet != NULL && payload != NULL && options != NULL &&
           anchor_pending_command_execution.active &&
           anchor_pending_command_execution.options.command_seq ==
               options->command_seq &&
           anchor_pending_command_execution.command.src_id == packet->src_id &&
           anchor_pending_command_execution.command.dst_id == packet->dst_id &&
           anchor_pending_command_execution.command.session_id ==
               packet->session_id &&
           anchor_pending_command_execution.command.seq == packet->seq &&
           anchor_pending_command_execution.payload_len == payload_len &&
           memcmp(anchor_pending_command_execution.payload,
                  payload,
                  payload_len) == 0;
}

static bool anchor_collection_result_matches_broadcast_command(
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    enum command_id command_id)
{
    return packet != NULL && options != NULL &&
           anchor_collection_result_pending.active &&
           anchor_collection_result_pending.command.dst_id ==
               MESH_BROADCAST_ID &&
           anchor_collection_result_pending.command_id == command_id &&
           anchor_collection_result_pending.result_id.command_seq ==
               options->command_seq &&
           anchor_collection_result_pending.collection_epoch_id ==
               options->collection_epoch_id &&
           anchor_collection_result_pending.command.src_id == packet->src_id &&
           anchor_collection_result_pending.command.session_id ==
               packet->session_id &&
           anchor_collection_result_pending.command.seq == packet->seq;
}

static int anchor_reconcile_pending_command_result(void)
{
    int ret;

    if (!anchor_collection_result_pending.active) {
        return -ENOENT;
    }
    ret = anchor_ram_owner_work_reschedule(
        &anchor_collection_result_work,
        K_NO_WAIT,
        "pending-command-result-reconcile");
    if (ret < 0) {
        return ret;
    }
    /*
     * Publish replay only after the RAM owner has a live work handoff. If the
     * replay commit fails, the scheduled owner retries the exact same commit.
     */
    return anchor_commit_pending_collection_command_replay();
}

static bool anchor_broadcast_command_targets_local(
    enum command_id command_id,
    const struct gateway_command_options *options)
{
    uint32_t assignment_epoch = 0u;
    uint8_t assignment_slot = 0u;
    uint8_t assignment_count = 0u;
    bool provisioned;

    if (command_id == CMD_ASSIGN_DISCOVERY_SLOTS) {
        return true;
    }
    provisioned = local_anchor_discovery_assignment_get(
        &assignment_epoch, &assignment_slot, &assignment_count);
    return gateway_command_applies_to_node(options,
                                           DEVICE_ID,
                                           provisioned,
                                           assignment_epoch);
}

static int anchor_command_execute_reschedule(uint32_t delay_ms)
{
    int ret = mesh_route_work_reschedule(
        &anchor_command_execute_work, delay_ms);

    if (ret == -ESHUTDOWN) {
        /*
         * A communication pause closes ordinary admission, but the immutable
         * delayed-command owner must retain a liveness clock. The dedicated
         * route-owner queue remains available and the worker will keep the
         * side effect gated/retryable until communication resumes.
         */
        ret = mesh_route_owner_work_reschedule(
            &anchor_command_execute_work, delay_ms);
    }
    if (ret < 0) {
        /*
         * There is no second scheduler owner for this volatile transaction.
         * Preserve the owner for diagnostics and force bounded reset recovery
         * instead of silently leaving an active command inert forever.
         */
        LOG_ERR("anchor delayed command liveness scheduling failed: ret=%d",
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static void anchor_command_execute_work_handler_locked(struct k_work *work)
{
    struct anchor_pending_command_execution pending;
    struct gateway_command_options options = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    uint32_t now_ms;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_pending_command_execution.active) {
        return;
    }

    pending = anchor_pending_command_execution;

    now_ms = k_uptime_get_32();
    packet_age_add_elapsed(&pending.command, now_ms - pending.received_at_ms);
    anchor_pending_options_to_gateway(&pending.options, &options);

    if (pending.side_effect_completed ||
        pending.terminal_without_execution) {
        ret = anchor_commit_broadcast_command_replay(
            &pending.command, &options, now_ms);
        if (ret == 0) {
            memset(&anchor_pending_command_execution,
                   0,
                   sizeof(anchor_pending_command_execution));
            return;
        }
        LOG_WRN("anchor delayed side-effect replay commit deferred: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        (void)anchor_command_execute_reschedule(
            COLLECTION_RETRY_ROUND_0_MS);
        return;
    }

    if (gateway_command_receive_expired(&pending.command, &options)) {
        LOG_WRN("anchor delayed broadcast command expired: command_seq=%u age_ms=%u expiry_s=%u",
                options.command_seq,
                pending.command.message_age_ms,
                options.command_expiry_s);
        ret = gateway_command_extract_id(
            pending.payload, pending.payload_len, &command_id);
        if (ret == PROTO_OK && options.collection_required &&
            !anchor_collection_result_pending.active) {
            ret = anchor_schedule_collection_command_result(
                &pending.command,
                &options,
                command_id,
                COMMAND_INVALID_STATE,
                ETIMEDOUT,
                false,
                false);
            if (ret == 0 ||
                anchor_collection_result_matches_broadcast_command(
                    &pending.command, &options, command_id)) {
                memset(&anchor_pending_command_execution,
                       0,
                       sizeof(anchor_pending_command_execution));
                return;
            }
        } else if (!options.collection_required) {
            ret = anchor_commit_broadcast_command_replay(
                &pending.command, &options, now_ms);
            if (ret == 0) {
                memset(&anchor_pending_command_execution,
                       0,
                       sizeof(anchor_pending_command_execution));
                return;
            }
        }
        LOG_WRN("anchor delayed expiry settlement deferred: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        (void)anchor_command_execute_reschedule(
            COLLECTION_RETRY_ROUND_0_MS);
        return;
    }

    ret = gateway_command_extract_id(
        pending.payload, pending.payload_len, &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("anchor dropped malformed delayed broadcast command: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        memset(&anchor_pending_command_execution,
               0,
               sizeof(anchor_pending_command_execution));
        return;
    }

    if (options.collection_required &&
        anchor_collection_result_pending.active) {
        (void)anchor_command_execute_reschedule(
            COLLECTION_RETRY_ROUND_0_MS);
        return;
    }

    if (!options.collection_required) {
        ret = anchor_commit_broadcast_command_replay(
            &pending.command, &options, now_ms);
        if (ret < 0) {
            LOG_WRN("anchor delayed broadcast replay commit failed: command_seq=%u ret=%d",
                    options.command_seq,
                    ret);
            (void)anchor_command_execute_reschedule(
                COLLECTION_RETRY_ROUND_0_MS);
            return;
        }
    }

    ret = anchor_finish_broadcast_command(&pending.command,
                                          pending.payload,
                                          pending.payload_len,
                                          command_id,
                                          &options,
                                          pending.execute_deadline_ms,
                                          0u);
    if (ret < 0) {
        if (options.collection_required &&
            anchor_collection_result_matches_broadcast_command(
                &pending.command, &options, command_id)) {
            /*
             * The exact RAM result owner has already been installed and will
             * retry its work handoff or force watchdog recovery. Retaining the
             * execution owner would run the side effect a second time after
             * the result slot drains.
             */
            memset(&anchor_pending_command_execution,
                   0,
                   sizeof(anchor_pending_command_execution));
            return;
        }
        int schedule_ret = anchor_command_execute_reschedule(
            COLLECTION_RETRY_ROUND_0_MS);

        if (schedule_ret < 0) {
            LOG_WRN("anchor delayed command retry admission failed: command_seq=%u ret=%d",
                    options.command_seq,
                    schedule_ret);
        }
        return;
    }
    if (options.collection_required) {
        /*
         * The live collection-result owner retries this replay commit if it
         * fails here, so execution ownership may retire safely.
         */
        ret = anchor_commit_broadcast_command_replay(
            &pending.command, &options, now_ms);
        if (ret < 0) {
            LOG_WRN("anchor delayed result replay commit deferred to result owner: command_seq=%u ret=%d",
                    options.command_seq,
                    ret);
        }
    }
    memset(&anchor_pending_command_execution,
           0,
           sizeof(anchor_pending_command_execution));
}

static void anchor_command_execute_work_handler(struct k_work *work)
{
    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    anchor_command_execute_work_handler_locked(work);
    k_mutex_unlock(&anchor_command_result_mutex);
}

/*
 * Command session, transport sequence, and command ID are the immutable
 * semantic origin of one local result. A redrive of that command therefore
 * coalesces only with its own short-lived reservation, while a successor has
 * a distinct owner generation even before it receives a delivery handle.
 */
static uint64_t anchor_command_result_owner_generation(
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint64_t generation;

    if (command == NULL) {
        return 0u;
    }
    generation = ((uint64_t)command->session_id << 32) |
                 ((uint64_t)command->seq << 16) |
                 (uint16_t)command_id;
    return generation == 0u ? 1u : generation;
}

static int anchor_handle_local_command_locked(const struct proto_packet *packet,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              uint64_t ingress_hop_id)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    struct gateway_command_options command_options = {0};
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    bool broadcast_command;
    bool expired;
    bool duplicate;
    struct app_node_comm_reservation_lease protocol_reservation = {0};
    uint32_t delay_ms;
    uint32_t now_ms;
    uint8_t completion_actions;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND) {
        return 0;
    }
    broadcast_command = packet->dst_id == MESH_BROADCAST_ID;
    if (packet->dst_id != DEVICE_ID && !broadcast_command) {
        return 0;
    }

    now_ms = k_uptime_get_32();
    ret = app_mesh_command_orchestrator_anchor_receive(
        mesh_gateway_command_orchestrator_context(),
        packet,
        payload,
        payload_len,
        GATEWAY_ID,
        now_ms,
        &command_id,
        &command_options,
        &broadcast_command,
        &expired,
        &duplicate);
    if (ret != PROTO_OK) {
        if (broadcast_command) {
            LOG_WRN("anchor ignored invalid broadcast command: ret=%d", ret);
            return 0;
        }
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(-ret);
    }

    if (broadcast_command) {
        if (expired) {
            LOG_WRN("anchor ignored expired broadcast command: cmd=0x%04x command_seq=%u age_ms=%u expiry_s=%u delay_ms=%u",
                    (unsigned int)command_id,
                    command_options.command_seq,
                    packet->message_age_ms,
                    command_options.command_expiry_s,
                    command_options.execute_delay_ms);
            ret = anchor_commit_broadcast_command_replay(
                packet, &command_options, now_ms);
            return ret == 0 ? 1 : 0;
        }
        if (!anchor_broadcast_command_targets_local(
                command_id, &command_options)) {
            ret = anchor_commit_broadcast_command_replay(
                packet, &command_options, now_ms);
            if (ret < 0) {
                LOG_WRN("anchor excluded-command replay commit failed: command_seq=%u ret=%d",
                        command_options.command_seq,
                        ret);
            }
            return ret == 0 ? 1 : 0;
        }
        if (duplicate) {
            if (command_id == CMD_ASSIGN_DISCOVERY_SLOTS) {
                ret = anchor_apply_discovery_assignment_command_serialized(
                    packet, payload, payload_len, ingress_hop_id);
                if (ret < 0) {
                    LOG_WRN("anchor rejected duplicate discovery assignment command: command_seq=%u ret=%d",
                            command_options.command_seq,
                            ret);
                }
                return ret < 0 ? 0 : 1;
            }
            if (command_id == CMD_SURVEY_START ||
                command_id == CMD_SURVEY_PLAN ||
                command_id == CMD_SURVEY_CANCEL) {
                struct survey_control control;

                ret = survey_control_extract_tlvs(payload, payload_len,
                                                  &control);
                if (ret == PROTO_OK) {
                    ret = app_survey_anchor_apply_control(packet, &control);
                }
                return ret == PROTO_OK || ret == 0 ? 1 : 0;
            }
            LOG_INF("anchor ignored duplicate broadcast command: cmd=0x%04x command_seq=%u",
                    (unsigned int)command_id,
                    command_options.command_seq);
            return 1;
        }

        if (command_options.collection_required &&
            anchor_collection_result_pending.active) {
            if (anchor_collection_result_matches_broadcast_command(
                    packet, &command_options, command_id)) {
                ret = anchor_reconcile_pending_command_result();
                return ret == 0 ? 1 : 0;
            }
            LOG_WRN("anchor deferred broadcast command for pending collection result: command_seq=%u pending_seq=%u",
                    command_options.command_seq,
                    anchor_collection_result_pending.result_id.command_seq);
            return 0;
        }

        if (command_options.collection_required &&
            anchor_pending_command_execution.active) {
            if (anchor_pending_command_execution_matches(
                    packet, payload, payload_len, &command_options)) {
                return 0;
            }
            LOG_WRN("anchor deferred collection command for accepted delayed command: command_seq=%u pending_seq=%u",
                    command_options.command_seq,
                    anchor_pending_command_execution.options.command_seq);
            return 0;
        }

        delay_ms = gateway_command_execute_delay_remaining_ms(packet, &command_options);
        if (delay_ms != 0u) {
            ret = anchor_schedule_broadcast_command_execution(packet,
                                                              payload,
                                                              payload_len,
                                                              command_id,
                                                              &command_options,
                                                              delay_ms);
            if (ret < 0) {
                if (ret == -EBUSY && command_options.collection_required &&
                    !anchor_collection_result_pending.active) {
                    ret = anchor_schedule_collection_command_result(
                        packet,
                        &command_options,
                        command_id,
                        COMMAND_BUSY,
                        EBUSY,
                        false,
                        false);
                    if (ret == 0) {
                        ret = anchor_commit_broadcast_command_replay(
                            packet, &command_options, now_ms);
                    }
                }
                LOG_WRN("anchor delayed broadcast command schedule failed: cmd=0x%04x command_seq=%u ret=%d",
                        (unsigned int)command_id,
                        command_options.command_seq,
                        ret);
                return ret == 0 ? 1 : 0;
            }
            return 0;
        }

        if (!command_options.collection_required &&
            command_id != CMD_REBOOT &&
            command_id != CMD_FORCE_REDISCOVERY) {
            ret = anchor_commit_broadcast_command_replay(
                packet, &command_options, now_ms);
            if (ret < 0) {
                LOG_WRN("anchor broadcast replay commit failed before execution: command_seq=%u ret=%d",
                        command_options.command_seq,
                        ret);
                return 0;
            }
        }
        ret = anchor_finish_broadcast_command(packet,
                                              payload,
                                              payload_len,
                                              command_id,
                                              &command_options,
                                              0u,
                                              ingress_hop_id);
        if (ret == 0 &&
            (command_options.collection_required ||
             command_id == CMD_REBOOT ||
             command_id == CMD_FORCE_REDISCOVERY)) {
            ret = anchor_commit_broadcast_command_replay(
                packet, &command_options, now_ms);
        }
        return ret == 0 ? 1 : 0;
    }

    if (anchor_direct_action_result_matches(packet, command_id)) {
        ret = anchor_reconcile_pending_command_result();
        return ret == 0 ? 1 : 0;
    }

    ret = app_node_comm_reserve_protocol_response(
        anchor_command_result_owner_generation(packet, command_id),
        &protocol_reservation);
    if (ret < 0) {
        LOG_WRN("anchor command deferred for result reservation: cmd=0x%04x ret=%d",
                (unsigned int)command_id,
                ret);
        return 0;
    }
    (void)anchor_execute_command_side_effects(
        packet,
        payload,
        payload_len,
        command_id,
        0u,
        &status,
        &reason,
        &force_rediscovery_after_result,
        &reboot_after_result,
        ingress_hop_id);

    completion_actions = anchor_command_completion_actions(
        status, force_rediscovery_after_result, reboot_after_result);
    if (completion_actions != 0u) {
        if (anchor_collection_result_pending.active) {
            (void)app_node_comm_cancel_protocol_response_reservation(
                &protocol_reservation);
            LOG_WRN("anchor deferred action-bearing command behind RAM result owner: cmd=0x%04x",
                    (unsigned int)command_id);
            return 0;
        }
        ret = anchor_admit_direct_action_result(
            packet,
            command_id,
            status,
            reason,
            completion_actions,
            &protocol_reservation);
        if (ret < 0) {
            LOG_WRN("anchor direct action result RAM admission deferred: cmd=0x%04x ret=%d",
                    (unsigned int)command_id,
                    ret);
            return 0;
        }
        LOG_INF("anchor action-bearing command accepted with RAM result custody: cmd=0x%04x",
                (unsigned int)command_id);
        return 1;
    }
    ret = anchor_submit_command_result(packet,
                                       command_id,
                                       status,
                                       reason,
                                       NULL,
                                       0u,
                                       &protocol_reservation,
                                       NULL);
    if (ret < 0) {
        (void)app_node_comm_cancel_protocol_response_reservation(
            &protocol_reservation);
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        return 0;
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
    return 1;
}

static int anchor_handle_local_command(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t ingress_hop_id)
{
    int ret;

    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    ret = anchor_handle_local_command_locked(packet, payload, payload_len,
                                             ingress_hop_id);
    k_mutex_unlock(&anchor_command_result_mutex);
    return ret;
}
