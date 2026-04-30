#include "status.h"

#include <assert.h>

static void test_short_press_is_click(void)
{
    struct button_fsm fsm;
    enum button_action action = BUTTON_ACTION_NONE;

    button_fsm_init(&fsm);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 100u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NONE);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 250u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NORMAL_CLICK);
    assert(!fsm.armed);
}

static void test_long_press_then_short_press_starts_self_test(void)
{
    struct button_fsm fsm;
    enum button_action action = BUTTON_ACTION_NONE;

    button_fsm_init(&fsm);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 100u, &action) == PROTO_OK);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 1700u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_SELF_TEST_ARMED);
    assert(fsm.armed);

    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 1900u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NONE);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 2020u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_SELF_TEST_START);
    assert(!fsm.armed);
}

static void test_arm_timeout_then_normal_click(void)
{
    struct button_fsm fsm;
    enum button_action action = BUTTON_ACTION_NONE;

    button_fsm_init(&fsm);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 100u, &action) == PROTO_OK);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 1700u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_SELF_TEST_ARMED);

    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_TICK, 4701u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_SELF_TEST_CANCELLED);
    assert(!fsm.armed);

    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 4800u, &action) == PROTO_OK);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 4920u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NORMAL_CLICK);
}

static void test_bounce_release_is_ignored(void)
{
    struct button_fsm fsm;
    enum button_action action = BUTTON_ACTION_NONE;

    button_fsm_init(&fsm);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_PRESS, 100u, &action) == PROTO_OK);
    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 120u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NONE);
    assert(fsm.pressed);

    assert(button_fsm_handle(&fsm, BUTTON_SIGNAL_RELEASE, 220u, &action) == PROTO_OK);
    assert(action == BUTTON_ACTION_NORMAL_CLICK);
}

static void test_status_priority_and_codes(void)
{
    struct status_inputs inputs = {
        .self_test_running = true,
        .failure = SELF_TEST_FAILURE_UWB,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
    assert(indication.red_blink_count == 5u);
    assert(indication.repeat_count == STATUS_FAILURE_REPEAT_COUNT);

    inputs.failure = SELF_TEST_FAILURE_NONE;
    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_BLUE_CHASE);

    inputs.self_test_running = false;
    inputs.click_accepted = true;
    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_GREEN_SOLID);
    assert(indication.duration_ms == STATUS_PASS_DURATION_MS);
}

int main(void)
{
    test_short_press_is_click();
    test_long_press_then_short_press_starts_self_test();
    test_arm_timeout_then_normal_click();
    test_bounce_release_is_ignored();
    test_status_priority_and_codes();
    return 0;
}
