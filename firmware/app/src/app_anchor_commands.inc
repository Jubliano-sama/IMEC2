#line 338 __BASE_FILE__
static bool anchor_discovery_assignment_required(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           !IS_ENABLED(CONFIG_IMEC_ML_ANCHOR) &&
           !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
           IMEC_HIGH_DEBUG_ANCHOR_SLOT_ENABLED == 0;
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

static uint32_t anchor_collection_node_boot_id(void)
{
    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = sys_rand32_get();
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = k_uptime_get_32();
        }
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = 1u;
        }
    }
    return anchor_collection_node_boot_counter;
}

static uint16_t anchor_next_collection_result_seq(void)
{
    anchor_collection_result_seq++;
    if (anchor_collection_result_seq == 0u) {
        anchor_collection_result_seq = 1u;
    }
    return anchor_collection_result_seq;
}

static int anchor_submit_command_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    const struct command_result_id *collection_result_id,
    uint32_t collection_epoch_id,
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
    result_timeout_ms =
        command_id == CMD_SURVEY_PREPARE_PAIR ||
        command_id == CMD_SURVEY_START_PAIR ?
            SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS :
            GATEWAY_COMMAND_RESULT_TIMEOUT_MS;
    absolute_deadline_ms = UINT64_MAX - now_ms <
                           result_timeout_ms ?
                           UINT64_MAX :
                           now_ms + result_timeout_ms;
    client_token = ((uint32_t)command_id << 16) | seq;
    ret = app_node_comm_submit_protocol_response(
        &outbound,
        absolute_deadline_ms,
        client_token,
        delivery_handle_out == NULL ? NULL : &delivery_handle);
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
        if (delivery_handle_out != NULL) {
            *delivery_handle_out = delivery_handle;
        }
    }
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=uwb_mesh command=0x%04x status=%s reason=%u ret=%d",
                         (unsigned int)command_id,
                         command_status_name(status),
                         reason,
                         ret);
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

static uint32_t anchor_discovery_response_delay_ms(
    const struct anchor_discovery_claim_pending *pending)
{
    uint32_t delay_ms = DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;

    if (pending == NULL ||
        discovery_assignment_response_delay_ms(pending->slot,
                                               pending->slot_count,
                                               pending->hop_count,
                                               pending->attempt,
                                               sys_rand32_get(),
                                               &delay_ms) != PROTO_OK) {
        return DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;
    }
    return delay_ms;
}

static int anchor_send_discovery_response(
    const struct anchor_discovery_claim_pending *pending,
    uint32_t *delivery_handle_out)
{
    struct mesh_outbound outbound = {0};
    uint64_t hash;
    size_t payload_len = 0u;
    int ret;

    if (pending == NULL || pending->epoch == 0u) {
        return -EINVAL;
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     COMMAND_OK,
                                     0u);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
             outbound.payload,
             sizeof(outbound.payload),
             &payload_len,
             pending->phase,
             pending->epoch);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(outbound.payload,
                                                     sizeof(outbound.payload),
                                                     &payload_len,
                                                     hash);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound.payload,
                            sizeof(outbound.payload),
                            &payload_len,
                            TLV_HOP_COUNT,
                            pending->hop_count);
    }
    if (ret == PROTO_OK) {
        ret = mesh_init_command_result(&outbound.packet,
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
    outbound.payload_len = (uint16_t)payload_len;
    ret = app_node_comm_submit_protocol_response(
        &outbound,
        (uint64_t)k_uptime_get() + GATEWAY_COMMAND_RESULT_TIMEOUT_MS,
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
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
    }
    return ret;
}

static void anchor_discovery_claim_work_handler(struct k_work *work)
{
    struct node_comm_terminal_event event;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_discovery_claim_pending.active) {
        return;
    }
    if (anchor_discovery_claim_pending.delivery_handle != 0u) {
        if (!app_node_comm_take_delivery_event_for(
                anchor_discovery_claim_pending.delivery_handle, &event)) {
            (void)mesh_route_work_reschedule(
                &anchor_discovery_claim_work,
                ANCHOR_COMMAND_DELIVERY_POLL_MS);
            return;
        }
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_DELIVERED phase=%u epoch=%u attempts=%u\n",
                                anchor_discovery_claim_pending.phase,
                                anchor_discovery_claim_pending.epoch,
                                event.attempts_started);
            anchor_discovery_claim_pending.delivery_handle = 0u;
            anchor_discovery_claim_pending.active = false;
            return;
        }
        LOG_ERR("anchor discovery-slot response failed: phase=%u epoch=%u reason=%u attempts=%u",
                anchor_discovery_claim_pending.phase,
                anchor_discovery_claim_pending.epoch,
                (unsigned int)event.reason,
                event.attempts_started);
        status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_FAILED phase=%u epoch=%u attempts=%u reason=%u\n",
                            anchor_discovery_claim_pending.phase,
                            anchor_discovery_claim_pending.epoch,
                            event.attempts_started,
                            (unsigned int)event.reason);
        anchor_discovery_claim_pending.delivery_handle = 0u;
        anchor_discovery_claim_pending.active = false;
        return;
    }
    anchor_discovery_claim_pending.hop_count =
        anchor_discovery_gateway_hop_count();
    ret = anchor_send_discovery_response(
        &anchor_discovery_claim_pending,
        &anchor_discovery_claim_pending.delivery_handle);
    if (ret < 0) {
        LOG_ERR("anchor discovery-slot response admission failed: phase=%u epoch=%u ret=%d",
                anchor_discovery_claim_pending.phase,
                anchor_discovery_claim_pending.epoch,
                ret);
        anchor_discovery_claim_pending.active = false;
    }
}

