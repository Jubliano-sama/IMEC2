#include "firmware_state_machines.h"
#include "firmware_delivery_loss.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static struct fw_event operation_event(enum fw_machine_id target,
                                       enum fw_event_type type,
                                       uint64_t operation_id,
                                       uint32_t generation)
{
    return (struct fw_event) {
        .operation_id = operation_id,
        .generation = generation,
        .target = target,
        .reply_to = target,
        .source = FW_EVENT_SOURCE_MACHINE,
        .type = type,
    };
}

static enum fw_sm_result apply(fw_event_handler_fn handler,
                               void *machine,
                               struct fw_event *event,
                               uint16_t expected_state,
                               enum fw_effect_type expected_effect)
{
    struct fw_transition transition;
    enum fw_sm_result result = handler(machine, event, &transition);

    assert(result == transition.result);
    assert(transition.new_state == expected_state);
    assert(transition.effect.type == expected_effect);
    return result;
}

static void test_radio_is_single_owner_and_preempts_only_upward(void)
{
    struct fw_radio_sm radio;
    struct fw_event job = operation_event(FW_MACHINE_RADIO,
                                          FW_EVENT_RADIO_REQUESTED,
                                          11u, 1u);
    struct fw_event result;

    fw_radio_sm_init(&radio);
    job.reply_to = FW_MACHINE_DELIVERY;
    job.payload.channel = FW_RADIO_CHANNEL_9;
    job.payload.not_before_ms = 10u;
    job.payload.deadline_ms = 110u;
    job.payload.duration_ms = 50u;
    job.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_TX, FW_RADIO_PRIORITY_BACKGROUND);
    {
        struct fw_event invalid = job;

        invalid.payload.duration_ms = 101u;
        assert(apply(fw_radio_sm_handle, &radio, &invalid,
                     FW_RADIO_OFF, FW_EFFECT_NONE) == FW_SM_INVALID);
    }
    assert(apply(fw_radio_sm_handle, &radio, &job,
                 FW_RADIO_STARTING,
                 FW_EFFECT_RADIO_INITIALIZE) == FW_SM_APPLIED);

    result = operation_event(FW_MACHINE_RADIO,
                             FW_EVENT_RADIO_INIT_SUCCEEDED, 11u, 1u);
    assert(apply(fw_radio_sm_handle, &radio, &result,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    job.operation_id = 12u;
    job.generation = 2u;
    assert(apply(fw_radio_sm_handle, &radio, &job,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_NONE) == FW_SM_BUSY);

    job.operation_id = 13u;
    job.generation = 3u;
    job.payload.channel = FW_RADIO_CHANNEL_5;
    job.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_CLICK);
    assert(apply(fw_radio_sm_handle, &radio, &job,
                 FW_RADIO_CANCELLING,
                 FW_EFFECT_RADIO_CANCEL_JOB) == FW_SM_APPLIED);

    result.type = FW_EVENT_RADIO_JOB_CANCELLED;
    {
        struct fw_transition transition;

        assert(fw_radio_sm_handle(&radio, &result, &transition) ==
               FW_SM_APPLIED);
        assert(transition.new_state == FW_RADIO_CANCELLED_WAIT_RELEASE);
        assert(transition.effect.type == FW_EFFECT_RADIO_REPORT_RESULT);
        assert(transition.effect.owner == FW_MACHINE_DELIVERY);
        assert(transition.effect.operation_id == 11u);
        assert(transition.effect.payload.value ==
               FW_EVENT_RADIO_JOB_CANCELLED);
    }
    result.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_radio_sm_handle, &radio, &result,
                 FW_RADIO_RETUNING_5,
                 FW_EFFECT_RADIO_RETUNE) == FW_SM_APPLIED);
    result.operation_id = 13u;
    result.generation = 3u;
    result.type = FW_EVENT_RADIO_RETUNE_SUCCEEDED;
    assert(apply(fw_radio_sm_handle, &radio, &result,
                 FW_RADIO_BUSY_5,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    result.operation_id = 11u;
    result.generation = 1u;
    result.type = FW_EVENT_RADIO_JOB_COMPLETED;
    assert(apply(fw_radio_sm_handle, &radio, &result,
                 FW_RADIO_BUSY_5,
                 FW_EFFECT_NONE) == FW_SM_STALE);
    result.operation_id = 13u;
    result.generation = 3u;
    assert(apply(fw_radio_sm_handle, &radio, &result,
                 FW_RADIO_READY_5,
                 FW_EFFECT_RADIO_REPORT_RESULT) == FW_SM_APPLIED);
}

static void test_radio_handoff_freezes_generation_after_safe_boundary(void)
{
    struct fw_radio_handoff_sm handoff;
    struct fw_event event = operation_event(FW_MACHINE_RADIO,
                                            FW_EVENT_RADIO_PREEMPT_REQUESTED,
                                            80u, 1u);

    fw_radio_handoff_sm_init(&handoff);
    event.payload.value = 123u;
    assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                 FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY,
                 FW_EFFECT_RADIO_REQUEST_ABORT) == FW_SM_APPLIED);
    event.payload.value = 124u;
    assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                 FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(handoff.admission_cutoff == 124u);

    event.type = FW_EVENT_RADIO_SAFE_BOUNDARY;
    assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                 FW_RADIO_HANDOFF_SCHEDULING,
                 FW_EFFECT_RADIO_SCHEDULE_PENDING) == FW_SM_APPLIED);
    event.type = FW_EVENT_EFFECT_FAILED;
    event.payload.flags = FW_EVENT_FLAG_RETRYABLE;
    assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                 FW_RADIO_HANDOFF_WAIT_RETRY,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);

    event.type = FW_EVENT_RADIO_PREEMPT_REQUESTED;
    event.payload.value = 125u;
    assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                 FW_RADIO_HANDOFF_WAIT_RETRY,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(handoff.admission_cutoff == 124u);

    for (uint8_t attempt = 1u;
         attempt < FW_RADIO_HANDOFF_MAX_SCHEDULE_ATTEMPTS;
         attempt++) {
        event.type = FW_EVENT_TIMER_EXPIRED;
        assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                     FW_RADIO_HANDOFF_SCHEDULING,
                     FW_EFFECT_RADIO_SCHEDULE_PENDING) == FW_SM_APPLIED);
        event.type = FW_EVENT_EFFECT_FAILED;
        event.payload.flags = FW_EVENT_FLAG_RETRYABLE;
        if (attempt + 1u == FW_RADIO_HANDOFF_MAX_SCHEDULE_ATTEMPTS) {
            struct fw_transition transition;

            assert(fw_radio_handoff_sm_handle(&handoff, &event,
                                              &transition) == FW_SM_APPLIED);
            assert(transition.new_state == FW_RADIO_HANDOFF_IDLE);
            assert(transition.effect.type == FW_EFFECT_RADIO_CLEAR_ABORT);
            assert(transition.effect.payload.value == 124u);
        } else {
            assert(apply(fw_radio_handoff_sm_handle, &handoff, &event,
                         FW_RADIO_HANDOFF_WAIT_RETRY,
                         FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
        }
    }
}

