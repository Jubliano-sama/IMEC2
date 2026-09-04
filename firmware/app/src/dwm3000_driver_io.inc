#line 3038 __BASE_FILE__
int dwm3000_driver_probe(uint32_t *dev_id)
{
    uint32_t read_id;
    int ret;

    if (dev_id == NULL) {
        return -EINVAL;
    }

    /*
     * Probe callers may have reset the IC out of band. Never carry a prior
     * configured/sleep-retention claim across an identity probe; success
     * proves only that the reset device answered at slow SPI.
     */
    invalidate_radio_state_tagged(__func__);
    ret = dwm3000_port_init();
    if (ret < 0) {
        return ret;
    }

    ret = ensure_local_data();
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_port_set_slow_spi();
    if (ret < 0) {
        return ret;
    }
    dwm3000_port_clear_error();
    read_id = dwt_readdevid();
    ret = take_port_error("probe");
    if (ret < 0) {
        return ret;
    }
    if (!dwm3000_port_dev_id_supported(read_id)) {
        return -ENODEV;
    }

    *dev_id = read_id;
    mark_radio_awake_unconfigured_tagged(__func__);
    return 0;
}

int dwm3000_driver_configure_default(void)
{
    int ret = configure_radio_from_reset(DWM3000_PHY_RANGE);

    if (ret < 0) {
        return ret;
    }
    LOG_INF("DWM3000 configured for channel %u no-STS DS-TWR at 850 kbps, %u Hz SPI; polling SYS_STATUS",
            DWM3000_PHY_CHANNEL,
            (unsigned int)dwm3000_port_current_spi_hz());
    return 0;
}

int dwm3000_driver_configure_wake_mode(void)
{
    int ret;

    /* Same-PHY re-entry reuses the active configuration; ensure_phy_mode()
     * still takes the from-reset path for cross-PHY changes and after any
     * invalidation.  A full re-init here cost ~30 ms per ACK and per RX
     * slice on the gateway. */
    ret = ensure_phy_mode(DWM3000_PHY_WAKE);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_RESET_FAIL ret=%d\n", ret);
        }
        return ret;
    }

    LOG_DBG("DWM3000 configured for UWB wake/discovery mode: sfd_timeout_symbols=%u",
            (unsigned int)wake_config.sfdTO);
    return 0;
}

int dwm3000_driver_ensure_wake_mode(void)
{
    return ensure_phy_mode(DWM3000_PHY_WAKE);
}

int dwm3000_driver_configure_range_mode(void)
{
    return ensure_phy_mode(DWM3000_PHY_RANGE);
}

int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    /*
     * The gateway re-enters this PHY before every receive arm and every
     * causal ACK.  Sample the state that decides whether ensure_phy_mode()
     * actually touches the radio, so the trace reports real transitions
     * instead of adding a line to the debug channel on every arm; a silent
     * success is by construction a no-op.
     */
    bool transition = !radio_configured || !radio_awake ||
                      radio_state_unknown || radio_restored_from_sleep ||
                      active_phy_mode != DWM3000_PHY_WAKE_MESH_CONTROL;
    int ret;

    /* Same-PHY re-entry reuses the active configuration; ensure_phy_mode()
     * still takes the from-reset path for cross-PHY changes and after any
     * invalidation.  A full re-init here cost ~30 ms per ACK and per RX
     * slice on the gateway. */
    ret = ensure_phy_mode(DWM3000_PHY_WAKE_MESH_CONTROL);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_MESH_CTRL_FAIL ret=%d\n", ret);
        }
        return ret;
    }

    if (!transition) {
        return 0;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_WAKE_MESH_CTRL_OK sfd=%u\n",
                            (unsigned int)wake_mesh_control_config.sfdTO);
    }
    LOG_INF("DWM3000 configured for channel %u extended-PHR mesh control at 850 kbps",
            UWB_CHANNEL_WAKE_CONTACT);
    return 0;
}

int dwm3000_driver_configure_mesh_payload_mode(void)
{
    int ret;

    ret = ensure_phy_mode(DWM3000_PHY_MESH_PAYLOAD);
    if (ret < 0) {
        return ret;
    }
#if DWM3000_MESH_PHY_STS_MODE == DWT_STS_MODE_OFF
    LOG_INF("DWM3000 configured for channel %u 1024-symbol no-STS mesh payload at 850 kbps",
            UWB_CHANNEL_MESH_PAYLOAD);
#else
    LOG_INF("DWM3000 configured for channel %u 1024-symbol STS-mode-1 mesh payload at 850 kbps",
            UWB_CHANNEL_MESH_PAYLOAD);
#endif
    return 0;
}

