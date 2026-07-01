#ifndef GATEWAY_COMMAND_H
#define GATEWAY_COMMAND_H

#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_COMMAND_RESULT_TIMEOUT_MS 12000u
#define GATEWAY_COLLECTION_RESULT_CACHE_SIZE 64u

struct gateway_command_pending {
    struct proto_packet command;
    enum command_id command_id;
    uint32_t deadline_ms;
    bool active;
};

struct gateway_command_options {
    enum command_scope scope;
    enum command_response_mode response_mode;
    uint32_t command_seq;
    uint32_t flood_epoch_id;
    uint32_t collection_epoch_id;
    uint32_t collection_slot_seed;
    uint32_t execute_delay_ms;
    uint32_t command_expiry_s;
    uint16_t membership_epoch;
    uint16_t expected_node_count;
    bool collection_required;
    bool flood_required;
};

struct gateway_collection_result_entry {
    struct command_result_id id;
    uint16_t payload_crc;
    uint16_t payload_len;
    bool valid;
};

struct gateway_collection_state {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t received_count;
    uint8_t retry_round;
    uint32_t next_retry_spread_ms;
    bool collection_open;
    struct gateway_collection_result_entry results[GATEWAY_COLLECTION_RESULT_CACHE_SIZE];
};

int gateway_command_extract_id(const uint8_t *payload,
                               size_t payload_len,
                               enum command_id *command_id);
int gateway_command_extract_options(const uint8_t *payload,
                                    size_t payload_len,
                                    struct gateway_command_options *options);
uint32_t gateway_command_collection_spread_ms(uint16_t expected_node_count);
uint32_t gateway_command_collection_initial_due_ms(uint32_t command_flood_end_ms,
                                                  uint64_t node_id,
                                                  uint32_t command_seq,
                                                  uint32_t collection_slot_seed,
                                                  uint16_t expected_node_count);
int gateway_command_extract_role(const uint8_t *payload,
                                 size_t payload_len,
                                 enum device_role *role);
int gateway_command_extract_duration_ms(const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t default_duration_ms,
                                        uint32_t *duration_ms);
int gateway_command_prepare_outbound(const struct proto_packet *host_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t gateway_id,
                                     uint32_t now_ms,
                                     uint16_t fallback_seq,
                                     struct mesh_outbound *out,
                                     enum command_id *command_id);
int gateway_command_build_failure_result(const struct proto_packet *command,
                                         uint64_t gateway_id,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason,
                                         uint32_t now_ms,
                                         struct proto_packet *result,
                                         uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *payload_len);
void gateway_command_pending_clear(struct gateway_command_pending *pending);
int gateway_command_pending_start(struct gateway_command_pending *pending,
                                  const struct proto_packet *command,
                                  enum command_id command_id,
                                  uint32_t now_ms,
                                  uint32_t timeout_ms);
bool gateway_command_pending_matches_result(const struct gateway_command_pending *pending,
                                            const struct proto_packet *result);
bool gateway_command_pending_complete_result(struct gateway_command_pending *pending,
                                             const struct proto_packet *result);
bool gateway_command_pending_expired(struct gateway_command_pending *pending,
                                     uint32_t now_ms,
                                     struct proto_packet *command,
                                     enum command_id *command_id);
void gateway_collection_clear(struct gateway_collection_state *collection);
int gateway_collection_start(struct gateway_collection_state *collection,
                             uint64_t gateway_id,
                             uint16_t gateway_epoch,
                             uint32_t command_seq,
                             uint32_t collection_epoch_id,
                             uint16_t membership_epoch,
                             uint16_t expected_count,
                             uint8_t retry_round,
                             uint32_t next_retry_spread_ms);
int gateway_collection_record_result(struct gateway_collection_state *collection,
                                     const struct proto_packet *result,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     bool *duplicate);
int gateway_collection_record_bundle(struct gateway_collection_state *collection,
                                     const struct proto_packet *bundle_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint16_t *accepted_count,
                                     uint16_t *duplicate_count);
bool gateway_collection_contains_result(const struct gateway_collection_state *collection,
                                        const struct command_result_id *id);
int gateway_collection_build_eack(const struct gateway_collection_state *collection,
                                  uint8_t eack_format,
                                  struct gateway_collection_eack *eack);
int gateway_collection_prepare_eack_outbound(const struct gateway_collection_state *collection,
                                             uint8_t eack_format,
                                             struct mesh_outbound *out);

#ifdef __cplusplus
}
#endif

#endif
