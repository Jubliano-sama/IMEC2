#line 2033 __BASE_FILE__
int dwm3000_driver_capture_last_rx_cir(uint8_t *buffer,
                                      uint16_t buffer_cap,
                                      struct dwm3000_range_result *result)
{
    uint32_t started_ms;
    uint32_t completed_ms;

    if (buffer == NULL || buffer_cap == 0u || result == NULL) {
        return -EINVAL;
    }

    started_ms = k_uptime_get_32();
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=cir-capture-start now=%u seq=%u round=%u cap=%u\n",
                            started_ms,
                            result->seq,
                            result->round_index,
                            buffer_cap);
    }
    result->anchor_rx_diag_sampled =
        capture_rx_diag_raw(result->anchor_rx_diag_raw,
                            &result->anchor_rx_diag_raw_len,
                            &result->anchor_full_cir_first_path_index);
    result->anchor_full_cir_total_len = DWM3000_FULL_CIR_BYTES;
    result->anchor_full_cir_len = read_cir_window(
        buffer,
        buffer_cap,
        DWM3000_FULL_CIR_BYTES,
        result->anchor_full_cir_first_path_index,
        &result->anchor_full_cir_start_index);
    result->anchor_full_cir_sampled = result->anchor_full_cir_len > 0u;
    result->anchor_full_cir_truncated =
        result->anchor_full_cir_len < DWM3000_FULL_CIR_BYTES;
    completed_ms = k_uptime_get_32();
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=cir-capture-done now=%u dur=%u seq=%u round=%u bytes=%u\n",
                            completed_ms,
                            completed_ms - started_ms,
                            result->seq,
                            result->round_index,
                            result->anchor_full_cir_len);
    }
    return result->anchor_full_cir_sampled ? 0 : -EIO;
}

static void capture_anchor_full_cir(const struct dwm3000_range_request *request,
                                    struct dwm3000_range_result *result)
{
    if (!request_captures_anchor_full_cir(request) || result == NULL) {
        return;
    }

    (void)dwm3000_driver_capture_last_rx_cir(request->anchor_full_cir,
                                             request->anchor_full_cir_cap,
                                             result);
}

static void result_set_request_metadata(struct dwm3000_range_result *result,
                                        const struct dwm3000_range_request *request)
{
    if (result == NULL || request == NULL) {
        return;
    }

    result->initiator_id = request->initiator_id;
    result->responder_id = request->responder_id;
    result->session_id = request->session_id;
    result->seq = request->seq;
    result->round_index = request->round_index;
    result->flags = request->flags;
}

static void result_set_poll_metadata(struct dwm3000_range_result *result,
                                     const struct uwb_range_header *poll,
                                     uint64_t local_anchor_id)
{
    if (result == NULL || poll == NULL) {
        return;
    }

    result->initiator_id = poll->initiator_id;
    result->responder_id = local_anchor_id;
    result->session_id = poll->session_id;
    result->seq = poll->seq;
    result->round_index = poll->round_index;
    result->flags = poll->flags;
}

static int validate_range_request(const struct dwm3000_range_request *request)
{
    if (request == NULL ||
        request->initiator_id == 0u ||
        request->responder_id == 0u ||
        request->initiator_id == request->responder_id ||
        request->network_id == 0u ||
        request->session_id == 0u ||
        request->session_nonce == 0u ||
        request->seq == 0u) {
        return -EINVAL;
    }
    if (request->responder_short_addr == 0u ||
        request->responder_short_addr != short_addr_from_id(request->responder_id)) {
        return -EINVAL;
    }
    if (!range_flags_valid(request->flags)) {
        return -EINVAL;
    }
    if ((request->anchor_full_cir == NULL) !=
        (request->anchor_full_cir_cap == 0u)) {
        return -EINVAL;
    }
    if (!request->skip_responder_report && request->anchor_full_cir != NULL) {
        return -EINVAL;
    }
    return 0;
}

