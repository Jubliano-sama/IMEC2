#include "firmware_state_machines.h"

#include "firmware_state_internal.h"

#include <limits.h>
#include <string.h>

void fw_route_sm_init(struct fw_route_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_connection_sm_init(struct fw_connection_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_delivery_sm_init(struct fw_delivery_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

static enum fw_sm_result route_transition(
    struct fw_route_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_route_state state,
    enum fw_effect_type effect,
    bool finish)
{
    machine->state = state;
    if (finish) {
        fw_operation_finish(&machine->identity);
    }
    return fw_transition_finish(transition, FW_SM_APPLIED,
                                (uint16_t)state, effect, event);
}

enum fw_sm_result fw_route_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition)
{
    struct fw_route_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_ROUTE,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_ROUTE_NEEDED) {
        if (machine->state != FW_ROUTE_EMPTY) {
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
        machine->parent_id = 0u;
        return route_transition(machine, event, transition,
                                FW_ROUTE_DIRECT_PROBE,
                                FW_EFFECT_ROUTE_DIRECT_PROBE, false);
    }
    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(transition, FW_SM_STALE,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }

    switch (event->type) {
    case FW_EVENT_DIRECT_PROBE_FAILED:
        if (machine->state == FW_ROUTE_DIRECT_PROBE) {
            return route_transition(machine, event, transition,
                                    FW_ROUTE_DISCOVERING,
                                    FW_EFFECT_ROUTE_DISCOVER, false);
        }
        break;
    case FW_EVENT_ROUTE_FOUND:
        if ((machine->state == FW_ROUTE_DIRECT_PROBE ||
             machine->state == FW_ROUTE_DISCOVERING) &&
            event->payload.subject_id != 0u) {
            machine->parent_id = event->payload.subject_id;
            return route_transition(machine, event, transition,
                                    FW_ROUTE_READY,
                                    FW_EFFECT_PUBLISH_EVENT, false);
        }
        break;
    case FW_EVENT_ROUTE_FAILED:
        if (machine->state == FW_ROUTE_DIRECT_PROBE ||
            machine->state == FW_ROUTE_DISCOVERING) {
            machine->parent_id = 0u;
            return route_transition(machine, event, transition,
                                    FW_ROUTE_EMPTY,
                                    FW_EFFECT_TRACE_TERMINAL, true);
        }
        break;
    case FW_EVENT_ROUTE_INVALIDATED:
        if (machine->state == FW_ROUTE_READY) {
            machine->parent_id = 0u;
            return route_transition(machine, event, transition,
                                    FW_ROUTE_HOLD_DOWN,
                                    FW_EFFECT_ROUTE_HOLD_DOWN, false);
        }
        break;
    case FW_EVENT_HOLD_DOWN_EXPIRED:
        if (machine->state == FW_ROUTE_HOLD_DOWN) {
            return route_transition(machine, event, transition,
                                    FW_ROUTE_EMPTY,
                                    FW_EFFECT_PUBLISH_EVENT, true);
        }
        break;
    case FW_EVENT_ROUTE_CLOSE_REQUESTED:
        if (machine->state == FW_ROUTE_READY) {
            return route_transition(machine, event, transition,
                                    FW_ROUTE_CLOSING,
                                    FW_EFFECT_ROUTE_CLOSE, false);
        }
        break;
    case FW_EVENT_ROUTE_CLOSE_COMPLETED:
        if (machine->state == FW_ROUTE_CLOSING) {
            machine->parent_id = 0u;
            return route_transition(machine, event, transition,
                                    FW_ROUTE_EMPTY,
                                    FW_EFFECT_PUBLISH_EVENT, true);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

static enum fw_sm_result connection_transition(
    struct fw_connection_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_connection_state state,
    enum fw_effect_type effect,
    bool finish)
{
    machine->state = state;
    if (finish) {
        fw_operation_finish(&machine->identity);
    }
    return fw_transition_finish(transition, FW_SM_APPLIED,
                                (uint16_t)state, effect, event);
}

enum fw_sm_result fw_connection_sm_handle(void *context,
                                          const struct fw_event *event,
                                          struct fw_transition *transition)
{
    struct fw_connection_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_CONNECTION,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_CONNECTION_NEEDED) {
        if (event->payload.subject_id == 0u ||
            (machine->state != FW_CONNECTION_EMPTY &&
             machine->state != FW_CONNECTION_STALE)) {
            return fw_transition_finish(
                transition,
                machine->state == FW_CONNECTION_EMPTY ? FW_SM_INVALID :
                                                        FW_SM_BUSY,
                (uint16_t)machine->state,
                FW_EFFECT_NONE,
                event);
        }
        result = fw_operation_begin(&machine->identity, event);
        if (result != FW_SM_APPLIED) {
            return fw_transition_finish(transition, result,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        machine->peer_id = event->payload.subject_id;
        machine->consecutive_missed_rx = 0u;
        return connection_transition(machine, event, transition,
                                     FW_CONNECTION_NEGOTIATING,
                                     FW_EFFECT_CONNECTION_NEGOTIATE, false);
    }
    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(transition, FW_SM_STALE,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }

    switch (event->type) {
    case FW_EVENT_PROPOSAL_ACCEPTED:
        if (machine->state == FW_CONNECTION_NEGOTIATING) {
            machine->event_counter = event->payload.value;
            machine->consecutive_missed_rx = 0u;
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_ACTIVE,
                                         FW_EFFECT_PUBLISH_EVENT, false);
        }
        break;
    case FW_EVENT_NEGOTIATION_FAILED:
        if (machine->state == FW_CONNECTION_NEGOTIATING) {
            machine->peer_id = 0u;
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_EMPTY,
                                         FW_EFFECT_TRACE_TERMINAL, true);
        }
        break;
    case FW_EVENT_CONNECTION_EVENT_COMPLETED:
        if (machine->state == FW_CONNECTION_ACTIVE) {
            machine->event_counter++;
            machine->consecutive_missed_rx = 0u;
            return fw_transition_finish(transition, FW_SM_APPLIED,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        break;
    case FW_EVENT_RECEIVE_TURN_MISSED:
        if (machine->state == FW_CONNECTION_ACTIVE) {
            if (machine->consecutive_missed_rx < UINT8_MAX) {
                machine->consecutive_missed_rx++;
            }
            if (machine->consecutive_missed_rx >=
                FW_CONNECTION_MISSED_RX_LIMIT) {
                /* Retire this generation so delayed work cannot repair it. */
                return connection_transition(machine, event, transition,
                                             FW_CONNECTION_STALE,
                                             FW_EFFECT_PUBLISH_EVENT, true);
            }
            return fw_transition_finish(transition, FW_SM_APPLIED,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        break;
    case FW_EVENT_SUPERVISION_EXPIRED:
        if (machine->state == FW_CONNECTION_ACTIVE) {
            /* Retire this generation so delayed work cannot repair it. */
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_STALE,
                                         FW_EFFECT_PUBLISH_EVENT, true);
        }
        break;
    case FW_EVENT_CLICK_ACCEPTED:
        if (machine->state == FW_CONNECTION_ACTIVE ||
            machine->state == FW_CONNECTION_STALE ||
            machine->state == FW_CONNECTION_NEGOTIATING) {
            machine->peer_id = 0u;
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_EMPTY,
                                         FW_EFFECT_CONNECTION_CLOSE, true);
        }
        break;
    case FW_EVENT_CONNECTION_CLOSE_REQUESTED:
        if (machine->state == FW_CONNECTION_ACTIVE ||
            machine->state == FW_CONNECTION_STALE) {
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_CLOSING,
                                         FW_EFFECT_CONNECTION_CLOSE, false);
        }
        break;
    case FW_EVENT_CONNECTION_CLOSE_COMPLETED:
        if (machine->state == FW_CONNECTION_CLOSING) {
            machine->peer_id = 0u;
            return connection_transition(machine, event, transition,
                                         FW_CONNECTION_EMPTY,
                                         FW_EFFECT_PUBLISH_EVENT, true);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

static enum fw_sm_result delivery_transition(
    struct fw_delivery_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_delivery_state state,
    enum fw_effect_type effect,
    bool finish)
{
    machine->state = state;
    if (finish) {
        fw_operation_finish(&machine->identity);
    }
    return fw_transition_finish(transition, FW_SM_APPLIED,
                                (uint16_t)state, effect, event);
}

enum fw_sm_result fw_delivery_sm_handle(void *context,
                                        const struct fw_event *event,
                                        struct fw_transition *transition)
{
    struct fw_delivery_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_DELIVERY,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_PACKET_OWNED) {
        if (machine->state != FW_DELIVERY_EMPTY &&
            machine->state != FW_DELIVERY_TRANSFERRED &&
            machine->state != FW_DELIVERY_DELIVERED &&
            machine->state != FW_DELIVERY_FAILED) {
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
        machine->owns_custody = true;
        machine->state = FW_DELIVERY_OWNED;
        if ((event->payload.flags & FW_EVENT_FLAG_ROUTE_READY) == 0u) {
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_ROUTE,
                                       FW_EFFECT_DELIVERY_WAIT_ROUTE, false);
        }
        if ((event->payload.flags & FW_EVENT_FLAG_CONNECTION_READY) == 0u) {
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_CONNECTION,
                                       FW_EFFECT_DELIVERY_WAIT_CONNECTION,
                                       false);
        }
        return delivery_transition(machine, event, transition,
                                   FW_DELIVERY_WAIT_TX,
                                   FW_EFFECT_DELIVERY_SEND, false);
    }
    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(transition, FW_SM_STALE,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }
    if (event->type == FW_EVENT_CANCEL ||
        event->type == FW_EVENT_DEADLINE_EXPIRED ||
        event->type == FW_EVENT_RETRY_EXHAUSTED) {
        return delivery_transition(machine, event, transition,
                                   FW_DELIVERY_FAILED,
                                   FW_EFFECT_DELIVERY_FAIL, true);
    }

    switch (machine->state) {
    case FW_DELIVERY_WAIT_ROUTE:
        if (event->type == FW_EVENT_ROUTE_READY) {
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_CONNECTION,
                                       FW_EFFECT_DELIVERY_WAIT_CONNECTION,
                                       false);
        }
        break;
    case FW_DELIVERY_WAIT_CONNECTION:
        if (event->type == FW_EVENT_CONNECTION_READY) {
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_TX,
                                       FW_EFFECT_DELIVERY_SEND, false);
        }
        break;
    case FW_DELIVERY_WAIT_TX:
        if (event->type == FW_EVENT_RF_DEFERRED) {
            return fw_transition_finish(transition, FW_SM_APPLIED,
                                        (uint16_t)machine->state,
                                        FW_EFFECT_NONE, event);
        }
        if (event->type == FW_EVENT_RF_STARTED) {
            if (machine->attempts_started < UINT8_MAX) {
                machine->attempts_started++;
            }
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_ACK,
                                       FW_EFFECT_START_TIMER, false);
        }
        break;
    case FW_DELIVERY_WAIT_ACK:
        if (event->type == FW_EVENT_HOP_ACK_RECEIVED) {
            machine->owns_custody = false;
            return delivery_transition(
                machine, event, transition, FW_DELIVERY_TRANSFERRED,
                FW_EFFECT_DELIVERY_TRANSFER_CUSTODY, true);
        }
        if (event->type == FW_EVENT_GATEWAY_ACK_RECEIVED) {
            machine->owns_custody = false;
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_DELIVERED,
                                       FW_EFFECT_DELIVERY_COMPLETE, true);
        }
        if (event->type == FW_EVENT_ACK_TIMED_OUT) {
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_RETRY,
                                       FW_EFFECT_START_TIMER, false);
        }
        break;
    case FW_DELIVERY_RETRY:
        if (event->type == FW_EVENT_RETRY_ALLOWED) {
            if ((event->payload.flags & FW_EVENT_FLAG_PATH_USABLE) != 0u) {
                return delivery_transition(machine, event, transition,
                                           FW_DELIVERY_WAIT_TX,
                                           FW_EFFECT_DELIVERY_SEND, false);
            }
            return delivery_transition(machine, event, transition,
                                       FW_DELIVERY_WAIT_ROUTE,
                                       FW_EFFECT_DELIVERY_WAIT_ROUTE, false);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}
