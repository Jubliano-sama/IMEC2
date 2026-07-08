#include "mesh_relay.h"

#include "mesh.h"

#include <string.h>

#define LEGACY_MSG_ROUTE_ADV 0x33u
#define LEGACY_MSG_ROUTE_STATUS 0x34u
#define MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION 1u
#define FLOOD_BETTER_METRIC_MARGIN_PERCENT 20u

struct route_discovery_fields {
    uint64_t origin_id;
    uint64_t target_id;
    struct mesh_event_timing proposed_channel9_timing;
    uint32_t route_epoch;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;
    uint32_t timing_reference_ms;
    uint16_t flood_profile_version;
    uint16_t reply_nonce;
    uint16_t metric_crc;
    uint16_t queue_free_hint;
    uint16_t capacity_validity_interval_ms;
    uint16_t route_reply_rx_delay_ms;
    uint8_t hop_count;
    uint8_t quality;
    uint8_t relay_capacity_state;
    uint8_t channel9_busy_hint;
    uint8_t request_flags;
    bool proposed_channel9_timing_valid;
};

struct route_reply_ack_fields {
    uint64_t origin_id;
    uint64_t target_id;
    uint32_t flood_epoch_id;
    uint16_t reply_nonce;
    uint16_t metric_crc;
};

struct relay_busy_fields {
    uint32_t requested_session_id;
    uint16_t requested_seq;
    uint16_t retry_after_ms;
    uint16_t capacity_validity_interval_ms;
    uint8_t capacity_state;
    uint64_t alternate_parent_id;
    bool has_alternate_parent;
};

struct gateway_route_adv_fields {
    uint64_t gateway_id;
    uint32_t gateway_route_seq;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;
    uint32_t random_backoff_max_ms;
    uint32_t flood_packet_age_ms;
    uint16_t gateway_epoch;
    uint16_t route_cost;
    uint16_t flood_profile_version;
    uint16_t capacity_validity_interval_ms;
    uint16_t random_backoff_slot_ms;
    uint8_t hop_count;
    uint8_t path_quality_min;
    uint8_t gateway_capacity_state;
    uint8_t flood_retry_count;
};

struct flood_control_fields {
    uint32_t random_backoff_max_ms;
    uint16_t random_backoff_slot_ms;
    uint8_t retry_count;
};

static bool id_is_unicast(uint64_t id)
{
    return id != MESH_BROADCAST_ID;
}

static uint16_t relay_next_seq(struct mesh_relay *relay)
{
    relay->next_seq++;
    if (relay->next_seq == 0u) {
        relay->next_seq = 1u;
    }
    return relay->next_seq;
}

static void result_reset(struct mesh_relay_result *result)
{
    memset(result, 0, sizeof(*result));
    result->status = PROTO_OK;
}

static uint32_t packet_age_add(uint32_t age_ms, uint32_t elapsed_ms)
{
    if (UINT32_MAX - age_ms < elapsed_ms) {
        return UINT32_MAX;
    }
    return age_ms + elapsed_ms;
}

static uint32_t jittered_delay_ms(uint32_t base_ms, uint32_t random_value)
{
    return base_ms + (random_value % (base_ms + 1u));
}

static uint32_t retry_jittered_delay_ms(uint32_t base_ms, uint32_t random_value)
{
    uint32_t jitter_span_ms = base_ms / 2u;

    return base_ms + (random_value % (jitter_span_ms + 1u));
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

static uint32_t collection_retry_seed(uint64_t node_id,
                                      uint32_t command_seq,
                                      uint32_t collection_epoch_id,
                                      uint8_t retry_round)
{
    uint32_t seed = command_seq ^ collection_epoch_id;

    seed ^= (uint32_t)node_id;
    seed ^= (uint32_t)(node_id >> 32);
    seed ^= (uint32_t)retry_round << 24;
    return mix32(seed);
}

static uint8_t route_request_ttl_for_attempt(uint8_t previous_attempts, bool critical)
{
    if (critical) {
        return FLOOD_EPOCH_CRITICAL_TTL;
    }
    if (previous_attempts == 0u) {
        return 1u;
    }
    if (previous_attempts == 1u) {
        return 2u;
    }
    if (previous_attempts == 2u) {
        return 4u;
    }
    return 6u;
}

static uint32_t route_discovery_slot_seed(uint64_t origin_id,
                                          uint64_t target_id,
                                          uint32_t request_id,
                                          uint32_t route_epoch)
{
    uint32_t seed = request_id ^ route_epoch;

    seed ^= (uint32_t)origin_id;
    seed ^= (uint32_t)(origin_id >> 32);
    seed ^= (uint32_t)target_id;
    seed ^= (uint32_t)(target_id >> 32);
    return seed == 0u ? 1u : seed;
}

static uint32_t gateway_route_adv_slot_seed(uint64_t gateway_id,
                                            uint32_t gateway_route_seq,
                                            uint16_t gateway_epoch)
{
    uint32_t seed = gateway_route_seq ^ gateway_epoch;

    seed ^= (uint32_t)gateway_id;
    seed ^= (uint32_t)(gateway_id >> 32);
    return seed == 0u ? 1u : seed;
}

static uint32_t flood_forward_delay_ms(uint64_t local_id,
                                       uint32_t slot_seed,
                                       uint8_t hop_count)
{
    uint32_t seed = slot_seed ^ ((uint32_t)hop_count << 24);

    seed ^= (uint32_t)local_id;
    seed ^= (uint32_t)(local_id >> 32);
    return mix32(seed) % FLOOD_WAVE_MS;
}

static uint32_t flood_random_slotted_delay_ms(uint32_t random_backoff_max_ms,
                                             uint16_t random_backoff_slot_ms,
                                             uint32_t random_value)
{
    uint32_t slot_count;
    uint64_t delay_ms;

    if (random_backoff_max_ms == 0u) {
        return 0u;
    }
    if (random_backoff_slot_ms == 0u) {
        if (random_backoff_max_ms == UINT32_MAX) {
            return random_value;
        }
        return random_value % (random_backoff_max_ms + 1u);
    }

    slot_count = random_backoff_max_ms / random_backoff_slot_ms;
    if (slot_count == UINT32_MAX) {
        slot_count--;
    }
    delay_ms = (uint64_t)(random_value % (slot_count + 1u)) * random_backoff_slot_ms;
    return delay_ms > random_backoff_max_ms ? random_backoff_max_ms : (uint32_t)delay_ms;
}

static uint32_t flood_forward_total_delay_ms(uint64_t local_id,
                                             uint32_t slot_seed,
                                             uint8_t hop_count,
                                             const struct flood_control_fields *control,
                                             uint32_t random_value)
{
    uint32_t deterministic_ms = flood_forward_delay_ms(local_id, slot_seed, hop_count);

    if (control == NULL) {
        return deterministic_ms;
    }
    {
        uint32_t random_ms =
            flood_random_slotted_delay_ms(control->random_backoff_max_ms,
                                          control->random_backoff_slot_ms,
                                          random_value);

        return UINT32_MAX - deterministic_ms < random_ms ?
               UINT32_MAX : deterministic_ms + random_ms;
    }
}

static uint32_t flood_identity_seed(const struct proto_packet *packet,
                                    uint64_t local_id)
{
    uint32_t seed = packet == NULL ? 0u : packet->session_id;

    if (packet != NULL) {
        seed ^= (uint32_t)packet->seq << 16;
        seed ^= (uint32_t)packet->msg_type << 8;
        seed ^= packet->ttl;
    }
    seed ^= (uint32_t)local_id;
    seed ^= (uint32_t)(local_id >> 32);
    return seed == 0u ? 1u : seed;
}

static uint16_t gateway_route_cost(uint8_t hop_count, uint8_t path_quality_min)
{
    return route_candidate_cost(hop_count, path_quality_min);
}

static uint8_t relay_active_channel9_timing_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    if (relay == NULL) {
        return 0u;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid) {
            count++;
        }
    }
    return count;
}

static uint8_t relay_current_capacity_state(const struct mesh_relay *relay)
{
    if (relay == NULL) {
        return RELAY_CAP_BLACK;
    }
    if (!id_is_unicast(relay->local_id) || !id_is_unicast(relay->gateway_id) ||
        (relay->role != MESH_RELAY_ROLE_ANCHOR &&
         relay->role != MESH_RELAY_ROLE_GATEWAY)) {
        return RELAY_CAP_BLACK;
    }
    if (relay->result_offer_reservation.valid ||
        (relay->result_bundle.active &&
         relay->result_bundle.record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS)) {
        return RELAY_CAP_RED;
    }
    if (relay->pending.state != MESH_RELAY_TX_IDLE) {
        return RELAY_CAP_RED;
    }
    if (relay->result_bundle.active && relay->result_bundle.record_count > 0u) {
        return RELAY_CAP_YELLOW;
    }
    return RELAY_CAP_GREEN;
}

static uint16_t relay_current_queue_free_hint(const struct mesh_relay *relay)
{
    uint16_t free_slots = MESH_RELAY_RESULT_BUNDLE_RECORDS;

    if (relay == NULL || relay_current_capacity_state(relay) >= RELAY_CAP_RED) {
        return 0u;
    }
    if (relay->pending.state != MESH_RELAY_TX_IDLE) {
        return 0u;
    }
    if (relay->result_bundle.active) {
        if (relay->result_bundle.record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS) {
            return 0u;
        }
        free_slots = MESH_RELAY_RESULT_BUNDLE_RECORDS - relay->result_bundle.record_count;
    }
    return free_slots;
}

static uint16_t relay_current_capacity_validity_interval_ms(const struct mesh_relay *relay)
{
    return relay_current_capacity_state(relay) == RELAY_CAP_UNKNOWN ?
           0u :
           RELAY_CAPACITY_HINT_VALIDITY_MS;
}

static uint32_t capacity_valid_until_ms(uint32_t observed_at_ms, uint16_t interval_ms)
{
    if (interval_ms == 0u) {
        return 0u;
    }
    return observed_at_ms + interval_ms;
}

static uint16_t relay_busy_retry_after_ms(const struct mesh_relay *relay)
{
    uint16_t retry_after_ms = RELAY_BUSY_RETRY_MIN_MS;

    if (relay_current_capacity_state(relay) >= RELAY_CAP_RED) {
        retry_after_ms = RELAY_BUSY_RETRY_MAX_MS;
    }
    if (retry_after_ms < RELAY_BUSY_RETRY_MIN_MS) {
        return RELAY_BUSY_RETRY_MIN_MS;
    }
    if (retry_after_ms > RELAY_BUSY_RETRY_MAX_MS) {
        return RELAY_BUSY_RETRY_MAX_MS;
    }
    return retry_after_ms;
}

static uint16_t route_reply_nonce(uint64_t origin_id,
                                  uint64_t target_id,
                                  uint32_t request_id,
                                  uint32_t flood_epoch_id)
{
    uint32_t mix = request_id ^ flood_epoch_id;

    mix ^= (uint32_t)origin_id;
    mix ^= (uint32_t)(origin_id >> 32);
    mix ^= (uint32_t)target_id;
    mix ^= (uint32_t)(target_id >> 32);

    uint16_t nonce = (uint16_t)(mix ^ (mix >> 16));

    return nonce == 0u ? 1u : nonce;
}

static uint16_t route_reply_metric_crc(const struct route_discovery_fields *fields)
{
    uint8_t metric[42];
    size_t offset = 0u;
    uint16_t crc;

    proto_put_u64_le(&metric[offset], fields->origin_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&metric[offset], fields->target_id);
    offset += sizeof(uint64_t);
    proto_put_u32_le(&metric[offset], fields->route_epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&metric[offset], fields->flood_epoch_id);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&metric[offset], fields->flood_profile_version);
    offset += sizeof(uint16_t);
    metric[offset++] = fields->hop_count;
    metric[offset++] = fields->quality;
    metric[offset++] = fields->relay_capacity_state;
    metric[offset++] = fields->channel9_busy_hint;
    proto_put_u16_le(&metric[offset], fields->queue_free_hint);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&metric[offset], fields->capacity_validity_interval_ms);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&metric[offset], fields->slot_seed);
    offset += sizeof(uint32_t);

    crc = proto_crc16_ccitt_false(metric, offset);
    return crc == 0u ? 1u : crc;
}

static void relay_diag_inc_u8(uint8_t *counter)
{
    if (counter != NULL && *counter < UINT8_MAX) {
        (*counter)++;
    }
}

uint32_t mesh_relay_retry_backoff_ms(uint8_t failure_count, uint32_t random_value)
{
    return retry_jittered_delay_ms(route_retry_backoff_ms(failure_count),
                                   random_value);
}

uint32_t mesh_relay_route_discovery_backoff_ms(uint8_t attempt_count,
                                               uint32_t random_value)
{
    uint32_t backoff_ms = MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_BASE_MS;

    if (attempt_count > 1u) {
        for (uint8_t i = 1u; i < attempt_count; i++) {
            if (backoff_ms >= MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS / 2u) {
                backoff_ms = MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS;
                break;
            }
            backoff_ms *= 2u;
        }
    }
    if (backoff_ms > MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS) {
        backoff_ms = MESH_RELAY_ROUTE_DISCOVERY_BACKOFF_MAX_MS;
    }

    return jittered_delay_ms(backoff_ms, random_value);
}

uint32_t mesh_relay_collection_retry_delay_ms(uint32_t base_delay_ms,
                                              uint32_t random_value)
{
    uint32_t jitter_ms;
    uint32_t window_ms;

    if (base_delay_ms == 0u) {
        return 0u;
    }

    jitter_ms = (uint32_t)(((uint64_t)base_delay_ms *
                            COLLECTION_RETRY_JITTER_PERCENT) / 100u);
    if (jitter_ms == 0u) {
        return base_delay_ms;
    }

    window_ms = (jitter_ms * 2u) + 1u;
    return (base_delay_ms - jitter_ms) + (random_value % window_ms);
}

void mesh_relay_note_route_reply_retry(struct mesh_relay *relay)
{
    if (relay == NULL) {
        return;
    }
    relay_diag_inc_u8(&relay->diagnostics.route_reply_retry_count);
}

static int append_route_discovery_tlvs(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *offset,
                                       const struct route_discovery_fields *fields)
{
    int ret;

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_INITIATOR_ID, fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_RESPONDER_ID, fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_ROUTE_EPOCH, fields->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, fields->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_RELAY_CAPACITY_STATE, fields->relay_capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_QUEUE_FREE_HINT, fields->queue_free_hint);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_CHANNEL9_BUSY_HINT, fields->channel9_busy_hint);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                         fields->capacity_validity_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_FLOOD_EPOCH_ID, fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_FLOOD_PROFILE_VERSION, fields->flood_profile_version);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SLOT_SEED, fields->slot_seed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->request_flags != 0u) {
        ret = tlv_append_u8(payload,
                            payload_cap,
                            offset,
                            TLV_ROUTE_REQUEST_FLAGS,
                            fields->request_flags);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (fields->route_reply_rx_delay_ms != 0u) {
        ret = tlv_append_u16(payload,
                             payload_cap,
                             offset,
                             TLV_ROUTE_REPLY_RX_DELAY_MS,
                             fields->route_reply_rx_delay_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (fields->proposed_channel9_timing_valid) {
        ret = mesh_append_compact_event_timing_tlvs_at(payload,
                                                       payload_cap,
                                                       offset,
                                                       &fields->proposed_channel9_timing,
                                                       fields->timing_reference_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (fields->reply_nonce != 0u || fields->metric_crc != 0u) {
        ret = tlv_append_u16(payload, payload_cap, offset,
                             TLV_REPLY_NONCE, fields->reply_nonce);
        if (ret != PROTO_OK) {
            return ret;
        }
        return tlv_append_u16(payload, payload_cap, offset,
                              TLV_METRIC_CRC, fields->metric_crc);
    }
    return PROTO_OK;
}

static int find_u64_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

static int find_u32_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int find_u16_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int find_u8_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static void flood_control_defaults(struct flood_control_fields *control)
{
    if (control == NULL) {
        return;
    }
    control->random_backoff_max_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS;
    control->random_backoff_slot_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS;
    control->retry_count = FLOOD_DEFAULT_RETRY_COUNT;
}

static int parse_flood_control_tlvs(const uint8_t *payload,
                                    size_t payload_len,
                                    struct flood_control_fields *control)
{
    uint32_t value_u32 = 0u;
    uint16_t value_u16 = 0u;
    uint8_t value_u8 = 0u;
    int ret;

    if (control == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }

    flood_control_defaults(control);
    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                       &value_u32);
    if (ret == PROTO_OK) {
        control->random_backoff_max_ms = value_u32;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }

    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                       &value_u16);
    if (ret == PROTO_OK) {
        control->random_backoff_slot_ms = value_u16;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }

    ret = find_u8_tlv(payload, payload_len, TLV_FLOOD_RETRY_COUNT, &value_u8);
    if (ret == PROTO_OK) {
        control->retry_count = value_u8;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    return PROTO_OK;
}

static int append_flood_control_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct flood_control_fields *control,
                                     uint32_t packet_age_ms)
{
    int ret;

    if (control == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                         control->random_backoff_max_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload,
                         payload_cap,
                         offset,
                         TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                         control->random_backoff_slot_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_FLOOD_RETRY_COUNT,
                        control->retry_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          offset,
                          TLV_FLOOD_PACKET_AGE_MS,
                          packet_age_ms);
}

int mesh_outbound_set_flood_packet_age_ms(struct mesh_outbound *out,
                                          uint32_t age_ms)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(out->payload,
                   out->payload_len,
                   TLV_FLOOD_PACKET_AGE_MS,
                   &tlv_value,
                   &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    proto_put_u32_le((uint8_t *)tlv_value, age_ms);
    return PROTO_OK;
}

static int ensure_flood_control_tlvs(struct mesh_outbound *out,
                                     const struct flood_control_fields *control,
                                     uint32_t packet_age_ms)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    size_t offset;
    int ret;

    if (out == NULL || control == NULL) {
        return PROTO_ERR_ARG;
    }

    offset = out->payload_len;
    ret = tlv_find(out->payload,
                   out->payload_len,
                   TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                   &tlv_value,
                   &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        ret = tlv_append_u32(out->payload,
                             sizeof(out->payload),
                             &offset,
                             TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                             control->random_backoff_max_ms);
    } else if (ret == PROTO_OK && tlv_len != sizeof(uint32_t)) {
        ret = PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_find(out->payload,
                   out->payload_len,
                   TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                   &tlv_value,
                   &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        ret = tlv_append_u16(out->payload,
                             sizeof(out->payload),
                             &offset,
                             TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                             control->random_backoff_slot_ms);
    } else if (ret == PROTO_OK && tlv_len != sizeof(uint16_t)) {
        ret = PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_find(out->payload,
                   out->payload_len,
                   TLV_FLOOD_RETRY_COUNT,
                   &tlv_value,
                   &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        ret = tlv_append_u8(out->payload,
                            sizeof(out->payload),
                            &offset,
                            TLV_FLOOD_RETRY_COUNT,
                            control->retry_count);
    } else if (ret == PROTO_OK && tlv_len != sizeof(uint8_t)) {
        ret = PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    out->payload_len = (uint16_t)offset;
    out->packet.payload_len = (uint16_t)offset;
    ret = mesh_outbound_set_flood_packet_age_ms(out, packet_age_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        ret = tlv_append_u32(out->payload,
                             sizeof(out->payload),
                             &offset,
                             TLV_FLOOD_PACKET_AGE_MS,
                             packet_age_ms);
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    out->payload_len = (uint16_t)offset;
    out->packet.payload_len = (uint16_t)offset;
    return PROTO_OK;
}

bool mesh_route_request_reply_rx_delay_ms(const struct mesh_outbound *out,
                                          uint16_t *delay_ms)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    if (out == NULL ||
        delay_ms == NULL ||
        out->packet.msg_type != MSG_ROUTE_REQ) {
        return false;
    }
    if (tlv_find(out->payload,
                 out->payload_len,
                 TLV_ROUTE_REPLY_RX_DELAY_MS,
                 &tlv_value,
                 &tlv_len) != PROTO_OK ||
        tlv_len != sizeof(uint16_t)) {
        return false;
    }

    *delay_ms = proto_get_u16_le(tlv_value);
    return true;
}

int mesh_route_request_set_reply_rx_delay_ms(struct mesh_outbound *out,
                                             uint16_t delay_ms)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    if (out == NULL ||
        out->packet.msg_type != MSG_ROUTE_REQ) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(out->payload,
                   out->payload_len,
                   TLV_ROUTE_REPLY_RX_DELAY_MS,
                   &tlv_value,
                   &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    proto_put_u16_le((uint8_t *)tlv_value, delay_ms);
    return PROTO_OK;
}

static uint8_t combined_quality(uint8_t advertised_quality, uint8_t link_quality)
{
    if (advertised_quality > 100u) {
        advertised_quality = 100u;
    }
    if (link_quality > 100u) {
        link_quality = 100u;
    }
    if (link_quality == 0u) {
        return advertised_quality;
    }
    if (advertised_quality == 0u) {
        return link_quality;
    }
    return advertised_quality < link_quality ? advertised_quality : link_quality;
}

static uint16_t downlink_effective_cost(const struct mesh_downlink_entry *entry)
{
    return (uint16_t)((uint16_t)entry->hop_count * 100u + (uint16_t)(100u - entry->quality));
}

static bool downlink_is_better(const struct mesh_downlink_entry *candidate,
                               const struct mesh_downlink_entry *selected);

static int downlink_index(const struct mesh_relay *relay, uint64_t target_id)
{
    int selected_index = -1;
    const struct mesh_downlink_entry *selected = NULL;

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (!entry->valid || entry->target_id != target_id) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
            selected_index = (int)i;
        }
    }
    return selected_index;
}

static const struct mesh_downlink_entry *downlink_backup_for(const struct mesh_relay *relay,
                                                             uint64_t target_id,
                                                             uint64_t primary_next_hop)
{
    const struct mesh_downlink_entry *selected = NULL;

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (!entry->valid ||
            entry->target_id != target_id ||
            entry->next_hop_id == primary_next_hop) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
        }
    }
    return selected;
}

