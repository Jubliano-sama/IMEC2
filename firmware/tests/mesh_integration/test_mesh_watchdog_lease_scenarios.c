#include "mesh_sim.h"

#include "report.h"

#include <stdint.h>
#include <stdio.h>

#define TEST_ANCHOR_ID UINT64_C(0x54455354414e4331)
#define TEST_GATEWAY_ID UINT64_C(0x5445535447575931)
#define TEST_ROUTE_EPOCH UINT32_C(17)
#define WATCHDOG_TIMEOUT_US UINT64_C(1000000)

struct watchdog_fixture {
    struct mesh_sim_world world;
    uint8_t anchor;
    uint8_t gateway;
    uint16_t connection;
};

static int failures;

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

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 30u,
        .event_window_ms = 20u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static void init_fixture(struct watchdog_fixture *fixture)
{
    const struct mesh_event_params params = connection_params(10u);

    mesh_sim_init(&fixture->world, UINT32_C(0x9c41e28b));
    CHECK(mesh_sim_add_role(&fixture->world,
                            MESH_SIM_ROLE_ANCHOR,
                            TEST_ANCHOR_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &fixture->anchor) == MESH_SIM_OK,
          "anchor setup failed");
    CHECK(mesh_sim_add_role(&fixture->world,
                            MESH_SIM_ROLE_GATEWAY,
                            TEST_GATEWAY_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &fixture->gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_set_link(&fixture->world,
                            fixture->anchor,
                            fixture->gateway,
                            100u,
                            0u) == MESH_SIM_OK,
          "link setup failed");
    CHECK(mesh_sim_install_route(&fixture->world,
                                 fixture->anchor,
                                 fixture->gateway,
                                 1u,
                                 TEST_ROUTE_EPOCH) == PROTO_OK,
          "route setup failed");
    CHECK(mesh_sim_add_connection(&fixture->world,
                                  fixture->anchor,
                                  fixture->gateway,
                                  &params,
                                  true,
                                  &fixture->connection) == MESH_SIM_OK,
          "connection setup failed");
    CHECK(mesh_sim_watchdog_arm(&fixture->world,
                                fixture->gateway,
                                WATCHDOG_TIMEOUT_US,
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK,
          "gateway watchdog setup failed");
}

static void queue_mesh_data(struct watchdog_fixture *fixture, uint16_t seq)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = TEST_ANCHOR_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = UINT32_C(0x10203040),
        .seq = seq,
        .ttl = 1u,
    };

    CHECK(tlv_append_u32(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_TEST_PACKET_ID,
                         seq) == PROTO_OK,
          "payload setup failed");
    packet.payload_len = (uint16_t)payload_len;
    CHECK(mesh_sim_queue_originated(&fixture->world,
                                    fixture->anchor,
                                    &packet,
                                    payload,
                                    payload_len) == MESH_SIM_OK,
          "packet queue failed");
}

static void run_next_connection(struct watchdog_fixture *fixture)
{
    struct mesh_sim_connection_action action;

    CHECK(mesh_sim_connection_next_action(&fixture->world,
                                          fixture->connection,
                                          &action) == MESH_SIM_OK,
          "connection action lookup failed");
    CHECK(!action.already_scheduled,
          "connection action was unexpectedly already scheduled");
    CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT,
          "expected channel-9 event, got action=%u",
          (unsigned int)action.kind);
    CHECK(mesh_sim_schedule_next_connection_event(&fixture->world,
                                                  fixture->connection,
                                                  false) == MESH_SIM_OK,
          "connection schedule failed");
    CHECK(mesh_sim_run_until(&fixture->world, action.end_us) == MESH_SIM_OK,
          "connection execution failed");
}

/*
 * Production progression predicate: the gateway's bounded channel-9 RX worker
 * reaches radio_guard_uwb_stop(), then calls app_watchdog_note_radio_progress().
 * app_mesh_rx_policy_gateway_ch9_rx_error_recoverable() permits an SFD timeout
 * to continue that bounded worker loop. This is not a packet-delivery predicate.
 */
