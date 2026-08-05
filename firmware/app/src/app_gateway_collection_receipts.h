#ifndef APP_GATEWAY_COLLECTION_RECEIPTS_H
#define APP_GATEWAY_COLLECTION_RECEIPTS_H

#include "protocol.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES 50u
#define APP_GATEWAY_COLLECTION_RECEIPT_RECORD_SIZE 76u

/*
 * The receipt is the latest terminal collection result for one node.  The
 * payload length and full SHA-256 commitment make different immutable bytes a
 * conflict rather than an idempotent retry. The wire CRC remains a corruption
 * check only and is never terminal authority.
 */
struct app_gateway_collection_receipt {
    struct command_result_id result_id;
    uint32_t collection_epoch_id;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t payload_len;
};

bool app_gateway_collection_receipt_valid(
    const struct app_gateway_collection_receipt *receipt);
bool app_gateway_collection_receipt_same_result(
    const struct app_gateway_collection_receipt *left,
    const struct app_gateway_collection_receipt *right);
bool app_gateway_collection_receipt_equal(
    const struct app_gateway_collection_receipt *left,
    const struct app_gateway_collection_receipt *right);

/*
 * Scan the NVS-only store for node_id. Returns 1 with the exact receipt, 0
 * when absent, or a negative errno. Any corrupt slot, duplicate receipt for
 * the requested node, or transient slot read fails the lookup closed.
 */
int app_gateway_collection_receipts_lookup(
    uint64_t node_id,
    struct app_gateway_collection_receipt *receipt);

/*
 * Insert or advance one node's durable receipt.
 *
 * A NULL superseded receipt permits only an empty-slot insert or an exact
 * idempotent replay. Replacing an existing receipt requires its exact prior
 * value and an RFC 1982-strictly-newer persistent command sequence for the
 * same gateway and node. This makes the caller state the proof supplied by
 * acceptance of that node's later single-outbox result; unrelated, stale,
 * same-sequence-conflicting, and half-range-ambiguous callers cannot
 * overwrite terminal receipt history.
 *
 * A write that became durable but returned an uncertain error is safe to
 * retry: finding the exact new receipt is success.
 */
int app_gateway_collection_receipts_record(
    const struct app_gateway_collection_receipt *receipt,
    const struct app_gateway_collection_receipt *superseded);

/*
 * Commit terminal proof from the exact immutable host-journal record after
 * its HOST_NOTIFIED phase is durable. A zero bundle mask selects every record;
 * a nonzero mask selects the corresponding wire-record indices. Ordinary
 * non-collection messages and command results are a successful no-op.
 *
 * Bundle records are fully validated before the first receipt write. A
 * persistence failure can still leave a selected prefix durable, but retrying
 * the same host-journal owner is idempotent and completes the suffix.
 */
int app_gateway_collection_receipts_record_host_notification(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t host_projection_mask);

/*
 * Classify a valid collection result retry without mutating persistence.
 * Returns 1 only when every represented result has exact or strictly newer
 * durable per-node proof, 0 when none has proof, or a negative errno for a
 * malformed packet, corrupt/conflicting receipt, RFC 1982 ambiguity, or a
 * partially proven bundle.
 */
int app_gateway_collection_receipts_classify_retry(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

#endif