static int send_range_frame(const uint8_t *frame, size_t frame_len, uint8_t tx_mode)
{
    int ret;

    driver_stats.tx_starts++;
    dwm3000_debug_event(false,
                        "UWB_TX_START",
                        "frame_len=%u tx_mode=0x%02x",
                        (unsigned int)frame_len,
                        tx_mode);
    ret = write_tx_frame(frame, frame_len);
    if (ret < 0) {
        driver_stats.tx_failures++;
        LOG_WRN("DWM3000 TX write failed: ret=%d frame_len=%u tx_mode=0x%02x",
                ret,
                (unsigned int)frame_len,
                tx_mode);
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=write-fail ret=%d frame_len=%u",
                            ret,
                            (unsigned int)frame_len);
        return ret;
    }

    if (dwt_starttx(tx_mode) != DWT_SUCCESS) {
        driver_stats.tx_failures++;
        LOG_WRN("DWM3000 TX start failed: frame_len=%u tx_mode=0x%02x",
                (unsigned int)frame_len,
                tx_mode);
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=start-fail frame_len=%u tx_mode=0x%02x",
                            (unsigned int)frame_len,
                            tx_mode);
        return -EIO;
    }
    return 0;
}

static int start_prepared_range_frame(size_t frame_len, uint8_t tx_mode)
{
    driver_stats.tx_starts++;
    dwm3000_debug_event(false,
                        "UWB_TX_START",
                        "frame_len=%u tx_mode=0x%02x prepared=1",
                        (unsigned int)frame_len,
                        tx_mode);
    if (dwt_starttx(tx_mode) != DWT_SUCCESS) {
        driver_stats.tx_failures++;
        LOG_WRN("DWM3000 prepared TX start failed: frame_len=%u tx_mode=0x%02x",
                (unsigned int)frame_len,
                tx_mode);
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=start-fail frame_len=%u tx_mode=0x%02x prepared=1",
                            (unsigned int)frame_len,
                            tx_mode);
        return -EIO;
    }
    return 0;
}

static int receive_frame(uint32_t timeout_ms, uint32_t *status,
                         uint8_t *buffer, size_t buffer_len, size_t *frame_len,
                         uint64_t *rx_timestamp, uint8_t *quality,
                         int8_t *rsl_dbm, uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                         bool *cir_sampled, int16_t *clock_offset_raw,
                         bool *clock_offset_sampled,
                         int32_t *carrier_integrator,
                         bool *carrier_integrator_sampled,
                         bool capture_rsl, bool log_events)
{
    int ret;

    driver_stats.rx_starts++;
    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_RX_START",
                            "timeout_ms=%u buffer_len=%u",
                            timeout_ms,
                            (unsigned int)buffer_len);
    }
    ret = wait_status_internal(SYS_STATUS_RXFCG_BIT_MASK |
                               SYS_STATUS_ALL_RX_TO |
                               SYS_STATUS_ALL_RX_ERR,
                               timeout_ms,
                               status,
                               log_events);
    if (ret < 0) {
        if (ret != -ECANCELED) {
            driver_stats.rx_timeouts++;
        }
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(ret != -ECANCELED,
                                ret == -ECANCELED ? "UWB_RX_ABORT" : "UWB_RX_TIMEOUT",
                                "ret=%d timeout_ms=%u sys_status=0x%08x",
                                ret,
                                timeout_ms,
                                status == NULL ? 0u : *status);
        }
        return ret;
    }

    if ((*status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        if ((*status & (SYS_STATUS_RXFCE_BIT_MASK |
                        SYS_STATUS_RXPHE_BIT_MASK |
                        SYS_STATUS_RXFSL_BIT_MASK)) != 0u) {
            driver_stats.rx_crc_failures++;
        } else {
            driver_stats.rx_failures++;
        }
        clear_status(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_RX_DONE",
                                "status=error sys_status=0x%08x",
                                *status);
        }
        return -EIO;
    }

    if (rx_timestamp != NULL) {
        *rx_timestamp = read_rx_timestamp_u64();
    }
    if (quality != NULL) {
        *quality = 100u;
    }
    if (capture_rsl) {
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled,
                            clock_offset_raw, clock_offset_sampled,
                            carrier_integrator, carrier_integrator_sampled);
    }

    *frame_len = read_rx_frame(buffer, buffer_len);
    clear_status(SYS_STATUS_RXFCG_BIT_MASK);
    if (*frame_len == 0u) {
        driver_stats.rx_failures++;
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_RX_DONE",
                                "status=empty-frame sys_status=0x%08x",
                                *status);
        }
        return -EMSGSIZE;
    }

    driver_stats.rx_dones++;
    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_RX_DONE",
                            "status=ok sys_status=0x%08x frame_len=%u",
                            *status,
                            (unsigned int)*frame_len);
    }
    return 0;
}

