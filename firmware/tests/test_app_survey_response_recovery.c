/* Execute the production response lane across invalidating radio failures.
 * The driver wrapper's stale-status classification has a separate seam test. */
#include "app_wake_train_politeness.h"
#include "dwm3000_timing.h"
#include "survey_response_lane.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_ID UINT64_C(0x22)
#define NETWORK_ID 9u
#define UWB_CONTROL_TX_TIMEOUT_MS 20u
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum fault_site { FAULT_RECEIVE, FAULT_BUNDLE_TX, FAULT_ACK_TX };
static enum fault_site site;
static uint64_t now_ms, lane_start_ms, recovery_delay_ms;
static unsigned phy, configurations, recoveries, receives, feeds, feeds_at_fault;
static int recovery_error, configure_error;
static bool live, faulted, received_after_fault;

static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static void sleep_until_ms(int64_t target)
{ if (now_ms < (uint64_t)target) now_ms = (uint64_t)target; }
static bool anchor_generation_live(uint32_t generation)
{ assert(generation == 10u); return live; }
static bool anchor_handle_cancel_raw(const uint8_t *frame, size_t length)
{ (void)frame; (void)length; return false; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static void app_watchdog_note_radio_progress(void) { feeds++; }
static int mesh_errno_from_proto(int ret)
{ assert(ret != PROTO_OK); return -EINVAL; }
static uint32_t sys_rand32_get(void) { return 0u; }

int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    configurations++;
    if (configurations > 1u) {
        assert(faulted && feeds == feeds_at_fault);
        if (site != FAULT_RECEIVE) {
            assert(phy == 0u);
            now_ms += recovery_delay_ms;
        }
    }
    if (configurations > 1u && configure_error != 0) return configure_error;
    phy = 4u;
    if (configurations > 1u) now_ms += 3u;
    return 0;
}
int dwm3000_driver_force_recovery(void)
{
    recoveries++;
    assert(faulted && phy == 0u);
    assert(feeds == feeds_at_fault);
    now_ms += recovery_delay_ms;
    if (recovery_error != 0) return recovery_error;
    /* Generic recovery configures the ranging PHY. The lane must explicitly
     * restore its control PHY before resuming receive. */
    phy = 1u;
    return 0;
}
static int invalidate_radio(void)
{
    assert(!faulted);
    faulted = true;
    feeds_at_fault = feeds;
    phy = 0u;
    now_ms++;
    return -EIO;
}
int dwm3000_driver_send_frame_tracked_until(const uint8_t *frame,
    size_t length, uint32_t timeout, uint64_t deadline,
    struct dwm3000_tx_observation *observation)
{
    (void)observation;
    assert(phy == 4u && timeout > 0u && now_ms < deadline);
    if (!faulted && site == FAULT_BUNDLE_TX) {
        struct survey_response_bundle bundle;
        assert(uwb_decode_survey_bundle(frame, length, &bundle) == PROTO_OK);
        return invalidate_radio();
    }
    if (!faulted && site == FAULT_ACK_TX) {
        struct survey_response_hop_ack ack;
        assert(uwb_decode_survey_hop_ack(frame, length, &ack) == PROTO_OK);
        return invalidate_radio();
    }
    /* A recovery can cross a round edge and make a new retained-bundle
     * attempt due before the first resumed RX. It still needs the control
     * PHY and cannot renew the original response schedule. */
    assert(faulted && configurations == 2u && feeds == feeds_at_fault);
    now_ms++;
    return 0;
}
int dwm3000_driver_receive_frame_continuous(uint32_t timeout,
    uint8_t *frame, size_t capacity, size_t *length, uint8_t *quality,
    int8_t *rsl_dbm, enum dwm3000_rx_failure *failure)
{
    (void)quality;
    (void)rsl_dbm;
    assert(phy == 4u && timeout > 0u);
    uint64_t round_end = now_ms -
        ((now_ms - lane_start_ms) % ENUMERATION_RESPONSE_ROUND_MS) +
        ENUMERATION_RESPONSE_ROUND_MS;
    assert(now_ms + timeout <= round_end);
    receives++;
    *length = 0u;
    *failure = DWM3000_RX_FAILURE_NONE;
    if (faulted) {
        assert(recoveries == (site == FAULT_RECEIVE ? 1u : 0u));
        assert(configurations == 2u);
        assert(feeds == feeds_at_fault);
        /* Recovery consumes the original lane schedule. No new relative
         * receive budget may extend the remainder of its current round. */
        assert(now_ms + timeout == round_end);
        received_after_fault = true;
        live = false;
        return -ECANCELED;
    }
    if (site == FAULT_RECEIVE) return invalidate_radio();
    if (site == FAULT_ACK_TX) {
        struct survey_response_bundle bundle = {
            .network_id = NETWORK_ID, .generation = 10u,
            .sender_id = 0x33u, .parent_id = DEVICE_ID,
            .kind = SURVEY_RESPONSE_NEIGHBORS, .record_count = 1u,
        };
        struct survey_neighbor_report report = {.own_slot = 2u};
        assert(survey_neighbor_report_encode(&report, bundle.records[0].bytes) == 8u);
        assert(uwb_encode_survey_bundle(&bundle, frame, capacity, length) == PROTO_OK);
        now_ms += 2u;
        return 0;
    }
    now_ms += timeout;
    return -ETIMEDOUT;
}

