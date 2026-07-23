#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"

#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"

#include "app_anchor.h"
#include "app_board.h"
#include "app_config.h"
#include "app_gateway_collection_eack.h"
#include "app_gateway_eack_retry.h"
#include "app_gateway_eack_policy.h"
#include "app_gateway_command_observability.h"
#include "app_gateway_ble_stream.h"
#include "app_stack_workload_diag.h"
#include "app_high_debug.h"
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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_gateway_ble, LOG_LEVEL_DBG);

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
static struct gateway_ble_stream_state gateway_ble_stream_state;
static struct gateway_command_observability_state gateway_command_observability_state;
static struct k_spinlock gateway_ble_stream_lock;

static bool gateway_ble_stream_ready(void);
static void gateway_ble_schedule_stream_drain(void);
#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observability_flush(bool include_snapshots);
#endif

uint16_t gateway_next_command_seq(void)
{
    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    return gateway_command_seq;
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
    gateway_command_observability_note_enqueue(
        &gateway_command_observability_state, event->event_seq, ret);
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
    ret = gateway_command_observability_prepare(
        &gateway_command_observability_state, event, terminal);
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
    if (gateway_command_observability_pending_terminal(
            &gateway_command_observability_state, &event)) {
        (void)gateway_observability_enqueue_prepared(&event);
    }
    for (enum gateway_command_event_kind kind =
             GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
         kind <= GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH;
         kind++) {
        bool available = include_snapshots ?
            gateway_command_observability_reconnect_snapshot(
                &gateway_command_observability_state, kind, &event) :
            gateway_command_observability_pending_snapshot(
                &gateway_command_observability_state, kind, &event);

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

    LOG_INF("BLE range bt_enable completed: ret=%d", ret);
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
    LOG_INF("BLE range advertiser start: ret=%d name=%s interval_units=%u",
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
    LOG_INF("BLE range advertisement seen: rssi=%d", rssi);
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
    LOG_INF("BLE range scanner start: ret=%d interval_units=%u window_units=%u",
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

void gateway_ble_cancel_stream_reservation(void)
{
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

void gateway_clear_pending_command_result(const struct proto_packet *command)
{
    ARG_UNUSED(command);
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
                                             size_t node_count)
{
    ARG_UNUSED(membership_epoch);
    ARG_UNUSED(node_ids);
    ARG_UNUSED(node_count);

    return -ENOTSUP;
}

void gateway_clear_registered_membership_roster(void)
{
}

int gateway_note_command_result(const struct proto_packet *packet,
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
#if DEVICE_ROLE == ROLE_GATEWAY
static struct gateway_command_pending gateway_command_pending_state;
static struct gateway_collection_state gateway_collection_state;
static struct gateway_membership_roster gateway_membership_roster_state;
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_work_delayable gateway_collection_eack_work;
static struct app_gateway_eack_retry_state gateway_collection_eack_retry_state;
static bool gateway_collection_eack_round_dirty;
static struct k_work_delayable gateway_persistence_retry_work;
static bool gateway_collection_persistence_dirty;
static bool gateway_collection_clear_pending;
static bool gateway_collection_restore_pending;
static bool gateway_membership_persistence_dirty;
static bool gateway_membership_clear_pending;
static bool gateway_membership_restore_pending;
static bool gateway_click_journal_restore_pending;
static bool gateway_click_journal_clear_pending;
static bool gateway_click_journal_restored;
static bool gateway_ble_stream_initialized;
static uint8_t gateway_persistence_retry_round;

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id);
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

static bool gateway_collection_tracking_active(void);
static int gateway_restore_membership_runtime(void);
static int gateway_restore_collection_runtime(void);
static int gateway_restore_click_journal_runtime(void);

static void gateway_schedule_persistence_retry(const char *reason)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        gateway_persistence_retry_round,
        sys_rand32_get());

    if (gateway_persistence_retry_round < UINT8_MAX) {
        gateway_persistence_retry_round++;
    }
    /*
     * Collection persistence reads the same mutable snapshot as EACK
     * generation.  Keep it on the mesh-route owner instead of allowing the
     * system workqueue to race result ingestion or round advancement.
     */
    (void)mesh_route_work_reschedule(&gateway_persistence_retry_work,
                                     delay_ms);
    LOG_WRN("gateway persistence retry scheduled: reason=%s round=%u delay_ms=%u",
            reason == NULL ? "unknown" : reason,
            gateway_persistence_retry_round,
            delay_ms);
}

static void gateway_finalize_collection_clear(void)
{
    gateway_collection_clear(&gateway_collection_state);
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    app_mesh_persistence_clear_gateway_eack_custody();
    gateway_collection_eack_round_dirty = false;
    gateway_collection_persistence_dirty = false;
    gateway_collection_clear_pending = false;
    (void)k_work_cancel_delayable(&gateway_collection_eack_work);
}

static void gateway_finalize_membership_clear(void)
{
    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_membership_persistence_dirty = false;
    gateway_membership_clear_pending = false;
}

static void gateway_persistence_retry_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (gateway_click_journal_restore_pending) {
        ret = gateway_restore_click_journal_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("click-restore");
            return;
        }
    }
    if (gateway_click_journal_clear_pending) {
        ret = app_mesh_persistence_clear_gateway_click_journal();
        if (ret < 0) {
            gateway_schedule_persistence_retry("click-clear");
            return;
        }
        gateway_click_journal_clear_pending = false;
        gateway_click_journal_restored = false;
    }
    if (gateway_membership_restore_pending) {
        ret = gateway_restore_membership_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-restore");
            return;
        }
    }
    if (gateway_collection_restore_pending) {
        ret = gateway_restore_collection_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("collection-restore");
            return;
        }
    }
    if (gateway_membership_clear_pending) {
        ret = app_mesh_persistence_clear_gateway_membership();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-clear");
            return;
        }
        gateway_finalize_membership_clear();
    }
    if (gateway_collection_clear_pending) {
        ret = app_mesh_persistence_clear_gateway_collection();
        if (ret < 0) {
            gateway_schedule_persistence_retry("collection-clear");
            return;
        }
        gateway_finalize_collection_clear();
    }
    if (gateway_collection_persistence_dirty) {
        if (!gateway_collection_tracking_active()) {
            gateway_collection_persistence_dirty = false;
        } else {
            ret = app_mesh_persistence_save_gateway_collection(
                &gateway_collection_state);
            if (ret < 0) {
                gateway_schedule_persistence_retry("collection");
                return;
            }
            gateway_collection_persistence_dirty = false;
        }
    }
    if (gateway_membership_persistence_dirty) {
        ret = app_mesh_persistence_save_gateway_membership(
            &gateway_membership_roster_state);
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership");
            return;
        }
        gateway_membership_persistence_dirty = false;
    }
    gateway_persistence_retry_round = 0u;
}
#endif

int gateway_ble_stream_packet(const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t received_at_ms)
{
    k_spinlock_key_t key;
    bool ready;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    ready = gateway_ble_stream_ready();
    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_enqueue_packet(&gateway_ble_stream_state,
                                            packet,
                                            payload,
                                            payload_len,
                                            received_at_ms,
                                            k_uptime_get_32(),
                                            ready);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret < 0 ? ret : 0;
}

int gateway_ble_reserve_stream_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms)
{
    k_spinlock_key_t key;
    bool ready;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

#if DEVICE_ROLE == ROLE_GATEWAY
    if (packet != NULL && packet->msg_type == MSG_CLICK_REPORT) {
        int journal_ret;

        /* A journal left by a previous boot owns admission until it has been
         * restored into the host queue.  This prevents a new click from
         * overwriting the only durable copy. */
        if (gateway_click_journal_restore_pending ||
            gateway_click_journal_clear_pending) {
            return -EAGAIN;
        }
        journal_ret = app_mesh_persistence_gateway_click_journal_matches(
            packet, payload, payload_len);
        if (journal_ret != 0) {
            return journal_ret;
        }
    }
#endif

    ready = gateway_ble_stream_ready();
    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_reserve_packet(&gateway_ble_stream_state,
                                            packet,
                                            payload,
                                            payload_len,
                                            received_at_ms,
                                            k_uptime_get_32(),
                                            ready);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    return ret;
}

