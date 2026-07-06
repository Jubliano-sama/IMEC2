#ifndef APP_MESH_TEST_H
#define APP_MESH_TEST_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

int app_mesh_test_init(void);
int app_mesh_test_start(void);
void app_mesh_test_note_wake_event(const struct proto_packet *packet,
                                   uint64_t previous_hop_id,
                                   uint8_t link_quality,
                                   uint8_t radio_channel);
void app_mesh_test_note_wake_claim(uint64_t source_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint8_t link_quality);
void app_mesh_test_note_ch9_missed(void);
void app_mesh_test_note_report_tx_retryable(uint16_t seq, int ret);
void app_mesh_test_note_report_tx_backoff(uint16_t seq, int ret, uint32_t delay_ms);
void app_mesh_test_note_direct_gateway_ack(uint16_t seq, int ret, uint32_t queue_depth);
void app_mesh_test_note_gateway_delivery(const struct proto_packet *packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint32_t received_at_ms,
                                         uint32_t queue_depth);

#endif
