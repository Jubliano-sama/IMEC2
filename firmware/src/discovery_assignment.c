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
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
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
    ret = tlv_find_unique(payload, payload_len, type, &raw, &raw_len);
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

int discovery_assignment_reconcile_epoch_baseline(
    uint32_t cursor_epoch,
    uint32_t proof_epoch,
    uint32_t *resolved_epoch,
    bool *cursor_repair_required)
{
    if (resolved_epoch == NULL || cursor_repair_required == NULL) {
        return PROTO_ERR_ARG;
    }

    *resolved_epoch = 0u;
    *cursor_repair_required = false;
    if (cursor_epoch == 0u) {
        *resolved_epoch = proof_epoch;
        *cursor_repair_required = proof_epoch != 0u;
        return PROTO_OK;
    }
    if (proof_epoch == 0u || cursor_epoch == proof_epoch ||
        discovery_assignment_epoch_strictly_newer(cursor_epoch,
                                                   proof_epoch)) {
        *resolved_epoch = cursor_epoch;
        return PROTO_OK;
    }
    if (discovery_assignment_epoch_strictly_newer(proof_epoch,
                                                   cursor_epoch)) {
        *resolved_epoch = proof_epoch;
        *cursor_repair_required = true;
        return PROTO_OK;
    }

    /*
     * Unequal nonzero serials for which neither is newer differ by exactly
     * half the uint32_t range. RFC 1982 deliberately leaves that ordering
     * undefined, so recovery cannot safely reserve from either side.
     */
    return PROTO_ERR_STALE;
}

