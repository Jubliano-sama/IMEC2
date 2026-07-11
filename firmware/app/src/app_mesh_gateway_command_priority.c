#include "app_mesh_gateway_command_priority.h"

#include <errno.h>
#include <stddef.h>

static bool priority_ops_valid(
    const struct app_mesh_gateway_command_priority_ops *ops,
    void *work)
{
    return ops != NULL && ops->gateway_role && work != NULL &&
           ops->request_receive_abort != NULL && ops->reschedule_now != NULL &&
           ops->clear_receive_abort != NULL;
}

void app_mesh_gateway_command_priority_reset(
    struct app_mesh_gateway_command_priority *priority)
{
    if (priority != NULL) {
        priority->state = APP_MESH_GATEWAY_COMMAND_PRIORITY_IDLE;
        priority->work = NULL;
    }
}

int app_mesh_gateway_command_priority_request(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    void *work)
{
    if (priority == NULL || !priority_ops_valid(ops, work)) {
        return -EINVAL;
    }

    if (priority->state == APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY) {
        return priority->work == work ? 0 : -EBUSY;
    }

    ops->request_receive_abort(ops->ctx);
    priority->work = work;
    priority->state = APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY;
    return 0;
}

int app_mesh_gateway_command_priority_acknowledge_safe_boundary(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops)
{
    int ret;

    if (priority == NULL || priority->state !=
        APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY ||
        !priority_ops_valid(ops, priority->work)) {
        return -EINVAL;
    }

    ret = ops->reschedule_now(ops->ctx, priority->work);
    app_mesh_gateway_command_priority_reset(priority);
    if (ret < 0) {
        ops->clear_receive_abort(ops->ctx);
    }
    return ret;
}

bool app_mesh_gateway_command_priority_waiting_for_safe_boundary(
    const struct app_mesh_gateway_command_priority *priority)
{
    return priority != NULL && priority->state ==
           APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY;
}
