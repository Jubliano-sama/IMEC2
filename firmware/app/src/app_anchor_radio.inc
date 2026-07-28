#include "app_anchor_radio_policy.h"
#include "app_mesh_radio_client.h"

#line 6569 __BASE_FILE__
static void anchor_uwb_scan_schedule_ms(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    int ret = k_work_reschedule_for_queue(&anchor_uwb_scan_work_q,
                                          &anchor_uwb_scan_work,
                                          K_MSEC(delay_ms));

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_RESCHED ret=%d delay=%u\n",
                            ret,
                            delay_ms);
    }
#else
    ARG_UNUSED(delay_ms);
#endif
}

static int anchor_queue_survey_sample_result(
    const struct survey_pair *pair,
    uint16_t round_id,
    uint16_t sample_index,
    uint64_t reporter_id,
    const struct dwm3000_range_result *range_result)
{
    struct mesh_outbound outbound = {0};
    struct survey_sample sample = {0};
    uint64_t absolute_deadline_ms;
    uint64_t now_ms;
    size_t payload_len = 0u;
    enum range_status status;
    int64_t sample_local_ms;
    int ret;

    if (pair == NULL || reporter_id == 0u || range_result == NULL) {
        return -EINVAL;
    }

    status = range_status_valid(range_result->status) ?
             range_result->status : RANGE_INTERNAL_ERROR;

    sample.pair = *pair;
    sample.round_id = round_id;
    sample.sample_index = sample_index;
    sample.distance_mm = range_result->distance_mm;
    sample.quality = range_result->quality;
    sample.range_status = status;

    ret = survey_append_sample_tlvs(outbound.payload,
                                    sizeof(outbound.payload),
                                    &payload_len,
                                    &sample);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    sample_local_ms = range_result->exchange_started ?
                      range_result->exchange_start_ms :
                      k_uptime_get();
    ret = anchor_append_sequence_time_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           sample_local_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = append_range_result_timing_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          range_result);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_result_packet_from_reporter(
        &outbound.packet,
        &sample,
        reporter_id,
        GATEWAY_ID,
        app_anchor_survey_runtime_next_sequence(),
        (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    now_ms = (uint64_t)k_uptime_get();
    absolute_deadline_ms = UINT64_MAX - now_ms <
                           SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS ?
                           UINT64_MAX :
                           now_ms + SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS;
    return app_node_comm_submit_reliable_uplink(
        &outbound,
        absolute_deadline_ms,
        ((uint32_t)MSG_SURVEY_PAIR_RESULT << 16) | outbound.packet.seq,
        NULL);
}

static bool anchor_relay_tx_active(void)
{
    return mesh_relay_tx_active(&mesh_runtime);
}

static void anchor_range_window_record(
    struct anchor_range_window_report *report,
    const struct dwm3000_range_result *result)
{
    int8_t sampled_rsl = 0;
    uint8_t sampled_cir[UWB_CIR_SAMPLE_LEN] = {0};
    uint16_t sampled_full_cir_len = 0u;
    uint16_t sampled_full_cir_total_len = 0u;
    uint16_t sampled_full_cir_first_path_index = 0u;
    uint16_t sampled_full_cir_start_index = 0u;
    bool sampled_full_cir_truncated = false;

    if (report == NULL || result == NULL) {
        return;
    }

    if (report->rsl_sampled) {
        sampled_rsl = report->result.rsl_dbm;
    }
    if (report->cir_sampled) {
        memcpy(sampled_cir, report->result.cir_sample, UWB_CIR_SAMPLE_LEN);
    }
    if (report->anchor_full_cir_sampled) {
        sampled_full_cir_len = report->result.anchor_full_cir_len;
        sampled_full_cir_total_len = report->result.anchor_full_cir_total_len;
        sampled_full_cir_first_path_index =
            report->result.anchor_full_cir_first_path_index;
        sampled_full_cir_start_index = report->result.anchor_full_cir_start_index;
        sampled_full_cir_truncated = report->result.anchor_full_cir_truncated;
    }
    if (!report->have_exchange_start_ms && result->exchange_started) {
        report->first_exchange_start_ms = result->exchange_start_ms;
        report->have_exchange_start_ms = true;
    }

    if (!report->have_result || result->status == RANGE_OK) {
        report->result = *result;
        report->have_result = true;
    }
    if (report->have_exchange_start_ms) {
        report->result.exchange_start_ms = report->first_exchange_start_ms;
        report->result.exchange_started = true;
    }
    if (result->rsl_sampled && !report->rsl_sampled) {
        sampled_rsl = result->rsl_dbm;
        report->rsl_sampled = true;
    }
    if (report->rsl_sampled) {
        report->result.rsl_dbm = sampled_rsl;
        report->result.rsl_sampled = true;
    }
    if (result->cir_sampled && !report->cir_sampled) {
        memcpy(sampled_cir, result->cir_sample, UWB_CIR_SAMPLE_LEN);
        report->cir_sampled = true;
    }
    if (report->cir_sampled) {
        memcpy(report->result.cir_sample, sampled_cir, UWB_CIR_SAMPLE_LEN);
        report->result.cir_sampled = true;
    }
    if (result->anchor_full_cir_sampled && !report->anchor_full_cir_sampled) {
        sampled_full_cir_len = result->anchor_full_cir_len;
        sampled_full_cir_total_len = result->anchor_full_cir_total_len;
        sampled_full_cir_first_path_index = result->anchor_full_cir_first_path_index;
        sampled_full_cir_start_index = result->anchor_full_cir_start_index;
        sampled_full_cir_truncated = result->anchor_full_cir_truncated;
        report->anchor_full_cir_sampled = true;
    }
    if (report->anchor_full_cir_sampled) {
        report->result.anchor_full_cir_len = sampled_full_cir_len;
        report->result.anchor_full_cir_total_len = sampled_full_cir_total_len;
        report->result.anchor_full_cir_first_path_index =
            sampled_full_cir_first_path_index;
        report->result.anchor_full_cir_start_index = sampled_full_cir_start_index;
        report->result.anchor_full_cir_truncated = sampled_full_cir_truncated;
        report->result.anchor_full_cir_sampled = true;
    }

    if (result->status != RANGE_OK ||
        report->sample_count >= RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return;
    }

    report->distance_samples_mm[report->sample_count] = result->distance_mm;
    report->sample_sequence_start_ms[report->sample_count] =
        result->exchange_started ? result->exchange_start_ms : k_uptime_get();
    report->range_round_indices[report->sample_count] = result->round_index;
    report->distance_sum_mm += result->distance_mm;
    report->quality_sum += result->quality;
    report->sample_count++;
}

static void anchor_range_window_finalize(
    struct anchor_range_window_report *report)
{
    if (report == NULL || !report->have_result || report->sample_count == 0u) {
        return;
    }

    report->result.distance_mm = average_i32_nearest(report->distance_sum_mm,
                                                     report->sample_count);
    report->result.quality = (uint8_t)((report->quality_sum +
                                        (report->sample_count / 2u)) /
                                       report->sample_count);
    report->result.status = RANGE_OK;
}

static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);

static void anchor_set_uwb_busy(bool busy)
{
    k_spinlock_key_t key = k_spin_lock(&anchor_uwb_lock);

    anchor_uwb_busy = busy;
    k_spin_unlock(&anchor_uwb_lock, key);
}

static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us)
{
    int64_t elapsed_ms;
    uint64_t elapsed_us;

    if (DEVICE_ROLE != ROLE_ANCHOR || start_ms < 0) {
        return;
    }

    elapsed_ms = k_uptime_get() - start_ms;
    if (elapsed_ms <= 0) {
        return;
    }

    elapsed_us = (uint64_t)elapsed_ms * 1000u;
    if (elapsed_us <= already_counted_us) {
        return;
    }
    elapsed_us -= already_counted_us;
    uwb_anchor_note_awake_time(&anchor_uwb_session,
                               (uint32_t)MIN(elapsed_us, (uint64_t)UINT32_MAX));
}

static void anchor_hold_channel5_rx_until_ms(int64_t deadline_ms,
                                             bool *deferred_mesh_rx_queued)
{
    uint32_t frame_count = 0u;
    uint32_t error_count = 0u;

    while (k_uptime_get() < deadline_ms) {
        struct uwb_wake_claim_frame repeated_claim;
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        int ret;

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      anchor_uwb_scan_frame,
                                                      sizeof(anchor_uwb_scan_frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT || ret == -ECANCELED) {
            break;
        }
        if (ret < 0) {
            error_count++;
            continue;
        }
        frame_count++;

        if (uwb_decode_wake_claim(anchor_uwb_scan_frame,
                                  frame_len,
                                  &repeated_claim) == PROTO_OK) {
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(anchor_uwb_scan_frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued && deferred_mesh_rx_queued != NULL) {
                *deferred_mesh_rx_queued = true;
            }
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_HOLD_DONE deadline=%lld now=%u frames=%u errors=%u deferred=%u\n",
                            (long long)deadline_ms,
                            k_uptime_get_32(),
                            frame_count,
                            error_count,
                            (deferred_mesh_rx_queued != NULL &&
                             *deferred_mesh_rx_queued) ? 1u : 0u);
    }
}

