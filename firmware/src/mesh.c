#include "mesh.h"

#include "route.h"
#include "semantic_digest.h"
#include "survey.h"

#include <string.h>

#define MESH_EVENT_COMPACT_DEFAULT_MAX_MISSED 3u
#define MESH_EVENT_COMPACT_SUPERVISION_INTERVALS 12u

_Static_assert(MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN <= UINT8_MAX,
               "ACK semantic identity must fit one TLV");
_Static_assert(MESH_ACK_SINGLE_PAYLOAD_LEN <= UINT8_MAX,
               "single gateway ACK payload must fit its compact header");
_Static_assert(MESH_ACK_BATCH_MAX_PAYLOAD_LEN <= UWB_MESH_MAX_PAYLOAD_LEN,
               "maximum ACK batch must fit one mesh payload");
_Static_assert(MESH_GATEWAY_ACK_CONFIRM_IDENTITY_VALUE_LEN <= UINT8_MAX &&
               MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN <= UINT8_MAX,
               "gateway ACK confirmation must fit one compact TLV");

bool mesh_packet_rf_channel_allowed(uint8_t msg_type,
                                    uint8_t radio_channel,
                                    bool synthetic_mesh_data_enabled)
{
    const bool channel5 = radio_channel == UWB_CHANNEL_WAKE_CONTACT;
    const bool channel9 = radio_channel == UWB_CHANNEL_MESH_PAYLOAD;

    if (!channel5 && !channel9) {
        return false;
    }

    switch (msg_type) {
    case MSG_ROUTE_REQ:
    case MSG_ROUTE_REPLY:
    case MSG_ROUTE_REPLY_ACK:
    case MSG_GATEWAY_ROUTE_ADV:
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
    case MSG_RELAY_BUSY:
    case MSG_RESULT_BUSY:
    case MSG_RESULT_OFFER:
    case MSG_RESULT_GRANT:
    case MSG_COMMAND:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_DISCOVERY_START:
        return channel5;
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_HOP_ACK:
    case MSG_GATEWAY_ACK:
    case MSG_GATEWAY_ACK_CONFIRM:
    case MSG_GATEWAY_ROUTE_REQ:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return channel9;
    case MSG_GATEWAY_COLLECTION_EACK:
        return true;
    case MSG_MESH_DATA:
        return synthetic_mesh_data_enabled && channel9;
    case MSG_SURVEY_REACH_REQ:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_GATEWAY_COMMAND_EVENT:
    case MSG_GATEWAY_HOST_RECEIPT:
    case MSG_ERROR:
    default:
        return false;
    }
}

struct mesh_exact_tlv_rule {
    uint8_t type;
    uint8_t value_len;
};

static int mesh_exact_tlv_set_validate(
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_exact_tlv_rule *rules,
    size_t rule_count,
    uint32_t required_mask,
    uint32_t *seen)
{
    size_t offset = 0u;
    uint32_t local_seen = 0u;

    if ((payload == NULL && payload_len != 0u) || rules == NULL ||
        rule_count == 0u || rule_count > 32u) {
        return PROTO_ERR_ARG;
    }
    while (offset < payload_len) {
        size_t rule_index;
        uint8_t type;
        uint8_t value_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (value_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        for (rule_index = 0u; rule_index < rule_count; rule_index++) {
            if (rules[rule_index].type == type) {
                break;
            }
        }
        if (rule_index == rule_count ||
            rules[rule_index].value_len != value_len ||
            (local_seen & (UINT32_C(1) << rule_index)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        local_seen |= UINT32_C(1) << rule_index;
        offset += value_len;
    }
    if ((local_seen & required_mask) != required_mask) {
        return PROTO_ERR_MALFORMED;
    }
    if (seen != NULL) {
        *seen = local_seen;
    }
    return PROTO_OK;
}

static bool command_result_id_complete(const struct command_result_id *id)
{
    return id != NULL && id->gateway_id != 0u &&
           id->gateway_epoch != 0u && id->command_seq != 0u &&
           id->node_id != 0u && id->node_boot_counter != 0u &&
           id->result_seq != 0u;
}

static int mesh_event_control_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    static const struct mesh_exact_tlv_rule rules[] = {
        {TLV_MESH_CHANNEL, sizeof(uint8_t)},
        {TLV_MESH_EVENT_INTERVAL_MS, sizeof(uint32_t)},
        {TLV_MESH_EVENT_WINDOW_MS, sizeof(uint16_t)},
        {TLV_MESH_NEXT_EVENT_TIME_MS, sizeof(uint32_t)},
        {TLV_MESH_EVENT_COUNTER, sizeof(uint32_t)},
        {TLV_MESH_EVENT_GUARD_MS, sizeof(uint16_t)},
        {TLV_MESH_CLOCK_SKEW_PPM, sizeof(uint16_t)},
        {TLV_MESH_MAX_MISSED_EVENTS, sizeof(uint8_t)},
        {TLV_MESH_SUPERVISION_TIMEOUT_MS, sizeof(uint32_t)},
        {TLV_MESH_EVENT_BOOT_NONCE, sizeof(uint64_t)},
        {TLV_MESH_EVENT_TX_ON_EVEN, sizeof(uint8_t)},
    };
    const uint32_t timing_mask = (UINT32_C(1) << 9u) - 1u;
    const uint32_t nonce_mask = UINT32_C(1) << 9u;
    const uint32_t parity_mask = UINT32_C(1) << 10u;
    struct mesh_event_timing timing = {0};
    const uint8_t *counter_raw = NULL;
    uint8_t counter_len = 0u;
    uint32_t seen = 0u;
    int ret;

    if (packet->msg_type == MSG_MESH_EVENT_END) {
        return payload_len == 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
    }
    ret = mesh_exact_tlv_set_validate(
        payload,
        payload_len,
        rules,
        sizeof(rules) / sizeof(rules[0]),
        timing_mask |
            (packet->msg_type == MSG_MESH_EVENT_PROPOSE ?
                 nonce_mask : 0u) |
            (packet->msg_type == MSG_MESH_EVENT_UPDATE ?
                 parity_mask : 0u),
        &seen);
    if (ret != PROTO_OK ||
        (packet->msg_type != MSG_MESH_EVENT_PROPOSE &&
         (seen & nonce_mask) != 0u) ||
        (packet->msg_type != MSG_MESH_EVENT_UPDATE &&
         (seen & parity_mask) != 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         0u,
                                         true);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        uint64_t boot_nonce = 0u;
        const uint8_t *nonce_raw = NULL;
        uint8_t nonce_len = 0u;

        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_MESH_EVENT_BOOT_NONCE,
                              &nonce_raw,
                              &nonce_len);
        if (ret != PROTO_OK || nonce_len != sizeof(uint64_t)) {
            return PROTO_ERR_MALFORMED;
        }
        boot_nonce = proto_get_u64_le(nonce_raw);
        if (boot_nonce == 0u) {
            return PROTO_ERR_MALFORMED;
        }
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_MESH_EVENT_COUNTER,
                          &counter_raw,
                          &counter_len);
    if (ret != PROTO_OK || counter_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE &&
        proto_get_u32_le(counter_raw) != packet->session_id) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int mesh_busy_payload_validate(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t local_id,
                                      uint64_t gateway_id)
{
    static const struct mesh_exact_tlv_rule relay_rules[] = {
        {TLV_REQUESTED_MSG_SESSION_ID, sizeof(uint32_t)},
        {TLV_REQUESTED_MSG_SEQ, sizeof(uint16_t)},
        {TLV_RETRY_AFTER_MS, sizeof(uint16_t)},
        {TLV_RELAY_CAPACITY_STATE, sizeof(uint8_t)},
        {TLV_CAPACITY_VALIDITY_INTERVAL_MS, sizeof(uint16_t)},
        {TLV_ALTERNATE_PARENT_ID, sizeof(uint64_t)},
    };
    static const struct mesh_exact_tlv_rule result_rules[] = {
        {TLV_REQUESTED_MSG_SESSION_ID, sizeof(uint32_t)},
        {TLV_REQUESTED_MSG_SEQ, sizeof(uint16_t)},
        {TLV_GATEWAY_ID, sizeof(uint64_t)},
        {TLV_GATEWAY_EPOCH, sizeof(uint16_t)},
        {TLV_COMMAND_SEQ, sizeof(uint32_t)},
        {TLV_NODE_ID, sizeof(uint64_t)},
        {TLV_NODE_BOOT_COUNTER, sizeof(uint32_t)},
        {TLV_RESULT_SEQ, sizeof(uint16_t)},
        {TLV_RETRY_AFTER_MS, sizeof(uint16_t)},
        {TLV_RELAY_CAPACITY_STATE, sizeof(uint8_t)},
        {TLV_CAPACITY_VALIDITY_INTERVAL_MS, sizeof(uint16_t)},
        {TLV_ALTERNATE_PARENT_ID, sizeof(uint64_t)},
    };
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint32_t seen = 0u;
    uint32_t requested_session_id;
    uint16_t retry_after_ms;
    uint16_t validity_ms;
    uint8_t capacity;
    int ret;

    if (packet->msg_type == MSG_RELAY_BUSY) {
        ret = mesh_exact_tlv_set_validate(
            payload,
            payload_len,
            relay_rules,
            sizeof(relay_rules) / sizeof(relay_rules[0]),
            (UINT32_C(1) << 5u) - 1u,
            &seen);
    } else {
        ret = mesh_exact_tlv_set_validate(
            payload,
            payload_len,
            result_rules,
            sizeof(result_rules) / sizeof(result_rules[0]),
            (UINT32_C(1) << 11u) - 1u,
            &seen);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    requested_session_id = proto_get_u32_le(value);
    if (requested_session_id == 0u ||
        requested_session_id != packet->session_id) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_REQUESTED_MSG_SEQ,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t) ||
        proto_get_u16_le(value) == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_RETRY_AFTER_MS,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    retry_after_ms = proto_get_u16_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    validity_ms = proto_get_u16_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_RELAY_CAPACITY_STATE,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    capacity = value[0];
    if (retry_after_ms == 0u || validity_ms == 0u ||
        capacity > RELAY_CAP_BLACK) {
        return PROTO_ERR_MALFORMED;
    }
    if ((seen & (UINT32_C(1) <<
                 (packet->msg_type == MSG_RELAY_BUSY ? 5u : 11u))) != 0u) {
        uint64_t alternate_parent_id;

        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_ALTERNATE_PARENT_ID,
                              &value,
                              &value_len);
        if (ret != PROTO_OK || value_len != sizeof(uint64_t)) {
            return PROTO_ERR_MALFORMED;
        }
        alternate_parent_id = proto_get_u64_le(value);
        if (alternate_parent_id == 0u ||
            alternate_parent_id == local_id ||
            alternate_parent_id == packet->src_id) {
            return PROTO_ERR_MALFORMED;
        }
    }
    if (packet->msg_type == MSG_RESULT_BUSY) {
        struct result_busy busy;

        ret = result_busy_from_tlvs(payload, payload_len, &busy);
        if (ret != PROTO_OK ||
            !command_result_id_complete(&busy.result_id) ||
            busy.result_id.gateway_id != gateway_id ||
            busy.result_id.command_seq != packet->session_id) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

static int mesh_command_result_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id)
{
    static const uint8_t identity_types[] = {
        TLV_GATEWAY_ID,
        TLV_GATEWAY_EPOCH,
        TLV_COMMAND_SEQ,
        TLV_NODE_ID,
        TLV_NODE_BOOT_COUNTER,
        TLV_RESULT_SEQ,
    };
    struct command_result_id identity;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t identity_fields = 0u;
    int ret;

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t) ||
        proto_get_u16_le(value) == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_STATUS,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t) ||
        proto_get_u16_le(value) > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_REASON,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t i = 0u; i < sizeof(identity_types); i++) {
        ret = tlv_find_unique(payload,
                              payload_len,
                              identity_types[i],
                              &value,
                              &value_len);
        if (ret == PROTO_OK) {
            identity_fields++;
        } else if (ret != PROTO_ERR_NOT_FOUND) {
            return PROTO_ERR_MALFORMED;
        }
    }
    if (identity_fields == 0u) {
        return PROTO_OK;
    }
    if (identity_fields != sizeof(identity_types) ||
        command_result_id_from_tlvs(payload,
                                    payload_len,
                                    &identity) != PROTO_OK ||
        identity.gateway_id != gateway_id ||
        identity.gateway_epoch == 0u ||
        identity.command_seq != packet->session_id ||
        identity.node_id != packet->src_id ||
        identity.node_boot_counter == 0u ||
        identity.result_seq != packet->seq) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          &value,
                          &value_len);
    return ret == PROTO_OK && value_len == sizeof(uint32_t) &&
                   proto_get_u32_le(value) != 0u ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

