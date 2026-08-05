#include "app_gateway_eack_retry.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct gateway_collection_state collection(uint32_t command_seq,
                                                  uint32_t collection_epoch_id,
                                                  uint8_t eack_round)
{
    return (struct gateway_collection_state) {
        .gateway_id = UINT64_C(0x1122334455667788),
        .gateway_epoch = 9u,
        .command_seq = command_seq,
        .collection_epoch_id = collection_epoch_id,
        .membership_epoch = 7u,
        .expected_count = 20u,
        .retry_round = eack_round,
        .eack_sequence = (uint16_t)eack_round + 1u,
        .collection_open = true,
        .eack_pending = true,
    };
}

static void collection_set_received_results(struct gateway_collection_state *state,
                                            uint16_t received_count)
{
    assert(state != NULL);
    assert(received_count <= GATEWAY_COLLECTION_RESULT_CACHE_SIZE);
    memset(state->results, 0, sizeof(state->results));
    state->received_count = received_count;
    for (uint16_t i = 0u; i < received_count; i++) {
        state->results[i].id.node_id =
            UINT64_C(0xA000000000000000) + (uint64_t)i + 1u;
        state->results[i].valid = true;
    }
}

static void test_repeated_total_failures_use_random_exponential_backoff(void)
{
    struct app_gateway_eack_retry_state state = {0};
    const struct gateway_collection_state eack = collection(33u, 44u, 2u);
    const uint32_t minimums[] = {100u, 200u, 400u, 800u, 800u};
    const uint32_t maximums[] = {300u, 600u, 1200u, 2400u, 2400u};

    for (size_t i = 0u; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
        uint32_t delay_ms = app_gateway_eack_retry_note_failure(
            &state,
            &eack,
            UINT32_C(0x10000000) + (uint32_t)i);

        assert(delay_ms >= minimums[i]);
        assert(delay_ms <= maximums[i]);
        assert(state.rf_retry.retry_round == i + 1u);
        assert(state.active);
        assert(state.identity.command_seq == eack.command_seq);
        assert(state.identity.collection_epoch_id == eack.collection_epoch_id);
        assert(state.identity.eack_round == eack.retry_round);
        assert(state.identity.eack_sequence == eack.eack_sequence);
        assert(state.rf_retry.key.sequence == eack.eack_sequence);
    }
}

static void test_packet_sequence_is_independent_of_saturated_backoff_round(void)
{
    const uint16_t sequences[] = {1u, 2u, 256u, UINT16_MAX};

    for (size_t i = 0u; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        struct app_gateway_eack_retry_state state = {0};
        struct gateway_collection_state eack =
            collection(33u, 44u, UINT8_MAX);

        eack.eack_sequence = sequences[i];

        assert(app_gateway_eack_retry_note_failure(&state, &eack, 1u) != 0u);
        assert(state.rf_retry.key.sequence == sequences[i]);
        assert(state.rf_retry.key.sequence != 0u);
    }
}

