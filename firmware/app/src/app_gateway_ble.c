#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_control_sequence.h"
#include "app_durable_state.h"

#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"

#include "app_anchor.h"
#include "app_board.h"
#include "app_config.h"
#include "app_gateway_collection_eack.h"
#include "app_gateway_collection_recovery.h"
#include "app_gateway_eack_retry.h"
#include "app_gateway_eack_policy.h"
#include "app_gateway_command_observability.h"
#include "app_gateway_command_result.h"
#include "app_gateway_ble_stream.h"
#include "app_stack_workload_diag.h"
#include "app_watchdog.h"
#include "app_mesh_report.h"
#include "app_state.h"
#include "gateway_membership.h"
#include "gateway_ble_transport.h"
#include "discovery_assignment.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "serial_frame.h"

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_gateway_ble, LOG_LEVEL_DBG);

#define GATEWAY_BLE_VERBOSE_LOG(...) LOG_INF(__VA_ARGS__)

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
/* BlueZ normally opens the link with a four-second supervision timeout, while
 * Zephyr's generic peripheral preference later narrows it to 420 ms.  A busy
 * UWB protocol turn can legitimately defer BLE longer than that, so retain
 * the existing 30-50 ms interval but request a supervision horizon that
 * survives bounded radio ownership. */
#define GATEWAY_BLE_CONN_INTERVAL_MIN 24u
#define GATEWAY_BLE_CONN_INTERVAL_MAX 40u
#define GATEWAY_BLE_CONN_LATENCY 0u
#define GATEWAY_BLE_CONN_SUPERVISION_TIMEOUT 1200u

BUILD_ASSERT(GATEWAY_BLE_CONN_SUPERVISION_TIMEOUT <= 3200u,
             "BLE supervision timeout must fit the HCI 32-second bound");
BUILD_ASSERT(4u * GATEWAY_BLE_CONN_SUPERVISION_TIMEOUT >
                 (1u + GATEWAY_BLE_CONN_LATENCY) *
                     GATEWAY_BLE_CONN_INTERVAL_MAX,
             "BLE supervision timeout must exceed the interval/latency floor");

static const struct bt_le_conn_param gateway_ble_conn_params =
    BT_LE_CONN_PARAM_INIT(GATEWAY_BLE_CONN_INTERVAL_MIN,
                          GATEWAY_BLE_CONN_INTERVAL_MAX,
                          GATEWAY_BLE_CONN_LATENCY,
                          GATEWAY_BLE_CONN_SUPERVISION_TIMEOUT);
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

static int gateway_command_delivery_boundary(void *ctx,
                                             const struct proto_packet *command,
                                             enum command_id command_id,
                                             enum command_status status,
                                             uint8_t reason)
{
    ARG_UNUSED(ctx);
    gateway_command_result_side_effects(command, command_id, status, reason);
    return 0;
}
#endif

static uint16_t gateway_command_seq;
static struct k_spinlock gateway_command_seq_lock;
static struct gateway_ble_stream_state gateway_ble_stream_state;
static struct gateway_command_observability_state gateway_command_observability_state;
static struct k_spinlock gateway_ble_stream_lock;
static struct k_spinlock gateway_command_observability_lock;
#if DEVICE_ROLE == ROLE_GATEWAY
/* RAM-only duplicate-receipt aid; no stream receipt history is persisted. */
static struct proto_packet gateway_command_event_last_host_receipt;
static uint8_t gateway_command_event_last_host_receipt_digest[
    SEMANTIC_DIGEST_SHA256_LEN];
static bool gateway_command_event_last_host_receipt_valid;
#endif

BUILD_ASSERT(GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH ==
             APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH,
             "every accepted host result credit needs terminal event custody");
/*
 * A full durable assignment publishes at most one mapping per slot plus
 * enumeration-complete, table-ready, and terminal events.  The receipt wire
 * projection can burn one extra reserved identity at a low-word-zero
 * boundary, so include it in the admission proof rather than borrowing the
 * protected control-sequence runway after assignment has begun.
 */
#define GATEWAY_ASSIGNMENT_PUBLISHER_EVENT_BUDGET \
    (APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES + 3u)
#define GATEWAY_HOST_RECEIPT_SEQUENCE_SKIP_BUDGET 1u
BUILD_ASSERT(APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET >=
                 GATEWAY_ASSIGNMENT_PUBLISHER_EVENT_BUDGET +
                     GATEWAY_HOST_RECEIPT_SEQUENCE_SKIP_BUDGET,
             "assignment admission must cover host-receipt sequence skip");

static bool gateway_ble_stream_ready(void);
static void gateway_ble_schedule_stream_drain(void);
static void gateway_ble_resume_rx(void);
#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observability_flush(bool include_snapshots);
#endif

static int gateway_observability_reserve_identity(
    struct gateway_command_event *event)
{
    uint32_t event_seq;
    int ret;

    if (event == NULL) {
        return -EINVAL;
    }
    if (event->event_seq != 0u) {
        return 0;
    }

    /* This takes only the RAM sequence allocator mutex, so callers must run it
     * before taking BLE, stream, or observability spinlocks. */
    ret = app_gateway_control_sequence_next_receiptable(&event_seq);
    if (ret < 0) {
        LOG_ERR("gateway host-event sequence unavailable: %d", ret);
        return ret;
    }
    event->event_seq = event_seq;
    return 0;
}

static int gateway_observability_prepare_reserved_state(
    struct gateway_command_event *event,
    bool terminal)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_command_observability_lock);
    int ret = gateway_command_observability_prepare_with_sequence(
        &gateway_command_observability_state,
        event,
        terminal,
        event == NULL ? 0u : event->event_seq);

    k_spin_unlock(&gateway_command_observability_lock, key);
    return ret;
}

static int gateway_observability_prepare_state(
    struct gateway_command_event *event,
    bool terminal)
{
    int ret = gateway_observability_reserve_identity(event);

    return ret < 0 ?
           ret :
           gateway_observability_prepare_reserved_state(event, terminal);
}

static void gateway_observability_note_enqueue_state(uint32_t event_seq,
                                                     int enqueue_result)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_command_observability_lock);

    gateway_command_observability_note_enqueue(
        &gateway_command_observability_state, event_seq, enqueue_result);
    k_spin_unlock(&gateway_command_observability_lock, key);
}

static bool gateway_observability_pending_terminal_state(
    struct gateway_command_event *event)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_command_observability_lock);
    bool pending = gateway_command_observability_pending_terminal(
        &gateway_command_observability_state, event);

    k_spin_unlock(&gateway_command_observability_lock, key);
    return pending;
}

static bool gateway_observability_snapshot_state(
    enum gateway_command_event_kind kind,
    bool include_sent,
    struct gateway_command_event *event)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_command_observability_lock);
    bool available = include_sent ?
        gateway_command_observability_reconnect_snapshot(
            &gateway_command_observability_state, kind, event) :
        gateway_command_observability_pending_snapshot(
            &gateway_command_observability_state, kind, event);

    k_spin_unlock(&gateway_command_observability_lock, key);
    return available;
}

static void gateway_observability_mark_sent_state(uint32_t event_seq)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_command_observability_lock);

    gateway_command_observability_mark_sent(
        &gateway_command_observability_state, event_seq);
    k_spin_unlock(&gateway_command_observability_lock, key);
}

uint16_t gateway_next_command_seq(void)
{
    uint16_t sequence;
    k_spinlock_key_t key = k_spin_lock(&gateway_command_seq_lock);

    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    sequence = gateway_command_seq;
    k_spin_unlock(&gateway_command_seq_lock, key);
    return sequence;
}

int gateway_broadcast_command_sequence_init(void)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    return app_gateway_control_sequence_init();
}

uint32_t gateway_next_broadcast_command_seq(void)
{
    uint32_t sequence = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0u;
    }
    ret = app_gateway_control_sequence_next(&sequence);
    if (ret < 0) {
        LOG_ERR("gateway broadcast command sequence unavailable: %d", ret);
    }
    return sequence;
}

static int gateway_observability_enqueue_prepared(
    const struct gateway_command_event *event)
{
    struct proto_packet packet = {0};
    uint8_t payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t payload_len = 0u;
    k_spinlock_key_t key;
    bool ready;
    int ret;

    if (event == NULL) {
        return -EINVAL;
    }
    ret = gateway_command_event_encode(event,
                                       payload,
                                       sizeof(payload),
                                       &payload_len);
    if (ret < 0) {
        return ret;
    }
    packet.msg_type = MSG_GATEWAY_COMMAND_EVENT;
    /* Every command event is BLE-only retained telemetry.  ACK_REQUIRED gives
     * it an exact GUI-receipt boundary; assignment publication still owns its
     * separate semantic cursor and is the only path allowed to advance that
     * publisher below. */
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet.src_id = DEVICE_ID;
    packet.dst_id = DEVICE_ID;
    packet.session_id = event->event_seq;
    packet.seq = (uint16_t)event->event_seq;
    packet.payload_len = (uint16_t)payload_len;
    ready = gateway_ble_stream_ready();
    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_enqueue_retained_packet(
        &gateway_ble_stream_state,
        &packet,
        payload,
        payload_len,
        k_uptime_get_32(),
        k_uptime_get_32(),
        ready);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
        ret = 0;
    }
    gateway_observability_note_enqueue_state(event->event_seq, ret);
    return ret;
}

