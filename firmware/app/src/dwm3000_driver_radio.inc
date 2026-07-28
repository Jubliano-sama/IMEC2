#line 487 __BASE_FILE__
static void clear_all_events(void);

static int take_port_error(const char *operation)
{
    int ret = dwm3000_port_take_error();

    if (ret >= 0) {
        return 0;
    }
    driver_stats.spi_failures++;
    radio_configured = false;
    radio_awake = false;
    radio_restored_from_sleep = false;
    active_phy_mode = DWM3000_PHY_NONE;
    LOG_ERR("DWM3000 port error: operation=%s ret=%d",
            operation == NULL ? "unknown" : operation,
            ret);
    return ret;
}

static int validate_device_identity(const char *operation)
{
    uint32_t dev_id;
    int ret = take_port_error(operation);

    if (ret < 0) {
        return ret;
    }
    dwm3000_port_clear_error();
    dev_id = dwt_readdevid();
    ret = take_port_error(operation);
    if (ret < 0) {
        return ret;
    }
    if (!dwm3000_port_dev_id_supported(dev_id)) {
        LOG_ERR("DWM3000 identity validation failed: operation=%s dev_id=0x%08x",
                operation == NULL ? "unknown" : operation,
                dev_id);
        return -ENODEV;
    }
    return 0;
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static const char *dwm3000_debug_role_name(void)
{
    switch (DEVICE_ROLE) {
    case ROLE_CLICKER:
#if defined(CONFIG_IMEC_ROLE_TAG)
        if (IS_ENABLED(CONFIG_IMEC_ROLE_TAG)) {
            return "tag";
        }
#endif
        return "clicker";
    case ROLE_ANCHOR:
        return "anchor";
    case ROLE_GATEWAY:
        return "gateway";
    default:
        return "unknown";
    }
}
#endif

static void dwm3000_debug_event(bool warning, const char *event, const char *fmt, ...)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    char prefix[96];
    char message[160];
    va_list args;
    int ret;

    ret = debug_log_format_prefix(prefix,
                                  sizeof(prefix),
                                  k_uptime_get_32(),
                                  dwm3000_debug_role_name(),
                                  DEVICE_ID,
                                  CONFIG_IMEC_BENCH_STAGE,
                                  event);
    if (ret < 0) {
        return;
    }

    va_start(args, fmt);
    ret = vsnprintk(message, sizeof(message), fmt, args);
    va_end(args);
    if (ret < 0) {
        return;
    }

    if (warning) {
        LOG_WRN("%s %s", prefix, message);
    } else {
        LOG_DBG("%s %s", prefix, message);
    }
#else
    ARG_UNUSED(warning);
    ARG_UNUSED(event);
    ARG_UNUSED(fmt);
#endif
}

static void configure_first_path_sensitivity(void)
{
    /*
     * Use the lower recommended CIA first-path threshold. It can increase
     * outliers, but the ranging/report path keeps sample-level rejection.
     */
    dwt_modify8bitoffsetreg(IP_CONFIG_LO_ID,
                            0,
                            (uint8_t)~IP_CONFIG_LO_IP_NTM_BIT_MASK,
                            DWM3000_FIRST_PATH_NTM_LOW);
}

