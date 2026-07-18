#include "mesh_sim.h"

#include <stdio.h>
#include <string.h>

#define ROUTE_EPOCH UINT32_C(17)
#define GATEWAY_ID UINT64_C(0x9000)
#define ANCHOR_ID UINT64_C(0xA100)
#define CHILD_ID UINT64_C(0xB001)

#define SEED_CONTRACT_ORDER UINT32_C(0xA17E5001)
#define SEED_DEFER_DS_TWR UINT32_C(0xA17E5002)
#define SEED_DEFER_TRANSIT UINT32_C(0xA17E5003)
#define SEED_CLICK_ONE UINT32_C(0xA17E5010)
#define SEED_CLICK_TEN UINT32_C(0xA17E5011)
#define SEED_COMMAND_BURST UINT32_C(0xA17E5020)
#define SEED_QUEUE_PRESSURE UINT32_C(0xA17E5030)
#define SEED_REPEATED_PREEMPTION UINT32_C(0xA17E5040)

#define INITIAL_DS_TWR_DURATION_US UINT64_C(5000)
#define COMMAND_DURATION_US UINT64_C(1000)
#define CLICK_DURATION_US UINT64_C(4000)
#define TRANSIT_DURATION_US UINT64_C(2000)
#define CLICK_READY_INTERVAL_US UINT64_C(250)
#define CLICK_COMPLETION_BOUND_US UINT64_C(100000)
#define WATCHDOG_TIMEOUT_US UINT64_C(25000)
#define STALL_LIMIT_US UINT64_C(45000000)

#define COMMAND_BURST_COUNT 8u
#define COMMAND_BURST_CLICK_COUNT 6u
#define SPAM_PREEMPTION_CYCLES 64u
#define RUNTIME_RESET_INTERVAL 8u
#define TRACE_CAPACITY 512u
#define SCENARIO_EVENT_BOUND 512u
#define LOGICAL_EVENT_BOUND 1024u

struct test_context {
    const char *scenario;
    const char *phase;
    uint32_t seed;
    uint64_t now_us;
};

struct action_record {
    enum mesh_runtime_action_kind action;
    uint64_t token;
    uint64_t at_us;
};

struct runtime_fixture {
    struct mesh_sim_world world;
    struct action_record traces[TRACE_CAPACITY];
    size_t trace_count;
    size_t schedule_count;
    uint8_t child;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t upstream_connection;
    uint16_t downstream_connection;
    bool trace_overflow;
    bool schedule_overflow;
};

static struct runtime_fixture fixture;

static int fail_requirement(const struct test_context *context,
                            int line,
                            const char *expression)
{
    fprintf(stderr,
            "scenario=%s seed=0x%08x phase=%s time_us=%llu line=%d assertion=%s\n",
            context->scenario,
            (unsigned int)context->seed,
            context->phase,
            (unsigned long long)context->now_us,
            line,
            expression);
    return 1;
}

#define REQUIRE(context, expression) do { \
    if (!(expression)) { \
        return fail_requirement((context), __LINE__, #expression); \
    } \
} while (0)

static int schedule_record_cb(enum mesh_runtime_work_kind kind,
                              uint64_t token,
                              uint64_t at_us,
                              void *ctx)
{
    struct runtime_fixture *current = ctx;

    (void)kind;
    (void)token;
    (void)at_us;
    if (current == NULL) {
        return MESH_RUNTIME_ERR_ARG;
    }
    current->schedule_count++;
    if (current->schedule_count > TRACE_CAPACITY) {
        current->schedule_overflow = true;
        return MESH_RUNTIME_ERR_CAPACITY;
    }
    return MESH_RUNTIME_OK;
}

static void trace_record_cb(enum mesh_runtime_action_kind action,
                            uint64_t token,
                            uint64_t at_us,
                            void *ctx)
{
    struct runtime_fixture *current = ctx;

    if (current == NULL) {
        return;
    }
    if (current->trace_count >= TRACE_CAPACITY) {
        current->trace_overflow = true;
        return;
    }
    current->traces[current->trace_count++] = (struct action_record) {
        .action = action,
        .token = token,
        .at_us = at_us,
    };
}

static struct mesh_runtime *fixture_runtime(struct runtime_fixture *current)
{
    return &current->world.roles[current->anchor].runtime;
}

static struct mesh_relay *fixture_relay(struct runtime_fixture *current)
{
    return &current->world.roles[current->anchor].relay;
}

