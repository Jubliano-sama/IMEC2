#include "app_node_comm.h"

#include "app_board.h"
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

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif
#ifndef APP_NODE_COMM_GATEWAY_ROLE
#define APP_NODE_COMM_GATEWAY_ROLE 0
#endif

struct app_node_comm_delivery_record {
    struct proto_packet packet;
    uint8_t payload[APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN];
    uint16_t payload_len;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
    uint32_t handle;
    enum node_comm_delivery_profile profile;
    uint8_t flood_retry_count;
    bool occupied;
    bool auto_reap_terminal;
    bool track_control_response_health;
    bool gateway_confirmed;
    bool backend_released;
    bool backend_attempt_outstanding;
    bool uses_large_control_payload;
    uint8_t backend_durable_attempt_token;
};

static struct node_comm node_comm_policy;
static struct app_node_comm_delivery_record
    node_comm_delivery_records[APP_NODE_COMM_MAX_DELIVERIES];
#if APP_NODE_COMM_GATEWAY_ROLE
static uint8_t node_comm_large_control_payload[
    APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN];
static uint32_t node_comm_large_control_payload_handle;
#endif
static struct k_work_delayable node_comm_lifecycle_watchdog_work;
static struct k_work_delayable node_comm_delivery_work;
static struct k_work_delayable node_comm_delivery_due_kick_work;
static uint64_t node_comm_lifecycle_recovery_deadline_ms;
static bool node_comm_backend_ready;
static bool node_comm_delivery_backend_active;
static uint32_t node_comm_delivery_backend_active_handle;
static uint32_t node_comm_reliable_uplink_inflight_handle;
static struct app_node_comm_control_response_health
    node_comm_control_response_health;
static struct app_node_comm_durable_attempt_ops node_comm_durable_attempt_ops;
static bool node_comm_durable_attempt_ops_registered;

#define NODE_COMM_LIFECYCLE_RECOVERY_POLL_MS 10u
#define NODE_COMM_LIFECYCLE_RECOVERY_TIMEOUT_MS 1000u
#define NODE_COMM_GATEWAY_DUE_KICK_RETRY_MS 1u

_Static_assert(sizeof(node_comm_delivery_records) /
                   sizeof(node_comm_delivery_records[0]) ==
                   APP_NODE_COMM_MAX_DELIVERIES,
               "node communication delivery capacity changed");
_Static_assert(APP_NODE_COMM_MAX_DELIVERIES <= NODE_COMM_MAX_REQUESTS,
               "facade delivery capacity exceeds scheduler capacity");
_Static_assert(APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES > 0u &&
                   APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES <
                       APP_NODE_COMM_MAX_DELIVERIES,
               "protocol delivery reserve must leave background capacity");
_Static_assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN <=
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "frozen delivery payload exceeds mesh envelope capacity");
_Static_assert(APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN ==
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "large control owner must freeze a complete mesh payload");
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
_Static_assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ==
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "synthetic transmitter must freeze full mesh payloads");
#else
_Static_assert(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN == 192u,
               "generic frozen payload bound changed");
#endif

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
static void app_node_comm_delivery_due_kick_handler(struct k_work *work);
static size_t app_node_comm_service_policy_locked(uint64_t now_ms);

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

static bool app_node_comm_protocol_priority_profile(
    enum node_comm_delivery_profile profile)
{
    return profile == NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD ||
           profile == NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
           profile == NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE ||
           profile == NODE_COMM_PROFILE_CONTROL_RESPONSE;
}

static bool app_node_comm_reliable_backend_profile(
    enum node_comm_delivery_profile profile)
{
    return profile == NODE_COMM_PROFILE_RELIABLE_UPLINK ||
           profile == NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
           profile == NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE;
}

static int app_node_comm_durable_attempt_begin(
    const struct app_node_comm_delivery_record *record,
    uint8_t *attempt_token)
{
    if (record == NULL || attempt_token == NULL) {
        return -EINVAL;
    }
    *attempt_token = 0u;
    if (record->profile != NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK) {
        return 0;
    }
    if (!node_comm_durable_attempt_ops_registered ||
        node_comm_durable_attempt_ops.begin == NULL ||
        node_comm_durable_attempt_ops.complete == NULL) {
        return -ENOTSUP;
    }
    return node_comm_durable_attempt_ops.begin(&record->packet,
                                                attempt_token);
}

static int app_node_comm_durable_attempt_complete(
    const struct app_node_comm_delivery_record *record,
    uint8_t attempt_token,
    bool rf_started)
{
    if (record == NULL) {
        return -EINVAL;
    }
    if (record->profile != NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK) {
        return 0;
    }
    if (!node_comm_durable_attempt_ops_registered ||
        node_comm_durable_attempt_ops.complete == NULL ||
        attempt_token == 0u) {
        return -ENOTSUP;
    }
    return node_comm_durable_attempt_ops.complete(&record->packet,
                                                   attempt_token,
                                                   rf_started);
}

static struct app_node_comm_delivery_record *
app_node_comm_free_delivery_record(enum node_comm_delivery_profile profile)
{
    struct app_node_comm_delivery_record *free_record = NULL;
    size_t free_count = 0u;

    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        if (!node_comm_delivery_records[i].occupied) {
            if (free_record == NULL) {
                free_record = &node_comm_delivery_records[i];
            }
            free_count++;
        }
    }
    if (!app_node_comm_protocol_priority_profile(profile) &&
        free_count <= APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES) {
        status_debug_printf(
            "DBG_NODE_COMM_PROTOCOL_RESERVE profile=%u free=%u reserved=%u\n",
            (unsigned int)profile,
            (unsigned int)free_count,
            (unsigned int)APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES);
        return NULL;
    }
    return free_record;
}

