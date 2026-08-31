#line 487 __BASE_FILE__
static void clear_all_events(void);

static uint32_t radio_transition_trace_next(void)
{
    return (uint32_t)atomic_inc(&radio_transition_trace_seq) + 1u;
}

#define DWM3000_TRANSITION_TRACE(...)                                      \
    do {                                                                   \
        if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {             \
            status_debug_printf(__VA_ARGS__);                              \
        }                                                                  \
    } while (false)

static void invalidate_radio_state_tagged(const char *tag)
{
    status_debug_printf(
        "DBG_DWM_INVALIDATE tag=%s active_phy=%u configured=%u awake=%u\n",
        tag == NULL ? "unknown" : tag,
        (unsigned int)active_phy_mode,
        radio_configured ? 1u : 0u,
        radio_awake ? 1u : 0u);
    radio_configured = false;
    radio_awake = false;
    radio_restored_from_sleep = false;
    active_phy_mode = DWM3000_PHY_NONE;
    radio_state_unknown = true;
}

static void mark_radio_awake_unconfigured_tagged(const char *tag)
{
    status_debug_printf(
        "DBG_DWM_MARK_UNCONFIGURED tag=%s prev_phy=%u\n",
        tag == NULL ? "unknown" : tag,
        (unsigned int)active_phy_mode);
    radio_configured = false;
    radio_awake = true;
    radio_restored_from_sleep = false;
    active_phy_mode = DWM3000_PHY_NONE;
    radio_state_unknown = false;
}

static int take_port_error(const char *operation)
{
    int ret = dwm3000_port_take_error();

    if (ret >= 0) {
        return 0;
    }
    driver_stats.spi_failures++;
    invalidate_radio_state_tagged(__func__);
    LOG_ERR("DWM3000 port error: operation=%s ret=%d",
            operation == NULL ? "unknown" : operation,
            ret);
    return ret;
}

