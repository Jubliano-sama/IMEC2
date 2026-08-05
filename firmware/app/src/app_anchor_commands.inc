#line 338 __BASE_FILE__
#if DEVICE_ROLE == ROLE_ANCHOR
static struct k_work_delayable anchor_discovery_ack_liveness_work;
static uint32_t anchor_discovery_ack_liveness_generation;
#endif

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
    int ret;

    if (packet == NULL || options == NULL ||
        packet->dst_id != MESH_BROADCAST_ID ||
        options->command_seq == 0u) {
        return -EINVAL;
    }
    candidate = orchestrator->anchor;
    app_mesh_gateway_command_flow_anchor_remember(
        &candidate, packet, options, now_ms);
    ret = app_mesh_persistence_save_anchor_command_replay(
        DEVICE_ID, GATEWAY_ID, &candidate.duplicate_cache);
    if (ret < 0 && ret != -ENOTSUP) {
        return ret;
    }
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
    uint32_t protocol_reservation_token,
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
    ret = protocol_reservation_token == 0u ?
        app_node_comm_submit_protocol_response(
            &outbound,
            absolute_deadline_ms,
            client_token,
            delivery_handle_out == NULL ? NULL : &delivery_handle) :
        app_node_comm_commit_protocol_response(
            protocol_reservation_token,
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
                                        0u,
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
        discovery_assignment_response_delay_ms(pending->response_spread_ms,
                                               pending->attempt,
                                               sys_rand32_get(),
                                               &delay_ms) != PROTO_OK) {
        return DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;
    }
    return delay_ms;
}