int gateway_ble_commit_stream_reservation(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len)
{
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_commit_reservation(&gateway_ble_stream_state,
                                                packet,
                                                payload,
                                                payload_len);
    k_spin_unlock(&gateway_ble_stream_lock, key);
#if DEVICE_ROLE == ROLE_GATEWAY
    if (ret < 0 && packet != NULL && packet->msg_type == MSG_CLICK_REPORT) {
        gateway_click_journal_restore_pending = true;
        gateway_click_journal_restored = false;
        gateway_schedule_persistence_retry("click-stream-commit");
    }
#endif
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret;
}

void gateway_ble_cancel_stream_reservation(void)
{
    k_spinlock_key_t key;

    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_ble_transport_enabled()) {
        return;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    gateway_ble_stream_cancel_reservation(&gateway_ble_stream_state);
    k_spin_unlock(&gateway_ble_stream_lock, key);
}

void gateway_ble_stream_get_status(struct gateway_ble_stream_diagnostics *diagnostics)
{
    k_spinlock_key_t key;

    if (diagnostics == NULL) {
        return;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    gateway_ble_stream_get_diagnostics(&gateway_ble_stream_state,
                                       k_uptime_get_32(),
                                       diagnostics);
    k_spin_unlock(&gateway_ble_stream_lock, key);
}

#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_restore_click_journal_runtime(void)
{
    struct proto_packet *packet;
    uint8_t *payload;
    size_t payload_len = 0u;
    uint32_t received_at_ms = 0u;
    uint32_t staging_offset =
        GATEWAY_BLE_STREAM_RECORD_POOL_BYTES -
        PACKET_EXT_MAX_PAYLOAD_LEN;
    k_spinlock_key_t key;
    int ret;

    if (!gateway_ble_stream_initialized) {
        gateway_click_journal_restore_pending = true;
        return -EAGAIN;
    }
    if (gateway_click_journal_restored) {
        gateway_click_journal_restore_pending = false;
        return 0;
    }

    /* Do not let a retry overwrite an active semantic reservation or a
     * record already occupying the payload staging tail. */
    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.reservation_active ||
        gateway_ble_stream_state.head_send_active ||
        gateway_ble_stream_state.restore_staging_active ||
        gateway_ble_stream_state.count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH ||
        gateway_ble_stream_state.pool_used > staging_offset) {
        k_spin_unlock(&gateway_ble_stream_lock, key);
        gateway_click_journal_restore_pending = true;
        return -EAGAIN;
    }
    packet = &gateway_ble_stream_state.items[
        GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u].packet;
    payload = &gateway_ble_stream_state.record_pool[staging_offset];
    gateway_ble_stream_state.restore_staging_active = true;
    gateway_ble_stream_state.restore_staging_offset = staging_offset;
    k_spin_unlock(&gateway_ble_stream_lock, key);

    ret = app_mesh_persistence_restore_gateway_click_journal(
        packet,
        payload,
        PACKET_EXT_MAX_PAYLOAD_LEN,
        &payload_len,
        &received_at_ms);
    if (ret == 0 || ret == -EBADMSG) {
        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_state.restore_staging_active = false;
        gateway_ble_stream_state.restore_staging_offset = 0u;
        k_spin_unlock(&gateway_ble_stream_lock, key);
        gateway_click_journal_restore_pending = false;
        gateway_click_journal_restored = false;
        return 0;
    }
    if (ret < 0) {
        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_state.restore_staging_active = false;
        gateway_ble_stream_state.restore_staging_offset = 0u;
        k_spin_unlock(&gateway_ble_stream_lock, key);
        gateway_click_journal_restore_pending = true;
        return ret;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_enqueue_staged_packet(
        &gateway_ble_stream_state,
        packet,
        payload_len,
        received_at_ms,
        k_uptime_get_32(),
        true);
    gateway_ble_stream_state.restore_staging_active = false;
    gateway_ble_stream_state.restore_staging_offset = 0u;
    if (ret > 0) {
        gateway_ble_stream_state.items[
            gateway_ble_stream_state.count - 1u].retain_until_sent = true;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret <= 0) {
        gateway_click_journal_restore_pending = true;
        return ret == 0 ? -EAGAIN : ret;
    }

    gateway_click_journal_restored = true;
    gateway_click_journal_restore_pending = false;
    gateway_ble_schedule_stream_drain();
    return 0;
}

static int gateway_persist_collection_state(const char *reason)
{
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_membership_restore_pending ||
        gateway_collection_restore_pending ||
        gateway_collection_clear_pending) {
        return -ENOENT;
    }

    ret = app_mesh_persistence_save_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        gateway_collection_persistence_dirty = true;
        gateway_schedule_persistence_retry(reason);
        LOG_WRN("gateway collection snapshot save failed: ret=%d reason=%s",
                ret,
                reason == NULL ? "" : reason);
        return ret;
    } else {
        gateway_collection_persistence_dirty = false;
        gateway_persistence_retry_round = 0u;
    }
    return 0;
}

static bool gateway_collection_tracking_active(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY &&
           gateway_collection_state.gateway_id == DEVICE_ID &&
           gateway_collection_state.command_seq != 0u &&
           gateway_collection_state.collection_epoch_id != 0u;
}

static bool gateway_collection_work_pending(void)
{
    return gateway_membership_restore_pending ||
           gateway_collection_restore_pending ||
           gateway_membership_persistence_dirty ||
           gateway_membership_clear_pending ||
           gateway_collection_clear_pending ||
           (gateway_collection_tracking_active() &&
           (gateway_collection_state.collection_open ||
            gateway_collection_state.eack_pending ||
            gateway_collection_eack_retry_state.active ||
            gateway_collection_eack_round_dirty ||
            gateway_collection_persistence_dirty));
}

static void gateway_collection_rollback_failed_start(void)
{
    int ret;

    gateway_collection_persistence_dirty = false;
    gateway_collection_eack_round_dirty = false;
    gateway_persistence_retry_round = 0u;
    ret = app_mesh_persistence_restore_gateway_collection(
        &gateway_collection_state);
    if (ret < 0) {
        gateway_collection_clear(&gateway_collection_state);
        gateway_collection_restore_pending = true;
        gateway_schedule_persistence_retry("collection-start-rollback-restore");
        LOG_WRN("gateway collection start rollback found no durable snapshot: ret=%d",
                ret);
    }
}

static void gateway_schedule_collection_eack_round(void)
{
    if (!gateway_collection_tracking_active() ||
        !gateway_collection_state.eack_pending) {
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        return;
    }

    (void)mesh_route_work_reschedule(
        &gateway_collection_eack_work,
        gateway_collection_state.next_retry_spread_ms);
}

static void gateway_schedule_collection_eack_retry(const char *reason,
                                                   int ret,
                                                   uint64_t failed_next_hop_id)
{
    uint32_t retry_delay_ms;

    retry_delay_ms = app_gateway_eack_retry_note_failure(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        sys_rand32_get());
    app_gateway_eack_retry_note_failed_channel9_target(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        failed_next_hop_id);
    if (retry_delay_ms == 0u) {
        LOG_ERR("gateway collection EACK retry state invalid: command_seq=%u collection=%u ret=%d reason=%s",
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                ret,
                reason == NULL ? "" : reason);
        return;
    }

    (void)mesh_route_work_reschedule(&gateway_collection_eack_work,
                                     retry_delay_ms);
    LOG_WRN("gateway collection EACK send failure retry: command_seq=%u collection=%u eack_round=%u rf_retry_round=%u delay_ms=%u ret=%d reason=%s",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.retry_round,
            gateway_collection_eack_retry_state.rf_retry.retry_round,
            retry_delay_ms,
            ret,
            reason == NULL ? "" : reason);
}

static int gateway_eack_plan_channel9(uint64_t next_hop_id,
                                      struct mesh_event_plan *plan,
                                      void *ctx)
{
    struct mesh_channel5_requirements requirements;

    ARG_UNUSED(ctx);

    mesh_fill_channel5_requirements(&requirements);
    return mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                next_hop_id,
                                                &requirements,
                                                k_uptime_get_32(),
                                                plan);
}