static uint32_t anchor_run_scheduled_uwb_ranges(const struct uwb_range_schedule_frame *schedule,
                                                int64_t schedule_rx_ms,
                                                bool *deferred_mesh_rx_queued)
{
    struct anchor_range_window_report window_report = {0};
    uint8_t *anchor_full_cir = NULL;
    size_t anchor_full_cir_cap = 0u;
    uint32_t retained_sleep_us = 0u;
    size_t total_samples;
    int64_t no_poll_deadline_ms;
    bool poll_received = false;
    bool poll_guard_expired = false;
    bool last_local_exchange_ok = false;

    if (schedule == NULL) {
        return 0u;
    }

    if ((schedule->flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) != 0u) {
        anchor_full_cir = mesh_anchor_click_cir_capture_begin(&anchor_full_cir_cap);
    }

    total_samples = uwb_range_schedule_total_samples(schedule);
    no_poll_deadline_ms = k_uptime_get() + ANCHOR_SCHEDULE_NO_POLL_GUARD_MS;
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_enter_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    LOG_INF("anchor scheduled UWB ranging start: clicker=0x%016llx event_seq=%u selected=%u total_samples=%u",
            (unsigned long long)schedule->clicker_id,
            schedule->click_event_id,
            schedule->selected_count,
            (unsigned int)total_samples);
    app_anchor_log_range_schedule("anchor", schedule);

    for (size_t sample_index = 0u; sample_index < total_samples; sample_index++) {
        struct dwm3000_range_request expected;
        struct dwm3000_range_result range_result;
        struct uwb_range_exchange_identity identity;
        uint64_t anchor_id = 0u;
        uint8_t seq = 0u;
        uint8_t round_index = 0u;
        int64_t listen_start_ms;
        int64_t listen_deadline_ms;
        int ret = -ETIMEDOUT;

        ret = uwb_range_schedule_sample_at(schedule, sample_index, &anchor_id, &seq);
        if (ret != PROTO_OK || anchor_id != DEVICE_ID) {
            high_debug_log_event("DS_TWR_POLL_RX",
                                 "sample=%u/%u expected_anchor=0x%016llx local_anchor=0x%016llx result=wrong-target",
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 (unsigned long long)anchor_id,
                                 (unsigned long long)DEVICE_ID);
            continue;
        }

        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        memset(&identity, 0, sizeof(identity));
        identity.network_id = schedule->network_id;
        identity.clicker_id = schedule->clicker_id;
        identity.click_event_id = schedule->click_event_id;
        identity.attempt_index = schedule->attempt_index;
        identity.nonce = schedule->nonce;
        identity.anchor_id = anchor_id;
        identity.ranging_channel = schedule->ranging_channel;
        identity.reply_delay_us = schedule->reply_delay_us;
        identity.seq = seq;
        identity.flags = schedule->flags;
        ret = uwb_anchor_range_round_index(&anchor_uwb_session,
                                           &identity,
                                           &round_index);
        if (ret != PROTO_OK) {
            uwb_anchor_note_timing_rejection(&anchor_uwb_session);
            continue;
        }
        uwb_anchor_note_sample_order(&anchor_uwb_session);

        listen_start_ms =
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index) / 1000;
        listen_start_ms -= UWB_SCHEDULE_GUARD_MS;
        if (!poll_received && listen_start_ms > no_poll_deadline_ms) {
            anchor_hold_channel5_rx_until_ms(no_poll_deadline_ms,
                                             deferred_mesh_rx_queued);
            poll_guard_expired = true;
            break;
        }
        anchor_hold_channel5_rx_until_ms(listen_start_ms,
                                         deferred_mesh_rx_queued);

        listen_deadline_ms = ceil_us_to_ms(
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index + 1u));
        listen_deadline_ms += UWB_SCHEDULE_GUARD_MS;

        memset(&expected, 0, sizeof(expected));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        expected.initiator_id = schedule->clicker_id;
        expected.responder_id = DEVICE_ID;
        expected.network_id = schedule->network_id;
        expected.session_nonce = schedule->nonce;
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = schedule->click_event_id;
        expected.seq = seq;
        expected.round_index = round_index;
        expected.flags = schedule->flags;
        expected.capture_rsl = true;
        expected.skip_responder_report = false;
        expected.expect_clicker_diag = false;
        expected.send_anchor_diag = false;

        LOG_INF("anchor scheduled UWB sample listen: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                (unsigned int)(sample_index + 1u),
                (unsigned int)total_samples,
                round_index,
                seq);
        stage1_led_phase(STAGE1_LED_PHASE_RANGE);
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
        high_debug_log_event("DS_TWR_POLL_RX",
                             "listen=1 clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u schedule_rx_ms=%lld target_us=%lld listen_start_ms=%lld now_ms=%lld deadline_ms=%lld first_poll_ms=%u stride_us=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             uwb_schedule_burst_id(schedule->click_event_id,
                                                   schedule->attempt_index),
                             (unsigned int)(sample_index + 1u),
                             (unsigned int)total_samples,
                             round_index,
                             seq,
                             (long long)schedule_rx_ms,
                             (long long)scheduled_range_sample_target_us(schedule_rx_ms,
                                                                         schedule,
                                                                         sample_index),
                             (long long)listen_start_ms,
                             (long long)k_uptime_get(),
                             (long long)listen_deadline_ms,
                             schedule->first_poll_delay_ms,
                             schedule->exchange_stride_us);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DS_SAMPLE_ARM evt=%u attempt=%u sample=%u/%u round=%u seq=%u target_us=%lld start=%lld now=%u deadline=%lld\n",
                                schedule->click_event_id,
                                schedule->attempt_index,
                                (unsigned int)(sample_index + 1u),
                                (unsigned int)total_samples,
                                round_index,
                                seq,
                                (long long)scheduled_range_sample_target_us(
                                    schedule_rx_ms, schedule, sample_index),
                                (long long)listen_start_ms,
                                k_uptime_get_32(),
                                (long long)listen_deadline_ms);
        }
        while (k_uptime_get() < listen_deadline_ms) {
            int64_t bounded_deadline_ms = listen_deadline_ms;
            int64_t remaining_ms;

            if (!poll_received && bounded_deadline_ms > no_poll_deadline_ms) {
                bounded_deadline_ms = no_poll_deadline_ms;
            }
            remaining_ms = bounded_deadline_ms - k_uptime_get();
            if (remaining_ms <= 0) {
                break;
            }

            expected.timeout_ms = (uint32_t)MAX(1, remaining_ms);
            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         expected.timeout_ms,
                                                         &range_result);
            if (ret == -EAGAIN) {
                if (range_result.status == RANGE_WRONG_TARGET) {
                    high_debug_log_event("DS_TWR_POLL_RX",
                                         "result=wrong-target expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                                         (unsigned long long)schedule->clicker_id,
                                         (unsigned long long)range_result.initiator_id,
                                         (unsigned long long)DEVICE_ID,
                                         (unsigned long long)range_result.responder_id,
                                         schedule->click_event_id,
                                         range_result.session_id,
                                         round_index,
                                         range_result.round_index,
                                         seq,
                                         range_result.seq);
                    LOG_DBG("anchor ignored nonmatching UWB POLL: expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                            (unsigned long long)schedule->clicker_id,
                            (unsigned long long)range_result.initiator_id,
                            (unsigned long long)DEVICE_ID,
                            (unsigned long long)range_result.responder_id,
                            schedule->click_event_id,
                            range_result.session_id,
                            round_index,
                            range_result.round_index,
                            seq,
                            range_result.seq);
                } else if (range_result.status == RANGE_BAD_FRAME ||
                           range_result.status == RANGE_RX_ERROR) {
                    LOG_DBG("anchor ignored pre-POLL UWB frame inside scheduled slot: status=%s(%u) expected_clicker=0x%016llx event_seq=%u seq=%u",
                            range_status_name(range_result.status),
                            range_result.status,
                            (unsigned long long)schedule->clicker_id,
                            schedule->click_event_id,
                            seq);
                }
                continue;
            }
            break;
        }
        if (ret == -ECANCELED) {
            goto scheduled_range_cancelled;
        }
        if (range_result.exchange_started) {
            poll_received = true;
        }
        last_local_exchange_ok = ret == 0 &&
                                 range_result.exchange_started &&
                                 range_result.status == RANGE_OK;
        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        if (!range_result.exchange_started) {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
            LOG_WRN("anchor scheduled UWB sample did not start: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret);
            high_debug_log_event("RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s exchange_started=0",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status));
        } else if (ret == 0 && range_result.status == RANGE_OK) {
            HIGH_DEBUG_COUNTER_INC(ds_twr_successes);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, RANGE_OK);
            anchor_range_window_record(&window_report, &range_result);
            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor scheduled UWB sample complete: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u count=%u",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality,
                    window_report.sample_count);
            high_debug_log_event("RANGE_OK",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u anchor=0x%016llx distance_mm=%d quality=%u",
                                 (unsigned long long)range_result.initiator_id,
                                 range_result.session_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 range_result.seq,
                                 (unsigned long long)range_result.responder_id,
                                 range_result.distance_mm,
                                 range_result.quality);
        } else {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            if (range_result.status == RANGE_TIMING_INVALID) {
                HIGH_DEBUG_COUNTER_INC(ds_twr_timing_rejects);
            }
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            if (range_result.initiator_id != 0u && range_result.responder_id != 0u) {
                anchor_range_window_record(&window_report, &range_result);
            }
            stage1_led_result(range_result.status == RANGE_RX_TIMEOUT ?
                              STAGE1_LED_RESULT_TIMEOUT :
                              STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor scheduled UWB sample failed: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status);
            high_debug_log_event(range_result.status == RANGE_TIMING_INVALID ?
                                 "RANGE_TIMING_REJECT" : "RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s status=%u",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status),
                                 range_result.status);
        }
    }

    if (last_local_exchange_ok && window_report.have_result &&
        anchor_full_cir != NULL && anchor_full_cir_cap > 0u) {
        int cir_ret = dwm3000_driver_capture_last_rx_cir(
            anchor_full_cir,
            (uint16_t)anchor_full_cir_cap,
            &window_report.result);

        window_report.anchor_full_cir_sampled =
            window_report.result.anchor_full_cir_sampled;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DS_BURST stage=cir-after-ranges ret=%d evt=%u seq=%u round=%u bytes=%u\n",
                                cir_ret,
                                schedule->click_event_id,
                                window_report.result.seq,
                                window_report.result.round_index,
                                window_report.result.anchor_full_cir_len);
        }
    }

    if (poll_guard_expired) {
        LOG_WRN("anchor scheduled UWB poll guard expired: clicker=0x%016llx event_seq=%u waited_ms=%u total_samples=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                ANCHOR_SCHEDULE_NO_POLL_GUARD_MS,
                (unsigned int)total_samples);
        high_debug_log_event("RANGE_FAIL",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=no-poll guard_ms=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             ANCHOR_SCHEDULE_NO_POLL_GUARD_MS);
    } else if (poll_received &&
               schedule->diagnostics_required != UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED) {
#if defined(CONFIG_IMEC_ML_ANCHOR)
        anchor_scan_recovery_gap_requested = true;
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            ml_anchor_run_post_burst_diagnostics(schedule, schedule_rx_ms, total_samples));
#endif
    }

    anchor_range_window_finalize(&window_report);
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    build_uwb_schedule_report_if_relevant(&anchor_uwb_session,
                                          schedule->flags,
                                          &window_report);
