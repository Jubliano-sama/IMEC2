#include "node_comm.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

struct node_comm_profile_policy {
    uint32_t retry_delay_ms;
    uint32_t success_repeat_delay_ms;
    uint8_t max_attempts;
    uint8_t successful_attempts_required;
    uint8_t retry_backoff_shift_cap;
    uint8_t priority;
};

static const struct node_comm_profile_policy profile_policies[] = {
    [NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD] = {
        .retry_delay_ms = 200u,
        .success_repeat_delay_ms = 40u,
        .max_attempts = 4u,
        .successful_attempts_required = 4u,
        .retry_backoff_shift_cap = 3u,
        .priority = 255u,
    },
    [NODE_COMM_PROFILE_RELIABLE_UPLINK] = {
        .retry_delay_ms = 1500u,
        .success_repeat_delay_ms = 0u,
        .max_attempts = 4u,
        .successful_attempts_required = 1u,
        .retry_backoff_shift_cap = 2u,
        .priority = 120u,
    },
    [NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK] = {
        .retry_delay_ms = 50u,
        .success_repeat_delay_ms = 0u,
        .max_attempts = 16u,
        .successful_attempts_required = 1u,
        .retry_backoff_shift_cap = 3u,
        .priority = 210u,
    },
    [NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE] = {
        .retry_delay_ms = NODE_COMM_PROTOCOL_RESPONSE_RETRY_BASE_MS,
        .success_repeat_delay_ms = 0u,
        /*
         * A survey START result can become ready while the gateway is still
         * completing the required four-copy channel-5 control flood. Keep
         * enough channel-9 opportunities to outlive that bounded blackout;
         * the caller's absolute deadline remains the final time bound.
         */
        .max_attempts = NODE_COMM_PROTOCOL_RESPONSE_MAX_ATTEMPTS,
        .successful_attempts_required = 1u,
        .retry_backoff_shift_cap =
            NODE_COMM_PROTOCOL_RESPONSE_RETRY_SHIFT_CAP,
        .priority = 220u,
    },
    [NODE_COMM_PROFILE_CONTROL_RESPONSE] = {
        .retry_delay_ms = 200u,
        .success_repeat_delay_ms = 0u,
        .max_attempts = 4u,
        .successful_attempts_required = 1u,
        .retry_backoff_shift_cap = 3u,
        /*
         * Match bounded control so FIFO preserves an ACK already owed for an
         * accepted response before a later gateway command starts its flood.
         */
        .priority = 255u,
    },
    [NODE_COMM_PROFILE_BEST_EFFORT] = {
        .retry_delay_ms = 0u,
        .success_repeat_delay_ms = 0u,
        .max_attempts = 1u,
        .successful_attempts_required = 1u,
        .retry_backoff_shift_cap = 0u,
        .priority = 20u,
    },
};

_Static_assert(sizeof(profile_policies) / sizeof(profile_policies[0]) ==
                   NODE_COMM_PROFILE_COUNT,
               "every node communication profile needs immutable policy");

static uint64_t add_saturating_u64(uint64_t lhs, uint64_t rhs)
{
    return UINT64_MAX - lhs < rhs ? UINT64_MAX : lhs + rhs;
}

static uint32_t lifecycle_next_pause_generation(
    struct node_comm_lifecycle *lifecycle)
{
    lifecycle->next_pause_generation++;
    if (lifecycle->next_pause_generation == 0u) {
        lifecycle->next_pause_generation++;
    }
    return lifecycle->next_pause_generation;
}

static void lifecycle_force_reclaim(struct node_comm_lifecycle *lifecycle)
{
    lifecycle->state = NODE_COMM_RESUMING;
    lifecycle->generation++;
    lifecycle->pause_lease = (struct node_comm_pause_lease) {
        .owner = UINT32_MAX,
        .generation = lifecycle_next_pause_generation(lifecycle),
        .expires_at_ms = UINT64_MAX,
    };
    lifecycle->pause_lease_active = true;
    lifecycle->pause_recovery_required = true;
}

static int lifecycle_validate_pause_lease(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms)
{
    if (lifecycle == NULL || lease == NULL) {
        return -EINVAL;
    }
    if (!lifecycle->pause_lease_active ||
        lease->owner != lifecycle->pause_lease.owner ||
        lease->generation != lifecycle->pause_lease.generation ||
        lease->expires_at_ms != lifecycle->pause_lease.expires_at_ms) {
        return -ESTALE;
    }
    if (!lifecycle->pause_recovery_required &&
        now_ms >= lifecycle->pause_lease.expires_at_ms) {
        lifecycle_force_reclaim(lifecycle);
        return -ETIMEDOUT;
    }
    return 0;
}

void node_comm_lifecycle_init(struct node_comm_lifecycle *lifecycle)
{
    if (lifecycle == NULL) {
        return;
    }
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->state = NODE_COMM_STOPPED;
}