#include "survey_response_recovery.inc"

static void response_fault_restores_owned_phy(void)
{
    for (unsigned fault = FAULT_RECEIVE; fault <= FAULT_ACK_TX; fault++) {
        for (unsigned recovery_case = 0u; recovery_case < 5u; recovery_case++) {
            site = (enum fault_site)fault;
            if (site != FAULT_RECEIVE && recovery_case == 2u) continue;
            now_ms = 900u;
            lane_start_ms = 1000u;
            recovery_delay_ms = recovery_case == 1u ? 150u : 9u;
            if (recovery_case == 4u) {
                recovery_delay_ms = enumeration_response_duration_ms(2u) + 5u;
            }
            recovery_error = recovery_case == 2u ? -EIO : 0;
            configure_error = recovery_case == 3u ? -ENODEV : 0;
            phy = configurations = recoveries = receives = feeds = feeds_at_fault = 0u;
            live = true;
            faulted = received_after_fault = false;
            struct survey_response_record record;
            struct survey_neighbor_report report = {.own_slot = 0u};
            assert(survey_neighbor_report_encode(&report, record.bytes) == 8u);
            int ret = anchor_run_response_lane(10u, 0u, SURVEY_RESPONSE_NEIGHBORS,
                lane_start_ms, 0x11u, 1u, site == FAULT_ACK_TX ? 2u : 1u,
                &record, site == FAULT_BUNDLE_TX ? 1u : 0u);
            assert(faulted && recoveries == (site == FAULT_RECEIVE ? 1u : 0u));
            assert(feeds == feeds_at_fault);
            if (recovery_case < 2u) {
                assert(ret == -ECANCELED && received_after_fault);
                assert(phy == 4u && configurations == 2u);
            } else if (recovery_case < 4u) {
                assert(ret < 0 && !received_after_fault);
                assert(configurations == (recovery_case == 2u ? 1u : 2u));
            } else {
                assert(ret == 0 && !received_after_fault);
                assert(configurations == 2u && phy == 4u);
                assert(enumeration_response_lane_complete_depth(
                    lane_start_ms, now_ms, site == FAULT_ACK_TX ? 2u : 1u));
            }
        }
    }
}

int main(void)
{
    response_fault_restores_owned_phy();
    puts("survey response recovery: invalid RX/TX restores control PHY without false progress or deadline renewal");
    return 0;
}