static void test_failed_eack_snapshot_is_exact_while_collection_changes(void)
{
    struct app_gateway_eack_retry_state state = {0};
    struct gateway_collection_state current = collection(33u, 44u, 0u);
    struct mesh_outbound original = {0};
    struct mesh_outbound rebuilt = {0};
    struct mesh_outbound restored = {0};

    assert(gateway_collection_prepare_eack_outbound(
               &current,
               EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
               &original) == PROTO_OK);
    assert(original.packet.seq == 1u);
    assert(app_gateway_eack_retry_freeze(&state,
                                         &current,
                                         &original) == PROTO_OK);
    assert(app_gateway_eack_retry_snapshot_active(&state, &current));
    assert(state.snapshot.payload_len == original.payload_len);

    collection_set_received_results(&current, 7u);
    current.collection_open = false;
    assert(gateway_collection_prepare_eack_outbound(
               &current,
               EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
               &rebuilt) == PROTO_OK);
    assert(rebuilt.payload_len > original.payload_len);
    assert(memcmp(rebuilt.payload,
                  original.payload,
                  original.payload_len) != 0);

    assert(app_gateway_eack_retry_restore(&state,
                                          &current,
                                          &restored) == PROTO_OK);
    assert(memcmp(&restored.packet,
                  &original.packet,
                  sizeof(original.packet)) == 0);
    assert(restored.payload_len == original.payload_len);
    assert(memcmp(restored.payload,
                  original.payload,
                  original.payload_len) == 0);
    assert(restored.next_hop_id == MESH_BROADCAST_ID);
    assert(restored.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(app_gateway_eack_retry_freeze(&state,
                                         &current,
                                         &rebuilt) == PROTO_ERR_MALFORMED);

    app_gateway_eack_retry_note_success(&state, &current);
    assert(!state.active);
    assert(app_gateway_eack_retry_restore(&state,
                                          &current,
                                          &restored) == PROTO_ERR_NOT_FOUND);
}

static void test_snapshot_is_scoped_to_exact_collection_round(void)
{
    struct app_gateway_eack_retry_state state = {0};
    struct gateway_collection_state first = collection(33u, 44u, 1u);
    struct gateway_collection_state next_round = collection(33u, 44u, 2u);
    struct mesh_outbound eack = {0};
    struct mesh_outbound restored = {0};

    assert(gateway_collection_prepare_eack_outbound(
               &first,
               EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
               &eack) == PROTO_OK);
    assert(app_gateway_eack_retry_freeze(&state, &first, &eack) == PROTO_OK);
    assert(app_gateway_eack_retry_restore(&state,
                                          &next_round,
                                          &restored) == PROTO_ERR_NOT_FOUND);
    assert(!app_gateway_eack_retry_snapshot_active(&state, &next_round));
}

static void test_persisted_custody_restores_exact_payload_after_reset(void)
{
    struct app_gateway_eack_retry_state before_reset = {0};
    struct app_gateway_eack_retry_state after_reset = {0};
    struct gateway_collection_state current = collection(33u, 44u, 5u);
    struct gateway_collection_eack_custody_snapshot persisted = {0};
    struct mesh_outbound original = {0};
    struct mesh_outbound restored = {0};

    current.eack_sequence = 700u;
    assert(gateway_collection_prepare_eack_outbound(
               &current,
               EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
               &original) == PROTO_OK);
    assert(app_gateway_eack_retry_freeze(&before_reset,
                                         &current,
                                         &original) == PROTO_OK);
    assert(app_gateway_eack_retry_export_custody(&before_reset,
                                                 &current,
                                                 &persisted) == PROTO_OK);
    assert(gateway_collection_eack_custody_validate(&persisted) == PROTO_OK);

    current.received_count = 7u;
    current.collection_open = false;
    after_reset.snapshot = persisted;
    assert(app_gateway_eack_retry_import_custody(&after_reset,
                                                 &current,
                                                 &after_reset.snapshot) == PROTO_OK);
    assert(app_gateway_eack_retry_restore(&after_reset,
                                          &current,
                                          &restored) == PROTO_OK);
    assert(memcmp(&restored.packet,
                  &original.packet,
                  sizeof(original.packet)) == 0);
    assert(restored.payload_len == original.payload_len);
    assert(memcmp(restored.payload,
                  original.payload,
                  original.payload_len) == 0);

    persisted.payload[0] ^= 0x01u;
    assert(gateway_collection_eack_custody_validate(&persisted) ==
           PROTO_ERR_MALFORMED);
    assert(app_gateway_eack_retry_import_custody(&after_reset,
                                                 &current,
                                                 &persisted) != PROTO_OK);
}

static void test_fresh_entropy_spreads_simultaneous_collection_retries(void)
{
    bool observed[301] = {false};
    size_t unique_delays = 0u;

    for (uint32_t collection_epoch_id = 1u;
         collection_epoch_id <= 50u;
         collection_epoch_id++) {
        struct app_gateway_eack_retry_state state = {0};
        const struct gateway_collection_state eack =
            collection(33u, collection_epoch_id, 1u);
        uint32_t delay_ms = app_gateway_eack_retry_note_failure(
            &state,
            &eack,
            UINT32_C(0xa5a50000) + collection_epoch_id);

        assert(delay_ms >= 100u && delay_ms <= 300u);
        if (!observed[delay_ms]) {
            observed[delay_ms] = true;
            unique_delays++;
        }
    }

    assert(unique_delays >= 30u);
}

static void test_success_resets_only_the_matching_logical_eack(void)
{
    struct app_gateway_eack_retry_state state = {0};
    const struct gateway_collection_state first = collection(33u, 44u, 2u);
    const struct gateway_collection_state other_collection =
        collection(33u, 45u, 2u);
    const struct gateway_collection_state other_round =
        collection(33u, 44u, 3u);

    (void)app_gateway_eack_retry_note_failure(&state, &first, 1u);
    (void)app_gateway_eack_retry_note_failure(&state, &first, 2u);
    assert(state.rf_retry.retry_round == 2u);

    app_gateway_eack_retry_note_success(&state, &other_collection);
    assert(state.active);
    assert(state.rf_retry.retry_round == 2u);
    app_gateway_eack_retry_note_success(&state, &other_round);
    assert(state.active);
    assert(state.rf_retry.retry_round == 2u);

    app_gateway_eack_retry_note_success(&state, &first);
    assert(!state.active);
    assert(state.rf_retry.retry_round == 0u);
}

static void test_commit_success_uses_frozen_identity_after_collection_advances(void)
{
    struct app_gateway_eack_retry_state state = {0};
    struct gateway_collection_state current = collection(33u, 44u, 2u);
    struct mesh_outbound eack = {0};

    assert(gateway_collection_prepare_eack_outbound(
               &current,
               EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
               &eack) == PROTO_OK);
    assert(app_gateway_eack_retry_freeze(&state, &current, &eack) == PROTO_OK);
    assert(app_gateway_eack_retry_note_failure(&state, &current, 1u) != 0u);
    assert(state.active);
    assert(state.snapshot.valid);

    assert(gateway_collection_advance_retry_round(&current) == PROTO_OK);
    app_gateway_eack_retry_note_success(&state, &current);
    assert(state.active);
    assert(state.snapshot.valid);

    assert(app_gateway_eack_retry_commit_success(&state) == PROTO_OK);
    assert(!state.active);
    assert(!state.snapshot.valid);
    assert(state.rf_retry.retry_round == 0u);
    assert(app_gateway_eack_retry_commit_success(&state) == PROTO_ERR_NOT_FOUND);
    assert(app_gateway_eack_retry_commit_success(NULL) == PROTO_ERR_ARG);

    state.active = true;
    assert(app_gateway_eack_retry_commit_success(&state) == PROTO_ERR_MALFORMED);
    assert(state.active);
}

static void test_new_collection_or_protocol_round_starts_at_rf_round_one(void)
{
    struct app_gateway_eack_retry_state state = {0};
    const struct gateway_collection_state first = collection(33u, 44u, 2u);
    const struct gateway_collection_state next_collection =
        collection(33u, 45u, 2u);
    const struct gateway_collection_state next_round =
        collection(33u, 45u, 3u);

    (void)app_gateway_eack_retry_note_failure(&state, &first, 1u);
    (void)app_gateway_eack_retry_note_failure(&state, &first, 2u);
    assert(state.rf_retry.retry_round == 2u);

    (void)app_gateway_eack_retry_note_failure(&state, &next_collection, 3u);
    assert(state.rf_retry.retry_round == 1u);
    assert(state.identity.collection_epoch_id == 45u);

    (void)app_gateway_eack_retry_note_failure(&state, &next_round, 4u);
    assert(state.rf_retry.retry_round == 1u);
    assert(state.identity.eack_round == 3u);
}

static void test_invalid_collection_does_not_create_retry_state(void)
{
    struct app_gateway_eack_retry_state state = {0};
    struct gateway_collection_state invalid = collection(33u, 44u, 2u);

    invalid.collection_epoch_id = 0u;
    assert(app_gateway_eack_retry_note_failure(&state, &invalid, 1u) == 0u);
    assert(!state.active);
    app_gateway_eack_retry_note_success(&state, &invalid);
    assert(!state.active);
}

static void test_failed_channel9_targets_are_exact_unique_and_identity_scoped(void)
{
    struct app_gateway_eack_retry_state state = {0};
    const struct gateway_collection_state first = collection(33u, 44u, 2u);
    const struct gateway_collection_state other = collection(33u, 45u, 2u);
    const uint64_t hop_a = UINT64_C(0x1111222233334444);
    const uint64_t hop_b = UINT64_C(0x2222333344445555);

    (void)app_gateway_eack_retry_note_failure(&state, &first, 1u);
    app_gateway_eack_retry_note_failed_channel9_target(&state, &first, hop_a);
    app_gateway_eack_retry_note_failed_channel9_target(&state, &first, hop_a);
    app_gateway_eack_retry_note_failed_channel9_target(&state, &first, hop_b);
    app_gateway_eack_retry_note_failed_channel9_target(&state, &other,
                                                       UINT64_C(0x3333));
    assert(state.failed_channel9_next_hop_count == 2u);
    assert(state.failed_channel9_next_hop_ids[0] == hop_a);
    assert(state.failed_channel9_next_hop_ids[1] == hop_b);
    assert(!state.force_c5_recovery);
    app_gateway_eack_retry_note_failed_channel9_target(
        &state, &first, UINT64_C(0x3333444455556666));
    assert(state.force_c5_recovery);

    app_gateway_eack_retry_note_success(&state, &first);
    assert(state.failed_channel9_next_hop_count == 0u);
    assert(!state.force_c5_recovery);
}

static void test_partial_c5_progress_is_exact_identity_scoped(void)
{
    struct app_gateway_eack_retry_state state = {0};
    const struct gateway_collection_state first = collection(33u, 44u, 2u);
    const struct gateway_collection_state other = collection(33u, 45u, 2u);

    (void)app_gateway_eack_retry_note_failure(&state, &first, 1u);
    state.c5_flood_progress.initialized = true;
    state.c5_flood_progress.next_opportunity = 3u;
    state.c5_flood_progress.result.sent_count = 3u;
    state.force_c5_recovery = true;

    (void)app_gateway_eack_retry_note_failure(&state, &first, 2u);
    assert(state.c5_flood_progress.initialized);
    assert(!state.c5_flood_progress.complete);
    assert(state.c5_flood_progress.next_opportunity == 3u);
    assert(state.c5_flood_progress.result.sent_count == 3u);
    assert(state.force_c5_recovery);

    (void)app_gateway_eack_retry_note_failure(&state, &other, 3u);
    assert(!state.c5_flood_progress.initialized);
    assert(state.c5_flood_progress.next_opportunity == 0u);
    assert(state.c5_flood_progress.result.sent_count == 0u);
    assert(!state.force_c5_recovery);
}

int main(void)
{
    test_repeated_total_failures_use_random_exponential_backoff();
    test_packet_sequence_is_independent_of_saturated_backoff_round();
    test_failed_eack_snapshot_is_exact_while_collection_changes();
    test_snapshot_is_scoped_to_exact_collection_round();
    test_persisted_custody_restores_exact_payload_after_reset();
    test_fresh_entropy_spreads_simultaneous_collection_retries();
    test_success_resets_only_the_matching_logical_eack();
    test_commit_success_uses_frozen_identity_after_collection_advances();
    test_new_collection_or_protocol_round_starts_at_rf_round_one();
    test_invalid_collection_does_not_create_retry_state();
    test_failed_channel9_targets_are_exact_unique_and_identity_scoped();
    test_partial_c5_progress_is_exact_identity_scoped();
    return 0;
}