static int downlink_exact_index(const struct mesh_relay *relay,
                                uint64_t target_id,
                                uint64_t gateway_id,
                                uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid &&
            entry->target_id == target_id &&
            entry->gateway_id == gateway_id &&
            entry->next_hop_id == next_hop_id) {
            return (int)i;
        }
    }
    return -1;
}

static bool downlink_is_better(const struct mesh_downlink_entry *candidate,
                               const struct mesh_downlink_entry *selected)
{
    uint16_t candidate_cost;
    uint16_t selected_cost;

    if (selected == NULL) {
        return true;
    }
    if (candidate->route_epoch != selected->route_epoch) {
        return candidate->route_epoch > selected->route_epoch;
    }
    candidate_cost = downlink_effective_cost(candidate);
    selected_cost = downlink_effective_cost(selected);
    if (candidate_cost != selected_cost) {
        return candidate_cost < selected_cost;
    }
    if (candidate->quality != selected->quality) {
        return candidate->quality > selected->quality;
    }
    if (candidate->hop_count != selected->hop_count) {
        return candidate->hop_count < selected->hop_count;
    }
    if (candidate->last_seen_ms != selected->last_seen_ms) {
        return candidate->last_seen_ms > selected->last_seen_ms;
    }
    return candidate->next_hop_id < selected->next_hop_id;
}

static int upsert_downlink(struct mesh_relay *relay, const struct mesh_downlink_entry *entry)
{
    int index;
    int free_index = -1;
    int replace_index = 0;
    const struct mesh_downlink_entry *replace = NULL;

    if (!id_is_unicast(entry->target_id) ||
        !id_is_unicast(entry->next_hop_id) ||
        !id_is_unicast(entry->gateway_id) ||
        entry->target_id == relay->local_id ||
        entry->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }

    index = downlink_exact_index(relay,
                                 entry->target_id,
                                 entry->gateway_id,
                                 entry->next_hop_id);
    if (index >= 0) {
        relay->downlinks[index] = *entry;
        relay->downlinks[index].valid = true;
        return PROTO_OK;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        if (!relay->downlinks[i].valid) {
            free_index = (int)i;
            break;
        }
        if (replace == NULL || downlink_is_better(replace, &relay->downlinks[i])) {
            replace = &relay->downlinks[i];
            replace_index = (int)i;
        }
    }

    if (free_index < 0 && !downlink_is_better(entry, replace)) {
        return PROTO_ERR_NO_SPACE;
    }
    index = free_index >= 0 ? free_index : replace_index;
    relay->downlinks[index] = *entry;
    relay->downlinks[index].valid = true;
    return PROTO_OK;
}

static bool duplicate_tracked(const struct proto_packet *packet)
{
    if (packet != NULL &&
        packet->msg_type == MSG_ROUTE_REQ &&
        packet->dst_id == MESH_BROADCAST_ID) {
        return false;
    }
    return true;
}

static bool duplicate_matches_packet(const struct mesh_duplicate_entry *entry,
                                     const struct proto_packet *packet)
{
    if (!entry->valid ||
        entry->msg_type != packet->msg_type ||
        entry->src_id != packet->src_id ||
        entry->dst_id != packet->dst_id ||
        entry->session_id != packet->session_id) {
        return false;
    }

    if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV) {
        return true;
    }
    if (packet->msg_type == MSG_COMMAND && packet->dst_id == MESH_BROADCAST_ID) {
        return true;
    }
    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
        packet->dst_id == MESH_BROADCAST_ID) {
        return true;
    }

    return entry->seq == packet->seq;
}

static uint32_t duplicate_window_ms(const struct mesh_duplicate_entry *entry)
{
    if ((entry->msg_type == MSG_COMMAND ||
         entry->msg_type == MSG_GATEWAY_COLLECTION_EACK) &&
        entry->dst_id == MESH_BROADCAST_ID) {
        return COMMAND_RESULT_EXPIRY_DEFAULT_S * 1000u;
    }
    return ROUTE_DEDUP_WINDOW_MS;
}

static void duplicate_expire_stale(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (entry->valid &&
            (uint32_t)(now_ms - entry->last_seen_ms) > duplicate_window_ms(entry)) {
            entry->valid = false;
        }
    }
}

static bool duplicate_seen(struct mesh_relay *relay,
                           const struct proto_packet *packet,
                           uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    if (!duplicate_tracked(packet)) {
        return false;
    }

    duplicate_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (duplicate_matches_packet(entry, packet)) {
            return true;
        }
    }
    return false;
}

static void duplicate_store(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    if (!duplicate_tracked(packet)) {
        return;
    }

    duplicate_expire_stale(relay, now_ms);
    entry = &relay->duplicates[relay->duplicate_next];
    entry->msg_type = packet->msg_type;
    entry->src_id = packet->src_id;
    entry->dst_id = packet->dst_id;
    entry->session_id = packet->session_id;
    entry->last_seen_ms = now_ms;
    entry->seq = packet->seq;
    entry->valid = true;
    relay->duplicate_next = (uint8_t)((relay->duplicate_next + 1u) % MESH_RELAY_DUP_CACHE_SIZE);
}

static uint32_t flood_seen_window_ms(uint8_t flood_type)
{
    (void)flood_type;
    return ROUTE_DEDUP_WINDOW_MS;
}

static void flood_seen_expire_stale(struct mesh_relay *relay, uint32_t now_ms)
{
    for (uint8_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        struct flood_seen_entry *entry = &relay->flood_seen[i];

        if (entry->valid && (int32_t)(now_ms - entry->expires_at_ms) >= 0) {
            entry->valid = false;
        }
    }
}

static bool flood_seen_matches_route_solicit(const struct flood_seen_entry *entry,
                                             const struct route_discovery_fields *fields,
                                             uint32_t request_id)
{
    return entry->valid &&
           entry->flood_type == FLOOD_EPOCH_TYPE_ROUTE_SOLICIT &&
           entry->gateway_id == fields->target_id &&
           entry->gateway_epoch == fields->route_epoch &&
           entry->flood_epoch_id == fields->flood_epoch_id &&
           entry->origin_id == fields->origin_id &&
           entry->origin_request_id == request_id;
}

static struct flood_seen_entry *flood_seen_find_route_solicit(
    struct mesh_relay *relay,
    const struct route_discovery_fields *fields,
    uint32_t request_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        if (flood_seen_matches_route_solicit(&relay->flood_seen[i], fields, request_id)) {
            return &relay->flood_seen[i];
        }
    }
    return NULL;
}

static struct flood_seen_entry *flood_seen_alloc_or_replace(struct mesh_relay *relay,
                                                           uint32_t now_ms)
{
    struct flood_seen_entry *entry;

    flood_seen_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        if (!relay->flood_seen[i].valid) {
            return &relay->flood_seen[i];
        }
    }

    entry = &relay->flood_seen[relay->flood_seen_next];
    relay->flood_seen_next = (uint8_t)((relay->flood_seen_next + 1u) %
                                       MESH_RELAY_FLOOD_SEEN_SIZE);
    return entry;
}

static bool flood_metric_is_better(uint16_t candidate_metric, uint16_t current_metric)
{
    uint32_t candidate = candidate_metric;
    uint32_t current = current_metric;

    return candidate * 100u <
           current * (100u - FLOOD_BETTER_METRIC_MARGIN_PERCENT);
}

static uint16_t route_solicit_metric(const struct route_discovery_fields *fields)
{
    return route_candidate_cost(fields->hop_count, fields->quality);
}

static struct flood_seen_entry *route_solicit_flood_note(
    struct mesh_relay *relay,
    const struct route_discovery_fields *fields,
    uint32_t request_id,
    uint64_t previous_hop_id,
    uint32_t now_ms,
    bool *first_seen)
{
    struct flood_seen_entry *entry;
    uint16_t metric;

    if (first_seen != NULL) {
        *first_seen = false;
    }

    flood_seen_expire_stale(relay, now_ms);
    entry = flood_seen_find_route_solicit(relay, fields, request_id);
    metric = route_solicit_metric(fields);
    if (entry == NULL) {
        entry = flood_seen_alloc_or_replace(relay, now_ms);
        memset(entry, 0, sizeof(*entry));
        entry->gateway_id = fields->target_id;
        entry->gateway_epoch = fields->route_epoch;
        entry->flood_epoch_id = fields->flood_epoch_id;
        entry->flood_type = FLOOD_EPOCH_TYPE_ROUTE_SOLICIT;
        entry->origin_id = fields->origin_id;
        entry->origin_request_id = request_id;
        entry->best_hop_count = fields->hop_count;
        entry->best_metric = metric;
        entry->best_previous_hop = previous_hop_id;
        entry->expires_at_ms = now_ms + flood_seen_window_ms(entry->flood_type);
        entry->valid = true;
        if (first_seen != NULL) {
            *first_seen = true;
        }
    } else if (previous_hop_id != entry->best_previous_hop &&
               flood_metric_is_better(metric, entry->best_metric)) {
        if (id_is_unicast(entry->best_previous_hop)) {
            entry->backup_previous_hop = entry->best_previous_hop;
        }
        entry->best_previous_hop = previous_hop_id;
        entry->best_hop_count = fields->hop_count;
        entry->best_metric = metric;
    } else if (previous_hop_id != entry->best_previous_hop &&
               previous_hop_id != entry->backup_previous_hop &&
               id_is_unicast(previous_hop_id)) {
        entry->backup_previous_hop = previous_hop_id;
    }

    if (entry->heard_count < UINT8_MAX) {
        entry->heard_count++;
    }
    return entry;
}

static bool route_solicit_forward_allowed(const struct flood_seen_entry *entry)
{
    return entry != NULL &&
           entry->forward_count < FLOOD_FORWARD_MAX_NORMAL &&
           (entry->forward_count == 0u ||
            entry->heard_count < FLOOD_FORWARD_SUPPRESS_AFTER_HEARD);
}

static int build_route_request_forward(const struct proto_packet *packet,
                                       const struct route_discovery_fields *fields,
                                       uint32_t now_ms,
                                       uint32_t random_value,
                                       struct mesh_outbound *out)
{
    struct route_discovery_fields forwarded = *fields;
    struct flood_control_fields flood_control;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl <= 1u || fields->hop_count == UINT8_MAX) {
        return PROTO_ERR_STALE;
    }

    forwarded.hop_count++;
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      &forwarded);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->flood_retry_count = 0u;
    flood_control.random_backoff_max_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS;
    flood_control.random_backoff_slot_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS;
    flood_control.retry_count = 0u;
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(packet->src_id,
                                                                forwarded.slot_seed,
                                                                forwarded.hop_count,
                                                                &flood_control,
                                                                random_value);
    return PROTO_OK;
}

static int ack_payload_contains_seq(const uint8_t *payload,
                                    size_t payload_len,
                                    uint16_t requested_seq,
                                    bool *contains)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (contains == NULL) {
        return PROTO_ERR_ARG;
    }
    *contains = false;

    ret = tlv_find(payload, payload_len, TLV_REQUESTED_MSG_SEQ, &value, &value_len);
    if (ret == PROTO_OK) {
        if (value_len != sizeof(uint16_t)) {
            return PROTO_ERR_MALFORMED;
        }
        if (proto_get_u16_le(value) == requested_seq) {
            *contains = true;
            return PROTO_OK;
        }
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }

    ret = tlv_find(payload, payload_len, TLV_MESH_ACK_SEQ_LIST, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((value_len % sizeof(uint16_t)) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t offset = 0u; offset < value_len; offset += sizeof(uint16_t)) {
        if (proto_get_u16_le(&value[offset]) == requested_seq) {
            *contains = true;
            return PROTO_OK;
        }
    }
    return PROTO_OK;
}

static bool command_result_id_matches(const struct command_result_id *a,
                                      const struct command_result_id *b)
{
    return a != NULL && b != NULL &&
           a->gateway_id == b->gateway_id &&
           a->gateway_epoch == b->gateway_epoch &&
           a->command_seq == b->command_seq &&
           a->node_id == b->node_id &&
           a->node_boot_counter == b->node_boot_counter &&
           a->result_seq == b->result_seq;
}

static void result_offer_reservation_clear(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->result_offer_reservation, 0,
               sizeof(relay->result_offer_reservation));
    }
}

static bool result_offer_reservation_matches_offer(
    const struct mesh_relay *relay,
    uint64_t child_id,
    const struct result_offer *offer)
{
    const struct mesh_result_offer_reservation *reservation;

    if (relay == NULL || offer == NULL) {
        return false;
    }
    reservation = &relay->result_offer_reservation;
    return reservation->valid &&
           reservation->child_id == child_id &&
           reservation->result_len == offer->result_len &&
           reservation->result_crc == offer->result_crc &&
           command_result_id_matches(&reservation->result_id, &offer->result_id);
}

static int result_offer_reserve(struct mesh_relay *relay,
                                uint64_t child_id,
                                const struct result_offer *offer)
{
    struct mesh_result_offer_reservation *reservation;

    if (relay == NULL || offer == NULL || !id_is_unicast(child_id)) {
        return PROTO_ERR_ARG;
    }
    if (relay->result_offer_reservation.valid) {
        return result_offer_reservation_matches_offer(relay, child_id, offer) ?
               PROTO_OK :
               PROTO_ERR_BUSY;
    }

    reservation = &relay->result_offer_reservation;
    memset(reservation, 0, sizeof(*reservation));
    reservation->valid = true;
    reservation->child_id = child_id;
    reservation->result_id = offer->result_id;
    reservation->result_len = offer->result_len;
    reservation->result_crc = offer->result_crc;
    return PROTO_OK;
}

