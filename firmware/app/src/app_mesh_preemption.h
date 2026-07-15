#ifndef APP_MESH_PREEMPTION_H
#define APP_MESH_PREEMPTION_H

#include "mesh_preemption.h"

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_click_preempt_ops {
    int (*save_outbox)(void *ctx);
    int (*clear_outbox)(void *ctx);
    int (*stage_click_handoff)(void *ctx,
                               const struct mesh_outbound *outbound);
    int (*commit_click_handoff)(void *ctx,
                                const struct mesh_outbound *outbound);
    int (*rollback_click_handoff)(void *ctx,
                                  const struct mesh_outbound *outbound);
    int (*cancel_timeout)(void *ctx);
    int (*schedule_timeout)(void *ctx);
    int (*requeue_click_report)(void *ctx,
                                const struct mesh_outbound *outbound);
    int (*discard_requeued_click_report)(void *ctx,
                                         const struct mesh_outbound *outbound);
    int (*cancel_active_tx)(void *ctx);
    void *ctx;
};

enum app_mesh_click_preempt_owner {
    APP_MESH_CLICK_PREEMPT_OWNER_NONE = 0,
    APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME,
    APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_OUTBOX,
    APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF,
    APP_MESH_CLICK_PREEMPT_OWNER_CLICK_REPORT,
};

struct app_mesh_click_preempt_result {
    bool outbox_saved;
    bool outbox_cleared;
    bool click_handoff_staged;
    bool click_handoff_committed;
    bool click_handoff_rollback_failed;
    bool timeout_cancelled;
    bool timeout_scheduled;
    bool click_report_requeued;
    bool click_report_requeue_discarded;
    bool click_report_requeue_failed;
    bool active_tx_cancelled;
    bool transaction_committed;
    enum app_mesh_click_preempt_owner custody_owner;
    int save_outbox_ret;
    int clear_outbox_ret;
    int stage_click_handoff_ret;
    int commit_click_handoff_ret;
    int rollback_click_handoff_ret;
    int cancel_timeout_ret;
    int schedule_timeout_ret;
    int click_report_requeue_ret;
    int discard_requeued_click_report_ret;
    int cancel_active_tx_ret;
};

struct app_mesh_queue_remove_ops {
    uint32_t (*count)(void *ctx);
    int (*get)(struct mesh_outbound *outbound, void *ctx);
    int (*put)(const struct mesh_outbound *outbound, void *ctx);
    int (*recover)(const struct mesh_outbound *outbound, void *ctx);
    bool (*matches)(const struct mesh_outbound *candidate,
                    const struct mesh_outbound *target,
                    void *ctx);
    void *ctx;
};

/*
 * The caller serializes access to this owner and the underlying queue.  A
 * head owner spans a fallible operation without holding the caller's mutex;
 * appends may continue, but destructive rotation/removal must first observe
 * that no owner is active.
 */
struct app_mesh_queue_head_owner {
    uint32_t generation;
    bool active;
};

struct app_mesh_queue_head_token {
    uint32_t generation;
};

struct app_mesh_queue_head_ops {
    int (*peek)(struct mesh_outbound *outbound, void *ctx);
    int (*get)(struct mesh_outbound *outbound, void *ctx);
    int (*recover)(const struct mesh_outbound *outbound, void *ctx);
    bool (*matches)(const struct mesh_outbound *candidate,
                    const struct mesh_outbound *target,
                    void *ctx);
    void *ctx;
};

enum app_mesh_parent_loss_custody {
    APP_MESH_PARENT_LOSS_CUSTODY_NONE = 0,
    APP_MESH_PARENT_LOSS_CUSTODY_CORE_RETRY,
    APP_MESH_PARENT_LOSS_CUSTODY_ROUTE_WAIT,
};

enum app_mesh_queue_reserve_action {
    APP_MESH_QUEUE_RESERVE_ADMIT = 0,
    APP_MESH_QUEUE_RESERVE_REJECT,
    APP_MESH_QUEUE_RESERVE_REPLACE_TRANSIT_ACCOUNT_LOSS,
    APP_MESH_QUEUE_RESERVE_REPLACE_LOCAL_ACCOUNT_LOSS,
};

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result);
int app_mesh_queue_remove_first(
    const struct app_mesh_queue_remove_ops *ops,
    const struct mesh_outbound *target,
    struct mesh_outbound *scratch,
    bool *removed_out);
int app_mesh_queue_remove_first_owned(
    const struct app_mesh_queue_head_owner *owner,
    const struct app_mesh_queue_remove_ops *ops,
    const struct mesh_outbound *target,
    struct mesh_outbound *scratch,
    bool *removed_out);
void app_mesh_queue_head_owner_init(struct app_mesh_queue_head_owner *owner);
bool app_mesh_queue_head_owned(const struct app_mesh_queue_head_owner *owner);
int app_mesh_queue_head_begin(struct app_mesh_queue_head_owner *owner,
                              const struct app_mesh_queue_head_ops *ops,
                              struct mesh_outbound *outbound,
                              struct app_mesh_queue_head_token *token);
int app_mesh_queue_head_commit(struct app_mesh_queue_head_owner *owner,
                               const struct app_mesh_queue_head_ops *ops,
                               const struct app_mesh_queue_head_token *token,
                               const struct mesh_outbound *expected,
                               struct mesh_outbound *removed);
int app_mesh_queue_head_abort(struct app_mesh_queue_head_owner *owner,
                              const struct app_mesh_queue_head_token *token);
enum app_mesh_parent_loss_custody app_mesh_parent_loss_custody_decide(
    bool route_discovery_needed,
    bool core_tx_active,
    bool route_wait_eligible);
enum app_mesh_queue_reserve_action app_mesh_queue_reserve_decide(
    bool reserve_valid,
    bool reserved_local_origin_priority,
    bool incoming_local_origin_priority);

#endif
