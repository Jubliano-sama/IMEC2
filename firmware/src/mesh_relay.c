#include "mesh_relay.h"

#include "mesh_relay_channel9.h"

#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "semantic_digest.h"

#include <string.h>

#ifndef IMEC_MESH_RELAY_GATEWAY_ONLY
#define IMEC_MESH_RELAY_GATEWAY_ONLY 0
#endif

#if IMEC_MESH_RELAY_GATEWAY_ONLY != 0 && IMEC_MESH_RELAY_GATEWAY_ONLY != 1
#error "IMEC_MESH_RELAY_GATEWAY_ONLY must be 0 or 1"
#endif

_Static_assert(sizeof(struct mesh_relay_result) <=
               (3u * sizeof(struct mesh_outbound)) +
                   sizeof(struct operation_policy_set) + 48u,
               "relay result must retain only three simultaneous outbound buffers");
_Static_assert(DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S * 1000u +
                   MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS + 1000u <=
                   INT32_MAX,
               "bounded gateway ACK history must remain wrap-safe");
_Static_assert(MESH_RELAY_RESULT_OFFER_EXPIRY_S <= INT32_MAX / 1000u,
               "result-offer reservation expiry must remain wrap-safe");
_Static_assert(MESH_RELAY_GATEWAY_ACK_CONFIRM_REPLAY_MS +
                   MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS <=
                   INT32_MAX,
               "gateway ACK-confirm recovery must remain wrap-safe");

#define LEGACY_MSG_ROUTE_STATUS 0x34u
#define MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION 1u
#define FLOOD_BETTER_METRIC_MARGIN_PERCENT 20u
#define GATEWAY_ACK_HISTORY_OWNER_MASK 0x3fu
#define GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY 0x40u
#define GATEWAY_ACK_HISTORY_BATCHED 0x80u
#define GATEWAY_ACK_HISTORY_TRANSITION_MSG_TYPE 0u

_Static_assert(MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX <=
                   GATEWAY_ACK_HISTORY_OWNER_MASK,
               "gateway ACK owner encoding must cover every anchor");

static bool gateway_delivery_requires_commit(
    const struct mesh_relay *relay,
    const struct proto_packet *packet);
static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms);

/*
 * Relay retries may decrement TTL and increase message age, so neither field
 * is part of semantic equality. All operation-defining header fields and the
 * complete payload are committed under a versioned domain.
 */
static bool semantic_packet_digest_under_domain(
    const uint8_t *domain,
    size_t domain_len,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct semantic_digest_sha256_context context;
    uint8_t header[1u + 1u + sizeof(uint64_t) + sizeof(uint64_t) +
                   sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t)];
    size_t offset = 0u;

    if (domain == NULL || domain_len == 0u ||
        packet == NULL || digest == NULL ||
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
                                         domain_len) &&
           semantic_digest_sha256_update(&context, header, sizeof(header)) &&
           semantic_digest_sha256_update(&context, payload, payload_len) &&
           semantic_digest_sha256_final(&context, digest);
}

static bool relay_semantic_packet_digest(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return mesh_packet_semantic_digest(packet,
                                       payload,
                                       payload_len,
                                       digest);
}

static bool route_reply_semantic_commitment(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    static const uint8_t domain[] = "mesh-route-reply-v1";
    struct proto_packet semantic_packet;

    if (packet == NULL || packet->msg_type != MSG_ROUTE_REPLY ||
        payload_len > UINT16_MAX) {
        return false;
    }

    semantic_packet = *packet;
    semantic_packet.payload_len = (uint16_t)payload_len;
    return semantic_packet_digest_under_domain(domain,
                                               sizeof(domain) - 1u,
                                               &semantic_packet,
                                               payload,
                                               payload_len,
                                               digest);
}

struct route_discovery_fields {
    uint64_t origin_id;
    uint64_t target_id;
    struct mesh_route_path path;
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
    uint8_t reply_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    bool proposed_channel9_timing_valid;
    bool reply_commitment_present;
};

struct route_reply_ack_fields {
    uint8_t reply_commitment[SEMANTIC_DIGEST_SHA256_LEN];
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
    struct mesh_route_path path;
    uint32_t gateway_route_seq;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;
    uint32_t random_backoff_max_ms;
    uint16_t gateway_epoch;
    uint16_t route_cost;
    uint16_t flood_profile_version;
    uint16_t capacity_validity_interval_ms;
    uint16_t random_backoff_slot_ms;
    uint8_t hop_count;
    uint8_t path_quality_min;
    uint8_t gateway_capacity_state;
    uint8_t flood_retry_count;
    struct operation_policy_set operation_policy;
    uint8_t operation_policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
    uint8_t operation_policy_tlvs_len;
    bool operation_policy_present;
};

static int route_epoch_select_newer(uint32_t a,
                                    uint32_t b,
                                    uint32_t *selected)
{
    if (selected == NULL || (uint16_t)a == 0u || (uint16_t)b == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (a == b || route_epoch_strictly_newer(a, b)) {
        *selected = a;
        return PROTO_OK;
    }
    if (route_epoch_strictly_newer(b, a)) {
        *selected = b;
        return PROTO_OK;
    }
    return PROTO_ERR_STALE;
}

struct downlink_upsert_rollback {
    struct mesh_downlink_entry previous;
    uint8_t index;
    bool valid;
};

enum reactive_route_rollback_kind {
    REACTIVE_ROUTE_ROLLBACK_NONE = 0,
    REACTIVE_ROUTE_ROLLBACK_UPSTREAM = 1,
    REACTIVE_ROUTE_ROLLBACK_DOWNLINK = 2,
    REACTIVE_ROUTE_ROLLBACK_UPSTREAM_EPOCH_COMMITTED = 3,
};

struct reactive_route_rollback {
    enum reactive_route_rollback_kind kind;
    uint32_t gateway_route_adv_seq;
    union {
        struct route_table upstream;
        struct downlink_upsert_rollback downlink;
    } state;
};

_Static_assert(MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN <=
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "gateway route advertisement exceeds mesh payload capacity");
_Static_assert(MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN <=
                   PACKET_MAX_PAYLOAD_LEN,
               "maximum-depth gateway route path exceeds mesh payload capacity");

static int mesh_relay_operation_policy_set_validate_complete(
    const struct operation_policy_set *set)
{
    struct operation_policy policy;
    if (set == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!set->assignment_present) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&policy, 0, sizeof(policy));
    policy.family = OPERATION_POLICY_FAMILY_ASSIGNMENT;
    policy.value.assignment = set->assignment;
    return operation_policy_validate(&policy);
}

static int mesh_relay_operation_policy_encode_complete(
    const struct operation_policy_set *set,
    uint8_t *encoded,
    uint8_t *encoded_len)
{
    struct operation_policy policy;
    uint8_t local[OPERATION_POLICY_ALL_TLVS_LEN] = {0};
    size_t offset = 0u;
    int ret;

    if (encoded == NULL || encoded_len == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_relay_operation_policy_set_validate_complete(set);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&policy, 0, sizeof(policy));
    policy.family = OPERATION_POLICY_FAMILY_ASSIGNMENT;
    policy.value.assignment = set->assignment;
    ret = operation_policy_append_tlv(local, sizeof(local), &offset, &policy);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (offset != sizeof(local)) {
        return PROTO_ERR_MALFORMED;
    }

    memcpy(encoded, local, sizeof(local));
    *encoded_len = (uint8_t)sizeof(local);
    return PROTO_OK;
}

