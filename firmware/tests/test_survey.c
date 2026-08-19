#include "survey.h"
#include "mesh_relay.h"
#include "operation_policy.h"
#include "survey_round_control.h"

#include <assert.h>
#include <string.h>

static struct survey_sample sample(void)
{
    struct survey_sample value = {
        .pair = {
            .survey_id = 0xAABBCCDDu,
            .initiator_id = 0x1111222233334444ull,
            .responder_id = 0x5555666677778888ull,
            .sample_count = 3u,
        },
        .round_id = 7u,
        .sample_index = 1u,
        .distance_mm = -1234,
        .quality = 88u,
        .range_status = RANGE_OK,
    };
    return value;
}

static void assert_sample_equal(const struct survey_sample *left,
                                const struct survey_sample *right)
{
    assert(left->pair.operation_generation ==
           right->pair.operation_generation);
    assert(left->pair.survey_id == right->pair.survey_id);
    assert(left->pair.initiator_id == right->pair.initiator_id);
    assert(left->pair.responder_id == right->pair.responder_id);
    assert(left->pair.sample_count == right->pair.sample_count);
    assert(left->round_id == right->round_id);
    assert(left->sample_index == right->sample_index);
    assert(left->distance_mm == right->distance_mm);
    assert(left->quality == right->quality);
    assert(left->range_status == right->range_status);
}

static void assert_packet_unchanged(
    const struct proto_packet *packet,
    const struct proto_packet *expected)
{
    assert(memcmp(packet, expected, sizeof(*packet)) == 0);
}

static void test_sample_count_validation(void)
{
    assert(!survey_sample_count_valid(0u));
    assert(survey_sample_count_valid(1u));
    assert(survey_sample_count_valid(1000u));
    assert(!survey_sample_count_valid(1001u));
}

static void test_pair_and_sample_validation(void)
{
    struct survey_sample value = sample();

    assert(survey_sample_validate(&value) == PROTO_OK);

    value.sample_index = value.pair.sample_count;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.quality = 101u;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.pair.responder_id = value.pair.initiator_id;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.pair.operation_generation = UINT64_C(0x0000000100000000);
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.range_status = RANGE_TIMING_INVALID;
    assert(survey_sample_validate(&value) == PROTO_OK);

    value.range_status = RANGE_STS_QUALITY_FAIL;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);
}

static void
test_pair_result_transport_sequence_is_exhaustive_and_reset_stable(void)
{
    static uint16_t first_boot[
        SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX];
    bool seen[SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX + 1u] = {false};
    uint16_t unrelated_gateway_command_sequence =
        SURVEY_PAIR_RESULT_MAX_BATCH_COUNT;
    uint16_t round_cursor = SURVEY_LEGACY_ROUND_ID;
    size_t identity_index = 0u;

    for (uint16_t expected_round_id = 1u;
         expected_round_id <= SURVEY_PAIR_RESULT_MAX_BATCH_COUNT;
         expected_round_id++) {
        uint16_t round_id = UINT16_MAX;

        /*
         * PREPARE and START controls consume unrelated command-sequence
         * values. Advancing that domain beyond 450 must not influence the
         * bounded per-survey result generation.
         */
        for (uint8_t control = 0u; control < 4u; control++) {
            unrelated_gateway_command_sequence++;
            if (unrelated_gateway_command_sequence == 0u) {
                unrelated_gateway_command_sequence = 1u;
            }
        }
        assert(unrelated_gateway_command_sequence >
               SURVEY_PAIR_RESULT_MAX_BATCH_COUNT);
        assert(survey_pair_result_next_round_id(
                   round_cursor, &round_id) == PROTO_OK);
        assert(round_id == expected_round_id);
        round_cursor = round_id;

        for (uint16_t sample_index = 0u;
             sample_index < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
             sample_index++) {
            uint16_t sequence = 0u;
            uint16_t expected =
                (uint16_t)(((uint32_t)(round_id - 1u) *
                            SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) +
                           sample_index + 1u);

            assert(survey_pair_result_transport_sequence(
                       round_id, sample_index, &sequence) == PROTO_OK);
            assert(sequence == expected);
            assert(sequence != 0u);
            assert(sequence <=
                   SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX);
            assert(!seen[sequence]);
            seen[sequence] = true;
            first_boot[identity_index++] = sequence;
        }
    }
    assert(identity_index ==
           SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX);
    for (uint16_t sequence = 1u;
         sequence <= SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX;
         sequence++) {
        assert(seen[sequence]);
    }

    /*
     * The second pass represents a fresh boot: no mutable sequence state is
     * supplied, so every logical round/sample identity must reproduce exactly.
     */
    identity_index = 0u;
    round_cursor = SURVEY_LEGACY_ROUND_ID;
    for (uint16_t expected_round_id = 1u;
         expected_round_id <= SURVEY_PAIR_RESULT_MAX_BATCH_COUNT;
         expected_round_id++) {
        uint16_t round_id = UINT16_MAX;

        assert(survey_pair_result_next_round_id(
                   round_cursor, &round_id) == PROTO_OK);
        assert(round_id == expected_round_id);
        round_cursor = round_id;
        for (uint16_t sample_index = 0u;
             sample_index < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
             sample_index++) {
            uint16_t sequence = UINT16_MAX;

            assert(survey_pair_result_transport_sequence(
                       round_id, sample_index, &sequence) == PROTO_OK);
            assert(sequence == first_boot[identity_index++]);
        }
    }
}

static void test_pair_result_transport_sequence_supports_legacy_controls(void)
{
    for (uint16_t sample_index = 0u;
         sample_index < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
         sample_index++) {
        uint16_t sequence = 0u;

        assert(survey_pair_result_transport_sequence(
                   SURVEY_LEGACY_ROUND_ID,
                   sample_index,
                   &sequence) == PROTO_OK);
        assert(sequence == sample_index + 1u);
    }
}

static void test_pair_result_transport_sequence_rejects_invalid_atomically(void)
{
    uint16_t round_id = UINT16_C(0x5aa5);
    uint16_t sequence = UINT16_C(0xa55a);

    assert(survey_pair_result_next_round_id(
               SURVEY_PAIR_RESULT_MAX_BATCH_COUNT, &round_id) ==
           PROTO_ERR_NO_SPACE);
    assert(round_id == UINT16_C(0x5aa5));
    assert(survey_pair_result_next_round_id(
               SURVEY_PAIR_RESULT_MAX_BATCH_COUNT + 1u, &round_id) ==
           PROTO_ERR_MALFORMED);
    assert(round_id == UINT16_C(0x5aa5));
    assert(survey_pair_result_next_round_id(0u, NULL) == PROTO_ERR_ARG);
    assert(survey_pair_result_transport_sequence(
               1u, SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT, &sequence) ==
           PROTO_ERR_MALFORMED);
    assert(sequence == UINT16_C(0xa55a));
    assert(survey_pair_result_transport_sequence(
               SURVEY_PAIR_RESULT_MAX_BATCH_COUNT + 1u, 0u, &sequence) ==
           PROTO_ERR_MALFORMED);
    assert(sequence == UINT16_C(0xa55a));
    assert(survey_pair_result_transport_sequence(
               UINT16_MAX, UINT16_MAX, &sequence) ==
           PROTO_ERR_MALFORMED);
    assert(sequence == UINT16_C(0xa55a));
    assert(survey_pair_result_transport_sequence(1u, 0u, NULL) ==
           PROTO_ERR_ARG);
}

static void test_sample_distance_usability(void)
{
    struct survey_sample value = sample();

    value.distance_mm = -4726;
    assert(!survey_sample_distance_usable(&value));

    value.distance_mm = 0;
    assert(!survey_sample_distance_usable(&value));

    value.distance_mm = SURVEY_MIN_USABLE_DISTANCE_MM + 1;
    assert(survey_sample_distance_usable(&value));

    value.distance_mm = 50;
    assert(survey_sample_distance_usable(&value));

    value.range_status = RANGE_RX_TIMEOUT;
    value.distance_mm = 4726;
    assert(!survey_sample_distance_usable(&value));
}

static void test_missing_samples_use_responder_outcomes_only(void)
{
    const uint16_t sample_0 = UINT16_C(1) << 0;
    const uint16_t sample_1 = UINT16_C(1) << 1;
    const uint16_t sample_2 = UINT16_C(1) << 2;

    assert(!survey_pair_missing_samples_all_unusable(
        0u, 0u, sample_0, sample_0));
    assert(!survey_pair_missing_samples_all_unusable(
        17u, 0u, UINT16_MAX, UINT16_MAX));
    assert(!survey_pair_missing_samples_all_unusable(
        3u, sample_1, sample_0 | sample_2, 0u));
    assert(survey_pair_missing_samples_all_unusable(
        3u, sample_1, 0u, sample_0 | sample_2));
    assert(!survey_pair_missing_samples_all_unusable(
        3u, sample_0 | sample_1 | sample_2, UINT16_MAX, UINT16_MAX));
}

static void test_pair_sample_admission_accepts_only_responder(void)
{
    struct survey_sample value = sample();
    uint16_t usable = 0u;
    uint16_t responder_usable = 0u;
    uint16_t initiator_unusable = 0u;
    uint16_t responder_unusable = 0u;
    const uint16_t expected_mask =
        (uint16_t)((UINT16_C(1) << value.pair.sample_count) - 1u);
    bool changed;

    value.distance_mm = 1000;
    for (uint16_t i = 0u; i < value.pair.sample_count; i++) {
        const uint16_t usable_before = usable;
        const uint16_t responder_usable_before = responder_usable;
        const uint16_t initiator_unusable_before = initiator_unusable;
        const uint16_t responder_unusable_before = responder_unusable;

        value.sample_index = i;
        changed = true;
        assert(survey_pair_note_sample_masks(
                   &value,
                   value.pair.initiator_id,
                   &usable,
                   &responder_usable,
                   &initiator_unusable,
                   &responder_unusable,
                   &changed) == PROTO_ERR_MALFORMED);
        assert(changed);
        assert(usable == usable_before &&
               responder_usable == responder_usable_before &&
               initiator_unusable == initiator_unusable_before &&
               responder_unusable == responder_unusable_before);
        assert(survey_pair_note_sample_masks(
                   &value,
                   value.pair.responder_id,
                   &usable,
                   &responder_usable,
                   &initiator_unusable,
                   &responder_unusable,
                   &changed) == PROTO_OK);
        assert(changed);
    }
    assert(usable == expected_mask);
    assert(responder_usable == expected_mask);
    value.sample_index = 0u;
    assert(survey_pair_note_sample_masks(
               &value,
               value.pair.responder_id,
               &usable,
               &responder_usable,
               &initiator_unusable,
               &responder_unusable,
               &changed) == PROTO_OK);
    assert(!changed);

    value.range_status = RANGE_RX_TIMEOUT;
    value.distance_mm = 0;
    usable = 0u;
    responder_usable = 0u;
    initiator_unusable = 0u;
    responder_unusable = 0u;
    assert(survey_pair_note_sample_masks(
               &value,
               value.pair.responder_id,
               &usable,
               &responder_usable,
               &initiator_unusable,
               &responder_unusable,
               &changed) == PROTO_OK);
    assert(changed);
    assert(responder_unusable == UINT16_C(1));
    changed = false;
    assert(survey_pair_note_sample_masks(
               &value,
               value.pair.initiator_id,
               &usable,
               &responder_usable,
               &initiator_unusable,
               &responder_unusable,
               &changed) == PROTO_ERR_MALFORMED);
    assert(!changed);
}

static void test_delayed_sequential_generation_cannot_mutate_current_masks(void)
{
    struct survey_sample delayed = sample();
    struct survey_pair current_pair = delayed.pair;
    uint16_t usable_mask = 0u;
    uint16_t responder_usable_mask = 0u;
    uint16_t initiator_unusable_mask = 0u;
    uint16_t responder_unusable_mask = 0u;
    bool changed = false;

    delayed.distance_mm = 1000;
    assert(survey_sample_matches_pair_run(&delayed, &current_pair, 7u));
    assert(!survey_sample_matches_pair_run(&delayed, &current_pair, 8u));
    if (survey_sample_matches_pair_run(&delayed, &current_pair, 8u)) {
        assert(survey_pair_note_sample_masks(
                   &delayed,
                   delayed.pair.initiator_id,
                   &usable_mask,
                   &responder_usable_mask,
                   &initiator_unusable_mask,
                   &responder_unusable_mask,
                   &changed) == PROTO_OK);
    }
    assert(usable_mask == 0u);
    assert(responder_usable_mask == 0u);
    assert(initiator_unusable_mask == 0u);
    assert(responder_unusable_mask == 0u);
    assert(!changed);
}

static void test_discovery_post_rf_terminal_preserves_delayed_report_horizon(void)
{
    assert(survey_gateway_discovery_collection_survives_terminal(
        true, 1u));
    assert(survey_gateway_discovery_collection_survives_terminal(
        false, 2u));
    assert(!survey_gateway_discovery_collection_survives_terminal(
        false, 0u));

    /*
     * A bounded flood can exhaust after RF starts but before the configured
     * delayed discovery run emits its first report. Keep the immutable
     * collection horizon alive, then close normally when that report arrives.
     */
    assert(survey_gateway_collection_decide(
               false, false, 0u, 1u, true) ==
           SURVEY_GATEWAY_COLLECTION_WAIT);
    assert(survey_gateway_collection_decide(
               true, false, 1u, 1u, true) ==
           SURVEY_GATEWAY_COLLECTION_CLOSE);
}

static void test_sample_nonce_is_unique_across_sequence_wrap(void)
{
    struct survey_sample value = sample();
    struct survey_pair next_incarnation;
    struct survey_pair projected_session_wrap;
    uint64_t first_nonce;
    uint64_t second_nonce;
    uint64_t wrapped_seq_nonce;

    value.pair.sample_count = 1000u;
    first_nonce = survey_sample_nonce(&value.pair, 0u);
    second_nonce = survey_sample_nonce(&value.pair, 1u);
    wrapped_seq_nonce = survey_sample_nonce(&value.pair, 255u);

    assert(first_nonce != 0u);
    assert(second_nonce != 0u);
    assert(wrapped_seq_nonce != 0u);
    assert(first_nonce != second_nonce);
    assert(first_nonce != wrapped_seq_nonce);
    assert(survey_sample_nonce(&value.pair, value.pair.sample_count) == 0u);

    next_incarnation = value.pair;
    value.pair.operation_generation =
        UINT64_C(0x0000000112345678);
    next_incarnation.operation_generation =
        UINT64_C(0x0000000212345678);
    assert(survey_operation_session_id(value.pair.operation_generation) ==
           survey_operation_session_id(
               next_incarnation.operation_generation));
    assert(survey_sample_nonce(&value.pair, 0u) !=
           survey_sample_nonce(&next_incarnation, 0u));

    projected_session_wrap = value.pair;
    projected_session_wrap.operation_generation++;
    assert(survey_sample_nonce(&value.pair, 0u) !=
           survey_sample_nonce(&projected_session_wrap, 0u));

    value.pair.survey_id = 0u;
    assert(survey_sample_nonce(&value.pair, 0u) == 0u);
}

static void expect_u64_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint64_t expected)
{
    const uint8_t *value = NULL;
    uint8_t len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &len) == PROTO_OK);
    assert(len == 8u);
    assert(proto_get_u64_le(value) == expected);
}

static void test_sample_tlvs_include_required_fields(void)
{
    uint8_t payload[128];
    size_t payload_len = 0u;
    const struct survey_sample value = sample();
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len, &value) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == value.pair.survey_id);

    expect_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, value.pair.initiator_id);
    expect_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, value.pair.responder_id);

    assert(tlv_find(payload, payload_len, TLV_SURVEY_ROUND_ID,
                    &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == value.round_id);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_INDEX, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == value.sample_index);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_COUNT, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == value.pair.sample_count);

    assert(tlv_find(payload, payload_len, TLV_DISTANCE_MM, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == (uint32_t)value.distance_mm);

    assert(tlv_find(payload, payload_len, TLV_QUALITY, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 1u);
    assert(tlv_value[0] == value.quality);

    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 1u);
    assert(tlv_value[0] == (uint8_t)value.range_status);
}

static void test_sample_tlvs_round_trip_round_ownership(void)
{
    struct survey_sample value = sample();
    struct survey_sample decoded = {0};
    uint8_t payload[SURVEY_SAMPLE_TLV_MAX_LEN];
    size_t payload_len = 0u;

    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &value) == PROTO_OK);
    assert(payload_len == SURVEY_SAMPLE_TLV_MAX_LEN -
                          PROTO_TLV_U64_ENCODED_LEN);
    assert(survey_extract_sample_tlvs(payload, payload_len, &decoded) ==
           PROTO_OK);
    assert(decoded.pair.survey_id == value.pair.survey_id);
    assert(decoded.pair.initiator_id == value.pair.initiator_id);
    assert(decoded.pair.responder_id == value.pair.responder_id);
    assert(decoded.pair.sample_count == value.pair.sample_count);
    assert(decoded.round_id == value.round_id);
    assert(decoded.sample_index == value.sample_index);
    assert(decoded.distance_mm == value.distance_mm);
    assert(decoded.quality == value.quality);
    assert(decoded.range_status == value.range_status);

    value.round_id = SURVEY_LEGACY_ROUND_ID;
    payload_len = 0u;
    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &value) == PROTO_OK);
    assert(payload_len == SURVEY_SAMPLE_TLV_MAX_LEN -
                          PROTO_TLV_U64_ENCODED_LEN -
                          PROTO_TLV_U16_ENCODED_LEN);
    assert(survey_extract_sample_tlvs(payload, payload_len, &decoded) ==
           PROTO_OK);
    assert(decoded.round_id == SURVEY_LEGACY_ROUND_ID);
}

static void test_sample_tlvs_bind_operation_generation(void)
{
    struct survey_sample value = sample();
    struct survey_sample decoded = {0};
    struct proto_packet packet = {0};
    uint8_t payload[SURVEY_SAMPLE_TLV_MAX_LEN +
                    PROTO_TLV_U64_ENCODED_LEN];
    size_t payload_len = 0u;

    value.pair.operation_generation =
        UINT64_C(0x00000002A1B2C3D4);
    assert(survey_append_sample_tlvs(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     &value) == PROTO_OK);
    assert(payload_len == SURVEY_SAMPLE_TLV_MAX_LEN);
    assert(survey_extract_sample_tlvs(payload,
                                      payload_len,
                                      &decoded) == PROTO_OK);
    assert_sample_equal(&decoded, &value);
    assert(survey_init_result_packet_from_reporter(
               &packet,
               &value,
               value.pair.responder_id,
               UINT64_C(0x9999),
               3u,
               (uint8_t)payload_len) == PROTO_OK);
    assert(packet.session_id ==
           survey_operation_session_id(
               value.pair.operation_generation));

    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_OPERATION_GENERATION,
                          value.pair.operation_generation + 1u) ==
           PROTO_OK);
    assert(survey_extract_sample_tlvs(payload,
                                      payload_len,
                                      &decoded) ==
           PROTO_ERR_MALFORMED);
}

