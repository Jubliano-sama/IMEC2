#include "app_node_comm.h"
#include "app_mesh_report.h"

#include <zephyr/kernel.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static atomic_int_fast64_t fake_now_ms;
static atomic_bool radio_busy;
static atomic_bool rx_response_active;
static uint32_t send_calls;
static uint32_t try_flood_calls;
static int try_flood_results[16];
static bool try_flood_sent[16];
static struct mesh_outbound try_flood_envelopes[16];
static uint32_t queue_calls;
static uint32_t stop_scan_calls;
static uint32_t restart_scan_calls;
static uint32_t receive_abort_calls;
static uint32_t legacy_queue_depth;
static atomic_bool receive_abort_pending;
static atomic_bool transport_paused;
static uint32_t transport_pause_calls;
static uint32_t transport_resume_calls;
static uint32_t route_refresh_pause_calls;
static uint32_t route_refresh_resume_calls;
static uint32_t watchdog_stop_calls;
static uint32_t reschedule_calls;
static struct k_work_delayable *last_rescheduled_work;
static int last_rescheduled_delay_ms;

static pthread_mutex_t interleave_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleave_cond = PTHREAD_COND_INITIALIZER;
static bool block_send;
static bool send_entered;
static bool release_send;
static bool block_resume;
static bool resume_entered;
static bool release_resume;

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

void zephyr_shim_note_work_reschedule(struct k_work_delayable *work,
                                      int timeout)
{
    reschedule_calls++;
    last_rescheduled_work = work;
    last_rescheduled_delay_ms = timeout;
}

void app_watchdog_stop_feeding(void)
{
    watchdog_stop_calls++;
}

void app_node_comm_gateway_route_refresh_pause(uint32_t now_ms)
{
    (void)now_ms;
    route_refresh_pause_calls++;
}

void app_node_comm_gateway_route_refresh_resume(uint32_t now_ms)
{
    (void)now_ms;
    route_refresh_resume_calls++;
}

void app_node_comm_gateway_route_refresh_start(void)
{
}

int app_node_comm_gateway_route_refresh_request(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation)
{
    (void)delay_ms;
    (void)reason;
    (void)forced;
    (void)correlation;
    return 0;
}

int64_t k_uptime_get(void)
{
    return atomic_load(&fake_now_ms);
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    (void)callbacks;
    return 0;
}

bool radio_guard_uwb_busy(void)
{
    return atomic_load(&radio_busy);
}

bool mesh_rx_response_active(void)
{
    return atomic_load(&rx_response_active);
}

void dwm3000_driver_request_receive_abort(void)
{
    receive_abort_calls++;
    atomic_store(&receive_abort_pending, true);
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return atomic_load(&receive_abort_pending);
}

int mesh_transport_pause_preserving_queued(void)
{
    transport_pause_calls++;
    stop_scan_calls++;
    atomic_store(&transport_paused, true);
    return 0;
}

bool mesh_transport_quiesced(void)
{
    return atomic_load(&transport_paused) &&
           !atomic_load(&radio_busy) &&
           !atomic_load(&rx_response_active);
}

void mesh_transport_resume(void)
{
    transport_resume_calls++;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_resume) {
        resume_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_resume) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    restart_scan_calls++;
    atomic_store(&transport_paused, false);
}

void mesh_stop_role_scan(void)
{
    stop_scan_calls++;
}

void mesh_restart_role_scan(void)
{
    restart_scan_calls++;
}

int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    (void)out;
    (void)reason;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    send_calls++;
    if (block_send) {
        atomic_store(&radio_busy, true);
        send_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
        atomic_store(&radio_busy, false);
        atomic_store(&receive_abort_pending, false);
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    return 17;
}

int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason)
{
    (void)out;
    (void)purpose;
    (void)mode;
    (void)reason;
    send_calls++;
    return 18;
}

int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason,
                       bool *sent_now)
{
    (void)out;
    (void)purpose;
    (void)reason;
    if (sent_now != NULL) {
        *sent_now = true;
    }
    send_calls++;
    return 19;
}

