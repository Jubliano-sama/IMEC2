#include "gateway_command.h"

#include "gateway_membership.h"
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

static bool collection_eack_format_valid(uint8_t eack_format)
{
    return eack_format == EACK_FORMAT_ROSTER_BITMAP ||
           eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST ||
           eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST;
}

static int ensure_tlv_u32(uint8_t *payload,
                          size_t payload_cap,
                          size_t current_len,
                          size_t *offset,
                          uint8_t type,
                          uint32_t value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, current_len, type, &tlv_value, &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return tlv_append_u32(payload, payload_cap, offset, type, value);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_len == sizeof(uint32_t) ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static int ensure_tlv_u16(uint8_t *payload,
                          size_t payload_cap,
                          size_t current_len,
                          size_t *offset,
                          uint8_t type,
                          uint16_t value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, current_len, type, &tlv_value, &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return tlv_append_u16(payload, payload_cap, offset, type, value);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_len == sizeof(uint16_t) ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static int ensure_tlv_u8(uint8_t *payload,
                         size_t payload_cap,
                         size_t current_len,
                         size_t *offset,
                         uint8_t type,
                         uint8_t value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, current_len, type, &tlv_value, &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return tlv_append_u8(payload, payload_cap, offset, type, value);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_len == sizeof(uint8_t) ? PROTO_OK : PROTO_ERR_MALFORMED;
}

int gateway_command_append_default_flood_controls(struct mesh_outbound *out)
{
    size_t offset;
    size_t original_len;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    offset = out->payload_len;
    original_len = out->payload_len;
    ret = ensure_tlv_u32(out->payload,
                         sizeof(out->payload),
                         original_len,
                         &offset,
                         TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                         FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = ensure_tlv_u16(out->payload,
                         sizeof(out->payload),
                         original_len,
                         &offset,
                         TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                         FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = ensure_tlv_u8(out->payload,
                        sizeof(out->payload),
                        original_len,
                        &offset,
                        TLV_FLOOD_RETRY_COUNT,
                        FLOOD_DEFAULT_RETRY_COUNT);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = ensure_tlv_u32(out->payload,
                         sizeof(out->payload),
                         original_len,
                         &offset,
                         TLV_FLOOD_PACKET_AGE_MS,
                         0u);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->payload_len = (uint16_t)offset;
    out->packet.payload_len = (uint16_t)offset;
    return PROTO_OK;
}

static bool command_response_requires_collection(enum command_response_mode response_mode)
{
    return response_mode == CMD_RESPONSE_ACK_ONLY ||
           response_mode == CMD_RESPONSE_SMALL_RESULT ||
           response_mode == CMD_RESPONSE_LARGE_RESULT;
}

static uint32_t command_expiry_ms(const struct gateway_command_options *options)
{
    if (options == NULL || options->command_expiry_s == 0u) {
        return 0u;
    }
    if (options->command_expiry_s > UINT32_MAX / 1000u) {
        return UINT32_MAX;
    }
    return options->command_expiry_s * 1000u;
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

static int extract_required_u32(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint32_t *out)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (out == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *out = proto_get_u32_le(value);
    return PROTO_OK;
}

static int extract_expected_node_roster(const uint8_t *payload,
                                        size_t payload_len,
                                        struct gateway_command_options *options)
{
    size_t offset = 0u;

    if (options == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t value_len;

        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset++];
        value_len = payload[offset++];
        if ((size_t)value_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }

        if (type == TLV_EXPECTED_NODE_ID) {
            if (value_len != sizeof(uint64_t) ||
                options->expected_node_id_count >= GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP) {
                return PROTO_ERR_MALFORMED;
            }
            options->expected_node_ids[options->expected_node_id_count++] =
                proto_get_u64_le(&payload[offset]);
        }
        offset += value_len;
    }

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

    ret = extract_expected_node_roster(payload, payload_len, options);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (options->expected_node_id_count != 0u &&
        (options->scope != CMD_SCOPE_ALL_REGISTERED ||
         options->expected_node_id_count != options->expected_node_count)) {
        return PROTO_ERR_MALFORMED;
    }

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
        (options->collection_epoch_id == 0u ||
         options->collection_slot_seed == 0u ||
         options->membership_epoch == 0u ||
         options->expected_node_count == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

enum gateway_command_tracking_mode gateway_command_tracking_mode_from_options(
    const struct gateway_command_options *options)
{
    if (options == NULL) {
        return GATEWAY_COMMAND_TRACK_NONE;
    }
    if (options->scope == CMD_SCOPE_SINGLE_NODE) {
        return GATEWAY_COMMAND_TRACK_LEGACY_RESULT;
    }
    if (options->response_mode == CMD_RESPONSE_NONE) {
        return GATEWAY_COMMAND_TRACK_NONE;
    }
    if (options->collection_required) {
        return GATEWAY_COMMAND_TRACK_COLLECTION;
    }
    return GATEWAY_COMMAND_TRACK_NONE;
}

enum gateway_command_transport_mode gateway_command_transport_mode_from_outbound(
    const struct mesh_outbound *out)
{
    if (out != NULL &&
        out->packet.dst_id == MESH_BROADCAST_ID &&
        out->next_hop_id == MESH_BROADCAST_ID) {
        return GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST;
    }
    return GATEWAY_COMMAND_TRANSPORT_UNICAST_TRACKED;
}

int gateway_command_resolve_collection_roster(
    const struct gateway_command_options *options,
    const struct gateway_membership_roster *membership_roster,
    uint64_t *out_node_ids,
    size_t out_cap,
    size_t *out_count,
    enum gateway_command_collection_roster_source *source)
{
    size_t membership_count = 0u;
    int ret;

    if (options == NULL || out_count == NULL || source == NULL ||
        (out_node_ids == NULL && out_cap != 0u)) {
        return PROTO_ERR_ARG;
    }

    *out_count = 0u;
    *source = GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;

    if (!options->collection_required ||
        options->scope != CMD_SCOPE_ALL_REGISTERED) {
        return PROTO_OK;
    }

    if (options->expected_node_id_count != 0u) {
        if (options->expected_node_id_count != options->expected_node_count) {
            return PROTO_ERR_MALFORMED;
        }
        if (out_cap < options->expected_node_id_count) {
            return PROTO_ERR_NO_SPACE;
        }
        memcpy(out_node_ids,
               options->expected_node_ids,
               options->expected_node_id_count * sizeof(options->expected_node_ids[0]));
        *out_count = options->expected_node_id_count;
        *source = GATEWAY_COMMAND_COLLECTION_ROSTER_EXPLICIT;
        return PROTO_OK;
    }

    ret = gateway_membership_export_node_ids_preserve_order(membership_roster,
                                                            options->membership_epoch,
                                                            out_node_ids,
                                                            out_cap,
                                                            &membership_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (membership_count != options->expected_node_count) {
        return PROTO_ERR_MALFORMED;
    }

    *out_count = membership_count;
    *source = GATEWAY_COMMAND_COLLECTION_ROSTER_MEMBERSHIP;
    return PROTO_OK;
}

bool gateway_command_receive_expired(const struct proto_packet *packet,
                                     const struct gateway_command_options *options)
{
    uint32_t expiry_ms = command_expiry_ms(options);

    if (packet == NULL || options == NULL || expiry_ms == 0u) {
        return true;
    }
    return packet->message_age_ms >= expiry_ms ||
           (options->execute_delay_ms != 0u &&
            options->execute_delay_ms >= expiry_ms);
}

uint32_t gateway_command_expiry_remaining_ms(const struct proto_packet *packet,
                                             const struct gateway_command_options *options)
{
    uint32_t expiry_ms = command_expiry_ms(options);

    if (packet == NULL || options == NULL ||
        expiry_ms == 0u || packet->message_age_ms >= expiry_ms) {
        return 0u;
    }
    return expiry_ms - packet->message_age_ms;
}

uint32_t gateway_command_execute_delay_remaining_ms(
    const struct proto_packet *packet,
    const struct gateway_command_options *options)
{
    if (packet == NULL || options == NULL ||
        options->execute_delay_ms <= packet->message_age_ms) {
        return 0u;
    }
    return options->execute_delay_ms - packet->message_age_ms;
}

static void command_rx_duplicate_expire(struct gateway_command_rx_duplicate_cache *cache,
                                        uint32_t now_ms)
{
    size_t i;

    if (cache == NULL) {
        return;
    }
    for (i = 0u; i < GATEWAY_COMMAND_RX_DUP_CACHE_SIZE; i++) {
        struct gateway_command_rx_duplicate_entry *entry = &cache->entries[i];

        if (entry->valid &&
            (entry->lifetime_ms == 0u ||
             (uint32_t)(now_ms - entry->stored_at_ms) >= entry->lifetime_ms)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

bool gateway_command_rx_duplicate_seen(struct gateway_command_rx_duplicate_cache *cache,
                                       uint32_t command_seq,
                                       uint32_t now_ms)
{
    size_t i;

    if (cache == NULL || command_seq == 0u) {
        return false;
    }

    command_rx_duplicate_expire(cache, now_ms);
    for (i = 0u; i < GATEWAY_COMMAND_RX_DUP_CACHE_SIZE; i++) {
        if (cache->entries[i].valid &&
            cache->entries[i].command_seq == command_seq) {
            return true;
        }
    }
    return false;
}

void gateway_command_rx_duplicate_store(struct gateway_command_rx_duplicate_cache *cache,
                                        const struct proto_packet *packet,
                                        const struct gateway_command_options *options,
                                        uint32_t now_ms)
{
    struct gateway_command_rx_duplicate_entry *entry;
    uint32_t lifetime_ms;

    if (cache == NULL || packet == NULL || options == NULL ||
        options->command_seq == 0u) {
        return;
    }

    lifetime_ms = gateway_command_expiry_remaining_ms(packet, options);
    if (lifetime_ms == 0u) {
        return;
    }

    command_rx_duplicate_expire(cache, now_ms);
    entry = &cache->entries[cache->next];
    entry->command_seq = options->command_seq;
    entry->stored_at_ms = now_ms;
    entry->lifetime_ms = lifetime_ms;
    entry->valid = true;
    cache->next = (uint8_t)((cache->next + 1u) % GATEWAY_COMMAND_RX_DUP_CACHE_SIZE);
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

uint32_t gateway_command_collection_retry_spread_ms(uint8_t retry_round)
{
    switch (retry_round) {
    case 0u:
        return COLLECTION_RETRY_ROUND_0_MS;
    case 1u:
        return COLLECTION_RETRY_ROUND_1_MS;
    case 2u:
        return COLLECTION_RETRY_ROUND_2_MS;
    case 3u:
        return COLLECTION_RETRY_ROUND_3_MS;
    default:
        return COLLECTION_RETRY_ROUND_STEADY_MS;
    }
}

int gateway_command_append_collection_result_identity(uint8_t *payload,
                                                      size_t payload_cap,
                                                      size_t *payload_len,
                                                      const struct command_result_id *id,
                                                      uint32_t collection_epoch_id)
{
    int ret;

    if (payload == NULL || payload_len == NULL || id == NULL) {
        return PROTO_ERR_ARG;
    }
    if (id->gateway_id == 0u ||
        id->command_seq == 0u ||
        id->node_id == 0u ||
        id->node_boot_counter == 0u ||
        id->result_seq == 0u ||
        collection_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = command_result_id_append_tlvs(payload, payload_cap, payload_len, id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          collection_epoch_id);
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
    if (options.flood_required) {
        out->next_hop_id = MESH_BROADCAST_ID;
        out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
        ret = gateway_command_append_default_flood_controls(out);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return PROTO_OK;
}

int gateway_command_build_result(const struct proto_packet *command,
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
    result->flags = status == COMMAND_OK ? 0u : FLAG_ERROR;
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

static bool command_result_id_equal(const struct command_result_id *a,
                                    const struct command_result_id *b)
{
    return a->gateway_id == b->gateway_id &&
           a->gateway_epoch == b->gateway_epoch &&
           a->command_seq == b->command_seq &&
           a->node_id == b->node_id &&
           a->node_boot_counter == b->node_boot_counter &&
           a->result_seq == b->result_seq;
}

static int result_bundle_first_record_offset(const uint8_t *payload,
                                             size_t payload_len,
                                             size_t *record_offset)
{
    size_t offset = 0u;

    if (payload == NULL || record_offset == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        if (payload_len - offset - 2u < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_RESULT_RECORD) {
            *record_offset = offset;
            return PROTO_OK;
        }
        offset += (size_t)len + 2u;
    }

    return PROTO_ERR_NOT_FOUND;
}

static void gateway_collection_refresh_open(struct gateway_collection_state *collection)
{
    if (collection->expected_count != 0u &&
        collection->received_count >= collection->expected_count) {
        collection->collection_open = false;
    }
}

void gateway_collection_clear(struct gateway_collection_state *collection)
{
    if (collection != NULL) {
        memset(collection, 0, sizeof(*collection));
    }
}

int gateway_collection_start(struct gateway_collection_state *collection,
                             uint64_t gateway_id,
                             uint16_t gateway_epoch,
                             uint32_t command_seq,
                             uint32_t collection_epoch_id,
                             uint16_t membership_epoch,
                             uint16_t expected_count,
                             uint8_t retry_round,
                             uint32_t next_retry_spread_ms)
{
    if (collection == NULL ||
        gateway_id == 0u ||
        command_seq == 0u ||
        collection_epoch_id == 0u ||
        membership_epoch == 0u ||
        expected_count == 0u) {
        return PROTO_ERR_ARG;
    }

    memset(collection, 0, sizeof(*collection));
    collection->gateway_id = gateway_id;
    collection->gateway_epoch = gateway_epoch;
    collection->command_seq = command_seq;
    collection->collection_epoch_id = collection_epoch_id;
    collection->membership_epoch = membership_epoch;
    collection->expected_count = expected_count;
    collection->retry_round = retry_round;
    collection->next_retry_spread_ms = next_retry_spread_ms;
    collection->collection_open = true;
    return PROTO_OK;
}

bool gateway_collection_contains_result(const struct gateway_collection_state *collection,
                                        const struct command_result_id *id)
{
    if (collection == NULL || id == NULL) {
        return false;
    }

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &collection->results[i];

        if (entry->valid && command_result_id_equal(&entry->id, id)) {
            return true;
        }
    }
    return false;
}

static bool gateway_collection_has_node_result(const struct gateway_collection_state *collection,
                                               uint64_t node_id)
{
    if (collection == NULL || node_id == 0u) {
        return false;
    }

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &collection->results[i];

        if (entry->valid && entry->id.node_id == node_id) {
            return true;
        }
    }
    return false;
}

int gateway_collection_record_result(struct gateway_collection_state *collection,
                                     const struct proto_packet *result,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     bool *duplicate)
{
    return gateway_collection_record_result_from_hop(collection,
                                                     result,
                                                     payload,
                                                     payload_len,
                                                     0u,
                                                     duplicate);
}

int gateway_collection_record_result_from_hop(struct gateway_collection_state *collection,
                                             const struct proto_packet *result,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             bool *duplicate)
{
    struct command_result_id id;
    struct gateway_collection_result_entry *free_entry = NULL;
    uint32_t collection_epoch_id = 0u;
    int ret;

    if (collection == NULL || result == NULL || duplicate == NULL ||
        (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (result->msg_type != MSG_COMMAND_RESULT ||
        result->dst_id != collection->gateway_id ||
        result->payload_len != payload_len) {
        return PROTO_ERR_MALFORMED;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, &id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (id.gateway_id != collection->gateway_id ||
        id.gateway_epoch != collection->gateway_epoch ||
        id.command_seq != collection->command_seq ||
        id.node_id == 0u ||
        result->src_id != id.node_id) {
        return PROTO_ERR_MALFORMED;
    }
    ret = extract_required_u32(payload,
                               payload_len,
                               TLV_COLLECTION_EPOCH_ID,
                               &collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (collection_epoch_id != collection->collection_epoch_id) {
        return PROTO_ERR_MALFORMED;
    }

    *duplicate = false;
    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        struct gateway_collection_result_entry *entry = &collection->results[i];

        if (entry->valid) {
            if (command_result_id_equal(&entry->id, &id)) {
                *duplicate = true;
                return PROTO_OK;
            }
            continue;
        }
        if (free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (!collection->collection_open) {
        return PROTO_ERR_STALE;
    }
    if (free_entry == NULL) {
        return PROTO_ERR_NO_SPACE;
    }

    free_entry->id = id;
    free_entry->previous_hop_id = previous_hop_id;
    free_entry->payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    free_entry->payload_len = (uint16_t)payload_len;
    free_entry->valid = true;
    if (collection->received_count < UINT16_MAX) {
        collection->received_count++;
    }
    gateway_collection_refresh_open(collection);
    return PROTO_OK;
}

int gateway_collection_record_bundle(struct gateway_collection_state *collection,
                                     const struct proto_packet *bundle_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint16_t *accepted_count,
                                     uint16_t *duplicate_count)
{
    return gateway_collection_record_bundle_from_hop(collection,
                                                     bundle_packet,
                                                     payload,
                                                     payload_len,
                                                     0u,
                                                     accepted_count,
                                                     duplicate_count);
}

int gateway_collection_record_bundle_from_hop(struct gateway_collection_state *collection,
                                             const struct proto_packet *bundle_packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             uint16_t *accepted_count,
                                             uint16_t *duplicate_count)
{
    struct result_bundle_header bundle;
    size_t cursor = 0u;
    uint8_t parsed_count = 0u;
    int ret;

    if (accepted_count != NULL) {
        *accepted_count = 0u;
    }
    if (duplicate_count != NULL) {
        *duplicate_count = 0u;
    }

    if (collection == NULL || bundle_packet == NULL ||
        (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (bundle_packet->msg_type != MSG_RESULT_BUNDLE ||
        bundle_packet->dst_id != collection->gateway_id ||
        bundle_packet->payload_len != payload_len) {
        return PROTO_ERR_MALFORMED;
    }

    ret = result_bundle_header_from_tlvs(payload, payload_len, &bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (bundle.gateway_id != collection->gateway_id ||
        bundle.gateway_epoch != collection->gateway_epoch ||
        bundle.command_seq != collection->command_seq ||
        bundle.collection_epoch_id != collection->collection_epoch_id ||
        bundle.record_count == 0u ||
        bundle.record_count > COLLECTION_BUNDLE_MAX_RECORDS ||
        bundle_packet->session_id != bundle.command_seq) {
        return PROTO_ERR_MALFORMED;
    }

    ret = result_bundle_first_record_offset(payload, payload_len, &cursor);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (proto_crc16_ccitt_false(&payload[cursor], payload_len - cursor) !=
        bundle.bundle_crc) {
        return PROTO_ERR_BAD_CRC;
    }

    while (cursor < payload_len) {
        struct result_bundle_record record;
        struct command_result_id payload_id;
        struct proto_packet result = {0};
        bool duplicate = false;
        size_t before = cursor;

        if (payload[cursor] != TLV_RESULT_RECORD) {
            return PROTO_ERR_MALFORMED;
        }
        ret = result_bundle_record_next_from_tlvs(payload,
                                                  payload_len,
                                                  &cursor,
                                                  &record);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (cursor == before || parsed_count >= bundle.record_count) {
            return PROTO_ERR_MALFORMED;
        }
        parsed_count++;

        if (record.result_id.gateway_id != bundle.gateway_id ||
            record.result_id.gateway_epoch != bundle.gateway_epoch ||
            record.result_id.command_seq != bundle.command_seq ||
            record.result_id.node_id == 0u ||
            command_result_id_from_tlvs(record.payload,
                                        record.payload_len,
                                        &payload_id) != PROTO_OK ||
            !command_result_id_equal(&record.result_id, &payload_id)) {
            return PROTO_ERR_MALFORMED;
        }

        result.msg_type = MSG_COMMAND_RESULT;
        result.src_id = record.result_id.node_id;
        result.dst_id = collection->gateway_id;
        result.session_id = record.result_id.command_seq;
        result.seq = record.result_id.result_seq;
        result.ttl = 1u;
        result.payload_len = record.payload_len;
        ret = gateway_collection_record_result_from_hop(collection,
                                                        &result,
                                                        record.payload,
                                                        record.payload_len,
                                                        previous_hop_id,
                                                        &duplicate);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (duplicate) {
            if (duplicate_count != NULL && *duplicate_count < UINT16_MAX) {
                (*duplicate_count)++;
            }
        } else if (accepted_count != NULL && *accepted_count < UINT16_MAX) {
            (*accepted_count)++;
        }
    }

    return parsed_count == bundle.record_count ? PROTO_OK : PROTO_ERR_MALFORMED;
}

size_t gateway_collection_return_candidates(const struct gateway_collection_state *collection,
                                            uint64_t *out,
                                            size_t out_cap)
{
    size_t count = 0u;

    if (collection == NULL || (out == NULL && out_cap != 0u)) {
        return 0u;
    }

    for (size_t offset = 0u; offset < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; offset++) {
        const size_t i = GATEWAY_COLLECTION_RESULT_CACHE_SIZE - 1u - offset;
        const struct gateway_collection_result_entry *entry = &collection->results[i];
        bool duplicate = false;

        if (!entry->valid ||
            entry->previous_hop_id == 0u ||
            entry->previous_hop_id == MESH_BROADCAST_ID) {
            continue;
        }

        for (size_t j = 0u; j < count; j++) {
            if (out[j] == entry->previous_hop_id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (count >= out_cap) {
            break;
        }
        out[count] = entry->previous_hop_id;
        count++;
    }

    return count;
}

static int gateway_collection_snapshot_validate(
    const struct gateway_collection_state_snapshot *snapshot,
    struct gateway_collection_state *restored)
{
    struct gateway_collection_state tmp;
    uint16_t valid_count = 0u;

    if (snapshot == NULL || restored == NULL) {
        return PROTO_ERR_ARG;
    }
    if (snapshot->version != GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (!snapshot->valid) {
        return PROTO_ERR_MALFORMED;
    }
    if (snapshot->gateway_id == 0u ||
        snapshot->command_seq == 0u ||
        snapshot->collection_epoch_id == 0u ||
        snapshot->membership_epoch == 0u ||
        snapshot->expected_count == 0u ||
        snapshot->received_count > snapshot->expected_count ||
        snapshot->received_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        snapshot->result_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        snapshot->result_count != snapshot->received_count ||
        (snapshot->received_count >= snapshot->expected_count && snapshot->collection_open)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.gateway_id = snapshot->gateway_id;
    tmp.gateway_epoch = snapshot->gateway_epoch;
    tmp.command_seq = snapshot->command_seq;
    tmp.collection_epoch_id = snapshot->collection_epoch_id;
    tmp.membership_epoch = snapshot->membership_epoch;
    tmp.expected_count = snapshot->expected_count;
    tmp.received_count = snapshot->received_count;
    tmp.retry_round = snapshot->retry_round;
    tmp.next_retry_spread_ms = snapshot->next_retry_spread_ms;
    tmp.collection_open = snapshot->collection_open;

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &snapshot->results[i];

        if (!entry->valid) {
            continue;
        }
        if (entry->id.gateway_id != snapshot->gateway_id ||
            entry->id.gateway_epoch != snapshot->gateway_epoch ||
            entry->id.command_seq != snapshot->command_seq ||
            entry->id.node_id == 0u ||
            gateway_collection_contains_result(&tmp, &entry->id)) {
            return PROTO_ERR_MALFORMED;
        }
        if (valid_count >= snapshot->result_count) {
            return PROTO_ERR_MALFORMED;
        }

        tmp.results[i] = *entry;
        valid_count++;
    }
    if (valid_count != snapshot->result_count) {
        return PROTO_ERR_MALFORMED;
    }

    *restored = tmp;
    return PROTO_OK;
}

int gateway_collection_export_snapshot(
    const struct gateway_collection_state *collection,
    struct gateway_collection_state_snapshot *snapshot)
{
    struct gateway_collection_state_snapshot tmp;
    uint16_t result_count = 0u;

    if (collection == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (collection->gateway_id == 0u ||
        collection->command_seq == 0u ||
        collection->collection_epoch_id == 0u ||
        collection->membership_epoch == 0u ||
        collection->expected_count == 0u ||
        collection->received_count > collection->expected_count ||
        collection->received_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        (collection->received_count >= collection->expected_count && collection->collection_open)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.version = GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION;
    tmp.valid = true;
    tmp.gateway_id = collection->gateway_id;
    tmp.gateway_epoch = collection->gateway_epoch;
    tmp.command_seq = collection->command_seq;
    tmp.collection_epoch_id = collection->collection_epoch_id;
    tmp.membership_epoch = collection->membership_epoch;
    tmp.expected_count = collection->expected_count;
    tmp.received_count = collection->received_count;
    tmp.retry_round = collection->retry_round;
    tmp.next_retry_spread_ms = collection->next_retry_spread_ms;
    tmp.collection_open = collection->collection_open;

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &collection->results[i];

        if (!entry->valid) {
            continue;
        }
        if (entry->id.gateway_id != collection->gateway_id ||
            entry->id.gateway_epoch != collection->gateway_epoch ||
            entry->id.command_seq != collection->command_seq ||
            entry->id.node_id == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            const struct gateway_collection_result_entry *prior = &collection->results[j];

            if (prior->valid && command_result_id_equal(&prior->id, &entry->id)) {
                return PROTO_ERR_MALFORMED;
            }
        }
        if (result_count >= collection->received_count) {
            return PROTO_ERR_MALFORMED;
        }
        tmp.results[i] = *entry;
        result_count++;
    }
    if (result_count != collection->received_count) {
        return PROTO_ERR_MALFORMED;
    }
    tmp.result_count = result_count;

    *snapshot = tmp;
    return PROTO_OK;
}

int gateway_collection_restore_snapshot(
    struct gateway_collection_state *collection,
    const struct gateway_collection_state_snapshot *snapshot)
{
    struct gateway_collection_state restored;
    int ret;

    if (collection == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_collection_snapshot_validate(snapshot, &restored);
    if (ret != PROTO_OK) {
        return ret;
    }

    *collection = restored;
    return PROTO_OK;
}

int gateway_collection_build_eack(const struct gateway_collection_state *collection,
                                  uint8_t eack_format,
                                  struct gateway_collection_eack *eack)
{
    if (collection == NULL || eack == NULL || !collection_eack_format_valid(eack_format)) {
        return PROTO_ERR_ARG;
    }
    if (collection->gateway_id == 0u ||
        collection->command_seq == 0u ||
        collection->collection_epoch_id == 0u ||
        collection->membership_epoch == 0u ||
        collection->expected_count == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(eack, 0, sizeof(*eack));
    eack->gateway_id = collection->gateway_id;
    eack->gateway_epoch = collection->gateway_epoch;
    eack->command_seq = collection->command_seq;
    eack->collection_epoch_id = collection->collection_epoch_id;
    eack->membership_epoch = collection->membership_epoch;
    eack->expected_count = collection->expected_count;
    eack->received_count = collection->received_count;
    eack->eack_format = eack_format;
    eack->retry_round = collection->retry_round;
    eack->next_retry_spread_ms = collection->next_retry_spread_ms;
    eack->collection_open = collection->collection_open;
    return PROTO_OK;
}

int gateway_collection_prepare_eack_outbound(const struct gateway_collection_state *collection,
                                             uint8_t eack_format,
                                             struct mesh_outbound *out)
{
    struct gateway_collection_eack eack;
    size_t payload_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_collection_build_eack(collection, eack_format, &eack);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    ret = gateway_collection_eack_append_tlvs(out->payload,
                                              sizeof(out->payload),
                                              &payload_len,
                                              &eack);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST) {
        for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
            if (!collection->results[i].valid) {
                continue;
            }
            ret = tlv_append_u64(out->payload,
                                 sizeof(out->payload),
                                 &payload_len,
                                 TLV_NODE_ID,
                                 collection->results[i].id.node_id);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
    }

    out->packet.msg_type = MSG_GATEWAY_COLLECTION_EACK;
    out->packet.src_id = collection->gateway_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = collection->command_seq;
    out->packet.seq = collection->retry_round == 0u ? 1u : collection->retry_round;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

int gateway_collection_prepare_missing_eack_outbound(
    const struct gateway_collection_state *collection,
    const uint64_t *expected_node_ids,
    size_t expected_node_count,
    struct mesh_outbound *out,
    uint16_t *missing_count)
{
    struct gateway_collection_eack eack;
    size_t payload_len = 0u;
    uint16_t missing = 0u;
    int ret;

    if (out == NULL || (expected_node_ids == NULL && expected_node_count != 0u) ||
        expected_node_count > UINT16_MAX) {
        return PROTO_ERR_ARG;
    }
    if (collection != NULL &&
        collection->expected_count != 0u &&
        expected_node_count != collection->expected_count) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_collection_build_eack(collection,
                                        EACK_FORMAT_EXPLICIT_MISSING_LIST,
                                        &eack);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    ret = gateway_collection_eack_append_tlvs(out->payload,
                                              sizeof(out->payload),
                                              &payload_len,
                                              &eack);
    if (ret != PROTO_OK) {
        return ret;
    }

    for (size_t i = 0u; i < expected_node_count; i++) {
        uint64_t node_id = expected_node_ids[i];

        if (node_id == 0u) {
            return PROTO_ERR_ARG;
        }
        if (gateway_collection_has_node_result(collection, node_id)) {
            continue;
        }

        ret = tlv_append_u64(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_NODE_ID,
                             node_id);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (missing < UINT16_MAX) {
            missing++;
        }
    }

    out->packet.msg_type = MSG_GATEWAY_COLLECTION_EACK;
    out->packet.src_id = collection->gateway_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = collection->command_seq;
    out->packet.seq = collection->retry_round == 0u ? 1u : collection->retry_round;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    if (missing_count != NULL) {
        *missing_count = missing;
    }
    return PROTO_OK;
}

int gateway_collection_advance_retry_round(struct gateway_collection_state *collection)
{
    if (collection == NULL) {
        return PROTO_ERR_ARG;
    }
    if (collection->gateway_id == 0u ||
        collection->command_seq == 0u ||
        collection->collection_epoch_id == 0u ||
        collection->membership_epoch == 0u ||
        collection->expected_count == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    if (collection->retry_round < UINT8_MAX) {
        collection->retry_round++;
    }
    collection->next_retry_spread_ms =
        gateway_command_collection_retry_spread_ms(collection->retry_round);
    return PROTO_OK;
}
