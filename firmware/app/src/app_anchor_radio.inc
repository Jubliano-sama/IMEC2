/*
 * A live report reservation may follow a better click only before discovery,
 * while it still owns capacity but no report bytes.  Same-epoch copies remain
 * admissible and a different claim must actually outrank the reserved one.
 */
static bool anchor_claim_collection_candidate_allowed(
    const struct uwb_anchor_epoch *epoch,
    const struct uwb_wake_claim_frame *claim,
    bool admitted_handoff_identity_frozen,
    bool *same_epoch)
{
    bool matches;

    if (epoch == NULL || claim == NULL) {
        if (same_epoch != NULL) {
            *same_epoch = false;
        }
        return false;
    }

    matches = uwb_anchor_epoch_matches(epoch,
                                       claim->network_id,
                                       claim->clicker_id,
                                       claim->click_event_id,
                                       claim->attempt_index,
                                       claim->nonce);
    if (same_epoch != NULL) {
        *same_epoch = matches;
    }
    return !admitted_handoff_identity_frozen || matches ||
           uwb_claim_precedence_compare(
               claim->attempt_index,
               claim->priority_id,
               claim->clicker_id,
               claim->click_event_id,
               epoch->attempt_index,
               epoch->priority_id,
               epoch->clicker_id,
               epoch->click_event_id) > 0;
}

#if !defined(APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY)
#include "app_anchor_click_event_runtime.h"
#include "app_node_comm.h"

#line 6569 __BASE_FILE__

int anchor_uwb_scan_schedule_ms(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    int ret;

    if (!radio_guard_uwb_rearm_allowed()) {
        ret = radio_guard_uwb_poison_error();
        LOG_ERR("anchor channel-5 scan rearm suppressed while radio parking is poisoned: %d",
                ret);
        return ret < 0 ? ret : -EIO;
    }
    ret = k_work_reschedule_for_queue(&anchor_uwb_scan_work_q,
                                          &anchor_uwb_scan_work,
                                          K_MSEC(delay_ms));

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_RESCHED ret=%d delay=%u\n",
                            ret,
                            delay_ms);
    }
    if (ret < 0) {
        LOG_ERR("anchor channel-5 scan scheduling failed closed: ret=%d delay_ms=%u",
                ret,
                delay_ms);
        app_watchdog_stop_feeding();
    }
    return ret;
#else
    ARG_UNUSED(delay_ms);
    return 0;
#endif
}