static int mesh_tlv_payload_framing_validate(const uint8_t *payload,
                                             size_t payload_len)
{
    size_t offset = 0u;

    if (payload == NULL || payload_len == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    while (offset < payload_len) {
        uint8_t value_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if ((size_t)value_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        offset += value_len;
    }
    return PROTO_OK;
}

static int mesh_click_report_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint32_t event_seq;
    int ret;

    if (mesh_tlv_payload_framing_validate(payload, payload_len) !=
        PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_CLICKER_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    clicker_id = proto_get_u64_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_ANCHOR_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    anchor_id = proto_get_u64_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_EVENT_SEQ,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    event_seq = proto_get_u32_le(value);
    return clicker_id != 0u && anchor_id == packet->src_id &&
                   proto_click_report_session_id(clicker_id, event_seq) ==
                       packet->session_id ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

static int mesh_result_bundle_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id)
{
    static const struct mesh_exact_tlv_rule header_rules[] = {
        {TLV_GATEWAY_ID, sizeof(uint64_t)},
        {TLV_GATEWAY_EPOCH, sizeof(uint16_t)},
        {TLV_COMMAND_SEQ, sizeof(uint32_t)},
        {TLV_COLLECTION_EPOCH_ID, sizeof(uint32_t)},
        {TLV_BUNDLE_ID, sizeof(uint16_t)},
        {TLV_RECORD_COUNT, sizeof(uint8_t)},
        {TLV_BUNDLE_CRC, sizeof(uint16_t)},
    };
    struct result_bundle_header bundle;
    size_t record_offset = 0u;
    size_t cursor;
    uint32_t seen = 0u;
    uint8_t parsed_count = 0u;

    while (record_offset < payload_len) {
        uint8_t type;
        uint8_t value_len;

        if (payload_len - record_offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[record_offset];
        value_len = payload[record_offset + 1u];
        if ((size_t)value_len >
            payload_len - record_offset - PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_RESULT_RECORD) {
            break;
        }
        size_t rule_index;

        for (rule_index = 0u;
             rule_index < sizeof(header_rules) / sizeof(header_rules[0]);
             rule_index++) {
            if (header_rules[rule_index].type == type) {
                break;
            }
        }
        if (rule_index ==
                sizeof(header_rules) / sizeof(header_rules[0]) ||
            header_rules[rule_index].value_len != value_len ||
            (seen & (UINT32_C(1) << rule_index)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        seen |= UINT32_C(1) << rule_index;
        record_offset += PROTO_TLV_HEADER_LEN + value_len;
    }
    if (seen !=
            ((UINT32_C(1) <<
              (sizeof(header_rules) / sizeof(header_rules[0]))) -
             1u) ||
        record_offset == payload_len ||
        result_bundle_header_from_tlvs(payload,
                                       payload_len,
                                       &bundle) != PROTO_OK ||
        bundle.gateway_id != gateway_id ||
        bundle.gateway_epoch == 0u ||
        bundle.command_seq != packet->session_id ||
        bundle.collection_epoch_id == 0u ||
        bundle.bundle_id != packet->seq ||
        bundle.record_count == 0u ||
        bundle.record_count > 8u ||
        proto_crc16_ccitt_false(&payload[record_offset],
                                payload_len - record_offset) !=
            bundle.bundle_crc) {
        return PROTO_ERR_MALFORMED;
    }

    cursor = record_offset;
    while (cursor < payload_len) {
        struct result_bundle_record record;
        size_t before = cursor;

        if (payload[cursor] != TLV_RESULT_RECORD ||
            result_bundle_record_next_from_tlvs(payload,
                                                payload_len,
                                                &cursor,
                                                &record) != PROTO_OK ||
            cursor <= before ||
            record.result_id.gateway_id != bundle.gateway_id ||
            record.result_id.gateway_epoch != bundle.gateway_epoch ||
            record.result_id.command_seq != bundle.command_seq ||
            record.result_id.node_id == 0u ||
            record.result_id.node_boot_counter == 0u ||
            record.result_id.result_seq == 0u ||
            record.payload_len == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        parsed_count++;
    }
    return cursor == payload_len && parsed_count == bundle.record_count ?
               PROTO_OK : PROTO_ERR_MALFORMED;
}

static int mesh_survey_discovery_report_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *value = NULL;
    uint64_t operation_generation = 0u;
    uint64_t anchor_id = 0u;
    uint32_t survey_id = 0u;
    size_t offset = 0u;
    uint8_t value_len = 0u;
    uint8_t entry_count = 0u;
    int ret;

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_SURVEY_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    survey_id = proto_get_u32_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_ANCHOR_ID,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    anchor_id = proto_get_u64_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_SURVEY_OPERATION_GENERATION,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    operation_generation = proto_get_u64_le(value);
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_COMMAND_STATUS,
                          &value,
                          &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint16_t) ||
        proto_get_u16_le(value) > COMMAND_INTERNAL_ERROR ||
        survey_id == 0u || anchor_id != packet->src_id ||
        operation_generation == 0u ||
        (uint32_t)operation_generation == 0u ||
        packet->session_id != (uint32_t)operation_generation) {
        return PROTO_ERR_MALFORMED;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if ((size_t)len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        switch (type) {
        case TLV_SURVEY_ID:
            if (len != sizeof(uint32_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_ANCHOR_ID:
        case TLV_SURVEY_OPERATION_GENERATION:
            if (len != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_COMMAND_STATUS:
            if (len != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_REACHABILITY_ENTRY: {
            struct survey_reachability_entry entry;

            if (len != SURVEY_REACHABILITY_ENTRY_LEN ||
                entry_count >= SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
                return PROTO_ERR_MALFORMED;
            }
            entry.peer_id = proto_get_u64_le(&payload[offset]);
            entry.rssi_dbm = (int8_t)payload[offset + 8u];
            entry.quality = payload[offset + 9u];
            if (entry.peer_id == 0u || entry.quality > 100u ||
                entry.peer_id == anchor_id) {
                return PROTO_ERR_MALFORMED;
            }
            entry_count++;
            break;
        }
        default:
            return PROTO_ERR_MALFORMED;
        }
        offset += len;
    }
    return PROTO_OK;
}

static int mesh_survey_pair_result_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const uint8_t *value = NULL;
    uint64_t operation_generation;
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t survey_id;
    uint32_t expected_seq;
    uint16_t sample_count;
    uint16_t sample_index;
    uint16_t round_id = 0u;
    uint8_t value_len = 0u;
    uint8_t quality;
    uint8_t range_status;
    int ret;

    if (mesh_tlv_payload_framing_validate(payload, payload_len) !=
        PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
#define REQUIRE_PAIR_TLV(_type, _size)                                      \
    do {                                                                     \
        ret = tlv_find_unique(payload, payload_len, (_type),                 \
                              &value, &value_len);                            \
        if (ret != PROTO_OK || value_len != (_size)) {                       \
            return PROTO_ERR_MALFORMED;                                      \
        }                                                                    \
    } while (0)
    REQUIRE_PAIR_TLV(TLV_SURVEY_ID, sizeof(uint32_t));
    survey_id = proto_get_u32_le(value);
    REQUIRE_PAIR_TLV(TLV_SURVEY_OPERATION_GENERATION, sizeof(uint64_t));
    operation_generation = proto_get_u64_le(value);
    REQUIRE_PAIR_TLV(TLV_INITIATOR_ID, sizeof(uint64_t));
    initiator_id = proto_get_u64_le(value);
    REQUIRE_PAIR_TLV(TLV_RESPONDER_ID, sizeof(uint64_t));
    responder_id = proto_get_u64_le(value);
    REQUIRE_PAIR_TLV(TLV_SAMPLE_COUNT, sizeof(uint16_t));
    sample_count = proto_get_u16_le(value);
    REQUIRE_PAIR_TLV(TLV_SAMPLE_INDEX, sizeof(uint16_t));
    sample_index = proto_get_u16_le(value);
    REQUIRE_PAIR_TLV(TLV_DISTANCE_MM, sizeof(uint32_t));
    REQUIRE_PAIR_TLV(TLV_QUALITY, sizeof(uint8_t));
    quality = value[0];
    REQUIRE_PAIR_TLV(TLV_RANGE_STATUS, sizeof(uint8_t));
    range_status = value[0];
#undef REQUIRE_PAIR_TLV

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_SURVEY_ROUND_ID,
                          &value,
                          &value_len);
    if (ret == PROTO_OK) {
        if (value_len != sizeof(uint16_t)) {
            return PROTO_ERR_MALFORMED;
        }
        round_id = proto_get_u16_le(value);
        if (round_id == 0u) {
            return PROTO_ERR_MALFORMED;
        }
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return PROTO_ERR_MALFORMED;
    }
    expected_seq = round_id == 0u ?
        (uint32_t)sample_index + 1u :
        ((uint32_t)(round_id - 1u) *
         SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) +
            (uint32_t)sample_index + 1u;
    if (survey_id == 0u || operation_generation == 0u ||
        (uint32_t)operation_generation == 0u ||
        packet->session_id != (uint32_t)operation_generation ||
        initiator_id == 0u || responder_id == 0u ||
        initiator_id == responder_id ||
        (packet->src_id != initiator_id &&
         packet->src_id != responder_id) ||
        sample_count < SURVEY_MIN_SAMPLE_COUNT ||
        sample_count > SURVEY_MAX_SAMPLE_COUNT ||
        sample_index >= sample_count ||
        quality > 100u || range_status > RANGE_TIMING_INVALID ||
        range_status == RANGE_STS_QUALITY_FAIL ||
        expected_seq == 0u || expected_seq > UINT16_MAX ||
        packet->seq != (uint16_t)expected_seq) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int mesh_gateway_uplink_payload_validate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id)
{
    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
        return mesh_click_report_payload_validate(packet,
                                                  payload,
                                                  payload_len);
    case MSG_SELF_TEST_REPORT:
        return proto_self_test_report_validate(packet, payload, payload_len);
    case MSG_ANCHOR_HEARTBEAT:
        return proto_anchor_heartbeat_validate(packet, payload, payload_len);
    case MSG_COMMAND_RESULT:
        return mesh_command_result_payload_validate(packet,
                                                    payload,
                                                    payload_len,
                                                    gateway_id);
    case MSG_RESULT_BUNDLE:
        return mesh_result_bundle_payload_validate(packet,
                                                   payload,
                                                   payload_len,
                                                   gateway_id);
    case MSG_SURVEY_DISCOVERY_REPORT:
        return mesh_survey_discovery_report_payload_validate(packet,
                                                             payload,
                                                             payload_len);
    case MSG_SURVEY_PAIR_RESULT:
        return mesh_survey_pair_result_payload_validate(packet,
                                                        payload,
                                                        payload_len);
    case MSG_MESH_DATA:
        return payload_len > 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
    default:
        return PROTO_ERR_MALFORMED;
    }
}

int mesh_packet_rx_semantics_validate(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint64_t local_id,
                                      uint64_t gateway_id)
{
    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len || previous_hop_id == 0u ||
        local_id == 0u || packet->src_id == 0u ||
        packet->session_id == 0u || packet->seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        if (gateway_id == 0u || packet->dst_id != gateway_id ||
            packet->src_id == gateway_id ||
            previous_hop_id == local_id ||
            packet->ttl == 0u || packet->ttl > MESH_DEFAULT_TTL ||
            ((previous_hop_id == packet->src_id) !=
             (packet->ttl == MESH_DEFAULT_TTL)) ||
            !mesh_gateway_ack_confirmed_flags_valid(packet->msg_type,
                                                    packet->flags)) {
            return PROTO_ERR_MALFORMED;
        }
        return mesh_gateway_uplink_payload_validate(packet,
                                                    payload,
                                                    payload_len,
                                                    gateway_id);
    case MSG_GATEWAY_ACK_CONFIRM: {
        struct mesh_gateway_ack_confirm_identity identity;

        if (gateway_id == 0u ||
            packet->dst_id != gateway_id ||
            packet->src_id == gateway_id ||
            previous_hop_id == local_id ||
            packet->ttl == 0u ||
            packet->ttl > MESH_GATEWAY_ACK_TTL ||
            ((previous_hop_id == packet->src_id) !=
             (packet->ttl == MESH_GATEWAY_ACK_TTL))) {
            return PROTO_ERR_MALFORMED;
        }
        return mesh_gateway_ack_confirm_payload_parse(
            packet,
            payload,
            payload_len,
            &identity);
    }
    case MSG_GATEWAY_COLLECTION_EACK: {
        struct gateway_collection_eack eack;

        if (gateway_id == 0u || packet->src_id != gateway_id ||
            packet->dst_id != 0u ||
            previous_hop_id == local_id ||
            packet->ttl == 0u ||
            packet->ttl > MESH_NETWORK_MAX_HOPS ||
            ((previous_hop_id == gateway_id) !=
             (packet->ttl == MESH_NETWORK_MAX_HOPS)) ||
            gateway_collection_eack_packet_validate(packet,
                                                    payload,
                                                    payload_len,
                                                    &eack) != PROTO_OK ||
            eack.gateway_id != gateway_id) {
            return PROTO_ERR_MALFORMED;
        }
        return PROTO_OK;
    }
    case MSG_RESULT_OFFER: {
        struct result_offer offer;
        int ret;

        if (packet->flags != 0u || packet->ttl != 1u ||
            packet->src_id != previous_hop_id ||
            packet->dst_id != local_id) {
            return PROTO_ERR_MALFORMED;
        }
        ret = result_offer_from_tlvs(payload, payload_len, &offer);
        return ret == PROTO_OK &&
                       command_result_id_complete(&offer.result_id) &&
                       offer.result_id.gateway_id == gateway_id &&
                       offer.result_id.node_id == packet->src_id &&
                       offer.result_id.command_seq == packet->session_id &&
                       offer.result_id.result_seq == packet->seq &&
                       offer.result_len > 0u &&
                       offer.result_len <= UWB_MESH_MAX_PAYLOAD_LEN ?
                   PROTO_OK : PROTO_ERR_MALFORMED;
    }
    case MSG_RESULT_GRANT: {
        struct result_grant grant;
        int ret;

        if (packet->flags != 0u || packet->ttl != 1u ||
            packet->src_id != previous_hop_id ||
            packet->dst_id != local_id) {
            return PROTO_ERR_MALFORMED;
        }
        ret = result_grant_from_tlvs(payload, payload_len, &grant);
        return ret == PROTO_OK &&
                       command_result_id_complete(&grant.result_id) &&
                       grant.result_id.gateway_id == gateway_id &&
                       grant.result_id.command_seq == packet->session_id &&
                       grant.granted_channel == UWB_CHANNEL_MESH_PAYLOAD &&
                       grant.max_bytes > 0u &&
                       grant.max_bytes <= UWB_MESH_MAX_PAYLOAD_LEN &&
                       grant.event_offset_hint == 0u ?
                   PROTO_OK : PROTO_ERR_MALFORMED;
    }
    case MSG_RELAY_BUSY:
    case MSG_RESULT_BUSY:
        if (packet->flags != 0u || packet->ttl != 1u ||
            packet->src_id != previous_hop_id ||
            packet->dst_id != local_id) {
            return PROTO_ERR_MALFORMED;
        }
        return mesh_busy_payload_validate(packet,
                                          payload,
                                          payload_len,
                                          local_id,
                                          gateway_id);
    case MSG_GATEWAY_ROUTE_REQ:
        return packet->flags == FLAG_GATEWAY_ACK_REQUIRED &&
                       packet->src_id == previous_hop_id &&
                       packet->dst_id == gateway_id &&
                       local_id == gateway_id &&
                       packet->ttl == MESH_DEFAULT_TTL &&
                       payload_len == 0u ?
                   PROTO_OK : PROTO_ERR_MALFORMED;
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
        if (packet->flags != 0u ||
            packet->src_id != previous_hop_id ||
            packet->dst_id != local_id ||
            packet->ttl != MESH_DEFAULT_TTL) {
            return PROTO_ERR_MALFORMED;
        }
        return mesh_event_control_payload_validate(packet,
                                                   payload,
                                                   payload_len);
    case MSG_SURVEY_REACH_REQ:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_GATEWAY_COMMAND_EVENT:
    case MSG_GATEWAY_HOST_RECEIPT:
    case MSG_ERROR:
        /*
         * These compatibility/control identifiers have no UWB RF lane.
         * Reject them again at semantic ingress so a caller that has already
         * decoded a packet cannot bypass channel admission and mutate relay
         * state.
         */
        return PROTO_ERR_MALFORMED;
    default:
        return PROTO_OK;
    }
}

int mesh_packet_rx_envelope_validate(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t previous_hop_id,
                                     uint64_t local_id,
                                     uint64_t gateway_id,
                                     uint8_t radio_channel,
                                     bool synthetic_mesh_data_enabled)
{
    if (packet == NULL ||
        !mesh_packet_rf_channel_allowed(packet->msg_type,
                                        radio_channel,
                                        synthetic_mesh_data_enabled)) {
        return PROTO_ERR_MALFORMED;
    }
    return mesh_packet_rx_semantics_validate(packet,
                                             payload,
                                             payload_len,
                                             previous_hop_id,
                                             local_id,
                                             gateway_id);
}

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool command_status_valid(enum command_status status)
{
    return status >= COMMAND_OK && status <= COMMAND_INTERNAL_ERROR;
}

static bool event_control_type_valid(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t time_until_deadline(uint32_t now_ms, uint32_t deadline_ms)
{
    if (time_reached(now_ms, deadline_ms)) {
        return 1u;
    }
    return deadline_ms - now_ms;
}

static uint32_t deadline_after(uint32_t now_ms, uint32_t delay_ms)
{
    return now_ms + (delay_ms == 0u ? 1u : delay_ms);
}

static uint32_t compact_supervision_timeout_ms(uint32_t event_interval_ms)
{
    if (event_interval_ms == 0u) {
        return 0u;
    }
    if (event_interval_ms > UINT32_MAX / MESH_EVENT_COMPACT_SUPERVISION_INTERVALS) {
        return UINT32_MAX;
    }
    return event_interval_ms * MESH_EVENT_COMPACT_SUPERVISION_INTERVALS;
}

static void counter_add(uint32_t *counter, uint32_t delta)
{
    if (counter == NULL) {
        return;
    }
    if (UINT32_MAX - *counter < delta) {
        *counter = UINT32_MAX;
    } else {
        *counter += delta;
    }
}

static bool mesh_event_params_valid(const struct mesh_event_params *params)
{
    return params != NULL &&
           params->event_interval_ms > 0u &&
           params->event_window_ms > 0u &&
           params->guard_ms > 0u &&
           params->max_missed_events > 0u &&
           params->supervision_timeout_ms >= params->event_interval_ms;
}

static int find_u8_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find_unique(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static int find_u16_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find_unique(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int find_u32_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find_unique(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

int mesh_ch9_batch_metadata_parse(const uint8_t *payload,
                                  size_t payload_len,
                                  struct mesh_ch9_batch_metadata *metadata)
{
    size_t cursor = 0u;
    bool batch_id_seen = false;
    bool flags_seen = false;

    if (metadata == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    *metadata = (struct mesh_ch9_batch_metadata) {0};

    while (cursor < payload_len) {
        uint8_t type;
        uint8_t value_len;

        if (payload_len - cursor < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[cursor];
        value_len = payload[cursor + 1u];
        cursor += PROTO_TLV_HEADER_LEN;
        if (payload_len - cursor < value_len) {
            return PROTO_ERR_MALFORMED;
        }

        if (type == TLV_MESH_CH9_BATCH_ID) {
            if (batch_id_seen || value_len != sizeof(uint32_t)) {
                return PROTO_ERR_MALFORMED;
            }
            metadata->batch_id = proto_get_u32_le(&payload[cursor]);
            if (metadata->batch_id == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            batch_id_seen = true;
        } else if (type == TLV_MESH_CH9_BATCH_FLAGS) {
            if (flags_seen || value_len != sizeof(uint8_t)) {
                return PROTO_ERR_MALFORMED;
            }
            metadata->flags = payload[cursor];
            if ((metadata->flags &
                 (uint8_t)~MESH_CH9_BATCH_FLAG_FINAL) != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            flags_seen = true;
        }
        cursor += value_len;
    }

    if (batch_id_seen != flags_seen) {
        return PROTO_ERR_MALFORMED;
    }
    if (batch_id_seen) {
        metadata->present = true;
        metadata->final_packet =
            (metadata->flags & MESH_CH9_BATCH_FLAG_FINAL) != 0u;
    }
    return PROTO_OK;
}

bool mesh_packet_semantic_digest(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    static const uint8_t domain[] = "mesh-relay-packet-v1";
    struct semantic_digest_sha256_context context;
    uint8_t header[1u + 1u + sizeof(uint64_t) + sizeof(uint64_t) +
                   sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t)];
    size_t offset = 0u;

    if (packet == NULL || digest == NULL ||
        packet->payload_len != payload_len ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        (payload_len != 0u && payload == NULL)) {
        return false;
    }

    header[offset++] = packet->msg_type;
    header[offset++] = packet->flags;
    proto_put_u64_le(&header[offset], packet->src_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&header[offset], packet->dst_id);
    offset += sizeof(uint64_t);
    proto_put_u32_le(&header[offset], packet->session_id);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&header[offset], packet->seq);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&header[offset], (uint16_t)payload_len);
    offset += sizeof(uint16_t);

    return offset == sizeof(header) &&
           semantic_digest_sha256_init(&context) &&
           semantic_digest_sha256_update(&context,
                                         domain,
                                         sizeof(domain) - 1u) &&
           semantic_digest_sha256_update(&context, header, sizeof(header)) &&
           semantic_digest_sha256_update(&context, payload, payload_len) &&
           semantic_digest_sha256_final(&context, digest);
}

int mesh_append_ack_semantic_identity(
    uint8_t *ack_payload,
    size_t ack_payload_cap,
    size_t *offset,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len)
{
    uint8_t value[MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN];

    if (ack_payload == NULL || offset == NULL ||
        acknowledged_packet == NULL ||
        acknowledged_packet->session_id == 0u ||
        acknowledged_packet->seq == 0u) {
        return PROTO_ERR_ARG;
    }
    proto_put_u32_le(value, acknowledged_packet->session_id);
    proto_put_u16_le(&value[sizeof(uint32_t)], acknowledged_packet->seq);
    if (!mesh_packet_semantic_digest(
            acknowledged_packet,
            acknowledged_payload,
            acknowledged_payload_len,
            &value[sizeof(uint32_t) + sizeof(uint16_t)])) {
        return PROTO_ERR_MALFORMED;
    }

    return tlv_append_bytes(ack_payload,
                            ack_payload_cap,
                            offset,
                            TLV_MESH_ACK_SEMANTIC_IDENTITY,
                            value,
                            (uint8_t)sizeof(value));
}

int mesh_ack_semantic_identity_at(
    const uint8_t *ack_payload,
    size_t ack_payload_len,
    uint8_t index,
    struct mesh_ack_semantic_identity *identity)
{
    struct mesh_ack_semantic_identity found_identity = {0};
    size_t offset = 0u;
    uint8_t identity_index = 0u;
    bool found = false;

    if (ack_payload == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < ack_payload_len) {
        uint8_t type;
        uint8_t value_len;
        const uint8_t *value;

        if (ack_payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = ack_payload[offset];
        value_len = ack_payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (ack_payload_len - offset < value_len) {
            return PROTO_ERR_MALFORMED;
        }
        value = &ack_payload[offset];
        if (type == TLV_MESH_ACK_SEMANTIC_IDENTITY) {
            struct mesh_ack_semantic_identity current;

            if (value_len != MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN ||
                identity_index >= MESH_ACK_SEMANTIC_IDENTITY_MAX) {
                return PROTO_ERR_MALFORMED;
            }
            current.session_id = proto_get_u32_le(value);
            current.seq = proto_get_u16_le(&value[sizeof(uint32_t)]);
            memcpy(current.digest,
                   &value[sizeof(uint32_t) + sizeof(uint16_t)],
                   sizeof(current.digest));
            if (current.session_id == 0u || current.seq == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            if (identity_index == index) {
                found_identity = current;
                found = true;
            }
            identity_index++;
        }
        offset += value_len;
    }

    if (!found) {
        return PROTO_ERR_NOT_FOUND;
    }
    *identity = found_identity;
    return PROTO_OK;
}

static int ack_payload_diagnostic_lists(
    const struct proto_packet *ack_packet,
    const uint8_t *payload,
    size_t payload_len,
    const uint8_t **seq_list,
    const uint8_t **session_list,
    const uint8_t **packet_id_list,
    uint8_t *entry_count,
    bool *batched)
{
    const uint8_t *requested_value = NULL;
    const uint8_t *local_seq_list = NULL;
    const uint8_t *local_session_list = NULL;
    const uint8_t *local_packet_id_list = NULL;
    uint8_t requested_len = 0u;
    uint8_t seq_list_len = 0u;
    uint8_t session_list_len = 0u;
    uint8_t packet_id_list_len = 0u;
    int requested_ret;
    int seq_ret;
    int session_ret;
    int packet_id_ret;

    if (ack_packet == NULL || payload == NULL || seq_list == NULL ||
        session_list == NULL || packet_id_list == NULL ||
        entry_count == NULL || batched == NULL) {
        return PROTO_ERR_ARG;
    }
    if ((ack_packet->msg_type != MSG_GATEWAY_ACK &&
         ack_packet->msg_type != MSG_MESH_HOP_ACK) ||
        ack_packet->payload_len != payload_len ||
        ack_packet->session_id == 0u ||
        ack_packet->seq == 0u ||
        ack_packet->ttl == 0u ||
        ack_packet->ttl > MESH_GATEWAY_ACK_TTL ||
        (ack_packet->msg_type == MSG_GATEWAY_ACK &&
         ack_packet->flags != FLAG_GATEWAY_ACK) ||
        (ack_packet->msg_type == MSG_MESH_HOP_ACK &&
         ack_packet->flags != 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    *seq_list = NULL;
    *session_list = NULL;
    *packet_id_list = NULL;
    *entry_count = 0u;
    *batched = false;

    requested_ret = tlv_find_unique(payload,
                                    payload_len,
                                    TLV_REQUESTED_MSG_SEQ,
                                    &requested_value,
                                    &requested_len);
    seq_ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_MESH_ACK_SEQ_LIST,
                              &local_seq_list,
                              &seq_list_len);
    session_ret = tlv_find_unique(payload,
                                  payload_len,
                                  TLV_MESH_ACK_SESSION_LIST,
                                  &local_session_list,
                                  &session_list_len);
    packet_id_ret = tlv_find_unique(payload,
                                    payload_len,
                                    TLV_MESH_ACK_PACKET_ID_LIST,
                                    &local_packet_id_list,
                                    &packet_id_list_len);
    if ((requested_ret != PROTO_OK &&
         requested_ret != PROTO_ERR_NOT_FOUND) ||
        (seq_ret != PROTO_OK && seq_ret != PROTO_ERR_NOT_FOUND) ||
        (session_ret != PROTO_OK &&
         session_ret != PROTO_ERR_NOT_FOUND) ||
        (packet_id_ret != PROTO_OK &&
         packet_id_ret != PROTO_ERR_NOT_FOUND)) {
        return PROTO_ERR_MALFORMED;
    }

    if (requested_ret != PROTO_OK ||
        requested_len != sizeof(uint16_t) ||
        proto_get_u16_le(requested_value) == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    if (seq_ret == PROTO_OK) {
        if (seq_list_len == 0u ||
            (seq_list_len % sizeof(uint16_t)) != 0u ||
            session_ret != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
        *entry_count = seq_list_len / sizeof(uint16_t);
        if (*entry_count == 0u ||
            *entry_count > MESH_ACK_SEMANTIC_IDENTITY_MAX ||
            session_list_len != *entry_count * sizeof(uint32_t) ||
            (packet_id_ret == PROTO_OK &&
             packet_id_list_len != *entry_count * sizeof(uint32_t))) {
            return PROTO_ERR_MALFORMED;
        }
        if (proto_get_u16_le(requested_value) !=
                proto_get_u16_le(local_seq_list) ||
            ack_packet->session_id !=
                proto_get_u32_le(local_session_list)) {
            return PROTO_ERR_MALFORMED;
        }
        for (uint8_t i = 0u; i < *entry_count; i++) {
            uint32_t session_id = proto_get_u32_le(
                &local_session_list[i * sizeof(uint32_t)]);
            uint16_t seq = proto_get_u16_le(
                &local_seq_list[i * sizeof(uint16_t)]);

            if (session_id == 0u || seq == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            for (uint8_t prior = 0u; prior < i; prior++) {
                if (session_id == proto_get_u32_le(
                                      &local_session_list[
                                          prior * sizeof(uint32_t)]) &&
                    seq == proto_get_u16_le(
                               &local_seq_list[
                                   prior * sizeof(uint16_t)])) {
                    return PROTO_ERR_MALFORMED;
                }
            }
        }
        *seq_list = local_seq_list;
        *session_list = local_session_list;
        *packet_id_list = packet_id_ret == PROTO_OK ?
                          local_packet_id_list : NULL;
        *batched = true;
        return PROTO_OK;
    }

    if (session_ret == PROTO_OK || packet_id_ret == PROTO_OK ||
        ack_packet->session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    *entry_count = 1u;
    return PROTO_OK;
}

int mesh_ack_payload_contains_packet(
    const struct proto_packet *ack_packet,
    const uint8_t *ack_payload,
    size_t ack_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    bool *contains)
{
    const uint8_t *seq_list = NULL;
    const uint8_t *session_list = NULL;
    const uint8_t *packet_id_list = NULL;
    uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t entry_count = 0u;
    bool batched = false;
    int ret;

    if (ack_packet == NULL || ack_payload == NULL ||
        acknowledged_packet == NULL || contains == NULL ||
        acknowledged_packet->session_id == 0u ||
        acknowledged_packet->seq == 0u) {
        return PROTO_ERR_ARG;
    }
    *contains = false;
    if (!mesh_packet_semantic_digest(acknowledged_packet,
                                     acknowledged_payload,
                                     acknowledged_payload_len,
                                     expected_digest)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = ack_payload_diagnostic_lists(ack_packet,
                                       ack_payload,
                                       ack_payload_len,
                                       &seq_list,
                                       &session_list,
                                       &packet_id_list,
                                       &entry_count,
                                       &batched);
    if (ret != PROTO_OK) {
        return ret;
    }
    (void)packet_id_list;

    for (uint8_t i = 0u; i < entry_count; i++) {
        struct mesh_ack_semantic_identity identity;

        ret = mesh_ack_semantic_identity_at(ack_payload,
                                            ack_payload_len,
                                            i,
                                            &identity);
        if (ret != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
        if (batched &&
            (identity.session_id !=
                 proto_get_u32_le(&session_list[i * sizeof(uint32_t)]) ||
             identity.seq !=
                 proto_get_u16_le(&seq_list[i * sizeof(uint16_t)]))) {
            return PROTO_ERR_MALFORMED;
        }
        if (!batched) {
            const uint8_t *requested_value = NULL;
            uint8_t requested_len = 0u;

            if (tlv_find_unique(ack_payload,
                                ack_payload_len,
                                TLV_REQUESTED_MSG_SEQ,
                                &requested_value,
                                &requested_len) != PROTO_OK ||
                requested_len != sizeof(uint16_t) ||
                identity.session_id != ack_packet->session_id ||
                identity.seq != proto_get_u16_le(requested_value)) {
                return PROTO_ERR_MALFORMED;
            }
        }
        if (identity.session_id == acknowledged_packet->session_id &&
            identity.seq == acknowledged_packet->seq &&
            semantic_digest_equal(identity.digest,
                                  expected_digest,
                                  sizeof(expected_digest))) {
            *contains = true;
        }
    }

    {
        struct mesh_ack_semantic_identity extra;

        if (mesh_ack_semantic_identity_at(ack_payload,
                                          ack_payload_len,
                                          entry_count,
                                          &extra) != PROTO_ERR_NOT_FOUND) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

int mesh_ack_payload_contains(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t requested_session_id,
                              uint16_t requested_seq,
                              bool *contains)
{
    const uint8_t *requested_value = NULL;
    const uint8_t *seq_list = NULL;
    const uint8_t *session_list = NULL;
    const uint8_t *packet_id_list = NULL;
    uint8_t requested_len = 0u;
    uint8_t seq_list_len = 0u;
    uint8_t session_list_len = 0u;
    uint8_t packet_id_list_len = 0u;
    int requested_ret;
    int seq_ret;
    int session_ret;
    int packet_id_ret;

    if (ack_packet == NULL || payload == NULL || contains == NULL ||
        requested_session_id == 0u || requested_seq == 0u) {
        return PROTO_ERR_ARG;
    }
    if (ack_packet->msg_type != MSG_GATEWAY_ACK &&
        ack_packet->msg_type != MSG_MESH_HOP_ACK) {
        return PROTO_ERR_MALFORMED;
    }
    *contains = false;

    requested_ret = tlv_find_unique(payload,
                                    payload_len,
                                    TLV_REQUESTED_MSG_SEQ,
                                    &requested_value,
                                    &requested_len);
    seq_ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_MESH_ACK_SEQ_LIST,
                              &seq_list,
                              &seq_list_len);
    session_ret = tlv_find_unique(payload,
                                  payload_len,
                                  TLV_MESH_ACK_SESSION_LIST,
                                  &session_list,
                                  &session_list_len);
    packet_id_ret = tlv_find_unique(payload,
                                    payload_len,
                                    TLV_MESH_ACK_PACKET_ID_LIST,
                                    &packet_id_list,
                                    &packet_id_list_len);
    if ((requested_ret != PROTO_OK &&
         requested_ret != PROTO_ERR_NOT_FOUND) ||
        (seq_ret != PROTO_OK && seq_ret != PROTO_ERR_NOT_FOUND) ||
        (session_ret != PROTO_OK &&
         session_ret != PROTO_ERR_NOT_FOUND) ||
        (packet_id_ret != PROTO_OK &&
         packet_id_ret != PROTO_ERR_NOT_FOUND)) {
        return PROTO_ERR_MALFORMED;
    }

    if (seq_ret == PROTO_OK) {
        uint8_t local_entry_count;

        if (seq_list_len == 0u ||
            (seq_list_len % sizeof(uint16_t)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        local_entry_count = seq_list_len / sizeof(uint16_t);
        if ((session_ret == PROTO_OK &&
             session_list_len != local_entry_count * sizeof(uint32_t)) ||
            (packet_id_ret == PROTO_OK &&
             packet_id_list_len != local_entry_count * sizeof(uint32_t))) {
            return PROTO_ERR_MALFORMED;
        }
        if (requested_ret == PROTO_OK &&
            (requested_len != sizeof(uint16_t) ||
             proto_get_u16_le(requested_value) !=
                 proto_get_u16_le(seq_list))) {
            return PROTO_ERR_MALFORMED;
        }

        for (uint8_t i = 0u; i < local_entry_count; i++) {
            uint16_t seq =
                proto_get_u16_le(&seq_list[i * sizeof(uint16_t)]);

            if (seq == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            if (session_ret == PROTO_OK) {
                uint32_t session_id =
                    proto_get_u32_le(&session_list[i * sizeof(uint32_t)]);

                if (session_id == 0u) {
                    return PROTO_ERR_MALFORMED;
                }
                if (session_id == requested_session_id &&
                    seq == requested_seq) {
                    *contains = true;
                }
            } else if (seq == requested_seq) {
                *contains = true;
            }
        }
        return PROTO_OK;
    }

    if (session_ret == PROTO_OK || packet_id_ret == PROTO_OK ||
        requested_ret != PROTO_OK ||
        requested_len != sizeof(uint16_t) ||
        proto_get_u16_le(requested_value) == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (ack_packet->session_id == requested_session_id &&
        proto_get_u16_le(requested_value) == requested_seq) {
        *contains = true;
    }
    return PROTO_OK;
}

bool mesh_gateway_ack_confirmed_type(uint8_t msg_type)
{
    switch (msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

bool mesh_gateway_ack_confirmed_flags_valid(uint8_t msg_type,
                                            uint8_t flags)
{
    const uint8_t ack = FLAG_GATEWAY_ACK_REQUIRED;
    const uint8_t diagnostic =
        FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;

    switch (msg_type) {
    case MSG_CLICK_REPORT:
        return flags == (ack | FLAG_COUNT_AS_CLICK) ||
               flags == diagnostic;
    case MSG_SELF_TEST_REPORT:
        return flags == diagnostic;
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_RESULT_BUNDLE:
        return flags == ack;
    case MSG_MESH_DATA:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return flags == diagnostic;
    case MSG_COMMAND_RESULT:
        return flags == ack || flags == diagnostic;
    default:
        return false;
    }
}

int mesh_gateway_ack_confirm_payload_build(
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    uint8_t *confirm_payload,
    size_t confirm_payload_cap,
    size_t *confirm_payload_len)
{
    uint8_t value[MESH_GATEWAY_ACK_CONFIRM_IDENTITY_VALUE_LEN];
    size_t offset = 0u;

    if (acknowledged_packet == NULL || confirm_payload == NULL ||
        confirm_payload_len == NULL ||
        (acknowledged_payload == NULL && acknowledged_payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    *confirm_payload_len = 0u;
    if (!mesh_gateway_ack_confirmed_type(acknowledged_packet->msg_type) ||
        !mesh_gateway_ack_confirmed_flags_valid(
            acknowledged_packet->msg_type,
            acknowledged_packet->flags) ||
        acknowledged_packet->session_id == 0u ||
        acknowledged_packet->seq == 0u ||
        acknowledged_packet->payload_len != acknowledged_payload_len ||
        (acknowledged_packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    value[0] = acknowledged_packet->msg_type;
    value[1] = acknowledged_packet->flags;
    proto_put_u32_le(&value[2], acknowledged_packet->session_id);
    proto_put_u16_le(&value[2u + sizeof(uint32_t)],
                     acknowledged_packet->seq);
    proto_put_u16_le(&value[2u + sizeof(uint32_t) + sizeof(uint16_t)],
                     acknowledged_packet->payload_len);
    if (!mesh_packet_semantic_digest(
            acknowledged_packet,
            acknowledged_payload,
            acknowledged_payload_len,
            &value[2u + sizeof(uint32_t) +
                   (2u * sizeof(uint16_t))])) {
        return PROTO_ERR_MALFORMED;
    }
    if (tlv_append_bytes(confirm_payload,
                         confirm_payload_cap,
                         &offset,
                         TLV_GATEWAY_ACK_CONFIRM_IDENTITY,
                         value,
                         (uint8_t)sizeof(value)) != PROTO_OK) {
        return PROTO_ERR_NO_SPACE;
    }
    *confirm_payload_len = offset;
    return PROTO_OK;
}

int mesh_gateway_ack_confirm_payload_parse(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct mesh_gateway_ack_confirm_identity *identity)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (confirm_packet == NULL || confirm_payload == NULL ||
        identity == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(identity, 0, sizeof(*identity));
    if (confirm_packet->msg_type != MSG_GATEWAY_ACK_CONFIRM ||
        confirm_packet->flags != FLAG_GATEWAY_ACK_REQUIRED ||
        confirm_packet->src_id == 0u || confirm_packet->dst_id == 0u ||
        confirm_packet->src_id == confirm_packet->dst_id ||
        confirm_packet->session_id == 0u || confirm_packet->seq == 0u ||
        confirm_packet->ttl == 0u ||
        confirm_packet->ttl > MESH_GATEWAY_ACK_TTL ||
        confirm_packet->payload_len != confirm_payload_len ||
        confirm_payload_len != MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_find_unique(confirm_payload,
                          confirm_payload_len,
                          TLV_GATEWAY_ACK_CONFIRM_IDENTITY,
                          &value,
                          &value_len);
    if (ret != PROTO_OK ||
        value_len != MESH_GATEWAY_ACK_CONFIRM_IDENTITY_VALUE_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    identity->msg_type = value[0];
    identity->flags = value[1];
    identity->session_id = proto_get_u32_le(&value[2]);
    identity->seq =
        proto_get_u16_le(&value[2u + sizeof(uint32_t)]);
    identity->payload_len =
        proto_get_u16_le(&value[2u + sizeof(uint32_t) + sizeof(uint16_t)]);
    memcpy(identity->digest,
           &value[2u + sizeof(uint32_t) + (2u * sizeof(uint16_t))],
           sizeof(identity->digest));
    if (!mesh_gateway_ack_confirmed_type(identity->msg_type) ||
        !mesh_gateway_ack_confirmed_flags_valid(identity->msg_type,
                                                identity->flags) ||
        identity->session_id == 0u || identity->seq == 0u ||
        identity->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        identity->session_id != confirm_packet->session_id ||
        identity->seq != confirm_packet->seq) {
        memset(identity, 0, sizeof(*identity));
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int mesh_gateway_ack_confirm_identity_packet(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct proto_packet *acknowledged_packet,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct mesh_gateway_ack_confirm_identity identity;
    int ret;

    if (acknowledged_packet == NULL || digest == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(acknowledged_packet, 0, sizeof(*acknowledged_packet));
    memset(digest, 0, SEMANTIC_DIGEST_SHA256_LEN);
    ret = mesh_gateway_ack_confirm_payload_parse(confirm_packet,
                                                 confirm_payload,
                                                 confirm_payload_len,
                                                 &identity);
    if (ret != PROTO_OK) {
        return ret;
    }
    acknowledged_packet->msg_type = identity.msg_type;
    acknowledged_packet->flags = identity.flags;
    acknowledged_packet->src_id = confirm_packet->src_id;
    acknowledged_packet->dst_id = confirm_packet->dst_id;
    acknowledged_packet->session_id = identity.session_id;
    acknowledged_packet->seq = identity.seq;
    acknowledged_packet->ttl = MESH_DEFAULT_TTL;
    acknowledged_packet->payload_len = identity.payload_len;
    memcpy(digest, identity.digest, SEMANTIC_DIGEST_SHA256_LEN);
    return PROTO_OK;
}

int mesh_gateway_ack_confirm_matches_packet(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    bool *matches)
{
    struct mesh_gateway_ack_confirm_identity identity;
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (acknowledged_packet == NULL || matches == NULL ||
        (acknowledged_payload == NULL && acknowledged_payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    *matches = false;
    ret = mesh_gateway_ack_confirm_payload_parse(confirm_packet,
                                                 confirm_payload,
                                                 confirm_payload_len,
                                                 &identity);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!mesh_packet_semantic_digest(acknowledged_packet,
                                     acknowledged_payload,
                                     acknowledged_payload_len,
                                     digest)) {
        return PROTO_ERR_MALFORMED;
    }
    *matches =
        confirm_packet->src_id == acknowledged_packet->src_id &&
        confirm_packet->dst_id == acknowledged_packet->dst_id &&
        identity.msg_type == acknowledged_packet->msg_type &&
        identity.session_id == acknowledged_packet->session_id &&
        identity.seq == acknowledged_packet->seq &&
        semantic_digest_equal(identity.digest, digest, sizeof(digest));
    return PROTO_OK;
}

int mesh_event_timing_negotiate(struct mesh_event_timing *timing,
                                const struct mesh_event_params *params,
                                bool channel5_contact_refreshed)
{
    if (timing == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!mesh_event_params_valid(params)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!channel5_contact_refreshed) {
        return PROTO_ERR_BUSY;
    }

    timing->mesh_channel = MESH_EVENT_CHANNEL;
    timing->event_interval_ms = params->event_interval_ms;
    timing->event_window_ms = params->event_window_ms;
    timing->next_event_time_ms = params->first_event_time_ms;
    timing->event_counter = 0u;
    timing->guard_ms = params->guard_ms;
    timing->peer_clock_skew_estimate_ppm = params->peer_clock_skew_estimate_ppm;
    timing->max_missed_events = params->max_missed_events;
    timing->missed_event_count = 0u;
    timing->supervision_timeout_ms = params->supervision_timeout_ms;
    timing->last_successful_ch9_event_ms = params->first_event_time_ms;
    timing->local_tx_on_even_events = true;
    timing->route_fresh = true;
    timing->timing_fresh = true;
    timing->fallback_required = false;
    return PROTO_OK;
}

bool mesh_event_timing_usable(const struct mesh_event_timing *timing,
                              uint32_t now_ms)
{
    if (timing == NULL ||
        timing->mesh_channel != MESH_EVENT_CHANNEL ||
        timing->event_interval_ms == 0u ||
        timing->event_window_ms == 0u ||
        timing->guard_ms == 0u ||
        timing->supervision_timeout_ms == 0u ||
        !timing->route_fresh ||
        !timing->timing_fresh ||
        timing->fallback_required) {
        return false;
    }

    return !time_reached(now_ms,
                         timing->last_successful_ch9_event_ms +
                         timing->supervision_timeout_ms);
}

void mesh_event_timing_set_local_first_slot_tx(struct mesh_event_timing *timing,
                                               bool local_first_slot_tx)
{
    bool first_event_is_even;

    if (timing == NULL) {
        return;
    }

    first_event_is_even = (timing->event_counter & 1u) == 0u;
    timing->local_tx_on_even_events =
        local_first_slot_tx == first_event_is_even;
}

bool mesh_event_timing_bind_proposal_session(
    struct mesh_event_timing *timing,
    uint32_t operation_session_id)
{
    if (timing == NULL || operation_session_id == 0u) {
        return false;
    }

    timing->event_counter = operation_session_id;
    mesh_event_timing_set_local_first_slot_tx(timing, true);
    return true;
}

bool mesh_event_timing_local_tx_slot(const struct mesh_event_timing *timing)
{
    bool even_event;

    if (timing == NULL) {
        return false;
    }

    even_event = (timing->event_counter & 1u) == 0u;
    return timing->local_tx_on_even_events == even_event;
}

bool mesh_event_timing_local_rx_slot(const struct mesh_event_timing *timing)
{
    return timing != NULL && !mesh_event_timing_local_tx_slot(timing);
}

uint32_t mesh_event_guard_start_ms(const struct mesh_event_timing *timing)
{
    uint16_t guard_ms;

    if (timing == NULL) {
        return 0u;
    }

    guard_ms = timing->guard_ms;
    return timing->next_event_time_ms - guard_ms;
}

void mesh_event_timing_reanchor_after_control_tx(
    struct mesh_event_timing *timing,
    uint32_t tx_done_ms,
    uint32_t encoded_delay_ms,
    uint32_t rx_reference_offset_ms)
{
    uint32_t delay_after_rx_ms = 1u;

    if (timing == NULL) {
        return;
    }
    if (encoded_delay_ms > rx_reference_offset_ms) {
        delay_after_rx_ms = encoded_delay_ms - rx_reference_offset_ms;
    }
    timing->next_event_time_ms = tx_done_ms + delay_after_rx_ms;
}

int mesh_event_plan_channel9(const struct mesh_event_timing *timing,
                             const struct mesh_channel5_requirements *requirements,
                             uint32_t now_ms,
                             struct mesh_event_plan *plan)
{
    uint32_t start_ms;
    uint32_t end_ms;
    uint32_t latest_end_ms = 0u;
    uint16_t retune_guard_ms;
    uint32_t guard_start_ms;
    bool latest_end_valid = false;

    if (timing == NULL || requirements == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }

    plan->start_ms = 0u;
    plan->end_ms = 0u;
    plan->window_ms = 0u;
    if (!mesh_event_timing_usable(timing, now_ms)) {
        plan->action = MESH_EVENT_PLAN_REFRESH_CONTACT_CH5;
        return PROTO_OK;
    }

    retune_guard_ms = requirements->retune_guard_ms > timing->guard_ms ?
                      requirements->retune_guard_ms : timing->guard_ms;
    start_ms = timing->next_event_time_ms;
    guard_start_ms = timing->next_event_time_ms - retune_guard_ms;
    plan->start_ms = start_ms;
    plan->window_ms = timing->event_window_ms;
    plan->end_ms = start_ms + timing->event_window_ms;

    if (!time_reached(now_ms, guard_start_ms)) {
        plan->action = MESH_EVENT_PLAN_WAIT;
        return PROTO_OK;
    }

    if (requirements->click_epoch_active ||
        requirements->discovery_active ||
        requirements->ranging_active ||
        (requirements->active_until_valid &&
         time_reached(requirements->active_until_ms, start_ms))) {
        plan->action = MESH_EVENT_PLAN_DEFER_CH5_ACTIVE;
        return PROTO_OK;
    }

    if (requirements->next_required_scan_start_valid &&
        !time_reached(requirements->next_required_scan_start_ms,
                      start_ms + retune_guard_ms)) {
        plan->action = MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
        plan->window_ms = 0u;
        plan->end_ms = start_ms;
        return PROTO_OK;
    }
    if (requirements->next_required_scan_start_valid) {
        latest_end_ms = requirements->next_required_scan_start_ms - retune_guard_ms;
        latest_end_valid = true;
    }
    end_ms = start_ms + timing->event_window_ms;
    if (latest_end_valid && time_reached(end_ms, latest_end_ms + 1u)) {
        if (time_reached(latest_end_ms, start_ms + 1u)) {
            plan->action = MESH_EVENT_PLAN_CLIP;
            plan->window_ms = (uint16_t)(latest_end_ms - start_ms);
            plan->end_ms = latest_end_ms;
            return PROTO_OK;
        }
        plan->action = MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
        plan->window_ms = 0u;
        plan->end_ms = start_ms;
        return PROTO_OK;
    }

    plan->action = MESH_EVENT_PLAN_START;
    return PROTO_OK;
}

bool mesh_event_plan_is_policy_deferral(enum mesh_event_plan_action action)
{
    return action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
           action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
}

void mesh_event_note_success(struct mesh_event_timing *timing,
                             uint32_t event_start_ms)
{
    if (timing == NULL) {
        return;
    }

    timing->last_successful_ch9_event_ms = event_start_ms;
    timing->next_event_time_ms = event_start_ms + timing->event_interval_ms;
    timing->event_counter++;
    timing->missed_event_count = 0u;
    timing->timing_fresh = true;
    timing->fallback_required = false;
}

void mesh_event_note_local_tx(struct mesh_event_timing *timing,
                              uint32_t event_start_ms)
{
    if (timing == NULL) {
        return;
    }

    timing->next_event_time_ms = event_start_ms + timing->event_interval_ms;
    timing->event_counter++;
    timing->timing_fresh = true;
    timing->fallback_required = false;
}

void mesh_event_note_observed_packet(struct mesh_event_timing *timing,
                                     uint32_t planned_event_start_ms,
                                     uint32_t observed_packet_ms)
{
    if (timing == NULL) {
        return;
    }

    (void)observed_packet_ms;
    if (timing->last_successful_ch9_event_ms == planned_event_start_ms &&
        timing->next_event_time_ms ==
            planned_event_start_ms + timing->event_interval_ms) {
        return;
    }
    mesh_event_note_success(timing, planned_event_start_ms);
}

void mesh_event_note_missed(struct mesh_event_timing *timing,
                            struct mesh_event_diagnostics *diagnostics)
{
    if (timing == NULL) {
        return;
    }

    if (timing->missed_event_count < UINT8_MAX) {
        timing->missed_event_count++;
    }
    timing->next_event_time_ms += timing->event_interval_ms;
    timing->event_counter++;
    if (timing->max_missed_events > 0u &&
        timing->missed_event_count >= timing->max_missed_events) {
        timing->timing_fresh = false;
        timing->fallback_required = true;
    }
    counter_add(diagnostics == NULL ? NULL : &diagnostics->ch9_event_misses, 1u);
}

uint8_t mesh_event_skip_elapsed(struct mesh_event_timing *timing,
                                uint32_t now_ms,
                                struct mesh_event_diagnostics *diagnostics)
{
    uint8_t skipped = 0u;

    if (timing == NULL ||
        timing->event_interval_ms == 0u ||
        timing->event_window_ms == 0u) {
        return 0u;
    }

    while (skipped < UINT8_MAX) {
        uint32_t event_end_ms = timing->next_event_time_ms + timing->event_window_ms;

        if (!time_reached(now_ms, event_end_ms)) {
            break;
        }
        if (mesh_event_timing_local_rx_slot(timing)) {
            mesh_event_note_missed(timing, diagnostics);
        } else {
            timing->next_event_time_ms += timing->event_interval_ms;
            timing->event_counter++;
        }
        skipped++;
    }

    return skipped;
}

void mesh_event_note_channel_switch(struct mesh_event_diagnostics *diagnostics,
                                    bool pll_ready,
                                    bool late_channel5_return)
{
    if (diagnostics == NULL) {
        return;
    }

    counter_add(&diagnostics->channel_switches, 1u);
    if (!pll_ready) {
        counter_add(&diagnostics->pll_ready_failures, 1u);
    }
    if (late_channel5_return) {
        counter_add(&diagnostics->late_channel5_returns, 1u);
    }
}

void mesh_event_note_plan_action(struct mesh_event_diagnostics *diagnostics,
                                 enum mesh_event_plan_action action)
{
    if (diagnostics == NULL) {
        return;
    }

    if (action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
        action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD ||
        action == MESH_EVENT_PLAN_CLIP) {
        counter_add(&diagnostics->mesh_deferrals, 1u);
    }
    if (action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
        action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD) {
        counter_add(&diagnostics->channel5_preemptions, 1u);
    }
}

void mesh_event_note_report_latency(struct mesh_event_diagnostics *diagnostics,
                                    uint32_t latency_ms)
{
    if (diagnostics == NULL) {
        return;
    }

    counter_add(&diagnostics->ch9_report_latency_ms, latency_ms);
}

int mesh_append_event_timing_tlvs_at(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct mesh_event_timing *timing,
                                     uint32_t now_ms)
{
    int ret;

    if (timing == NULL || timing->mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u8(payload, payload_cap, offset, TLV_MESH_CHANNEL, timing->mesh_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_EVENT_INTERVAL_MS,
                         timing->event_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_EVENT_WINDOW_MS,
                         timing->event_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_NEXT_EVENT_TIME_MS,
                         time_until_deadline(now_ms, timing->next_event_time_ms));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_EVENT_COUNTER,
                         timing->event_counter);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_EVENT_GUARD_MS,
                         timing->guard_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_CLOCK_SKEW_PPM,
                         (uint16_t)timing->peer_clock_skew_estimate_ppm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_MESH_MAX_MISSED_EVENTS,
                        timing->max_missed_events);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset,
                          TLV_MESH_SUPERVISION_TIMEOUT_MS,
                          timing->supervision_timeout_ms);
}

int mesh_append_event_timing_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct mesh_event_timing *timing)
{
    return mesh_append_event_timing_tlvs_at(payload, payload_cap, offset, timing, 0u);
}

int mesh_append_event_update_tlvs_at(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct mesh_event_timing *timing,
                                     uint32_t now_ms)
{
    int ret = mesh_append_event_timing_tlvs_at(payload,
                                               payload_cap,
                                               offset,
                                               timing,
                                               now_ms);

    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload,
                         payload_cap,
                         offset,
                         TLV_MESH_EVENT_TX_ON_EVEN,
                         timing->local_tx_on_even_events ? 1u : 0u);
}

int mesh_append_compact_event_timing_tlvs_at(uint8_t *payload,
                                             size_t payload_cap,
                                             size_t *offset,
                                             const struct mesh_event_timing *timing,
                                             uint32_t now_ms)
{
    int ret;

    if (timing == NULL || timing->mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_EVENT_INTERVAL_MS,
                         timing->event_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_MESH_EVENT_WINDOW_MS,
                         timing->event_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_MESH_NEXT_EVENT_TIME_MS,
                         time_until_deadline(now_ms, timing->next_event_time_ms));
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset,
                          TLV_MESH_EVENT_GUARD_MS,
                          timing->guard_ms);
}

int mesh_event_timing_from_tlvs_at(struct mesh_event_timing *timing,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms,
                                   bool channel5_contact_refreshed)
{
    struct mesh_event_params params = {0};
    uint8_t mesh_channel = 0u;
    uint16_t clock_skew = 0u;
    uint32_t event_counter = 0u;
    uint32_t event_delay_ms = 0u;
    uint8_t local_tx_on_even = 0u;
    bool event_counter_present = false;
    bool local_tx_on_even_present = false;
    int ret;

    if (timing == NULL || payload == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = find_u8_tlv(payload, payload_len, TLV_MESH_CHANNEL, &mesh_channel);
    if (ret == PROTO_ERR_NOT_FOUND) {
        mesh_channel = MESH_EVENT_CHANNEL;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_INTERVAL_MS,
                       &params.event_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_WINDOW_MS,
                       &params.event_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_NEXT_EVENT_TIME_MS,
                       &event_delay_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    params.first_event_time_ms = deadline_after(now_ms, event_delay_ms);
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_GUARD_MS,
                       &params.guard_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_MESH_CLOCK_SKEW_PPM,
                       &clock_skew);
    if (ret == PROTO_ERR_NOT_FOUND) {
        clock_skew = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload,
                      payload_len,
                      TLV_MESH_MAX_MISSED_EVENTS,
                      &params.max_missed_events);
    if (ret == PROTO_ERR_NOT_FOUND) {
        params.max_missed_events = MESH_EVENT_COMPACT_DEFAULT_MAX_MISSED;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_SUPERVISION_TIMEOUT_MS,
                       &params.supervision_timeout_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        params.supervision_timeout_ms =
            compact_supervision_timeout_ms(params.event_interval_ms);
    } else if (ret != PROTO_OK) {
        return ret;
    }

    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_MESH_EVENT_COUNTER,
                       &event_counter);
    if (ret == PROTO_OK) {
        event_counter_present = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    ret = find_u8_tlv(payload,
                      payload_len,
                      TLV_MESH_EVENT_TX_ON_EVEN,
                      &local_tx_on_even);
    if (ret == PROTO_OK) {
        if (local_tx_on_even > 1u) {
            return PROTO_ERR_MALFORMED;
        }
        local_tx_on_even_present = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }

    params.peer_clock_skew_estimate_ppm = (int16_t)clock_skew;
    ret = mesh_event_timing_negotiate(timing, &params, channel5_contact_refreshed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (event_counter_present) {
        timing->event_counter = event_counter;
    }
    if (local_tx_on_even_present) {
        timing->local_tx_on_even_events = local_tx_on_even != 0u;
    }
    return PROTO_OK;
}

int mesh_event_timing_from_tlvs(struct mesh_event_timing *timing,
                                const uint8_t *payload,
                                size_t payload_len,
                                bool channel5_contact_refreshed)
{
    return mesh_event_timing_from_tlvs_at(timing,
                                          payload,
                                          payload_len,
                                          0u,
                                          channel5_contact_refreshed);
}

int mesh_init_event_control(struct proto_packet *packet,
                            uint8_t msg_type,
                            uint64_t local_id,
                            uint64_t peer_id,
                            uint32_t session_id,
                            uint16_t seq,
                            uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!event_control_type_valid(msg_type) ||
        !ids_are_valid(local_id, peer_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = msg_type;
    packet->flags = 0u;
    packet->src_id = local_id;
    packet->dst_id = peer_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int mesh_append_requested_seq(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   uint16_t requested_seq)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_REQUESTED_MSG_SEQ, requested_seq);
}

int mesh_append_command_id(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                enum command_id command_id)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_ID, (uint16_t)command_id);
}

int mesh_append_command_result(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    enum command_id command_id,
                                    enum command_status status,
                                    uint8_t reason)
{
    int ret;

    if (!command_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_append_command_id(payload, payload_cap, offset, command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_STATUS, (uint16_t)status);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_REASON, reason);
}

int mesh_init_gateway_ack(struct proto_packet *packet,
                               uint64_t gateway_id,
                               uint64_t original_src_id,
                               uint32_t session_id,
                               uint16_t ack_seq,
                               uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, original_src_id) ||
        session_id == 0u ||
        ack_seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_GATEWAY_ACK;
    packet->flags = FLAG_GATEWAY_ACK;
    packet->src_id = gateway_id;
    packet->dst_id = original_src_id;
    packet->session_id = session_id;
    packet->seq = ack_seq;
    packet->ttl = MESH_GATEWAY_ACK_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int mesh_init_gateway_ack_confirm(struct proto_packet *packet,
                                  uint64_t source_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t confirm_seq)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(source_id, gateway_id) ||
        session_id == 0u || confirm_seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_GATEWAY_ACK_CONFIRM;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet->src_id = source_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = confirm_seq;
    packet->ttl = MESH_GATEWAY_ACK_TTL;
    packet->payload_len = MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int mesh_init_command(struct proto_packet *packet,
                           uint64_t gateway_id,
                           uint64_t target_id,
                           uint32_t session_id,
                           uint16_t seq,
                           uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, target_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND;
    packet->flags = 0u;
    packet->src_id = gateway_id;
    packet->dst_id = target_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int mesh_init_command_result(struct proto_packet *packet,
                                  uint64_t target_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len,
                                  bool diagnostic)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(target_id, gateway_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND_RESULT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    if (diagnostic) {
        packet->flags |= FLAG_DIAGNOSTIC;
    }
    packet->src_id = target_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}
