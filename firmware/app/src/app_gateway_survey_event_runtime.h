#ifndef APP_GATEWAY_SURVEY_EVENT_RUNTIME_H
#define APP_GATEWAY_SURVEY_EVENT_RUNTIME_H

#include "firmware_state_machines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The application owns the wire-format survey context and packet custody.
 * This seam owns only the high-level lifecycle and its immutable identity.
 * operation_generation is retained in full; the state-machine generation is
 * the protocol session projection used by fw_event matching.
 */
struct app_gateway_survey_event_runtime {
    struct fw_survey_sm survey;
    uint64_t operation_generation;
};

void app_gateway_survey_event_runtime_init(
    struct app_gateway_survey_event_runtime *runtime);

/*
 * Bounded migration seam: discovery, report collection, graph construction,
 * pair control, deadlines, and packet custody remain owned by the existing
 * gateway service. Establish the event-machine identity only at the real
 * immutable-plan boundary, immediately before pair selection begins. This
 * does not start survey discovery or claim full survey startup ownership, and
 * it avoids replaying completed phases as synthetic events.
 */
enum fw_sm_result app_gateway_survey_event_runtime_begin_selection(
    struct app_gateway_survey_event_runtime *runtime,
    uint64_t operation_generation,
    uint64_t timestamp_ms);

enum fw_sm_result app_gateway_survey_event_runtime_post(
    struct app_gateway_survey_event_runtime *runtime,
    enum fw_event_type type,
    uint8_t flags,
    uint64_t timestamp_ms);

enum fw_survey_state app_gateway_survey_event_runtime_state(
    const struct app_gateway_survey_event_runtime *runtime);

bool app_gateway_survey_event_runtime_active(
    const struct app_gateway_survey_event_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