static void test_radio_activity_priority_and_transition_trace(void)
{
    struct fw_radio_activity_capture capture = {
        .report_queue_used = 1u,
    };
    struct fw_radio_activity_runtime runtime;
    struct fw_radio_activity_decision decision;
    bool changed = false;

    fw_radio_activity_runtime_init(&runtime);
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_TX);
    assert(decision.c5_tx_allowed);
    assert(!decision.uwb_rx_allowed);

    capture.click_active = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_CLICK);
    assert(!decision.mesh_work_allowed);
    assert(!decision.c5_tx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(!decision.uwb_rx_allowed);
}

static void test_gateway_continuous_rx_blocks_competing_work(void)
{
    struct fw_radio_activity_capture capture = {
        .gateway_continuous_ch9 = true,
    };
    struct fw_radio_activity_runtime runtime;
    struct fw_radio_activity_decision decision;
    bool changed = false;

    fw_radio_activity_runtime_init(&runtime);
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_GATEWAY_RX);
    assert(!decision.mesh_work_allowed);
    assert(!decision.c5_tx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "gateway-rx") == 0);

    /* Pending work must not shadow the receiver owner on a later capture. */
    capture.rx_queue_used = 1u;
    capture.report_queue_used = 1u;
    capture.relay_tx_active = true;
    capture.route_waiting_tx_active = true;
    capture.ch9_ack_wait_active = true;
    capture.ch9_ack_send_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!changed);
    assert(decision.state == FW_RADIO_ACTIVITY_GATEWAY_RX);
    assert(!decision.mesh_work_allowed);
    assert(!decision.c5_tx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(decision.uwb_rx_allowed);
}

static void test_ch9_ack_custody_allows_rx_but_blocks_channel5_tx(void)
{
    struct fw_radio_activity_capture capture = {
        .ch9_ack_wait_active = true,
    };
    struct fw_radio_activity_runtime runtime;
    struct fw_radio_activity_decision decision;
    bool changed = false;

    fw_radio_activity_runtime_init(&runtime);
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(!decision.c5_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-wait") == 0);

    capture.ch9_ack_wait_active = false;
    capture.ch9_ack_send_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!changed);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(!decision.c5_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-send") == 0);

    capture.ch9_ack_send_pending = false;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_IDLE);
    assert(decision.mesh_work_allowed);
    assert(decision.c5_tx_allowed);
    assert(decision.uwb_rx_allowed);

    capture.ch9_ack_receive_eligible = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(decision.c5_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-rx-eligible") == 0);
}

static void test_causal_c5_response_only_overrides_queued_rx_work(void)
{
    struct fw_radio_activity_capture capture = {
        .rx_queue_used = 1u,
        .c5_tx_intent = FW_C5_TX_INTENT_CAUSAL_RESPONSE,
    };
    struct fw_radio_activity_runtime runtime;
    struct fw_radio_activity_decision decision;
    bool changed = false;

    /* The response was created by the queued frame itself, so queue occupancy
     * cannot veto that exact response and leave both sides waiting forever. */
    fw_radio_activity_runtime_init(&runtime);
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(changed);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(decision.c5_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(strcmp(decision.reason, "mesh-rx-causal-response") == 0);

    /* The default intent remains blocked, so unrelated Channel-5 producers
     * cannot borrow the synchronous-response exception. */
    capture.c5_tx_intent = FW_C5_TX_INTENT_BACKGROUND;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!changed);
    assert(!decision.c5_tx_allowed);
    assert(strcmp(decision.reason, "mesh-rx") == 0);

    /* A typed response overrides queue occupancy only. Every real owner and
     * higher-priority lane still wins when it is present in the same capture. */
    capture.c5_tx_intent = FW_C5_TX_INTENT_CAUSAL_RESPONSE;
    capture.ch9_ack_wait_active = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!decision.c5_tx_allowed);
    capture.ch9_ack_wait_active = false;

    capture.ch9_ack_send_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!decision.c5_tx_allowed);
    capture.ch9_ack_send_pending = false;

    capture.click_active = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!decision.c5_tx_allowed);
    capture.click_active = false;

    capture.survey_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!decision.c5_tx_allowed);
    capture.survey_pending = false;

    capture.gateway_continuous_ch9 = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, &changed) == 0);
    assert(!decision.c5_tx_allowed);
    capture.gateway_continuous_ch9 = false;
}

static void test_validated_survey_forward_only_overrides_live_ch9_ack_owner(void)
{
    struct fw_radio_activity_capture capture = {
        .ch9_ack_wait_active = true,
        .c5_tx_intent = FW_C5_TX_INTENT_BACKGROUND,
    };
    struct fw_radio_activity_runtime runtime;
    struct fw_radio_activity_decision decision;

    fw_radio_activity_runtime_init(&runtime);

    /* A retained report's live ACK deadline keeps every ordinary Channel-5
     * producer blocked, including the broader causal-response exception. */
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);
    capture.c5_tx_intent = FW_C5_TX_INTENT_CAUSAL_RESPONSE;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);

    /* The app may mint this intent only from the relay core's validated
     * gateway survey-forward action. It opens one control send without
     * weakening the ACK owner or its receive eligibility. */
    capture.c5_tx_intent = FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(decision.state == FW_RADIO_ACTIVITY_MESH_RX);
    assert(decision.c5_tx_allowed);
    assert(decision.mesh_work_allowed);
    assert(decision.uwb_rx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);

    capture.ch9_ack_wait_active = false;
    capture.ch9_ack_send_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(decision.c5_tx_allowed);

    /* The exception is intentionally narrower than a generic priority lane:
     * queued RX, clicks, survey RF, and continuous gateway RX still win. */
    capture.rx_queue_used = 1u;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);
    capture.rx_queue_used = 0u;

    capture.click_active = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);
    capture.click_active = false;

    capture.survey_pending = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);
    capture.survey_pending = false;

    capture.gateway_continuous_ch9 = true;
    assert(fw_radio_activity_decide(&capture, &runtime,
                                    &decision, NULL) == 0);
    assert(!decision.c5_tx_allowed);
}

