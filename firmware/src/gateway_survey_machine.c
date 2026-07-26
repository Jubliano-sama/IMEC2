#include "gateway_survey_machine.h"

#include <errno.h>
#include <string.h>

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS == 150u,
               "gateway survey machine must retain the complete pair map");
_Static_assert(SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES == 25u,
               "gateway survey machine must retain the runtime lane cap");

static uint32_t gateway_survey_machine_next_generation(uint32_t generation)
{
    generation++;
    return generation == 0u ? 1u : generation;
}

static bool gateway_survey_machine_add_u64(uint64_t base,
                                           uint64_t delta,
                                           uint64_t *sum)
{
    if (sum == NULL || delta > UINT64_MAX - base) {
        return false;
    }
    *sum = base + delta;
    return true;
}

static bool gateway_survey_machine_terminal_reason_valid(
    enum gateway_survey_machine_terminal_reason reason)
{
    return reason >= GATEWAY_SURVEY_MACHINE_TERMINAL_NONE &&
           reason <= GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED;
}

static void gateway_survey_machine_set_terminal(
    struct gateway_survey_machine *machine,
    enum gateway_survey_machine_terminal_reason reason)
{
    machine->phase = GATEWAY_SURVEY_MACHINE_TERMINAL;
    machine->terminal_reason = reason;
}

void gateway_survey_machine_init(struct gateway_survey_machine *machine)
{
    if (machine == NULL) {
        return;
    }
    memset(machine, 0, sizeof(*machine));
    machine->generation = 1u;
    machine->phase = GATEWAY_SURVEY_MACHINE_IDLE;
}

void gateway_survey_machine_reset(struct gateway_survey_machine *machine)
{
    uint32_t generation;

    if (machine == NULL) {
        return;
    }
    generation = gateway_survey_machine_next_generation(machine->generation);
    memset(machine, 0, sizeof(*machine));
    machine->generation = generation;
    machine->phase = GATEWAY_SURVEY_MACHINE_IDLE;
}

int gateway_survey_machine_begin(
    struct gateway_survey_machine *machine,
    uint32_t survey_id,
    uint64_t now_ms,
    uint64_t operation_budget_ms,
    uint64_t emission_delay_ms,
    uint64_t collection_duration_ms,
    uint16_t expected_count,
    bool expected_count_present)
{
    uint64_t operation_deadline_ms;
    uint64_t emission_deadline_ms;
    uint32_t generation;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_IDLE) {
        return PROTO_ERR_BUSY;
    }
    if (survey_id == 0u || operation_budget_ms == 0u ||
        collection_duration_ms == 0u ||
        (expected_count_present &&
         (expected_count == 0u ||
          expected_count > SURVEY_GATEWAY_MAX_REPORTS))) {
        return PROTO_ERR_MALFORMED;
    }
    if (!gateway_survey_machine_add_u64(now_ms,
                                         operation_budget_ms,
                                         &operation_deadline_ms) ||
        !gateway_survey_machine_add_u64(now_ms,
                                         emission_delay_ms,
                                         &emission_deadline_ms)) {
        return PROTO_ERR_NO_SPACE;
    }

    generation = gateway_survey_machine_next_generation(machine->generation);
    memset(machine, 0, sizeof(*machine));
    machine->generation = generation;
    machine->survey_id = survey_id;
    machine->operation_deadline_ms = operation_deadline_ms;
    machine->emission_deadline_ms = emission_deadline_ms;
    machine->collection_duration_ms = collection_duration_ms;
    machine->expected_count = expected_count_present ? expected_count : 0u;
    machine->expected_count_present = expected_count_present;
    machine->phase = GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING;
    machine->terminal_reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NONE;
    return PROTO_OK;
}

static int gateway_survey_machine_match_discovery(
    const struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token)
{
    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (generation == 0u || generation != machine->generation ||
        machine->phase != GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING) {
        return PROTO_ERR_STALE;
    }
    if (delivery_token == 0u ||
        delivery_token != machine->discovery_delivery_token) {
        return PROTO_ERR_NOT_FOUND;
    }
    return PROTO_OK;
}

