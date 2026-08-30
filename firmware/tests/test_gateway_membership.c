#include "gateway_membership.h"

#include <assert.h>
#include <string.h>

static const uint64_t sparse_ids[] = {
    UINT64_C(0x1001),
    UINT64_C(0x2002),
    UINT64_C(0x4004),
};
static const uint8_t sparse_slots[] = {0u, 1u, 3u};
static const struct discovery_assignment_table_commitment
    assignment_commitment = {
        .bytes = {
            0x65u, 0xabu, 0xcdu, 0xefu, 0x01u, 0x23u, 0x45u, 0x67u,
            0x89u, 0xabu, 0xcdu, 0xefu, 0xfeu, 0xdcu, 0xbau, 0x98u,
            0x76u, 0x54u, 0x32u, 0x10u, 0x5au, 0xa5u, 0xc3u, 0x3cu,
            0x96u, 0x69u, 0xf0u, 0x0fu, 0x55u, 0xaau, 0x12u, 0x21u,
        },
    };

static void snapshot_reseal(struct gateway_membership_snapshot *snapshot)
{
    snapshot->checksum = 0u;
    snapshot->checksum =
        proto_crc16_ccitt_false((const uint8_t *)snapshot, sizeof(*snapshot));
}

static struct gateway_membership_roster sparse_roster(uint16_t epoch)
{
    struct gateway_membership_roster roster = {0};

    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               epoch,
               sparse_ids,
               sparse_slots,
               sizeof(sparse_ids) / sizeof(sparse_ids[0])) == PROTO_OK);
    return roster;
}

static struct gateway_membership_publication pending_publication(void)
{
    struct gateway_membership_publication publication = {0};

    publication.claimed_node_ids[0] = sparse_ids[0];
    publication.claimed_node_ids[1] = sparse_ids[1];
    publication.claimed_node_ids[3] = sparse_ids[2];
    publication.claimed_node_ids[4] = UINT64_C(0x5005);
    publication.host_command = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = UINT64_C(0xabc),
        .dst_id = 0u,
        .session_id = 0u,
        .seq = 0u,
        .ttl = 0u,
        .payload_len = PROTO_TLV_U16_ENCODED_LEN,
        .message_age_ms = 7u,
    };
    publication.committed_mask =
        (UINT64_C(1) << 0) |
        (UINT64_C(1) << 1) |
        (UINT64_C(1) << 3);
    /*
     * Slot 1 is a retained durable member that missed this TABLE round.  Its
     * slot remains committed, but replayed telemetry must report the miss.
     */
    publication.acknowledged_mask =
        (UINT64_C(1) << 0) |
        (UINT64_C(1) << 3);
    publication.command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication.event_gateway_epoch = UINT16_C(0x4321);
    publication.duplicate_count = 9u;
    publication.claimed_count = 4u;
    publication.claimed_slot_span = 5u;
    publication.table_round = 2u;
    publication.publish_pending = true;
    return publication;
}

static bool packet_semantically_equal(const struct proto_packet *left,
                                      const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len &&
           left->message_age_ms == right->message_age_ms;
}

static bool publication_semantically_equal(
    const struct gateway_membership_publication *left,
    const struct gateway_membership_publication *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    for (size_t slot = 0u;
         slot < GATEWAY_MEMBERSHIP_MAX_NODES;
         slot++) {
        if (left->claimed_node_ids[slot] != right->claimed_node_ids[slot]) {
            return false;
        }
    }
    return packet_semantically_equal(&left->host_command,
                                     &right->host_command) &&
           left->committed_mask == right->committed_mask &&
           left->acknowledged_mask == right->acknowledged_mask &&
           left->command_id == right->command_id &&
           left->event_gateway_epoch == right->event_gateway_epoch &&
           left->duplicate_count == right->duplicate_count &&
           left->claimed_count == right->claimed_count &&
           left->claimed_slot_span == right->claimed_slot_span &&
           left->table_round == right->table_round &&
           left->publish_pending == right->publish_pending;
}

