#ifndef FIRMWARE_EVENTS_H
#define FIRMWARE_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_EVENT_QUEUE_CAPACITY 32u
#define FW_EVENT_HANDLER_CAPACITY 24u

enum fw_machine_id {
    FW_MACHINE_NONE = 0,
    FW_MACHINE_RADIO,
    FW_MACHINE_BUTTON,
    FW_MACHINE_CLICK,
    FW_MACHINE_ANCHOR_CLICK,
    FW_MACHINE_ROUTE,
    FW_MACHINE_CONNECTION,
    FW_MACHINE_DELIVERY,
    FW_MACHINE_GATEWAY_UWB,
    FW_MACHINE_HOST_LINK,
    FW_MACHINE_ENUMERATION,
    FW_MACHINE_SURVEY,
    FW_MACHINE_PAIR_COORDINATOR,
    FW_MACHINE_SURVEY_PAIR,
    FW_MACHINE_COUNT,
};

enum fw_event_source {
    FW_EVENT_SOURCE_BOOT = 0,
    FW_EVENT_SOURCE_ISR,
    FW_EVENT_SOURCE_TIMER,
    FW_EVENT_SOURCE_RADIO,
    FW_EVENT_SOURCE_BLE,
    FW_EVENT_SOURCE_SERVICE,
    FW_EVENT_SOURCE_MACHINE,
    FW_EVENT_SOURCE_COUNT,
};

enum fw_event_type {
    FW_EVENT_NONE = 0,
    FW_EVENT_START,
    FW_EVENT_RESET,
    FW_EVENT_CANCEL,
    FW_EVENT_DEADLINE_EXPIRED,
    FW_EVENT_TIMER_EXPIRED,

    FW_EVENT_BUTTON_PRESSED,
    FW_EVENT_BUTTON_RELEASED,
    FW_EVENT_DEBOUNCE_CONFIRMED,
    FW_EVENT_LONG_PRESS_DETECTED,
    FW_EVENT_ACTION_ACCEPTED,

    FW_EVENT_RADIO_REQUESTED,
    FW_EVENT_RADIO_INIT_SUCCEEDED,
    FW_EVENT_RADIO_INIT_FAILED,
    FW_EVENT_RADIO_RETUNE_SUCCEEDED,
    FW_EVENT_RADIO_JOB_COMPLETED,
    FW_EVENT_RADIO_JOB_TIMED_OUT,
    FW_EVENT_RADIO_JOB_CANCELLED,
    FW_EVENT_RADIO_JOB_FAILED,
    FW_EVENT_RADIO_RECOVERY_RETRY,
    FW_EVENT_RADIO_RECOVERY_EXHAUSTED,
    FW_EVENT_RADIO_PREEMPT_REQUESTED,
    FW_EVENT_RADIO_SAFE_BOUNDARY,
    FW_EVENT_EFFECT_SUCCEEDED,
    FW_EVENT_EFFECT_FAILED,

    FW_EVENT_CLICK_CREATED,
    FW_EVENT_CHANNEL_CLEAR,
    FW_EVENT_HIGHER_PRIORITY_TRAFFIC,
    FW_EVENT_PEER_WAIT_ENDED,
    FW_EVENT_WAKE_COMPLETED,
    FW_EVENT_DISCOVERY_COMPLETED,
    FW_EVENT_RELEASE_COMPLETED,
    FW_EVENT_SCHEDULE_COMPLETED,
    FW_EVENT_RANGE_COMPLETED,
    FW_EVENT_RF_STARTED,
    FW_EVENT_RETRY_ALLOWED,
    FW_EVENT_RETRY_EXHAUSTED,

    FW_EVENT_WAKE_CLAIM_ACCEPTED,
    FW_EVENT_DISCOVER_RECEIVED,
    FW_EVENT_SCHEDULE_RECEIVED,
    FW_EVENT_RANGE_DUE,
    FW_EVENT_RESULT_RETAINED,
    FW_EVENT_RESULT_CUSTODY_RELEASED,

    FW_EVENT_ROUTE_NEEDED,
    FW_EVENT_DIRECT_PROBE_FAILED,
    FW_EVENT_ROUTE_FOUND,
    FW_EVENT_ROUTE_FAILED,
    FW_EVENT_ROUTE_INVALIDATED,
    FW_EVENT_HOLD_DOWN_EXPIRED,
    FW_EVENT_ROUTE_CLOSE_REQUESTED,
    FW_EVENT_ROUTE_CLOSE_COMPLETED,

