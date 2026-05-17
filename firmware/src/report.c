#include "report.h"

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK && status <= RANGE_TIMING_INVALID;
}

static int append_distance_samples(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct range_report_fields *fields)
{
    uint8_t samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET * sizeof(int32_t)];
    uint16_t chunk_count;
    bool fragmented;
    size_t sample_bytes;
    int ret;

    if (fields->sample_count == 0u) {
        return PROTO_OK;
    }
    chunk_count = fields->distance_sample_count == 0u ?
                  fields->sample_count :
                  fields->distance_sample_count;
    fragmented = fields->sample_index != 0u || chunk_count != fields->sample_count;
    if (fields->distance_samples_mm == NULL ||
        fields->sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES ||
        chunk_count == 0u ||
        chunk_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET ||
        (fragmented && chunk_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT) ||
        fields->sample_index > fields->sample_count ||
        chunk_count > fields->sample_count - fields->sample_index) {
        return PROTO_ERR_MALFORMED;
    }

    sample_bytes = (size_t)chunk_count * sizeof(int32_t);
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_COUNT,
                         fields->sample_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fragmented) {
        ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_INDEX,
                             fields->sample_index);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    for (uint16_t i = 0u; i < chunk_count; i++) {
        proto_put_u32_le(&samples[(size_t)i * sizeof(int32_t)],
                         (uint32_t)fields->distance_samples_mm[i]);
    }
    return tlv_append_bytes(payload, payload_cap, offset,
                            TLV_DISTANCE_SAMPLES_MM,
                            samples, (uint8_t)sample_bytes);
}

int report_append_range_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct range_report_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->clicker_id == 0u ||
        fields->anchor_id == 0u ||
        fields->clicker_id == fields->anchor_id ||
        fields->quality > 100u ||
        !range_status_valid(fields->range_status)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_CLICKER_ID, fields->clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_ANCHOR_ID, fields->anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_EVENT_SEQ, fields->event_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_TIMESTAMP_MS, fields->timestamp_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_i32(payload, payload_cap, offset, TLV_DISTANCE_MM, fields->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = append_distance_samples(payload, payload_cap, offset, fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, fields->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!fields->omit_rsl) {
        ret = tlv_append_i8(payload, payload_cap, offset, TLV_UWB_RSL_DBM, fields->rsl_dbm);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (!fields->omit_cir && fields->cir_sample != NULL) {
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_UWB_CIR_SAMPLE,
                               fields->cir_sample,
                               UWB_CIR_SAMPLE_LEN);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_RANGE_STATUS, (uint8_t)fields->range_status);
}

int report_append_self_test_tlvs(uint8_t *payload,
                                      size_t payload_cap,
                                      size_t *offset,
                                      const struct self_test_report_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->clicker_id == 0u || fields->failure_code > 6u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_CLICKER_ID, fields->clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_EVENT_SEQ, fields->event_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_ERROR_CODE, fields->failure_code);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_BATTERY_MV, fields->battery_mv);
}

int report_append_anchor_heartbeat_tlvs(uint8_t *payload,
                                        size_t payload_cap,
                                        size_t *offset,
                                        const struct anchor_heartbeat_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->device_role == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u8(payload, payload_cap, offset, TLV_DEVICE_ROLE, fields->device_role);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_BATTERY_MV, fields->battery_mv);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_STATUS_BITS, fields->status_bits);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset, TLV_UPTIME_MS, fields->uptime_ms);
}

int report_init_range_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t report_flags,
                                  uint8_t payload_len)
{
    uint8_t mode_flags = report_flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC);

    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) ||
        session_id == 0u ||
        seq == 0u ||
        mode_flags == 0u ||
        mode_flags == (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_CLICK_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | mode_flags;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int report_init_anchor_heartbeat_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t session_id,
                                        uint16_t seq,
                                        uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || session_id == 0u || seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_ANCHOR_HEARTBEAT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int report_init_click_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len)
{
    return report_init_range_packet(packet,
                                    anchor_id,
                                    gateway_id,
                                    session_id,
                                    seq,
                                    FLAG_COUNT_AS_CLICK,
                                    payload_len);
}

int report_init_self_test_packet(struct proto_packet *packet,
                                      uint64_t clicker_id,
                                      uint64_t gateway_id,
                                      uint32_t session_id,
                                      uint16_t seq,
                                      uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(clicker_id, gateway_id) || session_id == 0u || seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SELF_TEST_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = clicker_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}
