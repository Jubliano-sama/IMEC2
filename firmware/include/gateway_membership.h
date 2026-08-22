#ifndef GATEWAY_MEMBERSHIP_H
#define GATEWAY_MEMBERSHIP_H

#include "discovery_assignment.h"
#include "gateway_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_MEMBERSHIP_MAX_NODES GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP
#define GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION 4u
#define GATEWAY_MEMBERSHIP_SNAPSHOT_MAGIC UINT32_C(0x474D5334)
/*
 * Durable state must never copy this ABI-sensitive structure directly to
 * flash.  This versioned byte codec covers only its semantic fields and
 * reconstructs the in-memory checksum on restore.
 */
#define GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_VERSION 1u
#define GATEWAY_MEMBERSHIP_TABLE_CONFIRM_PENDING UINT8_C(0x80)
#define GATEWAY_MEMBERSHIP_TABLE_ROUND_MASK UINT8_C(0x7f)
#define GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE \
    (2u * GATEWAY_MEMBERSHIP_MAX_NODES * sizeof(uint64_t) + \
     sizeof(struct discovery_assignment_table_commitment) + \
     3u * sizeof(uint64_t) + 4u * sizeof(uint32_t) + \
     5u * sizeof(uint16_t) + 8u)

struct gateway_membership_roster {
    /*
     * Node IDs are indexed by their stable logical slot.  A zero entry is an
     * unassigned gap and must never be compacted when the roster is restored.
     */
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint16_t membership_epoch;
    uint8_t node_count;
    uint8_t slot_span;
    bool valid;
};

struct gateway_membership_publication {
    /* Sparse TABLE roster indexed by logical slot, including failed entries. */
    uint64_t claimed_node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    struct proto_packet host_command;
    /* Durable membership may retain a prior member that missed this round. */
    uint64_t committed_mask;
    /*
     * Host-visible TABLE success mask.  Schema-4 keeps the historical field
     * name on flash, but current publication success follows completion of
     * the planned immutable flood and does not require per-anchor responses.
     */
    uint64_t acknowledged_mask;
    uint16_t command_id;
    uint16_t event_gateway_epoch;
    uint16_t duplicate_count;
    uint8_t claimed_count;
    uint8_t claimed_slot_span;
    uint8_t table_round;
    uint8_t publish_pending;
};

struct gateway_membership_snapshot {
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    /*
     * Publication state is stored canonically instead of embedding the live
     * structure.  Roster-derived masks/counts and constant command fields are
     * reconstructed on restore, keeping the SHA-256 proof within the existing
     * gateway NVS budget.
     */
    uint64_t publication_claimed_node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    struct discovery_assignment_table_commitment assignment_table_commitment;
    uint64_t publication_host_src_id;
    uint64_t publication_host_dst_id;
    uint64_t publication_acknowledged_mask;
    uint32_t assignment_epoch;
    uint32_t assignment_table_seq;
    uint32_t magic;
    uint32_t publication_host_session_id;
    uint32_t publication_host_message_age_ms;
    uint16_t membership_epoch;
    uint16_t checksum;
    uint16_t publication_host_seq;
    uint16_t publication_host_payload_len;
    uint16_t publication_event_gateway_epoch;
    uint16_t publication_duplicate_count;
    uint8_t version;
    uint8_t node_count;
    uint8_t slot_span;
    uint8_t valid;
    uint8_t assignment_proof_valid;
    uint8_t publication_host_flags;
    uint8_t publication_host_ttl;
    /* The high bit means the exact RF TABLE still needs anchor confirmation;
     * the low seven bits retain the host-visible publication attempt. */
    uint8_t publication_table_round;
};

_Static_assert(sizeof(struct gateway_membership_roster) <= 408u,
               "live gateway membership roster must stay within its RAM budget");
_Static_assert(
    sizeof(struct gateway_membership_snapshot) == 896u &&
    offsetof(struct gateway_membership_snapshot,
             publication_claimed_node_ids) == 400u &&
    offsetof(struct gateway_membership_snapshot,
             assignment_table_commitment) == 800u &&
    offsetof(struct gateway_membership_snapshot, assignment_epoch) == 856u &&
    offsetof(struct gateway_membership_snapshot, version) == 888u,
    "gateway membership schema-4 canonical layout changed");

void gateway_membership_clear(struct gateway_membership_roster *roster);
int gateway_membership_set_roster_preserve_order(struct gateway_membership_roster *roster,
                                                 uint16_t membership_epoch,
                                                 const uint64_t *node_ids,
                                                 size_t node_count);
int gateway_membership_set_roster_explicit_slots(
    struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    const uint64_t *node_ids,
    const uint8_t *slots,
    size_t node_count);
int gateway_membership_export_node_ids_preserve_order(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t *out_node_ids,
    size_t out_cap,
    size_t *out_count);
int gateway_membership_export_node_ids_with_slots(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t *out_node_ids,
    uint8_t *out_slots,
    size_t out_cap,
    size_t *out_count);
int gateway_membership_export_assignment_snapshot(
    const struct gateway_membership_roster *roster,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication,
    struct gateway_membership_snapshot *snapshot);
int gateway_membership_restore_snapshot(
    struct gateway_membership_roster *roster,
    const struct gateway_membership_snapshot *snapshot);
int gateway_membership_snapshot_get_publication(
    const struct gateway_membership_snapshot *snapshot,
    struct gateway_membership_publication *publication);
/*
 * Canonical semantic codec for the durable gateway-assignment journal.  The
 * wire record omits ABI padding plus the derived snapshot magic/checksum, and
 * decode validates every reconstructed field before returning it.
 */
int gateway_membership_snapshot_encode(
    const struct gateway_membership_snapshot *snapshot,
    uint8_t *wire,
    size_t wire_cap);
int gateway_membership_snapshot_decode(
    const uint8_t *wire,
    size_t wire_len,
    struct gateway_membership_snapshot *snapshot);
/* Compare only normalized semantic fields, never ABI padding or checksum. */
bool gateway_membership_snapshot_semantically_equal(
    const struct gateway_membership_snapshot *left,
    const struct gateway_membership_snapshot *right);
/*
 * Produce the post-host-receipt form of a committed assignment.  It retains
 * the sparse roster and assignment proof while removing the only replay debt
 * (the pending host publication).
 */
int gateway_membership_snapshot_retire_publication(
    const struct gateway_membership_snapshot *pending,
    struct gateway_membership_snapshot *retired);

#ifdef __cplusplus
}
#endif

#endif
