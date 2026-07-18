#include "app_gateway_survey_round.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void context_init(struct survey_gateway_context *context,
                         size_t pair_count,
                         uint16_t sample_count)
{
    assert(pair_count <= SURVEY_GATEWAY_MAX_REPORTS / 2u);
    memset(context, 0, sizeof(*context));
    context->survey_id = UINT32_C(0x27182818);
    context->sample_count = sample_count;
    context->pair_count = pair_count;
    context->pairs_planned = true;

    for (size_t i = 0u; i < pair_count; i++) {
        const uint64_t initiator_id = UINT64_C(0x1000) + 2u * i;
        const uint64_t responder_id = initiator_id + 1u;

        context->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_id = initiator_id,
            .responder_id = responder_id,
        };
        context->reports[2u * i] = (struct survey_gateway_report_slot) {
            .anchor_id = initiator_id,
            .reverse_next_hop_id = initiator_id,
            .reverse_hop_count = 1u,
            .reverse_hint_valid = true,
            .valid = true,
        };
        context->reports[2u * i + 1u] =
            (struct survey_gateway_report_slot) {
                .anchor_id = responder_id,
                .reverse_next_hop_id = responder_id,
                .reverse_hop_count = 1u,
                .reverse_hint_valid = true,
                .valid = true,
            };
    }
    context->report_count = 2u * pair_count;
}

static void dispatch_current_batch(struct app_gateway_survey_round *round)
{
    const enum survey_gateway_auto_stage stages[] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };

    size_t stage_index = 0u;
    size_t previous_lane = SIZE_MAX;

    while (!app_gateway_survey_round_go_needed(round)) {
        struct app_gateway_survey_round_control control;

        assert(app_gateway_survey_round_current_control(round, &control) ==
               PROTO_OK);
        if (control.lane_index != previous_lane) {
            previous_lane = control.lane_index;
            stage_index = 0u;
        }
        assert(control.stage == stages[stage_index]);
        assert(app_gateway_survey_round_note_control_success(
                   round,
                   control.command_id,
                   control.target_id,
                   control.pair.survey_id) == PROTO_OK);
        stage_index++;
    }
    assert(app_gateway_survey_round_go_needed(round));
}

static void test_maximum_25_lane_batch_arms_before_one_go(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;
    bool complete = false;

    context_init(&context, 25u, 2u);
    assert(app_gateway_survey_round_begin(
               &round,
               &context,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
               1u) == PROTO_OK);
    assert(round.planned_round_count == 1u);
    assert(app_gateway_survey_round_lane_count(&round) == 25u);

    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(app_gateway_survey_round_note_control_success(
               &round,
               control.command_id,
               control.target_id + 1u,
               control.pair.survey_id) == PROTO_ERR_NOT_FOUND);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(control.stage == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);

    dispatch_current_batch(&round);
    for (size_t i = 0u; i < 25u; i++) {
        assert(survey_pair_round_lane_armed(
            app_gateway_survey_round_lane(&round, i)));
    }
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    for (size_t i = 0u; i < 25u; i++) {
        assert(app_gateway_survey_round_lane(&round, i)->state ==
               SURVEY_PAIR_ROUND_LANE_OBSERVING);
        assert(app_gateway_survey_round_finalize_lane(
                   &round,
                   i,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    }
    assert(app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(complete);
    assert(app_gateway_survey_round_complete(&round));
    assert(round.runtime.completed_success_count == 25u);
}

static void test_one_lane_failure_rerun_does_not_disturb_peer(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    const struct survey_pair failed_pair = {
        .initiator_id = UINT64_C(0x1002),
        .responder_id = UINT64_C(0x1003),
        .survey_id = UINT32_C(0x27182818),
        .sample_count = 2u,
    };
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    bool complete = true;

    context_init(&context, 2u, 2u);
    assert(app_gateway_survey_round_begin(&round,
                                           &context,
                                           2u,
                                           1u) == PROTO_OK);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);

    sample = (struct survey_sample) {
        .pair = failed_pair,
        .sample_index = 0u,
        .distance_mm = 900,
        .quality = 80u,
        .range_status = RANGE_OK,
    };
    assert(app_gateway_survey_round_note_sample(
               &round,
               failed_pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 1u);
    assert(accepted_new);
    assert(app_gateway_survey_round_lane(&round, 0u)->usable_result_mask ==
           0u);

    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) == PROTO_OK);
    assert(round.runtime.completed_success_count == 1u);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(!app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));

    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(!complete);
    assert(app_gateway_survey_round_lane_count(&round) == 1u);
    assert(app_gateway_survey_round_lane(&round, 0u)->pair.initiator_id ==
           failed_pair.initiator_id);
    assert(app_gateway_survey_round_lane(&round, 0u)->reruns_started == 1u);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_FAIL) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(complete);
    assert(round.runtime.completed_success_count == 1u);
    assert(round.runtime.completed_failure_count == 1u);
}

static void test_control_failure_skips_only_one_lane_before_go(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;
    size_t failed_lane = SIZE_MAX;

    context_init(&context, 2u, 2u);
    assert(app_gateway_survey_round_begin(&round, &context, 2u, 1u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(app_gateway_survey_round_note_control_failure(
               &round,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &failed_lane) == PROTO_OK);
    assert(failed_lane == 0u);
    assert(round.runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_go_needed(&round));
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    assert(round.runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    assert(round.runtime.lanes[1].state ==
           SURVEY_PAIR_ROUND_LANE_OBSERVING);
}

int main(void)
{
    test_maximum_25_lane_batch_arms_before_one_go();
    test_one_lane_failure_rerun_does_not_disturb_peer();
    test_control_failure_skips_only_one_lane_before_go();
    puts("app gateway survey round tests passed");
    return 0;
}