static int gateway_eack_send_channel9(const struct mesh_outbound *out, void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_send_outbound(out, "collection-eack-channel9");
}

static int gateway_eack_prepare_channel9(struct mesh_outbound *out,
                                         const struct mesh_event_plan *plan,
                                         void *ctx)
{
    uint32_t required_ms = 0u;
    int ret;

    ARG_UNUSED(ctx);

    ret = mesh_prepare_channel9_outbound(out, plan, k_uptime_get_32(), &required_ms);
    if (ret < 0) {
        LOG_DBG("gateway collection EACK channel9 slot unavailable: ret=%d req=%u start=%u end=%u",
                ret,
                required_ms,
                plan == NULL ? 0u : plan->start_ms,
                plan == NULL ? 0u : plan->end_ms);
    }
    return ret;
}

struct gateway_eack_send_context {
    bool c5_rf_started;
};

static int gateway_eack_send_c5_flood(const struct mesh_outbound *out, void *ctx)
{
    struct gateway_eack_send_context *send_context = ctx;
    struct app_mesh_flood_result flood_result = {0};
    struct app_mesh_flood_progress *progress =
        &gateway_collection_eack_retry_state.c5_flood_progress;
    int ret;

    if (send_context == NULL) {
        return -EINVAL;
    }
    send_context->c5_rf_started = false;

    /*
     * EACK retry state is the sole custody owner.  Retain resumable progress
     * across busy periods and release the frozen datagram only after all four
     * real EACK transmissions complete.
     */
    ret = mesh_try_send_c5_flood_resume(
        out,
        C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD,
        "collection-eack-c5",
        progress,
        &flood_result,
        &send_context->c5_rf_started);
    if (progress->initialized && !progress->complete) {
        gateway_collection_eack_retry_state.force_c5_recovery = true;
    }
    if (ret == 0 &&
        (!progress->complete ||
         progress->next_opportunity != app_mesh_flood_repeat_limit() ||
         flood_result.sent_count != app_mesh_flood_repeat_limit())) {
        return -EAGAIN;
    }
    return ret;
}

static void gateway_eack_note_tx_sent(const struct mesh_outbound *out, void *ctx)
{
    const struct gateway_eack_send_context *send_context = ctx;

    if (out != NULL && out->radio_channel == UWB_CHANNEL_WAKE_CONTACT &&
        (send_context == NULL || !send_context->c5_rf_started)) {
        return;
    }

    mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
}

static void gateway_eack_note_channel9_tx(uint64_t next_hop_id,
                                          uint32_t event_start_ms,
                                          void *ctx)
{
    ARG_UNUSED(ctx);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_relay_note_channel9_tx(&mesh_runtime,
                                    next_hop_id,
                                    event_start_ms);
    } else {
        mesh_relay_note_channel9_success(&mesh_runtime,
                                         next_hop_id,
                                         event_start_ms);
    }
}

static uint32_t gateway_eack_now_ms(void *ctx)
{
    ARG_UNUSED(ctx);

    return k_uptime_get_32();
}

static int gateway_send_collection_eack(const char *reason,
                                        uint64_t previous_hop_id,
                                        uint8_t received_radio_channel,
                                        const struct mesh_event_plan *current_channel9_plan,
                                        uint64_t *failed_next_hop_id,
                                        bool *used_frozen_snapshot)
{
    struct mesh_outbound eack = {0};
    struct gateway_eack_send_context send_context = {
        .c5_rf_started = false,
    };
    struct app_gateway_collection_eack_input input = {
        .collection = &gateway_collection_state,
        .expected_node_ids = gateway_collection_state.expected_node_ids,
        .expected_node_id_count =
            gateway_collection_state.expected_node_id_count,
        .previous_hop_id = previous_hop_id,
        .received_radio_channel = received_radio_channel,
        .current_channel9_plan = current_channel9_plan,
        .self_id = DEVICE_ID,
        .excluded_channel9_next_hop_ids =
            gateway_collection_eack_retry_state.failed_channel9_next_hop_ids,
        .excluded_channel9_next_hop_count =
            gateway_collection_eack_retry_state.failed_channel9_next_hop_count,
        .force_c5_recovery =
            gateway_collection_eack_retry_state.force_c5_recovery,
    };
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = gateway_eack_plan_channel9,
        .prepare_channel9 = gateway_eack_prepare_channel9,
        .send_channel9 = gateway_eack_send_channel9,
        .send_c5_flood = gateway_eack_send_c5_flood,
        .note_tx_sent = gateway_eack_note_tx_sent,
        .note_channel9_tx = gateway_eack_note_channel9_tx,
        .now_ms = gateway_eack_now_ms,
        .ctx = &send_context,
    };
    struct app_gateway_collection_eack_result result;
    int ret;

    if (failed_next_hop_id != NULL) {
        *failed_next_hop_id = 0u;
    }
    if (used_frozen_snapshot != NULL) {
        *used_frozen_snapshot = false;
    }
    if (!gateway_collection_tracking_active()) {
        return -ENOENT;
    }
    if (!gateway_collection_state.eack_pending) {
        return -EALREADY;
    }
    if (gateway_collection_persistence_dirty) {
        /*
         * An EACK is a durable acceptance statement.  Never put it on RF
         * while the collection update it describes is still RAM-only.
         */
        return -EAGAIN;
    }

    ret = app_gateway_eack_retry_restore(
        &gateway_collection_eack_retry_state,
        &gateway_collection_state,
        &eack);
    if (ret == PROTO_OK) {
        input.use_prebuilt_eack = true;
        if (used_frozen_snapshot != NULL) {
            *used_frozen_snapshot = true;
        }
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        LOG_ERR("gateway collection EACK snapshot restore failed: ret=%d command_seq=%u collection=%u round=%u",
                ret,
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_state.retry_round);
        return -EBADMSG;
    }

    if (!input.use_prebuilt_eack) {
        ret = app_gateway_collection_eack_prepare(&eack, &input, &result);
        if (ret < 0) {
            LOG_WRN("gateway collection EACK build failed before custody: ret=%d command_seq=%u collection=%u round=%u",
                    ret,
                    gateway_collection_state.command_seq,
                    gateway_collection_state.collection_epoch_id,
                    gateway_collection_state.retry_round);
            return ret;
        }
        ret = app_gateway_eack_retry_freeze(
            &gateway_collection_eack_retry_state,
            &gateway_collection_state,
            &eack);
        if (ret != PROTO_OK) {
            LOG_ERR("gateway collection EACK snapshot freeze failed: ret=%d command_seq=%u collection=%u round=%u",
                    ret,
                    gateway_collection_state.command_seq,
                    gateway_collection_state.collection_epoch_id,
                    gateway_collection_state.retry_round);
            return -EBADMSG;
        }
        input.use_prebuilt_eack = true;
    }

    /*
     * Store the exact datagram before every RF attempt.  Repeating an
     * identical NVS write is harmless, and it closes the reset window after
     * an earlier persistence failure without rebuilding mutable payload.
     */
    ret = app_mesh_persistence_save_gateway_eack_custody(
        &gateway_collection_eack_retry_state.snapshot);
    if (ret < 0) {
        LOG_WRN("gateway collection EACK custody save failed before RF: ret=%d command_seq=%u collection=%u round=%u",
                ret,
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_state.retry_round);
        return ret;
    }

    ret = app_gateway_collection_eack_send(
        &eack,
        &input,
        &ops,
        &result);
    if (ret < 0) {
        if (failed_next_hop_id != NULL &&
            result.policy.channel9_attempt_count != 0u) {
            *failed_next_hop_id = result.policy.channel9_next_hop_id;
        }
        LOG_WRN("gateway collection EACK build/send failed: ret=%d candidates=%u mode=%u c9_candidates=%u c9_attempts=%u c9_plan=%d c9_prepare=%d c9_send=%d c5_send=%d",
                ret,
                (unsigned int)result.return_target_count,
                (unsigned int)result.policy.mode,
                (unsigned int)result.policy.channel9_candidate_count,
                (unsigned int)result.policy.channel9_attempt_count,
                result.policy.channel9_plan_ret,
                result.policy.channel9_prepare_ret,
                result.policy.channel9_send_ret,
                result.policy.c5_send_ret);
        return ret;
    }

    LOG_INF("gateway collection EACK sent: command_seq=%u received=%u expected=%u missing=%u open=%u candidates=%u mode=%u c9_next=0x%016llx reason=%s",
            gateway_collection_state.command_seq,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            result.missing_count,
            gateway_collection_state.collection_open ? 1u : 0u,
            (unsigned int)result.return_target_count,
            (unsigned int)result.policy.mode,
            (unsigned long long)result.policy.channel9_next_hop_id,
            reason == NULL ? "" : reason);
    return 0;
}

