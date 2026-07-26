#ifndef GATEWAY_SURVEY_MACHINE_H
#define GATEWAY_SURVEY_MACHINE_H

#include "node_comm.h"
#include "survey_pair_round_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gateway_survey_machine_phase {
    GATEWAY_SURVEY_MACHINE_IDLE = 0,
    GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING,
    GATEWAY_SURVEY_MACHINE_COLLECTING,
    GATEWAY_SURVEY_MACHINE_ROUND_READY,
    GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING,
    GATEWAY_SURVEY_MACHINE_ROUND_GO_REQUIRED,
    GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING,
    GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE,
    GATEWAY_SURVEY_MACHINE_TERMINAL,
};

enum gateway_survey_machine_terminal_reason {
    GATEWAY_SURVEY_MACHINE_TERMINAL_NONE = 0,
    GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT,
    GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_TIMEOUT,
    GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RETRY_EXHAUSTED,
    GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO,
    GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS,
    GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING,
    GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED,
    GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL,
    GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED,
};

enum gateway_survey_machine_drive_kind {
    GATEWAY_SURVEY_MACHINE_DRIVE_WAIT = 0,
    GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS,
    GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL,
};

struct gateway_survey_machine_drive {
    enum gateway_survey_machine_drive_kind kind;
    uint64_t wake_at_ms;
    enum gateway_survey_machine_terminal_reason reason;
};

struct gateway_survey_machine_control {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    enum command_id command_id;
    uint64_t target_id;
    size_t lane_index;
};

/*
 * Central lifecycle state for discovery collection and concurrent pair
 * rounds. Callers must serialize every transition. The planned survey context
 * remains caller-owned and immutable while the embedded round runtime is
 * active; the machine never copies that context.
 */
struct gateway_survey_machine {
    struct survey_pair_round_runtime round_runtime;
    struct survey_pair_round_metadata
        round_metadata[SURVEY_GATEWAY_MAX_PAIRS];
    uint64_t operation_deadline_ms;
    uint64_t emission_deadline_ms;
    uint64_t safety_deadline_ms;
    uint64_t collection_duration_ms;
    uint32_t generation;
    uint32_t survey_id;
    uint32_t discovery_delivery_token;
    size_t planned_round_count;
    size_t dispatch_lane_index;
    size_t failed_control_cleanup_lane_index;
    uint16_t expected_count;
    enum gateway_survey_machine_phase phase;
    enum gateway_survey_machine_terminal_reason terminal_reason;
    enum survey_gateway_auto_stage dispatch_stage;
    bool expected_count_present;
    bool discovery_rf_started;
    bool safety_deadline_armed;
    bool failed_control_cleanup_lane_valid;
};

void gateway_survey_machine_init(struct gateway_survey_machine *machine);

/*
 * Invalidates every outstanding generation/token callback and returns to an
 * inert IDLE state. The resulting generation is always nonzero.
 */
void gateway_survey_machine_reset(struct gateway_survey_machine *machine);

int gateway_survey_machine_begin(
    struct gateway_survey_machine *machine,
    uint32_t survey_id,
    uint64_t now_ms,
    uint64_t operation_budget_ms,
    uint64_t emission_delay_ms,
    uint64_t collection_duration_ms,
    uint16_t expected_count,
    bool expected_count_present);

int gateway_survey_machine_bind_discovery_delivery(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token);
int gateway_survey_machine_note_discovery_rf_started(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token);
int gateway_survey_machine_note_discovery_terminal(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token,
    uint64_t now_ms,
    const struct node_comm_terminal_event *event);

/*
 * Returns PROTO_OK while a report may mutate the caller-owned survey context,
 * PROTO_ERR_BUSY before the first proven discovery RF start, and
 * PROTO_ERR_STALE outside the current report-admission window.
 */
int gateway_survey_machine_report_admissible(
    const struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t survey_id,
    uint64_t now_ms);
int gateway_survey_machine_admit_report(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token,
    uint32_t survey_id,
    uint64_t now_ms,
    uint8_t attempts_started);

/*
 * Applies the shared survey_gateway_collection_decide() policy. A WAIT result
 * always names the strictly future minimum armed deadline.
 */
int gateway_survey_machine_collection_drive(
    struct gateway_survey_machine *machine,
    uint64_t now_ms,
    size_t report_count,
    struct gateway_survey_machine_drive *drive);

int gateway_survey_machine_abort(struct gateway_survey_machine *machine,
                                 uint32_t generation,
                                 uint32_t survey_id);

