#include "dwm3000_driver.h"
#include "dwm3000_timing.h"
#include "uwb_session.h"
#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DWM3000_DS_TWR_RTT_DEBUG_ENABLED 0
#define DWM3000_REPLY_DELAY_FROM_ROUND_INDEX 0
#define DWM3000_UUS_TO_DWT_TIME DWM3000_TIMING_UUS_TO_RCTU
#define DWM3000_TX_ANT_DLY DWM3000_TIMING_TX_ANTENNA_DELAY_RCTU
#define DELAYED_RX_PREAMBLE_TIMEOUT_PAC DWM3000_TIMING_DELAYED_PREAMBLE_TIMEOUT_PAC
#define DEFAULT_RESPONDER_WINDOW_MS UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS
#define DWM3000_PHY_RANGE DWM3000_TIMING_PHY_CH5_RANGE

enum stage { POLL, RESPONSE, FINAL, REPORT, INITIATOR };
enum noise { CLICK, OTHER_INITIATOR, OTHER_TARGET, OTHER_NETWORK,
             OTHER_SESSION, OTHER_NONCE, OTHER_SEQUENCE, OTHER_ROUND,
             OTHER_FLAGS, TRUNCATED, OVERSIZED, WRONG_TYPE, NOISE_COUNT };

struct physical_frame {
    uint8_t bytes[128];
    size_t length;
    uint32_t delay_ms;
    uint32_t completion_delay_ms;
    uint32_t status;
    int error;
    uint64_t timestamp;
    int16_t clock_offset;
    int32_t carrier;
};

static struct physical_frame frames[64];
static size_t frame_count, consumed;
static int64_t now_us, deadlines[2];
static unsigned int receives[2], arms, stops, clears, tx_starts, tx_writes, patches;
static unsigned int fail_arm;
static bool fail_clear, rx_armed;
static int deadline_abort_error;
static enum stage tested_stage;
static uint8_t staged_response[UWB_FINAL_LEN];
static size_t staged_length;
static uint32_t programmed_tx_time;
static uint32_t receive_entry_pause_ms, tx_start_pause_ms;
static uint32_t poll_completion_pause_ms, final_completion_pause_ms;
static unsigned int poll_starts, phy_configurations;
static unsigned int poll_completions, final_completions;
static uint64_t poll_tx_timestamp, observed_send_deadline;
static struct dwm3000_range_result *observed_result;

static const struct dwm3000_range_request default_request = {
    .initiator_id = UINT64_C(0x100000001234),
    .responder_id = UINT64_C(0x200000005678),
    .network_id = 123, .session_nonce = UINT64_C(0xabc12345678),
    .responder_short_addr = 0x5678, .session_id = 77,
    .seq = 3, .round_index = 2, .flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
    .timeout_ms = 23, .reply_delay_uus = UWB_RANGE_REPLY_DELAY_UUS,
    .capture_rsl = true,
};
static struct dwm3000_range_request request;

static int64_t k_uptime_get(void) { return now_us / 1000; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)k_uptime_get(); }
static void status_debug_printf(const char *format, ...) { (void)format; }

#include "dwm3000_range_filter_helpers.inc"

/* Each rearm costs 1 ms of actual modeled SPI/driver work. An RX completion
 * disables RX; ignoring a frame must explicitly arm it again. */