static int result_offer_reservation_matches_payload(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const struct mesh_result_offer_reservation *reservation;
    struct command_result_id result_id;
    uint16_t payload_crc;
    int ret;

    if (relay == NULL || packet == NULL) {
        return PROTO_ERR_ARG;
    }
    reservation = &relay->result_offer_reservation;
    if (!reservation->valid || reservation->child_id != packet->src_id) {
        return PROTO_OK;
    }
    if (payload == NULL) {
        return PROTO_ERR_ARG;
    }
    if (payload_len != reservation->result_len) {
        return PROTO_ERR_MALFORMED;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, &result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    if (!command_result_id_matches(&result_id, &reservation->result_id) ||
        payload_crc != reservation->result_crc) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int build_result_offer_from_pending(const struct mesh_relay *relay,
                                           const struct mesh_pending_tx *pending,
                                           uint32_t now_ms,
                                           struct mesh_outbound *out)
{
    struct result_offer offer;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || pending == NULL || out == NULL ||
        pending->packet.msg_type != MSG_COMMAND_RESULT ||
        pending->payload_len == 0u ||
        pending->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        !id_is_unicast(pending->next_hop_id)) {
        return PROTO_ERR_ARG;
    }

    memset(&offer, 0, sizeof(offer));
    ret = command_result_id_from_tlvs(pending->payload, pending->payload_len, &offer.result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (offer.result_id.gateway_id != relay->gateway_id ||
        offer.result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        offer.result_id.node_id != pending->packet.src_id) {
        return PROTO_ERR_MALFORMED;
    }

    offer.result_len = pending->payload_len;
    offer.result_crc = proto_crc16_ccitt_false(pending->payload, pending->payload_len);
    offer.priority = 0u;

    memset(out, 0, sizeof(*out));
    ret = result_offer_append_tlvs(out->payload, sizeof(out->payload), &payload_len, &offer);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_RESULT_OFFER;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = pending->next_hop_id;
    out->packet.session_id = pending->packet.session_id;
    out->packet.seq = pending->packet.seq;
    out->packet.ttl = 1u;
    out->packet.message_age_ms = packet_age_add(pending->packet.message_age_ms,
                                                now_ms - pending->queued_at_ms);
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = pending->next_hop_id;
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    return PROTO_OK;
}

static int outbound_from_pending(const struct mesh_relay *relay,
                                 const struct mesh_pending_tx *pending,
                                 uint32_t now_ms,
                                 struct mesh_outbound *out)
{
    if (pending->result_offer_active) {
        return build_result_offer_from_pending(relay, pending, now_ms, out);
    }

    memset(out, 0, sizeof(*out));
    out->packet = pending->packet;
    out->packet.message_age_ms = packet_age_add(pending->packet.message_age_ms,
                                                now_ms - pending->queued_at_ms);
    if (pending->payload_len > 0u) {
        memcpy(out->payload, pending->payload, pending->payload_len);
    }
    out->payload_len = pending->payload_len;
    out->radio_channel = pending->radio_channel;
    out->next_hop_id = pending->next_hop_id;
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    return PROTO_OK;
}

static void pending_refresh_age(struct mesh_pending_tx *pending, uint32_t now_ms)
{
    pending->packet.message_age_ms = packet_age_add(pending->packet.message_age_ms,
                                                    now_ms - pending->queued_at_ms);
    pending->queued_at_ms = now_ms;
}

static void pending_set_deadlines(struct mesh_pending_tx *pending, uint32_t now_ms)
{
    pending->gateway_ack_deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void result_bundle_clear(struct mesh_result_bundle_queue *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

static int collection_epoch_id_from_payload(const uint8_t *payload,
                                            size_t payload_len,
                                            uint32_t *collection_epoch_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (collection_epoch_id == NULL) {
        return PROTO_ERR_ARG;
    }
    *collection_epoch_id = 0u;
    ret = tlv_find(payload, payload_len, TLV_COLLECTION_EPOCH_ID, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *collection_epoch_id = proto_get_u32_le(value);
    return *collection_epoch_id == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

static uint32_t outbox_packet_id_for(const struct proto_packet *packet)
{
    uint8_t identity[sizeof(uint32_t) + sizeof(uint16_t)];
    uint16_t crc;

    proto_put_u32_le(&identity[0], packet->session_id);
    proto_put_u16_le(&identity[sizeof(uint32_t)], packet->seq);
    crc = proto_crc16_ccitt_false(identity, sizeof(identity));
    return ((uint32_t)crc << 16) | packet->seq;
}

static uint32_t outbox_age_from_pending(const struct mesh_pending_tx *pending,
                                        uint32_t now_ms)
{
    return packet_age_add(pending->packet.message_age_ms, now_ms - pending->queued_at_ms);
}

static uint8_t outbox_selected_parent_index(const struct mesh_relay *relay,
                                            uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->route_epoch == relay->upstream.current_epoch &&
            candidate->next_hop_id == next_hop_id) {
            return i;
        }
    }
    return UINT8_MAX;
}

static uint32_t outbox_expiry_s_for(const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len)
{
    uint32_t expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S;
    uint32_t requested_s = 0u;

    if (packet->msg_type != MSG_COMMAND_RESULT) {
        return expiry_s;
    }
    if (find_u32_tlv(payload, payload_len, TLV_COMMAND_EXPIRY_S, &requested_s) == PROTO_OK &&
        requested_s != 0u &&
        requested_s < expiry_s) {
        expiry_s = requested_s;
    }
    return expiry_s;
}

static enum mesh_relay_delivery_state outbox_delivery_state_for(
    const struct mesh_pending_tx *pending)
{
    uint32_t collection_epoch_id = 0u;

    if (pending->packet.msg_type == MSG_COMMAND_RESULT && pending->result_offer_active) {
        return MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK;
    }
    if (pending->packet.msg_type == MSG_COMMAND_RESULT &&
        collection_epoch_id_from_payload(pending->payload,
                                         pending->payload_len,
                                         &collection_epoch_id) == PROTO_OK) {
        return MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    }
    return MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK;
}

static bool pending_has_valid_command_result_id(const struct mesh_relay *relay,
                                                const struct mesh_pending_tx *pending)
{
    struct command_result_id result_id;

    return relay != NULL &&
           pending != NULL &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           pending->packet.dst_id == relay->gateway_id &&
           (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           command_result_id_from_tlvs(pending->payload,
                                       pending->payload_len,
                                       &result_id) == PROTO_OK &&
           result_id.gateway_id == relay->gateway_id &&
           result_id.gateway_epoch == (uint16_t)relay->upstream.current_epoch &&
           result_id.node_id == pending->packet.src_id;
}

static bool outbox_should_track_pending(const struct mesh_relay *relay,
                                        const struct mesh_pending_tx *pending)
{
    return relay != NULL &&
           pending != NULL &&
           pending->packet.dst_id == relay->gateway_id &&
           (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           (pending->packet.src_id == relay->local_id ||
            pending_has_valid_command_result_id(relay, pending));
}

static bool pending_is_local_collection_result(const struct mesh_relay *relay,
                                               const struct mesh_pending_tx *pending)
{
    uint32_t collection_epoch_id = 0u;

    return outbox_should_track_pending(relay, pending) &&
           pending->packet.src_id == relay->local_id &&
           pending->state != MESH_RELAY_TX_IDLE &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           collection_epoch_id_from_payload(pending->payload,
                                            pending->payload_len,
                                            &collection_epoch_id) == PROTO_OK;
}

static bool pending_is_forwarded_command_result(const struct mesh_relay *relay,
                                                const struct mesh_pending_tx *pending)
{
    return outbox_should_track_pending(relay, pending) &&
           pending->packet.src_id != relay->local_id &&
           pending->state != MESH_RELAY_TX_IDLE &&
           pending->packet.msg_type == MSG_COMMAND_RESULT &&
           pending_has_valid_command_result_id(relay, pending);
}

static bool pending_is_result_bundle(const struct mesh_relay *relay,
                                     const struct mesh_pending_tx *pending)
{
    struct result_bundle_header bundle;
    uint8_t header[64];
    size_t header_len = 0u;

    if (!outbox_should_track_pending(relay, pending) ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->packet.msg_type != MSG_RESULT_BUNDLE ||
        result_bundle_header_from_tlvs(pending->payload,
                                       pending->payload_len,
                                       &bundle) != PROTO_OK ||
        bundle.gateway_id != relay->gateway_id ||
        bundle.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        bundle.command_seq == 0u ||
        bundle.collection_epoch_id == 0u ||
        bundle.record_count == 0u ||
        result_bundle_header_append_tlvs(header,
                                         sizeof(header),
                                         &header_len,
                                         &bundle) != PROTO_OK ||
        header_len >= pending->payload_len) {
        return false;
    }
    return bundle.bundle_crc == proto_crc16_ccitt_false(
        &pending->payload[header_len],
        pending->payload_len - header_len);
}

static void outbox_record_clear(struct mesh_relay *relay)
{
    memset(&relay->outbox_record, 0, sizeof(relay->outbox_record));
}

static void outbox_record_sync_age_from_pending(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    if (relay->outbox_record.valid && mesh_relay_tx_active(relay)) {
        relay->outbox_record.age_ms_saturating =
            outbox_age_from_pending(&relay->pending, now_ms);
    }
}

static void outbox_record_track_pending(struct mesh_relay *relay,
                                        uint32_t now_ms)
{
    struct persistent_outbox_record *record;
    const struct mesh_pending_tx *pending;

    if (relay == NULL) {
        return;
    }
    if (!mesh_relay_tx_active(relay)) {
        outbox_record_clear(relay);
        return;
    }

    pending = &relay->pending;
    if (!outbox_should_track_pending(relay, pending)) {
        outbox_record_clear(relay);
        return;
    }

    record = &relay->outbox_record;
    memset(record, 0, sizeof(*record));
    record->valid = true;
    record->delivery_state = outbox_delivery_state_for(pending);
    record->packet_id = outbox_packet_id_for(&pending->packet);
    record->session_id = pending->packet.session_id;
    record->seq = pending->packet.seq;
    record->gateway_id = relay->gateway_id;
    record->packet_class = pending->packet.msg_type;
    record->created_uptime_ms = now_ms;
    record->age_ms_saturating = pending->packet.message_age_ms;
    record->selected_parent_index = outbox_selected_parent_index(relay, pending->next_hop_id);
    record->expiry_s = outbox_expiry_s_for(&pending->packet,
                                           pending->payload,
                                           pending->payload_len);
    record->payload_crc = proto_crc16_ccitt_false(pending->payload, pending->payload_len);
    record->payload_len = pending->payload_len;
}

static void outbox_record_mark_gateway_acked(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_GATEWAY_ACKED;
        relay->outbox_record.gateway_acked = true;
    }
}

static void outbox_record_mark_collection_retry(struct mesh_relay *relay,
                                                const struct gateway_collection_eack *eack,
                                                uint32_t now_ms)
{
    if (!relay->outbox_record.valid || eack == NULL) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    relay->outbox_record.retry_round = eack->retry_round;
    relay->outbox_record.gateway_acked = false;
}

static void outbox_record_mark_collection_retry_round(struct mesh_relay *relay,
                                                      uint8_t retry_round,
                                                      uint32_t now_ms)
{
    if (!relay->outbox_record.valid) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    relay->outbox_record.retry_round = retry_round;
    relay->outbox_record.gateway_acked = false;
}

static void outbox_record_mark_collection_closed(struct mesh_relay *relay,
                                                 const struct gateway_collection_eack *eack,
                                                 uint32_t now_ms)
{
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_COLLECTION_CLOSED;
        relay->outbox_record.retry_round = eack != NULL ? eack->retry_round : 0u;
        relay->outbox_record.gateway_acked = false;
    }
}

static bool outbox_record_is_expired(struct mesh_relay *relay, uint32_t now_ms)
{
    uint32_t age_ms;

    if (relay == NULL ||
        !relay->outbox_record.valid ||
        relay->outbox_record.packet_class != MSG_COMMAND_RESULT ||
        relay->outbox_record.expiry_s == 0u ||
        !mesh_relay_tx_active(relay)) {
        return false;
    }

    age_ms = outbox_age_from_pending(&relay->pending, now_ms);
    return (age_ms / 1000u) >= relay->outbox_record.expiry_s;
}

static void outbox_record_mark_expired(struct mesh_relay *relay, uint32_t now_ms)
{
    if (relay == NULL) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    if (relay->outbox_record.valid ||
        relay->outbox_record.delivery_state != MESH_RELAY_DELIVERY_NONE) {
        relay->outbox_record.valid = false;
        relay->outbox_record.delivery_state = MESH_RELAY_DELIVERY_EXPIRED;
        relay->outbox_record.gateway_acked = false;
    }
}

static void outbox_record_mark_custody_accepted(struct mesh_relay *relay,
                                                uint64_t custody_parent,
                                                uint32_t now_ms)
{
    if (!relay->outbox_record.valid) {
        return;
    }
    outbox_record_sync_age_from_pending(relay, now_ms);
    relay->outbox_record.custody_accepted = true;
    relay->outbox_record.custody_parent = custody_parent;
    relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
}

static bool preserve_pending_gateway_result(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    if (relay == NULL ||
        (!pending_is_local_collection_result(relay, &relay->pending) &&
         !pending_is_forwarded_command_result(relay, &relay->pending) &&
         !pending_is_result_bundle(relay, &relay->pending))) {
        return false;
    }

    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
    if (relay->outbox_record.valid) {
        relay->outbox_record.age_ms_saturating = relay->pending.packet.message_age_ms;
        relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
        relay->outbox_record.gateway_acked = false;
    }
    return true;
}

static uint32_t collection_retry_base_for_round(uint8_t retry_round)
{
    switch (retry_round) {
    case 0u:
    case 1u:
        return COLLECTION_RETRY_ROUND_0_MS;
    case 2u:
        return COLLECTION_RETRY_ROUND_1_MS;
    case 3u:
        return COLLECTION_RETRY_ROUND_2_MS;
    case 4u:
        return COLLECTION_RETRY_ROUND_3_MS;
    default:
        return COLLECTION_RETRY_ROUND_STEADY_MS;
    }
}

static int schedule_pending_collection_retry(struct mesh_relay *relay,
                                             uint32_t now_ms,
                                             struct mesh_relay_result *result)
{
    struct command_result_id pending_id;
    uint32_t collection_epoch_id = 0u;
    uint8_t retry_round;
    uint32_t retry_base_ms;
    uint32_t retry_seed;
    int ret;

    if (relay == NULL || result == NULL ||
        !pending_is_local_collection_result(relay, &relay->pending)) {
        return PROTO_ERR_ARG;
    }

    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = collection_epoch_id_from_payload(relay->pending.payload,
                                           relay->pending.payload_len,
                                           &collection_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    retry_round = relay->outbox_record.retry_round;
    if (retry_round < UINT8_MAX) {
        retry_round++;
    }
    retry_base_ms = collection_retry_base_for_round(retry_round);
    retry_seed = collection_retry_seed(pending_id.node_id,
                                       pending_id.command_seq,
                                       collection_epoch_id,
                                       retry_round);

    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms =
        now_ms + mesh_relay_collection_retry_delay_ms(retry_base_ms, retry_seed);
    outbox_record_mark_collection_retry_round(relay, retry_round, now_ms);
    result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_RETRY;
    return PROTO_OK;
}

static bool result_bundle_key_matches(const struct mesh_result_bundle_queue *queue,
                                      const struct command_result_id *result_id,
                                      uint32_t collection_epoch_id)
{
    return queue != NULL && result_id != NULL &&
           queue->gateway_id == result_id->gateway_id &&
           queue->gateway_epoch == result_id->gateway_epoch &&
           queue->command_seq == result_id->command_seq &&
           queue->collection_epoch_id == collection_epoch_id;
}

static bool result_bundle_entry_exists(const struct mesh_result_bundle_queue *queue,
                                       const struct command_result_id *result_id)
{
    if (queue == NULL || result_id == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < queue->record_count; i++) {
        if (queue->records[i].valid &&
            command_result_id_matches(&queue->records[i].result_id, result_id)) {
            return true;
        }
    }
    return false;
}

static bool command_result_can_bundle(const struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      struct command_result_id *result_id,
                                      uint32_t *collection_epoch_id)
{
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL ||
        result_id == NULL || collection_epoch_id == NULL ||
        relay->role == MESH_RELAY_ROLE_GATEWAY ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->dst_id != relay->gateway_id ||
        packet->ttl == 0u ||
        payload_len == 0u ||
        payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return false;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, result_id);
    if (ret != PROTO_OK) {
        return false;
    }
    if (result_id->gateway_id != relay->gateway_id ||
        result_id->gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id->node_id != packet->src_id) {
        return false;
    }

    ret = collection_epoch_id_from_payload(payload, payload_len, collection_epoch_id);
    return ret == PROTO_OK;
}

static bool result_bundle_ready_to_flush(const struct mesh_result_bundle_queue *queue,
                                         uint32_t now_ms)
{
    if (queue == NULL || !queue->active || queue->record_count == 0u) {
        return false;
    }
    return queue->record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS ||
           deadline_reached(now_ms, queue->due_ms);
}

static int result_bundle_enqueue_command_result(struct mesh_relay *relay,
                                                const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                const struct command_result_id *result_id,
                                                uint32_t collection_epoch_id,
                                                uint32_t now_ms)
{
    struct mesh_result_bundle_queue *queue;
    struct mesh_result_bundle_entry *entry;
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || result_id == NULL ||
        payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }

    ret = mesh_relay_select_next_hop(relay, relay->gateway_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    queue = &relay->result_bundle;
    if (!queue->active) {
        memset(queue, 0, sizeof(*queue));
        queue->active = true;
        queue->gateway_id = result_id->gateway_id;
        queue->gateway_epoch = result_id->gateway_epoch;
        queue->command_seq = result_id->command_seq;
        queue->collection_epoch_id = collection_epoch_id;
        queue->due_ms = now_ms + MESH_RELAY_RESULT_BUNDLE_HOLD_MS;
    } else if (!result_bundle_key_matches(queue, result_id, collection_epoch_id)) {
        return PROTO_ERR_BUSY;
    }

    if (result_bundle_entry_exists(queue, result_id)) {
        return PROTO_ERR_STALE;
    }
    if (queue->record_count >= MESH_RELAY_RESULT_BUNDLE_RECORDS) {
        return PROTO_ERR_BUSY;
    }

    entry = &queue->records[queue->record_count];
    memset(entry, 0, sizeof(*entry));
    entry->result_id = *result_id;
    entry->payload_len = (uint16_t)payload_len;
    entry->payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    entry->message_age_ms = packet->message_age_ms;
    entry->queued_at_ms = now_ms;
    memcpy(entry->payload, payload, payload_len);
    entry->valid = true;
    queue->record_count++;
    (void)next_hop_id;
    return PROTO_OK;
}

static int result_bundle_build_outbound(struct mesh_relay *relay,
                                        uint32_t now_ms,
                                        struct mesh_outbound *out)
{
    struct mesh_result_bundle_queue *queue;
    struct result_bundle_header bundle;
    uint8_t header[64];
    size_t header_len = 0u;
    size_t records_len = 0u;
    size_t payload_len;
    uint64_t next_hop_id = 0u;
    uint32_t max_age_ms = 0u;
    uint16_t seq;
    int ret;

    if (relay == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    queue = &relay->result_bundle;
    if (!queue->active || queue->record_count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    ret = mesh_relay_select_next_hop(relay, relay->gateway_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0u; i < queue->record_count; i++) {
        struct mesh_result_bundle_entry *entry = &queue->records[i];
        struct result_bundle_record record;
        uint32_t age_ms;

        if (!entry->valid) {
            return PROTO_ERR_MALFORMED;
        }
        memset(&record, 0, sizeof(record));
        record.result_id = entry->result_id;
        record.payload_len = entry->payload_len;
        record.payload_crc = entry->payload_crc;
        record.payload = entry->payload;
        ret = result_bundle_record_append_tlv(out->payload,
                                              sizeof(out->payload),
                                              &records_len,
                                              &record);
        if (ret != PROTO_OK) {
            return ret;
        }
        age_ms = packet_age_add(entry->message_age_ms, now_ms - entry->queued_at_ms);
        if (age_ms > max_age_ms) {
            max_age_ms = age_ms;
        }
    }

    seq = relay_next_seq(relay);
    memset(&bundle, 0, sizeof(bundle));
    bundle.gateway_id = queue->gateway_id;
    bundle.gateway_epoch = queue->gateway_epoch;
    bundle.command_seq = queue->command_seq;
    bundle.collection_epoch_id = queue->collection_epoch_id;
    bundle.bundle_id = seq;
    bundle.record_count = queue->record_count;
    bundle.bundle_crc = proto_crc16_ccitt_false(out->payload, records_len);

    ret = result_bundle_header_append_tlvs(header,
                                           sizeof(header),
                                           &header_len,
                                           &bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (header_len + records_len > sizeof(out->payload)) {
        return PROTO_ERR_NO_SPACE;
    }

    memmove(&out->payload[header_len], out->payload, records_len);
    memcpy(out->payload, header, header_len);
    payload_len = header_len + records_len;

    out->packet.msg_type = MSG_RESULT_BUNDLE;
    out->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = relay->gateway_id;
    out->packet.session_id = queue->command_seq;
    out->packet.seq = seq;
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->packet.message_age_ms = max_age_ms;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = next_hop_id;
    out->queued_at_ms = now_ms;

    return PROTO_OK;
}

static bool pending_ack_matches(const struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                int *status)
{
    const struct mesh_pending_tx *pending = &relay->pending;
    bool contains = false;
    int ret;

    if (status != NULL) {
        *status = PROTO_OK;
    }
    if (pending->state == MESH_RELAY_TX_IDLE ||
        packet->dst_id != relay->local_id ||
        packet->session_id != pending->packet.session_id) {
        return false;
    }

    ret = ack_payload_contains_seq(payload, payload_len, pending->packet.seq, &contains);
    if (ret != PROTO_OK) {
        if (status != NULL) {
            *status = ret;
        }
        return false;
    }
    return contains;
}

static void refresh_downlink(struct mesh_relay *relay,
                             uint64_t target_id,
                             uint64_t next_hop_id,
                             uint32_t now_ms)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid &&
            entry->target_id == target_id &&
            entry->next_hop_id == next_hop_id) {
            entry->last_seen_ms = now_ms;
            return;
        }
    }
}

static int build_gateway_ack(struct mesh_relay *relay,
                             const struct proto_packet *packet,
                             uint64_t previous_hop_id,
                             struct mesh_outbound *out)
{
    uint64_t next_hop_id = previous_hop_id;
    size_t payload_len = 0u;
    int ret;

    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_init_gateway_ack(&out->packet,
                                relay->local_id,
                                packet->src_id,
                                packet->session_id,
                                relay_next_seq(relay),
                                (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (mesh_relay_select_next_hop(relay, packet->src_id, &next_hop_id) != PROTO_OK) {
        next_hop_id = previous_hop_id;
    }
    if (!id_is_unicast(next_hop_id)) {
        return PROTO_ERR_NOT_FOUND;
    }

    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int build_hop_ack(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         uint64_t previous_hop_id,
                         struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->src_id == relay->local_id ||
        packet->dst_id == relay->local_id ||
        packet->dst_id == MESH_BROADCAST_ID) {
        return PROTO_ERR_STALE;
    }

    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_MESH_HOP_ACK;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = previous_hop_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_GATEWAY_ACK_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int parse_relay_busy_tlvs(const uint8_t *payload,
                                 size_t payload_len,
                                 struct relay_busy_fields *fields)
{
    int ret;

    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_REQUESTED_MSG_SESSION_ID,
                       &fields->requested_session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REQUESTED_MSG_SEQ, &fields->requested_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_RETRY_AFTER_MS, &fields->retry_after_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_RELAY_CAPACITY_STATE, &fields->capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->capacity_state > RELAY_CAP_BLACK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = fields->retry_after_ms;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload,
                       payload_len,
                       TLV_ALTERNATE_PARENT_ID,
                       &fields->alternate_parent_id);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->has_alternate_parent = false;
        fields->alternate_parent_id = 0u;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    fields->has_alternate_parent = id_is_unicast(fields->alternate_parent_id);
    return fields->has_alternate_parent ? PROTO_OK : PROTO_ERR_MALFORMED;
}

static int build_busy_response(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t incoming_payload_len,
                               uint64_t previous_hop_id,
                               uint8_t msg_type,
                               struct mesh_outbound *out)
{
    const struct route_candidate *alternate;
    size_t payload_len = 0u;
    uint16_t retry_after_ms;
    uint8_t capacity_state;
    uint64_t alternate_parent_id = 0u;
    bool has_alternate_parent = false;
    bool appended_busy_fields = false;
    int ret;

    if (relay == NULL ||
        packet == NULL ||
        out == NULL ||
        (msg_type != MSG_RELAY_BUSY && msg_type != MSG_RESULT_BUSY) ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    retry_after_ms = relay_busy_retry_after_ms(relay);
    capacity_state = relay_current_capacity_state(relay);

    ret = tlv_append_u32(out->payload,
                         sizeof(out->payload),
                         &payload_len,
                         TLV_REQUESTED_MSG_SESSION_ID,
                         packet->session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    alternate = route_selected(&relay->upstream);
    if (alternate != NULL &&
        alternate->next_hop_id != previous_hop_id &&
        alternate->next_hop_id != packet->src_id) {
        alternate_parent_id = alternate->next_hop_id;
        has_alternate_parent = true;
    }

    if (msg_type == MSG_RESULT_BUSY && payload != NULL && incoming_payload_len > 0u) {
        struct command_result_id result_id;
        ret = command_result_id_from_tlvs(payload, incoming_payload_len, &result_id);
        if (ret == PROTO_OK) {
            const struct result_busy busy = {
                .result_id = result_id,
                .retry_after_ms = retry_after_ms,
                .capacity_state = capacity_state,
                .capacity_validity_interval_ms = retry_after_ms,
                .optional_alternate_parent = alternate_parent_id,
                .has_optional_alternate_parent = has_alternate_parent,
            };

            ret = result_busy_append_tlvs(out->payload,
                                          sizeof(out->payload),
                                          &payload_len,
                                          &busy);
            if (ret != PROTO_OK) {
                return ret;
            }
            appended_busy_fields = true;
        }
    }

    if (!appended_busy_fields) {
        ret = tlv_append_u16(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_RETRY_AFTER_MS,
                             retry_after_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(out->payload,
                            sizeof(out->payload),
                            &payload_len,
                            TLV_RELAY_CAPACITY_STATE,
                            capacity_state);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u16(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                             retry_after_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (has_alternate_parent) {
            ret = tlv_append_u64(out->payload,
                                 sizeof(out->payload),
                                 &payload_len,
                                 TLV_ALTERNATE_PARENT_ID,
                                 alternate_parent_id);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
    }

    out->packet.msg_type = msg_type;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static void add_busy_action(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint64_t previous_hop_id,
                            struct mesh_relay_result *result)
{
    uint8_t busy_msg_type = packet->msg_type == MSG_COMMAND_RESULT ?
                            MSG_RESULT_BUSY :
                            MSG_RELAY_BUSY;
    struct mesh_outbound *out = &result->busy;

    if (build_busy_response(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            busy_msg_type,
                            out) != PROTO_OK) {
        return;
    }
    result->actions |= busy_msg_type == MSG_RESULT_BUSY ?
                       MESH_RELAY_ACTION_SEND_RESULT_BUSY :
                       MESH_RELAY_ACTION_SEND_RELAY_BUSY;
    relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
}

static int build_result_offer_busy_response(struct mesh_relay *relay,
                                            const struct proto_packet *packet,
                                            const struct result_offer *offer,
                                            uint64_t previous_hop_id,
                                            struct mesh_outbound *out)
{
    const struct route_candidate *alternate;
    struct result_busy busy;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || packet == NULL || offer == NULL || out == NULL ||
        !id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    memset(&busy, 0, sizeof(busy));
    busy.result_id = offer->result_id;
    busy.retry_after_ms = relay_busy_retry_after_ms(relay);
    busy.capacity_state = relay_current_capacity_state(relay);
    busy.capacity_validity_interval_ms = busy.retry_after_ms;

    ret = tlv_append_u32(out->payload,
                         sizeof(out->payload),
                         &payload_len,
                         TLV_REQUESTED_MSG_SESSION_ID,
                         packet->session_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }

    alternate = route_selected(&relay->upstream);
    if (alternate != NULL &&
        alternate->next_hop_id != previous_hop_id &&
        alternate->next_hop_id != packet->src_id) {
        busy.optional_alternate_parent = alternate->next_hop_id;
        busy.has_optional_alternate_parent = true;
    }

    ret = result_busy_append_tlvs(out->payload, sizeof(out->payload), &payload_len, &busy);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_RESULT_BUSY;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int build_result_grant_response(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const struct result_offer *offer,
                                       uint64_t previous_hop_id,
                                       struct mesh_outbound *out)
{
    struct result_grant grant;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || packet == NULL || offer == NULL || out == NULL ||
        !id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (offer->result_len == 0u || offer->result_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    memset(&grant, 0, sizeof(grant));
    grant.result_id = offer->result_id;
    grant.granted_channel = UWB_CHANNEL_MESH_PAYLOAD;
    grant.max_bytes = offer->result_len;
    grant.event_offset_hint = 0u;

    ret = result_grant_append_tlvs(out->payload, sizeof(out->payload), &payload_len, &grant);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_RESULT_GRANT;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = packet->src_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int handle_result_offer(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint64_t previous_hop_id,
                               struct mesh_relay_result *result)
{
    struct result_offer offer;
    int ret;

    if (packet->dst_id != relay->local_id || packet->msg_type != MSG_RESULT_OFFER) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) || previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = result_offer_from_tlvs(payload, payload_len, &offer);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (offer.result_id.gateway_id != relay->gateway_id ||
        offer.result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        offer.result_len == 0u ||
        offer.result_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    if (mesh_relay_tx_active(relay) ||
        (relay_current_capacity_state(relay) >= RELAY_CAP_RED &&
         !result_offer_reservation_matches_offer(relay, previous_hop_id, &offer))) {
        ret = build_result_offer_busy_response(relay,
                                               packet,
                                               &offer,
                                               previous_hop_id,
                                               &result->busy);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                               MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
        }
        return ret;
    }

    ret = result_offer_reserve(relay, previous_hop_id, &offer);
    if (ret == PROTO_ERR_BUSY) {
        ret = build_result_offer_busy_response(relay,
                                               packet,
                                               &offer,
                                               previous_hop_id,
                                               &result->busy);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                               MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            relay_diag_inc_u8(&relay->diagnostics.busy_response_count);
        }
        return ret;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = build_result_grant_response(relay,
                                      packet,
                                      &offer,
                                      previous_hop_id,
                                      &result->result_grant);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_RESULT_GRANT;
    }
    return ret;
}

static int handle_result_grant(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint64_t previous_hop_id,
                               uint32_t now_ms,
                               struct mesh_relay_result *result)
{
    struct result_grant grant;
    struct command_result_id pending_id;
    int ret;

    if (packet->dst_id != relay->local_id || packet->msg_type != MSG_RESULT_GRANT) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->pending.state != MESH_RELAY_TX_WAIT_RESULT_GRANT ||
        !relay->pending.result_offer_active ||
        relay->pending.next_hop_id != previous_hop_id) {
        return PROTO_OK;
    }

    ret = result_grant_from_tlvs(payload, payload_len, &grant);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!command_result_id_matches(&grant.result_id, &pending_id) ||
        grant.granted_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        grant.max_bytes == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->pending.payload_len > grant.max_bytes) {
        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
        result->status = PROTO_ERR_NO_SPACE;
        return PROTO_OK;
    }

    pending_refresh_age(&relay->pending, now_ms);
    relay->pending.result_offer_active = false;
    relay->pending.radio_channel = grant.granted_channel;
    relay->pending.next_hop_id = previous_hop_id;
    relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    pending_set_deadlines(&relay->pending, now_ms);
    outbox_record_mark_custody_accepted(relay, previous_hop_id, now_ms);
    ret = outbound_from_pending(relay, &relay->pending, now_ms, &result->retransmit);
    if (ret != PROTO_OK) {
        return ret;
    }
    result->actions |= MESH_RELAY_ACTION_RETRANSMIT;
    return PROTO_OK;
}

static int handle_local_busy(struct mesh_relay *relay,
                             const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len,
                             uint64_t previous_hop_id,
                             uint32_t now_ms,
                             struct mesh_relay_result *result)
{
    struct relay_busy_fields fields = {0};
    uint32_t retry_after_ms;
    int ret;

    if (packet->dst_id != relay->local_id ||
        (packet->msg_type != MSG_RELAY_BUSY && packet->msg_type != MSG_RESULT_BUSY)) {
        return PROTO_OK;
    }
    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_relay_busy_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (relay->pending.state == MESH_RELAY_TX_IDLE ||
        fields.requested_session_id != relay->pending.packet.session_id ||
        fields.requested_seq != relay->pending.packet.seq) {
        return PROTO_OK;
    }
    if (packet->msg_type == MSG_RESULT_BUSY &&
        relay->pending.packet.msg_type == MSG_COMMAND_RESULT &&
        relay->pending.result_offer_active) {
        struct result_busy busy;
        struct command_result_id pending_id;

        ret = result_busy_from_tlvs(payload, payload_len, &busy);
        if (ret != PROTO_OK) {
            return PROTO_OK;
        }
        ret = command_result_id_from_tlvs(relay->pending.payload,
                                          relay->pending.payload_len,
                                          &pending_id);
        if (ret != PROTO_OK ||
            !command_result_id_matches(&busy.result_id, &pending_id)) {
            return PROTO_OK;
        }
    }

    retry_after_ms = fields.retry_after_ms;
    if (retry_after_ms < RELAY_BUSY_RETRY_MIN_MS) {
        retry_after_ms = RELAY_BUSY_RETRY_MIN_MS;
    } else if (retry_after_ms > RELAY_BUSY_RETRY_MAX_MS) {
        retry_after_ms = RELAY_BUSY_RETRY_MAX_MS;
    }
    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    route_update_capacity_hint(&relay->upstream,
                               previous_hop_id,
                               relay->gateway_id,
                               fields.capacity_state,
                               0u,
                               0u,
                               now_ms,
                               capacity_valid_until_ms(
                                   now_ms,
                                   fields.capacity_validity_interval_ms),
                               now_ms);
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms = now_ms + retry_after_ms;
    result->actions |= MESH_RELAY_ACTION_TX_RELAY_BUSY;
    return PROTO_OK;
}

static int handle_local_ack(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint32_t now_ms,
                            struct mesh_relay_result *result)
{
    int ret;

    if (packet->dst_id != relay->local_id ||
        (packet->msg_type != MSG_GATEWAY_ACK && packet->msg_type != MSG_MESH_HOP_ACK)) {
        return PROTO_OK;
    }

    if (!pending_ack_matches(relay, packet, payload, payload_len, &ret)) {
        if (ret != PROTO_OK) {
            result->status = ret;
            return ret;
        }
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }

    if (packet->msg_type == MSG_GATEWAY_ACK &&
        (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
         relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) &&
        packet->src_id == relay->gateway_id) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
    } else if (packet->msg_type == MSG_MESH_HOP_ACK &&
               (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
                relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF)) {
        pending_set_deadlines(&relay->pending, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
        route_refresh_selected_at(&relay->upstream, now_ms);
        result->actions |= MESH_RELAY_ACTION_TX_HOP_PROGRESS;
    }

    return PROTO_OK;
}

static int build_forward(const struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (packet->ttl <= 1u) {
        return PROTO_ERR_STALE;
    }
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static bool packet_needs_forward(const struct mesh_relay *relay, const struct proto_packet *packet)
{
    return packet->dst_id != relay->local_id && packet->dst_id != MESH_BROADCAST_ID;
}

static bool packet_requires_channel9_payload_event(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_GATEWAY_COLLECTION_EACK:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static bool tlv_u32_nonzero(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK &&
           value_len == sizeof(uint32_t) &&
           proto_get_u32_le(value) != 0u;
}

static bool tlv_u16_nonzero(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK &&
           value_len == sizeof(uint16_t) &&
           proto_get_u16_le(value) != 0u;
}

static bool command_flood_broadcast_valid(const uint8_t *payload, size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t scope;
    uint8_t response_mode = CMD_RESPONSE_SMALL_RESULT;
    int ret;

    ret = tlv_find(payload, payload_len, TLV_COMMAND_SCOPE, &value, &value_len);
    if (ret != PROTO_OK || value_len != sizeof(uint8_t)) {
        return false;
    }
    scope = value[0];
    if (scope == CMD_SCOPE_SINGLE_NODE ||
        (scope != CMD_SCOPE_GROUP &&
         scope != CMD_SCOPE_ALL_REGISTERED &&
         scope != CMD_SCOPE_ALL_HEARD)) {
        return false;
    }

    ret = tlv_find(payload, payload_len, TLV_COMMAND_RESPONSE_MODE, &value, &value_len);
    if (ret == PROTO_OK) {
        if (value_len != sizeof(uint8_t)) {
            return false;
        }
        response_mode = value[0];
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return false;
    }
    if (response_mode != CMD_RESPONSE_NONE &&
        response_mode != CMD_RESPONSE_ACK_ONLY &&
        response_mode != CMD_RESPONSE_SMALL_RESULT &&
        response_mode != CMD_RESPONSE_LARGE_RESULT) {
        return false;
    }

    if (!tlv_u32_nonzero(payload, payload_len, TLV_COMMAND_SEQ) ||
        !tlv_u32_nonzero(payload, payload_len, TLV_FLOOD_EPOCH_ID)) {
        return false;
    }
    if (scope == CMD_SCOPE_ALL_REGISTERED &&
        (!tlv_u16_nonzero(payload, payload_len, TLV_MEMBERSHIP_EPOCH) ||
         !tlv_u16_nonzero(payload, payload_len, TLV_EXPECTED_NODE_COUNT))) {
        return false;
    }
    if (response_mode != CMD_RESPONSE_NONE &&
        (!tlv_u32_nonzero(payload, payload_len, TLV_COLLECTION_EPOCH_ID) ||
         !tlv_u32_nonzero(payload, payload_len, TLV_COLLECTION_SLOT_SEED))) {
        return false;
    }

    return true;
}

static bool collection_eack_broadcast_valid(const struct mesh_relay *relay,
                                            const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len)
{
    struct gateway_collection_eack eack;

    if (relay == NULL || packet == NULL ||
        packet->src_id != relay->gateway_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        gateway_collection_eack_from_tlvs(payload, payload_len, &eack) != PROTO_OK) {
        return false;
    }
    if (eack.gateway_id != relay->gateway_id ||
        eack.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        eack.command_seq == 0u ||
        eack.collection_epoch_id == 0u ||
        eack.received_count > eack.expected_count) {
        return false;
    }
    return true;
}

static int handle_collection_eack_for_pending(struct mesh_relay *relay,
                                              const uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t now_ms,
                                              struct mesh_relay_result *result)
{
    struct gateway_collection_eack eack;
    struct command_result_id pending_id;
    uint32_t collection_epoch_id = 0u;
    bool listed = false;
    int ret;

    if (relay == NULL || result == NULL || payload == NULL ||
        relay->pending.state == MESH_RELAY_TX_IDLE ||
        relay->pending.packet.msg_type != MSG_COMMAND_RESULT ||
        relay->pending.packet.dst_id != relay->gateway_id) {
        return PROTO_OK;
    }

    ret = gateway_collection_eack_from_tlvs(payload, payload_len, &eack);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &pending_id);
    if (ret != PROTO_OK) {
        return PROTO_OK;
    }
    ret = collection_epoch_id_from_payload(relay->pending.payload,
                                           relay->pending.payload_len,
                                           &collection_epoch_id);
    if (ret != PROTO_OK) {
        return PROTO_OK;
    }

    if (eack.gateway_id != pending_id.gateway_id ||
        eack.gateway_epoch != pending_id.gateway_epoch ||
        eack.command_seq != pending_id.command_seq ||
        eack.collection_epoch_id != collection_epoch_id ||
        pending_id.node_id != relay->local_id) {
        return PROTO_OK;
    }

    if (!eack.collection_open) {
        result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_CLOSED;
        outbox_record_mark_collection_closed(relay, &eack, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        return PROTO_OK;
    }

    if (eack.eack_format != EACK_FORMAT_EXPLICIT_RECEIVED_LIST &&
        eack.eack_format != EACK_FORMAT_EXPLICIT_MISSING_LIST) {
        return PROTO_OK;
    }

    ret = gateway_collection_eack_contains_node_id(payload,
                                                   payload_len,
                                                   pending_id.node_id,
                                                   &listed);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST && listed) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }
    if (eack.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST && !listed) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        mesh_relay_note_route_discovery_ready(relay, relay->pending.packet.dst_id);
        outbox_record_mark_gateway_acked(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
        return PROTO_OK;
    }

    if ((eack.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST && listed) ||
        (eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST && !listed)) {
        uint32_t retry_base_ms = eack.next_retry_spread_ms == 0u ?
                                 COLLECTION_RETRY_ROUND_0_MS :
                                 eack.next_retry_spread_ms;
        uint32_t retry_seed = collection_retry_seed(pending_id.node_id,
                                                    pending_id.command_seq,
                                                    collection_epoch_id,
                                                    eack.retry_round);

        pending_refresh_age(&relay->pending, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms =
            now_ms + mesh_relay_collection_retry_delay_ms(retry_base_ms, retry_seed);
        outbox_record_mark_collection_retry(relay, &eack, now_ms);
        result->actions |= MESH_RELAY_ACTION_TX_COLLECTION_RETRY;
    }
    return PROTO_OK;
}

static bool broadcast_packet_needs_forward(const struct mesh_relay *relay,
                                           const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    if (packet->dst_id != MESH_BROADCAST_ID || packet->ttl == 0u) {
        return false;
    }
    if (packet->msg_type == MSG_SURVEY_DISCOVERY_START) {
        return true;
    }
    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK) {
        return collection_eack_broadcast_valid(relay, packet, payload, payload_len);
    }
    return packet->msg_type == MSG_COMMAND &&
           command_flood_broadcast_valid(payload, payload_len);
}

static int build_broadcast_forward(const struct mesh_relay *relay,
                                   const struct proto_packet *packet,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint32_t now_ms,
                                   uint32_t random_value,
                                   struct mesh_outbound *out)
{
    struct flood_control_fields flood_control;
    uint32_t slot_seed;
    int ret;

    if (!broadcast_packet_needs_forward(relay, packet, payload, payload_len)) {
        return PROTO_ERR_STALE;
    }

    flood_control_defaults(&flood_control);
    if (packet->msg_type == MSG_COMMAND) {
        ret = parse_flood_control_tlvs(payload, payload_len, &flood_control);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->flood_retry_count = flood_control.retry_count;
    if (packet->msg_type == MSG_COMMAND) {
        ret = ensure_flood_control_tlvs(out,
                                        &flood_control,
                                        packet->message_age_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    slot_seed = flood_identity_seed(packet, relay->local_id);
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(
        relay->local_id,
        slot_seed,
        packet->ttl,
        packet->msg_type == MSG_COMMAND ? &flood_control : NULL,
        random_value);
    return PROTO_OK;
}

static bool local_delivery_needs_response(const struct mesh_relay *relay,
                                          const struct proto_packet *packet)
{
    if (packet->dst_id != relay->local_id) {
        return false;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY &&
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
        return true;
    }
    return relay->role == MESH_RELAY_ROLE_ANCHOR && packet->msg_type == MSG_COMMAND;
}

static int add_gateway_ack_action(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    int ret;

    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return PROTO_OK;
    }

    ret = build_gateway_ack(relay, packet, previous_hop_id, &result->gateway_ack);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ACK;
    } else if (result->status == PROTO_OK) {
        result->status = ret;
    }
    return ret;
}

static void add_hop_ack_action(struct mesh_relay *relay,
                               const struct proto_packet *packet,
                               uint64_t previous_hop_id,
                               struct mesh_relay_result *result)
{
    int ret;

    ret = build_hop_ack(relay, packet, previous_hop_id, &result->hop_ack);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_HOP_ACK;
    }
}

static void add_route_reply_backup(struct mesh_relay *relay,
                                   uint64_t origin_id,
                                   uint64_t primary_next_hop,
                                   struct mesh_relay_result *result)
{
    const struct mesh_downlink_entry *backup;

    if (relay == NULL ||
        result == NULL ||
        !id_is_unicast(origin_id) ||
        !id_is_unicast(primary_next_hop)) {
        return;
    }

    backup = downlink_backup_for(relay, origin_id, primary_next_hop);
    if (backup == NULL || !id_is_unicast(backup->next_hop_id)) {
        return;
    }

    result->route_reply_backup_next_hop_id = backup->next_hop_id;
    result->route_reply_backup_valid = true;
}

static int upsert_reactive_route(struct mesh_relay *relay,
                                 uint64_t target_id,
                                 uint64_t next_hop_id,
                                 uint32_t route_epoch,
                                 uint8_t advertised_hop_count,
                                 uint8_t quality,
                                 uint8_t relay_capacity_state,
                                 uint16_t queue_free_hint,
                                 uint8_t channel9_busy_hint,
                                 uint16_t capacity_validity_interval_ms,
                                 uint32_t now_ms)
{
    if (!id_is_unicast(target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        advertised_hop_count == UINT8_MAX ||
        quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    if (target_id == relay->local_id) {
        return PROTO_OK;
    }

    if (target_id == relay->gateway_id) {
        struct route_candidate candidate = {
            .next_hop_id = next_hop_id,
            .gateway_id = target_id,
            .route_epoch = route_epoch,
            .last_seen_ms = now_ms,
            .hop_count = advertised_hop_count,
            .link_quality = quality,
            .relay_capacity_state = relay_capacity_state,
            .queue_free_hint = queue_free_hint,
            .channel9_busy_hint = channel9_busy_hint,
            .capacity_observed_at_ms = capacity_validity_interval_ms != 0u ? now_ms : 0u,
            .capacity_valid_until_ms = capacity_valid_until_ms(
                now_ms,
                capacity_validity_interval_ms),
            .valid = true,
        };

        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_OK;
        }
        return route_upsert_candidate(&relay->upstream, &candidate);
    }

    if (advertised_hop_count + 1u == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    struct mesh_downlink_entry entry = {
        .target_id = target_id,
        .next_hop_id = next_hop_id,
        .gateway_id = relay->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = now_ms,
        .hop_count = advertised_hop_count + 1u,
        .quality = quality,
        .valid = true,
    };

    return upsert_downlink(relay, &entry);
}

static int parse_route_discovery_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t request_id,
                                      uint32_t timing_reference_ms,
                                      struct route_discovery_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, &fields->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_QUALITY, &fields->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_RELAY_CAPACITY_STATE, &fields->relay_capacity_state);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->relay_capacity_state = RELAY_CAP_UNKNOWN;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->relay_capacity_state > RELAY_CAP_BLACK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_QUEUE_FREE_HINT, &fields->queue_free_hint);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->queue_free_hint = UINT16_MAX;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_CHANNEL9_BUSY_HINT, &fields->channel9_busy_hint);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->channel9_busy_hint = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->relay_capacity_state == RELAY_CAP_UNKNOWN) {
        fields->queue_free_hint = 0u;
        fields->channel9_busy_hint = 0u;
        fields->capacity_validity_interval_ms = 0u;
    }

    ret = find_u32_tlv(payload, payload_len, TLV_FLOOD_EPOCH_ID,
                       &fields->flood_epoch_id);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_epoch_id = request_id;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_FLOOD_PROFILE_VERSION,
                       &fields->flood_profile_version);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_SLOT_SEED, &fields->slot_seed);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->slot_seed = route_discovery_slot_seed(fields->origin_id,
                                                      fields->target_id,
                                                      request_id,
                                                      fields->route_epoch);
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_ROUTE_REQUEST_FLAGS, &fields->request_flags);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->request_flags = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if ((fields->request_flags & ~MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_ROUTE_REPLY_RX_DELAY_MS,
                       &fields->route_reply_rx_delay_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->route_reply_rx_delay_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REPLY_NONCE, &fields->reply_nonce);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->reply_nonce = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_METRIC_CRC, &fields->metric_crc);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->metric_crc = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_event_timing_from_tlvs_at(&fields->proposed_channel9_timing,
                                         payload,
                                         payload_len,
                                         timing_reference_ms,
                                         true);
    if (ret == PROTO_OK) {
        fields->proposed_channel9_timing_valid = true;
        fields->timing_reference_ms = timing_reference_ms;
    } else if (ret == PROTO_ERR_NOT_FOUND) {
        memset(&fields->proposed_channel9_timing,
               0,
               sizeof(fields->proposed_channel9_timing));
        fields->proposed_channel9_timing_valid = false;
        fields->timing_reference_ms = 0u;
    } else {
        return ret;
    }
    return PROTO_OK;
}

static int parse_route_reply_ack_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      struct route_reply_ack_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_FLOOD_EPOCH_ID,
                       &fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REPLY_NONCE, &fields->reply_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    return find_u16_tlv(payload, payload_len, TLV_METRIC_CRC, &fields->metric_crc);
}

static int append_gateway_route_adv_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         const struct gateway_route_adv_fields *fields)
{
    const struct flood_control_fields flood_control = {
        .random_backoff_max_ms = fields->random_backoff_max_ms,
        .random_backoff_slot_ms = fields->random_backoff_slot_ms,
        .retry_count = fields->flood_retry_count,
    };
    int ret;

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, fields->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_GATEWAY_EPOCH, fields->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_GATEWAY_ROUTE_SEQ, fields->gateway_route_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_PATH_QUALITY_MIN, fields->path_quality_min);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_ACCUMULATED_COST, fields->route_cost);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_RELAY_CAPACITY_STATE, fields->gateway_capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                         fields->capacity_validity_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_FLOOD_PROFILE_VERSION, fields->flood_profile_version);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_FLOOD_EPOCH_ID, fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_SLOT_SEED, fields->slot_seed);
    if (ret != PROTO_OK) {
        return ret;
    }
    return append_flood_control_tlvs(payload,
                                     payload_cap,
                                     offset,
                                     &flood_control,
                                     fields->flood_packet_age_ms);
}