static bool channel9_timing_present(const struct mesh_relay *relay,
                                    uint64_t peer_id)
{
    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static size_t runtime_work_count(const struct mesh_runtime *runtime)
{
    size_t count = 0u;

    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        if (runtime->work[i].valid) {
            count++;
        }
    }
    return count;
}

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 260u,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 8u,
        .supervision_timeout_ms = 20000u,
    };
}

static int init_fixture(struct test_context *context,
                        struct runtime_fixture *current)
{
    struct mesh_event_params upstream_params = connection_params(100u);
    struct mesh_event_params downstream_params = connection_params(130u);
    struct mesh_runtime_ops ops = {
        .schedule = schedule_record_cb,
        .trace = trace_record_cb,
        .ctx = current,
    };

    memset(current, 0, sizeof(*current));
    mesh_sim_init(&current->world, context->seed);
    REQUIRE(context,
            mesh_sim_add_role(&current->world, MESH_SIM_ROLE_TRANSMITTER,
                              CHILD_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &current->child) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_add_role(&current->world, MESH_SIM_ROLE_ANCHOR,
                              ANCHOR_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &current->anchor) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_add_role(&current->world, MESH_SIM_ROLE_GATEWAY,
                              GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                              &current->gateway) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_set_link(&current->world, current->child,
                              current->anchor, 98u, 0u) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_set_link(&current->world, current->anchor,
                              current->gateway, 98u, 0u) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_install_route(&current->world, current->anchor,
                                   current->gateway, 0u,
                                   ROUTE_EPOCH) == PROTO_OK);
    REQUIRE(context,
            mesh_sim_install_downlink(&current->world, current->anchor,
                                      CHILD_ID, current->child, 1u,
                                      ROUTE_EPOCH) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_add_connection(&current->world, current->anchor,
                                    current->gateway, &upstream_params, true,
                                    &current->upstream_connection) == MESH_SIM_OK);
    REQUIRE(context,
            mesh_sim_add_connection(&current->world, current->child,
                                    current->anchor, &downstream_params, true,
                                    &current->downstream_connection) == MESH_SIM_OK);
    REQUIRE(context,
            current->upstream_connection != current->downstream_connection);

    mesh_runtime_init(fixture_runtime(current),
                      fixture_relay(current),
                      ANCHOR_ID,
                      &ops);
    REQUIRE(context, channel9_timing_present(fixture_relay(current), GATEWAY_ID));
    REQUIRE(context, channel9_timing_present(fixture_relay(current), CHILD_ID));
    REQUIRE(context, current->world.event_count == 0u);
    return 0;
}

static int restore_downstream_timing(struct runtime_fixture *current)
{
    const struct mesh_sim_connection *connection =
        &current->world.connections[current->downstream_connection];

    if (connection->node_b != current->anchor) {
        return PROTO_ERR_ARG;
    }
    return mesh_relay_set_channel9_timing(fixture_relay(current),
                                          CHILD_ID,
                                          &connection->timing_b);
}

static void reset_runtime_state(struct runtime_fixture *current)
{
    struct mesh_runtime *runtime = fixture_runtime(current);
    struct mesh_runtime_ops ops = runtime->ops;

    mesh_runtime_init(runtime, fixture_relay(current), ANCHOR_ID, &ops);
}

static struct mesh_outbound transit_outbound(uint16_t token)
{
    struct mesh_outbound outbound;

    memset(&outbound, 0, sizeof(outbound));
    outbound.packet.msg_type = MSG_MESH_DATA;
    outbound.packet.src_id = CHILD_ID;
    outbound.packet.dst_id = GATEWAY_ID;
    outbound.packet.seq = token;
    outbound.next_hop_id = GATEWAY_ID;
    return outbound;
}

static int reserve_transit(struct runtime_fixture *current,
                           uint16_t token,
                           uint64_t ready_us)
{
    struct mesh_outbound outbound = transit_outbound(token);

    return mesh_runtime_reserve_transit(fixture_runtime(current),
                                        &outbound,
                                        ready_us);
}

static int expect_wait(struct test_context *context,
                       struct runtime_fixture *current,
                       uint64_t now_us,
                       uint64_t boundary_us)
{
    struct mesh_runtime_action action;

    context->now_us = now_us;
    REQUIRE(context,
            mesh_runtime_run_boundary(fixture_runtime(current), now_us,
                                      &action) == MESH_RUNTIME_OK);
    REQUIRE(context,
            action.kind == MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY &&
            action.runnable_at_us == boundary_us);
    return 0;
}