enum node_comm_lifecycle_state node_comm_lifecycle_state(
    const struct node_comm_lifecycle *lifecycle)
{
    return lifecycle == NULL ? NODE_COMM_STOPPED : lifecycle->state;
}

uint32_t node_comm_lifecycle_current_generation(
    const struct node_comm_lifecycle *lifecycle)
{
    return lifecycle == NULL ? 0u : lifecycle->generation;
}

int node_comm_lifecycle_start(struct node_comm_lifecycle *lifecycle,
                              uint64_t now_ms)
{
    if (lifecycle == NULL || lifecycle->state != NODE_COMM_STOPPED) {
        return -EINVAL;
    }
    lifecycle->suspended_at_ms = now_ms;
    lifecycle->state = NODE_COMM_RUNNING;
    lifecycle->generation++;
    return 0;
}

int node_comm_lifecycle_request_pause(
    struct node_comm_lifecycle *lifecycle,
    uint32_t owner,
    uint32_t max_hold_ms,
    uint64_t now_ms,
    struct node_comm_pause_lease *lease_out)
{
    if (lifecycle == NULL || lease_out == NULL || owner == 0u ||
        owner == UINT32_MAX || max_hold_ms == 0u) {
        return -EINVAL;
    }
    (void)node_comm_lifecycle_service(lifecycle, now_ms);
    if (lifecycle->state != NODE_COMM_RUNNING ||
        lifecycle->pause_lease_active) {
        return -EINVAL;
    }
    lifecycle->pause_lease = (struct node_comm_pause_lease) {
        .owner = owner,
        .generation = lifecycle_next_pause_generation(lifecycle),
        .expires_at_ms = add_saturating_u64(now_ms, max_hold_ms),
    };
    lifecycle->pause_lease_active = true;
    *lease_out = lifecycle->pause_lease;
    lifecycle->state = NODE_COMM_QUIESCING;
    lifecycle->generation++;
    return 0;
}

