#include "mesh_sim.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

struct expected_marker {
    uint64_t time_us;
    uint32_t identity;
};

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            failures++; \
            return; \
        } \
    } while (0)

static void expect_marker(const struct mesh_sim_world *world,
                          uint64_t occurrence,
                          uint64_t time_us,
                          uint32_t detail)
{
    struct mesh_sim_transition transition;

    CHECK(mesh_sim_find_transition_snapshot(
              world,
              MESH_SIM_TRANSITION_SCHEDULER_MARKER,
              0u,
              occurrence,
              &transition) == MESH_SIM_SNAPSHOT_OK,
          "marker %llu was unavailable", (unsigned long long)occurrence);
    CHECK(transition.detail == detail,
          "marker %llu detail=%u expected=%u",
          (unsigned long long)occurrence,
          transition.detail,
          detail);
    CHECK(transition.time_us == time_us,
          "marker %llu time=%llu expected=%llu",
          (unsigned long long)occurrence,
          (unsigned long long)transition.time_us,
          (unsigned long long)time_us);
}

static void test_timestamp_priority_sequence_and_compaction_order(void)
{
    static struct mesh_sim_world world;
    enum {
        EVENTS_PER_BATCH = 8u,
        COMPACTION_CYCLES = 32u,
    };
    static struct expected_marker expected[EVENTS_PER_BATCH * COMPACTION_CYCLES];
    size_t expected_count = 0u;

    mesh_sim_init(&world, UINT32_C(0x1a0b1c0d));
    for (uint16_t cycle = 0u; cycle < COMPACTION_CYCLES; cycle++) {
        const uint16_t identity = (uint16_t)(cycle * EVENTS_PER_BATCH);
        const uint64_t at_us = world.now_us + 10u;

        /* Deliberately insert in non-sort order to exercise every tie-break. */
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us + 2u, 4u,
                                              identity + 7u) == MESH_SIM_OK,
              "cycle %u late marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us, 2u,
                                              identity + 3u) == MESH_SIM_OK,
              "cycle %u priority marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us, 1u,
                                              identity + 1u) == MESH_SIM_OK,
              "cycle %u first sequence marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us, 1u,
                                              identity + 2u) == MESH_SIM_OK,
              "cycle %u second sequence marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us - 1u, 255u,
                                              identity) == MESH_SIM_OK,
              "cycle %u early marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us + 1u, 0u,
                                              identity + 4u) == MESH_SIM_OK,
              "cycle %u middle marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us + 2u, 3u,
                                              identity + 5u) == MESH_SIM_OK,
              "cycle %u first late sequence marker schedule failed", cycle);
        CHECK(mesh_sim_schedule_trace_marker(&world, at_us + 2u, 3u,
                                              identity + 6u) == MESH_SIM_OK,
              "cycle %u second late sequence marker schedule failed", cycle);

        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us - 1u, .identity = identity,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us, .identity = identity + 1u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us, .identity = identity + 2u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us, .identity = identity + 3u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us + 1u, .identity = identity + 4u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us + 2u, .identity = identity + 5u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us + 2u, .identity = identity + 6u,
        };
        expected[expected_count++] = (struct expected_marker) {
            .time_us = at_us + 2u, .identity = identity + 7u,
        };
        CHECK(mesh_sim_run_until(&world, at_us + 2u) == MESH_SIM_OK,
              "cycle %u execution failed", cycle);
        CHECK(world.event_count == 0u,
              "cycle %u did not compact the scheduler queue", cycle);
        for (size_t marker = 0u; marker < expected_count; marker++) {
            expect_marker(&world,
                          marker,
                          expected[marker].time_us,
                          expected[marker].identity);
            if (failures != 0) {
                return;
            }
        }
    }

    for (uint16_t marker = 0u; marker < MESH_SIM_MAX_TRANSITIONS + 8u; marker++) {
        uint64_t at_us = world.now_us + 1u;

        CHECK(mesh_sim_schedule_trace_marker(&world, at_us, 2u, marker) ==
                  MESH_SIM_OK,
              "marker %u schedule failed", marker);
        CHECK(mesh_sim_run_until(&world, at_us) == MESH_SIM_OK,
              "marker %u run failed", marker);
    }
    CHECK(world.event_count == 0u,
          "scheduler queue was not compacted after repeated pops");
    CHECK(mesh_sim_trace_is_truncated(&world),
          "trace tail did not report eviction");
    {
        struct mesh_sim_transition transition;

        CHECK(mesh_sim_find_transition_snapshot(
                  &world,
                  MESH_SIM_TRANSITION_SCHEDULER_MARKER,
                  0u,
                  0u,
                  &transition) == MESH_SIM_SNAPSHOT_TRUNCATED,
              "old marker lookup did not report truncation");
        CHECK(mesh_sim_find_transition_snapshot(
                  &world,
                  MESH_SIM_TRANSITION_SCHEDULER_MARKER,
                  UINT64_C(0xdecafbad),
                  0u,
                  &transition) == MESH_SIM_SNAPSHOT_NOT_FOUND,
              "unknown node id changed detail lookup semantics after eviction");
        CHECK(mesh_sim_count_transitions(&world,
                                         MESH_SIM_TRANSITION_SCHEDULER_MARKER,
                                         UINT64_C(0xdecafbad)) == 0u,
              "unknown node id changed count semantics after eviction");
    }
}

