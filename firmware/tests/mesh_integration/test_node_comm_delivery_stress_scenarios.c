#include "node_comm.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>

#define LONG_DEADLINE_MS UINT64_C(1000000000)

static struct node_comm_request request_for(
    enum node_comm_delivery_profile profile,
    uint32_t token,
    uint32_t seed)
{
    return (struct node_comm_request) {
        .profile = profile,
        .absolute_deadline_ms = LONG_DEADLINE_MS,
        .client_token = token,
        .retry_jitter_seed = seed,
    };
}

static uint32_t submit(struct node_comm *comm,
                       const struct node_comm_request *request,
                       uint64_t now_ms)
{
    uint32_t handle = 0u;

    assert(node_comm_submit(comm, request, now_ms, &handle) == 0);
    assert(handle != 0u);
    return handle;
}

static struct node_comm_terminal_event take_terminal(
    struct node_comm *comm,
    uint32_t handle)
{
    struct node_comm_terminal_event event;

    assert(node_comm_take_terminal_event_for(comm, handle, &event));
    return event;
}

static void deliver_response_after_interference(
    struct node_comm *comm,
    uint32_t handle,
    uint8_t pre_rf_blocks,
    uint8_t destructive_rf_failures,
    uint64_t *now_ms)
{
    struct node_comm_lease lease;

    for (uint8_t blocked = 0u; blocked < pre_rf_blocks; blocked++) {
        uint64_t retry_at;

        assert(node_comm_acquire(comm, *now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == 1u);
        retry_at = *now_ms + 37u;
        assert(node_comm_lease_defer_pre_rf(comm, &lease, retry_at,
                                            *now_ms) == 0);
        *now_ms = retry_at;
    }

    for (uint8_t failure = 0u; failure < destructive_rf_failures; failure++) {
        assert(node_comm_acquire(comm, *now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == failure + 1u);
        assert(node_comm_lease_note_rf_started(comm, &lease, *now_ms) == 0);
        assert(node_comm_lease_complete(comm, &lease,
                                        NODE_COMM_DELIVERY_RETRY,
                                        *now_ms) == 0);
        /* Exceeds the maximum jittered delay for every response backoff. */
        *now_ms += UINT64_C(10000);
    }

    assert(node_comm_acquire(comm, *now_ms, &lease) == 0);
    assert(lease.handle == handle);
    assert(lease.attempt_number == destructive_rf_failures + 1u);
    assert(node_comm_lease_note_rf_started(comm, &lease, *now_ms) == 0);
    assert(node_comm_lease_complete(comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED,
                                    *now_ms) == 0);
}

static void test_control_response_burst_preempts_lower_priority_response(void)
{
    struct node_comm comm;
    struct node_comm_lease lease;
    uint32_t response_handles[3];
    uint32_t lower_priority_handle;
    uint64_t now_ms = 0u;

    node_comm_init(&comm);
    assert(node_comm_start(&comm, now_ms) == 0);
    lower_priority_handle = submit(&comm,
        &(struct node_comm_request) {
            .profile = NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
            .absolute_deadline_ms = LONG_DEADLINE_MS,
            .client_token = 1u,
            .retry_jitter_seed = 1u,
        }, now_ms);
    for (size_t i = 0u; i < 3u; i++) {
        struct node_comm_request response = request_for(
            NODE_COMM_PROFILE_CONTROL_RESPONSE,
            (uint32_t)(100u + i),
            (uint32_t)(0x13579bdu * (i + 1u)));

        response_handles[i] = submit(&comm, &response, now_ms);
    }

    for (size_t i = 0u; i < 3u; i++) {
        struct node_comm_terminal_event event;

        deliver_response_after_interference(&comm, response_handles[i],
                                            (uint8_t)(i % 4u),
                                            (uint8_t)(i % 4u),
                                            &now_ms);
        event = take_terminal(&comm, response_handles[i]);
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
        assert(event.attempts_started == (uint8_t)(i % 4u) + 1u);
    }

    assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
    assert(lease.handle == lower_priority_handle);
}

static void test_fifty_response_sweep_eventually_delivers(void)
{
    struct node_comm comm;
    uint64_t now_ms = 0u;

    node_comm_init(&comm);
    assert(node_comm_start(&comm, now_ms) == 0);
    for (uint32_t anchor = 0u; anchor < 50u; anchor++) {
        struct node_comm_request response = request_for(
            NODE_COMM_PROFILE_CONTROL_RESPONSE,
            1000u + anchor,
            UINT32_C(0x9e3779b9) * (anchor + 1u));
        struct node_comm_terminal_event event;
        uint32_t handle = submit(&comm, &response, now_ms);
        uint8_t destructive_failures = (uint8_t)(anchor % 4u);

        deliver_response_after_interference(
            &comm, handle, (uint8_t)((anchor * 3u) % 8u),
            destructive_failures, &now_ms);
        event = take_terminal(&comm, handle);
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
        assert(event.attempts_started == destructive_failures + 1u);
        assert(node_comm_pending_count(&comm) == 0u);
        now_ms++;
    }
}

static void test_persistent_collision_exhausts_then_queue_recovers(void)
{
    struct node_comm comm;
    struct node_comm_request response = request_for(
        NODE_COMM_PROFILE_CONTROL_RESPONSE, 2000u, UINT32_C(0xabcdef01));
    struct node_comm_lease lease;
    struct node_comm_terminal_event event;
    uint64_t now_ms = 0u;
    uint32_t failed_handle;
    uint32_t next_handle;

    node_comm_init(&comm);
    assert(node_comm_start(&comm, now_ms) == 0);
    failed_handle = submit(&comm, &response, now_ms);
    for (uint8_t attempt = 0u; attempt < 4u; attempt++) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == failed_handle);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_RETRY,
                                        now_ms) == 0);
        now_ms += UINT64_C(10000);
    }
    event = take_terminal(&comm, failed_handle);
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 4u);

    response.client_token++;
    next_handle = submit(&comm, &response, now_ms);
    deliver_response_after_interference(&comm, next_handle, 0u, 0u, &now_ms);
    event = take_terminal(&comm, next_handle);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(node_comm_pending_count(&comm) == 0u);
}

