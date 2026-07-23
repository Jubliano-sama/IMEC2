#line 8793 __BASE_FILE__
int app_anchor_init(void)
{
    const struct app_anchor_command_completion_ops completion_ops = {
        .force_rediscovery = anchor_force_rediscovery_from_command,
        .schedule_reboot = anchor_schedule_reboot_after_command_result,
    };
    const struct app_anchor_survey_runtime_ops runtime_ops = {
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
        .work_queue = &anchor_uwb_scan_work_q,
#endif
        .send_command_result = anchor_send_command_result,
        .enter_low_power = anchor_enter_low_power,
        .set_uwb_busy = anchor_set_uwb_busy,
        .note_uwb_awake_since = anchor_note_uwb_awake_since,
        .start_uwb_scan = anchor_start_uwb_scan,
        .queue_sample_result = anchor_queue_survey_sample_result,
        .report_queue_used = report_tx_queue_used,
        .report_schedule = report_tx_schedule,
        .relay_tx_active = anchor_relay_tx_active,
        .connected_radio_active = mesh_anchor_connected_radio_active,
    };
    const struct app_anchor_survey_discovery_ops discovery_ops = {
        .abort_requested = app_anchor_survey_runtime_abort_requested,
        .abort_pair = app_anchor_survey_runtime_abort_pair,
        .preempt_radio = anchor_preempt_for_survey_discovery,
        .admit_start = app_anchor_survey_runtime_admit_discovery,
        .queue_start = app_anchor_survey_runtime_queue_discovery,
        .schedule_work_ms = app_anchor_survey_runtime_schedule_ms,
        .next_sequence = app_anchor_survey_runtime_next_sequence,
        .seed_sequence = app_anchor_survey_runtime_seed_sequence,
    };
    int ret;

    ret = app_anchor_command_completion_init(&completion_ops);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_survey_runtime_init(&runtime_ops);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_survey_discovery_init(&discovery_ops);
    if (ret < 0) {
        return ret;
    }
#if DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY
    /* Mount persistence before any mesh work can query deferred custody.  The
     * scheduler's presence accessor is intentionally cache-only, so a failed
     * mount remains visible as a retryable error instead of doing flash I/O
     * from the system workqueue. */
    ret = app_mesh_persistence_init();
    if (ret < 0 && ret != -ENOTSUP) {
        LOG_WRN("mesh persistence startup mount unavailable: %d", ret);
    }
#endif
    k_work_init_delayable(&gateway_survey_work, gateway_survey_work_handler);
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
    if (gateway_host_command_msgq.max_msgs != GATEWAY_HOST_COMMAND_QUEUE_DEPTH) {
        return -EINVAL;
    }
    ret = app_gateway_command_lifecycle_init(&gateway_host_command_lifecycle,
                                             GATEWAY_HOST_COMMAND_QUEUE_DEPTH);
    if (ret < 0) {
        return ret;
    }
    k_work_init_delayable(&gateway_host_command_work,
                          gateway_host_command_work_handler);
    k_work_init_delayable(&gateway_host_command_retry_work,
                          gateway_host_command_retry_work_handler);
    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        gateway_host_command_schedule_failed, NULL);
#endif
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
#if DEVICE_ROLE == ROLE_GATEWAY
    {
        const struct app_gateway_assignment_publisher_ops publisher_ops = {
            .emit_if_available = gateway_observe_command_event_if_available,
        };

        ret = app_gateway_assignment_publisher_init(&publisher_ops);
        if (ret < 0) {
            return ret;
        }
    }
    k_work_init_delayable(&gateway_discovery_assignment_finalize_work,
                          gateway_discovery_assignment_finalize_work_handler);
    k_work_init_delayable(&gateway_discovery_assignment_publish_work,
                          gateway_discovery_assignment_publish_work_handler);
    app_discovery_assignment_work_guard_init(
        &gateway_discovery_assignment_publish_guard);
#endif
    return 0;
}

int app_anchor_start_anchor_role(void)
{
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = DEVICE_ID,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
    };
    int delivery_bind_ret = 0;
    int ret;
    bool delivery_restored = false;

    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_ANCHOR,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    ret = app_mesh_report_attach_anchor_downlink_store();
    if (ret < 0) {
        LOG_ERR("anchor downlink store attach failed: %d", ret);
        return ret;
    }
    ret = app_mesh_persistence_restore_outbox(&mesh_runtime, k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh outbox restore unavailable: %d", ret);
    }
    if (mesh_relay_tx_active(&mesh_runtime)) {
        ret = app_mesh_persistence_complete_deferred_outbox(
            &mesh_runtime,
            k_uptime_get_32());
        if (ret < 0 && ret != -ENOTSUP) {
            LOG_WRN("anchor deferred outbox cleanup unavailable: %d", ret);
        }
    } else {
        ret = app_mesh_persistence_restore_deferred_outbox(
            &mesh_runtime,
            k_uptime_get_32());
        if (ret < 0 && ret != -ENOTSUP) {
            LOG_WRN("anchor deferred outbox restore unavailable: %d", ret);
        }
    }
    ret = app_anchor_survey_discovery_restore(&delivery_restored);
    if (ret < 0) {
        return ret;
    }
    ret = app_mesh_persistence_restore_child_custody(&mesh_runtime,
                                                    k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh child custody restore unavailable: %d", ret);
    }
    if (anchor_discovery_assignment_required()) {
        struct app_mesh_discovery_assignment_snapshot snapshot = {0};
        const char *unprovisioned_source = "erased-nvs";
        int restore_ret;

        local_anchor_reset_discovery_assignment();
        restore_ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
        if (restore_ret == 0 && snapshot.valid &&
            snapshot.local_id == DEVICE_ID && snapshot.gateway_id == GATEWAY_ID) {
            ret = local_anchor_restore_discovery_assignment(snapshot.epoch,
                                                            snapshot.table_command_seq,
                                                            snapshot.table_fingerprint,
                                                            snapshot.slot,
                                                            snapshot.slot_count,
                                                            snapshot.provisioned);
            if (ret == PROTO_OK) {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=%s source=nvs epoch=%u generation=%u slot=%u slots=%u\n",
                                    snapshot.provisioned ? "PROVISIONED" : "UNPROVISIONED",
                                    snapshot.epoch,
                                    snapshot.table_command_seq,
                                    snapshot.slot,
                                    snapshot.slot_count);
                LOG_INF("anchor discovery assignment restored: state=%s epoch=%u generation=%u slot=%u slot_count=%u",
                        snapshot.provisioned ? "PROVISIONED" : "UNPROVISIONED",
                        snapshot.epoch,
                        snapshot.table_command_seq,
                        snapshot.slot,
                        snapshot.slot_count);
            } else {
                unprovisioned_source = "invalid-nvs";
                app_mesh_persistence_clear_discovery_assignment();
                local_anchor_reset_discovery_assignment();
            }
        } else if (restore_ret == 0) {
            unprovisioned_source = "invalid-nvs";
            app_mesh_persistence_clear_discovery_assignment();
        } else if (restore_ret == -ENOTSUP) {
            unprovisioned_source = "nvs-disabled";
        } else if (restore_ret != -ENOENT) {
            unprovisioned_source = "nvs-error";
            LOG_WRN("anchor discovery assignment restore unavailable: %d",
                    restore_ret);
        }
        if (local_anchor_discovery_assignment_provisioning_state() ==
            APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
            status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED source=%s restore=%d reply=disabled\n",
                                unprovisioned_source,
                                restore_ret);
            LOG_WRN("anchor discovery assignment UNPROVISIONED: source=%s restore=%d; normal click replies disabled until gateway assignment",
                    unprovisioned_source,
                    restore_ret);
        }
    }
    ret = uwb_anchor_session_init(&anchor_uwb_session, &anchor_config);
    if (ret < 0) {
        LOG_ERR("anchor UWB session init failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&anchor_uwb_scan_work, anchor_uwb_scan_work_handler);
    k_work_init(&anchor_click_handoff_work,
                anchor_click_handoff_work_handler);
    k_work_init_delayable(&anchor_heartbeat_work, anchor_heartbeat_work_handler);
    k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);
    ret = app_anchor_survey_runtime_start();
    if (ret < 0) {
        return ret;
    }
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
    if (mesh_relay_tx_active(&mesh_runtime)) {
        app_mesh_persistence_clear_collection_result();
    } else {
        ret = anchor_collection_result_restore();
        if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP) {
            LOG_WRN("anchor collection result restore unavailable: %d", ret);
        }
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (gateway_ble_transport_enabled()) {
        status_debug_note("DBG_ANCHOR_BLE_INIT_BEGIN\n");
        ret = gateway_ble_init();
        status_debug_note("DBG_ANCHOR_BLE_INIT_DONE\n");
        if (ret < 0) {
            LOG_ERR("mesh-test anchor BLE debug log link unavailable: %d", ret);
        } else {
            LOG_INF("mesh-test anchor BLE debug log link ready before UWB scan");
        }
    } else {
        status_debug_note("DBG_ANCHOR_BLE_DISABLED\n");
    }
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
    (void)app_ml_init();
#endif
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    k_work_queue_start(&anchor_uwb_scan_work_q,
                       anchor_uwb_scan_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_uwb_scan_work_q_stack),
                       ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
                       &anchor_uwb_scan_work_q_config);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_ANCHOR_CH5_SCAN_WQ_STARTED\n");
    }
#endif
    if (delivery_restored) {
        delivery_bind_ret = app_anchor_survey_discovery_retry_report();
        if (delivery_bind_ret < 0 && delivery_bind_ret != -EAGAIN) {
            LOG_WRN("restored survey delivery bind deferred: %d",
                    delivery_bind_ret);
        }
        app_anchor_survey_runtime_schedule_ms(0u);
    }
    if (!delivery_restored || delivery_bind_ret == 0) {
        mesh_report_resume_restored_outbox("anchor-startup-owned");
    } else {
        status_debug_printf(
            "DBG_OUTBOX_RESTORE_WAIT_SURVEY_OWNER ret=%d\n",
            delivery_bind_ret);
    }
    ret = anchor_start_uwb_scan();
    if (ret < 0) {
        LOG_ERR("anchor UWB scan unavailable: %d", ret);
    }
    return 0;
}

int app_anchor_start_gateway_role(void)
{
    int ret;

    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_GATEWAY,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    ret = app_mesh_report_attach_gateway_ack_store();
    if (ret < 0) {
        LOG_ERR("gateway ACK store attach failed: %d", ret);
        return ret;
    }
    return 0;
}
