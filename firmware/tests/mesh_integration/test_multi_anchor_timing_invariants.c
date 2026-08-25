#include "dwm3000_runtime.h"
#include "dwm3000_timing.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "operation_policy.h"
#include "uwb.h"
#include "uwb_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define NETWORK_ID UINT32_C(0x494d4543)
#define CLICKER_ID UINT64_C(0x1111111111111111)
#define SCAN_PERIOD_US ((uint64_t)MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS * 1000u)
#define ENUMERATION_SCAN_STARTUP_US UINT64_C(2500)
#define ENUMERATION_SCAN_PLL_US UINT64_C(170)
#define ENUMERATION_SCAN_SCHEDULER_OWNER_MARGIN_US UINT64_C(100000)
#define ENUMERATION_SCAN_START_TO_START_US \
    (SCAN_PERIOD_US + ENUMERATION_SCAN_STARTUP_US + \
     ENUMERATION_SCAN_PLL_US + MESH_RADIO_ANCHOR_SCAN_RX_US + \
     ENUMERATION_SCAN_SCHEDULER_OWNER_MARGIN_US)
#define ENUMERATION_CONTROL_TURNAROUND_US \
    ((uint64_t)MESH_RADIO_EVENT_RETUNE_GUARD_MS * 1000u)
#define ENUMERATION_CONTROL_OWNER_MARGIN_US UINT64_C(25000)
#define WORKQUEUE_CONTENTION_US UINT64_C(25000)
#define CLOCK_DRIFT_BOUND_US INT32_C(250)
#define PHASE_STEP_US UINT64_C(250)
#define WATCHDOG_LEASE_US UINT64_C(30000000)
#define OLD_DISCOVERY_SLOT_US UINT64_C(1000)

static unsigned int failures;

#define CHECK(condition, message)                                                \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, message);  \
            failures++;                                                          \
        }                                                                        \
    } while (0)

struct interval {
    uint64_t start_us;
    uint64_t end_us;
};


static bool fully_contained(struct interval frame, struct interval rx)
{
    return frame.start_us >= rx.start_us && frame.end_us <= rx.end_us;
}