int gateway_observe_command_event(struct gateway_command_event *event,
                                  bool terminal)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || event == NULL) {
        return -EINVAL;
    }
    if (terminal &&
        app_gateway_assignment_publisher_capture_terminal(event)) {
        return 0;
    }
    ret = gateway_observability_prepare_state(event, terminal);
    if (ret < 0) {
        return ret;
    }
    return gateway_observability_enqueue_prepared(event);
}

int gateway_reserve_command_event_sequence(struct gateway_command_event *event,
                                           void *ctx)
{
    ARG_UNUSED(ctx);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    return gateway_observability_reserve_identity(event);
}

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observability_flush(bool include_snapshots)
{
    struct gateway_command_event event;

    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_ble_stream_ready()) {
        return;
    }
    if (gateway_observability_pending_terminal_state(&event)) {
        (void)gateway_observability_enqueue_prepared(&event);
    }
    for (enum gateway_command_event_kind kind =
             GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
         kind <= GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH;
         kind++) {
        bool available = gateway_observability_snapshot_state(
            kind, include_snapshots, &event);

        if (available) {
            (void)gateway_observability_enqueue_prepared(&event);
        }
    }
}
#endif

/* Result and collection custody stay in this translation unit. */
#include "app_gateway_result_runtime.inc"

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
#define BT_UUID_IMEC_GATEWAY_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000001ULL)
#define BT_UUID_IMEC_GATEWAY_PACKET_TX_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000002ULL)
#define BT_UUID_IMEC_GATEWAY_PACKET_RX_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000003ULL)
#define BT_UUID_IMEC_GATEWAY_IDENTITY_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000005ULL)

#define BT_UUID_IMEC_GATEWAY_SERVICE BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_SERVICE_VAL)
#define BT_UUID_IMEC_GATEWAY_PACKET_TX BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_PACKET_TX_VAL)
#define BT_UUID_IMEC_GATEWAY_PACKET_RX BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_PACKET_RX_VAL)
#define BT_UUID_IMEC_GATEWAY_IDENTITY BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_IDENTITY_VAL)

#define GATEWAY_BLE_PACKET_TX_ATTR_INDEX 2u
#define GATEWAY_BLE_DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define GATEWAY_BLE_DEVICE_NAME_LEN (sizeof(GATEWAY_BLE_DEVICE_NAME) - 1u)
#define GATEWAY_BLE_PACKET_TX_FRAME_MAX_LEN SERIAL_FRAME_MAX_LEN
#define GATEWAY_BLE_PACKET_TX_CHUNK_MAX_LEN CONFIG_BT_L2CAP_TX_MTU
#define GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS 10000u
#define GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD 8u
#define GATEWAY_BLE_HOST_RECEIPT_TIMEOUT_MS 1000u

BUILD_ASSERT(GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS > 0u &&
             GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS < INT32_MAX,
             "BLE notification completion deadline must be wrap safe");
BUILD_ASSERT(GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD > 0u,
             "BLE synchronous notification failures need a reset bound");
BUILD_ASSERT(GATEWAY_BLE_HOST_RECEIPT_TIMEOUT_MS > 0u &&
             GATEWAY_BLE_HOST_RECEIPT_TIMEOUT_MS < INT32_MAX,
             "host receipt timeout must be wrap safe");

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY
BUILD_ASSERT(CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE >= 4096u,
             "gateway BLE ingress needs the verified 4 KiB admission stack");
#endif

enum gateway_ble_tx_source {
    GATEWAY_BLE_TX_NONE = 0,
    GATEWAY_BLE_TX_DIRECT,
    GATEWAY_BLE_TX_STREAM,
};

struct gateway_ble_frame_pending {
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    uint16_t len;
};

K_MSGQ_DEFINE(gateway_ble_rx_msgq,
              sizeof(struct gateway_ble_frame_pending),
              GATEWAY_BLE_RX_FRAME_QUEUE_DEPTH,
              4);

static struct k_work gateway_ble_rx_work;
static struct k_work_delayable gateway_ble_stream_work;
static struct k_work_delayable gateway_ble_host_receipt_timeout_work;
static struct k_work_delayable gateway_ble_recovery_work;
static struct bt_conn *gateway_ble_conn;
static bool gateway_ble_advertising_active;
static bool gateway_ble_stack_ready;
static uint8_t gateway_ble_recovery_round;
static uint8_t gateway_ble_notify_failure_count;
static bool gateway_ble_packet_notify_enabled;
static uint8_t gateway_ble_uwb_quiet_depth;
static bool gateway_ble_quiet_stopped_advertising;
static uint8_t gateway_ble_rx_frame[SERIAL_FRAME_MAX_LEN];
static size_t gateway_ble_rx_len;
static bool gateway_ble_rx_overflow;
static struct k_spinlock gateway_ble_tx_lock;
static enum gateway_ble_tx_source gateway_ble_tx_source;
static uint8_t gateway_ble_tx_frame[GATEWAY_BLE_PACKET_TX_FRAME_MAX_LEN];
static size_t gateway_ble_tx_frame_len;
static size_t gateway_ble_tx_frame_offset;
static uint8_t gateway_ble_tx_chunk[GATEWAY_BLE_PACKET_TX_CHUNK_MAX_LEN];
static size_t gateway_ble_tx_chunk_len;
static bool gateway_ble_tx_in_flight;
static uint32_t gateway_ble_tx_generation;
static uint32_t gateway_ble_tx_deadline_ms;
static struct gateway_ble_frame_pending
    gateway_ble_direct_tx_queue[GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH];
static struct gateway_ble_direct_queue_state gateway_ble_direct_tx_state;

static const struct bt_data gateway_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, GATEWAY_BLE_DEVICE_NAME, GATEWAY_BLE_DEVICE_NAME_LEN),
};

static const struct bt_data gateway_ble_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_IMEC_GATEWAY_SERVICE_VAL),
};

static int gateway_ble_rx_bytes(const uint8_t *data, size_t len);
static int gateway_ble_start_advertising(void);
static int gateway_ble_stop_advertising(const char *reason);
static void gateway_ble_rx_work_handler(struct k_work *work);
static void gateway_ble_stream_work_handler(struct k_work *work);
static void gateway_ble_host_receipt_timeout_work_handler(
    struct k_work *work);
static void gateway_ble_recovery_work_handler(struct k_work *work);
static void gateway_ble_tx_complete(struct bt_conn *conn, void *user_data);
static void gateway_ble_tx_reset_locked(void);
static void gateway_ble_stream_cancel_active(void);
static void gateway_ble_reset_stalled_link(uint32_t expected_generation,
                                           uint32_t expected_deadline_ms,
                                           const char *reason);
static void gateway_ble_reset_failed_link(uint32_t expected_generation,
                                          const char *reason);

static void gateway_ble_schedule_failed(const char *owner, int ret)
{
    LOG_ERR("gateway BLE work ownership failed closed: owner=%s ret=%d",
            owner == NULL ? "unknown" : owner,
            ret);
    /*
     * A queue or connection state may already hold accepted bytes.  If no
     * work item owns their next transition, the hardware watchdog is the only
     * safe terminal owner; continuing would silently strand host custody.
     */
    app_watchdog_stop_feeding();
}

static int gateway_ble_rx_write_capacity(const uint8_t *data, size_t len)
{
    uint32_t completed_frames = 0u;

    if (data == NULL && len != 0u) {
        return -EINVAL;
    }
    /*
     * COBS excludes zero from an encoded frame, so every delimiter in this
     * write completes at most one frame.  Reserve enough raw-ingress slots
     * before consuming any bytes from the ATT write.  This keeps a failed
     * write request retryable at the same chunk boundary and prevents a
     * coalesced write from admitting a prefix before reporting ENOSPC.
     */
    for (size_t i = 0u; i < len; i++) {
        if (data[i] == SERIAL_FRAME_DELIMITER) {
            completed_frames++;
        }
    }
    return completed_frames <=
               (uint32_t)k_msgq_num_free_get(&gateway_ble_rx_msgq) ?
           0 : -ENOSPC;
}

static void gateway_ble_resume_rx(void)
{
    int ret = k_work_submit(&gateway_ble_rx_work);

    if (gateway_ble_work_handoff_requires_reset(ret)) {
        gateway_ble_schedule_failed("rx", ret);
    }
}