static uint32_t anchor_uwb_scan_blocked_retry_ms(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return ANCHOR_UWB_SCAN_BUSY_RETRY_MS;
    }
    return anchor_uwb_scan_interval_ms;
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
    if (!report->have_click_timestamp_ms && result->click_timestamp_present) {
        report->click_timestamp_ms = result->click_timestamp_ms;
        report->have_click_timestamp_ms = true;
    }

    if (!report->have_result || result->status == RANGE_OK) {
        report->result = *result;
        report->have_result = true;
    }
    if (report->have_exchange_start_ms) {
        report->result.exchange_start_ms = report->first_exchange_start_ms;
        report->result.exchange_started = true;
    }
    if (report->have_click_timestamp_ms) {
        report->result.click_timestamp_ms = report->click_timestamp_ms;
        report->result.click_timestamp_present = true;
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

/*
 * The event runtime observes this worker's already-held radio lease.  It is
 * intentionally kept separate from the guard's claim/release transaction:
 * aborting a protocol phase must never release the physical Channel-5 owner
 * here.
 */
static void anchor_click_event_abort_if_needed(const char *reason)
{
    int ret;
    bool result_owned;

    if (!app_anchor_click_event_runtime_active()) {
        return;
    }
    result_owned = app_anchor_click_event_runtime_result_owned();
    ret = app_anchor_click_event_runtime_abort(k_uptime_get_32(), NULL);
    if (ret < 0) {
        LOG_ERR("anchor click event abort failed: reason=%s ret=%d",
                reason != NULL ? reason : "unknown",
                ret);
    }
    if (!result_owned) {
        /* This clears only the protocol epoch; the radio lease is external. */
        uwb_anchor_abort_epoch(&anchor_uwb_session);
    }
}

static bool anchor_click_event_handle(enum fw_event_type type,
                                      const char *reason)
{
    int ret = app_anchor_click_event_runtime_handle(type,
                                                     k_uptime_get_32(),
                                                     NULL);

    if (ret < 0) {
        LOG_ERR("anchor click event transition failed: event=%u reason=%s ret=%d",
                type,
                reason != NULL ? reason : "unknown",
                ret);
        return false;
    }
    return true;
}

static bool anchor_click_event_authorize_discovery_reply(const char *reason)
{
    struct fw_transition transition;
    int ret = app_anchor_click_event_runtime_handle(
        FW_EVENT_DISCOVER_RECEIVED, k_uptime_get_32(), &transition);

    if (ret < 0) {
        LOG_ERR("anchor click discovery transition failed: reason=%s ret=%d",
                reason != NULL ? reason : "unknown", ret);
        return false;
    }
    if (transition.effect.type != FW_EFFECT_ANCHOR_SEND_DISCOVERY_REPLY) {
        LOG_ERR("anchor click discovery transition has wrong effect: reason=%s effect=%u",
                reason != NULL ? reason : "unknown",
                transition.effect.type);
        return false;
    }
    return true;
}

static bool anchor_click_event_authorize_range(const char *reason)
{
    struct fw_transition transition;
    int ret = app_anchor_click_event_runtime_handle(
        FW_EVENT_RANGE_DUE, k_uptime_get_32(), &transition);

    if (ret < 0) {
        LOG_ERR("anchor click range transition failed: reason=%s ret=%d",
                reason != NULL ? reason : "unknown", ret);
        return false;
    }
    if (transition.effect.type != FW_EFFECT_ANCHOR_RANGE) {
        LOG_ERR("anchor click range transition has wrong effect: reason=%s effect=%u",
                reason != NULL ? reason : "unknown",
                transition.effect.type);
        return false;
    }
    return true;
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
        if (ret == -ETIMEDOUT) {
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
            status_debug_anchor_wake_claim_rx_pulse();
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
                                                bool *deferred_mesh_rx_queued,
                                                int *report_status)
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

    if (report_status != NULL) {
        *report_status = 0;
    }
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
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            LOG_WRN("anchor scheduled UWB sample did not start: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret);
        } else if (ret == 0 && range_result.status == RANGE_OK) {
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, RANGE_OK);
            anchor_range_window_record(&window_report, &range_result);
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
        } else {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            if (range_result.status == RANGE_TIMING_INVALID) {
            }
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            if (range_result.initiator_id != 0u && range_result.responder_id != 0u) {
                anchor_range_window_record(&window_report, &range_result);
            }
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
    {
        int report_ret =
            build_uwb_schedule_report_if_relevant(&anchor_uwb_session,
                                                  schedule,
                                                  schedule->flags,
                                                  &window_report);

        if (report_status != NULL) {
            *report_status = report_ret;
        }
        if (report_ret < 0) {
            LOG_ERR("anchor accepted range could not obtain retained report ownership: event=%u ret=%d",
                    schedule->click_event_id,
                    report_ret);
        } else if ((schedule->flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) != 0u &&
                   window_report.have_result &&
                   app_anchor_click_event_runtime_active() &&
                   !anchor_click_event_handle(FW_EVENT_RESULT_RETAINED,
                                              "range-result-retained")) {
            LOG_ERR("anchor range result was queued but event custody transition failed: event=%u",
                    schedule->click_event_id);
            if (report_status != NULL) {
                *report_status = -EPROTO;
            }
        }
    }
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_exit_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    return retained_sleep_us;
}

static int anchor_consider_active_click_claim(
    struct uwb_wake_claim_frame *selected_claim,
    int64_t *selected_rx_ms,
    uint8_t *selected_quality,
    const struct uwb_wake_claim_frame *claim,
    int64_t claim_rx_ms,
    uint8_t claim_quality,
    bool report_reservation_live,
    enum uwb_anchor_claim_decision *decision,
    bool *selection_changed)
{
    struct uwb_anchor_session candidate_session;
    bool same_epoch;
    int ret;

    if (selected_claim == NULL || selected_rx_ms == NULL ||
        selected_quality == NULL || claim == NULL || decision == NULL ||
        selection_changed == NULL) {
        return -EINVAL;
    }
    *selection_changed = false;
    if (!anchor_claim_collection_candidate_allowed(
            &anchor_uwb_session.epoch,
            claim,
            report_reservation_live,
            &same_epoch)) {
        return PROTO_ERR_BUSY;
    }

    candidate_session = anchor_uwb_session;
    ret = uwb_anchor_accept_wake_claim(&candidate_session,
                                       claim,
                                       (uint32_t)claim_rx_ms,
                                       decision);
    if (ret != PROTO_OK) {
        anchor_uwb_session = candidate_session;
        return ret;
    }
    if (same_epoch) {
        anchor_uwb_session = candidate_session;
        if (claim_quality > *selected_quality) {
            *selected_quality = claim_quality;
        }
        return PROTO_OK;
    }

    if (report_reservation_live) {
        ret = mesh_range_report_batch_rebind(
            selected_claim->clicker_id,
            selected_claim->click_event_id,
            selected_claim->attempt_index,
            claim->clicker_id,
            claim->click_event_id,
            claim->attempt_index);
        if (ret < 0) {
            LOG_ERR("anchor retained current click because empty report reservation could not follow priority claim: ret=%d",
                    ret);
            return ret;
        }
    }

    ret = app_anchor_click_event_runtime_claim(claim,
                                               (uint32_t)claim_rx_ms,
                                               NULL);
    if (ret < 0) {
        if (report_reservation_live) {
            mesh_range_report_batch_abort(claim->clicker_id,
                                          claim->click_event_id,
                                          claim->attempt_index);
        }
        anchor_click_event_abort_if_needed("priority-claim-phase");
        uwb_anchor_abort_epoch(&anchor_uwb_session);
        return -ECANCELED;
    }

    anchor_uwb_session = candidate_session;
    *selected_claim = *claim;
    *selected_rx_ms = claim_rx_ms;
    *selected_quality = claim_quality;
    *selection_changed = true;
    return PROTO_OK;
}

static bool anchor_handle_uwb_claim(const struct uwb_wake_claim_frame *first_claim,
                                    uint8_t first_quality,
                                    int64_t first_rx_ms,
                                    bool admitted_handoff_identity_frozen,
                                    uint32_t *retained_sleep_us,
                                    bool *deferred_mesh_rx_queued)
{
    struct uwb_wake_claim_frame selected_claim;
    int64_t selected_rx_ms;
    int64_t collect_deadline_ms;
    uint8_t selected_quality = first_quality;
    /*
     * The scan workqueue exclusively owns this buffer for the complete
     * receive/claim turn.  Reuse it after the first WAKE_CLAIM is decoded;
     * retaining a second 1 KiB frame here needlessly consumed the measured
     * anchor scan stack during the nested ranging/report path.
     */
    uint8_t *const frame = anchor_uwb_scan_frame;
    size_t frame_len = 0u;
    uint32_t range_sleep_us;
    int range_report_ret = 0;
    int ret;
    int last_discovery_ret = -ETIMEDOUT;
    int last_discovery_decode_ret = PROTO_OK;
    int64_t discovery_start_ms;
    int64_t discovery_deadline_ms;
    int64_t claim_arbitration_deadline_ms;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    enum uwb_anchor_claim_decision selected_decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    bool discover_received = false;
    bool schedule_frame_ready = false;
    bool claim_handled = false;
    int64_t schedule_rx_ms = -1;
    struct uwb_discover_frame discover = {0};
    bool discovery_uses_extended_phr;
    bool click_priority = false;
    uint32_t discovery_late_guard_ms;
    uint32_t preempt_deadline_ms;
    uint32_t trace_preempt_start_ms = 0u;
    uint32_t trace_preempt_end_ms = 0u;
    int trace_preempt_ret = 0;
    int64_t trace_schedule_arm_ms = -1;
    int64_t trace_schedule_deadline_ms = -1;
    int trace_schedule_ret = -EINPROGRESS;
    int trace_schedule_decode_ret = -EINPROGRESS;
    int trace_schedule_accept_ret = -EINPROGRESS;
    uint16_t trace_schedule_frames = 0u;
    uint16_t trace_schedule_errors = 0u;
    uint8_t trace_schedule_last_type = 0u;
    enum dwm3000_rx_failure trace_schedule_last_failure =
        DWM3000_RX_FAILURE_NONE;
    bool trace_range_authorized = false;

    if (first_claim == NULL) {
        return false;
    }
    if (app_survey_anchor_active()) {
        /* A survey owns the radio against its absolute plan.  Leaving the
         * claim unanswered makes the clicker use its existing retry path. */
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
    claim_arbitration_deadline_ms =
        first_rx_ms + UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS;
    ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                       first_claim,
                                       (uint32_t)first_rx_ms,
                                       &decision);
    if (ret != PROTO_OK) {
        LOG_DBG("anchor rejected UWB WAKE_CLAIM: clicker=0x%016llx ret=%d decision=%u",
                (unsigned long long)first_claim->clicker_id,
                ret,
                decision);
        return false;
    }
    ret = app_anchor_click_event_runtime_claim(first_claim,
                                               k_uptime_get_32(),
                                               NULL);
    if (ret < 0) {
        LOG_ERR("anchor accepted UWB WAKE_CLAIM but event phase could not start: ret=%d",
                ret);
        uwb_anchor_abort_epoch(&anchor_uwb_session);
        return false;
    }
    selected_decision = decision;
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
        int64_t claim_rx_ms;
        bool selection_changed;

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(anchor_uwb_scan_frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
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
        status_debug_anchor_wake_claim_rx_pulse();

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

        claim_rx_ms = k_uptime_get();
        ret = anchor_consider_active_click_claim(
            &selected_claim,
            &selected_rx_ms,
            &selected_quality,
            &claim,
            claim_rx_ms,
            quality,
            admitted_handoff_identity_frozen,
            &decision,
            &selection_changed);
        LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
                (unsigned long long)claim.clicker_id,
                claim.click_event_id,
                claim.attempt_index,
                (unsigned long long)claim.priority_id,
                ret,
                decision);
        if (ret == -ECANCELED) {
            return false;
        }
        if (ret == PROTO_OK) {
            selected_decision = decision;
        }
    }

    LOG_INF("anchor selected UWB claim: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx decision=%u",
            (unsigned long long)anchor_uwb_session.epoch.clicker_id,
            anchor_uwb_session.epoch.click_event_id,
            anchor_uwb_session.epoch.attempt_index,
            (unsigned long long)anchor_uwb_session.epoch.priority_id,
            selected_decision);
    /* Pass the physical discovery start itself; mesh preemption owns the
     * one guard subtraction so every caller shares the same cutoff. */
    discovery_start_ms =
        (int64_t)selected_rx_ms + selected_claim.discovery_starts_in_ms;
    preempt_deadline_ms = (uint32_t)discovery_start_ms;
    click_priority = app_mesh_c5_wake_claim_preempts_mesh(selected_claim.flags);
    anchor_click_window_set_active(click_priority);
    if (click_priority && !admitted_handoff_identity_frozen) {
        trace_preempt_start_ms = k_uptime_get_32();
        trace_preempt_ret =
            mesh_preempt_for_click_event_until(preempt_deadline_ms);
        trace_preempt_end_ms = k_uptime_get_32();
        if (trace_preempt_ret < 0) {
            anchor_click_window_set_active(false);
            LOG_ERR("anchor click claim deferred because mesh custody preemption failed");
            anchor_click_event_abort_if_needed("mesh-preemption");
            if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
                status_debug_printf("DBG_CH_AP2 e=%08x a=%02x d=%08x r=%08x w=%01x\n",
                                    selected_claim.click_event_id,
                                    selected_claim.attempt_index,
                                    preempt_deadline_ms,
                                    (uint32_t)trace_preempt_ret,
                                    anchor_click_window_active() ? 1u : 0u);
            }
            return false;
        }
    } else if (click_priority) {
        /*
         * mesh_handoff_anchor_click_claim() completed the deadline-bound
         * custody preemption before this queued worker was admitted.  The
         * handoff callback also reserved this exact click's report batch.
         * Repeating either admission after workqueue/radio setup can reject
         * an already-owned click merely because its original bridge cutoff
         * has passed.
         */
        trace_preempt_start_ms = k_uptime_get_32();
        trace_preempt_end_ms = trace_preempt_start_ms;
        trace_preempt_ret = 0;
        if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
            status_debug_printf(
                "DBG_CH_AP_ADMITTED e=%08x a=%02x now=%08x d=%08x\n",
                selected_claim.click_event_id,
                selected_claim.attempt_index,
                trace_preempt_start_ms,
                preempt_deadline_ms);
        }
    }
    if (click_priority && !admitted_handoff_identity_frozen) {
        ret = mesh_range_report_batch_reserve_capacity(
            selected_claim.clicker_id,
            selected_claim.click_event_id,
            selected_claim.attempt_index,
            ANCHOR_CLICK_RANGE_REPORT_FRAGMENT_CAPACITY);
        if (ret < 0) {
            status_debug_printf("DBG_ANCHOR_CLICK_REPORT_CAPACITY_BUSY evt=%u attempt=%u ret=%d release=0\n",
                                selected_claim.click_event_id,
                                selected_claim.attempt_index,
                                ret);
            anchor_click_window_set_active(false);
            anchor_click_event_abort_if_needed("click-report-capacity");
            return false;
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_WINDOW active=%u flags=0x%02x\n",
                            anchor_click_window_active() ? 1u : 0u,
                            selected_claim.flags);
    }

    discovery_uses_extended_phr =
        app_mesh_c5_wake_followup_uses_extended_phr(selected_claim.flags);
    discovery_late_guard_ms = discovery_uses_extended_phr ?
                              MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS :
                              UWB_CLICK_DISCOVERY_RX_LATE_GUARD_MS;
    discovery_deadline_ms = MIN(discovery_start_ms + discovery_late_guard_ms,
                                claim_arbitration_deadline_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_WAIT now=%u start=%lld deadline=%lld guard=%u listen=%u phr=%s\n",
                            k_uptime_get_32(),
                            (long long)discovery_start_ms,
                            (long long)discovery_deadline_ms,
                            UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_late_guard_ms + UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_uses_extended_phr ? "ext" : "std");
    }


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
        goto discovery_miss;
    }

    while (k_uptime_get() < discovery_deadline_ms) {
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint8_t quality = 0u;
        int64_t remaining_ms = discovery_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(anchor_uwb_scan_frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
        if (ret == -ETIMEDOUT) {
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
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_DECODE evt=%u attempt=%u slots=%u flags=0x%02x\n",
                                    discover.click_event_id,
                                    discover.attempt_index,
                                    discover.discovery_slot_count,
                                    discover.flags);
            }
            selected_quality = quality;
            discover_received = true;
            break;
        }

        if (frame_len == UWB_WAKE_CLAIM_LEN) {
            struct uwb_wake_claim_frame late_claim;

            ret = uwb_decode_wake_claim(frame, frame_len, &late_claim);
            if (ret == PROTO_OK) {
                int64_t claim_rx_ms = k_uptime_get();
                bool selection_changed = false;

                status_debug_anchor_wake_claim_rx_pulse();
                if (!app_mesh_c5_wake_claim_requires_anchor_handoff(
                        late_claim.flags, true)) {
                    LOG_DBG("anchor ignored non-click WAKE_CLAIM during click discovery wait: clicker=0x%016llx event_seq=%u flags=0x%02x",
                            (unsigned long long)late_claim.clicker_id,
                            late_claim.click_event_id,
                            late_claim.flags);
                    continue;
                }
                ret = anchor_consider_active_click_claim(
                    &selected_claim,
                    &selected_rx_ms,
                    &selected_quality,
                    &late_claim,
                    claim_rx_ms,
                    quality,
                    true,
                    &decision,
                    &selection_changed);
                LOG_DBG("anchor late UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u replaced=%u",
                        (unsigned long long)late_claim.clicker_id,
                        late_claim.click_event_id,
                        late_claim.attempt_index,
                        (unsigned long long)late_claim.priority_id,
                        ret,
                        decision,
                        selection_changed ? 1u : 0u);
                if (ret == -ECANCELED) {
                    return false;
                }
                if (ret == PROTO_OK) {
                    selected_decision = decision;
                }
                if (selection_changed) {
                    discovery_start_ms =
                        selected_rx_ms + selected_claim.discovery_starts_in_ms;
                    discovery_deadline_ms = MIN(
                        discovery_start_ms +
                            UWB_CLICK_DISCOVERY_RX_LATE_GUARD_MS,
                        claim_arbitration_deadline_ms);
                    LOG_INF("anchor switched pending click before discovery: clicker=0x%016llx event_seq=%u attempt=%u deadline=%lld",
                            (unsigned long long)selected_claim.clicker_id,
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            (long long)discovery_deadline_ms);
                    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                        status_debug_printf("DBG_ANCHOR_LATE_WAKE_REPLACE clicker=0x%llx evt=%u attempt=%u start=%lld deadline=%lld\n",
                                            (unsigned long long)selected_claim.clicker_id,
                                            selected_claim.click_event_id,
                                            selected_claim.attempt_index,
                                            (long long)discovery_start_ms,
                                            (long long)discovery_deadline_ms);
                    }
                }
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
                LOG_INF("anchor accepted channel-5 mesh frame after wake claim: clicker=0x%016llx event_seq=%u len=%u quality=%u valid=%u",
                        (unsigned long long)selected_claim.clicker_id,
                        selected_claim.click_event_id,
                        (unsigned int)frame_len,
                        quality,
                        valid_mesh_frame ? 1u : 0u);
                if (click_priority) {
                    continue;
                }
                anchor_click_event_abort_if_needed("mesh-frame-during-discovery");
                claim_handled = true;
                goto claim_complete;
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
        uint32_t reply_assignment_state = 0u;
#if defined(CONFIG_IMEC_ML_ANCHOR)
        uint16_t discovery_battery_mv = ml_anchor_cached_battery_mv();
#else
        uint16_t discovery_battery_mv = 0u;
#endif


        ret = uwb_anchor_build_discovery_reply(&anchor_uwb_session,
                                               &discover,
                                               selected_quality,
                                               discovery_battery_mv,
                                               &reply);
        if (ret != PROTO_OK) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY build rejected: %d", ret);
            anchor_click_event_abort_if_needed("discovery-reply-build");
            claim_handled = true;
            goto claim_complete;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (discover.discovery_slot_count > 0u) {
            reply.anchor_slot = ml_anchor_discovery_slot(discover.discovery_slot_count);
        }
#else
        enum app_discovery_assignment_provisioning_state provisioning_state =
            local_anchor_discovery_assignment_provisioning_state();

        reply_assignment_state = (uint32_t)provisioning_state;

        ret = local_anchor_discovery_slot(discover.discovery_slot_count,
                                          &reply.anchor_slot);
        if (ret != PROTO_OK) {
            enum app_discovery_assignment_provisioning_state provisioning_state =
                local_anchor_discovery_assignment_provisioning_state();

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
            anchor_click_event_abort_if_needed("discovery-slot");
            claim_handled = true;
            goto claim_complete;
        }
#endif

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_REPLY_ARM evt=%u attempt=%u slot=%u state=%u\n",
                                discover.click_event_id,
                                discover.attempt_index,
                                reply.anchor_slot,
                                (unsigned int)reply_assignment_state);
        }
        sleep_precise_us((uint32_t)reply.anchor_slot * UWB_DISCOVERY_SLOT_US);
        ret = uwb_encode_discovery_reply(&reply,
                                         frame,
                                         sizeof(anchor_uwb_scan_frame),
                                         &frame_len);
        if (ret != PROTO_OK) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY encode failed: %d", ret);
            anchor_click_event_abort_if_needed("discovery-reply-encode");
            claim_handled = true;
            goto claim_complete;
        }
        if (!anchor_click_event_authorize_discovery_reply(
                "discovery-reply-send")) {
            anchor_click_event_abort_if_needed("discovery-reply-transition");
            goto claim_complete;
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_REPLY_TX evt=%u attempt=%u slot=%u ret=%d\n",
                                discover.click_event_id,
                                discover.attempt_index,
                                reply.anchor_slot,
                                ret);
        }
        if (ret < 0) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY TX failed: %d", ret);
            anchor_click_event_abort_if_needed("discovery-reply-tx");
            claim_handled = true;
            goto claim_complete;
        }
        LOG_INF("anchor UWB DISCOVERY_REPLY sent: clicker=0x%016llx slot=%u source=%s quality=%u",
                (unsigned long long)reply.selected_clicker_id,
                reply.anchor_slot,
                ANCHOR_DISCOVERY_SLOT_SOURCE,
                reply.rx_quality);
    } else if (schedule_frame_ready) {
        LOG_INF("anchor fast range-only path: using direct RANGE_SCHEDULE without DISCOVER reply");
    } else {
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
        anchor_click_event_abort_if_needed("discovery-timeout");
        claim_handled = true;
        goto claim_complete;
    }

    if (!schedule_frame_ready) {
        uint32_t schedule_rx_window_ms =
            UWB_RANGE_SCHEDULE_RX_MS +
            discovery_window_ms_for_slots(discover.discovery_slot_count) +
            UWB_DISCOVERY_RX_GUARD_MS;
        int64_t schedule_deadline_ms = k_uptime_get() + schedule_rx_window_ms;
        int last_schedule_ret = -ETIMEDOUT;

        trace_schedule_arm_ms = k_uptime_get();
        trace_schedule_deadline_ms = schedule_deadline_ms;

        while (k_uptime_get() < schedule_deadline_ms) {
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint8_t frame_type = 0u;
            int64_t remaining_ms = schedule_deadline_ms - k_uptime_get();

            ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                          frame,
                                                          sizeof(anchor_uwb_scan_frame),
                                                          &frame_len,
                                                          NULL,
                                                          NULL,
                                                          &rx_failure);
            if (ret == -ETIMEDOUT) {
                trace_schedule_last_failure = rx_failure;
                break;
            }
            if (ret < 0) {
                last_schedule_ret = ret;
                trace_schedule_errors++;
                trace_schedule_last_failure = rx_failure;
                continue;
            }
            trace_schedule_frames++;
            if (frame_len < UWB_SYNC_HEADER_LEN) {
                last_schedule_ret = -EBADMSG;
                trace_schedule_errors++;
                continue;
            }

            frame_type = frame[2];
            trace_schedule_last_type = frame_type;
            if (frame_type != MSG_UWB_RANGE_SCHEDULE &&
                frame_type != MSG_UWB_RANGE_RELEASE) {
                last_schedule_ret = -EBADMSG;
                trace_schedule_errors++;
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
        trace_schedule_ret = ret;
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
                LOG_WRN("anchor UWB RANGE_RELEASE decode failed: %d", ret);
                anchor_click_event_abort_if_needed("range-release-decode");
                claim_handled = true;
                goto claim_complete;
            }

            ret = uwb_anchor_accept_range_release(&anchor_uwb_session, &release);
            if (ret != PROTO_OK) {
                LOG_WRN("anchor rejected UWB RANGE_RELEASE: %d", ret);
                anchor_click_event_abort_if_needed("range-release-reject");
                claim_handled = true;
                goto claim_complete;
            }

            LOG_INF("anchor UWB RANGE_RELEASE accepted: clicker=0x%016llx event_seq=%u discovered=%u min=%u reason=%u",
                    (unsigned long long)release.clicker_id,
                    release.click_event_id,
                    release.discovered_anchor_count,
                    release.min_anchor_count,
                    release.reason);
            anchor_click_event_abort_if_needed("range-release");
            claim_handled = true;
            goto claim_complete;
        }

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
        trace_schedule_decode_ret = ret;
        if (ret != PROTO_OK) {
            LOG_WRN("anchor UWB RANGE_SCHEDULE decode failed: %d", ret);
            anchor_click_event_abort_if_needed("range-schedule-decode");
            claim_handled = true;
            goto claim_complete;
        }

        ret = uwb_anchor_accept_range_schedule(&anchor_uwb_session,
                                               &schedule,
                                               (uint32_t)schedule_rx_ms,
                                               UWB_SCHEDULE_GUARD_MS);
        trace_schedule_accept_ret = ret;
        if (ret == PROTO_ERR_NOT_FOUND) {
            LOG_INF("anchor not selected in UWB RANGE_SCHEDULE: clicker=0x%016llx event_seq=%u",
                    (unsigned long long)schedule.clicker_id,
                    schedule.click_event_id);
            anchor_click_event_abort_if_needed("range-schedule-not-selected");
            claim_handled = true;
            goto claim_complete;
        }
        if (ret != PROTO_OK) {
            LOG_WRN("anchor rejected UWB RANGE_SCHEDULE: %d", ret);
            anchor_click_event_abort_if_needed("range-schedule-reject");
            claim_handled = true;
            goto claim_complete;
        }

        {
            struct fw_transition transition;

            ret = app_anchor_click_event_runtime_schedule_received(
                discover_received,
                k_uptime_get_32(),
                &transition);
            if (ret == 0 &&
                transition.effect.type != FW_EFFECT_START_TIMER) {
                LOG_ERR("anchor click schedule transition has wrong effect: effect=%u",
                        transition.effect.type);
                ret = -EPROTO;
            }
        }
        if (ret < 0) {
            LOG_ERR("anchor accepted UWB RANGE_SCHEDULE but event phase could not advance");
            anchor_click_event_abort_if_needed("range-schedule-transition");
            goto claim_complete;
        }
        if (!anchor_click_event_authorize_range("range-schedule-due")) {
            anchor_click_event_abort_if_needed("range-due-transition");
            goto claim_complete;
        }
        trace_range_authorized = true;

        range_sleep_us = anchor_run_scheduled_uwb_ranges(
            &schedule,
            schedule_rx_ms,
            deferred_mesh_rx_queued,
            &range_report_ret);
        if (retained_sleep_us != NULL) {
            *retained_sleep_us =
                u32_saturating_add(*retained_sleep_us, range_sleep_us);
        }
        if (range_report_ret < 0) {
            /*
             * The RF exchange happened, but this anchor did not complete the
             * accepted click contract because it lacks confirmed retained
             * ownership of the report. Keep the handler result truthful
             * while the queue-admission boundary forces watchdog recovery.
             */
            anchor_click_event_abort_if_needed("range-report-retention");
            goto claim_complete;
        }
    } else {
        LOG_WRN("anchor UWB RANGE_SCHEDULE not received: ret=%d", ret);
        anchor_click_event_abort_if_needed("range-schedule-timeout");
    }
    claim_handled = true;

