#include "survey_pair_round_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void plan_init(struct survey_gateway_context *plan,
                      struct survey_pair_round_metadata *metadata,
                      size_t pair_count,
                      uint16_t sample_count)
{
    assert(pair_count <= SURVEY_GATEWAY_MAX_PAIRS);
    assert((2u * pair_count) <= SURVEY_GATEWAY_MAX_REPORTS);
    assert(survey_gateway_begin(
               plan, UINT32_C(0xA1B2C3D4), sample_count) == PROTO_OK);
    for (size_t i = 0u; i < pair_count; i++) {
        const uint8_t initiator_index = plan->node_count++;
        const uint8_t responder_index = plan->node_count++;

        plan->node_ids[initiator_index] =
            UINT64_C(0x1000) + (2u * i);
        plan->node_ids[responder_index] =
            UINT64_C(0x1001) + (2u * i);
        plan->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_index = initiator_index,
            .responder_index = responder_index,
        };
        metadata[i] = (struct survey_pair_round_metadata) {
            .round_index = 0u,
            .pair_index_in_round = (uint8_t)i,
            .pair_count_in_round = (uint8_t)pair_count,
        };
    }
    plan->pair_count = (uint8_t)pair_count;
    plan->pairs_planned = true;
}

static uint8_t round_node_index(struct survey_gateway_context *context,
                                uint64_t node_id,
                                bool create)
{
    for (uint8_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == node_id) {
            return i;
        }
    }
    assert(create);
    assert(context->node_count < SURVEY_GATEWAY_MAX_REPORTS);
    context->node_ids[context->node_count] = node_id;
    return context->node_count++;
}

static struct survey_gateway_report_slot *round_report(
    struct survey_gateway_context *context,
    uint64_t anchor_id)
{
    const uint8_t existing_index =
        round_node_index(context, anchor_id, true);
    struct survey_gateway_reverse_hint reverse_hint = {
        .target_id = anchor_id,
        .next_hop_id = anchor_id,
        .quality = 100u,
        .hop_count = 1u,
        .valid = true,
    };

    if (context->reports[existing_index].metadata == UINT8_MAX) {
        assert(survey_gateway_note_reach_report_with_reverse_hint(
                   context,
                   context->survey_id,
                   anchor_id,
                   NULL,
                   0u,
                   &reverse_hint) == PROTO_OK);
    }
    return &context->reports[existing_index];
}

static void planner_context_init(struct survey_gateway_context *context)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA1u, .responder_id = 0xB1u},
        {.initiator_id = 0xC1u, .responder_id = 0xD1u},
    };

    assert(survey_gateway_begin(context, 1u, 2u) == PROTO_OK);
    for (size_t i = 0u; i < 2u; i++) {
        (void)round_report(context, pairs[i].initiator_id);
        (void)round_report(context, pairs[i].responder_id);
        context->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_index =
                round_node_index(context, pairs[i].initiator_id, false),
            .responder_index =
                round_node_index(context, pairs[i].responder_id, false),
        };
    }
    context->pair_count = 2u;
    context->pairs_planned = true;
}

static void test_planner_serializes_shared_reverse_next_hop(void)
{
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    planner_context_init(&context);
    const uint8_t shared_index = round_node_index(&context, 0xEEu, true);

    round_report(&context, 0xA1u)->reverse_next_hop_index = shared_index;
    round_report(&context, 0xC1u)->reverse_next_hop_index = shared_index;
    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 2u);
    assert(metadata[0].round_index != metadata[1].round_index);
}

static void test_planner_serializes_endpoint_used_as_reverse_next_hop(void)
{
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    struct survey_gateway_report_slot *first;
    size_t round_count = 0u;

    planner_context_init(&context);
    first = round_report(&context, 0xA1u);
    first->reverse_next_hop_index = UINT8_MAX;
    round_report(&context, 0xC1u)->reverse_next_hop_index =
        round_node_index(&context, 0xA1u, false);
    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 2u);
}

