#include "app_anchor_assignment_policy.h"

#include "discovery_assignment.h"

uint32_t app_anchor_assignment_normalize_epoch(uint32_t candidate)
{
    return candidate == 0u ? 1u : candidate;
}

uint32_t app_anchor_assignment_next_nonzero(uint32_t current)
{
    current++;
    return current == 0u ? 1u : current;
}

static uint64_t expected_ack_mask(size_t claim_count)
{
    return claim_count >= 64u ? UINT64_MAX :
           (UINT64_C(1) << claim_count) - 1u;
}

uint8_t app_anchor_assignment_missing_ack_count(size_t claim_count,
                                                uint64_t ack_mask)
{
    uint64_t missing = expected_ack_mask(claim_count) & ~ack_mask;
    uint8_t count = 0u;

    while (missing != 0u) {
        count += (uint8_t)(missing & 1u);
        missing >>= 1;
    }
    return count;
}

bool app_anchor_assignment_claims_complete(uint16_t expected_claim_count,
                                           size_t claim_count)
{
    return expected_claim_count != 0u &&
           claim_count >= expected_claim_count;
}

uint32_t app_anchor_assignment_settle_remaining_ms(bool armed,
                                                   uint64_t now_ms,
                                                   uint64_t deadline_ms)
{
    uint64_t remaining_ms;

    if (!armed ||
        !discovery_assignment_response_ack_settle_pending(now_ms,
                                                          deadline_ms)) {
        return 0u;
    }
    remaining_ms = deadline_ms - now_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX :
                                       (uint32_t)remaining_ms;
}

size_t app_anchor_assignment_collect_committed(
    const uint64_t *anchor_ids,
    size_t claim_count,
    uint64_t ack_mask,
    struct discovery_assignment_entry *committed_entries,
    uint64_t *committed_anchor_ids,
    size_t committed_capacity)
{
    size_t committed_count = 0u;

    if (anchor_ids == NULL || committed_entries == NULL ||
        committed_anchor_ids == NULL || committed_capacity == 0u) {
        return 0u;
    }

    for (size_t i = 0u;
         i < claim_count && i < 64u && committed_count < committed_capacity;
         i++) {
        if ((ack_mask & (UINT64_C(1) << i)) == 0u) {
            continue;
        }
        committed_anchor_ids[committed_count] = anchor_ids[i];
        committed_entries[committed_count] =
            (struct discovery_assignment_entry) {
                .anchor_id = anchor_ids[i],
                .hash = discovery_assignment_hash(anchor_ids[i]),
                .slot = (uint8_t)i,
            };
        committed_count++;
    }
    return committed_count;
}
