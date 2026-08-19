#include "discovery_assignment.h"

#include <assert.h>
#include <string.h>

static struct discovery_assignment_table_commitment
test_table_commitment(uint32_t value)
{
    struct discovery_assignment_table_commitment commitment = {0};

    proto_put_u32_le(commitment.bytes, value);
    return commitment;
}

static uint32_t legacy_assignment_fnv32(
    const struct discovery_assignment_entry *entry,
    uint8_t slot_count)
{
    uint8_t encoded[DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN];
    uint32_t hash = UINT32_C(2166136261);

    hash = (hash ^ slot_count) * UINT32_C(16777619);
    hash = (hash ^ 1u) * UINT32_C(16777619);
    proto_put_u64_le(encoded, entry->anchor_id);
    proto_put_u64_le(&encoded[8], entry->hash);
    encoded[16] = entry->slot;
    for (size_t i = 0u; i < sizeof(encoded); i++) {
        hash = (hash ^ encoded[i]) * UINT32_C(16777619);
    }
    return hash == 0u ? 1u : hash;
}

static void test_table_commitment_known_vector_and_legacy_collision(void)
{
    static const uint8_t known_digest[SEMANTIC_DIGEST_SHA256_LEN] = {
        0x9eu, 0x9eu, 0xcfu, 0xa3u, 0x0du, 0xe3u, 0xa7u, 0x5eu,
        0x25u, 0x89u, 0x17u, 0x06u, 0xc4u, 0x25u, 0x76u, 0xd6u,
        0x52u, 0xe4u, 0x8du, 0x04u, 0x0du, 0xccu, 0xa2u, 0x0fu,
        0x1eu, 0x36u, 0x28u, 0x86u, 0x2cu, 0x73u, 0x4du, 0x91u,
    };
    struct discovery_assignment_entry known = {
        .anchor_id = UINT64_C(0x0102030405060708),
        .slot = 7u,
    };
    struct discovery_assignment_entry colliding_legacy[2] = {
        {.anchor_id = UINT64_C(0x000000000000d7b3), .slot = 0u},
        {.anchor_id = UINT64_C(0x000000000000f105), .slot = 0u},
    };
    struct discovery_assignment_table_commitment known_commitment;
    struct discovery_assignment_table_commitment commitments[2];

    known.hash = discovery_assignment_hash(known.anchor_id);
    assert(known.hash == UINT64_C(0xd1af0fbb4178c20e));
    assert(discovery_assignment_table_commitment(
        &known, 1u, UWB_DISCOVERY_SLOT_COUNT, &known_commitment));
    assert(memcmp(known_commitment.bytes,
                  known_digest,
                  sizeof(known_digest)) == 0);

    for (size_t i = 0u; i < 2u; i++) {
        colliding_legacy[i].hash =
            discovery_assignment_hash(colliding_legacy[i].anchor_id);
        assert(discovery_assignment_table_commitment(
            &colliding_legacy[i],
            1u,
            UWB_DISCOVERY_SLOT_COUNT,
            &commitments[i]));
    }
    assert(legacy_assignment_fnv32(
               &colliding_legacy[0], UWB_DISCOVERY_SLOT_COUNT) ==
           UINT32_C(0x8e709b48));
    assert(legacy_assignment_fnv32(
               &colliding_legacy[1], UWB_DISCOVERY_SLOT_COUNT) ==
           UINT32_C(0x8e709b48));
    assert(!discovery_assignment_table_commitment_equal(
        &commitments[0], &commitments[1]));
}

static void build_sorted_entries(struct discovery_assignment_entry *entries,
                                 size_t count)
{
    struct discovery_assignment_claim claims[UWB_DISCOVERY_SLOT_COUNT];

    assert(count <= UWB_DISCOVERY_SLOT_COUNT);
    for (size_t i = 0u; i < count; i++) {
        claims[i].anchor_id = UINT64_C(0x2222222222222301) + i;
        claims[i].hash = discovery_assignment_hash(claims[i].anchor_id);
    }
    assert(discovery_assignment_sort_claims(claims, count) == PROTO_OK);
    assert(discovery_assignment_entries_from_claims(claims,
                                                    count,
                                                    entries,
                                                    count) == PROTO_OK);
}

static void test_hash_order_is_deterministic_and_tied_by_id(void)
{
    struct discovery_assignment_claim claims[] = {
        {UINT64_C(0x2222222222222303), 0u},
        {UINT64_C(0x2222222222222301), 0u},
        {UINT64_C(0x2222222222222302), 0u},
    };

    for (size_t i = 0u; i < sizeof(claims) / sizeof(claims[0]); i++) {
        claims[i].hash = discovery_assignment_hash(claims[i].anchor_id);
        assert(claims[i].hash != 0u);
    }
    assert(discovery_assignment_hash(0u) == 0u);
    assert(discovery_assignment_sort_claims(claims,
                                            sizeof(claims) / sizeof(claims[0])) ==
           PROTO_OK);
    for (size_t i = 1u; i < sizeof(claims) / sizeof(claims[0]); i++) {
        assert(claims[i - 1u].hash < claims[i].hash ||
               (claims[i - 1u].hash == claims[i].hash &&
                claims[i - 1u].anchor_id < claims[i].anchor_id));
    }

    claims[1] = claims[0];
    assert(discovery_assignment_sort_claims(claims,
                                            sizeof(claims) / sizeof(claims[0])) ==
           PROTO_ERR_MALFORMED);
}

