#ifndef NODE_COMM_H
#define NODE_COMM_H

#include "firmware_state_machines.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NODE_COMM_MAX_REQUESTS
#define NODE_COMM_MAX_REQUESTS 16u
#endif

#define NODE_COMM_PROTOCOL_RESPONSE_MAX_ATTEMPTS 16u
#define NODE_COMM_PROTOCOL_RESPONSE_RETRY_BASE_MS 200u
#define NODE_COMM_PROTOCOL_RESPONSE_RETRY_SHIFT_CAP 3u
#define NODE_COMM_PROTOCOL_RESPONSE_RETRY_BACKOFF_MAX_MS \
    (((NODE_COMM_PROTOCOL_RESPONSE_RETRY_BASE_MS << \
       NODE_COMM_PROTOCOL_RESPONSE_RETRY_SHIFT_CAP) * 3u) / 2u)
/*
 * One bounded Channel-5 control wave must either complete or return custody
 * within this per-hop horizon. Timed broadcast protocols use the same bound
 * to reserve an independent redrive wave without overtaking their shared
 * future execution instant.
 */
#define NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS 10000u

/*
 * A broadcast recipient forwards its command before dispatching it locally.
 * Applying a command at hop depth N therefore spans the gateway's origin
 * wave plus N independently bounded relay waves.  Timed protocols must use
 * this helper instead of reconstructing the flood timing from unrelated
 * fast-path constants.
 */
static inline uint32_t node_comm_bounded_control_apply_budget_ms(
    uint8_t max_hop_count)
{
    return max_hop_count == 0u ? 0u :
        ((uint32_t)max_hop_count + 1u) *
            NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS;
}

enum node_comm_lifecycle_state {
    NODE_COMM_STOPPED = 0,
    NODE_COMM_RUNNING,
    NODE_COMM_QUIESCING,
    NODE_COMM_PAUSED,
    NODE_COMM_RESUMING,
};

enum node_comm_stop_mode {
    NODE_COMM_STOP_PRESERVE_QUEUED = 0,
    NODE_COMM_STOP_CANCEL_ALL,
};

enum node_comm_delivery_outcome {
    NODE_COMM_DELIVERY_SUCCEEDED = 0,
    NODE_COMM_DELIVERY_RETRY,
    NODE_COMM_DELIVERY_FAILED,
    NODE_COMM_DELIVERY_ATTEMPTS_EXHAUSTED,
};

enum node_comm_delivery_profile {
    NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD = 0,
    NODE_COMM_PROFILE_RELIABLE_UPLINK,
    NODE_COMM_PROFILE_PRIORITY_RELIABLE_UPLINK,
    NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
    NODE_COMM_PROFILE_CONTROL_RESPONSE,
    NODE_COMM_PROFILE_BEST_EFFORT,
    /* One gateway-origin RF opportunity; downstream relays still own their
     * bounded flood repetitions encoded in the immutable command payload. */
    NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN,
    NODE_COMM_PROFILE_COUNT,
};

enum node_comm_terminal_reason {
    NODE_COMM_TERMINAL_DELIVERED = 0,
    NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
    NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
    NODE_COMM_TERMINAL_PERMANENT_FAILURE,
    NODE_COMM_TERMINAL_CANCELLED,
};

/* How the terminal DELIVERED result was proven.  Failure terminals use NONE. */
enum node_comm_terminal_proof {
    NODE_COMM_TERMINAL_PROOF_NONE = 0,
    NODE_COMM_TERMINAL_PROOF_TRANSPORT,
    NODE_COMM_TERMINAL_PROOF_SEMANTIC,
};

struct node_comm_request {
    enum node_comm_delivery_profile profile;
    uint64_t absolute_deadline_ms;
    uint32_t client_token;
    uint32_t retry_jitter_seed;
};

struct node_comm_lease {
    uint32_t handle;
    /* Delivery generation rejects a delayed backend callback after redrive. */
    uint32_t delivery_generation;
    uint32_t generation;
    uint8_t attempt_number;
};

