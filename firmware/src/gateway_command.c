#include "gateway_command.h"

#include "gateway_membership.h"
#include "mesh.h"

#include <string.h>

_Static_assert(FLOOD_EPOCH_GLOBAL_TTL == MESH_NETWORK_MAX_HOPS,
               "ordinary gateway commands must cover the reverse-route contract");
_Static_assert(MESH_DEFAULT_TTL <= FLOOD_EPOCH_GLOBAL_TTL,
               "bounded command TTL exceeds the network command TTL");

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool command_wait_packet_type(uint8_t msg_type)
{
    return msg_type == MSG_COMMAND;
}

static bool command_scope_valid(uint8_t scope)
{
    return scope == CMD_SCOPE_SINGLE_NODE ||
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

    ret = tlv_find_unique(payload, current_len, type, &tlv_value, &tlv_len);
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

    ret = tlv_find_unique(payload, current_len, type, &tlv_value, &tlv_len);
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

    ret = tlv_find_unique(payload, current_len, type, &tlv_value, &tlv_len);
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

    ret = tlv_find_unique(payload, payload_len, type, &value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, type, &value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, type, &value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, type, &value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, TLV_COMMAND_ID,
                          &value, &value_len);
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

int gateway_command_rebind_broadcast_sequence(uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t command_seq)
{
    const uint8_t *command_seq_value = NULL;
    const uint8_t *flood_epoch_value = NULL;
    uint8_t command_seq_len = 0u;
    uint8_t flood_epoch_len = 0u;
    int ret;

    if (payload == NULL || command_seq == 0u) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_SEQ,
                          &command_seq_value,
                          &command_seq_len);
    if (ret == PROTO_OK) {
        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_FLOOD_EPOCH_ID,
                              &flood_epoch_value,
                              &flood_epoch_len);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (command_seq_len != sizeof(uint32_t) ||
        flood_epoch_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    proto_put_u32_le((uint8_t *)command_seq_value, command_seq);
    proto_put_u32_le((uint8_t *)flood_epoch_value, command_seq);
    return PROTO_OK;
}

uint8_t gateway_command_origin_ttl(enum command_id command_id)
{
    (void)command_id;
    return FLOOD_EPOCH_GLOBAL_TTL;
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
        ret = gateway_membership_export_node_ids_preserve_order(
            membership_roster,
            options->membership_epoch,
            out_node_ids,
            out_cap,
            &membership_count);
        if (ret != PROTO_OK ||
            membership_count != options->expected_node_id_count) {
            return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
        }
        for (uint16_t i = 0u; i < options->expected_node_id_count; i++) {
            bool found = false;

            for (size_t j = 0u; j < membership_count; j++) {
                if (out_node_ids[j] == options->expected_node_ids[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return PROTO_ERR_STALE;
            }
        }
        memcpy(out_node_ids,
               options->expected_node_ids,
               options->expected_node_id_count *
                   sizeof(options->expected_node_ids[0]));
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

enum command_rx_serial_order {
    COMMAND_RX_SERIAL_OLDER = 0,
    COMMAND_RX_SERIAL_EQUAL,
    COMMAND_RX_SERIAL_NEWER,
    COMMAND_RX_SERIAL_AMBIGUOUS,
};

static enum command_rx_serial_order command_rx_serial_compare(
    uint32_t candidate,
    uint32_t reference)
{
    const uint32_t difference = candidate - reference;

    if (difference == 0u) {
        return COMMAND_RX_SERIAL_EQUAL;
    }
    if (difference == GATEWAY_COMMAND_RX_SERIAL_HALF_RANGE) {
        return COMMAND_RX_SERIAL_AMBIGUOUS;
    }
    return difference < GATEWAY_COMMAND_RX_SERIAL_HALF_RANGE ?
        COMMAND_RX_SERIAL_NEWER : COMMAND_RX_SERIAL_OLDER;
}

bool gateway_command_rx_duplicate_seen(struct gateway_command_rx_duplicate_cache *cache,
                                       uint32_t command_seq,
                                       uint32_t now_ms)
{
    enum command_rx_serial_order order;

    if (cache == NULL || command_seq == 0u) {
        return false;
    }
    (void)now_ms;
    if (!cache->initialized || cache->newest_command_seq == 0u ||
        cache->committed == 0u) {
        return false;
    }

    order = command_rx_serial_compare(
        command_seq, cache->newest_command_seq);
    return order != COMMAND_RX_SERIAL_NEWER;
}

void gateway_command_rx_duplicate_store(struct gateway_command_rx_duplicate_cache *cache,
                                        const struct proto_packet *packet,
                                        const struct gateway_command_options *options,
                                        uint32_t now_ms)
{
    enum command_rx_serial_order order;

    if (cache == NULL || packet == NULL || options == NULL ||
        options->command_seq == 0u) {
        return;
    }
    (void)packet;
    (void)now_ms;

    if (!cache->initialized || cache->newest_command_seq == 0u ||
        cache->committed == 0u) {
        cache->newest_command_seq = options->command_seq;
        cache->committed = GATEWAY_COMMAND_RX_PERSISTED_MARKER;
        cache->initialized = true;
        return;
    }

    order = command_rx_serial_compare(
        options->command_seq, cache->newest_command_seq);
    if (order == COMMAND_RX_SERIAL_NEWER) {
        cache->newest_command_seq = options->command_seq;
    }
    cache->committed = GATEWAY_COMMAND_RX_PERSISTED_MARKER;
}

bool gateway_command_applies_to_node(
    const struct gateway_command_options *options,
    uint64_t local_id,
    bool assignment_provisioned,
    uint32_t assignment_epoch)
{
    if (options == NULL || local_id == 0u) {
        return false;
    }
    if (options->scope == CMD_SCOPE_ALL_HEARD) {
        return true;
    }
    if (options->scope != CMD_SCOPE_ALL_REGISTERED) {
        return false;
    }
    /*
     * The gateway validates an explicit collection roster against its durable
     * current membership before flooding the command.  A retained member can
     * legitimately have an older local assignment epoch after missing the
     * latest enumeration, so explicit inclusion is the authority in this
     * form.  Without an explicit roster the anchor must prove that its local
     * assignment belongs to the command's membership generation.
     */
    if (options->expected_node_id_count != 0u) {
        for (uint16_t i = 0u; i < options->expected_node_id_count; i++) {
            if (options->expected_node_ids[i] == local_id) {
                return true;
            }
        }
        return false;
    }
    if (!assignment_provisioned || assignment_epoch == 0u ||
        options->membership_epoch == 0u) {
        return false;
    }
    {
        uint16_t membership_epoch =
            (uint16_t)(assignment_epoch ^ (assignment_epoch >> 16u));

        if (membership_epoch == 0u) {
            membership_epoch = 1u;
        }
        if (membership_epoch != options->membership_epoch) {
            return false;
        }
    }
    return true;
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

    ret = tlv_find_unique(payload, payload_len, TLV_DEVICE_ROLE,
                          &value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, TLV_DURATION_MS,
                          &value, &value_len);
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

int gateway_command_extract_budget_ms(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t default_budget_ms,
                                      uint32_t *budget_ms,
                                      bool *explicit_budget)
{
    bool present = false;
    int ret;

    if (payload == NULL || budget_ms == NULL || explicit_budget == NULL ||
        default_budget_ms < GATEWAY_COMMAND_BUDGET_MIN_MS ||
        default_budget_ms > GATEWAY_COMMAND_BUDGET_MAX_MS) {
        return PROTO_ERR_ARG;
    }
    ret = extract_optional_u32(payload, payload_len, TLV_COMMAND_BUDGET_MS,
                               budget_ms, &present);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!present) {
        *budget_ms = default_budget_ms;
        *explicit_budget = false;
        return PROTO_OK;
    }
    if (*budget_ms < GATEWAY_COMMAND_BUDGET_MIN_MS ||
        *budget_ms > GATEWAY_COMMAND_BUDGET_MAX_MS) {
        return PROTO_ERR_MALFORMED;
    }
    *explicit_budget = true;
    return PROTO_OK;
}

uint32_t gateway_command_budget_window_ms(bool explicit_budget,
                                          uint32_t remaining_ms,
                                          uint8_t phases_remaining,
                                          uint32_t natural_window_ms)
{
    if (natural_window_ms == 0u || phases_remaining == 0u) {
        return 0u;
    }
    if (!explicit_budget) {
        return natural_window_ms;
    }
    if (remaining_ms == 0u) {
        return 0u;
    }
    return remaining_ms < natural_window_ms ? remaining_ms :
                                              natural_window_ms;
}

uint32_t gateway_command_budget_weighted_window_ms(bool explicit_budget,
                                                   uint32_t remaining_ms,
                                                   uint8_t phase_weight,
                                                   uint8_t total_weight,
                                                   uint32_t natural_window_ms)
{
    if (!explicit_budget) {
        return natural_window_ms;
    }
    if (remaining_ms == 0u || natural_window_ms == 0u ||
        phase_weight == 0u || total_weight == 0u ||
        phase_weight > total_weight) {
        return 0u;
    }
    return remaining_ms < natural_window_ms ? remaining_ms :
                                              natural_window_ms;
}

uint8_t gateway_command_budget_retry_limit(bool explicit_budget,
                                           uint32_t budget_ms,
                                           uint8_t default_limit)
{
    uint32_t limit;

    if (default_limit == 0u) {
        return 0u;
    }
    if (!explicit_budget) {
        return default_limit;
    }
    limit = 1u + budget_ms / GATEWAY_COMMAND_BUDGET_RETRY_QUANTUM_MS;
    return limit < default_limit ? (uint8_t)limit : default_limit;
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
    /*
     * Host TTL is transport metadata, not operation policy.  Normalize it
     * before the first RF owner sees the packet so every receiver derives the
     * same reverse-route depth from the command class.
     */
    out->packet.ttl = gateway_command_origin_ttl(*command_id);
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
    /* This result is synthesized by the gateway and retained by the BLE
     * stream, not forwarded over RF.  Its explicit host-receipt marker keeps
     * it separate from an anchor's mesh command result, which has distinct
     * endpoints and its own gateway-ACK custody. */
    result->flags = FLAG_GATEWAY_ACK_REQUIRED |
                    (status == COMMAND_OK ? 0u : FLAG_ERROR);
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
    if (timeout_ms == 0u || timeout_ms > (uint32_t)INT32_MAX) {
        return PROTO_ERR_ARG;
    }
    return gateway_command_pending_start_until(pending,
                                               command,
                                               command_id,
                                               now_ms,
                                               now_ms + timeout_ms);
}

int gateway_command_pending_start_until(
    struct gateway_command_pending *pending,
    const struct proto_packet *command,
    enum command_id command_id,
    uint32_t now_ms,
    uint32_t absolute_deadline_ms)
{
    uint32_t remaining_ms = absolute_deadline_ms - now_ms;

    if (pending == NULL || command == NULL || remaining_ms == 0u ||
        remaining_ms > (uint32_t)INT32_MAX ||
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
    pending->started_at_ms = now_ms;
    pending->deadline_ms = absolute_deadline_ms;
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

enum gateway_command_result_admission gateway_command_result_admit(
    const struct gateway_command_pending *pending,
    const struct proto_packet *result,
    bool transaction_owned,
    bool transaction_accepted)
{
    if (!gateway_command_pending_matches_result(pending, result)) {
        return GATEWAY_COMMAND_RESULT_IGNORE;
    }
    if (transaction_owned && !transaction_accepted) {
        return GATEWAY_COMMAND_RESULT_WAIT;
    }
    return GATEWAY_COMMAND_RESULT_ACCEPT;
}

enum gateway_command_pending_result_claim
gateway_command_pending_claim_result(
    struct gateway_command_pending *pending,
    const struct proto_packet *result,
    uint32_t now_ms,
    struct proto_packet *command,
    enum command_id *command_id)
{
    enum gateway_command_pending_result_claim claim;
    struct proto_packet original_command;
    enum command_id original_command_id;

    if (!gateway_command_pending_matches_result(pending, result)) {
        return GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE;
    }
    if (!deadline_reached(now_ms, pending->started_at_ms)) {
        return GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE;
    }

    original_command = pending->command;
    original_command_id = pending->command_id;
    claim = deadline_reached(now_ms, pending->deadline_ms) ?
            GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED :
            GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED;
    gateway_command_pending_clear(pending);
    if (command != NULL) {
        *command = original_command;
    }
    if (command_id != NULL) {
        *command_id = original_command_id;
    }
    return claim;
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

void gateway_command_result_validation_clear(
    struct gateway_command_result_validation_leases *leases)
{
    if (leases != NULL) {
        uint32_t next_token = leases->next_token;

        memset(leases, 0, sizeof(*leases));
        leases->next_token = next_token;
    }
}

static bool validation_token_in_use(
    const struct gateway_command_result_validation_leases *leases,
    uint32_t token)
{
    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        if ((leases->entries[i].token_state & UINT32_C(0x7fffffff)) ==
            token) {
            return true;
        }
    }
    return false;
}

static int gateway_command_result_validation_allocate(
    struct gateway_command_result_validation_leases *leases,
    uint32_t timestamp_ms,
    uint32_t expires_at_ms,
    bool completed,
    uint32_t *token)
{
    struct gateway_command_result_validation_lease *empty = NULL;
    uint32_t candidate;

    if (leases == NULL || token == NULL ||
        deadline_reached(timestamp_ms, expires_at_ms)) {
        return PROTO_ERR_ARG;
    }

    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        if (leases->entries[i].token_state == 0u) {
            empty = &leases->entries[i];
            break;
        }
    }
    if (empty == NULL) {
        return PROTO_ERR_NO_SPACE;
    }

    candidate = leases->next_token;
    for (size_t i = 0u;
         i <= GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        candidate = (candidate + 1u) & UINT32_C(0x7fffffff);
        if (candidate != 0u &&
            !validation_token_in_use(leases, candidate)) {
            break;
        }
    }
    if (candidate == 0u || validation_token_in_use(leases, candidate)) {
        return PROTO_ERR_NO_SPACE;
    }

    leases->next_token = candidate;
    *empty = (struct gateway_command_result_validation_lease) {
        .timestamp_ms = timestamp_ms,
        .expires_at_ms = expires_at_ms,
        .token_state = candidate |
            (completed ? UINT32_C(0x80000000) : 0u),
    };
    *token = candidate;
    return PROTO_OK;
}

int gateway_command_result_validation_arm(
    struct gateway_command_result_validation_leases *leases,
    uint32_t armed_at_ms,
    uint32_t expires_at_ms,
    uint32_t *token)
{
    return gateway_command_result_validation_allocate(
        leases, armed_at_ms, expires_at_ms, false, token);
}

bool gateway_command_result_validation_complete(
    struct gateway_command_result_validation_leases *leases,
    uint32_t token,
    uint32_t received_at_ms)
{
    if (leases == NULL || token == 0u) {
        return false;
    }
    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        struct gateway_command_result_validation_lease *entry =
            &leases->entries[i];

        if ((entry->token_state & UINT32_C(0x7fffffff)) == token &&
            (entry->token_state & UINT32_C(0x80000000)) == 0u) {
            entry->timestamp_ms = received_at_ms;
            /*
             * The armed expiry only bounds the blocking radio receive.  Once
             * a frame exists, semantic validation can legitimately include
             * bounded queueing and the exact GUI-receipt custody round trip.
             * Give the completed lease the same processing budget as a lease
             * acquired from an already-decoded result; retaining the short RX
             * expiry makes the command timeout win while valid result bytes
             * are still owned by the gateway.
             */
            entry->expires_at_ms =
                received_at_ms +
                GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS;
            entry->token_state |= UINT32_C(0x80000000);
            return true;
        }
    }
    return false;
}

int gateway_command_result_validation_acquire(
    struct gateway_command_result_validation_leases *leases,
    const struct gateway_command_pending *pending,
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t *token)
{
    uint32_t received_at_32 = (uint32_t)received_at_ms;
    uint32_t expires_at_ms;

    if (leases == NULL || pending == NULL || result == NULL ||
        token == NULL ||
        !gateway_command_pending_matches_result(pending, result) ||
        !deadline_reached(received_at_32, pending->started_at_ms) ||
        deadline_reached(received_at_32, pending->deadline_ms)) {
        return PROTO_ERR_NOT_FOUND;
    }
    expires_at_ms = received_at_32 +
                    GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS;
    return gateway_command_result_validation_allocate(
        leases, received_at_32, expires_at_ms, true, token);
}

bool gateway_command_result_validation_contains(
    const struct gateway_command_result_validation_leases *leases,
    const struct gateway_command_pending *pending,
    uint32_t token,
    const struct proto_packet *result,
    uint64_t received_at_ms)
{
    if (leases == NULL || pending == NULL || token == 0u ||
        !gateway_command_pending_matches_result(pending, result)) {
        return false;
    }
    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        const struct gateway_command_result_validation_lease *entry =
            &leases->entries[i];

        if ((entry->token_state & UINT32_C(0x80000000)) != 0u &&
            (entry->token_state & UINT32_C(0x7fffffff)) == token &&
            entry->timestamp_ms == (uint32_t)received_at_ms) {
            return true;
        }
    }
    return false;
}

bool gateway_command_result_validation_release(
    struct gateway_command_result_validation_leases *leases,
    uint32_t token)
{
    if (leases == NULL || token == 0u) {
        return false;
    }
    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        if ((leases->entries[i].token_state & UINT32_C(0x7fffffff)) ==
            token) {
            memset(&leases->entries[i], 0,
                   sizeof(leases->entries[i]));
            return true;
        }
    }
    return false;
}

enum gateway_command_result_validation_check
gateway_command_result_validation_check_interval(
    const struct gateway_command_result_validation_leases *leases,
    uint32_t started_at_ms,
    uint32_t deadline_ms,
    uint32_t now_ms)
{
    bool blocked = false;

    if (leases == NULL ||
        !deadline_reached(deadline_ms, started_at_ms) ||
        deadline_ms == started_at_ms) {
        return GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;
    }
    for (size_t i = 0u;
         i < GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP;
         i++) {
        const struct gateway_command_result_validation_lease *entry =
            &leases->entries[i];
        uint32_t token = entry->token_state & UINT32_C(0x7fffffff);
        bool completed;
        bool relevant;

        if (token == 0u) {
            continue;
        }
        completed =
            (entry->token_state & UINT32_C(0x80000000)) != 0u;
        if (!completed) {
            /*
             * An armed receive may already have completed in hardware while
             * the owning thread is preempted. It can affect a boundary only
             * while its bounded receive/processing interval overlaps that
             * boundary's interval. An abandoned old receive must not poison
             * every later, unrelated protocol phase after it expires.
             */
            relevant =
                !deadline_reached(entry->timestamp_ms, deadline_ms) &&
                !deadline_reached(started_at_ms, entry->expires_at_ms);
        } else {
            relevant =
                deadline_reached(entry->timestamp_ms, started_at_ms) &&
                !deadline_reached(entry->timestamp_ms, deadline_ms);
        }
        if (!relevant) {
            continue;
        }
        if (deadline_reached(now_ms, entry->expires_at_ms)) {
            return GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED;
        }
        blocked = true;
    }
    return blocked ? GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED :
                     GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;
}

bool gateway_command_result_validation_blocks_timeout(
    const struct gateway_command_result_validation_leases *leases,
    const struct gateway_command_pending *pending)
{
    if (leases == NULL || pending == NULL || !pending->active) {
        return false;
    }
    return gateway_command_result_validation_check_interval(
               leases,
               pending->started_at_ms,
               pending->deadline_ms,
               pending->deadline_ms) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED;
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

static bool gateway_collection_result_id_equal(
    const struct gateway_collection_result_id *a,
    const struct gateway_collection_result_id *b)
{
    return a->node_id == b->node_id &&
           a->node_boot_counter == b->node_boot_counter &&
           a->result_seq == b->result_seq;
}

static int gateway_collection_roster_validate(
    uint64_t gateway_id,
    uint16_t expected_count,
    const uint64_t *expected_node_ids,
    size_t expected_node_id_count)
{
    if (expected_node_id_count == 0u) {
        return PROTO_OK;
    }
    if (expected_node_ids == NULL ||
        expected_node_id_count != expected_count ||
        expected_node_id_count > GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t i = 0u; i < expected_node_id_count; i++) {
        if (expected_node_ids[i] == 0u || expected_node_ids[i] == gateway_id) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            if (expected_node_ids[j] == expected_node_ids[i]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    return PROTO_OK;
}

static bool gateway_collection_node_expected(
    const struct gateway_collection_state *collection,
    uint64_t node_id)
{
    if (collection == NULL || node_id == 0u) {
        return false;
    }
    if (collection->expected_node_id_count == 0u) {
        return true;
    }

    for (size_t i = 0u; i < collection->expected_node_id_count; i++) {
        if (collection->expected_node_ids[i] == node_id) {
            return true;
        }
    }
    return false;
}

struct gateway_collection_staged_result {
    struct gateway_collection_result_id id;
    const uint8_t *payload;
    uint16_t payload_len;
    uint8_t result_slot;
};

_Static_assert(GATEWAY_COLLECTION_RESULT_CACHE_SIZE <= UINT8_MAX,
               "bundle staging indexes must address the result cache");
_Static_assert(COLLECTION_BUNDLE_MAX_RECORDS == 8u,
               "bundle projection mask must cover every wire record");
_Static_assert(COLLECTION_BUNDLE_MAX_RECORDS *
               sizeof(struct gateway_collection_staged_result) <= 256u,
               "bundle preflight staging must stay workqueue-stack bounded");

static const struct gateway_collection_staged_result *
gateway_collection_staged_find(
    const struct gateway_collection_staged_result *staged,
    uint8_t staged_count,
    const struct command_result_id *id)
{
    const struct gateway_collection_result_id candidate = {
        .node_id = id->node_id,
        .node_boot_counter = id->node_boot_counter,
        .result_seq = id->result_seq,
    };

    for (uint8_t i = 0u; i < staged_count; i++) {
        if (gateway_collection_result_id_equal(&staged[i].id, &candidate)) {
            return &staged[i];
        }
    }
    return NULL;
}

static bool gateway_collection_staged_has_node(
    const struct gateway_collection_staged_result *staged,
    uint8_t staged_count,
    uint64_t node_id)
{
    for (uint8_t i = 0u; i < staged_count; i++) {
        if (staged[i].id.node_id == node_id) {
            return true;
        }
    }
    return false;
}

static bool gateway_collection_payload_digest(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return payload_len <= UINT16_MAX &&
           semantic_digest_sha256(payload, payload_len, digest);
}

static bool gateway_collection_next_free_result_slot(
    const struct gateway_collection_state *collection,
    size_t *next_slot,
    uint8_t *result_slot)
{
    while (*next_slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE &&
           collection->results[*next_slot].valid) {
        (*next_slot)++;
    }
    if (*next_slot >= GATEWAY_COLLECTION_RESULT_CACHE_SIZE) {
        return false;
    }

    *result_slot = (uint8_t)*next_slot;
    (*next_slot)++;
    return true;
}

static int gateway_collection_bundle_record_validate(
    const struct gateway_collection_state *collection,
    const struct result_bundle_header *bundle,
    const struct result_bundle_record *record)
{
    struct command_result_id payload_id;
    uint32_t collection_epoch_id = 0u;
    int ret;

    if (record->result_id.gateway_id != bundle->gateway_id ||
        record->result_id.gateway_epoch != bundle->gateway_epoch ||
        record->result_id.command_seq != bundle->command_seq ||
        record->result_id.node_id == 0u ||
        command_result_id_from_tlvs(record->payload,
                                    record->payload_len,
                                    &payload_id) != PROTO_OK ||
        !command_result_id_equal(&record->result_id, &payload_id)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = extract_required_u32(record->payload,
                               record->payload_len,
                               TLV_COLLECTION_EPOCH_ID,
                               &collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (collection_epoch_id != collection->collection_epoch_id) {
        return PROTO_ERR_MALFORMED;
    }
    return gateway_collection_node_expected(collection, record->result_id.node_id) ?
           PROTO_OK : PROTO_ERR_NOT_FOUND;
}

static bool gateway_collection_entry_matches_id(
    const struct gateway_collection_state *collection,
    const struct gateway_collection_result_entry *entry,
    const struct command_result_id *id)
{
    return collection->gateway_id == id->gateway_id &&
           collection->gateway_epoch == id->gateway_epoch &&
           collection->command_seq == id->command_seq &&
           entry->id.node_id == id->node_id &&
           entry->id.node_boot_counter == id->node_boot_counter &&
           entry->id.result_seq == id->result_seq;
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
        expected_count == 0u ||
        expected_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE) {
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
    collection->eack_sequence = (uint16_t)retry_round + 1u;
    collection->next_retry_spread_ms = next_retry_spread_ms;
    collection->collection_open = true;
    collection->eack_pending = true;
    return PROTO_OK;
}

int gateway_collection_set_expected_roster(
    struct gateway_collection_state *collection,
    const uint64_t *expected_node_ids,
    size_t expected_node_id_count)
{
    int ret;

    if (collection == NULL || collection->gateway_id == 0u ||
        collection->command_seq == 0u || collection->collection_epoch_id == 0u ||
        collection->received_count != 0u) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_collection_roster_validate(collection->gateway_id,
                                             collection->expected_count,
                                             expected_node_ids,
                                             expected_node_id_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (expected_node_id_count != 0u &&
        expected_node_ids != collection->expected_node_ids) {
        memmove(collection->expected_node_ids,
                expected_node_ids,
                expected_node_id_count * sizeof(collection->expected_node_ids[0]));
    }
    if (expected_node_id_count < GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP) {
        memset(&collection->expected_node_ids[expected_node_id_count],
               0,
               (GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP - expected_node_id_count) *
                   sizeof(collection->expected_node_ids[0]));
    }
    collection->expected_node_id_count = (uint16_t)expected_node_id_count;
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

        if (entry->valid && gateway_collection_entry_matches_id(collection, entry, id)) {
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

static const struct gateway_collection_result_entry *
gateway_collection_find_result(
    const struct gateway_collection_state *collection,
    const struct command_result_id *id)
{
    if (collection == NULL || id == NULL) {
        return NULL;
    }

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &collection->results[i];

        if (entry->valid && gateway_collection_entry_matches_id(collection, entry, id)) {
            return entry;
        }
    }
    return NULL;
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

static int gateway_collection_result_from_hop(
    const struct gateway_collection_state *collection,
    struct gateway_collection_state *mutable_collection,
    const struct proto_packet *result,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    bool *duplicate)
{
    struct command_result_id id;
    size_t free_entry_index = SIZE_MAX;
    uint32_t collection_epoch_id = 0u;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
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
        result->session_id != id.command_seq ||
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
    if (!gateway_collection_node_expected(collection, id.node_id)) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (!gateway_collection_payload_digest(payload,
                                           payload_len,
                                           payload_digest)) {
        return PROTO_ERR_ARG;
    }

    *duplicate = false;
    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry =
            &collection->results[i];

        if (entry->valid) {
            if (gateway_collection_entry_matches_id(collection, entry, &id)) {
                if (entry->payload_len != payload_len ||
                    !semantic_digest_equal(entry->payload_digest,
                                           payload_digest,
                                           sizeof(payload_digest))) {
                    return PROTO_ERR_MALFORMED;
                }
                *duplicate = true;
                return PROTO_OK;
            }
            if (entry->id.node_id == id.node_id) {
                return PROTO_ERR_MALFORMED;
            }
            continue;
        }
        if (free_entry_index == SIZE_MAX) {
            free_entry_index = i;
        }
    }
    if (!collection->collection_open) {
        return PROTO_ERR_STALE;
    }
    if (free_entry_index == SIZE_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    if (mutable_collection == NULL) {
        return PROTO_OK;
    }

    struct gateway_collection_result_entry *free_entry =
        &mutable_collection->results[free_entry_index];
    free_entry->id.node_id = id.node_id;
    free_entry->id.node_boot_counter = id.node_boot_counter;
    free_entry->id.result_seq = id.result_seq;
    free_entry->previous_hop_id = previous_hop_id;
    memcpy(free_entry->payload_digest,
           payload_digest,
           sizeof(free_entry->payload_digest));
    free_entry->payload_len = (uint16_t)payload_len;
    free_entry->valid = true;
    if (mutable_collection->received_count < UINT16_MAX) {
        mutable_collection->received_count++;
    }
    gateway_collection_refresh_open(mutable_collection);
    mutable_collection->eack_pending = true;
    return PROTO_OK;
}

int gateway_collection_record_result_from_hop(
    struct gateway_collection_state *collection,
    const struct proto_packet *result,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    bool *duplicate)
{
    return gateway_collection_result_from_hop(collection,
                                              collection,
                                              result,
                                              payload,
                                              payload_len,
                                              previous_hop_id,
                                              duplicate);
}

int gateway_collection_preflight_result_from_hop(
    const struct gateway_collection_state *collection,
    const struct proto_packet *result,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    bool *duplicate)
{
    return gateway_collection_result_from_hop(collection,
                                              NULL,
                                              result,
                                              payload,
                                              payload_len,
                                              previous_hop_id,
                                              duplicate);
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

static int gateway_collection_bundle_from_hop(
    const struct gateway_collection_state *collection,
    struct gateway_collection_state *mutable_collection,
    const struct proto_packet *bundle_packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint16_t *accepted_count,
    uint16_t *duplicate_count,
    struct gateway_collection_bundle_projection *projection)
{
    struct result_bundle_header bundle;
    struct gateway_collection_staged_result staged[COLLECTION_BUNDLE_MAX_RECORDS];
    size_t cursor = 0u;
    size_t next_free_slot = 0u;
    uint16_t committed_received_count;
    uint16_t local_duplicate_count = 0u;
    uint8_t accepted_record_mask = 0u;
    uint8_t parsed_count = 0u;
    uint8_t staged_count = 0u;
    bool committed_collection_open;
    int ret;

    if (collection == NULL || bundle_packet == NULL ||
        (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (projection != NULL) {
        memset(projection, 0, sizeof(*projection));
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

    committed_received_count = collection->received_count;
    committed_collection_open = collection->collection_open;

    while (cursor < payload_len) {
        struct result_bundle_record record;
        const struct gateway_collection_result_entry *existing;
        const struct gateway_collection_staged_result *staged_duplicate;
        uint8_t record_digest[SEMANTIC_DIGEST_SHA256_LEN];
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

        ret = gateway_collection_bundle_record_validate(collection, &bundle, &record);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (!gateway_collection_payload_digest(record.payload,
                                               record.payload_len,
                                               record_digest)) {
            return PROTO_ERR_ARG;
        }

        existing = gateway_collection_find_result(collection, &record.result_id);
        if (existing != NULL) {
            if (existing->payload_len != record.payload_len ||
                !semantic_digest_equal(existing->payload_digest,
                                       record_digest,
                                       sizeof(record_digest))) {
                return PROTO_ERR_MALFORMED;
            }
            local_duplicate_count++;
            continue;
        }
        staged_duplicate = gateway_collection_staged_find(staged,
                                                          staged_count,
                                                          &record.result_id);
        if (staged_duplicate != NULL) {
            if (staged_duplicate->payload_len != record.payload_len ||
                memcmp(staged_duplicate->payload,
                       record.payload,
                       record.payload_len) != 0) {
                return PROTO_ERR_MALFORMED;
            }
            local_duplicate_count++;
            continue;
        }
        if (gateway_collection_has_node_result(collection,
                                               record.result_id.node_id) ||
            gateway_collection_staged_has_node(staged,
                                               staged_count,
                                               record.result_id.node_id)) {
            return PROTO_ERR_MALFORMED;
        }
        if (!committed_collection_open) {
            return PROTO_ERR_STALE;
        }
        if (staged_count >= COLLECTION_BUNDLE_MAX_RECORDS ||
            !gateway_collection_next_free_result_slot(collection,
                                                      &next_free_slot,
                                                      &staged[staged_count].result_slot)) {
            return PROTO_ERR_NO_SPACE;
        }

        staged[staged_count].id.node_id = record.result_id.node_id;
        staged[staged_count].id.node_boot_counter = record.result_id.node_boot_counter;
        staged[staged_count].id.result_seq = record.result_id.result_seq;
        staged[staged_count].payload = record.payload;
        staged[staged_count].payload_len = record.payload_len;
        accepted_record_mask |=
            (uint8_t)(1u << (uint8_t)(parsed_count - 1u));
        staged_count++;

        if (committed_received_count < UINT16_MAX) {
            committed_received_count++;
        }
        if (collection->expected_count != 0u &&
            committed_received_count >= collection->expected_count) {
            committed_collection_open = false;
        }
    }

    if (parsed_count != bundle.record_count) {
        return PROTO_ERR_MALFORMED;
    }

    if (mutable_collection != NULL) {
        for (uint8_t i = 0u; i < staged_count; i++) {
            struct gateway_collection_result_entry *entry =
                &mutable_collection->results[staged[i].result_slot];

            entry->id.node_id = staged[i].id.node_id;
            entry->id.node_boot_counter = staged[i].id.node_boot_counter;
            entry->id.result_seq = staged[i].id.result_seq;
            entry->previous_hop_id = previous_hop_id;
            if (!gateway_collection_payload_digest(
                    staged[i].payload,
                    staged[i].payload_len,
                    entry->payload_digest)) {
                return PROTO_ERR_ARG;
            }
            entry->payload_len = staged[i].payload_len;
            entry->valid = true;
        }
        mutable_collection->received_count = committed_received_count;
        mutable_collection->collection_open = committed_collection_open;
        if (staged_count != 0u) {
            mutable_collection->eack_pending = true;
        }
    }
    if (accepted_count != NULL) {
        *accepted_count = staged_count;
    }
    if (duplicate_count != NULL) {
        *duplicate_count = local_duplicate_count;
    }
    if (projection != NULL) {
        projection->accepted_record_mask = accepted_record_mask;
        projection->accepted_count = staged_count;
        projection->duplicate_count = (uint8_t)local_duplicate_count;
    }
    return PROTO_OK;
}

int gateway_collection_record_bundle_from_hop(
    struct gateway_collection_state *collection,
    const struct proto_packet *bundle_packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint16_t *accepted_count,
    uint16_t *duplicate_count)
{
    return gateway_collection_bundle_from_hop(collection,
                                              collection,
                                              bundle_packet,
                                              payload,
                                              payload_len,
                                              previous_hop_id,
                                              accepted_count,
                                              duplicate_count,
                                              NULL);
}

int gateway_collection_preflight_bundle_from_hop(
    const struct gateway_collection_state *collection,
    const struct proto_packet *bundle_packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint16_t *accepted_count,
    uint16_t *duplicate_count)
{
    return gateway_collection_bundle_from_hop(collection,
                                              NULL,
                                              bundle_packet,
                                              payload,
                                              payload_len,
                                              previous_hop_id,
                                              accepted_count,
                                              duplicate_count,
                                              NULL);
}

int gateway_collection_preflight_bundle_projection_from_hop(
    const struct gateway_collection_state *collection,
    const struct proto_packet *bundle_packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    struct gateway_collection_bundle_projection *projection)
{
    if (projection == NULL) {
        return PROTO_ERR_ARG;
    }
    return gateway_collection_bundle_from_hop(collection,
                                              NULL,
                                              bundle_packet,
                                              payload,
                                              payload_len,
                                              previous_hop_id,
                                              NULL,
                                              NULL,
                                              projection);
}

int gateway_collection_project_bundle_payload(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t accepted_record_mask,
    uint8_t *projected_payload,
    size_t projected_payload_cap,
    size_t *projected_payload_len)
{
    struct result_bundle_header bundle;
    size_t input_cursor = 0u;
    size_t output_cursor = 0u;
    size_t output_records_offset;
    uint8_t valid_mask;
    uint8_t input_record_count;
    uint8_t selected_count = 0u;
    int ret;

    if (payload == NULL || projected_payload == NULL ||
        projected_payload_len == NULL || payload_len == 0u) {
        return PROTO_ERR_ARG;
    }
    *projected_payload_len = 0u;
    ret = result_bundle_header_from_tlvs(payload, payload_len, &bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (bundle.record_count == 0u ||
        bundle.record_count > COLLECTION_BUNDLE_MAX_RECORDS) {
        return PROTO_ERR_MALFORMED;
    }
    input_record_count = bundle.record_count;
    valid_mask = input_record_count == 8u ?
                 UINT8_MAX :
                 (uint8_t)((1u << input_record_count) - 1u);
    if (accepted_record_mask == 0u ||
        (accepted_record_mask & (uint8_t)~valid_mask) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = result_bundle_first_record_offset(payload,
                                            payload_len,
                                            &input_cursor);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (proto_crc16_ccitt_false(&payload[input_cursor],
                                payload_len - input_cursor) !=
        bundle.bundle_crc) {
        return PROTO_ERR_BAD_CRC;
    }

    for (uint8_t i = 0u; i < input_record_count; i++) {
        if ((accepted_record_mask & (uint8_t)(1u << i)) != 0u) {
            selected_count++;
        }
    }
    bundle.record_count = selected_count;
    bundle.bundle_crc = 0u;
    ret = result_bundle_header_append_tlvs(projected_payload,
                                           projected_payload_cap,
                                           &output_cursor,
                                           &bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    output_records_offset = output_cursor;

    for (uint8_t i = 0u; i < input_record_count; i++) {
        struct result_bundle_record record;
        size_t before = input_cursor;
        size_t encoded_len;

        if (input_cursor >= payload_len ||
            payload[input_cursor] != TLV_RESULT_RECORD) {
            return PROTO_ERR_MALFORMED;
        }
        ret = result_bundle_record_next_from_tlvs(payload,
                                                  payload_len,
                                                  &input_cursor,
                                                  &record);
        if (ret != PROTO_OK || input_cursor <= before) {
            return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
        }
        if ((accepted_record_mask & (uint8_t)(1u << i)) == 0u) {
            continue;
        }
        encoded_len = input_cursor - before;
        if (output_cursor > projected_payload_cap ||
            projected_payload_cap - output_cursor < encoded_len) {
            return PROTO_ERR_NO_SPACE;
        }
        memmove(&projected_payload[output_cursor],
                &payload[before],
                encoded_len);
        output_cursor += encoded_len;
    }
    if (input_cursor != payload_len) {
        return PROTO_ERR_MALFORMED;
    }

    bundle.bundle_crc = proto_crc16_ccitt_false(
        &projected_payload[output_records_offset],
        output_cursor - output_records_offset);
    {
        size_t rewritten_header_len = 0u;

        ret = result_bundle_header_append_tlvs(projected_payload,
                                               projected_payload_cap,
                                               &rewritten_header_len,
                                               &bundle);
        if (ret != PROTO_OK || rewritten_header_len != output_records_offset) {
            return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
        }
    }
    *projected_payload_len = output_cursor;
    return PROTO_OK;
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

static int gateway_collection_validate(const struct gateway_collection_state *collection,
                                       uint16_t *result_count)
{
    uint16_t valid_count = 0u;

    if (collection == NULL || result_count == NULL) {
        return PROTO_ERR_ARG;
    }
    if (collection->gateway_id == 0u ||
        collection->command_seq == 0u ||
        collection->collection_epoch_id == 0u ||
        collection->membership_epoch == 0u ||
        collection->expected_count == 0u ||
        collection->expected_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        collection->eack_sequence == 0u ||
        collection->received_count > collection->expected_count ||
        collection->received_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        (collection->collection_open && !collection->eack_pending) ||
        (collection->received_count >= collection->expected_count &&
         collection->collection_open)) {
        return PROTO_ERR_MALFORMED;
    }
    if (gateway_collection_roster_validate(collection->gateway_id,
                                           collection->expected_count,
                                           collection->expected_node_ids,
                                           collection->expected_node_id_count) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t i = 0u; i < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; i++) {
        const struct gateway_collection_result_entry *entry = &collection->results[i];

        if (!entry->valid) {
            continue;
        }
        if (entry->id.node_id == 0u || entry->payload_len == 0u ||
            !gateway_collection_node_expected(collection, entry->id.node_id)) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            const struct gateway_collection_result_entry *prior = &collection->results[j];

            if (prior->valid &&
                (gateway_collection_result_id_equal(&prior->id, &entry->id) ||
                 prior->id.node_id == entry->id.node_id)) {
                return PROTO_ERR_MALFORMED;
            }
        }
        if (valid_count >= collection->received_count) {
            return PROTO_ERR_MALFORMED;
        }
        valid_count++;
    }
    if (valid_count != collection->received_count) {
        return PROTO_ERR_MALFORMED;
    }

    *result_count = valid_count;
    return PROTO_OK;
}

int gateway_collection_state_validate(
    const struct gateway_collection_state *collection)
{
    uint16_t result_count = 0u;

    return gateway_collection_validate(collection, &result_count);
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
        collection->expected_count == 0u ||
        collection->expected_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        collection->received_count > collection->expected_count ||
        (collection->collection_open &&
         collection->received_count >= collection->expected_count) ||
        (collection->collection_open &&
         eack_format == EACK_FORMAT_ROSTER_BITMAP) ||
        collection->eack_sequence == 0u) {
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
    eack->packet_sequence = collection->eack_sequence;
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
    out->packet.seq = collection->eack_sequence;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return gateway_collection_eack_packet_validate(&out->packet,
                                                   out->payload,
                                                   out->payload_len,
                                                   NULL);
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
    out->packet.seq = collection->eack_sequence;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    if (missing_count != NULL) {
        *missing_count = missing;
    }
    return gateway_collection_eack_packet_validate(&out->packet,
                                                   out->payload,
                                                   out->payload_len,
                                                   NULL);
}

int gateway_collection_eack_custody_capture(
    struct gateway_collection_eack_custody_snapshot *snapshot,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    int ret;

    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (packet == NULL || payload == NULL ||
        payload_len > GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN ||
        packet->dst_id != MESH_BROADCAST_ID) {
        return PROTO_ERR_MALFORMED;
    }
    ret = gateway_collection_eack_packet_validate(packet,
                                                  payload,
                                                  payload_len,
                                                  NULL);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->packet = *packet;
    memcpy(snapshot->payload, payload, payload_len);
    snapshot->version = GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_VERSION;
    snapshot->payload_len = (uint16_t)payload_len;
    snapshot->payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    snapshot->valid = true;
    return PROTO_OK;
}

int gateway_collection_eack_custody_validate(
    const struct gateway_collection_eack_custody_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (snapshot->version != GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (!snapshot->valid ||
        snapshot->payload_len == 0u ||
        snapshot->payload_len > GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN ||
        snapshot->packet.dst_id != MESH_BROADCAST_ID ||
        snapshot->payload_crc != proto_crc16_ccitt_false(snapshot->payload,
                                                         snapshot->payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    return gateway_collection_eack_packet_validate(&snapshot->packet,
                                                   snapshot->payload,
                                                   snapshot->payload_len,
                                                   NULL);
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
        collection->expected_count == 0u ||
        collection->eack_sequence == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    collection->eack_sequence = collection->eack_sequence == UINT16_MAX ?
                                1u : (uint16_t)(collection->eack_sequence + 1u);
    if (collection->retry_round < UINT8_MAX) {
        collection->retry_round++;
    }
    collection->next_retry_spread_ms =
        gateway_command_collection_retry_spread_ms(collection->retry_round);
    return PROTO_OK;
}