    FW_EVENT_CONNECTION_NEEDED,
    FW_EVENT_PROPOSAL_ACCEPTED,
    FW_EVENT_NEGOTIATION_FAILED,
    FW_EVENT_CONNECTION_EVENT_COMPLETED,
    FW_EVENT_RECEIVE_TURN_MISSED,
    FW_EVENT_SUPERVISION_EXPIRED,
    FW_EVENT_CLICK_ACCEPTED,
    FW_EVENT_CONNECTION_CLOSE_REQUESTED,
    FW_EVENT_CONNECTION_CLOSE_COMPLETED,

    FW_EVENT_PACKET_OWNED,
    FW_EVENT_ROUTE_READY,
    FW_EVENT_CONNECTION_READY,
    FW_EVENT_RF_DEFERRED,
    FW_EVENT_HOP_ACK_RECEIVED,
    FW_EVENT_GATEWAY_ACK_RECEIVED,
    FW_EVENT_ACK_TIMED_OUT,

    FW_EVENT_GATEWAY_BATCH_RECEIVED,
    FW_EVENT_GATEWAY_BATCH_ACCEPTED,
    FW_EVENT_GATEWAY_ACK_SENT,
    FW_EVENT_HOST_ITEM_QUEUED,
    FW_EVENT_HOST_ITEM_SENT,
    FW_EVENT_HOST_ITEM_ACCEPTED,
    FW_EVENT_BLE_BLOCKED,
    FW_EVENT_BLE_READY,

    FW_EVENT_CLAIM_SENT,
    FW_EVENT_RESPONSE_WINDOW_CLOSED,
    FW_EVENT_RESPONSES_FROZEN,
    FW_EVENT_TABLE_SENT,
    FW_EVENT_PUBLICATION_COMPLETED,
    FW_EVENT_OPERATION_FAILED,

    FW_EVENT_CONFIG_SENT,
    FW_EVENT_DISCOVERY_ROUNDS_COMPLETED,
    FW_EVENT_REPORT_WINDOW_CLOSED,
    FW_EVENT_GRAPH_BUILT,
    FW_EVENT_PAIR_AVAILABLE,
    FW_EVENT_NO_PAIR_AVAILABLE,
    FW_EVENT_PAIR_ARMED,
    FW_EVENT_PAIR_RESULT_RECEIVED,
    FW_EVENT_SURVEY_COMPLETE,
    FW_EVENT_SURVEY_PARTIAL,

    FW_EVENT_PAIR_PREPARED,
    FW_EVENT_PAIR_START_ARMED,
    FW_EVENT_PAIR_START_DUE,
    FW_EVENT_PAIR_RANGE_COMPLETED,
    FW_EVENT_PAIR_ABORTED,

    FW_EVENT_TYPE_COUNT,
};

enum fw_effect_type {
    FW_EFFECT_NONE = 0,
    FW_EFFECT_START_TIMER,
    FW_EFFECT_CANCEL_TIMER,
    FW_EFFECT_PUBLISH_EVENT,
    FW_EFFECT_TRACE_TERMINAL,

    FW_EFFECT_RADIO_INITIALIZE,
    FW_EFFECT_RADIO_RETUNE,
    FW_EFFECT_RADIO_RUN_JOB,
    FW_EFFECT_RADIO_CANCEL_JOB,
    FW_EFFECT_RADIO_RECOVER,
    FW_EFFECT_RADIO_POWER_OFF,
    FW_EFFECT_RADIO_REPORT_RESULT,
    FW_EFFECT_RADIO_REQUEST_ABORT,
    FW_EFFECT_RADIO_SCHEDULE_PENDING,
    FW_EFFECT_RADIO_CLEAR_ABORT,

    FW_EFFECT_CLICK_CREATE,
    FW_EFFECT_CLICK_CHECK_POLITENESS,
    FW_EFFECT_CLICK_SEND_WAKE,
    FW_EFFECT_CLICK_DISCOVER,
    FW_EFFECT_CLICK_SEND_RELEASE,
    FW_EFFECT_CLICK_SEND_SCHEDULE,
    FW_EFFECT_CLICK_RANGE,
    FW_EFFECT_CLICK_CLEANUP,