static void test_channel9_hil_phase_skew_is_inside_receiver_window(void)
{
    enum {
        HIL_PHASE_SKEW_MS = 32u,
        HIL_PHY_PREP_MAX_MS = 20u,
        HIL_FRAME_AIRTIME_MAX_MS = 5u,
        OLD_RETUNE_GUARD_MS = 30u,
        OLD_TX_OFFSET_MS = 15u,
        REQUIRED_C5_OPPORTUNITY_MS = 20u,
    };
    const uint32_t physical_leading_guard_ms =
        MESH_RADIO_EVENT_GUARD_MS > MESH_RADIO_EVENT_RETUNE_GUARD_MS ?
            MESH_RADIO_EVENT_GUARD_MS :
            MESH_RADIO_EVENT_RETUNE_GUARD_MS;
    const uint32_t physical_trailing_guard_ms =
        MESH_RADIO_EVENT_GUARD_MS > MESH_RADIO_EVENT_RX_LATE_GUARD_MS ?
            MESH_RADIO_EVENT_GUARD_MS :
            MESH_RADIO_EVENT_RX_LATE_GUARD_MS;
    const uint32_t physical_rx_reservation_ms =
        physical_leading_guard_ms + MESH_RADIO_EVENT_WINDOW_MS +
        physical_trailing_guard_ms;
    const uint32_t peer_phase_spacing_ms =
        MESH_RADIO_EVENT_INTERVAL_MS / 2u;
    const int32_t old_receiver_armed_ms =
        -(int32_t)OLD_RETUNE_GUARD_MS + HIL_PHY_PREP_MAX_MS;
    const int32_t old_frame_start_ms =
        -(int32_t)HIL_PHASE_SKEW_MS + OLD_TX_OFFSET_MS;
    const int32_t old_frame_end_ms =
        old_frame_start_ms + HIL_FRAME_AIRTIME_MAX_MS;
    const int32_t receiver_armed_ms =
        -(int32_t)MESH_RADIO_EVENT_RETUNE_GUARD_MS + HIL_PHY_PREP_MAX_MS;
    const int32_t frame_start_ms =
        -(int32_t)HIL_PHASE_SKEW_MS + MESH_RADIO_EVENT_TX_OFFSET_MS;
    const int32_t frame_end_ms =
        frame_start_ms + HIL_FRAME_AIRTIME_MAX_MS;

    CHECK(old_frame_end_ms <= old_receiver_armed_ms,
          "old channel-9 geometry no longer reproduces the missed HIL ACK");
    CHECK(frame_start_ms >= receiver_armed_ms,
          "channel-9 receiver is not armed before the skewed peer transmits");
    CHECK(frame_end_ms <=
              (int32_t)(MESH_RADIO_EVENT_WINDOW_MS +
                        MESH_RADIO_EVENT_RX_LATE_GUARD_MS),
          "skewed channel-9 frame extends beyond the receiver deadline");
    CHECK(MESH_RADIO_EVENT_GUARD_MS >= MESH_RADIO_EVENT_RETUNE_GUARD_MS &&
              MESH_RADIO_EVENT_GUARD_MS >=
                  MESH_RADIO_EVENT_RX_LATE_GUARD_MS,
          "negotiated channel-9 guard does not cover physical RX ownership");
    CHECK(physical_rx_reservation_ms <= peer_phase_spacing_ms,
          "channel-9 physical RX ownership overlaps the second peer slot");
    CHECK(physical_rx_reservation_ms + MESH_RADIO_EVENT_RETUNE_GUARD_MS +
              REQUIRED_C5_OPPORTUNITY_MS <= peer_phase_spacing_ms,
          "two channel-9 phases do not leave retune plus 20 ms for channel 5");
}

static uint64_t runtime_prepare_rx_us(enum dwm3000_timing_phy phy)
{
    struct dwm3000_runtime runtime;
    struct dwm3000_runtime_interval interval;

    dwm3000_runtime_init(&runtime);
    CHECK(dwm3000_runtime_prepare_phy(&runtime, phy, 0u, &interval) ==
              DWM3000_RUNTIME_OK,
          "production PHY prepare sequence failed");
    CHECK(dwm3000_runtime_arm_rx(&runtime,
                                 interval.end_us,
                                 interval.end_us + 1000u,
                                 &interval) == DWM3000_RUNTIME_OK,
          "production RX arm sequence failed");
    return interval.start_us;
}

static bool wake_train_has_contained_claim(uint64_t phase_us,
                                           int32_t drift_us,
                                           uint32_t jitter_us)
{
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_WAKE_CLAIM_LEN);
    const uint64_t preamble_us = dwm3000_timing_rctu_to_us_ceil(
        dwm3000_timing_preamble_rctu(DWM3000_TIMING_PHY_CH5_WAKE));
    const uint64_t rx_arm_us = runtime_prepare_rx_us(DWM3000_TIMING_PHY_CH5_WAKE);
    uint64_t tx_us = 0u;

    CHECK(rx_arm_us > 0u, "wake RX preparation cost was not modeled");

    while (tx_us < (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u) {
        int64_t shifted = (int64_t)tx_us + drift_us;
        struct interval frame;

        if (shifted >= 0) {
            frame.start_us = (uint64_t)shifted;
            frame.end_us = frame.start_us + airtime_us;
            /* Preamble detection extends this same RX operation through CRC. */
            for (uint8_t scan = 0u; scan < 2u; scan++) {
                struct interval rx = {
                    .start_us = phase_us + (uint64_t)scan * SCAN_PERIOD_US,
                    .end_us = phase_us + (uint64_t)scan * SCAN_PERIOD_US +
                              MESH_RADIO_ANCHOR_SCAN_RX_US,
                };

                struct interval preamble = {
                    frame.start_us,
                    frame.start_us + preamble_us,
                };

                if (preamble.start_us < rx.end_us && preamble.end_us > rx.start_us) {
                    return true;
                }
            }
        }
        tx_us += airtime_us + jitter_us;
    }
    return false;
}