void dwt_forcetrxoff(void) { stops++; rx_armed = false; now_us += 50; }
static void clear_all_events(void) { clears++; now_us += 200; }
static int take_port_error(const char *operation)
{
    now_us += 50;
    if (strcmp(operation, "range-deadline-abort") == 0) return deadline_abort_error;
    return fail_clear && strcmp(operation, "range-ignore-frame") == 0 ? -EIO : 0;
}
static int start_immediate_rx(void)
{
    assert(!rx_armed);
    now_us += 700;
    arms++;
    if (arms == fail_arm) {
        return -EIO;
    }
    rx_armed = true;
    return 0;
}
static int ensure_phy_mode(int mode) { assert(mode == DWM3000_PHY_RANGE); phy_configurations++; return 0; }
void dwt_setpreambledetecttimeout(uint16_t timeout) { assert(timeout == 0); }
void dwt_setrxtimeout(uint32_t timeout) { (void)timeout; }
void dwt_setrxaftertxdelay(uint32_t delay) { (void)delay; }
void dwt_setdelayedtrxtime(uint32_t time) { programmed_tx_time = time; }
uint32_t dwt_readsystimestamphi32(void)
{ return (uint32_t)(((uint64_t)now_us * DWM3000_UUS_TO_DWT_TIME) >> 8); }
static int send_range_frame_until(const uint8_t *frame, size_t length,
    uint8_t mode, uint64_t absolute_deadline_ms, bool *rf_start_possible,
    uint64_t *rf_start_at_ms)
{
    struct uwb_poll_frame poll;
    assert(uwb_decode_poll_frame(frame, length, &poll) == PROTO_OK);
    assert(mode == (DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED));
    (void)rf_start_possible;
    (void)rf_start_at_ms;
    observed_send_deadline = absolute_deadline_ms;
    if (absolute_deadline_ms != 0u && (uint64_t)k_uptime_get() >= absolute_deadline_ms) {
        return -ETIMEDOUT;
    }
    poll_starts++;
    poll_tx_timestamp = (uint64_t)now_us * DWM3000_UUS_TO_DWT_TIME;
    now_us += (int64_t)dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_RANGE, length);
    rx_armed = true;
    return 0;
}
static int capture_completed_tx_timestamp(uint32_t timeout_ms,
    uint64_t *timestamp, uint64_t absolute_deadline_ms)
{
    assert(timeout_ms > 0u);
    assert(absolute_deadline_ms == request.absolute_deadline_ms);
    poll_completions++;
    *timestamp = poll_tx_timestamp;
    now_us += 100 + (int64_t)poll_completion_pause_ms * 1000;
    return 0;
}
static int wait_tx_complete_observed(uint32_t timeout_ms,
    uint64_t *completed_at_ms, uint64_t absolute_deadline_ms)
{
    assert(timeout_ms > 0u && tx_starts == 1u);
    assert(absolute_deadline_ms == request.absolute_deadline_ms);
    assert(timeout_ms <= ds_twr_rx_wait_timeout_ms(request.reply_delay_uus));
    assert(absolute_deadline_ms == 0u ||
           (uint64_t)k_uptime_get() + timeout_ms <= absolute_deadline_ms);
    final_completions++;
    int32_t until_marker = (int32_t)((programmed_tx_time & UINT32_C(0xfffffffe)) -
                                   dwt_readsystimestamphi32());
    if (until_marker > 0) {
        uint64_t tail = dwm3000_timing_airtime_rctu(
            DWM3000_TIMING_PHY_CH5_RANGE, UWB_FINAL_LEN) -
            dwm3000_timing_shr_rctu(DWM3000_TIMING_PHY_CH5_RANGE);
        now_us += (int64_t)dwm3000_timing_rctu_to_us_ceil(
            ((uint64_t)(uint32_t)until_marker << 8) + tail);
    }
    now_us += 100 + (int64_t)final_completion_pause_ms * 1000;
    if (completed_at_ms != NULL) *completed_at_ms = (uint64_t)k_uptime_get();
    return 0;
}
static uint16_t dwt_delta_to_uus(uint32_t start, uint32_t end)
{ return dwm3000_driver_dwt_delta_to_uus(start, end); }
static int validate_driver_reply_timing(uint16_t first, uint16_t second, uint16_t expected)
{ return dwm3000_driver_validate_reply_timing(first, second, expected, 1u); }
uint32_t dwt_read32bitoffsetreg(int id, int offset)
{
    assert(id == SYS_STATUS_ID && offset == 0);
    return 0;
}
static int clear_status_checked(uint32_t mask, const char *operation)
{
    assert(mask == SYS_STATUS_TXFRS_BIT_MASK);
    assert(strcmp(operation, "response-tx-complete-clear") == 0);
    now_us += 100;
    return 0;
}
static int write_tx_frame(const uint8_t *bytes, size_t length)
{
    assert(length == (tested_stage == INITIATOR ? UWB_FINAL_LEN : UWB_RESP_LEN) && tx_writes++ == 0);
    memcpy(staged_response, bytes, length);
    staged_length = length;
    now_us += 350;
    return 0;
}
static int patch_tx_frame(const uint8_t *bytes, size_t length, uint16_t offset)
{
    assert(offset == UWB_HEADER_LEN && length ==
        (tested_stage == INITIATOR ? 3u : 2u) * sizeof(uint32_t));
    assert(patches++ == 0 && observed_result->exchange_started);
    memcpy(staged_response + offset, bytes, length);
    now_us += 200;
    return 0;
}
static int start_prepared_range_frame(size_t length, uint8_t mode)
{
    struct uwb_response_frame response;
    assert(length == staged_length);
    assert(mode == (DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED) ||
           (tested_stage == INITIATOR && mode == DWT_START_TX_DELAYED));
    now_us += (int64_t)tx_start_pause_ms * 1000;
    if (request.absolute_deadline_ms != 0u &&
        (int32_t)((programmed_tx_time & UINT32_C(0xfffffffe)) -
                  dwt_readsystimestamphi32()) <= 0) {
        return -ETIME; /* Delayed hardware start cannot fall back to immediate. */
    }
    assert(tx_starts++ == 0 && patches == 1 && observed_result->exchange_started);
    if (tested_stage == INITIATOR) {
        struct uwb_final_frame final;
        assert(uwb_decode_final(staged_response, length, &final) == PROTO_OK);
        assert(dwm3000_driver_header_matches_request(&final.header, &request, MSG_UWB_FINAL));
        assert(final.poll_tx_ts_32 == (uint32_t)poll_tx_timestamp);
        assert(final.resp_rx_ts_32 == (uint32_t)frames[consumed - 1].timestamp);
        assert(programmed_tx_time == delayed_tx_time_from_rx_reference(
            frames[consumed - 1].timestamp, request.reply_delay_uus));
        assert(final.final_tx_ts_32 ==
            (uint32_t)delayed_tx_timestamp_from_programmed_time(programmed_tx_time));
    } else {
    assert(uwb_decode_response(staged_response, length, &response) == PROTO_OK);
    assert(dwm3000_driver_header_matches_request(&response.header, &request, MSG_UWB_RESP));
    /* The response is scheduled from the accepted POLL's RF timestamp,
     * regardless of how many foreign packets preceded it. */
    assert(response.poll_rx_ts_32 == (uint32_t)frames[consumed - 1].timestamp);
    assert(programmed_tx_time == delayed_tx_time_from_rx_reference(
        frames[consumed - 1].timestamp, request.reply_delay_uus));
    assert(response.resp_tx_ts_32 ==
           (uint32_t)delayed_tx_timestamp_from_programmed_time(programmed_tx_time));
    }
    now_us += 300;
    rx_armed = true; /* Hardware RESPONSE_EXPECTED opens the FINAL window. */
    return 0;
}

