#include "app_gateway_survey_terminal.h"

#include <assert.h>
#include <stdio.h>

static void assert_outcome(
    enum gateway_survey_machine_terminal_reason reason,
    enum command_status status,
    enum gateway_command_event_reason event_reason)
{
    const struct app_gateway_survey_terminal_outcome outcome =
        app_gateway_survey_terminal_outcome(reason);

    assert(outcome.status == status);
    assert(outcome.reason == event_reason);
}

int main(void)
{
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_NONE,
                   COMMAND_INTERNAL_ERROR,
                   GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT,
                   COMMAND_TIMEOUT,
                   GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_TIMEOUT,
                   COMMAND_TIMEOUT,
                   GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
    assert_outcome(
        GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RETRY_EXHAUSTED,
        COMMAND_RADIO_ERROR,
        GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO,
                   COMMAND_RADIO_ERROR,
                   GATEWAY_COMMAND_EVENT_REASON_RADIO);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS,
                   COMMAND_TIMEOUT,
                   GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING,
                   COMMAND_TIMEOUT,
                   GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED,
                   COMMAND_INVALID_STATE,
                   GATEWAY_COMMAND_EVENT_REASON_CAPACITY);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL,
                   COMMAND_INTERNAL_ERROR,
                   GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
    assert_outcome(GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED,
                   COMMAND_OK,
                   GATEWAY_COMMAND_EVENT_REASON_NONE);

    assert(app_gateway_survey_terminal_is_count_mismatch(
        GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS));
    assert(app_gateway_survey_terminal_is_count_mismatch(
        GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING));
    assert(app_gateway_survey_terminal_is_count_mismatch(
        GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED));
    assert(!app_gateway_survey_terminal_is_count_mismatch(
        GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT));

    puts("gateway survey terminal mapping tests passed");
    return 0;
}
