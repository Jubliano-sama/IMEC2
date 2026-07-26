#ifndef APP_GATEWAY_SURVEY_TERMINAL_H
#define APP_GATEWAY_SURVEY_TERMINAL_H

#include "app_gateway_command_observability.h"
#include "gateway_survey_machine.h"

#include <stdbool.h>

struct app_gateway_survey_terminal_outcome {
    enum command_status status;
    enum gateway_command_event_reason reason;
};

struct app_gateway_survey_terminal_outcome
app_gateway_survey_terminal_outcome(
    enum gateway_survey_machine_terminal_reason reason);

bool app_gateway_survey_terminal_is_count_mismatch(
    enum gateway_survey_machine_terminal_reason reason);

#endif