static int receive_frame(uint32_t timeout_ms, uint32_t *status,
                         uint8_t *buffer, size_t buffer_len, size_t *frame_len,
                         uint64_t *rx_timestamp, uint8_t *quality,
                         int8_t *rsl_dbm, uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                         bool *cir_sampled, int16_t *clock_offset_raw,
                         bool *clock_offset_sampled, int32_t *carrier_integrator,
                         bool *carrier_integrator_sampled,
                         uint64_t *ipatov_rx_timestamp, bool capture_rsl,
                         uint64_t absolute_deadline_ms)
{
    unsigned int window = tx_starts != 0;
    now_us += (int64_t)receive_entry_pause_ms * 1000;
    receive_entry_pause_ms = 0u;
    int64_t deadline = k_uptime_get() + timeout_ms;
    assert(request.absolute_deadline_ms == 0u ? absolute_deadline_ms == 0u :
           absolute_deadline_ms != 0u && absolute_deadline_ms <= request.absolute_deadline_ms);
    if (absolute_deadline_ms != 0u && absolute_deadline_ms < (uint64_t)deadline) {
        deadline = (int64_t)absolute_deadline_ms;
    }
    (void)capture_rsl;
    assert(rx_armed && timeout_ms > 0);
    assert(++receives[window] <= 64);
    if (deadlines[window] == 0) {
        deadlines[window] = deadline;
    }
    assert(deadline == deadlines[window]); /* Never refresh on foreign traffic. */
    if (observed_result != NULL && tested_stage != INITIATOR) {
        assert(observed_result->exchange_started == (tx_starts != 0));
        assert(observed_result->clock_offset_raw == 0);
        assert(!observed_result->clock_offset_sampled);
        assert(observed_result->carrier_integrator == 0);
        assert(!observed_result->carrier_integrator_sampled);
        if (tx_starts == 0) {
            assert(observed_result->initiator_id == 0);
            assert(observed_result->responder_id == 0);
            assert(observed_result->session_id == 0);
        }
    }
    if (k_uptime_get() >= deadline || consumed == frame_count ||
        now_us + (int64_t)frames[consumed].delay_ms * 1000 > deadline * 1000) {
        if (now_us < deadline * 1000) now_us = deadline * 1000;
        *status = SYS_STATUS_RXFTO_BIT_MASK;
        rx_armed = false;
        return -ETIMEDOUT;
    }
    struct physical_frame *frame = &frames[consumed++];
    now_us += (int64_t)frame->delay_ms * 1000;
    frame->timestamp = (uint64_t)now_us * DWM3000_UUS_TO_DWT_TIME;
    now_us += (int64_t)frame->completion_delay_ms * 1000;
    *status = frame->status;
    rx_armed = false;
    if (frame->error != 0) {
        return frame->error;
    }
    if (frame->length > buffer_len) {
        return -EMSGSIZE;
    }
    memcpy(buffer, frame->bytes, frame->length);
    *frame_len = frame->length;
    if (rx_timestamp != NULL) { *rx_timestamp = frame->timestamp; }
    if (quality != NULL) { *quality = 73; }
    if (rsl_dbm != NULL) { *rsl_dbm = -61; }
    if (cir_sample != NULL) { memset(cir_sample, 0x6a, UWB_CIR_SAMPLE_LEN); }
    if (cir_sampled != NULL) { *cir_sampled = true; }
    if (clock_offset_raw != NULL) { *clock_offset_raw = frame->clock_offset; }
    if (clock_offset_sampled != NULL) { *clock_offset_sampled = true; }
    if (carrier_integrator != NULL) { *carrier_integrator = frame->carrier; }
    if (carrier_integrator_sampled != NULL) { *carrier_integrator_sampled = true; }
    if (ipatov_rx_timestamp != NULL) { *ipatov_rx_timestamp = frame->timestamp; }
    return 0;
}