claim_complete:
    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE) && click_priority) {
        status_debug_printf("DBG_CH_AP1 n=%016llx e=%08x a=%02x s=%08x x=%08x\n",
                            (unsigned long long)DEVICE_ID,
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            trace_preempt_start_ms,
                            trace_preempt_end_ms);
        status_debug_printf("DBG_CH_AP2 e=%08x a=%02x d=%08x r=%08x w=%01x\n",
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            preempt_deadline_ms,
                            (uint32_t)trace_preempt_ret,
                            anchor_click_window_active() ? 1u : 0u);
        status_debug_printf("DBG_CH_AS1 e=%08x a=%02x arm=%08x dl=%08x rx=%08x r=%08x\n",
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            (uint32_t)trace_schedule_arm_ms,
                            (uint32_t)trace_schedule_deadline_ms,
                            (uint32_t)schedule_rx_ms,
                            (uint32_t)trace_schedule_ret);
        status_debug_printf("DBG_CH_AS2 e=%08x a=%02x f=%04x err=%04x t=%02x fail=%02x dec=%08x acc=%08x\n",
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            trace_schedule_frames,
                            trace_schedule_errors,
                            trace_schedule_last_type,
                            (unsigned int)trace_schedule_last_failure,
                            (uint32_t)trace_schedule_decode_ret,
                            (uint32_t)trace_schedule_accept_ret);
        status_debug_printf("DBG_CH_AST e=%08x a=%02x range=%01x state=%02x w=%01x\n",
                            selected_claim.click_event_id,
                            selected_claim.attempt_index,
                            trace_range_authorized ? 1u : 0u,
                            (unsigned int)app_anchor_click_event_runtime_state(),
                            anchor_click_window_active() ? 1u : 0u);
    }
    if (click_priority) {
        mesh_range_report_batch_abort(selected_claim.clicker_id,
                                      selected_claim.click_event_id,
                                      selected_claim.attempt_index);
    }
    return claim_handled;
}
#endif /* !APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY */

/*
 * A queued click handoff has claimed the logical radio but has not touched
 * the DWM3000 yet.  Release that lease through both phases if admission
 * fails before RF starts; a stale release is an ownership violation and must
 * not leave the scheduler running as if the click had never existed.
 */
static int anchor_release_unstarted_click_lease(
    struct radio_guard_uwb_lease *radio_lease,
    const char *reason)
{
    int ret;

    if (radio_lease == NULL ||
        radio_lease->client != RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK ||
        radio_lease->generation == 0u) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_release_begin(radio_lease);
    if (ret == 0) {
        /* No DWM3000 operation has started for this handoff. */
        ret = radio_guard_uwb_release_finish(radio_lease, 0);
    }
    if (ret < 0) {
        LOG_ERR("anchor click handoff lease release failed before RF: reason=%s ret=%d",
                reason == NULL ? "unknown" : reason,
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

/* A published handoff is priority ownership even before its worker starts. */
static bool anchor_click_handoff_pending(void)
{
    k_spinlock_key_t key;
    bool pending;

    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    pending = anchor_pending_click_handoff.active;
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);
    return pending;
}

static bool anchor_run_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms,
    struct radio_guard_uwb_lease *radio_lease)
{
    uint32_t retained_sleep_us = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    bool deferred_mesh_rx_queued = false;
    bool handled = false;
    int64_t uwb_window_start_ms = -1;
    int low_power_ret;
    int release_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || claim == NULL ||
        !app_mesh_c5_wake_claim_preempts_mesh(claim->flags) ||
        radio_lease == NULL ||
        radio_lease->client != RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK ||
        radio_lease->generation == 0u) {
        LOG_ERR("invalid anchor click handoff request");
        if (radio_lease != NULL && radio_lease->generation != 0u) {
            (void)anchor_release_unstarted_click_lease(
                radio_lease, "invalid-click-handoff");
        }
        return false;
    }

    anchor_click_window_set_active(true);
    /*
     * The accepted handoff already committed click preemption and reserved
     * the report batch before it queued this worker.  Holding that exact
     * reservation through RF prevents a repair or report producer from
     * taking the capacity between handoff and physical execution.
     */

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
                                      true,
                                      &retained_sleep_us,
                                      &deferred_mesh_rx_queued);
    if (handled) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
    }

release_radio:
    release_ret = radio_guard_uwb_release_begin(radio_lease);
    if (release_ret == 0) {
        low_power_ret = anchor_enter_low_power(
            app_radio_low_power_mode_for_connection(
                mesh_anchor_connected_radio_active()),
            "click-release");
        release_ret = radio_guard_uwb_release_finish(radio_lease,
                                                      low_power_ret);
    }
    if (release_ret < 0) {
        anchor_scan_recovery_gap_requested = true;
        anchor_note_uwb_awake_since(uwb_window_start_ms, retained_sleep_us);
        LOG_ERR("anchor click handoff radio release failed; retaining poisoned owner: %d",
                release_ret);
        return false;
    }
    anchor_note_uwb_awake_since(uwb_window_start_ms, retained_sleep_us);
    anchor_set_uwb_busy(false);

    goto complete;

