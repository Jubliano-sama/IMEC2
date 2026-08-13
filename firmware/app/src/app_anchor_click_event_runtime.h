#ifndef APP_ANCHOR_CLICK_EVENT_RUNTIME_H
#define APP_ANCHOR_CLICK_EVENT_RUNTIME_H

#include "firmware_state_machines.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This runtime is the protocol-phase guard.  The anchor radio worker owns the
 * continuous Channel-5 lease and the mesh report service owns result custody;
 * this object owns the lifecycle transitions without duplicating either owner.
 */
void app_anchor_click_event_runtime_reset(void);

int app_anchor_click_event_runtime_claim(
    const struct uwb_wake_claim_frame *claim,
    uint32_t now_ms,
    struct fw_transition *transition);

/*
 * A normal schedule follows DISCOVER_RECEIVED.  RANGE_ONLY schedules omit the
 * discovery reply on the wire, but still advance through the same explicit
 * protocol boundary before ranging begins.
 */
int app_anchor_click_event_runtime_schedule_received(
    bool discovery_reply_sent,
    uint32_t now_ms,
    struct fw_transition *transition);

int app_anchor_click_event_runtime_handle(
    enum fw_event_type type,
    uint32_t now_ms,
    struct fw_transition *transition);

/* Cancel is fail-closed once result custody has been retained. */
int app_anchor_click_event_runtime_abort(
    uint32_t now_ms,
    struct fw_transition *transition);

/*
 * The report owner must pass the exact active identity. Completion is
 * idempotent after that phase has detached and a successor has started.
 */
int app_anchor_click_event_runtime_custody_released(
    uint64_t clicker_id,
    uint32_t click_event_id,
    uint8_t attempt_index,
    uint32_t now_ms,
    struct fw_transition *transition);

enum fw_anchor_click_state app_anchor_click_event_runtime_state(void);
bool app_anchor_click_event_runtime_active(void);
bool app_anchor_click_event_runtime_result_owned(void);

#ifdef __cplusplus
}
#endif

#endif
