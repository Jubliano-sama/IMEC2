#include "app_anchor_command_completion.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct terminal_store {
    struct node_comm_terminal_event terminal;
    uint32_t requested_handle;
    size_t take_calls;
    unsigned int transient_failures;
    bool available;
};

static bool take_terminal(uint32_t delivery_handle,
                          struct node_comm_terminal_event *event_out,
                          void *context)
{
    struct terminal_store *store = context;

    assert(store != NULL);
    assert(event_out != NULL);
    store->requested_handle = delivery_handle;
    store->take_calls++;
    if (store->transient_failures != 0u) {
        store->transient_failures--;
        return false;
    }
    if (!store->available) {
        return false;
    }
    *event_out = store->terminal;
    store->available = false;
    return true;
}

static struct node_comm_terminal_event terminal_event(
    enum node_comm_terminal_reason reason)
{
    const struct node_comm_terminal_event event = {
        .handle = 0x10203040u,
        .delivery_generation = 0x50607080u,
        .client_token = 0x90a0b0c0u,
        .terminal_at_ms = UINT64_C(0x123456789abcdef0),
        .reason = reason,
        .attempts_started = 7u,
        .proof = reason == NODE_COMM_TERMINAL_DELIVERED ?
            NODE_COMM_TERMINAL_PROOF_SEMANTIC :
            NODE_COMM_TERMINAL_PROOF_NONE,
    };

    return event;
}

static struct terminal_store store_for(
    const struct node_comm_terminal_event *event)
{
    const struct terminal_store store = {
        .terminal = *event,
        .available = true,
    };

    return store;
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

static void test_exact_delivered_terminal_is_consumed_once(void)
{
    const struct node_comm_terminal_event peeked =
        terminal_event(NODE_COMM_TERMINAL_DELIVERED);
    struct terminal_store store = store_for(&peeked);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_take_terminal_exact(
               peeked.handle,
               &peeked,
               take_terminal,
               &store,
               &taken) == 1);
    assert(store.take_calls == 1u);
    assert(store.requested_handle == peeked.handle);
    assert(!store.available);
    assert(terminal_equal(&taken, &peeked));

    assert(app_anchor_command_completion_take_terminal_exact(
               peeked.handle,
               &peeked,
               take_terminal,
               &store,
               &taken) == -EAGAIN);
    assert(store.take_calls == 2u);
}

static void test_failed_delivery_requests_same_owner_resubmit(void)
{
    const struct node_comm_terminal_event peeked =
        terminal_event(NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    struct terminal_store store = store_for(&peeked);
    struct node_comm_terminal_event taken = {0};

    assert(app_anchor_command_completion_take_terminal_exact(
               peeked.handle,
               &peeked,
               take_terminal,
               &store,
               &taken) == 0);
    assert(store.take_calls == 1u);
    assert(!store.available);
    assert(terminal_equal(&taken, &peeked));
}

static void test_transient_take_failure_leaves_terminal_retryable(void)
{
    const struct node_comm_terminal_event peeked =
        terminal_event(NODE_COMM_TERMINAL_DELIVERED);
    struct terminal_store store = store_for(&peeked);
    struct node_comm_terminal_event taken = {0};

    store.transient_failures = 1u;
    assert(app_anchor_command_completion_take_terminal_exact(
               peeked.handle,
               &peeked,
               take_terminal,
               &store,
               &taken) == -EAGAIN);
    assert(store.take_calls == 1u);
    assert(store.available);

    assert(app_anchor_command_completion_take_terminal_exact(
               peeked.handle,
               &peeked,
               take_terminal,
               &store,
               &taken) == 1);
    assert(store.take_calls == 2u);
    assert(!store.available);
    assert(terminal_equal(&taken, &peeked));
}

static void test_stale_peek_cannot_consume_successor(void)
{
    const struct node_comm_terminal_event current =
        terminal_event(NODE_COMM_TERMINAL_DELIVERED);
    struct node_comm_terminal_event stale = current;
    struct terminal_store store = store_for(&current);
    struct node_comm_terminal_event taken = {0};

    stale.handle++;
    assert(app_anchor_command_completion_take_terminal_exact(
               current.handle,
               &stale,
               take_terminal,
               &store,
               &taken) == -ESTALE);
    assert(store.take_calls == 0u);
    assert(store.available);
}

typedef void (*terminal_mutator_fn)(struct node_comm_terminal_event *event);

static void mutate_handle(struct node_comm_terminal_event *event)
{
    event->handle++;
}

static void mutate_delivery_generation(struct node_comm_terminal_event *event)
{
    event->delivery_generation++;
}

static void mutate_client_token(struct node_comm_terminal_event *event)
{
    event->client_token++;
}

static void mutate_terminal_time(struct node_comm_terminal_event *event)
{
    event->terminal_at_ms++;
}

static void mutate_reason(struct node_comm_terminal_event *event)
{
    event->reason = NODE_COMM_TERMINAL_PERMANENT_FAILURE;
}

static void mutate_attempts(struct node_comm_terminal_event *event)
{
    event->attempts_started++;
}

static void mutate_proof(struct node_comm_terminal_event *event)
{
    event->proof = NODE_COMM_TERMINAL_PROOF_TRANSPORT;
}

static void test_every_terminal_identity_field_is_exact(void)
{
    static const terminal_mutator_fn mutators[] = {
        mutate_handle,
        mutate_delivery_generation,
        mutate_client_token,
        mutate_terminal_time,
        mutate_reason,
        mutate_attempts,
        mutate_proof,
    };
    const struct node_comm_terminal_event peeked =
        terminal_event(NODE_COMM_TERMINAL_DELIVERED);

    for (size_t index = 0u; index < sizeof(mutators) / sizeof(mutators[0]);
         ++index) {
        struct terminal_store store = store_for(&peeked);
        struct node_comm_terminal_event taken = {0};

        mutators[index](&store.terminal);
        assert(app_anchor_command_completion_take_terminal_exact(
                   peeked.handle,
                   &peeked,
                   take_terminal,
                   &store,
                   &taken) == -EPROTO);
        assert(store.take_calls == 1u);
        assert(!store.available);
    }
}

int main(void)
{
    test_exact_delivered_terminal_is_consumed_once();
    test_failed_delivery_requests_same_owner_resubmit();
    test_transient_take_failure_leaves_terminal_retryable();
    test_stale_peek_cannot_consume_successor();
    test_every_terminal_identity_field_is_exact();
    return 0;
}