/*
 * Exact owner of a refunded lease that is waiting for a shared backend
 * resource.  The delivery generation is required because a bounded control
 * may redrive the same logical handle with a fresh persistent owner.
 */
struct node_comm_resource_wait_owner {
    uint32_t handle;
    uint32_t delivery_generation;
};

struct node_comm_pause_lease {
    uint32_t owner;
    uint32_t generation;
    uint64_t expires_at_ms;
};

/* Lifecycle-only control is small enough to coexist with the legacy runtime. */
struct node_comm_lifecycle {
    enum node_comm_lifecycle_state state;
    uint64_t suspended_at_ms;
    uint32_t next_pause_generation;
    uint32_t generation;
    struct node_comm_pause_lease pause_lease;
    bool pause_lease_active;
    bool pause_recovery_required;
};

struct node_comm_terminal_event {
    uint32_t handle;
    /* Retains the owner identity needed to reject late physical callbacks. */
    uint32_t delivery_generation;
    uint32_t client_token;
    uint64_t terminal_at_ms;
    enum node_comm_terminal_reason reason;
    uint8_t attempts_started;
    uint8_t proof;
};

/*
 * A real request slot has one delivery owner.  While live it is the shared
 * state machine; once it has finished, the same storage becomes immutable
 * terminal evidence until its caller consumes it.  There is deliberately no
 * parallel slot lifecycle enum to reconstruct or mirror.
 */
union node_comm_delivery_owner {
    struct fw_delivery_sm delivery;
    struct node_comm_terminal_event terminal;
};

struct node_comm_request_slot {
    union node_comm_delivery_owner owner;
    struct node_comm_request request;
    uint64_t retry_due_ms;
    uint64_t enqueue_order;
    uint64_t backend_guard_expires_at_ms;
    uint32_t lease_generation;
    uint32_t retry_delay_ms;
    uint16_t retry_rounds;
    uint8_t max_attempts;
    uint8_t successful_attempts_completed;
    uint8_t retry_backoff_shift_cap;
    uint8_t priority;
    bool terminal_pending;
    bool lease_active;
    bool waiting_resource;
    bool retry_due_active;
    bool backend_guard_active;
};

enum node_comm_delivery_trace_kind {
    NODE_COMM_DELIVERY_TRACE_TRANSITION = 0,
    NODE_COMM_DELIVERY_TRACE_RESOURCE_BLOCKED,
    NODE_COMM_DELIVERY_TRACE_RESOURCE_RESUMED,
};

/*
 * This is an observation seam, not another owner or a retained log.  The
 * callback runs synchronously after every delivery-machine transition and
 * resource-wait ownership change; a non-NULL terminal value is the exact
 * immutable proof committed by that transition.  It must not call back into
 * node_comm.
 */
typedef void (*node_comm_delivery_transition_trace_fn)(
    enum node_comm_delivery_trace_kind kind,
    const struct fw_event *event,
    const struct fw_transition *transition,
    const struct node_comm_terminal_event *terminal,
    void *context);

struct node_comm {
    struct node_comm_request_slot slots[NODE_COMM_MAX_REQUESTS];
    struct node_comm_lifecycle control;
    uint64_t enqueue_sequence;
    uint32_t next_handle;
    uint32_t next_delivery_generation;
    uint32_t next_lease_generation;
    node_comm_delivery_transition_trace_fn delivery_trace;
    void *delivery_trace_context;
};

void node_comm_lifecycle_init(struct node_comm_lifecycle *lifecycle);
enum node_comm_lifecycle_state node_comm_lifecycle_state(
    const struct node_comm_lifecycle *lifecycle);
uint32_t node_comm_lifecycle_current_generation(
    const struct node_comm_lifecycle *lifecycle);
int node_comm_lifecycle_start(struct node_comm_lifecycle *lifecycle,
                              uint64_t now_ms);
