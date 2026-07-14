#include "app_node_comm.h"
#include "app_mesh_report.h"

#include <zephyr/kernel.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static atomic_int_fast64_t fake_now_ms;
static atomic_bool radio_busy;
static atomic_bool rx_response_active;
static uint32_t send_calls;
static struct mesh_outbound last_control_flood;
static uint32_t try_flood_calls;
static int try_flood_results[16];
static bool try_flood_sent[16];
static struct mesh_outbound try_flood_envelopes[16];
static uint32_t try_response_calls;
static int try_response_results[64];
static bool try_response_sent[64];
static struct mesh_outbound try_response_envelopes[64];
static uint32_t try_uplink_calls;
static int try_uplink_results[64];
static bool try_uplink_sent[64];
static bool try_uplink_confirmed[64];
static struct mesh_outbound try_uplink_envelopes[64];
static uint32_t cancel_uplink_calls;
static struct proto_packet last_cancelled_uplink;
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

void status_debug_printf(const char *fmt, ...)
{
    (void)fmt;
}

bool mesh_id_is_unicast(uint64_t node_id)
{
    return node_id != MESH_BROADCAST_ID;
}

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

int app_node_comm_gateway_route_refresh_request_bounded(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation,
    uint32_t timeout_ms)
{
    (void)timeout_ms;
    return app_node_comm_gateway_route_refresh_request(
        delay_ms, reason, forced, correlation);
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
    (void)purpose;
    (void)reason;
    if (out != NULL) {
        last_control_flood = *out;
    }
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

int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    bool *sent_now)
{
    struct mesh_outbound *out;
    uint32_t index = try_response_calls++;

    (void)reason;
    assert(index < sizeof(try_response_results) /
                       sizeof(try_response_results[0]));
    assert(view != NULL);
    assert(view->packet != NULL);
    out = &try_response_envelopes[index];
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    memcpy(out->payload, view->payload, view->payload_len);
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
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
        *sent_now = try_response_sent[index];
    }
    return try_response_results[index];
}

int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    bool *rf_started,
    bool *gateway_confirmed)
{
    struct mesh_outbound *out;
    uint32_t index = try_uplink_calls++;

    (void)reason;
    assert(index < sizeof(try_uplink_results) /
                       sizeof(try_uplink_results[0]));
    assert(view != NULL);
    assert(view->packet != NULL);
    out = &try_uplink_envelopes[index];
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    memcpy(out->payload, view->payload, view->payload_len);
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
    if (rf_started != NULL) {
        *rf_started = try_uplink_sent[index];
    }
    if (gateway_confirmed != NULL) {
        *gateway_confirmed = try_uplink_confirmed[index];
    }
    return try_uplink_results[index];
}

