#ifndef APP_NODE_COMM_H
#define APP_NODE_COMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "node_comm.h"
#include "protocol.h"

/*
 * Five immutable priority uplinks (or stress datagrams) plus one slot that
 * ordinary reliable uplinks cannot consume. A producer reserves its complete
 * five-record burst before taking the radio.
 */
#define APP_NODE_COMM_MAX_DELIVERIES 6u
#define APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES 1u
#define APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY \
    (APP_NODE_COMM_MAX_DELIVERIES - \
     APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES)
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
/*
 * The synthetic transmitter deliberately owns full-size extended-PHR stress
 * frames.  Keep that RAM cost out of production roles while still freezing
 * every accepted test datagram inside the communication service.
 */
#define APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN PACKET_EXT_MAX_PAYLOAD_LEN
#else
#define APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN 192u
#endif
#define APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN PACKET_EXT_MAX_PAYLOAD_LEN
#define APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS 120000u
/*
 * A caller that requests a delivery handle owns its terminal event.  Losing
 * that owner's work item must not leave bounded communication capacity pinned
 * forever.  This grace is intentionally much longer than every normal owner
 * poll interval, but shorter than the watchdog progress lease so the system
 * recovers while durable source custody is still authoritative.
 */
#define APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS 30000u

struct app_mesh_report_callbacks;
struct mesh_delivery_health;
struct mesh_outbound;
struct proto_packet;

enum app_node_comm_route_refresh_event_kind {
    APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT = 0,
    APP_NODE_COMM_ROUTE_REFRESH_BACKOFF,
    APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,
};

struct app_node_comm_route_refresh_event {
    enum app_node_comm_route_refresh_event_kind kind;
    struct proto_packet correlation;
    uint32_t gateway_sequence;
    uint8_t attempt;
    uint8_t sent_count;
    int result;
    bool correlated;
};

struct app_node_comm_control_response_health {
    uint32_t submitted;
    uint32_t admission_failures;
    uint32_t delivered;
    uint32_t failed;
    enum node_comm_terminal_reason last_terminal_reason;
    uint8_t last_attempts_started;
};

struct app_node_comm_durable_attempt_ops {
    int (*begin)(const struct proto_packet *packet, uint8_t *attempt_token);
    int (*complete)(const struct proto_packet *packet,
                    uint8_t attempt_token,
                    bool rf_started);
};

/* Every reserved record belongs to one semantic producer generation. */
enum app_node_comm_reservation_owner_kind {
    APP_NODE_COMM_RESERVATION_OWNER_RELIABLE_UPLINK = 1,
    APP_NODE_COMM_RESERVATION_OWNER_PRIORITY_RELIABLE_UPLINK,
    APP_NODE_COMM_RESERVATION_OWNER_COMMAND_RESPONSE,
    APP_NODE_COMM_RESERVATION_OWNER_BOUNDED_CONTROL,
};

/*
 * The complete capability for a bounded in-RAM reservation. Callers retain
 * this exact value for commit or cancellation; a different generation, token,
 * or expiry is stale and cannot affect a successor owner.
 */
struct app_node_comm_reservation_lease {
    uint64_t expires_at_ms;
    uint64_t owner_generation;
    uint32_t token;
    uint8_t owner_kind;
};

_Static_assert(sizeof(struct app_node_comm_reservation_lease) == 24u,
               "reservation lease RAM budget changed");

/*
 * Protocol-facing communication service boundary.
 *
 * The first compatibility phase deliberately aliases the proven mesh runtime
 * envelope and callbacks.  Protocol code should use this header so the backing
 * transport can be separated without another protocol-wide API migration.
 */
typedef struct mesh_outbound app_node_comm_envelope;
typedef struct app_mesh_report_callbacks app_node_comm_callbacks;
typedef struct mesh_delivery_health app_node_comm_delivery_health;

enum app_node_comm_control_send_mode {
    APP_NODE_COMM_CONTROL_WAKE_IF_NEEDED = 0,
    APP_NODE_COMM_CONTROL_ACCEPTED_EXCHANGE = 1,
};

int app_node_comm_init(const app_node_comm_callbacks *callbacks);
/*
 * For production-boundary diagnostics and tests. The callback is invoked
 * while node-comm holds its serialized ownership lock and must not re-enter
 * the app_node_comm API.
 */
int app_node_comm_set_delivery_transition_trace(
    node_comm_delivery_transition_trace_fn trace,
    void *context);
int app_node_comm_register_durable_attempt_ops(
    const struct app_node_comm_durable_attempt_ops *ops);
void app_node_comm_stop_role_scan(void);
void app_node_comm_restart_role_scan(void);
int app_node_comm_send(const app_node_comm_envelope *envelope,
                       const char *reason);