static int anchor_schedule_discovery_response(
    const struct proto_packet *command,
    uint32_t epoch,
    enum discovery_assignment_phase phase,
    uint8_t slot,
    uint8_t slot_count)
{
    uint64_t hash;
    uint32_t delay_ms;

    if (command == NULL || epoch == 0u ||
        (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) ||
        slot_count == 0u || slot >= slot_count) {
        return -EINVAL;
    }
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.epoch == epoch &&
        anchor_discovery_claim_pending.phase == phase &&
        anchor_discovery_claim_pending.command.session_id ==
            command->session_id &&
        anchor_discovery_claim_pending.command.seq == command->seq) {
        return 0;
    }
    if (anchor_discovery_claim_pending.delivery_handle != 0u) {
        (void)app_node_comm_abandon_delivery(
            anchor_discovery_claim_pending.delivery_handle);
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    anchor_discovery_claim_pending.command = *command;
    anchor_discovery_claim_pending.epoch = epoch;
    anchor_discovery_claim_pending.delivery_handle = 0u;
    anchor_discovery_claim_pending.phase = phase;
    anchor_discovery_claim_pending.slot = slot;
    anchor_discovery_claim_pending.slot_count = slot_count;
    anchor_discovery_claim_pending.hop_count =
        anchor_discovery_gateway_hop_count();
    anchor_discovery_claim_pending.attempt = 0u;
    anchor_discovery_claim_pending.active = true;
    delay_ms = anchor_discovery_response_delay_ms(
        &anchor_discovery_claim_pending);
    (void)mesh_route_work_reschedule(&anchor_discovery_claim_work, delay_ms);
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_SCHEDULED phase=%u epoch=%u delay=%u slot=%u hop=%u hash=0x%016llx\n",
                        phase,
                        epoch,
                        delay_ms,
                        slot,
                        anchor_discovery_claim_pending.hop_count,
                        (unsigned long long)hash);
    return 0;
}

static int anchor_schedule_discovery_claim(const struct proto_packet *command,
                                           uint32_t epoch)
{
    uint8_t slot = (uint8_t)(discovery_assignment_hash(DEVICE_ID) %
                             UWB_DISCOVERY_SLOT_COUNT);

    return anchor_schedule_discovery_response(command,
                                              epoch,
                                              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                              slot,
                                              UWB_DISCOVERY_SLOT_COUNT);
}

static void anchor_collection_result_clear(void)
{
    memset(&anchor_collection_result_pending, 0, sizeof(anchor_collection_result_pending));
}

