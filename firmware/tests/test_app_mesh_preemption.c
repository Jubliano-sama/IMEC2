#include "app_mesh_preemption.h"
#include "mesh_capacity.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

_Static_assert(MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY ==
                   MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH +
                       MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY,
               "report storage must include the queue ownership reserve");
_Static_assert(MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY == 1u,
               "queue recovery fixture models exactly one reserve entry");

enum fixture_owner {
    FIXTURE_OWNER_ACTIVE = 0,
    FIXTURE_OWNER_DEFERRED_ACTIVE,
    FIXTURE_OWNER_CLICK_REPORT,
};

struct preempt_fixture {
    enum fixture_owner owner;
    bool active_present;
    bool click_report_present;
    bool retry_scheduled;
    struct mesh_outbound click_report;
    uint8_t transfer_calls;
    uint8_t defer_calls;
    uint8_t schedule_calls;
    uint8_t fail_stop_calls;
    uint8_t next_order;
    uint8_t defer_order;
    uint8_t schedule_order;
    int transfer_ret;
    int defer_ret;
    int schedule_ret;
};

struct queue_remove_fixture {
    struct mesh_outbound
        entries[MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY];
    uint8_t count;
    uint8_t get_calls;
    uint8_t put_calls;
    uint8_t fail_get_call;
    uint8_t fail_put_call;
    bool fill_on_put_failure;
    bool recovery_valid;
    struct mesh_outbound recovery;
};

static uint32_t queue_remove_count(void *ctx)
{
    return ((struct queue_remove_fixture *)ctx)->count;
}

static int queue_remove_get(struct mesh_outbound *outbound, void *ctx)
{
    struct queue_remove_fixture *fixture = ctx;

    fixture->get_calls++;
    if (fixture->fail_get_call == fixture->get_calls) {
        return -EIO;
    }
    if (fixture->count == 0u) {
        return -ENOENT;
    }
    *outbound = fixture->entries[0];
    memmove(&fixture->entries[0], &fixture->entries[1],
            (fixture->count - 1u) * sizeof(fixture->entries[0]));
    fixture->count--;
    return 0;
}

static int queue_remove_peek(struct mesh_outbound *outbound, void *ctx)
{
    const struct queue_remove_fixture *fixture = ctx;

    if (fixture->count == 0u) {
        return -ENOENT;
    }
    *outbound = fixture->entries[0];
    return 0;
}

static int queue_remove_put(const struct mesh_outbound *outbound, void *ctx)
{
    struct queue_remove_fixture *fixture = ctx;

    fixture->put_calls++;
    if (fixture->fail_put_call == fixture->put_calls) {
        if (fixture->fill_on_put_failure) {
            assert(fixture->count <
                   MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY);
            fixture->entries[fixture->count++].packet.seq = 99u;
        }
        return -ENOSPC;
    }
    assert(fixture->count < MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY);
    fixture->entries[fixture->count++] = *outbound;
    return 0;
}

static int queue_remove_recover(const struct mesh_outbound *outbound, void *ctx)
{
    struct queue_remove_fixture *fixture = ctx;

    if (fixture->recovery_valid) {
        return -ENOSPC;
    }
    fixture->recovery = *outbound;
    fixture->recovery_valid = true;
    return 0;
}

static bool queue_remove_matches(const struct mesh_outbound *candidate,
                                 const struct mesh_outbound *target,
                                 void *ctx)
{
    (void)ctx;
    return candidate->packet.seq == target->packet.seq;
}

static struct app_mesh_queue_remove_ops queue_remove_ops(
    struct queue_remove_fixture *fixture)
{
    return (struct app_mesh_queue_remove_ops) {
        .count = queue_remove_count,
        .get = queue_remove_get,
        .put = queue_remove_put,
        .recover = queue_remove_recover,
        .matches = queue_remove_matches,
        .ctx = fixture,
    };
}

static struct app_mesh_queue_head_ops queue_head_ops(
    struct queue_remove_fixture *fixture)
{
    return (struct app_mesh_queue_head_ops) {
        .peek = queue_remove_peek,
        .get = queue_remove_get,
        .recover = queue_remove_recover,
        .matches = queue_remove_matches,
        .ctx = fixture,
    };
}

static struct queue_remove_fixture queue_remove_fixture(void)
{
    struct queue_remove_fixture fixture = {
        .count = MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH,
    };

    for (uint8_t i = 0u; i < fixture.count; i++) {
        fixture.entries[i].packet.seq = (uint8_t)(i + 1u);
    }
    return fixture;
}

static int transfer_local_click(void *ctx, const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->transfer_calls++;
    if (fixture->transfer_ret < 0) {
        return fixture->transfer_ret;
    }
    assert(fixture->active_present);
    fixture->click_report = *outbound;
    fixture->active_present = false;
    fixture->click_report_present = true;
    fixture->owner = FIXTURE_OWNER_CLICK_REPORT;
    return 0;
}

static int defer_active_tx(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->defer_calls++;
    fixture->defer_order = ++fixture->next_order;
    if (fixture->defer_ret < 0) {
        return fixture->defer_ret;
    }
    assert(fixture->active_present);
    fixture->owner = FIXTURE_OWNER_DEFERRED_ACTIVE;
    return 0;
}

static int schedule_timeout(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->schedule_calls++;
    fixture->schedule_order = ++fixture->next_order;
    if (fixture->schedule_ret < 0) {
        return fixture->schedule_ret;
    }
    fixture->retry_scheduled = true;
    return 0;
}

static void fail_stop(void *ctx)
{
    ((struct preempt_fixture *)ctx)->fail_stop_calls++;
}

static struct app_mesh_click_preempt_ops ops_for(struct preempt_fixture *fixture)
{
    return (struct app_mesh_click_preempt_ops) {
        .transfer_local_click = transfer_local_click,
        .defer_active_tx = defer_active_tx,
        .schedule_timeout = schedule_timeout,
        .fail_stop = fail_stop,
        .ctx = fixture,
    };
}

static int apply_plan(const struct mesh_click_preempt_plan *plan,
                      struct preempt_fixture *fixture,
                      struct app_mesh_click_preempt_result *result)
{
    const struct app_mesh_click_preempt_ops ops = ops_for(fixture);

    return app_mesh_apply_click_preempt_plan(plan, &ops, result);
}

static struct preempt_fixture active_fixture(void)
{
    return (struct preempt_fixture) {
        .owner = FIXTURE_OWNER_ACTIVE,
        .active_present = true,
    };
}

static void assert_one_owner(const struct preempt_fixture *fixture)
{
    assert((fixture->active_present ? 1u : 0u) +
           (fixture->click_report_present ? 1u : 0u) == 1u);
}

static void test_local_transfer_is_one_fallible_operation(void)
{
    const struct mesh_click_preempt_plan plan = {
        .transfer_local_click = true,
        .click_report = {
            .packet = {.msg_type = MSG_CLICK_REPORT, .seq = 7u},
        },
    };
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();

    fixture.transfer_ret = -ENOSPC;
    assert(apply_plan(&plan, &fixture, &result) == -ENOSPC);
    assert(fixture.transfer_calls == 1u && fixture.schedule_calls == 0u);
    assert(fixture.active_present && !fixture.click_report_present);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME);
    assert(!result.local_click_transferred);

    fixture = active_fixture();
    assert(apply_plan(&plan, &fixture, &result) == 0);
    assert(fixture.transfer_calls == 1u && fixture.schedule_calls == 0u);
    assert(!fixture.active_present && fixture.click_report_present);
    assert(fixture.click_report.packet.seq == 7u);
    assert(result.local_click_transferred && result.transaction_committed);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_CLICK_REPORT);
    assert_one_owner(&fixture);
}

static void test_retained_relay_schedules_only_after_deferral(void)
{
    const struct mesh_click_preempt_plan plan = {
        .defer_active_tx = true,
        .schedule_timeout = true,
    };
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();

    assert(apply_plan(&plan, &fixture, &result) == 0);
    assert(fixture.active_present && !fixture.click_report_present);
    assert(fixture.defer_calls == 1u && fixture.schedule_calls == 1u);
    assert(fixture.defer_order == 1u && fixture.schedule_order == 2u);
    assert(fixture.retry_scheduled);
    assert(result.active_tx_deferred && result.timeout_scheduled);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DEFERRED_RUNTIME);
    assert_one_owner(&fixture);
}

static void test_retained_relay_scheduler_failure_fails_closed(void)
{
    const struct mesh_click_preempt_plan plan = {
        .defer_active_tx = true,
        .schedule_timeout = true,
    };
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();

    fixture.schedule_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.active_present && !fixture.click_report_present);
    assert(fixture.defer_calls == 1u && fixture.schedule_calls == 1u);
    assert(fixture.fail_stop_calls == 1u);
    assert(result.active_tx_deferred && !result.timeout_scheduled);
    assert(result.fail_stop_requested);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DEFERRED_RUNTIME);
    assert_one_owner(&fixture);
}