static void app_node_comm_log_full_delivery_records(void)
{
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        const struct app_node_comm_delivery_record *record =
            &node_comm_delivery_records[i];

        status_debug_printf(
            "DBG_NODE_COMM_FULL slot=%u occupied=%u handle=%u profile=%u auto=%u "
            "type=%u src=0x%016llx dst=0x%016llx session=%u seq=%u\n",
            (unsigned int)i,
            record->occupied ? 1u : 0u,
            record->handle,
            (unsigned int)record->profile,
            record->auto_reap_terminal ? 1u : 0u,
            record->packet.msg_type,
            (unsigned long long)record->packet.src_id,
            (unsigned long long)record->packet.dst_id,
            record->packet.session_id,
            record->packet.seq);
    }
}

static bool app_node_comm_packet_identity_matches(
    const struct proto_packet *left,
    const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq;
}

static struct app_node_comm_delivery_record *
app_node_comm_delivery_record_for_packet(const struct proto_packet *packet)
{
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        if (node_comm_delivery_records[i].occupied &&
            app_node_comm_packet_identity_matches(
                &node_comm_delivery_records[i].packet, packet)) {
            return &node_comm_delivery_records[i];
        }
    }
    return NULL;
}

static const uint8_t *app_node_comm_frozen_payload(
    const struct app_node_comm_delivery_record *record)
{
    if (record == NULL) {
        return NULL;
    }
    if (!record->uses_large_control_payload) {
        return record->payload;
    }
#if APP_NODE_COMM_GATEWAY_ROLE
    if (node_comm_large_control_payload_handle == record->handle) {
        return node_comm_large_control_payload;
    }
#endif
    return NULL;
}

static bool app_node_comm_payload_size_supported(
    size_t payload_len,
    enum node_comm_delivery_profile profile)
{
    if (payload_len <= APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN) {
        return true;
    }
#if APP_NODE_COMM_GATEWAY_ROLE
    return profile == NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD &&
           payload_len <= APP_NODE_COMM_LARGE_CONTROL_PAYLOAD_MAX_LEN;
#else
    ARG_UNUSED(profile);
    return false;
#endif
}

static bool app_node_comm_frozen_delivery_matches(
    const struct app_node_comm_delivery_record *record,
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile)
{
    const uint8_t *payload = app_node_comm_frozen_payload(record);

    return record != NULL && envelope != NULL && payload != NULL &&
           record->profile == profile &&
           record->payload_len == envelope->payload_len &&
           memcmp(payload, envelope->payload,
                  envelope->payload_len) == 0;
}

static void app_node_comm_clear_delivery_record(uint32_t handle)
{
    struct app_node_comm_delivery_record *record =
        app_node_comm_delivery_record_for_handle(handle);

    if (record != NULL) {
        if (node_comm_reliable_uplink_inflight_handle == handle) {
            node_comm_reliable_uplink_inflight_handle = 0u;
        }
#if APP_NODE_COMM_GATEWAY_ROLE
        if (record->uses_large_control_payload &&
            node_comm_large_control_payload_handle == handle) {
            node_comm_large_control_payload_handle = 0u;
        }
#endif
        memset(record, 0, sizeof(*record));
    }
}

static void app_node_comm_reconcile_terminal_backends_locked(void)
{
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        struct app_node_comm_delivery_record *record =
            &node_comm_delivery_records[i];
        struct node_comm_terminal_event terminal;
        int ret;

        if (!record->occupied || record->backend_released ||
            node_comm_delivery_backend_active_handle == record->handle ||
            record->backend_attempt_outstanding ||
            !app_node_comm_reliable_backend_profile(record->profile) ||
            !node_comm_peek_terminal_event_for(&node_comm_policy,
                                               record->handle,
                                               &terminal)) {
            continue;
        }
        ret = mesh_cancel_reliable_uplink(&record->packet);
        record->backend_released = true;
        if (node_comm_reliable_uplink_inflight_handle == record->handle) {
            node_comm_reliable_uplink_inflight_handle = 0u;
        }
        status_debug_printf(
            "DBG_NODE_COMM_TERMINAL_BACKEND_RELEASE handle=%u reason=%u attempts=%u ret=%d\n",
            record->handle,
            (unsigned int)terminal.reason,
            terminal.attempts_started,
            ret);
    }
}

static size_t app_node_comm_service_policy_locked(uint64_t now_ms)
{
    size_t terminalized = node_comm_service(&node_comm_policy, now_ms);

    app_node_comm_reconcile_terminal_backends_locked();
    return terminalized;
}

static void app_node_comm_reap_auto_terminal_events_locked(void)
{
    app_node_comm_reconcile_terminal_backends_locked();
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        struct app_node_comm_delivery_record *record =
            &node_comm_delivery_records[i];
        struct node_comm_terminal_event event;

        if (!record->occupied || !record->auto_reap_terminal ||
            record->backend_attempt_outstanding ||
            node_comm_delivery_backend_active_handle == record->handle ||
            !node_comm_take_terminal_event_for(&node_comm_policy,
                                               record->handle,
                                               &event)) {
            continue;
        }
        if (record->track_control_response_health) {
            node_comm_control_response_health.last_terminal_reason = event.reason;
            node_comm_control_response_health.last_attempts_started =
                event.attempts_started;
            if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
                node_comm_control_response_health.delivered++;
            } else {
                node_comm_control_response_health.failed++;
            }
        }
        status_debug_printf(
            "DBG_NODE_COMM_TERMINAL handle=%u profile=%u token=%u reason=%u attempts=%u\n",
            event.handle,
            (unsigned int)record->profile,
            event.client_token,
            (unsigned int)event.reason,
            event.attempts_started);
        if (node_comm_reliable_uplink_inflight_handle == event.handle) {
            node_comm_reliable_uplink_inflight_handle = 0u;
        }
        app_node_comm_clear_delivery_record(event.handle);
    }
}

