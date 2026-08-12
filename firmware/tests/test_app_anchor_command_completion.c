#include "app_anchor_command_completion.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

struct test_context {
    struct node_comm_terminal_event terminal;
    size_t take_calls;
    bool take_ret;
};

static bool take_terminal(uint32_t delivery_handle,
                          struct node_comm_terminal_event *event_out,
                          void *context)
{
    struct test_context *test = context;

    test->take_calls++;
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
            .delivery_generation = 29u,
            .client_token = 0x10203040u,
            .terminal_at_ms = UINT64_C(0x123456789),
            .reason = reason,
            .attempts_started = 3u,
            .proof = NODE_COMM_TERMINAL_PROOF_SEMANTIC,
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

static bool terminal_equal(const struct node_comm_terminal_event *left,
                           const struct node_comm_terminal_event *right)
{
    return left->handle == right->handle &&
           left->delivery_generation == right->delivery_generation &&
           left->client_token == right->client_token &&
           left->terminal_at_ms == right->terminal_at_ms &&
           left->reason == right->reason &&
           left->attempts_started == right->attempts_started &&
           left->proof == right->proof;
}

static void test_delivered_exact_take_succeeds(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_take_terminal_exact(
               test.terminal.handle,
               &test.terminal,
               take_terminal,
               &test,
               &taken) == 1);
    assert(test.take_calls == 1u);
    assert(terminal_equal(&taken, &test.terminal));
}

static void test_failure_terminal_requests_exact_resubmit(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_take_terminal_exact(
               test.terminal.handle,
               &test.terminal,
               take_terminal,
               &test,
               &taken) == 0);
    assert(test.take_calls == 1u);
    assert(taken.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
}

static void test_stale_terminal_does_not_mutate_owners(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event stale = test.terminal;
    struct node_comm_terminal_event taken = {0};

    stale.handle++;
    assert(app_anchor_command_completion_take_terminal_exact(
               test.terminal.handle,
               &stale,
               take_terminal,
               &test,
               &taken) == -ESTALE);
    assert(test.take_calls == 0u);
}

static void test_failed_take_leaves_ram_owner_recoverable(void)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event taken = {0};

    test.take_ret = false;
    assert(app_anchor_command_completion_take_terminal_exact(
               test.terminal.handle,
               &test.terminal,
               take_terminal,
               &test,
               &taken) == -EAGAIN);
    assert(test.take_calls == 1u);
}

static void expect_consumed_metadata_mismatch(
    const struct node_comm_terminal_event *taken_terminal)
{
    struct test_context test =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event peeked = test.terminal;
    struct node_comm_terminal_event taken = {0};

    test.terminal = *taken_terminal;
    assert(app_anchor_command_completion_take_terminal_exact(
               test.terminal.handle,
               &peeked,
               take_terminal,
               &test,
               &taken) == -EPROTO);
    assert(test.take_calls == 1u);
}

static void test_consumed_metadata_mismatch_requires_new_handle(void)
{
    struct test_context canonical =
        make_context(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event changed = canonical.terminal;

    changed.client_token++;
    expect_consumed_metadata_mismatch(&changed);
    changed = canonical.terminal;
    changed.delivery_generation++;
    expect_consumed_metadata_mismatch(&changed);
    changed = canonical.terminal;
    changed.proof = NODE_COMM_TERMINAL_PROOF_TRANSPORT;
    expect_consumed_metadata_mismatch(&changed);
}

int main(void)
{
    test_result_sequence_wrap_skips_reserved_zero();
    test_delivered_exact_take_succeeds();
    test_failure_terminal_requests_exact_resubmit();
    test_stale_terminal_does_not_mutate_owners();
    test_failed_take_leaves_ram_owner_recoverable();
    test_consumed_metadata_mismatch_requires_new_handle();
    return 0;
}
