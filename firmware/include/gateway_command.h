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
#define GATEWAY_COMMAND_BUDGET_MIN_MS 1000u
#define GATEWAY_COMMAND_BUDGET_MAX_MS 600000u
#define GATEWAY_COMMAND_BUDGET_RETRY_QUANTUM_MS 30000u
#define GATEWAY_COLLECTION_RESULT_CACHE_SIZE 50u
#define GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP GATEWAY_COLLECTION_RESULT_CACHE_SIZE
#define GATEWAY_COMMAND_RX_DUP_CACHE_SIZE 4u
#define GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION 4u
#define GATEWAY_COLLECTION_STATE_PERSISTENCE_VERSION 1u
#define GATEWAY_COLLECTION_RESULT_ENTRY_SIZE 32u
#define GATEWAY_COLLECTION_RESULT_SNAPSHOT_ENTRY_SIZE 48u
#define GATEWAY_COLLECTION_STATE_SIZE 2048u
#define GATEWAY_COLLECTION_STATE_SNAPSHOT_SIZE 2848u
#define GATEWAY_COLLECTION_EACK_FIXED_PAYLOAD_LEN \
    (PROTO_TLV_U64_ENCODED_LEN + \
     (5u * PROTO_TLV_U16_ENCODED_LEN) + \
     (3u * PROTO_TLV_U32_ENCODED_LEN) + \
     (3u * PROTO_TLV_U8_ENCODED_LEN))
#define GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN \
    (GATEWAY_COLLECTION_EACK_FIXED_PAYLOAD_LEN + \
     (GATEWAY_COLLECTION_RESULT_CACHE_SIZE * PROTO_TLV_U64_ENCODED_LEN))
#define GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_VERSION 1u
#define GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_SIZE 608u

struct gateway_membership_roster;

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
    uint16_t expected_node_id_count;
    uint64_t expected_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];
    bool collection_required;
    bool flood_required;
};

struct gateway_command_rx_duplicate_entry {
    uint32_t command_seq;
    uint32_t stored_at_ms;
    uint32_t lifetime_ms;
    bool valid;
};

struct gateway_command_rx_duplicate_cache {
    struct gateway_command_rx_duplicate_entry entries[GATEWAY_COMMAND_RX_DUP_CACHE_SIZE];
    uint8_t next;
};

enum gateway_command_tracking_mode {
    GATEWAY_COMMAND_TRACK_NONE = 0,
    GATEWAY_COMMAND_TRACK_LEGACY_RESULT = 1,
    GATEWAY_COMMAND_TRACK_COLLECTION = 2,
};

enum gateway_command_result_admission {
    GATEWAY_COMMAND_RESULT_IGNORE = 0,
    GATEWAY_COMMAND_RESULT_WAIT,
    GATEWAY_COMMAND_RESULT_ACCEPT,
};

enum gateway_command_transport_mode {
    GATEWAY_COMMAND_TRANSPORT_UNICAST_TRACKED = 0,
    GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST = 1,
};

enum gateway_command_collection_roster_source {
    GATEWAY_COMMAND_COLLECTION_ROSTER_NONE = 0,
    GATEWAY_COMMAND_COLLECTION_ROSTER_EXPLICIT = 1,
    GATEWAY_COMMAND_COLLECTION_ROSTER_MEMBERSHIP = 2,
};

struct gateway_collection_result_id {
    uint64_t node_id;
    uint32_t node_boot_counter;
    uint16_t result_seq;
};

struct gateway_collection_result_entry {
    struct gateway_collection_result_id id;
    uint64_t previous_hop_id;
    uint16_t payload_crc;
    uint16_t payload_len;
    bool valid;
};

struct gateway_collection_result_snapshot_entry {
    struct command_result_id id;
    uint64_t previous_hop_id;
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
    uint16_t eack_sequence;
    uint32_t next_retry_spread_ms;
    bool collection_open;
    bool eack_pending;
    uint16_t expected_node_id_count;
    struct gateway_collection_result_entry results[GATEWAY_COLLECTION_RESULT_CACHE_SIZE];
    uint64_t expected_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];
    uint8_t persistence_version;
    bool persistence_valid;
};