static void test_compact_anchor_id_path_matches_entry_wire_format(void)
{
    uint64_t anchor_ids[] = {
        UINT64_C(0x2222222222222303),
        UINT64_C(0x2222222222222301),
        UINT64_C(0x2222222222222302),
    };
    struct discovery_assignment_claim claims[3];
    struct discovery_assignment_entry entries[3];
    uint8_t entry_payload[256];
    uint8_t id_payload[256];
    size_t entry_len = 0u;
    size_t id_len = 0u;

    for (size_t i = 0u; i < 3u; i++) {
        claims[i].anchor_id = anchor_ids[i];
        claims[i].hash = discovery_assignment_hash(anchor_ids[i]);
    }
    assert(discovery_assignment_sort_claims(claims, 3u) == PROTO_OK);
    assert(discovery_assignment_entries_from_claims(
               claims, 3u, entries, 3u) == PROTO_OK);
    assert(discovery_assignment_sort_anchor_ids(anchor_ids, 3u) == PROTO_OK);
    for (size_t i = 0u; i < 3u; i++) {
        assert(anchor_ids[i] == entries[i].anchor_id);
    }
    assert(discovery_assignment_append_table_tlvs(
               entry_payload, sizeof(entry_payload), &entry_len,
               entries, 3u) == PROTO_OK);
    assert(discovery_assignment_append_table_from_anchor_ids(
               id_payload, sizeof(id_payload), &id_len,
               anchor_ids, 3u) == PROTO_OK);
    assert(id_len == entry_len);
    assert(memcmp(id_payload, entry_payload, id_len) == 0);

    anchor_ids[1] = anchor_ids[0];
    assert(discovery_assignment_sort_anchor_ids(anchor_ids, 3u) ==
           PROTO_ERR_MALFORMED);
}

static void test_roster_extension_preserves_prior_slots(void)
{
    uint64_t anchor_ids[] = {
        UINT64_C(0xf000000000000011),
        UINT64_C(0xe000000000000022),
        UINT64_C(0x0000000000000003),
        UINT64_C(0x0000000000000001),
        UINT64_C(0x0000000000000002),
    };
    uint64_t expected_new[] = {
        anchor_ids[2],
        anchor_ids[3],
        anchor_ids[4],
    };
    const uint64_t prior_0 = anchor_ids[0];
    const uint64_t prior_1 = anchor_ids[1];
    struct discovery_assignment_entry decoded[5];
    uint8_t hop_counts[] = {1u, 2u, 3u, 4u, 5u};
    uint8_t payload[256];
    size_t payload_len = 0u;
    size_t decoded_count = 0u;
    uint8_t slot_count = 0u;
    uint64_t lower_hash_id = 1u;
    /* Hop-aware ordering: suffix sorted by hop count then hash.  The
     * expected suffix keeps hop order 3,1,2 (hops 3,4,5) which is already
     * hop-sorted, so no hash sort is applied.  The extension must preserve
     * that hop-aware order and keep hop sidecar aligned. */
    (void)expected_new;
    assert(discovery_assignment_order_roster_extension(
               anchor_ids,
               hop_counts,
               sizeof(anchor_ids) / sizeof(anchor_ids[0]),
               2u) == PROTO_OK);
    assert(anchor_ids[0] == prior_0);
    assert(anchor_ids[1] == prior_1);
    assert(memcmp(&anchor_ids[2], expected_new, sizeof(expected_new)) == 0);
    for (size_t i = 2u; i < 5u; i++) {
        if (anchor_ids[i] == UINT64_C(0x0000000000000003)) {
            assert(hop_counts[i] == 3u);
        } else if (anchor_ids[i] == UINT64_C(0x0000000000000001)) {
            assert(hop_counts[i] == 4u);
        } else {
            assert(anchor_ids[i] == UINT64_C(0x0000000000000002));
            assert(hop_counts[i] == 5u);
        }
    }

    while (lower_hash_id == prior_0 || lower_hash_id == prior_1 ||
           discovery_assignment_hash(lower_hash_id) >=
               discovery_assignment_hash(prior_1)) {
        lower_hash_id++;
    }
    anchor_ids[2] = lower_hash_id;
    anchor_ids[3] = UINT64_C(0x0000000000000001);
    if (anchor_ids[3] == lower_hash_id) {
        anchor_ids[3]++;
    }
    anchor_ids[4] = UINT64_C(0x0000000000000002);
    while (anchor_ids[4] == lower_hash_id ||
           anchor_ids[4] == anchor_ids[3]) {
        anchor_ids[4]++;
    }
    assert(discovery_assignment_order_roster_extension(
               anchor_ids,
               NULL,
               sizeof(anchor_ids) / sizeof(anchor_ids[0]),
               2u) == PROTO_OK);
    assert(discovery_assignment_hash(anchor_ids[2]) <
           discovery_assignment_hash(anchor_ids[1]));
    assert(discovery_assignment_append_table_from_anchor_ids(
               payload,
               sizeof(payload),
               &payload_len,
               anchor_ids,
               sizeof(anchor_ids) / sizeof(anchor_ids[0])) == PROTO_OK);
    assert(discovery_assignment_parse_table_tlvs(
               payload,
               payload_len,
               decoded,
               sizeof(decoded) / sizeof(decoded[0]),
               &decoded_count,
               &slot_count) == PROTO_OK);
    assert(decoded_count == sizeof(anchor_ids) / sizeof(anchor_ids[0]));
    assert(slot_count == UWB_DISCOVERY_SLOT_COUNT);
    for (size_t i = 0u; i < decoded_count; i++) {
        assert(decoded[i].anchor_id == anchor_ids[i]);
        assert(decoded[i].slot == i);
    }

    anchor_ids[4] = prior_0;
    assert(discovery_assignment_order_roster_extension(
               anchor_ids,
               NULL,
               sizeof(anchor_ids) / sizeof(anchor_ids[0]),
               2u) == PROTO_ERR_MALFORMED);
    assert(discovery_assignment_order_roster_extension(NULL, NULL, 0u, 0u) ==
           PROTO_OK);
    assert(discovery_assignment_order_roster_extension(
               anchor_ids, NULL, 2u, 3u) ==
           PROTO_ERR_ARG);
}

