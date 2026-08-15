#include "app_node_comm.h"
#include "app_mesh_report.h"
#include "app_radio_guard.h"
#include "dwm3000_driver.h"
#include "survey.h"

#include <zephyr/kernel.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

#define ASSIGNMENT_SEVEN_ANCHOR_PAYLOAD_LEN 184u
#define ASSIGNMENT_EIGHT_ANCHOR_PAYLOAD_LEN 201u
#define ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN 921u

_Static_assert(ASSIGNMENT_SEVEN_ANCHOR_PAYLOAD_LEN <=
                   APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN,
               "seven-anchor assignment must stay in inline custody");
_Static_assert(ASSIGNMENT_EIGHT_ANCHOR_PAYLOAD_LEN > 192u,
               "eight-anchor assignment must exercise large custody");
_Static_assert(ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN <=
                   APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN,
               "fifty-anchor assignment must fit one mesh payload");
_Static_assert(APP_NODE_COMM_MAX_DELIVERIES == 6u,
               "production adapter capacity must remain six slots");

#define APP_DELIVERY_TRACE_CAPACITY 32u

struct app_delivery_trace_entry {
    enum node_comm_delivery_trace_kind kind;
    struct fw_event event;
    struct fw_transition transition;
    struct node_comm_terminal_event terminal;
    bool terminal_present;
};

struct app_delivery_trace_capture {
    struct app_delivery_trace_entry entries[APP_DELIVERY_TRACE_CAPACITY];
    size_t count;
};

static atomic_int_fast64_t fake_now_ms;
static atomic_bool radio_busy;
static atomic_int radio_owner_client;
static atomic_bool anchor_uwb_active;
static atomic_bool anchor_click_active;
static atomic_bool radio_admission_paused;
static atomic_bool rx_response_active;
static uint32_t send_calls;
static struct mesh_outbound last_control_flood;
static uint32_t try_flood_calls;
static int try_flood_results[32];
static bool try_flood_sent[32];
static bool try_flood_wake_train[32];
static struct mesh_outbound try_flood_envelopes[32];
static uint32_t try_response_calls;
static int try_response_results[64];
static bool try_response_sent[64];
static struct mesh_outbound try_response_envelopes[64];
static uint32_t try_uplink_calls;
static int try_uplink_results[64];
static bool try_uplink_sent[64];
static bool try_uplink_confirmed[64];
static uint32_t try_uplink_retry_delays_ms[64];
static uint32_t try_uplink_delivery_generations[64];
static struct mesh_outbound try_uplink_envelopes[64];
static uint32_t cancel_uplink_calls;
static int cancel_uplink_result;
static int cancel_uplink_request_result;
static bool cancel_uplink_complete_immediately;
static bool cancel_uplink_request_pending;
static bool cancel_uplink_completion_ready;
static struct proto_packet last_cancelled_uplink;
static uint8_t
    last_cancel_uplink_semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
static uint32_t last_cancel_uplink_delivery_handle;
static uint32_t last_cancel_uplink_delivery_generation;
static uint32_t last_cancel_uplink_request_token;
static uint32_t queue_calls;
static uint32_t stop_scan_calls;
static uint32_t restart_scan_calls;
static uint32_t receive_abort_calls;
static uint32_t receive_abort_node_comm_calls;
static uint32_t receive_abort_mesh_control_calls;
static uint32_t receive_abort_clear_calls;
static uint32_t last_receive_abort_request_mask;
static uint32_t last_receive_abort_clear_mask;
static uint32_t radio_admission_pause_calls;
static uint32_t radio_admission_resume_calls;
static uint32_t legacy_queue_depth;
static atomic_uint_fast32_t receive_abort_pending;
static atomic_bool transport_paused;
static uint32_t transport_pause_calls;
static uint32_t transport_resume_calls;

static struct mesh_outbound reliable_uplink_envelope(uint16_t seq);
static uint32_t route_refresh_pause_calls;
static uint32_t route_refresh_resume_calls;
static uint32_t watchdog_stop_calls;
static uint32_t reschedule_calls;
static struct k_work_delayable *last_rescheduled_work;
static int last_rescheduled_delay_ms;
static struct k_work_q dedicated_mesh_route_queue;
static uint32_t mesh_route_reschedule_calls;
static int mesh_route_reschedule_result;
static bool gateway_delivery_due_pending;
static bool gateway_scan_active;
static uint32_t gateway_delivery_due_begin_calls;
static uint32_t gateway_delivery_due_end_calls;
static uint32_t gateway_priority_safe_boundary_calls;
static int gateway_priority_safe_boundary_results[8];
static uint32_t scheduling_sequence;
static uint32_t gateway_priority_schedule_order;
static uint32_t mesh_route_schedule_order;
static uint32_t production_delivery_trace_logs;
static uint32_t production_delivery_trace_terminal_logs;
static int production_delivery_trace_format_length;
static char production_delivery_trace_line[128];

static struct mesh_outbound delivery_envelope(uint16_t seq);

static void capture_app_delivery_transition(
    enum node_comm_delivery_trace_kind kind,
    const struct fw_event *event,
    const struct fw_transition *transition,
    const struct node_comm_terminal_event *terminal,
    void *context)
{
    struct app_delivery_trace_capture *capture = context;
    struct app_delivery_trace_entry *entry;

    assert(capture != NULL);
    assert(event != NULL);
    assert(transition != NULL);
    assert(capture->count < APP_DELIVERY_TRACE_CAPACITY);
    entry = &capture->entries[capture->count++];
    entry->kind = kind;
    entry->event = *event;
    entry->transition = *transition;
    entry->terminal_present = terminal != NULL;
    if (terminal != NULL) {
        entry->terminal = *terminal;
    }
}

static pthread_mutex_t interleave_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleave_cond = PTHREAD_COND_INITIALIZER;
static bool block_send;
static bool block_flood_view_copy;
static bool block_uplink_view;
static bool block_backend_after_observation;
static bool backend_observation_ready;
static bool send_entered;
static bool release_send;
static bool block_resume;
static bool resume_entered;
static bool release_resume;

static uint32_t durable_attempt_begin_calls;
static uint32_t durable_attempt_complete_calls;
static int durable_attempt_begin_result;
static int durable_attempt_complete_result;
static uint32_t durable_attempt_complete_failures_remaining;
static uint8_t durable_attempt_next_token;
static struct proto_packet durable_attempt_begin_packets[16];
static struct proto_packet durable_attempt_complete_packets[16];
static uint8_t durable_attempt_complete_tokens[16];
static bool durable_attempt_complete_rf_started[16];
static uint32_t secondary_durable_attempt_begin_calls;
static uint32_t secondary_durable_attempt_complete_calls;
static uint8_t secondary_durable_attempt_token;

static int durable_attempt_begin(const struct proto_packet *packet,
                                 uint8_t *attempt_token)
{
    uint32_t index = durable_attempt_begin_calls++;

    assert(packet != NULL);
    assert(attempt_token != NULL);
    assert(index < sizeof(durable_attempt_begin_packets) /
                       sizeof(durable_attempt_begin_packets[0]));
    durable_attempt_begin_packets[index] = *packet;
    if (durable_attempt_begin_result < 0) {
        return durable_attempt_begin_result;
    }
    durable_attempt_next_token++;
    if (durable_attempt_next_token == 0u) {
        durable_attempt_next_token = 1u;
    }
    *attempt_token = durable_attempt_next_token;
    return 0;
}

static int durable_attempt_complete(const struct proto_packet *packet,
                                    uint8_t attempt_token,
                                    bool rf_started)
{
    uint32_t index = durable_attempt_complete_calls++;

    assert(packet != NULL);
    assert(attempt_token != 0u);
    assert(index < sizeof(durable_attempt_complete_packets) /
                       sizeof(durable_attempt_complete_packets[0]));
    durable_attempt_complete_packets[index] = *packet;
    durable_attempt_complete_tokens[index] = attempt_token;
    durable_attempt_complete_rf_started[index] = rf_started;
    if (durable_attempt_complete_result < 0) {
        return durable_attempt_complete_result;
    }
    if (durable_attempt_complete_failures_remaining > 0u) {
        durable_attempt_complete_failures_remaining--;
        return -EIO;
    }
    return 0;
}

static int secondary_durable_attempt_begin(
    const struct proto_packet *packet,
    uint8_t *attempt_token)
{
    assert(packet != NULL);
    assert(attempt_token != NULL);
    secondary_durable_attempt_begin_calls++;
    secondary_durable_attempt_token++;
    if (secondary_durable_attempt_token == 0u) {
        secondary_durable_attempt_token = 1u;
    }
    *attempt_token = secondary_durable_attempt_token;
    return 0;
}

static int secondary_durable_attempt_complete(
    const struct proto_packet *packet,
    uint8_t attempt_token,
    bool rf_started)
{
    assert(packet != NULL);
    assert(attempt_token == secondary_durable_attempt_token);
    assert(rf_started);
    secondary_durable_attempt_complete_calls++;
    return 0;
}

