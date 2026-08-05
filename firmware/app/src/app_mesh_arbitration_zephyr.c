#include "app_mesh_arbitration_zephyr.h"

#include "app_mesh_gateway_command_priority.h"
#include "dwm3000_driver.h"

#include <errno.h>
#include <string.h>

static struct app_mesh_gateway_command_priority gateway_priority;
static struct app_mesh_arbitration_zephyr_gateway_ops gateway_ops;
static app_mesh_arbitration_zephyr_schedule_failure_fn gateway_schedule_failure_handler;
static void *gateway_schedule_failure_ctx;
static struct k_spinlock gateway_priority_lock;
static struct k_work_delayable gateway_priority_retry_work;
static bool gateway_priority_retry_work_initialized;
static struct {
    struct k_work_delayable *work;
    uint32_t admission_cutoff;
    bool valid;
} gateway_priority_binding;

#define GATEWAY_PRIORITY_SCHEDULE_RETRY_MS 10u

static void gateway_command_request_receive_abort(void *ctx)
{
    ARG_UNUSED(ctx);
    dwm3000_driver_request_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
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
    dwm3000_driver_clear_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
}

static struct app_mesh_gateway_command_priority_ops gateway_priority_ops(void)
{
    return (struct app_mesh_gateway_command_priority_ops) {
        .gateway_role = gateway_ops.gateway_role,
        .request_receive_abort = gateway_command_request_receive_abort,
        .reschedule_now = gateway_command_reschedule_now,
        .clear_receive_abort = gateway_command_clear_receive_abort,
        .ctx = &gateway_ops,
    };
}

static void gateway_priority_notify_failure(
    app_mesh_arbitration_zephyr_schedule_failure_fn handler,
    void *ctx,
    int error,
    const struct app_mesh_gateway_command_priority_failure *failure)
{
    if (handler != NULL && failure != NULL &&
        failure->generation != 0u && failure->admission_cutoff != 0u) {
        handler(ctx,
                error,
                failure->generation,
                failure->admission_cutoff);
    }
}

static int gateway_priority_schedule_retry_locked(void)
{
    return k_work_reschedule(&gateway_priority_retry_work,
                             K_MSEC(GATEWAY_PRIORITY_SCHEDULE_RETRY_MS));
}

static int gateway_priority_retain_retry_locked(
    const struct app_mesh_gateway_command_priority_ops *priority_ops,
    struct app_mesh_gateway_command_priority_failure *failure,
    int target_error)
{
    int ret = gateway_priority_schedule_retry_locked();

    if (ret >= 0) {
        return target_error;
    }
    /*
     * A rejected retry-work submission is not a custody owner. Drive the
     * remaining target attempts synchronously so the generation either gains
     * executable work custody or reaches its bounded terminal callback.
     */
    while (app_mesh_gateway_command_priority_schedule_retry_pending(
               &gateway_priority)) {
        target_error = app_mesh_gateway_command_priority_retry_schedule(
            &gateway_priority, priority_ops, failure);
        if (target_error >= 0) {
            break;
        }
    }
    return target_error;
}

static void gateway_priority_retry_work_handler(struct k_work *work)
{
    struct app_mesh_gateway_command_priority_failure failure = {0};
    struct app_mesh_gateway_command_priority_ops priority_ops;
    app_mesh_arbitration_zephyr_schedule_failure_fn failure_handler = NULL;
    void *failure_ctx = NULL;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);
    key = k_spin_lock(&gateway_priority_lock);
    if (!app_mesh_gateway_command_priority_schedule_retry_pending(
            &gateway_priority)) {
        k_spin_unlock(&gateway_priority_lock, key);
        return;
    }
    priority_ops = gateway_priority_ops();
    ret = app_mesh_gateway_command_priority_retry_schedule(
        &gateway_priority, &priority_ops, &failure);
    if (ret < 0 &&
        app_mesh_gateway_command_priority_schedule_retry_pending(
            &gateway_priority)) {
        /*
         * Keep the frozen generation and one-shot receive abort asserted.
         * The safe-boundary owner is also allowed to retry this state, so a
         * rejected delayed-work reschedule cannot orphan accepted custody.
         */
        ret = gateway_priority_retain_retry_locked(
            &priority_ops, &failure, ret);
    }
    if (failure.generation != 0u) {
        failure_handler = gateway_schedule_failure_handler;
        failure_ctx = gateway_schedule_failure_ctx;
    }
    k_spin_unlock(&gateway_priority_lock, key);
    gateway_priority_notify_failure(failure_handler,
                                    failure_ctx,
                                    ret,
                                    &failure);
}