static void gateway_ble_schedule_recovery(const char *reason)
{
    uint32_t delay_ms;
    int ret;

    if (!gateway_ble_transport_enabled() || gateway_ble_conn != NULL) {
        return;
    }
    delay_ms = gateway_ble_recovery_backoff_ms(gateway_ble_recovery_round,
                                               sys_rand32_get());
    if (gateway_ble_recovery_round < UINT8_MAX) {
        gateway_ble_recovery_round++;
    }
    ret = k_work_reschedule(&gateway_ble_recovery_work, K_MSEC(delay_ms));
    if (gateway_ble_work_handoff_requires_reset(ret)) {
        gateway_ble_schedule_failed("recovery", ret);
        return;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE event=recovery round=%u delay_ms=%u reason=%s uptime=%u\n",
                        gateway_ble_recovery_round,
                        delay_ms,
                        reason == NULL ? "unknown" : reason,
                        k_uptime_get_32());
#endif
    LOG_WRN("gateway BLE recovery scheduled: reason=%s round=%u delay_ms=%u",
            reason == NULL ? "unknown" : reason,
            gateway_ble_recovery_round,
            delay_ms);
}

static bool gateway_ble_stream_ready(void)
{
    return gateway_ble_transport_enabled() &&
           !gateway_ble_uwb_quiet_active() &&
           gateway_ble_conn != NULL &&
           gateway_ble_packet_notify_enabled;
}

static void gateway_ble_schedule_stream_drain(void)
{
    if (gateway_ble_transport_enabled()) {
        int ret = k_work_reschedule(&gateway_ble_stream_work, K_NO_WAIT);

        if (gateway_ble_work_handoff_requires_reset(ret)) {
            gateway_ble_schedule_failed("stream-drain", ret);
        }
    }
}

static uint32_t gateway_ble_stream_retry_delay_ms(uint8_t failure_count)
{
    uint8_t shift = MIN(failure_count, 6u);
    uint32_t delay_ms = GATEWAY_BLE_TX_RETRY_MS << shift;

    return MIN(delay_ms, GATEWAY_BLE_TX_RETRY_MAX_MS);
}

static void gateway_ble_schedule_stream_retry(uint8_t failure_count)
{
    if (gateway_ble_transport_enabled()) {
        int ret = k_work_reschedule(
            &gateway_ble_stream_work,
            K_MSEC(gateway_ble_stream_retry_delay_ms(failure_count)));

        if (gateway_ble_work_handoff_requires_reset(ret)) {
            gateway_ble_schedule_failed("stream-retry", ret);
        }
    }
}

static void gateway_ble_schedule_in_flight_deadline(uint32_t delay_ms)
{
    int ret = k_work_reschedule(
        &gateway_ble_stream_work, K_MSEC(MAX(delay_ms, 1u)));

    if (gateway_ble_work_handoff_requires_reset(ret)) {
        gateway_ble_schedule_failed("notify-deadline", ret);
    }
}

bool gateway_ble_uwb_quiet_active(void)
{
    return gateway_ble_uwb_quiet_depth > 0u;
}

static bool gateway_ble_keep_active_during_uwb(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST);
}

void gateway_ble_enter_uwb_quiet(const char *reason)
{
    if (!gateway_ble_transport_enabled()) {
        return;
    }
    if (gateway_ble_keep_active_during_uwb()) {
        return;
    }

    if (gateway_ble_uwb_quiet_depth < UINT8_MAX) {
        gateway_ble_uwb_quiet_depth++;
    }
    if (gateway_ble_uwb_quiet_depth != 1u) {
        return;
    }

    if (gateway_ble_advertising_active) {
        int ret = gateway_ble_stop_advertising(reason);

        gateway_ble_quiet_stopped_advertising = ret == 0;
    }
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE quiet during UWB: reason=%s",
            reason == NULL ? "unknown" : reason);
}

void gateway_ble_exit_uwb_quiet(const char *reason)
{
    if (!gateway_ble_transport_enabled() || gateway_ble_uwb_quiet_depth == 0u) {
        return;
    }

    gateway_ble_uwb_quiet_depth--;
    if (gateway_ble_uwb_quiet_depth != 0u) {
        return;
    }

    if (gateway_ble_quiet_stopped_advertising && gateway_ble_conn == NULL) {
        int ret = gateway_ble_start_advertising();

        if (ret >= 0) {
            gateway_ble_recovery_round = 0u;
        }
    }
    if (gateway_ble_conn == NULL && !gateway_ble_advertising_active) {
        gateway_ble_schedule_recovery("uwb-quiet-exit");
    }
    gateway_ble_quiet_stopped_advertising = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    app_gateway_assignment_publisher_pump();
    (void)gateway_flush_host_command_results();
#endif
    gateway_ble_schedule_stream_drain();
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE resumed after UWB: reason=%s",
            reason == NULL ? "unknown" : reason);
}

static void gateway_ble_packet_ccc_changed(const struct bt_gatt_attr *attr,
                                           uint16_t value)
{
    bool notify_enabled = value == BT_GATT_CCC_NOTIFY;
    k_spinlock_key_t key;

    ARG_UNUSED(attr);

    key = k_spin_lock(&gateway_ble_tx_lock);
    gateway_ble_packet_notify_enabled = notify_enabled;
    if (!notify_enabled) {
        /*
         * A partial ATT frame is not complete host custody.  Invalidate its
         * callback generation and retain the selected queue head so a later
         * subscription restarts the exact frame from byte zero.
         */
        gateway_ble_tx_reset_locked();
    }
    k_spin_unlock(&gateway_ble_tx_lock, key);
    if (!notify_enabled) {
        gateway_ble_stream_cancel_active();
        return;
    }

    if (notify_enabled) {
#if DEVICE_ROLE == ROLE_GATEWAY
        (void)gateway_flush_host_command_results();
#endif
        gateway_observability_flush(true);
#if DEVICE_ROLE == ROLE_GATEWAY
        app_gateway_assignment_publisher_pump();
#endif
        gateway_ble_schedule_stream_drain();
    }
}

static ssize_t gateway_ble_packet_rx_write(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           const void *buf,
                                           uint16_t len,
                                           uint16_t offset,
                                           uint8_t flags)
{
    bool write_command = (flags & BT_GATT_WRITE_FLAG_CMD) != 0u;

    ARG_UNUSED(attr);

    if (!gateway_ble_transport_enabled()) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    {
        int ret = gateway_ble_rx_write_capacity(buf, len);

        if (ret == 0) {
            ret = gateway_ble_rx_bytes(buf, len);
        }
        if (ret < 0) {
            LOG_ERR("gateway BLE command ingress failed closed: len=%u ret=%d flags=0x%02x",
                    len, ret, flags);
            /*
             * A write request receives the ATT error returned below.  A write
             * command has no response, so disconnect it explicitly to make the
            * failed admission observable and force a complete-frame retry.
             */
            if (write_command && conn != NULL) {
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
                status_debug_printf(
                    "DBG_GATEWAY_BLE event=local-disconnect cause=ingress ret=%d len=%u flags=0x%02x uptime=%u\n",
                    ret,
                    len,
                    flags,
                    k_uptime_get_32());
#endif
                (void)bt_conn_disconnect(conn,
                                         BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            }
            return BT_GATT_ERR(ret == -EMSGSIZE ?
                               BT_ATT_ERR_INVALID_ATTRIBUTE_LEN :
                               BT_ATT_ERR_INSUFFICIENT_RESOURCES);
        }
    }
    return len;
}

static ssize_t gateway_ble_identity_read(struct bt_conn *conn,
                                         const struct bt_gatt_attr *attr,
                                         void *buf,
                                         uint16_t len,
                                         uint16_t offset)
{
    uint8_t identity[sizeof(uint64_t)];

    sys_put_le64(DEVICE_ID, identity);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, identity, sizeof(identity));
}

BT_GATT_SERVICE_DEFINE(gateway_ble_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_IMEC_GATEWAY_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMEC_GATEWAY_PACKET_TX,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           NULL, NULL, NULL),
    BT_GATT_CCC(gateway_ble_packet_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMEC_GATEWAY_PACKET_RX,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, gateway_ble_packet_rx_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_IMEC_GATEWAY_IDENTITY,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           gateway_ble_identity_read, NULL, NULL),
);

static uint16_t gateway_ble_notify_chunk_len(struct bt_conn *conn)
{
    if (conn == NULL) {
        return GATEWAY_BLE_DEFAULT_NOTIFY_CHUNK;
    }
    return gateway_ble_att_payload_max(bt_gatt_get_mtu(conn));
}