static void publication_set_fields_zero(
    struct gateway_membership_publication *publication)
{
    assert(publication != NULL);
    for (size_t slot = 0u;
         slot < GATEWAY_MEMBERSHIP_MAX_NODES;
         slot++) {
        publication->claimed_node_ids[slot] = 0u;
    }
    publication->host_command.msg_type = 0u;
    publication->host_command.flags = 0u;
    publication->host_command.src_id = 0u;
    publication->host_command.dst_id = 0u;
    publication->host_command.session_id = 0u;
    publication->host_command.seq = 0u;
    publication->host_command.ttl = 0u;
    publication->host_command.payload_len = 0u;
    publication->host_command.message_age_ms = 0u;
    publication->committed_mask = 0u;
    publication->acknowledged_mask = 0u;
    publication->command_id = 0u;
    publication->event_gateway_epoch = 0u;
    publication->duplicate_count = 0u;
    publication->claimed_count = 0u;
    publication->claimed_slot_span = 0u;
    publication->table_round = 0u;
    publication->publish_pending = 0u;
}

static struct gateway_membership_snapshot assignment_snapshot(void)
{
    struct gateway_membership_roster roster = sparse_roster(17u);
    struct gateway_membership_publication publication =
        pending_publication();
    struct gateway_membership_snapshot snapshot;

    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               UINT32_C(0x12345678),
               UINT32_C(0x92345678),
               &assignment_commitment,
               &publication,
               &snapshot) == PROTO_OK);
    return snapshot;
}

static struct gateway_membership_snapshot nonpending_assignment_snapshot(
    uint16_t membership_epoch)
{
    struct gateway_membership_roster roster = sparse_roster(membership_epoch);
    struct gateway_membership_snapshot snapshot;

    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               UINT32_C(0x12345678),
               UINT32_C(0x92345678),
               &assignment_commitment,
               NULL,
               &snapshot) == PROTO_OK);
    return snapshot;
}

static void assert_snapshot_rejected(
    const struct gateway_membership_snapshot *snapshot)
{
    struct gateway_membership_roster restored = {0};
    struct gateway_membership_publication publication = {0};

    assert(gateway_membership_restore_snapshot(&restored, snapshot) !=
           PROTO_OK);
    assert(gateway_membership_snapshot_get_publication(
               snapshot, &publication) != PROTO_OK);
}

static void test_live_roster_ram_budget_and_dense_compatibility(void)
{
    struct gateway_membership_roster roster = {0};
    const uint64_t nodes[] = {
        UINT64_C(0x1111),
        UINT64_C(0x2222),
        UINT64_C(0x3333),
    };
    uint64_t exported[3] = {0};
    uint8_t slots[3] = {0};
    size_t count = 0u;

    assert(sizeof(roster) <= 408u);
    assert(gateway_membership_set_roster_preserve_order(
               &roster,
               7u,
               nodes,
               sizeof(nodes) / sizeof(nodes[0])) == PROTO_OK);
    assert(roster.valid);
    assert(roster.node_count == 3u);
    assert(roster.slot_span == 3u);
    assert(memcmp(roster.node_ids, nodes, sizeof(nodes)) == 0);
    assert(gateway_membership_export_node_ids_with_slots(
               &roster, 7u, exported, slots, 3u, &count) == PROTO_OK);
    assert(count == 3u);
    assert(memcmp(exported, nodes, sizeof(nodes)) == 0);
    assert(slots[0] == 0u && slots[1] == 1u && slots[2] == 2u);
}