static void test_protocol_response_survives_bounded_receiver_blackout(void)
{
    enum {
        BOUNDED_CONTROL_RF_OPPORTUNITIES = 3,
        OLD_PROTOCOL_RESPONSE_ATTEMPT_LIMIT = 4,
        BLACKOUT_FAILED_ATTEMPTS = 8,
    };
    struct node_comm control_comm;
    struct node_comm response_comm;
    struct node_comm_request control = request_for(
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        2100u,
        UINT32_C(0x8cc4a7d1));
    struct node_comm_request response = request_for(
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        2101u,
        UINT32_C(0xa16e503b));
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint64_t now_ms = 0u;
    uint64_t due_ms = 0u;
    uint32_t control_handle;
    uint32_t response_handle;

    /* The gateway control contract remains exactly three real RF copies. */
    node_comm_init(&control_comm);
    assert(node_comm_start(&control_comm, now_ms) == 0);
    control_handle = submit(&control_comm, &control, now_ms);
    for (uint8_t attempt = 1u;
         attempt <= BOUNDED_CONTROL_RF_OPPORTUNITIES;
         attempt++) {
        assert(node_comm_acquire(&control_comm, now_ms, &lease) == 0);
        assert(lease.handle == control_handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(
                   &control_comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(
                   &control_comm, &lease,
                   NODE_COMM_DELIVERY_SUCCEEDED, now_ms) == 0);
        if (attempt < BOUNDED_CONTROL_RF_OPPORTUNITIES) {
            assert(!node_comm_take_terminal_event_for(
                &control_comm, control_handle, &event));
            assert(node_comm_next_service_due_ms(
                &control_comm, now_ms, &due_ms));
            now_ms = due_ms;
        }
    }
    event = take_terminal(&control_comm, control_handle);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == BOUNDED_CONTROL_RF_OPPORTUNITIES);
    assert(node_comm_pending_count(&control_comm) == 0u);

    /*
     * Each failed RF start models a complete response transmission while the
     * receiving gateway is unavailable.  Availability returns only after a
     * blackout twice the old four-attempt horizon, so response custody must
     * remain live and terminal-deliver on the first post-blackout attempt.
     */
    now_ms = 0u;
    node_comm_init(&response_comm);
    assert(node_comm_start(&response_comm, now_ms) == 0);
    response_handle = submit(&response_comm, &response, now_ms);
    for (uint8_t attempt = 1u; attempt <= BLACKOUT_FAILED_ATTEMPTS;
         attempt++) {
        assert(node_comm_acquire(&response_comm, now_ms, &lease) == 0);
        assert(lease.handle == response_handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(
                   &response_comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(
                   &response_comm, &lease,
                   NODE_COMM_DELIVERY_RETRY, now_ms) == 0);
        assert(!node_comm_take_terminal_event_for(
            &response_comm, response_handle, &event));
        assert(node_comm_next_service_due_ms(
            &response_comm, now_ms, &due_ms));
        now_ms = due_ms;

        if (attempt == OLD_PROTOCOL_RESPONSE_ATTEMPT_LIMIT) {
            assert(node_comm_pending_count(&response_comm) == 1u);
        }
    }

    assert(node_comm_acquire(&response_comm, now_ms, &lease) == 0);
    assert(lease.handle == response_handle);
    assert(lease.attempt_number == BLACKOUT_FAILED_ATTEMPTS + 1u);
    assert(node_comm_lease_note_rf_started(
               &response_comm, &lease, now_ms) == 0);
    assert(node_comm_lease_complete(
               &response_comm, &lease,
               NODE_COMM_DELIVERY_SUCCEEDED, now_ms) == 0);
    event = take_terminal(&response_comm, response_handle);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == BLACKOUT_FAILED_ATTEMPTS + 1u);
    assert(node_comm_pending_count(&response_comm) == 0u);
}