static void test_control_and_claim_hash_round_trip(void)
{
    uint8_t payload[64];
    enum discovery_assignment_phase phase = 0;
    uint32_t epoch = 0u;
    uint64_t hash = discovery_assignment_hash(UINT64_C(0x2222222222222301));
    uint64_t decoded_hash = 0u;
    size_t payload_len = 0u;

    assert(discovery_assignment_append_control_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
               77u) == PROTO_OK);
    assert(discovery_assignment_append_claim_hash(payload,
                                                  sizeof(payload),
                                                  &payload_len,
                                                  hash) == PROTO_OK);
    assert(discovery_assignment_extract_control_tlvs(payload,
                                                     payload_len,
                                                     &phase,
                                                     &epoch) == PROTO_OK);
    assert(phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM);
    assert(epoch == 77u);
    assert(discovery_assignment_extract_claim_hash(payload,
                                                   payload_len,
                                                   &decoded_hash) == PROTO_OK);
    assert(decoded_hash == hash);

    payload_len = 0u;
    assert(discovery_assignment_append_control_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_ACK,
               78u) == PROTO_OK);
    assert(discovery_assignment_extract_control_tlvs(payload,
                                                     payload_len,
                                                     &phase,
                                                     &epoch) == PROTO_OK);
    assert(phase == DISCOVERY_ASSIGNMENT_PHASE_ACK);
    assert(epoch == 78u);
}

static void test_control_rejects_legacy_or_wrong_scheme(void)
{
    uint8_t payload[32];
    enum discovery_assignment_phase phase = 0;
    uint32_t epoch = 0u;
    size_t payload_len = 0u;

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_ASSIGNMENT_PHASE,
                         DISCOVERY_ASSIGNMENT_PHASE_CLAIM) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                          77u) == PROTO_OK);
    assert(discovery_assignment_extract_control_tlvs(
               payload, payload_len, &phase, &epoch) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u8(
               payload,
               sizeof(payload),
               &payload_len,
               TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
               DISCOVERY_ASSIGNMENT_SCHEME_VERSION + 1u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_ASSIGNMENT_PHASE,
                         DISCOVERY_ASSIGNMENT_PHASE_CLAIM) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                          77u) == PROTO_OK);
    assert(discovery_assignment_extract_control_tlvs(
               payload, payload_len, &phase, &epoch) ==
           PROTO_ERR_MALFORMED);
}

