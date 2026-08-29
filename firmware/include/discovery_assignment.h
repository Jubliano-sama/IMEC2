#ifndef DISCOVERY_ASSIGNMENT_H
#define DISCOVERY_ASSIGNMENT_H

#include "protocol.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "node_comm.h"
#include "operation_policy.h"
#include "semantic_digest.h"
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
#define DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS 20u
#define DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS 10000u
#define DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS 1000u
#define DISCOVERY_ASSIGNMENT_MAX_HOPS 8u
#define DISCOVERY_ASSIGNMENT_RETRY_BASE_MS 100u
#define DISCOVERY_ASSIGNMENT_RETRY_MAX_MS 4000u
#define DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES 2u
/* Retry rounds 0..1 contribute at most 199 + 399 ms. */
#define DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS 598u
#define DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES 3u
/*
 * The ACK owner retries rounds 0..2 before entering its minute-scale
 * low-duty recovery.  Each retry_backoff result is in [base, 2*base), so the
 * exact worst-case sum is 199 + 399 + 799 ms.
 */
#define DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS 1397u
#define DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S 120u
#define DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS 1u
#define DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS 1u
#define DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP 16u
#define DISCOVERY_ASSIGNMENT_SCHEME_VERSION 2u
#define DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS \
    NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS
#define DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS 450u
/*
 * The analytical next-depth bound ends at the nominal response-ready edge.
 * Assignment responses listen 250 ms for their immediate gateway ACK, then
 * enter a prompt jittered retry whose first backoff is below 100 ms.  The
 * The 850 ms tail contains that complete retry opportunity plus the measured
 * worst-case parent-turn alignment, radio reconfigure, frame airtime, and
 * gateway RX workqueue margin.  The F1DD bound needs 788 ms after the nominal
 * band edge; retain another 62 ms of clock and scheduler redundancy.
 */
#define DISCOVERY_ASSIGNMENT_ADAPTIVE_RX_MARGIN_MS 850u
#define DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS 640u
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
#define DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS \
    (((MESH_ENUMERATION_RELAY_COPY_COUNT - 1u) * \
      (OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS + \
       MESH_ENUMERATION_RELAY_COPY_SPACING_MAX_MS)) + 5u)
#define DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS \
    (DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS + \
     MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS + \
     (MESH_ENUMERATION_RELAY_COPY_COUNT * \
      OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS) + \
     MESH_ENUMERATION_RELAY_COPY_TAIL_MS)
#define DISCOVERY_ASSIGNMENT_ACTIVATION_RELAY_HOP_MAX_MS \
    (MESH_RADIO_CONTROL_RELAY_WAKE_ENVELOPE_MS + \
     DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS)
#define DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS 150u
#define DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_REDUNDANCY_MS 2000u
#define DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_MIN_MS \
    (MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS + \
     MESH_RADIO_EVENT_RETUNE_GUARD_MS + \
     DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS + \
     DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS + \
     DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_REDUNDANCY_MS)
#define DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_MAX_MS \
    (MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS + \
     MESH_RADIO_EVENT_RETUNE_GUARD_MS + \
     DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS + \
     (DISCOVERY_ASSIGNMENT_MAX_HOPS * \
      DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS) + \
     DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_REDUNDANCY_MS)
#define DISCOVERY_ASSIGNMENT_RESPONSE_JITTER_CAP_MS(response_spread_ms) \
    ((response_spread_ms) < OPERATION_POLICY_FIRST_CONTACT_SLOT_MS ? \
         (response_spread_ms) : OPERATION_POLICY_FIRST_CONTACT_SLOT_MS)