static int mesh_relay_operation_policy_parse_exact(
    const uint8_t *payload,
    size_t payload_len,
    struct operation_policy_set *set,
    uint8_t *encoded,
    uint8_t *encoded_len,
    bool *present)
{
    struct operation_policy_set parsed;
    uint8_t local[OPERATION_POLICY_ALL_TLVS_LEN] = {0};
    size_t local_len = 0u;
    size_t offset = 0u;
    int ret;

    if ((payload == NULL && payload_len != 0u) || set == NULL ||
        encoded == NULL || encoded_len == NULL || present == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = operation_policy_set_from_tlvs(payload, payload_len, &parsed);
    if (ret != PROTO_OK) {
        return ret;
    }
    while (offset < payload_len) {
        size_t tlv_offset = offset;
        uint8_t type;
        uint8_t len;
        size_t tlv_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (payload_len - offset < len) {
            return PROTO_ERR_MALFORMED;
        }
        tlv_len = PROTO_TLV_HEADER_LEN + len;
        if (type == TLV_OPERATION_POLICY) {
            if (local_len + tlv_len > sizeof(local)) {
                return PROTO_ERR_MALFORMED;
            }
            memcpy(&local[local_len], &payload[tlv_offset], tlv_len);
            local_len += tlv_len;
        }
        offset += len;
    }

    if (local_len == 0u) {
        *set = parsed;
        memset(encoded, 0, OPERATION_POLICY_ALL_TLVS_LEN);
        *encoded_len = 0u;
        *present = false;
        return PROTO_OK;
    }
    if (local_len != sizeof(local)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_operation_policy_set_validate_complete(&parsed);
    if (ret != PROTO_OK) {
        return ret;
    }

    *set = parsed;
    memcpy(encoded, local, sizeof(local));
    *encoded_len = (uint8_t)sizeof(local);
    *present = true;
    return PROTO_OK;
}

static int mesh_relay_operation_policy_append_exact(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint8_t *encoded,
    uint8_t encoded_len,
    bool present)
{
    if (payload == NULL || offset == NULL || encoded == NULL ||
        *offset > payload_cap) {
        return PROTO_ERR_ARG;
    }
    if (!present) {
        return encoded_len == 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
    }
    if (encoded_len != OPERATION_POLICY_ALL_TLVS_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    if (payload_cap - *offset < encoded_len) {
        return PROTO_ERR_NO_SPACE;
    }

    memcpy(&payload[*offset], encoded, encoded_len);
    *offset += encoded_len;
    return PROTO_OK;
}

struct flood_control_fields {
    uint32_t random_backoff_max_ms;
    uint16_t random_backoff_slot_ms;
    uint8_t retry_count;
};

static bool id_is_unicast(uint64_t id)
{
    return id != MESH_BROADCAST_ID;
}

static int upstream_candidate_index(const struct mesh_relay *relay,
                                    const struct route_candidate *candidate)
{
    if (relay == NULL || candidate == NULL) {
        return -1;
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (&relay->upstream.candidates[i] == candidate) {
            return (int)i;
        }
    }
    return -1;
}

static const struct route_candidate *find_upstream_candidate(
    const struct mesh_relay *relay,
    uint64_t next_hop_id,
    uint32_t route_epoch)
{
    if (relay == NULL) {
        return NULL;
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate =
            &relay->upstream.candidates[i];

        if (candidate->valid && candidate->gateway_id == relay->gateway_id &&
            candidate->next_hop_id == next_hop_id &&
            candidate->route_epoch == route_epoch) {
            return candidate;
        }
    }
    return NULL;
}

static const struct mesh_route_path *upstream_candidate_ancestry(
    const struct mesh_relay *relay,
    const struct route_candidate *candidate)
{
    const struct mesh_upstream_ancestry_entry *entry;
    int index = upstream_candidate_index(relay, candidate);

    if (index < 0 || relay->role != MESH_RELAY_ROLE_ANCHOR ||
        relay->anchor_downlink_store == NULL || !candidate->valid ||
        candidate->gateway_id != relay->gateway_id ||
        candidate->hop_count > MESH_ROUTE_PATH_MAX_NODES - 2u) {
        return NULL;
    }
    entry = &relay->anchor_downlink_store->upstream_ancestry[index];
    if (!entry->valid || entry->next_hop_id != candidate->next_hop_id ||
        entry->route_epoch != candidate->route_epoch ||
        entry->path.count != (uint8_t)(candidate->hop_count + 2u) ||
        mesh_route_path_validate(&entry->path,
                                 relay->gateway_id,
                                 relay->local_id) != PROTO_OK) {
        return NULL;
    }
    return &entry->path;
}

static int store_upstream_candidate_ancestry(
    struct mesh_relay *relay,
    const struct route_candidate *candidate,
    const struct mesh_route_path *path,
    struct mesh_upstream_ancestry_entry *previous)
{
    struct mesh_upstream_ancestry_entry *entry;
    int index = upstream_candidate_index(relay, candidate);

    if (index < 0 || path == NULL || !candidate->valid ||
        candidate->gateway_id != relay->gateway_id ||
        candidate->hop_count > MESH_ROUTE_PATH_MAX_NODES - 2u ||
        path->count != (uint8_t)(candidate->hop_count + 2u) ||
        mesh_route_path_validate(path,
                                 relay->gateway_id,
                                 relay->local_id) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    /*
     * Only production anchors relay for other nodes and therefore retain the
     * ancestry sidecar. Clickers still install routes for their own traffic;
     * a test or compatibility caller without the anchor sidecar remains safe
     * because it cannot later advertise that candidate as an upstream path.
     */
    if (relay->role != MESH_RELAY_ROLE_ANCHOR ||
        relay->anchor_downlink_store == NULL) {
        if (previous != NULL) {
            memset(previous, 0, sizeof(*previous));
        }
        return PROTO_OK;
    }

    entry = &relay->anchor_downlink_store->upstream_ancestry[index];
    if (previous != NULL) {
        *previous = *entry;
    }
    memset(entry, 0, sizeof(*entry));
    entry->path = *path;
    entry->next_hop_id = candidate->next_hop_id;
    entry->route_epoch = candidate->route_epoch;
    entry->valid = true;
    return PROTO_OK;
}

static void restore_upstream_candidate_ancestry(
    struct mesh_relay *relay,
    const struct route_candidate *candidate,
    const struct mesh_upstream_ancestry_entry *previous)
{
    int index = upstream_candidate_index(relay, candidate);

    if (index >= 0 && previous != NULL &&
        relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay->anchor_downlink_store != NULL) {
        relay->anchor_downlink_store->upstream_ancestry[index] = *previous;
    }
}

static void clear_upstream_candidate_ancestry_at(struct mesh_relay *relay,
                                                 uint8_t index)
{
    if (relay != NULL && relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay->anchor_downlink_store != NULL &&
        index < ROUTE_MAX_CANDIDATES) {
        memset(&relay->anchor_downlink_store->upstream_ancestry[index],
               0,
               sizeof(relay->anchor_downlink_store->upstream_ancestry[index]));
    }
}

static void clear_all_upstream_candidate_ancestry(struct mesh_relay *relay)
{
    if (relay != NULL && relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay->anchor_downlink_store != NULL) {
        memset(relay->anchor_downlink_store->upstream_ancestry,
               0,
               sizeof(relay->anchor_downlink_store->upstream_ancestry));
    }
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
    if (previous_attempts == 3u) {
        return 6u;
    }
    return MESH_NETWORK_MAX_HOPS;
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
    uint8_t metric[43u + MESH_ROUTE_PATH_MAX_VALUE_BYTES];
    size_t offset = 0u;
    uint16_t crc;

    if (fields == NULL || fields->path.count == 0u ||
        fields->path.count > MESH_ROUTE_PATH_MAX_NODES) {
        return 1u;
    }

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
    metric[offset++] = fields->path.count;
    for (uint8_t i = 0u; i < fields->path.count; i++) {
        proto_put_u64_le(&metric[offset], fields->path.node_ids[i]);
        offset += sizeof(uint64_t);
    }

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
    ret = mesh_route_path_append_tlv(payload,
                                     payload_cap,
                                     offset,
                                     &fields->path);
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

    ret = tlv_find_unique(payload, payload_len, type, &tlv_value, &tlv_len);
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
                                     const struct flood_control_fields *control)
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
    return PROTO_OK;
}

static int ensure_flood_control_tlvs(struct mesh_outbound *out,
                                     const struct flood_control_fields *control)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    size_t offset;
    int ret;

    if (out == NULL || control == NULL) {
        return PROTO_ERR_ARG;
    }

    offset = out->payload_len;
    ret = tlv_find_unique(out->payload,
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

    ret = tlv_find_unique(out->payload,
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

    ret = tlv_find_unique(out->payload,
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
    if (tlv_find_unique(out->payload,
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

    ret = tlv_find_unique(out->payload,
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

size_t mesh_relay_downlink_capacity(const struct mesh_relay *relay)
{
    if (relay != NULL && relay->role == MESH_RELAY_ROLE_CLICKER) {
        return 0u;
    }
    if (relay != NULL && relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay->anchor_downlink_store != NULL) {
        return MESH_RELAY_ANCHOR_DOWNLINK_ROUTES;
    }
    return MESH_RELAY_DOWNLINK_ROUTES;
}

const struct mesh_downlink_entry *mesh_relay_downlink_at(
    const struct mesh_relay *relay,
    size_t index)
{
    if (relay == NULL || index >= mesh_relay_downlink_capacity(relay)) {
        return NULL;
    }
    if (index < MESH_RELAY_DOWNLINK_ROUTES) {
        return &relay->downlinks[index];
    }
    return &relay->anchor_downlink_store->entries[
        index - MESH_RELAY_DOWNLINK_ROUTES];
}

static struct mesh_downlink_entry *downlink_at_mutable(
    struct mesh_relay *relay,
    size_t index)
{
    return (struct mesh_downlink_entry *)mesh_relay_downlink_at(relay, index);
}

static void clear_all_downlinks(struct mesh_relay *relay)
{
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        memset(downlink_at_mutable(relay, i), 0,
               sizeof(struct mesh_downlink_entry));
    }
}

static void commit_route_epoch_transition(
    struct mesh_relay *relay,
    uint32_t previous_epoch,
    struct reactive_route_rollback *rollback)
{
    if (relay == NULL || relay->upstream.current_epoch == previous_epoch) {
        return;
    }

    relay->gateway_route_adv_seq = 0u;
    clear_all_downlinks(relay);
    memset(relay->event_timings, 0, sizeof(relay->event_timings));
    mesh_relay_reset_route_discovery(relay);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (!relay->upstream.candidates[i].valid) {
            clear_upstream_candidate_ancestry_at(relay, i);
        }
    }
    if (rollback != NULL &&
        rollback->kind == REACTIVE_ROUTE_ROLLBACK_UPSTREAM) {
        /*
         * Once stale epoch dependents are gone, reverting only the upstream
         * table would recreate a mixed-epoch state. Keep the validated newer
         * route and let an exact packet retry repair any later response work.
         */
        rollback->kind =
            REACTIVE_ROUTE_ROLLBACK_UPSTREAM_EPOCH_COMMITTED;
    }
}

static bool route_dependents_empty(const struct mesh_relay *relay)
{
    if (relay == NULL ||
        route_selected(&relay->upstream) != NULL ||
        relay->route_discovery.active ||
        relay->gateway_route_adv_seq != 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay->upstream.candidates[i].valid) {
            return false;
        }
    }
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

        if (entry != NULL && entry->valid) {
            return false;
        }
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid) {
            return false;
        }
    }
    return true;
}

int mesh_relay_seed_route_freshness(
    struct mesh_relay *relay,
    uint32_t route_epoch,
    uint32_t gateway_route_adv_seq)
{
    if (relay == NULL ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        (relay->role == MESH_RELAY_ROLE_GATEWAY) !=
            (relay->local_id == relay->gateway_id) ||
        route_epoch == 0u ||
        (uint16_t)route_epoch == 0u ||
        (relay->role != MESH_RELAY_ROLE_ANCHOR &&
         gateway_route_adv_seq != 0u) ||
        !route_dependents_empty(relay) ||
        mesh_relay_tx_active(relay) ||
        mesh_relay_result_bundle_pending(relay) ||
        relay->result_offer_reservation.valid) {
        return PROTO_ERR_ARG;
    }

    route_table_init(&relay->upstream, route_epoch);
    relay->gateway_route_adv_seq = gateway_route_adv_seq;
    return PROTO_OK;
}

int mesh_relay_validate_configured_gateway_route(
    const struct mesh_relay *relay,
    const struct route_candidate *candidate,
    bool *epoch_changed)
{
    struct route_table trial;
    int ret;

    if (relay == NULL || candidate == NULL || epoch_changed == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id ||
        candidate->gateway_id != relay->gateway_id ||
        !id_is_unicast(candidate->next_hop_id) ||
        candidate->next_hop_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    trial = relay->upstream;
    ret = route_upsert_candidate(&trial, candidate);
    if (ret != PROTO_OK) {
        return ret;
    }
    *epoch_changed =
        trial.current_epoch != relay->upstream.current_epoch;
    return PROTO_OK;
}

int mesh_relay_upsert_configured_gateway_route(
    struct mesh_relay *relay,
    const struct route_candidate *candidate)
{
    uint32_t previous_epoch;
    bool epoch_changed;
    int ret;

    ret = mesh_relay_validate_configured_gateway_route(
        relay, candidate, &epoch_changed);
    if (ret != PROTO_OK) {
        return ret;
    }
    previous_epoch = relay->upstream.current_epoch;
    ret = route_upsert_candidate(&relay->upstream, candidate);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (epoch_changed) {
        commit_route_epoch_transition(relay, previous_epoch, NULL);
    }
    return PROTO_OK;
}

static void note_route_state_change(struct mesh_relay_result *result)
{
    if (result == NULL || result->route_state_changed) {
        return;
    }
    result->route_state_changed = true;
}

static bool downlink_is_better(const struct mesh_downlink_entry *candidate,
                               const struct mesh_downlink_entry *selected);

static int downlink_index(const struct mesh_relay *relay, uint64_t target_id)
{
    int selected_index = -1;
    const struct mesh_downlink_entry *selected = NULL;

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

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

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

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

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

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
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(relay, i);

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
        return route_epoch_strictly_newer(candidate->route_epoch,
                                          selected->route_epoch);
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
        return (int32_t)(candidate->last_seen_ms -
                         selected->last_seen_ms) > 0;
    }
    return candidate->next_hop_id < selected->next_hop_id;
}

static int upsert_downlink(struct mesh_relay *relay,
                           const struct mesh_downlink_entry *entry,
                           struct downlink_upsert_rollback *rollback)
{
    bool discovery_scoped;
    int index;
    int free_index = -1;
    int replace_index = 0;
    const struct mesh_downlink_entry *replace = NULL;

    if (rollback != NULL) {
        memset(rollback, 0, sizeof(*rollback));
    }
    discovery_scoped = entry->discovery_flood_epoch_id != 0u;
    if (!id_is_unicast(entry->target_id) ||
        !id_is_unicast(entry->next_hop_id) ||
        !id_is_unicast(entry->gateway_id) ||
        entry->target_id == relay->local_id ||
        (uint16_t)entry->route_epoch == 0u ||
        entry->quality > 100u ||
        (discovery_scoped &&
         (entry->gateway_id == entry->target_id ||
          entry->discovery_session_id == 0u))) {
        return PROTO_ERR_MALFORMED;
    }

    index = downlink_exact_index(relay, entry);
    if (index >= 0) {
        struct mesh_downlink_entry *slot = downlink_at_mutable(
            relay, (size_t)index);

        if (entry->route_epoch != slot->route_epoch &&
            !route_epoch_strictly_newer(entry->route_epoch,
                                        slot->route_epoch)) {
            return PROTO_ERR_STALE;
        }
        if (rollback != NULL) {
            rollback->previous = *slot;
            rollback->index = (uint8_t)index;
            rollback->valid = true;
        }
        *slot = *entry;
        slot->valid = true;
        return PROTO_OK;
    }

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *candidate =
            mesh_relay_downlink_at(relay, i);

        if (!candidate->valid) {
            free_index = (int)i;
            break;
        }
        if (replace == NULL || downlink_is_better(replace, candidate)) {
            replace = candidate;
            replace_index = (int)i;
        }
    }

    if (free_index < 0 && !downlink_is_better(entry, replace)) {
        return PROTO_ERR_NO_SPACE;
    }
    index = free_index >= 0 ? free_index : replace_index;
    if (rollback != NULL) {
        rollback->previous = *downlink_at_mutable(relay, (size_t)index);
        rollback->index = (uint8_t)index;
        rollback->valid = true;
    }
    *downlink_at_mutable(relay, (size_t)index) = *entry;
    downlink_at_mutable(relay, (size_t)index)->valid = true;
    return PROTO_OK;
}

static int upsert_required_gateway_downlink(
    struct mesh_relay *relay,
    const struct mesh_downlink_entry *entry)
{
    int replace_index = -1;
    int ret = upsert_downlink(relay, entry, NULL);

    if (ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        const struct mesh_downlink_entry *candidate =
            mesh_relay_downlink_at(relay, i);

        if (!candidate->valid) {
            replace_index = (int)i;
            break;
        }
        if (replace_index < 0 ||
            downlink_is_better(mesh_relay_downlink_at(
                                   relay, (size_t)replace_index),
                               candidate)) {
            replace_index = (int)i;
        }
    }
    if (replace_index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    *downlink_at_mutable(relay, (size_t)replace_index) = *entry;
    downlink_at_mutable(relay, (size_t)replace_index)->valid = true;
    return PROTO_OK;
}

static bool duplicate_tracked(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return true;
    }

    /*
     * Direct event controls have their own bounded owner and replay
     * registries in the application.  An exact PROPOSE retry must reach that
     * owner so it can replay ACCEPT after the first response is lost; the same
     * applies to the remaining idempotent control transitions.  Keeping them
     * in the generic relay cache would drop the retry before that state
     * machine can classify it as exact, conflicting, or retired.
     */
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE ||
        packet->msg_type == MSG_MESH_EVENT_ACCEPT ||
        packet->msg_type == MSG_MESH_EVENT_UPDATE ||
        packet->msg_type == MSG_MESH_EVENT_END) {
        return false;
    }

    if (packet->msg_type == MSG_ROUTE_REQ &&
        packet->dst_id == MESH_BROADCAST_ID) {
        return false;
    }
    return true;
}

static uint32_t duplicate_session_identity(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint32_t recovery_attempt_id = 0u;

    /*
     * TLV_COMMAND_SEQ is the immutable application-operation identity.  The
     * flood epoch is the transport-attempt identity: redundant RF copies from
     * one attempt share it, while a bounded later attempt may use a fresh value
     * so every intermediary refloods it.  Most commands bind both values
     * together; assignment CLAIM/TABLE rounds deliberately do not.
     */
    if (packet != NULL && packet->msg_type == MSG_COMMAND &&
        packet->dst_id == MESH_BROADCAST_ID && payload != NULL &&
        tlv_find_unique(payload,
                        payload_len,
                        TLV_FLOOD_EPOCH_ID,
                        &value,
                        &value_len) == PROTO_OK &&
        value_len == sizeof(uint32_t) && proto_get_u32_le(value) != 0u) {
        return proto_get_u32_le(value);
    }
    /*
     * A collection-EACK recovery preserves the old semantic collection session while
     * each bounded RF reconstruction needs a fresh attempt identity. Without
     * this TLV, a lost CLOSED EACK is suppressed for the full command replay
     * window when the sender retries through another relay.
     */
    if (packet != NULL &&
        packet->msg_type == MSG_GATEWAY_COLLECTION_EACK &&
        packet->dst_id == MESH_BROADCAST_ID &&
        payload != NULL &&
        gateway_collection_eack_recovery_attempt_id(
            payload, payload_len, &recovery_attempt_id) == PROTO_OK) {
        return recovery_attempt_id;
    }
    return packet != NULL ? packet->session_id : 0u;
}

enum broadcast_command_replay_classification {
    BROADCAST_COMMAND_REPLAY_NEW = 0,
    BROADCAST_COMMAND_REPLAY_EXACT = 1,
    BROADCAST_COMMAND_REPLAY_STALE = 2,
    BROADCAST_COMMAND_REPLAY_CONFLICT = 3,
};

static enum broadcast_command_replay_classification
broadcast_command_replay_classify(
    const struct mesh_command_replay_window *window,
    uint32_t command_seq,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t forward_delta;
    uint32_t age;

    if (window == NULL || !window->initialized || command_seq == 0u) {
        return BROADCAST_COMMAND_REPLAY_NEW;
    }
    forward_delta = command_seq - window->newest_command_seq;
    if (forward_delta != 0u &&
        forward_delta < UINT32_C(0x80000000)) {
        return BROADCAST_COMMAND_REPLAY_NEW;
    }
    if (forward_delta == 0u) {
        if ((window->forwarded & UINT64_C(1)) == 0u) {
            return BROADCAST_COMMAND_REPLAY_NEW;
        }
        if (!relay_semantic_packet_digest(packet,
                                          payload,
                                          payload_len,
                                          semantic_digest)) {
            return BROADCAST_COMMAND_REPLAY_CONFLICT;
        }
        return semantic_digest_equal(window->newest_semantic_digest,
                                     semantic_digest,
                                     sizeof(semantic_digest)) ?
                   BROADCAST_COMMAND_REPLAY_EXACT :
                   BROADCAST_COMMAND_REPLAY_CONFLICT;
    }
    if (forward_delta == UINT32_C(0x80000000)) {
        return BROADCAST_COMMAND_REPLAY_STALE;
    }
    age = window->newest_command_seq - command_seq;
    if (age >= 64u) {
        return BROADCAST_COMMAND_REPLAY_STALE;
    }
    /*
     * Only the current transport attempt may be redelivered locally. Older
     * attempts are stale after their first forward even though the bitmap
     * retains them to admit one previously unseen out-of-order attempt.
     */
    return (window->forwarded & (UINT64_C(1) << age)) != 0u ?
               BROADCAST_COMMAND_REPLAY_STALE :
               BROADCAST_COMMAND_REPLAY_NEW;
}

static int broadcast_command_replay_store(
    struct mesh_command_replay_window *window,
    uint32_t command_seq,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t forward_delta;
    uint32_t age;

    if (window == NULL || command_seq == 0u ||
        !relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return PROTO_ERR_ARG;
    }
    if (!window->initialized) {
        window->forwarded = UINT64_C(1);
        window->newest_command_seq = command_seq;
        memcpy(window->newest_semantic_digest,
               semantic_digest,
               sizeof(window->newest_semantic_digest));
        window->initialized = true;
        return PROTO_OK;
    }

    forward_delta = command_seq - window->newest_command_seq;
    if (forward_delta != 0u &&
        forward_delta < UINT32_C(0x80000000)) {
        window->forwarded = forward_delta >= 64u ?
            UINT64_C(1) :
            (window->forwarded << forward_delta) | UINT64_C(1);
        window->newest_command_seq = command_seq;
        memcpy(window->newest_semantic_digest,
               semantic_digest,
               sizeof(window->newest_semantic_digest));
        return PROTO_OK;
    }
    if (forward_delta == 0u) {
        window->forwarded |= UINT64_C(1);
        memcpy(window->newest_semantic_digest,
               semantic_digest,
               sizeof(window->newest_semantic_digest));
        return PROTO_OK;
    }
    if (forward_delta == UINT32_C(0x80000000)) {
        return PROTO_ERR_STALE;
    }
    age = window->newest_command_seq - command_seq;
    if (age < 64u) {
        window->forwarded |= UINT64_C(1) << age;
        return PROTO_OK;
    }
    return PROTO_ERR_STALE;
}

static bool broadcast_command_replay_rollback(
    struct mesh_command_replay_window *window,
    uint32_t command_seq)
{
    uint32_t forward_delta;
    uint32_t age;
    uint64_t bit;

    if (window == NULL || !window->initialized || command_seq == 0u) {
        return false;
    }
    forward_delta = command_seq - window->newest_command_seq;
    if (forward_delta != 0u &&
        forward_delta < UINT32_C(0x80000000)) {
        return false;
    }
    if (forward_delta == UINT32_C(0x80000000)) {
        return false;
    }
    age = forward_delta == 0u ?
        0u :
        window->newest_command_seq - command_seq;
    if (age >= 64u) {
        return false;
    }
    bit = UINT64_C(1) << age;
    if ((window->forwarded & bit) == 0u) {
        return false;
    }
    window->forwarded &= ~bit;
    if (window->forwarded == 0u) {
        memset(window, 0, sizeof(*window));
    }
    return true;
}

static bool duplicate_matches_packet(const struct mesh_duplicate_entry *entry,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if (!entry->valid ||
        entry->msg_type != packet->msg_type ||
        entry->src_id != packet->src_id ||
        entry->dst_id != packet->dst_id ||
        entry->session_id !=
            duplicate_session_identity(packet, payload, payload_len)) {
        return false;
    }

    if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV) {
        return true;
    }
    if (packet->msg_type == MSG_COMMAND && packet->dst_id == MESH_BROADCAST_ID) {
        /* duplicate_session_identity() already selected the flood attempt. */
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

enum duplicate_classification {
    DUPLICATE_CLASS_NEW = 0,
    DUPLICATE_CLASS_EXACT = 1,
    DUPLICATE_CLASS_CONFLICT = 2,
};

static bool gateway_ack_history_applies(const struct mesh_relay *relay,
                                        const struct proto_packet *packet)
{
    return relay->role == MESH_RELAY_ROLE_GATEWAY &&
           packet->dst_id == relay->local_id &&
           (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
           /*
            * A direct gateway route probe is idempotent contact control, not
            * accepted data custody. Remembering every fresh probe identity for
            * the one-day data-custody horizon lets one disconnected node
            * exhaust the shared ACK store and block unrelated anchors.
            * Rebuild its ACK from every valid reception instead.
            */
           packet->msg_type != MSG_GATEWAY_ROUTE_REQ &&
           /*
            * ACK_CONFIRM is an idempotent terminal proof. Do not charge it to
            * the volatile per-origin ACK history, because that can reject a
            * valid late proof after reboot or when all origin partitions are
            * occupied. Its normal duplicate-cache entry is sufficient for
            * same-boot replay; after reset the normal RX/custody path handles
            * the proof, with no gateway host journal or receipt ledger
            * involved.
            */
           packet->msg_type != MSG_GATEWAY_ACK_CONFIRM;
}

static bool gateway_ack_identity_valid(
    const struct mesh_gateway_ack_identity_entry *entry)
{
    uint8_t owner_code = entry->owner_state & GATEWAY_ACK_HISTORY_OWNER_MASK;

    return owner_code > 0u && owner_code <= MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX;
}

static bool gateway_ack_identity_candidate_bit(
    const struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    uint8_t mask;

    if (store == NULL ||
        identity_index >= MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        return false;
    }
    mask = (uint8_t)(1u << (identity_index % 8u));
    return (store->candidate_identity_bits[identity_index / 8u] & mask) != 0u;
}

static void gateway_ack_identity_set_candidate_bit(
    struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    if (store != NULL &&
        identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        store->candidate_identity_bits[identity_index / 8u] |=
            (uint8_t)(1u << (identity_index % 8u));
    }
}

static void gateway_ack_identity_clear_candidate_bit(
    struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    if (store != NULL &&
        identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        store->candidate_identity_bits[identity_index / 8u] &=
            (uint8_t)~(1u << (identity_index % 8u));
    }
}

static bool gateway_ack_identity_confirmed_bit(
    const struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    uint8_t mask;

    if (store == NULL ||
        identity_index >= MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        return false;
    }
    mask = (uint8_t)(1u << (identity_index % 8u));
    return (store->confirmed_identity_bits[identity_index / 8u] & mask) != 0u;
}

static void gateway_ack_identity_set_confirmed_bit(
    struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    if (store != NULL &&
        identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        store->confirmed_identity_bits[identity_index / 8u] |=
            (uint8_t)(1u << (identity_index % 8u));
    }
}

static void gateway_ack_identity_clear_confirmed_bit(
    struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    if (store != NULL &&
        identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        store->confirmed_identity_bits[identity_index / 8u] &=
            (uint8_t)~(1u << (identity_index % 8u));
    }
}

static void gateway_ack_identity_clear(
    struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    if (store == NULL ||
        identity_index >= MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        return;
    }
    memset(&store->identities[identity_index],
           0,
           sizeof(store->identities[identity_index]));
    gateway_ack_identity_clear_candidate_bit(store, identity_index);
    gateway_ack_identity_clear_confirmed_bit(store, identity_index);
}

static bool gateway_ack_identity_is_transition_reservation(
    const struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    const struct mesh_gateway_ack_identity_entry *entry;

    if (store == NULL ||
        identity_index >= MESH_RELAY_GATEWAY_ACK_CAPACITY ||
        !gateway_ack_identity_candidate_bit(store, identity_index)) {
        return false;
    }
    entry = &store->identities[identity_index];
    return gateway_ack_identity_valid(entry) &&
           entry->msg_type == GATEWAY_ACK_HISTORY_TRANSITION_MSG_TYPE &&
           entry->session_id == 0u &&
           entry->seq == 0u;
}

static bool gateway_ack_identity_is_assignment_candidate(
    const struct mesh_gateway_ack_store *store,
    uint16_t identity_index)
{
    return store != NULL &&
           identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY &&
           gateway_ack_identity_valid(&store->identities[identity_index]) &&
           gateway_ack_identity_candidate_bit(store, identity_index);
}

static uint8_t gateway_ack_identity_owner_index(
    const struct mesh_gateway_ack_identity_entry *entry)
{
    return (uint8_t)((entry->owner_state & GATEWAY_ACK_HISTORY_OWNER_MASK) - 1u);
}

static bool gateway_ack_identity_matches_packet(
    const struct mesh_gateway_ack_identity_entry *entry,
    uint8_t owner_index,
    const struct proto_packet *packet)
{
    return gateway_ack_identity_valid(entry) &&
           gateway_ack_identity_owner_index(entry) == owner_index &&
           entry->msg_type == packet->msg_type &&
           entry->session_id == packet->session_id &&
           entry->seq == packet->seq;
}

static bool command_result_has_command_id(const uint8_t *payload,
                                          size_t payload_len,
                                          enum command_id expected)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find_unique(payload,
                           payload_len,
                           TLV_COMMAND_ID,
                           &value,
                           &value_len) == PROTO_OK &&
           value_len == sizeof(uint16_t) &&
           proto_get_u16_le(value) == (uint16_t)expected;
}

static uint32_t gateway_ack_history_retention_ms_for(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint32_t source_expiry_ms = 0u;

    if (packet->msg_type == MSG_COMMAND_RESULT &&
               command_result_has_command_id(
                   payload,
                   payload_len,
                   CMD_ASSIGN_DISCOVERY_SLOTS)) {
        source_expiry_ms = DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S * 1000u;
    }

    if (source_expiry_ms != 0u) {
        /*
         * A relay may start its final attempt immediately before the
         * immutable source-age limit. Keep the acceptance identity through
         * the complete ACK-forward/retry tail so a lost ACK can be repaired
         * without redelivering or mutating accepted data.
         */
        return source_expiry_ms +
               MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS + 1000u;
    }
    return MESH_RELAY_GATEWAY_ACK_RETENTION_MS;
}

static bool gateway_ack_origin_valid(
    const struct mesh_gateway_ack_store *store,
    uint8_t origin_index)
{
    return store->origin_src_ids[origin_index] != 0u;
}

static void gateway_ack_origin_clear(struct mesh_gateway_ack_store *store,
                                     uint8_t origin_index)
{
    store->origin_src_ids[origin_index] = 0u;
    store->origin_batch_ids[origin_index] = 0u;
}

static uint8_t gateway_ack_history_owner_count(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index);
static uint8_t gateway_ack_history_ordinary_owner_count(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index);

static void gateway_ack_history_expire_stale(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    struct mesh_gateway_ack_store *store;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY) {
        return;
    }
    store = relay->gateway_ack_store;
    if (store == NULL) {
        return;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];
        bool semantic_identity;

        if (!gateway_ack_identity_valid(identity) ||
            (int32_t)(now_ms - identity->expires_at_ms) < 0) {
            continue;
        }
        semantic_identity =
            (identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u;
        if (semantic_identity && i < MESH_RELAY_GATEWAY_ACK_CAPACITY) {
            continue;
        }
        gateway_ack_identity_clear(store, i);
    }
    for (uint8_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        if (gateway_ack_origin_valid(store, i) &&
            gateway_ack_history_owner_count(store, i) == 0u) {
            gateway_ack_origin_clear(store, i);
        }
    }
}

static uint8_t gateway_ack_history_origin_count(
    const struct mesh_gateway_ack_store *store)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        if (gateway_ack_origin_valid(store, i)) {
            count++;
        }
    }
    return count;
}

