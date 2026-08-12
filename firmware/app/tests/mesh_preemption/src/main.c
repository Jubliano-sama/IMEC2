#include "app_mesh_preemption.h"

#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>

enum test_custody_owner {
    TEST_CUSTODY_ACTIVE = 0,
    TEST_CUSTODY_DEFERRED,
    TEST_CUSTODY_CLICK_REPORT,
};

static enum test_custody_owner custody_owner;
static uint8_t transfer_count;
static uint8_t defer_count;
static uint8_t schedule_count;
static uint8_t fail_stop_count;
static int transfer_ret;
static int defer_ret;
static int schedule_ret;

static int transfer_local_click(void *ctx, const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    transfer_count++;
    if (transfer_ret == 0) {
        custody_owner = TEST_CUSTODY_CLICK_REPORT;
    }
    return transfer_ret;
}

static int defer_active_tx(void *ctx)
{
    ARG_UNUSED(ctx);

    defer_count++;
    if (defer_ret == 0) {
        custody_owner = TEST_CUSTODY_DEFERRED;
    }
    return defer_ret;
}

static int schedule_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    schedule_count++;
    return schedule_ret;
}

static void fail_stop(void *ctx)
{
    ARG_UNUSED(ctx);

    fail_stop_count++;
}

static void reset_runtime_objects(void)
{
    custody_owner = TEST_CUSTODY_ACTIVE;
    transfer_count = 0u;
    defer_count = 0u;
    schedule_count = 0u;
    fail_stop_count = 0u;
    transfer_ret = 0;
    defer_ret = 0;
    schedule_ret = 0;
}

static void reset_runtime_objects_after(void *fixture)
{
    ARG_UNUSED(fixture);

    reset_runtime_objects();
}

static struct app_mesh_click_preempt_ops make_ops(void)
{
    return (struct app_mesh_click_preempt_ops) {
        .transfer_local_click = transfer_local_click,
        .defer_active_tx = defer_active_tx,
        .schedule_timeout = schedule_timeout,
        .fail_stop = fail_stop,
    };
}

ZTEST(mesh_preemption_app, test_local_click_transfer_is_single_fallible_step)
{
    struct mesh_click_preempt_plan plan = {
        .transfer_local_click = true,
    };
    struct app_mesh_click_preempt_ops ops = make_ops();
    struct app_mesh_click_preempt_result result;

    reset_runtime_objects();

    zassert_ok(app_mesh_apply_click_preempt_plan(&plan, &ops, &result));
    zassert_true(result.local_click_transferred);
    zassert_true(result.transaction_committed);
    zassert_equal(result.custody_owner,
                  APP_MESH_CLICK_PREEMPT_OWNER_CLICK_REPORT);
    zassert_equal(transfer_count, 1u);
    zassert_equal(defer_count, 0u);
    zassert_equal(schedule_count, 0u);
    zassert_equal(custody_owner, TEST_CUSTODY_CLICK_REPORT);

    reset_runtime_objects();
    transfer_ret = -ENOSPC;
    zassert_equal(app_mesh_apply_click_preempt_plan(&plan, &ops, &result),
                  -ENOSPC);
    zassert_false(result.local_click_transferred);
    zassert_equal(result.custody_owner,
                  APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME);
    zassert_equal(transfer_count, 1u);
    zassert_equal(custody_owner, TEST_CUSTODY_ACTIVE);
}

ZTEST(mesh_preemption_app, test_nonclick_deferral_retains_runtime_then_schedules)
{
    struct mesh_click_preempt_plan plan = {
        .defer_active_tx = true,
        .schedule_timeout = true,
    };
    struct app_mesh_click_preempt_ops ops = make_ops();
    struct app_mesh_click_preempt_result result;

    reset_runtime_objects();

    zassert_ok(app_mesh_apply_click_preempt_plan(&plan, &ops, &result));
    zassert_true(result.active_tx_deferred);
    zassert_true(result.timeout_scheduled);
    zassert_true(result.transaction_committed);
    zassert_equal(result.custody_owner,
                  APP_MESH_CLICK_PREEMPT_OWNER_DEFERRED_RUNTIME);
    zassert_equal(defer_count, 1u);
    zassert_equal(schedule_count, 1u);
    zassert_equal(transfer_count, 0u);
    zassert_equal(fail_stop_count, 0u);
    zassert_equal(custody_owner, TEST_CUSTODY_DEFERRED);

    reset_runtime_objects();
    defer_ret = -EAGAIN;
    zassert_equal(app_mesh_apply_click_preempt_plan(&plan, &ops, &result),
                  -EAGAIN);
    zassert_false(result.active_tx_deferred);
    zassert_equal(schedule_count, 0u);
    zassert_equal(custody_owner, TEST_CUSTODY_ACTIVE);
}

ZTEST(mesh_preemption_app, test_scheduler_failure_after_deferral_fails_closed)
{
    struct mesh_click_preempt_plan plan = {
        .defer_active_tx = true,
        .schedule_timeout = true,
    };
    struct app_mesh_click_preempt_ops ops = make_ops();
    struct app_mesh_click_preempt_result result;

    reset_runtime_objects();
    schedule_ret = -ESHUTDOWN;

    zassert_equal(app_mesh_apply_click_preempt_plan(&plan, &ops, &result),
                  -ESHUTDOWN);
    zassert_true(result.active_tx_deferred);
    zassert_false(result.timeout_scheduled);
    zassert_true(result.fail_stop_requested);
    zassert_equal(result.custody_owner,
                  APP_MESH_CLICK_PREEMPT_OWNER_DEFERRED_RUNTIME);
    zassert_equal(defer_count, 1u);
    zassert_equal(schedule_count, 1u);
    zassert_equal(fail_stop_count, 1u);
    zassert_equal(custody_owner, TEST_CUSTODY_DEFERRED);
}

ZTEST_SUITE(mesh_preemption_app,
            NULL,
            NULL,
            NULL,
            reset_runtime_objects_after,
            NULL);
