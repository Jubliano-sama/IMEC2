#include "app_gateway_survey_round.h"

#include <errno.h>
#include <string.h>

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS == 150u,
               "gateway round wrapper must retain the complete pair map");
_Static_assert(SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES == 25u,
               "gateway round wrapper must retain the runtime lane cap");

bool app_gateway_survey_round_go_submit_retryable(int error)
{
    return error == -EAGAIN || error == -EBUSY ||
           error == -ENOSPC || error == -ESHUTDOWN;
}

bool app_gateway_survey_round_go_terminal_retryable(
    enum node_comm_terminal_reason reason,
    uint8_t attempts_started)
{
    if (attempts_started != 0u) {
        return false;
    }
    return reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ||
           reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED ||
           reason == NODE_COMM_TERMINAL_CANCELLED;
}

static int app_gateway_survey_round_stage_details(
    const struct survey_pair_round_lane *lane,
    enum survey_gateway_auto_stage stage,
    enum command_id *command_id,
    uint64_t *target_id,
    uint8_t *endpoint_mask)
{
    if (lane == NULL || command_id == NULL || target_id == NULL ||
        endpoint_mask == NULL) {
        return PROTO_ERR_ARG;
    }

    switch (stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
        *command_id = CMD_SURVEY_PREPARE_PAIR;
        *target_id = lane->pair.initiator_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK;
        return PROTO_OK;
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        *command_id = CMD_SURVEY_PREPARE_PAIR;
        *target_id = lane->pair.responder_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK;
        return PROTO_OK;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
        *command_id = CMD_SURVEY_START_PAIR;
        *target_id = lane->pair.responder_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK;
        return PROTO_OK;
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        *command_id = CMD_SURVEY_START_PAIR;
        *target_id = lane->pair.initiator_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK;
        return PROTO_OK;
    default:
        return PROTO_ERR_STALE;
    }
}

static void app_gateway_survey_round_start_dispatch(
    struct app_gateway_survey_round *round)
{
    round->dispatch_lane_index = 0u;
    round->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
    round->phase = APP_GATEWAY_SURVEY_ROUND_DISPATCHING;
}

