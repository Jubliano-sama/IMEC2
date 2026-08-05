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
    const struct mesh_event_plan *current_channel9_plan;
    uint64_t self_id;
    const uint64_t *excluded_channel9_next_hop_ids;
    size_t excluded_channel9_next_hop_count;
    bool force_c5_recovery;
    bool use_prebuilt_eack;
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

struct app_gateway_collection_recovery_eack_input {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint64_t packet_src_id;
    uint32_t recovery_attempt_id;
    uint16_t packet_seq;
    uint16_t payload_len;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
};

uint64_t app_gateway_collection_eack_current_channel9_return_hop(
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t self_id);
int app_gateway_collection_eack_prepare(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    struct app_gateway_collection_eack_result *result);
/*
 * Builds a compact CLOSED EACK for a collection whose per-node host receipt
 * outlived the volatile collection transaction. The recovery-attempt TLV is
 * the fresh flood/dedup identity; the source, packet sequence, payload length,
 * and SHA-256 commitment bind terminal authority to exactly one immutable
 * redriven COMMAND_RESULT or RESULT_BUNDLE.
 */
int app_gateway_collection_recovery_eack_prepare(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_recovery_eack_input *input);
int app_gateway_collection_eack_send(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_collection_eack_result *result);

#endif