static int gateway_ack_history_find_origin_index(
    const struct mesh_relay *relay,
    uint64_t src_id)
{
    const struct mesh_gateway_ack_store *store = relay->gateway_ack_store;

    if (store == NULL) {
        return -1;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        if (gateway_ack_origin_valid(store, i) &&
            store->origin_src_ids[i] == src_id) {
            return i;
        }
    }
    return -1;
}

static int gateway_ack_history_free_origin_index(
    struct mesh_relay *relay)
{
    struct mesh_gateway_ack_store *store = relay->gateway_ack_store;

    if (store == NULL) {
        return -1;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        if (!gateway_ack_origin_valid(store, i)) {
            return i;
        }
    }
    return -1;
}

static int gateway_ack_history_free_identity_index(
    const struct mesh_gateway_ack_store *store)
{
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        if (!gateway_ack_identity_valid(&store->identities[i])) {
            return i;
        }
    }
    return -1;
}

static int gateway_ack_history_find_identity_index(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index,
    const struct proto_packet *packet)
{
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        if (gateway_ack_identity_matches_packet(&store->identities[i],
                                                owner_index,
                                                packet)) {
            return i;
        }
    }
    return -1;
}

static uint8_t gateway_ack_history_owner_count(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index)
{
    uint8_t count = 0u;

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        if (gateway_ack_identity_valid(&store->identities[i]) &&
            gateway_ack_identity_owner_index(&store->identities[i]) ==
                owner_index) {
            count++;
        }
    }
    return count;
}

