#include "operation_policy.h"
#include "discovery_assignment.h"

#include <assert.h>
#include <string.h>

_Static_assert(OPERATION_POLICY_ASSIGNMENT_TLV_LEN == 13u,
               "assignment policy wire size changed");
_Static_assert(OPERATION_POLICY_DISCOVERY_TLV_LEN == 21u,
               "discovery policy wire size changed");
_Static_assert(OPERATION_POLICY_PAIR_TLV_LEN == 7u,
               "pair policy wire size changed");
_Static_assert(OPERATION_POLICY_ALL_TLVS_LEN == 41u,
               "complete policy wire size changed");
_Static_assert(OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN == 934u,
               "maximum assignment TABLE size changed");
_Static_assert(OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN <=
                   PACKET_EXT_MAX_PAYLOAD_LEN,
               "assignment policy must fit the extended mesh packet");

static struct operation_policy assignment_policy(void)
{
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
    };

    operation_policy_assignment_defaults(&policy.value.assignment);
    return policy;
}

static struct operation_policy discovery_policy(void)
{
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
    };

    operation_policy_discovery_defaults(&policy.value.discovery);
    return policy;
}

static struct operation_policy pair_policy(void)
{
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
    };

    operation_policy_pair_defaults(&policy.value.pair);
    return policy;
}

static void test_defaults_are_valid_and_preserve_robust_baseline(void)
{
    struct operation_policy_set set;
    struct operation_policy assignment = assignment_policy();
    struct operation_policy discovery = discovery_policy();
    struct operation_policy pair = pair_policy();

    assert(operation_policy_validate(&assignment) == PROTO_OK);
    assert(operation_policy_validate(&discovery) == PROTO_OK);
    assert(operation_policy_validate(&pair) == PROTO_OK);
    assert(assignment.value.assignment.expected_anchor_count == 0u);
    assert(assignment.value.assignment.operation_budget_ms == 1591204u);
    assert(assignment.value.assignment.response_spread_ms == 1000u);
    assert(discovery.value.discovery.start_delay_ms == 20000u);
    assert(discovery.value.discovery.slot_ms == 40u);
    assert(discovery.value.discovery.slot_count == 6u);
    assert(discovery.value.discovery.round_count == 4u);
    assert(discovery.value.discovery.report_grace_ms == 250u);
    assert(discovery.value.discovery.operation_budget_ms == 240000u);
    assert(pair.value.pair.max_reruns == 2u);
    assert(pair.value.pair.max_parallel_pairs == 25u);

    memset(&set, 0xa5, sizeof(set));
    assert(operation_policy_set_from_tlvs(NULL, 0u, &set) == PROTO_OK);
    assert(!set.assignment_present);
    assert(!set.discovery_present);
    assert(!set.pair_present);
    assert(set.assignment.response_spread_ms == 1000u);
    assert(set.discovery.round_count == 4u);
    assert(set.pair.max_parallel_pairs == 25u);
}

static void test_all_families_round_trip_in_one_payload(void)
{
    struct operation_policy assignment = assignment_policy();
    struct operation_policy discovery = discovery_policy();
    struct operation_policy pair = pair_policy();
    struct operation_policy_set decoded;
    uint8_t payload[64] = {0};
    size_t payload_len = 0u;

    assignment.value.assignment.expected_anchor_count = 5u;
    assignment.value.assignment.operation_budget_ms = 1600000u;
    assignment.value.assignment.response_spread_ms = 400u;
    discovery.value.discovery.start_delay_ms = 20000u;
    discovery.value.discovery.slot_ms = 75u;
    discovery.value.discovery.slot_count = 10u;
    discovery.value.discovery.round_count = 2u;
    discovery.value.discovery.report_grace_ms = 1200u;
    discovery.value.discovery.operation_budget_ms = 300000u;
    pair.value.pair.max_reruns = 1u;
    pair.value.pair.max_parallel_pairs = 8u;

    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_ID,
                          CMD_SURVEY_REACHABILITY) == PROTO_OK);
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &assignment) == PROTO_OK);
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &discovery) == PROTO_OK);
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &pair) == PROTO_OK);
    assert(payload_len == PROTO_TLV_U16_ENCODED_LEN +
                          OPERATION_POLICY_ALL_TLVS_LEN);

    assert(operation_policy_set_from_tlvs(payload, payload_len, &decoded) ==
           PROTO_OK);
    assert(decoded.assignment_present);
    assert(decoded.discovery_present);
    assert(decoded.pair_present);
    assert(decoded.assignment.expected_anchor_count == 5u);
    assert(decoded.assignment.operation_budget_ms == 1600000u);
    assert(decoded.assignment.response_spread_ms == 400u);
    assert(decoded.discovery.start_delay_ms == 20000u);
    assert(decoded.discovery.slot_ms == 75u);
    assert(decoded.discovery.slot_count == 10u);
    assert(decoded.discovery.round_count == 2u);
    assert(decoded.discovery.report_grace_ms == 1200u);
    assert(decoded.discovery.operation_budget_ms == 300000u);
    assert(decoded.pair.max_reruns == 1u);
    assert(decoded.pair.max_parallel_pairs == 8u);
}