static void test_radio_cancel_failure_enters_recovery(void)
{
    struct fw_radio_sm radio;
    struct fw_event active = operation_event(FW_MACHINE_RADIO,
                                             FW_EVENT_RADIO_REQUESTED,
                                             20u, 1u);
    struct fw_event pending;
    struct fw_event failure;
    struct fw_transition transition;

    fw_radio_sm_init(&radio);
    active.reply_to = FW_MACHINE_DELIVERY;
    active.payload.channel = FW_RADIO_CHANNEL_9;
    active.payload.not_before_ms = 1u;
    active.payload.deadline_ms = 101u;
    active.payload.duration_ms = 50u;
    active.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_BACKGROUND);
    assert(apply(fw_radio_sm_handle, &radio, &active,
                 FW_RADIO_STARTING,
                 FW_EFFECT_RADIO_INITIALIZE) == FW_SM_APPLIED);

    active.type = FW_EVENT_RADIO_INIT_SUCCEEDED;
    assert(apply(fw_radio_sm_handle, &radio, &active,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    pending = active;
    pending.type = FW_EVENT_RADIO_REQUESTED;
    pending.operation_id = 21u;
    pending.generation = 2u;
    pending.payload.channel = FW_RADIO_CHANNEL_5;
    pending.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_CLICK);
    assert(apply(fw_radio_sm_handle, &radio, &pending,
                 FW_RADIO_CANCELLING,
                 FW_EFFECT_RADIO_CANCEL_JOB) == FW_SM_APPLIED);

    failure = active;
    failure.type = FW_EVENT_RADIO_JOB_FAILED;
    assert(fw_radio_sm_handle(&radio, &failure, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_RECOVERING);
    assert(transition.effect.type == FW_EFFECT_RADIO_RECOVER);
    assert(transition.effect.operation_id == 20u);
    assert(transition.effect.generation == 1u);
    assert(transition.effect.owner == FW_MACHINE_DELIVERY);
    assert(radio.active.identity.active);
    assert(radio.pending.identity.active);
}

static void test_radio_recovery_completion_promotes_pending_after_report(void)
{
    struct fw_radio_sm radio;
    struct fw_event active = operation_event(FW_MACHINE_RADIO,
                                             FW_EVENT_RADIO_REQUESTED,
                                             30u, 1u);
    struct fw_event pending;
    struct fw_event event;
    struct fw_transition transition;

    fw_radio_sm_init(&radio);
    active.reply_to = FW_MACHINE_DELIVERY;
    active.payload.channel = FW_RADIO_CHANNEL_9;
    active.payload.not_before_ms = 1u;
    active.payload.deadline_ms = 101u;
    active.payload.duration_ms = 50u;
    active.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_BACKGROUND);
    assert(apply(fw_radio_sm_handle, &radio, &active,
                 FW_RADIO_STARTING,
                 FW_EFFECT_RADIO_INITIALIZE) == FW_SM_APPLIED);
    event = active;
    event.type = FW_EVENT_RADIO_INIT_SUCCEEDED;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    pending = active;
    pending.type = FW_EVENT_RADIO_REQUESTED;
    pending.operation_id = 31u;
    pending.generation = 2u;
    pending.payload.channel = FW_RADIO_CHANNEL_5;
    pending.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_CLICK);
    assert(apply(fw_radio_sm_handle, &radio, &pending,
                 FW_RADIO_CANCELLING,
                 FW_EFFECT_RADIO_CANCEL_JOB) == FW_SM_APPLIED);

    event.type = FW_EVENT_RADIO_JOB_FAILED;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_RECOVERING,
                 FW_EFFECT_RADIO_RECOVER) == FW_SM_APPLIED);
    event.type = FW_EVENT_RADIO_RECOVERY_RETRY;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_STARTING,
                 FW_EFFECT_RADIO_INITIALIZE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RADIO_INIT_SUCCEEDED;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    event.type = FW_EVENT_RADIO_JOB_COMPLETED;
    assert(fw_radio_sm_handle(&radio, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_CANCELLED_WAIT_RELEASE);
    assert(transition.effect.type == FW_EFFECT_RADIO_REPORT_RESULT);
    assert(transition.effect.operation_id == 30u);
    assert(transition.effect.owner == FW_MACHINE_DELIVERY);
    assert(transition.effect.payload.value == FW_EVENT_RADIO_JOB_COMPLETED);
    assert(radio.active.identity.active);
    assert(radio.pending.identity.active);

    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(fw_radio_sm_handle(&radio, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_RETUNING_5);
    assert(transition.effect.type == FW_EFFECT_RADIO_RETUNE);
    assert(transition.effect.operation_id == 31u);
    assert(transition.effect.owner == FW_MACHINE_DELIVERY);
    assert(!radio.pending.identity.active);
}

static void test_radio_recovery_exhaustion_reports_both_owners(void)
{
    struct fw_radio_sm radio;
    struct fw_event active = operation_event(FW_MACHINE_RADIO,
                                             FW_EVENT_RADIO_REQUESTED,
                                             40u, 1u);
    struct fw_event pending;
    struct fw_event event;
    struct fw_transition transition;

    fw_radio_sm_init(&radio);
    active.reply_to = FW_MACHINE_DELIVERY;
    active.payload.channel = FW_RADIO_CHANNEL_9;
    active.payload.not_before_ms = 1u;
    active.payload.deadline_ms = 101u;
    active.payload.duration_ms = 50u;
    active.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_BACKGROUND);
    assert(apply(fw_radio_sm_handle, &radio, &active,
                 FW_RADIO_STARTING,
                 FW_EFFECT_RADIO_INITIALIZE) == FW_SM_APPLIED);
    event = active;
    event.type = FW_EVENT_RADIO_INIT_SUCCEEDED;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_BUSY_9,
                 FW_EFFECT_RADIO_RUN_JOB) == FW_SM_APPLIED);

    pending = active;
    pending.type = FW_EVENT_RADIO_REQUESTED;
    pending.operation_id = 41u;
    pending.generation = 2u;
    pending.payload.channel = FW_RADIO_CHANNEL_5;
    pending.payload.value = FW_RADIO_REQUEST_VALUE(
        FW_RADIO_MODE_RX, FW_RADIO_PRIORITY_CLICK);
    assert(apply(fw_radio_sm_handle, &radio, &pending,
                 FW_RADIO_CANCELLING,
                 FW_EFFECT_RADIO_CANCEL_JOB) == FW_SM_APPLIED);

    event.type = FW_EVENT_RADIO_JOB_FAILED;
    assert(apply(fw_radio_sm_handle, &radio, &event,
                 FW_RADIO_RECOVERING,
                 FW_EFFECT_RADIO_RECOVER) == FW_SM_APPLIED);
    event.type = FW_EVENT_RADIO_RECOVERY_EXHAUSTED;
    assert(fw_radio_sm_handle(&radio, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_CANCELLED_WAIT_RELEASE);
    assert(transition.effect.type == FW_EFFECT_RADIO_REPORT_RESULT);
    assert(transition.effect.operation_id == 40u);
    assert(transition.effect.owner == FW_MACHINE_DELIVERY);
    assert(transition.effect.payload.value ==
           FW_EVENT_RADIO_RECOVERY_EXHAUSTED);
    assert(radio.active.identity.active);
    assert(radio.pending.identity.active);

    /* The second owner gets its own report only after the first is accepted. */
    event.operation_id = 40u;
    event.generation = 1u;
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(fw_radio_sm_handle(&radio, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_CANCELLED_WAIT_RELEASE);
    assert(transition.effect.type == FW_EFFECT_RADIO_REPORT_RESULT);
    assert(transition.effect.operation_id == 41u);
    assert(transition.effect.generation == 2u);
    assert(transition.effect.owner == FW_MACHINE_DELIVERY);
    assert(transition.effect.payload.value ==
           FW_EVENT_RADIO_RECOVERY_EXHAUSTED);
    assert(radio.active.identity.active);
    assert(!radio.pending.identity.active);

    event.operation_id = 41u;
    event.generation = 2u;
    assert(fw_radio_sm_handle(&radio, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_RADIO_OFF);
    assert(transition.effect.type == FW_EFFECT_NONE);
    assert(!radio.active.identity.active);
    assert(!radio.pending.identity.active);
}

static void test_delivery_loss_stays_owned_until_annotated_packet_is_sent(void)
{
    struct fw_delivery_loss_state state;
    struct fw_delivery_loss_store_result store;
    struct fw_delivery_loss_attach_result attach;
    struct mesh_outbound first = {0};
    struct mesh_outbound second = {0};

    first.packet.msg_type = MSG_MESH_DATA;
    first.packet.src_id = 1u;
    first.packet.dst_id = 2u;
    first.packet.session_id = 3u;
    first.packet.seq = 4u;
    second = first;
    second.packet.seq = 5u;

    fw_delivery_loss_init(&state);
    fw_delivery_loss_note_store(&state, true, &first, &second, &store);
    assert(store.replaced_existing);
    assert(store.lost_count == 1u);
    assert(fw_delivery_loss_attach(&state, &second, &attach) == PROTO_OK);
    assert(attach.tlv_attached);
    fw_delivery_loss_note_sent(&state, &first);
    assert(fw_delivery_loss_count(&state) == 1u);
    fw_delivery_loss_note_sent(&state, &second);
    assert(fw_delivery_loss_count(&state) == 0u);
}

static void test_button_gesture_machine(void)
{
    struct fw_button_sm button;
    struct fw_event event = operation_event(FW_MACHINE_BUTTON,
                                            FW_EVENT_BUTTON_PRESSED,
                                            0u, 0u);

    fw_button_sm_init(&button);
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_DEBOUNCE, FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_DEBOUNCE_CONFIRMED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_PRESSED, FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_LONG_PRESS_DETECTED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_WAIT_LONG_RELEASE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    event.type = FW_EVENT_BUTTON_RELEASED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_SELF_TEST_ARMED, FW_EFFECT_START_TIMER) ==
           FW_SM_APPLIED);
    event.type = FW_EVENT_BUTTON_PRESSED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_SELF_TEST_CONFIRM, FW_EFFECT_CANCEL_TIMER) ==
           FW_SM_APPLIED);
    event.type = FW_EVENT_BUTTON_RELEASED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_SELF_TEST, FW_EFFECT_PUBLISH_EVENT) ==
           FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_button_sm_handle, &button, &event,
                 FW_BUTTON_IDLE, FW_EFFECT_NONE) == FW_SM_APPLIED);
}

