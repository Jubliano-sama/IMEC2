#ifndef APP_GATEWAY_COMMAND_INGRESS_H
#define APP_GATEWAY_COMMAND_INGRESS_H

#include "gateway_command.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_gateway_command_ingress_item {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len;
    uint32_t admission_id;
};

struct app_gateway_command_identity {
    uint8_t msg_type;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint32_t admission_id;
    enum command_id command_id;
};

struct app_gateway_command_ingress_ops {
    bool gateway_role;
    int (*admit)(void *ctx, struct app_gateway_command_ingress_item *item);
    int (*submit_priority)(void *ctx);
    int (*cancel_admitted)(void *ctx,
                            const struct app_gateway_command_identity *identity);
    void (*emit_result)(void *ctx,
                        const struct proto_packet *command,
                        enum command_id command_id,
                        enum command_status status,
                        uint8_t reason);
    void (*note_decoded)(void *ctx,
                         const struct app_gateway_command_ingress_item *item);
    void *ctx;
};

int app_gateway_command_ingress_handle_frame(
    const struct app_gateway_command_ingress_ops *ops,
    const uint8_t *frame,
    size_t frame_len,
    struct app_gateway_command_ingress_item *item_out,
    bool *command_handled);

bool app_gateway_command_identity_matches(
    const struct app_gateway_command_identity *identity,
    const struct app_gateway_command_ingress_item *item);

#endif