int node_comm_lifecycle_note_quiesced(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced)
{
    int ret = lifecycle_validate_pause_lease(lifecycle, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (lifecycle->state != NODE_COMM_QUIESCING) {
        return -EINVAL;
    }
    if (!transport_quiesced) {
        return -EBUSY;
    }
    lifecycle->suspended_at_ms = now_ms;
    lifecycle->state = NODE_COMM_PAUSED;
    lifecycle->generation++;
    return 0;
}

int node_comm_lifecycle_begin_resume(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms)
{
    int ret = lifecycle_validate_pause_lease(lifecycle, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (lifecycle->state != NODE_COMM_PAUSED) {
        return -EINVAL;
    }
    lifecycle->state = NODE_COMM_RESUMING;
    lifecycle->generation++;
    return 0;
}

int node_comm_lifecycle_resume_ready(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced)
{
    int ret = lifecycle_validate_pause_lease(lifecycle, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (lifecycle->state != NODE_COMM_RESUMING) {
        return -EINVAL;
    }
    return transport_quiesced ? 0 : -EBUSY;
}

int node_comm_lifecycle_note_resumed(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced)
{
    int ret = node_comm_lifecycle_resume_ready(lifecycle, lease, now_ms,
                                               transport_quiesced);

    if (ret < 0) {
        return ret;
    }
    lifecycle->state = NODE_COMM_RUNNING;
    lifecycle->generation++;
    lifecycle->pause_lease_active = false;
    lifecycle->pause_recovery_required = false;
    memset(&lifecycle->pause_lease, 0, sizeof(lifecycle->pause_lease));
    return 0;
}

bool node_comm_lifecycle_recovery_lease(
    const struct node_comm_lifecycle *lifecycle,
    struct node_comm_pause_lease *lease_out)
{
    if (lifecycle == NULL || lease_out == NULL ||
        !lifecycle->pause_lease_active ||
        !lifecycle->pause_recovery_required ||
        lifecycle->state != NODE_COMM_RESUMING) {
        return false;
    }
    *lease_out = lifecycle->pause_lease;
    return true;
}

int node_comm_lifecycle_stop(struct node_comm_lifecycle *lifecycle,
                             uint64_t now_ms)
{
    if (lifecycle == NULL) {
        return -EINVAL;
    }
    if (lifecycle->state == NODE_COMM_STOPPED) {
        return -EALREADY;
    }
    lifecycle->suspended_at_ms = now_ms;
    lifecycle->state = NODE_COMM_STOPPED;
    lifecycle->generation++;
    lifecycle->pause_lease_active = false;
    lifecycle->pause_recovery_required = false;
    memset(&lifecycle->pause_lease, 0, sizeof(lifecycle->pause_lease));
    return 0;
}

bool node_comm_lifecycle_service(struct node_comm_lifecycle *lifecycle,
                                 uint64_t now_ms)
{
    if (lifecycle == NULL || !lifecycle->pause_lease_active ||
        lifecycle->pause_recovery_required ||
        now_ms < lifecycle->pause_lease.expires_at_ms) {
        return false;
    }
    lifecycle_force_reclaim(lifecycle);
    return true;
}

static uint64_t retry_backoff_ms(uint32_t base_delay_ms,
                                 uint8_t backoff_shift_cap,
                                 uint32_t retry_jitter_seed,
                                 uint16_t retry_round)
{
    uint16_t shift = retry_round == 0u ? 0u :
                     (uint16_t)(retry_round - 1u);
    uint64_t delay = base_delay_ms;

    if (shift > backoff_shift_cap) {
        shift = backoff_shift_cap;
    }
    for (uint16_t i = 0u; i < shift; i++) {
        delay = add_saturating_u64(delay, delay);
    }
    if (retry_jitter_seed != 0u && delay != 0u &&
        delay != UINT64_MAX) {
        uint32_t mixed = retry_jitter_seed ^
                         ((uint32_t)retry_round * UINT32_C(0x9e3779b9));
        uint64_t half = delay / 2u;
        uint64_t width = delay == UINT64_MAX ? UINT64_MAX : delay + 1u;

        mixed ^= mixed >> 16;
        mixed *= UINT32_C(0x7feb352d);
        mixed ^= mixed >> 15;
        mixed *= UINT32_C(0x846ca68b);
        mixed ^= mixed >> 16;
        delay = delay - half + ((uint64_t)mixed % width);
    }
    return delay;
}

static uint64_t retry_delay_ms(const struct node_comm_request_slot *slot)
{
    return retry_backoff_ms(slot->retry_delay_ms,
                            slot->retry_backoff_shift_cap,
                            slot->request.retry_jitter_seed,
                            slot->retry_rounds);
}

static void schedule_retry(struct node_comm_request_slot *slot,
                           uint64_t now_ms)
{
    if (slot->retry_rounds < UINT16_MAX) {
        slot->retry_rounds++;
    }
    slot->retry_due_ms = add_saturating_u64(now_ms, retry_delay_ms(slot));
    slot->state = NODE_COMM_SLOT_WAIT_RETRY;
}

int node_comm_retry_backoff_ms(enum node_comm_delivery_profile profile,
                               uint32_t retry_jitter_seed,
                               uint16_t retry_round,
                               uint32_t *delay_ms_out)
{
    uint64_t delay_ms;

    if (profile >= NODE_COMM_PROFILE_COUNT || retry_jitter_seed == 0u ||
        retry_round == 0u || delay_ms_out == NULL) {
        return -EINVAL;
    }
    delay_ms = retry_backoff_ms(profile_policies[profile].retry_delay_ms,
                                profile_policies[profile].retry_backoff_shift_cap,
                                retry_jitter_seed,
                                retry_round);
    if (delay_ms > UINT32_MAX) {
        return -EOVERFLOW;
    }
    *delay_ms_out = (uint32_t)delay_ms;
    return 0;
}

static bool deadline_expired(const struct node_comm_request_slot *slot,
                             uint64_t now_ms)
{
    return slot->request.absolute_deadline_ms != 0u &&
           now_ms >= slot->request.absolute_deadline_ms;
}

static struct node_comm_request_slot *find_handle(struct node_comm *comm,
                                                   uint32_t handle)
{
    if (comm == NULL || handle == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state != NODE_COMM_SLOT_FREE &&
            comm->slots[i].handle == handle) {
            return &comm->slots[i];
        }
    }
    return NULL;
}

static bool handle_in_use(const struct node_comm *comm, uint32_t handle)
{
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state != NODE_COMM_SLOT_FREE &&
            comm->slots[i].handle == handle) {
            return true;
        }
    }
    return false;
}

static uint32_t next_handle(struct node_comm *comm)
{
    for (size_t i = 0u; i <= NODE_COMM_MAX_REQUESTS; i++) {
        comm->next_handle++;
        if (comm->next_handle == 0u) {
            comm->next_handle++;
        }
        if (!handle_in_use(comm, comm->next_handle)) {
            return comm->next_handle;
        }
    }
    return 0u;
}

static uint32_t next_lease_generation(struct node_comm *comm)
{
    comm->next_lease_generation++;
    if (comm->next_lease_generation == 0u) {
        comm->next_lease_generation++;
    }
    return comm->next_lease_generation;
}

static uint32_t next_pause_generation(struct node_comm *comm)
{
    comm->control.next_pause_generation++;
    if (comm->control.next_pause_generation == 0u) {
        comm->control.next_pause_generation++;
    }
    return comm->control.next_pause_generation;
}

static void invalidate_lease(struct node_comm *comm,
                             struct node_comm_request_slot *slot)
{
    slot->lease_generation = next_lease_generation(comm);
    slot->rf_started = false;
}