static bool enumeration_activation_contains_claim_and_followup(
    uint64_t phase_us)
{
    const uint64_t claim_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_WAKE_CLAIM_LEN);
    const uint64_t claim_preamble_us = dwm3000_timing_rctu_to_us_ceil(
        dwm3000_timing_preamble_rctu(DWM3000_TIMING_PHY_CH5_WAKE));
    const uint64_t control_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL, UWB_MESH_MAX_FRAME_LEN);
    const uint64_t control_prepare_us = runtime_prepare_rx_us(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    const uint64_t claim_spacing_us = claim_airtime_us +
        UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US;
    const uint64_t train_end_us =
        (uint64_t)MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS * 1000u;
    const struct interval control = {
        .start_us = train_end_us + ENUMERATION_CONTROL_TURNAROUND_US +
                    ENUMERATION_CONTROL_OWNER_MARGIN_US,
        .end_us = train_end_us + ENUMERATION_CONTROL_TURNAROUND_US +
                  ENUMERATION_CONTROL_OWNER_MARGIN_US + control_airtime_us,
    };

    for (uint8_t scan = 0u; scan < 3u; scan++) {
        const struct interval rx = {
            .start_us = phase_us +
                        (uint64_t)scan *
                            ENUMERATION_SCAN_START_TO_START_US,
            .end_us = phase_us +
                      (uint64_t)scan *
                          ENUMERATION_SCAN_START_TO_START_US +
                      MESH_RADIO_ANCHOR_SCAN_RX_US,
        };
        const uint64_t nearest_claim = rx.start_us / claim_spacing_us;
        const uint64_t first_candidate = nearest_claim > 0u ?
            nearest_claim - 1u : 0u;

        /* Only claims adjacent to the RX start can overlap this short slice. */
        for (uint64_t candidate = first_candidate;
             candidate <= nearest_claim + 1u;
             candidate++) {
            const uint64_t claim_start_us = candidate * claim_spacing_us;
            const struct interval preamble = {
                .start_us = claim_start_us,
                .end_us = claim_start_us + claim_preamble_us,
            };

            if (claim_start_us < train_end_us &&
                preamble.start_us < rx.end_us &&
                preamble.end_us > rx.start_us) {
                const struct interval listener = {
                    .start_us = claim_start_us + claim_airtime_us +
                                control_prepare_us,
                    .end_us = claim_start_us + claim_airtime_us +
                              (uint64_t)
                                  discovery_assignment_control_listener_duration_ms(
                                      1u) * 1000u,
                };

                return fully_contained(control, listener);
            }
        }
    }
    return false;
}



static void add_candidate(struct uwb_clicker_session *session,
                          uint64_t anchor_id,
                          uint8_t assigned_slot,
                          uint8_t quality)
{
    struct uwb_discovery_reply_frame reply = {
        .network_id = NETWORK_ID,
        .anchor_id = anchor_id,
        .selected_clicker_id = CLICKER_ID,
        .click_event_id = session->config.click_event_id,
        .attempt_index = session->attempt_index,
        .nonce = session->config.nonce,
        .anchor_slot = assigned_slot,
        .status = UWB_DISCOVERY_REPLY_PRESENT,
        .rx_quality = quality,
        .flags = session->config.flags,
    };

    CHECK(uwb_clicker_note_discovery_reply(session, &reply) == PROTO_OK,
          "valid distinct discovery claim was rejected");
}