int dwm3000_driver_idle(void)
{
    uint32_t trace_id = radio_transition_trace_next();
    int ret;

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_IDLE id=%u step=entry tid=%p configured=%u awake=%u unknown=%u phy=%u spi=%u\n",
        trace_id, k_current_get(), radio_configured ? 1u : 0u,
        radio_awake ? 1u : 0u, radio_state_unknown ? 1u : 0u,
        (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    if (radio_state_unknown || (radio_awake && !radio_configured)) {
        ret = configure_radio_from_reset(DWM3000_PHY_RANGE);
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_IDLE id=%u step=recover ret=%d\n", trace_id, ret);
        if (ret < 0) {
            return ret;
        }
    }
    if (!radio_awake) {
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_IDLE id=%u step=already_asleep\n", trace_id);
        return 0;
    }
    if (!radio_configured) {
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_IDLE id=%u step=unconfigured ret=%d\n",
            trace_id, -EIO);
        return -EIO;
    }

    dwt_forcetrxoff();
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();
    ret = take_port_error("idle");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_IDLE id=%u step=port_error ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }
    ret = check_device_fatal_status("idle");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_IDLE id=%u step=done ret=%d configured=%u awake=%u unknown=%u phy=%u spi=%u\n",
        trace_id, ret, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u, (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    return ret;
}

int dwm3000_driver_standby(void)
{
    uint32_t trace_id = radio_transition_trace_next();
    int ret;

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=entry tid=%p configured=%u awake=%u unknown=%u phy=%u spi=%u\n",
        trace_id, k_current_get(), radio_configured ? 1u : 0u,
        radio_awake ? 1u : 0u, radio_state_unknown ? 1u : 0u,
        (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    if (radio_state_unknown || (radio_awake && !radio_configured)) {
        ret = configure_radio_from_reset(DWM3000_PHY_RANGE);
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_STANDBY id=%u step=recover ret=%d\n", trace_id, ret);
        if (ret < 0) {
            return ret;
        }
    }
    if (!radio_awake) {
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_STANDBY id=%u step=already_asleep\n", trace_id);
        return 0;
    }
    if (!radio_configured) {
        DWM3000_TRANSITION_TRACE(
            "DBG_DWM_STANDBY id=%u step=unconfigured ret=%d\n",
            trace_id, -EIO);
        return -EIO;
    }

    ret = dwm3000_driver_idle();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=idle ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_port_set_slow_spi();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=slow_spi ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        invalidate_radio_state_tagged(__func__);
        return ret;
    }
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=sleep_commands_begin\n", trace_id);
    dwt_configuresleep(DWM3000_SLEEP_MODE, DWM3000_SLEEP_WAKE_FLAGS);
    dwt_entersleep(DWT_DW_IDLE_RC);
    /*
     * A port error after dwt_entersleep() cannot prove that the device stayed
     * awake.  Fail closed so the next user takes the wake/recovery path.
     */
    radio_awake = false;
    radio_state_unknown = true;
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=sleep_commands_end configured=%u awake=%u unknown=%u\n",
        trace_id, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u);
    ret = take_port_error("standby");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=port_error ret=%d configured=%u awake=%u unknown=%u\n",
        trace_id, ret, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u);
    if (ret < 0) {
        return ret;
    }
    radio_state_unknown = false;
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_STANDBY id=%u step=done configured=%u awake=%u unknown=%u phy=%u spi=%u\n",
        trace_id, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u, (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    LOG_INF("DWM3000 entered sleep with retained config; wakeup pin required before next UWB window");
    return 0;
}

int dwm3000_driver_force_recovery(void)
{
    enum dwm3000_phy_mode recover_phy =
        active_phy_mode == DWM3000_PHY_NONE ?
        DWM3000_PHY_RANGE : active_phy_mode;
    int ret;

    invalidate_radio_state_tagged(__func__);
    dwm3000_port_clear_error();
    driver_stats.radio_recoveries++;
    ret = configure_radio_from_reset(recover_phy);
    if (ret < 0) {
        LOG_ERR("DWM3000 forced recovery failed: phy=%d ret=%d",
                recover_phy,
                ret);
    } else {
        LOG_WRN("DWM3000 forced recovery completed: phy=%d", recover_phy);
    }
    return ret;
}

int dwm3000_driver_send_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t timeout_ms)
{
    return dwm3000_driver_send_frame_tracked(frame, frame_len, timeout_ms,
                                             NULL);
}

int dwm3000_driver_send_frame_tracked(const uint8_t *frame,
                                      size_t frame_len,
                                      uint32_t timeout_ms,
                                      bool *rf_started)
{
    struct dwm3000_tx_observation observation = {0};
    int ret;

    ret = dwm3000_driver_send_frame_tracked_until(
        frame, frame_len, timeout_ms, 0u, &observation);
    if (rf_started != NULL) {
        *rf_started = observation.rf_started;
    }
    return ret;
}

