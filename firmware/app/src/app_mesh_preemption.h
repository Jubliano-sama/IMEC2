#ifndef APP_MESH_PREEMPTION_H
#define APP_MESH_PREEMPTION_H

#include "mesh_preemption.h"

#include <stdbool.h>

#include <zephyr/kernel.h>

struct app_mesh_click_preempt_ops {
    struct k_msgq *mesh_rx_msgq;
    struct k_msgq *report_tx_msgq;
    struct k_work_delayable *tx_timeout_work;
    int (*save_outbox)(void *ctx);
    void (*clear_outbox)(void *ctx);
    int (*schedule_timeout)(void *ctx);
    void *ctx;
};

struct app_mesh_click_preempt_result {
    bool outbox_saved;
    bool outbox_cleared;
    bool timeout_cancelled;
    bool timeout_scheduled;
    bool rx_queue_purged;
    bool click_report_requeued;
    bool click_report_requeue_failed;
    int save_outbox_ret;
    int cancel_timeout_ret;
    int schedule_timeout_ret;
};

int app_mesh_apply_click_preempt_plan(
    const struct mesh_click_preempt_plan *plan,
    const struct app_mesh_click_preempt_ops *ops,
    struct app_mesh_click_preempt_result *result);

#endif
