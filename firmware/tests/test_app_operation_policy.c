#include "app_operation_policy.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static size_t pair_policy_payload(uint8_t *payload, size_t payload_cap)
{
    const struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
        .value.pair = {
            .max_reruns = 1u,
            .max_parallel_pairs = 1u,
        },
    };
    size_t payload_len = 0u;

    assert(operation_policy_append_tlv(payload,
                                       payload_cap,
                                       &payload_len,
                                       &policy) == PROTO_OK);
    return payload_len;
}

static size_t append_assignment_policy(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t payload_len)
{
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
    };

    operation_policy_assignment_defaults(&policy.value.assignment);
    policy.value.assignment.ram_only_iteration = true;
    assert(operation_policy_append_tlv(payload,
                                       payload_cap,
                                       &payload_len,
                                       &policy) == PROTO_OK);
    return payload_len;
}

static void test_parse_and_resolve_do_not_mutate_active_policy(void)
{
    struct operation_policy_set before;
    struct operation_policy_set after;
    struct app_operation_policy_candidate candidate;
    uint8_t payload[OPERATION_POLICY_PAIR_TLV_LEN] = {0};
    size_t payload_len = pair_policy_payload(payload, sizeof(payload));

    app_operation_policy_reset_defaults();
    app_operation_policy_snapshot(&before);
    assert(app_operation_policy_prepare_payload(
               payload,
               payload_len,
               APP_OPERATION_POLICY_PAIR_MASK,
               APP_OPERATION_POLICY_PAIR_MASK,
               &candidate) == 0);
    assert(candidate.updates.pair_present);
    assert(candidate.updates.pair.max_parallel_pairs == 1u);
    assert(candidate.resolved.pair.max_parallel_pairs == 1u);

    app_operation_policy_snapshot(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);

    app_operation_policy_commit_prepared(&candidate);
    app_operation_policy_snapshot(&after);
    assert(after.pair.max_parallel_pairs == 1u);
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

static void test_scoped_prepare_rejects_cross_operation_policy(void)
{
    struct operation_policy_set before;
    struct operation_policy_set after;
    struct app_operation_policy_candidate candidate;
    uint8_t payload[
        OPERATION_POLICY_PAIR_TLV_LEN +
        OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    size_t payload_len = pair_policy_payload(payload, sizeof(payload));

    payload_len = append_assignment_policy(payload,
                                           sizeof(payload),
                                           payload_len);
    app_operation_policy_reset_defaults();
    app_operation_policy_snapshot(&before);

    assert(app_operation_policy_prepare_payload(
               payload,
               payload_len,
               APP_OPERATION_POLICY_PAIR_MASK,
               APP_OPERATION_POLICY_PAIR_MASK,
               &candidate) == -EBADMSG);
    app_operation_policy_snapshot(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);

    assert(app_operation_policy_prepare_payload(
               payload,
               payload_len,
               APP_OPERATION_POLICY_PAIR_MASK,
               APP_OPERATION_POLICY_PAIR_MASK |
                   APP_OPERATION_POLICY_ASSIGNMENT_MASK,
               &candidate) == 0);
    assert(candidate.updates.assignment_present);
    assert(candidate.updates.assignment.ram_only_iteration);
    assert(candidate.resolved.assignment.ram_only_iteration);
    assert(candidate.updates.pair_present);
    assert(app_operation_policy_prepare_payload(
               payload,
               payload_len,
               APP_OPERATION_POLICY_PAIR_MASK,
               (uint8_t)(APP_OPERATION_POLICY_ALL_MASK | 0x80u),
               &candidate) == -EINVAL);
    assert(app_operation_policy_prepare_payload(
               payload,
               payload_len,
               APP_OPERATION_POLICY_PAIR_MASK,
               APP_OPERATION_POLICY_DISCOVERY_MASK,
               &candidate) == -EINVAL);
}

static void test_survey_prepare_requires_both_policies_and_accepts_floor_start(void)
{
    struct operation_policy discovery = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
    };
    struct operation_policy pair = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
    };
    struct app_operation_policy_candidate candidate;
    uint8_t payload[OPERATION_POLICY_DISCOVERY_TLV_LEN +
                    OPERATION_POLICY_PAIR_TLV_LEN] = {0};
    uint8_t pair_only[OPERATION_POLICY_PAIR_TLV_LEN] = {0};
    size_t payload_len = 0u;
    size_t discovery_len;
    size_t pair_only_len;
    const uint8_t survey_mask = APP_OPERATION_POLICY_DISCOVERY_MASK |
                                APP_OPERATION_POLICY_PAIR_MASK;

    operation_policy_discovery_defaults(&discovery.value.discovery);
    discovery.value.discovery.start_delay_ms =
        OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS;
    operation_policy_pair_defaults(&pair.value.pair);

    assert(operation_policy_append_tlv(payload, sizeof(payload),
                                       &payload_len, &discovery) == PROTO_OK);
    discovery_len = payload_len;
    assert(operation_policy_append_tlv(payload, sizeof(payload),
                                       &payload_len, &pair) == PROTO_OK);
    assert(app_operation_policy_prepare_payload(
               payload, payload_len, survey_mask, survey_mask,
               &candidate) == 0);
    assert(candidate.updates.discovery_present);
    assert(candidate.updates.pair_present);
    assert(candidate.updates.discovery.start_delay_ms == 20000u);

    assert(app_operation_policy_prepare_payload(
               payload, discovery_len, survey_mask, survey_mask,
               &candidate) == -EBADMSG);
    pair_only_len = pair_policy_payload(pair_only, sizeof(pair_only));
    assert(app_operation_policy_prepare_payload(
               pair_only, pair_only_len, survey_mask, survey_mask,
               &candidate) == -EBADMSG);
}

int main(void)
{
    test_parse_and_resolve_do_not_mutate_active_policy();
    test_rejected_updates_leave_active_policy_unchanged();
    test_scoped_prepare_rejects_cross_operation_policy();
    test_survey_prepare_requires_both_policies_and_accepts_floor_start();
    return 0;
}
