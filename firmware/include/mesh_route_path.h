#ifndef MESH_ROUTE_PATH_H
#define MESH_ROUTE_PATH_H

#include "mesh.h"
#include "operation_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Route-control paths name every transmitter from the path root through the
 * current sender. The network limit is also the wire bound, so one malformed
 * frame cannot force an unbounded path walk or retained allocation.
 */
#define MESH_ROUTE_PATH_MAX_NODES (MESH_NETWORK_MAX_HOPS + 1u)
#define MESH_ROUTE_PATH_MAX_VALUE_BYTES \
    (MESH_ROUTE_PATH_MAX_NODES * sizeof(uint64_t))
#define MESH_ROUTE_PATH_MAX_TLV_BYTES \
    (PROTO_TLV_HEADER_LEN + MESH_ROUTE_PATH_MAX_VALUE_BYTES)

/*
 * Route-control messages use a bounded TLV schema even though the generic
 * extended protocol envelope can carry a much larger payload.  Keep these
 * limits beside the path bound so builders and receivers cannot drift apart
 * when the ancestry shape changes.
 */
#define MESH_ROUTE_DISCOVERY_REQUIRED_FIXED_TLV_BYTES \
    ((2u * PROTO_TLV_U64_ENCODED_LEN) + \
     (3u * PROTO_TLV_U32_ENCODED_LEN) + \
     (3u * PROTO_TLV_U16_ENCODED_LEN) + \
     (4u * PROTO_TLV_U8_ENCODED_LEN))
#define MESH_ROUTE_DISCOVERY_MIN_PAYLOAD_LEN \
    (MESH_ROUTE_DISCOVERY_REQUIRED_FIXED_TLV_BYTES + \
     PROTO_TLV_U64_ENCODED_LEN)
#define MESH_ROUTE_COMPACT_TIMING_MAX_TLV_BYTES \
    ((2u * PROTO_TLV_U32_ENCODED_LEN) + \
     (2u * PROTO_TLV_U16_ENCODED_LEN))
#define MESH_ROUTE_REQUEST_MAX_PATH_NODES 6u
#define MESH_ROUTE_REQUEST_MAX_PATH_TLV_BYTES \
    (PROTO_TLV_HEADER_LEN + \
     (MESH_ROUTE_REQUEST_MAX_PATH_NODES * sizeof(uint64_t)))
#define MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN \
    (MESH_ROUTE_DISCOVERY_REQUIRED_FIXED_TLV_BYTES + \
     MESH_ROUTE_REQUEST_MAX_PATH_TLV_BYTES + \
     PROTO_TLV_U8_ENCODED_LEN + \
     PROTO_TLV_U16_ENCODED_LEN + \
     MESH_ROUTE_COMPACT_TIMING_MAX_TLV_BYTES)
#define MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN \
    (MESH_ROUTE_DISCOVERY_REQUIRED_FIXED_TLV_BYTES + \
     MESH_ROUTE_PATH_MAX_TLV_BYTES + \
     MESH_ROUTE_COMPACT_TIMING_MAX_TLV_BYTES + \
     (2u * PROTO_TLV_U16_ENCODED_LEN) + \
     PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN)
#define MESH_GATEWAY_ROUTE_ADV_FIXED_TLV_BYTES \
    (PROTO_TLV_U64_ENCODED_LEN + \
     (4u * PROTO_TLV_U32_ENCODED_LEN) + \
     (5u * PROTO_TLV_U16_ENCODED_LEN) + \
     (4u * PROTO_TLV_U8_ENCODED_LEN))
#define MESH_GATEWAY_ROUTE_ADV_MAX_PATH_NODES MESH_NETWORK_MAX_HOPS
#define MESH_GATEWAY_ROUTE_ADV_MAX_PATH_TLV_BYTES \
    (PROTO_TLV_HEADER_LEN + \
     (MESH_GATEWAY_ROUTE_ADV_MAX_PATH_NODES * sizeof(uint64_t)))
#define MESH_GATEWAY_ROUTE_ADV_LEGACY_MAX_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_FIXED_TLV_BYTES + \
     MESH_GATEWAY_ROUTE_ADV_MAX_PATH_TLV_BYTES)
#define MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN \
    (MESH_GATEWAY_ROUTE_ADV_LEGACY_MAX_PAYLOAD_LEN + \
     OPERATION_POLICY_ALL_TLVS_LEN)
#define MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN \
    (PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN)

struct mesh_route_path {
    uint64_t node_ids[MESH_ROUTE_PATH_MAX_NODES];
    uint8_t count;
};

bool mesh_route_path_contains(const struct mesh_route_path *path,
                              uint64_t node_id);
bool mesh_route_paths_intersect(const struct mesh_route_path *first,
                                const struct mesh_route_path *second);
int mesh_route_path_validate(const struct mesh_route_path *path,
                             uint64_t expected_root_id,
                             uint64_t expected_transmitter_id);
int mesh_route_path_append(struct mesh_route_path *path, uint64_t node_id);
int mesh_route_path_append_tlv(uint8_t *payload,
                               size_t payload_cap,
                               size_t *offset,
                               const struct mesh_route_path *path);
int mesh_route_path_from_tlvs(const uint8_t *payload,
                              size_t payload_len,
                              struct mesh_route_path *path);
bool mesh_route_control_payload_len_valid(uint8_t msg_type,
                                          size_t payload_len);
int mesh_route_control_ttl_validate(uint8_t msg_type,
                                    uint8_t remaining_ttl,
                                    uint8_t hop_count);

#ifdef __cplusplus
}
#endif

#endif
