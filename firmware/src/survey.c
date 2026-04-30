#include "survey.h"

bool survey_sample_count_valid(uint16_t sample_count)
{
    return sample_count >= SURVEY_MIN_SAMPLE_COUNT &&
           sample_count <= SURVEY_MAX_SAMPLE_COUNT;
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
