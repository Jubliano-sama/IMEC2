#include "dwm3000_runtime.h"
#include "dwm3000_timing.h"
#include "gateway_command.h"
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
#define SURVEY_PAIR_GRAPH_SWEEP_SEEDS 512u
#define SURVEY_GATEWAY_START_DELAY_MS 6000u
#define SURVEY_GATEWAY_REPORT_SLOT_MS \
    (ROUTE_GATEWAY_ACK_TIMEOUT_MS + 20u + 250u)
#define SURVEY_GATEWAY_BENCH_BUDGET_MS 20000u

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

struct survey_probe_event {
    struct interval airtime;
    uint8_t sender;
    bool collided;
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

struct survey_probe_retry_model_result {
    uint32_t tx_ms;
    uint16_t retry_rounds;
    uint8_t pre_rf_refusals;
    uint8_t real_rf_starts;
    bool completion_failed;
    bool deadline_expired;
};

static struct survey_probe_retry_model_result
run_survey_probe_retry_model(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint8_t additional_pre_rf_refusals,
    uint32_t reserve_lateness_ms,
    bool completion_fails_after_rf_start)
{
    struct survey_probe_retry_model_result result = {
        /* The nominal scheduled window was refused before RF. */
        .pre_rf_refusals = 1u,
    };
    struct survey_discovery_probe_attempt attempt;
    uint32_t duration_ms = survey_discovery_duration_ms(config);
    uint32_t retry_origin_ms = duration_ms / 2u + reserve_lateness_ms;
    uint32_t tx_budget_ms = survey_discovery_probe_tx_budget_ms();

    CHECK(survey_discovery_probe_attempt_begin(&attempt, opportunity) ==
              PROTO_OK,
          "survey probe model could not begin an opportunity");

    while (retry_origin_ms < duration_ms) {
        uint32_t due_ms;
        int ret = survey_discovery_probe_attempt_defer(
            &attempt, config, anchor_id, retry_origin_ms, duration_ms);

        if (ret != PROTO_OK) {
            result.deadline_expired = true;
            break;
        }
        due_ms = attempt.due_ms;
        result.retry_rounds = attempt.retry_round;
        if (due_ms >= duration_ms ||
            duration_ms - due_ms <= tx_budget_ms) {
            result.deadline_expired = true;
            break;
        }
        if (additional_pre_rf_refusals > 0u) {
            additional_pre_rf_refusals--;
            result.pre_rf_refusals++;
            retry_origin_ms = due_ms;
            continue;
        }

        result.tx_ms = due_ms;
        CHECK(survey_discovery_probe_attempt_note_rf_started(&attempt) ==
                  PROTO_OK,
              "survey probe model could not commit an RF start");
        result.real_rf_starts = (uint8_t)
            survey_discovery_probe_real_attempt_count(&attempt, 1u);
        result.completion_failed = completion_fails_after_rf_start;
        break;
    }
    if (result.real_rf_starts == 0u) {
        result.deadline_expired = true;
    }
    return result;
}

static void test_survey_real_opportunity_retry_sweep(void)
{
    const struct survey_discovery_config base_config = {
        .survey_id = UINT32_C(0x50666000),
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    uint32_t second_block_recoveries = 0u;
    uint32_t completion_timeout_starts = 0u;
    uint32_t explicit_deadlines = 0u;

    for (uint8_t anchor_count = 1u;
         anchor_count <= SURVEY_GATEWAY_MAX_REPORTS;
         anchor_count++) {
        struct survey_discovery_config config = base_config;
        uint32_t duration_ms;
        uint32_t nominal_duration_ms;
        uint32_t late_offsets_ms[4];
        uint32_t first_retry_times[SURVEY_GATEWAY_MAX_REPORTS] = {0};
        size_t distinct_first_retry_times = 0u;

        config.survey_id += anchor_count;
        duration_ms = survey_discovery_duration_ms(&config);
        nominal_duration_ms = duration_ms / 2u;
        late_offsets_ms[0] = 0u;
        late_offsets_ms[1] = nominal_duration_ms / 8u;
        late_offsets_ms[2] = nominal_duration_ms / 3u;
        late_offsets_ms[3] = nominal_duration_ms - 1u;

        for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
            uint64_t anchor_id = UINT64_C(0xa700000000000001) + anchor;

            for (size_t late_index = 0u;
                 late_index < ARRAY_SIZE(late_offsets_ms);
                 late_index++) {
                for (uint8_t additional_blocks = 0u;
                     additional_blocks <= 3u;
                     additional_blocks++) {
                    uint8_t total_real_starts = 0u;

                    for (uint8_t opportunity = 0u;
                         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                         opportunity++) {
                        bool completion_fails =
                            ((anchor + opportunity + additional_blocks +
                              late_index) & 1u) != 0u;
                        struct survey_probe_retry_model_result result =
                            run_survey_probe_retry_model(
                                &config, anchor_id, opportunity,
                                additional_blocks,
                                late_offsets_ms[late_index],
                                completion_fails);

                        CHECK(result.real_rf_starts <= 1u,
                              "one probe opportunity started RF more than once");
                        CHECK(result.real_rf_starts != 0u ||
                                  result.deadline_expired,
                              "unconsumed probe opportunity vanished before the deadline");
                        CHECK(result.pre_rf_refusals >= 1u &&
                                  result.real_rf_starts <=
                                      result.retry_rounds,
                              "pre-RF refusal consumed a real probe opportunity");
                        if (result.real_rf_starts != 0u) {
                            CHECK(result.tx_ms +
                                      survey_discovery_probe_tx_budget_ms() <
                                      duration_ms,
                                  "retry could not complete inside the discovery horizon");
                            total_real_starts++;
                            if (result.pre_rf_refusals >= 2u) {
                                second_block_recoveries++;
                            }
                            if (result.completion_failed) {
                                completion_timeout_starts++;
                            }
                        } else {
                            explicit_deadlines++;
                        }
                    }

                    if (late_index == 0u && additional_blocks <= 1u) {
                        CHECK(total_real_starts ==
                                  SURVEY_DISCOVERY_OPPORTUNITY_COUNT,
                              "a second pre-RF refusal reduced the four real opportunities");
                    }
                }
            }

            {
                struct survey_probe_retry_model_result first =
                    run_survey_probe_retry_model(
                        &config, anchor_id, 0u, 0u, 0u, false);

                CHECK(first.real_rf_starts == 1u,
                      "first deferred probe did not fit the reserve horizon");
                first_retry_times[anchor] = first.tx_ms;
            }
        }

        for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
            bool seen = false;

            for (uint8_t prior = 0u; prior < anchor; prior++) {
                seen |= first_retry_times[prior] ==
                        first_retry_times[anchor];
            }
            distinct_first_retry_times += seen ? 0u : 1u;
        }
        if (anchor_count > 1u) {
            CHECK(distinct_first_retry_times > 1u,
                  "identity-seeded retries kept every anchor synchronized");
        }
    }