#include "dwm3000_range_filter_receivers.inc"

/* This is the unmodified responder prefix through FINAL acceptance. Its
 * downstream-only locals remain declared even though ranging math is omitted. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "dwm3000_range_filter_responder.inc"
#include "dwm3000_range_filter_initiator.inc"
#pragma GCC diagnostic pop

static struct uwb_range_header expected_header(uint8_t type)
{
    return (struct uwb_range_header) {
        .type = type, .seq = request.seq, .round_index = request.round_index,
        .network_id = request.network_id, .session_id = request.session_id,
        .session_nonce = request.session_nonce,
        .initiator_short_addr = short_addr_from_id(request.initiator_id),
        .responder_short_addr = short_addr_from_id(request.responder_id),
        .flags = request.flags, .initiator_id = request.initiator_id,
        .responder_id = request.responder_id,
    };
}

static struct physical_frame *append_range(uint8_t type, int noise, bool fcs)
{
    assert(frame_count < sizeof(frames) / sizeof(frames[0]));
    struct physical_frame *frame = &frames[frame_count++];
    struct uwb_range_header header = expected_header(type);
    frame->delay_ms = 3;
    frame->status = SYS_STATUS_RXFCG_BIT_MASK;
    frame->timestamp = (UINT64_C(100) + frame_count) * 1000000;
    frame->clock_offset = (int16_t)(100 + frame_count);
    frame->carrier = (int32_t)(2000 + frame_count);
    switch (noise) {
    case OTHER_INITIATOR: header.initiator_id ^= UINT64_C(1) << 40; break;
    case OTHER_TARGET: header.responder_id ^= UINT64_C(1) << 40; break;
    case OTHER_NETWORK: header.network_id++; break;
    case OTHER_SESSION: header.session_id++; break;
    case OTHER_NONCE: header.session_nonce++; break;
    case OTHER_SEQUENCE: header.seq++; break;
    case OTHER_ROUND: header.round_index++; break;
    case OTHER_FLAGS: header.flags = FLAG_DIAGNOSTIC; break;
    default: break;
    }
    int ret;
    switch (type) {
    case MSG_UWB_POLL:
        ret = uwb_encode_poll(&header, frame->bytes, sizeof(frame->bytes), &frame->length);
        break;
    case MSG_UWB_RESP: {
        struct uwb_response_frame response = { .header = header,
            .poll_rx_ts_32 = 100,
            .resp_tx_ts_32 = 100 + UWB_RANGE_REPLY_DELAY_UUS * DWM3000_UUS_TO_DWT_TIME };
        ret = uwb_encode_response(&response, frame->bytes, sizeof(frame->bytes), &frame->length);
        break;
    }
    case MSG_UWB_FINAL: {
        struct uwb_final_frame final = { .header = header,
            .poll_tx_ts_32 = 100, .resp_rx_ts_32 = 200, .final_tx_ts_32 = 300 };
        ret = uwb_encode_final(&final, frame->bytes, sizeof(frame->bytes), &frame->length);
        break;
    }
    default: {
        assert(type == MSG_UWB_REPORT);
        struct uwb_report_frame report = { .header = header, .distance_mm = 2345,
            .quality = 88, .status = RANGE_OK, .rsl_dbm = -64 };
        ret = uwb_encode_report(&report, frame->bytes, sizeof(frame->bytes), &frame->length);
        break;
    }
    }
    assert(ret == PROTO_OK);
    if (noise == CLICK) {
        struct uwb_wake_claim_frame click = {
            .network_id = request.network_id, .clicker_id = UINT64_C(0xc000009999),
            .click_event_id = 1234, .attempt_index = 1, .priority_id = 1,
            .wake_channel = 5, .ranging_channel = 5, .wake_train_ends_in_ms = 400,
            .discovery_starts_in_ms = 420, .claimed_duration_ms = 1000,
            .min_anchor_count = 3, .max_anchor_count = 3, .nonce = 99,
            .flags = FLAG_COUNT_AS_CLICK,
        };
        assert(uwb_encode_wake_claim(&click, frame->bytes, sizeof(frame->bytes),
                                     &frame->length) == PROTO_OK);
        assert(frame->length == 49); /* The escaped 51-byte click includes FCS. */
        fcs = true;
    } else if (noise == TRUNCATED) {
        frame->length = UWB_HEADER_LEN - 1;
    } else if (noise == OVERSIZED) {
        frame->length = sizeof(frame->bytes);
        fcs = false;
    } else if (noise == WRONG_TYPE) {
        frame->bytes[2] = MSG_UWB_DISCOVER;
    }
    if (fcs) {
        memset(frame->bytes + frame->length, 0xee, FCS_LEN);
        frame->length += FCS_LEN;
    }
    return frame;
}