#endif
scheduled_range_cancelled:
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_exit_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    return retained_sleep_us;
}

#if defined(CONFIG_IMEC_ML_ANCHOR)
static int anchor_send_pair_survey_result(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    uint8_t pair_index,
    uint64_t initiator_id,
    uint64_t responder_id,
    const struct dwm3000_range_result *range_result)
{
    struct uwb_anchor_pair_result_frame result = {0};
    uint8_t frame[UWB_ANCHOR_PAIR_RESULT_LEN];
    size_t frame_len = 0u;
    int ret;

    if (schedule == NULL || range_result == NULL) {
        return -EINVAL;
    }

    result.network_id = schedule->network_id;
    result.clicker_id = schedule->clicker_id;
    result.survey_id = schedule->survey_id;
    result.nonce = schedule->nonce;
    result.initiator_id = initiator_id;
    result.responder_id = responder_id;
    result.pair_index = pair_index;
    result.pair_count = schedule->pair_count;
    result.seq = (uint8_t)(pair_index + 1u);
    result.status = range_status_valid(range_result->status) ?
                    range_result->status : RANGE_INTERNAL_ERROR;
    result.quality = range_result->quality;
    result.distance_mm = range_result->status == RANGE_OK ?
                         range_result->distance_mm : 0;
    result.rsl_dbm = range_result->rsl_sampled ? range_result->rsl_dbm : 0;
    result.flags = schedule->flags;

    ret = uwb_encode_anchor_pair_result(&result, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        LOG_WRN("anchor pair-survey result TX failed: survey=%u pair=%u ret=%d",
                schedule->survey_id,
                pair_index,
                ret);
        return ret;
    }
    LOG_INF("anchor pair-survey result TX: survey=%u pair=%u/%u initiator=0x%016llx responder=0x%016llx status=%s(%u) distance_mm=%d",
            schedule->survey_id,
            (unsigned int)(pair_index + 1u),
            schedule->pair_count,
            (unsigned long long)initiator_id,
            (unsigned long long)responder_id,
            range_status_name(result.status),
            result.status,
            result.distance_mm);
    return 0;
}

static uint32_t anchor_run_clicker_pair_survey(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_rx_ms)
{
    uint32_t retained_sleep_us = 0u;
    uint16_t local_start_delay_ms = 0u;
    bool local_listed = false;

    if (!app_anchor_radio_pair_schedule_matches_epoch(
            schedule, &anchor_uwb_session.epoch)) {
        LOG_WRN("anchor rejected pair-survey schedule identity");
        return 0u;
    }

    for (uint8_t i = 0u; i < schedule->anchor_count; i++) {
        if (schedule->anchor_ids[i] == DEVICE_ID) {
            local_start_delay_ms = schedule->anchor_start_delay_ms[i];
            local_listed = true;
            break;
        }
    }
    if (!local_listed) {
        LOG_INF("anchor pair-survey schedule not for local anchor: survey=%u",
                schedule->survey_id);
        return 0u;
    }

    LOG_INF("anchor pair-survey start: survey=%u anchors=%u pairs=%u schedule_start_ms=%u stride_ms=%u window_ms=%u rx_guard_ms=%u",
            schedule->survey_id,
            schedule->anchor_count,
            schedule->pair_count,
            local_start_delay_ms,
            schedule->pair_stride_ms,
            schedule->pair_window_ms,
            UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS);

    for (uint8_t pair_index = 0u; pair_index < schedule->pair_count; pair_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result range_result = {0};
        uint64_t initiator_id = 0u;
        uint64_t responder_id = 0u;
        int64_t target_ms;
        int low_power_ret = 0;
        int ret;

        ret = uwb_anchor_pair_at(schedule, pair_index, &initiator_id, &responder_id);
        if (ret != PROTO_OK) {
            continue;
        }
        if (DEVICE_ID != initiator_id && DEVICE_ID != responder_id) {
            continue;
        }

        target_ms = schedule_rx_ms + local_start_delay_ms +
                    ((int64_t)pair_index * schedule->pair_stride_ms);
        request.initiator_id = initiator_id;
        request.responder_id = responder_id;
        request.network_id = schedule->network_id;
        request.session_nonce = schedule->nonce;
        request.responder_short_addr = uwb_session_short_addr_from_id(responder_id);
        request.session_id = schedule->survey_id;
        request.seq = (uint8_t)(pair_index + 1u);
        request.round_index = 0u;
        request.flags = schedule->flags;
        request.timeout_ms = schedule->pair_window_ms;
        request.reply_delay_uus = 0u;
        request.capture_rsl = false;
        request.skip_responder_report = false;

        if (DEVICE_ID == initiator_id) {
            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(target_ms));
            memset(&range_result, 0, sizeof(range_result));
            range_result.status = RANGE_RX_TIMEOUT;
            ret = dwm3000_driver_range_initiator(&request, &range_result);
            low_power_ret = anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,
                                                   "pair-survey-initiator");
            if (ret == -ECANCELED) {
                return retained_sleep_us;
            }
            if (ret < 0 && !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            (void)anchor_send_pair_survey_result(schedule,
                                                 pair_index,
                                                 initiator_id,
                                                 responder_id,
                                                 &range_result);
        } else {
            int64_t listen_start_ms = target_ms -
                                      UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS;
            int64_t listen_deadline_ms = target_ms + schedule->pair_window_ms;

            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(listen_start_ms));
            while (k_uptime_get() < listen_deadline_ms) {
                int64_t remaining_ms = listen_deadline_ms - k_uptime_get();

                memset(&range_result, 0, sizeof(range_result));
                range_result.status = RANGE_RX_TIMEOUT;
                request.timeout_ms = (uint32_t)MAX(1, remaining_ms);
                ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                             &request,
                                                             request.timeout_ms,
                                                             &range_result);
                if (ret == -EAGAIN) {
                    continue;
                }
                break;
            }
            low_power_ret = anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,
                                                   "pair-survey-responder");
            if (ret == -ECANCELED) {
                return retained_sleep_us;
            }
        }
        if (low_power_ret < 0) {
            break;
        }
    }

    LOG_INF("anchor pair-survey done: survey=%u", schedule->survey_id);
    return retained_sleep_us;
}
#endif

static bool anchor_handle_uwb_claim(const struct uwb_wake_claim_frame *first_claim,
                                    uint8_t first_quality,
                                    int64_t first_rx_ms,
                                    uint32_t *retained_sleep_us,
                                    bool *deferred_mesh_rx_queued)
{
    struct uwb_wake_claim_frame selected_claim;
    int64_t selected_rx_ms;
    int64_t collect_deadline_ms;
    uint8_t selected_quality = first_quality;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int ret;
    int last_discovery_ret = -ETIMEDOUT;
    int last_discovery_decode_ret = PROTO_OK;
    int64_t discovery_start_ms;
    int64_t discovery_deadline_ms;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    enum uwb_anchor_claim_decision selected_decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    bool discover_received = false;
    bool schedule_frame_ready = false;
    int64_t schedule_rx_ms = -1;
    struct uwb_discover_frame discover = {0};
    bool discovery_uses_extended_phr;
    bool click_priority = false;
    uint32_t discovery_late_guard_ms;

    if (first_claim == NULL) {
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        !app_mesh_c5_wake_claim_requires_anchor_handoff(first_claim->flags,
                                                         true)) {
        status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_REJECT_CLICK_PATH src=0x%llx evt=%u attempt=%u flags=0x%02x\n",
                            (unsigned long long)first_claim->clicker_id,
                            first_claim->click_event_id,
                            first_claim->attempt_index,
                            first_claim->flags);
        return false;
    }
    if (retained_sleep_us != NULL) {
        *retained_sleep_us = 0u;
    }

    selected_claim = *first_claim;
    selected_rx_ms = first_rx_ms;
    HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    high_debug_log_event("WAKE_CLAIM_RX",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx nonce=0x%016llx quality=%u",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         (unsigned long long)first_claim->nonce,
                         first_quality);
    ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                       first_claim,
                                       (uint32_t)first_rx_ms,
                                       &decision);
    if (ret != PROTO_OK) {
        HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        high_debug_log_event("WAKE_CLAIM_REJECT",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                             (unsigned long long)first_claim->clicker_id,
                             first_claim->click_event_id,
                             first_claim->attempt_index,
                             claim_decision_name(decision),
                             ret);
        LOG_DBG("anchor rejected UWB WAKE_CLAIM: clicker=0x%016llx ret=%d decision=%u",
                (unsigned long long)first_claim->clicker_id,
                ret,
                decision);
        return false;
    }
    selected_decision = decision;
    HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
    stage1_led_result(STAGE1_LED_RESULT_OK);
    high_debug_log_event("WAKE_CLAIM_ACCEPT",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         claim_decision_name(decision));
    LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
            (unsigned long long)first_claim->clicker_id,
            first_claim->click_event_id,
            first_claim->attempt_index,
            (unsigned long long)first_claim->priority_id,
            ret,
            decision);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_WAKE_ACCEPT clicker=0x%llx evt=%u attempt=%u flags=0x%02x disc_in=%u q=%u\n",
                            (unsigned long long)first_claim->clicker_id,
                            first_claim->click_event_id,
                            first_claim->attempt_index,
                            first_claim->flags,
                            first_claim->discovery_starts_in_ms,
                            first_quality);
    }
    collect_deadline_ms = k_uptime_get() + ANCHOR_CLAIM_COLLECTION_MS;
    while (k_uptime_get() < collect_deadline_ms) {
        struct uwb_wake_claim_frame claim;
        uint8_t quality = 0u;
        int64_t remaining_ms = collect_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT || ret == -ECANCELED) {
            break;
        }
        if (ret < 0) {
            continue;
        }
        ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (ret != PROTO_OK) {
            enum uwb_wake_decode_failure failure =
                app_anchor_wake_failure_from_proto_ret(ret);

            uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                failure);
            LOG_DBG("anchor ignored competing UWB WAKE_CLAIM decode failure: ret=%d reason=%s frame_len=%u",
                    ret,
                    app_anchor_wake_decode_failure_name(failure),
                    (unsigned int)frame_len);
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            !app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags,
                                                             true)) {
            status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DROP_DURING_CLICK src=0x%llx evt=%u attempt=%u flags=0x%02x\n",
                                (unsigned long long)claim.clicker_id,
                                claim.click_event_id,
                                claim.attempt_index,
                                claim.flags);
            continue;
        }

        ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                           &claim,
                                           k_uptime_get_32(),
                                           &decision);
        HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
        LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
                (unsigned long long)claim.clicker_id,
                claim.click_event_id,
                claim.attempt_index,
                (unsigned long long)claim.priority_id,
                ret,
                decision);
        if (ret == PROTO_OK &&
            (decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
             decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY)) {
            HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
            selected_claim = claim;
            selected_rx_ms = k_uptime_get();
            selected_quality = quality;
            selected_decision = decision;
            stage1_led_result(STAGE1_LED_RESULT_OK);
            high_debug_log_event("WAKE_CLAIM_ACCEPT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 (unsigned long long)claim.priority_id,
                                 claim_decision_name(decision));
        } else {
            HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("WAKE_CLAIM_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 claim_decision_name(decision),
                                 ret);
        }
    }
    if (ret == -ECANCELED) {
        return false;
    }

    LOG_INF("anchor selected UWB claim: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx decision=%u",
            (unsigned long long)anchor_uwb_session.epoch.clicker_id,
            anchor_uwb_session.epoch.click_event_id,
            anchor_uwb_session.epoch.attempt_index,
            (unsigned long long)anchor_uwb_session.epoch.priority_id,
            selected_decision);
    click_priority = app_mesh_c5_wake_claim_preempts_mesh(selected_claim.flags);
    if (click_priority && mesh_preempt_for_click_event() < 0) {
        LOG_ERR("anchor click claim deferred because mesh custody preemption failed");
        return false;
    }
    anchor_click_window_set_active(click_priority);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_WINDOW active=%u flags=0x%02x\n",
                            anchor_click_window_active() ? 1u : 0u,
                            selected_claim.flags);
    }

    discovery_uses_extended_phr =
        app_mesh_c5_wake_followup_uses_extended_phr(selected_claim.flags);
    discovery_late_guard_ms = discovery_uses_extended_phr ?
                              MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS :
                              UWB_DISCOVERY_RX_LATE_GUARD_MS;
    discovery_start_ms =
        (int64_t)selected_rx_ms + selected_claim.discovery_starts_in_ms;
    discovery_deadline_ms = discovery_start_ms + discovery_late_guard_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_WAIT now=%u start=%lld deadline=%lld guard=%u listen=%u phr=%s\n",
                            k_uptime_get_32(),
                            (long long)discovery_start_ms,
                            (long long)discovery_deadline_ms,
                            UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_late_guard_ms + UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_uses_extended_phr ? "ext" : "std");
    }
    high_debug_log_event("DISCOVER_WAIT",
                         "clicker=0x%016llx event_seq=%u attempt=%u selected_rx_ms=%lld discovery_starts_in_ms=%u wait_start_ms=%lld deadline_ms=%lld listen_ms=%u",
                         (unsigned long long)selected_claim.clicker_id,
                         selected_claim.click_event_id,
                         selected_claim.attempt_index,
                         (long long)selected_rx_ms,
                         selected_claim.discovery_starts_in_ms,
                         (long long)(discovery_start_ms - UWB_DISCOVERY_RX_GUARD_MS),
                         (long long)discovery_deadline_ms,
                         UWB_DISCOVERY_LISTEN_MS);

    stage1_led_phase(STAGE1_LED_PHASE_DISCOVERY);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    ret = discovery_uses_extended_phr ?
          dwm3000_driver_configure_wake_mesh_control_mode() :
          dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_CONFIG phr=%s ret=%d\n",
                            discovery_uses_extended_phr ? "ext" : "std",
                            ret);
    }
    if (ret < 0) {
        last_discovery_ret = ret;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        high_debug_log_event("DISCOVER_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u result=config-fail phr=%s ret=%d",
                             (unsigned long long)selected_claim.clicker_id,
                             selected_claim.click_event_id,
                             selected_claim.attempt_index,
                             discovery_uses_extended_phr ? "ext" : "std",
                             ret);
        goto discovery_miss;
    }

    while (k_uptime_get() < discovery_deadline_ms) {
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint8_t quality = 0u;
        int64_t remaining_ms = discovery_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
        if (ret == -ETIMEDOUT || ret == -ECANCELED) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_TIMEOUT now=%u rem=%lld fail=%u\n",
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            break;
        }
        if (ret < 0) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_FAIL ret=%d now=%u rem=%lld fail=%u\n",
                                    ret,
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_FRAME len=%u type=0x%02x q=%u now=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                k_uptime_get_32());
        }

        if ((selected_claim.flags & FLAG_RANGE_ONLY) != 0u &&
            frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_SCHEDULE) {
            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }

        last_discovery_decode_ret = uwb_decode_discover(frame, frame_len, &discover);
        if (last_discovery_decode_ret == PROTO_OK) {
            selected_quality = quality;
            discover_received = true;
            break;
        }

        if (frame_len == UWB_WAKE_CLAIM_LEN) {
            struct uwb_wake_claim_frame late_claim;

            ret = uwb_decode_wake_claim(frame, frame_len, &late_claim);
            if (ret == PROTO_OK) {
                LOG_DBG("anchor ignored late UWB WAKE_CLAIM while waiting for DISCOVER: clicker=0x%016llx event_seq=%u attempt=%u",
                        (unsigned long long)late_claim.clicker_id,
                        late_claim.click_event_id,
                        late_claim.attempt_index);
                continue;
            }
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued && deferred_mesh_rx_queued != NULL) {
                    *deferred_mesh_rx_queued = true;
                }
                high_debug_log_event("MESH_CH5_AFTER_WAKE_RX",
                                     "clicker=0x%016llx event_seq=%u frame_len=%u quality=%u valid=%u",
                                     (unsigned long long)selected_claim.clicker_id,
                                     selected_claim.click_event_id,
                                     (unsigned int)frame_len,
                                     quality,
                                     valid_mesh_frame ? 1u : 0u);
                LOG_INF("anchor accepted channel-5 mesh frame after wake claim: clicker=0x%016llx event_seq=%u len=%u quality=%u valid=%u",
                        (unsigned long long)selected_claim.clicker_id,
                        selected_claim.click_event_id,
                        (unsigned int)frame_len,
                        quality,
                        valid_mesh_frame ? 1u : 0u);
                if (click_priority) {
                    continue;
                }
                return true;
            }
            status_debug_printf("DBG_ANCHOR_MESH_AFTER_WAKE_REJECT len=%u type=0x%02x q=%u discover_ret=%d valid=%u queued=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                last_discovery_decode_ret,
                                valid_mesh_frame ? 1u : 0u,
                                queued ? 1u : 0u);
        }

        LOG_DBG("anchor ignored non-DISCOVER frame during discovery wait: decode_ret=%d frame_len=%u type=0x%02x quality=%u",
                last_discovery_decode_ret,
                (unsigned int)frame_len,
                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                quality);
    }

