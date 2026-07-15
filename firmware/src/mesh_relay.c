#include "mesh_relay.h"

#include "mesh_relay_channel9.h"

#include "mesh.h"

#include <string.h>

_Static_assert(sizeof(struct mesh_relay_result) <=
               (3u * sizeof(struct mesh_outbound)) + 32u,
               "relay result must retain only three simultaneous outbound buffers");

#define LEGACY_MSG_ROUTE_ADV 0x33u
#define LEGACY_MSG_ROUTE_STATUS 0x34u
#define MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION 1u
#define FLOOD_BETTER_METRIC_MARGIN_PERCENT 20u

static bool gateway_delivery_requires_commit(
    const struct mesh_relay *relay,
    const struct proto_packet *packet);

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

_Static_assert(MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN <=
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "gateway route advertisement exceeds mesh payload capacity");

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
                                      uint8_t retry_round,
                                      uint32_t random_value)
{
    uint32_t seed = command_seq ^ collection_epoch_id ^ random_value;

    seed ^= (uint32_t)node_id;
    seed ^= (uint32_t)(node_id >> 32);
    seed ^= (uint32_t)retry_round << 24;
    return mix32(seed);
}

static uint32_t result_offer_retry_base_ms(uint8_t actual_attempt_count)
{
    uint32_t base_ms = MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS;
    uint8_t completed_retries = actual_attempt_count > 0u ?
                                (uint8_t)(actual_attempt_count - 1u) : 0u;

    while (completed_retries > 0u &&
           base_ms < MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS) {
        if (base_ms > MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS / 2u) {
            base_ms = MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS;
        } else {
            base_ms *= 2u;
        }
        completed_retries--;
    }
    return base_ms;
}

static uint32_t result_offer_retry_seed(
    const struct command_result_id *result_id,
    const struct proto_packet *packet,
    uint8_t actual_attempt_count,
    uint32_t random_value)
{
    uint32_t seed = mix32(random_value ^ packet->session_id);

    seed = mix32(seed ^ ((uint32_t)packet->seq << 16) ^ packet->msg_type);
    seed = mix32(seed ^ (uint32_t)result_id->gateway_id);
    seed = mix32(seed ^ (uint32_t)(result_id->gateway_id >> 32));
    seed = mix32(seed ^ result_id->gateway_epoch ^ result_id->command_seq);
    seed = mix32(seed ^ (uint32_t)result_id->node_id);
    seed = mix32(seed ^ (uint32_t)(result_id->node_id >> 32));
    seed = mix32(seed ^ result_id->node_boot_counter ^ result_id->result_seq);
    return mix32(seed ^ ((uint32_t)actual_attempt_count << 24));
}

