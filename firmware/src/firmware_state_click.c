#include "firmware_state_machines.h"

#include "firmware_state_internal.h"

#include <limits.h>
#include <string.h>

void fw_button_sm_init(struct fw_button_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_click_sm_init(struct fw_click_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_anchor_click_sm_init(struct fw_anchor_click_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

static enum fw_sm_result button_transition(
    struct fw_button_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_button_state state,
    enum fw_effect_type effect,
    uint32_t timer_ms)
{
    machine->state = state;
    (void)fw_transition_finish(transition,
                               FW_SM_APPLIED,
                               (uint16_t)state,
                               effect,
                               event);
    if (effect == FW_EFFECT_START_TIMER) {
        transition->effect.payload.value = timer_ms;
    }
    return FW_SM_APPLIED;
}

enum fw_sm_result fw_button_sm_handle(void *context,
                                      const struct fw_event *event,
                                      struct fw_transition *transition)
{
    struct fw_button_sm *machine = context;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition,
                        event,
                        FW_MACHINE_BUTTON,
                        (uint16_t)machine->state);

    switch (machine->state) {
    case FW_BUTTON_IDLE:
        if (event->type == FW_EVENT_BUTTON_PRESSED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_DEBOUNCE,
                               FW_EFFECT_START_TIMER,
                               FW_BUTTON_DEBOUNCE_MS);
        }
        break;
    case FW_BUTTON_DEBOUNCE:
        if (event->type == FW_EVENT_BUTTON_RELEASED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_IDLE,
                                     FW_EFFECT_CANCEL_TIMER, 0u);
        }
        if (event->type == FW_EVENT_DEBOUNCE_CONFIRMED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_PRESSED,
                                     FW_EFFECT_START_TIMER,
                                     FW_BUTTON_LONG_PRESS_MS);
        }
        break;
    case FW_BUTTON_PRESSED:
        if (event->type == FW_EVENT_BUTTON_RELEASED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_NORMAL_CLICK,
                                     FW_EFFECT_PUBLISH_EVENT, 0u);
        }
        if (event->type == FW_EVENT_LONG_PRESS_DETECTED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_WAIT_LONG_RELEASE,
                                     FW_EFFECT_NONE, 0u);
        }
        break;
    case FW_BUTTON_WAIT_LONG_RELEASE:
        if (event->type == FW_EVENT_BUTTON_RELEASED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_SELF_TEST_ARMED,
                                     FW_EFFECT_START_TIMER,
                                     FW_BUTTON_SELF_TEST_ARM_MS);
        }
        break;
    case FW_BUTTON_SELF_TEST_ARMED:
        if (event->type == FW_EVENT_BUTTON_PRESSED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_SELF_TEST_CONFIRM,
                                     FW_EFFECT_CANCEL_TIMER, 0u);
        }
        if (event->type == FW_EVENT_TIMER_EXPIRED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_IDLE,
                                     FW_EFFECT_NONE, 0u);
        }
        break;
    case FW_BUTTON_SELF_TEST_CONFIRM:
        if (event->type == FW_EVENT_BUTTON_RELEASED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_SELF_TEST,
                                     FW_EFFECT_PUBLISH_EVENT, 0u);
        }
        if (event->type == FW_EVENT_LONG_PRESS_DETECTED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_IDLE,
                                     FW_EFFECT_PUBLISH_EVENT, 0u);
        }
        break;
    case FW_BUTTON_NORMAL_CLICK:
    case FW_BUTTON_SELF_TEST:
        if (event->type == FW_EVENT_ACTION_ACCEPTED) {
            return button_transition(machine, event, transition,
                                     FW_BUTTON_IDLE,
                                     FW_EFFECT_NONE, 0u);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition,
                                FW_SM_IGNORED,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE,
                                event);
}