static void test_assignment_is_equal_response_policy_without_hop_field(void)
{
    struct operation_policy original = assignment_policy();
    struct operation_policy decoded;
    uint8_t payload[OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;

    original.value.assignment.expected_anchor_count = 12u;
    original.value.assignment.operation_budget_ms = 1600000u;
    original.value.assignment.response_spread_ms = 750u;
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &original) == PROTO_OK);
    assert(payload_len == OPERATION_POLICY_ASSIGNMENT_TLV_LEN);
    assert(tlv_find(payload, payload_len, TLV_OPERATION_POLICY,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == OPERATION_POLICY_ASSIGNMENT_VALUE_LEN);
    assert(value[0] == OPERATION_POLICY_VERSION);
    assert(value[1] == OPERATION_POLICY_FAMILY_ASSIGNMENT);
    assert(value[2] == OPERATION_POLICY_FLAGS_NONE);
    assert(proto_get_u16_le(&value[3]) == 12u);
    assert(proto_get_u32_le(&value[5]) == 1600000u);
    assert(proto_get_u16_le(&value[9]) == 750u);
    assert(operation_policy_decode_value(value, value_len, &decoded) ==
           PROTO_OK);
    assert(decoded.value.assignment.response_spread_ms == 750u);
}

static void test_duplicate_family_is_rejected_but_distinct_families_repeat_type(void)
{
    struct operation_policy assignment = assignment_policy();
    struct operation_policy discovery = discovery_policy();
    struct operation_policy pair = pair_policy();
    struct operation_policy_set decoded;
    uint8_t payload[64] = {0};
    uint8_t duplicate[64] = {0};
    size_t payload_len = 0u;
    size_t duplicate_len;

    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &assignment) == PROTO_OK);
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &discovery) == PROTO_OK);
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &assignment) == PROTO_ERR_MALFORMED);

    memcpy(duplicate, payload, payload_len);
    duplicate_len = payload_len;
    memcpy(&duplicate[duplicate_len], payload,
           OPERATION_POLICY_ASSIGNMENT_TLV_LEN);
    duplicate_len += OPERATION_POLICY_ASSIGNMENT_TLV_LEN;
    assert(operation_policy_set_from_tlvs(duplicate, duplicate_len,
                                          &decoded) ==
           PROTO_ERR_MALFORMED);
    assert(operation_policy_append_tlv(duplicate, sizeof(duplicate),
                                       &duplicate_len, &pair) ==
           PROTO_ERR_MALFORMED);
}

static void test_decode_rejects_noncanonical_headers_and_lengths(void)
{
    struct operation_policy policy = assignment_policy();
    struct operation_policy decoded;
    uint8_t payload[OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    uint8_t value[OPERATION_POLICY_ASSIGNMENT_VALUE_LEN];
    struct operation_policy unchanged;
    size_t payload_len = 0u;

    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &policy) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_OPERATION_POLICY,
                    &raw, &raw_len) == PROTO_OK);
    memcpy(value, raw, raw_len);

    memset(&decoded, 0xa5, sizeof(decoded));
    unchanged = decoded;
    value[0]++;
    assert(operation_policy_decode_value(value, raw_len, &decoded) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
    memcpy(value, raw, raw_len);
    value[1] = 0xffu;
    assert(operation_policy_decode_value(value, raw_len, &decoded) ==
           PROTO_ERR_MALFORMED);
    memcpy(value, raw, raw_len);
    value[2] = 1u;
    assert(operation_policy_decode_value(value, raw_len, &decoded) ==
           PROTO_ERR_MALFORMED);
    assert(operation_policy_decode_value(
               raw, (uint8_t)(raw_len - 1u), &decoded) ==
           PROTO_ERR_MALFORMED);
    assert(operation_policy_decode_value(NULL, raw_len, &decoded) ==
           PROTO_ERR_ARG);
}

static void test_assignment_bounds(void)
{
    struct operation_policy policy = assignment_policy();
    uint32_t required_budget_ms = 0u;

    policy.value.assignment.expected_anchor_count =
        OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = assignment_policy();
    policy.value.assignment.operation_budget_ms =
        OPERATION_POLICY_COMMAND_BUDGET_MIN_MS - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy.value.assignment.operation_budget_ms =
        OPERATION_POLICY_COMMAND_BUDGET_MAX_MS + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = assignment_policy();
    policy.value.assignment.response_spread_ms =
        OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy.value.assignment.response_spread_ms =
        OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);

    policy = assignment_policy();
    policy.value.assignment.response_spread_ms =
        OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS;
    assert(operation_policy_assignment_required_budget_ms(
               &policy.value.assignment, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms ==
           DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS);
    policy.value.assignment.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);
    policy.value.assignment.operation_budget_ms = required_budget_ms - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);

    policy = assignment_policy();
    assert(operation_policy_assignment_required_budget_ms(
               &policy.value.assignment, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms ==
           OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS);

    policy.value.assignment.expected_anchor_count = 3u;
    assert(operation_policy_assignment_required_budget_ms(
               &policy.value.assignment, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms == 526204u);
    policy.value.assignment.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);
    policy.value.assignment.operation_budget_ms = required_budget_ms - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);

    policy = assignment_policy();
    policy.value.assignment.response_spread_ms =
        OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS;
    assert(operation_policy_assignment_required_budget_ms(
               &policy.value.assignment, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms == 1735204u);
    assert(required_budget_ms <= OPERATION_POLICY_COMMAND_BUDGET_MAX_MS);
}

