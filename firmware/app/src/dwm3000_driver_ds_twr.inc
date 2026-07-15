#line 3887 __BASE_FILE__
int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result)
{
    struct uwb_response_frame response;
    struct uwb_final_frame final = {0};
    struct uwb_range_header poll_header;
    uint8_t tx_buffer[UWB_CLICKER_DIAG_MAX_LEN];
    size_t tx_len;
    uint64_t poll_tx_ts;
    uint64_t resp_rx_ts;
    uint64_t final_tx_ts;
    uint32_t final_tx_time;
    uint16_t reply_delay_uus;
    uint8_t quality = 0u;
    int8_t rsl_dbm = 0;
    enum range_status range_status = RANGE_INTERNAL_ERROR;
    int ret;

    if (result == NULL) {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    ret = validate_range_request(request);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=validate ret=%d\n", ret);
        }
        return ret;
    }

    ret = ensure_phy_mode(DWM3000_PHY_RANGE);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=phy ret=%d now=%u\n",
                                ret,
                                k_uptime_get_32());
        }
        return ret;
    }
    reply_delay_uus = request_reply_delay_uus(request);
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_INIT stage=arm evt=%u seq=%u round=%u now=%u timeout=%u reply_uus=%u\n",
                            request->session_id,
                            request->seq,
                            request->round_index,
                            k_uptime_get_32(),
                            request->timeout_ms == 0u ?
                                RESP_RX_TIMEOUT_MS : request->timeout_ms,
                            reply_delay_uus);
    }

    dwm3000_debug_event(false,
                        "DS_TWR_TIMING",
                        "role=initiator reply_delay_uus=%u resp_rx_after_uus=%u resp_rx_timeout_uus=%u wait_timeout_ms=%u",
                        reply_delay_uus,
                        ds_twr_rx_after_tx_delay_uus(reply_delay_uus),
                        ds_twr_rx_timeout_uus(reply_delay_uus),
                        request->timeout_ms == 0u ?
                        RESP_RX_TIMEOUT_MS : request->timeout_ms);
    dwt_setrxaftertxdelay(ds_twr_rx_after_tx_delay_uus(reply_delay_uus));
    dwt_setrxtimeout(ds_twr_rx_timeout_uus(reply_delay_uus));
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    poll_header.type = MSG_UWB_POLL;
    poll_header.seq = request->seq;
    poll_header.round_index = request->round_index;
    poll_header.network_id = request->network_id;
    poll_header.session_id = request->session_id;
    poll_header.session_nonce = request->session_nonce;
    poll_header.initiator_short_addr = short_addr_from_id(request->initiator_id);
    poll_header.responder_short_addr = request->responder_short_addr;
    poll_header.flags = request->flags;
    poll_header.initiator_id = request->initiator_id;
    poll_header.responder_id = request->responder_id;

    ret = uwb_encode_poll(&poll_header, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }

    clear_all_events();
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=poll-tx ret=%d sys=0x%08x now=%u seq=%u round=%u\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                request->seq,
                                request->round_index);
        }
        return ret;
    }
    result_set_request_metadata(result, request);
    result->exchange_start_ms = k_uptime_get();
    result->exchange_started = true;

    ret = capture_completed_tx_timestamp(
        ds_twr_rx_wait_timeout_ms(reply_delay_uus),
        &poll_tx_ts);
    if (ret < 0) {
        dwt_forcetrxoff();
        result->status = RANGE_INTERNAL_ERROR;
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=poll-complete ret=%d sys=0x%08x now=%u seq=%u round=%u\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                request->seq,
                                request->round_index);
        }
        return ret;
    }

    /*
     * Stage every invariant FINAL byte while the responder is still waiting
     * to transmit. After RESP, only the three timestamp fields need an SPI
     * patch before the delayed-TX command.
     */
    final.header.type = MSG_UWB_FINAL;
    final.header.seq = request->seq;
    final.header.round_index = request->round_index;
    final.header.network_id = request->network_id;
    final.header.session_id = request->session_id;
    final.header.session_nonce = request->session_nonce;
    final.header.initiator_short_addr = poll_header.initiator_short_addr;
    final.header.responder_short_addr = request->responder_short_addr;
    final.header.flags = request->flags;
    final.header.initiator_id = request->initiator_id;
    final.header.responder_id = request->responder_id;
    final.poll_tx_ts_32 = (uint32_t)poll_tx_ts;
    final.diagnostic_flags = result->clicker_clock_offset_sampled ?
        UWB_FINAL_DIAG_CLICKER_CLOCK_OFFSET_PRESENT : 0u;
    final.clicker_clock_offset_raw = result->clicker_clock_offset_sampled ?
        result->clicker_clock_offset_raw : 0;
    ret = uwb_encode_final(&final, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        dwt_forcetrxoff();
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }
    ret = write_tx_frame(tx_buffer, tx_len);
    if (ret == 0) {
        ret = take_port_error("final-prestage");
    }
    if (ret < 0) {
        dwt_forcetrxoff();
        result->status = RANGE_INTERNAL_ERROR;
        return ret;
    }

    ret = receive_response(request, &response, &resp_rx_ts, &quality,
                           &range_status);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=resp-rx ret=%d status=%u sys=0x%08x now=%u seq=%u round=%u\n",
                                ret,
                                range_status,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                request->seq,
                                request->round_index);
        }
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB initiator RESP RX failed: ret=%d status=%u seq=%u round=%u timeout_ms=%u reply_delay_uus=%u",
                ret,
                range_status,
                request->seq,
                request->round_index,
                request->timeout_ms == 0u ? RESP_RX_TIMEOUT_MS : request->timeout_ms,
                reply_delay_uus);
