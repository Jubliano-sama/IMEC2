#ifndef DISCOVERY_ASSIGNMENT_H
#define DISCOVERY_ASSIGNMENT_H

#include "protocol.h"
#include "uwb.h"

#include <stdbool.h>
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
#define DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_MS - 1u)
#define DISCOVERY_ASSIGNMENT_HOP_STAGGER_MS 100u
#define DISCOVERY_ASSIGNMENT_MAX_HOPS 8u
#define DISCOVERY_ASSIGNMENT_RETRY_BASE_MS 100u
#define DISCOVERY_ASSIGNMENT_RETRY_MAX_MS 4000u
#define DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS 1u
#define DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS 1u
#define DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS 10000u
#define DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS 3000u
#define DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS 1000u
#define DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS + \
     ((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
      DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS))
#define DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS 30000u
#define DISCOVERY_ASSIGNMENT_RESPONSE_PER_ADDITIONAL_HOP_MS 10000u
#define DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS + \
     ((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
      DISCOVERY_ASSIGNMENT_RESPONSE_PER_ADDITIONAL_HOP_MS))
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS + \
     ((UWB_DISCOVERY_SLOT_COUNT - 1u) * \
      DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_MS) + \
     ((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
      DISCOVERY_ASSIGNMENT_HOP_STAGGER_MS) + \
     DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS)
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_HOP_INITIAL_DELAY_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS + \
     ((UWB_DISCOVERY_SLOT_COUNT - 1u) * \
      DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_MS) + \
     DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS)
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_ROUTE_WINDOW_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS + \
     DISCOVERY_ASSIGNMENT_RESPONSE_MAX_HOP_INITIAL_DELAY_MS)
#define DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS 5u
#define DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT 2u
#define DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS \
    (DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT * \
     DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS)
#define DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_GUARD_MS 1u
/*
 * CLAIM/ACK responses can become ready while the gateway is still completing
 * its four-copy control flood.  Keep custody through the full reliable
 * protocol-response retry horizon so the later collection RX window remains
 * reachable even after a multi-hop flood.
 */
#define DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS \
    (2u * (DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS + \
           DISCOVERY_ASSIGNMENT_RESPONSE_MAX_ROUTE_WINDOW_MS) + \
     DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS + \
     DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS + \
     DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS + \
     DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_GUARD_MS)
#define DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS \
    DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS

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
int discovery_assignment_sort_anchor_ids(uint64_t *anchor_ids,
                                         size_t anchor_count);
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
int discovery_assignment_append_table_from_anchor_ids(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint64_t *anchor_ids,
    size_t anchor_count);
int discovery_assignment_parse_table_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    uint8_t *slot_count);
uint32_t discovery_assignment_table_fingerprint(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t slot_count);
int discovery_assignment_response_delay_ms(uint8_t slot,
                                           uint8_t slot_count,
                                           uint8_t hop_count,
                                           uint8_t retry_round,
                                           uint32_t random_value,
                                           uint32_t *delay_ms);
uint32_t discovery_assignment_retry_backoff_ms(uint8_t retry_round,
                                               uint32_t random_value);
uint32_t discovery_assignment_response_custody_ms(uint8_t hop_count);
uint64_t discovery_assignment_response_deadline_ms(uint64_t now_ms,
                                                   uint32_t response_delay_ms,
                                                   uint8_t hop_count);
uint16_t discovery_assignment_membership_epoch(uint32_t assignment_epoch);
uint32_t discovery_assignment_collection_window_ms(uint8_t slot_count,
                                                   uint8_t max_hop_count);
uint64_t discovery_assignment_control_flood_deadline_ms(
    uint64_t now_ms,
    uint64_t operation_deadline_ms);
uint64_t discovery_assignment_response_ack_settle_deadline_ms(uint64_t now_ms);
uint32_t discovery_assignment_claim_ack_settle_duration_ms(uint8_t hop_count);
uint64_t discovery_assignment_claim_ack_settle_deadline_ms(
    uint64_t now_ms,
    uint8_t hop_count);
bool discovery_assignment_response_ack_settle_pending(
    uint64_t now_ms,
    uint64_t settle_deadline_ms);
int discovery_assignment_extract_expected_count(const uint8_t *payload,
                                                 size_t payload_len,
                                                 uint16_t *expected_count,
                                                 bool *present);
bool discovery_assignment_response_custody_matches(
    bool active,
    uint32_t pending_epoch,
    enum discovery_assignment_phase pending_phase,
    uint32_t pending_session_id,
    uint32_t incoming_epoch,
    enum discovery_assignment_phase incoming_phase,
    uint32_t incoming_session_id);

#ifdef __cplusplus
}
#endif

#endif