static uint16_t short_addr_from_id(uint64_t device_id)
{
    uint16_t short_addr = (uint16_t)(device_id & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

static uint64_t timestamp_to_u64(const uint8_t timestamp[5])
{
    return ((uint64_t)timestamp[0]) |
           ((uint64_t)timestamp[1] << 8) |
           ((uint64_t)timestamp[2] << 16) |
           ((uint64_t)timestamp[3] << 24) |
           ((uint64_t)timestamp[4] << 32);
}

static uint64_t read_tx_timestamp_u64(void)
{
    uint8_t timestamp[5];

    dwt_readtxtimestamp(timestamp);
    return timestamp_to_u64(timestamp);
}

static uint64_t read_rx_timestamp_u64(void)
{
    uint8_t timestamp[5];

    dwt_readrxtimestamp(timestamp);
    return timestamp_to_u64(timestamp);
}

static uint16_t dwt_delta_to_uus(uint32_t start_ts, uint32_t end_ts)
{
    return dwm3000_driver_dwt_delta_to_uus(start_ts, end_ts);
}

static uint32_t dwt_time32_delta_to_uus(uint32_t start_time32, uint32_t end_time32)
{
    uint32_t delta = end_time32 - start_time32;

    return (uint32_t)((((uint64_t)delta << 8) +
                       (DWM3000_UUS_TO_DWT_TIME / 2u)) /
                      DWM3000_UUS_TO_DWT_TIME);
}

static uint32_t delayed_tx_time_from_rx_reference(uint64_t rx_reference_ts,
                                                  uint16_t delay_uus)
{
    return (uint32_t)((rx_reference_ts +
                       ((uint64_t)delay_uus * DWM3000_UUS_TO_DWT_TIME)) >> 8);
}

static uint64_t delayed_tx_timestamp_from_programmed_time(uint32_t tx_time)
{
    return (((uint64_t)(tx_time & 0xfffffffeUL)) << 8) + DWM3000_TX_ANT_DLY;
}

static uint16_t reply_delay_uus_from_round_index(uint8_t round_index)
{
#if DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
    uint32_t span_uus = DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS -
                        DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS;
    uint32_t max_index = span_uus / DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS;
    uint32_t index = MIN((uint32_t)round_index, max_index);
    uint32_t delay_uus = DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS -
                         (index * DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS);

    return (uint16_t)MAX(delay_uus, DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS);
#else
    (void)round_index;
    return DS_TWR_REPLY_DLY_UUS;
#endif
}

static uint16_t request_reply_delay_uus(const struct dwm3000_range_request *request)
{
    if (request != NULL && request->reply_delay_uus != 0u) {
        return request->reply_delay_uus;
    }
    return reply_delay_uus_from_round_index(request == NULL ? 0u : request->round_index);
}

static uint16_t ds_twr_rx_guard_before_uus(uint16_t reply_delay_uus)
{
    return reply_delay_uus > DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS ?
           DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS : reply_delay_uus;
}

static uint16_t ds_twr_rx_after_tx_delay_uus(uint16_t reply_delay_uus)
{
    return reply_delay_uus - ds_twr_rx_guard_before_uus(reply_delay_uus);
}

static uint16_t ds_twr_rx_timeout_uus(uint16_t reply_delay_uus)
{
    uint32_t timeout_uus = (uint32_t)ds_twr_rx_guard_before_uus(reply_delay_uus) +
                           DWM3000_DS_TWR_RX_GUARD_AFTER_UUS;

    return timeout_uus > UINT16_MAX ? UINT16_MAX : (uint16_t)timeout_uus;
}

static int validate_driver_reply_timing(uint16_t poll_to_resp_uus,
                                        uint16_t resp_to_final_uus,
                                        uint16_t expected_uus)
{
    int ret = dwm3000_driver_validate_reply_timing(
        poll_to_resp_uus,
        resp_to_final_uus,
        expected_uus,
        DWM3000_REPLY_TIMING_TOLERANCE_US);

    if (ret != PROTO_OK) {
        LOG_WRN("DS-TWR timing invalid: poll_to_resp_uus=%u resp_to_final_uus=%u expected_uus=%u tolerance_uus=%u",
                poll_to_resp_uus,
                resp_to_final_uus,
                expected_uus,
                DWM3000_REPLY_TIMING_TOLERANCE_US);
    }
    return ret;
}

static uint32_t ds_twr_rx_wait_timeout_ms(uint16_t reply_delay_uus)
{
    uint32_t timeout_uus = (uint32_t)ds_twr_rx_after_tx_delay_uus(reply_delay_uus) +
                           ds_twr_rx_timeout_uus(reply_delay_uus);

    return DS_TWR_UUS_TO_MS_CEIL(timeout_uus) + DWM3000_DS_TWR_RX_WAIT_MARGIN_MS;
}

static int ensure_local_data(void)
{
    return dwt_setlocaldataptr(0) == DWT_SUCCESS ? 0 : -EINVAL;
}

static int initialise_radio(bool idle_after_init)
{
    int ret;
    int mode = idle_after_init ? DWT_DW_IDLE : DWT_DW_INIT;

    ret = dwm3000_port_init();
    if (ret < 0) {
        return ret;
    }

    ret = ensure_local_data();
    if (ret < 0) {
        return ret;
    }

    dwm3000_port_clear_error();
    (void)dwm3000_port_set_slow_spi();
    if (dwt_initialise(mode) != DWT_SUCCESS) {
        return -EIO;
    }
    ret = take_port_error("initialise");
    if (ret < 0) {
        return ret;
    }
    ret = validate_device_identity("initialise");
    if (ret < 0) {
        return ret;
    }
    radio_awake = true;

    return dwm3000_port_set_fast_spi();
}

static dwt_txconfig_t *txconfig_for_phy(enum dwm3000_phy_mode phy_mode)
{
    return phy_mode == DWM3000_PHY_MESH_PAYLOAD ?
           &mesh_payload_txconfig :
           &default_txconfig;
}

static int apply_radio_config(const dwt_config_t *config,
                              enum dwm3000_phy_mode phy_mode)
{
    if (config == NULL) {
        return -EINVAL;
    }

    dwm3000_port_clear_error();

    /* Qorvo requires the IC to be returned to idle before dwt_configure(). */
    if (radio_configured && radio_awake) {
        dwt_forcetrxoff();
        clear_all_events();
    }

    if (dwt_configure((dwt_config_t *)config) != DWT_SUCCESS) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_CONFIG_FAIL phy=%d ret=%d sys=0x%08x\n",
                                phy_mode, -EIO, (unsigned int)sys_status);
        }
        return -EIO;
    }

    configure_first_path_sensitivity();
    dwt_configuretxrf(txconfig_for_phy(phy_mode));
    dwt_configciadiag(DWM3000_CIA_DIAG_MODE);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    {
        int ret = take_port_error("apply-config");

        if (ret < 0) {
            return ret;
        }
    }

    radio_configured = true;
    radio_awake = true;
    radio_restored_from_sleep = false;
    active_phy_mode = phy_mode;
    return 0;
}

static int restore_txrx_after_sleep(enum dwm3000_phy_mode phy_mode)
{
    int ret;

    dwm3000_port_clear_error();
    ret = dwt_restore_txrx(DWT_RESTORE_TXRX_MODE);
    if (ret != DWT_SUCCESS) {
        return -EIO;
    }

    configure_first_path_sensitivity();
    dwt_configuretxrf(txconfig_for_phy(phy_mode));
    dwt_configciadiag(DWM3000_CIA_DIAG_MODE);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    ret = take_port_error("restore-txrx");
    if (ret < 0) {
        return ret;
    }

    return ensure_local_data();
}

static uint16_t effective_sfd_timeout(const dwt_config_t *config)
{
    return config->sfdTO == 0u ? DWT_SFDTOC_DEF : config->sfdTO;
}

static const dwt_config_t *config_for_phy(enum dwm3000_phy_mode phy_mode)
{
    switch (phy_mode) {
    case DWM3000_PHY_WAKE:
        return &wake_config;
    case DWM3000_PHY_WAKE_MESH_CONTROL:
        return &wake_mesh_control_config;
    case DWM3000_PHY_MESH_PAYLOAD:
        return &mesh_payload_config;
    case DWM3000_PHY_RANGE:
    case DWM3000_PHY_NONE:
    default:
        return &default_config;
    }
}

static bool phy_configs_equal(const dwt_config_t *a, const dwt_config_t *b)
{
    return a->chan == b->chan &&
           a->txPreambLength == b->txPreambLength &&
           a->rxPAC == b->rxPAC &&
           a->txCode == b->txCode &&
           a->rxCode == b->rxCode &&
           a->sfdType == b->sfdType &&
           a->dataRate == b->dataRate &&
           a->phrMode == b->phrMode &&
           a->phrRate == b->phrRate &&
           effective_sfd_timeout(a) == effective_sfd_timeout(b) &&
           a->stsMode == b->stsMode &&
           a->stsLength == b->stsLength &&
           a->pdoaMode == b->pdoaMode;
}

static int configure_radio_from_reset(enum dwm3000_phy_mode phy_mode)
{
    const dwt_config_t *config = config_for_phy(phy_mode);
    int ret;

    radio_configured = false;
    radio_awake = false;
    radio_restored_from_sleep = false;
    active_phy_mode = DWM3000_PHY_NONE;

    ret = dwm3000_port_init();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_PORT_INIT_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_WAKE_PIN_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_HW_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = initialise_radio(false);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_RESET_INIT_FAIL phy=%d ret=%d sys=0x%08x\n",
                                phy_mode, ret, (unsigned int)sys_status);
        }
        return ret;
    }

    return apply_radio_config(config, phy_mode);
}

static int wake_configured_radio(enum dwm3000_phy_mode requested_phy)
{
    const dwt_config_t *active_config;
    const dwt_config_t *requested_config;
    bool restore_txrx;
    uint32_t wake_start_us;
    uint32_t wake_elapsed_us;
    int ret;

    if (!radio_configured || radio_awake) {
        return 0;
    }

    if (requested_phy == DWM3000_PHY_NONE) {
        requested_phy = active_phy_mode == DWM3000_PHY_NONE ?
            DWM3000_PHY_RANGE : active_phy_mode;
    }
    active_config = config_for_phy(active_phy_mode);
    requested_config = config_for_phy(requested_phy);
    restore_txrx = active_phy_mode == requested_phy ||
                   phy_configs_equal(active_config, requested_config);

    wake_start_us = k_cyc_to_us_floor32(k_cycle_get_32());
    driver_stats.sleep_wake_count++;

    ret = dwm3000_port_set_slow_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_SLOW_SPI_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
        return ret;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_PIN_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
        return ret;
    }

    dwm3000_port_clear_error();
    for (uint32_t waited_us = 0u; waited_us <= DWM3000_WAKE_IDLE_RC_TIMEOUT_US;
         waited_us += DWM3000_STATUS_POLL_INTERVAL_US) {
        if (dwt_checkidlerc()) {
            ret = 0;
            break;
        }
        ret = take_port_error("wake-idle-check");
        if (ret < 0) {
            break;
        }
        k_busy_wait(DWM3000_STATUS_POLL_INTERVAL_US);
        ret = -ETIMEDOUT;
    }
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_WAKE_IDLE_RC_TIMEOUT phy=%d ret=%d sys=0x%08x\n",
                                requested_phy, ret, (unsigned int)sys_status);
        }
        return ret;
    }

    dwt_restore_common();
    ret = take_port_error("restore-common");
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }
    if (restore_txrx) {
        ret = restore_txrx_after_sleep(requested_phy);
        if (ret < 0) {
            driver_stats.sleep_wake_failures++;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

                status_debug_printf("DBG_DWM_WAKE_RESTORE_FAIL phy=%d ret=%d sys=0x%08x\n",
                                    requested_phy, ret, (unsigned int)sys_status);
            }
            return ret;
        }
        active_phy_mode = requested_phy;
    }
    radio_awake = true;
    radio_restored_from_sleep = !restore_txrx;

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_FAST_SPI_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
        return ret;
    }
    ret = validate_device_identity("wake-restore");
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }
    wake_elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32()) - wake_start_us;
    driver_stats.sleep_wake_total_us += wake_elapsed_us;
    if (wake_elapsed_us > driver_stats.sleep_wake_max_us) {
        driver_stats.sleep_wake_max_us = wake_elapsed_us;
    }
    LOG_DBG("DWM3000 PHY mode %d woke from sleep%s",
            requested_phy,
            restore_txrx ? " with TX/RX restore" : " for reconfigure");
    return 0;
}