discovery_miss:
    if (discover_received) {
        struct uwb_discovery_reply_frame reply;
#if defined(CONFIG_IMEC_ML_ANCHOR)
        uint16_t discovery_battery_mv = ml_anchor_cached_battery_mv();
#else
        uint16_t discovery_battery_mv = 0u;
#endif

        HIGH_DEBUG_COUNTER_INC(discovery_rx);
        high_debug_log_event("DISCOVER_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u network_id=0x%08x channel=%u quality=%u",
                             (unsigned long long)discover.clicker_id,
                             discover.click_event_id,
                             discover.attempt_index,
                             discover.network_id,
                             UWB_WAKE_CHANNEL,
                             selected_quality);

        ret = uwb_anchor_build_discovery_reply(&anchor_uwb_session,
                                               &discover,
                                               selected_quality,
                                               discovery_battery_mv,
                                               &reply);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY build rejected: %d", ret);
            return true;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (discover.discovery_slot_count > 0u) {
            reply.anchor_slot = ml_anchor_discovery_slot(discover.discovery_slot_count);
        }
#else
        enum app_discovery_assignment_provisioning_state provisioning_state =
            local_anchor_discovery_assignment_provisioning_state();

        ret = local_anchor_discovery_slot(discover.discovery_slot_count,
                                          &reply.anchor_slot);
        if (ret != PROTO_OK) {
            enum app_discovery_assignment_provisioning_state provisioning_state =
                local_anchor_discovery_assignment_provisioning_state();

            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            if (provisioning_state == APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED event=%u attempt=%u reply=disabled\n",
                                    discover.click_event_id,
                                    discover.attempt_index);
                LOG_WRN("anchor UWB DISCOVERY_REPLY suppressed: assignment UNPROVISIONED event=%u attempt=%u",
                        discover.click_event_id,
                        discover.attempt_index);
            } else {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_INVALID event=%u attempt=%u slot_count=%u ret=%d\n",
                                    discover.click_event_id,
                                    discover.attempt_index,
                                    discover.discovery_slot_count,
                                    ret);
                LOG_WRN("anchor UWB DISCOVERY_REPLY slot rejected: slot_count=%u ret=%d",
                        discover.discovery_slot_count,
                        ret);
            }
            return true;
        }
        if (provisioning_state == APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
            status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED event=%u attempt=%u reply=fallback slot=%u\n",
                                discover.click_event_id,
                                discover.attempt_index,
                                reply.anchor_slot);
            LOG_WRN("anchor UWB DISCOVERY_REPLY using deterministic fallback while UNPROVISIONED: event=%u attempt=%u slot=%u",
                    discover.click_event_id,
                    discover.attempt_index,
                    reply.anchor_slot);
        }
#endif

        sleep_precise_us((uint32_t)reply.anchor_slot * UWB_DISCOVERY_SLOT_US);
        ret = uwb_encode_discovery_reply(&reply, frame, sizeof(frame), &frame_len);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY encode failed: %d", ret);
            return true;
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS);
        if (ret < 0) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY TX failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(discovery_reply_tx);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("DISCOVERY_REPLY_TX",
                             "clicker=0x%016llx event_seq=%u slot=%u slot_source=%s quality=%u",
                             (unsigned long long)reply.selected_clicker_id,
                             reply.click_event_id,
                             reply.anchor_slot,
                             ANCHOR_DISCOVERY_SLOT_SOURCE,
                             reply.rx_quality);
        LOG_INF("anchor UWB DISCOVERY_REPLY sent: clicker=0x%016llx slot=%u source=%s quality=%u",
                (unsigned long long)reply.selected_clicker_id,
                reply.anchor_slot,
                ANCHOR_DISCOVERY_SLOT_SOURCE,
                reply.rx_quality);
    } else if (schedule_frame_ready) {
        stage1_led_result(STAGE1_LED_RESULT_OK);
        LOG_INF("anchor fast range-only path: using direct RANGE_SCHEDULE without DISCOVER reply");
        high_debug_log_event("DISCOVER_SKIP",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=range_only_schedule",
                             (unsigned long long)selected_claim.clicker_id,
                             selected_claim.click_event_id,
                             selected_claim.attempt_index);
    } else {
        stage1_led_result(last_discovery_ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_MISS ret=%d decode=%d now=%u\n",
                                last_discovery_ret,
                                last_discovery_decode_ret,
                                k_uptime_get_32());
        }
        LOG_WRN("anchor UWB DISCOVER not received: ret=%d decode_ret=%d listen_ms=%u",
                last_discovery_ret,
                last_discovery_decode_ret,
                UWB_DISCOVERY_LISTEN_MS);
        return true;
    }

    stage1_led_phase(STAGE1_LED_PHASE_SCHEDULE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!schedule_frame_ready) {
        uint32_t schedule_rx_window_ms =
            UWB_RANGE_SCHEDULE_RX_MS +
            discovery_window_ms_for_slots(discover.discovery_slot_count) +
            UWB_DISCOVERY_RX_GUARD_MS;
        int64_t schedule_deadline_ms = k_uptime_get() + schedule_rx_window_ms;
        int last_schedule_ret = -ETIMEDOUT;

        while (k_uptime_get() < schedule_deadline_ms) {
            uint8_t frame_type = 0u;
            int64_t remaining_ms = schedule_deadline_ms - k_uptime_get();

            ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                          frame,
                                                          sizeof(frame),
                                                          &frame_len,
                                                          NULL,
                                                          NULL,
                                                          NULL);
            if (ret == -ETIMEDOUT || ret == -ECANCELED) {
                break;
            }
            if (ret < 0) {
                last_schedule_ret = ret;
                continue;
            }
            if (frame_len < UWB_SYNC_HEADER_LEN) {
                last_schedule_ret = -EBADMSG;
                continue;
            }

            frame_type = frame[2];
            if (frame_type != MSG_UWB_RANGE_SCHEDULE &&
#if defined(CONFIG_IMEC_ML_ANCHOR)
                frame_type != MSG_UWB_ANCHOR_PAIR_SCHEDULE &&
#endif
                frame_type != MSG_UWB_RANGE_RELEASE) {
                last_schedule_ret = -EBADMSG;
                LOG_DBG("anchor ignored non-schedule frame while waiting for RANGE_SCHEDULE: type=0x%02x len=%u",
                        frame_type,
                        (unsigned int)frame_len);
                continue;
            }

            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }
        if (!schedule_frame_ready) {
            ret = last_schedule_ret;
        }
    }
    if (ret == 0) {
        struct uwb_range_schedule_frame schedule;

        if (schedule_rx_ms < 0) {
            schedule_rx_ms = k_uptime_get();
        }

        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_RELEASE) {
            struct uwb_range_release_frame release;

            ret = uwb_decode_range_release(frame, frame_len, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor UWB RANGE_RELEASE decode failed: %d", ret);
                return true;
            }

            ret = uwb_anchor_accept_range_release(&anchor_uwb_session, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected UWB RANGE_RELEASE: %d", ret);
                return true;
            }

            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor UWB RANGE_RELEASE accepted: clicker=0x%016llx event_seq=%u discovered=%u min=%u reason=%u",
                    (unsigned long long)release.clicker_id,
                    release.click_event_id,
                    release.discovered_anchor_count,
                    release.min_anchor_count,
                    release.reason);
            return true;
        }