static void test_owed_gateway_ack_precedes_later_priority_control(void)
{
    enum {
        PRIORITY_PHASE_COUNT = 3,
        BOUNDED_CONTROL_RF_OPPORTUNITIES = 3,
    };
    struct node_comm comm;
    uint64_t now_ms = 0u;

    node_comm_init(&comm);
    assert(node_comm_start(&comm, now_ms) == 0);

    for (uint32_t phase = 0u; phase < PRIORITY_PHASE_COUNT; phase++) {
        struct node_comm_request owed_ack = request_for(
            NODE_COMM_PROFILE_CONTROL_RESPONSE,
            5000u + phase,
            UINT32_C(0x45d9f3b) * (phase + 1u));
        struct node_comm_request next_control = request_for(
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            6000u + phase,
            UINT32_C(0x27d4eb2d) * (phase + 1u));
        struct node_comm_terminal_event event;
        struct node_comm_lease lease;
        uint32_t ack_handle;
        uint32_t control_handle;

        /*
         * Accepting one priority command result owes its gateway ACK before
         * driving the next phase. The later control must not overtake that
         * already-queued ACK, or the anchor's response single-flight remains
         * occupied and cannot carry the next phase result.
         */
        ack_handle = submit(&comm, &owed_ack, now_ms);
        control_handle = submit(&comm, &next_control, now_ms);

        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == ack_handle);
        assert(node_comm_lease_note_rf_started(
                   &comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(
                   &comm, &lease,
                   NODE_COMM_DELIVERY_SUCCEEDED, now_ms) == 0);
        event = take_terminal(&comm, ack_handle);
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

        for (uint8_t attempt = 1u;
             attempt <= BOUNDED_CONTROL_RF_OPPORTUNITIES;
             attempt++) {
            uint64_t due_ms;

            assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
            assert(lease.handle == control_handle);
            assert(lease.attempt_number == attempt);
            assert(node_comm_lease_note_rf_started(
                       &comm, &lease, now_ms) == 0);
            assert(node_comm_lease_complete(
                       &comm, &lease,
                       NODE_COMM_DELIVERY_SUCCEEDED, now_ms) == 0);
            if (attempt < BOUNDED_CONTROL_RF_OPPORTUNITIES) {
                assert(node_comm_next_service_due_ms(
                    &comm, now_ms, &due_ms));
                now_ms = due_ms;
            }
        }
        event = take_terminal(&comm, control_handle);
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
        assert(event.attempts_started == BOUNDED_CONTROL_RF_OPPORTUNITIES);
        assert(node_comm_pending_count(&comm) == 0u);
        now_ms++;
    }
}