struct gateway_collection_state_snapshot {
    uint8_t version;
    bool valid;
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t received_count;
    uint16_t result_count;
    uint8_t retry_round;
    uint16_t eack_sequence;
    uint32_t next_retry_spread_ms;
    bool collection_open;
    bool eack_pending;
    uint16_t expected_node_id_count;
    struct gateway_collection_result_snapshot_entry
        results[GATEWAY_COLLECTION_RESULT_CACHE_SIZE];
    uint64_t expected_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];
};

struct gateway_collection_eack_custody_snapshot {
    struct proto_packet packet;
    uint8_t payload[GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN];
    uint8_t version;
    uint16_t payload_len;
    uint16_t payload_crc;
    bool valid;
};

_Static_assert(sizeof(struct gateway_collection_result_entry) ==
               GATEWAY_COLLECTION_RESULT_ENTRY_SIZE,
               "live gateway collection entries must remain compact");
_Static_assert(sizeof(struct gateway_collection_state) == GATEWAY_COLLECTION_STATE_SIZE,
               "live gateway collection state must retain its RAM budget");
_Static_assert(offsetof(struct gateway_collection_state, expected_node_id_count) == 38u &&
               offsetof(struct gateway_collection_state, results) == 40u &&
               offsetof(struct gateway_collection_state, expected_node_ids) == 1640u &&
               offsetof(struct gateway_collection_state, persistence_version) == 2040u &&
               offsetof(struct gateway_collection_state, persistence_valid) == 2041u,
               "live gateway collection roster layout must remain bounded");
_Static_assert(sizeof(struct gateway_collection_result_snapshot_entry) ==
               GATEWAY_COLLECTION_RESULT_SNAPSHOT_ENTRY_SIZE,
               "gateway collection result snapshot layout is persistent");
_Static_assert(sizeof(struct command_result_id) == 32u &&
               offsetof(struct command_result_id, gateway_id) == 0u &&
               offsetof(struct command_result_id, gateway_epoch) == 8u &&
               offsetof(struct command_result_id, command_seq) == 12u &&
               offsetof(struct command_result_id, node_id) == 16u &&
               offsetof(struct command_result_id, node_boot_counter) == 24u &&
               offsetof(struct command_result_id, result_seq) == 28u,
               "gateway collection snapshot identity layout is persistent");
_Static_assert(offsetof(struct gateway_collection_result_snapshot_entry, id) == 0u &&
               offsetof(struct gateway_collection_result_snapshot_entry,
                        previous_hop_id) == 32u &&
               offsetof(struct gateway_collection_result_snapshot_entry, payload_crc) == 40u &&
               offsetof(struct gateway_collection_result_snapshot_entry, payload_len) == 42u &&
               offsetof(struct gateway_collection_result_snapshot_entry, valid) == 44u,
               "gateway collection result snapshot offsets are persistent");
_Static_assert(sizeof(struct gateway_collection_state_snapshot) ==
               GATEWAY_COLLECTION_STATE_SNAPSHOT_SIZE,
               "gateway collection snapshot layout is persistent");
_Static_assert(offsetof(struct gateway_collection_state_snapshot, version) == 0u &&
               offsetof(struct gateway_collection_state_snapshot, valid) == 1u &&
               offsetof(struct gateway_collection_state_snapshot, gateway_id) == 8u &&
               offsetof(struct gateway_collection_state_snapshot, gateway_epoch) == 16u &&
               offsetof(struct gateway_collection_state_snapshot, command_seq) == 20u &&
               offsetof(struct gateway_collection_state_snapshot, collection_epoch_id) == 24u &&
               offsetof(struct gateway_collection_state_snapshot, membership_epoch) == 28u &&
               offsetof(struct gateway_collection_state_snapshot, expected_count) == 30u &&
               offsetof(struct gateway_collection_state_snapshot, received_count) == 32u &&
               offsetof(struct gateway_collection_state_snapshot, result_count) == 34u &&
               offsetof(struct gateway_collection_state_snapshot, retry_round) == 36u &&
               offsetof(struct gateway_collection_state_snapshot, eack_sequence) == 38u &&
               offsetof(struct gateway_collection_state_snapshot, next_retry_spread_ms) == 40u &&
               offsetof(struct gateway_collection_state_snapshot, collection_open) == 44u &&
               offsetof(struct gateway_collection_state_snapshot, eack_pending) == 45u &&
               offsetof(struct gateway_collection_state_snapshot,
                        expected_node_id_count) == 46u &&
               offsetof(struct gateway_collection_state_snapshot, results) == 48u &&
               offsetof(struct gateway_collection_state_snapshot,
                        expected_node_ids) == 2448u,
               "gateway collection snapshot offsets are persistent");
