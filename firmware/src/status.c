#include "status.h"

#include <string.h>

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
