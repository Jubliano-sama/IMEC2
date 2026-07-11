#include "app_mesh_preemption.h"

#include <errno.h>
#include <string.h>

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result)
{
    struct app_mesh_click_preempt_result local_result;
    int first_failure = 0;

    if (plan == NULL || ops == NULL) {
        return -EINVAL;
    }

    memset(&local_result, 0, sizeof(local_result));

    if (plan->save_outbox) {
        if (ops->save_outbox == NULL) {
            local_result.save_outbox_ret = -ENOTSUP;
        } else {
        local_result.save_outbox_ret = ops->save_outbox(ops->ctx);
        }
        local_result.outbox_saved = local_result.save_outbox_ret == 0;
        if (local_result.save_outbox_ret < 0) {
            first_failure = local_result.save_outbox_ret;
        }
    }

    if (plan->schedule_timeout) {
        if (ops->schedule_timeout == NULL) {
            local_result.schedule_timeout_ret = -ENOTSUP;
        } else {
            local_result.schedule_timeout_ret = ops->schedule_timeout(ops->ctx);
        }
        local_result.timeout_scheduled = local_result.schedule_timeout_ret >= 0;
        if (local_result.schedule_timeout_ret < 0 && first_failure == 0) {
            first_failure = local_result.schedule_timeout_ret;
        }
    }

    if (plan->requeue_click_report) {
        if (ops->requeue_click_report == NULL) {
            local_result.click_report_requeue_ret = -ENOTSUP;
        } else {
            local_result.click_report_requeue_ret =
                ops->requeue_click_report(ops->ctx, &plan->click_report);
        }
        local_result.click_report_requeued =
            local_result.click_report_requeue_ret == 0;
        local_result.click_report_requeue_failed =
            !local_result.click_report_requeued;
        if (local_result.click_report_requeue_ret < 0 && first_failure == 0) {
            first_failure = local_result.click_report_requeue_ret;
        }
    }

    /* Do not clear the durable owner until a local-click report has custody. */
    if (plan->cancel_timeout && local_result.click_report_requeue_failed == false) {
        if (ops->cancel_timeout == NULL) {
            local_result.cancel_timeout_ret = -ENOTSUP;
        } else {
            local_result.cancel_timeout_ret = ops->cancel_timeout(ops->ctx);
        }
        local_result.timeout_cancelled = local_result.cancel_timeout_ret >= 0;
        if (local_result.cancel_timeout_ret < 0 && first_failure == 0) {
            first_failure = local_result.cancel_timeout_ret;
        }
    }

    if (plan->clear_outbox && local_result.click_report_requeue_failed == false &&
        (!plan->cancel_timeout || local_result.timeout_cancelled)) {
        if (ops->clear_outbox == NULL) {
            local_result.clear_outbox_ret = -ENOTSUP;
        } else {
            local_result.clear_outbox_ret = ops->clear_outbox(ops->ctx);
        }
        local_result.outbox_cleared = local_result.clear_outbox_ret == 0;
        if (local_result.clear_outbox_ret < 0 && first_failure == 0) {
            first_failure = local_result.clear_outbox_ret;
        }
    }

    if (result != NULL) {
        *result = local_result;
    }
    return first_failure;
}
