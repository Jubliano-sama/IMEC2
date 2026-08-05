#ifndef BUTTON_ACTION_HANDOFF_H
#define BUTTON_ACTION_HANDOFF_H

#include "status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUTTON_ACTION_HANDOFF_CAPACITY 16u

enum button_action_handoff_submit_result {
    BUTTON_ACTION_HANDOFF_QUEUED = 0,
    BUTTON_ACTION_HANDOFF_START_OWNER,
    BUTTON_ACTION_HANDOFF_FULL,
};

/*
 * Callers serialize these helpers.  The owner remains set while an action is
 * running and while the button is re-armed, so a completion-edge submission
 * is retained in the FIFO instead of observing a false busy state.
 */
struct button_action_handoff {
    enum button_action queue[BUTTON_ACTION_HANDOFF_CAPACITY];
    uint8_t head;
    uint8_t count;
    bool owner_active;
};

static inline void button_action_handoff_init(
    struct button_action_handoff *handoff)
{
    if (handoff == NULL) {
        return;
    }
    handoff->head = 0u;
    handoff->count = 0u;
    handoff->owner_active = false;
}

static inline enum button_action_handoff_submit_result
button_action_handoff_submit(struct button_action_handoff *handoff,
                             enum button_action action)
{
    uint8_t tail;
    bool start_owner;

    if (handoff == NULL || action == BUTTON_ACTION_NONE ||
        handoff->count >= BUTTON_ACTION_HANDOFF_CAPACITY) {
        return BUTTON_ACTION_HANDOFF_FULL;
    }

    tail = (uint8_t)((handoff->head + handoff->count) %
                     BUTTON_ACTION_HANDOFF_CAPACITY);
    handoff->queue[tail] = action;
    handoff->count++;
    start_owner = !handoff->owner_active;
    handoff->owner_active = true;
    return start_owner ? BUTTON_ACTION_HANDOFF_START_OWNER :
                         BUTTON_ACTION_HANDOFF_QUEUED;
}

static inline bool button_action_handoff_take(
    struct button_action_handoff *handoff,
    enum button_action *action)
{
    if (handoff == NULL || action == NULL || !handoff->owner_active ||
        handoff->count == 0u) {
        return false;
    }

    *action = handoff->queue[handoff->head];
    handoff->head =
        (uint8_t)((handoff->head + 1u) % BUTTON_ACTION_HANDOFF_CAPACITY);
    handoff->count--;
    return true;
}

static inline bool button_action_handoff_release_if_empty(
    struct button_action_handoff *handoff)
{
    if (handoff == NULL || handoff->count != 0u) {
        return false;
    }

    /*
     * A Zephyr work item that is resubmitted while its handler is running can
     * execute once more after the owner drained.  Make empty release
     * idempotent so that harmless trailing invocation cannot turn into a
     * recovery reset.
     */
    handoff->owner_active = false;
    return true;
}

#endif
