#ifndef APP_MESH_ROUTE_REPLY_MATCH_H
#define APP_MESH_ROUTE_REPLY_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mesh_outbound;
struct proto_packet;

bool app_mesh_route_reply_ack_matches(
    const struct mesh_outbound *route_reply,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint64_t local_id);

#endif