static void test_set_rejects_zero_duplicate_capacity_and_bad_slots(void)
{
    struct gateway_membership_roster roster = {0};
    const uint64_t with_zero[] = {UINT64_C(0x1111), 0u};
    const uint64_t duplicate[] = {
        UINT64_C(0x1111),
        UINT64_C(0x2222),
        UINT64_C(0x1111),
    };
    const uint64_t valid[] = {UINT64_C(0x1111), UINT64_C(0x2222)};
    const uint8_t duplicate_slots[] = {1u, 1u};
    const uint8_t out_of_range_slots[] = {
        0u,
        GATEWAY_MEMBERSHIP_MAX_NODES,
    };
    uint64_t too_many[GATEWAY_MEMBERSHIP_MAX_NODES + 1u];
    uint8_t too_many_slots[GATEWAY_MEMBERSHIP_MAX_NODES + 1u];

    for (size_t i = 0u;
         i < GATEWAY_MEMBERSHIP_MAX_NODES + 1u;
         i++) {
        too_many[i] = UINT64_C(0x10000) + i;
        too_many_slots[i] = (uint8_t)i;
    }

    assert(gateway_membership_set_roster_preserve_order(
               &roster, 1u, with_zero, 2u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_preserve_order(
               &roster, 1u, duplicate, 3u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_preserve_order(
               &roster, 0u, valid, 2u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_preserve_order(
               &roster,
               1u,
               too_many,
               GATEWAY_MEMBERSHIP_MAX_NODES + 1u) ==
           PROTO_ERR_NO_SPACE);
    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               1u,
               valid,
               duplicate_slots,
               2u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               1u,
               valid,
               out_of_range_slots,
               2u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               1u,
               duplicate,
               (const uint8_t[]){0u, 1u, 2u},
               3u) == PROTO_ERR_MALFORMED);
    assert(gateway_membership_set_roster_explicit_slots(
               &roster,
               1u,
               too_many,
               too_many_slots,
               GATEWAY_MEMBERSHIP_MAX_NODES + 1u) ==
           PROTO_ERR_NO_SPACE);
    assert(!roster.valid);
}

static void test_sparse_gap_exports_reset_and_extension(void)
{
    struct gateway_membership_roster roster = sparse_roster(9u);
    struct gateway_membership_roster restored = {0};
    struct gateway_membership_roster extended = {0};
    struct gateway_membership_snapshot snapshot =
        nonpending_assignment_snapshot(9u);
    uint64_t dense[4] = {0};
    uint64_t explicit_ids[4] = {0};
    uint8_t explicit_slots[4] = {0};
    size_t count = 0u;

    assert(roster.node_count == 3u);
    assert(roster.slot_span == 4u);
    assert(roster.node_ids[2] == 0u);
    assert(gateway_membership_export_node_ids_preserve_order(
               &roster, 9u, dense, 4u, &count) == PROTO_OK);
    assert(count == 3u);
    assert(memcmp(dense, sparse_ids, sizeof(sparse_ids)) == 0);

    gateway_membership_clear(&roster);
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) ==
           PROTO_OK);
    assert(restored.node_count == 3u);
    assert(restored.slot_span == 4u);
    assert(restored.node_ids[2] == 0u);

    assert(gateway_membership_export_node_ids_with_slots(
               &restored,
               9u,
               explicit_ids,
               explicit_slots,
               4u,
               &count) == PROTO_OK);
    assert(count == 3u);
    assert(memcmp(explicit_ids, sparse_ids, sizeof(sparse_ids)) == 0);
    assert(memcmp(explicit_slots, sparse_slots, sizeof(sparse_slots)) == 0);

    explicit_ids[count] = UINT64_C(0x5005);
    explicit_slots[count] = 4u;
    count++;
    assert(gateway_membership_set_roster_explicit_slots(
               &extended,
               10u,
               explicit_ids,
               explicit_slots,
               count) == PROTO_OK);
    assert(extended.node_count == 4u);
    assert(extended.slot_span == 5u);
    assert(extended.node_ids[2] == 0u);
    assert(extended.node_ids[3] == sparse_ids[2]);
    assert(extended.node_ids[4] == UINT64_C(0x5005));
}

static void test_exports_report_required_capacity(void)
{
    struct gateway_membership_roster roster = sparse_roster(3u);
    uint64_t ids[2] = {0};
    uint8_t slots[2] = {0};
    size_t count = 0u;

    assert(gateway_membership_export_node_ids_preserve_order(
               &roster, 3u, ids, 2u, &count) == PROTO_ERR_NO_SPACE);
    assert(count == 3u);
    assert(gateway_membership_export_node_ids_with_slots(
               &roster, 3u, ids, slots, 2u, &count) ==
           PROTO_ERR_NO_SPACE);
    assert(count == 3u);
}

static void test_pending_publication_exact_roundtrip(void)
{
    struct gateway_membership_snapshot snapshot = assignment_snapshot();
    struct gateway_membership_roster restored = {0};
    struct gateway_membership_publication publication = {0};
    struct gateway_membership_publication expected =
        pending_publication();

    assert(snapshot.version == GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION);
    assert(snapshot.magic == GATEWAY_MEMBERSHIP_SNAPSHOT_MAGIC);
    assert(snapshot.assignment_proof_valid == 1u);
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) ==
           PROTO_OK);
    assert(restored.node_count == 3u);
    assert(restored.slot_span == 4u);
    assert(restored.node_ids[2] == 0u);
    assert(gateway_membership_snapshot_get_publication(
               &snapshot, &publication) == PROTO_OK);
    assert(publication_semantically_equal(&publication, &expected));
}