static uint8_t total_attempts_started(
    const struct node_comm_request_slot *slot)
{
    uint16_t total;

    if (slot == NULL) {
        return 0u;
    }
    total = (uint16_t)slot->attempts_started +
            (uint16_t)slot->backend_attempts_started;
    return total > UINT8_MAX ? UINT8_MAX : (uint8_t)total;
}

static void terminalize(struct node_comm *comm,
                        struct node_comm_request_slot *slot,
                        enum node_comm_terminal_reason reason)
{
    if (slot->state == NODE_COMM_SLOT_FREE ||
        slot->state == NODE_COMM_SLOT_TERMINAL) {
        return;
    }
    invalidate_lease(comm, slot);
    slot->terminal = (struct node_comm_terminal_event) {
        .handle = slot->handle,
        .client_token = slot->request.client_token,
        .reason = reason,
        .attempts_started = total_attempts_started(slot),
    };
    slot->state = NODE_COMM_SLOT_TERMINAL;
}

static int validate_lease(struct node_comm *comm,
                          const struct node_comm_lease *lease,
                          struct node_comm_request_slot **slot_out)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || lease == NULL || slot_out == NULL) {
        return -EINVAL;
    }
    slot = find_handle(comm, lease->handle);
    if (slot == NULL || slot->state != NODE_COMM_SLOT_LEASED ||
        slot->lease_generation != lease->generation) {
        return -ESTALE;
    }
    *slot_out = slot;
    return 0;
}

static void rebase_retry_timers(struct node_comm *comm,
                                uint64_t suspended_at_ms,
                                uint64_t now_ms)
{
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        struct node_comm_request_slot *slot = &comm->slots[i];
        uint64_t remaining;

        if (slot->state != NODE_COMM_SLOT_WAIT_RETRY) {
            continue;
        }
        remaining = slot->retry_due_ms > suspended_at_ms ?
                    slot->retry_due_ms - suspended_at_ms : 0u;
        slot->retry_due_ms = add_saturating_u64(now_ms, remaining);
    }
}

static void expire_pause_lease(struct node_comm *comm, uint64_t now_ms)
{
    if (!comm->control.pause_lease_active || comm->control.pause_recovery_required ||
        now_ms < comm->control.pause_lease.expires_at_ms) {
        return;
    }
    if (comm->control.state == NODE_COMM_PAUSED) {
        rebase_retry_timers(comm, comm->control.suspended_at_ms, now_ms);
    }
    if (comm->control.state == NODE_COMM_QUIESCING ||
        comm->control.state == NODE_COMM_PAUSED ||
        comm->control.state == NODE_COMM_RESUMING) {
        comm->control.state = NODE_COMM_RESUMING;
        comm->control.generation++;
    }
    comm->control.pause_lease = (struct node_comm_pause_lease) {
        .owner = UINT32_MAX,
        .generation = next_pause_generation(comm),
        .expires_at_ms = UINT64_MAX,
    };
    comm->control.pause_lease_active = true;
    comm->control.pause_recovery_required = true;
}

static int validate_pause_lease(struct node_comm *comm,
                                const struct node_comm_pause_lease *lease,
                                uint64_t now_ms)
{
    if (comm == NULL || lease == NULL) {
        return -EINVAL;
    }
    if (!comm->control.pause_lease_active ||
        lease->owner != comm->control.pause_lease.owner ||
        lease->generation != comm->control.pause_lease.generation ||
        lease->expires_at_ms != comm->control.pause_lease.expires_at_ms) {
        return -ESTALE;
    }
    if (!comm->control.pause_recovery_required &&
        now_ms >= comm->control.pause_lease.expires_at_ms) {
        expire_pause_lease(comm, now_ms);
        return -ETIMEDOUT;
    }
    return 0;
}

void node_comm_init(struct node_comm *comm)
{
    if (comm == NULL) {
        return;
    }
    memset(comm, 0, sizeof(*comm));
    comm->control.state = NODE_COMM_STOPPED;
}

enum node_comm_lifecycle_state node_comm_state(const struct node_comm *comm)
{
    return comm == NULL ? NODE_COMM_STOPPED : comm->control.state;
}

uint32_t node_comm_lifecycle_generation(const struct node_comm *comm)
{
    return comm == NULL ? 0u : comm->control.generation;
}

int node_comm_start(struct node_comm *comm, uint64_t now_ms)
{
    if (comm == NULL || comm->control.state != NODE_COMM_STOPPED) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    rebase_retry_timers(comm, comm->control.suspended_at_ms, now_ms);
    comm->control.state = NODE_COMM_RUNNING;
    comm->control.generation++;
    return 0;
}