#endif
        result_set_request_metadata(result, request);
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = false;
        result->status = range_status;
        return ret;
    }

    final_tx_time = delayed_tx_time_from_rx_reference(resp_rx_ts,
                                                      reply_delay_uus);
    final_tx_ts = delayed_tx_timestamp_from_programmed_time(final_tx_time);

    final.resp_rx_ts_32 = (uint32_t)resp_rx_ts;
    final.final_tx_ts_32 = (uint32_t)final_tx_ts;
    result->poll_tx_ts_32 = final.poll_tx_ts_32;
    result->poll_rx_ts_32 = response.poll_rx_ts_32;
    result->resp_tx_ts_32 = response.resp_tx_ts_32;
    result->resp_rx_ts_32 = final.resp_rx_ts_32;
    result->final_tx_ts_32 = final.final_tx_ts_32;

    ret = validate_driver_reply_timing(
        dwt_delta_to_uus(response.poll_rx_ts_32, response.resp_tx_ts_32),
        dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
        reply_delay_uus);
    if (ret != PROTO_OK) {
        result_set_request_metadata(result, request);
        result->responder_id = request->responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = false;
        result->status = RANGE_TIMING_INVALID;
        return -ETIME;
    }

    ret = uwb_encode_final(&final, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }

    ret = patch_tx_frame(&tx_buffer[UWB_HEADER_LEN],
                         3u * sizeof(uint32_t),
                         UWB_HEADER_LEN);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        return ret;
    }
    if (request->skip_responder_report) {
        dwt_setdelayedtrxtime(final_tx_time);
        ret = start_prepared_range_frame(tx_len, DWT_START_TX_DELAYED);
    } else {
        dwt_setrxaftertxdelay(0u);
        dwt_setrxtimeout(REPORT_RX_TIMEOUT_UUS);

        dwt_setdelayedtrxtime(final_tx_time);
        ret = start_prepared_range_frame(
            tx_len,
            DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    }
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=final-tx ret=%d sys=0x%08x now=%u seq=%u round=%u tx_time=0x%08x\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                request->seq,
                                request->round_index,
                                final_tx_time);
        }
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
            LOG_WRN("UWB initiator FINAL TX start failed: ret=%d anchor=0x%016llx seq=%u round=%u reply_delay_uus=%u",
                    ret,
                    (unsigned long long)request->responder_id,
                    request->seq,
                    request->round_index,
                    reply_delay_uus);