static int check_device_fatal_status(const char *operation)
{
    uint32_t status_hi;
    uint32_t fatal_status;
    int ret;

    status_hi = dwt_read32bitreg(SYS_STATUS_HI_ID);
    ret = take_port_error(operation);
    if (ret < 0) {
        return ret;
    }

    fatal_status = status_hi & DWM3000_SYS_STATUS_HI_FATAL_MASK;
    if (fatal_status == 0u) {
        return 0;
    }

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && \
    defined(CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE)
    {
        uint32_t original_spi_hz = dwm3000_port_current_spi_hz();
        uint32_t fint_status;
        uint32_t status_low;
        uint32_t status_hi_repeat;
        uint32_t dev_id;
        uint16_t status_hi_short;
        int fint_ret;
        int status_low_ret;
        int status_hi_short_ret;
        int status_hi_repeat_ret;
        int dev_id_ret;
        int slow_spi_ret;
        int restore_spi_ret;

        /*
         * This path runs only after the operation is already terminal.  The
         * SDK treats SYS_STATUS_HI as authoritative only when FINT_STAT has
         * SYS_PANIC set, and reads the meaningful low 16 bits there.  Capture
         * both widths plus the neighbouring status and identity registers at
         * the current and slow SPI rates.  That separates a genuine device
         * panic from an address/header or retained-wake read failure without
         * changing the successful radio path or its timing.
         */
        dwm3000_port_clear_error();
        fint_status = dwt_read32bitreg(FINT_STAT_ID);
        fint_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_low = dwt_read32bitreg(SYS_STATUS_ID);
        status_low_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_hi_short = dwt_read16bitoffsetreg(SYS_STATUS_HI_ID, 0);
        status_hi_short_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_hi_repeat = dwt_read32bitreg(SYS_STATUS_HI_ID);
        status_hi_repeat_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        dev_id = dwt_readdevid();
        dev_id_ret = dwm3000_port_take_error();
        status_debug_printf(
            "DBG_DWM_FATAL_CURRENT op=%s spi=%u first=%08x fatal=%04x panic=%u\n",
            operation == NULL ? "unknown" : operation,
            (unsigned int)original_spi_hz,
            (unsigned int)status_hi,
            (unsigned int)fatal_status,
            (fint_status & FINT_STAT_SYS_PANIC_BIT_MASK) != 0u ? 1u : 0u);
        status_debug_printf(
            "DBG_DWM_FATAL_CURRENT_REG fint=%08x low=%08x hi16=%04x hi32=%08x id=%08x\n",
            (unsigned int)fint_status,
            (unsigned int)status_low,
            (unsigned int)status_hi_short,
            (unsigned int)status_hi_repeat,
            (unsigned int)dev_id);
        status_debug_printf(
            "DBG_DWM_FATAL_CURRENT_RET fint=%d low=%d hi16=%d hi32=%d id=%d\n",
            fint_ret, status_low_ret, status_hi_short_ret,
            status_hi_repeat_ret, dev_id_ret);

        slow_spi_ret = dwm3000_port_set_slow_spi();
        dwm3000_port_clear_error();
        fint_status = dwt_read32bitreg(FINT_STAT_ID);
        fint_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_low = dwt_read32bitreg(SYS_STATUS_ID);
        status_low_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_hi_short = dwt_read16bitoffsetreg(SYS_STATUS_HI_ID, 0);
        status_hi_short_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        status_hi_repeat = dwt_read32bitreg(SYS_STATUS_HI_ID);
        status_hi_repeat_ret = dwm3000_port_take_error();
        dwm3000_port_clear_error();
        dev_id = dwt_readdevid();
        dev_id_ret = dwm3000_port_take_error();
        restore_spi_ret = original_spi_hz > 7000000u ?
            dwm3000_port_set_fast_spi() : dwm3000_port_set_slow_spi();

        status_debug_printf(
            "DBG_DWM_FATAL_SLOW_REG fint=%08x low=%08x hi16=%04x hi32=%08x id=%08x\n",
            (unsigned int)fint_status,
            (unsigned int)status_low,
            (unsigned int)status_hi_short,
            (unsigned int)status_hi_repeat,
            (unsigned int)dev_id);
        status_debug_printf(
            "DBG_DWM_FATAL_SLOW_RET set=%d fint=%d low=%d hi16=%d hi32=%d id=%d restore=%d\n",
            slow_spi_ret, fint_ret, status_low_ret, status_hi_short_ret,
            status_hi_repeat_ret, dev_id_ret, restore_spi_ret);
        status_debug_printf(
            "DBG_DWM_FATAL_PROBE3 cfg=%u awake=%u unknown=%u phy=%u tid=%p uptime=%u\n",
            radio_configured ? 1u : 0u,
            radio_awake ? 1u : 0u,
            radio_state_unknown ? 1u : 0u,
            (unsigned int)active_phy_mode,
            k_current_get(),
            k_uptime_get_32());
    }
#endif

    dwt_write32bitreg(SYS_STATUS_HI_ID, fatal_status);
    ret = take_port_error("device-fatal-status-clear");
    if (ret >= 0) {
        driver_stats.spi_failures++;
    }
    status_debug_printf(
        "DBG_DWM_FATAL operation=%s status_hi=0x%08x fatal=0x%08x clear_ret=%d\n",
        operation == NULL ? "unknown" : operation,
        (unsigned int)status_hi,
        (unsigned int)fatal_status,
        ret);
    invalidate_radio_state_tagged(__func__);
    LOG_ERR("DWM3000 fatal command/SPI status: operation=%s status_hi=0x%08x",
            operation == NULL ? "unknown" : operation,
            status_hi);
    return ret < 0 ? ret : -EIO;
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
    return check_device_fatal_status(operation);
}