int node_comm_request_pause(struct node_comm *comm,
                            uint32_t owner,
                            uint32_t max_hold_ms,
                            uint64_t now_ms,
                            struct node_comm_pause_lease *lease_out)
{
    if (comm == NULL || lease_out == NULL || owner == 0u ||
        owner == UINT32_MAX ||
        max_hold_ms == 0u) {
        return -EINVAL;
    }
    expire_pause_lease(comm, now_ms);
    if (comm->control.state != NODE_COMM_RUNNING || comm->control.pause_lease_active) {
        return -EINVAL;
    }
    comm->control.pause_lease = (struct node_comm_pause_lease) {
        .owner = owner,
        .generation = next_pause_generation(comm),
        .expires_at_ms = add_saturating_u64(now_ms, max_hold_ms),
    };
    comm->control.pause_lease_active = true;
    *lease_out = comm->control.pause_lease;
    comm->control.state = NODE_COMM_QUIESCING;
    comm->control.generation++;
    return 0;
}

int node_comm_note_quiesced(struct node_comm *comm,
                            const struct node_comm_pause_lease *lease,
                            uint64_t now_ms)
{
    int ret = validate_pause_lease(comm, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (comm->control.state != NODE_COMM_QUIESCING) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    if (node_comm_lease_active(comm)) {
        return -EBUSY;
    }
    comm->control.suspended_at_ms = now_ms;
    comm->control.state = NODE_COMM_PAUSED;
    comm->control.generation++;
    return 0;
}

int node_comm_begin_resume(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms)
{
    int ret = validate_pause_lease(comm, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (comm->control.state != NODE_COMM_PAUSED) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    rebase_retry_timers(comm, comm->control.suspended_at_ms, now_ms);
    comm->control.state = NODE_COMM_RESUMING;
    comm->control.generation++;
    return 0;
}

int node_comm_note_resumed(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms)
{
    int ret = node_comm_resume_ready(comm, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    comm->control.state = NODE_COMM_RUNNING;
    comm->control.generation++;
    comm->control.pause_lease_active = false;
    comm->control.pause_recovery_required = false;
    memset(&comm->control.pause_lease, 0, sizeof(comm->control.pause_lease));
    return 0;
}

int node_comm_resume_ready(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms)
{
    int ret = validate_pause_lease(comm, lease, now_ms);

    if (ret < 0) {
        return ret;
    }
    if (comm->control.state != NODE_COMM_RESUMING) {
        return -EINVAL;
    }
    return node_comm_lease_active(comm) ? -EBUSY : 0;
}

bool node_comm_pause_recovery_lease(
    const struct node_comm *comm,
    struct node_comm_pause_lease *lease_out)
{
    if (comm == NULL || lease_out == NULL ||
        !comm->control.pause_lease_active || !comm->control.pause_recovery_required ||
        comm->control.state != NODE_COMM_RESUMING) {
        return false;
    }
    *lease_out = comm->control.pause_lease;
    return true;
}

int node_comm_stop(struct node_comm *comm,
                   enum node_comm_stop_mode mode,
                   uint64_t now_ms)
{
    bool retry_timer_suspended;

    if (comm == NULL ||
        (mode != NODE_COMM_STOP_PRESERVE_QUEUED &&
         mode != NODE_COMM_STOP_CANCEL_ALL)) {
        return -EINVAL;
    }
    if (comm->control.state == NODE_COMM_STOPPED) {
        return -EALREADY;
    }
    retry_timer_suspended = comm->control.state == NODE_COMM_PAUSED;
    (void)node_comm_service(comm, now_ms);
    if (retry_timer_suspended && comm->control.state == NODE_COMM_PAUSED) {
        rebase_retry_timers(comm, comm->control.suspended_at_ms, now_ms);
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        struct node_comm_request_slot *slot = &comm->slots[i];

        if (slot->state == NODE_COMM_SLOT_FREE ||
            slot->state == NODE_COMM_SLOT_TERMINAL) {
            continue;
        }
        if (mode == NODE_COMM_STOP_CANCEL_ALL) {
            terminalize(comm, slot, NODE_COMM_TERMINAL_CANCELLED);
            continue;
        }
        if (slot->state == NODE_COMM_SLOT_LEASED) {
            bool rf_started = slot->rf_started;

            invalidate_lease(comm, slot);
            if (rf_started) {
                if (slot->attempts_started >= slot->max_attempts) {
                    terminalize(comm, slot,
                                NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
                } else {
                    schedule_retry(slot, now_ms);
                }
            } else {
                slot->state = NODE_COMM_SLOT_READY;
            }
        }
    }
    comm->control.suspended_at_ms = now_ms;
    comm->control.state = NODE_COMM_STOPPED;
    comm->control.generation++;
    comm->control.pause_lease_active = false;
    comm->control.pause_recovery_required = false;
    memset(&comm->control.pause_lease, 0, sizeof(comm->control.pause_lease));
    return 0;
}

int node_comm_submit(struct node_comm *comm,
                     const struct node_comm_request *request,
                     uint64_t now_ms,
                     uint32_t *handle_out)
{
    struct node_comm_request_slot *slot = NULL;
    uint32_t handle;

    if (comm == NULL || request == NULL || handle_out == NULL ||
        request->profile < NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD ||
        request->profile >= NODE_COMM_PROFILE_COUNT) {
        return -EINVAL;
    }
    if (comm->control.state != NODE_COMM_RUNNING) {
        return -ESHUTDOWN;
    }
    if (request->absolute_deadline_ms != 0u &&
        now_ms >= request->absolute_deadline_ms) {
        return -ETIMEDOUT;
    }
    (void)node_comm_service(comm, now_ms);
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state == NODE_COMM_SLOT_FREE) {
            slot = &comm->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        return -ENOSPC;
    }
    handle = next_handle(comm);
    if (handle == 0u) {
        return -ENOSPC;
    }
    memset(slot, 0, sizeof(*slot));
    slot->request = *request;
    slot->retry_delay_ms = profile_policies[request->profile].retry_delay_ms;
    slot->max_attempts = profile_policies[request->profile].max_attempts;
    slot->retry_backoff_shift_cap =
        profile_policies[request->profile].retry_backoff_shift_cap;
    slot->priority = profile_policies[request->profile].priority;
    slot->handle = handle;
    slot->enqueue_order = ++comm->enqueue_sequence;
    slot->state = NODE_COMM_SLOT_READY;
    *handle_out = handle;
    return 0;
}

int node_comm_acquire(struct node_comm *comm,
                      uint64_t now_ms,
                      struct node_comm_lease *lease_out)
{
    struct node_comm_request_slot *selected = NULL;

    if (comm == NULL || lease_out == NULL) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    if (comm->control.state != NODE_COMM_RUNNING) {
        return -ESHUTDOWN;
    }
    if (node_comm_lease_active(comm)) {
        return -EBUSY;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        struct node_comm_request_slot *slot = &comm->slots[i];

        if (slot->state != NODE_COMM_SLOT_READY &&
            !(slot->state == NODE_COMM_SLOT_WAIT_RETRY &&
              now_ms >= slot->retry_due_ms)) {
            continue;
        }
        if (selected == NULL ||
            slot->priority > selected->priority ||
            (slot->priority == selected->priority &&
             slot->enqueue_order < selected->enqueue_order)) {
            selected = slot;
        }
    }
    if (selected == NULL) {
        return -EAGAIN;
    }
    selected->state = NODE_COMM_SLOT_LEASED;
    selected->rf_started = false;
    selected->lease_generation = next_lease_generation(comm);
    *lease_out = (struct node_comm_lease) {
        .handle = selected->handle,
        .generation = selected->lease_generation,
        .attempt_number = (uint8_t)(selected->attempts_started + 1u),
    };
    return 0;
}

int node_comm_lease_note_rf_started(struct node_comm *comm,
                                    const struct node_comm_lease *lease,
                                    uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (comm->control.state == NODE_COMM_STOPPED) {
        return -ESHUTDOWN;
    }
    if (slot->rf_started) {
        return -EALREADY;
    }
    if (slot->attempts_started >= slot->max_attempts) {
        terminalize(comm, slot, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
        return -ENOSPC;
    }
    slot->attempts_started++;
    slot->rf_started = true;
    return 0;
}

int node_comm_lease_defer_pre_rf(struct node_comm *comm,
                                const struct node_comm_lease *lease,
                                uint64_t not_before_ms,
                                uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (slot->rf_started) {
        return -EALREADY;
    }
    invalidate_lease(comm, slot);
    if (not_before_ms > now_ms) {
        slot->retry_due_ms = not_before_ms;
        slot->state = NODE_COMM_SLOT_WAIT_RETRY;
    } else {
        slot->state = NODE_COMM_SLOT_READY;
    }
    return 0;
}

int node_comm_lease_defer_pre_rf_retry(
    struct node_comm *comm,
    const struct node_comm_lease *lease,
    uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (slot->rf_started) {
        return -EALREADY;
    }
    invalidate_lease(comm, slot);
    schedule_retry(slot, now_ms);
    return 0;
}

int node_comm_lease_wait_resource(struct node_comm *comm,
                                  const struct node_comm_lease *lease,
                                  uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (slot->rf_started) {
        return -EALREADY;
    }
    invalidate_lease(comm, slot);
    slot->state = NODE_COMM_SLOT_WAIT_RESOURCE;
    return 0;
}

int node_comm_release_resource_wait(struct node_comm *comm,
                                    uint32_t handle,
                                    uint64_t now_ms)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || handle == 0u) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    slot = find_handle(comm, handle);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == NODE_COMM_SLOT_TERMINAL) {
        return -EALREADY;
    }
    if (slot->state != NODE_COMM_SLOT_WAIT_RESOURCE) {
        return -EAGAIN;
    }
    slot->state = NODE_COMM_SLOT_READY;
    return 0;
}

int node_comm_lease_complete(struct node_comm *comm,
                             const struct node_comm_lease *lease,
                             enum node_comm_delivery_outcome outcome,
                             uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    if (outcome > NODE_COMM_DELIVERY_ATTEMPTS_EXHAUSTED) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (outcome == NODE_COMM_DELIVERY_SUCCEEDED && !slot->rf_started) {
        return -EPROTO;
    }
    if (outcome == NODE_COMM_DELIVERY_SUCCEEDED) {
        const struct node_comm_profile_policy *policy =
            &profile_policies[slot->request.profile];

        if (slot->attempts_started < policy->successful_attempts_required) {
            invalidate_lease(comm, slot);
            slot->retry_due_ms = add_saturating_u64(
                now_ms, policy->success_repeat_delay_ms);
            slot->state = NODE_COMM_SLOT_WAIT_RETRY;
            return 0;
        }
        terminalize(comm, slot, NODE_COMM_TERMINAL_DELIVERED);
        return 0;
    }
    if (outcome == NODE_COMM_DELIVERY_FAILED) {
        terminalize(comm, slot, NODE_COMM_TERMINAL_PERMANENT_FAILURE);
        return 0;
    }
    if (outcome == NODE_COMM_DELIVERY_ATTEMPTS_EXHAUSTED) {
        terminalize(comm, slot, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
        return 0;
    }
    if (!slot->rf_started) {
        return -EPROTO;
    }
    if (slot->attempts_started >= slot->max_attempts) {
        terminalize(comm, slot, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
        return 0;
    }
    invalidate_lease(comm, slot);
    schedule_retry(slot, now_ms);
    return 0;
}

int node_comm_lease_await_confirmation(struct node_comm *comm,
                                       const struct node_comm_lease *lease,
                                       uint64_t now_ms)
{
    struct node_comm_request_slot *slot;
    int ret;

    (void)node_comm_service(comm, now_ms);
    ret = validate_lease(comm, lease, &slot);
    if (ret < 0) {
        return ret;
    }
    if (!slot->rf_started) {
        return -EPROTO;
    }
    invalidate_lease(comm, slot);
    slot->state = NODE_COMM_SLOT_WAIT_CONFIRMATION;
    return 0;
}

int node_comm_confirm_delivery(struct node_comm *comm,
                               uint32_t handle,
                               uint64_t now_ms)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || handle == 0u) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    slot = find_handle(comm, handle);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == NODE_COMM_SLOT_TERMINAL) {
        return -EALREADY;
    }
    if (slot->state == NODE_COMM_SLOT_LEASED && slot->rf_started) {
        /* A synchronous backend confirmation will be committed at lease exit. */
        return -EINPROGRESS;
    }
    if (slot->state != NODE_COMM_SLOT_WAIT_CONFIRMATION) {
        return -EAGAIN;
    }
    terminalize(comm, slot, NODE_COMM_TERMINAL_DELIVERED);
    return 0;
}