static void test_click_machine_counts_only_rf_and_rejects_stale_results(void)
{
    struct fw_click_sm click;
    struct fw_event event = operation_event(FW_MACHINE_CLICK,
                                            FW_EVENT_START, 100u, 1u);

    fw_click_sm_init(&click);
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_CREATE, FW_EFFECT_CLICK_CREATE) == FW_SM_APPLIED);
    event.type = FW_EVENT_CLICK_CREATED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_POLITENESS,
                 FW_EFFECT_CLICK_CHECK_POLITENESS) == FW_SM_APPLIED);
    event.type = FW_EVENT_CHANNEL_CLEAR;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_CLICK_SEND_WAKE) == FW_SM_APPLIED);
    assert(click.attempts_started == 0u);
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(click.attempts_started == 1u);
    event.type = FW_EVENT_WAKE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_DISCOVER, FW_EFFECT_CLICK_DISCOVER) == FW_SM_APPLIED);
    event.type = FW_EVENT_DISCOVERY_COMPLETED;
    event.payload.count = 3u;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_SCHEDULE,
                 FW_EFFECT_CLICK_SEND_SCHEDULE) == FW_SM_APPLIED);
    event.type = FW_EVENT_SCHEDULE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_RANGE, FW_EFFECT_CLICK_RANGE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RANGE_COMPLETED;
    event.payload.count = 3u;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_SUCCESS, FW_EFFECT_CLICK_CLEANUP) == FW_SM_APPLIED);

    event.generation = 2u;
    event.operation_id = 101u;
    event.type = FW_EVENT_START;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_CREATE, FW_EFFECT_CLICK_CREATE) == FW_SM_APPLIED);
    event.generation = 1u;
    event.operation_id = 100u;
    event.type = FW_EVENT_RANGE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_CREATE, FW_EFFECT_NONE) == FW_SM_STALE);
}

