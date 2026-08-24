#include "node_comm.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define SWEEP_STEPS 20000u
#define SWEEP_MAX_HANDLES (SWEEP_STEPS + NODE_COMM_MAX_REQUESTS + 1u)
#define DELIVERY_TRACE_CAPACITY 64u

struct delivery_trace_entry {
    enum node_comm_delivery_trace_kind kind;
    struct fw_event event;
    struct fw_transition transition;
    struct node_comm_terminal_event terminal;
    bool terminal_present;
};

struct delivery_trace_capture {
    struct delivery_trace_entry entries[DELIVERY_TRACE_CAPACITY];
    size_t count;
};

static void capture_delivery_transition(
    enum node_comm_delivery_trace_kind kind,
    const struct fw_event *event,
    const struct fw_transition *transition,
    const struct node_comm_terminal_event *terminal,
    void *context)
{
    struct delivery_trace_capture *capture = context;
    struct delivery_trace_entry *entry;

    assert(capture != NULL);
    assert(event != NULL);
    assert(transition != NULL);
    assert(capture->count < DELIVERY_TRACE_CAPACITY);
    entry = &capture->entries[capture->count++];
    entry->kind = kind;
    entry->event = *event;
    entry->transition = *transition;
    entry->terminal_present = terminal != NULL;
    if (terminal != NULL) {
        entry->terminal = *terminal;
    }
}

static uint32_t sweep_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static void assert_internal_invariants(const struct node_comm *comm)
{
    size_t leased = 0u;

    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        const struct node_comm_request_slot *slot = &comm->slots[i];
        bool terminal = slot->terminal_pending;
        bool live = !terminal && slot->owner.delivery.identity.active;
        uint32_t handle;

        if (!terminal && !live) {
            continue;
        }
        handle = terminal ? slot->owner.terminal.handle :
                            (uint32_t)slot->owner.delivery.identity.operation_id;
        assert(handle != 0u);
        assert(slot->request.profile >= NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD);
        assert(slot->request.profile < NODE_COMM_PROFILE_COUNT);
        assert(slot->max_attempts > 0u);
        if (live) {
            assert(slot->owner.delivery.owns_custody);
            assert(slot->owner.delivery.identity.generation != 0u);
        }
        if (slot->lease_active) {
            leased++;
            assert(slot->lease_generation != 0u);
            assert(live);
        }
        if (slot->backend_guard_active) {
            assert(slot->lease_active);
            assert(live);
            assert(slot->backend_guard_expires_at_ms != 0u);
        }
        if (slot->waiting_resource) {
            assert(live);
            assert(!slot->lease_active);
            assert(slot->owner.delivery.state == FW_DELIVERY_WAIT_TX);
        }
        if (terminal) {
            assert(slot->owner.terminal.handle == handle);
            assert(slot->owner.terminal.reason <= NODE_COMM_TERMINAL_CANCELLED);
            assert(slot->owner.terminal.proof <=
                   NODE_COMM_TERMINAL_PROOF_SEMANTIC);
            if (slot->owner.terminal.reason ==
                NODE_COMM_TERMINAL_DELIVERED) {
                assert(slot->owner.terminal.proof !=
                       NODE_COMM_TERMINAL_PROOF_NONE);
            } else {
                assert(slot->owner.terminal.proof ==
                       NODE_COMM_TERMINAL_PROOF_NONE);
            }
        }
        for (size_t j = i + 1u; j < NODE_COMM_MAX_REQUESTS; j++) {
            const struct node_comm_request_slot *other = &comm->slots[j];
            bool other_live = !other->terminal_pending &&
                              other->owner.delivery.identity.active;

            if (other->terminal_pending || other_live) {
                uint32_t other_handle = other->terminal_pending ?
                    other->owner.terminal.handle :
                    (uint32_t)other->owner.delivery.identity.operation_id;

                assert(other_handle != handle);
            }
        }
    }
    assert(leased <= 1u);
    assert(leased == (node_comm_lease_active(comm) ? 1u : 0u));
    if (comm->control.pause_lease_active) {
        assert(comm->control.pause_lease.owner != 0u);
        assert(comm->control.pause_lease.generation != 0u);
        assert(comm->control.state == NODE_COMM_QUIESCING ||
               comm->control.state == NODE_COMM_PAUSED ||
               comm->control.state == NODE_COMM_RESUMING);
    } else {
        assert(comm->control.state == NODE_COMM_STOPPED ||
               comm->control.state == NODE_COMM_RUNNING);
    }
}

static struct node_comm_request request_with(uint32_t token,
                                             enum node_comm_delivery_profile profile,
                                             uint64_t deadline_ms)
{
    return (struct node_comm_request) {
        .profile = profile,
        .absolute_deadline_ms = deadline_ms,
        .client_token = token,
    };
}

static void init_running(struct node_comm *comm, uint64_t now_ms)
{
    node_comm_init(comm);
    assert(node_comm_state(comm) == NODE_COMM_STOPPED);
    assert(node_comm_start(comm, now_ms) == 0);
    assert(node_comm_state(comm) == NODE_COMM_RUNNING);
}

static uint32_t submit_request(struct node_comm *comm,
                               const struct node_comm_request *request,
                               uint64_t now_ms)
{
    uint32_t handle = 0u;

    assert(node_comm_submit(comm, request, now_ms, &handle) == 0);
    assert(handle != 0u);
    return handle;
}

static size_t collect_resource_wait_owners(
    const struct node_comm *comm,
    struct node_comm_resource_wait_owner *owners,
    size_t owner_capacity)
{
    struct node_comm_resource_wait_owner owner;
    size_t cursor = 0u;
    size_t owner_count = 0u;

    while (node_comm_resource_wait_owner_next(comm, &cursor, &owner)) {
        if (owners != NULL && owner_count < owner_capacity) {
            owners[owner_count] = owner;
        }
        owner_count++;
    }
    return owner_count;
}

static struct node_comm_terminal_event take_terminal(struct node_comm *comm)
{
    struct node_comm_terminal_event event;

    memset(&event, 0, sizeof(event));
    assert(node_comm_take_terminal_event(comm, &event));
    return event;
}

static void test_lifecycle_requires_every_transition(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease pause_lease = {0};
    struct node_comm_request request = request_with(
        1u, NODE_COMM_PROFILE_BEST_EFFORT, 0u);
    uint32_t handle;

    node_comm_init(&comm);
    assert(node_comm_lifecycle_generation(&comm) == 0u);
    assert(node_comm_request_pause(&comm, 1u, 100u, 0u,
                                   &pause_lease) == -EINVAL);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 0u) == -ESTALE);
    assert(node_comm_begin_resume(&comm, &pause_lease, 0u) == -ESTALE);
    assert(node_comm_note_resumed(&comm, &pause_lease, 0u) == -ESTALE);
    assert(node_comm_submit(&comm, &request, 0u, &handle) == -ESHUTDOWN);

    assert(node_comm_start(&comm, 10u) == 0);
    assert(node_comm_lifecycle_generation(&comm) == 1u);
    assert(node_comm_start(&comm, 10u) == -EINVAL);
    assert(node_comm_request_pause(&comm, 1u, 100u, 10u,
                                   &pause_lease) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_QUIESCING);
    assert(node_comm_lifecycle_generation(&comm) == 2u);
    assert(node_comm_begin_resume(&comm, &pause_lease, 11u) == -EINVAL);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 12u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_PAUSED);
    assert(node_comm_submit(&comm, &request, 12u, &handle) == -ESHUTDOWN);
    assert(node_comm_begin_resume(&comm, &pause_lease, 20u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RESUMING);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 20u) == -EINVAL);
    assert(node_comm_note_resumed(&comm, &pause_lease, 20u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RUNNING);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 30u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_STOPPED);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 31u) ==
           -EALREADY);
    assert(node_comm_start(&comm, 40u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RUNNING);
    assert(node_comm_lifecycle_generation(&comm) == 7u);
}

static void test_quiesce_waits_for_generation_checked_lease(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease pause_lease;
    struct node_comm_request request = request_with(
        2u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease lease;
    uint8_t attempts;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_request_pause(&comm, 2u, 100u, 0u,
                                   &pause_lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 1u) == -EBUSY);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 1u) == 0);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 1u) == 0);
    assert(node_comm_begin_resume(&comm, &pause_lease, 2u) == 0);
    assert(node_comm_note_resumed(&comm, &pause_lease, 2u) == 0);
    assert(node_comm_acquire(&comm, 1501u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1502u, &lease) == 0);
}

static void test_blocked_pre_rf_opportunities_never_consume_attempts(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        3u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease leases[5];
    uint8_t attempts;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    for (uint64_t i = 0u; i < 4u; i++) {
        uint64_t now_ms = i * 10u;

        assert(node_comm_acquire(&comm, now_ms, &leases[i]) == 0);
        assert(leases[i].attempt_number == 1u);
        assert(node_comm_lease_defer_pre_rf(&comm, &leases[i],
                                            now_ms + 10u, now_ms) == 0);
        assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
        assert(attempts == 0u);
        if (i > 0u) {
            assert(leases[i].generation != leases[i - 1u].generation);
            assert(node_comm_lease_note_rf_started(&comm, &leases[i - 1u],
                                                   now_ms) == -ESTALE);
        }
    }
    assert(node_comm_acquire(&comm, 40u, &leases[4]) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &leases[4], 40u) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
}

