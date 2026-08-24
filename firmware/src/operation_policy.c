#include "operation_policy.h"

#include "discovery_assignment.h"

#include <string.h>

void operation_policy_assignment_defaults(
    struct operation_policy_assignment *policy)
{
    if (policy == NULL) {
        return;
    }
    *policy = (struct operation_policy_assignment) {
        .expected_anchor_count =
            OPERATION_POLICY_ASSIGNMENT_DEFAULT_EXPECTED_ANCHOR_COUNT,
        .operation_budget_ms =
            OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS,
        .response_spread_ms =
            OPERATION_POLICY_ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
        .ram_only_iteration = false,
    };
}

void operation_policy_set_defaults(struct operation_policy_set *set)
{
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof(*set));
    operation_policy_assignment_defaults(&set->assignment);
}

uint32_t operation_policy_first_contact_cell_ms(uint8_t hop_count)
{
    uint8_t effective_hop_count =
        hop_count == 0u || hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
            DISCOVERY_ASSIGNMENT_MAX_HOPS : hop_count;

    return OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS +
           ((uint32_t)(effective_hop_count - 1u) *
            OPERATION_POLICY_FIRST_CONTACT_PER_ADDITIONAL_HOP_MS);
}

int operation_policy_first_contact_offset_ms(uint8_t slot,
                                             uint8_t slot_count,
                                             uint8_t hop_count,
                                             uint32_t *offset_ms)
{
    uint8_t effective_hop_count;
    uint64_t offset = 0u;

    if (offset_ms == NULL || slot_count == 0u || slot >= slot_count) {
        return PROTO_ERR_ARG;
    }
    effective_hop_count =
        hop_count == 0u || hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
            DISCOVERY_ASSIGNMENT_MAX_HOPS : hop_count;
    for (uint8_t depth = 1u; depth < effective_hop_count; depth++) {
        offset += (uint64_t)slot_count *
                  operation_policy_first_contact_cell_ms(depth);
    }
    offset += (uint64_t)slot *
              operation_policy_first_contact_cell_ms(effective_hop_count);
    if (offset > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    *offset_ms = (uint32_t)offset;
    return PROTO_OK;
}

int operation_policy_assignment_required_budget_ms(
    const struct operation_policy_assignment *policy,
    uint32_t *required_budget_ms)
{
    uint8_t effective_hop_count;
    uint8_t slot_count;
    uint32_t response_custody_ms;
    uint32_t claim_ack_settle_ms;
    uint64_t collection_ms;
    uint64_t table_collection_ms;
    uint64_t total_ms;

    if (policy == NULL || required_budget_ms == NULL ||
        policy->expected_anchor_count >
            OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX ||
        policy->response_spread_ms <
            OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        policy->response_spread_ms >
            OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return PROTO_ERR_ARG;
    }

    effective_hop_count = policy->expected_anchor_count == 0u ?
        DISCOVERY_ASSIGNMENT_MAX_HOPS :
        policy->expected_anchor_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
            DISCOVERY_ASSIGNMENT_MAX_HOPS :
            (uint8_t)policy->expected_anchor_count;
    slot_count = policy->expected_anchor_count == 0u ?
        OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX :
        (uint8_t)policy->expected_anchor_count;
    response_custody_ms = discovery_assignment_response_custody_ms(
        effective_hop_count);
    claim_ack_settle_ms = DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS +
        ((uint32_t)(effective_hop_count - 1u) *
         DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS);
    collection_ms = discovery_assignment_collection_window_for_topology_ms(
        policy->response_spread_ms, slot_count, effective_hop_count);
    table_collection_ms =
        discovery_assignment_table_collection_window_for_topology_ms(
            policy->response_spread_ms, slot_count, effective_hop_count);
    total_ms =
        (uint64_t)DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *
            DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS +
        collection_ms + table_collection_ms +
        ((uint64_t)DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES *
         response_custody_ms) +
        DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS +
        claim_ack_settle_ms +
        DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS +
        DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS +
        DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_GUARD_MS;
    *required_budget_ms = total_ms > OPERATION_POLICY_COMMAND_BUDGET_MAX_MS ?
        OPERATION_POLICY_COMMAND_BUDGET_MAX_MS : (uint32_t)total_ms;
    return PROTO_OK;
}

static int operation_policy_assignment_validate(
    const struct operation_policy_assignment *policy)
{
    uint32_t required_budget_ms;

    if (policy->expected_anchor_count >
            OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX ||
        policy->operation_budget_ms <
            OPERATION_POLICY_COMMAND_BUDGET_MIN_MS ||
        policy->operation_budget_ms >
            OPERATION_POLICY_COMMAND_BUDGET_MAX_MS ||
        policy->response_spread_ms <
            OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        policy->response_spread_ms >
            OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return PROTO_ERR_MALFORMED;
    }
    return operation_policy_assignment_required_budget_ms(
               policy, &required_budget_ms) == PROTO_OK &&
           policy->operation_budget_ms >= required_budget_ms ?
           PROTO_OK : PROTO_ERR_MALFORMED;
}

int operation_policy_validate(const struct operation_policy *policy)
{
    if (policy == NULL) {
        return PROTO_ERR_ARG;
    }
    return policy->family == OPERATION_POLICY_FAMILY_ASSIGNMENT ?
        operation_policy_assignment_validate(&policy->value.assignment) :
        PROTO_ERR_MALFORMED;
}

int operation_policy_decode_value(const uint8_t *value,
                                  uint8_t value_len,
                                  struct operation_policy *policy)
{
    struct operation_policy decoded = {0};

    if (value == NULL || policy == NULL) {
        return PROTO_ERR_ARG;
    }
    if (value_len != OPERATION_POLICY_ASSIGNMENT_VALUE_LEN ||
        value[0] != OPERATION_POLICY_VERSION ||
        value[1] != OPERATION_POLICY_FAMILY_ASSIGNMENT ||
        (value[2] & ~OPERATION_POLICY_ASSIGNMENT_FLAGS_MASK) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    decoded.family = OPERATION_POLICY_FAMILY_ASSIGNMENT;
    decoded.value.assignment.expected_anchor_count =
        proto_get_u16_le(&value[3]);
    decoded.value.assignment.operation_budget_ms =
        proto_get_u32_le(&value[5]);
    decoded.value.assignment.response_spread_ms =
        proto_get_u16_le(&value[9]);
    decoded.value.assignment.ram_only_iteration =
        (value[2] & OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION) != 0u;
    if (operation_policy_validate(&decoded) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    *policy = decoded;
    return PROTO_OK;
}

int operation_policy_append_tlv(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                const struct operation_policy *policy)
{
    struct operation_policy_set existing;
    uint8_t value[OPERATION_POLICY_ASSIGNMENT_VALUE_LEN] = {0};
    int ret;

    if (payload == NULL || offset == NULL || policy == NULL ||
        *offset > payload_cap) {
        return PROTO_ERR_ARG;
    }
    ret = operation_policy_validate(policy);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = operation_policy_set_from_tlvs(payload, *offset, &existing);
    if (ret != PROTO_OK || existing.assignment_present) {
        return ret != PROTO_OK ? ret : PROTO_ERR_MALFORMED;
    }
    value[0] = OPERATION_POLICY_VERSION;
    value[1] = (uint8_t)policy->family;
    value[2] = policy->value.assignment.ram_only_iteration ?
        OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION :
        OPERATION_POLICY_FLAGS_NONE;
    proto_put_u16_le(&value[3],
                     policy->value.assignment.expected_anchor_count);
    proto_put_u32_le(&value[5],
                     policy->value.assignment.operation_budget_ms);
    proto_put_u16_le(&value[9],
                     policy->value.assignment.response_spread_ms);
    return tlv_append_bytes(payload, payload_cap, offset,
                            TLV_OPERATION_POLICY, value, sizeof(value));
}

int operation_policy_set_from_tlvs(const uint8_t *payload,
                                   size_t payload_len,
                                   struct operation_policy_set *set)
{
    struct operation_policy_set parsed;
    size_t cursor = 0u;

    if (set == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    operation_policy_set_defaults(&parsed);
    while (cursor < payload_len) {
        struct operation_policy decoded;
        uint8_t type;
        uint8_t len;

        if (payload_len - cursor < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[cursor];
        len = payload[cursor + 1u];
        cursor += PROTO_TLV_HEADER_LEN;
        if (payload_len - cursor < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_OPERATION_POLICY) {
            if (parsed.assignment_present ||
                operation_policy_decode_value(&payload[cursor], len,
                                              &decoded) != PROTO_OK) {
                return PROTO_ERR_MALFORMED;
            }
            parsed.assignment = decoded.value.assignment;
            parsed.assignment_present = true;
        }
        cursor += len;
    }
    *set = parsed;
    return PROTO_OK;
}
