#ifndef APP_GATEWAY_BLE_H
#define APP_GATEWAY_BLE_H

#include "gateway_command.h"
#include "gateway_membership.h"
#include "app_gateway_command_observability.h"
#include "app_gateway_ble_stream.h"
#include "mesh.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gateway_ble_status {
    bool connected;
    bool packet_notify_enabled;
};

enum gateway_ble_stream_reservation_result {
    GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED = 1,
};

int gateway_ble_init(void);
int gateway_ble_send_packet_frame(const uint8_t *frame, size_t frame_len);
void gateway_ble_get_status(struct gateway_ble_status *status);
bool gateway_ble_uwb_quiet_active(void);
void gateway_ble_enter_uwb_quiet(const char *reason);
void gateway_ble_exit_uwb_quiet(const char *reason);
int gateway_encode_host_packet_frame(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *frame,
                                     size_t frame_cap,
                                     size_t *frame_len,
                                     struct proto_packet *frame_packet);
int gateway_emit_host_packet(const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len);
int gateway_ble_stream_packet(const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t received_at_ms);
/*
 * Returns one gateway_ble_stream_reservation_result value on success, or a
 * negative errno. Only ACQUIRED authorizes commit or cancellation.
 */
int gateway_ble_reserve_stream_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms);
int gateway_ble_commit_stream_reservation(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len);
int gateway_ble_commit_stream_reservation_projection(
    const struct proto_packet *packet,
    const uint8_t *raw_payload,
    size_t raw_payload_len,
    uint8_t accepted_record_mask);
void gateway_ble_cancel_stream_reservation(void);
/* Complete the retained host item only after its mesh ACK/EACK action owns a
 * mesh/radio retry path. */
int gateway_ble_finish_host_delivery(const struct proto_packet *packet);
/*
 * Finish a host-accepted command-event head. Non-command packets return zero
 * so the generic receipt path can call this safely; a mismatched command
 * event fails closed and must block mesh receipt completion.
 */
int gateway_command_event_finish_host_receipt(
    const struct proto_packet *packet);
/*
 * Validate a duplicate command-event receipt after the first exact receipt
 * already retired the stream head. The complete host-receipt identity,
 * including its record digest, must match the RAM-only last-receipt cache.
 */
int gateway_command_event_duplicate_host_receipt_valid(
    const struct gateway_host_receipt_identity *identity);
/*
 * Consume a serial host receipt.  Return 1 when the frame is a host receipt
 * (accepted or rejected), 0 when it is an ordinary frame, and a negative
 * errno only for a decoded host receipt that failed validation.
 */
int gateway_ble_accept_host_receipt_frame(const uint8_t *frame,
                                          size_t frame_len);
/*
 * Validate the complete command/collection semantic transition without
 * mutating its durable or protocol-visible state. The mesh RX owner must
 * remain held until the matching gateway_note_* call.
 */
int gateway_preflight_result_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t first_received_at_ms,
    uint32_t result_validation_token,
    uint64_t previous_hop_id);
int gateway_result_bundle_host_projection_mask(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t *accepted_record_mask);
int gateway_finalize_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    int semantic_acceptance);
/*
 * The exact GUI receipt of a stale collection result owns the only legal
 * source-custody release after gateway reset. This emits a bounded recovery
 * EACK from RAM; it never writes a collection receipt journal.
 */
int gateway_collection_recovery_after_host_receipt(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/* Bind a stale collection packet to the collection lane before its reserved
 * BLE record is exposed.  Matching cancellation rolls that pre-receipt owner
 * back if the stream commit itself fails. */
int gateway_collection_recovery_reserve_host_custody(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int gateway_collection_recovery_cancel_host_custody(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/* The recovery flood remains a collection barrier until the BLE record was
 * retired; only then may this exact owner be released. */
int gateway_collection_recovery_finish_host_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/* True only while a frozen RAM-only recovery EACK exists. The mesh delivery
 * owner combines this with its exact HOST_ACCEPTED state before it permits
 * that EACK to pass later queued stale packets. */
bool gateway_collection_recovery_active(void);
/* True only for the frozen recovery EACK itself.  Mesh coordinator bypasses
 * must combine this with their local retained HOST_ACCEPTED owner state. */
bool gateway_collection_recovery_flood_matches(
    const struct mesh_outbound *outbound);
void gateway_ble_stream_get_status(struct gateway_ble_stream_diagnostics *diagnostics);
int gateway_observe_command_event(struct gateway_command_event *event,
                                  bool terminal);
/* Reserve a command-event transport identity without exposing a BLE record. */
int gateway_reserve_command_event_sequence(struct gateway_command_event *event,
                                           void *ctx);
int gateway_observe_command_event_if_available(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx);
/* Publisher-only BLE boundary. It is the only command-event path that marks
 * the outer record ACK-required and retains it until an exact GUI receipt. */
int gateway_publish_assignment_event_if_available(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx);
int gateway_observe_command_acceptance_if_available(
    struct gateway_command_event *queued);
uint16_t gateway_next_command_seq(void);
int gateway_broadcast_command_sequence_init(void);
uint32_t gateway_next_broadcast_command_seq(void);
void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason);
void gateway_emit_host_command_result_reserved(
    uint32_t result_reservation_token,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason);
/* Convert one exact admission reservation into a local gateway result.  The
 * caller retains its token until this returns zero, so an ambiguous durable
 * assignment save can retry without fabricating a second terminal result. */
int gateway_commit_host_command_result_reserved(
    uint32_t result_reservation_token,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason);
void gateway_command_result_set_dispatch_token(uint32_t token);
uint32_t gateway_command_result_get_dispatch_token(void);
int gateway_command_result_reserve_ingress(uint32_t *token);
int gateway_command_result_bind_ingress(uint32_t token,
                                        const struct proto_packet *command,
                                        enum command_id command_id);
int gateway_command_result_rebind_command(
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command);
void gateway_command_result_release_ingress(uint32_t token);
void gateway_command_result_release_command(
    const struct proto_packet *command,
    enum command_id command_id);
void gateway_command_result_release_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id);
void gateway_command_result_release_terminal_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id);
int gateway_begin_command_result_wait(const struct mesh_outbound *command,
                                      enum command_id command_id);
