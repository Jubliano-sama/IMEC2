#include "survey_pair_round_runtime.h"

#include <string.h>

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS <= UINT8_MAX,
               "round runtime pair indices must cover the complete plan");
_Static_assert(SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES > 0u,
               "round runtime must retain at least one lane");
_Static_assert(SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES <= UINT8_MAX,
               "round runtime lane count must fit compact storage");

static bool survey_pair_round_runtime_pair_equal(
    const struct survey_pair *first,
    const struct survey_pair *second)
{
    return first->operation_generation == second->operation_generation &&
           first->survey_id == second->survey_id &&
           first->initiator_id == second->initiator_id &&
           first->responder_id == second->responder_id &&
           first->sample_count == second->sample_count;
}

static int survey_pair_round_runtime_pair_at(
    const struct survey_pair_round_runtime *runtime,
    uint8_t pair_index,
    struct survey_pair *pair)
{
    return survey_gateway_pair_at(runtime->plan, pair_index, pair);
}

static int survey_pair_round_runtime_validate_plan(
    const struct survey_gateway_context *plan,
    const struct survey_pair_round_metadata *metadata,
    size_t metadata_count,
    uint8_t *round_count)
{
    uint8_t rounds = 0u;

    if (!plan->pairs_planned) {
        return PROTO_ERR_STALE;
    }
    if (plan->pair_count > SURVEY_GATEWAY_MAX_PAIRS ||
        metadata_count < plan->pair_count) {
        return PROTO_ERR_NO_SPACE;
    }
    if (plan->pair_count != 0u && metadata == NULL) {
        return PROTO_ERR_ARG;
    }

    for (size_t i = 0u; i < plan->pair_count; i++) {
        struct survey_pair pair;

        if (survey_gateway_pair_at(plan, i, &pair) != PROTO_OK ||
            pair.sample_count > SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES ||
            metadata[i].pair_count_in_round == 0u ||
            metadata[i].pair_index_in_round >=
                metadata[i].pair_count_in_round) {
            return PROTO_ERR_MALFORMED;
        }
        if (metadata[i].round_index == UINT8_MAX) {
            return PROTO_ERR_MALFORMED;
        }
        if ((uint8_t)(metadata[i].round_index + 1u) > rounds) {
            rounds = (uint8_t)(metadata[i].round_index + 1u);
        }
    }

    for (uint8_t round = 0u; round < rounds; round++) {
        uint8_t expected_count = 0u;
        uint8_t actual_count = 0u;

        for (size_t i = 0u; i < plan->pair_count; i++) {
            if (metadata[i].round_index != round) {
                continue;
            }
            if (expected_count == 0u) {
                expected_count = metadata[i].pair_count_in_round;
            } else if (metadata[i].pair_count_in_round != expected_count) {
                return PROTO_ERR_MALFORMED;
            }
            actual_count++;
        }
        if (expected_count == 0u || actual_count != expected_count) {
            return PROTO_ERR_MALFORMED;
        }
        for (uint8_t position = 0u; position < expected_count; position++) {
            uint8_t matches = 0u;

            for (size_t i = 0u; i < plan->pair_count; i++) {
                if (metadata[i].round_index == round &&
                    metadata[i].pair_index_in_round == position) {
                    matches++;
                }
            }
            if (matches != 1u) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }

    *round_count = rounds;
    return PROTO_OK;
}

int survey_pair_round_runtime_begin(
    struct survey_pair_round_runtime *runtime,
    const struct survey_gateway_context *plan,
    const struct survey_pair_round_metadata *metadata,
    size_t metadata_count,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns)
{
    uint8_t round_count;
    int ret;

    if (runtime == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }
    if (max_parallel_pairs == 0u ||
        max_parallel_pairs > SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES ||
        max_reruns > SURVEY_GATEWAY_PAIR_MAX_RERUNS) {
        return PROTO_ERR_MALFORMED;
    }
    ret = survey_pair_round_runtime_validate_plan(plan,
                                                  metadata,
                                                  metadata_count,
                                                  &round_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->plan = plan;
    runtime->metadata = metadata;
    runtime->metadata_count = metadata_count;
    runtime->max_parallel_pairs = max_parallel_pairs;
    runtime->max_reruns = max_reruns;
    runtime->round_count = round_count;
    runtime->active = true;
    return PROTO_OK;
}

static int survey_pair_round_runtime_load_lane(
    struct survey_pair_round_runtime *runtime,
    struct survey_pair_round_lane *lane,
    uint8_t plan_pair_index,
    uint8_t reruns_started)
{
    int ret;

    memset(lane, 0, sizeof(*lane));
    ret = survey_pair_round_runtime_pair_at(runtime,
                                            plan_pair_index,
                                            &lane->pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    lane->plan_pair_index = plan_pair_index;
    lane->planner_round_index =
        runtime->metadata[plan_pair_index].round_index;
    lane->reruns_started = reruns_started;
    lane->state = SURVEY_PAIR_ROUND_LANE_READY;
    return PROTO_OK;
}

static int survey_pair_round_runtime_find_planned_index(
    const struct survey_pair_round_runtime *runtime,
    uint8_t round_index,
    uint8_t position,
    uint8_t *plan_pair_index)
{
    for (size_t i = 0u; i < runtime->plan->pair_count; i++) {
        if (runtime->metadata[i].round_index == round_index &&
            runtime->metadata[i].pair_index_in_round == position) {
            *plan_pair_index = (uint8_t)i;
            return PROTO_OK;
        }
    }
    return PROTO_ERR_NOT_FOUND;
}

static bool survey_pair_round_lane_attempt_complete(
    const struct survey_pair_round_lane *lane)
{
    return lane->state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED ||
           lane->state == SURVEY_PAIR_ROUND_LANE_FAILED ||
           lane->state == SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED;
}

bool survey_pair_round_runtime_batch_complete(
    const struct survey_pair_round_runtime *runtime)
{
    if (runtime == NULL || !runtime->active || runtime->lane_count == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < runtime->lane_count; i++) {
        if (!survey_pair_round_lane_attempt_complete(&runtime->lanes[i])) {
            return false;
        }
    }
    return true;
}

int survey_pair_round_runtime_load_next_batch(
    struct survey_pair_round_runtime *runtime)
{
    struct survey_pair_round_lane staged[SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES];
    uint8_t staged_count = 0u;
    uint8_t next_pair = 0u;
    uint8_t next_round = 0u;
    uint8_t pending_rerun_count;
    enum survey_pair_round_batch_kind batch_kind;
    int ret;

    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active) {
        return PROTO_ERR_STALE;
    }
    if (runtime->lane_count != 0u &&
        !survey_pair_round_runtime_batch_complete(runtime)) {
        return PROTO_ERR_BUSY;
    }

    next_pair = runtime->next_pair_in_round;
    next_round = runtime->next_planner_round;
    pending_rerun_count = runtime->pending_rerun_count;
    memset(staged, 0, sizeof(staged));

    if (pending_rerun_count != 0u) {
        if (pending_rerun_count > runtime->max_parallel_pairs) {
            return PROTO_ERR_NO_SPACE;
        }
        for (uint8_t i = 0u; i < pending_rerun_count; i++) {
            ret = survey_pair_round_runtime_load_lane(
                runtime,
                &staged[i],
                runtime->pending_reruns[i].plan_pair_index,
                runtime->pending_reruns[i].reruns_started);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
        staged_count = pending_rerun_count;
        pending_rerun_count = 0u;
        batch_kind = SURVEY_PAIR_ROUND_BATCH_RERUN;
    } else {
        uint8_t pair_count_in_round;

        if (next_round >= runtime->round_count) {
            return PROTO_ERR_NOT_FOUND;
        }
        pair_count_in_round = 0u;
        for (size_t i = 0u; i < runtime->plan->pair_count; i++) {
            if (runtime->metadata[i].round_index == next_round) {
                pair_count_in_round =
                    runtime->metadata[i].pair_count_in_round;
                break;
            }
        }
        while (staged_count < runtime->max_parallel_pairs &&
               next_pair < pair_count_in_round) {
            uint8_t plan_pair_index;

            ret = survey_pair_round_runtime_find_planned_index(
                runtime,
                next_round,
                next_pair,
                &plan_pair_index);
            if (ret != PROTO_OK) {
                return ret;
            }
            ret = survey_pair_round_runtime_load_lane(
                runtime,
                &staged[staged_count],
                plan_pair_index,
                0u);
            if (ret != PROTO_OK) {
                return ret;
            }
            staged_count++;
            next_pair++;
        }
        if (next_pair == pair_count_in_round) {
            next_round++;
            next_pair = 0u;
        }
        batch_kind = SURVEY_PAIR_ROUND_BATCH_PLANNED;
    }

    memset(runtime->lanes, 0, sizeof(runtime->lanes));
    if (staged_count > 0u) {
        memcpy(runtime->lanes, staged,
               (size_t)staged_count * sizeof(staged[0]));
    }
    runtime->lane_count = staged_count;
    runtime->next_pair_in_round = next_pair;
    runtime->next_planner_round = next_round;
    runtime->pending_rerun_count = pending_rerun_count;
    runtime->batch_kind = batch_kind;
    runtime->batch_sequence++;
    if (runtime->batch_sequence == 0u) {
        runtime->batch_sequence = 1u;
    }
    return PROTO_OK;
}

size_t survey_pair_round_runtime_lane_count(
    const struct survey_pair_round_runtime *runtime)
{
    return runtime == NULL ? 0u : runtime->lane_count;
}

const struct survey_pair_round_lane *survey_pair_round_runtime_lane(
    const struct survey_pair_round_runtime *runtime,
    size_t lane_index)
{
    if (runtime == NULL || lane_index >= runtime->lane_count) {
        return NULL;
    }
    return &runtime->lanes[lane_index];
}

int survey_pair_round_runtime_mark_observing(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index)
{
    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || lane_index >= runtime->lane_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (runtime->lanes[lane_index].state ==
        SURVEY_PAIR_ROUND_LANE_OBSERVING) {
        return PROTO_OK;
    }
    if (runtime->lanes[lane_index].state !=
        SURVEY_PAIR_ROUND_LANE_ARMED) {
        return PROTO_ERR_STALE;
    }
    runtime->lanes[lane_index].state = SURVEY_PAIR_ROUND_LANE_OBSERVING;
    return PROTO_OK;
}

static bool survey_pair_round_endpoint_mask_valid(uint8_t endpoint_mask)
{
    return endpoint_mask != 0u &&
           (endpoint_mask &
            (uint8_t)~SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) == 0u;
}

int survey_pair_round_runtime_note_prepared(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t endpoint_mask)
{
    struct survey_pair_round_lane *lane;

    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || lane_index >= runtime->lane_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (!survey_pair_round_endpoint_mask_valid(endpoint_mask)) {
        return PROTO_ERR_MALFORMED;
    }
    lane = &runtime->lanes[lane_index];
    if (lane->state != SURVEY_PAIR_ROUND_LANE_READY &&
        lane->state != SURVEY_PAIR_ROUND_LANE_ARMING) {
        return PROTO_ERR_STALE;
    }
    lane->prepared_mask |= endpoint_mask;
    lane->state = SURVEY_PAIR_ROUND_LANE_ARMING;
    return PROTO_OK;
}

int survey_pair_round_runtime_note_started(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t endpoint_mask)
{
    struct survey_pair_round_lane *lane;

    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || lane_index >= runtime->lane_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (!survey_pair_round_endpoint_mask_valid(endpoint_mask)) {
        return PROTO_ERR_MALFORMED;
    }
    lane = &runtime->lanes[lane_index];
    if ((endpoint_mask & (uint8_t)~lane->prepared_mask) != 0u) {
        return PROTO_ERR_STALE;
    }
    if (lane->state == SURVEY_PAIR_ROUND_LANE_ARMED ||
        lane->state == SURVEY_PAIR_ROUND_LANE_OBSERVING) {
        return ((endpoint_mask & (uint8_t)~lane->started_mask) == 0u) ?
                   PROTO_OK : PROTO_ERR_STALE;
    }
    if (lane->state != SURVEY_PAIR_ROUND_LANE_ARMING ||
        lane->prepared_mask != SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) {
        return PROTO_ERR_STALE;
    }
    lane->started_mask |= endpoint_mask;
    if (lane->started_mask == SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) {
        lane->state = SURVEY_PAIR_ROUND_LANE_ARMED;
    }
    return PROTO_OK;
}

bool survey_pair_round_lane_armed(
    const struct survey_pair_round_lane *lane)
{
    return lane != NULL && lane->state == SURVEY_PAIR_ROUND_LANE_ARMED &&
           lane->prepared_mask == SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK &&
           lane->started_mask == SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK;
}

int survey_pair_round_runtime_note_sample(
    struct survey_pair_round_runtime *runtime,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new)
{
    struct survey_pair_round_lane *matched = NULL;
    size_t matched_index = 0u;
    bool changed = false;
    int ret;

    if (runtime == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || sample->round_id != runtime->batch_sequence) {
        return PROTO_ERR_STALE;
    }
    if (survey_sample_validate(sample) != PROTO_OK ||
        sample->sample_index >= SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES ||
        reporter_id != sample->pair.responder_id) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t i = 0u; i < runtime->lane_count; i++) {
        struct survey_pair_round_lane *candidate = &runtime->lanes[i];

        if ((candidate->state != SURVEY_PAIR_ROUND_LANE_OBSERVING &&
             candidate->state != SURVEY_PAIR_ROUND_LANE_ARMED &&
             candidate->state != SURVEY_PAIR_ROUND_LANE_ARMING) ||
            !survey_pair_round_runtime_pair_equal(&candidate->pair,
                                                  &sample->pair)) {
            continue;
        }
        if (matched != NULL) {
            return PROTO_ERR_MALFORMED;
        }
        matched = candidate;
        matched_index = i;
    }
    if (matched == NULL) {
        return PROTO_ERR_STALE;
    }

    ret = survey_pair_note_sample_masks(
        sample,
        reporter_id,
        &matched->usable_result_mask,
        &matched->responder_usable_mask,
        &matched->initiator_unusable_mask,
        &matched->responder_unusable_mask,
        &changed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (lane_index != NULL) {
        *lane_index = matched_index;
    }
    if (accepted_new != NULL) {
        *accepted_new = changed;
    }
    return PROTO_OK;
}

static uint16_t survey_pair_round_expected_mask(uint16_t sample_count)
{
    return sample_count == 16u ? UINT16_MAX :
        (uint16_t)((UINT16_C(1) << sample_count) - 1u);
}

bool survey_pair_round_lane_results_complete(
    const struct survey_pair_round_lane *lane)
{
    if (lane == NULL ||
        lane->pair.sample_count == 0u ||
        lane->pair.sample_count >
            SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES) {
        return false;
    }
    return lane->responder_usable_mask ==
        survey_pair_round_expected_mask(lane->pair.sample_count);
}

bool survey_pair_round_lane_preferred_results_complete(
    const struct survey_pair_round_lane *lane)
{
    if (lane == NULL ||
        lane->pair.sample_count == 0u ||
        lane->pair.sample_count >
            SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES) {
        return false;
    }
    return lane->responder_usable_mask ==
        survey_pair_round_expected_mask(lane->pair.sample_count);
}

bool survey_pair_round_lane_missing_samples_all_unusable(
    const struct survey_pair_round_lane *lane)
{
    return lane != NULL && survey_pair_missing_samples_all_unusable(
        lane->pair.sample_count,
        lane->usable_result_mask,
        lane->initiator_unusable_mask,
        lane->responder_unusable_mask);
}

static int survey_pair_round_runtime_finalize_lane(
    struct survey_pair_round_runtime *runtime,
    struct survey_pair_round_lane *lane)
{
    switch (lane->cleanup_outcome) {
    case SURVEY_PAIR_ROUND_CLEANUP_SUCCESS:
        lane->state = SURVEY_PAIR_ROUND_LANE_SUCCEEDED;
        runtime->completed_success_count++;
        return PROTO_OK;
    case SURVEY_PAIR_ROUND_CLEANUP_FAIL:
        lane->state = SURVEY_PAIR_ROUND_LANE_FAILED;
        runtime->completed_failure_count++;
        return PROTO_OK;
    case SURVEY_PAIR_ROUND_CLEANUP_RETRY:
        if (lane->reruns_started >= runtime->max_reruns) {
            lane->state = SURVEY_PAIR_ROUND_LANE_FAILED;
            runtime->completed_failure_count++;
            return PROTO_OK;
        }
        if (runtime->pending_rerun_count >=
            SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES) {
            return PROTO_ERR_NO_SPACE;
        }
        runtime->pending_reruns[runtime->pending_rerun_count++] =
            (struct survey_pair_round_rerun) {
                .plan_pair_index = lane->plan_pair_index,
                .reruns_started = (uint8_t)(lane->reruns_started + 1u),
            };
        lane->state = SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED;
        return PROTO_OK;
    default:
        return PROTO_ERR_MALFORMED;
    }
}

int survey_pair_round_runtime_require_cleanup(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome)
{
    struct survey_pair_round_lane *lane;

    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || lane_index >= runtime->lane_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    if ((cleanup_mask & (uint8_t)~SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) != 0u ||
        outcome < SURVEY_PAIR_ROUND_CLEANUP_SUCCESS ||
        outcome > SURVEY_PAIR_ROUND_CLEANUP_FAIL) {
        return PROTO_ERR_MALFORMED;
    }
    lane = &runtime->lanes[lane_index];
    if (lane->state != SURVEY_PAIR_ROUND_LANE_READY &&
        lane->state != SURVEY_PAIR_ROUND_LANE_ARMING &&
        lane->state != SURVEY_PAIR_ROUND_LANE_ARMED &&
        lane->state != SURVEY_PAIR_ROUND_LANE_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    lane->cleanup_required_mask = cleanup_mask;
    lane->cleanup_mask = cleanup_mask;
    lane->cleanup_outcome = outcome;
    lane->state = SURVEY_PAIR_ROUND_LANE_CLEANUP;
    if (cleanup_mask == 0u) {
        return survey_pair_round_runtime_finalize_lane(runtime, lane);
    }
    return PROTO_OK;
}

int survey_pair_round_runtime_note_cleanup_complete(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t completed_mask)
{
    struct survey_pair_round_lane *lane;

    if (runtime == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!runtime->active || lane_index >= runtime->lane_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    lane = &runtime->lanes[lane_index];
    if (lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP) {
        return PROTO_ERR_STALE;
    }
    if (completed_mask == 0u ||
        (completed_mask & (uint8_t)~lane->cleanup_required_mask) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    lane->cleanup_mask &= (uint8_t)~completed_mask;
    if (lane->cleanup_mask != 0u) {
        return PROTO_OK;
    }
    return survey_pair_round_runtime_finalize_lane(runtime, lane);
}

bool survey_pair_round_runtime_complete(
    const struct survey_pair_round_runtime *runtime)
{
    if (runtime == NULL || !runtime->active) {
        return false;
    }
    if (runtime->pending_rerun_count != 0u ||
        runtime->next_planner_round < runtime->round_count) {
        return false;
    }
    return runtime->lane_count == 0u ||
           survey_pair_round_runtime_batch_complete(runtime);
}