static uint8_t gateway_ack_history_ordinary_owner_count(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index)
{
    uint8_t count = 0u;

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        if (gateway_ack_identity_valid(&store->identities[i]) &&
            gateway_ack_identity_owner_index(&store->identities[i]) ==
                owner_index) {
            count++;
        }
    }
    return count;
}

static int gateway_ack_history_transition_reservation_index(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index)
{
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];

        if (gateway_ack_identity_is_transition_reservation(store, i) &&
            gateway_ack_identity_owner_index(identity) == owner_index) {
            return i;
        }
    }
    return -1;
}

static int gateway_ack_history_assignment_candidate_identity_index(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index)
{
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];

        if (gateway_ack_identity_is_assignment_candidate(store, i) &&
            gateway_ack_identity_owner_index(identity) == owner_index) {
            return i;
        }
    }
    return -1;
}

static int gateway_ack_history_owner_replacement_index(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index,
    uint32_t now_ms);

int mesh_relay_reserve_gateway_ack_candidate(struct mesh_relay *relay,
                                             uint64_t candidate_id,
                                             uint32_t now_ms)
{
    struct mesh_gateway_ack_store *store;
    struct mesh_gateway_ack_identity_entry *reservation;
    int origin_index;
    int identity_index;
    int replaced_owner_index = -1;

    if (relay == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        relay->gateway_ack_store == NULL ||
        !id_is_unicast(candidate_id) ||
        candidate_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    store = relay->gateway_ack_store;
    gateway_ack_history_expire_stale(relay, now_ms);
    origin_index = gateway_ack_history_find_origin_index(relay, candidate_id);
    if (origin_index >= 0) {
        if (gateway_ack_history_assignment_candidate_identity_index(
                store, (uint8_t)origin_index) >= 0) {
            return PROTO_OK;
        }
        /* A confirmed semantic tombstone is safe source-local capacity for a
         * later assignment owner. Unconfirmed acceptance identity is exact
         * ACK-confirm authority and must never be displaced by admission. */
        if (gateway_ack_history_ordinary_owner_count(
                store, (uint8_t)origin_index) >=
            MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN) {
            identity_index = gateway_ack_history_owner_replacement_index(
                store, (uint8_t)origin_index, now_ms);
        } else {
            identity_index = gateway_ack_history_free_identity_index(store);
        }
        if (identity_index < 0) {
            return PROTO_ERR_NO_SPACE;
        }
    } else {
        if (gateway_ack_history_origin_count(store) >=
            MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX) {
            return PROTO_ERR_NO_SPACE;
        }

        origin_index = gateway_ack_history_free_origin_index(relay);
        identity_index = gateway_ack_history_free_identity_index(store);
        if (origin_index < 0 || identity_index < 0) {
            return PROTO_ERR_NO_SPACE;
        }

        gateway_ack_origin_clear(store, (uint8_t)origin_index);
        store->origin_src_ids[origin_index] = candidate_id;
    }
    reservation = &store->identities[identity_index];
    if (gateway_ack_identity_valid(reservation)) {
        replaced_owner_index = gateway_ack_identity_owner_index(reservation);
    }
    gateway_ack_identity_clear(store, (uint16_t)identity_index);
    if (replaced_owner_index >= 0 &&
        replaced_owner_index != origin_index &&
        gateway_ack_history_owner_count(
            store, (uint8_t)replaced_owner_index) == 0u) {
        gateway_ack_origin_clear(store, (uint8_t)replaced_owner_index);
    }
    reservation->expires_at_ms = now_ms +
        DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S * 1000u +
        MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS + 1000u;
    reservation->owner_state = (uint8_t)(origin_index + 1);
    gateway_ack_identity_set_candidate_bit(
        store, (uint16_t)identity_index);
    return PROTO_OK;
}

int mesh_relay_note_gateway_origin_reboot(struct mesh_relay *relay,
                                          uint64_t source_id)
{
    struct mesh_gateway_ack_store *store;
    int origin_index;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        relay->gateway_ack_store == NULL || !id_is_unicast(source_id) ||
        source_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay, source_id);
    if (origin_index < 0) {
        return PROTO_OK;
    }

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];

        if (!gateway_ack_identity_valid(identity) ||
            gateway_ack_identity_owner_index(identity) !=
                (uint8_t)origin_index) {
            continue;
        }
        if ((identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u) {
            gateway_ack_identity_set_confirmed_bit(store, i);
        } else {
            gateway_ack_identity_clear(store, i);
        }
    }
    store->origin_batch_ids[origin_index] = 0u;
    return PROTO_OK;
}

