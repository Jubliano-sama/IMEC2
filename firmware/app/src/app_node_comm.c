#include "app_node_comm.h"

#include "app_mesh_report.h"
#include "app_node_comm_gateway_route_refresh.h"
#include "app_node_comm_sync.h"
#include "app_state.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "node_comm.h"

#include <zephyr/kernel.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

struct app_node_comm_delivery_record {
    struct proto_packet packet;
    uint8_t payload[APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
    uint32_t handle;
    uint8_t flood_retry_count;
    bool occupied;
};

static struct node_comm node_comm_policy;
static struct app_node_comm_delivery_record
    node_comm_delivery_records[APP_NODE_COMM_MAX_DELIVERIES];
static struct k_work_delayable node_comm_lifecycle_watchdog_work;
static struct k_work_delayable node_comm_delivery_work;
static uint64_t node_comm_lifecycle_recovery_deadline_ms;
static bool node_comm_backend_ready;
static bool node_comm_delivery_backend_active;

#define NODE_COMM_LIFECYCLE_RECOVERY_POLL_MS 10u
#define NODE_COMM_LIFECYCLE_RECOVERY_TIMEOUT_MS 1000u
#define NODE_COMM_PRE_RF_RETRY_MS 10u

_Static_assert(sizeof(node_comm_delivery_records) /
                   sizeof(node_comm_delivery_records[0]) ==
                   APP_NODE_COMM_MAX_DELIVERIES,
               "node communication delivery capacity changed");
_Static_assert(APP_NODE_COMM_MAX_DELIVERIES <= NODE_COMM_MAX_REQUESTS,
               "facade delivery capacity exceeds scheduler capacity");
_Static_assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN <=
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "frozen delivery payload exceeds mesh envelope capacity");
_Static_assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN == 192u,
               "generic frozen payload bound changed");

static uint64_t app_node_comm_now_ms(void)
{
    return (uint64_t)k_uptime_get();
}

static bool app_node_comm_transport_quiesced(void)
{
    return mesh_transport_quiesced() &&
           !radio_guard_uwb_busy() && !mesh_rx_response_active() &&
           !dwm3000_driver_receive_abort_pending();
}

static bool app_node_comm_lifecycle_service_locked(void);
static void app_node_comm_schedule_delivery_locked(uint64_t now_ms);

static struct app_node_comm_delivery_record *
app_node_comm_delivery_record_for_handle(uint32_t handle)
{
    if (handle == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        if (node_comm_delivery_records[i].occupied &&
            node_comm_delivery_records[i].handle == handle) {
            return &node_comm_delivery_records[i];
        }
    }
    return NULL;
}

static struct app_node_comm_delivery_record *
app_node_comm_free_delivery_record(void)
{
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        if (!node_comm_delivery_records[i].occupied) {
            return &node_comm_delivery_records[i];
        }
    }
    return NULL;
}

static void app_node_comm_clear_delivery_record(uint32_t handle)
{
    struct app_node_comm_delivery_record *record =
        app_node_comm_delivery_record_for_handle(handle);

    if (record != NULL) {
        memset(record, 0, sizeof(*record));
    }
}

static int app_node_comm_freeze_delivery(
    struct app_node_comm_delivery_record *record,
    const app_node_comm_envelope *envelope,
    uint32_t handle)
{
    if (record == NULL || envelope == NULL || handle == 0u ||
        envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        envelope->packet.payload_len != envelope->payload_len) {
        return envelope != NULL &&
               envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ?
               -EMSGSIZE : -EINVAL;
    }
    memset(record, 0, sizeof(*record));
    record->packet = envelope->packet;
    memcpy(record->payload, envelope->payload, envelope->payload_len);
    record->payload_len = envelope->payload_len;
    record->radio_channel = envelope->radio_channel;
    record->next_hop_id = envelope->next_hop_id;
    record->queued_at_ms = envelope->queued_at_ms;
    record->earliest_tx_ms = envelope->earliest_tx_ms;
    record->flood_retry_count = envelope->flood_retry_count;
    record->handle = handle;
    record->occupied = true;
    return 0;
}

