#include "app_mesh_route_reply_ack.h"

#include <assert.h>
#include <errno.h>

static void test_successful_listen_completes_attempt(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 0u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = 0,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS);
    assert(!result.note_retry);
    assert(result.return_ret == 0);
}

static void test_send_failure_retries_until_limit(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 2u,
        .max_retries = 4u,
        .send_ret = -EIO,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY);
    assert(result.note_retry);
    assert(result.return_ret == -EIO);
}

static void test_listen_timeout_retries_until_limit(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 3u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = -ETIMEDOUT,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY);
    assert(result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
}

static void test_final_listen_timeout_fails_without_retry_note(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 4u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = -ETIMEDOUT,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_FAILED);
    assert(!result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
}

static void test_primary_failure_uses_valid_backup_hop(void)
{
    const struct app_mesh_route_reply_ack_backup_state state = {
        .primary_ret = -ETIMEDOUT,
        .backup_valid = true,
        .primary_next_hop_id = 0x1002u,
        .backup_next_hop_id = 0x2003u,
    };
    struct app_mesh_route_reply_ack_backup_result result;

    app_mesh_route_reply_ack_decide_backup(&state, &result);

    assert(result.try_backup);
    assert(result.note_retry);
    assert(result.backup_next_hop_id == 0x2003u);
}

static void test_primary_failure_without_distinct_backup_fails(void)
{
    const struct app_mesh_route_reply_ack_backup_state state = {
        .primary_ret = -ETIMEDOUT,
        .backup_valid = true,
        .primary_next_hop_id = 0x1002u,
        .backup_next_hop_id = 0x1002u,
    };
    struct app_mesh_route_reply_ack_backup_result result;

    app_mesh_route_reply_ack_decide_backup(&state, &result);

    assert(!result.try_backup);
    assert(!result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
    assert(result.clear_reason != 0);
}

static void test_c5_preemption_extends_ack_deadline_by_full_timeout(void)
{
    assert(app_mesh_route_reply_ack_deadline_after_preemption(1000u, 150u) == 1150u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(UINT32_MAX, 1u) == 1u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(10u, 0u) == 11u);
}

int main(void)
{
    test_successful_listen_completes_attempt();
    test_send_failure_retries_until_limit();
    test_listen_timeout_retries_until_limit();
    test_final_listen_timeout_fails_without_retry_note();
    test_primary_failure_uses_valid_backup_hop();
    test_primary_failure_without_distinct_backup_fails();
    test_c5_preemption_extends_ack_deadline_by_full_timeout();
    return 0;
}