int app_gateway_survey_round_begin(
    struct app_gateway_survey_round *round,
    const struct survey_gateway_context *planned_context,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns)
{
    size_t planned_round_count = 0u;
    int ret;

    if (round == NULL || planned_context == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(round, 0, sizeof(*round));
    memset(round->sample_identities,
           SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID,
           sizeof(round->sample_identities));
    ret = survey_gateway_plan_pair_rounds(planned_context,
                                          round->metadata,
                                          SURVEY_GATEWAY_MAX_PAIRS,
                                          &planned_round_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_pair_round_runtime_begin(&round->runtime,
                                           planned_context,
                                           round->metadata,
                                           planned_context->pair_count,
                                           max_parallel_pairs,
                                           max_reruns);
    if (ret != PROTO_OK) {
        return ret;
    }
    round->planned_round_count = planned_round_count;
    ret = survey_pair_round_runtime_load_next_batch(&round->runtime);
    if (ret != PROTO_OK) {
        round->phase = ret == PROTO_ERR_NOT_FOUND ?
            APP_GATEWAY_SURVEY_ROUND_COMPLETE :
            APP_GATEWAY_SURVEY_ROUND_INACTIVE;
        return ret;
    }
    app_gateway_survey_round_start_dispatch(round);
    return PROTO_OK;
}

size_t app_gateway_survey_round_lane_count(
    const struct app_gateway_survey_round *round)
{
    return round == NULL ? 0u :
        survey_pair_round_runtime_lane_count(&round->runtime);
}

const struct survey_pair_round_lane *app_gateway_survey_round_lane(
    const struct app_gateway_survey_round *round,
    size_t lane_index)
{
    return round == NULL ? NULL :
        survey_pair_round_runtime_lane(&round->runtime, lane_index);
}

int app_gateway_survey_round_current_control(
    const struct app_gateway_survey_round *round,
    struct app_gateway_survey_round_control *control)
{
    const struct survey_pair_round_lane *lane;
    uint8_t endpoint_mask;
    int ret;

    if (round == NULL || control == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_DISPATCHING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&round->runtime,
                                          round->dispatch_lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }

    memset(control, 0, sizeof(*control));
    ret = app_gateway_survey_round_stage_details(lane,
                                                  round->dispatch_stage,
                                                  &control->command_id,
                                                  &control->target_id,
                                                  &endpoint_mask);
    if (ret != PROTO_OK) {
        return ret;
    }
    control->pair = lane->pair;
    control->stage = round->dispatch_stage;
    control->lane_index = round->dispatch_lane_index;
    return PROTO_OK;
}

static bool app_gateway_survey_round_lane_attempt_terminal(
    const struct survey_pair_round_lane *lane)
{
    return lane != NULL &&
           (lane->state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_FAILED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
}

static bool app_gateway_survey_round_every_live_lane_armed(
    const struct app_gateway_survey_round *round)
{
    const size_t lane_count = app_gateway_survey_round_lane_count(round);
    bool armed = false;

    if (lane_count == 0u) {
        return false;
    }
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&round->runtime, i);

        if (survey_pair_round_lane_armed(lane)) {
            armed = true;
            continue;
        }
        if (!app_gateway_survey_round_lane_attempt_terminal(lane)) {
            return false;
        }
    }
    return armed;
}

static void app_gateway_survey_round_advance_dispatch(
    struct app_gateway_survey_round *round)
{
    round->dispatch_lane_index++;
    while (round->dispatch_lane_index <
           app_gateway_survey_round_lane_count(round)) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&round->runtime,
                                            round->dispatch_lane_index);

        if (!app_gateway_survey_round_lane_attempt_terminal(lane)) {
            round->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
            round->phase = APP_GATEWAY_SURVEY_ROUND_DISPATCHING;
            return;
        }
        round->dispatch_lane_index++;
    }
    round->dispatch_stage = SURVEY_GATEWAY_AUTO_IDLE;
    if (app_gateway_survey_round_every_live_lane_armed(round)) {
        round->phase = APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED;
    } else if (survey_pair_round_runtime_batch_complete(&round->runtime)) {
        round->phase = APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE;
    }
}

