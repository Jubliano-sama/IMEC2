#include "app_mesh_gateway_command_priority.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>

enum call_kind {
    CALL_ABORT = 1,
    CALL_RESCHEDULE,
    CALL_CLEAR,
};

struct priority_fixture {
    enum call_kind calls[3];
    size_t call_count;
    void *scheduled_work;
    int reschedule_ret;
};

static void request_abort(void *ctx)
{
    struct priority_fixture *fixture = ctx;

    fixture->calls[fixture->call_count++] = CALL_ABORT;
}

static int reschedule_now(void *ctx, void *work)
{
    struct priority_fixture *fixture = ctx;

    fixture->calls[fixture->call_count++] = CALL_RESCHEDULE;
    fixture->scheduled_work = work;
    return fixture->reschedule_ret;
}

static void clear_abort(void *ctx)
{
    struct priority_fixture *fixture = ctx;

    fixture->calls[fixture->call_count++] = CALL_CLEAR;
}

static struct app_mesh_gateway_command_priority_ops ops_for(
    struct priority_fixture *fixture)
{
    return (struct app_mesh_gateway_command_priority_ops){
        .gateway_role = true,
        .request_receive_abort = request_abort,
        .reschedule_now = reschedule_now,
        .clear_receive_abort = clear_abort,
        .ctx = fixture,
    };
}

static void test_abort_happens_before_command_reschedule(void)
{
    struct priority_fixture fixture = {0};
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    int work;

    assert(app_mesh_gateway_command_priority_request(&priority, &ops, &work) == 0);
    assert(app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority));
    assert(fixture.call_count == 1u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops) == 0);
    assert(fixture.call_count == 2u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(fixture.calls[1] == CALL_RESCHEDULE);
    assert(fixture.scheduled_work == &work);
}

static void test_failed_safe_boundary_schedule_clears_abort_request(void)
{
    struct priority_fixture fixture = {
        .reschedule_ret = -EBUSY,
    };
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    int work;

    assert(app_mesh_gateway_command_priority_request(&priority, &ops, &work) == 0);
    assert(fixture.call_count == 1u);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops) == -EBUSY);
    assert(!app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority));
    assert(fixture.call_count == 3u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(fixture.calls[1] == CALL_RESCHEDULE);
    assert(fixture.calls[2] == CALL_CLEAR);
}

static void test_non_gateway_cannot_preempt_radio(void)
{
    struct priority_fixture fixture = {0};
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    int work;

    ops.gateway_role = false;
    assert(app_mesh_gateway_command_priority_request(&priority, &ops, &work) == -EINVAL);
    assert(fixture.call_count == 0u);
}

int main(void)
{
    test_abort_happens_before_command_reschedule();
    test_failed_safe_boundary_schedule_clears_abort_request();
    test_non_gateway_cannot_preempt_radio();
    return 0;
}