static bool app_node_comm_backend_error_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -EWOULDBLOCK ||
           ret == -EINPROGRESS || ret == -EHOSTUNREACH ||
           ret == -ENETUNREACH || ret == -ENOTCONN || ret == -ETIMEDOUT;
}

static uint32_t app_node_comm_retry_jitter_seed(
    const app_node_comm_envelope *envelope)
{
    uint32_t seed = UINT32_C(0x811c9dc5);
    const uint32_t identity_words[] = {
        (uint32_t)envelope->packet.src_id,
        (uint32_t)(envelope->packet.src_id >> 32),
        (uint32_t)envelope->packet.dst_id,
        (uint32_t)(envelope->packet.dst_id >> 32),
        envelope->packet.session_id,
        envelope->packet.seq,
        envelope->packet.msg_type,
    };

    for (size_t i = 0u;
         i < sizeof(identity_words) / sizeof(identity_words[0]); i++) {
        seed ^= identity_words[i];
        seed *= UINT32_C(0x01000193);
    }
    return seed == 0u ? UINT32_C(0x6d2b79f5) : seed;
}

static void app_node_comm_delivery_work_handler(struct k_work *work)
{
    (void)work;
    (void)app_node_comm_service_deliveries();
}

static void app_node_comm_schedule_delivery_locked(uint64_t now_ms)
{
    uint64_t delay_ms;
    uint64_t due_ms;

    if (!node_comm_backend_ready || node_comm_delivery_backend_active ||
        node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_next_service_due_ms(&node_comm_policy, now_ms, &due_ms)) {
        return;
    }
    delay_ms = due_ms > now_ms ? due_ms - now_ms : 0u;
    if (delay_ms > INT64_MAX) {
        delay_ms = INT64_MAX;
    }
    (void)k_work_reschedule(&node_comm_delivery_work,
                            K_MSEC((int64_t)delay_ms));
}

static void app_node_comm_begin_recovery(void)
{
    uint32_t now_ms = (uint32_t)app_node_comm_now_ms();

    (void)k_work_cancel_delayable(&node_comm_delivery_work);
    app_node_comm_gateway_route_refresh_pause(now_ms);
    (void)mesh_transport_pause_preserving_queued();
    if (!app_node_comm_transport_quiesced()) {
        dwm3000_driver_request_receive_abort();
    }
    (void)k_work_reschedule(&node_comm_lifecycle_watchdog_work, K_NO_WAIT);
}

static void app_node_comm_lifecycle_watchdog_handler(struct k_work *work)
{
    struct node_comm_pause_lease recovery_lease;
    bool recovery_started;
    bool have_recovery_lease;
    bool recovery_expired;

    (void)work;
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    recovery_started = app_node_comm_lifecycle_service_locked();
    have_recovery_lease = node_comm_pause_recovery_lease(
        &node_comm_policy, &recovery_lease);
    recovery_expired = have_recovery_lease &&
        app_node_comm_now_ms() >= node_comm_lifecycle_recovery_deadline_ms;
    app_node_comm_sync_unlock();

    if (recovery_started) {
        app_node_comm_begin_recovery();
    }
    if (!have_recovery_lease) {
        return;
    }
    if (app_node_comm_transport_quiesced()) {
        (void)app_node_comm_resume_complete(&recovery_lease);
        return;
    }
    if (recovery_expired) {
        /* The bounded RF owner ignored abort; force a clean watchdog reset. */
        app_watchdog_stop_feeding();
        return;
    }
    (void)k_work_reschedule(&node_comm_lifecycle_watchdog_work,
                            K_MSEC(NODE_COMM_LIFECYCLE_RECOVERY_POLL_MS));
}

static bool app_node_comm_lifecycle_service_locked(void)
{
    struct node_comm_pause_lease recovery_lease;
    bool recovery_before = node_comm_pause_recovery_lease(
        &node_comm_policy, &recovery_lease);
    uint64_t now_ms = app_node_comm_now_ms();

    (void)node_comm_service(&node_comm_policy, now_ms);
    app_node_comm_schedule_delivery_locked(now_ms);
    if (!recovery_before && node_comm_pause_recovery_lease(
            &node_comm_policy, &recovery_lease)) {
        node_comm_backend_ready = false;
        node_comm_lifecycle_recovery_deadline_ms = now_ms +
            NODE_COMM_LIFECYCLE_RECOVERY_TIMEOUT_MS;
        return true;
    }
    return false;
}