int mesh_cancel_reliable_uplink(const struct proto_packet *packet)
{
    assert(packet != NULL);
    cancel_uplink_calls++;
    last_cancelled_uplink = *packet;
    return 0;
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
    try_response_calls = 0u;
    memset(try_response_results, 0, sizeof(try_response_results));
    memset(try_response_sent, 1, sizeof(try_response_sent));
    memset(try_response_envelopes, 0, sizeof(try_response_envelopes));
    try_uplink_calls = 0u;
    memset(try_uplink_results, 0, sizeof(try_uplink_results));
    memset(try_uplink_sent, 1, sizeof(try_uplink_sent));
    memset(try_uplink_confirmed, 0, sizeof(try_uplink_confirmed));
    memset(try_uplink_envelopes, 0, sizeof(try_uplink_envelopes));
    cancel_uplink_calls = 0u;
    memset(&last_cancelled_uplink, 0, sizeof(last_cancelled_uplink));
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

static struct mesh_outbound control_response_envelope(uint16_t seq)
{
    struct mesh_outbound envelope = {0};

    envelope.packet.msg_type = MSG_GATEWAY_ACK;
    envelope.packet.flags = FLAG_GATEWAY_ACK;
    envelope.packet.src_id = UINT64_C(0x9999888877776666);
    envelope.packet.dst_id = UINT64_C(0x1111222233334444);
    envelope.packet.session_id = UINT32_C(0x55667788);
    envelope.packet.seq = seq;
    envelope.packet.ttl = MESH_GATEWAY_ACK_TTL;
    envelope.packet.payload_len = 2u;
    envelope.payload[0] = 0xa1u;
    envelope.payload[1] = 0xb2u;
    envelope.payload_len = 2u;
    envelope.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    envelope.next_hop_id = envelope.packet.dst_id;
    envelope.queued_at_ms = 7u;
    return envelope;
}

static struct mesh_outbound reliable_uplink_envelope(uint16_t seq)
{
    struct mesh_outbound envelope = delivery_envelope(seq);

    envelope.packet.dst_id = UINT64_C(0x9999888877776666);
    envelope.next_hop_id = envelope.packet.dst_id;
    envelope.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    return envelope;
}

static void test_control_flood_freezes_age_before_wake_work(void)
{
    struct mesh_outbound envelope = delivery_envelope(9u);
    bool sent_now = false;

    reset_fixture();
    envelope.queued_at_ms = 0u;
    atomic_store(&fake_now_ms, 1234);
    assert(app_node_comm_send_control_flood(
        &envelope, 1u, "age-origin", &sent_now) == 19);
    assert(sent_now);
    assert(last_control_flood.queued_at_ms == 1234u);
    assert(envelope.queued_at_ms == 0u);

    envelope.queued_at_ms = 777u;
    atomic_store(&fake_now_ms, 4321);
    assert(app_node_comm_send_control_flood(
        &envelope, 1u, "preserve-origin", &sent_now) == 19);
    assert(last_control_flood.queued_at_ms == 777u);
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
    uint32_t first_delay_ms;
    uint32_t second_delay_ms;
    uint32_t first_rf_ms;
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
    assert(app_node_comm_retry_backoff_ms(
               &envelope, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1u, &first_delay_ms) == 0);
    assert(app_node_comm_retry_backoff_ms(
               &envelope, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               2u, &second_delay_ms) == 0);

    assert(app_node_comm_service_deliveries() == -EBUSY);
    atomic_store(&fake_now_ms, first_delay_ms - 1u);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_flood_calls == 1u);
    atomic_store(&fake_now_ms, first_delay_ms);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_flood_calls == 2u);

    first_rf_ms = first_delay_ms + second_delay_ms;
    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(first_rf_ms + attempt * 40u));
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 4u);
    assert(try_flood_calls == 6u);
}

static void test_auto_reaped_control_flood_retries_without_leaking_handle(void)
{
    struct mesh_outbound envelope = delivery_envelope(12u);
    struct node_comm_terminal_event event;
    uint32_t first_delay_ms;
    uint32_t handle;

    reset_fixture();
    try_flood_results[0] = -EBUSY;
    try_flood_sent[0] = false;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        79u,
        &handle) == 0);
    assert(app_node_comm_auto_reap_delivery(handle) == 0);
    assert(app_node_comm_retry_backoff_ms(
               &envelope, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1u, &first_delay_ms) == 0);

    assert(app_node_comm_service_deliveries() == -EBUSY);
    atomic_store(&fake_now_ms, first_delay_ms);
    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        assert(app_node_comm_service_deliveries() == 0);
        atomic_fetch_add(&fake_now_ms, 40);
    }

    assert(try_flood_calls == 5u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
}

static void test_gateway_control_flood_preempts_queued_control_response(void)
{
    struct mesh_outbound flood = delivery_envelope(40u);
    struct mesh_outbound response = control_response_envelope(41u);
    struct app_node_comm_control_response_health health;
    uint32_t flood_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &flood, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u, 400u, &flood_handle) == 0);
    assert(app_node_comm_submit_control_response(
        &response, 10000u, 401u) == 0);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_flood_calls == 1u);
    assert(try_response_calls == 0u);
    assert(memcmp(&try_flood_envelopes[0], &flood,
                  sizeof(flood)) == 0);
    for (uint8_t attempt = 1u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)attempt * 40);
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_response_calls == 1u);
    assert(memcmp(&try_response_envelopes[0], &response,
                  sizeof(response)) == 0);
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);
    assert(health.delivered == 1u);
    assert(health.failed == 0u);
    assert(health.last_attempts_started == 1u);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_auto_reap_delivery(flood_handle) == 0);
}