bool discovery_assignment_response_custody_matches(
    bool active,
    uint32_t pending_epoch,
    enum discovery_assignment_phase pending_phase,
    uint32_t incoming_epoch,
    enum discovery_assignment_phase incoming_phase)
{
    return active &&
           pending_epoch != 0u &&
           pending_epoch == incoming_epoch &&
           (pending_phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM ||
            pending_phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) &&
           pending_phase == incoming_phase;
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

int discovery_assignment_order_roster_extension(uint64_t *anchor_ids,
                                                 size_t anchor_count,
                                                 size_t prior_anchor_count)
{
    int ret;

    if ((anchor_ids == NULL && anchor_count != 0u) ||
        anchor_count > UWB_DISCOVERY_SLOT_COUNT ||
        prior_anchor_count > anchor_count) {
        return PROTO_ERR_ARG;
    }
    if (anchor_count == 0u) {
        return PROTO_OK;
    }
    for (size_t i = 0u; i < anchor_count; i++) {
        if (anchor_ids[i] == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            if (anchor_ids[j] == anchor_ids[i]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    ret = discovery_assignment_sort_anchor_ids(
        &anchor_ids[prior_anchor_count],
        anchor_count - prior_anchor_count);
    return ret;
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
                        TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
                        DISCOVERY_ASSIGNMENT_SCHEME_VERSION);
    if (ret != PROTO_OK) {
        return ret;
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
    uint8_t scheme_version = 0u;
    uint8_t raw_phase = 0u;
    int ret;

    if (payload == NULL || phase == NULL || epoch == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = find_u8(payload,
                  payload_len,
                  TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
                  &scheme_version);
    if (ret != PROTO_OK ||
        scheme_version != DISCOVERY_ASSIGNMENT_SCHEME_VERSION) {
        return PROTO_ERR_MALFORMED;
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

int discovery_assignment_parse_result_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct discovery_assignment_result *result)
{
    struct discovery_assignment_result parsed = {0};
    const uint8_t *raw = NULL;
    uint8_t raw_len = 0u;
    uint16_t command_id;
    uint16_t status;
    uint8_t reason;
    int ret;

    if (payload == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, TLV_COMMAND_ID,
                          &raw, &raw_len);
    if (ret != PROTO_OK || raw_len != sizeof(uint16_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    command_id = proto_get_u16_le(raw);
    if (command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return PROTO_ERR_NOT_FOUND;
    }

    ret = tlv_find_unique(payload, payload_len, TLV_COMMAND_STATUS,
                          &raw, &raw_len);
    if (ret != PROTO_OK || raw_len != sizeof(uint16_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    status = proto_get_u16_le(raw);
    if (status != COMMAND_OK) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_find_unique(payload, payload_len, TLV_REASON, &raw, &raw_len);
    if (ret != PROTO_OK || raw_len != sizeof(uint8_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    reason = raw[0];
    if (reason != 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = discovery_assignment_extract_control_tlvs(
        payload, payload_len, &parsed.phase, &parsed.epoch);
    if (ret != PROTO_OK ||
        (parsed.phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         parsed.phase != DISCOVERY_ASSIGNMENT_PHASE_ACK)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    ret = discovery_assignment_extract_claim_hash(
        payload, payload_len, &parsed.hash);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_find_unique(
        payload,
        payload_len,
        TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT,
        &raw,
        &raw_len);
    if (parsed.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        if (ret != PROTO_OK ||
            raw_len != sizeof(parsed.table_commitment.bytes)) {
            return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
        }
        memcpy(parsed.table_commitment.bytes,
               raw,
               sizeof(parsed.table_commitment.bytes));
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }

    ret = tlv_find_unique(payload, payload_len, TLV_HOP_COUNT,
                          &raw, &raw_len);
    if (ret == PROTO_OK) {
        if (raw_len != sizeof(uint8_t)) {
            return PROTO_ERR_MALFORMED;
        }
        parsed.hop_count = raw[0];
        parsed.hop_count_present = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }

    /*
     * Batch metadata is an optional transport extension, but either field
     * alone, a duplicate, a zero ID, or unknown flags is malformed.
     */
    {
        const uint8_t *batch_id_raw = NULL;
        const uint8_t *batch_flags_raw = NULL;
        uint8_t batch_id_len = 0u;
        uint8_t batch_flags_len = 0u;
        int batch_id_ret = tlv_find_unique(
            payload, payload_len, TLV_MESH_CH9_BATCH_ID,
            &batch_id_raw, &batch_id_len);
        int batch_flags_ret = tlv_find_unique(
            payload, payload_len, TLV_MESH_CH9_BATCH_FLAGS,
            &batch_flags_raw, &batch_flags_len);

        if (batch_id_ret != batch_flags_ret) {
            return PROTO_ERR_MALFORMED;
        }
        if (batch_id_ret != PROTO_OK &&
            batch_id_ret != PROTO_ERR_NOT_FOUND) {
            return batch_id_ret;
        }
        if (batch_id_ret == PROTO_OK &&
            (batch_id_len != sizeof(uint32_t) ||
             batch_flags_len != sizeof(uint8_t) ||
             proto_get_u32_le(batch_id_raw) == 0u ||
             (batch_flags_raw[0] & (uint8_t)~UINT8_C(0x01)) != 0u)) {
            return PROTO_ERR_MALFORMED;
        }
    }

    *result = parsed;
    return PROTO_OK;
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
                entry->slot >= UWB_DISCOVERY_SLOT_COUNT) {
                return PROTO_ERR_MALFORMED;
            }
            for (size_t prior = 0u; prior < entry_index + i; prior++) {
                if (entries[prior].anchor_id == entry->anchor_id ||
                    entries[prior].slot == entry->slot) {
                    return PROTO_ERR_MALFORMED;
                }
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

            if (anchor_id == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            for (size_t prior = 0u; prior < index; prior++) {
                if (anchor_ids[prior] == anchor_id) {
                    return PROTO_ERR_MALFORMED;
                }
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
    ret = tlv_find_unique(payload, payload_len, TLV_EXPECTED_NODE_COUNT,
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
                    entry->slot >= configured_slot_count) {
                    return PROTO_ERR_MALFORMED;
                }
                for (size_t prior = 0u; prior < count; prior++) {
                    if (entries[prior].anchor_id == entry->anchor_id ||
                        entries[prior].slot == entry->slot) {
                        return PROTO_ERR_MALFORMED;
                    }
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

bool discovery_assignment_table_commitment(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t slot_count,
    struct discovery_assignment_table_commitment *commitment)
{
    static const uint8_t domain[] = {
        'I', 'M', 'E', 'C', '-', 'A', 'S', 'S', 'I', 'G', 'N',
        'M', 'E', 'N', 'T', '-', 'T', 'A', 'B', 'L', 'E',
        DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
    };
    struct semantic_digest_sha256_context context;
    uint8_t header[2];

    if (commitment == NULL) {
        return false;
    }
    memset(commitment, 0, sizeof(*commitment));
    if (entries == NULL || entry_count == 0u ||
        entry_count > UWB_DISCOVERY_SLOT_COUNT || slot_count == 0u ||
        slot_count > UWB_DISCOVERY_SLOT_COUNT || entry_count > slot_count) {
        return false;
    }
    if (!semantic_digest_sha256_init(&context) ||
        !semantic_digest_sha256_update(&context, domain, sizeof(domain))) {
        return false;
    }
    header[0] = slot_count;
    header[1] = (uint8_t)entry_count;
    if (!semantic_digest_sha256_update(&context, header, sizeof(header))) {
        return false;
    }

    for (size_t i = 0u; i < entry_count; i++) {
        const struct discovery_assignment_entry *entry = &entries[i];

        if (entry->anchor_id == 0u ||
            entry->hash != discovery_assignment_hash(entry->anchor_id) ||
            entry->slot >= slot_count) {
            return false;
        }
        for (size_t prior = 0u; prior < i; prior++) {
            if (entries[prior].anchor_id == entry->anchor_id ||
                entries[prior].slot == entry->slot) {
                return false;
            }
        }
    }

    /*
     * Hash by explicit slot rather than arrival order. Two valid TABLE
     * encodings of the same sparse membership therefore have one semantic
     * commitment, while the slot byte still binds every gap and owner.
     */
    for (uint8_t slot = 0u; slot < slot_count; slot++) {
        const struct discovery_assignment_entry *entry = NULL;
        uint8_t encoded[DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN];

        for (size_t i = 0u; i < entry_count; i++) {
            if (entries[i].slot == slot) {
                entry = &entries[i];
                break;
            }
        }
        if (entry == NULL) {
            continue;
        }
        proto_put_u64_le(encoded, entry->anchor_id);
        proto_put_u64_le(&encoded[8], entry->hash);
        encoded[16] = entry->slot;
        if (!semantic_digest_sha256_update(
                &context, encoded, sizeof(encoded))) {
            return false;
        }
    }
    return semantic_digest_sha256_final(&context, commitment->bytes);
}

bool discovery_assignment_table_commitment_equal(
    const struct discovery_assignment_table_commitment *left,
    const struct discovery_assignment_table_commitment *right)
{
    return left != NULL && right != NULL &&
           semantic_digest_equal(left->bytes,
                                 right->bytes,
                                 sizeof(left->bytes));
}

int discovery_assignment_append_table_commitment(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct discovery_assignment_table_commitment *commitment)
{
    if (commitment == NULL) {
        return PROTO_ERR_ARG;
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT,
                            commitment->bytes,
                            sizeof(commitment->bytes));
}

int discovery_assignment_response_delay_ms(uint8_t slot,
                                           uint8_t slot_count,
                                           uint8_t hop_count,
                                           uint16_t response_spread_ms,
                                           uint8_t retry_round,
                                           uint32_t random_value,
                                           uint32_t *delay_ms)
{
    uint32_t effective_hop_count;
    uint32_t farthest_first_hop_band;
    uint32_t slot_width_ms;
    uint32_t hop_band_ms;
    uint32_t retry_base;
    uint64_t delay;

    if (delay_ms == NULL || slot_count == 0u ||
        slot_count > UWB_DISCOVERY_SLOT_COUNT || slot >= slot_count ||
        response_spread_ms < DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS ||
        response_spread_ms > DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) {
        return PROTO_ERR_ARG;
    }

    effective_hop_count = hop_count == 0u ? DISCOVERY_ASSIGNMENT_MAX_HOPS :
                          hop_count > DISCOVERY_ASSIGNMENT_MAX_HOPS ?
                          DISCOVERY_ASSIGNMENT_MAX_HOPS : hop_count;
    farthest_first_hop_band =
        DISCOVERY_ASSIGNMENT_MAX_HOPS - effective_hop_count;
    slot_width_ms = DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_WIDTH_MS(
        response_spread_ms, slot_count);
    hop_band_ms = DISCOVERY_ASSIGNMENT_RESPONSE_HOP_BAND_MS(
        response_spread_ms, slot_count);
    retry_base = retry_base_ms(retry_round);
    delay = DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS +
            ((uint64_t)farthest_first_hop_band * hop_band_ms) +
            ((uint64_t)slot * slot_width_ms) +
            (retry_round == 0u ? 0u : retry_base) +
            (random_value % slot_width_ms);
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
           DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS(
               response_spread_ms);
}

uint32_t discovery_assignment_table_collection_window_ms(
    uint16_t response_spread_ms,
    uint8_t max_hop_count)
{
    uint32_t first_handle_window_ms =
        discovery_assignment_collection_window_ms(response_spread_ms,
                                                  max_hop_count);
    uint32_t response_custody_ms;
    uint64_t table_window_ms;

    if (first_handle_window_ms == 0u) {
        return 0u;
    }
    response_custody_ms =
        discovery_assignment_response_custody_ms(max_hop_count);
    table_window_ms =
        (uint64_t)first_handle_window_ms +
        ((uint64_t)DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES *
         response_custody_ms) +
        DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS;
    return table_window_ms > UINT32_MAX ? UINT32_MAX :
                                          (uint32_t)table_window_ms;
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

bool discovery_assignment_ack_quorum_settle_should_arm(
    bool settle_armed,
    uint8_t missing_ack_count)
{
    return missing_ack_count == 0u && !settle_armed;
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
    ret = tlv_find_unique(payload, payload_len, TLV_EXPECTED_NODE_COUNT,
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