static int app_node_comm_freeze_delivery(
    struct app_node_comm_delivery_record *record,
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint32_t handle,
    bool auto_reap_terminal,
    bool track_control_response_health)
{
    bool use_large_payload;

    if (record == NULL || envelope == NULL || handle == 0u ||
        !app_node_comm_payload_size_supported(envelope->payload_len, profile) ||
        envelope->packet.payload_len != envelope->payload_len) {
        return envelope != NULL && !app_node_comm_payload_size_supported(
                   envelope->payload_len, profile) ?
               -EMSGSIZE : -EINVAL;
    }
    use_large_payload =
        envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN;
#if APP_NODE_COMM_GATEWAY_ROLE
    if (use_large_payload && node_comm_large_control_payload_handle != 0u) {
        return -ENOSPC;
    }
#endif
    memset(record, 0, sizeof(*record));
    record->packet = envelope->packet;
    if (use_large_payload) {
#if APP_NODE_COMM_GATEWAY_ROLE
        memcpy(node_comm_large_control_payload,
               envelope->payload,
               envelope->payload_len);
        node_comm_large_control_payload_handle = handle;
        record->uses_large_control_payload = true;
#endif
    } else {
        memcpy(record->payload, envelope->payload, envelope->payload_len);
    }
    record->payload_len = envelope->payload_len;
    record->radio_channel = envelope->radio_channel;
    record->next_hop_id = envelope->next_hop_id;
    record->queued_at_ms = envelope->queued_at_ms;
    record->earliest_tx_ms = envelope->earliest_tx_ms;
    record->flood_retry_count = envelope->flood_retry_count;
    record->handle = handle;
    record->profile = profile;
    record->occupied = true;
    record->auto_reap_terminal = auto_reap_terminal;
    record->track_control_response_health = track_control_response_health;
    return 0;
}

static bool app_node_comm_backend_error_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -EWOULDBLOCK ||
           ret == -EINPROGRESS || ret == -EHOSTUNREACH ||
           ret == -ENETUNREACH || ret == -ENOTCONN || ret == -ETIMEDOUT ||
           ret == -EIO || ret == -ECANCELED || ret == -ETIME;
}

static uint32_t app_node_comm_retry_jitter_seed_words(
    const uint32_t *identity_words,
    size_t identity_word_count)
{
    uint32_t seed = UINT32_C(0x811c9dc5);

    for (size_t i = 0u; i < identity_word_count; i++) {
        seed ^= identity_words[i];
        seed *= UINT32_C(0x01000193);
    }
    return seed == 0u ? UINT32_C(0x6d2b79f5) : seed;
}

static uint32_t app_node_comm_retry_jitter_seed(
    const app_node_comm_envelope *envelope)
{
    const uint32_t identity_words[] = {
        (uint32_t)envelope->packet.src_id,
        (uint32_t)(envelope->packet.src_id >> 32),
        (uint32_t)envelope->packet.dst_id,
        (uint32_t)(envelope->packet.dst_id >> 32),
        envelope->packet.session_id,
        envelope->packet.seq,
        envelope->packet.msg_type,
    };

    return app_node_comm_retry_jitter_seed_words(
        identity_words,
        sizeof(identity_words) / sizeof(identity_words[0]));
}

static void app_node_comm_delivery_work_handler(struct k_work *work)
{
    (void)work;
    if (DEVICE_ROLE == ROLE_GATEWAY &&
        !mesh_node_comm_gateway_delivery_due_ready()) {
        return;
    }
    (void)app_node_comm_service_deliveries();
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        (void)mesh_node_comm_gateway_delivery_due_end();
        if (app_node_comm_sync_lock() == 0) {
            app_node_comm_schedule_delivery_locked(app_node_comm_now_ms());
            app_node_comm_sync_unlock();
        }
        mesh_restart_role_scan();
    }
}

static void app_node_comm_schedule_delivery_locked(uint64_t now_ms)
{
    uint64_t delay_ms;
    uint64_t due_ms;
    uint32_t queue_delay_ms;

    if (!node_comm_backend_ready || node_comm_delivery_backend_active ||
        node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_next_service_due_ms(&node_comm_policy, now_ms, &due_ms)) {
        return;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY &&
        mesh_node_comm_gateway_delivery_due_pending()) {
        return;
    }
    delay_ms = due_ms > now_ms ? due_ms - now_ms : 0u;
    if (delay_ms > INT64_MAX) {
        delay_ms = INT64_MAX;
    }
    queue_delay_ms = delay_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delay_ms;
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        /*
         * The gateway's mesh-route queue may be inside a 30 s continuous RX
         * call.  A tiny system-workqueue kick requests the handoff at the
         * exact due time; only the real delivery worker executes on
         * mesh-route.
         */
        (void)k_work_reschedule(&node_comm_delivery_due_kick_work,
                                K_MSEC(queue_delay_ms));
    } else {
        (void)mesh_route_work_reschedule(&node_comm_delivery_work,
                                         queue_delay_ms);
    }
}

static void app_node_comm_delivery_due_kick_handler(struct k_work *work)
{
    bool keep_pending_for_retry = false;
    bool release_pending = false;
    bool wait_for_scan_boundary = false;
    uint64_t due_ms = 0u;
    uint64_t now_ms;
    int ret = 0;

    (void)work;
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    now_ms = app_node_comm_now_ms();
    if (!node_comm_backend_ready || node_comm_delivery_backend_active ||
        node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_next_service_due_ms(&node_comm_policy, now_ms, &due_ms)) {
        release_pending = mesh_node_comm_gateway_delivery_due_end();
        app_node_comm_sync_unlock();
        if (release_pending) {
            mesh_restart_role_scan();
        }
        return;
    }
    if (due_ms > now_ms) {
        release_pending = mesh_node_comm_gateway_delivery_due_end();
        app_node_comm_schedule_delivery_locked(now_ms);
        app_node_comm_sync_unlock();
        if (release_pending) {
            mesh_restart_role_scan();
        }
        return;
    }
    if (mesh_node_comm_gateway_delivery_due_pending()) {
        wait_for_scan_boundary =
            !mesh_node_comm_gateway_delivery_due_ready();
    } else {
        ret = mesh_node_comm_gateway_delivery_due_begin(
            &wait_for_scan_boundary);
    }

    if (ret == 0 && !wait_for_scan_boundary) {
        /* A host command already awaiting a boundary keeps queue priority. */
        ret = mesh_gateway_command_priority_safe_boundary();
        if (ret < 0) {
            /* Keep the gate and retry without starting an RF opportunity. */
            keep_pending_for_retry = true;
            (void)k_work_reschedule(
                &node_comm_delivery_due_kick_work,
                K_MSEC(NODE_COMM_GATEWAY_DUE_KICK_RETRY_MS));
        } else {
            ret = mesh_route_work_reschedule(&node_comm_delivery_work, 0u);
        }
    }
    if (ret < 0 && !keep_pending_for_retry) {
        release_pending = mesh_node_comm_gateway_delivery_due_end();
        if (release_pending) {
            (void)k_work_reschedule(
                &node_comm_delivery_due_kick_work,
                K_MSEC(NODE_COMM_GATEWAY_DUE_KICK_RETRY_MS));
        }
    }
    app_node_comm_sync_unlock();

    if (release_pending) {
        mesh_restart_role_scan();
    }
}