void status_debug_printf(const char *fmt, ...)
{
    va_list args;
    int formatted;

    if (fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    formatted = vsnprintf(production_delivery_trace_line,
                          sizeof(production_delivery_trace_line),
                          fmt, args);
    va_end(args);
    if (strncmp(fmt, "DBG_NC ", sizeof("DBG_NC ") - 1u) == 0) {
        production_delivery_trace_logs++;
        production_delivery_trace_format_length = formatted;
        if (strstr(production_delivery_trace_line, "t=00000004") != NULL) {
            production_delivery_trace_terminal_logs++;
        }
    }
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

static void block_after_backend_observation_if_requested(void)
{
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_backend_after_observation) {
        backend_observation_ready = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
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

int mesh_route_work_reschedule(struct k_work_delayable *work,
                               uint32_t delay_ms)
{
    int ret;

    mesh_route_reschedule_calls++;
    mesh_route_schedule_order = ++scheduling_sequence;
    if (mesh_route_reschedule_result != 0) {
        return mesh_route_reschedule_result;
    }
    ret = k_work_reschedule_for_queue(&dedicated_mesh_route_queue,
                                      work,
                                      K_MSEC(delay_ms));
    return ret;
}

int mesh_gateway_command_priority_safe_boundary(void)
{
    uint32_t index = gateway_priority_safe_boundary_calls;

    gateway_priority_safe_boundary_calls++;
    gateway_priority_schedule_order = ++scheduling_sequence;
    return index < sizeof(gateway_priority_safe_boundary_results) /
                       sizeof(gateway_priority_safe_boundary_results[0]) ?
           gateway_priority_safe_boundary_results[index] : 0;
}

int mesh_node_comm_gateway_delivery_due_begin(bool *wait_for_scan_boundary)
{
    if (wait_for_scan_boundary == NULL) {
        return -EINVAL;
    }
    gateway_delivery_due_begin_calls++;
    gateway_delivery_due_pending = true;
    *wait_for_scan_boundary = gateway_scan_active;
    if (gateway_scan_active) {
        receive_abort_calls++;
        atomic_store(&receive_abort_pending, true);
    }
    return 0;
}

bool mesh_node_comm_gateway_delivery_due_pending(void)
{
    return gateway_delivery_due_pending;
}

bool mesh_node_comm_gateway_delivery_due_ready(void)
{
    return gateway_delivery_due_pending && !gateway_scan_active;
}

bool mesh_node_comm_gateway_delivery_due_end(void)
{
    bool was_pending = gateway_delivery_due_pending;

    gateway_delivery_due_end_calls++;
    gateway_delivery_due_pending = false;
    return was_pending;
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

enum radio_guard_uwb_client radio_guard_uwb_owner_client(void)
{
    return (enum radio_guard_uwb_client)atomic_load(&radio_owner_client);
}

bool anchor_uwb_window_active(void)
{
    return atomic_load(&anchor_uwb_active);
}

bool anchor_click_window_active(void)
{
    return atomic_load(&anchor_click_active);
}

void radio_guard_uwb_admission_pause(void)
{
    radio_admission_pause_calls++;
    atomic_store(&radio_admission_paused, true);
}

void radio_guard_uwb_admission_resume(void)
{
    radio_admission_resume_calls++;
    atomic_store(&radio_admission_paused, false);
}

bool mesh_rx_response_active(void)
{
    return atomic_load(&rx_response_active);
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    assert(owner_mask == DWM3000_RECEIVE_ABORT_NODE_COMM ||
           owner_mask == DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    receive_abort_calls++;
    last_receive_abort_request_mask = owner_mask;
    if (owner_mask == DWM3000_RECEIVE_ABORT_NODE_COMM) {
        receive_abort_node_comm_calls++;
    } else {
        receive_abort_mesh_control_calls++;
    }
    (void)atomic_fetch_or(&receive_abort_pending, owner_mask);
}

void dwm3000_driver_clear_receive_abort(uint32_t owner_mask)
{
    assert(owner_mask == DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    receive_abort_clear_calls++;
    last_receive_abort_clear_mask = owner_mask;
    (void)atomic_fetch_and(&receive_abort_pending,
                           ~(uint_fast32_t)owner_mask);
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return atomic_load(&receive_abort_pending) != 0u;
}

int mesh_transport_pause_preserving_queued(void)
{
    transport_pause_calls++;
    stop_scan_calls++;
    radio_guard_uwb_admission_pause();
    atomic_store(&transport_paused, true);
    return 0;
}

bool mesh_transport_pause_active(void)
{
    return atomic_load(&transport_paused);
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
    radio_guard_uwb_admission_resume();
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
                                bool send_wake_train,
                                struct app_mesh_tx_observation *observation)
{
    struct mesh_outbound out = {0};
    bool sent_now = false;
    uint32_t index;
    int ret;

    assert(view != NULL);
    assert(view->packet != NULL);
    assert(view->payload_len <= sizeof(out.payload));
    index = try_flood_calls;
    assert(index < sizeof(try_flood_wake_train) /
                       sizeof(try_flood_wake_train[0]));
    try_flood_wake_train[index] = send_wake_train;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_flood_view_copy) {
        send_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    out.packet = *view->packet;
    memcpy(out.payload, view->payload, view->payload_len);
    out.payload_len = view->payload_len;
    out.radio_channel = view->radio_channel;
    out.next_hop_id = view->next_hop_id;
    out.queued_at_ms = view->queued_at_ms;
    out.earliest_tx_ms = view->earliest_tx_ms;
    out.flood_retry_count = view->flood_retry_count;
    out.queued_at_valid = view->queued_at_valid;
    out.earliest_tx_valid = view->earliest_tx_valid;
    ret = mesh_try_send_c5_flood(&out, purpose, reason, &sent_now);
    if (observation != NULL) {
        const uint64_t now_ms = (uint64_t)atomic_load(&fake_now_ms);

        memset(observation, 0, sizeof(*observation));
        observation->rf_started = sent_now;
        observation->rf_started_at_ms = now_ms;
        observation->tx_completed = sent_now && ret == 0;
        observation->tx_completed_at_ms = now_ms;
        observation->result_at_ms = now_ms;
    }
    block_after_backend_observation_if_requested();
    return ret;
}

int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation)
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
    out->queued_at_valid = view->queued_at_valid;
    out->earliest_tx_valid = view->earliest_tx_valid;
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
    if (observation != NULL) {
        const uint64_t now_ms = (uint64_t)atomic_load(&fake_now_ms);

        memset(observation, 0, sizeof(*observation));
        observation->rf_started = try_response_sent[index];
        observation->rf_started_at_ms = now_ms;
        observation->tx_completed =
            try_response_sent[index] && try_response_results[index] == 0;
        observation->tx_completed_at_ms = now_ms;
        observation->result_at_ms = now_ms;
    }
    block_after_backend_observation_if_requested();
    return try_response_results[index];
}

int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation,
    uint32_t *scheduled_retry_delay_ms)
{
    struct mesh_outbound *out;
    uint32_t index = try_uplink_calls++;

    (void)reason;
    assert(index < sizeof(try_uplink_results) /
                       sizeof(try_uplink_results[0]));
    assert(view != NULL);
    assert(view->packet != NULL);
    assert(view->delivery_generation != 0u);
    try_uplink_delivery_generations[index] = view->delivery_generation;
    out = &try_uplink_envelopes[index];
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    memcpy(out->payload, view->payload, view->payload_len);
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
    out->queued_at_valid = view->queued_at_valid;
    out->earliest_tx_valid = view->earliest_tx_valid;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    if (block_uplink_view) {
        send_entered = true;
        assert(pthread_cond_broadcast(&interleave_cond) == 0);
        while (!release_send) {
            assert(pthread_cond_wait(&interleave_cond,
                                     &interleave_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    if (observation != NULL) {
        const uint64_t now_ms = (uint64_t)atomic_load(&fake_now_ms);

        memset(observation, 0, sizeof(*observation));
        observation->rf_started = try_uplink_sent[index];
        observation->rf_started_at_ms = now_ms;
        observation->tx_completed =
            try_uplink_sent[index] && try_uplink_results[index] == 0;
        observation->tx_completed_at_ms = now_ms;
        observation->gateway_confirmed = try_uplink_confirmed[index];
        observation->gateway_confirmed_at_ms = now_ms;
        observation->result_at_ms = now_ms;
    }
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = try_uplink_retry_delays_ms[index];
    }
    block_after_backend_observation_if_requested();
    return try_uplink_results[index];
}

int mesh_request_reliable_uplink_cancel(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t delivery_handle,
    uint32_t delivery_generation,
    uint32_t request_token)
{
    assert(packet != NULL);
    assert(semantic_digest != NULL);
    assert(delivery_handle != 0u);
    assert(delivery_generation != 0u);
    assert(request_token != 0u);
    cancel_uplink_calls++;
    last_cancelled_uplink = *packet;
    memcpy(last_cancel_uplink_semantic_digest,
           semantic_digest,
           sizeof(last_cancel_uplink_semantic_digest));
    last_cancel_uplink_delivery_handle = delivery_handle;
    last_cancel_uplink_delivery_generation = delivery_generation;
    last_cancel_uplink_request_token = request_token;
    if (cancel_uplink_request_result < 0) {
        return cancel_uplink_request_result;
    }
    cancel_uplink_request_pending = true;
    cancel_uplink_completion_ready = cancel_uplink_complete_immediately;
    return 0;
}

int mesh_take_reliable_uplink_cancel_result(uint32_t delivery_handle,
                                            uint32_t request_token,
                                            int *cancel_result)
{
    assert(cancel_result != NULL);
    if (!cancel_uplink_request_pending ||
        delivery_handle != last_cancel_uplink_delivery_handle ||
        request_token != last_cancel_uplink_request_token) {
        return -ENOENT;
    }
    if (!cancel_uplink_completion_ready) {
        return 0;
    }
    *cancel_result = cancel_uplink_result;
    cancel_uplink_request_pending = false;
    cancel_uplink_completion_ready = false;
    return 1;
}

int mesh_schedule_route_request(uint64_t target_id, const char *reason)
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
    const struct app_node_comm_durable_attempt_ops durable_ops = {
        .begin = durable_attempt_begin,
        .complete = durable_attempt_complete,
    };

    atomic_store(&fake_now_ms, 0);
    atomic_store(&radio_busy, false);
    atomic_store(&radio_owner_client, RADIO_GUARD_UWB_CLIENT_NONE);
    atomic_store(&anchor_uwb_active, false);
    atomic_store(&anchor_click_active, false);
    atomic_store(&radio_admission_paused, false);
    atomic_store(&rx_response_active, false);
    send_calls = 0u;
    try_flood_calls = 0u;
    memset(try_flood_results, 0, sizeof(try_flood_results));
    memset(try_flood_sent, 1, sizeof(try_flood_sent));
    memset(try_flood_wake_train, 0, sizeof(try_flood_wake_train));
    memset(try_flood_envelopes, 0, sizeof(try_flood_envelopes));
    try_response_calls = 0u;
    memset(try_response_results, 0, sizeof(try_response_results));
    memset(try_response_sent, 1, sizeof(try_response_sent));
    memset(try_response_envelopes, 0, sizeof(try_response_envelopes));
    try_uplink_calls = 0u;
    memset(try_uplink_results, 0, sizeof(try_uplink_results));
    memset(try_uplink_sent, 1, sizeof(try_uplink_sent));
    memset(try_uplink_confirmed, 0, sizeof(try_uplink_confirmed));
    memset(try_uplink_retry_delays_ms, 0,
           sizeof(try_uplink_retry_delays_ms));
    memset(try_uplink_delivery_generations, 0,
           sizeof(try_uplink_delivery_generations));
    memset(try_uplink_envelopes, 0, sizeof(try_uplink_envelopes));
    cancel_uplink_calls = 0u;
    cancel_uplink_result = 0;
    cancel_uplink_request_result = 0;
    cancel_uplink_complete_immediately = true;
    cancel_uplink_request_pending = false;
    cancel_uplink_completion_ready = false;
    memset(&last_cancelled_uplink, 0, sizeof(last_cancelled_uplink));
    memset(last_cancel_uplink_semantic_digest, 0,
           sizeof(last_cancel_uplink_semantic_digest));
    last_cancel_uplink_delivery_handle = 0u;
    last_cancel_uplink_delivery_generation = 0u;
    last_cancel_uplink_request_token = 0u;
    queue_calls = 0u;
    stop_scan_calls = 0u;
    restart_scan_calls = 0u;
    receive_abort_calls = 0u;
    receive_abort_node_comm_calls = 0u;
    receive_abort_mesh_control_calls = 0u;
    receive_abort_clear_calls = 0u;
    last_receive_abort_request_mask = 0u;
    last_receive_abort_clear_mask = 0u;
    radio_admission_pause_calls = 0u;
    radio_admission_resume_calls = 0u;
    atomic_store(&receive_abort_pending, 0u);
    atomic_store(&transport_paused, false);
    transport_pause_calls = 0u;
    transport_resume_calls = 0u;
    route_refresh_pause_calls = 0u;
    route_refresh_resume_calls = 0u;
    watchdog_stop_calls = 0u;
    reschedule_calls = 0u;
    last_rescheduled_work = NULL;
    last_rescheduled_delay_ms = -1;
    memset(&dedicated_mesh_route_queue, 0,
           sizeof(dedicated_mesh_route_queue));
    mesh_route_reschedule_calls = 0u;
    mesh_route_reschedule_result = 0;
    gateway_delivery_due_pending = false;
    gateway_scan_active = false;
    gateway_delivery_due_begin_calls = 0u;
    gateway_delivery_due_end_calls = 0u;
    gateway_priority_safe_boundary_calls = 0u;
    memset(gateway_priority_safe_boundary_results, 0,
           sizeof(gateway_priority_safe_boundary_results));
    scheduling_sequence = 0u;
    gateway_priority_schedule_order = 0u;
    mesh_route_schedule_order = 0u;
    production_delivery_trace_logs = 0u;
    production_delivery_trace_terminal_logs = 0u;
    production_delivery_trace_format_length = -1;
    memset(production_delivery_trace_line, 0,
           sizeof(production_delivery_trace_line));
    legacy_queue_depth = 3u;
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_send = false;
    block_flood_view_copy = false;
    block_uplink_view = false;
    block_backend_after_observation = false;
    backend_observation_ready = false;
    send_entered = false;
    release_send = false;
    block_resume = false;
    resume_entered = false;
    release_resume = false;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    durable_attempt_begin_calls = 0u;
    durable_attempt_complete_calls = 0u;
    durable_attempt_begin_result = 0;
    durable_attempt_complete_result = 0;
    durable_attempt_complete_failures_remaining = 0u;
    durable_attempt_next_token = 0u;
    memset(durable_attempt_begin_packets, 0,
           sizeof(durable_attempt_begin_packets));
    memset(durable_attempt_complete_packets, 0,
           sizeof(durable_attempt_complete_packets));
    memset(durable_attempt_complete_tokens, 0,
           sizeof(durable_attempt_complete_tokens));
    memset(durable_attempt_complete_rf_started, 0,
           sizeof(durable_attempt_complete_rf_started));
    secondary_durable_attempt_begin_calls = 0u;
    secondary_durable_attempt_complete_calls = 0u;
    secondary_durable_attempt_token = 0u;
    assert(app_node_comm_init(NULL) == 0);
    assert(app_node_comm_register_durable_attempt_ops(&durable_ops) == 0);
    assert(app_node_comm_policy_running());
}

static void test_production_init_installs_delivery_transition_trace(void)
{
    struct mesh_outbound envelope = delivery_envelope(1u);
    uint32_t handle = 0u;

    reset_fixture();
    assert(production_delivery_trace_logs == 0u);
    assert(app_node_comm_submit_delivery(
               &envelope, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1000u, 1u, &handle) == 0);
    assert(handle != 0u);
    /* Ownership, a terminal edge, and immutable terminal evidence use this sink. */
    assert(production_delivery_trace_logs > 0u);
    assert(app_node_comm_cancel_delivery(handle) == 0);
    assert(production_delivery_trace_logs > 1u);
    assert(production_delivery_trace_terminal_logs == 1u);
    assert(production_delivery_trace_format_length > 0);
    assert((size_t)production_delivery_trace_format_length <
           sizeof(production_delivery_trace_line));
    assert(strstr(production_delivery_trace_line, "o=") != NULL);
    assert(strstr(production_delivery_trace_line, "g=") != NULL);
    assert(strstr(production_delivery_trace_line, "q=") != NULL);
    assert(strstr(production_delivery_trace_line, "f=") != NULL);
    assert(strstr(production_delivery_trace_line, "t=00000004") != NULL);
    assert(strstr(production_delivery_trace_line, "p=00000000") != NULL);
}

static void test_survey_transport_preempt_preserves_custody_and_exact_abort_owner(void)
{
    uint32_t original_queue_depth;

    reset_fixture();
    original_queue_depth = legacy_queue_depth;
    assert(app_node_comm_transport_preempt_ready() == -EACCES);
    assert(app_node_comm_transport_preempt_begin() == 0);
    assert(transport_pause_calls == 1u);
    assert(atomic_load(&transport_paused));
    assert(atomic_load(&radio_admission_paused));
    assert(legacy_queue_depth == original_queue_depth);

    /* Re-entering the same survey-owned pause is idempotent. */
    assert(app_node_comm_transport_preempt_begin() == 0);
    assert(transport_pause_calls == 1u);

    /* Click and anchor-ranging owners are never targets for the mesh-control
     * abort edge, even while they keep transport short of quiescence. */
    atomic_store(&radio_busy, true);
    atomic_store(&radio_owner_client,
                 RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK);
    atomic_store(&anchor_click_active, true);
    assert(app_node_comm_transport_preempt_ready() == -EBUSY);
    assert(receive_abort_calls == 0u);

    atomic_store(&anchor_click_active, false);
    atomic_store(&radio_owner_client, RADIO_GUARD_UWB_CLIENT_MESH_RX);
    atomic_store(&anchor_uwb_active, true);
    assert(app_node_comm_transport_preempt_ready() == -EBUSY);
    assert(receive_abort_calls == 0u);

    atomic_store(&anchor_uwb_active, false);
    assert(app_node_comm_transport_preempt_ready() == -EBUSY);
    assert(receive_abort_calls == 1u);
    assert(receive_abort_mesh_control_calls == 1u);
    assert(receive_abort_node_comm_calls == 0u);
    assert(last_receive_abort_request_mask ==
           DWM3000_RECEIVE_ABORT_MESH_CONTROL);

    /* At the safe boundary, consume only the survey-owned edge. An unrelated
     * lifecycle abort remains authoritative and keeps admission closed. */
    atomic_store(&radio_busy, false);
    atomic_store(&radio_owner_client, RADIO_GUARD_UWB_CLIENT_NONE);
    (void)atomic_fetch_or(&receive_abort_pending,
                          DWM3000_RECEIVE_ABORT_NODE_COMM);
    assert(app_node_comm_transport_preempt_ready() == -EBUSY);
    assert(receive_abort_clear_calls == 1u);
    assert(last_receive_abort_clear_mask ==
           DWM3000_RECEIVE_ABORT_MESH_CONTROL);
    assert(atomic_load(&receive_abort_pending) ==
           DWM3000_RECEIVE_ABORT_NODE_COMM);
    assert(radio_admission_resume_calls == 0u);

    atomic_store(&receive_abort_pending, 0u);
    assert(app_node_comm_transport_preempt_ready() == 0);
    assert(radio_admission_resume_calls == 1u);
    assert(!atomic_load(&radio_admission_paused));
    assert(atomic_load(&transport_paused));
    assert(legacy_queue_depth == original_queue_depth);

    app_node_comm_transport_preempt_end();
    assert(transport_resume_calls == 1u);
    assert(!atomic_load(&transport_paused));
    assert(!atomic_load(&radio_admission_paused));
    assert(legacy_queue_depth == original_queue_depth);
}

static void test_survey_transport_preempt_cannot_steal_lifecycle_pause(void)
{
    struct node_comm_pause_lease pause_lease;

    reset_fixture();
    atomic_store(&transport_paused, true);
    atomic_store(&radio_admission_paused, true);
    assert(app_node_comm_transport_preempt_begin() == -ESHUTDOWN);
    assert(transport_pause_calls == 0u);
    app_node_comm_transport_preempt_end();
    assert(transport_resume_calls == 0u);
    assert(atomic_load(&transport_paused));

    reset_fixture();
    assert(app_node_comm_transport_preempt_begin() == 0);
    assert(app_node_comm_transport_preempt_ready() == 0);
    assert(app_node_comm_pause_request(91u, 1000u, &pause_lease) == 0);
    assert(!app_node_comm_policy_running());
    app_node_comm_transport_preempt_end();
    assert(transport_resume_calls == 0u);
    assert(atomic_load(&transport_paused));
    assert(atomic_load(&radio_admission_paused));
}

static void test_survey_transport_preempt_accepts_atomic_result_reservation(void)
{
    struct app_node_comm_reservation_lease reservations[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    uint32_t handles[APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    const size_t count = sizeof(reservations) / sizeof(reservations[0]);
    uint32_t schedules_before_resume;

    reset_fixture();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        /* The hardware failure was on the anchor's dedicated mesh queue. */
        return;
    }
    assert(app_node_comm_transport_preempt_begin() == 0);
    assert(app_node_comm_transport_preempt_ready() == 0);
    mesh_route_reschedule_result = -ESHUTDOWN;

    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0x5a5au, count, reservations, count) == 0);
    assert(watchdog_stop_calls == 0u);
    for (size_t i = 0u; i < count; i++) {
        struct mesh_outbound sample =
            reliable_uplink_envelope((uint16_t)(230u + i));

        assert(reservations[i].token != 0u);
        assert(reservations[i].owner_generation == 0x5a5au);
        sample.packet.msg_type = MSG_SURVEY_PAIR_RESULT;
        assert(app_node_comm_commit_durable_reliable_uplink_reservation(
                   &reservations[i], &sample, 10000u,
                   (uint32_t)(0x5300u + i), &handles[i]) == 0);
        assert(handles[i] != 0u);
        memset(&reservations[i], 0, sizeof(reservations[i]));
    }
    assert(app_node_comm_pending_delivery_count() == count);

    schedules_before_resume = mesh_route_reschedule_calls;
    mesh_route_reschedule_result = 0;
    app_node_comm_transport_preempt_end();
    assert(mesh_route_reschedule_calls == schedules_before_resume + 1u);
    for (size_t i = 0u; i < count; i++) {
        assert(app_node_comm_abandon_delivery(handles[i]) == 0);
    }
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

static void test_pause_watchdog_schedule_failure_stops_feeding(void)
{
    struct node_comm_pause_lease first_lease;
    struct node_comm_pause_lease failed_owner_lease;
    struct k_work_delayable *watchdog_work;

    reset_fixture();
    assert(app_node_comm_pause_request(30u, 10u, &first_lease) == 0);
    watchdog_work = last_rescheduled_work;
    assert(watchdog_work != NULL);
    assert(app_node_comm_pause_note_quiesced(&first_lease) == 0);
    assert(app_node_comm_resume_begin(&first_lease) == 0);
    assert(app_node_comm_resume_complete(&first_lease) == 0);

    watchdog_work->reschedule_result = -EIO;
    assert(app_node_comm_pause_request(
        31u, 10u, &failed_owner_lease) == 0);
    assert(watchdog_stop_calls == 1u);
    assert(!app_node_comm_policy_running());
    assert(atomic_load(&transport_paused));
}

static void test_recovery_watchdog_retry_failure_stops_feeding(void)
{
    struct node_comm_pause_lease caller_lease;
    struct node_comm_pause_lease recovery_lease;
    struct k_work_delayable *watchdog_work;

    reset_fixture();
    assert(app_node_comm_pause_request(32u, 10u, &caller_lease) == 0);
    watchdog_work = last_rescheduled_work;
    assert(watchdog_work != NULL);
    atomic_store(&radio_busy, true);
    atomic_store(&rx_response_active, true);
    watchdog_work->reschedule_result = -EIO;
    atomic_store(&fake_now_ms, 10);
    watchdog_work->work.handler(&watchdog_work->work);

    assert(app_node_comm_forced_reclaim_lease(&recovery_lease));
    assert(recovery_lease.generation != caller_lease.generation);
    assert(watchdog_stop_calls >= 1u);
    assert(!app_node_comm_policy_running());
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

struct adapter_cancel_thread_request {
    struct adapter_thread_result completion;
    uint32_t handle;
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

static void *adapter_cancel_delivery_thread(void *arg)
{
    struct adapter_cancel_thread_request *request = arg;

    adapter_thread_complete(&request->completion,
                            app_node_comm_cancel_delivery(request->handle));
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
    envelope.queued_at_valid = true;
    envelope.earliest_tx_ms = 0u;
    envelope.flood_retry_count = 0u;
    return envelope;
}

static struct mesh_outbound assignment_sized_control_envelope(
    uint16_t seq,
    uint16_t payload_len)
{
    struct mesh_outbound envelope = delivery_envelope(seq);

    assert(payload_len <= sizeof(envelope.payload));
    envelope.packet.session_id ^= payload_len;
    envelope.packet.payload_len = payload_len;
    envelope.payload_len = payload_len;
    for (uint16_t i = 0u; i < payload_len; i++) {
        envelope.payload[i] =
            (uint8_t)(i ^ (uint16_t)(i * 29u) ^ seq ^ payload_len);
    }
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
    envelope.queued_at_valid = true;
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

static int note_gateway_confirmed_envelope_at(
    const struct mesh_outbound *envelope,
    uint64_t confirmed_at_ms)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    assert(envelope != NULL);
    assert(mesh_packet_semantic_digest(&envelope->packet,
                                       envelope->payload,
                                       envelope->payload_len,
                                       semantic_digest));
    return app_node_comm_note_gateway_confirmed_digest_at(
        &envelope->packet, semantic_digest, confirmed_at_ms);
}

static int note_gateway_confirmed_envelope(
    const struct mesh_outbound *envelope)
{
    return note_gateway_confirmed_envelope_at(
        envelope, (uint64_t)atomic_load(&fake_now_ms));
}

static int note_gateway_failed_envelope(
    const struct mesh_outbound *envelope,
    enum node_comm_terminal_reason reason)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    assert(envelope != NULL);
    assert(mesh_packet_semantic_digest(&envelope->packet,
                                       envelope->payload,
                                       envelope->payload_len,
                                       semantic_digest));
    return app_node_comm_note_gateway_failed_digest(
        &envelope->packet, semantic_digest, reason);
}

static struct mesh_outbound survey_report_envelope(uint16_t seq)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(seq);

    envelope.packet.msg_type = MSG_SURVEY_DISCOVERY_REPORT;
    envelope.packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    envelope.packet.src_id = UINT64_C(0xa002000000010011);
    envelope.packet.dst_id = UINT64_C(0x9999888877776666);
    envelope.packet.session_id = UINT32_C(0x50665006);
    envelope.packet.ttl = 6u;
    envelope.packet.payload_len = 96u;
    for (size_t i = 0u; i < envelope.packet.payload_len; i++) {
        envelope.payload[i] = (uint8_t)(i ^ (i * 29u) ^ seq);
    }
    envelope.payload_len = envelope.packet.payload_len;
    envelope.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    envelope.next_hop_id = envelope.packet.dst_id;
    envelope.queued_at_ms = 37u;
    envelope.queued_at_valid = true;
    envelope.earliest_tx_ms = 411u;
    envelope.earliest_tx_valid = true;
    return envelope;
}

static void test_peek_attempt_count_does_not_service_delivery_policy(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(59u);
    uint32_t handle = 0u;
    uint8_t attempts_started = UINT8_MAX;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_RELIABLE_UPLINK,
               1000u,
               590u,
               &handle) == 0);

    assert(app_node_comm_peek_delivery_attempts_started(
               handle, &attempts_started) == 0);
    assert(attempts_started == 0u);
    assert(try_uplink_calls == 0u);
    assert(durable_attempt_begin_calls == 0u);
    assert(durable_attempt_complete_calls == 0u);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
}

static void test_control_flood_freezes_age_before_wake_work(void)
{
    struct mesh_outbound envelope = delivery_envelope(9u);
    bool sent_now = false;

    reset_fixture();
    envelope.queued_at_ms = 0u;
    envelope.queued_at_valid = false;
    atomic_store(&fake_now_ms, 1234);
    assert(app_node_comm_send_control_flood(
        &envelope, 1u, "age-origin", &sent_now) == 19);
    assert(sent_now);
    assert(last_control_flood.queued_at_ms == 1234u);
    assert(last_control_flood.queued_at_valid);
    assert(envelope.queued_at_ms == 0u);
    assert(!envelope.queued_at_valid);

    envelope.queued_at_ms = 777u;
    envelope.queued_at_valid = true;
    atomic_store(&fake_now_ms, 4321);
    assert(app_node_comm_send_control_flood(
        &envelope, 1u, "preserve-origin", &sent_now) == 19);
    assert(last_control_flood.queued_at_ms == 777u);
    assert(last_control_flood.queued_at_valid);

    envelope.queued_at_ms = 99u;
    envelope.queued_at_valid = false;
    atomic_store(&fake_now_ms, 0);
    assert(app_node_comm_send_control_flood(
        &envelope, 1u, "zero-is-real-origin", &sent_now) == 19);
    assert(last_control_flood.queued_at_ms == 0u);
    assert(last_control_flood.queued_at_valid);
}

static void test_delivery_copies_envelope_and_wakes_once_per_flood(void)
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
            struct k_work_delayable *scheduled_work;

            assert(last_rescheduled_work != NULL);
            assert(last_rescheduled_delay_ms == 0);
            scheduled_work = last_rescheduled_work;
            scheduled_work->work.handler(&scheduled_work->work);
            if (DEVICE_ROLE == ROLE_GATEWAY) {
                assert(gateway_delivery_due_pending);
                assert(last_rescheduled_work != scheduled_work);
                scheduled_work = last_rescheduled_work;
                assert(scheduled_work->last_queue ==
                       &dedicated_mesh_route_queue);
                scheduled_work->work.handler(&scheduled_work->work);
            }
        } else {
            assert(app_node_comm_service_deliveries() == 0);
        }
        assert(try_flood_calls == attempt + 1u);
        assert(memcmp(&try_flood_envelopes[attempt],
                      &expected,
                      sizeof(expected)) == 0);
        assert(try_flood_wake_train[attempt] == (attempt == 0u));
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

static void test_assignment_sized_control_payload_admission(void)
{
    struct mesh_outbound inline_assignment =
        assignment_sized_control_envelope(
            70u, ASSIGNMENT_SEVEN_ANCHOR_PAYLOAD_LEN);
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &inline_assignment,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        700u,
        &handle) == 0);
    assert(app_node_comm_abandon_delivery(handle) == 0);

    if (DEVICE_ROLE == ROLE_GATEWAY) {
        struct mesh_outbound eight_anchor_assignment =
            assignment_sized_control_envelope(
                71u, ASSIGNMENT_EIGHT_ANCHOR_PAYLOAD_LEN);
        struct mesh_outbound fifty_anchor_assignment =
            assignment_sized_control_envelope(
                72u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);

        assert(app_node_comm_submit_delivery(
            &eight_anchor_assignment,
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            10000u,
            701u,
            &handle) == 0);
        assert(app_node_comm_abandon_delivery(handle) == 0);
        assert(app_node_comm_submit_delivery(
            &fifty_anchor_assignment,
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            10000u,
            702u,
            &handle) == 0);
        assert(app_node_comm_abandon_delivery(handle) == 0);
    }
}

