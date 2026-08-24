#ifndef APP_GATEWAY_CONTROL_SEQUENCE_H
#define APP_GATEWAY_CONTROL_SEQUENCE_H

#include "app_durable_state.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Gateway control identities share one durable, non-wrapping namespace.
 * The active and prefetched standby blocks are RAM-only; admission must never
 * perform storage I/O.  A power loss abandons both blocks, so the next boot
 * deliberately reserves a fresh block before it admits control work.
 */
#define APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE \
    APP_DURABLE_STATE_COMMAND_BLOCK_SIZE
#define APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR UINT32_C(2048)
#define APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET UINT32_C(1024)
#define APP_GATEWAY_CONTROL_SEQUENCE_FORCED_ROUTE_REFRESH_BUDGET UINT32_C(64)
#define APP_GATEWAY_CONTROL_SEQUENCE_GENERIC_FLOOD_BUDGET UINT32_C(4)
#define APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS \
    (UINT32_C(24) * UINT32_C(60) * UINT32_C(60) * UINT32_C(1000))
#define APP_GATEWAY_CONTROL_SEQUENCE_MIN_DAILY_LIFETIME_DAYS \
    (UINT32_C(44) * UINT32_C(365))

/*
 * Reserve the first active block for this boot.  This is the sole allowed
 * startup reservation; subsequent blocks are attempted only by maintenance.
 */
int app_gateway_control_sequence_init(void);

/*
 * Return one already-reserved identity.  This path is RAM-only and fails
 * closed only when proven reserved capacity is exhausted.  Callers that
 * admitted work through app_gateway_control_sequence_admission_available()
 * may consume the protected runway through this API; the floor blocks new
 * admission, not already committed terminal work.
 */
int app_gateway_control_sequence_next(uint32_t *sequence);

/*
 * Return one already-reserved control identity whose low 16-bit projection is
 * nonzero. Gateway host receipts carry that projection in proto_packet.seq;
 * a low-word-zero identity is deliberately consumed in RAM rather than
 * exposing a publisher event the host cannot receipt. It never reserves an
 * additional durable block.
 */
int app_gateway_control_sequence_next_receiptable(uint32_t *sequence);

/*
 * True only when proven active plus standby capacity can cover requested_ids
 * while retaining the protected floor.  A refill in flight is not capacity
 * until its durable write/readback has completed and it becomes standby.
 */
bool app_gateway_control_sequence_admission_available(uint32_t requested_ids);

/*
 * Run only from background maintenance.  It may reserve one standby block
 * after the active block reaches its half-block prefetch threshold and the
 * 24-hour gate permits another attempt.  It never belongs on an admission/RF
 * path.
 */
int app_gateway_control_sequence_maintain(void);

#if defined(APP_GATEWAY_CONTROL_SEQUENCE_TESTING)
/* Native tests drive maintenance with an explicit clock; no test sleeps. */
int app_gateway_control_sequence_test_maintain_at(uint32_t now_ms);
void app_gateway_control_sequence_test_reset(void);
#endif

#endif
