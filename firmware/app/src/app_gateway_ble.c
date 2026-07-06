#include "app_gateway_ble.h"

#include "app_anchor.h"
#include "app_board.h"
#include "app_config.h"
#include "app_gateway_collection_eack.h"
#include "app_gateway_eack_policy.h"
#include "app_gateway_ble_stream.h"
#include "app_high_debug.h"
#include "app_mesh_report.h"
#include "app_mesh_persistence.h"
#include "app_state.h"
#include "gateway_membership.h"
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
#if defined(CONFIG_IMEC_GATEWAY_BLE_LOG_BACKEND)
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>
#endif
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_gateway_ble, LOG_LEVEL_DBG);

static uint16_t gateway_command_seq;
static struct gateway_ble_stream_state gateway_ble_stream_state;
static struct k_spinlock gateway_ble_stream_lock;

static bool gateway_ble_stream_ready(void);
static void gateway_ble_schedule_stream_drain(void);

uint16_t gateway_next_command_seq(void)
{
    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    return gateway_command_seq;
}

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

void gateway_note_command_result(const struct proto_packet *packet,
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
}

void gateway_note_command_result_bundle(const struct proto_packet *packet,
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
}

void gateway_command_result_tracking_init(void)
{
}
#else
static struct gateway_command_pending gateway_command_pending_state;
static struct gateway_collection_state gateway_collection_state;
static struct gateway_membership_roster gateway_membership_roster_state;
static uint64_t gateway_collection_expected_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];
static uint16_t gateway_collection_expected_node_id_count;
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_work_delayable gateway_collection_eack_work;

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id);
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

static bool gateway_collection_tracking_active(void);

int gateway_ble_stream_packet(const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t received_at_ms)
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
    ret = gateway_ble_stream_enqueue_packet(&gateway_ble_stream_state,
                                            packet,
                                            payload,
                                            payload_len,
                                            received_at_ms,
                                            k_uptime_get_32(),
                                            gateway_ble_stream_ready());
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret < 0 ? ret : 0;
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

static void gateway_persist_collection_state(const char *reason)
{
    int ret;

    if (!gateway_collection_tracking_active()) {
        return;
    }

    ret = app_mesh_persistence_save_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        LOG_WRN("gateway collection snapshot save failed: ret=%d reason=%s",
                ret,
                reason == NULL ? "" : reason);
    }
}

static bool gateway_collection_tracking_active(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY &&
           gateway_collection_state.gateway_id == DEVICE_ID &&
           gateway_collection_state.command_seq != 0u &&
           gateway_collection_state.collection_epoch_id != 0u;
}

