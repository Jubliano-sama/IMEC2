#include "app_mesh_coordinator_runtime.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void test_state_transition_is_reported_once(void)
{
    struct app_mesh_coordinator_runtime_capture capture = {0};
    struct app_mesh_coordinator_runtime_state state;
    struct app_mesh_coordinator_decision decision;
    bool state_changed;

    app_mesh_coordinator_runtime_reset(&state);
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_IDLE);

    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &state,
                                               &decision,
                                               &state_changed) == 0);
    assert(!state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_IDLE);
}

static void test_capture_sources_drive_coordinator_priority(void)
{
    const struct {
        struct app_mesh_coordinator_runtime_capture capture;
        enum app_mesh_coordinator_state expected_state;
        const char *expected_reason;
        bool mesh_work_allowed;
        bool route_wait_allowed;
        bool report_tx_allowed;
        bool uwb_rx_allowed;
    } cases[] = {
        { { .click_active = true }, APP_MESH_COORDINATOR_CLICK, "click",
          false, false, false, false },
        { { .survey_pending = true }, APP_MESH_COORDINATOR_SURVEY, "survey",
          false, false, false, false },
        { { .rx_queue_used = 1u }, APP_MESH_COORDINATOR_MESH_RX, "mesh-rx",
          true, false, false, true },
        { { .relay_tx_active = true }, APP_MESH_COORDINATOR_MESH_TX, "mesh-tx",
          true, true, false, false },
        { { .route_waiting_tx_active = true }, APP_MESH_COORDINATOR_MESH_TX,
          "mesh-tx", true, true, false, false },
        { { .ch9_ack_wait_active = true }, APP_MESH_COORDINATOR_MESH_RX,
          "ch9-ack-wait", true, false, false, true },
        { { .ch9_ack_send_pending = true }, APP_MESH_COORDINATOR_MESH_RX,
          "ch9-ack-send", true, false, false, true },
        { { .report_queue_used = 1u }, APP_MESH_COORDINATOR_MESH_TX, "mesh-tx",
          true, true, true, false },
        { { .gateway_continuous_ch9 = true }, APP_MESH_COORDINATOR_GATEWAY_RX,
          "gateway-rx", true, true, true, true },
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct app_mesh_coordinator_runtime_state state;
        struct app_mesh_coordinator_decision decision;
        bool state_changed;

        app_mesh_coordinator_runtime_reset(&state);
        assert(app_mesh_coordinator_runtime_decide(&cases[i].capture,
                                                   &state,
                                                   &decision,
                                                   &state_changed) == 0);
        assert(state_changed);
        assert(decision.state == cases[i].expected_state);
        assert(strcmp(decision.reason, cases[i].expected_reason) == 0);
        assert(decision.mesh_work_allowed == cases[i].mesh_work_allowed);
        assert(decision.route_wait_allowed == cases[i].route_wait_allowed);
        assert(decision.report_tx_allowed == cases[i].report_tx_allowed);
        assert(decision.uwb_rx_allowed == cases[i].uwb_rx_allowed);
    }
}

static void test_invalid_capture_does_not_advance_runtime_state(void)
{
    struct app_mesh_coordinator_runtime_state state;
    struct app_mesh_coordinator_decision decision = {0};
    bool state_changed = true;

    app_mesh_coordinator_runtime_reset(&state);
    assert(app_mesh_coordinator_runtime_decide(NULL,
                                               &state,
                                               &decision,
                                               &state_changed) == -EINVAL);

    assert(!state_changed);
    assert(!state.last_state_valid);
}

int main(void)
{
    test_state_transition_is_reported_once();
    test_capture_sources_drive_coordinator_priority();
    test_invalid_capture_does_not_advance_runtime_state();
    return 0;
}
