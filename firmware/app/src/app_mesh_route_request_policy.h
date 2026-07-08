#ifndef APP_MESH_ROUTE_REQUEST_POLICY_H
#define APP_MESH_ROUTE_REQUEST_POLICY_H

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_route_request_policy_state {
    bool relay_required;
    bool direct_bulk_suppressed;
    int direct_probe_ret;
};

struct app_mesh_route_request_policy_decision {
    bool install_direct_route_from_probe;
    bool direct_probe_satisfies_request;
    uint8_t route_request_flags;
};

enum app_mesh_route_request_rf_phase {
    APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN,
    APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
};

struct app_mesh_route_request_rf_failure_decision {
    bool defer_retry;
    bool restore_prepared_attempt;
};

void app_mesh_route_request_policy_decide(
    const struct app_mesh_route_request_policy_state *state,
    struct app_mesh_route_request_policy_decision *decision);

void app_mesh_route_request_rf_failure_decide(
    enum app_mesh_route_request_rf_phase phase,
    int ret,
    bool embedded_route_sent,
    int radio_busy_ret,
    struct app_mesh_route_request_rf_failure_decision *decision);

#endif
