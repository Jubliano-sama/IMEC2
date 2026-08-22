#ifndef FIRMWARE_STATE_MACHINES_H
#define FIRMWARE_STATE_MACHINES_H

#include "firmware_events.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_CONNECTION_MISSED_RX_LIMIT 8u
#define FW_CLICK_MIN_UNIQUE_RANGES 3u
#define FW_BUTTON_DEBOUNCE_MS 50u
#define FW_BUTTON_LONG_PRESS_MS 1500u
#define FW_BUTTON_SELF_TEST_ARM_MS 3000u
#define FW_RADIO_HANDOFF_MAX_SCHEDULE_ATTEMPTS 8u

struct fw_operation_identity {
    uint64_t operation_id;
    uint32_t generation;
    bool active;
};

enum fw_radio_state {
    FW_RADIO_OFF = 0,
    FW_RADIO_STARTING,
    FW_RADIO_READY_5,
    FW_RADIO_BUSY_5,
    FW_RADIO_RETUNING_9,
    FW_RADIO_READY_9,
    FW_RADIO_BUSY_9,
    FW_RADIO_RETUNING_5,
    FW_RADIO_CANCELLING,
    FW_RADIO_CANCELLED_WAIT_RELEASE,
    FW_RADIO_RECOVERING,
};

struct fw_radio_job {
    struct fw_operation_identity identity;
    enum fw_machine_id owner;
    uint16_t owner_instance;
    enum fw_radio_channel channel;
    enum fw_radio_mode mode;
    enum fw_radio_priority priority;
    uint64_t not_before_ms;
    uint64_t deadline_ms;
    uint32_t maximum_duration_ms;
};

struct fw_radio_sm {
    enum fw_radio_state state;
    struct fw_radio_job active;
    struct fw_radio_job pending;
    enum fw_radio_channel tuned_channel;
};

enum fw_radio_handoff_state {
    FW_RADIO_HANDOFF_IDLE = 0,
    FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY,
    FW_RADIO_HANDOFF_SCHEDULING,
    FW_RADIO_HANDOFF_WAIT_RETRY,
};

/*
 * Owns the asynchronous handoff from a lower-priority receive to accepted
 * priority work. The platform adapter owns only the opaque work item; this
 * machine owns generation, ordering, retry, and terminal state.
 */
struct fw_radio_handoff_sm {
    struct fw_operation_identity identity;
    enum fw_radio_handoff_state state;
    uint32_t admission_cutoff;
    uint8_t schedule_attempts;
};

enum fw_radio_activity_state {
    FW_RADIO_ACTIVITY_IDLE = 0,
    FW_RADIO_ACTIVITY_CLICK,
    FW_RADIO_ACTIVITY_SURVEY,
    FW_RADIO_ACTIVITY_MESH_RX,
    FW_RADIO_ACTIVITY_MESH_TX,
    FW_RADIO_ACTIVITY_GATEWAY_RX,
};

enum fw_c5_tx_intent {
    FW_C5_TX_INTENT_BACKGROUND = 0,
    FW_C5_TX_INTENT_CAUSAL_RESPONSE,
    /* An exact EVENT_ACCEPT toward the current upstream ACK owner establishes
     * the Channel-9 cadence needed to receive that ACK. */
    FW_C5_TX_INTENT_ACK_RX_TIMING_RESPONSE,
    /* A relay-core-validated gateway survey control may interrupt a retained
     * Channel-9 ACK wait without taking ownership of the retained bytes. */
    FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL,
};

struct fw_radio_activity_capture {
    bool click_active;
    bool survey_pending;
    uint32_t rx_queue_used;
    uint32_t report_queue_used;
    bool relay_tx_active;
    bool route_waiting_tx_active;
    bool ch9_ack_wait_active;
    bool ch9_ack_send_pending;
    bool ch9_ack_receive_eligible;
    bool gateway_continuous_ch9;
    enum fw_c5_tx_intent c5_tx_intent;
};

struct fw_radio_activity_decision {
    enum fw_radio_activity_state state;
    bool mesh_work_allowed;
    bool c5_tx_allowed;
    bool route_wait_allowed;
    bool report_tx_allowed;
    bool uwb_rx_allowed;
    const char *reason;
};

struct fw_radio_activity_runtime {
    bool last_state_valid;
    enum fw_radio_activity_state last_state;
};

enum fw_button_state {
    FW_BUTTON_IDLE = 0,
    FW_BUTTON_DEBOUNCE,
    FW_BUTTON_PRESSED,
    FW_BUTTON_WAIT_LONG_RELEASE,
    FW_BUTTON_NORMAL_CLICK,
    FW_BUTTON_SELF_TEST_ARMED,
    FW_BUTTON_SELF_TEST_CONFIRM,
    FW_BUTTON_SELF_TEST,
};

struct fw_button_sm {
    enum fw_button_state state;
};

