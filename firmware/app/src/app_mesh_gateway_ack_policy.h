#ifndef APP_MESH_GATEWAY_ACK_POLICY_H
#define APP_MESH_GATEWAY_ACK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_gateway_ack_action {
    APP_MESH_GATEWAY_ACK_ACTION_NONE = 0,
    APP_MESH_GATEWAY_ACK_ACTION_QUEUE_ROUTE_TEST_ACK,
    APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9,
    APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9,
    APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY,
    APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9,
};

struct app_mesh_gateway_ack_state {
    bool route_test_enabled;
    bool gateway_role;
    bool received_on_channel9;
    bool current_channel9_attempted;
    int current_channel9_ret;
    int channel9_require_ret;
    uint32_t channel9_retry_delay_ms;
};

struct app_mesh_gateway_ack_decision {
    enum app_mesh_gateway_ack_action action;
    const char *reason;
    uint32_t delay_ms;
};

void app_mesh_gateway_ack_decide(
    const struct app_mesh_gateway_ack_state *state,
    struct app_mesh_gateway_ack_decision *decision);

#endif