#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_ANCHOR_PAIR_SCHEDULE) {
            struct uwb_anchor_pair_schedule_frame pair_schedule;

            ret = uwb_decode_anchor_pair_schedule(frame, frame_len, &pair_schedule);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor pair-survey schedule decode failed: %d", ret);
                return true;
            }
            if (!app_anchor_radio_pair_schedule_matches_epoch(
                    &pair_schedule, &anchor_uwb_session.epoch)) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected pair-survey schedule identity: survey=%u",
                        pair_schedule.survey_id);
                return true;
            }
            stage1_led_result(STAGE1_LED_RESULT_OK);
            if (retained_sleep_us != NULL) {
                *retained_sleep_us = u32_saturating_add(
                    *retained_sleep_us,
                    anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms));
            } else {
                (void)anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms);
            }
            return true;
        }
#endif

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB RANGE_SCHEDULE decode failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_rx);
        high_debug_log_event("RANGE_SCHEDULE_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u reply_delay_uus=%u nonce=0x%016llx",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             schedule.reply_delay_us,
                             (unsigned long long)schedule.nonce);

        ret = uwb_anchor_accept_range_schedule(&anchor_uwb_session,
                                               &schedule,
                                               (uint32_t)schedule_rx_ms,
                                               UWB_SCHEDULE_GUARD_MS);
        if (ret == PROTO_ERR_NOT_FOUND) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_INF("anchor not selected in UWB RANGE_SCHEDULE: clicker=0x%016llx event_seq=%u",
                    (unsigned long long)schedule.clicker_id,
                    schedule.click_event_id);
            return true;
        }
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("RANGE_SCHEDULE_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=proto_%d",
                                 (unsigned long long)schedule.clicker_id,
                                 schedule.click_event_id,
                                 schedule.attempt_index,
                                 ret);
            LOG_WRN("anchor rejected UWB RANGE_SCHEDULE: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_accepted);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("RANGE_SCHEDULE_ACCEPT",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u burst_id=%u",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             uwb_schedule_burst_id(schedule.click_event_id,
                                                   schedule.attempt_index));

        if (retained_sleep_us != NULL) {
            *retained_sleep_us = u32_saturating_add(
                *retained_sleep_us,
                anchor_run_scheduled_uwb_ranges(&schedule,
                                                schedule_rx_ms,
                                                deferred_mesh_rx_queued));
        } else {
            (void)anchor_run_scheduled_uwb_ranges(&schedule,
                                                  schedule_rx_ms,
                                                  deferred_mesh_rx_queued);
        }
    } else {
        stage1_led_result(ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        LOG_WRN("anchor UWB RANGE_SCHEDULE not received: ret=%d", ret);
    }
    return true;
}

static bool anchor_run_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms)
{
    struct app_mesh_radio_owner_lease radio_lease = {0};
    uint32_t retained_sleep_us = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    bool deferred_mesh_rx_queued = false;
    bool handled = false;
    int64_t uwb_window_start_ms = -1;
    int low_power_ret;
    int ret;
    if (DEVICE_ROLE != ROLE_ANCHOR || claim == NULL ||
        !app_mesh_c5_wake_claim_preempts_mesh(claim->flags)) {
        LOG_ERR("invalid anchor click handoff request");
        return false;
    }

    anchor_click_window_set_active(true);
    if (mesh_preempt_for_click_event() < 0) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click handoff deferred because mesh custody preemption failed");
        return false;
    }
    ret = app_mesh_radio_owner_try_claim(APP_MESH_RADIO_CLIENT_ANCHOR_CLICK, "anchor mesh click handoff", &radio_lease);
    if (ret < 0) {
        LOG_ERR("anchor click handoff could not acquire UWB radio: ret=%d", ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_GUARD_FAIL ret=%d\n", ret);
        }
        goto complete;
    }

    anchor_set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        LOG_ERR("anchor click handoff wake PHY configuration failed: ret=%d", ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_CONFIG_FAIL ret=%d\n", ret);
        }
        goto release_radio;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_START evt=%u attempt=%u rx=%lld q=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            (long long)received_at_ms,
                            link_quality);
    }
    handled = anchor_handle_uwb_claim(claim,
                                      link_quality,
                                      received_at_ms,
                                      &retained_sleep_us,
                                      &deferred_mesh_rx_queued);
    if (handled) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
    }

release_radio:
    low_power_ret = anchor_enter_low_power(
        app_radio_low_power_mode_for_connection(
            mesh_anchor_connected_radio_active()),
        "click-release");
    if (low_power_ret < 0) {
        anchor_scan_recovery_gap_requested = true;
    }
    anchor_note_uwb_awake_since(uwb_window_start_ms, retained_sleep_us);
    (void)app_mesh_radio_owner_release(&radio_lease);
    anchor_set_uwb_busy(false);

complete:
    anchor_click_window_set_active(false);
    if (deferred_mesh_rx_queued) {
        mesh_stop_role_scan();
        mesh_submit_queued_rx();
    }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    report_tx_schedule(0u);
#endif
    if (handled && anchor_scan_recovery_gap_requested &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS;
    }
    anchor_scan_recovery_gap_requested = false;
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_DONE evt=%u attempt=%u handled=%u deferred=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            handled ? 1u : 0u,
                            deferred_mesh_rx_queued ? 1u : 0u);
    }
    return handled;
}

static bool anchor_handle_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms)
{
#if DEVICE_ROLE != ROLE_ANCHOR || \
    defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    ARG_UNUSED(claim);
    ARG_UNUSED(link_quality);
    ARG_UNUSED(received_at_ms);
    return false;
#else
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || claim == NULL ||
        !app_mesh_c5_wake_claim_preempts_mesh(claim->flags)) {
        return false;
    }

    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    if (anchor_pending_click_handoff.active) {
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_ALREADY_PENDING evt=%u attempt=%u\n",
                            claim->click_event_id,
                            claim->attempt_index);
        return true;
    }
    anchor_pending_click_handoff.claim = *claim;
    anchor_pending_click_handoff.link_quality = link_quality;
    anchor_pending_click_handoff.received_at_ms = received_at_ms;
    anchor_pending_click_handoff.active = true;
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);

    (void)k_work_cancel_delayable(&anchor_uwb_scan_work);
    ret = k_work_submit_to_queue(&anchor_uwb_scan_work_q,
                                 &anchor_click_handoff_work);
    if (ret >= 0) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_QUEUED evt=%u attempt=%u ret=%d\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            ret);
        return true;
    }

    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    if (anchor_pending_click_handoff.active &&
        anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
        anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
        anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index) {
        anchor_pending_click_handoff.active = false;
    }
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);
    LOG_ERR("anchor click handoff queue admission failed: ret=%d", ret);
    return false;
#endif
}

static void anchor_click_handoff_work_handler(struct k_work *work)
{
    struct anchor_pending_click_handoff pending = {0};
    k_spinlock_key_t key;

    ARG_UNUSED(work);
    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    if (anchor_pending_click_handoff.active) {
        pending = anchor_pending_click_handoff;
        anchor_pending_click_handoff.active = false;
    }
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);

    if (!pending.active) {
        anchor_click_window_set_active(false);
        anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
        return;
    }
    (void)anchor_run_mesh_click_wake_claim(&pending.claim,
                                           pending.link_quality,
                                           pending.received_at_ms);
}

