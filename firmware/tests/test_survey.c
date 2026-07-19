#include "survey.h"
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
    value.range_status = RANGE_TIMING_INVALID;
    assert(survey_sample_validate(&value) == PROTO_OK);
}

static void test_sample_distance_usability(void)
{
    struct survey_sample value = sample();

    value.distance_mm = -4726;
    assert(!survey_sample_distance_usable(&value));

    value.distance_mm = 0;
    assert(!survey_sample_distance_usable(&value));

    value.distance_mm = SURVEY_MIN_USABLE_DISTANCE_MM;
    assert(!survey_sample_distance_usable(&value));

    value.distance_mm = SURVEY_MIN_USABLE_DISTANCE_MM + 1;
    assert(survey_sample_distance_usable(&value));

    value.range_status = RANGE_RX_TIMEOUT;
    value.distance_mm = 4726;
    assert(!survey_sample_distance_usable(&value));
}

static void test_missing_samples_require_unusable_from_both_reporters(void)
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
    assert(!survey_pair_missing_samples_all_unusable(
        3u, sample_1, sample_0 | sample_2, sample_0));
    assert(survey_pair_missing_samples_all_unusable(
        3u, sample_1, sample_0 | sample_2, sample_0 | sample_2));
    assert(!survey_pair_missing_samples_all_unusable(
        3u, sample_0 | sample_1 | sample_2, UINT16_MAX, UINT16_MAX));
}

static void test_sample_nonce_is_unique_across_sequence_wrap(void)
{
    struct survey_sample value = sample();
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
    assert(payload_len == SURVEY_SAMPLE_TLV_MAX_LEN);
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
                              PROTO_TLV_U16_ENCODED_LEN);
    assert(survey_extract_sample_tlvs(payload, payload_len, &decoded) ==
           PROTO_OK);
    assert(decoded.round_id == SURVEY_LEGACY_ROUND_ID);
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
        .start_delay_ms = 6000u,
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

static void test_discovery_round_count_comes_from_runtime_profile(void)
{
    const struct survey_discovery_config config = {
        .survey_id = 0xABCDEF01u,
        .start_delay_ms = 6000u,
        .slot_ms = 40u,
        .slot_count = 50u,
        .round_count = 2u,
    };
    const struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
        .value.discovery = {
            .start_delay_ms = 6000u,
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
    assert(direct_ms == 5000u);
    assert(two_hop_ms == 9000u);
    assert(two_hop_ms > SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS);
    assert(three_hop_ms == 13000u);
    assert(maximum_ms == 17000u);
    assert(maximum_ms == SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS);

    assert(survey_discovery_report_custody_ms(0u) == maximum_ms);
    assert(survey_discovery_report_custody_ms(SURVEY_DEFAULT_TTL + 1u) ==
           maximum_ms);
    assert(survey_discovery_report_custody_ms(UINT8_MAX) == maximum_ms);
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
                                               45u,
                                               24u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert(packet.src_id == 0x1111222233334444ull);
    assert(packet.dst_id == 0x9999888877776666ull);
    assert(packet.session_id == config.survey_id);
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
    size_t entry_count = 99u;

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
                                               77u,
                                               (uint8_t)payload_len) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == expected_anchor_id);
    assert(packet.dst_id == expected_gateway_id);
    assert(packet.session_id == expected_survey_id);
    assert(packet.seq == 77u);
    assert(packet.payload_len == payload_len);

    assert(survey_gateway_begin(&gateway, expected_survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&gateway,
                                            survey_id,
                                            anchor_id,
                                            NULL,
                                            entry_count) == PROTO_OK);
    assert(gateway.report_count == 1u);
    assert(gateway.reports[0].valid);
    assert(gateway.reports[0].anchor_id == expected_anchor_id);
    assert(gateway.reports[0].entry_count == 0u);
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
    assert(pair_count == 2u);
    assert(pairs[0].survey_id == 0xAABBCCDDu);
    assert(pairs[0].initiator_id == 0x1111000000000001ull);
    assert(pairs[0].responder_id == 0x2222000000000002ull);
    assert(pairs[0].sample_count == 5u);
    assert(pairs[1].initiator_id == 0x1111000000000001ull);
    assert(pairs[1].responder_id == 0x3333000000000003ull);
    assert(pairs[1].sample_count == 5u);
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