static void gateway_ble_tx_reset_locked(void)
{
    gateway_ble_tx_source = GATEWAY_BLE_TX_NONE;
    gateway_ble_tx_frame_len = 0u;
    gateway_ble_tx_frame_offset = 0u;
    gateway_ble_tx_chunk_len = 0u;
    gateway_ble_tx_in_flight = false;
    gateway_ble_tx_deadline_ms = 0u;
    gateway_ble_notify_failure_count = 0u;
    gateway_ble_direct_queue_cancel(&gateway_ble_direct_tx_state);
    gateway_ble_tx_generation++;
    if (gateway_ble_tx_generation == 0u) {
        gateway_ble_tx_generation = 1u;
    }
}

static void gateway_ble_stream_cancel_active(void)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_ble_stream_lock);

    gateway_ble_stream_cancel_send(&gateway_ble_stream_state);
    (void)gateway_ble_stream_rewind_host_notification(
        &gateway_ble_stream_state);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    (void)k_work_cancel_delayable(&gateway_ble_host_receipt_timeout_work);
}

static void gateway_ble_host_receipt_timeout_work_handler(
    struct k_work *work)
{
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);
    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_rewind_host_notification(
        &gateway_ble_stream_state);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret == 0) {
        LOG_WRN("gateway host receipt timeout; resending exact BLE record");
        gateway_ble_schedule_stream_drain();
    } else if (ret != -EALREADY && ret != -EAGAIN && ret != -ENOENT) {
        LOG_ERR("gateway host receipt timeout rewind failed: ret=%d", ret);
        gateway_ble_schedule_failed("host-receipt-timeout", ret);
    }
}

static bool gateway_ble_tx_select_frame_locked(void)
{
    if (gateway_ble_tx_source != GATEWAY_BLE_TX_NONE) {
        return true;
    }

    if (gateway_ble_direct_tx_state.count > 0u &&
        gateway_ble_packet_notify_enabled) {
        uint8_t head_slot = 0u;
        int ret = gateway_ble_direct_queue_begin(
            &gateway_ble_direct_tx_state,
            GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH,
            &head_slot);
        const struct gateway_ble_frame_pending *pending;

        if (ret < 0) {
            if (ret != -EBUSY && ret != -ENOENT) {
                app_watchdog_stop_feeding();
            }
            return false;
        }
        pending = &gateway_ble_direct_tx_queue[head_slot];

        memcpy(gateway_ble_tx_frame, pending->frame, pending->len);
        gateway_ble_tx_frame_len = pending->len;
        gateway_ble_tx_frame_offset = 0u;
        gateway_ble_tx_source = GATEWAY_BLE_TX_DIRECT;
        return true;
    }

    if (gateway_ble_stream_ready()) {
        const uint8_t *record = NULL;
        size_t record_len = 0u;
        k_spinlock_key_t key = k_spin_lock(&gateway_ble_stream_lock);
        int ret = gateway_ble_stream_begin_send_view(&gateway_ble_stream_state,
                                                     &record,
                                                     &record_len);

        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (ret == 0) {
            ARG_UNUSED(record);
            gateway_ble_tx_frame_len = record_len;
            gateway_ble_tx_frame_offset = 0u;
            gateway_ble_tx_source = GATEWAY_BLE_TX_STREAM;
            return true;
        }
    }

    return false;
}

int gateway_ble_send_packet_frame(const uint8_t *frame, size_t frame_len)
{
    struct gateway_ble_frame_pending *pending;
    uint8_t slot = 0u;
    int ret;
    k_spinlock_key_t key;

    if (frame == NULL || frame_len == 0u) {
        return -EINVAL;
    }
    if (frame_len > SERIAL_FRAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    if (gateway_ble_uwb_quiet_active()) {
        return -EAGAIN;
    }

    key = k_spin_lock(&gateway_ble_tx_lock);
    if (gateway_ble_conn == NULL) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return -ENOTCONN;
    }
    if (!gateway_ble_packet_notify_enabled) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return -EACCES;
    }
    ret = gateway_ble_direct_queue_enqueue(
        &gateway_ble_direct_tx_state,
        GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH,
        &slot);
    if (ret < 0) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return ret;
    }
    pending = &gateway_ble_direct_tx_queue[slot];
    memcpy(pending->frame, frame, frame_len);
    pending->len = (uint16_t)frame_len;
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_schedule_stream_drain();
    return 0;
}

/* Packets whose BLE completion owns a source-custody ACK boundary. */
static bool gateway_host_custody_supported(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    /* Only the durable assignment publisher is command-event host custody.
     * Generic observability packets are self-addressed best-effort telemetry. */
    if (packet->msg_type == MSG_GATEWAY_COMMAND_EVENT) {
        return packet->flags == FLAG_GATEWAY_ACK_REQUIRED;
    }
    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    case MSG_MESH_DATA:
        return (packet->flags & FLAG_DIAGNOSTIC) != 0u;
    default:
        return false;
    }
}

int gateway_command_event_finish_host_receipt(
    const struct proto_packet *packet)
{
    if (packet == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type != MSG_GATEWAY_COMMAND_EVENT) {
        return 0;
    }

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE) && \
    DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_command_event event;
    struct gateway_ble_stream_item *head;
    struct proto_packet head_packet;
    const uint8_t *payload;
    uint8_t record_digest[SEMANTIC_DIGEST_SHA256_LEN];
    k_spinlock_key_t key;
    bool publisher_owned;
    int publisher_ret = 0;
    int ret = 0;

    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.count == 0u) {
        ret = -ESTALE;
        goto unlock;
    }
    head = &gateway_ble_stream_state.items[0];
    if (!gateway_ble_packet_identity_matches(packet, &head->packet)) {
        ret = -ESTALE;
        goto unlock;
    }
    if (gateway_ble_stream_state.head_send_phase !=
        GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED) {
        ret = -EAGAIN;
        goto unlock;
    }
    if (head->offset > gateway_ble_stream_state.pool_used ||
        head->len < GATEWAY_BLE_STREAM_RECORD_HEADER_LEN ||
        (size_t)head->len >
            (size_t)gateway_ble_stream_state.pool_used - head->offset ||
        head->packet.payload_len !=
            (uint16_t)(head->len - GATEWAY_BLE_STREAM_RECORD_HEADER_LEN)) {
        ret = -EPROTO;
        goto unlock;
    }
    payload = &gateway_ble_stream_state.record_pool[
        head->offset + GATEWAY_BLE_STREAM_RECORD_HEADER_LEN];
    if (packet->flags != FLAG_GATEWAY_ACK_REQUIRED ||
        gateway_command_event_decode(payload,
                                     head->packet.payload_len,
                                     &event) < 0 ||
        event.event_seq == 0u ||
        event.event_seq != head->packet.session_id ||
        head->packet.seq != (uint16_t)event.event_seq) {
        ret = -EBADMSG;
        goto unlock;
    }
    if (!semantic_digest_sha256(
            &gateway_ble_stream_state.record_pool[head->offset],
            head->len,
            record_digest)) {
        ret = -EIO;
        goto unlock;
    }
    head_packet = head->packet;
unlock:
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret < 0) {
        return ret;
    }

    publisher_owned = false;
    if (app_gateway_assignment_publisher_event_is_reliable(&event)) {
        /* The publisher is the semantic owner of assignment mapping progress.
         * It must accept the exact event before the host-only stream head can
         * retire, otherwise a stream/publisher drift would strand publication.
         * A shape-valid event with no live publisher owner is ordinary bounded
         * observability, such as a no-anchor terminal before TABLE commit. */
        publisher_ret = app_gateway_assignment_publisher_note_host_receipt(
            &event);
        if (publisher_ret < 0) {
            return publisher_ret;
        }
        publisher_owned = publisher_ret > 0;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.count == 0u ||
        !gateway_ble_packet_identity_matches(
            packet, &gateway_ble_stream_state.items[0].packet)) {
        ret = -ESTALE;
    } else if (gateway_ble_stream_state.head_send_phase !=
               GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED) {
        ret = -EAGAIN;
    } else {
        gateway_command_event_last_host_receipt = head_packet;
        memcpy(gateway_command_event_last_host_receipt_digest,
               record_digest,
               sizeof(record_digest));
        gateway_command_event_last_host_receipt_valid = true;
        gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                     k_uptime_get_32());
        ret = 0;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret < 0) {
        return ret;
    }

    gateway_observability_mark_sent_state(event.event_seq);
    /* The receipt is the semantic edge that makes the successor publishable.
     * Pump it directly while the BLE stream is live; routing that edge only
     * through mesh-route work can strand a host publication behind unrelated
     * radio custody.  Keep one prompt route-owned fallback for terminal
     * durability or queue pressure, but do not compound its backoff across a
     * burst whose exact receipts are making forward progress. */
    if (publisher_owned && publisher_ret > 0) {
        gateway_persistence_retry_round = 0u;
        gateway_schedule_persistence_retry(
            "assignment-publication-host-receipt");
    }
    /*
     * Every accepted host receipt frees one retained BLE stream slot.  The
     * assignment publisher can be waiting for that capacity even when this
     * particular event came from live operation progress rather than from
     * the publisher itself.  Make the capacity edge level-triggered here;
     * UWB-quiet exit provides the matching retry when radio ownership wins
     * the opposite ordering.
     */
    app_gateway_assignment_publisher_pump();
    gateway_observability_flush(false);
    gateway_ble_schedule_stream_drain();
    return 0;