int app_node_comm_gateway_delivery_safe_boundary(void)
{
    bool release_pending = false;
    uint64_t due_ms = 0u;
    uint64_t now_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    ret = mesh_gateway_command_priority_safe_boundary();

    if (ret < 0) {
        if (mesh_node_comm_gateway_delivery_due_ready()) {
            (void)k_work_reschedule(
                &node_comm_delivery_due_kick_work,
                K_MSEC(NODE_COMM_GATEWAY_DUE_KICK_RETRY_MS));
        }
        return ret;
    }
    if (!mesh_node_comm_gateway_delivery_due_ready()) {
        return 0;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    if (!node_comm_backend_ready || node_comm_delivery_backend_active ||
        node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_next_service_due_ms(&node_comm_policy, now_ms, &due_ms)) {
        release_pending = mesh_node_comm_gateway_delivery_due_end();
    } else if (due_ms > now_ms) {
        release_pending = mesh_node_comm_gateway_delivery_due_end();
        app_node_comm_schedule_delivery_locked(now_ms);
    } else {
        ret = mesh_route_work_reschedule(&node_comm_delivery_work, 0u);
        if (ret < 0) {
            release_pending = mesh_node_comm_gateway_delivery_due_end();
            app_node_comm_schedule_delivery_locked(now_ms);
        }
    }
    app_node_comm_sync_unlock();

    if (release_pending) {
        mesh_restart_role_scan();
    }
    return ret < 0 ? ret : 0;
}

static void app_node_comm_begin_recovery(void)
{
    uint32_t now_ms = (uint32_t)app_node_comm_now_ms();

    if (DEVICE_ROLE == ROLE_GATEWAY) {
        (void)k_work_cancel_delayable(&node_comm_delivery_due_kick_work);
        (void)mesh_node_comm_gateway_delivery_due_end();
    }
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

    (void)app_node_comm_service_policy_locked(now_ms);
    app_node_comm_reap_auto_terminal_events_locked();
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
#if APP_NODE_COMM_GATEWAY_ROLE
    node_comm_large_control_payload_handle = 0u;
#endif
    memset(&node_comm_control_response_health, 0,
           sizeof(node_comm_control_response_health));
    memset(&node_comm_durable_attempt_ops, 0,
           sizeof(node_comm_durable_attempt_ops));
    node_comm_durable_attempt_ops_registered = false;
    node_comm_backend_ready = false;
    node_comm_delivery_backend_active = false;
    node_comm_delivery_backend_active_handle = 0u;
    node_comm_reliable_uplink_inflight_handle = 0u;
    node_comm_lifecycle_recovery_deadline_ms = 0u;
    k_work_init_delayable(&node_comm_lifecycle_watchdog_work,
                          app_node_comm_lifecycle_watchdog_handler);
    k_work_init_delayable(&node_comm_delivery_work,
                          app_node_comm_delivery_work_handler);
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        k_work_init_delayable(&node_comm_delivery_due_kick_work,
                              app_node_comm_delivery_due_kick_handler);
    }
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

int app_node_comm_register_durable_attempt_ops(
    const struct app_node_comm_durable_attempt_ops *ops)
{
    int ret;

    if (ops == NULL || ops->begin == NULL || ops->complete == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    if (node_comm_durable_attempt_ops_registered) {
        ret = -EALREADY;
    } else {
        node_comm_durable_attempt_ops = *ops;
        node_comm_durable_attempt_ops_registered = true;
        ret = 0;
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
    app_node_comm_envelope stamped_envelope;
    const app_node_comm_envelope *flood_envelope = envelope;
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        if (sent_now != NULL) {
            *sent_now = false;
        }
        return ret;
    }
    if (envelope != NULL && envelope->queued_at_ms == 0u) {
        uint32_t now_ms = (uint32_t)app_node_comm_now_ms();

        stamped_envelope = *envelope;
        /*
         * Freeze age before the lower layer starts a wake train.  Protocols
         * such as survey discovery use message age to align independent
         * receivers to the same gateway-originated phase.
         */
        stamped_envelope.queued_at_ms = now_ms == 0u ? 1u : now_ms;
        flood_envelope = &stamped_envelope;
    }
    return mesh_send_c5_flood(flood_envelope, purpose, reason, sent_now);
}

int app_node_comm_schedule_path_refresh(uint64_t target_id,
                                        const char *reason)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return mesh_schedule_route_request(target_id, reason);
}

int app_node_comm_retry_backoff_ms(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint16_t retry_round,
    uint32_t *delay_ms_out)
{
    if (envelope == NULL) {
        return -EINVAL;
    }
    return node_comm_retry_backoff_ms(profile,
                                      app_node_comm_retry_jitter_seed(envelope),
                                      retry_round,
                                      delay_ms_out);
}

int app_node_comm_retry_identity_backoff_ms(
    uint64_t node_id,
    uint32_t session_id,
    uint32_t opportunity,
    enum node_comm_delivery_profile profile,
    uint16_t retry_round,
    uint32_t *delay_ms_out)
{
    const uint32_t identity_words[] = {
        (uint32_t)node_id,
        (uint32_t)(node_id >> 32),
        session_id,
        opportunity,
    };

    if (node_id == 0u || session_id == 0u) {
        return -EINVAL;
    }
    return node_comm_retry_backoff_ms(
        profile,
        app_node_comm_retry_jitter_seed_words(
            identity_words,
            sizeof(identity_words) / sizeof(identity_words[0])),
        retry_round,
        delay_ms_out);
}

int app_node_comm_queue_local_delivery(const app_node_comm_envelope *envelope)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    return queue_anchor_report(envelope);
}

