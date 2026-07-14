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

int main(void)
{
    test_terminal_readiness_sweeps_every_pair_capacity();
    test_inflight_or_unfinished_pair_never_terminalizes_early();
    puts("survey completion policy tests passed");
    return 0;
}
