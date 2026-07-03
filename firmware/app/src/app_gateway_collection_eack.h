#ifndef APP_GATEWAY_COLLECTION_EACK_H
#define APP_GATEWAY_COLLECTION_EACK_H

#include "app_gateway_eack_policy.h"
#include "gateway_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_GATEWAY_COLLECTION_EACK_RETURN_TARGET_CAP 2u

struct app_gateway_collection_eack_input {
    const struct gateway_collection_state *collection;
    const uint64_t *expected_node_ids;
    size_t expected_node_id_count;
    uint64_t previous_hop_id;
    uint8_t received_radio_channel;
    uint64_t self_id;
};

struct app_gateway_collection_eack_result {
    struct app_gateway_eack_policy_result policy;
    uint64_t current_channel9_next_hop_id;
    size_t return_target_count;
    uint32_t command_seq;
    uint16_t expected_count;
    uint16_t received_count;
    uint16_t missing_count;
    uint8_t eack_format;
    bool collection_open;
};

uint64_t app_gateway_collection_eack_current_channel9_return_hop(
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    uint64_t self_id);
int app_gateway_collection_eack_send(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_collection_eack_result *result);

#endif
