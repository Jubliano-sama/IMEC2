#include "app_stack_diag.h"
#include "app_stack_workload_diag.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_RECORDS 4096u
#define CONCURRENCY_STRESS_ROUNDS 1000u

static uint32_t next_run_id;
static uint32_t begin_count;
static uint32_t sample_count;
static uint32_t end_count;
static uint32_t sample_attempt_count;
static uint32_t end_attempt_count;
static uint32_t active_count;
static uint32_t begin_failures_remaining;
static uint32_t sample_failures_remaining;
static uint32_t end_failures_remaining;
static enum app_stack_diag_workload workloads[MAX_RECORDS];
static enum app_stack_diag_owner owners[MAX_RECORDS];
static enum app_stack_diag_terminal_outcome outcomes[MAX_RECORDS];
static struct app_stack_diag_state begin_states[MAX_RECORDS];
static struct app_stack_diag_state sample_states[MAX_RECORDS];
static struct app_stack_diag_state end_states[MAX_RECORDS];
static struct app_stack_diag_state run_identities[MAX_RECORDS + 1u];
static bool run_active[MAX_RECORDS + 1u];
static pthread_mutex_t diag_stub_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t diag_stub_cond = PTHREAD_COND_INITIALIZER;
static bool block_next_begin;
static bool blocked_begin_entered;
static bool release_blocked_begin;
static uint32_t diag_calls_in_flight;
static uint32_t max_diag_calls_in_flight;
static pthread_mutex_t lock_attempt_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lock_attempt_cond = PTHREAD_COND_INITIALIZER;
static uint32_t lock_attempt_count;

void app_stack_workload_diag_test_lock_attempt(void)
{
    assert(pthread_mutex_lock(&lock_attempt_mutex) == 0);
    lock_attempt_count++;
    assert(pthread_cond_broadcast(&lock_attempt_cond) == 0);
    assert(pthread_mutex_unlock(&lock_attempt_mutex) == 0);
}

static void diag_call_enter_locked(void)
{
    diag_calls_in_flight++;
    if (diag_calls_in_flight > max_diag_calls_in_flight) {
        max_diag_calls_in_flight = diag_calls_in_flight;
    }
}

static void diag_call_leave_locked(void)
{
    assert(diag_calls_in_flight > 0u);
    diag_calls_in_flight--;
}

static bool identity_matches(const struct app_stack_diag_state *left,
                             const struct app_stack_diag_state *right)
{
    return left->source_id == right->source_id &&
           left->destination_id == right->destination_id &&
           left->session_id == right->session_id &&
           left->packet_sequence == right->packet_sequence &&
           left->message_type == right->message_type;
}

void app_stack_diag_start(void)
{
}

uint32_t app_stack_diag_run_begin(enum app_stack_diag_workload workload,
                                  enum app_stack_diag_owner owner,
                                  const struct app_stack_diag_state *state)
{
    uint32_t run_id;

    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    diag_call_enter_locked();
    if (block_next_begin) {
        block_next_begin = false;
        blocked_begin_entered = true;
        assert(pthread_cond_broadcast(&diag_stub_cond) == 0);
        while (!release_blocked_begin) {
            assert(pthread_cond_wait(&diag_stub_cond, &diag_stub_mutex) == 0);
        }
    }
    assert(workload <= APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL);
    assert(owner != APP_STACK_DIAG_OWNER_SHARED_MIN);
    assert(begin_count < MAX_RECORDS);
    owners[begin_count] = owner;
    workloads[begin_count] = workload;
    begin_states[begin_count++] = *state;
    if (begin_failures_remaining > 0u) {
        begin_failures_remaining--;
        diag_call_leave_locked();
        assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
        return 0u;
    }
    if (active_count >= APP_STACK_DIAG_COMBINED_PEAK_RUNS) {
        diag_call_leave_locked();
        assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
        return 0u;
    }
    active_count++;
    run_id = ++next_run_id;
    assert(run_id <= MAX_RECORDS);
    run_identities[run_id] = *state;
    run_active[run_id] = true;
    diag_call_leave_locked();
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
    return run_id;
}

