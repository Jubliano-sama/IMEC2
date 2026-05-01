#ifndef GATEWAY_COMMAND_H
#define GATEWAY_COMMAND_H

#include "mesh_relay.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gateway_command_extract_id(const uint8_t *payload,
                               size_t payload_len,
                               enum command_id *command_id);
int gateway_command_prepare_outbound(const struct proto_packet *host_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t gateway_id,
                                     uint32_t now_ms,
                                     uint16_t fallback_seq,
                                     struct mesh_outbound *out,
                                     enum command_id *command_id);
int gateway_command_build_failure_result(const struct proto_packet *command,
                                         uint64_t gateway_id,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason,
                                         uint32_t now_ms,
                                         struct proto_packet *result,
                                         uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif
