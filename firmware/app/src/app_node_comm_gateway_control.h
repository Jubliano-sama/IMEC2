#ifndef APP_NODE_COMM_GATEWAY_CONTROL_H
#define APP_NODE_COMM_GATEWAY_CONTROL_H

#include "app_node_comm_gateway_abort.h"
#include "app_mesh_radio_client.h"

#include <stdbool.h>

struct app_mesh_command_orchestrator;
struct app_mesh_flood_result;
struct k_work_delayable;
struct k_work_q;

typedef int (*app_node_comm_gateway_control_flood_fn)(
    void *ctx,
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    struct app_mesh_flood_result *result);
typedef void (*app_node_comm_gateway_control_priority_observer_fn)(
    void *ctx,
    int result);

struct app_node_comm_gateway_control_config {
    bool gateway_role;
    struct k_work_q *priority_work_queue;
    app_node_comm_gateway_control_flood_fn send_flood;
    app_node_comm_gateway_control_priority_observer_fn priority_observer;
    void *ctx;
};

void app_node_comm_gateway_control_init(
    const struct app_node_comm_gateway_control_config *config);
struct app_mesh_command_orchestrator *app_node_comm_gateway_control_context(void);
int app_node_comm_gateway_control_send(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now);
int app_node_comm_gateway_control_radio_handoff_submit(
    struct k_work_delayable *work,
    app_mesh_radio_owner_schedule_failure_fn schedule_failure,
    void *schedule_failure_ctx,
    uint32_t schedule_failure_token);
int app_node_comm_gateway_control_radio_handoff_begin(
    struct k_work_delayable *work);
int app_node_comm_gateway_control_radio_handoff_cancel(
    struct k_work_delayable *work);
int app_node_comm_gateway_control_safe_boundary_schedule(void *ctx);

#endif
