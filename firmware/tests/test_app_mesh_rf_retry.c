#include "app_mesh_rf_retry.h"
#include "app_mesh_route_request_policy.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>

static struct app_mesh_rf_retry_key retry_key(uint64_t source_id,
                                               uint32_t sequence,
                                               uint8_t operation)
{
    return (struct app_mesh_rf_retry_key) {
        .source_id = source_id,
        .destination_id = UINT64_C(0x2222222222222222),
        .session_id = 0x12345678u,
        .sequence = sequence,
        .message_type = 0x15u,
        .operation = operation,
    };
}

static void test_reliable_data_backoff_advances_and_caps(void)
{
    struct app_mesh_rf_retry_state state = {0};
    const struct app_mesh_rf_retry_key key = retry_key(
        UINT64_C(0x1111111111111111),
        7u,
        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
    const uint32_t minimums[] = {25u, 50u, 100u, 200u, 200u, 200u};
    const uint32_t maximums[] = {75u, 150u, 300u, 600u, 600u, 600u};

    for (size_t i = 0u; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
        uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
            &state, &key, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            UINT32_C(0x1000) + (uint32_t)i);

        assert(delay_ms >= minimums[i]);
        assert(delay_ms <= maximums[i]);
        assert(state.retry_round == i + 1u);
    }
}

static void test_wake_backoff_stays_inside_contract_bounds(void)
{
    struct app_mesh_rf_retry_state state = {0};
    const struct app_mesh_rf_retry_key key = retry_key(
        UINT64_C(0x1111111111111111),
        0u,
        APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE);
    const uint32_t minimums[] = {200u, 400u, 800u, 1600u, 1600u};
    const uint32_t maximums[] = {399u, 799u, 1599u, 2000u, 2000u};

    for (size_t i = 0u; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
        uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
            &state, &key, APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
            UINT32_C(0x2000) + (uint32_t)i);

        assert(delay_ms >= minimums[i]);
        assert(delay_ms <= maximums[i]);
        assert(state.retry_round == i + 1u);
    }
}

static void test_identity_change_and_success_restart_the_round(void)
{
    struct app_mesh_rf_retry_state state = {0};
    const struct app_mesh_rf_retry_key first = retry_key(
        UINT64_C(0x1111111111111111),
        7u,
        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
    const struct app_mesh_rf_retry_key second = retry_key(
        UINT64_C(0x1111111111111111),
        8u,
        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

    (void)app_mesh_rf_retry_next_delay_ms(
        &state, &first, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA, 1u);
    (void)app_mesh_rf_retry_next_delay_ms(
        &state, &first, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA, 2u);
    assert(state.retry_round == 2u);

    (void)app_mesh_rf_retry_next_delay_ms(
        &state, &second, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA, 3u);
    assert(state.retry_round == 1u);
    app_mesh_rf_retry_note_success(&state, &first);
    assert(state.active);
    assert(state.retry_round == 1u);
    app_mesh_rf_retry_note_success(&state, &second);
    assert(!state.active);
    assert(state.retry_round == 0u);

    (void)app_mesh_rf_retry_next_delay_ms(
        &state, &second, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA, 4u);
    assert(state.retry_round == 1u);
}

static void test_many_simultaneous_nodes_do_not_share_one_retry_delay(void)
{
    bool observed[76] = {false};
    size_t unique_delays = 0u;

    for (uint64_t node = 1u; node <= 64u; node++) {
        struct app_mesh_rf_retry_state state = {0};
        const struct app_mesh_rf_retry_key key = retry_key(
            UINT64_C(0xabc0000000000000) + node,
            7u,
            APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
            &state, &key, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            UINT32_C(0xa5a5a5a5));

        assert(delay_ms >= 25u && delay_ms <= 75u);
        if (!observed[delay_ms]) {
            observed[delay_ms] = true;
            unique_delays++;
        }
    }

    assert(unique_delays >= 20u);
}

static void test_same_identity_and_entropy_replay_deterministically(void)
{
    struct app_mesh_rf_retry_state first_state = {0};
    struct app_mesh_rf_retry_state second_state = {0};
    const struct app_mesh_rf_retry_key key = retry_key(
        UINT64_C(0x1111111111111111),
        7u,
        APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);

    for (size_t i = 0u; i < 8u; i++) {
        assert(app_mesh_rf_retry_next_delay_ms(
                   &first_state, &key,
                   APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                   UINT32_C(0x3000) + (uint32_t)i) ==
               app_mesh_rf_retry_next_delay_ms(
                   &second_state, &key,
                   APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                   UINT32_C(0x3000) + (uint32_t)i));
    }
}

static void test_fresh_entropy_changes_same_identity_and_round(void)
{
    bool reliable_observed[76] = {false};
    bool capped_wake_observed[2001] = {false};
    size_t reliable_unique = 0u;
    size_t capped_wake_unique = 0u;
    const struct app_mesh_rf_retry_key reliable_key = retry_key(
        UINT64_C(0x1111111111111111),
        7u,
        APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);
    const struct app_mesh_rf_retry_key wake_key = retry_key(
        UINT64_C(0x1111111111111111),
        0u,
        APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE);

    for (uint32_t entropy = 1u; entropy <= 64u; entropy++) {
        struct app_mesh_rf_retry_state reliable_state = {0};
        struct app_mesh_rf_retry_state wake_state = {0};
        uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
            &reliable_state,
            &reliable_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            entropy);

        assert(delay_ms >= 25u && delay_ms <= 75u);
        if (!reliable_observed[delay_ms]) {
            reliable_observed[delay_ms] = true;
            reliable_unique++;
        }

        for (uint32_t round = 0u; round < 5u; round++) {
            delay_ms = app_mesh_rf_retry_next_delay_ms(
                &wake_state,
                &wake_key,
                APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
                round == 4u ? entropy : UINT32_C(0x4000) + round);
        }
        assert(delay_ms >= 1600u && delay_ms <= 2000u);
        if (!capped_wake_observed[delay_ms]) {
            capped_wake_observed[delay_ms] = true;
            capped_wake_unique++;
        }
    }

    assert(reliable_unique >= 20u);
    assert(capped_wake_unique >= 20u);
}

static void test_wake_success_does_not_reset_control_deferral_round(void)
{
    struct app_mesh_rf_retry_state wake_state = {0};
    struct app_mesh_rf_retry_state control_state = {0};
    const struct app_mesh_rf_retry_key wake_key = retry_key(
        UINT64_C(0x1111111111111111),
        0u,
        APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE);
    const struct app_mesh_rf_retry_key control_key = retry_key(
        UINT64_C(0x1111111111111111),
        0u,
        APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_CONTROL);
    const uint32_t minimums[] = {200u, 400u, 800u};
    const uint32_t maximums[] = {399u, 799u, 1599u};
    uint8_t protocol_attempts = 0u;

    for (size_t i = 0u; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
        struct app_mesh_route_request_rf_failure_decision decision;
        uint32_t delay_ms;

        app_mesh_rf_retry_note_success(&wake_state, &wake_key);
        protocol_attempts++;
        app_mesh_route_request_rf_failure_decide(
            APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
            -EBUSY,
            false,
            -EBUSY,
            &decision);
        assert(decision.defer_retry);
        assert(decision.restore_prepared_attempt);
        if (decision.restore_prepared_attempt) {
            protocol_attempts--;
        }

        delay_ms = app_mesh_rf_retry_next_delay_ms(
            &control_state,
            &control_key,
            APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
            UINT32_C(0x7000) + (uint32_t)i);
        assert(delay_ms >= minimums[i]);
        assert(delay_ms <= maximums[i]);
        assert(control_state.retry_round == i + 1u);
        assert(protocol_attempts == 0u);
    }
}

static void test_control_flood_first_deferral_is_random_exponential(void)
{
    struct app_mesh_rf_retry_state state = {0};
    const struct app_mesh_rf_retry_key key = retry_key(
        UINT64_C(0x1111111111111111),
        19u,
        APP_MESH_RF_RETRY_OPERATION_CONTROL_FLOOD);
    const uint32_t minimums[] = {100u, 200u, 400u, 800u, 800u};
    const uint32_t maximums[] = {300u, 600u, 1200u, 2400u, 2400u};

    for (size_t i = 0u; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
        uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
            &state,
            &key,
            APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
            UINT32_C(0x8100) + (uint32_t)i);

        assert(delay_ms >= minimums[i]);
        assert(delay_ms <= maximums[i]);
        assert(state.retry_round == i + 1u);
    }
}

