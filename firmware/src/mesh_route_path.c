#include "mesh_route_path.h"

#include "protocol.h"

#include <string.h>

_Static_assert(MESH_ROUTE_PATH_MAX_NODES > 0u,
               "route path must retain at least one node");
_Static_assert(MESH_ROUTE_PATH_MAX_VALUE_BYTES <= UINT8_MAX,
               "route path TLV length must fit the wire length field");
_Static_assert(MESH_ROUTE_PATH_MAX_NODES <= 9u,
               "route path walk must remain bounded to nine nodes");
_Static_assert(MESH_ROUTE_DISCOVERY_MIN_PAYLOAD_LEN == 72u,
               "minimum route request must include one exact path node");
_Static_assert(MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN == 139u,
               "route-request payload bound changed");
_Static_assert(MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN == 164u,
               "route-reply payload bound changed");
_Static_assert(MESH_GATEWAY_ROUTE_ADV_LEGACY_MAX_PAYLOAD_LEN == 138u,
               "legacy gateway-route advertisement payload bound changed");
_Static_assert(MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN == 179u,
               "policy gateway-route advertisement payload bound changed");
_Static_assert(MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN == 34u,
               "route-reply ACK payload bound changed");
_Static_assert(MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN <= PACKET_MAX_PAYLOAD_LEN &&
               MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN <= PACKET_MAX_PAYLOAD_LEN &&
               MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN <=
                   PACKET_MAX_PAYLOAD_LEN &&
               MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN <= PACKET_MAX_PAYLOAD_LEN,
               "route controls must fit the bounded short protocol payload");

static bool route_path_id_is_unicast(uint64_t node_id)
{
    return node_id != 0u;
}

bool mesh_route_path_contains(const struct mesh_route_path *path,
                              uint64_t node_id)
{
    if (path == NULL || !route_path_id_is_unicast(node_id) ||
        path->count > MESH_ROUTE_PATH_MAX_NODES) {
        return false;
    }

    for (uint8_t i = 0u; i < path->count; i++) {
        if (path->node_ids[i] == node_id) {
            return true;
        }
    }
    return false;
}

bool mesh_route_paths_intersect(const struct mesh_route_path *first,
                                const struct mesh_route_path *second)
{
    if (first == NULL || second == NULL ||
        first->count == 0u || first->count > MESH_ROUTE_PATH_MAX_NODES ||
        second->count == 0u || second->count > MESH_ROUTE_PATH_MAX_NODES) {
        return false;
    }

    for (uint8_t i = 0u; i < first->count; i++) {
        if (mesh_route_path_contains(second, first->node_ids[i])) {
            return true;
        }
    }
    return false;
}