static void test_pair_result_payload_validator_commits_atomically(void)
{
    const struct survey_sample value = sample();
    struct survey_sample decoded = {0};
    struct survey_sample unchanged = sample();
    struct survey_sample expected_unchanged;
    uint8_t payload[SURVEY_SAMPLE_TLV_MAX_LEN +
                    PROTO_TLV_U64_ENCODED_LEN +
                    PROTO_TLV_U8_ENCODED_LEN];
    size_t payload_len = 0u;

    unchanged.distance_mm++;
    unchanged.quality--;
    expected_unchanged = unchanged;
    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &value) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_TIMESTAMP_MS,
                          UINT64_C(0x0102030405060708)) == PROTO_OK);
    assert(survey_pair_result_payload_validate(payload,
                                               payload_len,
                                               &decoded) == PROTO_OK);
    assert_sample_equal(&decoded, &value);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         UINT8_C(0x27),
                         1u) == PROTO_OK);
    assert(survey_pair_result_payload_validate(payload,
                                               payload_len,
                                               &unchanged) ==
           PROTO_ERR_MALFORMED);
    assert_sample_equal(&unchanged, &expected_unchanged);
}

static void test_sample_tlv_parser_rejects_bad_scalar_lengths_atomically(void)
{
    const struct survey_sample value = sample();
    struct survey_sample output = sample();
    struct survey_sample expected_output;
    uint8_t payload[SURVEY_SAMPLE_TLV_MAX_LEN + 1u];
    const uint8_t *tlv_value = NULL;
    size_t payload_len = 0u;
    size_t value_offset;
    uint8_t tlv_len = 0u;

    output.pair.survey_id++;
    output.round_id++;
    output.sample_index = 0u;
    output.distance_mm++;
    output.quality--;
    output.range_status = RANGE_RX_TIMEOUT;
    expected_output = output;

    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &value) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_DISTANCE_MM,
                    &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == sizeof(uint32_t));
    value_offset = (size_t)(tlv_value - payload);
    payload[value_offset - 1u] = sizeof(uint32_t) - 1u;
    memmove(&payload[value_offset + sizeof(uint32_t) - 1u],
            &payload[value_offset + sizeof(uint32_t)],
            payload_len - value_offset - sizeof(uint32_t));
    payload_len--;
    assert(survey_extract_sample_tlvs(payload, payload_len, &output) ==
           PROTO_ERR_MALFORMED);
    assert_sample_equal(&output, &expected_output);

    payload_len = 0u;
    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len,
                                     &value) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS,
                    &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == sizeof(uint8_t));
    value_offset = (size_t)(tlv_value - payload);
    payload[value_offset - 1u] = sizeof(uint16_t);
    payload[payload_len++] = 0xa5u;
    assert(survey_extract_sample_tlvs(payload, payload_len, &output) ==
           PROTO_ERR_MALFORMED);
    assert_sample_equal(&output, &expected_output);
}

static void append_conflicting_survey_singleton(uint8_t *payload,
                                                size_t payload_cap,
                                                size_t *payload_len,
                                                uint8_t type)
{
    switch (type) {
    case TLV_SURVEY_ID:
        assert(tlv_append_u32(payload, payload_cap, payload_len, type,
                              UINT32_C(0x01020304)) == PROTO_OK);
        break;
    case TLV_INITIATOR_ID:
    case TLV_RESPONDER_ID:
    case TLV_ANCHOR_ID:
        assert(tlv_append_u64(payload, payload_cap, payload_len, type,
                              UINT64_C(0x0102030405060708)) == PROTO_OK);
        break;
    case TLV_SAMPLE_COUNT:
    case TLV_SURVEY_ROUND_ID:
    case TLV_SAMPLE_INDEX:
    case TLV_DISCOVERY_SLOT_MS:
        assert(tlv_append_u16(payload, payload_cap, payload_len, type,
                              2u) == PROTO_OK);
        break;
    case TLV_DISTANCE_MM:
    case TLV_DURATION_MS:
    case TLV_DISCOVERY_START_DELAY_MS:
        assert(tlv_append_u32(payload, payload_cap, payload_len, type,
                              123u) == PROTO_OK);
        break;
    case TLV_QUALITY:
    case TLV_RANGE_STATUS:
    case TLV_DISCOVERY_SLOT_COUNT:
        assert(tlv_append_u8(payload, payload_cap, payload_len, type,
                             1u) == PROTO_OK);
        break;
    default:
        assert(false);
    }
}

static void test_sample_parser_rejects_conflicting_singletons_atomically(void)
{
    static const uint8_t singleton_types[] = {
        TLV_SURVEY_ID,
        TLV_INITIATOR_ID,
        TLV_RESPONDER_ID,
        TLV_SAMPLE_COUNT,
        TLV_SURVEY_ROUND_ID,
        TLV_SAMPLE_INDEX,
        TLV_DISTANCE_MM,
        TLV_QUALITY,
        TLV_RANGE_STATUS,
    };
    const struct survey_sample value = sample();
    struct survey_sample output = sample();
    struct survey_sample expected_output;
    uint8_t payload[128];
    size_t payload_len;

    output.pair.survey_id++;
    output.sample_index = 0u;
    output.distance_mm++;
    expected_output = output;
    for (size_t i = 0u;
         i < sizeof(singleton_types) / sizeof(singleton_types[0]);
         i++) {
        payload_len = 0u;
        assert(survey_append_sample_tlvs(
                   payload, sizeof(payload), &payload_len, &value) ==
               PROTO_OK);
        append_conflicting_survey_singleton(
            payload,
            sizeof(payload),
            &payload_len,
            singleton_types[i]);
        output = expected_output;
        assert(survey_extract_sample_tlvs(
                   payload, payload_len, &output) ==
               PROTO_ERR_MALFORMED);
        assert_sample_equal(&output, &expected_output);
    }
}

static void test_reach_request_tlvs_include_survey_and_duration(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;

    assert(survey_append_reach_request_tlvs(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            0xABCDEF01u,
                                            250u) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 0xABCDEF01u);
    assert(tlv_find(payload, payload_len, TLV_DURATION_MS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 250u);

    assert(survey_extract_reach_request_tlvs(payload,
                                             payload_len,
                                             &survey_id,
                                             &duration_ms) == PROTO_OK);
    assert(survey_id == 0xABCDEF01u);
    assert(duration_ms == 250u);
}

static void test_reach_request_parser_rejects_malformed_tlvs(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0xABCDEF01u) == PROTO_OK);
    assert(survey_extract_reach_request_tlvs(payload,
                                             payload_len,
                                             &survey_id,
                                             &duration_ms) == PROTO_ERR_NOT_FOUND);

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DURATION_MS,
                          250u) == PROTO_OK);
    assert(survey_extract_reach_request_tlvs(payload,
                                             payload_len,
                                             &survey_id,
                                             &duration_ms) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    payload[payload_len++] = TLV_SURVEY_ID;
    payload[payload_len++] = 2u;
    payload[payload_len++] = 0x01u;
    payload[payload_len++] = 0x02u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DURATION_MS,
                          250u) == PROTO_OK);
    assert(survey_extract_reach_request_tlvs(payload,
                                             payload_len,
                                             &survey_id,
                                             &duration_ms) == PROTO_ERR_MALFORMED);
}

static void test_discovery_start_tlvs_round_trip_timing_config(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 20000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 4u,
    };
    uint8_t payload[48];
    size_t payload_len = 0u;
    struct survey_discovery_config decoded = {0};
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    assert(survey_discovery_config_validate(&config) == PROTO_OK);
    assert(survey_discovery_duration_ms(&config) == 8000u);
    assert(survey_append_discovery_start_tlvs(payload,
                                              sizeof(payload),
                                              &payload_len,
                                              &config) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == config.survey_id);
    assert(tlv_find(payload,
                    payload_len,
                    TLV_DISCOVERY_START_DELAY_MS,
                    &tlv_value,
                    &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == config.start_delay_ms);
    assert(tlv_find(payload, payload_len, TLV_DISCOVERY_SLOT_MS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == config.slot_ms);
    assert(tlv_find(payload,
                    payload_len,
                    TLV_DISCOVERY_SLOT_COUNT,
                    &tlv_value,
                    &tlv_len) == PROTO_OK);
    assert(tlv_len == 1u);
    assert(tlv_value[0] == config.slot_count);
    assert(tlv_find(payload, payload_len, TLV_DURATION_MS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 8000u);

    assert(survey_extract_discovery_start_tlvs(payload,
                                               payload_len,
                                               &decoded) == PROTO_OK);
    assert(decoded.survey_id == config.survey_id);
    assert(decoded.start_delay_ms == config.start_delay_ms);
    assert(decoded.slot_ms == config.slot_ms);
    assert(decoded.slot_count == config.slot_count);
    assert(decoded.round_count == config.round_count);
}

static void test_discovery_start_rejects_conflicting_singletons(void)
{
    static const uint8_t singleton_types[] = {
        TLV_SURVEY_ID,
        TLV_DISCOVERY_START_DELAY_MS,
        TLV_DISCOVERY_SLOT_MS,
        TLV_DISCOVERY_SLOT_COUNT,
        TLV_DURATION_MS,
    };
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 20000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 4u,
    };
    struct survey_discovery_config decoded;
    uint8_t payload[96];
    size_t payload_len;

    for (size_t i = 0u;
         i < sizeof(singleton_types) / sizeof(singleton_types[0]);
         i++) {
        payload_len = 0u;
        assert(survey_append_discovery_start_tlvs(
                   payload, sizeof(payload), &payload_len, &config) ==
               PROTO_OK);
        append_conflicting_survey_singleton(
            payload,
            sizeof(payload),
            &payload_len,
            singleton_types[i]);
        memset(&decoded, 0xa5, sizeof(decoded));
        assert(survey_extract_discovery_start_tlvs(
                   payload, payload_len, &decoded) ==
               PROTO_ERR_MALFORMED);
    }
}

static void test_discovery_round_count_comes_from_runtime_profile(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 20000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 2u,
    };
    const struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
        .value.discovery = {
            .start_delay_ms = 20000u,
            .slot_ms = 40u,
            .slot_count = 50u,
            .round_count = 2u,
            .report_grace_ms = 250u,
            .operation_budget_ms = 600000u,
        },
    };
    struct survey_discovery_config decoded = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(survey_append_discovery_start_tlvs(
               payload, sizeof(payload), &payload_len, &config) == PROTO_OK);
    assert(operation_policy_append_tlv(
               payload, sizeof(payload), &payload_len, &policy) == PROTO_OK);
    assert(survey_extract_discovery_start_tlvs(
               payload, payload_len, &decoded) == PROTO_OK);
    assert(decoded.round_count == 2u);
    assert(survey_discovery_duration_ms(&decoded) == 4000u);
}

static void test_discovery_slot_validation_uses_physical_probe_budget(void)
{
    struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = SURVEY_DISCOVERY_MIN_SLOT_MS,
        .slot_count = 6u,
        .round_count = 4u,
    };
    uint32_t tx_budget_ms = survey_discovery_probe_tx_budget_ms();

    assert(tx_budget_ms >= SURVEY_DISCOVERY_TX_TIMEOUT_MS +
                           SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS);
    assert(SURVEY_DISCOVERY_MIN_SLOT_MS ==
           tx_budget_ms + SURVEY_DISCOVERY_RX_GUARD_MS);
    assert(survey_discovery_config_validate(&config) == PROTO_OK);
    config.operation_generation = UINT64_C(0x0000000100000000);
    assert(survey_discovery_config_validate(&config) == PROTO_ERR_MALFORMED);
    config.operation_generation = 0u;
    config.slot_ms--;
    assert(survey_discovery_config_validate(&config) == PROTO_ERR_MALFORMED);
    config.slot_ms = SURVEY_DISCOVERY_MIN_SLOT_MS;
    config.round_count = 0u;
    assert(survey_discovery_config_validate(&config) == PROTO_ERR_MALFORMED);
    config.round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT + 1u;
    assert(survey_discovery_config_validate(&config) == PROTO_ERR_MALFORMED);
}

static void test_discovery_round_scheduler_has_one_continuous_window(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
    };
    const uint64_t anchor_id = UINT64_C(0x1111222233334444);
    const uint32_t round_duration_ms = config.slot_ms * config.slot_count;

    for (uint8_t opportunity = 0u;
         opportunity < config.round_count;
         opportunity++) {
        struct survey_discovery_attempt_schedule nominal = {0};

        assert(survey_discovery_schedule_attempt(&config, anchor_id,
                                                 opportunity, 0u,
                                                 &nominal) == PROTO_OK);
        assert(nominal.window_start_ms ==
               (uint32_t)opportunity * round_duration_ms);
        assert(nominal.window_end_ms ==
               (uint32_t)(opportunity + 1u) * round_duration_ms);
        assert(nominal.tx_ms <= nominal.latest_tx_start_ms);
        assert(nominal.latest_tx_start_ms < nominal.slot_end_ms);
        assert(survey_discovery_schedule_attempt(
                   &config, anchor_id, opportunity,
                   nominal.latest_tx_start_ms + 1u,
                   &nominal) == PROTO_ERR_BUSY);
    }

    {
        struct survey_discovery_attempt_schedule first = {0};
        struct survey_discovery_attempt_schedule second = {0};
        uint32_t after_first_miss_ms;

        assert(survey_discovery_schedule_attempt(&config, anchor_id, 0u, 0u,
                                                 &first) == PROTO_OK);
        after_first_miss_ms = first.latest_tx_start_ms + 1u;
        assert(survey_discovery_schedule_attempt(
                   &config, anchor_id, 0u,
                   after_first_miss_ms,
                   &first) == PROTO_ERR_BUSY);
        assert(survey_discovery_schedule_attempt(
                   &config, anchor_id, 1u,
                   after_first_miss_ms,
                   &second) == PROTO_OK);
    }
    assert(survey_discovery_schedule_attempt(
               &config, anchor_id, config.round_count, 0u,
               &(struct survey_discovery_attempt_schedule){0}) ==
           PROTO_ERR_ARG);

    for (uint8_t slot = 0u; slot < config.slot_count; slot++) {
        uint32_t slot_tx_ms = 0u;
        struct survey_discovery_attempt_schedule slot_sched = {0};

        assert(survey_discovery_opportunity_slot_tx_ms(&config, slot, 0u,
                                                       &slot_tx_ms) == PROTO_OK);
        assert(slot_tx_ms == (uint32_t)slot * config.slot_ms);
        assert(survey_discovery_schedule_slot_attempt(&config, slot, 0u, 0u,
                                                      &slot_sched) == PROTO_OK);
        assert(slot_sched.tx_ms == slot_tx_ms + SURVEY_DISCOVERY_RX_GUARD_MS);
    }
    assert(survey_discovery_opportunity_slot_tx_ms(&config, config.slot_count,
                                                   0u, &(uint32_t){0}) == PROTO_ERR_ARG);
    assert(survey_discovery_schedule_slot_attempt(&config, config.slot_count,
                                                  0u, 0u,
                                                  &(struct survey_discovery_attempt_schedule){0}) == PROTO_ERR_ARG);
}

static void test_pending_discovery_report_survives_queue_and_route_pressure(void)
{
    struct survey_pending_report_state state = {0};
    const uint32_t now_ms = UINT32_MAX - 100u;
    const uint32_t earliest_ms = now_ms + 200u;
    const uint32_t deadline_ms = earliest_ms +
                                 SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS;

    assert(survey_pending_report_begin(&state, 0xABCDEF01u, now_ms,
                                       earliest_ms) == PROTO_OK);
    assert(state.active);
    assert(state.survey_id == 0xABCDEF01u);
    assert(state.deadline_ms == deadline_ms);
    assert(survey_pending_report_action(&state, now_ms) ==
           SURVEY_PENDING_REPORT_WAIT);
    assert(survey_pending_report_delay_ms(&state, now_ms) == 200u);
    assert(survey_pending_report_action(&state, earliest_ms) ==
           SURVEY_PENDING_REPORT_ATTEMPT);

    /* Queue full, report worker busy, and route delay retain one identity. */
    assert(survey_pending_report_note_temporary_failure(&state,
                                                        earliest_ms) ==
           PROTO_OK);
    assert(state.survey_id == 0xABCDEF01u);
    assert(state.retry_count == 1u);
    assert(survey_pending_report_delay_ms(&state, earliest_ms) ==
           SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS);
    assert(survey_pending_report_action(
               &state,
               earliest_ms + SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS) ==
           SURVEY_PENDING_REPORT_ATTEMPT);
    assert(survey_pending_report_note_temporary_failure(
               &state,
               earliest_ms + SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS) ==
           PROTO_OK);
    assert(state.retry_count == 2u);
    assert(survey_pending_report_delay_ms(
               &state,
               earliest_ms + SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS) ==
           SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS * 2u);

    assert(survey_pending_report_action(&state, deadline_ms) ==
           SURVEY_PENDING_REPORT_EXPIRED);
    assert(survey_pending_report_note_temporary_failure(&state,
                                                        deadline_ms) ==
           PROTO_ERR_BUSY);
    assert(state.active);
    survey_pending_report_clear(&state);
    assert(survey_pending_report_action(&state, deadline_ms) ==
           SURVEY_PENDING_REPORT_IDLE);
}

static void test_discovery_report_custody_tracks_upstream_hops(void)
{
    const uint32_t direct_ms = survey_discovery_report_custody_ms(1u);
    const uint32_t two_hop_ms = survey_discovery_report_custody_ms(2u);
    const uint32_t three_hop_ms = survey_discovery_report_custody_ms(3u);
    const uint32_t maximum_ms =
        survey_discovery_report_custody_ms(SURVEY_DEFAULT_TTL);

    assert(direct_ms == SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS);
    assert(direct_ms == 30000u);
    assert(two_hop_ms == 34000u);
    assert(two_hop_ms > SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS);
    assert(three_hop_ms == 38000u);
    assert(maximum_ms == 42000u);
    assert(maximum_ms == SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS);

    assert(survey_discovery_report_custody_ms(0u) == maximum_ms);
    assert(survey_discovery_report_custody_ms(SURVEY_DEFAULT_TTL + 1u) ==
           maximum_ms);
    assert(survey_discovery_report_custody_ms(UINT8_MAX) == maximum_ms);
}

static void test_discovery_report_deadline_stays_fixed_after_eligibility(void)
{
    const uint32_t eligible_ms = 1000u;
    const uint64_t expected_direct_deadline =
        eligible_ms + SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS;
    const uint64_t before_wrap = UINT32_MAX - 15u;
    const uint64_t wrapped_now = (UINT64_C(1) << 32) | 16u;

    assert(survey_discovery_report_deadline_ms(
               900u, eligible_ms, 1u) == expected_direct_deadline);
    assert(survey_discovery_report_deadline_ms(
               eligible_ms, eligible_ms, 1u) == expected_direct_deadline);
    assert(survey_discovery_report_deadline_ms(
               2500u, eligible_ms, 1u) == expected_direct_deadline);
    assert(survey_discovery_report_deadline_ms(
               expected_direct_deadline, eligible_ms, 1u) ==
           expected_direct_deadline);
    assert(survey_discovery_report_deadline_ms(
               expected_direct_deadline + 1000u, eligible_ms, 1u) ==
           expected_direct_deadline);

    assert(survey_discovery_report_deadline_ms(
               UINT32_MAX - 31u, (uint32_t)before_wrap, 1u) ==
           before_wrap + SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS);
    assert(survey_discovery_report_deadline_ms(
               wrapped_now, (uint32_t)before_wrap, 1u) ==
           before_wrap + SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS);
    assert(survey_discovery_report_deadline_ms(
               UINT64_MAX - 1u, (uint32_t)(UINT64_MAX - 1u), 1u) ==
           UINT64_MAX);
}