static void anchor_uwb_scan_work_handler(struct k_work *work)
{
    struct app_mesh_radio_owner_lease radio_lease = {0};
    uint8_t *frame = anchor_uwb_scan_frame;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    uint32_t retained_sleep_us = 0u;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int64_t uwb_window_start_ms = -1;
    bool preamble_detected = false;
    bool focused_logs = stage1_anchor_focused_rx_logs_enabled();
    bool log_scan_window = false;
    uint32_t scan_debug_now_ms = 0u;
    int64_t focused_spin_deadline_ms = -1;
    bool handled_claim = false;
    bool deferred_mesh_rx_queued = false;
    bool route_wake_handoff = false;
    size_t route_wake_frame_len = 0u;
    uint8_t route_wake_quality = 0u;
    bool route_waiting_active = false;
    bool relay_tx_active = false;
    bool mesh_rx_active = false;
    bool ch9_rx_conflict = false;
    bool uwb_radio_busy = false;
    uint32_t ch9_retry_ms = 0u;
    uint32_t mesh_rx_queue_depth = 0u;
    bool mesh_rx_response_busy = false;
    uint32_t blocked_retry_ms = app_anchor_radio_blocked_retry_ms(
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST), ANCHOR_UWB_SCAN_BUSY_RETRY_MS,
        anchor_uwb_scan_interval_ms);
    uint32_t scan_rx_start_cycles = 0u;
    uint32_t scan_rx_elapsed_us = ANCHOR_UWB_SCAN_RX_US;
    uint32_t scan_extra_awake_us = 0u;
    int low_power_ret;
    int ret;
    ARG_UNUSED(work);
    app_watchdog_note_radio_progress();

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_HANDLER role=%u\n", DEVICE_ROLE);
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_rx_queue_depth = mesh_rx_pending_count();
        mesh_rx_response_busy = mesh_rx_response_active();
        mesh_rx_active = mesh_rx_response_busy;
    }
    ch9_rx_conflict = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                      mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms);
    uwb_radio_busy = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                     app_mesh_radio_owner_busy();
    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_discovery_is_pending() ||
        relay_tx_active ||
        route_waiting_active ||
        mesh_rx_active ||
        ch9_rx_conflict ||
        uwb_radio_busy) {
        uint32_t retry_ms = mesh_rx_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            uwb_radio_busy ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_rx_conflict ? ch9_retry_ms : blocked_retry_ms;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_BLOCKED win=%u survey=%u tx=%u wait=%u rx=%u rxq=%u rxr=%u ch9=%u rf=%u retry=%u interval=%u\n",
                                anchor_uwb_window_active() ? 1u : 0u,
                                app_anchor_survey_runtime_discovery_is_pending() ? 1u : 0u,
                                relay_tx_active ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                mesh_rx_active ? 1u : 0u,
                                mesh_rx_queue_depth,
                                mesh_rx_response_busy ? 1u : 0u,
                                ch9_rx_conflict ? 1u : 0u,
                                uwb_radio_busy ? 1u : 0u,
                                retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(retry_ms);
        return;
    }

    ret = app_mesh_radio_owner_try_claim(APP_MESH_RADIO_CLIENT_ANCHOR_SCAN, "anchor low-duty UWB wake scan", &radio_lease);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret == -EBUSY) {
            blocked_retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_GUARD_FAIL ret=%d retry=%u interval=%u\n",
                                ret,
                                blocked_retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(blocked_retry_ms);
        return;
    }
    anchor_set_uwb_busy(true);
    anchor_click_window_set_active(false);
    uwb_window_start_ms = k_uptime_get();
    if (focused_logs) {
        focused_spin_deadline_ms = uwb_window_start_ms + ANCHOR_STAGE1_FOCUSED_RX_SPIN_MS;
    }

focused_scan_attempt:
    frame_len = 0u;
    quality = 0u;
    retained_sleep_us = 0u;
    rx_failure = DWM3000_RX_FAILURE_NONE;
    preamble_detected = false;
    next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    stage1_led_phase(STAGE1_LED_PHASE_SCAN);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!focused_logs) {
        high_debug_log_event("UWB_RX_START",
                             "mode=anchor_wake_scan interval_ms=%u rx_window_ms=%u",
                             anchor_uwb_scan_interval_ms,
                             ANCHOR_UWB_SCAN_RX_MS);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        scan_debug_now_ms = k_uptime_get_32();
        log_scan_window = anchor_ch5_scan_debug_next_ms == 0u ||
                          uptime_deadline_reached(scan_debug_now_ms,
                                                  anchor_ch5_scan_debug_next_ms);
        if (log_scan_window) {
            anchor_ch5_scan_debug_next_ms =
                scan_debug_now_ms + ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS;
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_ARM now=%u win=%u complete=%u interval=%u wait=%u tx=%u\n",
                                scan_debug_now_ms,
                                ANCHOR_UWB_SCAN_RX_MS,
                                ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
                                anchor_uwb_scan_interval_ms,
                                mesh_route_waiting_tx_active() ? 1u : 0u,
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u);
        }
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret != 0) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_CONFIG_FAIL ret=%d\n", ret);
    }
    if (ret == 0) {
        scan_rx_start_cycles = k_cycle_get_32();
        ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
            ANCHOR_UWB_SCAN_RX_MS,
            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len,
            &quality,
            NULL,
            &rx_failure);
        scan_rx_elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - scan_rx_start_cycles));
        if (scan_rx_elapsed_us < ANCHOR_UWB_SCAN_RX_US) {
            scan_rx_elapsed_us = ANCHOR_UWB_SCAN_RX_US;
        }
    }
    preamble_detected = ret == 0 ||
                        app_anchor_rx_failure_detected_preamble(rx_failure);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (log_scan_window || ret == 0 || ret != -ETIMEDOUT ||
         preamble_detected)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_DONE ret=%d len=%u fail=%u pre=%u q=%u rx_us=%u complete=%u\n",
                            ret,
                            (unsigned int)frame_len,
                            (unsigned int)rx_failure,
                            preamble_detected ? 1u : 0u,
                            quality,
                            scan_rx_elapsed_us,
                            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS);
    }
    uwb_anchor_note_idle_scan(&anchor_uwb_session,
                              ANCHOR_UWB_STARTUP_US,
                              ANCHOR_UWB_PLL_US,
                              scan_rx_elapsed_us,
                              preamble_detected);

    if (ret == 0) {
        struct uwb_wake_claim_frame claim;
        int decode_ret;

        stage1_led_result(STAGE1_LED_RESULT_OK);
        status_debug_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
        high_debug_log_event("UWB_RX_DONE",
                             "mode=anchor_wake_scan frame_len=%u quality=%u rx_failure=%s",
                             (unsigned int)frame_len,
                             quality,
                             app_anchor_rx_failure_name(rx_failure));
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_FRAME len=%u q=%u wake_ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
        }
        stage1_anchor_focused_note_rx_frame(frame_len, quality, decode_ret);
        if (decode_ret == PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                !app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags,
                                                                 true)) {
                if (!app_mesh_c5_route_wake_claim_allowed(
                        claim.clicker_id,
                        GATEWAY_ID,
                        claim.flags,
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ),
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL))) {
                    status_debug_printf("DBG_ANCHOR_DIRECT_GATEWAY_WAKE_IGNORED src=0x%llx evt=%u flags=0x%02x\n",
                                        (unsigned long long)claim.clicker_id,
                                        claim.click_event_id,
                                        claim.flags);
                    goto scan_complete;
                }
                route_wake_handoff = true;
                route_wake_frame_len = frame_len;
                route_wake_quality = quality;
                status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DISPATCH src=0x%llx evt=%u attempt=%u flags=0x%02x len=%u q=%u\n",
                                    (unsigned long long)claim.clicker_id,
                                    claim.click_event_id,
                                    claim.attempt_index,
                                    claim.flags,
                                    (unsigned int)frame_len,
                                    quality);
                goto scan_complete;
            }
            stage1_anchor_focused_note_wake_claim(&claim);
            if (anchor_handle_uwb_claim(&claim,
                                        quality,
                                        k_uptime_get(),
                                        &retained_sleep_us,
                                        &deferred_mesh_rx_queued)) {
                handled_claim = true;
                uwb_anchor_abort_epoch(&anchor_uwb_session);
            }
        } else {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued) {
                    deferred_mesh_rx_queued = true;
                }
                goto scan_complete;
            }
            {
                enum uwb_wake_decode_failure failure =
                    app_anchor_wake_failure_from_proto_ret(decode_ret);

                uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                    failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
                next_scan_delay_ms = 0u;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor high-duty UWB wake ignored bad frame: wake_decode_ret=%d reason=%s frame_len=%u quality=%u retry_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        app_anchor_wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#else
                uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
                next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor false UWB wake cooldown: wake_decode_ret=%d reason=%s frame_len=%u quality=%u cooldown_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        app_anchor_wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#endif
            }
        }
    } else if (ret != -ETIMEDOUT) {
        enum uwb_wake_decode_failure failure =
            app_anchor_wake_failure_from_rx(rx_failure);

        uwb_anchor_note_wake_decode_failure(&anchor_uwb_session, failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
        next_scan_delay_ms = 0u;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        if (!focused_logs) {
            LOG_WRN("anchor high-duty UWB wake scan retry after activity: ret=%d rx_failure=%s reason=%s retry_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                    ret,
                    app_anchor_rx_failure_name(rx_failure),
                    app_anchor_wake_decode_failure_name(failure),
                    next_scan_delay_ms,
                    anchor_uwb_session.diagnostics.preambles,
                    anchor_uwb_session.diagnostics.sfd_timeouts,
                    anchor_uwb_session.diagnostics.frame_timeouts,
                    anchor_uwb_session.diagnostics.crc_failures);
        }
#else
        uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
        next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_ACTIVITY_COOLDOWN ret=%d fail=%u reason=%s rx_us=%u cooldown=%u\n",
                                ret,
                                (unsigned int)rx_failure,
                                app_anchor_wake_decode_failure_name(failure),
                                scan_rx_elapsed_us,
                                next_scan_delay_ms);
        }
        LOG_WRN("anchor UWB wake scan failure cooldown: ret=%d rx_failure=%s reason=%s cooldown_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                ret,
                app_anchor_rx_failure_name(rx_failure),
                app_anchor_wake_decode_failure_name(failure),
                next_scan_delay_ms,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures);