static void test_recoverable_gateway_sfd_timeout_renews_completed_worker_lease(void)
{
    static struct watchdog_fixture fixture;
    const struct mesh_sim_connection_event *event;
    const struct mesh_sim_reception *reception;

    init_fixture(&fixture);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_sim_set_directed_rx_failures(&fixture.world,
                                            fixture.anchor,
                                            fixture.gateway,
                                            1u,
                                            MESH_SIM_RX_SFD_TIMEOUT) == MESH_SIM_OK,
          "SFD-timeout setup failed");
    queue_mesh_data(&fixture, 1u);
    if (failures != 0) {
        return;
    }
    run_next_connection(&fixture);
    if (failures != 0) {
        return;
    }

    CHECK(fixture.world.connection_event_count == 1u,
          "expected one completed connection event");
    CHECK(fixture.world.reception_count == 1u,
          "expected one modeled gateway reception");
    event = &fixture.world.connection_events[0];
    reception = &fixture.world.receptions[0];
    CHECK(event->had_packet && !event->decoded,
          "expected a completed, undecoded SFD-timeout event");
    CHECK(reception->outcome == MESH_SIM_RX_SFD_TIMEOUT,
          "expected modeled SFD timeout, got outcome=%u",
          (unsigned int)reception->outcome);
    CHECK(fixture.world.connections[fixture.connection].completed_events == 1u,
          "bounded worker completion was not recorded");
    CHECK(fixture.world.connections[fixture.connection].timing_b.missed_event_count == 1u,
          "gateway miss accounting did not record the recoverable worker outcome");
    CHECK(fixture.world.roles[fixture.gateway].watchdog.feeds == 1u,
          "app_watchdog_note_radio_progress predicate was not represented by "
          "the completed recoverable gateway RX worker");
    CHECK(!fixture.world.roles[fixture.gateway].watchdog.expired,
          "recoverable completed worker must keep the gateway lease fresh");
    CHECK(fixture.world.roles[fixture.gateway].watchdog.workers_started == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_completed == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.recoverable_completions == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.radio_lease_state ==
                  MESH_SIM_RADIO_LEASE_COMPLETED_RECOVERABLE,
          "recoverable worker lifecycle was not recorded");
}

/*
 * The simulator exposes no event state between scheduled and completed. This
 * is the faithful counterpart of a stalled worker: no completion means no
 * app_watchdog_note_radio_progress-equivalent feed and the watchdog resets.
 */
static void test_no_completed_radio_worker_stops_feeding_and_resets(void)
{
    static struct mesh_sim_world world;
    uint8_t gateway;

    mesh_sim_init(&world, UINT32_C(0x1b998bce));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            TEST_GATEWAY_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_watchdog_arm(&world,
                                gateway,
                                UINT64_C(1000),
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK,
          "watchdog setup failed");
    CHECK(mesh_sim_run_until(&world, UINT64_C(1000)) == MESH_SIM_OK,
          "watchdog execution failed");
    CHECK(world.roles[gateway].watchdog.feeds == 0u,
          "no completed radio worker must not renew the radio-progress lease");
    CHECK(world.roles[gateway].watchdog.expired &&
              world.roles[gateway].watchdog.expirations == 1u &&
              world.roles[gateway].watchdog.resets == 1u,
          "stalled radio progress must stop feeds and reset the role");
    CHECK(world.roles[gateway].radio_state == MESH_SIM_RADIO_SLEEP,
          "watchdog reset did not restore the simulated role to sleep");
    CHECK(world.roles[gateway].watchdog.feeds_stopped &&
              world.roles[gateway].watchdog.radio_lease_state ==
                  MESH_SIM_RADIO_LEASE_RESET,
          "lease expiry must stop production feeds before reset");
}

static void test_watchdog_feed_replaces_one_pending_expiry(void)
{
    static struct mesh_sim_world world;
    uint8_t gateway;

    mesh_sim_init(&world, UINT32_C(0x510e0a11));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            TEST_GATEWAY_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_watchdog_arm(&world,
                                gateway,
                                UINT64_C(45000),
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK,
          "watchdog setup failed");
    for (unsigned int feed = 0u; feed < 4097u; feed++) {
        CHECK(mesh_sim_run_until(&world, world.now_us + UINT64_C(30000)) ==
                  MESH_SIM_OK,
              "feed %u advanced into a stale watchdog expiry", feed);
        CHECK(mesh_sim_watchdog_feed(&world, gateway) == MESH_SIM_OK,
              "feed %u failed", feed);
        CHECK(world.event_count == 1u &&
                  world.roles[gateway].watchdog.expiry_event_pending,
              "feed %u did not retain exactly one pending expiry", feed);
    }
    CHECK(world.roles[gateway].watchdog.feeds == 4097u,
          "feed count was not exact");
    CHECK(mesh_sim_count_transitions(&world,
                                     MESH_SIM_TRANSITION_WATCHDOG_FED,
                                     TEST_GATEWAY_ID) == 4097u,
          "trace count did not retain cumulative feeds");
    CHECK(mesh_sim_run_until(&world,
                             world.roles[gateway].watchdog.deadline_us) ==
              MESH_SIM_OK,
          "actual watchdog deadline did not execute");
    CHECK(world.event_count == 0u && world.roles[gateway].watchdog.expired &&
              world.roles[gateway].watchdog.expirations == 1u &&
              world.roles[gateway].watchdog.resets == 1u,
          "actual deadline did not expire exactly once");
}

