#include "anchor_range_fragment_policy.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static void test_prewrite_contention_retries_same_owned_fragment(void)
{
    struct anchor_range_fragment_policy policy;

    assert(anchor_range_fragment_policy_begin(&policy, 1100) == 0);
    assert(policy.fragment_owned);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE,
               1000) == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY);
    assert(policy.fragment_owned);
    assert(!policy.fail_closed);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED,
               1050) == ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE);
    assert(!policy.fragment_owned);
    assert(!policy.fail_closed);
}

static void test_contention_deadline_retains_fail_closed_owner(void)
{
    struct anchor_range_fragment_policy policy;

    assert(anchor_range_fragment_policy_begin(&policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE,
               1100) == ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED);
    assert(policy.fragment_owned);
    assert(policy.fail_closed);
}

static void test_exact_commit_retries_only_inside_shared_deadline(void)
{
    struct anchor_range_fragment_policy policy;

    assert(anchor_range_fragment_policy_begin(&policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT,
               1000) == ANCHOR_RANGE_FRAGMENT_POLICY_RETRY);
    assert(policy.fragment_owned);
    assert(!policy.fail_closed);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED,
               1050) == ANCHOR_RANGE_FRAGMENT_POLICY_COMPLETE);
    assert(!policy.fragment_owned);
    assert(!policy.fail_closed);

    assert(anchor_range_fragment_policy_begin(&policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_EXACT_COMMIT,
               1100) == ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED);
    assert(policy.fragment_owned);
    assert(policy.fail_closed);
}

static void test_ambiguous_write_or_readback_never_retries(void)
{
    struct anchor_range_fragment_policy write_policy;
    struct anchor_range_fragment_policy readback_policy;

    assert(anchor_range_fragment_policy_begin(&write_policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &write_policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS,
               1000) == ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED);
    assert(write_policy.fragment_owned);
    assert(write_policy.fail_closed);

    assert(anchor_range_fragment_policy_begin(&readback_policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &readback_policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS,
               1000) == ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED);
    assert(readback_policy.fragment_owned);
    assert(readback_policy.fail_closed);
}

static void test_rejected_fragment_retains_fail_closed_owner(void)
{
    struct anchor_range_fragment_policy policy;

    assert(anchor_range_fragment_policy_begin(&policy, 1100) == 0);
    assert(anchor_range_fragment_policy_observe(
               &policy,
               ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED,
               1000) == ANCHOR_RANGE_FRAGMENT_POLICY_FAIL_CLOSED);
    assert(policy.fragment_owned);
    assert(policy.fail_closed);
}

int main(void)
{
    struct anchor_range_fragment_policy policy;

    assert(anchor_range_fragment_policy_begin(NULL, 1) == -EINVAL);
    assert(anchor_range_fragment_policy_begin(&policy, 0) == -EINVAL);
    test_prewrite_contention_retries_same_owned_fragment();
    test_contention_deadline_retains_fail_closed_owner();
    test_exact_commit_retries_only_inside_shared_deadline();
    test_ambiguous_write_or_readback_never_retries();
    test_rejected_fragment_retains_fail_closed_owner();
    return 0;
}
