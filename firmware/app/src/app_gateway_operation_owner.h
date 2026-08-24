#ifndef APP_GATEWAY_OPERATION_OWNER_H
#define APP_GATEWAY_OPERATION_OWNER_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum app_gateway_operation_owner_kind {
    APP_GATEWAY_OPERATION_OWNER_NONE = 0,
    APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,
};

struct app_gateway_operation_lease {
    enum app_gateway_operation_owner_kind owner;
    uint32_t generation;
};

/* The caller serializes this compact policy object. */
struct app_gateway_operation_owner {
    struct app_gateway_operation_lease active;
    uint32_t next_generation;
};

static inline bool app_gateway_operation_lease_valid(
    const struct app_gateway_operation_lease *lease)
{
    return lease != NULL &&
           lease->owner != APP_GATEWAY_OPERATION_OWNER_NONE &&
           lease->generation != 0u;
}

static inline int app_gateway_operation_owner_claim(
    struct app_gateway_operation_owner *owner,
    enum app_gateway_operation_owner_kind kind,
    struct app_gateway_operation_lease *lease)
{
    uint32_t generation;

    if (owner == NULL || lease == NULL ||
        kind == APP_GATEWAY_OPERATION_OWNER_NONE) {
        return -EINVAL;
    }
    if (app_gateway_operation_lease_valid(&owner->active)) {
        return -EBUSY;
    }
    generation = owner->next_generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    owner->next_generation = generation;
    owner->active = (struct app_gateway_operation_lease) {
        .owner = kind,
        .generation = generation,
    };
    *lease = owner->active;
    return 0;
}

static inline int app_gateway_operation_owner_release(
    struct app_gateway_operation_owner *owner,
    const struct app_gateway_operation_lease *lease)
{
    if (owner == NULL || !app_gateway_operation_lease_valid(lease)) {
        return -EINVAL;
    }
    if (owner->active.owner != lease->owner ||
        owner->active.generation != lease->generation) {
        return -ESTALE;
    }
    memset(&owner->active, 0, sizeof(owner->active));
    return 0;
}

#endif
