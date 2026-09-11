/* Execute production survey slot dispatch with fake time and physical radio
 * operations. Driver-level deadline enforcement has separate seam tests. */
#include "app_radio_guard.h"
#include "app_wake_train_politeness.h"
#include "dwm3000_timing.h"
#include "survey_response_lane.h"
#include "uwb_session.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_ID UINT64_C(0x22)
#define NETWORK_ID 9u
#define UWB_CONTROL_TX_TIMEOUT_MS 20u
#define K_MSEC(ms) (ms)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#include "survey_slot_state.inc"

static uint64_t now_ms, jump_ms, setup_delay_ms;
static unsigned claims, releases, sleeps, feeds, recoveries, rx_calls;
static unsigned tx_calls, transmitted, range_calls;
static uint64_t tx_started[8], tx_ended[8], tx_deadlines[8];
static uint8_t range_attempts[SURVEY_RANGE_ATTEMPT_COUNT];
static uint64_t range_deadlines[SURVEY_RANGE_ATTEMPT_COUNT];
static int claim_error, tx_error, rx_error, range_error;
static enum range_status range_status;
static bool live, cancel_raw;
static uint64_t active_wave_start;
static uint32_t abort_mask;
static const uint64_t tx_lead_ms = (DWM3000_DEADLINE_TX_LEAD_UUS + 999u) / 1000u;