static void test_control_response_queued_during_active_control_runs_next(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound flood = delivery_envelope(44u);
    struct mesh_outbound response = control_response_envelope(45u);
    struct app_node_comm_control_response_health health;
    pthread_t service_thread;
    uint32_t flood_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &flood, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u, 404u, &flood_handle) == 0);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(app_node_comm_submit_control_response(
        &response, 10000u, 405u) == 0);
    assert(try_flood_calls == 1u);
    assert(try_response_calls == 0u);

    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);

    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = false;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_response_calls == 1u);
    assert(try_flood_calls == 1u);
    assert(memcmp(&try_response_envelopes[0], &response,
                  sizeof(response)) == 0);
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);
    assert(health.delivered == 1u);
    assert(health.failed == 0u);
    assert(app_node_comm_cancel_delivery(flood_handle) == 0);
}

static void test_control_response_busy_defers_without_spending_attempt(void)
{
    struct mesh_outbound response = control_response_envelope(42u);
    struct app_node_comm_control_response_health health;
    uint32_t retry_delay_ms;

    reset_fixture();
    try_response_results[0] = -EBUSY;
    try_response_sent[0] = false;
    assert(app_node_comm_submit_control_response(
        &response, 10000u, 402u) == 0);
    assert(app_node_comm_service_deliveries() == -EBUSY);
    assert(try_response_calls == 1u);
    assert(app_node_comm_retry_backoff_ms(
               &response, NODE_COMM_PROFILE_CONTROL_RESPONSE,
               1u, &retry_delay_ms) == 0);

    atomic_store(&fake_now_ms, retry_delay_ms - 1u);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    atomic_store(&fake_now_ms, retry_delay_ms);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_response_calls == 2u);
    app_node_comm_control_response_health_get(&health);
    assert(health.delivered == 1u);
    assert(health.last_attempts_started == 1u);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_control_response_retry_exhaustion_is_terminal_and_reaped(void)
{
    struct mesh_outbound response = control_response_envelope(43u);
    struct app_node_comm_control_response_health health;
    const uint32_t attempt_times[] = {0u, 1000u, 3000u, 7000u};

    reset_fixture();
    for (size_t i = 0u; i < 4u; i++) {
        try_response_results[i] = -ETIMEDOUT;
        try_response_sent[i] = true;
    }
    assert(app_node_comm_submit_control_response(
        &response, 10000u, 403u) == 0);
    for (size_t i = 0u; i < 4u; i++) {
        atomic_store(&fake_now_ms, attempt_times[i]);
        assert(app_node_comm_service_deliveries() == -ETIMEDOUT);
    }
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);
    assert(health.delivered == 0u);
    assert(health.failed == 1u);
    assert(health.last_terminal_reason ==
           NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(health.last_attempts_started == 4u);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_control_response_phy_errors_consume_four_real_attempts(void)
{
    struct mesh_outbound response = control_response_envelope(46u);
    struct app_node_comm_control_response_health health;
    const uint32_t attempt_times[] = {0u, 1000u, 3000u, 7000u};

    reset_fixture();
    for (size_t i = 0u; i < 4u; i++) {
        try_response_results[i] = -EIO;
        try_response_sent[i] = true;
    }
    assert(app_node_comm_submit_control_response(
        &response, 10000u, 406u) == 0);
    for (size_t i = 0u; i < 4u; i++) {
        atomic_store(&fake_now_ms, attempt_times[i]);
        assert(app_node_comm_service_deliveries() == -EIO);
    }
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);
    assert(health.delivered == 0u);
    assert(health.failed == 1u);
    assert(health.last_terminal_reason ==
           NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(health.last_attempts_started == 4u);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_control_response_pre_rf_phy_block_does_not_spend_attempt(void)
{
    struct mesh_outbound response = control_response_envelope(47u);
    struct app_node_comm_control_response_health health;
    uint32_t retry_delay_ms;

    reset_fixture();
    try_response_results[0] = -EIO;
    try_response_sent[0] = false;
    assert(app_node_comm_submit_control_response(
        &response, 10000u, 407u) == 0);
    assert(app_node_comm_service_deliveries() == -EIO);
    assert(try_response_calls == 1u);
    assert(app_node_comm_retry_backoff_ms(
               &response, NODE_COMM_PROFILE_CONTROL_RESPONSE,
               1u, &retry_delay_ms) == 0);

    atomic_store(&fake_now_ms, retry_delay_ms);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_response_calls == 2u);
    app_node_comm_control_response_health_get(&health);
    assert(health.delivered == 1u);
    assert(health.failed == 0u);
    assert(health.last_attempts_started == 1u);
}

static void test_control_response_terminal_records_do_not_leak_capacity(void)
{
    struct app_node_comm_control_response_health health;

    reset_fixture();
    for (uint16_t seq = 1u; seq <= 50u; seq++) {
        struct mesh_outbound response = control_response_envelope(seq);

        assert(app_node_comm_submit_control_response(
            &response, 10000u, seq) == 0);
        assert(app_node_comm_service_deliveries() == 0);
        assert(app_node_comm_pending_delivery_count() == 0u);
    }
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 50u);
    assert(health.delivered == 50u);
    assert(health.failed == 0u);
}