static void gateway_complete_collection_eack_round(
    const char *reason,
    bool used_frozen_snapshot)
{
    bool send_updated_round =
        used_frozen_snapshot && gateway_collection_eack_round_dirty;
    uint32_t previous_retry_spread_ms =
        gateway_collection_state.next_retry_spread_ms;
    uint16_t previous_eack_sequence = gateway_collection_state.eack_sequence;
    uint8_t previous_retry_round = gateway_collection_state.retry_round;
    bool previous_eack_pending = gateway_collection_state.eack_pending;
    int ret;

    if (!used_frozen_snapshot) {
        gateway_collection_eack_round_dirty = false;
    }

    ret = gateway_collection_advance_retry_round(&gateway_collection_state);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection retry round advance failed: ret=%d", ret);
        gateway_schedule_collection_eack_retry(reason, -EBADMSG, 0u);
        return;
    }
    gateway_collection_state.eack_pending =
        gateway_collection_state.collection_open || send_updated_round;
    ret = gateway_persist_collection_state(reason);
    if (ret < 0) {
        /*
         * The previously persisted collection identity still owns the
         * frozen EACK.  Roll RAM back to that identity and retry the exact
         * datagram instead of exposing a half-committed next round.
         */
        gateway_collection_state.retry_round = previous_retry_round;
        gateway_collection_state.eack_sequence = previous_eack_sequence;
        gateway_collection_state.next_retry_spread_ms =
            previous_retry_spread_ms;
        gateway_collection_state.eack_pending = previous_eack_pending;
        gateway_collection_persistence_dirty = false;
        gateway_schedule_collection_eack_retry(reason, ret, 0u);
        return;
    }

    ret = app_gateway_eack_retry_commit_success(
        &gateway_collection_eack_retry_state);
    if (ret != PROTO_OK) {
        LOG_ERR("gateway collection EACK RAM custody commit failed after durable round advance: ret=%d",
                ret);
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    }
    app_mesh_persistence_clear_gateway_eack_custody();

    if (send_updated_round) {
        gateway_collection_eack_round_dirty = false;
        (void)mesh_route_work_reschedule(&gateway_collection_eack_work, 0u);
        return;
    }

    gateway_collection_eack_round_dirty = false;
    gateway_schedule_collection_eack_round();
}

static void gateway_collection_eack_work_handler(struct k_work *work)
{
    uint64_t failed_next_hop_id = 0u;
    bool used_frozen_snapshot = false;
    int ret;

    ARG_UNUSED(work);

    if (!gateway_collection_tracking_active()) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        app_mesh_persistence_clear_gateway_eack_custody();
        return;
    }
    if (!gateway_collection_state.eack_pending) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        app_mesh_persistence_clear_gateway_eack_custody();
        return;
    }

    ret = gateway_send_collection_eack("collection-eack-round",
                                       0u,
                                       0u,
                                       NULL,
                                       &failed_next_hop_id,
                                       &used_frozen_snapshot);
    if (ret < 0) {
        gateway_schedule_collection_eack_retry("collection-eack-round",
                                               ret,
                                               failed_next_hop_id);
        return;
    }
    gateway_complete_collection_eack_round("collection-eack-round",
                                           used_frozen_snapshot);
}

static int gateway_accept_collection_record(
    const char *reason,
    bool collection_changed)
{
    bool reopened = !gateway_collection_state.eack_pending;
    int ret;

    if (reopened) {
        gateway_collection_state.eack_pending = true;
    }
    if (collection_changed || reopened || gateway_collection_persistence_dirty) {
        ret = gateway_persist_collection_state(reason);
        if (ret < 0) {
            if (collection_changed) {
                int rollback_ret =
                    app_mesh_persistence_rollback_gateway_collection(
                        &gateway_collection_state);

                gateway_collection_persistence_dirty = false;
                if (rollback_ret < 0) {
                    LOG_ERR("gateway collection result rollback failed: ret=%d persist_ret=%d",
                            rollback_ret,
                            ret);
                }
            } else if (reopened) {
                gateway_collection_state.eack_pending = false;
                gateway_collection_persistence_dirty = false;
            }
            return ret;
        }
    }

    if (gateway_collection_eack_retry_state.active) {
        if (collection_changed) {
            gateway_collection_eack_round_dirty = true;
        }
        LOG_DBG("gateway collection EACK retry already pending after record: command_seq=%u collection=%u rf_retry_round=%u",
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_eack_retry_state.rf_retry.retry_round);
    }

    return 0;
}

static int gateway_drive_collection_eack_after_record(
    const char *reason,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan)
{
    bool used_frozen_snapshot = false;
    uint64_t failed_next_hop_id = 0u;
    int ret;

    if (!gateway_collection_tracking_active() ||
        !gateway_collection_state.eack_pending) {
        return -ESTALE;
    }
    if (gateway_collection_eack_retry_state.active) {
        return 0;
    }

    ret = gateway_send_collection_eack(reason,
                                       previous_hop_id,
                                       received_radio_channel,
                                       current_channel9_plan,
                                       &failed_next_hop_id,
                                       &used_frozen_snapshot);
    if (ret < 0) {
        gateway_schedule_collection_eack_retry(reason,
                                               ret,
                                               failed_next_hop_id);
    } else {
        gateway_complete_collection_eack_round(reason,
                                               used_frozen_snapshot);
    }

    /* The result itself is safely accepted once its collection is durable. */
    return 0;
}

static int gateway_note_collection_result(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    bool duplicate = false;
    int accept_ret;
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_collection_clear_pending ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT) {
        return -ENOENT;
    }

    ret = gateway_collection_record_result_from_hop(&gateway_collection_state,
                                                    packet,
                                                    payload,
                                                    payload_len,
                                                    previous_hop_id,
                                                    &duplicate);
    if (ret != PROTO_OK) {
        LOG_DBG("gateway collection result ignored: src=0x%016llx ret=%d",
                packet == NULL ? 0ull : (unsigned long long)packet->src_id,
                ret);
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection result recorded: src=0x%016llx command_seq=%u received=%u expected=%u duplicate=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            duplicate ? 1u : 0u);
    accept_ret = gateway_accept_collection_record(
        duplicate ? "collection-eack-result-duplicate" :
                    "collection-eack-result",
        !duplicate);
    if (accept_ret < 0) {
        return accept_ret;
    }
    return duplicate ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                       APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}

static int gateway_note_collection_bundle(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;
    int accept_ret;
    int ret;

    if (!gateway_collection_tracking_active() ||
        gateway_collection_clear_pending ||
        packet == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE) {
        return -ENOENT;
    }

    ret = gateway_collection_record_bundle_from_hop(&gateway_collection_state,
                                                    packet,
                                                    payload,
                                                    payload_len,
                                                    previous_hop_id,
                                                    &accepted_count,
                                                    &duplicate_count);
    if (ret != PROTO_OK) {
        LOG_DBG("gateway collection bundle ignored: src=0x%016llx ret=%d",
                packet == NULL ? 0ull : (unsigned long long)packet->src_id,
                ret);
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection bundle recorded: src=0x%016llx command_seq=%u accepted=%u duplicate=%u received=%u expected=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            (unsigned int)accepted_count,
            (unsigned int)duplicate_count,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count);
    if (accepted_count == 0u && duplicate_count == 0u) {
        return -EBADMSG;
    }
    accept_ret = gateway_accept_collection_record(
        accepted_count == 0u ? "collection-eack-bundle-duplicate" :
                               "collection-eack-bundle",
        accepted_count != 0u);
    if (accept_ret < 0) {
        return accept_ret;
    }
    return accepted_count == 0u ? APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
                                  APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
}
#endif

