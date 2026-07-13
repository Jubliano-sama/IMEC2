#ifndef APP_MESH_GATEWAY_COMMAND_FLOW_H
#define APP_MESH_GATEWAY_COMMAND_FLOW_H

#include "gateway_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_gateway_command_flow {
    struct mesh_outbound outbound;
    struct gateway_command_options options;
    enum command_id command_id;
    enum gateway_command_tracking_mode tracking_mode;
};

struct app_mesh_gateway_command_anchor_state {
    struct gateway_command_rx_duplicate_cache duplicate_cache;
};

int app_mesh_gateway_command_flow_prepare(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint32_t now_ms,
    uint16_t fallback_seq,
    struct app_mesh_gateway_command_flow *flow);
int app_mesh_gateway_command_flow_anchor_receive(
    struct app_mesh_gateway_command_anchor_state *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms,
    enum command_id *command_id,
    struct gateway_command_options *options,
    bool *broadcast,
    bool *expired,
    bool *duplicate);
void app_mesh_gateway_command_flow_anchor_remember(
    struct app_mesh_gateway_command_anchor_state *state,
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    uint32_t now_ms);
int app_mesh_gateway_command_flow_init_result(
    struct mesh_outbound *outbound,
    const struct proto_packet *command,
    uint64_t source_id,
    uint64_t gateway_id,
    bool diagnostic);
bool app_mesh_gateway_command_flow_result_matches(
    const struct proto_packet *pending_command,
    const struct proto_packet *result);
int app_mesh_gateway_command_flow_decode_result(
    enum command_id pending_command_id,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason);

#endif
