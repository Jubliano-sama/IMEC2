#include "operation_policy.h"

#include "discovery_assignment.h"
#include "survey.h"

#include <string.h>

static uint8_t operation_policy_value_len(enum operation_policy_family family)
{
    switch (family) {
    case OPERATION_POLICY_FAMILY_ASSIGNMENT:
        return OPERATION_POLICY_ASSIGNMENT_VALUE_LEN;
    case OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        return OPERATION_POLICY_DISCOVERY_VALUE_LEN;
    case OPERATION_POLICY_FAMILY_SURVEY_PAIR:
        return OPERATION_POLICY_PAIR_VALUE_LEN;
    default:
        return 0u;
    }
}

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
    };
}

void operation_policy_discovery_defaults(
    struct operation_policy_discovery *policy)
{
    if (policy == NULL) {
        return;
    }
    *policy = (struct operation_policy_discovery) {
        .start_delay_ms = OPERATION_POLICY_DISCOVERY_DEFAULT_START_DELAY_MS,
        .slot_ms = OPERATION_POLICY_DISCOVERY_DEFAULT_SLOT_MS,
        .slot_count = OPERATION_POLICY_DISCOVERY_DEFAULT_SLOT_COUNT,
        .round_count = OPERATION_POLICY_DISCOVERY_DEFAULT_ROUND_COUNT,
        .report_grace_ms =
            OPERATION_POLICY_DISCOVERY_DEFAULT_REPORT_GRACE_MS,
        .operation_budget_ms = OPERATION_POLICY_DISCOVERY_DEFAULT_BUDGET_MS,
    };
}

void operation_policy_pair_defaults(struct operation_policy_pair *policy)
{
    if (policy == NULL) {
        return;
    }
    *policy = (struct operation_policy_pair) {
        .max_reruns = OPERATION_POLICY_PAIR_DEFAULT_MAX_RERUNS,
        .max_parallel_pairs =
            OPERATION_POLICY_PAIR_DEFAULT_MAX_PARALLEL_PAIRS,
    };
}

void operation_policy_set_defaults(struct operation_policy_set *set)
{
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof(*set));
    operation_policy_assignment_defaults(&set->assignment);
    operation_policy_discovery_defaults(&set->discovery);
    operation_policy_pair_defaults(&set->pair);
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

static int operation_policy_discovery_validate(
    const struct operation_policy_discovery *policy)
{
    const struct operation_policy_discovery_budget_terms base_terms = {0};
    uint32_t required_budget_ms;

    if (policy->start_delay_ms <
            OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS ||
        policy->start_delay_ms >
            OPERATION_POLICY_DISCOVERY_START_DELAY_MAX_MS ||
        policy->slot_ms < OPERATION_POLICY_DISCOVERY_SLOT_MIN_MS ||
        policy->slot_ms > OPERATION_POLICY_DISCOVERY_SLOT_MAX_MS ||
        policy->slot_count < OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MIN ||
        policy->slot_count > OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MAX ||
        policy->round_count < OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MIN ||
        policy->round_count > OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MAX ||
        policy->report_grace_ms <
            OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MIN_MS ||
        policy->report_grace_ms >
            OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MAX_MS ||
        policy->operation_budget_ms <
            OPERATION_POLICY_COMMAND_BUDGET_MIN_MS ||
        policy->operation_budget_ms >
            OPERATION_POLICY_COMMAND_BUDGET_MAX_MS) {
        return PROTO_ERR_MALFORMED;
    }
    return operation_policy_discovery_required_budget_ms(
               policy, &base_terms, &required_budget_ms) == PROTO_OK &&
           policy->operation_budget_ms >= required_budget_ms ?
           PROTO_OK : PROTO_ERR_MALFORMED;
}

