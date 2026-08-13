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

static void test_forced_relay_duplicate_probe_replies_stay_contact_only(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .relay_required = true,
        .direct_probe_ret = 0,
    };
    struct app_mesh_route_request_policy_decision decision;

    for (unsigned int attempt = 0u; attempt < 4u; attempt++) {
        decide(&state, &decision);
        assert(!decision.install_direct_route_from_probe);
        assert(!decision.direct_probe_satisfies_request);
        assert(decision.route_request_flags == MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED);
    }
}

static void test_forced_route_encodes_exact_gateway_relay_hops(void)
{
    const struct app_mesh_route_request_policy_state state = {
        .relay_required = true,
        .direct_probe_ret = 0,
        .required_gateway_relay_hops = 2u,
    };
    struct app_mesh_route_request_policy_decision decision;

    decide(&state, &decision);

    assert(!decision.install_direct_route_from_probe);
    assert(!decision.direct_probe_satisfies_request);
    assert((decision.route_request_flags &
            MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u);
    assert(MESH_ROUTE_REQ_REQUIRED_HOPS_DECODE(
               decision.route_request_flags) == 2u);
}

static void test_gateway_control_requires_exact_relay_depth(void)
{
    assert(app_mesh_gateway_control_relay_hops_allowed(8u, 8u, 0u));
    assert(app_mesh_gateway_control_relay_hops_allowed(8u, 7u, 1u));
    assert(app_mesh_gateway_control_relay_hops_allowed(8u, 6u, 2u));
    assert(!app_mesh_gateway_control_relay_hops_allowed(8u, 8u, 1u));
    assert(!app_mesh_gateway_control_relay_hops_allowed(8u, 7u, 2u));
    assert(!app_mesh_gateway_control_relay_hops_allowed(8u, 5u, 2u));
    assert(!app_mesh_gateway_control_relay_hops_allowed(0u, 0u, 1u));
    assert(!app_mesh_gateway_control_relay_hops_allowed(7u, 8u, 1u));
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

static void test_deferred_forward_accepts_new_request(void)
{
    const struct app_mesh_route_request_defer_state state = {
        .requested_due_ms = 4000u,
        .requested_reply_deadline_ms = 9000u,
    };
    struct app_mesh_route_request_defer_decision decision;

    app_mesh_route_request_defer_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_REQUEST_DEFER_ACCEPT_NEW);
    assert(decision.due_ms == 4000u);
    assert(decision.reply_deadline_ms == 9000u);
}

static void test_better_copy_replaces_same_request_without_restarting_deadline(void)
{
    const struct app_mesh_route_request_defer_state state = {
        .pending_due_ms = 4000u,
        .requested_due_ms = 4700u,
        .pending_reply_deadline_ms = 9000u,
        .requested_reply_deadline_ms = 9700u,
        .pending = true,
        .same_identity = true,
        .update_only = true,
    };
    struct app_mesh_route_request_defer_decision decision;

    app_mesh_route_request_defer_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_REQUEST_DEFER_REPLACE_SAME);
    assert(decision.due_ms == 4000u);
    assert(decision.reply_deadline_ms == 9000u);
}

static void test_deferred_forward_rejects_unrelated_or_orphan_update(void)
{
    struct app_mesh_route_request_defer_state state = {
        .pending_due_ms = 4000u,
        .requested_due_ms = 3500u,
        .pending = true,
    };
    struct app_mesh_route_request_defer_decision decision;

    app_mesh_route_request_defer_decide(&state, &decision);
    assert(decision.action == APP_MESH_ROUTE_REQUEST_DEFER_REJECT);

    state.pending = false;
    state.same_identity = true;
    state.update_only = true;
    app_mesh_route_request_defer_decide(&state, &decision);
    assert(decision.action == APP_MESH_ROUTE_REQUEST_DEFER_REJECT);
}

static void test_deferred_forward_due_delay_expires_to_zero(void)
{
    assert(app_mesh_route_request_defer_delay_ms(1000u, 0u) == 0u);
    assert(app_mesh_route_request_defer_delay_ms(1000u, 1000u) == 0u);
    assert(app_mesh_route_request_defer_delay_ms(1001u, 1000u) == 0u);
    assert(app_mesh_route_request_defer_delay_ms(999u, 1000u) == 1u);
    assert(app_mesh_route_request_defer_delay_ms(UINT32_MAX - 2u, 2u) == 5u);
    assert(app_mesh_route_request_defer_delay_ms(UINT32_MAX, 0u) == 1u);
    assert(app_mesh_route_request_defer_delay_ms(2u, UINT32_MAX - 2u) == 0u);
}

int main(void)
{
    test_normal_direct_probe_can_satisfy_route_request();
    test_forced_relay_probe_is_contact_only();
    test_forced_relay_duplicate_probe_replies_stay_contact_only();
    test_forced_route_encodes_exact_gateway_relay_hops();
    test_gateway_control_requires_exact_relay_depth();
    test_direct_bulk_suppression_keeps_probe_contact_only();
    test_failed_probe_does_not_satisfy_route_request();
    test_wake_train_radio_busy_defers_without_counting_attempt();
    test_control_tx_radio_busy_defers_unless_embedded_request_was_sent();
    test_non_busy_route_request_failure_keeps_normal_backoff_path();
    test_deferred_forward_accepts_new_request();
    test_better_copy_replaces_same_request_without_restarting_deadline();
    test_deferred_forward_rejects_unrelated_or_orphan_update();
    test_deferred_forward_due_delay_expires_to_zero();
    return 0;
}