int dwm3000_driver_send_frame_tracked_until(
    const uint8_t *frame,
    size_t frame_len,
    uint32_t timeout_ms,
    uint64_t absolute_deadline_ms,
    struct dwm3000_tx_observation *observation)
{
    struct dwm3000_tx_observation result = {0};
    uint64_t now_ms;
    int ret;

    if (observation != NULL) {
        *observation = result;
    }
    if (frame == NULL || frame_len == 0u) {
        return -EINVAL;
    }
    if (frame_len > UWB_PHY_EXTENDED_PAYLOAD_MAX_LEN) {
        return -EMSGSIZE;
    }

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        LOG_WRN("DWM3000 send-frame PHY wake/restore failed: ret=%d frame_len=%u",
                ret,
                (unsigned int)frame_len);
        return ret;
    }

    clear_all_events();
    ret = take_port_error("send-frame-clear");
    if (ret < 0) {
        return ret;
    }
    now_ms = (uint64_t)k_uptime_get();
    if (absolute_deadline_ms != 0u && now_ms >= absolute_deadline_ms) {
        return -ETIMEDOUT;
    }
    ret = send_range_frame_until(frame,
                                 frame_len,
                                 DWT_START_TX_IMMEDIATE,
                                 absolute_deadline_ms,
                                 &result.rf_started,
                                 &result.rf_started_at_ms);
    if (ret < 0) {
        if (observation != NULL) {
            *observation = result;
        }
        LOG_WRN("DWM3000 send-frame immediate TX failed: ret=%d frame_len=%u active_phy=%d awake=%u restored=%u",
                ret,
                (unsigned int)frame_len,
                active_phy_mode,
                radio_awake ? 1u : 0u,
                radio_restored_from_sleep ? 1u : 0u);
        return ret;
    }

    ret = wait_tx_complete_observed(
        timeout_ms == 0u ? REPORT_RX_TIMEOUT_MS : timeout_ms,
        &result.tx_completed_at_ms);
    result.tx_completed = ret == 0;
    if (observation != NULL) {
        *observation = result;
    }
    return ret;
}

int dwm3000_driver_receive_frame(uint32_t timeout_ms,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 size_t *frame_len,
                                 uint8_t *quality,
                                 int8_t *rsl_dbm)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms,
                                                 frame,
                                                 frame_cap,
                                                 frame_len,
                                                 quality,
                                                 rsl_dbm,
                                                 NULL);
}

/*
 * Bound on same-window receiver re-enables.  A single mesh-control window is
 * hundreds of milliseconds and a foreign frame costs milliseconds, so this
 * only stops a pathological status storm from spinning without ever handing
 * control back to the caller's own recovery.
 */
#define DWM3000_RX_CHEAP_REARM_MAX 16u

static bool rx_error_allows_cheap_reenable(bool continuous,
                                           int ret,
                                           uint32_t status,
                                           uint16_t rearms_so_far)
{
    if (!continuous || ret != -EIO ||
        rearms_so_far >= DWM3000_RX_CHEAP_REARM_MAX) {
        return false;
    }
    /* receive_frame() only reports -EIO after it has forced RX off and
     * cleared the terminal status, so re-enabling needs no extra teardown.
     * Anything without a decodable-error status is left to the caller. */
    switch (status_to_rx_failure(status)) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
        return true;
    case DWM3000_RX_FAILURE_NONE:
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    case DWM3000_RX_FAILURE_BAD_FRAME:
    default:
        return false;
    }
}