    CHECK(second_block_recoveries > 0u,
          "busy-phase sweep never recovered a twice-blocked opportunity");
    CHECK(completion_timeout_starts > 0u,
          "late-TX sweep did not count RF starts before completion timeout");
    CHECK(explicit_deadlines > 0u,
          "busy-phase sweep did not exercise bounded deadline exhaustion");

    {
        struct survey_discovery_probe_attempt
            attempts[SURVEY_DISCOVERY_OPPORTUNITY_COUNT];

        for (uint8_t opportunity = 0u;
             opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
             opportunity++) {
            CHECK(survey_discovery_probe_attempt_begin(
                      &attempts[opportunity], opportunity) == PROTO_OK,
                  "exact-count fixture could not begin an opportunity");
            CHECK(survey_discovery_probe_attempt_note_rf_started(
                      &attempts[opportunity]) == PROTO_OK,
                  "exact-count fixture could not commit an RF start");
        }
        CHECK(survey_discovery_probe_real_attempt_count(
                  attempts, ARRAY_SIZE(attempts)) ==
                  SURVEY_DISCOVERY_OPPORTUNITY_COUNT,
              "four distinct opportunities did not produce exactly four starts");
        CHECK(survey_discovery_probe_attempt_note_rf_started(&attempts[0]) ==
                  PROTO_ERR_STALE &&
                  survey_discovery_probe_real_attempt_count(
                      attempts, ARRAY_SIZE(attempts)) ==
                      SURVEY_DISCOVERY_OPPORTUNITY_COUNT,
              "a completion retry consumed a fifth real opportunity");
    }