static void test_click_retry_keeps_identity_and_threshold_is_event_owned(void)
{
    struct fw_click_sm click;
    struct fw_event event = operation_event(FW_MACHINE_CLICK,
                                            FW_EVENT_START, 110u, 7u);

    fw_click_sm_init(&click);
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_CREATE, FW_EFFECT_CLICK_CREATE) == FW_SM_APPLIED);
    event.type = FW_EVENT_CLICK_CREATED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_POLITENESS,
                 FW_EFFECT_CLICK_CHECK_POLITENESS) == FW_SM_APPLIED);
    event.type = FW_EVENT_CHANNEL_CLEAR;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_CLICK_SEND_WAKE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RADIO_JOB_FAILED;
    event.payload.flags = FW_EVENT_FLAG_RETRYABLE;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_RETRY, FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    assert(click.identity.active);
    assert(click.identity.operation_id == 110u);
    assert(click.identity.generation == 7u);

    event.payload = (struct fw_event_payload){0};
    event.type = FW_EVENT_RETRY_ALLOWED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_POLITENESS,
                 FW_EFFECT_CLICK_CHECK_POLITENESS) == FW_SM_APPLIED);
    event.type = FW_EVENT_CHANNEL_CLEAR;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_CLICK_SEND_WAKE) == FW_SM_APPLIED);
    event.type = FW_EVENT_WAKE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_DISCOVER, FW_EFFECT_CLICK_DISCOVER) == FW_SM_APPLIED);
    event.type = FW_EVENT_DISCOVERY_COMPLETED;
    event.payload.count = 3u;
    event.payload.value = 4u;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_RELEASE,
                 FW_EFFECT_CLICK_SEND_RELEASE) == FW_SM_APPLIED);

    event.type = FW_EVENT_RELEASE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_RETRY, FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_RETRY_ALLOWED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_POLITENESS,
                 FW_EFFECT_CLICK_CHECK_POLITENESS) == FW_SM_APPLIED);
    event.type = FW_EVENT_CHANNEL_CLEAR;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_CLICK_SEND_WAKE) == FW_SM_APPLIED);
    event.type = FW_EVENT_WAKE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_DISCOVER, FW_EFFECT_CLICK_DISCOVER) == FW_SM_APPLIED);
    event.type = FW_EVENT_DISCOVERY_COMPLETED;
    event.payload.count = 4u;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_SCHEDULE,
                 FW_EFFECT_CLICK_SEND_SCHEDULE) == FW_SM_APPLIED);
    event.type = FW_EVENT_SCHEDULE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_RANGE, FW_EFFECT_CLICK_RANGE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RANGE_COMPLETED;
    event.payload.count = 1u;
    event.payload.value = 1u;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_SUCCESS, FW_EFFECT_CLICK_CLEANUP) == FW_SM_APPLIED);
}

static void test_click_rf_started_is_bound_to_wake_child(void)
{
    struct fw_click_sm click;
    struct fw_event event = operation_event(FW_MACHINE_CLICK,
                                            FW_EVENT_START, 115u, 8u);

    fw_click_sm_init(&click);
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_CREATE, FW_EFFECT_CLICK_CREATE) == FW_SM_APPLIED);
    event.type = FW_EVENT_CLICK_CREATED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_POLITENESS,
                 FW_EFFECT_CLICK_CHECK_POLITENESS) == FW_SM_APPLIED);
    event.type = FW_EVENT_CHANNEL_CLEAR;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_CLICK_SEND_WAKE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(click.attempts_started == 1u);
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_WAKE, FW_EFFECT_NONE) == FW_SM_IGNORED);
    assert(click.attempts_started == 1u);

    event.type = FW_EVENT_WAKE_COMPLETED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_DISCOVER, FW_EFFECT_CLICK_DISCOVER) == FW_SM_APPLIED);
    /* Same operation and generation do not make a late child event current. */
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_click_sm_handle, &click, &event,
                 FW_CLICK_DISCOVER, FW_EFFECT_NONE) == FW_SM_IGNORED);
    assert(click.attempts_started == 1u);
}

static void test_anchor_click_retains_result_until_custody_release(void)
{
    struct fw_anchor_click_sm anchor;
    struct fw_event event = operation_event(FW_MACHINE_ANCHOR_CLICK,
                                            FW_EVENT_WAKE_CLAIM_ACCEPTED,
                                            200u, 1u);

    fw_anchor_click_sm_init(&anchor);
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_CLAIMED,
                 FW_EFFECT_ANCHOR_WAIT_SCHEDULE) == FW_SM_APPLIED);
    event.type = FW_EVENT_DISCOVER_RECEIVED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_DISCOVERY_REPLIED,
                 FW_EFFECT_ANCHOR_SEND_DISCOVERY_REPLY) == FW_SM_APPLIED);
    event.type = FW_EVENT_SCHEDULE_RECEIVED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_SCHEDULED,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_RANGE_DUE;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_RANGING,
                 FW_EFFECT_ANCHOR_RANGE) == FW_SM_APPLIED);
    event.type = FW_EVENT_RESULT_RETAINED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_RESULT_OWNED,
                 FW_EFFECT_ANCHOR_RETAIN_RESULT) == FW_SM_APPLIED);
    assert(anchor.identity.active);
    event.type = FW_EVENT_CANCEL;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_DEADLINE_EXPIRED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_RADIO_JOB_FAILED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    assert(anchor.identity.active);
    event.type = FW_EVENT_RESULT_CUSTODY_RELEASED;
    assert(apply(fw_anchor_click_sm_handle, &anchor, &event,
                 FW_ANCHOR_CLICK_IDLE,
                 FW_EFFECT_ANCHOR_CLEANUP) == FW_SM_APPLIED);
    assert(!anchor.identity.active);
}

