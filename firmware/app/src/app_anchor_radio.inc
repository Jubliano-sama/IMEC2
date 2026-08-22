/*
 * A mesh handoff reserves report custody before this anchor enters RF.  Later
 * Channel-5 claims can still be arbitrated for an ordinary scan, but they
 * must not replace an identity whose report reservation is already live.
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
    return !admitted_handoff_identity_frozen || matches;
}

#if !defined(APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY)
#include "app_anchor_click_event_runtime.h"
#include "app_node_comm.h"

#line 6569 __BASE_FILE__
#define SURVEY_PAIR_RESULT_TIMING_TLV_MAX_LEN                            \
    (PROTO_TLV_U8_ENCODED_LEN + (2u * PROTO_TLV_U16_ENCODED_LEN) +     \
     PROTO_TLV_U32_ENCODED_LEN +                                       \
     PROTO_TLV_HEADER_LEN + (6u * sizeof(uint32_t)))
#define SURVEY_PAIR_RESULT_MAX_PAYLOAD_LEN                               \
    (SURVEY_SAMPLE_TLV_MAX_LEN + PROTO_TLV_U64_ENCODED_LEN +           \
     SURVEY_PAIR_RESULT_TIMING_TLV_MAX_LEN)

BUILD_ASSERT(SURVEY_PAIR_RESULT_MAX_PAYLOAD_LEN <=
                 APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN,
             "maximum survey pair result must fit durable communication");

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

static int anchor_queue_survey_sample_result(
    const struct survey_pair *pair,
    uint16_t round_id,
    uint16_t sample_index,
    uint64_t reporter_id,
    const struct app_node_comm_reservation_lease *delivery_reservation,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN],
    const struct dwm3000_range_result *range_result)
{
    struct mesh_outbound outbound = {0};
    struct survey_sample sample = {0};
    struct dwm3000_range_result survey_result;
    size_t payload_len = 0u;
    uint16_t transport_sequence;
    enum range_status status;
    int64_t sample_local_ms;
    int ret;

    if (pair == NULL || reporter_id == 0u || round_commitment == NULL ||
        delivery_reservation == NULL || delivery_reservation->token == 0u ||
        delivery_reservation->owner_generation != pair->operation_generation ||
        range_result == NULL) {
        return -EINVAL;
    }
    ret = survey_pair_result_transport_sequence(round_id,
                                                sample_index,
                                                &transport_sequence);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    status = range_status_valid(range_result->status) ?
             range_result->status : RANGE_INTERNAL_ERROR;

    sample.pair = *pair;
    sample.round_id = round_id;
    sample.sample_index = sample_index;
    sample.distance_mm = range_result->distance_mm;
    sample.quality = range_result->quality;
    sample.range_status = status;
    survey_result = *range_result;
    /*
     * Raw receiver diagnostics are click-debug artifacts and can add 110
     * bytes to one record. Survey results retain the bounded scalar timing
     * fields and raw DS-TWR timestamps, so every legal sample remains within
     * the production durable-communication snapshot on every driver path.
     */
    survey_result.clicker_rx_diag_sampled = false;
    survey_result.anchor_rx_diag_sampled = false;
    survey_result.clicker_rx_diag_raw_len = 0u;
    survey_result.anchor_rx_diag_raw_len = 0u;

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
                                          &survey_result);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_result_packet_from_reporter(
        &outbound.packet,
        &sample,
        reporter_id,
        GATEWAY_ID,
        transport_sequence,
        (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    return app_anchor_survey_result_delivery_stage_reserved(
        delivery_reservation,
        &outbound,
        round_commitment);
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

#if defined(CONFIG_IMEC_ML_ANCHOR)
static bool anchor_pair_schedule_matches_epoch(
    const struct uwb_anchor_pair_schedule_frame *schedule)
{
    return schedule != NULL &&
           anchor_uwb_session.epoch.active &&
           schedule->network_id == anchor_uwb_session.epoch.network_id &&
           schedule->clicker_id == anchor_uwb_session.epoch.clicker_id &&
           schedule->survey_id == anchor_uwb_session.epoch.click_event_id &&
           schedule->attempt_index == anchor_uwb_session.epoch.attempt_index &&
           schedule->nonce == anchor_uwb_session.epoch.nonce &&
           schedule->flags == anchor_uwb_session.epoch.flags;
}

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

    if (!anchor_pair_schedule_matches_epoch(schedule)) {
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
            if (ret < 0 && !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            (void)anchor_send_pair_survey_result(schedule,
                                                 pair_index,
                                                 initiator_id,
                                                 responder_id,
                                                 &range_result);
            low_power_ret = anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,
                                                   "pair-survey-initiator");
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
        bool same_epoch;

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

        if (!anchor_claim_collection_candidate_allowed(
                &anchor_uwb_session.epoch,
                &claim,
                admitted_handoff_identity_frozen,
                &same_epoch)) {
            LOG_DBG("anchor retained reserved UWB claim during collection: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx",
                    (unsigned long long)claim.clicker_id,
                    claim.click_event_id,
                    claim.attempt_index,
                    (unsigned long long)claim.priority_id);
            continue;
        }
        ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                           &claim,
                                           k_uptime_get_32(),
                                           &decision);
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
            /*
             * Keep the first copy's absolute schedule.  Later copies carry
             * countdowns and may arrive with bounded transport skew; using
             * each last copy would let that skew accumulate into a missed
             * discovery window.
             */
            if (!same_epoch) {
                selected_claim = claim;
                selected_rx_ms = k_uptime_get();
                selected_quality = quality;
                ret = app_anchor_click_event_runtime_claim(&claim,
                                                           k_uptime_get_32(),
                                                           NULL);
                if (ret < 0) {
                    LOG_ERR("anchor priority claim replaced RF epoch but event phase could not restart: ret=%d",
                            ret);
                    uwb_anchor_abort_epoch(&anchor_uwb_session);
                    return false;
                }
            } else if (quality > selected_quality) {
                selected_quality = quality;
            }
            selected_decision = decision;
        } else {
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
    if (click_priority &&
        mesh_preempt_for_click_event_until(preempt_deadline_ms) < 0) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click claim deferred because mesh custody preemption failed");
        anchor_click_event_abort_if_needed("mesh-preemption");
        return false;
    }
    if (click_priority) {
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
                              UWB_DISCOVERY_RX_LATE_GUARD_MS;
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

        while (k_uptime_get() < schedule_deadline_ms) {
            uint8_t frame_type = 0u;
            int64_t remaining_ms = schedule_deadline_ms - k_uptime_get();

            ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                          frame,
                                                          sizeof(anchor_uwb_scan_frame),
                                                          &frame_len,
                                                          NULL,
                                                          NULL,
                                                          NULL);
            if (ret == -ETIMEDOUT) {
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

#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_ANCHOR_PAIR_SCHEDULE) {
            struct uwb_anchor_pair_schedule_frame pair_schedule;

            ret = uwb_decode_anchor_pair_schedule(frame, frame_len, &pair_schedule);
            if (ret != PROTO_OK) {
                LOG_WRN("anchor pair-survey schedule decode failed: %d", ret);
                anchor_click_event_abort_if_needed("pair-schedule-decode");
                claim_handled = true;
                goto claim_complete;
            }
            if (!anchor_pair_schedule_matches_epoch(&pair_schedule)) {
                LOG_WRN("anchor rejected pair-survey schedule identity: survey=%u",
                        pair_schedule.survey_id);
                anchor_click_event_abort_if_needed("pair-schedule-identity");
                claim_handled = true;
                goto claim_complete;
            }
            if (retained_sleep_us != NULL) {
                *retained_sleep_us = u32_saturating_add(
                    *retained_sleep_us,
                    anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms));
            } else {
                (void)anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms);
            }
            anchor_click_event_abort_if_needed("pair-survey-complete");
            claim_handled = true;
            goto claim_complete;
        }
