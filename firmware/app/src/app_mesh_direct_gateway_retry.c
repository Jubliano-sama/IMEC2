#include "app_mesh_direct_gateway_retry.h"

#include <stddef.h>
#include <string.h>

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint32_t survey_random(const struct app_mesh_direct_gateway_retry_state *state,
                              uint8_t ordinal)
{
    uint64_t value = state->anchor_id ^ ((uint64_t)state->survey_id << 19) ^
                     ((uint64_t)(ordinal + 1u) *
                      UINT64_C(0x9e3779b97f4a7c15));
    uint64_t mixed = mix64(value);

    return (uint32_t)(mixed ^ (mixed >> 32));
}

static uint32_t exponential_base_ms(uint32_t initial_ms,
                                    uint32_t maximum_ms,
                                    uint8_t retry_round)
{
    uint32_t base_ms = initial_ms;

    for (uint8_t round = 0u; round < retry_round; round++) {
        if (base_ms >= maximum_ms / 2u) {
            return maximum_ms;
        }
        base_ms *= 2u;
    }
    return base_ms > maximum_ms ? maximum_ms : base_ms;
}

int app_mesh_direct_gateway_retry_init(
    struct app_mesh_direct_gateway_retry_state *state,
    enum app_mesh_direct_gateway_retry_mode mode,
    uint64_t anchor_id,
    uint32_t survey_id)
{
    if (state == NULL || mode > APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY ||
        (mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY &&
         (anchor_id == 0u || survey_id == 0u))) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->mode = mode;
    state->anchor_id = anchor_id;
    state->survey_id = survey_id;
    return 0;
}

int app_mesh_direct_gateway_retry_note(
    struct app_mesh_direct_gateway_retry_state *state,
    enum app_mesh_direct_gateway_attempt_outcome outcome,
    uint32_t random_value,
    struct app_mesh_direct_gateway_retry_decision *decision)
{
    uint8_t max_attempts;

    if (state == NULL || decision == NULL ||
        outcome > APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY) {
        return -1;
    }
    memset(decision, 0, sizeof(*decision));
    if (outcome == APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS) {
        state->attempts++;
        decision->attempt_consumed = true;
        return 0;
    }
    if (state->mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY &&
        outcome == APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY) {
        if (state->busy_deferrals >= APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_DEFERRALS) {
            decision->exhausted = true;
            decision->busy_exhausted = true;
            return 0;
        }
        uint32_t base_ms = exponential_base_ms(
            APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_BASE_MS,
            APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_MAX_BASE_MS,
            state->busy_deferrals);

        decision->delay_ms = base_ms +
            survey_random(state, state->busy_deferrals) % (base_ms + 1u);
        state->busy_deferrals++;
        decision->retry = true;
        return 0;
    }

    state->attempts++;
    decision->attempt_consumed = true;
    max_attempts = state->mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY ?
                   APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS :
                   APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS;
    if (state->attempts >= max_attempts) {
        decision->exhausted = true;
        return 0;
    }
    decision->retry = true;
    if (state->mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
        uint32_t base_ms = APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS <<
                           (state->attempts - 1u);

        decision->delay_ms = base_ms +
            survey_random(state, state->attempts) % (base_ms + 1u);
    } else {
        uint32_t base_ms = exponential_base_ms(
            APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS,
            APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS,
            (uint8_t)(state->attempts - 1u));

        decision->delay_ms = base_ms + random_value % (base_ms + 1u);
    }
    return 0;
}

uint32_t app_mesh_direct_gateway_retry_policy_horizon_ms(
    enum app_mesh_direct_gateway_retry_mode mode,
    uint32_t attempt_ms,
    uint32_t busy_evaluation_ms)
{
    if (mode == APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE) {
        return APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS * attempt_ms +
            2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS +
            2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS;
    }
    if (mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
        return APP_MESH_DIRECT_GATEWAY_SURVEY_POLICY_HORIZON_MS(
            attempt_ms, busy_evaluation_ms);
    }
    return 0u;
}

bool app_mesh_direct_gateway_retry_deadline_reached(uint32_t now_ms,
                                                    uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

uint32_t app_mesh_direct_gateway_retry_deadline_remaining_ms(
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    return app_mesh_direct_gateway_retry_deadline_reached(now_ms, deadline_ms) ?
           0u : deadline_ms - now_ms;
}
