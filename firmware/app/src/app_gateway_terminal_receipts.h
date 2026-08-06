#ifndef APP_GATEWAY_TERMINAL_RECEIPTS_H
#define APP_GATEWAY_TERMINAL_RECEIPTS_H

#include "mesh_capacity.h"
#include "protocol.h"
#include "semantic_digest.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY \
    MESH_CONNECTED_REQUIRED_SOURCES
#define APP_GATEWAY_TERMINAL_RECEIPT_RECORD_SIZE 41u
#define APP_GATEWAY_TERMINAL_RECEIPT_RETENTION_MS UINT32_C(86400000)

/*
 * A terminal receipt is the gateway's compact durable proof that one
 * non-collection host record reached terminal BLE notification. The stored
 * SHA-256 semantic commitment covers message type, flags, endpoints,
 * session/sequence, payload length, and complete payload bytes.
 *
 * At most one receipt can be live per source because a source cannot emit its
 * next raw gateway-bound record until the prior raw ACK advances it to
 * ACK_CONFIRM. The fixed ledger covers all 50 anchors plus the required
 * 18-clicker fleet without allowing one source to consume another source's
 * recovery capacity.
 */
bool app_gateway_terminal_receipts_supports(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

/* Returns 1 for an exact retained terminal receipt, 0 when absent, or a
 * negative errno for corruption, a same-source conflicting record, capacity,
 * or storage failure. Expired receipts are deleted transactionally. */
int app_gateway_terminal_receipts_classify(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms);
/*
 * Match an already-validated durable semantic commitment. This narrow form
 * lets startup reconcile a NOTIFIED journal whose payload was already
 * deleted by a torn terminal clear; callers must obtain the digest from that
 * journal's validated metadata.
 */
int app_gateway_terminal_receipts_classify_identity(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t now_ms);

/* Insert the exact host-terminal packet after NOTIFIED and any collection
 * receipts are durable. Exact replay is idempotent. */
int app_gateway_terminal_receipts_record(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms);

/* Consume an exact retained receipt named by ACK_CONFIRM. A structurally
 * valid late/stale confirm is a safe zero and cannot mutate another source
 * receipt. */
int app_gateway_terminal_receipts_confirm(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len,
    uint32_t now_ms);

/* Explicit startup/readiness pass. Existing receipts conservatively receive
 * a fresh full raw-custody horizon after every gateway reboot. */
int app_gateway_terminal_receipts_restore(uint32_t now_ms);

#if defined(CONFIG_ZTEST)
void app_gateway_terminal_receipts_test_reset_runtime(void);
#endif

#endif