static void test_discovery_bounds_include_configurable_rounds(void)
{
    struct operation_policy policy = discovery_policy();
    struct operation_policy_discovery_budget_terms terms = {
        .report_slot_ms = 2270u,
        .report_custody_ms = 17000u,
        .report_delivery_tail_ms = 63060u,
        .terminal_scheduling_guard_ms = 102u,
    };
    uint32_t required_budget_ms = 0u;

    policy.value.discovery.start_delay_ms =
        OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = discovery_policy();
    policy.value.discovery.slot_ms =
        OPERATION_POLICY_DISCOVERY_SLOT_MIN_MS - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = discovery_policy();
    policy.value.discovery.slot_count = 0u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = discovery_policy();
    policy.value.discovery.round_count = 0u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy.value.discovery.round_count =
        OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MAX + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = discovery_policy();
    policy.value.discovery.report_grace_ms = 0u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = discovery_policy();
    policy.value.discovery.operation_budget_ms =
        OPERATION_POLICY_COMMAND_BUDGET_MAX_MS + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);

    policy = discovery_policy();
    assert(operation_policy_discovery_required_budget_ms(
               &policy.value.discovery, &terms, &required_budget_ms) ==
           PROTO_OK);
    assert(required_budget_ms == 114993u);
    policy.value.discovery.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);
    policy.value.discovery.operation_budget_ms = required_budget_ms - 1u;
    assert(operation_policy_validate(&policy) == PROTO_OK);

    policy = discovery_policy();
    policy.value.discovery.start_delay_ms =
        OPERATION_POLICY_DISCOVERY_START_DELAY_MAX_MS;
    policy.value.discovery.slot_ms = OPERATION_POLICY_DISCOVERY_SLOT_MAX_MS;
    policy.value.discovery.slot_count =
        OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MAX;
    policy.value.discovery.round_count =
        OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MAX;
    policy.value.discovery.report_grace_ms =
        OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MAX_MS;
    assert(operation_policy_discovery_required_budget_ms(
               &policy.value.discovery, &terms, &required_budget_ms) ==
           PROTO_OK);
    assert(required_budget_ms == 473663u);
    policy.value.discovery.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);

    terms.report_slot_ms = UINT32_MAX;
    terms.report_custody_ms = UINT32_MAX;
    terms.report_delivery_tail_ms = UINT32_MAX;
    assert(operation_policy_discovery_required_budget_ms(
               &policy.value.discovery, &terms, &required_budget_ms) ==
           PROTO_ERR_NO_SPACE);
}

static void test_pair_bounds_never_bypass_conflict_admission(void)
{
    struct operation_policy policy = pair_policy();

    policy.value.pair.max_reruns = OPERATION_POLICY_PAIR_MAX_RERUNS + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy = pair_policy();
    policy.value.pair.max_parallel_pairs = 0u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
    policy.value.pair.max_parallel_pairs =
        OPERATION_POLICY_PAIR_MAX_PARALLEL_PAIRS + 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
}

static void test_malformed_container_and_no_space_fail_closed(void)
{
    struct operation_policy policy = pair_policy();
    struct operation_policy_set set;
    struct operation_policy_set unchanged;
    uint8_t malformed[] = {TLV_COMMAND_ID, 2u, 0x00u};
    uint8_t payload[OPERATION_POLICY_PAIR_TLV_LEN] = {0};
    size_t payload_len = 0u;

    memset(&set, 0xa5, sizeof(set));
    unchanged = set;
    assert(operation_policy_set_from_tlvs(malformed, sizeof(malformed),
                                          &set) == PROTO_ERR_MALFORMED);
    assert(memcmp(&set, &unchanged, sizeof(set)) == 0);
    assert(operation_policy_append_tlv(payload, sizeof(payload) - 1u,
                                       &payload_len, &policy) ==
           PROTO_ERR_NO_SPACE);
    assert(payload_len == 0u);
    assert(operation_policy_append_tlv(NULL, sizeof(payload), &payload_len,
                                       &policy) == PROTO_ERR_ARG);
    assert(operation_policy_validate(NULL) == PROTO_ERR_ARG);
}

int main(void)
{
    test_defaults_are_valid_and_preserve_robust_baseline();
    test_all_families_round_trip_in_one_payload();
    test_assignment_is_equal_response_policy_without_hop_field();
    test_duplicate_family_is_rejected_but_distinct_families_repeat_type();
    test_decode_rejects_noncanonical_headers_and_lengths();
    test_assignment_bounds();
    test_discovery_bounds_include_configurable_rounds();
    test_pair_bounds_never_bypass_conflict_admission();
    test_malformed_container_and_no_space_fail_closed();
    return 0;
}
