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

int gateway_command_extract_timestamp_ms(const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t *timestamp_ms)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || timestamp_ms == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, TLV_TIMESTAMP_MS, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *timestamp_ms = proto_get_u64_le(value);
    return PROTO_OK;
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
    uint32_t session_id;
    int ret;

    if (host_packet == NULL || out == NULL || command_id == NULL ||
        (payload == NULL && payload_len != 0u) ||
        payload_len > PACKET_MAX_PAYLOAD_LEN ||
        gateway_id == 0u ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->dst_id == 0u ||
        host_packet->dst_id == gateway_id ||
        host_packet->payload_len != payload_len) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_command_extract_id(payload, payload_len, command_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    session_id = host_packet->session_id;
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
    out->packet.ttl = host_packet->ttl == 0u ? MESH_DEFAULT_TTL : host_packet->ttl;
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
