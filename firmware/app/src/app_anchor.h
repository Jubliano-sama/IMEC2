#ifndef APP_ANCHOR_H
#define APP_ANCHOR_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_report_callbacks;

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void);
int app_anchor_init(void);
int app_anchor_start_anchor_role(void);
int app_anchor_start_gateway_role(void);
int gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t previous_hop_id);
bool gateway_survey_auto_preflight_result(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len);
bool gateway_survey_auto_owns_pending_command(
    const struct proto_packet *command,
    enum command_id command_id);
#if defined(CONFIG_IMEC_GATEWAY_BLE)
void gateway_handle_ble_frame(const uint8_t *frame, size_t frame_len);
#endif

#endif