static int receive_response(const struct dwm3000_range_request *request,
                            struct uwb_response_frame *response,
                            uint64_t *resp_rx_ts,
                            uint8_t *quality,
                            enum range_status *status_out)
{
    uint8_t rx_buffer[UWB_RESP_LEN + FCS_LEN];
    uint32_t status;
    size_t frame_len;
    int ret;

    ret = wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                      request->timeout_ms == 0u ? RESP_RX_TIMEOUT_MS : request->timeout_ms,
                      &status);
    if (ret < 0) {
        dwt_forcetrxoff();
        *status_out = RANGE_RX_TIMEOUT;
        return ret;
    }

    if ((status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        *status_out = status_to_range_status(status);
        clear_status(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff();
        return -EIO;
    }

    *resp_rx_ts = read_rx_timestamp_u64();
    if (quality != NULL) {
        *quality = 100u;
    }
    frame_len = read_rx_frame(rx_buffer, sizeof(rx_buffer));
    clear_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);
    if (frame_len == 0u) {
        dwt_forcetrxoff();
        *status_out = RANGE_BAD_FRAME;
        return -EMSGSIZE;
    }

    ret = decode_response_frame(rx_buffer, frame_len, response);
    if (ret < 0) {
        *status_out = RANGE_BAD_FRAME;
        return ret;
    }

    if (!header_matches_request(&response->header, request, MSG_UWB_RESP)) {
        *status_out = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }

    *status_out = RANGE_OK;
    return 0;
}

static int receive_report(const struct dwm3000_range_request *request,
                          uint64_t responder_id,
                          struct dwm3000_range_result *result)
{
    struct uwb_report_frame report;
    uint8_t rx_buffer[UWB_REPORT_LEN + FCS_LEN];
    uint32_t status;
    size_t frame_len;
    int ret;

    ret = receive_frame(REPORT_RX_TIMEOUT_MS, &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len, NULL, NULL,
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, true);
    if (ret < 0) {
        result_set_request_metadata(result, request);
        result->responder_id = responder_id;
        result->distance_mm = 0;
        result->quality = 0u;
        result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : status_to_range_status(status);
        return ret;
    }

    ret = decode_report_frame(rx_buffer, frame_len, &report);
    if (ret < 0) {
        result->status = RANGE_BAD_FRAME;
        return ret;
    }

    if (!header_matches_request(&report.header, request, MSG_UWB_REPORT) ||
        report.header.responder_short_addr != short_addr_from_id(responder_id) ||
        report.header.responder_id != responder_id) {
        result->status = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }

    result->initiator_id = request->initiator_id;
    result->responder_id = responder_id;
    result->session_id = report.header.session_id;
    result->seq = report.header.seq;
    result->round_index = report.header.round_index;
    result->flags = report.header.flags;
    result->distance_mm = report.distance_mm;
    result->quality = report.quality;
    result->rsl_dbm = report.rsl_dbm;
    result->rsl_sampled = true;
    result->status = report.status;
    return report.status == RANGE_OK ? 0 : -EIO;
}

static bool clicker_diag_matches_poll(const struct uwb_clicker_diag_frame *diag,
                                      const struct uwb_range_header *poll,
                                      uint64_t local_anchor_id)
{
    if (diag == NULL || poll == NULL) {
        return false;
    }

    return diag->header.seq == poll->seq &&
           diag->header.round_index == poll->round_index &&
           diag->header.network_id == poll->network_id &&
           diag->header.session_id == poll->session_id &&
           diag->header.session_nonce == poll->session_nonce &&
           diag->header.initiator_short_addr == poll->initiator_short_addr &&
           diag->header.responder_short_addr == short_addr_from_id(local_anchor_id) &&
           diag->header.flags == poll->flags &&
           diag->header.initiator_id == poll->initiator_id &&
           diag->header.responder_id == local_anchor_id;
}

