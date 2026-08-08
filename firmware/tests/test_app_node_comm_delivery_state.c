#include "node_comm.h"

#include <assert.h>
#include <errno.h>

static void test_invalid_terminal_sequence_preserves_custody(void)
{
    struct node_comm comm;
    struct node_comm_request request = {
        .profile = NODE_COMM_PROFILE_BEST_EFFORT,
        .client_token = UINT32_C(0x55aa),
    };
    struct node_comm_lease lease;
    struct node_comm_terminal_event event = {0};
    uint32_t handle = 0u;
    uint32_t lease_generation;

    node_comm_init(&comm);
    assert(node_comm_start(&comm, 0u) == 0);
    assert(node_comm_submit(&comm, &request, 0u, &handle) == 0);
    assert(node_comm_acquire(&comm, 0u, &lease) == 0);
    lease_generation = lease.generation;

    /* A success before RF is an invalid terminal sequence. */
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) ==
           -EPROTO);
    assert(node_comm_pending_count(&comm) == 1u);
    assert(node_comm_lease_active(&comm));
    assert(comm.slots[0].lease_generation == lease_generation);
    assert(!node_comm_peek_terminal_event_for(&comm, handle, &event));

    /* The same custody can still complete after the valid RF boundary. */
    assert(node_comm_lease_note_rf_started(&comm, &lease, 1u) == 0);
    assert(node_comm_lease_complete(&comm, &lease,
                                    NODE_COMM_DELIVERY_SUCCEEDED, 1u) == 0);
    assert(node_comm_peek_terminal_event_for(&comm, handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

int main(void)
{
    test_invalid_terminal_sequence_preserves_custody();
    return 0;
}