int mesh_relay_reconcile_gateway_ack_membership(
    struct mesh_relay *relay,
    const uint64_t *member_ids,
    size_t member_count)
{
    struct mesh_gateway_ack_store *store;

    if (relay == NULL ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->local_id != relay->gateway_id ||
        relay->gateway_ack_store == NULL ||
        member_count > MESH_CONNECTED_MAX_ANCHORS ||
        (member_count != 0u && member_ids == NULL)) {
        return PROTO_ERR_ARG;
    }
    for (size_t i = 0u; i < member_count; i++) {
        if (!id_is_unicast(member_ids[i]) ||
            member_ids[i] == relay->local_id) {
            return PROTO_ERR_ARG;
        }
        for (size_t j = 0u; j < i; j++) {
            if (member_ids[j] == member_ids[i]) {
                return PROTO_ERR_ARG;
            }
        }
    }

    store = relay->gateway_ack_store;
    for (uint8_t origin_index = 0u;
         origin_index < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX;
         origin_index++) {
        bool committed = false;

        if (!gateway_ack_origin_valid(store, origin_index)) {
            continue;
        }
        for (size_t member_index = 0u;
             member_index < member_count;
             member_index++) {
            if (store->origin_src_ids[origin_index] ==
                member_ids[member_index]) {
                committed = true;
                break;
            }
        }
        if (committed) {
            /*
             * Promotion keeps accepted packet identities but releases the
             * temporary reservation marker. A reservation that was never
             * replaced by a committed packet has no ACK identity to retain.
             */
            for (uint16_t identity_index = 0u;
                 identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY;
                 identity_index++) {
                struct mesh_gateway_ack_identity_entry *identity =
                    &store->identities[identity_index];

                if (gateway_ack_identity_valid(identity) &&
                    gateway_ack_identity_owner_index(identity) ==
                        origin_index &&
                    gateway_ack_identity_candidate_bit(store,
                                                       identity_index)) {
                    if (gateway_ack_identity_is_transition_reservation(
                            store, identity_index)) {
                        gateway_ack_identity_clear(store, identity_index);
                    } else {
                        gateway_ack_identity_clear_candidate_bit(
                            store, identity_index);
                    }
                }
            }
            continue;
        }
        /* Roster absence is not terminal proof for accepted traffic. A relay
         * can still retain the exact source ACK-confirm after enumeration has
         * moved on, so retire only nonsemantic reservations here. */
        for (uint16_t identity_index = 0u;
             identity_index < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY;
             identity_index++) {
            struct mesh_gateway_ack_identity_entry *identity =
                &store->identities[identity_index];

            if (gateway_ack_identity_valid(identity) &&
                gateway_ack_identity_owner_index(identity) == origin_index) {
                if ((identity->owner_state &
                     GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u) {
                    gateway_ack_identity_clear_candidate_bit(
                        store, identity_index);
                } else {
                    gateway_ack_identity_clear(store, identity_index);
                }
            }
        }
        if (gateway_ack_history_owner_count(store, origin_index) == 0u) {
            gateway_ack_origin_clear(store, origin_index);
        }
    }
    return PROTO_OK;
}

static int gateway_ack_history_owner_replacement_index(
    const struct mesh_gateway_ack_store *store,
    uint8_t owner_index,
    uint32_t now_ms)
{
    int replacement = -1;
    uint32_t shortest_remaining_ms = UINT32_MAX;

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];
        uint32_t remaining_ms;

        if (!gateway_ack_identity_valid(identity) ||
            gateway_ack_identity_owner_index(identity) != owner_index ||
            (identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) == 0u ||
            !gateway_ack_identity_confirmed_bit(store, i)) {
            continue;
        }
        remaining_ms =
            (int32_t)(identity->expires_at_ms - now_ms) <= 0 ?
                0u :
                identity->expires_at_ms - now_ms;
        if (replacement < 0 || remaining_ms < shortest_remaining_ms) {
            replacement = (int)i;
            shortest_remaining_ms = remaining_ms;
        }
    }
    return replacement;
}

static bool gateway_ack_history_seen(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    bool require_payload_identity,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct mesh_gateway_ack_store *store;
    int origin_index;

    if (!gateway_ack_history_applies(relay, packet) ||
        relay->gateway_ack_store == NULL) {
        return false;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay, packet->src_id);
    if (origin_index < 0) {
        return false;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];

        if (!gateway_ack_identity_matches_packet(identity,
                                                 (uint8_t)origin_index,
                                                 packet)) {
            continue;
        }
        if (require_payload_identity &&
            (((identity->owner_state &
               GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) == 0u) ||
             !semantic_digest_equal(identity->semantic_digest,
                                    semantic_digest,
                                    SEMANTIC_DIGEST_SHA256_LEN))) {
            continue;
        }
        return true;
    }
    return false;
}

int mesh_relay_gateway_ack_confirm_history_match(
    struct mesh_relay *relay,
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    struct mesh_gateway_ack_confirm_identity *identity)
{
    struct mesh_gateway_ack_store *store;
    struct mesh_gateway_ack_confirm_identity parsed;
    int origin_index;
    int ret;

    if (relay == NULL || confirm_packet == NULL || confirm_payload == NULL ||
        identity == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        confirm_packet->dst_id != relay->local_id ||
        relay->gateway_ack_store == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_gateway_ack_confirm_payload_parse(
        confirm_packet,
        confirm_payload,
        confirm_payload_len,
        &parsed);
    if (ret != PROTO_OK) {
        return ret;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(
        relay, confirm_packet->src_id);
    if (origin_index < 0) {
        return PROTO_ERR_NOT_FOUND;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &store->identities[i];

        if (!gateway_ack_identity_valid(entry) ||
            gateway_ack_identity_owner_index(entry) !=
                (uint8_t)origin_index ||
            entry->msg_type != parsed.msg_type ||
            entry->session_id != parsed.session_id ||
            entry->seq != parsed.seq ||
            (entry->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) == 0u ||
            !semantic_digest_equal(entry->semantic_digest,
                                   parsed.digest,
                                   sizeof(parsed.digest))) {
            continue;
        }
        gateway_ack_identity_set_confirmed_bit(store, i);
        *identity = parsed;
        return PROTO_OK;
    }
    return PROTO_ERR_NOT_FOUND;
}

static bool gateway_ack_identity_confirmation_pending(
    const struct mesh_gateway_ack_store *store,
    uint16_t identity_index,
    uint32_t now_ms)
{
    const struct mesh_gateway_ack_identity_entry *entry;

    if (store == NULL ||
        identity_index >= MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY) {
        return false;
    }
    entry = &store->identities[identity_index];
    return gateway_ack_identity_valid(entry) &&
           (entry->owner_state &
            GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u &&
           (int32_t)(entry->expires_at_ms - now_ms) > 0 &&
           !gateway_ack_identity_confirmed_bit(store, identity_index);
}

bool mesh_relay_gateway_delivery_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms)
{
    const struct mesh_gateway_ack_store *store;
    int origin_index;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->gateway_ack_store == NULL || !id_is_unicast(src_id)) {
        return false;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay, src_id);
    if (origin_index < 0) {
        return false;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &store->identities[i];

        if (gateway_ack_identity_confirmation_pending(store, i, now_ms) &&
            gateway_ack_identity_owner_index(entry) ==
                (uint8_t)origin_index &&
            entry->msg_type == msg_type &&
            entry->session_id == session_id) {
            return true;
        }
    }
    return false;
}

bool mesh_relay_gateway_identity_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint16_t seq,
    uint32_t now_ms)
{
    const struct mesh_gateway_ack_store *store;
    int origin_index;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->gateway_ack_store == NULL || !id_is_unicast(src_id)) {
        return false;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay, src_id);
    if (origin_index < 0) {
        return false;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &store->identities[i];

        if (gateway_ack_identity_confirmation_pending(store, i, now_ms) &&
            gateway_ack_identity_owner_index(entry) ==
                (uint8_t)origin_index &&
            entry->msg_type == msg_type &&
            entry->session_id == session_id &&
            entry->seq == seq) {
            return true;
        }
    }
    return false;
}

bool mesh_relay_gateway_operation_confirmation_pending(
    const struct mesh_relay *relay,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms)
{
    const struct mesh_gateway_ack_store *store;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->gateway_ack_store == NULL) {
        return false;
    }
    store = relay->gateway_ack_store;
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &store->identities[i];

        if (gateway_ack_identity_confirmation_pending(store, i, now_ms) &&
            entry->msg_type == msg_type &&
            entry->session_id == session_id) {
            return true;
        }
    }
    return false;
}

bool mesh_relay_gateway_origin_confirmation_pending(
    const struct mesh_relay *relay,
    uint64_t src_id,
    uint32_t now_ms)
{
    const struct mesh_gateway_ack_store *store;
    int origin_index;

    if (relay == NULL || relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->gateway_ack_store == NULL || !id_is_unicast(src_id)) {
        return false;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay, src_id);
    if (origin_index < 0) {
        return false;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &store->identities[i];

        if (gateway_ack_identity_confirmation_pending(store, i, now_ms) &&
            gateway_ack_identity_owner_index(entry) ==
                (uint8_t)origin_index) {
            return true;
        }
    }
    return false;
}