    {
        struct survey_discovery_probe_attempt attempt;
        uint32_t deadline_ms = survey_discovery_duration_ms(&base_config);

        CHECK(survey_discovery_probe_attempt_begin(&attempt, 0u) == PROTO_OK,
              "deadline fixture could not begin an opportunity");
        CHECK(survey_discovery_probe_attempt_defer(
                  &attempt, &base_config,
                  UINT64_C(0xa7000000000000fe), deadline_ms,
                  deadline_ms) == PROTO_ERR_BUSY &&
                  survey_discovery_probe_real_attempt_count(&attempt, 1u) == 0u,
              "an exact-deadline refusal consumed an RF opportunity");
        CHECK(survey_discovery_probe_attempt_defer(
                  &attempt, &base_config,
                  UINT64_C(0xa7000000000000fe), deadline_ms - 1u,
                  deadline_ms) == PROTO_OK &&
                  attempt.due_ms == deadline_ms && attempt.pending &&
                  survey_discovery_probe_real_attempt_count(&attempt, 1u) == 0u,
              "deadline clipping lost the pending unconsumed opportunity");
    }

    {
        struct survey_gateway_context gateway;
        struct proto_packet packet;
        uint8_t payload[32];
        uint32_t survey_id = 0u;
        uint64_t anchor_id = 0u;
        size_t payload_len = 0u;
        size_t entry_count = 99u;

        CHECK(survey_append_reach_report_tlvs(
                  payload, sizeof(payload), &payload_len,
                  base_config.survey_id,
                  UINT64_C(0xa7000000000000fe), NULL, 0u) == PROTO_OK,
              "deadline exhaustion could not encode an empty report");
        CHECK(survey_init_discovery_report_packet(
                  &packet, UINT64_C(0xa7000000000000fe),
                  UINT64_C(0xa001000000000001), base_config.survey_id,
                  77u, (uint8_t)payload_len) == PROTO_OK,
              "deadline exhaustion could not wrap its empty report");
        CHECK(survey_extract_reach_report_tlvs(
                  payload, payload_len, &survey_id, &anchor_id,
                  NULL, 0u, &entry_count) == PROTO_OK &&
                  entry_count == 0u &&
                  packet.session_id == survey_id &&
                  packet.src_id == anchor_id,
              "deadline empty report did not survive wire decoding");
        CHECK(survey_gateway_begin(&gateway, base_config.survey_id, 1u) ==
                  PROTO_OK &&
                  survey_gateway_note_reach_report(
                      &gateway, survey_id, anchor_id, NULL,
                      entry_count) == PROTO_OK &&
                  gateway.report_count == 1u,
              "gateway did not count the deadline exhaustion report");
    }
}

static uint32_t reconstructed_survey_start_ms(
    const struct survey_discovery_config *config,
    uint32_t receive_ms,
    uint32_t message_age_ms)
{
    struct survey_discovery_timing timing;

    CHECK(survey_discovery_timing_from_age(config, message_age_ms, &timing) ==
              PROTO_OK,
          "survey repeat age was rejected");
    CHECK(!timing.expired, "fresh survey repeat was treated as expired");
    return timing.pending ? receive_ms + timing.wait_ms :
           receive_ms - timing.elapsed_ms;
}

