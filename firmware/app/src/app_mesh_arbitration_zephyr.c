#include "app_mesh_arbitration_zephyr.h"

#include "dwm3000_driver.h"
#include "firmware_state_machines.h"

#include <errno.h>
#include <string.h>

#define GATEWAY_HANDOFF_OPERATION_ID UINT64_C(1)
#define GATEWAY_HANDOFF_RETRY_MS 10u

static struct fw_radio_handoff_sm gateway_handoff;
static struct app_mesh_arbitration_zephyr_gateway_ops gateway_ops;
static app_mesh_arbitration_zephyr_schedule_failure_fn failure_handler;
static void *failure_context;
static struct k_spinlock gateway_handoff_lock;
static struct k_work_delayable retry_work;
static bool retry_work_initialized;
static uint32_t next_generation;
static struct {
    struct k_work_delayable *work;
    uint32_t admission_cutoff;
    bool valid;
} pending_binding;
static struct k_work_delayable *pending_work;

static bool schedule_error_retryable(int error)
{
    return error == -EAGAIN || error == -EBUSY || error == -ENOSPC;
}

static uint32_t generation_next(uint32_t generation)
{
    generation++;
    return generation == 0u ? 1u : generation;
}

static struct fw_event handoff_event(enum fw_event_type type)
{
    return (struct fw_event) {
        .operation_id = gateway_handoff.identity.operation_id,
        .generation = gateway_handoff.identity.generation,
        .target = FW_MACHINE_RADIO,
        .source = FW_EVENT_SOURCE_SERVICE,
        .type = type,
    };
}

static int schedule_pending_work(void)
{
    if (pending_work == NULL) {
        return -EINVAL;
    }
    if (gateway_ops.priority_work_queue != NULL) {
        return k_work_reschedule_for_queue(gateway_ops.priority_work_queue,
                                           pending_work,
                                           K_NO_WAIT);
    }
    return k_work_reschedule(pending_work, K_NO_WAIT);
}

static int schedule_retry_owner(void)
{
    return k_work_reschedule(&retry_work,
                             K_MSEC(GATEWAY_HANDOFF_RETRY_MS));
}

static int drive_schedule_locked(
    enum fw_event_type trigger,
    uint32_t *failed_generation,
    uint32_t *failed_cutoff)
{
    struct fw_transition transition;
    struct fw_event event;
    int ret;

    for (;;) {
        event = handoff_event(trigger);
        if (fw_radio_handoff_sm_handle(&gateway_handoff,
                                       &event,
                                       &transition) != FW_SM_APPLIED ||
            transition.effect.type != FW_EFFECT_RADIO_SCHEDULE_PENDING) {
            return -EINVAL;
        }

        ret = schedule_pending_work();
        event = handoff_event(ret >= 0 ? FW_EVENT_EFFECT_SUCCEEDED :
                                         FW_EVENT_EFFECT_FAILED);
        if (ret < 0 && schedule_error_retryable(ret)) {
            event.payload.flags |= FW_EVENT_FLAG_RETRYABLE;
        }
        (void)fw_radio_handoff_sm_handle(&gateway_handoff,
                                         &event,
                                         &transition);
        if (ret >= 0) {
            /*
             * Retire this generation's level-triggered abort while the
             * handoff lock still prevents a successor from requesting the
             * same bit.  A delayed generation-A worker must never clear a
             * generation-B abort after B has been admitted.
             */
            dwm3000_driver_clear_receive_abort(
                DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
            pending_work = NULL;
            return 0;
        }
        if (transition.effect.type == FW_EFFECT_RADIO_CLEAR_ABORT) {
            if (failed_generation != NULL) {
                *failed_generation = event.generation;
            }
            if (failed_cutoff != NULL) {
                *failed_cutoff = transition.effect.payload.value;
            }
            pending_work = NULL;
            dwm3000_driver_clear_receive_abort(
                DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
            return ret;
        }
        if (transition.effect.type != FW_EFFECT_START_TIMER) {
            return -EINVAL;
        }
        if (schedule_retry_owner() >= 0) {
            return ret;
        }
        /* Rejected retry work owns nothing; take the next bounded attempt. */
        trigger = FW_EVENT_TIMER_EXPIRED;
    }
}

static void notify_failure(app_mesh_arbitration_zephyr_schedule_failure_fn fn,
                           void *context,
                           int error,
                           uint32_t generation,
                           uint32_t admission_cutoff)
{
    if (fn != NULL && generation != 0u && admission_cutoff != 0u) {
        fn(context, error, generation, admission_cutoff);
    }
}

static void retry_work_handler(struct k_work *work)
{
    app_mesh_arbitration_zephyr_schedule_failure_fn notify = NULL;
    void *notify_context = NULL;
    uint32_t failed_generation = 0u;
    uint32_t failed_cutoff = 0u;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);
    key = k_spin_lock(&gateway_handoff_lock);
    if (gateway_handoff.state != FW_RADIO_HANDOFF_WAIT_RETRY) {
        k_spin_unlock(&gateway_handoff_lock, key);
        return;
    }
    ret = drive_schedule_locked(FW_EVENT_TIMER_EXPIRED,
                                &failed_generation,
                                &failed_cutoff);
    if (failed_generation != 0u) {
        notify = failure_handler;
        notify_context = failure_context;
    }
    k_spin_unlock(&gateway_handoff_lock, key);
    notify_failure(notify, notify_context, ret,
                   failed_generation, failed_cutoff);
}

int app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
    struct k_work_delayable *work,
    uint32_t admission_cutoff)
{
    k_spinlock_key_t key;

    if (work == NULL || admission_cutoff == 0u) {
        return -EINVAL;
    }
    key = k_spin_lock(&gateway_handoff_lock);
    if (pending_binding.valid && pending_binding.work != work) {
        k_spin_unlock(&gateway_handoff_lock, key);
        return -EBUSY;
    }
    pending_binding.work = work;
    pending_binding.admission_cutoff = admission_cutoff;
    pending_binding.valid = true;
    k_spin_unlock(&gateway_handoff_lock, key);
    return 0;
}

