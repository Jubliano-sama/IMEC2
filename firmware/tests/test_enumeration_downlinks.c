#include "enumeration_response_lane.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY UINT64_C(0x9000)
#define ANCHOR_A UINT64_C(0xa101)
#define ANCHOR_B UINT64_C(0xa102)
#define ANCHOR_C UINT64_C(0xa103)
#define ROUTE_EPOCH 37u
#define ENUM_EPOCH 1234u

static void init_anchor(struct mesh_relay *relay, uint64_t id,
                        struct mesh_anchor_downlink_store *store)
{
    mesh_relay_init(relay, MESH_RELAY_ROLE_ANCHOR, id, GATEWAY, ROUTE_EPOCH);
    assert(mesh_relay_attach_anchor_downlink_store(relay, store) == PROTO_OK);
}

static size_t valid_downlinks(const struct mesh_relay *relay)
{
    size_t count = 0u;
    for (size_t index = 0u; index < mesh_relay_downlink_capacity(relay); index++) {
        count += mesh_relay_downlink_at(relay, index)->valid ? 1u : 0u;
    }
    return count;
}

static void assert_path(const struct mesh_relay *relay, uint64_t target,
                        uint64_t child, uint8_t remaining_hops)
{
    const struct mesh_downlink_entry *entry =
        mesh_relay_find_current_downlink(relay, target);
    uint64_t next_hop = 0u;
    assert(entry != NULL && entry->valid);
    assert(entry->target_id == target && entry->next_hop_id == child);
    assert(entry->gateway_id == GATEWAY);
    assert(entry->route_epoch == relay->upstream.current_epoch);
    assert(entry->hop_count == remaining_hops);
    assert(mesh_relay_select_next_hop(relay, target, &next_hop) == PROTO_OK);
    assert(next_hop == child);
}

static void assert_missing(const struct mesh_relay *relay, uint64_t target)
{
    uint64_t next_hop = UINT64_MAX;
    assert(mesh_relay_find_current_downlink(relay, target) == NULL);
    assert(mesh_relay_select_next_hop(relay, target, &next_hop) == PROTO_ERR_NOT_FOUND);
    assert(next_hop == UINT64_MAX);
}

static size_t status_command(struct proto_packet *packet, uint64_t target,
                             uint8_t *payload, size_t cap)
{
    size_t len = 0u;
    assert(mesh_append_command_id(payload, cap, &len, CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(packet, GATEWAY, target, 71u, 3u,
        (uint8_t)len) == PROTO_OK);
    packet->ttl = gateway_command_origin_ttl(CMD_GET_STATUS);
    return len;
}

/* Build and decode the real compact wire bundle, including aggregate IDs
 * learned from descendants. These are explicit test edges, not routes seeded
 * merely because a target ID exists. */
static struct uwb_enumeration_bundle_frame bundle_from_lane(
    struct enumeration_response_lane *lane, uint8_t depth)
{
    struct uwb_enumeration_bundle_frame sent, decoded;
    struct enumeration_response_timing timing = {.depth = depth};
    uint8_t wire[UWB_MESH_MAX_FRAME_LEN];
    size_t wire_len = 0u;
    timing.round = depth == lane->hop_count ? 0u :
        ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
        (depth - lane->hop_count - 1u) * ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP;
    assert(enumeration_response_lane_prepare_round(lane, timing.round, 17u) == PROTO_OK);
    timing.round_offset_ms = enumeration_response_lane_round_offset_ms(lane, 0u);
    assert(enumeration_response_lane_bundle_for_offset(lane, &timing, &sent) == PROTO_OK);
    assert(uwb_encode_enumeration_bundle(&sent, wire, sizeof(wire), &wire_len) == PROTO_OK);
    assert(wire_len == uwb_enumeration_bundle_encoded_len(sent.record_count));
    assert(uwb_decode_enumeration_bundle(wire, wire_len, &decoded) == PROTO_OK);
    return decoded;
}

static void accept_child_bundle(struct mesh_relay *relay,
                                 struct enumeration_response_lane *lane,
                                 const struct uwb_enumeration_bundle_frame *bundle)
{
    bool added = false;
    assert(enumeration_response_lane_merge_bundle(lane, bundle, &added) == PROTO_OK);
    assert(added);
    for (uint8_t index = 0u; index < bundle->record_count; index++) {
        const struct uwb_enumeration_record *record = &bundle->records[index];
        assert(record->hop_count > lane->hop_count);
        assert(mesh_relay_note_enumeration_downlink(relay, record->anchor_id,
            bundle->sender_id, record->hop_count - lane->hop_count, 1000u) == PROTO_OK);
    }
}

static void test_missing_route_has_no_implicit_direct_fallback(void)
{
    struct mesh_relay gateway;
    struct mesh_outbound out;
    struct proto_packet command;
    uint8_t payload[16];
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, ROUTE_EPOCH);
    size_t len = status_command(&command, ANCHOR_C, payload, sizeof(payload));
    assert_missing(&gateway, ANCHOR_C);
    assert(mesh_relay_start_tx(&gateway, &command, payload, len, 1000u, &out) ==
           PROTO_ERR_NOT_FOUND);
    assert(!mesh_relay_tx_active(&gateway) && valid_downlinks(&gateway) == 0u);

    assert(mesh_relay_note_enumeration_downlink(&gateway, ANCHOR_C, ANCHOR_C,
        1u, 1001u) == PROTO_OK);
    assert_path(&gateway, ANCHOR_C, ANCHOR_C, 1u);
    assert(mesh_relay_start_tx(&gateway, &command, payload, len, 1002u, &out) == PROTO_OK);
    assert(out.next_hop_id == ANCHOR_C && out.packet.dst_id == ANCHOR_C);
}