#define DISCOVERY_ASSIGNMENT_FIRST_CONTACT_MAX_OFFSET_MS(slot_count) \
    (((uint32_t)(slot_count) * \
      (((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
        OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS) + \
       ((OPERATION_POLICY_FIRST_CONTACT_PER_ADDITIONAL_HOP_MS * \
         (DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
         (DISCOVERY_ASSIGNMENT_MAX_HOPS - 2u)) / 2u))) + \
     (((uint32_t)(slot_count) - 1u) * \
      (OPERATION_POLICY_FIRST_CONTACT_DIRECT_SLOT_MS + \
       ((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) * \
        OPERATION_POLICY_FIRST_CONTACT_PER_ADDITIONAL_HOP_MS))))
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS( \
    response_spread_ms) \
    (DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS + \
     DISCOVERY_ASSIGNMENT_FIRST_CONTACT_MAX_OFFSET_MS( \
         UWB_DISCOVERY_SLOT_COUNT) + \
     DISCOVERY_ASSIGNMENT_RESPONSE_JITTER_CAP_MS(response_spread_ms) - 1u)
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS \
    DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS( \
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS)
#define DISCOVERY_ASSIGNMENT_RESPONSE_MAX_ROUTE_WINDOW_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS + \
     DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS)
#define DISCOVERY_ASSIGNMENT_TABLE_RESPONSE_MAX_ROUTE_WINDOW_MS \
    (DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS + \
     ((1u + DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES) * \
      DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS) + \
     DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS)
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
#define DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(response_spread_ms) \
    ((DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *                          \
      DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS) +                  \
     (DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS +                     \
      DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS(     \
          (response_spread_ms))) +                                       \
     (DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES *                   \
      DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS) +                    \
     DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS +              \
     ((1u + DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES) *              \
      DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS) +                    \
     DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS(      \
         (response_spread_ms)) +                                         \
     DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS +                \
     DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS +                      \
     DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS +                       \
     DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS +       \
     DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_GUARD_MS)
#define DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS \
    OPERATION_POLICY_COMMAND_BUDGET_MAX_MS
#define DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS \
    OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS

enum discovery_assignment_phase {
    DISCOVERY_ASSIGNMENT_PHASE_CLAIM = 1,
    DISCOVERY_ASSIGNMENT_PHASE_TABLE = 2,
    DISCOVERY_ASSIGNMENT_PHASE_ACK = 3,
    DISCOVERY_ASSIGNMENT_PHASE_END = 4,
    DISCOVERY_ASSIGNMENT_PHASE_ABORT = 5,
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

struct discovery_assignment_table_commitment {
    uint8_t bytes[SEMANTIC_DIGEST_SHA256_LEN];
};

#define DISCOVERY_ASSIGNMENT_END_IDENTITY_WIRE_LEN \
    (sizeof(uint32_t) + sizeof(uint32_t) + \
     sizeof(struct discovery_assignment_table_commitment))

struct discovery_assignment_end_identity {
    uint32_t epoch;
    uint32_t table_command_seq;
    struct discovery_assignment_table_commitment table_commitment;
};

#define DISCOVERY_ASSIGNMENT_ABORT_IDENTITY_WIRE_LEN \
    (3u * sizeof(uint32_t))

struct discovery_assignment_abort_identity {
    uint32_t epoch;
    uint32_t claim_session_id;
    uint32_t claim_command_seq;
};

_Static_assert(sizeof(struct discovery_assignment_table_commitment) ==
                   SEMANTIC_DIGEST_SHA256_LEN,
               "assignment TABLE commitment must be one SHA-256 digest");

/*
 * CLAIM and ACK are internal command-result controls.  Parse their complete
 * authoritative singleton envelope before classifying them as internal or
 * allowing gateway assignment state to change.
 */
struct discovery_assignment_result {
    enum discovery_assignment_phase phase;
    uint32_t epoch;
    uint64_t hash;
    struct discovery_assignment_table_commitment table_commitment;
    uint8_t hop_count;
    bool hop_count_present;
};

uint64_t discovery_assignment_hash(uint64_t anchor_id);
static inline bool discovery_assignment_epoch_strictly_newer(
    uint32_t candidate,
    uint32_t reference)
{
    return candidate != 0u && reference != 0u &&
           candidate != reference &&
           (int32_t)(candidate - reference) > 0;
}

static inline uint32_t discovery_assignment_next_epoch(uint32_t current)
{
    current++;
    return current == 0u ? 1u : current;
}

/*
 * Reconcile the standalone reservation cursor with the newest assignment
 * proof retained in the durable membership record. A zero input means that
 * the corresponding validated record is absent. RFC 1982 half-range
 * ambiguity fails closed instead of choosing an epoch.
 */
int discovery_assignment_reconcile_epoch_baseline(
    uint32_t cursor_epoch,
    uint32_t proof_epoch,
    uint32_t *resolved_epoch,
    bool *cursor_repair_required);

int discovery_assignment_sort_claims(struct discovery_assignment_claim *claims,
                                     size_t claim_count);
int discovery_assignment_sort_anchor_ids(uint64_t *anchor_ids,
                                         size_t anchor_count);
/*
 * Preserve the durable roster prefix (and therefore its slots), while
 * deterministically ordering only newly discovered suffix members. When
 * hop_counts is non-NULL, move that live per-anchor sidecar with each ID.
 */
int discovery_assignment_order_roster_extension(uint64_t *anchor_ids,
                                                 uint8_t *hop_counts,
                                                 size_t anchor_count,
                                                 size_t prior_anchor_count);
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
int discovery_assignment_parse_result_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_result *result);
/*
 * TABLE entries are authoritative explicit slot mappings. Anchor IDs and
 * slots must each be unique and hash-valid, but slots may contain gaps and
 * the entry list need not be in global hash order because a durable prefix
 * keeps its existing slots while a newly discovered suffix is sorted
 * independently.
 */
int discovery_assignment_append_table_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_entry *entries,
    size_t entry_count);
