#include "app_anchor_command_completion.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum trace_step {
    TRACE_CLEAR = 1,
    TRACE_TAKE,
};

struct test_context {
    struct node_comm_terminal_event terminal;
    enum trace_step trace[2];
    size_t trace_len;
    int clear_ret;
    bool take_ret;
};

static int clear_durable(void *context)
{
    struct test_context *test = context;

    test->trace[test->trace_len++] = TRACE_CLEAR;
    return test->clear_ret;
}

static bool take_terminal(uint32_t delivery_handle,
                          struct node_comm_terminal_event *event_out,
                          void *context)
{
    struct test_context *test = context;

    test->trace[test->trace_len++] = TRACE_TAKE;
    if (!test->take_ret || delivery_handle != test->terminal.handle) {
        return false;
    }
    *event_out = test->terminal;
    return true;
}

static struct test_context make_context(enum node_comm_terminal_reason reason)
{
    struct test_context test = {
        .terminal = {
            .handle = 17u,
            .client_token = 0x10203040u,
            .terminal_at_ms = UINT64_C(0x123456789),
            .reason = reason,
            .attempts_started = 3u,
        },
        .take_ret = true,
    };

    return test;
}

static void test_result_sequence_wrap_skips_reserved_zero(void)
{
    assert(app_anchor_command_completion_next_result_seq(0u) == 1u);
    assert(app_anchor_command_completion_next_result_seq(UINT16_MAX - 1u) ==
           UINT16_MAX);
    assert(app_anchor_command_completion_next_result_seq(UINT16_MAX) == 1u);
}

static void test_delivered_clears_before_exact_take(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &test.terminal,
               clear_durable,
               take_terminal,
               &test,
               &taken) == 1);
    assert(test.trace_len == 2u);
    assert(test.trace[0] == TRACE_CLEAR);
    assert(test.trace[1] == TRACE_TAKE);
    assert(memcmp(&taken, &test.terminal, sizeof(taken)) == 0);
}

static void test_clear_failure_retains_terminal(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event taken = {0};

    test.clear_ret = -EIO;
    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &test.terminal,
               clear_durable,
               take_terminal,
               &test,
               &taken) == -EIO);
    assert(test.trace_len == 1u);
    assert(test.trace[0] == TRACE_CLEAR);
}

static void test_failure_terminal_stays_durable_and_requests_resubmit(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &test.terminal,
               clear_durable,
               take_terminal,
               &test,
               &taken) == 0);
    assert(test.trace_len == 1u);
    assert(test.trace[0] == TRACE_TAKE);
    assert(taken.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
}

static void test_stale_terminal_does_not_mutate_owners(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event stale = test.terminal;
    struct node_comm_terminal_event taken = {0};

    stale.handle++;
    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &stale,
               clear_durable,
               take_terminal,
               &test,
               &taken) == -ESTALE);
    assert(test.trace_len == 0u);
}

static void test_failed_take_is_recoverable_after_clear(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event taken = {0};

    test.take_ret = false;
    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &test.terminal,
               clear_durable,
               take_terminal,
               &test,
               &taken) == -EAGAIN);
    assert(test.trace_len == 2u);
    assert(test.trace[0] == TRACE_CLEAR);
    assert(test.trace[1] == TRACE_TAKE);
}

static void test_consumed_metadata_mismatch_requires_new_handle(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event peeked = test.terminal;
    struct node_comm_terminal_event taken = {0};

    test.terminal.client_token++;
    assert(app_anchor_command_completion_commit_terminal(
               test.terminal.handle,
               &peeked,
               clear_durable,
               take_terminal,
               &test,
               &taken) == -EPROTO);
    assert(test.trace_len == 2u);
    assert(test.trace[0] == TRACE_CLEAR);
    assert(test.trace[1] == TRACE_TAKE);
}

int main(void)
{
    test_result_sequence_wrap_skips_reserved_zero();
    test_delivered_clears_before_exact_take();
    test_clear_failure_retains_terminal();
    test_failure_terminal_stays_durable_and_requests_resubmit();
    test_stale_terminal_does_not_mutate_owners();
    test_failed_take_is_recoverable_after_clear();
    test_consumed_metadata_mismatch_requires_new_handle();
    return 0;
}