static void test_discovery_slot_count_tlv_defaults_and_overrides(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    uint8_t slot_count = 0u;

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0xABCDEF01u) == PROTO_OK);
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   6u,
                                                   &slot_count) == PROTO_OK);
    assert(slot_count == 6u);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT,
                         4u) == PROTO_OK);
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   6u,
                                                   &slot_count) == PROTO_OK);
    assert(slot_count == 4u);

    payload[payload_len - 1u] = 0u;
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   6u,
                                                   &slot_count) == PROTO_ERR_MALFORMED);
    payload[payload_len - 1u] = SURVEY_DISCOVERY_MAX_SLOT_COUNT + 1u;
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   6u,
                                                   &slot_count) == PROTO_ERR_MALFORMED);
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len - 1u,
                                                   6u,
                                                   &slot_count) == PROTO_ERR_MALFORMED);
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   0u,
                                                   &slot_count) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT, 4u) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT, 5u) == PROTO_OK);
    assert(survey_extract_discovery_slot_count_tlv(payload,
                                                   payload_len,
                                                   6u,
                                                   &slot_count) ==
           PROTO_ERR_MALFORMED);
}

static void test_discovery_expected_node_count_is_optional_and_bounded(void)
{
    uint8_t payload[8];
    uint16_t decoded = UINT16_MAX;
    bool present = true;

    assert(survey_extract_expected_node_count_tlv(
               payload, 0u, &decoded, &present) == PROTO_OK);
    assert(!present);
    assert(decoded == 0u);

    for (uint16_t expected = 1u;
         expected <= SURVEY_DISCOVERY_MAX_SLOT_COUNT;
         expected++) {
        size_t payload_len = 0u;

        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_EXPECTED_NODE_COUNT,
                              expected) == PROTO_OK);
        decoded = 0u;
        present = false;
        assert(survey_extract_expected_node_count_tlv(
                   payload, payload_len, &decoded, &present) == PROTO_OK);
        assert(present);
        assert(decoded == expected);
    }

    for (uint16_t invalid = 0u;
         invalid <= SURVEY_DISCOVERY_MAX_SLOT_COUNT + 1u;
         invalid += SURVEY_DISCOVERY_MAX_SLOT_COUNT + 1u) {
        size_t payload_len = 0u;

        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_EXPECTED_NODE_COUNT,
                              invalid) == PROTO_OK);
        assert(survey_extract_expected_node_count_tlv(
                   payload, payload_len, &decoded, &present) ==
               PROTO_ERR_MALFORMED);
    }

    payload[0] = TLV_EXPECTED_NODE_COUNT;
    payload[1] = sizeof(uint8_t);
    payload[2] = 1u;
    assert(survey_extract_expected_node_count_tlv(
               payload, 3u, &decoded, &present) == PROTO_ERR_MALFORMED);

    {
        size_t payload_len = 0u;

        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_EXPECTED_NODE_COUNT,
                              2u) == PROTO_OK);
        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_EXPECTED_NODE_COUNT,
                              3u) == PROTO_OK);
        assert(survey_extract_expected_node_count_tlv(
                   payload, payload_len, &decoded, &present) ==
               PROTO_ERR_MALFORMED);
    }
}

static void test_multi_output_survey_parsers_fail_atomically(void)
{
    const struct survey_discovery_config config = {
        .operation_generation = UINT64_C(0x1234000000000001),
        .survey_id = 0x10203040u,
        .start_delay_ms = 90000u,
        .slot_ms = 40u,
        .slot_count = 4u,
        .round_count = 2u,
    };
    const struct survey_reachability_entry first_entry = {
        .peer_id = UINT64_C(0x1111222233334444),
        .rssi_dbm = -61,
        .quality = 82u,
    };
    struct survey_reachability_entry decoded_entries[2];
    struct survey_reachability_entry entries_sentinel[2];
    struct survey_discovery_config decoded_config;
    struct survey_discovery_config config_sentinel;
    uint8_t payload[128];
    uint8_t invalid_entry[SURVEY_REACHABILITY_ENTRY_LEN] = {0};
    size_t payload_len = 0u;
    size_t entry_count = 77u;
    uint64_t anchor_id = UINT64_C(0xA5A5A5A5A5A5A5A5);
    uint32_t survey_id = UINT32_C(0xA5A5A5A5);
    uint32_t duration_ms = UINT32_C(0x5A5A5A5A);
    uint16_t expected_count = UINT16_C(0xA55A);
    bool expected_count_present = true;

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          config.survey_id) == PROTO_OK);
    assert(survey_extract_reach_request_tlvs(
               payload, payload_len, &survey_id, &duration_ms) ==
           PROTO_ERR_NOT_FOUND);
    assert(survey_id == UINT32_C(0xA5A5A5A5));
    assert(duration_ms == UINT32_C(0x5A5A5A5A));

    payload_len = 0u;
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          0u) == PROTO_OK);
    assert(survey_extract_expected_node_count_tlv(
               payload,
               payload_len,
               &expected_count,
               &expected_count_present) == PROTO_ERR_MALFORMED);
    assert(expected_count == UINT16_C(0xA55A));
    assert(expected_count_present);

    payload_len = 0u;
    assert(survey_append_reach_report_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               config.survey_id,
               UINT64_C(0x9999888877776666),
               &first_entry,
               1u) == PROTO_OK);
    proto_put_u64_le(invalid_entry, UINT64_C(0x5555666677778888));
    invalid_entry[8] = (uint8_t)-70;
    invalid_entry[9] = 101u;
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_REACHABILITY_ENTRY,
                            invalid_entry,
                            sizeof(invalid_entry)) == PROTO_OK);
    memset(decoded_entries, 0xA5, sizeof(decoded_entries));
    entries_sentinel[0] = decoded_entries[0];
    entries_sentinel[1] = decoded_entries[1];
    assert(survey_extract_reach_report_tlvs(
               payload,
               payload_len,
               &survey_id,
               &anchor_id,
               decoded_entries,
               sizeof(decoded_entries) / sizeof(decoded_entries[0]),
               &entry_count) == PROTO_ERR_MALFORMED);
    assert(survey_id == UINT32_C(0xA5A5A5A5));
    assert(anchor_id == UINT64_C(0xA5A5A5A5A5A5A5A5));
    assert(entry_count == 77u);
    assert(memcmp(decoded_entries,
                  entries_sentinel,
                  sizeof(decoded_entries)) == 0);

    payload_len = 0u;
    assert(survey_append_discovery_start_tlvs(
               payload, sizeof(payload), &payload_len, &config) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DURATION_MS,
                          survey_discovery_duration_ms(&config)) == PROTO_OK);
    memset(&decoded_config, 0xA5, sizeof(decoded_config));
    config_sentinel = decoded_config;
    assert(survey_extract_discovery_start_tlvs(
               payload, payload_len, &decoded_config) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&decoded_config,
                  &config_sentinel,
                  sizeof(decoded_config)) == 0);
}

static void test_ml_anchor_pair_request_accepts_optional_slots_and_ignores_sample_count(void)
{
    uint8_t payload[24];
    size_t payload_len = 0u;
    struct survey_ml_anchor_pair_request request = {0};

    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_ID,
                          CMD_ML_START_ANCHOR_PAIR_SURVEY) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SAMPLE_COUNT,
                          SURVEY_MAX_SAMPLE_COUNT) == PROTO_OK);

    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) == PROTO_OK);
    assert(request.discovery_slot_count == 6u);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT,
                         SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT) == PROTO_OK);
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) == PROTO_OK);
    assert(request.discovery_slot_count == SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT);

    payload[payload_len - 1u] = SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT;
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) == PROTO_OK);
    assert(request.discovery_slot_count == SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT,
                         SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT) ==
           PROTO_OK);
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) ==
           PROTO_ERR_MALFORMED);
}

static void test_ml_anchor_pair_request_rejects_invalid_slot_counts(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct survey_ml_anchor_pair_request request = {0};

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DISCOVERY_SLOT_COUNT,
                         SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT - 1u) == PROTO_OK);
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) == PROTO_ERR_MALFORMED);

    payload[payload_len - 1u] = SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT + 1u;
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      6u,
                                                      &request) == PROTO_ERR_MALFORMED);

    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len,
                                                      SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT - 1u,
                                                      &request) == PROTO_ERR_MALFORMED);
    assert(survey_extract_ml_anchor_pair_request_tlvs(payload,
                                                      payload_len - 1u,
                                                      6u,
                                                      &request) == PROTO_ERR_MALFORMED);
}

static void test_discovery_timing_uses_packet_age(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 4u,
    };
    struct survey_discovery_timing timing = {0};
    uint32_t start_at_ms = 0u;

    assert(survey_discovery_timing_from_age(&config, 500u, &timing) == PROTO_OK);
    assert(timing.pending);
    assert(!timing.active);
    assert(!timing.expired);
    assert(timing.wait_ms == 1500u);
    assert(timing.elapsed_ms == 0u);
    assert(timing.duration_ms == 8000u);
    assert(survey_discovery_start_at_ms(&timing, 1000u, &start_at_ms) ==
           PROTO_OK);
    assert(start_at_ms == 2500u);

    assert(survey_discovery_timing_from_age(&config, 800u, &timing) == PROTO_OK);
    assert(survey_discovery_start_at_ms(&timing, 1300u, &start_at_ms) ==
           PROTO_OK);
    assert(start_at_ms == 2500u);

    assert(survey_discovery_timing_from_age(&config, 2123u, &timing) == PROTO_OK);
    assert(!timing.pending);
    assert(timing.active);
    assert(!timing.expired);
    assert(timing.wait_ms == 0u);
    assert(timing.elapsed_ms == 123u);
    assert(survey_discovery_start_at_ms(&timing, 2623u, &start_at_ms) ==
           PROTO_OK);
    assert(start_at_ms == 2500u);

    assert(survey_discovery_timing_from_age(&config, 9560u, &timing) == PROTO_OK);
    assert(!timing.pending);
    assert(timing.active);
    assert(!timing.expired);

    assert(survey_discovery_timing_from_age(&config, 10000u, &timing) == PROTO_OK);
    assert(!timing.pending);
    assert(!timing.active);
    assert(timing.expired);
    assert(timing.elapsed_ms == 8000u);
    assert(survey_discovery_start_at_ms(&timing, 10500u, &start_at_ms) ==
           PROTO_ERR_ARG);
}

static void test_discovery_report_delay_uses_deterministic_anchor_slot(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
    };
    uint32_t delay_ms = 0u;

    assert(survey_discovery_report_delay_ms(&config, 0u, 2270u, &delay_ms) == PROTO_OK);
    assert(delay_ms == 960u);
    assert(survey_discovery_report_delay_ms(&config, 3u, 2270u, &delay_ms) == PROTO_OK);
    assert(delay_ms == 960u + (3u * 2270u));
    assert(survey_discovery_report_delay_ms(&config, 6u, 2270u, &delay_ms) ==
           PROTO_ERR_MALFORMED);
    assert(survey_discovery_report_delay_ms(&config, 0u, 0u, &delay_ms) ==
           PROTO_ERR_MALFORMED);
}

static void test_discovery_report_delay_rejects_overflow(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = 1000u,
        .slot_count = 50u,
        .round_count = 4u,
    };
    uint32_t delay_ms = 0u;

    assert(survey_discovery_report_delay_ms(&config,
                                            49u,
                                            UINT32_MAX,
                                            &delay_ms) == PROTO_ERR_NO_SPACE);
}

static void test_discovery_packets_use_diagnostic_ids(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 4u,
    };
    struct proto_packet packet = {0};

    assert(survey_init_discovery_start_packet(&packet,
                                              0x9999888877776666ull,
                                              &config,
                                              44u,
                                              20u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_DISCOVERY_START);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u);
    assert(packet.src_id == 0x9999888877776666ull);
    assert(packet.dst_id == 0u);
    assert(packet.session_id == config.survey_id);
    assert(packet.seq == 44u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 20u);

    assert(survey_init_discovery_report_packet(&packet,
                                               0x1111222233334444ull,
                                               0x9999888877776666ull,
                                               config.survey_id,
                                               config.operation_generation,
                                               UINT32_C(0x12345678),
                                               45u,
                                               24u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert(packet.src_id == 0x1111222233334444ull);
    assert(packet.dst_id == 0x9999888877776666ull);
    assert(packet.session_id == UINT32_C(0x12345678));
    assert(packet.seq == 45u);
    assert(packet.payload_len == 24u);
}

static void test_reach_report_tlvs_include_peer_entries(void)
{
    const struct survey_reachability_entry entries[] = {
        {
            .peer_id = 0x1111222233334444ull,
            .rssi_dbm = -61,
            .quality = 82u,
        },
        {
            .peer_id = 0x5555666677778888ull,
            .rssi_dbm = -74,
            .quality = 63u,
        },
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    uint8_t entry_count = 0u;

    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           0xAABBCCDDu,
                                           0x9999888877776666ull,
                                           entries,
                                           sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 0xAABBCCDDu);
    assert(tlv_find(payload, payload_len, TLV_ANCHOR_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 8u);
    assert(proto_get_u64_le(tlv_value) == 0x9999888877776666ull);

    for (size_t offset = 0u; offset < payload_len;) {
        uint8_t type = payload[offset];
        uint8_t len = payload[offset + 1u];

        offset += 2u;
        if (type == TLV_REACHABILITY_ENTRY) {
            assert(len == SURVEY_REACHABILITY_ENTRY_LEN);
            assert(proto_get_u64_le(&payload[offset]) == entries[entry_count].peer_id);
            assert((int8_t)payload[offset + 8u] == entries[entry_count].rssi_dbm);
            assert(payload[offset + 9u] == entries[entry_count].quality);
            entry_count++;
        }
        offset += len;
    }
    assert(entry_count == 2u);
}

static void test_reach_report_tlv_parser_round_trips_entries(void)
{
    const struct survey_reachability_entry entries[] = {
        {
            .peer_id = 0x1111222233334444ull,
            .rssi_dbm = -61,
            .quality = 82u,
        },
        {
            .peer_id = 0x5555666677778888ull,
            .rssi_dbm = -74,
            .quality = 63u,
        },
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    struct survey_reachability_entry decoded[2] = {0};
    size_t entry_count = 99u;

    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           0xAABBCCDDu,
                                           0x9999888877776666ull,
                                           entries,
                                           sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
    assert(survey_extract_reach_report_tlvs(payload,
                                            payload_len,
                                            &survey_id,
                                            &anchor_id,
                                            decoded,
                                            sizeof(decoded) / sizeof(decoded[0]),
                                            &entry_count) == PROTO_OK);
    assert(survey_id == 0xAABBCCDDu);
    assert(anchor_id == 0x9999888877776666ull);
    assert(entry_count == 2u);
    assert(decoded[0].peer_id == entries[0].peer_id);
    assert(decoded[0].rssi_dbm == entries[0].rssi_dbm);
    assert(decoded[0].quality == entries[0].quality);
    assert(decoded[1].peer_id == entries[1].peer_id);
    assert(decoded[1].rssi_dbm == entries[1].rssi_dbm);
    assert(decoded[1].quality == entries[1].quality);
}

static void test_reach_report_rejects_conflicting_identity_singletons(void)
{
    const struct survey_reachability_entry entries[] = {
        {
            .peer_id = 0x1111222233334444ull,
            .rssi_dbm = -61,
            .quality = 82u,
        },
        {
            .peer_id = 0x5555666677778888ull,
            .rssi_dbm = -74,
            .quality = 63u,
        },
    };
    const uint8_t singleton_types[] = {
        TLV_SURVEY_ID,
        TLV_ANCHOR_ID,
    };
    struct survey_reachability_entry decoded[2];
    uint8_t payload[128];
    size_t payload_len;
    size_t entry_count;
    uint32_t survey_id;
    uint64_t anchor_id;

    for (size_t i = 0u;
         i < sizeof(singleton_types) / sizeof(singleton_types[0]);
         i++) {
        payload_len = 0u;
        assert(survey_append_reach_report_tlvs(
                   payload,
                   sizeof(payload),
                   &payload_len,
                   0xAABBCCDDu,
                   0x9999888877776666ull,
                   entries,
                   sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
        append_conflicting_survey_singleton(
            payload,
            sizeof(payload),
            &payload_len,
            singleton_types[i]);
        assert(survey_extract_reach_report_tlvs(
                   payload,
                   payload_len,
                   &survey_id,
                   &anchor_id,
                   decoded,
                   sizeof(decoded) / sizeof(decoded[0]),
                   &entry_count) == PROTO_ERR_MALFORMED);
    }
}

static void test_reach_report_tlv_parser_accepts_empty_peer_list(void)
{
    const uint32_t expected_survey_id = 0xAABBCCDDu;
    const uint64_t expected_anchor_id = 0x9999888877776666ull;
    const uint64_t expected_gateway_id = 0x8888777766665555ull;
    struct survey_gateway_context gateway = {0};
    struct proto_packet packet = {0};
    uint8_t payload[32];
    size_t payload_len = 0u;
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    uint64_t stored_anchor_id = 0u;
    size_t entry_count = 99u;
    size_t stored_entry_count = 99u;
    enum command_status stored_status = COMMAND_INTERNAL_ERROR;

    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           expected_survey_id,
                                           expected_anchor_id,
                                           NULL,
                                           0u) == PROTO_OK);
    assert(survey_extract_reach_report_tlvs(payload,
                                            payload_len,
                                            &survey_id,
                                            &anchor_id,
                                            NULL,
                                            0u,
                                            &entry_count) == PROTO_OK);
    assert(survey_id == expected_survey_id);
    assert(anchor_id == expected_anchor_id);
    assert(entry_count == 0u);

    assert(payload_len <= UINT8_MAX);
    assert(survey_init_discovery_report_packet(&packet,
                                               expected_anchor_id,
                                               expected_gateway_id,
                                               expected_survey_id,
                                               0u,
                                               1u,
                                               77u,
                                               (uint8_t)payload_len) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == expected_anchor_id);
    assert(packet.dst_id == expected_gateway_id);
    assert(packet.session_id == 1u);
    assert(packet.seq == 77u);
    assert(packet.payload_len == payload_len);

    assert(survey_gateway_begin(&gateway, expected_survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&gateway,
                                            survey_id,
                                            anchor_id,
                                            NULL,
                                            entry_count) == PROTO_OK);
    assert(gateway.report_count == 1u);
    assert(survey_gateway_report_info_at(
               &gateway,
               0u,
               &stored_anchor_id,
               &stored_entry_count,
               &stored_status) == PROTO_OK);
    assert(stored_anchor_id == expected_anchor_id);
    assert(stored_entry_count == 0u);
    assert(stored_status == COMMAND_OK);
}

static void test_reach_report_tlv_parser_rejects_invalid_entries(void)
{
    uint8_t payload[96];
    size_t payload_len = 0u;
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    struct survey_reachability_entry decoded[1] = {0};
    size_t entry_count = 0u;
    uint8_t raw[SURVEY_REACHABILITY_ENTRY_LEN] = {0};

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0xAABBCCDDu) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_ANCHOR_ID,
                          0x9999888877776666ull) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_REACHABILITY_ENTRY,
                            raw,
                            2u) == PROTO_OK);
    assert(survey_extract_reach_report_tlvs(payload,
                                            payload_len,
                                            &survey_id,
                                            &anchor_id,
                                            decoded,
                                            sizeof(decoded) / sizeof(decoded[0]),
                                            &entry_count) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           0xAABBCCDDu,
                                           0x9999888877776666ull,
                                           &(struct survey_reachability_entry){
                                               .peer_id = 0x1111222233334444ull,
                                               .rssi_dbm = -61,
                                               .quality = 82u,
                                           },
                                           1u) == PROTO_OK);
    assert(survey_extract_reach_report_tlvs(payload,
                                            payload_len,
                                            &survey_id,
                                            &anchor_id,
                                            decoded,
                                            0u,
                                            &entry_count) == PROTO_ERR_NO_SPACE);

    payload_len = 0u;
    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           0xAABBCCDDu,
                                           0x9999888877776666ull,
                                           &(struct survey_reachability_entry){
                                               .peer_id = 0x9999888877776666ull,
                                               .rssi_dbm = -61,
                                               .quality = 82u,
                                           },
                                           1u) == PROTO_OK);
    assert(survey_extract_reach_report_tlvs(payload,
                                            payload_len,
                                            &survey_id,
                                            &anchor_id,
                                            decoded,
                                            sizeof(decoded) / sizeof(decoded[0]),
                                            &entry_count) == PROTO_ERR_MALFORMED);
}

