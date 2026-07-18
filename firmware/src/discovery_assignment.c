#include "discovery_assignment.h"

#include <stdbool.h>
#include <string.h>

static bool phase_valid(enum discovery_assignment_phase phase)
{
    return phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM ||
           phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE ||
           phase == DISCOVERY_ASSIGNMENT_PHASE_ACK;
}

static uint32_t retry_base_ms(uint8_t retry_round)
{
    uint32_t base_ms = DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;

    for (uint8_t i = 0u; i < retry_round; i++) {
        if (base_ms >= DISCOVERY_ASSIGNMENT_RETRY_MAX_MS / 2u) {
            return DISCOVERY_ASSIGNMENT_RETRY_MAX_MS;
        }
        base_ms *= 2u;
    }
    return base_ms > DISCOVERY_ASSIGNMENT_RETRY_MAX_MS ?
           DISCOVERY_ASSIGNMENT_RETRY_MAX_MS : base_ms;
}

static int find_u8(const uint8_t *payload,
                   size_t payload_len,
                   uint8_t type,
                   uint8_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = raw[0];
    return PROTO_OK;
}

static int find_u32(const uint8_t *payload,
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
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(raw);
    return PROTO_OK;
}

static int find_u64(const uint8_t *payload,
                    size_t payload_len,
                    uint8_t type,
                    uint64_t *value)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &raw, &raw_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (raw_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u64_le(raw);
    return PROTO_OK;
}

uint64_t discovery_assignment_hash(uint64_t anchor_id)
{
    uint64_t value = anchor_id;

    if (anchor_id == 0u) {
        return 0u;
    }
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value == 0u ? 1u : value;
}

bool discovery_assignment_response_custody_matches(
    bool active,
    uint32_t pending_epoch,
    enum discovery_assignment_phase pending_phase,
    uint32_t pending_session_id,
    uint32_t incoming_epoch,
    enum discovery_assignment_phase incoming_phase,
    uint32_t incoming_session_id)
{
    return active &&
           pending_epoch != 0u &&
           pending_epoch == incoming_epoch &&
           phase_valid(pending_phase) &&
           pending_phase == incoming_phase &&
           (pending_phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM ||
            (pending_session_id != 0u &&
             pending_session_id == incoming_session_id));
}

static bool claim_before(const struct discovery_assignment_claim *left,
                         const struct discovery_assignment_claim *right)
{
    return left->hash < right->hash ||
           (left->hash == right->hash && left->anchor_id < right->anchor_id);
}

