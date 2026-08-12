#include "firmware_state_machines.h"

#include "firmware_state_internal.h"

#include <errno.h>
#include <string.h>

void fw_radio_sm_init(struct fw_radio_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_radio_handoff_sm_init(struct fw_radio_handoff_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_radio_activity_runtime_init(struct fw_radio_activity_runtime *runtime)
{
    if (runtime != NULL) {
        memset(runtime, 0, sizeof(*runtime));
    }
}

static void radio_activity_decision_init(
    struct fw_radio_activity_decision *decision)
{
    memset(decision, 0, sizeof(*decision));
    decision->state = FW_RADIO_ACTIVITY_IDLE;
    decision->mesh_work_allowed = true;
    decision->c5_tx_allowed = true;
    decision->route_wait_allowed = true;
    decision->report_tx_allowed = true;
    decision->uwb_rx_allowed = true;
    decision->reason = "idle";
}

int fw_radio_activity_decide(
    const struct fw_radio_activity_capture *capture,
    struct fw_radio_activity_runtime *runtime,
    struct fw_radio_activity_decision *decision,
    bool *state_changed)
{
    if (capture == NULL || decision == NULL) {
        return -EINVAL;
    }
    if (state_changed != NULL) {
        *state_changed = false;
    }
    radio_activity_decision_init(decision);

    if (capture->click_active) {
        decision->state = FW_RADIO_ACTIVITY_CLICK;
        decision->mesh_work_allowed = false;
        decision->c5_tx_allowed = false;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "click";
    } else if (capture->survey_pending) {
        decision->state = FW_RADIO_ACTIVITY_SURVEY;
        decision->mesh_work_allowed = false;
        decision->c5_tx_allowed = false;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "survey";
    } else if (capture->gateway_continuous_ch9) {
        /*
         * The gateway receiver owns the physical radio for this interval.
         * Keep UWB RX enabled for that owner, but defer every competing
         * mesh, route, and report producer until it releases the radio.
         */
        decision->state = FW_RADIO_ACTIVITY_GATEWAY_RX;
        decision->mesh_work_allowed = false;
        decision->c5_tx_allowed = false;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = "gateway-rx";
    } else if (capture->rx_queue_used > 0u ||
               capture->ch9_ack_send_pending ||
               capture->ch9_ack_wait_active) {
        bool live_ack_owner = capture->ch9_ack_send_pending ||
                              capture->ch9_ack_wait_active;

        decision->state = FW_RADIO_ACTIVITY_MESH_RX;
        /* The handler may owe an immediate response to the exact packet it
         * just dequeued while unrelated RX records remain queued behind it.
         * Queue depth alone cannot suppress that causal response, but a live
         * ACK deadline/send owner still outranks every Channel-5 exchange. */
        decision->c5_tx_allowed =
            !live_ack_owner && capture->rx_queue_used > 0u &&
            capture->c5_tx_intent == FW_C5_TX_INTENT_CAUSAL_RESPONSE;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = decision->c5_tx_allowed ?
                           "mesh-rx-causal-response" :
                           capture->rx_queue_used > 0u ? "mesh-rx" :
                           (capture->ch9_ack_send_pending ?
                                "ch9-ack-send" : "ch9-ack-wait");
    } else if (capture->ch9_ack_receive_eligible) {
        /* Retry backoff and forwarded-custody retention still accept a late
         * ACK, but they do not own a live deadline that can veto Channel 5. */
        decision->state = FW_RADIO_ACTIVITY_MESH_RX;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = "ch9-ack-rx-eligible";
    } else if (capture->relay_tx_active ||
               capture->route_waiting_tx_active) {
        decision->state = FW_RADIO_ACTIVITY_MESH_TX;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "mesh-tx";
    } else if (capture->report_queue_used > 0u) {
        decision->state = FW_RADIO_ACTIVITY_MESH_TX;
        decision->uwb_rx_allowed = false;
        decision->reason = "mesh-tx";
    }

    if (runtime != NULL &&
        (!runtime->last_state_valid ||
         runtime->last_state != decision->state)) {
        runtime->last_state_valid = true;
        runtime->last_state = decision->state;
        if (state_changed != NULL) {
            *state_changed = true;
        }
    }
    return 0;
}

const char *fw_radio_activity_state_name(enum fw_radio_activity_state state)
{
    switch (state) {
    case FW_RADIO_ACTIVITY_IDLE:
        return "idle";
    case FW_RADIO_ACTIVITY_CLICK:
        return "click";
    case FW_RADIO_ACTIVITY_SURVEY:
        return "survey";
    case FW_RADIO_ACTIVITY_MESH_RX:
        return "mesh-rx";
    case FW_RADIO_ACTIVITY_MESH_TX:
        return "mesh-tx";
    case FW_RADIO_ACTIVITY_GATEWAY_RX:
        return "gateway-rx";
    default:
        return "unknown";
    }
}

static bool handoff_serial_after(uint32_t candidate, uint32_t reference)
{
    uint32_t distance;

    if (candidate == 0u || reference == 0u || candidate == reference) {
        return false;
    }
    distance = candidate - reference;
    return distance < UINT32_C(0x80000000);
}

static enum fw_sm_result handoff_finish(
    struct fw_radio_handoff_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_sm_result result,
    enum fw_effect_type effect)
{
    uint32_t admission_cutoff = machine->admission_cutoff;

    machine->state = FW_RADIO_HANDOFF_IDLE;
    fw_operation_finish(&machine->identity);
    (void)fw_transition_finish(transition, result,
                               (uint16_t)machine->state, effect, event);
    transition->effect.payload.value = admission_cutoff;
    return result;
}

enum fw_sm_result fw_radio_handoff_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    struct fw_radio_handoff_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_RADIO,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_RADIO_PREEMPT_REQUESTED) {
        if (machine->identity.active) {
            if (!fw_operation_matches(&machine->identity, event)) {
                return fw_transition_finish(
                    transition, FW_SM_BUSY, (uint16_t)machine->state,
                    FW_EFFECT_NONE, event);
            }
            if (machine->state == FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY &&
                handoff_serial_after(event->payload.value,
                                     machine->admission_cutoff)) {
                machine->admission_cutoff = event->payload.value;
            }
            return fw_transition_finish(
                transition, FW_SM_APPLIED, (uint16_t)machine->state,
                FW_EFFECT_NONE, event);
        }
        if (event->payload.value == 0u) {
            return fw_transition_finish(
                transition, FW_SM_INVALID, (uint16_t)machine->state,
                FW_EFFECT_NONE, event);
        }
        result = fw_operation_begin(&machine->identity, event);
        if (result != FW_SM_APPLIED) {
            return fw_transition_finish(
                transition, result, (uint16_t)machine->state,
                FW_EFFECT_NONE, event);
        }
        machine->admission_cutoff = event->payload.value;
        machine->schedule_attempts = 0u;
        machine->state = FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY;
        return fw_transition_finish(
            transition, FW_SM_APPLIED, (uint16_t)machine->state,
            FW_EFFECT_RADIO_REQUEST_ABORT, event);
    }

    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(
            transition, FW_SM_STALE, (uint16_t)machine->state,
            FW_EFFECT_NONE, event);
    }

    if ((event->type == FW_EVENT_RADIO_SAFE_BOUNDARY &&
         machine->state == FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY) ||
        (event->type == FW_EVENT_TIMER_EXPIRED &&
         machine->state == FW_RADIO_HANDOFF_WAIT_RETRY)) {
        machine->state = FW_RADIO_HANDOFF_SCHEDULING;
        if (machine->schedule_attempts < UINT8_MAX) {
            machine->schedule_attempts++;
        }
        (void)fw_transition_finish(
            transition, FW_SM_APPLIED, (uint16_t)machine->state,
            FW_EFFECT_RADIO_SCHEDULE_PENDING, event);
        transition->effect.payload.value = machine->admission_cutoff;
        return FW_SM_APPLIED;
    }

    if (event->type == FW_EVENT_EFFECT_SUCCEEDED &&
        machine->state == FW_RADIO_HANDOFF_SCHEDULING) {
        return handoff_finish(machine, event, transition,
                              FW_SM_APPLIED, FW_EFFECT_NONE);
    }

    if (event->type == FW_EVENT_EFFECT_FAILED &&
        machine->state == FW_RADIO_HANDOFF_SCHEDULING) {
        if ((event->payload.flags & FW_EVENT_FLAG_RETRYABLE) != 0u &&
            machine->schedule_attempts <
                FW_RADIO_HANDOFF_MAX_SCHEDULE_ATTEMPTS) {
            machine->state = FW_RADIO_HANDOFF_WAIT_RETRY;
            (void)fw_transition_finish(
                transition, FW_SM_APPLIED, (uint16_t)machine->state,
                FW_EFFECT_START_TIMER, event);
            transition->effect.payload.value = machine->admission_cutoff;
            return FW_SM_APPLIED;
        }
        return handoff_finish(machine, event, transition,
                              FW_SM_APPLIED,
                              FW_EFFECT_RADIO_CLEAR_ABORT);
    }

    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

