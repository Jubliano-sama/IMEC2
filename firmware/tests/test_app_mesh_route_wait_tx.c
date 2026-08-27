#include "app_mesh_route_wait_tx.h"
#include "mesh.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define CHANNEL9_RETRY_MS 50u
#define BUSY_RETRY_MS 1000u

static struct app_mesh_route_wait_tx_state base_state(void)
{
    const struct app_mesh_route_wait_tx_state state = {
        .outbound_ready = true,
        .channel9_retry_delay_ms = CHANNEL9_RETRY_MS,
        .busy_retry_delay_ms = BUSY_RETRY_MS,
    };

    return state;
}

static void test_not_ready_schedules_route_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.outbound_ready = false;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY);
    assert(strcmp(decision.reason, "route-waiting-not-ready") == 0);
}

static void test_success_clears_waiting_packet(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = 0;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID);
}

static void test_unreachable_requests_route_once(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE);
    assert(strcmp(decision.reason, "route-waiting-packet") == 0);
}

static void test_unreachable_route_request_timeout_schedules_slow_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    state.route_request_attempted = true;
    state.route_request_ret = -ETIMEDOUT;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action ==
           APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-exhausted") == 0);
}

static void test_unreachable_successful_route_request_waits(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    state.route_request_attempted = true;
    state.route_request_ret = 0;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_NONE);
}

static void test_tx_timeout_schedules_slow_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -ETIMEDOUT;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action ==
           APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-stale") == 0);
}

static void test_busy_schedules_channel9_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EBUSY;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-channel9-event") == 0);
    assert(decision.delay_ms == CHANNEL9_RETRY_MS);
}

static void test_other_failure_schedules_busy_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EINVAL;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-busy") == 0);
    assert(decision.delay_ms == BUSY_RETRY_MS);
}

static void test_retained_owner_cannot_overwrite_generic_wait_slot(void)
{
    assert(app_mesh_route_wait_tx_may_store(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC));
    assert(!app_mesh_route_wait_tx_may_store(
        APP_MESH_ROUTE_WAIT_TX_OWNER_RETAINED_LOCAL));
}

static void test_rx_wake_preserves_armed_future_route_wait(void)
{
    assert(app_mesh_route_wait_tx_should_wake_from_rx(true, false));
    assert(!app_mesh_route_wait_tx_should_wake_from_rx(true, true));
    assert(!app_mesh_route_wait_tx_should_wake_from_rx(false, false));
}

static struct proto_packet route_wait_command_packet(void)
{
    const struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = UINT64_C(0x0102030405060708),
        .dst_id = UINT64_C(0x1112131415161718),
        .session_id = UINT32_C(0x21222324),
        .seq = UINT16_C(0x3132),
        .ttl = 8u,
        .payload_len = 37u,
        .message_age_ms = 5u,
    };

    return packet;
}

static void test_clear_requires_exact_owner_and_immutable_packet_identity(void)
{
    const struct proto_packet active = route_wait_command_packet();
    const uint8_t active_payload[37] = {0x11u, 0x22u, 0x33u};
    uint8_t active_digest[SEMANTIC_DIGEST_SHA256_LEN];
    struct proto_packet expected = active;

    assert(mesh_packet_semantic_digest(&active,
                                       active_payload,
                                       sizeof(active_payload),
                                       active_digest));
    expected.ttl--;
    expected.message_age_ms += 100u;
    assert(app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        active_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &expected,
        active_digest));

    assert(!app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK,
        &active,
        active_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &expected,
        active_digest));

#define ASSERT_IMMUTABLE_MUTATION_REJECTED(field, value)                      \
    do {                                                                       \
        expected = active;                                                     \
        expected.field = (value);                                              \
        assert(!app_mesh_route_wait_tx_clear_matches(                          \
            APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,                              \
            &active,                                                           \
            active_digest,                                                     \
            APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,                              \
            &expected,                                                         \
            active_digest));                                                   \
    } while (0)

    ASSERT_IMMUTABLE_MUTATION_REJECTED(msg_type, MSG_GATEWAY_ACK);
    ASSERT_IMMUTABLE_MUTATION_REJECTED(flags, 0u);
    ASSERT_IMMUTABLE_MUTATION_REJECTED(src_id, active.src_id + 1u);
    ASSERT_IMMUTABLE_MUTATION_REJECTED(dst_id, active.dst_id + 1u);
    ASSERT_IMMUTABLE_MUTATION_REJECTED(session_id, active.session_id + 1u);
    ASSERT_IMMUTABLE_MUTATION_REJECTED(seq, (uint16_t)(active.seq + 1u));
    ASSERT_IMMUTABLE_MUTATION_REJECTED(payload_len,
                                       (uint16_t)(active.payload_len + 1u));

#undef ASSERT_IMMUTABLE_MUTATION_REJECTED

    assert(!app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        NULL,
        active_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        active_digest));
    assert(!app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        active_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        NULL,
        active_digest));
    assert(!app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        NULL,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        active_digest));
}

static void test_clear_rejects_same_header_different_payload(void)
{
    const struct proto_packet active = route_wait_command_packet();
    const uint8_t original_payload[37] = {0x11u, 0x22u, 0x33u};
    uint8_t successor_payload[37] = {0x11u, 0x22u, 0x33u};
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t successor_digest[SEMANTIC_DIGEST_SHA256_LEN];

    assert(mesh_packet_semantic_digest(&active,
                                       original_payload,
                                       sizeof(original_payload),
                                       original_digest));
    successor_payload[sizeof(successor_payload) - 1u] = 0x44u;
    assert(mesh_packet_semantic_digest(&active,
                                       successor_payload,
                                       sizeof(successor_payload),
                                       successor_digest));
    assert(!app_mesh_route_wait_tx_clear_matches(
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        successor_digest,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        &active,
        original_digest));
}

int main(void)
{
    test_not_ready_schedules_route_retry();
    test_success_clears_waiting_packet();
    test_unreachable_requests_route_once();
    test_unreachable_route_request_timeout_schedules_slow_retry();
    test_unreachable_successful_route_request_waits();
    test_tx_timeout_schedules_slow_retry();
    test_busy_schedules_channel9_retry();
    test_other_failure_schedules_busy_retry();
    test_retained_owner_cannot_overwrite_generic_wait_slot();
    test_rx_wake_preserves_armed_future_route_wait();
    test_clear_requires_exact_owner_and_immutable_packet_identity();
    test_clear_rejects_same_header_different_payload();
    return 0;
}