static void test_survey_repeat_age_convergence_sweep(void)
{
    static const uint32_t repeat_delays_ms[] = {0u, 60u, 120u, 180u};
    const uint32_t origin_ms = UINT32_C(500000);
    bool zero_age_mutation_diverged = false;

    for (uint8_t anchor_count = 2u; anchor_count <= 50u; anchor_count++) {
        for (uint32_t seed = 1u; seed <= 32u; seed++) {
            struct survey_discovery_config config = {
                .survey_id = UINT32_C(0x50665000) + seed,
                .start_delay_ms = 2000u,
                .slot_ms = 40u,
                .slot_count = 6u,
            };
            uint32_t expected_start_ms = origin_ms + config.start_delay_ms;
            uint32_t expected_deadline_ms = expected_start_ms +
                survey_discovery_duration_ms(&config);

            for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
                uint64_t anchor_id = UINT64_C(0xa700000000000001) ^
                    ((uint64_t)seed << 48) ^
                    ((uint64_t)(anchor + 1u) *
                     UINT64_C(0x9e3779b97f4a7c15));
                uint8_t selected_repeat = (uint8_t)((anchor + seed) %
                    ARRAY_SIZE(repeat_delays_ms));
                uint32_t selected_start_ms = 0u;

                for (uint8_t repeat = 0u;
                     repeat < ARRAY_SIZE(repeat_delays_ms); repeat++) {
                    uint32_t receive_ms = origin_ms + repeat_delays_ms[repeat];
                    uint32_t start_ms = reconstructed_survey_start_ms(
                        &config, receive_ms, repeat_delays_ms[repeat]);

                    CHECK(start_ms == expected_start_ms,
                          "different 0x54 repeat changed reconstructed survey origin");
                    CHECK(start_ms + survey_discovery_duration_ms(&config) ==
                              expected_deadline_ms,
                          "different 0x54 repeat changed survey deadline");
                    if (repeat == selected_repeat) {
                        selected_start_ms = start_ms;
                    }

                    if (repeat > 0u &&
                        reconstructed_survey_start_ms(&config, receive_ms, 0u) !=
                            expected_start_ms) {
                        zero_age_mutation_diverged = true;
                    }
                }

                for (uint8_t opportunity = 0u;
                     opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                     opportunity++) {
                    struct survey_discovery_attempt_schedule schedule;

                    CHECK(survey_discovery_schedule_attempt(
                              &config, anchor_id, opportunity, 0u,
                              &schedule) == PROTO_OK,
                          "repeat-skew survey attempt scheduling failed");
                    CHECK(selected_start_ms + schedule.tx_ms ==
                              expected_start_ms + schedule.tx_ms,
                          "repeat selection shifted an absolute survey probe");
                    CHECK(selected_start_ms + schedule.window_end_ms <=
                              expected_deadline_ms,
                          "repeat selection moved a probe beyond the survey horizon");
                }
            }
        }
    }
    CHECK(zero_age_mutation_diverged,
          "zero-age repeated 0x54 mutation did not perturb the sweep");
}

