#include "survey.h"
#include "operation_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_terminal_readiness_sweeps_every_pair_capacity(void)
{
    struct survey_gateway_auto_context automatic = {
        .stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR,
        .running = true,
    };
    struct survey_gateway_context gateway = {0};

    for (size_t pair_count = 1u;
         pair_count <= SURVEY_GATEWAY_MAX_PAIRS;
         pair_count++) {
        gateway.pair_count = pair_count;
        for (size_t next_pair = 0u;
             next_pair <= pair_count;
             next_pair++) {
            gateway.next_pair_index = next_pair;
            assert(survey_gateway_auto_no_unstarted_pairs(
                       &automatic, &gateway) ==
                   (next_pair == pair_count));
        }
    }
}

static void test_inflight_or_unfinished_pair_never_terminalizes_early(void)
{
    struct survey_gateway_auto_context automatic = {
        .stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR,
        .running = true,
    };
    struct survey_gateway_context gateway = {
        .pair_count = SURVEY_GATEWAY_MAX_PAIRS,
        .next_pair_index = SURVEY_GATEWAY_MAX_PAIRS,
    };

    automatic.waiting = true;
    assert(!survey_gateway_auto_no_unstarted_pairs(&automatic, &gateway));
    automatic.waiting = false;

    for (enum survey_gateway_auto_stage stage =
             SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
         stage <= SURVEY_GATEWAY_AUTO_START_INITIATOR;
         stage++) {
        automatic.stage = stage;
        assert(!survey_gateway_auto_no_unstarted_pairs(&automatic, &gateway));
    }

    automatic.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
    automatic.running = false;
    assert(!survey_gateway_auto_no_unstarted_pairs(&automatic, &gateway));
    assert(!survey_gateway_auto_no_unstarted_pairs(NULL, &gateway));
    assert(!survey_gateway_auto_no_unstarted_pairs(&automatic, NULL));
}

static enum survey_gateway_collection_decision collection_decide(
    bool emission_horizon_elapsed,
    bool safety_deadline_elapsed,
    size_t report_count,
    uint16_t expected_count,
    bool expected_present)
{
    return survey_gateway_collection_decide(emission_horizon_elapsed,
                                            safety_deadline_elapsed,
                                            report_count,
                                            expected_count,
                                            expected_present);
}

static void test_expected_count_closes_only_after_full_emission_horizon(void)
{
    for (uint16_t expected = 1u;
         expected <= SURVEY_DISCOVERY_MAX_SLOT_COUNT;
         expected++) {
        assert(collection_decide(false, false, expected, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(false, true, expected, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, false, expected, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_CLOSE);
    }
}

static void test_expected_count_shortfall_keeps_full_safety_deadline(void)
{
    for (uint16_t expected = 1u;
         expected <= SURVEY_DISCOVERY_MAX_SLOT_COUNT;
         expected++) {
        size_t fewer = expected - 1u;

        assert(collection_decide(false, false, fewer, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, false, fewer, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, true, fewer, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH);
    }
}

static void test_expected_count_overflow_fails_without_truncating(void)
{
    for (uint16_t expected = 1u;
         expected < SURVEY_DISCOVERY_MAX_SLOT_COUNT;
         expected++) {
        size_t more = expected + 1u;

        assert(collection_decide(false, false, more, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, false, more, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH);
        assert(collection_decide(true, true, more, expected, true) ==
               SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH);
    }
}

static void test_absent_expected_count_preserves_legacy_safety_deadline(void)
{
    static const size_t report_counts[] = {
        0u,
        1u,
        SURVEY_DISCOVERY_MAX_SLOT_COUNT,
    };

    for (size_t i = 0u;
         i < sizeof(report_counts) / sizeof(report_counts[0]);
         i++) {
        assert(collection_decide(false, false, report_counts[i], 0u, false) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, false, report_counts[i], 0u, false) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(false, true, report_counts[i], 0u, false) ==
               SURVEY_GATEWAY_COLLECTION_WAIT);
        assert(collection_decide(true, true, report_counts[i], 0u, false) ==
               SURVEY_GATEWAY_COLLECTION_CLOSE);
    }
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void test_exact_minimum_budget_leaves_terminal_scheduling_guard(void)
{
    const struct operation_policy_discovery policy = {
        .start_delay_ms = 6000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
        .report_grace_ms = 250u,
        .operation_budget_ms = 600000u,
    };
    const struct operation_policy_discovery_budget_terms terms = {
        .report_slot_ms = 2270u,
        .report_custody_ms = 17000u,
        .report_delivery_tail_ms = 63060u,
        .terminal_scheduling_guard_ms = 102u,
    };
    const uint32_t collection_horizon_ms =
        policy.start_delay_ms +
        policy.slot_ms * policy.slot_count * policy.round_count +
        terms.report_slot_ms * policy.slot_count +
        policy.report_grace_ms +
        terms.report_custody_ms +
        terms.report_delivery_tail_ms;
    const uint32_t origin_ms = UINT32_MAX - collection_horizon_ms + 1u;
    const uint32_t flood_terminal_ms = origin_ms + 5000u;
    const uint32_t collection_deadline_ms =
        origin_ms + collection_horizon_ms;
    const uint32_t report_arrival_ms = collection_deadline_ms - 1u;
    uint32_t required_budget_ms = 0u;
    uint32_t operation_deadline_ms;

    assert(operation_policy_discovery_required_budget_ms(
               &policy, &terms, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms ==
           collection_horizon_ms +
               terms.terminal_scheduling_guard_ms +
               SURVEY_DISCOVERY_OPERATION_TERMINAL_GUARD_MS);
    operation_deadline_ms = origin_ms + required_budget_ms;

    /* The delayed flood terminal only arms the frozen absolute horizon. */
    assert(!deadline_reached(flood_terminal_ms, collection_deadline_ms));
    assert(!deadline_reached(report_arrival_ms, collection_deadline_ms));
    assert(collection_decide(true, false, 1u, 0u, false) ==
           SURVEY_GATEWAY_COLLECTION_WAIT);

    /* This witness deliberately wraps the collection deadline to zero. */
    assert(collection_deadline_ms == 0u);
    assert(deadline_reached(collection_deadline_ms,
                            collection_deadline_ms));
    assert(!deadline_reached(collection_deadline_ms,
                             operation_deadline_ms));
    assert(collection_decide(true, true, 1u, 0u, false) ==
           SURVEY_GATEWAY_COLLECTION_CLOSE);
}

int main(void)
{
    test_terminal_readiness_sweeps_every_pair_capacity();
    test_inflight_or_unfinished_pair_never_terminalizes_early();
    test_expected_count_closes_only_after_full_emission_horizon();
    test_expected_count_shortfall_keeps_full_safety_deadline();
    test_expected_count_overflow_fails_without_truncating();
    test_absent_expected_count_preserves_legacy_safety_deadline();
    test_exact_minimum_budget_leaves_terminal_scheduling_guard();
    puts("survey completion policy tests passed");
    return 0;
}