static void test_reliable_uplink_waits_for_exact_gateway_confirmation(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(60u);
    struct mesh_outbound conflicting = envelope;
    struct proto_packet wrong = envelope.packet;
    struct node_comm_terminal_event event;
    uint32_t duplicate_handle = 0u;
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 600u, &handle) == 0);
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 600u, &duplicate_handle) == 0);
    assert(duplicate_handle == handle);
    conflicting.payload[0] ^= 0xffu;
    assert(app_node_comm_submit_delivery(
        &conflicting, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 600u, &duplicate_handle) == -EEXIST);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(memcmp(&try_uplink_envelopes[0], &envelope,
                  sizeof(envelope)) == 0);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_pending_delivery_count() == 1u);

    wrong.seq++;
    assert(app_node_comm_note_gateway_confirmed(&wrong) == -ENOENT);
    assert(app_node_comm_note_gateway_confirmed(&envelope.packet) == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
    assert(event.client_token == 600u);
}

static void test_reliable_uplink_synchronous_and_late_confirmations_are_bounded(void)
{
    struct mesh_outbound immediate = reliable_uplink_envelope(61u);
    struct mesh_outbound late = reliable_uplink_envelope(62u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
        &immediate, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 601u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    assert(app_node_comm_submit_delivery(
        &late, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        100u, 602u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    atomic_store(&fake_now_ms, 100);
    assert(app_node_comm_note_gateway_confirmed(&late.packet) == -ESTALE);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 1u);
}

static void test_reliable_uplinks_serialize_single_flight_backend(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(63u);
    struct mesh_outbound second = reliable_uplink_envelope(64u);
    struct node_comm_terminal_event event;
    uint32_t retry_delay_ms;
    uint32_t first_handle;
    uint32_t second_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 603u, &first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 604u, &second_handle) == 0);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_retry_backoff_ms(
               &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               1u, &retry_delay_ms) == 0);

    assert(app_node_comm_note_gateway_confirmed(&first.packet) == 0);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));
    atomic_store(&fake_now_ms, retry_delay_ms);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(try_uplink_envelopes[1].packet.seq == second.packet.seq);
    assert(app_node_comm_note_gateway_confirmed(&second.packet) == 0);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));
}

