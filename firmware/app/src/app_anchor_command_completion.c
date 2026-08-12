#include "app_anchor_command_completion.h"

#include <errno.h>
#include <string.h>

uint16_t app_anchor_command_completion_next_result_seq(uint16_t current)
{
    current++;
    return current == 0u ? 1u : current;
}

int app_anchor_command_completion_take_terminal_exact(
    uint32_t delivery_handle,
    const struct node_comm_terminal_event *peeked_event,
    app_anchor_command_completion_take_fn take_terminal,
    void *context,
    struct node_comm_terminal_event *taken_event_out)
{
    struct node_comm_terminal_event taken_event;

    if (delivery_handle == 0u || peeked_event == NULL ||
        take_terminal == NULL || taken_event_out == NULL) {
        return -EINVAL;
    }
    if (peeked_event->handle != delivery_handle) {
        return -ESTALE;
    }

    memset(&taken_event, 0, sizeof(taken_event));
    if (!take_terminal(delivery_handle, &taken_event, context)) {
        return -EAGAIN;
    }
    if (taken_event.handle != peeked_event->handle ||
        taken_event.delivery_generation !=
            peeked_event->delivery_generation ||
        taken_event.client_token != peeked_event->client_token ||
        taken_event.terminal_at_ms != peeked_event->terminal_at_ms ||
        taken_event.reason != peeked_event->reason ||
        taken_event.attempts_started != peeked_event->attempts_started ||
        taken_event.proof != peeked_event->proof) {
        /*
         * The exact terminal has already been consumed. Distinguish this
         * impossible immutable-record mismatch from a stale peek so the
         * RAM owner can discard the dead handle and resubmit.
         */
        return -EPROTO;
    }
    *taken_event_out = taken_event;
    return taken_event.reason == NODE_COMM_TERMINAL_DELIVERED ? 1 : 0;
}