static void test_four_interleaved_packets_keep_independent_rounds(void)
{
    struct app_mesh_rf_retry_state states[4] = {0};
    struct app_mesh_rf_retry_bank bank = {
        .states = states,
        .state_count = sizeof(states) / sizeof(states[0]),
    };
    struct app_mesh_rf_retry_key keys[4];

    for (size_t i = 0u; i < sizeof(keys) / sizeof(keys[0]); i++) {
        keys[i] = retry_key(UINT64_C(0x1111111111111111),
                            (uint32_t)i + 1u,
                            APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        assert(app_mesh_rf_retry_bank_next_delay_ms(
                   &bank, &keys[i],
                   APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                   UINT32_C(0x5000) + (uint32_t)i) >= 25u);
    }
    for (size_t i = 0u; i < sizeof(keys) / sizeof(keys[0]); i++) {
        uint32_t delay_ms = app_mesh_rf_retry_bank_next_delay_ms(
            &bank, &keys[i], APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            UINT32_C(0x6000) + (uint32_t)i);

        assert(delay_ms >= 50u && delay_ms <= 150u);
        assert(states[i].retry_round == 2u);
    }

    app_mesh_rf_retry_bank_note_success(&bank, &keys[1]);
    assert(!states[1].active);
    for (size_t i = 0u; i < sizeof(states) / sizeof(states[0]); i++) {
        if (i != 1u) {
            assert(states[i].active);
            assert(states[i].retry_round == 2u);
        }
    }
    app_mesh_rf_retry_bank_reset(&bank);
    for (size_t i = 0u; i < sizeof(states) / sizeof(states[0]); i++) {
        assert(!states[i].active);
    }
}

static void test_invalid_input_fails_without_mutating_state(void)
{
    struct app_mesh_rf_retry_state state = {0};
    struct app_mesh_rf_retry_key key = retry_key(
        0u,
        7u,
        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

    assert(app_mesh_rf_retry_next_delay_ms(
               &state, &key, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
               1u) == 0u);
    assert(!state.active);
    assert(app_mesh_rf_retry_next_delay_ms(
               NULL, &key, APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
               1u) == 0u);
}

int main(void)
{
    test_reliable_data_backoff_advances_and_caps();
    test_wake_backoff_stays_inside_contract_bounds();
    test_identity_change_and_success_restart_the_round();
    test_many_simultaneous_nodes_do_not_share_one_retry_delay();
    test_same_identity_and_entropy_replay_deterministically();
    test_fresh_entropy_changes_same_identity_and_round();
    test_wake_success_does_not_reset_control_deferral_round();
    test_control_flood_first_deferral_is_random_exponential();
    test_four_interleaved_packets_keep_independent_rounds();
    test_invalid_input_fails_without_mutating_state();
    return 0;
}