static bool radio_request_valid(const struct fw_event *event)
{
    enum fw_radio_mode mode = FW_RADIO_REQUEST_MODE(event->payload.value);
    enum fw_radio_priority priority =
        FW_RADIO_REQUEST_PRIORITY(event->payload.value);

    return event->operation_id != 0u && event->generation != 0u &&
           event->reply_to > FW_MACHINE_NONE &&
           event->reply_to < FW_MACHINE_COUNT &&
           (event->payload.channel == FW_RADIO_CHANNEL_5 ||
            event->payload.channel == FW_RADIO_CHANNEL_9) &&
           (mode == FW_RADIO_MODE_RX || mode == FW_RADIO_MODE_TX) &&
           priority >= FW_RADIO_PRIORITY_BACKGROUND &&
           priority <= FW_RADIO_PRIORITY_GATEWAY_CONTROL &&
           event->payload.duration_ms != 0u &&
           event->payload.deadline_ms > event->payload.not_before_ms &&
           event->payload.duration_ms <=
               event->payload.deadline_ms - event->payload.not_before_ms;
}

static struct fw_radio_job radio_job_from_event(const struct fw_event *event)
{
    return (struct fw_radio_job) {
        .identity = {
            .operation_id = event->operation_id,
            .generation = event->generation,
            .active = true,
        },
        .owner = event->reply_to,
        .owner_instance = event->reply_instance,
        .channel = (enum fw_radio_channel)event->payload.channel,
        .mode = FW_RADIO_REQUEST_MODE(event->payload.value),
        .priority = FW_RADIO_REQUEST_PRIORITY(event->payload.value),
        .not_before_ms = event->payload.not_before_ms,
        .deadline_ms = event->payload.deadline_ms,
        .maximum_duration_ms = event->payload.duration_ms,
    };
}