static int anchor_collection_result_persist(uint32_t delay_ms)
{
    struct app_mesh_collection_result_snapshot snapshot = {
        .version = APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION,
        .local_id = DEVICE_ID,
        .gateway_id = GATEWAY_ID,
        .valid = anchor_collection_result_pending.active,
    };

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return -EINVAL;
    }

    snapshot.command = anchor_collection_result_pending.command;
    snapshot.result_id = anchor_collection_result_pending.result_id;
    snapshot.collection_epoch_id = anchor_collection_result_pending.collection_epoch_id;
    snapshot.delay_ms = delay_ms == 0u ? 1u : delay_ms;
    snapshot.command_id = anchor_collection_result_pending.command_id;
    snapshot.status = anchor_collection_result_pending.status;
    snapshot.reason = anchor_collection_result_pending.reason;
    snapshot.force_rediscovery_after_result =
        anchor_collection_result_pending.force_rediscovery_after_result;
    snapshot.reboot_after_result =
        anchor_collection_result_pending.reboot_after_result;

    return app_mesh_persistence_save_collection_result(&snapshot);
}

static int anchor_collection_result_restore(void)
{
    struct app_mesh_collection_result_snapshot snapshot;
    uint32_t delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || anchor_collection_result_pending.active) {
        return -EBUSY;
    }

    ret = app_mesh_persistence_restore_collection_result(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid) {
        return 0;
    }
    if (snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID ||
        snapshot.result_id.node_id != DEVICE_ID ||
        snapshot.result_id.gateway_id != GATEWAY_ID ||
        (snapshot.command.dst_id != DEVICE_ID &&
         snapshot.command.dst_id != MESH_BROADCAST_ID)) {
        app_mesh_persistence_clear_collection_result();
        return -EINVAL;
    }

    anchor_collection_result_pending.command = snapshot.command;
    anchor_collection_result_pending.result_id = snapshot.result_id;
    anchor_collection_result_pending.collection_epoch_id = snapshot.collection_epoch_id;
    anchor_collection_result_pending.command_id = snapshot.command_id;
    anchor_collection_result_pending.status = snapshot.status;
    anchor_collection_result_pending.reason = snapshot.reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        snapshot.force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result =
        snapshot.reboot_after_result;
    anchor_collection_result_pending.active = true;
    anchor_collection_result_pending.persistence_dirty = false;
    anchor_collection_result_pending.persistence_retry_round = 0u;

    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = snapshot.result_id.node_boot_counter;
    }
    if (anchor_collection_result_seq < snapshot.result_id.result_seq) {
        anchor_collection_result_seq = snapshot.result_id.result_seq;
    }

    delay_ms = snapshot.delay_ms == 0u ? 1u : snapshot.delay_ms;
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));
    LOG_INF("anchor collection command result restored: cmd=0x%04x delay_ms=%u",
            (unsigned int)anchor_collection_result_pending.command_id,
            delay_ms);
    return 0;
}