static struct survey_gateway_report_slot *round_test_report(
    struct survey_gateway_context *context,
    uint64_t anchor_id)
{
    for (size_t i = 0u; i < context->report_count; i++) {
        if (context->reports[i].valid &&
            context->reports[i].anchor_id == anchor_id) {
            return &context->reports[i];
        }
    }
    assert(false);
    return NULL;
}

static void round_test_context_init(
    struct survey_gateway_context *context,
    const struct survey_gateway_pair_entry *pairs,
    size_t pair_count)
{
    memset(context, 0, sizeof(*context));
    context->survey_id = 0xAABBCCDDu;
    context->sample_count = 2u;
    context->pair_count = pair_count;
    context->pairs_planned = true;
    memcpy(context->pairs, pairs, pair_count * sizeof(pairs[0]));

    for (size_t i = 0u; i < pair_count; i++) {
        const uint64_t endpoint_ids[] = {
            pairs[i].initiator_id,
            pairs[i].responder_id,
        };

        for (size_t endpoint = 0u; endpoint < 2u; endpoint++) {
            bool found = false;

            for (size_t report = 0u; report < context->report_count; report++) {
                if (context->reports[report].anchor_id ==
                    endpoint_ids[endpoint]) {
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
            assert(context->report_count < SURVEY_GATEWAY_MAX_REPORTS);
            context->reports[context->report_count] =
                (struct survey_gateway_report_slot) {
                    .anchor_id = endpoint_ids[endpoint],
                    .reverse_hop_count = 1u,
                    .reverse_hint_valid = true,
                    .valid = true,
                };
            context->report_count++;
        }
    }
}

static void round_test_add_peer(struct survey_gateway_context *context,
                                uint64_t anchor_id,
                                uint64_t peer_id)
{
    struct survey_gateway_report_slot *slot =
        round_test_report(context, anchor_id);

    assert(slot->entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
    slot->entries[slot->entry_count++] = (struct survey_reachability_entry) {
        .peer_id = peer_id,
        .rssi_dbm = -60,
        .quality = 80u,
    };
}

static void test_pair_rounds_serialize_shared_endpoints(void)
{
    const struct survey_gateway_pair_entry pairs[] = {
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
    const struct survey_gateway_pair_entry pairs[] = {
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

static void test_pair_rounds_pack_fully_disjoint_pairs_together(void)
{
    const struct survey_gateway_pair_entry pairs[] = {
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
    const struct survey_gateway_pair_entry pairs[] = {
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
    const struct survey_gateway_pair_entry pairs[] = {
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
    const struct survey_gateway_pair_entry pairs[] = {
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
    saturated->entries[0].peer_id = 0xC5u;
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
    assert(context.pair_count == 2u);

    assert(survey_gateway_next_pair(&context, &pair) == PROTO_OK);
    assert(pair.survey_id == 0xAABBCCDDu);
    assert(pair.initiator_id == 0x1111000000000001ull);
    assert(pair.responder_id == 0x2222000000000002ull);
    assert(pair.sample_count == 5u);

    assert(survey_gateway_next_pair(&context, &pair) == PROTO_OK);
    assert(pair.initiator_id == 0x1111000000000001ull);
    assert(pair.responder_id == 0x3333000000000003ull);
    assert(pair.sample_count == 5u);

    assert(survey_gateway_next_pair(&context, &pair) == PROTO_ERR_NOT_FOUND);
}

static void test_gateway_pair_orientation_uses_reverse_depth(void)
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
    assert(survey_gateway_next_pair(&context, &pair) == PROTO_OK);
    assert(pair.initiator_id == higher_id);
    assert(pair.responder_id == lower_id);

    lower_hint.hop_count = 1u;
    assert(survey_gateway_begin(&context, survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, lower_id, lower_entries, 1u,
               &lower_hint) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, higher_id, higher_entries, 1u,
               &higher_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_next_pair(&context, &pair) == PROTO_OK);
    assert(pair.initiator_id == lower_id);
    assert(pair.responder_id == higher_id);

    assert(survey_gateway_begin(&context, survey_id, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(
               &context, survey_id, lower_id, lower_entries, 1u) == PROTO_OK);
    assert(survey_gateway_note_reach_report_with_reverse_hint(
               &context, survey_id, higher_id, higher_entries, 1u,
               &higher_hint) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_next_pair(&context, &pair) == PROTO_OK);
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
               replacement_entries,
               sizeof(replacement_entries) / sizeof(replacement_entries[0]),
               &duplicate_hint) == PROTO_OK);
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
    assert(context.reports[0].entry_count == 1u);
    assert(context.reports[0].entries[0].peer_id ==
           first_entries[0].peer_id);
    assert(context.reports[0].entries[0].quality == first_entries[0].quality);
    assert(survey_gateway_reverse_hint_for_target(&context,
                                                  first_hint.target_id,
                                                  &stored_hint) == PROTO_OK);
    assert(stored_hint.next_hop_id == first_hint.next_hop_id);
    assert(stored_hint.quality == first_hint.quality);
    assert(stored_hint.hop_count == first_hint.hop_count);
    assert(survey_gateway_plan_pairs(&context) == PROTO_ERR_NOT_FOUND);
    assert(context.pair_count == 0u);
}

static void test_gateway_context_rejects_stale_or_oversized_reports(void)
{
    struct survey_gateway_context context;
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 1u];

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
    assert(context.reports[0].entry_count ==
           SURVEY_GATEWAY_MAX_PEERS_PER_REPORT);
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
}

static void test_gateway_context_retains_fifty_reverse_hints(void)
{
    struct survey_gateway_context context;
    const uint64_t anchor_base = UINT64_C(0xa700000000000100);
    const uint64_t relay_base = UINT64_C(0xa800000000000100);

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 1u) == PROTO_OK);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        const uint64_t target_id = anchor_base + i;
        const struct survey_gateway_reverse_hint hint = {
            .target_id = target_id,
            .next_hop_id = i < 20u ? target_id : relay_base + (i % 5u),
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
               (i < 20u ? target_id : relay_base + (i % 5u)));
    }
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

    _Static_assert(sizeof(context.pairs[0]) == 16u,
                   "gateway context must retain endpoint-only pairs");

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

        assert(survey_gateway_next_pair(&context, &next) == PROTO_OK);
        assert(next.survey_id == pairs[i].survey_id);
        assert(next.initiator_id == pairs[i].initiator_id);
        assert(next.responder_id == pairs[i].responder_id);
        assert(next.sample_count == pairs[i].sample_count);
    }
    assert(survey_gateway_next_pair(&context, &pairs[0]) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_gateway_auto_sequences_prepare_and_start_actions(void)
{
    struct survey_gateway_context context;
    struct survey_gateway_auto_context auto_context;
    struct survey_gateway_auto_action action = {0};
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
    };
    bool launched = false;
    bool skipped = false;

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 4u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x1111000000000001ull,
                                            a_entries,
                                            sizeof(a_entries) / sizeof(a_entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x2222000000000002ull,
                                            NULL,
                                            0u) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_auto_begin(&auto_context) == PROTO_OK);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(!action.complete);
    assert(action.stage == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);
    assert(action.command_id == CMD_SURVEY_PREPARE_PAIR);
    assert(action.target_id == 0x1111000000000001ull);
    assert(action.pair.responder_id == 0x2222000000000002ull);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);
    assert(survey_gateway_auto_command_matches(&auto_context,
                                               CMD_SURVEY_PREPARE_PAIR,
                                               0x1111000000000001ull,
                                               0xAABBCCDDu));
    assert(!survey_gateway_auto_command_matches(&auto_context,
                                                CMD_SURVEY_START_PAIR,
                                                0x1111000000000001ull,
                                                0xAABBCCDDu));
    assert(survey_gateway_auto_note_result(&auto_context,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           0x1111000000000001ull,
                                           0xAABBCCDDu,
                                           COMMAND_OK,
                                           &launched,
                                           &skipped) == PROTO_OK);
    assert(!launched && !skipped);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.stage == SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER);
    assert(action.command_id == CMD_SURVEY_PREPARE_PAIR);
    assert(action.target_id == 0x2222000000000002ull);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);
    assert(survey_gateway_auto_note_result(&auto_context,
                                           action.command_id,
                                           action.target_id,
                                           action.pair.survey_id,
                                           COMMAND_OK,
                                           &launched,
                                           &skipped) == PROTO_OK);
    assert(!launched && !skipped);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.stage == SURVEY_GATEWAY_AUTO_START_RESPONDER);
    assert(action.command_id == CMD_SURVEY_START_PAIR);
    assert(action.target_id == 0x2222000000000002ull);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);
    assert(survey_gateway_auto_note_result(&auto_context,
                                           action.command_id,
                                           action.target_id,
                                           action.pair.survey_id,
                                           COMMAND_OK,
                                           &launched,
                                           &skipped) == PROTO_OK);
    assert(!launched && !skipped);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.stage == SURVEY_GATEWAY_AUTO_START_INITIATOR);
    assert(action.command_id == CMD_SURVEY_START_PAIR);
    assert(action.target_id == 0x1111000000000001ull);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);
    assert(survey_gateway_auto_note_result(&auto_context,
                                           action.command_id,
                                           action.target_id,
                                           action.pair.survey_id,
                                           COMMAND_OK,
                                           &launched,
                                           &skipped) == PROTO_OK);
    assert(launched && !skipped);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.complete);
    assert(!auto_context.running);
    assert(auto_context.stage == SURVEY_GATEWAY_AUTO_IDLE);
}