#endif
            result_set_request_metadata(result, request);
            result->responder_id = request->responder_id;
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->rsl_sampled = false;
            result->status = RANGE_DELAYED_TX_MISSED;
            return -ETIME;
    }

    /*
     * Optional initiator diagnostics describe the received RESP, but none of
     * their SPI work may consume delayed-FINAL arm headroom.  The DW3000 keeps
     * the completed RX diagnostics available while the already-armed delayed
     * TX is pending, so collect them only after dwt_starttx() has accepted the
     * schedule.  Current click and survey initiators leave capture_rsl false;
     * this preserves the shared driver's explicit opt-in behavior without
     * putting the production fast path back inside the deadline.
     */
    if (request->capture_rsl) {
        read_rx_diagnostics(&rsl_dbm,
                            result->cir_sample,
                            &result->cir_sampled,
                            &result->clicker_clock_offset_raw,
                            &result->clicker_clock_offset_sampled,
                            &result->carrier_integrator,
                            &result->carrier_integrator_sampled);
        result->clicker_rx_diag_sampled = capture_rx_diag_raw(
            result->clicker_rx_diag_raw,
            &result->clicker_rx_diag_raw_len,
            NULL);
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = true;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_INIT stage=final-armed now=%u seq=%u round=%u tx_time=0x%08x final_tx=0x%08x report=%u\n",
                            k_uptime_get_32(),
                            request->seq,
                            request->round_index,
                            final_tx_time,
                            (unsigned int)final_tx_ts,
                            request->skip_responder_report ? 0u : 1u);
        status_debug_printf("DBG_DS_INIT stage=final-raw poll_tx=0x%08x resp_rx=0x%08x q=%u\n",
                            (unsigned int)poll_tx_ts,
                            (unsigned int)resp_rx_ts,
                            quality);
    }

#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
    LOG_INF("UWB initiator RESP RX ok: anchor=0x%016llx seq=%u round=%u quality=%u rsl=%d dBm poll_tx=0x%08x resp_rx=0x%08x",
            (unsigned long long)response.header.responder_id,
            request->seq,
            request->round_index,
            quality,
            rsl_dbm,
            (uint32_t)poll_tx_ts,
            (uint32_t)resp_rx_ts);
    LOG_INF("UWB initiator FINAL TX scheduled: anchor=0x%016llx seq=%u round=%u resp_to_final_uus=%u tx_time=0x%08x final_tx=0x%08x skip_report=%u",
            (unsigned long long)response.header.responder_id,
            request->seq,
            request->round_index,
            dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
            final_tx_time,
            final.final_tx_ts_32,
            request->skip_responder_report ? 1u : 0u);
    if (!request->skip_responder_report) {
        dwm3000_debug_event(false,
                            "DS_TWR_TIMING",
                            "role=initiator_report report_rx_after_uus=0 report_rx_timeout_uus=%u wait_timeout_ms=%u",
                            REPORT_RX_TIMEOUT_UUS,
                            REPORT_RX_TIMEOUT_MS);
    }
