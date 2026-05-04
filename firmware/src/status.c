#include "status.h"

#include <string.h>

static uint32_t elapsed_ms(uint32_t now_ms, uint32_t then_ms)
{
    return now_ms - then_ms;
}

static bool arm_window_open(const struct button_fsm *fsm, uint32_t now_ms)
{
    return fsm->armed && elapsed_ms(now_ms, fsm->armed_at_ms) <= SELF_TEST_ARM_WINDOW_MS;
}

void button_fsm_init(struct button_fsm *fsm)
{
    if (fsm != NULL) {
        memset(fsm, 0, sizeof(*fsm));
    }
}

int button_fsm_handle(struct button_fsm *fsm,
                           enum button_signal signal,
                           uint32_t now_ms,
                           enum button_action *action)
{
    if (fsm == NULL || action == NULL) {
        return PROTO_ERR_ARG;
    }

    *action = BUTTON_ACTION_NONE;

    if (signal == BUTTON_SIGNAL_TICK) {
        if (fsm->armed && !arm_window_open(fsm, now_ms)) {
            fsm->armed = false;
            fsm->confirm_press = false;
            *action = BUTTON_ACTION_SELF_TEST_CANCELLED;
        }
        return PROTO_OK;
    }

    if (signal != BUTTON_SIGNAL_PRESS && signal != BUTTON_SIGNAL_RELEASE) {
        return PROTO_ERR_ARG;
    }

    if (fsm->have_last_edge_ms &&
        elapsed_ms(now_ms, fsm->last_edge_ms) < BUTTON_DEBOUNCE_MS) {
        return PROTO_OK;
    }
    fsm->have_last_edge_ms = true;
    fsm->last_edge_ms = now_ms;

    if (signal == BUTTON_SIGNAL_PRESS) {
        if (fsm->pressed) {
            return PROTO_OK;
        }

        if (fsm->armed && !arm_window_open(fsm, now_ms)) {
            fsm->armed = false;
            fsm->confirm_press = false;
        }

        fsm->pressed = true;
        fsm->pressed_at_ms = now_ms;
        fsm->confirm_press = arm_window_open(fsm, now_ms);
        return PROTO_OK;
    }

    if (!fsm->pressed) {
        return PROTO_OK;
    }

    const uint32_t press_duration_ms = elapsed_ms(now_ms, fsm->pressed_at_ms);
    fsm->pressed = false;

    if (fsm->confirm_press) {
        fsm->confirm_press = false;
        if (arm_window_open(fsm, now_ms) && press_duration_ms < BUTTON_LONG_PRESS_MS) {
            fsm->armed = false;
            *action = BUTTON_ACTION_SELF_TEST_START;
        } else {
            fsm->armed = false;
            *action = BUTTON_ACTION_SELF_TEST_CANCELLED;
        }
        return PROTO_OK;
    }

    if (press_duration_ms >= BUTTON_LONG_PRESS_MS) {
        fsm->armed = true;
        fsm->armed_at_ms = now_ms;
        *action = BUTTON_ACTION_SELF_TEST_ARMED;
    } else {
        *action = BUTTON_ACTION_NORMAL_CLICK;
    }

    return PROTO_OK;
}

int status_select(const struct status_inputs *inputs,
                       struct status_indication *indication)
{
    if (inputs == NULL || indication == NULL) {
        return PROTO_ERR_ARG;
    }
    if (inputs->failure < SELF_TEST_FAILURE_NONE ||
        inputs->failure > SELF_TEST_FAILURE_INTERNAL) {
        return PROTO_ERR_ARG;
    }
    if (inputs->click_failure < CLICK_FAILURE_NONE ||
        inputs->click_failure > CLICK_FAILURE_INSUFFICIENT_RANGES) {
        return PROTO_ERR_ARG;
    }

    memset(indication, 0, sizeof(*indication));
    indication->pattern = STATUS_PATTERN_OFF;

    if (inputs->failure != SELF_TEST_FAILURE_NONE) {
        indication->pattern = STATUS_PATTERN_RED_BLINK_CODE;
        indication->red_blink_count = (uint8_t)inputs->failure;
        indication->repeat_count = STATUS_FAILURE_REPEAT_COUNT;
        return PROTO_OK;
    }

    if (inputs->click_failure != CLICK_FAILURE_NONE) {
        indication->pattern = STATUS_PATTERN_RED_BLINK_CODE;
        indication->red_blink_count = (uint8_t)inputs->click_failure;
        indication->repeat_count = STATUS_FAILURE_REPEAT_COUNT;
        return PROTO_OK;
    }

    if (inputs->self_test_running) {
        indication->pattern = STATUS_PATTERN_BLUE_CHASE;
        return PROTO_OK;
    }

    if (inputs->self_test_armed) {
        indication->pattern = STATUS_PATTERN_BLUE_PULSE;
        return PROTO_OK;
    }

    if (inputs->self_test_passed || inputs->click_accepted) {
        indication->pattern = STATUS_PATTERN_GREEN_SOLID;
        indication->duration_ms = STATUS_PASS_DURATION_MS;
        return PROTO_OK;
    }

    if (inputs->low_battery) {
        indication->pattern = STATUS_PATTERN_AMBER_BLINK_ONCE;
        return PROTO_OK;
    }

    if (inputs->charging) {
        indication->pattern = STATUS_PATTERN_AMBER_SLOW_BLINK;
        return PROTO_OK;
    }

    if (inputs->charged) {
        indication->pattern = STATUS_PATTERN_GREEN_SLOW_BLINK;
        return PROTO_OK;
    }

    return PROTO_OK;
}