static void test_route_and_connection_are_independent(void)
{
    struct fw_route_sm route;
    struct fw_connection_sm connection;
    struct fw_event route_event = operation_event(FW_MACHINE_ROUTE,
                                                  FW_EVENT_ROUTE_NEEDED,
                                                  300u, 1u);
    struct fw_event connection_event = operation_event(
        FW_MACHINE_CONNECTION, FW_EVENT_CONNECTION_NEEDED, 301u, 1u);

    fw_route_sm_init(&route);
    fw_connection_sm_init(&connection);
    assert(apply(fw_route_sm_handle, &route, &route_event,
                 FW_ROUTE_DIRECT_PROBE,
                 FW_EFFECT_ROUTE_DIRECT_PROBE) == FW_SM_APPLIED);
    route_event.type = FW_EVENT_DIRECT_PROBE_FAILED;
    assert(apply(fw_route_sm_handle, &route, &route_event,
                 FW_ROUTE_DISCOVERING,
                 FW_EFFECT_ROUTE_DISCOVER) == FW_SM_APPLIED);
    route_event.type = FW_EVENT_ROUTE_FOUND;
    route_event.payload.subject_id = 0xabcdu;
    assert(apply(fw_route_sm_handle, &route, &route_event,
                 FW_ROUTE_READY, FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);

    connection_event.payload.subject_id = route.parent_id;
    assert(apply(fw_connection_sm_handle, &connection, &connection_event,
                 FW_CONNECTION_NEGOTIATING,
                 FW_EFFECT_CONNECTION_NEGOTIATE) == FW_SM_APPLIED);
    connection_event.type = FW_EVENT_PROPOSAL_ACCEPTED;
    assert(apply(fw_connection_sm_handle, &connection, &connection_event,
                 FW_CONNECTION_ACTIVE,
                 FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);
    for (uint8_t i = 0u; i < FW_CONNECTION_MISSED_RX_LIMIT - 1u; i++) {
        connection_event.type = FW_EVENT_RECEIVE_TURN_MISSED;
        assert(apply(fw_connection_sm_handle, &connection, &connection_event,
                     FW_CONNECTION_ACTIVE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    }
    assert(route.state == FW_ROUTE_READY);
    assert(apply(fw_connection_sm_handle, &connection, &connection_event,
                 FW_CONNECTION_STALE,
                 FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);
    assert(route.state == FW_ROUTE_READY);
}

static void test_connection_stale_repair_retires_old_generation(void)
{
    struct fw_connection_sm connection;
    struct fw_event event = operation_event(
        FW_MACHINE_CONNECTION, FW_EVENT_CONNECTION_NEEDED, 310u, 1u);

    assert(FW_CONNECTION_MISSED_RX_LIMIT == 8u);
    fw_connection_sm_init(&connection);
    event.payload.subject_id = 0x1111u;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_NEGOTIATING,
                 FW_EFFECT_CONNECTION_NEGOTIATE) == FW_SM_APPLIED);
    event.type = FW_EVENT_PROPOSAL_ACCEPTED;
    event.payload.value = 7u;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_ACTIVE,
                 FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);

    for (uint8_t missed = 0u;
         missed < FW_CONNECTION_MISSED_RX_LIMIT - 1u; missed++) {
        event.type = FW_EVENT_RECEIVE_TURN_MISSED;
        assert(apply(fw_connection_sm_handle, &connection, &event,
                     FW_CONNECTION_ACTIVE, FW_EFFECT_NONE) == FW_SM_APPLIED);
    }
    assert(connection.consecutive_missed_rx ==
           FW_CONNECTION_MISSED_RX_LIMIT - 1u);
    event.type = FW_EVENT_RECEIVE_TURN_MISSED;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_STALE,
                 FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);
    assert(!connection.identity.active);
    assert(connection.identity.operation_id == 310u);
    assert(connection.identity.generation == 1u);

    /* Delayed work from the retired generation cannot repair the stale link. */
    event.type = FW_EVENT_CONNECTION_NEEDED;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_STALE, FW_EFFECT_NONE) == FW_SM_STALE);
    event.type = FW_EVENT_CONNECTION_EVENT_COMPLETED;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_STALE, FW_EFFECT_NONE) == FW_SM_STALE);

    event.operation_id = 311u;
    event.generation = 2u;
    event.payload.subject_id = 0x2222u;
    event.type = FW_EVENT_CONNECTION_NEEDED;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_NEGOTIATING,
                 FW_EFFECT_CONNECTION_NEGOTIATE) == FW_SM_APPLIED);
    assert(connection.identity.active);
    assert(connection.identity.operation_id == 311u);
    assert(connection.identity.generation == 2u);
    assert(connection.peer_id == 0x2222u);
    assert(connection.consecutive_missed_rx == 0u);

    /* The old completion cannot mutate the new negotiation. */
    event.operation_id = 310u;
    event.generation = 1u;
    event.type = FW_EVENT_PROPOSAL_ACCEPTED;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_NEGOTIATING, FW_EFFECT_NONE) == FW_SM_STALE);
    event.operation_id = 311u;
    event.generation = 2u;
    event.payload.value = 9u;
    assert(apply(fw_connection_sm_handle, &connection, &event,
                 FW_CONNECTION_ACTIVE,
                 FW_EFFECT_PUBLISH_EVENT) == FW_SM_APPLIED);
}

static void test_delivery_keeps_custody_and_attempt_accounting_exact(void)
{
    struct fw_delivery_sm delivery;
    struct fw_event event = operation_event(FW_MACHINE_DELIVERY,
                                            FW_EVENT_PACKET_OWNED,
                                            400u, 1u);

    fw_delivery_sm_init(&delivery);
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_ROUTE,
                 FW_EFFECT_DELIVERY_WAIT_ROUTE) == FW_SM_APPLIED);
    assert(delivery.owns_custody);
    event.type = FW_EVENT_ROUTE_READY;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_CONNECTION,
                 FW_EFFECT_DELIVERY_WAIT_CONNECTION) == FW_SM_APPLIED);
    event.type = FW_EVENT_CONNECTION_READY;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_TX,
                 FW_EFFECT_DELIVERY_SEND) == FW_SM_APPLIED);
    event.type = FW_EVENT_RF_DEFERRED;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_TX, FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(delivery.attempts_started == 0u);
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_ACK,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    assert(delivery.attempts_started == 1u);
    event.type = FW_EVENT_ACK_TIMED_OUT;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_RETRY,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    assert(delivery.owns_custody);
    event.type = FW_EVENT_RETRY_ALLOWED;
    event.payload.flags = FW_EVENT_FLAG_PATH_USABLE;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_TX,
                 FW_EFFECT_DELIVERY_SEND) == FW_SM_APPLIED);
    event.type = FW_EVENT_RF_STARTED;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_WAIT_ACK,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_HOP_ACK_RECEIVED;
    assert(apply(fw_delivery_sm_handle, &delivery, &event,
                 FW_DELIVERY_TRANSFERRED,
                 FW_EFFECT_DELIVERY_TRANSFER_CUSTODY) == FW_SM_APPLIED);
    assert(!delivery.owns_custody);
}

static void test_gateway_uwb_and_ble_flow_control_are_separate(void)
{
    struct fw_gateway_uwb_sm uwb;
    struct fw_host_link_sm host;
    struct fw_event event = operation_event(FW_MACHINE_GATEWAY_UWB,
                                            FW_EVENT_GATEWAY_BATCH_RECEIVED,
                                            0u, 0u);

    fw_gateway_uwb_sm_init(&uwb);
    fw_host_link_sm_init(&host);
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_RECEIVE_BATCH,
                 FW_EFFECT_GATEWAY_VALIDATE_BATCH) == FW_SM_APPLIED);

    event.target = FW_MACHINE_HOST_LINK;
    event.type = FW_EVENT_BLE_BLOCKED;
    assert(apply(fw_host_link_sm_handle, &host, &event,
                 FW_HOST_LINK_BLOCKED, FW_EFFECT_NONE) == FW_SM_APPLIED);
    event.type = FW_EVENT_HOST_ITEM_QUEUED;
    assert(apply(fw_host_link_sm_handle, &host, &event,
                 FW_HOST_LINK_BLOCKED, FW_EFFECT_NONE) == FW_SM_APPLIED);
    event.type = FW_EVENT_BLE_READY;
    assert(apply(fw_host_link_sm_handle, &host, &event,
                 FW_HOST_LINK_SENDING, FW_EFFECT_HOST_SEND_ITEM) ==
           FW_SM_APPLIED);
    event.type = FW_EVENT_HOST_ITEM_SENT;
    assert(apply(fw_host_link_sm_handle, &host, &event,
                 FW_HOST_LINK_READY, FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(uwb.state == FW_GATEWAY_UWB_RECEIVE_BATCH);

    event.target = FW_MACHINE_GATEWAY_UWB;
    event.type = FW_EVENT_GATEWAY_BATCH_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_ACCEPT_BATCH,
                 FW_EFFECT_GATEWAY_ACCEPT_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_HOST_SEND_ITEM) == FW_SM_APPLIED);
    assert(uwb.host_item_pending);

    /* BLE flow-control events belong to the host-link machine. Neither a
     * blocked link nor a ready notification proves host delivery. */
    event.type = FW_EVENT_BLE_BLOCKED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_BLE_READY;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);

    /* The GUI receipt cannot arrive before transport completion. */
    event.type = FW_EVENT_HOST_ITEM_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_HOST_ITEM_SENT;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);

    event.type = FW_EVENT_BLE_BLOCKED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_BLE_READY;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_HOST_ITEM_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_SEND_ACK,
                 FW_EFFECT_GATEWAY_SEND_ACK) == FW_SM_APPLIED);
    assert(uwb.host_item_pending);
    event.type = FW_EVENT_GATEWAY_ACK_SENT;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_RETIRE_HOST_ITEM,
                 FW_EFFECT_HOST_RETIRE_ITEM) == FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_LISTEN_9,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(!uwb.host_item_pending);
}

static void test_gateway_uwb_failure_and_reset_never_ack(void)
{
    struct fw_gateway_uwb_sm uwb;
    struct fw_transition transition;
    struct fw_event event = operation_event(FW_MACHINE_GATEWAY_UWB,
                                            FW_EVENT_GATEWAY_BATCH_RECEIVED,
                                            0u, 0u);

    fw_gateway_uwb_sm_init(&uwb);
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_RECEIVE_BATCH,
                 FW_EFFECT_GATEWAY_VALIDATE_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_OPERATION_FAILED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_LISTEN_9,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);

    event.type = FW_EVENT_GATEWAY_BATCH_RECEIVED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_RECEIVE_BATCH,
                 FW_EFFECT_GATEWAY_VALIDATE_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_GATEWAY_BATCH_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_ACCEPT_BATCH,
                 FW_EFFECT_GATEWAY_ACCEPT_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_HOST_SEND_ITEM) == FW_SM_APPLIED);

    /* A failed ACK does not transfer upstream custody. Only the volatile
     * gateway reservation is discarded, so the source can retry. */
    event.type = FW_EVENT_HOST_ITEM_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_HOST_ITEM_SENT;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_GUI_RECEIPT,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);
    event.type = FW_EVENT_HOST_ITEM_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_SEND_ACK,
                 FW_EFFECT_GATEWAY_SEND_ACK) == FW_SM_APPLIED);
    event.type = FW_EVENT_OPERATION_FAILED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_LISTEN_9,
                 FW_EFFECT_NONE) == FW_SM_APPLIED);
    assert(!uwb.host_item_pending);

    /* Re-admit the same source retry and verify reset clears only volatile
     * state without producing a gateway ACK. */
    event.type = FW_EVENT_GATEWAY_BATCH_RECEIVED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_RECEIVE_BATCH,
                 FW_EFFECT_GATEWAY_VALIDATE_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_GATEWAY_BATCH_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_ACCEPT_BATCH,
                 FW_EFFECT_GATEWAY_ACCEPT_BATCH) == FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_WAIT_HOST_ITEM,
                 FW_EFFECT_HOST_SEND_ITEM) == FW_SM_APPLIED);

    event.type = FW_EVENT_RESET;
    assert(fw_gateway_uwb_sm_handle(&uwb, &event, &transition) ==
           FW_SM_APPLIED);
    assert(transition.new_state == FW_GATEWAY_UWB_LISTEN_9);
    assert(transition.effect.type == FW_EFFECT_NONE);
    assert(!uwb.host_item_pending);

    /* A stale transport or GUI receipt after reset cannot authorize an ACK. */
    event.type = FW_EVENT_HOST_ITEM_SENT;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_LISTEN_9,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_HOST_ITEM_ACCEPTED;
    assert(apply(fw_gateway_uwb_sm_handle, &uwb, &event,
                 FW_GATEWAY_UWB_LISTEN_9,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
}

static void test_enumeration_survey_and_pair_lifecycles(void)
{
    struct fw_enumeration_sm enumeration;
    struct fw_survey_sm survey;
    struct fw_pair_coordinator_sm pair_coordinator;
    struct fw_survey_pair_sm pair;
    struct fw_event event = operation_event(FW_MACHINE_ENUMERATION,
                                            FW_EVENT_START, 500u, 1u);

    fw_enumeration_sm_init(&enumeration);
    fw_survey_sm_init(&survey);
    fw_pair_coordinator_sm_init(&pair_coordinator);
    fw_survey_pair_sm_init(&pair);
    assert(apply(fw_enumeration_sm_handle, &enumeration, &event,
                 FW_ENUMERATION_SEND_CLAIM,
                 FW_EFFECT_ENUM_SEND_CLAIM) == FW_SM_APPLIED);
    event.type = FW_EVENT_CLAIM_SENT;
    assert(apply(fw_enumeration_sm_handle, &enumeration, &event,
                 FW_ENUMERATION_COLLECT_RESPONSES,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_RESPONSE_WINDOW_CLOSED;
    assert(apply(fw_enumeration_sm_handle, &enumeration, &event,
                 FW_ENUMERATION_FREEZE_TABLE,
                 FW_EFFECT_ENUM_FREEZE_RESPONSES) == FW_SM_APPLIED);
    event.type = FW_EVENT_RESPONSES_FROZEN;
    assert(apply(fw_enumeration_sm_handle, &enumeration, &event,
                 FW_ENUMERATION_SEND_TABLE,
                 FW_EFFECT_ENUM_SEND_TABLE) == FW_SM_APPLIED);
    event.type = FW_EVENT_TABLE_SENT;
    assert(apply(fw_enumeration_sm_handle, &enumeration, &event,
                 FW_ENUMERATION_COMPLETE,
                 FW_EFFECT_ENUM_COMPLETE) == FW_SM_APPLIED);

    event = operation_event(FW_MACHINE_SURVEY, FW_EVENT_START, 501u, 1u);
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_SEND_CONFIG,
                 FW_EFFECT_SURVEY_SEND_CONFIG) == FW_SM_APPLIED);
    event.type = FW_EVENT_CONFIG_SENT;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_DISCOVERY,
                 FW_EFFECT_SURVEY_BEGIN_DISCOVERY) == FW_SM_APPLIED);
    event.type = FW_EVENT_DISCOVERY_ROUNDS_COMPLETED;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_COLLECT_REPORTS,
                 FW_EFFECT_SURVEY_COLLECT_REPORTS) == FW_SM_APPLIED);
    event.type = FW_EVENT_REPORT_WINDOW_CLOSED;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_BUILD_GRAPH,
                 FW_EFFECT_SURVEY_BUILD_GRAPH) == FW_SM_APPLIED);
    event.type = FW_EVENT_GRAPH_BUILT;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_SELECT_PAIRS,
                 FW_EFFECT_SURVEY_SELECT_PAIR) == FW_SM_APPLIED);
    event.type = FW_EVENT_NO_PAIR_AVAILABLE;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_PUBLISH,
                 FW_EFFECT_SURVEY_PUBLISH) == FW_SM_APPLIED);
    event.type = FW_EVENT_SURVEY_COMPLETE;
    event.payload.flags = 0u;
    assert(apply(fw_survey_sm_handle, &survey, &event,
                 FW_SURVEY_PARTIAL,
                 FW_EFFECT_SURVEY_PUBLISH_PARTIAL) == FW_SM_APPLIED);

    event = operation_event(FW_MACHINE_PAIR_COORDINATOR,
                            FW_EVENT_START, 503u, 1u);
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_PREPARE_INITIATOR,
                 FW_EFFECT_PAIR_PREPARE_INITIATOR) == FW_SM_APPLIED);
    event.type = FW_EVENT_ACTION_ACCEPTED;
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_PREPARE_RESPONDER,
                 FW_EFFECT_PAIR_PREPARE_RESPONDER) == FW_SM_APPLIED);
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_START_RESPONDER,
                 FW_EFFECT_PAIR_START_RESPONDER) == FW_SM_APPLIED);
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_START_INITIATOR,
                 FW_EFFECT_PAIR_START_INITIATOR) == FW_SM_APPLIED);
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_WAIT_RESULT,
                 FW_EFFECT_PAIR_WAIT_RESULT) == FW_SM_APPLIED);
    event.type = FW_EVENT_PAIR_RESULT_RECEIVED;
    assert(apply(fw_pair_coordinator_sm_handle, &pair_coordinator, &event,
                 FW_PAIR_COORDINATOR_COMPLETE,
                 FW_EFFECT_PAIR_COMPLETE) == FW_SM_APPLIED);

    event = operation_event(FW_MACHINE_SURVEY_PAIR,
                            FW_EVENT_START, 502u, 1u);
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_PREPARED,
                 FW_EFFECT_PAIR_PREPARE) == FW_SM_APPLIED);
    event.type = FW_EVENT_PAIR_PREPARED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_ARMED,
                 FW_EFFECT_PAIR_ARM_START) == FW_SM_APPLIED);
    event.type = FW_EVENT_PAIR_START_ARMED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_WAIT_START,
                 FW_EFFECT_START_TIMER) == FW_SM_APPLIED);
    event.type = FW_EVENT_PAIR_START_DUE;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RANGE,
                 FW_EFFECT_PAIR_RANGE) == FW_SM_APPLIED);
    event.type = FW_EVENT_PAIR_RANGE_COMPLETED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RESULT_OWNED,
                 FW_EFFECT_PAIR_RETAIN_RESULT) == FW_SM_APPLIED);
    event.type = FW_EVENT_CANCEL;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_DEADLINE_EXPIRED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_PAIR_ABORTED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    event.type = FW_EVENT_RADIO_JOB_FAILED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_RESULT_OWNED,
                 FW_EFFECT_NONE) == FW_SM_IGNORED);
    assert(pair.identity.active);
    event.type = FW_EVENT_RESULT_CUSTODY_RELEASED;
    assert(apply(fw_survey_pair_sm_handle, &pair, &event,
                 FW_SURVEY_PAIR_COMPLETE,
                 FW_EFFECT_PAIR_COMPLETE) == FW_SM_APPLIED);
}

int main(void)
{
    test_radio_is_single_owner_and_preempts_only_upward();
    test_radio_handoff_freezes_generation_after_safe_boundary();
    test_radio_activity_priority_and_transition_trace();
    test_gateway_continuous_rx_blocks_competing_work();
    test_ch9_ack_custody_allows_rx_but_blocks_channel5_tx();
    test_causal_c5_response_only_overrides_queued_rx_work();
    test_validated_survey_forward_only_overrides_live_ch9_ack_owner();
    test_radio_cancel_failure_enters_recovery();
    test_radio_recovery_completion_promotes_pending_after_report();
    test_radio_recovery_exhaustion_reports_both_owners();
    test_delivery_loss_stays_owned_until_annotated_packet_is_sent();
    test_button_gesture_machine();
    test_click_machine_counts_only_rf_and_rejects_stale_results();
    test_click_retry_keeps_identity_and_threshold_is_event_owned();
    test_click_rf_started_is_bound_to_wake_child();
    test_anchor_click_retains_result_until_custody_release();
    test_route_and_connection_are_independent();
    test_connection_stale_repair_retires_old_generation();
    test_delivery_keeps_custody_and_attempt_accounting_exact();
    test_gateway_uwb_and_ble_flow_control_are_separate();
    test_gateway_uwb_failure_and_reset_never_ack();
    test_enumeration_survey_and_pair_lifecycles();
    return 0;
}
