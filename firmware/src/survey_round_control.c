#include "survey_round_control.h"

#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "survey.h"

_Static_assert(
    SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS >=
        FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS +
        MESH_RADIO_WAKE_TRAIN_MS +
        FLOOD_RELAY_BURST_MS +
        FLOOD_POST_ROOT_GUARD_MS,
    "survey GO hop delay must cover synchronous local flood forwarding");

static int required_u16_tlv(const uint8_t *payload,
                            size_t payload_len,
                            uint8_t type,
                            uint16_t *value)
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
    if (raw_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(raw);
    return PROTO_OK;
}

static int required_u32_tlv(const uint8_t *payload,
                            size_t payload_len,
                            uint8_t type,
                            uint32_t *value)
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
    if (raw_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(raw);
    return PROTO_OK;
}

int survey_round_id_append_tlv(uint8_t *payload,
                               size_t payload_cap,
                               size_t *offset,
                               uint16_t round_id)
{
    if (round_id == SURVEY_LEGACY_ROUND_ID) {
        return PROTO_ERR_MALFORMED;
    }
    return tlv_append_u16(payload,
                          payload_cap,
                          offset,
                          TLV_SURVEY_ROUND_ID,
                          round_id);
}

int survey_round_id_extract_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *round_id)
{
    int ret;

    if (payload == NULL || round_id == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = required_u16_tlv(payload,
                           payload_len,
                           TLV_SURVEY_ROUND_ID,
                           round_id);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *round_id = SURVEY_LEGACY_ROUND_ID;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    return *round_id == SURVEY_LEGACY_ROUND_ID ? PROTO_ERR_MALFORMED :
                                                 PROTO_OK;
}

int survey_round_go_append_tlvs(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                const struct survey_round_go *go)
{
    int ret;

    if (go == NULL) {
        return PROTO_ERR_ARG;
    }
    if (go->survey_id == 0u || go->round_id == SURVEY_LEGACY_ROUND_ID) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_append_u16(payload,
                         payload_cap,
                         offset,
                         TLV_COMMAND_ID,
                         CMD_SURVEY_GO);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_SURVEY_ID,
                         go->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return survey_round_id_append_tlv(payload,
                                      payload_cap,
                                      offset,
                                      go->round_id);
}

int survey_round_go_from_tlvs(const uint8_t *payload,
                              size_t payload_len,
                              struct survey_round_go *go)
{
    uint16_t command_id;
    int ret;

    if (payload == NULL || go == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = required_u16_tlv(payload,
                           payload_len,
                           TLV_COMMAND_ID,
                           &command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (command_id != CMD_SURVEY_GO) {
        return PROTO_ERR_MALFORMED;
    }
    ret = required_u32_tlv(payload,
                           payload_len,
                           TLV_SURVEY_ID,
                           &go->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_round_id_extract_tlv(payload,
                                      payload_len,
                                      &go->round_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (go->survey_id == 0u || go->round_id == SURVEY_LEGACY_ROUND_ID) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_round_go_init_packet(struct proto_packet *packet,
                                uint64_t gateway_id,
                                uint32_t survey_id,
                                uint16_t seq,
                                uint16_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (gateway_id == 0u || survey_id == 0u || seq == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND;
    packet->flags = 0u;
    packet->src_id = gateway_id;
    packet->dst_id = 0u;
    packet->session_id = survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

uint32_t survey_round_go_execute_delay_ms(uint8_t gateway_hop_count)
{
    const uint8_t bounded_hops = gateway_hop_count > 0u ?
        gateway_hop_count : 1u;

    return (uint32_t)bounded_hops *
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS;
}
