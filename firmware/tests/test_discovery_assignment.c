#include "discovery_assignment.h"

#include <assert.h>
#include <string.h>

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

static void test_response_custody_matches_logical_epoch_and_phase(void)
{
    assert(discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        999u));
    assert(discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        102u,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        102u));
}

static void test_response_custody_allows_only_valid_supersession_boundaries(void)
{
    assert(!discovery_assignment_response_custody_matches(
        false,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u));
    assert(!discovery_assignment_response_custody_matches(
        true,
        0u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u,
        0u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u,
        78u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        101u,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        101u));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        (enum discovery_assignment_phase)0,
        101u,
        77u,
        (enum discovery_assignment_phase)0,
        101u));
    assert(!discovery_assignment_response_custody_matches(
        true,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        102u,
        77u,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        103u));
}

static void test_response_delay_uses_slot_hops_and_bounded_backoff(void)
{
    uint32_t far_delay = 0u;
    uint32_t near_delay = 0u;
    uint32_t later_slot_delay = 0u;
    uint32_t retry_delay = 0u;
    uint32_t max_initial_delay = 0u;

    assert(discovery_assignment_response_delay_ms(3u, 50u, 8u, 0u, 0u,
                                                  &far_delay) == PROTO_OK);
    assert(discovery_assignment_response_delay_ms(3u, 50u, 1u, 0u, 0u,
                                                  &near_delay) == PROTO_OK);
    assert(discovery_assignment_response_delay_ms(4u, 50u, 8u, 0u, 0u,
                                                  &later_slot_delay) == PROTO_OK);
    assert(discovery_assignment_response_delay_ms(3u, 50u, 8u, 2u, UINT32_MAX,
                                                  &retry_delay) == PROTO_OK);
    assert(discovery_assignment_response_delay_ms(
               49u, 50u, 1u, 0u,
               DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS,
               &max_initial_delay) == PROTO_OK);
    assert(far_delay < near_delay);
    assert(later_slot_delay - far_delay == DISCOVERY_ASSIGNMENT_RESPONSE_SLOT_MS);
    assert(retry_delay > far_delay);
    assert(max_initial_delay ==
           DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS);
    assert(discovery_assignment_response_delay_ms(50u, 50u, 1u, 0u, 0u,
                                                  &far_delay) == PROTO_ERR_ARG);

    assert(discovery_assignment_retry_backoff_ms(0u, 0u) ==
           DISCOVERY_ASSIGNMENT_RETRY_BASE_MS);
    assert(discovery_assignment_retry_backoff_ms(UINT8_MAX, UINT32_MAX) >=
           DISCOVERY_ASSIGNMENT_RETRY_MAX_MS);
    assert(discovery_assignment_retry_backoff_ms(UINT8_MAX, UINT32_MAX) <
           2u * DISCOVERY_ASSIGNMENT_RETRY_MAX_MS);
}

static void test_collection_window_covers_slots_and_hops(void)
{
    uint32_t direct = discovery_assignment_collection_window_ms(50u, 1u);
    uint32_t forced_hop = discovery_assignment_collection_window_ms(50u, 4u);
    uint32_t unknown = discovery_assignment_collection_window_ms(50u, 0u);
    uint32_t delay_ms = 0u;

    assert(direct >= DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS);
    assert(forced_hop > direct);
    assert(unknown > forced_hop);
    assert(discovery_assignment_response_custody_ms(1u) == 30000u);
    assert(discovery_assignment_response_custody_ms(4u) == 60000u);
    assert(discovery_assignment_response_custody_ms(0u) ==
           DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS);
    assert(discovery_assignment_response_delay_ms(
               49u, 50u, 1u, 0u,
               DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS,
               &delay_ms) == PROTO_OK);
    assert(delay_ms == 1799u);
    assert(discovery_assignment_response_deadline_ms(1000u, delay_ms, 1u) ==
           32799u);
    assert(discovery_assignment_response_delay_ms(
               49u, 50u, 8u, 0u,
               DISCOVERY_ASSIGNMENT_RESPONSE_INITIAL_JITTER_MAX_MS,
               &delay_ms) == PROTO_OK);
    assert(delay_ms == 1099u);
    assert(discovery_assignment_response_deadline_ms(1000u, delay_ms, 8u) ==
           102099u);
    assert(discovery_assignment_response_deadline_ms(
               UINT64_MAX - 100u, delay_ms, 8u) == UINT64_MAX);
    assert(discovery_assignment_membership_epoch(UINT32_C(0x12345678)) ==
           UINT16_C(0x444c));
    assert(discovery_assignment_membership_epoch(UINT32_C(0x00010001)) == 1u);
    assert(direct == DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS +
                     DISCOVERY_ASSIGNMENT_RESPONSE_MAX_INITIAL_DELAY_MS);
    assert(direct >= 31000u);
    assert(forced_hop >= 61000u);
    assert(unknown >= 101000u);
    assert(discovery_assignment_collection_window_ms(0u, 1u) == 0u);
    assert(DISCOVERY_ASSIGNMENT_RESPONSE_MAX_ROUTE_WINDOW_MS == 101099u);
    assert(DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS == 10000u);
    assert(DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS == 3000u);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS == 225199u);
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

static void test_expected_count_is_optional_and_bounded(void)
{
    uint8_t payload[8];
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
}

int main(void)
{
    test_hash_order_is_deterministic_and_tied_by_id();
    test_compact_anchor_id_path_matches_entry_wire_format();
    test_control_and_claim_hash_round_trip();
    test_response_custody_matches_logical_epoch_and_phase();
    test_response_custody_allows_only_valid_supersession_boundaries();
    test_response_delay_uses_slot_hops_and_bounded_backoff();
    test_collection_window_covers_slots_and_hops();
    test_expected_count_is_optional_and_bounded();
    test_full_table_fits_one_extended_packet_and_round_trips();
    test_table_rejects_missing_and_corrupt_entries();
    return 0;
}