static int app_node_comm_submit_delivery_internal(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    bool auto_reap_terminal,
    bool track_control_response_health,
    uint32_t *handle_out)
{
    struct app_node_comm_delivery_record *record;
    struct node_comm_request request;
    struct node_comm_terminal_event cancelled;
    bool recovery_started;
    bool duplicate = false;
    uint64_t now_ms;
    uint32_t handle = 0u;
    int ret;

    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    recovery_started = app_node_comm_lifecycle_service_locked();
    if (node_comm_state(&node_comm_policy) != NODE_COMM_RUNNING ||
        !node_comm_backend_ready) {
        ret = -ESHUTDOWN;
    } else {
        record = app_node_comm_delivery_record_for_packet(&envelope->packet);
        if (record != NULL) {
            if (!app_node_comm_frozen_delivery_matches(record, envelope,
                                                       profile)) {
                ret = -EEXIST;
            } else {
                handle = record->handle;
                if (handle_out != NULL) {
                    *handle_out = handle;
                }
                duplicate = true;
                ret = 0;
            }
        } else {
            record = app_node_comm_free_delivery_record(profile);
            if (record == NULL) {
                app_node_comm_log_full_delivery_records();
                ret = -ENOSPC;
            } else {
                now_ms = app_node_comm_now_ms();
                request = (struct node_comm_request) {
                    .profile = profile,
                    .absolute_deadline_ms = absolute_deadline_ms,
                    .client_token = client_token,
                    .retry_jitter_seed = app_node_comm_retry_jitter_seed(envelope),
                };
                ret = node_comm_submit(&node_comm_policy, &request, now_ms,
                                       &handle);
                if (ret == 0) {
                    ret = app_node_comm_freeze_delivery(
                        record, envelope, profile, handle, auto_reap_terminal,
                        track_control_response_health);
                }
                if (ret < 0 && handle != 0u) {
                    (void)node_comm_cancel(&node_comm_policy, handle, now_ms);
                    (void)node_comm_take_terminal_event_for(
                        &node_comm_policy, handle, &cancelled);
                }
                if (ret == 0) {
                    if (handle_out != NULL) {
                        *handle_out = handle;
                    }
                    if (track_control_response_health) {
                        node_comm_control_response_health.submitted++;
                    }
                    app_node_comm_schedule_delivery_locked(now_ms);
                }
            }
        }
    }
    if (ret < 0 && track_control_response_health) {
        node_comm_control_response_health.admission_failures++;
    }
    app_node_comm_sync_unlock();
    if (ret == 0 && !duplicate) {
        status_debug_printf(
            "DBG_NODE_COMM_SUBMIT handle=%u profile=%u token=%u deadline=%llu\n",
            handle,
            (unsigned int)profile,
            client_token,
            (unsigned long long)absolute_deadline_ms);
    }
    if (recovery_started) {
        app_node_comm_begin_recovery();
    }
    return ret;
}

int app_node_comm_submit_delivery(
    const app_node_comm_envelope *envelope,
    enum node_comm_delivery_profile profile,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out)
{
    if (envelope == NULL || handle_out == NULL || absolute_deadline_ms == 0u ||
        profile < NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD ||
        profile >= NODE_COMM_PROFILE_COUNT) {
        return -EINVAL;
    }
    if (profile != NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD &&
        profile != NODE_COMM_PROFILE_RELIABLE_UPLINK &&
        profile != NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK &&
        profile != NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE &&
        profile != NODE_COMM_PROFILE_CONTROL_RESPONSE) {
        return -ENOTSUP;
    }
    if (!app_node_comm_payload_size_supported(envelope->payload_len, profile)) {
        return -EMSGSIZE;
    }
    if (envelope->packet.payload_len != envelope->payload_len) {
        return -EINVAL;
    }
    if (profile == NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD &&
        envelope->flood_retry_count != 0u) {
        /* One scheduler attempt must correspond to one bounded flood. */
        return -EINVAL;
    }

    return app_node_comm_submit_delivery_internal(
        envelope, profile, absolute_deadline_ms, client_token, false, false,
        handle_out);
}

int app_node_comm_submit_control_response(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token)
{
    if (envelope == NULL || absolute_deadline_ms == 0u ||
        envelope->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        envelope->packet.payload_len != envelope->payload_len) {
        return -EINVAL;
    }
    return app_node_comm_submit_delivery_internal(
        envelope, NODE_COMM_PROFILE_CONTROL_RESPONSE, absolute_deadline_ms,
        client_token, true, true, NULL);
}

int app_node_comm_submit_reliable_uplink(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out)
{
    if (envelope == NULL || absolute_deadline_ms == 0u ||
        envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        envelope->packet.payload_len != envelope->payload_len ||
        !mesh_id_is_unicast(envelope->packet.dst_id)) {
        return -EINVAL;
    }
    return app_node_comm_submit_delivery_internal(
        envelope, NODE_COMM_PROFILE_RELIABLE_UPLINK, absolute_deadline_ms,
        client_token, handle_out == NULL, false, handle_out);
}

int app_node_comm_submit_protocol_response(
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out)
{
    if (envelope == NULL || absolute_deadline_ms == 0u ||
        envelope->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        envelope->packet.payload_len != envelope->payload_len ||
        !mesh_id_is_unicast(envelope->packet.dst_id)) {
        return -EINVAL;
    }
    return app_node_comm_submit_delivery_internal(
        envelope, NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        absolute_deadline_ms, client_token, handle_out == NULL, false,
        handle_out);
}