static void test_cancelling_reliable_uplink_releases_backend_owner(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(65u);
    struct mesh_outbound second = reliable_uplink_envelope(66u);
    struct node_comm_terminal_event event;
    uint32_t first_handle;
    uint32_t second_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 605u, &first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 606u, &second_handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);

    assert(app_node_comm_cancel_delivery(first_handle) == 0);
    assert(cancel_uplink_calls == 1u);
    assert(last_cancelled_uplink.seq == first.packet.seq);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(try_uplink_envelopes[1].packet.seq == second.packet.seq);
    assert(app_node_comm_cancel_delivery(second_handle) == 0);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));
}

static void test_protocol_response_preempts_ordinary_reliable_uplink(void)
{
    struct mesh_outbound ordinary = reliable_uplink_envelope(67u);
    struct mesh_outbound protocol = reliable_uplink_envelope(68u);
    struct node_comm_terminal_event event;
    uint32_t ordinary_handle;
    uint32_t protocol_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &ordinary, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 607u, &ordinary_handle) == 0);
    assert(app_node_comm_submit_protocol_response(
        &protocol, 10000u, 608u, &protocol_handle) == 0);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_envelopes[0].packet.seq == protocol.packet.seq);
    assert(app_node_comm_note_gateway_confirmed(&protocol.packet) == 0);
    assert(app_node_comm_take_delivery_event_for(protocol_handle, &event));

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(try_uplink_envelopes[1].packet.seq == ordinary.packet.seq);
    assert(app_node_comm_abandon_delivery(ordinary_handle) == 0);
}

static void test_protocol_capacity_is_reserved_and_abandonment_reaps_slots(void)
{
    uint32_t handles[APP_NODE_COMM_MAX_DELIVERIES];
    uint32_t protocol_handle;
    uint32_t low_priority_count = APP_NODE_COMM_MAX_DELIVERIES -
                                  APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES;

    reset_fixture();
    for (uint32_t i = 0u; i < low_priority_count; i++) {
        struct mesh_outbound ordinary =
            reliable_uplink_envelope((uint16_t)(130u + i));

        assert(app_node_comm_submit_delivery(
            &ordinary, NODE_COMM_PROFILE_RELIABLE_UPLINK,
            10000u, 700u + i, &handles[i]) == 0);
    }
    {
        struct mesh_outbound blocked = reliable_uplink_envelope(140u);
        struct mesh_outbound protocol = reliable_uplink_envelope(141u);

        assert(app_node_comm_submit_delivery(
            &blocked, NODE_COMM_PROFILE_RELIABLE_UPLINK,
            10000u, 710u, &handles[low_priority_count]) == -ENOSPC);
        assert(app_node_comm_submit_protocol_response(
            &protocol, 10000u, 711u, &protocol_handle) == 0);
    }
    assert(app_node_comm_pending_delivery_count() ==
           APP_NODE_COMM_MAX_DELIVERIES);
    for (uint32_t i = 0u; i < low_priority_count; i++) {
        assert(app_node_comm_abandon_delivery(handles[i]) == 0);
    }
    assert(app_node_comm_abandon_delivery(protocol_handle) == 0);
    assert(app_node_comm_pending_delivery_count() == 0u);

    {
        struct mesh_outbound expired = reliable_uplink_envelope(142u);
        uint32_t expired_handle;

        assert(app_node_comm_submit_protocol_response(
            &expired, 100u, 712u, &expired_handle) == 0);
        atomic_store(&fake_now_ms, 100u);
        assert(app_node_comm_pending_delivery_count() == 1u);
        assert(app_node_comm_abandon_delivery(expired_handle) == 0);
        assert(app_node_comm_pending_delivery_count() == 0u);
        assert(app_node_comm_submit_protocol_response(
            &expired, 1000u, 713u, &expired_handle) == 0);
        assert(app_node_comm_abandon_delivery(expired_handle) == 0);
    }
}