void app_node_comm_lifecycle_service(void)
{
    struct node_comm_pause_lease recovery_lease;
    bool recovery_started = false;
    bool have_recovery_lease = false;

    if (app_node_comm_sync_lock() == 0) {
        recovery_started = app_node_comm_lifecycle_service_locked();
        app_node_comm_sync_unlock();
    }
    if (recovery_started) {
        app_node_comm_begin_recovery();
        if (app_node_comm_transport_quiesced() &&
            app_node_comm_sync_lock() == 0) {
            have_recovery_lease = node_comm_pause_recovery_lease(
                &node_comm_policy, &recovery_lease);
            app_node_comm_sync_unlock();
        }
        if (have_recovery_lease) {
            (void)app_node_comm_resume_complete(&recovery_lease);
        }
    }
}

static int app_node_comm_require_running(void)
{
    bool recovery_started;
    int ret;

    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    recovery_started = app_node_comm_lifecycle_service_locked();
    ret = node_comm_state(&node_comm_policy) == NODE_COMM_RUNNING ?
          (node_comm_backend_ready ? 0 : -ESHUTDOWN) : -ESHUTDOWN;
    app_node_comm_sync_unlock();
    if (recovery_started) {
        app_node_comm_begin_recovery();
    }
    return ret;
}

_Static_assert((int)APP_NODE_COMM_CONTROL_WAKE_IF_NEEDED ==
                   (int)MESH_C5_CONTROL_WAKE_IF_NEEDED,
               "node communication wake control mode changed");
_Static_assert((int)APP_NODE_COMM_CONTROL_ACCEPTED_EXCHANGE ==
                   (int)MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
               "node communication accepted-exchange mode changed");

int app_node_comm_init(const app_node_comm_callbacks *callbacks)
{
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }

    node_comm_init(&node_comm_policy);
    memset(node_comm_delivery_records, 0, sizeof(node_comm_delivery_records));
    node_comm_backend_ready = false;
    node_comm_delivery_backend_active = false;
    node_comm_lifecycle_recovery_deadline_ms = 0u;
    k_work_init_delayable(&node_comm_lifecycle_watchdog_work,
                          app_node_comm_lifecycle_watchdog_handler);
    k_work_init_delayable(&node_comm_delivery_work,
                          app_node_comm_delivery_work_handler);
    ret = node_comm_start(&node_comm_policy, app_node_comm_now_ms());
    if (ret < 0) {
        app_node_comm_sync_unlock();
        return ret;
    }
    app_node_comm_sync_unlock();
    ret = app_mesh_report_init(callbacks);
    if (app_node_comm_sync_lock() < 0) {
        return -EWOULDBLOCK;
    }
    if (ret < 0) {
        (void)node_comm_stop(&node_comm_policy,
                             NODE_COMM_STOP_CANCEL_ALL,
                             app_node_comm_now_ms());
    } else {
        node_comm_backend_ready = true;
    }
    app_node_comm_sync_unlock();
    return ret;
}

void app_node_comm_stop_role_scan(void)
{
    if (app_node_comm_require_running() < 0) {
        return;
    }
    mesh_stop_role_scan();
}

void app_node_comm_restart_role_scan(void)
{
    if (app_node_comm_require_running() < 0) {
        return;
    }
    mesh_restart_role_scan();
}

int app_node_comm_send(const app_node_comm_envelope *envelope,
                       const char *reason)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return mesh_send_outbound(envelope, reason);
}

int app_node_comm_send_control(
    const app_node_comm_envelope *envelope,
    uint8_t purpose,
    enum app_node_comm_control_send_mode mode,
    const char *reason)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return mesh_send_c5_control(envelope,
                                purpose,
                                (enum mesh_c5_control_send_mode)mode,
                                reason);
}

