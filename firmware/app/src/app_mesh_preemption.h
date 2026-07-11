#ifndef APP_MESH_PREEMPTION_H
#define APP_MESH_PREEMPTION_H

#include "mesh_preemption.h"

#include <stdbool.h>

struct app_mesh_click_preempt_ops {
    int (*save_outbox)(void *ctx);
    int (*clear_outbox)(void *ctx);
    int (*cancel_timeout)(void *ctx);
    int (*schedule_timeout)(void *ctx);
    int (*requeue_click_report)(void *ctx,
                                const struct mesh_outbound *outbound);
    void *ctx;
};

struct app_mesh_click_preempt_result {
    bool outbox_saved;
    bool outbox_cleared;
    bool timeout_cancelled;
    bool timeout_scheduled;
    bool click_report_requeued;
    bool click_report_requeue_failed;
    int save_outbox_ret;
    int clear_outbox_ret;
    int cancel_timeout_ret;
    int schedule_timeout_ret;
    int click_report_requeue_ret;
};

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result);

#endif