static void test_planner_keeps_proven_separated_reverse_paths_parallel(void)
{
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    planner_context_init(&context);
    round_report(&context, 0xA1u)->reverse_hop_count = 1u;
    round_report(&context, 0xB1u)->reverse_hop_count = 1u;
    round_report(&context, 0xC1u)->reverse_hop_count = 3u;
    round_report(&context, 0xD1u)->reverse_hop_count = 3u;
    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 1u);
    assert(metadata[0].round_index == metadata[1].round_index);
}

static struct survey_sample lane_sample(
    const struct survey_pair_round_runtime *runtime,
    const struct survey_pair_round_lane *lane,
    uint16_t sample_index,
    int32_t distance_mm,
    enum range_status status)
{
    return (struct survey_sample) {
        .pair = lane->pair,
        .round_id = runtime->batch_sequence,
        .sample_index = sample_index,
        .distance_mm = distance_mm,
        .quality = 80u,
        .range_status = status,
    };
}

static void arm_lane(struct survey_pair_round_runtime *runtime,
                     size_t lane_index)
{
    assert(survey_pair_round_runtime_note_prepared(
               runtime,
               lane_index,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_note_prepared(
               runtime,
               lane_index,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_note_started(
               runtime,
               lane_index,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_note_started(
               runtime,
               lane_index,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(survey_pair_round_lane_armed(&runtime->lanes[lane_index]));
}

static void test_exact_result_demux_and_usability_are_lane_local(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[2];
    struct survey_pair_round_runtime runtime;
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;

    plan_init(&plan, metadata, 2u, 2u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           2u,
                                           2u,
                                           1u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    arm_lane(&runtime, 0u);
    arm_lane(&runtime, 1u);
    assert(survey_pair_round_runtime_mark_observing(&runtime, 0u) == PROTO_OK);
    assert(survey_pair_round_runtime_mark_observing(&runtime, 1u) == PROTO_OK);

    sample = lane_sample(&runtime,
                         &runtime.lanes[1],
                         0u,
                         1200,
                         RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_ERR_MALFORMED);
    assert(runtime.lanes[0].usable_result_mask == 0u);
    assert(runtime.lanes[1].usable_result_mask == 0u);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 1u);
    assert(accepted_new);
    assert(runtime.lanes[0].usable_result_mask == 0u);
    assert(runtime.lanes[1].usable_result_mask == 0x01u);

    accepted_new = true;
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(!accepted_new);

    sample.pair.sample_count = 1u;
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               NULL,
               NULL) == PROTO_ERR_STALE);
    sample.pair = runtime.lanes[1].pair;
    assert(survey_pair_round_runtime_note_sample(&runtime,
                                                  0xDEADu,
                                                  &sample,
                                                  NULL,
                                                  NULL) ==
           PROTO_ERR_MALFORMED);

    for (uint16_t index = 0u; index < 2u; index++) {
        sample = lane_sample(&runtime,
                             &runtime.lanes[0],
                             index,
                             0,
                             RANGE_RX_TIMEOUT);
        assert(survey_pair_round_runtime_note_sample(
                   &runtime,
                   sample.pair.responder_id,
                   &sample,
                   NULL,
                   NULL) == PROTO_OK);
    }
    assert(survey_pair_round_lane_missing_samples_all_unusable(
        &runtime.lanes[0]));

    sample = lane_sample(&runtime,
                         &runtime.lanes[0],
                         0u,
                         800,
                         RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               NULL,
               &accepted_new) == PROTO_OK);
    assert(accepted_new);
    assert(survey_pair_round_lane_missing_samples_all_unusable(
        &runtime.lanes[0]));
    assert((runtime.lanes[0].initiator_unusable_mask & 0x01u) == 0u);
    assert((runtime.lanes[0].responder_unusable_mask & 0x01u) == 0u);

    sample = lane_sample(&runtime,
                         &runtime.lanes[0],
                         1u,
                         850,
                         RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               NULL,
               NULL) == PROTO_OK);
    assert(!survey_pair_round_lane_missing_samples_all_unusable(
        &runtime.lanes[0]));

    sample = lane_sample(&runtime,
                         &runtime.lanes[1],
                         1u,
                         900,
                         RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               NULL,
               NULL) == PROTO_OK);
    assert(survey_pair_round_lane_results_complete(&runtime.lanes[1]));
    assert(survey_pair_round_lane_results_complete(&runtime.lanes[0]));
}