static uint8_t stage_type(enum stage stage)
{
    static const uint8_t types[] = { MSG_UWB_POLL, MSG_UWB_RESP, MSG_UWB_FINAL, MSG_UWB_REPORT };
    return types[stage];
}

static void reset_case(enum stage stage, int64_t start_ms)
{
    memset(frames, 0, sizeof(frames));
    memset(deadlines, 0, sizeof(deadlines));
    memset(receives, 0, sizeof(receives));
    frame_count = consumed = 0;
    now_us = start_ms * 1000;
    request = default_request;
    receive_entry_pause_ms = tx_start_pause_ms = 0u;
    poll_completion_pause_ms = final_completion_pause_ms = 0u;
    poll_starts = phy_configurations = 0u;
    poll_completions = final_completions = 0u;
    observed_send_deadline = poll_tx_timestamp = 0u;
    arms = stops = clears = tx_starts = tx_writes = patches = 0;
    fail_arm = 0;
    fail_clear = false;
    deadline_abort_error = 0;
    rx_armed = stage == RESPONSE || stage == REPORT;
    observed_result = NULL;
    tested_stage = stage;
    if (stage == INITIATOR) request.skip_responder_report = true;
    if (stage == FINAL) {
        append_range(MSG_UWB_POLL, -1, false);
    }
}

static int run_stage(struct dwm3000_range_result *result)
{
    memset(result, 0, sizeof(*result));
    if (tested_stage == INITIATOR) {
        observed_result = result;
        return dwm3000_driver_range_initiator(&request, result);
    }
    if (tested_stage == POLL || tested_stage == FINAL) {
        observed_result = result;
        return responder_poll_once(request.responder_id, &request, request.timeout_ms, result);
    }
    if (tested_stage == REPORT) {
        return receive_report(&request, request.responder_id, result);
    }
    struct uwb_response_frame response;
    uint64_t timestamp = 0;
    uint8_t quality = 0;
    int ret = receive_response(&request, &response, &timestamp, &quality, &result->status);
    if (ret == 0) {
        assert(dwm3000_driver_header_matches_request(&response.header, &request, MSG_UWB_RESP));
        assert(timestamp == frames[consumed - 1].timestamp && quality == 73);
    }
    return ret;
}

static void assert_success(const struct dwm3000_range_result *result)
{
    assert(consumed == frame_count);
    if (tested_stage == POLL || tested_stage == FINAL) {
        assert(tx_starts == 1 && patches == 1 && tx_writes == 1);
        assert(result->exchange_started && result->initiator_id == request.initiator_id);
        assert(result->responder_id == request.responder_id && result->session_id == request.session_id);
        assert(result->seq == request.seq && result->round_index == request.round_index);
        assert(result->clock_offset_sampled && result->carrier_integrator_sampled);
        assert(result->clock_offset_raw == frames[consumed - 1].clock_offset);
        assert(result->carrier_integrator == frames[consumed - 1].carrier);
        assert(result->final_rx_ts_32 == (uint32_t)frames[consumed - 1].timestamp);
    } else {
        assert(tx_starts == 0 && patches == 0 && tx_writes == 0);
        assert(result->status == RANGE_OK);
        if (tested_stage == REPORT) {
            assert(result->distance_mm == 2345 && result->quality == 88 && result->rsl_dbm == -64);
        }
    }
}