static int ensure_phy_mode(enum dwm3000_phy_mode phy_mode)
{
    const dwt_config_t *active_config;
    const dwt_config_t *config;
    int ret;

    if (!radio_configured) {
        ret = configure_radio_from_reset(phy_mode);
        if (ret < 0) {
            return ret;
        }
    } else {
        ret = wake_configured_radio(phy_mode);
        if (ret < 0) {
            LOG_WRN("DWM3000 retained sleep wake/restore failed: phy=%d ret=%d; forcing full reinit",
                    phy_mode,
                    ret);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DWM_WAKE_REINIT phy=%d ret=%d\n",
                                    phy_mode,
                                    ret);
            }
            ret = configure_radio_from_reset(phy_mode);
            if (ret < 0) {
                LOG_WRN("DWM3000 full reinit after retained wake failure failed: ret=%d",
                        ret);
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_DWM_REINIT_FAIL ret=%d\n", ret);
                }
                return ret;
            }
        }
    }

    config = config_for_phy(phy_mode);
    active_config = config_for_phy(active_phy_mode);
    if (active_phy_mode == phy_mode || phy_configs_equal(active_config, config)) {
        if (radio_restored_from_sleep) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

                status_debug_printf("DBG_DWM_RETAIN_REUSE phy=%d active=%d sys=0x%08x\n",
                                    phy_mode,
                                    active_phy_mode,
                                    (unsigned int)sys_status);
            }
            radio_restored_from_sleep = false;
        } else if (active_phy_mode != phy_mode) {
            LOG_DBG("DWM3000 PHY mode %d selected without redundant reconfigure",
                    phy_mode);
        }
        active_phy_mode = phy_mode;
        return 0;
    }

    /*
     * Cross-PHY changes retune the DW3000 PLL. In practice the active channel-5
     * wake PHY does not reliably transition back to an IDLE_RC state suitable
     * for dwt_configure(); the reset configure path is the supported recovery
     * path used by Qorvo examples when PLL calibration fails.
     */
    if (radio_awake) {
        return configure_radio_from_reset(phy_mode);
    }

    ret = apply_radio_config(config, phy_mode);
    if (ret == 0) {
        return 0;
    }

    LOG_WRN("DWM3000 PHY configure failed: phy=%d ret=%d restored=%u active=%d; forcing full reinit",
            phy_mode,
            ret,
            radio_restored_from_sleep ? 1u : 0u,
            active_phy_mode);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_CONFIG_REINIT phy=%d ret=%d restored=%u active=%d\n",
                            phy_mode,
                            ret,
                            radio_restored_from_sleep ? 1u : 0u,
                            active_phy_mode);
    }
    ret = configure_radio_from_reset(phy_mode);
    if (ret < 0) {
        LOG_WRN("DWM3000 full reinit after PHY configure failure failed: ret=%d",
                ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_REINIT_FAIL ret=%d\n", ret);
        }
        return ret;
    }
    LOG_INF("DWM3000 PHY mode %d configured after full reinit", phy_mode);
    return 0;
}

static int ensure_current_phy_or_range(void)
{
    if (!radio_configured) {
        return dwm3000_driver_configure_default();
    }

    return wake_configured_radio(active_phy_mode == DWM3000_PHY_NONE ?
                                 DWM3000_PHY_RANGE : active_phy_mode);
}

static void clear_status(uint32_t mask)
{
    dwt_write32bitreg(SYS_STATUS_ID, mask);
}

static void clear_all_events(void)
{
    clear_status(0xffffffffu);
}