#else
    return -ENOTSUP;
#endif
}

int gateway_command_event_duplicate_host_receipt_valid(
    const struct gateway_host_receipt_identity *identity)
{
    if (identity == NULL ||
        identity->original_msg_type != MSG_GATEWAY_COMMAND_EVENT) {
        return -ESTALE;
    }

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE) && \
    DEVICE_ROLE == ROLE_GATEWAY
    k_spinlock_key_t key;
    bool matched;

    key = k_spin_lock(&gateway_ble_stream_lock);
    matched = gateway_command_event_last_host_receipt_valid &&
              gateway_host_receipt_identity_matches(
                  identity,
                  &gateway_command_event_last_host_receipt,
                  gateway_command_event_last_host_receipt_digest);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    return matched ? 0 : -ESTALE;
#else
    return -ENOTSUP;
#endif
}

static void gateway_ble_tx_complete(struct bt_conn *conn, void *user_data)
{
    enum gateway_ble_tx_source completed_source = GATEWAY_BLE_TX_NONE;
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    bool frame_complete = false;
    bool direct_completion_failed = false;
    k_spinlock_key_t key;

    ARG_UNUSED(conn);

    key = k_spin_lock(&gateway_ble_tx_lock);
    if (!gateway_ble_tx_in_flight || generation != gateway_ble_tx_generation) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }

    gateway_ble_tx_in_flight = false;
    gateway_ble_tx_deadline_ms = 0u;
    gateway_ble_notify_failure_count = 0u;
    gateway_ble_tx_frame_offset += gateway_ble_tx_chunk_len;
    gateway_ble_tx_chunk_len = 0u;
    frame_complete = gateway_ble_tx_frame_offset >= gateway_ble_tx_frame_len;
    if (frame_complete) {
        completed_source = gateway_ble_tx_source;
        if (completed_source == GATEWAY_BLE_TX_DIRECT &&
            gateway_ble_direct_queue_complete(
                &gateway_ble_direct_tx_state,
                GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH) < 0) {
            direct_completion_failed = true;
        }
        gateway_ble_tx_source = GATEWAY_BLE_TX_NONE;
        gateway_ble_tx_frame_len = 0u;
        gateway_ble_tx_frame_offset = 0u;
    }
    k_spin_unlock(&gateway_ble_tx_lock, key);

    if (direct_completion_failed) {
        gateway_ble_schedule_failed("direct-complete", -EIO);
        return;
    }

    if (completed_source == GATEWAY_BLE_TX_STREAM) {
        struct proto_packet completed_packet;
        bool unretained_command_event = false;
        bool host_custody_pending = false;
        bool host_custody_supported = false;
        bool host_notification_transitioned = false;
        int host_notification_ret = -EAGAIN;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_stream_lock);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &completed_packet);
        host_custody_supported =
            packet_ret == 0 && gateway_ble_stream_state.count > 0u &&
            gateway_ble_stream_state.items[0].retain_until_sent &&
            gateway_host_custody_supported(&completed_packet);
        if (host_custody_supported) {
            host_notification_ret = gateway_ble_stream_mark_host_notified(
                &gateway_ble_stream_state);
            host_notification_transitioned = host_notification_ret == 0;
            host_custody_pending = host_notification_transitioned ||
                                   host_notification_ret == -EALREADY;
            if (host_notification_ret < 0 &&
                host_notification_ret != -EALREADY) {
                LOG_ERR("gateway host-notified transition rejected: ret=%d",
                        host_notification_ret);
                /* Keep the retained record and fail closed; retiring it here
                 * would release mesh custody without a GUI receipt. */
                host_custody_pending = true;
                gateway_ble_schedule_failed("host-notified-transition",
                                            host_notification_ret);
            }
        }
        unretained_command_event =
            packet_ret == 0 &&
            completed_packet.msg_type == MSG_GATEWAY_COMMAND_EVENT &&
            !host_custody_supported;
        if (!host_custody_supported && !unretained_command_event) {
            gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                         k_uptime_get_32());
        }
        uint16_t queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (packet_ret == 0) {
            if (unretained_command_event) {
                /* Every command event must retain an exact host-receipt owner.
                 * Reaching ATT completion without one is an impossible custody
                 * shape, so keep the stream head and fail closed. */
                gateway_ble_schedule_failed(
                    "unretained-command-observability", -EPROTO);
            }
#if DEVICE_ROLE == ROLE_GATEWAY
            if (host_custody_supported) {
                if (!host_custody_pending) {
                    LOG_WRN("gateway host-custody completion phase invalid: %u",
                            gateway_ble_stream_state.head_send_phase);
                }
            }
#endif
            if (host_notification_transitioned) {
                int receipt_schedule_ret = k_work_reschedule(
                    &gateway_ble_host_receipt_timeout_work,
                    K_MSEC(GATEWAY_BLE_HOST_RECEIPT_TIMEOUT_MS));

                if (receipt_schedule_ret < 0) {
                    LOG_ERR("gateway host receipt timeout scheduling failed: ret=%d",
                            receipt_schedule_ret);
                    gateway_ble_schedule_failed("host-receipt-timeout-schedule",
                                                receipt_schedule_ret);
                }
            }
            const struct app_stack_workload_diag_pressure pressure = {
                .queue_depth = queue_depth,
                .custody_depth = gateway_ble_tx_in_flight ? 1u : 0u,
                .credit_available = gateway_ble_tx_in_flight ? 0u : 1u,
                .retry_depth = gateway_ble_notify_failure_count,
                .drain_depth = queue_depth,
            };

            app_stack_workload_diag_ble_terminal_with_pressure(
                &completed_packet, APP_STACK_DIAG_TERMINAL_ACK, &pressure);
        }
        gateway_observability_flush(false);
#if DEVICE_ROLE == ROLE_GATEWAY
        (void)gateway_flush_host_command_results();
#endif
    }
    gateway_ble_schedule_stream_drain();
}

static void gateway_ble_reset_link(uint32_t expected_generation,
                                   uint32_t expected_deadline_ms,
                                   bool timeout_reset,
                                   const char *reason)
{
    struct bt_conn *conn = NULL;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t observed_deadline_ms;
    uint8_t observed_failure_count;
    bool observed_in_flight;
    int ret;
    k_spinlock_key_t key = k_spin_lock(&gateway_ble_tx_lock);

    if (expected_generation != gateway_ble_tx_generation ||
        (timeout_reset &&
         (!gateway_ble_tx_in_flight ||
          gateway_ble_tx_deadline_ms != expected_deadline_ms ||
          !uptime_deadline_reached(now_ms, expected_deadline_ms))) ||
        (!timeout_reset &&
         (gateway_ble_tx_in_flight ||
          gateway_ble_tx_deadline_ms != 0u ||
          gateway_ble_notify_failure_count <
              GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD))) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }
    if (gateway_ble_conn != NULL) {
        conn = bt_conn_ref(gateway_ble_conn);
    }
    observed_in_flight = gateway_ble_tx_in_flight;
    observed_deadline_ms = gateway_ble_tx_deadline_ms;
    observed_failure_count = gateway_ble_notify_failure_count;
    gateway_ble_tx_reset_locked();
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_stream_cancel_active();

    if (conn == NULL) {
        gateway_ble_schedule_recovery(reason);
        return;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf(
        "DBG_GATEWAY_BLE event=local-disconnect cause=%s timeout=%u in_flight=%u deadline=%u failures=%u uptime=%u\n",
        reason == NULL ? "unknown" : reason,
        timeout_reset ? 1u : 0u,
        observed_in_flight ? 1u : 0u,
        observed_deadline_ms,
        observed_failure_count,
        now_ms);
#endif
    ret = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    bt_conn_unref(conn);
    if (ret < 0 && ret != -ENOTCONN) {
        LOG_ERR("gateway BLE stalled-link reset failed: reason=%s ret=%d",
                reason == NULL ? "unknown" : reason,
                ret);
        gateway_ble_schedule_failed("stalled-link-reset", ret);
    }
}

static void gateway_ble_reset_stalled_link(uint32_t expected_generation,
                                           uint32_t expected_deadline_ms,
                                           const char *reason)
{
    gateway_ble_reset_link(expected_generation,
                           expected_deadline_ms,
                           true,
                           reason);
}