static void radio_effect_owner(struct fw_transition *transition,
                               const struct fw_radio_job *job)
{
    transition->effect.operation_id = job->identity.operation_id;
    transition->effect.generation = job->identity.generation;
    transition->effect.owner = job->owner;
    transition->effect.owner_instance = job->owner_instance;
    transition->effect.payload.not_before_ms = job->not_before_ms;
    transition->effect.payload.deadline_ms = job->deadline_ms;
    transition->effect.payload.duration_ms = job->maximum_duration_ms;
    transition->effect.payload.channel = (uint8_t)job->channel;
    transition->effect.payload.value =
        FW_RADIO_REQUEST_VALUE(job->mode, job->priority);
}

static enum fw_sm_result start_stored_radio_job(
    struct fw_radio_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    enum fw_effect_type effect;

    if (machine->state == FW_RADIO_OFF) {
        machine->state = FW_RADIO_STARTING;
        effect = FW_EFFECT_RADIO_INITIALIZE;
    } else if (machine->active.channel == machine->tuned_channel) {
        machine->state = machine->active.channel == FW_RADIO_CHANNEL_5 ?
                             FW_RADIO_BUSY_5 : FW_RADIO_BUSY_9;
        effect = FW_EFFECT_RADIO_RUN_JOB;
    } else {
        machine->state = machine->active.channel == FW_RADIO_CHANNEL_5 ?
                             FW_RADIO_RETUNING_5 : FW_RADIO_RETUNING_9;
        effect = FW_EFFECT_RADIO_RETUNE;
    }
    (void)fw_transition_finish(transition,
                               FW_SM_APPLIED,
                               (uint16_t)machine->state,
                               effect,
                               event);
    radio_effect_owner(transition, &machine->active);
    return FW_SM_APPLIED;
}

static enum fw_sm_result start_radio_job(struct fw_radio_sm *machine,
                                         const struct fw_event *event,
                                         struct fw_transition *transition)
{
    machine->active = radio_job_from_event(event);
    return start_stored_radio_job(machine, event, transition);
}

