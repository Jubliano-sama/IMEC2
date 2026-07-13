#include "app_gateway_command_lifecycle.h"

#include <errno.h>
#include <string.h>

static bool identities_equal(const struct app_gateway_command_identity *left,
                             const struct app_gateway_command_identity *right)
{
    return left->msg_type == right->msg_type &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->admission_id == right->admission_id &&
           left->command_id == right->command_id;
}

static struct app_gateway_command_lifecycle_slot *find_slot(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_identity *identity)
{
    if (lifecycle == NULL || identity == NULL) {
        return NULL;
    }

    for (size_t i = 0u; i < lifecycle->capacity; i++) {
        struct app_gateway_command_lifecycle_slot *slot = &lifecycle->slots[i];

        if (slot->state != APP_GATEWAY_COMMAND_LIFECYCLE_EMPTY &&
            identities_equal(&slot->identity, identity)) {
            return slot;
        }
    }
    return NULL;
}

static int identity_for_item(const struct app_gateway_command_ingress_item *item,
                             struct app_gateway_command_identity *identity)
{
    return app_gateway_command_identity_from_item(item, identity);
}

int app_gateway_command_lifecycle_init(
    struct app_gateway_command_lifecycle *lifecycle,
    size_t queue_capacity)
{
    if (lifecycle == NULL || queue_capacity == 0u ||
        queue_capacity > APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS) {
        return -EINVAL;
    }

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->capacity = queue_capacity;
    return 0;
}

int app_gateway_command_lifecycle_admit(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item)
{
    struct app_gateway_command_identity identity;
    int ret;

    if (lifecycle == NULL || lifecycle->capacity == 0u) {
        return -EINVAL;
    }
    ret = identity_for_item(item, &identity);
    if (ret < 0) {
        return ret;
    }
    if (find_slot(lifecycle, &identity) != NULL) {
        return -EALREADY;
    }

    for (size_t i = 0u; i < lifecycle->capacity; i++) {
        if (lifecycle->slots[i].state == APP_GATEWAY_COMMAND_LIFECYCLE_EMPTY) {
            lifecycle->slots[i].identity = identity;
            lifecycle->slots[i].state = APP_GATEWAY_COMMAND_LIFECYCLE_QUEUED;
            return 0;
        }
    }
    return -ENOSPC;
}

int app_gateway_command_lifecycle_cancel(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_identity *identity,
    app_gateway_command_lifecycle_remove_fn remove_physical,
    void *remove_ctx,
    struct app_gateway_command_lifecycle_cancel_result *result)
{
    struct app_gateway_command_lifecycle_slot *slot;
    int physical_remove_ret = 0;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    slot = find_slot(lifecycle, identity);
    if (slot == NULL) {
        return -ENOENT;
    }

    /* The dispatcher reads this state before any queued item can execute. */
    slot->state = APP_GATEWAY_COMMAND_LIFECYCLE_CANCELLED;
    if (remove_physical != NULL) {
        physical_remove_ret = remove_physical(remove_ctx, identity);
    }
    if (result != NULL) {
        result->authoritative_cancelled = true;
        result->physical_remove_ret = physical_remove_ret;
    }
    return 0;
}

int app_gateway_command_lifecycle_begin_dispatch(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item,
    enum app_gateway_command_lifecycle_dispatch *dispatch)
{
    struct app_gateway_command_identity identity;
    struct app_gateway_command_lifecycle_slot *slot;
    int ret;

    if (dispatch == NULL) {
        return -EINVAL;
    }
    ret = identity_for_item(item, &identity);
    if (ret < 0) {
        return ret;
    }
    slot = find_slot(lifecycle, &identity);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == APP_GATEWAY_COMMAND_LIFECYCLE_CANCELLED) {
        memset(slot, 0, sizeof(*slot));
        *dispatch = APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED;
        return 0;
    }
    if (slot->state != APP_GATEWAY_COMMAND_LIFECYCLE_QUEUED) {
        return -EBUSY;
    }

    slot->state = APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCHING;
    *dispatch = APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE;
    return 0;
}

int app_gateway_command_lifecycle_requeue_retry(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item)
{
    struct app_gateway_command_identity identity;
    struct app_gateway_command_lifecycle_slot *slot;
    int ret;

    ret = identity_for_item(item, &identity);
    if (ret < 0) {
        return ret;
    }
    slot = find_slot(lifecycle, &identity);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == APP_GATEWAY_COMMAND_LIFECYCLE_CANCELLED) {
        return -ECANCELED;
    }
    if (slot->state != APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCHING) {
        return -EBUSY;
    }

    slot->state = APP_GATEWAY_COMMAND_LIFECYCLE_QUEUED;
    return 0;
}

int app_gateway_command_lifecycle_finish(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item)
{
    return app_gateway_command_lifecycle_discard(lifecycle, item);
}

int app_gateway_command_lifecycle_discard(
    struct app_gateway_command_lifecycle *lifecycle,
    const struct app_gateway_command_ingress_item *item)
{
    struct app_gateway_command_identity identity;
    struct app_gateway_command_lifecycle_slot *slot;
    int ret;

    ret = identity_for_item(item, &identity);
    if (ret < 0) {
        return ret;
    }
    slot = find_slot(lifecycle, &identity);
    if (slot == NULL) {
        return -ENOENT;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}