static void test_started_preempted_worker_stops_feeding(void)
{
    static struct watchdog_fixture fixture;

    init_fixture(&fixture);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_sim_watchdog_arm(&fixture.world,
                                fixture.gateway,
                                UINT64_C(35000),
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK,
          "watchdog setup failed");
    CHECK(mesh_sim_schedule_next_connection_event(&fixture.world,
                                                  fixture.connection,
                                                  true) == MESH_SIM_OK,
          "preempted event schedule failed");
    CHECK(mesh_sim_run_until(&fixture.world, UINT64_C(35000)) == MESH_SIM_OK,
          "watchdog execution failed");
    CHECK(fixture.world.roles[fixture.gateway].watchdog.workers_started == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_completed == 0u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_aborted == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_active == 0u &&
              fixture.world.roles[fixture.gateway].watchdog.feeds == 0u &&
              fixture.world.roles[fixture.gateway].watchdog.expired &&
              fixture.world.roles[fixture.gateway].watchdog.radio_lease_state ==
                  MESH_SIM_RADIO_LEASE_RESET,
          "started-but-never-completed worker incorrectly renewed the lease");
}

static void test_reset_cancels_started_channel9_work_and_fresh_sfd_workers_recover(void)
{
    static struct watchdog_fixture fixture;
    struct mesh_sim_connection_action action;
    struct mesh_sim_transmission *tx;
    struct mesh_sim_rx_window *window;
    uint64_t started_at_us;
    uint64_t completed_at_us;
    uint64_t reset_epoch;
    uint32_t feeds_before_expiry;
    size_t fresh_gateway_completions = 0u;

    init_fixture(&fixture);
    if (failures != 0) {
        return;
    }
    queue_mesh_data(&fixture, 1u);
    if (failures != 0) {
        return;
    }
    CHECK(mesh_sim_connection_next_action(&fixture.world,
                                          fixture.connection,
                                          &action) == MESH_SIM_OK,
          "connection action lookup failed");
    CHECK(mesh_sim_schedule_next_connection_event(&fixture.world,
                                                  fixture.connection,
                                                  false) == MESH_SIM_OK,
          "connection schedule failed");
    CHECK(mesh_sim_run_until(&fixture.world, action.start_us) == MESH_SIM_OK,
          "connection start failed");
    CHECK(fixture.world.transmission_count == 1u &&
              fixture.world.rx_window_count == 1u,
          "started channel-9 event did not create both TX and RX work");
    tx = &fixture.world.transmissions[0];
    window = &fixture.world.rx_windows[0];
    started_at_us = tx->start_us > window->start_us ? tx->start_us :
                                                     window->start_us;
    completed_at_us = tx->end_us < window->end_us ? tx->end_us :
                                                   window->end_us;
    CHECK(started_at_us < completed_at_us,
          "TX/RX work did not overlap for reset cancellation");
    CHECK(mesh_sim_run_until(&fixture.world, started_at_us) == MESH_SIM_OK,
          "channel-9 work did not reach both start boundaries");
    CHECK(fixture.world.roles[fixture.anchor].radio_state == MESH_SIM_RADIO_TX &&
              fixture.world.roles[fixture.gateway].radio_state == MESH_SIM_RADIO_RX,
          "watchdog did not expire while both channel-9 workers were active");

    CHECK(completed_at_us - started_at_us > 1u,
          "channel-9 work left no time for a mid-airtime expiry");
    fixture.world.roles[fixture.gateway].watchdog.timeout_us =
        completed_at_us - started_at_us - 1u;
    CHECK(mesh_sim_watchdog_feed(&fixture.world, fixture.gateway) == MESH_SIM_OK,
          "watchdog deadline update failed");
    feeds_before_expiry = fixture.world.roles[fixture.gateway].watchdog.feeds;
    reset_epoch = fixture.world.roles[fixture.gateway].work_epoch;
    CHECK(mesh_sim_run_until(&fixture.world, completed_at_us) == MESH_SIM_OK,
          "reset during channel-9 airtime produced an event-order failure");
    CHECK(fixture.world.roles[fixture.gateway].work_epoch == reset_epoch + 1u,
          "watchdog reset did not advance the role work epoch");
    CHECK(fixture.world.roles[fixture.gateway].watchdog.workers_aborted == 1u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_active == 0u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_completed == 0u,
          "started gateway worker was not recorded as aborted");
    CHECK(mesh_sim_count_transitions(
              &fixture.world,
              MESH_SIM_TRANSITION_WATCHDOG_WORKER_ABORTED,
              TEST_GATEWAY_ID) == 1u,
          "aborted worker was not recorded in the trace");
    CHECK(fixture.world.roles[fixture.gateway].watchdog.feeds == feeds_before_expiry &&
              fixture.world.roles[fixture.gateway].delivery_count == 0u &&
              !fixture.world.rx_windows[0].valid &&
              !fixture.world.connection_events[0].valid &&
              fixture.world.event_count == 0u &&
              fixture.world.last_error == MESH_SIM_OK,
          "cancelled stale work produced a feed, delivery, or simulator error");

    CHECK(mesh_sim_watchdog_arm(&fixture.world,
                                fixture.gateway,
                                WATCHDOG_TIMEOUT_US,
                                MESH_SIM_WATCHDOG_RESET_ROLE) == MESH_SIM_OK,
          "fresh gateway watchdog setup failed");
    CHECK(mesh_sim_set_directed_rx_failures(&fixture.world,
                                            fixture.anchor,
                                            fixture.gateway,
                                            2u,
                                            MESH_SIM_RX_SFD_TIMEOUT) == MESH_SIM_OK,
          "fresh SFD-timeout setup failed");
    for (uint16_t seq = 2u; seq <= 5u; seq++) {
        queue_mesh_data(&fixture, seq);
        if (failures != 0) {
            return;
        }
        run_next_connection(&fixture);
        if (failures != 0) {
            return;
        }
    }
    for (size_t event_index = 1u;
         event_index < fixture.world.connection_event_count;
         event_index++) {
        const struct mesh_sim_connection_event *event =
            &fixture.world.connection_events[event_index];

        if (event->valid && event->receiver_index == fixture.gateway &&
            event->receiver_worker_completed &&
            event->node_a_work_epoch ==
                fixture.world.roles[fixture.anchor].work_epoch &&
            event->node_b_work_epoch ==
                fixture.world.roles[fixture.gateway].work_epoch) {
            fresh_gateway_completions++;
        }
    }
    CHECK(fixture.world.roles[fixture.gateway].watchdog.workers_started == 2u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_completed == 2u &&
              fixture.world.roles[fixture.gateway].watchdog.workers_active == 0u &&
              fixture.world.roles[fixture.gateway].watchdog.recoverable_completions == 2u &&
              fixture.world.roles[fixture.gateway].watchdog.feeds == 2u &&
              !fixture.world.roles[fixture.gateway].watchdog.expired &&
              fixture.world.roles[fixture.gateway].delivery_count == 0u &&
              fresh_gateway_completions == 2u &&
              fixture.world.reception_count == 2u &&
              fixture.world.receptions[0].outcome == MESH_SIM_RX_SFD_TIMEOUT &&
              fixture.world.receptions[1].outcome == MESH_SIM_RX_SFD_TIMEOUT,
          "fresh workers did not recover through repeated SFD-timeout completions "
          "(started=%u completed=%u active=%u recoverable=%u feeds=%u expired=%u "
          "deliveries=%zu)",
          fixture.world.roles[fixture.gateway].watchdog.workers_started,
          fixture.world.roles[fixture.gateway].watchdog.workers_completed,
          fixture.world.roles[fixture.gateway].watchdog.workers_active,
          fixture.world.roles[fixture.gateway].watchdog.recoverable_completions,
          fixture.world.roles[fixture.gateway].watchdog.feeds,
          fixture.world.roles[fixture.gateway].watchdog.expired,
          fixture.world.roles[fixture.gateway].delivery_count);
}

int main(void)
{
    test_recoverable_gateway_sfd_timeout_renews_completed_worker_lease();
    test_no_completed_radio_worker_stops_feeding_and_resets();
    test_watchdog_feed_replaces_one_pending_expiry();
    test_started_preempted_worker_stops_feeding();
    test_reset_cancels_started_channel9_work_and_fresh_sfd_workers_recover();
    if (failures != 0) {
        fprintf(stderr, "mesh watchdog lease scenarios: FAIL (%d)\n", failures);
        return 1;
    }
    puts("mesh watchdog lease scenarios: PASS");
    return 0;
}