int node_comm_fail_delivery(struct node_comm *comm,
                            uint32_t handle,
                            enum node_comm_terminal_reason reason,
                            uint64_t now_ms)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || handle == 0u ||
        (reason != NODE_COMM_TERMINAL_DEADLINE_EXPIRED &&
         reason != NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED &&
         reason != NODE_COMM_TERMINAL_PERMANENT_FAILURE)) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    slot = find_handle(comm, handle);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == NODE_COMM_SLOT_TERMINAL) {
        return -EALREADY;
    }
    if (slot->state != NODE_COMM_SLOT_WAIT_CONFIRMATION) {
        return -EAGAIN;
    }
    terminalize(comm, slot, reason);
    return 0;
}

int node_comm_note_backend_rf_started(struct node_comm *comm,
                                      uint32_t handle,
                                      uint64_t now_ms)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || handle == 0u) {
        return -EINVAL;
    }
    slot = find_handle(comm, handle);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == NODE_COMM_SLOT_TERMINAL) {
        /*
         * A backend can pass its final preflight, lose the race to facade
         * terminalization, and still have started RF before cancellation took
         * effect. Preserve the terminal reason and state, but update the
         * frozen terminal snapshot so every real RF start remains observable.
         */
        if (slot->backend_attempts_started != UINT8_MAX) {
            slot->backend_attempts_started++;
        }
        slot->terminal.attempts_started = total_attempts_started(slot);
        return -EALREADY;
    }
    if (slot->state != NODE_COMM_SLOT_WAIT_CONFIRMATION) {
        return -EAGAIN;
    }
    if (slot->backend_attempts_started != UINT8_MAX) {
        slot->backend_attempts_started++;
    }
    if (deadline_expired(slot, now_ms)) {
        terminalize(comm, slot, NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
        return -ETIMEDOUT;
    }
    return 0;
}