int app_stack_diag_sample(uint32_t run_id,
                          const struct app_stack_diag_state *state)
{
    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    diag_call_enter_locked();
    assert(run_id != 0u);
    assert(run_id <= MAX_RECORDS);
    assert(run_active[run_id]);
    assert(identity_matches(&run_identities[run_id], state));
    sample_attempt_count++;
    if (sample_failures_remaining > 0u) {
        sample_failures_remaining--;
        diag_call_leave_locked();
        assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
        return -EIO;
    }
    assert(sample_count < MAX_RECORDS);
    sample_states[sample_count++] = *state;
    diag_call_leave_locked();
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
    return 0;
}

int app_stack_diag_run_end(uint32_t run_id,
                           enum app_stack_diag_terminal_outcome outcome,
                           const struct app_stack_diag_state *state)
{
    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    diag_call_enter_locked();
    assert(run_id != 0u);
    assert(run_id <= MAX_RECORDS);
    assert(run_active[run_id]);
    assert(identity_matches(&run_identities[run_id], state));
    assert(outcome <= APP_STACK_DIAG_TERMINAL_ERROR);
    end_attempt_count++;
    if (end_failures_remaining > 0u) {
        end_failures_remaining--;
        diag_call_leave_locked();
        assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
        return -EIO;
    }
    assert(end_count < MAX_RECORDS);
    outcomes[end_count] = outcome;
    end_states[end_count++] = *state;
    assert(active_count > 0u);
    active_count--;
    run_active[run_id] = false;
    diag_call_leave_locked();
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
    return 0;
}

static struct proto_packet packet(uint64_t source, uint32_t session, uint16_t seq)
{
    return (struct proto_packet){
        .src_id = source,
        .dst_id = 7u,
        .session_id = session,
        .seq = seq,
        .msg_type = MSG_CLICK_REPORT,
    };
}

enum concurrent_workload {
    CONCURRENT_GATEWAY_REPORT = 0,
    CONCURRENT_GATEWAY_CONTROL,
    CONCURRENT_BLE_BACKPRESSURE,
};

struct concurrent_worker {
    enum concurrent_workload workload;
    pthread_barrier_t *barrier;
    uint32_t rounds;
};

static pthread_mutex_t worker_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_ready_cond = PTHREAD_COND_INITIALIZER;
static uint32_t workers_ready;

static void worker_note_ready(void)
{
    assert(pthread_mutex_lock(&worker_ready_mutex) == 0);
    workers_ready++;
    assert(pthread_cond_broadcast(&worker_ready_cond) == 0);
    assert(pthread_mutex_unlock(&worker_ready_mutex) == 0);
}

static void run_concurrent_workload(enum concurrent_workload workload,
                                    uint32_t iteration)
{
    const uint64_t source = 100u + (uint64_t)workload;
    struct proto_packet current = packet(
        source, 1000u + iteration,
        (uint16_t)(1u + iteration + (uint32_t)workload * 1000u));
    const struct app_stack_workload_diag_pressure pressure = {
        .queue_depth = (uint16_t)(1u + workload),
        .custody_depth = (uint16_t)workload,
        .credit_available = 1u,
        .retry_depth = (uint16_t)workload,
        .drain_depth = 2u,
    };

    switch (workload) {
    case CONCURRENT_GATEWAY_REPORT:
        app_stack_workload_diag_gateway_report_cycle(&current, 1u, 0u);
        break;
    case CONCURRENT_GATEWAY_CONTROL:
        app_stack_workload_diag_gateway_control_admit(&current, 2u, 1u);
        app_stack_workload_diag_gateway_control_sample(&current, 2u, 1u);
        app_stack_workload_diag_gateway_control_release(&current, 0, 0u, 0u);
        break;
    case CONCURRENT_BLE_BACKPRESSURE:
        app_stack_workload_diag_ble_admit_with_pressure(
            &current, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &pressure);
        app_stack_workload_diag_ble_sample_with_pressure(&current, &pressure);
        app_stack_workload_diag_ble_terminal_with_pressure(
            &current, APP_STACK_DIAG_TERMINAL_ACK, &pressure);
        break;
    default:
        assert(false);
    }
}

