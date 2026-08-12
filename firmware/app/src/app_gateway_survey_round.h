#ifndef APP_GATEWAY_SURVEY_ROUND_H
#define APP_GATEWAY_SURVEY_ROUND_H

#include "node_comm.h"
#include "node_transaction.h"
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
    APP_GATEWAY_SURVEY_ROUND_OBSERVING,
    APP_GATEWAY_SURVEY_ROUND_BATCH_COMPLETE,
    APP_GATEWAY_SURVEY_ROUND_TERMINATING,
    APP_GATEWAY_SURVEY_ROUND_COMPLETE,
};

enum app_gateway_survey_control_stage {
    APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR = 0,
    APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER,
    APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER,
    APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR,
};

/*
 * Gateway-RAM view of the newest boot incarnation observed for each anchor.
 * Packet/result custody remains volatile; this table only lets a newer startup
 * heartbeat invalidate an in-flight survey promptly.  It deliberately lives
 * across survey operations and is reset only with the gateway boot.
 */
struct app_gateway_survey_incarnation_tracker {
    uint64_t anchor_ids[SURVEY_GATEWAY_MAX_REPORTS];
    uint32_t boot_incarnations[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t count;
};

_Static_assert(sizeof(struct app_gateway_survey_incarnation_tracker) == 608u,
               "gateway survey incarnation tracker exceeded its RAM gate");

void app_gateway_survey_incarnation_tracker_init(
    struct app_gateway_survey_incarnation_tracker *tracker);
int app_gateway_survey_incarnation_tracker_classify(
    const struct app_gateway_survey_incarnation_tracker *tracker,
    uint64_t anchor_id,
    uint32_t boot_incarnation,
    uint32_t *previous_incarnation);
/*
 * Returns one only when boot_incarnation is RFC1982-newer than the retained
 * value, zero for a first observation or exact replay, and a negative errno
 * for invalid, stale/ambiguous, or full-table input.  previous_incarnation is
 * zero for a first observation and otherwise receives the retained value.
 */
int app_gateway_survey_incarnation_tracker_note(
    struct app_gateway_survey_incarnation_tracker *tracker,
    uint64_t anchor_id,
    uint32_t boot_incarnation,
    uint32_t *previous_incarnation);

struct app_gateway_survey_round_control {
    struct survey_pair pair;
    enum app_gateway_survey_control_stage stage;
    enum command_id command_id;
    uint64_t target_id;
    size_t lane_index;
};

/*
 * Protocol-level proof view supplied by the gateway adapter after transport
 * has validated an ACK_CONFIRM.  It deliberately carries no mesh ownership
 * type: the round only needs the immutable acknowledged packet identity.
 */
struct app_gateway_survey_round_ack_confirm {
    uint64_t source_id;
    uint64_t destination_id;
    uint64_t first_received_at_ms;
    uint32_t session_id;
    uint16_t seq;
    uint8_t msg_type;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
};

/*
 * A control result is only a gateway-local semantic acceptance.  The source
 * and every relay may still own the result until the exact ACK_CONFIRM
 * returns.  Keep that immutable identity with the round instead of replacing
 * it with a time-based quiet interval.
 */
struct app_gateway_survey_round_control_confirmation {
    struct app_gateway_survey_round_control control;
    struct node_transaction_key result_key;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum command_status status;
    uint64_t started_at_ms;
    uint64_t deadline_ms;
    uint64_t confirmed_at_ms;
    bool valid;
    bool confirmed;
};

/*
 * Owns the compact complete-plan metadata and the one serialized control
 * dispatcher. The caller retains the planned survey context until complete.
 * Transport handles, radio timestamps, and deadlines remain caller-owned.
 */
struct app_gateway_survey_round {
    struct survey_pair_round_runtime runtime;
    struct survey_pair_round_metadata metadata[SURVEY_GATEWAY_MAX_PAIRS];
    struct survey_sample_observation_identity sample_identities
        [SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES]
        [2u]
        [SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT];
    size_t planned_round_count;
    size_t dispatch_lane_index;
    size_t outcome_event_lane_index;
    enum app_gateway_survey_control_stage dispatch_stage;
    enum app_gateway_survey_round_phase phase;
    uint32_t outcome_event_seq;
    bool outcome_event_pending;
    struct app_gateway_survey_round_control_confirmation
        control_confirmation;
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

/*
 * Capture the exact semantic packet identity after the gateway has accepted
 * a PREPARE/START result.  The caller may retire its request-delivery handle,
 * but must not advance this control until note_control_ack_confirm() proves
 * that the same result has reached terminal ACK-confirm custody.
 */
int app_gateway_survey_round_capture_control_result(
    struct app_gateway_survey_round *round,
    enum command_id command_id,
    uint64_t target_id,
    const struct node_transaction *transaction,
    uint32_t started_at_ms,
    enum command_status status);
int app_gateway_survey_round_note_control_ack_confirm(
    struct app_gateway_survey_round *round,
    const struct app_gateway_survey_round_ack_confirm *confirm);
bool app_gateway_survey_round_control_confirmation_pending(
    const struct app_gateway_survey_round *round);
uint64_t app_gateway_survey_round_control_confirmation_deadline(
    const struct app_gateway_survey_round *round);
bool app_gateway_survey_round_control_confirmation_expired(
    const struct app_gateway_survey_round *round,
    uint64_t now_ms);
bool app_gateway_survey_round_control_confirmation_received_in_interval(
    const struct app_gateway_survey_round *round,
    uint32_t started_at_ms,
    uint32_t deadline_ms);
int app_gateway_survey_round_control_confirmation_ready(
    const struct app_gateway_survey_round *round,
    struct app_gateway_survey_round_control *control,
    enum command_status *status);
void app_gateway_survey_round_clear_control_confirmation(
    struct app_gateway_survey_round *round);

/*
 * Purely classify one sample against the current batch. The caller selects
 * OBSERVING after this lane's ordered START responder/initiator controls.
 */
int app_gateway_survey_round_preflight_sample(
    const struct app_gateway_survey_round *round,
    enum survey_pair_round_lane_state admissible_lane_state,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *duplicate);
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

/*
 * A terminal survey outcome must retain cleanup custody for every endpoint
 * whose PREPARE was confirmed, plus the exact in-flight transaction mask
 * whose PREPARE may have reached the radio. The current batch and its round
 * commitment remain live until every returned cleanup mask is retired.
 */
int app_gateway_survey_round_begin_termination(
    struct app_gateway_survey_round *round,
    const struct survey_pair *active_pair,
    uint8_t active_cleanup_mask,
    size_t *active_lane_index);
int app_gateway_survey_round_next_termination_cleanup(
    const struct app_gateway_survey_round *round,
    size_t *lane_index,
    struct survey_pair *pair,
    uint8_t *cleanup_mask);
int app_gateway_survey_round_note_termination_cleanup_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint8_t completed_mask);
bool app_gateway_survey_round_terminating(
    const struct app_gateway_survey_round *round);

bool app_gateway_survey_round_batch_complete(
    const struct app_gateway_survey_round *round);
/* True when at least one lane retained a range-result sample or unusable
 * sample marker; a control timeout alone is not pair-result evidence. */
bool app_gateway_survey_round_batch_has_result_evidence(
    const struct app_gateway_survey_round *round);
/* Loads the next rerun/planned chunk only after the live batch is complete. */
int app_gateway_survey_round_advance_batch(
    struct app_gateway_survey_round *round,
    bool *complete);
bool app_gateway_survey_round_complete(
    const struct app_gateway_survey_round *round);

/*
 * A failed host enqueue may already have consumed an event identity. Retain
 * that identity against the exact lane until the same semantic outcome is
 * accepted, so a two-millisecond retry cannot manufacture another event.
 */
int app_gateway_survey_round_outcome_event_seed(
    const struct app_gateway_survey_round *round,
    size_t lane_index,
    uint32_t *event_seq);
int app_gateway_survey_round_outcome_event_retain(
    struct app_gateway_survey_round *round,
    size_t lane_index,
    uint32_t event_seq);
int app_gateway_survey_round_outcome_event_complete(
    struct app_gateway_survey_round *round,
    size_t lane_index);

#ifdef __cplusplus
}
#endif

#endif