int app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
    struct k_work_delayable *work,
    uint32_t admission_cutoff)
{
    k_spinlock_key_t key;

    if (work == NULL || admission_cutoff == 0u) {
        return -EINVAL;
    }
    key = k_spin_lock(&gateway_priority_lock);
    if (gateway_priority_binding.valid &&
        gateway_priority_binding.work != work) {
        k_spin_unlock(&gateway_priority_lock, key);
        return -EBUSY;
    }
    gateway_priority_binding.work = work;
    gateway_priority_binding.admission_cutoff = admission_cutoff;
    gateway_priority_binding.valid = true;
    k_spin_unlock(&gateway_priority_lock, key);
    return 0;
}

int app_mesh_arbitration_zephyr_gateway_command_submit(
    const struct app_mesh_arbitration_zephyr_gateway_ops *ops,
    struct k_work_delayable *work)
{
    struct app_mesh_arbitration_zephyr_gateway_ops candidate_ops;
    struct app_mesh_gateway_command_priority_ops priority_ops;
    bool already_active;
    uint32_t admission_cutoff = 0u;
    void *waiting_work;
    k_spinlock_key_t key;
    int ret;

    if (ops == NULL || work == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&gateway_priority_lock);
    if (!gateway_priority_retry_work_initialized) {
        k_work_init_delayable(&gateway_priority_retry_work,
                              gateway_priority_retry_work_handler);
        gateway_priority_retry_work_initialized = true;
    }
    candidate_ops = *ops;
    already_active =
        app_mesh_gateway_command_priority_active(&gateway_priority);
    waiting_work = gateway_priority.work;
    if (gateway_priority_binding.valid &&
        gateway_priority_binding.work == work) {
        admission_cutoff = gateway_priority_binding.admission_cutoff;
        memset(&gateway_priority_binding, 0,
               sizeof(gateway_priority_binding));
    }
    priority_ops = (struct app_mesh_gateway_command_priority_ops) {
        .gateway_role = candidate_ops.gateway_role,
        .request_receive_abort = gateway_command_request_receive_abort,
        .reschedule_now = gateway_command_reschedule_now,
        .clear_receive_abort = gateway_command_clear_receive_abort,
        .ctx = &candidate_ops,
    };

    ret = app_mesh_gateway_command_priority_request(&gateway_priority,
                                                    &priority_ops,
                                                    work,
                                                    admission_cutoff);
    if (ret == 0 && (!already_active || waiting_work != work)) {
        gateway_ops = candidate_ops;
    }
    k_spin_unlock(&gateway_priority_lock, key);
    return ret;
}

int app_mesh_arbitration_zephyr_gateway_receive_abort_observed(void)
{
    struct app_mesh_gateway_command_priority_failure failure = {0};
    struct app_mesh_gateway_command_priority_ops priority_ops;
    app_mesh_arbitration_zephyr_schedule_failure_fn failure_handler = NULL;
    void *failure_ctx = NULL;
    k_spinlock_key_t key;
    int ret;

    key = k_spin_lock(&gateway_priority_lock);
    if (app_mesh_gateway_command_priority_waiting_for_safe_boundary(
            &gateway_priority)) {
        priority_ops = gateway_priority_ops();
        ret = app_mesh_gateway_command_priority_acknowledge_safe_boundary(
            &gateway_priority, &priority_ops, &failure);
    } else if (app_mesh_gateway_command_priority_schedule_retry_pending(
                   &gateway_priority)) {
        /*
         * The independent delayed retry is the normal owner. A repeated
         * safe-boundary observation is a second liveness edge and may make
         * the same frozen attempt without changing its generation or cutoff.
         */
        priority_ops = gateway_priority_ops();
        ret = app_mesh_gateway_command_priority_retry_schedule(
            &gateway_priority, &priority_ops, &failure);
    } else {
        k_spin_unlock(&gateway_priority_lock, key);
        return 0;
    }
    if (ret < 0 &&
        app_mesh_gateway_command_priority_schedule_retry_pending(
            &gateway_priority)) {
        ret = gateway_priority_retain_retry_locked(
            &priority_ops, &failure, ret);
    }
    if (failure.generation != 0u) {
        failure_handler = gateway_schedule_failure_handler;
        failure_ctx = gateway_schedule_failure_ctx;
    }
    k_spin_unlock(&gateway_priority_lock, key);
    gateway_priority_notify_failure(failure_handler,
                                    failure_ctx,
                                    ret,
                                    &failure);
    return ret;
}

void app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
    app_mesh_arbitration_zephyr_schedule_failure_fn handler,
    void *ctx)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_priority_lock);

    gateway_schedule_failure_handler = handler;
    gateway_schedule_failure_ctx = ctx;
    k_spin_unlock(&gateway_priority_lock, key);
}