complete:
    /* Preserve retained report custody; abort only non-terminal phases. */
    anchor_click_event_abort_if_needed(handled ? "click-complete" :
                                       "click-aborted");
    mesh_range_report_batch_abort(claim->clicker_id,
                                  claim->click_event_id,
                                  claim->attempt_index);
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
    struct radio_guard_uwb_lease radio_lease = {0};
    k_spinlock_key_t key;
    bool pending_matches;
    int ret;
    int release_ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || claim == NULL ||
        app_survey_anchor_active() ||
        !app_mesh_c5_wake_claim_preempts_mesh(claim->flags)) {
        return false;
    }

    /*
     * Make accepted handoff identity visible before claiming the radio.  A
     * duplicate or concurrent wake cannot replace this record while its
     * ANCHOR_CLICK lease is being acquired or queued.
     */
    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    if (anchor_pending_click_handoff.active) {
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_ALREADY_PENDING evt=%u attempt=%u\n",
                            claim->click_event_id,
                            claim->attempt_index);
        /* Keep the accepted click priority and identity intact. */
        return true;
    }
    anchor_pending_click_handoff.claim = *claim;
    anchor_pending_click_handoff.link_quality = link_quality;
    anchor_pending_click_handoff.received_at_ms = received_at_ms;
    anchor_pending_click_handoff.active = true;
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);

    ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK,
                                "anchor mesh click handoff",
                                &radio_lease);
    if (ret < 0) {
        key = k_spin_lock(&anchor_pending_click_handoff_lock);
        pending_matches = anchor_pending_click_handoff.active &&
            anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
            anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
            anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index &&
            anchor_pending_click_handoff.radio_lease.generation == 0u;
        if (pending_matches) {
            memset(&anchor_pending_click_handoff, 0,
                   sizeof(anchor_pending_click_handoff));
        }
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
        LOG_ERR("anchor click handoff could not reserve UWB lease: ret=%d", ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_GUARD_FAIL ret=%d\n", ret);
        }
        return false;
    }

    ret = app_anchor_click_event_runtime_claim(claim,
                                               k_uptime_get_32(),
                                               NULL);
    if (ret < 0) {
        key = k_spin_lock(&anchor_pending_click_handoff_lock);
        pending_matches = anchor_pending_click_handoff.active &&
            anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
            anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
            anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index &&
            anchor_pending_click_handoff.radio_lease.generation == 0u;
        if (pending_matches) {
            memset(&anchor_pending_click_handoff, 0,
                   sizeof(anchor_pending_click_handoff));
        }
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
        release_ret = anchor_release_unstarted_click_lease(
            &radio_lease, "click-phase-claim");
        status_debug_printf("DBG_ANCHOR_CLICK_PHASE_CLAIM_FAIL evt=%u attempt=%u ret=%d release=%d\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            ret,
                            release_ret);
        return false;
    }

    ret = mesh_range_report_batch_reserve_capacity(
        claim->clicker_id,
        claim->click_event_id,
        claim->attempt_index,
        ANCHOR_CLICK_RANGE_REPORT_FRAGMENT_CAPACITY);
    if (ret < 0) {
        key = k_spin_lock(&anchor_pending_click_handoff_lock);
        pending_matches = anchor_pending_click_handoff.active &&
            anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
            anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
            anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index &&
            anchor_pending_click_handoff.radio_lease.generation == 0u;
        if (pending_matches) {
            memset(&anchor_pending_click_handoff, 0,
                   sizeof(anchor_pending_click_handoff));
        }
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
        anchor_click_event_abort_if_needed("click-report-capacity");
        release_ret = anchor_release_unstarted_click_lease(
            &radio_lease, "click-report-capacity");
        status_debug_printf("DBG_ANCHOR_CLICK_REPORT_CAPACITY_BUSY evt=%u attempt=%u ret=%d release=%d\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            ret,
                            release_ret);
        return false;
    }

    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    pending_matches = anchor_pending_click_handoff.active &&
        anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
        anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
        anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index &&
        anchor_pending_click_handoff.radio_lease.generation == 0u;
    if (pending_matches) {
        anchor_pending_click_handoff.radio_lease = radio_lease;
    }
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);
    if (!pending_matches) {
        mesh_range_report_batch_abort(claim->clicker_id,
                                      claim->click_event_id,
                                      claim->attempt_index);
        anchor_click_event_abort_if_needed("click-handoff-replaced");
        (void)anchor_release_unstarted_click_lease(
            &radio_lease, "click-handoff-replaced");
        return false;
    }

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
    pending_matches = anchor_pending_click_handoff.active &&
        anchor_pending_click_handoff.claim.clicker_id == claim->clicker_id &&
        anchor_pending_click_handoff.claim.click_event_id == claim->click_event_id &&
        anchor_pending_click_handoff.claim.attempt_index == claim->attempt_index &&
        anchor_pending_click_handoff.radio_lease.generation ==
            radio_lease.generation &&
        anchor_pending_click_handoff.radio_lease.client == radio_lease.client;
    if (pending_matches) {
        radio_lease = anchor_pending_click_handoff.radio_lease;
        memset(&anchor_pending_click_handoff, 0,
               sizeof(anchor_pending_click_handoff));
    }
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);
    mesh_range_report_batch_abort(claim->clicker_id,
                                  claim->click_event_id,
                                  claim->attempt_index);
    anchor_click_event_abort_if_needed("click-handoff-queue");
    release_ret = pending_matches ?
        anchor_release_unstarted_click_lease(&radio_lease,
                                             "click-handoff-queue") :
        -ESTALE;
    LOG_ERR("anchor click handoff queue admission failed: ret=%d release=%d",
            ret,
            release_ret);
    return false;
#endif
}

static void anchor_click_handoff_work_handler(struct k_work *work)
{
    struct anchor_pending_click_handoff pending = {0};
    k_spinlock_key_t key;
    uint32_t radio_generation;

    ARG_UNUSED(work);
    key = k_spin_lock(&anchor_pending_click_handoff_lock);
    if (anchor_pending_click_handoff.active) {
        pending = anchor_pending_click_handoff;
    }
    k_spin_unlock(&anchor_pending_click_handoff_lock, key);

    if (!pending.active) {
        anchor_click_window_set_active(false);
        anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
        return;
    }
    radio_generation = pending.radio_lease.generation;
    (void)anchor_run_mesh_click_wake_claim(&pending.claim,
                                           pending.link_quality,
                                           pending.received_at_ms,
                                           &pending.radio_lease);

    /*
     * Keep the handoff identity published until its exact lease has reached
     * IDLE.  A route/repair worker that arrives while this workqueue item is
     * running can then observe click priority instead of replacing it.
     */
    if (pending.radio_lease.generation == 0u) {
        key = k_spin_lock(&anchor_pending_click_handoff_lock);
        if (anchor_pending_click_handoff.active &&
            anchor_pending_click_handoff.radio_lease.client ==
                RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK &&
            anchor_pending_click_handoff.radio_lease.generation ==
                radio_generation) {
            memset(&anchor_pending_click_handoff, 0,
                   sizeof(anchor_pending_click_handoff));
        }
        k_spin_unlock(&anchor_pending_click_handoff_lock, key);
    }
}

#if !defined(APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY)
static bool anchor_relay_control_followup_boost_active_state;
static uint32_t anchor_relay_control_followup_boost_deadline_ms;
static struct protocol_rx_lifecycle anchor_enumeration_rx_lifecycle;
static uint32_t anchor_enumeration_rx_claim_session_id;
static uint32_t anchor_enumeration_rx_claim_command_seq;
static bool anchor_enumeration_rx_claim_identity_valid;
static uint32_t anchor_enumeration_rx_table_command_seq;
static struct discovery_assignment_table_commitment
    anchor_enumeration_rx_table_commitment;
static bool anchor_enumeration_rx_table_identity_valid;
static struct k_spinlock anchor_enumeration_rx_lock;
static atomic_t anchor_ch5_post_ch9_recovery_scan_pending;

static void anchor_enumeration_rx_clear_claim_identity(void)
{
    anchor_enumeration_rx_claim_session_id = 0u;
    anchor_enumeration_rx_claim_command_seq = 0u;
    anchor_enumeration_rx_claim_identity_valid = false;
}

static void anchor_enumeration_rx_clear_table_identity(void)
{
    anchor_enumeration_rx_table_command_seq = 0u;
    memset(&anchor_enumeration_rx_table_commitment,
           0,
           sizeof(anchor_enumeration_rx_table_commitment));
    anchor_enumeration_rx_table_identity_valid = false;
}

static void anchor_enumeration_rx_clear_phase_identities(void)
{
    anchor_enumeration_rx_clear_claim_identity();
    anchor_enumeration_rx_clear_table_identity();
}

static void anchor_compact_enumeration_deactivate(uint32_t epoch)
{
    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_enumeration_response_config.epoch == epoch) {
        anchor_enumeration_response_config.active = false;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
}