int discovery_assignment_sort_claims(struct discovery_assignment_claim *claims,
                                     size_t claim_count)
{
    if ((claims == NULL && claim_count != 0u) ||
        claim_count > UWB_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_ARG;
    }
    for (size_t i = 0u; i < claim_count; i++) {
        struct discovery_assignment_claim current = claims[i];
        size_t j = i;

        if (current.anchor_id == 0u ||
            current.hash != discovery_assignment_hash(current.anchor_id)) {
            return PROTO_ERR_MALFORMED;
        }
        while (j > 0u && claim_before(&current, &claims[j - 1u])) {
            claims[j] = claims[j - 1u];
            j--;
        }
        claims[j] = current;
    }
    for (size_t i = 1u; i < claim_count; i++) {
        if (claims[i - 1u].anchor_id == claims[i].anchor_id) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

static bool anchor_id_before(uint64_t left, uint64_t right)
{
    uint64_t left_hash = discovery_assignment_hash(left);
    uint64_t right_hash = discovery_assignment_hash(right);

    return left_hash < right_hash ||
           (left_hash == right_hash && left < right);
}

int discovery_assignment_sort_anchor_ids(uint64_t *anchor_ids,
                                         size_t anchor_count)
{
    if ((anchor_ids == NULL && anchor_count != 0u) ||
        anchor_count > UWB_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_ARG;
    }
    for (size_t i = 0u; i < anchor_count; i++) {
        uint64_t current = anchor_ids[i];
        size_t j = i;

        if (current == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        while (j > 0u && anchor_id_before(current, anchor_ids[j - 1u])) {
            anchor_ids[j] = anchor_ids[j - 1u];
            j--;
        }
        anchor_ids[j] = current;
    }
    for (size_t i = 1u; i < anchor_count; i++) {
        if (anchor_ids[i - 1u] == anchor_ids[i]) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

int discovery_assignment_entries_from_claims(
    const struct discovery_assignment_claim *claims,
    size_t claim_count,
    struct discovery_assignment_entry *entries,
    size_t entry_cap)
{
    if ((claims == NULL && claim_count != 0u) ||
        (entries == NULL && claim_count != 0u) ||
        claim_count == 0u || claim_count > UWB_DISCOVERY_SLOT_COUNT ||
        entry_cap < claim_count) {
        return PROTO_ERR_ARG;
    }
    for (size_t i = 0u; i < claim_count; i++) {
        if (claims[i].anchor_id == 0u ||
            claims[i].hash != discovery_assignment_hash(claims[i].anchor_id) ||
            (i > 0u && !claim_before(&claims[i - 1u], &claims[i]))) {
            return PROTO_ERR_MALFORMED;
        }
        entries[i].anchor_id = claims[i].anchor_id;
        entries[i].hash = claims[i].hash;
        entries[i].slot = (uint8_t)i;
    }
    return PROTO_OK;
}

int discovery_assignment_append_control_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    enum discovery_assignment_phase phase,
    uint32_t epoch)
{
    int ret;

    if (!phase_valid(phase) || epoch == 0u) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_DISCOVERY_ASSIGNMENT_PHASE, (uint8_t)phase);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset,
                          TLV_DISCOVERY_ASSIGNMENT_EPOCH, epoch);
}

int discovery_assignment_extract_control_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    enum discovery_assignment_phase *phase,
    uint32_t *epoch)
{
    uint8_t raw_phase = 0u;
    int ret;

    if (payload == NULL || phase == NULL || epoch == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = find_u8(payload, payload_len, TLV_DISCOVERY_ASSIGNMENT_PHASE,
                  &raw_phase);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32(payload, payload_len, TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                   epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    *phase = (enum discovery_assignment_phase)raw_phase;
    return phase_valid(*phase) && *epoch != 0u ? PROTO_OK : PROTO_ERR_MALFORMED;
}

int discovery_assignment_append_claim_hash(uint8_t *payload,
                                           size_t payload_cap,
                                           size_t *offset,
                                           uint64_t hash)
{
    if (hash == 0u) {
        return PROTO_ERR_ARG;
    }
    return tlv_append_u64(payload, payload_cap, offset,
                          TLV_DISCOVERY_ASSIGNMENT_HASH, hash);
}

int discovery_assignment_extract_claim_hash(const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t *hash)
{
    int ret = find_u64(payload, payload_len,
                       TLV_DISCOVERY_ASSIGNMENT_HASH, hash);

    if (ret != PROTO_OK) {
        return ret;
    }
    return *hash == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

int discovery_assignment_append_table_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_entry *entries,
    size_t entry_count)
{
    uint8_t raw[DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV *
                DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN];
    size_t entry_index = 0u;
    int ret;

    if (payload == NULL || offset == NULL || entries == NULL ||
        entry_count == 0u || entry_count > UWB_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_DISCOVERY_SLOT_COUNT, UWB_DISCOVERY_SLOT_COUNT);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_EXPECTED_NODE_COUNT, (uint16_t)entry_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    while (entry_index < entry_count) {
        size_t chunk_count = entry_count - entry_index;
        size_t raw_len;

        if (chunk_count > DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV) {
            chunk_count = DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV;
        }
        raw_len = chunk_count * DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN;
        for (size_t i = 0u; i < chunk_count; i++) {
            const struct discovery_assignment_entry *entry =
                &entries[entry_index + i];
            size_t raw_offset = i * DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN;

            if (entry->anchor_id == 0u ||
                entry->hash != discovery_assignment_hash(entry->anchor_id) ||
                entry->slot != entry_index + i ||
                (entry_index + i > 0u &&
                 (entries[entry_index + i - 1u].hash > entry->hash ||
                  (entries[entry_index + i - 1u].hash == entry->hash &&
                   entries[entry_index + i - 1u].anchor_id >= entry->anchor_id)))) {
                return PROTO_ERR_MALFORMED;
            }
            proto_put_u64_le(&raw[raw_offset], entry->anchor_id);
            proto_put_u64_le(&raw[raw_offset + 8u], entry->hash);
            raw[raw_offset + 16u] = entry->slot;
        }
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_DISCOVERY_ASSIGNMENT_TABLE,
                               raw, (uint8_t)raw_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        entry_index += chunk_count;
    }
    return PROTO_OK;
}

int discovery_assignment_append_table_from_anchor_ids(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint64_t *anchor_ids,
    size_t anchor_count)
{
    uint8_t raw[DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV *
                DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN];
    size_t anchor_index = 0u;
    int ret;

    if (payload == NULL || offset == NULL || anchor_ids == NULL ||
        anchor_count == 0u || anchor_count > UWB_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_DISCOVERY_SLOT_COUNT, UWB_DISCOVERY_SLOT_COUNT);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_EXPECTED_NODE_COUNT, (uint16_t)anchor_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    while (anchor_index < anchor_count) {
        size_t chunk_count = anchor_count - anchor_index;
        size_t raw_len;

        if (chunk_count > DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV) {
            chunk_count = DISCOVERY_ASSIGNMENT_ENTRIES_PER_TLV;
        }
        raw_len = chunk_count * DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN;
        for (size_t i = 0u; i < chunk_count; i++) {
            size_t index = anchor_index + i;
            size_t raw_offset = i * DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN;
            uint64_t anchor_id = anchor_ids[index];
            uint64_t hash = discovery_assignment_hash(anchor_id);

            if (anchor_id == 0u ||
                (index > 0u &&
                 !anchor_id_before(anchor_ids[index - 1u], anchor_id))) {
                return PROTO_ERR_MALFORMED;
            }
            proto_put_u64_le(&raw[raw_offset], anchor_id);
            proto_put_u64_le(&raw[raw_offset + 8u], hash);
            raw[raw_offset + 16u] = (uint8_t)index;
        }
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_DISCOVERY_ASSIGNMENT_TABLE,
                               raw, (uint8_t)raw_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        anchor_index += chunk_count;
    }
    return PROTO_OK;
}

int discovery_assignment_parse_table_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    uint8_t *slot_count)
{
    size_t offset = 0u;
    size_t count = 0u;
    const uint8_t *expected_count_raw = NULL;
    uint8_t expected_count_len = 0u;
    uint16_t expected_count = 0u;
    uint8_t configured_slot_count = 0u;
    int ret;

    if (payload == NULL || entries == NULL || entry_count == NULL ||
        slot_count == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = find_u8(payload, payload_len, TLV_DISCOVERY_SLOT_COUNT,
                  &configured_slot_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find(payload, payload_len, TLV_EXPECTED_NODE_COUNT,
                   &expected_count_raw, &expected_count_len);
    if (ret != PROTO_OK || expected_count_len != sizeof(uint16_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    expected_count = proto_get_u16_le(expected_count_raw);
    if (configured_slot_count == 0u ||
        configured_slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        expected_count == 0u || expected_count > configured_slot_count) {
        return PROTO_ERR_MALFORMED;
    }
    if (entry_cap < expected_count) {
        return PROTO_ERR_NO_SPACE;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += 2u;
        if (payload_len - offset < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_DISCOVERY_ASSIGNMENT_TABLE) {
            if (len == 0u || len % DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            for (size_t raw_offset = 0u; raw_offset < len;
                 raw_offset += DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN) {
                struct discovery_assignment_entry *entry;

                if (count >= expected_count || count >= entry_cap) {
                    return PROTO_ERR_MALFORMED;
                }
                entry = &entries[count];
                entry->anchor_id = proto_get_u64_le(&payload[offset + raw_offset]);
                entry->hash = proto_get_u64_le(&payload[offset + raw_offset + 8u]);
                entry->slot = payload[offset + raw_offset + 16u];
                if (entry->anchor_id == 0u ||
                    entry->hash != discovery_assignment_hash(entry->anchor_id) ||
                    entry->slot != count || entry->slot >= configured_slot_count ||
                    (count > 0u &&
                     (entries[count - 1u].hash > entry->hash ||
                      (entries[count - 1u].hash == entry->hash &&
                       entries[count - 1u].anchor_id >= entry->anchor_id)))) {
                    return PROTO_ERR_MALFORMED;
                }
                count++;
            }
        }
        offset += len;
    }
    if (count != expected_count) {
        return PROTO_ERR_MALFORMED;
    }
    *entry_count = count;
    *slot_count = configured_slot_count;
    return PROTO_OK;
}

uint32_t discovery_assignment_table_fingerprint(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t slot_count)
{
    uint32_t hash = UINT32_C(2166136261);

    if (entries == NULL || entry_count == 0u ||
        entry_count > UWB_DISCOVERY_SLOT_COUNT || slot_count == 0u ||
        slot_count > UWB_DISCOVERY_SLOT_COUNT || entry_count > slot_count) {
        return 0u;
    }
    hash = (hash ^ slot_count) * UINT32_C(16777619);
    hash = (hash ^ (uint8_t)entry_count) * UINT32_C(16777619);
    for (size_t i = 0u; i < entry_count; i++) {
        const struct discovery_assignment_entry *entry = &entries[i];
        uint8_t encoded[DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN];

        if (entry->anchor_id == 0u ||
            entry->hash != discovery_assignment_hash(entry->anchor_id) ||
            entry->slot != i || entry->slot >= slot_count) {
            return 0u;
        }
        proto_put_u64_le(encoded, entry->anchor_id);
        proto_put_u64_le(&encoded[8], entry->hash);
        encoded[16] = entry->slot;
        for (size_t j = 0u; j < sizeof(encoded); j++) {
            hash = (hash ^ encoded[j]) * UINT32_C(16777619);
        }
    }
    return hash == 0u ? 1u : hash;
}

int discovery_assignment_response_delay_ms(uint16_t response_spread_ms,
                                           uint8_t retry_round,
                                           uint32_t random_value,
                                           uint32_t *delay_ms)
{
    uint32_t retry_base;
    uint32_t jitter_window;
    uint64_t delay;

    if (delay_ms == NULL ||
        response_spread_ms < DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        response_spread_ms > DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return PROTO_ERR_ARG;
    }

    retry_base = retry_base_ms(retry_round);
    jitter_window = retry_round == 0u ? response_spread_ms :
                    retry_base;
    delay = DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS +
            (retry_round == 0u ? 0u : retry_base) +
            (random_value % jitter_window);
    if (delay > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }

    *delay_ms = (uint32_t)delay;
    return PROTO_OK;
}

uint32_t discovery_assignment_retry_backoff_ms(uint8_t retry_round,
                                               uint32_t random_value)
{
    uint32_t base_ms = retry_base_ms(retry_round);

    return base_ms + (random_value % base_ms);
}

uint32_t discovery_assignment_response_custody_ms(uint8_t hop_count)
{
    uint8_t effective_hop_count =
        hop_count == 0u ? DISCOVERY_ASSIGNMENT_MAX_HOPS :
        hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
            DISCOVERY_ASSIGNMENT_MAX_HOPS : hop_count;

    return DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS +
           ((uint32_t)(effective_hop_count - 1u) *
            DISCOVERY_ASSIGNMENT_RESPONSE_PER_ADDITIONAL_HOP_MS);
}

uint64_t discovery_assignment_response_deadline_ms(uint64_t now_ms,
                                                   uint32_t response_delay_ms,
                                                   uint8_t hop_count)
{
    uint64_t response_window_ms =
        (uint64_t)response_delay_ms +
        discovery_assignment_response_custody_ms(hop_count);

    return UINT64_MAX - now_ms < response_window_ms ?
           UINT64_MAX : now_ms + response_window_ms;
}

uint16_t discovery_assignment_membership_epoch(uint32_t assignment_epoch)
{
    uint16_t membership_epoch =
        (uint16_t)(assignment_epoch ^ (assignment_epoch >> 16u));

    return membership_epoch == 0u ? 1u : membership_epoch;
}

uint32_t discovery_assignment_collection_window_ms(uint16_t response_spread_ms,
                                                   uint8_t max_hop_count)
{
    uint32_t effective_hop_count;

    if (response_spread_ms < DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        response_spread_ms > DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return 0u;
    }
    effective_hop_count = max_hop_count == 0u ? DISCOVERY_ASSIGNMENT_MAX_HOPS :
                          max_hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
                          DISCOVERY_ASSIGNMENT_MAX_HOPS : max_hop_count;
    return discovery_assignment_response_custody_ms(
               (uint8_t)effective_hop_count) +
           DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS +
           response_spread_ms - 1u;
}

uint64_t discovery_assignment_control_flood_deadline_ms(
    uint64_t now_ms,
    uint64_t operation_deadline_ms)
{
    uint64_t flood_deadline_ms;

    if (operation_deadline_ms <= now_ms) {
        return operation_deadline_ms;
    }
    flood_deadline_ms = UINT64_MAX - now_ms <
                            DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS ?
                        UINT64_MAX :
                        now_ms + DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS;
    return flood_deadline_ms < operation_deadline_ms ? flood_deadline_ms :
                                                       operation_deadline_ms;
}

uint64_t discovery_assignment_response_ack_settle_deadline_ms(uint64_t now_ms)
{
    return UINT64_MAX - now_ms < DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS ?
           UINT64_MAX : now_ms + DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS;
}

uint32_t discovery_assignment_claim_ack_settle_duration_ms(uint8_t hop_count)
{
    uint8_t effective_hop_count =
        hop_count == 0u || hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
            DISCOVERY_ASSIGNMENT_MAX_HOPS : hop_count;

    return DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS +
           ((uint32_t)(effective_hop_count - 1u) *
            DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS);
}

uint64_t discovery_assignment_claim_ack_settle_deadline_ms(
    uint64_t now_ms,
    uint8_t hop_count)
{
    uint32_t settle_ms =
        discovery_assignment_claim_ack_settle_duration_ms(hop_count);

    return UINT64_MAX - now_ms < settle_ms ? UINT64_MAX :
                                             now_ms + settle_ms;
}

bool discovery_assignment_response_ack_settle_pending(
    uint64_t now_ms,
    uint64_t settle_deadline_ms)
{
    return settle_deadline_ms != 0u && now_ms < settle_deadline_ms;
}

int discovery_assignment_extract_expected_count(const uint8_t *payload,
                                                 size_t payload_len,
                                                 uint16_t *expected_count,
                                                 bool *present)
{
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    int ret;

    if (payload == NULL || expected_count == NULL || present == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, TLV_EXPECTED_NODE_COUNT,
                   &raw, &raw_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *expected_count = 0u;
        *present = false;
        return PROTO_OK;
    }
    if (ret != PROTO_OK || raw_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *expected_count = proto_get_u16_le(raw);
    if (*expected_count == 0u || *expected_count > UWB_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_MALFORMED;
    }
    *present = true;
    return PROTO_OK;
}
