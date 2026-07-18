#include "app_node_comm_gateway_route_refresh.h"

#include <zephyr/kernel.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum {
    GATEWAY_ID = 0x6601u,
};

struct refresh_fixture {
    struct app_node_comm_gateway_route_refresh_config config;
    struct k_work_delayable *work;
    struct app_node_comm_route_refresh_event events[32];
    uint32_t message_ages[64];
    uint32_t now_ms;
    uint32_t scheduled_delay_ms;
    uint32_t schedule_calls;
    uint32_t wake_calls;
    uint32_t send_calls;
    uint32_t note_sent_calls;
    uint32_t stop_calls;
    uint32_t restart_calls;
    uint8_t event_count;
    uint8_t quiet_failures_remaining;
    uint8_t flood_retry_count;
    int schedule_result;
    int send_result;
    bool allowed;
    bool policy_running;
    bool pause_after_first_send;
    bool run_schedule_synchronously;
    bool response_active;
    uint8_t synchronous_schedule_calls;
};

static pthread_mutex_t interleave_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleave_cond = PTHREAD_COND_INITIALIZER;
static bool block_first_send;
static bool first_send_entered;
static bool release_first_send;

static void wait_for_interleave_flag(bool *flag)
{
    struct timespec deadline;
    int ret;

    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += 2;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    while (!*flag) {
        ret = pthread_cond_timedwait(&interleave_cond, &interleave_lock,
                                     &deadline);
        assert(ret == 0);
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static uint32_t fixture_now(void *ctx)
{
    return ((struct refresh_fixture *)ctx)->now_ms;
}

static uint32_t fixture_random(void *ctx)
{
    (void)ctx;
    return 0u;
}

static bool fixture_allowed(void *ctx)
{
    return ((struct refresh_fixture *)ctx)->allowed;
}

static bool fixture_policy_running(void *ctx)
{
    return ((struct refresh_fixture *)ctx)->policy_running;
}

static bool fixture_response_active(uint32_t now_ms, void *ctx)
{
    (void)now_ms;
    return ((struct refresh_fixture *)ctx)->response_active;
}

static void fixture_sleep(uint32_t due_ms, void *ctx)
{
    ((struct refresh_fixture *)ctx)->now_ms = due_ms;
}

static bool fixture_defer(void *ctx)
{
    (void)ctx;
    return false;
}

static bool fixture_quiet(uint32_t sniff_ms, void *ctx)
{
    struct refresh_fixture *fixture = ctx;

    assert(sniff_ms == 20u);
    if (fixture->quiet_failures_remaining > 0u) {
        fixture->quiet_failures_remaining--;
        return false;
    }
    return true;
}

static int fixture_send(const struct mesh_outbound *out, void *ctx)
{
    struct refresh_fixture *fixture = ctx;

    assert(out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(fixture->send_calls <
           sizeof(fixture->message_ages) / sizeof(fixture->message_ages[0]));
    fixture->message_ages[fixture->send_calls] = out->packet.message_age_ms;
    fixture->send_calls++;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_first_send && fixture->send_calls == 1u) {
        first_send_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_first_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    if (fixture->pause_after_first_send && fixture->send_calls == 1u) {
        app_node_comm_gateway_route_refresh_pause(fixture->now_ms);
    }
    return fixture->send_result;
}

static int fixture_build(void *ctx,
                         uint32_t sequence,
                         uint32_t now_ms,
                         struct mesh_gateway_route_adv_snapshot *snapshot,
                         struct mesh_outbound *out)
{
    struct refresh_fixture *fixture = ctx;

    assert(snapshot != NULL);
    if (!snapshot->valid) {
        snapshot->gateway_route_seq = sequence;
        snapshot->queued_at_ms = now_ms;
        snapshot->gateway_epoch = 1u;
        snapshot->packet_seq = (uint16_t)sequence;
        snapshot->gateway_capacity_state = RELAY_CAP_GREEN;
        snapshot->capacity_validity_interval_ms =
            RELAY_CAPACITY_HINT_VALIDITY_MS;
        snapshot->valid = true;
    }
    memset(out, 0, sizeof(*out));
    out->packet.msg_type = MSG_GATEWAY_ROUTE_ADV;
    out->packet.src_id = GATEWAY_ID;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = snapshot->gateway_route_seq;
    out->packet.seq = snapshot->packet_seq;
    out->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    out->packet.payload_len = MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN;
    out->payload_len = MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN;
    memset(out->payload, 0, out->payload_len);
    out->payload[out->payload_len - PROTO_TLV_U32_ENCODED_LEN] =
        TLV_FLOOD_PACKET_AGE_MS;
    out->payload[out->payload_len - PROTO_TLV_U32_ENCODED_LEN + 1u] =
        sizeof(uint32_t);
    out->next_hop_id = MESH_BROADCAST_ID;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->queued_at_ms = snapshot->queued_at_ms;
    out->earliest_tx_ms = snapshot->queued_at_ms;
    out->flood_retry_count = fixture->flood_retry_count;
    return 0;
}

static int fixture_wake(void *ctx, const char *reason)
{
    struct refresh_fixture *fixture = ctx;

    assert(reason != NULL);
    fixture->wake_calls++;
    return 0;
}

static void fixture_note_sent(const struct mesh_outbound *out,
                              uint32_t now_ms,
                              void *ctx)
{
    struct refresh_fixture *fixture = ctx;

    assert(out->packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(now_ms == fixture->now_ms);
    fixture->note_sent_calls++;
}

static void fixture_stop(void *ctx)
{
    ((struct refresh_fixture *)ctx)->stop_calls++;
}

static void fixture_restart(void *ctx)
{
    ((struct refresh_fixture *)ctx)->restart_calls++;
}

static int fixture_schedule(void *ctx,
                            struct k_work_delayable *work,
                            uint32_t delay_ms)
{
    struct refresh_fixture *fixture = ctx;
    bool run_synchronously = fixture->run_schedule_synchronously;

    fixture->work = work;
    fixture->scheduled_delay_ms = delay_ms;
    fixture->schedule_calls++;
    if (fixture->schedule_result == 0 && run_synchronously) {
        fixture->run_schedule_synchronously = false;
        fixture->synchronous_schedule_calls++;
        fixture->now_ms += fixture->scheduled_delay_ms;
        fixture->scheduled_delay_ms = 0u;
        assert(work->work.handler != NULL);
        work->work.handler(&work->work);
    }
    return fixture->schedule_result;
}

static void fixture_observe(
    void *ctx, const struct app_node_comm_route_refresh_event *event)
{
    struct refresh_fixture *fixture = ctx;

    assert(fixture->event_count <
           sizeof(fixture->events) / sizeof(fixture->events[0]));
    fixture->events[fixture->event_count++] = *event;
}

static void fixture_init(struct refresh_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_first_send = false;
    first_send_entered = false;
    release_first_send = false;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    fixture->config = (struct app_node_comm_gateway_route_refresh_config) {
        .gateway_role = true,
        .wake_train_ms = 0u,
        .allowed = fixture_allowed,
        .policy_running = fixture_policy_running,
        .response_priority_active = fixture_response_active,
        .now_ms = fixture_now,
        .random_u32 = fixture_random,
        .sleep_until_ms = fixture_sleep,
        .defer_active = fixture_defer,
        .c5_quiet = fixture_quiet,
        .send = fixture_send,
        .build = fixture_build,
        .send_wake = fixture_wake,
        .note_sent = fixture_note_sent,
        .stop_role_scan = fixture_stop,
        .restart_role_scan = fixture_restart,
        .schedule = fixture_schedule,
        .observe = fixture_observe,
        .ctx = fixture,
    };

    fixture->policy_running = true;
    app_node_comm_gateway_route_refresh_init(&fixture->config, GATEWAY_ID);
}

static void fixture_run(struct refresh_fixture *fixture)
{
    assert(fixture->work != NULL);
    assert(fixture->work->work.handler != NULL);
    fixture->now_ms += fixture->scheduled_delay_ms;
    fixture->scheduled_delay_ms = 0u;
    fixture->work->work.handler(&fixture->work->work);
}

static struct proto_packet correlated_command(void)
{
    return (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .src_id = 0x11u,
        .dst_id = GATEWAY_ID,
        .session_id = 0x12345678u,
        .seq = 77u,
    };
}

static void test_pause_between_opportunities_preserves_four_real_sends(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();

    fixture_init(&fixture);
    fixture.pause_after_first_send = true;
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "stack-local-reason", true, &command) == 0);
    fixture_run(&fixture);
    assert(fixture.send_calls == 1u);
    assert(fixture.event_count == 0u);
    assert(fixture.stop_calls == 1u && fixture.restart_calls == 1u);

    fixture.now_ms += 1000u;
    app_node_comm_gateway_route_refresh_resume(fixture.now_ms);
    assert(fixture.scheduled_delay_ms == 40u);
    fixture.pause_after_first_send = false;
    fixture_run(&fixture);
    assert(fixture.send_calls == app_mesh_flood_repeat_limit());
    assert(fixture.event_count == 2u);
    assert(fixture.events[0].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT);
    assert(fixture.events[0].attempt == 1u);
    assert(fixture.events[1].kind == APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
    assert(fixture.events[1].result == 0);
    assert(fixture.events[1].correlation.session_id == command.session_id);
    assert(fixture.note_sent_calls == 1u);
    assert(fixture.message_ages[1] >= 1040u);
}

static void test_pause_rebases_outer_backoff_but_deadline_stays_absolute(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();

    fixture_init(&fixture);
    fixture.quiet_failures_remaining = UINT8_MAX;
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "outer-backoff", true, &command) == 0);
    fixture_run(&fixture);
    assert(fixture.event_count == 0u);
    assert(fixture.send_calls == 0u);
    assert(fixture.scheduled_delay_ms == 10u);

    fixture.now_ms += 50u;
    app_node_comm_gateway_route_refresh_pause(fixture.now_ms);
    fixture.now_ms += 200000u;
    app_node_comm_gateway_route_refresh_resume(fixture.now_ms);
    assert(fixture.scheduled_delay_ms == 0u);
    fixture_run(&fixture);
    assert(fixture.events[fixture.event_count - 1u].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
    assert(fixture.events[fixture.event_count - 1u].result == -ETIMEDOUT);
}

static void test_schedule_failure_is_terminal(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();

    fixture_init(&fixture);
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "schedule-failure", true, &command) == 0);
    fixture.quiet_failures_remaining = app_mesh_flood_repeat_limit();
    fixture.schedule_result = -EIO;
    fixture_run(&fixture);
    assert(fixture.events[fixture.event_count - 1u].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
    assert(fixture.events[fixture.event_count - 1u].result == -EIO);
}

static void test_manual_refresh_completion_does_not_schedule_maintenance(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();
    uint32_t wait_ms = 0u;

    fixture_init(&fixture);
    fixture.allowed = true;
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "manual-only", true, &command) == 0);
    fixture_run(&fixture);
    assert(fixture.send_calls == 4u);
    assert(fixture.events[fixture.event_count - 1u].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
    assert(fixture.events[fixture.event_count - 1u].result == 0);
    assert(!app_node_comm_gateway_route_refresh_pending_wait_ms(
        fixture.now_ms, &wait_ms));
    assert(fixture.scheduled_delay_ms == 0u);
}