#endif

    if (request->skip_responder_report) {
        ret = wait_tx_complete(ds_twr_rx_wait_timeout_ms(reply_delay_uus));
        if (ret < 0) {
            if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
                status_debug_printf("DBG_DS_INIT stage=final-complete ret=%d sys=0x%08x now=%u seq=%u round=%u\n",
                                    ret,
                                    (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                    k_uptime_get_32(),
                                    request->seq,
                                    request->round_index);
            }
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
            LOG_WRN("UWB initiator FINAL TX complete failed: ret=%d anchor=0x%016llx seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                    ret,
                    (unsigned long long)request->responder_id,
                    request->seq,
                    request->round_index,
                    ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                    reply_delay_uus);
#endif
            result_set_request_metadata(result, request);
            result->responder_id = request->responder_id;
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_INTERNAL_ERROR;
            return ret;
        }
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_INF("UWB initiator FINAL TX complete: anchor=0x%016llx seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                (unsigned long long)response.header.responder_id,
                request->seq,
                request->round_index,
                ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                reply_delay_uus);
#endif

        result_set_request_metadata(result, request);
        result->responder_id = response.header.responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = false;
        result->status = RANGE_OK;
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_INIT stage=done ret=0 status=%u now=%u seq=%u round=%u\n",
                                result->status,
                                k_uptime_get_32(),
                                request->seq,
                                request->round_index);
        }

        if (request_sends_clicker_diag(request)) {
            int diag_ret;

            k_busy_wait(CLICKER_DIAG_TX_AFTER_FINAL_DELAY_US);
            dwm3000_debug_event(false,
                                "CLICKER_DIAG_TX",
                                "anchor=0x%016llx seq=%u round=%u delay_after_final_us=%u",
                                (unsigned long long)response.header.responder_id,
                                request->seq,
                                request->round_index,
                                CLICKER_DIAG_TX_AFTER_FINAL_DELAY_US);
            diag_ret = send_clicker_diag(request,
                                         &response,
                                         &final,
                                         resp_rx_ts,
                                         quality,
                                         rsl_dbm,
                                         false,
                                         result);
            if (diag_ret < 0) {
                result->status = RANGE_INTERNAL_ERROR;
                LOG_WRN("UWB clicker diagnostic TX failed: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        diag_ret);
                return diag_ret;
            }
        }

        if (request_expects_anchor_diag(request)) {
            int anchor_diag_ret = receive_anchor_diag(request, result);

            if (anchor_diag_ret < 0) {
                LOG_WRN("UWB anchor diagnostic not received: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        anchor_diag_ret);
            } else {
                LOG_INF("UWB anchor diagnostic received: anchor=0x%016llx seq=%u bytes=%u",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
                if (request_expects_anchor_diag_fragments(request) &&
                    (result->anchor_diag_status_flags &
                     (UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT |
                      UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT)) != 0u) {
                    int fragment_ret = receive_anchor_diag_fragments(request, result);

                    if (fragment_ret < 0) {
                        LOG_WRN("UWB anchor diagnostic fragments not received: anchor=0x%016llx seq=%u ret=%d",
                                (unsigned long long)response.header.responder_id,
                                request->seq,
                                fragment_ret);
                    }
                }
            }
        }

        return 0;
    }

    ret = receive_report(request, response.header.responder_id, result);
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_INIT stage=report-rx ret=%d status=%u sys=0x%08x now=%u seq=%u round=%u\n",
                            ret,
                            result->status,
                            (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                            k_uptime_get_32(),
                            request->seq,
                            request->round_index);
    }
    if (ret == 0 && result->status == RANGE_OK && request_sends_clicker_diag(request)) {
        int diag_ret = send_clicker_diag(request,
                                         &response,
                                         &final,
                                         resp_rx_ts,
                                         quality,
                                         rsl_dbm,
                                         false,
                                         result);

        if (diag_ret < 0) {
            LOG_WRN("UWB clicker diagnostic TX failed: anchor=0x%016llx seq=%u ret=%d",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    diag_ret);
        }
    }
    if (ret == 0 && result->status == RANGE_OK && request_expects_anchor_diag(request)) {
        int anchor_diag_ret = receive_anchor_diag(request, result);

        if (anchor_diag_ret < 0) {
            LOG_WRN("UWB anchor diagnostic not received: anchor=0x%016llx seq=%u ret=%d",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    anchor_diag_ret);
        } else {
            LOG_INF("UWB anchor diagnostic received: anchor=0x%016llx seq=%u bytes=%u",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
            if (request_expects_anchor_diag_fragments(request) &&
                (result->anchor_diag_status_flags &
                 (UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT |
                  UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT)) != 0u) {
                int fragment_ret = receive_anchor_diag_fragments(request, result);

                if (fragment_ret < 0) {
                    LOG_WRN("UWB anchor diagnostic fragments not received: anchor=0x%016llx seq=%u ret=%d",
                            (unsigned long long)response.header.responder_id,
                            request->seq,
                            fragment_ret);
                }
            }
        }
    }
    if (ret < 0 && result->status == RANGE_OK) {
        result->status = RANGE_INTERNAL_ERROR;
    }
    if (result->quality == 0u) {
        result->quality = quality;
    }
    if (result->rsl_dbm == 0) {
        result->rsl_dbm = rsl_dbm;
    }
    return ret;
}

static int responder_poll_once(uint64_t local_anchor_id,
                               const struct dwm3000_range_request *expected,
                               uint32_t timeout_ms,
                               struct dwm3000_range_result *result)
{
    struct uwb_range_header poll;
    struct uwb_response_frame response;
    struct uwb_final_frame final;
    struct uwb_report_frame report;
    uint8_t rx_buffer[UWB_FINAL_LEN + FCS_LEN];
    uint8_t tx_buffer[UWB_RESP_LEN];
    size_t frame_len;
    size_t tx_len;
    uint32_t status = 0u;
    uint32_t resp_tx_time;
    uint64_t poll_rx_ts;
    uint64_t resp_tx_ts;
    uint64_t final_rx_ts;
    uint16_t reply_delay_uus;
    uint8_t quality = 0u;
    int8_t rsl_dbm = 0;
    uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0};
    int32_t distance_mm = 0;
    enum range_status report_status = RANGE_OK;
    bool cir_sampled = false;
    bool capture_final_rsl;
    int ret;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = RANGE_INTERNAL_ERROR;
    }

    if (local_anchor_id == 0u || expected == NULL) {
        return -EINVAL;
    }

    ret = ensure_phy_mode(DWM3000_PHY_RANGE);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=phy ret=%d now=%u seq=%u round=%u\n",
                                ret,
                                k_uptime_get_32(),
                                expected->seq,
                                expected->round_index);
        }
        return ret;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=arm evt=%u seq=%u round=%u now=%u timeout=%u\n",
                            expected->session_id,
                            expected->seq,
                            expected->round_index,
                            k_uptime_get_32(),
                            timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms);
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms,
                        &status, rx_buffer, sizeof(rx_buffer), &frame_len,
                        &poll_rx_ts, &quality, &rsl_dbm, NULL, NULL,
                        NULL, NULL, NULL, NULL, false, true);
    if (ret < 0) {
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT :
                             ret == -EMSGSIZE ? RANGE_BAD_FRAME :
                             RANGE_RX_ERROR;
        }
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=poll-rx ret=%d sys=0x%08x fail=%u now=%u seq=%u round=%u\n",
                                ret,
                                status,
                                (unsigned int)status_to_rx_failure(status),
                                k_uptime_get_32(),
                                expected->seq,
                                expected->round_index);
        }
        if (ret == -ETIMEDOUT) {
            return -ETIMEDOUT;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB responder POLL RX error: ret=%d sys_status=0x%08x failure=%s timeout_ms=%u",
                ret,
                status,
                rx_failure_debug_name(status_to_rx_failure(status)),
                timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms);