static void test_multi_anchor_claim_and_range_schedule_invariants(void)
{
    for (uint8_t anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS;
         anchor_count <= 6u;
         anchor_count++) {
        for (uint8_t permutation = 0u; permutation < anchor_count; permutation++) {
            struct uwb_clicker_session session;
            struct uwb_clicker_config config = {
                .network_id = NETWORK_ID,
                .clicker_id = CLICKER_ID,
                .click_event_id = UINT32_C(0x12340000) + anchor_count,
                .nonce = UINT64_C(0xabcdef0000000000) + permutation,
                .min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS,
                .max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS,
                .max_attempts = 1u,
                .samples_per_anchor = 1u,
                .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
                .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
                .flags = FLAG_COUNT_AS_CLICK,
            };
            struct uwb_discover_frame discover;
            struct uwb_range_schedule_frame schedule;
            bool scheduled[6] = {false};

            CHECK(uwb_clicker_session_start(&session, &config) == PROTO_OK,
                  "clicker session start failed");
            session.state = UWB_CLICKER_WAKE;
            CHECK(uwb_clicker_build_discover(&session, &discover) == PROTO_OK,
                  "discover build failed");
            for (uint8_t i = 0u; i < anchor_count; i++) {
                uint8_t index = (uint8_t)((i + permutation) % anchor_count);
                add_candidate(&session,
                              UINT64_C(0xa700000000000000) + index + 1u,
                              index,
                              (uint8_t)(80u + index));
            }
            add_candidate(&session,
                          UINT64_C(0xa700000000000001),
                          0u,
                          100u);
            CHECK(session.candidate_count == anchor_count,
                  "duplicate claim changed unique candidate count");
            CHECK(uwb_clicker_build_range_schedule(
                      &session,
                      UWB_DS_TWR_REPLY_DELAY_US,
                      50u,
                      UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                      &schedule) == PROTO_OK,
                  "multi-anchor range schedule build failed");
            CHECK(schedule.selected_count == anchor_count,
                  "range schedule silently dropped a discovered anchor");
            CHECK(schedule.exchange_stride_us ==
                      UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
                  "multi-anchor spacing path mismatch");
            CHECK(schedule.exchange_stride_us == 50000u,
                  "multi-anchor exchange stride is shorter than the hardware handoff");
            CHECK(schedule.burst_window_ms == 400u,
                  "multi-anchor schedule no longer uses the shared 400 ms burst");
            CHECK(schedule.max_exchanges == 8u,
                  "400 ms burst does not contain exactly eight 50 ms exchanges");
            CHECK((uint32_t)schedule.max_exchanges *
                          schedule.exchange_stride_us ==
                      (uint32_t)schedule.burst_window_ms * 1000u,
                  "multi-anchor exchange capacity does not exactly fill its burst");
            for (uint8_t i = 0u; i < schedule.selected_count; i++) {
                uint8_t index = (uint8_t)(schedule.entries[i].anchor_id -
                                          UINT64_C(0xa700000000000001));
                CHECK(index < anchor_count && !scheduled[index],
                      "anchor scheduled twice or outside discovered set");
                if (index < anchor_count) {
                    scheduled[index] = true;
                }
            }
            for (uint8_t i = 0u; i < anchor_count; i++) {
                CHECK(scheduled[i], "discovered anchor was not scheduled");
            }
        }
    }
}

static uint8_t simulate_discovery_replies(uint8_t anchor_count,
                                          uint64_t slot_spacing_us,
                                          uint32_t workqueue_jitter_us,
                                          int32_t clock_drift_us)
{
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_DISCOVERY_REPLY_LEN);
    struct interval replies[6];
    bool collided[6] = {false};
    uint8_t accepted = 0u;

    for (uint8_t i = 0u; i < anchor_count; i++) {
        int64_t start_us = (int64_t)i * (int64_t)slot_spacing_us +
                           (int64_t)workqueue_jitter_us + clock_drift_us;

        CHECK(start_us >= 0, "discovery reply started before the RX window");
        replies[i].start_us = start_us < 0 ? 0u : (uint64_t)start_us;
        replies[i].end_us = replies[i].start_us + airtime_us;
    }
    for (uint8_t i = 0u; i < anchor_count; i++) {
        for (uint8_t other = (uint8_t)(i + 1u); other < anchor_count; other++) {
            if (replies[i].start_us < replies[other].end_us &&
                replies[other].start_us < replies[i].end_us) {
                collided[i] = true;
                collided[other] = true;
            }
        }
        if (!collided[i]) {
            accepted++;
        }
    }
    return accepted;
}

