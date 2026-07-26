#include "app_gateway_survey_terminal.h"

struct app_gateway_survey_terminal_outcome
app_gateway_survey_terminal_outcome(
    enum gateway_survey_machine_terminal_reason reason)
{
    switch (reason) {
    case GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT:
    case GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_TIMEOUT:
    case GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_TIMEOUT,
            .reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RETRY_EXHAUSTED:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_RADIO_ERROR,
            .reason = GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_RADIO_ERROR,
            .reason = GATEWAY_COMMAND_EVENT_REASON_RADIO,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_TIMEOUT,
            .reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_INVALID_STATE,
            .reason = GATEWAY_COMMAND_EVENT_REASON_CAPACITY,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_OK,
            .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        };
    case GATEWAY_SURVEY_MACHINE_TERMINAL_NONE:
    case GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL:
    default:
        return (struct app_gateway_survey_terminal_outcome) {
            .status = COMMAND_INTERNAL_ERROR,
            .reason = GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
        };
    }
}

bool app_gateway_survey_terminal_is_count_mismatch(
    enum gateway_survey_machine_terminal_reason reason)
{
    return reason == GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS ||
           reason ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING ||
           reason ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED;
}
