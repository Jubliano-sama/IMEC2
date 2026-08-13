#include "app_mesh_async_route_request.h"

#include <assert.h>
#include <string.h>

static void test_deny_once_retains_exact_request_until_success(void)
{
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt first;
    struct app_mesh_async_route_attempt retry;
    uint32_t delay_ms = 0u;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x1020304050607080),
        "forced-rediscovery", 100u, NULL, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &first));

    assert(app_mesh_async_route_request_defer(
        &request, &first, 100u, 50u));
    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, 125u, &delay_ms));
    assert(delay_ms == 25u);
    assert(app_mesh_async_route_request_snapshot(&request, &retry));
    assert(retry.target_id == first.target_id);
    assert(retry.generation == first.generation);
    assert(strcmp(retry.reason, first.reason) == 0);

    assert(app_mesh_async_route_request_complete(&request, &retry));
    assert(!request.pending);
}

static void test_pause_resume_reconstructs_remaining_retry(void)
{
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt before_pause;
    struct app_mesh_async_route_attempt after_resume;
    uint32_t delay_ms = 0u;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x8877665544332211),
        "forced-rediscovery", UINT32_MAX - 20u, NULL, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &before_pause));
    assert(app_mesh_async_route_request_defer(
        &request, &before_pause, UINT32_MAX - 20u, 40u));

    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, UINT32_MAX - 5u, &delay_ms));
    assert(delay_ms == 25u);
    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, 4u, &delay_ms));
    assert(delay_ms == 15u);
    assert(app_mesh_async_route_request_snapshot(&request, &after_resume));
    assert(after_resume.target_id == before_pause.target_id);
    assert(after_resume.generation == before_pause.generation);
    assert(strcmp(after_resume.reason, before_pause.reason) == 0);
}

static void test_old_completion_cannot_clear_new_generation(void)
{
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt old_attempt;
    struct app_mesh_async_route_attempt new_attempt;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x1111111111111111), "old", 10u, NULL, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &old_attempt));
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x2222222222222222), "new", 11u, NULL, NULL));

    assert(!app_mesh_async_route_request_complete(&request, &old_attempt));
    assert(!app_mesh_async_route_request_defer(
        &request, &old_attempt, 12u, 100u));
    assert(app_mesh_async_route_request_snapshot(&request, &new_attempt));
    assert(new_attempt.target_id == UINT64_C(0x2222222222222222));
    assert(new_attempt.generation != old_attempt.generation);
    assert(strcmp(new_attempt.reason, "new") == 0);
}

static void test_reason_is_frozen_and_oversize_is_rejected_atomically(void)
{
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt attempt;
    char reason[] = "mutable-reason";
    char oversize[APP_MESH_ASYNC_ROUTE_REASON_CAPACITY + 1u];

    memset(oversize, 'x', sizeof(oversize));
    oversize[sizeof(oversize) - 1u] = '\0';
    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x123456789abcdef0), reason, 1u, NULL, NULL));
    reason[0] = 'X';
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));
    assert(strcmp(attempt.reason, "mutable-reason") == 0);
    assert(!app_mesh_async_route_request_submit(
        &request, UINT64_C(0xfedcba9876543210), oversize, 2u, NULL, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));
    assert(attempt.target_id == UINT64_C(0x123456789abcdef0));
    assert(strcmp(attempt.reason, "mutable-reason") == 0);
}

static void test_terminal_transfer_requires_exact_waiting_target(void)
{
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt attempt;
    const uint64_t target_b = UINT64_C(0xbbbbbbbbbbbbbbbb);
    const struct app_mesh_async_route_transfer_identity transfer = {
        .target_id = target_b,
        .owner_generation = UINT32_C(0x11223344),
        .packet_seq = 77u,
        .msg_type = 0x0eu,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
    };

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, target_b, "target-b", 1u, &transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));

    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        false, target_b, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        true, UINT64_C(0xaaaaaaaaaaaaaaaa),
        transfer.owner_generation, transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        true, target_b, transfer.owner_generation + 1u,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        true, target_b, transfer.owner_generation,
        transfer.packet_seq + 1u, transfer.msg_type));
    assert(request.pending);
    assert(app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        true, target_b, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));
}

static void test_core_pending_transfer_becomes_stale_on_terminal_or_replacement(void)
{
    const uint64_t gateway = UINT64_C(0x9999888877776666);
    const struct app_mesh_async_route_transfer_identity transfer = {
        .target_id = gateway,
        .owner_generation = UINT32_C(0x11223344),
        .packet_seq = 2u,
        .msg_type = 0x33u,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
    };
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt attempt;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "pending-tx-timeout", 100u, &transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));
    assert(attempt.transfer.owner_kind ==
           APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING);
    assert(app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));

    /* A terminal ACK removes the singleton core owner, so queued discovery
     * must become stale before it can probe or retain another retry. */
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
        false, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));

    /* A newer exact owner can share the gateway target, but it must not
     * inherit the old timeout repair coroutine. */
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
        true, gateway, transfer.owner_generation + 1u,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq + 1u, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));
}

