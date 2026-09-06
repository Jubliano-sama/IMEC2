#include "app_anchor_actions.h"

#include <errno.h>
#include <string.h>

bool app_anchor_action_command(enum command_id command_id)
{
    return command_id == CMD_IDENTIFY_ANCHOR ||
           command_id == CMD_READ_ANCHOR_BATTERY;
}

uint32_t app_anchor_action_delivery_ms(uint8_t hop_count)
{
    return hop_count >= 1u && hop_count <= ANCHOR_ACTION_MAX_HOPS ?
        hop_count * ANCHOR_ACTION_PER_HOP_MS : 0u;
}

int app_anchor_action_request(const uint8_t *payload, size_t payload_len,
    uint32_t *assignment_epoch, uint8_t *hop_count)
{
    const uint8_t *value;
    uint8_t len;

    if (payload == NULL || assignment_epoch == NULL || hop_count == NULL ||
        payload_len != ANCHOR_ACTION_REQUEST_MAX_LEN ||
        tlv_find_unique(payload, payload_len, TLV_COMMAND_ID, &value, &len) !=
            PROTO_OK || len != 2u ||
        !app_anchor_action_command((enum command_id)proto_get_u16_le(value)) ||
        tlv_find_unique(payload, payload_len, TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                        &value, &len) != PROTO_OK || len != 4u) {
        return -EINVAL;
    }
    *assignment_epoch = proto_get_u32_le(value);
    if (*assignment_epoch == 0u ||
        tlv_find_unique(payload, payload_len, TLV_HOP_COUNT, &value, &len) !=
            PROTO_OK || len != 1u || app_anchor_action_delivery_ms(value[0]) == 0u) {
        return -EINVAL;
    }
    *hop_count = value[0];
    return 0;
}

uint8_t app_anchor_identify_color(uint32_t elapsed_ms, uint32_t *next_ms)
{
    uint32_t phase;
    uint32_t remaining;

    if (next_ms == NULL) {
        return 0u;
    }
    *next_ms = 0u;
    if (elapsed_ms >= ANCHOR_IDENTIFY_DURATION_MS) {
        return 0u;
    }
    phase = elapsed_ms % 330u;
    remaining = (phase < 250u ? 250u : 330u) - phase;
    *next_ms = remaining < ANCHOR_IDENTIFY_DURATION_MS - elapsed_ms ?
        remaining : ANCHOR_IDENTIFY_DURATION_MS - elapsed_ms;
    return phase < 250u ? (uint8_t)(1u << ((elapsed_ms / 330u) % 3u)) : 0u;
}

int app_anchor_action_result_payload(enum command_id command_id,
    uint64_t anchor_id, uint32_t assignment_epoch,
    const struct app_anchor_action_result *result,
    uint8_t *payload, size_t capacity, size_t *length)
{
    int ret;

    if (!app_anchor_action_command(command_id) || result == NULL ||
        payload == NULL || length == NULL) {
        return PROTO_ERR_ARG;
    }
    *length = 0u;
#define APPEND_ACTION_TLV(call) do { ret = (call); if (ret != PROTO_OK) return ret; } while (0)
    APPEND_ACTION_TLV(tlv_append_u16(payload, capacity, length,
                                    TLV_COMMAND_ID, command_id));
    APPEND_ACTION_TLV(tlv_append_u16(payload, capacity, length,
                                    TLV_COMMAND_STATUS, result->status));
    APPEND_ACTION_TLV(tlv_append_u8(payload, capacity, length,
                                   TLV_REASON, result->reason));
    APPEND_ACTION_TLV(tlv_append_u64(payload, capacity, length,
                                    TLV_ANCHOR_ID, anchor_id));
    APPEND_ACTION_TLV(tlv_append_u32(payload, capacity, length,
                                    TLV_DISCOVERY_ASSIGNMENT_EPOCH, assignment_epoch));
    APPEND_ACTION_TLV(tlv_append_u32(payload, capacity, length,
                                    TLV_NODE_BOOT_COUNTER, result->boot_counter));
    APPEND_ACTION_TLV(tlv_append_u64(payload, capacity, length,
                                    TLV_TIMESTAMP_MS, result->sampled_at_ms));
    if (result->status == COMMAND_OK) {
        if (command_id == CMD_READ_ANCHOR_BATTERY) {
            APPEND_ACTION_TLV(tlv_append_u16(payload, capacity, length,
                                            TLV_BATTERY_MV, result->battery_mv));
        } else {
            APPEND_ACTION_TLV(tlv_append_u32(payload, capacity, length,
                                            TLV_DURATION_MS, ANCHOR_IDENTIFY_DURATION_MS));
        }
    }
#undef APPEND_ACTION_TLV
    return PROTO_OK;
}