#endif
    }
    if (ret == -ETIMEDOUT) {
        stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
        if (!focused_logs) {
            high_debug_log_event("UWB_RX_TIMEOUT",
                                 "mode=anchor_wake_scan rx_failure=%s preamble=%u",
                                 app_anchor_rx_failure_name(rx_failure),
                                 preamble_detected ? 1u : 0u);
        }
    }

    if (focused_logs &&
        ret != 0 &&
        ret != -ECANCELED &&
        focused_spin_deadline_ms > 0 &&
        k_uptime_get() < focused_spin_deadline_ms &&
        !anchor_uwb_window_active() &&
        !app_anchor_survey_runtime_discovery_is_pending() &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        goto focused_scan_attempt;
    }

scan_complete:
    if (scan_rx_elapsed_us > ANCHOR_UWB_SCAN_RX_US) {
        scan_extra_awake_us = scan_rx_elapsed_us - ANCHOR_UWB_SCAN_RX_US;
    }
    low_power_ret = anchor_enter_low_power(
        app_radio_low_power_mode_for_connection(
            mesh_anchor_connected_radio_active()),
        "low-duty-scan-release");
    if (low_power_ret < 0 && next_scan_delay_ms < REPORT_TX_RETRY_DELAY_MS) {
        next_scan_delay_ms = REPORT_TX_RETRY_DELAY_MS;
    }
    anchor_note_uwb_awake_since(
        uwb_window_start_ms,
        u32_saturating_add(
            u32_saturating_add(ANCHOR_UWB_IDLE_SCAN_AWAKE_US, scan_extra_awake_us),
            retained_sleep_us));
    (void)app_mesh_radio_owner_release(&radio_lease);
    anchor_set_uwb_busy(false);
    anchor_click_window_set_active(false);
    if (route_wake_handoff) {
        bool handed_off = mesh_anchor_handoff_route_wake_frame(
            frame,
            route_wake_frame_len,
            route_wake_quality);

        status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DISPATCH_DONE handled=%u len=%u q=%u\n",
                            handed_off ? 1u : 0u,
                            (unsigned int)route_wake_frame_len,
                            route_wake_quality);
        if (!handed_off) {
            LOG_ERR("route-class wake claim handoff failed after anchor scan released radio");
        }
        if (next_scan_delay_ms < ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS) {
            next_scan_delay_ms = ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS;
        }
    }
    if (deferred_mesh_rx_queued &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS;
    }
    if (deferred_mesh_rx_queued) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_MESH_RX_HANDOFF_DEFERRED q=%u delay=%u rf=%u\n",
                                mesh_rx_pending_count(),
                                next_scan_delay_ms,
                                app_mesh_radio_owner_busy() ? 1u : 0u);
        }
        mesh_stop_role_scan();
        mesh_submit_queued_rx();
    }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    report_tx_schedule(0u);
#endif
    if (handled_claim &&
        anchor_scan_recovery_gap_requested &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS;
    }
    anchor_scan_recovery_gap_requested = false;
    if (focused_logs) {
        stage1_anchor_focused_log_diagnostics(ret,
                                              app_anchor_rx_failure_name(rx_failure),
                                              preamble_detected,
                                              &anchor_uwb_session);
    } else {
        LOG_DBG("anchor UWB diagnostics: last_ret=%d last_rx_failure=%s last_preamble=%u scans=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u claims=%u collisions=%u wins=%u losses=%u replies=%u schedules=%u ds_ok=%u ds_fail=%u timing_rejections=%u mesh_packets=%u sample_order=%u scan_startup_us=%u scan_pll_us=%u scan_rx_us=%u awake_us=%u false_cooldowns=%u",
                ret,
                app_anchor_rx_failure_name(rx_failure),
                preamble_detected ? 1u : 0u,
                anchor_uwb_session.diagnostics.scans,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures,
                anchor_uwb_session.diagnostics.claims,
                anchor_uwb_session.diagnostics.collisions,
                anchor_uwb_session.diagnostics.arbitration_wins,
                anchor_uwb_session.diagnostics.arbitration_losses,
                anchor_uwb_session.diagnostics.discovery_replies,
                anchor_uwb_session.diagnostics.schedules,
                anchor_uwb_session.diagnostics.ds_twr_successes,
                anchor_uwb_session.diagnostics.ds_twr_failures,
                anchor_uwb_session.diagnostics.timing_rejections,
                anchor_uwb_session.diagnostics.uwb_mesh_packets,
                anchor_uwb_session.diagnostics.sample_order_count,
                anchor_uwb_session.diagnostics.scan_startup_time_us,
                anchor_uwb_session.diagnostics.scan_pll_time_us,
                anchor_uwb_session.diagnostics.scan_rx_time_us,
                anchor_uwb_session.diagnostics.awake_time_us,
                anchor_uwb_session.diagnostics.false_wake_cooldowns);
        high_debug_log_event("UWB_SLEEP",
                             "mode=anchor_wake_scan next_delay_ms=%u retained_sleep_us=%u",
                             next_scan_delay_ms,
                             retained_sleep_us);
    }
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
}

static int anchor_start_uwb_scan(void)
{
    uint32_t startup_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ? 2000u : 0u;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        LOG_INF("mesh-test source transmitter skipping anchor low-duty UWB scan");
        status_debug_note("DBG_ANCHOR_SCAN_SKIPPED_TX\n");
        return 0;
    }

    anchor_uwb_scan_schedule_ms(startup_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_SCHEDULED startup=%u interval=%u win=%u\n",
                            startup_delay_ms,
                            anchor_uwb_scan_interval_ms,
                            ANCHOR_UWB_SCAN_RX_MS);
    }
    LOG_INF("anchor low-duty UWB wake scan scheduled: startup_delay_ms=%u interval_ms=%u rx_window_ms=%u hash_slots=%u",
            startup_delay_ms,
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            UWB_DISCOVERY_SLOT_COUNT);
    return 0;
}


static const struct app_mesh_report_callbacks anchor_mesh_report_callbacks = {
    .anchor_survey_discovery_is_pending =
        app_anchor_survey_runtime_discovery_is_pending,
    .anchor_note_uwb_awake_since = anchor_note_uwb_awake_since,
    .anchor_handle_click_wake_claim = anchor_handle_mesh_click_wake_claim,
    .anchor_handle_local_command = anchor_handle_local_command,
    .anchor_handle_survey_discovery_start =
        app_anchor_survey_discovery_handle_start,
    .anchor_handle_survey_pair_prepare =
        app_anchor_survey_runtime_handle_pair_prepare,
    .gateway_handle_survey_discovery_report = gateway_handle_survey_discovery_report,
    .anchor_survey_delivery_gateway_confirmed =
        app_anchor_survey_delivery_gateway_confirmed,
    .anchor_survey_delivery_transport_released =
        app_anchor_survey_delivery_transport_released,
    .gateway_route_refresh_event = gateway_route_refresh_observe,
};

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void)
{
    return &anchor_mesh_report_callbacks;
}