static void anchor_collection_result_work_handler(struct k_work *work)
{
    struct anchor_collection_result_pending pending;
    uint32_t delivery_handle = 0u;
    uint8_t completion_actions;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return;
    }

    if (anchor_collection_result_pending.persistence_dirty) {
        uint32_t retry_ms;

        ret = anchor_collection_result_persist(1u);
        if (ret < 0) {
            retry_ms = discovery_assignment_retry_backoff_ms(
                anchor_collection_result_pending.persistence_retry_round,
                sys_rand32_get());
            if (anchor_collection_result_pending.persistence_retry_round < UINT8_MAX) {
                anchor_collection_result_pending.persistence_retry_round++;
            }
            LOG_WRN("anchor collection result persistence retry: ret=%d round=%u delay_ms=%u",
                    ret,
                    anchor_collection_result_pending.persistence_retry_round,
                    retry_ms);
            (void)k_work_reschedule(&anchor_collection_result_work,
                                    K_MSEC(retry_ms));
            return;
        }
        anchor_collection_result_pending.persistence_dirty = false;
        anchor_collection_result_pending.persistence_retry_round = 0u;
    }

    pending = anchor_collection_result_pending;
    anchor_collection_result_clear();
    completion_actions = anchor_command_completion_actions(
        pending.status,
        pending.force_rediscovery_after_result,
        pending.reboot_after_result);

    ret = anchor_submit_command_result(
        &pending.command,
        pending.command_id,
        pending.status,
        pending.reason,
        &pending.result_id,
        pending.collection_epoch_id,
        completion_actions == 0u ? NULL : &delivery_handle);
    if (ret < 0) {
        LOG_WRN("anchor collection command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)pending.command_id,
                pending.status,
                ret);
        pending.active = true;
        pending.persistence_dirty = true;
        anchor_collection_result_pending = pending;
        (void)k_work_reschedule(&anchor_collection_result_work,
                                K_MSEC(COLLECTION_RETRY_ROUND_0_MS));
        (void)anchor_collection_result_persist(COLLECTION_RETRY_ROUND_0_MS);
        return;
    }
    if (completion_actions != 0u) {
        ret = app_anchor_command_completion_watch(delivery_handle,
                                                  pending.command_id,
                                                  completion_actions);
        if (ret < 0) {
            (void)app_node_comm_abandon_delivery(delivery_handle);
            pending.active = true;
            pending.persistence_dirty = true;
            anchor_collection_result_pending = pending;
            (void)k_work_reschedule(&anchor_collection_result_work,
                                    K_MSEC(COLLECTION_RETRY_ROUND_0_MS));
            (void)anchor_collection_result_persist(
                COLLECTION_RETRY_ROUND_0_MS);
            LOG_WRN("anchor command completion admission failed: cmd=0x%04x ret=%d",
                    (unsigned int)pending.command_id, ret);
            return;
        }
    }

    app_mesh_persistence_clear_collection_result();
    LOG_INF("anchor collection command result sent: cmd=0x%04x status=%u reason=%u",
            (unsigned int)pending.command_id,
            pending.status,
            pending.reason);
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
    uint32_t now_ms;
    uint32_t due_ms;
    uint32_t delay_ms;

    if (command == NULL || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.active) {
        LOG_WRN("anchor collection command result already pending: cmd=0x%04x new=0x%04x",
                (unsigned int)anchor_collection_result_pending.command_id,
                (unsigned int)command_id);
        return -EBUSY;
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

    anchor_collection_result_pending.command = *command;
    anchor_collection_result_pending.collection_epoch_id = options->collection_epoch_id;
    anchor_collection_result_pending.result_id.gateway_id = GATEWAY_ID;
    anchor_collection_result_pending.result_id.gateway_epoch =
        (uint16_t)mesh_runtime.upstream.current_epoch;
    anchor_collection_result_pending.result_id.command_seq = options->command_seq;
    anchor_collection_result_pending.result_id.node_id = DEVICE_ID;
    anchor_collection_result_pending.result_id.node_boot_counter =
        anchor_collection_node_boot_id();
    anchor_collection_result_pending.result_id.result_seq =
        anchor_next_collection_result_seq();
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result = reboot_after_result;
    anchor_collection_result_pending.active = true;
    anchor_collection_result_pending.persistence_dirty = true;
    anchor_collection_result_pending.persistence_retry_round = 0u;
    if (anchor_collection_result_persist(delay_ms) == 0) {
        anchor_collection_result_pending.persistence_dirty = false;
    }
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));

    LOG_INF("anchor collection command result scheduled: cmd=0x%04x due_ms=%u delay_ms=%u",
            (unsigned int)command_id,
            due_ms,
            delay_ms);
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=collection-scheduled command=0x%04x command_seq=%u result_seq=%u collection=%u due_ms=%u delay_ms=%u",
                         (unsigned int)command_id,
                         anchor_collection_result_pending.result_id.command_seq,
                         anchor_collection_result_pending.result_id.result_seq,
                         anchor_collection_result_pending.collection_epoch_id,
                         due_ms,
                         delay_ms);
    return 0;
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
        (void)k_work_reschedule(&anchor_reboot_work,
                                K_MSEC(ANCHOR_REBOOT_RESULT_POLL_MS));
        return;
    }

    LOG_INF("anchor rebooting after command-result drain");
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static void anchor_schedule_reboot_after_command_result(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    anchor_reboot_pending = true;
    anchor_reboot_deadline_ms = k_uptime_get_32() + ANCHOR_REBOOT_RESULT_MAX_WAIT_MS;
    (void)k_work_reschedule(&anchor_reboot_work, K_MSEC(ANCHOR_REBOOT_RESULT_DRAIN_MS));
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
    size_t payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

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
                                              nonzero_uptime_session_id(),
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
    return app_node_comm_submit_reliable_uplink(
        &outbound,
        absolute_deadline_ms,
        ((uint32_t)MSG_ANCHOR_HEARTBEAT << 16) | outbound.packet.seq,
        NULL);
}