int gateway_finalize_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    int semantic_acceptance)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    const uint8_t *collection_epoch_raw = NULL;
    uint8_t collection_epoch_len = 0u;
    uint32_t collection_epoch_id;
    bool collection_packet = false;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        (semantic_acceptance != APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
         semantic_acceptance != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE)) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        collection_packet = true;
    } else if (packet->msg_type == MSG_COMMAND_RESULT &&
               tlv_find(payload,
                        payload_len,
                        TLV_COLLECTION_EPOCH_ID,
                        &collection_epoch_raw,
                        &collection_epoch_len) == PROTO_OK &&
               collection_epoch_len == sizeof(uint32_t)) {
        collection_epoch_id = proto_get_u32_le(collection_epoch_raw);
        collection_packet =
            collection_epoch_id == gateway_collection_state.collection_epoch_id;
    }
    if (!collection_packet) {
        return 0;
    }
    if (!gateway_collection_tracking_active() ||
        packet->session_id != gateway_collection_state.command_seq) {
        return -ESTALE;
    }

    return gateway_drive_collection_eack_after_record(
        semantic_acceptance == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE ?
            "collection-eack-semantic-duplicate" :
            "collection-eack-semantic-new",
        previous_hop_id,
        received_radio_channel,
        current_channel9_plan);
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    ARG_UNUSED(semantic_acceptance);
    return -ENOTSUP;
#endif
}

int gateway_encode_host_packet_frame(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *frame,
                                     size_t frame_cap,
                                     size_t *frame_len,
                                     struct proto_packet *frame_packet)
{
    int ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        payload_len > UINT8_MAX || frame == NULL || frame_len == NULL ||
        frame_packet == NULL) {
        return -EINVAL;
    }

    *frame_packet = *packet;
    frame_packet->payload_len = (uint8_t)payload_len;
    ret = serial_frame_encode_packet(frame_packet,
                                     payload,
                                     frame,
                                     frame_cap,
                                     frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    return 0;
}

int gateway_emit_host_packet(const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len)
{
    struct proto_packet frame_packet;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY &&
        !(DEVICE_ROLE == ROLE_CLICKER && IS_ENABLED(CONFIG_IMEC_ML_CLICKER)) &&
        !(DEVICE_ROLE == ROLE_ANCHOR && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST))) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    ret = gateway_encode_host_packet_frame(packet,
                                           payload,
                                           payload_len,
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &frame_packet);
    if (ret < 0) {
        return ret;
    }

    ret = gateway_ble_send_packet_frame(frame, frame_len);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(gateway_ble_notify_failures);
        return ret;
    }

    LOG_INF("gateway BLE COBS frame emitted: msg=0x%02x src=0x%016llx seq=%u payload_len=%u frame_len=%u",
            frame_packet.msg_type,
            (unsigned long long)frame_packet.src_id,
            frame_packet.seq,
            (unsigned int)payload_len,
            (unsigned int)frame_len);
    HIGH_DEBUG_COUNTER_INC(gateway_packets_emitted);
    high_debug_log_event("BLE_GATEWAY_PACKET_TX",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u frame_len=%u",
                         frame_packet.msg_type,
                         (unsigned long long)frame_packet.src_id,
                         (unsigned long long)frame_packet.dst_id,
                         frame_packet.seq,
                         (unsigned int)payload_len,
                         (unsigned int)frame_len);
    return 0;
}

#if DEVICE_ROLE == ROLE_GATEWAY
void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    struct proto_packet result = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    if (command == NULL || DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = gateway_command_build_result(command,
                                       DEVICE_ID,
                                       command_id,
                                       status,
                                       reason,
                                       k_uptime_get_32(),
                                       &result,
                                       payload,
                                       sizeof(payload),
                                       &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway command failure result build failed: %d", ret);
        return;
    }

    ret = gateway_emit_host_packet(&result, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE command failure result not emitted: %d", ret);
    }
    HIGH_DEBUG_COUNTER_INC(command_result_tx);
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=gateway_ble command=0x%04x status=%s reason=%u ret=%d",
                         (unsigned int)command_id,
                         command_status_name(status),
                         reason,
                         ret);
}

static bool gateway_pending_command_matches(const struct proto_packet *command)
{
    if (command == NULL || !gateway_command_pending_state.active) {
        return false;
    }

    return gateway_command_pending_state.command.dst_id == command->dst_id &&
           gateway_command_pending_state.command.session_id == command->session_id &&
           gateway_command_pending_state.command.seq == command->seq;
}

void gateway_clear_pending_command_result(const struct proto_packet *command)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_pending_command_matches(command)) {
        return;
    }

    gateway_command_pending_clear(&gateway_command_pending_state);
    (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
}

int gateway_begin_command_collection(const struct gateway_command_options *options)
{
    enum gateway_command_collection_roster_source roster_source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;
    uint64_t resolved_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP] = {0};
    size_t resolved_node_count = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (gateway_command_pending_state.active || gateway_collection_work_pending()) {
        return -EBUSY;
    }

    ret = gateway_command_resolve_collection_roster(options,
                                                    &gateway_membership_roster_state,
                                                    resolved_node_ids,
                                                    ARRAY_SIZE(resolved_node_ids),
                                                    &resolved_node_count,
                                                    &roster_source);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection roster unavailable: ret=%d command_seq=%u epoch=%u expected=%u explicit=%u",
                ret,
                options->command_seq,
                options->membership_epoch,
                options->expected_node_count,
                options->expected_node_id_count);
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_collection_start(&gateway_collection_state,
                                   DEVICE_ID,
                                   (uint16_t)mesh_runtime.upstream.current_epoch,
                                   options->command_seq,
                                   options->collection_epoch_id,
                                   options->membership_epoch,
                                   options->expected_node_count,
                                   0u,
                                   COLLECTION_RETRY_ROUND_0_MS);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = gateway_collection_set_expected_roster(&gateway_collection_state,
                                                 resolved_node_ids,
                                                 resolved_node_count);
    if (ret != PROTO_OK) {
        gateway_collection_rollback_failed_start();
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection tracking started: command_seq=%u collection=%u epoch=%u expected=%u roster_count=%u roster_source=%u",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.gateway_epoch,
            gateway_collection_state.expected_count,
            gateway_collection_state.expected_node_id_count,
            (unsigned int)roster_source);
    ret = gateway_persist_collection_state("collection-start");
    if (ret < 0) {
        /*
         * The previous closed collection remains the durable snapshot.  Do
         * not expose or schedule a new collection that exists only in RAM.
         */
        gateway_collection_rollback_failed_start();
        return ret;
    }
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    app_mesh_persistence_clear_gateway_eack_custody();
    gateway_collection_eack_round_dirty = false;
    gateway_schedule_collection_eack_round();
    return 0;
}

int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             size_t node_count)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }
    if (gateway_membership_restore_pending ||
        gateway_collection_restore_pending ||
        gateway_membership_persistence_dirty ||
        gateway_membership_clear_pending) {
        return -EAGAIN;
    }

    ret = gateway_membership_set_roster_preserve_order(&gateway_membership_roster_state,
                                                       membership_epoch,
                                                       node_ids,
                                                       node_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    ret = app_mesh_persistence_save_gateway_membership(&gateway_membership_roster_state);
    if (ret < 0) {
        gateway_membership_persistence_dirty = true;
        gateway_schedule_persistence_retry("membership-set");
        LOG_WRN("gateway membership snapshot save failed: ret=%d", ret);
        return ret;
    }
    gateway_membership_persistence_dirty = false;
    return 0;
}

