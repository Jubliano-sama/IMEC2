#include "app_clicker_event_runtime.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static bool self_test_arm_window_expired(uint32_t now_ms,
                                         uint32_t armed_at_ms)
{
    return (int32_t)(now_ms - armed_at_ms) >
           (int32_t)FW_BUTTON_SELF_TEST_ARM_MS;
}

static enum button_action button_action_for_transition(
    const struct fw_transition *transition)
{
    if (transition == NULL || transition->result != FW_SM_APPLIED) {
        return BUTTON_ACTION_NONE;
    }

    if (transition->old_state == FW_BUTTON_PRESSED &&
        transition->new_state == FW_BUTTON_NORMAL_CLICK) {
        return BUTTON_ACTION_NORMAL_CLICK;
    }
    if (transition->old_state == FW_BUTTON_WAIT_LONG_RELEASE &&
        transition->new_state == FW_BUTTON_SELF_TEST_ARMED) {
        return BUTTON_ACTION_SELF_TEST_ARMED;
    }
    if (transition->old_state == FW_BUTTON_SELF_TEST_CONFIRM &&
        transition->new_state == FW_BUTTON_SELF_TEST) {
        return BUTTON_ACTION_SELF_TEST_START;
    }
    if ((transition->old_state == FW_BUTTON_SELF_TEST_ARMED ||
         transition->old_state == FW_BUTTON_SELF_TEST_CONFIRM) &&
        transition->new_state == FW_BUTTON_IDLE) {
        return BUTTON_ACTION_SELF_TEST_CANCELLED;
    }
    return BUTTON_ACTION_NONE;
}

static int queue_effect(struct app_clicker_event_runtime *runtime,
                        const struct fw_effect *effect)
{
    uint8_t tail;

    if (runtime == NULL || effect == NULL || effect->type == FW_EFFECT_NONE) {
        return 0;
    }
    if (runtime->effect_count >= APP_CLICKER_EVENT_RUNTIME_EFFECT_CAPACITY) {
        if (runtime->effect_drop_count != UINT32_MAX) {
            runtime->effect_drop_count++;
        }
        return -ENOSPC;
    }

    tail = (uint8_t)((runtime->effect_head + runtime->effect_count) %
                     APP_CLICKER_EVENT_RUNTIME_EFFECT_CAPACITY);
    runtime->effects[tail] = *effect;
    runtime->effect_count++;
    return 0;
}

static struct fw_event event_for(enum fw_machine_id target,
                                 enum fw_event_type type,
                                 uint64_t operation_id,
                                 uint32_t generation,
                                 const struct fw_event_payload *payload,
                                 enum fw_event_source source)
{
    struct fw_event event = {
        .timestamp_ms = 0u,
        .operation_id = operation_id,
        .generation = generation,
        .target = target,
        .source = source,
        .type = type,
    };

    if (payload != NULL) {
        event.payload = *payload;
    }
    return event;
}

static int dispatch_button_event(struct app_clicker_event_runtime *runtime,
                                 const struct fw_event *event,
                                 enum button_action *last_action)
{
    struct fw_transition transition;
    enum fw_sm_result result;
    enum button_action action;

    result = fw_button_sm_handle(&runtime->button, event, &transition);
    if (result == FW_SM_INVALID || result == FW_SM_BUSY ||
        result == FW_SM_STALE) {
        return -EPROTO;
    }
    if (result != FW_SM_APPLIED) {
        return 0;
    }

    if (queue_effect(runtime, &transition.effect) < 0) {
        return -ENOSPC;
    }
    action = button_action_for_transition(&transition);
    if (action != BUTTON_ACTION_NONE) {
        if (last_action != NULL) {
            *last_action = action;
        }
    }

    /* PUBLISH_EVENT is the machine's action boundary.  The application
     * consumes the mapped action, while this second event returns the button
     * machine to IDLE before any later GPIO edge can arrive. */
    if (transition.new_state == FW_BUTTON_NORMAL_CLICK ||
        transition.new_state == FW_BUTTON_SELF_TEST) {
        struct fw_event accepted = *event;

        accepted.source = FW_EVENT_SOURCE_MACHINE;
        accepted.type = FW_EVENT_ACTION_ACCEPTED;
        result = fw_button_sm_handle(&runtime->button,
                                     &accepted,
                                     &transition);
        if (result != FW_SM_APPLIED) {
            return -EPROTO;
        }
    }
    return 0;
}

static int dispatch_click_event(struct app_clicker_event_runtime *runtime,
                                const struct fw_event *event)
{
    struct fw_transition transition;
    enum fw_sm_result result;

    result = fw_click_sm_handle(&runtime->click, event, &transition);
    if (result == FW_SM_INVALID || result == FW_SM_BUSY ||
        result == FW_SM_STALE) {
        return -EPROTO;
    }
    if (result != FW_SM_APPLIED) {
        return 0;
    }
    return queue_effect(runtime, &transition.effect);
}

static int dispatch_button_signal(struct app_clicker_event_runtime *runtime,
                                  enum fw_event_type type,
                                  uint32_t now_ms,
                                  enum button_action *action)
{
    struct fw_event event = event_for(FW_MACHINE_BUTTON,
                                      type,
                                      0u,
                                      0u,
                                      NULL,
                                      type == FW_EVENT_TIMER_EXPIRED ?
                                          FW_EVENT_SOURCE_TIMER :
                                          FW_EVENT_SOURCE_ISR);
    int ret;

    event.timestamp_ms = now_ms;
    ret = dispatch_button_event(runtime, &event, action);
    return ret;
}

void app_clicker_event_runtime_init(
    struct app_clicker_event_runtime *runtime)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    fw_button_sm_init(&runtime->button);
    fw_click_sm_init(&runtime->click);
}

int app_clicker_event_runtime_button_signal(
    struct app_clicker_event_runtime *runtime,
    enum button_signal signal,
    uint32_t now_ms,
    enum button_action *action)
{
    uint32_t duration_ms;
    int ret;

    if (runtime == NULL || action == NULL) {
        return -EINVAL;
    }
    *action = BUTTON_ACTION_NONE;

    if (signal == BUTTON_SIGNAL_PRESS) {
        if (runtime->button.state == FW_BUTTON_SELF_TEST_ARMED &&
            self_test_arm_window_expired(now_ms,
                                         runtime->button_armed_at_ms)) {
            ret = dispatch_button_signal(runtime,
                                         FW_EVENT_TIMER_EXPIRED,
                                         now_ms,
                                         action);
            if (ret < 0) {
                return ret;
            }
            /* The expiry is an internal prerequisite of this press. */
            *action = BUTTON_ACTION_NONE;
        }
        if (runtime->button.state != FW_BUTTON_IDLE &&
            runtime->button.state != FW_BUTTON_SELF_TEST_ARMED) {
            return 0;
        }
        runtime->button_pressed_at_ms = now_ms;
        return dispatch_button_signal(runtime,
                                      FW_EVENT_BUTTON_PRESSED,
                                      now_ms,
                                      action);
    }

    if (signal == BUTTON_SIGNAL_TICK) {
        if (runtime->button.state != FW_BUTTON_SELF_TEST_ARMED ||
            !self_test_arm_window_expired(now_ms,
                                          runtime->button_armed_at_ms)) {
            return 0;
        }
        return dispatch_button_signal(runtime,
                                      FW_EVENT_TIMER_EXPIRED,
                                      now_ms,
                                      action);
    }

    if (signal != BUTTON_SIGNAL_RELEASE) {
        return -EINVAL;
    }
    if (runtime->button.state != FW_BUTTON_DEBOUNCE &&
        runtime->button.state != FW_BUTTON_PRESSED &&
        runtime->button.state != FW_BUTTON_WAIT_LONG_RELEASE &&
        runtime->button.state != FW_BUTTON_SELF_TEST_CONFIRM) {
        return 0;
    }

    duration_ms = now_ms - runtime->button_pressed_at_ms;
    if (duration_ms < FW_BUTTON_DEBOUNCE_MS) {
        /* A release before debounce confirms that the edge was noise.  Give
         * the state machine the release so it cancels its timer and returns to
         * IDLE; leaving DEBOUNCE pending can turn a later timer tick into a
         * normal click after the button is already up. */
        if (runtime->button.state == FW_BUTTON_DEBOUNCE) {
            return dispatch_button_signal(runtime,
                                          FW_EVENT_BUTTON_RELEASED,
                                          now_ms,
                                          action);
        }
        return 0;
    }
    if (runtime->button.state == FW_BUTTON_DEBOUNCE) {
        ret = dispatch_button_signal(runtime,
                                     FW_EVENT_DEBOUNCE_CONFIRMED,
                                     now_ms,
                                     action);
        if (ret < 0) {
            return ret;
        }
    }
    if ((runtime->button.state == FW_BUTTON_PRESSED ||
         runtime->button.state == FW_BUTTON_SELF_TEST_CONFIRM) &&
        duration_ms >= FW_BUTTON_LONG_PRESS_MS) {
        ret = dispatch_button_signal(runtime,
                                     FW_EVENT_LONG_PRESS_DETECTED,
                                     now_ms,
                                     action);
        if (ret < 0 || runtime->button.state == FW_BUTTON_IDLE) {
            return ret;
        }
    }
    ret = dispatch_button_signal(runtime,
                                 FW_EVENT_BUTTON_RELEASED,
                                 now_ms,
                                 action);
    if (runtime->button.state == FW_BUTTON_SELF_TEST_ARMED) {
        runtime->button_armed_at_ms = now_ms;
    }
    return ret;
}

int app_clicker_event_runtime_click_start(
    struct app_clicker_event_runtime *runtime,
    uint64_t operation_id)
{
    struct fw_event event;

    if (runtime == NULL || operation_id == 0u) {
        return -EINVAL;
    }
    if (runtime->generation == UINT32_MAX) {
        runtime->generation = 1u;
    } else {
        runtime->generation++;
        if (runtime->generation == 0u) {
            runtime->generation = 1u;
        }
    }
    event = event_for(FW_MACHINE_CLICK,
                      FW_EVENT_START,
                      operation_id,
                      runtime->generation,
                      NULL,
                      FW_EVENT_SOURCE_SERVICE);
    return dispatch_click_event(runtime, &event);
}

int app_clicker_event_runtime_click_event(
    struct app_clicker_event_runtime *runtime,
    enum fw_event_type type,
    const struct fw_event_payload *payload)
{
    struct fw_event event;

    if (runtime == NULL || type <= FW_EVENT_NONE ||
        type >= FW_EVENT_TYPE_COUNT || !runtime->click.identity.active) {
        return -EINVAL;
    }
    event = event_for(FW_MACHINE_CLICK,
                      type,
                      runtime->click.identity.operation_id,
                      runtime->click.identity.generation,
                      payload,
                      FW_EVENT_SOURCE_SERVICE);
    return dispatch_click_event(runtime, &event);
}

uint32_t app_clicker_event_runtime_button_pressed_at_ms(
    const struct app_clicker_event_runtime *runtime)
{
    return runtime == NULL ? 0u : runtime->button_pressed_at_ms;
}

bool app_clicker_event_runtime_take_effect(
    struct app_clicker_event_runtime *runtime,
    struct fw_effect *effect)
{
    if (runtime == NULL || effect == NULL || runtime->effect_count == 0u) {
        return false;
    }
    *effect = runtime->effects[runtime->effect_head];
    runtime->effect_head = (uint8_t)((runtime->effect_head + 1u) %
                                     APP_CLICKER_EVENT_RUNTIME_EFFECT_CAPACITY);
    runtime->effect_count--;
    return true;
}

uint32_t app_clicker_event_runtime_effect_drop_count(
    const struct app_clicker_event_runtime *runtime)
{
    return runtime == NULL ? 0u : runtime->effect_drop_count;
}

enum fw_button_state app_clicker_event_runtime_button_state(
    const struct app_clicker_event_runtime *runtime)
{
    return runtime == NULL ? FW_BUTTON_IDLE : runtime->button.state;
}

enum fw_click_state app_clicker_event_runtime_click_state(
    const struct app_clicker_event_runtime *runtime)
{
    return runtime == NULL ? FW_CLICK_IDLE : runtime->click.state;
}