static void test_foreign_then_expected(int64_t start_ms)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        for (int noise = -1; noise < NOISE_COUNT; noise++) {
            for (unsigned int fcs = 0; fcs < 2; fcs++) {
                reset_case(stage, start_ms);
                if (noise >= 0) {
                    append_range(stage_type(stage), noise, fcs != 0);
                }
                append_range(stage_type(stage), -1, fcs != 0);
                if (stage == POLL) { append_range(MSG_UWB_FINAL, -1, false); }
                assert(run_stage(&result) == 0);
                assert_success(&result);
                assert(stops == (unsigned int)(noise >= 0));
            }
        }
        reset_case(stage, start_ms);
        append_range(stage_type(stage), CLICK, false);
        append_range(stage_type(stage), OTHER_NONCE, false);
        append_range(stage_type(stage), TRUNCATED, false);
        append_range(stage_type(stage), -1, false);
        if (stage == POLL) { append_range(MSG_UWB_FINAL, -1, false); }
        assert(run_stage(&result) == 0);
        assert_success(&result);
        assert(stops == 3);
    }
}

static void test_noise_timeout(int64_t start_ms)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        reset_case(stage, start_ms);
        for (unsigned int n = 0; n < 40; n++) {
            append_range(stage_type(stage), (int)(n % NOISE_COUNT), false)->delay_ms = 1;
        }
        assert(run_stage(&result) == -ETIMEDOUT);
        assert(result.status == RANGE_RX_TIMEOUT);
        unsigned int window = stage == FINAL;
        assert(deadlines[window] > start_ms);
        assert(now_us >= deadlines[window] * 1000);
        assert(now_us < (deadlines[window] + 1) * 1000);
        assert(consumed < frame_count && receives[window] < 40);
        assert(tx_starts == (unsigned int)(stage == FINAL));
        assert(result.clock_offset_raw == 0 && !result.clock_offset_sampled);
        assert(result.carrier_integrator == 0 && !result.carrier_integrator_sampled);
        if (stage == POLL) {
            assert(!result.exchange_started && result.initiator_id == 0 && result.session_id == 0);
        }
    }
}

static void test_hard_errors(void)
{
    struct dwm3000_range_result result;
    const int errors[] = { -EIO, -ECANCELED, -EHOSTDOWN };
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        for (unsigned int n = 0; n < sizeof(errors) / sizeof(errors[0]); n++) {
            reset_case(stage, 1000);
            struct physical_frame *frame = append_range(stage_type(stage), -1, false);
            frame->error = errors[n];
            frame->status = SYS_STATUS_RXFCE_BIT_MASK;
            append_range(stage_type(stage), -1, false);
            assert(run_stage(&result) < 0);
            assert(result.status == RANGE_RX_ERROR);
            assert(consumed == 1u + (unsigned int)(stage == FINAL));
            assert(stops == 0 && tx_starts == (unsigned int)(stage == FINAL));
        }
        for (unsigned int clear_failure = 0; clear_failure < 2; clear_failure++) {
            reset_case(stage, 1000);
            append_range(stage_type(stage), CLICK, false);
            append_range(stage_type(stage), -1, false);
            fail_clear = clear_failure != 0;
            fail_arm = clear_failure ? 0 : 1u + (unsigned int)(stage == POLL || stage == FINAL);
            assert(run_stage(&result) < 0);
            assert(result.status == RANGE_RX_ERROR);
            assert(consumed == 1u + (unsigned int)(stage == FINAL));
            assert(tx_starts == (unsigned int)(stage == FINAL) && stops == 1);
        }
    }
}

static void test_stale_attempt_starts_no_rf(int64_t start_ms)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= INITIATOR; stage++) {
        for (unsigned expired_by = 0u; expired_by < 2u; expired_by++) {
            reset_case(stage, start_ms);
            request.absolute_deadline_ms = (uint64_t)start_ms - expired_by;
            bool entry = stage == POLL || stage == FINAL || stage == INITIATOR;
            assert(run_stage(&result) == (entry ? -ESTALE : -ETIMEDOUT));
            assert(result.status == RANGE_RX_TIMEOUT);
            assert(arms == 0u && tx_starts == 0u && poll_starts == 0u);
            assert(phy_configurations == 0u && consumed == 0u);
            assert(!result.exchange_started);
        }
    }
}