static void test_reach_report_packet_is_diagnostic_gateway_bound(void)
{
    struct proto_packet packet = {0};

    assert(survey_init_reach_report_packet(&packet,
                                           0x1111222233334444ull,
                                           0x9999888877776666ull,
                                           0xAABBCCDDu,
                                           11u,
                                           32u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_REACH_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == 0x1111222233334444ull);
    assert(packet.dst_id == 0x9999888877776666ull);
    assert(packet.session_id == 0xAABBCCDDu);
    assert(packet.seq == 11u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 32u);
}

static void test_reach_request_packet_is_diagnostic_broadcast(void)
{
    struct proto_packet packet = {0};

    assert(survey_init_reach_request_packet(&packet,
                                            0x9999888877776666ull,
                                            0xAABBCCDDu,
                                            10u,
                                            12u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_REACH_REQ);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == 0x9999888877776666ull);
    assert(packet.dst_id == 0u);
    assert(packet.session_id == 0xAABBCCDDu);
    assert(packet.seq == 10u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 12u);
}

static void test_pair_tlv_parser_round_trips_prepare_payload(void)
{
    uint8_t payload[64];
    size_t payload_len = 0u;
    const struct survey_pair pair = {
        .survey_id = 0xAABBCCDDu,
        .initiator_id = 0x1111222233334444ull,
        .responder_id = 0x5555666677778888ull,
        .sample_count = 7u,
    };
    struct survey_pair decoded = {0};

    assert(survey_append_pair_tlvs(payload, sizeof(payload), &payload_len, &pair) == PROTO_OK);
    assert(survey_extract_pair_tlvs(payload, payload_len, &decoded) == PROTO_OK);
    assert(decoded.survey_id == pair.survey_id);
    assert(decoded.initiator_id == pair.initiator_id);
    assert(decoded.responder_id == pair.responder_id);
    assert(decoded.sample_count == pair.sample_count);
}

static void test_pair_tlv_parser_rejects_missing_or_invalid_fields(void)
{
    uint8_t payload[64];
    size_t payload_len = 0u;
    struct survey_pair decoded = {0};

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0xAABBCCDDu) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_INITIATOR_ID,
                          0x1111222233334444ull) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_RESPONDER_ID,
                          0x5555666677778888ull) == PROTO_OK);
    assert(survey_extract_pair_tlvs(payload, payload_len, &decoded) == PROTO_ERR_NOT_FOUND);

    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SAMPLE_COUNT,
                          0u) == PROTO_OK);
    assert(survey_extract_pair_tlvs(payload, payload_len, &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          0xAABBCCDDu) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_INITIATOR_ID,
                          0x1111222233334444ull) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_RESPONDER_ID,
                          0x1111222233334444ull) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SAMPLE_COUNT,
                          7u) == PROTO_OK);
    assert(survey_extract_pair_tlvs(payload, payload_len, &decoded) == PROTO_ERR_MALFORMED);
}

static void test_pair_prepare_packet_targets_initiator_anchor(void)
{
    const struct survey_pair pair = {
        .survey_id = 0xAABBCCDDu,
        .initiator_id = 0x1111222233334444ull,
        .responder_id = 0x5555666677778888ull,
        .sample_count = 7u,
    };
    struct proto_packet packet = {0};

    assert(survey_init_pair_prepare_packet(&packet,
                                           &pair,
                                           0x9999888877776666ull,
                                           pair.initiator_id,
                                           11u,
                                           24u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_PAIR_PREPARE);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == 0x9999888877776666ull);
    assert(packet.dst_id == pair.initiator_id);
    assert(packet.session_id == pair.survey_id);
    assert(packet.seq == 11u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 24u);

    assert(survey_init_pair_prepare_packet(&packet,
                                           &(struct survey_pair){
                                               .survey_id = 0xAABBCCDDu,
                                               .initiator_id = pair.initiator_id,
                                               .responder_id = pair.initiator_id,
                                               .sample_count = 7u,
                                           },
                                           0x9999888877776666ull,
                                           pair.initiator_id,
                                           11u,
                                           24u) == PROTO_ERR_MALFORMED);
}

static void test_reachability_graph_plans_unique_pairs_with_requested_samples(void)
{
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
        {.peer_id = 0x3333000000000003ull, .rssi_dbm = -64, .quality = 78u},
        {.peer_id = 0x9999000000000009ull, .rssi_dbm = -70, .quality = 60u},
    };
    const struct survey_reachability_entry b_entries[] = {
        {.peer_id = 0x1111000000000001ull, .rssi_dbm = -62, .quality = 81u},
    };
    const struct survey_reachability_entry c_entries[] = {
        {.peer_id = 0x1111000000000001ull, .rssi_dbm = -63, .quality = 79u},
    };
    const struct survey_reachability_report reports[] = {
        {
            .anchor_id = 0x1111000000000001ull,
            .entries = a_entries,
            .entry_count = sizeof(a_entries) / sizeof(a_entries[0]),
        },
        {
            .anchor_id = 0x2222000000000002ull,
            .entries = b_entries,
            .entry_count = sizeof(b_entries) / sizeof(b_entries[0]),
        },
        {
            .anchor_id = 0x3333000000000003ull,
            .entries = c_entries,
            .entry_count = sizeof(c_entries) / sizeof(c_entries[0]),
        },
    };
    struct survey_pair pairs[4] = {0};
    size_t pair_count = 99u;

    assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                               reports,
                                               sizeof(reports) / sizeof(reports[0]),
                                               5u,
                                               pairs,
                                               sizeof(pairs) / sizeof(pairs[0]),
                                               &pair_count) == PROTO_OK);
    assert(pair_count == 3u);
    assert(pairs[0].survey_id == 0xAABBCCDDu);
    assert(pairs[0].initiator_id == 0x1111000000000001ull);
    assert(pairs[0].responder_id == 0x2222000000000002ull);
    assert(pairs[0].sample_count == 5u);
    assert(pairs[1].initiator_id == 0x1111000000000001ull);
    assert(pairs[1].responder_id == 0x3333000000000003ull);
    assert(pairs[1].sample_count == 5u);
    assert(pairs[2].initiator_id == 0x1111000000000001ull);
    assert(pairs[2].responder_id == 0x9999000000000009ull);
    assert(pairs[2].sample_count == 5u);
}

static void test_reachability_retains_strongest_peers_deterministically(void)
{
    struct survey_reachability_entry forward[
        SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {0};
    struct survey_reachability_entry reverse[
        SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {0};
    struct survey_reachability_entry candidates[
        SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 2u];
    size_t forward_count = 0u;
    size_t reverse_count = 0u;

    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT; i++) {
        candidates[i] = (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0xa700000000000100) + i,
            .rssi_dbm = (int8_t)(-90 + (int)i),
            .quality = (uint8_t)(70u + i),
        };
    }
    candidates[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] =
        (struct survey_reachability_entry) {
            .peer_id = candidates[0].peer_id,
            .rssi_dbm = -20,
            .quality = 99u,
        };
    candidates[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 1u] =
        (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0xa700000000000200),
            .rssi_dbm = -30,
            .quality = 90u,
        };

    for (size_t i = 0u; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        assert(survey_reachability_entry_retain(
                   forward, SURVEY_GATEWAY_MAX_PEERS_PER_REPORT,
                   &forward_count, &candidates[i]) == PROTO_OK);
    }
    for (size_t i = sizeof(candidates) / sizeof(candidates[0]); i > 0u; i--) {
        assert(survey_reachability_entry_retain(
                   reverse, SURVEY_GATEWAY_MAX_PEERS_PER_REPORT,
                   &reverse_count, &candidates[i - 1u]) == PROTO_OK);
    }

    assert(forward_count == SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
    assert(reverse_count == forward_count);
    assert(memcmp(forward, reverse, sizeof(forward)) == 0);
    assert(forward[0].peer_id == candidates[0].peer_id);
    assert(forward[0].quality == 99u);
    assert(forward[1].peer_id ==
           candidates[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 1u].peer_id);
    for (size_t i = 0u; i < forward_count; i++) {
        assert(forward[i].peer_id != candidates[1].peer_id);
        if (i > 0u) {
            assert(forward[i - 1u].quality > forward[i].quality ||
                   (forward[i - 1u].quality == forward[i].quality &&
                    forward[i - 1u].rssi_dbm >= forward[i].rssi_dbm));
        }
    }
}

static void test_reachability_report_rejects_non_anchor_endpoints(void)
{
    const uint64_t anchor_id = UINT64_C(0xa700000000000001);
    const uint64_t gateway_id = UINT64_C(0x9999888877776666);
    struct survey_reachability_entry entries[] = {
        {
            .peer_id = UINT64_C(0xa700000000000002),
            .rssi_dbm = -60,
            .quality = 80u,
        },
        {
            .peer_id = UINT64_C(0xa700000000000003),
            .rssi_dbm = -64,
            .quality = 75u,
        },
    };

    assert(survey_reachability_report_endpoints_validate(
               anchor_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
    assert(survey_reachability_report_endpoints_validate(
               gateway_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_MALFORMED);

    entries[0].peer_id = anchor_id;
    assert(survey_reachability_report_endpoints_validate(
               anchor_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_MALFORMED);
    entries[0].peer_id = gateway_id;
    assert(survey_reachability_report_endpoints_validate(
               anchor_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_MALFORMED);
    entries[0].peer_id = 0u;
    assert(survey_reachability_report_endpoints_validate(
               anchor_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_MALFORMED);
    entries[0].peer_id = entries[1].peer_id;
    assert(survey_reachability_report_endpoints_validate(
               anchor_id, gateway_id, entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_MALFORMED);
}

static void test_reachability_graph_retains_directed_partial_components(void)
{
    const uint64_t anchor_a = UINT64_C(0xa700000000000001);
    const uint64_t anchor_b = UINT64_C(0xa700000000000002);
    const uint64_t anchor_c = UINT64_C(0xa700000000000003);
    const uint64_t anchor_d = UINT64_C(0xa700000000000004);
    const uint64_t isolated = UINT64_C(0xa700000000000005);
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = anchor_b, .rssi_dbm = -60, .quality = 90u},
    };
    const struct survey_reachability_entry c_entries[] = {
        {.peer_id = anchor_d, .rssi_dbm = -65, .quality = 85u},
    };
    const struct survey_reachability_report reports[] = {
        {.anchor_id = anchor_a, .entries = a_entries, .entry_count = 1u},
        {.anchor_id = anchor_c, .entries = c_entries, .entry_count = 1u},
        {.anchor_id = isolated},
    };
    const struct survey_reachability_report reversed_reports[] = {
        reports[2], reports[1], reports[0],
    };
    struct survey_pair pairs[4] = {0};
    struct survey_pair reversed_pairs[4] = {0};
    struct survey_gateway_context context;
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    assert(survey_plan_pairs_from_reachability(
               0xAABBCCDDu, reports,
               sizeof(reports) / sizeof(reports[0]), 2u,
               pairs, sizeof(pairs) / sizeof(pairs[0]),
               &pair_count) == PROTO_OK);
    assert(pair_count == 2u);
    assert(pairs[0].initiator_id == anchor_a);
    assert(pairs[0].responder_id == anchor_b);
    assert(pairs[1].initiator_id == anchor_c);
    assert(pairs[1].responder_id == anchor_d);
    assert(survey_plan_pairs_from_reachability(
               0xAABBCCDDu, reversed_reports,
               sizeof(reversed_reports) / sizeof(reversed_reports[0]), 2u,
               reversed_pairs,
               sizeof(reversed_pairs) / sizeof(reversed_pairs[0]),
               &reversed_pair_count) == PROTO_OK);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    for (size_t i = 0u; i < sizeof(reports) / sizeof(reports[0]); i++) {
        assert(survey_gateway_note_reach_report(
                   &context,
                   0xAABBCCDDu,
                   reports[i].anchor_id,
                   reports[i].entries,
                   reports[i].entry_count) == PROTO_OK);
    }
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == pair_count);
    assert(!context.topology_complete);

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(
               &context,
               0xAABBCCDDu,
               anchor_a,
               a_entries,
               sizeof(a_entries) / sizeof(a_entries[0])) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == 1u);
    assert(context.topology_complete);
}

static void test_generated_complete_reachability_counts_for_one_to_six_anchors(void)
{
    struct survey_reachability_entry entries[6][5] = {{{0}}};
    struct survey_reachability_report reports[6] = {{0}};
    struct survey_pair pairs[15] = {{0}};

    for (size_t anchor_count = 1u; anchor_count <= 6u; anchor_count++) {
        size_t pair_count = 0u;

        memset(entries, 0, sizeof(entries));
        memset(reports, 0, sizeof(reports));
        memset(pairs, 0, sizeof(pairs));
        for (size_t anchor = 0u; anchor < anchor_count; anchor++) {
            size_t entry_count = 0u;

            reports[anchor].anchor_id = UINT64_C(0xa700000000000001) + anchor;
            reports[anchor].entries = entries[anchor];
            for (size_t peer = 0u; peer < anchor_count; peer++) {
                if (peer == anchor) {
                    continue;
                }
                entries[anchor][entry_count++] = (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0xa700000000000001) + peer,
                    .rssi_dbm = -60,
                    .quality = 80u,
                };
            }
            reports[anchor].entry_count = entry_count;
        }

        assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                                   reports,
                                                   anchor_count,
                                                   1u,
                                                   pairs,
                                                   sizeof(pairs) / sizeof(pairs[0]),
                                                   &pair_count) == PROTO_OK);
        assert(pair_count == anchor_count * (anchor_count - 1u) / 2u);
        if (anchor_count == 2u) {
            assert(pair_count == 1u);
            assert(pairs[0].initiator_id != pairs[0].responder_id);
        }
    }
}

static void test_complete_k8_reaches_degree_cap_for_every_anchor(void)
{
    enum { ANCHOR_COUNT = 8 };
    struct survey_reachability_entry entries[ANCHOR_COUNT][ANCHOR_COUNT - 1u];
    struct survey_reachability_report reports[ANCHOR_COUNT];
    struct survey_pair pairs[
        (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;

    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        size_t entry_count = 0u;

        reports[anchor].anchor_id = UINT64_C(0xa800000000000000) + anchor;
        reports[anchor].entries = entries[anchor];
        for (size_t peer = 0u; peer < ANCHOR_COUNT; peer++) {
            if (peer == anchor) {
                continue;
            }
            entries[anchor][entry_count++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0xa800000000000000) + peer,
                    .rssi_dbm = -60,
                    .quality = 80u,
                };
        }
        reports[anchor].entry_count = entry_count;
    }

    assert(survey_plan_pairs_from_reachability(
               0xAABBCCDDu, reports, ANCHOR_COUNT, 1u,
               pairs, sizeof(pairs) / sizeof(pairs[0]),
               &pair_count) == PROTO_OK);
    assert(pair_count ==
           (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t initiator =
            (size_t)(pairs[i].initiator_id - UINT64_C(0xa800000000000000));
        const size_t responder =
            (size_t)(pairs[i].responder_id - UINT64_C(0xa800000000000000));

        assert(initiator < ANCHOR_COUNT);
        assert(responder < ANCHOR_COUNT);
        assert(initiator != responder);
        degree[initiator]++;
        degree[responder]++;
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
}

static void test_complete_k8_maximizes_high_quality_edges_under_degree_cap(void)
{
    enum { ANCHOR_COUNT = 8 };
    struct survey_reachability_entry entries[ANCHOR_COUNT][ANCHOR_COUNT - 1u];
    struct survey_reachability_report reports[ANCHOR_COUNT];
    struct survey_pair pairs[
        (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t high_quality_pair_count = 0u;
    size_t pair_count = 0u;

    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        size_t entry_count = 0u;

        reports[anchor].anchor_id = UINT64_C(0xa900000000000000) + anchor;
        reports[anchor].entries = entries[anchor];
        for (size_t peer = 0u; peer < ANCHOR_COUNT; peer++) {
            if (peer == anchor) {
                continue;
            }
            entries[anchor][entry_count++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0xa900000000000000) + peer,
                    .rssi_dbm = -60,
                    .quality = anchor < 2u || peer < 2u ? 100u : 1u,
                };
        }
        reports[anchor].entry_count = entry_count;
    }

    assert(survey_plan_pairs_from_reachability(
               0xAABBCCDDu, reports, ANCHOR_COUNT, 1u,
               pairs, sizeof(pairs) / sizeof(pairs[0]),
               &pair_count) == PROTO_OK);
    assert(pair_count ==
           (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t initiator =
            (size_t)(pairs[i].initiator_id - UINT64_C(0xa900000000000000));
        const size_t responder =
            (size_t)(pairs[i].responder_id - UINT64_C(0xa900000000000000));

        assert(initiator < ANCHOR_COUNT);
        assert(responder < ANCHOR_COUNT);
        degree[initiator]++;
        degree[responder]++;
        if (initiator < 2u || responder < 2u) {
            high_quality_pair_count++;
        }
    }
    assert(high_quality_pair_count == 12u);
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
}

static struct survey_gateway_report_slot *round_test_report(
    struct survey_gateway_context *context,
    uint64_t anchor_id)
{
    for (size_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == anchor_id &&
            context->reports[i].metadata != UINT8_MAX) {
            return &context->reports[i];
        }
    }
    assert(false);
    return NULL;
}

static uint8_t round_test_node_index(
    const struct survey_gateway_context *context,
    uint64_t node_id)
{
    for (uint8_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == node_id) {
            return i;
        }
    }
    assert(false);
    return UINT8_MAX;
}

static void round_test_context_init(
    struct survey_gateway_context *context,
    const struct survey_pair *pairs,
    size_t pair_count)
{
    assert(survey_gateway_begin(context, 0xAABBCCDDu, 2u) == PROTO_OK);

    for (size_t i = 0u; i < pair_count; i++) {
        const uint64_t endpoint_ids[] = {
            pairs[i].initiator_id,
            pairs[i].responder_id,
        };

        for (size_t endpoint = 0u; endpoint < 2u; endpoint++) {
            struct survey_gateway_reverse_hint reverse_hint = {
                .target_id = endpoint_ids[endpoint],
                .next_hop_id = endpoint_ids[endpoint],
                .quality = 100u,
                .hop_count = 1u,
                .valid = true,
            };

            if (survey_gateway_reach_report_compare(
                    context,
                    endpoint_ids[endpoint],
                    NULL,
                    0u,
                    COMMAND_OK) == PROTO_OK) {
                continue;
            }
            assert(survey_gateway_note_reach_report_with_reverse_hint(
                       context,
                       context->survey_id,
                       endpoint_ids[endpoint],
                       NULL,
                       0u,
                       &reverse_hint) == PROTO_OK);
        }
    }
    for (size_t i = 0u; i < pair_count; i++) {
        context->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_index =
                round_test_node_index(context, pairs[i].initiator_id),
            .responder_index =
                round_test_node_index(context, pairs[i].responder_id),
        };
    }
    context->pair_count = (uint8_t)pair_count;
    context->pairs_planned = true;
}

