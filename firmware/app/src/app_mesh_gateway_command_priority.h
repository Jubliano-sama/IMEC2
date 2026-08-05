#ifndef APP_MESH_GATEWAY_COMMAND_PRIORITY_H
#define APP_MESH_GATEWAY_COMMAND_PRIORITY_H

#include <stdbool.h>
#include <stdint.h>

#define APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS 8u

enum app_mesh_gateway_command_priority_state {
    APP_MESH_GATEWAY_COMMAND_PRIORITY_IDLE = 0,
    APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY,
    APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SCHEDULE_RETRY,
};

struct app_mesh_gateway_command_priority {
    enum app_mesh_gateway_command_priority_state state;
    void *work;
    uint32_t next_generation;
    uint32_t generation;
    uint32_t admission_cutoff;
    uint8_t schedule_attempts;
};

struct app_mesh_gateway_command_priority_failure {
    uint32_t generation;
    uint32_t admission_cutoff;
};

/*
 * Gateway command ingress must abort an in-progress receive before it queues
 * command work. The callbacks keep that ordering independently testable while
 * the Zephyr runtime supplies the concrete radio and workqueue operations.
 */
struct app_mesh_gateway_command_priority_ops {
    bool gateway_role;
    void (*request_receive_abort)(void *ctx);
    int (*reschedule_now)(void *ctx, void *work);
    void (*clear_receive_abort)(void *ctx);
    void *ctx;
};

void app_mesh_gateway_command_priority_reset(
    struct app_mesh_gateway_command_priority *priority);

int app_mesh_gateway_command_priority_request(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    void *work,
    uint32_t admission_cutoff);

int app_mesh_gateway_command_priority_acknowledge_safe_boundary(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    struct app_mesh_gateway_command_priority_failure *failure);

int app_mesh_gateway_command_priority_retry_schedule(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    struct app_mesh_gateway_command_priority_failure *failure);

bool app_mesh_gateway_command_priority_waiting_for_safe_boundary(
    const struct app_mesh_gateway_command_priority *priority);

bool app_mesh_gateway_command_priority_schedule_retry_pending(
    const struct app_mesh_gateway_command_priority *priority);

bool app_mesh_gateway_command_priority_active(
    const struct app_mesh_gateway_command_priority *priority);

#endif