int app_node_comm_service_deliveries(void)
{
    struct app_node_comm_delivery_record attempt_record;
    struct app_node_comm_delivery_record *record;
    struct app_mesh_outbound_view attempt_view;
    const uint8_t *attempt_payload;
    struct node_comm_lease lease;
    enum node_comm_delivery_outcome outcome;
    bool recovery_started;
    bool durable_attempt_started = false;
    bool gateway_confirmed = false;
    bool rf_started = false;
    uint8_t durable_attempt_token = 0u;
    uint32_t scheduled_retry_delay_ms = 0u;
    uint64_t attempt_begin_ms;
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
    attempt_payload = app_node_comm_frozen_payload(&attempt_record);
    if (attempt_payload == NULL) {
        (void)node_comm_lease_complete(&node_comm_policy,
                                       &lease,
                                       NODE_COMM_DELIVERY_FAILED,
                                       attempt_begin_ms);
        app_node_comm_sync_unlock();
        return -EFAULT;
    }
    if ((attempt_record.profile == NODE_COMM_PROFILE_RELIABLE_UPLINK ||
         attempt_record.profile == NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
         attempt_record.profile ==
             NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE) &&
        node_comm_reliable_uplink_inflight_handle != 0u &&
        node_comm_reliable_uplink_inflight_handle != lease.handle) {
        state_ret = node_comm_lease_defer_pre_rf_retry(&node_comm_policy,
                                                        &lease,
                                                        attempt_begin_ms);
        app_node_comm_schedule_delivery_locked(attempt_begin_ms);
        app_node_comm_sync_unlock();
        status_debug_printf(
            "DBG_NODE_COMM_SINGLE_FLIGHT_WAIT handle=%u owner=%u state=%d\n",
            lease.handle,
            node_comm_reliable_uplink_inflight_handle,
            state_ret);
        return state_ret < 0 ? state_ret : -EAGAIN;
    }
    attempt_view = (struct app_mesh_outbound_view) {
        .packet = &attempt_record.packet,
        .payload = attempt_payload,
        .payload_len = attempt_record.payload_len,
        .radio_channel = attempt_record.radio_channel,
        .next_hop_id = attempt_record.next_hop_id,
        .queued_at_ms = attempt_record.queued_at_ms,
        .earliest_tx_ms = attempt_record.earliest_tx_ms,
        .flood_retry_count = attempt_record.flood_retry_count,
    };
    ret = app_node_comm_durable_attempt_begin(
        &attempt_record, &durable_attempt_token);
    if (ret < 0) {
        state_ret = ret == -ETIMEDOUT ?
            node_comm_lease_complete(&node_comm_policy,
                                      &lease,
                                      NODE_COMM_DELIVERY_ATTEMPTS_EXHAUSTED,
                                      attempt_begin_ms) :
            node_comm_lease_defer_pre_rf_retry(&node_comm_policy,
                                                &lease,
                                                attempt_begin_ms);
        app_node_comm_reap_auto_terminal_events_locked();
        app_node_comm_schedule_delivery_locked(attempt_begin_ms);
        app_node_comm_sync_unlock();
        return state_ret < 0 ? state_ret : ret;
    }
    durable_attempt_started = durable_attempt_token != 0u;
    node_comm_delivery_backend_active = true;
    node_comm_delivery_backend_active_handle = lease.handle;
    app_node_comm_sync_unlock();

    if (attempt_record.profile == NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD) {
        ret = mesh_try_send_c5_flood_view(
            &attempt_view,
            C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
            "node-comm-bounded-control-flood",
            lease.attempt_number == 1u,
            &rf_started);
    } else if (attempt_record.profile == NODE_COMM_PROFILE_CONTROL_RESPONSE) {
        ret = mesh_try_send_control_response_view(
            &attempt_view,
            "node-comm-control-response",
            &rf_started);
    } else if (attempt_record.profile == NODE_COMM_PROFILE_RELIABLE_UPLINK ||
               attempt_record.profile ==
                   NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
               attempt_record.profile ==
                   NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE) {
        ret = mesh_try_send_reliable_uplink_view(
            &attempt_view,
            "node-comm-reliable-uplink",
            &rf_started,
            &gateway_confirmed,
            &scheduled_retry_delay_ms);
    } else {
        ret = -ENOTSUP;
    }

    if (durable_attempt_started) {
        int durable_ret = app_node_comm_durable_attempt_complete(
            &attempt_record, durable_attempt_token, rf_started);

        if (durable_ret < 0) {
            status_debug_printf(
                "DBG_NODE_COMM_DURABLE_ATTEMPT_COMMIT handle=%u token=%u rf=%u ret=%d\n",
                lease.handle,
                durable_attempt_token,
                rf_started ? 1u : 0u,
                durable_ret);
        }
    }

    if (app_node_comm_sync_lock() < 0) {
        /* The active lease prevents pause completion; recovery will reset. */
        return -EWOULDBLOCK;
    }
    now_ms = app_node_comm_now_ms();
    record = app_node_comm_delivery_record_for_handle(lease.handle);
    if (record != NULL && record->gateway_confirmed) {
        gateway_confirmed = true;
    }
    if (rf_started) {
        state_ret = node_comm_lease_note_rf_started(
            &node_comm_policy, &lease, attempt_begin_ms);
        if (state_ret == -ESTALE || state_ret == -EALREADY) {
            int late_rf_ret = node_comm_note_backend_rf_started(
                &node_comm_policy, lease.handle, attempt_begin_ms);

            if (late_rf_ret == 0 || late_rf_ret == -EALREADY) {
                state_ret = 0;
            }
        }
        if (state_ret == 0) {
            if (gateway_confirmed) {
                state_ret = node_comm_lease_complete(
                    &node_comm_policy, &lease,
                    NODE_COMM_DELIVERY_SUCCEEDED, now_ms);
            } else if (ret == 0 &&
                       (attempt_record.profile ==
                            NODE_COMM_PROFILE_RELIABLE_UPLINK ||
                        attempt_record.profile ==
                            NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK ||
                        attempt_record.profile ==
                            NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE)) {
                state_ret = node_comm_lease_await_confirmation(
                    &node_comm_policy, &lease, now_ms);
                if (state_ret == 0) {
                    node_comm_reliable_uplink_inflight_handle = lease.handle;
                }
            } else {
                outcome = ret == 0 ? NODE_COMM_DELIVERY_SUCCEEDED :
                          app_node_comm_backend_error_retryable(ret) ?
                              NODE_COMM_DELIVERY_RETRY :
                              NODE_COMM_DELIVERY_FAILED;
                state_ret = node_comm_lease_complete(&node_comm_policy,
                                                      &lease,
                                                      outcome,
                                                      now_ms);
            }
        }
    } else if (ret == 0 || app_node_comm_backend_error_retryable(ret)) {
        if (scheduled_retry_delay_ms > 0u) {
            uint64_t not_before_ms =
                now_ms + (uint64_t)scheduled_retry_delay_ms;

            if (not_before_ms < now_ms) {
                not_before_ms = UINT64_MAX;
            }
            state_ret = node_comm_lease_defer_pre_rf(&node_comm_policy,
                                                      &lease,
                                                      not_before_ms,
                                                      now_ms);
        } else {
            state_ret = node_comm_lease_defer_pre_rf_retry(&node_comm_policy,
                                                            &lease,
                                                            now_ms);
        }
    } else {
        state_ret = node_comm_lease_complete(&node_comm_policy,
                                              &lease,
                                              NODE_COMM_DELIVERY_FAILED,
                                              now_ms);
    }
    node_comm_delivery_backend_active = false;
    node_comm_delivery_backend_active_handle = 0u;
    app_node_comm_reap_auto_terminal_events_locked();
    app_node_comm_schedule_delivery_locked(now_ms);
    app_node_comm_sync_unlock();

    status_debug_printf(
        "DBG_NODE_COMM_ATTEMPT handle=%u profile=%u attempt=%u ret=%d rf=%u scheduled=%u state=%d\n",
        lease.handle,
        (unsigned int)attempt_record.profile,
        lease.attempt_number,
        ret,
        rf_started ? 1u : 0u,
        scheduled_retry_delay_ms,
        state_ret);

    if (state_ret < 0 && state_ret != -ESTALE) {
        return state_ret;
    }
    return ret;
}