static int parse_gateway_route_adv_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct gateway_route_adv_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_GATEWAY_ID, &fields->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_GATEWAY_EPOCH, &fields->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len,
                       TLV_GATEWAY_ROUTE_SEQ, &fields->gateway_route_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_PATH_QUALITY_MIN, &fields->path_quality_min);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_ACCUMULATED_COST, &fields->route_cost);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_RELAY_CAPACITY_STATE, &fields->gateway_capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->gateway_capacity_state == RELAY_CAP_UNKNOWN) {
        fields->capacity_validity_interval_ms = 0u;
    }
    ret = find_u16_tlv(payload, payload_len,
                       TLV_FLOOD_PROFILE_VERSION, &fields->flood_profile_version);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len,
                       TLV_FLOOD_EPOCH_ID, &fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_SLOT_SEED, &fields->slot_seed);
    if (ret != PROTO_OK) {
        return ret;
    }

    {
        struct flood_control_fields flood_control;

        ret = parse_flood_control_tlvs(payload, payload_len, &flood_control);
        if (ret != PROTO_OK) {
            return ret;
        }
        fields->random_backoff_max_ms = flood_control.random_backoff_max_ms;
        fields->random_backoff_slot_ms = flood_control.random_backoff_slot_ms;
        fields->flood_retry_count = flood_control.retry_count;
    }

    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_FLOOD_PACKET_AGE_MS,
                       &fields->flood_packet_age_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_packet_age_ms = 0u;
        return PROTO_OK;
    }
    return ret;
}

