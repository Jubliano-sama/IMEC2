#include "app_mesh_direct_gateway_retry.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ANCHOR_ID UINT64_C(0x1111111111111111)
#define CONTEXT_ID UINT32_C(0x12345678)

static struct app_mesh_direct_gateway_retry_state retry_state(void)
{
    struct app_mesh_direct_gateway_retry_state state;

    assert(app_mesh_direct_gateway_retry_init(
        &state,
        APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,
        ANCHOR_ID,
        CONTEXT_ID) == 0);
    return state;
}

static void assert_rf_busy_defers_without_attempt(
    struct app_mesh_direct_gateway_retry_state *state,
    uint8_t expected_attempts,
    uint32_t random_value)
{
    struct app_mesh_direct_gateway_retry_decision decision;

    assert(app_mesh_direct_gateway_retry_note(
        state,
        APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY,
        random_value,
        &decision) == 0);
    assert(state->attempts == expected_attempts);
    assert(!decision.attempt_consumed);
    assert(decision.retry);
    assert(!decision.exhausted);
    assert(!decision.busy_exhausted);
}

static void test_rf_busy_does_not_consume_or_exhaust_real_attempts(void)
{
    struct app_mesh_direct_gateway_retry_state state = retry_state();

    for (uint8_t busy = 0u;
         busy < APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS + 2u;
         busy++) {
        assert_rf_busy_defers_without_attempt(&state, 0u, busy);
    }
}

static void test_real_failures_consume_and_exhaust_three_attempts(void)
{
    struct app_mesh_direct_gateway_retry_state state = retry_state();

    for (uint8_t attempt = 1u;
         attempt <= APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS;
         attempt++) {
        struct app_mesh_direct_gateway_retry_decision decision;

        assert(app_mesh_direct_gateway_retry_note(
            &state,
            APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED,
            attempt,
            &decision) == 0);
        assert(state.attempts == attempt);
        assert(decision.attempt_consumed);
        assert(decision.retry ==
               (attempt < APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS));
        assert(decision.exhausted ==
               (attempt == APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS));
        assert(!decision.busy_exhausted);
        if (attempt < APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS) {
            assert_rf_busy_defers_without_attempt(
                &state, attempt, UINT32_C(0xa5a50000) + attempt);
        }
    }
}

static void test_success_consumes_one_attempt_and_stops_retrying(void)
{
    struct app_mesh_direct_gateway_retry_state state = retry_state();
    struct app_mesh_direct_gateway_retry_decision decision;

    assert(app_mesh_direct_gateway_retry_note(
        &state,
        APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS,
        0u,
        &decision) == 0);
    assert(state.attempts == 1u);
    assert(decision.attempt_consumed);
    assert(!decision.retry);
    assert(!decision.exhausted);
    assert(!decision.busy_exhausted);
}

int main(void)
{
    test_rf_busy_does_not_consume_or_exhaust_real_attempts();
    test_real_failures_consume_and_exhaust_three_attempts();
    test_success_consumes_one_attempt_and_stops_retrying();
    puts("app mesh direct gateway retry tests passed");
    return 0;
}