static void test_initiator_reporter_is_rejected_and_responder_is_idempotent(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[1];
    struct survey_pair_round_runtime runtime;
    struct survey_sample sample;
    bool accepted_new;

    plan_init(&plan, metadata, 1u, 3u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           1u,
                                           1u,
                                           0u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    arm_lane(&runtime, 0u);
    assert(survey_pair_round_runtime_mark_observing(&runtime, 0u) ==
           PROTO_OK);

    for (uint16_t i = 0u; i < runtime.lanes[0].pair.sample_count; i++) {
        sample = lane_sample(&runtime,
                             &runtime.lanes[0],
                             i,
                             1000,
                             RANGE_OK);
        accepted_new = true;
        assert(survey_pair_round_runtime_note_sample(
                   &runtime,
                   sample.pair.initiator_id,
                   &sample,
                   NULL,
                   &accepted_new) == PROTO_ERR_MALFORMED);
        assert(accepted_new);
        assert(runtime.lanes[0].usable_result_mask ==
               (uint16_t)((UINT16_C(1) << i) - 1u));
        assert(survey_pair_round_runtime_note_sample(
                   &runtime,
                   sample.pair.responder_id,
                   &sample,
                   NULL,
                   &accepted_new) == PROTO_OK);
        assert(accepted_new);
        assert(survey_pair_round_runtime_note_sample(
                   &runtime,
                   sample.pair.responder_id,
                   &sample,
                   NULL,
                   &accepted_new) == PROTO_OK);
        assert(!accepted_new);
    }
    assert(survey_pair_round_lane_results_complete(&runtime.lanes[0]));
    assert(survey_pair_round_lane_preferred_results_complete(
        &runtime.lanes[0]));
}

static void test_delayed_prior_batch_sample_cannot_complete_rerun(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[1];
    struct survey_pair_round_runtime runtime;
    struct survey_sample delayed_sample;
    struct survey_sample current_sample;
    struct survey_pair first_attempt_pair;
    uint16_t first_round_id;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;

    plan_init(&plan, metadata, 1u, 1u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           1u,
                                           1u,
                                           1u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_PLANNED);
    first_round_id = runtime.batch_sequence;
    first_attempt_pair = runtime.lanes[0].pair;
    arm_lane(&runtime, 0u);
    assert(survey_pair_round_runtime_mark_observing(&runtime, 0u) ==
           PROTO_OK);
    delayed_sample = lane_sample(&runtime,
                                 &runtime.lanes[0],
                                 0u,
                                 1250,
                                 RANGE_OK);

    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_batch_complete(&runtime));

    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_RERUN);
    assert(runtime.batch_sequence != first_round_id);
    assert(runtime.lanes[0].pair.survey_id == first_attempt_pair.survey_id);
    assert(runtime.lanes[0].pair.initiator_id ==
           first_attempt_pair.initiator_id);
    assert(runtime.lanes[0].pair.responder_id ==
           first_attempt_pair.responder_id);
    assert(runtime.lanes[0].pair.sample_count ==
           first_attempt_pair.sample_count);
    arm_lane(&runtime, 0u);
    assert(survey_pair_round_runtime_mark_observing(&runtime, 0u) ==
           PROTO_OK);

    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               delayed_sample.pair.responder_id,
               &delayed_sample,
               &lane_index,
               &accepted_new) == PROTO_ERR_STALE);
    assert(runtime.lanes[0].usable_result_mask == 0u);
    assert(runtime.lanes[0].initiator_unusable_mask == 0u);
    assert(runtime.lanes[0].responder_unusable_mask == 0u);

    current_sample = lane_sample(&runtime,
                                 &runtime.lanes[0],
                                 0u,
                                 1250,
                                 RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               current_sample.pair.responder_id,
               &current_sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 0u);
    assert(accepted_new);
    assert(runtime.lanes[0].usable_result_mask == 0x01u);
}