static void test_pre_rf_retry_uses_randomized_exponential_backoff(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        31u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 10000u);
    struct node_comm_lease lease;
    uint64_t now_ms = 0u;
    uint8_t attempts;
    uint32_t handle;

    request.retry_jitter_seed = UINT32_C(0x51f15e37);
    init_running(&comm, now_ms);
    handle = submit_request(&comm, &request, now_ms);
    for (uint16_t round = 1u; round <= 6u; round++) {
        uint64_t due_ms;
        uint32_t delay_ms;

        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.attempt_number == 1u);
        assert(node_comm_lease_defer_pre_rf_retry(
                   &comm, &lease, now_ms) == 0);
        assert(node_comm_retry_backoff_ms(
                   request.profile, request.retry_jitter_seed,
                   round, &delay_ms) == 0);
        assert(node_comm_next_service_due_ms(&comm, now_ms, &due_ms));
        assert(due_ms == now_ms + delay_ms);
        assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
        assert(attempts == 0u);
        now_ms = due_ms;
    }
    assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
}

static void test_retry_backoff_and_attempt_exhaustion(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        4u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint64_t due_ms = 0u;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10u) == 0);
    assert(node_comm_acquire(&comm, 1509u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1510u, &lease) == 0);
    assert(lease.attempt_number == 2u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1510u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 1520u) == 0);
    assert(node_comm_acquire(&comm, 4519u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 4520u, &lease) == 0);
    assert(lease.attempt_number == 3u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 4520u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 4530u) == 0);
    assert(node_comm_acquire(&comm, 10529u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 10530u, &lease) == 0);
    assert(lease.attempt_number == 4u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 10530u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10540u) == 0);
    event = take_terminal(&comm);
    assert(event.handle == handle);
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 4u);
    assert(!node_comm_take_terminal_event(&comm, &event));
}

static void test_pause_rebases_retry_but_not_absolute_deadline(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease pause_lease;
    struct node_comm_request retrying = request_with(
        5u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_request expiring = request_with(
        6u, NODE_COMM_PROFILE_BEST_EFFORT, 150u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t retry_handle;
    uint32_t expire_handle;

    init_running(&comm, 0u);
    retry_handle = submit_request(&comm, &retrying, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == retry_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10u) == 0);
    expire_handle = submit_request(&comm, &expiring, 20u);
    assert(node_comm_request_pause(&comm, 3u, 1000u, 20u,
                                   &pause_lease) == 0);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 20u) == 0);
    assert(node_comm_service(&comm, 149u) == 0u);
    assert(node_comm_service(&comm, 150u) == 1u);
    event = take_terminal(&comm);
    assert(event.handle == expire_handle);
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);

    assert(node_comm_begin_resume(&comm, &pause_lease, 220u) == 0);
    assert(node_comm_note_resumed(&comm, &pause_lease, 220u) == 0);
    assert(node_comm_acquire(&comm, 1709u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1710u, &lease) == 0);
    assert(lease.handle == retry_handle);
}

static void test_preserving_stop_invalidates_leases_and_rebases_retry(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        7u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease first;
    struct node_comm_lease second;
    uint8_t attempts;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &first) == 0);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 10u) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &first, 10u) == -ESTALE);
    assert(node_comm_start(&comm, 100u) == 0);
    assert(node_comm_acquire(&comm, 100u, &second) == 0);
    assert(second.handle == handle);
    assert(second.generation != first.generation);
    assert(second.attempt_number == 1u);
    assert(node_comm_lease_note_rf_started(&comm, &second, 100u) == 0);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 110u) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
    assert(node_comm_start(&comm, 210u) == 0);
    assert(node_comm_acquire(&comm, 1709u, &first) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1710u, &first) == 0);
    assert(first.attempt_number == 2u);
}

static void test_stop_cancel_emits_one_terminal_per_request(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        8u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_terminal_event events[3];
    uint32_t handles[3];

    init_running(&comm, 0u);
    for (size_t i = 0u; i < 3u; i++) {
        request.client_token = (uint32_t)(8u + i);
        handles[i] = submit_request(&comm, &request, 0u);
    }
    assert(node_comm_stop(&comm, NODE_COMM_STOP_CANCEL_ALL, 1u) == 0);
    for (size_t i = 0u; i < 3u; i++) {
        events[i] = take_terminal(&comm);
        assert(events[i].handle == handles[i]);
        assert(events[i].reason == NODE_COMM_TERMINAL_CANCELLED);
    }
    assert(!node_comm_take_terminal_event(&comm, &events[0]));
}

static void test_priority_then_fifo_selection(void)
{
    struct node_comm comm;
    struct node_comm_request low = request_with(
        11u, NODE_COMM_PROFILE_BEST_EFFORT, 0u);
    struct node_comm_request high_a = request_with(
        12u, NODE_COMM_PROFILE_CONTROL_RESPONSE, 0u);
    struct node_comm_request high_b = request_with(
        13u, NODE_COMM_PROFILE_CONTROL_RESPONSE, 0u);
    struct node_comm_lease lease;
    uint32_t low_handle;
    uint32_t high_a_handle;
    uint32_t high_b_handle;

    init_running(&comm, 0u);
    low_handle = submit_request(&comm, &low, 0u);
    high_a_handle = submit_request(&comm, &high_a, 1u);
    high_b_handle = submit_request(&comm, &high_b, 2u);
    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == high_a_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 2u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 2u) == 0);
    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == high_b_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 2u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 2u) == 0);
    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == low_handle);
}

static void test_same_priority_deferred_head_blocks_younger_ready_record(void)
{
    struct node_comm comm;
    struct node_comm_request older = request_with(
        140u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 1000u);
    struct node_comm_request younger = request_with(
        141u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 1000u);
    struct node_comm_lease lease;
    uint64_t due_ms = 0u;
    uint32_t older_handle;
    uint32_t younger_handle;

    init_running(&comm, 0u);
    older_handle = submit_request(&comm, &older, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == older_handle);
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 250u, 0u) == 0);

    younger_handle = submit_request(&comm, &younger, 1u);
    assert(node_comm_acquire(&comm, 1u, &lease) == -EAGAIN);
    assert(node_comm_next_service_due_ms(&comm, 1u, &due_ms));
    assert(due_ms == 250u);

    assert(node_comm_acquire(&comm, 250u, &lease) == 0);
    assert(lease.handle == older_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 250u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 250u) == 0);

    /* RF has started for the head, so its ACK wait no longer blocks FIFO. */
    assert(node_comm_acquire(&comm, 250u, &lease) == 0);
    assert(lease.handle == younger_handle);
}

static void test_protocol_response_preempts_durable_priority_result(void)
{
    struct node_comm comm;
    struct node_comm_request assignment_response = request_with(
        142u, NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE, 1000u);
    struct node_comm_request priority_result = request_with(
        143u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 1000u);
    struct node_comm_lease lease;
    uint32_t assignment_handle;
    uint32_t priority_handle;

    init_running(&comm, 0u);
    assignment_handle = submit_request(&comm, &assignment_response, 0u);
    priority_handle = submit_request(&comm, &priority_result, 1u);
    assert(node_comm_acquire(&comm, 1u, &lease) == 0);
    assert(lease.handle == assignment_handle);
    assert(lease.handle != priority_handle);
}

static void test_deferred_durable_priority_blocks_lower_but_not_controls(void)
{
    struct node_comm comm;
    struct node_comm_request priority_result = request_with(
        147u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 1000u);
    struct node_comm_request lower_priority = request_with(
        148u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u);
    struct node_comm_request control_response = request_with(
        149u, NODE_COMM_PROFILE_CONTROL_RESPONSE, 1000u);
    struct node_comm_request control_flood = request_with(
        150u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_lease lease;
    uint64_t due_ms = 0u;
    uint32_t priority_handle;
    uint32_t lower_handle;
    uint32_t response_handle;
    uint32_t flood_handle;

    init_running(&comm, 0u);
    priority_handle = submit_request(&comm, &priority_result, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == priority_handle);
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 250u, 0u) == 0);

    lower_handle = submit_request(&comm, &lower_priority, 1u);
    response_handle = submit_request(&comm, &control_response, 1u);
    flood_handle = submit_request(&comm, &control_flood, 1u);

    /* Both priority-255 control profiles remain eligible over the deferred
     * measurement owner and retain FIFO order with each other. */
    assert(node_comm_next_service_due_ms(&comm, 1u, &due_ms));
    assert(due_ms == 1u);
    assert(node_comm_acquire(&comm, 1u, &lease) == 0);
    assert(lease.handle == response_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) == 0);
    assert(node_comm_acquire(&comm, 1u, &lease) == 0);
    assert(lease.handle == flood_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_FAILED, 1u) == 0);

    /* The lower ready record is blocked until the priority retry, so it must
     * not advertise now as its next service time and create a busy loop. */
    assert(node_comm_next_service_due_ms(&comm, 1u, &due_ms));
    assert(due_ms == 250u);
    assert(node_comm_acquire(&comm, 1u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 249u, &lease) == -EAGAIN);
    assert(node_comm_next_service_due_ms(&comm, 249u, &due_ms));
    assert(due_ms == 250u);

    assert(node_comm_acquire(&comm, 250u, &lease) == 0);
    assert(lease.handle == priority_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 250u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 250u) == 0);

    /* Once the durable owner has crossed the pre-RF boundary, the ordinary
     * lower class can use the backend while priority confirmation is pending. */
    assert(node_comm_acquire(&comm, 250u, &lease) == 0);
    assert(lease.handle == lower_handle);
}

