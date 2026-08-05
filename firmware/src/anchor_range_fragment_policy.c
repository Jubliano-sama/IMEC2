#include "anchor_range_fragment_policy.h"

#include <errno.h>
#include <stddef.h>

int anchor_range_fragment_policy_begin(
    struct anchor_range_fragment_policy *policy,
    int64_t deadline_ms)
{
    if (policy == NULL || deadline_ms <= 0) {
        return -EINVAL;
    }

    *policy = (struct anchor_range_fragment_policy) {
        .deadline_ms = deadline_ms,
        .fragment_owned = true,
    };
    return 0;
}

enum anchor_range_fragment_policy_action
anchor_range_fragment_policy_observe(
    struct anchor_range_fragment_policy *policy,
    enum anchor_range_fragment_persistence_observation observation,
    int64_t now_ms)
{
    if (policy == NULL || !policy->fragment_owned || policy->fail_closed) {
        return ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED;
    }

    if (observation == ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED) {
        policy->fragment_owned = false;
        return ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE;
    }

    if ((observation ==
             ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE ||
         observation ==
             ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT) &&
        now_ms < policy->deadline_ms) {
        return ANCHOR_RANGE_FRAGMENT_POLICY_RETRY;
    }

    policy->fail_closed = true;
    return ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED;
}