int app_gateway_survey_round_note_control_success(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id)
{
    const struct survey_pair_round_lane *lane;
    enum command_id expected_command;
    uint64_t expected_target;
    uint8_t endpoint_mask;
    int ret;

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_DISPATCHING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&round->runtime,
                                          round->dispatch_lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = app_gateway_survey_round_stage_details(lane,
                                                  round->dispatch_stage,
                                                  &expected_command,
                                                  &expected_target,
                                                  &endpoint_mask);
    if (ret != PROTO_OK) {
        return ret;
    }
    const uint32_t operation_session_id =
        lane->pair.operation_generation == 0u ?
            lane->pair.survey_id :
            survey_operation_session_id(
                lane->pair.operation_generation);

    if (command_id != expected_command || target_id != expected_target ||
        survey_id != operation_session_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    switch (round->dispatch_stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        ret = survey_pair_round_runtime_note_prepared(&round->runtime,
                                                       round->dispatch_lane_index,
                                                       endpoint_mask);
        break;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        ret = survey_pair_round_runtime_note_started(&round->runtime,
                                                      round->dispatch_lane_index,
                                                      endpoint_mask);
        break;
    default:
        return PROTO_ERR_STALE;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    switch (round->dispatch_stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
        round->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER;
        break;
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_RESPONDER;
        break;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
        round->dispatch_stage = SURVEY_GATEWAY_AUTO_START_INITIATOR;
        break;
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        app_gateway_survey_round_advance_dispatch(round);
        break;
    default:
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

int app_gateway_survey_round_note_control_failure(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome,
    size_t *lane_index)
{
    struct app_gateway_survey_round_control control;
    int ret;

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = app_gateway_survey_round_current_control(round, &control);
    if (ret != PROTO_OK) {
        return ret;
    }
    const uint32_t operation_session_id =
        control.pair.operation_generation == 0u ?
            control.pair.survey_id :
            survey_operation_session_id(
                control.pair.operation_generation);

    if (control.command_id != command_id || control.target_id != target_id ||
        operation_session_id != survey_id) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = survey_pair_round_runtime_require_cleanup(&round->runtime,
                                                     control.lane_index,
                                                     cleanup_mask,
                                                     outcome);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (lane_index != NULL) {
        *lane_index = control.lane_index;
    }
    app_gateway_survey_round_advance_dispatch(round);
    return PROTO_OK;
}

bool app_gateway_survey_round_go_needed(
    const struct app_gateway_survey_round *round)
{
    return round != NULL &&
           round->phase == APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED &&
           app_gateway_survey_round_every_live_lane_armed(round);
}

int app_gateway_survey_round_mark_observing_after_go(
    struct app_gateway_survey_round *round)
{
    const size_t lane_count = app_gateway_survey_round_lane_count(round);

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!app_gateway_survey_round_go_needed(round)) {
        return PROTO_ERR_STALE;
    }
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&round->runtime, i);
        int ret;

        if (!survey_pair_round_lane_armed(lane)) {
            continue;
        }
        ret = survey_pair_round_runtime_mark_observing(&round->runtime, i);

        if (ret != PROTO_OK) {
            return ret;
        }
    }
    round->phase = APP_GATEWAY_SURVEY_ROUND_OBSERVING;
    return PROTO_OK;
}

static bool app_gateway_survey_round_pair_equal(
    const struct survey_pair *left,
    const struct survey_pair *right)
{
    return left != NULL && right != NULL &&
           left->operation_generation == right->operation_generation &&
           left->survey_id == right->survey_id &&
           left->initiator_id == right->initiator_id &&
           left->responder_id == right->responder_id &&
           left->sample_count == right->sample_count;
}

int app_gateway_survey_round_preflight_sample(
    const struct app_gateway_survey_round *round,
    enum survey_pair_round_lane_state admissible_lane_state,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *duplicate)
{
    const struct survey_pair_round_lane *matched = NULL;
    struct survey_sample_observation_identity identity;
    const struct survey_sample_observation_identity *existing_identity;
    size_t matched_index = SIZE_MAX;
    size_t reporter_index;
    int ret;

    if (round == NULL || sample == NULL || duplicate == NULL) {
        return PROTO_ERR_ARG;
    }
    if ((admissible_lane_state != SURVEY_PAIR_ROUND_LANE_ARMED &&
         admissible_lane_state != SURVEY_PAIR_ROUND_LANE_OBSERVING) ||
        !round->runtime.active ||
        sample->round_id != round->runtime.batch_sequence) {
        return PROTO_ERR_STALE;
    }
    if (survey_sample_validate(sample) != PROTO_OK ||
        sample->pair.sample_count >
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT ||
        sample->sample_index >=
            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT ||
        (reporter_id != sample->pair.initiator_id &&
         reporter_id != sample->pair.responder_id)) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t i = 0u; i < round->runtime.lane_count; i++) {
        const struct survey_pair_round_lane *candidate =
            &round->runtime.lanes[i];

        if (candidate->state != admissible_lane_state ||
            !app_gateway_survey_round_pair_equal(
                &candidate->pair, &sample->pair)) {
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

    reporter_index =
        reporter_id == sample->pair.responder_id ? 1u : 0u;
    ret = survey_sample_observation_identity_capture(sample, &identity);
    if (ret != PROTO_OK) {
        return ret;
    }
    existing_identity =
        &round->sample_identities[matched_index][reporter_index]
                                 [sample->sample_index];
    if (survey_sample_observation_identity_valid(existing_identity) &&
        !survey_sample_observation_identity_equal(
            existing_identity, &identity)) {
        return PROTO_ERR_MALFORMED;
    }
    if (lane_index != NULL) {
        *lane_index = matched_index;
    }
    *duplicate =
        survey_sample_observation_identity_valid(existing_identity);
    return PROTO_OK;
}

int app_gateway_survey_round_note_sample(
    struct app_gateway_survey_round *round,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new)
{
    bool duplicate = false;
    struct survey_sample_observation_identity identity;
    size_t matched_index = SIZE_MAX;
    size_t reporter_index;
    int ret;

    if (round == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    ret = app_gateway_survey_round_preflight_sample(
        round,
        SURVEY_PAIR_ROUND_LANE_OBSERVING,
        reporter_id,
        sample,
        &matched_index,
        &duplicate);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (lane_index != NULL) {
        *lane_index = matched_index;
    }
    if (duplicate) {
        if (accepted_new != NULL) {
            *accepted_new = false;
        }
        return PROTO_OK;
    }

    ret = survey_pair_round_runtime_note_sample(&round->runtime,
                                                 reporter_id,
                                                 sample,
                                                 NULL,
                                                 NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    reporter_index =
        reporter_id == sample->pair.responder_id ? 1u : 0u;
    ret = survey_sample_observation_identity_capture(sample, &identity);
    if (ret != PROTO_OK) {
        return ret;
    }
    round->sample_identities[matched_index][reporter_index]
                            [sample->sample_index] = identity;
    if (accepted_new != NULL) {
        *accepted_new = true;
    }
    return PROTO_OK;
}

static void app_gateway_survey_round_update_batch_phase(
    struct app_gateway_survey_round *round)
{
    if (survey_pair_round_runtime_batch_complete(&round->runtime)) {
        round->phase = APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE;
    }
}

int app_gateway_survey_round_finalize_lane(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome)
{
    const struct survey_pair_round_lane *lane;
    int ret;

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&round->runtime, lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (lane->state != SURVEY_PAIR_ROUND_LANE_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    ret = survey_pair_round_runtime_require_cleanup(&round->runtime,
                                                     lane_index,
                                                     cleanup_mask,
                                                     outcome);
    if (ret == PROTO_OK) {
        app_gateway_survey_round_update_batch_phase(round);
    }
    return ret;
}

int app_gateway_survey_round_note_cleanup_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t completed_mask)
{
    int ret;

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_pair_round_runtime_note_cleanup_complete(&round->runtime,
                                                           lane_index,
                                                           completed_mask);
    if (ret == PROTO_OK) {
        app_gateway_survey_round_update_batch_phase(round);
    }
    return ret;
}

int app_gateway_survey_round_begin_termination(
    struct app_gateway_survey_round *round,
    const struct survey_pair *active_pair,
    uint8_t active_cleanup_mask,
    size_t *active_lane_index)
{
    size_t matched_index = SIZE_MAX;

    if (round == NULL ||
        (active_cleanup_mask &
         (uint8_t)~SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) != 0u ||
        ((active_cleanup_mask == 0u) != (active_pair == NULL))) {
        return PROTO_ERR_ARG;
    }
    if (round->phase == APP_GATEWAY_SURVEY_ROUND_INACTIVE ||
        round->phase == APP_GATEWAY_SURVEY_ROUND_COMPLETE ||
        round->phase == APP_GATEWAY_SURVEY_ROUND_TERMINATING ||
        !round->runtime.active) {
        return PROTO_ERR_STALE;
    }
    if (active_pair != NULL) {
        for (size_t i = 0u; i < round->runtime.lane_count; i++) {
            if (!app_gateway_survey_round_pair_equal(
                    &round->runtime.lanes[i].pair, active_pair)) {
                continue;
            }
            if (matched_index != SIZE_MAX) {
                return PROTO_ERR_MALFORMED;
            }
            matched_index = i;
        }
        if (matched_index == SIZE_MAX) {
            return PROTO_ERR_NOT_FOUND;
        }
    }

    /*
     * Validation above is mutation-free. Once TERMINATING is visible, every
     * possible remote lease has exact lane custody and normal dispatch can no
     * longer expose another PREPARE, START, or GO.
     */
    round->runtime.pending_rerun_count = 0u;
    for (size_t i = 0u; i < round->runtime.lane_count; i++) {
        struct survey_pair_round_lane *lane = &round->runtime.lanes[i];
        uint8_t cleanup_mask =
            (uint8_t)(lane->prepared_mask | lane->started_mask);

        if (lane->state == SURVEY_PAIR_ROUND_LANE_CLEANUP) {
            cleanup_mask |= lane->cleanup_mask;
        }
        if (i == matched_index) {
            cleanup_mask |= active_cleanup_mask;
        }
        if (cleanup_mask != 0u) {
            lane->cleanup_mask = cleanup_mask;
            lane->cleanup_outcome = SURVEY_PAIR_ROUND_CLEANUP_FAIL;
            lane->state = SURVEY_PAIR_ROUND_LANE_CLEANUP;
        } else if (!app_gateway_survey_round_lane_attempt_terminal(lane)) {
            lane->cleanup_mask = 0u;
            lane->cleanup_outcome = SURVEY_PAIR_ROUND_CLEANUP_FAIL;
            lane->state = SURVEY_PAIR_ROUND_LANE_FAILED;
        }
    }
    round->dispatch_lane_index = 0u;
    round->dispatch_stage = SURVEY_GATEWAY_AUTO_IDLE;
    round->phase = APP_GATEWAY_SURVEY_ROUND_TERMINATING;
    if (active_lane_index != NULL) {
        *active_lane_index = matched_index;
    }
    return PROTO_OK;
}

int app_gateway_survey_round_next_termination_cleanup(
    const struct app_gateway_survey_round *round,
    size_t *lane_index,
    struct survey_pair *pair,
    uint8_t *cleanup_mask)
{
    if (round == NULL || lane_index == NULL ||
        pair == NULL || cleanup_mask == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_TERMINATING ||
        !round->runtime.active) {
        return PROTO_ERR_STALE;
    }
    for (size_t i = 0u; i < round->runtime.lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            &round->runtime.lanes[i];

        if (lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP ||
            lane->cleanup_mask == 0u) {
            continue;
        }
        *lane_index = i;
        *pair = lane->pair;
        *cleanup_mask = lane->cleanup_mask;
        return PROTO_OK;
    }
    return PROTO_ERR_NOT_FOUND;
}

int app_gateway_survey_round_note_termination_cleanup_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t completed_mask)
{
    struct survey_pair_round_lane *lane;

    if (round == NULL || completed_mask == 0u ||
        (completed_mask &
         (uint8_t)~SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) != 0u) {
        return PROTO_ERR_ARG;
    }
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_TERMINATING ||
        !round->runtime.active ||
        lane_index >= round->runtime.lane_count) {
        return PROTO_ERR_STALE;
    }
    lane = &round->runtime.lanes[lane_index];
    if (lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP ||
        (completed_mask & (uint8_t)~lane->cleanup_mask) != 0u) {
        return PROTO_ERR_STALE;
    }
    lane->cleanup_mask &= (uint8_t)~completed_mask;
    if (lane->cleanup_mask == 0u) {
        lane->state = SURVEY_PAIR_ROUND_LANE_FAILED;
    }
    return PROTO_OK;
}

bool app_gateway_survey_round_terminating(
    const struct app_gateway_survey_round *round)
{
    return round != NULL &&
           round->phase == APP_GATEWAY_SURVEY_ROUND_TERMINATING;
}

bool app_gateway_survey_round_batch_complete(
    const struct app_gateway_survey_round *round)
{
    return round != NULL &&
           round->phase == APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE &&
           survey_pair_round_runtime_batch_complete(&round->runtime);
}

int app_gateway_survey_round_advance_batch(
    struct app_gateway_survey_round *round,
    bool *complete)
{
    int ret;

    if (round == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!app_gateway_survey_round_batch_complete(round)) {
        return PROTO_ERR_BUSY;
    }
    if (survey_pair_round_runtime_complete(&round->runtime)) {
        round->phase = APP_GATEWAY_SURVEY_ROUND_COMPLETE;
        if (complete != NULL) {
            *complete = true;
        }
        return PROTO_OK;
    }

    ret = survey_pair_round_runtime_load_next_batch(&round->runtime);
    if (ret != PROTO_OK) {
        return ret;
    }
    memset(round->sample_identities,
           SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID,
           sizeof(round->sample_identities));
    app_gateway_survey_round_start_dispatch(round);
    if (complete != NULL) {
        *complete = false;
    }
    return PROTO_OK;
}

bool app_gateway_survey_round_complete(
    const struct app_gateway_survey_round *round)
{
    return round != NULL &&
           round->phase == APP_GATEWAY_SURVEY_ROUND_COMPLETE &&
           survey_pair_round_runtime_complete(&round->runtime);
}
