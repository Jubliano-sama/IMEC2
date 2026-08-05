#ifndef ANCHOR_RANGE_FRAGMENT_POLICY_H
#define ANCHOR_RANGE_FRAGMENT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum anchor_range_fragment_persistence_observation {
    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED = 0,
    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE,
    /*
     * The final control marker is an exact idempotent commit. Retrying it can
     * either confirm an already-written equal marker or write the same marker;
     * unlike a fragment append, it cannot create a second owner.
     */
    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT,
    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS,
    ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED,
};

enum anchor_range_fragment_policy_action {
    ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE = 0,
    ANCHOR_RANGE_FRAGMENT_POLICY_RETRY,
    ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED,
};

struct anchor_range_fragment_policy {
    int64_t deadline_ms;
    bool fragment_owned;
    bool fail_closed;
};

int anchor_range_fragment_policy_begin(
    struct anchor_range_fragment_policy *policy,
    int64_t deadline_ms);

enum anchor_range_fragment_policy_action
anchor_range_fragment_policy_observe(
    struct anchor_range_fragment_policy *policy,
    enum anchor_range_fragment_persistence_observation observation,
    int64_t now_ms);

#endif