static void test_chunks_cleanup_and_reruns_remain_isolated(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[3];
    struct survey_pair_round_runtime runtime;

    plan_init(&plan, metadata, 3u, 2u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           3u,
                                           2u,
                                           1u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_PLANNED);
    assert(runtime.lane_count == 2u);
    assert(runtime.lanes[0].plan_pair_index == 0u);
    assert(runtime.lanes[1].plan_pair_index == 1u);

    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    assert(runtime.lanes[1].state == SURVEY_PAIR_ROUND_LANE_CLEANUP);
    assert(!survey_pair_round_runtime_batch_complete(&runtime));

    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(runtime.lanes[1].state == SURVEY_PAIR_ROUND_LANE_CLEANUP);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(runtime.lanes[1].state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED);
    assert(survey_pair_round_runtime_batch_complete(&runtime));

    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_RERUN);
    assert(runtime.lane_count == 1u);
    assert(runtime.lanes[0].plan_pair_index == 0u);
    assert(runtime.lanes[0].reruns_started == 1u);
    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(runtime.lanes[0].state == SURVEY_PAIR_ROUND_LANE_FAILED);
    assert(survey_pair_round_runtime_batch_complete(&runtime));

    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_PLANNED);
    assert(runtime.lane_count == 1u);
    assert(runtime.lanes[0].plan_pair_index == 2u);
    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(survey_pair_round_runtime_complete(&runtime));
    assert(runtime.completed_success_count == 2u);
    assert(runtime.completed_failure_count == 1u);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_interleaved_round_metadata_loads_in_round_position_order(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[4];
    struct survey_pair_round_runtime runtime;

    plan_init(&plan, metadata, 4u, 1u);
    metadata[0] = (struct survey_pair_round_metadata) {0u, 0u, 2u};
    metadata[1] = (struct survey_pair_round_metadata) {1u, 0u, 2u};
    metadata[2] = (struct survey_pair_round_metadata) {0u, 1u, 2u};
    metadata[3] = (struct survey_pair_round_metadata) {1u, 1u, 2u};

    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           4u,
                                           2u,
                                           0u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.lanes[0].plan_pair_index == 0u);
    assert(runtime.lanes[1].plan_pair_index == 2u);
    for (size_t i = 0u; i < runtime.lane_count; i++) {
        assert(survey_pair_round_runtime_require_cleanup(
                   &runtime,
                   i,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    }

    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.lanes[0].plan_pair_index == 1u);
    assert(runtime.lanes[1].plan_pair_index == 3u);
    for (size_t i = 0u; i < runtime.lane_count; i++) {
        assert(survey_pair_round_runtime_require_cleanup(
                   &runtime,
                   i,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    }
    assert(survey_pair_round_runtime_complete(&runtime));
}

static void test_maximum_runtime_cap_is_bounded(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata
        metadata[SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES];
    struct survey_pair_round_runtime runtime;

    plan_init(&plan,
              metadata,
              SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
              1u);
    assert(survey_pair_round_runtime_begin(
               &runtime,
               &plan,
               metadata,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
               0u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(runtime.lane_count == SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES);
    for (size_t i = 0u; i < runtime.lane_count; i++) {
        assert(survey_pair_round_runtime_require_cleanup(
                   &runtime,
                   i,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    }
    assert(survey_pair_round_runtime_complete(&runtime));
    assert(runtime.completed_success_count ==
           SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES);

    assert(survey_pair_round_runtime_begin(
               &runtime,
               &plan,
               metadata,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES + 1u,
               0u) == PROTO_ERR_MALFORMED);
}

static void test_batch_load_failure_does_not_advance_cursors(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[2];
    struct survey_pair_round_runtime runtime;

    plan_init(&plan, metadata, 2u, 1u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           2u,
                                           2u,
                                           0u) == PROTO_OK);
    metadata[1].pair_index_in_round = 5u;
    assert(survey_pair_round_runtime_load_next_batch(&runtime) ==
           PROTO_ERR_NOT_FOUND);
    assert(runtime.lane_count == 0u);
    assert(runtime.next_pair_in_round == 0u);
    assert(runtime.next_planner_round == 0u);
    assert(runtime.batch_kind == SURVEY_PAIR_ROUND_BATCH_NONE);
}

static void test_duplicate_cleanup_completion_is_idempotent(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[1];
    struct survey_pair_round_runtime runtime;

    plan_init(&plan, metadata, 1u, 1u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           1u,
                                           1u,
                                           0u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    assert(survey_pair_round_runtime_require_cleanup(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(runtime.lanes[0].state == SURVEY_PAIR_ROUND_LANE_CLEANUP);
    assert(survey_pair_round_runtime_note_cleanup_complete(
               &runtime,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(runtime.lanes[0].state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED);
}

static void test_armed_lane_accepts_current_attempt_sample(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[1];
    struct survey_pair_round_runtime runtime;
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;

    plan_init(&plan, metadata, 1u, 1u);
    assert(survey_pair_round_runtime_begin(&runtime,
                                           &plan,
                                           metadata,
                                           1u,
                                           1u,
                                           0u) == PROTO_OK);
    assert(survey_pair_round_runtime_load_next_batch(&runtime) == PROTO_OK);
    arm_lane(&runtime, 0u);
    sample = lane_sample(&runtime, &runtime.lanes[0], 0u, 1250, RANGE_OK);
    assert(survey_pair_round_runtime_note_sample(
               &runtime,
               sample.pair.responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 0u);
    assert(accepted_new);
    assert(runtime.lanes[0].state == SURVEY_PAIR_ROUND_LANE_ARMED);
}

static void test_compact_pair_indices_fail_before_runtime_mutation(void)
{
    struct survey_gateway_context plan;
    struct survey_pair_round_metadata metadata[1];
    struct survey_pair_round_runtime runtime;
    struct survey_pair_round_runtime before;

    plan_init(&plan, metadata, 1u, 1u);
    memset(&runtime, 0xa5, sizeof(runtime));
    before = runtime;
    plan.pairs[0].responder_index = plan.node_count;
    assert(survey_pair_round_runtime_begin(
               &runtime, &plan, metadata, 1u, 1u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);

    plan_init(&plan, metadata, 1u, 1u);
    plan.pairs[0].responder_index = plan.pairs[0].initiator_index;
    assert(survey_pair_round_runtime_begin(
               &runtime, &plan, metadata, 1u, 1u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);

    plan_init(&plan, metadata, 1u, 1u);
    plan.node_ids[1] = plan.node_ids[0];
    assert(survey_pair_round_runtime_begin(
               &runtime, &plan, metadata, 1u, 1u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
}

int main(void)
{
    test_planner_serializes_shared_reverse_next_hop();
    test_planner_serializes_endpoint_used_as_reverse_next_hop();
    test_planner_keeps_proven_separated_reverse_paths_parallel();
    test_exact_result_demux_and_usability_are_lane_local();
    test_initiator_reporter_is_rejected_and_responder_is_idempotent();
    test_delayed_prior_batch_sample_cannot_complete_rerun();
    test_chunks_cleanup_and_reruns_remain_isolated();
    test_interleaved_round_metadata_loads_in_round_position_order();
    test_maximum_runtime_cap_is_bounded();
    test_batch_load_failure_does_not_advance_cursors();
    test_duplicate_cleanup_completion_is_idempotent();
    test_armed_lane_accepts_current_attempt_sample();
    test_compact_pair_indices_fail_before_runtime_mutation();
    puts("survey pair round runtime tests passed");
    return 0;
}