int mesh_try_send_c5_flood(const struct mesh_outbound *out,
                           uint8_t purpose,
                           const char *reason,
                           bool *sent_now)
{
    uint32_t index = try_flood_calls++;

    (void)purpose;
    (void)reason;
    assert(index < sizeof(try_flood_results) /
                       sizeof(try_flood_results[0]));
    assert(out != NULL);
    try_flood_envelopes[index] = *out;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_send) {
        atomic_store(&radio_busy, true);
        send_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
        atomic_store(&radio_busy, false);
        atomic_store(&receive_abort_pending, false);
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    if (sent_now != NULL) {
        *sent_now = try_flood_sent[index];
    }
    return try_flood_results[index];
}

int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool *sent_now)
{
    struct mesh_outbound out = {0};

    assert(view != NULL);
    assert(view->packet != NULL);
    assert(view->payload_len <= sizeof(out.payload));
    out.packet = *view->packet;
    memcpy(out.payload, view->payload, view->payload_len);
    out.payload_len = view->payload_len;
    out.radio_channel = view->radio_channel;
    out.next_hop_id = view->next_hop_id;
    out.queued_at_ms = view->queued_at_ms;
    out.earliest_tx_ms = view->earliest_tx_ms;
    out.flood_retry_count = view->flood_retry_count;
    return mesh_try_send_c5_flood(&out, purpose, reason, sent_now);
}

int mesh_request_route(uint64_t target_id, const char *reason)
{
    (void)target_id;
    (void)reason;
    send_calls++;
    return 20;
}

int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    (void)out;
    (void)reason;
    send_calls++;
    return 21;
}

int mesh_start_owned_tracked_tx(const struct mesh_outbound *out,
                                const char *reason,
                                bool *rf_sent)
{
    (void)out;
    (void)reason;
    if (rf_sent != NULL) {
        *rf_sent = true;
    }
    send_calls++;
    return 22;
}

int queue_anchor_report(const struct mesh_outbound *out)
{
    (void)out;
    queue_calls++;
    legacy_queue_depth++;
    return 23;
}

bool mesh_report_tx_backlog_active(void)
{
    return legacy_queue_depth != 0u;
}

bool mesh_report_ch9_ack_wait_active(void)
{
    return false;
}

void mesh_delivery_health_get(struct mesh_delivery_health *health)
{
    (void)health;
}

static void reset_fixture(void)
{
    atomic_store(&fake_now_ms, 0);
    atomic_store(&radio_busy, false);
    atomic_store(&rx_response_active, false);
    send_calls = 0u;
    try_flood_calls = 0u;
    memset(try_flood_results, 0, sizeof(try_flood_results));
    memset(try_flood_sent, 1, sizeof(try_flood_sent));
    memset(try_flood_envelopes, 0, sizeof(try_flood_envelopes));
    queue_calls = 0u;
    stop_scan_calls = 0u;
    restart_scan_calls = 0u;
    receive_abort_calls = 0u;
    atomic_store(&receive_abort_pending, false);
    atomic_store(&transport_paused, false);
    transport_pause_calls = 0u;
    transport_resume_calls = 0u;
    route_refresh_pause_calls = 0u;
    route_refresh_resume_calls = 0u;
    watchdog_stop_calls = 0u;
    reschedule_calls = 0u;
    last_rescheduled_work = NULL;
    last_rescheduled_delay_ms = -1;
    legacy_queue_depth = 3u;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = false;
    send_entered = false;
    release_send = false;
    block_resume = false;
    resume_entered = false;
    release_resume = false;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(app_node_comm_init(NULL) == 0);
    assert(app_node_comm_policy_running());
}

