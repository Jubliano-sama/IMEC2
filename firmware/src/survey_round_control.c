#include "survey_round_control.h"

#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "survey.h"

#include <string.h>

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
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(raw);
    return PROTO_OK;
}

static bool commitment_update_u8(
    struct semantic_digest_sha256_context *context,
    uint8_t value)
{
    return semantic_digest_sha256_update(context, &value, sizeof(value));
}

static bool commitment_update_u16(
    struct semantic_digest_sha256_context *context,
    uint16_t value)
{
    uint8_t encoded[sizeof(value)];

    proto_put_u16_le(encoded, value);
    return semantic_digest_sha256_update(context, encoded, sizeof(encoded));
}

static bool commitment_update_u32(
    struct semantic_digest_sha256_context *context,
    uint32_t value)
{
    uint8_t encoded[sizeof(value)];

    proto_put_u32_le(encoded, value);
    return semantic_digest_sha256_update(context, encoded, sizeof(encoded));
}

static bool commitment_update_u64(
    struct semantic_digest_sha256_context *context,
    uint64_t value)
{
    uint8_t encoded[sizeof(value)];

    proto_put_u64_le(encoded, value);
    return semantic_digest_sha256_update(context, encoded, sizeof(encoded));
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
    if (go->operation_generation == 0u ||
        survey_operation_session_id(go->operation_generation) == 0u ||
        go->survey_id == 0u || go->round_id == SURVEY_LEGACY_ROUND_ID) {
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
    ret = survey_operation_generation_append_tlv(
        payload,
        payload_cap,
        offset,
        go->operation_generation);
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(payload,
                                         payload_cap,
                                         offset,
                                         go->round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_append_tlv(
            payload,
            payload_cap,
            offset,
            go->round_commitment);
    }
    return ret;
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
    ret = survey_operation_generation_extract_tlv(
        payload, payload_len, &go->operation_generation);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_round_id_extract_tlv(payload,
                                      payload_len,
                                      &go->round_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_round_commitment_extract_tlv(
        payload, payload_len, go->round_commitment);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (go->survey_id == 0u || go->round_id == SURVEY_LEGACY_ROUND_ID ||
        survey_operation_session_id(go->operation_generation) == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_round_go_init_packet(struct proto_packet *packet,
                                uint64_t gateway_id,
                                uint32_t operation_session_id,
                                uint16_t seq,
                                uint16_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (gateway_id == 0u || operation_session_id == 0u || seq == 0u ||
        payload_len == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    /*
     * GO is the network-wide broadcast phase boundary. Unlike targeted
     * PREPARE/START/ABORT, it must use the canonical global command TTL so
     * relay envelope validation and every supported route depth agree.
     */
    *packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = 0u,
        .src_id = gateway_id,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = operation_session_id,
        .seq = seq,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
    return PROTO_OK;
}

uint32_t survey_round_go_execute_delay_ms(uint8_t gateway_hop_count)
{
    const uint8_t bounded_hops = gateway_hop_count > 0u ?
        gateway_hop_count : 1u;

    return (uint32_t)bounded_hops *
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS;
}

int survey_round_commitment_compute(
    const struct survey_round_plan_identity *identity,
    const struct survey_round_plan_entry *entries,
    size_t entry_count,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    static const uint8_t domain[] = "IMEC-SURVEY-ROUND-V1";
    struct semantic_digest_sha256_context context;

    if (identity == NULL || entries == NULL || commitment == NULL ||
        entry_count == 0u || entry_count > UINT8_MAX ||
        identity->operation_generation == 0u ||
        identity->operation_session_id == 0u ||
        identity->operation_session_id !=
            survey_operation_session_id(identity->operation_generation) ||
        identity->survey_id == 0u ||
        identity->round_id == SURVEY_LEGACY_ROUND_ID ||
        identity->execute_delay_ms == 0u ||
        identity->observation_window_ms == 0u ||
        identity->max_parallel_pairs == 0u ||
        entry_count > identity->max_parallel_pairs ||
        !semantic_digest_sha256_init(&context) ||
        !semantic_digest_sha256_update(&context, domain, sizeof(domain)) ||
        !commitment_update_u64(&context, identity->operation_generation) ||
        !commitment_update_u32(&context, identity->survey_id) ||
        !commitment_update_u32(&context, identity->operation_session_id) ||
        !commitment_update_u16(&context, identity->round_id) ||
        !commitment_update_u8(&context, (uint8_t)entry_count) ||
        !commitment_update_u8(&context, identity->max_parallel_pairs) ||
        !commitment_update_u8(&context, identity->max_reruns) ||
        !commitment_update_u32(&context, identity->execute_delay_ms) ||
        !commitment_update_u32(&context, identity->observation_window_ms) ||
        !commitment_update_u16(&context, CMD_SURVEY_PREPARE_PAIR) ||
        !commitment_update_u16(&context, CMD_SURVEY_START_PAIR) ||
        !commitment_update_u16(&context, CMD_SURVEY_GO) ||
        !commitment_update_u8(&context, CMD_SCOPE_ALL_HEARD) ||
        !commitment_update_u8(&context, CMD_RESPONSE_NONE) ||
        !commitment_update_u32(&context,
                               SURVEY_PAIR_START_SKEW_MARGIN_MS) ||
        !commitment_update_u32(&context,
                               SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS)) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t i = 0u; i < entry_count; i++) {
        const struct survey_round_plan_entry *entry = &entries[i];

        if (entry->lane_index != i ||
            entry->pair.operation_generation !=
                identity->operation_generation ||
            entry->pair.survey_id != identity->survey_id ||
            survey_pair_validate(&entry->pair) != PROTO_OK ||
            !commitment_update_u8(&context, entry->lane_index) ||
            !commitment_update_u8(&context, entry->plan_pair_index) ||
            !commitment_update_u8(&context, entry->reruns_started) ||
            !commitment_update_u64(&context,
                                   entry->pair.initiator_id) ||
            !commitment_update_u64(&context,
                                   entry->pair.responder_id) ||
            !commitment_update_u16(&context,
                                   entry->pair.sample_count)) {
            memset(&context, 0, sizeof(context));
            return PROTO_ERR_MALFORMED;
        }
    }
    return semantic_digest_sha256_final(&context, commitment) ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

int survey_pair_control_commitment_compute(
    const struct survey_pair *pair,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    static const uint8_t domain[] = "IMEC-SURVEY-PAIR-V1";
    struct semantic_digest_sha256_context context;

    if (pair == NULL || commitment == NULL ||
        pair->operation_generation == 0u ||
        survey_operation_session_id(pair->operation_generation) == 0u ||
        survey_pair_validate(pair) != PROTO_OK ||
        !semantic_digest_sha256_init(&context) ||
        !semantic_digest_sha256_update(&context, domain, sizeof(domain)) ||
        !commitment_update_u64(&context, pair->operation_generation) ||
        !commitment_update_u32(&context, pair->survey_id) ||
        !commitment_update_u32(
            &context,
            survey_operation_session_id(pair->operation_generation)) ||
        !commitment_update_u64(&context, pair->initiator_id) ||
        !commitment_update_u64(&context, pair->responder_id) ||
        !commitment_update_u16(&context, pair->sample_count) ||
        !commitment_update_u16(&context, CMD_SURVEY_PREPARE_PAIR) ||
        !commitment_update_u16(&context, CMD_SURVEY_START_PAIR) ||
        !commitment_update_u8(&context, CMD_SCOPE_SINGLE_NODE) ||
        !commitment_update_u8(&context, CMD_RESPONSE_ACK_ONLY)) {
        return PROTO_ERR_MALFORMED;
    }
    return semantic_digest_sha256_final(&context, commitment) ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

int survey_round_commitment_append_tlv(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (commitment == NULL) {
        return PROTO_ERR_ARG;
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_SURVEY_ROUND_COMMITMENT,
                            commitment,
                            SEMANTIC_DIGEST_SHA256_LEN);
}

int survey_round_commitment_extract_tlv(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || commitment == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_SURVEY_ROUND_COMMITMENT,
                          &value,
                          &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != SEMANTIC_DIGEST_SHA256_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    memcpy(commitment, value, SEMANTIC_DIGEST_SHA256_LEN);
    return PROTO_OK;
}