static int receive_frame_with_preamble_timeout(uint32_t timeout_ms,
                                               uint16_t preamble_timeout_pac,
                                               uint8_t *frame,
                                               size_t frame_cap,
                                               size_t *frame_len,
                                               uint8_t *quality,
                                               int8_t *rsl_dbm,
                                               enum dwm3000_rx_failure *failure,
                                               struct dwm3000_rx_frame_timing *timing,
                                               bool allow_receive_abort)
{
    uint8_t rx_buffer[UWB_MESH_MAX_FRAME_LEN + FCS_LEN];
    uint64_t rx_timestamp = 0u;
    uint32_t rx_enable_time32 = 0u;
    uint32_t status = 0u;
    size_t raw_len = 0u;
    size_t payload_len;
    uint8_t rx_quality = 0u;
    int8_t rx_rsl_dbm = DWM3000_RSL_INVALID_DBM;
    int64_t receive_deadline_ms;
    uint32_t remaining_ms;
    uint16_t recoverable_rearms = 0u;
    int ret;

    if (frame == NULL || frame_len == NULL || frame_cap == 0u || timeout_ms == 0u) {
        return -EINVAL;
    }
    *frame_len = 0u;
    if (rsl_dbm != NULL) {
        *rsl_dbm = DWM3000_RSL_INVALID_DBM;
    }
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NONE;
    }
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    last_rx_host_uptime_valid = false;

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }

    receive_deadline_ms = k_uptime_get() + timeout_ms;
    if (allow_receive_abort) {
        atomic_set(&receive_abort_enabled, 1);
    }

    /*
     * A foreign or corrupt frame is ordinary shared-spectrum weather, not a
     * radio fault: receive_frame() has already forced RX off and cleared the
     * terminal status by the time it reports one.  Re-enabling the receiver
     * here keeps the caller's whole window listening, instead of unwinding
     * through radio parking, lease release, PHY re-selection and a fresh arm
     * for every neighbouring transmitter.  The caller still sees the pressure
     * through the retained rearm count, so its storm re-init stays armed.
     */
    while (true) {
        remaining_ms = remaining_timeout_ms(receive_deadline_ms);
        if (remaining_ms == 0u) {
            status = 0u;
            ret = -ETIMEDOUT;
            break;
        }

        dwt_setpreambledetecttimeout(preamble_timeout_pac);
        dwt_setrxtimeout(0u);
        clear_all_events();
        ret = start_immediate_rx();
        if (ret < 0) {
            if (allow_receive_abort) {
                atomic_set(&receive_abort_enabled, 0);
            }
            return ret;
        }
        if (timing != NULL) {
            /* Sample every arm: rx_since_enable_uus must be measured from the
             * enable that actually produced the frame, not from an earlier
             * attempt a foreign frame ended. */
            rx_enable_time32 = dwt_readsystimestamphi32();
        }

        last_rx_finfo_register = 0u;
        ret = receive_frame(remaining_ms,
                            &status,
                            rx_buffer,
                            sizeof(rx_buffer),
                            &raw_len,
                            timing == NULL ? NULL : &rx_timestamp,
                            &rx_quality,
                            &rx_rsl_dbm,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            rsl_dbm != NULL);
        if (!rx_error_allows_cheap_reenable(allow_receive_abort,
                                            ret,
                                            status,
                                            recoverable_rearms)) {
            break;
        }
        /* Deliberately silent: one line per foreign frame is exactly the
         * burst that overruns the debug channel.  The retained count is
         * reported once per receive call by the caller's result trace and by
         * driver_stats.rx_cheap_rearms. */
        recoverable_rearms++;
        driver_stats.rx_cheap_rearms++;
    }
    {
        const dwt_config_t *config = config_for_phy(active_phy_mode);

        last_rx_debug = (struct dwm3000_rx_debug_snapshot) {
            .status = status,
            /* Successful frame extraction already read RX_FINFO. Do not add
             * an unbounded diagnostic SPI transaction after timeout/error. */
            .rx_finfo = ret == 0 ? last_rx_finfo_register : 0u,
            .sfd_timeout = effective_sfd_timeout(config),
            .recoverable_rearms = recoverable_rearms,
            .phy_mode = (uint8_t)active_phy_mode,
            .channel = config->chan,
            .preamble_length = config->txPreambLength,
            .pac = config->rxPAC,
            .tx_code = config->txCode,
            .rx_code = config->rxCode,
            .sfd_type = config->sfdType,
            .data_rate = config->dataRate,
            .phr_mode = config->phrMode,
            .phr_rate = config->phrRate,
        };
    }
    if (allow_receive_abort) {
        atomic_set(&receive_abort_enabled, 0);
    }
    if (ret < 0) {
        if (ret == -ECANCELED) {
            return -ECANCELED;
        }
        if (failure != NULL) {
            *failure = status_to_rx_failure(status);
            if (*failure == DWM3000_RX_FAILURE_NONE) {
                if (ret == -EMSGSIZE) {
                    *failure = DWM3000_RX_FAILURE_BAD_FRAME;
                }
            }
        }
        if (status_to_rx_failure(status) == DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT) {
            return -ETIMEDOUT;
        }
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    payload_len = payload_len_without_fcs(raw_len);
    if (payload_len > frame_cap) {
        dwt_forcetrxoff();
        if (failure != NULL) {
            *failure = DWM3000_RX_FAILURE_BAD_FRAME;
        }
        return -EMSGSIZE;
    }

    memcpy(frame, rx_buffer, payload_len);
    *frame_len = payload_len;
    if (quality != NULL) {
        *quality = rx_quality;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = rx_rsl_dbm;
    }
    if (timing != NULL) {
        timing->rx_timestamp = rx_timestamp;
        timing->rx_enable_time32 = rx_enable_time32;
        timing->rx_timestamp_time32 = (uint32_t)(rx_timestamp >> 8);
        timing->rx_since_enable_uus =
            dwt_time32_delta_to_uus(rx_enable_time32, timing->rx_timestamp_time32);
        timing->valid = true;
    }
    return 0;
}

void dwm3000_driver_last_rx_debug_get(struct dwm3000_rx_debug_snapshot *snapshot)
{
    if (snapshot != NULL) {
        *snapshot = last_rx_debug;
    }
}

static int finish_abortible_continuous_receive(int ret)
{
    atomic_set(&receive_abort_enabled, 0);
    return ret;
}

static int receive_frame_continuous_extend_on_activity(uint32_t acquire_timeout_ms,
                                                       uint32_t completion_timeout_ms,
                                                       int64_t activity_deadline_ms,
                                                       uint8_t *frame,
                                                       size_t frame_cap,
                                                       size_t *frame_len,
                                                       uint8_t *quality,
                                                       int8_t *rsl_dbm,
                                                       enum dwm3000_rx_failure *failure)
{
    uint8_t rx_buffer[UWB_MESH_MAX_FRAME_LEN + FCS_LEN];
    uint32_t status = 0u;
    uint32_t last_status = 0u;
    enum dwm3000_rx_failure last_failure = DWM3000_RX_FAILURE_NONE;
    size_t raw_len = 0u;
    size_t payload_len;
    uint8_t rx_quality = 0u;
    int8_t rx_rsl_dbm = DWM3000_RSL_INVALID_DBM;
    int64_t acquire_deadline_ms;
    int64_t completion_deadline_ms = 0;
    uint32_t remaining_ms;
    uint32_t rearm_count = 0u;
    uint32_t hunt_started_ms = 0u;
    uint32_t hunt_ended_ms = 0u;
    uint64_t observed_at_ms = 0u;
    bool activity_seen = false;
    bool completion_active = false;
    int ret;