static void test_connection_and_rx_telemetry_tails(void)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params = {
        .event_interval_ms = 30u,
        .event_window_ms = 20u,
        .first_event_time_ms = 10u,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
    uint8_t node_a;
    uint8_t node_b;
    uint16_t connection;

    mesh_sim_init(&world, UINT32_C(0x5eeda11c));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_TRANSMITTER,
                            UINT64_C(0xa101),
                            UINT64_C(0xa102),
                            1u,
                            &node_a) == MESH_SIM_OK,
          "first role setup failed");
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            UINT64_C(0xa102),
                            UINT64_C(0xa102),
                            1u,
                            &node_b) == MESH_SIM_OK,
          "second role setup failed");
    CHECK(mesh_sim_set_link(&world, node_a, node_b, 100u, 0u) == MESH_SIM_OK,
          "link setup failed");
    CHECK(mesh_sim_add_connection(&world,
                                  node_a,
                                  node_b,
                                  &params,
                                  true,
                                  &connection) == MESH_SIM_OK,
          "connection setup failed");
    for (size_t i = 0u; i < MESH_SIM_MAX_CONNECTION_EVENTS + 8u; i++) {
        struct mesh_sim_connection_action action;
        uint8_t frame[] = { 0u };
        uint16_t transmission;
        uint16_t connection_event;

        CHECK(mesh_sim_connection_next_action(&world, connection, &action) ==
                  MESH_SIM_OK &&
                  action.kind != MESH_SIM_CONNECTION_ACTION_NONE,
              "event %zu action lookup failed", i);
        CHECK(mesh_sim_schedule_next_connection_event(&world, connection, false) ==
                  MESH_SIM_OK,
              "event %zu schedule failed", i);
        CHECK(mesh_sim_run_until(&world, action.start_us) == MESH_SIM_OK,
              "event %zu start failed", i);
        connection_event = (uint16_t)(world.connection_event_count - 1u);
        CHECK(mesh_sim_schedule_raw_tx(
                  &world,
                  world.connection_events[connection_event].sender_index,
                  action.start_us + MESH_SIM_SLOT_TX_OFFSET_US,
                  UWB_CHANNEL_MESH_PAYLOAD,
                  MESH_SIM_PHY_CHANNEL9_MESH,
                  frame,
                  sizeof(frame),
                  false,
                  &transmission) == MESH_SIM_OK,
              "event %zu transmission schedule failed", i);
        world.transmissions[transmission].connection_event_index = connection_event;
        CHECK(mesh_sim_run_until(&world, action.end_us - 1u) == MESH_SIM_OK,
              "event %zu radio execution failed", i);
        world.connection_events[connection_event].decoded = true;
        CHECK(mesh_sim_run_until(&world, action.end_us) == MESH_SIM_OK,
              "event %zu execution failed", i);
    }
    CHECK(mesh_sim_telemetry_is_truncated(
              &world, MESH_SIM_TELEMETRY_CONNECTION_EVENT) &&
              mesh_sim_telemetry_is_truncated(
                  &world, MESH_SIM_TELEMETRY_RX_WINDOW),
          "connection/RX telemetry did not retain bounded tails");
    {
        struct mesh_sim_connection_event event_snapshot;
        struct mesh_sim_rx_window window_snapshot;

        CHECK(mesh_sim_connection_event_snapshot(
                  &world, 0u, &event_snapshot) == MESH_SIM_SNAPSHOT_TRUNCATED,
              "old connection event did not report truncation");
        CHECK(mesh_sim_rx_window_snapshot(
                  &world, 0u, &window_snapshot) == MESH_SIM_SNAPSHOT_TRUNCATED,
              "old RX window did not report truncation");
        CHECK(mesh_sim_connection_event_snapshot(
                  &world,
                  world.connection_event_total_count - 1u,
                  &event_snapshot) == MESH_SIM_SNAPSHOT_OK,
              "latest connection event snapshot was unavailable");
    }
}

int main(void)
{
    test_timestamp_priority_sequence_and_compaction_order();
    test_connection_and_rx_telemetry_tails();
    if (failures != 0) {
        fprintf(stderr, "mesh simulator trace/scheduler scenarios: FAIL (%d)\n",
                failures);
        return 1;
    }
    puts("mesh simulator trace/scheduler scenarios: PASS");
    return 0;
}