static int wait_status_internal(uint32_t mask,
                                uint32_t timeout_ms,
                                uint32_t *status,
                                bool log_events)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint32_t read_status;
    uint32_t start_cycles = k_cycle_get_32();
    uint32_t poll_loops = 0u;
    int64_t now_ms;

    {
        int ret = take_port_error("status-entry");

        if (ret < 0) {
            if (status != NULL) {
                *status = 0u;
            }
            return ret;
        }
    }
    dwm3000_port_clear_error();
    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_SYS_STATUS_POLL_START",
                            "mask=0x%08x timeout_ms=%u",
                            mask,
                            timeout_ms);
    }
    do {
        read_status = dwt_read32bitreg(SYS_STATUS_ID);
        poll_loops++;
        {
            int ret = take_port_error("status-read");

            if (ret < 0) {
                if (status != NULL) {
                    *status = 0u;
                }
                return ret;
            }
        }
        if ((read_status & mask) != 0u) {
            uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
                (uint32_t)(k_cycle_get_32() - start_cycles));

            driver_stats.sys_status_poll_loops += poll_loops;
            if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
                driver_stats.sys_status_poll_max_duration_us = elapsed_us;
            }
            if (status != NULL) {
                *status = read_status;
            }
            if (log_events) {
                dwm3000_debug_event(false,
                                    "UWB_SYS_STATUS_POLL_DONE",
                                    "mask=0x%08x status=0x%08x loops=%u duration_us=%u",
                                    mask,
                                    read_status,
                                    poll_loops,
                                    elapsed_us);
            }
            return 0;
        }

        if (atomic_get(&receive_abort_enabled) != 0 &&
            atomic_get(&receive_abort_requested) != 0) {
            uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
                (uint32_t)(k_cycle_get_32() - start_cycles));

            driver_stats.sys_status_poll_loops += poll_loops;
            if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
                driver_stats.sys_status_poll_max_duration_us = elapsed_us;
            }
            if (status != NULL) {
                *status = read_status;
            }
            if (log_events) {
                dwm3000_debug_event(false,
                                    "UWB_SYS_STATUS_POLL_ABORT",
                                    "mask=0x%08x status=0x%08x loops=%u duration_us=%u",
                                    mask,
                                    read_status,
                                    poll_loops,
                                    elapsed_us);
            }
            return -ECANCELED;
        }

        now_ms = k_uptime_get();
        if (now_ms >= deadline) {
            break;
        }
        k_busy_wait(DWM3000_STATUS_POLL_INTERVAL_US);
    } while (k_uptime_get() <= deadline);

    if (status != NULL) {
        *status = dwt_read32bitreg(SYS_STATUS_ID);
        if (take_port_error("status-timeout-read") < 0) {
            *status = 0u;
        }
    } else {
        (void)take_port_error("status-timeout-read");
    }
    driver_stats.sys_status_poll_loops += poll_loops;
    driver_stats.sys_status_poll_timeouts++;
    {
        uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - start_cycles));

        if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
            driver_stats.sys_status_poll_max_duration_us = elapsed_us;
        }
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_SYS_STATUS_POLL_TIMEOUT",
                                "mask=0x%08x status=0x%08x loops=%u duration_us=%u timeout_ms=%u",
                                mask,
                                status == NULL ? 0u : *status,
                                poll_loops,
                                elapsed_us,
                                timeout_ms);
        }
    }
    return -ETIMEDOUT;
}

static int wait_status(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    return wait_status_internal(mask, timeout_ms, status, true);
}

static int capture_completed_tx_timestamp(uint32_t timeout_ms,
                                          uint64_t *tx_timestamp)
{
    uint32_t status = 0u;
    int ret;

    if (tx_timestamp == NULL) {
        return -EINVAL;
    }

    /*
     * The TX timestamp register is only valid after TXFRS. Keep this wait and
     * register read quiet so DS-TWR can use the poll-to-response interval to
     * collect the timestamp instead of spending FINAL-arm headroom on it.
     */
    ret = wait_status_internal(SYS_STATUS_TXFRS_BIT_MASK,
                               timeout_ms,
                               &status,
                               false);
    if (ret < 0) {
        return ret;
    }

    *tx_timestamp = read_tx_timestamp_u64();
    return take_port_error("tx-timestamp");
}

static int wait_tx_complete(uint32_t timeout_ms)
{
    uint32_t status = 0u;
    int ret;

    ret = wait_status(SYS_STATUS_TXFRS_BIT_MASK, timeout_ms, &status);
    if (ret < 0) {
        driver_stats.tx_failures++;
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=timeout ret=%d sys_status=0x%08x timeout_ms=%u",
                            ret,
                            status,
                            timeout_ms);
        return ret;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    driver_stats.tx_dones++;
    dwm3000_debug_event(false,
                        "UWB_TX_DONE",
                        "status=ok sys_status=0x%08x",
                        status);
    return 0;
}

static enum range_status status_to_range_status(uint32_t status)
{
    if ((status & SYS_STATUS_ALL_RX_TO) != 0u) {
        return RANGE_RX_TIMEOUT;
    }
    if ((status & SYS_STATUS_ALL_RX_ERR) != 0u) {
        return RANGE_RX_ERROR;
    }
    return RANGE_INTERNAL_ERROR;
}

