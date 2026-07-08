#include "app_mesh_route_request_policy.h"

#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void decide(const struct app_mesh_route_request_policy_state *state,
                   struct app_mesh_route_request_policy_decision *decision)
{
    memset(decision, 0xaa, sizeof(*decision));
    app_mesh_route_request_policy_decide(state, decision);
}

static void test_normal_direct_probe_can_satisfy_route_request(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .direct_probe_ret = 0,
    };
    struct app_mesh_route_request_policy_decision decision;

    decide(&state, &decision);

    assert(decision.install_direct_route_from_probe);
    assert(decision.direct_probe_satisfies_request);
    assert(decision.route_request_flags == 0u);
}

static void test_forced_relay_probe_is_contact_only(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .relay_required = true,
        .direct_probe_ret = 0,
    };
    struct app_mesh_route_request_policy_decision decision;

    decide(&state, &decision);

    assert(!decision.install_direct_route_from_probe);
    assert(!decision.direct_probe_satisfies_request);
    assert(decision.route_request_flags == MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED);
}

static void test_direct_bulk_suppression_keeps_probe_contact_only(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .direct_bulk_suppressed = true,
        .direct_probe_ret = 0,
    };
    struct app_mesh_route_request_policy_decision decision;

    decide(&state, &decision);

    assert(!decision.install_direct_route_from_probe);
    assert(!decision.direct_probe_satisfies_request);
    assert(decision.route_request_flags == 0u);
}

static void test_failed_probe_does_not_satisfy_route_request(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .direct_probe_ret = -1,
    };
    struct app_mesh_route_request_policy_decision decision;

    decide(&state, &decision);

    assert(decision.install_direct_route_from_probe);
    assert(!decision.direct_probe_satisfies_request);
    assert(decision.route_request_flags == 0u);
}

static void test_wake_train_radio_busy_defers_without_counting_attempt(void)
{
    struct app_mesh_route_request_rf_failure_decision decision = {0};

    app_mesh_route_request_rf_failure_decide(
        APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN,
        -EBUSY,
        false,
        -EBUSY,
        &decision);

    assert(decision.defer_retry);
    assert(decision.restore_prepared_attempt);
}

static void test_control_tx_radio_busy_defers_unless_embedded_request_was_sent(void)
{
    struct app_mesh_route_request_rf_failure_decision decision = {0};

    app_mesh_route_request_rf_failure_decide(
        APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
        -EBUSY,
        false,
        -EBUSY,
        &decision);

    assert(decision.defer_retry);
    assert(decision.restore_prepared_attempt);

    app_mesh_route_request_rf_failure_decide(
        APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
        -EBUSY,
        true,
        -EBUSY,
        &decision);

    assert(decision.defer_retry);
    assert(!decision.restore_prepared_attempt);
}

static void test_non_busy_route_request_failure_keeps_normal_backoff_path(void)
{
    struct app_mesh_route_request_rf_failure_decision decision = {
        .defer_retry = true,
        .restore_prepared_attempt = true,
    };

    app_mesh_route_request_rf_failure_decide(
        APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
        -5,
        false,
        -EBUSY,
        &decision);

    assert(!decision.defer_retry);
    assert(!decision.restore_prepared_attempt);
}

int main(void)
{
    test_normal_direct_probe_can_satisfy_route_request();
    test_forced_relay_probe_is_contact_only();
    test_direct_bulk_suppression_keeps_probe_contact_only();
    test_failed_probe_does_not_satisfy_route_request();
    test_wake_train_radio_busy_defers_without_counting_attempt();
    test_control_tx_radio_busy_defers_unless_embedded_request_was_sent();
    test_non_busy_route_request_failure_keeps_normal_backoff_path();
    return 0;
}