static bool radio_active_event_matches(const struct fw_radio_sm *machine,
                                       const struct fw_event *event)
{
    return machine->active.identity.active &&
           machine->active.identity.operation_id == event->operation_id &&
           machine->active.identity.generation == event->generation;
}

static enum fw_sm_result radio_report_and_ready(
    struct fw_radio_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    struct fw_radio_job completed = machine->active;
    bool pending = machine->pending.identity.active;

    machine->tuned_channel = completed.channel;
    /*
     * A pending preemptor makes the terminal report a two-step handoff:
     * report the completed job first, then wait for its ACTION_ACCEPTED
     * before promoting pending custody. The single-effect API cannot report
     * and start both owners in one transition.
     */
    machine->state = pending ? FW_RADIO_CANCELLED_WAIT_RELEASE :
                               (completed.channel == FW_RADIO_CHANNEL_5 ?
                                    FW_RADIO_READY_5 : FW_RADIO_READY_9);
    if (!pending) {
        memset(&machine->active, 0, sizeof(machine->active));
    }
    (void)fw_transition_finish(transition,
                               FW_SM_APPLIED,
                               (uint16_t)machine->state,
                               FW_EFFECT_RADIO_REPORT_RESULT,
                               event);
    radio_effect_owner(transition, &completed);
    transition->effect.payload.value = (uint32_t)event->type;
    return FW_SM_APPLIED;
}

