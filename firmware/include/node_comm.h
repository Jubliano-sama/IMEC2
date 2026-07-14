#ifndef NODE_COMM_H
#define NODE_COMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NODE_COMM_MAX_REQUESTS
#define NODE_COMM_MAX_REQUESTS 16u
#endif

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
};

enum node_comm_delivery_profile {
    NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD = 0,
    NODE_COMM_PROFILE_RELIABLE_UPLINK,
    NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
    NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
    NODE_COMM_PROFILE_CONTROL_RESPONSE,
    NODE_COMM_PROFILE_BEST_EFFORT,
    NODE_COMM_PROFILE_COUNT,
};

enum node_comm_terminal_reason {
    NODE_COMM_TERMINAL_DELIVERED = 0,
    NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
    NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
    NODE_COMM_TERMINAL_PERMANENT_FAILURE,
    NODE_COMM_TERMINAL_CANCELLED,
};

enum node_comm_request_slot_state {
    NODE_COMM_SLOT_FREE = 0,
    NODE_COMM_SLOT_READY,
    NODE_COMM_SLOT_WAIT_RETRY,
    NODE_COMM_SLOT_LEASED,
    NODE_COMM_SLOT_WAIT_CONFIRMATION,
    NODE_COMM_SLOT_TERMINAL,
};

struct node_comm_request {
    enum node_comm_delivery_profile profile;
    uint64_t absolute_deadline_ms;
    uint32_t client_token;
    uint32_t retry_jitter_seed;
};

struct node_comm_lease {
    uint32_t handle;
    uint32_t generation;
    uint8_t attempt_number;
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
    uint32_t client_token;
    enum node_comm_terminal_reason reason;
    uint8_t attempts_started;
};

struct node_comm_request_slot {
    struct node_comm_request request;
    struct node_comm_terminal_event terminal;
    uint64_t retry_due_ms;
    uint64_t enqueue_order;
    uint32_t handle;
    uint32_t lease_generation;
    uint32_t retry_delay_ms;
    enum node_comm_request_slot_state state;
    uint16_t retry_rounds;
    uint8_t attempts_started;
    uint8_t backend_attempts_started;
    uint8_t max_attempts;
    uint8_t retry_backoff_shift_cap;
    uint8_t priority;
    bool rf_started;
};

struct node_comm {
    struct node_comm_request_slot slots[NODE_COMM_MAX_REQUESTS];
    struct node_comm_lifecycle control;
    uint64_t enqueue_sequence;
    uint32_t next_handle;
    uint32_t next_lease_generation;
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
int node_comm_acquire(struct node_comm *comm,
                      uint64_t now_ms,
                      struct node_comm_lease *lease_out);
int node_comm_lease_note_rf_started(struct node_comm *comm,
                                    const struct node_comm_lease *lease,
                                    uint64_t now_ms);
int node_comm_lease_defer_pre_rf(struct node_comm *comm,
                                const struct node_comm_lease *lease,
                                uint64_t not_before_ms,
                                uint64_t now_ms);
int node_comm_lease_defer_pre_rf_retry(
    struct node_comm *comm,
    const struct node_comm_lease *lease,
    uint64_t now_ms);
int node_comm_lease_complete(struct node_comm *comm,
                             const struct node_comm_lease *lease,
                             enum node_comm_delivery_outcome outcome,
                             uint64_t now_ms);
int node_comm_lease_await_confirmation(struct node_comm *comm,
                                       const struct node_comm_lease *lease,
                                       uint64_t now_ms);
int node_comm_confirm_delivery(struct node_comm *comm,
                               uint32_t handle,
                               uint64_t now_ms);
int node_comm_fail_delivery(struct node_comm *comm,
                            uint32_t handle,
                            enum node_comm_terminal_reason reason,
                            uint64_t now_ms);
int node_comm_note_backend_rf_started(struct node_comm *comm,
                                      uint32_t handle,
                                      uint64_t now_ms);
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
