#ifndef FIRMWARE_STATE_INTERNAL_H
#define FIRMWARE_STATE_INTERNAL_H

#include "firmware_state_machines.h"

#include <string.h>

static inline void fw_transition_begin(struct fw_transition *transition,
                                       const struct fw_event *event,
                                       enum fw_machine_id machine,
                                       uint16_t old_state)
{
    memset(transition, 0, sizeof(*transition));
    transition->machine = machine;
    transition->event = event->type;
    transition->instance = event->target_instance;
    transition->old_state = old_state;
    transition->new_state = old_state;
    transition->result = FW_SM_IGNORED;
}

static inline enum fw_sm_result fw_transition_finish(
    struct fw_transition *transition,
    enum fw_sm_result result,
    uint16_t new_state,
    enum fw_effect_type effect_type,
    const struct fw_event *event)
{
    transition->result = result;
    transition->new_state = new_state;
    if (effect_type != FW_EFFECT_NONE) {
        transition->effect.type = effect_type;
        transition->effect.operation_id = event->operation_id;
        transition->effect.generation = event->generation;
        transition->effect.owner = event->reply_to;
        transition->effect.owner_instance = event->reply_instance;
        transition->effect.payload = event->payload;
    }
    return result;
}

static inline bool fw_generation_newer(uint32_t candidate, uint32_t current)
{
    uint32_t difference = candidate - current;

    return difference != 0u && difference < UINT32_C(0x80000000);
}

static inline enum fw_sm_result fw_operation_begin(
    struct fw_operation_identity *identity,
    const struct fw_event *event)
{
    if (event->operation_id == 0u || event->generation == 0u) {
        return FW_SM_INVALID;
    }
    if (identity->active) {
        return identity->operation_id == event->operation_id &&
                       identity->generation == event->generation ?
                   FW_SM_IGNORED : FW_SM_BUSY;
    }
    if (identity->generation != 0u &&
        !fw_generation_newer(event->generation, identity->generation)) {
        return FW_SM_STALE;
    }
    identity->operation_id = event->operation_id;
    identity->generation = event->generation;
    identity->active = true;
    return FW_SM_APPLIED;
}

static inline bool fw_operation_matches(
    const struct fw_operation_identity *identity,
    const struct fw_event *event)
{
    return identity->active &&
           identity->operation_id == event->operation_id &&
           identity->generation == event->generation;
}

static inline void fw_operation_finish(struct fw_operation_identity *identity)
{
    identity->active = false;
}

static inline enum fw_sm_result fw_operation_mismatch_result(
    const struct fw_operation_identity *identity,
    const struct fw_event *event)
{
    if (!identity->active || event->operation_id == 0u ||
        event->generation == 0u) {
        return FW_SM_STALE;
    }
    return FW_SM_STALE;
}

#endif