static void test_node_comm_transfer_requires_exact_live_delivery_owner(void)
{
    const uint64_t gateway = UINT64_C(0x9999888877776666);
    const struct app_mesh_async_route_transfer_identity transfer = {
        .target_id = gateway,
        .owner_generation = UINT32_C(0x55667788),
        .packet_seq = 41u,
        .msg_type = 0x35u,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
    };
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt attempt;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "node-comm-reliable-uplink", 10u,
        &transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));
    assert(app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));

    /* A terminal/no-owner delivery is stale even while its frozen route
     * request is still pending. */
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        false, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));

    /* Reuse of the same gateway by a successor delivery must not inherit
     * the predecessor's asynchronous route request. */
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        true, gateway, transfer.owner_generation + 1u,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq + 1u, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        true, gateway, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type + 1u));
}

static void test_node_comm_terminal_cancels_deferred_request_before_retry(void)
{
    const uint64_t gateway = UINT64_C(0x9999888877776666);
    const struct app_mesh_async_route_transfer_identity old_transfer = {
        .target_id = gateway,
        .owner_generation = UINT32_C(0x10203040),
        .packet_seq = 51u,
        .msg_type = 0x36u,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
    };
    struct app_mesh_async_route_transfer_identity successor_transfer =
        old_transfer;
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt deferred;
    struct app_mesh_async_route_attempt successor;
    uint32_t delay_ms = 0u;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "node-comm-reliable-uplink", 100u,
        &old_transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &deferred));
    assert(app_mesh_async_route_request_defer(
        &request, &deferred, 100u, 4000u));
    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, 101u, &delay_ms));
    assert(delay_ms == 3999u);

    /* Terminal cancellation completes this exact deferred coroutine.  There
     * is then no retry for the worker to turn into another RF probe. */
    assert(app_mesh_async_route_request_transfer_matches(
        &deferred, APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM,
        true, gateway, old_transfer.owner_generation,
        old_transfer.packet_seq, old_transfer.msg_type));
    assert(app_mesh_async_route_request_complete(&request, &deferred));
    assert(!app_mesh_async_route_request_snapshot(&request, &successor));
    assert(!app_mesh_async_route_request_retry_delay_ms(
        &request, 4100u, &delay_ms));
    assert(!app_mesh_async_route_request_defer(
        &request, &deferred, 4100u, 1u));

    successor_transfer.owner_generation++;
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "successor", 4101u,
        &successor_transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &successor));
    assert(!app_mesh_async_route_request_complete(&request, &deferred));
    assert(successor.generation != deferred.generation);
    assert(successor.transfer.owner_generation ==
           successor_transfer.owner_generation);
    assert(request.pending);
}

static void test_transfer_owned_request_coalesces_exactly_and_cannot_be_displaced(void)
{
    const uint64_t gateway = UINT64_C(0x9999888877776666);
    const struct app_mesh_async_route_transfer_identity transfer = {
        .target_id = gateway,
        .owner_generation = UINT32_C(0x44556677),
        .packet_seq = 31u,
        .msg_type = 0x55u,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
    };
    struct app_mesh_async_route_transfer_identity conflicting = transfer;
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt first;
    struct app_mesh_async_route_attempt retained;
    uint32_t delay_ms = 0u;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "pending-tx-timeout", 100u, &transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &first));

    assert(app_mesh_async_route_request_defer(
        &request, &first, 100u, 4000u));
    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, 101u, &delay_ms));
    assert(delay_ms == 3999u);

    /* Repeated timeout edges for the same immutable core owner coalesce;
     * they must not reset generation, reason, or the retained retry due time. */
    assert(app_mesh_async_route_request_submit(
        &request, gateway, "duplicate-timeout", 101u, &transfer, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &retained));
    assert(retained.generation == first.generation);
    assert(strcmp(retained.reason, first.reason) == 0);
    assert(memcmp(&retained.transfer, &transfer, sizeof(transfer)) == 0);
    assert(app_mesh_async_route_request_retry_delay_ms(
        &request, 101u, &delay_ms));
    assert(delay_ms == 3999u);

    /* An unowned node-communication retry for the same gateway cannot steal
     * a route coroutine from the exact retained core packet. */
    assert(!app_mesh_async_route_request_submit(
        &request, gateway, "node-comm-reliable-uplink", 102u, NULL, NULL));

    conflicting.owner_generation++;
    assert(!app_mesh_async_route_request_submit(
        &request, gateway, "different-generation", 103u,
        &conflicting, NULL));
    conflicting = transfer;
    conflicting.packet_seq++;
    assert(!app_mesh_async_route_request_submit(
        &request, gateway, "different-sequence", 104u,
        &conflicting, NULL));
    conflicting = transfer;
    conflicting.owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT;
    assert(!app_mesh_async_route_request_submit(
        &request, gateway, "different-owner-kind", 105u,
        &conflicting, NULL));
    conflicting = transfer;
    conflicting.target_id = UINT64_C(0x1111222233334444);
    assert(!app_mesh_async_route_request_submit(
        &request, conflicting.target_id, "different-target", 106u,
        &conflicting, NULL));

    assert(app_mesh_async_route_request_snapshot(&request, &retained));
    assert(retained.generation == first.generation);
    assert(retained.target_id == gateway);
    assert(strcmp(retained.reason, "pending-tx-timeout") == 0);
    assert(memcmp(&retained.transfer, &transfer, sizeof(transfer)) == 0);
}