#endif
        return -EAGAIN;
    }

    ret = decode_poll_frame(rx_buffer, frame_len, &poll);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=poll-decode ret=%d len=%u type=0x%02x now=%u seq=%u round=%u\n",
                                ret,
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? rx_buffer[2] : 0u,
                                k_uptime_get_32(),
                                expected->seq,
                                expected->round_index);
        }
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_BAD_FRAME;
        }
        return -EAGAIN;
    }
    if (!poll_targets_anchor(&poll, local_anchor_id)) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=poll-other reason=target observed=0x%llx expected=0x%llx seq=%u round=%u\n",
                                (unsigned long long)poll.responder_id,
                                (unsigned long long)local_anchor_id,
                                poll.seq,
                                poll.round_index);
        }
        if (result != NULL) {
            result_set_poll_metadata(result, &poll, poll.responder_id);
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_WRONG_TARGET;
        }
        return -EAGAIN;
    }
    if (!poll_matches_expected(&poll, expected)) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=poll-other reason=identity evt=%u/%u seq=%u/%u round=%u/%u init=0x%llx/0x%llx\n",
                                poll.session_id,
                                expected->session_id,
                                poll.seq,
                                expected->seq,
                                poll.round_index,
                                expected->round_index,
                                (unsigned long long)poll.initiator_id,
                                (unsigned long long)expected->initiator_id);
        }
        if (result != NULL) {
            result_set_poll_metadata(result, &poll, local_anchor_id);
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_WRONG_TARGET;
        }
        return -EAGAIN;
    }
    if (result != NULL) {
        result_set_poll_metadata(result, &poll, local_anchor_id);
        result->exchange_start_ms = k_uptime_get();
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->exchange_started = true;
        result->status = RANGE_INTERNAL_ERROR;
    }
    reply_delay_uus = request_reply_delay_uus(expected);

    resp_tx_time = delayed_tx_time_from_rx_reference(poll_rx_ts,
                                                     reply_delay_uus);
    resp_tx_ts = delayed_tx_timestamp_from_programmed_time(resp_tx_time);

    response.header.type = MSG_UWB_RESP;
    response.header.seq = poll.seq;
    response.header.round_index = poll.round_index;
    response.header.network_id = poll.network_id;
    response.header.session_id = poll.session_id;
    response.header.session_nonce = poll.session_nonce;
    response.header.initiator_short_addr = poll.initiator_short_addr;
    response.header.responder_short_addr = short_addr_from_id(local_anchor_id);
    response.header.flags = poll.flags;
    response.header.initiator_id = poll.initiator_id;
    response.header.responder_id = local_anchor_id;
    response.poll_rx_ts_32 = (uint32_t)poll_rx_ts;
    response.resp_tx_ts_32 = (uint32_t)resp_tx_ts;

    ret = uwb_encode_response(&response, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        if (result != NULL) {
            result->status = RANGE_INTERNAL_ERROR;
        }
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(ds_twr_rx_after_tx_delay_uus(reply_delay_uus));
    dwt_setrxtimeout(ds_twr_rx_timeout_uus(reply_delay_uus));
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    dwt_setdelayedtrxtime(resp_tx_time);
    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        if (result != NULL) {
            result->status = RANGE_DELAYED_TX_MISSED;
        }
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=resp-tx ret=%d sys=0x%08x now=%u seq=%u round=%u tx_time=0x%08x\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                poll.seq,
                                poll.round_index,
                                resp_tx_time);
        }
        return -ETIME;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=resp-armed now=%u seq=%u round=%u tx_time=0x%08x resp_tx=0x%08x\n",
                            k_uptime_get_32(),
                            poll.seq,
                            poll.round_index,
                            resp_tx_time,
                            (unsigned int)resp_tx_ts);
        status_debug_printf("DBG_DS_RESP stage=resp-raw poll_rx=0x%08x q=%u reply_uus=%u\n",
                            (unsigned int)poll_rx_ts,
                            quality,
                            reply_delay_uus);
    }
    dwm3000_debug_event(false,
                        "DS_TWR_TIMING",
                        "role=responder reply_delay_uus=%u final_rx_after_uus=%u final_rx_timeout_uus=%u wait_timeout_ms=%u",
                        reply_delay_uus,
                        ds_twr_rx_after_tx_delay_uus(reply_delay_uus),
                        ds_twr_rx_timeout_uus(reply_delay_uus),
                        ds_twr_rx_wait_timeout_ms(reply_delay_uus));

    capture_final_rsl = expected->capture_rsl;
    ret = receive_frame(ds_twr_rx_wait_timeout_ms(reply_delay_uus), &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len,
                        &final_rx_ts, &quality, &rsl_dbm,
                        cir_sample, &cir_sampled,
                        result != NULL ? &result->clock_offset_raw : NULL,
                        result != NULL ? &result->clock_offset_sampled : NULL,
                        result != NULL ? &result->carrier_integrator : NULL,
                        result != NULL ? &result->carrier_integrator_sampled : NULL,
                        capture_final_rsl,
                        true);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=final-rx ret=%d sys=0x%08x fail=%u now=%u seq=%u round=%u\n",
                                ret,
                                status,
                                (unsigned int)status_to_rx_failure(status),
                                k_uptime_get_32(),
                                poll.seq,
                                poll.round_index);
        }