static void test_invalid_mixed_plan_is_rejected_without_callbacks(void)
{
    const struct mesh_click_preempt_plan plan = {
        .transfer_local_click = true,
        .defer_active_tx = true,
    };
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();

    assert(apply_plan(&plan, &fixture, &result) == -EINVAL);
    assert(fixture.transfer_calls == 0u && fixture.defer_calls == 0u);
    assert_one_owner(&fixture);
}

static void test_timeout_work_predicate_remains_conservative(void)
{
    assert(!app_mesh_tx_timeout_work_needed(false, false, false));
    assert(app_mesh_tx_timeout_work_needed(true, false, false));
    assert(app_mesh_tx_timeout_work_needed(false, true, false));
    assert(app_mesh_tx_timeout_work_needed(false, false, true));
}

static void test_queue_remove_rotates_once_and_preserves_relative_order(void)
{
    struct queue_remove_fixture fixture = queue_remove_fixture();
    struct app_mesh_queue_remove_ops ops = queue_remove_ops(&fixture);
    struct mesh_outbound target = {.packet.seq = 2u};
    struct mesh_outbound scratch;
    bool removed = false;

    assert(app_mesh_queue_remove_first(&ops, &target, &scratch, &removed) == 0);
    assert(removed);
    assert(fixture.count ==
           MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH - 1u);
    assert(fixture.get_calls ==
           MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH);
    assert(fixture.put_calls ==
           MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH - 1u);
    assert(fixture.entries[0].packet.seq == 1u);
    for (uint8_t i = 1u; i < fixture.count; i++) {
        assert(fixture.entries[i].packet.seq == i + 2u);
    }
}

static void test_queue_remove_absent_target_preserves_order(void)
{
    struct queue_remove_fixture fixture = queue_remove_fixture();
    struct app_mesh_queue_remove_ops ops = queue_remove_ops(&fixture);
    struct mesh_outbound target = {
        .packet.seq = MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH + 1u,
    };
    struct mesh_outbound scratch;
    bool removed = true;

    assert(app_mesh_queue_remove_first(&ops, &target, &scratch, &removed) ==
           -ENOENT);
    assert(!removed);
    assert(fixture.count == MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH);
    for (uint8_t i = 0u; i < fixture.count; i++) {
        assert(fixture.entries[i].packet.seq == i + 1u);
    }
}

static void test_queue_remove_reports_get_and_put_failures(void)
{
    struct queue_remove_fixture get_failure = queue_remove_fixture();
    struct queue_remove_fixture put_failure = queue_remove_fixture();
    struct mesh_outbound target = {.packet.seq = 4u};
    struct mesh_outbound scratch;
    bool removed = true;

    get_failure.fail_get_call = 2u;
    {
        struct app_mesh_queue_remove_ops ops = queue_remove_ops(&get_failure);

        assert(app_mesh_queue_remove_first(&ops, &target, &scratch, &removed) ==
               -EIO);
        assert(!removed);
        assert(get_failure.get_calls == 2u);
    }

    removed = true;
    put_failure.fail_put_call = 1u;
    put_failure.fill_on_put_failure = true;
    {
        struct app_mesh_queue_remove_ops ops = queue_remove_ops(&put_failure);

        assert(app_mesh_queue_remove_first(&ops, &target, &scratch, &removed) ==
               -ENOSPC);
        assert(!removed);
        assert(put_failure.get_calls == 1u);
        assert(put_failure.put_calls == 1u);
        assert(put_failure.recovery_valid);
        assert(put_failure.recovery.packet.seq == 1u);
        assert(put_failure.count ==
               MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH);
        for (uint8_t i = 0u; i + 1u < put_failure.count; i++) {
            assert(put_failure.entries[i].packet.seq == i + 2u);
        }
        assert(put_failure.entries[put_failure.count - 1u].packet.seq == 99u);
    }
}

static void test_queue_head_owner_blocks_rotation_but_allows_append(void)
{
    struct queue_remove_fixture fixture = queue_remove_fixture();
    struct app_mesh_queue_remove_ops remove_ops = queue_remove_ops(&fixture);
    struct app_mesh_queue_head_ops head_ops = queue_head_ops(&fixture);
    struct app_mesh_queue_head_owner owner;
    struct app_mesh_queue_head_token token;
    struct mesh_outbound expected;
    struct mesh_outbound removed;
    struct mesh_outbound target = {.packet.seq = 2u};
    struct mesh_outbound appended = {
        .packet.seq = MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH + 1u,
    };
    bool target_removed = true;

    app_mesh_queue_head_owner_init(&owner);
    assert(app_mesh_queue_head_begin(&owner, &head_ops, &expected, &token) == 0);
    assert(expected.packet.seq == 1u);
    assert(app_mesh_queue_head_owned(&owner));

    /* A producer may append while the sender owns the immutable head. */
    assert(queue_remove_put(&appended, &fixture) == 0);
    assert(fixture.count ==
           MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY);
    assert(app_mesh_queue_remove_first_owned(&owner,
                                             &remove_ops,
                                             &target,
                                             &removed,
                                             &target_removed) == -EBUSY);
    assert(!target_removed);
    assert(fixture.get_calls == 0u);
    assert(fixture.entries[0].packet.seq == 1u);

    assert(app_mesh_queue_head_commit(&owner,
                                      &head_ops,
                                      &token,
                                      &expected,
                                      &removed) == 0);
    assert(removed.packet.seq == 1u);
    assert(!app_mesh_queue_head_owned(&owner));
    assert(fixture.count == MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH);
    assert(fixture.entries[0].packet.seq == 2u);
    assert(fixture.entries[fixture.count - 1u].packet.seq ==
           MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH + 1u);
}