static void test_c5_authorization_is_retained_and_cannot_be_displaced(void)
{
    const uint64_t target_b = UINT64_C(0xbbbbbbbbbbbbbbbb);
    struct app_mesh_async_route_request request;
    struct app_mesh_async_route_attempt first;
    struct app_mesh_async_route_attempt retry;
    struct app_mesh_c5_tx_authorization_token authorization = {
        .kind = APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
        .peer_id = target_b,
        .pending_session_id = UINT32_C(0x11223344),
        .pending_seq = UINT16_C(0x5566),
        .pending_msg_type = 0x0eu,
        .valid = true,
    };
    struct app_mesh_c5_tx_authorization_token conflicting = authorization;

    memset(authorization.pending_digest, 0x5au,
           sizeof(authorization.pending_digest));
    conflicting = authorization;
    conflicting.pending_digest[0] ^= 0xffu;

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, target_b, "ack-route-repair", 100u,
        NULL, &authorization));
    assert(app_mesh_async_route_request_snapshot(&request, &first));
    assert(first.c5_authorization.valid);
    assert(memcmp(&first.c5_authorization,
                  &authorization,
                  sizeof(authorization)) == 0);

    assert(app_mesh_async_route_request_defer(
        &request, &first, 100u, 50u));
    assert(app_mesh_async_route_request_snapshot(&request, &retry));
    assert(memcmp(&retry.c5_authorization,
                  &authorization,
                  sizeof(authorization)) == 0);

    /* Exact coalescing is inert, while ordinary or conflicting work cannot
     * replace the critical owner or advance its generation. */
    assert(app_mesh_async_route_request_submit(
        &request, target_b, "duplicate", 120u,
        NULL, &authorization));
    assert(request.generation == first.generation);
    assert(!app_mesh_async_route_request_submit(
        &request, target_b, "ordinary", 121u, NULL, NULL));
    assert(!app_mesh_async_route_request_submit(
        &request, target_b, "conflict", 122u, NULL, &conflicting));
    assert(!app_mesh_async_route_request_submit(
        &request, UINT64_C(0xaaaaaaaaaaaaaaaa), "other", 123u,
        NULL, &authorization));
    assert(app_mesh_async_route_request_snapshot(&request, &retry));
    assert(retry.generation == first.generation);
    assert(strcmp(retry.reason, first.reason) == 0);
    assert(memcmp(&retry.c5_authorization,
                  &authorization,
                  sizeof(authorization)) == 0);

    assert(app_mesh_async_route_request_complete(&request, &retry));
    assert(!request.pending);
    assert(!request.c5_authorization.valid);

    authorization.peer_id = target_b + 1u;
    assert(!app_mesh_async_route_request_submit(
        &request, target_b, "mismatched-token", 200u,
        NULL, &authorization));
    assert(!request.pending);
}

int main(void)
{
    test_deny_once_retains_exact_request_until_success();
    test_pause_resume_reconstructs_remaining_retry();
    test_old_completion_cannot_clear_new_generation();
    test_reason_is_frozen_and_oversize_is_rejected_atomically();
    test_terminal_transfer_requires_exact_waiting_target();
    test_core_pending_transfer_becomes_stale_on_terminal_or_replacement();
    test_node_comm_transfer_requires_exact_live_delivery_owner();
    test_node_comm_terminal_cancels_deferred_request_before_retry();
    test_transfer_owned_request_coalesces_exactly_and_cannot_be_displaced();
    test_c5_authorization_is_retained_and_cannot_be_displaced();
    return 0;
}