void gateway_clear_registered_membership_roster(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || gateway_membership_restore_pending ||
        gateway_collection_restore_pending) {
        return;
    }

    ret = app_mesh_persistence_clear_gateway_membership();
    if (ret < 0) {
        gateway_membership_clear_pending = true;
        gateway_membership_persistence_dirty = false;
        gateway_schedule_persistence_retry("membership-clear");
        LOG_WRN("gateway membership clear deferred until NVS delete succeeds: ret=%d",
                ret);
        return;
    }
    gateway_finalize_membership_clear();
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL ||
        !gateway_collection_tracking_active()) {
        return;
    }
    if (gateway_collection_state.command_seq != options->command_seq ||
        gateway_collection_state.collection_epoch_id != options->collection_epoch_id) {
        return;
    }

    ret = app_mesh_persistence_clear_gateway_collection();
    if (ret < 0) {
        gateway_collection_clear_pending = true;
        gateway_collection_persistence_dirty = false;
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        gateway_schedule_persistence_retry("collection-clear");
        LOG_WRN("gateway collection clear deferred until journal tombstone persists: ret=%d",
                ret);
        return;
    }
    gateway_finalize_collection_clear();
}

static void gateway_command_result_timeout_handler(struct k_work *work)
{
    struct proto_packet command = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    if (!gateway_command_pending_expired(&gateway_command_pending_state,
                                         k_uptime_get_32(),
                                         &command,
                                         &command_id)) {
        return;
    }

    LOG_WRN("gateway command result timeout: cmd=0x%04x dst=0x%016llx session=%u seq=%u",
            (unsigned int)command_id,
            (unsigned long long)command.dst_id,
            command.session_id,
            command.seq);
    mesh_relay_note_delivery_failure_at(&mesh_runtime,
                                        command.dst_id,
                                        k_uptime_get_32());
    gateway_command_timeout_side_effects(&command, command_id);
    gateway_emit_host_command_result(&command, command_id, COMMAND_TIMEOUT, 0u);
}

int gateway_note_command_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t previous_hop_id,
                                uint8_t received_radio_channel,
                                const struct mesh_event_plan *current_channel9_plan)
{
    struct proto_packet command;
    enum command_id pending_command_id;
    enum gateway_command_result_admission admission;
    bool auto_transaction_owned;
    int claim_ret;
    bool pending_matches;
    bool survey_transaction_result;
    int collection_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);

    claim_ret = gateway_discovery_assignment_note_claim(packet,
                                                        payload,
                                                        payload_len,
                                                        previous_hop_id);
    if (claim_ret != -ENOENT && claim_ret != -ENOTSUP) {
        return claim_ret;
    }

    collection_ret = gateway_note_collection_result(packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id);

    auto_transaction_owned = gateway_survey_auto_owns_pending_command(
        &gateway_command_pending_state.command,
        gateway_command_pending_state.command_id);
    pending_matches = gateway_command_pending_matches_result(
        &gateway_command_pending_state, packet);
    survey_transaction_result = false;
    if (auto_transaction_owned || !pending_matches) {
        survey_transaction_result = gateway_survey_auto_preflight_result(
            packet, payload, payload_len);
    }
    admission = gateway_command_result_admit(&gateway_command_pending_state,
                                              packet,
                                              auto_transaction_owned,
                                              survey_transaction_result);

    if (admission == GATEWAY_COMMAND_RESULT_IGNORE) {
        if (survey_transaction_result) {
            LOG_DBG("gateway survey duplicate/late result reconciled: src=0x%016llx session=%u seq=%u",
                    packet == NULL ? 0ull :
                    (unsigned long long)packet->src_id,
                    packet == NULL ? 0u : packet->session_id,
                    packet == NULL ? 0u : packet->seq);
        }
        if (collection_ret >= 0) {
            return collection_ret;
        }
        return survey_transaction_result ?
               APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE : -ENOENT;
    }
    if (admission == GATEWAY_COMMAND_RESULT_WAIT) {
        LOG_WRN("gateway survey result identity rejected while wait remains active: src=0x%016llx session=%u seq=%u",
                packet == NULL ? 0ull :
                (unsigned long long)packet->src_id,
                packet == NULL ? 0u : packet->session_id,
                packet == NULL ? 0u : packet->seq);
        return collection_ret >= 0 ? collection_ret : -EAGAIN;
    }

    command = gateway_command_pending_state.command;
    pending_command_id = gateway_command_pending_state.command_id;
    ret = app_mesh_command_orchestrator_gateway_deliver(
        &command,
        pending_command_id,
        packet,
        payload,
        payload_len,
        gateway_command_delivery_boundary,
        NULL);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway command result payload mismatch: pending=0x%04x ret=%d",
                (unsigned int)pending_command_id,
                ret);
    }

    if (ret == PROTO_OK) {
        gateway_command_pending_clear(&gateway_command_pending_state);
        (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
        LOG_INF("gateway command result received: src=0x%016llx session=%u seq=%u",
                (unsigned long long)packet->src_id,
                packet->session_id,
                packet->seq);
        return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }
    return collection_ret >= 0 ? collection_ret : mesh_errno_from_proto(ret);
}

int gateway_note_command_result_bundle(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint8_t received_radio_channel,
                                       const struct mesh_event_plan *current_channel9_plan)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);

    return gateway_note_collection_bundle(packet,
                                          payload,
                                          payload_len,
                                          previous_hop_id);
}

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    return gateway_begin_command_result_wait_for(
        command, command_id, GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
}

int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms)
{
    int ret;

    if (timeout_ms == 0u) {
        return -EINVAL;
    }
    if (gateway_collection_work_pending()) {
        return -EBUSY;
    }

    ret = gateway_command_pending_start(&gateway_command_pending_state,
                                        command,
                                        command_id,
                                        k_uptime_get_32(),
                                        timeout_ms);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_MALFORMED ? -EBUSY : mesh_errno_from_proto(ret);
    }

    (void)k_work_reschedule(&gateway_command_result_timeout_work,
                            K_MSEC(timeout_ms));
    return 0;
}

static int gateway_restore_membership_runtime(void)
{
    int ret = app_mesh_persistence_restore_gateway_membership(
        &gateway_membership_roster_state);

    if (ret < 0) {
        gateway_membership_clear(&gateway_membership_roster_state);
        gateway_membership_restore_pending = true;
        LOG_WRN("gateway membership snapshot restore unavailable: ret=%d", ret);
        return ret;
    }
    gateway_membership_restore_pending = false;
    return 0;
}

static int gateway_restore_collection_runtime(void)
{
    struct gateway_collection_eack decoded_eack;
    int ret;

    ret = app_mesh_persistence_restore_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        gateway_collection_clear(&gateway_collection_state);
        gateway_collection_restore_pending = true;
        LOG_WRN("gateway collection journal restore unavailable: ret=%d", ret);
        return ret;
    }
    gateway_collection_restore_pending = false;
    if (!gateway_collection_tracking_active()) {
        app_mesh_persistence_clear_gateway_eack_custody();
        return 0;
    }

    LOG_INF("gateway collection tracking restored: command_seq=%u collection=%u received=%u expected=%u open=%u eack_pending=%u",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            gateway_collection_state.collection_open ? 1u : 0u,
            gateway_collection_state.eack_pending ? 1u : 0u);
    if (!gateway_collection_state.eack_pending) {
        app_mesh_persistence_clear_gateway_eack_custody();
        return 0;
    }

    ret = app_mesh_persistence_restore_gateway_eack_custody(
        &gateway_collection_eack_retry_state.snapshot);
    if (ret < 0) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        LOG_WRN("gateway collection EACK custody restore unavailable: ret=%d",
                ret);
    } else if (gateway_collection_eack_retry_state.snapshot.valid) {
        ret = app_gateway_eack_retry_import_custody(
            &gateway_collection_eack_retry_state,
            &gateway_collection_state,
            &gateway_collection_eack_retry_state.snapshot);
        if (ret == PROTO_OK) {
            ret = gateway_collection_eack_packet_validate(
                &gateway_collection_eack_retry_state.snapshot.packet,
                gateway_collection_eack_retry_state.snapshot.payload,
                gateway_collection_eack_retry_state.snapshot.payload_len,
                &decoded_eack);
        }
        if (ret == PROTO_OK) {
            gateway_collection_eack_round_dirty =
                decoded_eack.received_count !=
                    gateway_collection_state.received_count ||
                decoded_eack.collection_open !=
                    gateway_collection_state.collection_open;
            gateway_schedule_collection_eack_retry(
                "collection-eack-reset-resume", -EAGAIN, 0u);
            return 0;
        }

        LOG_WRN("gateway collection EACK custody is stale for restored collection: ret=%d",
                ret);
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        app_mesh_persistence_clear_gateway_eack_custody();
    }

    if (!gateway_collection_state.collection_open) {
        /* A reset landed after the final result but before its EACK custody. */
        gateway_schedule_collection_eack_retry(
            "collection-eack-final-reset-gap", -EAGAIN, 0u);
    } else {
        gateway_schedule_collection_eack_round();
    }
    return 0;
}