int gateway_survey_machine_bind_discovery_delivery(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token)
{
    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (generation == 0u || generation != machine->generation ||
        machine->phase != GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING) {
        return PROTO_ERR_STALE;
    }
    if (delivery_token == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->discovery_delivery_token != 0u) {
        return machine->discovery_delivery_token == delivery_token ?
            PROTO_OK : PROTO_ERR_BUSY;
    }
    machine->discovery_delivery_token = delivery_token;
    return PROTO_OK;
}

int gateway_survey_machine_note_discovery_rf_started(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token)
{
    int ret = gateway_survey_machine_match_discovery(machine,
                                                      generation,
                                                      delivery_token);

    if (ret != PROTO_OK) {
        return ret;
    }
    machine->discovery_rf_started = true;
    return PROTO_OK;
}

static enum gateway_survey_machine_terminal_reason
gateway_survey_machine_discovery_failure_reason(
    enum node_comm_terminal_reason reason)
{
    switch (reason) {
    case NODE_COMM_TERMINAL_DEADLINE_EXPIRED:
        return GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_TIMEOUT;
    case NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED:
        return GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RETRY_EXHAUSTED;
    case NODE_COMM_TERMINAL_PERMANENT_FAILURE:
    case NODE_COMM_TERMINAL_CANCELLED:
        return GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO;
    case NODE_COMM_TERMINAL_DELIVERED:
    default:
        return GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL;
    }
}

int gateway_survey_machine_note_discovery_terminal(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token,
    uint64_t now_ms,
    const struct node_comm_terminal_event *event)
{
    uint64_t safety_deadline_ms;
    int ret = gateway_survey_machine_match_discovery(machine,
                                                      generation,
                                                      delivery_token);

    /*
     * Identity checks intentionally precede event validation. A stale callback
     * must be byte-for-byte side-effect-free even if its retained payload is
     * no longer meaningful to the current operation.
     */
    if (ret != PROTO_OK) {
        return ret;
    }
    if (event == NULL) {
        return PROTO_ERR_ARG;
    }
    if (event->handle != delivery_token) {
        return PROTO_ERR_NOT_FOUND;
    }

    /*
     * A matching terminal callback consumes the node-communication handle,
     * regardless of the terminal reason. Retire it before any state outcome so
     * later finish/abort cleanup cannot abandon an already-consumed handle.
     */
    machine->discovery_delivery_token = 0u;
    if (event->reason < NODE_COMM_TERMINAL_DELIVERED ||
        event->reason > NODE_COMM_TERMINAL_CANCELLED) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return PROTO_ERR_MALFORMED;
    }

    if (event->attempts_started != 0u) {
        machine->discovery_rf_started = true;
    }
    if (now_ms >= machine->operation_deadline_ms) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT);
        return PROTO_OK;
    }
    if (event->reason != NODE_COMM_TERMINAL_DELIVERED) {
        gateway_survey_machine_set_terminal(
            machine,
            gateway_survey_machine_discovery_failure_reason(event->reason));
        return PROTO_OK;
    }
    if (event->attempts_started == 0u) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO);
        return PROTO_OK;
    }
    if (!gateway_survey_machine_add_u64(now_ms,
                                         machine->collection_duration_ms,
                                         &safety_deadline_ms)) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return PROTO_ERR_NO_SPACE;
    }

    machine->safety_deadline_ms = safety_deadline_ms;
    machine->safety_deadline_armed = true;
    machine->phase = GATEWAY_SURVEY_MACHINE_COLLECTING;
    return PROTO_OK;
}