static int operation_policy_pair_validate(
    const struct operation_policy_pair *policy)
{
    if (policy->max_reruns > OPERATION_POLICY_PAIR_MAX_RERUNS ||
        policy->max_parallel_pairs == 0u ||
        policy->max_parallel_pairs >
            OPERATION_POLICY_PAIR_MAX_PARALLEL_PAIRS) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int operation_policy_validate(const struct operation_policy *policy)
{
    if (policy == NULL) {
        return PROTO_ERR_ARG;
    }
    switch (policy->family) {
    case OPERATION_POLICY_FAMILY_ASSIGNMENT:
        return operation_policy_assignment_validate(&policy->value.assignment);
    case OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        return operation_policy_discovery_validate(&policy->value.discovery);
    case OPERATION_POLICY_FAMILY_SURVEY_PAIR:
        return operation_policy_pair_validate(&policy->value.pair);
    default:
        return PROTO_ERR_MALFORMED;
    }
}

int operation_policy_decode_value(const uint8_t *value,
                                  uint8_t value_len,
                                  struct operation_policy *policy)
{
    struct operation_policy decoded = {0};
    enum operation_policy_family family;
    uint8_t expected_len;
    int ret;

    if (value == NULL || policy == NULL) {
        return PROTO_ERR_ARG;
    }
    if (value_len < OPERATION_POLICY_COMMON_VALUE_LEN ||
        value[0] != OPERATION_POLICY_VERSION ||
        value[2] != OPERATION_POLICY_FLAGS_NONE) {
        return PROTO_ERR_MALFORMED;
    }
    family = (enum operation_policy_family)value[1];
    expected_len = operation_policy_value_len(family);
    if (expected_len == 0u || value_len != expected_len) {
        return PROTO_ERR_MALFORMED;
    }

    decoded.family = family;
    switch (family) {
    case OPERATION_POLICY_FAMILY_ASSIGNMENT:
        decoded.value.assignment.expected_anchor_count =
            proto_get_u16_le(&value[3]);
        decoded.value.assignment.operation_budget_ms =
            proto_get_u32_le(&value[5]);
        decoded.value.assignment.response_spread_ms =
            proto_get_u16_le(&value[9]);
        break;
    case OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        decoded.value.discovery.start_delay_ms =
            proto_get_u32_le(&value[3]);
        decoded.value.discovery.slot_ms = proto_get_u16_le(&value[7]);
        decoded.value.discovery.slot_count = value[9];
        decoded.value.discovery.round_count = value[10];
        decoded.value.discovery.report_grace_ms =
            proto_get_u32_le(&value[11]);
        decoded.value.discovery.operation_budget_ms =
            proto_get_u32_le(&value[15]);
        break;
    case OPERATION_POLICY_FAMILY_SURVEY_PAIR:
        decoded.value.pair.max_reruns = value[3];
        decoded.value.pair.max_parallel_pairs = value[4];
        break;
    default:
        return PROTO_ERR_MALFORMED;
    }
    ret = operation_policy_validate(&decoded);
    if (ret != PROTO_OK) {
        return ret;
    }
    *policy = decoded;
    return PROTO_OK;
}

static void operation_policy_encode_value(const struct operation_policy *policy,
                                          uint8_t *value,
                                          uint8_t value_len)
{
    memset(value, 0, value_len);
    value[0] = OPERATION_POLICY_VERSION;
    value[1] = (uint8_t)policy->family;
    value[2] = OPERATION_POLICY_FLAGS_NONE;

    switch (policy->family) {
    case OPERATION_POLICY_FAMILY_ASSIGNMENT:
        proto_put_u16_le(&value[3],
                         policy->value.assignment.expected_anchor_count);
        proto_put_u32_le(&value[5],
                         policy->value.assignment.operation_budget_ms);
        proto_put_u16_le(&value[9],
                         policy->value.assignment.response_spread_ms);
        break;
    case OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
        proto_put_u32_le(&value[3], policy->value.discovery.start_delay_ms);
        proto_put_u16_le(&value[7], policy->value.discovery.slot_ms);
        value[9] = policy->value.discovery.slot_count;
        value[10] = policy->value.discovery.round_count;
        proto_put_u32_le(&value[11],
                         policy->value.discovery.report_grace_ms);
        proto_put_u32_le(&value[15],
                         policy->value.discovery.operation_budget_ms);
        break;
    case OPERATION_POLICY_FAMILY_SURVEY_PAIR:
        value[3] = policy->value.pair.max_reruns;
        value[4] = policy->value.pair.max_parallel_pairs;
        break;
    default:
        break;
    }
}

int operation_policy_append_tlv(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                const struct operation_policy *policy)
{
    struct operation_policy_set existing;
    uint8_t value[OPERATION_POLICY_DISCOVERY_VALUE_LEN];
    uint8_t value_len;
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
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((policy->family == OPERATION_POLICY_FAMILY_ASSIGNMENT &&
         existing.assignment_present) ||
        (policy->family == OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY &&
         existing.discovery_present) ||
        (policy->family == OPERATION_POLICY_FAMILY_SURVEY_PAIR &&
         existing.pair_present)) {
        return PROTO_ERR_MALFORMED;
    }
    value_len = operation_policy_value_len(policy->family);
    operation_policy_encode_value(policy, value, value_len);
    return tlv_append_bytes(payload, payload_cap, offset,
                            TLV_OPERATION_POLICY, value, value_len);
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
        int ret;

        if (payload_len - cursor < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[cursor];
        len = payload[cursor + 1u];
        cursor += PROTO_TLV_HEADER_LEN;
        if (payload_len - cursor < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type != TLV_OPERATION_POLICY) {
            cursor += len;
            continue;
        }
        ret = operation_policy_decode_value(&payload[cursor], len, &decoded);
        if (ret != PROTO_OK) {
            return ret;
        }
        switch (decoded.family) {
        case OPERATION_POLICY_FAMILY_ASSIGNMENT:
            if (parsed.assignment_present) {
                return PROTO_ERR_MALFORMED;
            }
            parsed.assignment = decoded.value.assignment;
            parsed.assignment_present = true;
            break;
        case OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY:
            if (parsed.discovery_present) {
                return PROTO_ERR_MALFORMED;
            }
            parsed.discovery = decoded.value.discovery;
            parsed.discovery_present = true;
            break;
        case OPERATION_POLICY_FAMILY_SURVEY_PAIR:
            if (parsed.pair_present) {
                return PROTO_ERR_MALFORMED;
            }
            parsed.pair = decoded.value.pair;
            parsed.pair_present = true;
            break;
        default:
            return PROTO_ERR_MALFORMED;
        }
        cursor += len;
    }
    *set = parsed;
    return PROTO_OK;
}

int operation_policy_assignment_required_budget_ms(
    const struct operation_policy_assignment *policy,
    uint32_t *required_budget_ms)
{
    uint64_t collection_ms;
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

    collection_ms =
        (uint64_t)discovery_assignment_response_custody_ms(
            DISCOVERY_ASSIGNMENT_MAX_HOPS) +
        DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS +
        policy->response_spread_ms - 1u;
    total_ms =
        (uint64_t)DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *
            DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS +
        2u * collection_ms +
        DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS +
        DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS +
        DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS +
        DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_GUARD_MS;
    if (total_ms > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    *required_budget_ms = (uint32_t)total_ms;
    return PROTO_OK;
}

int operation_policy_discovery_required_budget_ms(
    const struct operation_policy_discovery *policy,
    const struct operation_policy_discovery_budget_terms *terms,
    uint32_t *required_budget_ms)
{
    uint64_t discovery_ms;
    uint64_t report_emission_ms;
    uint64_t total_ms;

    if (policy == NULL || terms == NULL || required_budget_ms == NULL ||
        policy->start_delay_ms <
            OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS ||
        policy->start_delay_ms >
            OPERATION_POLICY_DISCOVERY_START_DELAY_MAX_MS ||
        policy->slot_ms < OPERATION_POLICY_DISCOVERY_SLOT_MIN_MS ||
        policy->slot_ms > OPERATION_POLICY_DISCOVERY_SLOT_MAX_MS ||
        policy->slot_count < OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MIN ||
        policy->slot_count > OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MAX ||
        policy->round_count < OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MIN ||
        policy->round_count > OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MAX ||
        policy->report_grace_ms <
            OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MIN_MS ||
        policy->report_grace_ms >
            OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MAX_MS) {
        return PROTO_ERR_ARG;
    }

    discovery_ms = (uint64_t)policy->slot_ms * policy->slot_count *
                   policy->round_count;
    report_emission_ms =
        (uint64_t)terms->report_slot_ms * policy->slot_count;
    total_ms = policy->start_delay_ms + discovery_ms + report_emission_ms +
               policy->report_grace_ms + terms->report_custody_ms +
               terms->report_delivery_tail_ms +
               terms->terminal_scheduling_guard_ms +
               SURVEY_DISCOVERY_OPERATION_TERMINAL_GUARD_MS;
    if (total_ms > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    *required_budget_ms = (uint32_t)total_ms;
    return PROTO_OK;
}