static void anchor_heartbeat_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR && anchor_heartbeat_enabled) {
        (void)k_work_reschedule(&anchor_heartbeat_work, K_MSEC(delay_ms));
    }
}

static void anchor_heartbeat_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_heartbeat_enabled) {
        return;
    }
    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_discovery_is_pending() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        anchor_heartbeat_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    ret = anchor_send_heartbeat();
    if (ret < 0) {
        LOG_WRN("anchor heartbeat TX failed: ret=%d interval_ms=%u", ret,
                anchor_heartbeat_interval_ms);
    } else {
        LOG_INF("anchor heartbeat queued: interval_ms=%u seq=%u",
                anchor_heartbeat_interval_ms,
                anchor_heartbeat_seq);
    }

    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
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
    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
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

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
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

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int command_find_i32_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                int32_t *value)
{
    uint32_t encoded_value;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = command_find_u32_tlv(payload, payload_len, type, &encoded_value);
    if (ret != PROTO_OK) {
        return ret;
    }

    memcpy(value, &encoded_value, sizeof(*value));
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

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
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

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
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

    ret = route_upsert_candidate(&mesh_runtime.upstream, &candidate);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return ret == PROTO_ERR_NO_SPACE ? -ENOSPC : -EINVAL;
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

static void anchor_force_rediscovery_from_command(void)
{
    int ret;

    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    ret = app_node_comm_schedule_path_refresh(GATEWAY_ID,
                                              "forced-rediscovery");
    if (ret < 0 && ret != -EAGAIN) {
        LOG_WRN("anchor forced rediscovery request failed: ret=%d", ret);
    } else {
        LOG_INF("anchor forced rediscovery requested");
    }
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
        anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    LOG_INF("anchor UWB scan duty command applied: interval_ms=%u min_ms=%u max_ms=%u awake_us=%u",
            anchor_uwb_scan_interval_ms,
            (unsigned int)ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
            ANCHOR_UWB_IDLE_SCAN_AWAKE_US);
    return 0;
}

static void anchor_preempt_for_survey_discovery(uint32_t survey_id)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    (void)k_work_cancel_delayable(&anchor_uwb_scan_work);
    if (anchor_uwb_session.state != UWB_ANCHOR_IDLE) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
        LOG_INF("survey discovery preempted pending click epoch: survey=%u", survey_id);
    }
    app_node_comm_stop_role_scan();
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
    options->expected_node_count = pending->expected_node_count;
    options->collection_required = pending->collection_required;
    options->flood_required = true;
}