static void test_survey_gateway_collection_budget_sweep(void)
{
    static const uint16_t discovery_slot_widths_ms[] = {
        SURVEY_DISCOVERY_MIN_SLOT_MS,
        40u,
        SURVEY_DISCOVERY_MAX_SLOT_MS,
    };
    static const uint32_t report_grace_windows_ms[] = {
        1u, 250u, 1000u, 5000u,
    };
    bool reproduced_old_three_phase_close = false;
    bool covered_bench_50_anchor_case = false;
    bool covered_50_slot_case = false;

    for (uint8_t anchor_count = 1u;
         anchor_count <= SURVEY_GATEWAY_MAX_REPORTS; anchor_count++) {
        for (uint8_t slot_count = 1u;
             slot_count <= SURVEY_DISCOVERY_MAX_SLOT_COUNT; slot_count++) {
            for (size_t width_index = 0u;
                 width_index < ARRAY_SIZE(discovery_slot_widths_ms);
                 width_index++) {
                struct survey_discovery_config config = {
                    .survey_id = UINT32_C(0x50665000) + anchor_count,
                    .start_delay_ms = SURVEY_GATEWAY_START_DELAY_MS,
                    .slot_ms = discovery_slot_widths_ms[width_index],
                    .slot_count = slot_count,
                };
                uint32_t discovery_duration_ms =
                    survey_discovery_duration_ms(&config);
                uint32_t first_report_ms = config.start_delay_ms +
                    discovery_duration_ms;
                uint32_t last_report_start_ms = 0u;

                CHECK(discovery_duration_ms != 0u,
                      "survey deadline sweep produced an invalid discovery horizon");
                CHECK(survey_discovery_report_delay_ms(
                          &config, (uint8_t)(slot_count - 1u),
                          SURVEY_GATEWAY_REPORT_SLOT_MS,
                          &last_report_start_ms) == PROTO_OK,
                      "survey deadline sweep could not place the final report slot");
                last_report_start_ms += config.start_delay_ms;

                for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
                    uint8_t report_slot = (uint8_t)(anchor % slot_count);
                    uint32_t report_start_ms = 0u;

                    CHECK(survey_discovery_report_delay_ms(
                              &config, report_slot,
                              SURVEY_GATEWAY_REPORT_SLOT_MS,
                              &report_start_ms) == PROTO_OK,
                          "anchor report slot could not be scheduled");
                    report_start_ms += config.start_delay_ms;
                    CHECK(report_start_ms >= first_report_ms &&
                              report_start_ms <= last_report_start_ms,
                          "anchor report fell outside the composed report train");
                }

                for (size_t grace_index = 0u;
                     grace_index < ARRAY_SIZE(report_grace_windows_ms);
                     grace_index++) {
                    uint32_t no_anchor_evidence_ms = last_report_start_ms +
                        SURVEY_GATEWAY_REPORT_SLOT_MS +
                        report_grace_windows_ms[grace_index];
                    uint32_t boundary_budgets_ms[] = {
                        1u,
                        first_report_ms - 1u,
                        first_report_ms,
                        SURVEY_GATEWAY_BENCH_BUDGET_MS,
                        no_anchor_evidence_ms - 1u,
                        no_anchor_evidence_ms,
                        no_anchor_evidence_ms + 1u,
                    };

                    CHECK(no_anchor_evidence_ms > last_report_start_ms,
                          "survey no-anchor evidence horizon overflowed");
                    for (size_t budget_index = 0u;
                         budget_index < ARRAY_SIZE(boundary_budgets_ms);
                         budget_index++) {
                        uint32_t budget_ms = boundary_budgets_ms[budget_index];
                        uint32_t expected_wake_ms =
                            budget_ms < no_anchor_evidence_ms ?
                                budget_ms : no_anchor_evidence_ms;
                        uint32_t actual_wake_ms =
                            gateway_command_budget_window_ms(
                                true, budget_ms, 1u,
                                no_anchor_evidence_ms);

                        CHECK(actual_wake_ms == expected_wake_ms,
                              "explicit survey budget shortened the indivisible collection phase");
                        if (actual_wake_ms < no_anchor_evidence_ms) {
                            CHECK(actual_wake_ms == budget_ms,
                                  "survey woke before both its host and evidence deadlines");
                        } else {
                            CHECK(actual_wake_ms == no_anchor_evidence_ms,
                                  "survey ran past the complete no-anchor evidence horizon");
                        }
                    }

                    if (anchor_count == 50u && slot_count == 6u &&
                        config.slot_ms == 40u &&
                        report_grace_windows_ms[grace_index] == 1000u) {
                        uint32_t current_wake_ms =
                            gateway_command_budget_window_ms(
                                true, SURVEY_GATEWAY_BENCH_BUDGET_MS, 1u,
                                no_anchor_evidence_ms);
                        uint32_t old_three_phase_wake_ms =
                            gateway_command_budget_window_ms(
                                true, SURVEY_GATEWAY_BENCH_BUDGET_MS, 3u,
                                no_anchor_evidence_ms);

                        covered_bench_50_anchor_case = true;
                        CHECK(first_report_ms == 9040u,
                              "six-slot survey first-report timing drifted");
                        CHECK(last_report_start_ms == 20390u,
                              "six-slot survey final-report timing drifted");
                        CHECK(no_anchor_evidence_ms == 23660u,
                              "six-slot survey evidence horizon drifted");
                        CHECK(current_wake_ms ==
                                  SURVEY_GATEWAY_BENCH_BUDGET_MS &&
                                  current_wake_ms < no_anchor_evidence_ms,
                              "20-second bench budget was not preserved as a generic timeout boundary");
                        if (old_three_phase_wake_ms < first_report_ms) {
                            reproduced_old_three_phase_close = true;
                        }
                    }
                    if (anchor_count == 50u && slot_count == 50u &&
                        config.slot_ms == 40u &&
                        report_grace_windows_ms[grace_index] == 1000u) {
                        covered_50_slot_case = true;
                        CHECK(SURVEY_GATEWAY_BENCH_BUDGET_MS < first_report_ms,
                              "20-second budget unexpectedly covered 50-slot discovery");
                        CHECK(gateway_command_budget_window_ms(
                                  true, SURVEY_GATEWAY_BENCH_BUDGET_MS, 1u,
                                  no_anchor_evidence_ms) ==
                                  SURVEY_GATEWAY_BENCH_BUDGET_MS,
                              "50-slot survey budget closed before its global timeout");
                    }
                }
            }
        }
    }

    CHECK(covered_bench_50_anchor_case,
          "survey deadline sweep omitted the 50-anchor/six-slot bench case");
    CHECK(covered_50_slot_case,
          "survey deadline sweep omitted the maximum-slot case");
    CHECK(reproduced_old_three_phase_close,
          "three-phase mutation no longer reproduces premature no-anchor closure");
}

