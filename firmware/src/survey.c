#include "survey.h"

bool survey_sample_count_valid(uint16_t sample_count)
{
    return sample_count >= SURVEY_MIN_SAMPLE_COUNT &&
           sample_count <= SURVEY_MAX_SAMPLE_COUNT;
}

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

int survey_pair_validate(const struct survey_pair *pair)
{
    if (pair == NULL) {
        return PROTO_ERR_ARG;
    }
    if (pair->survey_id == 0u ||
        pair->initiator_id == 0u ||
        pair->responder_id == 0u ||
        pair->initiator_id == pair->responder_id ||
        !survey_sample_count_valid(pair->sample_count)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_sample_validate(const struct survey_sample *sample)
{
    int ret;

    if (sample == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_pair_validate(&sample->pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (sample->sample_index >= sample->pair.sample_count || sample->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    if (sample->range_status < RANGE_OK ||
        sample->range_status > RANGE_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_reachability_entry_validate(const struct survey_reachability_entry *entry)
{
    if (entry == NULL) {
        return PROTO_ERR_ARG;
    }
    if (entry->peer_id == 0u || entry->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_append_reach_request_tlvs(uint8_t *payload,
                                          size_t payload_cap,
                                          size_t *offset,
                                          uint32_t survey_id,
                                          uint32_t duration_ms)
{
    int ret;

    if (survey_id == 0u || duration_ms == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset, TLV_DURATION_MS, duration_ms);
}

int survey_append_reachability_entry_tlv(uint8_t *payload,
                                              size_t payload_cap,
                                              size_t *offset,
                                              const struct survey_reachability_entry *entry)
{
    uint8_t raw[SURVEY_REACHABILITY_ENTRY_LEN];
    int ret;

    ret = survey_reachability_entry_validate(entry);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u64_le(raw, entry->peer_id);
    raw[8] = (uint8_t)entry->rssi_dbm;
    raw[9] = entry->quality;
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_REACHABILITY_ENTRY,
                            raw,
                            sizeof(raw));
}

int survey_append_reach_report_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         uint32_t survey_id,
                                         uint64_t anchor_id,
                                         const struct survey_reachability_entry *entries,
                                         size_t entry_count)
{
    int ret;

    if (survey_id == 0u || anchor_id == 0u ||
        (entries == NULL && entry_count != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_ANCHOR_ID, anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        ret = survey_append_reachability_entry_tlv(payload,
                                                   payload_cap,
                                                   offset,
                                                   &entries[i]);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return PROTO_OK;
}

int survey_append_pair_tlvs(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *offset,
                                 const struct survey_pair *pair)
{
    int ret;

    ret = survey_pair_validate(pair);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, pair->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_INITIATOR_ID, pair->initiator_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_RESPONDER_ID, pair->responder_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_COUNT, pair->sample_count);
}

int survey_append_sample_tlvs(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct survey_sample *sample)
{
    int ret;

    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = survey_append_pair_tlvs(payload, payload_cap, offset, &sample->pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_INDEX, sample->sample_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_i32(payload, payload_cap, offset, TLV_DISTANCE_MM, sample->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, sample->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_RANGE_STATUS, (uint8_t)sample->range_status);
}

int survey_init_result_packet(struct proto_packet *packet,
                                   const struct survey_sample *sample,
                                   uint64_t gateway_id,
                                   uint16_t seq,
                                   uint8_t payload_len)
{
    int ret;

    if (packet == NULL || gateway_id == 0u) {
        return PROTO_ERR_ARG;
    }

    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }

    packet->msg_type = MSG_SURVEY_PAIR_RESULT;
    packet->flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = sample->pair.initiator_id;
    packet->dst_id = gateway_id;
    packet->session_id = sample->pair.survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int survey_init_reach_report_packet(struct proto_packet *packet,
                                         uint64_t anchor_id,
                                         uint64_t gateway_id,
                                         uint32_t survey_id,
                                         uint16_t seq,
                                         uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || survey_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SURVEY_REACH_REPORT;
    packet->flags = FLAG_ACK_REQUESTED | FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}
