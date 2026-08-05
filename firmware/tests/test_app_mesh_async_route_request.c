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
        "forced-rediscovery", 100u, NULL));
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
        "forced-rediscovery", UINT32_MAX - 20u, NULL));
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
        &request, UINT64_C(0x1111111111111111), "old", 10u, NULL));
    assert(app_mesh_async_route_request_snapshot(&request, &old_attempt));
    assert(app_mesh_async_route_request_submit(
        &request, UINT64_C(0x2222222222222222), "new", 11u, NULL));

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
        &request, UINT64_C(0x123456789abcdef0), reason, 1u, NULL));
    reason[0] = 'X';
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));
    assert(strcmp(attempt.reason, "mutable-reason") == 0);
    assert(!app_mesh_async_route_request_submit(
        &request, UINT64_C(0xfedcba9876543210), oversize, 2u, NULL));
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
        .valid = true,
    };

    app_mesh_async_route_request_init(&request);
    assert(app_mesh_async_route_request_submit(
        &request, target_b, "target-b", 1u, &transfer));
    assert(app_mesh_async_route_request_snapshot(&request, &attempt));

    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, false, target_b, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, true, UINT64_C(0xaaaaaaaaaaaaaaaa),
        transfer.owner_generation, transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, true, target_b, transfer.owner_generation + 1u,
        transfer.packet_seq, transfer.msg_type));
    assert(!app_mesh_async_route_request_transfer_matches(
        &attempt, true, target_b, transfer.owner_generation,
        transfer.packet_seq + 1u, transfer.msg_type));
    assert(request.pending);
    assert(app_mesh_async_route_request_transfer_matches(
        &attempt, true, target_b, transfer.owner_generation,
        transfer.packet_seq, transfer.msg_type));
}

int main(void)
{
    test_deny_once_retains_exact_request_until_success();
    test_pause_resume_reconstructs_remaining_retry();
    test_old_completion_cannot_clear_new_generation();
    test_reason_is_frozen_and_oversize_is_rejected_atomically();
    test_terminal_transfer_requires_exact_waiting_target();
    return 0;
}