static void test_pause_and_stop_gate_new_submissions_without_clearing_queue(void)
{
    struct node_comm_pause_lease pause_lease;
    uint32_t original_queue_depth;

    reset_fixture();
    original_queue_depth = legacy_queue_depth;
    assert(app_node_comm_send(NULL, "running") == 17);
    assert(send_calls == 1u);
    assert(app_node_comm_pause_request(1u, 100u, &pause_lease) == 0);
    assert(stop_scan_calls == 1u);
    assert(route_refresh_pause_calls == 1u);
    assert(app_node_comm_send(NULL, "quiescing") == -ESHUTDOWN);
    assert(send_calls == 1u);
    assert(app_node_comm_pause_note_quiesced(&pause_lease) == 0);
    assert(app_node_comm_queue_local_delivery(NULL) == -ESHUTDOWN);
    assert(queue_calls == 0u);
    assert(legacy_queue_depth == original_queue_depth);
    assert(app_node_comm_resume_begin(&pause_lease) == 0);
    assert(app_node_comm_resume_complete(&pause_lease) == 0);
    assert(restart_scan_calls == 1u);
    assert(route_refresh_resume_calls == 1u);
    assert(app_node_comm_send(NULL, "resumed") == 17);
    assert(send_calls == 2u);

    assert(app_node_comm_stop_preserving_queued() == 0);
    assert(route_refresh_pause_calls == 2u);
    assert(app_node_comm_send(NULL, "stopped") == -ESHUTDOWN);
    assert(app_node_comm_queue_local_delivery(NULL) == -ESHUTDOWN);
    assert(legacy_queue_depth == original_queue_depth);
    assert(app_node_comm_start() == 0);
    assert(route_refresh_resume_calls == 2u);
    assert(app_node_comm_send(NULL, "restarted") == 17);
    assert(legacy_queue_depth == original_queue_depth);
}

static void test_safe_boundary_and_expired_pause_force_reclaim(void)
{
    struct node_comm_pause_lease caller_lease;
    struct node_comm_pause_lease recovery_lease;

    reset_fixture();
    assert(app_node_comm_pause_request(2u, 10u, &caller_lease) == 0);
    radio_busy = true;
    assert(app_node_comm_pause_note_quiesced(&caller_lease) == -EBUSY);
    radio_busy = false;
    assert(app_node_comm_pause_note_quiesced(&caller_lease) == 0);
    rx_response_active = true;
    fake_now_ms = 10;
    app_node_comm_lifecycle_service();
    assert(receive_abort_calls == 1u);
    assert(!app_node_comm_policy_running());
    assert(app_node_comm_send(NULL, "forced-reclaim") == -ESHUTDOWN);
    assert(app_node_comm_forced_reclaim_lease(&recovery_lease));
    assert(recovery_lease.generation != caller_lease.generation);
    assert(app_node_comm_resume_complete(&caller_lease) == -ESTALE);
    assert(app_node_comm_resume_complete(&recovery_lease) == -EBUSY);
    rx_response_active = false;
    receive_abort_pending = false;
    assert(app_node_comm_resume_complete(&recovery_lease) == 0);
    assert(app_node_comm_policy_running());
}

static void test_idle_pause_expiry_recovers_without_an_api_poll(void)
{
    struct node_comm_pause_lease caller_lease;

    reset_fixture();
    assert(app_node_comm_pause_request(3u, 10u, &caller_lease) == 0);
    assert(last_rescheduled_work != NULL);
    assert(last_rescheduled_delay_ms == 10);
    assert(last_rescheduled_work->work.handler != NULL);
    fake_now_ms = 10;
    last_rescheduled_work->work.handler(&last_rescheduled_work->work);
    assert(app_node_comm_policy_running());
    assert(receive_abort_calls == 0u);
    assert(route_refresh_resume_calls == 1u);
    assert(restart_scan_calls == 1u);
}

static void test_stuck_pause_expiry_escalates_once_without_polling_forever(void)
{
    struct node_comm_pause_lease caller_lease;
    struct k_work_delayable *watchdog_work;
    uint32_t reschedules_before_terminal;

    reset_fixture();
    assert(app_node_comm_pause_request(4u, 10u, &caller_lease) == 0);
    watchdog_work = last_rescheduled_work;
    assert(watchdog_work != NULL);
    radio_busy = true;
    rx_response_active = true;
    fake_now_ms = 10;
    watchdog_work->work.handler(&watchdog_work->work);
    assert(receive_abort_calls == 1u);
    assert(watchdog_stop_calls == 0u);
    assert(last_rescheduled_delay_ms == 10);

    reschedules_before_terminal = reschedule_calls;
    fake_now_ms = 1010;
    watchdog_work->work.handler(&watchdog_work->work);
    assert(watchdog_stop_calls == 1u);
    assert(reschedule_calls == reschedules_before_terminal);
    assert(!app_node_comm_policy_running());
}

struct adapter_thread_result {
    int result;
    bool done;
    struct node_comm_pause_lease lease;
};

