#ifndef DISCOVERY_ASSIGNMENT_H
#define DISCOVERY_ASSIGNMENT_H

#include "protocol.h"
#include "uwb.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN 17u
#define DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV \
    (UINT8_MAX / DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN)
#define DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS 100u
#define DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_MS 20u
#define DISCOVERY_ASSIGNMENT_HOP_STAGGER_MS 100u
#define DISCOVERY_ASSIGNMENT_MAX_HOPS 8u
#define DISCOVERY_ASSIGNMENT_RETRY_BASE_MS 100u
#define DISCOVERY_ASSIGNMENT_RETRY_MAX_MS 4000u
#define DISCOVERY_ASSIGNMENT_COLLECTION_BASE_MS 3000u
#define DISCOVERY_ASSIGNMENT_COLLECTION_PER_HOP_MS 750u

enum discovery_assignment_phase {
    DISCOVERY_ASSIGNMENT_PHASE_CLAIM = 1,
    DISCOVERY_ASSIGNMENT_PHASE_TABLE = 2,
    DISCOVERY_ASSIGNMENT_PHASE_ACK = 3,
};

struct discovery_assignment_claim {
    uint64_t anchor_id;
    uint64_t hash;
};

struct discovery_assignment_entry {
    uint64_t anchor_id;
    uint64_t hash;
    uint8_t slot;
};

uint64_t discovery_assignment_hash(uint64_t anchor_id);
int discovery_assignment_sort_claims(struct discovery_assignment_claim *claims,
                                     size_t claim_count);
int discovery_assignment_entries_from_claims(
    const struct discovery_assignment_claim *claims,
    size_t claim_count,
    struct discovery_assignment_entry *entries,
    size_t entry_cap);
int discovery_assignment_append_control_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    enum discovery_assignment_phase phase,
    uint32_t epoch);
int discovery_assignment_extract_control_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    enum discovery_assignment_phase *phase,
    uint32_t *epoch);
int discovery_assignment_append_claim_hash(uint8_t *payload,
                                           size_t payload_cap,
                                           size_t *offset,
                                           uint64_t hash);
int discovery_assignment_extract_claim_hash(const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t *hash);
int discovery_assignment_append_table_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_entry *entries,
    size_t entry_count);
int discovery_assignment_parse_table_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    uint8_t *slot_count);
int discovery_assignment_response_delay_ms(uint8_t slot,
                                           uint8_t slot_count,
                                           uint8_t hop_count,
                                           uint8_t retry_round,
                                           uint32_t random_value,
                                           uint32_t *delay_ms);
uint32_t discovery_assignment_retry_backoff_ms(uint8_t retry_round,
                                               uint32_t random_value);
uint32_t discovery_assignment_collection_window_ms(uint8_t slot_count,
                                                   uint8_t max_hop_count);

#ifdef __cplusplus
}
#endif

#endif
