#include "firmware_state_machines.h"

#include "firmware_state_internal.h"

#include <string.h>

void fw_gateway_uwb_sm_init(struct fw_gateway_uwb_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_host_link_sm_init(struct fw_host_link_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

void fw_enumeration_sm_init(struct fw_enumeration_sm *machine)
{
    if (machine != NULL) {
        memset(machine, 0, sizeof(*machine));
    }
}

static enum fw_sm_result simple_transition(
    struct fw_transition *transition,
    const struct fw_event *event,
    uint16_t state,
    enum fw_effect_type effect)
{
    return fw_transition_finish(transition, FW_SM_APPLIED,
                                state, effect, event);
}

static enum fw_sm_result gateway_uwb_transition(
    struct fw_gateway_uwb_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_gateway_uwb_state state,
    enum fw_effect_type effect,
    bool host_item_pending)
{
    machine->state = state;
    machine->host_item_pending = host_item_pending;
    return simple_transition(transition, event, (uint16_t)state, effect);
}

enum fw_sm_result fw_gateway_uwb_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    struct fw_gateway_uwb_sm *machine = context;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_GATEWAY_UWB,
                        (uint16_t)machine->state);

    /*
     * Reset abandons only the volatile gateway-side reservation. It never
     * authorizes an upstream ACK; the source therefore retains custody and
     * can retry after the gateway starts listening again.
     */
    if (event->type == FW_EVENT_RESET) {
        return gateway_uwb_transition(machine, event, transition,
                                      FW_GATEWAY_UWB_LISTEN_9,
                                      FW_EFFECT_NONE, false);
    }

    switch (machine->state) {
    case FW_GATEWAY_UWB_LISTEN_9:
        if (event->type == FW_EVENT_GATEWAY_BATCH_RECEIVED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_RECEIVE_BATCH,
                FW_EFFECT_GATEWAY_VALIDATE_BATCH, false);
        }
        break;
    case FW_GATEWAY_UWB_RECEIVE_BATCH:
        if (event->type == FW_EVENT_GATEWAY_BATCH_ACCEPTED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_ACCEPT_BATCH,
                FW_EFFECT_GATEWAY_ACCEPT_BATCH, false);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    case FW_GATEWAY_UWB_ACCEPT_BATCH:
        if (event->type == FW_EVENT_ACTION_ACCEPTED) {
            /*
             * Acceptance reserves one bounded host item. The host-link
             * service may be blocked, so this effect queues the item and
             * custody remains here until HOST_ITEM_ACCEPTED arrives.
             */
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                FW_EFFECT_HOST_SEND_ITEM, true);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    case FW_GATEWAY_UWB_WAIT_HOST_ITEM:
        /*
         * HOST_ITEM_SENT belongs to the transport machine: it proves only
         * that the notification left the gateway. The GUI's process-RAM
         * receipt is the custody boundary, and only HOST_ITEM_ACCEPTED may
         * authorize the upstream ACK. BLE blocked/ready events and a
         * premature GUI receipt are deliberately ignored here.
         */
        if (event->type == FW_EVENT_HOST_ITEM_SENT &&
            machine->host_item_pending) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
                FW_EFFECT_NONE, true);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    case FW_GATEWAY_UWB_WAIT_GUI_RECEIPT:
        /*
         * Transport completion has already occurred. Only the GUI's
         * process-RAM receipt can now authorize the upstream ACK; BLE
         * blocked/ready/sent events do not change custody.
         */
        if (event->type == FW_EVENT_HOST_ITEM_ACCEPTED &&
            machine->host_item_pending) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_SEND_ACK,
                FW_EFFECT_GATEWAY_SEND_ACK, true);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    case FW_GATEWAY_UWB_SEND_ACK:
        if (event->type == FW_EVENT_GATEWAY_ACK_SENT) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_RETIRE_HOST_ITEM,
                FW_EFFECT_HOST_RETIRE_ITEM, true);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            /*
             * ACK transmission failed, so the source still owns the
             * packet. Discard only this volatile gateway reservation; a
             * later source retry will establish a fresh bounded item.
             */
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    case FW_GATEWAY_UWB_RETIRE_HOST_ITEM:
        if (event->type == FW_EVENT_ACTION_ACCEPTED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        if (event->type == FW_EVENT_OPERATION_FAILED) {
            return gateway_uwb_transition(
                machine, event, transition,
                FW_GATEWAY_UWB_LISTEN_9,
                FW_EFFECT_NONE, false);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_IGNORED,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

enum fw_sm_result fw_host_link_sm_handle(void *context,
                                         const struct fw_event *event,
                                         struct fw_transition *transition)
{
    struct fw_host_link_sm *machine = context;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_HOST_LINK,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_BLE_BLOCKED) {
        machine->state = FW_HOST_LINK_BLOCKED;
        return simple_transition(transition, event,
                                 (uint16_t)machine->state,
                                 FW_EFFECT_NONE);
    }
    if (event->type == FW_EVENT_HOST_ITEM_QUEUED) {
        machine->item_pending = true;
        if (machine->state == FW_HOST_LINK_READY) {
            machine->state = FW_HOST_LINK_SENDING;
            return simple_transition(transition, event,
                                     (uint16_t)machine->state,
                                     FW_EFFECT_HOST_SEND_ITEM);
        }
        return simple_transition(transition, event,
                                 (uint16_t)machine->state,
                                 FW_EFFECT_NONE);
    }
    if (event->type == FW_EVENT_BLE_READY &&
        machine->state == FW_HOST_LINK_BLOCKED) {
        if (machine->item_pending) {
            machine->state = FW_HOST_LINK_SENDING;
            return simple_transition(transition, event,
                                     (uint16_t)machine->state,
                                     FW_EFFECT_HOST_SEND_ITEM);
        }
        machine->state = FW_HOST_LINK_READY;
        return simple_transition(transition, event,
                                 (uint16_t)machine->state,
                                 FW_EFFECT_NONE);
    }
    if (event->type == FW_EVENT_HOST_ITEM_SENT &&
        machine->state == FW_HOST_LINK_SENDING) {
        if ((event->payload.flags & FW_EVENT_FLAG_MORE_PENDING) != 0u) {
            machine->item_pending = true;
            return simple_transition(transition, event,
                                     (uint16_t)machine->state,
                                     FW_EFFECT_HOST_SEND_ITEM);
        }
        machine->item_pending = false;
        machine->state = FW_HOST_LINK_READY;
        return simple_transition(transition, event,
                                 (uint16_t)machine->state,
                                 FW_EFFECT_NONE);
    }
    return fw_transition_finish(transition, FW_SM_IGNORED,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}

static enum fw_sm_result enumeration_transition(
    struct fw_enumeration_sm *machine,
    const struct fw_event *event,
    struct fw_transition *transition,
    enum fw_enumeration_state state,
    enum fw_effect_type effect,
    bool finish)
{
    machine->state = state;
    if (finish) {
        fw_operation_finish(&machine->identity);
    }
    return simple_transition(transition, event, (uint16_t)state, effect);
}

enum fw_sm_result fw_enumeration_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition)
{
    struct fw_enumeration_sm *machine = context;
    enum fw_sm_result result;

    if (machine == NULL || event == NULL || transition == NULL) {
        return FW_SM_INVALID;
    }
    fw_transition_begin(transition, event, FW_MACHINE_ENUMERATION,
                        (uint16_t)machine->state);

    if (event->type == FW_EVENT_START) {
        if (machine->state != FW_ENUMERATION_IDLE &&
            machine->state != FW_ENUMERATION_COMPLETE &&
            machine->state != FW_ENUMERATION_FAILED) {
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
        return enumeration_transition(machine, event, transition,
                                      FW_ENUMERATION_SEND_CLAIM,
                                      FW_EFFECT_ENUM_SEND_CLAIM, false);
    }
    if (!fw_operation_matches(&machine->identity, event)) {
        return fw_transition_finish(transition, FW_SM_STALE,
                                    (uint16_t)machine->state,
                                    FW_EFFECT_NONE, event);
    }
    if (event->type == FW_EVENT_CANCEL ||
        event->type == FW_EVENT_DEADLINE_EXPIRED ||
        event->type == FW_EVENT_OPERATION_FAILED) {
        return enumeration_transition(machine, event, transition,
                                      FW_ENUMERATION_FAILED,
                                      FW_EFFECT_TRACE_TERMINAL, true);
    }

    switch (machine->state) {
    case FW_ENUMERATION_SEND_CLAIM:
        if (event->type == FW_EVENT_CLAIM_SENT) {
            return enumeration_transition(
                machine, event, transition,
                FW_ENUMERATION_COLLECT_RESPONSES,
                FW_EFFECT_START_TIMER, false);
        }
        break;
    case FW_ENUMERATION_COLLECT_RESPONSES:
        if (event->type == FW_EVENT_RESPONSE_WINDOW_CLOSED) {
            return enumeration_transition(machine, event, transition,
                                          FW_ENUMERATION_FREEZE_TABLE,
                                          FW_EFFECT_ENUM_FREEZE_RESPONSES,
                                          false);
        }
        break;
    case FW_ENUMERATION_FREEZE_TABLE:
        if (event->type == FW_EVENT_RESPONSES_FROZEN) {
            return enumeration_transition(machine, event, transition,
                                          FW_ENUMERATION_SEND_TABLE,
                                          FW_EFFECT_ENUM_SEND_TABLE, false);
        }
        break;
    case FW_ENUMERATION_SEND_TABLE:
        if (event->type == FW_EVENT_TABLE_SENT ||
            event->type == FW_EVENT_PUBLICATION_COMPLETED) {
            return enumeration_transition(machine, event, transition,
                                          FW_ENUMERATION_COMPLETE,
                                          FW_EFFECT_ENUM_COMPLETE, true);
        }
        break;
    default:
        break;
    }
    return fw_transition_finish(transition, FW_SM_INVALID,
                                (uint16_t)machine->state,
                                FW_EFFECT_NONE, event);
}
