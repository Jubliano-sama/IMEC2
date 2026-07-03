#include "gateway_membership.h"

#include <assert.h>
#include <string.h>

static void test_set_rejects_zero_duplicate_and_zero_epoch(void)
{
    struct gateway_membership_roster roster = {0};
    const uint64_t with_zero[] = {0x1111u, 0u};
    const uint64_t with_duplicate[] = {0x1111u, 0x2222u, 0x1111u};
    const uint64_t valid[] = {0x1111u};

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        1u,
                                                        with_zero,
                                                        sizeof(with_zero) /
                                                            sizeof(with_zero[0])) ==
           PROTO_ERR_MALFORMED);
    assert(!roster.valid);

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        1u,
                                                        with_duplicate,
                                                        sizeof(with_duplicate) /
                                                            sizeof(with_duplicate[0])) ==
           PROTO_ERR_MALFORMED);
    assert(!roster.valid);

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        0u,
                                                        valid,
                                                        sizeof(valid) / sizeof(valid[0])) ==
           PROTO_ERR_MALFORMED);
    assert(!roster.valid);
}

static void test_capacity_limit_reuses_gateway_command_capacity(void)
{
    struct gateway_membership_roster roster = {0};
    uint64_t nodes[GATEWAY_MEMBERSHIP_MAX_NODES + 1u];

    for (size_t i = 0u; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        nodes[i] = 0x1000u + (uint64_t)i;
    }

    assert(GATEWAY_MEMBERSHIP_MAX_NODES == GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP);
    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        1u,
                                                        nodes,
                                                        GATEWAY_MEMBERSHIP_MAX_NODES) ==
           PROTO_OK);
    assert(roster.node_count == GATEWAY_MEMBERSHIP_MAX_NODES);

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        2u,
                                                        nodes,
                                                        GATEWAY_MEMBERSHIP_MAX_NODES + 1u) ==
           PROTO_ERR_NO_SPACE);
    assert(roster.membership_epoch == 1u);
    assert(roster.node_count == GATEWAY_MEMBERSHIP_MAX_NODES);
}

static void test_lookup_contains_and_epoch_mismatch(void)
{
    struct gateway_membership_roster roster = {0};
    const uint64_t nodes[] = {0x1111u, 0x2222u, 0x3333u};
    size_t index = 99u;

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        7u,
                                                        nodes,
                                                        sizeof(nodes) / sizeof(nodes[0])) ==
           PROTO_OK);

    assert(gateway_membership_contains_node_id(&roster, 7u, 0x2222u));
    assert(!gateway_membership_contains_node_id(&roster, 7u, 0x4444u));
    assert(!gateway_membership_contains_node_id(&roster, 6u, 0x2222u));

    assert(gateway_membership_lookup_node_index(&roster, 7u, 0x3333u, &index) == PROTO_OK);
    assert(index == 2u);
    assert(gateway_membership_lookup_node_index(&roster, 6u, 0x3333u, &index) ==
           PROTO_ERR_STALE);
    assert(gateway_membership_lookup_node_index(&roster, 7u, 0x4444u, &index) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_export_restore_preserves_order(void)
{
    struct gateway_membership_roster roster = {0};
    struct gateway_membership_roster restored = {0};
    struct gateway_membership_snapshot snapshot;
    const uint64_t nodes[] = {0x3333u, 0x1111u, 0x4444u, 0x2222u};
    uint64_t exported[4];
    size_t exported_count = 0u;

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        9u,
                                                        nodes,
                                                        sizeof(nodes) / sizeof(nodes[0])) ==
           PROTO_OK);

    memset(&snapshot, 0xA5, sizeof(snapshot));
    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);
    assert(snapshot.version == GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION);
    assert(snapshot.valid);
    assert(snapshot.membership_epoch == 9u);
    assert(snapshot.node_count == sizeof(nodes) / sizeof(nodes[0]));
    assert(memcmp(snapshot.node_ids, nodes, sizeof(nodes)) == 0);

    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_OK);
    assert(restored.valid);
    assert(restored.membership_epoch == 9u);
    assert(restored.node_count == sizeof(nodes) / sizeof(nodes[0]));

    assert(gateway_membership_export_node_ids_preserve_order(&restored,
                                                             9u,
                                                             exported,
                                                             sizeof(exported) /
                                                                 sizeof(exported[0]),
                                                             &exported_count) ==
           PROTO_OK);
    assert(exported_count == sizeof(nodes) / sizeof(nodes[0]));
    assert(memcmp(exported, nodes, sizeof(nodes)) == 0);
}

static void test_export_reports_required_capacity(void)
{
    struct gateway_membership_roster roster = {0};
    const uint64_t nodes[] = {0x1111u, 0x2222u, 0x3333u};
    uint64_t exported[2];
    size_t exported_count = 0u;

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        3u,
                                                        nodes,
                                                        sizeof(nodes) / sizeof(nodes[0])) ==
           PROTO_OK);
    assert(gateway_membership_export_node_ids_preserve_order(&roster,
                                                             3u,
                                                             exported,
                                                             sizeof(exported) /
                                                                 sizeof(exported[0]),
                                                             &exported_count) ==
           PROTO_ERR_NO_SPACE);
    assert(exported_count == sizeof(nodes) / sizeof(nodes[0]));
}

static void test_corrupt_snapshot_rejected(void)
{
    struct gateway_membership_roster roster = {0};
    struct gateway_membership_roster restored = {0};
    struct gateway_membership_snapshot snapshot;
    const uint64_t nodes[] = {0x1111u, 0x2222u, 0x3333u};

    assert(gateway_membership_set_roster_preserve_order(&roster,
                                                        5u,
                                                        nodes,
                                                        sizeof(nodes) / sizeof(nodes[0])) ==
           PROTO_OK);
    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);

    snapshot.version = GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION + 1u;
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_ERR_BAD_VERSION);

    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);
    snapshot.valid = false;
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_ERR_MALFORMED);

    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);
    snapshot.membership_epoch = 0u;
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_ERR_MALFORMED);

    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);
    snapshot.node_ids[1] = snapshot.node_ids[0];
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_ERR_MALFORMED);

    assert(gateway_membership_export_snapshot(&roster, &snapshot) == PROTO_OK);
    snapshot.node_ids[2] = 0u;
    assert(gateway_membership_restore_snapshot(&restored, &snapshot) == PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_set_rejects_zero_duplicate_and_zero_epoch();
    test_capacity_limit_reuses_gateway_command_capacity();
    test_lookup_contains_and_epoch_mismatch();
    test_export_restore_preserves_order();
    test_export_reports_required_capacity();
    test_corrupt_snapshot_rejected();
    return 0;
}