static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static void k_sleep(uint64_t delay) { sleeps++; now_ms += delay; }
static void sleep_until_ms(int64_t target)
{
    if (now_ms < (uint64_t)target) now_ms = (uint64_t)target;
    now_ms += jump_ms;
    jump_ms = 0u;
}
static bool anchor_generation_live(uint32_t generation)
{ assert(generation == 10u); return live; }
static int anchor_wait_until(uint32_t generation, uint64_t deadline)
{
    if (!anchor_generation_live(generation)) return -ECANCELED;
    sleep_until_ms((int64_t)deadline);
    return 0;
}
static bool anchor_handle_cancel_raw(const uint8_t *frame, size_t length)
{ (void)frame; (void)length; return cancel_raw; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static void app_watchdog_note_radio_progress(void) { feeds++; }
static int mesh_errno_from_proto(int ret)
{ return ret == PROTO_OK ? 0 : -EINVAL; }
static uint32_t sys_rand32_get(void) { return 0u; }

void dwm3000_driver_request_receive_abort(uint32_t mask) { abort_mask |= mask; }
void dwm3000_driver_clear_receive_abort(uint32_t mask) { abort_mask &= ~mask; }
int radio_guard_uwb_claim(enum radio_guard_uwb_client client,
    const char *reason, struct radio_guard_uwb_lease *lease)
{
    (void)reason;
    assert(client == RADIO_GUARD_UWB_CLIENT_SURVEY);
    claims++;
    if (claim_error != 0) return claim_error;
    lease->client = client;
    lease->generation = 1u;
    return 0;
}
static int anchor_radio_release(struct radio_guard_uwb_lease *lease,
    const char *reason)
{
    (void)reason;
    assert(lease->client == RADIO_GUARD_UWB_CLIENT_SURVEY);
    releases++;
    return 0;
}
int dwm3000_driver_configure_wake_mesh_control_mode(void) { return 0; }
int dwm3000_driver_force_recovery(void) { recoveries++; return 0; }
int dwm3000_driver_send_frame_tracked_until(const uint8_t *frame,
    size_t length, uint32_t timeout, uint64_t deadline,
    struct dwm3000_tx_observation *observation)
{
    (void)frame;
    assert(timeout > 0u && tx_calls < ARRAY_SIZE(tx_deadlines));
    tx_deadlines[tx_calls++] = deadline;
    if (observation != NULL) memset(observation, 0, sizeof(*observation));
    /* A delayed SPI/setup completion is checked by the real driver; model
     * its rejection here and verify the application supplied the right edge. */
    now_ms += setup_delay_ms;
    if (now_ms >= deadline || deadline - now_ms <= tx_lead_ms) return -ETIMEDOUT;
    if (tx_error != 0) return tx_error;
    uint64_t airtime = (dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL, length) + 999u) / 1000u;
    now_ms += tx_lead_ms;
    tx_started[transmitted] = now_ms;
    now_ms += airtime;
    tx_ended[transmitted++] = now_ms;
    if (observation != NULL) {
        observation->rf_started = true;
        observation->tx_completed = true;
    }
    return 0;
}
int dwm3000_driver_receive_frame_continuous(uint32_t timeout,
    uint8_t *frame, size_t capacity, size_t *length, uint8_t *quality,
    int8_t *rsl_dbm, enum dwm3000_rx_failure *failure)
{
    (void)quality;
    (void)rsl_dbm;
    assert(timeout > 0u && capacity > 0u);
    rx_calls++;
    *failure = DWM3000_RX_FAILURE_NONE;
    *length = 0u;
    if (cancel_raw) {
        now_ms++;
        frame[0] = 0xa5;
        *length = 1u;
        return 0;
    }
    if (rx_error != 0) {
        now_ms++;
        return rx_error;
    }
    now_ms += timeout;
    return -ETIMEDOUT;
}
static int anchor_run_response_lane(uint32_t generation, uint8_t batch,
    enum survey_response_kind kind, uint64_t start, uint64_t parent,
    uint8_t hop, uint8_t max_hop, const struct survey_response_record *records,
    uint8_t count)
{
    (void)batch; (void)start; (void)parent; (void)hop; (void)max_hop;
    assert(generation == 10u && kind == SURVEY_RESPONSE_NEIGHBORS);
    assert(records != NULL && count > 0u);
    return 0;
}
static int range_operation(const struct dwm3000_range_request *request,
    uint32_t poll_timeout, bool responder, struct dwm3000_range_result *result)
{
    assert(range_calls < SURVEY_RANGE_ATTEMPT_COUNT);
    uint64_t target = active_wave_start +
        survey_range_attempt_offset_ms(request->round_index);
    assert(now_ms < request->absolute_deadline_ms);
    assert(request->absolute_deadline_ms == target + APP_SURVEY_RANGE_TIMEOUT_MS);
    assert(request->timeout_ms > 0u &&
           request->timeout_ms <= APP_SURVEY_RANGE_TIMEOUT_MS);
    assert(now_ms + request->timeout_ms <= request->absolute_deadline_ms);
    if (responder) {
        assert(poll_timeout <= (request->round_index == 0u ?
            SURVEY_RESPONDER_HEAD_START_MS + APP_SURVEY_RANGE_RX_GUARD_MS :
            APP_SURVEY_RANGE_RX_GUARD_MS + APP_SURVEY_RANGE_TIMEOUT_MS));
        assert(now_ms + poll_timeout <= request->absolute_deadline_ms);
    }
    range_attempts[range_calls] = request->round_index;
    range_deadlines[range_calls++] = request->absolute_deadline_ms;
    if (range_error == -ETIMEDOUT) {
        now_ms += responder ? poll_timeout : request->timeout_ms;
    } else if (range_error == 0) {
        if (now_ms < target) now_ms = target;
        assert(now_ms + 25u <= request->absolute_deadline_ms);
        now_ms += 25u;
    } else {
        now_ms++;
    }
    result->status = range_status;
    result->distance_mm = 1200 + request->round_index;
    return range_error;
}
int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
    struct dwm3000_range_result *result)
{ return range_operation(request, 0u, false, result); }
int dwm3000_driver_responder_poll_expected(uint64_t local,
    const struct dwm3000_range_request *request, uint32_t timeout,
    struct dwm3000_range_result *result)
{ assert(local == DEVICE_ID); return range_operation(request, timeout, true, result); }

#include "survey_slot_production.inc"