static void test_gateway_auto_reranges_current_pair_without_advancing_plan(void)
{
    const struct survey_pair pair = {
        .survey_id = 0xAABBCCDDu,
        .initiator_id = 0x1111000000000001ull,
        .responder_id = 0x2222000000000002ull,
        .sample_count = 1u,
    };
    struct survey_gateway_context gateway_context = {
        .survey_id = pair.survey_id,
        .sample_count = pair.sample_count,
        .pairs = {{
            .initiator_id = pair.initiator_id,
            .responder_id = pair.responder_id,
        }},
        .pair_count = 1u,
        .next_pair_index = 1u,
        .pairs_planned = true,
    };
    struct survey_gateway_auto_context auto_context = {
        .pair = pair,
        .stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR,
        .running = true,
    };
    struct survey_gateway_auto_context invalid_context = auto_context;
    const enum survey_gateway_auto_stage expected_stages[] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };
    const uint64_t expected_targets[] = {
        pair.initiator_id,
        pair.responder_id,
        pair.responder_id,
        pair.initiator_id,
    };
    struct survey_gateway_auto_action action = {0};
    bool launched = false;
    bool skipped = false;

    invalid_context.waiting = true;
    assert(survey_gateway_auto_rerun_pair(&invalid_context) == PROTO_ERR_BUSY);
    invalid_context = auto_context;
    invalid_context.stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER;
    assert(survey_gateway_auto_rerun_pair(&invalid_context) == PROTO_ERR_STALE);

    for (uint8_t rerun = 1u;
         rerun <= SURVEY_GATEWAY_PAIR_MAX_RERUNS;
         rerun++) {
        assert(survey_gateway_auto_rerun_pair(&auto_context) == PROTO_OK);
        assert(gateway_context.next_pair_index == 1u);
        assert(auto_context.pair_reruns_started == rerun);
        assert(auto_context.stage == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);
        assert(memcmp(&auto_context.pair, &pair, sizeof(pair)) == 0);

        for (size_t i = 0u;
             i < sizeof(expected_stages) / sizeof(expected_stages[0]);
             i++) {
            memset(&action, 0, sizeof(action));
            assert(survey_gateway_auto_next_action(&auto_context,
                                                   &gateway_context,
                                                   &action) == PROTO_OK);
            assert(!action.complete);
            assert(action.stage == expected_stages[i]);
            assert(action.target_id == expected_targets[i]);
            assert(memcmp(&action.pair, &pair, sizeof(pair)) == 0);
            assert(gateway_context.next_pair_index == 1u);
            assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);
            assert(survey_gateway_auto_note_result(&auto_context,
                                                   action.command_id,
                                                   action.target_id,
                                                   action.pair.survey_id,
                                                   COMMAND_OK,
                                                   &launched,
                                                   &skipped) == PROTO_OK);
            assert(!skipped);
            assert(launched ==
                   (expected_stages[i] == SURVEY_GATEWAY_AUTO_START_INITIATOR));
        }
    }

    assert(survey_gateway_auto_rerun_pair(&auto_context) == PROTO_ERR_NO_SPACE);
    assert(auto_context.pair_reruns_started ==
           SURVEY_GATEWAY_PAIR_MAX_RERUNS);
    assert(auto_context.stage == SURVEY_GATEWAY_AUTO_LOAD_PAIR);
    assert(memcmp(&auto_context.pair, &pair, sizeof(pair)) == 0);
    assert(gateway_context.next_pair_index == 1u);

    memset(&action, 0, sizeof(action));
    assert(survey_gateway_auto_next_action(&auto_context,
                                           &gateway_context,
                                           &action) == PROTO_OK);
    assert(action.complete);
    assert(gateway_context.next_pair_index == 1u);
}