static void store_clicker_diag_result(struct dwm3000_range_result *result,
                                      const struct uwb_clicker_diag_frame *diag)
{
    uint8_t raw_len;
    uint8_t copy_len;

    if (result == NULL || diag == NULL) {
        return;
    }

    raw_len = diag->diag_len;
    copy_len = raw_len;
    if (copy_len > UWB_CLICKER_DIAG_MAX_BYTES - 15u) {
        copy_len = UWB_CLICKER_DIAG_MAX_BYTES - 15u;
    }

    proto_put_u32_le(&result->clicker_diag[0], diag->final_tx_ts_32);
    proto_put_u32_le(&result->clicker_diag[4], diag->status_flags);
    proto_put_u32_le(&result->clicker_diag[8], diag->status_detect_latency_us);
    result->clicker_diag[12] = diag->resp_quality;
    result->clicker_diag[13] = (uint8_t)diag->resp_rsl_dbm;
    result->clicker_diag[14] = raw_len;
    if (copy_len > 0u) {
        memcpy(&result->clicker_diag[15], diag->diag_bytes, copy_len);
    }
    result->clicker_diag_len = 15u + copy_len;
    result->clicker_diag_status_flags = diag->status_flags;
    result->clicker_diag_status_detect_latency_us = diag->status_detect_latency_us;
    result->clicker_diag_received = true;
    result->clicker_diag_truncated = copy_len != raw_len;
}

static int send_clicker_diag(const struct dwm3000_range_request *request,
                             const struct uwb_response_frame *response,
                             const struct uwb_final_frame *final,
                             uint64_t resp_rx_ts,
                             uint8_t resp_quality,
                             int8_t resp_rsl_dbm,
                             bool resp_rsl_sampled,
                             struct dwm3000_range_result *result)
{
    struct uwb_clicker_diag_frame diag = {0};
    uint8_t tx_buffer[UWB_CLICKER_DIAG_MAX_LEN];
    size_t tx_len = 0u;
    int ret;

    if (request == NULL || response == NULL || final == NULL) {
        return -EINVAL;
    }

    diag.header.type = MSG_UWB_CLICKER_DIAG;
    diag.header.seq = request->seq;
    diag.header.round_index = request->round_index;
    diag.header.network_id = request->network_id;
    diag.header.session_id = request->session_id;
    diag.header.session_nonce = request->session_nonce;
    diag.header.initiator_short_addr = short_addr_from_id(request->initiator_id);
    diag.header.responder_short_addr = response->header.responder_short_addr;
    diag.header.flags = request->flags;
    diag.header.initiator_id = request->initiator_id;
    diag.header.responder_id = response->header.responder_id;
    diag.final_tx_ts_32 = final->final_tx_ts_32;
    diag.status_flags = UWB_CLICKER_DIAG_STATUS_RESP_RX_PRESENT |
                        UWB_CLICKER_DIAG_STATUS_COMPACT_BYTES_PRESENT;
    if (resp_rsl_sampled) {
        diag.status_flags |= UWB_CLICKER_DIAG_STATUS_RESP_RSL_PRESENT;
    }
    diag.status_detect_latency_us = 0u;
    diag.resp_quality = resp_quality;
    diag.resp_rsl_dbm = resp_rsl_dbm;
    diag.diag_len = CLICKER_DIAG_COMPACT_BYTES_LEN;
    proto_put_u32_le(&diag.diag_bytes[0], (uint32_t)resp_rx_ts);
    proto_put_u32_le(&diag.diag_bytes[4], response->poll_rx_ts_32);
    proto_put_u32_le(&diag.diag_bytes[8], response->resp_tx_ts_32);

    ret = uwb_encode_clicker_diag(&diag, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }
    ret = wait_tx_complete(CLICKER_DIAG_TX_TIMEOUT_MS);
    if (ret == 0) {
        store_clicker_diag_result(result, &diag);
    }
    return ret;
}

static int receive_clicker_diag(const struct uwb_range_header *poll,
                                uint64_t local_anchor_id,
                                struct dwm3000_range_result *result)
{
    struct uwb_clicker_diag_frame diag;
    uint8_t rx_buffer[UWB_CLICKER_DIAG_MAX_LEN + FCS_LEN];
    uint32_t status = 0u;
    size_t frame_len = 0u;
    int ret;

