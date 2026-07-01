#include "gateway_command.h"

#include "mesh.h"

#include <string.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool command_wait_packet_type(uint8_t msg_type)
{
    return msg_type == MSG_COMMAND ||
           msg_type == MSG_SURVEY_PAIR_PREPARE;
}

static bool command_scope_valid(uint8_t scope)
{
    return scope == CMD_SCOPE_SINGLE_NODE ||
           scope == CMD_SCOPE_GROUP ||
           scope == CMD_SCOPE_ALL_REGISTERED ||
           scope == CMD_SCOPE_ALL_HEARD;
}

static bool command_response_mode_valid(uint8_t response_mode)
{
    return response_mode == CMD_RESPONSE_NONE ||
           response_mode == CMD_RESPONSE_ACK_ONLY ||
           response_mode == CMD_RESPONSE_SMALL_RESULT ||
           response_mode == CMD_RESPONSE_LARGE_RESULT;
}

static bool command_response_requires_collection(enum command_response_mode response_mode)
{
    return response_mode == CMD_RESPONSE_ACK_ONLY ||
           response_mode == CMD_RESPONSE_SMALL_RESULT ||
           response_mode == CMD_RESPONSE_LARGE_RESULT;
}

static uint32_t mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static int extract_optional_u8(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint8_t *out,
                               bool *present)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *present = false;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *out = value[0];
    *present = true;
    return PROTO_OK;
}

static int extract_optional_u16(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint16_t *out,
                                bool *present)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *present = false;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *out = proto_get_u16_le(value);
    *present = true;
    return PROTO_OK;
}

static int extract_optional_u32(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint32_t *out,
                                bool *present)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *present = false;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *out = proto_get_u32_le(value);
    *present = true;
    return PROTO_OK;
}

int gateway_command_extract_id(const uint8_t *payload,
                               size_t payload_len,
                               enum command_id *command_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || command_id == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, TLV_COMMAND_ID, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *command_id = (enum command_id)proto_get_u16_le(value);
    return PROTO_OK;
}

int gateway_command_extract_options(const uint8_t *payload,
                                    size_t payload_len,
                                    struct gateway_command_options *options)
{
    bool present = false;
    uint8_t value_u8 = 0u;
    uint16_t value_u16 = 0u;
    uint32_t value_u32 = 0u;
    int ret;

    if (options == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }

    memset(options, 0, sizeof(*options));
    options->scope = CMD_SCOPE_SINGLE_NODE;
    options->response_mode = CMD_RESPONSE_SMALL_RESULT;
    options->command_expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S;