static void test_fifty_anchor_gateway_confirmation_sweep(void)
{
    enum { ANCHOR_COUNT = 50 };
    struct node_comm comms[ANCHOR_COUNT];
    uint32_t handles[ANCHOR_COUNT];
    uint64_t confirmation_ms[ANCHOR_COUNT];

    for (uint32_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        struct node_comm_request uplink = request_for(
            NODE_COMM_PROFILE_RELIABLE_UPLINK,
            3000u + anchor,
            UINT32_C(0x85ebca6b) * (anchor + 1u));
        struct node_comm_lease lease;
        uint8_t hops = anchor < 20u ? 1u :
                       (uint8_t)(2u + ((anchor - 20u) % 5u));
        uint64_t now_ms = anchor % 7u;

        node_comm_init(&comms[anchor]);
        assert(node_comm_start(&comms[anchor], 0u) == 0);
        handles[anchor] = submit(&comms[anchor], &uplink, now_ms);

        for (uint8_t blocked = 0u; blocked < anchor % 5u; blocked++) {
            assert(node_comm_acquire(&comms[anchor], now_ms, &lease) == 0);
            assert(lease.handle == handles[anchor]);
            assert(lease.attempt_number == 1u);
            assert(node_comm_lease_defer_pre_rf(
                &comms[anchor], &lease, now_ms + 11u, now_ms) == 0);
            now_ms += 11u;
        }
        assert(node_comm_acquire(&comms[anchor], now_ms, &lease) == 0);
        assert(node_comm_lease_note_rf_started(
            &comms[anchor], &lease, now_ms) == 0);
        assert(node_comm_lease_await_confirmation(
            &comms[anchor], &lease, now_ms) == 0);
        confirmation_ms[anchor] = now_ms + (uint64_t)hops * 37u;

        /* Hop progress is not final gateway acceptance. */
        assert(node_comm_service(&comms[anchor],
                                 confirmation_ms[anchor] - 1u) == 0u);
        {
            struct node_comm_terminal_event event;

            assert(!node_comm_take_terminal_event_for(
                &comms[anchor], handles[anchor], &event));
        }

        /* A response may use the radio scheduler while the uplink waits. */
        {
            struct node_comm_request response = request_for(
                NODE_COMM_PROFILE_CONTROL_RESPONSE,
                4000u + anchor,
                UINT32_C(0xc2b2ae35) * (anchor + 1u));
            struct node_comm_terminal_event event;
            uint32_t response_handle = submit(
                &comms[anchor], &response, confirmation_ms[anchor] - 1u);

            assert(node_comm_acquire(&comms[anchor],
                                     confirmation_ms[anchor] - 1u,
                                     &lease) == 0);
            assert(lease.handle == response_handle);
            assert(node_comm_lease_note_rf_started(
                &comms[anchor], &lease,
                confirmation_ms[anchor] - 1u) == 0);
            assert(node_comm_lease_complete(
                &comms[anchor], &lease,
                NODE_COMM_DELIVERY_SUCCEEDED,
                confirmation_ms[anchor] - 1u) == 0);
            assert(node_comm_take_terminal_event_for(
                &comms[anchor], response_handle, &event));
        }
    }

    for (uint32_t anchor = 0u; anchor < ANCHOR_COUNT; anchor++) {
        struct node_comm_terminal_event event;

        assert(node_comm_confirm_delivery(&comms[anchor],
                                          handles[anchor] + 1000u,
                                          confirmation_ms[anchor]) ==
               -ENOENT);
        assert(node_comm_confirm_delivery(&comms[anchor],
                                          handles[anchor],
                                          confirmation_ms[anchor]) == 0);
        event = take_terminal(&comms[anchor], handles[anchor]);
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
        assert(event.attempts_started == 1u);
        assert(node_comm_pending_count(&comms[anchor]) == 0u);
    }
}

int main(void)
{
    test_control_response_burst_preempts_lower_priority_response();
    test_fifty_response_sweep_eventually_delivers();
    test_persistent_collision_exhausts_then_queue_recovers();
    test_protocol_response_survives_bounded_receiver_blackout();
    test_owed_gateway_ack_precedes_later_priority_control();
    test_fifty_anchor_gateway_confirmation_sweep();

    return 0;
}
