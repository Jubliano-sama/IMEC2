#include "watchdog_adoption.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static uint32_t available_mask(uint8_t available_reload_requests)
{
    if (available_reload_requests >= 32u) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << available_reload_requests) - UINT32_C(1);
}

static uint8_t count_requests(uint32_t mask)
{
    uint8_t count = 0u;

    while (mask != 0u) {
        count = (uint8_t)(count + (uint8_t)(mask & UINT32_C(1)));
        mask >>= 1u;
    }
    return count;
}

int watchdog_adoption_plan(bool hardware_running,
                           uint32_t enabled_reload_request_mask,
                           uint8_t available_reload_requests,
                           struct watchdog_adoption_plan *plan)
{
    uint32_t valid_mask;

    if (plan == NULL || available_reload_requests == 0u ||
        available_reload_requests > WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS) {
        return -EINVAL;
    }

    memset(plan, 0, sizeof(*plan));
    plan->mode = WATCHDOG_ADOPTION_INVALID;
    if (!hardware_running) {
        plan->mode = WATCHDOG_ADOPTION_FRESH;
        return 0;
    }

    valid_mask = available_mask(available_reload_requests);
    if ((enabled_reload_request_mask & ~valid_mask) != 0u) {
        return -ERANGE;
    }
    if (enabled_reload_request_mask == 0u) {
        return -EIO;
    }

    plan->mode = WATCHDOG_ADOPTION_INHERITED;
    plan->reload_request_mask = enabled_reload_request_mask;
    plan->reload_request_count = count_requests(enabled_reload_request_mask);
    return 0;
}

uint8_t watchdog_adoption_feed_mask(uint32_t reload_request_mask,
                                    uint8_t available_reload_requests,
                                    bool feeding_allowed,
                                    watchdog_adoption_feed_fn feed,
                                    void *ctx)
{
    uint8_t fed = 0u;
    uint8_t index;

    if (!feeding_allowed || feed == NULL || available_reload_requests == 0u ||
        available_reload_requests > WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS) {
        return 0u;
    }

    reload_request_mask &= available_mask(available_reload_requests);
    for (index = 0u; index < available_reload_requests; index++) {
        if ((reload_request_mask & (UINT32_C(1) << index)) == 0u) {
            continue;
        }
        feed(index, ctx);
        fed++;
    }
    return fed;
}