static void test_non_durable_deferred_classes_remain_non_hol(void)
{
    static const struct {
        enum node_comm_delivery_profile older_profile;
        enum node_comm_delivery_profile lower_profile;
    } cases[] = {
        {
            NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
            NODE_COMM_PROFILE_RELIABLE_UPLINK,
        },
        {
            NODE_COMM_PROFILE_RELIABLE_UPLINK,
            NODE_COMM_PROFILE_BEST_EFFORT,
        },
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct node_comm comm;
        struct node_comm_request older = request_with(
            (uint32_t)(160u + i * 2u), cases[i].older_profile, 1000u);
        struct node_comm_request lower = request_with(
            (uint32_t)(161u + i * 2u), cases[i].lower_profile, 1000u);
        struct node_comm_lease lease;
        uint64_t due_ms = 0u;
        uint32_t older_handle;
        uint32_t lower_handle;

        init_running(&comm, 0u);
        older_handle = submit_request(&comm, &older, 0u);
        assert(node_comm_acquire(&comm, 0u, &lease) == 0);
        assert(lease.handle == older_handle);
        assert(node_comm_lease_defer_pre_rf(
                   &comm, &lease, 250u, 0u) == 0);

        lower_handle = submit_request(&comm, &lower, 1u);
        assert(node_comm_next_service_due_ms(&comm, 1u, &due_ms));
        assert(due_ms == 1u);
        assert(node_comm_acquire(&comm, 1u, &lease) == 0);
        assert(lease.handle == lower_handle);
    }
}

static void test_cancel_retires_exact_wait_retry_owner_without_rf(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        146u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == handle);
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 250u, 0u) == 0);

    assert(node_comm_cancel(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.handle == handle);
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(event.attempts_started == 0u);
    assert(node_comm_pending_count(&comm) == 0u);
}

static void test_bounded_control_pre_rf_deferral_retains_equal_priority_fifo(void)
{
    struct node_comm comm;
    struct node_comm_request flood = request_with(
        144u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_request response = request_with(
        145u, NODE_COMM_PROFILE_CONTROL_RESPONSE, 1000u);
    struct node_comm_lease lease;
    uint32_t flood_handle;

    init_running(&comm, 0u);
    flood_handle = submit_request(&comm, &flood, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == flood_handle);
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 40u, 0u) == 0);

    (void)submit_request(&comm, &response, 1u);
    assert(node_comm_acquire(&comm, 1u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 40u, &lease) == 0);
    assert(lease.handle == flood_handle);
    assert(lease.attempt_number == 1u);
}

static void test_terminal_is_exactly_once_and_late_lease_is_stale(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        14u, NODE_COMM_PROFILE_BEST_EFFORT, 0u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) ==
           -EPROTO);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) == -ESTALE);
    assert(node_comm_cancel(&comm, handle, 1u) == -EALREADY);
    event = take_terminal(&comm);
    assert(event.handle == handle);
    assert(event.client_token == 14u);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(!node_comm_take_terminal_event(&comm, &event));
    assert(node_comm_cancel(&comm, handle, 2u) == -ENOENT);
}

static void test_targeted_terminal_poll_leaves_other_clients_queued(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        140u, NODE_COMM_PROFILE_BEST_EFFORT, 0u);
    struct node_comm_terminal_event event;
    uint32_t first;
    uint32_t second;

    init_running(&comm, 0u);
    first = submit_request(&comm, &request, 0u);
    request.client_token = 141u;
    second = submit_request(&comm, &request, 0u);
    assert(node_comm_cancel(&comm, first, 1u) == 0);
    assert(node_comm_cancel(&comm, second, 1u) == 0);

    assert(!node_comm_take_terminal_event_for(&comm, 0u, &event));
    assert(!node_comm_take_terminal_event_for(&comm, UINT32_MAX, &event));
    assert(node_comm_take_terminal_event_for(&comm, second, &event));
    assert(event.handle == second);
    assert(event.client_token == 141u);
    assert(node_comm_pending_count(&comm) == 1u);
    assert(node_comm_take_terminal_event_for(&comm, first, &event));
    assert(event.handle == first);
    assert(event.client_token == 140u);
    assert(!node_comm_take_terminal_event_for(&comm, first, &event));
    assert(node_comm_pending_count(&comm) == 0u);
}

static void test_next_service_due_tracks_ready_retry_and_deadline(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        142u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u);
    struct node_comm_lease lease;
    struct node_comm_pause_lease pause;
    uint64_t due_ms = UINT64_MAX;

    init_running(&comm, 100u);
    (void)submit_request(&comm, &request, 100u);
    assert(node_comm_next_service_due_ms(&comm, 100u, &due_ms));
    assert(due_ms == 100u);
    assert(node_comm_acquire(&comm, 100u, &lease) == 0);
    assert(node_comm_next_service_due_ms(&comm, 100u, &due_ms));
    assert(due_ms == 1000u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 100u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 100u) == 0);
    assert(node_comm_next_service_due_ms(&comm, 100u, &due_ms));
    assert(due_ms == 1000u);

    assert(node_comm_request_pause(&comm, 142u, 100u, 101u, &pause) == 0);
    assert(!node_comm_next_service_due_ms(&comm, 101u, &due_ms));
}

static void test_inflight_attempt_can_finish_while_quiescing(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        143u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_lease delivery;
    struct node_comm_pause_lease pause;

    init_running(&comm, 0u);
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &delivery) == 0);
    assert(node_comm_request_pause(&comm, 143u, 100u, 0u, &pause) == 0);
    assert(node_comm_note_quiesced(&comm, &pause, 1u) == -EBUSY);
    assert(node_comm_lease_note_rf_started(&comm, &delivery, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &delivery,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) == 0);
    assert(node_comm_note_quiesced(&comm, &pause, 1u) == 0);
}

static void test_bounded_control_flood_runs_three_successful_rf_opportunities(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        144u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;
    uint64_t now_ms = 0u;
    uint64_t due_ms = 0u;

    init_running(&comm, now_ms);
    handle = submit_request(&comm, &request, now_ms);
    for (uint8_t attempt = 1u; attempt <= 3u; attempt++) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED,
                                        now_ms) == 0);
        if (attempt < 3u) {
            assert(!node_comm_take_terminal_event_for(&comm, handle, &event));
            assert(node_comm_next_service_due_ms(&comm, now_ms, &due_ms));
            assert(due_ms == now_ms + 40u);
            assert(node_comm_acquire(&comm, due_ms - 1u, &lease) == -EAGAIN);
            now_ms = due_ms;
        }
    }
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 3u);
    assert(node_comm_acquire(&comm, now_ms, &lease) == -EAGAIN);
}

static void test_bounded_control_flood_does_not_count_failed_copy_as_success(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        145u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;
    uint64_t now_ms = 0u;
    uint64_t due_ms = 0u;

    init_running(&comm, now_ms);
    handle = submit_request(&comm, &request, now_ms);
    for (uint8_t attempt = 1u; attempt <= 3u; attempt++) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(
                   &comm, &lease,
                   attempt == 2u ? NODE_COMM_DELIVERY_RETRY :
                                   NODE_COMM_DELIVERY_SUCCEEDED,
                   now_ms) == 0);
        if (attempt < 3u) {
            assert(!node_comm_take_terminal_event_for(&comm, handle, &event));
            assert(node_comm_next_service_due_ms(&comm, now_ms, &due_ms));
            now_ms = due_ms;
        }
    }
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 3u);
    assert(node_comm_acquire(&comm, now_ms, &lease) == -EAGAIN);
}

static void test_single_control_origin_runs_exactly_one_rf_opportunity(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        146u, NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == handle);
    assert(lease.attempt_number == 1u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 0u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
    assert(node_comm_acquire(&comm, 1u, &lease) == -EAGAIN);
}

static void test_delivered_control_redrive_retains_identity_and_starts_fresh_wave(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        145u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 500u);
    struct node_comm_terminal_event event;
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_lease lease;
    uint32_t handle;
    uint64_t now_ms = 0u;
    uint64_t due_ms = 0u;

    init_running(&comm, now_ms);
    handle = submit_request(&comm, &request, now_ms);
    for (uint8_t attempt = 1u; attempt <= 3u; attempt++) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED,
                                        now_ms) == 0);
        if (attempt < 3u) {
            now_ms += 40u;
        }
    }
    assert(node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.client_token == request.client_token);
    assert(event.attempts_started == 3u);
    assert(event.terminal_at_ms == now_ms);

    memset(&prior_terminal, 0, sizeof(prior_terminal));
    assert(node_comm_redrive_delivered(
               &comm, handle, now_ms - 1u, 1000u, now_ms,
               &prior_terminal) == -EINVAL);
    assert(node_comm_redrive_delivered(
               &comm, handle, 300u, 300u, now_ms,
               &prior_terminal) == -EINVAL);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &event));

    assert(node_comm_redrive_delivered(
               &comm, handle, 300u, 1000u, now_ms,
               &prior_terminal) == 0);
    assert(prior_terminal.handle == handle);
    assert(prior_terminal.client_token == request.client_token);
    assert(prior_terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(prior_terminal.attempts_started == 3u);
    assert(!node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(node_comm_pending_count(&comm) == 1u);
    assert(node_comm_next_service_due_ms(&comm, now_ms, &due_ms));
    assert(due_ms == 300u);
    assert(node_comm_acquire(&comm, due_ms - 1u, &lease) == -EAGAIN);

    now_ms = due_ms;
    for (uint8_t attempt = 1u; attempt <= 3u; attempt++) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.handle == handle);
        assert(lease.attempt_number == attempt);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED,
                                        now_ms) == 0);
        if (attempt < 3u) {
            now_ms += 40u;
        }
    }
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.handle == handle);
    assert(event.client_token == request.client_token);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 3u);
    assert(event.terminal_at_ms == now_ms);
}

