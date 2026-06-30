#ifndef APP_MESH_TEST_H
#define APP_MESH_TEST_H

#include "protocol.h"

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

#endif