static void gateway_ble_reset_failed_link(uint32_t expected_generation,
                                          const char *reason)
{
    gateway_ble_reset_link(expected_generation, 0u, false, reason);
}

static void gateway_ble_stream_work_handler(struct k_work *work)
{
    struct bt_gatt_notify_params params = {0};
    const struct bt_gatt_attr *attr;
    struct bt_conn *conn;
    enum gateway_ble_tx_source source;
    uint32_t generation;
    uint16_t chunk_cap;
    int ret;
    k_spinlock_key_t key;

    ARG_UNUSED(work);

    key = k_spin_lock(&gateway_ble_tx_lock);
    if (gateway_ble_tx_in_flight) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t deadline_ms = gateway_ble_tx_deadline_ms;
        uint32_t blocked_generation = gateway_ble_tx_generation;
        struct proto_packet blocked_packet;
        uint16_t queue_depth;
        int packet_ret;

        if (uptime_deadline_reached(now_ms, deadline_ms)) {
            k_spin_unlock(&gateway_ble_tx_lock, key);
            gateway_ble_reset_stalled_link(
                blocked_generation, deadline_ms, "notify-timeout");
            return;
        }
        k_spinlock_key_t stream_key = k_spin_lock(&gateway_ble_stream_lock);

        queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &blocked_packet);
        k_spin_unlock(&gateway_ble_stream_lock, stream_key);
        k_spin_unlock(&gateway_ble_tx_lock, key);
        gateway_ble_schedule_in_flight_deadline(deadline_ms - now_ms);
        if (packet_ret == 0 && queue_depth > 0u) {
            const struct app_stack_workload_diag_pressure pressure = {
                .queue_depth = queue_depth,
                .custody_depth = queue_depth,
                .credit_available = 0u,
                .retry_depth = gateway_ble_notify_failure_count,
                .drain_depth = queue_depth,
            };

            app_stack_workload_diag_ble_admit_with_pressure(
                &blocked_packet, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE,
                &pressure);
            app_stack_workload_diag_ble_sample_with_pressure(&blocked_packet,
                                                             &pressure);
        }
        return;
    }
    if (gateway_ble_conn == NULL || gateway_ble_uwb_quiet_active() ||
        !gateway_ble_tx_select_frame_locked()) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }

    source = gateway_ble_tx_source;
    if ((source == GATEWAY_BLE_TX_DIRECT || source == GATEWAY_BLE_TX_STREAM) &&
        !gateway_ble_packet_notify_enabled) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }
    conn = bt_conn_ref(gateway_ble_conn);
    chunk_cap = MIN(gateway_ble_notify_chunk_len(conn),
                    (uint16_t)sizeof(gateway_ble_tx_chunk));
    if (chunk_cap == 0u) {
        bt_conn_unref(conn);
        k_spin_unlock(&gateway_ble_tx_lock, key);
        gateway_ble_schedule_stream_retry(0u);
        return;
    }
    gateway_ble_tx_chunk_len = MIN(gateway_ble_tx_frame_len - gateway_ble_tx_frame_offset,
                                   (size_t)chunk_cap);
    if (source == GATEWAY_BLE_TX_STREAM) {
        const uint8_t *record = NULL;
        size_t record_len = 0u;
        k_spinlock_key_t stream_key = k_spin_lock(&gateway_ble_stream_lock);
        int stream_ret = gateway_ble_stream_peek(&gateway_ble_stream_state,
                                                 &record,
                                                 &record_len);

        if (stream_ret == 0 && record_len == gateway_ble_tx_frame_len &&
            gateway_ble_tx_frame_offset <= record_len &&
            record_len - gateway_ble_tx_frame_offset >= gateway_ble_tx_chunk_len) {
            memcpy(gateway_ble_tx_chunk,
                   &record[gateway_ble_tx_frame_offset],
                   gateway_ble_tx_chunk_len);
        } else {
            stream_ret = -EIO;
        }
        k_spin_unlock(&gateway_ble_stream_lock, stream_key);
        if (stream_ret < 0) {
            gateway_ble_tx_source = GATEWAY_BLE_TX_NONE;
            gateway_ble_tx_frame_len = 0u;
            gateway_ble_tx_frame_offset = 0u;
            bt_conn_unref(conn);
            k_spin_unlock(&gateway_ble_tx_lock, key);
            gateway_ble_stream_cancel_active();
            gateway_ble_schedule_stream_drain();
            return;
        }
    } else {
        memcpy(gateway_ble_tx_chunk,
               &gateway_ble_tx_frame[gateway_ble_tx_frame_offset],
               gateway_ble_tx_chunk_len);
    }
    gateway_ble_tx_in_flight = true;
    gateway_ble_tx_deadline_ms =
        k_uptime_get_32() + GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS;
    generation = gateway_ble_tx_generation;
    attr = &gateway_ble_svc.attrs[GATEWAY_BLE_PACKET_TX_ATTR_INDEX];
    k_spin_unlock(&gateway_ble_tx_lock, key);

    params.attr = attr;
    params.data = gateway_ble_tx_chunk;
    params.len = (uint16_t)gateway_ble_tx_chunk_len;
    params.func = gateway_ble_tx_complete;
    params.user_data = (void *)(uintptr_t)generation;
    gateway_ble_schedule_in_flight_deadline(
        GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS);
    ret = bt_gatt_notify_cb(conn, &params);
    if (ret < 0) {
        struct proto_packet blocked_packet;
        bool attempt_current = false;
        uint16_t queue_depth;
        uint8_t failure_count;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_tx_lock);
        if (gateway_ble_tx_in_flight && generation == gateway_ble_tx_generation) {
            gateway_ble_tx_in_flight = false;
            gateway_ble_tx_deadline_ms = 0u;
            gateway_ble_tx_chunk_len = 0u;
            if (gateway_ble_notify_failure_count < UINT8_MAX) {
                gateway_ble_notify_failure_count++;
            }
            attempt_current = true;
        }
        failure_count = gateway_ble_notify_failure_count;
        k_spin_unlock(&gateway_ble_tx_lock, key);
        if (!attempt_current) {
            /*
             * Disconnect/CCC reset invalidated this asynchronous submission
             * while the Bluetooth host call was returning. Do not charge the
             * failure to a later connection generation or schedule stale
             * retry work against its state.
             */
            bt_conn_unref(conn);
            return;
        }
        key = k_spin_lock(&gateway_ble_stream_lock);
        queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &blocked_packet);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (packet_ret == 0) {
            const struct app_stack_workload_diag_pressure pressure = {
                .queue_depth = queue_depth,
                .custody_depth = queue_depth,
                .credit_available = 0u,
                .retry_depth = failure_count,
                .drain_depth = queue_depth,
            };

            app_stack_workload_diag_ble_admit_with_pressure(
                &blocked_packet, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE,
                &pressure);
            app_stack_workload_diag_ble_sample_with_pressure(&blocked_packet,
                                                             &pressure);
        }
        if (failure_count != 0u &&
            (failure_count & (failure_count - 1u)) == 0u) {
            LOG_WRN("gateway BLE notification deferred: ret=%d failures=%u retry_ms=%u",
                    ret,
                    failure_count,
                    gateway_ble_stream_retry_delay_ms(failure_count));
        }
        if (failure_count >=
            GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD) {
            gateway_ble_reset_failed_link(
                generation, "notify-submit-failures");
        } else {
            gateway_ble_schedule_stream_retry(failure_count);
        }
    } else if (source == GATEWAY_BLE_TX_STREAM) {
        struct proto_packet inflight_packet;
        uint16_t queue_depth;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_stream_lock);
        queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &inflight_packet);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (packet_ret == 0) {
            const struct app_stack_workload_diag_pressure pressure = {
                .queue_depth = queue_depth,
                .custody_depth = queue_depth,
                .credit_available = 0u,
                .retry_depth = gateway_ble_notify_failure_count,
                .drain_depth = queue_depth,
            };

            /* A successful async submit consumes the controller credit until
             * gateway_ble_tx_complete() returns it. */
            app_stack_workload_diag_ble_admit_with_pressure(
                &inflight_packet, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE,
                &pressure);
            app_stack_workload_diag_ble_sample_with_pressure(&inflight_packet,
                                                             &pressure);
        }
    }
    bt_conn_unref(conn);
}

