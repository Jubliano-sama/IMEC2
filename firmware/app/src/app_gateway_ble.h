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
    /*
     * The exact host-output journal is already terminal. Its restored or
     * same-boot stream record owns delivery, so the caller must not commit or
     * cancel the stream singleton as though it acquired a new reservation.
     */
    GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL = 2,
    /*
     * A compact durable receipt proves that this exact raw packet already
     * reached terminal host notification. No stream/journal slot is owned;
     * the caller may commit duplicate transport acceptance and re-ACK it.
     */
    GATEWAY_BLE_STREAM_RESERVATION_RECEIPT_TERMINAL = 3,
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
/*
 * A failed NVS write/readback can leave a terminal journal phase durable even
 * though the caller observed an error. Block new semantic admission until the
 * exact journal owner has been classified and restored into host-output RAM.
 */
void gateway_ble_require_host_journal_restore(const char *reason);
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
int gateway_accept_collection_receipt_redrive(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan);
void gateway_ble_stream_get_status(struct gateway_ble_stream_diagnostics *diagnostics);
int gateway_observe_command_event(struct gateway_command_event *event,
                                  bool terminal);
int gateway_observe_command_event_if_available(
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
int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id);
int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms);
int gateway_begin_command_result_wait_until(
    const struct proto_packet *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms);
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
int gateway_get_registered_membership_roster_with_slots(
    uint64_t *node_ids,
    uint8_t *slots,
    size_t node_cap,
    size_t *node_count,
    uint16_t *membership_epoch);
bool gateway_assignment_publication_pending(void);
int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx);
void gateway_clear_registered_membership_roster(void);
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
