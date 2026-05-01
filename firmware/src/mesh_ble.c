#include "mesh_ble.h"

#include <string.h>

static size_t capped_frame_cap(size_t frame_cap)
{
    return frame_cap < MESH_BLE_MAX_FRAME_LEN ? frame_cap : MESH_BLE_MAX_FRAME_LEN;
}

int mesh_ble_frame_encode(uint64_t previous_hop_id,
                          const struct mesh_outbound *out,
                          uint8_t *frame,
                          size_t frame_cap,
                          size_t *frame_len)
{
    size_t usable_cap;
    size_t packet_len = 0u;
    int ret;

    if (out == NULL || frame == NULL || frame_len == NULL ||
        previous_hop_id == 0u ||
        out->packet.payload_len != out->payload_len ||
        out->payload_len > MESH_BLE_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }

    usable_cap = capped_frame_cap(frame_cap);
    if (usable_cap < MESH_BLE_FRAME_HEADER_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    frame[0] = MESH_BLE_FRAME_MAGIC;
    frame[1] = MESH_BLE_FRAME_VERSION;
    proto_put_u64_le(&frame[2], previous_hop_id);
    proto_put_u64_le(&frame[10], out->next_hop_id);

    ret = proto_packet_encode(&out->packet,
                              out->payload,
                              &frame[MESH_BLE_FRAME_HEADER_LEN],
                              usable_cap - MESH_BLE_FRAME_HEADER_LEN,
                              &packet_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet_len > MESH_BLE_MAX_PACKET_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    proto_put_u16_le(&frame[18], (uint16_t)packet_len);
    *frame_len = MESH_BLE_FRAME_HEADER_LEN + packet_len;
    return PROTO_OK;
}

int mesh_ble_frame_decode(const uint8_t *frame,
                          size_t frame_len,
                          uint64_t local_id,
                          uint64_t *previous_hop_id,
                          struct proto_packet *packet,
                          uint8_t *payload,
                          size_t payload_cap,
                          size_t *payload_len)
{
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    uint64_t hop_id;
    uint64_t next_hop_id;
    uint16_t packet_len;
    int ret;

    if (frame == NULL || previous_hop_id == NULL || packet == NULL ||
        payload == NULL || payload_len == NULL ||
        local_id == 0u ||
        frame_len < MESH_BLE_FRAME_HEADER_LEN ||
        frame_len > MESH_BLE_MAX_FRAME_LEN ||
        frame[0] != MESH_BLE_FRAME_MAGIC ||
        frame[1] != MESH_BLE_FRAME_VERSION) {
        return PROTO_ERR_ARG;
    }

    hop_id = proto_get_u64_le(&frame[2]);
    next_hop_id = proto_get_u64_le(&frame[10]);
    packet_len = proto_get_u16_le(&frame[18]);
    if (hop_id == 0u ||
        hop_id == local_id ||
        (next_hop_id != MESH_BROADCAST_ID && next_hop_id != local_id) ||
        packet_len != frame_len - MESH_BLE_FRAME_HEADER_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    ret = proto_packet_decode(&frame[MESH_BLE_FRAME_HEADER_LEN],
                              packet_len,
                              packet,
                              &decoded_payload,
                              &decoded_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded_payload_len > payload_cap) {
        return PROTO_ERR_NO_SPACE;
    }

    if (decoded_payload_len > 0u) {
        memcpy(payload, decoded_payload, decoded_payload_len);
    }
    *payload_len = decoded_payload_len;
    *previous_hop_id = hop_id;
    return PROTO_OK;
}