static void test_queue_head_owner_rejects_stale_or_changed_head(void)
{
    struct queue_remove_fixture fixture = queue_remove_fixture();
    struct app_mesh_queue_head_ops ops = queue_head_ops(&fixture);
    struct app_mesh_queue_head_owner owner;
    struct app_mesh_queue_head_token token;
    struct app_mesh_queue_head_token stale;
    struct mesh_outbound expected;
    struct mesh_outbound removed;

    app_mesh_queue_head_owner_init(&owner);
    assert(app_mesh_queue_head_begin(&owner, &ops, &expected, &token) == 0);
    stale = token;
    stale.generation++;
    assert(app_mesh_queue_head_commit(&owner,
                                      &ops,
                                      &stale,
                                      &expected,
                                      &removed) == -ESTALE);
    assert(fixture.count == MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH &&
           fixture.get_calls == 0u);

    /* Model an illegal unowned destructive interleaving: fail closed. */
    fixture.entries[0].packet.seq = 99u;
    assert(app_mesh_queue_head_commit(&owner,
                                      &ops,
                                      &token,
                                      &expected,
                                      &removed) == -ESTALE);
    assert(fixture.count == MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH &&
           fixture.get_calls == 0u);
    assert(app_mesh_queue_head_abort(&owner, &token) == 0);
}

static void test_parent_loss_keeps_exactly_one_retry_owner(void)
{
    assert(app_mesh_parent_loss_custody_decide(
               true, true, true) == APP_MESH_PARENT_LOSS_CUSTODY_CORE_RETRY);
    assert(app_mesh_parent_loss_custody_decide(
               true, false, true) == APP_MESH_PARENT_LOSS_CUSTODY_ROUTE_WAIT);
    assert(app_mesh_parent_loss_custody_decide(
               true, false, false) == APP_MESH_PARENT_LOSS_CUSTODY_NONE);
    assert(app_mesh_parent_loss_custody_decide(
               true, true, false) == APP_MESH_PARENT_LOSS_CUSTODY_NONE);
    assert(app_mesh_parent_loss_custody_decide(
               false, true, true) == APP_MESH_PARENT_LOSS_CUSTODY_NONE);
}

static void test_queue_recovery_reserve_never_replaces_existing_custody(void)
{
    assert(app_mesh_queue_reserve_decide(false, false, false) ==
           APP_MESH_QUEUE_RESERVE_ADMIT);
    assert(app_mesh_queue_reserve_decide(true, false, false) ==
           APP_MESH_QUEUE_RESERVE_REJECT);
    assert(app_mesh_queue_reserve_decide(true, false, true) ==
           APP_MESH_QUEUE_RESERVE_REJECT);
    assert(app_mesh_queue_reserve_decide(true, true, true) ==
           APP_MESH_QUEUE_RESERVE_REJECT);
    assert(app_mesh_queue_reserve_decide(true, true, false) ==
           APP_MESH_QUEUE_RESERVE_REJECT);
}

int main(void)
{
    test_local_transfer_is_one_fallible_operation();
    test_retained_relay_schedules_only_after_deferral();
    test_retained_relay_scheduler_failure_fails_closed();
    test_invalid_mixed_plan_is_rejected_without_callbacks();
    test_timeout_work_predicate_remains_conservative();
    test_queue_remove_rotates_once_and_preserves_relative_order();
    test_queue_remove_absent_target_preserves_order();
    test_queue_remove_reports_get_and_put_failures();
    test_queue_head_owner_blocks_rotation_but_allows_append();
    test_queue_head_owner_rejects_stale_or_changed_head();
    test_parent_loss_keeps_exactly_one_retry_owner();
    test_queue_recovery_reserve_never_replaces_existing_custody();
    return 0;
}
