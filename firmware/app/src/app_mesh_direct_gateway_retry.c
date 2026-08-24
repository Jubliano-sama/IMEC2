#include "app_mesh_direct_gateway_retry.h"

#include <stddef.h>
#include <string.h>

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
    uint32_t context_id)
{
    if (state == NULL || mode != APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    state->mode = mode;
    state->anchor_id = anchor_id;
    state->context_id = context_id;
    return 0;
}

int app_mesh_direct_gateway_retry_note(
    struct app_mesh_direct_gateway_retry_state *state,
    enum app_mesh_direct_gateway_attempt_outcome outcome,
    uint32_t random_value,
    struct app_mesh_direct_gateway_retry_decision *decision)
{
    uint32_t base_ms;

    if (state == NULL || decision == NULL ||
        state->mode != APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE ||
        outcome > APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY) {
        return -1;
    }
    memset(decision, 0, sizeof(*decision));
    state->attempts++;
    decision->attempt_consumed = true;
    if (outcome == APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS) {
        return 0;
    }
    if (state->attempts >= APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS) {
        decision->exhausted = true;
        return 0;
    }
    base_ms = exponential_base_ms(
        APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS,
        APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS,
        (uint8_t)(state->attempts - 1u));
    decision->delay_ms = base_ms + random_value % (base_ms + 1u);
    decision->retry = true;
    return 0;
}

uint32_t app_mesh_direct_gateway_retry_policy_horizon_ms(
    enum app_mesh_direct_gateway_retry_mode mode,
    uint32_t attempt_ms,
    uint32_t busy_evaluation_ms)
{
    (void)busy_evaluation_ms;
    if (mode != APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE) {
        return 0u;
    }
    return APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS * attempt_ms +
        2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS +
        2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS;
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
