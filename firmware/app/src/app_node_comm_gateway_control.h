#ifndef APP_NODE_COMM_GATEWAY_CONTROL_H
#define APP_NODE_COMM_GATEWAY_CONTROL_H

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
int app_node_comm_gateway_control_priority_submit(
    struct k_work_delayable *work);
int app_node_comm_gateway_control_safe_boundary_schedule(void *ctx);

#endif