struct survey_control_budget_model_result {
    uint32_t remaining_ms;
    uint8_t pair_successes;
    uint8_t pair_failures;
};

static struct survey_control_budget_model_result
run_three_pair_control_budget_model(bool divide_by_remaining_phases)
{
    struct survey_control_budget_model_result result = {
        .remaining_ms = 40000u,
    };

    for (uint8_t pair = 0u; pair < 3u; pair++) {
        bool pair_failed = false;

        for (uint8_t phase = 0u; phase < 4u; phase++) {
            uint8_t phases_remaining = (uint8_t)(
                ((3u - pair - 1u) * 4u) + (4u - phase));
            uint32_t result_delay_ms = phase < 2u ? 1500u : 5100u;
            uint32_t timeout_ms = gateway_command_budget_window_ms(
                true,
                result.remaining_ms,
                divide_by_remaining_phases ? phases_remaining : 1u,
                SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);

            if (result_delay_ms >= timeout_ms) {
                result.remaining_ms -= timeout_ms;
                result.pair_failures++;
                pair_failed = true;
                break;
            }
            result.remaining_ms -= result_delay_ms;
        }
        if (!pair_failed) {
            result.pair_successes++;
        }
    }
    return result;
}

static void test_survey_control_global_budget_multi_pair_sweep(void)
{
    static const uint32_t budgets_ms[] = {
        1u, 1500u, 1501u, 5100u, 5101u,
        40000u, 90000u, 90001u, 180000u,
    };
    static const uint32_t result_delays_ms[] = {
        1u, 1499u, 1500u, 5099u, 5100u,
        10000u, 89999u, 90000u,
    };
    struct survey_control_budget_model_result old_model =
        run_three_pair_control_budget_model(true);
    struct survey_control_budget_model_result global_model =
        run_three_pair_control_budget_model(false);
    uint32_t old_false_timeouts = 0u;

    CHECK(old_model.pair_successes == 1u && old_model.pair_failures == 2u,
          "fair-share mutation did not reproduce two early pair failures");
    CHECK(global_model.pair_successes == 3u &&
              global_model.pair_failures == 0u &&
              global_model.remaining_ms == 400u,
          "global survey deadline rejected results that all fit before it");

    for (uint16_t pair_count = 1u;
         pair_count <= SURVEY_GATEWAY_MAX_PAIRS; pair_count++) {
        uint32_t total_phases = (uint32_t)pair_count * 4u;

        for (uint32_t phase = 0u; phase < total_phases; phase++) {
            uint32_t remaining_phases = total_phases - phase;
            uint8_t old_phase_divisor = remaining_phases > UINT8_MAX ?
                UINT8_MAX : (uint8_t)remaining_phases;

            for (size_t budget = 0u; budget < ARRAY_SIZE(budgets_ms); budget++) {
                uint32_t fixed_timeout_ms = gateway_command_budget_window_ms(
                    true,
                    budgets_ms[budget],
                    1u,
                    SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
                uint32_t old_timeout_ms = gateway_command_budget_window_ms(
                    true,
                    budgets_ms[budget],
                    old_phase_divisor,
                    SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
                uint32_t expected_timeout_ms =
                    budgets_ms[budget] < SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS ?
                        budgets_ms[budget] :
                        SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;

                CHECK(fixed_timeout_ms == expected_timeout_ms,
                      "control timeout was not clipped only by the global deadline");
                for (size_t delay = 0u;
                     delay < ARRAY_SIZE(result_delays_ms); delay++) {
                    bool result_is_on_time =
                        result_delays_ms[delay] < budgets_ms[budget] &&
                        result_delays_ms[delay] <
                            SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
                    bool fixed_accepts =
                        result_delays_ms[delay] < fixed_timeout_ms;
                    bool old_accepts =
                        result_delays_ms[delay] < old_timeout_ms;

                    CHECK(fixed_accepts == result_is_on_time,
                          "matching result disposition disagreed with the global deadline");
                    if (fixed_accepts && !old_accepts) {
                        old_false_timeouts++;
                    }
                }
            }
        }
    }
    CHECK(old_false_timeouts > 0u,
          "multi-pair sweep did not detect fair-share false timeouts");
}

static bool survey_probe_event_precedes(const struct survey_probe_event *left,
                                        const struct survey_probe_event *right)
{
    return left->airtime.start_us < right->airtime.start_us ||
           (left->airtime.start_us == right->airtime.start_us &&
            left->sender < right->sender);
}

static bool build_survey_probe_reports(
    uint8_t anchor_count,
    uint32_t seed,
    struct survey_reachability_report *reports,
    struct survey_reachability_entry
        entries[SURVEY_GATEWAY_MAX_REPORTS][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT])
{
    const struct survey_discovery_config config = {
        .survey_id = UINT32_C(0x50665000) + seed,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    const uint64_t anchor_base = UINT64_C(0xa700000000000001) +
        (uint64_t)seed * UINT64_C(0x10001);
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_SURVEY_DISCOVERY_PROBE_LEN);
    struct survey_probe_event events[
        SURVEY_GATEWAY_MAX_REPORTS * SURVEY_DISCOVERY_OPPORTUNITY_COUNT];
    size_t event_count = 0u;

    memset(reports, 0,
           SURVEY_GATEWAY_MAX_REPORTS * sizeof(reports[0]));
    memset(entries, 0,
           SURVEY_GATEWAY_MAX_REPORTS * sizeof(entries[0]));
    if (airtime_us == 0u) {
        return false;
    }
    for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
        reports[anchor].anchor_id = anchor_base + anchor;
        reports[anchor].entries = entries[anchor];
    }
    for (uint8_t opportunity = 0u;
         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT; opportunity++) {
        const size_t first_event = event_count;

        for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
            struct survey_discovery_attempt_schedule schedule;

            if (survey_discovery_schedule_attempt(
                    &config, anchor_base + anchor, opportunity, 0u,
                    &schedule) != PROTO_OK || schedule.deferred) {
                return false;
            }
            events[event_count] = (struct survey_probe_event) {
                .airtime = {
                    .start_us = (uint64_t)schedule.tx_ms * 1000u,
                    .end_us = (uint64_t)schedule.tx_ms * 1000u + airtime_us,
                },
                .sender = anchor,
            };
            event_count++;
        }
        for (size_t left = first_event; left < event_count; left++) {
            for (size_t right = left + 1u; right < event_count; right++) {
                if (events[left].airtime.start_us <
                        events[right].airtime.end_us &&
                    events[right].airtime.start_us <
                        events[left].airtime.end_us) {
                    events[left].collided = true;
                    events[right].collided = true;
                }
            }
        }
    }

    for (size_t i = 1u; i < event_count; i++) {
        struct survey_probe_event value = events[i];
        size_t insert = i;

        while (insert > 0u &&
               survey_probe_event_precedes(&value, &events[insert - 1u])) {
            events[insert] = events[insert - 1u];
            insert--;
        }
        events[insert] = value;
    }
    for (size_t event = 0u; event < event_count; event++) {
        const uint8_t sender = events[event].sender;

        if (events[event].collided) {
            continue;
        }
        for (uint8_t receiver = 0u; receiver < anchor_count; receiver++) {
            bool already_seen = false;

            if (receiver == sender) {
                continue;
            }
            for (size_t peer = 0u; peer < reports[receiver].entry_count;
                 peer++) {
                already_seen |= entries[receiver][peer].peer_id ==
                    anchor_base + sender;
            }
            if (!already_seen && reports[receiver].entry_count <
                                     SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
                entries[receiver][reports[receiver].entry_count++] =
                    (struct survey_reachability_entry) {
                        .peer_id = anchor_base + sender,
                        .rssi_dbm = -60,
                        .quality = 80u,
                    };
            }
        }
    }
    return true;
}

