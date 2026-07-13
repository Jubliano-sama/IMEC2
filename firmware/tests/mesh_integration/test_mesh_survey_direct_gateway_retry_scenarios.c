#include "app_mesh_direct_gateway_retry.h"
#include "dwm3000_timing.h"
#include "mesh_relay.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DIRECT_ANCHORS 20u
#define SURVEY_ID UINT32_C(0x50665006)
#define ANCHOR_BASE UINT64_C(0xa002000000010000)
#define FOLDED_ID_PATTERN_BASE UINT64_C(0xf01ded0000000000)
#define PROBE_ATTEMPT_MS 280u
#define RUNTIME_PROBE_ATTEMPT_MS \
    (PROBE_ATTEMPT_MS + APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS + \
     APP_MESH_DIRECT_GATEWAY_SURVEY_TRANSITION_GUARD_MS)

struct reporter {
    struct app_mesh_direct_gateway_retry_state retry;
    uint64_t tx_start_us;
    bool delivered;
    bool exhausted;
};

static int failures;

#define CHECK(expression) do {                                                \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expression);       \
        failures++;                                                           \
        return;                                                               \
    }                                                                         \
} while (0)

#define CHECK_VALUE(expression, value) do {                                   \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expression);       \
        failures++;                                                           \
        return (value);                                                       \
    }                                                                         \
} while (0)

static bool overlaps(uint64_t first_start_us,
                     uint64_t second_start_us,
                     uint64_t airtime_us)
{
    return first_start_us < second_start_us + airtime_us &&
           second_start_us < first_start_us + airtime_us;
}

static size_t run_twenty(uint32_t survey_id,
                         uint64_t anchor_base,
                         enum app_mesh_direct_gateway_retry_mode mode,
                         bool correlated_route_random,
                         uint64_t *last_attempt_end_us)
{
    struct reporter reporters[DIRECT_ANCHORS];
    const uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH9_MESH, UWB_MESH_FRAME_HEADER_LEN);
    const uint64_t gateway_service_us = airtime_us +
        (uint64_t)APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS * 1000u;
    size_t delivered = 0u;

    memset(reporters, 0, sizeof(reporters));
    for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
        uint64_t anchor_id = anchor_base == FOLDED_ID_PATTERN_BASE ?
            ((uint64_t)(i + 1u) << 32) | (uint64_t)(i + 1u) :
            anchor_base + i;

        CHECK_VALUE(app_mesh_direct_gateway_retry_init(&reporters[i].retry,
                                                       mode, anchor_id,
                                                       survey_id) == 0, 0u);
    }
    CHECK_VALUE(airtime_us > 0u, 0u);

    for (unsigned int wave = 0u; wave < 32u && delivered < DIRECT_ANCHORS;
         wave++) {
        bool collision[DIRECT_ANCHORS] = {false};
        bool any = false;

        for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
            if (reporters[i].delivered || reporters[i].exhausted) {
                continue;
            }
            any = true;
            for (size_t j = i + 1u; j < DIRECT_ANCHORS; j++) {
                if (reporters[j].delivered || reporters[j].exhausted) {
                    continue;
                }
                if (overlaps(reporters[i].tx_start_us,
                             reporters[j].tx_start_us,
                             gateway_service_us)) {
                    collision[i] = true;
                    collision[j] = true;
                }
            }
        }
        if (!any) {
            break;
        }
        for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
            struct app_mesh_direct_gateway_retry_decision decision;
            uint32_t random_value = correlated_route_random ? 0u :
                                    (uint32_t)(anchor_base + i + wave * 101u);

            if (reporters[i].delivered || reporters[i].exhausted) {
                continue;
            }
            CHECK_VALUE(app_mesh_direct_gateway_retry_note(
                            &reporters[i].retry,
                            collision[i] ?
                                APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED :
                                APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS,
                            random_value, &decision) == 0, 0u);
            if (!collision[i]) {
                reporters[i].delivered = true;
                delivered++;
            } else if (decision.retry) {
                reporters[i].tx_start_us +=
                    ((uint64_t)PROBE_ATTEMPT_MS + decision.delay_ms) * 1000u;
            } else {
                reporters[i].exhausted = true;
            }
            if (last_attempt_end_us != NULL &&
                reporters[i].tx_start_us + airtime_us > *last_attempt_end_us) {
                *last_attempt_end_us = reporters[i].tx_start_us + airtime_us;
            }
        }
    }
    return delivered;
}