static int gateway_ack_history_can_accept(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    struct mesh_gateway_ack_store *store;
    struct mesh_ch9_batch_metadata batch_metadata;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int origin_index;
    int ret;

    if (!gateway_ack_history_applies(relay, packet)) {
        return PROTO_OK;
    }
    if (!relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_ch9_batch_metadata_parse(payload,
                                        payload_len,
                                        &batch_metadata);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (relay->gateway_ack_store == NULL) {
        return PROTO_ERR_NO_SPACE;
    }
    store = relay->gateway_ack_store;
    gateway_ack_history_expire_stale(relay, now_ms);
    origin_index = gateway_ack_history_find_origin_index(relay, packet->src_id);
    if (origin_index < 0) {
        if (gateway_ack_history_origin_count(store) <
                MESH_CONNECTED_MAX_ANCHORS &&
            gateway_ack_history_free_origin_index(relay) >= 0 &&
            gateway_ack_history_free_identity_index(store) >= 0) {
            return PROTO_OK;
        }
        if (packet->msg_type == MSG_COMMAND_RESULT &&
            command_result_has_command_id(
                payload, payload_len, CMD_ASSIGN_DISCOVERY_SLOTS)) {
            /*
             * Transport pre-admission must not bind the only overflow
             * partition. The gateway assignment semantic gate validates the
             * full result and reserves the exact candidate transactionally
             * before it mutates roster or ACK-mask state.
             */
            return PROTO_OK;
        }
        return PROTO_ERR_NO_SPACE;
    }
    {
        int identity_index = gateway_ack_history_find_identity_index(
            store,
            (uint8_t)origin_index,
            packet);

        if (identity_index >= 0) {
            const struct mesh_gateway_ack_identity_entry *identity =
                &store->identities[identity_index];

            if ((identity->owner_state &
                 GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u &&
                !semantic_digest_equal(identity->semantic_digest,
                                       semantic_digest,
                                       sizeof(semantic_digest))) {
                return PROTO_ERR_MALFORMED;
            }
            return PROTO_OK;
        }
    }
    /*
     * Every source owns a fixed-size replay partition. A new identity can
     * reuse only a confirmed source-local tombstone; four unconfirmed records
     * are four live terminal-proof debts and fail closed.
     */
    if (gateway_ack_history_ordinary_owner_count(
            store, (uint8_t)origin_index) >=
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN) {
        int identity_index = gateway_ack_history_owner_replacement_index(
            store, (uint8_t)origin_index, now_ms);

        return identity_index >= 0 ? PROTO_OK : PROTO_ERR_NO_SPACE;
    }
    return gateway_ack_history_free_identity_index(store) >= 0 ?
               PROTO_OK :
               PROTO_ERR_NO_SPACE;
}

static int gateway_ack_history_store(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms,
    bool semantic_identity_valid)
{
    struct mesh_gateway_ack_store *store;
    struct mesh_gateway_ack_identity_entry *identity;
    struct mesh_ch9_batch_metadata batch_metadata;
    int origin_index;
    int identity_index;
    uint32_t batch_id = 0u;
    bool batch_id_valid;
    bool assignment_candidate_origin = false;
    bool assignment_candidate_packet = false;
    bool initialize_origin = false;
    int replaced_owner_index = -1;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    int ret;

    if (!gateway_ack_history_applies(relay, packet)) {
        return PROTO_OK;
    }
    if (semantic_identity_valid &&
        !relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_ch9_batch_metadata_parse(payload,
                                        payload_len,
                                        &batch_metadata);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (relay->gateway_ack_store == NULL) {
        return PROTO_ERR_NO_SPACE;
    }
    store = relay->gateway_ack_store;
    gateway_ack_history_expire_stale(relay, now_ms);
    batch_id_valid = batch_metadata.present;
    batch_id = batch_metadata.batch_id;
    origin_index = gateway_ack_history_find_origin_index(relay, packet->src_id);
    if (origin_index < 0) {
        if (gateway_ack_history_origin_count(store) >=
            MESH_CONNECTED_MAX_ANCHORS) {
            return PROTO_ERR_NO_SPACE;
        }
        origin_index = gateway_ack_history_free_origin_index(relay);
        if (origin_index < 0) {
            return PROTO_ERR_NO_SPACE;
        }
        initialize_origin = true;
    } else {
        assignment_candidate_origin =
            gateway_ack_history_assignment_candidate_identity_index(
                store, (uint8_t)origin_index) >= 0;
    }
    assignment_candidate_packet =
        assignment_candidate_origin &&
        packet->msg_type == MSG_COMMAND_RESULT &&
        command_result_has_command_id(
            payload, payload_len, CMD_ASSIGN_DISCOVERY_SLOTS);
    identity_index = -1;
    if (!initialize_origin) {
        identity_index = gateway_ack_history_find_identity_index(
            store, (uint8_t)origin_index, packet);
    }
    if (identity_index >= 0) {
        identity = &store->identities[identity_index];
        if ((identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u &&
            (!semantic_identity_valid ||
             !semantic_digest_equal(identity->semantic_digest,
                                    semantic_digest,
                                    sizeof(semantic_digest)))) {
            return PROTO_ERR_MALFORMED;
        }
        if (!semantic_identity_valid ||
            (identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u) {
            return PROTO_OK;
        }
    }
    if (identity_index < 0 && !initialize_origin) {
        identity_index = gateway_ack_history_transition_reservation_index(
            store, (uint8_t)origin_index);
    }
    if (identity_index < 0 && !initialize_origin &&
        gateway_ack_history_ordinary_owner_count(
            store, (uint8_t)origin_index) >=
            MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN) {
        if (assignment_candidate_packet) {
            /*
             * CLAIM and TABLE ACK are one assignment-control owner. Reuse its
             * older confirmed identity before touching unrelated ACK debt
             * held by the same unprovisioned anchor.
             */
            int candidate_index =
                gateway_ack_history_assignment_candidate_identity_index(
                    store, (uint8_t)origin_index);

            if (candidate_index >= 0 &&
                gateway_ack_identity_confirmed_bit(
                    store, (uint16_t)candidate_index)) {
                identity_index = candidate_index;
            }
        }
        if (identity_index < 0) {
            identity_index = gateway_ack_history_owner_replacement_index(
                store, (uint8_t)origin_index, now_ms);
        }
        if (identity_index < 0) {
            return PROTO_ERR_NO_SPACE;
        }
    }
    if (identity_index < 0) {
        identity_index = gateway_ack_history_free_identity_index(store);
    }
    if (identity_index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    if (initialize_origin) {
        gateway_ack_origin_clear(store, (uint8_t)origin_index);
        store->origin_src_ids[origin_index] = packet->src_id;
    }
    if (batch_id_valid && !assignment_candidate_origin) {
        store->origin_batch_ids[origin_index] = batch_id;
    }

    identity = &store->identities[identity_index];
    if (identity_index >= (int)MESH_RELAY_GATEWAY_ACK_CAPACITY &&
        gateway_ack_identity_valid(identity)) {
        replaced_owner_index = gateway_ack_identity_owner_index(identity);
    }
    gateway_ack_identity_clear(store, (uint16_t)identity_index);
    if (replaced_owner_index >= 0 &&
        replaced_owner_index != origin_index &&
        gateway_ack_history_owner_count(
            store, (uint8_t)replaced_owner_index) == 0u) {
        gateway_ack_origin_clear(store, (uint8_t)replaced_owner_index);
    }
    identity->session_id = packet->session_id;
    identity->expires_at_ms = now_ms +
        gateway_ack_history_retention_ms_for(packet, payload, payload_len);
    identity->seq = packet->seq;
    if (semantic_identity_valid) {
        memcpy(identity->semantic_digest,
               semantic_digest,
               sizeof(identity->semantic_digest));
    }
    identity->msg_type = packet->msg_type;
    identity->owner_state = (uint8_t)(origin_index + 1) |
        (semantic_identity_valid ? GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY : 0u) |
        (batch_id_valid ? GATEWAY_ACK_HISTORY_BATCHED : 0u);
    if (assignment_candidate_packet) {
        gateway_ack_identity_set_candidate_bit(
            store, (uint16_t)identity_index);
    }
    return PROTO_OK;
}

static int gateway_ack_history_confirm_packet(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct mesh_gateway_ack_store *store;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int origin_index;

    if (relay == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        relay->role != MESH_RELAY_ROLE_GATEWAY ||
        relay->gateway_ack_store == NULL ||
        !relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return PROTO_ERR_ARG;
    }
    store = relay->gateway_ack_store;
    origin_index = gateway_ack_history_find_origin_index(relay,
                                                          packet->src_id);
    if (origin_index < 0) {
        return PROTO_ERR_NOT_FOUND;
    }
    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_STORAGE_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *identity =
            &store->identities[i];

        if (gateway_ack_identity_matches_packet(identity,
                                                (uint8_t)origin_index,
                                                packet) &&
            (identity->owner_state &
             GATEWAY_ACK_HISTORY_SEMANTIC_IDENTITY) != 0u &&
            semantic_digest_equal(identity->semantic_digest,
                                  semantic_digest,
                                  sizeof(semantic_digest))) {
            gateway_ack_identity_set_confirmed_bit(store, i);
            return PROTO_OK;
        }
    }
    return PROTO_ERR_NOT_FOUND;
}

static int gateway_ack_history_accept_generic(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    int ret = gateway_ack_history_can_accept(relay,
                                             packet,
                                             payload,
                                             payload_len,
                                             now_ms);

    if (ret != PROTO_OK) {
        return ret;
    }
    return gateway_ack_history_store(relay,
                                     packet,
                                     payload,
                                     payload_len,
                                     now_ms,
                                     true);
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
    gateway_ack_history_expire_stale(relay, now_ms);
}

static enum duplicate_classification duplicate_classify(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;
    bool require_gateway_history;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (!duplicate_tracked(packet)) {
        return DUPLICATE_CLASS_NEW;
    }

    require_gateway_history = gateway_ack_history_applies(relay, packet);
    if (!relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return DUPLICATE_CLASS_CONFLICT;
    }
    duplicate_expire_stale(relay, now_ms);
    if (require_gateway_history &&
        relay->gateway_ack_store == NULL) {
        return DUPLICATE_CLASS_NEW;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (duplicate_matches_packet(entry, packet, payload, payload_len)) {
            if (!entry->delivery_accepted) {
                continue;
            }
            if (!entry->semantic_identity_valid ||
                !semantic_digest_equal(entry->semantic_digest,
                                       semantic_digest,
                                       sizeof(semantic_digest))) {
                /*
                 * Route replies keep the semantic target as packet src_id.
                 * Distinct responders and forwarding paths can therefore
                 * legitimately share the header identity while carrying
                 * different path metrics. Search for an exact cached reply,
                 * then admit a different candidate as new.
                 *
                 * A gateway route advertisement can legitimately reach an
                 * anchor through more than one loop-free ancestry. Admit a
                 * distinct path so the route layer can retain its physical
                 * ingress parent as a fallback. The exact same path still
                 * matches the semantic digest above and remains a duplicate.
                 */
                if (packet->msg_type == MSG_ROUTE_REPLY ||
                    packet->msg_type == MSG_GATEWAY_ACK_CONFIRM) {
                    /*
                     * ACK_CONFIRM repeats its original packet's sequence in
                     * an outer terminal-proof wrapper. Independent original
                     * message types have separate sequence streams, so two
                     * valid proofs can share the complete outer header while
                     * their embedded identities and semantic digests differ.
                     * Keep searching for an exact proof and admit a distinct
                     * one as new; its inner identity is validated downstream.
                     */
                    continue;
                }
                if (packet->msg_type == MSG_GATEWAY_ROUTE_ADV) {
                    continue;
                }
                return DUPLICATE_CLASS_CONFLICT;
            }
            return DUPLICATE_CLASS_EXACT;
        }
    }
    return gateway_ack_history_seen(relay,
                                    packet,
                                    require_gateway_history,
                                    semantic_digest) ?
           DUPLICATE_CLASS_EXACT :
           DUPLICATE_CLASS_NEW;
}

static void duplicate_store(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (!duplicate_tracked(packet)) {
        return;
    }
    if (!relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      semantic_digest)) {
        return;
    }

    duplicate_expire_stale(relay, now_ms);
    entry = NULL;
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        if (duplicate_matches_packet(&relay->duplicates[i], packet,
                                     payload, payload_len)) {
            if (packet->msg_type == MSG_GATEWAY_ACK_CONFIRM &&
                relay->duplicates[i].semantic_identity_valid &&
                !semantic_digest_equal(
                    relay->duplicates[i].semantic_digest,
                    semantic_digest,
                    sizeof(semantic_digest))) {
                continue;
            }
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
    entry->session_id = duplicate_session_identity(packet, payload,
                                                   payload_len);
    entry->last_seen_ms = now_ms;
    entry->busy_response_at_ms = 0u;
    entry->seq = packet->seq;
    memcpy(entry->semantic_digest,
           semantic_digest,
           sizeof(entry->semantic_digest));
    entry->busy_response_interval_ms = 0u;
    entry->semantic_identity_valid = true;
    entry->delivery_accepted = true;
    entry->valid = true;
}

static void duplicate_store_payload_identity(struct mesh_relay *relay,
                                             const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint32_t now_ms)
{
    duplicate_store(relay, packet, payload, payload_len, now_ms);
}

int mesh_relay_rollback_forward_admission(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_relay_result *admission)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool rolled_back = false;

    if (relay == NULL || packet == NULL || admission == NULL ||
        (payload_len != 0u && payload == NULL) ||
        packet->payload_len != payload_len ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }

    if (packet->msg_type == MSG_COMMAND &&
        packet->dst_id == MESH_BROADCAST_ID) {
        rolled_back = broadcast_command_replay_rollback(
            &relay->command_replay,
            duplicate_session_identity(packet, payload, payload_len));
    } else {
        if (!relay_semantic_packet_digest(packet,
                                          payload,
                                          payload_len,
                                          semantic_digest)) {
            return PROTO_ERR_ARG;
        }
        for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
            struct mesh_duplicate_entry *entry = &relay->duplicates[i];

            if (!entry->valid || !entry->delivery_accepted ||
                !entry->semantic_identity_valid ||
                entry->msg_type != packet->msg_type ||
                entry->src_id != packet->src_id ||
                entry->dst_id != packet->dst_id ||
                entry->session_id !=
                    duplicate_session_identity(packet, payload, payload_len) ||
                entry->seq != packet->seq ||
                !semantic_digest_equal(entry->semantic_digest,
                                       semantic_digest,
                                       sizeof(semantic_digest))) {
                continue;
            }
            entry->valid = false;
            rolled_back = true;
            break;
        }
    }

    return rolled_back ? PROTO_OK : PROTO_ERR_NOT_FOUND;
}

static bool duplicate_busy_response_due(struct mesh_relay *relay,
                                        const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry = NULL;

    if (!duplicate_tracked(packet)) {
        return true;
    }
    duplicate_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        if (duplicate_matches_packet(&relay->duplicates[i], packet,
                                     payload, payload_len)) {
            entry = &relay->duplicates[i];
            break;
        }
    }
    if (entry == NULL) {
        entry = &relay->duplicates[relay->duplicate_next];
        relay->duplicate_next = (uint8_t)((relay->duplicate_next + 1u) %
                                          MESH_RELAY_DUP_CACHE_SIZE);
        entry->msg_type = packet->msg_type;
        entry->src_id = packet->src_id;
        entry->dst_id = packet->dst_id;
        entry->session_id = duplicate_session_identity(packet, payload,
                                                       payload_len);
        entry->last_seen_ms = now_ms;
        entry->seq = packet->seq;
        memset(entry->semantic_digest, 0, sizeof(entry->semantic_digest));
        entry->busy_response_interval_ms = 0u;
        entry->semantic_identity_valid = false;
        entry->delivery_accepted = false;
        entry->valid = true;
    }
    if (entry->busy_response_interval_ms > 0u &&
        (uint32_t)(now_ms - entry->busy_response_at_ms) <
            entry->busy_response_interval_ms) {
        return false;
    }
    entry->busy_response_at_ms = now_ms;
    entry->busy_response_interval_ms = relay_busy_retry_after_ms(relay);
    return true;
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

    ret = mesh_route_path_append(&forwarded.path, local_id);
    if (ret != PROTO_OK) {
        return ret;
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
    out->packet.ttl = (uint8_t)(packet->ttl - 1u);
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
    out->flood_retry_count = 0u;
    flood_control.random_backoff_max_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS;
    flood_control.random_backoff_slot_ms = FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS;
    flood_control.retry_count = 0u;
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(local_id,
                                                                forwarded.slot_seed,
                                                                forwarded.hop_count,
                                                                &flood_control,
                                                                random_value);
    out->earliest_tx_valid = true;
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

static bool command_result_id_is_well_formed(
    const struct command_result_id *id)
{
    return id != NULL &&
           id->gateway_id != 0u &&
           id->gateway_epoch != 0u &&
           id->command_seq != 0u &&
           id_is_unicast(id->node_id) &&
           id->node_boot_counter != 0u &&
           id->result_seq != 0u;
}

static void result_offer_reservation_clear(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->result_offer_reservation, 0,
               sizeof(relay->result_offer_reservation));
        relay->result_offer_reservation_deadline_ms = 0u;
    }
}

static bool result_offer_reservation_expire(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    const struct mesh_result_offer_reservation *reservation;

    if (relay == NULL) {
        return false;
    }
    reservation = &relay->result_offer_reservation;
    if (!reservation->valid) {
        relay->result_offer_reservation_deadline_ms = 0u;
        return false;
    }
    if (reservation->result_id.gateway_epoch !=
            (uint16_t)relay->upstream.current_epoch ||
        deadline_reached(now_ms,
                         relay->result_offer_reservation_deadline_ms)) {
        result_offer_reservation_clear(relay);
        return true;
    }
    return false;
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
           semantic_digest_equal(reservation->result_digest,
                                 offer->result_digest,
                                 SEMANTIC_DIGEST_SHA256_LEN) &&
           command_result_id_matches(&reservation->result_id, &offer->result_id);
}

static int result_offer_reserve(struct mesh_relay *relay,
                                uint64_t child_id,
                                const struct result_offer *offer,
                                uint32_t now_ms)
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
    memcpy(reservation->result_digest,
           offer->result_digest,
           sizeof(reservation->result_digest));
    relay->result_offer_reservation_deadline_ms =
        now_ms + MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u;
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
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
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
    if (!semantic_digest_sha256(payload, payload_len, payload_digest)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!command_result_id_matches(&result_id, &reservation->result_id) ||
        !semantic_digest_equal(payload_digest,
                               reservation->result_digest,
                               sizeof(payload_digest))) {
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
    if (!command_result_id_is_well_formed(&offer.result_id) ||
        offer.result_id.gateway_id != relay->gateway_id ||
        offer.result_id.gateway_epoch != (uint16_t)relay->upstream.current_epoch ||
        offer.result_id.node_id != pending->packet.src_id ||
        offer.result_id.command_seq != pending->packet.session_id ||
        offer.result_id.result_seq != pending->packet.seq) {
        return PROTO_ERR_MALFORMED;
    }

    offer.result_len = pending->payload_len;
    offer.result_crc = proto_crc16_ccitt_false(pending->payload, pending->payload_len);
    if (!semantic_digest_sha256(pending->payload,
                                pending->payload_len,
                                offer.result_digest)) {
        return PROTO_ERR_MALFORMED;
    }
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
    out->queued_at_valid = true;
    out->earliest_tx_valid = true;
    return PROTO_OK;
}

static int outbound_from_pending(const struct mesh_relay *relay,
                                 const struct mesh_pending_tx *pending,
                                 uint32_t now_ms,
                                 struct mesh_outbound *out)
{
    size_t confirm_payload_len = 0u;
    int ret;

    if (relay == NULL || pending == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!id_is_unicast(pending->next_hop_id)) {
        memset(out, 0, sizeof(*out));
        return PROTO_ERR_NOT_FOUND;
    }
    if (pending->result_offer_active) {
        return build_result_offer_from_pending(relay, pending, now_ms, out);
    }
    if (pending->gateway_ack_confirm_pending) {
        memset(out, 0, sizeof(*out));
        ret = mesh_relay_pending_gateway_ack_confirm_wire(
            relay,
            now_ms,
            &out->packet,
            out->payload,
            sizeof(out->payload),
            &confirm_payload_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        out->payload_len = (uint16_t)confirm_payload_len;
        out->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        out->next_hop_id = pending->next_hop_id;
        out->queued_at_ms = now_ms;
        out->earliest_tx_ms = now_ms;
        out->queued_at_valid = true;
        out->earliest_tx_valid = true;
        return PROTO_OK;
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
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
    out->ingress_previous_hop_id = pending->transit_previous_hop_id;
#endif
    out->queued_at_ms = now_ms;
    out->earliest_tx_ms = now_ms;
    out->queued_at_valid = true;
    out->earliest_tx_valid = true;
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