static void configure_first_path_sensitivity(void)
{
    /*
     * Keep Qorvo's lower recommended threshold so a weak genuine first path
     * remains visible in NLOS. Very-close links can occasionally produce a
     * negative calibrated ToF; the distance contract preserves those valid
     * exchanges as 0 mm samples for the multi-sample estimator to absorb.
     */
    dwt_modify8bitoffsetreg(IP_CONFIG_LO_ID,
                            0,
                            (uint8_t)~IP_CONFIG_LO_IP_NTM_BIT_MASK,
                            DWM3000_FIRST_PATH_NTM);
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

static int64_t timestamp_delta_40(uint64_t lhs, uint64_t rhs)
{
    const uint64_t mask = (UINT64_C(1) << 40) - 1u;
    const uint64_t sign = UINT64_C(1) << 39;
    uint64_t delta = (lhs - rhs) & mask;

    if ((delta & sign) != 0u) {
        return (int64_t)(delta - (UINT64_C(1) << 40));
    }
    return (int64_t)delta;
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
    uint32_t trace_id = radio_transition_trace_next();
    int initialise_ret;
    int ret;
    int mode = idle_after_init ? DWT_DW_IDLE : DWT_DW_INIT;

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=entry tid=%p idle=%u spi=%u\n",
        trace_id, k_current_get(), idle_after_init ? 1u : 0u,
        (unsigned int)dwm3000_port_current_spi_hz());
    ret = dwm3000_port_init();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=port_init ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        return ret;
    }

    ret = ensure_local_data();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=local_data ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }

    dwm3000_port_clear_error();
    ret = dwm3000_port_set_slow_spi();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=slow_spi ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        return ret;
    }
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=dwt_initialise_begin mode=%d\n",
        trace_id, mode);
    initialise_ret = dwt_initialise(mode);
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=dwt_initialise_end ret=%d\n",
        trace_id, initialise_ret);
    ret = take_port_error("initialise");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=port_error ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }
    ret = check_device_fatal_status("initialise");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=fatal_status ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }
    if (initialise_ret != DWT_SUCCESS) {
        return -EIO;
    }
    ret = validate_device_identity("initialise");
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=identity ret=%d\n", trace_id, ret);
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_port_set_fast_spi();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=fast_spi ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        invalidate_radio_state_tagged(__func__);
        return ret;
    }
    mark_radio_awake_unconfigured_tagged(__func__);
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_INIT id=%u step=done configured=%u awake=%u unknown=%u phy=%u\n",
        trace_id, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u, (unsigned int)active_phy_mode);
    return 0;
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
    int configure_ret;
    int ret;

    if (config == NULL) {
        return -EINVAL;
    }

    dwm3000_port_clear_error();

    /* Qorvo requires the IC to be returned to idle before dwt_configure(). */
    if (radio_configured && radio_awake) {
        dwt_forcetrxoff();
        clear_all_events();
    }

    configure_ret = dwt_configure((dwt_config_t *)config);
    ret = take_port_error("configure");
    if (ret < 0) {
        return ret;
    }
    ret = check_device_fatal_status("configure");
    if (ret < 0) {
        return ret;
    }
    if (configure_ret != DWT_SUCCESS) {
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
        ret = take_port_error("apply-config");

        if (ret < 0) {
            return ret;
        }
        ret = check_device_fatal_status("apply-config");
        if (ret < 0) {
            return ret;
        }
    }

    radio_configured = true;
    radio_awake = true;
    radio_restored_from_sleep = false;
    active_phy_mode = phy_mode;
    radio_state_unknown = false;
    return 0;
}