static enum dwm3000_rx_failure status_to_rx_failure(uint32_t status)
{
    if ((status & SYS_STATUS_RXPTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXSTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_SFD_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXFTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    if ((status & (SYS_STATUS_RXFCE_BIT_MASK |
                   SYS_STATUS_RXPHE_BIT_MASK |
                   SYS_STATUS_RXFSL_BIT_MASK)) != 0u) {
        return DWM3000_RX_FAILURE_CRC_OR_PHY;
    }
    return DWM3000_RX_FAILURE_NONE;
}

static bool rx_status_has_activity(uint32_t status)
{
    return (status & (RX_ACTIVITY_STATUS_MASK |
                      SYS_STATUS_RXFCG_BIT_MASK |
                      SYS_STATUS_RXSTO_BIT_MASK |
                      SYS_STATUS_RXFTO_BIT_MASK |
                      SYS_STATUS_RXFCE_BIT_MASK |
                      SYS_STATUS_RXPHE_BIT_MASK |
                      SYS_STATUS_RXFSL_BIT_MASK)) != 0u;
}

static uint32_t remaining_timeout_ms(int64_t deadline_ms)
{
    int64_t remaining_ms = deadline_ms - k_uptime_get();

    if (remaining_ms <= 0) {
        return 0u;
    }
    if (remaining_ms > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)remaining_ms;
}

#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
static const char *rx_failure_debug_name(enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_NONE:
        return "none";
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
        return "no_preamble_timeout";
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
        return "sfd_timeout";
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
        return "frame_timeout";
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
        return "crc_or_phy";
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return "bad_frame";
    default:
        return "unknown";
    }
}
#endif

static uint8_t floor_log2_u32(uint32_t value)
{
    uint8_t log2 = 0u;

    while (value > 1u) {
        value >>= 1;
        log2++;
    }
    return log2;
}

static int32_t log2_q8_u32(uint32_t value)
{
    uint8_t log2;
    uint32_t normalized;
    uint32_t fraction_q8;

    if (value == 0u) {
        return 0;
    }

    log2 = floor_log2_u32(value);
    normalized = value << (31u - log2);
    fraction_q8 = (normalized - 0x80000000u) >> 23;
    return ((int32_t)log2 << 8) + (int32_t)fraction_q8;
}

static int8_t clamp_i8(int32_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static int8_t estimate_ipatov_rsl_dbm(const dwt_rxdiag_t *diagnostics)
{
    int32_t power_db_q8;
    int32_t accum_db_q8;
    int32_t rsl_q8;
    int32_t rounded_dbm;

    if (diagnostics == NULL ||
        diagnostics->ipatovPower == 0u ||
        diagnostics->ipatovAccumCount == 0u) {
        return 0;
    }

    power_db_q8 = (log2_q8_u32(diagnostics->ipatovPower) * DWM3000_LOG2_TO_DB_Q8) >> 8;
    accum_db_q8 = (log2_q8_u32(diagnostics->ipatovAccumCount) * DWM3000_LOG2_TO_DB_Q8) >> 8;
    rsl_q8 = power_db_q8 +
             DWM3000_IPATOV_POWER_SCALE_DB_Q8 -
             (2 * accum_db_q8) -
             DWM3000_IPATOV_RSL_OFFSET_DB_Q8;
    rounded_dbm = rsl_q8 >= 0 ? (rsl_q8 + 128) >> 8 : -(((-rsl_q8) + 128) >> 8);
    return clamp_i8(rounded_dbm);
}

static bool read_cir_sample(const dwt_rxdiag_t *diagnostics,
                            uint8_t cir_sample[UWB_CIR_SAMPLE_LEN])
{
    uint8_t accum[DWM3000_ACCUM_CIR_SAMPLE_READ_LEN];
    uint16_t fp_int;

    if (diagnostics == NULL ||
        cir_sample == NULL ||
        diagnostics->ipatovAccumCount == 0u) {
        return false;
    }

    fp_int = diagnostics->ipatovFpIndex >> 6;
    if (fp_int >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        return false;
    }

    dwt_readaccdata(accum, sizeof(accum), fp_int);
    memcpy(cir_sample, &accum[1], UWB_CIR_SAMPLE_LEN);
    return true;
}

static void pack_rx_diag_raw(const dwt_rxdiag_t *diagnostics,
                             uint8_t raw[DWM3000_RX_DIAG_RAW_LEN],
                             uint8_t *raw_len)
{
    size_t offset = 0u;

    if (raw_len != NULL) {
        *raw_len = 0u;
    }
    if (diagnostics == NULL || raw == NULL) {
        return;
    }

    memcpy(&raw[offset], diagnostics->ipatovRxTime, sizeof(diagnostics->ipatovRxTime));
    offset += sizeof(diagnostics->ipatovRxTime);
    raw[offset++] = diagnostics->ipatovRxStatus;
    proto_put_u16_le(&raw[offset], diagnostics->ipatovPOA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->stsRxTime, sizeof(diagnostics->stsRxTime));
    offset += sizeof(diagnostics->stsRxTime);
    proto_put_u16_le(&raw[offset], diagnostics->stsRxStatus);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsPOA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->sts2RxTime, sizeof(diagnostics->sts2RxTime));
    offset += sizeof(diagnostics->sts2RxTime);
    proto_put_u16_le(&raw[offset], diagnostics->sts2RxStatus);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2POA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->tdoa, sizeof(diagnostics->tdoa));
    offset += sizeof(diagnostics->tdoa);
    proto_put_u16_le(&raw[offset], (uint16_t)diagnostics->pdoa);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], (uint16_t)diagnostics->xtalOffset);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->ciaDiag1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovPeak);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovPower);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->ipatovFpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->ipatovAccumCount);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsPeak);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsPower);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsFpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsAccumCount);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2Peak);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2Power);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2FpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2AccumCount);
    offset += sizeof(uint16_t);

    if (offset == DWM3000_RX_DIAG_RAW_LEN && raw_len != NULL) {
        *raw_len = (uint8_t)offset;
    }
}

static bool capture_rx_diag_raw(uint8_t raw[DWM3000_RX_DIAG_RAW_LEN],
                                uint8_t *raw_len,
                                uint16_t *first_path_index)
{
    dwt_rxdiag_t diagnostics;

    if (raw_len != NULL) {
        *raw_len = 0u;
    }
    if (first_path_index != NULL) {
        *first_path_index = 0u;
    }
    if (raw == NULL) {
        return false;
    }

    memset(&diagnostics, 0, sizeof(diagnostics));
    dwt_readdiagnostics(&diagnostics);
    pack_rx_diag_raw(&diagnostics, raw, raw_len);
    if (first_path_index != NULL) {
        *first_path_index = diagnostics.ipatovFpIndex >> 6;
    }
    return raw_len == NULL || *raw_len == DWM3000_RX_DIAG_RAW_LEN;
}

