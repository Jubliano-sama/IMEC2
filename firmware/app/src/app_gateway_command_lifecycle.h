#ifndef APP_GATEWAY_COMMAND_LIFECYCLE_H
#define APP_GATEWAY_COMMAND_LIFECYCLE_H

#include "app_gateway_command_ingress.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Production admission has two serialized command slots. Keeping unused
 * lifecycle identities here would consume RAM that is required to retain the
 * terminal event for every independently accepted host-command credit.
 */
#define APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS 2u

enum app_gateway_command_lifecycle_state {
    APP_GATEWAY_COMMAND_LIFECYCLE_EMPTY = 0,
    APP_GATEWAY_COMMAND_LIFECYCLE_QUEUED,
    APP_GATEWAY_COMMAND_LIFECYCLE_CANCELLED,
    APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCHING,
};

enum app_gateway_command_lifecycle_dispatch {
    APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE = 0,
    APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED,
};

struct app_gateway_command_lifecycle_slot {
    struct app_gateway_command_identity identity;
    enum app_gateway_command_lifecycle_state state;
};

struct app_gateway_command_lifecycle {
    struct app_gateway_command_lifecycle_slot
        slots[APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS];
    size_t capacity;
};

typedef int (*app_gateway_command_lifecycle_remove_fn)(
    void *ctx,
    const struct app_gateway_command_identity *identity);

struct app_gateway_command_lifecycle_cancel_result {
    bool authoritative_cancelled;
    int physical_remove_ret;
};

int app_gateway_command_lifecycle_init(
    struct app_gateway_command_lifecycle *lifecycle,
    size_t queue_capacity);
int app_gateway_command_lifecycle_admit(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item);
int app_gateway_command_lifecycle_cancel(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_identity *identity,
    app_gateway_command_lifecycle_remove_fn remove_physical,
    void *remove_ctx,
    struct app_gateway_command_lifecycle_cancel_result *result);
int app_gateway_command_lifecycle_begin_dispatch(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item,
    enum app_gateway_command_lifecycle_dispatch *dispatch);
int app_gateway_command_lifecycle_requeue_retry(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item);
int app_gateway_command_lifecycle_finish(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item);
int app_gateway_command_lifecycle_discard(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item);

#endif