static void test_legacy_policy_has_correlated_collision_failure(void)
{
    uint64_t last_us = 0u;

    CHECK(run_twenty(0u, ANCHOR_BASE,
                     APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE, true,
                     &last_us) == 0u);
    CHECK(last_us <= (uint64_t)
              app_mesh_direct_gateway_retry_policy_horizon_ms(
                  APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,
                  PROBE_ATTEMPT_MS, 0u) * 1000u);
}

static void test_survey_policy_decorrelates_twenty_direct_reporters(void)
{
    uint64_t last_us = 0u;
    uint32_t horizon_ms = app_mesh_direct_gateway_retry_policy_horizon_ms(
        APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, PROBE_ATTEMPT_MS, 0u);

    CHECK(run_twenty(SURVEY_ID, ANCHOR_BASE,
                     APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, false,
                     &last_us) == DIRECT_ANCHORS);
    CHECK(last_us <= (uint64_t)horizon_ms * 1000u);
    /*
     * A 250 ms retry window holds at most twelve 20 ms gateway re-arm gaps
     * before airtime and ACK turnaround. The 2 second first survey retry has
     * one hundred such gaps, and the 4/8 second retries widen the colliding
     * tail again.
     */
    CHECK(APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS /
              APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS >=
          DIRECT_ANCHORS);
    CHECK(APP_MESH_DIRECT_GATEWAY_SURVEY_DELIVERY_TAIL_MS(
              RUNTIME_PROBE_ATTEMPT_MS,
              APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS,
              MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS) ==
          app_mesh_direct_gateway_retry_policy_horizon_ms(
              APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
              RUNTIME_PROBE_ATTEMPT_MS,
              APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS) +
              MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS +
              APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS +
              APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS);
}

static void test_busy_deferral_does_not_consume_four_real_attempts(void)
{
    struct app_mesh_direct_gateway_retry_state state;
    struct app_mesh_direct_gateway_retry_decision decision;

    CHECK(app_mesh_direct_gateway_retry_init(
              &state, APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
              ANCHOR_BASE, SURVEY_ID) == 0);
    for (uint8_t i = 0u; i < APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_DEFERRALS; i++) {
        CHECK(app_mesh_direct_gateway_retry_note(
                  &state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY, 0u,
                  &decision) == 0);
        CHECK(decision.retry && !decision.attempt_consumed &&
              state.attempts == 0u);
    }
    CHECK(app_mesh_direct_gateway_retry_note(
              &state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY, 0u,
              &decision) == 0);
    CHECK(decision.exhausted && decision.busy_exhausted && !decision.retry &&
          state.attempts == 0u);

    CHECK(app_mesh_direct_gateway_retry_init(
              &state, APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
              ANCHOR_BASE, SURVEY_ID) == 0);
    for (uint8_t i = 0u; i < APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS; i++) {
        CHECK(app_mesh_direct_gateway_retry_note(
                  &state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED, 0u,
                  &decision) == 0);
        CHECK(decision.attempt_consumed && state.attempts == i + 1u);
    }
    CHECK(decision.exhausted && !decision.retry);
}

static void test_full_width_identity_mixing_breaks_folded_collisions(void)
{
    const uint64_t first_id = UINT64_C(0x0000000100000001);
    const uint64_t second_id = UINT64_C(0x0000000200000002);
    struct app_mesh_direct_gateway_retry_state first;
    struct app_mesh_direct_gateway_retry_state second;
    struct app_mesh_direct_gateway_retry_decision first_decision;
    struct app_mesh_direct_gateway_retry_decision second_decision;
    bool differs = false;

    CHECK(((uint32_t)first_id ^ (uint32_t)(first_id >> 32)) ==
          ((uint32_t)second_id ^ (uint32_t)(second_id >> 32)));
    CHECK(app_mesh_direct_gateway_retry_init(
              &first, APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
              first_id, SURVEY_ID) == 0);
    CHECK(app_mesh_direct_gateway_retry_init(
              &second, APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
              second_id, SURVEY_ID) == 0);
    for (uint8_t attempt = 0u;
         attempt + 1u < APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS; attempt++) {
        CHECK(app_mesh_direct_gateway_retry_note(
                  &first, APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED, 0u,
                  &first_decision) == 0);
        CHECK(app_mesh_direct_gateway_retry_note(
                  &second, APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED, 0u,
                  &second_decision) == 0);
        differs = differs || first_decision.delay_ms != second_decision.delay_ms;
    }
    CHECK(differs);
    CHECK(run_twenty(SURVEY_ID, FOLDED_ID_PATTERN_BASE,
                     APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, true,
                     NULL) == DIRECT_ANCHORS);
}