    if (frame == NULL || frame_len == NULL || frame_cap == 0u ||
        acquire_timeout_ms == 0u || completion_timeout_ms == 0u) {
        return -EINVAL;
    }
    *frame_len = 0u;
    if (rsl_dbm != NULL) {
        *rsl_dbm = DWM3000_RSL_INVALID_DBM;
    }
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NONE;
    }
    last_rx_host_uptime_valid = false;

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    ret = start_immediate_rx();
    if (ret < 0) {
        return ret;
    }
    hunt_started_ms = k_uptime_get_32();
    acquire_deadline_ms = k_uptime_get() + acquire_timeout_ms;

    driver_stats.rx_starts++;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_CH5_RX_HUNT_START acq=%u complete=%u cap=%u\n",
                            acquire_timeout_ms,
                            completion_timeout_ms,
                            (unsigned int)frame_cap);
    }

    atomic_set(&receive_abort_enabled, 1);
    ret = wait_status_internal(RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK,
                               acquire_timeout_ms,
                               &status,
                               true,
                               &observed_at_ms);
    last_status = status;
    if (ret == -ECANCELED) {
        dwt_forcetrxoff();
        return finish_abortible_continuous_receive(-ECANCELED);
    }
    if (ret < 0 && ret != -ETIMEDOUT && ret != -ECANCELED) {
        driver_stats.rx_failures++;
        return finish_abortible_continuous_receive(ret);
    }
    if (ret < 0 && !rx_status_has_activity(status)) {
        hunt_ended_ms = k_uptime_get_32();
        dwt_forcetrxoff();
        if (ret != -ETIMEDOUT) {
            driver_stats.rx_failures++;
            return finish_abortible_continuous_receive(ret);
        }
        driver_stats.rx_timeouts++;
        if (failure != NULL) {
            *failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_CH5_RX_HUNT_EMPTY s=%u e=%u ret=%d sys=%08x phy=%u cfg=%u aw=%u unk=%u\n",
                                hunt_started_ms,
                                hunt_ended_ms,
                                ret,
                                status,
                                (unsigned int)active_phy_mode,
                                radio_configured ? 1u : 0u,
                                radio_awake ? 1u : 0u,
                                radio_state_unknown ? 1u : 0u);
        }
        return finish_abortible_continuous_receive(-ETIMEDOUT);
    }

    if (ret < 0 || (status & RX_ACTIVITY_STATUS_MASK) != 0u ||
        ((status & RX_TERMINAL_STATUS_MASK) != 0u &&
         (status & SYS_STATUS_RXFCG_BIT_MASK) == 0u)) {
        activity_seen = rx_status_has_activity(status);
    }

    if (activity_seen) {
        completion_active = true;
        completion_deadline_ms = k_uptime_get() + completion_timeout_ms;
        if (activity_deadline_ms > 0 &&
            completion_deadline_ms > activity_deadline_ms) {
            completion_deadline_ms = activity_deadline_ms;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_EXTEND ret=%d status=0x%08x complete=%u\n",
                                ret,
                                status,
                                completion_timeout_ms);
        }
    }

    while (true) {
        if ((status & SYS_STATUS_RXFCG_BIT_MASK) != 0u) {
            payload_len = 0u;
            ret = read_rx_frame(rx_buffer, sizeof(rx_buffer), &raw_len);
            if (ret == -EHOSTUNREACH) {
                activity_seen = false;
                completion_active = false;
                last_failure = DWM3000_RX_FAILURE_NONE;
                remaining_ms = remaining_timeout_ms(acquire_deadline_ms);
                if (remaining_ms == 0u) {
                    ret = clear_status_checked(SYS_STATUS_RXFCG_BIT_MASK,
                                               "continuous-rx-scope-expired-clear");
                    if (ret < 0) {
                        dwt_forcetrxoff();
                        return finish_abortible_continuous_receive(ret);
                    }
                    last_failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
                    break;
                }

                ret = rearm_after_rf_scope_drop(
                    "continuous-rx-scope-drop-clear");
                if (ret < 0) {
                    driver_stats.rx_failures++;
                    return finish_abortible_continuous_receive(ret);
                }
                rearm_count++;
                status = 0u;
                ret = wait_status_internal(
                    RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK,
                    remaining_ms,
                    &status,
                    true,
                    &observed_at_ms);
                last_status = status;
                if (ret == -ECANCELED) {
                    dwt_forcetrxoff();
                    return finish_abortible_continuous_receive(-ECANCELED);
                }
                if (ret < 0 && ret != -ETIMEDOUT) {
                    driver_stats.rx_failures++;
                    return finish_abortible_continuous_receive(ret);
                }
                if (ret < 0 && !rx_status_has_activity(status)) {
                    last_failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
                    break;
                }
                if (ret < 0 || (status & RX_ACTIVITY_STATUS_MASK) != 0u ||
                    ((status & RX_TERMINAL_STATUS_MASK) != 0u &&
                     (status & SYS_STATUS_RXFCG_BIT_MASK) == 0u)) {
                    activity_seen = rx_status_has_activity(status);
                }
                if (activity_seen) {
                    completion_active = true;
                    completion_deadline_ms =
                        k_uptime_get() + completion_timeout_ms;
                    if (activity_deadline_ms > 0 &&
                        completion_deadline_ms > activity_deadline_ms) {
                        completion_deadline_ms = activity_deadline_ms;
                    }
                }
                continue;
            }
            if (ret < 0) {
                int clear_ret = clear_status_checked(
                    SYS_STATUS_RXFCG_BIT_MASK,
                    "continuous-rx-invalid-clear");

                driver_stats.rx_failures++;
                dwt_forcetrxoff();
                if (failure != NULL) {
                    *failure = DWM3000_RX_FAILURE_BAD_FRAME;
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_BAD status=0x%08x rearm=%u\n",
                                        status,
                                        rearm_count);
                }
                return finish_abortible_continuous_receive(
                    clear_ret < 0 ? clear_ret : ret);
            }

            last_rx_host_uptime_ms = (uint32_t)observed_at_ms;
            last_rx_host_uptime_valid = true;
            if (rsl_dbm != NULL) {
                if (!read_rx_diagnostics(&rx_rsl_dbm,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL)) {
                    rx_rsl_dbm = DWM3000_RSL_INVALID_DBM;
                }
            }
            if (quality != NULL) {
                rx_quality = 100u;
            }
            ret = clear_status_checked(SYS_STATUS_RXFCG_BIT_MASK,
                                       "continuous-rx-complete-clear");
            if (ret < 0) {
                dwt_forcetrxoff();
                return finish_abortible_continuous_receive(ret);
            }

            payload_len = payload_len_without_fcs(raw_len);
            if (payload_len > frame_cap) {
                dwt_forcetrxoff();
                if (failure != NULL) {
                    *failure = DWM3000_RX_FAILURE_BAD_FRAME;
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_OVERSIZE raw=%u payload=%u cap=%u\n",
                                        (unsigned int)raw_len,
                                        (unsigned int)payload_len,
                                        (unsigned int)frame_cap);
                }
                return finish_abortible_continuous_receive(-EMSGSIZE);
            }

            memcpy(frame, rx_buffer, payload_len);
            *frame_len = payload_len;
            if (quality != NULL) {
                *quality = rx_quality;
            }
            if (rsl_dbm != NULL) {
                *rsl_dbm = rx_rsl_dbm;
            }
            driver_stats.rx_dones++;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_DONE raw=%u payload=%u status=0x%08x rearm=%u extended=%u\n",
                                    (unsigned int)raw_len,
                                    (unsigned int)payload_len,
                                    status,
                                    rearm_count,
                                    completion_active ? 1u : 0u);
            }
            return finish_abortible_continuous_receive(0);
        }

        if (!completion_active || !activity_seen) {
            last_failure = status_to_rx_failure(status);
            break;
        }

        if ((status & RX_TERMINAL_STATUS_MASK) != 0u) {
            last_failure = status_to_rx_failure(status);
            if (last_failure == DWM3000_RX_FAILURE_NONE) {
                last_failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT;
            }
            dwt_forcetrxoff();
            ret = clear_status_checked(RX_CLEAR_STATUS_MASK,
                                       "continuous-rx-error-clear");
            if (ret < 0) {
                if (failure != NULL) {
                    *failure = last_failure;
                }
                return finish_abortible_continuous_receive(ret);
            }
            remaining_ms = remaining_timeout_ms(completion_deadline_ms);
            if (remaining_ms == 0u) {
                break;
            }
            rearm_count++;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_REARM status=0x%08x fail=%u remain=%u rearm=%u\n",
                                    status,
                                    (unsigned int)last_failure,
                                    remaining_ms,
                                    rearm_count);
            }
            dwt_setpreambledetecttimeout(0u);
            dwt_setrxtimeout(0u);
            ret = start_immediate_rx();
            if (ret < 0) {
                if (failure != NULL) {
                    *failure = last_failure;
                }
                return finish_abortible_continuous_receive(ret);
            }
            status = 0u;
            ret = wait_status_internal(RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK,
                                       remaining_ms,
                                       &status,
                                       true,
                                       &observed_at_ms);
            last_status = status;
            if (ret == -ECANCELED) {
                dwt_forcetrxoff();
                return finish_abortible_continuous_receive(-ECANCELED);
            }
            if (ret < 0 && ret != -ETIMEDOUT && ret != -ECANCELED) {
                driver_stats.rx_failures++;
                return finish_abortible_continuous_receive(ret);
            }
            if (ret < 0 && !rx_status_has_activity(status)) {
                break;
            }
            if (rx_status_has_activity(status)) {
                activity_seen = true;
            }
            continue;
        }

        if ((status & RX_ACTIVITY_STATUS_MASK) != 0u || ret < 0) {
            remaining_ms = remaining_timeout_ms(completion_deadline_ms);
            if (remaining_ms == 0u) {
                last_failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT;
                break;
            }
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_WAIT status=0x%08x remain=%u\n",
                                    status,
                                    remaining_ms);
            }
            ret = wait_status_internal(RX_TERMINAL_STATUS_MASK,
                                       remaining_ms,
                                       &status,
                                       true,
                                       &observed_at_ms);
            last_status = status;
            if (ret == -ECANCELED) {
                dwt_forcetrxoff();
                return finish_abortible_continuous_receive(-ECANCELED);
            }
            if (ret < 0 && ret != -ETIMEDOUT && ret != -ECANCELED) {
                driver_stats.rx_failures++;
                return finish_abortible_continuous_receive(ret);
            }
            if (ret < 0) {
                last_failure = rx_status_has_activity(status) ?
                               DWM3000_RX_FAILURE_FRAME_TIMEOUT :
                               DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
                break;
            }
            continue;
        }

        break;
    }

    driver_stats.rx_timeouts++;
    dwt_forcetrxoff();
    if (last_failure == DWM3000_RX_FAILURE_NONE) {
        last_failure = activity_seen ? DWM3000_RX_FAILURE_FRAME_TIMEOUT :
                       DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    if (failure != NULL) {
        *failure = last_failure;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_CH5_RX_ACTIVITY_FAIL ret=%d status=0x%08x last=0x%08x fail=%u activity=%u rearm=%u\n",
                            ret,
                            status,
                            last_status,
                            (unsigned int)last_failure,
                            activity_seen ? 1u : 0u,
                            rearm_count);
    }
    return finish_abortible_continuous_receive(
        activity_seen ? -EIO : -ETIMEDOUT);
}