static int expect_action(struct test_context *context,
                         struct runtime_fixture *current,
                         uint64_t now_us,
                         enum mesh_runtime_action_kind expected,
                         uint64_t token,
                         struct mesh_runtime_action *action)
{
    context->now_us = now_us;
    REQUIRE(context,
            mesh_runtime_run_boundary(fixture_runtime(current), now_us,
                                      action) == MESH_RUNTIME_OK);
    REQUIRE(context,
            action->kind == expected && action->token == token &&
            action->runnable_at_us == now_us);
    REQUIRE(context, !current->trace_overflow);
    return 0;
}

static int expect_none(struct test_context *context,
                       struct runtime_fixture *current,
                       uint64_t now_us)
{
    struct mesh_runtime_action action;

    context->now_us = now_us;
    REQUIRE(context,
            mesh_runtime_run_boundary(fixture_runtime(current), now_us,
                                      &action) == MESH_RUNTIME_OK);
    REQUIRE(context, action.kind == MESH_RUNTIME_ACTION_NONE);
    return 0;
}

static int claim_radio(struct test_context *context,
                       struct runtime_fixture *current,
                       enum mesh_runtime_radio_owner owner,
                       uint64_t end_us)
{
    struct mesh_runtime *runtime = fixture_runtime(current);

    REQUIRE(context, end_us > context->now_us);
    REQUIRE(context, mesh_runtime_radio_safe(runtime, context->now_us));
    REQUIRE(context,
            mesh_runtime_claim_radio(runtime, owner, context->now_us,
                                     end_us) == MESH_RUNTIME_OK);
    REQUIRE(context, runtime->radio_owner == owner);
    REQUIRE(context, !mesh_runtime_radio_safe(runtime, context->now_us));
    return 0;
}

static int release_radio(struct test_context *context,
                         struct runtime_fixture *current,
                         enum mesh_runtime_radio_owner owner,
                         uint64_t end_us)
{
    struct mesh_runtime *runtime = fixture_runtime(current);

    context->now_us = end_us;
    REQUIRE(context,
            mesh_runtime_release_radio(runtime, owner, end_us) ==
            MESH_RUNTIME_OK);
    REQUIRE(context, runtime->radio_owner == MESH_RUNTIME_RADIO_NONE);
    REQUIRE(context, mesh_runtime_radio_safe(runtime, end_us));
    return 0;
}

static int complete_action(struct test_context *context,
                           struct runtime_fixture *current,
                           enum mesh_runtime_radio_owner owner,
                           uint64_t duration_us,
                           bool feed_watchdog)
{
    uint64_t start_us = context->now_us;
    uint64_t end_us = start_us + duration_us;

    REQUIRE(context, end_us > start_us);
    REQUIRE(context, claim_radio(context, current, owner, end_us) == 0);
    REQUIRE(context,
            expect_wait(context, current, start_us + duration_us / 2u,
                        end_us) == 0);
    if (feed_watchdog) {
        REQUIRE(context,
                mesh_sim_run_until(&current->world, end_us) == MESH_SIM_OK);
        REQUIRE(context, current->world.now_us == end_us);
    }
    REQUIRE(context, release_radio(context, current, owner, end_us) == 0);
    if (feed_watchdog) {
        REQUIRE(context,
                mesh_sim_watchdog_feed(&current->world, current->anchor) ==
                MESH_SIM_OK);
    }
    return 0;
}

static int require_bounded_events(struct test_context *context,
                                  const struct runtime_fixture *current)
{
    size_t logical_events = current->schedule_count + current->trace_count +
                            current->world.event_count +
                            current->world.transition_count;

    REQUIRE(context, !current->schedule_overflow && !current->trace_overflow);
    REQUIRE(context, current->schedule_count <= SCENARIO_EVENT_BOUND);
    REQUIRE(context, current->trace_count <= SCENARIO_EVENT_BOUND);
    REQUIRE(context, current->world.event_count <= SCENARIO_EVENT_BOUND);
    REQUIRE(context, current->world.transition_count <= SCENARIO_EVENT_BOUND);
    REQUIRE(context, logical_events <= LOGICAL_EVENT_BOUND);
    REQUIRE(context, context->now_us < STALL_LIMIT_US);
    REQUIRE(context, current->world.last_error == MESH_SIM_OK);
    return 0;
}