void gateway_command_result_tracking_init(void)
{
    int ret;

    gateway_collection_clear(&gateway_collection_state);
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_eack_round_dirty = false;
    gateway_membership_clear(&gateway_membership_roster_state);
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
    k_work_init_delayable(&gateway_collection_eack_work,
                          gateway_collection_eack_work_handler);
    k_work_init_delayable(&gateway_persistence_retry_work,
                          gateway_persistence_retry_work_handler);
    gateway_collection_persistence_dirty = false;
    gateway_collection_clear_pending = false;
    gateway_collection_restore_pending = true;
    gateway_membership_persistence_dirty = false;
    gateway_membership_clear_pending = false;
    gateway_membership_restore_pending = true;
    gateway_click_journal_restore_pending = true;
    gateway_click_journal_clear_pending = false;
    gateway_click_journal_restored = false;
    gateway_ble_stream_initialized = false;
    gateway_persistence_retry_round = 0u;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = gateway_restore_membership_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("membership-restore");
        return;
    }

    ret = gateway_restore_collection_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("collection-restore");
    }
}
#else
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

void gateway_clear_pending_command_result(const struct proto_packet *command)
{
    ARG_UNUSED(command);
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
                                             size_t node_count)
{
    ARG_UNUSED(membership_epoch);
    ARG_UNUSED(node_ids);
    ARG_UNUSED(node_count);
    return -ENOTSUP;
}

void gateway_clear_registered_membership_roster(void)
{
}

int gateway_note_command_result(const struct proto_packet *packet,
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
#endif
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
static struct gateway_ble_frame_pending
    gateway_ble_direct_tx_queue[GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH];
static uint8_t gateway_ble_direct_tx_head;
static uint8_t gateway_ble_direct_tx_count;

static const struct bt_data gateway_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, GATEWAY_BLE_DEVICE_NAME, GATEWAY_BLE_DEVICE_NAME_LEN),
};

static const struct bt_data gateway_ble_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_IMEC_GATEWAY_SERVICE_VAL),
};

static void gateway_ble_rx_bytes(const uint8_t *data, size_t len);
static int gateway_ble_start_advertising(void);
static int gateway_ble_stop_advertising(const char *reason);
static void gateway_ble_rx_work_handler(struct k_work *work);
static void gateway_ble_stream_work_handler(struct k_work *work);
static void gateway_ble_recovery_work_handler(struct k_work *work);
static void gateway_ble_tx_complete(struct bt_conn *conn, void *user_data);

static void gateway_ble_schedule_recovery(const char *reason)
{
    uint32_t delay_ms;

    if (!gateway_ble_transport_enabled() || gateway_ble_conn != NULL) {
        return;
    }
    delay_ms = gateway_ble_recovery_backoff_ms(gateway_ble_recovery_round,
                                               sys_rand32_get());
    if (gateway_ble_recovery_round < UINT8_MAX) {
        gateway_ble_recovery_round++;
    }
    (void)k_work_reschedule(&gateway_ble_recovery_work, K_MSEC(delay_ms));
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
        (void)k_work_reschedule(&gateway_ble_stream_work, K_NO_WAIT);
    }
}

static uint32_t gateway_ble_stream_retry_delay_ms(void)
{
    uint8_t shift = MIN(gateway_ble_notify_failure_count, 6u);
    uint32_t delay_ms = GATEWAY_BLE_TX_RETRY_MS << shift;

    return MIN(delay_ms, GATEWAY_BLE_TX_RETRY_MAX_MS);
}

