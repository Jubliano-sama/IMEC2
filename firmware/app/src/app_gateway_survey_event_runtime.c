#include "app_gateway_survey_event_runtime.h"

#include "survey.h"

#include <string.h>

static struct fw_event app_gateway_survey_event(
    const struct app_gateway_survey_event_runtime *runtime,
    enum fw_event_type type,
    uint8_t flags,
    uint64_t timestamp_ms)
{
    return (struct fw_event) {
        .timestamp_ms = timestamp_ms,
        .operation_id = runtime->operation_generation,
        .generation = survey_operation_session_id(
            runtime->operation_generation),
        .target_instance = 0u,
        .reply_instance = 0u,
        .target = FW_MACHINE_SURVEY,
        .reply_to = FW_MACHINE_NONE,
        .source = FW_EVENT_SOURCE_SERVICE,
        .type = type,
        .payload = {
            .flags = flags,
        },
    };
}

static enum fw_sm_result app_gateway_survey_dispatch(
    struct app_gateway_survey_event_runtime *runtime,
    const struct fw_event *event)
{
    struct fw_transition transition;

    return fw_survey_sm_handle(&runtime->survey,
                               event,
                               &transition);
}

void app_gateway_survey_event_runtime_init(
    struct app_gateway_survey_event_runtime *runtime)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    fw_survey_sm_init(&runtime->survey);
}

enum fw_sm_result app_gateway_survey_event_runtime_begin_selection(
    struct app_gateway_survey_event_runtime *runtime,
    uint64_t operation_generation,
    uint64_t timestamp_ms)
{
    if (runtime == NULL || operation_generation == 0u ||
        survey_operation_session_id(operation_generation) == 0u) {
        return FW_SM_INVALID;
    }
    if (runtime->survey.identity.active) {
        return FW_SM_BUSY;
    }

    app_gateway_survey_event_runtime_init(runtime);
    runtime->operation_generation = operation_generation;
    runtime->survey.identity.operation_id = operation_generation;
    runtime->survey.identity.generation = survey_operation_session_id(
        operation_generation);
    runtime->survey.identity.active = true;
    /* The legacy survey service has already reached this boundary. The
     * adapter records that identity and mirrors later phase events; it does
     * not own discovery or the rest of survey startup. */
    runtime->survey.state = FW_SURVEY_SELECT_PAIRS;
    (void)timestamp_ms;
    return FW_SM_APPLIED;
}

enum fw_sm_result app_gateway_survey_event_runtime_post(
    struct app_gateway_survey_event_runtime *runtime,
    enum fw_event_type type,
    uint8_t flags,
    uint64_t timestamp_ms)
{
    struct fw_event event;

    if (runtime == NULL || !runtime->survey.identity.active ||
        runtime->operation_generation == 0u ||
        type == FW_EVENT_NONE ||
        type == FW_EVENT_START) {
        return FW_SM_INVALID;
    }
    event = app_gateway_survey_event(runtime, type, flags, timestamp_ms);
    return app_gateway_survey_dispatch(runtime, &event);
}

enum fw_survey_state app_gateway_survey_event_runtime_state(
    const struct app_gateway_survey_event_runtime *runtime)
{
    return runtime == NULL ? FW_SURVEY_IDLE : runtime->survey.state;
}

bool app_gateway_survey_event_runtime_active(
    const struct app_gateway_survey_event_runtime *runtime)
{
    return runtime != NULL && runtime->survey.identity.active;
}
