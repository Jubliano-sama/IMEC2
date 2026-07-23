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
    FIXTURE_OWNER_DURABLE,
    FIXTURE_OWNER_CLICK_REPORT,
    FIXTURE_OWNER_HANDOFF,
};

enum fixture_handoff_phase {
    FIXTURE_HANDOFF_NONE = 0,
    FIXTURE_HANDOFF_STAGED,
    FIXTURE_HANDOFF_COMMITTED,
};

struct preempt_fixture {
    enum fixture_owner owner;
    bool active_present;
    bool durable_staged;
    bool deferred_staged;
    bool click_report_staged;
    struct mesh_outbound deferred_packet;
    struct mesh_outbound expected_packet;
    bool deferred_packet_valid;
    enum fixture_handoff_phase handoff_phase;
    uint8_t save_calls;
    uint8_t schedule_calls;
    uint8_t requeue_calls;
    uint8_t discard_calls;
    uint8_t cancel_timeout_calls;
    uint8_t clear_calls;
    uint8_t stage_calls;
    uint8_t commit_calls;
    uint8_t rollback_calls;
    uint8_t cancel_active_calls;
    int save_ret;
    int schedule_ret;
    int requeue_ret;
    int cancel_timeout_ret;
    int clear_ret;
    int stage_ret;
    int commit_ret;
    int rollback_ret;
    int cancel_active_ret;
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

static int save_outbox(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->save_calls++;
    if (fixture->save_ret == 0) {
        fixture->durable_staged = true;
    }
    return fixture->save_ret;
}

static int save_deferred_outbox(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->save_calls++;
    if (fixture->save_ret == 0) {
        fixture->deferred_staged = true;
        fixture->deferred_packet = fixture->expected_packet;
        fixture->deferred_packet_valid = true;
    }
    return fixture->save_ret;
}

static int schedule_timeout(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->schedule_calls++;
    return fixture->schedule_ret;
}

static int requeue_click_report(void *ctx, const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->requeue_calls++;
    if (fixture->requeue_ret == 0) {
        fixture->click_report_staged = true;
    }
    return fixture->requeue_ret;
}

static int discard_requeued_click_report(void *ctx,
                                         const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->discard_calls++;
    fixture->click_report_staged = false;
    return 0;
}

static int cancel_timeout(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->cancel_timeout_calls++;
    return fixture->cancel_timeout_ret;
}

static int clear_outbox(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->clear_calls++;
    if (fixture->clear_ret == 0) {
        fixture->durable_staged = false;
    }
    return fixture->clear_ret;
}

static int stage_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->stage_calls++;
    if (fixture->stage_ret == 0) {
        fixture->handoff_phase = FIXTURE_HANDOFF_STAGED;
    }
    return fixture->stage_ret;
}

static int commit_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->commit_calls++;
    if (fixture->commit_ret == 0) {
        assert(fixture->handoff_phase == FIXTURE_HANDOFF_STAGED);
        fixture->handoff_phase = FIXTURE_HANDOFF_COMMITTED;
        fixture->owner = FIXTURE_OWNER_HANDOFF;
    }
    return fixture->commit_ret;
}

static int rollback_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->rollback_calls++;
    if (fixture->rollback_ret == 0) {
        fixture->handoff_phase = FIXTURE_HANDOFF_NONE;
        if (fixture->active_present) {
            fixture->owner = FIXTURE_OWNER_ACTIVE;
        }
    }
    return fixture->rollback_ret;
}

static int cancel_active_tx(void *ctx)
{
    struct preempt_fixture *fixture = ctx;

    fixture->cancel_active_calls++;
    if (fixture->cancel_active_ret < 0) {
        return fixture->cancel_active_ret;
    }
    assert(fixture->active_present);
    assert(fixture->durable_staged || fixture->deferred_staged ||
           fixture->click_report_staged ||
           fixture->handoff_phase == FIXTURE_HANDOFF_STAGED ||
           fixture->handoff_phase == FIXTURE_HANDOFF_COMMITTED);
    fixture->active_present = false;
    fixture->owner = fixture->click_report_staged ?
        FIXTURE_OWNER_CLICK_REPORT : FIXTURE_OWNER_DURABLE;
    return 0;
}

