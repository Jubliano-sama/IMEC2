#ifndef SURVEY_PAIR_ROUND_RUNTIME_H
#define SURVEY_PAIR_ROUND_RUNTIME_H

#include "operation_policy.h"
#include "survey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES \
    OPERATION_POLICY_PAIR_MAX_PARALLEL_PAIRS
#define SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES 16u
#define SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK 0x01u
#define SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK 0x02u
#define SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK \
    (SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK | \
     SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK)

enum survey_pair_round_batch_kind {
    SURVEY_PAIR_ROUND_BATCH_NONE = 0,
    SURVEY_PAIR_ROUND_BATCH_PLANNED,
    SURVEY_PAIR_ROUND_BATCH_RERUN,
};

enum survey_pair_round_lane_state {
    SURVEY_PAIR_ROUND_LANE_EMPTY = 0,
    SURVEY_PAIR_ROUND_LANE_READY,
    SURVEY_PAIR_ROUND_LANE_ARMING,
    SURVEY_PAIR_ROUND_LANE_ARMED,
    SURVEY_PAIR_ROUND_LANE_OBSERVING,
    SURVEY_PAIR_ROUND_LANE_CLEANUP,
    SURVEY_PAIR_ROUND_LANE_SUCCEEDED,
    SURVEY_PAIR_ROUND_LANE_FAILED,
    SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED,
};

enum survey_pair_round_cleanup_outcome {
    SURVEY_PAIR_ROUND_CLEANUP_SUCCESS = 0,
    SURVEY_PAIR_ROUND_CLEANUP_RETRY,
    SURVEY_PAIR_ROUND_CLEANUP_FAIL,
};

struct survey_pair_round_lane {
    struct survey_pair pair;
    uint16_t usable_result_mask;
    uint16_t initiator_unusable_mask;
    uint16_t responder_unusable_mask;
    uint8_t plan_pair_index;
    uint8_t planner_round_index;
    uint8_t reruns_started;
    uint8_t prepared_mask;
    uint8_t started_mask;
    uint8_t cleanup_mask;
    enum survey_pair_round_lane_state state;
    enum survey_pair_round_cleanup_outcome cleanup_outcome;
};

struct survey_pair_round_rerun {
    uint8_t plan_pair_index;
    uint8_t reruns_started;
};

/*
 * The pair plan and metadata remain caller-owned and immutable from begin until
 * all work is complete. Only one capped batch is live at a time; pair controls
 * may therefore remain serialized while disjoint armed pairs range together.
 */
struct survey_pair_round_runtime {
    const struct survey_gateway_context *plan;
    const struct survey_pair_round_metadata *metadata;
    struct survey_pair_round_lane lanes[SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES];
    struct survey_pair_round_rerun
        pending_reruns[SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES];
    size_t metadata_count;
    uint16_t completed_success_count;
    uint16_t completed_failure_count;
    uint16_t batch_sequence;
    uint8_t lane_count;
    uint8_t pending_rerun_count;
    uint8_t round_count;
    uint8_t next_planner_round;
    uint8_t next_pair_in_round;
    uint8_t max_parallel_pairs;
    uint8_t max_reruns;
    enum survey_pair_round_batch_kind batch_kind;
    bool active;
};

int survey_pair_round_runtime_begin(
    struct survey_pair_round_runtime *runtime,
    const struct survey_gateway_context *plan,
    const struct survey_pair_round_metadata *metadata,
    size_t metadata_count,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns);

/* Returns PROTO_ERR_NOT_FOUND when no planned or rerun work remains. */
int survey_pair_round_runtime_load_next_batch(
    struct survey_pair_round_runtime *runtime);

size_t survey_pair_round_runtime_lane_count(
    const struct survey_pair_round_runtime *runtime);
const struct survey_pair_round_lane *survey_pair_round_runtime_lane(
    const struct survey_pair_round_runtime *runtime,
    size_t lane_index);

int survey_pair_round_runtime_mark_observing(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index);
int survey_pair_round_runtime_note_prepared(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t endpoint_mask);
int survey_pair_round_runtime_note_started(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t endpoint_mask);
bool survey_pair_round_lane_armed(
    const struct survey_pair_round_lane *lane);

/*
 * Demultiplexes one validated sample by the exact survey, ordered endpoints,
 * sample count, and reporter identity. A duplicate returns PROTO_OK with
 * accepted_new set false and cannot mutate another lane.
 */
int survey_pair_round_runtime_note_sample(
    struct survey_pair_round_runtime *runtime,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new);

bool survey_pair_round_lane_results_complete(
    const struct survey_pair_round_lane *lane);
bool survey_pair_round_lane_missing_samples_all_unusable(
    const struct survey_pair_round_lane *lane);

/*
 * Cleanup is lane-scoped. RETRY queues the same planned pair only after every
 * requested endpoint cleanup completes; exhausted retries become one final
 * failure. FAIL bypasses the rerun budget for permanent errors.
 */
int survey_pair_round_runtime_require_cleanup(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome);
int survey_pair_round_runtime_note_cleanup_complete(
    struct survey_pair_round_runtime *runtime,
    size_t lane_index,
    uint8_t completed_mask);

bool survey_pair_round_runtime_batch_complete(
    const struct survey_pair_round_runtime *runtime);
bool survey_pair_round_runtime_complete(
    const struct survey_pair_round_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
