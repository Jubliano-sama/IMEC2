#ifndef APP_MESH_RADIO_OWNER_H
#define APP_MESH_RADIO_OWNER_H

#include "app_mesh_radio_client.h"

#include <zephyr/kernel.h>

#include <stdbool.h>

int app_mesh_radio_owner_pause(
    struct app_mesh_radio_owner_pause_lease *lease_in_out);
int app_mesh_radio_owner_resume(
    struct app_mesh_radio_owner_pause_lease *lease_in_out);
bool app_mesh_radio_owner_paused(void);

bool app_mesh_radio_owner_abort_pending(void);

struct app_mesh_radio_owner_gateway_ops {
    bool gateway_role;
    struct k_work_q *priority_work_queue;
    app_mesh_radio_owner_schedule_failure_fn schedule_failure;
    void *schedule_failure_ctx;
    uint32_t schedule_failure_token;
};

struct app_mesh_radio_owner_platform_ops {
    void (*enter_uwb_quiet)(const char *reason);
    void (*exit_uwb_quiet)(const char *reason);
    void (*request_receive_abort)(void);
    void (*clear_receive_abort)(void);
};

int app_mesh_radio_owner_init(
    const struct app_mesh_radio_owner_platform_ops *ops);
int app_mesh_radio_owner_gateway_command_submit(
    const struct app_mesh_radio_owner_gateway_ops *ops,
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_out);
int app_mesh_radio_owner_gateway_safe_boundary(
    struct app_mesh_radio_owner_handoff_lease *lease_in_out);
int app_mesh_radio_owner_gateway_command_begin(
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_in_out);
int app_mesh_radio_owner_gateway_command_cancel(
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_in_out);
#endif