static void test_absolute_end_caps_every_receive_phase(int64_t start_ms)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        reset_case(stage, start_ms);
        request.absolute_deadline_ms = (uint64_t)start_ms + 20u;
        for (unsigned n = 0u; n < 40u; n++) {
            append_range(stage_type(stage), (int)(n % NOISE_COUNT), false)->delay_ms = 1u;
        }
        assert(run_stage(&result) == -ETIMEDOUT);
        assert(result.status == RANGE_RX_TIMEOUT);
        unsigned window = stage == FINAL;
        assert(deadlines[window] == (int64_t)request.absolute_deadline_ms);
        assert(now_us >= deadlines[window] * 1000);
        assert(now_us < (deadlines[window] + 2) * 1000);
        assert(consumed < frame_count && receives[window] < 40u);
        assert(!result.clock_offset_sampled && !result.carrier_integrator_sampled);
    }
}

static void test_late_software_completion_cannot_accept_expected_frame(int64_t start_ms)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        reset_case(stage, start_ms);
        request.absolute_deadline_ms = (uint64_t)start_ms + 20u;
        append_range(stage_type(stage), -1, false)->completion_delay_ms = 30u;
        assert(run_stage(&result) == -ETIMEDOUT);
        assert(result.status == RANGE_RX_TIMEOUT);
        assert(consumed == 1u + (unsigned)(stage == FINAL));
        assert(tx_starts == (unsigned)(stage == FINAL));
        assert(result.distance_mm == 0);
        assert(!result.clock_offset_sampled && !result.carrier_integrator_sampled);
    }
}

static void test_receive_entry_pause_keeps_original_end(void)
{
    struct dwm3000_range_result result;
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        reset_case(stage, 1000);
        request.absolute_deadline_ms = 1020u;
        /* This pause occurs after the production caller calculated remaining
         * time. The physical boundary must still receive the original end. */
        receive_entry_pause_ms = 25u;
        append_range(stage_type(stage), -1, false);
        assert(run_stage(&result) == -ETIMEDOUT);
        assert(result.status == RANGE_RX_TIMEOUT);
        assert(consumed == 0u && tx_starts == 0u);
        assert(deadlines[0] == 1020);
    }
    reset_case(RESPONSE, 1000);
    request.absolute_deadline_ms = 1055u;
    receive_entry_pause_ms = 25u;
    append_range(MSG_UWB_RESP, -1, false);
    assert(run_stage(&result) == -ETIMEDOUT);
    assert(result.status == RANGE_RX_TIMEOUT && consumed == 0u);
    assert(deadlines[0] == 1023); /* Original phase ends before the attempt. */
}

static void test_scheduled_response_and_final_must_fit_attempt(void)
{
    struct dwm3000_range_result result;
    const enum stage stages[] = {POLL, INITIATOR};
    for (unsigned n = 0u; n < sizeof(stages) / sizeof(stages[0]); n++) {
        for (unsigned late = 0u; late < 2u; late++) {
            reset_case(stages[n], 1000);
            request.absolute_deadline_ms = late ? 1012u : 1055u;
            append_range(stages[n] == POLL ? MSG_UWB_POLL : MSG_UWB_RESP, -1, false);
            if (stages[n] == POLL) append_range(MSG_UWB_FINAL, -1, false);
            int ret = run_stage(&result);
            if (late) {
                assert(ret == -ETIMEDOUT && result.status == RANGE_RX_TIMEOUT);
                assert(tx_starts == 0u && consumed == 1u);
            } else {
                assert(ret == 0 && tx_starts == 1u);
            }
            if (stages[n] == INITIATOR) {
                uint64_t airtime_ms = (dwm3000_timing_airtime_us_ceil(
                    DWM3000_TIMING_PHY_CH5_RANGE, UWB_POLL_LEN) + 999u) / 1000u;
                assert(poll_starts == 1u);
                assert(observed_send_deadline == request.absolute_deadline_ms - airtime_ms);
            }
        }
        reset_case(stages[n], 1000);
        request.absolute_deadline_ms = 1055u;
        tx_start_pause_ms = 60u;
        append_range(stages[n] == POLL ? MSG_UWB_POLL : MSG_UWB_RESP, -1, false);
        assert(run_stage(&result) == -ETIME);
        assert(result.status == RANGE_DELAYED_TX_MISSED);
        assert(tx_starts == 0u && consumed == 1u);
    }
}