_Static_assert(GATEWAY_COLLECTION_EACK_FIXED_PAYLOAD_LEN == 57u,
               "collection EACK fixed TLVs must retain their wire budget");
_Static_assert(GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN == 557u &&
               GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN <= PACKET_EXT_MAX_PAYLOAD_LEN,
               "a maximum collection EACK must fit one extended packet");
_Static_assert(sizeof(struct gateway_collection_eack_custody_snapshot) ==
               GATEWAY_COLLECTION_EACK_CUSTODY_SNAPSHOT_SIZE,
               "persisted collection EACK custody must remain compact");
_Static_assert(offsetof(struct gateway_collection_eack_custody_snapshot, packet) == 0u &&
               offsetof(struct gateway_collection_eack_custody_snapshot, payload) == 40u &&
               offsetof(struct gateway_collection_eack_custody_snapshot, version) == 597u &&
               offsetof(struct gateway_collection_eack_custody_snapshot, payload_len) == 598u &&
               offsetof(struct gateway_collection_eack_custody_snapshot, payload_crc) == 600u &&
               offsetof(struct gateway_collection_eack_custody_snapshot, valid) == 602u,
               "persisted collection EACK custody offsets are stable");

int gateway_command_extract_id(const uint8_t *payload,
                               size_t payload_len,
                               enum command_id *command_id);
int gateway_command_extract_options(const uint8_t *payload,
                                    size_t payload_len,
                                    struct gateway_command_options *options);
enum gateway_command_tracking_mode gateway_command_tracking_mode_from_options(
    const struct gateway_command_options *options);
enum gateway_command_transport_mode gateway_command_transport_mode_from_outbound(
    const struct mesh_outbound *out);
int gateway_command_resolve_collection_roster(
    const struct gateway_command_options *options,
    const struct gateway_membership_roster *membership_roster,
    uint64_t *out_node_ids,
    size_t out_cap,
    size_t *out_count,
    enum gateway_command_collection_roster_source *source);
bool gateway_command_receive_expired(const struct proto_packet *packet,
                                     const struct gateway_command_options *options);
uint32_t gateway_command_expiry_remaining_ms(const struct proto_packet *packet,
                                             const struct gateway_command_options *options);
uint32_t gateway_command_execute_delay_remaining_ms(
    const struct proto_packet *packet,
    const struct gateway_command_options *options);
bool gateway_command_rx_duplicate_seen(struct gateway_command_rx_duplicate_cache *cache,
                                       uint32_t command_seq,
                                       uint32_t now_ms);
void gateway_command_rx_duplicate_store(struct gateway_command_rx_duplicate_cache *cache,
                                        const struct proto_packet *packet,
                                        const struct gateway_command_options *options,
                                        uint32_t now_ms);
uint32_t gateway_command_collection_spread_ms(uint16_t expected_node_count);
uint32_t gateway_command_collection_initial_due_ms(uint32_t command_flood_end_ms,
                                                  uint64_t node_id,
                                                  uint32_t command_seq,
                                                  uint32_t collection_slot_seed,
                                                  uint16_t expected_node_count);
uint32_t gateway_command_collection_retry_spread_ms(uint8_t retry_round);
int gateway_command_append_collection_result_identity(uint8_t *payload,
                                                      size_t payload_cap,
                                                      size_t *payload_len,
                                                      const struct command_result_id *id,
                                                      uint32_t collection_epoch_id);