int node_comm_cancel(struct node_comm *comm,
                     uint32_t handle,
                     uint64_t now_ms)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL) {
        return -EINVAL;
    }
    (void)node_comm_service(comm, now_ms);
    slot = find_handle(comm, handle);
    if (slot == NULL) {
        return -ENOENT;
    }
    if (slot->state == NODE_COMM_SLOT_TERMINAL) {
        return -EALREADY;
    }
    terminalize(comm, slot, NODE_COMM_TERMINAL_CANCELLED);
    return 0;
}

size_t node_comm_service(struct node_comm *comm, uint64_t now_ms)
{
    size_t expired = 0u;

    if (comm == NULL) {
        return 0u;
    }
    expire_pause_lease(comm, now_ms);
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        struct node_comm_request_slot *slot = &comm->slots[i];

        if (slot->state != NODE_COMM_SLOT_FREE &&
            slot->state != NODE_COMM_SLOT_TERMINAL &&
            deadline_expired(slot, now_ms)) {
            terminalize(comm, slot, NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
            expired++;
        }
    }
    return expired;
}

bool node_comm_next_service_due_ms(const struct node_comm *comm,
                                   uint64_t now_ms,
                                   uint64_t *due_ms_out)
{
    uint64_t due_ms = UINT64_MAX;
    bool found = false;

    if (comm == NULL || due_ms_out == NULL ||
        comm->control.state != NODE_COMM_RUNNING) {
        return false;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        const struct node_comm_request_slot *slot = &comm->slots[i];
        uint64_t slot_due_ms = UINT64_MAX;

        if (slot->state == NODE_COMM_SLOT_READY) {
            slot_due_ms = now_ms;
        } else if (slot->state == NODE_COMM_SLOT_WAIT_RETRY) {
            slot_due_ms = slot->retry_due_ms;
        } else if (slot->state == NODE_COMM_SLOT_WAIT_CONFIRMATION ||
                   slot->state == NODE_COMM_SLOT_WAIT_RESOURCE) {
            if (slot->request.absolute_deadline_ms == 0u) {
                continue;
            }
            slot_due_ms = slot->request.absolute_deadline_ms;
        } else if (slot->state != NODE_COMM_SLOT_LEASED) {
            continue;
        }
        if (slot->request.absolute_deadline_ms != 0u &&
            slot->request.absolute_deadline_ms < slot_due_ms) {
            slot_due_ms = slot->request.absolute_deadline_ms;
        }
        if (!found || slot_due_ms < due_ms) {
            due_ms = slot_due_ms;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *due_ms_out = due_ms;
    return true;
}

bool node_comm_take_terminal_event_for(
    struct node_comm *comm,
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    struct node_comm_request_slot *slot;

    if (comm == NULL || handle == 0u || event_out == NULL) {
        return false;
    }
    slot = find_handle(comm, handle);
    if (slot == NULL || slot->state != NODE_COMM_SLOT_TERMINAL) {
        return false;
    }
    *event_out = slot->terminal;
    memset(slot, 0, sizeof(*slot));
    return true;
}

bool node_comm_peek_terminal_event_for(
    const struct node_comm *comm,
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    if (comm == NULL || handle == 0u || event_out == NULL) {
        return false;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        const struct node_comm_request_slot *slot = &comm->slots[i];

        if (slot->state == NODE_COMM_SLOT_TERMINAL &&
            slot->handle == handle) {
            *event_out = slot->terminal;
            return true;
        }
    }
    return false;
}

bool node_comm_take_terminal_event(struct node_comm *comm,
                                   struct node_comm_terminal_event *event_out)
{
    if (comm == NULL || event_out == NULL) {
        return false;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        struct node_comm_request_slot *slot = &comm->slots[i];

        if (slot->state == NODE_COMM_SLOT_TERMINAL) {
            *event_out = slot->terminal;
            memset(slot, 0, sizeof(*slot));
            return true;
        }
    }
    return false;
}

size_t node_comm_pending_count(const struct node_comm *comm)
{
    size_t count = 0u;

    if (comm == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state != NODE_COMM_SLOT_FREE) {
            count++;
        }
    }
    return count;
}

bool node_comm_lease_active(const struct node_comm *comm)
{
    if (comm == NULL) {
        return false;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state == NODE_COMM_SLOT_LEASED) {
            return true;
        }
    }
    return false;
}

int node_comm_attempts_started(const struct node_comm *comm,
                               uint32_t handle,
                               uint8_t *attempts_out)
{
    if (comm == NULL || attempts_out == NULL || handle == 0u) {
        return -EINVAL;
    }
    for (size_t i = 0u; i < NODE_COMM_MAX_REQUESTS; i++) {
        if (comm->slots[i].state != NODE_COMM_SLOT_FREE &&
            comm->slots[i].handle == handle) {
            *attempts_out = total_attempts_started(&comm->slots[i]);
            return 0;
        }
    }
    return -ENOENT;
}