static void gateway_ble_schedule_stream_retry(void)
{
    if (gateway_ble_transport_enabled()) {
        (void)k_work_reschedule(&gateway_ble_stream_work,
                                K_MSEC(gateway_ble_stream_retry_delay_ms()));
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
    LOG_INF("gateway BLE quiet during UWB: reason=%s",
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
#endif
    gateway_ble_schedule_stream_drain();
    LOG_INF("gateway BLE resumed after UWB: reason=%s",
            reason == NULL ? "unknown" : reason);
}

static void gateway_ble_packet_ccc_changed(const struct bt_gatt_attr *attr,
                                           uint16_t value)
{
    ARG_UNUSED(attr);

    gateway_ble_packet_notify_enabled = value == BT_GATT_CCC_NOTIFY;
    if (gateway_ble_packet_notify_enabled) {
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
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (!gateway_ble_transport_enabled()) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    gateway_ble_rx_bytes(buf, len);
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
    gateway_ble_notify_failure_count = 0u;
    gateway_ble_direct_tx_head = 0u;
    gateway_ble_direct_tx_count = 0u;
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

    if (gateway_ble_direct_tx_count > 0u && gateway_ble_packet_notify_enabled) {
        const struct gateway_ble_frame_pending *pending =
            &gateway_ble_direct_tx_queue[gateway_ble_direct_tx_head];

        memcpy(gateway_ble_tx_frame, pending->frame, pending->len);
        gateway_ble_tx_frame_len = pending->len;
        gateway_ble_tx_frame_offset = 0u;
        gateway_ble_tx_source = GATEWAY_BLE_TX_DIRECT;
        gateway_ble_direct_tx_head =
            (uint8_t)((gateway_ble_direct_tx_head + 1u) %
                      GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH);
        gateway_ble_direct_tx_count--;
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
    if (gateway_ble_direct_tx_count >= GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return -ENOSPC;
    }

    pending = &gateway_ble_direct_tx_queue[
        (gateway_ble_direct_tx_head + gateway_ble_direct_tx_count) %
        GATEWAY_BLE_DIRECT_TX_QUEUE_DEPTH];
    memcpy(pending->frame, frame, frame_len);
    pending->len = (uint16_t)frame_len;
    gateway_ble_direct_tx_count++;
    k_spin_unlock(&gateway_ble_tx_lock, key);
    gateway_ble_schedule_stream_drain();
    return 0;
}

static void gateway_ble_tx_complete(struct bt_conn *conn, void *user_data)
{
    enum gateway_ble_tx_source completed_source = GATEWAY_BLE_TX_NONE;
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    bool frame_complete = false;
    k_spinlock_key_t key;

    ARG_UNUSED(conn);

    key = k_spin_lock(&gateway_ble_tx_lock);
    if (!gateway_ble_tx_in_flight || generation != gateway_ble_tx_generation) {
        k_spin_unlock(&gateway_ble_tx_lock, key);
        return;
    }

    gateway_ble_tx_in_flight = false;
    gateway_ble_notify_failure_count = 0u;
    gateway_ble_tx_frame_offset += gateway_ble_tx_chunk_len;
    gateway_ble_tx_chunk_len = 0u;
    frame_complete = gateway_ble_tx_frame_offset >= gateway_ble_tx_frame_len;
    if (frame_complete) {
        completed_source = gateway_ble_tx_source;
        gateway_ble_tx_source = GATEWAY_BLE_TX_NONE;
        gateway_ble_tx_frame_len = 0u;
        gateway_ble_tx_frame_offset = 0u;
    }
    k_spin_unlock(&gateway_ble_tx_lock, key);

    if (completed_source == GATEWAY_BLE_TX_STREAM) {
        struct proto_packet completed_packet;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_stream_lock);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &completed_packet);
        gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                     k_uptime_get_32());
        uint16_t queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (packet_ret == 0) {
#if DEVICE_ROLE == ROLE_GATEWAY
            if (completed_packet.msg_type == MSG_CLICK_REPORT) {
                int clear_ret =
                    app_mesh_persistence_clear_gateway_click_journal_if_matches(
                        &completed_packet);

                if (clear_ret < 0) {
                    gateway_click_journal_clear_pending = true;
                    gateway_schedule_persistence_retry("click-clear-send");
                    LOG_WRN("gateway click journal clear deferred after BLE send: ret=%d",
                            clear_ret);
                } else {
                    gateway_click_journal_clear_pending = false;
                    gateway_click_journal_restored = false;
                }
            }
#endif
            if (completed_packet.msg_type == MSG_GATEWAY_COMMAND_EVENT) {
                gateway_command_observability_mark_sent(
                    &gateway_command_observability_state,
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
            app_gateway_assignment_publisher_note_sent(
                completed_packet.session_id);
        }
        app_gateway_assignment_publisher_pump();
#endif
    }
    gateway_ble_schedule_stream_drain();
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
        struct proto_packet blocked_packet;
        uint16_t queue_depth;
        int packet_ret;
        k_spinlock_key_t stream_key = k_spin_lock(&gateway_ble_stream_lock);

        queue_depth = gateway_ble_stream_depth(&gateway_ble_stream_state);
        packet_ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                                    &blocked_packet);
        k_spin_unlock(&gateway_ble_stream_lock, stream_key);
        k_spin_unlock(&gateway_ble_tx_lock, key);
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
        gateway_ble_schedule_stream_retry();
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
    generation = gateway_ble_tx_generation;
    attr = &gateway_ble_svc.attrs[GATEWAY_BLE_PACKET_TX_ATTR_INDEX];
    k_spin_unlock(&gateway_ble_tx_lock, key);

    params.attr = attr;
    params.data = gateway_ble_tx_chunk;
    params.len = (uint16_t)gateway_ble_tx_chunk_len;
    params.func = gateway_ble_tx_complete;
    params.user_data = (void *)(uintptr_t)generation;
    ret = bt_gatt_notify_cb(conn, &params);
    if (ret < 0) {
        struct proto_packet blocked_packet;
        uint16_t queue_depth;
        int packet_ret;

        key = k_spin_lock(&gateway_ble_tx_lock);
        if (gateway_ble_tx_in_flight && generation == gateway_ble_tx_generation) {
            gateway_ble_tx_in_flight = false;
            gateway_ble_tx_chunk_len = 0u;
        }
        if (gateway_ble_notify_failure_count < UINT8_MAX) {
            gateway_ble_notify_failure_count++;
        }
        k_spin_unlock(&gateway_ble_tx_lock, key);
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
                .retry_depth = gateway_ble_notify_failure_count,
                .drain_depth = queue_depth,
            };

            app_stack_workload_diag_ble_admit_with_pressure(
                &blocked_packet, APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE,
                &pressure);
            app_stack_workload_diag_ble_sample_with_pressure(&blocked_packet,
                                                             &pressure);
        }
        if (gateway_ble_notify_failure_count != 0u &&
            (gateway_ble_notify_failure_count &
             (gateway_ble_notify_failure_count - 1u)) == 0u) {
            LOG_WRN("gateway BLE notification deferred: ret=%d failures=%u retry_ms=%u",
                    ret,
                    gateway_ble_notify_failure_count,
                    gateway_ble_stream_retry_delay_ms());
        }
        gateway_ble_schedule_stream_retry();
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
    gateway_ble_notify_failure_count = 0u;
    (void)k_work_cancel_delayable(&gateway_ble_recovery_work);
    HIGH_DEBUG_COUNTER_INC(gateway_ble_connects);
    LOG_INF("gateway BLE PC link connected");
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
    LOG_INF("gateway BLE PC link disconnected: reason=0x%02x", reason);
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
    LOG_INF("gateway BLE advertising start requested: ret=%d name=%s",
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
        LOG_INF("gateway BLE identity: index=%u addr=%s",
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
        LOG_INF("gateway BLE recovery bt_enable completed: ret=%d", ret);
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
    LOG_INF("gateway BLE PC link advertising as %s", GATEWAY_BLE_DEVICE_NAME);
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
    LOG_INF("gateway BLE advertising stopped: reason=%s primary_channels=37-39",
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
    gateway_ble_stream_initialized = true;
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        gateway_click_journal_restored = false;
        gateway_click_journal_restore_pending = true;
        ret = gateway_restore_click_journal_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("click-restore-init");
        }
    }
    gateway_command_observability_init(&gateway_command_observability_state);
    gateway_ble_tx_reset_locked();
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    gateway_ble_stack_ready = false;
    gateway_ble_recovery_round = 0u;
    gateway_ble_notify_failure_count = 0u;

    ret = bt_enable(NULL);
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_GATEWAY_BLE stage=bt_enable ret=%d uptime=%u\n",
                        ret,
                        k_uptime_get_32());
#endif
    LOG_INF("gateway BLE bt_enable completed: ret=%d", ret);
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

    LOG_INF("gateway BLE PC link advertising as %s", GATEWAY_BLE_DEVICE_NAME);
    high_debug_log_event("BLE_GATEWAY_READY",
                         "device_name=%s packet_notify=0",
                         GATEWAY_BLE_DEVICE_NAME);
    return 0;
}

static void gateway_ble_queue_frame(void)
{
    struct gateway_ble_frame_pending pending = {0};
    int ret;

    if (gateway_ble_rx_len <= 1u) {
        gateway_ble_rx_len = 0u;
        return;
    }

    pending.len = (uint16_t)gateway_ble_rx_len;
    memcpy(pending.frame, gateway_ble_rx_frame, gateway_ble_rx_len);
    ret = k_msgq_put(&gateway_ble_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(gateway_ble_rx_drops);
        LOG_WRN("gateway BLE RX frame queue full: len=%u", pending.len);
    } else {
        (void)k_work_submit(&gateway_ble_rx_work);
    }
    gateway_ble_rx_len = 0u;
}

static void gateway_ble_rx_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL && len != 0u) {
        return;
    }

    for (size_t i = 0u; i < len; i++) {
        uint8_t byte = data[i];

        if (gateway_ble_rx_overflow) {
            if (byte == SERIAL_FRAME_DELIMITER) {
                gateway_ble_rx_overflow = false;
                gateway_ble_rx_len = 0u;
                HIGH_DEBUG_COUNTER_INC(gateway_ble_rx_drops);
                LOG_WRN("gateway BLE RX frame dropped after overflow");
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
            gateway_ble_queue_frame();
        }
    }
}

static void gateway_ble_rx_work_handler(struct k_work *work)
{
    struct gateway_ble_frame_pending pending;

    ARG_UNUSED(work);

    if (!gateway_ble_transport_enabled()) {
        return;
    }

    while (k_msgq_get(&gateway_ble_rx_msgq, &pending, K_NO_WAIT) == 0) {
        gateway_handle_ble_frame(pending.frame, pending.len);
    }
}

void gateway_ble_get_status(struct gateway_ble_status *status)
{
    if (status == NULL) {
        return;
    }

    status->connected = gateway_ble_conn != NULL;
    status->packet_notify_enabled = gateway_ble_packet_notify_enabled;
}
#else
static bool gateway_ble_stream_ready(void)
{
    return false;
}

static void gateway_ble_schedule_stream_drain(void)
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
    ret = gateway_command_observability_prepare(
        &gateway_command_observability_state, event, terminal);
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
            gateway_command_observability_note_enqueue(
                &gateway_command_observability_state, event->event_seq, ret);
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
    ret = gateway_command_observability_prepare(
        &gateway_command_observability_state, queued, false);
    if (ret < 0) {
        return ret;
    }
    gateway_command_observability_note_enqueue(
        &gateway_command_observability_state, queued->event_seq, -EAGAIN);
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

    LOG_INF("gateway BLE range test active: mode=%s no UWB, mesh, DWM3000, ADC, or buttons initialized",
            IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER) ? "scanner" : "advertiser");
    for (;;) {
        LOG_INF("gateway BLE range test heartbeat: mode=%s",
                IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER) ? "scanner" : "advertiser");
        k_sleep(K_SECONDS(5));
    }
}
#endif
