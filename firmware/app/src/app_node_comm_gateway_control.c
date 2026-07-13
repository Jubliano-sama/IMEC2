#include "app_node_comm_gateway_control.h"

#include "app_mesh_arbitration_zephyr.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_flood.h"

#include <errno.h>
#include <string.h>

static struct app_mesh_command_orchestrator gateway_control_orchestrator;
static struct app_node_comm_gateway_control_config gateway_control_config;

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

int app_node_comm_gateway_control_priority_submit(
    struct k_work_delayable *work)
{
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = gateway_control_config.gateway_role,
        .priority_work_queue = gateway_control_config.priority_work_queue,
    };
    int ret = app_mesh_arbitration_zephyr_gateway_command_submit(&ops, work);

    if (gateway_control_config.priority_observer != NULL) {
        gateway_control_config.priority_observer(gateway_control_config.ctx,
                                                 ret);
    }
    return ret;
}

int app_node_comm_gateway_control_safe_boundary_schedule(void *ctx)
{
    (void)ctx;
    return app_mesh_arbitration_zephyr_gateway_receive_abort_observed();
}