static void round_test_add_peer(struct survey_gateway_context *context,
                                uint64_t anchor_id,
                                uint64_t peer_id)
{
    struct survey_gateway_report_slot *slot =
        round_test_report(context, anchor_id);
    uint8_t peer_index = UINT8_MAX;
    uint8_t entry_count = slot->metadata & 0x0fu;

    for (uint8_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == peer_id) {
            peer_index = i;
            break;
        }
    }
    if (peer_index == UINT8_MAX) {
        assert(context->node_count < SURVEY_GATEWAY_MAX_REPORTS);
        peer_index = context->node_count;
        context->node_ids[context->node_count++] = peer_id;
    }
    assert(entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
    const uint8_t report_order_bits =
        slot->entries[entry_count].peer_index & 0xc0u;

    slot->entries[entry_count] =
        (struct survey_gateway_compact_reachability_entry) {
        .peer_index = (uint8_t)(report_order_bits | peer_index),
        .rssi_dbm = -60,
        .quality = 80u,
    };
    entry_count++;
    slot->metadata = (uint8_t)((slot->metadata & 0xf0u) | entry_count);
}

static void test_pair_rounds_serialize_shared_endpoints(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA1u, .responder_id = 0xB1u},
        {.initiator_id = 0xA1u, .responder_id = 0xC1u},
    };
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    round_test_context_init(&context, pairs, 2u);
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);
    assert(metadata[0].round_index == 0u);
    assert(metadata[1].round_index == 1u);
    assert(metadata[0].pair_count_in_round == 1u);
    assert(metadata[1].pair_count_in_round == 1u);
}

static void test_pair_rounds_check_asymmetric_cross_neighborhoods_both_ways(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA2u, .responder_id = 0xB2u},
        {.initiator_id = 0xC2u, .responder_id = 0xD2u},
    };
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    round_test_context_init(&context, pairs, 2u);
    round_test_add_peer(&context, 0xC2u, 0xA2u);
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);

    round_test_context_init(&context, pairs, 2u);
    round_test_add_peer(&context, 0xA2u, 0xC2u);
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);
}

static void test_pair_rounds_require_separation_proof_for_sparse_reports(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA3u, .responder_id = 0xB3u},
        {.initiator_id = 0xC3u, .responder_id = 0xD3u},
    };
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    round_test_context_init(&context, pairs, 2u);
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);
    assert(metadata[0].round_index == 0u);
    assert(metadata[0].pair_index_in_round == 0u);
    assert(metadata[0].pair_count_in_round == 1u);
    assert(metadata[1].round_index == 1u);
    assert(metadata[1].pair_index_in_round == 0u);
    assert(metadata[1].pair_count_in_round == 1u);

    for (size_t i = 0u; i < context.node_count; i++) {
        context.reports[i].reverse_next_hop_index = UINT8_MAX;
    }
    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 2u);

    round_test_report(&context, 0xA3u)->reverse_next_hop_index =
        round_test_node_index(&context, 0xA3u);
    round_test_report(&context, 0xA3u)->reverse_hop_count = 1u;
    round_test_report(&context, 0xB3u)->reverse_next_hop_index =
        round_test_node_index(&context, 0xB3u);
    round_test_report(&context, 0xB3u)->reverse_hop_count = 1u;
    round_test_report(&context, 0xC3u)->reverse_next_hop_index =
        round_test_node_index(&context, 0xC3u);
    round_test_report(&context, 0xC3u)->reverse_hop_count = 3u;
    round_test_report(&context, 0xD3u)->reverse_next_hop_index =
        round_test_node_index(&context, 0xD3u);
    round_test_report(&context, 0xD3u)->reverse_hop_count = 3u;
    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 1u);
    assert(metadata[0].round_index == 0u);
    assert(metadata[0].pair_index_in_round == 0u);
    assert(metadata[0].pair_count_in_round == 2u);
    assert(metadata[1].round_index == 0u);
    assert(metadata[1].pair_index_in_round == 1u);
    assert(metadata[1].pair_count_in_round == 2u);
}

static void test_pair_rounds_serialize_shared_third_neighbor(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA6u, .responder_id = 0xB6u},
        {.initiator_id = 0xC6u, .responder_id = 0xD6u},
    };
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    size_t round_count = 0u;

    round_test_context_init(&context, pairs, 2u);
    round_test_add_peer(&context, 0xA6u, 0xE6u);
    round_test_add_peer(&context, 0xD6u, 0xE6u);

    assert(survey_gateway_plan_pair_rounds(&context,
                                           metadata,
                                           2u,
                                           &round_count) == PROTO_OK);
    assert(round_count == 2u);
    assert(metadata[0].round_index != metadata[1].round_index);
}

static void test_pair_rounds_pack_deterministically_without_reordering_pairs(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA4u, .responder_id = 0xB4u},
        {.initiator_id = 0xC4u, .responder_id = 0xD4u},
        {.initiator_id = 0xE4u, .responder_id = 0xF4u},
        {.initiator_id = 0x74u, .responder_id = 0x84u},
    };
    struct survey_gateway_context context;
    struct survey_gateway_pair_entry original_pairs[4];
    struct survey_pair_round_metadata first[4] = {0};
    struct survey_pair_round_metadata second[4] = {0};
    size_t first_round_count = 0u;
    size_t second_round_count = 0u;

    round_test_context_init(&context, pairs, 4u);
    memcpy(original_pairs, context.pairs, sizeof(original_pairs));
    round_test_report(&context, 0xC4u)->reverse_hop_count = 3u;
    round_test_report(&context, 0xD4u)->reverse_hop_count = 3u;
    round_test_report(&context, 0xE4u)->reverse_hop_count = 3u;
    round_test_report(&context, 0xF4u)->reverse_hop_count = 3u;
    round_test_add_peer(&context, 0xE4u, 0xA4u);
    round_test_add_peer(&context, 0x74u, 0xC4u);

    assert(survey_gateway_plan_pair_rounds(&context,
                                                  first,
                                                  4u,
                                                  &first_round_count) == PROTO_OK);
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  second,
                                                  4u,
                                                  &second_round_count) == PROTO_OK);
    assert(first_round_count == 2u);
    assert(second_round_count == first_round_count);
    assert(memcmp(first, second, sizeof(first)) == 0);
    assert(memcmp(context.pairs, original_pairs, sizeof(original_pairs)) == 0);
    assert(first[0].round_index == 0u);
    assert(first[1].round_index == 0u);
    assert(first[2].round_index == 1u);
    assert(first[3].round_index == 1u);
    assert(first[0].pair_index_in_round == 0u);
    assert(first[1].pair_index_in_round == 1u);
    assert(first[2].pair_index_in_round == 0u);
    assert(first[3].pair_index_in_round == 1u);
    for (size_t i = 0u; i < 4u; i++) {
        assert(first[i].pair_count_in_round == 2u);
    }
}

static void test_pair_rounds_use_hop_depth_only_for_incomplete_neighborhoods(void)
{
    const struct survey_pair pairs[] = {
        {.initiator_id = 0xA5u, .responder_id = 0xB5u},
        {.initiator_id = 0xC5u, .responder_id = 0xD5u},
    };
    struct survey_gateway_context context;
    struct survey_pair_round_metadata metadata[2] = {0};
    struct survey_gateway_report_slot *saturated;
    size_t round_count = 0u;

    round_test_context_init(&context, pairs, 2u);
    saturated = round_test_report(&context, 0xA5u);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT; i++) {
        round_test_add_peer(&context, 0xA5u, 0xF000u + i);
    }
    round_test_report(&context, 0xA5u)->reverse_hop_count = 1u;
    round_test_report(&context, 0xB5u)->reverse_hop_count = 1u;
    round_test_report(&context, 0xC5u)->reverse_hop_count = 3u;
    round_test_report(&context, 0xD5u)->reverse_hop_count = 3u;

    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 1u);

    round_test_report(&context, 0xC5u)->reverse_hop_count = 2u;
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);

    round_test_report(&context, 0xC5u)->reverse_hop_count = 0u;
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);

    round_test_report(&context, 0xC5u)->reverse_hop_count = 3u;
    saturated->entries[0].peer_index =
        (uint8_t)((saturated->entries[0].peer_index & 0xc0u) |
                  round_test_node_index(&context, 0xC5u));
    assert(survey_gateway_plan_pair_rounds(&context,
                                                  metadata,
                                                  2u,
                                                  &round_count) == PROTO_OK);
    assert(round_count == 2u);
}

static void test_gateway_context_collects_reports_and_sequences_pairs(void)
{
    struct survey_gateway_context context;
    struct survey_pair pair = {0};
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
        {.peer_id = 0x3333000000000003ull, .rssi_dbm = -64, .quality = 78u},
        {.peer_id = 0x9999000000000009ull, .rssi_dbm = -70, .quality = 60u},
    };
    const struct survey_reachability_entry b_entries[] = {
        {.peer_id = 0x1111000000000001ull, .rssi_dbm = -62, .quality = 81u},
    };
    const struct survey_reachability_entry c_entries[] = {
        {.peer_id = 0x1111000000000001ull, .rssi_dbm = -63, .quality = 79u},
    };

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 5u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x1111000000000001ull,
                                            a_entries,
                                            sizeof(a_entries) / sizeof(a_entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x2222000000000002ull,
                                            b_entries,
                                            sizeof(b_entries) / sizeof(b_entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x3333000000000003ull,
                                            c_entries,
                                            sizeof(c_entries) / sizeof(c_entries[0])) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.report_count == 3u);
    assert(context.pair_count == 3u);

    assert(survey_gateway_pair_at(&context, 0u, &pair) == PROTO_OK);
    assert(pair.survey_id == 0xAABBCCDDu);
    assert(pair.initiator_id == 0x1111000000000001ull);
    assert(pair.responder_id == 0x2222000000000002ull);
    assert(pair.sample_count == 5u);

    assert(survey_gateway_pair_at(&context, 1u, &pair) == PROTO_OK);
    assert(pair.initiator_id == 0x1111000000000001ull);
    assert(pair.responder_id == 0x3333000000000003ull);
    assert(pair.sample_count == 5u);

    assert(survey_gateway_pair_at(&context, 2u, &pair) == PROTO_OK);
    assert(pair.initiator_id == 0x1111000000000001ull);
    assert(pair.responder_id == 0x9999000000000009ull);
    assert(pair.sample_count == 5u);

    assert(survey_gateway_pair_at(&context, context.pair_count, &pair) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_gateway_pair_orientation_is_independent_of_reverse_depth(void)
{
    const uint32_t survey_id = 0xAABBCCDDu;
    const uint64_t lower_id = 0x1111000000000001ull;
    const uint64_t higher_id = 0x2222000000000002ull;
    const struct survey_reachability_entry lower_entries[] = {
        {.peer_id = higher_id, .rssi_dbm = -61, .quality = 82u},
    };
    const struct survey_reachability_entry higher_entries[] = {
        {.peer_id = lower_id, .rssi_dbm = -62, .quality = 81u},
    };
    struct survey_gateway_reverse_hint lower_hint = {
        .target_id = lower_id,
        .next_hop_id = higher_id,
        .quality = 80u,
        .hop_count = 2u,
        .valid = true,
    };
    struct survey_gateway_reverse_hint higher_hint = {
        .target_id = higher_id,
        .next_hop_id = higher_id,
        .quality = 90u,
        .hop_count = 1u,
        .valid = true,
    };
    struct survey_gateway_context context;
    struct survey_pair pair = {0};

    assert(survey_gateway_begin(&context, survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, lower_id, lower_entries, 1u,
               &lower_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, higher_id, higher_entries, 1u,
               &higher_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_pair_at(&context, 0u, &pair) == PROTO_OK);
    assert(pair.initiator_id == lower_id);
    assert(pair.responder_id == higher_id);

    lower_hint.hop_count = 1u;
    assert(survey_gateway_begin(&context, survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, lower_id, lower_entries, 1u,
               &lower_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, higher_id, higher_entries, 1u,
               &higher_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_pair_at(&context, 0u, &pair) == PROTO_OK);
    assert(pair.initiator_id == lower_id);
    assert(pair.responder_id == higher_id);

    assert(survey_gateway_begin(&context, survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(
               &context, survey_id, lower_id, lower_entries, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, higher_id, higher_entries, 1u,
               &higher_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_pair_at(&context, 0u, &pair) == PROTO_OK);
    assert(pair.initiator_id == lower_id);
    assert(pair.responder_id == higher_id);
}

static void test_gateway_context_preserves_first_duplicate_report_and_hint(void)
{
    struct survey_gateway_context context;
    const struct survey_reachability_entry first_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
    };
    const struct survey_reachability_entry replacement_entries[] = {
        {.peer_id = 0x3333000000000003ull, .rssi_dbm = -64, .quality = 78u},
    };
    const struct survey_reachability_entry b_entries[] = {
        {.peer_id = 0x1111000000000001ull, .rssi_dbm = -62, .quality = 81u},
    };
    const struct survey_gateway_reverse_hint first_hint = {
        .target_id = 0x1111000000000001ull,
        .next_hop_id = 0x4444000000000004ull,
        .quality = 91u,
        .hop_count = 3u,
        .valid = true,
    };
    const struct survey_gateway_reverse_hint duplicate_hint = {
        .target_id = 0x1111000000000001ull,
        .next_hop_id = 0x5555000000000005ull,
        .quality = 37u,
        .hop_count = 2u,
        .valid = true,
    };
    struct survey_gateway_reverse_hint stored_hint = {0};
    struct survey_reachability_entry stored_entry = {0};
    struct survey_pair stored_pair = {0};
    enum command_status stored_status;
    uint64_t stored_anchor_id;
    size_t stored_entry_count;

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               0x1111000000000001ull,
               first_entries,
               sizeof(first_entries) / sizeof(first_entries[0]),
               &first_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               0x1111000000000001ull,
               first_entries,
               sizeof(first_entries) / sizeof(first_entries[0]),
               &duplicate_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               0x1111000000000001ull,
               replacement_entries,
               sizeof(replacement_entries) / sizeof(replacement_entries[0]),
               &duplicate_hint) == PROTO_ERR_MALFORMED);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x2222000000000002ull,
                                            b_entries,
                                            sizeof(b_entries) / sizeof(b_entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x3333000000000003ull,
                                            NULL,
                                            0u) == PROTO_OK);

    assert(context.report_count == 3u);
    assert(survey_gateway_report_info_at(
               &context,
               0u,
               &stored_anchor_id,
               &stored_entry_count,
               &stored_status) == PROTO_OK);
    assert(stored_anchor_id == first_hint.target_id);
    assert(stored_entry_count == 1u);
    assert(stored_status == COMMAND_OK);
    assert(survey_gateway_report_entry_at(
               &context, 0u, 0u, &stored_entry) == PROTO_OK);
    assert(memcmp(&stored_entry,
                  &first_entries[0],
                  sizeof(stored_entry)) == 0);
    assert(survey_gateway_reverse_hint_for_target(&context,
                                                  first_hint.target_id,
                                                  &stored_hint) == PROTO_OK);
    assert(stored_hint.next_hop_id == first_hint.next_hop_id);
    assert(stored_hint.quality == first_hint.quality);
    assert(stored_hint.hop_count == first_hint.hop_count);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == 1u);
    assert(survey_gateway_pair_at(&context, 0u, &stored_pair) == PROTO_OK);
    assert(stored_pair.initiator_id == 0x1111000000000001ull);
    assert(stored_pair.responder_id == 0x2222000000000002ull);
}

static void test_gateway_context_rejects_stale_or_oversized_reports(void)
{
    struct survey_gateway_context context;
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 1u];
    enum command_status stored_status;
    uint64_t stored_anchor_id;
    size_t stored_entry_count;

    for (size_t i = 0u; i < sizeof(entries) / sizeof(entries[0]); i++) {
        entries[i].peer_id = 0x1111000000000001ull + i + 1u;
        entries[i].rssi_dbm = -70;
        entries[i].quality = 80u;
    }

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDEu,
                                            0x1111000000000001ull,
                                            entries,
                                            1u) == PROTO_ERR_STALE);
    assert(survey_gateway_note_reach_report(
               &context,
               0xAABBCCDDu,
               0x1111000000000001ull,
               entries,
               SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) == PROTO_OK);
    assert(context.report_count == 1u);
    assert(survey_gateway_report_info_at(
               &context,
               0u,
               &stored_anchor_id,
               &stored_entry_count,
               &stored_status) == PROTO_OK);
    assert(stored_entry_count == SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x9999000000000009ull,
                                            entries,
                                            sizeof(entries) / sizeof(entries[0])) == PROTO_ERR_NO_SPACE);
    entries[0].peer_id = 0x1111000000000001ull;
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x1111000000000001ull,
                                            entries,
                                            1u) == PROTO_ERR_MALFORMED);
}

static void test_gateway_context_retains_only_accepted_reverse_hints(void)
{
    struct survey_gateway_context context;
    struct survey_gateway_reverse_hint hint = {
        .target_id = 0x1111000000000001ull,
        .next_hop_id = 0x2222000000000002ull,
        .quality = 83u,
        .hop_count = 2u,
        .valid = true,
    };
    struct survey_gateway_reverse_hint stored = {0};

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDEu,
               hint.target_id,
               NULL,
               0u,
               &hint) == PROTO_ERR_STALE);
    assert(context.report_count == 0u);
    assert(survey_gateway_reverse_hint_for_target(&context,
                                                  hint.target_id,
                                                  &stored) ==
           PROTO_ERR_NOT_FOUND);

    hint.target_id++;
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               0x1111000000000001ull,
               NULL,
               0u,
               &hint) == PROTO_ERR_MALFORMED);
    assert(context.report_count == 0u);

    hint.target_id = 0x1111000000000001ull;
    hint.quality = 101u;
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               hint.target_id,
               NULL,
               0u,
               &hint) == PROTO_ERR_MALFORMED);
    assert(context.report_count == 0u);

    hint.quality = 83u;
    hint.hop_count = SURVEY_DEFAULT_TTL + 1u;
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               hint.target_id,
               NULL,
               0u,
               &hint) == PROTO_ERR_MALFORMED);
    assert(context.report_count == 0u);

    hint.hop_count = 2u;
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context,
               0xAABBCCDDu,
               hint.target_id,
               NULL,
               0u,
               &hint) == PROTO_OK);
    assert(survey_gateway_reverse_hint_for_target(&context,
                                                  hint.target_id,
                                                  &stored) == PROTO_OK);
    assert(stored.valid);
    assert(stored.target_id == hint.target_id);
    assert(stored.next_hop_id == hint.next_hop_id);
    assert(stored.quality == hint.quality);
    assert(stored.hop_count == hint.hop_count);

    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            hint.target_id,
                                            NULL,
                                            0u) == PROTO_OK);
    assert(survey_gateway_reverse_hint_for_target(&context,
                                                  hint.target_id,
                                                  &stored) == PROTO_OK);
    assert(stored.next_hop_id == hint.next_hop_id);
    assert(stored.quality == hint.quality);
    assert(stored.hop_count == hint.hop_count);
}