static int build_route_reply_ack(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const struct route_discovery_fields *fields,
                                 uint64_t previous_hop_id,
                                 struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if (fields == NULL ||
        packet->msg_type != MSG_ROUTE_REPLY ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        fields->reply_nonce == 0u ||
        fields->metric_crc == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    ret = tlv_append_u64(out->payload, sizeof(out->payload), &payload_len,
                         TLV_INITIATOR_ID, fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(out->payload, sizeof(out->payload), &payload_len,
                         TLV_RESPONDER_ID, fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(out->payload, sizeof(out->payload), &payload_len,
                         TLV_FLOOD_EPOCH_ID, fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(out->payload, sizeof(out->payload), &payload_len,
                         TLV_REPLY_NONCE, fields->reply_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(out->payload, sizeof(out->payload), &payload_len,
                         TLV_METRIC_CRC, fields->metric_crc);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REPLY_ACK;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = previous_hop_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static void complete_route_reply_ack_fields(const struct proto_packet *packet,
                                            struct route_discovery_fields *fields)
{
    if (fields->reply_nonce == 0u) {
        fields->reply_nonce = route_reply_nonce(fields->origin_id,
                                                fields->target_id,
                                                packet->session_id,
                                                fields->flood_epoch_id);
    }
    if (fields->metric_crc == 0u) {
        fields->metric_crc = route_reply_metric_crc(fields);
    }
}

static void add_route_reply_ack_action(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       struct mesh_relay_result *result)
{
    struct route_discovery_fields fields = {0};

    if (packet->msg_type != MSG_ROUTE_REPLY ||
        parse_route_discovery_tlvs(payload,
                                   payload_len,
                                   packet->session_id,
                                   0u,
                                   &fields) != PROTO_OK) {
        return;
    }
    if (packet->src_id != fields.target_id ||
        packet->dst_id != fields.origin_id ||
        !id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.flood_epoch_id == 0u) {
        return;
    }

    complete_route_reply_ack_fields(packet, &fields);
    if (build_route_reply_ack(relay,
                              packet,
                              &fields,
                              previous_hop_id,
                              &result->route_reply_ack) == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK;
    }
}

static int handle_route_reply_ack(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    struct route_reply_ack_fields fields = {0};
    int ret;

    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->dst_id != relay->local_id ||
        packet->src_id != previous_hop_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_reply_ack_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.flood_epoch_id == 0u ||
        fields.reply_nonce == 0u ||
        fields.metric_crc == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    result->actions |= MESH_RELAY_ACTION_ROUTE_REPLY_ACKED;
    return PROTO_OK;
}

static bool gateway_route_adv_fields_valid(const struct gateway_route_adv_fields *fields)
{
    if (fields == NULL ||
        !id_is_unicast(fields->gateway_id) ||
        fields->flood_epoch_id == 0u ||
        fields->gateway_route_seq == 0u ||
        fields->flood_profile_version == 0u ||
        fields->path_quality_min > 100u ||
        fields->gateway_capacity_state > RELAY_CAP_BLACK ||
        (fields->gateway_capacity_state != RELAY_CAP_UNKNOWN &&
         fields->capacity_validity_interval_ms == 0u) ||
        fields->route_cost != gateway_route_cost(fields->hop_count, fields->path_quality_min)) {
        return false;
    }
    return true;
}

static int build_gateway_route_adv_forward(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint8_t link_quality,
                                           uint64_t local_id,
                                           uint32_t now_ms,
                                           uint32_t random_value,
                                           struct mesh_outbound *out)
{
    struct gateway_route_adv_fields fields = {0};
    struct flood_control_fields flood_control;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl <= 1u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_gateway_route_adv_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!gateway_route_adv_fields_valid(&fields) ||
        fields.hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    fields.path_quality_min = combined_quality(fields.path_quality_min, link_quality);
    fields.hop_count++;
    fields.route_cost = gateway_route_cost(fields.hop_count, fields.path_quality_min);
    fields.flood_packet_age_ms = packet->message_age_ms;
    ret = append_gateway_route_adv_tlvs(out->payload,
                                        sizeof(out->payload),
                                        &out_payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->queued_at_ms = now_ms;
    out->flood_retry_count = fields.flood_retry_count;
    flood_control.random_backoff_max_ms = fields.random_backoff_max_ms;
    flood_control.random_backoff_slot_ms = fields.random_backoff_slot_ms;
    flood_control.retry_count = fields.flood_retry_count;
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(local_id,
                                                                fields.slot_seed,
                                                                fields.hop_count,
                                                                &flood_control,
                                                                random_value);
    return PROTO_OK;
}

static int handle_gateway_route_adv(struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint64_t previous_hop_id,
                                    uint8_t link_quality,
                                    uint32_t now_ms,
                                    uint32_t random_value,
                                    struct mesh_relay_result *result)
{
    struct gateway_route_adv_fields fields = {0};
    uint8_t path_quality;
    int ret;
    int route_ret;

    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->dst_id != MESH_BROADCAST_ID) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_gateway_route_adv_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!gateway_route_adv_fields_valid(&fields) ||
        packet->src_id != fields.gateway_id ||
        fields.gateway_id != relay->gateway_id) {
        return PROTO_ERR_MALFORMED;
    }

    path_quality = combined_quality(fields.path_quality_min, link_quality);
    route_ret = upsert_reactive_route(relay,
                                      fields.gateway_id,
                                      previous_hop_id,
                                      fields.gateway_epoch,
                                      fields.hop_count,
                                      path_quality,
                                      fields.gateway_capacity_state,
                                      UINT16_MAX,
                                      0u,
                                      fields.capacity_validity_interval_ms,
                                      now_ms);
    if (route_ret != PROTO_OK &&
        route_ret != PROTO_ERR_NO_SPACE) {
        return route_ret;
    }
    if (route_ret == PROTO_OK && relay->route_discovery.active &&
        relay->route_discovery.target_id == fields.gateway_id) {
        mesh_relay_note_route_discovery_ready(relay, fields.gateway_id);
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
    }

    ret = build_gateway_route_adv_forward(packet,
                                          payload,
                                          payload_len,
                                          link_quality,
                                          relay->local_id,
                                          now_ms,
                                          random_value,
                                          &result->gateway_route_adv);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV;
    } else if (ret != PROTO_ERR_STALE) {
        return ret;
    }

    return PROTO_OK;
}

static int build_route_reply(struct mesh_relay *relay,
                             const struct route_discovery_fields *request,
                             uint64_t next_hop_id,
                             uint32_t session_id,
                             uint32_t route_epoch,
                             struct mesh_outbound *out)
{
    struct route_discovery_fields fields;
    const struct route_candidate *selected = NULL;
    size_t payload_len = 0u;
    int ret;

    if (request == NULL ||
        !id_is_unicast(request->origin_id) ||
        !id_is_unicast(request->target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    fields = *request;
    fields.route_epoch = route_epoch;
    if (request->target_id == relay->gateway_id &&
        request->target_id != relay->local_id) {
        selected = route_selected(&relay->upstream);
    }
    if (selected != NULL && selected->hop_count < UINT8_MAX) {
        fields.hop_count = selected->hop_count + 1u;
        fields.quality = selected->link_quality;
    } else {
        fields.hop_count = 0u;
        fields.quality = 100u;
    }
    fields.relay_capacity_state = relay_current_capacity_state(relay);
    fields.queue_free_hint = relay_current_queue_free_hint(relay);
    fields.channel9_busy_hint = relay_active_channel9_timing_count(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.request_flags = 0u;
    fields.route_reply_rx_delay_ms = 0u;
    fields.reply_nonce = route_reply_nonce(fields.origin_id,
                                           fields.target_id,
                                           session_id,
                                           fields.flood_epoch_id);
    fields.metric_crc = route_reply_metric_crc(&fields);

    memset(out, 0, sizeof(*out));
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REPLY;
    out->packet.flags = 0u;
    out->packet.src_id = request->target_id;
    out->packet.dst_id = request->origin_id;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int build_route_reply_forward(const struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t link_quality,
                                     uint32_t now_ms,
                                     struct mesh_outbound *out)
{
    struct route_discovery_fields fields = {0};
    uint64_t next_hop_id = 0u;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl == 0u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     0u,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields.hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }
    if (fields.reply_nonce == 0u) {
        fields.reply_nonce = route_reply_nonce(fields.origin_id,
                                               fields.target_id,
                                               packet->session_id,
                                               fields.flood_epoch_id);
    }
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    fields.quality = combined_quality(fields.quality, link_quality);
    fields.hop_count++;
    fields.proposed_channel9_timing_valid = false;
    fields.timing_reference_ms = 0u;
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            fields.proposed_channel9_timing = entry->timing;
            fields.proposed_channel9_timing_valid = true;
            fields.timing_reference_ms = now_ms;
            break;
        }
    }
    fields.metric_crc = route_reply_metric_crc(&fields);
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int handle_route_request(struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t previous_hop_id,
                                uint8_t link_quality,
                                uint32_t now_ms,
                                uint32_t random_value,
                                struct mesh_relay_result *result,
                                bool duplicate_packet)
{
    struct route_discovery_fields fields = {0};
    struct flood_seen_entry *flood_entry = NULL;
    const struct route_candidate *selected = NULL;
    bool first_seen = false;
    bool install_reply_timing = false;
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields.origin_id != packet->src_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        !id_is_unicast(fields.target_id) ||
        fields.target_id == fields.origin_id ||
        fields.hop_count == UINT8_MAX ||
        fields.flood_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY &&
        fields.target_id == relay->local_id &&
        (fields.request_flags & MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u &&
        (fields.hop_count == 0u || previous_hop_id == fields.origin_id)) {
        return PROTO_ERR_STALE;
    }
    if (duplicate_packet) {
        relay_diag_inc_u8(&relay->diagnostics.flood_suppression_count);
        return PROTO_ERR_STALE;
    }
    if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay_active_channel9_timing_count(relay) != 0u) {
        return PROTO_ERR_BUSY;
    }

    fields.quality = combined_quality(fields.quality, link_quality);
    flood_entry = route_solicit_flood_note(relay,
                                           &fields,
                                           packet->session_id,
                                           previous_hop_id,
                                           now_ms,
                                           &first_seen);
    ret = upsert_reactive_route(relay,
                                fields.origin_id,
                                previous_hop_id,
                                fields.route_epoch,
                                fields.hop_count,
                                fields.quality,
                                fields.relay_capacity_state,
                                fields.queue_free_hint,
                                fields.channel9_busy_hint,
                                fields.capacity_validity_interval_ms,
                                now_ms);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }
    install_reply_timing = (ret == PROTO_OK &&
                            fields.proposed_channel9_timing_valid);

    selected = route_selected(&relay->upstream);

    if (fields.target_id == relay->local_id ||
        (fields.target_id == relay->gateway_id && selected != NULL)) {
        uint32_t reply_epoch = fields.route_epoch;

        if (selected != NULL && selected->route_epoch > reply_epoch) {
            reply_epoch = selected->route_epoch;
        } else if (relay->upstream.current_epoch > reply_epoch) {
            reply_epoch = relay->upstream.current_epoch;
        }
        ret = build_route_reply(relay,
                                &fields,
                                previous_hop_id,
                                packet->session_id,
                                reply_epoch,
                                &result->route_reply);
        if (ret == PROTO_OK) {
            if (fields.route_reply_rx_delay_ms != 0u) {
                result->route_reply.earliest_tx_ms =
                    now_ms + fields.route_reply_rx_delay_ms;
            }
            add_route_reply_backup(relay, fields.origin_id, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
            if (install_reply_timing) {
                struct mesh_event_timing downstream_timing = fields.proposed_channel9_timing;

                mesh_event_timing_set_local_first_slot_tx(&downstream_timing, false);
                (void)mesh_relay_set_channel9_timing_guarded(
                    relay,
                    previous_hop_id,
                    &downstream_timing,
                    MESH_RELAY_EVENT_TIMINGS,
                    NULL);
            }
        }
        if (fields.target_id == relay->local_id) {
            return ret;
        }
    }
    if (mesh_relay_tx_active(relay)) {
        if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
            return PROTO_OK;
        }
        add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
        return PROTO_ERR_BUSY;
    }

    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
        return PROTO_OK;
    }
    if (first_seen &&
        route_solicit_forward_allowed(flood_entry)) {
        ret = build_route_request_forward(packet,
                                          &fields,
                                          now_ms,
                                          random_value,
                                          &result->route_request);
        if (ret == PROTO_OK) {
            flood_entry->forward_count++;
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REQ;
            return PROTO_OK;
        }
        if (ret != PROTO_ERR_STALE) {
            return ret;
        }
    }

    return PROTO_ERR_NOT_FOUND;
}

static int handle_route_reply(struct mesh_relay *relay,
                              const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint64_t previous_hop_id,
                              uint8_t link_quality,
                              uint32_t now_ms,
                              struct mesh_relay_result *result)
{
    struct route_discovery_fields fields = {0};
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->src_id != fields.target_id ||
        packet->dst_id != fields.origin_id ||
        !id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.hop_count == UINT8_MAX ||
        fields.flood_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    complete_route_reply_ack_fields(packet, &fields);

    fields.quality = combined_quality(fields.quality, link_quality);
    ret = upsert_reactive_route(relay,
                                fields.target_id,
                                previous_hop_id,
                                fields.route_epoch,
                                fields.hop_count,
                                fields.quality,
                                fields.relay_capacity_state,
                                fields.queue_free_hint,
                                fields.channel9_busy_hint,
                                fields.capacity_validity_interval_ms,
                                now_ms);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }
    if (ret == PROTO_OK) {
        if (fields.proposed_channel9_timing_valid) {
            struct mesh_event_timing upstream_timing = fields.proposed_channel9_timing;

            mesh_event_timing_set_local_first_slot_tx(&upstream_timing, true);
            (void)mesh_relay_set_channel9_timing_guarded(
                relay,
                previous_hop_id,
                &upstream_timing,
                MESH_RELAY_EVENT_TIMINGS,
                NULL);
        }
        mesh_relay_note_route_discovery_ready(relay, fields.target_id);
        add_route_reply_ack_action(relay, packet, payload, payload_len, previous_hop_id, result);
    }

    if (packet->dst_id == relay->local_id) {
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
        return PROTO_OK;
    }
    if (mesh_relay_tx_active(relay)) {
        add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
        return PROTO_ERR_BUSY;
    }

    ret = build_route_reply_forward(relay,
                                    packet,
                                    payload,
                                    payload_len,
                                    link_quality,
                                    now_ms,
                                    &result->route_reply);
    if (ret == PROTO_OK) {
        add_route_reply_backup(relay,
                               packet->dst_id,
                               result->route_reply.next_hop_id,
                               result);
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
    }
    return ret;
}

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch)
{
    if (relay == NULL) {
        return;
    }

    memset(relay, 0, sizeof(*relay));
    relay->role = role;
    relay->local_id = local_id;
    relay->gateway_id = gateway_id;
    relay->next_seq = 1u;
    route_table_init(&relay->upstream, route_epoch);
}

const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(target_id)) {
        return NULL;
    }

    index = downlink_index(relay, target_id);
    return index >= 0 ? &relay->downlinks[index] : NULL;
}