static void test_gateway_auto_skips_pair_on_failed_command_result(void)
{
    struct survey_gateway_context context;
    struct survey_gateway_auto_context auto_context;
    struct survey_gateway_auto_action action = {0};
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = 0x2222000000000002ull, .rssi_dbm = -61, .quality = 82u},
    };
    bool launched = true;
    bool skipped = false;

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 4u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x1111000000000001ull,
                                            a_entries,
                                            sizeof(a_entries) / sizeof(a_entries[0])) == PROTO_OK);
    assert(survey_gateway_note_reach_report(&context,
                                            0xAABBCCDDu,
                                            0x2222000000000002ull,
                                            NULL,
                                            0u) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(survey_gateway_auto_begin(&auto_context) == PROTO_OK);
    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);

    assert(survey_gateway_auto_retry_pending(&auto_context,
                                             CMD_SURVEY_PREPARE_PAIR,
                                             0x2222000000000002ull,
                                             0xAABBCCDDu) == PROTO_ERR_NOT_FOUND);
    assert(survey_gateway_auto_retry_pending(&auto_context,
                                             CMD_SURVEY_PREPARE_PAIR,
                                             0x1111000000000001ull,
                                             0xAABBCCDDu) == PROTO_OK);
    assert(!auto_context.waiting);
    assert(auto_context.stage == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);
    memset(&action, 0, sizeof(action));
    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.command_id == CMD_SURVEY_PREPARE_PAIR);
    assert(action.target_id == 0x1111000000000001ull);
    assert(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK);

    assert(survey_gateway_auto_note_result(&auto_context,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           0x1111000000000001ull,
                                           0xAABBCCDDu,
                                           COMMAND_TIMEOUT,
                                           &launched,
                                           &skipped) == PROTO_OK);
    assert(!launched && skipped);
    assert(!auto_context.waiting);
    assert(auto_context.stage == SURVEY_GATEWAY_AUTO_LOAD_PAIR);

    assert(survey_gateway_auto_next_action(&auto_context, &context, &action) == PROTO_OK);
    assert(action.complete);
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
                   &pair_count) == PROTO_ERR_NOT_FOUND);
        assert(pair_count == 0u);
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
    assert(packet.src_id == value.pair.initiator_id);
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
                                                   0x5555000000000005ull,
                                                   0x9999888877776666ull,
                                                   43u,
                                                   78u) == PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_sample_count_validation();
    test_pair_and_sample_validation();
    test_sample_distance_usability();
    test_missing_samples_require_unusable_from_both_reporters();
    test_sample_nonce_is_unique_across_sequence_wrap();
    test_sample_tlvs_include_required_fields();
    test_sample_tlvs_round_trip_round_ownership();
    test_sample_tlv_parser_rejects_bad_scalar_lengths_atomically();
    test_reach_request_tlvs_include_survey_and_duration();
    test_reach_request_parser_rejects_malformed_tlvs();
    test_discovery_start_tlvs_round_trip_timing_config();
    test_discovery_round_count_comes_from_runtime_profile();
    test_discovery_slot_validation_uses_physical_probe_budget();
    test_discovery_round_scheduler_has_one_continuous_window();
    test_pending_discovery_report_survives_queue_and_route_pressure();
    test_discovery_report_custody_tracks_upstream_hops();
    test_discovery_slot_count_tlv_defaults_and_overrides();
    test_ml_anchor_pair_request_accepts_optional_slots_and_ignores_sample_count();
    test_ml_anchor_pair_request_rejects_invalid_slot_counts();
    test_discovery_timing_uses_packet_age();
    test_discovery_report_delay_uses_deterministic_anchor_slot();
    test_discovery_report_delay_rejects_overflow();
    test_discovery_packets_use_diagnostic_ids();
    test_reach_report_tlvs_include_peer_entries();
    test_reach_report_tlv_parser_round_trips_entries();
    test_reach_report_tlv_parser_accepts_empty_peer_list();
    test_reach_report_tlv_parser_rejects_invalid_entries();
    test_reach_report_packet_is_diagnostic_gateway_bound();
    test_reach_request_packet_is_diagnostic_broadcast();
    test_pair_tlv_parser_round_trips_prepare_payload();
    test_pair_tlv_parser_rejects_missing_or_invalid_fields();
    test_pair_prepare_packet_targets_initiator_anchor();
    test_reachability_graph_plans_unique_pairs_with_requested_samples();
    test_generated_complete_reachability_counts_for_one_to_six_anchors();
    test_pair_rounds_serialize_shared_endpoints();
    test_pair_rounds_check_asymmetric_cross_neighborhoods_both_ways();
    test_pair_rounds_pack_fully_disjoint_pairs_together();
    test_pair_rounds_serialize_shared_third_neighbor();
    test_pair_rounds_pack_deterministically_without_reordering_pairs();
    test_pair_rounds_use_hop_depth_only_for_incomplete_neighborhoods();
    test_gateway_context_collects_reports_and_sequences_pairs();
    test_gateway_pair_orientation_uses_reverse_depth();
    test_gateway_context_preserves_first_duplicate_report_and_hint();
    test_gateway_context_rejects_stale_or_oversized_reports();
    test_gateway_context_retains_only_accepted_reverse_hints();
    test_gateway_control_timeout_tracks_accepted_route_depth();
    test_gateway_context_retains_fifty_reverse_hints();
    test_pair_planner_caps_degree_and_is_report_order_independent();
    test_pair_planner_reaches_six_degree_ceiling_for_50_anchors();
    test_gateway_auto_sequences_prepare_and_start_actions();
    test_gateway_auto_reranges_current_pair_without_advancing_plan();
    test_gateway_auto_skips_pair_on_failed_command_result();
    test_reachability_plan_rejects_invalid_graph_or_capacity();
    test_result_packet_is_diagnostic_not_click();
    test_result_packet_can_use_responder_as_reporter();
    return 0;
}