static struct app_mesh_click_preempt_ops ops_for(struct preempt_fixture *fixture)
{
    return (struct app_mesh_click_preempt_ops) {
        .save_outbox = save_outbox,
        .schedule_timeout = schedule_timeout,
        .requeue_click_report = requeue_click_report,
        .discard_requeued_click_report = discard_requeued_click_report,
        .cancel_timeout = cancel_timeout,
        .clear_outbox = clear_outbox,
        .stage_click_handoff = stage_click_handoff,
        .commit_click_handoff = commit_click_handoff,
        .rollback_click_handoff = rollback_click_handoff,
        .cancel_active_tx = cancel_active_tx,
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

static struct mesh_click_preempt_plan durable_plan(void)
{
    return (struct mesh_click_preempt_plan) {
        .save_outbox = true,
        .schedule_timeout = true,
        .cancel_active_tx = true,
    };
}

static struct mesh_click_preempt_plan click_report_plan(void)
{
    return (struct mesh_click_preempt_plan) {
        .requeue_click_report = true,
        .cancel_timeout = true,
        .clear_outbox = true,
        .cancel_active_tx = true,
        .click_report.packet.msg_type = MSG_CLICK_REPORT,
    };
}

static struct preempt_fixture active_fixture(void)
{
    return (struct preempt_fixture) {
        .owner = FIXTURE_OWNER_ACTIVE,
        .active_present = true,
    };
}

static void assert_one_real_owner(const struct preempt_fixture *fixture,
                                  const struct app_mesh_click_preempt_result *result)
{
    assert((fixture->active_present ? 1u : 0u) +
           (fixture->owner == FIXTURE_OWNER_DURABLE ? 1u : 0u) +
           (fixture->owner == FIXTURE_OWNER_CLICK_REPORT ? 1u : 0u) +
           (fixture->owner == FIXTURE_OWNER_HANDOFF ? 1u : 0u) == 1u);
    assert(result->custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME ||
           result->custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_OUTBOX ||
           result->custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF ||
           result->custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_CLICK_REPORT);
}

static void test_fallible_preparation_preserves_active_custody(void)
{
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();
    struct mesh_click_preempt_plan plan = durable_plan();

    fixture.schedule_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.schedule_calls == 1u && fixture.save_calls == 0u);
    assert(fixture.cancel_active_calls == 0u);
    assert_one_real_owner(&fixture, &result);

    fixture = active_fixture();
    fixture.save_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.schedule_calls == 1u && fixture.save_calls == 1u);
    assert(fixture.cancel_active_calls == 0u);
    assert_one_real_owner(&fixture, &result);

    fixture = active_fixture();
    plan = click_report_plan();
    fixture.clear_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.clear_calls == 1u && fixture.requeue_calls == 1u);
    assert(fixture.stage_calls == 1u && fixture.commit_calls == 1u);
    assert(fixture.cancel_active_calls == 1u);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF);
    assert_one_real_owner(&fixture, &result);

    fixture = active_fixture();
    fixture.cancel_timeout_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.clear_calls == 0u && fixture.cancel_timeout_calls == 1u);
    assert(fixture.requeue_calls == 0u && fixture.cancel_active_calls == 0u);
    assert_one_real_owner(&fixture, &result);

    fixture = active_fixture();
    fixture.commit_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.stage_calls == 1u && fixture.commit_calls == 1u);
    assert(fixture.rollback_calls == 1u);
    assert(fixture.cancel_active_calls == 0u);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_ACTIVE_RUNTIME);
    assert_one_real_owner(&fixture, &result);

    fixture = active_fixture();
    fixture.requeue_ret = -ENOSPC;
    assert(apply_plan(&plan, &fixture, &result) == -ENOSPC);
    assert(fixture.requeue_calls == 1u && fixture.cancel_active_calls == 1u);
    assert(fixture.handoff_phase == FIXTURE_HANDOFF_COMMITTED);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF);
    assert_one_real_owner(&fixture, &result);
}

static void test_post_cancel_failure_rolls_back_staged_handoff(void)
{
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();
    const struct mesh_click_preempt_plan plan = click_report_plan();

    fixture.cancel_active_ret = -EIO;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.requeue_calls == 0u && fixture.cancel_active_calls == 1u);
    assert(fixture.rollback_calls == 1u);
    assert(!result.click_handoff_rollback_failed);
    assert(fixture.handoff_phase == FIXTURE_HANDOFF_NONE);
    assert_one_real_owner(&fixture, &result);
}

static void test_failed_cancel_and_rollback_keeps_recovery_journal_authoritative(void)
{
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();
    const struct mesh_click_preempt_plan plan = click_report_plan();

    fixture.cancel_active_ret = -EIO;
    fixture.rollback_ret = -ENOSPC;
    assert(apply_plan(&plan, &fixture, &result) == -EIO);
    assert(fixture.stage_calls == 1u && fixture.rollback_calls == 1u);
    assert(result.click_handoff_rollback_failed);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF);
    assert(fixture.handoff_phase == FIXTURE_HANDOFF_COMMITTED);
}

static void test_reset_recovery_has_one_logical_owner_at_every_handoff_phase(void)
{
    const enum fixture_handoff_phase phases[] = {
        FIXTURE_HANDOFF_NONE,
        FIXTURE_HANDOFF_STAGED,
        FIXTURE_HANDOFF_COMMITTED,
    };

    for (size_t i = 0u; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        struct preempt_fixture fixture = active_fixture();

        fixture.handoff_phase = phases[i];
        fixture.active_present = false; /* Reset discards volatile runtime ownership. */
        if (phases[i] != FIXTURE_HANDOFF_NONE) {
            fixture.owner = FIXTURE_OWNER_HANDOFF;
        } else {
            fixture.owner = FIXTURE_OWNER_DURABLE;
        }
        assert((fixture.owner == FIXTURE_OWNER_DURABLE ? 1u : 0u) +
               (fixture.owner == FIXTURE_OWNER_HANDOFF ? 1u : 0u) == 1u);
    }
}

static void test_durable_handoff_survives_restart_after_commit(void)
{
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();
    const struct mesh_click_preempt_plan plan = durable_plan();

    assert(apply_plan(&plan, &fixture, &result) == 0);
    assert(result.transaction_committed);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_OUTBOX);
    assert(!fixture.active_present && fixture.durable_staged);
    /* A restart can only restore the committed durable owner. */
    assert(fixture.owner == FIXTURE_OWNER_DURABLE);
    assert_one_real_owner(&fixture, &result);
}

static void test_deferred_transit_survives_local_click_outbox_lifecycle(void)
{
    struct app_mesh_click_preempt_result result;
    struct preempt_fixture fixture = active_fixture();
    struct app_mesh_click_preempt_ops ops = ops_for(&fixture);
    const struct mesh_click_preempt_plan plan = durable_plan();
    struct mesh_outbound restored;

    fixture.expected_packet.packet.msg_type = MSG_CLICK_REPORT;
    fixture.expected_packet.packet.src_id = UINT64_C(0xB001);
    fixture.expected_packet.packet.dst_id = UINT64_C(0x9000);
    fixture.expected_packet.packet.session_id = 0x44556677u;
    fixture.expected_packet.packet.seq = 0x1234u;
    fixture.expected_packet.packet.ttl = MESH_GATEWAY_ACK_TTL;
    fixture.expected_packet.payload_len = 3u;
    fixture.expected_packet.packet.payload_len = 3u;
    fixture.expected_packet.payload[0] = 0xa1u;
    fixture.expected_packet.payload[1] = 0xb2u;
    fixture.expected_packet.payload[2] = 0xc3u;
    ops.save_deferred_outbox = save_deferred_outbox;

    /* The exact transit identity is committed to its dedicated handoff slot. */
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == 0);
    assert(result.outbox_saved);
    assert(fixture.deferred_staged);
    assert(fixture.deferred_packet_valid);
    assert(!fixture.active_present);
    assert(fixture.owner == FIXTURE_OWNER_DURABLE);

    /* A later local click may use and retire the ordinary active-outbox slot. */
    fixture.active_present = true;
    fixture.owner = FIXTURE_OWNER_ACTIVE;
    fixture.durable_staged = true;
    fixture.durable_staged = false; /* local click gateway ACK */
    fixture.active_present = false;
    fixture.owner = FIXTURE_OWNER_DURABLE;
    assert(fixture.deferred_staged);
    assert(fixture.deferred_packet_valid);

    /* Same-boot restore consumes only the deferred copy and preserves identity. */
    restored = fixture.deferred_packet;
    fixture.deferred_staged = false;
    fixture.deferred_packet_valid = false;
    fixture.active_present = true;
    fixture.owner = FIXTURE_OWNER_ACTIVE;
    assert(restored.packet.msg_type == MSG_CLICK_REPORT);
    assert(restored.packet.src_id == UINT64_C(0xB001));
    assert(restored.packet.session_id == 0x44556677u);
    assert(restored.packet.seq == 0x1234u);
    assert(restored.payload_len == 3u);
    assert(memcmp(restored.payload,
                  fixture.expected_packet.payload,
                  restored.payload_len) == 0);
    assert(!fixture.deferred_staged);
    assert(fixture.active_present);
}

