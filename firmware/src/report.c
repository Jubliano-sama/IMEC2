#include "report.h"

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK && status <= RANGE_INTERNAL_ERROR;
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
    ret = tlv_append_i32(payload, payload_cap, offset, TLV_DISTANCE_MM, fields->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, fields->quality);
    if (ret != PROTO_OK) {
        return ret;
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

int report_init_click_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_CLICK_REPORT;
    packet->flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
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
    if (!ids_are_valid(clicker_id, gateway_id) || session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SELF_TEST_REPORT;
    packet->flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = clicker_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}
