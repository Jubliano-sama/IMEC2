#include "app_node_comm_gateway_control.h"

#include "app_mesh_command_orchestrator.h"
#include "app_mesh_flood.h"
#include "app_mesh_radio_owner.h"

#include <errno.h>
#include <string.h>

static struct app_mesh_command_orchestrator gateway_control_orchestrator;
static struct app_node_comm_gateway_control_config gateway_control_config;
static struct app_mesh_radio_owner_handoff_lease gateway_radio_handoff;
static struct app_mesh_radio_owner_abort_lease gateway_preemptive_abort;

void app_node_comm_gateway_control_init(
    const struct app_node_comm_gateway_control_config *config)
{
    if (config == NULL) {
        memset(&gateway_control_config, 0, sizeof(gateway_control_config));
        return;
    }
    gateway_control_config = *config;
}

struct app_mesh_command_orchestrator *app_node_comm_gateway_control_context(void)
{
    return &gateway_control_orchestrator;
}

int app_node_comm_gateway_control_send(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now)
{
    struct app_mesh_flood_result result = {0};
    int ret;

    if (orchestrator == NULL || !orchestrator->command_admitted ||
        gateway_control_config.send_flood == NULL) {
        return -EINVAL;
    }
    if (sent_now != NULL) {
        *sent_now = false;
    }
    ret = gateway_control_config.send_flood(gateway_control_config.ctx,
                                            orchestrator,
                                            reason,
                                            &result);
    if (result.sent_count > 0u && sent_now != NULL) {
        *sent_now = true;
    }
    return ret;
}

int app_node_comm_gateway_control_radio_handoff_submit(
    struct k_work_delayable *work,
    app_mesh_radio_owner_schedule_failure_fn schedule_failure,
    void *schedule_failure_ctx,
    uint32_t schedule_failure_token)
{
    const struct app_mesh_radio_owner_gateway_ops ops = {
        .gateway_role = gateway_control_config.gateway_role,
        .priority_work_queue = gateway_control_config.priority_work_queue,
        .schedule_failure = schedule_failure,
        .schedule_failure_ctx = schedule_failure_ctx,
        .schedule_failure_token = schedule_failure_token,
    };
    int ret = app_mesh_radio_owner_gateway_command_submit(
        &ops, work, &gateway_radio_handoff);

    if (gateway_control_config.priority_observer != NULL) {
        gateway_control_config.priority_observer(gateway_control_config.ctx,
                                                 ret);
    }
    return ret;
}

int app_node_comm_gateway_control_safe_boundary_schedule(void *ctx)
{
    (void)ctx;
    return app_mesh_radio_owner_gateway_safe_boundary(
        &gateway_radio_handoff);
}

int app_node_comm_gateway_control_radio_handoff_begin(
    struct k_work_delayable *work)
{
    return app_mesh_radio_owner_gateway_command_begin(
        work, &gateway_radio_handoff);
}

int app_node_comm_gateway_control_radio_handoff_cancel(
    struct k_work_delayable *work)
{
    return app_mesh_radio_owner_gateway_command_cancel(
        work, &gateway_radio_handoff);
}

int app_node_comm_gateway_control_preemptive_abort_request(void)
{
    return app_mesh_radio_owner_abort_request(
        APP_MESH_RADIO_ABORT_SURVEY_ABORT,
        &gateway_preemptive_abort);
}

void app_node_comm_gateway_control_preemptive_abort_release(void)
{
    (void)app_mesh_radio_owner_abort_release(
        &gateway_preemptive_abort);
}