static int test_contract_order_at_safe_boundary(void)
{
    struct test_context context = {
        .scenario = "contract-order",
        .phase = "setup",
        .seed = SEED_CONTRACT_ORDER,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    struct mesh_relay *relay;
    uint64_t local_complete_us;

    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);
    relay = fixture_relay(&fixture);

    context.phase = "queue-transit-click-command";
    REQUIRE(&context, reserve_transit(&fixture, 100u, 0u) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                1u, 0u) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                1001u, 0u) == MESH_RUNTIME_OK);

    context.phase = "initial-ds-twr-active";
    REQUIRE(&context,
            claim_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context,
            expect_wait(&context, &fixture, 0u,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context,
            expect_wait(&context, &fixture,
                        INITIAL_DS_TWR_DURATION_US - 1u,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context, fixture.trace_count == 0u);

    context.phase = "first-safe-boundary-command";
    REQUIRE(&context,
            release_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                          INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                          1001u, &action) == 0);
    REQUIRE(&context, runtime->transit_reserved);
    REQUIRE(&context, runtime->transit_abandon_count == 0u);
    REQUIRE(&context, channel9_timing_present(relay, GATEWAY_ID));
    REQUIRE(&context, channel9_timing_present(relay, CHILD_ID));
    REQUIRE(&context,
            complete_action(&context, &fixture,
                            MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                            COMMAND_DURATION_US, false) == 0);

    context.phase = "local-click-before-transit";
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                          1u, &action) == 0);
    REQUIRE(&context, action.transit_reservation_abandoned);
    REQUIRE(&context, !runtime->transit_reserved);
    REQUIRE(&context, runtime->transit_abandon_count == 1u);
    REQUIRE(&context, channel9_timing_present(relay, GATEWAY_ID));
    REQUIRE(&context, !channel9_timing_present(relay, CHILD_ID));
    REQUIRE(&context, mesh_relay_find_downlink(relay, CHILD_ID) != NULL);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                            CLICK_DURATION_US, false) == 0);
    local_complete_us = context.now_us;
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "bounded-transit-recovery";
    REQUIRE(&context,
            reserve_transit(&fixture, 101u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_TRANSIT,
                          101u, &action) == 0);
    REQUIRE(&context, action.runnable_at_us >= local_complete_us);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                            TRANSIT_DURATION_US, false) == 0);
    REQUIRE(&context,
            context.now_us - local_complete_us <= TRANSIT_DURATION_US);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "contract-order-bounds";
    REQUIRE(&context, fixture.trace_count == 3u);
    REQUIRE(&context,
            fixture.traces[0].action ==
                MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND &&
            fixture.traces[1].action ==
                MESH_RUNTIME_ACTION_START_LOCAL_CLICK &&
            fixture.traces[2].action == MESH_RUNTIME_ACTION_RUN_TRANSIT);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