static uint16_t cir_window_start_sample(uint16_t first_path_index,
                                        uint16_t window_samples)
{
    uint16_t start_sample;

    if (window_samples >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        return 0u;
    }
    if (first_path_index >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        first_path_index = DWM3000_CIR_ACCUM_SAMPLE_COUNT - 1u;
    }

    start_sample = first_path_index > DWM3000_CIR_WINDOW_PRE_SAMPLES ?
                   first_path_index - DWM3000_CIR_WINDOW_PRE_SAMPLES : 0u;
    if (start_sample + window_samples > DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        start_sample = DWM3000_CIR_ACCUM_SAMPLE_COUNT - window_samples;
    }
    return start_sample;
}

static uint16_t read_cir_window(uint8_t *buffer,
                                uint16_t buffer_cap,
                                uint16_t requested_len,
                                uint16_t first_path_index,
                                uint16_t *start_index)
{
    uint8_t accum[DWM3000_FULL_CIR_READ_CHUNK_BYTES + 1u];
    uint16_t total_len;
    uint16_t total_samples;
    uint16_t start_sample;
    uint16_t sample_index;
    uint16_t byte_offset = 0u;

    if (start_index != NULL) {
        *start_index = 0u;
    }
    if (buffer == NULL || buffer_cap == 0u) {
        return 0u;
    }

    total_len = MIN(requested_len, buffer_cap);
    total_len -= total_len % DWM3000_FULL_CIR_SAMPLE_BYTES;
    total_samples = total_len / DWM3000_FULL_CIR_SAMPLE_BYTES;
    start_sample = cir_window_start_sample(first_path_index, total_samples);
    sample_index = start_sample;
    if (start_index != NULL) {
        *start_index = start_sample;
    }
    while (sample_index < start_sample + total_samples) {
        uint16_t chunk_samples =
            MIN((uint16_t)DWM3000_FULL_CIR_READ_CHUNK_SAMPLES,
                (uint16_t)(start_sample + total_samples - sample_index));
        uint16_t chunk_len = chunk_samples * DWM3000_FULL_CIR_SAMPLE_BYTES;

        dwt_readaccdata(accum, chunk_len + 1u, sample_index);
        memcpy(&buffer[byte_offset], &accum[1], chunk_len);
        sample_index += chunk_samples;
        byte_offset += chunk_len;
    }
    return byte_offset;
}

static void read_rx_diagnostics(int8_t *rsl_dbm,
                                uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                                bool *cir_sampled,
                                int16_t *clock_offset_raw,
                                bool *clock_offset_sampled,
                                int32_t *carrier_integrator,
                                bool *carrier_integrator_sampled)
{
    dwt_rxdiag_t diagnostics;

    if (rsl_dbm == NULL && cir_sample == NULL &&
        clock_offset_raw == NULL && carrier_integrator == NULL) {
        return;
    }
    if (cir_sampled != NULL) {
        *cir_sampled = false;
    }
    if (clock_offset_sampled != NULL) {
        *clock_offset_sampled = false;
    }
    if (carrier_integrator_sampled != NULL) {
        *carrier_integrator_sampled = false;
    }

    memset(&diagnostics, 0, sizeof(diagnostics));
    dwt_readdiagnostics(&diagnostics);
    if (rsl_dbm != NULL) {
        *rsl_dbm = estimate_ipatov_rsl_dbm(&diagnostics);
    }
    if (read_cir_sample(&diagnostics, cir_sample)) {
        if (cir_sampled != NULL) {
            *cir_sampled = true;
        }
    }
    if (clock_offset_raw != NULL) {
        *clock_offset_raw = dwt_readclockoffset();
        if (clock_offset_sampled != NULL) {
            *clock_offset_sampled = true;
        }
    }
    if (carrier_integrator != NULL) {
        *carrier_integrator = dwt_readcarrierintegrator();
        if (carrier_integrator_sampled != NULL) {
            *carrier_integrator_sampled = true;
        }
    }
}

static uint16_t read_rx_frame(uint8_t *buffer, size_t buffer_len)
{
    uint32_t rx_finfo = dwt_read32bitreg(RX_FINFO_ID);
    uint32_t frame_len;

    last_rx_finfo_register = rx_finfo;
    frame_len = rx_finfo & RX_FINFO_RXFLEN_BIT_MASK;

    if (take_port_error("read-rx-frame-length") < 0) {
        return 0u;
    }
    if (frame_len > buffer_len) {
        return 0u;
    }

    dwt_readrxdata(buffer, (uint16_t)frame_len, 0);
    if (take_port_error("read-rx-frame-data") < 0) {
        return 0u;
    }
    return (uint16_t)frame_len;
}

static size_t payload_len_without_fcs(size_t frame_len)
{
    if (frame_len >= FCS_LEN) {
        return frame_len - FCS_LEN;
    }
    return frame_len;
}

static int decode_poll_frame(const uint8_t *buffer, size_t frame_len,
                             struct uwb_range_header *header)
{
    int ret;