static void test_delivered_control_redrive_rejects_other_profiles(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        146u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 500u);
    struct node_comm_terminal_event event;
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 0u) == 0);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    memset(&prior_terminal, 0, sizeof(prior_terminal));
    assert(node_comm_redrive_delivered(
               &comm, handle, 100u, 500u, 0u,
               &prior_terminal) == -EINVAL);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(event.client_token == request.client_token);
}

static uint32_t collision_sweep_seed(uint32_t scenario, uint32_t node)
{
    uint32_t seed = (scenario + 1u) * UINT32_C(0x9e3779b9) ^
                    (node + 1u) * UINT32_C(0x85ebca6b);

    seed ^= seed >> 16;
    seed *= UINT32_C(0x7feb352d);
    seed ^= seed >> 15;
    return seed == 0u ? node + 1u : seed;
}

static void test_twenty_independent_nodes_diversify_destructive_collisions(void)
{
    enum {
        NODE_COUNT = 20,
        SCENARIO_COUNT = 256,
    };
    struct node_comm comms[NODE_COUNT];
    uint32_t handles[NODE_COUNT];
    bool terminal[NODE_COUNT];

    for (uint32_t scenario = 0u; scenario < SCENARIO_COUNT; scenario++) {
        size_t terminal_count = 0u;
        size_t delivered_count = 0u;

        memset(terminal, 0, sizeof(terminal));
        for (uint32_t node = 0u; node < NODE_COUNT; node++) {
            struct node_comm_request request = request_with(
                node + 1u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 60000u);
            struct node_comm_lease lease;

            request.retry_jitter_seed = collision_sweep_seed(scenario, node);
            init_running(&comms[node], 0u);
            handles[node] = submit_request(&comms[node], &request, 0u);
            assert(node_comm_acquire(&comms[node], 0u, &lease) == 0);
            assert(node_comm_lease_note_rf_started(&comms[node],
                                                   &lease, 0u) == 0);
            assert(node_comm_lease_complete(&comms[node], &lease,
                                            NODE_COMM_DELIVERY_RETRY,
                                            0u) == 0);
        }

        while (terminal_count < NODE_COUNT) {
            uint64_t next_due_ms = UINT64_MAX;
            size_t contenders = 0u;

            for (uint32_t node = 0u; node < NODE_COUNT; node++) {
                uint64_t due_ms;

                if (!terminal[node] &&
                    node_comm_next_service_due_ms(&comms[node], 0u,
                                                  &due_ms) &&
                    due_ms < next_due_ms) {
                    next_due_ms = due_ms;
                }
            }
            assert(next_due_ms != UINT64_MAX);
            for (uint32_t node = 0u; node < NODE_COUNT; node++) {
                uint64_t due_ms;

                if (!terminal[node] &&
                    node_comm_next_service_due_ms(&comms[node], 0u,
                                                  &due_ms) &&
                    due_ms == next_due_ms) {
                    contenders++;
                }
            }
            assert(contenders > 0u);

            for (uint32_t node = 0u; node < NODE_COUNT; node++) {
                struct node_comm_terminal_event event;
                struct node_comm_lease lease;
                uint64_t due_ms;

                if (terminal[node] ||
                    !node_comm_next_service_due_ms(&comms[node], 0u,
                                                   &due_ms) ||
                    due_ms != next_due_ms) {
                    continue;
                }
                assert(node_comm_acquire(&comms[node], next_due_ms,
                                         &lease) == 0);
                assert(node_comm_lease_note_rf_started(&comms[node],
                                                       &lease,
                                                       next_due_ms) == 0);
                assert(node_comm_lease_complete(
                    &comms[node],
                    &lease,
                    contenders == 1u ? NODE_COMM_DELIVERY_SUCCEEDED :
                                       NODE_COMM_DELIVERY_RETRY,
                    next_due_ms) == 0);
                if (node_comm_take_terminal_event_for(&comms[node],
                                                      handles[node],
                                                      &event)) {
                    terminal[node] = true;
                    terminal_count++;
                    if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
                        delivered_count++;
                    }
                }
            }
        }
        assert(delivered_count == NODE_COUNT);
    }
}

static void test_deadline_invalidates_active_lease(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        15u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 50u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;

    init_running(&comm, 0u);
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 10u) == 0);
    assert(node_comm_service(&comm, 50u) == 1u);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 50u) == -ESTALE);
    event = take_terminal(&comm);
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 50u);
    assert(event.attempts_started == 1u);
}

static void test_backend_guard_publishes_causal_completion_at_deadline_edges(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        151u, NODE_COMM_PROFILE_BEST_EFFORT, 50u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint64_t due_ms = 0u;

    init_running(&comm, 0u);
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(node_comm_lease_backend_guard_begin(
               &comm, &lease, 100u, 10u) == 0);
    assert(node_comm_service(&comm, 50u) == 0u);
    assert(node_comm_next_service_due_ms(&comm, 50u, &due_ms));
    assert(due_ms == 100u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 49u) == 0);
    assert(node_comm_lease_complete(
               &comm, &lease, NODE_COMM_DELIVERY_SUCCEEDED, 49u) == 0);
    event = take_terminal(&comm);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.terminal_at_ms == 49u);

    request.client_token++;
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(node_comm_lease_backend_guard_begin(
               &comm, &lease, 100u, 10u) == 0);
    assert(node_comm_service(&comm, 50u) == 0u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 50u) == -ESTALE);
    event = take_terminal(&comm);
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 50u);

    request.client_token++;
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(node_comm_lease_backend_guard_begin(
               &comm, &lease, 100u, 10u) == 0);
    assert(node_comm_service(&comm, 50u) == 0u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 51u) == -ESTALE);
    event = take_terminal(&comm);
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 51u);
}

static void test_backend_guard_is_bounded_and_request_local(void)
{
    struct node_comm comm;
    struct node_comm_request guarded = request_with(
        154u, NODE_COMM_PROFILE_BEST_EFFORT, 50u);
    struct node_comm_request unrelated = request_with(
        155u, NODE_COMM_PROFILE_BEST_EFFORT, 60u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint64_t due_ms = 0u;
    uint32_t guarded_handle;
    uint32_t unrelated_handle;

    init_running(&comm, 0u);
    guarded_handle = submit_request(&comm, &guarded, 0u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(lease.handle == guarded_handle);
    assert(node_comm_lease_backend_guard_begin(
               &comm, &lease, 100u, 10u) == 0);
    unrelated_handle = submit_request(&comm, &unrelated, 10u);

    assert(node_comm_next_service_due_ms(&comm, 50u, &due_ms));
    assert(due_ms == 60u);
    assert(node_comm_service(&comm, 60u) == 1u);
    assert(node_comm_take_terminal_event_for(
               &comm, unrelated_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 60u);
    assert(!node_comm_take_terminal_event_for(
               &comm, guarded_handle, &event));
    assert(node_comm_next_service_due_ms(&comm, 60u, &due_ms));
    assert(due_ms == 100u);

    assert(node_comm_service(&comm, 99u) == 0u);
    assert(node_comm_service(&comm, 100u) == 1u);
    assert(node_comm_take_terminal_event_for(
               &comm, guarded_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 100u);
    assert(node_comm_lease_complete(
               &comm, &lease, NODE_COMM_DELIVERY_SUCCEEDED, 49u) == -ESTALE);
}

static void test_capacity_includes_unconsumed_terminal_events(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        16u, NODE_COMM_PROFILE_BEST_EFFORT, 0u);
    struct node_comm_terminal_event event;
    uint32_t handles[NODE_COMM_MAX_REQUESTS];
    uint32_t extra;

    init_running(&comm, 0u);
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        handles[i] = submit_request(&comm, &request, 0u);
    }
    assert(node_comm_pending_count(&comm) == NODE_COMM_MAX_REQUESTS);
    assert(node_comm_submit(&comm, &request, 0u, &extra) == -ENOSPC);
    assert(node_comm_cancel(&comm, handles[0], 0u) == 0);
    assert(node_comm_submit(&comm, &request, 0u, &extra) == -ENOSPC);
    event = take_terminal(&comm);
    assert(event.handle == handles[0]);
    assert(node_comm_submit(&comm, &request, 0u, &extra) == 0);
}

static void test_invalid_requests_and_retry_without_rf_fail_closed(void)
{
    struct node_comm comm;
    struct node_comm_request invalid = request_with(
        17u, NODE_COMM_PROFILE_COUNT, 0u);
    struct node_comm_request expired = request_with(
        18u, NODE_COMM_PROFILE_BEST_EFFORT, 10u);
    struct node_comm_request valid = request_with(
        19u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 10u);
    assert(node_comm_submit(&comm, &invalid, 10u, &handle) == -EINVAL);
    assert(node_comm_submit(&comm, &expired, 10u, &handle) == -ETIMEDOUT);
    (void)submit_request(&comm, &valid, 10u);
    assert(node_comm_acquire(&comm, 10u, &lease) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10u) == -EPROTO);
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 10u, 10u) == 0);
}