int app_mesh_arbitration_zephyr_gateway_command_submit(
    const struct app_mesh_arbitration_zephyr_gateway_ops *ops,
    struct k_work_delayable *work)
{
    struct fw_transition transition;
    struct fw_event event;
    uint32_t admission_cutoff = 0u;
    k_spinlock_key_t key;
    int ret;

    if (ops == NULL || !ops->gateway_role || work == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&gateway_handoff_lock);
    if (!retry_work_initialized) {
        fw_radio_handoff_sm_init(&gateway_handoff);
        k_work_init_delayable(&retry_work, retry_work_handler);
        retry_work_initialized = true;
    }
    if (pending_binding.valid && pending_binding.work == work) {
        admission_cutoff = pending_binding.admission_cutoff;
        memset(&pending_binding, 0, sizeof(pending_binding));
    }
    if (admission_cutoff == 0u) {
        admission_cutoff = gateway_handoff.identity.active ?
                               gateway_handoff.admission_cutoff : 1u;
    }
    if (gateway_handoff.identity.active && pending_work != work) {
        k_spin_unlock(&gateway_handoff_lock, key);
        return -EBUSY;
    }
    if (!gateway_handoff.identity.active) {
        next_generation = generation_next(next_generation);
        gateway_ops = *ops;
        pending_work = work;
        event = (struct fw_event) {
            .operation_id = GATEWAY_HANDOFF_OPERATION_ID,
            .generation = next_generation,
            .target = FW_MACHINE_RADIO,
            .source = FW_EVENT_SOURCE_SERVICE,
            .type = FW_EVENT_RADIO_PREEMPT_REQUESTED,
        };
    } else {
        event = handoff_event(FW_EVENT_RADIO_PREEMPT_REQUESTED);
    }
    event.payload.value = admission_cutoff;
    ret = fw_radio_handoff_sm_handle(&gateway_handoff,
                                     &event,
                                     &transition) == FW_SM_APPLIED ? 0 :
          -EBUSY;
    if (ret == 0 &&
        transition.effect.type == FW_EFFECT_RADIO_REQUEST_ABORT) {
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    }
    k_spin_unlock(&gateway_handoff_lock, key);
    return ret;
}

int app_mesh_arbitration_zephyr_gateway_receive_abort_observed(void)
{
    app_mesh_arbitration_zephyr_schedule_failure_fn notify = NULL;
    void *notify_context = NULL;
    uint32_t failed_generation = 0u;
    uint32_t failed_cutoff = 0u;
    enum fw_event_type trigger;
    k_spinlock_key_t key;
    int ret;

    key = k_spin_lock(&gateway_handoff_lock);
    if (gateway_handoff.state == FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY) {
        trigger = FW_EVENT_RADIO_SAFE_BOUNDARY;
    } else if (gateway_handoff.state == FW_RADIO_HANDOFF_WAIT_RETRY) {
        trigger = FW_EVENT_TIMER_EXPIRED;
    } else {
        k_spin_unlock(&gateway_handoff_lock, key);
        return 0;
    }
    ret = drive_schedule_locked(trigger,
                                &failed_generation,
                                &failed_cutoff);
    if (failed_generation != 0u) {
        notify = failure_handler;
        notify_context = failure_context;
    }
    k_spin_unlock(&gateway_handoff_lock, key);
    notify_failure(notify, notify_context, ret,
                   failed_generation, failed_cutoff);
    return ret;
}

void app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
    app_mesh_arbitration_zephyr_schedule_failure_fn handler,
    void *ctx)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_handoff_lock);

    failure_handler = handler;
    failure_context = ctx;
    k_spin_unlock(&gateway_handoff_lock, key);
}