    ret = uwb_decode_poll(buffer, frame_len, header);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_poll(buffer, payload_len_without_fcs(frame_len), header);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_response_frame(const uint8_t *buffer, size_t frame_len,
                                 struct uwb_response_frame *frame)
{
    int ret;

    ret = uwb_decode_response(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_response(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_final_frame(const uint8_t *buffer, size_t frame_len,
                              struct uwb_final_frame *frame)
{
    int ret;

    ret = uwb_decode_final(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_final(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_report_frame(const uint8_t *buffer, size_t frame_len,
                               struct uwb_report_frame *frame)
{
    int ret;

    ret = uwb_decode_report(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_report(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_clicker_diag_frame(const uint8_t *buffer, size_t frame_len,
                                     struct uwb_clicker_diag_frame *frame)
{
    int ret;

    ret = uwb_decode_clicker_diag(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_clicker_diag(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_anchor_diag_frame(const uint8_t *buffer, size_t frame_len,
                                    struct uwb_anchor_diag_frame *frame)
{
    int ret;

    ret = uwb_decode_anchor_diag(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_anchor_diag(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_anchor_diag_fragment_frame(
    const uint8_t *buffer,
    size_t frame_len,
    struct uwb_anchor_diag_fragment_frame *frame)
{
    int ret;

    ret = uwb_decode_anchor_diag_fragment(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_anchor_diag_fragment(buffer,
                                          payload_len_without_fcs(frame_len),
                                          frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int write_tx_frame(const uint8_t *frame, size_t frame_len)
{
    if (frame == NULL || frame_len > UINT16_MAX) {
        return -EINVAL;
    }

    if (dwt_writetxdata((uint16_t)frame_len, (uint8_t *)frame, 0) != DWT_SUCCESS) {
        return -EIO;
    }

    dwt_writetxfctrl((uint16_t)(frame_len + FCS_LEN), 0, 1);
    return 0;
}

static int patch_tx_frame(const uint8_t *bytes,
                          size_t length,
                          uint16_t offset)
{
    if (bytes == NULL || length == 0u || length > UINT16_MAX) {
        return -EINVAL;
    }
    if ((size_t)offset + length > UWB_PHY_EXTENDED_FRAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    if (dwt_writetxdata((uint16_t)length,
                        (uint8_t *)bytes,
                        offset) != DWT_SUCCESS) {
        return -EIO;
    }
    return take_port_error("tx-frame-patch");
}

/* Poll acceptance remains here because it includes optional round-index
 * reply-delay calibration that only the Zephyr radio build knows about. */
static bool poll_targets_anchor(const struct uwb_range_header *header,
                                uint64_t local_anchor_id)
{
    return header->responder_short_addr == short_addr_from_id(local_anchor_id) &&
           header->responder_id == local_anchor_id;
}

static bool poll_matches_expected(const struct uwb_range_header *poll,
                                  const struct dwm3000_range_request *expected)
{
    uint16_t expected_responder_short_addr;

    expected_responder_short_addr = expected->responder_short_addr != 0u ?
                                    expected->responder_short_addr :
                                    short_addr_from_id(expected->responder_id);
    return poll->initiator_short_addr == short_addr_from_id(expected->initiator_id) &&
           poll->responder_short_addr == expected_responder_short_addr &&
           poll->initiator_id == expected->initiator_id &&
           poll->responder_id == expected->responder_id &&
           poll->network_id == expected->network_id &&
           poll->session_id == expected->session_id &&
           poll->session_nonce == expected->session_nonce &&
           poll->seq == expected->seq &&
#if DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
           reply_delay_uus_from_round_index(poll->round_index) >=
           DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS &&
#else
           poll->round_index == expected->round_index &&
#endif
           poll->flags == expected->flags;
}

static bool range_flags_valid(uint8_t flags)
{
    return flags == FLAG_DIAGNOSTIC ||
           flags == (FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY) ||
           flags == FLAG_COUNT_AS_CLICK;
}

static bool request_sends_clicker_diag(const struct dwm3000_range_request *request)
{
    return DWM3000_CLICKER_DIAG_ENABLED &&
           request != NULL &&
           request->send_clicker_diag &&
           request->round_index == 0u;
}

static bool request_expects_clicker_diag(const struct dwm3000_range_request *request)
{
    return DWM3000_CLICKER_DIAG_ENABLED &&
           request != NULL &&
           request->expect_clicker_diag &&
           request->round_index == 0u;
}

static bool request_expects_anchor_diag(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->expect_anchor_diag &&
           request->round_index == 0u;
}

static bool request_expects_anchor_diag_fragments(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->expect_anchor_diag_fragments &&
           request->anchor_full_cir != NULL &&
           request->anchor_full_cir_cap > 0u &&
           request->round_index == 0u;
}

static bool request_sends_anchor_diag(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->send_anchor_diag &&
           request->round_index == 0u;
}

static bool request_sends_anchor_diag_fragments(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->send_anchor_diag_fragments &&
           request->round_index == 0u;
}

static bool request_captures_anchor_full_cir(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->anchor_full_cir != NULL &&
           request->anchor_full_cir_cap > 0u &&
           request->round_index == 0u;
}
