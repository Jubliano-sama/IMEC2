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
    case MSG_ERROR:
        return true;
    default:
        return false;
    }
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

static int tlv_require_u8(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
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

    if (offer == NULL) {
        return PROTO_ERR_ARG;
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
    return tlv_append_u8(payload, payload_cap, offset, TLV_PRIORITY, offer->priority);
}

int result_offer_from_tlvs(const uint8_t *payload,
                           size_t payload_len,
                           struct result_offer *offer)
{
    int ret;

    if (offer == NULL) {
        return PROTO_ERR_ARG;
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
    return tlv_require_u8(payload, payload_len, TLV_PRIORITY, &offer->priority);
}

int result_grant_append_tlvs(uint8_t *payload,
                             size_t payload_cap,
                             size_t *offset,
                             const struct result_grant *grant)
{
    int ret;

    if (grant == NULL) {
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

    if (grant == NULL) {
        return PROTO_ERR_ARG;
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

    if (busy == NULL) {
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
    int ret;

    if (busy == NULL) {
        return PROTO_ERR_ARG;
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
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    busy->has_optional_alternate_parent = true;
    busy->optional_alternate_parent = alternate_parent;
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

int gateway_collection_eack_packet_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    struct gateway_collection_eack *eack)
{
    struct gateway_collection_eack decoded;
    int ret;

    if (packet == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN ||
        packet->msg_type != MSG_GATEWAY_COLLECTION_EACK ||
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
        decoded.received_count > decoded.expected_count ||
        decoded.packet_sequence == 0u ||
        packet->src_id != decoded.gateway_id ||
        packet->session_id != decoded.command_seq ||
        packet->seq != decoded.packet_sequence) {
        return PROTO_ERR_MALFORMED;
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

    if (payload == NULL || contains == NULL) {
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
            if (current_len != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            if (proto_get_u64_le(&payload[offset]) == node_id) {
                *contains = true;
                return PROTO_OK;
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