static void adapter_thread_complete(struct adapter_thread_result *result,
                                    int value)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    result->result = value;
    result->done = true;
    assert(pthread_cond_broadcast(&interleave_cond) == 0);
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void *adapter_send_thread(void *arg)
{
    struct adapter_thread_result *result = arg;

    adapter_thread_complete(result,
                            app_node_comm_send(NULL, "interleaved-send"));
    return NULL;
}

static void *adapter_pause_thread(void *arg)
{
    struct adapter_thread_result *result = arg;
    int ret = app_node_comm_pause_request(7u, 1000u, &result->lease);

    adapter_thread_complete(result, ret);
    return NULL;
}

static void *adapter_stop_thread(void *arg)
{
    struct adapter_thread_result *result = arg;

    adapter_thread_complete(result, app_node_comm_stop_preserving_queued());
    return NULL;
}

static void *adapter_resume_complete_thread(void *arg)
{
    struct adapter_thread_result *result = arg;

    adapter_thread_complete(
        result, app_node_comm_resume_complete(&result->lease));
    return NULL;
}

static void *adapter_delivery_service_thread(void *arg)
{
    struct adapter_thread_result *result = arg;

    adapter_thread_complete(result, app_node_comm_service_deliveries());
    return NULL;
}

static void release_blocked_send(void)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    release_send = true;
    assert(pthread_cond_broadcast(&interleave_cond) == 0);
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void release_blocked_resume(void)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    release_resume = true;
    assert(pthread_cond_broadcast(&interleave_cond) == 0);
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void test_pause_completes_while_backend_send_is_blocked(void)
{
    struct adapter_thread_result send_result = {0};
    struct adapter_thread_result pause_result = {0};
    struct adapter_thread_result rejected_result = {0};
    pthread_t send_thread;
    pthread_t pause_thread;
    pthread_t rejected_thread;

    reset_fixture();
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&send_thread, NULL, adapter_send_thread,
                          &send_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(pthread_create(&pause_thread, NULL, adapter_pause_thread,
                          &pause_result) == 0);
    wait_for_interleave_flag(&pause_result.done);
    assert(pause_result.result == 0);
    assert(transport_pause_calls == 1u);
    assert(app_node_comm_pause_note_quiesced(&pause_result.lease) == -EBUSY);

    assert(pthread_create(&rejected_thread, NULL, adapter_send_thread,
                          &rejected_result) == 0);
    wait_for_interleave_flag(&rejected_result.done);
    assert(rejected_result.result == -ESHUTDOWN);
    assert(send_calls == 1u);

    release_blocked_send();
    assert(pthread_join(send_thread, NULL) == 0);
    assert(pthread_join(pause_thread, NULL) == 0);
    assert(pthread_join(rejected_thread, NULL) == 0);
    assert(send_result.result == 17);
    assert(send_calls == 1u);
    assert(app_node_comm_pause_note_quiesced(&pause_result.lease) == 0);
    assert(app_node_comm_resume_begin(&pause_result.lease) == 0);
    assert(app_node_comm_resume_complete(&pause_result.lease) == 0);
    assert(transport_resume_calls == 1u);
}

static void test_stop_completes_while_backend_send_is_blocked(void)
{
    struct adapter_thread_result send_result = {0};
    struct adapter_thread_result stop_result = {0};
    struct adapter_thread_result rejected_result = {0};
    pthread_t send_thread;
    pthread_t stop_thread;
    pthread_t rejected_thread;

    reset_fixture();
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&send_thread, NULL, adapter_send_thread,
                          &send_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(pthread_create(&stop_thread, NULL, adapter_stop_thread,
                          &stop_result) == 0);
    wait_for_interleave_flag(&stop_result.done);
    assert(stop_result.result == -EINPROGRESS);
    assert(transport_pause_calls == 1u);
    assert(receive_abort_calls == 1u);

    assert(pthread_create(&rejected_thread, NULL, adapter_send_thread,
                          &rejected_result) == 0);
    wait_for_interleave_flag(&rejected_result.done);
    assert(rejected_result.result == -ESHUTDOWN);
    assert(send_calls == 1u);

    release_blocked_send();
    assert(pthread_join(send_thread, NULL) == 0);
    assert(pthread_join(stop_thread, NULL) == 0);
    assert(pthread_join(rejected_thread, NULL) == 0);
    assert(send_result.result == 17);
    assert(send_calls == 1u);
    atomic_store(&receive_abort_pending, false);
    assert(app_node_comm_stop_preserving_queued() == 0);
    assert(app_node_comm_start() == 0);
    assert(transport_resume_calls == 1u);
}

static void test_pause_expiry_preserves_full_64_bit_uptime(void)
{
    const int64_t start_ms = INT64_C(0x100000000) + 37;
    struct node_comm_pause_lease lease;

    reset_fixture();
    atomic_store(&fake_now_ms, start_ms);
    assert(app_node_comm_pause_request(8u, 100u, &lease) == 0);
    assert(lease.expires_at_ms == (uint64_t)start_ms + 100u);
    assert(app_node_comm_pause_note_quiesced(&lease) == 0);

    atomic_store(&fake_now_ms, (int64_t)lease.expires_at_ms - 1);
    app_node_comm_lifecycle_service();
    assert(!app_node_comm_policy_running());
    atomic_store(&fake_now_ms, (int64_t)lease.expires_at_ms);
    app_node_comm_lifecycle_service();
    assert(app_node_comm_policy_running());
    assert(route_refresh_resume_calls == 1u);
}

static void test_send_stays_closed_until_backend_resume_is_ready(void)
{
    struct adapter_thread_result resume_result = {0};
    pthread_t resume_thread;
    uint32_t sends_before_resume;

    reset_fixture();
    assert(app_node_comm_pause_request(9u, 1000u,
                                       &resume_result.lease) == 0);
    assert(app_node_comm_pause_note_quiesced(&resume_result.lease) == 0);
    assert(app_node_comm_resume_begin(&resume_result.lease) == 0);

    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_resume = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    sends_before_resume = send_calls;
    assert(pthread_create(&resume_thread, NULL,
                          adapter_resume_complete_thread,
                          &resume_result) == 0);
    wait_for_interleave_flag(&resume_entered);

    assert(app_node_comm_send(NULL, "resume-backend-not-ready") ==
           -ESHUTDOWN);
    assert(send_calls == sends_before_resume);

    release_blocked_resume();
    wait_for_interleave_flag(&resume_result.done);
    assert(pthread_join(resume_thread, NULL) == 0);
    assert(resume_result.result == 0);
    assert(app_node_comm_send(NULL, "resume-backend-ready") == 17);
    assert(send_calls == sends_before_resume + 1u);
}

static struct mesh_outbound delivery_envelope(uint16_t seq)
{
    struct mesh_outbound envelope = {0};

    envelope.packet.msg_type = MSG_COMMAND;
    envelope.packet.src_id = UINT64_C(0x1111222233334444);
    envelope.packet.dst_id = MESH_BROADCAST_ID;
    envelope.packet.session_id = UINT32_C(0x55667788);
    envelope.packet.seq = seq;
    envelope.packet.ttl = 4u;
    envelope.packet.payload_len = 3u;
    envelope.payload[0] = 0xa1u;
    envelope.payload[1] = 0xb2u;
    envelope.payload[2] = 0xc3u;
    envelope.payload_len = 3u;
    envelope.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    envelope.next_hop_id = MESH_BROADCAST_ID;
    envelope.queued_at_ms = 7u;
    envelope.earliest_tx_ms = 0u;
    envelope.flood_retry_count = 0u;
    return envelope;
}

static void test_delivery_copies_envelope_and_runs_four_rf_opportunities(void)
{
    struct mesh_outbound envelope = delivery_envelope(10u);
    const struct mesh_outbound expected = envelope;
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        77u,
        &handle) == 0);
    envelope.packet.seq = 999u;
    envelope.packet.session_id = 0u;
    memset(envelope.payload, 0xee, sizeof(envelope.payload));

    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(attempt * 40u));
        if (attempt == 0u) {
            assert(last_rescheduled_work != NULL);
            assert(last_rescheduled_delay_ms == 0);
            last_rescheduled_work->work.handler(&last_rescheduled_work->work);
        } else {
            assert(app_node_comm_service_deliveries() == 0);
        }
        assert(try_flood_calls == attempt + 1u);
        assert(memcmp(&try_flood_envelopes[attempt],
                      &expected,
                      sizeof(expected)) == 0);
        if (attempt < 3u) {
            assert(!app_node_comm_take_delivery_event_for(handle, &event));
        }
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.handle == handle);
    assert(event.client_token == 77u);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 4u);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_delivery_pre_rf_busy_defers_without_consuming_attempts(void)
{
    struct mesh_outbound envelope = delivery_envelope(11u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    try_flood_results[0] = -EBUSY;
    try_flood_sent[0] = false;
    try_flood_results[1] = -EAGAIN;
    try_flood_sent[1] = false;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        78u,
        &handle) == 0);

    assert(app_node_comm_service_deliveries() == -EBUSY);
    atomic_store(&fake_now_ms, 9);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_flood_calls == 1u);
    atomic_store(&fake_now_ms, 10);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_flood_calls == 2u);

    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(20u + attempt * 40u));
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 4u);
    assert(try_flood_calls == 6u);
}