static void test_pause_lease_owner_generation_and_expiry(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease first;
    struct node_comm_pause_lease second;
    struct node_comm_pause_lease wrong;
    struct node_comm_pause_lease recovery;

    init_running(&comm, 0u);
    assert(node_comm_request_pause(&comm, 0u, 100u, 0u, &first) == -EINVAL);
    assert(node_comm_request_pause(&comm, 21u, 0u, 0u, &first) == -EINVAL);
    assert(node_comm_request_pause(&comm, 21u, 100u, 0u, &first) == 0);
    wrong = first;
    wrong.owner++;
    assert(node_comm_note_quiesced(&comm, &wrong, 1u) == -ESTALE);
    wrong = first;
    wrong.generation++;
    assert(node_comm_note_quiesced(&comm, &wrong, 1u) == -ESTALE);
    assert(node_comm_state(&comm) == NODE_COMM_QUIESCING);
    assert(node_comm_note_quiesced(&comm, &first, 10u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_PAUSED);
    assert(node_comm_begin_resume(&comm, &wrong, 20u) == -ESTALE);
    assert(node_comm_state(&comm) == NODE_COMM_PAUSED);

    assert(node_comm_service(&comm, 99u) == 0u);
    assert(node_comm_state(&comm) == NODE_COMM_PAUSED);
    assert(node_comm_service(&comm, 100u) == 0u);
    assert(node_comm_state(&comm) == NODE_COMM_RESUMING);
    assert(node_comm_note_resumed(&comm, &first, 100u) == -ESTALE);
    assert(node_comm_pause_recovery_lease(&comm, &recovery));
    assert(recovery.generation != first.generation);
    assert(node_comm_note_resumed(&comm, &recovery, 100u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RUNNING);

    assert(node_comm_request_pause(&comm, 21u, 100u, 110u, &second) == 0);
    assert(second.generation != first.generation);
    assert(node_comm_note_quiesced(&comm, &first, 111u) == -ESTALE);
    assert(node_comm_note_quiesced(&comm, &second, 111u) == 0);
    assert(node_comm_begin_resume(&comm, &second, 112u) == 0);
    assert(node_comm_note_resumed(&comm, &first, 112u) == -ESTALE);
    assert(node_comm_note_resumed(&comm, &second, 112u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RUNNING);
}

static void test_pause_expiry_while_quiescing_requires_forced_reclaim(void)
{
    struct node_comm comm;
    struct node_comm_request request = request_with(
        40u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease delivery_lease;
    struct node_comm_pause_lease caller_lease;
    struct node_comm_pause_lease recovery_lease;

    init_running(&comm, 0u);
    (void)submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &delivery_lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &delivery_lease, 0u) == 0);
    assert(node_comm_request_pause(&comm, 41u, 10u, 0u,
                                   &caller_lease) == 0);
    assert(node_comm_service(&comm, 10u) == 0u);
    assert(node_comm_state(&comm) == NODE_COMM_RESUMING);
    assert(node_comm_note_quiesced(&comm, &caller_lease, 10u) == -ESTALE);
    assert(node_comm_pause_recovery_lease(&comm, &recovery_lease));
    assert(node_comm_note_resumed(&comm, &recovery_lease, 10u) == -EBUSY);
    assert(node_comm_lease_complete(&comm, &delivery_lease,
                                    NODE_COMM_DELIVERY_FAILED, 10u) == 0);
    assert(node_comm_note_resumed(&comm, &recovery_lease, 10u) == 0);
    assert(node_comm_state(&comm) == NODE_COMM_RUNNING);
}

static void test_pause_then_preserving_stop_rebases_only_retry_timer(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease pause_lease;
    struct node_comm_request retrying = request_with(
        20u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_request expiring = request_with(
        21u, NODE_COMM_PROFILE_BEST_EFFORT, 300u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t retry_handle;
    uint32_t expire_handle;

    init_running(&comm, 0u);
    retry_handle = submit_request(&comm, &retrying, 0u);
    expire_handle = submit_request(&comm, &expiring, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == retry_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10u) == 0);
    assert(node_comm_request_pause(&comm, 22u, 1000u, 20u,
                                   &pause_lease) == 0);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 20u) == 0);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 220u) == 0);
    assert(node_comm_start(&comm, 420u) == 0);
    event = take_terminal(&comm);
    assert(event.handle == expire_handle);
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(node_comm_acquire(&comm, 1909u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1910u, &lease) == 0);
    assert(lease.handle == retry_handle);
}

static void test_stop_during_resuming_does_not_rebase_retry_twice(void)
{
    struct node_comm comm;
    struct node_comm_pause_lease pause_lease;
    struct node_comm_request retrying = request_with(
        23u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 0u);
    struct node_comm_lease lease;

    init_running(&comm, 0u);
    (void)submit_request(&comm, &retrying, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_RETRY, 10u) == 0);
    assert(node_comm_request_pause(&comm, 24u, 1000u, 20u,
                                   &pause_lease) == 0);
    assert(node_comm_note_quiesced(&comm, &pause_lease, 20u) == 0);
    assert(node_comm_begin_resume(&comm, &pause_lease, 220u) == 0);
    assert(node_comm_stop(&comm, NODE_COMM_STOP_PRESERVE_QUEUED, 230u) == 0);
    assert(node_comm_start(&comm, 430u) == 0);
    assert(node_comm_acquire(&comm, 1909u, &lease) == -EAGAIN);
    assert(node_comm_acquire(&comm, 1910u, &lease) == 0);
}

static void test_all_delivery_profiles_use_priority_then_fifo_order(void)
{
    struct node_comm comm;
    const enum node_comm_delivery_profile profiles[] = {
        NODE_COMM_PROFILE_BEST_EFFORT,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        NODE_COMM_PROFILE_CONTROL_RESPONSE,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN,
    };
    const enum node_comm_delivery_profile expected[] = {
        NODE_COMM_PROFILE_CONTROL_RESPONSE,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN,
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_BEST_EFFORT,
    };
    struct node_comm_lease lease;
    uint32_t handles[NODE_COMM_PROFILE_COUNT];

    init_running(&comm, 0u);
    for (size_t i = 0u; i < NODE_COMM_PROFILE_COUNT; i++) {
        struct node_comm_request request = request_with(
            (uint32_t)(30u + i), profiles[i], 0u);

        handles[profiles[i]] = submit_request(&comm, &request, 0u);
    }
    for (size_t i = 0u; i < NODE_COMM_PROFILE_COUNT; i++) {
        assert(node_comm_acquire(&comm, 0u, &lease) == 0);
        assert(lease.handle == handles[expected[i]]);
        assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED, 0u) == 0);
    }
}

static void test_deterministic_state_and_request_sweep(void)
{
    static bool terminal_seen[SWEEP_MAX_HANDLES];
    struct node_comm comm;
    struct node_comm_lease delivery_lease = {0};
    struct node_comm_pause_lease pause_lease = {0};
    uint32_t random_state = UINT32_C(0x9e3779b9);
    uint64_t now_ms = 0u;
    size_t accepted_count = 0u;
    size_t terminal_count = 0u;

    memset(terminal_seen, 0, sizeof(terminal_seen));
    init_running(&comm, now_ms);
    for (size_t step = 0u; step < SWEEP_STEPS; step++) {
        uint32_t random_value = sweep_random(&random_state);

        now_ms += random_value % 7u;
        (void)node_comm_service(&comm, now_ms);
        if (!node_comm_lease_active(&comm)) {
            memset(&delivery_lease, 0, sizeof(delivery_lease));
        }
        if (!comm.control.pause_lease_active) {
            memset(&pause_lease, 0, sizeof(pause_lease));
        } else if (comm.control.pause_recovery_required) {
            assert(node_comm_pause_recovery_lease(&comm, &pause_lease));
        }

        switch (comm.control.state) {
        case NODE_COMM_STOPPED:
            assert(node_comm_start(&comm, now_ms) == 0);
            break;
        case NODE_COMM_RUNNING:
            switch ((random_value >> 4) % 7u) {
            case 0u: {
                struct node_comm_request request = request_with(
                    (uint32_t)step,
                    (enum node_comm_delivery_profile)
                        ((random_value >> 12) % NODE_COMM_PROFILE_COUNT),
                    (random_value & 1u) != 0u ?
                        now_ms + 1u + ((random_value >> 20) % 500u) : 0u);
                uint32_t handle;
                int ret = node_comm_submit(&comm, &request, now_ms, &handle);

                assert(ret == 0 || ret == -ENOSPC);
                if (ret == 0) {
                    accepted_count++;
                    assert(handle < SWEEP_MAX_HANDLES);
                }
                break;
            }
            case 1u:
                if (!node_comm_lease_active(&comm)) {
                    int ret = node_comm_acquire(&comm, now_ms, &delivery_lease);
                    assert(ret == 0 || ret == -EAGAIN);
                }
                break;
            case 2u:
                if (node_comm_lease_active(&comm)) {
                    if ((random_value & 1u) != 0u) {
                        int ret = node_comm_lease_note_rf_started(
                            &comm, &delivery_lease, now_ms);
                        assert(ret == 0 || ret == -EALREADY);
                    } else {
                        int ret = node_comm_lease_defer_pre_rf(
                            &comm, &delivery_lease,
                            now_ms + (random_value % 20u), now_ms);
                        assert(ret == 0 || ret == -EALREADY);
                        if (ret == -EALREADY) {
                            assert(node_comm_lease_complete(
                                       &comm, &delivery_lease,
                                       NODE_COMM_DELIVERY_FAILED, now_ms) == 0);
                        }
                    }
                }
                break;
            case 3u:
                if (node_comm_lease_active(&comm)) {
                    if ((random_value & 1u) != 0u) {
                        int rf_ret = node_comm_lease_note_rf_started(
                            &comm, &delivery_lease, now_ms);
                        assert(rf_ret == 0 || rf_ret == -EALREADY);
                    }
                    {
                        int ret = node_comm_lease_complete(
                            &comm, &delivery_lease,
                            (enum node_comm_delivery_outcome)
                                ((random_value >> 8) % 3u),
                            now_ms);
                        assert(ret == 0 || ret == -EPROTO);
                        if (ret == -EPROTO) {
                            assert(node_comm_lease_defer_pre_rf(
                                       &comm, &delivery_lease, now_ms, now_ms) == 0);
                        }
                    }
                }
                break;
            case 4u:
                if (!comm.control.pause_lease_active) {
                    assert(node_comm_request_pause(
                               &comm, 100u + (uint32_t)step,
                               10u + (random_value % 200u), now_ms,
                               &pause_lease) == 0);
                }
                break;
            case 5u:
                assert(node_comm_stop(
                           &comm,
                           (random_value & 1u) != 0u ?
                               NODE_COMM_STOP_PRESERVE_QUEUED :
                               NODE_COMM_STOP_CANCEL_ALL,
                           now_ms) == 0);
                break;
            default:
                break;
            }
            break;
        case NODE_COMM_QUIESCING:
            if (node_comm_lease_active(&comm)) {
                int ret = node_comm_lease_defer_pre_rf(
                    &comm, &delivery_lease, now_ms, now_ms);
                assert(ret == 0 || ret == -EALREADY);
                if (ret == -EALREADY) {
                    assert(node_comm_lease_complete(
                               &comm, &delivery_lease,
                               NODE_COMM_DELIVERY_FAILED, now_ms) == 0);
                }
            } else {
                int ret = node_comm_note_quiesced(&comm, &pause_lease, now_ms);
                assert(ret == 0 || ret == -ETIMEDOUT);
            }
            break;
        case NODE_COMM_PAUSED: {
            int ret = node_comm_begin_resume(&comm, &pause_lease, now_ms);
            assert(ret == 0 || ret == -ETIMEDOUT);
            break;
        }
        case NODE_COMM_RESUMING: {
            int ret = node_comm_note_resumed(&comm, &pause_lease, now_ms);
            assert(ret == 0 || ret == -ETIMEDOUT || ret == -EBUSY);
            if (ret == -EBUSY) {
                assert(node_comm_lease_complete(
                           &comm, &delivery_lease,
                           NODE_COMM_DELIVERY_FAILED, now_ms) == 0);
            }
            break;
        }
        default:
            assert(false);
        }

        if ((random_value % 3u) == 0u) {
            struct node_comm_terminal_event event;

            while (node_comm_take_terminal_event(&comm, &event)) {
                assert(event.handle < SWEEP_MAX_HANDLES);
                assert(!terminal_seen[event.handle]);
                terminal_seen[event.handle] = true;
                terminal_count++;
            }
        }
        assert_internal_invariants(&comm);
    }

    if (comm.control.state == NODE_COMM_STOPPED) {
        assert(node_comm_start(&comm, now_ms) == 0);
    }
    assert(node_comm_stop(&comm, NODE_COMM_STOP_CANCEL_ALL, now_ms) == 0);
    {
        struct node_comm_terminal_event event;

        while (node_comm_take_terminal_event(&comm, &event)) {
            assert(event.handle < SWEEP_MAX_HANDLES);
            assert(!terminal_seen[event.handle]);
            terminal_seen[event.handle] = true;
            terminal_count++;
        }
    }
    assert(terminal_count == accepted_count);
    assert(node_comm_pending_count(&comm) == 0u);
}

static void test_gateway_confirmation_is_exact_and_does_not_hold_scheduler(void)
{
    struct node_comm comm;
    struct node_comm_request uplink = request_with(
        901u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u);
    struct node_comm_request response = request_with(
        902u, NODE_COMM_PROFILE_CONTROL_RESPONSE, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t response_handle;
    uint32_t uplink_handle;

    init_running(&comm, 0u);
    uplink_handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == uplink_handle);
    assert(node_comm_confirm_delivery(&comm, uplink_handle, 0u) == -EAGAIN);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 0u) ==
           -EPROTO);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_confirm_delivery(&comm, uplink_handle, 0u) ==
           -EINPROGRESS);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(!node_comm_lease_active(&comm));
    assert(node_comm_confirm_delivery(&comm, uplink_handle + 100u, 2u) ==
           -ENOENT);

    response_handle = submit_request(&comm, &response, 2u);
    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == response_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 2u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 3u) == 0);
    assert(node_comm_confirm_delivery(&comm, uplink_handle, 4u) == 0);
    assert(node_comm_confirm_delivery(&comm, uplink_handle, 4u) ==
           -EALREADY);

    assert(node_comm_take_terminal_event_for(&comm, uplink_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
    assert(node_comm_take_terminal_event_for(&comm, response_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

static void
test_priority_yield_requeues_exact_confirmation_owner_without_refunding_rf(void)
{
    struct node_comm comm;
    struct node_comm_request durable = request_with(
        920u, NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK, 10000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t generation = 0u;
    uint32_t handle;
    uint8_t attempts = 0u;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &durable, 0u);
    assert(node_comm_delivery_generation(&comm, handle, &generation) == 0);
    assert(generation != 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.delivery_generation == generation);
    assert(lease.attempt_number == 1u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);

    assert(node_comm_requeue_awaiting_confirmation(
               &comm, handle, generation + 1u, 2u) == -ESTALE);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
    assert(node_comm_requeue_awaiting_confirmation(
               &comm, handle, generation, 2u) == 0);
    assert(node_comm_delivery_generation(&comm, handle, &generation) == 0);
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 1u);
    assert(!node_comm_peek_terminal_event_for(&comm, handle, &event));

    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == handle);
    assert(lease.delivery_generation == generation);
    assert(lease.attempt_number == 2u);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 3u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 3u) == 0);
    assert(node_comm_confirm_delivery(&comm, handle, 4u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 2u);

    /* A late exact ACK for the cancelled attempt also remains one real RF. */
    durable.client_token++;
    init_running(&comm, 0u);
    handle = submit_request(&comm, &durable, 0u);
    assert(node_comm_delivery_generation(&comm, handle, &generation) == 0);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(node_comm_requeue_awaiting_confirmation(
               &comm, handle, generation, 2u) == 0);
    assert(node_comm_confirm_delivery_external_proof(
               &comm, handle, 2u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

static void test_external_gateway_proof_recovers_only_unleased_pre_rf_states(void)
{
    struct node_comm comm;
    struct node_comm_request uplink = request_with(
        907u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;

    /* READY: ordinary confirmation remains strict, exact external proof wins. */
    init_running(&comm, 0u);
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_confirm_delivery(&comm, handle, 1u) == -EAGAIN);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    /* WAIT_RETRY has refunded its lease and may be recovered atomically. */
    init_running(&comm, 0u);
    uplink.client_token++;
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_defer_pre_rf_retry(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery(&comm, handle, 1u) == -EAGAIN);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    /* WAIT_RESOURCE likewise has no backend callback that can still start RF. */
    init_running(&comm, 0u);
    uplink.client_token++;
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_wait_resource(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery(&comm, handle, 1u) == -EAGAIN);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    /* A live pre-RF lease remains callback-owned; proof cannot revoke it. */
    init_running(&comm, 0u);
    uplink.client_token++;
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) ==
           -EINPROGRESS);
    assert(!node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(node_comm_lease_defer_pre_rf(&comm, &lease, 1u, 1u) == 0);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    /* The existing in-flight RF race keeps the same EINPROGRESS contract. */
    init_running(&comm, 0u);
    uplink.client_token++;
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) ==
           -EINPROGRESS);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery_external_proof(&comm, handle, 1u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

static void test_late_gateway_confirmation_cannot_revive_expired_delivery(void)
{
    struct node_comm comm;
    struct node_comm_request uplink = request_with(
        903u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 100u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery(&comm, handle, 100u) == -EALREADY);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 1u);
    assert(node_comm_confirm_delivery(&comm, handle, 101u) == -ENOENT);
}

static void test_backend_rf_starts_are_accounted_without_changing_retry_policy(void)
{
    struct node_comm comm;
    struct node_comm_request uplink = request_with(
        906u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 100u);
    struct node_comm_terminal_event event;
    struct node_comm_terminal_event peeked;
    struct node_comm_lease lease;
    uint8_t attempts = 0u;
    uint32_t handle;

    init_running(&comm, 0u);
    handle = submit_request(&comm, &uplink, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);

    /* The legacy backend owns retry policy; facade accounting must not cap it. */
    for (uint32_t retry = 0u; retry < 12u; retry++) {
        assert(node_comm_note_backend_rf_started(&comm,
                                                 handle,
                                                 2u + retry) == 0);
    }
    assert(node_comm_attempts_started(&comm, handle, &attempts) == 0);
    assert(attempts == 13u);
    assert(!node_comm_peek_terminal_event_for(&comm, handle, &peeked));

    assert(node_comm_confirm_delivery(&comm, handle, 20u) == 0);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &peeked));
    assert(peeked.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(peeked.attempts_started == 13u);
    assert(node_comm_note_backend_rf_started(&comm, handle, 20u) ==
           -EALREADY);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &peeked));
    assert(peeked.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(peeked.attempts_started == 14u);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 14u);
    assert(!node_comm_take_terminal_event_for(&comm, handle, &event));

    uplink.client_token++;
    uplink.absolute_deadline_ms = 50u;
    handle = submit_request(&comm, &uplink, 21u);
    assert(node_comm_acquire(&comm, 21u, &lease) == 0);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 21u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 21u) == 0);
    assert(node_comm_note_backend_rf_started(&comm, handle, 49u) == 0);
    assert(node_comm_note_backend_rf_started(&comm, handle, 50u) ==
           -ETIMEDOUT);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &peeked));
    assert(peeked.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(peeked.attempts_started == 3u);
    assert(node_comm_note_backend_rf_started(&comm, handle, 51u) ==
           -EALREADY);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &peeked));
    assert(peeked.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(peeked.attempts_started == 4u);
    assert(node_comm_take_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 4u);
    assert(!node_comm_take_terminal_event_for(&comm, handle, &event));
}

