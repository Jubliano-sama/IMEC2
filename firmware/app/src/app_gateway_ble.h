#ifndef APP_GATEWAY_BLE_H
#define APP_GATEWAY_BLE_H

#include "gateway_command.h"
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

int gateway_ble_init(void);
#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
void gateway_ble_connectivity_test_run(void);
#endif
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
void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason);
int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id);
int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms);
void gateway_clear_pending_command_result(const struct proto_packet *command);
int gateway_begin_command_collection(const struct gateway_command_options *options);
void gateway_clear_command_collection(const struct gateway_command_options *options);
int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             size_t node_count);
void gateway_clear_registered_membership_roster(void);
void gateway_note_command_result(const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 uint64_t previous_hop_id,
                                 uint8_t received_radio_channel,
                                 const struct mesh_event_plan *current_channel9_plan);
void gateway_note_command_result_bundle(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint64_t previous_hop_id,
                                        uint8_t received_radio_channel,
                                        const struct mesh_event_plan *current_channel9_plan);
void gateway_command_result_tracking_init(void);

#endif