int app_anchor_action_execute(struct app_anchor_actions *state,
    const struct proto_packet *command, const uint8_t *payload, size_t payload_len,
    uint64_t anchor_id, uint64_t gateway_id, uint32_t boot_counter,
    uint32_t assignment_epoch,
    uint64_t now_ms, const struct app_anchor_action_ops *ops,
    struct app_anchor_action_result *result)
{
    const uint8_t *value;
    uint8_t value_len;
    enum command_id command_id;
    uint32_t requested_epoch;
    uint8_t hop_count;
    int ret;

    if (state == NULL || command == NULL || payload == NULL || ops == NULL ||
        result == NULL || ops->identify == NULL || ops->read_battery == NULL) {
        return -EINVAL;
    }
    *result = (struct app_anchor_action_result) {
        .status = COMMAND_MALFORMED_PAYLOAD,
        .reason = EINVAL,
        .boot_counter = boot_counter,
        .sampled_at_ms = now_ms,
    };
    if (command->msg_type != MSG_COMMAND || command->src_id != gateway_id ||
        command->dst_id != anchor_id || anchor_id == 0u || gateway_id == 0u ||
        command->session_id == 0u || command->seq == 0u || boot_counter == 0u ||
        command->payload_len != payload_len ||
        payload_len > ANCHOR_ACTION_REQUEST_MAX_LEN ||
        tlv_find_unique(payload, payload_len, TLV_COMMAND_ID, &value,
                        &value_len) != PROTO_OK || value_len != 2u) {
        return 0;
    }
    command_id = (enum command_id)proto_get_u16_le(value);
    if (!app_anchor_action_command(command_id)) {
        return -ENOTSUP;
    }
    if (app_anchor_action_request(payload, payload_len, &requested_epoch,
                                  &hop_count) < 0) {
        return 0;
    }
    if (assignment_epoch == 0u || requested_epoch != assignment_epoch) {
        result->status = COMMAND_INVALID_STATE;
        result->reason = ESTALE;
        return 0;
    }
    if (command->message_age_ms >= app_anchor_action_delivery_ms(hop_count)) {
        result->status = COMMAND_TIMEOUT;
        result->reason = ETIMEDOUT;
        return 0;
    }
    if (state->valid && state->result.boot_counter == boot_counter) {
        if (command->session_id == state->session_id &&
            command->seq == state->sequence) {
            if (payload_len != state->request_len ||
                memcmp(payload, state->request, payload_len) != 0) {
                result->status = COMMAND_DENIED;
                result->reason = EEXIST;
                return 0;
            }
            *result = state->result;
            return 0;
        }
        /* While packets from the current receive horizon can still arrive,
         * an older GUI command cannot replace a newer accepted action.
         * After a quiet horizon, wire age rejects delayed traffic and a
         * restarted host can establish a fresh sequence. */
        if (now_ms - state->accepted_at_ms < ANCHOR_ACTION_MAX_AGE_MS &&
            (int32_t)(command->session_id - state->session_id) <= 0) {
            result->status = COMMAND_INVALID_STATE;
            result->reason = ESTALE;
            return 0;
        }
    }

    result->status = COMMAND_OK;
    result->reason = 0u;
    ret = command_id == CMD_IDENTIFY_ANCHOR ? ops->identify(ops->ctx) :
          ops->read_battery(ops->ctx, &result->battery_mv);
    if (ret < 0) {
        result->status = COMMAND_INTERNAL_ERROR;
        result->reason = (uint8_t)(-ret);
        result->battery_mv = 0u;
    }
    state->result = *result;
    state->accepted_at_ms = now_ms;
    state->session_id = command->session_id;
    state->sequence = command->seq;
    state->command_id = (uint16_t)command_id;
    state->request_len = (uint8_t)payload_len;
    memcpy(state->request, payload, payload_len);
    state->valid = true;
    return 0;
}