/*
 * Encode the gateway's existing parallel roster arrays directly. This keeps
 * the full 50-entry scratch table off the constrained gateway workqueue stack
 * while producing the same wire bytes as discovery_assignment_append_table_tlvs().
 */
int discovery_assignment_append_table_from_roster(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint64_t *anchor_ids,
    const uint8_t *anchor_slots,
    size_t anchor_count);
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
/*
 * Stable assignment slots are durable identities and may be sparse.  Timing
 * must instead use a compact rank across the entries that are present in this
 * TABLE, otherwise a retained slot such as 49 creates 49 empty RF windows.
 */
int discovery_assignment_response_lane(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint64_t anchor_id,
    uint8_t *lane,
    uint8_t *lane_count);
bool discovery_assignment_table_commitment(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t slot_count,
    struct discovery_assignment_table_commitment *commitment);
bool discovery_assignment_table_commitment_from_roster(
    const uint64_t *anchor_ids,
    const uint8_t *anchor_slots,
    size_t anchor_count,
    uint8_t slot_count,
    struct discovery_assignment_table_commitment *commitment);
bool discovery_assignment_table_commitment_equal(
    const struct discovery_assignment_table_commitment *left,
    const struct discovery_assignment_table_commitment *right);
int discovery_assignment_append_table_commitment(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_table_commitment *commitment);
int discovery_assignment_append_end_identity(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_end_identity *identity);
int discovery_assignment_extract_end_identity(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_end_identity *identity);
int discovery_assignment_append_abort_identity(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_abort_identity *identity);
int discovery_assignment_extract_abort_identity(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_abort_identity *identity);
int discovery_assignment_response_delay_ms(uint8_t slot,
                                           uint8_t slot_count,
                                           uint8_t hop_count,
                                           uint16_t response_spread_ms,
                                           uint8_t retry_round,
                                           uint32_t random_value,
                                           uint32_t *delay_ms);
uint32_t discovery_assignment_retry_backoff_ms(uint8_t retry_round,
                                               uint32_t random_value);
uint32_t discovery_assignment_response_custody_ms(uint8_t hop_count);
uint32_t discovery_assignment_control_propagation_hold_ms(
    uint8_t max_hop_count);
uint32_t discovery_assignment_activation_propagation_hold_ms(
    uint8_t max_hop_count);
/* One-based RF depth learned from the selected gateway route. Zero uses the
 * maximum depth so a missing route cannot shorten the receive window. */
uint32_t discovery_assignment_control_listener_duration_ms(
    uint8_t gateway_hop_count);
int discovery_assignment_adaptive_depth_deadline_offset_ms(
    uint16_t response_spread_ms,
    uint8_t slot_count,
    uint8_t observed_hop_count,
    uint8_t max_hop_count,
    uint32_t *deadline_offset_ms);
uint64_t discovery_assignment_response_deadline_ms(uint64_t now_ms,
                                                   uint32_t response_delay_ms,
                                                   uint8_t hop_count);
uint16_t discovery_assignment_membership_epoch(uint32_t assignment_epoch);
uint32_t discovery_assignment_collection_window_ms(uint16_t response_spread_ms,
                                                   uint8_t max_hop_count);
uint32_t discovery_assignment_collection_window_for_topology_ms(
    uint16_t response_spread_ms,
    uint8_t slot_count,
    uint8_t max_hop_count);
uint32_t discovery_assignment_table_collection_window_ms(
    uint16_t response_spread_ms,
    uint8_t max_hop_count);
uint32_t discovery_assignment_table_collection_window_for_topology_ms(
    uint16_t response_spread_ms,
    uint8_t slot_count,
    uint8_t max_hop_count);
uint64_t discovery_assignment_control_flood_deadline_ms(
    uint64_t now_ms,
    uint64_t operation_deadline_ms);
uint64_t discovery_assignment_response_ack_settle_deadline_ms(uint64_t now_ms);
bool discovery_assignment_ack_quorum_settle_should_arm(
    bool settle_armed,
    uint8_t missing_ack_count);
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
    uint32_t incoming_epoch,
    enum discovery_assignment_phase incoming_phase);

#ifdef __cplusplus
}
#endif

#endif