static void test_nonpending_assignment_snapshot_has_no_publication(void)
{
    struct gateway_membership_roster roster = sparse_roster(4u);
    struct gateway_membership_snapshot snapshot =
        nonpending_assignment_snapshot(4u);
    struct gateway_membership_publication publication =
        pending_publication();
    struct gateway_membership_publication restored;

    memset(&restored, 0xA5, sizeof(restored));
    assert(gateway_membership_snapshot_get_publication(
               &snapshot, &restored) == PROTO_ERR_NOT_FOUND);
    assert(!restored.publish_pending);
    assert(publication_semantically_equal(
        &restored, &(struct gateway_membership_publication){0}));

    publication_set_fields_zero(&publication);
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               1u,
               2u,
               &assignment_commitment,
               &publication,
               &snapshot) ==
           PROTO_OK);
    assert(gateway_membership_snapshot_get_publication(
               &snapshot, &restored) == PROTO_ERR_NOT_FOUND);
}

static void test_fieldwise_zero_publication_ignores_padding(void)
{
    struct gateway_membership_roster roster = sparse_roster(4u);
    struct gateway_membership_publication publication;
    struct gateway_membership_publication restored;
    struct gateway_membership_snapshot snapshot;

    /*
     * Member assignments leave any ABI padding at 0xa5.  A semantic zero check
     * must accept this object without depending on compiler-specific padding
     * bytes, and the exported snapshot must contain no publication debt.
     */
    memset(&publication, 0xa5, sizeof(publication));
    publication_set_fields_zero(&publication);
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               1u,
               2u,
               &assignment_commitment,
               &publication,
               &snapshot) ==
           PROTO_OK);
    assert(gateway_membership_snapshot_get_publication(
               &snapshot, &restored) == PROTO_ERR_NOT_FOUND);
    assert(publication_semantically_equal(
        &restored, &(struct gateway_membership_publication){0}));
}

static void test_snapshot_roster_and_proof_corruption_rejected(void)
{
    struct gateway_membership_snapshot valid = assignment_snapshot();
    struct gateway_membership_snapshot corrupt;

    corrupt = valid;
    corrupt.checksum ^= 1u;
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.version++;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.magic ^= 1u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.valid = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.valid = 2u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.membership_epoch = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.node_count--;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.slot_span = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.node_ids[3] = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.node_ids[2] = corrupt.node_ids[1];
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.node_ids[4] = UINT64_C(0x9999);
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.assignment_proof_valid = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.assignment_epoch = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);

    corrupt = valid;
    corrupt.publication_table_round = 0u;
    snapshot_reseal(&corrupt);
    assert_snapshot_rejected(&corrupt);
}

static void test_pending_publication_corruption_rejected(void)
{
    struct gateway_membership_snapshot valid = assignment_snapshot();
    struct gateway_membership_snapshot corrupt;

#define ASSERT_PUBLICATION_CORRUPTION_REJECTED(statement) do { \
        corrupt = valid;                                        \
        statement;                                              \
        snapshot_reseal(&corrupt);                              \
        assert_snapshot_rejected(&corrupt);                     \
    } while (false)

    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.assignment_proof_valid = 0u;
        corrupt.assignment_epoch = 0u;
        corrupt.assignment_table_seq = 0u;
        memset(&corrupt.assignment_table_commitment,
               0,
               sizeof(corrupt.assignment_table_commitment)));
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_table_round = 0u);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_host_payload_len = 0u);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_host_payload_len =
            PACKET_MAX_PAYLOAD_LEN + 1u);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_claimed_node_ids[1] =
            corrupt.publication_claimed_node_ids[0]);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_acknowledged_mask = 0u);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_acknowledged_mask |= UINT64_C(1) << 50);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_acknowledged_mask |= UINT64_C(1) << 2);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.publication_acknowledged_mask |= UINT64_C(1) << 4);
    ASSERT_PUBLICATION_CORRUPTION_REJECTED(
        corrupt.node_ids[3] = corrupt.publication_claimed_node_ids[4]);

#undef ASSERT_PUBLICATION_CORRUPTION_REJECTED
}