static void assert_same_frozen_control(
    const struct mesh_outbound *actual,
    const struct mesh_outbound *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->packet.msg_type == expected->packet.msg_type);
    assert(actual->packet.src_id == expected->packet.src_id);
    assert(actual->packet.dst_id == expected->packet.dst_id);
    assert(actual->packet.session_id == expected->packet.session_id);
    assert(actual->packet.seq == expected->packet.seq);
    assert(actual->packet.payload_len == expected->packet.payload_len);
    assert(actual->payload_len == expected->payload_len);
    assert(actual->radio_channel == expected->radio_channel);
    assert(actual->next_hop_id == expected->next_hop_id);
    assert(actual->queued_at_ms == expected->queued_at_ms);
    assert(actual->earliest_tx_ms == expected->earliest_tx_ms);
    assert(actual->queued_at_valid == expected->queued_at_valid);
    assert(actual->earliest_tx_valid == expected->earliest_tx_valid);
    assert(actual->flood_retry_count == expected->flood_retry_count);
    assert(memcmp(actual->payload,
                  expected->payload,
                  expected->payload_len) == 0);
}

static void test_gateway_large_control_retries_exact_fifty_anchor_payload(void)
{
    struct mesh_outbound envelope = assignment_sized_control_envelope(
        73u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);
    const struct mesh_outbound frozen = envelope;
    struct mesh_outbound replacement = assignment_sized_control_envelope(
        74u, ASSIGNMENT_EIGHT_ANCHOR_PAYLOAD_LEN);
    struct node_comm_terminal_event event;
    uint32_t retry_delay_ms;
    uint32_t handle;
    uint32_t replacement_handle;

    reset_fixture();
    try_flood_results[0] = -ETIMEDOUT;
    try_flood_sent[0] = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        20000u,
        703u,
        &handle) == 0);

    envelope.packet.session_id++;
    envelope.packet.seq++;
    memset(envelope.payload, 0xee, envelope.payload_len);

    assert(app_node_comm_service_deliveries() == -ETIMEDOUT);
    assert(app_node_comm_retry_backoff_ms(
        &frozen,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1u,
        &retry_delay_ms) == 0);
    atomic_store(&fake_now_ms, retry_delay_ms);
    assert(app_node_comm_service_deliveries() == 0);
    atomic_store(&fake_now_ms, retry_delay_ms + 40u);
    assert(app_node_comm_service_deliveries() == 0);
    atomic_store(&fake_now_ms, retry_delay_ms + 80u);
    assert(app_node_comm_service_deliveries() == 0);

    assert(try_flood_calls == 4u);
    for (uint32_t attempt = 0u; attempt < try_flood_calls; attempt++) {
        assert_same_frozen_control(&try_flood_envelopes[attempt], &frozen);
    }
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 4u);

    assert(app_node_comm_submit_delivery(
        &replacement,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        20000u,
        704u,
        &replacement_handle) == 0);
    assert(app_node_comm_abandon_delivery(replacement_handle) == 0);
}

static void test_gateway_large_control_single_owner_and_cancel_release(void)
{
    struct mesh_outbound first = assignment_sized_control_envelope(
        75u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);
    struct mesh_outbound second = assignment_sized_control_envelope(
        76u, ASSIGNMENT_EIGHT_ANCHOR_PAYLOAD_LEN);
    struct mesh_outbound third = assignment_sized_control_envelope(
        77u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);
    struct node_comm_terminal_event event;
    uint32_t first_handle;
    uint32_t second_handle;
    uint32_t third_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        705u,
        &first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        706u,
        &second_handle) == -ENOSPC);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_flood_calls == 1u);
    assert_same_frozen_control(&try_flood_envelopes[0], &first);

    assert(app_node_comm_cancel_delivery(first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        706u,
        &second_handle) == -ENOSPC);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);

    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        706u,
        &second_handle) == 0);
    assert(app_node_comm_abandon_delivery(second_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &third,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        707u,
        &third_handle) == 0);
    assert(app_node_comm_abandon_delivery(third_handle) == 0);
}

static void test_gateway_large_control_owner_survives_active_backend_cancel(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound first = assignment_sized_control_envelope(
        78u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);
    const struct mesh_outbound frozen = first;
    struct mesh_outbound second = assignment_sized_control_envelope(
        79u, ASSIGNMENT_FIFTY_ANCHOR_PAYLOAD_LEN);
    pthread_t service_thread;
    uint32_t first_handle = 0u;
    uint32_t second_handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        708u,
        &first_handle) == 0);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_flood_view_copy = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(app_node_comm_abandon_delivery(first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        709u,
        &second_handle) == -ENOSPC);
    assert(second_handle == 0u);

    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(try_flood_calls == 1u);
    assert_same_frozen_control(&try_flood_envelopes[0], &frozen);

    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        10000u,
        709u,
        &second_handle) == 0);
    assert(app_node_comm_abandon_delivery(second_handle) == 0);
}

static void test_delivery_schedule_uses_role_queue_path(void)
{
    struct mesh_outbound envelope = delivery_envelope(11u);
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        78u,
        &handle) == 0);
    assert(handle != 0u);
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        assert(mesh_route_reschedule_calls == 0u);
        assert(last_rescheduled_work != NULL);
        assert(last_rescheduled_work->last_queue == NULL);
        assert(last_rescheduled_delay_ms == 0);
        {
            struct k_work_delayable *due_kick = last_rescheduled_work;

            due_kick->work.handler(&due_kick->work);
        }
        assert(gateway_delivery_due_begin_calls == 1u);
        assert(gateway_priority_safe_boundary_calls == 1u);
        assert(mesh_route_reschedule_calls == 1u);
        assert(gateway_priority_schedule_order < mesh_route_schedule_order);
    } else {
        assert(mesh_route_reschedule_calls == 1u);
    }
    assert(last_rescheduled_work != NULL);
    assert(last_rescheduled_work->last_queue == &dedicated_mesh_route_queue);
    assert(last_rescheduled_delay_ms == 0);
}

static void
test_initial_delivery_schedule_failure_rolls_back_before_acceptance(void)
{
    struct mesh_outbound first = delivery_envelope(170u);
    struct mesh_outbound rejected = delivery_envelope(171u);
    struct k_work_delayable *due_kick = NULL;
    uint32_t first_handle = 0u;
    uint32_t rejected_handle = 0u;

    reset_fixture();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        assert(app_node_comm_submit_delivery(
            &first,
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            1000u,
            1700u,
            &first_handle) == 0);
        due_kick = last_rescheduled_work;
        assert(due_kick != NULL);
        assert(app_node_comm_abandon_delivery(first_handle) == 0);
        due_kick->reschedule_result = -EIO;
    } else {
        mesh_route_reschedule_result = -EIO;
    }

    assert(app_node_comm_submit_delivery(
        &rejected,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        1710u,
        &rejected_handle) == -EIO);
    assert(rejected_handle == 0u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(watchdog_stop_calls == 0u);
}

static void test_positive_work_schedule_result_is_normalized_to_acceptance(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(172u);
    struct mesh_outbound response = reliable_uplink_envelope(173u);
    struct mesh_outbound committed = reliable_uplink_envelope(174u);
    struct k_work_delayable *gateway_due_kick = NULL;
    struct app_node_comm_reservation_lease reservation = {0};
    uint32_t first_handle = 0u;
    uint32_t handle = 0u;

    reset_fixture();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        assert(app_node_comm_submit_protocol_response(
                   &first, 1000u, 1720u, &first_handle) == 0);
        gateway_due_kick = last_rescheduled_work;
        assert(gateway_due_kick != NULL);
        assert(app_node_comm_abandon_delivery(first_handle) == 0);
        gateway_due_kick->reschedule_result = 1;
    } else {
        mesh_route_reschedule_result = 1;
    }
    assert(app_node_comm_submit_protocol_response(
               &response, 1000u, 1730u, &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_abandon_delivery(handle) == 0);

    assert(app_node_comm_reserve_protocol_response(1740u, &reservation) == 0);
    assert(reservation.token != 0u);
    handle = 0u;
    assert(app_node_comm_commit_protocol_response(
               &reservation,
               &committed,
               1000u,
               1740u,
               &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_abandon_delivery(handle) == 0);
}

static void
test_durable_completion_retry_does_not_delay_earlier_policy_work(void)
{
    struct mesh_outbound durable = reliable_uplink_envelope(177u);
    struct mesh_outbound priority = delivery_envelope(178u);
    uint32_t durable_handle = 0u;
    uint32_t priority_handle = 0u;

    reset_fixture();
    durable_attempt_complete_failures_remaining = 1u;
    try_uplink_results[0] = -EBUSY;
    try_uplink_sent[0] = false;
    assert(app_node_comm_submit_delivery(
        &durable,
        NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
        1000u,
        1770u,
        &durable_handle) == 0);
    assert(app_node_comm_service_deliveries() == -EIO);
    assert(last_rescheduled_delay_ms == 10);

    /*
     * The durable refund retry is due at t+10, but a newly accepted priority
     * request is runnable now. The scheduler must choose the earlier owner.
     */
    assert(app_node_comm_submit_delivery(
        &priority,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        1780u,
        &priority_handle) == 0);
    assert(last_rescheduled_delay_ms == 0);
}

static void test_gateway_due_kick_aborts_active_scan_at_safe_boundary(void)
{
    struct mesh_outbound envelope = delivery_envelope(12u);
    struct k_work_delayable *due_kick;
    struct k_work_delayable *delivery_work;
    uint32_t handle;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        79u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    assert(due_kick != NULL);
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(receive_abort_calls == 1u);
    assert(mesh_route_reschedule_calls == 0u);

    /* RX owns the boundary; host priority is scheduled before node_comm. */
    gateway_scan_active = false;
    assert(app_node_comm_gateway_delivery_safe_boundary() == 0);
    assert(mesh_route_reschedule_calls == 1u);
    assert(gateway_priority_schedule_order < mesh_route_schedule_order);
    delivery_work = last_rescheduled_work;
    assert(delivery_work != due_kick);
    delivery_work->work.handler(&delivery_work->work);
    assert(try_flood_calls == 1u);
    assert(!gateway_delivery_due_pending);
    assert(restart_scan_calls == 1u);
}

static void
test_gateway_scan_boundary_retry_failure_retains_gate_and_fails_closed(void)
{
    struct mesh_outbound envelope = delivery_envelope(172u);
    struct k_work_delayable *due_kick;
    uint32_t handle = 0u;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        1720u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    assert(due_kick != NULL);
    due_kick->reschedule_result = -EIO;

    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(receive_abort_calls == 1u);
    assert(mesh_route_reschedule_calls == 0u);
    assert(restart_scan_calls == 0u);
    assert(watchdog_stop_calls == 1u);
    assert(app_node_comm_pending_delivery_count() == 1u);
}

static void test_gateway_due_kick_recovers_missed_scan_boundary_callback(void)
{
    struct mesh_outbound envelope = delivery_envelope(15u);
    struct k_work_delayable *due_kick;
    struct k_work_delayable *delivery_work;
    uint32_t handle;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        80u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    assert(due_kick != NULL);

    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(receive_abort_calls == 1u);
    assert(mesh_route_reschedule_calls == 0u);
    assert(last_rescheduled_work == due_kick);
    assert(last_rescheduled_delay_ms == 1);

    /*
     * Model continuous RX ending normally in the request/timeout race. It
     * releases scan ownership but never invokes the usual safe-boundary hook.
     */
    gateway_scan_active = false;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(gateway_priority_safe_boundary_calls == 1u);
    assert(mesh_route_reschedule_calls == 1u);
    assert(gateway_delivery_due_pending);

    delivery_work = last_rescheduled_work;
    assert(delivery_work != due_kick);
    delivery_work->work.handler(&delivery_work->work);
    assert(try_flood_calls == 1u);
    assert(!gateway_delivery_due_pending);
    assert(restart_scan_calls == 1u);
}

static void test_gateway_due_kick_missed_boundary_stress(void)
{
    for (uint32_t cycle = 0u; cycle < 512u; cycle++) {
        struct mesh_outbound envelope =
            delivery_envelope((uint16_t)(150u + cycle));
        struct k_work_delayable *due_kick;
        struct k_work_delayable *delivery_work;
        uint32_t handle;

        reset_fixture();
        gateway_scan_active = true;
        assert(app_node_comm_submit_delivery(
            &envelope,
            NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
            10000u,
            900u + cycle,
            &handle) == 0);
        due_kick = last_rescheduled_work;
        assert(due_kick != NULL);
        due_kick->work.handler(&due_kick->work);

        for (uint32_t wait = 0u; wait < cycle % 9u; wait++) {
            due_kick->work.handler(&due_kick->work);
            assert(gateway_delivery_due_pending);
            assert(gateway_delivery_due_begin_calls == 1u);
            assert(receive_abort_calls == 1u);
            assert(mesh_route_reschedule_calls == 0u);
        }

        gateway_scan_active = false;
        due_kick->work.handler(&due_kick->work);
        assert(gateway_priority_safe_boundary_calls == 1u);
        assert(mesh_route_reschedule_calls == 1u);
        delivery_work = last_rescheduled_work;
        assert(delivery_work != due_kick);
        delivery_work->work.handler(&delivery_work->work);
        assert(try_flood_calls == 1u);
        assert(!gateway_delivery_due_pending);
        assert(restart_scan_calls == 1u);
    }
}

static void test_gateway_due_kick_retries_behind_host_priority(void)
{
    struct mesh_outbound envelope = delivery_envelope(14u);
    struct k_work_delayable *due_kick;
    uint32_t handle;

    reset_fixture();
    gateway_priority_safe_boundary_results[0] = -EAGAIN;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        81u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(gateway_priority_safe_boundary_calls == 1u);
    assert(mesh_route_reschedule_calls == 0u);
    assert(last_rescheduled_work == due_kick);
    assert(last_rescheduled_delay_ms == 1);

    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(gateway_priority_safe_boundary_calls == 2u);
    assert(mesh_route_reschedule_calls == 1u);
    assert(gateway_priority_schedule_order < mesh_route_schedule_order);
    assert(gateway_delivery_due_pending);
}

static void test_gateway_due_kick_retries_transient_route_queue_failure(void)
{
    struct mesh_outbound envelope = delivery_envelope(16u);
    struct k_work_delayable *due_kick;
    uint32_t handle;

    reset_fixture();
    mesh_route_reschedule_result = -EBUSY;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        84u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(mesh_route_reschedule_calls == 1u);
    assert(last_rescheduled_work == due_kick);
    assert(last_rescheduled_delay_ms == 1);
    assert(restart_scan_calls == 0u);
    assert(try_flood_calls == 0u);

    mesh_route_reschedule_result = 0;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_begin_calls == 1u);
    assert(gateway_priority_safe_boundary_calls == 2u);
    assert(mesh_route_reschedule_calls == 2u);
    assert(gateway_delivery_due_pending);
}