static void test_accepted_three_hop_bundles_support_targeted_command(void)
{
    struct mesh_relay gateway, a, b, c;
    struct mesh_anchor_downlink_store stores[3];
    struct enumeration_response_lane lanes[3];
    struct uwb_enumeration_bundle_frame bundle;
    struct proto_packet command;
    struct mesh_outbound outgoing;
    struct mesh_relay_result result;
    uint8_t payload[16];
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, ROUTE_EPOCH);
    init_anchor(&a, ANCHOR_A, &stores[0]);
    init_anchor(&b, ANCHOR_B, &stores[1]);
    init_anchor(&c, ANCHOR_C, &stores[2]);
    assert(enumeration_response_lane_begin(&lanes[0], 1u, ENUM_EPOCH,
        ANCHOR_A, GATEWAY, 1u, 1000u) == PROTO_OK);
    assert(enumeration_response_lane_begin(&lanes[1], 1u, ENUM_EPOCH,
        ANCHOR_B, ANCHOR_A, 2u, 1000u) == PROTO_OK);
    assert(enumeration_response_lane_begin(&lanes[2], 1u, ENUM_EPOCH,
        ANCHOR_C, ANCHOR_B, 3u, 1000u) == PROTO_OK);
    assert_missing(&a, ANCHOR_C);
    assert_missing(&b, ANCHOR_C);

    bundle = bundle_from_lane(&lanes[2], 3u);
    accept_child_bundle(&b, &lanes[1], &bundle);
    assert_path(&b, ANCHOR_C, ANCHOR_C, 1u);
    assert_missing(&a, ANCHOR_C); /* Hearing at B alone proves nothing at A. */
    bundle = bundle_from_lane(&lanes[1], 3u);
    accept_child_bundle(&a, &lanes[0], &bundle);
    assert_path(&a, ANCHOR_B, ANCHOR_B, 1u);
    assert_path(&a, ANCHOR_C, ANCHOR_B, 2u);
    bundle = bundle_from_lane(&lanes[0], 3u);
    assert(bundle.parent_id == GATEWAY && bundle.sender_id == ANCHOR_A);
    /* Gateway production retains these same observations in its existing
     * enumeration roster. Exercise the public core API's gateway role too. */
    for (uint8_t index = 0u; index < bundle.record_count; index++) {
        assert(mesh_relay_note_enumeration_downlink(&gateway,
            bundle.records[index].anchor_id, bundle.sender_id,
            bundle.records[index].hop_count, 1000u) == PROTO_OK);
    }
    assert_path(&gateway, ANCHOR_C, ANCHOR_A, 3u);

    size_t len = status_command(&command, ANCHOR_C, payload, sizeof(payload));
    assert(mesh_relay_start_tx(&gateway, &command, payload, len, 1001u, &outgoing) == PROTO_OK);
    assert(outgoing.next_hop_id == ANCHOR_A);
    assert(mesh_relay_handle_rx(&a, &outgoing.packet, outgoing.payload,
        outgoing.payload_len, GATEWAY, 90u, 1002u, &result) == PROTO_OK);
    assert(result.status == PROTO_OK && (result.actions & MESH_RELAY_ACTION_FORWARD));
    assert(!(result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(result.forward.next_hop_id == ANCHOR_B && result.forward.packet.dst_id == ANCHOR_C);
    outgoing = result.forward;
    assert(mesh_relay_handle_rx(&b, &outgoing.packet, outgoing.payload,
        outgoing.payload_len, ANCHOR_A, 90u, 1003u, &result) == PROTO_OK);
    assert(result.status == PROTO_OK && (result.actions & MESH_RELAY_ACTION_FORWARD));
    assert(!(result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(result.forward.next_hop_id == ANCHOR_C);
    outgoing = result.forward;
    assert(mesh_relay_handle_rx(&c, &outgoing.packet, outgoing.payload,
        outgoing.payload_len, ANCHOR_B, 90u, 1004u, &result) == PROTO_OK);
    assert(result.status == PROTO_OK && (result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!(result.actions & MESH_RELAY_ACTION_FORWARD));
}

static void test_gateway_cache_replacement_preserves_selected_observed_path(void)
{
    struct mesh_relay gateway;
    const uint64_t first = UINT64_C(0xb000);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, ROUTE_EPOCH);
    assert(mesh_relay_downlink_capacity(&gateway) == 16u);
    for (uint64_t index = 0u; index < MESH_CONNECTED_MAX_ANCHORS; index++) {
        assert(mesh_relay_note_enumeration_downlink(&gateway, first + index,
            ANCHOR_A, 3u, 1000u + (uint32_t)index) == PROTO_OK);
    }
    assert(valid_downlinks(&gateway) == MESH_RELAY_DOWNLINK_ROUTES);
    uint64_t evicted = 0u;
    for (uint64_t index = 0u; index < MESH_CONNECTED_MAX_ANCHORS; index++) {
        if (mesh_relay_find_current_downlink(&gateway, first + index) == NULL) {
            evicted = first + index;
            break;
        }
    }
    assert(evicted != 0u);
    assert_missing(&gateway, evicted);
    /* Selection may retain an older, longer observed path even when the
     * small cache is full of shorter paths; it must retain the actual child. */
    assert(mesh_relay_note_enumeration_downlink(&gateway, evicted,
        ANCHOR_A, 8u, 1100u) == PROTO_OK);
    assert_path(&gateway, evicted, ANCHOR_A, 8u);
    assert(valid_downlinks(&gateway) == MESH_RELAY_DOWNLINK_ROUTES);
}

static void test_anchor_preserves_all_fifty_observed_descendants(void)
{
    struct mesh_relay anchor;
    struct {
        uint64_t before;
        struct mesh_anchor_downlink_store store;
        uint64_t after;
    } guarded = {.before = UINT64_MAX, .after = UINT64_MAX};
    const uint64_t first = UINT64_C(0xc000);
    init_anchor(&anchor, ANCHOR_A, &guarded.store);
    assert(mesh_relay_downlink_capacity(&anchor) == 50u);
    for (uint64_t index = 0u; index < MESH_CONNECTED_MAX_ANCHORS; index++) {
        assert(mesh_relay_note_enumeration_downlink(&anchor, first + index,
            first + index, 1u, 1000u) == PROTO_OK);
    }
    for (uint64_t index = 0u; index < MESH_CONNECTED_MAX_ANCHORS; index++) {
        assert_path(&anchor, first + index, first + index, 1u);
    }
    assert(valid_downlinks(&anchor) == 50u);
    assert(mesh_relay_note_enumeration_downlink(&anchor, first,
        first, 1u, 1100u) == PROTO_OK);
    assert(valid_downlinks(&anchor) == 50u);
    assert(mesh_relay_find_current_downlink(&anchor, first)->last_seen_ms == 1100u);
    assert(guarded.before == UINT64_MAX && guarded.after == UINT64_MAX);
}

static void test_new_enumeration_replaces_an_older_shorter_shortcut(void)
{
    struct mesh_relay anchor;
    struct mesh_anchor_downlink_store store;
    init_anchor(&anchor, ANCHOR_A, &store);
    assert(mesh_relay_note_enumeration_downlink(&anchor, ANCHOR_B,
        ANCHOR_B, 1u, 1000u) == PROTO_OK);
    assert(mesh_relay_note_enumeration_downlink(&anchor, ANCHOR_C,
        ANCHOR_C, 1u, 1000u) == PROTO_OK);
    assert_path(&anchor, ANCHOR_C, ANCHOR_C, 1u);
    assert(mesh_relay_note_enumeration_downlink(&anchor, ANCHOR_C,
        ANCHOR_B, 2u, 1100u) == PROTO_OK);
    assert_path(&anchor, ANCHOR_C, ANCHOR_B, 2u);
    assert_path(&anchor, ANCHOR_B, ANCHOR_B, 1u);
    assert(valid_downlinks(&anchor) == 2u);
    for (size_t index = 0u; index < mesh_relay_downlink_capacity(&anchor); index++) {
        const struct mesh_downlink_entry *entry = mesh_relay_downlink_at(&anchor, index);
        assert(!entry->valid || entry->target_id != ANCHOR_C ||
               entry->next_hop_id == ANCHOR_B);
    }
}

static void assert_rejected_unchanged(struct mesh_relay *relay,
                                      uint64_t target, uint64_t child, uint8_t hops)
{
    unsigned char before[sizeof(*relay)];
    unsigned char overflow[sizeof(struct mesh_anchor_downlink_store)];
    memcpy(before, relay, sizeof(before));
    if (relay->role == MESH_RELAY_ROLE_ANCHOR && relay->anchor_downlink_store) {
        memcpy(overflow, relay->anchor_downlink_store, sizeof(overflow));
    }
    assert(mesh_relay_note_enumeration_downlink(relay, target, child, hops, 2000u) != PROTO_OK);
    /* Compare a byte snapshot of the same object to catch any rejected write,
     * rather than comparing independently initialized structs with padding. */
    assert(memcmp(before, relay, sizeof(before)) == 0);
    if (relay->role == MESH_RELAY_ROLE_ANCHOR && relay->anchor_downlink_store) {
        assert(memcmp(overflow, relay->anchor_downlink_store, sizeof(overflow)) == 0);
    }
}

static void test_invalid_evidence_and_leaf_roles_cannot_publish_routes(void)
{
    struct mesh_relay relay;
    struct mesh_anchor_downlink_store store;
    init_anchor(&relay, ANCHOR_A, &store);
    assert(mesh_relay_note_enumeration_downlink(&relay, ANCHOR_C,
        ANCHOR_B, 2u, 1000u) == PROTO_OK);
    const struct { uint64_t target, child; uint8_t hops; } invalid[] = {
        {0u, ANCHOR_B, 2u}, {ANCHOR_C, 0u, 2u},
        {ANCHOR_A, ANCHOR_B, 2u}, {GATEWAY, ANCHOR_B, 2u},
        {ANCHOR_C, ANCHOR_A, 2u}, {ANCHOR_C, GATEWAY, 2u},
        {ANCHOR_C, ANCHOR_B, 0u}, {ANCHOR_C, ANCHOR_B, 9u},
        {ANCHOR_C, ANCHOR_B, UINT8_MAX},
        {ANCHOR_C, ANCHOR_C, 2u}, {ANCHOR_C, ANCHOR_B, 1u},
    };
    assert(mesh_relay_note_enumeration_downlink(NULL, ANCHOR_C, ANCHOR_B, 2u, 0u) != PROTO_OK);
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        assert_rejected_unchanged(&relay, invalid[index].target, invalid[index].child,
            invalid[index].hops);
    }
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 0u);
    assert_rejected_unchanged(&relay, ANCHOR_C, ANCHOR_B, 2u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_CLICKER, ANCHOR_A, GATEWAY, ROUTE_EPOCH);
    assert_rejected_unchanged(&relay, ANCHOR_C, ANCHOR_C, 1u);
    mesh_relay_init(&relay, (enum mesh_relay_role)0, ANCHOR_A, GATEWAY, ROUTE_EPOCH);
    assert_rejected_unchanged(&relay, ANCHOR_C, ANCHOR_C, 1u);
}

static void test_route_epoch_change_invalidates_enumeration_paths(void)
{
    struct mesh_relay relay;
    struct mesh_anchor_downlink_store store;
    init_anchor(&relay, ANCHOR_A, &store);
    assert(mesh_relay_note_enumeration_downlink(&relay, ANCHOR_C,
        ANCHOR_B, 2u, 1000u) == PROTO_OK);
    mesh_relay_invalidate_routes(&relay);
    assert(relay.upstream.current_epoch == ROUTE_EPOCH + 1u);
    assert_missing(&relay, ANCHOR_C);
    assert(mesh_relay_note_enumeration_downlink(&relay, ANCHOR_C,
        ANCHOR_B, 2u, 1001u) == PROTO_OK);
    const struct route_candidate new_route = {
        .next_hop_id = GATEWAY, .gateway_id = GATEWAY,
        .route_epoch = ROUTE_EPOCH + 2u, .last_seen_ms = 1002u,
        .hop_count = 0u, .link_quality = 90u, .valid = true,
    };
    assert(mesh_relay_upsert_configured_gateway_route(&relay, &new_route) == PROTO_OK);
    assert_missing(&relay, ANCHOR_C);
    assert(valid_downlinks(&relay) == 0u);
}

int main(void)
{
    test_missing_route_has_no_implicit_direct_fallback();
    test_accepted_three_hop_bundles_support_targeted_command();
    test_gateway_cache_replacement_preserves_selected_observed_path();
    test_anchor_preserves_all_fifty_observed_descendants();
    test_new_enumeration_replaces_an_older_shorter_shortcut();
    test_invalid_evidence_and_leaf_roles_cannot_publish_routes();
    test_route_epoch_change_invalidates_enumeration_paths();
    puts("enumeration downlinks: 7 scenarios passed");
    return 0;
}