static int restore_txrx_after_sleep(enum dwm3000_phy_mode phy_mode)
{
    int restore_ret;
    int ret;

    dwm3000_port_clear_error();
    restore_ret = dwt_restore_txrx(DWT_RESTORE_TXRX_MODE);
    ret = take_port_error("restore-txrx-command");
    if (ret < 0) {
        return ret;
    }
    ret = check_device_fatal_status("restore-txrx-command");
    if (ret < 0) {
        return ret;
    }
    if (restore_ret != DWT_SUCCESS) {
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
    ret = check_device_fatal_status("restore-txrx");
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
    uint32_t trace_id = radio_transition_trace_next();
    int ret;

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=entry tid=%p requested_phy=%u configured=%u awake=%u unknown=%u active_phy=%u spi=%u\n",
        trace_id, k_current_get(), (unsigned int)phy_mode,
        radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u, (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    invalidate_radio_state_tagged(__func__);

    ret = dwm3000_port_init();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=port_init ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_PORT_INIT_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_wakeup();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=wake_before_reset ret=%d\n",
        trace_id, ret);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_WAKE_PIN_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_hw_reset();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=hw_reset ret=%d spi=%u\n",
        trace_id, ret, (unsigned int)dwm3000_port_current_spi_hz());
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_HW_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    /* nRESET leaves the DW3000 with its wake logic latched off on some
     * power-state histories; re-assert the wake pin so the first fast-SPI
     * transactions land on an awake device instead of returning the device
     * ID for every register. */
    ret = dwm3000_port_wakeup();
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=wake_after_reset ret=%d\n",
        trace_id, ret);
    if (ret < 0) {
        return ret;
    }

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=initialise_begin\n", trace_id);
    ret = initialise_radio(false);
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=initialise_end ret=%d\n", trace_id, ret);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_RESET_INIT_FAIL phy=%d ret=%d sys=0x%08x\n",
                                phy_mode, ret, (unsigned int)sys_status);
        }
        return ret;
    }

    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=apply_begin phy=%u spi=%u\n",
        trace_id, (unsigned int)phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    ret = apply_radio_config(config, phy_mode);
    DWM3000_TRANSITION_TRACE(
        "DBG_DWM_RESET id=%u step=done ret=%d configured=%u awake=%u unknown=%u active_phy=%u spi=%u\n",
        trace_id, ret, radio_configured ? 1u : 0u, radio_awake ? 1u : 0u,
        radio_state_unknown ? 1u : 0u, (unsigned int)active_phy_mode,
        (unsigned int)dwm3000_port_current_spi_hz());
    return ret;
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
        invalidate_radio_state_tagged(__func__);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_SLOW_SPI_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
        return ret;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        invalidate_radio_state_tagged(__func__);
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
        invalidate_radio_state_tagged(__func__);
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
    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        invalidate_radio_state_tagged(__func__);
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
    radio_awake = true;
    radio_restored_from_sleep = !restore_txrx;
    radio_state_unknown = false;
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
    return ensure_phy_mode(
        !radio_configured || active_phy_mode == DWM3000_PHY_NONE ?
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

static int clear_status_checked(uint32_t mask, const char *operation)
{
    int ret;

    clear_status(mask);
    ret = take_port_error(operation);
    if (ret < 0) {
        return ret;
    }
    return check_device_fatal_status(operation);
}

static int wait_status_internal(uint32_t mask,
                                uint32_t timeout_ms,
                                uint32_t *status,
                                bool log_events,
                                uint64_t *observed_at_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint32_t read_status;
    uint32_t start_cycles = k_cycle_get_32();
    uint32_t poll_loops = 0u;
    int64_t now_ms;
    uint64_t sampled_at_ms = 0u;

    if (observed_at_ms != NULL) {
        *observed_at_ms = 0u;
    }
    {
        int ret = check_device_fatal_status("status-entry");

        if (ret < 0) {
            if (status != NULL) {
                *status = 0u;
            }
            return ret;
        }
    }
    dwm3000_port_clear_error();
    if (log_events) {
    }
    do {
        read_status = dwt_read32bitreg(SYS_STATUS_ID);
        sampled_at_ms = (uint64_t)k_uptime_get();
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
            int ret = check_device_fatal_status("status-match");

            if (ret < 0) {
                if (IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE)) {
                    status_debug_printf(
                        "DBG_DWM_STATUS_FATAL phase=match low=0x%08x mask=0x%08x polls=%u elapsed_us=%u ret=%d\n",
                        (unsigned int)read_status,
                        (unsigned int)mask,
                        (unsigned int)poll_loops,
                        (unsigned int)elapsed_us,
                        ret);
                }
                if (status != NULL) {
                    *status = read_status;
                }
                return ret;
            }

            driver_stats.sys_status_poll_loops += poll_loops;
            if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
                driver_stats.sys_status_poll_max_duration_us = elapsed_us;
            }
            if (status != NULL) {
                *status = read_status;
            }
            if (observed_at_ms != NULL) {
                *observed_at_ms = sampled_at_ms;
            }
            if (log_events) {
            }
            return 0;
        }

        /*
         * Edge owners need one receive boundary and are consumed together by
         * that boundary.  Gateway priority is a level owner: it remains armed
         * until the exact priority lifecycle clears it, preventing a scan
         * rearm from stealing the safe boundary.  Separate owner bits ensure
         * one lifecycle cannot erase another requester's pending abort.
         */
        if (atomic_get(&receive_abort_enabled) != 0) {
            atomic_val_t pending = atomic_get(&receive_abort_owners);
            atomic_val_t edge_owners =
                pending & ~(atomic_val_t)DWM3000_RECEIVE_ABORT_LEVEL_MASK;

            if (edge_owners != 0) {
                (void)atomic_and(&receive_abort_owners, ~edge_owners);
            }
            if (pending == 0) {
                goto receive_abort_not_requested;
            }
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
            }
            return -ECANCELED;
        }

