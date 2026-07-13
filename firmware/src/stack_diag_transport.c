#include "stack_diag_transport.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool valid_ops(const struct stack_diag_transport_ops *ops)
{
    return ops != NULL && ops->now_ms != NULL && ops->available != NULL &&
           ops->write != NULL && ops->wait_ms != NULL;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static unsigned observe_available(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops)
{
    unsigned available = ops->available(ops->context);

    if (transport->have_available_observation &&
        available > transport->last_available) {
        transport->host_confirmed = true;
    }
    transport->last_available = available;
    transport->have_available_observation = true;
    return available;
}

static void observe_write_result(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops,
    unsigned available_before,
    unsigned written)
{
    unsigned expected_after = available_before > written ?
        available_before - written : 0u;
    unsigned available_after = ops->available(ops->context);

    if (available_after > expected_after) {
        transport->host_confirmed = true;
    }
    transport->last_available = available_after;
    transport->have_available_observation = true;
}

int stack_diag_transport_begin(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops)
{
    if (transport == NULL || !valid_ops(ops)) {
        return -EINVAL;
    }
    if (transport->transaction_active) {
        return -EBUSY;
    }

    (void)observe_available(transport, ops);
    transport->deadline_ms =
        ops->now_ms(ops->context) + STACK_DIAG_TRANSPORT_RETRY_WINDOW_MS;
    transport->transaction_active = true;
    return 0;
}

int stack_diag_transport_write(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops,
    const char *record)
{
    size_t length;

    if (transport == NULL || !valid_ops(ops) || record == NULL ||
        !transport->transaction_active) {
        return -EINVAL;
    }

    length = strlen(record);
    if (length == 0u || length > UINT_MAX) {
        return -EMSGSIZE;
    }

    for (;;) {
        unsigned available_before = observe_available(transport, ops);
        unsigned written = ops->write(ops->context, record, length);

        if (written == length) {
            observe_write_result(transport, ops, available_before, written);
            return 0;
        }
        if (written != 0u) {
            observe_write_result(transport, ops, available_before, written);
            return -EIO;
        }
        if (!transport->host_confirmed) {
            ops->wait_ms(ops->context,
                         STACK_DIAG_TRANSPORT_ATTACH_PROBE_MS);
            (void)observe_available(transport, ops);
            if (!transport->host_confirmed) {
                return -EAGAIN;
            }
            continue;
        }
        if (deadline_reached(ops->now_ms(ops->context),
                             transport->deadline_ms)) {
            transport->host_confirmed = false;
            return -ETIMEDOUT;
        }
        ops->wait_ms(ops->context, 1u);
    }
}

void stack_diag_transport_end(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops)
{
    if (transport == NULL || !valid_ops(ops) ||
        !transport->transaction_active) {
        return;
    }

    (void)observe_available(transport, ops);
    transport->transaction_active = false;
}