static void test_gateway_due_gate_cancellation_releases_scan_without_rf(void)
{
    struct mesh_outbound envelope = delivery_envelope(13u);
    struct k_work_delayable *due_kick;
    struct k_work_delayable *restart_work;
    uint32_t handle;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        80u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(app_node_comm_cancel_delivery(handle) == 0);
    assert(!gateway_delivery_due_pending);
    assert(mesh_route_reschedule_calls == 0u);
    assert(try_flood_calls == 0u);
    assert(restart_scan_calls == 0u);
    assert(due_kick->cancel_calls >= 1u);
    restart_work = last_rescheduled_work;
    assert(restart_work != NULL);
    assert(restart_work != due_kick);
    restart_work->work.handler(&restart_work->work);
    assert(restart_scan_calls == 1u);
}

static void
test_gateway_scan_restart_schedule_failure_stops_watchdog_feeding(void)
{
    struct mesh_outbound first = delivery_envelope(173u);
    struct mesh_outbound second = delivery_envelope(174u);
    struct node_comm_terminal_event event;
    struct k_work_delayable *due_kick;
    struct k_work_delayable *restart_work;
    uint32_t first_handle = 0u;
    uint32_t second_handle = 0u;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &first,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        1730u,
        &first_handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(app_node_comm_cancel_delivery(first_handle) == 0);
    restart_work = last_rescheduled_work;
    assert(restart_work != NULL);
    assert(restart_work != due_kick);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));

    restart_work->reschedule_result = -EIO;
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &second,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        1740u,
        &second_handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(app_node_comm_cancel_delivery(second_handle) == 0);
    assert(!gateway_delivery_due_pending);
    assert(restart_scan_calls == 0u);
    assert(watchdog_stop_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));
}

static void test_gateway_due_gate_pause_and_stop_clear_without_rf(void)
{
    struct mesh_outbound envelope = delivery_envelope(15u);
    struct node_comm_pause_lease pause_lease;
    struct k_work_delayable *due_kick;
    uint32_t handle;

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        82u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(app_node_comm_pause_request(90u, 100u, &pause_lease) == 0);
    assert(!gateway_delivery_due_pending);
    assert(try_flood_calls == 0u);
    assert(due_kick->cancel_calls >= 1u);

    gateway_scan_active = false;
    atomic_store(&receive_abort_pending, false);
    assert(app_node_comm_pause_note_quiesced(&pause_lease) == 0);
    assert(app_node_comm_resume_begin(&pause_lease) == 0);
    assert(app_node_comm_resume_complete(&pause_lease) == 0);
    assert(app_node_comm_cancel_delivery(handle) == 0);

    reset_fixture();
    gateway_scan_active = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        1000u,
        83u,
        &handle) == 0);
    due_kick = last_rescheduled_work;
    due_kick->work.handler(&due_kick->work);
    assert(gateway_delivery_due_pending);
    assert(app_node_comm_stop_preserving_queued() == -EINPROGRESS);
    assert(!gateway_delivery_due_pending);
    assert(try_flood_calls == 0u);
    gateway_scan_active = false;
    atomic_store(&receive_abort_pending, false);
    assert(app_node_comm_stop_preserving_queued() == 0);
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
    assert(try_flood_wake_train[0]);
    assert(try_flood_wake_train[1]);

    first_rf_ms = first_delay_ms + second_delay_ms;
    for (uint32_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)(first_rf_ms + attempt * 40u));
        assert(app_node_comm_service_deliveries() == 0);
        assert(try_flood_wake_train[2u + attempt] == (attempt == 0u));
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

static void test_best_effort_uplink_sends_once_without_reliable_custody(void)
{
    struct mesh_outbound heartbeat = reliable_uplink_envelope(13u);
    uint64_t target_ids[1u] = {0u};

    reset_fixture();
    heartbeat.packet.msg_type = MSG_ANCHOR_HEARTBEAT;
    heartbeat.packet.flags = 0u;
    assert(app_node_comm_submit_best_effort_uplink(
               &heartbeat, 1000u, 130u) == 0);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 1u) == 0u);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_envelopes[0].packet.msg_type ==
           MSG_ANCHOR_HEARTBEAT);
    assert(try_uplink_envelopes[0].packet.flags == 0u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 1u) == 0u);

    reset_fixture();
    try_uplink_results[0] = -EHOSTUNREACH;
    assert(app_node_comm_submit_best_effort_uplink(
               &heartbeat, 1000u, 131u) == 0);
    assert(app_node_comm_service_deliveries() == -EHOSTUNREACH);
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_pending_delivery_count() == 0u);
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

static void test_replayed_gateway_ack_coalesces_by_acknowledged_identity(void)
{
    struct mesh_outbound flood = delivery_envelope(140u);
    struct mesh_outbound first = control_response_envelope(141u);
    struct mesh_outbound replay = first;
    struct mesh_outbound different = first;
    struct app_node_comm_control_response_health health;
    uint32_t flood_handle = 0u;

    reset_fixture();
    different.packet.seq += 2u;
    different.payload[1] ^= 0x01u;

    assert(app_node_comm_submit_delivery(
               &flood, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               10000u, flood.packet.seq, &flood_handle) == 0);
    assert(app_node_comm_auto_reap_delivery(flood_handle) == 0);
    assert(app_node_comm_submit_control_response(
               &first, 10000u, first.packet.seq) == 0);
    for (uint16_t seq = 142u; seq < 162u; seq++) {
        replay.packet.seq = seq;
        assert(app_node_comm_submit_control_response(
                   &replay, 10000u, replay.packet.seq) == 0);
    }
    assert(app_node_comm_pending_delivery_count() == 2u);
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);

    assert(app_node_comm_submit_control_response(
               &different, 10000u, different.packet.seq) == 0);
    assert(app_node_comm_pending_delivery_count() == 3u);
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 2u);

    for (uint8_t attempt = 0u; attempt < 4u; attempt++) {
        atomic_store(&fake_now_ms, (int64_t)attempt * 40);
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_flood_calls == 4u);
    assert(try_response_calls == 2u);
    assert(try_response_envelopes[0].packet.seq == first.packet.seq);
    assert(try_response_envelopes[1].packet.seq == different.packet.seq);
    assert(app_node_comm_pending_delivery_count() == 0u);
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

static void test_gateway_batch_ack_retry_keeps_exact_identity_and_one_terminal(void)
{
    struct mesh_outbound response = control_response_envelope(48u);
    struct mesh_outbound frozen;
    struct app_node_comm_control_response_health health;
    uint32_t retry_delay_ms;

    response.payload[0] = UINT8_C(0x04);
    response.payload[1] = UINT8_C(0x91);
    frozen = response;

    reset_fixture();
    try_response_results[0] = -ETIMEDOUT;
    try_response_sent[0] = true;
    assert(app_node_comm_submit_control_response(
        &response, 10000u, UINT32_C(0x4848)) == 0);

    response.packet.seq++;
    response.payload[0] ^= UINT8_C(0xff);
    response.payload[1] ^= UINT8_C(0xff);
    assert(app_node_comm_service_deliveries() == -ETIMEDOUT);
    assert(app_node_comm_retry_backoff_ms(
               &frozen, NODE_COMM_PROFILE_CONTROL_RESPONSE,
               1u, &retry_delay_ms) == 0);
    atomic_store(&fake_now_ms, retry_delay_ms);
    assert(app_node_comm_service_deliveries() == 0);

    assert(try_response_calls == 2u);
    for (uint32_t i = 0u; i < try_response_calls; i++) {
        assert(try_response_envelopes[i].packet.msg_type ==
               frozen.packet.msg_type);
        assert(try_response_envelopes[i].packet.src_id ==
               frozen.packet.src_id);
        assert(try_response_envelopes[i].packet.dst_id ==
               frozen.packet.dst_id);
        assert(try_response_envelopes[i].packet.session_id ==
               frozen.packet.session_id);
        assert(try_response_envelopes[i].packet.seq == frozen.packet.seq);
        assert(try_response_envelopes[i].next_hop_id == frozen.next_hop_id);
        assert(try_response_envelopes[i].payload_len == frozen.payload_len);
        assert(memcmp(try_response_envelopes[i].payload,
                      frozen.payload,
                      frozen.payload_len) == 0);
    }
    app_node_comm_control_response_health_get(&health);
    assert(health.submitted == 1u);
    assert(health.delivered == 1u);
    assert(health.failed == 0u);
    assert(health.last_attempts_started == 2u);
    assert(app_node_comm_pending_delivery_count() == 0u);
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

static void test_control_response_txfrs_before_deadline_beats_service_race(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound response = control_response_envelope(181u);
    struct mesh_outbound unrelated = reliable_uplink_envelope(182u);
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t response_handle = 0u;
    uint32_t unrelated_handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &response, NODE_COMM_PROFILE_CONTROL_RESPONSE,
               100u, 181u, &response_handle) == 0);
    assert(app_node_comm_submit_delivery(
               &unrelated, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               100u, 182u, &unrelated_handle) == 0);
    atomic_store(&fake_now_ms, 99);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_backend_after_observation = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&backend_observation_ready);

    atomic_store(&fake_now_ms, 100);
    assert(app_node_comm_take_delivery_event_for(unrelated_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 100u);
    assert(!app_node_comm_take_delivery_event_for(response_handle, &event));

    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(app_node_comm_take_delivery_event_for(response_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.terminal_at_ms == 99u);
    assert(event.attempts_started == 1u);
}

static void test_control_response_txfrs_at_or_after_deadline_loses(void)
{
    const uint64_t event_times_ms[] = {100u, 101u};

    for (size_t i = 0u;
         i < sizeof(event_times_ms) / sizeof(event_times_ms[0]);
         i++) {
        struct adapter_thread_result service_result = {0};
        struct mesh_outbound response =
            control_response_envelope((uint16_t)(183u + i));
        struct node_comm_terminal_event event;
        pthread_t service_thread;
        uint32_t handle = 0u;

        reset_fixture();
        assert(app_node_comm_submit_delivery(
                   &response, NODE_COMM_PROFILE_CONTROL_RESPONSE,
                   100u, (uint32_t)(183u + i), &handle) == 0);
        atomic_store(&fake_now_ms, 99);
        assert(pthread_mutex_lock(&interleave_lock) == 0);
        block_send = true;
        assert(pthread_mutex_unlock(&interleave_lock) == 0);
        assert(pthread_create(&service_thread, NULL,
                              adapter_delivery_service_thread,
                              &service_result) == 0);
        wait_for_interleave_flag(&send_entered);
        atomic_store(&fake_now_ms, (int64_t)event_times_ms[i]);
        (void)app_node_comm_pending_delivery_count();
        release_blocked_send();
        assert(pthread_join(service_thread, NULL) == 0);
        assert(service_result.result == 0);
        assert(app_node_comm_take_delivery_event_for(handle, &event));
        assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
        assert(event.terminal_at_ms == event_times_ms[i]);
        /* A deliberately misbehaving test backend did start RF, so account it. */
        assert(event.attempts_started == 1u);
    }
}

static void test_fourth_bounded_copy_before_deadline_beats_service_race(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound flood = delivery_envelope(185u);
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &flood, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               130u, 185u, &handle) == 0);
    for (uint64_t attempt_ms = 0u; attempt_ms <= 80u; attempt_ms += 40u) {
        atomic_store(&fake_now_ms, (int64_t)attempt_ms);
        assert(app_node_comm_service_deliveries() == 0);
    }
    atomic_store(&fake_now_ms, 129);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_backend_after_observation = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&backend_observation_ready);
    atomic_store(&fake_now_ms, 130);
    assert(app_node_comm_pending_delivery_count() == 1u);
    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.terminal_at_ms == 129u);
    assert(event.attempts_started == 4u);
}

static void test_reliable_ack_timestamp_controls_deadline_race(void)
{
    for (bool gateway_confirmed = false; ; gateway_confirmed = true) {
        struct adapter_thread_result service_result = {0};
        struct mesh_outbound uplink = reliable_uplink_envelope(
            gateway_confirmed ? 187u : 186u);
        struct node_comm_terminal_event event;
        pthread_t service_thread;
        uint32_t handle = 0u;

        reset_fixture();
        try_uplink_confirmed[0] = gateway_confirmed;
        assert(app_node_comm_submit_delivery(
                   &uplink, NODE_COMM_PROFILE_RELIABLE_UPLINK,
                   100u, gateway_confirmed ? 187u : 186u, &handle) == 0);
        atomic_store(&fake_now_ms, 99);
        assert(pthread_mutex_lock(&interleave_lock) == 0);
        block_backend_after_observation = true;
        assert(pthread_mutex_unlock(&interleave_lock) == 0);
        assert(pthread_create(&service_thread, NULL,
                              adapter_delivery_service_thread,
                              &service_result) == 0);
        wait_for_interleave_flag(&backend_observation_ready);
        atomic_store(&fake_now_ms, 100);
        assert(app_node_comm_pending_delivery_count() == 1u);
        release_blocked_send();
        assert(pthread_join(service_thread, NULL) == 0);
        assert(service_result.result == 0);
        assert(app_node_comm_take_delivery_event_for(handle, &event));
        assert(event.reason == (gateway_confirmed ?
            NODE_COMM_TERMINAL_DELIVERED :
            NODE_COMM_TERMINAL_DEADLINE_EXPIRED));
        assert(event.terminal_at_ms == (gateway_confirmed ? 99u : 100u));
        assert(event.attempts_started == 1u);
        if (gateway_confirmed) {
            break;
        }
    }

    struct adapter_thread_result service_result = {0};
    struct mesh_outbound uplink = reliable_uplink_envelope(188u);
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
               &uplink, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               100u, 188u, &handle) == 0);
    atomic_store(&fake_now_ms, 99);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_uplink_view = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);
    atomic_store(&fake_now_ms, 100);
    (void)app_node_comm_pending_delivery_count();
    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.terminal_at_ms == 100u);
    assert(event.attempts_started == 1u);
}

static void test_reliable_uplink_waits_for_exact_gateway_confirmation(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(60u);
    struct mesh_outbound conflicting = envelope;
    struct mesh_outbound flag_conflict = envelope;
    struct proto_packet wrong = envelope.packet;
    struct proto_packet wrong_flags = envelope.packet;
    struct node_comm_terminal_event event;
    uint8_t conflicting_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
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
    flag_conflict.packet.flags ^= FLAG_DIAGNOSTIC;
    assert(app_node_comm_submit_delivery(
        &flag_conflict, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 600u, &duplicate_handle) == -EEXIST);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(memcmp(&try_uplink_envelopes[0], &envelope,
                  sizeof(envelope)) == 0);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_pending_delivery_count() == 1u);

    assert(mesh_packet_semantic_digest(&envelope.packet,
                                       envelope.payload,
                                       envelope.payload_len,
                                       semantic_digest));
    assert(mesh_packet_semantic_digest(&conflicting.packet,
                                       conflicting.payload,
                                       conflicting.payload_len,
                                       conflicting_digest));
    wrong.seq++;
    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &wrong, semantic_digest,
               (uint64_t)atomic_load(&fake_now_ms)) == -ENOENT);
    wrong_flags.flags ^= FLAG_DIAGNOSTIC;
    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &wrong_flags, semantic_digest,
               (uint64_t)atomic_load(&fake_now_ms)) == -EBADMSG);
    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &envelope.packet, conflicting_digest,
               (uint64_t)atomic_load(&fake_now_ms)) == -EBADMSG);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_note_gateway_confirmed(&envelope.packet) ==
           -ENOTSUP);
    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
    assert(event.client_token == 600u);
}