receive_abort_not_requested:
        now_ms = k_uptime_get();
        if (now_ms >= deadline) {
            break;
        }
        k_busy_wait(DWM3000_STATUS_POLL_INTERVAL_US);
    } while (k_uptime_get() <= deadline);

    /*
     * The deadline can expire between the loop's status read and its time
     * check.  Sample once more so a completion at that boundary is not
     * reported as a timeout, and preserve a final SPI failure as a hardware
     * error instead of collapsing it into ordinary RF silence.
    */
    read_status = dwt_read32bitreg(SYS_STATUS_ID);
    sampled_at_ms = (uint64_t)k_uptime_get();
    poll_loops++;
    {
        int ret = take_port_error("status-timeout-read");

        if (ret < 0) {
            if (status != NULL) {
                *status = 0u;
            }
            return ret;
        }
    }
    if (status != NULL) {
        *status = read_status;
    }
    if ((read_status & mask) != 0u) {
        uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - start_cycles));
        int ret = check_device_fatal_status("status-boundary-match");

        if (ret < 0) {
            if (IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE)) {
                status_debug_printf(
                    "DBG_DWM_STATUS_FATAL phase=boundary low=0x%08x mask=0x%08x polls=%u elapsed_us=%u ret=%d\n",
                    (unsigned int)read_status,
                    (unsigned int)mask,
                    (unsigned int)poll_loops,
                    (unsigned int)elapsed_us,
                    ret);
            }
            return ret;
        }

        driver_stats.sys_status_poll_loops += poll_loops;
        if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
            driver_stats.sys_status_poll_max_duration_us = elapsed_us;
        }
        if (log_events) {
        }
        if (observed_at_ms != NULL) {
            *observed_at_ms = sampled_at_ms;
        }
        return 0;
    }
    {
        int ret = check_device_fatal_status("status-timeout");

        if (ret < 0) {
            return ret;
        }
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
        }
    }
    return -ETIMEDOUT;
}

static int wait_status(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    return wait_status_internal(mask, timeout_ms, status, true, NULL);
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
                               false,
                               NULL);
    if (ret < 0) {
        return ret;
    }

    *tx_timestamp = read_tx_timestamp_u64();
    ret = take_port_error("tx-timestamp");
    if (ret < 0) {
        return ret;
    }
    return check_device_fatal_status("tx-timestamp");
}

