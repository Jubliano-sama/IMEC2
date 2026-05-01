#ifndef MESH_BLE_H
#define MESH_BLE_H

#include "mesh_relay.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_BLE_FRAME_MAGIC 0xB7u
#define MESH_BLE_FRAME_VERSION 0x01u
#define MESH_BLE_FRAME_HEADER_LEN 20u
#define MESH_BLE_MAX_FRAME_LEN 254u
#define MESH_BLE_MAX_PACKET_LEN (MESH_BLE_MAX_FRAME_LEN - MESH_BLE_FRAME_HEADER_LEN)
#define MESH_BLE_MAX_PAYLOAD_LEN (MESH_BLE_MAX_PACKET_LEN - PACKET_HEADER_LEN - PACKET_CRC_LEN)

int mesh_ble_frame_encode(uint64_t previous_hop_id,
                          const struct mesh_outbound *out,
                          uint8_t *frame,
                          size_t frame_cap,
                          size_t *frame_len);
int mesh_ble_frame_decode(const uint8_t *frame,
                          size_t frame_len,
                          uint64_t local_id,
                          uint64_t *previous_hop_id,
                          struct proto_packet *packet,
                          uint8_t *payload,
                          size_t payload_cap,
                          size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif
