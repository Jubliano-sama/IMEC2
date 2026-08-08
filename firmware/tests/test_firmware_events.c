#include "firmware_events.h"
#include "firmware_state_machines.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

struct callback_capture {
    struct fw_event_dispatcher *dispatcher;
    struct fw_effect last_effect;
    struct fw_transition last_transition;
    size_t effects;
    size_t traces;
    int reentrant_result;
};

static struct fw_event button_event(enum fw_event_type type)
{
    return (struct fw_event) {
        .target = FW_MACHINE_BUTTON,
        .source = FW_EVENT_SOURCE_ISR,
        .type = type,
    };
}

static struct fw_event radio_request_event(void)
{
    return (struct fw_event) {
        .operation_id = 1u,
        .generation = 1u,
        .target = FW_MACHINE_RADIO,
        .reply_to = FW_MACHINE_DELIVERY,
        .source = FW_EVENT_SOURCE_SERVICE,
        .type = FW_EVENT_RADIO_REQUESTED,
        .payload = {
            .not_before_ms = 10u,
            .deadline_ms = 110u,
            .duration_ms = 50u,
            .channel = FW_RADIO_CHANNEL_5,
            .value = FW_RADIO_REQUEST_VALUE(FW_RADIO_MODE_TX,
                                             FW_RADIO_PRIORITY_BACKGROUND),
        },
    };
}

static struct fw_event delivery_ack_event(enum fw_event_type type)
{
    return (struct fw_event) {
        .operation_id = 1u,
        .generation = 1u,
        .target = FW_MACHINE_DELIVERY,
        .source = type == FW_EVENT_ACK_TIMED_OUT ?
                      FW_EVENT_SOURCE_TIMER : FW_EVENT_SOURCE_RADIO,
        .type = type,
    };
}

static void capture_effect(const struct fw_effect *effect, void *context)
{
    struct callback_capture *capture = context;

    capture->last_effect = *effect;
    capture->effects++;
    capture->reentrant_result =
        fw_event_dispatcher_dispatch_one(capture->dispatcher, NULL);
}

static void capture_trace(const struct fw_event *event,
                          const struct fw_transition *transition,
                          void *context)
{
    struct callback_capture *capture = context;

    (void)event;
    capture->last_transition = *transition;
    capture->traces++;
}

static void test_dispatch_is_fifo_serial_and_effect_driven(void)
{
    struct fw_event_dispatcher dispatcher;
    struct callback_capture capture = {0};
    struct fw_button_sm button;
    struct fw_transition transition;
    struct fw_event press = button_event(FW_EVENT_BUTTON_PRESSED);
    struct fw_event confirmed = button_event(FW_EVENT_DEBOUNCE_CONFIRMED);

    capture.dispatcher = &dispatcher;
    fw_button_sm_init(&button);
    fw_event_dispatcher_init(&dispatcher,
                             capture_effect,
                             capture_trace,
                             &capture);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_BUTTON,
                                        0u,
                                        fw_button_sm_handle,
                                        &button) == 0);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_BUTTON,
                                        0u,
                                        fw_button_sm_handle,
                                        &button) == -EEXIST);
    assert(fw_event_dispatcher_post(&dispatcher, &press) == 0);
    assert(fw_event_dispatcher_post(&dispatcher, &confirmed) == 0);
    assert(fw_event_dispatcher_pending(&dispatcher) == 2u);

    assert(fw_event_dispatcher_dispatch_one(&dispatcher, &transition) == 0);
    assert(transition.old_state == FW_BUTTON_IDLE);
    assert(transition.new_state == FW_BUTTON_DEBOUNCE);
    assert(transition.effect.type == FW_EFFECT_START_TIMER);
    assert(transition.effect.payload.value == 50u);
    assert(button.state == FW_BUTTON_DEBOUNCE);
    assert(capture.reentrant_result == -EBUSY);

    assert(fw_event_dispatcher_dispatch_one(&dispatcher, &transition) == 0);
    assert(transition.old_state == FW_BUTTON_DEBOUNCE);
    assert(transition.new_state == FW_BUTTON_PRESSED);
    assert(transition.effect.payload.value == 1500u);
    assert(button.state == FW_BUTTON_PRESSED);
    assert(capture.effects == 2u);
    assert(capture.traces == 2u);
    assert(fw_event_dispatcher_pending(&dispatcher) == 0u);
    assert(fw_event_dispatcher_dispatch_one(&dispatcher, NULL) == -EAGAIN);
}

static void test_queue_is_bounded_and_rejects_bad_envelopes(void)
{
    struct fw_event_dispatcher dispatcher;
    struct fw_button_sm button;
    struct fw_event event = button_event(FW_EVENT_BUTTON_PRESSED);

    fw_button_sm_init(&button);
    fw_event_dispatcher_init(&dispatcher, NULL, NULL, NULL);
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -ENOENT);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_BUTTON,
                                        0u,
                                        fw_button_sm_handle,
                                        &button) == 0);
    for (size_t i = 0u; i < FW_EVENT_QUEUE_CAPACITY; i++) {
        event.timestamp_ms = i;
        assert(fw_event_dispatcher_post(&dispatcher, &event) == 0);
    }
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -ENOSPC);
    event.target = FW_MACHINE_NONE;
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -EINVAL);
    event.target = FW_MACHINE_BUTTON;
    event.target_instance = 1u;
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -ENOENT);
    assert(fw_event_dispatcher_post(NULL, &event) == -EINVAL);
}