static int wait_tx_complete_observed(uint32_t timeout_ms,
                                     uint64_t *completed_at_ms)
{
    uint32_t status = 0u;
    int ret;

    ret = wait_status_internal(SYS_STATUS_TXFRS_BIT_MASK,
                               timeout_ms,
                               &status,
                               true,
                               completed_at_ms);
    if (ret < 0) {
        driver_stats.tx_failures++;
        return ret;
    }

    ret = clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK,
                               "tx-complete-clear");
    if (ret < 0) {
        driver_stats.tx_failures++;
        return ret;
    }
    driver_stats.tx_dones++;
    return 0;
}

static int wait_tx_complete(uint32_t timeout_ms)
{
    return wait_tx_complete_observed(timeout_ms, NULL);
}

static enum range_status status_to_range_status(uint32_t status)
{
    if ((status & DWM3000_RX_ERROR_STATUS_MASK) != 0u) {
        return RANGE_RX_ERROR;
    }
    if ((status & SYS_STATUS_ALL_RX_TO) != 0u) {
        return RANGE_RX_TIMEOUT;
    }
    return RANGE_INTERNAL_ERROR;
}

static enum dwm3000_rx_failure status_to_rx_failure(uint32_t status)
{
    if ((status & (SYS_STATUS_ARFE_BIT_MASK |
                   SYS_STATUS_CIAERR_BIT_MASK |
                   SYS_STATUS_RXOVRR_BIT_MASK)) != 0u) {
        return DWM3000_RX_FAILURE_CRC_OR_PHY;
    }
    if ((status & SYS_STATUS_RXPTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXSTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_SFD_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXFTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    if ((status & DWM3000_RX_ERROR_STATUS_MASK) != 0u) {
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
                      SYS_STATUS_RXFSL_BIT_MASK |
                      SYS_STATUS_ARFE_BIT_MASK |
                      SYS_STATUS_CIAERR_BIT_MASK |
                      SYS_STATUS_RXOVRR_BIT_MASK)) != 0u;
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

#if defined(CONFIG_IMEC_ML_ANCHOR)
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

static bool ipatov_diagnostics_valid(const dwt_rxdiag_t *diagnostics,
                                     bool require_power)
{
    uint16_t first_path_index;

    if (diagnostics == NULL || diagnostics->ipatovAccumCount == 0u) {
        return false;
    }
    first_path_index = diagnostics->ipatovFpIndex >> 6;
    if (first_path_index >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        return false;
    }
    return !require_power || diagnostics->ipatovPower != 0u;
}

static bool read_cir_sample(const dwt_rxdiag_t *diagnostics,
                            uint8_t cir_sample[UWB_CIR_SAMPLE_LEN])
{
    uint8_t accum[DWM3000_ACCUM_CIR_SAMPLE_READ_LEN] = {0};
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
    if (take_port_error("rx-diagnostics-raw") < 0 ||
        check_device_fatal_status("rx-diagnostics-raw") < 0) {
        return false;
    }
    if (!ipatov_diagnostics_valid(&diagnostics, false)) {
        return false;
    }
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
        if (take_port_error("rx-cir-window") < 0 ||
            check_device_fatal_status("rx-cir-window") < 0) {
            memset(buffer, 0, total_len);
            if (start_index != NULL) {
                *start_index = 0u;
            }
            return 0u;
        }
        memcpy(&buffer[byte_offset], &accum[1], chunk_len);
        sample_index += chunk_samples;
        byte_offset += chunk_len;
    }
    return byte_offset;
}

static bool read_rx_diagnostics(int8_t *rsl_dbm,
                                uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                                bool *cir_sampled,
                                int16_t *clock_offset_raw,
                                bool *clock_offset_sampled,
                                int32_t *carrier_integrator,
                                bool *carrier_integrator_sampled,
                                uint64_t *ipatov_rx_timestamp)
{
    dwt_rxdiag_t diagnostics;

    if (rsl_dbm == NULL && cir_sample == NULL &&
        clock_offset_raw == NULL && carrier_integrator == NULL &&
        ipatov_rx_timestamp == NULL) {
        return true;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = 0;
    }
    if (cir_sample != NULL) {
        memset(cir_sample, 0, UWB_CIR_SAMPLE_LEN);
    }
    if (cir_sampled != NULL) {
        *cir_sampled = false;
    }
    if (clock_offset_raw != NULL) {
        *clock_offset_raw = 0;
    }
    if (clock_offset_sampled != NULL) {
        *clock_offset_sampled = false;
    }
    if (carrier_integrator != NULL) {
        *carrier_integrator = 0;
    }
    if (carrier_integrator_sampled != NULL) {
        *carrier_integrator_sampled = false;
    }
    if (ipatov_rx_timestamp != NULL) {
        *ipatov_rx_timestamp = 0u;
    }

    memset(&diagnostics, 0, sizeof(diagnostics));
    dwt_readdiagnostics(&diagnostics);
    if (take_port_error("rx-diagnostics-snapshot") < 0 ||
        check_device_fatal_status("rx-diagnostics-snapshot") < 0 ||
        !ipatov_diagnostics_valid(&diagnostics, rsl_dbm != NULL)) {
        return false;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = estimate_ipatov_rsl_dbm(&diagnostics);
    }
    if (ipatov_rx_timestamp != NULL) {
        *ipatov_rx_timestamp = timestamp_to_u64(diagnostics.ipatovRxTime);
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
    if (take_port_error("rx-diagnostics") < 0 ||
        check_device_fatal_status("rx-diagnostics") < 0) {
        if (rsl_dbm != NULL) {
            *rsl_dbm = 0;
        }
        if (cir_sample != NULL) {
            memset(cir_sample, 0, UWB_CIR_SAMPLE_LEN);
        }
        if (cir_sampled != NULL) {
            *cir_sampled = false;
        }
        if (clock_offset_raw != NULL) {
            *clock_offset_raw = 0;
        }
        if (clock_offset_sampled != NULL) {
            *clock_offset_sampled = false;
        }
        if (carrier_integrator != NULL) {
            *carrier_integrator = 0;
        }
        if (carrier_integrator_sampled != NULL) {
            *carrier_integrator_sampled = false;
        }
        return false;
    }
    return true;
}

static int read_rx_frame(uint8_t *buffer,
                         size_t buffer_len,
                         size_t *frame_len_out)
{
    struct uwb_rf_scope local_scope;
    struct uwb_rf_scope remote_scope;
    uint32_t rx_finfo = dwt_read32bitreg(RX_FINFO_ID);
    uint32_t raw_frame_len;
    uint32_t frame_len;
    uint8_t scope_wire = 0u;
    int ret;

    if (buffer == NULL || frame_len_out == NULL) {
        return -EINVAL;
    }
    *frame_len_out = 0u;
    last_rx_finfo_register = rx_finfo;
    raw_frame_len = rx_finfo & RX_FINFO_RXFLEN_BIT_MASK;

    {
        ret = take_port_error("read-rx-frame-length");

        if (ret < 0) {
            return ret;
        }
        ret = check_device_fatal_status("read-rx-frame-length");
        if (ret < 0) {
            return ret;
        }
    }
    if (raw_frame_len <= UWB_RF_SCOPE_WIRE_LEN + FCS_LEN) {
        return -EBADMSG;
    }
    dwt_readrxdata(&scope_wire, UWB_RF_SCOPE_WIRE_LEN, 0u);
    ret = take_port_error("read-rx-frame-scope");
    if (ret < 0) {
        return ret;
    }
    ret = check_device_fatal_status("read-rx-frame-scope");
    if (ret < 0) {
        return ret;
    }
    if (dwm3000_local_rf_scope(&local_scope) < 0 ||
        uwb_rf_scope_decode(scope_wire, &remote_scope) < 0) {
        return -EBADMSG;
    }
    if (!uwb_rf_scope_visible(&local_scope, &remote_scope)) {
        driver_stats.rx_scope_drops++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf(
                "DBG_DWM_RF_SCOPE_DROP local=%u remote=%u remote_clicker=%u\n",
                local_scope.layer,
                remote_scope.layer,
                remote_scope.clicker ? 1u : 0u);
        }
        return -EHOSTUNREACH;
    }

    frame_len = raw_frame_len - UWB_RF_SCOPE_WIRE_LEN;
    if (frame_len > buffer_len) {
        return -EMSGSIZE;
    }

    dwt_readrxdata(buffer,
                   (uint16_t)frame_len,
                   UWB_RF_SCOPE_WIRE_LEN);
    {
        ret = take_port_error("read-rx-frame-data");

        if (ret < 0) {
            return ret;
        }
        ret = check_device_fatal_status("read-rx-frame-data");
        if (ret < 0) {
            return ret;
        }
    }
    *frame_len_out = frame_len;
    return 0;
}

static size_t payload_len_without_fcs(size_t frame_len)
{
    if (frame_len >= FCS_LEN) {
        return frame_len - FCS_LEN;
    }
    return frame_len;
}

static int decode_poll_frame(const uint8_t *buffer, size_t frame_len,
                             struct uwb_poll_frame *poll)
{
    int ret;

    ret = uwb_decode_poll_frame(buffer, frame_len, poll);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_poll_frame(buffer,
                                payload_len_without_fcs(frame_len),
                                poll);
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
    struct uwb_rf_scope local_scope;
    const dwt_config_t *config;
    size_t max_frame_len;
    uint8_t scope_wire;
    int ret;

    if (frame == NULL || frame_len == 0u) {
        return -EINVAL;
    }
    config = config_for_phy(active_phy_mode);
    max_frame_len = config->phrMode == DWT_PHRMODE_EXT ?
                    UWB_PHY_EXTENDED_FRAME_MAX_LEN :
                    DWM3000_STANDARD_FRAME_MAX_LEN;
    if (frame_len > max_frame_len - FCS_LEN - UWB_RF_SCOPE_WIRE_LEN ||
        dwm3000_local_rf_scope(&local_scope) < 0 ||
        uwb_rf_scope_encode(&local_scope, &scope_wire) < 0) {
        return -EMSGSIZE;
    }

    if (dwt_writetxdata(UWB_RF_SCOPE_WIRE_LEN, &scope_wire, 0u) !=
        DWT_SUCCESS) {
        ret = take_port_error("tx-frame-scope");
        return ret < 0 ? ret : -EIO;
    }
    ret = take_port_error("tx-frame-scope");
    if (ret < 0) {
        return ret;
    }
    if (dwt_writetxdata((uint16_t)frame_len,
                        (uint8_t *)frame,
                        UWB_RF_SCOPE_WIRE_LEN) != DWT_SUCCESS) {
        ret = take_port_error("tx-frame-data");
        return ret < 0 ? ret : -EIO;
    }

    dwt_writetxfctrl(
        (uint16_t)(UWB_RF_SCOPE_WIRE_LEN + frame_len + FCS_LEN), 0, 1);
    return take_port_error("tx-frame-control");
}

static int patch_tx_frame(const uint8_t *bytes,
                          size_t length,
                          uint16_t offset)
{
    if (bytes == NULL || length == 0u || length > UINT16_MAX) {
        return -EINVAL;
    }
    if ((size_t)offset + length + UWB_RF_SCOPE_WIRE_LEN >
        UWB_PHY_EXTENDED_FRAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    if (dwt_writetxdata((uint16_t)length,
                        (uint8_t *)bytes,
                        (uint16_t)(offset + UWB_RF_SCOPE_WIRE_LEN)) !=
        DWT_SUCCESS) {
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
