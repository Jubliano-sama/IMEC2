#ifndef APP_MESH_ROUTE_WAIT_TX_H
#define APP_MESH_ROUTE_WAIT_TX_H

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_route_wait_tx_action {
    APP_MESH_ROUTE_WAIT_TX_ACTION_NONE = 0,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE,
    APP_MESH_ROUTE_WAIT_TX_ACTION_DROP,
    APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID,
};

struct app_mesh_route_wait_tx_state {
    bool outbound_ready;
    int tx_ret;
    bool route_request_attempted;
    int route_request_ret;
    uint32_t channel9_retry_delay_ms;
    uint32_t busy_retry_delay_ms;
};

struct app_mesh_route_wait_tx_decision {
    enum app_mesh_route_wait_tx_action action;
    const char *reason;
    uint32_t delay_ms;
};

void app_mesh_route_wait_tx_decide(
    const struct app_mesh_route_wait_tx_state *state,
    struct app_mesh_route_wait_tx_decision *decision);

#endif
