#include "firmware_events.h"

#include <errno.h>
#include <string.h>

static bool machine_valid(enum fw_machine_id machine)
{
    return machine > FW_MACHINE_NONE && machine < FW_MACHINE_COUNT;
}

static bool event_identity_pair_valid(const struct fw_event *event)
{
    return event != NULL &&
           ((event->operation_id == 0u) == (event->generation == 0u));
}

static bool event_payload_flags_valid(const struct fw_event *event)
{
    const uint8_t known_flags = (uint8_t)(FW_EVENT_FLAG_ROUTE_READY |
                                          FW_EVENT_FLAG_CONNECTION_READY |
                                          FW_EVENT_FLAG_PATH_USABLE |
                                          FW_EVENT_FLAG_MORE_PENDING |
                                          FW_EVENT_FLAG_GRAPH_COMPLETE |
                                          FW_EVENT_FLAG_RETRYABLE);

    return event != NULL &&
           (event->payload.flags & (uint8_t)~known_flags) == 0u;
}

static bool event_payload_is_empty(const struct fw_event_payload *payload)
{
    return payload != NULL && payload->subject_id == 0u &&
           payload->not_before_ms == 0u && payload->deadline_ms == 0u &&
           payload->value == 0u && payload->duration_ms == 0u &&
           payload->count == 0u && payload->channel == 0u &&
           payload->flags == 0u;
}

static bool event_timer_payload_valid(const struct fw_event_payload *payload)
{
    /* A timer may carry its opaque timer token in value and a retry marker,
     * but radio/job fields must never be interpreted as timer data. */
    return payload != NULL && payload->subject_id == 0u &&
           payload->not_before_ms == 0u && payload->deadline_ms == 0u &&
           payload->duration_ms == 0u && payload->count == 0u &&
           payload->channel == 0u;
}

static bool event_source_allowed_for_timer(const struct fw_event *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->target == FW_MACHINE_BUTTON) {
        return event->source == FW_EVENT_SOURCE_TIMER;
    }
    return event->source == FW_EVENT_SOURCE_TIMER ||
           event->source == FW_EVENT_SOURCE_RADIO ||
           event->source == FW_EVENT_SOURCE_SERVICE ||
           event->source == FW_EVENT_SOURCE_MACHINE;
}

static bool event_target_requires_identity(enum fw_machine_id target)
{
    switch (target) {
    case FW_MACHINE_BUTTON:
    case FW_MACHINE_GATEWAY_UWB:
    case FW_MACHINE_HOST_LINK:
        return false;
    default:
        return true;
    }
}

static bool event_source_allowed_for_radio(const struct fw_event *event)
{
    return event != NULL &&
           (event->source == FW_EVENT_SOURCE_RADIO ||
            event->source == FW_EVENT_SOURCE_SERVICE ||
            event->source == FW_EVENT_SOURCE_MACHINE);
}

static bool event_source_allowed_for_ack(const struct fw_event *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == FW_EVENT_ACK_TIMED_OUT) {
        return event->source == FW_EVENT_SOURCE_TIMER ||
               event->source == FW_EVENT_SOURCE_SERVICE ||
               event->source == FW_EVENT_SOURCE_MACHINE;
    }
    return event->source == FW_EVENT_SOURCE_RADIO ||
           event->source == FW_EVENT_SOURCE_SERVICE ||
           event->source == FW_EVENT_SOURCE_MACHINE;
}

static bool radio_request_payload_valid(const struct fw_event *event)
{
    enum fw_radio_mode mode;
    enum fw_radio_priority priority;

    if (event == NULL || event->reply_to == FW_MACHINE_NONE ||
        !machine_valid(event->reply_to) ||
        event->payload.subject_id != 0u || event->payload.flags != 0u ||
        event->payload.count != 0u) {
        return false;
    }

    mode = FW_RADIO_REQUEST_MODE(event->payload.value);
    priority = FW_RADIO_REQUEST_PRIORITY(event->payload.value);
    return event->payload.channel == FW_RADIO_CHANNEL_5 ||
               event->payload.channel == FW_RADIO_CHANNEL_9 ?
           (mode == FW_RADIO_MODE_RX || mode == FW_RADIO_MODE_TX) &&
           priority >= FW_RADIO_PRIORITY_BACKGROUND &&
           priority <= FW_RADIO_PRIORITY_GATEWAY_CONTROL &&
           event->payload.value == FW_RADIO_REQUEST_VALUE(mode, priority) &&
           event->payload.duration_ms != 0u &&
           event->payload.deadline_ms > event->payload.not_before_ms &&
           event->payload.duration_ms <=
               event->payload.deadline_ms - event->payload.not_before_ms :
           false;
}

static bool radio_event_valid(const struct fw_event *event)
{
    if (event == NULL || event->target != FW_MACHINE_RADIO ||
        !event_source_allowed_for_radio(event) ||
        event->operation_id == 0u || event->generation == 0u) {
        return false;
    }

    if (event->type == FW_EVENT_RADIO_REQUESTED) {
        return radio_request_payload_valid(event);
    }
    if (event->type == FW_EVENT_RADIO_PREEMPT_REQUESTED) {
        return event->payload.value != 0u &&
               event->payload.subject_id == 0u &&
               event->payload.not_before_ms == 0u &&
               event->payload.deadline_ms == 0u &&
               event->payload.duration_ms == 0u &&
               event->payload.count == 0u && event->payload.channel == 0u &&
               event->payload.flags == 0u;
    }
    return event_payload_is_empty(&event->payload);
}

static bool ack_event_valid(const struct fw_event *event)
{
    return event != NULL && event->target == FW_MACHINE_DELIVERY &&
           event_source_allowed_for_ack(event) &&
           event->operation_id != 0u && event->generation != 0u &&
           event_payload_is_empty(&event->payload);
}

bool fw_event_validate(const struct fw_event *event)
{
    if (event == NULL || !machine_valid(event->target) ||
        event->source < FW_EVENT_SOURCE_BOOT ||
        event->source >= FW_EVENT_SOURCE_COUNT ||
        event->type <= FW_EVENT_NONE || event->type >= FW_EVENT_TYPE_COUNT ||
        (event->reply_to != FW_MACHINE_NONE &&
         !machine_valid(event->reply_to)) ||
        (event->reply_to == FW_MACHINE_NONE && event->reply_instance != 0u) ||
        !event_identity_pair_valid(event) ||
        !event_payload_flags_valid(event)) {
        return false;
    }
    if (event_target_requires_identity(event->target) &&
        (event->operation_id == 0u || event->generation == 0u)) {
        return false;
    }

    if (event->type == FW_EVENT_TIMER_EXPIRED) {
        return event_source_allowed_for_timer(event) &&
               event_timer_payload_valid(&event->payload);
    }
    if (event->type == FW_EVENT_RADIO_REQUESTED ||
        event->type == FW_EVENT_RADIO_INIT_SUCCEEDED ||
        event->type == FW_EVENT_RADIO_INIT_FAILED ||
        event->type == FW_EVENT_RADIO_RETUNE_SUCCEEDED ||
        event->type == FW_EVENT_RADIO_JOB_COMPLETED ||
        event->type == FW_EVENT_RADIO_JOB_TIMED_OUT ||
        event->type == FW_EVENT_RADIO_JOB_CANCELLED ||
        event->type == FW_EVENT_RADIO_JOB_FAILED ||
        event->type == FW_EVENT_RADIO_RECOVERY_RETRY ||
        event->type == FW_EVENT_RADIO_RECOVERY_EXHAUSTED ||
        event->type == FW_EVENT_RADIO_PREEMPT_REQUESTED ||
        event->type == FW_EVENT_RADIO_SAFE_BOUNDARY) {
        return radio_event_valid(event);
    }
    if (event->type == FW_EVENT_HOP_ACK_RECEIVED ||
        event->type == FW_EVENT_GATEWAY_ACK_RECEIVED ||
        event->type == FW_EVENT_ACK_TIMED_OUT) {
        return ack_event_valid(event);
    }
    return true;
}

static struct fw_event_handler_registration *find_handler(
    struct fw_event_dispatcher *dispatcher,
    const struct fw_event *event);

void fw_event_dispatcher_init(struct fw_event_dispatcher *dispatcher,
                              fw_effect_sink_fn effect_sink,
                              fw_transition_trace_fn trace,
                              void *callback_context)
{
    if (dispatcher == NULL) {
        return;
    }
    memset(dispatcher, 0, sizeof(*dispatcher));
    dispatcher->effect_sink = effect_sink;
    dispatcher->trace = trace;
    dispatcher->callback_context = callback_context;
}

int fw_event_dispatcher_register(struct fw_event_dispatcher *dispatcher,
                                 enum fw_machine_id machine,
                                 uint16_t instance,
                                 fw_event_handler_fn handler,
                                 void *context)
{
    struct fw_event_handler_registration *free_slot = NULL;

    if (dispatcher == NULL || !machine_valid(machine) || handler == NULL ||
        context == NULL) {
        return -EINVAL;
    }
    for (size_t i = 0u; i < FW_EVENT_HANDLER_CAPACITY; i++) {
        struct fw_event_handler_registration *registration =
            &dispatcher->handlers[i];

        if (registration->used && registration->machine == machine &&
            registration->instance == instance) {
            return -EEXIST;
        }
        if (!registration->used && free_slot == NULL) {
            free_slot = registration;
        }
    }
    if (free_slot == NULL) {
        return -ENOSPC;
    }
    *free_slot = (struct fw_event_handler_registration) {
        .machine = machine,
        .instance = instance,
        .handler = handler,
        .context = context,
        .used = true,
    };
    return 0;
}

int fw_event_dispatcher_post(struct fw_event_dispatcher *dispatcher,
                             const struct fw_event *event)
{
    size_t tail;

    if (dispatcher == NULL || !fw_event_validate(event)) {
        return -EINVAL;
    }
    if (find_handler(dispatcher, event) == NULL) {
        return -ENOENT;
    }
    if (dispatcher->count == FW_EVENT_QUEUE_CAPACITY) {
        return -ENOSPC;
    }
    tail = (dispatcher->head + dispatcher->count) % FW_EVENT_QUEUE_CAPACITY;
    dispatcher->queue[tail] = *event;
    dispatcher->count++;
    return 0;
}

static struct fw_event_handler_registration *find_handler(
    struct fw_event_dispatcher *dispatcher,
    const struct fw_event *event)
{
    for (size_t i = 0u; i < FW_EVENT_HANDLER_CAPACITY; i++) {
        struct fw_event_handler_registration *registration =
            &dispatcher->handlers[i];

        if (registration->used && registration->machine == event->target &&
            registration->instance == event->target_instance) {
            return registration;
        }
    }
    return NULL;
}

int fw_event_dispatcher_dispatch_one(struct fw_event_dispatcher *dispatcher,
                                     struct fw_transition *transition)
{
    struct fw_event_handler_registration *registration;
    struct fw_transition local_transition;
    struct fw_event event;

    if (dispatcher == NULL) {
        return -EINVAL;
    }
    if (dispatcher->dispatching) {
        return -EBUSY;
    }
    if (dispatcher->count == 0u) {
        return -EAGAIN;
    }

    event = dispatcher->queue[dispatcher->head];
    dispatcher->head = (dispatcher->head + 1u) % FW_EVENT_QUEUE_CAPACITY;
    dispatcher->count--;
    if (!fw_event_validate(&event)) {
        return -EINVAL;
    }
    registration = find_handler(dispatcher, &event);
    if (registration == NULL) {
        return -ENOENT;
    }

    dispatcher->dispatching = true;
    memset(&local_transition, 0, sizeof(local_transition));
    local_transition.result = registration->handler(registration->context,
                                                    &event,
                                                    &local_transition);
    if (dispatcher->trace != NULL) {
        dispatcher->trace(&event,
                          &local_transition,
                          dispatcher->callback_context);
    }
    if (local_transition.result == FW_SM_APPLIED &&
        local_transition.effect.type != FW_EFFECT_NONE &&
        dispatcher->effect_sink != NULL) {
        dispatcher->effect_sink(&local_transition.effect,
                                dispatcher->callback_context);
    }
    dispatcher->dispatching = false;
    if (transition != NULL) {
        *transition = local_transition;
    }
    return 0;
}

int fw_event_dispatcher_dispatch_all(struct fw_event_dispatcher *dispatcher,
                                     size_t maximum_events,
                                     size_t *processed)
{
    size_t count = 0u;
    int ret = 0;

    if (dispatcher == NULL || maximum_events == 0u) {
        return -EINVAL;
    }
    while (count < maximum_events && dispatcher->count != 0u) {
        ret = fw_event_dispatcher_dispatch_one(dispatcher, NULL);
        if (ret != 0) {
            break;
        }
        count++;
    }
    if (processed != NULL) {
        *processed = count;
    }
    return ret;
}

size_t fw_event_dispatcher_pending(const struct fw_event_dispatcher *dispatcher)
{
    return dispatcher == NULL ? 0u : dispatcher->count;
}
