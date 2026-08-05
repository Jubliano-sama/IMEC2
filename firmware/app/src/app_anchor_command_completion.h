#ifndef APP_ANCHOR_COMMAND_COMPLETION_H
#define APP_ANCHOR_COMMAND_COMPLETION_H

#include "node_comm.h"

#include <stdbool.h>
#include <stdint.h>

enum app_anchor_command_completion_action {
    APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY = 1u << 0,
    APP_ANCHOR_COMMAND_COMPLETION_REBOOT = 1u << 1,
};

/*
 * Result sequence zero is reserved. The node boot counter is the outer
 * incarnation identity, so wrapping this per-boot cursor skips zero.
 */
uint16_t app_anchor_command_completion_next_result_seq(uint16_t current);

typedef int (*app_anchor_command_completion_clear_fn)(void *context);
typedef bool (*app_anchor_command_completion_take_fn)(
    uint32_t delivery_handle,
    struct node_comm_terminal_event *event_out,
    void *context);

/*
 * Commit one already-peeked terminal event without opening a reset window
 * that can repeat a post-result side effect.
 *
 * A delivered terminal first clears the durable result/action owner, then
 * consumes the exact delivery terminal. A failed clear leaves the terminal
 * untouched. A non-delivered terminal is consumed while durable custody stays
 * intact so the caller can resubmit the same result.
 *
 * Returns 1 for a durably completed delivery, 0 for a consumed failure that
 * needs exact resubmission, or a negative errno. -EPROTO means the terminal
 * was consumed but its supposedly immutable metadata changed; all other
 * negative errors leave terminal ownership unconsumed.
 */
int app_anchor_command_completion_commit_terminal(
    uint32_t delivery_handle,
    const struct node_comm_terminal_event *peeked_event,
    app_anchor_command_completion_clear_fn clear_durable,
    app_anchor_command_completion_take_fn take_terminal,
    void *context,
    struct node_comm_terminal_event *taken_event_out);

#endif