int app_node_comm_send_control_flood(const app_node_comm_envelope *envelope,
                                     uint8_t purpose,
                                     const char *reason,
                                     bool *sent_now)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        if (sent_now != NULL) {
            *sent_now = false;
        }
        return ret;
    }
    return mesh_send_c5_flood(envelope, purpose, reason, sent_now);
}

int app_node_comm_request_path(uint64_t target_id, const char *reason)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return mesh_request_route(target_id, reason);
}

int app_node_comm_start_delivery(const app_node_comm_envelope *envelope,
                                 const char *reason)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return mesh_start_tracked_tx(envelope, reason);
}

int app_node_comm_start_owned_delivery(const app_node_comm_envelope *envelope,
                                       const char *reason,
                                       bool *rf_sent)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        if (rf_sent != NULL) {
            *rf_sent = false;
        }
        return ret;
    }
    return mesh_start_owned_tracked_tx(envelope, reason, rf_sent);
}

int app_node_comm_queue_local_delivery(const app_node_comm_envelope *envelope)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return queue_anchor_report(envelope);
}

int app_node_comm_submit_delivery(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out)
{
    struct app_node_comm_delivery_record *record;
    struct node_comm_request request;
    bool recovery_started;
    uint64_t now_ms;
    uint32_t handle;
    int ret;

    if (envelope == NULL || handle_out == NULL || absolute_deadline_ms == 0u ||
        profile < NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD ||
        profile >= NODE_COMM_PROFILE_COUNT) {
        return -EINVAL;
    }
    if (profile != NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD) {
        return -ENOTSUP;
    }
    if (envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN) {
        return -EMSGSIZE;
    }
    if (envelope->packet.payload_len != envelope->payload_len) {
        return -EINVAL;
    }
    if (envelope->flood_retry_count != 0u) {
        /* One scheduler attempt must correspond to one bounded flood. */
        return -EINVAL;
    }

    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    recovery_started = app_node_comm_lifecycle_service_locked();
    if (node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_backend_ready) {
        app_node_comm_sync_unlock();
        if (recovery_started) {
            app_node_comm_begin_recovery();
        }
        return -ESHUTDOWN;
    }
    record = app_node_comm_free_delivery_record();
    if (record == NULL) {
        app_node_comm_sync_unlock();
        return -ENOSPC;
    }
    now_ms = app_node_comm_now_ms();
    request = (struct node_comm_request) {
        .profile = profile,
        .absolute_deadline_ms = absolute_deadline_ms,
        .client_token = client_token,
        .retry_jitter_seed = app_node_comm_retry_jitter_seed(envelope),
    };
    ret = node_comm_submit(&node_comm_policy, &request, now_ms, &handle);
    if (ret == 0) {
        ret = app_node_comm_freeze_delivery(record, envelope, handle);
    }
    if (ret == 0) {
        *handle_out = handle;
        app_node_comm_schedule_delivery_locked(now_ms);
    }
    app_node_comm_sync_unlock();
    if (recovery_started) {
        app_node_comm_begin_recovery();
    }
    return ret;
}

