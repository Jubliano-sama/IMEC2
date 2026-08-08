#include "status.h"

#include <assert.h>

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

static void test_click_failure_no_anchor(void)
{
    struct status_inputs inputs = {
        .click_failure = CLICK_FAILURE_NO_ANCHOR,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
    assert(indication.red_blink_count == 1u);
    assert(indication.repeat_count == STATUS_FAILURE_REPEAT_COUNT);
}

static void test_click_failure_insufficient_ranges(void)
{
    struct status_inputs inputs = {
        .click_failure = CLICK_FAILURE_INSUFFICIENT_RANGES,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
    assert(indication.red_blink_count == 2u);
    assert(indication.repeat_count == STATUS_FAILURE_REPEAT_COUNT);
}

static void test_click_failure_takes_priority_over_battery(void)
{
    struct status_inputs inputs = {
        .click_failure = CLICK_FAILURE_INSUFFICIENT_RANGES,
        .low_battery = true,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
}

static void test_click_accepted_no_failure_shows_green(void)
{
    struct status_inputs inputs = {
        .click_accepted = true,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_GREEN_SOLID);
}

static void test_click_failure_overrides_click_accepted(void)
{
    struct status_inputs inputs = {
        .click_failure = CLICK_FAILURE_NO_ANCHOR,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
    assert(indication.red_blink_count == 1u);
}

static void test_self_test_failure_overrides_click_failure(void)
{
    struct status_inputs inputs = {
        .failure = SELF_TEST_FAILURE_UWB,
        .click_failure = CLICK_FAILURE_INSUFFICIENT_RANGES,
    };
    struct status_indication indication;

    assert(status_select(&inputs, &indication) == PROTO_OK);
    assert(indication.pattern == STATUS_PATTERN_RED_BLINK_CODE);
    assert(indication.red_blink_count == 5u);
}

int main(void)
{
    test_status_priority_and_codes();
    test_click_failure_no_anchor();
    test_click_failure_insufficient_ranges();
    test_click_failure_takes_priority_over_battery();
    test_click_accepted_no_failure_shows_green();
    test_click_failure_overrides_click_accepted();
    test_self_test_failure_overrides_click_failure();
    return 0;
}
