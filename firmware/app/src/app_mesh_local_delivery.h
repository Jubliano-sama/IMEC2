#ifndef APP_MESH_LOCAL_DELIVERY_H
#define APP_MESH_LOCAL_DELIVERY_H

#include "protocol.h"
#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION 2u
#define APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS 16u

enum app_mesh_local_delivery_state {
    APP_MESH_LOCAL_DELIVERY_EMPTY = 0,
    APP_MESH_LOCAL_DELIVERY_STAGED,
    APP_MESH_LOCAL_DELIVERY_STARTING,
    APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT,
    APP_MESH_LOCAL_DELIVERY_TRACKED,
    APP_MESH_LOCAL_DELIVERY_RETRY,
    APP_MESH_LOCAL_DELIVERY_PREEMPTED,
    APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED,
    APP_MESH_LOCAL_DELIVERY_FAILED,
    /* RAM-only sentinel: persistent custody exists but could not be read yet. */
    APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT,
    /* RAM-only: the persisted STARTING token was refused before RF. */
    APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE,
};

struct app_mesh_local_delivery_identity {
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint8_t msg_type;
};

struct app_mesh_local_delivery_snapshot {
    uint16_t version;
    uint16_t size;
    uint32_t checksum;
    uint32_t generation;
    uint16_t attempts_remaining;
    uint8_t state;
    uint8_t attempt_token;
    struct mesh_outbound outbound;
};

struct app_mesh_local_delivery_ops {
    int (*save)(void *ctx,
                const struct app_mesh_local_delivery_snapshot *snapshot);
    int (*clear)(void *ctx);
    void *ctx;
};

struct app_mesh_local_delivery {
    struct app_mesh_local_delivery_snapshot snapshot;
    struct app_mesh_local_delivery_ops ops;
};

struct app_mesh_local_delivery_recovery {
    bool restored;
    bool quarantined;
    bool retry_required;
    int source_error;
    int clear_error;
};

_Static_assert(sizeof(struct app_mesh_local_delivery_snapshot) <= 1536u,
               "local delivery journal exceeds the bounded stack/NVS budget");

void app_mesh_local_delivery_init(struct app_mesh_local_delivery *delivery,
                                  const struct app_mesh_local_delivery_ops *ops);
void app_mesh_local_delivery_identity_from_outbound(
    const struct mesh_outbound *outbound,
    struct app_mesh_local_delivery_identity *identity);
bool app_mesh_local_delivery_identity_matches(
    const struct app_mesh_local_delivery_identity *identity,
    const struct proto_packet *packet);
int app_mesh_local_delivery_stage(struct app_mesh_local_delivery *delivery,
                                  const struct mesh_outbound *outbound,
                                  uint32_t generation);
int app_mesh_local_delivery_restore(
    struct app_mesh_local_delivery *delivery,
    const struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_local_delivery_rebase_after_boot(
    struct app_mesh_local_delivery *delivery,
    uint32_t now_ms);
int app_mesh_local_delivery_recover(
    struct app_mesh_local_delivery *delivery,
    const struct app_mesh_local_delivery_snapshot *snapshot,
    int persistence_result,
    struct app_mesh_local_delivery_recovery *recovery);
int app_mesh_local_delivery_note_state(
    struct app_mesh_local_delivery *delivery,
    enum app_mesh_local_delivery_state state);
int app_mesh_local_delivery_note_tracked(
    struct app_mesh_local_delivery *delivery);
int app_mesh_local_delivery_begin_attempt(
    struct app_mesh_local_delivery *delivery,
    uint8_t *attempt_token);
int app_mesh_local_delivery_note_attempt_sent(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token);
int app_mesh_local_delivery_note_attempt_not_sent(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token,
    enum app_mesh_local_delivery_state state);
int app_mesh_local_delivery_note_attempt_released(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token,
    enum app_mesh_local_delivery_state state);
int app_mesh_local_delivery_note_attempt_blocked(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token);
int app_mesh_local_delivery_resume_blocked_attempt(
    struct app_mesh_local_delivery *delivery,
    uint8_t *attempt_token);
uint16_t app_mesh_local_delivery_attempts_available(
    const struct app_mesh_local_delivery *delivery);
int app_mesh_local_delivery_note_ack(
    struct app_mesh_local_delivery *delivery,
    const struct proto_packet *packet);
int app_mesh_local_delivery_note_failed(
    struct app_mesh_local_delivery *delivery);
int app_mesh_local_delivery_discard_failed(
    struct app_mesh_local_delivery *delivery);
bool app_mesh_local_delivery_active(const struct app_mesh_local_delivery *delivery);
const struct mesh_outbound *app_mesh_local_delivery_outbound(
    const struct app_mesh_local_delivery *delivery);
bool app_mesh_local_delivery_snapshot_valid(
    const struct app_mesh_local_delivery_snapshot *snapshot);

#endif