static void test_packet_retry_bursts_each_keep_four_opportunities(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();

    fixture_init(&fixture);
    fixture.flood_retry_count = 2u;
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "three-bursts", true, &command) == 0);
    fixture_run(&fixture);
    assert(fixture.wake_calls == 3u);
    assert(fixture.send_calls == 3u * app_mesh_flood_repeat_limit());
    assert(fixture.events[0].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT);
    assert(fixture.events[0].sent_count == fixture.send_calls);
    assert(fixture.events[1].kind == APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
}

struct refresh_thread_result {
    struct refresh_fixture *fixture;
    uint32_t pause_now_ms;
    bool done;
};

static void refresh_thread_complete(struct refresh_thread_result *result)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    result->done = true;
    assert(pthread_cond_broadcast(&interleave_cond) == 0);
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void *refresh_work_thread(void *arg)
{
    struct refresh_thread_result *result = arg;

    fixture_run(result->fixture);
    refresh_thread_complete(result);
    return NULL;
}

static void *refresh_pause_thread(void *arg)
{
    struct refresh_thread_result *result = arg;

    app_node_comm_gateway_route_refresh_pause(result->pause_now_ms);
    refresh_thread_complete(result);
    return NULL;
}

static void release_blocked_refresh_send(void)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    release_first_send = true;
    assert(pthread_cond_broadcast(&interleave_cond) == 0);
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void test_concurrent_pause_stops_callbacks_and_preserves_correlation(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();
    struct proto_packet competing_command = command;
    struct refresh_thread_result work_result = {0};
    struct refresh_thread_result pause_result = {0};
    pthread_t work_thread;
    pthread_t pause_thread;
    uint8_t terminal_count = 0u;

    fixture_init(&fixture);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_first_send = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "interleaved-original", true, &command) == 0);

    work_result.fixture = &fixture;
    assert(pthread_create(&work_thread, NULL, refresh_work_thread,
                          &work_result) == 0);
    wait_for_interleave_flag(&first_send_entered);

    pause_result.fixture = &fixture;
    pause_result.pause_now_ms = fixture.now_ms;
    assert(pthread_create(&pause_thread, NULL, refresh_pause_thread,
                          &pause_result) == 0);
    wait_for_interleave_flag(&pause_result.done);
    assert(fixture.wake_calls == 1u);
    assert(fixture.send_calls == 1u);
    assert(fixture.event_count == 0u);

    competing_command.session_id++;
    competing_command.seq++;
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "interleaved-competing", true,
               &competing_command) == -EBUSY);

    release_blocked_refresh_send();
    wait_for_interleave_flag(&work_result.done);
    assert(pthread_join(work_thread, NULL) == 0);
    assert(pthread_join(pause_thread, NULL) == 0);
    assert(fixture.wake_calls == 1u);
    assert(fixture.send_calls == 1u);
    assert(fixture.event_count == 0u);

    fixture.now_ms += 100u;
    app_node_comm_gateway_route_refresh_resume(fixture.now_ms);
    fixture_run(&fixture);
    assert(fixture.send_calls == app_mesh_flood_repeat_limit());
    for (uint8_t i = 0u; i < fixture.event_count; i++) {
        if (fixture.events[i].kind ==
            APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) {
            terminal_count++;
            assert(fixture.events[i].correlation.session_id ==
                   command.session_id);
            assert(fixture.events[i].correlation.seq == command.seq);
        }
    }
    assert(terminal_count == 1u);
}

