#include "app_mesh_preemption.h"

#include <errno.h>
#include <string.h>

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result)
{
    struct app_mesh_click_preempt_result local_result;

    if (plan == NULL || ops == NULL) {
        return -EINVAL;
    }

    memset(&local_result, 0, sizeof(local_result));

    if (plan->save_outbox && ops->save_outbox != NULL) {
        local_result.save_outbox_ret = ops->save_outbox(ops->ctx);
        local_result.outbox_saved = true;
    }
    if (plan->clear_outbox && ops->clear_outbox != NULL) {
        ops->clear_outbox(ops->ctx);
        local_result.outbox_cleared = true;
    }
    if (plan->cancel_timeout && ops->tx_timeout_work != NULL) {
        local_result.cancel_timeout_ret =
            k_work_cancel_delayable(ops->tx_timeout_work);
        local_result.timeout_cancelled = true;
    }
    if (plan->schedule_timeout && ops->schedule_timeout != NULL) {
        local_result.schedule_timeout_ret = ops->schedule_timeout(ops->ctx);
        local_result.timeout_scheduled = true;
    }
    if (plan->purge_rx_queue && ops->mesh_rx_msgq != NULL) {
        k_msgq_purge(ops->mesh_rx_msgq);
        local_result.rx_queue_purged = true;
    }
    if (plan->requeue_click_report && ops->report_tx_msgq != NULL) {
        if (k_msgq_put(ops->report_tx_msgq,
                       &plan->click_report,
                       K_NO_WAIT) == 0) {
            local_result.click_report_requeued = true;
        } else {
            local_result.click_report_requeue_failed = true;
        }
    }

    if (result != NULL) {
        *result = local_result;
    }
    return 0;
}