static int anchor_apply_discovery_assignment_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    struct gateway_command_options assignment_options = {0};
    enum discovery_assignment_phase phase = 0;
    enum app_discovery_assignment_table_decision table_decision;
    uint32_t epoch = 0u;
    uint32_t table_fingerprint = 0u;
    size_t entry_count = 0u;
    uint8_t slot_count = 0u;
    int ret;

    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = gateway_command_extract_options(payload,
                                          payload_len,
                                          &assignment_options);
    if (ret != PROTO_OK || !assignment_options.flood_required ||
        assignment_options.command_seq == 0u ||
        assignment_options.command_seq != packet->session_id ||
        assignment_options.flood_epoch_id != assignment_options.command_seq) {
        return -EINVAL;
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
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
        return anchor_schedule_discovery_claim(packet, epoch);
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE) {
        return -EINVAL;
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
    table_fingerprint = discovery_assignment_table_fingerprint(
        entries, entry_count, slot_count);
    if (packet->session_id == 0u || table_fingerprint == 0u) {
        return -EINVAL;
    }
    table_decision = local_anchor_discovery_assignment_note_table(
        epoch, packet->session_id, table_fingerprint);
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID) {
        return -EINVAL;
    }
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE) {
        status_debug_printf("DBG_DISCOVERY_SLOT_STALE phase=table epoch=%u state=%s\n",
                            epoch,
                            app_discovery_assignment_provisioning_state_name(
                                local_anchor_discovery_assignment_provisioning_state()));
        LOG_WRN("anchor ignored stale discovery assignment table: epoch=%u",
                epoch);
        return 0;
    }
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM) {
        status_debug_printf("DBG_DISCOVERY_SLOT_LATE_CLAIM epoch=%u state=%s count=%u\n",
                            epoch,
                            app_discovery_assignment_provisioning_state_name(
                                local_anchor_discovery_assignment_provisioning_state()),
                            (unsigned int)entry_count);
        LOG_WRN("anchor received discovery table before claim epoch: epoch=%u count=%u; retaining committed assignment and late-claiming",
                epoch,
                (unsigned int)entry_count);
        return anchor_schedule_discovery_claim(packet, epoch);
    }

    (void)k_work_cancel_delayable(&anchor_discovery_claim_work);
    anchor_discovery_claim_pending.active = false;
    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].anchor_id != DEVICE_ID) {
            continue;
        }
        {
            const struct app_mesh_discovery_assignment_snapshot snapshot = {
                .epoch = epoch,
                .table_command_seq = packet->session_id,
                .table_fingerprint = table_fingerprint,
                .local_id = DEVICE_ID,
                .gateway_id = GATEWAY_ID,
                .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
                .slot = entries[i].slot,
                .slot_count = slot_count,
                .provisioned = true,
                .valid = true,
            };

            ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
            if (ret < 0) {
                LOG_WRN("anchor discovery assignment persistence failed: epoch=%u slot=%u ret=%d",
                        epoch,
                        entries[i].slot,
                        ret);
                return ret;
            }
        }
        ret = local_anchor_commit_discovery_assignment(epoch,
                                                       packet->session_id,
                                                       table_fingerprint,
                                                       entries[i].slot,
                                                       slot_count);
        if (ret != PROTO_OK) {
            LOG_ERR("anchor discovery assignment commit rejected after persistence: epoch=%u slot=%u ret=%d",
                    epoch,
                    entries[i].slot,
                    ret);
            return -ESTALE;
        }
        ret = anchor_schedule_discovery_response(
            packet,
            epoch,
            DISCOVERY_ASSIGNMENT_PHASE_ACK,
            entries[i].slot,
            slot_count);
        if (ret < 0) {
            return ret;
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_ASSIGNED state=PROVISIONED epoch=%u slot=%u slot_count=%u anchors=%u hash=0x%016llx\n",
                            epoch,
                            entries[i].slot,
                            slot_count,
                            (unsigned int)entry_count,
                            (unsigned long long)entries[i].hash);
        LOG_INF("anchor discovery slot assigned: epoch=%u slot=%u slot_count=%u anchors=%u",
                epoch,
                entries[i].slot,
                slot_count,
                (unsigned int)entry_count);
        return 0;
    }

    {
        const struct app_mesh_discovery_assignment_snapshot snapshot = {
            .epoch = epoch,
            .table_command_seq = packet->session_id,
            .table_fingerprint = table_fingerprint,
            .local_id = DEVICE_ID,
            .gateway_id = GATEWAY_ID,
            .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
            .slot = 0u,
            .slot_count = slot_count,
            .provisioned = false,
            .valid = true,
        };

        ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
        if (ret < 0) {
            LOG_WRN("anchor unassigned discovery generation persistence failed: epoch=%u generation=%u ret=%d",
                    epoch,
                    packet->session_id,
                    ret);
            return ret;
        }
    }
    ret = local_anchor_mark_discovery_assignment_unprovisioned(
        epoch, packet->session_id, table_fingerprint);
    if (ret != PROTO_OK) {
        app_mesh_persistence_clear_discovery_assignment();
        return -ESTALE;
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_UNASSIGNED state=UNPROVISIONED epoch=%u count=%u reason=not-listed\n",
                        epoch,
                        (unsigned int)entry_count);
    LOG_WRN("anchor not listed in discovery-slot assignment: epoch=%u count=%u",
            epoch,
            (unsigned int)entry_count);
    return anchor_schedule_discovery_claim(packet, epoch);
}

static void anchor_execute_command_side_effects(const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                enum command_id command_id,
                                                enum command_status *status,
                                                uint8_t *reason,
                                                bool *force_rediscovery_after_result,
                                                bool *reboot_after_result)
{
    enum device_role requested_role = ROLE_ANCHOR;
    int ret;

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
            *status = ret == -ENOSPC ? COMMAND_BUSY : COMMAND_MALFORMED_PAYLOAD;
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
        ret = anchor_apply_discovery_assignment_command(packet,
                                                        payload,
                                                        payload_len);
        if (ret < 0) {
            *status = ret == -ESTALE ? COMMAND_INVALID_STATE :
                      COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        ret = app_anchor_survey_runtime_start_pair_from_command(packet,
                                                               payload,
                                                               payload_len,
                                                               status,
                                                               reason);
        if (ret < 0 && *status == COMMAND_OK) {
            *status = COMMAND_INTERNAL_ERROR;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_ABORT) {
        struct survey_pair pair = {0};

        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
        if (ret == PROTO_OK) {
            (void)app_anchor_survey_runtime_abort_pair_matching(
                &pair,
                packet->session_id);
        } else if (ret == PROTO_ERR_NOT_FOUND && payload_len == 4u) {
            /* A command-ID-only host abort intentionally remains broad. */
            app_anchor_survey_runtime_abort_pair();
        } else {
            *status = COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        *status = COMMAND_UNSUPPORTED_COMMAND;
        *reason = 1u;
    }
}

static void anchor_finish_broadcast_command(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            enum command_id command_id,
                                            const struct gateway_command_options *command_options)
{
    enum command_status status = COMMAND_OK;
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    uint8_t reason = 0u;
    int ret;

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    if (command_options->response_mode == CMD_RESPONSE_NONE) {
        LOG_INF("anchor broadcast command handled without result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        if (reboot_after_result && status == COMMAND_OK) {
            anchor_schedule_reboot_after_command_result();
        }
        return;
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
}

static int anchor_schedule_broadcast_command_execution(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_command_options *options,
    uint32_t delay_ms)
{
    if (packet == NULL || payload == NULL || options == NULL ||
        payload_len > sizeof(anchor_pending_command_execution.payload)) {
        return -EINVAL;
    }
    if (anchor_pending_command_execution.active) {
        return -EBUSY;
    }

    anchor_pending_command_execution.command = *packet;
    anchor_pending_command_execution.options.response_mode = options->response_mode;
    anchor_pending_command_execution.options.command_seq = options->command_seq;
    anchor_pending_command_execution.options.collection_epoch_id = options->collection_epoch_id;
    anchor_pending_command_execution.options.collection_slot_seed = options->collection_slot_seed;
    anchor_pending_command_execution.options.command_expiry_s = options->command_expiry_s;
    anchor_pending_command_execution.options.expected_node_count = options->expected_node_count;
    anchor_pending_command_execution.options.collection_required = options->collection_required;
    memcpy(anchor_pending_command_execution.payload, payload, payload_len);
    anchor_pending_command_execution.payload_len = (uint16_t)payload_len;
    anchor_pending_command_execution.received_at_ms = k_uptime_get_32();
    anchor_pending_command_execution.active = true;
    (void)k_work_reschedule(&anchor_command_execute_work,
                            K_MSEC(delay_ms == 0u ? 1u : delay_ms));
    LOG_INF("anchor broadcast command execution scheduled: command_seq=%u delay_ms=%u",
            options->command_seq,
            delay_ms);
    return 0;
}

static void anchor_command_execute_work_handler(struct k_work *work)
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
    memset(&anchor_pending_command_execution, 0, sizeof(anchor_pending_command_execution));

    now_ms = k_uptime_get_32();
    packet_age_add_elapsed(&pending.command, now_ms - pending.received_at_ms);
    anchor_pending_options_to_gateway(&pending.options, &options);

    if (gateway_command_receive_expired(&pending.command, &options)) {
        LOG_WRN("anchor dropped expired delayed broadcast command: command_seq=%u age_ms=%u expiry_s=%u",
                options.command_seq,
                pending.command.message_age_ms,
                options.command_expiry_s);
        return;
    }

    ret = gateway_command_extract_id(pending.payload, pending.payload_len, &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("anchor dropped malformed delayed broadcast command: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        return;
    }

    anchor_finish_broadcast_command(&pending.command,
                                    pending.payload,
                                    pending.payload_len,
                                    command_id,
                                    &options);
}

static void anchor_handle_local_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    struct gateway_command_options command_options = {0};
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    bool wait_for_delivery_completion;
    bool broadcast_command;
    bool expired;
    bool duplicate;
    uint32_t delivery_handle = 0u;
    uint32_t delay_ms;
    uint32_t now_ms;
    uint8_t completion_actions;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND) {
        return;
    }
    broadcast_command = packet->dst_id == MESH_BROADCAST_ID;
    if (packet->dst_id != DEVICE_ID && !broadcast_command) {
        return;
    }

    now_ms = k_uptime_get_32();
    ret = app_mesh_command_orchestrator_anchor_receive(
        mesh_gateway_command_orchestrator_context(),
        packet,
        payload,
        payload_len,
        now_ms,
        &command_id,
        &command_options,
        &broadcast_command,
        &expired,
        &duplicate);
    if (ret != PROTO_OK) {
        if (broadcast_command) {
            LOG_WRN("anchor ignored invalid broadcast command: ret=%d", ret);
            return;
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
            return;
        }
        if (duplicate) {
            if (command_id == CMD_ASSIGN_DISCOVERY_SLOTS) {
                ret = anchor_apply_discovery_assignment_command(packet,
                                                                payload,
                                                                payload_len);
                if (ret < 0) {
                    LOG_WRN("anchor rejected duplicate discovery assignment command: command_seq=%u ret=%d",
                            command_options.command_seq,
                            ret);
                }
                return;
            }
            LOG_INF("anchor ignored duplicate broadcast command: cmd=0x%04x command_seq=%u",
                    (unsigned int)command_id,
                    command_options.command_seq);
            return;
        }

        delay_ms = gateway_command_execute_delay_remaining_ms(packet, &command_options);
        if (delay_ms != 0u) {
            ret = anchor_schedule_broadcast_command_execution(packet,
                                                              payload,
                                                              payload_len,
                                                              &command_options,
                                                              delay_ms);
            if (ret < 0) {
                LOG_WRN("anchor delayed broadcast command schedule failed: cmd=0x%04x command_seq=%u ret=%d",
                        (unsigned int)command_id,
                        command_options.command_seq,
                        ret);
                return;
            }
            return;
        }

        anchor_finish_broadcast_command(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &command_options);
        return;
    }

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    completion_actions = anchor_command_completion_actions(
        status, force_rediscovery_after_result, reboot_after_result);
    wait_for_delivery_completion =
        (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) ||
        completion_actions != 0u;
    ret = anchor_submit_command_result(packet,
                                       command_id,
                                       status,
                                       reason,
                                       NULL,
                                       0u,
                                       wait_for_delivery_completion ?
                                           &delivery_handle : NULL);
    if (ret < 0) {
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
            (void)app_anchor_survey_runtime_cancel_pair_start(packet);
        }
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        return;
    }
    if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
        int bind_ret = app_anchor_survey_runtime_bind_pair_start_delivery(
            packet, delivery_handle);

        if (bind_ret < 0 && bind_ret != -ESTALE) {
            LOG_WRN("survey pair start delivery binding failed: ret=%d",
                    bind_ret);
        }
        if (bind_ret < 0) {
            (void)app_node_comm_auto_reap_delivery(delivery_handle);
        }
    }
    if (completion_actions != 0u) {
        ret = app_anchor_command_completion_watch(delivery_handle,
                                                  command_id,
                                                  completion_actions);
        if (ret < 0) {
            (void)app_node_comm_abandon_delivery(delivery_handle);
            LOG_ERR("anchor command completion admission failed: cmd=0x%04x ret=%d",
                    (unsigned int)command_id, ret);
            return;
        }
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
}