static void
test_four_durable_pair_results_survive_long_single_flight_confirmation_outage(
    void)
{
    struct mesh_outbound results[SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT];
    uint32_t handles[SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT] = {0};
    struct node_comm_terminal_event event;

    reset_fixture();
    for (size_t i = 0u; i < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT; i++) {
        results[i] = reliable_uplink_envelope((uint16_t)(210u + i));
        results[i].packet.msg_type = MSG_SURVEY_PAIR_RESULT;
        results[i].packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
        results[i].queued_at_ms = 0u;
        results[i].queued_at_valid = false;
        results[i].earliest_tx_ms = 0u;
        results[i].earliest_tx_valid = false;
        assert(app_node_comm_submit_delivery(
                   &results[i],
                   NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
                   UINT64_MAX,
                   2100u + (uint32_t)i,
                   &handles[i]) == 0);
    }
    assert(app_node_comm_pending_delivery_count() ==
           SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_envelopes[0].packet.seq ==
           results[0].packet.seq);
    assert(!try_uplink_envelopes[0].queued_at_valid);

    atomic_store(
        &fake_now_ms,
        (int_fast64_t)SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS + 1000);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_pending_delivery_count() ==
           SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
    for (size_t i = 0u; i < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT; i++) {
        assert(!app_node_comm_peek_delivery_event_for(
            handles[i], &event));
    }

    assert(note_gateway_confirmed_envelope(&results[0]) == 0);
    assert(app_node_comm_take_delivery_event_for(
        handles[0], &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    for (size_t i = 1u; i < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT; i++) {
        try_uplink_confirmed[i] = true;
        assert(app_node_comm_service_deliveries() == 0);
        assert(try_uplink_calls == i + 1u);
        assert(try_uplink_envelopes[i].packet.seq ==
               results[i].packet.seq);
        assert(!try_uplink_envelopes[i].queued_at_valid);
        assert(app_node_comm_take_delivery_event_for(
            handles[i], &event));
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    }
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(try_uplink_calls == SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
}

static void test_reliable_targets_track_only_backend_profiles(void)
{
    static const enum node_comm_delivery_profile profiles[] = {
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        NODE_COMM_PROFILE_CONTROL_RESPONSE,
    };

    for (size_t i = 0u; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        struct mesh_outbound envelope = profiles[i] == NODE_COMM_PROFILE_CONTROL_RESPONSE ?
            control_response_envelope((uint16_t)(70u + i)) :
            reliable_uplink_envelope((uint16_t)(70u + i));
        uint64_t target_ids[2u] = {0};
        uint32_t handle;
        bool reliable = profiles[i] == NODE_COMM_PROFILE_RELIABLE_UPLINK ||
                        profiles[i] == NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
                        profiles[i] == NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE;

        reset_fixture();
        assert(app_node_comm_submit_delivery(&envelope, profiles[i],
                                             1000u, 700u + i, &handle) == 0);
        assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) ==
               (reliable ? 1u : 0u));
        assert(!reliable || target_ids[0] == envelope.packet.dst_id);
    }

    struct mesh_outbound response = reliable_uplink_envelope(80u);
    uint64_t target_ids[2u] = {0};
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &response, NODE_COMM_PROFILE_BEST_EFFORT,
               1000u, 799u, &handle) == -ENOTSUP);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);

    reset_fixture();
    try_uplink_results[0] = -EAGAIN;
    try_uplink_sent[0] = false;
    assert(app_node_comm_submit_protocol_response(
               &response, 1000u, 800u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 1u);
    assert(target_ids[0] == response.packet.dst_id);
    atomic_store(&fake_now_ms, 1000);
    (void)app_node_comm_pending_delivery_count();
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);

    reset_fixture();
    assert(app_node_comm_submit_protocol_response(
               &response, 1000u, 801u, &handle) == 0);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 1u);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);
    assert(note_gateway_confirmed_envelope(&response) == 0);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);

    struct mesh_outbound waiting = response;

    waiting.packet.seq++;
    reset_fixture();
    assert(app_node_comm_submit_protocol_response(
               &response, 1000u, 802u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_submit_protocol_response(
               &waiting, 1000u, 803u, &handle) == 0);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 1u);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);
}

static void test_durable_survey_submit_is_async_and_exact_under_pressure_pause(void)
{
    struct mesh_outbound survey = survey_report_envelope(169u);
    const struct mesh_outbound expected = survey;
    struct node_comm_pause_lease pause_lease;
    struct node_comm_terminal_event event;
    struct app_node_comm_reservation_lease protocol_reservation = {0};
    uint32_t ordinary_handles[APP_NODE_COMM_MAX_DELIVERIES];
    const uint32_t ordinary_count = APP_NODE_COMM_MAX_DELIVERIES -
        APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES;
    uint32_t survey_handle = 0u;

    reset_fixture();
    for (uint32_t i = 0u; i < ordinary_count; i++) {
        struct mesh_outbound ordinary = reliable_uplink_envelope(
            (uint16_t)(180u + i));

        assert(app_node_comm_submit_delivery(
                   &ordinary,
                   NODE_COMM_PROFILE_RELIABLE_UPLINK,
                   60000u,
                   1800u + i,
                   &ordinary_handles[i]) == 0);
    }
    assert(app_node_comm_submit_delivery(
               &survey,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               60000u,
               survey.packet.session_id,
               &survey_handle) == -ENOSPC);
    assert(survey_handle == 0u);
    /* A durable survey sample cannot spend the protected command-response slot. */
    assert(app_node_comm_reserve_protocol_response(
               0x169u, &protocol_reservation) == 0);
    assert(app_node_comm_cancel_delivery(ordinary_handles[0]) == 0);
    assert(app_node_comm_take_delivery_event_for(ordinary_handles[0], &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(app_node_comm_submit_delivery(
               &survey,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               60000u,
               survey.packet.session_id,
               &survey_handle) == 0);
    assert(survey_handle != 0u);
    assert(app_node_comm_pending_delivery_count() == ordinary_count);
    assert(try_uplink_calls == 0u);

    memset(&survey, 0xee, sizeof(survey));
    assert(app_node_comm_pause_request(909u, 1000u, &pause_lease) == 0);
    assert(app_node_comm_pause_note_quiesced(&pause_lease) == 0);
    assert(app_node_comm_service_deliveries() == -ESHUTDOWN);
    assert(try_uplink_calls == 0u);
    assert(app_node_comm_pending_delivery_count() == ordinary_count);

    assert(app_node_comm_resume_begin(&pause_lease) == 0);
    assert(app_node_comm_resume_complete(&pause_lease) == 0);
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(memcmp(&try_uplink_envelopes[0], &expected,
                  sizeof(expected)) == 0);
    assert(app_node_comm_take_delivery_event_for(survey_handle, &event));
    assert(event.handle == survey_handle);
    assert(event.client_token == expected.packet.session_id);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);

    for (uint32_t i = 1u; i < ordinary_count; i++) {
        assert(app_node_comm_abandon_delivery(ordinary_handles[i]) == 0);
    }
    assert(app_node_comm_cancel_protocol_response_reservation(
               &protocol_reservation) == 0);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_reliable_uplink_waits_for_scheduled_channel9_boundary(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(166u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_results[0] = -EBUSY;
    try_uplink_sent[0] = false;
    try_uplink_retry_delays_ms[0] = 137u;
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1660u, &handle) == 0);

    assert(app_node_comm_service_deliveries() == -EBUSY);
    assert(try_uplink_calls == 1u);
    atomic_store(&fake_now_ms, 136);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_uplink_calls == 1u);

    atomic_store(&fake_now_ms, 137);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

static void test_reliable_backend_retries_are_observed_without_being_capped(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(162u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        100u, 1620u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);

    /* The facade observes the backend's retries; it does not cap or reschedule them. */
    for (uint32_t retry = 0u; retry < 12u; retry++) {
        atomic_store(&fake_now_ms, (int64_t)retry + 1);
        assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
        assert(app_node_comm_note_backend_rf_started(&envelope.packet) == 0);
    }
    assert(cancel_uplink_calls == 0u);

    atomic_store(&fake_now_ms, 100);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) ==
           -ETIMEDOUT);
    assert(cancel_uplink_calls == 1u);
    assert(last_cancelled_uplink.msg_type == envelope.packet.msg_type);
    assert(last_cancelled_uplink.src_id == envelope.packet.src_id);
    assert(last_cancelled_uplink.dst_id == envelope.packet.dst_id);
    assert(last_cancelled_uplink.session_id == envelope.packet.session_id);
    assert(last_cancelled_uplink.seq == envelope.packet.seq);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) ==
           -ETIMEDOUT);
    assert(cancel_uplink_calls == 1u);

    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 13u);
    assert(event.client_token == 1620u);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
}

static void test_terminal_race_after_preflight_cannot_restart_backend(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(163u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1630u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);

    /* A confirmation can win after preflight but before the RF-start report. */
    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(cancel_uplink_calls == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(!app_node_comm_take_delivery_event(&event));
    assert(app_node_comm_note_backend_rf_started(&envelope.packet) ==
           -ECANCELED);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 2u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
}

static void test_backend_attempt_completion_gates_cancel_and_auto_reap(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(165u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1650u, &handle) == 0);
    assert(app_node_comm_auto_reap_delivery(handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);

    assert(app_node_comm_cancel_delivery(handle) == 0);
    assert(cancel_uplink_calls == 0u);
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_complete_backend_attempt(&envelope.packet, true) ==
           -ECANCELED);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
}

static void test_outer_backend_cancel_waits_for_active_call_to_return(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound envelope = reliable_uplink_envelope(167u);
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1670u, &handle) == 0);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_uplink_view = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);

    assert(app_node_comm_cancel_delivery(handle) == 0);
    assert(cancel_uplink_calls == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));

    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == 0);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
}

static void
test_external_gateway_proof_during_pre_rf_callback_resolves_after_return(void)
{
    struct adapter_thread_result service_result = {0};
    struct mesh_outbound envelope = reliable_uplink_envelope(169u);
    struct node_comm_terminal_event event;
    pthread_t service_thread;
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_sent[0] = false;
    try_uplink_results[0] = -EBUSY;
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1690u, &handle) == 0);
    assert(pthread_mutex_lock(&interleave_lock) == 0);
    block_uplink_view = true;
    assert(pthread_mutex_unlock(&interleave_lock) == 0);
    assert(pthread_create(&service_thread, NULL,
                          adapter_delivery_service_thread,
                          &service_result) == 0);
    wait_for_interleave_flag(&send_entered);

    /* The digest-bound proof is retained, but cannot revoke a live lease. */
    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));

    release_blocked_send();
    assert(pthread_join(service_thread, NULL) == 0);
    assert(service_result.result == -EBUSY);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    /* Exact proof reconstructs the one backend RF start the facade missed. */
    assert(event.attempts_started == 1u);
}

static void test_external_gateway_proof_recovery_rejects_wrong_identity(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(170u);
    struct mesh_outbound conflicting = envelope;
    struct proto_packet wrong = envelope.packet;
    struct node_comm_terminal_event event;
    uint8_t conflicting_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1700u, &handle) == 0);
    assert(mesh_packet_semantic_digest(&envelope.packet,
                                       envelope.payload,
                                       envelope.payload_len,
                                       semantic_digest));
    conflicting.payload[0] ^= 0x5au;
    assert(mesh_packet_semantic_digest(&conflicting.packet,
                                       conflicting.payload,
                                       conflicting.payload_len,
                                       conflicting_digest));
    wrong.seq++;

    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &wrong, semantic_digest, 0u) == -ENOENT);
    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &envelope.packet, conflicting_digest, 0u) == -EBADMSG);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_note_gateway_confirmed(&envelope.packet) ==
           -ENOTSUP);

    assert(app_node_comm_note_gateway_confirmed_digest_at(
               &envelope.packet, semantic_digest, 0u) == 0);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

static void
test_foreign_workqueue_cancel_waits_for_exact_route_owned_completion(void)
{
    struct adapter_cancel_thread_request request = {0};
    struct mesh_outbound envelope = reliable_uplink_envelope(168u);
    struct node_comm_terminal_event event;
    uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];
    pthread_t cancel_thread;
    uint32_t request_token;
    uint32_t handle = 0u;

    reset_fixture();
    envelope.packet.flags = 0xa5u;
    cancel_uplink_complete_immediately = false;
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1680u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);

    request.handle = handle;
    assert(pthread_create(&cancel_thread, NULL,
                          adapter_cancel_delivery_thread,
                          &request) == 0);
    assert(pthread_join(cancel_thread, NULL) == 0);
    assert(request.completion.result == 0);
    assert(cancel_uplink_calls == 1u);
    assert(cancel_uplink_request_pending);
    assert(!cancel_uplink_completion_ready);
    assert(last_cancel_uplink_delivery_handle == handle);
    request_token = last_cancel_uplink_request_token;
    assert(request_token != 0u);
    assert(mesh_packet_semantic_digest(&envelope.packet,
                                       envelope.payload,
                                       envelope.payload_len,
                                       expected_digest));
    assert(memcmp(expected_digest,
                  last_cancel_uplink_semantic_digest,
                  sizeof(expected_digest)) == 0);

    /* Polling cannot reissue or expose the terminal event before completion. */
    assert(!app_node_comm_peek_delivery_event_for(handle, &event));
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(cancel_uplink_calls == 1u);

    /* A stale completion edge cannot release another request generation. */
    app_node_comm_backend_release_ready(handle, request_token + 1u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(cancel_uplink_calls == 1u);

    cancel_uplink_completion_ready = true;
    app_node_comm_backend_release_ready(handle, request_token);
    assert(app_node_comm_peek_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_CANCELLED);
    assert(cancel_uplink_calls == 1u);

    /* Late duplicate publication is harmless after the exact record retires. */
    app_node_comm_backend_release_ready(handle, request_token);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
}

static void test_backend_false_start_completion_releases_attempt_lease(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(164u);
    struct node_comm_terminal_event event;
    uint32_t handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 1640u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);

    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == -EBUSY);
    assert(app_node_comm_complete_backend_attempt(&envelope.packet, false) ==
           0);
    assert(cancel_uplink_calls == 0u);

    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(cancel_uplink_calls == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_complete_backend_attempt(&envelope.packet, false) ==
           -ECANCELED);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
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
    assert(note_gateway_confirmed_envelope(&late) == -ESTALE);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 1u);
}

