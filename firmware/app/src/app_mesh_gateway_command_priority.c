#include "app_mesh_gateway_command_priority.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static bool schedule_error_retryable(int error)
{
    return error == -EAGAIN || error == -EBUSY || error == -ENOSPC;
}

static uint32_t next_generation(uint32_t generation)
{
    generation++;
    return generation == 0u ? 1u : generation;
}

static bool serial_after(uint32_t candidate, uint32_t reference)
{
    uint32_t distance;

    if (candidate == 0u || reference == 0u || candidate == reference) {
        return false;
    }
    distance = candidate - reference;
    return distance < UINT32_C(0x80000000);
}

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
        priority->generation = 0u;
        priority->admission_cutoff = 0u;
        priority->schedule_attempts = 0u;
    }
}

int app_mesh_gateway_command_priority_request(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    void *work,
    uint32_t admission_cutoff)
{
    if (priority == NULL || !priority_ops_valid(ops, work)) {
        return -EINVAL;
    }

    if (priority->state != APP_MESH_GATEWAY_COMMAND_PRIORITY_IDLE) {
        if (priority->work != work) {
            return -EBUSY;
        }
        /*
         * Commands admitted before the first safe boundary share its
         * generation. Once scheduling has failed, freeze the cutoff so a
         * later admission cannot be retired with the older generation.
         */
        if (priority->state ==
                APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY &&
            (priority->admission_cutoff == 0u ||
             serial_after(admission_cutoff,
                          priority->admission_cutoff))) {
            priority->admission_cutoff = admission_cutoff;
        }
        return 0;
    }

    ops->request_receive_abort(ops->ctx);
    priority->next_generation =
        next_generation(priority->next_generation);
    priority->work = work;
    priority->generation = priority->next_generation;
    priority->admission_cutoff = admission_cutoff;
    priority->schedule_attempts = 0u;
    priority->state = APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY;
    return 0;
}

static int schedule_current(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    enum app_mesh_gateway_command_priority_state required_state,
    struct app_mesh_gateway_command_priority_failure *failure)
{
    int ret;

    if (failure != NULL) {
        memset(failure, 0, sizeof(*failure));
    }
    if (priority == NULL || priority->state != required_state ||
        !priority_ops_valid(ops, priority->work)) {
        return -EINVAL;
    }

    ret = ops->reschedule_now(ops->ctx, priority->work);
    priority->schedule_attempts++;
    if (ret >= 0) {
        app_mesh_gateway_command_priority_reset(priority);
        return ret;
    }
    if (schedule_error_retryable(ret) &&
        priority->schedule_attempts <
            APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS) {
        priority->state =
            APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SCHEDULE_RETRY;
        return ret;
    }
    if (failure != NULL) {
        failure->generation = priority->generation;
        failure->admission_cutoff = priority->admission_cutoff;
    }
    app_mesh_gateway_command_priority_reset(priority);
    if (ops->clear_receive_abort != NULL) {
        ops->clear_receive_abort(ops->ctx);
    }
    return ret;
}

int app_mesh_gateway_command_priority_acknowledge_safe_boundary(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    struct app_mesh_gateway_command_priority_failure *failure)
{
    return schedule_current(
        priority,
        ops,
        APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY,
        failure);
}

int app_mesh_gateway_command_priority_retry_schedule(
    struct app_mesh_gateway_command_priority *priority,
    const struct app_mesh_gateway_command_priority_ops *ops,
    struct app_mesh_gateway_command_priority_failure *failure)
{
    return schedule_current(
        priority,
        ops,
        APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SCHEDULE_RETRY,
        failure);
}

bool app_mesh_gateway_command_priority_waiting_for_safe_boundary(
    const struct app_mesh_gateway_command_priority *priority)
{
    return priority != NULL && priority->state ==
           APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SAFE_BOUNDARY;
}

bool app_mesh_gateway_command_priority_schedule_retry_pending(
    const struct app_mesh_gateway_command_priority *priority)
{
    return priority != NULL && priority->state ==
           APP_MESH_GATEWAY_COMMAND_PRIORITY_WAIT_SCHEDULE_RETRY;
}

bool app_mesh_gateway_command_priority_active(
    const struct app_mesh_gateway_command_priority *priority)
{
    return priority != NULL &&
           priority->state != APP_MESH_GATEWAY_COMMAND_PRIORITY_IDLE;
}
