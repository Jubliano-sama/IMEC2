#ifndef APP_ANCHOR_ASSIGNMENT_POLICY_H
#define APP_ANCHOR_ASSIGNMENT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct discovery_assignment_entry;

uint32_t app_anchor_assignment_normalize_epoch(uint32_t candidate);
uint32_t app_anchor_assignment_next_nonzero(uint32_t current);
uint8_t app_anchor_assignment_missing_ack_count(size_t claim_count,
                                                uint64_t ack_mask);
bool app_anchor_assignment_claims_complete(uint16_t expected_claim_count,
                                           size_t claim_count);
uint32_t app_anchor_assignment_settle_remaining_ms(bool armed,
                                                   uint64_t now_ms,
                                                   uint64_t deadline_ms);
size_t app_anchor_assignment_collect_committed(
    const uint64_t *anchor_ids,
    size_t claim_count,
    uint64_t ack_mask,
    struct discovery_assignment_entry *committed_entries,
    uint64_t *committed_anchor_ids,
    size_t committed_capacity);

#endif
