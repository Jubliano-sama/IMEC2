#ifndef APP_MESH_DIRECT_GATEWAY_RETRY_H
#define APP_MESH_DIRECT_GATEWAY_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS 3u
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MIN_MS 30u
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_MS 50u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS 4u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_DEFERRALS 8u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_BASE_MS 20u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS 2000u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS 20u
#define APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS 10u
#define APP_MESH_DIRECT_GATEWAY_ACK_RX_MS 250u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS 500u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_TRANSITION_GUARD_MS 250u
#define APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_MAX_TOTAL_MS \
    (2u * (APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS + \
           (APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS << 1u) + \
           (APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_BASE_MS << 2u)))
#define APP_MESH_DIRECT_GATEWAY_SURVEY_POLICY_HORIZON_MS(attempt_ms, busy_eval_ms) \
    ((APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS * (attempt_ms)) + \
     APP_MESH_DIRECT_GATEWAY_SURVEY_RETRY_MAX_TOTAL_MS + \
     (APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_DEFERRALS * \
      ((busy_eval_ms) + APP_MESH_DIRECT_GATEWAY_SURVEY_BUSY_BASE_MS * 2u)))
#define APP_MESH_DIRECT_GATEWAY_SURVEY_DELIVERY_TAIL_MS( \
    attempt_ms, busy_eval_ms, ack_budget_ms) \
    (APP_MESH_DIRECT_GATEWAY_SURVEY_POLICY_HORIZON_MS( \
         (attempt_ms), (busy_eval_ms)) + \
     (ack_budget_ms) + APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS + \
     APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS)

enum app_mesh_direct_gateway_retry_mode {
    APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE = 0,
    APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY,
};

enum app_mesh_direct_gateway_attempt_outcome {
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS = 0,
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED,
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY,
};

struct app_mesh_direct_gateway_retry_state {
    uint64_t anchor_id;
    uint32_t survey_id;
    uint8_t attempts;
    uint8_t busy_deferrals;
    enum app_mesh_direct_gateway_retry_mode mode;
};

struct app_mesh_direct_gateway_retry_decision {
    uint32_t delay_ms;
    bool retry;
    bool attempt_consumed;
    bool exhausted;
    bool busy_exhausted;
};

int app_mesh_direct_gateway_retry_init(
    struct app_mesh_direct_gateway_retry_state *state,
    enum app_mesh_direct_gateway_retry_mode mode,
    uint64_t anchor_id,
    uint32_t survey_id);
int app_mesh_direct_gateway_retry_note(
    struct app_mesh_direct_gateway_retry_state *state,
    enum app_mesh_direct_gateway_attempt_outcome outcome,
    uint32_t random_value,
    struct app_mesh_direct_gateway_retry_decision *decision);
uint32_t app_mesh_direct_gateway_retry_policy_horizon_ms(
    enum app_mesh_direct_gateway_retry_mode mode,
    uint32_t attempt_ms,
    uint32_t busy_evaluation_ms);
bool app_mesh_direct_gateway_retry_deadline_reached(uint32_t now_ms,
                                                    uint32_t deadline_ms);
uint32_t app_mesh_direct_gateway_retry_deadline_remaining_ms(
    uint32_t now_ms,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