static void
test_terminal_waits_for_confirmed_backend_release_after_cancel_failure(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(175u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    cancel_uplink_result = -EIO;
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u,
        1750u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(cancel_uplink_calls == 1u);
    assert(!app_node_comm_peek_delivery_event_for(handle, &event));
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
    assert(app_node_comm_pending_delivery_count() == 1u);

    cancel_uplink_result = 0;
    atomic_store(&fake_now_ms, 10);
    assert(app_node_comm_peek_delivery_event_for(handle, &event));
    assert(cancel_uplink_calls == 2u);
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

static void
test_serialized_backend_cancel_contention_does_not_spend_hard_failures(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(179u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    cancel_uplink_request_result = -EBUSY;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u,
        1790u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(cancel_uplink_calls == 1u);
    assert(watchdog_stop_calls == 0u);
    assert(app_node_comm_pending_delivery_count() == 1u);

    /* Four serialized busy returns used to exhaust the hard-failure budget. */
    for (uint32_t retry = 1u; retry <= 4u; retry++) {
        atomic_store(&fake_now_ms, (int64_t)retry * 10);
        assert(!app_node_comm_peek_delivery_event_for(handle, &event));
        assert(cancel_uplink_calls == retry + 1u);
        assert(watchdog_stop_calls == 0u);
    }

    /* Once the competing cancellation leaves the serialized slot, release. */
    cancel_uplink_request_result = 0;
    atomic_store(&fake_now_ms, 50);
    assert(app_node_comm_peek_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_serialized_backend_cancel_contention_is_bounded(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(180u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    cancel_uplink_request_result = -EBUSY;
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u,
        1800u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);

    /* Persistent serialization contention eventually fails closed once. */
    for (uint32_t retry = 1u;
         retry <= 64u && watchdog_stop_calls == 0u;
         retry++) {
        atomic_store(&fake_now_ms, (int64_t)retry * 10);
        assert(!app_node_comm_peek_delivery_event_for(handle, &event));
    }
    assert(watchdog_stop_calls == 1u);
    assert(cancel_uplink_calls <= 33u);
    assert(app_node_comm_pending_delivery_count() == 1u);
}

static void test_caller_owned_terminal_event_has_bounded_watchdog_grace(void)
{
    struct mesh_outbound abandoned = reliable_uplink_envelope(177u);
    struct mesh_outbound consumed = reliable_uplink_envelope(178u);
    struct node_comm_terminal_event event;
    struct k_work_delayable *terminal_watchdog_work;
    const uint64_t abandoned_terminal_ms = 100u;
    const uint64_t consumed_terminal_ms = 200u;
    uint32_t handle = 0u;

    reset_fixture();
    atomic_store(&fake_now_ms, (int64_t)abandoned_terminal_ms);
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
        &abandoned,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        60000u,
        1770u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(cancel_uplink_calls == 1u);
    assert(app_node_comm_peek_delivery_event_for(handle, &event));
    assert(event.terminal_at_ms == abandoned_terminal_ms);
    assert(watchdog_stop_calls == 0u);
    terminal_watchdog_work = last_rescheduled_work;
    assert(terminal_watchdog_work != NULL);
    assert(last_rescheduled_delay_ms ==
           APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS);

    atomic_store(
        &fake_now_ms,
        (int64_t)(abandoned_terminal_ms +
                  APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS - 1u));
    assert(watchdog_stop_calls == 0u);

    atomic_store(
        &fake_now_ms,
        (int64_t)(abandoned_terminal_ms +
                  APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS));
    terminal_watchdog_work->work.handler(&terminal_watchdog_work->work);
    assert(watchdog_stop_calls == 1u);
    atomic_store(
        &fake_now_ms,
        (int64_t)(abandoned_terminal_ms +
                  APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS + 1000u));
    assert(watchdog_stop_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));

    reset_fixture();
    atomic_store(&fake_now_ms, (int64_t)consumed_terminal_ms);
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
        &consumed,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        60000u,
        1780u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(cancel_uplink_calls == 1u);
    terminal_watchdog_work = last_rescheduled_work;
    assert(terminal_watchdog_work != NULL);
    assert(last_rescheduled_delay_ms ==
           APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.terminal_at_ms == consumed_terminal_ms);
    assert(watchdog_stop_calls == 0u);

    atomic_store(
        &fake_now_ms,
        (int64_t)(consumed_terminal_ms +
                  APP_NODE_COMM_CALLER_TERMINAL_OWNER_TIMEOUT_MS));
    terminal_watchdog_work->work.handler(&terminal_watchdog_work->work);
    assert(watchdog_stop_calls == 0u);
}

static void test_unpublished_external_backend_attempt_fails_closed(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(176u);
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &envelope,
        NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u,
        1760u,
        &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);

    atomic_store(&fake_now_ms, 5000);
    (void)app_node_comm_pending_delivery_count();
    assert(watchdog_stop_calls == 1u);
    assert(!app_node_comm_policy_running());
    assert(!app_node_comm_take_delivery_event_for(
        handle, &(struct node_comm_terminal_event){0}));
}

static void test_reliable_uplink_failure_preserves_reason_and_releases_owner(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(160u);
    struct mesh_outbound second = reliable_uplink_envelope(161u);
    struct proto_packet wrong = first.packet;
    struct node_comm_terminal_event event;
    uint8_t first_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t first_handle;
    uint32_t second_handle;

    reset_fixture();
    assert(app_node_comm_submit_protocol_response(
        &first, 10000u, 1600u, &first_handle) == 0);
    assert(app_node_comm_submit_protocol_response(
        &second, 10000u, 1601u, &second_handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);

    assert(mesh_packet_semantic_digest(&first.packet,
                                       first.payload,
                                       first.payload_len,
                                       first_digest));
    wrong.seq += 100u;
    assert(app_node_comm_note_gateway_failed_digest(
               &wrong,
               first_digest,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED) == -ENOENT);
    assert(note_gateway_failed_envelope(
               &first,
               NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED) == 0);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 1u);
    assert(event.client_token == 1600u);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(try_uplink_envelopes[1].packet.seq == second.packet.seq);
    assert(note_gateway_failed_envelope(
               &second,
               NODE_COMM_TERMINAL_DEADLINE_EXPIRED) == 0);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED);
    assert(event.attempts_started == 1u);
}

static void test_adapter_resource_wait_uses_exact_core_owner_trace(void)
{
    struct app_delivery_trace_capture capture = {0};
    struct mesh_outbound first = reliable_uplink_envelope(160u);
    struct mesh_outbound second = reliable_uplink_envelope(161u);
    struct node_comm_terminal_event event;
    uint64_t target_ids[2u] = {0};
    uint32_t first_handle;
    uint32_t second_handle;
    uint32_t blocked_generation = 0u;
    bool saw_blocked = false;
    bool saw_resumed = false;

    reset_fixture();
    assert(app_node_comm_set_delivery_transition_trace(
               capture_app_delivery_transition, &capture) == 0);
    assert(app_node_comm_submit_delivery(
               &first, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               10000u, 1600u, &first_handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_submit_delivery(
               &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               10000u, 1601u, &second_handle) == 0);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 0u);

    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        if (entry->kind == NODE_COMM_DELIVERY_TRACE_RESOURCE_BLOCKED &&
            entry->event.operation_id == second_handle) {
            assert(entry->transition.result == FW_SM_APPLIED);
            assert(entry->transition.old_state == FW_DELIVERY_WAIT_TX);
            assert(entry->transition.new_state == FW_DELIVERY_WAIT_TX);
            blocked_generation = entry->event.generation;
            saw_blocked = true;
        }
    }
    assert(saw_blocked);
    assert(blocked_generation != 0u);

    assert(note_gateway_confirmed_envelope(&first) == 0);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        if (entry->kind == NODE_COMM_DELIVERY_TRACE_RESOURCE_RESUMED &&
            entry->event.operation_id == second_handle &&
            entry->event.generation == blocked_generation) {
            assert(entry->transition.result == FW_SM_APPLIED);
            assert(entry->transition.old_state == FW_DELIVERY_WAIT_TX);
            assert(entry->transition.new_state == FW_DELIVERY_WAIT_TX);
            saw_resumed = true;
        }
    }
    assert(saw_resumed);
    assert(app_node_comm_reliable_delivery_targets(target_ids, 2u) == 1u);
    assert(target_ids[0] == second.packet.dst_id);

    assert(app_node_comm_service_deliveries() == 0);
    assert(note_gateway_confirmed_envelope(&second) == 0);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
}

static void test_single_flight_wait_preserves_priority_and_fifo(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(63u);
    struct mesh_outbound second = reliable_uplink_envelope(64u);
    struct mesh_outbound third = reliable_uplink_envelope(65u);
    struct mesh_outbound protocol = reliable_uplink_envelope(66u);
    struct mesh_outbound control = control_response_envelope(67u);
    struct node_comm_terminal_event event;
    uint32_t first_handle;
    uint32_t second_handle;
    uint32_t third_handle;
    uint32_t protocol_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 603u, &first_handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_envelopes[0].packet.seq == first.packet.seq);

    /*
     * Queue enough work to reproduce a busy single-flight backend under load.
     * Polling that backend must not turn queued work into independently
     * jittered retries: after the owner completes, protocol work must retain
     * priority and ordinary reliable uplinks must retain FIFO order.
     */
    assert(app_node_comm_submit_delivery(
        &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 604u, &second_handle) == 0);
    assert(app_node_comm_submit_protocol_response(
        &protocol, 10000u, 606u, &protocol_handle) == 0);
    assert(app_node_comm_submit_control_response(
        &control, 10000u, 607u) == 0);

    /* The independent control-response lane remains eligible immediately. */
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_response_calls == 1u);
    assert(try_response_envelopes[0].packet.seq == control.packet.seq);
    assert(try_uplink_calls == 1u);

    for (uint32_t poll = 0u; poll < 16u; poll++) {
        assert(app_node_comm_service_deliveries() == -EAGAIN);
        assert(try_uplink_calls == 1u);
    }

    /* Releasing the owner must not require unrelated randomized wait time. */
    assert(note_gateway_confirmed_envelope(&first) == 0);
    assert(app_node_comm_take_delivery_event_for(first_handle, &event));

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 2u);
    assert(try_uplink_envelopes[1].packet.seq == protocol.packet.seq);
    assert(note_gateway_confirmed_envelope(&protocol) == 0);
    assert(app_node_comm_take_delivery_event_for(protocol_handle, &event));
    assert(app_node_comm_submit_delivery(
        &third, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        10000u, 605u, &third_handle) == 0);

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 3u);
    assert(try_uplink_envelopes[2].packet.seq == second.packet.seq);
    assert(note_gateway_confirmed_envelope(&second) == 0);
    assert(app_node_comm_take_delivery_event_for(second_handle, &event));

    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 4u);
    assert(try_uplink_envelopes[3].packet.seq == third.packet.seq);
    assert(note_gateway_confirmed_envelope(&third) == 0);
    assert(app_node_comm_take_delivery_event_for(third_handle, &event));
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_cancelling_reliable_uplink_releases_backend_owner(void)
{
    struct mesh_outbound first = reliable_uplink_envelope(65u);
    struct mesh_outbound second = reliable_uplink_envelope(66u);
    struct node_comm_terminal_event event;
    uint32_t first_handle;
    uint32_t first_generation;
    uint32_t second_handle;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
        &first, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 605u, &first_handle) == 0);
    assert(app_node_comm_submit_delivery(
        &second, NODE_COMM_PROFILE_RELIABLE_UPLINK,
        1000u, 606u, &second_handle) == 0);
    assert(app_node_comm_delivery_generation(
        first_handle, &first_generation) == 0);
    assert(first_generation != 0u);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_delivery_generations[0] == first_generation);

    assert(app_node_comm_cancel_delivery(first_handle) == 0);
    assert(cancel_uplink_calls == 1u);
    assert(last_cancelled_uplink.seq == first.packet.seq);
    assert(last_cancel_uplink_delivery_handle == first_handle);
    assert(last_cancel_uplink_delivery_generation == first_generation);
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
    assert(note_gateway_confirmed_envelope(&protocol) == 0);
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

static void test_protocol_response_reservation_survives_capacity_exhaustion(void)
{
    uint32_t occupied_handles[APP_NODE_COMM_MAX_DELIVERIES - 1u];
    struct app_node_comm_reservation_lease reservation = {0};
    struct app_node_comm_reservation_lease coalesced = {0};
    struct app_node_comm_reservation_lease blocked = {0};
    uint32_t committed_handle;

    reset_fixture();
    assert(app_node_comm_reserve_protocol_response(0x801u, &reservation) ==
           0);
    assert(reservation.token != 0u);

    /*
     * Fill every other facade and scheduler slot after reserving the terminal
     * response. Unrelated traffic must not be able to steal the reserved
     * record, and a further reservation must fail without returning a token.
     */
    for (uint32_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES - 1u; i++) {
        struct mesh_outbound occupied =
            reliable_uplink_envelope((uint16_t)(230u + i));

        assert(app_node_comm_submit_reliable_uplink(
            &occupied,
            10000u,
            800u + i,
            &occupied_handles[i]) == 0);
    }
    assert(app_node_comm_pending_delivery_count() ==
           APP_NODE_COMM_MAX_DELIVERIES - 1u);
    assert(app_node_comm_reserve_protocol_response(0x802u, &blocked) ==
           -ENOSPC);
    assert(blocked.token == 0u);
    {
        struct mesh_outbound blocked = reliable_uplink_envelope(240u);

        assert(app_node_comm_submit_protocol_response(
            &blocked, 10000u, 810u, &committed_handle) == -ENOSPC);
    }

    {
        struct mesh_outbound terminal = reliable_uplink_envelope(241u);

        assert(app_node_comm_commit_protocol_response(
            &reservation,
            &terminal,
            10000u,
            811u,
            &committed_handle) == 0);
    }
    assert(committed_handle != 0u);
    assert(app_node_comm_pending_delivery_count() ==
           APP_NODE_COMM_MAX_DELIVERIES);
    assert(app_node_comm_cancel_protocol_response_reservation(
               &reservation) == -ESTALE);

    for (uint32_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES - 1u; i++) {
        assert(app_node_comm_abandon_delivery(occupied_handles[i]) == 0);
    }
    assert(app_node_comm_abandon_delivery(committed_handle) == 0);
    assert(app_node_comm_pending_delivery_count() == 0u);

    assert(app_node_comm_reserve_protocol_response(0x803u, &reservation) ==
           0);
    assert(app_node_comm_reserve_protocol_response(0x803u, &coalesced) ==
           0);
    assert(coalesced.token == reservation.token);
    assert(coalesced.owner_generation == reservation.owner_generation);
    assert(coalesced.expires_at_ms == reservation.expires_at_ms);
    assert(app_node_comm_cancel_protocol_response_reservation(
               &reservation) == 0);
    assert(app_node_comm_cancel_protocol_response_reservation(
               &coalesced) == -ESTALE);
}

static void test_bounded_control_reservation_obeys_ordinary_capacity(void)
{
    struct app_node_comm_reservation_lease reservations[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    struct app_node_comm_reservation_lease blocked = {0};
    struct app_node_comm_reservation_lease replacement = {0};

    reset_fixture();
    for (size_t i = 0u;
         i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY;
         i++) {
        assert(app_node_comm_reserve_bounded_control(
                   UINT64_C(0xb001) + i, &reservations[i]) == 0);
        assert(reservations[i].token != 0u);
        assert(reservations[i].owner_generation == UINT64_C(0xb001) + i);
        assert(reservations[i].owner_kind ==
               APP_NODE_COMM_RESERVATION_OWNER_BOUNDED_CONTROL);
    }

    assert(app_node_comm_reserve_bounded_control(0xb101u, &blocked) ==
           -ENOSPC);
    assert(blocked.token == 0u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(try_flood_calls == 0u);

    assert(app_node_comm_cancel_bounded_control_reservation(
               &reservations[2]) == 0);
    assert(app_node_comm_reserve_bounded_control(0xb102u, &replacement) ==
           0);
    assert(replacement.token != 0u);
    assert(replacement.owner_generation == 0xb102u);

    for (size_t i = 0u;
         i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY;
         i++) {
        if (i == 2u) {
            continue;
        }
        assert(app_node_comm_cancel_bounded_control_reservation(
                   &reservations[i]) == 0);
    }
    assert(app_node_comm_cancel_bounded_control_reservation(
               &replacement) == 0);
}

static void test_bounded_control_reservation_commit_is_exact_and_consuming(void)
{
    struct app_node_comm_reservation_lease reservation = {0};
    struct mesh_outbound envelope = delivery_envelope(245u);
    const struct mesh_outbound expected = envelope;
    uint32_t handle = 0u;
    uint32_t stale_handle = 0u;

    reset_fixture();
    assert(app_node_comm_reserve_bounded_control(0xb201u, &reservation) ==
           0);
    assert(app_node_comm_commit_bounded_control_reservation(
               &reservation, &envelope, 10000u, 0xb201u, &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_pending_delivery_count() == 1u);

    /* Commit consumes the lease before any caller-visible RF progress. */
    assert(app_node_comm_cancel_bounded_control_reservation(
               &reservation) == -ESTALE);
    assert(app_node_comm_commit_bounded_control_reservation(
               &reservation, &expected, 10000u, 0xb202u,
               &stale_handle) == -ESTALE);
    assert(stale_handle == 0u);

    envelope.packet.seq = 0u;
    envelope.packet.session_id = 0u;
    envelope.payload_len = 0u;
    memset(envelope.payload, 0xee, sizeof(envelope.payload));
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_flood_calls == 1u);
    assert(try_response_calls == 0u);
    assert(try_uplink_calls == 0u);
    assert(memcmp(&try_flood_envelopes[0], &expected,
                  sizeof(expected)) == 0);

    assert(app_node_comm_cancel_delivery(handle) == 0);
}

static void
test_bounded_control_reservation_coalesces_and_rejects_wrong_owners(void)
{
    struct app_node_comm_reservation_lease first = {0};
    struct app_node_comm_reservation_lease coalesced = {0};
    struct app_node_comm_reservation_lease stale;
    struct app_node_comm_reservation_lease successor = {0};
    struct app_node_comm_reservation_lease wrong_owner;
    struct mesh_outbound envelope = delivery_envelope(246u);
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_reserve_bounded_control(0xb301u, &first) == 0);
    assert(app_node_comm_reserve_bounded_control(0xb301u, &coalesced) == 0);
    assert(coalesced.expires_at_ms == first.expires_at_ms);
    assert(coalesced.owner_generation == first.owner_generation);
    assert(coalesced.token == first.token);
    assert(coalesced.owner_kind == first.owner_kind);

    stale = first;
    assert(app_node_comm_cancel_bounded_control_reservation(&first) == 0);
    assert(app_node_comm_reserve_bounded_control(0xb302u, &successor) == 0);

    assert(app_node_comm_cancel_bounded_control_reservation(&stale) ==
           -ESTALE);
    assert(app_node_comm_commit_bounded_control_reservation(
               &stale, &envelope, 10000u, 0xb301u, &handle) == -ESTALE);
    assert(handle == 0u);

    wrong_owner = successor;
    wrong_owner.owner_generation++;
    assert(app_node_comm_cancel_bounded_control_reservation(&wrong_owner) ==
           -ESTALE);
    assert(app_node_comm_commit_bounded_control_reservation(
               &wrong_owner, &envelope, 10000u, 0xb303u,
               &handle) == -ESTALE);
    assert(handle == 0u);

    wrong_owner = successor;
    wrong_owner.owner_kind =
        APP_NODE_COMM_RESERVATION_OWNER_RELIABLE_UPLINK;
    assert(app_node_comm_cancel_bounded_control_reservation(&wrong_owner) ==
           -EINVAL);
    assert(app_node_comm_commit_bounded_control_reservation(
               &wrong_owner, &envelope, 10000u, 0xb304u,
               &handle) == -EINVAL);
    assert(handle == 0u);

    assert(app_node_comm_commit_bounded_control_reservation(
               &successor, &envelope, 10000u, 0xb302u, &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_abandon_delivery(handle) == 0);
}

static void test_durable_burst_reservation_is_atomic_and_retains_protocol_slot(void)
{
    struct app_node_comm_reservation_lease reservation_leases[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    struct app_node_comm_reservation_lease blocked_leases[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    uint32_t committed_handle = 0u;
    uint32_t protocol_handle = 0u;
    const size_t reservation_count =
        sizeof(reservation_leases) / sizeof(reservation_leases[0]);

    reset_fixture();
    assert(APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY == 5u);
    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0x9001u,
               reservation_count,
               reservation_leases,
               reservation_count) == 0);
    for (size_t i = 0u; i < reservation_count; i++) {
        assert(reservation_leases[i].token != 0u);
        assert(reservation_leases[i].owner_generation == 0x9001u);
    }
    {
        struct mesh_outbound protocol = reliable_uplink_envelope(242u);

        assert(app_node_comm_submit_protocol_response(
                   &protocol, 10000u, 812u, &protocol_handle) == 0);
        assert(protocol_handle != 0u);
        assert(app_node_comm_abandon_delivery(protocol_handle) == 0);
    }
    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0x9002u, 1u, blocked_leases, reservation_count) == -ENOSPC);
    assert(blocked_leases[0].token == 0u);

    {
        struct mesh_outbound sample = reliable_uplink_envelope(243u);

        sample.packet.msg_type = MSG_SURVEY_PAIR_RESULT;
        assert(app_node_comm_commit_durable_reliable_uplink_reservation(
                   &reservation_leases[0],
                   &sample,
                   10000u,
                   813u,
                   &committed_handle) == 0);
        memset(&reservation_leases[0], 0, sizeof(reservation_leases[0]));
    }
    for (size_t i = 1u; i < reservation_count; i++) {
        assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
                   &reservation_leases[i]) == 0);
        memset(&reservation_leases[i], 0, sizeof(reservation_leases[i]));
    }
    assert(app_node_comm_pending_delivery_count() == 1u);
    assert(app_node_comm_abandon_delivery(committed_handle) == 0);

    reset_fixture();
    {
        struct mesh_outbound occupied = reliable_uplink_envelope(244u);

        assert(app_node_comm_submit_reliable_uplink(
                   &occupied, 10000u, 814u, &committed_handle) == 0);
    }
    memset(blocked_leases, 0xa5, sizeof(blocked_leases));
    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0x9003u,
               reservation_count,
               blocked_leases,
               reservation_count) == -ENOSPC);
    for (size_t i = 0u; i < reservation_count; i++) {
        assert(blocked_leases[i].token == 0u);
    }
    assert(app_node_comm_abandon_delivery(committed_handle) == 0);
}

