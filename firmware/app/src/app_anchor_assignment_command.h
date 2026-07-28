#ifndef APP_ANCHOR_ASSIGNMENT_COMMAND_H
#define APP_ANCHOR_ASSIGNMENT_COMMAND_H

#include "discovery_assignment.h"

#include <stdint.h>

struct mesh_outbound;

struct app_anchor_assignment_command_params {
    enum discovery_assignment_phase phase;
    uint32_t epoch;
    uint32_t command_seq;
    uint32_t operation_budget_ms;
    uint32_t response_spread_ms;
    uint64_t source_id;
    uint16_t expected_anchor_count;
    uint32_t command_expiry_s;
    uint8_t ttl;
    uint8_t radio_channel;
};

int app_anchor_assignment_command_prepare(
    struct mesh_outbound *outbound,
    const struct app_anchor_assignment_command_params *params);

#endif
