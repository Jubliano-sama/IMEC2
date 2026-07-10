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
    test_control_and_claim_hash_round_trip();
    test_full_table_fits_one_extended_packet_and_round_trips();
    test_table_rejects_missing_and_corrupt_entries();
    return 0;
}
