#include "protocol.h"

#include <string.h>

uint16_t proto_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    if (data == NULL && len != 0u) {
        return 0u;
    }

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8u; bit++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

uint16_t proto_get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t proto_get_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

uint64_t proto_get_u64_le(const uint8_t *data)
{
    return (uint64_t)proto_get_u32_le(data) |
           ((uint64_t)proto_get_u32_le(data + 4) << 32);
}

void proto_put_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8);
}

void proto_put_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8) & 0xFFu);
    data[2] = (uint8_t)((value >> 16) & 0xFFu);
    data[3] = (uint8_t)((value >> 24) & 0xFFu);
}

void proto_put_u64_le(uint8_t *data, uint64_t value)
{
    proto_put_u32_le(data, (uint32_t)(value & 0xFFFFFFFFu));
    proto_put_u32_le(data + 4, (uint32_t)(value >> 32));
}

size_t proto_packet_encoded_len(uint8_t payload_len)
{
    return PACKET_HEADER_LEN + (size_t)payload_len + PACKET_CRC_LEN;
}

int proto_packet_encode(const struct proto_packet *packet,
                       const uint8_t *payload,
                       uint8_t *out,
                       size_t out_len,
                       size_t *written)
{
    const size_t total_len = packet == NULL ? 0u : proto_packet_encoded_len(packet->payload_len);

    if (packet == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (packet->payload_len > 0u && payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_len < total_len) {
        return PROTO_ERR_NO_SPACE;
    }

    out[0] = PROTO_MAGIC;
    out[1] = PROTO_VERSION;
    out[2] = packet->msg_type;
    out[3] = packet->flags;
    proto_put_u64_le(&out[4], packet->src_id);
    proto_put_u64_le(&out[12], packet->dst_id);
    proto_put_u32_le(&out[20], packet->session_id);
    proto_put_u16_le(&out[24], packet->seq);
    out[26] = packet->ttl;
    out[27] = packet->payload_len;

    if (packet->payload_len > 0u) {
        memcpy(&out[PACKET_HEADER_LEN], payload, packet->payload_len);
    }

    const uint16_t crc = proto_crc16_ccitt_false(out, total_len - PACKET_CRC_LEN);
    proto_put_u16_le(&out[total_len - PACKET_CRC_LEN], crc);
    *written = total_len;

    return PROTO_OK;
}

int proto_packet_decode(const uint8_t *data,
                       size_t len,
                       struct proto_packet *packet,
                       const uint8_t **payload,
                       size_t *payload_len)
{
    if (data == NULL || packet == NULL || payload == NULL || payload_len == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len < PACKET_HEADER_LEN + PACKET_CRC_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (data[0] != PROTO_MAGIC) {
        return PROTO_ERR_BAD_MAGIC;
    }
    if (data[1] != PROTO_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }

    const uint8_t declared_payload_len = data[27];
    const size_t expected_len = proto_packet_encoded_len(declared_payload_len);
    if (len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }

    const uint16_t expected_crc = proto_get_u16_le(&data[len - PACKET_CRC_LEN]);
    const uint16_t actual_crc = proto_crc16_ccitt_false(data, len - PACKET_CRC_LEN);
    if (actual_crc != expected_crc) {
        return PROTO_ERR_BAD_CRC;
    }

    packet->msg_type = data[2];
    packet->flags = data[3];
    packet->src_id = proto_get_u64_le(&data[4]);
    packet->dst_id = proto_get_u64_le(&data[12]);
    packet->session_id = proto_get_u32_le(&data[20]);
    packet->seq = proto_get_u16_le(&data[24]);
    packet->ttl = data[26];
    packet->payload_len = declared_payload_len;
    *payload = &data[PACKET_HEADER_LEN];
    *payload_len = declared_payload_len;

    return PROTO_OK;
}

int tlv_append_bytes(uint8_t *payload,
                          size_t payload_cap,
                          size_t *offset,
                          uint8_t type,
                          const uint8_t *value,
                          uint8_t len)
{
    if (payload == NULL || offset == NULL || (value == NULL && len > 0u)) {
        return PROTO_ERR_ARG;
    }
    if (*offset > payload_cap || payload_cap - *offset < (size_t)len + 2u) {
        return PROTO_ERR_NO_SPACE;
    }

    payload[*offset] = type;
    payload[*offset + 1u] = len;
    if (len > 0u) {
        memcpy(&payload[*offset + 2u], value, len);
    }
    *offset += (size_t)len + 2u;
    return PROTO_OK;
}

int tlv_append_u8(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint8_t value)
{
    return tlv_append_bytes(payload, payload_cap, offset, type, &value, 1u);
}

int tlv_append_i8(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, int8_t value)
{
    const uint8_t raw = (uint8_t)value;
    return tlv_append_bytes(payload, payload_cap, offset, type, &raw, 1u);
}

int tlv_append_u16(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint16_t value)
{
    uint8_t raw[2];
    proto_put_u16_le(raw, value);
    return tlv_append_bytes(payload, payload_cap, offset, type, raw, sizeof(raw));
}

int tlv_append_u32(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint32_t value)
{
    uint8_t raw[4];
    proto_put_u32_le(raw, value);
    return tlv_append_bytes(payload, payload_cap, offset, type, raw, sizeof(raw));
}

int tlv_append_i32(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, int32_t value)
{
    uint8_t raw[4];
    proto_put_u32_le(raw, (uint32_t)value);
    return tlv_append_bytes(payload, payload_cap, offset, type, raw, sizeof(raw));
}

int tlv_append_u64(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint64_t value)
{
    uint8_t raw[8];
    proto_put_u64_le(raw, value);
    return tlv_append_bytes(payload, payload_cap, offset, type, raw, sizeof(raw));
}

int tlv_find(const uint8_t *payload,
                  size_t payload_len,
                  uint8_t type,
                  const uint8_t **value,
                  uint8_t *len)
{
    size_t offset = 0u;

    if (payload == NULL || value == NULL || len == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }

        const uint8_t current_type = payload[offset];
        const uint8_t current_len = payload[offset + 1u];
        offset += 2u;
        if (payload_len - offset < current_len) {
            return PROTO_ERR_MALFORMED;
        }

        if (current_type == type) {
            *value = &payload[offset];
            *len = current_len;
            return PROTO_OK;
        }

        offset += current_len;
    }

    return PROTO_ERR_NOT_FOUND;
}

int proto_cobs_encode(const uint8_t *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *written)
{
    size_t read_index = 0u;
    size_t write_index = 1u;
    size_t code_index = 0u;
    uint8_t code = 1u;

    if ((input == NULL && input_len != 0u) || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap == 0u) {
        return PROTO_ERR_NO_SPACE;
    }

    while (read_index < input_len) {
        if (input[read_index] == 0u) {
            if (code_index >= out_cap) {
                return PROTO_ERR_NO_SPACE;
            }
            out[code_index] = code;
            code = 1u;
            code_index = write_index;
            write_index++;
            read_index++;
        } else {
            if (write_index >= out_cap) {
                return PROTO_ERR_NO_SPACE;
            }
            out[write_index] = input[read_index];
            write_index++;
            read_index++;
            code++;

            if (code == 0xFFu) {
                if (code_index >= out_cap) {
                    return PROTO_ERR_NO_SPACE;
                }
                out[code_index] = code;
                code = 1u;
                code_index = write_index;
                write_index++;
            }
        }
    }

    if (code_index >= out_cap) {
        return PROTO_ERR_NO_SPACE;
    }
    out[code_index] = code;
    *written = write_index;
    return PROTO_OK;
}

int proto_cobs_decode(const uint8_t *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *written)
{
    size_t read_index = 0u;
    size_t write_index = 0u;

    if ((input == NULL && input_len != 0u) || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }

    while (read_index < input_len) {
        const uint8_t code = input[read_index];
        if (code == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        read_index++;

        for (uint8_t i = 1u; i < code; i++) {
            if (read_index >= input_len) {
                return PROTO_ERR_MALFORMED;
            }
            if (write_index >= out_cap) {
                return PROTO_ERR_NO_SPACE;
            }
            out[write_index] = input[read_index];
            write_index++;
            read_index++;
        }

        if (code < 0xFFu && read_index < input_len) {
            if (write_index >= out_cap) {
                return PROTO_ERR_NO_SPACE;
            }
            out[write_index] = 0u;
            write_index++;
        }
    }

    *written = write_index;
    return PROTO_OK;
}