static void test_gateway_control_timeout_tracks_accepted_route_depth(void)
{
    assert(NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS == 10000u);
    assert(SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS == 1000u);
    assert(SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS <
           NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS);
    assert(SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS == 30000u);
    assert(SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS == 15000u);
    assert(SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS == 90000u);

    assert(survey_gateway_hop_count_from_report_ttl(SURVEY_DEFAULT_TTL) == 1u);
    assert(survey_gateway_hop_count_from_report_ttl(
               SURVEY_DEFAULT_TTL - 1u) == 2u);
    assert(survey_gateway_hop_count_from_report_ttl(1u) ==
           SURVEY_DEFAULT_TTL);
    assert(survey_gateway_hop_count_from_report_ttl(0u) == 0u);
    assert(survey_gateway_hop_count_from_report_ttl(
               SURVEY_DEFAULT_TTL + 1u) == 0u);

    assert(survey_pair_control_timeout_ms(1u) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS);
    assert(survey_pair_control_timeout_ms(2u) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
               SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS);
    assert(survey_pair_control_timeout_ms(SURVEY_DEFAULT_TTL) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
               ((SURVEY_DEFAULT_TTL - 1u) *
                SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS));
    assert(survey_pair_control_timeout_ms(0u) ==
           SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_timeout_ms(SURVEY_DEFAULT_TTL + 1u) ==
           SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_timeout_ms(1u) == 30000u);
    assert(survey_pair_control_timeout_ms(2u) == 45000u);
    assert(survey_pair_control_timeout_ms(3u) == 60000u);
    assert(survey_pair_control_timeout_ms(SURVEY_DEFAULT_TTL) == 75000u);
    assert(4u * SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS == 4000u);

    assert(survey_pair_control_round_trip_timeout_ms(1u) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
               SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_round_trip_timeout_ms(2u) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
               SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS +
               SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_round_trip_timeout_ms(SURVEY_DEFAULT_TTL) ==
           SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
               ((SURVEY_DEFAULT_TTL - 1u) *
                SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS) +
               SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_round_trip_timeout_ms(0u) ==
           2u * SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    assert(survey_pair_control_round_trip_timeout_ms(
               SURVEY_DEFAULT_TTL + 1u) ==
           2u * SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
}

static void test_gateway_context_retains_fifty_reverse_hints(void)
{
    struct survey_gateway_context context;
    const uint64_t anchor_base = UINT64_C(0xa700000000000100);

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 1u) == PROTO_OK);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        const uint64_t target_id = anchor_base + i;
        const struct survey_gateway_reverse_hint hint = {
            .target_id = target_id,
            .next_hop_id = i < 20u ? target_id : anchor_base + (i % 5u),
            .quality = (uint8_t)(100u - i),
            .valid = true,
        };

        assert(survey_gateway_note_reach_report_with_reverse_hint(
                   &context,
                   0xAABBCCDDu,
                   target_id,
                   NULL,
                   0u,
                   &hint) == PROTO_OK);
    }
    assert(context.report_count == SURVEY_GATEWAY_MAX_REPORTS);

    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        struct survey_gateway_reverse_hint stored = {0};
        const uint64_t target_id = anchor_base + i;

        assert(survey_gateway_reverse_hint_for_target(&context,
                                                      target_id,
                                                      &stored) == PROTO_OK);
        assert(stored.target_id == target_id);
        assert(stored.next_hop_id ==
               (i < 20u ? target_id : anchor_base + (i % 5u)));
    }
}

static void test_gateway_compact_context_round_trips_worst_case(void)
{
    const uint64_t node_base = UINT64_C(0xc700000000000100);
    struct survey_reachability_entry
        entries[SURVEY_GATEWAY_MAX_REPORTS]
               [SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct survey_gateway_context context;
    struct survey_pair_round_metadata
        metadata[SURVEY_GATEWAY_MAX_PAIRS];
    size_t round_count = 0u;

    _Static_assert(sizeof(struct survey_gateway_context) == 2720u,
                   "worst-case gateway survey storage must recover 7312 bytes");
    assert(survey_gateway_begin_operation(
               &context,
               UINT32_C(0x89abcdef),
               UINT64_C(0x0123456789abcdef),
               4u) == PROTO_OK);
    for (size_t anchor = 0u;
         anchor < SURVEY_GATEWAY_MAX_REPORTS;
         anchor++) {
        const uint64_t anchor_id = node_base + anchor;
        const struct survey_gateway_reverse_hint reverse_hint = {
            .target_id = anchor_id,
            .next_hop_id =
                node_base + ((anchor + 7u) % SURVEY_GATEWAY_MAX_REPORTS),
            .quality = (uint8_t)(100u - anchor),
            .hop_count = (uint8_t)((anchor % SURVEY_DEFAULT_TTL) + 1u),
            .valid = true,
        };
        const enum command_status status =
            (enum command_status)(anchor % (COMMAND_INTERNAL_ERROR + 1u));

        for (size_t peer = 0u;
             peer < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT;
             peer++) {
            entries[anchor][peer] = (struct survey_reachability_entry) {
                .peer_id =
                    node_base +
                    ((anchor + peer + 1u) %
                     SURVEY_GATEWAY_MAX_REPORTS),
                .rssi_dbm = (int8_t)(-30 - (int)peer),
                .quality = (uint8_t)(100u - peer),
            };
        }
        assert(survey_gateway_note_reach_report_with_reverse_hint_status(
                   &context,
                   context.survey_id,
                   anchor_id,
                   entries[anchor],
                   SURVEY_GATEWAY_MAX_PEERS_PER_REPORT,
                   status,
                   &reverse_hint) == PROTO_OK);
    }
    assert(context.node_count == SURVEY_GATEWAY_MAX_REPORTS);
    assert(context.report_count == SURVEY_GATEWAY_MAX_REPORTS);

    for (size_t anchor = 0u;
         anchor < SURVEY_GATEWAY_MAX_REPORTS;
         anchor++) {
        const uint64_t expected_anchor_id = node_base + anchor;
        const enum command_status expected_status =
            (enum command_status)(anchor % (COMMAND_INTERNAL_ERROR + 1u));
        struct survey_gateway_reverse_hint reverse_hint;
        enum command_status report_status;
        uint64_t anchor_id;
        size_t entry_count;

        assert(survey_gateway_report_info_at(
                   &context,
                   anchor,
                   &anchor_id,
                   &entry_count,
                   &report_status) == PROTO_OK);
        assert(anchor_id == expected_anchor_id);
        assert(entry_count == SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
        assert(report_status == expected_status);
        for (size_t peer = 0u; peer < entry_count; peer++) {
            struct survey_reachability_entry decoded;

            assert(survey_gateway_report_entry_at(
                       &context, anchor, peer, &decoded) == PROTO_OK);
            assert(decoded.peer_id == entries[anchor][peer].peer_id);
            assert(decoded.rssi_dbm == entries[anchor][peer].rssi_dbm);
            assert(decoded.quality == entries[anchor][peer].quality);
        }
        assert(survey_gateway_report_entry_at(
                   &context,
                   anchor,
                   entry_count,
                   &entries[0][0]) == PROTO_ERR_NOT_FOUND);
        assert(survey_gateway_reach_report_compare(
                   &context,
                   expected_anchor_id,
                   entries[anchor],
                   entry_count,
                   expected_status) == PROTO_OK);
        assert(survey_gateway_reverse_hint_for_target(
                   &context,
                   expected_anchor_id,
                   &reverse_hint) == PROTO_OK);
        assert(reverse_hint.next_hop_id ==
               node_base +
               ((anchor + 7u) % SURVEY_GATEWAY_MAX_REPORTS));
        assert(reverse_hint.quality == (uint8_t)(100u - anchor));
        assert(reverse_hint.hop_count ==
               (uint8_t)((anchor % SURVEY_DEFAULT_TTL) + 1u));
    }
    {
        enum command_status report_status;
        uint64_t anchor_id;
        size_t entry_count;

        assert(survey_gateway_report_info_at(
                   &context,
                   SURVEY_GATEWAY_MAX_REPORTS,
                   &anchor_id,
                   &entry_count,
                   &report_status) == PROTO_ERR_NOT_FOUND);
    }

    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == SURVEY_GATEWAY_MAX_PAIRS);
    for (size_t i = 0u; i < context.pair_count; i++) {
        struct survey_pair pair;

        assert(survey_gateway_pair_at(&context, i, &pair) == PROTO_OK);
        assert(pair.operation_generation ==
               UINT64_C(0x0123456789abcdef));
        assert(pair.survey_id == context.survey_id);
        assert(pair.initiator_id >= node_base);
        assert(pair.initiator_id <
               node_base + SURVEY_GATEWAY_MAX_REPORTS);
        assert(pair.responder_id >= node_base);
        assert(pair.responder_id <
               node_base + SURVEY_GATEWAY_MAX_REPORTS);
        assert(pair.initiator_id != pair.responder_id);
        assert(pair.sample_count == context.sample_count);
    }
    assert(survey_gateway_plan_pair_rounds(
               &context,
               metadata,
               SURVEY_GATEWAY_MAX_PAIRS,
               &round_count) == PROTO_OK);
    assert(round_count > 0u && round_count <= context.pair_count);
}

static void test_gateway_compact_context_capacity_is_transactional(void)
{
    const uint64_t node_base = UINT64_C(0xc800000000000100);
    struct survey_reachability_entry entries[4][12];
    struct survey_gateway_context context;
    struct survey_gateway_context before;
    size_t next_node = 0u;

    assert(survey_gateway_begin(&context, UINT32_C(0x76543210), 1u) ==
           PROTO_OK);
    for (size_t report = 0u; report < 4u; report++) {
        const uint64_t anchor_id = node_base + next_node++;
        size_t entry_count = 0u;

        while (next_node < SURVEY_GATEWAY_MAX_REPORTS &&
               entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
            entries[report][entry_count++] =
                (struct survey_reachability_entry) {
                    .peer_id = node_base + next_node++,
                    .rssi_dbm = -70,
                    .quality = 80u,
                };
        }
        assert(survey_gateway_note_reach_report(
                   &context,
                   context.survey_id,
                   anchor_id,
                   entries[report],
                   entry_count) == PROTO_OK);
    }
    assert(next_node == SURVEY_GATEWAY_MAX_REPORTS);
    assert(context.node_count == SURVEY_GATEWAY_MAX_REPORTS);
    assert(context.report_count == 4u);

    before = context;
    assert(survey_gateway_note_reach_report_with_reverse_hint_status(
               &context,
               context.survey_id,
               node_base,
               entries[0],
               SURVEY_GATEWAY_MAX_PEERS_PER_REPORT,
               (enum command_status)-1,
               NULL) == PROTO_ERR_MALFORMED);
    assert(memcmp(&context, &before, sizeof(context)) == 0);

    assert(survey_gateway_note_reach_report(
               &context,
               context.survey_id,
               node_base + SURVEY_GATEWAY_MAX_REPORTS,
               NULL,
               0u) == PROTO_ERR_NO_SPACE);
    assert(memcmp(&context, &before, sizeof(context)) == 0);

    {
        const struct survey_gateway_reverse_hint unseen_duplicate_hint = {
            .target_id = node_base,
            .next_hop_id = node_base + SURVEY_GATEWAY_MAX_REPORTS,
            .quality = 50u,
            .hop_count = 2u,
            .valid = true,
        };

        assert(survey_gateway_note_reach_report_with_reverse_hint(
                   &context,
                   context.survey_id,
                   node_base,
                   entries[0],
                   SURVEY_GATEWAY_MAX_PEERS_PER_REPORT,
                   &unseen_duplicate_hint) == PROTO_OK);
        assert(memcmp(&context, &before, sizeof(context)) == 0);
    }
}

static void test_gateway_compact_context_rejects_corrupt_indices(void)
{
    const uint64_t first_id = UINT64_C(0xc900000000000001);
    const uint64_t second_id = UINT64_C(0xc900000000000002);
    const struct survey_reachability_entry first_entries[] = {
        {.peer_id = second_id, .rssi_dbm = -60, .quality = 90u},
    };
    const struct survey_reachability_entry second_entries[] = {
        {.peer_id = first_id, .rssi_dbm = -61, .quality = 89u},
    };
    struct survey_gateway_reverse_hint first_hint = {
        .target_id = first_id,
        .next_hop_id = first_id,
        .quality = 100u,
        .hop_count = 1u,
        .valid = true,
    };
    struct survey_gateway_reverse_hint second_hint = {
        .target_id = second_id,
        .next_hop_id = second_id,
        .quality = 99u,
        .hop_count = 1u,
        .valid = true,
    };
    struct survey_gateway_context valid;
    struct survey_gateway_context corrupt;
    struct survey_pair pair = {
        .operation_generation = UINT64_C(0xaaaaaaaaaaaaaaaa),
        .survey_id = UINT32_C(0xbbbbbbbb),
        .initiator_id = UINT64_C(0xcccccccccccccccc),
        .responder_id = UINT64_C(0xdddddddddddddddd),
        .sample_count = UINT16_C(0xeeee),
    };
    const struct survey_pair pair_sentinel = pair;
    struct survey_pair_round_metadata metadata;
    struct survey_reachability_entry entry;
    size_t round_count = 0u;

    assert(survey_gateway_begin(&valid, UINT32_C(0x11223344), 1u) ==
           PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &valid,
               valid.survey_id,
               first_id,
               first_entries,
               1u,
               &first_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &valid,
               valid.survey_id,
               second_id,
               second_entries,
               1u,
               &second_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&valid) == PROTO_OK);
    assert(valid.pair_count == 1u);

    corrupt = valid;
    corrupt.reports[0].entries[0].peer_index =
        (uint8_t)((corrupt.reports[0].entries[0].peer_index & 0xc0u) |
                  SURVEY_GATEWAY_MAX_REPORTS);
    assert(survey_gateway_report_entry_at(
               &corrupt, 0u, 0u, &entry) == PROTO_ERR_MALFORMED);
    assert(survey_gateway_plan_pairs(&corrupt) == PROTO_ERR_MALFORMED);
    assert(!corrupt.pairs_planned);
    assert(corrupt.pair_count == 0u);

    corrupt = valid;
    corrupt.reports[0].reverse_next_hop_index =
        SURVEY_GATEWAY_MAX_REPORTS;
    assert(survey_gateway_reverse_hint_for_target(
               &corrupt, first_id, &first_hint) == PROTO_ERR_MALFORMED);
    assert(survey_gateway_plan_pair_rounds(
               &corrupt, &metadata, 1u, &round_count) ==
           PROTO_ERR_MALFORMED);

    corrupt = valid;
    corrupt.pairs[0].initiator_index = SURVEY_GATEWAY_MAX_REPORTS;
    assert(survey_gateway_pair_at(&corrupt, 0u, &pair) ==
           PROTO_ERR_MALFORMED);
    assert(survey_gateway_plan_pair_rounds(
               &corrupt, &metadata, 1u, &round_count) ==
           PROTO_ERR_MALFORMED);

    corrupt = valid;
    for (size_t part = 0u; part < 3u; part++) {
        corrupt.reports[1].entries[part].peer_index &=
            0x3fu;
    }
    assert(survey_gateway_report_entry_at(
               &corrupt, 0u, 0u, &entry) == PROTO_ERR_MALFORMED);

    corrupt = valid;
    corrupt.node_ids[1] = corrupt.node_ids[0];
    assert(survey_gateway_pair_at(&corrupt, 0u, &pair) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&pair, &pair_sentinel, sizeof(pair)) == 0);
    assert(survey_gateway_plan_pairs(&corrupt) == PROTO_ERR_MALFORMED);

    corrupt = valid;
    corrupt.sample_count = 0u;
    assert(survey_gateway_pair_at(&corrupt, 0u, &pair) ==
           PROTO_ERR_MALFORMED);
    assert(memcmp(&pair, &pair_sentinel, sizeof(pair)) == 0);
}

static void test_pair_planner_caps_degree_and_is_report_order_independent(void)
{
    enum { ANCHOR_COUNT = 10 };
    struct survey_reachability_entry entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct survey_reachability_report forward[ANCHOR_COUNT];
    struct survey_reachability_report reverse[ANCHOR_COUNT];
    struct survey_pair pairs_forward[SURVEY_GATEWAY_MAX_PAIRS] = {0};
    struct survey_pair pairs_reverse[SURVEY_GATEWAY_MAX_PAIRS] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t forward_count = 0u;
    size_t reverse_count = 0u;

    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        size_t entry_count = 0u;

        for (size_t j = 0u; j < ANCHOR_COUNT &&
             entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT; j++) {
            if (i == j) {
                continue;
            }
            entries[i][entry_count].peer_id = 0x1000u + j;
            entries[i][entry_count].rssi_dbm = -60;
            entries[i][entry_count].quality = 80u;
            entry_count++;
        }
        forward[i].anchor_id = 0x1000u + i;
        forward[i].entries = entries[i];
        forward[i].entry_count = entry_count;
        reverse[ANCHOR_COUNT - i - 1u] = forward[i];
    }

    assert(survey_plan_pairs_from_reachability(0x1234u,
                                               forward,
                                               ANCHOR_COUNT,
                                               1u,
                                               pairs_forward,
                                               SURVEY_GATEWAY_MAX_PAIRS,
                                               &forward_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(0x1234u,
                                               reverse,
                                               ANCHOR_COUNT,
                                               1u,
                                               pairs_reverse,
                                               SURVEY_GATEWAY_MAX_PAIRS,
                                               &reverse_count) == PROTO_OK);
    assert(forward_count == reverse_count);
    assert(forward_count <=
           (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u);
    assert(memcmp(pairs_forward,
                  pairs_reverse,
                  forward_count * sizeof(pairs_forward[0])) == 0);

    for (size_t i = 0u; i < forward_count; i++) {
        size_t initiator = (size_t)(pairs_forward[i].initiator_id - 0x1000u);
        size_t responder = (size_t)(pairs_forward[i].responder_id - 0x1000u);

        assert(initiator < ANCHOR_COUNT);
        assert(responder < ANCHOR_COUNT);
        degree[initiator]++;
        degree[responder]++;
    }
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        assert(degree[i] <= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
}

static void test_pair_planner_augments_cardinality_without_disconnect(void)
{
    enum {
        ANCHOR_COUNT = 14,
        LEFT_COUNT = 7,
        PAIR_CAP = (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2,
    };
    static const uint8_t peer_indices[LEFT_COUNT][LEFT_COUNT] = {
        {7u, 8u, 9u, 10u, 11u, 12u, 13u},
        {7u, 8u, 9u, 10u, 11u, 12u, 13u},
        {7u, 8u, 10u, 11u, 12u, 13u},
        {7u, 8u, 9u, 10u, 12u, 13u},
        {7u, 8u, 9u, 10u, 12u, 13u},
        {7u, 8u, 9u, 11u, 12u, 13u},
        {8u, 9u, 10u, 11u, 12u, 13u},
    };
    static const uint8_t peer_counts[LEFT_COUNT] = {
        7u, 7u, 6u, 6u, 6u, 6u, 6u,
    };
    struct survey_reachability_entry entries[ANCHOR_COUNT][LEFT_COUNT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[PAIR_CAP] = {{0}};
    struct survey_pair reversed_pairs[PAIR_CAP] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {true};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    for (size_t left = 0u; left < LEFT_COUNT; left++) {
        for (size_t peer = 0u; peer < peer_counts[left]; peer++) {
            const size_t right = peer_indices[left][peer];

            candidate[left][right] = true;
            candidate[right][left] = true;
            entries[left][entry_counts[left]++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0x1000) + right,
                    .rssi_dbm = -60,
                    .quality = 50u,
                };
            entries[right][entry_counts[right]++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0x1000) + left,
                    .rssi_dbm = -60,
                    .quality = 50u,
                };
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x1000) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs, PAIR_CAP,
               &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs, PAIR_CAP,
               &reversed_pair_count) == PROTO_OK);
    assert(pair_count == 41u);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x1000));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x1000));

        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] <= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(reachable[anchor]);
    }
}

