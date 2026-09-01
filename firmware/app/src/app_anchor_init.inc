#line 8793 __BASE_FILE__
int app_anchor_init(void)
{
#if defined(CONFIG_IMEC_ML_ANCHOR) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    /*
     * The ML anchor is a deterministic full-duty ranging endpoint.  It does
     * not participate in connected-mesh routing, discovery
     * assignment, or gateway command execution, so initializing those owners
     * would retain their queues and custody stores without a consumer.
     */
    return 0;
#else
    int ret;
    {
        const struct app_survey_ops survey_runtime_ops = {
#if DEVICE_ROLE == ROLE_GATEWAY
            .send_control = gateway_survey_send_control,
            .control_origin = gateway_survey_control_origin,
            .control_detach = gateway_survey_control_detach,
            .control_abandon = gateway_survey_control_abandon,
            .emit_event = gateway_survey_emit_event,
            .wake_gateway_rx = gateway_survey_wake_rx,
            .gateway_terminal = gateway_survey_terminal,
#elif DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
            .anchor_upstream = anchor_survey_upstream,
            .anchor_consume_enumeration_handoff =
                anchor_enumeration_rx_consume_survey_handoff,
            .anchor_reschedule = anchor_survey_work_reschedule,
#endif
        };

        ret = app_survey_init(&survey_runtime_ops);
        if (ret < 0) {
            return ret;
        }
    }
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
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
#endif
#if DEVICE_ROLE == ROLE_GATEWAY
    {
        const struct app_gateway_assignment_publisher_ops publisher_ops = {
            .emit_if_available = gateway_publish_assignment_event_if_available,
            .reserve_event_seq = gateway_reserve_command_event_sequence,
            .batch_completed = gateway_complete_assignment_publication,
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
#endif
}

#if defined(CONFIG_IMEC_ML_ANCHOR) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
static int app_ml_anchor_start_role(void)
{
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = DEVICE_ID,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
    };
    int ret;

    ret = uwb_anchor_session_init(&anchor_uwb_session, &anchor_config);
    if (ret < 0) {
        LOG_ERR("ML anchor UWB session init failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&anchor_uwb_scan_work, anchor_uwb_scan_work_handler);
    k_work_init(&anchor_click_handoff_work,
                anchor_click_handoff_work_handler);
    k_work_init_delayable(&anchor_heartbeat_work,
                          anchor_heartbeat_work_handler);
    k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);

    ret = app_ml_init();
    if (ret < 0) {
        LOG_ERR("ML anchor runtime init failed: %d", ret);
        return ret;
    }

    k_work_queue_start(&anchor_uwb_scan_work_q,
                       anchor_uwb_scan_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_uwb_scan_work_q_stack),
                       ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
                       &anchor_uwb_scan_work_q_config);
    ret = anchor_start_uwb_scan();
    if (ret < 0) {
        LOG_ERR("ML anchor UWB scan unavailable: %d", ret);
        return ret;
    }
    return 0;
}
#endif

int app_anchor_start_anchor_role(void)
{
#if defined(CONFIG_IMEC_ML_ANCHOR) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    return app_ml_anchor_start_role();
#else
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = DEVICE_ID,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
    };
    int ret;

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
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_init_delayable(
        &anchor_discovery_ack_liveness_work,
        anchor_discovery_ack_liveness_work_handler);
#endif
    ret = anchor_resume_pending_discovery_assignment_ack(false);
    if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP) {
        LOG_ERR("anchor pending TABLE ACK resume failed closed: %d", ret);
        return ret;
    }
    /*
     * Command-result and post-result action custody is intentionally RAM-only.
     * Reset aborts an unconfirmed result and the gateway must redrive it under
     * the new boot incarnation. If reset follows result confirmation, reset
     * itself completes CMD_REBOOT and clears route state for
     * CMD_FORCE_REDISCOVERY, so no false flash-backed action debt is required.
     */
    anchor_collection_result_start_ram_owner();
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
    ret = anchor_start_uwb_scan();
    if (ret < 0) {
        LOG_ERR("anchor UWB scan unavailable: %d", ret);
        return ret;
    }
    anchor_heartbeat_request_startup();
    return 0;
#endif
}

int app_anchor_start_gateway_role(void)
{
    uint32_t boot_incarnation = 0u;
    int ret;

#if defined(CONFIG_IMEC_DURABLE_STATE)
    ret = app_durable_state_boot_incarnation(&boot_incarnation);
    if (ret < 0) {
        LOG_ERR("gateway boot incarnation unavailable: %d", ret);
        return ret;
    }
#else
    LOG_ERR("gateway role requires durable boot incarnation");
    return -ENOTSUP;
#endif
    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_GATEWAY,
                    DEVICE_ID,
                    GATEWAY_ID,
                    boot_incarnation);
    ret = app_mesh_report_attach_gateway_ack_store();
    if (ret < 0) {
        LOG_ERR("gateway ACK store attach failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);
    return 0;
}