uint8_t mesh_relay_expire_routes(struct mesh_relay *relay, uint32_t now_ms)
{
    if (relay == NULL) {
        return 0u;
    }

    (void)now_ms;
    return 0u;
}

int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id)
{
    const struct route_candidate *upstream;
    const struct mesh_downlink_entry *downlink;

    if (relay == NULL || next_hop_id == NULL || !id_is_unicast(dst_id)) {
        return PROTO_ERR_ARG;
    }
    if (dst_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_ERR_MALFORMED;
        }
        upstream = route_selected(&relay->upstream);
        if (upstream == NULL) {
            return PROTO_ERR_NOT_FOUND;
        }
        *next_hop_id = upstream->next_hop_id;
        return PROTO_OK;
    }

    downlink = mesh_relay_find_downlink(relay, dst_id);
    if (downlink == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    *next_hop_id = downlink->next_hop_id;
    return PROTO_OK;
}

static int event_timing_index(const struct mesh_relay *relay, uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == next_hop_id) {
            return (int)i;
        }
    }
    return -1;
}

static int free_event_timing_index(const struct mesh_relay *relay)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (!relay->event_timings[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

static enum mesh_relay_channel9_direction channel9_peer_direction(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    const struct route_candidate *upstream;
    bool matches_upstream = false;
    bool matches_downstream = false;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
    }

    upstream = route_selected(&relay->upstream);
    matches_upstream = upstream != NULL && upstream->next_hop_id == next_hop_id;
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            matches_downstream = true;
            break;
        }
    }

    if (matches_upstream && matches_downstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS;
    }
    if (matches_upstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM;
    }
    if (matches_downstream) {
        return MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM;
    }
    return MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN;
}

static bool channel9_direction_valid(enum mesh_relay_channel9_direction direction)
{
    return direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM ||
           direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM;
}

static void channel9_guard_reset(struct mesh_relay_channel9_guard_status *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

static bool channel9_plan_misses_event(enum mesh_event_plan_action action)
{
    return action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
           action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD;
}

int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing)
{
    int index;

    if (relay == NULL || timing == NULL || !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        timing->mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_ARG;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index < 0) {
        index = free_event_timing_index(relay);
    }
    if (index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    relay->event_timings[index].next_hop_id = next_hop_id;
    relay->event_timings[index].timing = *timing;
    relay->event_timings[index].valid = true;
    route_set_channel9_timing_valid(&relay->upstream,
                                    next_hop_id,
                                    relay->gateway_id,
                                    true,
                                    timing->next_event_time_ms);
    return PROTO_OK;
}

int mesh_relay_set_channel9_timing_guarded(struct mesh_relay *relay,
                                           uint64_t next_hop_id,
                                           const struct mesh_event_timing *timing,
                                           uint8_t max_active_peers,
                                           struct mesh_relay_channel9_guard_status *status)
{
    enum mesh_relay_channel9_direction direction;
    uint8_t active_peer_count = 0u;
    int index;

    channel9_guard_reset(status);
    if (relay == NULL || timing == NULL || !id_is_unicast(next_hop_id) ||
        max_active_peers == 0u) {
        return PROTO_ERR_ARG;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        if (status != NULL) {
            status->reason = MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER;
            status->direction = channel9_peer_direction(relay, next_hop_id);
        }
        return mesh_relay_set_channel9_timing(relay, next_hop_id, timing);
    }

    direction = channel9_peer_direction(relay, next_hop_id);
    if (status != NULL) {
        status->direction = direction;
    }
    if (!channel9_direction_valid(direction)) {
        if (status != NULL) {
            status->reason = MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER;
        }
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        enum mesh_relay_channel9_direction entry_direction;

        if (!entry->valid) {
            continue;
        }

        active_peer_count++;
        entry_direction = channel9_peer_direction(relay, entry->next_hop_id);
        if (!channel9_direction_valid(entry_direction)) {
            if (status != NULL) {
                status->reason = MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_ACTIVE_PEER;
                status->conflict_peer_id = entry->next_hop_id;
                status->conflict_direction = entry_direction;
                status->active_peer_count = active_peer_count;
            }
            return PROTO_ERR_MALFORMED;
        }
    }

    if (status != NULL) {
        status->active_peer_count = active_peer_count;
    }
    if (active_peer_count >= max_active_peers) {
        if (status != NULL) {
            status->reason = MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS;
        }
        return PROTO_ERR_NO_SPACE;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];
        enum mesh_relay_channel9_direction entry_direction;

        if (!entry->valid) {
            continue;
        }

        entry_direction = channel9_peer_direction(relay, entry->next_hop_id);
        if (entry_direction == direction) {
            if (status != NULL) {
                status->reason = MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY;
                status->conflict_peer_id = entry->next_hop_id;
                status->conflict_direction = entry_direction;
            }
            return PROTO_ERR_BUSY;
        }
    }

    if (status != NULL) {
        status->reason = MESH_RELAY_CHANNEL9_GUARD_OK;
    }
    return mesh_relay_set_channel9_timing(relay, next_hop_id, timing);
}

void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        relay->event_timings[index].valid = false;
        route_set_channel9_timing_valid(&relay->upstream,
                                        next_hop_id,
                                        relay->gateway_id,
                                        false,
                                        0u);
    }
}

void mesh_relay_reset_route_discovery(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->route_discovery, 0, sizeof(relay->route_discovery));
    }
}

void mesh_relay_note_route_discovery_ready(struct mesh_relay *relay,
                                           uint64_t target_id)
{
    if (relay == NULL || !id_is_unicast(target_id)) {
        return;
    }
    if (relay->route_discovery.active &&
        relay->route_discovery.target_id == target_id) {
        mesh_relay_reset_route_discovery(relay);
    }
}

void mesh_relay_invalidate_routes(struct mesh_relay *relay)
{
    uint32_t next_epoch;

    if (relay == NULL) {
        return;
    }

    next_epoch = relay->upstream.current_epoch + 1u;
    if (next_epoch == 0u) {
        next_epoch = 1u;
    }
    route_table_init(&relay->upstream, next_epoch);
    memset(relay->downlinks, 0, sizeof(relay->downlinks));
    memset(relay->event_timings, 0, sizeof(relay->event_timings));
    mesh_relay_reset_route_discovery(relay);
}

static int mesh_relay_require_channel9_event_for_slot(const struct mesh_relay *relay,
                                                      uint64_t next_hop_id,
                                                      const struct mesh_channel5_requirements *requirements,
                                                      uint32_t now_ms,
                                                      bool require_local_tx,
                                                      struct mesh_event_plan *plan)
{
    int index;
    int ret;

    if (relay == NULL || requirements == NULL || plan == NULL ||
        !id_is_unicast(next_hop_id)) {
        return PROTO_ERR_ARG;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index < 0) {
        return PROTO_ERR_STALE;
    }

    ret = mesh_event_plan_channel9(&relay->event_timings[index].timing,
                                   requirements,
                                   now_ms,
                                   plan);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (plan->action == MESH_EVENT_PLAN_START ||
        plan->action == MESH_EVENT_PLAN_CLIP) {
        if (require_local_tx &&
            !mesh_event_timing_local_tx_slot(&relay->event_timings[index].timing)) {
            return PROTO_ERR_BUSY;
        }
        return PROTO_OK;
    }
    if (plan->action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5) {
        return PROTO_ERR_STALE;
    }
    return PROTO_ERR_BUSY;
}

int mesh_relay_require_channel9_event(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      const struct mesh_channel5_requirements *requirements,
                                      uint32_t now_ms,
                                      struct mesh_event_plan *plan)
{
    return mesh_relay_require_channel9_event_for_slot(relay,
                                                      next_hop_id,
                                                      requirements,
                                                      now_ms,
                                                      false,
                                                      plan);
}

int mesh_relay_require_channel9_tx_event(const struct mesh_relay *relay,
                                         uint64_t next_hop_id,
                                         const struct mesh_channel5_requirements *requirements,
                                         uint32_t now_ms,
                                         struct mesh_event_plan *plan)
{
    return mesh_relay_require_channel9_event_for_slot(relay,
                                                      next_hop_id,
                                                      requirements,
                                                      now_ms,
                                                      true,
                                                      plan);
}

uint8_t mesh_relay_expire_channel9_timings(struct mesh_relay *relay,
                                           uint32_t now_ms)
{
    uint8_t expired = 0u;

    if (relay == NULL) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && !mesh_event_timing_usable(&entry->timing, now_ms)) {
            route_set_channel9_timing_valid(&relay->upstream,
                                            entry->next_hop_id,
                                            relay->gateway_id,
                                            false,
                                            now_ms);
            entry->valid = false;
            expired++;
        }
    }

    return expired;
}

static int build_route_request_with_timing_flags_id(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    struct mesh_outbound *out,
    uint32_t now_ms,
    uint32_t request_id)
{
    struct route_discovery_fields fields = {0};
    uint32_t route_epoch;
    uint32_t session_id;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || out == NULL ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(target_id) ||
        target_id == relay->local_id ||
        (request_flags & ~MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u) {
        return PROTO_ERR_ARG;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    memset(out, 0, sizeof(*out));

    route_epoch = relay->upstream.current_epoch;
    session_id = request_id;
    if (session_id == 0u) {
        session_id = now_ms != 0u ? now_ms : route_epoch;
    }
    if (session_id == 0u) {
        session_id = 1u;
    }

    fields.origin_id = relay->local_id;
    fields.target_id = target_id;
    fields.route_epoch = route_epoch;
    fields.flood_epoch_id = session_id;
    fields.slot_seed = route_discovery_slot_seed(relay->local_id,
                                                 target_id,
                                                 session_id,
                                                 route_epoch);
    fields.flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    fields.hop_count = 0u;
    fields.quality = 100u;
    fields.relay_capacity_state = relay_current_capacity_state(relay);
    fields.queue_free_hint = relay_current_queue_free_hint(relay);
    fields.channel9_busy_hint = relay_active_channel9_timing_count(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.request_flags = request_flags;
    fields.route_reply_rx_delay_ms = route_reply_rx_delay_ms;
    if (proposed_channel9_timing != NULL) {
        fields.proposed_channel9_timing = *proposed_channel9_timing;
        fields.proposed_channel9_timing_valid = true;
        fields.timing_reference_ms = timing_reference_ms;
    } else {
        memset(&fields.proposed_channel9_timing,
               0,
               sizeof(fields.proposed_channel9_timing));
        fields.proposed_channel9_timing_valid = false;
        fields.timing_reference_ms = 0u;
    }

    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REQ;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = route_request_ttl_for_attempt(0u, false);
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    return PROTO_OK;
}

int mesh_relay_build_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    struct mesh_outbound *out,
    uint32_t now_ms)
{
    return build_route_request_with_timing_flags_id(relay,
                                                    target_id,
                                                    proposed_channel9_timing,
                                                    timing_reference_ms,
                                                    request_flags,
                                                    route_reply_rx_delay_ms,
                                                    out,
                                                    now_ms,
                                                    now_ms);
}

int mesh_relay_build_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    struct mesh_outbound *out,
    uint32_t now_ms)
{
    return mesh_relay_build_route_request_with_timing_flags(relay,
                                                            target_id,
                                                            proposed_channel9_timing,
                                                            timing_reference_ms,
                                                            0u,
                                                            0u,
                                                            out,
                                                            now_ms);
}

int mesh_relay_build_route_request(struct mesh_relay *relay,
                                   uint64_t target_id,
                                   struct mesh_outbound *out,
                                   uint32_t now_ms)
{
    return mesh_relay_build_route_request_with_timing(relay,
                                                     target_id,
                                                     NULL,
                                                     0u,
                                                     out,
                                                     now_ms);
}