static void test_synchronous_resume_schedule_cannot_strand_refresh(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();
    uint32_t pending_wait_ms = 0u;
    uint8_t terminal_count = 0u;

    fixture_init(&fixture);
    assert(app_node_comm_gateway_route_refresh_request(
               0u, "synchronous-resume", true, &command) == 0);
    app_node_comm_gateway_route_refresh_pause(fixture.now_ms);
    fixture.now_ms += 100u;
    fixture.run_schedule_synchronously = true;
    app_node_comm_gateway_route_refresh_resume(fixture.now_ms);
    assert(fixture.synchronous_schedule_calls == 1u);

    for (uint8_t i = 0u; i < fixture.event_count; i++) {
        if (fixture.events[i].kind ==
            APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) {
            terminal_count++;
            assert(fixture.events[i].correlation.session_id ==
                   command.session_id);
            assert(fixture.events[i].correlation.seq == command.seq);
        }
    }
    if (terminal_count == 0u) {
        assert(app_node_comm_gateway_route_refresh_pending_wait_ms(
            fixture.now_ms, &pending_wait_ms));
        fixture_run(&fixture);
        for (uint8_t i = 0u; i < fixture.event_count; i++) {
            if (fixture.events[i].kind ==
                APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) {
                terminal_count++;
                assert(fixture.events[i].correlation.session_id ==
                       command.session_id);
                assert(fixture.events[i].correlation.seq == command.seq);
            }
        }
    }
    assert(terminal_count == 1u);
}