static void test_delivery_rejects_unbounded_or_over_capacity_work(void)
{
    struct mesh_outbound envelope = delivery_envelope(12u);
    struct node_comm_terminal_event event;
    uint32_t handles[APP_NODE_COMM_MAX_DELIVERIES];
    uint32_t extra;

    reset_fixture();
    assert(app_node_comm_submit_delivery(NULL,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 1u, &extra) ==
        -EINVAL);
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 0u, 1u, &extra) ==
        -EINVAL);
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_RELIABLE_UPLINK, 1000u, 1u, &extra) ==
        -ENOTSUP);
    envelope.flood_retry_count = 1u;
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 1u, &extra) ==
        -EINVAL);
    envelope = delivery_envelope(12u);
    envelope.payload_len = APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN + 1u;
    envelope.packet.payload_len = envelope.payload_len;
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 1u, &extra) ==
        -EMSGSIZE);
    envelope = delivery_envelope(12u);
    envelope.packet.payload_len++;
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 1u, &extra) ==
        -EINVAL);
    envelope = delivery_envelope(12u);
    atomic_store(&fake_now_ms, 1000);
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 1u, &extra) ==
        -ETIMEDOUT);

    reset_fixture();
    for (uint32_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        envelope = delivery_envelope((uint16_t)(20u + i));
        assert(app_node_comm_submit_delivery(&envelope,
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            1000u,
            100u + i,
            &handles[i]) == 0);
    }
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 200u, &extra) ==
        -ENOSPC);
    assert(app_node_comm_cancel_delivery(handles[0]) == 0);
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 200u, &extra) ==
        -ENOSPC);
    assert(app_node_comm_take_delivery_event_for(handles[0], &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(app_node_comm_submit_delivery(&envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD, 1000u, 200u, &extra) == 0);
}

static void test_delivery_queue_survives_stop_and_restart(void)
{
    struct mesh_outbound envelope = delivery_envelope(30u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        300u,
        &handle) == 0);
    assert(app_node_comm_stop_preserving_queued() == 0);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_service_deliveries() == -ESHUTDOWN);
    assert(app_node_comm_start() == 0);
    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(attempt * 40u));
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

static void test_inflight_delivery_is_accounted_before_preserving_stop(void)
{
    struct mesh_outbound envelope = delivery_envelope(31u);
    struct adapter_thread_result service_result = {0};
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        301u,
        &handle) == 0);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(app_node_comm_stop_preserving_queued() == -EINPROGRESS);
    assert(app_node_comm_pending_delivery_count() == 1u);
    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(app_node_comm_stop_preserving_queued() == 0);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_start() == 0);

    for (uint32_t attempt = 1u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(attempt * 40u));
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 4u);
}

int main(void)
{
    test_pause_and_stop_gate_new_submissions_without_clearing_queue();
    test_safe_boundary_and_expired_pause_force_reclaim();
    test_idle_pause_expiry_recovers_without_an_api_poll();
    test_stuck_pause_expiry_escalates_once_without_polling_forever();
    test_pause_completes_while_backend_send_is_blocked();
    test_stop_completes_while_backend_send_is_blocked();
    test_pause_expiry_preserves_full_64_bit_uptime();
    test_send_stays_closed_until_backend_resume_is_ready();
    test_delivery_copies_envelope_and_runs_four_rf_opportunities();
    test_delivery_pre_rf_busy_defers_without_consuming_attempts();
    test_delivery_rejects_unbounded_or_over_capacity_work();
    test_delivery_queue_survives_stop_and_restart();
    test_inflight_delivery_is_accounted_before_preserving_stop();
    return 0;
}
