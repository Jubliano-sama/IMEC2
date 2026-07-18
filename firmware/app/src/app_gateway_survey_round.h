#ifndef APP_GATEWAY_SURVEY_ROUND_H
#define APP_GATEWAY_SURVEY_ROUND_H

#include "survey_pair_round_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_gateway_survey_round_phase {
    APP_GATEWAY_SURVEY_ROUND_INACTIVE = 0,
    APP_GATEWAY_SURVEY_ROUND_DISPATCHING,
    APP_GATEWAY_SURVEY_ROUND_GO_REQUIRED,
    APP_GATEWAY_SURVEY_ROUND_OBSERVING,
    APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE,
    APP_GATEWAY_SURVEY_ROUND_COMPLETE,
};

struct app_gateway_survey_round_control {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    enum command_id command_id;
    uint64_t target_id;
    size_t lane_index;
};

/*
 * Owns the compact complete-plan metadata and the one serialized control
 * dispatcher. The caller retains the planned survey context until complete.
 * Transport handles, radio timestamps, and deadlines remain caller-owned.
 */
struct app_gateway_survey_round {
    struct survey_pair_round_runtime runtime;
    struct survey_pair_round_metadata metadata[SURVEY_GATEWAY_MAX_PAIRS];
    size_t planned_round_count;
    size_t dispatch_lane_index;
    enum survey_gateway_auto_stage dispatch_stage;
    enum app_gateway_survey_round_phase phase;
};

int app_gateway_survey_round_begin(
    struct app_gateway_survey_round *round,
    const struct survey_gateway_context *planned_context,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns);

size_t app_gateway_survey_round_lane_count(
    const struct app_gateway_survey_round *round);
const struct survey_pair_round_lane *app_gateway_survey_round_lane(
    const struct app_gateway_survey_round *round,
    size_t lane_index);

int app_gateway_survey_round_current_control(
    const struct app_gateway_survey_round *round,
    struct app_gateway_survey_round_control *control);
int app_gateway_survey_round_note_control_success(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id);
int app_gateway_survey_round_note_control_failure(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome,
    size_t *lane_index);

bool app_gateway_survey_round_go_needed(
    const struct app_gateway_survey_round *round);
int app_gateway_survey_round_mark_observing_after_go(
    struct app_gateway_survey_round *round);

int app_gateway_survey_round_note_sample(
    struct app_gateway_survey_round *round,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new);

int app_gateway_survey_round_finalize_lane(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome);
int app_gateway_survey_round_note_cleanup_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t completed_mask);

bool app_gateway_survey_round_batch_complete(
    const struct app_gateway_survey_round *round);
/* Loads the next rerun/planned chunk only after the live batch is complete. */
int app_gateway_survey_round_advance_batch(
    struct app_gateway_survey_round *round,
    bool *complete);
bool app_gateway_survey_round_complete(
    const struct app_gateway_survey_round *round);

#ifdef __cplusplus
}
#endif

#endif
