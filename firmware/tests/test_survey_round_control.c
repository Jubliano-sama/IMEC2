#include "survey_round_control.h"

#include "survey.h"

#include <assert.h>
#include <string.h>

#define SURVEY_ID 0xAABBCCDDu
#define ROUND_ID 0x1234u
#define OPERATION_GENERATION UINT64_C(0x00000002A1B2C3D4)

static void test_round_id_optional_parser_and_encoding(void)
{
    uint8_t payload[16] = {TLV_COMMAND_ID, 2u, 1u, 0u};
    size_t payload_len = 4u;
    uint16_t round_id = UINT16_MAX;

    assert(TLV_SURVEY_ROUND_ID == 0xAFu);
    assert(CMD_SURVEY_GO_RETIRED_ID == 0x0105u);
    assert(survey_round_id_extract_tlv(payload,
                                       payload_len,
                                       &round_id) == PROTO_OK);
    assert(round_id == SURVEY_LEGACY_ROUND_ID);
    assert(survey_round_id_append_tlv(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      ROUND_ID) == PROTO_OK);
    assert(survey_round_id_extract_tlv(payload,
                                       payload_len,
                                       &round_id) == PROTO_OK);
    assert(round_id == ROUND_ID);
    assert(survey_round_id_append_tlv(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      SURVEY_LEGACY_ROUND_ID) ==
           PROTO_ERR_MALFORMED);
}

static void assert_commitment_changed(
    const struct survey_round_plan_identity *identity,
    const struct survey_round_plan_entry *entries,
    size_t entry_count,
    const uint8_t baseline[SEMANTIC_DIGEST_SHA256_LEN])
{
    uint8_t changed[SEMANTIC_DIGEST_SHA256_LEN];

    assert(survey_round_commitment_compute(identity,
                                           entries,
                                           entry_count,
                                           changed) == PROTO_OK);
    assert(memcmp(changed,
                  baseline,
                  SEMANTIC_DIGEST_SHA256_LEN) != 0);
}