int app_node_comm_service_deliveries(void)
{
    struct app_node_comm_delivery_record attempt_record;
    struct app_node_comm_delivery_record *record;
    struct app_mesh_outbound_view attempt_view;
    struct node_comm_lease lease;
    enum node_comm_delivery_outcome outcome;
    bool recovery_started;
    bool sent_now = false;
    uint64_t attempt_begin_ms;
    uint64_t not_before_ms;
    uint64_t now_ms;
    int state_ret = 0;
    int ret;

    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    recovery_started = app_node_comm_lifecycle_service_locked();
    if (node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_backend_ready) {
        app_node_comm_sync_unlock();
        if (recovery_started) {
            app_node_comm_begin_recovery();
        }
        return -ESHUTDOWN;
    }
    attempt_begin_ms = app_node_comm_now_ms();
    ret = node_comm_acquire(&node_comm_policy, attempt_begin_ms, &lease);
    if (ret < 0) {
        app_node_comm_schedule_delivery_locked(attempt_begin_ms);
        app_node_comm_sync_unlock();
        return ret;
    }
    record = app_node_comm_delivery_record_for_handle(lease.handle);
    if (record == NULL) {
        (void)node_comm_lease_complete(&node_comm_policy,
                                       &lease,
                                       NODE_COMM_DELIVERY_FAILED,
                                       attempt_begin_ms);
        app_node_comm_sync_unlock();
        return -EFAULT;
    }
    attempt_record = *record;
    attempt_view = (struct app_mesh_outbound_view) {
        .packet = &attempt_record.packet,
        .payload = attempt_record.payload,
        .payload_len = attempt_record.payload_len,
        .radio_channel = attempt_record.radio_channel,
        .next_hop_id = attempt_record.next_hop_id,
        .queued_at_ms = attempt_record.queued_at_ms,
        .earliest_tx_ms = attempt_record.earliest_tx_ms,
        .flood_retry_count = attempt_record.flood_retry_count,
    };
    node_comm_delivery_backend_active = true;
    app_node_comm_sync_unlock();

    ret = mesh_try_send_c5_flood_view(
        &attempt_view,
        C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
        "node-comm-bounded-control-flood",
        &sent_now);

    if (app_node_comm_sync_lock() < 0) {
        /* The active lease prevents pause completion; recovery will reset. */
        return -EWOULDBLOCK;
    }
    node_comm_delivery_backend_active = false;
    now_ms = app_node_comm_now_ms();
    if (sent_now) {
        state_ret = node_comm_lease_note_rf_started(
            &node_comm_policy, &lease, attempt_begin_ms);
        if (state_ret == 0) {
            outcome = ret == 0 ? NODE_COMM_DELIVERY_SUCCEEDED :
                      app_node_comm_backend_error_retryable(ret) ?
                          NODE_COMM_DELIVERY_RETRY :
                          NODE_COMM_DELIVERY_FAILED;
            state_ret = node_comm_lease_complete(&node_comm_policy,
                                                  &lease,
                                                  outcome,
                                                  now_ms);
        }
    } else if (ret == 0 || app_node_comm_backend_error_retryable(ret)) {
        not_before_ms = UINT64_MAX - now_ms < NODE_COMM_PRE_RF_RETRY_MS ?
                        UINT64_MAX : now_ms + NODE_COMM_PRE_RF_RETRY_MS;
        state_ret = node_comm_lease_defer_pre_rf(&node_comm_policy,
                                                  &lease,
                                                  not_before_ms,
                                                  now_ms);
    } else {
        state_ret = node_comm_lease_complete(&node_comm_policy,
                                              &lease,
                                              NODE_COMM_DELIVERY_FAILED,
                                              now_ms);
    }
    app_node_comm_schedule_delivery_locked(now_ms);
    app_node_comm_sync_unlock();

    if (state_ret < 0 && state_ret != -ESTALE) {
        return state_ret;
    }
    return ret;
}

int app_node_comm_cancel_delivery(uint32_t handle)
{
    uint64_t now_ms;
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    ret = node_comm_cancel(&node_comm_policy, handle, now_ms);
    app_node_comm_schedule_delivery_locked(now_ms);
    app_node_comm_sync_unlock();
    return ret;
}

bool app_node_comm_take_delivery_event(
    struct node_comm_terminal_event *event_out)
{
    bool have_event = false;

    if (event_out == NULL || app_node_comm_sync_lock() < 0) {
        return false;
    }
    (void)node_comm_service(&node_comm_policy, app_node_comm_now_ms());
    have_event = node_comm_take_terminal_event(&node_comm_policy, event_out);
    if (have_event) {
        app_node_comm_clear_delivery_record(event_out->handle);
    }
    app_node_comm_sync_unlock();
    return have_event;
}

bool app_node_comm_take_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    bool have_event = false;

    if (event_out == NULL || handle == 0u ||
        app_node_comm_sync_lock() < 0) {
        return false;
    }
    (void)node_comm_service(&node_comm_policy, app_node_comm_now_ms());
    have_event = node_comm_take_terminal_event_for(&node_comm_policy,
                                                   handle,
                                                   event_out);
    if (have_event) {
        app_node_comm_clear_delivery_record(handle);
    }
    app_node_comm_sync_unlock();
    return have_event;
}