int mesh_route_path_validate(const struct mesh_route_path *path,
                             uint64_t expected_root_id,
                             uint64_t expected_transmitter_id)
{
    if (path == NULL || path->count == 0u ||
        path->count > MESH_ROUTE_PATH_MAX_NODES ||
        !route_path_id_is_unicast(expected_root_id) ||
        !route_path_id_is_unicast(expected_transmitter_id) ||
        path->node_ids[0] != expected_root_id ||
        path->node_ids[path->count - 1u] != expected_transmitter_id) {
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < path->count; i++) {
        if (!route_path_id_is_unicast(path->node_ids[i])) {
            return PROTO_ERR_MALFORMED;
        }
        for (uint8_t j = (uint8_t)(i + 1u); j < path->count; j++) {
            if (path->node_ids[i] == path->node_ids[j]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    return PROTO_OK;
}

int mesh_route_path_append(struct mesh_route_path *path, uint64_t node_id)
{
    if (path == NULL || !route_path_id_is_unicast(node_id)) {
        return PROTO_ERR_ARG;
    }
    if (path->count == 0u || path->count > MESH_ROUTE_PATH_MAX_NODES) {
        return PROTO_ERR_MALFORMED;
    }
    if (mesh_route_path_contains(path, node_id)) {
        return PROTO_ERR_STALE;
    }
    if (path->count == MESH_ROUTE_PATH_MAX_NODES) {
        return PROTO_ERR_NO_SPACE;
    }

    path->node_ids[path->count] = node_id;
    path->count++;
    return PROTO_OK;
}

int mesh_route_path_append_tlv(uint8_t *payload,
                               size_t payload_cap,
                               size_t *offset,
                               const struct mesh_route_path *path)
{
    uint8_t encoded[MESH_ROUTE_PATH_MAX_VALUE_BYTES];
    size_t encoded_len;
    int ret;

    if (payload == NULL || offset == NULL || path == NULL ||
        path->count == 0u || path->count > MESH_ROUTE_PATH_MAX_NODES) {
        return PROTO_ERR_ARG;
    }

    ret = mesh_route_path_validate(path,
                                   path->node_ids[0],
                                   path->node_ids[path->count - 1u]);
    if (ret != PROTO_OK) {
        return ret;
    }

    encoded_len = (size_t)path->count * sizeof(uint64_t);
    for (uint8_t i = 0u; i < path->count; i++) {
        proto_put_u64_le(&encoded[(size_t)i * sizeof(uint64_t)],
                         path->node_ids[i]);
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_ROUTE_NODE_PATH,
                            encoded,
                            (uint8_t)encoded_len);
}

int mesh_route_path_from_tlvs(const uint8_t *payload,
                              size_t payload_len,
                              struct mesh_route_path *path)
{
    const uint8_t *path_value = NULL;
    uint8_t path_len = 0u;
    size_t offset = 0u;
    bool found = false;

    if (payload == NULL || path == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (payload_len - offset < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_ROUTE_NODE_PATH) {
            if (found) {
                return PROTO_ERR_MALFORMED;
            }
            found = true;
            path_value = &payload[offset];
            path_len = len;
        }
        offset += len;
    }

    if (!found) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (path_len == 0u || path_len > MESH_ROUTE_PATH_MAX_VALUE_BYTES ||
        (path_len % sizeof(uint64_t)) != 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(path, 0, sizeof(*path));
    path->count = (uint8_t)(path_len / sizeof(uint64_t));
    for (uint8_t i = 0u; i < path->count; i++) {
        path->node_ids[i] =
            proto_get_u64_le(&path_value[(size_t)i * sizeof(uint64_t)]);
    }
    return mesh_route_path_validate(path,
                                    path->node_ids[0],
                                    path->node_ids[path->count - 1u]);
}

bool mesh_route_control_payload_len_valid(uint8_t msg_type,
                                          size_t payload_len)
{
    if (payload_len == 0u) {
        return false;
    }

    switch (msg_type) {
    case MSG_ROUTE_REQ:
        return payload_len <= MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN;
    case MSG_ROUTE_REPLY:
        return payload_len <= MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN;
    case MSG_GATEWAY_ROUTE_ADV:
        return payload_len <= MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN;
    case MSG_ROUTE_REPLY_ACK:
        return payload_len <= MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN;
    default:
        return false;
    }
}

int mesh_route_control_ttl_validate(uint8_t msg_type,
                                    uint8_t remaining_ttl,
                                    uint8_t hop_count)
{
    uint16_t original_request_ttl;

    if (remaining_ttl == 0u) {
        return PROTO_ERR_STALE;
    }

    switch (msg_type) {
    case MSG_ROUTE_REQ:
        original_request_ttl = (uint16_t)remaining_ttl + hop_count;
        if (original_request_ttl == 1u || original_request_ttl == 2u ||
            original_request_ttl == 4u || original_request_ttl == 6u) {
            return PROTO_OK;
        }
        return PROTO_ERR_MALFORMED;
    case MSG_GATEWAY_ROUTE_ADV:
        return (uint16_t)remaining_ttl + hop_count ==
                       MESH_NETWORK_MAX_HOPS ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    case MSG_ROUTE_REPLY:
        return remaining_ttl <= MESH_NETWORK_MAX_HOPS ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    case MSG_ROUTE_REPLY_ACK:
        return remaining_ttl == 1u ? PROTO_OK : PROTO_ERR_MALFORMED;
    default:
        return PROTO_ERR_ARG;
    }
}