enum fw_sm_result fw_radio_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition)
{
    struct fw_radio_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition,
                        event,
                        FW_MACHINE_RADIO,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_RADIO_REQUESTED) {
        if (!radio_request_valid(event)) {
            return fw_transition_finish(transition,
                                        FW_SM_INVALID,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE,
                                        event);
        }
        if (machine->state == FW_RADIO_OFF ||
            machine->state == FW_RADIO_READY_5 ||
            machine->state == FW_RADIO_READY_9) {
            return start_radio_job(machine, event, transition);
        }
        if (machine->state != FW_RADIO_BUSY_5 &&
            machine->state != FW_RADIO_BUSY_9) {
            return fw_transition_finish(transition,
                                        FW_SM_BUSY,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE,
                                        event);
        }
        if (machine->pending.identity.active) {
            return fw_transition_finish(transition,
                                        FW_SM_BUSY,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE,
                                        event);
        }
        if (FW_RADIO_REQUEST_PRIORITY(event->payload.value) <=
                machine->active.priority ||
            event->payload.channel != FW_RADIO_CHANNEL_5 ||
            FW_RADIO_REQUEST_PRIORITY(event->payload.value) <
                FW_RADIO_PRIORITY_CLICK) {
            return fw_transition_finish(transition,
                                        FW_SM_BUSY,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE,
                                        event);
        }
        machine->pending = radio_job_from_event(event);
        machine->state = FW_RADIO_CANCELLING;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_CANCEL_JOB,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    }

    if (!radio_active_event_matches(machine, event)) {
        result = FW_SM_STALE;
        return fw_transition_finish(transition,
                                    result,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE,
                                    event);
    }

    if (event->type == FW_EVENT_CANCEL &&
        machine->state != FW_RADIO_CANCELLING &&
        machine->state != FW_RADIO_CANCELLED_WAIT_RELEASE &&
        machine->state != FW_RADIO_RECOVERING) {
        machine->state = FW_RADIO_CANCELLING;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_CANCEL_JOB,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    }

    if (event->type == FW_EVENT_RADIO_RECOVERY_EXHAUSTED &&
        machine->state == FW_RADIO_RECOVERING) {
        struct fw_radio_job failed = machine->active;

        if (machine->pending.identity.active) {
            /*
             * The radio is off, so both owners need terminal evidence. Keep
             * both records through the first report; ACTION_ACCEPTED then
             * reports pending and a second ACTION_ACCEPTED retires it. A
             * single transition must never clear one owner silently.
             */
            machine->tuned_channel = FW_RADIO_CHANNEL_NONE;
            machine->state = FW_RADIO_CANCELLED_WAIT_RELEASE;
        } else {
            memset(&machine->active, 0, sizeof(machine->active));
            machine->state = FW_RADIO_OFF;
        }
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_REPORT_RESULT,
                                   event);
        radio_effect_owner(transition, &failed);
        transition->effect.payload.value =
            (uint32_t)FW_EVENT_RADIO_RECOVERY_EXHAUSTED;
        return FW_SM_APPLIED;
    }

    switch (event->type) {
    case FW_EVENT_RADIO_INIT_SUCCEEDED:
        if (machine->state != FW_RADIO_STARTING) {
            break;
        }
        machine->state = machine->active.channel == FW_RADIO_CHANNEL_5 ?
                             FW_RADIO_BUSY_5 : FW_RADIO_BUSY_9;
        machine->tuned_channel = machine->active.channel;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_RUN_JOB,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    case FW_EVENT_RADIO_RETUNE_SUCCEEDED:
        if (machine->state != FW_RADIO_RETUNING_5 &&
            machine->state != FW_RADIO_RETUNING_9) {
            break;
        }
        machine->state = machine->active.channel == FW_RADIO_CHANNEL_5 ?
                             FW_RADIO_BUSY_5 : FW_RADIO_BUSY_9;
        machine->tuned_channel = machine->active.channel;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_RUN_JOB,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    case FW_EVENT_RADIO_JOB_COMPLETED:
    case FW_EVENT_RADIO_JOB_TIMED_OUT:
        if (machine->state != FW_RADIO_BUSY_5 &&
            machine->state != FW_RADIO_BUSY_9) {
            break;
        }
        return radio_report_and_ready(machine, event, transition);
    case FW_EVENT_RADIO_JOB_CANCELLED:
        if (machine->state != FW_RADIO_CANCELLING) {
            break;
        }
        machine->state = FW_RADIO_CANCELLED_WAIT_RELEASE;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_REPORT_RESULT,
                                   event);
        radio_effect_owner(transition, &machine->active);
        transition->effect.payload.value =
            (uint32_t)FW_EVENT_RADIO_JOB_CANCELLED;
        return FW_SM_APPLIED;
    case FW_EVENT_ACTION_ACCEPTED:
        if (machine->state != FW_RADIO_CANCELLED_WAIT_RELEASE) {
            break;
        }
        if (machine->tuned_channel == FW_RADIO_CHANNEL_NONE &&
            machine->pending.identity.active) {
            struct fw_radio_job failed_pending = machine->pending;

            machine->active = failed_pending;
            memset(&machine->pending, 0, sizeof(machine->pending));
            (void)fw_transition_finish(transition,
                                       FW_SM_APPLIED,
                                       (uint16_t)machine->state,
                                       FW_EFFECT_RADIO_REPORT_RESULT,
                                       event);
            radio_effect_owner(transition, &failed_pending);
            transition->effect.payload.value =
                (uint32_t)FW_EVENT_RADIO_RECOVERY_EXHAUSTED;
            return FW_SM_APPLIED;
        }
        memset(&machine->active, 0, sizeof(machine->active));
        if (!machine->pending.identity.active) {
            machine->state = machine->tuned_channel == FW_RADIO_CHANNEL_NONE ?
                                 FW_RADIO_OFF :
                                 (machine->tuned_channel == FW_RADIO_CHANNEL_5 ?
                                      FW_RADIO_READY_5 : FW_RADIO_READY_9);
            return fw_transition_finish(transition,
                                        FW_SM_APPLIED,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE,
                                        event);
        }
        machine->active = machine->pending;
        memset(&machine->pending, 0, sizeof(machine->pending));
        return start_stored_radio_job(machine, event, transition);
    case FW_EVENT_RADIO_INIT_FAILED:
    case FW_EVENT_RADIO_JOB_FAILED:
        if (machine->state != FW_RADIO_STARTING &&
            machine->state != FW_RADIO_BUSY_5 &&
            machine->state != FW_RADIO_BUSY_9 &&
            machine->state != FW_RADIO_RETUNING_5 &&
            machine->state != FW_RADIO_RETUNING_9 &&
            machine->state != FW_RADIO_CANCELLING) {
            break;
        }
        machine->state = FW_RADIO_RECOVERING;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_RECOVER,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    case FW_EVENT_RADIO_RECOVERY_RETRY:
        if (machine->state != FW_RADIO_RECOVERING) {
            break;
        }
        machine->state = FW_RADIO_STARTING;
        (void)fw_transition_finish(transition,
                                   FW_SM_APPLIED,
                                   (uint16_t)machine->state,
                                   FW_EFFECT_RADIO_INITIALIZE,
                                   event);
        radio_effect_owner(transition, &machine->active);
        return FW_SM_APPLIED;
    default:
        break;
    }
    return fw_transition_finish(transition,
                                FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE,
                                event);
}
