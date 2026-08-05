#include "mesh_relay.h"

#include "mesh.h"
#include "route.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define LOCAL_ID UINT64_C(0x1111222233334444)
#define GATEWAY_ID UINT64_C(0x9999888877776666)

struct local_outbox_case {
    enum mesh_relay_role role;
    uint8_t msg_type;
    uint8_t flags;
};

static struct route_candidate direct_gateway_route(void)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 13u,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = 90u,
        .valid = true,
    };
}

static void assert_local_class_survives_snapshot(
    const struct local_outbox_case *test_case,
    uint16_t sequence)
{
    const uint8_t payload[] = {
        test_case->msg_type,
        test_case->flags,
        (uint8_t)sequence,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct route_candidate route = direct_gateway_route();
    struct proto_packet packet = {
        .msg_type = test_case->msg_type,
        .flags = test_case->flags,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x10203040) + sequence,
        .seq = sequence,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(payload),
    };
    struct mesh_outbound tx;
    struct mesh_relay_outbox_snapshot snapshot;

    mesh_relay_init(&relay,
                    test_case->role,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               1000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             1010u,
                                             &snapshot) == PROTO_OK);

    mesh_relay_init(&restored,
                    test_case->role,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              2000u) == PROTO_OK);
    assert(mesh_relay_tx_active(&restored));
    assert(restored.pending.packet.msg_type == packet.msg_type);
    assert(restored.pending.packet.flags == packet.flags);
    assert(restored.pending.packet.src_id == packet.src_id);
    assert(restored.pending.packet.dst_id == packet.dst_id);
    assert(restored.pending.packet.session_id == packet.session_id);
    assert(restored.pending.packet.seq == packet.seq);
    assert(restored.pending.packet.payload_len == packet.payload_len);
    assert(restored.pending.payload_len == sizeof(payload));
    assert(memcmp(restored.pending.payload, payload, sizeof(payload)) == 0);
}

static void test_local_gateway_host_snapshot_class_matrix(void)
{
    static const struct local_outbox_case cases[] = {
        {
            MESH_RELAY_ROLE_CLICKER,
            MSG_SELF_TEST_REPORT,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_CLICK_REPORT,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_SELF_TEST_REPORT,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_ANCHOR_HEARTBEAT,
            FLAG_GATEWAY_ACK_REQUIRED,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_MESH_DATA,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_SURVEY_DISCOVERY_REPORT,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        },
        {
            MESH_RELAY_ROLE_ANCHOR,
            MSG_SURVEY_PAIR_RESULT,
            FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        },
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_local_class_survives_snapshot(&cases[i],
                                             (uint16_t)(i + 1u));
    }
}

static void test_unlisted_local_class_fails_closed(void)
{
    const uint8_t payload[] = {0x41u};
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route();
    struct proto_packet packet = {
        .msg_type = MSG_ERROR,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x55667788),
        .seq = 19u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(payload),
    };
    struct mesh_outbound tx;
    struct mesh_relay_outbox_snapshot snapshot;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &packet,
                               payload,
                               sizeof(payload),
                               1000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             1010u,
                                             &snapshot) ==
           PROTO_ERR_NOT_FOUND);
}

int main(void)
{
    test_local_gateway_host_snapshot_class_matrix();
    test_unlisted_local_class_fails_closed();
    return 0;
}
