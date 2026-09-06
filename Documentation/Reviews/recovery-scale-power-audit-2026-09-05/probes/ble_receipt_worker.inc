static void gateway_ble_rx_work_handler(struct k_work *work)
{
    struct gateway_ble_frame_pending pending;

    ARG_UNUSED(work);

#if DEVICE_ROLE == ROLE_GATEWAY
    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
        status_debug_printf(
            "DBG_GATEWAY_BLE_RX_WORK stage=enter q=%u receipts=%d\n",
            (unsigned int)k_msgq_num_used_get(&gateway_ble_rx_msgq),
            (int)atomic_get(&gateway_ble_host_receipt_ingress_count));
    }
#endif

    if (!gateway_ble_transport_enabled()) {
        return;
    }

    for (;;) {
        uint32_t result_reservation_token = 0u;
        int ret;

#if DEVICE_ROLE == ROLE_GATEWAY
        /* Host receipts must bypass result-credit admission, or a full
         * command-result pool can strand the exact item they release. Peek
         * first, then dequeue/consume receipts with token zero. */
        ret = k_msgq_peek(&gateway_ble_rx_msgq, &pending);
        if (ret < 0) {
            break;
        }
        if (gateway_ble_pending_is_host_receipt(&pending)) {
            ret = k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
            if (ret < 0) {
                break;
            }
            (void)gateway_handle_ble_frame(pending.frame, pending.len, 0u);
            atomic_dec(&gateway_ble_host_receipt_ingress_count);
            if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
                status_debug_printf(
                    "DBG_GATEWAY_BLE_RX_WORK stage=receipt q=%u receipts=%d\n",
                    (unsigned int)k_msgq_num_used_get(&gateway_ble_rx_msgq),
                    (int)atomic_get(
                        &gateway_ble_host_receipt_ingress_count));
            }
            continue;
        }
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
        ret = gateway_command_result_reserve_ingress(
            &result_reservation_token);
        if (ret < 0) {
            if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
                status_debug_printf(
                    "DBG_GATEWAY_BLE_RX_WORK stage=reserve ret=%d q=%u receipts=%d\n",
                    ret,
                    (unsigned int)k_msgq_num_used_get(&gateway_ble_rx_msgq),
                    (int)atomic_get(
                        &gateway_ble_host_receipt_ingress_count));
            }
            if (ret != -ENOSPC) {
                LOG_ERR("gateway command result admission failed: %d", ret);
                gateway_ble_schedule_failed("rx-result-admission", ret);
            }
            break;
        }
#endif
        ret = k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
        if (ret < 0) {
            if (result_reservation_token != 0u) {
                gateway_command_result_release_ingress(
                    result_reservation_token);
            }
            break;
        }
        if (!gateway_handle_ble_frame(pending.frame,
                                      pending.len,
                                      result_reservation_token) &&
            result_reservation_token != 0u) {
            gateway_command_result_release_ingress(
                result_reservation_token);
        }
    }
}