static void test_exact_identity_fields_are_checksum_bound(void)
{
    struct gateway_membership_snapshot valid = assignment_snapshot();
    struct gateway_membership_snapshot corrupt;

#define ASSERT_EXACT_FIELD_CHECKSUM_BOUND(statement) do { \
        corrupt = valid;                                  \
        statement;                                        \
        assert_snapshot_rejected(&corrupt);               \
    } while (false)

    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(corrupt.membership_epoch ^= 1u);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(corrupt.assignment_epoch ^= 1u);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.assignment_table_seq ^= 1u);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.assignment_table_commitment.bytes[0] ^= 1u);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_claimed_node_ids[4] ^= UINT64_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_acknowledged_mask |= UINT64_C(1) << 1);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_flags ^= FLAG_ERROR);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_src_id ^= UINT64_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_dst_id ^= UINT64_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_session_id ^= UINT32_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_seq ^= UINT16_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_ttl ^= UINT8_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_payload_len++);
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_host_message_age_ms ^= UINT32_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_event_gateway_epoch ^= UINT16_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(
        corrupt.publication_duplicate_count ^= UINT16_C(1));
    ASSERT_EXACT_FIELD_CHECKSUM_BOUND(corrupt.publication_table_round++);

#undef ASSERT_EXACT_FIELD_CHECKSUM_BOUND
}

static void test_invalid_pending_publication_rejected_at_export(void)
{
    struct gateway_membership_roster roster = sparse_roster(17u);
    struct gateway_membership_publication publication =
        pending_publication();
    const struct discovery_assignment_table_commitment zero_commitment = {0};
    struct gateway_membership_snapshot snapshot;

    publication.committed_mask |= UINT64_C(1) << 2;
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               1u,
               2u,
               &assignment_commitment,
               &publication,
               &snapshot) ==
           PROTO_ERR_MALFORMED);
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               1u,
               2u,
               &zero_commitment,
               NULL,
               &snapshot) ==
           PROTO_ERR_ARG);
}

static void test_propagation_pending_table_roundtrips_and_retires(void)
{
    struct gateway_membership_roster roster = sparse_roster(17u);
    struct gateway_membership_publication publication =
        pending_publication();
    struct gateway_membership_publication restored_publication = {0};
    struct gateway_membership_snapshot snapshot;
    struct gateway_membership_snapshot decoded;
    struct gateway_membership_snapshot retired;
    uint8_t wire[GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE];

    publication.table_round =
        GATEWAY_MEMBERSHIP_TABLE_PROPAGATION_PENDING | 1u;
    assert(gateway_membership_export_assignment_snapshot(
               &roster,
               UINT32_C(0x12345678),
               UINT32_C(0x92345678),
               &assignment_commitment,
               &publication,
               &snapshot) == PROTO_OK);
    assert(gateway_membership_snapshot_encode(
               &snapshot, wire, sizeof(wire)) == PROTO_OK);
    assert(gateway_membership_snapshot_decode(
               wire, sizeof(wire), &decoded) == PROTO_OK);
    assert(gateway_membership_snapshot_get_publication(
               &decoded, &restored_publication) == PROTO_OK);
    assert(restored_publication.table_round == publication.table_round);
    assert(restored_publication.acknowledged_mask ==
           publication.acknowledged_mask);
    assert(gateway_membership_snapshot_retire_publication(
               &decoded, &retired) == PROTO_OK);
    assert(retired.assignment_proof_valid == 1u);
    assert(gateway_membership_snapshot_get_publication(
               &retired, &restored_publication) == PROTO_ERR_NOT_FOUND);
}

int main(void)
{
    test_live_roster_ram_budget_and_dense_compatibility();
    test_set_rejects_zero_duplicate_capacity_and_bad_slots();
    test_sparse_gap_exports_reset_and_extension();
    test_exports_report_required_capacity();
    test_pending_publication_exact_roundtrip();
    test_nonpending_assignment_snapshot_has_no_publication();
    test_fieldwise_zero_publication_ignores_padding();
    test_snapshot_roster_and_proof_corruption_rejected();
    test_pending_publication_corruption_rejected();
    test_exact_identity_fields_are_checksum_bound();
    test_invalid_pending_publication_rejected_at_export();
    test_propagation_pending_table_roundtrips_and_retires();
    return 0;
}