#endif

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
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
static atomic_t anchor_ch5_post_ch9_recovery_scan_pending;

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

static bool anchor_relay_retry_blocks_scan(uint32_t now_ms,
                                           uint32_t scan_rx_ms,
                                           uint32_t *retry_ms)
{
    uint32_t remaining_ms;
    uint32_t scan_occupancy_ms;

    if (retry_ms == NULL ||
        !mesh_relay_tx_active(&mesh_runtime) ||
        mesh_runtime.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return false;
    }

    if (uptime_deadline_reached(now_ms,
                                mesh_runtime.pending.retry_after_ms)) {
        *retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
        return true;
    }

    remaining_ms = uptime_ms_until_deadline(
        now_ms, mesh_runtime.pending.retry_after_ms);
    scan_occupancy_ms = scan_rx_ms +
                        ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS +
                        MESH_RADIO_EVENT_RETUNE_GUARD_MS;
    if (remaining_ms > scan_occupancy_ms) {
        return false;
    }

    /*
     * Wake at the exact higher-priority retry boundary. The mesh route queue
     * then claims the radio before this lower-priority scan queue can run.
     */
    *retry_ms = remaining_ms;
    return true;
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
    bool post_ch9_recovery_scan = false;
    uint32_t ch9_retry_ms = 0u;
    uint32_t relay_tx_retry_ms = 0u;
    uint32_t scan_rx_ms = ANCHOR_UWB_SCAN_RX_MS;
    uint32_t mesh_rx_queue_depth = 0u;
    bool mesh_rx_response_busy = false;
    uint32_t blocked_retry_ms = anchor_uwb_scan_blocked_retry_ms();
    uint32_t scan_rx_start_cycles = 0u;
    uint32_t scan_rx_elapsed_us = ANCHOR_UWB_SCAN_RX_US;
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
    post_ch9_recovery_scan =
        atomic_get(&anchor_ch5_post_ch9_recovery_scan_pending) != 0;
    if (control_followup_boost_active || post_ch9_recovery_scan) {
        scan_rx_ms = MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS;
    }
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    relay_tx_retry_blocks_scan = anchor_relay_retry_blocks_scan(
        k_uptime_get_32(), scan_rx_ms, &relay_tx_retry_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_rx_queue_depth = mesh_rx_pending_count();
        mesh_rx_response_busy = mesh_rx_response_active();
        mesh_rx_active = mesh_rx_response_busy;
        ch9_ack_wait_active = mesh_report_ch9_ack_wait_active();
    }
    uwb_radio_busy = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                     radio_guard_uwb_busy();
    ch9_rx_conflict = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                      !uwb_radio_busy &&
                      mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms);
    if (ch9_rx_conflict) {
        atomic_set(&anchor_ch5_post_ch9_recovery_scan_pending, 1);
    }
    /*
     * Relay, route-wait, and gateway-ACK records are passive custody between
     * physical attempts. They must not suppress the Channel-5 contact scan.
     * Once a retained retry is actually due, however, it owns one bounded
     * radio turn; otherwise a zero-gap contact scan can starve the exact
     * packet until its semantic-custody deadline.
     */
    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_radio_active() ||
        relay_tx_retry_blocks_scan ||
        mesh_rx_active ||
        ch9_rx_conflict ||
        uwb_radio_busy) {
        uint32_t retry_ms = relay_tx_retry_blocks_scan ? relay_tx_retry_ms :
                            mesh_rx_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            uwb_radio_busy ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_rx_conflict ? ch9_retry_ms : blocked_retry_ms;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_BLOCKED win=%u survey=%u tx=%u retry_gap=%u wait=%u rx=%u rxq=%u rxr=%u ack=%u ch9=%u rf=%u retry=%u interval=%u\n",
                                anchor_uwb_window_active() ? 1u : 0u,
                                app_anchor_survey_runtime_radio_active() ? 1u : 0u,
                                relay_tx_active ? 1u : 0u,
                                relay_tx_retry_blocks_scan ? 1u : 0u,
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
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_ARM now=%u win=%u complete=%u interval=%u wait=%u tx=%u\n",
                                scan_debug_now_ms,
                                scan_rx_ms,
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
        scan_rx_elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - scan_rx_start_cycles));
        if (scan_rx_elapsed_us < scan_rx_ms * 1000u) {
            scan_rx_elapsed_us = scan_rx_ms * 1000u;
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

        status_debug_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_FRAME len=%u q=%u wake_ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
        }
        if (decode_ret == PROTO_OK) {
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
    } else if (ret != -ETIMEDOUT && control_followup_boost_active) {
        status_debug_printf("DBG_ANCHOR_RELAY_CONTROL_FOLLOWUP_RX_FAIL ret=%d fail=%u\n",
                            ret,
                            (unsigned int)rx_failure);
    } else if (ret != -ETIMEDOUT) {
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

scan_complete:
    if (scan_rx_elapsed_us > ANCHOR_UWB_SCAN_RX_US) {
        scan_extra_awake_us = scan_rx_elapsed_us - ANCHOR_UWB_SCAN_RX_US;
    }
    release_ret = radio_guard_uwb_release_begin(&radio_lease);
    if (release_ret == 0) {
        low_power_ret = anchor_enter_low_power(
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
        (ret == 0 || ret == -ETIMEDOUT ||
         app_anchor_rx_failure_detected_preamble(rx_failure))) {
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
    if (anchor_relay_control_followup_boost_active()) {
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


static int anchor_survey_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_SURVEY_DISCOVERY_REPORT) {
        return app_anchor_survey_delivery_gateway_confirmed(
            packet, semantic_digest);
    }
    if (packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        return app_anchor_survey_result_delivery_gateway_confirmed(
            packet, semantic_digest);
    }
    return -ENOENT;
}

static int anchor_delivery_gateway_accepted(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return anchor_discovery_assignment_gateway_accepted(packet,
                                                        semantic_digest);
}

static int anchor_survey_delivery_transport_released(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool preempted)
{
    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_SURVEY_DISCOVERY_REPORT) {
        return app_anchor_survey_delivery_transport_released(
            packet, semantic_digest, preempted);
    }
    if (packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        return app_anchor_survey_result_delivery_transport_released(
            packet, semantic_digest, preempted);
    }
    return 0;
}

static const struct app_mesh_report_callbacks anchor_mesh_report_callbacks = {
#if DEVICE_ROLE == ROLE_ANCHOR
    .anchor_survey_radio_active =
        app_anchor_survey_runtime_radio_active,
    .anchor_survey_operation_generation_active =
        app_anchor_survey_runtime_operation_generation_active,
    .anchor_note_uwb_awake_since = anchor_note_uwb_awake_since,
    .anchor_handle_click_wake_claim = anchor_handle_mesh_click_wake_claim,
    .anchor_handle_local_command = anchor_handle_local_command,
    .anchor_handle_survey_discovery_start =
        app_anchor_survey_discovery_handle_start,
    .anchor_handle_survey_pair_prepare =
        app_anchor_survey_runtime_handle_pair_prepare,
    .anchor_delivery_gateway_accepted =
        anchor_delivery_gateway_accepted,
    .anchor_survey_delivery_gateway_confirmed =
        anchor_survey_delivery_gateway_confirmed,
    .anchor_survey_delivery_transport_released =
        anchor_survey_delivery_transport_released,
#endif
#if DEVICE_ROLE == ROLE_GATEWAY
    .gateway_handle_survey_discovery_report = gateway_handle_survey_discovery_report,
    .gateway_note_anchor_boot_observation =
        gateway_note_anchor_boot_observation,
    .gateway_note_ack_confirm = gateway_note_survey_ack_confirm,
    .gateway_route_refresh_event = gateway_route_refresh_observe,
#endif
};

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void)
{
    return &anchor_mesh_report_callbacks;
}
#endif /* !APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY */
