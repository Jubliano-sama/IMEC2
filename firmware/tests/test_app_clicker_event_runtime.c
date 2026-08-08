#include "app_clicker_event_runtime.h"

#include <assert.h>
#include <stdio.h>

static void expect_effect(struct app_clicker_event_runtime *runtime,
                          enum fw_effect_type type)
{
    struct fw_effect effect;

    assert(app_clicker_event_runtime_take_effect(runtime, &effect));
    assert(effect.type == type);
}

static void drain_effects(struct app_clicker_event_runtime *runtime)
{
    struct fw_effect effect;

    while (app_clicker_event_runtime_take_effect(runtime, &effect)) {
    }
}

static void test_button_normal_click_is_explicit(void)
{
    struct app_clicker_event_runtime runtime;
    enum button_action action;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_PRESS,
                                                   100u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_NONE);
    expect_effect(&runtime, FW_EFFECT_START_TIMER);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_RELEASE,
                                                   200u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_NORMAL_CLICK);
    assert(app_clicker_event_runtime_button_pressed_at_ms(&runtime) == 100u);
    expect_effect(&runtime, FW_EFFECT_START_TIMER);
    expect_effect(&runtime, FW_EFFECT_PUBLISH_EVENT);
    assert(app_clicker_event_runtime_button_state(&runtime) == FW_BUTTON_IDLE);
    assert(app_clicker_event_runtime_effect_drop_count(&runtime) == 0u);
}

static void test_button_short_release_cancels_debounce(void)
{
    struct app_clicker_event_runtime runtime;
    enum button_action action;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_PRESS,
                                                   100u,
                                                   &action) == 0);
    expect_effect(&runtime, FW_EFFECT_START_TIMER);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_RELEASE,
                                                   120u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_NONE);
    expect_effect(&runtime, FW_EFFECT_CANCEL_TIMER);
    assert(app_clicker_event_runtime_button_state(&runtime) == FW_BUTTON_IDLE);

    /* A delayed debounce tick or duplicate release must not publish a click. */
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_TICK,
                                                   200u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_NONE);
    {
        struct fw_effect effect;

        assert(!app_clicker_event_runtime_take_effect(&runtime, &effect));
    }
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_RELEASE,
                                                   220u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_NONE);
    assert(app_clicker_event_runtime_button_state(&runtime) == FW_BUTTON_IDLE);
}

static void test_button_long_press_and_expiry(void)
{
    struct app_clicker_event_runtime runtime;
    enum button_action action;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_PRESS,
                                                   100u,
                                                   &action) == 0);
    drain_effects(&runtime);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_RELEASE,
                                                   1700u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_SELF_TEST_ARMED);
    drain_effects(&runtime);
    assert(app_clicker_event_runtime_button_state(&runtime) ==
           FW_BUTTON_SELF_TEST_ARMED);
    assert(app_clicker_event_runtime_button_signal(&runtime,
                                                   BUTTON_SIGNAL_TICK,
                                                   4701u,
                                                   &action) == 0);
    assert(action == BUTTON_ACTION_SELF_TEST_CANCELLED);
    assert(app_clicker_event_runtime_button_state(&runtime) == FW_BUTTON_IDLE);
}

static void test_click_events_are_generation_bound(void)
{
    struct app_clicker_event_runtime runtime;
    struct fw_event_payload payload = {0};

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_click_start(&runtime, 42u) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CREATE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CLICK_CREATED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CHANNEL_CLEAR,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_WAKE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_WAKE_COMPLETED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_DISCOVER);
    payload.count = 0u;
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_DISCOVERY_COMPLETED,
                                                 &payload) == 0);
    expect_effect(&runtime, FW_EFFECT_START_TIMER);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RETRY_ALLOWED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_state(&runtime) ==
           FW_CLICK_POLITENESS);
}

static void test_retryable_radio_failure_keeps_generation(void)
{
    struct app_clicker_event_runtime runtime;
    struct fw_event_payload payload = {
        .flags = FW_EVENT_FLAG_RETRYABLE,
        .value = 1u,
    };
    uint32_t generation;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_click_start(&runtime, 43u) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CREATE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CLICK_CREATED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CHANNEL_CLEAR,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_WAKE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RF_STARTED,
                                                 NULL) == 0);
    assert(runtime.click.attempts_started == 1u);
    generation = runtime.click.identity.generation;

    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RADIO_JOB_FAILED,
                                                 &payload) == 0);
    expect_effect(&runtime, FW_EFFECT_START_TIMER);
    assert(runtime.click.identity.generation == generation);
    assert(app_clicker_event_runtime_click_state(&runtime) == FW_CLICK_RETRY);

    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RETRY_ALLOWED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(runtime.click.identity.generation == generation);
}

static void test_late_rf_started_after_wake_does_not_count(void)
{
    struct app_clicker_event_runtime runtime;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_click_start(&runtime, 46u) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CREATE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CLICK_CREATED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CHANNEL_CLEAR,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_WAKE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RF_STARTED,
                                                 NULL) == 0);
    assert(runtime.click.attempts_started == 1u);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_WAKE_COMPLETED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_DISCOVER);

    /* This event belongs to the completed wake child, despite its live
     * operation identity still matching the click. */
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RF_STARTED,
                                                 NULL) == 0);
    assert(runtime.click.attempts_started == 1u);
    assert(app_clicker_event_runtime_click_state(&runtime) ==
           FW_CLICK_DISCOVER);
}

static void test_discovery_payload_uses_configured_threshold(void)
{
    struct app_clicker_event_runtime runtime;
    struct fw_event_payload payload = {
        .count = 3u,
        .value = 4u,
    };

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_click_start(&runtime, 44u) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CREATE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CLICK_CREATED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CHANNEL_CLEAR,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_WAKE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_WAKE_COMPLETED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_DISCOVER);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_DISCOVERY_COMPLETED,
                                                 &payload) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_RELEASE);
    assert(app_clicker_event_runtime_click_state(&runtime) ==
           FW_CLICK_RELEASE);
}

static void test_non_retryable_radio_failure_cleans_up(void)
{
    struct app_clicker_event_runtime runtime;

    app_clicker_event_runtime_init(&runtime);
    assert(app_clicker_event_runtime_click_start(&runtime, 45u) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CREATE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CLICK_CREATED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CHECK_POLITENESS);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_CHANNEL_CLEAR,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_SEND_WAKE);
    assert(app_clicker_event_runtime_click_event(&runtime,
                                                 FW_EVENT_RADIO_JOB_FAILED,
                                                 NULL) == 0);
    expect_effect(&runtime, FW_EFFECT_CLICK_CLEANUP);
    assert(app_clicker_event_runtime_click_state(&runtime) == FW_CLICK_FAILURE);
}

int main(void)
{
    test_button_normal_click_is_explicit();
    test_button_short_release_cancels_debounce();
    test_button_long_press_and_expiry();
    test_click_events_are_generation_bound();
    test_retryable_radio_failure_keeps_generation();
    test_late_rf_started_after_wake_does_not_count();
    test_discovery_payload_uses_configured_threshold();
    test_non_retryable_radio_failure_cleans_up();
    puts("app_clicker_event_runtime: ok");
    return 0;
}