static void test_expired_durable_reservation_reclaims_all_five_slots(void)
{
    struct app_node_comm_reservation_lease stale[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    struct app_node_comm_reservation_lease fresh[
        APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    struct mesh_outbound sample = reliable_uplink_envelope(246u);
    struct mesh_outbound blocked = delivery_envelope(247u);
    struct k_work_delayable *reservation_expiry_work;
    uint32_t handle = 0u;
    const size_t count = sizeof(stale) / sizeof(stale[0]);

    reset_fixture();
    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0xa001u, count, stale, count) == 0);
    for (size_t i = 0u; i < count; i++) {
        assert(stale[i].token != 0u);
        assert(stale[i].owner_generation == 0xa001u);
        assert(stale[i].expires_at_ms == stale[0].expires_at_ms);
    }
    /* Reservation expiry shares the production owner-aware scheduler. */
    reservation_expiry_work = last_rescheduled_work;
    assert(reservation_expiry_work != NULL);
    assert(last_rescheduled_delay_ms == (int)stale[0].expires_at_ms);
    assert(app_node_comm_submit_delivery(
               &blocked, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               60000u, 247u, &handle) == -ENOSPC);

    atomic_store(&fake_now_ms, (int64_t)stale[0].expires_at_ms);
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        reservation_expiry_work->work.handler(
            &reservation_expiry_work->work);
    }
    /* The existing service path, rather than a second reservation table, reaps. */
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(app_node_comm_commit_durable_reliable_uplink_reservation(
               &stale[0], &sample, 60000u, 246u, &handle) == -ESTALE);
    assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
               &stale[1]) == -ESTALE);

    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0xa002u, count, fresh, count) == 0);
    for (size_t i = 0u; i < count; i++) {
        assert(fresh[i].token != 0u);
        assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
                   &fresh[i]) == 0);
    }
}

static void test_reservation_generation_rejects_old_owner_release(void)
{
    struct app_node_comm_reservation_lease old = {0};
    struct app_node_comm_reservation_lease successor = {0};
    struct app_node_comm_reservation_lease forged;

    reset_fixture();
    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0xa101u, 1u, &old, 1u) == 0);
    forged = old;
    forged.owner_generation++;
    assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
               &forged) == -ESTALE);
    assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
               &old) == 0);

    assert(app_node_comm_reserve_durable_reliable_uplinks(
               0xa102u, 1u, &successor, 1u) == 0);
    assert(successor.token != old.token ||
           successor.owner_generation != old.owner_generation);
    assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
               &old) == -ESTALE);
    assert(app_node_comm_cancel_durable_reliable_uplink_reservation(
               &successor) == 0);
}

static void test_command_response_reservation_survives_five_transport_owners(void)
{
    struct app_node_comm_reservation_lease command_response = {0};
    uint32_t transport_handles[APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY] = {0};
    uint32_t command_handle = 0u;

    reset_fixture();
    for (uint32_t i = 0u;
         i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY;
         i++) {
        struct mesh_outbound transport =
            reliable_uplink_envelope((uint16_t)(250u + i));

        assert(app_node_comm_submit_reliable_uplink(
                   &transport, 60000u, 250u + i,
                   &transport_handles[i]) == 0);
    }
    {
        struct mesh_outbound durable = reliable_uplink_envelope(260u);

        assert(app_node_comm_submit_delivery(
                   &durable, NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
                   60000u, 260u, &command_handle) == -ENOSPC);
    }
    assert(app_node_comm_reserve_protocol_response(
               0xa201u, &command_response) == 0);
    {
        struct mesh_outbound response = reliable_uplink_envelope(261u);

        assert(app_node_comm_commit_protocol_response(
                   &command_response, &response, 60000u, 261u,
                   &command_handle) == 0);
    }
    assert(app_node_comm_pending_delivery_count() ==
           APP_NODE_COMM_MAX_DELIVERIES);
    for (size_t i = 0u; i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY; i++) {
        assert(app_node_comm_abandon_delivery(transport_handles[i]) == 0);
    }
    assert(app_node_comm_abandon_delivery(command_handle) == 0);
}

static void test_durable_attempt_registry_dispatches_exact_owner(void)
{
    const struct app_node_comm_durable_attempt_ops secondary_ops = {
        .begin = secondary_durable_attempt_begin,
        .complete = secondary_durable_attempt_complete,
    };
    struct mesh_outbound envelope = reliable_uplink_envelope(245u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    assert(app_node_comm_register_durable_attempt_ops(&secondary_ops) == 0);
    assert(app_node_comm_register_durable_attempt_ops(&secondary_ops) ==
           -EALREADY);
    durable_attempt_begin_result = -ENOENT;
    durable_attempt_complete_result = -ENOENT;
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               10000u,
               815u,
               &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(durable_attempt_begin_calls == 1u);
    assert(secondary_durable_attempt_begin_calls == 1u);
    assert(durable_attempt_complete_calls == 1u);
    assert(secondary_durable_attempt_complete_calls == 1u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
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

static void test_auto_reap_protocol_response_returns_correlation_handle(void)
{
    struct mesh_outbound direct = reliable_uplink_envelope(220u);
    struct mesh_outbound reserved = reliable_uplink_envelope(221u);
    struct node_comm_terminal_event event;
    struct app_node_comm_reservation_lease reservation = {0};
    uint32_t handle = 0u;

    reset_fixture();
    try_uplink_confirmed[0] = true;
    assert(app_node_comm_submit_protocol_response_auto_reap(
               &direct, 10000u, 220u, &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));

    reset_fixture();
    assert(app_node_comm_reserve_protocol_response(0x221u, &reservation) ==
           0);
    try_uplink_confirmed[0] = true;
    handle = 0u;
    assert(app_node_comm_commit_protocol_response_auto_reap(
               &reservation,
               &reserved,
               10000u,
               221u,
               &handle) == 0);
    assert(handle != 0u);
    assert(app_node_comm_service_deliveries() == 0);
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(!app_node_comm_take_delivery_event_for(handle, &event));
}

static void test_adapter_trace_distinguishes_transport_and_semantic_proof(void)
{
    struct app_delivery_trace_capture capture = {0};
    struct mesh_outbound reliable = reliable_uplink_envelope(610u);
    struct mesh_outbound control = delivery_envelope(611u);
    struct node_comm_terminal_event terminal;
    uint32_t handle = 0u;
    bool saw_rf_start = false;
    bool saw_semantic_terminal = false;
    bool saw_transport_terminal = false;

    reset_fixture();
    assert(app_node_comm_set_delivery_transition_trace(
               capture_app_delivery_transition, &capture) == 0);
    assert(app_node_comm_submit_delivery(
               &reliable, NODE_COMM_PROFILE_RELIABLE_UPLINK,
               1000u, 610u, &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        assert(entry->event.target == FW_MACHINE_DELIVERY);
        assert(entry->event.operation_id == handle);
        assert(entry->event.generation != 0u);
        if (entry->event.type == FW_EVENT_RF_STARTED) {
            assert(entry->transition.old_state == FW_DELIVERY_WAIT_TX);
            assert(entry->transition.new_state == FW_DELIVERY_WAIT_ACK);
            saw_rf_start = true;
        }
        assert(!entry->terminal_present);
    }
    assert(saw_rf_start);
    assert(!app_node_comm_take_delivery_event_for(handle, &terminal));
    assert(note_gateway_confirmed_envelope(&reliable) == 0);
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        if (entry->terminal_present) {
            assert(entry->event.type == FW_EVENT_GATEWAY_ACK_RECEIVED);
            assert(entry->transition.old_state == FW_DELIVERY_WAIT_ACK);
            assert(entry->transition.new_state == FW_DELIVERY_DELIVERED);
            assert(entry->terminal.proof == NODE_COMM_TERMINAL_PROOF_SEMANTIC);
            saw_semantic_terminal = true;
        }
    }
    assert(saw_semantic_terminal);
    assert(app_node_comm_take_delivery_event_for(handle, &terminal));
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_SEMANTIC);

    reset_fixture();
    memset(&capture, 0, sizeof(capture));
    assert(app_node_comm_set_delivery_transition_trace(
               capture_app_delivery_transition, &capture) == 0);
    assert(app_node_comm_submit_delivery(
               &control, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1000u, 611u, &handle) == 0);
    for (uint64_t now_ms = 0u; now_ms < 160u; now_ms += 40u) {
        atomic_store(&fake_now_ms, (int64_t)now_ms);
        assert(app_node_comm_service_deliveries() == 0);
    }
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        if (entry->terminal_present) {
            assert(entry->event.type == FW_EVENT_GATEWAY_ACK_RECEIVED);
            assert(entry->terminal.proof ==
                   NODE_COMM_TERMINAL_PROOF_TRANSPORT);
            saw_transport_terminal = true;
        }
    }
    assert(saw_transport_terminal);
    assert(app_node_comm_take_delivery_event_for(handle, &terminal));
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_TRANSPORT);
}

static void
test_redrive_rejects_delayed_old_generation_rf_evidence(void)
{
    struct app_delivery_trace_capture capture = {0};
    struct mesh_outbound control = delivery_envelope(612u);
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_terminal_event terminal;
    uint32_t handle = 0u;
    uint32_t old_generation = 0u;
    uint32_t new_generation = 0u;
    uint8_t attempts_started = UINT8_MAX;
    bool saw_stale_rf_evidence = false;

    reset_fixture();
    assert(app_node_comm_set_delivery_transition_trace(
               capture_app_delivery_transition, &capture) == 0);
    assert(app_node_comm_submit_delivery(
               &control, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1000u, 612u, &handle) == 0);
    assert(app_node_comm_delivery_generation(handle, &old_generation) == 0);

    for (uint64_t now_ms = 0u; now_ms < 160u; now_ms += 40u) {
        atomic_store(&fake_now_ms, (int64_t)now_ms);
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(try_flood_calls == 4u);
    assert(app_node_comm_redrive_delivered_control(
               handle, 200u, 1000u, &prior_terminal) == 0);
    assert(prior_terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(prior_terminal.attempts_started == 4u);
    assert(app_node_comm_delivery_generation(handle, &new_generation) == 0);
    assert(new_generation != old_generation);

    /*
     * This callback belongs to the completed flood. It must trace as stale
     * evidence, leave the fresh generation in WAIT_TX, and never spend one
     * of its four physical-copy attempts.
     */
    assert(app_node_comm_note_backend_rf_started_for_generation(
               &control.packet, old_generation) == -ESTALE);
    assert(app_node_comm_peek_delivery_attempts_started(
               handle, &attempts_started) == 0);
    assert(attempts_started == 0u);
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        if (entry->event.type == FW_EVENT_RF_STARTED &&
            entry->event.operation_id == handle &&
            entry->event.generation == old_generation) {
            assert(!entry->terminal_present);
            saw_stale_rf_evidence = true;
        }
    }
    assert(saw_stale_rf_evidence);

    for (uint64_t now_ms = 200u; now_ms < 360u; now_ms += 40u) {
        atomic_store(&fake_now_ms, (int64_t)now_ms);
        assert(app_node_comm_service_deliveries() == 0);
    }
    assert(try_flood_calls == 8u);
    assert(app_node_comm_take_delivery_event_for(handle, &terminal));
    assert(terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_TRANSPORT);
    assert(terminal.attempts_started == 4u);
}

static void test_delivered_control_supports_four_same_handle_redrives(void)
{
    struct mesh_outbound control = delivery_envelope(613u);
    struct node_comm_terminal_event prior_terminal;
    struct node_comm_terminal_event terminal;
    uint32_t handle = 0u;
    uint32_t prior_generation = 0u;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &control, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               2000u, 613u, &handle) == 0);
    assert(app_node_comm_delivery_generation(handle, &prior_generation) == 0);

    for (uint32_t wave = 0u; wave <= 4u; wave++) {
        const uint64_t wave_start_ms = (uint64_t)wave * 200u;

        for (uint64_t now_ms = wave_start_ms;
             now_ms < wave_start_ms + 160u;
             now_ms += 40u) {
            atomic_store(&fake_now_ms, (int64_t)now_ms);
            assert(app_node_comm_service_deliveries() == 0);
        }
        assert(try_flood_calls == (wave + 1u) * 4u);

        if (wave < 4u) {
            uint32_t next_generation = 0u;

            assert(app_node_comm_redrive_delivered_control(
                       handle,
                       (uint64_t)(wave + 1u) * 200u,
                       2000u,
                       &prior_terminal) == 0);
            assert(prior_terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
            assert(prior_terminal.attempts_started == 4u);
            assert(app_node_comm_delivery_generation(
                       handle, &next_generation) == 0);
            assert(next_generation != prior_generation);
            prior_generation = next_generation;
        }
    }

    assert(app_node_comm_take_delivery_event_for(handle, &terminal));
    assert(terminal.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(terminal.proof == NODE_COMM_TERMINAL_PROOF_TRANSPORT);
    assert(terminal.attempts_started == 4u);
}

static void test_production_six_slot_exhaustion_keeps_unique_trace_owners(void)
{
    struct app_delivery_trace_capture capture = {0};
    struct node_comm_terminal_event terminal;
    struct mesh_outbound overflow = delivery_envelope(999u);
    uint32_t handles[APP_NODE_COMM_MAX_DELIVERIES] = {0};
    uint32_t extra = 0u;

    reset_fixture();
    assert(app_node_comm_set_delivery_transition_trace(
               capture_app_delivery_transition, &capture) == 0);
    for (uint32_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        struct mesh_outbound envelope =
            i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY ?
                delivery_envelope((uint16_t)(620u + i)) :
                reliable_uplink_envelope((uint16_t)(620u + i));
        int ret;

        ret = i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY ?
            app_node_comm_submit_delivery(
                &envelope, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
                1000u, 620u + i, &handles[i]) :
            app_node_comm_submit_protocol_response(
                &envelope, 1000u, 620u + i, &handles[i]);
        assert(ret == 0);
        assert(handles[i] != 0u);
        for (uint32_t prior = 0u; prior < i; prior++) {
            assert(handles[prior] != handles[i]);
        }
    }
    assert(app_node_comm_pending_delivery_count() ==
           APP_NODE_COMM_MAX_DELIVERIES);
    assert(capture.count == APP_NODE_COMM_MAX_DELIVERIES);
    for (size_t i = 0u; i < capture.count; i++) {
        const struct app_delivery_trace_entry *entry = &capture.entries[i];

        assert(entry->event.type == FW_EVENT_PACKET_OWNED);
        assert(entry->event.operation_id == handles[i]);
        assert(entry->event.generation != 0u);
        assert(entry->transition.old_state == FW_DELIVERY_EMPTY);
        assert(entry->transition.new_state == FW_DELIVERY_WAIT_TX);
        assert(!entry->terminal_present);
        for (size_t prior = 0u; prior < i; prior++) {
            assert(capture.entries[prior].event.operation_id !=
                   entry->event.operation_id);
            assert(capture.entries[prior].event.generation !=
                   entry->event.generation);
        }
    }
    assert(app_node_comm_submit_delivery(
               &overflow, NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
               1000u, 999u, &extra) == -ENOSPC);
    for (uint32_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        assert(app_node_comm_cancel_delivery(handles[i]) == 0);
        assert(app_node_comm_take_delivery_event_for(handles[i], &terminal));
        assert(terminal.reason == NODE_COMM_TERMINAL_CANCELLED);
    }
    assert(app_node_comm_pending_delivery_count() == 0u);
}

static void test_delivery_rejects_unbounded_or_over_capacity_work(void)
{
    struct mesh_outbound envelope = delivery_envelope(12u);
    struct mesh_outbound protocol = reliable_uplink_envelope(99u);
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
    envelope.payload_len = APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN + 1u;
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
    for (uint32_t i = 0u;
         i < APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY;
         i++) {
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
    assert(app_node_comm_submit_protocol_response(
               &protocol, 1000u, 201u,
               &handles[APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY]) == 0);
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

static void test_survey_rf_retry_identity_sweep_diversifies_and_caps(void)
{
    static const uint32_t expected_base_ms[] = {
        200u, 400u, 800u, 1600u, 1600u, 1600u, 1600u, 1600u,
    };
    uint32_t previous_survey_delay_ms = 0u;
    uint32_t changed_survey_delays = 0u;

    for (uint32_t survey_index = 0u; survey_index < 256u; survey_index++) {
        uint32_t schedules[50u][sizeof(expected_base_ms) /
                                sizeof(expected_base_ms[0])] = {{0}};
        uint32_t survey_id = UINT32_C(0x50665000) + survey_index + 1u;
        bool opportunity_diversified = false;

        for (uint32_t anchor = 0u; anchor < 50u; anchor++) {
            uint64_t anchor_id = UINT64_C(0xa002000000010000) + anchor;

            for (uint16_t round = 1u;
                 round <= sizeof(expected_base_ms) /
                              sizeof(expected_base_ms[0]);
                 round++) {
                uint32_t base_ms = expected_base_ms[round - 1u];
                uint32_t delay_ms = 0u;

                assert(app_node_comm_retry_identity_backoff_ms(
                           anchor_id,
                           survey_id,
                           (uint32_t)MSG_SURVEY_DISCOVERY_START << 16,
                           NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
                           round,
                           &delay_ms) == 0);
                assert(delay_ms >= base_ms / 2u);
                assert(delay_ms <= base_ms + base_ms / 2u);
                schedules[anchor][round - 1u] = delay_ms;
                if (anchor == 0u) {
                    uint32_t next_opportunity_delay_ms = 0u;

                    assert(app_node_comm_retry_identity_backoff_ms(
                               anchor_id,
                               survey_id,
                               ((uint32_t)MSG_SURVEY_DISCOVERY_START << 16) |
                                   1u,
                               NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
                               round,
                               &next_opportunity_delay_ms) == 0);
                    opportunity_diversified |=
                        next_opportunity_delay_ms != delay_ms;
                }
            }
        }

        assert(opportunity_diversified);

        for (uint32_t first = 0u; first < 50u; first++) {
            for (uint32_t second = first + 1u; second < 50u; second++) {
                bool remained_synchronized = true;

                for (size_t round = 0u;
                     round < sizeof(expected_base_ms) /
                                 sizeof(expected_base_ms[0]);
                     round++) {
                    if (schedules[first][round] != schedules[second][round]) {
                        remained_synchronized = false;
                        break;
                    }
                }
                assert(!remained_synchronized);
            }
        }

        if (survey_index > 0u &&
            schedules[0][0] != previous_survey_delay_ms) {
            changed_survey_delays++;
        }
        previous_survey_delay_ms = schedules[0][0];
    }

    assert(changed_survey_delays >= 100u);
    assert(app_node_comm_retry_identity_backoff_ms(
               0u, 1u, 1u,
               NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
               1u, &(uint32_t){0}) == -EINVAL);
    assert(app_node_comm_retry_identity_backoff_ms(
               1u, 0u, 1u,
               NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
               1u, &(uint32_t){0}) == -EINVAL);
}

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
static void test_transmitter_freezes_five_full_payloads_with_protocol_reserve(void)
{
    struct mesh_outbound envelopes[5u];
    struct mesh_outbound blocked = reliable_uplink_envelope(140u);
    struct mesh_outbound protocol = reliable_uplink_envelope(141u);
    struct node_comm_terminal_event event;
    uint8_t expected[5u][900u];
    uint32_t handles[5u];
    uint32_t blocked_handle = 0u;
    uint32_t protocol_handle;

    assert(APP_NODE_COMM_MAX_DELIVERIES == 6u);
    assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ==
           PACKET_EXT_MAX_PAYLOAD_LEN);
    reset_fixture();
    for (size_t request = 0u; request < 5u; request++) {
        envelopes[request] = reliable_uplink_envelope(
            (uint16_t)(129u + request));
        for (size_t byte = 0u; byte < sizeof(expected[request]); byte++) {
            expected[request][byte] =
                (uint8_t)(byte ^ (byte >> 8) ^ (request * 37u));
        }
        memcpy(envelopes[request].payload,
               expected[request],
               sizeof(expected[request]));
        envelopes[request].payload_len =
            (uint16_t)sizeof(expected[request]);
        envelopes[request].packet.payload_len =
            (uint16_t)sizeof(expected[request]);
        assert(app_node_comm_submit_reliable_uplink(
                   &envelopes[request],
                   60000u,
                   (uint32_t)(129u + request),
                   &handles[request]) == 0);
    }

    assert(app_node_comm_submit_reliable_uplink(
               &blocked, 60000u, 140u, &blocked_handle) == -ENOSPC);
    assert(blocked_handle == 0u);
    assert(app_node_comm_submit_protocol_response(
               &protocol, 60000u, 141u, &protocol_handle) == 0);
    assert(app_node_comm_pending_delivery_count() == 6u);

    for (size_t request = 0u; request < 5u; request++) {
        memset(envelopes[request].payload,
               0,
               sizeof(expected[request]));
        envelopes[request].packet.seq++;
    }
    memset(&protocol, 0, sizeof(protocol));
    memset(try_uplink_confirmed, 1, 6u * sizeof(try_uplink_confirmed[0]));

    /* Protocol priority consumes the reserved sixth slot before source work. */
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_envelopes[0].packet.seq == 141u);
    assert(app_node_comm_take_delivery_event_for(protocol_handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);

    for (size_t request = 0u; request < 5u; request++) {
        assert(app_node_comm_service_deliveries() == 0);
        assert(try_uplink_envelopes[request + 1u].payload_len ==
               sizeof(expected[request]));
        assert(memcmp(try_uplink_envelopes[request + 1u].payload,
                      expected[request],
                      sizeof(expected[request])) == 0);
        assert(try_uplink_envelopes[request + 1u].packet.seq ==
               (uint16_t)(129u + request));
        assert(app_node_comm_take_delivery_event_for(handles[request],
                                                     &event));
        assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
        assert(event.attempts_started == 1u);
    }
    assert(try_uplink_calls == 6u);
    assert(app_node_comm_pending_delivery_count() == 0u);
}
#endif

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
    assert(durable_attempt_begin_calls == 1u);
    assert(durable_attempt_complete_calls == 1u);
    assert(durable_attempt_complete_tokens[0] != 0u);
    assert(durable_attempt_complete_rf_started[0]);
    assert(memcmp(&durable_attempt_begin_packets[0],
                  &envelope.packet,
                  sizeof(envelope.packet)) == 0);
    assert(memcmp(&durable_attempt_complete_packets[0],
                  &envelope.packet,
                  sizeof(envelope.packet)) == 0);
}

static void test_durable_uplink_budget_exhaustion_keeps_terminal_reason(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(131u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;

    reset_fixture();
    durable_attempt_begin_result = -ETIMEDOUT;
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               5000u,
               131u,
               &handle) == 0);
    assert(app_node_comm_service_deliveries() == -ETIMEDOUT);
    assert(try_uplink_calls == 0u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED);
    assert(event.attempts_started == 0u);
}