static int test_command_deferral_variants(void)
{
    struct test_context context = {
        .scenario = "command-deferral",
        .phase = "ds-twr-setup",
        .seed = SEED_DEFER_DS_TWR,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    uint64_t click_start_us;
    uint64_t click_end_us;

    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);
    REQUIRE(&context, reserve_transit(&fixture, 200u, 0u) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                1u, 0u) == MESH_RUNTIME_OK);

    context.phase = "command-arrives-during-ds-twr";
    REQUIRE(&context,
            claim_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    context.now_us = INITIAL_DS_TWR_DURATION_US / 2u;
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                2001u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_wait(&context, &fixture, context.now_us,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context, fixture.trace_count == 0u);
    REQUIRE(&context,
            release_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                          INITIAL_DS_TWR_DURATION_US) == 0);

    context.phase = "ds-twr-boundary-command-wins";
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                          2001u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture,
                            MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                            COMMAND_DURATION_US, false) == 0);

    context.phase = "command-arrives-during-click";
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                          1u, &action) == 0);
    REQUIRE(&context, action.transit_reservation_abandoned);
    click_start_us = context.now_us;
    click_end_us = click_start_us + CLICK_DURATION_US;
    REQUIRE(&context,
            claim_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                        click_end_us) == 0);
    context.now_us = click_start_us + CLICK_DURATION_US / 2u;
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                2002u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                2u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_wait(&context, &fixture, context.now_us, click_end_us) == 0);
    REQUIRE(&context, fixture.trace_count == 2u);
    REQUIRE(&context,
            release_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                          click_end_us) == 0);

    context.phase = "click-boundary-command-wins";
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                          2002u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture,
                            MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                            COMMAND_DURATION_US, false) == 0);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                          2u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                            CLICK_DURATION_US, false) == 0);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "ds-twr-click-deferral-bounds";
    REQUIRE(&context, fixture.trace_count == 4u);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);

    context.seed = SEED_DEFER_TRANSIT;
    context.phase = "active-transit-setup";
    context.now_us = 0u;
    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);
    REQUIRE(&context, reserve_transit(&fixture, 201u, 0u) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_action(&context, &fixture, 0u,
                          MESH_RUNTIME_ACTION_RUN_TRANSIT,
                          201u, &action) == 0);

    context.phase = "command-arrives-during-active-transit";
    REQUIRE(&context,
            claim_radio(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                        TRANSIT_DURATION_US) == 0);
    context.now_us = TRANSIT_DURATION_US / 2u;
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                3u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                2003u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_wait(&context, &fixture, context.now_us,
                        TRANSIT_DURATION_US) == 0);
    REQUIRE(&context, fixture.trace_count == 1u);
    REQUIRE(&context,
            release_radio(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                          TRANSIT_DURATION_US) == 0);

    context.phase = "transit-boundary-command-wins";
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                          2003u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture,
                            MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                            COMMAND_DURATION_US, false) == 0);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                          3u, &action) == 0);
    REQUIRE(&context, !action.transit_reservation_abandoned);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                            CLICK_DURATION_US, false) == 0);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "active-transit-deferral-bounds";
    REQUIRE(&context, fixture.trace_count == 3u);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

static int test_click_load(const char *scenario,
                           uint32_t seed,
                           size_t click_count)
{
    struct test_context context = {
        .scenario = scenario,
        .phase = "setup",
        .seed = seed,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    uint64_t local_complete_us;
    char phase[48];

    REQUIRE(&context, click_count > 0u);
    REQUIRE(&context, click_count < MESH_RUNTIME_WORK_CAPACITY);
    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);

    context.phase = "queue-click-load";
    REQUIRE(&context, reserve_transit(&fixture, 300u, 0u) == MESH_RUNTIME_OK);
    for (size_t i = 0u; i < click_count; i++) {
        REQUIRE(&context,
                mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                    i + 1u,
                                    i * CLICK_READY_INTERVAL_US) ==
                MESH_RUNTIME_OK);
    }

    for (size_t i = 0u; i < click_count; i++) {
        (void)snprintf(phase, sizeof(phase), "click-%u-start",
                       (unsigned int)(i + 1u));
        context.phase = phase;
        REQUIRE(&context,
                expect_action(&context, &fixture, context.now_us,
                              MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                              i + 1u, &action) == 0);
        REQUIRE(&context,
                action.transit_reservation_abandoned == (i == 0u));
        REQUIRE(&context,
                complete_action(&context, &fixture,
                                MESH_RUNTIME_RADIO_DS_TWR,
                                CLICK_DURATION_US, false) == 0);
        REQUIRE(&context,
                context.now_us - i * CLICK_READY_INTERVAL_US <=
                CLICK_COMPLETION_BOUND_US);
    }
    local_complete_us = context.now_us;
    REQUIRE(&context, fixture.trace_count == click_count);
    for (size_t i = 0u; i < click_count; i++) {
        REQUIRE(&context,
                fixture.traces[i].action ==
                    MESH_RUNTIME_ACTION_START_LOCAL_CLICK &&
                fixture.traces[i].token == i + 1u);
    }
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "post-click-transit-recovery";
    REQUIRE(&context,
            reserve_transit(&fixture, 301u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_TRANSIT,
                          301u, &action) == 0);
    REQUIRE(&context, action.runnable_at_us >= local_complete_us);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                            TRANSIT_DURATION_US, false) == 0);
    REQUIRE(&context,
            context.now_us - local_complete_us <= TRANSIT_DURATION_US);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "click-load-bounds";
    REQUIRE(&context, fixture_runtime(&fixture)->transit_abandon_count == 1u);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