static int anchor_enumeration_rx_begin(uint32_t epoch,
                                       uint32_t operation_budget_ms,
                                       bool allow_supersede)
{
    enum protocol_rx_begin_result result;
    uint32_t now_ms;
    k_spinlock_key_t key;

    if (epoch == 0u || operation_budget_ms == 0u ||
        operation_budget_ms > INT32_MAX) {
        return -EINVAL;
    }
    now_ms = k_uptime_get_32();
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    result = protocol_rx_lifecycle_begin(
        &anchor_enumeration_rx_lifecycle,
        PROTOCOL_RX_OPERATION_ENUMERATION,
        epoch,
        now_ms,
        now_ms + operation_budget_ms);
    if (result == PROTOCOL_RX_BEGIN_BUSY && allow_supersede) {
        protocol_rx_lifecycle_init(&anchor_enumeration_rx_lifecycle);
        anchor_enumeration_rx_clear_phase_identities();
        result = protocol_rx_lifecycle_begin(
            &anchor_enumeration_rx_lifecycle,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            epoch,
            now_ms,
            now_ms + operation_budget_ms);
    }
    if (result == PROTOCOL_RX_BEGIN_ACCEPTED) {
        anchor_enumeration_rx_clear_phase_identities();
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (result != PROTOCOL_RX_BEGIN_ACCEPTED &&
        result != PROTOCOL_RX_BEGIN_DUPLICATE) {
        return result == PROTOCOL_RX_BEGIN_BUSY ? -EBUSY : -EINVAL;
    }
    status_debug_printf(
        "DBG_ENUM_RX_ACTIVE epoch=%u budget=%u duplicate=%u\n",
        epoch,
        operation_budget_ms,
        result == PROTOCOL_RX_BEGIN_DUPLICATE ? 1u : 0u);
    (void)anchor_uwb_scan_schedule_ms(0u);
    return 0;
}

static int anchor_enumeration_rx_begin_table(
    uint32_t epoch,
    uint32_t operation_budget_ms,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    bool allow_supersede)
{
    bool bound = false;
    int ret;
    k_spinlock_key_t key;

    if (table_command_seq == 0u || table_commitment == NULL) {
        return -EINVAL;
    }
    ret = anchor_enumeration_rx_begin(epoch,
                                      operation_budget_ms,
                                      allow_supersede);
    if (ret < 0) {
        return ret;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    if (anchor_enumeration_rx_lifecycle.operation ==
            PROTOCOL_RX_OPERATION_ENUMERATION &&
        anchor_enumeration_rx_lifecycle.generation == epoch) {
        if (!anchor_enumeration_rx_table_identity_valid) {
            anchor_enumeration_rx_table_command_seq = table_command_seq;
            anchor_enumeration_rx_table_commitment = *table_commitment;
            anchor_enumeration_rx_table_identity_valid = true;
            bound = true;
        } else {
            bound = anchor_enumeration_rx_table_command_seq ==
                        table_command_seq &&
                    discovery_assignment_table_commitment_equal(
                        &anchor_enumeration_rx_table_commitment,
                        table_commitment);
        }
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (bound) {
        /* TABLE is the semantic end of CLAIM collection. The persistent lane
         * remains scanner-owned but becomes inaccessible immediately, so an
         * unACKed response cannot interfere with TABLE/END propagation. */
        anchor_compact_enumeration_deactivate(epoch);
    }
    return bound ? 0 : -ESTALE;
}

static bool anchor_enumeration_rx_bind_claim(uint32_t epoch,
                                             uint32_t claim_session_id,
                                             uint32_t claim_command_seq)
{
    bool bound = false;
    k_spinlock_key_t key;

    if (epoch == 0u || claim_session_id == 0u || claim_command_seq == 0u) {
        return false;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    if (anchor_enumeration_rx_lifecycle.operation ==
            PROTOCOL_RX_OPERATION_ENUMERATION &&
        anchor_enumeration_rx_lifecycle.generation == epoch) {
        if (!anchor_enumeration_rx_claim_identity_valid) {
            anchor_enumeration_rx_claim_session_id = claim_session_id;
            anchor_enumeration_rx_claim_command_seq = claim_command_seq;
            anchor_enumeration_rx_claim_identity_valid = true;
            bound = true;
        } else {
            bound = anchor_enumeration_rx_claim_session_id ==
                        claim_session_id &&
                    anchor_enumeration_rx_claim_command_seq ==
                        claim_command_seq;
        }
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    return bound;
}

static bool anchor_enumeration_rx_active(void)
{
    bool expired;
    bool active;
    uint64_t expired_generation = 0u;
    k_spinlock_key_t key = k_spin_lock(&anchor_enumeration_rx_lock);

    if (anchor_enumeration_rx_lifecycle.operation ==
        PROTOCOL_RX_OPERATION_ENUMERATION) {
        expired_generation = anchor_enumeration_rx_lifecycle.generation;
    }
    expired = protocol_rx_lifecycle_expire(
        &anchor_enumeration_rx_lifecycle, k_uptime_get_32());
    if (expired) {
        anchor_enumeration_rx_clear_phase_identities();
    }
    active = anchor_enumeration_rx_lifecycle.operation ==
                 PROTOCOL_RX_OPERATION_ENUMERATION &&
             anchor_enumeration_rx_lifecycle.mode ==
                 PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5;
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (expired) {
        status_debug_printf("DBG_ENUM_RX_TERMINAL epoch=%u reason=timeout\n",
                            (uint32_t)expired_generation);
    }
    return active;
}

static bool anchor_enumeration_rx_matches_claim(
    uint32_t epoch,
    uint32_t claim_session_id,
    uint32_t claim_command_seq)
{
    bool matches;
    k_spinlock_key_t key;

    if (epoch == 0u || claim_session_id == 0u || claim_command_seq == 0u) {
        return false;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    matches = anchor_enumeration_rx_lifecycle.operation ==
                  PROTOCOL_RX_OPERATION_ENUMERATION &&
              anchor_enumeration_rx_lifecycle.generation == epoch &&
              anchor_enumeration_rx_claim_identity_valid &&
              anchor_enumeration_rx_claim_session_id == claim_session_id &&
              anchor_enumeration_rx_claim_command_seq == claim_command_seq;
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    return matches;
}

static bool anchor_enumeration_rx_matches_table(
    uint32_t epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    bool matches;
    k_spinlock_key_t key;

    if (epoch == 0u || table_command_seq == 0u || table_commitment == NULL) {
        return false;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    matches = anchor_enumeration_rx_lifecycle.operation ==
                  PROTOCOL_RX_OPERATION_ENUMERATION &&
              anchor_enumeration_rx_lifecycle.generation == epoch &&
              anchor_enumeration_rx_table_identity_valid &&
              anchor_enumeration_rx_table_command_seq == table_command_seq &&
              discovery_assignment_table_commitment_equal(
                  &anchor_enumeration_rx_table_commitment,
                  table_commitment);
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    return matches;
}

static enum protocol_rx_recovery_result anchor_enumeration_rx_note_recovery(
    bool recovered,
    const char *reason)
{
    enum protocol_rx_recovery_result result = PROTOCOL_RX_RECOVERY_INVALID;
    uint32_t epoch = 0u;
    k_spinlock_key_t key;

    key = k_spin_lock(&anchor_enumeration_rx_lock);
    if (anchor_enumeration_rx_lifecycle.operation ==
        PROTOCOL_RX_OPERATION_ENUMERATION) {
        epoch = (uint32_t)anchor_enumeration_rx_lifecycle.generation;
        result = protocol_rx_lifecycle_note_rx_recovery(
            &anchor_enumeration_rx_lifecycle,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            epoch,
            recovered);
    }
    if (result == PROTOCOL_RX_RECOVERY_TERMINATED) {
        anchor_enumeration_rx_clear_phase_identities();
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (result == PROTOCOL_RX_RECOVERY_TERMINATED) {
        status_debug_printf("DBG_ENUM_RX_TERMINAL epoch=%u reason=%s\n",
                            epoch,
                            reason == NULL ? "failure" : reason);
        (void)anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    return result;
}

static bool anchor_enumeration_rx_terminate_claim(
    uint32_t epoch,
    uint32_t claim_session_id,
    uint32_t claim_command_seq,
    const char *reason)
{
    bool terminated = false;
    k_spinlock_key_t key;

    if (epoch == 0u || claim_session_id == 0u || claim_command_seq == 0u) {
        return false;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    if (anchor_enumeration_rx_claim_identity_valid &&
        anchor_enumeration_rx_claim_session_id == claim_session_id &&
        anchor_enumeration_rx_claim_command_seq == claim_command_seq) {
        terminated = protocol_rx_lifecycle_terminate(
            &anchor_enumeration_rx_lifecycle,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            epoch);
    }
    if (terminated) {
        anchor_enumeration_rx_clear_phase_identities();
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (terminated) {
        anchor_compact_enumeration_deactivate(epoch);
        status_debug_printf("DBG_ENUM_RX_TERMINAL epoch=%u reason=%s\n",
                            epoch,
                            reason == NULL ? "claim-terminal" : reason);
        (void)anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    return terminated;
}

static bool anchor_enumeration_rx_terminate_table(
    uint32_t epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const char *reason)
{
    bool terminated = false;
    k_spinlock_key_t key;

    if (epoch == 0u || table_command_seq == 0u || table_commitment == NULL) {
        return false;
    }
    key = k_spin_lock(&anchor_enumeration_rx_lock);
    if (anchor_enumeration_rx_table_identity_valid &&
        anchor_enumeration_rx_table_command_seq == table_command_seq &&
        discovery_assignment_table_commitment_equal(
            &anchor_enumeration_rx_table_commitment,
            table_commitment)) {
        terminated = protocol_rx_lifecycle_terminate(
            &anchor_enumeration_rx_lifecycle,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            epoch);
    }
    if (terminated) {
        anchor_enumeration_rx_clear_phase_identities();
    }
    k_spin_unlock(&anchor_enumeration_rx_lock, key);
    if (terminated) {
        anchor_compact_enumeration_deactivate(epoch);
        status_debug_printf("DBG_ENUM_RX_TERMINAL epoch=%u reason=%s\n",
                            epoch,
                            reason == NULL ? "table-terminal" : reason);
        (void)anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    return terminated;
}

void app_anchor_note_channel9_window_released(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        atomic_set(&anchor_ch5_post_ch9_recovery_scan_pending, 1);
    }
#endif
}

static void anchor_relay_control_followup_boost_begin(uint32_t duration_ms)
{
    uint32_t now_ms;
    uint32_t candidate_deadline_ms;

    if (duration_ms == 0u) {
        return;
    }
    duration_ms = MIN(duration_ms, UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS);
    now_ms = k_uptime_get_32();
    candidate_deadline_ms = now_ms + duration_ms;
    if (!anchor_relay_control_followup_boost_active_state ||
        uptime_deadline_reached(now_ms,
                                anchor_relay_control_followup_boost_deadline_ms) ||
        (int32_t)(candidate_deadline_ms -
                  anchor_relay_control_followup_boost_deadline_ms) > 0) {
        anchor_relay_control_followup_boost_deadline_ms = candidate_deadline_ms;
    }
    anchor_relay_control_followup_boost_active_state = true;
}
static void anchor_operation_high_duty_boost_begin(uint32_t duration_ms)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t candidate_deadline_ms = now_ms + duration_ms;

    if (!anchor_relay_control_followup_boost_active_state ||
        uptime_deadline_reached(now_ms,
                                anchor_relay_control_followup_boost_deadline_ms) ||
        (int32_t)(candidate_deadline_ms -
                  anchor_relay_control_followup_boost_deadline_ms) > 0) {
        anchor_relay_control_followup_boost_deadline_ms = candidate_deadline_ms;
    }
    anchor_relay_control_followup_boost_active_state = true;
}

/*
 * Not static: the mesh route wake-train owner (app_mesh_report.c) queries
 * this to collapse its wake_adv window while the sender's own operation
 * decode keeps the widened Channel-5 scan latched.
 */
bool anchor_relay_control_followup_boost_active(void)
{
    if (!anchor_relay_control_followup_boost_active_state) {
        return false;
    }
    if (uptime_deadline_reached(k_uptime_get_32(),
                                anchor_relay_control_followup_boost_deadline_ms)) {
        anchor_relay_control_followup_boost_active_state = false;
        return false;
    }
    return true;
}

static void anchor_relay_control_followup_boost_cancel(void)
{
    anchor_relay_control_followup_boost_active_state = false;
}

static bool anchor_relay_retry_plan_scan(uint32_t now_ms,
                                         bool local_protocol_response_active,
                                         uint32_t *scan_rx_ms,
                                         uint32_t *retry_ms)
{
    uint32_t response_boundary_ms;
    uint32_t remaining_ms;
    uint32_t scan_guard_ms;

    if (scan_rx_ms == NULL || retry_ms == NULL ||
        !mesh_relay_tx_active(&mesh_runtime)) {
        return false;
    }

    if (mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        response_boundary_ms = mesh_runtime.pending.retry_after_ms;
    } else if (local_protocol_response_active &&
               mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
               mesh_runtime.pending.gateway_ack_deadline_ms != 0u) {
        /* A missed immediate gateway ACK keeps the exact local response in
         * WAIT_GATEWAY_ACK until its timeout owner decides the retry. That
         * owner runs on the same system workqueue as this scan, so a full
         * protocol RX slice must release before the ACK deadline. */
        response_boundary_ms =
            mesh_runtime.pending.gateway_ack_deadline_ms;
    } else {
        return false;
    }

    if (uptime_deadline_reached(now_ms, response_boundary_ms)) {
        *retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
        return true;
    }

    remaining_ms = uptime_ms_until_deadline(
        now_ms, response_boundary_ms);
    scan_guard_ms = ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +
                    MESH_RADIO_EVENT_RETUNE_GUARD_MS + 1u;
    if (remaining_ms <= scan_guard_ms) {
        *retry_ms = remaining_ms == 0u ?
            ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS : remaining_ms;
        return true;
    }

    /*
     * Stay continuously on Channel 5 until just before the higher-priority
     * response retry. Fixed short slices add a retune gap every few
     * milliseconds, which can lose the gateway's one-shot TABLE even though
     * the anchor is logically in protocol RX mode.
     */
    if (*scan_rx_ms >= remaining_ms - scan_guard_ms) {
        *scan_rx_ms = remaining_ms - scan_guard_ms;
    }
    return false;
}

static bool anchor_compact_enumeration_window(
    uint64_t now_ms,
    const struct anchor_enumeration_response_config *expected,
    struct enumeration_response_timing *timing_out)
{
    struct enumeration_response_timing timing;
    bool matches;
    bool active;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    matches = expected == NULL ||
        (anchor_enumeration_response_config.epoch == expected->epoch &&
         anchor_enumeration_response_config.start_ms == expected->start_ms &&
         anchor_enumeration_response_config.parent_id == expected->parent_id &&
         anchor_enumeration_response_config.hop_count == expected->hop_count &&
         anchor_enumeration_response_config.max_hop_count ==
             expected->max_hop_count);
    if (anchor_enumeration_response_config.active && matches &&
        enumeration_response_lane_complete_depth(
            anchor_enumeration_response_config.start_ms,
            now_ms,
            anchor_enumeration_response_config.max_hop_count)) {
        anchor_enumeration_response_config.active = false;
    }
    active = anchor_enumeration_response_config.active && matches &&
        enumeration_response_timing_at_depth(
            anchor_enumeration_response_config.start_ms,
            now_ms,
            anchor_enumeration_response_config.max_hop_count,
            &timing);
    if (active && timing_out != NULL) {
        *timing_out = timing;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return active;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int anchor_compact_enumeration_lane_resume(
    const struct anchor_enumeration_response_config *config)
{
    struct enumeration_response_lane *lane =
        &anchor_enumeration_response_lane;
    int ret;

    if (config == NULL || !config->active || config->epoch == 0u ||
        config->parent_id == 0u) {
        return PROTO_ERR_ARG;
    }
    if (lane->active && lane->network_id == NETWORK_ID &&
        lane->epoch == config->epoch &&
        enumeration_response_lane_local_id(lane) == DEVICE_ID &&
        lane->parent_id == config->parent_id &&
        lane->hop_count == config->hop_count &&
        lane->start_ms == config->start_ms) {
        status_debug_printf(
            "DBG_ENUM_COMPACT_RESUME epoch=%u records=%u acked=0x%02x\n",
            lane->epoch,
            lane->record_count,
            lane->acked_mask);
        return PROTO_OK;
    }
    ret = enumeration_response_lane_begin(lane,
                                           NETWORK_ID,
                                           config->epoch,
                                           DEVICE_ID,
                                           config->parent_id,
                                           config->hop_count,
                                           config->start_ms);
    status_debug_printf(
        "DBG_ENUM_COMPACT_STATE_BEGIN epoch=%u parent=0x%016llx hop=%u bytes=%u ret=%d\n",
        config->epoch,
        (unsigned long long)config->parent_id,
        config->hop_count,
        (unsigned int)sizeof(*lane),
        ret);
    return ret;
}
#endif

static bool anchor_compact_enumeration_prepare_window(uint64_t now_ms)
{
    uint64_t prepare_start_ms;
    bool active = false;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_enumeration_response_config.active &&
        now_ms < anchor_enumeration_response_config.start_ms) {
        prepare_start_ms =
            anchor_enumeration_response_config.start_ms >
                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS ?
                anchor_enumeration_response_config.start_ms -
                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS :
                0u;
        active = now_ms >= prepare_start_ms;
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return active;
}

static uint32_t anchor_compact_enumeration_ms_until_prepare(uint64_t now_ms)
{
    uint64_t prepare_start_ms;
    uint32_t remaining_ms = 0u;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    if (anchor_enumeration_response_config.active &&
        now_ms < anchor_enumeration_response_config.start_ms) {
        prepare_start_ms =
            anchor_enumeration_response_config.start_ms >
                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS ?
                anchor_enumeration_response_config.start_ms -
                    ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS :
                0u;
        if (now_ms < prepare_start_ms) {
            uint64_t remaining = prepare_start_ms - now_ms;

            remaining_ms = remaining > UINT32_MAX ?
                UINT32_MAX : (uint32_t)remaining;
        }
    }
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    return remaining_ms;
}

static int anchor_compact_enumeration_try_tx(
    struct enumeration_response_lane *lane,
    const struct enumeration_response_timing *timing)
{
    struct uwb_enumeration_bundle_frame bundle;
    uint8_t encoded[UWB_ENUM_BUNDLE_MAX_LEN];
    size_t encoded_len = 0u;
    int ret;

    if (lane == NULL || !lane->active ||
        !enumeration_response_lane_tx_round_allowed(
            timing->depth, lane->hop_count, timing->round)) {
        return 0;
    }
    ret = enumeration_response_lane_prepare_round(
        lane,
        timing->round,
        sys_rand32_get());
    if (ret == PROTO_OK) {
        ret = enumeration_response_lane_bundle_for_offset(
            lane, timing, &bundle);
    }
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = uwb_encode_enumeration_bundle(
        &bundle, encoded, sizeof(encoded), &encoded_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = dwm3000_driver_send_frame(
        encoded, encoded_len, UWB_CONTROL_TX_TIMEOUT_MS);
    status_debug_printf(
        "DBG_ENUM_COMPACT_TX epoch=%u parent=0x%016llx hop=%u round=%u offset_ms=%u seq=%u records=%u ret=%d\n",
        bundle.epoch,
        (unsigned long long)bundle.parent_id,
        timing->depth,
        timing->round,
        timing->round_offset_ms,
        bundle.sequence,
        bundle.record_count,
        ret);
    return ret < 0 ? ret : 1;
}

static int anchor_compact_enumeration_handle_raw(
    struct enumeration_response_lane *lane,
    const uint8_t *frame,
    size_t frame_len,
    const struct enumeration_response_timing *timing,
    uint64_t round_deadline_ms)
{
    struct uwb_enumeration_bundle_frame bundle;
    struct uwb_enumeration_hop_ack_frame ack;
    uint8_t encoded[UWB_ENUM_HOP_ACK_LEN];
    size_t encoded_len = 0u;
    bool added = false;
    bool matched;
    int ret;

    ret = uwb_decode_enumeration_hop_ack(frame, frame_len, &ack);
    if (ret == PROTO_OK) {
        matched = enumeration_response_lane_note_ack(lane, &ack);
        status_debug_printf(
            "DBG_ENUM_COMPACT_ACK_RX epoch=%u parent=0x%016llx seq=%u matched=%u\n",
            ack.epoch,
            (unsigned long long)ack.parent_id,
            ack.sequence,
            matched ? 1u : 0u);
        return 1;
    }
    ret = uwb_decode_enumeration_bundle(frame, frame_len, &bundle);
    if (ret != PROTO_OK) {
        return 0;
    }

    if (lane == NULL || !lane->active ||
        timing->depth <= lane->hop_count) {
        ret = PROTO_ERR_STALE;
    } else {
        ret = enumeration_response_lane_merge_bundle(lane, &bundle, &added);
        if (ret == PROTO_OK) {
            ack = (struct uwb_enumeration_hop_ack_frame) {
                .network_id = lane->network_id,
                .epoch = lane->epoch,
                .parent_id = enumeration_response_lane_local_id(lane),
                .child_id = bundle.sender_id,
                .sequence = bundle.sequence,
            };
        }
    }
    if (ret != PROTO_OK) {
        status_debug_printf(
            "DBG_ENUM_COMPACT_BUNDLE_REJECT epoch=%u sender=0x%016llx depth=%u ret=%d\n",
            bundle.epoch,
            (unsigned long long)bundle.sender_id,
            timing->depth,
            ret);
        return 1;
    }
    ret = uwb_encode_enumeration_hop_ack(
        &ack, encoded, sizeof(encoded), &encoded_len);
    if (ret != PROTO_OK) {
        status_debug_printf(
            "DBG_ENUM_COMPACT_ACK_ENCODE_FAIL epoch=%u child=0x%016llx seq=%u ret=%d\n",
            ack.epoch,
            (unsigned long long)ack.child_id,
            ack.sequence,
            ret);
        return 1;
    }
    {
        uint64_t ack_start_ms = (uint64_t)k_uptime_get();

        ret = dwm3000_driver_send_frame_tracked_until(
            encoded,
            encoded_len,
            UWB_CONTROL_TX_TIMEOUT_MS,
            round_deadline_ms,
            NULL);
        if (ret == -ETIMEDOUT) {
            status_debug_printf(
                "DBG_ENUM_COMPACT_ACK_SKIP epoch=%u child=0x%016llx seq=%u remaining=%u\n",
                ack.epoch,
                (unsigned long long)ack.child_id,
                ack.sequence,
                round_deadline_ms > ack_start_ms ?
                    (uint32_t)(round_deadline_ms - ack_start_ms) : 0u);
        } else {
            status_debug_printf(
                "DBG_ENUM_COMPACT_ACK_TX epoch=%u child=0x%016llx seq=%u remaining=%u ret=%d\n",
                ack.epoch,
                (unsigned long long)ack.child_id,
                ack.sequence,
                round_deadline_ms > ack_start_ms ?
                    (uint32_t)(round_deadline_ms - ack_start_ms) : 0u,
                ret);
        }
    }
    status_debug_printf(
        "DBG_ENUM_COMPACT_BUNDLE_RX epoch=%u child=0x%016llx seq=%u records=%u added=%u ack_ret=%d\n",
        bundle.epoch,
        (unsigned long long)bundle.sender_id,
        bundle.sequence,
        bundle.record_count,
        added ? 1u : 0u,
        ret);
    return 1;
}

/*
 * Keep extended-PHR RX armed across the expanding lane, without cell gaps.
 * Compact BUNDLE/ACK frames remain short, while the same listener can hand
 * the full assignment TABLE straight to the normal mesh decoder as soon as
 * the gateway closes collection.  One PHY for both phases removes a lossy
 * retune boundary without changing the response-slot schedule.
 */
static int anchor_run_compact_enumeration_lane(
    uint8_t *frame,
    size_t *frame_len,
    uint8_t *quality,
    enum dwm3000_rx_failure *rx_failure,
    uint32_t *elapsed_us)
{
#if DEVICE_ROLE != ROLE_ANCHOR
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(quality);
    ARG_UNUSED(rx_failure);
    ARG_UNUSED(elapsed_us);
    return -ENOTSUP;
#else
    struct anchor_enumeration_response_config config;
    struct enumeration_response_lane *lane =
        &anchor_enumeration_response_lane;
    int ret;

    k_mutex_lock(&anchor_discovery_claim_mutex, K_FOREVER);
    config = anchor_enumeration_response_config;
    k_mutex_unlock(&anchor_discovery_claim_mutex);
    if (!config.active) {
        return -EAGAIN;
    }
    ret = anchor_compact_enumeration_lane_resume(&config);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = dwm3000_driver_configure_wake_mesh_control_mode();

    if (ret < 0) {
        return ret;
    }
    while (true) {
        struct enumeration_response_timing timing;
        uint64_t now_ms = (uint64_t)k_uptime_get();
        uint64_t round_start_ms;
        uint64_t round_deadline_ms;
        uint64_t receive_deadline_ms;
        uint8_t next_tx_offset_ms;
        uint32_t remaining_ms;
        uint32_t started_cycles;
        int tx_ret;
        bool lane_started = now_ms >= config.start_ms;

        if (lane_started && !anchor_compact_enumeration_window(
                                now_ms, &config, &timing)) {
            return -EAGAIN;
        }
        if (dwm3000_driver_receive_abort_pending() ||
            mesh_c5_protocol_flood_work_pending() ||
            radio_guard_uwb_admission_paused()) {
            return -ECANCELED;
        }
        if (!lane_started) {
            round_deadline_ms = config.start_ms;
            receive_deadline_ms = config.start_ms;
        } else {
            round_start_ms = now_ms - timing.round_offset_ms;
            round_deadline_ms = round_start_ms +
                                ENUMERATION_RESPONSE_ROUND_MS;
            tx_ret = anchor_compact_enumeration_try_tx(lane, &timing);
            if (tx_ret < 0) {
                status_debug_printf(
                    "DBG_ENUM_COMPACT_TX_FAIL depth=%u round=%u offset_ms=%u ret=%d\n",
                    timing.depth,
                    timing.round,
                    timing.round_offset_ms,
                    tx_ret);
            }
            next_tx_offset_ms = enumeration_response_lane_next_offset_ms(
                lane, &timing);
            receive_deadline_ms = round_deadline_ms;
            if (next_tx_offset_ms != ENUMERATION_RESPONSE_NO_OFFSET) {
                uint64_t next_tx_ms = round_start_ms + next_tx_offset_ms;

                if (next_tx_ms > now_ms && next_tx_ms < receive_deadline_ms) {
                    receive_deadline_ms = next_tx_ms;
                }
            }
        }
        now_ms = (uint64_t)k_uptime_get();
        if (now_ms >= receive_deadline_ms) {
            continue;
        }
        remaining_ms = (uint32_t)(receive_deadline_ms - now_ms);
        *frame_len = 0u;
        *quality = 0u;
        *rx_failure = DWM3000_RX_FAILURE_NONE;
        started_cycles = k_cycle_get_32();
        ret = dwm3000_driver_receive_frame_continuous(
            remaining_ms,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            frame_len,
            quality,
            NULL,
            rx_failure);
        *elapsed_us = u32_saturating_add(
            *elapsed_us,
            (uint32_t)k_cyc_to_us_floor64(
                (uint32_t)(k_cycle_get_32() - started_cycles)));
        if (ret == 0) {
            if (!lane_started) {
                /* Let the normal decoder handle any last control frame. */
                return 0;
            }
            int handled = anchor_compact_enumeration_handle_raw(
                lane, frame, *frame_len, &timing, round_deadline_ms);

            if (handled == 0) {
                return 0;
            }
            app_watchdog_note_radio_progress();
            continue;
        }
        if (ret == -ETIMEDOUT) {
            app_watchdog_note_radio_progress();
            continue;
        }
        if (ret == -ECANCELED) {
            return ret;
        }
        /* A collision/CRC loss consumes this attempt; the next round retries. */
        status_debug_printf(
            "DBG_ENUM_COMPACT_RX_RETRY depth=%u round=%u offset_ms=%u ret=%d fail=%u\n",
            timing.depth,
            timing.round,
            timing.round_offset_ms,
            ret,
            (unsigned int)*rx_failure);
    }
#endif
}

static void anchor_uwb_scan_work_handler(struct k_work *work)
{
    uint8_t *frame = anchor_uwb_scan_frame;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    uint32_t retained_sleep_us = 0u;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int64_t uwb_window_start_ms = -1;
    bool preamble_detected = false;
    bool log_scan_window = false;
    uint32_t scan_debug_now_ms = 0u;
    bool handled_claim = false;
    bool deferred_mesh_rx_queued = false;
    bool route_wake_handoff = false;
    bool rx_attempted = false;
    size_t route_wake_frame_len = 0u;
    uint8_t route_wake_quality = 0u;
    bool route_waiting_active = false;
    bool relay_tx_active = false;
    bool relay_tx_retry_blocks_scan = false;
    bool mesh_rx_active = false;
    bool ch9_ack_wait_active = false;
    bool ch9_rx_conflict = false;
    bool uwb_radio_busy = false;
    bool control_followup_boost_active = false;
    bool enumeration_continuous_rx = false;
    bool survey_continuous_rx = false;
    bool protocol_continuous_rx = false;
    bool compact_enumeration_window = false;
    bool local_protocol_response_active = false;
    bool protocol_owned_work_due = false;
    bool protocol_flood_pending = false;
    bool post_ch9_recovery_scan = false;
    bool ch9_receive_prepare_pending = false;
    uint32_t ch9_retry_ms = 0u;
    uint32_t ch9_receive_prepare_delay_ms = 0u;
    uint32_t compact_lane_start_wait_ms = 0u;
    uint32_t relay_tx_retry_ms = 0u;
    uint32_t scan_rx_ms = ANCHOR_UWB_SCAN_RX_MS;
    uint32_t mesh_rx_queue_depth = 0u;
    bool mesh_rx_response_busy = false;
    uint32_t blocked_retry_ms = anchor_uwb_scan_blocked_retry_ms();
    uint32_t scan_rx_start_cycles = 0u;
    uint32_t scan_rx_elapsed_us = 0u;
    uint32_t scan_rx_attempt_elapsed_us = 0u;
    uint32_t scan_extra_awake_us = 0u;
    struct radio_guard_uwb_lease radio_lease = {0};
    /* A rejected/stale release must never qualify the scan as radio progress. */
    int low_power_ret = -EIO;
    int release_ret;
    int ret;

    ARG_UNUSED(work);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_HANDLER role=%u\n", DEVICE_ROLE);
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (radio_guard_uwb_poisoned()) {
        LOG_ERR("anchor scan suppressed while radio parking is poisoned: %d",
                radio_guard_uwb_poison_error());
        return;
    }
    if (anchor_click_handoff_pending() || anchor_click_window_active()) {
        /* A received click must keep priority until its lease is resolved. */
        anchor_uwb_scan_schedule_ms(ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS);
        return;
    }
    control_followup_boost_active =
        anchor_relay_control_followup_boost_active();
    enumeration_continuous_rx = anchor_enumeration_rx_active();
    survey_continuous_rx = app_survey_anchor_rx_continuous();
    protocol_continuous_rx = enumeration_continuous_rx ||
                             survey_continuous_rx;
    local_protocol_response_active =
        mesh_report_local_protocol_response_active();
    protocol_flood_pending = mesh_c5_protocol_flood_work_pending();
    post_ch9_recovery_scan =
        atomic_get(&anchor_ch5_post_ch9_recovery_scan_pending) != 0;
    if (enumeration_continuous_rx && local_protocol_response_active) {
        /*
         * Keep one continuous receive window for the gateway's one-shot
         * TABLE while our CLAIM remains in reliable custody. The retry
         * planner below caps this window immediately before the exact
         * Channel-9 retry boundary.
         */
        scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS;
    } else if (protocol_continuous_rx) {
        /*
         * Later operation controls are sent while the accepted protocol owns
         * Channel 5. Keep one protocol-max acquisition slice so workqueue
         * activity does not recreate a low-duty gap between those controls.
         */
        scan_rx_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS;
    } else if (post_ch9_recovery_scan) {
        /*
         * Returning from a required child turn is the weakest point in the
         * gateway contact path.  Give the anchor one useful acquisition
         * window before it falls back to ordinary low-duty scans; the next
         * Channel-9 prepare boundary below still caps this window.
         */
        scan_rx_ms = MESH_ROUTE_TEST_CH5_GAP_SCAN_MS;
    } else if (control_followup_boost_active) {
        scan_rx_ms = MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS;
    }
    if (enumeration_continuous_rx) {
        compact_lane_start_wait_ms =
            anchor_compact_enumeration_ms_until_prepare(
                (uint64_t)k_uptime_get());
        if (compact_lane_start_wait_ms != 0u &&
            scan_rx_ms > compact_lane_start_wait_ms) {
            scan_rx_ms = compact_lane_start_wait_ms;
        }
    }
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    relay_tx_retry_blocks_scan = anchor_relay_retry_plan_scan(
        k_uptime_get_32(),
        local_protocol_response_active,
        &scan_rx_ms,
        &relay_tx_retry_ms);
    mesh_rx_response_busy = mesh_rx_response_active();
    mesh_rx_active = mesh_rx_response_busy;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_rx_queue_depth = mesh_rx_pending_count();
        ch9_ack_wait_active = app_mesh_ch9_core_ack_wait_active(
            &mesh_runtime.pending, relay_tx_active);
    }
    uwb_radio_busy = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                     radio_guard_uwb_busy();
    ch9_rx_conflict = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                      !uwb_radio_busy &&
                      mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms);
    ch9_receive_prepare_pending =
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (protocol_continuous_rx || post_ch9_recovery_scan) &&
        mesh_report_next_channel9_activity_prepare_delay_ms(
            k_uptime_get_32(), &ch9_receive_prepare_delay_ms);
    if (ch9_receive_prepare_pending) {
        const uint32_t scan_reserve_ms =
            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +
            MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS;

        if (ch9_receive_prepare_delay_ms <= scan_reserve_ms) {
            ch9_rx_conflict = true;
            ch9_retry_ms = MAX(ch9_receive_prepare_delay_ms, 1u);
        } else if (scan_rx_ms >
                   ch9_receive_prepare_delay_ms - scan_reserve_ms) {
            scan_rx_ms =
                ch9_receive_prepare_delay_ms - scan_reserve_ms;
            status_debug_printf(
                "DBG_CH5_SCAN_CAP next=%u reserve=%u win=%u protocol=%u recovery=%u\n",
                ch9_receive_prepare_delay_ms,
                scan_reserve_ms,
                scan_rx_ms,
                protocol_continuous_rx ? 1u : 0u,
                post_ch9_recovery_scan ? 1u : 0u);
        }
    }
    if (ch9_rx_conflict) {
        atomic_set(&anchor_ch5_post_ch9_recovery_scan_pending, 1);
    }
    /*
     * Relay and route-wait records are passive custody between physical
     * attempts, so they must not suppress the Channel-5 contact scan. A live
     * Channel-9 ACK wait already owns the receive turn, however, and retuning
     * to Channel 5 here would guarantee that the hop ACK is missed.
     */
    if (anchor_uwb_window_active() ||
        relay_tx_retry_blocks_scan ||
        protocol_flood_pending ||
        mesh_rx_active ||
        ch9_ack_wait_active ||
        ch9_rx_conflict ||
        uwb_radio_busy) {
        uint32_t retry_ms = relay_tx_retry_blocks_scan ? relay_tx_retry_ms :
                            mesh_rx_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_ack_wait_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            uwb_radio_busy ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_rx_conflict ? ch9_retry_ms : blocked_retry_ms;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_BLOCKED win=%u tx=%u retry_gap=%u flood=%u wait=%u rx=%u rxq=%u rxr=%u ack=%u ch9=%u rf=%u retry=%u interval=%u\n",
                                anchor_uwb_window_active() ? 1u : 0u,
                                relay_tx_active ? 1u : 0u,
                                relay_tx_retry_blocks_scan ? 1u : 0u,
                                protocol_flood_pending ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                mesh_rx_active ? 1u : 0u,
                                mesh_rx_queue_depth,
                                mesh_rx_response_busy ? 1u : 0u,
                                ch9_ack_wait_active ? 1u : 0u,
                                ch9_rx_conflict ? 1u : 0u,
                                uwb_radio_busy ? 1u : 0u,
                                retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(retry_ms);
        return;
    }

    ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,
                                "anchor low-duty UWB wake scan",
                                &radio_lease);
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
    uwb_window_start_ms = k_uptime_get();
    frame_len = 0u;
    quality = 0u;
    retained_sleep_us = 0u;
    rx_failure = DWM3000_RX_FAILURE_NONE;
    preamble_detected = false;
    next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        scan_debug_now_ms = k_uptime_get_32();
        log_scan_window = anchor_ch5_scan_debug_next_ms == 0u ||
                          uptime_deadline_reached(scan_debug_now_ms,
                                                  anchor_ch5_scan_debug_next_ms);
        if (log_scan_window) {
            anchor_ch5_scan_debug_next_ms =
                scan_debug_now_ms + ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS;
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_ARM now=%u win=%u complete=%u interval=%u wait=%u tx=%u response=%u\n",
                                scan_debug_now_ms,
                                scan_rx_ms,
                                ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
                                anchor_uwb_scan_interval_ms,
                                mesh_route_waiting_tx_active() ? 1u : 0u,
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                                local_protocol_response_active ? 1u : 0u);
        }
    }

    compact_enumeration_window = anchor_compact_enumeration_window(
        (uint64_t)k_uptime_get(), NULL, NULL) ||
        anchor_compact_enumeration_prepare_window(
            (uint64_t)k_uptime_get());
    if (compact_enumeration_window) {
        rx_attempted = true;
        ret = anchor_run_compact_enumeration_lane(
            frame,
            &frame_len,
            &quality,
            &rx_failure,
            &scan_rx_elapsed_us);
        if (ret == 0) {
            goto scan_frame_ready;
        }
        next_scan_delay_ms = 0u;
        goto scan_complete;
    }

    ret = protocol_continuous_rx ?
        dwm3000_driver_configure_wake_mesh_control_mode() :
        dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret != 0) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_CONFIG_FAIL ret=%d\n", ret);
    }
    if (ret == 0) {
protocol_rx_window:
        enumeration_continuous_rx = anchor_enumeration_rx_active();
        survey_continuous_rx = app_survey_anchor_rx_continuous();
        protocol_continuous_rx = enumeration_continuous_rx ||
                                 survey_continuous_rx;
        if (enumeration_continuous_rx) {
            uint64_t enumeration_now_ms = (uint64_t)k_uptime_get();

            /*
             * Every duplicate control frame and recoverable RX error returns
             * through this label.  Re-evaluate the phase boundary here rather
             * than reusing the generic-listener decision made when this work
             * item first acquired the radio: otherwise continuous activity can
             * carry that listener across compact-lane start and consume the
             * anchor's own response bands.
             */
            if (anchor_compact_enumeration_window(
                    enumeration_now_ms, NULL, NULL) ||
                anchor_compact_enumeration_prepare_window(
                    enumeration_now_ms)) {
                ret = anchor_run_compact_enumeration_lane(
                    frame,
                    &frame_len,
                    &quality,
                    &rx_failure,
                    &scan_rx_elapsed_us);
                if (ret == 0) {
                    goto scan_frame_ready;
                }
                next_scan_delay_ms = 0u;
                goto scan_complete;
            }
            compact_lane_start_wait_ms =
                anchor_compact_enumeration_ms_until_prepare(
                    enumeration_now_ms);
            if (compact_lane_start_wait_ms != 0u &&
                scan_rx_ms > compact_lane_start_wait_ms) {
                scan_rx_ms = compact_lane_start_wait_ms;
            }
        }
        scan_rx_start_cycles = k_cycle_get_32();
        rx_attempted = true;
        ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
            scan_rx_ms,
            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len,
            &quality,
            NULL,
            &rx_failure);
        scan_rx_attempt_elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - scan_rx_start_cycles));
        if (scan_rx_attempt_elapsed_us < scan_rx_ms * 1000u) {
            scan_rx_attempt_elapsed_us = scan_rx_ms * 1000u;
        }
        scan_rx_elapsed_us = u32_saturating_add(scan_rx_elapsed_us,
                                                scan_rx_attempt_elapsed_us);
        enumeration_continuous_rx = anchor_enumeration_rx_active();
        survey_continuous_rx = app_survey_anchor_rx_continuous();
        protocol_continuous_rx = enumeration_continuous_rx ||
                                 survey_continuous_rx;
        if (protocol_continuous_rx) {
            uint32_t receive_prepare_delay_ms = 0u;
            const uint32_t scan_reserve_ms =
                ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +
                MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS;
            bool receive_prepare_due =
                mesh_report_next_channel9_activity_prepare_delay_ms(
                    k_uptime_get_32(), &receive_prepare_delay_ms) &&
                receive_prepare_delay_ms <= scan_reserve_ms;

            protocol_owned_work_due =
                mesh_relay_tx_active(&mesh_runtime) ||
                mesh_c5_protocol_flood_work_pending() ||
                mesh_rx_pending_count() > 0u ||
                mesh_rx_response_active() ||
                mesh_report_ch9_ack_wait_active() ||
                radio_guard_uwb_admission_paused() ||
                receive_prepare_due;
            if (receive_prepare_due) {
                status_debug_printf(
                    "DBG_PROTOCOL_CH5_YIELD_CH9 next=%u reserve=%u\n",
                    receive_prepare_delay_ms,
                    scan_reserve_ms);
            }

            if (ret == -ETIMEDOUT && !protocol_owned_work_due) {
                if (enumeration_continuous_rx &&
                    (anchor_compact_enumeration_window(
                         (uint64_t)k_uptime_get(), NULL, NULL) ||
                     anchor_compact_enumeration_prepare_window(
                         (uint64_t)k_uptime_get()))) {
                    next_scan_delay_ms = 0u;
                    goto scan_complete;
                }
                /*
                 * A bounded timeout leaves the configured Channel-5 PHY in a
                 * known idle state. Rearm RX under the same guard lease:
                 * parking/reconfiguration here would create the very gap the
                 * operation-owned listener exists to remove.
                 */
                app_watchdog_note_radio_progress();
                goto protocol_rx_window;
            }
        }
    }
scan_frame_ready:
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

        status_debug_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_FRAME len=%u q=%u wake_ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
        }
        if (decode_ret == PROTO_OK) {
            status_debug_anchor_wake_claim_rx_pulse();
            if ((anchor_enumeration_rx_active() ||
                 app_survey_anchor_rx_continuous()) &&
                app_mesh_c5_wake_claim_requires_anchor_handoff(
                    claim.flags, true)) {
                status_debug_printf(
                    "DBG_PROTOCOL_RX_CLICK_DEFER src=0x%llx event=%u\n",
                    (unsigned long long)claim.clicker_id,
                    claim.click_event_id);
                app_watchdog_note_radio_progress();
                goto protocol_rx_window;
            }
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                !app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags,
                                                                 true)) {
                if (!app_mesh_c5_route_wake_should_listen(
                        claim.clicker_id,
                        GATEWAY_ID,
                        claim.flags,
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ),
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL))) {
                    status_debug_printf("DBG_ANCHOR_DIRECT_GATEWAY_WAKE_IGNORED src=0x%llx evt=%u flags=0x%02x\n",
                                        (unsigned long long)claim.clicker_id,
                                        claim.click_event_id,
                                        claim.flags);
                    anchor_relay_control_followup_boost_begin(
                        claim.claimed_duration_ms);
                    if (app_mesh_c5_control_followup_yields_to_ack(
                            claim.flags, ch9_ack_wait_active)) {
                        status_debug_printf("DBG_ANCHOR_CONTROL_FOLLOWUP_DEFER_ACK src=0x%llx evt=%u direct=1\n",
                                            (unsigned long long)claim.clicker_id,
                                            claim.click_event_id);
                        goto scan_complete;
                    }
                    goto scan_complete;
                }
                if (!app_mesh_c5_route_wake_claim_allowed(
                        claim.clicker_id,
                        GATEWAY_ID,
                        claim.flags,
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ),
                        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL))) {
                    status_debug_printf("DBG_ANCHOR_DIRECT_GATEWAY_WAKE_LISTEN src=0x%llx evt=%u flags=0x%02x\n",
                                        (unsigned long long)claim.clicker_id,
                                        claim.click_event_id,
                                        claim.flags);
                }
                if (app_mesh_c5_wake_followup_is_control(claim.flags)) {
                    anchor_relay_control_followup_boost_cancel();
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
            if (anchor_handle_uwb_claim(&claim,
                                        quality,
                                        k_uptime_get(),
                                        false,
                                        &retained_sleep_us,
                                        &deferred_mesh_rx_queued)) {
                anchor_relay_control_followup_boost_cancel();
                handled_claim = true;
                uwb_anchor_abort_epoch(&anchor_uwb_session);
            }
        } else if (control_followup_boost_active) {
            status_debug_printf("DBG_ANCHOR_RELAY_CONTROL_FOLLOWUP_SKIP len=%u q=%u ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
            goto scan_complete;
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
                uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
                next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
                LOG_WRN("anchor false UWB wake cooldown: wake_decode_ret=%d reason=%s frame_len=%u quality=%u cooldown_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        app_anchor_wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                anchor_uwb_session.diagnostics.crc_failures,
                anchor_uwb_session.diagnostics.frame_timeouts);
            }
        }
    } else if (ret != -ETIMEDOUT && ret != -ECANCELED &&
               control_followup_boost_active) {
        status_debug_printf("DBG_ANCHOR_RELAY_CONTROL_FOLLOWUP_RX_FAIL ret=%d fail=%u\n",
                            ret,
                            (unsigned int)rx_failure);
    } else if (ret != -ETIMEDOUT && ret != -ECANCELED) {
        enum uwb_wake_decode_failure failure =
            app_anchor_wake_failure_from_rx(rx_failure);

        uwb_anchor_note_wake_decode_failure(&anchor_uwb_session, failure);
        uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
        next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
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
    }
    enumeration_continuous_rx = anchor_enumeration_rx_active();
    survey_continuous_rx = app_survey_anchor_rx_continuous();
    protocol_continuous_rx = enumeration_continuous_rx ||
                             survey_continuous_rx;
    if (protocol_continuous_rx && !protocol_owned_work_due) {
        if (ret == -ECANCELED) {
            goto scan_complete;
        }
        if (ret != 0 && ret != -ETIMEDOUT) {
            int recovery_ret = dwm3000_driver_force_recovery();
            int reconfigure_ret = recovery_ret < 0 ? recovery_ret :
                dwm3000_driver_configure_wake_mesh_control_mode();
            bool recovered = recovery_ret >= 0 && reconfigure_ret >= 0;
            enum protocol_rx_recovery_result recovery_result =
                enumeration_continuous_rx ?
                    anchor_enumeration_rx_note_recovery(
                        recovered,
                        recovered ? "rx-recovered" :
                                    "rx-recovery-failed") :
                    app_survey_anchor_rx_note_recovery(recovered);

            if (recovery_result == PROTOCOL_RX_RECOVERY_REARMED) {
                status_debug_printf(
                    "DBG_PROTOCOL_RX_RECOVERED owner=%s ret=%d failure=%u\n",
                    enumeration_continuous_rx ? "enumeration" : "survey",
                    ret,
                    (unsigned int)rx_failure);
                goto protocol_rx_window;
            }
            LOG_ERR("protocol RX recovery failed: owner=%s ret=%d failure=%u recovery_ret=%d reconfigure_ret=%d",
                    enumeration_continuous_rx ? "enumeration" : "survey",
                    ret,
                    (unsigned int)rx_failure,
                    recovery_ret,
                    reconfigure_ret);
            goto scan_complete;
        }
        /*
         * A duplicate, malformed frame, or recoverable receive failure did
         * not create protocol-owned relay/ACK work. Keep the configured PHY
         * and guard lease and immediately restore the operation listener.
         */
        goto protocol_rx_window;
    }

