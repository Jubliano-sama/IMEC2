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
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_SURVEY_DISCOVERY_PROBE_LEN);
    const uint64_t rx_transition_us = runtime_prepare_rx_us(
        DWM3000_TIMING_PHY_CH5_WAKE);
    uint64_t ids[6] = {0};
    bool fixed_retry_decoded = false;

    CHECK(SURVEY_DISCOVERY_OPPORTUNITY_COUNT == SURVEY_REPLY_OPPORTUNITY_COUNT,
          "survey opportunity count drifted from four");
    CHECK(rx_transition_us > 0u &&
              rx_transition_us <=
                  (uint64_t)SURVEY_DISCOVERY_RX_GUARD_MS * 1000u,
          "survey runtime RX guard does not cover the standard-wake transition");
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
                uint64_t first_start_us = (uint64_t)first_ms * 1000u +
                                          (uint64_t)SURVEY_DISCOVERY_RX_GUARD_MS *
                                              1000u;
                uint64_t second_start_us = (uint64_t)second_ms * 1000u +
                                           (uint64_t)SURVEY_DISCOVERY_RX_GUARD_MS *
                                               1000u;

                if (first_start_us + airtime_us <= second_start_us ||
                    second_start_us + airtime_us <= first_start_us) {
                    diversified = true;
                }
            }
            CHECK(diversified,
                  "same initial survey slot repeated collision through all retries");
        }

        for (uint8_t permutation = 0u; permutation < anchor_count; permutation++) {
            bool accepted[6] = {false};
            bool had_available[6] = {false};
            uint8_t attempted[6] = {0};

            for (uint8_t opportunity = 0u;
                 opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                 opportunity++) {
                struct interval tx[6] = {0};
                bool collided[6] = {false};
                for (uint8_t order = 0u; order < anchor_count; order++) {
                    uint8_t index = (uint8_t)((order + permutation) % anchor_count);
                    struct survey_discovery_attempt_schedule schedule;
                    int schedule_ret = survey_discovery_schedule_attempt(
                        &config, ids[index], opportunity, 0u, &schedule);

                    CHECK(schedule_ret == PROTO_OK,
                          "survey attempt schedule calculation failed");
                    if (schedule_ret != PROTO_OK) {
                        continue;
                    }
                    if (opportunity == 1u && index == 1u) {
                        uint32_t blocked_until_ms =
                            schedule.latest_tx_start_ms + 1u;

                        schedule_ret = survey_discovery_schedule_attempt(
                            &config, ids[index], opportunity, blocked_until_ms,
                            &schedule);
                        CHECK(schedule_ret == PROTO_OK && schedule.deferred,
                              "blocked nominal survey window consumed an attempt");
                    } else {
                        CHECK(!schedule.deferred,
                              "unblocked survey attempt unexpectedly deferred");
                    }
                    if (schedule_ret != PROTO_OK) {
                        continue;
                    }
                    attempted[index]++;
                    tx[index].start_us = (uint64_t)schedule.tx_ms * 1000u;
                    tx[index].end_us = tx[index].start_us + airtime_us;
                    CHECK(tx[index].start_us >=
                              (uint64_t)schedule.window_start_ms * 1000u +
                                  rx_transition_us &&
                              schedule.tx_ms <= schedule.latest_tx_start_ms &&
                              tx[index].end_us <=
                                  (uint64_t)schedule.slot_end_ms * 1000u &&
                              tx[index].end_us <=
                                  (uint64_t)schedule.window_end_ms * 1000u,
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
                    bool late = opportunity < 2u && i == anchor_count - 1u;

                    if (!collided[i] && !lost && !late) {
                        had_available[i] = true;
                        accepted[i] = true;
                    }
                }
            }
            for (uint8_t i = 0u; i < anchor_count; i++) {
                CHECK(attempted[i] == SURVEY_DISCOVERY_OPPORTUNITY_COUNT,
                      "blocked survey window reduced the real TX attempt count");
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
        struct interval first = {
            UINT64_C(40000) +
                (uint64_t)SURVEY_DISCOVERY_RX_GUARD_MS * 1000u,
            UINT64_C(40000) +
                (uint64_t)SURVEY_DISCOVERY_RX_GUARD_MS * 1000u + airtime_us,
        };
        struct interval second = first;

        if (!(first.start_us < second.end_us && second.start_us < first.end_us)) {
            fixed_retry_decoded = true;
        }
    }
    CHECK(!fixed_retry_decoded,
          "fixed same-slot retry sensitivity mutation did not deadlock");
}

static void test_survey_deferred_phase_runtime_invariants(void)
{
    const struct survey_discovery_config config = {
        .survey_id = UINT32_C(0x50665006),
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    const uint64_t anchor_id = UINT64_C(0xa700000000000011);
    struct survey_discovery_attempt_schedule nominal[4];
    struct survey_discovery_attempt_schedule deferred;
    uint32_t tx_budget_ms = survey_discovery_probe_tx_budget_ms();
    uint32_t airtime_only_ms = (uint32_t)(
        (dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_WAKE,
                                        UWB_SURVEY_DISCOVERY_PROBE_LEN) + 999u) /
        1000u) + SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS;
    uint8_t real_attempts = 0u;

    CHECK(tx_budget_ms >= 20u + SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS,
          "survey TX budget omitted the blocking send-timeout envelope");
    CHECK(airtime_only_ms < tx_budget_ms,
          "timeout-envelope mutation is not sensitive to airtime-only planning");
    for (uint8_t opportunity = 0u;
         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
         opportunity++) {
        CHECK(survey_discovery_schedule_attempt(&config, anchor_id, opportunity,
                                                 0u, &nominal[opportunity]) ==
                  PROTO_OK && !nominal[opportunity].deferred,
              "nominal survey attempt scheduling failed");
        CHECK(nominal[opportunity].tx_ms <=
                  nominal[opportunity].latest_tx_start_ms &&
                  nominal[opportunity].latest_tx_start_ms + tx_budget_ms <=
                      nominal[opportunity].slot_end_ms,
              "nominal survey attempt can start too late to finish");
    }

    CHECK(survey_discovery_schedule_attempt(
              &config, anchor_id, 0u,
              nominal[0].latest_tx_start_ms + 1u, &deferred) == PROTO_OK &&
              deferred.deferred,
          "blocked nominal opportunity was not preserved for reserve");
    for (uint8_t opportunity = 1u;
         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
         opportunity++) {
        CHECK(nominal[opportunity].window_end_ms <= deferred.window_start_ms,
              "reserve TX preempted a later chronological nominal listen window");
        real_attempts++;
    }
    CHECK(deferred.tx_ms <= deferred.latest_tx_start_ms &&
              deferred.latest_tx_start_ms + tx_budget_ms <=
                  deferred.slot_end_ms,
          "deferred survey attempt can start too late to finish");
    real_attempts++;
    CHECK(real_attempts == SURVEY_DISCOVERY_OPPORTUNITY_COUNT,
          "blocked nominal window reduced four real survey transmissions");
    CHECK(survey_discovery_schedule_attempt(
              &config, anchor_id, 0u,
              deferred.latest_tx_start_ms + 1u, &deferred) == PROTO_ERR_BUSY,
          "missed bounded reserve did not fail explicitly");

    {
        struct survey_discovery_attempt_schedule timeout_sensitive;
        uint32_t airtime_only_latest = nominal[0].slot_end_ms - airtime_only_ms;

        CHECK(airtime_only_latest > nominal[0].latest_tx_start_ms,
              "airtime-only mutation did not create a late unsafe start");
        CHECK(survey_discovery_schedule_attempt(
                  &config, anchor_id, 0u, airtime_only_latest,
                  &timeout_sensitive) == PROTO_OK &&
                  timeout_sensitive.deferred,
              "send-timeout envelope failed to defer an airtime-only late start");
    }

    {
        uint32_t epoch_ms = UINT32_MAX - nominal[3].window_start_ms + 5u;
        uint32_t absolute_window_start = epoch_ms + nominal[3].window_start_ms;
        uint32_t absolute_tx = epoch_ms + nominal[3].tx_ms;
        uint32_t absolute_slot_end = epoch_ms + nominal[3].slot_end_ms;

        CHECK((int32_t)(absolute_tx - absolute_window_start) >= 0 &&
                  (int32_t)(absolute_slot_end - absolute_tx) > 0,
              "UINT32 uptime wrap reversed a valid survey attempt interval");
    }
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
    test_survey_deferred_phase_runtime_invariants();
    test_multi_anchor_claim_and_range_schedule_invariants();

    if (failures != 0u) {
        fprintf(stderr, "RESULT multi_anchor_timing_invariants failures=%u\n",
                failures);
        return EXIT_FAILURE;
    }
    printf("PASS multi_anchor_timing_invariants\n");
    return EXIT_SUCCESS;
}
