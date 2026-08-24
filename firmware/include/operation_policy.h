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
#define OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION (1u << 0)
#define OPERATION_POLICY_ASSIGNMENT_FLAGS_MASK \
    OPERATION_POLICY_ASSIGNMENT_FLAG_RAM_ONLY_ITERATION

/*
 * These are host-selectable mechanism bounds. Radio airtime, ACK/custody
 * horizons, retune guards, and PHY configuration deliberately do not appear in
 * this profile and remain firmware-owned safety policy.
 */
#define OPERATION_POLICY_EXPECTED_ANCHOR_COUNT_MAX 50u
#define OPERATION_POLICY_COMMAND_BUDGET_MIN_MS 1000u
#define OPERATION_POLICY_COMMAND_BUDGET_MAX_MS 1800000u

#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_EXPECTED_ANCHOR_COUNT 0u
#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS 1800000u
#define OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS 20u
#define OPERATION_POLICY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS 10000u
#define OPERATION_POLICY_ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS 1000u

/*
 * One direct response cell covers the complete first Channel-9 handoff:
 * payload TX, gateway ACK turnaround/RX, radio setup and service guards, both
 * retune edges, and five milliseconds of scheduler/clock slop.  A response
 * that traverses another connected relay adds one complete 640 ms cadence.
 * These cells serialize first attempts only; reliable transport custody and
 * retries retain their independent, much longer deadlines.
 */
#define OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS 20u
#define OPERATION_POLICY_RESPONSE_ACK_GUARD_MS 10u
#define OPERATION_POLICY_RESPONSE_ACK_RX_MS 250u
#define OPERATION_POLICY_RESPONSE_RADIO_CONFIG_GUARD_MS 25u
#define OPERATION_POLICY_RESPONSE_SERVICE_GUARD_MS 20u
#define OPERATION_POLICY_RESPONSE_RETUNE_EDGE_MS 60u
#define OPERATION_POLICY_RESPONSE_SCHEDULER_SLOP_MS 5u
#define OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS \
    (OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS + \
     OPERATION_POLICY_RESPONSE_ACK_GUARD_MS + \
     OPERATION_POLICY_RESPONSE_ACK_RX_MS + \
     OPERATION_POLICY_RESPONSE_RADIO_CONFIG_GUARD_MS + \
     OPERATION_POLICY_RESPONSE_SERVICE_GUARD_MS + \
     (2u * OPERATION_POLICY_RESPONSE_RETUNE_EDGE_MS) + \
     OPERATION_POLICY_RESPONSE_SCHEDULER_SLOP_MS)
#define OPERATION_POLICY_FIRST_CONTACT_PER_ADDITIONAL_HOP_MS 640u
/* Compatibility name for callers that need the direct response cell. */
#define OPERATION_POLICY_FIRST_CONTACT_SLOT_MS \
    OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS

enum operation_policy_family {
    OPERATION_POLICY_FAMILY_ASSIGNMENT = 1,
};

/* Exact v1 value sizes, excluding the outer two-byte TLV header. */
#define OPERATION_POLICY_COMMON_VALUE_LEN 3u
#define OPERATION_POLICY_ASSIGNMENT_VALUE_LEN 11u
#define OPERATION_POLICY_ASSIGNMENT_TLV_LEN \
    (PROTO_TLV_HEADER_LEN + OPERATION_POLICY_ASSIGNMENT_VALUE_LEN)
#define OPERATION_POLICY_ALL_TLVS_LEN OPERATION_POLICY_ASSIGNMENT_TLV_LEN

/* Current 50-anchor TABLE payload plus the propagated assignment profile. */
#define OPERATION_POLICY_ASSIGNMENT_TABLE_BASE_MAX_PAYLOAD_LEN 921u
#define OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN \
    (OPERATION_POLICY_ASSIGNMENT_TABLE_BASE_MAX_PAYLOAD_LEN + \
     OPERATION_POLICY_ASSIGNMENT_TLV_LEN)

#if OPERATION_POLICY_ASSIGNMENT_VALUE_LEN > UINT8_MAX
#error "Operation policy values must fit the one-byte TLV length"
#endif

#if OPERATION_POLICY_ASSIGNMENT_TABLE_MAX_PAYLOAD_LEN > \
    PACKET_EXT_MAX_PAYLOAD_LEN
#error "The maximum assignment table plus policy exceeds one mesh payload"
#endif

struct operation_policy_assignment {
    uint16_t expected_anchor_count;
    uint32_t operation_budget_ms;
    /* Randomized offset inside a firmware-owned hop/slot first-contact cell. */
    uint16_t response_spread_ms;
    /* Explicit bench-only opt-out; production assignments remain durable. */
    bool ram_only_iteration;
};

struct operation_policy {
    enum operation_policy_family family;
    union {
        struct operation_policy_assignment assignment;
    } value;
};

struct operation_policy_set {
    struct operation_policy_assignment assignment;
    bool assignment_present;
};

void operation_policy_assignment_defaults(
    struct operation_policy_assignment *policy);
void operation_policy_set_defaults(struct operation_policy_set *set);

uint32_t operation_policy_first_contact_cell_ms(uint8_t hop_count);
int operation_policy_first_contact_offset_ms(uint8_t slot,
                                             uint8_t slot_count,
                                             uint8_t hop_count,
                                             uint32_t *offset_ms);

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
#ifdef __cplusplus
}
#endif

#endif