int mesh_relay_build_gateway_route_adv(struct mesh_relay *relay,
                                       uint32_t gateway_route_seq,
                                       uint32_t now_ms,
                                       struct mesh_outbound *out)
{
    struct gateway_route_adv_fields fields;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || out == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        !id_is_unicast(relay->local_id) ||
        gateway_route_seq == 0u) {
        return PROTO_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    memset(&fields, 0, sizeof(fields));
    fields.gateway_id = relay->gateway_id;
    fields.gateway_epoch = (uint16_t)relay->upstream.current_epoch;
    fields.gateway_route_seq = gateway_route_seq;
    fields.hop_count = 0u;
    fields.path_quality_min = 100u;
    fields.route_cost = gateway_route_cost(fields.hop_count, fields.path_quality_min);
    fields.gateway_capacity_state = relay_current_capacity_state(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    fields.flood_epoch_id = gateway_route_seq;
    fields.slot_seed = gateway_route_adv_slot_seed(fields.gateway_id,
                                                   fields.gateway_route_seq,
                                                   fields.gateway_epoch);
    fields.random_backoff_max_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS;
    fields.random_backoff_slot_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS;
    fields.flood_retry_count = FLOOD_DEFAULT_RETRY_COUNT;
    fields.flood_packet_age_ms = 0u;

    ret = append_gateway_route_adv_tlvs(out->payload,
                                        sizeof(out->payload),
                                        &payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_GATEWAY_ROUTE_ADV;
    out->packet.flags = 0u;
    out->packet.src_id = relay->gateway_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = gateway_route_seq;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    out->flood_retry_count = fields.flood_retry_count;
    return PROTO_OK;
}

int mesh_relay_prepare_route_request_with_timing_flags(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint8_t request_flags,
    uint16_t route_reply_rx_delay_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out)
{
    uint32_t delay_ms;
    int ret;

    if (relay == NULL || out == NULL ||
        !id_is_unicast(target_id) ||
        target_id == relay->local_id ||
        (request_flags & ~MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u) {
        return PROTO_ERR_ARG;
    }

    if (!relay->route_discovery.active ||
        relay->route_discovery.target_id != target_id) {
        memset(&relay->route_discovery, 0, sizeof(relay->route_discovery));
        relay->route_discovery.active = true;
        relay->route_discovery.target_id = target_id;
        relay->route_discovery.next_request_id = now_ms != 0u ? now_ms :
                                                 relay->upstream.current_epoch;
        if (relay->route_discovery.next_request_id == 0u) {
            relay->route_discovery.next_request_id = 1u;
        }
    }

    if (relay->route_discovery.attempts >= MESH_RELAY_ROUTE_DISCOVERY_MAX_ATTEMPTS) {
        return PROTO_ERR_STALE;
    }
    if (relay->route_discovery.attempts > 0u &&
        !deadline_reached(now_ms, relay->route_discovery.next_request_ms)) {
        return PROTO_ERR_BUSY;
    }

    ret = build_route_request_with_timing_flags_id(
        relay,
        target_id,
        proposed_channel9_timing,
        timing_reference_ms,
        request_flags,
        route_reply_rx_delay_ms,
        out,
        now_ms,
        relay->route_discovery.next_request_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    out->packet.ttl = route_request_ttl_for_attempt(relay->route_discovery.attempts, false);

    relay->route_discovery.attempts++;
    relay->route_discovery.next_request_id++;
    if (relay->route_discovery.next_request_id == 0u) {
        relay->route_discovery.next_request_id = 1u;
    }
    delay_ms = mesh_relay_route_discovery_backoff_ms(relay->route_discovery.attempts,
                                                     random_value);
    relay->route_discovery.next_request_ms = now_ms + delay_ms;
    return PROTO_OK;
}

int mesh_relay_prepare_route_request_with_timing(
    struct mesh_relay *relay,
    uint64_t target_id,
    const struct mesh_event_timing *proposed_channel9_timing,
    uint32_t timing_reference_ms,
    uint32_t now_ms,
    uint32_t random_value,
    struct mesh_outbound *out)
{
    return mesh_relay_prepare_route_request_with_timing_flags(relay,
                                                              target_id,
                                                              proposed_channel9_timing,
                                                              timing_reference_ms,
                                                              0u,
                                                              0u,
                                                              now_ms,
                                                              random_value,
                                                              out);
}

int mesh_relay_prepare_route_request(struct mesh_relay *relay,
                                     uint64_t target_id,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_outbound *out)
{
    return mesh_relay_prepare_route_request_with_timing(relay,
                                                       target_id,
                                                       NULL,
                                                       0u,
                                                       now_ms,
                                                       random_value,
                                                       out);
}

int mesh_relay_note_direct_gateway_route(struct mesh_relay *relay,
                                         uint32_t now_ms)
{
    int ret;

    if (relay == NULL ||
        relay->role == MESH_RELAY_ROLE_GATEWAY ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    ret = upsert_reactive_route(relay,
                                relay->gateway_id,
                                relay->gateway_id,
                                relay->upstream.current_epoch,
                                0u,
                                100u,
                                RELAY_CAP_UNKNOWN,
                                0u,
                                0u,
                                0u,
                                now_ms);
    if (ret == PROTO_OK || ret == PROTO_ERR_NOT_FOUND) {
        ret = route_record_candidate_success_at(&relay->upstream,
                                                relay->gateway_id,
                                                relay->gateway_id,
                                                now_ms);
    }
    if (ret == PROTO_OK) {
        mesh_relay_note_route_discovery_ready(relay, relay->gateway_id);
    }
    return ret;
}

int mesh_relay_build_route_reply_for_request(struct mesh_relay *relay,
                                             const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             uint32_t now_ms,
                                             struct mesh_outbound *out)
{
    struct route_discovery_fields fields = {0};
    const struct route_candidate *selected = NULL;
    uint32_t reply_epoch;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->msg_type != MSG_ROUTE_REQ ||
        packet->dst_id != MESH_BROADCAST_ID ||
        packet->payload_len != payload_len ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields.origin_id != packet->src_id ||
        !id_is_unicast(fields.target_id) ||
        fields.target_id == fields.origin_id ||
        fields.flood_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    selected = route_selected(&relay->upstream);
    if (fields.target_id != relay->local_id &&
        !(fields.target_id == relay->gateway_id && selected != NULL)) {
        return PROTO_ERR_NOT_FOUND;
    }

    reply_epoch = fields.route_epoch;
    if (selected != NULL && selected->route_epoch > reply_epoch) {
        reply_epoch = selected->route_epoch;
    } else if (relay->upstream.current_epoch > reply_epoch) {
        reply_epoch = relay->upstream.current_epoch;
    }

    ret = build_route_reply(relay,
                            &fields,
                            previous_hop_id,
                            packet->session_id,
                            reply_epoch,
                            out);
    if (ret == PROTO_OK && fields.route_reply_rx_delay_ms != 0u) {
        out->earliest_tx_ms = now_ms + fields.route_reply_rx_delay_ms;
    }
    return ret;
}

static uint8_t relay_duplicate_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    for (size_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        if (relay->duplicates[i].valid && count < UINT8_MAX) {
            count++;
        }
    }
    return count;
}

static uint8_t relay_parent_hold_down_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    for (size_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid &&
            candidate->hold_down_until_ms != 0u &&
            count < UINT8_MAX) {
            count++;
        }
    }
    return count;
}

static uint8_t relay_collection_pending_count(const struct mesh_relay *relay)
{
    uint8_t count = relay->result_bundle.active ? relay->result_bundle.record_count : 0u;

    if (relay->outbox_record.valid &&
        relay->outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK &&
        count < UINT8_MAX) {
        count++;
    }
    return count;
}

static int mesh_relay_append_telemetry_tlvs(const struct mesh_relay *relay,
                                            uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *offset)
{
    int ret;

    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_MESH_DUPLICATE_COUNT,
                        relay_duplicate_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_COLLECTION_PENDING_COUNT,
                        relay_collection_pending_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_PARENT_HOLDDOWN_COUNT,
                        relay_parent_hold_down_count(relay));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_ROUTE_DISCOVERY_ATTEMPTS,
                        relay->route_discovery.attempts);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_OUTBOX_DELIVERY_STATE,
                        relay->outbox_record.delivery_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_FLOOD_SUPPRESSION_COUNT,
                        relay->diagnostics.flood_suppression_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_ROUTE_REPLY_RETRY_COUNT,
                        relay->diagnostics.route_reply_retry_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload,
                         payload_cap,
                         offset,
                         TLV_BUSY_RESPONSE_COUNT,
                         relay->diagnostics.busy_response_count);
}

int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset)
{
    const struct route_candidate *selected;
    int ret;

    if (relay == NULL || payload == NULL || offset == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, relay->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        ret = tlv_append_u32(payload,
                             payload_cap,
                             offset,
                             TLV_ROUTE_EPOCH,
                             relay->upstream.current_epoch);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, 0u);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, 100u);
        if (ret != PROTO_OK) {
            return ret;
        }
        return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
    }

    selected = route_selected(&relay->upstream);
    if (selected == NULL || selected->hop_count == UINT8_MAX) {
        ret = tlv_append_u8(payload,
                            payload_cap,
                            offset,
                            TLV_REASON,
                            (uint8_t)(-PROTO_ERR_NOT_FOUND));
        if (ret != PROTO_OK) {
            return ret;
        }
        return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_NEXT_HOP_ID, selected->next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_ROUTE_EPOCH, selected->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, selected->hop_count + 1u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, selected->link_quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_RETRY_COUNT, selected->failure_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_append_telemetry_tlvs(relay, payload, payload_cap, offset);
}

bool mesh_relay_tx_active(const struct mesh_relay *relay)
{
    return relay != NULL && relay->pending.state != MESH_RELAY_TX_IDLE;
}

bool mesh_relay_tx_active_local_collection_result(const struct mesh_relay *relay)
{
    return relay != NULL &&
           pending_is_local_collection_result(relay, &relay->pending);
}

bool mesh_relay_result_bundle_pending(const struct mesh_relay *relay)
{
    return relay != NULL &&
           relay->result_bundle.active &&
           relay->result_bundle.record_count > 0u;
}

uint32_t mesh_relay_result_bundle_due_ms(const struct mesh_relay *relay)
{
    if (!mesh_relay_result_bundle_pending(relay)) {
        return UINT32_MAX;
    }
    return relay->result_bundle.due_ms;
}

void mesh_relay_result_bundle_note_forwarded(struct mesh_relay *relay,
                                             const struct mesh_outbound *out)
{
    struct result_bundle_header bundle;

    if (relay == NULL || out == NULL ||
        !mesh_relay_result_bundle_pending(relay) ||
        out->packet.msg_type != MSG_RESULT_BUNDLE ||
        out->packet.src_id != relay->local_id ||
        out->packet.dst_id != relay->gateway_id ||
        out->payload_len == 0u ||
        result_bundle_header_from_tlvs(out->payload,
                                       out->payload_len,
                                       &bundle) != PROTO_OK) {
        return;
    }

    if (bundle.gateway_id == relay->result_bundle.gateway_id &&
        bundle.gateway_epoch == relay->result_bundle.gateway_epoch &&
        bundle.command_seq == relay->result_bundle.command_seq &&
        bundle.collection_epoch_id == relay->result_bundle.collection_epoch_id &&
        bundle.record_count == relay->result_bundle.record_count) {
        result_bundle_clear(&relay->result_bundle);
    }
}

static int outbox_snapshot_validate(const struct mesh_relay *relay,
                                    const struct mesh_relay_outbox_snapshot *snapshot)
{
    const struct persistent_outbox_record *record;
    const struct mesh_pending_tx *pending;
    struct command_result_id result_id;
    uint32_t collection_epoch_id = 0u;

    if (relay == NULL || snapshot == NULL || !snapshot->valid ||
        snapshot->version != MESH_RELAY_OUTBOX_SNAPSHOT_VERSION) {
        return PROTO_ERR_ARG;
    }

    record = &snapshot->record;
    pending = &snapshot->pending;
    if (!record->valid ||
        record->gateway_acked ||
        record->delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED ||
        record->delivery_state == MESH_RELAY_DELIVERY_EXPIRED ||
        record->delivery_state == MESH_RELAY_DELIVERY_COLLECTION_CLOSED ||
        record->expiry_s == 0u ||
        (record->age_ms_saturating / 1000u) >= record->expiry_s ||
        snapshot->role != relay->role ||
        snapshot->local_id != relay->local_id ||
        snapshot->gateway_id != relay->gateway_id ||
        record->gateway_id != relay->gateway_id ||
        pending->state == MESH_RELAY_TX_IDLE ||
        pending->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        pending->packet.payload_len != pending->payload_len ||
        pending->packet.dst_id != relay->gateway_id ||
        (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        record->packet_id != outbox_packet_id_for(&pending->packet) ||
        record->session_id != pending->packet.session_id ||
        record->seq != pending->packet.seq ||
        record->packet_class != pending->packet.msg_type ||
        record->payload_len != pending->payload_len ||
        record->payload_crc != proto_crc16_ccitt_false(pending->payload,
                                                       pending->payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    if (pending_is_result_bundle(relay, pending)) {
        return PROTO_OK;
    }
    if (!pending_has_valid_command_result_id(relay, pending) ||
        command_result_id_from_tlvs(pending->payload,
                                    pending->payload_len,
                                    &result_id) != PROTO_OK ||
        result_id.gateway_id != relay->gateway_id ||
        result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id.node_id != pending->packet.src_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (collection_epoch_id_from_payload(pending->payload,
                                         pending->payload_len,
                                         &collection_epoch_id) == PROTO_OK &&
        collection_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    return PROTO_OK;
}

static uint32_t outbox_snapshot_restore_retry_delay_ms(
    const struct mesh_relay_outbox_snapshot *snapshot)
{
    if (snapshot == NULL ||
        snapshot->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        return RELAY_BUSY_RETRY_MIN_MS;
    }
    if (deadline_reached(snapshot->snapshot_at_ms, snapshot->pending.retry_after_ms)) {
        return 1u;
    }
    return snapshot->pending.retry_after_ms - snapshot->snapshot_at_ms;
}

int mesh_relay_export_outbox_snapshot(struct mesh_relay *relay,
                                      uint32_t now_ms,
                                      struct mesh_relay_outbox_snapshot *snapshot)
{
    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_relay_tx_active(relay) ||
        (!pending_has_valid_command_result_id(relay, &relay->pending) &&
         !pending_is_result_bundle(relay, &relay->pending)) ||
        !relay->outbox_record.valid) {
        return PROTO_ERR_NOT_FOUND;
    }

    pending_refresh_age(&relay->pending, now_ms);
    outbox_record_sync_age_from_pending(relay, now_ms);
    snapshot->version = MESH_RELAY_OUTBOX_SNAPSHOT_VERSION;
    snapshot->role = relay->role;
    snapshot->local_id = relay->local_id;
    snapshot->gateway_id = relay->gateway_id;
    snapshot->record = relay->outbox_record;
    snapshot->pending = relay->pending;
    snapshot->snapshot_at_ms = now_ms;
    snapshot->valid = true;
    return PROTO_OK;
}

int mesh_relay_restore_outbox_snapshot(struct mesh_relay *relay,
                                       const struct mesh_relay_outbox_snapshot *snapshot,
                                       uint32_t now_ms)
{
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = outbox_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }

    relay->pending = snapshot->pending;
    relay->outbox_record = snapshot->record;
    relay->pending.packet.message_age_ms = relay->outbox_record.age_ms_saturating;
    relay->pending.queued_at_ms = now_ms;
    relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    relay->pending.retry_after_ms =
        now_ms + outbox_snapshot_restore_retry_delay_ms(snapshot);
    relay->pending.gateway_ack_deadline_ms = 0u;
    relay->pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = 0u;
    relay->outbox_record.gateway_acked = false;
    relay->outbox_record.delivery_state = outbox_delivery_state_for(&relay->pending);
    relay->outbox_record.age_ms_saturating = relay->pending.packet.message_age_ms;
    return PROTO_OK;
}

static int child_custody_bundle_validate(
    const struct mesh_relay *relay,
    const struct mesh_result_bundle_queue *queue)
{
    if (relay == NULL || queue == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!queue->active) {
        return queue->record_count == 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
    }
    if (queue->gateway_id != relay->gateway_id ||
        queue->gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        queue->command_seq == 0u ||
        queue->collection_epoch_id == 0u ||
        queue->record_count == 0u ||
        queue->record_count > MESH_RELAY_RESULT_BUNDLE_RECORDS) {
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_RESULT_BUNDLE_RECORDS; i++) {
        const struct mesh_result_bundle_entry *entry = &queue->records[i];
        struct command_result_id result_id;
        uint32_t collection_epoch_id = 0u;

        if (i >= queue->record_count) {
            if (entry->valid) {
                return PROTO_ERR_MALFORMED;
            }
            continue;
        }
        if (!entry->valid ||
            entry->payload_len == 0u ||
            entry->payload_len > RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN ||
            entry->payload_crc != proto_crc16_ccitt_false(entry->payload,
                                                          entry->payload_len) ||
            command_result_id_from_tlvs(entry->payload,
                                        entry->payload_len,
                                        &result_id) != PROTO_OK ||
            collection_epoch_id_from_payload(entry->payload,
                                             entry->payload_len,
                                             &collection_epoch_id) != PROTO_OK ||
            collection_epoch_id != queue->collection_epoch_id ||
            !command_result_id_matches(&result_id, &entry->result_id) ||
            entry->result_id.gateway_id != queue->gateway_id ||
            entry->result_id.gateway_epoch != queue->gateway_epoch ||
            entry->result_id.command_seq != queue->command_seq ||
            !id_is_unicast(entry->result_id.node_id)) {
            return PROTO_ERR_MALFORMED;
        }
    }

    return PROTO_OK;
}

static int child_custody_reservation_validate(
    const struct mesh_relay *relay,
    const struct mesh_result_offer_reservation *reservation)
{
    if (relay == NULL || reservation == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!reservation->valid) {
        return PROTO_OK;
    }
    if (!id_is_unicast(reservation->child_id) ||
        reservation->result_len == 0u ||
        reservation->result_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        reservation->result_id.gateway_id != relay->gateway_id ||
        reservation->result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        reservation->result_id.command_seq == 0u ||
        reservation->result_id.node_id != reservation->child_id ||
        !id_is_unicast(reservation->result_id.node_id)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int child_custody_snapshot_validate(
    const struct mesh_relay *relay,
    const struct mesh_relay_child_custody_snapshot *snapshot)
{
    int ret;

    if (relay == NULL || snapshot == NULL || !snapshot->valid ||
        snapshot->version != MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION ||
        snapshot->role != relay->role ||
        snapshot->local_id != relay->local_id ||
        snapshot->gateway_id != relay->gateway_id) {
        return PROTO_ERR_ARG;
    }

    ret = child_custody_bundle_validate(relay, &snapshot->result_bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = child_custody_reservation_validate(relay,
                                             &snapshot->result_offer_reservation);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!snapshot->result_bundle.active &&
        !snapshot->result_offer_reservation.valid) {
        return PROTO_ERR_NOT_FOUND;
    }
    return PROTO_OK;
}

int mesh_relay_export_child_custody_snapshot(
    const struct mesh_relay *relay,
    uint32_t now_ms,
    struct mesh_relay_child_custody_snapshot *snapshot)
{
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_relay_result_bundle_pending(relay) &&
        !relay->result_offer_reservation.valid) {
        return PROTO_ERR_NOT_FOUND;
    }

    snapshot->version = MESH_RELAY_CHILD_CUSTODY_SNAPSHOT_VERSION;
    snapshot->role = relay->role;
    snapshot->local_id = relay->local_id;
    snapshot->gateway_id = relay->gateway_id;
    snapshot->snapshot_at_ms = now_ms;
    snapshot->result_bundle = relay->result_bundle;
    snapshot->result_offer_reservation = relay->result_offer_reservation;
    snapshot->valid = true;

    ret = child_custody_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        return ret;
    }
    return PROTO_OK;
}

int mesh_relay_restore_child_custody_snapshot(
    struct mesh_relay *relay,
    const struct mesh_relay_child_custody_snapshot *snapshot,
    uint32_t now_ms)
{
    uint32_t exported_at_ms;
    uint32_t remaining_hold_ms = 1u;
    int ret;

    if (relay == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_result_bundle_pending(relay) ||
        relay->result_offer_reservation.valid) {
        return PROTO_ERR_MALFORMED;
    }

    ret = child_custody_snapshot_validate(relay, snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }

    exported_at_ms = snapshot->snapshot_at_ms;
    relay->result_bundle = snapshot->result_bundle;
    relay->result_offer_reservation = snapshot->result_offer_reservation;

    if (relay->result_bundle.active) {
        if (!deadline_reached(exported_at_ms, relay->result_bundle.due_ms)) {
            remaining_hold_ms = relay->result_bundle.due_ms - exported_at_ms;
        }
        relay->result_bundle.due_ms = now_ms + remaining_hold_ms;
        for (uint8_t i = 0u; i < relay->result_bundle.record_count; i++) {
            struct mesh_result_bundle_entry *entry = &relay->result_bundle.records[i];
            uint32_t elapsed_ms = exported_at_ms - entry->queued_at_ms;

            entry->message_age_ms = packet_age_add(entry->message_age_ms, elapsed_ms);
            entry->queued_at_ms = now_ms;
        }
    }
    return PROTO_OK;
}

void mesh_relay_cancel_tx(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->pending, 0, sizeof(relay->pending));
        outbox_record_clear(relay);
    }
}

bool mesh_relay_defer_tx(struct mesh_relay *relay, uint32_t now_ms)
{
    return preserve_pending_gateway_result(relay, now_ms);
}

int mesh_relay_start_tx(struct mesh_relay *relay,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        !id_is_unicast(packet->src_id) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&relay->pending, 0, sizeof(relay->pending));
    relay->pending.packet = *packet;
    if (payload_len > 0u) {
        memcpy(relay->pending.payload, payload, payload_len);
    }
    relay->pending.payload_len = (uint16_t)payload_len;
    relay->pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.queued_at_ms = now_ms;
    relay->pending.state = outbox_should_track_pending(relay, &relay->pending) ?
                           MESH_RELAY_TX_WAIT_GATEWAY_ACK :
                           MESH_RELAY_TX_IDLE;
    pending_set_deadlines(&relay->pending, now_ms);
    outbox_record_track_pending(relay, now_ms);
    return outbound_from_pending(relay, &relay->pending, now_ms, out);
}