static void test_discovery_collision_sweep_and_old_spacing_sensitivity(void)
{
    bool old_spacing_failed = false;

    for (uint8_t anchor_count = 1u; anchor_count <= 6u; anchor_count++) {
        for (uint32_t jitter_us = 0u; jitter_us <= 500u; jitter_us += 250u) {
            for (int32_t drift_us = 0; drift_us <= CLOCK_DRIFT_BOUND_US;
                 drift_us += CLOCK_DRIFT_BOUND_US) {
                uint8_t old = simulate_discovery_replies(
                    anchor_count, OLD_DISCOVERY_SLOT_US, jitter_us, drift_us);
                uint8_t fixed = simulate_discovery_replies(
                    anchor_count, MESH_RADIO_DISCOVERY_SLOT_US,
                    jitter_us, drift_us);

                CHECK(fixed == anchor_count,
                      "production discovery spacing dropped a reachable anchor");
                if (old != anchor_count) {
                    old_spacing_failed = true;
                }
            }
        }
    }
    CHECK(old_spacing_failed,
          "1 ms discovery-slot mutation did not reproduce winner-takes-all");
}








static void test_claim_phase_sweep(void)
{
    static const int32_t drifts[] = {
        -CLOCK_DRIFT_BOUND_US, 0, CLOCK_DRIFT_BOUND_US,
    };

    for (uint64_t phase_us = 0u; phase_us < SCAN_PERIOD_US;
         phase_us += PHASE_STEP_US) {
        for (size_t drift = 0u; drift < ARRAY_SIZE(drifts); drift++) {
            CHECK(wake_train_has_contained_claim(phase_us,
                                                 drifts[drift],
                                                 UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US),
                  "wake train phase sweep lacked a fully contained claim");
        }
    }
}

static void test_enumeration_activation_phase_sweep(void)
{
    CHECK(2u * ENUMERATION_SCAN_START_TO_START_US <
              (uint64_t)MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS *
                  1000u,
          "enumeration activation train no longer covers two delayed scans");
    CHECK(MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS == 1000u,
          "enumeration activation no longer uses the protocol maximum");

    for (uint64_t phase_us = 0u;
         phase_us < ENUMERATION_SCAN_START_TO_START_US;
         phase_us += PHASE_STEP_US) {
        CHECK(enumeration_activation_contains_claim_and_followup(phase_us),
              "enumeration activation missed wake or extended CLAIM containment");
    }
}

static void test_depth_aware_enumeration_control_listener(void)
{
    const uint32_t old_fixed_listener_ms = 2000u;
    uint32_t previous_ms = 0u;

    CHECK(discovery_assignment_control_listener_duration_ms(1u) == 3750u,
          "direct control listener lost its two-second safety margin");
    CHECK(discovery_assignment_control_listener_duration_ms(2u) == 4290u,
          "one-relay control listener does not cover its route depth");
    CHECK(discovery_assignment_control_listener_duration_ms(3u) == 4830u,
          "two-relay control listener does not cover F2F1D");
    CHECK(discovery_assignment_control_listener_duration_ms(0u) == 7530u,
          "unknown route depth did not fail safe to the maximum window");
    CHECK(old_fixed_listener_ms <
              MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS +
                  MESH_RADIO_EVENT_RETUNE_GUARD_MS +
                  discovery_assignment_control_propagation_hold_ms(3u),
          "old fixed listener no longer reproduces the F2 timing gap");

    for (uint8_t hop_count = 1u;
         hop_count <= DISCOVERY_ASSIGNMENT_MAX_HOPS;
         hop_count++) {
        uint32_t listen_ms =
            discovery_assignment_control_listener_duration_ms(hop_count);
        uint32_t required_ms =
            MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS +
            MESH_RADIO_EVENT_RETUNE_GUARD_MS +
            discovery_assignment_control_propagation_hold_ms(hop_count);

        CHECK(listen_ms == required_ms +
                               DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_REDUNDANCY_MS,
              "depth-aware listener lost its explicit safety margin");
        CHECK(listen_ms > previous_ms,
              "deeper gateway route did not lengthen the listener");
        previous_ms = listen_ms;
    }
}

