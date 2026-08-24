#include "app_operation_policy.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static size_t assignment_policy_payload(uint8_t *payload,
                                        size_t payload_cap,
                                        bool ram_only)
{
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
    };
    size_t payload_len = 0u;

    operation_policy_assignment_defaults(&policy.value.assignment);
    policy.value.assignment.ram_only_iteration = ram_only;
    assert(operation_policy_append_tlv(payload, payload_cap, &payload_len,
                                       &policy) == PROTO_OK);
    return payload_len;
}

static void test_prepare_and_commit_are_transactional(void)
{
    struct operation_policy_set before;
    struct operation_policy_set after;
    struct app_operation_policy_candidate candidate;
    uint8_t payload[OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    size_t payload_len = assignment_policy_payload(payload, sizeof(payload),
                                                   true);

    app_operation_policy_reset_defaults();
    app_operation_policy_snapshot(&before);
    assert(app_operation_policy_prepare_payload(
               payload, payload_len,
               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
               &candidate) == 0);
    assert(candidate.updates.assignment_present);
    assert(candidate.updates.assignment.ram_only_iteration);
    assert(candidate.resolved.assignment.ram_only_iteration);

    app_operation_policy_snapshot(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);

    app_operation_policy_commit_prepared(&candidate);
    app_operation_policy_snapshot(&after);
    assert(after.assignment.ram_only_iteration);
    assert(after.assignment.operation_budget_ms ==
           before.assignment.operation_budget_ms);
}

static void test_rejected_updates_leave_active_policy_unchanged(void)
{
    struct operation_policy_set before;
    struct operation_policy_set after;
    struct app_operation_policy_candidate candidate;
    struct operation_policy_set invalid = {0};
    uint8_t malformed[] = {TLV_OPERATION_POLICY, 1u, 0xffu};

    app_operation_policy_reset_defaults();
    app_operation_policy_snapshot(&before);
    assert(app_operation_policy_prepare_payload(
               malformed, sizeof(malformed), 0u,
               APP_OPERATION_POLICY_ALL_MASK, &candidate) == -EBADMSG);
    assert(app_operation_policy_prepare_payload(
               NULL, 0u, APP_OPERATION_POLICY_ASSIGNMENT_MASK,
               APP_OPERATION_POLICY_ASSIGNMENT_MASK, &candidate) ==
           -EBADMSG);

    operation_policy_assignment_defaults(&invalid.assignment);
    invalid.assignment.operation_budget_ms = 1000u;
    invalid.assignment_present = true;
    assert(app_operation_policy_install(&invalid, 0u) == -EBADMSG);

    app_operation_policy_snapshot(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_scope_masks_fail_closed(void)
{
    struct app_operation_policy_candidate candidate;
    uint8_t payload[OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    size_t payload_len = assignment_policy_payload(payload, sizeof(payload),
                                                   false);

    assert(app_operation_policy_prepare_payload(
               payload, payload_len,
               APP_OPERATION_POLICY_ASSIGNMENT_MASK, 0u,
               &candidate) == -EINVAL);
    assert(app_operation_policy_prepare_payload(
               payload, payload_len,
               APP_OPERATION_POLICY_ASSIGNMENT_MASK,
               (uint8_t)(APP_OPERATION_POLICY_ALL_MASK | 0x80u),
               &candidate) == -EINVAL);
    assert(app_operation_policy_prepare_payload(
               payload, payload_len, 0u, 0u, &candidate) == -EBADMSG);
}

int main(void)
{
    test_prepare_and_commit_are_transactional();
    test_rejected_updates_leave_active_policy_unchanged();
    test_scope_masks_fail_closed();
    return 0;
}
