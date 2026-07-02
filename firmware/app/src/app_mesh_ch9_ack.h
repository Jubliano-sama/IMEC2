#ifndef APP_MESH_CH9_ACK_H
#define APP_MESH_CH9_ACK_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_ch9_tx_ack_entry {
    uint32_t session_id;
    uint16_t seq;
    bool acked;
};

struct app_mesh_ch9_tx_ack_result {
    uint8_t acked_now;
    uint8_t unacked_count;
    bool any_match;
    bool all_acked;
};

int app_mesh_ch9_tx_ack_apply(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              struct app_mesh_ch9_tx_ack_entry *entries,
                              uint8_t entry_count,
                              struct app_mesh_ch9_tx_ack_result *result);

#endif
