#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"

#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"

#include "app_anchor.h"
#include "app_board.h"
#include "app_config.h"
#include "app_gateway_collection_eack.h"
#include "app_gateway_collection_receipts.h"
#include "app_gateway_terminal_receipts.h"
#include "app_gateway_eack_retry.h"
#include "app_gateway_eack_policy.h"
#include "app_gateway_command_observability.h"
#include "app_gateway_command_result.h"
#include "app_gateway_ble_stream.h"
#include "app_stack_workload_diag.h"
#include "app_high_debug.h"
#include "app_watchdog.h"
#include "app_mesh_report.h"
#include "app_mesh_persistence.h"
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

#if defined(CONFIG_IMEC_HIGH_DEBUG)
#define GATEWAY_BLE_VERBOSE_LOG(...) LOG_INF(__VA_ARGS__)
#else
#define GATEWAY_BLE_VERBOSE_LOG(...) do { } while (0)
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
#define GATEWAY_BROADCAST_COMMAND_SEQUENCE_BLOCK_SIZE 256u
static uint32_t gateway_broadcast_command_sequence_next;
static uint32_t gateway_broadcast_command_sequence_remaining;
K_MUTEX_DEFINE(gateway_broadcast_command_sequence_mutex);
static struct gateway_ble_stream_state gateway_ble_stream_state;
static struct gateway_command_observability_state gateway_command_observability_state;
static struct k_spinlock gateway_ble_stream_lock;
static struct k_spinlock gateway_command_observability_lock;

BUILD_ASSERT(GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH ==
             APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH,
             "every accepted host result credit needs terminal event custody");

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

    if (event == NULL) {
        return -EINVAL;
    }
    if (event->event_seq != 0u) {
        return 0;
    }

    /*
     * This may reserve the next durable NVS block, so every caller must run
     * it before taking BLE, stream, or observability spinlocks.
     */
    event_seq = gateway_next_broadcast_command_seq();
    if (event_seq == 0u) {
        return -EIO;
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

static uint32_t gateway_command_sequence_increment(uint32_t sequence)
{
    return sequence == UINT32_MAX ? 1u : sequence + 1u;
}

static int gateway_broadcast_command_sequence_reserve_locked(void)
{
    uint32_t first_sequence = 0u;
    int ret;

    if (gateway_broadcast_command_sequence_remaining != 0u) {
        return 0;
    }
    ret = app_mesh_persistence_reserve_gateway_command_sequences(
        GATEWAY_BROADCAST_COMMAND_SEQUENCE_BLOCK_SIZE,
        &first_sequence);
    if (ret < 0) {
        return ret;
    }
    gateway_broadcast_command_sequence_next = first_sequence;
    gateway_broadcast_command_sequence_remaining =
        GATEWAY_BROADCAST_COMMAND_SEQUENCE_BLOCK_SIZE;
    return 0;
}

int gateway_broadcast_command_sequence_init(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    k_mutex_lock(&gateway_broadcast_command_sequence_mutex, K_FOREVER);
    ret = gateway_broadcast_command_sequence_reserve_locked();
    k_mutex_unlock(&gateway_broadcast_command_sequence_mutex);
    return ret;
}

uint32_t gateway_next_broadcast_command_seq(void)
{
    uint32_t sequence = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0u;
    }
    k_mutex_lock(&gateway_broadcast_command_sequence_mutex, K_FOREVER);
    ret = gateway_broadcast_command_sequence_reserve_locked();
    if (ret == 0) {
        sequence = gateway_broadcast_command_sequence_next;
        gateway_broadcast_command_sequence_next =
            gateway_command_sequence_increment(sequence);
        gateway_broadcast_command_sequence_remaining--;
    }
    k_mutex_unlock(&gateway_broadcast_command_sequence_mutex);
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
    packet.src_id = DEVICE_ID;
    packet.dst_id = DEVICE_ID;
    packet.session_id = event->event_seq;
    packet.seq = (uint16_t)event->event_seq;
    packet.payload_len = (uint16_t)payload_len;
    ret = gateway_ble_stream_packet(&packet,
                                    payload,
                                    payload_len,
                                    k_uptime_get_32());
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

#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST) && defined(CONFIG_BT)
#define BLE_RANGE_ADV_INTERVAL_MIN_UNITS 0x00a0u
#define BLE_RANGE_ADV_INTERVAL_MAX_UNITS 0x00a0u
#define BLE_RANGE_SCAN_INTERVAL_UNITS 0x0060u
#define BLE_RANGE_SCAN_WINDOW_UNITS 0x0060u
#define BLE_RANGE_LED_HOLD_MS 1000u
#define BLE_RANGE_LED_POLL_MS 50u

static const uint8_t gateway_ble_range_marker[] = {
    0xff, 0xff, 'I', 'M', 'E', 'C', 'R', 'N', 'G', 0x01
};
static const char gateway_ble_range_name[] = "IMEC BLE Range";

static struct k_work_delayable gateway_ble_range_led_work;
static uint32_t gateway_ble_range_last_seen_ms;

static int gateway_ble_range_enable(void)
{
    int ret = bt_enable(NULL);

    GATEWAY_BLE_VERBOSE_LOG("BLE range bt_enable completed: ret=%d", ret);
    if (ret != 0 && ret != -EALREADY) {
        LOG_ERR("BLE range init failed: %d", ret);
        return ret;
    }
    return 0;
}

static int gateway_ble_range_start_advertiser(void)
{
    const struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0u,
        .secondary_max_skip = 0u,
        .options = BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = BLE_RANGE_ADV_INTERVAL_MIN_UNITS,
        .interval_max = BLE_RANGE_ADV_INTERVAL_MAX_UNITS,
        .peer = NULL,
    };
    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                gateway_ble_range_marker,
                sizeof(gateway_ble_range_marker)),
        BT_DATA(BT_DATA_NAME_COMPLETE,
                gateway_ble_range_name,
                sizeof(gateway_ble_range_name) - 1u),
    };
    int ret;

    ret = gateway_ble_range_enable();
    if (ret < 0) {
        return ret;
    }

    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
    GATEWAY_BLE_VERBOSE_LOG("BLE range advertiser start: ret=%d name=%s interval_units=%u",
            ret,
            gateway_ble_range_name,
            BLE_RANGE_ADV_INTERVAL_MIN_UNITS);
    return ret;
}

static bool gateway_ble_range_parse_ad(struct bt_data *data, void *user_data)
{
    bool *matched = user_data;

    if (data->type == BT_DATA_MANUFACTURER_DATA &&
        data->data_len == sizeof(gateway_ble_range_marker) &&
        memcmp(data->data,
               gateway_ble_range_marker,
               sizeof(gateway_ble_range_marker)) == 0) {
        *matched = true;
        return false;
    }
    return true;
}

static void gateway_ble_range_led_work_handler(struct k_work *work)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t last_seen_ms = gateway_ble_range_last_seen_ms;
    uint32_t age_ms = now_ms - last_seen_ms;
    bool seen_recently = last_seen_ms != 0u && age_ms < BLE_RANGE_LED_HOLD_MS;

    ARG_UNUSED(work);

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_led0_set(false, seen_recently, false);
    }
    (void)k_work_reschedule(&gateway_ble_range_led_work,
                            K_MSEC(BLE_RANGE_LED_POLL_MS));
}

static void gateway_ble_range_scan_cb(const bt_addr_le_t *addr,
                                      int8_t rssi,
                                      uint8_t adv_type,
                                      struct net_buf_simple *buf)
{
    bool matched = false;

    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    bt_data_parse(buf, gateway_ble_range_parse_ad, &matched);
    if (!matched) {
        return;
    }

