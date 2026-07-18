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

int app_mesh_queue_remove_first(
    const struct app_mesh_queue_remove_ops *ops,
    const struct mesh_outbound *target,
    struct mesh_outbound *scratch,
    bool *removed_out)
{
    uint32_t initial_count;
    bool removed = false;

    if (ops == NULL || target == NULL || scratch == NULL ||
        removed_out == NULL || ops->count == NULL || ops->get == NULL ||
        ops->put == NULL || ops->recover == NULL || ops->matches == NULL) {
        return -EINVAL;
    }
    initial_count = ops->count(ops->ctx);
    for (uint32_t i = 0u; i < initial_count; i++) {
        int ret = ops->get(scratch, ops->ctx);

        if (ret != 0) {
            *removed_out = removed;
            return ret;
        }
        if (!removed && ops->matches(scratch, target, ops->ctx)) {
            removed = true;
            continue;
        }
        ret = ops->put(scratch, ops->ctx);
        if (ret != 0) {
            int recover_ret = ops->recover(scratch, ops->ctx);

            *removed_out = removed;
            return recover_ret == 0 ? ret : recover_ret;
        }
    }
    *removed_out = removed;
    return removed ? 0 : -ENOENT;
}

int app_mesh_queue_remove_first_owned(
    const struct app_mesh_queue_head_owner *owner,
    const struct app_mesh_queue_remove_ops *ops,
    const struct mesh_outbound *target,
    struct mesh_outbound *scratch,
    bool *removed_out)
{
    if (owner == NULL) {
        return -EINVAL;
    }
    if (owner->active) {
        if (removed_out != NULL) {
            *removed_out = false;
        }
        return -EBUSY;
    }
    return app_mesh_queue_remove_first(ops, target, scratch, removed_out);
}

void app_mesh_queue_head_owner_init(struct app_mesh_queue_head_owner *owner)
{
    if (owner != NULL) {
        memset(owner, 0, sizeof(*owner));
    }
}

bool app_mesh_queue_head_owned(const struct app_mesh_queue_head_owner *owner)
{
    return owner != NULL && owner->active;
}

int app_mesh_queue_head_begin(struct app_mesh_queue_head_owner *owner,
                              const struct app_mesh_queue_head_ops *ops,
                              struct mesh_outbound *outbound,
                              struct app_mesh_queue_head_token *token)
{
    int ret;

    if (owner == NULL || ops == NULL || outbound == NULL || token == NULL ||
        ops->peek == NULL || ops->get == NULL || ops->recover == NULL ||
        ops->matches == NULL) {
        return -EINVAL;
    }
    if (owner->active) {
        return -EBUSY;
    }

    ret = ops->peek(outbound, ops->ctx);
    if (ret != 0) {
        return ret;
    }
    owner->generation++;
    if (owner->generation == 0u) {
        owner->generation = 1u;
    }
    owner->active = true;
    token->generation = owner->generation;
    return 0;
}

int app_mesh_queue_head_commit(struct app_mesh_queue_head_owner *owner,
                               const struct app_mesh_queue_head_ops *ops,
                               const struct app_mesh_queue_head_token *token,
                               const struct mesh_outbound *expected,
                               struct mesh_outbound *removed)
{
    int ret;

    if (owner == NULL || ops == NULL || token == NULL || expected == NULL ||
        removed == NULL || expected == removed || ops->peek == NULL ||
        ops->get == NULL || ops->recover == NULL || ops->matches == NULL) {
        return -EINVAL;
    }
    if (!owner->active || token->generation == 0u ||
        token->generation != owner->generation) {
        return -ESTALE;
    }

    ret = ops->peek(removed, ops->ctx);
    if (ret != 0) {
        return ret;
    }
    if (!ops->matches(removed, expected, ops->ctx)) {
        return -ESTALE;
    }

    ret = ops->get(removed, ops->ctx);
    if (ret != 0) {
        return ret;
    }
    if (!ops->matches(removed, expected, ops->ctx)) {
        int recover_ret = ops->recover(removed, ops->ctx);

        return recover_ret == 0 ? -EIO : recover_ret;
    }

    owner->active = false;
    return 0;
}

int app_mesh_queue_head_abort(struct app_mesh_queue_head_owner *owner,
                              const struct app_mesh_queue_head_token *token)
{
    if (owner == NULL || token == NULL) {
        return -EINVAL;
    }
    if (!owner->active || token->generation == 0u ||
        token->generation != owner->generation) {
        return -ESTALE;
    }
    owner->active = false;
    return 0;
}

enum app_mesh_parent_loss_custody app_mesh_parent_loss_custody_decide(
    bool route_discovery_needed,
    bool core_tx_active,
    bool route_wait_eligible)
{
    if (!route_discovery_needed || !route_wait_eligible) {
        return APP_MESH_PARENT_LOSS_CUSTODY_NONE;
    }
    if (core_tx_active) {
        return APP_MESH_PARENT_LOSS_CUSTODY_CORE_RETRY;
    }
    return APP_MESH_PARENT_LOSS_CUSTODY_ROUTE_WAIT;
}

enum app_mesh_queue_reserve_action app_mesh_queue_reserve_decide(
    bool reserve_valid,
    bool reserved_local_origin_priority,
    bool incoming_local_origin_priority)
{
    (void)reserved_local_origin_priority;
    (void)incoming_local_origin_priority;

    if (!reserve_valid) {
        return APP_MESH_QUEUE_RESERVE_ADMIT;
    }
    return APP_MESH_QUEUE_RESERVE_REJECT;
}