int gateway_command_extract_role(const uint8_t *payload,
                                 size_t payload_len,
                                 enum device_role *role);
int gateway_command_extract_duration_ms(const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t default_duration_ms,
                                        uint32_t *duration_ms);
int gateway_command_extract_budget_ms(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t default_budget_ms,
                                      uint32_t *budget_ms,
                                      bool *explicit_budget);
uint32_t gateway_command_budget_window_ms(bool explicit_budget,
                                          uint32_t remaining_ms,
                                          uint8_t phases_remaining,
                                          uint32_t natural_window_ms);
uint32_t gateway_command_budget_weighted_window_ms(bool explicit_budget,
                                                   uint32_t remaining_ms,
                                                   uint8_t phase_weight,
                                                   uint8_t total_weight,
                                                   uint32_t natural_window_ms);
uint8_t gateway_command_budget_retry_limit(bool explicit_budget,
                                           uint32_t budget_ms,
                                           uint8_t default_limit);
int gateway_command_prepare_outbound(const struct proto_packet *host_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint64_t gateway_id,
                                     uint32_t now_ms,
                                     uint16_t fallback_seq,
                                     struct mesh_outbound *out,
                                     enum command_id *command_id);
int gateway_command_append_default_flood_controls(struct mesh_outbound *out);
int gateway_command_build_result(const struct proto_packet *command,
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
enum gateway_command_result_admission gateway_command_result_admit(
    const struct gateway_command_pending *pending,
    const struct proto_packet *result,
    bool transaction_owned,
    bool transaction_recognized);
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
int gateway_collection_set_expected_roster(
    struct gateway_collection_state *collection,
    const uint64_t *expected_node_ids,
    size_t expected_node_id_count);
int gateway_collection_record_result(struct gateway_collection_state *collection,
                                     const struct proto_packet *result,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     bool *duplicate);
int gateway_collection_record_result_from_hop(struct gateway_collection_state *collection,
                                             const struct proto_packet *result,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             bool *duplicate);
int gateway_collection_record_bundle(struct gateway_collection_state *collection,
                                     const struct proto_packet *bundle_packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint16_t *accepted_count,
                                     uint16_t *duplicate_count);
int gateway_collection_record_bundle_from_hop(struct gateway_collection_state *collection,
                                             const struct proto_packet *bundle_packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id,
                                             uint16_t *accepted_count,
                                             uint16_t *duplicate_count);
bool gateway_collection_contains_result(const struct gateway_collection_state *collection,
                                        const struct command_result_id *id);
size_t gateway_collection_return_candidates(const struct gateway_collection_state *collection,
                                            uint64_t *out,
                                            size_t out_cap);
int gateway_collection_export_snapshot(
    const struct gateway_collection_state *collection,
    struct gateway_collection_state_snapshot *snapshot);
int gateway_collection_restore_snapshot(
    struct gateway_collection_state *collection,
    const struct gateway_collection_state_snapshot *snapshot);
int gateway_collection_state_validate(
    const struct gateway_collection_state *collection);
int gateway_collection_build_eack(const struct gateway_collection_state *collection,
                                  uint8_t eack_format,
                                  struct gateway_collection_eack *eack);
int gateway_collection_prepare_eack_outbound(const struct gateway_collection_state *collection,
                                             uint8_t eack_format,
                                             struct mesh_outbound *out);
int gateway_collection_prepare_missing_eack_outbound(
    const struct gateway_collection_state *collection,
    const uint64_t *expected_node_ids,
    size_t expected_node_count,
    struct mesh_outbound *out,
    uint16_t *missing_count);
int gateway_collection_eack_custody_capture(
    struct gateway_collection_eack_custody_snapshot *snapshot,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int gateway_collection_eack_custody_validate(
    const struct gateway_collection_eack_custody_snapshot *snapshot);
int gateway_collection_advance_retry_round(struct gateway_collection_state *collection);

#ifdef __cplusplus
}
#endif

#endif