static void
test_durable_pre_rf_completion_failure_retries_exact_token_before_new_rf(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(132u);
    struct node_comm_terminal_event event;
    uint32_t retry_delay_ms = 0u;
    uint32_t handle = 0u;
    uint8_t attempts_started = UINT8_MAX;
    uint8_t first_token;

    reset_fixture();
    try_uplink_results[0] = -EBUSY;
    try_uplink_sent[0] = false;
    durable_attempt_complete_failures_remaining = 2u;
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               5000u,
               132u,
               &handle) == 0);

    assert(app_node_comm_service_deliveries() == -EIO);
    assert(try_uplink_calls == 1u);
    assert(durable_attempt_begin_calls == 1u);
    assert(durable_attempt_complete_calls == 1u);
    first_token = durable_attempt_complete_tokens[0];
    assert(first_token != 0u);
    assert(!durable_attempt_complete_rf_started[0]);
    assert(app_node_comm_delivery_attempts_started(
               handle, &attempts_started) == 0);
    assert(attempts_started == 0u);

    /* Repeated failures retain the same pre-RF refund identity. */
    assert(app_node_comm_service_deliveries() == -EIO);
    assert(durable_attempt_complete_calls == 2u);
    assert(durable_attempt_complete_tokens[1] == first_token);
    assert(!durable_attempt_complete_rf_started[1]);
    assert(durable_attempt_begin_calls == 1u);
    assert(try_uplink_calls == 1u);

    /*
     * Any service call must first persist the exact failed refund.  Once that
     * succeeds, the scheduler is still inside its pre-RF backoff, so no new
     * token or radio call is allowed yet.
     */
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(durable_attempt_complete_calls == 3u);
    assert(durable_attempt_complete_tokens[2] == first_token);
    assert(!durable_attempt_complete_rf_started[2]);
    assert(durable_attempt_begin_calls == 1u);
    assert(try_uplink_calls == 1u);

    assert(app_node_comm_retry_backoff_ms(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               1u,
               &retry_delay_ms) == 0);
    atomic_store(&fake_now_ms, retry_delay_ms);
    try_uplink_confirmed[1] = true;
    assert(app_node_comm_service_deliveries() == 0);
    assert(durable_attempt_begin_calls == 2u);
    assert(durable_attempt_complete_calls == 4u);
    assert(durable_attempt_complete_tokens[3] != first_token);
    assert(durable_attempt_complete_rf_started[3]);
    assert(try_uplink_calls == 2u);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

static void
test_durable_external_pre_rf_completion_failure_retains_exact_token(void)
{
    struct mesh_outbound envelope = reliable_uplink_envelope(133u);
    struct node_comm_terminal_event event;
    uint32_t handle = 0u;
    uint8_t first_external_token;

    reset_fixture();
    assert(app_node_comm_submit_delivery(
               &envelope,
               NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
               5000u,
               133u,
               &handle) == 0);
    assert(app_node_comm_service_deliveries() == 0);
    assert(durable_attempt_begin_calls == 1u);
    assert(durable_attempt_complete_calls == 1u);
    assert(durable_attempt_complete_rf_started[0]);

    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
    assert(durable_attempt_begin_calls == 2u);
    first_external_token = durable_attempt_next_token;
    durable_attempt_complete_failures_remaining = 2u;
    assert(app_node_comm_complete_backend_attempt(
               &envelope.packet, false) == -EIO);
    assert(durable_attempt_complete_calls == 2u);
    assert(durable_attempt_complete_tokens[1] == first_external_token);
    assert(!durable_attempt_complete_rf_started[1]);

    /*
     * Even a repeated completion call with conflicting input must retry the
     * retained refund exactly, rather than changing its RF-start ownership.
     */
    assert(app_node_comm_complete_backend_attempt(
               &envelope.packet, true) == -EIO);
    assert(durable_attempt_complete_calls == 3u);
    assert(durable_attempt_complete_tokens[2] == first_external_token);
    assert(!durable_attempt_complete_rf_started[2]);
    assert(durable_attempt_begin_calls == 2u);

    /*
     * Preflight owns the retry barrier: it commits the same refund token
     * before allocating another backend attempt.
     */
    assert(app_node_comm_backend_retry_preflight(&envelope.packet) == 0);
    assert(durable_attempt_complete_calls == 4u);
    assert(durable_attempt_complete_tokens[3] == first_external_token);
    assert(!durable_attempt_complete_rf_started[3]);
    assert(durable_attempt_begin_calls == 3u);
    assert(app_node_comm_complete_backend_attempt(
               &envelope.packet, false) == 0);
    assert(durable_attempt_complete_calls == 5u);
    assert(durable_attempt_complete_tokens[4] != first_external_token);
    assert(!durable_attempt_complete_rf_started[4]);

    assert(note_gateway_confirmed_envelope(&envelope) == 0);
    assert(app_node_comm_take_delivery_event_for(handle, &event));
    assert(event.reason == NODE_COMM_TERMINAL_DELIVERED);
    assert(event.attempts_started == 1u);
}

int main(void)
{
    test_production_init_installs_delivery_transition_trace();
    test_survey_transport_preempt_preserves_custody_and_exact_abort_owner();
    test_survey_transport_preempt_cannot_steal_lifecycle_pause();
    test_survey_transport_preempt_accepts_atomic_result_reservation();
    test_peek_attempt_count_does_not_service_delivery_policy();
    test_control_flood_freezes_age_before_wake_work();
    test_pause_and_stop_gate_new_submissions_without_clearing_queue();
    test_safe_boundary_and_expired_pause_force_reclaim();
    test_idle_pause_expiry_recovers_without_an_api_poll();
    test_pause_watchdog_schedule_failure_stops_feeding();
    test_recovery_watchdog_retry_failure_stops_feeding();
    test_stuck_pause_expiry_escalates_once_without_polling_forever();
    test_pause_completes_while_backend_send_is_blocked();
    test_stop_completes_while_backend_send_is_blocked();
    test_pause_expiry_preserves_full_64_bit_uptime();
    test_send_stays_closed_until_backend_resume_is_ready();
    test_delivery_copies_envelope_and_wakes_once_per_flood();
    test_assignment_sized_control_payload_admission();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        test_gateway_large_control_retries_exact_fifty_anchor_payload();
        test_gateway_large_control_single_owner_and_cancel_release();
        test_gateway_large_control_owner_survives_active_backend_cancel();
    }
    test_initial_delivery_schedule_failure_rolls_back_before_acceptance();
    test_positive_work_schedule_result_is_normalized_to_acceptance();
    test_durable_completion_retry_does_not_delay_earlier_policy_work();
    test_delivery_schedule_uses_role_queue_path();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        test_gateway_due_kick_aborts_active_scan_at_safe_boundary();
        test_gateway_scan_boundary_retry_failure_retains_gate_and_fails_closed();
        test_gateway_due_kick_recovers_missed_scan_boundary_callback();
        test_gateway_due_kick_missed_boundary_stress();
        test_gateway_due_kick_retries_behind_host_priority();
        test_gateway_due_kick_retries_transient_route_queue_failure();
        test_gateway_due_gate_cancellation_releases_scan_without_rf();
        test_gateway_scan_restart_schedule_failure_stops_watchdog_feeding();
        test_gateway_due_gate_pause_and_stop_clear_without_rf();
    }
    test_delivery_pre_rf_busy_defers_without_consuming_attempts();
    test_auto_reaped_control_flood_retries_without_leaking_handle();
    test_best_effort_uplink_sends_once_without_reliable_custody();
    test_gateway_control_flood_preempts_queued_control_response();
    test_control_response_queued_during_active_control_runs_next();
    test_replayed_gateway_ack_coalesces_by_acknowledged_identity();
    test_control_response_busy_defers_without_spending_attempt();
    test_control_response_retry_exhaustion_is_terminal_and_reaped();
    test_control_response_phy_errors_consume_four_real_attempts();
    test_control_response_pre_rf_phy_block_does_not_spend_attempt();
    test_gateway_batch_ack_retry_keeps_exact_identity_and_one_terminal();
    test_control_response_terminal_records_do_not_leak_capacity();
    test_control_response_txfrs_before_deadline_beats_service_race();
    test_control_response_txfrs_at_or_after_deadline_loses();
    test_fourth_bounded_copy_before_deadline_beats_service_race();
    test_reliable_ack_timestamp_controls_deadline_race();
    test_reliable_uplink_waits_for_exact_gateway_confirmation();
    test_four_durable_pair_results_survive_long_single_flight_confirmation_outage();
    test_reliable_targets_track_only_backend_profiles();
    test_durable_survey_submit_is_async_and_exact_under_pressure_pause();
    test_reliable_uplink_waits_for_scheduled_channel9_boundary();
    test_reliable_backend_retries_are_observed_without_being_capped();
    test_terminal_race_after_preflight_cannot_restart_backend();
    test_backend_false_start_completion_releases_attempt_lease();
    test_backend_attempt_completion_gates_cancel_and_auto_reap();
    test_outer_backend_cancel_waits_for_active_call_to_return();
    test_external_gateway_proof_during_pre_rf_callback_resolves_after_return();
    test_external_gateway_proof_recovery_rejects_wrong_identity();
    test_foreign_workqueue_cancel_waits_for_exact_route_owned_completion();
    test_reliable_uplink_synchronous_and_late_confirmations_are_bounded();
    test_terminal_waits_for_confirmed_backend_release_after_cancel_failure();
    test_serialized_backend_cancel_contention_does_not_spend_hard_failures();
    test_serialized_backend_cancel_contention_is_bounded();
    test_caller_owned_terminal_event_has_bounded_watchdog_grace();
    test_unpublished_external_backend_attempt_fails_closed();
    test_reliable_uplink_failure_preserves_reason_and_releases_owner();
    test_adapter_resource_wait_uses_exact_core_owner_trace();
    test_single_flight_wait_preserves_priority_and_fifo();
    test_cancelling_reliable_uplink_releases_backend_owner();
    test_protocol_response_preempts_ordinary_reliable_uplink();
    test_protocol_capacity_is_reserved_and_abandonment_reaps_slots();
    test_protocol_response_reservation_survives_capacity_exhaustion();
    test_bounded_control_reservation_obeys_ordinary_capacity();
    test_bounded_control_reservation_commit_is_exact_and_consuming();
    test_bounded_control_reservation_coalesces_and_rejects_wrong_owners();
    test_durable_burst_reservation_is_atomic_and_retains_protocol_slot();
    test_expired_durable_reservation_reclaims_all_five_slots();
    test_reservation_generation_rejects_old_owner_release();
    test_command_response_reservation_survives_five_transport_owners();
    test_durable_attempt_registry_dispatches_exact_owner();
    test_fire_and_forget_reliable_uplinks_reap_terminal_capacity();
    test_fire_and_forget_protocol_responses_reap_terminal_capacity();
    test_auto_reap_protocol_response_returns_correlation_handle();
    test_adapter_trace_distinguishes_transport_and_semantic_proof();
    test_redrive_rejects_delayed_old_generation_rf_evidence();
    test_delivered_control_supports_four_same_handle_redrives();
    test_production_six_slot_exhaustion_keeps_unique_trace_owners();
    test_delivery_rejects_unbounded_or_over_capacity_work();
    test_delivery_queue_survives_stop_and_restart();
    test_inflight_delivery_is_accounted_before_preserving_stop();
    test_retry_backoff_hashes_complete_packet_identity();
    test_survey_rf_retry_identity_sweep_diversifies_and_caps();
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    test_transmitter_freezes_five_full_payloads_with_protocol_reserve();
#endif
    test_durable_uplink_uses_shared_reliable_backend();
    test_durable_uplink_budget_exhaustion_keeps_terminal_reason();
    test_durable_pre_rf_completion_failure_retries_exact_token_before_new_rf();
    test_durable_external_pre_rf_completion_failure_retains_exact_token();
    return 0;
}