static void test_maintained_normal_click_phy_and_capacity_contract(void)
{
    const struct dwm3000_phy_timing *wake =
        dwm3000_timing_phy_profile(DWM3000_TIMING_PHY_CH5_WAKE);
    const struct dwm3000_phy_timing *range =
        dwm3000_timing_phy_profile(DWM3000_TIMING_PHY_CH5_RANGE);

    CHECK(wake != NULL && range != NULL,
          "production channel-5 PHY profiles are unavailable");
    if (wake != NULL && range != NULL) {
        CHECK(wake->pac_symbols == 32u && range->pac_symbols == 32u,
              "production channel-5 PHY must use PAC32");
        CHECK(wake->sfd_timeout_symbols == 4073u &&
                  range->sfd_timeout_symbols == 4073u,
              "production channel-5 PHY must use the documented 4073-symbol SFD timeout");
    }
    CHECK(UWB_NORMAL_CLICK_MAX_ANCHORS == 4u,
          "normal click schedules must cap selection at four anchors");
    CHECK(UWB_RANGE_SCHEDULE_MAX_ANCHORS == 8u,
          "the generic schedule wire capacity must remain eight anchors");
    CHECK(UWB_NORMAL_CLICK_MAX_ANCHORS <= UWB_RANGE_SCHEDULE_MAX_ANCHORS,
          "normal click capacity must fit the generic schedule frame");
    CHECK(UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US == 50000u,
          "multi-anchor DS-TWR exchanges need a 50 ms minimum stride");
    CHECK(UWB_RANGE_SCHEDULE_SINGLE_ANCHOR_MIN_EXCHANGE_STRIDE_US ==
              UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
          "single- and multi-anchor schedules must share the safe stride");
    CHECK(UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS == 400u,
          "normal clicks must retain the 400 ms ranging burst");
    CHECK(((uint32_t)UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS * 1000u) /
                  UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US ==
              8u,
          "the maintained 400 ms burst must hold eight safe exchanges");
}

int main(void)
{
    CHECK(SCAN_PERIOD_US == UINT64_C(380000),
          "test is not using the production low-duty scan period");
    CHECK(MESH_RADIO_WAKE_TRAIN_MS == 500u,
          "ordinary production wake trains must remain at least 500 ms");
    CHECK(MESH_RADIO_ANCHOR_SCAN_RX_US == 3500u,
          "test is not using the production scan window");
    CHECK(dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_WAKE,
                                         UWB_WAKE_CLAIM_LEN) > 0u,
          "wake claim airtime unavailable");
    test_maintained_normal_click_phy_and_capacity_contract();
    test_channel9_hil_phase_skew_is_inside_receiver_window();
    test_claim_phase_sweep();
    test_enumeration_activation_phase_sweep();
    test_depth_aware_enumeration_control_listener();
    test_discovery_collision_sweep_and_old_spacing_sensitivity();
    test_multi_anchor_claim_and_range_schedule_invariants();

    if (failures != 0u) {
        fprintf(stderr, "RESULT multi_anchor_timing_invariants failures=%u\n",
                failures);
        return EXIT_FAILURE;
    }
    printf("PASS multi_anchor_timing_invariants\n");
    return EXIT_SUCCESS;
}