static int test_multiple_gateway_commands(void)
{
    static const uint64_t command_tokens[COMMAND_BURST_COUNT] = {
        4108u, 4101u, 4107u, 4102u, 4106u, 4103u, 4105u, 4104u,
    };
    struct test_context context = {
        .scenario = "ordered-command-burst",
        .phase = "setup",
        .seed = SEED_COMMAND_BURST,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    uint64_t command_start_us;
    char phase[48];

    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);

    context.phase = "queue-command-click-transit-burst";
    REQUIRE(&context, reserve_transit(&fixture, 400u, 0u) == MESH_RUNTIME_OK);
    for (size_t i = 0u; i < COMMAND_BURST_CLICK_COUNT; i++) {
        REQUIRE(&context,
                mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                    i + 1u, 0u) == MESH_RUNTIME_OK);
    }
    for (size_t i = 0u; i < COMMAND_BURST_COUNT; i++) {
        REQUIRE(&context,
                mesh_runtime_submit(runtime,
                                    MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                    command_tokens[i], 0u) == MESH_RUNTIME_OK);
    }

    context.phase = "command-burst-initial-ds-twr";
    REQUIRE(&context,
            claim_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context,
            expect_wait(&context, &fixture, 0u,
                        INITIAL_DS_TWR_DURATION_US) == 0);
    REQUIRE(&context,
            release_radio(&context, &fixture, MESH_RUNTIME_RADIO_DS_TWR,
                          INITIAL_DS_TWR_DURATION_US) == 0);
    command_start_us = context.now_us;

    for (size_t i = 0u; i < COMMAND_BURST_COUNT; i++) {
        (void)snprintf(phase, sizeof(phase), "command-%u",
                       (unsigned int)(i + 1u));
        context.phase = phase;
        REQUIRE(&context,
                expect_action(&context, &fixture, context.now_us,
                              MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                              command_tokens[i], &action) == 0);
        REQUIRE(&context,
                complete_action(&context, &fixture,
                                MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                                COMMAND_DURATION_US, false) == 0);
        REQUIRE(&context,
                context.now_us - command_start_us <=
                COMMAND_BURST_COUNT * COMMAND_DURATION_US);
    }

    for (size_t i = 0u; i < COMMAND_BURST_CLICK_COUNT; i++) {
        (void)snprintf(phase, sizeof(phase), "post-command-click-%u",
                       (unsigned int)(i + 1u));
        context.phase = phase;
        REQUIRE(&context,
                expect_action(&context, &fixture, context.now_us,
                              MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                              i + 1u, &action) == 0);
        REQUIRE(&context,
                action.transit_reservation_abandoned == (i == 0u));
        REQUIRE(&context,
                complete_action(&context, &fixture,
                                MESH_RUNTIME_RADIO_DS_TWR,
                                CLICK_DURATION_US, false) == 0);
    }
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "command-order-bounds";
    REQUIRE(&context,
            fixture.trace_count ==
                COMMAND_BURST_COUNT + COMMAND_BURST_CLICK_COUNT);
    for (size_t i = 0u; i < COMMAND_BURST_COUNT; i++) {
        REQUIRE(&context,
                fixture.traces[i].action ==
                    MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND &&
                fixture.traces[i].token == command_tokens[i]);
    }
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

static int test_queue_pressure_preserves_clicks(void)
{
    const size_t click_count = MESH_RUNTIME_WORK_CAPACITY - 1u;
    struct test_context context = {
        .scenario = "queue-pressure",
        .phase = "setup",
        .seed = SEED_QUEUE_PRESSURE,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    char phase[48];

    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);

    context.phase = "fill-runtime-queue";
    REQUIRE(&context, reserve_transit(&fixture, 500u, 0u) == MESH_RUNTIME_OK);
    for (size_t i = 0u; i < click_count; i++) {
        REQUIRE(&context,
                mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                    i + 1u, 0u) == MESH_RUNTIME_OK);
    }
    REQUIRE(&context,
            runtime_work_count(runtime) == MESH_RUNTIME_WORK_CAPACITY);
    REQUIRE(&context,
            mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_TRANSIT,
                                501u, 0u) == MESH_RUNTIME_ERR_CAPACITY);
    REQUIRE(&context,
            runtime_work_count(runtime) == MESH_RUNTIME_WORK_CAPACITY);

    for (size_t i = 0u; i < click_count; i++) {
        (void)snprintf(phase, sizeof(phase), "pressure-click-%u",
                       (unsigned int)(i + 1u));
        context.phase = phase;
        REQUIRE(&context,
                expect_action(&context, &fixture, context.now_us,
                              MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                              i + 1u, &action) == 0);
        REQUIRE(&context,
                action.transit_reservation_abandoned == (i == 0u));
        if (i == 0u) {
            REQUIRE(&context,
                    runtime_work_count(runtime) == click_count - 1u);
        }
        REQUIRE(&context,
                complete_action(&context, &fixture,
                                MESH_RUNTIME_RADIO_DS_TWR,
                                CLICK_DURATION_US, false) == 0);
    }
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);
    REQUIRE(&context, fixture.trace_count == click_count);
    for (size_t i = 0u; i < click_count; i++) {
        REQUIRE(&context,
                fixture.traces[i].action ==
                    MESH_RUNTIME_ACTION_START_LOCAL_CLICK &&
                fixture.traces[i].token == i + 1u);
    }

    context.phase = "pressure-recovery-transit";
    REQUIRE(&context,
            reserve_transit(&fixture, 502u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_TRANSIT,
                          502u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                            TRANSIT_DURATION_US, false) == 0);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "queue-pressure-bounds";
    REQUIRE(&context, runtime->transit_abandon_count == 1u);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