static enum fw_sm_result click_transition(
    struct fw_click_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_click_state state,
    enum fw_effect_type effect)
{
    machine->state = state;
    if (state != FW_CLICK_WAKE) {
        machine->expected_rf_phase = FW_CLICK_IDLE;
        machine->expected_rf_attempt = 0u;
    }
    if (state == FW_CLICK_SUCCESS || state == FW_CLICK_FAILURE) {
        fw_operation_finish(&machine->identity);
    }
    return fw_transition_finish(transition,
                                FW_SM_APPLIED,
                                (uint16_t)state,
                                effect,
                                event);
}

static bool click_state_active(enum fw_click_state state)
{
    return state >= FW_CLICK_CREATE && state <= FW_CLICK_RETRY;
}

static uint16_t click_required_ranges(const struct fw_event *event)
{
    if (event == NULL || event->payload.value == 0u) {
        return FW_CLICK_MIN_UNIQUE_RANGES;
    }
    return event->payload.value > UINT8_MAX ?
               UINT8_MAX : (uint16_t)event->payload.value;
}

enum fw_sm_result fw_click_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition)
{
    struct fw_click_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition,
                        event,
                        FW_MACHINE_CLICK,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_START) {
        if (machine->state != FW_CLICK_IDLE &&
            machine->state != FW_CLICK_SUCCESS &&
            machine->state != FW_CLICK_FAILURE) {
            return fw_transition_finish(transition, FW_SM_BUSY,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        result = fw_operation_begin(&machine->identity, event);
        if (result != FW_SM_APPLIED) {
            return fw_transition_finish(transition, result,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        machine->attempts_started = 0u;
        machine->unique_ranges = 0u;
        machine->expected_rf_phase = FW_CLICK_IDLE;
        machine->expected_rf_attempt = 0u;
        return click_transition(machine, event, transition,
                                FW_CLICK_CREATE, FW_EFFECT_CLICK_CREATE);
    }

    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(
            transition,
            fw_operation_mismatch_result(&machine->identity, event),
            (uint16_t)machine->state,
            FW_EFFECT_NONE,
            event);
    }
    if (click_state_active(machine->state) &&
        event->type == FW_EVENT_RADIO_JOB_FAILED &&
        (event->payload.flags & FW_EVENT_FLAG_RETRYABLE) != 0u) {
        return click_transition(machine, event, transition,
                                FW_CLICK_RETRY, FW_EFFECT_START_TIMER);
    }
    if (click_state_active(machine->state) &&
        (event->type == FW_EVENT_CANCEL ||
         event->type == FW_EVENT_DEADLINE_EXPIRED ||
         event->type == FW_EVENT_RADIO_JOB_FAILED)) {
        return click_transition(machine, event, transition,
                                FW_CLICK_FAILURE, FW_EFFECT_CLICK_CLEANUP);
    }
    if (event->type == FW_EVENT_RF_STARTED &&
        machine->state != FW_CLICK_WAKE) {
        return fw_transition_finish(transition, FW_SM_IGNORED,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }
    switch (machine->state) {
    case FW_CLICK_CREATE:
        if (event->type == FW_EVENT_CLICK_CREATED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_POLITENESS,
                                    FW_EFFECT_CLICK_CHECK_POLITENESS);
        }
        break;
    case FW_CLICK_POLITENESS:
        if (event->type == FW_EVENT_HIGHER_PRIORITY_TRAFFIC) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_WAIT_PEER,
                                    FW_EFFECT_START_TIMER);
        }
        if (event->type == FW_EVENT_CHANNEL_CLEAR) {
            enum fw_sm_result channel_result;

            channel_result = click_transition(machine, event, transition,
                                              FW_CLICK_WAKE,
                                              FW_EFFECT_CLICK_SEND_WAKE);
            if (channel_result == FW_SM_APPLIED) {
                machine->expected_rf_phase = FW_CLICK_WAKE;
                machine->expected_rf_attempt =
                    machine->attempts_started < UINT8_MAX ?
                        (uint8_t)(machine->attempts_started + 1u) :
                        UINT8_MAX;
            }
            return channel_result;
        }
        break;
    case FW_CLICK_WAIT_PEER:
        if (event->type == FW_EVENT_PEER_WAIT_ENDED ||
            event->type == FW_EVENT_TIMER_EXPIRED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_POLITENESS,
                                    FW_EFFECT_CLICK_CHECK_POLITENESS);
        }
        break;
    case FW_CLICK_WAKE:
        if (event->type == FW_EVENT_RF_STARTED) {
            if (machine->expected_rf_phase != FW_CLICK_WAKE ||
                machine->expected_rf_attempt == 0u ||
                machine->expected_rf_attempt !=
                    (machine->attempts_started < UINT8_MAX ?
                         (uint8_t)(machine->attempts_started + 1u) :
                         UINT8_MAX)) {
                return fw_transition_finish(transition, FW_SM_IGNORED,
                                            (uint16_t)machine->state,
                                            FW_EFFECT_NONE, event);
            }
            if (machine->attempts_started < UINT8_MAX) {
                machine->attempts_started++;
            }
            machine->expected_rf_phase = FW_CLICK_IDLE;
            machine->expected_rf_attempt = 0u;
            return fw_transition_finish(transition, FW_SM_APPLIED,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        if (event->type == FW_EVENT_WAKE_COMPLETED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_DISCOVER,
                                    FW_EFFECT_CLICK_DISCOVER);
        }
        break;
    case FW_CLICK_DISCOVER:
        if (event->type == FW_EVENT_DISCOVERY_COMPLETED) {
            if (event->payload.count == 0u) {
                return click_transition(machine, event, transition,
                                        FW_CLICK_RETRY,
                                        FW_EFFECT_START_TIMER);
            }
            if (event->payload.count < click_required_ranges(event)) {
                return click_transition(machine, event, transition,
                                        FW_CLICK_RELEASE,
                                        FW_EFFECT_CLICK_SEND_RELEASE);
            }
            return click_transition(machine, event, transition,
                                    FW_CLICK_SCHEDULE,
                                    FW_EFFECT_CLICK_SEND_SCHEDULE);
        }
        break;
    case FW_CLICK_RELEASE:
        if (event->type == FW_EVENT_RELEASE_COMPLETED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_RETRY,
                                    FW_EFFECT_START_TIMER);
        }
        break;
    case FW_CLICK_SCHEDULE:
        if (event->type == FW_EVENT_SCHEDULE_COMPLETED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_RANGE,
                                    FW_EFFECT_CLICK_RANGE);
        }
        break;
    case FW_CLICK_RANGE:
        if (event->type == FW_EVENT_RANGE_COMPLETED) {
            machine->unique_ranges = event->payload.count > UINT8_MAX ?
                                         UINT8_MAX :
                                         (uint8_t)event->payload.count;
            machine->state = FW_CLICK_EVALUATE;
            if (machine->unique_ranges >= click_required_ranges(event)) {
                return click_transition(machine, event, transition,
                                        FW_CLICK_SUCCESS,
                                        FW_EFFECT_CLICK_CLEANUP);
            }
            return click_transition(machine, event, transition,
                                    FW_CLICK_RETRY,
                                    FW_EFFECT_START_TIMER);
        }
        break;
    case FW_CLICK_RETRY:
        if (event->type == FW_EVENT_RETRY_ALLOWED ||
            event->type == FW_EVENT_TIMER_EXPIRED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_POLITENESS,
                                    FW_EFFECT_CLICK_CHECK_POLITENESS);
        }
        if (event->type == FW_EVENT_RETRY_EXHAUSTED) {
            return click_transition(machine, event, transition,
                                    FW_CLICK_FAILURE,
                                    FW_EFFECT_CLICK_CLEANUP);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

static enum fw_sm_result anchor_click_transition(
    struct fw_anchor_click_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_anchor_click_state state,
    enum fw_effect_type effect)
{
    machine->state = state;
    if (state == FW_ANCHOR_CLICK_IDLE || state == FW_ANCHOR_CLICK_ABORTED) {
        fw_operation_finish(&machine->identity);
    }
    return fw_transition_finish(transition, FW_SM_APPLIED,
                                (uint16_t)state, effect, event);
}

enum fw_sm_result fw_anchor_click_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    struct fw_anchor_click_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_ANCHOR_CLICK,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_WAKE_CLAIM_ACCEPTED) {
        if (machine->state != FW_ANCHOR_CLICK_IDLE &&
            machine->state != FW_ANCHOR_CLICK_ABORTED) {
            return fw_transition_finish(transition, FW_SM_BUSY,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        result = fw_operation_begin(&machine->identity, event);
        if (result != FW_SM_APPLIED) {
            return fw_transition_finish(transition, result,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        return anchor_click_transition(machine, event, transition,
                                       FW_ANCHOR_CLICK_CLAIMED,
                                       FW_EFFECT_ANCHOR_WAIT_SCHEDULE);
    }
    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(transition, FW_SM_STALE,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }
    if (machine->state == FW_ANCHOR_CLICK_RESULT_OWNED &&
        (event->type == FW_EVENT_CANCEL ||
         event->type == FW_EVENT_DEADLINE_EXPIRED ||
         event->type == FW_EVENT_RADIO_JOB_FAILED)) {
        /* A retained result owns terminal custody until explicit release. */
        return fw_transition_finish(transition, FW_SM_IGNORED,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }
    if (event->type == FW_EVENT_CANCEL ||
        event->type == FW_EVENT_DEADLINE_EXPIRED ||
        event->type == FW_EVENT_RADIO_JOB_FAILED) {
        return anchor_click_transition(machine, event, transition,
                                       FW_ANCHOR_CLICK_ABORTED,
                                       FW_EFFECT_ANCHOR_CLEANUP);
    }

    switch (machine->state) {
    case FW_ANCHOR_CLICK_CLAIMED:
        if (event->type == FW_EVENT_DISCOVER_RECEIVED) {
            return anchor_click_transition(
                machine, event, transition,
                FW_ANCHOR_CLICK_DISCOVERY_REPLIED,
                FW_EFFECT_ANCHOR_SEND_DISCOVERY_REPLY);
        }
        break;
    case FW_ANCHOR_CLICK_DISCOVERY_REPLIED:
        if (event->type == FW_EVENT_SCHEDULE_RECEIVED) {
            return anchor_click_transition(machine, event, transition,
                                           FW_ANCHOR_CLICK_SCHEDULED,
                                           FW_EFFECT_START_TIMER);
        }
        break;
    case FW_ANCHOR_CLICK_SCHEDULED:
        if (event->type == FW_EVENT_RANGE_DUE ||
            event->type == FW_EVENT_TIMER_EXPIRED) {
            return anchor_click_transition(machine, event, transition,
                                           FW_ANCHOR_CLICK_RANGING,
                                           FW_EFFECT_ANCHOR_RANGE);
        }
        break;
    case FW_ANCHOR_CLICK_RANGING:
        if (event->type == FW_EVENT_RESULT_RETAINED) {
            return anchor_click_transition(machine, event, transition,
                                           FW_ANCHOR_CLICK_RESULT_OWNED,
                                           FW_EFFECT_ANCHOR_RETAIN_RESULT);
        }
        break;
    case FW_ANCHOR_CLICK_RESULT_OWNED:
        if (event->type == FW_EVENT_RESULT_CUSTODY_RELEASED) {
            return anchor_click_transition(machine, event, transition,
                                           FW_ANCHOR_CLICK_IDLE,
                                           FW_EFFECT_ANCHOR_CLEANUP);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}
