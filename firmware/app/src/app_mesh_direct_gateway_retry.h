#ifndef APP_MESH_DIRECT_GATEWAY_RETRY_H
#define APP_MESH_DIRECT_GATEWAY_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS 3u
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS 30u
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MIN_MS     APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS 60u
#define APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_MS     (APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_BASE_MS * 2u)
#define APP_MESH_DIRECT_GATEWAY_SERVICE_GUARD_MS 20u
#define APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS 2u
/*
 * The gateway listens continuously and answers a direct channel-5 uplink from
 * its own receive turn: bench captures place the ACK about 15 ms after the
 * frame ends. Two full receive slices plus slop bound that turnaround, and
 * every millisecond beyond it is dead air the sender holds the radio for.
 */
#define APP_MESH_DIRECT_GATEWAY_ACK_RX_MS 60u
/*
 * A parent anchor is not a gateway: it is low-duty scanning, and a queued
 * mesh RX handoff waits up to ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS before
 * its route thread can answer with MSG_MESH_HOP_ACK. The parent-hop uplink
 * therefore keeps the original quarter-second wait.
 */
#define APP_MESH_PARENT_HOP_ACK_RX_MS 250u

enum app_mesh_direct_gateway_retry_mode {
    APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE = 0,
};

enum app_mesh_direct_gateway_attempt_outcome {
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS = 0,
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED,
    APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY,
};

struct app_mesh_direct_gateway_retry_state {
    uint64_t anchor_id;
    uint32_t context_id;
    uint8_t attempts;
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
    uint32_t context_id);
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