static void *concurrent_worker_main(void *opaque)
{
    const struct concurrent_worker *worker = opaque;

    worker_note_ready();
    for (uint32_t iteration = 0u; iteration < worker->rounds; iteration++) {
        int barrier_result = pthread_barrier_wait(worker->barrier);

        assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
        run_concurrent_workload(worker->workload, iteration);
    }
    return NULL;
}

static void test_concurrent_workload_state_is_serialized(void)
{
    pthread_t threads[3];
    pthread_barrier_t barrier;
    struct concurrent_worker workers[3] = {
        { CONCURRENT_GATEWAY_REPORT, &barrier, 1u },
        { CONCURRENT_GATEWAY_CONTROL, &barrier, 1u },
        { CONCURRENT_BLE_BACKPRESSURE, &barrier, 1u },
    };
    uint32_t begins_before = begin_count;
    uint32_t samples_before = sample_count;
    uint32_t ends_before = end_count;

    assert(active_count == 0u);
    assert(pthread_barrier_init(&barrier, NULL, 3u) == 0);
    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    block_next_begin = true;
    blocked_begin_entered = false;
    release_blocked_begin = false;
    max_diag_calls_in_flight = 0u;
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);
    assert(pthread_mutex_lock(&lock_attempt_mutex) == 0);
    lock_attempt_count = 0u;
    assert(pthread_mutex_unlock(&lock_attempt_mutex) == 0);

    assert(pthread_create(&threads[0], NULL, concurrent_worker_main,
                          &workers[0]) == 0);
    assert(pthread_create(&threads[1], NULL, concurrent_worker_main,
                          &workers[1]) == 0);
    assert(pthread_create(&threads[2], NULL, concurrent_worker_main,
                          &workers[2]) == 0);

    assert(pthread_mutex_lock(&worker_ready_mutex) == 0);
    while (workers_ready < 3u) {
        assert(pthread_cond_wait(&worker_ready_cond, &worker_ready_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&worker_ready_mutex) == 0);

    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    while (!blocked_begin_entered) {
        assert(pthread_cond_wait(&diag_stub_cond, &diag_stub_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);

    assert(pthread_mutex_lock(&lock_attempt_mutex) == 0);
    while (lock_attempt_count < 3u) {
        assert(pthread_cond_wait(&lock_attempt_cond, &lock_attempt_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&lock_attempt_mutex) == 0);

    assert(pthread_mutex_lock(&diag_stub_mutex) == 0);
    release_blocked_begin = true;
    assert(pthread_cond_broadcast(&diag_stub_cond) == 0);
    assert(pthread_mutex_unlock(&diag_stub_mutex) == 0);

    for (size_t index = 0u; index < 3u; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(max_diag_calls_in_flight == 1u);
    assert(begin_count == begins_before + 3u);
    assert(sample_count == samples_before + 3u);
    assert(end_count == ends_before + 3u);
    assert(active_count == 0u);
}

static void test_concurrent_gateway_workloads_stress(void)
{
    pthread_t threads[3];
    pthread_barrier_t barrier;
    struct concurrent_worker workers[3] = {
        { CONCURRENT_GATEWAY_REPORT, &barrier, CONCURRENCY_STRESS_ROUNDS },
        { CONCURRENT_GATEWAY_CONTROL, &barrier, CONCURRENCY_STRESS_ROUNDS },
        { CONCURRENT_BLE_BACKPRESSURE, &barrier, CONCURRENCY_STRESS_ROUNDS },
    };
    const uint32_t expected = 3u * CONCURRENCY_STRESS_ROUNDS;
    uint32_t begins_before = begin_count;
    uint32_t samples_before = sample_count;
    uint32_t ends_before = end_count;

    workers_ready = 0u;
    assert(pthread_barrier_init(&barrier, NULL, 3u) == 0);
    for (size_t index = 0u; index < 3u; index++) {
        assert(pthread_create(&threads[index], NULL, concurrent_worker_main,
                              &workers[index]) == 0);
    }
    for (size_t index = 0u; index < 3u; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
    assert(pthread_barrier_destroy(&barrier) == 0);
    assert(begin_count == begins_before + expected);
    assert(sample_count == samples_before + expected);
    assert(end_count == ends_before + expected);
    assert(active_count == 0u);
}

static void test_failed_diagnostic_commits_retain_only_usable_runs(void)
{
    struct proto_packet begin_failure = packet(20u, 200u, 20u);
    struct proto_packet sample_failure = packet(21u, 201u, 21u);
    struct proto_packet end_failure = packet(22u, 202u, 22u);
    uint32_t begins_before = begin_count;
    uint32_t samples_before = sample_count;
    uint32_t sample_attempts_before = sample_attempt_count;
    uint32_t ends_before = end_count;
    uint32_t end_attempts_before = end_attempt_count;

    begin_failures_remaining = 1u;
    app_stack_workload_diag_gateway_control_admit(&begin_failure, 1u, 0u);
    app_stack_workload_diag_gateway_control_sample(&begin_failure, 1u, 0u);
    app_stack_workload_diag_gateway_control_release(&begin_failure, 0, 0u, 0u);
    assert(begin_count == begins_before + 1u);
    assert(sample_attempt_count == sample_attempts_before);
    assert(end_attempt_count == end_attempts_before);
    assert(active_count == 0u);

    app_stack_workload_diag_gateway_control_admit(&begin_failure, 1u, 0u);
    app_stack_workload_diag_gateway_control_sample(&begin_failure, 1u, 0u);
    app_stack_workload_diag_gateway_control_release(&begin_failure, 0, 0u, 0u);
    assert(begin_count == begins_before + 2u);
    assert(sample_count == samples_before + 1u);
    assert(end_count == ends_before + 1u);
    assert(active_count == 0u);

    app_stack_workload_diag_gateway_control_admit(&sample_failure, 1u, 0u);
    sample_failures_remaining = 1u;
    app_stack_workload_diag_gateway_control_sample(&sample_failure, 1u, 0u);
    assert(sample_count == samples_before + 1u);
    app_stack_workload_diag_gateway_control_sample(&sample_failure, 1u, 0u);
    assert(sample_count == samples_before + 2u);
    app_stack_workload_diag_gateway_control_release(&sample_failure, 0, 0u, 0u);
    assert(active_count == 0u);

    app_stack_workload_diag_gateway_control_admit(&end_failure, 1u, 0u);
    app_stack_workload_diag_gateway_control_sample(&end_failure, 1u, 0u);
    end_failures_remaining = 1u;
    app_stack_workload_diag_gateway_control_release(&end_failure, 0, 0u, 0u);
    assert(active_count == 1u);
    assert(end_count == ends_before + 2u);
    app_stack_workload_diag_gateway_control_release(&end_failure, 0, 0u, 0u);
    assert(end_attempt_count == end_attempts_before + 4u);
    assert(end_count == ends_before + 3u);
    assert(active_count == 0u);
}

int main(void)
{
    struct proto_packet first = packet(1u, 10u, 1u);
    struct proto_packet second = packet(1u, 11u, 2u);
    struct proto_packet cir = packet(2u, 12u, 3u);
    struct proto_packet relay = packet(3u, 13u, 4u);
    struct proto_packet ble = packet(4u, 14u, 5u);
    struct proto_packet overflow = packet(5u, 15u, 6u);
    const struct app_stack_workload_diag_pressure ble_pressure = {
        .queue_depth = 7u,
        .custody_depth = 3u,
        .credit_available = 1u,
        .retry_depth = 2u,
        .drain_depth = 4u,
    };

    app_stack_workload_diag_click_admit(&first, 1u, 1u);
    app_stack_workload_diag_click_admit(&second, 2u, 2u);
    app_stack_workload_diag_cir_admit(&cir, 3u, 3u);
    app_stack_workload_diag_relay_admit(&relay, 4u, 4u);
    app_stack_workload_diag_ble_admit_with_pressure(
        &ble, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE, &ble_pressure);
    app_stack_workload_diag_click_admit(&overflow, 5u, 5u);
    assert(begin_count == APP_STACK_DIAG_COMBINED_PEAK_RUNS + 1u);
    assert(owners[4] == APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE);
    assert(begin_states[4].credit_available == 1u);
    assert(begin_states[4].retry_depth == 2u);
    assert(begin_states[4].drain_depth == 4u);

    app_stack_workload_diag_ble_sample_with_pressure(&ble, &ble_pressure);
    app_stack_workload_diag_click_sample(&second, 2u, 2u);
    assert(sample_count == 2u);

    app_stack_workload_diag_click_release(&first, 0, 4u, 4u);
    app_stack_workload_diag_cir_release(&cir, -1, 3u, 3u);
    app_stack_workload_diag_relay_release(&relay, 0, 2u, 2u);
    app_stack_workload_diag_ble_terminal_with_pressure(
        &ble, APP_STACK_DIAG_TERMINAL_DISCONNECT, &ble_pressure);
    assert(end_count == 4u);
    assert(outcomes[0] == APP_STACK_DIAG_TERMINAL_ACK);
    assert(outcomes[1] == APP_STACK_DIAG_TERMINAL_ERROR);
    assert(outcomes[3] == APP_STACK_DIAG_TERMINAL_DISCONNECT);

    app_stack_workload_diag_click_release(&second, 0, 0u, 0u);
    assert(end_count == 5u);
    assert(active_count == 0u);

    app_stack_workload_diag_click_activity_admit(&first, 1u, 1u);
    app_stack_workload_diag_click_activity_sample(&first, 1u, 1u);
    app_stack_workload_diag_click_activity_release(&first, 0, 0u, 0u);
    app_stack_workload_diag_anchor_survey_admit(&cir, 1u, 1u);
    app_stack_workload_diag_anchor_survey_sample(&cir, 1u, 1u);
    app_stack_workload_diag_anchor_survey_release(&cir, 0, 0u, 0u);
    app_stack_workload_diag_gateway_report_cycle(&relay, 1u, 1u);
    app_stack_workload_diag_gateway_control_admit(&ble, 1u, 1u);
    app_stack_workload_diag_gateway_control_sample(&ble, 1u, 1u);
    app_stack_workload_diag_gateway_control_release(&ble, 0, 0u, 0u);
    assert(workloads[6] == APP_STACK_DIAG_WORKLOAD_CLICK_ACTIVITY);
    assert(workloads[7] == APP_STACK_DIAG_WORKLOAD_ANCHOR_SURVEY_REPORT);
    assert(workloads[8] == APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS);
    assert(workloads[9] == APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL);
    assert(owners[6] == APP_STACK_DIAG_OWNER_CLICKER_ACTION);
    assert(owners[7] == APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN);
    assert(owners[8] == APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE);
    assert(owners[9] == APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE);
    assert(end_count == 9u);
    assert(active_count == 0u);
    test_failed_diagnostic_commits_retain_only_usable_runs();
    test_concurrent_workload_state_is_serialized();
    test_concurrent_gateway_workloads_stress();
    return 0;
}