static struct app_survey_anchor_state reset(void)
{
    now_ms = 900u;
    active_wave_start = 1000u;
    jump_ms = setup_delay_ms = 0u;
    claims = releases = sleeps = feeds = recoveries = rx_calls = 0u;
    tx_calls = transmitted = range_calls = 0u;
    claim_error = tx_error = rx_error = range_error = 0;
    abort_mask = 0u;
    range_status = RANGE_OK;
    live = true;
    cancel_raw = false;
    memset(tx_started, 0, sizeof(tx_started));
    memset(tx_ended, 0, sizeof(tx_ended));
    return (struct app_survey_anchor_state) {
        .identity = {.generation = 10u, .assignment = {.max_hop_count = 1u}},
        .neighbor_start_ms = 1000u, .slot_span = 1u, .hop_count = 1u,
        .own_slot = 0u, .parent_id = 0x11u,
        .node_ids_by_slot = {DEVICE_ID, 0x33u},
        .plan = {.pair_count = 1u, .pairs = {{.initiator_slot = 0u,
                                            .responder_slot = 1u}}},
    };
}
static void claim_deadline_and_progress(void)
{
    (void)reset();
    struct radio_guard_uwb_lease lease = {0};
    assert(anchor_radio_claim(now_ms, &lease) == -ETIMEDOUT);
    assert(claims == 0u && sleeps == 0u && feeds == 0u && abort_mask == 0u);
    claim_error = -EBUSY;
    uint64_t deadline = now_ms + 5u;
    assert(anchor_radio_claim(deadline, &lease) < 0);
    assert(now_ms == deadline && claims == 3u && sleeps == 3u);
    assert(feeds == 0u && abort_mask == 0u);
}
static void neighbor_slots(void)
{
    for (unsigned scenario = 0u; scenario < 5u; scenario++) {
        struct app_survey_anchor_state state = reset();
        if (scenario == 1u) jump_ms = 520u;
        if (scenario == 2u) jump_ms = 950u;
        if (scenario == 3u) tx_error = -EIO;
        if (scenario == 4u) setup_delay_ms = SURVEY_NEIGHBOR_BEACON_SPACING_MS;
        assert(anchor_neighbor_sequence(&state) == 0);
        unsigned expected = scenario == 0u ? 5u : scenario == 1u ? 3u : 0u;
        assert(transmitted == expected && feeds == expected);
        assert(releases == 1u && abort_mask == 0u);
        for (unsigned tx = 0u; tx < transmitted; tx++) {
            assert(tx_started[tx] >= state.neighbor_start_ms);
            assert(tx_ended[tx] <= state.neighbor_start_ms +
                   SURVEY_NEIGHBOR_SLOT_MS - SURVEY_NEIGHBOR_QUIET_MARGIN_MS);
            assert(tx_ended[tx] <= tx_deadlines[tx] +
                (dwm3000_timing_airtime_us_ceil(
                    DWM3000_TIMING_PHY_CH5_MESH_CONTROL,
                    UWB_SURVEY_PRESENCE_LEN) + 999u) / 1000u);
        }
        if (scenario == 1u) assert(tx_started[0] == 1520u + tx_lead_ms);
        if (scenario == 3u) assert(recoveries == 5u);
    }
    struct app_survey_anchor_state state = reset();
    state.slot_span = 2u;
    rx_error = -EIO;
    assert(anchor_neighbor_sequence(&state) == 0);
    assert(rx_calls > 0u && recoveries == rx_calls);
    assert(feeds == SURVEY_NEIGHBOR_BEACON_COUNT);
    state = reset();
    state.own_slot = 1u;
    state.slot_span = 2u;
    cancel_raw = true;
    assert(anchor_neighbor_sequence(&state) == -ECANCELED);
    assert(feeds == 0u && tx_calls == 0u && releases == 1u);
}
static void pair_slots(void)
{
    for (unsigned responder = 0u; responder < 2u; responder++) {
        for (unsigned scenario = 0u; scenario < 7u; scenario++) {
            struct app_survey_anchor_state state = reset();
            struct survey_range_result result;
            if (scenario == 1u) jump_ms = responder ? 250u : 150u;
            if (scenario == 2u) jump_ms = 1000u;
            if (scenario == 3u) { range_error = -EIO; range_status = RANGE_RX_ERROR; }
            if (scenario == 4u) { range_error = -ETIMEDOUT; range_status = RANGE_RX_TIMEOUT; }
            if (scenario == 5u) { range_error = -ESTALE; range_status = RANGE_RX_TIMEOUT; }
            if (scenario == 6u) { range_error = -EAGAIN; range_status = RANGE_RX_ERROR; }
            int ret = responder ? anchor_run_responder_pair(&state, 0u,
                active_wave_start, &result) : anchor_run_initiator_pair(&state,
                0u, active_wave_start);
            assert(ret == 0);
            unsigned expected = scenario == 1u ? 3u : scenario == 2u ? 0u : 5u;
            assert(range_calls == expected);
            assert(feeds == (scenario == 3u || scenario >= 5u ? 0u : expected));
            for (unsigned call = 0u; call < range_calls; call++) {
                assert(range_attempts[call] == call + (scenario == 1u ? 2u : 0u));
                assert(range_deadlines[call] <= active_wave_start + SURVEY_RANGE_WAVE_MS);
            }
            if (responder && scenario == 0u) assert(result.success_count == 5u);
        }
    }
}
static void response_lane_dispatch_deadline(void)
{
    for (uint8_t count = 1u; count <= SURVEY_RESPONSE_RECORDS_PER_BUNDLE; count++) {
        uint64_t airtime_ms = (dwm3000_timing_airtime_us_ceil(
            DWM3000_TIMING_PHY_CH5_MESH_CONTROL,
            uwb_survey_bundle_encoded_len(count)) + 999u) / 1000u;
        assert(tx_lead_ms + airtime_ms <
               ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
        for (unsigned setup = 0u; setup <= tx_lead_ms; setup++) {
            (void)reset();
            struct survey_response_lane lane;
            struct enumeration_response_timing timing = {.depth = 1u, .round = 0u};
            assert(survey_response_lane_begin(&lane, NETWORK_ID, 10u, DEVICE_ID,
                0x11u, SURVEY_RESPONSE_NEIGHBORS, 0u, 1u, 1u, 1000u) == PROTO_OK);
            for (uint8_t slot = 0u; slot < count; slot++) {
                struct survey_response_record record;
                struct survey_neighbor_report report = {.own_slot = slot};
                assert(survey_neighbor_report_encode(&report, record.bytes) == 8u);
                assert(survey_response_lane_add_record(&lane, &record, NULL) == PROTO_OK);
            }
            assert(survey_response_lane_prepare_round(&lane, 0u, 0u) == PROTO_OK);
            timing.round_offset_ms = lane.round_offsets_ms[0];
            uint64_t slot_start = 1000u + timing.round_offset_ms;
            uint64_t slot_end = slot_start + ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS;
            now_ms = slot_start;
            setup_delay_ms = setup;
            bool fits = setup + tx_lead_ms + airtime_ms <
                ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS;
            int ret = survey_lane_try_tx(&lane, &timing, 1000u);
            assert(ret == (fits ? 1 : -ETIMEDOUT));
            assert(transmitted == (fits ? 1u : 0u));
            assert(tx_deadlines[0] == slot_end - airtime_ms);
            if (fits) {
                assert(tx_started[0] == slot_start + setup + tx_lead_ms);
                assert(tx_ended[0] <= slot_end);
            }
            assert(survey_lane_try_tx(&lane, &timing, 1000u) == 0);
        }
    }
}
int main(void)
{
    claim_deadline_and_progress();
    neighbor_slots();
    pair_slots();
    response_lane_dispatch_deadline();
    puts("survey production slots: stale work skipped, bounded dispatch and functional progress passed");
    return 0;
}