static void test_gateway_failure_is_exact_and_preserves_terminal_reason(void)
{
    struct node_comm comm;
    struct node_comm_request failed = request_with(
        904u, NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE, 1000u);
    struct node_comm_request queued = request_with(
        905u, NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE, 1000u);
    struct node_comm_terminal_event event;
    struct node_comm_lease lease;
    uint32_t failed_handle;
    uint32_t queued_handle;

    init_running(&comm, 0u);
    failed_handle = submit_request(&comm, &failed, 0u);
    queued_handle = submit_request(&comm, &queued, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == failed_handle);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 0u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);

    assert(node_comm_fail_delivery(
               &comm, queued_handle,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED, 2u) == -EAGAIN);
    assert(node_comm_fail_delivery(
               &comm, failed_handle,
               NODE_COMM_TERMINAL_DELIVERED, 2u) == -EINVAL);
    assert(node_comm_fail_delivery(
               &comm, failed_handle + 100u,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED, 2u) == -ENOENT);
    assert(node_comm_fail_delivery(
               &comm, failed_handle,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED, 2u) == 0);
    assert(node_comm_fail_delivery(
               &comm, failed_handle,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED, 2u) == -EALREADY);

    assert(node_comm_take_terminal_event_for(&comm, failed_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 1u);
    assert(node_comm_pending_count(&comm) == 1u);
    assert(node_comm_acquire(&comm, 2u, &lease) == 0);
    assert(lease.handle == queued_handle);
}

static void test_durable_retry_backoff_diversifies_fifty_reporters(void)
{
    for (uint16_t round = 1u; round <= 16u; round++) {
        uint16_t shift = round - 1u;
        uint32_t previous_delay = 0u;
        uint32_t distinct_delays = 0u;
        uint32_t base_ms;

        if (shift > 3u) {
            shift = 3u;
        }
        base_ms = 50u << shift;
        for (uint32_t anchor = 0u; anchor < 50u; anchor++) {
            uint32_t delay_ms = 0u;
            uint32_t seed = UINT32_C(0x6d2b79f5) ^
                            (anchor * UINT32_C(0x9e3779b9));

            assert(node_comm_retry_backoff_ms(
                       NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
                       seed, round, &delay_ms) == 0);
            assert(delay_ms >= base_ms / 2u);
            assert(delay_ms <= base_ms + base_ms / 2u);
            if (anchor == 0u || delay_ms != previous_delay) {
                distinct_delays++;
            }
            previous_delay = delay_ms;
        }
        assert(distinct_delays >= 20u);
    }
    assert(node_comm_retry_backoff_ms(
               NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
               0u, 1u, &(uint32_t){0}) == -EINVAL);
    assert(node_comm_retry_backoff_ms(
               NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
               1u, 0u, &(uint32_t){0}) == -EINVAL);
}

static void test_persistent_delivery_owner_emits_exact_semantic_proof(void)
{
    struct node_comm comm;
    struct delivery_trace_capture capture = {0};
    struct node_comm_request request = request_with(
        1001u, NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u);
    struct node_comm_terminal_event terminal;
    struct node_comm_lease lease;
    uint32_t handle;
    uint32_t generation = 0u;

    init_running(&comm, 0u);
    node_comm_set_delivery_transition_trace(
        &comm, capture_delivery_transition, &capture);
    handle = submit_request(&comm, &request, 0u);
    assert(capture.count == 1u);
    assert(capture.entries[0].event.type == FW_EVENT_PACKET_OWNED);
    assert(capture.entries[0].event.operation_id == handle);
    assert(capture.entries[0].event.generation != 0u);
    assert(capture.entries[0].transition.old_state == FW_DELIVERY_EMPTY);
    assert(capture.entries[0].transition.new_state == FW_DELIVERY_WAIT_TX);
    assert(!capture.entries[0].terminal_present);
    assert(comm.slots[0].owner.delivery.identity.active);
    assert(comm.slots[0].owner.delivery.state == FW_DELIVERY_WAIT_TX);
    assert(node_comm_delivery_generation(&comm, handle, &generation) == 0);
    assert(generation == capture.entries[0].event.generation);

    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(lease.handle == handle);
    assert(lease.delivery_generation == generation);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(capture.count == 2u);
    assert(capture.entries[1].event.type == FW_EVENT_RF_STARTED);
    assert(capture.entries[1].event.operation_id == handle);
    assert(capture.entries[1].event.generation == generation);
    assert(capture.entries[1].transition.old_state == FW_DELIVERY_WAIT_TX);
    assert(capture.entries[1].transition.new_state == FW_DELIVERY_WAIT_ACK);
    assert(comm.slots[0].owner.delivery.identity.active);
    assert(comm.slots[0].owner.delivery.owns_custody);
    assert(comm.slots[0].owner.delivery.state == FW_DELIVERY_WAIT_ACK);

    assert(node_comm_lease_await_confirmation(&comm, &lease, 1u) == 0);
    assert(node_comm_confirm_delivery(&comm, handle, 2u) == 0);
    assert(capture.count == 3u);
    assert(capture.entries[2].event.type == FW_EVENT_GATEWAY_ACK_RECEIVED);
    assert(capture.entries[2].event.operation_id == handle);
    assert(capture.entries[2].event.generation == generation);
    assert(capture.entries[2].transition.old_state == FW_DELIVERY_WAIT_ACK);
    assert(capture.entries[2].transition.new_state == FW_DELIVERY_DELIVERED);
    assert(capture.entries[2].terminal_present);
    assert(capture.entries[2].terminal.reason ==
           NODE_COMM_TERMINAL_DELIVERED);
    assert(capture.entries[2].terminal.proof ==
           NODE_COMM_TERMINAL_PROOF_SEMANTIC);
    assert(node_comm_take_terminal_event_for(&comm, handle, &terminal));
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_SEMANTIC);
}

