#include "survey_round_control.h"

#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "survey.h"

#include <assert.h>
#include <string.h>

#define GATEWAY_ID 0x1111222233334444ull
#define SURVEY_ID 0xAABBCCDDu
#define ROUND_ID 0x1234u
#define OPERATION_GENERATION UINT64_C(0x00000002A1B2C3D4)

static void test_round_id_optional_parser_and_encoding(void)
{
    uint8_t payload[16] = {TLV_COMMAND_ID, 2u, 1u, 0u};
    size_t payload_len = 4u;
    uint16_t round_id = UINT16_MAX;

    assert(TLV_SURVEY_ROUND_ID == 0xAFu);
    assert(CMD_SURVEY_GO == 0x0105u);
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

static void test_go_payload_round_trip_and_parser_rejections(void)
{
    const struct survey_round_go go = {
        .operation_generation = OPERATION_GENERATION,
        .round_commitment = {0x11u, 0x22u, 0x33u, 0x44u},
        .survey_id = SURVEY_ID,
        .round_id = ROUND_ID,
    };
    struct survey_round_go decoded = {0};
    const uint8_t *round_value = NULL;
    uint8_t round_len = 0u;
    uint8_t payload[96] = {0};
    size_t payload_len = 0u;

    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_OK);
    assert(decoded.survey_id == go.survey_id);
    assert(decoded.round_id == go.round_id);
    assert(decoded.operation_generation == go.operation_generation);
    assert(memcmp(decoded.round_commitment,
                  go.round_commitment,
                  sizeof(decoded.round_commitment)) == 0);

    payload[2u] = (uint8_t)(CMD_SURVEY_START_PAIR & 0xffu);
    payload[3u] = (uint8_t)(CMD_SURVEY_START_PAIR >> 8);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);
    payload[2u] = (uint8_t)(CMD_SURVEY_GO & 0xffu);
    payload[3u] = (uint8_t)(CMD_SURVEY_GO >> 8);
    assert(tlv_find_unique(payload,
                           payload_len,
                           TLV_SURVEY_ROUND_ID,
                           &round_value,
                           &round_len) == PROTO_OK);
    assert(round_len == sizeof(uint16_t));
    ((uint8_t *)round_value)[0] = 0u;
    ((uint8_t *)round_value)[1] = 0u;
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len - 1u,
                                     &decoded) == PROTO_ERR_MALFORMED);
}

static void test_go_parser_rejects_conflicting_singletons(void)
{
    const struct survey_round_go go = {
        .operation_generation = OPERATION_GENERATION,
        .round_commitment = {0x55u},
        .survey_id = SURVEY_ID,
        .round_id = ROUND_ID,
    };
    struct survey_round_go decoded;
    uint8_t payload[128];
    size_t payload_len = 0u;

    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ID,
                          SURVEY_ID + 1u) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_ROUND_ID,
                          ROUND_ID + 1u) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_SURVEY_OPERATION_GENERATION,
                          OPERATION_GENERATION + 1u) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_SURVEY_ROUND_COMMITMENT,
                            go.round_commitment,
                            sizeof(go.round_commitment)) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);
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
                .sample_count = 3u,
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

static void test_go_packet_initializer(void)
{
    struct proto_packet packet;
    struct proto_packet unchanged;

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       7u,
                                       16u) == PROTO_OK);
    assert(packet.msg_type == MSG_COMMAND);
    assert(packet.flags == 0u);
    assert(packet.src_id == GATEWAY_ID);
    assert(packet.dst_id == 0u);
    assert(packet.session_id == SURVEY_ID);
    assert(packet.seq == 7u);
    assert(packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(packet.payload_len == 16u);
    assert(packet.message_age_ms == 0u);

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       UINT16_MAX,
                                       16u) == PROTO_OK);
    assert(packet.seq == UINT16_MAX);
    assert(packet.dst_id == MESH_BROADCAST_ID);
    assert(packet.message_age_ms == 0u);

    assert(survey_round_go_init_packet(NULL,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       1u,
                                       0u) == PROTO_ERR_ARG);
    assert(survey_round_go_init_packet(&packet,
                                       0u,
                                       SURVEY_ID,
                                       1u,
                                       0u) == PROTO_ERR_MALFORMED);
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       0u,
                                       1u,
                                       0u) == PROTO_ERR_MALFORMED);
    memset(&packet, 0xa5, sizeof(packet));
    unchanged = packet;
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       0u,
                                       0u) == PROTO_ERR_MALFORMED);
    assert(memcmp(&packet, &unchanged, sizeof(packet)) == 0);
}

static void test_go_execute_delay_scales_by_complete_forward_horizon(void)
{
    assert(survey_round_go_execute_delay_ms(0u) ==
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(1u) ==
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(2u) ==
           2u * SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(3u) ==
           3u * SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
}

static void test_go_execute_delay_covers_first_receiver_forward(void)
{
    const uint32_t first_receiver_forward_horizon_ms =
        FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS +
        MESH_RADIO_WAKE_TRAIN_MS +
        FLOOD_RELAY_BURST_MS +
        FLOOD_POST_ROOT_GUARD_MS;

    /*
     * Local GO delivery happens after the relay core has synchronously built
     * and sent the first broadcast forward. Even a directly reached anchor
     * must therefore retain enough GO delay for that complete worst case.
     */
    assert(SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS >=
           first_receiver_forward_horizon_ms);
    assert(SURVEY_ROUND_GO_BASE_EXECUTE_DELAY_MS >=
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(0u) >=
           first_receiver_forward_horizon_ms);
    assert(survey_round_go_execute_delay_ms(1u) >=
           first_receiver_forward_horizon_ms);
}

int main(void)
{
    test_round_id_optional_parser_and_encoding();
    test_go_payload_round_trip_and_parser_rejections();
    test_go_parser_rejects_conflicting_singletons();
    test_round_commitment_binds_complete_plan();
    test_manual_pair_commitment_binds_generation_and_pair();
    test_go_packet_initializer();
    test_go_execute_delay_scales_by_complete_forward_horizon();
    test_go_execute_delay_covers_first_receiver_forward();
    return 0;
}