static void test_ordered_epoch_helpers_wrap_and_reject_ambiguity(void)
{
    uint32_t resolved = UINT32_MAX;
    bool repair = true;

    assert(discovery_assignment_epoch_strictly_newer(1u, UINT32_MAX));
    assert(!discovery_assignment_epoch_strictly_newer(UINT32_MAX, 1u));
    assert(!discovery_assignment_epoch_strictly_newer(1u, 1u));
    assert(!discovery_assignment_epoch_strictly_newer(0u, 1u));
    assert(!discovery_assignment_epoch_strictly_newer(1u, 0u));
    assert(!discovery_assignment_epoch_strictly_newer(
        UINT32_C(0x80000001), 1u));
    assert(discovery_assignment_next_epoch(0u) == 1u);
    assert(discovery_assignment_next_epoch(UINT32_MAX) == 1u);
    assert(discovery_assignment_next_epoch(77u) == 78u);

    assert(discovery_assignment_reconcile_epoch_baseline(
               0u, 0u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 0u);
    assert(!repair);

    assert(discovery_assignment_reconcile_epoch_baseline(
               77u, 0u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 77u);
    assert(!repair);
    assert(discovery_assignment_reconcile_epoch_baseline(
               0u, 77u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 77u);
    assert(repair);

    assert(discovery_assignment_reconcile_epoch_baseline(
               77u, 77u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 77u);
    assert(!repair);
    assert(discovery_assignment_reconcile_epoch_baseline(
               77u, 78u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 78u);
    assert(repair);
    assert(discovery_assignment_reconcile_epoch_baseline(
               78u, 77u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 78u);
    assert(!repair);

    assert(discovery_assignment_reconcile_epoch_baseline(
               UINT32_MAX, 1u, &resolved, &repair) == PROTO_OK);
    assert(resolved == 1u);
    assert(repair);
    assert(discovery_assignment_reconcile_epoch_baseline(
               1u, UINT32_MAX, &resolved, &repair) == PROTO_OK);
    assert(resolved == 1u);
    assert(!repair);

    resolved = UINT32_MAX;
    repair = true;
    assert(discovery_assignment_reconcile_epoch_baseline(
               1u, UINT32_C(0x80000001), &resolved, &repair) ==
           PROTO_ERR_STALE);
    assert(resolved == 0u);
    assert(!repair);
    assert(discovery_assignment_reconcile_epoch_baseline(
               1u, 2u, NULL, &repair) == PROTO_ERR_ARG);
    assert(discovery_assignment_reconcile_epoch_baseline(
               1u, 2u, &resolved, NULL) == PROTO_ERR_ARG);
}

static void test_response_custody_matches_logical_epoch_and_phase(void)
{
    assert(discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM));
    assert(discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK));
}

static void test_response_custody_allows_only_valid_supersession_boundaries(void)
{
    assert(!discovery_assignment_response_custody_matches(
        false,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM));
    assert(!discovery_assignment_response_custody_matches(
        true,
        0u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        0u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        78u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        (enum discovery_assignment_phase)0,
        77u,
        (enum discovery_assignment_phase)0));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_TABLE,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_TABLE));
}

static uint32_t response_delay_ms(uint8_t slot,
                                  uint8_t slot_count,
                                  uint8_t hop_count,
                                  uint16_t response_spread_ms,
                                  uint32_t random_value)
{
    uint32_t delay_ms = 0u;

    assert(discovery_assignment_response_delay_ms(
               slot,
               slot_count,
               hop_count,
               response_spread_ms,
               0u,
               random_value,
               &delay_ms) == PROTO_OK);
    return delay_ms;
}

static uint32_t expected_hop_band_start_ms(uint8_t hop_count,
                                           uint8_t slot_count,
                                           uint16_t response_spread_ms)
{
    uint32_t start_ms = DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS;
    uint32_t hop_band_ms = DISCOVERY_ASSIGNMENT_RESPONSE_HOP_BAND_MS(
        response_spread_ms, slot_count);

    for (uint8_t nearer_hop = 1u; nearer_hop < hop_count; nearer_hop++) {
        start_ms += hop_band_ms;
        start_ms += discovery_assignment_response_custody_ms(nearer_hop);
    }
    return start_ms;
}

static void test_response_delay_serializes_near_hops_and_bounded_backoff(void)
{
    static const uint16_t spreads[] = {
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS,
    };
    uint32_t retry_delay = 0u;
    uint32_t first_delay = response_delay_ms(
        3u, 50u, 1u, DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 0u);

    for (size_t spread_index = 0u;
         spread_index < sizeof(spreads) / sizeof(spreads[0]);
         spread_index++) {
        uint16_t spread_ms = spreads[spread_index];
        uint32_t slot_width_ms = DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_WIDTH_MS(
            spread_ms, UWB_DISCOVERY_SLOT_COUNT);

        for (uint8_t hop = 1u; hop <= DISCOVERY_ASSIGNMENT_MAX_HOPS; hop++) {
            uint32_t earliest_ms = response_delay_ms(
                0u, UWB_DISCOVERY_SLOT_COUNT, hop, spread_ms, 0u);

            assert(earliest_ms == expected_hop_band_start_ms(
                                      hop,
                                      UWB_DISCOVERY_SLOT_COUNT,
                                      spread_ms));
            if (hop < DISCOVERY_ASSIGNMENT_MAX_HOPS) {
                uint32_t latest_ms = response_delay_ms(
                    UWB_DISCOVERY_SLOT_COUNT - 1u,
                    UWB_DISCOVERY_SLOT_COUNT,
                    hop,
                    spread_ms,
                    slot_width_ms - 1u);
                uint32_t next_earliest_ms = response_delay_ms(
                    0u,
                    UWB_DISCOVERY_SLOT_COUNT,
                    hop + 1u,
                    spread_ms,
                    0u);
                uint64_t latest_deadline_ms =
                    discovery_assignment_response_deadline_ms(
                        0u, latest_ms, hop);

                assert(earliest_ms < next_earliest_ms);
                assert(latest_deadline_ms < next_earliest_ms);
            }
        }
    }

    assert(discovery_assignment_response_delay_ms(
               3u, 50u, 8u, 1000u, 2u, UINT32_MAX,
                                                  &retry_delay) == PROTO_OK);
    assert(retry_delay > first_delay);

    /* Reproduce the three-board HIL topology exactly: hop-one slot 0,
     * hop-two slot 10, and hop-three slot 29. Even the latest jittered send
     * plus full route custody must finish before the next board may send. */
    {
        static const uint8_t bench_slots[] = {0u, 10u, 29u};
        uint32_t earliest_ms[3];
        uint64_t latest_deadline_ms[3];
        uint32_t slot_width_ms = DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_WIDTH_MS(
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 30u);

        for (size_t index = 0u; index < 3u; index++) {
            uint8_t hop = (uint8_t)index + 1u;
            uint32_t latest_ms;

            earliest_ms[index] = response_delay_ms(
                bench_slots[index],
                30u,
                hop,
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
                0u);
            latest_ms = response_delay_ms(
                bench_slots[index],
                30u,
                hop,
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
                slot_width_ms - 1u);
            latest_deadline_ms[index] =
                discovery_assignment_response_deadline_ms(
                    0u, latest_ms, hop);
        }
        assert(earliest_ms[0] < earliest_ms[1]);
        assert(earliest_ms[1] < earliest_ms[2]);
        assert(latest_deadline_ms[0] < earliest_ms[1]);
        assert(latest_deadline_ms[1] < earliest_ms[2]);
    }

    assert(discovery_assignment_response_delay_ms(
               50u, 50u, 1u, 1000u, 0u, 0u,
               &retry_delay) == PROTO_ERR_ARG);
    assert(discovery_assignment_response_delay_ms(
               0u, 0u, 1u, 1000u, 0u, 0u,
               &retry_delay) == PROTO_ERR_ARG);
    assert(discovery_assignment_response_delay_ms(
               0u, 50u, 1u, 0u, 0u, 0u,
               &retry_delay) == PROTO_ERR_ARG);

    assert(discovery_assignment_retry_backoff_ms(0u, 0u) ==
           DISCOVERY_ASSIGNMENT_RETRY_BASE_MS);
    assert(discovery_assignment_retry_backoff_ms(UINT8_MAX, UINT32_MAX) >=
           DISCOVERY_ASSIGNMENT_RETRY_MAX_MS);
    assert(discovery_assignment_retry_backoff_ms(UINT8_MAX, UINT32_MAX) <
           2u * DISCOVERY_ASSIGNMENT_RETRY_MAX_MS);
}

static void test_collection_window_covers_spread_and_hops(void)
{
    uint32_t direct = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 1u);
    uint32_t forced_hop = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 4u);
    uint32_t unknown = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 0u);
    uint32_t delay_ms = 0u;

    assert(direct >= DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS);
    assert(forced_hop > direct);
    assert(unknown > forced_hop);
    assert(discovery_assignment_response_custody_ms(1u) == 30000u);
    assert(discovery_assignment_response_custody_ms(4u) == 60000u);
    assert(discovery_assignment_response_custody_ms(0u) ==
           DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS);
    assert(discovery_assignment_response_delay_ms(
               49u, 50u, 1u,
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 0u,
               (DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS / 50u) - 1u,
               &delay_ms) == PROTO_OK);
    assert(delay_ms == 1099u);
    assert(discovery_assignment_response_deadline_ms(1000u, delay_ms, 1u) ==
           32099u);
    assert(discovery_assignment_response_delay_ms(
               49u, 50u, 8u,
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 0u,
               (DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS / 50u) - 1u,
               &delay_ms) == PROTO_OK);
    assert(delay_ms == 428099u);
    assert(discovery_assignment_response_deadline_ms(1000u, delay_ms, 8u) ==
           529099u);
    assert(discovery_assignment_response_deadline_ms(
               UINT64_MAX - 100u, delay_ms, 8u) == UINT64_MAX);
    assert(discovery_assignment_membership_epoch(UINT32_C(0x12345678)) ==
           UINT16_C(0x444c));
    assert(discovery_assignment_membership_epoch(UINT32_C(0x00010001)) == 1u);
    assert(direct == 31099u);
    assert(forced_hop == 184099u);
    assert(unknown ==
           DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS +
               DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_FOR_SPREAD_MS(
                   DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS));
    assert(unknown == 528099u);
    assert(direct >= 31000u);
    assert(forced_hop >= 61000u);
    assert(unknown >= 101000u);
    assert(discovery_assignment_collection_window_ms(0u, 1u) == 0u);
    assert(DISCOVERY_ASSIGNMENT_RESPONSE_MAX_ROUTE_WINDOW_MS == 600099u);
    assert(DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS == 10000u);
    assert(DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS == 3000u);
    assert(DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS == 5u);
    assert(DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT == 2u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS ==
           DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *
               DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS ==
           10u);
    assert(DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES == 2u);
    assert(DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS == 598u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS == 1576004u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS == 1591204u);
    assert(DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS == 1u);
    assert(DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS == 1u);
    assert(discovery_assignment_control_flood_deadline_ms(1000u, 50000u) ==
           11000u);
    assert(discovery_assignment_control_flood_deadline_ms(1000u, 5000u) ==
           5000u);
    assert(discovery_assignment_control_flood_deadline_ms(5000u, 5000u) ==
           5000u);
    assert(discovery_assignment_control_flood_deadline_ms(
               UINT64_MAX - 5000u, UINT64_MAX) == UINT64_MAX);
    {
        uint64_t first_deadline =
            discovery_assignment_response_ack_settle_deadline_ms(1000u);
        uint64_t duplicate_deadline =
            discovery_assignment_response_ack_settle_deadline_ms(3000u);

        assert(first_deadline == 4000u);
        assert(discovery_assignment_response_ack_settle_pending(
                   3999u, first_deadline));
        assert(!discovery_assignment_response_ack_settle_pending(
                   4000u, first_deadline));
        assert(duplicate_deadline == 6000u);
        assert(discovery_assignment_response_ack_settle_pending(
                   4000u, duplicate_deadline));
        assert(!discovery_assignment_response_ack_settle_pending(
                   6000u, duplicate_deadline));
    }
}

