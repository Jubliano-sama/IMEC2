#ifndef APP_GATEWAY_COLLECTION_RECOVERY_H
#define APP_GATEWAY_COLLECTION_RECOVERY_H

#include "app_mesh_flood.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The owner first reserves one exact stale packet while its BLE host record
 * is retained, then (only after that record's exact receipt) freezes the
 * bounded C5 recovery flood.  It deliberately has no durable representation:
 * a reboot at either boundary leaves the source packet retryable, which is the
 * only safe recovery point when the raw collection ledger is RAM-only.
 */
struct app_gateway_collection_recovery {
    struct proto_packet eack_packet;
    struct gateway_collection_recovery_identity identity;
    struct app_mesh_flood_progress flood_progress;
    uint8_t eack_payload[PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN];
    uint16_t eack_payload_len;
    /* identity is populated with attempt zero while the BLE host record is
     * retained.  A nonzero attempt exists only after exact host receipt. */
    bool host_custody_pending;
    bool active;
};

_Static_assert(PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN <=
                   PROTO_GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN,
               "recovery EACK must fit the normal EACK payload envelope");
_Static_assert(sizeof(struct app_gateway_collection_recovery) <= 256u,
               "RAM-only recovery owner exceeded its bounded gateway budget");

/*
 * Validate the immutable collection identity carried by a result or bundle.
 * Callers must validate the complete raw result/bundle before this function;
 * this check binds the recovery EACK to the packet envelope and its semantic
 * collection identity rather than reparsing every bundle record.
 */
int app_gateway_collection_recovery_preflight(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch);

/* Reserve the collection lane after a BLE stream reservation but before that
 * record becomes visible to the host.  No transport attempt is allocated at
 * this stage.  The exact reservation blocks a fresh collection until it is
 * cancelled on failed stream commit or completed after host retirement. */
int app_gateway_collection_recovery_reserve_host_custody(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch);

int app_gateway_collection_recovery_cancel_host_custody(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

/*
 * Freeze one CLOSED 1-of-1 recovery EACK. recovery_attempt_id is allocated by
 * the shared gateway control-sequence owner immediately after host receipt.
 * Repeating begin for the same immutable source packet is idempotent; a
 * different packet is rejected while the previous flood remains owned.
 */
int app_gateway_collection_recovery_begin(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch,
    uint32_t recovery_attempt_id);

/* Reconstruct the frozen outbound EACK without allocating a new identity. */
int app_gateway_collection_recovery_outbound(
    const struct app_gateway_collection_recovery *state,
    struct mesh_outbound *outbound);

/* Match the one immutable C5 EACK frozen by this RAM-only owner.  This is
 * deliberately narrower than the source-packet matcher: coordinator escape
 * hatches may authorize only this exact outbound while its GUI receipt is
 * being finalized. */
bool app_gateway_collection_recovery_outbound_matches(
    const struct app_gateway_collection_recovery *state,
    const struct mesh_outbound *outbound);

bool app_gateway_collection_recovery_matches(
    const struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

/* Retire an exact recovery owner only after the retained BLE record itself is
 * retired.  A completed flood remains an active collection barrier until this
 * call succeeds. */
int app_gateway_collection_recovery_finish_host_delivery(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

void app_gateway_collection_recovery_reset(
    struct app_gateway_collection_recovery *state);

#endif
