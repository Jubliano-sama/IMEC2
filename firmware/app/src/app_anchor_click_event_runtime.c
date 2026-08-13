#include "app_anchor_click_event_runtime.h"

#include <errno.h>
#include <string.h>

struct app_anchor_click_event_runtime_context {
    struct fw_anchor_click_sm machine;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint64_t operation_id;
    uint32_t next_generation;
    bool key_valid;
};

static struct app_anchor_click_event_runtime_context runtime;

static uint64_t operation_id_for_claim(
    const struct uwb_wake_claim_frame *claim)
{
    uint64_t value;

    value = claim->priority_id ^ claim->nonce ^ claim->clicker_id;
    value ^= ((uint64_t)claim->network_id << 32u) | claim->click_event_id;
    value ^= ((uint64_t)claim->attempt_index << 56u);
    return value == 0u ? UINT64_C(1) : value;
}

static bool claim_key_matches(
    const struct app_anchor_click_event_runtime_context *context,
    const struct uwb_wake_claim_frame *claim)
{
    return context->key_valid && context->machine.identity.active &&
           context->clicker_id == claim->clicker_id &&
           context->click_event_id == claim->click_event_id &&
           context->attempt_index == claim->attempt_index &&
           context->nonce == claim->nonce;
}

static bool claim_is_valid(const struct uwb_wake_claim_frame *claim)
{
    return claim != NULL && claim->clicker_id != 0u &&
           claim->click_event_id != 0u && claim->attempt_index != 0u &&
           claim->nonce != 0u && claim->priority_id != 0u;
}

static int result_to_errno(enum fw_sm_result result)
{
    switch (result) {
    case FW_SM_APPLIED:
    case FW_SM_IGNORED:
        return 0;
    case FW_SM_STALE:
        return -ESTALE;
    case FW_SM_BUSY:
        return -EBUSY;
    case FW_SM_INVALID:
    default:
        return -EPROTO;
    }
}

static struct fw_event runtime_event(enum fw_event_type type,
                                     uint32_t now_ms)
{
    struct fw_event event = {
        .timestamp_ms = now_ms,
        .operation_id = runtime.operation_id,
        .generation = runtime.machine.identity.generation,
        .target_instance = 0u,
        .reply_instance = 0u,
        .target = FW_MACHINE_ANCHOR_CLICK,
        .reply_to = FW_MACHINE_ANCHOR_CLICK,
        .source = FW_EVENT_SOURCE_RADIO,
        .type = type,
    };

    return event;
}

static int dispatch_event(const struct fw_event *event,
                          struct fw_transition *transition)
{
    struct fw_transition local_transition;
    enum fw_sm_result result;

    if (event == NULL) {
        return -EINVAL;
    }
    if (transition == NULL) {
        transition = &local_transition;
    }
    result = fw_anchor_click_sm_handle(&runtime.machine,
                                       event,
                                       transition);
    return result_to_errno(result);
}

void app_anchor_click_event_runtime_reset(void)
{
    memset(&runtime, 0, sizeof(runtime));
    fw_anchor_click_sm_init(&runtime.machine);
}

int app_anchor_click_event_runtime_claim(
    const struct uwb_wake_claim_frame *claim,
    uint32_t now_ms,
    struct fw_transition *transition)
{
    struct fw_event event;
    int ret;

    if (!claim_is_valid(claim)) {
        return -EINVAL;
    }

    if (claim_key_matches(&runtime, claim)) {
        return 0;
    }
    if (app_anchor_click_event_runtime_result_owned()) {
        /*
         * RESULT_RETAINED means the report queue/relay has already accepted
         * the immutable bytes. Keep that transport custody, but release this
         * phase-only owner so a later physical click can start while the old
         * report finishes its independent gateway-ACK path.
         */
        ret = app_anchor_click_event_runtime_handle(
            FW_EVENT_RESULT_CUSTODY_RELEASED, now_ms, NULL);
        if (ret != 0) {
            return ret;
        }
    }
    if (runtime.machine.identity.active) {
        ret = app_anchor_click_event_runtime_abort(now_ms, NULL);
        if (ret != 0) {
            return ret;
        }
    }

    runtime.next_generation++;
    if (runtime.next_generation == 0u) {
        runtime.next_generation = 1u;
    }
    runtime.clicker_id = claim->clicker_id;
    runtime.click_event_id = claim->click_event_id;
    runtime.attempt_index = claim->attempt_index;
    runtime.nonce = claim->nonce;
    runtime.operation_id = operation_id_for_claim(claim);
    runtime.key_valid = true;

    event = runtime_event(FW_EVENT_WAKE_CLAIM_ACCEPTED, now_ms);
    event.operation_id = runtime.operation_id;
    event.generation = runtime.next_generation;
    ret = dispatch_event(&event, transition);
    if (ret != 0) {
        runtime.key_valid = false;
        return ret;
    }
    return 0;
}

int app_anchor_click_event_runtime_schedule_received(
    bool discovery_reply_sent,
    uint32_t now_ms,
    struct fw_transition *transition)
{
    struct fw_event event;
    int ret;

    if (!runtime.machine.identity.active) {
        return -ESTALE;
    }

    if (!discovery_reply_sent &&
        runtime.machine.state == FW_ANCHOR_CLICK_CLAIMED) {
        /*
         * RANGE_ONLY has no discovery reply on the wire.  The shared state
         * machine keeps one linear phase model, so consume the omitted
         * discovery boundary without executing its reply effect.
         */
        event = runtime_event(FW_EVENT_DISCOVER_RECEIVED, now_ms);
        ret = dispatch_event(&event, NULL);
        if (ret != 0) {
            return ret;
        }
    }

    return app_anchor_click_event_runtime_handle(
        FW_EVENT_SCHEDULE_RECEIVED, now_ms, transition);
}

int app_anchor_click_event_runtime_handle(
    enum fw_event_type type,
    uint32_t now_ms,
    struct fw_transition *transition)
{
    struct fw_event event;

    if (!runtime.machine.identity.active) {
        return -ESTALE;
    }
    event = runtime_event(type, now_ms);
    return dispatch_event(&event, transition);
}

int app_anchor_click_event_runtime_abort(
    uint32_t now_ms,
    struct fw_transition *transition)
{
    struct fw_event event;

    if (!runtime.machine.identity.active ||
        runtime.machine.state == FW_ANCHOR_CLICK_IDLE ||
        runtime.machine.state == FW_ANCHOR_CLICK_ABORTED) {
        return 0;
    }
    if (runtime.machine.state == FW_ANCHOR_CLICK_RESULT_OWNED) {
        /* Result custody is a separate owner and must not be discarded here. */
        return 0;
    }
    event = runtime_event(FW_EVENT_CANCEL, now_ms);
    return dispatch_event(&event, transition);
}

int app_anchor_click_event_runtime_custody_released(
    uint64_t clicker_id,
    uint32_t click_event_id,
    uint8_t attempt_index,
    uint32_t now_ms,
    struct fw_transition *transition)
{
    if (!runtime.machine.identity.active ||
        runtime.machine.state != FW_ANCHOR_CLICK_RESULT_OWNED) {
        /* The transport owner may finish after a successor phase started. */
        return 0;
    }
    if (!runtime.key_valid || runtime.clicker_id != clicker_id ||
        runtime.click_event_id != click_event_id ||
        runtime.attempt_index != attempt_index) {
        return -ESTALE;
    }
    return app_anchor_click_event_runtime_handle(
        FW_EVENT_RESULT_CUSTODY_RELEASED, now_ms, transition);
}

enum fw_anchor_click_state app_anchor_click_event_runtime_state(void)
{
    return runtime.machine.state;
}

bool app_anchor_click_event_runtime_active(void)
{
    return runtime.machine.identity.active;
}

bool app_anchor_click_event_runtime_result_owned(void)
{
    return runtime.machine.identity.active &&
           runtime.machine.state == FW_ANCHOR_CLICK_RESULT_OWNED;
}