static void test_table_window_covers_every_fast_ack_handle(void)
{
    const uint32_t natural_window = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 2u);
    const uint32_t table_window =
        discovery_assignment_table_collection_window_ms(
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS, 2u);
    const uint32_t custody = discovery_assignment_response_custody_ms(2u);

    /*
     * TABLE ACK ownership differs from CLAIM ownership: an ACK_PENDING anchor
     * opens a fresh bounded node-communication handle after each of the fast
     * terminal retries.  The gateway must retain the exact operation through
     * that whole bounded phase or an on-time autonomous retry is retired as
     * inactive even though the sender still owns it.
     */
    assert(DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES == 3u);
    assert(DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS == 1397u);
    assert(table_window ==
           natural_window +
               DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES * custody +
               DISCOVERY_ASSIGNMENT_ACK_FAST_RETRY_BACKOFF_MAX_MS);
    assert(natural_window == 72099u);
    assert(table_window == 193496u);
    assert(discovery_assignment_table_collection_window_ms(
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
               DISCOVERY_ASSIGNMENT_MAX_HOPS) == 829496u);

    assert(DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS == 1576004u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS == 1591204u);
}

static void test_claim_window_covers_two_fresh_handles(void)
{
    const uint32_t natural_window = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        DISCOVERY_ASSIGNMENT_MAX_HOPS);
    const uint32_t custody = discovery_assignment_response_custody_ms(
        DISCOVERY_ASSIGNMENT_MAX_HOPS);
    const uint32_t bounded_claim_window =
        natural_window +
        DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES * custody +
        DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS;

    assert(DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES == 2u);
    assert(DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS ==
           (DISCOVERY_ASSIGNMENT_RETRY_BASE_MS * 2u - 1u) +
               (DISCOVERY_ASSIGNMENT_RETRY_BASE_MS * 4u - 1u));
    assert(natural_window == 528099u);
    assert(bounded_claim_window == 728697u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS) ==
           DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) == 1735204u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(
               DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS) <= 1800000u);
}