scan_complete:
    if (scan_rx_elapsed_us > ANCHOR_UWB_SCAN_RX_US) {
        scan_extra_awake_us = scan_rx_elapsed_us - ANCHOR_UWB_SCAN_RX_US;
    }
    release_ret = radio_guard_uwb_release_begin(&radio_lease);
    if (release_ret == 0) {
        enumeration_continuous_rx = anchor_enumeration_rx_active();
        survey_continuous_rx = app_survey_anchor_rx_continuous();
        protocol_continuous_rx = enumeration_continuous_rx ||
                                 survey_continuous_rx;
        low_power_ret = anchor_enter_low_power(
            protocol_continuous_rx ? APP_RADIO_LOW_POWER_IDLE :
                app_radio_low_power_mode_for_connection(
                    mesh_anchor_connected_radio_active()),
            "low-duty-scan-release");
        release_ret = radio_guard_uwb_release_finish(&radio_lease,
                                                      low_power_ret);
    }
    anchor_note_uwb_awake_since(
        uwb_window_start_ms,
        u32_saturating_add(
            u32_saturating_add(ANCHOR_UWB_IDLE_SCAN_AWAKE_US, scan_extra_awake_us),
            retained_sleep_us));
    if (release_ret < 0) {
        LOG_ERR("anchor low-duty scan radio release failed; retaining poisoned owner: %d",
                release_ret);
        return;
    }
    anchor_set_uwb_busy(false);
    if (!anchor_click_handoff_pending()) {
        anchor_click_window_set_active(false);
    }
    if (low_power_ret == 0 && rx_attempted &&
        app_mesh_rx_policy_dwm_attempt_made_progress(ret, rx_failure)) {
        app_watchdog_note_radio_progress();
    }
    if (post_ch9_recovery_scan && rx_attempted) {
        atomic_clear(&anchor_ch5_post_ch9_recovery_scan_pending);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_POST_CH9_RECOVERY_SCAN win=%u ret=%d pre=%u\n",
                                scan_rx_ms,
                                ret,
                                preamble_detected ? 1u : 0u);
        }
    }
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
                                radio_guard_uwb_busy() ? 1u : 0u);
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
    if (deferred_mesh_rx_queued &&
        next_scan_delay_ms > ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS) {
        /* A queued mesh RX handoff must never wait out a long idle gap. */
        next_scan_delay_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
    }
    protocol_flood_pending = mesh_c5_protocol_flood_work_pending();
    if (protocol_flood_pending) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
    } else if (anchor_relay_control_followup_boost_active() ||
               anchor_enumeration_rx_active() ||
               app_survey_anchor_rx_continuous()) {
        next_scan_delay_ms = 0u;
    }
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
    {
        const struct proto_packet scan_identity = {
            .src_id = DEVICE_ID,
            .dst_id = GATEWAY_ID,
            .msg_type = MSG_MESH_DATA,
        };

        app_stack_workload_diag_anchor_scan_cycle(
            &scan_identity,
            (uint16_t)MIN(mesh_rx_queue_depth, UINT16_MAX),
            anchor_uwb_window_active() ? 1u : 0u);
    }
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
}

