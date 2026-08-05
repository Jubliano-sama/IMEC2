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
    enum call_kind calls[24];
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
    struct app_mesh_gateway_command_priority_failure failure;
    int work;

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 11u) == 0);
    assert(app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority));
    assert(fixture.call_count == 1u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == 0);
    assert(failure.generation == 0u);
    assert(failure.admission_cutoff == 0u);
    assert(fixture.call_count == 2u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(fixture.calls[1] == CALL_RESCHEDULE);
    assert(fixture.scheduled_work == &work);
}

static void test_retryable_schedule_failure_keeps_exact_generation_until_bound(void)
{
    struct priority_fixture fixture = {
        .reschedule_ret = -EBUSY,
    };
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    struct app_mesh_gateway_command_priority_failure failure;
    int work;

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 41u) == 0);
    assert(fixture.call_count == 1u);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == -EBUSY);
    assert(failure.generation == 0u);
    assert(failure.admission_cutoff == 0u);
    assert(!app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority));
    assert(app_mesh_gateway_command_priority_schedule_retry_pending(&priority));
    assert(app_mesh_gateway_command_priority_active(&priority));
    assert(fixture.call_count == 2u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(fixture.calls[1] == CALL_RESCHEDULE);

    /*
     * Once a scheduling attempt has failed, a newer admission may not extend
     * the generation that will be retired if the bounded retry exhausts.
     */
    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 42u) == 0);
    assert(priority.admission_cutoff == 41u);
    for (uint8_t attempt = 1u;
         attempt < APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS;
         attempt++) {
        assert(app_mesh_gateway_command_priority_retry_schedule(
                   &priority, &ops, &failure) == -EBUSY);
    }
    assert(!app_mesh_gateway_command_priority_active(&priority));
    assert(failure.generation == 1u);
    assert(failure.admission_cutoff == 41u);
    assert(fixture.call_count ==
           APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS + 2u);
    assert(fixture.calls[fixture.call_count - 1u] == CALL_CLEAR);

    fixture.reschedule_ret = 0;
    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 42u) == 0);
    assert(priority.generation == 2u);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == 0);
    assert(!app_mesh_gateway_command_priority_active(&priority));
    assert(failure.generation == 0u);
}

static void test_commands_before_boundary_share_the_latest_cutoff(void)
{
    struct priority_fixture fixture = {0};
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    struct app_mesh_gateway_command_priority_failure failure;
    int work;

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 21u) == 0);
    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 23u) == 0);
    assert(priority.generation == 1u);
    assert(priority.admission_cutoff == 23u);
    assert(fixture.call_count == 1u);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == 0);
}

static void test_successful_handoff_requires_a_new_generation(void)
{
    struct priority_fixture fixture = {0};
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    struct app_mesh_gateway_command_priority_failure failure;
    int work;

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 31u) == 0);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == 0);
    assert(fixture.call_count == 2u);
    assert(fixture.calls[0] == CALL_ABORT);
    assert(fixture.calls[1] == CALL_RESCHEDULE);

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 32u) == 0);
    assert(app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority));
    assert(priority.generation == 2u);
    assert(fixture.call_count == 3u);
    assert(fixture.calls[2] == CALL_ABORT);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == 0);
    assert(fixture.call_count == 4u);
    assert(fixture.calls[3] == CALL_RESCHEDULE);
}

static void test_nonretryable_schedule_failure_retires_immediately(void)
{
    struct priority_fixture fixture = {
        .reschedule_ret = -EIO,
    };
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    struct app_mesh_gateway_command_priority_failure failure;
    int work;

    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 51u) == 0);
    assert(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
               &priority, &ops, &failure) == -EIO);
    assert(failure.generation == 1u);
    assert(failure.admission_cutoff == 51u);
    assert(!app_mesh_gateway_command_priority_active(&priority));
    assert(fixture.call_count == 3u);
    assert(fixture.calls[2] == CALL_CLEAR);
}

static void test_non_gateway_cannot_preempt_radio(void)
{
    struct priority_fixture fixture = {0};
    struct app_mesh_gateway_command_priority_ops ops = ops_for(&fixture);
    struct app_mesh_gateway_command_priority priority = {0};
    int work;

    ops.gateway_role = false;
    assert(app_mesh_gateway_command_priority_request(
               &priority, &ops, &work, 1u) == -EINVAL);
    assert(fixture.call_count == 0u);
}

int main(void)
{
    test_abort_happens_before_command_reschedule();
    test_retryable_schedule_failure_keeps_exact_generation_until_bound();
    test_commands_before_boundary_share_the_latest_cutoff();
    test_successful_handoff_requires_a_new_generation();
    test_nonretryable_schedule_failure_retires_immediately();
    test_non_gateway_cannot_preempt_radio();
    return 0;
}
