#ifndef APP_MESH_PREEMPTION_H
#define APP_MESH_PREEMPTION_H

#include "mesh_preemption.h"

#include <stdbool.h>

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

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result);
int app_mesh_queue_remove_first(
    const struct app_mesh_queue_remove_ops *ops,
    const struct mesh_outbound *target,
    struct mesh_outbound *scratch,
    bool *removed_out);

#endif