static void test_resource_wait_failure_and_stale_resume_are_transactional(void)
{
    struct node_comm comm;
    struct delivery_trace_capture capture = {0};
    struct node_comm_request request = request_with(
        1002u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_resource_wait_owner old_owner;
    struct node_comm_resource_wait_owner new_owner;
    struct node_comm_resource_wait_owner owners[2];
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_terminal_event terminal;
    struct node_comm_lease lease;
    uint32_t handle;
    bool saw_failed_block = false;
    bool saw_stale_resume = false;
    bool saw_new_resume = false;

    /* Expiry between lease acquisition and blocking may terminalize the
     * delivery, but it must never publish a resource-wait owner. */
    request.absolute_deadline_ms = 10u;
    init_running(&comm, 0u);
    node_comm_set_delivery_transition_trace(
        &comm, capture_delivery_transition, &capture);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_wait_resource(&comm, &lease, 10u) == -ESTALE);
    assert(collect_resource_wait_owners(&comm, NULL, 0u) == 0u);
    for (size_t i = 0u; i < capture.count; i++) {
        if (capture.entries[i].kind ==
            NODE_COMM_DELIVERY_TRACE_RESOURCE_BLOCKED) {
            saw_failed_block = true;
        }
    }
    assert(!saw_failed_block);
    assert(node_comm_take_terminal_event_for(&comm, handle, &terminal));
    assert(terminal.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);

    /* Retain an old exact wait token, complete that generation, then redrive
     * the same handle and prove the delayed resume cannot wake the successor. */
    memset(&capture, 0, sizeof(capture));
    request.absolute_deadline_ms = 1000u;
    init_running(&comm, 0u);
    node_comm_set_delivery_transition_trace(
        &comm, capture_delivery_transition, &capture);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    assert(node_comm_lease_wait_resource(&comm, &lease, 1u) == 0);
    assert(collect_resource_wait_owners(&comm, owners, 2u) == 1u);
    old_owner = owners[0];
    assert(old_owner.handle == handle);
    assert(old_owner.delivery_generation == lease.delivery_generation);
    assert(node_comm_release_resource_wait(&comm, &old_owner, 2u) == 0);

    for (uint64_t now_ms = 2u; now_ms < 122u; now_ms += 40u) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.delivery_generation == old_owner.delivery_generation);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED,
                                        now_ms) == 0);
    }
    assert(node_comm_redrive_delivered(&comm, handle, 200u, 1000u, 122u,
                                       &prior_terminal) == 0);
    assert(prior_terminal.delivery_generation == old_owner.delivery_generation);
    assert(node_comm_acquire(&comm, 200u, &lease) == 0);
    assert(lease.delivery_generation != old_owner.delivery_generation);
    assert(node_comm_lease_wait_resource(&comm, &lease, 201u) == 0);
    assert(collect_resource_wait_owners(&comm, owners, 2u) == 1u);
    new_owner = owners[0];
    assert(new_owner.handle == handle);
    assert(new_owner.delivery_generation == lease.delivery_generation);

    assert(node_comm_release_resource_wait(&comm, &old_owner, 202u) ==
           -ESTALE);
    memset(owners, 0, sizeof(owners));
    assert(collect_resource_wait_owners(&comm, owners, 2u) == 1u);
    assert(owners[0].handle == new_owner.handle);
    assert(owners[0].delivery_generation == new_owner.delivery_generation);
    assert(node_comm_release_resource_wait(&comm, &new_owner, 203u) == 0);
    assert(collect_resource_wait_owners(&comm, NULL, 0u) == 0u);

    for (size_t i = 0u; i < capture.count; i++) {
        const struct delivery_trace_entry *entry = &capture.entries[i];

        if (entry->kind != NODE_COMM_DELIVERY_TRACE_RESOURCE_RESUMED) {
            continue;
        }
        if (entry->event.generation == old_owner.delivery_generation &&
            entry->transition.result == FW_SM_STALE) {
            saw_stale_resume = true;
        }
        if (entry->event.generation == new_owner.delivery_generation &&
            entry->transition.result == FW_SM_APPLIED) {
            saw_new_resume = true;
        }
    }
    assert(saw_stale_resume);
    assert(saw_new_resume);
    assert(node_comm_cancel(&comm, handle, 204u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &terminal));
    assert(terminal.reason == NODE_COMM_TERMINAL_CANCELLED);
}

static void test_redrive_rejects_stale_generation_without_duplicate_custody(void)
{
    struct node_comm comm;
    struct delivery_trace_capture capture = {0};
    struct node_comm_request request = request_with(
        1002u, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u);
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_terminal_event terminal;
    struct node_comm_lease lease;
    uint32_t handle;
    uint32_t old_generation = 0u;
    uint32_t new_generation = 0u;
    size_t live_owners = 0u;
    bool saw_stale = false;

    init_running(&comm, 0u);
    node_comm_set_delivery_transition_trace(
        &comm, capture_delivery_transition, &capture);
    handle = submit_request(&comm, &request, 0u);
    assert(node_comm_delivery_generation(&comm, handle, &old_generation) ==
           0);

    for (uint64_t now_ms = 0u; now_ms < 120u; now_ms += 40u) {
        assert(node_comm_acquire(&comm, now_ms, &lease) == 0);
        assert(lease.delivery_generation == old_generation);
        assert(node_comm_lease_note_rf_started(&comm, &lease, now_ms) == 0);
        assert(node_comm_lease_complete(&comm, &lease,
                                        NODE_COMM_DELIVERY_SUCCEEDED,
                                        now_ms) == 0);
    }
    assert(node_comm_redrive_delivered(&comm, handle, 200u, 1000u, 120u,
                                       &prior_terminal) == 0);
    assert(prior_terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(prior_terminal.proof == NODE_COMM_TERMINAL_PROOF_TRANSPORT);
    assert(node_comm_delivery_generation(&comm, handle, &new_generation) ==
           0);
    assert(new_generation != old_generation);
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        const struct node_comm_request_slot *slot = &comm.slots[i];

        if (!slot->terminal_pending && slot->owner.delivery.identity.active &&
            slot->owner.delivery.identity.operation_id == handle) {
            live_owners++;
            assert(slot->owner.delivery.identity.generation ==
                   new_generation);
            assert(slot->owner.delivery.owns_custody);
        }
    }
    assert(live_owners == 1u);

    assert(node_comm_acquire(&comm, 200u, &lease) == 0);
    assert(lease.delivery_generation == new_generation);
    assert(node_comm_lease_note_rf_started(&comm, &lease, 200u) == 0);
    assert(node_comm_lease_await_confirmation(&comm, &lease, 200u) == 0);
    assert(node_comm_note_backend_rf_started_for_generation(
               &comm, handle, old_generation, 201u) == -ESTALE);
    assert(node_comm_delivery_generation(&comm, handle, &new_generation) ==
           0);
    for (size_t i = 0u; i < capture.count; i++) {
        if (capture.entries[i].event.type == FW_EVENT_RF_STARTED &&
            capture.entries[i].event.operation_id == handle &&
            capture.entries[i].event.generation == old_generation &&
            capture.entries[i].transition.result == FW_SM_STALE) {
            assert(!capture.entries[i].terminal_present);
            saw_stale = true;
        }
    }
    assert(saw_stale);
    assert(node_comm_cancel(&comm, handle, 202u) == 0);
    assert(node_comm_take_terminal_event_for(&comm, handle, &terminal));
    assert(terminal.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_NONE);
}

int main(void)
{
    test_lifecycle_requires_every_transition();
    test_quiesce_waits_for_generation_checked_lease();
    test_blocked_pre_rf_opportunities_never_consume_attempts();
    test_pre_rf_retry_uses_randomized_exponential_backoff();
    test_retry_backoff_and_attempt_exhaustion();
    test_pause_rebases_retry_but_not_absolute_deadline();
    test_preserving_stop_invalidates_leases_and_rebases_retry();
    test_stop_cancel_emits_one_terminal_per_request();
    test_priority_then_fifo_selection();
    test_same_priority_deferred_head_blocks_younger_ready_record();
    test_protocol_response_preempts_durable_priority_result();
    test_deferred_durable_priority_blocks_lower_but_not_controls();
    test_non_durable_deferred_classes_remain_non_hol();
    test_cancel_retires_exact_wait_retry_owner_without_rf();
    test_bounded_control_pre_rf_deferral_retains_equal_priority_fifo();
    test_terminal_is_exactly_once_and_late_lease_is_stale();
    test_targeted_terminal_poll_leaves_other_clients_queued();
    test_next_service_due_tracks_ready_retry_and_deadline();
    test_inflight_attempt_can_finish_while_quiescing();
    test_bounded_control_flood_runs_three_successful_rf_opportunities();
    test_bounded_control_flood_does_not_count_failed_copy_as_success();
    test_single_control_origin_runs_exactly_one_rf_opportunity();
    test_delivered_control_redrive_retains_identity_and_starts_fresh_wave();
    test_delivered_control_redrive_rejects_other_profiles();
    test_twenty_independent_nodes_diversify_destructive_collisions();
    test_deadline_invalidates_active_lease();
    test_backend_guard_publishes_causal_completion_at_deadline_edges();
    test_backend_guard_is_bounded_and_request_local();
    test_capacity_includes_unconsumed_terminal_events();
    test_invalid_requests_and_retry_without_rf_fail_closed();
    test_pause_lease_owner_generation_and_expiry();
    test_pause_expiry_while_quiescing_requires_forced_reclaim();
    test_pause_then_preserving_stop_rebases_only_retry_timer();
    test_stop_during_resuming_does_not_rebase_retry_twice();
    test_all_delivery_profiles_use_priority_then_fifo_order();
    test_gateway_confirmation_is_exact_and_does_not_hold_scheduler();
    test_priority_yield_requeues_exact_confirmation_owner_without_refunding_rf();
    test_external_gateway_proof_recovers_only_unleased_pre_rf_states();
    test_late_gateway_confirmation_cannot_revive_expired_delivery();
    test_backend_rf_starts_are_accounted_without_changing_retry_policy();
    test_gateway_failure_is_exact_and_preserves_terminal_reason();
    test_durable_retry_backoff_diversifies_fifty_reporters();
    test_persistent_delivery_owner_emits_exact_semantic_proof();
    test_resource_wait_failure_and_stale_resume_are_transactional();
    test_redrive_rejects_stale_generation_without_duplicate_custody();
    test_deterministic_state_and_request_sweep();
    return 0;
}