static int result_offer_retry_delay_ms(const struct mesh_relay *relay,
                                       uint32_t random_value,
                                       uint32_t *delay_ms)
{
    struct command_result_id result_id;
    uint32_t base_ms;
    uint32_t seed;
    int ret;

    if (relay == NULL || delay_ms == NULL ||
        !relay->pending.result_offer_active ||
        relay->pending.packet.msg_type != MSG_COMMAND_RESULT) {
        return PROTO_ERR_ARG;
    }
    ret = command_result_id_from_tlvs(relay->pending.payload,
                                      relay->pending.payload_len,
                                      &result_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    base_ms = result_offer_retry_base_ms(relay->outbox_record.retry_round);
    seed = result_offer_retry_seed(&result_id,
                                   &relay->pending.packet,
                                   relay->outbox_record.retry_round,
                                   random_value);
    *delay_ms = retry_jittered_delay_ms(base_ms, seed);
    return PROTO_OK;
}

static uint32_t pending_busy_retry_delay_ms(struct mesh_relay *relay,
                                            uint32_t minimum_delay_ms,
                                            uint32_t random_value)
{
    uint32_t base_ms = minimum_delay_ms < RELAY_BUSY_RETRY_MIN_MS ?
                       RELAY_BUSY_RETRY_MIN_MS : minimum_delay_ms;
    uint32_t seed;
    uint8_t next_round;

    if (relay == NULL) {
        return RELAY_BUSY_RETRY_MIN_MS;
    }
    next_round = relay->pending.busy_retry_round == UINT8_MAX ? UINT8_MAX :
                 (uint8_t)(relay->pending.busy_retry_round + 1u);
    for (uint8_t round = 1u;
         round < next_round && base_ms < RELAY_BUSY_RETRY_MAX_MS;
         round++) {
        if (base_ms > RELAY_BUSY_RETRY_MAX_MS / 2u) {
            base_ms = RELAY_BUSY_RETRY_MAX_MS;
        } else {
            base_ms *= 2u;
        }
    }
    if (base_ms > RELAY_BUSY_RETRY_MAX_MS) {
        base_ms = RELAY_BUSY_RETRY_MAX_MS;
    }

    seed = mix32(random_value ^ relay->pending.packet.session_id ^
                 ((uint32_t)relay->pending.packet.seq << 16) ^
                 (uint32_t)relay->pending.packet.src_id ^
                 (uint32_t)(relay->pending.packet.src_id >> 32) ^
                 ((uint32_t)next_round << 24));
    relay->pending.busy_retry_round = next_round;
    return retry_jittered_delay_ms(base_ms, seed);
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

static uint32_t route_reply_earliest_tx_ms(uint32_t now_ms,
                                           uint16_t reply_delay_ms,
                                           uint32_t random_value)
{
    uint32_t slot = random_value % RREP_RESPONDER_SLOT_COUNT;
    uint32_t jitter_ms = slot * RREP_RESPONDER_SLOT_MS;

    return now_ms + (uint32_t)reply_delay_ms + jitter_ms;
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

uint16_t mesh_route_reply_nonce(uint64_t origin_id,
                                uint64_t target_id,
                                uint32_t session_id,
                                uint32_t flood_epoch_id)
{
    uint32_t mix = session_id ^ flood_epoch_id;

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

static bool downlink_matches_discovery(const struct mesh_downlink_entry *entry,
                                       uint64_t origin_id,
                                       uint64_t target_id,
                                       uint32_t session_id,
                                       uint32_t flood_epoch_id,
                                       uint16_t reply_nonce)
{
    return entry->valid &&
           entry->target_id == origin_id &&
           entry->gateway_id == target_id &&
           entry->discovery_session_id == session_id &&
           entry->discovery_flood_epoch_id == flood_epoch_id &&
           reply_nonce == mesh_route_reply_nonce(origin_id,
                                                 target_id,
                                                 session_id,
                                                 flood_epoch_id);
}

static const struct mesh_downlink_entry *downlink_for_discovery(
    const struct mesh_relay *relay,
    uint64_t origin_id,
    uint64_t target_id,
    uint32_t session_id,
    uint32_t flood_epoch_id,
    uint16_t reply_nonce)
{
    const struct mesh_downlink_entry *selected = NULL;

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (!downlink_matches_discovery(entry,
                                        origin_id,
                                        target_id,
                                        session_id,
                                        flood_epoch_id,
                                        reply_nonce)) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
        }
    }
    return selected;
}

static const struct mesh_downlink_entry *downlink_backup_for_discovery(
    const struct mesh_relay *relay,
    uint64_t origin_id,
    uint64_t target_id,
    uint32_t session_id,
    uint32_t flood_epoch_id,
    uint16_t reply_nonce,
    uint64_t primary_next_hop)
{
    const struct mesh_downlink_entry *selected = NULL;

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (!downlink_matches_discovery(entry,
                                        origin_id,
                                        target_id,
                                        session_id,
                                        flood_epoch_id,
                                        reply_nonce) ||
            entry->next_hop_id == primary_next_hop) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
        }
    }
    return selected;
}

static bool downlink_same_key(const struct mesh_downlink_entry *a,
                              const struct mesh_downlink_entry *b)
{
    return a->target_id == b->target_id &&
           a->next_hop_id == b->next_hop_id &&
           a->gateway_id == b->gateway_id &&
           a->discovery_flood_epoch_id == b->discovery_flood_epoch_id &&
           (a->discovery_flood_epoch_id == 0u ||
            a->discovery_session_id == b->discovery_session_id);
}