int gateway_begin_command_result_wait_for(const struct mesh_outbound *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms);
int gateway_begin_command_result_wait_until(
    const struct mesh_outbound *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms);
/* Two-phase singleton admission used before an infrequent durable identity. */
int gateway_command_result_wait_reserve(uint32_t *reservation_token);
int gateway_command_result_wait_commit(
    uint32_t reservation_token,
    const struct mesh_outbound *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms);
int gateway_command_result_wait_cancel(uint32_t reservation_token);
int gateway_command_result_validation_reserve(
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t *token);
int gateway_protocol_validation_arm(uint32_t receive_timeout_ms,
                                    uint32_t *token);
int gateway_protocol_validation_complete(uint32_t token,
                                         uint64_t received_at_ms);
enum gateway_command_result_validation_check
gateway_protocol_validation_check_interval(uint32_t started_at_ms,
                                           uint32_t deadline_ms);
void gateway_command_result_validation_release_reserved(uint32_t token);
bool gateway_clear_pending_command_result(const struct proto_packet *command);
int gateway_begin_command_collection(const struct gateway_command_options *options);
void gateway_clear_command_collection(const struct gateway_command_options *options);
int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             const uint8_t *slots,
                                             size_t node_count,
                                             uint32_t assignment_epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment,
                                             const struct gateway_membership_publication *publication);
int gateway_set_registered_membership_roster_ram_only(
    uint16_t membership_epoch,
    const uint64_t *node_ids,
    const uint8_t *slots,
    size_t node_count,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication);
int gateway_abort_pending_assignment_publication_ram_only(
    const struct gateway_command_event *base_event);
/* The exact NVS commit may have landed before readback failed. This positive
 * result retains the prepared publication and original operation lease while
 * owner work adopts the same candidate; it is neither failure nor success. */
#define GATEWAY_MEMBERSHIP_COMMIT_ADOPTION_PENDING 1
int gateway_get_registered_membership_roster_with_slots(
    uint64_t *node_ids,
    uint8_t *slots,
    size_t node_cap,
    size_t *node_count,
    uint16_t *membership_epoch);
int gateway_get_registered_membership_assignment_identity(
    uint16_t membership_epoch,
    size_t node_count,
    uint32_t *assignment_epoch,
    uint32_t *assignment_table_seq,
    struct discovery_assignment_table_commitment *table_commitment,
    uint8_t *slot_span);
/* Return 1 only when the current durable roster proves this exact historical
 * assignment response and 0 when it does not. The caller must supply the
 * already-validated nonzero assignment identity used by semantic admission.
 * This narrow transport-retirement proof does not expose or mutate the
 * durable snapshot. */
int gateway_registered_membership_proves_assignment_ack(
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id);
bool gateway_assignment_publication_pending(void);
/* Reconstruct the exact sparse assignment publication after durable restore. */
int gateway_replay_pending_assignment_publication(void);
int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx);
void gateway_clear_registered_membership_roster(void);
int gateway_clear_registered_membership_roster_ram_only(void);
int gateway_note_command_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t first_received_at_ms,
                                uint32_t result_validation_token,
                                uint64_t previous_hop_id,
                                uint8_t received_radio_channel,
                                const struct mesh_event_plan *current_channel9_plan);
int gateway_note_command_result_bundle(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint8_t received_radio_channel,
                                       const struct mesh_event_plan *current_channel9_plan);
void gateway_command_result_tracking_init(void);
/*
 * Re-arm route-owned persistence and collection work whose scheduling was
 * rejected while the shared mesh transport was paused.
 */
void gateway_command_result_tracking_resume(void);

#endif
