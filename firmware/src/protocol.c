#include "protocol.h"

#include <string.h>

#define CLICK_REPORT_IDENTITY_FNV_OFFSET UINT32_C(2166136261)
#define CLICK_REPORT_IDENTITY_FNV_PRIME UINT32_C(16777619)

uint16_t proto_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    return proto_crc16_ccitt_false_update(UINT16_C(0xFFFF), data, len);
}

uint16_t proto_crc16_ccitt_false_update(uint16_t crc,
                                        const uint8_t *data,
                                        size_t len)
{
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

uint32_t proto_click_report_session_id(uint64_t clicker_id,
                                       uint32_t event_seq)
{
    uint32_t hash = CLICK_REPORT_IDENTITY_FNV_OFFSET;

    if (clicker_id == 0u || event_seq == 0u) {
        return 0u;
    }
    /* FNV-1a over the canonical little-endian wire representation. */
    for (uint8_t shift = 0u; shift < 64u; shift += 8u) {
        hash ^= (uint8_t)(clicker_id >> shift);
        hash *= CLICK_REPORT_IDENTITY_FNV_PRIME;
    }
    for (uint8_t shift = 0u; shift < 32u; shift += 8u) {
        hash ^= (uint8_t)(event_seq >> shift);
        hash *= CLICK_REPORT_IDENTITY_FNV_PRIME;
    }
    return hash == 0u ? 1u : hash;
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

size_t proto_packet_header_len(uint16_t payload_len)
{
    return payload_len < PACKET_EXT_LENGTH_SENTINEL ? PACKET_HEADER_LEN : PACKET_EXT_HEADER_LEN;
}

size_t proto_packet_encoded_len(uint16_t payload_len)
{
    return proto_packet_header_len(payload_len) + (size_t)payload_len + PACKET_CRC_LEN;
}

static bool proto_packet_msg_type_valid(uint8_t msg_type)
{
    switch (msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_MESH_HOP_ACK:
    case MSG_GATEWAY_ACK:
    case MSG_GATEWAY_ACK_CONFIRM:
    case MSG_ROUTE_REQ:
    case MSG_ROUTE_REPLY:
    case MSG_ROUTE_REPLY_ACK:
    case MSG_GATEWAY_ROUTE_ADV:
    case MSG_RELAY_BUSY:
    case MSG_RESULT_BUSY:
    case MSG_GATEWAY_ROUTE_REQ:
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_OFFER:
    case MSG_RESULT_GRANT:
    case MSG_RESULT_BUNDLE:
    case MSG_GATEWAY_COLLECTION_EACK:
    case MSG_SURVEY_REACH_REQ:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_START:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_GATEWAY_COMMAND_EVENT:
    case MSG_GATEWAY_HOST_RECEIPT:
    case MSG_ERROR:
        return true;
    default:
        return false;
    }
}

bool proto_packet_msg_type_allowed_over_uwb(uint8_t msg_type)
{
    if (!proto_packet_msg_type_valid(msg_type)) {
        return false;
    }

    /*
     * These are host-transport records.  They have no RF producer or mesh
     * state-machine contract and must never enter routing, replay, ACK, or BLE
     * retention state from a received UWB frame.
     */
    return msg_type != MSG_GATEWAY_COMMAND_EVENT &&
           msg_type != MSG_GATEWAY_HOST_RECEIPT &&
           msg_type != MSG_ERROR;
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
    if (packet->payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (packet->payload_len > 0u && payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!proto_packet_msg_type_valid(packet->msg_type)) {
        return PROTO_ERR_MALFORMED;
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
    if (packet->payload_len < PACKET_EXT_LENGTH_SENTINEL) {
        out[27] = (uint8_t)packet->payload_len;
        proto_put_u32_le(&out[28], packet->message_age_ms);
    } else {
        out[27] = PACKET_EXT_LENGTH_SENTINEL;
        proto_put_u16_le(&out[28], packet->payload_len);
        proto_put_u32_le(&out[30], packet->message_age_ms);
    }

    if (packet->payload_len > 0u) {
        memcpy(&out[proto_packet_header_len(packet->payload_len)], payload, packet->payload_len);
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

    const bool extended = data[27] == PACKET_EXT_LENGTH_SENTINEL;
    uint16_t declared_payload_len;
    size_t header_len;

    if (extended) {
        if (len < PACKET_EXT_HEADER_LEN + PACKET_CRC_LEN) {
            return PROTO_ERR_BAD_LENGTH;
        }
        declared_payload_len = proto_get_u16_le(&data[28]);
        header_len = PACKET_EXT_HEADER_LEN;
        if (declared_payload_len < PACKET_EXT_LENGTH_SENTINEL) {
            return PROTO_ERR_MALFORMED;
        }
    } else {
        declared_payload_len = data[27];
        header_len = PACKET_HEADER_LEN;
    }
    if (declared_payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }
    const size_t expected_len = proto_packet_encoded_len(declared_payload_len);
    if (len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }

    const uint16_t expected_crc = proto_get_u16_le(&data[len - PACKET_CRC_LEN]);
    const uint16_t actual_crc = proto_crc16_ccitt_false(data, len - PACKET_CRC_LEN);
    if (actual_crc != expected_crc) {
        return PROTO_ERR_BAD_CRC;
    }
    if (!proto_packet_msg_type_valid(data[2])) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = data[2];
    packet->flags = data[3];
    packet->src_id = proto_get_u64_le(&data[4]);
    packet->dst_id = proto_get_u64_le(&data[12]);
    packet->session_id = proto_get_u32_le(&data[20]);
    packet->seq = proto_get_u16_le(&data[24]);
    packet->ttl = data[26];
    packet->payload_len = declared_payload_len;
    packet->message_age_ms = proto_get_u32_le(&data[extended ? 30u : 28u]);
    *payload = &data[header_len];
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

int tlv_find_unique(const uint8_t *payload,
                    size_t payload_len,
                    uint8_t type,
                    const uint8_t **value,
                    uint8_t *len)
{
    const uint8_t *found_value = NULL;
    uint8_t found_len = 0u;
    size_t offset = 0u;
    bool found = false;

    if (payload == NULL || value == NULL || len == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        uint8_t current_type;
        uint8_t current_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        current_type = payload[offset];
        current_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (payload_len - offset < current_len) {
            return PROTO_ERR_MALFORMED;
        }

        if (current_type == type) {
            if (found) {
                return PROTO_ERR_MALFORMED;
            }
            found_value = &payload[offset];
            found_len = current_len;
            found = true;
        }
        offset += current_len;
    }

    if (!found) {
        return PROTO_ERR_NOT_FOUND;
    }
    *value = found_value;
    *len = found_len;
    return PROTO_OK;
}

static bool gateway_host_receipt_identity_valid(
    const struct gateway_host_receipt_identity *identity)
{
    bool gateway_local;

    if (identity == NULL ||
        (identity->original_msg_type != MSG_GATEWAY_COMMAND_EVENT &&
         !proto_packet_msg_type_allowed_over_uwb(identity->original_msg_type)) ||
        identity->src_id == 0u || identity->dst_id == 0u ||
        identity->session_id == 0u || identity->seq == 0u) {
        return false;
    }

    gateway_local = identity->original_msg_type == MSG_GATEWAY_COMMAND_EVENT ||
                    (identity->original_msg_type == MSG_COMMAND_RESULT &&
                     identity->src_id == identity->dst_id);
    if (gateway_local && identity->src_id != identity->dst_id) {
        return false;
    }
    if (!gateway_local && identity->src_id == identity->dst_id) {
        return false;
    }
    if (identity->original_msg_type == MSG_GATEWAY_COMMAND_EVENT) {
        return identity->original_flags == FLAG_GATEWAY_ACK_REQUIRED;
    }
    if (identity->original_msg_type == MSG_COMMAND_RESULT) {
        if ((identity->original_flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
            return false;
        }
        return !gateway_local ||
               (identity->original_flags &
                ~(FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC | FLAG_ERROR)) ==
                   0u;
    }
    return true;
}

bool gateway_local_command_result_valid(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    uint16_t command_id;
    uint16_t status;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->payload_len != payload_len ||
        payload_len != PROTO_GATEWAY_LOCAL_COMMAND_RESULT_PAYLOAD_LEN ||
        packet->src_id == 0u || packet->src_id != packet->dst_id ||
        packet->session_id == 0u || packet->seq == 0u || packet->ttl != 1u ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        (packet->flags &
         ~(FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC | FLAG_ERROR)) != 0u ||
        payload[0] != TLV_COMMAND_ID || payload[1] != sizeof(uint16_t) ||
        payload[4] != TLV_COMMAND_STATUS || payload[5] != sizeof(uint16_t) ||
        payload[8] != TLV_REASON || payload[9] != sizeof(uint8_t)) {
        return false;
    }

    command_id = proto_get_u16_le(&payload[2]);
    status = proto_get_u16_le(&payload[6]);
    if (command_id == 0u || status > COMMAND_INTERNAL_ERROR) {
        return false;
    }
    return (status == COMMAND_OK) ==
           ((packet->flags & FLAG_ERROR) == 0u);
}

int gateway_host_receipt_identity_encode(
    const struct gateway_host_receipt_identity *identity,
    uint8_t *value,
    size_t value_cap,
    size_t *written)
{
    if (identity == NULL || value == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!gateway_host_receipt_identity_valid(identity)) {
        return PROTO_ERR_MALFORMED;
    }
    if (value_cap < PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    value[0] = identity->original_msg_type;
    value[1] = identity->original_flags;
    proto_put_u64_le(&value[2], identity->src_id);
    proto_put_u64_le(&value[10], identity->dst_id);
    proto_put_u32_le(&value[18], identity->session_id);
    proto_put_u16_le(&value[22], identity->seq);
    memcpy(&value[24],
           identity->stream_record_digest,
           SEMANTIC_DIGEST_SHA256_LEN);
    *written = PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN;
    return PROTO_OK;
}

int gateway_host_receipt_identity_decode(
    const uint8_t *value,
    size_t value_len,
    struct gateway_host_receipt_identity *identity)
{
    if (value == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    if (value_len != PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    memset(identity, 0, sizeof(*identity));
    identity->original_msg_type = value[0];
    identity->original_flags = value[1];
    identity->src_id = proto_get_u64_le(&value[2]);
    identity->dst_id = proto_get_u64_le(&value[10]);
    identity->session_id = proto_get_u32_le(&value[18]);
    identity->seq = proto_get_u16_le(&value[22]);
    memcpy(identity->stream_record_digest,
           &value[24],
           SEMANTIC_DIGEST_SHA256_LEN);
    return gateway_host_receipt_identity_valid(identity) ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

int gateway_host_receipt_identity_append_tlv(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct gateway_host_receipt_identity *identity)
{
    uint8_t value[PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN];
    size_t value_len = 0u;
    int ret;

    if (payload == NULL || offset == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = gateway_host_receipt_identity_encode(identity,
                                               value,
                                               sizeof(value),
                                               &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_GATEWAY_HOST_RECEIPT_IDENTITY,
                            value,
                            (uint8_t)value_len);
}

int gateway_host_receipt_identity_from_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct gateway_host_receipt_identity *identity)
{
    /* A host receipt has exactly one fixed-width TLV.  Requiring the exact
     * serialized length rejects duplicate fields, unknown trailing TLVs, and
     * partial headers instead of silently accepting an ambiguous receipt. */
    if (payload == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len != PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES ||
        payload[0] != TLV_GATEWAY_HOST_RECEIPT_IDENTITY ||
        payload[1] != PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    return gateway_host_receipt_identity_decode(
        &payload[PROTO_TLV_HEADER_LEN],
        PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN,
        identity);
}

int gateway_host_receipt_packet_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct gateway_host_receipt_identity *identity)
{
    if (packet == NULL || payload == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    if (packet->msg_type != MSG_GATEWAY_HOST_RECEIPT || packet->flags != 0u ||
        packet->src_id == 0u || packet->dst_id == 0u ||
        packet->src_id == packet->dst_id || packet->session_id == 0u ||
        packet->seq == 0u || packet->ttl != 1u ||
        packet->payload_len != payload_len ||
        payload_len != PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES) {
        return PROTO_ERR_MALFORMED;
    }
    return gateway_host_receipt_identity_from_tlvs(payload,
                                                   payload_len,
                                                   identity);
}

static int tlv_require_u8(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != 1u) {
        return PROTO_ERR_MALFORMED;
    }
    *value = raw[0];
    return PROTO_OK;
}

static int tlv_require_u16(const uint8_t *payload, size_t payload_len, uint8_t type, uint16_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != 2u) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(raw);
    return PROTO_OK;
}

static int tlv_require_u32(const uint8_t *payload, size_t payload_len, uint8_t type, uint32_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != 4u) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(raw);
    return PROTO_OK;
}

static int tlv_require_u64(const uint8_t *payload, size_t payload_len, uint8_t type, uint64_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != 8u) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u64_le(raw);
    return PROTO_OK;
}

int command_result_id_append_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct command_result_id *id)
{
    int ret;

    if (id == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, id->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_GATEWAY_EPOCH, id->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_COMMAND_SEQ, id->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_NODE_ID, id->node_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_NODE_BOOT_COUNTER, id->node_boot_counter);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_RESULT_SEQ, id->result_seq);
}

int command_result_id_from_tlvs(const uint8_t *payload,
                                size_t payload_len,
                                struct command_result_id *id)
{
    int ret;

    if (id == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(id, 0, sizeof(*id));
    ret = tlv_require_u64(payload, payload_len, TLV_GATEWAY_ID, &id->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_GATEWAY_EPOCH, &id->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload, payload_len, TLV_COMMAND_SEQ, &id->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u64(payload, payload_len, TLV_NODE_ID, &id->node_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload, payload_len, TLV_NODE_BOOT_COUNTER, &id->node_boot_counter);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_require_u16(payload, payload_len, TLV_RESULT_SEQ, &id->result_seq);
}

int result_offer_append_tlvs(uint8_t *payload,
                             size_t payload_cap,
                             size_t *offset,
                             const struct result_offer *offer)
{
    int ret;

    if (payload == NULL || offset == NULL || offer == NULL) {
        return PROTO_ERR_ARG;
    }
    if (*offset > payload_cap ||
        payload_cap - *offset < RESULT_OFFER_TLV_BYTES) {
        return PROTO_ERR_NO_SPACE;
    }
    ret = command_result_id_append_tlvs(payload, payload_cap, offset, &offer->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_PAYLOAD_LEN, offer->result_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_PAYLOAD_CRC, offer->result_crc);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_bytes(payload,
                           payload_cap,
                           offset,
                           TLV_RESULT_SHA256_COMMITMENT,
                           offer->result_digest,
                           SEMANTIC_DIGEST_SHA256_LEN);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_PRIORITY, offer->priority);
}

int result_offer_from_tlvs(const uint8_t *payload,
                           size_t payload_len,
                           struct result_offer *offer)
{
    const uint8_t *result_digest = NULL;
    uint8_t result_digest_len = 0u;
    int ret;

    if (payload == NULL || offer == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len != RESULT_OFFER_TLV_BYTES) {
        return PROTO_ERR_MALFORMED;
    }
    memset(offer, 0, sizeof(*offer));
    ret = command_result_id_from_tlvs(payload, payload_len, &offer->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_PAYLOAD_LEN, &offer->result_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_PAYLOAD_CRC, &offer->result_crc);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_RESULT_SHA256_COMMITMENT,
                          &result_digest,
                          &result_digest_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (result_digest_len != SEMANTIC_DIGEST_SHA256_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    memcpy(offer->result_digest, result_digest, sizeof(offer->result_digest));
    return tlv_require_u8(payload, payload_len, TLV_PRIORITY, &offer->priority);
}

int result_grant_append_tlvs(uint8_t *payload,
                             size_t payload_cap,
                             size_t *offset,
                             const struct result_grant *grant)
{
    int ret;

    if (payload == NULL || offset == NULL || grant == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = command_result_id_append_tlvs(payload, payload_cap, offset, &grant->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_GRANTED_CHANNEL, grant->granted_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_MAX_BYTES, grant->max_bytes);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          offset,
                          TLV_EVENT_OFFSET_HINT,
                          grant->event_offset_hint);
}

int result_grant_from_tlvs(const uint8_t *payload,
                           size_t payload_len,
                           struct result_grant *grant)
{
    int ret;

    if (payload == NULL || grant == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len != RESULT_GRANT_TLV_BYTES) {
        return PROTO_ERR_MALFORMED;
    }
    memset(grant, 0, sizeof(*grant));
    ret = command_result_id_from_tlvs(payload, payload_len, &grant->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_GRANTED_CHANNEL, &grant->granted_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_MAX_BYTES, &grant->max_bytes);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_require_u32(payload, payload_len, TLV_EVENT_OFFSET_HINT, &grant->event_offset_hint);
}

int result_busy_append_tlvs(uint8_t *payload,
                            size_t payload_cap,
                            size_t *offset,
                            const struct result_busy *busy)
{
    int ret;

    if (payload == NULL || offset == NULL || busy == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = command_result_id_append_tlvs(payload, payload_cap, offset, &busy->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_RETRY_AFTER_MS, busy->retry_after_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_RELAY_CAPACITY_STATE, busy->capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload,
                         payload_cap,
                         offset,
                         TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                         busy->capacity_validity_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!busy->has_optional_alternate_parent) {
        return PROTO_OK;
    }
    return tlv_append_u64(payload,
                          payload_cap,
                          offset,
                          TLV_ALTERNATE_PARENT_ID,
                          busy->optional_alternate_parent);
}

int result_busy_from_tlvs(const uint8_t *payload,
                          size_t payload_len,
                          struct result_busy *busy)
{
    uint64_t alternate_parent = 0u;
    const uint8_t *requested_session = NULL;
    const uint8_t *requested_seq = NULL;
    uint8_t requested_session_len = 0u;
    uint8_t requested_seq_len = 0u;
    bool correlated;
    size_t semantic_len;
    int requested_session_ret;
    int requested_seq_ret;
    int ret;

    if (payload == NULL || busy == NULL) {
        return PROTO_ERR_ARG;
    }
    requested_session_ret = tlv_find_unique(
        payload,
        payload_len,
        TLV_REQUESTED_MSG_SESSION_ID,
        &requested_session,
        &requested_session_len);
    requested_seq_ret = tlv_find_unique(payload,
                                        payload_len,
                                        TLV_REQUESTED_MSG_SEQ,
                                        &requested_seq,
                                        &requested_seq_len);
    if ((requested_session_ret != PROTO_OK &&
         requested_session_ret != PROTO_ERR_NOT_FOUND) ||
        (requested_seq_ret != PROTO_OK &&
         requested_seq_ret != PROTO_ERR_NOT_FOUND) ||
        ((requested_session_ret == PROTO_OK) !=
         (requested_seq_ret == PROTO_OK))) {
        return PROTO_ERR_MALFORMED;
    }
    correlated = requested_session_ret == PROTO_OK;
    if (correlated &&
        (requested_session_len != sizeof(uint32_t) ||
         requested_seq_len != sizeof(uint16_t) ||
         proto_get_u32_le(requested_session) == 0u ||
         proto_get_u16_le(requested_seq) == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    semantic_len = correlated ?
                       payload_len - RESULT_BUSY_CORRELATION_TLV_BYTES :
                       payload_len;
    if (semantic_len != RESULT_BUSY_TLV_BYTES &&
        semantic_len != RESULT_BUSY_WITH_ALTERNATE_TLV_BYTES) {
        return PROTO_ERR_MALFORMED;
    }
    memset(busy, 0, sizeof(*busy));
    ret = command_result_id_from_tlvs(payload, payload_len, &busy->result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_RETRY_AFTER_MS, &busy->retry_after_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_RELAY_CAPACITY_STATE, &busy->capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload,
                          payload_len,
                          TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                          &busy->capacity_validity_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u64(payload, payload_len, TLV_ALTERNATE_PARENT_ID, &alternate_parent);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return semantic_len == RESULT_BUSY_TLV_BYTES ?
                   PROTO_OK : PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    busy->has_optional_alternate_parent = true;
    busy->optional_alternate_parent = alternate_parent;
    return semantic_len == RESULT_BUSY_WITH_ALTERNATE_TLV_BYTES ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

struct exact_tlv_rule {
    uint8_t type;
    uint8_t value_len;
};

static int exact_tlv_set_validate(const uint8_t *payload,
                                  size_t payload_len,
                                  const struct exact_tlv_rule *rules,
                                  size_t rule_count,
                                  uint32_t *seen)
{
    size_t offset = 0u;
    uint32_t local_seen = 0u;

    if ((payload == NULL && payload_len != 0u) || rules == NULL ||
        rule_count == 0u || rule_count > 32u || seen == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        size_t rule_index;
        uint8_t type;
        uint8_t value_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (value_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        for (rule_index = 0u; rule_index < rule_count; rule_index++) {
            if (rules[rule_index].type == type) {
                break;
            }
        }
        if (rule_index == rule_count ||
            rules[rule_index].value_len != value_len ||
            (local_seen & (UINT32_C(1) << rule_index)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        local_seen |= UINT32_C(1) << rule_index;
        offset += value_len;
    }

    *seen = local_seen;
    return PROTO_OK;
}

int proto_self_test_report_validate(const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    static const struct exact_tlv_rule rules[] = {
        {TLV_CLICKER_ID, sizeof(uint64_t)},
        {TLV_EVENT_SEQ, sizeof(uint32_t)},
        {TLV_ERROR_CODE, sizeof(uint16_t)},
        {TLV_BATTERY_MV, sizeof(uint16_t)},
    };
    uint64_t clicker_id = 0u;
    uint32_t event_seq = 0u;
    uint16_t failure_code = 0u;
    uint16_t expected_seq;
    uint32_t seen = 0u;
    int ret;

    if (packet == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (packet->msg_type != MSG_SELF_TEST_REPORT ||
        packet->flags !=
            (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC) ||
        packet->src_id == 0u || packet->dst_id == 0u ||
        packet->src_id == packet->dst_id || packet->session_id == 0u ||
        packet->seq == 0u || packet->ttl == 0u ||
        packet->payload_len != payload_len) {
        return PROTO_ERR_MALFORMED;
    }
    ret = exact_tlv_set_validate(payload,
                                 payload_len,
                                 rules,
                                 sizeof(rules) / sizeof(rules[0]),
                                 &seen);
    if (ret != PROTO_OK ||
        seen != ((UINT32_C(1) << (sizeof(rules) / sizeof(rules[0]))) -
                 1u)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_require_u64(payload, payload_len, TLV_CLICKER_ID, &clicker_id);
    if (ret == PROTO_OK) {
        ret = tlv_require_u32(payload,
                              payload_len,
                              TLV_EVENT_SEQ,
                              &event_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_require_u16(payload,
                              payload_len,
                              TLV_ERROR_CODE,
                              &failure_code);
    }
    expected_seq = (uint16_t)event_seq;
    if (expected_seq == 0u) {
        expected_seq = 1u;
    }
    return ret == PROTO_OK && clicker_id == packet->src_id &&
                   event_seq == packet->session_id &&
                   expected_seq == packet->seq && failure_code <= 6u ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

int proto_anchor_heartbeat_validate(const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    static const uint8_t unprovisioned[] = "UNPROVISIONED";
    static const struct exact_tlv_rule rules[] = {
        {TLV_DEVICE_ROLE, sizeof(uint8_t)},                    /* 0 */
        {TLV_BATTERY_MV, sizeof(uint16_t)},                    /* 1 */
        {TLV_STATUS_BITS, sizeof(uint32_t)},                   /* 2 */
        {TLV_UPTIME_MS, sizeof(uint32_t)},                     /* 3 */
        {TLV_TIMESTAMP_MS, sizeof(uint64_t)},                  /* 4 */
        {TLV_GATEWAY_ID, sizeof(uint64_t)},                    /* 5 */
        {TLV_NEXT_HOP_ID, sizeof(uint64_t)},                   /* 6 */
        {TLV_ROUTE_EPOCH, sizeof(uint32_t)},                   /* 7 */
        {TLV_HOP_COUNT, sizeof(uint8_t)},                      /* 8 */
        {TLV_QUALITY, sizeof(uint8_t)},                        /* 9 */
        {TLV_RETRY_COUNT, sizeof(uint8_t)},                    /* 10 */
        {TLV_REASON, sizeof(uint8_t)},                         /* 11 */
        {TLV_MESH_DUPLICATE_COUNT, sizeof(uint8_t)},           /* 12 */
        {TLV_COLLECTION_PENDING_COUNT, sizeof(uint8_t)},       /* 13 */
        {TLV_PARENT_HOLDDOWN_COUNT, sizeof(uint8_t)},          /* 14 */
        {TLV_ROUTE_DISCOVERY_ATTEMPTS, sizeof(uint8_t)},       /* 15 */
        {TLV_OUTBOX_DELIVERY_STATE, sizeof(uint8_t)},          /* 16 */
        {TLV_FLOOD_SUPPRESSION_COUNT, sizeof(uint8_t)},        /* 17 */
        {TLV_ROUTE_REPLY_RETRY_COUNT, sizeof(uint8_t)},        /* 18 */
        {TLV_BUSY_RESPONSE_COUNT, sizeof(uint8_t)},            /* 19 */
        {TLV_MESH_CHANNEL_SWITCHES, sizeof(uint32_t)},         /* 20 */
        {TLV_MESH_PLL_READY_FAILURES, sizeof(uint32_t)},       /* 21 */
        {TLV_MESH_LATE_CHANNEL5_RETURNS, sizeof(uint32_t)},    /* 22 */
        {TLV_MESH_DEFERRALS, sizeof(uint32_t)},                /* 23 */
        {TLV_MESH_CH9_EVENT_MISSES, sizeof(uint32_t)},         /* 24 */
        {TLV_MESH_CHANNEL5_PREEMPTIONS, sizeof(uint32_t)},     /* 25 */
        {TLV_MESH_CH9_REPORT_LATENCY_MS, sizeof(uint32_t)},    /* 26 */
        {TLV_DISCOVERY_ASSIGNMENT_EPOCH, sizeof(uint32_t)},    /* 27 */
        {TLV_ERROR_DETAIL, sizeof(unprovisioned) - 1u},         /* 28 */
    };
    const uint32_t base_mask = (UINT32_C(1) << 6u) - 1u;
    const uint32_t route_mask = UINT32_C(0x1f) << 6u;
    const uint32_t reason_mask = UINT32_C(1) << 11u;
    const uint32_t telemetry_mask = UINT32_C(0xff) << 12u;
    const uint32_t metric_mask = UINT32_C(0x7f) << 20u;
    const uint32_t assignment_mask = UINT32_C(3) << 27u;
    const uint8_t *error_detail = NULL;
    uint8_t error_detail_len = 0u;
    uint64_t gateway_id = 0u;
    uint64_t next_hop_id = 0u;
    uint32_t assignment_epoch = 0u;
    uint32_t seen = 0u;
    uint8_t role = 0u;
    uint8_t hop_count = 0u;
    uint8_t reason = 0u;
    int ret;

    if (packet == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (packet->msg_type != MSG_ANCHOR_HEARTBEAT ||
        packet->flags != FLAG_GATEWAY_ACK_REQUIRED ||
        packet->src_id == 0u || packet->dst_id == 0u ||
        packet->src_id == packet->dst_id || packet->session_id == 0u ||
        packet->seq == 0u || packet->ttl == 0u ||
        packet->payload_len != payload_len) {
        return PROTO_ERR_MALFORMED;
    }
    ret = exact_tlv_set_validate(payload,
                                 payload_len,
                                 rules,
                                 sizeof(rules) / sizeof(rules[0]),
                                 &seen);
    if (ret != PROTO_OK ||
        (seen & (base_mask | telemetry_mask | metric_mask)) !=
            (base_mask | telemetry_mask | metric_mask) ||
        ((seen & route_mask) != route_mask) ==
            ((seen & reason_mask) != reason_mask) ||
        ((seen & assignment_mask) == assignment_mask)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_DEVICE_ROLE, &role);
    if (ret == PROTO_OK) {
        ret = tlv_require_u64(payload,
                              payload_len,
                              TLV_GATEWAY_ID,
                              &gateway_id);
    }
    if (ret != PROTO_OK || role != ROLE_ANCHOR ||
        gateway_id != packet->dst_id) {
        return PROTO_ERR_MALFORMED;
    }
    if ((seen & route_mask) != 0u) {
        ret = tlv_require_u64(payload,
                              payload_len,
                              TLV_NEXT_HOP_ID,
                              &next_hop_id);
        if (ret == PROTO_OK) {
            ret = tlv_require_u8(payload,
                                 payload_len,
                                 TLV_HOP_COUNT,
                                 &hop_count);
        }
        if (ret != PROTO_OK || next_hop_id == 0u ||
            next_hop_id == packet->src_id || hop_count == 0u) {
            return PROTO_ERR_MALFORMED;
        }
    } else {
        ret = tlv_require_u8(payload, payload_len, TLV_REASON, &reason);
        if (ret != PROTO_OK || reason != (uint8_t)(-PROTO_ERR_NOT_FOUND)) {
            return PROTO_ERR_MALFORMED;
        }
    }
    if ((seen & (UINT32_C(1) << 27u)) != 0u) {
        ret = tlv_require_u32(payload,
                              payload_len,
                              TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                              &assignment_epoch);
        if (ret != PROTO_OK || assignment_epoch == 0u) {
            return PROTO_ERR_MALFORMED;
        }
    } else if ((seen & (UINT32_C(1) << 28u)) != 0u) {
        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_ERROR_DETAIL,
                              &error_detail,
                              &error_detail_len);
        if (ret != PROTO_OK ||
            error_detail_len != sizeof(unprovisioned) - 1u ||
            memcmp(error_detail,
                   unprovisioned,
                   sizeof(unprovisioned) - 1u) != 0) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

int result_bundle_header_append_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct result_bundle_header *bundle)
{
    int ret;

    if (bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, bundle->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_GATEWAY_EPOCH, bundle->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_COMMAND_SEQ, bundle->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_COLLECTION_EPOCH_ID,
                         bundle->collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_BUNDLE_ID, bundle->bundle_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_RECORD_COUNT, bundle->record_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_BUNDLE_CRC, bundle->bundle_crc);
}

int result_bundle_header_from_tlvs(const uint8_t *payload,
                                   size_t payload_len,
                                   struct result_bundle_header *bundle)
{
    int ret;

    if (bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(bundle, 0, sizeof(*bundle));
    ret = tlv_require_u64(payload, payload_len, TLV_GATEWAY_ID, &bundle->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_GATEWAY_EPOCH, &bundle->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload, payload_len, TLV_COMMAND_SEQ, &bundle->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          &bundle->collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_BUNDLE_ID, &bundle->bundle_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_RECORD_COUNT, &bundle->record_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_require_u16(payload, payload_len, TLV_BUNDLE_CRC, &bundle->bundle_crc);
}

int result_bundle_record_append_tlv(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    const struct result_bundle_record *record)
{
    uint8_t *value;
    size_t record_len;

    if (payload == NULL || offset == NULL || record == NULL ||
        (record->payload == NULL && record->payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (record->payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    record_len = RESULT_BUNDLE_RECORD_HEADER_LEN + (size_t)record->payload_len;
    if (*offset > payload_cap || payload_cap - *offset < record_len + 2u) {
        return PROTO_ERR_NO_SPACE;
    }

    payload[*offset] = TLV_RESULT_RECORD;
    payload[*offset + 1u] = (uint8_t)record_len;
    value = &payload[*offset + 2u];
    proto_put_u64_le(&value[0], record->result_id.gateway_id);
    proto_put_u16_le(&value[8], record->result_id.gateway_epoch);
    proto_put_u32_le(&value[10], record->result_id.command_seq);
    proto_put_u64_le(&value[14], record->result_id.node_id);
    proto_put_u32_le(&value[22], record->result_id.node_boot_counter);
    proto_put_u16_le(&value[26], record->result_id.result_seq);
    proto_put_u16_le(&value[28], record->payload_len);
    proto_put_u16_le(&value[30], record->payload_crc);
    if (record->payload_len > 0u) {
        memcpy(&value[RESULT_BUNDLE_RECORD_HEADER_LEN],
               record->payload,
               record->payload_len);
    }
    *offset += record_len + 2u;
    return PROTO_OK;
}

int result_bundle_record_next_from_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        size_t *offset,
                                        struct result_bundle_record *record)
{
    size_t cursor;
    uint8_t record_len;
    const uint8_t *value;

    if (payload == NULL || offset == NULL || record == NULL) {
        return PROTO_ERR_ARG;
    }

    cursor = *offset;
    while (cursor < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - cursor < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[cursor];
        len = payload[cursor + 1u];
        cursor += 2u;
        if (payload_len - cursor < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type != TLV_RESULT_RECORD) {
            cursor += len;
            continue;
        }

        record_len = len;
        value = &payload[cursor];
        if (record_len < RESULT_BUNDLE_RECORD_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }

        memset(record, 0, sizeof(*record));
        record->result_id.gateway_id = proto_get_u64_le(&value[0]);
        record->result_id.gateway_epoch = proto_get_u16_le(&value[8]);
        record->result_id.command_seq = proto_get_u32_le(&value[10]);
        record->result_id.node_id = proto_get_u64_le(&value[14]);
        record->result_id.node_boot_counter = proto_get_u32_le(&value[22]);
        record->result_id.result_seq = proto_get_u16_le(&value[26]);
        record->payload_len = proto_get_u16_le(&value[28]);
        record->payload_crc = proto_get_u16_le(&value[30]);
        if (record->payload_len != record_len - RESULT_BUNDLE_RECORD_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        record->payload = &value[RESULT_BUNDLE_RECORD_HEADER_LEN];
        if (proto_crc16_ccitt_false(record->payload, record->payload_len) !=
            record->payload_crc) {
            return PROTO_ERR_BAD_CRC;
        }

        *offset = cursor + record_len;
        return PROTO_OK;
    }

    *offset = cursor;
    return PROTO_ERR_NOT_FOUND;
}

int gateway_collection_eack_append_tlvs(uint8_t *payload,
                                        size_t payload_cap,
                                        size_t *offset,
                                        const struct gateway_collection_eack *eack)
{
    int ret;

    if (eack == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, eack->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_GATEWAY_EPOCH, eack->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_COMMAND_SEQ, eack->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_COLLECTION_EPOCH_ID,
                         eack->collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_MEMBERSHIP_EPOCH, eack->membership_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_EXPECTED_NODE_COUNT, eack->expected_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_RECEIVED_COUNT, eack->received_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack->packet_sequence == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_append_u16(payload,
                         payload_cap,
                         offset,
                         TLV_EACK_PACKET_SEQUENCE,
                         eack->packet_sequence);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_EACK_FORMAT, eack->eack_format);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_RETRY_ROUND, eack->retry_round);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_NEXT_RETRY_SPREAD_MS,
                         eack->next_retry_spread_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload,
                         payload_cap,
                         offset,
                         TLV_COLLECTION_OPEN,
                         eack->collection_open ? 1u : 0u);
}

int gateway_collection_eack_from_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      struct gateway_collection_eack *eack)
{
    uint8_t collection_open = 0u;
    int ret;

    if (eack == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(eack, 0, sizeof(*eack));
    ret = tlv_require_u64(payload, payload_len, TLV_GATEWAY_ID, &eack->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_GATEWAY_EPOCH, &eack->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload, payload_len, TLV_COMMAND_SEQ, &eack->command_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          &eack->collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_MEMBERSHIP_EPOCH, &eack->membership_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_EXPECTED_NODE_COUNT, &eack->expected_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload, payload_len, TLV_RECEIVED_COUNT, &eack->received_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload,
                          payload_len,
                          TLV_EACK_PACKET_SEQUENCE,
                          &eack->packet_sequence);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack->packet_sequence == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_EACK_FORMAT, &eack->eack_format);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack->eack_format > EACK_FORMAT_EXPLICIT_MISSING_LIST) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_RETRY_ROUND, &eack->retry_round);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u32(payload,
                          payload_len,
                          TLV_NEXT_RETRY_SPREAD_MS,
                          &eack->next_retry_spread_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u8(payload, payload_len, TLV_COLLECTION_OPEN, &collection_open);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (collection_open > 1u) {
        return PROTO_ERR_MALFORMED;
    }
    eack->collection_open = collection_open != 0u;
    return PROTO_OK;
}

int gateway_collection_eack_recovery_attempt_id(
    const uint8_t *payload,
    size_t payload_len,
    uint32_t *attempt_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || attempt_id == NULL) {
        return PROTO_ERR_ARG;
    }
    *attempt_id = 0u;
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *attempt_id = proto_get_u32_le(value);
    return *attempt_id == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

int gateway_collection_eack_recovery_identity(
    const uint8_t *payload,
    size_t payload_len,
    struct gateway_collection_recovery_identity *identity)
{
    const uint8_t *digest = NULL;
    uint8_t digest_len = 0u;
    int ret;

    if (payload == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(identity, 0, sizeof(*identity));
    if (payload_len != PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    ret = gateway_collection_eack_recovery_attempt_id(
        payload, payload_len, &identity->recovery_attempt_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u64(payload,
                          payload_len,
                          TLV_NODE_ID,
                          &identity->packet_src_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload,
                          payload_len,
                          TLV_RESULT_SEQ,
                          &identity->packet_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_require_u16(payload,
                          payload_len,
                          TLV_PAYLOAD_LEN,
                          &identity->payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_RESULT_SHA256_COMMITMENT,
                          &digest,
                          &digest_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (identity->packet_src_id == 0u ||
        identity->packet_seq == 0u ||
        identity->payload_len == 0u ||
        identity->payload_len > PACKET_EXT_MAX_PAYLOAD_LEN ||
        digest_len != SEMANTIC_DIGEST_SHA256_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    memcpy(identity->payload_digest, digest, sizeof(identity->payload_digest));
    return PROTO_OK;
}

static int gateway_collection_eack_node_list_validate(
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_collection_eack *eack,
    bool recovery)
{
    size_t offset = 0u;
    uint16_t node_count = 0u;

    if (payload == NULL || eack == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        size_t record_offset = offset;
        uint8_t type;
        uint8_t length;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        length = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (length > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        switch (type) {
        case TLV_GATEWAY_ID:
        case TLV_GATEWAY_EPOCH:
        case TLV_COMMAND_SEQ:
        case TLV_COLLECTION_EPOCH_ID:
        case TLV_MEMBERSHIP_EPOCH:
        case TLV_EXPECTED_NODE_COUNT:
        case TLV_RECEIVED_COUNT:
        case TLV_EACK_PACKET_SEQUENCE:
        case TLV_EACK_FORMAT:
        case TLV_RETRY_ROUND:
        case TLV_NEXT_RETRY_SPREAD_MS:
        case TLV_COLLECTION_OPEN:
        case TLV_NODE_ID:
            break;
        case TLV_COLLECTION_RECOVERY_ATTEMPT_ID:
        case TLV_RESULT_SEQ:
        case TLV_PAYLOAD_LEN:
        case TLV_RESULT_SHA256_COMMITMENT:
            if (!recovery) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        default:
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_NODE_ID) {
            uint64_t node_id;
            size_t prior_offset = 0u;

            if (length != sizeof(uint64_t) ||
                node_count >= PROTO_GATEWAY_COLLECTION_EACK_NODE_CAP) {
                return PROTO_ERR_MALFORMED;
            }
            node_id = proto_get_u64_le(&payload[offset]);
            if (node_id == 0u) {
                return PROTO_ERR_MALFORMED;
            }

            /*
             * Keep this validator stack-bounded: at most 50 node IDs are
             * legal, so comparing the current ID with preceding TLVs is a
             * small bounded scan and avoids a 400-byte scratch array in
             * every relay receive stack.
             */
            while (prior_offset < record_offset) {
                uint8_t prior_type;
                uint8_t prior_length;

                if (record_offset - prior_offset < PROTO_TLV_HEADER_LEN) {
                    return PROTO_ERR_MALFORMED;
                }
                prior_type = payload[prior_offset];
                prior_length = payload[prior_offset + 1u];
                prior_offset += PROTO_TLV_HEADER_LEN;
                if (prior_length > record_offset - prior_offset) {
                    return PROTO_ERR_MALFORMED;
                }
                if (prior_type == TLV_NODE_ID &&
                    prior_length == sizeof(uint64_t) &&
                    proto_get_u64_le(&payload[prior_offset]) == node_id) {
                    return PROTO_ERR_MALFORMED;
                }
                prior_offset += prior_length;
            }
            node_count++;
        }
        offset += length;
    }

    if (recovery) {
        return !eack->collection_open &&
                       eack->eack_format ==
                           EACK_FORMAT_EXPLICIT_RECEIVED_LIST &&
                       eack->expected_count == 1u &&
                       eack->received_count == 1u &&
                       eack->retry_round == 0u &&
                       eack->next_retry_spread_ms == 0u &&
                       node_count == 1u &&
                       payload_len ==
                           PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    }

    switch (eack->eack_format) {
    case EACK_FORMAT_EXPLICIT_RECEIVED_LIST:
        return node_count == eack->received_count ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    case EACK_FORMAT_EXPLICIT_MISSING_LIST:
        return node_count ==
                   (uint16_t)(eack->expected_count - eack->received_count) ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    case EACK_FORMAT_ROSTER_BITMAP:
        /* No roster-bitmap TLV exists in protocol version 1. */
        return !eack->collection_open && node_count == 0u ?
               PROTO_OK : PROTO_ERR_MALFORMED;
    default:
        return PROTO_ERR_MALFORMED;
    }
}

int gateway_collection_eack_packet_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct gateway_collection_eack *eack)
{
    struct gateway_collection_eack decoded;
    struct gateway_collection_recovery_identity recovery_identity;
    uint32_t recovery_attempt_id = 0u;
    int ret;

    if (packet == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len == 0u ||
        payload_len > PROTO_GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN ||
        packet->msg_type != MSG_GATEWAY_COLLECTION_EACK ||
        packet->flags != 0u ||
        packet->src_id == 0u ||
        packet->dst_id != 0u ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        packet->payload_len != payload_len) {
        return PROTO_ERR_MALFORMED;
    }

    ret = gateway_collection_eack_from_tlvs(payload, payload_len, &decoded);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded.gateway_id == 0u ||
        decoded.command_seq == 0u ||
        decoded.collection_epoch_id == 0u ||
        decoded.membership_epoch == 0u ||
        decoded.expected_count == 0u ||
        decoded.expected_count > PROTO_GATEWAY_COLLECTION_EACK_NODE_CAP ||
        decoded.received_count > decoded.expected_count ||
        (decoded.collection_open &&
         decoded.received_count >= decoded.expected_count) ||
        decoded.packet_sequence == 0u ||
        packet->src_id != decoded.gateway_id ||
        packet->session_id != decoded.command_seq ||
        packet->seq != decoded.packet_sequence) {
        return PROTO_ERR_MALFORMED;
    }
    ret = gateway_collection_eack_recovery_attempt_id(
        payload, payload_len, &recovery_attempt_id);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (ret == PROTO_OK && decoded.collection_open) {
        return PROTO_ERR_MALFORMED;
    }
    if (ret == PROTO_OK &&
        gateway_collection_eack_recovery_identity(payload,
                                                  payload_len,
                                                  &recovery_identity) !=
            PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = gateway_collection_eack_node_list_validate(payload,
                                                      payload_len,
                                                      &decoded,
                                                      recovery_attempt_id != 0u);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (eack != NULL) {
        *eack = decoded;
    }
    return PROTO_OK;
}

int gateway_collection_eack_contains_node_id(const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t node_id,
                                             bool *contains)
{
    size_t offset = 0u;

    if (payload == NULL || contains == NULL || node_id == 0u) {
        return PROTO_ERR_ARG;
    }

    *contains = false;
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

        if (current_type == TLV_NODE_ID) {
            uint64_t listed_node_id;

            if (current_len != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            listed_node_id = proto_get_u64_le(&payload[offset]);
            if (listed_node_id == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            if (listed_node_id == node_id) {
                *contains = true;
            }
        }

        offset += current_len;
    }

    return PROTO_OK;
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