static void test_claim_ack_settle_scales_and_duplicate_restarts_deadline(void)
{
    uint32_t direct_ms =
        discovery_assignment_claim_ack_settle_duration_ms(1u);
    uint32_t two_hop_ms =
        discovery_assignment_claim_ack_settle_duration_ms(2u);
    uint32_t max_hop_ms =
        discovery_assignment_claim_ack_settle_duration_ms(
            DISCOVERY_ASSIGNMENT_MAX_HOPS);
    uint64_t first_deadline;
    uint64_t duplicate_deadline;

    assert(direct_ms == DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS);
    assert(two_hop_ms ==
           direct_ms +
               DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS);
    assert(max_hop_ms == direct_ms +
           ((DISCOVERY_ASSIGNMENT_MAX_HOPS - 1u) *
            DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_PER_ADDITIONAL_HOP_MS));
    assert(max_hop_ms == DISCOVERY_ASSIGNMENT_CLAIM_ACK_SETTLE_MAX_MS);
    assert(discovery_assignment_claim_ack_settle_duration_ms(0u) ==
           max_hop_ms);
    assert(discovery_assignment_claim_ack_settle_duration_ms(UINT8_MAX) ==
           max_hop_ms);

    first_deadline = discovery_assignment_claim_ack_settle_deadline_ms(
        1000u, 2u);
    duplicate_deadline = discovery_assignment_claim_ack_settle_deadline_ms(
        2000u, 2u);
    assert(first_deadline == 1000u + two_hop_ms);
    assert(duplicate_deadline == 2000u + two_hop_ms);
    assert(duplicate_deadline > first_deadline);
    assert(discovery_assignment_response_ack_settle_pending(
        first_deadline - 1u, first_deadline));
    assert(!discovery_assignment_response_ack_settle_pending(
        first_deadline, first_deadline));
    assert(discovery_assignment_response_ack_settle_pending(
        first_deadline, duplicate_deadline));
    assert(!discovery_assignment_response_ack_settle_pending(
        duplicate_deadline, duplicate_deadline));
    assert(discovery_assignment_claim_ack_settle_deadline_ms(
               UINT64_MAX - 100u, DISCOVERY_ASSIGNMENT_MAX_HOPS) ==
           UINT64_MAX);
}

static void test_ack_quorum_settle_deadline_freezes_under_duplicate_retries(void)
{
    uint64_t settle_deadline_ms = 0u;
    bool settle_armed = false;

    assert(!discovery_assignment_ack_quorum_settle_should_arm(false, 1u));
    for (uint64_t received_at_ms = 1000u;
         received_at_ms < 10000u;
         received_at_ms += 100u) {
        if (discovery_assignment_ack_quorum_settle_should_arm(
                settle_armed,
                0u)) {
            settle_deadline_ms =
                discovery_assignment_response_ack_settle_deadline_ms(
                    received_at_ms);
            settle_armed = true;
        }
        assert(settle_armed);
        assert(settle_deadline_ms ==
               1000u + DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS);
    }
}

static void test_expected_count_is_optional_and_bounded(void)
{
    uint8_t payload[8] = {0};
    size_t payload_len = 0u;
    uint16_t expected_count = UINT16_MAX;
    bool present = true;

    assert(discovery_assignment_extract_expected_count(
               payload, payload_len, &expected_count, &present) == PROTO_OK);
    assert(!present);
    assert(expected_count == 0u);

    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_EXPECTED_NODE_COUNT, 50u) == PROTO_OK);
    assert(discovery_assignment_extract_expected_count(
               payload, payload_len, &expected_count, &present) == PROTO_OK);
    assert(present);
    assert(expected_count == 50u);

    proto_put_u16_le(&payload[payload_len - sizeof(uint16_t)], 0u);
    assert(discovery_assignment_extract_expected_count(
               payload, payload_len, &expected_count, &present) ==
           PROTO_ERR_MALFORMED);
    proto_put_u16_le(&payload[payload_len - sizeof(uint16_t)], 51u);
    assert(discovery_assignment_extract_expected_count(
               payload, payload_len, &expected_count, &present) ==
           PROTO_ERR_MALFORMED);
    payload[payload_len - 3u] = 1u;
    assert(discovery_assignment_extract_expected_count(
               payload, payload_len - 1u, &expected_count, &present) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_EXPECTED_NODE_COUNT, 2u) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_EXPECTED_NODE_COUNT, 3u) == PROTO_OK);
    assert(discovery_assignment_extract_expected_count(
               payload, payload_len, &expected_count, &present) ==
           PROTO_ERR_MALFORMED);
}

static void build_assignment_result(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    enum discovery_assignment_phase phase)
{
    const uint64_t anchor_id = UINT64_C(0x2222222222222301);

    *payload_len = 0u;
    assert(tlv_append_u16(payload, payload_cap, payload_len,
                          TLV_COMMAND_ID,
                          CMD_ASSIGN_DISCOVERY_SLOTS) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, payload_len,
                          TLV_COMMAND_STATUS, COMMAND_OK) == PROTO_OK);
    assert(tlv_append_u8(payload, payload_cap, payload_len,
                         TLV_REASON, 0u) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(
               payload, payload_cap, payload_len, phase, 77u) == PROTO_OK);
    assert(discovery_assignment_append_claim_hash(
               payload,
               payload_cap,
               payload_len,
               discovery_assignment_hash(anchor_id)) == PROTO_OK);
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        struct discovery_assignment_table_commitment commitment =
            test_table_commitment(UINT32_C(0x12345678));

        assert(discovery_assignment_append_table_commitment(
                   payload,
                   payload_cap,
                   payload_len,
                   &commitment) == PROTO_OK);
    }
    assert(tlv_append_u8(payload, payload_cap, payload_len,
                         TLV_HOP_COUNT, 2u) == PROTO_OK);
}

