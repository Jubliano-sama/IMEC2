#ifndef APP_GATEWAY_EACK_RETRY_H
#define APP_GATEWAY_EACK_RETRY_H

#include "app_mesh_rf_retry.h"
#include "app_mesh_flood.h"
#include "gateway_command.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_GATEWAY_EACK_RETRY_FAILED_HOP_CAP 2u
#define APP_GATEWAY_EACK_FIXED_PAYLOAD_LEN \
    GATEWAY_COLLECTION_EACK_FIXED_PAYLOAD_LEN
#define APP_GATEWAY_EACK_NODE_ID_PAYLOAD_LEN PROTO_TLV_U64_ENCODED_LEN
#define APP_GATEWAY_EACK_SNAPSHOT_MAX_PAYLOAD_LEN \
    GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN

struct app_gateway_eack_retry_identity {
    uint64_t gateway_id;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint8_t eack_round;
    uint16_t eack_sequence;
};

struct app_gateway_eack_retry_state {
    struct app_mesh_rf_retry_state rf_retry;
    struct app_mesh_flood_progress c5_flood_progress;
    struct app_gateway_eack_retry_identity identity;
    struct gateway_collection_eack_custody_snapshot snapshot;
    uint64_t failed_channel9_next_hop_ids[APP_GATEWAY_EACK_RETRY_FAILED_HOP_CAP];
    uint8_t failed_channel9_next_hop_count;
    bool force_c5_recovery;
    bool active;
};

_Static_assert(APP_GATEWAY_EACK_FIXED_PAYLOAD_LEN == 57u,
               "collection EACK fixed TLVs must retain the compact snapshot budget");
_Static_assert(APP_GATEWAY_EACK_SNAPSHOT_MAX_PAYLOAD_LEN == 557u,
               "50-node collection EACKs must fit the compact snapshot budget");
_Static_assert(APP_GATEWAY_EACK_SNAPSHOT_MAX_PAYLOAD_LEN <=
               PACKET_EXT_MAX_PAYLOAD_LEN,
               "the frozen collection EACK must fit one protocol packet");
_Static_assert(sizeof(struct gateway_collection_eack_custody_snapshot) <= 608u,
               "collection EACK retry custody must remain RAM-bounded");

uint32_t app_gateway_eack_retry_note_failure(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    uint32_t fresh_entropy);
int app_gateway_eack_retry_freeze(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    const struct mesh_outbound *eack);
int app_gateway_eack_retry_restore(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    struct mesh_outbound *eack);
int app_gateway_eack_retry_export_custody(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    struct gateway_collection_eack_custody_snapshot *snapshot);
int app_gateway_eack_retry_import_custody(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    const struct gateway_collection_eack_custody_snapshot *snapshot);
bool app_gateway_eack_retry_snapshot_active(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection);
void app_gateway_eack_retry_note_failed_channel9_target(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    uint64_t next_hop_id);
void app_gateway_eack_retry_note_success(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection);
int app_gateway_eack_retry_commit_success(
    struct app_gateway_eack_retry_state *state);
void app_gateway_eack_retry_reset(
    struct app_gateway_eack_retry_state *state);

#endif
