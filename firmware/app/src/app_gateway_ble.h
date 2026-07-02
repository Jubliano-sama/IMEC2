#ifndef APP_GATEWAY_BLE_H
#define APP_GATEWAY_BLE_H

#include "gateway_command.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gateway_ble_status {
    bool connected;
    bool packet_notify_enabled;
    bool log_notify_enabled;
};

int gateway_ble_init(void);
#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
void gateway_ble_connectivity_test_run(void);
#endif
int gateway_ble_send_packet_frame(const uint8_t *frame, size_t frame_len);
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
uint16_t gateway_next_command_seq(void);
void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason);
int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id);
void gateway_clear_pending_command_result(const struct proto_packet *command);
int gateway_begin_command_collection(const struct gateway_command_options *options);
void gateway_clear_command_collection(const struct gateway_command_options *options);
void gateway_note_command_result(const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 uint64_t previous_hop_id);
void gateway_note_command_result_bundle(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint64_t previous_hop_id);
void gateway_command_result_tracking_init(void);

#endif