int mesh_relay_start_result_offer(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint32_t now_ms,
                                  struct mesh_outbound *out)
{
    struct command_result_id result_id;
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        payload_len <= COLLECTION_RESULT_INLINE_C5_MAX_BYTES ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        !id_is_unicast(packet->src_id) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id != relay->gateway_id) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = command_result_id_from_tlvs(payload, payload_len, &result_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (result_id.gateway_id != relay->gateway_id ||
        result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        result_id.node_id != packet->src_id) {
        return PROTO_ERR_MALFORMED;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&relay->pending, 0, sizeof(relay->pending));
    relay->pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
    relay->pending.packet = *packet;
    memcpy(relay->pending.payload, payload, payload_len);
    relay->pending.payload_len = (uint16_t)payload_len;
    relay->pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    relay->pending.next_hop_id = next_hop_id;
    relay->pending.queued_at_ms = now_ms;
    relay->pending.gateway_ack_deadline_ms = now_ms + RREP_ACK_TIMEOUT_MS;
    relay->pending.result_offer_active = true;
    outbox_record_track_pending(relay, now_ms);
    return outbound_from_pending(relay, &relay->pending, now_ms, out);
}

int mesh_relay_start_channel9_tx(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const struct mesh_channel5_requirements *requirements,
                                 uint32_t now_ms,
                                 struct mesh_event_plan *plan,
                                 struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!packet_requires_channel9_payload_event(packet)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    (void)mesh_relay_expire_channel9_timings(relay, now_ms);
    ret = mesh_relay_require_channel9_tx_event(relay,
                                               next_hop_id,
                                               requirements,
                                               now_ms,
                                               plan);
    if (ret != PROTO_OK) {
        if (ret == PROTO_ERR_BUSY && channel9_plan_misses_event(plan->action)) {
            mesh_relay_note_channel9_missed(relay, next_hop_id, NULL);
        }
        return ret;
    }

    ret = mesh_relay_start_tx(relay,
                              packet,
                              payload,
                              payload_len,
                              now_ms,
                              out);
    if (ret == PROTO_OK) {
        out->radio_channel = MESH_EVENT_CHANNEL;
        if (mesh_relay_tx_active(relay) &&
            relay->pending.packet.session_id == packet->session_id &&
            relay->pending.packet.seq == packet->seq) {
            relay->pending.radio_channel = MESH_EVENT_CHANNEL;
        }
    }
    return ret;
}

void mesh_relay_note_channel9_success(struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_success(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_tx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_local_tx(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_rx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t planned_event_start_ms,
                                 uint32_t observed_packet_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_observed_packet(&relay->event_timings[index].timing,
                                        planned_event_start_ms,
                                        observed_packet_ms);
    }
}

void mesh_relay_note_channel9_missed(struct mesh_relay *relay,
                                     uint64_t next_hop_id,
                                     struct mesh_event_diagnostics *diagnostics)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_missed(&relay->event_timings[index].timing, diagnostics);
    }
}

void mesh_relay_note_tx_sent(struct mesh_relay *relay,
                             const struct mesh_outbound *out,
                             uint32_t now_ms)
{
    if (relay == NULL || out == NULL || !id_is_unicast(out->next_hop_id)) {
        return;
    }

    if (out->packet.dst_id == relay->gateway_id) {
        route_refresh_selected_at(&relay->upstream, now_ms);
    } else if (out->packet.dst_id != MESH_BROADCAST_ID) {
        refresh_downlink(relay, out->packet.dst_id, out->next_hop_id, now_ms);
    }
}

void mesh_relay_note_delivery_failure(struct mesh_relay *relay,
                                      uint64_t dst_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(dst_id) || dst_id == relay->local_id) {
        return;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role != MESH_RELAY_ROLE_GATEWAY) {
            (void)route_record_failure(&relay->upstream, ROUTE_FAILURE_GATEWAY_ACK);
        }
        return;
    }

    index = downlink_index(relay, dst_id);
    if (index >= 0) {
        relay->downlinks[index].valid = false;
    }
}

int mesh_relay_tick_with_random(struct mesh_relay *relay,
                                uint32_t now_ms,
                                uint32_t random_value,
                                struct mesh_relay_result *result)
{
    enum route_delivery_action action;
    const struct route_candidate *selected;
    uint8_t failure_count;
    uint64_t next_hop_id = 0u;
    uint32_t delay_ms;
    int ret;

    if (relay == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    (void)mesh_relay_expire_routes(relay, now_ms);
    if (!mesh_relay_tx_active(relay)) {
        if (result_bundle_ready_to_flush(&relay->result_bundle, now_ms)) {
            ret = result_bundle_build_outbound(relay, now_ms, &result->forward);
            if (ret == PROTO_OK) {
                result->actions |= MESH_RELAY_ACTION_FORWARD;
                return PROTO_OK;
            }
            relay->result_bundle.due_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
            result->status = ret;
            if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
                result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
            }
        }
        return PROTO_OK;
    }

    if (outbox_record_is_expired(relay, now_ms)) {
        outbox_record_mark_expired(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_IDLE;
        result->status = PROTO_ERR_STALE;
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) {
        if (!deadline_reached(now_ms, relay->pending.retry_after_ms)) {
            return PROTO_OK;
        }
        if (relay->pending.packet.dst_id == relay->gateway_id &&
            relay->outbox_record.valid) {
            pending_refresh_age(&relay->pending, now_ms);
            outbox_record_sync_age_from_pending(relay, now_ms);
            ret = mesh_relay_select_next_hop(relay,
                                             relay->pending.packet.dst_id,
                                             &next_hop_id);
            if (ret != PROTO_OK) {
                relay->pending.retry_after_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
                result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
                result->status = ret;
                return PROTO_OK;
            }
            relay->pending.next_hop_id = next_hop_id;
        }
        ret = outbound_from_pending(relay, &relay->pending, now_ms, &result->retransmit);
        if (ret != PROTO_OK) {
            result->status = ret;
            return ret;
        }
        relay->pending.packet.message_age_ms = result->retransmit.packet.message_age_ms;
        relay->pending.queued_at_ms = now_ms;
        outbox_record_sync_age_from_pending(relay, now_ms);
        if (relay->pending.result_offer_active) {
            relay->pending.state = MESH_RELAY_TX_WAIT_RESULT_GRANT;
            relay->pending.gateway_ack_deadline_ms = now_ms + RREP_ACK_TIMEOUT_MS;
        } else {
            relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
            pending_set_deadlines(&relay->pending, now_ms);
        }
        result->actions |= MESH_RELAY_ACTION_RETRANSMIT;
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.retry_after_ms = now_ms + RELAY_BUSY_RETRY_MIN_MS;
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
        if (pending_is_local_collection_result(relay, &relay->pending)) {
            ret = schedule_pending_collection_retry(relay, now_ms, result);
            if (ret != PROTO_OK) {
                result->status = ret;
                return ret;
            }
            return PROTO_OK;
        }

        pending_refresh_age(&relay->pending, now_ms);
        outbox_record_sync_age_from_pending(relay, now_ms);
        action = route_record_failure_at(&relay->upstream,
                                         ROUTE_FAILURE_GATEWAY_ACK,
                                         now_ms);
        if (action == ROUTE_DELIVERY_DISCOVER ||
            mesh_relay_select_next_hop(relay, relay->pending.packet.dst_id, &next_hop_id) != PROTO_OK) {
            if (preserve_pending_gateway_result(relay, now_ms)) {
                result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
                result->status = PROTO_ERR_NOT_FOUND;
                return PROTO_OK;
            }
            relay->pending.state = MESH_RELAY_TX_IDLE;
            outbox_record_clear(relay);
            result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
            result->status = PROTO_ERR_NOT_FOUND;
            return PROTO_OK;
        }
        selected = route_selected(&relay->upstream);
        if (action == ROUTE_DELIVERY_TRY_ALTERNATE) {
            delay_ms = 0u;
        } else {
            failure_count = selected == NULL ? ROUTE_RETRIES_PER_CANDIDATE :
                            selected->failure_count;
            delay_ms = mesh_relay_retry_backoff_ms(failure_count, random_value);
        }
        relay->pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
        relay->pending.next_hop_id = next_hop_id;
        relay->pending.retry_after_ms = now_ms + delay_ms;
    }

    return PROTO_OK;
}

int mesh_relay_tick(struct mesh_relay *relay,
                    uint32_t now_ms,
                    struct mesh_relay_result *result)
{
    return mesh_relay_tick_with_random(relay, now_ms, 0u, result);
}

int mesh_relay_handle_rx(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t previous_hop_id,
                         uint8_t link_quality,
                         uint32_t now_ms,
                         struct mesh_relay_result *result)
{
    return mesh_relay_handle_rx_with_random(relay,
                                            packet,
                                            payload,
                                            payload_len,
                                            previous_hop_id,
                                            link_quality,
                                            now_ms,
                                            0u,
                                            result);
}

int mesh_relay_handle_rx_with_random(struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t previous_hop_id,
                                     uint8_t link_quality,
                                     uint32_t now_ms,
                                     uint32_t random_value,
                                     struct mesh_relay_result *result)
{
    bool duplicate;
    int ret;

    if (relay == NULL || packet == NULL || result == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(packet->src_id) ||
        packet->src_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    (void)mesh_relay_expire_routes(relay, now_ms);

    if ((packet->msg_type == MSG_GATEWAY_ACK || packet->msg_type == MSG_MESH_HOP_ACK) &&
        packet->dst_id == relay->local_id) {
        (void)handle_local_ack(relay, packet, payload, payload_len, now_ms, result);
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REPLY_ACK) {
        ret = handle_route_reply_ack(relay,
                                     packet,
                                     payload,
                                     payload_len,
                                     previous_hop_id,
                                     result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if ((packet->msg_type == MSG_RELAY_BUSY || packet->msg_type == MSG_RESULT_BUSY) &&
        packet->dst_id == relay->local_id) {
        ret = handle_local_busy(relay,
                                packet,
                                payload,
                                payload_len,
                                previous_hop_id,
                                now_ms,
                                result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_RESULT_OFFER && packet->dst_id == relay->local_id) {
        ret = handle_result_offer(relay,
                                  packet,
                                  payload,
                                  payload_len,
                                  previous_hop_id,
                                  result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_RESULT_GRANT && packet->dst_id == relay->local_id) {
        ret = handle_result_grant(relay,
                                  packet,
                                  payload,
                                  payload_len,
                                  previous_hop_id,
                                  now_ms,
                                  result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        }
        return PROTO_OK;
    }

    duplicate = duplicate_seen(relay, packet, now_ms);
    if (duplicate) {
        if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {
            add_route_reply_ack_action(relay, packet, payload, payload_len, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (packet->dst_id == relay->local_id) {
            (void)add_gateway_ack_action(relay, packet, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (packet->msg_type == MSG_ROUTE_REQ &&
            packet->dst_id == MESH_BROADCAST_ID) {
            ret = handle_route_request(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       link_quality,
                                       now_ms,
                                       random_value,
                                       result,
                                       true);
            if (ret != PROTO_OK) {
                result->status = ret;
                result->actions |= MESH_RELAY_ACTION_DROP;
            }
            return PROTO_OK;
        }
        if (packet->dst_id == MESH_BROADCAST_ID) {
            relay_diag_inc_u8(&relay->diagnostics.flood_suppression_count);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        if (mesh_relay_tx_active(relay)) {
            add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            return PROTO_OK;
        }

        ret = build_forward(relay, packet, payload, payload_len, &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
            add_hop_ack_action(relay, packet, previous_hop_id, result);
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        result->actions |= MESH_RELAY_ACTION_DROP;
        result->status = ret;
        return PROTO_OK;
    }

    if (packet->msg_type != MSG_ROUTE_REQ &&
        packet->msg_type != MSG_ROUTE_REPLY &&
        (packet_needs_forward(relay, packet) || local_delivery_needs_response(relay, packet)) &&
        mesh_relay_tx_active(relay)) {
        add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
        result->status = PROTO_ERR_BUSY;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == LEGACY_MSG_ROUTE_ADV ||
        packet->msg_type == LEGACY_MSG_ROUTE_STATUS) {
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REQ && packet->dst_id == MESH_BROADCAST_ID) {
        ret = handle_route_request(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   link_quality,
                                   now_ms,
                                   random_value,
                                   result,
                                   false);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {
        ret = handle_route_reply(relay,
                                 packet,
                                 payload,
                                 payload_len,
                                 previous_hop_id,
                                 link_quality,
                                 now_ms,
                                 result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV && packet->dst_id == MESH_BROADCAST_ID) {
        ret = handle_gateway_route_adv(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       link_quality,
                                       now_ms,
                                       random_value,
                                       result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
        packet->dst_id == MESH_BROADCAST_ID) {
        if (!collection_eack_broadcast_valid(relay, packet, payload, payload_len)) {
            result->status = PROTO_ERR_MALFORMED;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        ret = handle_collection_eack_for_pending(relay,
                                                 payload,
                                                 payload_len,
                                                 now_ms,
                                                 result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
    }

    if (packet->dst_id == relay->local_id) {
        duplicate_store(relay, packet, now_ms);
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        (void)add_gateway_ack_action(relay, packet, previous_hop_id, result);
        return PROTO_OK;
    }

    if (packet->dst_id == MESH_BROADCAST_ID) {
        duplicate_store(relay, packet, now_ms);
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        ret = build_broadcast_forward(relay,
                                      packet,
                                      payload,
                                      payload_len,
                                      now_ms,
                                      random_value,
                                      &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_COMMAND_RESULT) {
        struct command_result_id result_id;
        uint32_t collection_epoch_id = 0u;
        bool reservation_applies =
            relay->result_offer_reservation.valid &&
            relay->result_offer_reservation.child_id == packet->src_id;

        ret = result_offer_reservation_matches_payload(relay, packet, payload, payload_len);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
            return PROTO_OK;
        }
        if (command_result_can_bundle(relay,
                                      packet,
                                      payload,
                                      payload_len,
                                      &result_id,
                                      &collection_epoch_id)) {
            ret = result_bundle_enqueue_command_result(relay,
                                                       packet,
                                                       payload,
                                                       payload_len,
                                                       &result_id,
                                                       collection_epoch_id,
                                                       now_ms);
            if (ret == PROTO_OK || ret == PROTO_ERR_STALE) {
                duplicate_store(relay, packet, now_ms);
                if (reservation_applies) {
                    result_offer_reservation_clear(relay);
                }
                add_hop_ack_action(relay, packet, previous_hop_id, result);
                result->actions |= MESH_RELAY_ACTION_CUSTODY_ACCEPTED;
                result->status = ret;
                if (result_bundle_ready_to_flush(&relay->result_bundle, now_ms)) {
                    ret = result_bundle_build_outbound(relay, now_ms, &result->forward);
                    if (ret == PROTO_OK) {
                        result->actions |= MESH_RELAY_ACTION_FORWARD;
                        result->status = PROTO_OK;
                    } else {
                        result->status = ret;
                    }
                }
                return PROTO_OK;
            }
        }
    }

    ret = build_forward(relay, packet, payload, payload_len, &result->forward);
    if (ret == PROTO_OK) {
        duplicate_store(relay, packet, now_ms);
        if (relay->result_offer_reservation.valid &&
            relay->result_offer_reservation.child_id == packet->src_id) {
            result_offer_reservation_clear(relay);
        }
        result->actions |= MESH_RELAY_ACTION_FORWARD;
        add_hop_ack_action(relay, packet, previous_hop_id, result);
        return PROTO_OK;
    }

    result->status = ret;
    result->actions |= MESH_RELAY_ACTION_DROP;
    return PROTO_OK;
}
