#include "app_mesh_preemption.h"

#include "mesh.h"
#include "protocol.h"

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/ztest.h>

K_MSGQ_DEFINE(test_mesh_rx_msgq, sizeof(uint32_t), 2, 4);
K_MSGQ_DEFINE(test_report_tx_msgq, sizeof(struct mesh_outbound), 2, 4);

static struct k_work_delayable test_timeout_work;
static uint8_t save_count;
static uint8_t clear_count;
static uint8_t schedule_count;

static void timeout_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

static int save_outbox(void *ctx)
{
    ARG_UNUSED(ctx);

    save_count++;
    return 0;
}

static void clear_outbox(void *ctx)
{
    ARG_UNUSED(ctx);

    clear_count++;
}

static int schedule_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    schedule_count++;
    return k_work_reschedule(&test_timeout_work, K_MSEC(1000));
}

static void reset_runtime_objects(void)
{
    k_msgq_purge(&test_mesh_rx_msgq);
    k_msgq_purge(&test_report_tx_msgq);
    (void)k_work_cancel_delayable(&test_timeout_work);
    save_count = 0u;
    clear_count = 0u;
    schedule_count = 0u;
}

static void reset_runtime_objects_after(void *fixture)
{
    ARG_UNUSED(fixture);

    reset_runtime_objects();
}

static struct app_mesh_click_preempt_ops make_ops(void)
{
    return (struct app_mesh_click_preempt_ops) {
        .mesh_rx_msgq = &test_mesh_rx_msgq,
        .report_tx_msgq = &test_report_tx_msgq,
        .tx_timeout_work = &test_timeout_work,
        .save_outbox = save_outbox,
        .clear_outbox = clear_outbox,
        .schedule_timeout = schedule_timeout,
    };
}

ZTEST(mesh_preemption_app, test_collection_preempt_saves_purges_and_schedules)
{
    struct mesh_click_preempt_plan plan = {
        .purge_rx_queue = true,
        .save_outbox = true,
        .schedule_timeout = true,
    };
    struct app_mesh_click_preempt_ops ops = make_ops();
    struct app_mesh_click_preempt_result result;
    uint32_t rx_marker = 0x12345678u;

    reset_runtime_objects();
    zassert_ok(k_msgq_put(&test_mesh_rx_msgq, &rx_marker, K_NO_WAIT));

    zassert_ok(app_mesh_apply_click_preempt_plan(&plan, &ops, &result));

    zassert_true(result.outbox_saved);
    zassert_true(result.timeout_scheduled);
    zassert_true(result.rx_queue_purged);
    zassert_false(result.outbox_cleared);
    zassert_false(result.timeout_cancelled);
    zassert_equal(save_count, 1u);
    zassert_equal(clear_count, 0u);
    zassert_equal(schedule_count, 1u);
    zassert_true(k_work_delayable_is_pending(&test_timeout_work));
    zassert_equal(k_msgq_num_used_get(&test_mesh_rx_msgq), 0u);
}

ZTEST(mesh_preemption_app, test_non_collection_preempt_clears_cancels_and_requeues)
{
    struct mesh_click_preempt_plan plan = {
        .purge_rx_queue = true,
        .requeue_click_report = true,
        .clear_outbox = true,
        .cancel_timeout = true,
        .click_report = {
            .packet = {
                .msg_type = MSG_CLICK_REPORT,
                .src_id = 0x1111222233334444ull,
                .dst_id = 0x9999888877776666ull,
                .session_id = 5u,
                .seq = 6u,
                .ttl = MESH_DEFAULT_TTL,
            },
            .payload_len = 2u,
            .payload = { 0xAAu, 0x55u },
        },
    };
    struct app_mesh_click_preempt_ops ops = make_ops();
    struct app_mesh_click_preempt_result result;
    struct mesh_outbound requeued;
    uint32_t rx_marker = 0xA5A5A5A5u;
    int ret;

    reset_runtime_objects();
    ret = k_work_reschedule(&test_timeout_work, K_MSEC(1000));
    zassert_true(ret >= 0, "k_work_reschedule failed: %d", ret);
    zassert_ok(k_msgq_put(&test_mesh_rx_msgq, &rx_marker, K_NO_WAIT));

    zassert_ok(app_mesh_apply_click_preempt_plan(&plan, &ops, &result));

    zassert_true(result.outbox_cleared);
    zassert_true(result.timeout_cancelled);
    zassert_true(result.rx_queue_purged);
    zassert_true(result.click_report_requeued);
    zassert_false(result.outbox_saved);
    zassert_false(result.timeout_scheduled);
    zassert_equal(save_count, 0u);
    zassert_equal(clear_count, 1u);
    zassert_equal(schedule_count, 0u);
    zassert_false(k_work_delayable_is_pending(&test_timeout_work));
    zassert_equal(k_msgq_num_used_get(&test_mesh_rx_msgq), 0u);
    zassert_ok(k_msgq_get(&test_report_tx_msgq, &requeued, K_NO_WAIT));
    zassert_mem_equal(&requeued, &plan.click_report, sizeof(requeued));
}

ZTEST_SUITE(mesh_preemption_app,
            NULL,
            NULL,
            NULL,
            reset_runtime_objects_after,
            NULL);

static int main_init(void)
{
    k_work_init_delayable(&test_timeout_work, timeout_handler);
    return 0;
}

SYS_INIT(main_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