static void test_typed_timer_events_fail_closed(void)
{
    struct fw_event_dispatcher dispatcher;
    struct fw_button_sm button;
    struct fw_event event = button_event(FW_EVENT_TIMER_EXPIRED);

    fw_button_sm_init(&button);
    fw_event_dispatcher_init(&dispatcher, NULL, NULL, NULL);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_BUTTON,
                                        0u,
                                        fw_button_sm_handle,
                                        &button) == 0);
    event.source = FW_EVENT_SOURCE_ISR;
    assert(!fw_event_validate(&event));
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -EINVAL);

    event.source = FW_EVENT_SOURCE_TIMER;
    event.payload.channel = FW_RADIO_CHANNEL_9;
    assert(!fw_event_validate(&event));
    assert(fw_event_dispatcher_post(&dispatcher, &event) == -EINVAL);

    event = (struct fw_event) {
        .operation_id = 1u,
        .generation = 1u,
        .target = FW_MACHINE_CLICK,
        .source = FW_EVENT_SOURCE_SERVICE,
        .type = FW_EVENT_TIMER_EXPIRED,
    };
    assert(fw_event_validate(&event));
}

static void test_typed_radio_events_require_canonical_request(void)
{
    struct fw_event_dispatcher dispatcher;
    struct fw_radio_sm radio;
    struct fw_event event = radio_request_event();

    fw_radio_sm_init(&radio);
    fw_event_dispatcher_init(&dispatcher, NULL, NULL, NULL);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_RADIO,
                                        0u,
                                        fw_radio_sm_handle,
                                        &radio) == 0);
    assert(fw_event_validate(&event));
    assert(fw_event_dispatcher_post(&dispatcher, &event) == 0);
    assert(fw_event_dispatcher_dispatch_one(&dispatcher, NULL) == 0);
    assert(radio.state == FW_RADIO_STARTING);

    event.source = FW_EVENT_SOURCE_ISR;
    assert(!fw_event_validate(&event));
    event.source = FW_EVENT_SOURCE_SERVICE;
    event.payload.channel = FW_RADIO_CHANNEL_NONE;
    assert(!fw_event_validate(&event));
    event.payload.channel = FW_RADIO_CHANNEL_5;
    event.payload.value |= UINT32_C(0x10000);
    assert(!fw_event_validate(&event));
    event = radio_request_event();
    event.payload.deadline_ms = event.payload.not_before_ms;
    assert(!fw_event_validate(&event));

    event = radio_request_event();
    event.type = FW_EVENT_RADIO_JOB_COMPLETED;
    event.reply_to = FW_MACHINE_NONE;
    event.payload.value = 1u;
    assert(!fw_event_validate(&event));
}

static void test_typed_ack_events_require_delivery_identity_and_empty_payload(void)
{
    struct fw_event_dispatcher dispatcher;
    struct fw_delivery_sm delivery;
    struct fw_event event = delivery_ack_event(FW_EVENT_HOP_ACK_RECEIVED);

    fw_delivery_sm_init(&delivery);
    fw_event_dispatcher_init(&dispatcher, NULL, NULL, NULL);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_DELIVERY,
                                        0u,
                                        fw_delivery_sm_handle,
                                        &delivery) == 0);
    assert(fw_event_validate(&event));
    assert(fw_event_dispatcher_post(&dispatcher, &event) == 0);
    assert(fw_event_dispatcher_dispatch_one(&dispatcher, NULL) == 0);

    event.source = FW_EVENT_SOURCE_ISR;
    assert(!fw_event_validate(&event));
    event.source = FW_EVENT_SOURCE_RADIO;
    event.target = FW_MACHINE_RADIO;
    assert(!fw_event_validate(&event));
    event.target = FW_MACHINE_DELIVERY;
    event.payload.value = 1u;
    assert(!fw_event_validate(&event));

    event = delivery_ack_event(FW_EVENT_ACK_TIMED_OUT);
    event.source = FW_EVENT_SOURCE_RADIO;
    assert(!fw_event_validate(&event));
    event.source = FW_EVENT_SOURCE_TIMER;
    event.operation_id = 0u;
    assert(!fw_event_validate(&event));
}

static void test_dispatch_all_has_an_explicit_bound(void)
{
    struct fw_event_dispatcher dispatcher;
    struct fw_button_sm button;
    struct fw_event press = button_event(FW_EVENT_BUTTON_PRESSED);
    struct fw_event release = button_event(FW_EVENT_BUTTON_RELEASED);
    size_t processed = 0u;

    fw_button_sm_init(&button);
    fw_event_dispatcher_init(&dispatcher, NULL, NULL, NULL);
    assert(fw_event_dispatcher_register(&dispatcher,
                                        FW_MACHINE_BUTTON,
                                        0u,
                                        fw_button_sm_handle,
                                        &button) == 0);
    assert(fw_event_dispatcher_post(&dispatcher, &press) == 0);
    assert(fw_event_dispatcher_post(&dispatcher, &release) == 0);
    assert(fw_event_dispatcher_dispatch_all(&dispatcher, 1u, &processed) == 0);
    assert(processed == 1u);
    assert(fw_event_dispatcher_pending(&dispatcher) == 1u);
    assert(fw_event_dispatcher_dispatch_all(&dispatcher, 4u, &processed) == 0);
    assert(processed == 1u);
    assert(button.state == FW_BUTTON_IDLE);
}

int main(void)
{
    test_dispatch_is_fifo_serial_and_effect_driven();
    test_queue_is_bounded_and_rejects_bad_envelopes();
    test_typed_timer_events_fail_closed();
    test_typed_radio_events_require_canonical_request();
    test_typed_ack_events_require_delivery_identity_and_empty_payload();
    test_dispatch_all_has_an_explicit_bound();
    return 0;
}