static int test_repeated_preemption_reset_watchdog(void)
{
    struct test_context context = {
        .scenario = "repeated-preemption-reset-watchdog",
        .phase = "setup",
        .seed = SEED_REPEATED_PREEMPTION,
        .now_us = 0u,
    };
    struct mesh_runtime_action action;
    struct mesh_runtime *runtime;
    struct mesh_relay *relay;
    uint32_t preemption_count = 0u;
    uint32_t runtime_reset_count = 0u;
    char phase[64];

    REQUIRE(&context, init_fixture(&context, &fixture) == 0);
    runtime = fixture_runtime(&fixture);
    relay = fixture_relay(&fixture);
    REQUIRE(&context,
            mesh_sim_watchdog_arm(&fixture.world, fixture.anchor,
                                  WATCHDOG_TIMEOUT_US,
                                  MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK);

    for (size_t cycle = 0u; cycle < SPAM_PREEMPTION_CYCLES; cycle++) {
        bool command_ready = (cycle % 4u) == 0u;

        (void)snprintf(phase, sizeof(phase), "cycle-%u-queue",
                       (unsigned int)(cycle + 1u));
        context.phase = phase;
        if (!channel9_timing_present(relay, CHILD_ID)) {
            REQUIRE(&context, restore_downstream_timing(&fixture) == PROTO_OK);
        }
        REQUIRE(&context,
                reserve_transit(&fixture, (uint16_t)(1000u + cycle),
                                context.now_us) == MESH_RUNTIME_OK);
        REQUIRE(&context,
                mesh_runtime_submit(runtime, MESH_RUNTIME_WORK_LOCAL_CLICK,
                                    cycle + 1u, context.now_us) ==
                MESH_RUNTIME_OK);
        if (command_ready) {
            REQUIRE(&context,
                    mesh_runtime_submit(runtime,
                                        MESH_RUNTIME_WORK_GATEWAY_COMMAND,
                                        5000u + cycle, context.now_us) ==
                    MESH_RUNTIME_OK);

            (void)snprintf(phase, sizeof(phase), "cycle-%u-command",
                           (unsigned int)(cycle + 1u));
            context.phase = phase;
            REQUIRE(&context,
                    expect_action(&context, &fixture, context.now_us,
                                  MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND,
                                  5000u + cycle, &action) == 0);
            REQUIRE(&context,
                    complete_action(&context, &fixture,
                                    MESH_RUNTIME_RADIO_GATEWAY_COMMAND,
                                    COMMAND_DURATION_US, true) == 0);
        }

        (void)snprintf(phase, sizeof(phase), "cycle-%u-click",
                       (unsigned int)(cycle + 1u));
        context.phase = phase;
        REQUIRE(&context,
                expect_action(&context, &fixture, context.now_us,
                              MESH_RUNTIME_ACTION_START_LOCAL_CLICK,
                              cycle + 1u, &action) == 0);
        REQUIRE(&context, action.transit_reservation_abandoned);
        preemption_count++;
        REQUIRE(&context,
                complete_action(&context, &fixture,
                                MESH_RUNTIME_RADIO_DS_TWR,
                                CLICK_DURATION_US, true) == 0);
        REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);
        REQUIRE(&context, runtime->radio_owner == MESH_RUNTIME_RADIO_NONE);

        if (((cycle + 1u) % RUNTIME_RESET_INTERVAL) == 0u) {
            (void)snprintf(phase, sizeof(phase), "cycle-%u-link-runtime-reset",
                           (unsigned int)(cycle + 1u));
            context.phase = phase;
            REQUIRE(&context, restore_downstream_timing(&fixture) == PROTO_OK);
            mesh_relay_clear_channel9_timing(relay, CHILD_ID);
            REQUIRE(&context, !channel9_timing_present(relay, CHILD_ID));
            REQUIRE(&context, restore_downstream_timing(&fixture) == PROTO_OK);
            reset_runtime_state(&fixture);
            runtime = fixture_runtime(&fixture);
            runtime_reset_count++;
            REQUIRE(&context,
                    runtime->radio_owner == MESH_RUNTIME_RADIO_NONE &&
                    runtime->radio_busy_until_us == 0u &&
                    !runtime->transit_reserved &&
                    runtime_work_count(runtime) == 0u);
            REQUIRE(&context, channel9_timing_present(relay, GATEWAY_ID));
            REQUIRE(&context, channel9_timing_present(relay, CHILD_ID));
        }
    }

    context.phase = "spam-transit-recovery";
    REQUIRE(&context,
            reserve_transit(&fixture, 2000u, context.now_us) == MESH_RUNTIME_OK);
    REQUIRE(&context,
            expect_action(&context, &fixture, context.now_us,
                          MESH_RUNTIME_ACTION_RUN_TRANSIT,
                          2000u, &action) == 0);
    REQUIRE(&context,
            complete_action(&context, &fixture, MESH_RUNTIME_RADIO_TRANSIT,
                            TRANSIT_DURATION_US, true) == 0);
    REQUIRE(&context, expect_none(&context, &fixture, context.now_us) == 0);

    context.phase = "watchdog-safe-margin";
    REQUIRE(&context,
            mesh_sim_run_until(&fixture.world,
                               fixture.world.now_us +
                                   WATCHDOG_TIMEOUT_US / 2u) == MESH_SIM_OK);
    context.now_us = fixture.world.now_us;
    REQUIRE(&context,
            mesh_sim_watchdog_feed(&fixture.world, fixture.anchor) == MESH_SIM_OK);

    context.phase = "repeated-cycle-bounds";
    REQUIRE(&context, preemption_count == SPAM_PREEMPTION_CYCLES);
    REQUIRE(&context,
            runtime_reset_count ==
                SPAM_PREEMPTION_CYCLES / RUNTIME_RESET_INTERVAL);
    REQUIRE(&context, runtime->radio_owner == MESH_RUNTIME_RADIO_NONE);
    REQUIRE(&context, mesh_runtime_radio_safe(runtime, context.now_us));
    REQUIRE(&context, fixture.world.roles[fixture.anchor].watchdog.armed);
    REQUIRE(&context, !fixture.world.roles[fixture.anchor].watchdog.expired);
    REQUIRE(&context,
            fixture.world.roles[fixture.anchor].watchdog.expirations == 0u &&
            fixture.world.roles[fixture.anchor].watchdog.resets == 0u);
    REQUIRE(&context,
            mesh_sim_count_transitions(&fixture.world,
                                       MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                       ANCHOR_ID) == 0u);
    REQUIRE(&context,
            mesh_sim_count_transitions(&fixture.world,
                                       MESH_SIM_TRANSITION_WATCHDOG_RESET,
                                       ANCHOR_ID) == 0u);
    REQUIRE(&context, require_bounded_events(&context, &fixture) == 0);
    return 0;
}

int main(void)
{
    if (test_contract_order_at_safe_boundary() != 0) {
        return 1;
    }
    if (test_command_deferral_variants() != 0) {
        return 1;
    }
    if (test_click_load("single-click-load", SEED_CLICK_ONE, 1u) != 0) {
        return 1;
    }
    if (test_click_load("ten-click-load", SEED_CLICK_TEN, 10u) != 0) {
        return 1;
    }
    if (test_multiple_gateway_commands() != 0) {
        return 1;
    }
    if (test_queue_pressure_preserves_clicks() != 0) {
        return 1;
    }
    if (test_repeated_preemption_reset_watchdog() != 0) {
        return 1;
    }

    printf("mesh runtime priority scenarios passed scenarios=8 "
           "spam_cycles=%u stall_limit_us=%llu\n",
           SPAM_PREEMPTION_CYCLES,
           (unsigned long long)STALL_LIMIT_US);
    return 0;
}