size_t app_node_comm_pending_delivery_count(void)
{
    size_t count = 0u;

    if (app_node_comm_sync_lock() == 0) {
        (void)node_comm_service(&node_comm_policy, app_node_comm_now_ms());
        count = node_comm_pending_count(&node_comm_policy);
        app_node_comm_sync_unlock();
    }
    return count;
}

bool app_node_comm_delivery_backlog_active(void)
{
    bool active;

    if (app_node_comm_sync_lock() < 0) {
        return false;
    }
    active = mesh_report_tx_backlog_active();
    app_node_comm_sync_unlock();
    return active;
}

bool app_node_comm_ack_wait_active(void)
{
    bool active;

    if (app_node_comm_sync_lock() < 0) {
        return false;
    }
    active = mesh_report_ch9_ack_wait_active();
    app_node_comm_sync_unlock();
    return active;
}

void app_node_comm_delivery_health_get(app_node_comm_delivery_health *health)
{
    if (app_node_comm_sync_lock() == 0) {
        mesh_delivery_health_get(health);
        app_node_comm_sync_unlock();
    }
}

bool app_node_comm_policy_running(void)
{
    return app_node_comm_require_running() == 0;
}

int app_node_comm_pause_request(uint32_t owner,
                                uint32_t max_hold_ms,
                                struct node_comm_pause_lease *lease_out)
{
    uint64_t now_ms;
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    ret = node_comm_request_pause(&node_comm_policy,
                                  owner,
                                  max_hold_ms,
                                  now_ms,
                                  lease_out);
    if (ret == 0) {
        node_comm_backend_ready = false;
    }
    app_node_comm_sync_unlock();

    if (ret == 0) {
        (void)k_work_cancel_delayable(&node_comm_delivery_work);
        app_node_comm_gateway_route_refresh_pause((uint32_t)now_ms);
        (void)mesh_transport_pause_preserving_queued();
        if (!app_node_comm_transport_quiesced()) {
            dwm3000_driver_request_receive_abort();
        }
        (void)k_work_reschedule(
            &node_comm_lifecycle_watchdog_work,
            K_MSEC(max_hold_ms));
    }
    return ret;
}

