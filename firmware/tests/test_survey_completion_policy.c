#include "survey.h"

#include <assert.h>
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

int main(void)
{
    test_terminal_readiness_sweeps_every_pair_capacity();
    test_inflight_or_unfinished_pair_never_terminalizes_early();
    test_expected_count_closes_only_after_full_emission_horizon();
    test_expected_count_shortfall_keeps_full_safety_deadline();
    test_expected_count_overflow_fails_without_truncating();
    test_absent_expected_count_preserves_legacy_safety_deadline();
    puts("survey completion policy tests passed");
    return 0;
}