int node_comm_lifecycle_request_pause(
    struct node_comm_lifecycle *lifecycle,
    uint32_t owner,
    uint32_t max_hold_ms,
    uint64_t now_ms,
    struct node_comm_pause_lease *lease_out);
int node_comm_lifecycle_note_quiesced(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced);
int node_comm_lifecycle_begin_resume(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms);
int node_comm_lifecycle_resume_ready(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced);
int node_comm_lifecycle_note_resumed(
    struct node_comm_lifecycle *lifecycle,
    const struct node_comm_pause_lease *lease,
    uint64_t now_ms,
    bool transport_quiesced);
bool node_comm_lifecycle_recovery_lease(
    const struct node_comm_lifecycle *lifecycle,
    struct node_comm_pause_lease *lease_out);
int node_comm_lifecycle_stop(struct node_comm_lifecycle *lifecycle,
                             uint64_t now_ms);
bool node_comm_lifecycle_service(struct node_comm_lifecycle *lifecycle,
                                 uint64_t now_ms);

void node_comm_init(struct node_comm *comm);
void node_comm_set_delivery_transition_trace(
    struct node_comm *comm,
    node_comm_delivery_transition_trace_fn trace,
    void *context);
enum node_comm_lifecycle_state node_comm_state(const struct node_comm *comm);
uint32_t node_comm_lifecycle_generation(const struct node_comm *comm);
int node_comm_start(struct node_comm *comm, uint64_t now_ms);
int node_comm_request_pause(struct node_comm *comm,
                            uint32_t owner,
                            uint32_t max_hold_ms,
                            uint64_t now_ms,
                            struct node_comm_pause_lease *lease_out);
int node_comm_note_quiesced(struct node_comm *comm,
                            const struct node_comm_pause_lease *lease,
                            uint64_t now_ms);
int node_comm_begin_resume(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms);
int node_comm_note_resumed(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms);
int node_comm_resume_ready(struct node_comm *comm,
                           const struct node_comm_pause_lease *lease,
                           uint64_t now_ms);
bool node_comm_pause_recovery_lease(
    const struct node_comm *comm,
    struct node_comm_pause_lease *lease_out);
int node_comm_stop(struct node_comm *comm,
                   enum node_comm_stop_mode mode,
                   uint64_t now_ms);

int node_comm_submit(struct node_comm *comm,
                     const struct node_comm_request *request,
                     uint64_t now_ms,
                     uint32_t *handle_out);
/*
 * A bounded control delivery reports local RF completion, not remote semantic
 * acceptance.  Re-arm that exact logical handle after a full path horizon so
 * an idempotent control can get a genuinely separate wake/relay wave while
 * retaining one immutable custody identity.
 */
int node_comm_redrive_delivered(
    struct node_comm *comm,
    uint32_t handle,
    uint64_t not_before_ms,
    uint64_t absolute_deadline_ms,
    uint64_t now_ms,
    struct node_comm_terminal_event *prior_terminal_out);
int node_comm_acquire(struct node_comm *comm,
                      uint64_t now_ms,
                      struct node_comm_lease *lease_out);
int node_comm_lease_note_rf_started(struct node_comm *comm,
                                    const struct node_comm_lease *lease,
                                    uint64_t now_ms);
int node_comm_lease_backend_guard_begin(
    struct node_comm *comm,
    const struct node_comm_lease *lease,
    uint64_t expires_at_ms,
    uint64_t now_ms);
int node_comm_lease_defer_pre_rf(struct node_comm *comm,
                                const struct node_comm_lease *lease,
                                uint64_t not_before_ms,
                                uint64_t now_ms);
int node_comm_lease_defer_pre_rf_retry(
    struct node_comm *comm,
    const struct node_comm_lease *lease,
    uint64_t now_ms);
int node_comm_lease_wait_resource(struct node_comm *comm,
                                  const struct node_comm_lease *lease,
                                  uint64_t now_ms);