static int downlink_exact_index(const struct mesh_relay *relay,
                                const struct mesh_downlink_entry *candidate)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid && downlink_same_key(entry, candidate)) {
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
    bool discovery_scoped;
    int index;
    int free_index = -1;
    int replace_index = 0;
    const struct mesh_downlink_entry *replace = NULL;

    discovery_scoped = entry->discovery_flood_epoch_id != 0u;
    if (!id_is_unicast(entry->target_id) ||
        !id_is_unicast(entry->next_hop_id) ||
        !id_is_unicast(entry->gateway_id) ||
        entry->target_id == relay->local_id ||
        entry->quality > 100u ||
        (discovery_scoped &&
         (entry->gateway_id == entry->target_id ||
          entry->discovery_session_id == 0u))) {
        return PROTO_ERR_MALFORMED;
    }

    index = downlink_exact_index(relay, entry);
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

static int upsert_required_gateway_downlink(
    struct mesh_relay *relay,
    const struct mesh_downlink_entry *entry)
{
    int replace_index = -1;
    int ret = upsert_downlink(relay, entry);

    if (ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        if (!relay->downlinks[i].valid) {
            replace_index = (int)i;
            break;
        }
        if (replace_index < 0 ||
            downlink_is_better(&relay->downlinks[replace_index],
                               &relay->downlinks[i])) {
            replace_index = (int)i;
        }
    }
    if (replace_index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    relay->downlinks[replace_index] = *entry;
    relay->downlinks[replace_index].valid = true;
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
                           const uint8_t *payload,
                           size_t payload_len,
                           uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;
    bool require_payload_identity;
    uint16_t payload_crc = 0u;

    if (!duplicate_tracked(packet)) {
        return false;
    }

    require_payload_identity = gateway_delivery_requires_commit(relay, packet);
    if (require_payload_identity) {
        payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    }
    duplicate_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (duplicate_matches_packet(entry, packet)) {
            if (require_payload_identity &&
                (!entry->payload_identity_valid ||
                 entry->payload_len != payload_len ||
                 entry->payload_crc != payload_crc)) {
                continue;
            }
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
    entry->payload_len = 0u;
    entry->payload_crc = 0u;
    entry->payload_identity_valid = false;
    entry->valid = true;
    relay->duplicate_next = (uint8_t)((relay->duplicate_next + 1u) % MESH_RELAY_DUP_CACHE_SIZE);
}

static void duplicate_store_payload_identity(struct mesh_relay *relay,
                                             const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry = NULL;

    if (!duplicate_tracked(packet)) {
        return;
    }

    duplicate_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        if (duplicate_matches_packet(&relay->duplicates[i], packet)) {
            entry = &relay->duplicates[i];
            break;
        }
    }
    if (entry == NULL) {
        entry = &relay->duplicates[relay->duplicate_next];
        relay->duplicate_next = (uint8_t)((relay->duplicate_next + 1u) %
                                          MESH_RELAY_DUP_CACHE_SIZE);
    }

    entry->msg_type = packet->msg_type;
    entry->src_id = packet->src_id;
    entry->dst_id = packet->dst_id;
    entry->session_id = packet->session_id;
    entry->last_seen_ms = now_ms;
    entry->seq = packet->seq;
    entry->payload_len = (uint16_t)payload_len;
    entry->payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    entry->payload_identity_valid = true;
    entry->valid = true;
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
    bool *first_seen,
    bool *best_updated)
{
    struct flood_seen_entry *entry;
    uint16_t metric;

    if (first_seen != NULL) {
        *first_seen = false;
    }
    if (best_updated != NULL) {
        *best_updated = false;
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
        if (best_updated != NULL) {
            *best_updated = true;
        }
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

static int build_route_request_forward(uint64_t local_id,
                                       const struct proto_packet *packet,
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
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(local_id,
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


/* Implementation is split by responsibility but remains one translation unit. */
#include "mesh_relay_custody.inc"
#include "mesh_relay_route_rx.inc"
#include "mesh_relay_routes.inc"
#include "mesh_relay_delivery.inc"