static void gateway_ble_connected(struct bt_conn *conn, uint8_t err)
{
    k_spinlock_key_t key;
    int param_ret;

    if (!gateway_ble_transport_enabled()) {
        return;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE event=connected err=%u uptime=%u\n",
                        err,
                        k_uptime_get_32());
#endif
    if (err != 0u) {
        LOG_WRN("gateway BLE connection failed: err=0x%02x", err);
        gateway_ble_schedule_recovery("connection-failed");
        return;
    }
    key = k_spin_lock(&gateway_ble_tx_lock);
    if (gateway_ble_conn != NULL) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }
    k_spin_unlock(&gateway_ble_tx_lock, key);

    (void)gateway_ble_stop_advertising("connected");
    key = k_spin_lock(&gateway_ble_tx_lock);
    gateway_ble_tx_reset_locked();
    gateway_ble_conn = bt_conn_ref(conn);
    gateway_ble_packet_notify_enabled = false;
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_stream_cancel_active();
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    gateway_ble_recovery_round = 0u;
    (void)k_work_cancel_delayable(&gateway_ble_recovery_work);
    /* As a peripheral Zephyr retains this request until the standards-defined
     * update delay expires.  It replaces the generic 420 ms preferred
     * supervision timeout before that value can be installed. */
    param_ret = bt_conn_le_param_update(conn, &gateway_ble_conn_params);
    if (param_ret < 0) {
        LOG_WRN("gateway BLE connection-parameter request failed: ret=%d",
                param_ret);
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf(
        "DBG_GATEWAY_BLE event=param-request ret=%d interval=%u-%u latency=%u timeout_ms=%u uptime=%u\n",
        param_ret,
        GATEWAY_BLE_CONN_INTERVAL_MIN,
        GATEWAY_BLE_CONN_INTERVAL_MAX,
        GATEWAY_BLE_CONN_LATENCY,
        GATEWAY_BLE_CONN_SUPERVISION_TIMEOUT * 10u,
        k_uptime_get_32());
#endif
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link connected");
}

static void gateway_ble_le_param_updated(struct bt_conn *conn,
                                         uint16_t interval,
                                         uint16_t latency,
                                         uint16_t timeout)
{
    ARG_UNUSED(conn);
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf(
        "DBG_GATEWAY_BLE event=param-updated interval=%u latency=%u timeout_ms=%u uptime=%u\n",
        interval,
        latency,
        timeout * 10u,
        k_uptime_get_32());
#else
    ARG_UNUSED(interval);
    ARG_UNUSED(latency);
    ARG_UNUSED(timeout);
#endif
}

static void gateway_ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct bt_conn *disconnected_conn;
    k_spinlock_key_t key;

    if (!gateway_ble_transport_enabled()) {
        return;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE event=disconnected reason=%u uptime=%u\n",
                        reason,
                        k_uptime_get_32());
#endif
    key = k_spin_lock(&gateway_ble_tx_lock);
    if (gateway_ble_conn != conn) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }

    disconnected_conn = gateway_ble_conn;
    gateway_ble_conn = NULL;
    gateway_ble_packet_notify_enabled = false;
    gateway_ble_tx_reset_locked();
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_stream_cancel_active();
    {
        uint16_t queue_depth = (uint16_t)gateway_ble_stream_depth(
            &gateway_ble_stream_state);
        const struct app_stack_workload_diag_pressure pressure = {
            .queue_depth = queue_depth,
            .custody_depth = gateway_ble_tx_in_flight ? 1u : 0u,
            .credit_available = 0u,
            .retry_depth = gateway_ble_notify_failure_count,
            .drain_depth = queue_depth,
        };

        app_stack_workload_diag_ble_release_all_with_pressure(
            APP_STACK_DIAG_TERMINAL_DISCONNECT, &pressure);
    }
    (void)k_work_cancel_delayable(&gateway_ble_stream_work);
    bt_conn_unref(disconnected_conn);
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link disconnected: reason=0x%02x", reason);
    gateway_ble_schedule_recovery("disconnected");
}

BT_CONN_CB_DEFINE(gateway_ble_conn_callbacks) = {
    .connected = gateway_ble_connected,
    .disconnected = gateway_ble_disconnected,
    .le_param_updated = gateway_ble_le_param_updated,
};

static int gateway_ble_start_advertising(void)
{
    int ret;

    ret = bt_le_adv_start(BT_LE_ADV_CONN,
                          gateway_ble_ad,
                          ARRAY_SIZE(gateway_ble_ad),
                          gateway_ble_sd,
                          ARRAY_SIZE(gateway_ble_sd));
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE advertising start requested: ret=%d name=%s",
            ret,
            GATEWAY_BLE_DEVICE_NAME);
    if (ret == -EALREADY) {
        gateway_ble_advertising_active = true;
        return 0;
    }
    if (ret == 0) {
        gateway_ble_advertising_active = true;
    }
    return ret;
}

static void gateway_ble_log_identities(void)
{
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t addr_count = ARRAY_SIZE(addrs);

    bt_id_get(addrs, &addr_count);
    for (size_t i = 0u; i < addr_count; i++) {
        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(&addrs[i], addr, sizeof(addr));
        GATEWAY_BLE_VERBOSE_LOG("gateway BLE identity: index=%u addr=%s",
                (unsigned int)i,
                addr);
    }
}

static void gateway_ble_recovery_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (!gateway_ble_transport_enabled() || gateway_ble_conn != NULL) {
        return;
    }
    if (!gateway_ble_stack_ready) {
        ret = bt_enable(NULL);
        GATEWAY_BLE_VERBOSE_LOG("gateway BLE recovery bt_enable completed: ret=%d", ret);
        if (ret != 0 && ret != -EALREADY) {
            gateway_ble_schedule_recovery("bt-enable");
            return;
        }
        gateway_ble_stack_ready = true;
        gateway_ble_log_identities();
    }
    if (gateway_ble_uwb_quiet_active()) {
        return;
    }

    ret = gateway_ble_start_advertising();
    if (ret < 0) {
        gateway_ble_schedule_recovery("advertising");
        return;
    }
    gateway_ble_recovery_round = 0u;
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link advertising as %s", GATEWAY_BLE_DEVICE_NAME);
}

static int gateway_ble_stop_advertising(const char *reason)
{
    int ret;

    if (!gateway_ble_advertising_active) {
        return 0;
    }

    ret = bt_le_adv_stop();
    if (ret != 0 && ret != -EALREADY) {
        LOG_WRN("gateway BLE advertising stop failed: reason=%s ret=%d",
                reason == NULL ? "unknown" : reason,
                ret);
        return ret;
    }

    gateway_ble_advertising_active = false;
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE advertising stopped: reason=%s primary_channels=37-39",
            reason == NULL ? "unknown" : reason);
    return 0;
}

int gateway_ble_init(void)
{
    int ret;

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE stage=entry enabled=%u uptime=%u\n",
                        gateway_ble_transport_enabled() ? 1u : 0u,
                        k_uptime_get_32());
#endif
    if (!gateway_ble_transport_enabled()) {
        return 0;
    }

    k_work_init(&gateway_ble_rx_work, gateway_ble_rx_work_handler);
    k_work_init_delayable(&gateway_ble_stream_work,
                          gateway_ble_stream_work_handler);
    k_work_init_delayable(&gateway_ble_host_receipt_timeout_work,
                          gateway_ble_host_receipt_timeout_work_handler);
    k_work_init_delayable(&gateway_ble_recovery_work,
                          gateway_ble_recovery_work_handler);
    gateway_ble_stream_init(&gateway_ble_stream_state);
    gateway_ble_direct_queue_init(&gateway_ble_direct_tx_state);
    {
        k_spinlock_key_t key =
            k_spin_lock(&gateway_command_observability_lock);

        gateway_command_observability_init(
            &gateway_command_observability_state);
        k_spin_unlock(&gateway_command_observability_lock, key);
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    memset(&gateway_command_event_last_host_receipt,
           0,
           sizeof(gateway_command_event_last_host_receipt));
    memset(gateway_command_event_last_host_receipt_digest,
           0,
           sizeof(gateway_command_event_last_host_receipt_digest));
    gateway_command_event_last_host_receipt_valid = false;
    gateway_ble_stream_initialized = true;
    (void)gateway_flush_host_command_results();
    if (gateway_assignment_publication_pending()) {
        /*
         * Publication restore reconstructs a full 50-anchor batch. Keep that
         * NVS/validation chain off the boot/main stack and resume it through
         * the already initialized route-owned persistence worker.
         */
        gateway_schedule_persistence_retry(
            "assignment-publication-init");
    }
#endif
    gateway_ble_tx_reset_locked();
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    gateway_ble_stack_ready = false;
    gateway_ble_recovery_round = 0u;

    ret = bt_enable(NULL);
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE stage=bt_enable ret=%d uptime=%u\n",
                        ret,
                        k_uptime_get_32());
#endif
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE bt_enable completed: ret=%d", ret);
    if (ret != 0 && ret != -EALREADY) {
        LOG_ERR("gateway BLE init failed; recovery remains active: %d", ret);
        gateway_ble_schedule_recovery("initial-bt-enable");
        return ret;
    }
    gateway_ble_stack_ready = true;
    gateway_ble_log_identities();

    ret = gateway_ble_start_advertising();
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE stage=adv_start ret=%d active=%u quiet=%u uptime=%u\n",
                        ret,
                        gateway_ble_advertising_active ? 1u : 0u,
                        gateway_ble_uwb_quiet_active() ? 1u : 0u,
                        k_uptime_get_32());
