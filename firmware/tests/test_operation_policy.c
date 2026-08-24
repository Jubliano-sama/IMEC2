#include "operation_policy.h"
#include "discovery_assignment.h"

#include <assert.h>
#include <string.h>

_Static_assert(OPERATION_POLICY_ASSIGNMENT_TLV_LEN == 13u,
               "assignment policy wire size changed");
_Static_assert(OPERATION_POLICY_ALL_TLVS_LEN == 13u,
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

static void test_first_contact_cells_follow_direct_ack_and_relay_cadence(void)
{
    uint32_t offset_ms = 0u;

    assert(OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS == 450u);
    assert(operation_policy_first_contact_cell_ms(1u) == 450u);
    assert(operation_policy_first_contact_cell_ms(2u) == 1090u);
    assert(operation_policy_first_contact_cell_ms(3u) == 1730u);
    assert(operation_policy_first_contact_cell_ms(0u) == 4930u);

    assert(operation_policy_first_contact_offset_ms(
               0u, 3u, 1u, &offset_ms) == PROTO_OK);
    assert(offset_ms == 0u);
    assert(operation_policy_first_contact_offset_ms(
               2u, 3u, 1u, &offset_ms) == PROTO_OK);
    assert(offset_ms == 900u);
    assert(operation_policy_first_contact_offset_ms(
               0u, 3u, 2u, &offset_ms) == PROTO_OK);
    assert(offset_ms == 1350u);
    assert(operation_policy_first_contact_offset_ms(
               2u, 3u, 2u, &offset_ms) == PROTO_OK);
    assert(offset_ms == 3530u);
    assert(operation_policy_first_contact_offset_ms(
               3u, 3u, 1u, &offset_ms) == PROTO_ERR_ARG);
}

static void test_defaults_and_empty_set(void)
{
    struct operation_policy assignment = assignment_policy();
    struct operation_policy_set set;

    assert(operation_policy_validate(&assignment) == PROTO_OK);
    assert(assignment.value.assignment.expected_anchor_count == 0u);
    assert(assignment.value.assignment.operation_budget_ms == 1800000u);
    assert(assignment.value.assignment.response_spread_ms == 1000u);
    assert(!assignment.value.assignment.ram_only_iteration);

    memset(&set, 0xa5, sizeof(set));
    assert(operation_policy_set_from_tlvs(NULL, 0u, &set) == PROTO_OK);
    assert(!set.assignment_present);
    assert(set.assignment.response_spread_ms == 1000u);
    assert(!set.assignment.ram_only_iteration);
}

static void test_assignment_round_trip_and_duplicate_rejection(void)
{
    struct operation_policy original = assignment_policy();
    struct operation_policy decoded;
    struct operation_policy_set set;
    uint8_t payload[2u * OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;

    original.value.assignment.expected_anchor_count = 12u;
    original.value.assignment.operation_budget_ms = 1600000u;
    original.value.assignment.response_spread_ms = 750u;
    original.value.assignment.ram_only_iteration = true;
    assert(operation_policy_append_tlv(payload, sizeof(payload), &payload_len,
                                       &original) == PROTO_OK);
    assert(payload_len == OPERATION_POLICY_ASSIGNMENT_TLV_LEN);
    assert(tlv_find(payload, payload_len, TLV_OPERATION_POLICY,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == OPERATION_POLICY_ASSIGNMENT_VALUE_LEN);
    assert(value[0] == OPERATION_POLICY_VERSION);
    assert(value[1] == OPERATION_POLICY_FAMILY_ASSIGNMENT);
    assert(value[2] == OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION);
    assert(proto_get_u16_le(&value[3]) == 12u);
    assert(proto_get_u32_le(&value[5]) == 1600000u);
    assert(proto_get_u16_le(&value[9]) == 750u);
    assert(operation_policy_decode_value(value, value_len, &decoded) ==
           PROTO_OK);
    assert(decoded.value.assignment.ram_only_iteration);
    assert(operation_policy_set_from_tlvs(payload, payload_len, &set) ==
           PROTO_OK);
    assert(set.assignment_present);
    assert(set.assignment.expected_anchor_count == 12u);

    memcpy(&payload[payload_len], payload, OPERATION_POLICY_ASSIGNMENT_TLV_LEN);
    payload_len += OPERATION_POLICY_ASSIGNMENT_TLV_LEN;
    assert(operation_policy_set_from_tlvs(payload, payload_len, &set) ==
           PROTO_ERR_MALFORMED);
}

static void test_decode_rejects_noncanonical_values(void)
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
    value[2] = 2u;
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
    assert(required_budget_ms == DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS);
    policy.value.assignment.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);
    policy.value.assignment.operation_budget_ms = required_budget_ms - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);

    policy = assignment_policy();
    policy.value.assignment.expected_anchor_count = 3u;
    assert(operation_policy_assignment_required_budget_ms(
               &policy.value.assignment, &required_budget_ms) == PROTO_OK);
    assert(required_budget_ms == 391444u);
    policy.value.assignment.operation_budget_ms = required_budget_ms;
    assert(operation_policy_validate(&policy) == PROTO_OK);
    policy.value.assignment.operation_budget_ms = required_budget_ms - 1u;
    assert(operation_policy_validate(&policy) == PROTO_ERR_MALFORMED);
}

static void test_malformed_container_and_no_space_fail_closed(void)
{
    struct operation_policy policy = assignment_policy();
    struct operation_policy_set set;
    struct operation_policy_set unchanged;
    uint8_t malformed[] = {TLV_COMMAND_ID, 2u, 0x00u};
    uint8_t payload[OPERATION_POLICY_ASSIGNMENT_TLV_LEN] = {0};
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
    test_first_contact_cells_follow_direct_ack_and_relay_cadence();
    test_defaults_and_empty_set();
    test_assignment_round_trip_and_duplicate_rejection();
    test_decode_rejects_noncanonical_values();
    test_assignment_bounds();
    test_malformed_container_and_no_space_fail_closed();
    return 0;
}