static int anchor_start_uwb_scan(void)
{
    uint32_t startup_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ? 2000u : 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        LOG_INF("mesh-test source transmitter skipping anchor low-duty UWB scan");
        status_debug_note("DBG_ANCHOR_SCAN_SKIPPED_TX\n");
        return 0;
    }

    ret = anchor_uwb_scan_schedule_ms(startup_delay_ms);
    if (ret < 0) {
        return ret;
    }
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


static int anchor_delivery_gateway_accepted(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return anchor_discovery_assignment_gateway_accepted(packet,
                                                        semantic_digest);
}

static int anchor_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return anchor_discovery_assignment_gateway_confirmed(packet,
                                                         semantic_digest);
}

static const struct app_mesh_report_callbacks anchor_mesh_report_callbacks = {
#if DEVICE_ROLE == ROLE_ANCHOR
    .anchor_note_uwb_awake_since = anchor_note_uwb_awake_since,
    .anchor_handle_click_wake_claim = anchor_handle_mesh_click_wake_claim,
    .anchor_handle_local_command = anchor_handle_local_command,
    .anchor_delivery_gateway_accepted =
        anchor_delivery_gateway_accepted,
    .anchor_delivery_gateway_confirmed =
        anchor_delivery_gateway_confirmed,
#endif
#if DEVICE_ROLE == ROLE_GATEWAY
    .gateway_route_refresh_event = gateway_route_refresh_observe,
#endif
};

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void)
{
    return &anchor_mesh_report_callbacks;
}
#endif /* !APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY */
