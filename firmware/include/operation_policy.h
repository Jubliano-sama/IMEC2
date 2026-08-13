#ifndef OPERATION_POLICY_H
#define OPERATION_POLICY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPERATION_POLICY_VERSION 1u
#define OPERATION_POLICY_FLAGS_NONE 0u

/*
 * These are host-selectable mechanism bounds. Radio airtime, ACK/custody
 * horizons, retune guards, and PHY configuration deliberately do not appear in
 * this profile and remain firmware-owned safety policy.
 */
#define OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX 50u
#define OPERATION_POLICY_COMMAND_BUDGET_MIN_MS 1000u
#define OPERATION_POLICY_COMMAND_BUDGET_MAX_MS 1800000u

#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_EXPECTED_ANCHOR_COUNT 0u
#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS 1591204u
#define OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS 20u
#define OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS 10000u
#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS 1000u

#define OPERATION_POLICY_DISCOVERY_START_DELAY_MIN_MS 60000u
#define OPERATION_POLICY_DISCOVERY_START_DELAY_MAX_MS 60000u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_START_DELAY_MS 60000u
#define OPERATION_POLICY_DISCOVERY_SLOT_MIN_MS 30u
#define OPERATION_POLICY_DISCOVERY_SLOT_MAX_MS 1000u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_SLOT_MS 40u
#define OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MIN 1u
#define OPERATION_POLICY_DISCOVERY_SLOT_COUNT_MAX 50u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_SLOT_COUNT 6u
#define OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MIN 1u
#define OPERATION_POLICY_DISCOVERY_ROUND_COUNT_MAX 4u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_ROUND_COUNT 4u
#define OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MIN_MS 1u
#define OPERATION_POLICY_DISCOVERY_REPORT_GRACE_MAX_MS 60000u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_REPORT_GRACE_MS 250u
#define OPERATION_POLICY_DISCOVERY_DEFAULT_BUDGET_MS 900000u

#define OPERATION_POLICY_PAIR_MAX_RERUNS 2u
#define OPERATION_POLICY_PAIR_DEFAULT_MAX_RERUNS 2u
#define OPERATION_POLICY_PAIR_MAX_PARALLEL_PAIRS 25u
#define OPERATION_POLICY_PAIR_DEFAULT_MAX_PARALLEL_PAIRS 25u

enum operation_policy_family {
    OPERATION_POLICY_FAMILY_ASSIGNMENT = 1,
    OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY = 2,
    OPERATION_POLICY_FAMILY_SURVEY_PAIR = 3,
};

/* Exact v1 value sizes, excluding the outer two-byte TLV header. */
#define OPERATION_POLICY_COMMON_VALUE_LEN 3u
#define OPERATION_POLICY_ASSIGNMENT_VALUE_LEN 11u
#define OPERATION_POLICY_DISCOVERY_VALUE_LEN 19u
#define OPERATION_POLICY_PAIR_VALUE_LEN 5u
#define OPERATION_POLICY_ASSIGNMENT_TLV_LEN \
    (PROTO_TLV_HEADER_LEN + OPERATION_POLICY_ASSIGNMENT_VALUE_LEN)
#define OPERATION_POLICY_DISCOVERY_TLV_LEN \
    (PROTO_TLV_HEADER_LEN + OPERATION_POLICY_DISCOVERY_VALUE_LEN)
#define OPERATION_POLICY_PAIR_TLV_LEN \
    (PROTO_TLV_HEADER_LEN + OPERATION_POLICY_PAIR_VALUE_LEN)
#define OPERATION_POLICY_ALL_TLVS_LEN \
    (OPERATION_POLICY_ASSIGNMENT_TLV_LEN + \
     OPERATION_POLICY_DISCOVERY_TLV_LEN + \
     OPERATION_POLICY_PAIR_TLV_LEN)

/* Current 50-anchor TABLE payload plus the propagated assignment profile. */
#define OPERATION_POLICY_ASSIGNMENT_TABLE_BASE_MAX_PAYLOAD_LEN 921u
#define OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN \
    (OPERATION_POLICY_ASSIGNMENT_TABLE_BASE_MAX_PAYLOAD_LEN + \
     OPERATION_POLICY_ASSIGNMENT_TLV_LEN)

#if OPERATION_POLICY_ASSIGNMENT_VALUE_LEN > UINT8_MAX || \
    OPERATION_POLICY_DISCOVERY_VALUE_LEN > UINT8_MAX || \
    OPERATION_POLICY_PAIR_VALUE_LEN > UINT8_MAX
#error "Operation policy values must fit the one-byte TLV length"
#endif

#if OPERATION_POLICY_ALL_TLVS_LEN > PACKET_MAX_PAYLOAD_LEN
#error "A complete operation policy set must fit a standard packet payload"
#endif

#if OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN > \
    PACKET_EXT_MAX_PAYLOAD_LEN
#error "The maximum assignment table plus policy exceeds one mesh payload"
#endif

struct operation_policy_assignment {
    uint16_t expected_anchor_count;
    uint32_t operation_budget_ms;
    /* Equal randomized spreading for every responder; never hop-ranked. */
    uint16_t response_spread_ms;
};

struct operation_policy_discovery {
    uint32_t start_delay_ms;
    uint16_t slot_ms;
    uint8_t slot_count;
    uint8_t round_count;
    uint32_t report_grace_ms;
    uint32_t operation_budget_ms;
};

struct operation_policy_pair {
    uint8_t max_reruns;
    uint8_t max_parallel_pairs;
};

struct operation_policy {
    enum operation_policy_family family;
    union {
        struct operation_policy_assignment assignment;
        struct operation_policy_discovery discovery;
        struct operation_policy_pair pair;
    } value;
};

struct operation_policy_set {
    struct operation_policy_assignment assignment;
    struct operation_policy_discovery discovery;
    struct operation_policy_pair pair;
    bool assignment_present;
    bool discovery_present;
    bool pair_present;
};

struct operation_policy_discovery_budget_terms {
    uint32_t report_slot_ms;
    uint32_t report_custody_ms;
    uint32_t report_delivery_tail_ms;
    uint32_t terminal_scheduling_guard_ms;
};

void operation_policy_assignment_defaults(
    struct operation_policy_assignment *policy);
void operation_policy_discovery_defaults(
    struct operation_policy_discovery *policy);
void operation_policy_pair_defaults(struct operation_policy_pair *policy);
void operation_policy_set_defaults(struct operation_policy_set *set);

int operation_policy_validate(const struct operation_policy *policy);
int operation_policy_decode_value(const uint8_t *value,
                                  uint8_t value_len,
                                  struct operation_policy *policy);
int operation_policy_append_tlv(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                const struct operation_policy *policy);
int operation_policy_set_from_tlvs(const uint8_t *payload,
                                   size_t payload_len,
                                   struct operation_policy_set *set);
int operation_policy_assignment_required_budget_ms(
    const struct operation_policy_assignment *policy,
    uint32_t *required_budget_ms);
int operation_policy_discovery_required_budget_ms(
    const struct operation_policy_discovery *policy,
    const struct operation_policy_discovery_budget_terms *terms,
    uint32_t *required_budget_ms);

#ifdef __cplusplus
}
#endif

#endif
