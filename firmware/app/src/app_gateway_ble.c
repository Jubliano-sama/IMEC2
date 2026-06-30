#include "app_gateway_ble.h"

#include "app_anchor.h"
#include "app_config.h"
#include "app_high_debug.h"
#include "app_state.h"
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

uint16_t gateway_next_command_seq(void)
{
    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    return gateway_command_seq;
}

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

void gateway_note_command_result(const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
}

void gateway_command_result_tracking_init(void)
{
}
#else
static struct gateway_command_pending gateway_command_pending_state;
static struct k_work_delayable gateway_command_result_timeout_work;

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id);
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

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
                                 size_t payload_len)
{
    struct proto_packet command;
    enum command_id pending_command_id;
    enum command_id result_command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !gateway_command_pending_matches_result(&gateway_command_pending_state, packet)) {
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

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    int ret;

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
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
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
static struct k_work_delayable gateway_ble_quiet_flush_work;
static struct bt_conn *gateway_ble_conn;
static bool gateway_ble_advertising_active;
static bool gateway_ble_packet_notify_enabled;
static bool gateway_ble_log_notify_enabled;
static uint8_t gateway_ble_uwb_quiet_depth;
static bool gateway_ble_quiet_stopped_advertising;
static uint8_t gateway_ble_quiet_log_buf[GATEWAY_BLE_QUIET_LOG_BUFFER_SIZE];
static size_t gateway_ble_quiet_log_len;
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
    size_t room;
    size_t copy_len;

    if (data == NULL || len == 0u) {
        return;
    }

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
    LOG_INF("gateway BLE resumed after UWB: reason=%s",
            reason == NULL ? "unknown" : reason);
}

static void gateway_ble_packet_ccc_changed(const struct bt_gatt_attr *attr,
                                           uint16_t value)
{
    ARG_UNUSED(attr);

    gateway_ble_packet_notify_enabled = value == BT_GATT_CCC_NOTIFY;
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

    if (gateway_ble_quiet_log_len > 0u) {
        (void)gateway_ble_notify_attr(&gateway_ble_svc.attrs[GATEWAY_BLE_LOG_TX_ATTR_INDEX],
                                      gateway_ble_quiet_log_buf,
                                      gateway_ble_quiet_log_len,
                                      gateway_ble_log_notify_enabled);
    }
    gateway_ble_quiet_log_len = 0u;

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
    k_work_init_delayable(&gateway_ble_quiet_flush_work,
                          gateway_ble_quiet_flush_work_handler);
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
    uint32_t heartbeat = 0u;
    int ret;

    printk("gateway BLE connectivity test booting\n");
    ret = gateway_ble_init();
    if (ret < 0) {
        printk("gateway BLE connectivity test init failed: %d\n", ret);
        LOG_ERR("gateway BLE connectivity test init failed: %d", ret);
        for (;;) {
            k_sleep(K_SECONDS(30));
        }
    }

    LOG_INF("gateway BLE connectivity test active; no UWB, mesh, DWM3000, ADC, LEDs, or buttons initialized");
    for (;;) {
        struct gateway_ble_status status = {0};
        char line[128];
        int len;

        gateway_ble_get_status(&status);
        LOG_INF("gateway BLE connectivity test heartbeat: seq=%u connected=%u packet_notify=%u log_notify=%u",
                heartbeat,
                status.connected ? 1u : 0u,
                status.packet_notify_enabled ? 1u : 0u,
                status.log_notify_enabled ? 1u : 0u);
        len = snprintk(line,
                       sizeof(line),
                       "BLE_GATEWAY_TEST heartbeat=%u connected=%u packet_notify=%u log_notify=%u\n",
                       heartbeat,
                       status.connected ? 1u : 0u,
                       status.packet_notify_enabled ? 1u : 0u,
                       status.log_notify_enabled ? 1u : 0u);
        if (len > 0) {
            ret = gateway_ble_send_log_bytes((const uint8_t *)line,
                                             (size_t)MIN(len, (int)sizeof(line) - 1));
            if (ret < 0 && ret != -ENOTCONN && ret != -EACCES) {
                LOG_WRN("gateway BLE connectivity test log notify failed: %d", ret);
            }
        }
        heartbeat++;
        k_sleep(K_SECONDS(5));
    }
}
#endif
