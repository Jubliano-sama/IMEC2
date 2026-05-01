#include "serial_frame.h"

#include <string.h>

int serial_frame_encode_packet(const struct proto_packet *packet,
                               const uint8_t *payload,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written)
{
    uint8_t packet_buf[PACKET_MAX_LEN];
    size_t packet_len = 0u;
    size_t cobs_len = 0u;
    int ret;

    if (packet == NULL || out == NULL || written == NULL ||
        (payload == NULL && packet->payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < 2u) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = proto_packet_encode(packet, payload, packet_buf, sizeof(packet_buf), &packet_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = proto_cobs_encode(packet_buf, packet_len, out, out_cap - 1u, &cobs_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (cobs_len + 1u > out_cap) {
        return PROTO_ERR_NO_SPACE;
    }

    out[cobs_len] = SERIAL_FRAME_DELIMITER;
    *written = cobs_len + 1u;
    return PROTO_OK;
}

int serial_frame_decode_packet(const uint8_t *frame,
                               size_t frame_len,
                               struct proto_packet *packet,
                               uint8_t *payload,
                               size_t payload_cap,
                               size_t *payload_len)
{
    uint8_t packet_buf[PACKET_MAX_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t encoded_len;
    size_t packet_len = 0u;
    size_t decoded_payload_len = 0u;
    int ret;

    if (frame == NULL || frame_len == 0u || packet == NULL ||
        payload == NULL || payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame[frame_len - 1u] != SERIAL_FRAME_DELIMITER) {
        return PROTO_ERR_MALFORMED;
    }

    encoded_len = frame_len - 1u;
    ret = proto_cobs_decode(frame, encoded_len, packet_buf, sizeof(packet_buf), &packet_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = proto_packet_decode(packet_buf, packet_len, packet, &decoded_payload, &decoded_payload_len);
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
    return PROTO_OK;
}