    if (poll == NULL) {
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(0u);
    dwt_setrxtimeout(0u);
    dwt_setpreambledetecttimeout(CLICKER_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    dwm3000_debug_event(false,
                        "CLICKER_DIAG_RX",
                        "short=0x%04x seq=%u round=%u timeout_ms=%u preamble_timeout_pac=%u",
                        poll->initiator_short_addr,
                        poll->seq,
                        poll->round_index,
                        CLICKER_DIAG_RX_TIMEOUT_MS,
                        CLICKER_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(CLICKER_DIAG_RX_TIMEOUT_MS,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &frame_len,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        false,
                        true);
    if (ret < 0) {
        if (result != NULL) {
            result->clicker_diag_dropped = true;
        }
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    memset(&diag, 0, sizeof(diag));
    ret = decode_clicker_diag_frame(rx_buffer, frame_len, &diag);
    if (ret < 0 || !clicker_diag_matches_poll(&diag, poll, local_anchor_id)) {
        if (result != NULL) {
            result->clicker_diag_dropped = true;
        }
        return -EBADMSG;
    }

    store_clicker_diag_result(result, &diag);
    return 0;
}

static bool anchor_diag_matches_request(const struct uwb_anchor_diag_frame *diag,
                                        const struct dwm3000_range_request *request)
{
    if (diag == NULL || request == NULL) {
        return false;
    }

    return diag->header.type == MSG_UWB_ANCHOR_DIAG &&
           diag->header.seq == request->seq &&
           diag->header.round_index == request->round_index &&
           diag->header.network_id == request->network_id &&
           diag->header.session_id == request->session_id &&
           diag->header.session_nonce == request->session_nonce &&
           diag->header.initiator_short_addr == short_addr_from_id(request->initiator_id) &&
           diag->header.responder_short_addr == request->responder_short_addr &&
           diag->header.flags == request->flags &&
           diag->header.initiator_id == request->initiator_id &&
           diag->header.responder_id == request->responder_id;
}

static void store_anchor_diag_result(struct dwm3000_range_result *result,
                                     const struct uwb_anchor_diag_frame *diag)
{
    if (result == NULL || diag == NULL) {
        return;
    }

    result->distance_mm = diag->distance_mm;
    result->quality = diag->quality;
    result->status = diag->status;
    result->anchor_diag_status_flags = diag->status_flags;
    result->rsl_dbm = diag->rsl_dbm;
    result->rsl_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT) != 0u;
    result->clock_offset_raw = diag->clock_offset_raw;
    result->clock_offset_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CLOCK_OFFSET_PRESENT) != 0u;
    result->carrier_integrator = diag->carrier_integrator;
    result->carrier_integrator_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CARRIER_INTEGRATOR_PRESENT) != 0u;
    result->poll_rx_ts_32 = diag->poll_rx_ts_32;
    result->resp_tx_ts_32 = diag->resp_tx_ts_32;
    result->final_rx_ts_32 = diag->final_rx_ts_32;
    result->poll_tx_ts_32 = diag->poll_tx_ts_32;
    result->resp_rx_ts_32 = diag->resp_rx_ts_32;
    result->final_tx_ts_32 = diag->final_tx_ts_32;
    result->cir_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CIR_SAMPLE_PRESENT) != 0u &&
        diag->diag_len >= UWB_CIR_SAMPLE_LEN;
    if (result->cir_sampled) {
        memcpy(result->cir_sample, diag->diag_bytes, UWB_CIR_SAMPLE_LEN);
    }
}

static int receive_anchor_diag(const struct dwm3000_range_request *request,
                               struct dwm3000_range_result *result)
{
    struct uwb_anchor_diag_frame diag;
    uint8_t rx_buffer[UWB_ANCHOR_DIAG_MAX_LEN + FCS_LEN];
    uint32_t status = 0u;
    size_t frame_len = 0u;
    int ret;

    if (request == NULL) {
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(0u);
    dwt_setrxtimeout(0u);
    dwt_setpreambledetecttimeout(ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    dwm3000_debug_event(false,
                        "ANCHOR_DIAG_RX",
                        "anchor=0x%016llx seq=%u round=%u timeout_ms=%u preamble_timeout_pac=%u",
                        (unsigned long long)request->responder_id,
                        request->seq,
                        request->round_index,
                        ANCHOR_DIAG_RX_TIMEOUT_MS,
                        ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(ANCHOR_DIAG_RX_TIMEOUT_MS,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &frame_len,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        false,
                        true);
    if (ret < 0) {
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    memset(&diag, 0, sizeof(diag));
    ret = decode_anchor_diag_frame(rx_buffer, frame_len, &diag);
    if (ret < 0 || !anchor_diag_matches_request(&diag, request)) {
        return -EBADMSG;
    }

    store_anchor_diag_result(result, &diag);
    return 0;
}

static bool anchor_diag_fragment_matches_request(
    const struct uwb_anchor_diag_fragment_frame *fragment,
    const struct dwm3000_range_request *request)
{
    if (fragment == NULL || request == NULL) {
        return false;
    }

    return fragment->header.type == MSG_UWB_ANCHOR_DIAG_FRAGMENT &&
           fragment->header.seq == request->seq &&
           fragment->header.round_index == request->round_index &&
           fragment->header.network_id == request->network_id &&
           fragment->header.session_id == request->session_id &&
           fragment->header.session_nonce == request->session_nonce &&
           fragment->header.initiator_short_addr == short_addr_from_id(request->initiator_id) &&
           fragment->header.responder_short_addr == request->responder_short_addr &&
           fragment->header.flags == request->flags &&
           fragment->header.initiator_id == request->initiator_id &&
           fragment->header.responder_id == request->responder_id;
}

static int receive_anchor_diag_fragments(const struct dwm3000_range_request *request,
                                         struct dwm3000_range_result *result)
{
    uint8_t rx_buffer[UWB_ANCHOR_DIAG_FRAGMENT_MAX_LEN + FCS_LEN];
    int64_t deadline_ms;
    int last_error = -ETIMEDOUT;

    if (request == NULL || result == NULL || request->anchor_full_cir == NULL) {
        return -EINVAL;
    }

    result->anchor_full_cir_len = 0u;
    result->anchor_full_cir_total_len = 0u;
    result->anchor_full_cir_first_path_index = 0u;
    result->anchor_full_cir_start_index = 0u;
    result->anchor_rx_diag_raw_len = 0u;
    deadline_ms = k_uptime_get() + ANCHOR_DIAG_FRAGMENT_RX_TIMEOUT_MS;

    while (k_uptime_get() < deadline_ms) {
        bool expect_rx_diag =
            (result->anchor_diag_status_flags & UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT) != 0u;
        bool expect_full_cir =
            (result->anchor_diag_status_flags & UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT) != 0u;
        struct uwb_anchor_diag_fragment_frame fragment = {0};
        uint32_t status = 0u;
        size_t frame_len = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        int ret;

        dwt_setrxaftertxdelay(0u);
        dwt_setrxtimeout(0u);
        dwt_setpreambledetecttimeout(ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
        clear_all_events();
        if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
            return -EIO;
        }

        ret = receive_frame((uint32_t)MAX(1, remaining_ms),
                            &status,
                            rx_buffer,
                            sizeof(rx_buffer),
                            &frame_len,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            false,
                            true);
        if (ret < 0) {
            last_error = ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
            if (ret == -ETIMEDOUT) {
                break;
            }
            continue;
        }

        ret = decode_anchor_diag_fragment_frame(rx_buffer, frame_len, &fragment);
        if (ret < 0) {
            last_error = -EBADMSG;
            LOG_DBG("ignored anchor diagnostic fragment decode miss: ret=%d len=%zu",
                    ret,
                    frame_len);
            continue;
        }
        if (!anchor_diag_fragment_matches_request(&fragment, request)) {
            last_error = -EBADMSG;
            LOG_DBG("ignored anchor diagnostic fragment mismatch: seq=%u round=%u session=%u type=%u",
                    fragment.header.seq,
                    fragment.header.round_index,
                    fragment.header.session_id,
                    fragment.header.type);
            continue;
        }

        if (fragment.block_type == UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_RX_DIAG) {
            if (fragment.total_len > sizeof(result->anchor_rx_diag_raw) ||
                fragment.offset != result->anchor_rx_diag_raw_len) {
                return -EMSGSIZE;
            }
            memcpy(&result->anchor_rx_diag_raw[fragment.offset],
                   fragment.chunk,
                   fragment.chunk_len);
            result->anchor_rx_diag_raw_len += fragment.chunk_len;
            if ((fragment.flags & UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST) != 0u) {
                result->anchor_rx_diag_sampled =
                    result->anchor_rx_diag_raw_len == fragment.total_len;
            }
        } else if (fragment.block_type == UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_CIR) {
            if (fragment.total_len > request->anchor_full_cir_cap ||
                fragment.offset != result->anchor_full_cir_len) {
                return -EMSGSIZE;
            }
            memcpy(&request->anchor_full_cir[fragment.offset],
                   fragment.chunk,
                   fragment.chunk_len);
            result->anchor_full_cir_len += fragment.chunk_len;
            result->anchor_full_cir_total_len = fragment.total_len;
            result->anchor_full_cir_first_path_index = fragment.first_path_index;
            result->anchor_full_cir_start_index =
                cir_window_start_sample(fragment.first_path_index,
                                        fragment.total_len / DWM3000_FULL_CIR_SAMPLE_BYTES);
            if ((fragment.flags & UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST) != 0u) {
                result->anchor_full_cir_sampled =
                    result->anchor_full_cir_len == fragment.total_len;
                result->anchor_full_cir_truncated =
                    result->anchor_full_cir_len < DWM3000_FULL_CIR_BYTES;
            }
        }

        if ((!expect_rx_diag || result->anchor_rx_diag_sampled) &&
            (!expect_full_cir || result->anchor_full_cir_sampled)) {
            return 0;
        }
    }

    return last_error;
}

static int send_anchor_diag_fragment_block(const struct uwb_range_header *poll,
                                           uint8_t block_type,
                                           const uint8_t *data,
                                           uint16_t data_len,
                                           uint16_t first_path_index)
{
    uint8_t tx_buffer[UWB_ANCHOR_DIAG_FRAGMENT_MAX_LEN];
    uint8_t fragment_count;
    uint16_t offset = 0u;

    if (poll == NULL || data == NULL || data_len == 0u) {
        return -EINVAL;
    }

    fragment_count = (uint8_t)((data_len + UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES - 1u) /
                               UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES);
    for (uint8_t fragment_index = 0u; fragment_index < fragment_count; fragment_index++) {
        struct uwb_anchor_diag_fragment_frame fragment = {0};
        size_t tx_len = 0u;
        uint16_t chunk_len = MIN((uint16_t)UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES,
                                 (uint16_t)(data_len - offset));
        int ret;

        k_busy_wait(ANCHOR_DIAG_FRAGMENT_TX_GAP_US);
        fragment.header = *poll;
        fragment.header.type = MSG_UWB_ANCHOR_DIAG_FRAGMENT;
        fragment.block_type = block_type;
        fragment.offset = offset;
        fragment.total_len = data_len;
        fragment.first_path_index = first_path_index;
        fragment.fragment_index = fragment_index;
        fragment.fragment_count = fragment_count;
        fragment.flags = fragment_index == fragment_count - 1u ?
                         UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST : 0u;
        fragment.chunk_len = (uint8_t)chunk_len;
        memcpy(fragment.chunk, &data[offset], chunk_len);

        ret = uwb_encode_anchor_diag_fragment(&fragment,
                                              tx_buffer,
                                              sizeof(tx_buffer),
                                              &tx_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        clear_status(SYS_STATUS_TXFRS_BIT_MASK);
        ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
        if (ret < 0) {
            return ret;
        }
        ret = wait_tx_complete(ANCHOR_DIAG_FRAGMENT_TX_TIMEOUT_MS);
        if (ret < 0) {
            return ret;
        }
        offset += chunk_len;
    }

    return 0;
}

static int send_anchor_diag_fragments(const struct uwb_range_header *poll,
                                      const struct dwm3000_range_request *expected,
                                      const struct dwm3000_range_result *result)
{
    int ret;

    if (poll == NULL || expected == NULL || result == NULL) {
        return -EINVAL;
    }

    if (result->anchor_rx_diag_sampled && result->anchor_rx_diag_raw_len > 0u) {
        ret = send_anchor_diag_fragment_block(poll,
                                              UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_RX_DIAG,
                                              result->anchor_rx_diag_raw,
                                              result->anchor_rx_diag_raw_len,
                                              result->anchor_full_cir_first_path_index);
        if (ret < 0) {
            return ret;
        }
    }
    if (result->anchor_full_cir_sampled &&
        expected->anchor_full_cir != NULL &&
        result->anchor_full_cir_len > 0u) {
        ret = send_anchor_diag_fragment_block(poll,
                                              UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_CIR,
                                              expected->anchor_full_cir,
                                              result->anchor_full_cir_len,
                                              result->anchor_full_cir_first_path_index);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int send_anchor_diag(const struct uwb_range_header *poll,
                            const struct dwm3000_range_result *result)
{
    struct uwb_anchor_diag_frame diag = {0};
    uint8_t tx_buffer[UWB_ANCHOR_DIAG_MAX_LEN];
    size_t tx_len = 0u;
    int ret;

    if (poll == NULL || result == NULL) {
        return -EINVAL;
    }

    diag.header = *poll;
    diag.header.type = MSG_UWB_ANCHOR_DIAG;
    diag.distance_mm = result->distance_mm;
    diag.quality = result->quality;
    diag.status = result->status;
    diag.rsl_dbm = result->rsl_dbm;
    diag.poll_rx_ts_32 = result->poll_rx_ts_32;
    diag.resp_tx_ts_32 = result->resp_tx_ts_32;
    diag.final_rx_ts_32 = result->final_rx_ts_32;
    diag.poll_tx_ts_32 = result->poll_tx_ts_32;
    diag.resp_rx_ts_32 = result->resp_rx_ts_32;
    diag.final_tx_ts_32 = result->final_tx_ts_32;
    diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RAW_TIMESTAMPS_PRESENT;
    if (result->rsl_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT;
    }
    if (result->cir_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CIR_SAMPLE_PRESENT;
        diag.diag_len = UWB_CIR_SAMPLE_LEN;
        memcpy(diag.diag_bytes, result->cir_sample, UWB_CIR_SAMPLE_LEN);
    }
    if (result->clock_offset_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CLOCK_OFFSET_PRESENT;
        diag.clock_offset_raw = result->clock_offset_raw;
    }
    if (result->carrier_integrator_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CARRIER_INTEGRATOR_PRESENT;
        diag.carrier_integrator = result->carrier_integrator;
    }
    if (result->anchor_rx_diag_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT;
    }
    if (result->anchor_full_cir_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT;
    }
    if (diag.status_flags == 0u) {
        diag.status_flags = UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT;
    }

    ret = uwb_encode_anchor_diag(&diag, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }
    return wait_tx_complete(ANCHOR_DIAG_TX_TIMEOUT_MS);
}

static int compute_distance_mm(const struct uwb_final_frame *final,
                               uint64_t poll_rx_ts,
                               uint64_t resp_tx_ts,
                               uint64_t final_rx_ts,
                               int32_t *distance_mm)
{
    uint32_t poll_tx_ts_32 = final->poll_tx_ts_32;
    uint32_t resp_rx_ts_32 = final->resp_rx_ts_32;
    uint32_t final_tx_ts_32 = final->final_tx_ts_32;
    uint32_t poll_rx_ts_32 = (uint32_t)poll_rx_ts;
    uint32_t resp_tx_ts_32 = (uint32_t)resp_tx_ts;
    uint32_t final_rx_ts_32 = (uint32_t)final_rx_ts;
    double ra = (double)(uint32_t)(resp_rx_ts_32 - poll_tx_ts_32);
    double rb = (double)(uint32_t)(final_rx_ts_32 - resp_tx_ts_32);
    double da = (double)(uint32_t)(final_tx_ts_32 - resp_rx_ts_32);
    double db = (double)(uint32_t)(resp_tx_ts_32 - poll_rx_ts_32);
    double denominator = ra + rb + da + db;
    double tof_dtu;
    double distance_m;

    if (distance_mm == NULL || denominator == 0.0) {
        return -EINVAL;
    }

    tof_dtu = ((ra * rb) - (da * db)) / denominator;
    distance_m = tof_dtu * DWM3000_TIME_UNITS * DWM3000_SPEED_OF_LIGHT_MPS;
    *distance_mm = (int32_t)(distance_m * 1000.0);
    return 0;
}