enum fw_click_state {
    FW_CLICK_IDLE = 0,
    FW_CLICK_CREATE,
    FW_CLICK_POLITENESS,
    FW_CLICK_WAIT_PEER,
    FW_CLICK_WAKE,
    FW_CLICK_DISCOVER,
    FW_CLICK_RELEASE,
    FW_CLICK_SCHEDULE,
    FW_CLICK_RANGE,
    FW_CLICK_EVALUATE,
    FW_CLICK_RETRY,
    FW_CLICK_SUCCESS,
    FW_CLICK_FAILURE,
};

struct fw_click_sm {
    struct fw_operation_identity identity;
    enum fw_click_state state;
    uint8_t attempts_started;
    uint8_t unique_ranges;
    /* The adapter currently reports RF start for the wake child only.  Keep
     * that child binding explicit so a late same-generation completion cannot
     * be charged to a later click phase or a duplicate wake. */
    enum fw_click_state expected_rf_phase;
    uint8_t expected_rf_attempt;
};

enum fw_anchor_click_state {
    FW_ANCHOR_CLICK_IDLE = 0,
    FW_ANCHOR_CLICK_CLAIMED,
    FW_ANCHOR_CLICK_DISCOVERY_REPLIED,
    FW_ANCHOR_CLICK_SCHEDULED,
    FW_ANCHOR_CLICK_RANGING,
    FW_ANCHOR_CLICK_RESULT_OWNED,
    FW_ANCHOR_CLICK_ABORTED,
};

struct fw_anchor_click_sm {
    struct fw_operation_identity identity;
    enum fw_anchor_click_state state;
};

enum fw_route_state {
    FW_ROUTE_EMPTY = 0,
    FW_ROUTE_DIRECT_PROBE,
    FW_ROUTE_DISCOVERING,
    FW_ROUTE_READY,
    FW_ROUTE_HOLD_DOWN,
    FW_ROUTE_CLOSING,
};

struct fw_route_sm {
    struct fw_operation_identity identity;
    enum fw_route_state state;
    uint64_t parent_id;
};

enum fw_connection_state {
    FW_CONNECTION_EMPTY = 0,
    FW_CONNECTION_NEGOTIATING,
    FW_CONNECTION_ACTIVE,
    FW_CONNECTION_STALE,
    FW_CONNECTION_CLOSING,
};

struct fw_connection_sm {
    struct fw_operation_identity identity;
    enum fw_connection_state state;
    uint64_t peer_id;
    uint32_t event_counter;
    uint8_t consecutive_missed_rx;
};

enum fw_delivery_state {
    FW_DELIVERY_EMPTY = 0,
    FW_DELIVERY_OWNED,
    FW_DELIVERY_WAIT_ROUTE,
    FW_DELIVERY_WAIT_CONNECTION,
    FW_DELIVERY_WAIT_TX,
    FW_DELIVERY_WAIT_ACK,
    FW_DELIVERY_RETRY,
    FW_DELIVERY_TRANSFERRED,
    FW_DELIVERY_DELIVERED,
    FW_DELIVERY_FAILED,
};

struct fw_delivery_sm {
    struct fw_operation_identity identity;
    enum fw_delivery_state state;
    uint8_t attempts_started;
    bool owns_custody;
};

enum fw_gateway_uwb_state {
    FW_GATEWAY_UWB_LISTEN_9 = 0,
    FW_GATEWAY_UWB_RECEIVE_BATCH,
    FW_GATEWAY_UWB_ACCEPT_BATCH,
    FW_GATEWAY_UWB_WAIT_HOST_ITEM,
    FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
    FW_GATEWAY_UWB_SEND_ACK,
    FW_GATEWAY_UWB_RETIRE_HOST_ITEM,
};

struct fw_gateway_uwb_sm {
    enum fw_gateway_uwb_state state;
    /*
     * The accepted packet remains gateway-owned until the GUI has accepted
     * the exact stream record into process RAM and the upstream ACK has been
     * handed off. This is deliberately a bounded volatile marker, not a
     * receipt journal or NVS-backed ledger.
     */
    bool host_item_pending;
};

enum fw_host_link_state {
    FW_HOST_LINK_READY = 0,
    FW_HOST_LINK_BLOCKED,
    FW_HOST_LINK_SENDING,
};

struct fw_host_link_sm {
    enum fw_host_link_state state;
    bool item_pending;
};

enum fw_enumeration_state {
    FW_ENUMERATION_IDLE = 0,
    FW_ENUMERATION_SEND_CLAIM,
    FW_ENUMERATION_COLLECT_RESPONSES,
    FW_ENUMERATION_FREEZE_TABLE,
    FW_ENUMERATION_SEND_TABLE,
    FW_ENUMERATION_COMPLETE,
    FW_ENUMERATION_FAILED,
};

struct fw_enumeration_sm {
    struct fw_operation_identity identity;
    enum fw_enumeration_state state;
};

enum fw_survey_state {
    FW_SURVEY_IDLE = 0,
    FW_SURVEY_SEND_CONFIG,
    FW_SURVEY_DISCOVERY,
    FW_SURVEY_COLLECT_REPORTS,
    FW_SURVEY_BUILD_GRAPH,
    FW_SURVEY_SELECT_PAIRS,
    FW_SURVEY_ARM_PAIRS,
    FW_SURVEY_WAIT_RESULTS,
    FW_SURVEY_UPDATE_GRAPH,
    FW_SURVEY_PUBLISH,
    FW_SURVEY_COMPLETE,
    FW_SURVEY_PARTIAL,
    FW_SURVEY_FAILED,
};

struct fw_survey_sm {
    struct fw_operation_identity identity;
    enum fw_survey_state state;
};

enum fw_pair_coordinator_state {
    FW_PAIR_COORDINATOR_IDLE = 0,
    FW_PAIR_COORDINATOR_PREPARE_INITIATOR,
    FW_PAIR_COORDINATOR_PREPARE_RESPONDER,
    FW_PAIR_COORDINATOR_START_RESPONDER,
    FW_PAIR_COORDINATOR_START_INITIATOR,
    FW_PAIR_COORDINATOR_WAIT_RESULT,
    FW_PAIR_COORDINATOR_COMPLETE,
    FW_PAIR_COORDINATOR_FAILED,
};

struct fw_pair_coordinator_sm {
    struct fw_operation_identity identity;
    enum fw_pair_coordinator_state state;
};

enum fw_survey_pair_state {
    FW_SURVEY_PAIR_IDLE = 0,
    FW_SURVEY_PAIR_PREPARED,
    FW_SURVEY_PAIR_ARMED,
    FW_SURVEY_PAIR_WAIT_START,
    FW_SURVEY_PAIR_RANGE,
    FW_SURVEY_PAIR_RESULT_OWNED,
    FW_SURVEY_PAIR_COMPLETE,
    FW_SURVEY_PAIR_ABORTED,
};

struct fw_survey_pair_sm {
    struct fw_operation_identity identity;
    enum fw_survey_pair_state state;
};

void fw_radio_sm_init(struct fw_radio_sm *machine);
void fw_radio_handoff_sm_init(struct fw_radio_handoff_sm *machine);
void fw_radio_activity_runtime_init(struct fw_radio_activity_runtime *runtime);
int fw_radio_activity_decide(
    const struct fw_radio_activity_capture *capture,
    struct fw_radio_activity_runtime *runtime,
    struct fw_radio_activity_decision *decision,
    bool *state_changed);
const char *fw_radio_activity_state_name(enum fw_radio_activity_state state);
void fw_button_sm_init(struct fw_button_sm *machine);
void fw_click_sm_init(struct fw_click_sm *machine);
void fw_anchor_click_sm_init(struct fw_anchor_click_sm *machine);
void fw_route_sm_init(struct fw_route_sm *machine);
void fw_connection_sm_init(struct fw_connection_sm *machine);
void fw_delivery_sm_init(struct fw_delivery_sm *machine);
void fw_gateway_uwb_sm_init(struct fw_gateway_uwb_sm *machine);
void fw_host_link_sm_init(struct fw_host_link_sm *machine);
void fw_enumeration_sm_init(struct fw_enumeration_sm *machine);
void fw_survey_sm_init(struct fw_survey_sm *machine);
void fw_pair_coordinator_sm_init(struct fw_pair_coordinator_sm *machine);
void fw_survey_pair_sm_init(struct fw_survey_pair_sm *machine);

enum fw_sm_result fw_radio_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition);
enum fw_sm_result fw_radio_handoff_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
enum fw_sm_result fw_button_sm_handle(void *context,
                                      const struct fw_event *event,
                                      struct fw_transition *transition);
enum fw_sm_result fw_click_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition);
enum fw_sm_result fw_anchor_click_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
enum fw_sm_result fw_route_sm_handle(void *context,
                                     const struct fw_event *event,
                                     struct fw_transition *transition);
enum fw_sm_result fw_connection_sm_handle(void *context,
                                          const struct fw_event *event,
                                          struct fw_transition *transition);
enum fw_sm_result fw_delivery_sm_handle(void *context,
                                        const struct fw_event *event,
                                        struct fw_transition *transition);
enum fw_sm_result fw_gateway_uwb_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
enum fw_sm_result fw_host_link_sm_handle(void *context,
                                         const struct fw_event *event,
                                         struct fw_transition *transition);
enum fw_sm_result fw_enumeration_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
enum fw_sm_result fw_survey_sm_handle(void *context,
                                      const struct fw_event *event,
                                      struct fw_transition *transition);
enum fw_sm_result fw_pair_coordinator_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);
enum fw_sm_result fw_survey_pair_sm_handle(
    void *context,
    const struct fw_event *event,
    struct fw_transition *transition);

#ifdef __cplusplus
}
#endif

#endif