int gateway_survey_machine_validate(
    const struct gateway_survey_machine *machine);

uint32_t gateway_survey_machine_generation(
    const struct gateway_survey_machine *machine);
uint32_t gateway_survey_machine_survey_id(
    const struct gateway_survey_machine *machine);
enum gateway_survey_machine_phase gateway_survey_machine_phase(
    const struct gateway_survey_machine *machine);
bool gateway_survey_machine_active(
    const struct gateway_survey_machine *machine);
enum gateway_survey_machine_terminal_reason
gateway_survey_machine_terminal_reason(
    const struct gateway_survey_machine *machine);
uint32_t gateway_survey_machine_discovery_delivery_token(
    const struct gateway_survey_machine *machine);
bool gateway_survey_machine_discovery_rf_started(
    const struct gateway_survey_machine *machine);
uint64_t gateway_survey_machine_operation_deadline_ms(
    const struct gateway_survey_machine *machine);
uint32_t gateway_survey_machine_operation_remaining_ms(
    const struct gateway_survey_machine *machine,
    uint64_t now_ms);
uint64_t gateway_survey_machine_emission_deadline_ms(
    const struct gateway_survey_machine *machine);
bool gateway_survey_machine_safety_deadline_ms(
    const struct gateway_survey_machine *machine,
    uint64_t *deadline_ms);
uint64_t gateway_survey_machine_collection_duration_ms(
    const struct gateway_survey_machine *machine);
uint16_t gateway_survey_machine_expected_count(
    const struct gateway_survey_machine *machine);
bool gateway_survey_machine_expected_count_present(
    const struct gateway_survey_machine *machine);

int gateway_survey_machine_round_begin(
    struct gateway_survey_machine *machine,
    const struct survey_gateway_context *planned_context,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns);

bool gateway_survey_machine_round_active(
    const struct gateway_survey_machine *machine);
size_t gateway_survey_machine_round_lane_count(
    const struct gateway_survey_machine *machine);
const struct survey_pair_round_lane *gateway_survey_machine_round_lane(
    const struct gateway_survey_machine *machine,
    size_t lane_index);
uint16_t gateway_survey_machine_round_id(
    const struct gateway_survey_machine *machine);
size_t gateway_survey_machine_planned_round_count(
    const struct gateway_survey_machine *machine);
uint16_t gateway_survey_machine_round_success_count(
    const struct gateway_survey_machine *machine);
uint16_t gateway_survey_machine_round_failure_count(
    const struct gateway_survey_machine *machine);

int gateway_survey_machine_round_current_control(
    const struct gateway_survey_machine *machine,
    struct gateway_survey_machine_control *control);
int gateway_survey_machine_round_note_control_success(
    struct gateway_survey_machine *machine,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id);
int gateway_survey_machine_round_note_control_failure(
    struct gateway_survey_machine *machine,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome,
    size_t *lane_index);

bool gateway_survey_machine_failed_control_cleanup_pending(
    const struct gateway_survey_machine *machine);
int gateway_survey_machine_failed_control_cleanup_lane(
    const struct gateway_survey_machine *machine,
    size_t *lane_index);
int gateway_survey_machine_release_failed_control_cleanup(
    struct gateway_survey_machine *machine,
    size_t lane_index);

bool gateway_survey_machine_round_go_needed(
    const struct gateway_survey_machine *machine);
int gateway_survey_machine_round_mark_observing_after_go(
    struct gateway_survey_machine *machine);
bool gateway_survey_machine_round_go_submit_retryable(int error);
bool gateway_survey_machine_round_go_terminal_retryable(
    enum node_comm_terminal_reason reason,
    uint8_t attempts_started);

int gateway_survey_machine_round_note_sample(
    struct gateway_survey_machine *machine,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new);

int gateway_survey_machine_round_finalize_lane(
    struct gateway_survey_machine *machine,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome);
int gateway_survey_machine_round_note_cleanup_complete(
    struct gateway_survey_machine *machine,
    size_t lane_index,
    uint8_t completed_mask);

bool gateway_survey_machine_round_batch_complete(
    const struct gateway_survey_machine *machine);
/* Loads the next rerun/planned chunk only after the live batch is complete. */
int gateway_survey_machine_round_advance_batch(
    struct gateway_survey_machine *machine,
    bool *complete);
bool gateway_survey_machine_round_complete(
    const struct gateway_survey_machine *machine);

#ifdef __cplusplus
}
#endif

#endif