int app_node_comm_pause_note_quiesced(
    const struct node_comm_pause_lease *lease)
{
    bool quiesced = app_node_comm_transport_quiesced();
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    ret = quiesced ? node_comm_note_quiesced(
        &node_comm_policy, lease, app_node_comm_now_ms()) : -EBUSY;
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_resume_begin(const struct node_comm_pause_lease *lease)
{
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    ret = node_comm_begin_resume(&node_comm_policy,
                                 lease,
                                 app_node_comm_now_ms());
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_resume_complete(const struct node_comm_pause_lease *lease)
{
    uint64_t now_ms = app_node_comm_now_ms();
    bool quiesced = app_node_comm_transport_quiesced();
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    ret = node_comm_resume_ready(&node_comm_policy, lease, now_ms);
    if (ret == 0 && !quiesced) {
        ret = -EBUSY;
    }
    app_node_comm_sync_unlock();
    if (ret == 0) {
        uint64_t completion_now_ms;

        (void)k_work_cancel_delayable(&node_comm_lifecycle_watchdog_work);
        mesh_transport_resume();
        completion_now_ms = app_node_comm_now_ms();
        ret = app_node_comm_sync_lock();
        if (ret < 0) {
            (void)mesh_transport_pause_preserving_queued();
            app_node_comm_gateway_route_refresh_pause(
                (uint32_t)completion_now_ms);
            return ret;
        }
        ret = node_comm_note_resumed(&node_comm_policy,
                                     lease,
                                     completion_now_ms);
        if (ret == 0) {
            node_comm_lifecycle_recovery_deadline_ms = 0u;
            node_comm_backend_ready = true;
            app_node_comm_schedule_delivery_locked(completion_now_ms);
        }
        app_node_comm_sync_unlock();
        if (ret == 0) {
            app_node_comm_gateway_route_refresh_resume(
                (uint32_t)completion_now_ms);
        } else {
            app_node_comm_begin_recovery();
        }
    }
    return ret;
}

bool app_node_comm_forced_reclaim_lease(
    struct node_comm_pause_lease *lease_out)
{
    bool have_lease;

    app_node_comm_lifecycle_service();
    if (app_node_comm_sync_lock() < 0) {
        return false;
    }
    have_lease = node_comm_pause_recovery_lease(&node_comm_policy,
                                                lease_out);
    app_node_comm_sync_unlock();
    return have_lease;
}

int app_node_comm_stop_preserving_queued(void)
{
    uint64_t now_ms = app_node_comm_now_ms();
    enum node_comm_lifecycle_state state;
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    state = node_comm_state(&node_comm_policy);
    if (state == NODE_COMM_STOPPED) {
        ret = 0;
    } else if (node_comm_delivery_backend_active) {
        node_comm_backend_ready = false;
        ret = -EINPROGRESS;
    } else {
        ret = node_comm_stop(&node_comm_policy,
                             NODE_COMM_STOP_PRESERVE_QUEUED,
                             now_ms);
    }
    if (ret == 0) {
        node_comm_lifecycle_recovery_deadline_ms = 0u;
        node_comm_backend_ready = false;
    }
    app_node_comm_sync_unlock();
    if (ret < 0 && ret != -EINPROGRESS) {
        return ret;
    }

    (void)k_work_cancel_delayable(&node_comm_lifecycle_watchdog_work);
    (void)k_work_cancel_delayable(&node_comm_delivery_work);
    app_node_comm_gateway_route_refresh_pause((uint32_t)now_ms);
    (void)mesh_transport_pause_preserving_queued();
    if (app_node_comm_transport_quiesced()) {
        return ret == -EINPROGRESS ? -EINPROGRESS : 0;
    }
    dwm3000_driver_request_receive_abort();
    return -EINPROGRESS;
}

int app_node_comm_start(void)
{
    uint64_t now_ms = app_node_comm_now_ms();
    int ret;

    if (!app_node_comm_transport_quiesced()) {
        return -EBUSY;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    if (node_comm_state(&node_comm_policy) != NODE_COMM_STOPPED) {
        app_node_comm_sync_unlock();
        return -EINVAL;
    }
    node_comm_lifecycle_recovery_deadline_ms = 0u;
    ret = node_comm_start(&node_comm_policy, now_ms);
    if (ret == 0) {
        node_comm_backend_ready = false;
    }
    app_node_comm_sync_unlock();

    if (ret == 0) {
        (void)k_work_cancel_delayable(&node_comm_lifecycle_watchdog_work);
        mesh_transport_resume();
        if (app_node_comm_sync_lock() < 0) {
            (void)mesh_transport_pause_preserving_queued();
            app_node_comm_gateway_route_refresh_pause((uint32_t)now_ms);
            return -EWOULDBLOCK;
        }
        if (node_comm_state(&node_comm_policy) == NODE_COMM_RUNNING) {
            node_comm_backend_ready = true;
            app_node_comm_schedule_delivery_locked(app_node_comm_now_ms());
        } else {
            ret = -ECANCELED;
        }
        app_node_comm_sync_unlock();
        if (ret == 0) {
            app_node_comm_gateway_route_refresh_resume((uint32_t)now_ms);
        } else {
            (void)mesh_transport_pause_preserving_queued();
            app_node_comm_gateway_route_refresh_pause((uint32_t)now_ms);
        }
    }
    return ret;
}

void app_node_comm_start_route_refresh(void)
{
    if (app_node_comm_require_running() == 0) {
        app_node_comm_gateway_route_refresh_start();
    }
}

int app_node_comm_request_route_refresh(uint32_t delay_ms,
                                        const char *reason,
                                        bool forced)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return app_node_comm_gateway_route_refresh_request(delay_ms,
                                                       reason,
                                                       forced,
                                                       NULL);
}

int app_node_comm_request_route_refresh_correlated(
    uint32_t delay_ms,
    const char *reason,
    const struct proto_packet *correlation)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    if (correlation == NULL) {
        return -EINVAL;
    }
    return app_node_comm_gateway_route_refresh_request(delay_ms,
                                                       reason,
                                                       true,
                                                       correlation);
}