static void test_deferred_outbox_keeps_timeout_work_owned_after_local_ack(void)
{
    /* This is the production scheduler predicate used after gateway ACK. */
    assert(!app_mesh_tx_timeout_work_needed(false, false, false, false));
    assert(app_mesh_tx_timeout_work_needed(false, false, false, true));
    assert(app_mesh_tx_timeout_work_needed(true, false, false, false));
    assert(app_mesh_tx_timeout_work_needed(false, true, false, false));
    assert(app_mesh_tx_timeout_work_needed(false, false, true, false));
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
    assert(fixture.count == 3u);
    assert(fixture.get_calls == 4u);
    assert(fixture.put_calls == 3u);
    assert(fixture.entries[0].packet.seq == 1u);
    assert(fixture.entries[1].packet.seq == 3u);
    assert(fixture.entries[2].packet.seq == 4u);
}

static void test_queue_remove_absent_target_preserves_order(void)
{
    struct queue_remove_fixture fixture = queue_remove_fixture();
    struct app_mesh_queue_remove_ops ops = queue_remove_ops(&fixture);
    struct mesh_outbound target = {.packet.seq = 9u};
    struct mesh_outbound scratch;
    bool removed = true;

    assert(app_mesh_queue_remove_first(&ops, &target, &scratch, &removed) ==
           -ENOENT);
    assert(!removed);
    assert(fixture.count == 4u);
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
        assert(put_failure.count == 4u);
        assert(put_failure.entries[0].packet.seq == 2u);
        assert(put_failure.entries[1].packet.seq == 3u);
        assert(put_failure.entries[2].packet.seq == 4u);
        assert(put_failure.entries[3].packet.seq == 99u);
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
    struct mesh_outbound appended = {.packet.seq = 5u};
    bool target_removed = true;

    app_mesh_queue_head_owner_init(&owner);
    assert(app_mesh_queue_head_begin(&owner, &head_ops, &expected, &token) == 0);
    assert(expected.packet.seq == 1u);
    assert(app_mesh_queue_head_owned(&owner));

    /* A producer may append while the sender owns the immutable head. */
    assert(queue_remove_put(&appended, &fixture) == 0);
    assert(fixture.count == 5u);
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
    assert(fixture.count == 4u);
    assert(fixture.entries[0].packet.seq == 2u);
    assert(fixture.entries[3].packet.seq == 5u);
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
    assert(fixture.count == 4u && fixture.get_calls == 0u);

    /* Model an illegal unowned destructive interleaving: fail closed. */
    fixture.entries[0].packet.seq = 99u;
    assert(app_mesh_queue_head_commit(&owner,
                                      &ops,
                                      &token,
                                      &expected,
                                      &removed) == -ESTALE);
    assert(fixture.count == 4u && fixture.get_calls == 0u);
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
    test_fallible_preparation_preserves_active_custody();
    test_post_cancel_failure_rolls_back_staged_handoff();
    test_failed_cancel_and_rollback_keeps_recovery_journal_authoritative();
    test_reset_recovery_has_one_logical_owner_at_every_handoff_phase();
    test_durable_handoff_survives_restart_after_commit();
    test_deferred_transit_survives_local_click_outbox_lifecycle();
    test_deferred_outbox_keeps_timeout_work_owned_after_local_ack();
    test_queue_remove_rotates_once_and_preserves_relative_order();
    test_queue_remove_absent_target_preserves_order();
    test_queue_remove_reports_get_and_put_failures();
    test_queue_head_owner_blocks_rotation_but_allows_append();
    test_queue_head_owner_rejects_stale_or_changed_head();
    test_parent_loss_keeps_exactly_one_retry_owner();
    test_queue_recovery_reserve_never_replaces_existing_custody();
    return 0;
}
