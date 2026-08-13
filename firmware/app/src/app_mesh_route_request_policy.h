#ifndef APP_MESH_ROUTE_REQUEST_POLICY_H
#define APP_MESH_ROUTE_REQUEST_POLICY_H

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_route_request_policy_state {
    bool relay_required;
    bool direct_bulk_suppressed;
    int direct_probe_ret;
    uint8_t required_gateway_relay_hops;
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

enum app_mesh_route_request_defer_action {
    APP_MESH_ROUTE_REQUEST_DEFER_REJECT = 0,
    APP_MESH_ROUTE_REQUEST_DEFER_ACCEPT_NEW,
    APP_MESH_ROUTE_REQUEST_DEFER_REPLACE_SAME,
};

struct app_mesh_route_request_defer_state {
    uint32_t pending_due_ms;
    uint32_t requested_due_ms;
    uint32_t pending_reply_deadline_ms;
    uint32_t requested_reply_deadline_ms;
    bool pending;
    bool same_identity;
    bool update_only;
};

struct app_mesh_route_request_defer_decision {
    enum app_mesh_route_request_defer_action action;
    uint32_t due_ms;
    uint32_t reply_deadline_ms;
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

void app_mesh_route_request_defer_decide(
    const struct app_mesh_route_request_defer_state *state,
    struct app_mesh_route_request_defer_decision *decision);
uint32_t app_mesh_route_request_defer_delay_ms(uint32_t now_ms,
                                               uint32_t due_ms);
bool app_mesh_gateway_control_relay_hops_allowed(
    uint8_t origin_ttl,
    uint8_t packet_ttl,
    uint8_t required_gateway_relay_hops);

#endif