int app_node_comm_note_gateway_confirmed(const struct proto_packet *packet)
{
    struct app_node_comm_delivery_record *record;
    uint64_t now_ms;
    int ret;

    if (packet == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    (void)app_node_comm_service_policy_locked(now_ms);
    record = app_node_comm_delivery_record_for_packet(packet);
    if (record == NULL ||
        (record->profile != NODE_COMM_PROFILE_RELIABLE_UPLINK &&
         record->profile != NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK &&
         record->profile !=
             NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE)) {
        ret = -ENOENT;
    } else {
        ret = node_comm_confirm_delivery(&node_comm_policy,
                                         record->handle,
                                         now_ms);
        if (ret == -EINPROGRESS) {
            record->gateway_confirmed = true;
            ret = 0;
        } else if (ret == -EALREADY) {
            ret = -ESTALE;
        } else if (ret == 0) {
            record->gateway_confirmed = true;
            app_node_comm_reap_auto_terminal_events_locked();
            app_node_comm_schedule_delivery_locked(now_ms);
        }
    }
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_note_gateway_failed(
    const struct proto_packet *packet,
    enum node_comm_terminal_reason reason)
{
    struct app_node_comm_delivery_record *record;
    uint64_t now_ms;
    int ret;

    if (packet == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    (void)app_node_comm_service_policy_locked(now_ms);
    record = app_node_comm_delivery_record_for_packet(packet);
    if (record == NULL ||
        (record->profile != NODE_COMM_PROFILE_RELIABLE_UPLINK &&
         record->profile != NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK &&
         record->profile !=
             NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE)) {
        ret = -ENOENT;
    } else {
        ret = node_comm_fail_delivery(&node_comm_policy,
                                      record->handle,
                                      reason,
                                      now_ms);
        if (ret == 0 || ret == -EALREADY) {
            app_node_comm_reap_auto_terminal_events_locked();
            app_node_comm_schedule_delivery_locked(now_ms);
        }
    }
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_backend_retry_preflight(const struct proto_packet *packet)
{
    struct app_node_comm_delivery_record *record;
    struct node_comm_terminal_event terminal;
    uint64_t now_ms;
    int ret;

    if (packet == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    (void)app_node_comm_service_policy_locked(now_ms);
    record = app_node_comm_delivery_record_for_packet(packet);
    if (record == NULL) {
        /* Most mesh retransmits are not owned by the facade. */
        ret = 0;
    } else if (node_comm_peek_terminal_event_for(&node_comm_policy,
                                                 record->handle,
                                                 &terminal)) {
        ret = terminal.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
              -ETIMEDOUT : -ECANCELED;
    } else if (record->backend_attempt_outstanding) {
        ret = -EBUSY;
    } else {
        record->backend_attempt_outstanding = true;
        ret = app_node_comm_durable_attempt_begin(
            record, &record->backend_durable_attempt_token);
        if (ret < 0) {
            record->backend_attempt_outstanding = false;
            record->backend_durable_attempt_token = 0u;
            if (ret == -ETIMEDOUT) {
                (void)node_comm_fail_delivery(
                    &node_comm_policy,
                    record->handle,
                    NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
                    now_ms);
                app_node_comm_reap_auto_terminal_events_locked();
                app_node_comm_schedule_delivery_locked(now_ms);
            }
        }
    }
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_complete_backend_attempt(const struct proto_packet *packet,
                                           bool rf_started)
{
    struct app_node_comm_delivery_record *record;
    struct node_comm_terminal_event terminal;
    uint64_t now_ms;
    int ret;

    if (packet == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    record = app_node_comm_delivery_record_for_packet(packet);
    if (record == NULL) {
        ret = -ENOENT;
    } else if (!record->backend_attempt_outstanding) {
        ret = -EAGAIN;
    } else {
        int durable_ret = app_node_comm_durable_attempt_complete(
            record, record->backend_durable_attempt_token, rf_started);

        if (durable_ret < 0) {
            status_debug_printf(
                "DBG_NODE_COMM_DURABLE_BACKEND_COMMIT handle=%u token=%u rf=%u ret=%d\n",
                record->handle,
                record->backend_durable_attempt_token,
                rf_started ? 1u : 0u,
                durable_ret);
        }
        record->backend_durable_attempt_token = 0u;
        record->backend_attempt_outstanding = false;
        ret = rf_started ?
              node_comm_note_backend_rf_started(&node_comm_policy,
                                                record->handle,
                                                now_ms) : 0;
        (void)app_node_comm_service_policy_locked(now_ms);
        if (node_comm_peek_terminal_event_for(&node_comm_policy,
                                              record->handle,
                                              &terminal)) {
            ret = terminal.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                  -ETIMEDOUT : -ECANCELED;
        }
        app_node_comm_reap_auto_terminal_events_locked();
        app_node_comm_schedule_delivery_locked(now_ms);
    }
    app_node_comm_sync_unlock();
    return ret;
}

int app_node_comm_note_backend_rf_started(const struct proto_packet *packet)
{
    return app_node_comm_complete_backend_attempt(packet, true);
}

int app_node_comm_cancel_delivery(uint32_t handle)
{
    bool restart_scan = false;
    uint64_t next_due_ms = 0u;
    uint64_t now_ms;
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    now_ms = app_node_comm_now_ms();
    ret = node_comm_cancel(&node_comm_policy, handle, now_ms);
    app_node_comm_reconcile_terminal_backends_locked();
    if (DEVICE_ROLE == ROLE_GATEWAY &&
        !node_comm_delivery_backend_active &&
        mesh_node_comm_gateway_delivery_due_pending() &&
        (!node_comm_next_service_due_ms(&node_comm_policy, now_ms,
                                        &next_due_ms) ||
         next_due_ms > now_ms)) {
        (void)k_work_cancel_delayable(&node_comm_delivery_due_kick_work);
        (void)k_work_cancel_delayable(&node_comm_delivery_work);
        restart_scan = mesh_node_comm_gateway_delivery_due_end();
    }
    app_node_comm_schedule_delivery_locked(now_ms);
    app_node_comm_sync_unlock();
    if (restart_scan) {
        mesh_restart_role_scan();
    }
    return ret;
}

int app_node_comm_abandon_delivery(uint32_t handle)
{
    struct node_comm_terminal_event ignored;
    int ret;

    if (handle == 0u) {
        return -EINVAL;
    }
    ret = app_node_comm_cancel_delivery(handle);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
    if (app_node_comm_take_delivery_event_for(handle, &ignored)) {
        return 0;
    }
    ret = app_node_comm_auto_reap_delivery(handle);
    return ret == 0 ? 0 : ret;
}

int app_node_comm_auto_reap_delivery(uint32_t handle)
{
    struct app_node_comm_delivery_record *record;
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    record = app_node_comm_delivery_record_for_handle(handle);
    if (record == NULL) {
        ret = -ENOENT;
    } else {
        record->auto_reap_terminal = true;
        record->track_control_response_health = false;
        app_node_comm_reap_auto_terminal_events_locked();
        ret = 0;
    }
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
    (void)app_node_comm_service_policy_locked(app_node_comm_now_ms());
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        struct app_node_comm_delivery_record *record =
            &node_comm_delivery_records[i];

        if (!record->occupied || record->backend_attempt_outstanding ||
            node_comm_delivery_backend_active_handle == record->handle ||
            !node_comm_take_terminal_event_for(&node_comm_policy,
                                               record->handle,
                                               event_out)) {
            continue;
        }
        app_node_comm_clear_delivery_record(event_out->handle);
        have_event = true;
        break;
    }
    app_node_comm_sync_unlock();
    return have_event;
}

bool app_node_comm_take_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    struct app_node_comm_delivery_record *record;
    bool have_event = false;

    if (event_out == NULL || handle == 0u ||
        app_node_comm_sync_lock() < 0) {
        return false;
    }
    (void)app_node_comm_service_policy_locked(app_node_comm_now_ms());
    record = app_node_comm_delivery_record_for_handle(handle);

    have_event = record != NULL && !record->backend_attempt_outstanding &&
        node_comm_delivery_backend_active_handle != handle &&
        node_comm_take_terminal_event_for(&node_comm_policy,
                                          handle,
                                          event_out);
    if (have_event) {
        app_node_comm_clear_delivery_record(handle);
    }
    app_node_comm_sync_unlock();
    return have_event;
}

int app_node_comm_delivery_attempts_started(uint32_t handle,
                                            uint8_t *attempts_out)
{
    int ret;

    if (handle == 0u || attempts_out == NULL) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    (void)app_node_comm_service_policy_locked(app_node_comm_now_ms());
    ret = node_comm_attempts_started(&node_comm_policy, handle, attempts_out);
    app_node_comm_sync_unlock();
    return ret;
}

size_t app_node_comm_pending_delivery_count(void)
{
    size_t count = 0u;

    if (app_node_comm_sync_lock() == 0) {
        (void)app_node_comm_service_policy_locked(app_node_comm_now_ms());
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

void app_node_comm_control_response_health_get(
    struct app_node_comm_control_response_health *health)
{
    if (health == NULL) {
        return;
    }
    memset(health, 0, sizeof(*health));
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    (void)app_node_comm_service_policy_locked(app_node_comm_now_ms());
    app_node_comm_reap_auto_terminal_events_locked();
    *health = node_comm_control_response_health;
    app_node_comm_sync_unlock();
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
        if (DEVICE_ROLE == ROLE_GATEWAY) {
            (void)k_work_cancel_delayable(&node_comm_delivery_due_kick_work);
            (void)mesh_node_comm_gateway_delivery_due_end();
        }
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
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        (void)k_work_cancel_delayable(&node_comm_delivery_due_kick_work);
        (void)mesh_node_comm_gateway_delivery_due_end();
    }
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
    return app_node_comm_request_route_refresh_correlated_bounded(
        delay_ms,
        reason,
        correlation,
        APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS);
}

int app_node_comm_request_route_refresh_correlated_bounded(
    uint32_t delay_ms,
    const char *reason,
    const struct proto_packet *correlation,
    uint32_t timeout_ms)
{
    int ret = app_node_comm_require_running();

    if (ret < 0) {
        return ret;
    }
    if (correlation == NULL) {
        return -EINVAL;
    }
    return app_node_comm_gateway_route_refresh_request_bounded(
        delay_ms,
        reason,
        true,
        correlation,
        timeout_ms);
}