int gateway_survey_machine_report_admissible(
    const struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t survey_id,
    uint64_t now_ms)
{
    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (generation == 0u || generation != machine->generation ||
        survey_id == 0u || survey_id != machine->survey_id) {
        return PROTO_ERR_STALE;
    }
    if (now_ms >= machine->operation_deadline_ms) {
        return PROTO_ERR_STALE;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING) {
        return machine->discovery_delivery_token != 0u &&
                       machine->discovery_rf_started ?
            PROTO_OK : PROTO_ERR_BUSY;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_COLLECTING ||
        !machine->safety_deadline_armed ||
        now_ms >= machine->safety_deadline_ms) {
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

int gateway_survey_machine_admit_report(
    struct gateway_survey_machine *machine,
    uint32_t generation,
    uint32_t delivery_token,
    uint32_t survey_id,
    uint64_t now_ms,
    uint8_t attempts_started)
{
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING &&
        !machine->discovery_rf_started) {
        ret = gateway_survey_machine_match_discovery(
            machine, generation, delivery_token);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (attempts_started == 0u) {
            return PROTO_ERR_BUSY;
        }
        machine->discovery_rf_started = true;
    }
    return gateway_survey_machine_report_admissible(
        machine, generation, survey_id, now_ms);
}

static void gateway_survey_machine_drive_terminal(
    struct gateway_survey_machine *machine,
    struct gateway_survey_machine_drive *drive,
    enum gateway_survey_machine_terminal_reason reason)
{
    gateway_survey_machine_set_terminal(machine, reason);
    *drive = (struct gateway_survey_machine_drive) {
        .kind = GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL,
        .reason = reason,
    };
}

static void gateway_survey_machine_drive_existing_terminal(
    const struct gateway_survey_machine *machine,
    struct gateway_survey_machine_drive *drive)
{
    *drive = (struct gateway_survey_machine_drive) {
        .kind = GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL,
        .reason = machine->terminal_reason,
    };
}

int gateway_survey_machine_collection_drive(
    struct gateway_survey_machine *machine,
    uint64_t now_ms,
    size_t report_count,
    struct gateway_survey_machine_drive *drive)
{
    enum survey_gateway_collection_decision decision;
    bool emission_horizon_elapsed;
    bool safety_deadline_elapsed;
    uint64_t wake_at_ms;

    if (machine == NULL || drive == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_TERMINAL) {
        gateway_survey_machine_drive_existing_terminal(machine, drive);
        return PROTO_OK;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_READY) {
        *drive = (struct gateway_survey_machine_drive) {
            .kind = GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS,
            .reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NONE,
        };
        return PROTO_OK;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING &&
        machine->phase != GATEWAY_SURVEY_MACHINE_COLLECTING) {
        return PROTO_ERR_STALE;
    }
    if (report_count > SURVEY_GATEWAY_MAX_REPORTS) {
        gateway_survey_machine_drive_terminal(
            machine, drive, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return PROTO_OK;
    }
    if (now_ms >= machine->operation_deadline_ms) {
        gateway_survey_machine_drive_terminal(
            machine, drive,
            GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT);
        return PROTO_OK;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING) {
        *drive = (struct gateway_survey_machine_drive) {
            .kind = GATEWAY_SURVEY_MACHINE_DRIVE_WAIT,
            .wake_at_ms = machine->operation_deadline_ms,
            .reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NONE,
        };
        return PROTO_OK;
    }
    if (!machine->safety_deadline_armed) {
        gateway_survey_machine_drive_terminal(
            machine, drive, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return PROTO_OK;
    }

    emission_horizon_elapsed = now_ms >= machine->emission_deadline_ms;
    safety_deadline_elapsed = now_ms >= machine->safety_deadline_ms;
    decision = survey_gateway_collection_decide(
        emission_horizon_elapsed,
        safety_deadline_elapsed,
        report_count,
        machine->expected_count,
        machine->expected_count_present);
    if (decision == SURVEY_GATEWAY_COLLECTION_WAIT) {
        wake_at_ms = emission_horizon_elapsed ?
            machine->safety_deadline_ms : machine->emission_deadline_ms;
        if (machine->operation_deadline_ms < wake_at_ms) {
            wake_at_ms = machine->operation_deadline_ms;
        }
        if (wake_at_ms <= now_ms) {
            gateway_survey_machine_drive_terminal(
                machine, drive, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
            return PROTO_OK;
        }
        *drive = (struct gateway_survey_machine_drive) {
            .kind = GATEWAY_SURVEY_MACHINE_DRIVE_WAIT,
            .wake_at_ms = wake_at_ms,
            .reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NONE,
        };
        return PROTO_OK;
    }
    if (decision == SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH) {
        enum gateway_survey_machine_terminal_reason reason;

        if (report_count == 0u) {
            reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS;
        } else if (report_count < machine->expected_count) {
            reason =
                GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING;
        } else {
            reason =
                GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED;
        }
        gateway_survey_machine_drive_terminal(machine, drive, reason);
        return PROTO_OK;
    }
    if (decision != SURVEY_GATEWAY_COLLECTION_CLOSE) {
        gateway_survey_machine_drive_terminal(
            machine, drive, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return PROTO_OK;
    }
    if (report_count == 0u) {
        gateway_survey_machine_drive_terminal(
            machine, drive, GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS);
        return PROTO_OK;
    }

    machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_READY;
    *drive = (struct gateway_survey_machine_drive) {
        .kind = GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS,
        .reason = GATEWAY_SURVEY_MACHINE_TERMINAL_NONE,
    };
    return PROTO_OK;
}

int gateway_survey_machine_abort(struct gateway_survey_machine *machine,
                                 uint32_t generation,
                                 uint32_t survey_id)
{
    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (generation == 0u || generation != machine->generation ||
        survey_id == 0u || survey_id != machine->survey_id ||
        machine->phase == GATEWAY_SURVEY_MACHINE_IDLE) {
        return PROTO_ERR_STALE;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_TERMINAL) {
        return machine->terminal_reason ==
                       GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED ?
            PROTO_OK : PROTO_ERR_STALE;
    }
    gateway_survey_machine_set_terminal(
        machine, GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED);
    return PROTO_OK;
}

uint32_t gateway_survey_machine_generation(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->generation;
}

uint32_t gateway_survey_machine_survey_id(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->survey_id;
}

enum gateway_survey_machine_phase gateway_survey_machine_phase(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ?
        GATEWAY_SURVEY_MACHINE_IDLE : machine->phase;
}

bool gateway_survey_machine_active(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL &&
           machine->phase != GATEWAY_SURVEY_MACHINE_IDLE;
}

enum gateway_survey_machine_terminal_reason
gateway_survey_machine_terminal_reason(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ?
        GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL :
        machine->terminal_reason;
}

uint32_t gateway_survey_machine_discovery_delivery_token(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->discovery_delivery_token;
}

bool gateway_survey_machine_discovery_rf_started(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL && machine->discovery_rf_started;
}

uint64_t gateway_survey_machine_operation_deadline_ms(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->operation_deadline_ms;
}

uint32_t gateway_survey_machine_operation_remaining_ms(
    const struct gateway_survey_machine *machine,
    uint64_t now_ms)
{
    uint64_t remaining_ms;

    if (machine == NULL || machine->operation_deadline_ms <= now_ms) {
        return 0u;
    }
    remaining_ms = machine->operation_deadline_ms - now_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX :
           (uint32_t)remaining_ms;
}

uint64_t gateway_survey_machine_emission_deadline_ms(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->emission_deadline_ms;
}

bool gateway_survey_machine_safety_deadline_ms(
    const struct gateway_survey_machine *machine,
    uint64_t *deadline_ms)
{
    if (machine == NULL || deadline_ms == NULL ||
        !machine->safety_deadline_armed) {
        return false;
    }
    *deadline_ms = machine->safety_deadline_ms;
    return true;
}

uint64_t gateway_survey_machine_collection_duration_ms(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->collection_duration_ms;
}

uint16_t gateway_survey_machine_expected_count(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->expected_count;
}

bool gateway_survey_machine_expected_count_present(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL && machine->expected_count_present;
}

bool gateway_survey_machine_round_go_submit_retryable(int error)
{
    return error == -EAGAIN || error == -EBUSY ||
           error == -ENOSPC || error == -ESHUTDOWN;
}

bool gateway_survey_machine_round_go_terminal_retryable(
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

static int gateway_survey_machine_round_stage_details(
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

static void gateway_survey_machine_round_start_dispatch(
    struct gateway_survey_machine *machine)
{
    machine->dispatch_lane_index = 0u;
    machine->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
    machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING;
}

int gateway_survey_machine_round_begin(
    struct gateway_survey_machine *machine,
    const struct survey_gateway_context *planned_context,
    uint8_t max_parallel_pairs,
    uint8_t max_reruns)
{
    size_t planned_round_count = 0u;
    int ret;

    if (machine == NULL || planned_context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_ROUND_READY ||
        machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_STALE;
    }
    if (planned_context->survey_id != machine->survey_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    memset(&machine->round_runtime, 0, sizeof(machine->round_runtime));
    memset(machine->round_metadata, 0, sizeof(machine->round_metadata));
    machine->planned_round_count = 0u;
    ret = survey_gateway_plan_pair_rounds(planned_context,
                                           machine->round_metadata,
                                           SURVEY_GATEWAY_MAX_PAIRS,
                                           &planned_round_count);
    if (ret != PROTO_OK) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return ret;
    }
    ret = survey_pair_round_runtime_begin(&machine->round_runtime,
                                           planned_context,
                                           machine->round_metadata,
                                           planned_context->pair_count,
                                           max_parallel_pairs,
                                           max_reruns);
    if (ret != PROTO_OK) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return ret;
    }
    machine->planned_round_count = planned_round_count;
    ret = survey_pair_round_runtime_load_next_batch(&machine->round_runtime);
    if (ret != PROTO_OK) {
        if (ret == PROTO_ERR_NOT_FOUND &&
            survey_pair_round_runtime_complete(&machine->round_runtime)) {
            gateway_survey_machine_set_terminal(
                machine, GATEWAY_SURVEY_MACHINE_TERMINAL_NONE);
        } else {
            gateway_survey_machine_set_terminal(
                machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        }
        return ret;
    }
    gateway_survey_machine_round_start_dispatch(machine);
    return PROTO_OK;
}

bool gateway_survey_machine_round_active(
    const struct gateway_survey_machine *machine)
{
    if (machine == NULL) {
        return false;
    }
    switch (machine->phase) {
    case GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING:
    case GATEWAY_SURVEY_MACHINE_ROUND_GO_REQUIRED:
    case GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING:
    case GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE:
        return true;
    default:
        return false;
    }
}

size_t gateway_survey_machine_round_lane_count(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u :
        survey_pair_round_runtime_lane_count(&machine->round_runtime);
}

const struct survey_pair_round_lane *gateway_survey_machine_round_lane(
    const struct gateway_survey_machine *machine,
    size_t lane_index)
{
    return machine == NULL ? NULL :
        survey_pair_round_runtime_lane(&machine->round_runtime, lane_index);
}

uint16_t gateway_survey_machine_round_id(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL || !machine->round_runtime.active ?
        0u : machine->round_runtime.batch_sequence;
}

size_t gateway_survey_machine_planned_round_count(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ? 0u : machine->planned_round_count;
}

uint16_t gateway_survey_machine_round_success_count(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ?
        0u : machine->round_runtime.completed_success_count;
}

uint16_t gateway_survey_machine_round_failure_count(
    const struct gateway_survey_machine *machine)
{
    return machine == NULL ?
        0u : machine->round_runtime.completed_failure_count;
}

int gateway_survey_machine_round_current_control(
    const struct gateway_survey_machine *machine,
    struct gateway_survey_machine_control *control)
{
    const struct survey_pair_round_lane *lane;
    uint8_t endpoint_mask;
    int ret;

    if (machine == NULL || control == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_BUSY;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&machine->round_runtime,
                                           machine->dispatch_lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }

    memset(control, 0, sizeof(*control));
    ret = gateway_survey_machine_round_stage_details(
        lane,
        machine->dispatch_stage,
        &control->command_id,
        &control->target_id,
        &endpoint_mask);
    if (ret != PROTO_OK) {
        return ret;
    }
    control->pair = lane->pair;
    control->stage = machine->dispatch_stage;
    control->lane_index = machine->dispatch_lane_index;
    return PROTO_OK;
}

static bool gateway_survey_machine_round_lane_attempt_terminal(
    const struct survey_pair_round_lane *lane)
{
    return lane != NULL &&
           (lane->state == SURVEY_PAIR_ROUND_LANE_SUCCEEDED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_FAILED ||
            lane->state == SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
}

static bool gateway_survey_machine_round_every_live_lane_armed(
    const struct gateway_survey_machine *machine)
{
    const size_t lane_count =
        gateway_survey_machine_round_lane_count(machine);
    bool armed = false;

    if (lane_count == 0u) {
        return false;
    }
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&machine->round_runtime, i);

        if (survey_pair_round_lane_armed(lane)) {
            armed = true;
            continue;
        }
        if (!gateway_survey_machine_round_lane_attempt_terminal(lane)) {
            return false;
        }
    }
    return armed;
}

static void gateway_survey_machine_round_advance_dispatch(
    struct gateway_survey_machine *machine)
{
    machine->dispatch_lane_index++;
    while (machine->dispatch_lane_index <
           gateway_survey_machine_round_lane_count(machine)) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(
                &machine->round_runtime,
                machine->dispatch_lane_index);

        if (!gateway_survey_machine_round_lane_attempt_terminal(lane)) {
            machine->dispatch_stage =
                SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
            machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING;
            return;
        }
        machine->dispatch_lane_index++;
    }
    machine->dispatch_stage = SURVEY_GATEWAY_AUTO_IDLE;
    if (gateway_survey_machine_round_every_live_lane_armed(machine)) {
        machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_GO_REQUIRED;
    } else if (survey_pair_round_runtime_batch_complete(
                   &machine->round_runtime)) {
        machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE;
    }
}

int gateway_survey_machine_round_note_control_success(
    struct gateway_survey_machine *machine,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id)
{
    const struct survey_pair_round_lane *lane;
    enum command_id expected_command;
    uint64_t expected_target;
    uint8_t endpoint_mask;
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_BUSY;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&machine->round_runtime,
                                           machine->dispatch_lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = gateway_survey_machine_round_stage_details(
        lane,
        machine->dispatch_stage,
        &expected_command,
        &expected_target,
        &endpoint_mask);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (command_id != expected_command || target_id != expected_target ||
        survey_id != lane->pair.survey_id) {
        return PROTO_ERR_NOT_FOUND;
    }

    switch (machine->dispatch_stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        ret = survey_pair_round_runtime_note_prepared(
            &machine->round_runtime,
            machine->dispatch_lane_index,
            endpoint_mask);
        break;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        ret = survey_pair_round_runtime_note_started(
            &machine->round_runtime,
            machine->dispatch_lane_index,
            endpoint_mask);
        break;
    default:
        return PROTO_ERR_STALE;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    switch (machine->dispatch_stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
        machine->dispatch_stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER;
        break;
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        machine->dispatch_stage = SURVEY_GATEWAY_AUTO_START_RESPONDER;
        break;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
        machine->dispatch_stage = SURVEY_GATEWAY_AUTO_START_INITIATOR;
        break;
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        gateway_survey_machine_round_advance_dispatch(machine);
        break;
    default:
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

int gateway_survey_machine_round_note_control_failure(
    struct gateway_survey_machine *machine,
    enum command_id command_id,
    uint64_t target_id,
    uint32_t survey_id,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome,
    size_t *lane_index)
{
    struct gateway_survey_machine_control control;
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_BUSY;
    }
    ret = gateway_survey_machine_round_current_control(machine, &control);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (control.command_id != command_id || control.target_id != target_id ||
        control.pair.survey_id != survey_id) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = survey_pair_round_runtime_require_cleanup(
        &machine->round_runtime,
        control.lane_index,
        cleanup_mask,
        outcome);
    if (ret != PROTO_OK) {
        return ret;
    }

    machine->failed_control_cleanup_lane_index = control.lane_index;
    machine->failed_control_cleanup_lane_valid = true;
    if (lane_index != NULL) {
        *lane_index = control.lane_index;
    }
    gateway_survey_machine_round_advance_dispatch(machine);
    return PROTO_OK;
}

bool gateway_survey_machine_failed_control_cleanup_pending(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL && machine->failed_control_cleanup_lane_valid;
}

int gateway_survey_machine_failed_control_cleanup_lane(
    const struct gateway_survey_machine *machine,
    size_t *lane_index)
{
    if (machine == NULL || lane_index == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_NOT_FOUND;
    }
    *lane_index = machine->failed_control_cleanup_lane_index;
    return PROTO_OK;
}

int gateway_survey_machine_release_failed_control_cleanup(
    struct gateway_survey_machine *machine,
    size_t lane_index)
{
    const struct survey_pair_round_lane *lane;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!machine->failed_control_cleanup_lane_valid ||
        lane_index != machine->failed_control_cleanup_lane_index) {
        return PROTO_ERR_NOT_FOUND;
    }
    lane = survey_pair_round_runtime_lane(&machine->round_runtime, lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (lane->state == SURVEY_PAIR_ROUND_LANE_CLEANUP) {
        return PROTO_ERR_BUSY;
    }
    if (lane->state != SURVEY_PAIR_ROUND_LANE_FAILED &&
        lane->state != SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED) {
        return PROTO_ERR_STALE;
    }

    machine->failed_control_cleanup_lane_index = 0u;
    machine->failed_control_cleanup_lane_valid = false;
    return PROTO_OK;
}

bool gateway_survey_machine_round_go_needed(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL &&
           !machine->failed_control_cleanup_lane_valid &&
           machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_GO_REQUIRED &&
           gateway_survey_machine_round_every_live_lane_armed(machine);
}

int gateway_survey_machine_round_mark_observing_after_go(
    struct gateway_survey_machine *machine)
{
    size_t lane_count;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!gateway_survey_machine_round_go_needed(machine)) {
        return PROTO_ERR_STALE;
    }
    lane_count = gateway_survey_machine_round_lane_count(machine);
    for (size_t i = 0u; i < lane_count; i++) {
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&machine->round_runtime, i);
        int ret;

        if (!survey_pair_round_lane_armed(lane)) {
            continue;
        }
        ret = survey_pair_round_runtime_mark_observing(
            &machine->round_runtime, i);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING;
    return PROTO_OK;
}

int gateway_survey_machine_round_note_sample(
    struct gateway_survey_machine *machine,
    uint64_t reporter_id,
    const struct survey_sample *sample,
    size_t *lane_index,
    bool *accepted_new)
{
    if (machine == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    return survey_pair_round_runtime_note_sample(
        &machine->round_runtime,
        reporter_id,
        sample,
        lane_index,
        accepted_new);
}

static void gateway_survey_machine_round_update_batch_phase(
    struct gateway_survey_machine *machine)
{
    if (survey_pair_round_runtime_batch_complete(
            &machine->round_runtime)) {
        machine->phase = GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE;
    }
}

int gateway_survey_machine_round_finalize_lane(
    struct gateway_survey_machine *machine,
    size_t lane_index,
    uint8_t cleanup_mask,
    enum survey_pair_round_cleanup_outcome outcome)
{
    const struct survey_pair_round_lane *lane;
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    lane = survey_pair_round_runtime_lane(&machine->round_runtime, lane_index);
    if (lane == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (lane->state != SURVEY_PAIR_ROUND_LANE_OBSERVING) {
        return PROTO_ERR_STALE;
    }
    ret = survey_pair_round_runtime_require_cleanup(
        &machine->round_runtime,
        lane_index,
        cleanup_mask,
        outcome);
    if (ret == PROTO_OK) {
        gateway_survey_machine_round_update_batch_phase(machine);
    }
    return ret;
}

int gateway_survey_machine_round_note_cleanup_complete(
    struct gateway_survey_machine *machine,
    size_t lane_index,
    uint8_t completed_mask)
{
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_IDLE ||
        machine->phase == GATEWAY_SURVEY_MACHINE_TERMINAL) {
        return PROTO_ERR_STALE;
    }
    ret = survey_pair_round_runtime_note_cleanup_complete(
        &machine->round_runtime,
        lane_index,
        completed_mask);
    if (ret == PROTO_OK) {
        gateway_survey_machine_round_update_batch_phase(machine);
    }
    return ret;
}

bool gateway_survey_machine_round_batch_complete(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL &&
           machine->phase ==
               GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE &&
           survey_pair_round_runtime_batch_complete(
               &machine->round_runtime);
}

int gateway_survey_machine_round_advance_batch(
    struct gateway_survey_machine *machine,
    bool *complete)
{
    int ret;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->failed_control_cleanup_lane_valid) {
        return PROTO_ERR_BUSY;
    }
    if (!gateway_survey_machine_round_batch_complete(machine)) {
        return PROTO_ERR_BUSY;
    }
    if (survey_pair_round_runtime_complete(&machine->round_runtime)) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_NONE);
        if (complete != NULL) {
            *complete = true;
        }
        return PROTO_OK;
    }

    ret = survey_pair_round_runtime_load_next_batch(
        &machine->round_runtime);
    if (ret != PROTO_OK) {
        gateway_survey_machine_set_terminal(
            machine, GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
        return ret;
    }
    gateway_survey_machine_round_start_dispatch(machine);
    if (complete != NULL) {
        *complete = false;
    }
    return PROTO_OK;
}

bool gateway_survey_machine_round_complete(
    const struct gateway_survey_machine *machine)
{
    return machine != NULL &&
           machine->phase == GATEWAY_SURVEY_MACHINE_TERMINAL &&
           machine->terminal_reason ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_NONE &&
           !machine->failed_control_cleanup_lane_valid &&
           survey_pair_round_runtime_complete(&machine->round_runtime);
}

int gateway_survey_machine_validate(
    const struct gateway_survey_machine *machine)
{
    const struct survey_pair_round_lane *cleanup_lane = NULL;
    bool round_phase;

    if (machine == NULL) {
        return PROTO_ERR_ARG;
    }
    if (machine->generation == 0u ||
        machine->phase < GATEWAY_SURVEY_MACHINE_IDLE ||
        machine->phase > GATEWAY_SURVEY_MACHINE_TERMINAL ||
        !gateway_survey_machine_terminal_reason_valid(
            machine->terminal_reason)) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_IDLE) {
        return machine->survey_id == 0u &&
                       machine->operation_deadline_ms == 0u &&
                       machine->emission_deadline_ms == 0u &&
                       machine->safety_deadline_ms == 0u &&
                       machine->collection_duration_ms == 0u &&
                       machine->discovery_delivery_token == 0u &&
                       machine->terminal_reason ==
                           GATEWAY_SURVEY_MACHINE_TERMINAL_NONE &&
                       !machine->expected_count_present &&
                       !machine->discovery_rf_started &&
                       !machine->safety_deadline_armed &&
                       !machine->round_runtime.active &&
                       !machine->failed_control_cleanup_lane_valid ?
            PROTO_OK : PROTO_ERR_MALFORMED;
    }
    if (machine->survey_id == 0u ||
        machine->operation_deadline_ms == 0u ||
        machine->collection_duration_ms == 0u ||
        (machine->expected_count_present &&
         (machine->expected_count == 0u ||
          machine->expected_count > SURVEY_GATEWAY_MAX_REPORTS)) ||
        (!machine->expected_count_present &&
         machine->expected_count != 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->phase != GATEWAY_SURVEY_MACHINE_TERMINAL &&
        machine->terminal_reason !=
            GATEWAY_SURVEY_MACHINE_TERMINAL_NONE) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_DISCOVERY_PENDING &&
        machine->safety_deadline_armed) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_COLLECTING &&
        (machine->discovery_delivery_token != 0u ||
         !machine->discovery_rf_started ||
         !machine->safety_deadline_armed)) {
        return PROTO_ERR_MALFORMED;
    }

    round_phase =
        machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING ||
        machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_GO_REQUIRED ||
        machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_OBSERVING ||
        machine->phase ==
            GATEWAY_SURVEY_MACHINE_ROUND_BATCH_COMPLETE;
    if (round_phase &&
        (!machine->round_runtime.active ||
         machine->round_runtime.plan == NULL ||
         machine->round_runtime.plan->survey_id != machine->survey_id ||
         !machine->safety_deadline_armed)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!round_phase &&
        machine->phase != GATEWAY_SURVEY_MACHINE_TERMINAL &&
        machine->round_runtime.active) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_ROUND_DISPATCHING &&
        (machine->dispatch_stage <
             SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR ||
         machine->dispatch_stage >
             SURVEY_GATEWAY_AUTO_START_INITIATOR)) {
        return PROTO_ERR_MALFORMED;
    }
    if (machine->failed_control_cleanup_lane_valid) {
        cleanup_lane = survey_pair_round_runtime_lane(
            &machine->round_runtime,
            machine->failed_control_cleanup_lane_index);
        if (cleanup_lane == NULL ||
            (cleanup_lane->state != SURVEY_PAIR_ROUND_LANE_CLEANUP &&
             cleanup_lane->state != SURVEY_PAIR_ROUND_LANE_FAILED &&
             cleanup_lane->state !=
                 SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED)) {
            return PROTO_ERR_MALFORMED;
        }
    }
    if (machine->phase == GATEWAY_SURVEY_MACHINE_TERMINAL &&
        machine->terminal_reason ==
            GATEWAY_SURVEY_MACHINE_TERMINAL_NONE &&
        (!machine->round_runtime.active ||
         !survey_pair_round_runtime_complete(&machine->round_runtime))) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}
