#include "app_mesh_tx_handoff_gate.h"

#include <assert.h>
#include <stddef.h>

struct handoff_gate_test_ctx {
    enum app_mesh_tx_handoff_work work;
    enum app_mesh_tx_handoff_reason reason;
    uint32_t delay_ms;
    int schedule_ret;
    int schedule_count;
};

static int schedule_retry(enum app_mesh_tx_handoff_work work,
                          enum app_mesh_tx_handoff_reason reason,
                          uint32_t delay_ms,
                          void *ctx)
{
    struct handoff_gate_test_ctx *test = ctx;

    assert(test != NULL);
    test->work = work;
    test->reason = reason;
    test->delay_ms = delay_ms;
    test->schedule_count++;
    return test->schedule_ret;
}

static struct app_mesh_tx_handoff_ops handoff_ops(
    struct handoff_gate_test_ctx *ctx)
{
    const struct app_mesh_tx_handoff_ops ops = {
        .schedule_retry = schedule_retry,
        .ctx = ctx,
    };

    return ops;
}

static void test_route_reply_handoff_defers_queued_result_work(void)
{
    struct handoff_gate_test_ctx ctx = {0};
    const struct app_mesh_tx_handoff_ops ops = handoff_ops(&ctx);
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = true,
        .route_reply_handoff_active = true,
        .work = APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
        .retry_delay_ms = 25u,
    };
    struct app_mesh_tx_handoff_result result;

    assert(app_mesh_tx_handoff_gate_yield(&state, &ops, &result));
    assert(result.yield);
    assert(result.keep_queued_tx);
    assert(result.retry_scheduled);
    assert(result.schedule_ret == 0);
    assert(result.work == APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE);
    assert(result.reason == APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY);
    assert(result.retry_delay_ms == 25u);
    assert(ctx.schedule_count == 1);
    assert(ctx.work == APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE);
    assert(ctx.reason == APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY);
    assert(ctx.delay_ms == 25u);
}

static void test_rx_control_handoff_defers_route_waiting_work(void)
{
    struct handoff_gate_test_ctx ctx = {0};
    const struct app_mesh_tx_handoff_ops ops = handoff_ops(&ctx);
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = true,
        .route_reply_handoff_active = true,
        .rx_control_handoff_active = true,
        .work = APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING,
        .retry_delay_ms = 40u,
    };
    struct app_mesh_tx_handoff_result result;

    assert(app_mesh_tx_handoff_gate_yield(&state, &ops, &result));
    assert(result.yield);
    assert(result.keep_queued_tx);
    assert(result.retry_scheduled);
    assert(result.work == APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING);
    assert(result.reason == APP_MESH_TX_HANDOFF_REASON_RX_CONTROL);
    assert(ctx.schedule_count == 1);
    assert(ctx.work == APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING);
    assert(ctx.reason == APP_MESH_TX_HANDOFF_REASON_RX_CONTROL);
    assert(ctx.delay_ms == 40u);
}

static void test_handoff_gate_keeps_work_even_when_retry_schedule_fails(void)
{
    struct handoff_gate_test_ctx ctx = {
        .schedule_ret = -5,
    };
    const struct app_mesh_tx_handoff_ops ops = handoff_ops(&ctx);
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = true,
        .rx_control_handoff_active = true,
        .work = APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
        .retry_delay_ms = 15u,
    };
    struct app_mesh_tx_handoff_result result;

    assert(app_mesh_tx_handoff_gate_yield(&state, &ops, &result));
    assert(result.yield);
    assert(result.keep_queued_tx);
    assert(!result.retry_scheduled);
    assert(result.schedule_ret == -5);
    assert(ctx.schedule_count == 1);
}

static void test_non_blocked_queued_work_is_allowed_through(void)
{
    struct handoff_gate_test_ctx ctx = {0};
    const struct app_mesh_tx_handoff_ops ops = handoff_ops(&ctx);
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = true,
        .work = APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
        .retry_delay_ms = 25u,
    };
    struct app_mesh_tx_handoff_result result;

    assert(!app_mesh_tx_handoff_gate_yield(&state, &ops, &result));
    assert(!result.yield);
    assert(!result.keep_queued_tx);
    assert(!result.retry_scheduled);
    assert(result.reason == APP_MESH_TX_HANDOFF_REASON_NONE);
    assert(ctx.schedule_count == 0);
}

static void test_idle_handoff_gate_does_not_schedule_without_queued_work(void)
{
    struct handoff_gate_test_ctx ctx = {0};
    const struct app_mesh_tx_handoff_ops ops = handoff_ops(&ctx);
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = false,
        .route_reply_handoff_active = true,
        .rx_control_handoff_active = true,
        .work = APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
        .retry_delay_ms = 25u,
    };
    struct app_mesh_tx_handoff_result result;

    assert(!app_mesh_tx_handoff_gate_yield(&state, &ops, &result));
    assert(!result.yield);
    assert(!result.keep_queued_tx);
    assert(!result.retry_scheduled);
    assert(result.reason == APP_MESH_TX_HANDOFF_REASON_NONE);
    assert(ctx.schedule_count == 0);
}

int main(void)
{
    test_route_reply_handoff_defers_queued_result_work();
    test_rx_control_handoff_defers_route_waiting_work();
    test_handoff_gate_keeps_work_even_when_retry_schedule_fails();
    test_non_blocked_queued_work_is_allowed_through();
    test_idle_handoff_gate_does_not_schedule_without_queued_work();
    return 0;
}