    FW_EFFECT_ANCHOR_SEND_DISCOVERY_REPLY,
    FW_EFFECT_ANCHOR_WAIT_SCHEDULE,
    FW_EFFECT_ANCHOR_RANGE,
    FW_EFFECT_ANCHOR_RETAIN_RESULT,
    FW_EFFECT_ANCHOR_CLEANUP,

    FW_EFFECT_ROUTE_DIRECT_PROBE,
    FW_EFFECT_ROUTE_DISCOVER,
    FW_EFFECT_ROUTE_HOLD_DOWN,
    FW_EFFECT_ROUTE_CLOSE,
    FW_EFFECT_CONNECTION_NEGOTIATE,
    FW_EFFECT_CONNECTION_CLOSE,

    FW_EFFECT_DELIVERY_WAIT_ROUTE,
    FW_EFFECT_DELIVERY_WAIT_CONNECTION,
    FW_EFFECT_DELIVERY_SEND,
    FW_EFFECT_DELIVERY_TRANSFER_CUSTODY,
    FW_EFFECT_DELIVERY_COMPLETE,
    FW_EFFECT_DELIVERY_FAIL,

    FW_EFFECT_GATEWAY_VALIDATE_BATCH,
    FW_EFFECT_GATEWAY_ACCEPT_BATCH,
    FW_EFFECT_GATEWAY_SEND_ACK,
    FW_EFFECT_HOST_SEND_ITEM,
    FW_EFFECT_HOST_RETIRE_ITEM,

    FW_EFFECT_ENUM_SEND_CLAIM,
    FW_EFFECT_ENUM_FREEZE_RESPONSES,
    FW_EFFECT_ENUM_SEND_TABLE,
    FW_EFFECT_ENUM_COMPLETE,

    FW_EFFECT_SURVEY_SEND_CONFIG,
    FW_EFFECT_SURVEY_BEGIN_DISCOVERY,
    FW_EFFECT_SURVEY_COLLECT_REPORTS,
    FW_EFFECT_SURVEY_BUILD_GRAPH,
    FW_EFFECT_SURVEY_SELECT_PAIR,
    FW_EFFECT_SURVEY_ARM_PAIR,
    FW_EFFECT_SURVEY_UPDATE_GRAPH,
    FW_EFFECT_SURVEY_PUBLISH,
    FW_EFFECT_SURVEY_COMPLETE,
    FW_EFFECT_SURVEY_PUBLISH_PARTIAL,

    FW_EFFECT_PAIR_PREPARE_INITIATOR,
    FW_EFFECT_PAIR_PREPARE_RESPONDER,
    FW_EFFECT_PAIR_START_RESPONDER,
    FW_EFFECT_PAIR_START_INITIATOR,
    FW_EFFECT_PAIR_WAIT_RESULT,

    FW_EFFECT_PAIR_PREPARE,
    FW_EFFECT_PAIR_ARM_START,
    FW_EFFECT_PAIR_RANGE,
    FW_EFFECT_PAIR_RETAIN_RESULT,
    FW_EFFECT_PAIR_COMPLETE,
    FW_EFFECT_PAIR_ABORT,

    FW_EFFECT_TYPE_COUNT,
};

enum fw_sm_result {
    FW_SM_APPLIED = 0,
    FW_SM_IGNORED,
    FW_SM_STALE,
    FW_SM_BUSY,
    FW_SM_INVALID,
};

enum fw_radio_channel {
    FW_RADIO_CHANNEL_NONE = 0,
    FW_RADIO_CHANNEL_5 = 5,
    FW_RADIO_CHANNEL_9 = 9,
};

enum fw_radio_mode {
    FW_RADIO_MODE_NONE = 0,
    FW_RADIO_MODE_RX,
    FW_RADIO_MODE_TX,
};

enum fw_radio_priority {
    FW_RADIO_PRIORITY_BACKGROUND = 0,
    FW_RADIO_PRIORITY_CONNECTION_RX,
    FW_RADIO_PRIORITY_ROUTE_CONTROL,
    FW_RADIO_PRIORITY_CLICK,
    FW_RADIO_PRIORITY_GATEWAY_CONTROL,
};

#define FW_RADIO_REQUEST_VALUE(mode, priority) \
    ((uint32_t)(uint8_t)(mode) | ((uint32_t)(uint8_t)(priority) << 8u))
#define FW_RADIO_REQUEST_MODE(value) \
    ((enum fw_radio_mode)((value) & UINT32_C(0xff)))