#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB responder FINAL RX failed: ret=%d sys_status=0x%08x failure=%s seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                ret,
                status,
                rx_failure_debug_name(status_to_rx_failure(status)),
                poll.seq,
                poll.round_index,
                ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                reply_delay_uus);
#endif
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : RANGE_RX_ERROR;
        }
        return ret;
    }
    if (result != NULL) {
        result->poll_rx_ts_32 = (uint32_t)poll_rx_ts;
        result->resp_tx_ts_32 = (uint32_t)resp_tx_ts;
        result->final_rx_ts_32 = (uint32_t)final_rx_ts;
    }
    ret = decode_final_frame(rx_buffer, frame_len, &final);
    if (ret < 0) {
        report_status = RANGE_BAD_FRAME;
    } else if (final.header.seq != poll.seq ||
               final.header.round_index != poll.round_index ||
               final.header.network_id != poll.network_id ||
               final.header.session_id != poll.session_id ||
               final.header.session_nonce != poll.session_nonce ||
               final.header.initiator_short_addr != poll.initiator_short_addr ||
               final.header.responder_short_addr != short_addr_from_id(local_anchor_id) ||
               final.header.flags != poll.flags ||
               final.header.initiator_id != poll.initiator_id ||
               final.header.responder_id != local_anchor_id) {
        report_status = RANGE_WRONG_TARGET;
    } else if (validate_driver_reply_timing(
                   dwt_delta_to_uus((uint32_t)poll_rx_ts, (uint32_t)resp_tx_ts),
                   dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
                   reply_delay_uus) != PROTO_OK) {
        report_status = RANGE_TIMING_INVALID;
    } else {
        if (result != NULL) {
            result->poll_tx_ts_32 = final.poll_tx_ts_32;
            result->resp_rx_ts_32 = final.resp_rx_ts_32;
            result->final_tx_ts_32 = final.final_tx_ts_32;
            result->clicker_clock_offset_raw = final.clicker_clock_offset_raw;
            result->clicker_clock_offset_sampled =
                (final.diagnostic_flags &
                 UWB_FINAL_DIAG_CLICKER_CLOCK_OFFSET_PRESENT) != 0u;
        }
        ret = compute_distance_mm(&final, poll_rx_ts, resp_tx_ts, final_rx_ts, &distance_mm);
        if (ret < 0) {
            report_status = RANGE_INTERNAL_ERROR;
        }
    }

    if (result != NULL) {
        result_set_poll_metadata(result, &poll, local_anchor_id);
        result->distance_mm = distance_mm;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = capture_final_rsl;
        if (cir_sampled) {
            memcpy(result->cir_sample, cir_sample, UWB_CIR_SAMPLE_LEN);
        }
        result->cir_sampled = cir_sampled;
        result->status = report_status;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=final-decode status=%u now=%u seq=%u round=%u final_rx=0x%08x\n",
                            report_status,
                            k_uptime_get_32(),
                            poll.seq,
                            poll.round_index,
                            (unsigned int)final_rx_ts);
    }

    if (expected->skip_responder_report &&
        report_status == RANGE_OK &&
        request_expects_clicker_diag(expected)) {
        int diag_ret = receive_clicker_diag(&poll, local_anchor_id, result);

        if (diag_ret < 0) {
            LOG_DBG("UWB clicker diagnostic not received: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    diag_ret);
        } else {
            LOG_INF("UWB clicker diagnostic received: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL ? result->clicker_diag_len : 0u);
        }
    }

    if (expected->skip_responder_report) {
        capture_anchor_full_cir(expected, result);
        LOG_INF("UWB responder report skipped for short=0x%04x: status=%u distance_mm=%d quality=%u rsl=%d dBm",
                poll.initiator_short_addr,
                report_status,
                distance_mm,
                quality,
                rsl_dbm);
        if (report_status == RANGE_OK && request_sends_anchor_diag(expected)) {
            int anchor_diag_ret = send_anchor_diag(&poll, result);

            if (anchor_diag_ret < 0) {
                LOG_WRN("UWB anchor diagnostic TX failed: short=0x%04x seq=%u ret=%d",
                        poll.initiator_short_addr,
                        poll.seq,
                        anchor_diag_ret);
            } else {
                LOG_INF("UWB anchor diagnostic sent: short=0x%04x seq=%u bytes=%u",
                        poll.initiator_short_addr,
                        poll.seq,
                        result != NULL && result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
                if (request_sends_anchor_diag_fragments(expected)) {
                    int fragment_ret = send_anchor_diag_fragments(&poll, expected, result);

                    if (fragment_ret < 0) {
                        LOG_WRN("UWB anchor diagnostic fragments TX failed: short=0x%04x seq=%u ret=%d",
                                poll.initiator_short_addr,
                                poll.seq,
                                fragment_ret);
                    }
                }
            }
        }
        return report_status == RANGE_OK ? 0 : -EIO;
    }

    report.header.type = MSG_UWB_REPORT;
    report.header.seq = poll.seq;
    report.header.round_index = poll.round_index;
    report.header.network_id = poll.network_id;
    report.header.session_id = poll.session_id;
    report.header.session_nonce = poll.session_nonce;
    report.header.initiator_short_addr = poll.initiator_short_addr;
    report.header.responder_short_addr = short_addr_from_id(local_anchor_id);
    report.header.flags = poll.flags;
    report.header.initiator_id = poll.initiator_id;
    report.header.responder_id = local_anchor_id;
    report.distance_mm = distance_mm;
    report.quality = quality;
    report.status = report_status;
    report.rsl_dbm = rsl_dbm;

    ret = uwb_encode_report(&report, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=report-tx ret=%d sys=0x%08x now=%u seq=%u round=%u\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                poll.seq,
                                poll.round_index);
        }
        return ret;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=report-armed now=%u seq=%u round=%u\n",
                            k_uptime_get_32(),
                            poll.seq,
                            poll.round_index);
    }

    ret = wait_tx_complete(REPORT_RX_TIMEOUT_MS);
    if (ret < 0) {
        if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
            status_debug_printf("DBG_DS_RESP stage=report-complete ret=%d sys=0x%08x now=%u seq=%u round=%u\n",
                                ret,
                                (unsigned int)dwt_read32bitreg(SYS_STATUS_ID),
                                k_uptime_get_32(),
                                poll.seq,
                                poll.round_index);
        }
        return ret;
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=report-sent now=%u seq=%u round=%u\n",
                            k_uptime_get_32(),
                            poll.seq,
                            poll.round_index);
    }
    if (DWM3000_DS_TWR_RTT_DEBUG_ENABLED) {
        status_debug_printf("DBG_DS_RESP stage=done ret=%d status=%u now=%u seq=%u round=%u\n",
                            report_status == RANGE_OK ? 0 : -EIO,
                            report_status,
                            k_uptime_get_32(),
                            poll.seq,
                            poll.round_index);
    }

    LOG_INF("UWB report sent to short=0x%04x: status=%u distance_mm=%d quality=%u rsl=%d dBm",
            poll.initiator_short_addr,
            report.status,
            report.distance_mm,
            report.quality,
            report.rsl_dbm);
    if (report_status == RANGE_OK && request_expects_clicker_diag(expected)) {
        int diag_ret = receive_clicker_diag(&poll, local_anchor_id, result);

        if (diag_ret < 0) {
            LOG_DBG("UWB clicker diagnostic not received: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    diag_ret);
        } else {
            LOG_INF("UWB clicker diagnostic received: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL ? result->clicker_diag_len : 0u);
        }
    }
    if (report_status == RANGE_OK && request_sends_anchor_diag(expected)) {
        int anchor_diag_ret = send_anchor_diag(&poll, result);

        if (anchor_diag_ret < 0) {
            LOG_WRN("UWB anchor diagnostic TX failed: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    anchor_diag_ret);
        } else {
            LOG_INF("UWB anchor diagnostic sent: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL && result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
            if (request_sends_anchor_diag_fragments(expected)) {
                int fragment_ret = send_anchor_diag_fragments(&poll, expected, result);

                if (fragment_ret < 0) {
                    LOG_WRN("UWB anchor diagnostic fragments TX failed: short=0x%04x seq=%u ret=%d",
                            poll.initiator_short_addr,
                            poll.seq,
                            fragment_ret);
                }
            }
        }
    }
    return report_status == RANGE_OK ? 0 : -EIO;
}

int dwm3000_driver_responder_poll_expected(uint64_t local_anchor_id,
                                           const struct dwm3000_range_request *expected,
                                           uint32_t timeout_ms,
                                           struct dwm3000_range_result *result)
{
    int ret;

    if (expected == NULL) {
        return -EINVAL;
    }
    ret = validate_range_request(expected);
    if (ret < 0) {
        return ret;
    }
    if (expected->responder_id != local_anchor_id ||
        expected->responder_short_addr != short_addr_from_id(local_anchor_id)) {
        return -EINVAL;
    }

    return responder_poll_once(local_anchor_id, expected, timeout_ms, result);
}

void dwm3000_driver_stats_get(struct dwm3000_driver_stats *stats)
{
    if (stats != NULL) {
        *stats = driver_stats;
    }
}