int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
{
    return receive_frame_with_preamble_timeout(timeout_ms,
                                               immediate_preamble_timeout_pac(
                                                   active_phy_mode),
                                               frame,
                                               frame_cap,
                                               frame_len,
                                               quality,
                                               rsl_dbm,
                                               failure,
                                               NULL,
                                               false);
}

int dwm3000_driver_receive_frame_detailed_quiet(uint32_t timeout_ms,
                                                uint8_t *frame,
                                                size_t frame_cap,
                                                size_t *frame_len,
                                                uint8_t *quality,
                                                int8_t *rsl_dbm,
                                                enum dwm3000_rx_failure *failure)
{
    return receive_frame_with_preamble_timeout(timeout_ms,
                                               immediate_preamble_timeout_pac(
                                                   active_phy_mode),
                                               frame,
                                               frame_cap,
                                               frame_len,
                                               quality,
                                               rsl_dbm,
                                               failure,
                                               NULL,
                                               false);
}

int dwm3000_driver_sniff_activity(uint32_t timeout_ms,
                                  enum dwm3000_rx_failure *failure)
{
    uint32_t status = 0u;
    enum dwm3000_rx_failure local_failure = DWM3000_RX_FAILURE_NONE;
    bool activity_seen;
    int ret;

    if (timeout_ms == 0u) {
        return -EINVAL;
    }
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NONE;
    }

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    ret = start_immediate_rx();
    if (ret < 0) {
        return ret;
    }

    driver_stats.rx_starts++;
    ret = wait_status_internal(RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK,
                               timeout_ms,
                               &status,
                               false,
                               NULL);
    activity_seen = rx_status_has_activity(status);
    dwt_forcetrxoff();
    {
        int cleanup_ret = clear_status_checked(RX_CLEAR_STATUS_MASK,
                                               "sniff-rx-clear");

        if (cleanup_ret < 0) {
            driver_stats.rx_failures++;
            return cleanup_ret;
        }
    }
    if (ret < 0 && ret != -ETIMEDOUT) {
        driver_stats.rx_failures++;
        return ret;
    }
    if (!activity_seen) {
        driver_stats.rx_timeouts++;
        if (failure != NULL) {
            *failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
        }
        return -ETIMEDOUT;
    }

    local_failure = status_to_rx_failure(status);
    if (local_failure == DWM3000_RX_FAILURE_NONE &&
        (status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        local_failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    if (failure != NULL) {
        *failure = local_failure;
    }
    return 0;
}

int dwm3000_driver_receive_frame_continuous(uint32_t timeout_ms,
                                            uint8_t *frame,
                                            size_t frame_cap,
                                            size_t *frame_len,
                                            uint8_t *quality,
                                            int8_t *rsl_dbm,
                                            enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_continuous_timed(timeout_ms,
                                                        frame,
                                                        frame_cap,
                                                        frame_len,
                                                        quality,
                                                        rsl_dbm,
                                                        failure,
                                                        NULL);
}

int dwm3000_driver_receive_frame_continuous_extend_on_activity(
    uint32_t acquire_timeout_ms,
    uint32_t completion_timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    return receive_frame_continuous_extend_on_activity(acquire_timeout_ms,
                                                       completion_timeout_ms,
                                                       0,
                                                       frame,
                                                       frame_cap,
                                                       frame_len,
                                                       quality,
                                                       rsl_dbm,
                                                       failure);
}

int dwm3000_driver_receive_frame_continuous_extend_on_activity_until(
    uint32_t acquire_timeout_ms,
    uint32_t completion_timeout_ms,
    int64_t activity_deadline_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    uint32_t remaining_ms = remaining_timeout_ms(activity_deadline_ms);

    if (remaining_ms == 0u) {
        return -ETIMEDOUT;
    }
    if (acquire_timeout_ms > remaining_ms) {
        acquire_timeout_ms = remaining_ms;
    }
    return receive_frame_continuous_extend_on_activity(acquire_timeout_ms,
                                                       completion_timeout_ms,
                                                       activity_deadline_ms,
                                                       frame,
                                                       frame_cap,
                                                       frame_len,
                                                       quality,
                                                       rsl_dbm,
                                                       failure);
}

int dwm3000_driver_receive_frame_continuous_timed(uint32_t timeout_ms,
                                                  uint8_t *frame,
                                                  size_t frame_cap,
                                                  size_t *frame_len,
                                                  uint8_t *quality,
                                                  int8_t *rsl_dbm,
                                                  enum dwm3000_rx_failure *failure,
                                                  struct dwm3000_rx_frame_timing *timing)
{
    return receive_frame_with_preamble_timeout(timeout_ms,
                                               0u,
                                               frame,
                                               frame_cap,
                                               frame_len,
                                               quality,
                                               rsl_dbm,
                                               failure,
                                               timing,
                                               true);
}

int dwm3000_driver_last_rx_host_uptime(uint32_t *received_at_ms)
{
    if (received_at_ms == NULL) {
        return -EINVAL;
    }
    if (!last_rx_host_uptime_valid) {
        return -ENOENT;
    }
    *received_at_ms = last_rx_host_uptime_ms;
    return 0;
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    atomic_val_t requested =
        (atomic_val_t)(owner_mask & DWM3000_RECEIVE_ABORT_OWNER_MASK);

    if (requested != 0) {
        (void)atomic_or(&receive_abort_owners, requested);
    }
}

void dwm3000_driver_clear_receive_abort(uint32_t owner_mask)
{
    atomic_val_t cleared =
        (atomic_val_t)(owner_mask & DWM3000_RECEIVE_ABORT_OWNER_MASK);

    if (cleared != 0) {
        (void)atomic_and(&receive_abort_owners, ~cleared);
    }
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return atomic_get(&receive_abort_owners) != 0;
}