#define FW_RADIO_REQUEST_PRIORITY(value) \
    ((enum fw_radio_priority)(((value) >> 8u) & UINT32_C(0xff)))

#define FW_EVENT_FLAG_ROUTE_READY UINT8_C(0x01)
#define FW_EVENT_FLAG_CONNECTION_READY UINT8_C(0x02)
#define FW_EVENT_FLAG_PATH_USABLE UINT8_C(0x04)
#define FW_EVENT_FLAG_MORE_PENDING UINT8_C(0x08)
#define FW_EVENT_FLAG_GRAPH_COMPLETE UINT8_C(0x10)
#define FW_EVENT_FLAG_RETRYABLE UINT8_C(0x20)

struct fw_event_payload {
    uint64_t subject_id;
    uint64_t not_before_ms;
    uint64_t deadline_ms;
    uint32_t value;
    uint32_t duration_ms;
    uint16_t count;
    uint8_t channel;
    uint8_t flags;
};

struct fw_event {
    uint64_t timestamp_ms;
    uint64_t operation_id;
    uint32_t generation;
    uint16_t target_instance;
    uint16_t reply_instance;
    enum fw_machine_id target;
    enum fw_machine_id reply_to;
    enum fw_event_source source;
    enum fw_event_type type;
    struct fw_event_payload payload;
};

struct fw_effect {
    uint64_t operation_id;
    uint32_t generation;
    uint16_t owner_instance;
    enum fw_machine_id owner;
    enum fw_effect_type type;
    struct fw_event_payload payload;
};

struct fw_transition {
    enum fw_machine_id machine;
    enum fw_event_type event;
    enum fw_sm_result result;
    uint16_t instance;
    uint16_t old_state;
    uint16_t new_state;
    struct fw_effect effect;
};

typedef enum fw_sm_result (*fw_event_handler_fn)(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
typedef void (*fw_effect_sink_fn)(const struct fw_effect *effect, void *context);
typedef void (*fw_transition_trace_fn)(const struct fw_event *event,
                                       const struct fw_transition *transition,
                                       void *context);

/*
 * The core dispatcher is deliberately not thread-safe. Producers outside its
 * serialized owner must use a platform ingress queue. Effect and trace
 * callbacks must not block or call dispatch recursively; effects may enqueue
 * later result events.
 */

struct fw_event_handler_registration {
    enum fw_machine_id machine;
    uint16_t instance;
    fw_event_handler_fn handler;
    void *context;
    bool used;
};

struct fw_event_dispatcher {
    struct fw_event queue[FW_EVENT_QUEUE_CAPACITY];
    struct fw_event_handler_registration handlers[FW_EVENT_HANDLER_CAPACITY];
    fw_effect_sink_fn effect_sink;
    fw_transition_trace_fn trace;
    void *callback_context;
    size_t head;
    size_t count;
    bool dispatching;
};

/*
 * Validate an event before it crosses an asynchronous dispatch boundary.
 *
 * Every event receives envelope, identity-pair, reply-instance, and known-flag
 * checks.  TIMER_EXPIRED, radio-manager events, and delivery ACK events also
 * have typed target/source/payload checks because their consumers interpret
 * the shared payload as a concrete record.  The remaining event types keep
 * the generic checks until their payload contracts are documented; callers
 * must not treat this function as exhaustive validation for those types.
 */
bool fw_event_validate(const struct fw_event *event);

void fw_event_dispatcher_init(struct fw_event_dispatcher *dispatcher,
                              fw_effect_sink_fn effect_sink,
                              fw_transition_trace_fn trace,
                              void *callback_context);
int fw_event_dispatcher_register(struct fw_event_dispatcher *dispatcher,
                                 enum fw_machine_id machine,
                                 uint16_t instance,
                                 fw_event_handler_fn handler,
                                 void *context);
int fw_event_dispatcher_post(struct fw_event_dispatcher *dispatcher,
                             const struct fw_event *event);
int fw_event_dispatcher_dispatch_one(struct fw_event_dispatcher *dispatcher,
                                     struct fw_transition *transition);
int fw_event_dispatcher_dispatch_all(struct fw_event_dispatcher *dispatcher,
                                     size_t maximum_events,
                                     size_t *processed);
size_t fw_event_dispatcher_pending(const struct fw_event_dispatcher *dispatcher);

#ifdef __cplusplus
}
#endif

#endif
