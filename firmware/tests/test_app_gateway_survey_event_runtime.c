#include "app_gateway_survey_event_runtime.h"

#include <assert.h>
#include <stdint.h>

static void post(struct app_gateway_survey_event_runtime *runtime,
                 enum fw_event_type type,
                 uint8_t flags)
{
    assert(app_gateway_survey_event_runtime_post(runtime,
                                                  type,
                                                  flags,
                                                  100u) == FW_SM_APPLIED);
}

static void reach_publish(struct app_gateway_survey_event_runtime *runtime)
{
    app_gateway_survey_event_runtime_init(runtime);
    assert(app_gateway_survey_event_runtime_begin_selection(
               runtime,
               UINT64_C(0x0000000127182818),
               1u) == FW_SM_APPLIED);
    post(runtime, FW_EVENT_PAIR_AVAILABLE, 0u);
    post(runtime, FW_EVENT_PAIR_ARMED, 0u);
    post(runtime, FW_EVENT_PAIR_RESULT_RECEIVED, 0u);
    post(runtime, FW_EVENT_GRAPH_BUILT, 0u);
    post(runtime, FW_EVENT_NO_PAIR_AVAILABLE, 0u);
    assert(app_gateway_survey_event_runtime_state(runtime) ==
           FW_SURVEY_PUBLISH);
}

static void test_complete_preserves_full_generation(void)
{
    struct app_gateway_survey_event_runtime runtime;

    reach_publish(&runtime);
    assert(runtime.operation_generation == UINT64_C(0x0000000127182818));
    assert(runtime.survey.identity.operation_id ==
           UINT64_C(0x0000000127182818));
    assert(runtime.survey.identity.generation == UINT32_C(0x27182818));
    post(&runtime, FW_EVENT_SURVEY_COMPLETE, FW_EVENT_FLAG_GRAPH_COMPLETE);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_COMPLETE);
    assert(!app_gateway_survey_event_runtime_active(&runtime));
}

static void test_partial_never_becomes_complete(void)
{
    struct app_gateway_survey_event_runtime runtime;

    reach_publish(&runtime);
    post(&runtime, FW_EVENT_SURVEY_PARTIAL, 0u);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_PARTIAL);
    assert(!app_gateway_survey_event_runtime_active(&runtime));
    assert(app_gateway_survey_event_runtime_post(
               &runtime,
               FW_EVENT_SURVEY_COMPLETE,
               FW_EVENT_FLAG_GRAPH_COMPLETE,
               101u) == FW_SM_INVALID);
}

static void test_pair_armed_is_single_transition(void)
{
    struct app_gateway_survey_event_runtime runtime;

    app_gateway_survey_event_runtime_init(&runtime);
    assert(app_gateway_survey_event_runtime_begin_selection(
               &runtime,
               UINT64_C(0x0000000127182818),
               1u) == FW_SM_APPLIED);
    post(&runtime, FW_EVENT_PAIR_AVAILABLE, 0u);
    post(&runtime, FW_EVENT_PAIR_ARMED, 0u);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_WAIT_RESULTS);
    assert(app_gateway_survey_event_runtime_post(
               &runtime,
               FW_EVENT_PAIR_ARMED,
               0u,
               100u) == FW_SM_INVALID);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_WAIT_RESULTS);
}

static void test_timeout_updates_graph_without_result(void)
{
    struct app_gateway_survey_event_runtime runtime;

    app_gateway_survey_event_runtime_init(&runtime);
    assert(app_gateway_survey_event_runtime_begin_selection(
               &runtime,
               UINT64_C(0x0000000127182818),
               1u) == FW_SM_APPLIED);
    post(&runtime, FW_EVENT_PAIR_AVAILABLE, 0u);
    post(&runtime, FW_EVENT_PAIR_ARMED, 0u);
    post(&runtime, FW_EVENT_TIMER_EXPIRED, 0u);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_UPDATE_GRAPH);
    post(&runtime, FW_EVENT_GRAPH_BUILT, 0u);
    assert(app_gateway_survey_event_runtime_state(&runtime) ==
           FW_SURVEY_SELECT_PAIRS);
}

static void test_invalid_selection_boundary_is_rejected(void)
{
    struct app_gateway_survey_event_runtime runtime;

    app_gateway_survey_event_runtime_init(&runtime);
    assert(app_gateway_survey_event_runtime_begin_selection(&runtime,
                                                  0u,
                                                  0u) == FW_SM_INVALID);
    assert(app_gateway_survey_event_runtime_begin_selection(&runtime,
                                                  UINT64_C(0x100000001),
                                                  0u) == FW_SM_APPLIED);
    assert(app_gateway_survey_event_runtime_begin_selection(&runtime,
                                                   UINT64_C(0x100000002),
                                                   0u) == FW_SM_BUSY);
    assert(app_gateway_survey_event_runtime_post(
               &runtime,
               FW_EVENT_CONFIG_SENT,
               0u,
               0u) == FW_SM_INVALID);
    assert(app_gateway_survey_event_runtime_post(&runtime,
                                                 FW_EVENT_START,
                                                 0u,
                                                 0u) == FW_SM_INVALID);
}

int main(void)
{
    test_complete_preserves_full_generation();
    test_partial_never_becomes_complete();
    test_pair_armed_is_single_transition();
    test_timeout_updates_graph_without_result();
    test_invalid_selection_boundary_is_rejected();
    return 0;
}
