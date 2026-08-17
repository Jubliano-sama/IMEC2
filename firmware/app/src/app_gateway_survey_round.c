#include "app_gateway_survey_round.h"

#include "protocol.h"
#include "survey_gateway_transaction.h"

#include <errno.h>
#include <string.h>

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS == 150u,
               "gateway round wrapper must retain the complete pair map");
_Static_assert(SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES == 25u,
               "gateway round wrapper must retain the runtime lane cap");

void app_gateway_survey_incarnation_tracker_init(
    struct app_gateway_survey_incarnation_tracker *tracker)
{
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

int app_gateway_survey_incarnation_tracker_classify(
    const struct app_gateway_survey_incarnation_tracker *tracker,
    uint64_t anchor_id,
    uint32_t boot_incarnation,
    uint32_t *previous_incarnation)
{
    uint32_t delta;

    if (tracker == NULL || previous_incarnation == NULL || anchor_id == 0u ||
        boot_incarnation == 0u ||
        tracker->count > SURVEY_GATEWAY_MAX_REPORTS) {
        return -EINVAL;
    }
    *previous_incarnation = 0u;
    for (uint8_t i = 0u; i < tracker->count; i++) {
        uint32_t retained;

        if (tracker->anchor_ids[i] != anchor_id) {
            continue;
        }
        retained = tracker->boot_incarnations[i];
        if (retained == 0u) {
            return -EIO;
        }
        *previous_incarnation = retained;
        if (boot_incarnation == retained) {
            return 0;
        }
        delta = boot_incarnation - retained;
        if (delta >= UINT32_C(0x80000000)) {
            return -ESTALE;
        }
        return 1;
    }

    if (tracker->count == SURVEY_GATEWAY_MAX_REPORTS) {
        return -ENOSPC;
    }
    return 0;
}

int app_gateway_survey_incarnation_tracker_note(
    struct app_gateway_survey_incarnation_tracker *tracker,
    uint64_t anchor_id,
    uint32_t boot_incarnation,
    uint32_t *previous_incarnation)
{
    int ret = app_gateway_survey_incarnation_tracker_classify(
        tracker, anchor_id, boot_incarnation, previous_incarnation);

    if (ret < 0) {
        return ret;
    }
    if (*previous_incarnation == 0u) {
        tracker->anchor_ids[tracker->count] = anchor_id;
        tracker->boot_incarnations[tracker->count] = boot_incarnation;
        tracker->count++;
        return 0;
    }
    if (ret == 1) {
        for (uint8_t i = 0u; i < tracker->count; i++) {
            if (tracker->anchor_ids[i] == anchor_id) {
                tracker->boot_incarnations[i] = boot_incarnation;
                return 1;
            }
        }
        return -EIO;
    }
    return 0;
}

static int app_gateway_survey_round_stage_details(
    const struct survey_pair_round_lane *lane,
    enum app_gateway_survey_control_stage stage,
    enum command_id *command_id,
    uint64_t *target_id,
    uint8_t *endpoint_mask)
{
    if (lane == NULL || command_id == NULL || target_id == NULL ||
        endpoint_mask == NULL) {
        return PROTO_ERR_ARG;
    }

    switch (stage) {
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR:
        *command_id = CMD_SURVEY_PREPARE_PAIR;
        *target_id = lane->pair.initiator_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK;
        return PROTO_OK;
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER:
        *command_id = CMD_SURVEY_PREPARE_PAIR;
        *target_id = lane->pair.responder_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK;
        return PROTO_OK;
    case APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER:
        *command_id = CMD_SURVEY_START_PAIR;
        *target_id = lane->pair.responder_id;
        *endpoint_mask = SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK;
        return PROTO_OK;
    case APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR:
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
    round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR;
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

static uint32_t app_gateway_survey_round_control_session_id(
    const struct survey_pair *pair)
{
    if (pair == NULL) {
        return 0u;
    }
    return pair->operation_generation == 0u ? pair->survey_id :
           survey_operation_session_id(pair->operation_generation);
}

static bool app_gateway_survey_round_control_equal(
    const struct app_gateway_survey_round_control *left,
    const struct app_gateway_survey_round_control *right)
{
    return left != NULL && right != NULL &&
           left->command_id == right->command_id &&
           left->target_id == right->target_id &&
           left->lane_index == right->lane_index &&
           left->stage == right->stage &&
           left->pair.operation_generation == right->pair.operation_generation &&
           left->pair.survey_id == right->pair.survey_id &&
           left->pair.initiator_id == right->pair.initiator_id &&
           left->pair.responder_id == right->pair.responder_id &&
           left->pair.sample_count == right->pair.sample_count;
}

int app_gateway_survey_round_capture_control_result(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    const struct node_transaction *transaction,
    uint32_t started_at_ms,
    enum command_status status)
{
    struct app_gateway_survey_round_control control;
    struct app_gateway_survey_round_control_confirmation *confirmation;
    uint32_t operation_session_id;
    int ret;

    if (round == NULL || transaction == NULL ||
        status > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_ARG;
    }
    ret = app_gateway_survey_round_current_control(round, &control);
    if (ret != PROTO_OK) {
        return ret;
    }
    operation_session_id = app_gateway_survey_round_control_session_id(
        &control.pair);
    if (command_id != control.command_id || target_id != control.target_id ||
        transaction->state != NODE_TRANSACTION_SUCCEEDED ||
        !transaction->request_delivery_terminal ||
        transaction->spec.key.requester_id == 0u ||
        transaction->spec.key.responder_id != control.target_id ||
        transaction->spec.key.session_id != operation_session_id ||
        transaction->spec.key.transaction_id == 0u ||
        transaction->spec.key.operation_id != (uint16_t)command_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    confirmation = &round->control_confirmation;
    if (confirmation->valid) {
        return app_gateway_survey_round_control_equal(
                   &confirmation->control, &control) &&
               node_transaction_key_equal(&confirmation->result_key,
                                          &transaction->spec.key) &&
               confirmation->status == status &&
               confirmation->started_at_ms == started_at_ms &&
               confirmation->deadline_ms ==
                   transaction->spec.absolute_deadline_ms &&
               memcmp(confirmation->semantic_digest,
                      transaction->accepted_result_digest,
                      sizeof(confirmation->semantic_digest)) == 0 ?
                   PROTO_OK : PROTO_ERR_BUSY;
    }

    memset(confirmation, 0, sizeof(*confirmation));
    confirmation->control = control;
    confirmation->result_key = transaction->spec.key;
    memcpy(confirmation->semantic_digest,
           transaction->accepted_result_digest,
           sizeof(confirmation->semantic_digest));
    confirmation->status = status;
    confirmation->started_at_ms = started_at_ms;
    confirmation->deadline_ms =
        transaction->spec.absolute_deadline_ms;
    confirmation->valid = true;
    return PROTO_OK;
}

int app_gateway_survey_round_note_control_ack_confirm(
    struct app_gateway_survey_round *round,
    const struct app_gateway_survey_round_ack_confirm *confirm)
{
    struct app_gateway_survey_round_control_confirmation *confirmation;

    if (round == NULL || confirm == NULL) {
        return PROTO_ERR_ARG;
    }
    confirmation = &round->control_confirmation;
    if (!confirmation->valid) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (confirm->msg_type != MSG_COMMAND_RESULT ||
        confirm->session_id != confirmation->result_key.session_id ||
        confirm->seq != confirmation->result_key.transaction_id ||
        confirm->source_id != confirmation->result_key.responder_id ||
        confirm->destination_id != confirmation->result_key.requester_id) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (memcmp(confirm->semantic_digest,
               confirmation->semantic_digest,
               sizeof(confirmation->semantic_digest)) != 0) {
        return PROTO_ERR_MALFORMED;
    }
    if (!confirmation->confirmed ||
        confirm->first_received_at_ms < confirmation->confirmed_at_ms) {
        confirmation->confirmed_at_ms = confirm->first_received_at_ms;
    }
    confirmation->confirmed = true;
    return PROTO_OK;
}

bool app_gateway_survey_round_control_confirmation_pending(
    const struct app_gateway_survey_round *round)
{
    return round != NULL && round->control_confirmation.valid &&
           !round->control_confirmation.confirmed;
}

uint64_t app_gateway_survey_round_control_confirmation_deadline(
    const struct app_gateway_survey_round *round)
{
    return round == NULL || !round->control_confirmation.valid ? 0u :
        round->control_confirmation.deadline_ms;
}

bool app_gateway_survey_round_control_confirmation_expired(
    const struct app_gateway_survey_round *round,
    uint64_t now_ms)
{
    const uint64_t deadline_ms =
        app_gateway_survey_round_control_confirmation_deadline(round);

    return deadline_ms != 0u && now_ms >= deadline_ms;
}

bool app_gateway_survey_round_control_confirmation_received_in_interval(
    const struct app_gateway_survey_round *round,
    uint32_t started_at_ms,
    uint32_t deadline_ms)
{
    return round != NULL && round->control_confirmation.valid &&
           round->control_confirmation.confirmed &&
           survey_gateway_receive_in_interval(
               round->control_confirmation.confirmed_at_ms,
               started_at_ms,
               deadline_ms);
}

int app_gateway_survey_round_control_confirmation_ready(
    const struct app_gateway_survey_round *round,
    struct app_gateway_survey_round_control *control,
    enum command_status *status)
{
    struct app_gateway_survey_round_control current;
    const struct app_gateway_survey_round_control_confirmation *confirmation;
    int ret;

    if (round == NULL || control == NULL || status == NULL) {
        return PROTO_ERR_ARG;
    }
    confirmation = &round->control_confirmation;
    if (!confirmation->valid || !confirmation->confirmed) {
        return PROTO_ERR_BUSY;
    }
    if (!survey_gateway_receive_in_interval(
            confirmation->confirmed_at_ms,
            (uint32_t)confirmation->started_at_ms,
            (uint32_t)confirmation->deadline_ms)) {
        return -ETIMEDOUT;
    }
    ret = app_gateway_survey_round_current_control(round, &current);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!app_gateway_survey_round_control_equal(&current,
                                                &confirmation->control)) {
        return PROTO_ERR_STALE;
    }
    *control = confirmation->control;
    *status = confirmation->status;
    return PROTO_OK;
}

void app_gateway_survey_round_clear_control_confirmation(
    struct app_gateway_survey_round *round)
{
    if (round != NULL) {
        memset(&round->control_confirmation, 0,
               sizeof(round->control_confirmation));
    }
}

static bool app_gateway_survey_round_lane_attempt_terminal(
    const struct survey_pair_round_lane *lane)
{
    return lane != NULL &&
           (lane->state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_FAILED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
}

static bool app_gateway_survey_round_has_observing_lane(
    const struct app_gateway_survey_round *round)
{
    const size_t lane_count = app_gateway_survey_round_lane_count(round);
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&round->runtime, i);

        if (lane != NULL &&
            lane->state == SURVEY_PAIR_ROUND_LANE_OBSERVING) {
            return true;
        }
    }
    return false;
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
            round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR;
            round->phase = APP_GATEWAY_SURVEY_ROUND_DISPATCHING;
            return;
        }
        round->dispatch_lane_index++;
    }
    round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR;
    if (app_gateway_survey_round_has_observing_lane(round)) {
        round->phase = APP_GATEWAY_SURVEY_ROUND_OBSERVING;
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
    struct app_gateway_survey_round_control current_control;
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
    if (round->control_confirmation.valid) {
        ret = app_gateway_survey_round_current_control(round,
                                                        &current_control);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (!round->control_confirmation.confirmed ||
            !app_gateway_survey_round_control_equal(
                &current_control,
                &round->control_confirmation.control)) {
            return PROTO_ERR_BUSY;
        }
    }

    switch (round->dispatch_stage) {
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR:
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER:
        ret = survey_pair_round_runtime_note_prepared(&round->runtime,
                                                       round->dispatch_lane_index,
                                                       endpoint_mask);
        break;
    case APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER:
    case APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR:
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
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR:
        round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER;
        break;
    case APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER:
        round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER;
        break;
    case APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER:
        round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR;
        break;
    case APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR:
        ret = survey_pair_round_runtime_mark_observing(
            &round->runtime, round->dispatch_lane_index);
        if (ret != PROTO_OK) {
            return ret;
        }
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
    if ((admissible_lane_state != SURVEY_PAIR_ROUND_LANE_ARMING &&
         admissible_lane_state != SURVEY_PAIR_ROUND_LANE_ARMED &&
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
        reporter_id != sample->pair.responder_id) {
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

    reporter_index = 1u;
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

int app_gateway_survey_round_preflight_admissible_sample(
    const struct app_gateway_survey_round *round,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *duplicate)
{
    int ret;

    ret = app_gateway_survey_round_preflight_sample(
        round,
        SURVEY_PAIR_ROUND_LANE_OBSERVING,
        reporter_id,
        sample,
        lane_index,
        duplicate);
    if (ret == PROTO_ERR_STALE) {
        ret = app_gateway_survey_round_preflight_sample(
            round,
            SURVEY_PAIR_ROUND_LANE_ARMED,
            reporter_id,
            sample,
            lane_index,
            duplicate);
    }
    /*
     * START_INITIATOR ACK-confirm is what promotes ARMING to ARMED and then
     * OBSERVING. A hop-1 responder can finish DS-TWR before that confirm
     * arrives, so semantic admission must keep the same fallback the commit
     * path uses.
     */
    if (ret == PROTO_ERR_STALE) {
        ret = app_gateway_survey_round_preflight_sample(
            round,
            SURVEY_PAIR_ROUND_LANE_ARMING,
            reporter_id,
            sample,
            lane_index,
            duplicate);
    }
    return ret;
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
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_DISPATCHING &&
        round->phase != APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    ret = app_gateway_survey_round_preflight_admissible_sample(
        round,
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
    reporter_index = 1u;
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
    if (round->phase != APP_GATEWAY_SURVEY_ROUND_DISPATCHING &&
        round->phase != APP_GATEWAY_SURVEY_ROUND_OBSERVING) {
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
     * longer expose another PREPARE or START.
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
    round->dispatch_stage = APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR;
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

bool app_gateway_survey_round_batch_has_result_evidence(
    const struct app_gateway_survey_round *round)
{
    if (round == NULL || !app_gateway_survey_round_batch_complete(round)) {
        return false;
    }
    for (size_t i = 0u; i < round->runtime.lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            &round->runtime.lanes[i];

        if (lane->usable_result_mask != 0u ||
            lane->responder_usable_mask != 0u ||
            lane->initiator_unusable_mask != 0u ||
            lane->responder_unusable_mask != 0u) {
            return true;
        }
    }
    return false;
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
    if (round->outcome_event_pending) {
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

int app_gateway_survey_round_outcome_event_seed(
    const struct app_gateway_survey_round *round,
    size_t lane_index,
    uint32_t *event_seq)
{
    if (round == NULL || event_seq == NULL ||
        lane_index >= app_gateway_survey_round_lane_count(round)) {
        return PROTO_ERR_ARG;
    }
    if (round->outcome_event_pending &&
        round->outcome_event_lane_index != lane_index) {
        return PROTO_ERR_BUSY;
    }
    *event_seq = round->outcome_event_pending ?
        round->outcome_event_seq : 0u;
    return PROTO_OK;
}

int app_gateway_survey_round_outcome_event_retain(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint32_t event_seq)
{
    if (round == NULL || event_seq == 0u ||
        lane_index >= app_gateway_survey_round_lane_count(round)) {
        return PROTO_ERR_ARG;
    }
    if (round->outcome_event_pending &&
        (round->outcome_event_lane_index != lane_index ||
         round->outcome_event_seq != event_seq)) {
        return PROTO_ERR_BUSY;
    }
    round->outcome_event_lane_index = lane_index;
    round->outcome_event_seq = event_seq;
    round->outcome_event_pending = true;
    return PROTO_OK;
}

int app_gateway_survey_round_outcome_event_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index)
{
    if (round == NULL ||
        lane_index >= app_gateway_survey_round_lane_count(round)) {
        return PROTO_ERR_ARG;
    }
    if (round->outcome_event_pending &&
        round->outcome_event_lane_index != lane_index) {
        return PROTO_ERR_BUSY;
    }
    round->outcome_event_lane_index = 0u;
    round->outcome_event_seq = 0u;
    round->outcome_event_pending = false;
    return PROTO_OK;
}