static void test_pair_planner_augments_two_edge_alternating_trail(void)
{
    enum {
        ANCHOR_COUNT = 9,
        EDGE_COUNT = 31,
        PAIR_CAP = (ANCHOR_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2,
    };
    static const uint8_t edge_vertices[EDGE_COUNT][2] = {
        {0u, 2u}, {0u, 3u}, {0u, 4u}, {0u, 5u}, {0u, 6u}, {0u, 7u},
        {0u, 8u}, {1u, 3u}, {1u, 4u}, {1u, 5u}, {1u, 6u}, {1u, 7u},
        {1u, 8u}, {2u, 3u}, {2u, 4u}, {2u, 5u}, {2u, 6u}, {2u, 7u},
        {2u, 8u}, {3u, 4u}, {3u, 5u}, {3u, 7u}, {3u, 8u}, {4u, 5u},
        {4u, 6u}, {4u, 7u}, {4u, 8u}, {5u, 6u}, {5u, 7u}, {5u, 8u},
        {6u, 7u},
    };
    struct survey_reachability_entry entries[ANCHOR_COUNT][ANCHOR_COUNT - 1u] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[PAIR_CAP] = {{0}};
    struct survey_pair reversed_pairs[PAIR_CAP] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {true};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    for (size_t i = 0u; i < EDGE_COUNT; i++) {
        const size_t first = edge_vertices[i][0];
        const size_t second = edge_vertices[i][1];

        candidate[first][second] = true;
        candidate[second][first] = true;
        entries[first][entry_counts[first]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x2000) + second,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        entries[second][entry_counts[second]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x2000) + first,
                .rssi_dbm = -60,
                .quality = 50u,
            };
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x2000) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs, PAIR_CAP,
               &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs, PAIR_CAP,
               &reversed_pair_count) == PROTO_OK);
    assert(pair_count == PAIR_CAP);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x2000));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x2000));

        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(reachable[anchor]);
    }
}

static void test_pair_planner_augments_repeated_vertex_trail(void)
{
    enum {
        ANCHOR_COUNT = 12,
        EDGE_COUNT = 40,
        EXPECTED_PAIR_COUNT = 34,
    };
    static const uint8_t edge_vertices[EDGE_COUNT][2] = {
        {0u, 1u}, {0u, 2u}, {0u, 3u}, {0u, 4u}, {0u, 6u}, {0u, 8u},
        {1u, 2u}, {1u, 3u}, {1u, 5u}, {1u, 8u}, {1u, 9u}, {1u, 10u},
        {1u, 11u}, {2u, 3u}, {2u, 4u}, {2u, 6u}, {2u, 7u},
        {3u, 4u}, {3u, 5u}, {3u, 10u}, {3u, 11u}, {4u, 5u},
        {4u, 6u}, {4u, 7u}, {4u, 8u}, {4u, 9u}, {4u, 10u},
        {4u, 11u}, {5u, 8u}, {6u, 7u}, {6u, 9u}, {6u, 10u},
        {7u, 8u}, {7u, 10u}, {7u, 11u}, {8u, 9u}, {8u, 10u},
        {8u, 11u}, {9u, 10u}, {10u, 11u},
    };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    struct survey_pair reversed_pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {true};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    for (size_t i = 0u; i < EDGE_COUNT; i++) {
        const size_t first = edge_vertices[i][0];
        const size_t second = edge_vertices[i][1];

        entries[first][entry_counts[first]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x2800) + second,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        entries[second][entry_counts[second]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x2800) + first,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        candidate[first][second] = true;
        candidate[second][first] = true;
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x2800) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &reversed_pair_count) == PROTO_OK);
    assert(pair_count == EXPECTED_PAIR_COUNT);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x2800));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x2800));

        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] <=
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(reachable[anchor]);
    }
}

static void test_pair_planner_repeats_augmentation_within_one_component(void)
{
    enum {
        COMPONENT_ANCHOR_COUNT = 9,
        COMPONENT_EDGE_COUNT = 31,
        ANCHOR_COUNT = 2 * COMPONENT_ANCHOR_COUNT,
        EXPECTED_PAIR_COUNT =
            (ANCHOR_COUNT *
             SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2,
    };
    static const uint8_t component_edges[COMPONENT_EDGE_COUNT][2] = {
        {0u, 2u}, {0u, 3u}, {0u, 4u}, {0u, 5u}, {0u, 6u}, {0u, 7u},
        {0u, 8u}, {1u, 3u}, {1u, 4u}, {1u, 5u}, {1u, 6u}, {1u, 7u},
        {1u, 8u}, {2u, 3u}, {2u, 4u}, {2u, 5u}, {2u, 6u}, {2u, 7u},
        {2u, 8u}, {3u, 4u}, {3u, 5u}, {3u, 7u}, {3u, 8u}, {4u, 5u},
        {4u, 6u}, {4u, 7u}, {4u, 8u}, {5u, 6u}, {5u, 7u}, {5u, 8u},
        {6u, 7u},
    };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    struct survey_pair reversed_pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {true};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    for (size_t copy = 0u; copy < 2u; copy++) {
        const size_t offset = copy * COMPONENT_ANCHOR_COUNT;

        for (size_t edge = 0u; edge < COMPONENT_EDGE_COUNT; edge++) {
            const size_t first = offset + component_edges[edge][0];
            const size_t second = offset + component_edges[edge][1];

            entries[first][entry_counts[first]++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0x2c00) + second,
                    .rssi_dbm = -60,
                    .quality = 50u,
                };
            entries[second][entry_counts[second]++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0x2c00) + first,
                    .rssi_dbm = -60,
                    .quality = 50u,
                };
            candidate[first][second] = true;
            candidate[second][first] = true;
        }
    }

    entries[0][entry_counts[0]++] =
        (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0x2c00) + COMPONENT_ANCHOR_COUNT,
            .rssi_dbm = -100,
            .quality = 1u,
        };
    entries[COMPONENT_ANCHOR_COUNT]
           [entry_counts[COMPONENT_ANCHOR_COUNT]++] =
        (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0x2c00),
            .rssi_dbm = -100,
            .quality = 1u,
        };
    candidate[0][COMPONENT_ANCHOR_COUNT] = true;
    candidate[COMPONENT_ANCHOR_COUNT][0] = true;
    entries[1][entry_counts[1]++] =
        (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0x2c00) + COMPONENT_ANCHOR_COUNT + 1u,
            .rssi_dbm = -100,
            .quality = 1u,
        };
    entries[COMPONENT_ANCHOR_COUNT + 1u]
           [entry_counts[COMPONENT_ANCHOR_COUNT + 1u]++] =
        (struct survey_reachability_entry) {
            .peer_id = UINT64_C(0x2c00) + 1u,
            .rssi_dbm = -100,
            .quality = 1u,
        };
    candidate[1][COMPONENT_ANCHOR_COUNT + 1u] = true;
    candidate[COMPONENT_ANCHOR_COUNT + 1u][1] = true;

    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x2c00) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &reversed_pair_count) == PROTO_OK);
    assert(pair_count == EXPECTED_PAIR_COUNT);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x2c00));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x2c00));

        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] ==
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(reachable[anchor]);
    }
}

static void test_pair_planner_repairs_connected_component_after_forest_stall(void)
{
    enum {
        ANCHOR_COUNT = 11,
        EDGE_COUNT = 11,
        EXPECTED_PAIR_COUNT = 10,
    };
    static const uint8_t edge_vertices[EDGE_COUNT][2] = {
        {0u, 2u}, {0u, 3u}, {0u, 4u}, {0u, 5u}, {0u, 6u}, {0u, 9u},
        {0u, 10u}, {1u, 9u}, {2u, 3u}, {4u, 7u}, {8u, 9u},
    };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    struct survey_pair reversed_pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {true};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    for (size_t edge = 0u; edge < EDGE_COUNT; edge++) {
        const size_t first = edge_vertices[edge][0];
        const size_t second = edge_vertices[edge][1];

        entries[first][entry_counts[first]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x3800) + second,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        entries[second][entry_counts[second]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x3800) + first,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        candidate[first][second] = true;
        candidate[second][first] = true;
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x3800) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &reversed_pair_count) == PROTO_OK);
    /*
     * Selecting all eleven candidates would give anchor zero degree seven,
     * so ten is the exact degree-bounded maximum.
     */
    assert(pair_count == EXPECTED_PAIR_COUNT);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x3800));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x3800));

        assert(pairs[i].survey_id == 0x1234u);
        assert(pairs[i].sample_count == 1u);
        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    assert(selected[0][9]);
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] <=
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(reachable[anchor]);
    }
}

static void test_pair_planner_component_budget_fairness(void)
{
    enum {
        SATURATED_SIDE_COUNT = SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
        SATURATED_PEER_COUNT = 35,
        SATURATED_COMPONENT_COUNT =
            SATURATED_SIDE_COUNT + SATURATED_PEER_COUNT,
        WITNESS_COUNT = 9,
        WITNESS_EDGE_COUNT = 31,
        ANCHOR_COUNT = SATURATED_COMPONENT_COUNT + WITNESS_COUNT,
        EXPECTED_SATURATED_PAIRS =
            SATURATED_SIDE_COUNT *
            SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
        EXPECTED_WITNESS_PAIRS =
            (WITNESS_COUNT *
             SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2,
        EXPECTED_PAIR_COUNT =
            EXPECTED_SATURATED_PAIRS + EXPECTED_WITNESS_PAIRS,
    };
    static const uint8_t witness_edges[WITNESS_EDGE_COUNT][2] = {
        {0u, 2u}, {0u, 3u}, {0u, 4u}, {0u, 5u}, {0u, 6u}, {0u, 7u},
        {0u, 8u}, {1u, 3u}, {1u, 4u}, {1u, 5u}, {1u, 6u}, {1u, 7u},
        {1u, 8u}, {2u, 3u}, {2u, 4u}, {2u, 5u}, {2u, 6u}, {2u, 7u},
        {2u, 8u}, {3u, 4u}, {3u, 5u}, {3u, 7u}, {3u, 8u}, {4u, 5u},
        {4u, 6u}, {4u, 7u}, {4u, 8u}, {5u, 6u}, {5u, 7u}, {5u, 8u},
        {6u, 7u},
    };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PEERS_PER_REPORT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_reachability_report reversed[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    struct survey_pair reversed_pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    bool candidate[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    bool reachable[ANCHOR_COUNT] = {false};
    uint8_t entry_counts[ANCHOR_COUNT] = {0};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t saturated_pair_count = 0u;
    size_t witness_pair_count = 0u;
    size_t pair_count = 0u;
    size_t reversed_pair_count = 0u;

    _Static_assert(ANCHOR_COUNT == SURVEY_GATEWAY_MAX_REPORTS,
                   "budget fairness witness must exercise the K50 bound");
    _Static_assert(EXPECTED_PAIR_COUNT == 63u,
                   "budget fairness witness cardinality changed");

    for (size_t anchor = SATURATED_SIDE_COUNT;
         anchor < SATURATED_COMPONENT_COUNT;
         anchor++) {
        for (size_t peer = 0u; peer < SATURATED_SIDE_COUNT; peer++) {
            entries[anchor][entry_counts[anchor]++] =
                (struct survey_reachability_entry) {
                    .peer_id = UINT64_C(0x4000) + peer,
                    .rssi_dbm = -60,
                    .quality = 50u,
                };
            candidate[anchor][peer] = true;
            candidate[peer][anchor] = true;
        }
    }
    for (size_t edge = 0u; edge < WITNESS_EDGE_COUNT; edge++) {
        const size_t first =
            SATURATED_COMPONENT_COUNT + witness_edges[edge][0];
        const size_t second =
            SATURATED_COMPONENT_COUNT + witness_edges[edge][1];

        entries[first][entry_counts[first]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x4000) + second,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        entries[second][entry_counts[second]++] =
            (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x4000) + first,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        candidate[first][second] = true;
        candidate[second][first] = true;
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor] = (struct survey_reachability_report) {
            .anchor_id = UINT64_C(0x4000) + anchor,
            .entries = entries[anchor],
            .entry_count = entry_counts[anchor],
        };
        reversed[ANCHOR_COUNT - anchor - 1u] = reports[anchor];
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &pair_count) == PROTO_OK);
    assert(survey_plan_pairs_from_reachability(
               0x1234u, reversed, ANCHOR_COUNT, 1u, reversed_pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &reversed_pair_count) == PROTO_OK);
    assert(pair_count == EXPECTED_PAIR_COUNT);
    assert(reversed_pair_count == pair_count);
    assert(memcmp(reversed_pairs, pairs,
                  pair_count * sizeof(pairs[0])) == 0);

    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x4000));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x4000));

        assert(first < ANCHOR_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(candidate[first][second]);
        assert(!selected[first][second]);
        selected[first][second] = true;
        selected[second][first] = true;
        degree[first]++;
        degree[second]++;
        if (first < SATURATED_COMPONENT_COUNT) {
            assert(second < SATURATED_COMPONENT_COUNT);
            saturated_pair_count++;
        } else {
            assert(second >= SATURATED_COMPONENT_COUNT);
            witness_pair_count++;
        }
    }
    assert(saturated_pair_count == EXPECTED_SATURATED_PAIRS);
    assert(witness_pair_count == EXPECTED_WITNESS_PAIRS);

    reachable[SATURATED_COMPONENT_COUNT] = true;
    for (size_t pass = 0u; pass < ANCHOR_COUNT; pass++) {
        for (size_t first = 0u; first < ANCHOR_COUNT; first++) {
            if (!reachable[first]) {
                continue;
            }
            for (size_t second = 0u; second < ANCHOR_COUNT; second++) {
                if (selected[first][second]) {
                    reachable[second] = true;
                }
            }
        }
    }
    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        assert(degree[anchor] <=
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        assert(degree[anchor] > 0u);
        if (anchor >= SATURATED_COMPONENT_COUNT) {
            assert(reachable[anchor]);
        }
    }
    for (size_t anchor = 0u;
         anchor < SATURATED_SIDE_COUNT;
         anchor++) {
        assert(degree[anchor] ==
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
    for (size_t anchor = SATURATED_COMPONENT_COUNT;
         anchor < ANCHOR_COUNT;
         anchor++) {
        assert(degree[anchor] ==
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
}

static void test_pair_planner_reaches_six_degree_ceiling_for_50_anchors(void)
{
    enum { ANCHOR_COUNT = SURVEY_GATEWAY_MAX_REPORTS };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR];
    struct survey_reachability_report reports[ANCHOR_COUNT];
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {0};
    struct survey_gateway_context context;
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;

    _Static_assert(sizeof(context.pairs[0]) == 2u,
                   "gateway context pairs must retain two exact indices");

    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        size_t entry_index = 0u;

        for (size_t distance = 1u;
             distance <= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR / 2u;
             distance++) {
            const size_t forward = (i + distance) % ANCHOR_COUNT;
            const size_t reverse = (i + ANCHOR_COUNT - distance) % ANCHOR_COUNT;

            entries[i][entry_index++] = (struct survey_reachability_entry) {
                .peer_id = 0x1000u + forward,
                .rssi_dbm = -60,
                .quality = 80u,
            };
            entries[i][entry_index++] = (struct survey_reachability_entry) {
                .peer_id = 0x1000u + reverse,
                .rssi_dbm = -60,
                .quality = 80u,
            };
        }
        reports[i].anchor_id = 0x1000u + i;
        reports[i].entries = entries[i];
        reports[i].entry_count = entry_index;
    }

    assert(survey_gateway_begin(&context, 0x1234u, 1u) == PROTO_OK);
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        assert(survey_gateway_note_reach_report(&context,
                                                0x1234u,
                                                reports[i].anchor_id,
                                                reports[i].entries,
                                                reports[i].entry_count) ==
               PROTO_OK);
    }

    assert(survey_plan_pairs_from_reachability(0x1234u,
                                               reports,
                                               ANCHOR_COUNT,
                                               1u,
                                               pairs,
                                               SURVEY_GATEWAY_MAX_PAIRS,
                                               &pair_count) == PROTO_OK);
    assert(pair_count == SURVEY_GATEWAY_MAX_PAIRS);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == pair_count);

    for (size_t i = 0u; i < pair_count; i++) {
        struct survey_pair context_pair = {0};
        const size_t initiator = (size_t)(pairs[i].initiator_id - 0x1000u);
        const size_t responder = (size_t)(pairs[i].responder_id - 0x1000u);

        assert(survey_gateway_pair_at(&context, i, &context_pair) == PROTO_OK);
        assert(context_pair.survey_id == pairs[i].survey_id);
        assert(context_pair.initiator_id == pairs[i].initiator_id);
        assert(context_pair.responder_id == pairs[i].responder_id);
        assert(context_pair.sample_count == pairs[i].sample_count);
        assert(initiator < ANCHOR_COUNT);
        assert(responder < ANCHOR_COUNT);
        degree[initiator]++;
        degree[responder]++;
    }
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        assert(degree[i] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
    assert(survey_gateway_pair_at(&context, pair_count, &pairs[0]) ==
           PROTO_ERR_NOT_FOUND);
    for (size_t i = 0u; i < pair_count; i++) {
        struct survey_pair next = {0};

        assert(survey_gateway_pair_at(&context, i, &next) == PROTO_OK);
        assert(next.survey_id == pairs[i].survey_id);
        assert(next.initiator_id == pairs[i].initiator_id);
        assert(next.responder_id == pairs[i].responder_id);
        assert(next.sample_count == pairs[i].sample_count);
    }
    assert(survey_gateway_pair_at(&context, pair_count, &pairs[0]) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_pair_planner_bounds_sparse_k50_augmentation_work(void)
{
    enum {
        ANCHOR_COUNT = SURVEY_GATEWAY_MAX_REPORTS,
        SATURATED_SIDE_COUNT = SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
        EXPECTED_PAIR_COUNT =
            SATURATED_SIDE_COUNT * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
    };
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SATURATED_SIDE_COUNT] = {{{0}}};
    struct survey_reachability_report reports[ANCHOR_COUNT] = {{0}};
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS] = {{0}};
    bool selected[ANCHOR_COUNT][ANCHOR_COUNT] = {{false}};
    uint8_t degree[ANCHOR_COUNT] = {0};
    size_t pair_count = 0u;

    for (size_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        reports[anchor].anchor_id = UINT64_C(0x3000) + anchor;
        if (anchor < SATURATED_SIDE_COUNT) {
            continue;
        }
        reports[anchor].entries = entries[anchor];
        reports[anchor].entry_count = SATURATED_SIDE_COUNT;
        for (size_t peer = 0u; peer < SATURATED_SIDE_COUNT; peer++) {
            entries[anchor][peer] = (struct survey_reachability_entry) {
                .peer_id = UINT64_C(0x3000) + peer,
                .rssi_dbm = -60,
                .quality = 50u,
            };
        }
    }

    assert(survey_plan_pairs_from_reachability(
               0x1234u, reports, ANCHOR_COUNT, 1u, pairs,
               SURVEY_GATEWAY_MAX_PAIRS, &pair_count) == PROTO_OK);
    assert(pair_count == EXPECTED_PAIR_COUNT);
    for (size_t i = 0u; i < pair_count; i++) {
        const size_t first =
            (size_t)(pairs[i].initiator_id - UINT64_C(0x3000));
        const size_t second =
            (size_t)(pairs[i].responder_id - UINT64_C(0x3000));

        assert(first < SATURATED_SIDE_COUNT);
        assert(second >= SATURATED_SIDE_COUNT);
        assert(second < ANCHOR_COUNT);
        assert(!selected[first][second]);
        selected[first][second] = true;
        degree[first]++;
        degree[second]++;
    }
    for (size_t anchor = 0u;
         anchor < SATURATED_SIDE_COUNT;
         anchor++) {
        assert(degree[anchor] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
    for (size_t anchor = SATURATED_SIDE_COUNT;
         anchor < ANCHOR_COUNT;
         anchor++) {
        assert(degree[anchor] <= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
}

static void test_gateway_operation_identity_keeps_host_id_and_generation_distinct(void)
{
    const uint32_t host_survey_id = UINT32_C(0x55667788);
    const uint64_t operation_generation =
        UINT64_C(0x11223344a1b2c3d4);
    const uint32_t operation_session_id =
        survey_operation_session_id(operation_generation);
    const uint64_t initiator_id = UINT64_C(0x1111000000000001);
    const uint64_t responder_id = UINT64_C(0x2222000000000002);
    const struct survey_reachability_entry entries[] = {
        {
            .peer_id = responder_id,
            .rssi_dbm = -61,
            .quality = 82u,
        },
    };
    struct survey_gateway_context context;
    struct survey_pair planned_pair = {0};

    assert(operation_session_id != host_survey_id);
    assert(survey_gateway_begin_operation(
               &context,
               host_survey_id,
               operation_generation,
               4u) == PROTO_OK);
    assert(context.survey_id == host_survey_id);
    assert(context.operation_generation == operation_generation);
    assert(survey_gateway_note_reach_report(
               &context,
               host_survey_id,
               initiator_id,
               entries,
               sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(
               &context,
               host_survey_id,
               responder_id,
               NULL,
               0u) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_pair_at(&context, 0u, &planned_pair) == PROTO_OK);
    assert(planned_pair.survey_id == host_survey_id);
    assert(planned_pair.operation_generation == operation_generation);
}

static void test_reachability_plan_rejects_invalid_graph_or_capacity(void)
{
    const struct survey_reachability_entry self_entry = {
        .peer_id = 0x1111000000000001ull,
        .rssi_dbm = -61,
        .quality = 82u,
    };
    const struct survey_reachability_entry invalid_entry = {
        .peer_id = 0x2222000000000002ull,
        .rssi_dbm = -61,
        .quality = 101u,
    };
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
    };
    const struct survey_reachability_report reports[] = {
        {
            .anchor_id = 0x1111000000000001ull,
            .entries = a_entries,
            .entry_count = sizeof(a_entries) / sizeof(a_entries[0]),
        },
        {
            .anchor_id = 0x2222000000000002ull,
        },
    };
    struct survey_reachability_report bad_reports[] = {
        {
            .anchor_id = 0x1111000000000001ull,
            .entries = &self_entry,
            .entry_count = 1u,
        },
    };
    struct survey_pair pairs[1] = {0};
    size_t pair_count = 0u;

    assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                               reports,
                                               sizeof(reports) / sizeof(reports[0]),
                                               0u,
                                               pairs,
                                               sizeof(pairs) / sizeof(pairs[0]),
                                               &pair_count) == PROTO_ERR_MALFORMED);
    assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                               reports,
                                               sizeof(reports) / sizeof(reports[0]),
                                               5u,
                                               pairs,
                                               0u,
                                               &pair_count) == PROTO_ERR_NO_SPACE);
    assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                               bad_reports,
                                               sizeof(bad_reports) / sizeof(bad_reports[0]),
                                               5u,
                                               pairs,
                                               sizeof(pairs) / sizeof(pairs[0]),
                                               &pair_count) == PROTO_ERR_MALFORMED);
    bad_reports[0].entries = &invalid_entry;
    assert(survey_plan_pairs_from_reachability(0xAABBCCDDu,
                                               bad_reports,
                                               sizeof(bad_reports) / sizeof(bad_reports[0]),
                                               5u,
                                               pairs,
                                               sizeof(pairs) / sizeof(pairs[0]),
                                               &pair_count) == PROTO_ERR_MALFORMED);

    {
        enum { STAR_REPORT_COUNT = SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR + 2u };
        struct survey_reachability_entry star_entries[STAR_REPORT_COUNT - 1u];
        struct survey_reachability_report star_reports[STAR_REPORT_COUNT] = {0};
        struct survey_pair star_pairs[STAR_REPORT_COUNT - 1u] = {0};

        star_reports[0].anchor_id = UINT64_C(0xa700000000000001);
        for (size_t i = 1u; i < STAR_REPORT_COUNT; i++) {
            star_entries[i - 1u] = (struct survey_reachability_entry) {
                .peer_id = star_reports[0].anchor_id,
                .rssi_dbm = -60,
                .quality = 80u,
            };
            star_reports[i].anchor_id = star_reports[0].anchor_id + i;
            star_reports[i].entries = &star_entries[i - 1u];
            star_reports[i].entry_count = 1u;
        }
        pair_count = 99u;
        assert(survey_plan_pairs_from_reachability(
                   0xAABBCCDDu,
                   star_reports,
                   STAR_REPORT_COUNT,
                   5u,
                   star_pairs,
                   sizeof(star_pairs) / sizeof(star_pairs[0]),
                   &pair_count) == PROTO_OK);
        assert(pair_count == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
        for (size_t i = 0u; i < pair_count; i++) {
            assert(star_pairs[i].initiator_id ==
                   star_reports[0].anchor_id);
            assert(star_pairs[i].responder_id ==
                   star_reports[0].anchor_id + i + 1u);
        }
    }
}

static void test_result_packet_is_diagnostic_not_click(void)
{
    struct proto_packet packet = {0};
    const struct survey_sample value = sample();

    assert(survey_init_result_packet(&packet, &value, 0x9999888877776666ull, 42u, 77u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_PAIR_RESULT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == value.pair.responder_id);
    assert(packet.session_id == value.pair.survey_id);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 77u);
}

static void test_result_packet_can_use_responder_as_reporter(void)
{
    struct proto_packet packet = {0};
    const struct survey_sample value = sample();

    assert(survey_init_result_packet_from_reporter(&packet,
                                                   &value,
                                                   value.pair.responder_id,
                                                   0x9999888877776666ull,
                                                   43u,
                                                   78u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_PAIR_RESULT);
    assert(packet.src_id == value.pair.responder_id);
    assert(packet.dst_id == 0x9999888877776666ull);
    assert(packet.session_id == value.pair.survey_id);
    assert(packet.seq == 43u);
    assert(packet.payload_len == 78u);
    assert(survey_init_result_packet_from_reporter(&packet,
                                                   &value,
                                                   value.pair.initiator_id,
                                                   0x9999888877776666ull,
                                                   43u,
                                                   78u) == PROTO_ERR_MALFORMED);
    assert(survey_init_result_packet_from_reporter(&packet,
                                                   &value,
                                                   0x5555000000000005ull,
                                                   0x9999888877776666ull,
                                                   43u,
                                                   78u) == PROTO_ERR_MALFORMED);
}

static void test_survey_packet_initializers_reject_zero_sequence_atomically(void)
{
    const struct survey_sample value = sample();
    const struct survey_pair pair = value.pair;
    const struct survey_discovery_config config = {
        .survey_id = value.pair.survey_id,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 4u,
        .round_count = 2u,
    };
    struct proto_packet packet;
    struct proto_packet unchanged;

#define RESET_PACKET_SENTINEL()                                           \
    do {                                                                  \
        memset(&packet, 0xa5, sizeof(packet));                             \
        unchanged = packet;                                               \
    } while (0)

    RESET_PACKET_SENTINEL();
    assert(survey_init_result_packet_from_reporter(
               &packet,
               &value,
               value.pair.responder_id,
               UINT64_C(0x9999888877776666),
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_result_packet(
               &packet,
               &value,
               UINT64_C(0x9999888877776666),
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_reach_request_packet(
               &packet,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_reach_report_packet(
               &packet,
               value.pair.initiator_id,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_discovery_start_packet(
               &packet,
               UINT64_C(0x9999888877776666),
               &config,
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_discovery_report_packet(
               &packet,
               value.pair.initiator_id,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               0u,
               1u,
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

    RESET_PACKET_SENTINEL();
    assert(survey_init_pair_prepare_packet(
               &packet,
               &pair,
               UINT64_C(0x9999888877776666),
               pair.initiator_id,
               0u,
               10u) == PROTO_ERR_MALFORMED);
    assert_packet_unchanged(&packet, &unchanged);

#undef RESET_PACKET_SENTINEL
}

static void test_survey_packet_initializers_accept_sequence_wrap_boundary(void)
{
    const struct survey_sample value = sample();
    const struct survey_pair pair = value.pair;
    const struct survey_discovery_config config = {
        .survey_id = value.pair.survey_id,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 4u,
        .round_count = 2u,
    };
    struct proto_packet packet;

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_result_packet_from_reporter(
               &packet,
               &value,
               value.pair.responder_id,
               UINT64_C(0x9999888877776666),
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_reach_request_packet(
               &packet,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.dst_id == MESH_BROADCAST_ID);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_reach_report_packet(
               &packet,
               value.pair.initiator_id,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_discovery_start_packet(
               &packet,
               UINT64_C(0x9999888877776666),
               &config,
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.dst_id == MESH_BROADCAST_ID);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_discovery_report_packet(
               &packet,
               value.pair.initiator_id,
               UINT64_C(0x9999888877776666),
               value.pair.survey_id,
               0u,
               1u,
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_init_pair_prepare_packet(
               &packet,
               &pair,
               UINT64_C(0x9999888877776666),
               pair.initiator_id,
               UINT16_MAX,
               10u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.message_age_ms == 0u);
}

static void test_discovery_report_sequence_exhaustion_is_fail_closed(void)
{
    uint16_t sequence = 0u;

    assert(survey_discovery_sequence_next(NULL) == 0u);
    assert(survey_discovery_sequence_next(&sequence) == 1u);
    assert(sequence == 1u);

    sequence = UINT16_MAX - 1u;
    assert(survey_discovery_sequence_next(&sequence) == UINT16_MAX);
    assert(sequence == UINT16_MAX);
    assert(survey_discovery_sequence_next(&sequence) == 0u);
    assert(sequence == UINT16_MAX);
    assert(survey_discovery_sequence_next(&sequence) == 0u);
    assert(sequence == UINT16_MAX);
}

static void test_sample_observation_identity_is_exact(void)
{
    struct survey_sample changed = sample();
    const struct survey_sample original = changed;
    struct survey_sample_observation_identity original_identity;
    struct survey_sample_observation_identity changed_identity;
    struct survey_sample_observation_identity invalid_identity;

    memset(&invalid_identity,
           SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID,
           sizeof(invalid_identity));
    assert(!survey_sample_observation_identity_valid(&invalid_identity));
    assert(survey_sample_observation_identity_capture(
               &original, &original_identity) == PROTO_OK);
    assert(survey_sample_observation_identity_valid(&original_identity));

    changed.distance_mm++;
    assert(survey_sample_observation_identity_capture(
               &changed, &changed_identity) == PROTO_OK);
    assert(!survey_sample_observation_identity_equal(
        &original_identity, &changed_identity));
    changed = original;
    changed.quality--;
    assert(survey_sample_observation_identity_capture(
               &changed, &changed_identity) == PROTO_OK);
    assert(!survey_sample_observation_identity_equal(
        &original_identity, &changed_identity));
    changed = original;
    changed.range_status = RANGE_RX_TIMEOUT;
    assert(survey_sample_observation_identity_capture(
               &changed, &changed_identity) == PROTO_OK);
    assert(!survey_sample_observation_identity_equal(
        &original_identity, &changed_identity));
    assert(survey_sample_observation_identity_equal(
        &original_identity, &original_identity));
}

static void test_reach_report_identity_retains_exact_status(void)
{
    struct survey_gateway_context context;
    const uint32_t survey_id = UINT32_C(0x12345678);
    const uint64_t anchor_id = UINT64_C(0x1111000000000001);
    struct survey_reachability_entry entries[] = {
        {
            .peer_id = UINT64_C(0x2222000000000002),
            .rssi_dbm = -61,
            .quality = 82u,
        },
    };
    struct survey_reachability_entry changed_entries[1];
    enum command_status stored_status;
    uint64_t stored_anchor_id;
    size_t stored_entry_count;

    assert(survey_gateway_begin(&context, survey_id, 4u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint_status(
               &context,
               survey_id,
               anchor_id,
               entries,
               1u,
               COMMAND_OK,
               NULL) == PROTO_OK);
    assert(context.report_count == 1u);
    assert(survey_gateway_report_info_at(
               &context,
               0u,
               &stored_anchor_id,
               &stored_entry_count,
               &stored_status) == PROTO_OK);
    assert(stored_status == COMMAND_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint_status(
               &context,
               survey_id,
               anchor_id,
               entries,
               1u,
               COMMAND_OK,
               NULL) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint_status(
               &context,
               survey_id,
               anchor_id,
               entries,
               1u,
               COMMAND_TIMEOUT,
               NULL) == PROTO_ERR_MALFORMED);
    memcpy(changed_entries, entries, sizeof(entries));
    changed_entries[0].quality--;
    assert(survey_gateway_note_reach_report_with_reverse_hint_status(
               &context,
               survey_id,
               anchor_id,
               changed_entries,
               1u,
               COMMAND_OK,
               NULL) == PROTO_ERR_MALFORMED);
}

static void
test_sample_observation_identity_distinguishes_prior_hash_collision(void)
{
    struct survey_sample first = {
        .pair = {
            .initiator_id = UINT64_C(0x0102030405060708),
            .responder_id = UINT64_C(0x1112131415161718),
            .survey_id = UINT32_C(0x12345678),
            .sample_count = 4u,
        },
        .round_id = 7u,
        .sample_index = 2u,
        .distance_mm = 65637,
        .quality = 58u,
        .range_status = RANGE_DELAYED_TX_MISSED,
    };
    struct survey_sample conflicting = first;
    struct survey_sample_observation_identity first_identity;
    struct survey_sample_observation_identity conflicting_identity;

    /*
     * These valid observations collide at 0x08765b68 under the former 32-bit
     * FNV semantic hash even when the reporter is the pair initiator.
     */
    conflicting.distance_mm = 948493;
    conflicting.quality = 24u;
    conflicting.range_status = RANGE_OK;
    assert(survey_sample_observation_identity_capture(
               &first, &first_identity) == PROTO_OK);
    assert(survey_sample_observation_identity_capture(
               &conflicting, &conflicting_identity) == PROTO_OK);
    assert(!survey_sample_observation_identity_equal(
        &first_identity, &conflicting_identity));
}

int main(void)
{
    test_sample_count_validation();
    test_pair_and_sample_validation();
    test_pair_result_transport_sequence_is_exhaustive_and_reset_stable();
    test_pair_result_transport_sequence_supports_legacy_controls();
    test_pair_result_transport_sequence_rejects_invalid_atomically();
    test_sample_distance_usability();
    test_missing_samples_use_responder_outcomes_only();
    test_pair_sample_admission_accepts_only_responder();
    test_delayed_sequential_generation_cannot_mutate_current_masks();
    test_discovery_post_rf_terminal_preserves_delayed_report_horizon();
    test_sample_nonce_is_unique_across_sequence_wrap();
    test_sample_tlvs_include_required_fields();
    test_sample_tlvs_round_trip_round_ownership();
    test_sample_tlvs_bind_operation_generation();
    test_pair_result_payload_validator_commits_atomically();
    test_sample_tlv_parser_rejects_bad_scalar_lengths_atomically();
    test_sample_parser_rejects_conflicting_singletons_atomically();
    test_reach_request_tlvs_include_survey_and_duration();
    test_reach_request_parser_rejects_malformed_tlvs();
    test_discovery_start_tlvs_round_trip_timing_config();
    test_discovery_start_rejects_conflicting_singletons();
    test_discovery_round_count_comes_from_runtime_profile();
    test_discovery_slot_validation_uses_physical_probe_budget();
    test_discovery_round_scheduler_has_one_continuous_window();
    test_pending_discovery_report_survives_queue_and_route_pressure();
    test_discovery_report_custody_tracks_upstream_hops();
    test_discovery_report_deadline_stays_fixed_after_eligibility();
    test_discovery_slot_count_tlv_defaults_and_overrides();
    test_discovery_expected_node_count_is_optional_and_bounded();
    test_multi_output_survey_parsers_fail_atomically();
    test_ml_anchor_pair_request_accepts_optional_slots_and_ignores_sample_count();
    test_ml_anchor_pair_request_rejects_invalid_slot_counts();
    test_discovery_timing_uses_packet_age();
    test_discovery_report_delay_uses_deterministic_anchor_slot();
    test_discovery_report_delay_rejects_overflow();
    test_discovery_packets_use_diagnostic_ids();
    test_reach_report_tlvs_include_peer_entries();
    test_reach_report_tlv_parser_round_trips_entries();
    test_reach_report_rejects_conflicting_identity_singletons();
    test_reach_report_tlv_parser_accepts_empty_peer_list();
    test_reach_report_tlv_parser_rejects_invalid_entries();
    test_reach_report_packet_is_diagnostic_gateway_bound();
    test_reach_request_packet_is_diagnostic_broadcast();
    test_pair_tlv_parser_round_trips_prepare_payload();
    test_pair_tlv_parser_rejects_missing_or_invalid_fields();
    test_pair_prepare_packet_targets_initiator_anchor();
    test_reachability_graph_plans_unique_pairs_with_requested_samples();
    test_reachability_retains_strongest_peers_deterministically();
    test_reachability_report_rejects_non_anchor_endpoints();
    test_reachability_graph_retains_directed_partial_components();
    test_generated_complete_reachability_counts_for_one_to_six_anchors();
    test_complete_k8_reaches_degree_cap_for_every_anchor();
    test_complete_k8_maximizes_high_quality_edges_under_degree_cap();
    test_pair_rounds_serialize_shared_endpoints();
    test_pair_rounds_check_asymmetric_cross_neighborhoods_both_ways();
    test_pair_rounds_require_separation_proof_for_sparse_reports();
    test_pair_rounds_serialize_shared_third_neighbor();
    test_pair_rounds_pack_deterministically_without_reordering_pairs();
    test_pair_rounds_use_hop_depth_only_for_incomplete_neighborhoods();
    test_gateway_context_collects_reports_and_sequences_pairs();
    test_gateway_pair_orientation_is_independent_of_reverse_depth();
    test_gateway_context_preserves_first_duplicate_report_and_hint();
    test_gateway_context_rejects_stale_or_oversized_reports();
    test_gateway_context_retains_only_accepted_reverse_hints();
    test_gateway_control_timeout_tracks_accepted_route_depth();
    test_gateway_context_retains_fifty_reverse_hints();
    test_gateway_compact_context_round_trips_worst_case();
    test_gateway_compact_context_capacity_is_transactional();
    test_gateway_compact_context_rejects_corrupt_indices();
    test_pair_planner_caps_degree_and_is_report_order_independent();
    test_pair_planner_augments_cardinality_without_disconnect();
    test_pair_planner_augments_two_edge_alternating_trail();
    test_pair_planner_augments_repeated_vertex_trail();
    test_pair_planner_repeats_augmentation_within_one_component();
    test_pair_planner_repairs_connected_component_after_forest_stall();
    test_pair_planner_component_budget_fairness();
    test_pair_planner_reaches_six_degree_ceiling_for_50_anchors();
    test_pair_planner_bounds_sparse_k50_augmentation_work();
    test_gateway_operation_identity_keeps_host_id_and_generation_distinct();
    test_reachability_plan_rejects_invalid_graph_or_capacity();
    test_result_packet_is_diagnostic_not_click();
    test_result_packet_can_use_responder_as_reporter();
    test_survey_packet_initializers_reject_zero_sequence_atomically();
    test_survey_packet_initializers_accept_sequence_wrap_boundary();
    test_discovery_report_sequence_exhaustion_is_fail_closed();
    test_sample_observation_identity_is_exact();
    test_reach_report_identity_retains_exact_status();
    test_sample_observation_identity_distinguishes_prior_hash_collision();
    return 0;
}
