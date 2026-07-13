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
    if (plan->save_outbox && plan->requeue_click_report) {
        return -EINVAL;
    }
    if (plan->save_outbox || plan->clear_outbox || plan->cancel_timeout ||
        plan->schedule_timeout || plan->requeue_click_report) {
        local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME;
    }

    /* Timeout setup is harmless while the active runtime copy still owns it. */
    if (plan->schedule_timeout) {
        if (ops->schedule_timeout == NULL) {
            local_result.schedule_timeout_ret = -ENOTSUP;
        } else {
            local_result.schedule_timeout_ret = ops->schedule_timeout(ops->ctx);
        }
        local_result.timeout_scheduled = local_result.schedule_timeout_ret >= 0;
        if (local_result.schedule_timeout_ret < 0) {
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.schedule_timeout_ret;
        }
    }

    if (plan->save_outbox) {
        if (ops->save_outbox == NULL) {
            local_result.save_outbox_ret = -ENOTSUP;
        } else {
            local_result.save_outbox_ret = ops->save_outbox(ops->ctx);
        }
        local_result.outbox_saved = local_result.save_outbox_ret == 0;
        if (local_result.save_outbox_ret < 0) {
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.save_outbox_ret;
        }
    }

    if (plan->cancel_timeout) {
        if (ops->cancel_timeout == NULL) {
            local_result.cancel_timeout_ret = -ENOTSUP;
        } else {
            local_result.cancel_timeout_ret = ops->cancel_timeout(ops->ctx);
        }
        local_result.timeout_cancelled = local_result.cancel_timeout_ret >= 0;
        if (local_result.cancel_timeout_ret < 0) {
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.cancel_timeout_ret;
        }
    }

    if (plan->requeue_click_report) {
        if (ops->stage_click_handoff == NULL) {
            local_result.stage_click_handoff_ret = -ENOTSUP;
        } else {
            local_result.stage_click_handoff_ret =
                ops->stage_click_handoff(ops->ctx, &plan->click_report);
        }
        local_result.click_handoff_staged =
            local_result.stage_click_handoff_ret == 0;
        if (local_result.stage_click_handoff_ret < 0) {
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.stage_click_handoff_ret;
        }

        if (ops->commit_click_handoff == NULL) {
            local_result.commit_click_handoff_ret = -ENOTSUP;
        } else {
            local_result.commit_click_handoff_ret =
                ops->commit_click_handoff(ops->ctx, &plan->click_report);
        }
        local_result.click_handoff_committed =
            local_result.commit_click_handoff_ret == 0;
        if (local_result.commit_click_handoff_ret < 0) {
            if (ops->rollback_click_handoff == NULL) {
                local_result.rollback_click_handoff_ret = -ENOTSUP;
            } else {
                local_result.rollback_click_handoff_ret =
                    ops->rollback_click_handoff(ops->ctx, &plan->click_report);
            }
            local_result.click_handoff_rollback_failed =
                local_result.rollback_click_handoff_ret < 0;
            local_result.custody_owner = local_result.click_handoff_rollback_failed ?
                APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF :
                APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME;
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.commit_click_handoff_ret;
        }
    }

    if (plan->cancel_active_tx) {
        if (ops->cancel_active_tx == NULL) {
            local_result.cancel_active_tx_ret = -ENOTSUP;
        } else {
            local_result.cancel_active_tx_ret = ops->cancel_active_tx(ops->ctx);
        }
        local_result.active_tx_cancelled = local_result.cancel_active_tx_ret == 0;
        if (local_result.cancel_active_tx_ret < 0) {
            if (local_result.click_handoff_staged) {
                if (ops->rollback_click_handoff == NULL) {
                    local_result.rollback_click_handoff_ret = -ENOTSUP;
                } else {
                    local_result.rollback_click_handoff_ret =
                        ops->rollback_click_handoff(ops->ctx,
                                                    &plan->click_report);
                }
                local_result.click_handoff_rollback_failed =
                    local_result.rollback_click_handoff_ret < 0;
            } else if (local_result.outbox_saved) {
                if (ops->clear_outbox == NULL) {
                    local_result.clear_outbox_ret = -ENOTSUP;
                } else {
                    local_result.clear_outbox_ret = ops->clear_outbox(ops->ctx);
                }
                local_result.outbox_cleared = local_result.clear_outbox_ret == 0;
            }
            /* A rollback failure leaves a recoverable durable copy authoritative. */
            local_result.custody_owner = local_result.click_handoff_rollback_failed ?
                APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF :
                APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME;
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.cancel_active_tx_ret;
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
        if (local_result.click_report_requeue_ret < 0) {
            local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF;
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.click_report_requeue_ret;
        }

        /* The journal is committed before the obsolete primary record is cleared. */
        if (plan->clear_outbox) {
            if (ops->clear_outbox == NULL) {
                local_result.clear_outbox_ret = -ENOTSUP;
            } else {
                local_result.clear_outbox_ret = ops->clear_outbox(ops->ctx);
            }
            local_result.outbox_cleared = local_result.clear_outbox_ret == 0;
            if (local_result.clear_outbox_ret < 0) {
                local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF;
                if (result != NULL) {
                    *result = local_result;
                }
                return local_result.clear_outbox_ret;
            }
        }
        local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF;
    } else if (plan->save_outbox) {
        local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_OUTBOX;
    } else if (plan->cancel_active_tx) {
        local_result.custody_owner = APP_MESH_CLICK_PREEMPT_OWNER_NONE;
    }

    local_result.transaction_committed = true;
    if (result != NULL) {
        *result = local_result;
    }
    return 0;
}