static void append_conflicting_assignment_singleton(uint8_t *payload,
                                                    size_t payload_cap,
                                                    size_t *payload_len,
                                                    uint8_t type)
{
    switch (type) {
    case TLV_COMMAND_ID:
        assert(tlv_append_u16(payload, payload_cap, payload_len, type,
                              CMD_SURVEY_ABORT) == PROTO_OK);
        break;
    case TLV_COMMAND_STATUS:
        assert(tlv_append_u16(payload, payload_cap, payload_len, type,
                              COMMAND_TIMEOUT) == PROTO_OK);
        break;
    case TLV_REASON:
        assert(tlv_append_u8(payload, payload_cap, payload_len, type,
                             1u) == PROTO_OK);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION:
        assert(tlv_append_u8(
                   payload, payload_cap, payload_len, type,
                   DISCOVERY_ASSIGNMENT_SCHEME_VERSION + 1u) == PROTO_OK);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_PHASE:
        assert(tlv_append_u8(payload, payload_cap, payload_len, type,
                             DISCOVERY_ASSIGNMENT_PHASE_CLAIM) == PROTO_OK);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_EPOCH:
        assert(tlv_append_u32(payload, payload_cap, payload_len, type,
                              78u) == PROTO_OK);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_HASH:
        assert(tlv_append_u64(payload, payload_cap, payload_len, type,
                              UINT64_C(0x0102030405060708)) == PROTO_OK);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT:
    {
        struct discovery_assignment_table_commitment commitment =
            test_table_commitment(UINT32_C(0x87654321));

        assert(discovery_assignment_append_table_commitment(
                   payload,
                   payload_cap,
                   payload_len,
                   &commitment) == PROTO_OK);
        break;
    }
    case TLV_HOP_COUNT:
        assert(tlv_append_u8(payload, payload_cap, payload_len, type,
                             3u) == PROTO_OK);
        break;
    default:
        assert(false);
    }
}

static void test_result_parser_rejects_duplicate_authoritative_singletons(void)
{
    static const uint8_t singleton_types[] = {
        TLV_COMMAND_ID,
        TLV_COMMAND_STATUS,
        TLV_REASON,
        TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
        TLV_DISCOVERY_ASSIGNMENT_PHASE,
        TLV_DISCOVERY_ASSIGNMENT_EPOCH,
        TLV_DISCOVERY_ASSIGNMENT_HASH,
        TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT,
        TLV_HOP_COUNT,
    };
    struct discovery_assignment_result parsed = {0};
    struct discovery_assignment_table_commitment expected_commitment =
        test_table_commitment(UINT32_C(0x12345678));
    const struct discovery_assignment_table_commitment zero_commitment = {0};
    uint8_t payload[192];
    size_t payload_len = 0u;

    build_assignment_result(payload,
                            sizeof(payload),
                            &payload_len,
                            DISCOVERY_ASSIGNMENT_PHASE_ACK);
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_OK);
    assert(parsed.phase == DISCOVERY_ASSIGNMENT_PHASE_ACK);
    assert(parsed.epoch == 77u);
    assert(parsed.hash ==
           discovery_assignment_hash(UINT64_C(0x2222222222222301)));
    assert(discovery_assignment_table_commitment_equal(
        &parsed.table_commitment, &expected_commitment));
    assert(parsed.hop_count_present);
    assert(parsed.hop_count == 2u);

    for (size_t i = 0u;
         i < sizeof(singleton_types) / sizeof(singleton_types[0]);
         i++) {
        const struct discovery_assignment_result unchanged = {
            .phase = DISCOVERY_ASSIGNMENT_PHASE_TABLE,
            .epoch = UINT32_C(0xa5a5a5a5),
            .hash = UINT64_C(0x1122334455667788),
            .table_commitment = {.bytes = {0x55u, 0x66u, 0x77u, 0x88u}},
            .hop_count = 0x5au,
            .hop_count_present = true,
        };

        build_assignment_result(payload,
                                sizeof(payload),
                                &payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_ACK);
        append_conflicting_assignment_singleton(
            payload,
            sizeof(payload),
            &payload_len,
            singleton_types[i]);
        parsed = unchanged;
        assert(discovery_assignment_parse_result_tlvs(
                   payload, payload_len, &parsed) ==
               PROTO_ERR_MALFORMED);
        assert(memcmp(&parsed, &unchanged, sizeof(parsed)) == 0);
    }

    build_assignment_result(payload,
                            sizeof(payload),
                            &payload_len,
                            DISCOVERY_ASSIGNMENT_PHASE_CLAIM);
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_OK);
    assert(parsed.phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM);
    assert(memcmp(&parsed.table_commitment,
                  &zero_commitment,
                  sizeof(zero_commitment)) == 0);

    payload[payload_len++] = 0xa5u;
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_ERR_MALFORMED);
}

static void test_result_parser_rejects_ambiguous_batch_metadata(void)
{
    struct discovery_assignment_result parsed;
    uint8_t payload[192];
    size_t payload_len = 0u;

    build_assignment_result(payload,
                            sizeof(payload),
                            &payload_len,
                            DISCOVERY_ASSIGNMENT_PHASE_ACK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 9u) == PROTO_OK);
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_ERR_MALFORMED);

    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS, 0u) == PROTO_OK);
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_MESH_CH9_BATCH_ID, 10u) == PROTO_OK);
    assert(discovery_assignment_parse_result_tlvs(
               payload, payload_len, &parsed) == PROTO_ERR_MALFORMED);
}

static void test_full_table_fits_one_extended_packet_and_round_trips(void)
{
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    struct discovery_assignment_entry decoded[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    size_t decoded_count = 0u;
    uint8_t slot_count = 0u;

    build_sorted_entries(entries, UWB_DISCOVERY_SLOT_COUNT);
    assert(discovery_assignment_append_control_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_TABLE,
               1234u) == PROTO_OK);
    assert(discovery_assignment_append_table_tlvs(payload,
                                                  sizeof(payload),
                                                  &payload_len,
                                                  entries,
                                                  UWB_DISCOVERY_SLOT_COUNT) ==
           PROTO_OK);
    assert(payload_len <= PACKET_EXT_MAX_PAYLOAD_LEN);
    assert(discovery_assignment_parse_table_tlvs(payload,
                                                 payload_len,
                                                 decoded,
                                                 UWB_DISCOVERY_SLOT_COUNT,
                                                 &decoded_count,
                                                 &slot_count) == PROTO_OK);
    assert(decoded_count == UWB_DISCOVERY_SLOT_COUNT);
    assert(slot_count == UWB_DISCOVERY_SLOT_COUNT);
    for (size_t i = 0u; i < decoded_count; i++) {
        assert(decoded[i].anchor_id == entries[i].anchor_id);
        assert(decoded[i].hash == entries[i].hash);
        assert(decoded[i].slot == entries[i].slot);
    }
}