    gateway_ble_range_last_seen_ms = k_uptime_get_32();
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_led0_set(false, true, false);
    }
    GATEWAY_BLE_VERBOSE_LOG("BLE range advertisement seen: rssi=%d", rssi);
}

static int gateway_ble_range_start_scanner(void)
{
    const struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BLE_RANGE_SCAN_INTERVAL_UNITS,
        .window = BLE_RANGE_SCAN_WINDOW_UNITS,
        .timeout = 0u,
        .interval_coded = 0u,
        .window_coded = 0u,
    };
    int ret;

    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("BLE range scanner LED setup incomplete: %d", ret);
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_led0_set(false, false, false);
    }
    k_work_init_delayable(&gateway_ble_range_led_work,
                          gateway_ble_range_led_work_handler);

    ret = gateway_ble_range_enable();
    if (ret < 0) {
        return ret;
    }

    ret = bt_le_scan_start(&scan_param, gateway_ble_range_scan_cb);
    GATEWAY_BLE_VERBOSE_LOG("BLE range scanner start: ret=%d interval_units=%u window_units=%u",
            ret,
            BLE_RANGE_SCAN_INTERVAL_UNITS,
            BLE_RANGE_SCAN_WINDOW_UNITS);
    if (ret == 0) {
        (void)k_work_reschedule(&gateway_ble_range_led_work,
                                K_MSEC(BLE_RANGE_LED_POLL_MS));
    }
    return ret;
}
#endif

#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
int gateway_emit_host_packet(const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);

    return -ENOTSUP;
}

int gateway_ble_stream_packet(const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t received_at_ms)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);

    return -ENOTSUP;
}

int gateway_ble_reserve_stream_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);

    return -ENOTSUP;
}

int gateway_ble_commit_stream_reservation(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);

    return -ENOTSUP;
}

int gateway_ble_commit_stream_reservation_projection(
    const struct proto_packet *packet,
    const uint8_t *raw_payload,
    size_t raw_payload_len,
    uint8_t accepted_record_mask)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(raw_payload);
    ARG_UNUSED(raw_payload_len);
    ARG_UNUSED(accepted_record_mask);

    return -ENOTSUP;
}

void gateway_ble_cancel_stream_reservation(void)
{
}

int gateway_preflight_result_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t first_received_at_ms,
    uint32_t result_validation_token,
    uint64_t previous_hop_id)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(first_received_at_ms);
    ARG_UNUSED(result_validation_token);
    ARG_UNUSED(previous_hop_id);

    return -ENOTSUP;
}

int gateway_result_bundle_host_projection_mask(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t *accepted_record_mask)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    if (accepted_record_mask != NULL) {
        *accepted_record_mask = 0u;
    }
    return -ENOTSUP;
}

int gateway_finalize_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    int semantic_acceptance)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    ARG_UNUSED(semantic_acceptance);

    return -ENOTSUP;
}

void gateway_ble_stream_get_status(struct gateway_ble_stream_diagnostics *diagnostics)
{
    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
    }
}

int gateway_encode_host_packet_frame(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *frame,
                                     size_t frame_cap,
                                     size_t *frame_len,
                                     struct proto_packet *frame_packet)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_cap);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(frame_packet);

    return -ENOTSUP;
}

void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(status);
    ARG_UNUSED(reason);
}

int gateway_command_result_reserve_ingress(uint32_t *token)
{
    ARG_UNUSED(token);
    return -ENOTSUP;
}

int gateway_command_result_bind_ingress(uint32_t token,
                                        const struct proto_packet *command,
                                        enum command_id command_id)
{
    ARG_UNUSED(token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return -ENOTSUP;
}

int gateway_command_result_rebind_command(
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(result_command);
    return -ENOTSUP;
}

void gateway_command_result_release_ingress(uint32_t token)
{
    ARG_UNUSED(token);
}

void gateway_command_result_release_command(
    const struct proto_packet *command,
    enum command_id command_id)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
}

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);

    return -ENOTSUP;
}

int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(timeout_ms);

    return -ENOTSUP;
}

int gateway_begin_command_result_wait_until(
    const struct proto_packet *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(absolute_deadline_ms);
    return -ENOTSUP;
}

int gateway_command_result_validation_reserve(
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t *token)
{
    ARG_UNUSED(result);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(token);
    return -ENOTSUP;
}

void gateway_command_result_validation_release_reserved(uint32_t token)
{
    ARG_UNUSED(token);
}

bool gateway_clear_pending_command_result(const struct proto_packet *command)
{
    ARG_UNUSED(command);
    return false;
}

int gateway_begin_command_collection(const struct gateway_command_options *options)
{
    ARG_UNUSED(options);

    return -ENOTSUP;
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    ARG_UNUSED(options);
}

int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             const uint8_t *slots,
                                             size_t node_count,
                                             uint32_t assignment_epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment,
                                             const struct gateway_membership_publication *publication)
{
    ARG_UNUSED(membership_epoch);
    ARG_UNUSED(node_ids);
    ARG_UNUSED(slots);
    ARG_UNUSED(node_count);
    ARG_UNUSED(assignment_epoch);
    ARG_UNUSED(table_seq);
    ARG_UNUSED(table_commitment);
    ARG_UNUSED(publication);

    return -ENOTSUP;
}

int gateway_get_registered_membership_roster_with_slots(
    uint64_t *node_ids,
    uint8_t *slots,
    size_t node_cap,
    size_t *node_count,
    uint16_t *membership_epoch)
{
    ARG_UNUSED(node_ids);
    ARG_UNUSED(slots);
    ARG_UNUSED(node_cap);
    if (node_count != NULL) {
        *node_count = 0u;
    }
    if (membership_epoch != NULL) {
        *membership_epoch = 0u;
    }
    return -ENOTSUP;
}

bool gateway_assignment_publication_pending(void)
{
    return false;
}

int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx)
{
    ARG_UNUSED(base_event);
    ARG_UNUSED(ctx);
    return -ENOTSUP;
}

void gateway_clear_registered_membership_roster(void)
{
}

int gateway_note_command_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t first_received_at_ms,
                                uint32_t result_validation_token,
                                uint64_t previous_hop_id,
                                uint8_t received_radio_channel,
                                const struct mesh_event_plan *current_channel9_plan)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(first_received_at_ms);
    ARG_UNUSED(result_validation_token);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    return -ENOTSUP;
}

int gateway_note_command_result_bundle(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint8_t received_radio_channel,
                                       const struct mesh_event_plan *current_channel9_plan)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    return -ENOTSUP;
}

void gateway_command_result_tracking_init(void)
{
}
#else
/* Result and collection custody stay in this translation unit. */
#include "app_gateway_result_runtime.inc"
#line 2855 __BASE_FILE__
#endif

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

BUILD_ASSERT(GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS > 0u &&
             GATEWAY_BLE_TX_IN_FLIGHT_TIMEOUT_MS < INT32_MAX,
             "BLE notification completion deadline must be wrap safe");