static void test_explicit_budget_bounds_forced_refresh(void)
{
    struct refresh_fixture fixture;
    struct proto_packet command = correlated_command();
    unsigned int steps = 0u;

    fixture_init(&fixture);
    fixture.quiet_failures_remaining = UINT8_MAX;
    assert(app_node_comm_gateway_route_refresh_request_bounded(
               0u, "short-budget", true, &command, 75u) == 0);
    while ((fixture.event_count == 0u ||
            fixture.events[fixture.event_count - 1u].kind !=
                APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) &&
           steps++ < 32u) {
        fixture_run(&fixture);
    }
    assert(fixture.event_count > 0u);
    assert(fixture.events[fixture.event_count - 1u].kind ==
           APP_NODE_COMM_ROUTE_REFRESH_COMPLETE);
    assert(fixture.events[fixture.event_count - 1u].result == -ETIMEDOUT);
    assert(fixture.events[fixture.event_count - 1u].correlation.session_id ==
           command.session_id);
    assert(fixture.now_ms >= 75u && fixture.now_ms <= 85u);
}

static void test_response_priority_deadline_zero_at_wrap_remains_armed(void)
{
    struct refresh_fixture fixture;

    fixture_init(&fixture);
    fixture.response_active = true;
    fixture.now_ms = UINT32_MAX - 19u;
    assert(app_node_comm_gateway_route_refresh_request_bounded(
               20u, "response-wrap", true, NULL, 1000u) == 0);
    assert(app_node_comm_gateway_route_refresh_response_priority_wait_ms(
               fixture.now_ms) == 20u);
    assert(!app_node_comm_gateway_route_refresh_response_priority_due(
        fixture.now_ms));

    fixture.now_ms = 0u;
    assert(app_node_comm_gateway_route_refresh_response_priority_wait_ms(
               fixture.now_ms) == 0u);
    assert(app_node_comm_gateway_route_refresh_response_priority_due(
        fixture.now_ms));

    app_node_comm_gateway_route_refresh_response_priority_clear();
    fixture.now_ms = UINT32_MAX - 19u;
    assert(app_node_comm_gateway_route_refresh_response_priority_wait_ms(
               fixture.now_ms) == 0u);
    fixture.response_active = false;
    assert(!app_node_comm_gateway_route_refresh_response_priority_due(
        fixture.now_ms));
}

int main(void)
{
    test_pause_between_opportunities_preserves_four_real_sends();
    test_pause_rebases_outer_backoff_but_deadline_stays_absolute();
    test_schedule_failure_is_terminal();
    test_manual_refresh_completion_does_not_schedule_maintenance();
    test_packet_retry_bursts_each_keep_four_opportunities();
    test_concurrent_pause_stops_callbacks_and_preserves_correlation();
    test_synchronous_resume_schedule_cannot_strand_refresh();
    test_explicit_budget_bounds_forced_refresh();
    test_response_priority_deadline_zero_at_wrap_remains_armed();
    return 0;
}