static bool survey_pair_graph_connected(
    uint8_t anchor_count,
    uint64_t anchor_base,
    const struct survey_pair *pairs,
    size_t pair_count)
{
    bool adjacent[SURVEY_GATEWAY_MAX_REPORTS][SURVEY_GATEWAY_MAX_REPORTS] = {{0}};
    bool visited[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    uint8_t degree[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    uint8_t queue[SURVEY_GATEWAY_MAX_REPORTS];
    size_t head = 0u;
    size_t tail = 0u;

    for (size_t pair = 0u; pair < pair_count; pair++) {
        if (pairs[pair].initiator_id < anchor_base ||
            pairs[pair].responder_id < anchor_base) {
            return false;
        }
        const size_t first = (size_t)(pairs[pair].initiator_id - anchor_base);
        const size_t second = (size_t)(pairs[pair].responder_id - anchor_base);

        if (first >= anchor_count || second >= anchor_count || first == second ||
            adjacent[first][second]) {
            return false;
        }
        adjacent[first][second] = true;
        adjacent[second][first] = true;
        degree[first]++;
        degree[second]++;
        if (degree[first] > SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
            degree[second] > SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return false;
        }
    }

    visited[0] = true;
    queue[tail++] = 0u;
    while (head < tail) {
        const uint8_t current = queue[head++];

        for (uint8_t peer = 0u; peer < anchor_count; peer++) {
            if (adjacent[current][peer] && !visited[peer]) {
                visited[peer] = true;
                queue[tail++] = peer;
            }
        }
    }
    for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
        if (!visited[anchor]) {
            return false;
        }
    }
    return true;
}

static void test_survey_probe_pair_graph_stress_sweep(void)
{
    static const uint8_t anchor_counts[] = {2u, 6u, 16u, 20u, 32u, 50u};
    struct survey_reachability_report reports[SURVEY_GATEWAY_MAX_REPORTS];
    struct survey_reachability_entry
        entries[SURVEY_GATEWAY_MAX_REPORTS][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS];

    for (size_t count_index = 0u; count_index < ARRAY_SIZE(anchor_counts);
         count_index++) {
        const uint8_t anchor_count = anchor_counts[count_index];
        size_t connected = 0u;
        size_t explicit_unconnectable = 0u;
        size_t nonempty_reports = 0u;
        bool model_valid = true;

        for (uint32_t seed = 1u; seed <= SURVEY_PAIR_GRAPH_SWEEP_SEEDS; seed++) {
            const uint64_t anchor_base = UINT64_C(0xa700000000000001) +
                (uint64_t)seed * UINT64_C(0x10001);
            size_t pair_count = 0u;
            int ret;

            model_valid &= build_survey_probe_reports(anchor_count, seed,
                                                       reports, entries);
            if (!model_valid) {
                break;
            }
            bool all_nonempty = true;
            for (uint8_t anchor = 0u; anchor < anchor_count; anchor++) {
                all_nonempty &= reports[anchor].entry_count != 0u;
            }
            nonempty_reports += all_nonempty ? 1u : 0u;
            ret = survey_plan_pairs_from_reachability(
                UINT32_C(0x50665000) + seed, reports, anchor_count, 1u,
                pairs, ARRAY_SIZE(pairs), &pair_count);
            if (ret == PROTO_ERR_NOT_FOUND) {
                explicit_unconnectable++;
            } else if (ret == PROTO_OK &&
                       survey_pair_graph_connected(anchor_count, anchor_base,
                                                   pairs, pair_count)) {
                connected++;
            } else if (ret != PROTO_OK) {
                model_valid = false;
                break;
            }
        }
        printf("survey-pair-graph anchors=%u seeds=%u reports=%zu connected=%zu explicit_unconnectable=%zu\n",
               anchor_count, SURVEY_PAIR_GRAPH_SWEEP_SEEDS, nonempty_reports,
               connected, explicit_unconnectable);
        CHECK(model_valid, "survey pair-graph model failed internally");
        CHECK(nonempty_reports == SURVEY_PAIR_GRAPH_SWEEP_SEEDS,
              "collision retries left an anchor without reportable peers");
        CHECK(connected == SURVEY_PAIR_GRAPH_SWEEP_SEEDS &&
                  explicit_unconnectable == 0u,
              "connectable survey reports produced a disconnected pair graph");
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
    test_survey_real_opportunity_retry_sweep();
    test_survey_repeat_age_convergence_sweep();
    test_survey_gateway_collection_budget_sweep();
    test_survey_control_global_budget_multi_pair_sweep();
    test_survey_probe_pair_graph_stress_sweep();
    test_multi_anchor_claim_and_range_schedule_invariants();

    if (failures != 0u) {
        fprintf(stderr, "RESULT multi_anchor_timing_invariants failures=%u\n",
                failures);
        return EXIT_FAILURE;
    }
    printf("PASS multi_anchor_timing_invariants\n");
    return EXIT_SUCCESS;
}
