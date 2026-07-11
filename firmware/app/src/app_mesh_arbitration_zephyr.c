#include "app_mesh_arbitration_zephyr.h"

#include "app_mesh_gateway_command_priority.h"
#include "dwm3000_driver.h"

#include <errno.h>

static struct app_mesh_gateway_command_priority gateway_priority;
static struct app_mesh_arbitration_zephyr_gateway_ops gateway_ops;
static app_mesh_arbitration_zephyr_schedule_failure_fn gateway_schedule_failure_handler;
static void *gateway_schedule_failure_ctx;

static void gateway_command_request_receive_abort(void *ctx)
{
    ARG_UNUSED(ctx);
    dwm3000_driver_request_receive_abort();
}

static int gateway_command_reschedule_now(void *ctx, void *work)
{
    const struct app_mesh_arbitration_zephyr_gateway_ops *ops = ctx;
    struct k_work_delayable *delayed_work = work;

    if (ops == NULL || delayed_work == NULL) {
        return -EINVAL;
    }
    if (ops->priority_work_queue != NULL) {
        return k_work_reschedule_for_queue(ops->priority_work_queue,
                                           delayed_work,
                                           K_NO_WAIT);
    }
    return k_work_reschedule(delayed_work, K_NO_WAIT);
}

static void gateway_command_clear_receive_abort(void *ctx)
{
    ARG_UNUSED(ctx);
    dwm3000_driver_clear_receive_abort();
}

int app_mesh_arbitration_zephyr_gateway_command_submit(
    const struct app_mesh_arbitration_zephyr_gateway_ops *ops,
    struct k_work_delayable *work)
{
    struct app_mesh_gateway_command_priority_ops priority_ops;

    if (ops == NULL) {
        return -EINVAL;
    }
    gateway_ops = *ops;
    priority_ops = (struct app_mesh_gateway_command_priority_ops) {
        .gateway_role = gateway_ops.gateway_role,
        .request_receive_abort = gateway_command_request_receive_abort,
        .reschedule_now = gateway_command_reschedule_now,
        .clear_receive_abort = gateway_command_clear_receive_abort,
        .ctx = &gateway_ops,
    };

    return app_mesh_gateway_command_priority_request(&gateway_priority,
                                                      &priority_ops,
                                                      work);
}

int app_mesh_arbitration_zephyr_gateway_receive_abort_observed(void)
{
    const struct app_mesh_gateway_command_priority_ops priority_ops = {
        .gateway_role = gateway_ops.gateway_role,
        .request_receive_abort = gateway_command_request_receive_abort,
        .reschedule_now = gateway_command_reschedule_now,
        .clear_receive_abort = gateway_command_clear_receive_abort,
        .ctx = &gateway_ops,
    };
    int ret;

    if (!app_mesh_gateway_command_priority_waiting_for_safe_boundary(
            &gateway_priority)) {
        return 0;
    }
    ret = app_mesh_gateway_command_priority_acknowledge_safe_boundary(
        &gateway_priority, &priority_ops);
    if (ret < 0 && gateway_schedule_failure_handler != NULL) {
        gateway_schedule_failure_handler(gateway_schedule_failure_ctx, ret);
    }
    return ret;
}

void app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
    app_mesh_arbitration_zephyr_schedule_failure_fn handler,
    void *ctx)
{
    gateway_schedule_failure_handler = handler;
    gateway_schedule_failure_ctx = ctx;
}