static void gateway_schedule_collection_eack_round(void)
{
    if (!gateway_collection_tracking_active() ||
        !gateway_collection_state.collection_open) {
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        return;
    }

    (void)k_work_reschedule(&gateway_collection_eack_work,
                            K_MSEC(gateway_collection_state.next_retry_spread_ms));
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

static int gateway_eack_send_c5_flood(const struct mesh_outbound *out, void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_send_c5_flood(out,
                              C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD,
                              "collection-eack-c5");
}

static void gateway_eack_note_tx_sent(const struct mesh_outbound *out, void *ctx)
{
    ARG_UNUSED(ctx);

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
                                        const struct mesh_event_plan *current_channel9_plan)
{
    struct mesh_outbound eack = {0};
    struct app_gateway_collection_eack_input input = {
        .collection = &gateway_collection_state,
        .expected_node_ids = gateway_collection_expected_node_ids,
        .expected_node_id_count = gateway_collection_expected_node_id_count,
        .previous_hop_id = previous_hop_id,
        .received_radio_channel = received_radio_channel,
        .current_channel9_plan = current_channel9_plan,
        .self_id = DEVICE_ID,
    };
    const struct app_gateway_eack_policy_ops ops = {
        .plan_channel9 = gateway_eack_plan_channel9,
        .prepare_channel9 = gateway_eack_prepare_channel9,
        .send_channel9 = gateway_eack_send_channel9,
        .send_c5_flood = gateway_eack_send_c5_flood,
        .note_tx_sent = gateway_eack_note_tx_sent,
        .note_channel9_tx = gateway_eack_note_channel9_tx,
        .now_ms = gateway_eack_now_ms,
    };
    struct app_gateway_collection_eack_result result;
    int ret;

    if (!gateway_collection_tracking_active()) {
        return -ENOENT;
    }

    ret = app_gateway_collection_eack_send(
        &eack,
        &input,
        &ops,
        &result);
    if (ret < 0) {
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

static void gateway_collection_eack_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (!gateway_collection_tracking_active()) {
        return;
    }

    ret = gateway_send_collection_eack("collection-eack-round", 0u, 0u, NULL);
    if (ret < 0) {
        (void)k_work_reschedule(&gateway_collection_eack_work,
                                K_MSEC(RELAY_BUSY_RETRY_MIN_MS));
        return;
    }
    if (!gateway_collection_state.collection_open) {
        return;
    }

    ret = gateway_collection_advance_retry_round(&gateway_collection_state);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection retry round advance failed: ret=%d", ret);
        return;
    }
    gateway_persist_collection_state("collection-eack-round");
    gateway_schedule_collection_eack_round();
}

static void gateway_note_collection_result(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id,
                                           uint8_t received_radio_channel,
                                           const struct mesh_event_plan *current_channel9_plan)
{
    bool duplicate = false;
    int ret;

    if (!gateway_collection_tracking_active() ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT) {
        return;
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
        return;
    }

    LOG_INF("gateway collection result recorded: src=0x%016llx command_seq=%u received=%u expected=%u duplicate=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            duplicate ? 1u : 0u);
    if (!duplicate) {
        gateway_persist_collection_state("collection-result");
        (void)gateway_send_collection_eack("collection-eack-result",
                                           previous_hop_id,
                                           received_radio_channel,
                                           current_channel9_plan);
        gateway_schedule_collection_eack_round();
    }
}

static void gateway_note_collection_bundle(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id,
                                           uint8_t received_radio_channel,
                                           const struct mesh_event_plan *current_channel9_plan)
{
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;
    int ret;

    if (!gateway_collection_tracking_active() ||
        packet == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE) {
        return;
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
        return;
    }

    LOG_INF("gateway collection bundle recorded: src=0x%016llx command_seq=%u accepted=%u duplicate=%u received=%u expected=%u",
            (unsigned long long)packet->src_id,
            gateway_collection_state.command_seq,
            (unsigned int)accepted_count,
            (unsigned int)duplicate_count,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count);
    if (accepted_count != 0u) {
        gateway_persist_collection_state("collection-bundle");
        (void)gateway_send_collection_eack("collection-eack-bundle",
                                           previous_hop_id,
                                           received_radio_channel,
                                           current_channel9_plan);
        gateway_schedule_collection_eack_round();
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

static int gateway_command_find_u8_tlv(const uint8_t *payload,
                                       size_t payload_len,
                                       uint8_t type,
                                       uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = tlv_value[0];
    return PROTO_OK;
}

static int gateway_command_find_u16_tlv(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int gateway_command_extract_result_tlvs(const uint8_t *payload,
                                               size_t payload_len,
                                               enum command_id *command_id,
                                               enum command_status *status,
                                               uint8_t *reason)
{
    uint16_t command_value = 0u;
    uint16_t status_value = 0u;
    uint8_t reason_value = 0u;
    int ret;

    if (command_id == NULL || status == NULL || reason == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_command_find_u16_tlv(payload, payload_len, TLV_COMMAND_ID, &command_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = gateway_command_find_u16_tlv(payload, payload_len, TLV_COMMAND_STATUS, &status_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = gateway_command_find_u8_tlv(payload, payload_len, TLV_REASON, &reason_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (status_value > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }

    *command_id = (enum command_id)command_value;
    *status = (enum command_status)status_value;
    *reason = reason_value;
    return PROTO_OK;
}

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

    ret = gateway_command_build_failure_result(command,
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
    size_t resolved_node_count = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (gateway_command_pending_state.active || gateway_collection_state.collection_open) {
        return -EBUSY;
    }

    ret = gateway_command_resolve_collection_roster(options,
                                                    &gateway_membership_roster_state,
                                                    gateway_collection_expected_node_ids,
                                                    ARRAY_SIZE(gateway_collection_expected_node_ids),
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
    gateway_collection_expected_node_id_count = (uint16_t)resolved_node_count;

    LOG_INF("gateway collection tracking started: command_seq=%u collection=%u epoch=%u expected=%u roster_source=%u",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.gateway_epoch,
            gateway_collection_state.expected_count,
            (unsigned int)roster_source);
    gateway_persist_collection_state("collection-start");
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

    ret = gateway_membership_set_roster_preserve_order(&gateway_membership_roster_state,
                                                       membership_epoch,
                                                       node_ids,
                                                       node_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    ret = app_mesh_persistence_save_gateway_membership(&gateway_membership_roster_state);
    if (ret < 0) {
        LOG_WRN("gateway membership snapshot save failed: ret=%d", ret);
    }
    return 0;
}

void gateway_clear_registered_membership_roster(void)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    gateway_membership_clear(&gateway_membership_roster_state);
    app_mesh_persistence_clear_gateway_membership();
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL ||
        !gateway_collection_tracking_active()) {
        return;
    }
    if (gateway_collection_state.command_seq != options->command_seq ||
        gateway_collection_state.collection_epoch_id != options->collection_epoch_id) {
        return;
    }

    gateway_collection_clear(&gateway_collection_state);
    gateway_collection_expected_node_id_count = 0u;
    app_mesh_persistence_clear_gateway_collection();
    (void)k_work_cancel_delayable(&gateway_collection_eack_work);
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
    mesh_relay_note_delivery_failure(&mesh_runtime, command.dst_id);
    gateway_command_timeout_side_effects(&command, command_id);
    gateway_emit_host_command_result(&command, command_id, COMMAND_TIMEOUT, 0u);
}

void gateway_note_command_result(const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 uint64_t previous_hop_id,
                                 uint8_t received_radio_channel,
                                 const struct mesh_event_plan *current_channel9_plan)
{
    struct proto_packet command;
    enum command_id pending_command_id;
    enum command_id result_command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    gateway_note_collection_result(packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   received_radio_channel,
                                   current_channel9_plan);

    if (!gateway_command_pending_matches_result(&gateway_command_pending_state, packet)) {
        return;
    }

    command = gateway_command_pending_state.command;
    pending_command_id = gateway_command_pending_state.command_id;
    gateway_command_pending_clear(&gateway_command_pending_state);
    (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
    ret = gateway_command_extract_result_tlvs(payload,
                                              payload_len,
                                              &result_command_id,
                                              &status,
                                              &reason);
    if (ret != PROTO_OK || result_command_id != pending_command_id) {
        LOG_WRN("gateway command result payload mismatch: pending=0x%04x result=0x%04x ret=%d",
                (unsigned int)pending_command_id,
                (unsigned int)result_command_id,
                ret);
        status = COMMAND_INTERNAL_ERROR;
        reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
    }

    LOG_INF("gateway command result received: src=0x%016llx session=%u seq=%u",
            (unsigned long long)packet->src_id,
            packet->session_id,
            packet->seq);
    gateway_command_result_side_effects(&command, pending_command_id, status, reason);
}

void gateway_note_command_result_bundle(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint64_t previous_hop_id,
                                        uint8_t received_radio_channel,
                                        const struct mesh_event_plan *current_channel9_plan)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    gateway_note_collection_bundle(packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   received_radio_channel,
                                   current_channel9_plan);
}

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    int ret;

    if (gateway_collection_state.collection_open) {
        return -EBUSY;
    }

    ret = gateway_command_pending_start(&gateway_command_pending_state,
                                        command,
                                        command_id,
                                        k_uptime_get_32(),
                                        GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_MALFORMED ? -EBUSY : mesh_errno_from_proto(ret);
    }

    (void)k_work_reschedule(&gateway_command_result_timeout_work,
                            K_MSEC(GATEWAY_COMMAND_RESULT_TIMEOUT_MS));
    return 0;
}

void gateway_command_result_tracking_init(void)
{
    int ret;

    gateway_collection_clear(&gateway_collection_state);
    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_collection_expected_node_id_count = 0u;
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
    k_work_init_delayable(&gateway_collection_eack_work,
                          gateway_collection_eack_work_handler);
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = app_mesh_persistence_restore_gateway_membership(&gateway_membership_roster_state);
    if (ret < 0) {
        gateway_membership_clear(&gateway_membership_roster_state);
        LOG_WRN("gateway membership snapshot restore unavailable: ret=%d", ret);
    }

    ret = app_mesh_persistence_restore_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        gateway_collection_clear(&gateway_collection_state);
        LOG_WRN("gateway collection snapshot restore unavailable: ret=%d", ret);
        return;
    }
    if (gateway_collection_tracking_active()) {
        LOG_INF("gateway collection tracking restored: command_seq=%u collection=%u received=%u expected=%u open=%u",
                gateway_collection_state.command_seq,
                gateway_collection_state.collection_epoch_id,
                gateway_collection_state.received_count,
                gateway_collection_state.expected_count,
                gateway_collection_state.collection_open ? 1u : 0u);
        gateway_schedule_collection_eack_round();
    }
}
#endif

#if defined(CONFIG_BT) && defined(CONFIG_IMEC_GATEWAY_BLE)
#define BT_UUID_IMEC_GATEWAY_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000001ULL)
#define BT_UUID_IMEC_GATEWAY_PACKET_TX_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000002ULL)
#define BT_UUID_IMEC_GATEWAY_PACKET_RX_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000003ULL)
#define BT_UUID_IMEC_GATEWAY_LOG_TX_VAL \
    BT_UUID_128_ENCODE(0x494d4543, 0x0001, 0x4757, 0x8000, 0x000000000004ULL)

#define BT_UUID_IMEC_GATEWAY_SERVICE BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_SERVICE_VAL)
#define BT_UUID_IMEC_GATEWAY_PACKET_TX BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_PACKET_TX_VAL)
#define BT_UUID_IMEC_GATEWAY_PACKET_RX BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_PACKET_RX_VAL)
#define BT_UUID_IMEC_GATEWAY_LOG_TX BT_UUID_DECLARE_128(BT_UUID_IMEC_GATEWAY_LOG_TX_VAL)

#define GATEWAY_BLE_PACKET_TX_ATTR_INDEX 2u
#define GATEWAY_BLE_LOG_TX_ATTR_INDEX 7u
#define GATEWAY_BLE_DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define GATEWAY_BLE_DEVICE_NAME_LEN (sizeof(GATEWAY_BLE_DEVICE_NAME) - 1u)

struct gateway_ble_frame_pending {
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    uint16_t len;
};

K_MSGQ_DEFINE(gateway_ble_rx_msgq,
              sizeof(struct gateway_ble_frame_pending),
              GATEWAY_BLE_RX_FRAME_QUEUE_DEPTH,
              4);

static struct k_work gateway_ble_rx_work;
static struct k_work gateway_ble_stream_work;
static struct k_work_delayable gateway_ble_quiet_flush_work;
static struct bt_conn *gateway_ble_conn;
static bool gateway_ble_advertising_active;
static bool gateway_ble_packet_notify_enabled;
static bool gateway_ble_log_notify_enabled;
static uint8_t gateway_ble_uwb_quiet_depth;
static bool gateway_ble_quiet_stopped_advertising;
#if GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE > 0u
static uint8_t gateway_ble_quiet_log_buf[GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE];
static size_t gateway_ble_quiet_log_len;
#endif
static uint32_t gateway_ble_quiet_log_dropped;
static uint8_t gateway_ble_rx_frame[SERIAL_FRAME_MAX_LEN];
static size_t gateway_ble_rx_len;
static bool gateway_ble_rx_overflow;

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
static void gateway_ble_flush_quiet_logs(void);
static void gateway_ble_quiet_flush_work_handler(struct k_work *work);
static void gateway_ble_rx_work_handler(struct k_work *work);
static void gateway_ble_stream_work_handler(struct k_work *work);

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
        (void)k_work_submit(&gateway_ble_stream_work);
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

static void gateway_ble_buffer_quiet_log_bytes(const uint8_t *data, size_t len)
{
#if GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE > 0u
    size_t room;
    size_t copy_len;
#endif

    if (data == NULL || len == 0u) {
        return;
    }

#if GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE > 0u
    room = sizeof(gateway_ble_quiet_log_buf) - gateway_ble_quiet_log_len;
    copy_len = MIN(room, len);
    if (copy_len > 0u) {
        memcpy(&gateway_ble_quiet_log_buf[gateway_ble_quiet_log_len],
               data,
               copy_len);
        gateway_ble_quiet_log_len += copy_len;
    }
    if (copy_len < len) {
        gateway_ble_quiet_log_dropped += (uint32_t)MIN(len - copy_len,
                                                       (size_t)UINT32_MAX);
    }
#else
    gateway_ble_quiet_log_dropped += (uint32_t)MIN(len, (size_t)UINT32_MAX);
#endif
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
        (void)gateway_ble_start_advertising();
    }
    gateway_ble_quiet_stopped_advertising = false;
    (void)k_work_reschedule(&gateway_ble_quiet_flush_work,
                            K_MSEC(GATEWAY_BLE_QUIET_LOG_FLUSH_DELAY_MS));
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
        gateway_ble_schedule_stream_drain();
    }
}

static void gateway_ble_log_ccc_changed(const struct bt_gatt_attr *attr,
                                        uint16_t value)
{
    ARG_UNUSED(attr);

    gateway_ble_log_notify_enabled = value == BT_GATT_CCC_NOTIFY;
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
    BT_GATT_CHARACTERISTIC(BT_UUID_IMEC_GATEWAY_LOG_TX,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           NULL, NULL, NULL),
    BT_GATT_CCC(gateway_ble_log_ccc_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static uint16_t gateway_ble_notify_chunk_len(void)
{
    uint16_t mtu;

    if (gateway_ble_conn == NULL) {
        return GATEWAY_BLE_DEFAULT_NOTIFY_CHUNK;
    }

    mtu = bt_gatt_get_mtu(gateway_ble_conn);
    return mtu > 3u ? (uint16_t)(mtu - 3u) : GATEWAY_BLE_DEFAULT_NOTIFY_CHUNK;
}

static int gateway_ble_notify_attr(const struct bt_gatt_attr *attr,
                                   const uint8_t *data,
                                   size_t len,
                                   bool notify_enabled)
{
    size_t offset = 0u;
    uint16_t chunk_cap;
    int ret;

    if (gateway_ble_conn == NULL) {
        return -ENOTCONN;
    }
    if (!notify_enabled) {
        return -EACCES;
    }
    if (data == NULL && len != 0u) {
        return -EINVAL;
    }

    chunk_cap = gateway_ble_notify_chunk_len();
    if (chunk_cap == 0u) {
        return -EMSGSIZE;
    }

    while (offset < len) {
        uint16_t chunk_len = (uint16_t)MIN(len - offset, (size_t)chunk_cap);

        ret = bt_gatt_notify(gateway_ble_conn, attr, &data[offset], chunk_len);
        if (ret < 0) {
            return ret;
        }
        offset += chunk_len;
    }

    return 0;
}

static void gateway_ble_flush_quiet_logs(void)
{
    uint8_t dropped_line[64];
    int dropped_len;

#if GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE > 0u
    if (gateway_ble_quiet_log_len > 0u) {
        (void)gateway_ble_notify_attr(&gateway_ble_svc.attrs[GATEWAY_BLE_LOG_TX_ATTR_INDEX],
                                      gateway_ble_quiet_log_buf,
                                      gateway_ble_quiet_log_len,
                                      gateway_ble_log_notify_enabled);
    }
    gateway_ble_quiet_log_len = 0u;
#endif

    if (gateway_ble_quiet_log_dropped == 0u) {
        return;
    }

    dropped_len = snprintk(dropped_line,
                           sizeof(dropped_line),
                           "\n[ble-log-quiet-drop bytes=%u]\n",
                           gateway_ble_quiet_log_dropped);
    gateway_ble_quiet_log_dropped = 0u;
    if (dropped_len <= 0) {
        return;
    }
    (void)gateway_ble_notify_attr(&gateway_ble_svc.attrs[GATEWAY_BLE_LOG_TX_ATTR_INDEX],
                                  dropped_line,
                                  (size_t)MIN(dropped_len, (int)sizeof(dropped_line)),
                                  gateway_ble_log_notify_enabled);
}

static void gateway_ble_quiet_flush_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (gateway_ble_uwb_quiet_active()) {
        return;
    }
    gateway_ble_flush_quiet_logs();
    gateway_ble_schedule_stream_drain();
}

int gateway_ble_send_packet_frame(const uint8_t *frame, size_t frame_len)
{
    if (gateway_ble_uwb_quiet_active()) {
        return -EAGAIN;
    }

    return gateway_ble_notify_attr(&gateway_ble_svc.attrs[GATEWAY_BLE_PACKET_TX_ATTR_INDEX],
                                   frame,
                                   frame_len,
                                   gateway_ble_packet_notify_enabled);
}

int gateway_ble_send_log_bytes(const uint8_t *data, size_t len)
{
    if (gateway_ble_uwb_quiet_active()) {
        gateway_ble_buffer_quiet_log_bytes(data, len);
        return 0;
    }

    return gateway_ble_notify_attr(&gateway_ble_svc.attrs[GATEWAY_BLE_LOG_TX_ATTR_INDEX],
                                   data,
                                   len,
                                   gateway_ble_log_notify_enabled);
}

static void gateway_ble_stream_work_handler(struct k_work *work)
{
    uint8_t record[GATEWAY_BLE_STREAM_RECORD_MAX_LEN];
    size_t record_len = 0u;
    int ret;

    ARG_UNUSED(work);

    for (;;) {
        bool have_record = false;
        const uint8_t *queued = NULL;
        k_spinlock_key_t key = k_spin_lock(&gateway_ble_stream_lock);

        if (gateway_ble_stream_ready() &&
            gateway_ble_stream_peek(&gateway_ble_stream_state,
                                    &queued,
                                    &record_len) == 0 &&
            record_len <= sizeof(record)) {
            memcpy(record, queued, record_len);
            have_record = true;
        }
        k_spin_unlock(&gateway_ble_stream_lock, key);

        if (!have_record) {
            return;
        }

        ret = gateway_ble_send_packet_frame(record, record_len);
        if (ret < 0) {
            return;
        }

        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                     k_uptime_get_32());
        k_spin_unlock(&gateway_ble_stream_lock, key);
    }
}

#if defined(CONFIG_IMEC_GATEWAY_BLE_LOG_BACKEND)
static uint8_t gateway_ble_log_output_buf[128];

static int gateway_ble_log_backend_out(uint8_t *buf, size_t len, void *ctx)
{
    ARG_UNUSED(ctx);

    (void)gateway_ble_send_log_bytes(buf, len);
    return (int)len;
}

LOG_OUTPUT_DEFINE(gateway_ble_log_output,
                  gateway_ble_log_backend_out,
                  gateway_ble_log_output_buf,
                  sizeof(gateway_ble_log_output_buf));

static void gateway_ble_log_backend_process(const struct log_backend *const backend,
                                            union log_msg_generic *msg)
{
    uint32_t flags = log_backend_std_get_flags();
    log_format_func_t log_output_func = log_format_func_t_get(LOG_OUTPUT_TEXT);

    ARG_UNUSED(backend);

    log_output_func(&gateway_ble_log_output, &msg->log, flags);
}

static void gateway_ble_log_backend_dropped(const struct log_backend *const backend,
                                            uint32_t cnt)
{
    ARG_UNUSED(backend);

    log_backend_std_dropped(&gateway_ble_log_output, cnt);
}

static void gateway_ble_log_backend_panic(const struct log_backend *const backend)
{
    ARG_UNUSED(backend);

    log_backend_std_panic(&gateway_ble_log_output);
}

static const struct log_backend_api gateway_ble_log_backend_api = {
    .process = gateway_ble_log_backend_process,
    .dropped = gateway_ble_log_backend_dropped,
    .panic = gateway_ble_log_backend_panic,
};

LOG_BACKEND_DEFINE(log_backend_gateway_ble, gateway_ble_log_backend_api, true);
#endif

static void gateway_ble_connected(struct bt_conn *conn, uint8_t err)
{
    if (!gateway_ble_transport_enabled()) {
        return;
    }
    if (err != 0u) {
        LOG_WRN("gateway BLE connection failed: err=0x%02x", err);
        (void)gateway_ble_start_advertising();
        return;
    }
    if (gateway_ble_conn != NULL) {
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    (void)gateway_ble_stop_advertising("connected");
    gateway_ble_conn = bt_conn_ref(conn);
    gateway_ble_packet_notify_enabled = false;
    gateway_ble_log_notify_enabled = false;
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    HIGH_DEBUG_COUNTER_INC(gateway_ble_connects);
    LOG_INF("gateway BLE PC link connected");
}

static void gateway_ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (!gateway_ble_transport_enabled()) {
        return;
    }
    if (gateway_ble_conn != conn) {
        return;
    }

    bt_conn_unref(gateway_ble_conn);
    gateway_ble_conn = NULL;
    gateway_ble_packet_notify_enabled = false;
    gateway_ble_log_notify_enabled = false;
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;
    HIGH_DEBUG_COUNTER_INC(gateway_ble_disconnects);
    LOG_INF("gateway BLE PC link disconnected: reason=0x%02x", reason);
    (void)gateway_ble_start_advertising();
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
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t addr_count = ARRAY_SIZE(addrs);

    if (!gateway_ble_transport_enabled()) {
        return 0;
    }

    k_work_init(&gateway_ble_rx_work, gateway_ble_rx_work_handler);
    k_work_init(&gateway_ble_stream_work, gateway_ble_stream_work_handler);
    k_work_init_delayable(&gateway_ble_quiet_flush_work,
                          gateway_ble_quiet_flush_work_handler);
    gateway_ble_stream_init(&gateway_ble_stream_state);
    gateway_ble_rx_len = 0u;
    gateway_ble_rx_overflow = false;

    ret = bt_enable(NULL);
    LOG_INF("gateway BLE bt_enable completed: ret=%d", ret);
    if (ret != 0 && ret != -EALREADY) {
        LOG_ERR("gateway BLE init failed: %d", ret);
        return ret;
    }
    bt_id_get(addrs, &addr_count);
    for (size_t i = 0u; i < addr_count; i++) {
        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(&addrs[i], addr, sizeof(addr));
        LOG_INF("gateway BLE identity: index=%u addr=%s",
                (unsigned int)i,
                addr);
    }

    ret = gateway_ble_start_advertising();
    if (ret < 0) {
        LOG_ERR("gateway BLE advertising failed: %d", ret);
        return ret;
    }

    LOG_INF("gateway BLE PC link advertising as %s", GATEWAY_BLE_DEVICE_NAME);
    high_debug_log_event("BLE_GATEWAY_READY",
                         "device_name=%s packet_notify=0 log_notify=0",
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
    status->log_notify_enabled = gateway_ble_log_notify_enabled;
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

int gateway_ble_send_log_bytes(const uint8_t *data, size_t len)
{
    ARG_UNUSED(data);
    ARG_UNUSED(len);

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
    status->log_notify_enabled = false;
}
#endif

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