#endif
    if (ret < 0) {
        LOG_ERR("gateway BLE advertising failed; recovery remains active: %d", ret);
        gateway_ble_schedule_recovery("initial-advertising");
        return ret;
    }

    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link advertising as %s", GATEWAY_BLE_DEVICE_NAME);
    return 0;
}

static int gateway_ble_queue_frame(void)
{
    struct gateway_ble_frame_pending pending = {0};
    int ret;

    if (gateway_ble_rx_len <= 1u) {
        gateway_ble_rx_len = 0u;
        return 0;
    }

    pending.len = (uint16_t)gateway_ble_rx_len;
    memcpy(pending.frame, gateway_ble_rx_frame, gateway_ble_rx_len);
    ret = k_msgq_put(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("gateway BLE RX frame queue full: len=%u", pending.len);
    } else {
        gateway_ble_resume_rx();
    }
    gateway_ble_rx_len = 0u;
    return ret < 0 ? -ENOSPC : 0;
}

static int gateway_ble_rx_bytes(const uint8_t *data, size_t len)
{
    int first_error = 0;

    if (data == NULL && len != 0u) {
        return -EINVAL;
    }

    for (size_t i = 0u; i < len; i++) {
        uint8_t byte = data[i];

        if (gateway_ble_rx_overflow) {
            if (byte == SERIAL_FRAME_DELIMITER) {
                gateway_ble_rx_overflow = false;
                gateway_ble_rx_len = 0u;
                LOG_WRN("gateway BLE RX frame dropped after overflow");
                if (first_error == 0) {
                    first_error = -EMSGSIZE;
                }
            }
            continue;
        }

        if (gateway_ble_rx_len >= sizeof(gateway_ble_rx_frame)) {
            gateway_ble_rx_overflow = true;
            gateway_ble_rx_len = 0u;
            continue;
        }

        gateway_ble_rx_frame[gateway_ble_rx_len] = byte;
        gateway_ble_rx_len++;
        if (byte == SERIAL_FRAME_DELIMITER) {
            int ret = gateway_ble_queue_frame();

            if (ret < 0 && first_error == 0) {
                first_error = ret;
            }
        }
    }
    return first_error;
}

#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_ble_pending_is_host_receipt(
    const struct gateway_ble_frame_pending *pending)
{
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    if (pending == NULL ||
        serial_frame_decode_packet(pending->frame,
                                    pending->len,
                                    &packet,
                                    payload,
                                    sizeof(payload),
                                    &payload_len) != PROTO_OK) {
        return false;
    }
    return packet.msg_type == MSG_GATEWAY_HOST_RECEIPT;
}
#endif

static void gateway_ble_rx_work_handler(struct k_work *work)
{
    struct gateway_ble_frame_pending pending;

    ARG_UNUSED(work);

    if (!gateway_ble_transport_enabled()) {
        return;
    }

    for (;;) {
        uint32_t result_reservation_token = 0u;
        int ret;

#if DEVICE_ROLE == ROLE_GATEWAY
        /* Host receipts must bypass result-credit admission, or a full
         * command-result pool can strand the exact item they release. Peek
         * first, then dequeue/consume receipts with token zero. */
        ret = k_msgq_peek(&gateway_ble_rx_msgq, &pending);
        if (ret < 0) {
            break;
        }
        if (gateway_ble_pending_is_host_receipt(&pending)) {
            ret = k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
            if (ret < 0) {
                break;
            }
            (void)gateway_handle_ble_frame(pending.frame, pending.len, 0u);
            continue;
        }
#endif

#if DEVICE_ROLE == ROLE_GATEWAY
        ret = gateway_command_result_reserve_ingress(
            &result_reservation_token);
        if (ret < 0) {
            if (ret != -ENOSPC) {
                LOG_ERR("gateway command result admission failed: %d", ret);
                gateway_ble_schedule_failed("rx-result-admission", ret);
            }
            break;
        }
#endif
        ret = k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
        if (ret < 0) {
            if (result_reservation_token != 0u) {
                gateway_command_result_release_ingress(
                    result_reservation_token);
            }
            break;
        }
        if (!gateway_handle_ble_frame(pending.frame,
                                      pending.len,
                                      result_reservation_token) &&
            result_reservation_token != 0u) {
            gateway_command_result_release_ingress(
                result_reservation_token);
        }
    }
}

void gateway_ble_get_status(struct gateway_ble_status *status)
{
    k_spinlock_key_t key;

    if (status == NULL) {
        return;
    }

    key = k_spin_lock(&gateway_ble_tx_lock);
    status->connected = gateway_ble_conn != NULL;
    status->packet_notify_enabled = gateway_ble_packet_notify_enabled;
    k_spin_unlock(&gateway_ble_tx_lock, key);
}
#else
static bool gateway_ble_stream_ready(void)
{
    return false;
}

static void gateway_ble_schedule_stream_drain(void)
{
}

static void gateway_ble_resume_rx(void)
{
}

int gateway_ble_init(void)
{
    return gateway_ble_transport_enabled() ? -ENOTSUP : 0;
}

int gateway_ble_send_packet_frame(const uint8_t *frame, size_t frame_len)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);

    return -ENOTSUP;
}

bool gateway_ble_uwb_quiet_active(void)
{
    return false;
}

void gateway_ble_enter_uwb_quiet(const char *reason)
{
    ARG_UNUSED(reason);
}

void gateway_ble_exit_uwb_quiet(const char *reason)
{
    ARG_UNUSED(reason);
}

void gateway_ble_get_status(struct gateway_ble_status *status)
{
    if (status == NULL) {
        return;
    }

    status->connected = false;
    status->packet_notify_enabled = false;
}
#endif

int gateway_observe_command_event_if_available(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx)
{
#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
    int ret;

    ARG_UNUSED(ctx);
    if (DEVICE_ROLE != ROLE_GATEWAY || event == NULL) {
        return -EINVAL;
    }

    ret = gateway_observability_prepare_state(event, terminal);
    if (ret < 0) {
        return ret;
    }
    /* RAM custody is the acceptance boundary.  BLE disconnection or queue
     * pressure leaves the exact terminal or latest per-kind snapshot pending
     * for reconnect replay and must never stall or abort accepted RF work. */
    (void)gateway_observability_enqueue_prepared(event);
    return 0;
#else
    ARG_UNUSED(event);
    ARG_UNUSED(terminal);
    ARG_UNUSED(ctx);
    return -EAGAIN;
#endif
}

int gateway_publish_assignment_event_if_available(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx)
{
#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
    struct proto_packet packet = {0};
    uint8_t payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t payload_len = 0u;
    int ret;

    ARG_UNUSED(ctx);
    ARG_UNUSED(terminal);
    if (DEVICE_ROLE != ROLE_GATEWAY || event == NULL ||
        !app_gateway_assignment_publisher_event_is_reliable(event)) {
        return -EINVAL;
    }
    if (!gateway_ble_stream_ready()) {
        return -EAGAIN;
    }
    ret = gateway_command_event_encode(event, payload, sizeof(payload),
                                       &payload_len);
    if (ret < 0) {
        return ret;
    }
    packet.msg_type = MSG_GATEWAY_COMMAND_EVENT;
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet.src_id = DEVICE_ID;
    packet.dst_id = DEVICE_ID;
    packet.session_id = event->event_seq;
    packet.seq = (uint16_t)event->event_seq;
    packet.payload_len = (uint16_t)payload_len;
    return gateway_ble_stream_packet(&packet, payload, payload_len,
                                     k_uptime_get_32());
#else
    ARG_UNUSED(event);
    ARG_UNUSED(terminal);
    ARG_UNUSED(ctx);
    return -EAGAIN;
#endif
}

int gateway_observe_command_acceptance_if_available(
    struct gateway_command_event *queued)
{
#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || queued == NULL) {
        return -EINVAL;
    }
    /* QUEUED is the coalesced externally visible proof of ACCEPTED custody. */
    ret = gateway_observe_command_event_if_available(queued, false, NULL);
    if (ret == 0) {
        return 0;
    }
    if (ret != -EAGAIN) {
        return ret;
    }
    ret = gateway_observability_prepare_state(queued, false);
    if (ret < 0) {
        return ret;
    }
    gateway_observability_note_enqueue_state(
        queued->event_seq, -EAGAIN);
    return 0;
#else
    ARG_UNUSED(queued);
    return -EAGAIN;
#endif
}



#undef GATEWAY_BLE_VERBOSE_LOG