static void test_fire_and_forget_reliable_uplinks_reap_terminal_capacity(void)
{
    reset_fixture();
    for (uint16_t seq = 70u; seq < 120u; seq++) {
        struct mesh_outbound envelope = reliable_uplink_envelope(seq);

        try_uplink_confirmed[try_uplink_calls] = true;
        assert(app_node_comm_submit_reliable_uplink(
            &envelope, 10000u, seq, NULL) == 0);
        assert(app_node_comm_service_deliveries() == 0);
        assert(app_node_comm_pending_delivery_count() == 0u);
    }
    assert(try_uplink_calls == 50u);
}

static void test_fire_and_forget_protocol_responses_reap_terminal_capacity(void)
{
    reset_fixture();
    for (uint16_t seq = 170u; seq < 220u; seq++) {
        struct mesh_outbound envelope = reliable_uplink_envelope(seq);

        try_uplink_confirmed[try_uplink_calls] = true;
        assert(app_node_comm_submit_protocol_response(
            &envelope, 10000u, seq, NULL) == 0);
        assert(app_node_comm_service_deliveries() == 0);
        assert(app_node_comm_pending_delivery_count() == 0u);
    }
    assert(try_uplink_calls == 50u);
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
        NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK, 1000u, 1u, &extra) ==
        0);
    assert(app_node_comm_cancel_delivery(extra) == 0);
    assert(app_node_comm_take_delivery_event_for(extra, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
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
    envelope = delivery_envelope(99u);
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

static void test_retry_backoff_hashes_complete_packet_identity(void)
{
    uint32_t previous_delay_ms = 0u;
    uint32_t changed = 0u;

    for (uint32_t anchor = 0u; anchor < 50u; anchor++) {
        struct mesh_outbound envelope = reliable_uplink_envelope(
            (uint16_t)(anchor + 1u));
        uint32_t delay_ms = 0u;

        envelope.packet.src_id = UINT64_C(0xa002000000010000) + anchor;
        envelope.packet.session_id = UINT32_C(0x50665006);
        assert(app_node_comm_retry_backoff_ms(
                   &envelope,
                   NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
                   1u,
                   &delay_ms) == 0);
        assert(delay_ms >= 25u && delay_ms <= 75u);
        if (anchor > 0u && delay_ms != previous_delay_ms) {
            changed++;
        }
        previous_delay_ms = delay_ms;
    }
    assert(changed >= 20u);
}

static void test_durable_uplink_uses_shared_reliable_backend(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(130u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               5000u,
               130u,
               &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

int main(void)
{
    test_control_flood_freezes_age_before_wake_work();
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
    test_auto_reaped_control_flood_retries_without_leaking_handle();
    test_gateway_control_flood_preempts_queued_control_response();
    test_control_response_queued_during_active_control_runs_next();
    test_control_response_busy_defers_without_spending_attempt();
    test_control_response_retry_exhaustion_is_terminal_and_reaped();
    test_control_response_phy_errors_consume_four_real_attempts();
    test_control_response_pre_rf_phy_block_does_not_spend_attempt();
    test_control_response_terminal_records_do_not_leak_capacity();
    test_reliable_uplink_waits_for_exact_gateway_confirmation();
    test_reliable_uplink_synchronous_and_late_confirmations_are_bounded();
    test_reliable_uplinks_serialize_single_flight_backend();
    test_cancelling_reliable_uplink_releases_backend_owner();
    test_protocol_response_preempts_ordinary_reliable_uplink();
    test_protocol_capacity_is_reserved_and_abandonment_reaps_slots();
    test_fire_and_forget_reliable_uplinks_reap_terminal_capacity();
    test_fire_and_forget_protocol_responses_reap_terminal_capacity();
    test_delivery_rejects_unbounded_or_over_capacity_work();
    test_delivery_queue_survives_stop_and_restart();
    test_inflight_delivery_is_accounted_before_preserving_stop();
    test_retry_backoff_hashes_complete_packet_identity();
    test_durable_uplink_uses_shared_reliable_backend();
    return 0;
}