/*
 * Enumerate exact wait owners without allocating a facade-side mirror. Start
 * with cursor zero; each successful call advances it past the returned core
 * slot. The cursor is meaningful only while the caller holds serialization.
 */
bool node_comm_resource_wait_owner_next(
    const struct node_comm *comm,
    size_t *cursor,
    struct node_comm_resource_wait_owner *owner_out);
int node_comm_release_resource_wait(struct node_comm *comm,
                                    const struct node_comm_resource_wait_owner *owner,
                                    uint64_t now_ms);
int node_comm_lease_complete(struct node_comm *comm,
                             const struct node_comm_lease *lease,
                             enum node_comm_delivery_outcome outcome,
                             uint64_t now_ms);
int node_comm_lease_await_confirmation(struct node_comm *comm,
                                       const struct node_comm_lease *lease,
                                       uint64_t now_ms);
/*
 * After the transport has synchronously cancelled this exact reliable owner,
 * stop waiting for an ACK that can no longer arrive and make the preserved
 * delivery eligible for a later attempt.  The spent RF attempt and immutable
 * delivery generation are retained; a stale cancellation completion cannot
 * requeue a newer owner.
 */
int node_comm_requeue_awaiting_confirmation(
    struct node_comm *comm,
    uint32_t handle,
    uint32_t delivery_generation,
    uint64_t now_ms);
int node_comm_confirm_delivery(struct node_comm *comm,
                               uint32_t handle,
                               uint64_t now_ms);
/*
 * Terminalize an exact reliable delivery after its higher-level owner has
 * validated external semantic proof. Unlike the ordinary confirmation path,
 * this may recover a facade record whose asynchronous backend already sent
 * RF while the facade still reflects a pre-RF retry state. A live lease is
 * never invalidated underneath its backend call.
 */
int node_comm_confirm_delivery_external_proof(struct node_comm *comm,
                                              uint32_t handle,
                                              uint64_t now_ms);
/* Generation-aware ingress is required for asynchronous backend callbacks. */
int node_comm_confirm_delivery_external_proof_for_generation(
    struct node_comm *comm,
    uint32_t handle,
    uint32_t delivery_generation,
    uint64_t now_ms);
int node_comm_fail_delivery(struct node_comm *comm,
                            uint32_t handle,
                            enum node_comm_terminal_reason reason,
                            uint64_t now_ms);
int node_comm_note_backend_rf_started(struct node_comm *comm,
                                      uint32_t handle,
                                      uint64_t now_ms);
int node_comm_note_backend_rf_started_for_generation(
    struct node_comm *comm,
    uint32_t handle,
    uint32_t delivery_generation,
    uint64_t now_ms);
int node_comm_delivery_generation(const struct node_comm *comm,
                                  uint32_t handle,
                                  uint32_t *generation_out);
int node_comm_cancel(struct node_comm *comm,
                     uint32_t handle,
                     uint64_t now_ms);
size_t node_comm_service(struct node_comm *comm, uint64_t now_ms);
bool node_comm_next_service_due_ms(const struct node_comm *comm,
                                   uint64_t now_ms,
                                   uint64_t *due_ms_out);
bool node_comm_take_terminal_event(struct node_comm *comm,
                                   struct node_comm_terminal_event *event_out);
bool node_comm_take_terminal_event_for(
    struct node_comm *comm,
    uint32_t handle,
    struct node_comm_terminal_event *event_out);
bool node_comm_peek_terminal_event_for(
    const struct node_comm *comm,
    uint32_t handle,
    struct node_comm_terminal_event *event_out);
size_t node_comm_pending_count(const struct node_comm *comm);
bool node_comm_lease_active(const struct node_comm *comm);
int node_comm_attempts_started(const struct node_comm *comm,
                               uint32_t handle,
                               uint8_t *attempts_out);
int node_comm_retry_backoff_ms(enum node_comm_delivery_profile profile,
                               uint32_t retry_jitter_seed,
                               uint16_t retry_round,
                               uint32_t *delay_ms_out);

#endif
