#ifndef APP_GATEWAY_SURVEY_OBSERVABILITY_H
#define APP_GATEWAY_SURVEY_OBSERVABILITY_H

#include "app_gateway_command_observability.h"
#include "survey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_GATEWAY_SURVEY_OBSERVABILITY_RAM_BUDGET_BYTES 160u

typedef int (*app_gateway_survey_observability_emit_fn)(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx);

struct app_gateway_survey_observability_ops {
    app_gateway_survey_observability_emit_fn emit_if_available;
    void *ctx;
};

struct app_gateway_survey_observability_state {
    struct gateway_command_event boundary_event;
    size_t report_cursor;
    uint32_t pending_event_seq;
    bool collection_complete;
    bool schedule_complete;
    bool boundary_pending;
};

void app_gateway_survey_observability_reset(
    struct app_gateway_survey_observability_state *state);
int app_gateway_survey_observability_emit_collection_next(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops,
    const struct survey_gateway_context *survey,
    const struct gateway_command_event *base_event,
    uint16_t duplicate_count);
int app_gateway_survey_observability_submit_boundary(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops,
    const struct gateway_command_event *event);
int app_gateway_survey_observability_flush_boundary(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops);

#endif
