#ifndef STACK_DIAG_TRANSPORT_H
#define STACK_DIAG_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACK_DIAG_TRANSPORT_RETRY_WINDOW_MS 100u
#define STACK_DIAG_TRANSPORT_ATTACH_PROBE_MS 1u

struct stack_diag_transport_ops {
    uint32_t (*now_ms)(void *context);
    unsigned (*available)(void *context);
    unsigned (*write)(void *context, const char *data, size_t length);
    void (*wait_ms)(void *context, uint32_t delay_ms);
    void *context;
};

struct stack_diag_transport {
    uint32_t deadline_ms;
    unsigned last_available;
    bool have_available_observation;
    bool host_confirmed;
    bool transaction_active;
};

int stack_diag_transport_begin(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops);
int stack_diag_transport_write(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops,
    const char *record);
void stack_diag_transport_end(
    struct stack_diag_transport *transport,
    const struct stack_diag_transport_ops *ops);

#endif