static void test_policy_deadline_boundary_and_wrap(void)
{
    const uint32_t start_ms = UINT32_MAX - 20u;
    const uint32_t horizon_ms = 100u;
    const uint32_t deadline_ms = start_ms + horizon_ms;

    CHECK(horizon_ms < INT32_MAX);
    CHECK(!app_mesh_direct_gateway_retry_deadline_reached(start_ms,
                                                          deadline_ms));
    CHECK(app_mesh_direct_gateway_retry_deadline_remaining_ms(
              start_ms, deadline_ms) == horizon_ms);
    CHECK(!app_mesh_direct_gateway_retry_deadline_reached(deadline_ms - 1u,
                                                          deadline_ms));
    CHECK(app_mesh_direct_gateway_retry_deadline_remaining_ms(
              deadline_ms - 1u, deadline_ms) == 1u);
    CHECK(app_mesh_direct_gateway_retry_deadline_reached(deadline_ms,
                                                         deadline_ms));
    CHECK(app_mesh_direct_gateway_retry_deadline_remaining_ms(
              deadline_ms, deadline_ms) == 0u);
}

static void test_seeded_corpus_reports_empirical_result_without_claim(void)
{
    size_t passed = 0u;
    const size_t cases = 256u;

    for (size_t i = 0u; i < cases; i++) {
        uint64_t last_us = 0u;

        if (run_twenty(SURVEY_ID + (uint32_t)i,
                       ANCHOR_BASE + i * UINT64_C(0x10001),
                       APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, false,
                       &last_us) == DIRECT_ANCHORS) {
            passed++;
        }
    }
    printf("survey-direct seeded_cases=%zu complete=%zu empirical_percent=%.3f "
           "reliability_claim=none\n", cases, passed,
           100.0 * (double)passed / (double)cases);
    CHECK(passed == cases);
}

static void test_fifty_identity_matrix_and_worst_hash_bin(void)
{
    static const uint32_t survey_ids[] = {
        UINT32_C(0x00000001), UINT32_C(0x01000100),
        UINT32_C(0x50665006), UINT32_C(0x7fffffff),
        UINT32_C(0x80000001), UINT32_C(0xfffffffe),
    };
    size_t cases = 0u;
    size_t complete = 0u;
    uint8_t worst_bin_occupancy = 0u;

    for (size_t survey = 0u;
         survey < sizeof(survey_ids) / sizeof(survey_ids[0]); survey++) {
        uint8_t bins[APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS /
                     APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS + 2u];

        memset(bins, 0, sizeof(bins));
        for (size_t identity = 0u; identity < 50u; identity++) {
            struct app_mesh_direct_gateway_retry_state state;
            struct app_mesh_direct_gateway_retry_decision decision;
            size_t bin;

            CHECK(app_mesh_direct_gateway_retry_init(
                      &state, APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
                      ANCHOR_BASE + identity, survey_ids[survey]) == 0);
            CHECK(app_mesh_direct_gateway_retry_note(
                      &state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED, 0u,
                      &decision) == 0);
            bin = (decision.delay_ms -
                   APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS) /
                  APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS;
            CHECK(bin < sizeof(bins));
            bins[bin]++;
            if (bins[bin] > worst_bin_occupancy) {
                worst_bin_occupancy = bins[bin];
            }
        }
        for (size_t first = 0u; first + DIRECT_ANCHORS <= 50u; first++) {
            uint64_t last_us = 0u;

            cases++;
            if (run_twenty(survey_ids[survey], ANCHOR_BASE + first,
                           APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY, true,
                           &last_us) == DIRECT_ANCHORS) {
                complete++;
            }
        }
    }
    printf("survey-direct identity_matrix_cases=%zu complete=%zu "
           "worst_first_retry_service_bin=%u reliability_claim=none\n",
           cases, complete, worst_bin_occupancy);
    CHECK(complete == cases);
    CHECK(worst_bin_occupancy < DIRECT_ANCHORS);
}

int main(void)
{
    test_legacy_policy_has_correlated_collision_failure();
    test_survey_policy_decorrelates_twenty_direct_reporters();
    test_busy_deferral_does_not_consume_four_real_attempts();
    test_full_width_identity_mixing_breaks_folded_collisions();
    test_policy_deadline_boundary_and_wrap();
    test_seeded_corpus_reports_empirical_result_without_claim();
    test_fifty_identity_matrix_and_worst_hash_bin();
    return failures == 0 ? 0 : 1;
}
