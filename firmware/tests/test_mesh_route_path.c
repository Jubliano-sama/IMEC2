#include "mesh_route_path.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_three_node_cycle_is_rejected(void)
{
    const uint64_t gateway = 0x1001u;
    const uint64_t relay_a = 0x2002u;
    const uint64_t relay_b = 0x3003u;
    const uint64_t child = 0x4004u;
    struct mesh_route_path candidate_path = {
        .node_ids = {gateway, relay_a, relay_b},
        .count = 3u,
    };
    const struct mesh_route_path child_request_path = {
        .node_ids = {child, relay_a},
        .count = 2u,
    };

    assert(mesh_route_path_validate(&candidate_path,
                                    gateway,
                                    relay_b) == PROTO_OK);
    assert(mesh_route_path_append(&candidate_path,
                                  relay_a) == PROTO_ERR_STALE);
    assert(candidate_path.count == 3u);
    assert(mesh_route_paths_intersect(&candidate_path,
                                      &child_request_path));
}

static void test_wire_round_trip_at_max_depth(void)
{
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    struct mesh_route_path decoded;
    struct mesh_route_path path = {0};
    size_t payload_len = 0u;

    for (uint8_t i = 0u; i < MESH_ROUTE_PATH_MAX_NODES; i++) {
        path.node_ids[i] = 0x1000000000000000ull + i;
    }
    path.count = MESH_ROUTE_PATH_MAX_NODES;

    assert(mesh_route_path_append_tlv(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      &path) == PROTO_OK);
    assert(payload_len == MESH_ROUTE_PATH_MAX_TLV_BYTES);
    assert(payload_len <= UWB_MESH_MAX_PAYLOAD_LEN);
    assert(mesh_route_path_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_OK);
    assert(decoded.count == MESH_ROUTE_PATH_MAX_NODES);
    assert(memcmp(decoded.node_ids,
                  path.node_ids,
                  sizeof(path.node_ids)) == 0);
    assert(mesh_route_path_append(&decoded, 0x9999u) == PROTO_ERR_NO_SPACE);
}

static void test_malformed_paths_fail_closed(void)
{
    uint8_t payload[2u + (3u * sizeof(uint64_t))];
    struct mesh_route_path path;
    size_t payload_len = 0u;
    uint8_t duplicate_nodes[3u * sizeof(uint64_t)];

    proto_put_u64_le(&duplicate_nodes[0u], 0x10u);
    proto_put_u64_le(&duplicate_nodes[sizeof(uint64_t)], 0x20u);
    proto_put_u64_le(&duplicate_nodes[2u * sizeof(uint64_t)], 0x10u);

    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_ROUTE_NODE_PATH,
                            duplicate_nodes,
                            sizeof(duplicate_nodes)) == PROTO_OK);
    assert(mesh_route_path_from_tlvs(payload,
                                     payload_len,
                                     &path) == PROTO_ERR_MALFORMED);

    payload[1] = 7u;
    assert(mesh_route_path_from_tlvs(payload,
                                     payload_len,
                                     &path) == PROTO_ERR_MALFORMED);
}

static void test_duplicate_path_tlv_is_rejected(void)
{
    uint8_t payload[2u * (PROTO_TLV_HEADER_LEN + sizeof(uint64_t))];
    uint8_t node_id[sizeof(uint64_t)];
    struct mesh_route_path path;
    size_t payload_len = 0u;

    proto_put_u64_le(node_id, 0x1234u);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_ROUTE_NODE_PATH,
                            node_id,
                            sizeof(node_id)) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_ROUTE_NODE_PATH,
                            node_id,
                            sizeof(node_id)) == PROTO_OK);
    assert(mesh_route_path_from_tlvs(payload,
                                     payload_len,
                                     &path) == PROTO_ERR_MALFORMED);
}

static void test_route_control_wire_bounds(void)
{
    assert(MESH_ROUTE_DISCOVERY_MIN_PAYLOAD_LEN == 72u);
    assert(MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN == 139u);
    assert(MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN == 198u);
    assert(MESH_GATEWAY_ROUTE_ADV_LEGACY_MAX_PAYLOAD_LEN == 138u);
    assert(MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN == 179u);
    assert(MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN == 34u);

    assert(mesh_route_control_payload_len_valid(
        MSG_ROUTE_REQ, MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN));
    assert(!mesh_route_control_payload_len_valid(
        MSG_ROUTE_REQ, MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN + 1u));
    assert(mesh_route_control_payload_len_valid(
        MSG_ROUTE_REPLY, MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN));
    assert(!mesh_route_control_payload_len_valid(
        MSG_ROUTE_REPLY, MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN + 1u));
    assert(mesh_route_control_payload_len_valid(
        MSG_GATEWAY_ROUTE_ADV, MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN));
    assert(!mesh_route_control_payload_len_valid(
        MSG_GATEWAY_ROUTE_ADV,
        MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN + 1u));
    assert(mesh_route_control_payload_len_valid(
        MSG_ROUTE_REPLY_ACK, MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN));
    assert(!mesh_route_control_payload_len_valid(
        MSG_ROUTE_REPLY_ACK, MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN + 1u));
    assert(!mesh_route_control_payload_len_valid(MSG_MESH_DATA, 1u));
}

static void test_route_control_ttl_admission(void)
{
    static const uint8_t request_origin_ttls[] = {1u, 2u, 4u, 6u};

    for (size_t i = 0u;
         i < sizeof(request_origin_ttls) / sizeof(request_origin_ttls[0]);
         i++) {
        const uint8_t origin_ttl = request_origin_ttls[i];

        for (uint8_t hop_count = 0u; hop_count < origin_ttl; hop_count++) {
            assert(mesh_route_control_ttl_validate(
                       MSG_ROUTE_REQ,
                       (uint8_t)(origin_ttl - hop_count),
                       hop_count) == PROTO_OK);
        }
    }
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REQ, 0u, 0u) ==
           PROTO_ERR_STALE);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REQ, 3u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REQ, 6u, 1u) ==
           PROTO_ERR_MALFORMED);

    assert(mesh_route_control_ttl_validate(MSG_GATEWAY_ROUTE_ADV, 8u, 0u) ==
           PROTO_OK);
    assert(mesh_route_control_ttl_validate(MSG_GATEWAY_ROUTE_ADV, 1u, 7u) ==
           PROTO_OK);
    assert(mesh_route_control_ttl_validate(MSG_GATEWAY_ROUTE_ADV, 7u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_route_control_ttl_validate(MSG_GATEWAY_ROUTE_ADV, 0u, 8u) ==
           PROTO_ERR_STALE);

    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REPLY, 1u, 8u) ==
           PROTO_OK);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REPLY, 8u, 0u) ==
           PROTO_OK);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REPLY, 9u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REPLY_ACK, 1u, 0u) ==
           PROTO_OK);
    assert(mesh_route_control_ttl_validate(MSG_ROUTE_REPLY_ACK, 2u, 0u) ==
           PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_three_node_cycle_is_rejected();
    test_wire_round_trip_at_max_depth();
    test_malformed_paths_fail_closed();
    test_duplicate_path_tlv_is_rejected();
    test_route_control_wire_bounds();
    test_route_control_ttl_admission();
    puts("mesh route path tests passed");
    return 0;
}