BUILD_ASSERT(GATEWAY_BLE_NOTIFY_FAILURE_RESET_THRESHOLD > 0u,
             "BLE synchronous notification failures need a reset bound");

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
            HIGH_DEBUG_COUNTER_INC(gateway_ble_rx_drops);
            LOG_ERR("gateway BLE command ingress failed closed: len=%u ret=%d flags=0x%02x",
                    len, ret, flags);
            /*
             * A write request receives the ATT error returned below.  A write
             * command has no response, so disconnect it explicitly to make the
             * failed admission observable and force a complete-frame retry.
             */
            if (write_command && conn != NULL) {
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
    k_spin_unlock(&gateway_ble_stream_lock, key);
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
        uint8_t completed_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
        bool journal_retirement_pending = false;
        bool journal_supported = false;
        int journal_identity_ret;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_stream_lock);
        journal_identity_ret =
            gateway_ble_stream_head_journal_identity(
                &gateway_ble_stream_state,
                &completed_packet,
                completed_payload_digest);
        packet_ret = journal_identity_ret == 0 ? 0 :
            gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                           &completed_packet);
        journal_supported =
            packet_ret == 0 &&
            app_mesh_persistence_gateway_host_journal_supports(
                &completed_packet);
        if (journal_identity_ret == 0 && journal_supported) {
            journal_retirement_pending =
                gateway_ble_stream_state.head_send_phase ==
                    GATEWAY_BLE_STREAM_HEAD_SENDING;
            if (journal_retirement_pending) {
                gateway_ble_stream_state.head_send_phase =
                    GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED;
            }
        }
        if (!journal_retirement_pending) {
            gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                         k_uptime_get_32());
        }
        uint16_t queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (packet_ret == 0) {
#if DEVICE_ROLE == ROLE_GATEWAY
            if (journal_supported) {
                gateway_ble_require_host_journal_restore(
                    "host-output-restore");
                if (!journal_retirement_pending) {
                    LOG_WRN("gateway journal identity missing: %d",
                            journal_identity_ret);
                }
            }
#endif
            if (completed_packet.msg_type == MSG_GATEWAY_COMMAND_EVENT) {
                gateway_observability_mark_sent_state(
                    completed_packet.session_id);
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
        if (packet_ret == 0 &&
            completed_packet.msg_type == MSG_GATEWAY_COMMAND_EVENT) {
            (void)app_gateway_assignment_publisher_note_sent(
                completed_packet.session_id);
        }
        if (app_gateway_assignment_publisher_work_pending()) {
            gateway_schedule_persistence_retry(
                "assignment-publication-progress");
        }
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
    gateway_ble_tx_reset_locked();
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_stream_cancel_active();

    if (conn == NULL) {
        gateway_ble_schedule_recovery(reason);
        return;
    }
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
    HIGH_DEBUG_COUNTER_INC(gateway_ble_connects);
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link connected");
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
    HIGH_DEBUG_COUNTER_INC(gateway_ble_disconnects);
    GATEWAY_BLE_VERBOSE_LOG("gateway BLE PC link disconnected: reason=0x%02x", reason);
    gateway_ble_schedule_recovery("disconnected");
}

BT_CONN_CB_DEFINE(gateway_ble_conn_callbacks) = {
    .connected = gateway_ble_connected,
    .disconnected = gateway_ble_disconnected,
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
    high_debug_log_event("BLE_GATEWAY_READY",
                         "device_name=%s packet_notify=0",
                         GATEWAY_BLE_DEVICE_NAME);
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
    high_debug_log_event("BLE_GATEWAY_ADV_STOP",
                         "reason=%s primary_channels=37-39",
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
#if DEVICE_ROLE == ROLE_GATEWAY && \
    !defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    gateway_ble_stream_initialized = true;
    (void)gateway_flush_host_command_results();
    atomic_clear(&gateway_host_journal_restored);
    atomic_set(&gateway_host_journal_restore_pending, 1);
    ret = gateway_restore_host_journal_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("host-output-restore-init");
    }
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
    high_debug_log_event("BLE_GATEWAY_READY",
                         "device_name=%s packet_notify=0",
                         GATEWAY_BLE_DEVICE_NAME);
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

#if DEVICE_ROLE == ROLE_GATEWAY && \
    !defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
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
    struct proto_packet packet = {0};
    uint8_t payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t payload_len = 0u;
    k_spinlock_key_t tx_key;
    k_spinlock_key_t stream_key;
    int ret;

    ARG_UNUSED(ctx);
    if (DEVICE_ROLE != ROLE_GATEWAY || event == NULL) {
        return -EINVAL;
    }

    /*
     * Avoid consuming an identity for ordinary disconnected/full-queue
     * backpressure. The state is rechecked after reservation because a link
     * callback may run while the durable allocator is outside all spinlocks.
     */
    tx_key = k_spin_lock(&gateway_ble_tx_lock);
    if (!gateway_ble_stream_ready()) {
        k_spin_unlock(&gateway_ble_tx_lock, tx_key);
        return -EAGAIN;
    }
    stream_key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH ||
        gateway_ble_stream_state.pool_used +
            GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
            GATEWAY_COMMAND_EVENT_WIRE_LEN >
            sizeof(gateway_ble_stream_state.record_pool)) {
        k_spin_unlock(&gateway_ble_stream_lock, stream_key);
        k_spin_unlock(&gateway_ble_tx_lock, tx_key);
        return -EAGAIN;
    }
    k_spin_unlock(&gateway_ble_stream_lock, stream_key);
    k_spin_unlock(&gateway_ble_tx_lock, tx_key);

    ret = gateway_observability_reserve_identity(event);
    if (ret < 0) {
        return ret;
    }

    tx_key = k_spin_lock(&gateway_ble_tx_lock);
    if (!gateway_ble_stream_ready()) {
        k_spin_unlock(&gateway_ble_tx_lock, tx_key);
        return -EAGAIN;
    }
    stream_key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH ||
        gateway_ble_stream_state.pool_used +
            GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
            GATEWAY_COMMAND_EVENT_WIRE_LEN >
            sizeof(gateway_ble_stream_state.record_pool)) {
        k_spin_unlock(&gateway_ble_stream_lock, stream_key);
        k_spin_unlock(&gateway_ble_tx_lock, tx_key);
        return -EAGAIN;
    }
    ret = gateway_observability_prepare_reserved_state(event, terminal);
    if (ret == 0) {
        ret = gateway_command_event_encode(event, payload, sizeof(payload),
                                           &payload_len);
    }
    if (ret == 0) {
        packet.msg_type = MSG_GATEWAY_COMMAND_EVENT;
        packet.src_id = DEVICE_ID;
        packet.dst_id = DEVICE_ID;
        packet.session_id = event->event_seq;
        packet.seq = (uint16_t)event->event_seq;
        packet.payload_len = (uint16_t)payload_len;
        ret = gateway_ble_stream_enqueue_packet(
            &gateway_ble_stream_state, &packet, payload, payload_len,
            k_uptime_get_32(), k_uptime_get_32(), true);
        if (ret > 0) {
            gateway_ble_stream_state.items[
                gateway_ble_stream_state.count - 1u].retain_until_sent = true;
            gateway_observability_note_enqueue_state(
                event->event_seq, ret);
            ret = 0;
        }
    }
    k_spin_unlock(&gateway_ble_stream_lock, stream_key);
    k_spin_unlock(&gateway_ble_tx_lock, tx_key);
    if (ret == 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret;
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

int app_gateway_ble_init(void)
{
    return gateway_ble_init();
}

#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
void gateway_ble_connectivity_test_run(void)
{
    int ret;

    printk("gateway BLE range test booting\n");
#if defined(CONFIG_BT)
    if (IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER)) {
        ret = gateway_ble_range_start_scanner();
    } else {
        ret = gateway_ble_range_start_advertiser();
    }
#else
    ret = -ENOTSUP;
#endif
    if (ret < 0) {
        printk("gateway BLE range test init failed: %d\n", ret);
        LOG_ERR("gateway BLE range test init failed: %d", ret);
        for (;;) {
            k_sleep(K_SECONDS(30));
        }
    }

    GATEWAY_BLE_VERBOSE_LOG("gateway BLE range test active: mode=%s no UWB, mesh, DWM3000, ADC, or buttons initialized",
            IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER) ? "scanner" : "advertiser");
    for (;;) {
        GATEWAY_BLE_VERBOSE_LOG("gateway BLE range test heartbeat: mode=%s",
                IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER) ? "scanner" : "advertiser");
        k_sleep(K_SECONDS(5));
    }
}
#endif

#undef GATEWAY_BLE_VERBOSE_LOG