static void test_round_commitment_binds_complete_plan(void)
{
    static const uint8_t expected_baseline[SEMANTIC_DIGEST_SHA256_LEN] = {
        0x9Bu, 0x20u, 0xD4u, 0x53u, 0x69u, 0x4Eu, 0xDEu, 0xC3u,
        0xC1u, 0xCAu, 0x9Bu, 0xA1u, 0xEAu, 0x7Bu, 0x51u, 0x8Fu,
        0xC9u, 0x4Fu, 0x01u, 0x2Bu, 0x38u, 0x39u, 0x0Du, 0xF0u,
        0xE2u, 0x01u, 0xC1u, 0x99u, 0xF5u, 0x0Fu, 0x2Cu, 0xEAu,
    };
    const struct survey_round_plan_identity base_identity = {
        .operation_generation = OPERATION_GENERATION,
        .survey_id = SURVEY_ID,
        .operation_session_id =
            (uint32_t)OPERATION_GENERATION,
        .execute_delay_ms = 11000u,
        .observation_window_ms = 32000u,
        .round_id = ROUND_ID,
        .max_parallel_pairs = 2u,
        .max_reruns = 3u,
    };
    const struct survey_round_plan_entry base_entries[2] = {
        {
            .pair = {
                .operation_generation = OPERATION_GENERATION,
                .survey_id = SURVEY_ID,
                .initiator_id = UINT64_C(0x101),
                .responder_id = UINT64_C(0x202),
                .sample_count = 5u,
            },
            .lane_index = 0u,
            .plan_pair_index = 4u,
            .reruns_started = 0u,
        },
        {
            .pair = {
                .operation_generation = OPERATION_GENERATION,
                .survey_id = SURVEY_ID,
                .initiator_id = UINT64_C(0x303),
                .responder_id = UINT64_C(0x404),
                .sample_count = 5u,
            },
            .lane_index = 1u,
            .plan_pair_index = 7u,
            .reruns_started = 1u,
        },
    };
    struct survey_round_plan_identity identity;
    struct survey_round_plan_entry entries[2];
    uint8_t baseline[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t repeated[SEMANTIC_DIGEST_SHA256_LEN];

    assert(survey_round_commitment_compute(&base_identity,
                                           base_entries,
                                           2u,
                                           baseline) == PROTO_OK);
    assert(survey_round_commitment_compute(&base_identity,
                                           base_entries,
                                           2u,
                                           repeated) == PROTO_OK);
    assert(memcmp(baseline, repeated, sizeof(baseline)) == 0);
    assert(memcmp(baseline,
                  expected_baseline,
                  sizeof(baseline)) == 0);

#define MUTATE_IDENTITY(field, value)                                      \
    do {                                                                   \
        identity = base_identity;                                          \
        identity.field = (value);                                          \
        assert_commitment_changed(&identity, base_entries, 2u, baseline);   \
    } while (0)
    MUTATE_IDENTITY(round_id, ROUND_ID + 1u);
    MUTATE_IDENTITY(execute_delay_ms, 11001u);
    MUTATE_IDENTITY(observation_window_ms, 32001u);
    MUTATE_IDENTITY(max_parallel_pairs, 3u);
    MUTATE_IDENTITY(max_reruns, 4u);
#undef MUTATE_IDENTITY

    identity = base_identity;
    identity.survey_id++;
    memcpy(entries, base_entries, sizeof(entries));
    entries[0].pair.survey_id = identity.survey_id;
    entries[1].pair.survey_id = identity.survey_id;
    assert_commitment_changed(&identity, entries, 2u, baseline);

    identity = base_identity;
    identity.operation_generation++;
    identity.operation_session_id =
        survey_operation_session_id(identity.operation_generation);
    memcpy(entries, base_entries, sizeof(entries));
    entries[0].pair.operation_generation =
        identity.operation_generation;
    entries[1].pair.operation_generation =
        identity.operation_generation;
    assert_commitment_changed(&identity, entries, 2u, baseline);

#define MUTATE_ENTRY(index, field, value)                                  \
    do {                                                                   \
        memcpy(entries, base_entries, sizeof(entries));                    \
        entries[(index)].field = (value);                                  \
        assert_commitment_changed(&base_identity, entries, 2u, baseline);  \
    } while (0)
    MUTATE_ENTRY(0u, plan_pair_index, 5u);
    MUTATE_ENTRY(1u, reruns_started, 2u);
    MUTATE_ENTRY(0u, pair.initiator_id, UINT64_C(0x102));
    MUTATE_ENTRY(0u, pair.responder_id, UINT64_C(0x203));
    MUTATE_ENTRY(0u, pair.sample_count, 4u);
#undef MUTATE_ENTRY

    identity = base_identity;
    identity.operation_session_id++;
    assert(survey_round_commitment_compute(&identity,
                                           base_entries,
                                           2u,
                                           repeated) ==
           PROTO_ERR_MALFORMED);
    memcpy(entries, base_entries, sizeof(entries));
    entries[1].lane_index = 0u;
    assert(survey_round_commitment_compute(&base_identity,
                                           entries,
                                           2u,
                                           repeated) ==
           PROTO_ERR_MALFORMED);
}

static void test_manual_pair_commitment_binds_generation_and_pair(void)
{
    const struct survey_pair base = {
        .operation_generation = OPERATION_GENERATION,
        .survey_id = SURVEY_ID,
        .initiator_id = UINT64_C(0x1111),
        .responder_id = UINT64_C(0x2222),
        .sample_count = 3u,
    };
    struct survey_pair changed = base;
    uint8_t baseline[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t other[SEMANTIC_DIGEST_SHA256_LEN];

    assert(survey_pair_control_commitment_compute(
               &base, baseline) == PROTO_OK);
    assert(survey_pair_control_commitment_compute(
               &base, other) == PROTO_OK);
    assert(memcmp(baseline, other, sizeof(baseline)) == 0);

    changed.operation_generation++;
    assert(survey_pair_control_commitment_compute(
               &changed, other) == PROTO_OK);
    assert(memcmp(baseline, other, sizeof(baseline)) != 0);
    changed = base;
    changed.responder_id++;
    assert(survey_pair_control_commitment_compute(
               &changed, other) == PROTO_OK);
    assert(memcmp(baseline, other, sizeof(baseline)) != 0);
}

static void test_start_release_is_separate_from_failure_horizons(void)
{
    assert(SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS == 1000u);
    assert(SURVEY_ROUND_START_EXECUTE_DELAY_MS == 15000u);
    assert(SURVEY_ROUND_START_EXECUTE_DELAY_MS >=
           8u * SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS +
               SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS +
               2u * SURVEY_PAIR_START_SKEW_MARGIN_MS);
    assert(SURVEY_ROUND_START_EXECUTE_DELAY_MS <
           SURVEY_PAIR_CONTROL_MAX_REQUEST_TIMEOUT_MS);
    assert(SURVEY_PAIR_CONTROL_MAX_REQUEST_TIMEOUT_MS == 135000u);
    assert(SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS == 135000u);
    assert(SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS == 540000u);
    assert(SURVEY_PAIR_PREPARED_LEASE_MS == 2340000u);
    assert(SURVEY_PAIR_PREPARED_LEASE_MS ==
           SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS +
               SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS);
    assert(SURVEY_ROUND_START_EXECUTE_DELAY_MS <
           SURVEY_PAIR_PREPARED_LEASE_MS);
}

static void test_initiator_start_uses_shared_execute_age_only(void)
{
    assert(survey_round_start_initiator_send_allowed(0u));
    assert(SURVEY_ROUND_START_INITIATOR_SEND_CUTOFF_MS == 13000u);
    assert(survey_round_start_initiator_send_allowed(
        SURVEY_ROUND_START_INITIATOR_SEND_CUTOFF_MS - 1u));
    assert(!survey_round_start_initiator_send_allowed(
        SURVEY_ROUND_START_INITIATOR_SEND_CUTOFF_MS));
    assert(!survey_round_start_initiator_send_allowed(27500u));
}

int main(void)
{
    test_round_id_optional_parser_and_encoding();
    test_round_commitment_binds_complete_plan();
    test_manual_pair_commitment_binds_generation_and_pair();
    test_start_release_is_separate_from_failure_horizons();
    test_initiator_start_uses_shared_execute_age_only();
    return 0;
}