static void test_sparse_explicit_slots_round_trip_without_compaction(void)
{
    struct discovery_assignment_entry entries[4];
    struct discovery_assignment_entry decoded[4];
    uint64_t ids[4] = {
        UINT64_C(0x5000000000000001),
        UINT64_C(0x5000000000000002),
        UINT64_C(0x5000000000000003),
        UINT64_C(0x5000000000000004),
    };
    const uint8_t slots[4] = {0u, 1u, 2u, 4u};
    uint8_t payload[256];
    struct discovery_assignment_table_commitment commitment;
    size_t payload_len = 0u;
    size_t decoded_count = 0u;
    uint8_t slot_count = 0u;

    for (size_t i = 0u; i < 4u; i++) {
        entries[i] = (struct discovery_assignment_entry) {
            .anchor_id = ids[i],
            .hash = discovery_assignment_hash(ids[i]),
            .slot = slots[i],
        };
    }
    assert(discovery_assignment_append_table_tlvs(
               payload, sizeof(payload), &payload_len, entries, 4u) ==
           PROTO_OK);
    assert(discovery_assignment_parse_table_tlvs(
               payload, payload_len, decoded, 4u, &decoded_count,
               &slot_count) == PROTO_OK);
    assert(decoded_count == 4u);
    assert(slot_count == UWB_DISCOVERY_SLOT_COUNT);
    assert(discovery_assignment_table_commitment(
        decoded, decoded_count, slot_count, &commitment));
    for (size_t i = 0u; i < decoded_count; i++) {
        assert(decoded[i].anchor_id == ids[i]);
        assert(decoded[i].slot == slots[i]);
    }

    entries[3].slot = entries[2].slot;
    payload_len = 0u;
    assert(discovery_assignment_append_table_tlvs(
               payload, sizeof(payload), &payload_len, entries, 4u) ==
           PROTO_ERR_MALFORMED);
}

static void test_table_rejects_missing_and_corrupt_entries(void)
{
    struct discovery_assignment_entry entries[2];
    struct discovery_assignment_entry decoded[2];
    uint8_t payload[128];
    size_t payload_len = 0u;
    size_t decoded_count = 0u;
    uint8_t slot_count = 0u;
    size_t table_value_offset = 0u;

    build_sorted_entries(entries, 2u);
    assert(discovery_assignment_append_table_tlvs(payload,
                                                  sizeof(payload),
                                                  &payload_len,
                                                  entries,
                                                  2u) == PROTO_OK);
    assert(discovery_assignment_parse_table_tlvs(payload,
                                                 payload_len -
                                                     DISCOVERY_ASSIGNMENT_ENTRY_WIRE_LEN,
                                                 decoded,
                                                 2u,
                                                 &decoded_count,
                                                 &slot_count) ==
           PROTO_ERR_MALFORMED);

    while (table_value_offset + 2u <= payload_len &&
           payload[table_value_offset] != TLV_DISCOVERY_ASSIGNMENT_TABLE) {
        table_value_offset += (size_t)payload[table_value_offset + 1u] + 2u;
    }
    assert(table_value_offset + 2u < payload_len);
    payload[table_value_offset + 2u + 8u] ^= 0x01u;
    assert(discovery_assignment_parse_table_tlvs(payload,
                                                 payload_len,
                                                 decoded,
                                                 2u,
                                                 &decoded_count,
                                                 &slot_count) ==
           PROTO_ERR_MALFORMED);

    build_sorted_entries(entries, 2u);
    payload_len = 0u;
    assert(discovery_assignment_append_table_tlvs(payload,
                                                  sizeof(payload),
                                                  &payload_len,
                                                  entries,
                                                  2u) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT,
                         UWB_DISCOVERY_SLOT_COUNT - 1u) == PROTO_OK);
    assert(discovery_assignment_parse_table_tlvs(payload,
                                                 payload_len,
                                                 decoded,
                                                 2u,
                                                 &decoded_count,
                                                 &slot_count) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(discovery_assignment_append_table_tlvs(payload,
                                                  sizeof(payload),
                                                  &payload_len,
                                                  entries,
                                                  2u) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_EXPECTED_NODE_COUNT, 1u) == PROTO_OK);
    assert(discovery_assignment_parse_table_tlvs(payload,
                                                 payload_len,
                                                 decoded,
                                                 2u,
                                                 &decoded_count,
                                                 &slot_count) ==
           PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_table_commitment_known_vector_and_legacy_collision();
    test_hash_order_is_deterministic_and_tied_by_id();
    test_compact_anchor_id_path_matches_entry_wire_format();
    test_roster_extension_preserves_prior_slots();
    test_control_and_claim_hash_round_trip();
    test_control_rejects_legacy_or_wrong_scheme();
    test_ordered_epoch_helpers_wrap_and_reject_ambiguity();
    test_response_custody_matches_logical_epoch_and_phase();
    test_response_custody_allows_only_valid_supersession_boundaries();
    test_response_delay_serializes_near_hops_and_bounded_backoff();
    test_collection_window_covers_spread_and_hops();
    test_table_window_covers_every_fast_ack_handle();
    test_claim_window_covers_two_fresh_handles();
    test_claim_ack_settle_scales_and_duplicate_restarts_deadline();
    test_ack_quorum_settle_deadline_freezes_under_duplicate_retries();
    test_expected_count_is_optional_and_bounded();
    test_result_parser_rejects_duplicate_authoritative_singletons();
    test_result_parser_rejects_ambiguous_batch_metadata();
    test_full_table_fits_one_extended_packet_and_round_trips();
    test_sparse_explicit_slots_round_trip_without_compaction();
    test_table_rejects_missing_and_corrupt_entries();
    return 0;
}