int app_node_comm_send_control(
    const app_node_comm_envelope *envelope,
    uint8_t purpose,
    enum app_node_comm_control_send_mode mode,
    const char *reason);
int app_node_comm_send_control_flood(const app_node_comm_envelope *envelope,
                                     uint8_t purpose,
                                     const char *reason,
                                     bool *sent_now);
int app_node_comm_schedule_path_refresh(uint64_t target_id,
                                        const char *reason);
int app_node_comm_retry_backoff_ms(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint16_t retry_round,
    uint32_t *delay_ms_out);
int app_node_comm_retry_identity_backoff_ms(
    uint64_t node_id,
    uint32_t session_id,
    uint32_t opportunity,
    enum node_comm_delivery_profile profile,
    uint16_t retry_round,
    uint32_t *delay_ms_out);
int app_node_comm_queue_local_delivery(
    const app_node_comm_envelope *envelope);
int app_node_comm_submit_delivery(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_redrive_delivered_control(
    uint32_t handle,
    uint64_t not_before_ms,
    uint64_t absolute_deadline_ms,
    struct node_comm_terminal_event *prior_terminal_out);
int app_node_comm_submit_control_response(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token);
int app_node_comm_submit_reliable_uplink(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_submit_best_effort_uplink(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token);
int app_node_comm_submit_protocol_response(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
/*
 * Retain a handle for exact application correlation while making terminal
 * cleanup facade-owned from admission. This avoids a later caller-to-auto
 * ownership transition after the semantic action has already been accepted.
 */
int app_node_comm_submit_protocol_response_auto_reap(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
/*
 * Reserve one ordinary-capacity bounded-control record before an infrequent
 * durable operation identity is consumed. Commit is the first point at which
 * RF work becomes visible; cancellation leaves no delivery behind.
 */
int app_node_comm_reserve_bounded_control(
    uint64_t owner_generation,
    struct app_node_comm_reservation_lease *reservation_out);
int app_node_comm_commit_bounded_control_reservation(
    const struct app_node_comm_reservation_lease *reservation,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_cancel_bounded_control_reservation(
    const struct app_node_comm_reservation_lease *reservation);
int app_node_comm_reserve_reliable_uplinks(
    uint64_t owner_generation,
    size_t reservation_count,
    struct app_node_comm_reservation_lease *reservation_leases,
    size_t reservation_lease_capacity);
int app_node_comm_commit_reliable_uplink_reservation(
    const struct app_node_comm_reservation_lease *reservation,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_cancel_reliable_uplink_reservation(
    const struct app_node_comm_reservation_lease *reservation);
/*
 * Reserve the ordinary-capacity records for a complete durable burst while
 * retaining the protocol-response slot.  The committed records use the
 * durable attempt journal, but they do not gain admission to the protocol
 * reserve merely by selecting that retry profile.
 */
int app_node_comm_reserve_priority_reliable_uplinks(
    uint64_t owner_generation,
    size_t reservation_count,
    struct app_node_comm_reservation_lease *reservation_leases,
    size_t reservation_lease_capacity);
int app_node_comm_commit_priority_reliable_uplink_reservation(
    const struct app_node_comm_reservation_lease *reservation,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_cancel_priority_reliable_uplink_reservation(
    const struct app_node_comm_reservation_lease *reservation);
int app_node_comm_reserve_protocol_response(
    uint64_t owner_generation,
    struct app_node_comm_reservation_lease *reservation_out);
int app_node_comm_commit_protocol_response(
    const struct app_node_comm_reservation_lease *reservation,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_commit_protocol_response_auto_reap(
    const struct app_node_comm_reservation_lease *reservation,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out);
int app_node_comm_cancel_protocol_response_reservation(
    const struct app_node_comm_reservation_lease *reservation);
int app_node_comm_service_deliveries(void);
int app_node_comm_gateway_delivery_safe_boundary(void);
int app_node_comm_cancel_delivery(uint32_t handle);
int app_node_comm_abandon_delivery(uint32_t handle);
int app_node_comm_auto_reap_delivery(uint32_t handle);
/* Returns 1 while the exact facade record exists, 0 after retirement. */
int app_node_comm_delivery_handle_state(uint32_t handle);
/*
 * Mesh-route publishes an exact asynchronous backend-release completion, then
 * calls this wake edge so the communication owner can consume it. It does not
 * itself release or reap the terminal record.
 */
void app_node_comm_backend_release_ready(uint32_t handle,
                                         uint32_t request_token);
bool app_node_comm_take_delivery_event(
    struct node_comm_terminal_event *event_out);
bool app_node_comm_take_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out);
bool app_node_comm_peek_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out);
int app_node_comm_delivery_attempts_started(uint32_t handle,
                                            uint8_t *attempts_out);
/* Exact first physical RF edge retained by the delivery owner. */
int app_node_comm_delivery_first_rf_started_at(
    uint32_t handle,
    uint64_t *rf_started_at_ms_out);
/* The active delivery generation binds an asynchronous backend attempt. */
int app_node_comm_delivery_generation(uint32_t handle,
                                      uint32_t *generation_out);
/* True only while the exact delivery generation still owns this packet. */
bool app_node_comm_delivery_owner_matches(uint32_t delivery_generation,
                                          uint64_t target_id,
                                          uint16_t packet_seq,
                                          uint8_t msg_type);
/*
 * Read the already-recorded attempt count without advancing delivery policy.
 * This is suitable for non-mutating semantic-admission preflight while the
 * serialized RX owner remains held.
 */
int app_node_comm_peek_delivery_attempts_started(uint32_t handle,
                                                 uint8_t *attempts_out);
int app_node_comm_note_gateway_confirmed(const struct proto_packet *packet);
int app_node_comm_note_gateway_confirmed_at(
    const struct proto_packet *packet,
    uint64_t confirmed_at_ms);
int app_node_comm_note_gateway_confirmed_digest_at(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint64_t confirmed_at_ms);
int app_node_comm_note_gateway_failed(
    const struct proto_packet *packet,
    enum node_comm_terminal_reason reason);
int app_node_comm_note_gateway_failed_digest(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum node_comm_terminal_reason reason);
int app_node_comm_backend_retry_preflight(const struct proto_packet *packet);
int app_node_comm_backend_retry_preflight_until(
    const struct proto_packet *packet,
    uint64_t *absolute_deadline_ms_out);
/*
 * External backends retain this generation and return it with completion.
 * Packet-only compatibility entry points assume callbacks remain serialized
 * behind the one outstanding attempt; they cannot identify an older attempt
 * after a logical control redrive.
 */
int app_node_comm_backend_retry_preflight_with_generation(
    const struct proto_packet *packet,
    uint32_t *delivery_generation_out,
    uint64_t *absolute_deadline_ms_out);
int app_node_comm_complete_backend_attempt(const struct proto_packet *packet,
                                           bool rf_started);
int app_node_comm_complete_backend_attempt_at(
    const struct proto_packet *packet,
    bool rf_started,
    uint64_t rf_started_at_ms);
int app_node_comm_complete_backend_attempt_for_generation(
    const struct proto_packet *packet,
    uint32_t delivery_generation,
    bool rf_started);
int app_node_comm_complete_backend_attempt_at_for_generation(
    const struct proto_packet *packet,
    uint32_t delivery_generation,
    bool rf_started,
    uint64_t rf_started_at_ms);
int app_node_comm_note_backend_rf_started(const struct proto_packet *packet);
int app_node_comm_note_backend_rf_started_for_generation(
    const struct proto_packet *packet,
    uint32_t delivery_generation);
size_t app_node_comm_pending_delivery_count(void);
size_t app_node_comm_reliable_delivery_targets(uint64_t *target_ids,
                                               size_t target_cap);
bool app_node_comm_delivery_backlog_active(void);
bool app_node_comm_ack_wait_active(void);
void app_node_comm_control_response_health_get(
    struct app_node_comm_control_response_health *health);
void app_node_comm_delivery_health_get(app_node_comm_delivery_health *health);
bool app_node_comm_policy_running(void);
/*
 * Temporarily stop only mesh transport work while a higher-priority local
 * radio operation reaches its physical start. The communication facade keeps
 * queued custody intact and serializes the final resume with lifecycle pause.
 */
int app_node_comm_transport_preempt_begin(void);
int app_node_comm_transport_preempt_ready(void);
void app_node_comm_transport_preempt_end(void);
int app_node_comm_pause_request(uint32_t owner,
                                uint32_t max_hold_ms,
                                struct node_comm_pause_lease *lease_out);
int app_node_comm_pause_note_quiesced(
    const struct node_comm_pause_lease *lease);
int app_node_comm_resume_begin(const struct node_comm_pause_lease *lease);
int app_node_comm_resume_complete(const struct node_comm_pause_lease *lease);
bool app_node_comm_forced_reclaim_lease(
    struct node_comm_pause_lease *lease_out);
int app_node_comm_stop_preserving_queued(void);
int app_node_comm_start(void);
void app_node_comm_lifecycle_service(void);
int app_node_comm_request_route_refresh(uint32_t delay_ms,
                                        const char *reason,
                                        bool forced);
int app_node_comm_request_route_refresh_correlated(
    uint32_t delay_ms,
    const char *reason,
    const struct proto_packet *correlation);
int app_node_comm_request_route_refresh_correlated_bounded(
    uint32_t delay_ms,
    const char *reason,
    const struct proto_packet *correlation,
    uint32_t timeout_ms);

#endif