    ret = extract_optional_u8(payload,
                              payload_len,
                              TLV_COMMAND_SCOPE,
                              &value_u8,
                              &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (present) {
        if (!command_scope_valid(value_u8)) {
            return PROTO_ERR_MALFORMED;
        }
        options->scope = (enum command_scope)value_u8;
    }

    ret = extract_optional_u8(payload,
                              payload_len,
                              TLV_COMMAND_RESPONSE_MODE,
                              &value_u8,
                              &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (present) {
        if (!command_response_mode_valid(value_u8)) {
            return PROTO_ERR_MALFORMED;
        }
        options->response_mode = (enum command_response_mode)value_u8;
    }

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_COMMAND_SEQ,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->command_seq = present ? value_u32 : 0u;

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_FLOOD_EPOCH_ID,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->flood_epoch_id = present ? value_u32 : 0u;

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_COLLECTION_EPOCH_ID,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->collection_epoch_id = present ? value_u32 : 0u;

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_COLLECTION_SLOT_SEED,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->collection_slot_seed = present ? value_u32 : 0u;

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_EXECUTE_DELAY_MS,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->execute_delay_ms = present ? value_u32 : 0u;

    ret = extract_optional_u32(payload,
                               payload_len,
                               TLV_COMMAND_EXPIRY_S,
                               &value_u32,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (present) {
        if (value_u32 == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        options->command_expiry_s = value_u32;
    }

    ret = extract_optional_u16(payload,
                               payload_len,
                               TLV_MEMBERSHIP_EPOCH,
                               &value_u16,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->membership_epoch = present ? value_u16 : 0u;

    ret = extract_optional_u16(payload,
                               payload_len,
                               TLV_EXPECTED_NODE_COUNT,
                               &value_u16,
                               &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    options->expected_node_count = present ? value_u16 : 0u;

    options->flood_required = options->scope != CMD_SCOPE_SINGLE_NODE;
    options->collection_required =
        options->flood_required &&
        command_response_requires_collection(options->response_mode);

    if (options->flood_required &&
        (options->command_seq == 0u || options->flood_epoch_id == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    if (options->scope == CMD_SCOPE_ALL_REGISTERED &&
        (options->membership_epoch == 0u || options->expected_node_count == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    if (options->collection_required &&
        (options->collection_epoch_id == 0u || options->collection_slot_seed == 0u)) {
        return PROTO_ERR_MALFORMED;
    }

    return PROTO_OK;
}

uint32_t gateway_command_collection_spread_ms(uint16_t expected_node_count)
{
    uint32_t spread_ms;

    spread_ms = (uint32_t)expected_node_count *
                COLLECTION_INITIAL_SPREAD_PER_NODE_MS;
    if (spread_ms < COLLECTION_INITIAL_SPREAD_MIN_MS) {
        spread_ms = COLLECTION_INITIAL_SPREAD_MIN_MS;
    }
    return spread_ms;
}

uint32_t gateway_command_collection_initial_due_ms(uint32_t command_flood_end_ms,
                                                  uint64_t node_id,
                                                  uint32_t command_seq,
                                                  uint32_t collection_slot_seed,
                                                  uint16_t expected_node_count)
{
    uint32_t spread_ms;
    uint32_t hash;

    spread_ms = gateway_command_collection_spread_ms(expected_node_count);
    hash = command_seq ^ collection_slot_seed;
    hash ^= (uint32_t)node_id;
    hash ^= (uint32_t)(node_id >> 32);
    hash = mix32(hash);
    return command_flood_end_ms + (hash % spread_ms);
}

int gateway_command_extract_role(const uint8_t *payload,
                                 size_t payload_len,
                                 enum device_role *role)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t parsed_role;
    int ret;

    if (payload == NULL || role == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, TLV_DEVICE_ROLE, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    parsed_role = value[0];
    if (parsed_role != ROLE_CLICKER &&
        parsed_role != ROLE_ANCHOR &&
        parsed_role != ROLE_GATEWAY) {
        return PROTO_ERR_MALFORMED;
    }

    *role = (enum device_role)parsed_role;
    return PROTO_OK;
}

int gateway_command_extract_duration_ms(const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t default_duration_ms,
                                        uint32_t *duration_ms)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || duration_ms == NULL || default_duration_ms == 0u) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, TLV_DURATION_MS, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *duration_ms = default_duration_ms;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *duration_ms = proto_get_u32_le(value);
    return *duration_ms == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

int gateway_command_prepare_outbound(const struct proto_packet *host_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t gateway_id,
                                     uint32_t now_ms,
                                     uint16_t fallback_seq,
                                     struct mesh_outbound *out,
                                     enum command_id *command_id)
{
    struct gateway_command_options options;
    uint32_t session_id;
    int ret;

    if (host_packet == NULL || out == NULL || command_id == NULL ||
        (payload == NULL && payload_len != 0u) ||
        payload_len > PACKET_MAX_PAYLOAD_LEN ||
        gateway_id == 0u ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->dst_id == gateway_id ||
        host_packet->payload_len != payload_len) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_command_extract_id(payload, payload_len, command_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = gateway_command_extract_options(payload, payload_len, &options);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (options.scope == CMD_SCOPE_SINGLE_NODE) {
        if (host_packet->dst_id == MESH_BROADCAST_ID) {
            return PROTO_ERR_ARG;
        }
    } else if (host_packet->dst_id != MESH_BROADCAST_ID) {
        return PROTO_ERR_ARG;
    }

    session_id = options.flood_required ? options.command_seq : host_packet->session_id;
    if (session_id == 0u) {
        session_id = now_ms == 0u ? 1u : now_ms;
    }

    memset(out, 0, sizeof(*out));
    out->packet = *host_packet;
    out->packet.src_id = gateway_id;
    out->packet.session_id = session_id;
    out->packet.seq = host_packet->seq == 0u ? fallback_seq : host_packet->seq;
    if (out->packet.seq == 0u) {
        out->packet.seq = 1u;
    }
    out->packet.ttl = host_packet->ttl != 0u ? host_packet->ttl :
        options.flood_required ? FLOOD_EPOCH_GLOBAL_TTL : MESH_DEFAULT_TTL;
    out->packet.flags = 0u;
    if ((host_packet->flags & FLAG_DIAGNOSTIC) != 0u) {
        out->packet.flags |= FLAG_DIAGNOSTIC;
    }
    out->packet.payload_len = (uint8_t)payload_len;

    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint8_t)payload_len;
    return PROTO_OK;
}

int gateway_command_build_failure_result(const struct proto_packet *command,
                                         uint64_t gateway_id,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason,
                                         uint32_t now_ms,
                                         struct proto_packet *result,
                                         uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *payload_len)
{
    int ret;

    if (command == NULL || result == NULL || payload == NULL ||
        payload_len == NULL || gateway_id == 0u) {
        return PROTO_ERR_ARG;
    }

    *payload_len = 0u;
    ret = mesh_append_command_result(payload,
                                     payload_cap,
                                     payload_len,
                                     command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(result, 0, sizeof(*result));
    result->msg_type = MSG_COMMAND_RESULT;
    result->flags = FLAG_ERROR;
    if ((command->flags & FLAG_DIAGNOSTIC) != 0u) {
        result->flags |= FLAG_DIAGNOSTIC;
    }
    result->src_id = gateway_id;
    result->dst_id = gateway_id;
    result->session_id = command->session_id == 0u ? now_ms : command->session_id;
    if (result->session_id == 0u) {
        result->session_id = 1u;
    }
    result->seq = command->seq;
    result->ttl = 1u;
    result->payload_len = (uint8_t)*payload_len;
    return PROTO_OK;
}

void gateway_command_pending_clear(struct gateway_command_pending *pending)
{
    if (pending != NULL) {
        memset(pending, 0, sizeof(*pending));
    }
}

int gateway_command_pending_start(struct gateway_command_pending *pending,
                                  const struct proto_packet *command,
                                  enum command_id command_id,
                                  uint32_t now_ms,
                                  uint32_t timeout_ms)
{
    if (pending == NULL || command == NULL || timeout_ms == 0u ||
        !command_wait_packet_type(command->msg_type) ||
        command->src_id == 0u ||
        command->dst_id == 0u ||
        command->src_id == command->dst_id ||
        command->session_id == 0u ||
        command->seq == 0u) {
        return PROTO_ERR_ARG;
    }
    if (pending->active) {
        return PROTO_ERR_MALFORMED;
    }

    memset(pending, 0, sizeof(*pending));
    pending->command = *command;
    pending->command_id = command_id;
    pending->deadline_ms = now_ms + timeout_ms;
    pending->active = true;
    return PROTO_OK;
}

bool gateway_command_pending_matches_result(const struct gateway_command_pending *pending,
                                            const struct proto_packet *result)
{
    if (pending == NULL || result == NULL || !pending->active) {
        return false;
    }

    return result->msg_type == MSG_COMMAND_RESULT &&
           result->src_id == pending->command.dst_id &&
           result->dst_id == pending->command.src_id &&
           result->session_id == pending->command.session_id &&
           result->seq == pending->command.seq;
}

bool gateway_command_pending_complete_result(struct gateway_command_pending *pending,
                                             const struct proto_packet *result)
{
    if (!gateway_command_pending_matches_result(pending, result)) {
        return false;
    }

    gateway_command_pending_clear(pending);
    return true;
}

bool gateway_command_pending_expired(struct gateway_command_pending *pending,
                                     uint32_t now_ms,
                                     struct proto_packet *command,
                                     enum command_id *command_id)
{
    if (pending == NULL || !pending->active || !deadline_reached(now_ms, pending->deadline_ms)) {
        return false;
    }

    if (command != NULL) {
        *command = pending->command;
    }
    if (command_id != NULL) {
        *command_id = pending->command_id;
    }
    gateway_command_pending_clear(pending);
    return true;
}
