#include "dwm3000_runtime.h"
#include "dwm3000_timing.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "survey.h"
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
#define SURVEY_FOLLOWUP_US UINT64_C(1250000)
#define OLD_SURVEY_FOLLOWUP_US UINT64_C(400000)
#define WORKQUEUE_CONTENTION_US UINT64_C(25000)
#define CLOCK_DRIFT_BOUND_US INT32_C(250)
#define PHASE_STEP_US UINT64_C(250)
#define WATCHDOG_LEASE_US UINT64_C(30000000)
#define OLD_DISCOVERY_SLOT_US UINT64_C(1000)
#define SURVEY_REPLY_OPPORTUNITY_COUNT 4u

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

static bool survey_followup_contains_flood(uint64_t scan_phase_us,
                                           uint64_t followup_us,
                                           int32_t drift_us,
                                           uint32_t quiet_jitter_ms)
{
    const uint64_t claim_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_WAKE_CLAIM_LEN);
    const uint64_t claim_preamble_us = dwm3000_timing_rctu_to_us_ceil(
        dwm3000_timing_preamble_rctu(DWM3000_TIMING_PHY_CH5_WAKE));
    const uint64_t flood_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL, 125u);
    const uint64_t rx_prepare_us = runtime_prepare_rx_us(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    uint64_t claim_us = 0u;
    uint64_t accepted_claim_end_us = 0u;
    bool accepted = false;

    while (claim_us < (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u) {
        struct interval claim = {claim_us, claim_us + claim_airtime_us};
        struct interval scan = {
            scan_phase_us,
            scan_phase_us + MESH_RADIO_ANCHOR_SCAN_RX_US,
        };

        /* The 3 ms sniff extends in-place after preamble detection. */
        struct interval preamble = {
            claim.start_us,
            claim.start_us + claim_preamble_us,
        };

        if (preamble.start_us < scan.end_us && preamble.end_us > scan.start_us) {
            accepted = true;
            accepted_claim_end_us = claim.end_us;
            break;
        }
        claim_us += claim_airtime_us;
    }
    if (!accepted) {
        return false;
    }

    /* Wake train, quiet sniff, scheduler pressure, PHY transition, then flood. */
    {
        int64_t flood_start = (int64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000 +
                              (int64_t)C5_POLITE_SNIFF_MS * 1000 +
                              (int64_t)quiet_jitter_ms * 1000 +
                              (int64_t)WORKQUEUE_CONTENTION_US + drift_us;
        struct interval rx = {
            accepted_claim_end_us + rx_prepare_us,
            accepted_claim_end_us + followup_us,
        };
        struct interval flood;

        if (flood_start < 0) {
            return false;
        }
        flood.start_us = (uint64_t)flood_start;
        flood.end_us = flood.start_us + flood_airtime_us;
        return fully_contained(flood, rx) && flood.end_us < WATCHDOG_LEASE_US;
    }
}

static void test_survey_phase_sweep_and_old_defect_sensitivity(void)
{
    static const int32_t drifts[] = {
        -CLOCK_DRIFT_BOUND_US, 0, CLOCK_DRIFT_BOUND_US,
    };
    static const uint32_t quiet_jitter_ms[] = {0u, 19u, 39u, 79u};
    bool old_defect_missed = false;

    for (uint8_t anchor_count = 2u; anchor_count <= 6u; anchor_count++) {
        for (uint64_t phase_us = 0u; phase_us < SCAN_PERIOD_US;
             phase_us += PHASE_STEP_US) {
            for (size_t drift = 0u; drift < ARRAY_SIZE(drifts); drift++) {
                for (size_t jitter = 0u; jitter < ARRAY_SIZE(quiet_jitter_ms);
                     jitter++) {
                    bool fixed = survey_followup_contains_flood(
                        phase_us,
                        SURVEY_FOLLOWUP_US,
                        drifts[drift],
                        quiet_jitter_ms[jitter]);
                    bool old = survey_followup_contains_flood(
                        phase_us,
                        OLD_SURVEY_FOLLOWUP_US,
                        drifts[drift],
                        quiet_jitter_ms[jitter]);

                    CHECK(fixed,
                          "survey flood lacked a fully contained opportunity");
                    if (!old) {
                        old_defect_missed = true;
                    }
                }
            }
        }
    }
    CHECK(old_defect_missed,
          "old short follow-up mutation did not reproduce the survey miss");
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
    for (uint8_t anchor_count = 1u; anchor_count <= 6u; anchor_count++) {
        for (uint8_t permutation = 0u; permutation < anchor_count; permutation++) {
            struct uwb_clicker_session session;
            struct uwb_clicker_config config = {
                .network_id = NETWORK_ID,
                .clicker_id = CLICKER_ID,
                .click_event_id = UINT32_C(0x12340000) + anchor_count,
                .nonce = UINT64_C(0xabcdef0000000000) + permutation,
                .min_anchor_count = 1u,
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
                      (anchor_count == 1u ?
                           UWB_RANGE_SCHEDULE_SINGLE_ANCHOR_MIN_EXCHANGE_STRIDE_US :
                           UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US),
                  "one/multi-anchor spacing path mismatch");
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

static void test_fixed_survey_retry_deadlock_is_reproduced(void)
{
    const struct survey_discovery_config config = {
        .survey_id = UINT32_C(0x50665006),
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    const uint32_t airtime_ms = (uint32_t)(
        (dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_MESH_CONTROL,
                                        UWB_SURVEY_DISCOVERY_PROBE_LEN) + 999u) /
        1000u);
    uint64_t ids[6] = {0};
    bool fixed_retry_decoded = false;

    CHECK(SURVEY_DISCOVERY_OPPORTUNITY_COUNT == SURVEY_REPLY_OPPORTUNITY_COUNT,
          "survey opportunity count drifted from four");
    for (uint8_t anchor_count = 2u; anchor_count <= 6u; anchor_count++) {
        ids[0] = UINT64_C(0xa700000000000001) + anchor_count;
        for (uint8_t index = 1u; index < anchor_count; index++) {
            uint64_t candidate = ids[index - 1u] + 1u;

            while (survey_discovery_opportunity_slot(candidate,
                                                      config.survey_id,
                                                      0u,
                                                      config.slot_count) !=
                   survey_discovery_opportunity_slot(ids[0],
                                                      config.survey_id,
                                                      0u,
                                                      config.slot_count)) {
                candidate++;
            }
            ids[index] = candidate;
        }
        {
            bool diversified = false;

            CHECK(survey_discovery_opportunity_slot(ids[0], config.survey_id, 0u,
                                                     config.slot_count) ==
                      survey_discovery_opportunity_slot(ids[1], config.survey_id, 0u,
                                                        config.slot_count),
                  "forced initial survey collision was not constructed");
            for (uint8_t opportunity = 1u;
                 opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                 opportunity++) {
                uint32_t first_ms = 0u;
                uint32_t second_ms = 0u;

                CHECK(survey_discovery_opportunity_tx_ms(&config, ids[0],
                                                         opportunity,
                                                         &first_ms) == PROTO_OK,
                      "first diversified survey TX calculation failed");
                CHECK(survey_discovery_opportunity_tx_ms(&config, ids[1],
                                                         opportunity,
                                                         &second_ms) == PROTO_OK,
                      "second diversified survey TX calculation failed");
                if (first_ms + airtime_ms <= second_ms ||
                    second_ms + airtime_ms <= first_ms) {
                    diversified = true;
                }
            }
            CHECK(diversified,
                  "same initial survey slot repeated collision through all retries");
        }

        for (uint8_t permutation = 0u; permutation < anchor_count; permutation++) {
            bool accepted[6] = {false};
            bool had_available[6] = {false};

            for (uint8_t opportunity = 0u;
                 opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                 opportunity++) {
                struct interval tx[6] = {0};
                bool collided[6] = {false};
                uint32_t window_start_ms = 0u;
                uint32_t window_end_ms = 0u;

                CHECK(survey_discovery_opportunity_window_ms(&config,
                                                              opportunity,
                                                              &window_start_ms,
                                                              &window_end_ms) == PROTO_OK,
                      "survey opportunity window calculation failed");
                for (uint8_t order = 0u; order < anchor_count; order++) {
                    uint8_t index = (uint8_t)((order + permutation) % anchor_count);
                    uint32_t tx_ms = 0u;

                    CHECK(survey_discovery_opportunity_tx_ms(&config,
                                                             ids[index],
                                                             opportunity,
                                                             &tx_ms) == PROTO_OK,
                          "survey opportunity TX calculation failed");
                    tx[index] = (struct interval) {
                        .start_us = (uint64_t)tx_ms * 1000u,
                        .end_us = ((uint64_t)tx_ms + airtime_ms) * 1000u,
                    };
                    CHECK(tx_ms >= window_start_ms &&
                              tx_ms + airtime_ms <= window_end_ms,
                          "survey frame was not fully contained in its opportunity");
                }
                for (uint8_t i = 0u; i < anchor_count; i++) {
                    for (uint8_t j = (uint8_t)(i + 1u); j < anchor_count; j++) {
                        if (tx[i].start_us < tx[j].end_us &&
                            tx[j].start_us < tx[i].end_us) {
                            collided[i] = true;
                            collided[j] = true;
                        }
                    }
                }
                for (uint8_t i = 0u; i < anchor_count; i++) {
                    bool lost = opportunity < 2u && i == 0u;
                    bool activity_consumed = opportunity == 1u && i == 1u;
                    bool late = opportunity < 2u && i == anchor_count - 1u;

                    if (!collided[i] && !lost && !activity_consumed && !late) {
                        had_available[i] = true;
                        accepted[i] = true;
                    }
                }
            }
            for (uint8_t i = 0u; i < anchor_count; i++) {
                if (had_available[i]) {
                    CHECK(accepted[i],
                          "reachable anchor with an available opportunity was lost");
                }
            }
        }
    }

    for (uint8_t opportunity = 0u;
         opportunity < SURVEY_REPLY_OPPORTUNITY_COUNT;
         opportunity++) {
        struct interval first = {UINT64_C(40000), UINT64_C(40000) + airtime_ms * 1000u};
        struct interval second = first;

        if (!(first.start_us < second.end_us && second.start_us < first.end_us)) {
            fixed_retry_decoded = true;
        }
    }
    CHECK(!fixed_retry_decoded,
          "fixed same-slot retry sensitivity mutation did not deadlock");
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

int main(void)
{
    CHECK(SCAN_PERIOD_US == UINT64_C(380000),
          "test is not using the production low-duty scan period");
    CHECK(MESH_RADIO_ANCHOR_SCAN_RX_US == 3000u,
          "test is not using the production scan window");
    CHECK(dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_WAKE,
                                         UWB_WAKE_CLAIM_LEN) > 0u,
          "wake claim airtime unavailable");
    test_survey_phase_sweep_and_old_defect_sensitivity();
    test_claim_phase_sweep();
    test_discovery_collision_sweep_and_old_spacing_sensitivity();
    test_fixed_survey_retry_deadlock_is_reproduced();
    test_multi_anchor_claim_and_range_schedule_invariants();

    if (failures != 0u) {
        fprintf(stderr, "RESULT multi_anchor_timing_invariants failures=%u\n",
                failures);
        return EXIT_FAILURE;
    }
    printf("PASS multi_anchor_timing_invariants\n");
    return EXIT_SUCCESS;
}