static void test_delayed_marker_before_end_cannot_hide_late_frame_tail(void)
{
    const size_t lengths[] = {UWB_RESP_LEN, UWB_FINAL_LEN};
    for (unsigned i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        reset_case(POLL, 1000);
        now_us += 950; /* Exercise the sub-ms phase hidden by integer uptime. */
        request.absolute_deadline_ms = 1010u;
        /* The delayed TX marker is 100 us before the end, but the remaining
         * PHR/data/FCS extend beyond it. Checking only the marker is unsafe. */
        const uint64_t marker_us = UINT64_C(1009900);
        uint64_t tail_us = dwm3000_timing_rctu_to_us_ceil(
            dwm3000_timing_airtime_rctu(DWM3000_TIMING_PHY_CH5_RANGE, lengths[i]) -
            dwm3000_timing_shr_rctu(DWM3000_TIMING_PHY_CH5_RANGE));
        assert(marker_us < request.absolute_deadline_ms * 1000u);
        assert(marker_us + tail_us > request.absolute_deadline_ms * 1000u);
        uint32_t target = (uint32_t)((marker_us * DWM3000_UUS_TO_DWT_TIME) >> 8);
        assert(range_request_start_prepared_frame(&request, lengths[i],
            DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED, target) == -ETIMEDOUT);
        assert(tx_starts == 0u && tx_writes == 0u && patches == 0u);
    }
}

static void test_initiator_completion_cannot_publish_late_success(void)
{
    struct dwm3000_range_result result;
    for (unsigned final = 0u; final < 2u; final++) {
        reset_case(INITIATOR, 1000);
        request.absolute_deadline_ms = 1055u;
        append_range(MSG_UWB_RESP, -1, false);
        if (final) final_completion_pause_ms = 60u;
        else poll_completion_pause_ms = 60u;
        assert(run_stage(&result) == -ETIMEDOUT);
        assert(result.status == RANGE_RX_TIMEOUT && result.exchange_started);
        assert(poll_starts == 1u && poll_completions == 1u);
        assert(tx_starts == final && final_completions == final);
        assert(consumed == final);
    }
}

static void test_deadline_abort_error_is_not_timeout_success(void)
{
    struct dwm3000_range_result result;
    const enum stage entries[] = {POLL, INITIATOR};
    for (unsigned i = 0u; i < sizeof(entries) / sizeof(entries[0]); i++) {
        reset_case(entries[i], 1000);
        request.absolute_deadline_ms = 1000u;
        deadline_abort_error = -EIO;
        assert(run_stage(&result) == -EIO && result.status == RANGE_RX_ERROR);
        assert(tx_starts == 0u && poll_starts == 0u && arms == 0u);
    }
    for (enum stage stage = POLL; stage <= REPORT; stage++) {
        reset_case(stage, 1000);
        request.absolute_deadline_ms = 1020u;
        append_range(stage_type(stage), -1, false)->completion_delay_ms = 30u;
        deadline_abort_error = -EIO;
        assert(run_stage(&result) == (stage == POLL ? -EAGAIN : -EIO));
        assert(result.status != RANGE_RX_TIMEOUT && result.status != RANGE_OK);
        assert(tx_starts == (unsigned)(stage == FINAL));
    }
    reset_case(POLL, 1000);
    request.absolute_deadline_ms = 1000u;
    deadline_abort_error = -ETIMEDOUT; /* A timed-out SPI command isn't quiet RF. */
    assert(run_stage(&result) == -EIO && result.status == RANGE_RX_ERROR);
    assert(arms == 0u && poll_starts == 0u && tx_starts == 0u);
}

int main(void)
{
    /* Run both sides of 32-bit uptime rollover without resetting the clock. */
    const int64_t starts[] = { 1000, (int64_t)UINT32_MAX - 10, (int64_t)UINT32_MAX + 1000 };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        test_foreign_then_expected(starts[i]);
        test_noise_timeout(starts[i]);
        test_stale_attempt_starts_no_rf(starts[i]);
        test_absolute_end_caps_every_receive_phase(starts[i]);
        test_late_software_completion_cannot_accept_expected_frame(starts[i]);
    }
    test_hard_errors();
    test_receive_entry_pause_keeps_original_end();
    test_scheduled_response_and_final_must_fit_attempt();
    test_delayed_marker_before_end_cannot_hide_late_frame_tail();
    test_initiator_completion_cannot_publish_late_success();
    test_deadline_abort_error_is_not_timeout_success();
    puts("DS-TWR: receive filtering, whole-attempt deadlines and delayed RESPONSE/FINAL edges passed");
    return 0;
}