static uint32_t anchor_discovery_ack_low_duty_retry_delay_ms(
    uint8_t terminal_retry_count,
    uint32_t random_value)
{
    uint32_t delay_ms = ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS;
    uint8_t low_duty_round =
        terminal_retry_count > ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES ?
        terminal_retry_count - ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES :
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
    if (ret == PROTO_OK &&
        pending->phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        ret = discovery_assignment_append_table_commitment(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            &pending->table_commitment);
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
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
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
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
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

static int anchor_promote_discovery_assignment_after_ack_locked(
    const struct anchor_discovery_claim_pending *pending)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    int ret;

    if (pending == NULL ||
        pending->phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
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
    if (snapshot.ack_pending == 0u ||
        snapshot.pending_valid == 0u ||
        snapshot.pending_epoch != pending->epoch ||
        snapshot.pending_table_command_seq != pending->command.session_id ||
        !discovery_assignment_table_commitment_equal(
            &snapshot.pending_table_commitment,
            &pending->table_commitment) ||
        snapshot.pending_slot != pending->slot ||
        snapshot.pending_slot_count != pending->slot_count ||
        snapshot.table_packet_seq != pending->command.seq ||
        snapshot.response_spread_ms != pending->response_spread_ms) {
        return -ESTALE;
    }

    /*
     * The TABLE ACK is now physically delivered.  Commit that fact to NVS
     * before enabling normal ranging replies, so a reset can never restore a
     * slot that the gateway did not include in its roster.
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
    snapshot.pending_valid = 0u;
    snapshot.ack_retry_round = 0u;
    ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
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
        "DBG_DISCOVERY_SLOT_ASSIGNED state=PROVISIONED epoch=%u slot=%u slot_count=%u ack=delivered\n",
        snapshot.epoch,
        snapshot.slot,
        snapshot.slot_count);
    LOG_INF("anchor discovery slot committed after TABLE ACK delivery: epoch=%u slot=%u slot_count=%u",
            snapshot.epoch,
            snapshot.slot,
            snapshot.slot_count);
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
    ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
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
        snapshot.table_packet_seq != pending->command.seq) {
        return -ESTALE;
    }
    if (snapshot.ack_retry_round == retry_round) {
        return 0;
    }
    snapshot.ack_retry_round = retry_round;
    return app_mesh_persistence_save_discovery_assignment(&snapshot);
}

static void anchor_discovery_assignment_service_ack_promotion(
    const struct anchor_discovery_claim_pending *pending)
{
    uint32_t retry_ms = 0u;
    uint8_t attempt = 0u;
    bool retryable;
    int schedule_ret = 0;
    int ret;

    if (pending == NULL || !pending->ack_delivered) {
        return;
    }
    ret = anchor_promote_discovery_assignment_after_ack(pending);
    retryable = ret == -EIO || ret == -EAGAIN || ret == -ENOSPC ||
                ret == -EBUSY || ret == -ENODEV || ret == -ENOMEM;
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
                anchor_discovery_claim_pending.active = false;
            }
        } else {
            anchor_discovery_claim_pending.active = false;
        }
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
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
    ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
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
        snapshot.pending_valid = 0u;
        snapshot.ack_retry_round = 0u;
        ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
    } else {
        /*
         * A first-assignment candidate has no finalized identity, so a
         * pending-less v7 record is invalid. Delete that exact obsolete record
         * through the checked path instead of pretending an empty snapshot was
         * committed.
         */
        ret = app_mesh_persistence_clear_discovery_assignment_checked();
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
         * Each node-communication handle is bounded, but durable TABLE ACK
         * custody is not discarded when one handle expires. A fresh handle
         * and deadline are created until exact delivery, supersession, or an
         * explicit cancellation wins the generation race.
         */
        retry = pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
                event.reason != NODE_COMM_TERMINAL_CANCELLED;
        if (retry) {
            low_duty_retry =
                pending.terminal_retry_count >=
                    ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES;
            retry_ms = low_duty_retry ?
                anchor_discovery_ack_low_duty_retry_delay_ms(
                    pending.terminal_retry_count, sys_rand32_get()) :
                discovery_assignment_retry_backoff_ms(
                    pending.attempt, sys_rand32_get());
        }
        anchor_discovery_claim_pending.delivery_handle = 0u;
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED &&
            pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
            anchor_discovery_claim_pending.ack_delivered = true;
            anchor_discovery_claim_pending.next_attempt_not_before_ms =
                (uint64_t)k_uptime_get();
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
            anchor_discovery_ack_refresh_attempt_deadline_locked(
                retry_not_before_ms);
            persistence_ret =
                anchor_persist_discovery_assignment_ack_retry_round(
                    &pending,
                    anchor_discovery_claim_pending.terminal_retry_count);
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
            if (pending.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
                pending.ack_delivered = true;
                anchor_discovery_assignment_service_ack_promotion(&pending);
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

    pending.hop_count = anchor_discovery_gateway_hop_count();
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
            anchor_discovery_claim_pending.hop_count = pending.hop_count;
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
        anchor_discovery_claim_pending.hop_count = pending.hop_count;
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

static int anchor_settle_ack_before_newer_assignment(uint32_t incoming_epoch)
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
        !discovery_assignment_epoch_strictly_newer(
            incoming_epoch, anchor_discovery_claim_pending.epoch)) {
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

static int anchor_schedule_discovery_response(
    const struct proto_packet *command,
    uint32_t epoch,
    enum discovery_assignment_phase phase,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint8_t slot,
    uint8_t slot_count,
    uint16_t response_spread_ms,
    uint8_t restored_ack_retry_round)
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
        !discovery_assignment_epoch_strictly_newer(
            epoch, anchor_discovery_claim_pending.epoch)) {
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
    anchor_discovery_claim_pending.response_spread_ms = response_spread_ms;
    anchor_discovery_claim_pending.hop_count = hop_count;
    anchor_discovery_claim_pending.attempt = restored_ack_retry_round;
    anchor_discovery_claim_pending.terminal_retry_count =
        restored_ack_retry_round;
    anchor_discovery_claim_pending.ack_delivered = false;
    anchor_discovery_claim_pending.active = true;
    delay_ms =
        phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
        restored_ack_retry_round >=
            ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES ?
        anchor_discovery_ack_low_duty_retry_delay_ms(
            restored_ack_retry_round, sys_rand32_get()) :
        anchor_discovery_response_delay_ms(
            &anchor_discovery_claim_pending);
    not_before_ms = (uint64_t)k_uptime_get() + delay_ms;
    anchor_discovery_claim_pending.next_attempt_not_before_ms = not_before_ms;
    anchor_discovery_claim_pending.absolute_deadline_ms =
        discovery_assignment_response_deadline_ms(
            not_before_ms - delay_ms,
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
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_SCHEDULED phase=%u epoch=%u delay=%u slot=%u hop=%u hash=0x%016llx\n",
                        phase,
                        epoch,
                        delay_ms,
                        slot,
                        hop_count,
                        (unsigned long long)hash);
    k_mutex_unlock(&anchor_discovery_assignment_transaction_mutex);
    return 0;
}

static int anchor_schedule_discovery_claim(const struct proto_packet *command,
                                           uint32_t epoch,
                                           uint16_t response_spread_ms)
{
    uint8_t slot = (uint8_t)(discovery_assignment_hash(DEVICE_ID) %
                             UWB_DISCOVERY_SLOT_COUNT);

    return anchor_schedule_discovery_response(command,
                                              epoch,
                                              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                              NULL,
                                              slot,
                                              UWB_DISCOVERY_SLOT_COUNT,
                                              response_spread_ms,
                                              0u);
}

static int anchor_schedule_late_discovery_claim(
    const struct proto_packet *table_command,
    uint32_t epoch,
    uint16_t response_spread_ms)
{
    struct proto_packet claim_command;

    if (table_command == NULL || epoch == 0u) {
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
        &claim_command, epoch, response_spread_ms);
}

static int anchor_resume_pending_discovery_assignment_ack(
    bool explicit_table_replay)
{
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
    };
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }
    ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
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
    if (explicit_table_replay && snapshot.ack_retry_round != 0u) {
        snapshot.ack_retry_round = 0u;
        ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
        if (ret < 0) {
            return ret;
        }
    }
    command.session_id = snapshot.pending_table_command_seq;
    command.seq = snapshot.table_packet_seq;
    ret = anchor_schedule_discovery_response(
        &command,
        snapshot.pending_epoch,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        &snapshot.pending_table_commitment,
        snapshot.pending_slot,
        snapshot.pending_slot_count,
        snapshot.response_spread_ms,
        snapshot.ack_retry_round);
    if (ret == 0) {
        status_debug_printf(
            "DBG_DISCOVERY_SLOT_ACK_RESUME state=ACK_PENDING epoch=%u generation=%u slot=%u\n",
            snapshot.pending_epoch,
            snapshot.pending_table_command_seq,
            snapshot.pending_slot);
        return 1;
    }
    return ret;
}

static void anchor_collection_result_clear(void)
{
    memset(&anchor_collection_result_pending, 0, sizeof(anchor_collection_result_pending));
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

static int anchor_collection_result_clear_durable(void *context)
{
    ARG_UNUSED(context);
    return app_mesh_persistence_clear_collection_result();
}

static bool anchor_collection_result_take_terminal(
    uint32_t delivery_handle,
    struct node_comm_terminal_event *event_out,
    void *context)
{
    ARG_UNUSED(context);
    return app_node_comm_take_delivery_event_for(delivery_handle, event_out);
}

static void anchor_collection_result_apply_actions(
    const struct anchor_collection_result_pending *pending)
{
    uint8_t actions;

    if (pending == NULL) {
        return;
    }
    actions = anchor_command_completion_actions(
        pending->status,
        pending->force_rediscovery_after_result,
        pending->reboot_after_result);
    if ((actions &
         APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY) != 0u) {
        anchor_force_rediscovery_from_command();
    }
    if ((actions & APP_ANCHOR_COMMAND_COMPLETION_REBOOT) != 0u) {
        anchor_schedule_reboot_after_command_result();
    }
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

static void anchor_collection_result_retry(const char *source)
{
    uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
        anchor_collection_result_pending.persistence_retry_round,
        sys_rand32_get());
    int persist_ret;

    if (anchor_collection_result_pending.persistence_retry_round <
        UINT8_MAX) {
        anchor_collection_result_pending.persistence_retry_round++;
    }
    persist_ret = anchor_collection_result_persist(retry_ms);
    if (persist_ret < 0) {
        anchor_collection_result_pending.persistence_dirty = true;
        LOG_WRN("anchor command result retry persistence failed: cmd=0x%04x ret=%d",
                (unsigned int)anchor_collection_result_pending.command_id,
                persist_ret);
    } else {
        anchor_collection_result_pending.persistence_dirty = false;
    }
    (void)anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(retry_ms),
        source);
}

static int anchor_collection_result_restore_locked(void)
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
        anchor_collection_result_restore_complete = true;
        return 0;
    }
    if (snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID ||
        (snapshot.command.dst_id != DEVICE_ID &&
         snapshot.command.dst_id != MESH_BROADCAST_ID)) {
        return -EINVAL;
    }
    if (snapshot.result_id.node_id != 0u &&
        (snapshot.result_id.node_id != DEVICE_ID ||
         snapshot.result_id.gateway_id != GATEWAY_ID ||
         snapshot.result_id.command_seq == 0u ||
         snapshot.result_id.node_boot_counter == 0u ||
         snapshot.result_id.result_seq == 0u)) {
        return -EINVAL;
    }
    if (snapshot.result_id.node_id == 0u &&
        (snapshot.result_id.gateway_id != 0u ||
         snapshot.result_id.command_seq != 0u ||
         snapshot.result_id.node_boot_counter != 0u ||
         snapshot.result_id.result_seq != 0u ||
         snapshot.collection_epoch_id != 0u ||
         (!snapshot.force_rediscovery_after_result &&
          !snapshot.reboot_after_result))) {
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

    /*
     * The restored pending result keeps its exact pre-reset identity in
     * `anchor_collection_result_pending`. Do not seed the allocator from it:
     * every result created after this boot must use a fresh boot counter, and
     * its per-boot sequence starts independently at one.
     */

    delay_ms = snapshot.delay_ms == 0u ? 1u : snapshot.delay_ms;
    ret = anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(delay_ms),
        "collection-result-restore");
    if (ret < 0) {
        return ret;
    }
    anchor_collection_result_restore_complete = true;
    LOG_INF("anchor collection command result restored: cmd=0x%04x delay_ms=%u",
            (unsigned int)anchor_collection_result_pending.command_id,
            delay_ms);
    return 0;
}

static int anchor_collection_result_restore(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    ret = anchor_collection_result_restore_locked();
    if (ret == -ENOENT || ret == -ENOTSUP) {
        /*
         * These are definitive no-durable-owner outcomes accepted by startup,
         * so later command admission may begin with a fresh boot identity.
         */
        anchor_collection_result_restore_complete = true;
    }
    k_mutex_unlock(&anchor_command_result_mutex);
    return ret;
}

static void anchor_collection_result_work_handler_locked(struct k_work *work)
{
    struct anchor_collection_result_pending pending;
    struct node_comm_terminal_event event;
    struct node_comm_terminal_event taken_event;
    const struct command_result_id *collection_result_id;
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
            (void)anchor_checked_work_reschedule(
                &anchor_collection_result_work,
                K_MSEC(retry_ms),
                "collection-result-persistence-retry");
            return;
        }
        anchor_collection_result_pending.persistence_dirty = false;
        anchor_collection_result_pending.persistence_retry_round = 0u;
    }
    ret = anchor_commit_pending_collection_command_replay();
    if (ret < 0) {
        uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
            anchor_collection_result_pending.persistence_retry_round,
            sys_rand32_get());

        if (anchor_collection_result_pending.persistence_retry_round <
            UINT8_MAX) {
            anchor_collection_result_pending.persistence_retry_round++;
        }
        LOG_WRN("anchor collection result replay commit retry: ret=%d round=%u delay_ms=%u",
                ret,
                anchor_collection_result_pending.persistence_retry_round,
                retry_ms);
        (void)anchor_checked_work_reschedule(
            &anchor_collection_result_work,
            K_MSEC(retry_ms),
            "collection-result-replay-retry");
        return;
    }

    if (anchor_collection_result_pending.delivery_handle != 0u) {
        uint32_t delivery_handle =
            anchor_collection_result_pending.delivery_handle;

        if (!app_node_comm_peek_delivery_event_for(delivery_handle, &event)) {
            (void)anchor_checked_work_reschedule(
                &anchor_collection_result_work,
                K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
                "command-result-terminal-poll");
            return;
        }
        ret = app_anchor_command_completion_commit_terminal(
            delivery_handle,
            &event,
            anchor_collection_result_clear_durable,
            anchor_collection_result_take_terminal,
            NULL,
            &taken_event);
        if (ret < 0) {
            /*
             * A delivered terminal may have cleared NVS before an unexpected
             * exact-take race. Restore the same result/action owner before
             * retrying so reset cannot lose the side effect.
             */
            if (event.reason == NODE_COMM_TERMINAL_DELIVERED &&
                anchor_collection_result_persist(
                    ANCHOR_COMMAND_DELIVERY_POLL_MS) < 0) {
                anchor_collection_result_pending.persistence_dirty = true;
            }
            if (ret == -EPROTO) {
                /*
                 * The exact terminal was consumed before its immutable
                 * metadata mismatch was detected. The durable snapshot is
                 * still authoritative, but this handle can never become
                 * terminal again.
                 */
                anchor_collection_result_pending.delivery_handle = 0u;
            }
            LOG_WRN("anchor command result terminal commit deferred: cmd=0x%04x handle=%u ret=%d",
                    (unsigned int)anchor_collection_result_pending.command_id,
                    delivery_handle,
                    ret);
            anchor_collection_result_retry(
                "command-result-terminal-commit-retry");
            return;
        }
        if (ret == 0) {
            LOG_WRN("anchor command result delivery failed; retaining exact durable result: cmd=0x%04x handle=%u reason=%u attempts=%u",
                    (unsigned int)anchor_collection_result_pending.command_id,
                    delivery_handle,
                    (unsigned int)taken_event.reason,
                    taken_event.attempts_started);
            anchor_collection_result_pending.delivery_handle = 0u;
            anchor_collection_result_retry(
                "command-result-terminal-resubmit");
            return;
        }

        pending = anchor_collection_result_pending;
        anchor_collection_result_clear();
        anchor_collection_result_apply_actions(&pending);
        LOG_INF("anchor command result gateway-confirmed: cmd=0x%04x handle=%u attempts=%u",
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
        0u,
        &anchor_collection_result_pending.delivery_handle);
    if (ret < 0) {
        LOG_WRN("anchor collection command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)pending.command_id,
                pending.status,
                ret);
        anchor_collection_result_pending.delivery_handle = 0u;
        anchor_collection_result_retry("command-result-submit-retry");
        return;
    }
    (void)anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
        "command-result-terminal-owner");
    LOG_INF("anchor command result submitted with durable terminal custody: cmd=0x%04x status=%u reason=%u handle=%u",
            (unsigned int)pending.command_id,
            pending.status,
            pending.reason,
            anchor_collection_result_pending.delivery_handle);
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
    uint32_t now_ms;
    uint32_t due_ms;
    uint32_t delay_ms;
    int schedule_ret;
    int ret;

    if (command == NULL || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (!anchor_collection_result_restore_complete) {
        return -EAGAIN;
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
    memset(&anchor_collection_result_pending.result_id,
           0,
           sizeof(anchor_collection_result_pending.result_id));
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
    anchor_collection_result_pending.delivery_handle = 0u;
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result = reboot_after_result;
    anchor_collection_result_pending.active = true;
    anchor_collection_result_pending.persistence_dirty = true;
    anchor_collection_result_pending.persistence_retry_round = 0u;
    ret = anchor_collection_result_persist(delay_ms);
    if (ret == 0) {
        anchor_collection_result_pending.persistence_dirty = false;
    }
    schedule_ret = anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(delay_ms),
        "collection-result-admission");
    if (schedule_ret < 0) {
        /*
         * Once NVS owns the exact result and actions, a scheduling failure is
         * recovered by the watchdog/reset restore path. Reporting rejection
         * here would let the gateway replay a command whose side effect has
         * already been accepted.
         */
        return ret == 0 ? 0 : schedule_ret;
    }

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
    return ret;
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
    uint32_t protocol_reservation_token)
{
    uint32_t delivery_handle = 0u;
    int persist_ret;
    int ret;

    if (command == NULL || completion_actions == 0u ||
        protocol_reservation_token == 0u) {
        return -EINVAL;
    }
    if (!anchor_collection_result_restore_complete) {
        return -EAGAIN;
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
    anchor_collection_result_pending.persistence_dirty = true;

    persist_ret = anchor_collection_result_persist(1u);
    if (persist_ret < 0) {
        (void)app_node_comm_cancel_protocol_response_reservation(
            protocol_reservation_token);
        anchor_collection_result_retry(
            "direct-command-result-persistence-retry");
        return anchor_collection_result_pending.persistence_dirty ?
            persist_ret : 0;
    }
    anchor_collection_result_pending.persistence_dirty = false;

    ret = anchor_submit_command_result(
        command,
        command_id,
        status,
        reason,
        NULL,
        0u,
        protocol_reservation_token,
        &delivery_handle);
    if (ret < 0) {
        (void)app_node_comm_cancel_protocol_response_reservation(
            protocol_reservation_token);
        anchor_collection_result_retry(
            "direct-command-result-submit-retry");
        /*
         * Durable custody is authoritative now. The exact command must not
         * be reported unhandled merely because its first transport admission
         * failed.
         */
        return 0;
    }

    anchor_collection_result_pending.delivery_handle = delivery_handle;
    (void)anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_MSEC(ANCHOR_COMMAND_DELIVERY_POLL_MS),
        "direct-command-result-terminal-owner");
    /*
     * Scheduling failure is fail-closed through the watchdog and the durable
     * snapshot restores the exact result after reset.
     */
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
        (void)anchor_checked_work_reschedule(
            &anchor_reboot_work,
            K_MSEC(ANCHOR_REBOOT_RESULT_POLL_MS),
            "post-result-reboot-poll");
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
    (void)anchor_checked_work_reschedule(
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
        (void)anchor_checked_work_reschedule(
            &anchor_heartbeat_work,
            K_MSEC(delay_ms),
            "heartbeat");
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
    if (epoch_changed) {
        /*
         * Reserve the new generation before clearing any old-epoch route,
         * timing, discovery, or custody relationship. A reset between this
         * write and the mutation restores the newer empty generation, which
         * is safe and forces rediscovery instead of reviving old custody.
         */
        ret = app_mesh_route_state_persist(&mesh_runtime,
                                           candidate.route_epoch,
                                           0u);
        if (ret < 0) {
            if (reason != NULL) {
                *reason = (uint8_t)(-ret);
            }
            return ret;
        }
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

static void anchor_rearm_discovery_ack_after_table_rollback(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    int ret;

    if (snapshot == NULL || !snapshot->valid ||
        snapshot->pending_valid == 0u ||
        snapshot->ack_pending == 0u) {
        return;
    }
    ret = anchor_resume_pending_discovery_assignment_ack(false);
    if (ret < 0) {
        LOG_ERR("anchor prior TABLE ACK rearm after rollback failed: epoch=%u ret=%d",
                snapshot->pending_epoch,
                ret);
    }
}

static int anchor_apply_discovery_assignment_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    struct app_mesh_discovery_assignment_snapshot snapshot = {0};
    struct app_operation_policy_candidate policy_candidate;
    struct operation_policy_set operation_policy;
    struct gateway_command_options assignment_options = {0};
    enum discovery_assignment_phase phase = 0;
    enum app_discovery_assignment_table_decision table_decision;
    uint32_t epoch = 0u;
    struct discovery_assignment_table_commitment table_commitment = {0};
    size_t entry_count = 0u;
    uint8_t slot_count = 0u;
    int ret;

    ret = app_operation_policy_prepare_payload(payload,
                                               payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
                                               &policy_candidate);
    if (ret < 0) {
        return ret;
    }
    operation_policy = policy_candidate.resolved;

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
        assignment_options.flood_epoch_id == 0u) {
        return -EINVAL;
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE) {
        return -EINVAL;
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
        if (ret < 0) {
            return ret;
        }
        if (snapshot.valid &&
            (snapshot.local_id != DEVICE_ID ||
             snapshot.gateway_id != GATEWAY_ID)) {
            return -ESTALE;
        }
        if (snapshot.valid && snapshot.pending_valid != 0u &&
            snapshot.ack_pending != 0u &&
            (epoch == snapshot.pending_epoch ||
             (snapshot.provisioned != 0u &&
              discovery_assignment_epoch_strictly_newer(
                  epoch, snapshot.pending_epoch)))) {
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
            snapshot.provisioned == 0u &&
            discovery_assignment_epoch_strictly_newer(
                epoch, snapshot.pending_epoch)) {
            /*
             * A first-assignment ACK excluded from the gateway's committed
             * roster cannot suppress the next operation.  Drain its exact
             * handle first: a queued DELIVERED terminal still promotes the
             * candidate, while every other classified terminal releases the
             * obsolete response owner before the newer CLAIM is scheduled.
             */
            ret = anchor_settle_ack_before_newer_assignment(epoch);
            if (ret < 0) {
                return ret;
            }
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
        ret = anchor_schedule_discovery_claim(
            packet, epoch, operation_policy.assignment.response_spread_ms);
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
    ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (snapshot.valid &&
        (snapshot.local_id != DEVICE_ID ||
         snapshot.gateway_id != GATEWAY_ID)) {
        return -ESTALE;
    }
    table_decision = local_anchor_discovery_assignment_preview_table(
        epoch, packet->session_id, &table_commitment);
    if ((table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY ||
         table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM) &&
        snapshot.valid && snapshot.pending_valid != 0u &&
        snapshot.ack_pending != 0u &&
        discovery_assignment_epoch_strictly_newer(
            epoch, snapshot.pending_epoch)) {
        enum app_discovery_assignment_claim_decision claim_decision;
        bool claim_observed =
            table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY;
        bool retired_first_assignment =
            snapshot.provisioned == 0u;

        /*
         * The authoritative older pending ACK must be drained before either a
         * correlated newer TABLE or a TABLE-before-CLAIM can replace its live
         * response owner. A queued DELIVERED terminal still promotes; every
         * other terminal retires the obsolete candidate durably. If a newer
         * CLAIM had already been observed, replay that RAM-only fact after
         * restoring the resulting durable state.
         */
        ret = anchor_restore_live_discovery_assignment_snapshot(&snapshot);
        if (ret != PROTO_OK) {
            return -ESTALE;
        }
        ret = anchor_settle_ack_before_newer_assignment(epoch);
        if (ret < 0) {
            return ret;
        }
        ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
        if (ret < 0) {
            return ret;
        }
        if ((!snapshot.valid && !retired_first_assignment) ||
            (snapshot.valid &&
             (snapshot.local_id != DEVICE_ID ||
              snapshot.gateway_id != GATEWAY_ID))) {
            return -ESTALE;
        }
        if (claim_observed) {
            claim_decision =
                local_anchor_discovery_assignment_note_claim(epoch);
            if (claim_decision != APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND) {
                return -ESTALE;
            }
        }
    }
    table_decision = local_anchor_discovery_assignment_note_table(
        epoch, packet->session_id, &table_commitment);
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
        enum app_discovery_assignment_claim_decision claim_decision =
            local_anchor_discovery_assignment_note_claim(epoch);

        if (claim_decision != APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND) {
            (void)anchor_restore_live_discovery_assignment_snapshot(
                &snapshot);
            anchor_rearm_discovery_ack_after_table_rollback(&snapshot);
            return -ESTALE;
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_LATE_CLAIM epoch=%u state=%s count=%u\n",
                            epoch,
                            app_discovery_assignment_provisioning_state_name(
                                local_anchor_discovery_assignment_provisioning_state()),
                            (unsigned int)entry_count);
        LOG_WRN("anchor received discovery table before claim epoch: epoch=%u count=%u; retaining committed assignment and late-claiming",
                epoch,
                (unsigned int)entry_count);
        ret = anchor_schedule_late_discovery_claim(
            packet, epoch, operation_policy.assignment.response_spread_ms);
        if (ret < 0) {
            (void)anchor_restore_live_discovery_assignment_snapshot(
                &snapshot);
            anchor_rearm_discovery_ack_after_table_rollback(&snapshot);
            return ret;
        }
        app_operation_policy_commit_prepared(&policy_candidate);
        return 0;
    }
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY) {
        for (size_t i = 0u; i < entry_count; i++) {
            if (entries[i].anchor_id == DEVICE_ID) {
                ret = anchor_resume_pending_discovery_assignment_ack(true);
                if (ret < 0) {
                    return ret;
                }
                if (ret > 0) {
                    app_operation_policy_commit_prepared(&policy_candidate);
                    return 0;
                }
                status_debug_printf("DBG_DISCOVERY_SLOT_TABLE_REPLAY epoch=%u generation=%u slot=%u\n",
                                    epoch,
                                    packet->session_id,
                                    entries[i].slot);
                ret = anchor_schedule_discovery_response(
                    packet,
                    epoch,
                    DISCOVERY_ASSIGNMENT_PHASE_ACK,
                    &table_commitment,
                    entries[i].slot,
                    slot_count,
                    operation_policy.assignment.response_spread_ms,
                    0u);
                if (ret < 0) {
                    return ret;
                }
                app_operation_policy_commit_prepared(&policy_candidate);
                return 0;
            }
        }
        /*
         * An exact replay of a persisted unassigned generation has no table
         * ACK to send, but it still re-establishes the accepted RAM-only
         * operation profile after a reset.
         */
        app_operation_policy_commit_prepared(&policy_candidate);
        return 0;
    }

    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].anchor_id != DEVICE_ID) {
            continue;
        }
        {
            struct app_mesh_discovery_assignment_snapshot original_snapshot =
                snapshot;
            bool establishing_order;

            if (snapshot.valid &&
                snapshot.pending_valid != 0u &&
                snapshot.ack_pending != 0u &&
                snapshot.pending_epoch == epoch &&
                snapshot.pending_table_command_seq ==
                    packet->session_id &&
                discovery_assignment_table_commitment_equal(
                    &snapshot.pending_table_commitment,
                    &table_commitment) &&
                snapshot.pending_slot == entries[i].slot &&
                snapshot.pending_slot_count == slot_count) {
                ret = anchor_resume_pending_discovery_assignment_ack(true);
                if (ret < 0) {
                    return ret;
                }
                app_operation_policy_commit_prepared(&policy_candidate);
                status_debug_printf(
                    "DBG_DISCOVERY_SLOT_TABLE_RETAIN epoch=%u generation=%u slot=%u\n",
                    epoch,
                    packet->session_id,
                    entries[i].slot);
                return 0;
            }
            if (!snapshot.valid) {
                snapshot.local_id = DEVICE_ID;
                snapshot.gateway_id = GATEWAY_ID;
                snapshot.version =
                    APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION;
                snapshot.valid = true;
            }
            establishing_order = !snapshot.ordered_epoch_valid;
            if (establishing_order) {
                snapshot.epoch = 0u;
                snapshot.table_command_seq = 0u;
                memset(&snapshot.table_commitment,
                       0,
                       sizeof(snapshot.table_commitment));
                snapshot.slot = 0u;
                snapshot.slot_count = 0u;
                snapshot.provisioned = 0u;
            }
            snapshot.ordered_epoch_valid = true;
            snapshot.pending_epoch = epoch;
            snapshot.pending_table_command_seq = packet->session_id;
            snapshot.pending_table_commitment = table_commitment;
            snapshot.table_packet_seq = packet->seq;
            snapshot.response_spread_ms =
                operation_policy.assignment.response_spread_ms;
            snapshot.pending_slot = entries[i].slot;
            snapshot.pending_slot_count = slot_count;
            snapshot.pending_valid = true;
            snapshot.ack_pending = true;
            snapshot.ack_retry_round = 0u;
            if (!local_anchor_discovery_assignment_export_retired_epochs(
                    snapshot.retired_epochs,
                    &snapshot.retired_epoch_count)) {
                (void)anchor_restore_live_discovery_assignment_snapshot(
                    &original_snapshot);
                anchor_rearm_discovery_ack_after_table_rollback(
                    &original_snapshot);
                return -ESTALE;
            }
            if (establishing_order) {
                memset(snapshot.retired_epochs,
                       0,
                       sizeof(snapshot.retired_epochs));
                snapshot.retired_epoch_count = 0u;
            }
            ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
            if (ret < 0) {
                int rollback_ret =
                    anchor_restore_live_discovery_assignment_snapshot(
                        &original_snapshot);

                LOG_WRN("anchor pending discovery assignment persistence failed: epoch=%u slot=%u ret=%d",
                        epoch,
                        entries[i].slot,
                        ret);
                if (rollback_ret != PROTO_OK) {
                    LOG_ERR("anchor pending assignment live rollback failed: epoch=%u ret=%d",
                            epoch,
                            rollback_ret);
                }
                anchor_rearm_discovery_ack_after_table_rollback(
                    &original_snapshot);
                return ret;
            }
        }
        app_operation_policy_commit_prepared(&policy_candidate);
        ret = anchor_schedule_discovery_response(
            packet,
            epoch,
            DISCOVERY_ASSIGNMENT_PHASE_ACK,
            &table_commitment,
            entries[i].slot,
            slot_count,
            operation_policy.assignment.response_spread_ms,
            0u);
        if (ret < 0) {
            return ret;
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_ASSIGNED state=ACK_PENDING epoch=%u slot=%u slot_count=%u anchors=%u hash=0x%016llx\n",
                            epoch,
                            entries[i].slot,
                            slot_count,
                            (unsigned int)entry_count,
                            (unsigned long long)entries[i].hash);
        LOG_INF("anchor discovery slot pending TABLE ACK: epoch=%u slot=%u slot_count=%u anchors=%u",
                epoch,
                entries[i].slot,
                slot_count,
                (unsigned int)entry_count);
        return 0;
    }

    {
        struct app_mesh_discovery_assignment_snapshot original_snapshot =
            snapshot;
        bool establishing_order;

        if (!snapshot.valid) {
            snapshot.local_id = DEVICE_ID;
            snapshot.gateway_id = GATEWAY_ID;
            snapshot.version =
                APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION;
            snapshot.valid = true;
        }
        establishing_order = !snapshot.ordered_epoch_valid;
        if (establishing_order) {
            snapshot.epoch = 0u;
            snapshot.table_command_seq = 0u;
            memset(&snapshot.table_commitment,
                   0,
                   sizeof(snapshot.table_commitment));
            snapshot.slot = 0u;
            snapshot.slot_count = 0u;
            snapshot.provisioned = 0u;
        }
        snapshot.ordered_epoch_valid = true;
        snapshot.pending_epoch = epoch;
        snapshot.pending_table_command_seq = packet->session_id;
        snapshot.pending_table_commitment = table_commitment;
        snapshot.table_packet_seq = 0u;
        snapshot.response_spread_ms = 0u;
        snapshot.pending_slot = 0u;
        snapshot.pending_slot_count = slot_count;
        snapshot.pending_valid = true;
        snapshot.ack_pending = false;
        snapshot.ack_retry_round = 0u;
        if (!local_anchor_discovery_assignment_export_retired_epochs(
                snapshot.retired_epochs,
                &snapshot.retired_epoch_count)) {
            (void)anchor_restore_live_discovery_assignment_snapshot(
                &original_snapshot);
            anchor_rearm_discovery_ack_after_table_rollback(
                &original_snapshot);
            return -ESTALE;
        }
        if (establishing_order) {
            memset(snapshot.retired_epochs,
                   0,
                   sizeof(snapshot.retired_epochs));
            snapshot.retired_epoch_count = 0u;
        }
        ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
        if (ret < 0) {
            int rollback_ret =
                anchor_restore_live_discovery_assignment_snapshot(
                    &original_snapshot);

            if (rollback_ret != PROTO_OK) {
                LOG_ERR("anchor unlisted assignment live rollback failed: epoch=%u ret=%d",
                        epoch,
                        rollback_ret);
            }
            anchor_rearm_discovery_ack_after_table_rollback(
                &original_snapshot);
            return ret;
        }
    }
    /*
     * Absence from an uncommitted replacement table is not proof that the
     * gateway durably removed this anchor. Retain any committed slot across
     * reset and late-claim with the operation epoch; a later included TABLE
     * plus delivered ACK is the only assignment-changing transaction.
     */
    app_operation_policy_commit_prepared(&policy_candidate);
    status_debug_printf("DBG_DISCOVERY_SLOT_UNASSIGNED state=%s epoch=%u count=%u reason=not-listed-retain-committed\n",
                        app_discovery_assignment_provisioning_state_name(
                            local_anchor_discovery_assignment_provisioning_state()),
                        epoch,
                        (unsigned int)entry_count);
    LOG_WRN("anchor not listed in uncommitted discovery table; retaining committed assignment and late-claiming: epoch=%u count=%u",
            epoch,
            (unsigned int)entry_count);
    return anchor_schedule_late_discovery_claim(
        packet, epoch, operation_policy.assignment.response_spread_ms);
}

static int anchor_apply_discovery_assignment_command_serialized(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    int ret;

    k_mutex_lock(&anchor_discovery_assignment_transaction_mutex, K_FOREVER);
    ret = anchor_apply_discovery_assignment_command(
        packet, payload, payload_len);
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
                                               bool *reboot_after_result)
{
    enum device_role requested_role = ROLE_ANCHOR;
    int ret = 0;

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
            packet, payload, payload_len);
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
    } else if (command_id == CMD_SURVEY_GO) {
        ret = app_anchor_survey_runtime_go_round_from_command(packet,
                                                             payload,
                                                             payload_len,
                                                             execution_deadline_ms,
                                                             status,
                                                             reason);
        if (ret < 0 && *status == COMMAND_OK) {
            *status = COMMAND_INTERNAL_ERROR;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_ABORT) {
        struct survey_pair pair = {0};
        uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
        uint16_t round_id = SURVEY_LEGACY_ROUND_ID;

        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
        if (ret == PROTO_OK) {
            int round_ret = survey_round_id_extract_tlv(
                payload, payload_len, &round_id);

            if (round_ret == PROTO_OK) {
                round_ret = survey_round_commitment_extract_tlv(
                    payload, payload_len, round_commitment);
            }
            if (round_ret == PROTO_OK &&
                pair.operation_generation != 0u &&
                packet->session_id == survey_operation_session_id(
                    pair.operation_generation)) {
                (void)app_anchor_survey_runtime_abort_pair_matching_round(
                    &pair,
                    packet->session_id,
                    round_id,
                    round_commitment);
            } else {
                *status = COMMAND_MALFORMED_PAYLOAD;
                *reason = (uint8_t)(
                    round_ret == PROTO_OK ? EPROTO : -round_ret);
            }
        } else {
            *status = COMMAND_MALFORMED_PAYLOAD;
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
    uint32_t execution_deadline_ms)
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
                                              &reboot_after_result);
    if (command_id == CMD_SURVEY_GO && ret < 0) {
        return ret;
    }

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

static int anchor_schedule_broadcast_command_execution(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_command_options *options,
    uint32_t delay_ms)
{
    struct anchor_pending_command_execution previous = {0};
    struct survey_round_go incoming_go = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    bool parked_previous = false;
    bool preempted_previous = false;
    int ret;

    if (packet == NULL || payload == NULL || options == NULL ||
        payload_len > sizeof(anchor_pending_command_execution.payload)) {
        return -EINVAL;
    }
    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    if (command_id == CMD_SURVEY_GO) {
        ret = survey_round_go_from_tlvs(payload,
                                        payload_len,
                                        &incoming_go);
        if (ret != PROTO_OK ||
            packet->session_id != survey_operation_session_id(
                incoming_go.operation_generation) ||
            !app_anchor_survey_runtime_operation_generation_active(
                incoming_go.operation_generation)) {
            return -ESTALE;
        }
    }
    if (anchor_pending_command_execution.active) {
        enum command_id pending_command_id = CMD_VENDOR_BASE;

        if (command_id != CMD_SURVEY_GO ||
            gateway_command_extract_id(
                anchor_pending_command_execution.payload,
                anchor_pending_command_execution.payload_len,
                &pending_command_id) != PROTO_OK) {
            return -EBUSY;
        }
        if (pending_command_id == CMD_SURVEY_GO) {
            struct survey_round_go pending_go = {0};

            if (survey_round_go_from_tlvs(
                    anchor_pending_command_execution.payload,
                    anchor_pending_command_execution.payload_len,
                    &pending_go) != PROTO_OK ||
                incoming_go.operation_generation <=
                    pending_go.operation_generation) {
                return -ESTALE;
            }
        } else if (anchor_deferred_command_execution.active) {
            return -EBUSY;
        }

        previous = anchor_pending_command_execution;
        preempted_previous = true;
        if (pending_command_id != CMD_SURVEY_GO) {
            anchor_deferred_command_execution = previous;
            parked_previous = true;
        }
        memset(&anchor_pending_command_execution,
               0,
               sizeof(anchor_pending_command_execution));
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
        anchor_pending_command_execution.received_at_ms + delay_ms;
    anchor_pending_command_execution.execute_deadline_ms =
        anchor_pending_command_execution.execute_at_ms +
        SURVEY_PAIR_START_SKEW_MARGIN_MS;
    anchor_pending_command_execution.active = true;
    ret = mesh_route_work_reschedule(
        &anchor_command_execute_work,
        delay_ms == 0u ? 1u : delay_ms);
    if (ret < 0) {
        memset(&anchor_pending_command_execution,
               0,
               sizeof(anchor_pending_command_execution));
        if (preempted_previous) {
            anchor_pending_command_execution = previous;
            if (parked_previous) {
                memset(&anchor_deferred_command_execution,
                       0,
                       sizeof(anchor_deferred_command_execution));
            }
            (void)anchor_command_execute_reschedule(
                uptime_deadline_reached(
                    k_uptime_get_32(),
                    anchor_pending_command_execution.execute_at_ms) ?
                    1u :
                    uptime_ms_until_deadline(
                        k_uptime_get_32(),
                        anchor_pending_command_execution.execute_at_ms));
        }
        return ret;
    }
    LOG_INF("anchor broadcast command execution scheduled: command_seq=%u delay_ms=%u",
            options->command_seq,
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
    if (anchor_collection_result_pending.persistence_dirty) {
        ret = anchor_collection_result_persist(1u);
        if (ret < 0) {
            return ret;
        }
        anchor_collection_result_pending.persistence_dirty = false;
        anchor_collection_result_pending.persistence_retry_round = 0u;
    }
    ret = anchor_commit_pending_collection_command_replay();
    if (ret < 0) {
        return ret;
    }
    ret = anchor_checked_work_reschedule(
        &anchor_collection_result_work,
        K_NO_WAIT,
        "pending-command-result-reconcile");
    /*
     * A durable owner survives a failed acceleration handoff and is restored
     * after the scheduler helper forces watchdog recovery.
     */
    return ret < 0 && anchor_collection_result_pending.persistence_dirty ?
        ret : 0;
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

    if (command_id == CMD_SURVEY_GO) {
        if (uptime_deadline_reached(now_ms,
                                    pending.execute_deadline_ms)) {
            LOG_WRN("anchor synchronized survey GO missed local execution window: command_seq=%u now_ms=%u execute_ms=%u deadline_ms=%u",
                    options.command_seq,
                    now_ms,
                    pending.execute_at_ms,
                    pending.execute_deadline_ms);
            ret = anchor_commit_broadcast_command_replay(
                &pending.command, &options, now_ms);
            if (ret == 0) {
                memset(&anchor_pending_command_execution,
                       0,
                       sizeof(anchor_pending_command_execution));
                return;
            }
            anchor_pending_command_execution.terminal_without_execution = true;
            (void)anchor_command_execute_reschedule(
                COLLECTION_RETRY_ROUND_0_MS);
            return;
        }
        if (!uptime_deadline_reached(now_ms, pending.execute_at_ms)) {
            (void)anchor_command_execute_reschedule(
                uptime_ms_until_deadline(now_ms,
                                         pending.execute_at_ms));
            return;
        }
    }

    if (options.collection_required &&
        anchor_collection_result_pending.active) {
        (void)anchor_command_execute_reschedule(
            COLLECTION_RETRY_ROUND_0_MS);
        return;
    }

    if (!options.collection_required && command_id != CMD_SURVEY_GO) {
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
                                          pending.execute_deadline_ms);
    if (ret < 0) {
        if (options.collection_required &&
            anchor_collection_result_matches_broadcast_command(
                &pending.command, &options, command_id)) {
            /*
             * Result persistence may fail after the exact result owner has
             * already been installed.  That owner retries persistence and
             * commits replay; retaining the execution owner would run the
             * side effect a second time after the result slot drains.
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
    if (command_id == CMD_SURVEY_GO) {
        ret = anchor_commit_broadcast_command_replay(
            &pending.command, &options, now_ms);
        if (ret < 0) {
            LOG_WRN("anchor delayed result replay commit deferred: command_seq=%u ret=%d",
                    options.command_seq,
                    ret);
            anchor_pending_command_execution.side_effect_completed = true;
            (void)anchor_command_execute_reschedule(
                COLLECTION_RETRY_ROUND_0_MS);
            return;
        }
    }
    if (options.collection_required) {
        /*
         * The durable collection-result owner retries this replay commit if
         * it fails here, so execution ownership may retire safely.
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
    uint32_t delay_ms;
    uint32_t now_ms;

    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    anchor_command_execute_work_handler_locked(work);
    if (!anchor_pending_command_execution.active &&
        anchor_deferred_command_execution.active) {
        anchor_pending_command_execution =
            anchor_deferred_command_execution;
        memset(&anchor_deferred_command_execution,
               0,
               sizeof(anchor_deferred_command_execution));
        now_ms = k_uptime_get_32();
        delay_ms = uptime_deadline_reached(
            now_ms,
            anchor_pending_command_execution.execute_at_ms) ?
            1u :
            uptime_ms_until_deadline(
                now_ms,
                anchor_pending_command_execution.execute_at_ms);
        (void)anchor_command_execute_reschedule(delay_ms);
        LOG_INF("anchor resumed delayed broadcast after synchronized survey GO: command_seq=%u delay_ms=%u",
                anchor_pending_command_execution.options.command_seq,
                delay_ms);
    }
    k_mutex_unlock(&anchor_command_result_mutex);
}

static int anchor_handle_local_command_locked(const struct proto_packet *packet,
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
    uint32_t protocol_reservation_token = 0u;
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
                    packet, payload, payload_len);
                if (ret < 0) {
                    LOG_WRN("anchor rejected duplicate discovery assignment command: command_seq=%u ret=%d",
                            command_options.command_seq,
                            ret);
                }
                return ret < 0 ? 0 : 1;
            }
            LOG_INF("anchor ignored duplicate broadcast command: cmd=0x%04x command_seq=%u",
                    (unsigned int)command_id,
                    command_options.command_seq);
            return 1;
        }

        if (command_id == CMD_SURVEY_GO &&
            (command_options.execute_delay_ms == 0u ||
             packet->message_age_ms >= command_options.execute_delay_ms)) {
            LOG_WRN("anchor ignored late or immediate survey GO: command_seq=%u age_ms=%u delay_ms=%u",
                    command_options.command_seq,
                    packet->message_age_ms,
                    command_options.execute_delay_ms);
            ret = anchor_commit_broadcast_command_replay(
                packet, &command_options, now_ms);
            return ret == 0 ? 1 : 0;
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

        if (!command_options.collection_required) {
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
                                              0u);
        if (ret == 0 && command_options.collection_required) {
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
        &protocol_reservation_token);
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
        &reboot_after_result);

    completion_actions = anchor_command_completion_actions(
        status, force_rediscovery_after_result, reboot_after_result);
    if (completion_actions != 0u) {
        if (anchor_collection_result_pending.active) {
            (void)app_node_comm_cancel_protocol_response_reservation(
                protocol_reservation_token);
            LOG_WRN("anchor deferred action-bearing command behind durable result owner: cmd=0x%04x",
                    (unsigned int)command_id);
            return 0;
        }
        ret = anchor_admit_direct_action_result(
            packet,
            command_id,
            status,
            reason,
            completion_actions,
            protocol_reservation_token);
        if (ret < 0) {
            LOG_WRN("anchor direct action result durable admission deferred: cmd=0x%04x ret=%d",
                    (unsigned int)command_id,
                    ret);
            return 0;
        }
        LOG_INF("anchor action-bearing command accepted with durable result custody: cmd=0x%04x",
                (unsigned int)command_id);
        return 1;
    }
    wait_for_delivery_completion =
        command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK;
    ret = anchor_submit_command_result(packet,
                                       command_id,
                                       status,
                                       reason,
                                       NULL,
                                       0u,
                                       protocol_reservation_token,
                                       wait_for_delivery_completion ?
                                           &delivery_handle : NULL);
    if (ret < 0) {
        (void)app_node_comm_cancel_protocol_response_reservation(
            protocol_reservation_token);
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
            (void)app_anchor_survey_runtime_cancel_pair_start(packet);
        }
        return 0;
    }
    if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
        int bind_ret = app_anchor_survey_runtime_bind_pair_start_delivery(
            packet, delivery_handle);

        if (bind_ret < 0 && bind_ret != -ESTALE) {
            LOG_WRN("survey pair start delivery binding failed: ret=%d",
                    bind_ret);
        }
        if (bind_ret < 0) {
            int abandon_ret =
                app_anchor_survey_runtime_abandon_pair_start_delivery(
                    delivery_handle, "pair-start-bind-failed");

            if (abandon_ret < 0) {
                LOG_ERR("survey pair start delivery binding cleanup failed: handle=%u ret=%d",
                        delivery_handle,
                        abandon_ret);
            }
            (void)app_anchor_survey_runtime_cancel_pair_start(packet);
        }
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
    return 1;
}

static int anchor_handle_local_command(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len)
{
    int ret;

    k_mutex_lock(&anchor_command_result_mutex, K_FOREVER);
    ret = anchor_handle_local_command_locked(packet, payload, payload_len);
    k_mutex_unlock(&anchor_command_result_mutex);
    return ret;
}
