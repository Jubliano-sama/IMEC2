#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "discovery.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_ble.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "serial_frame.h"
#include "status.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#endif

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(uwb_ble_app, LOG_LEVEL_DBG);

#if !defined(CONFIG_BT_EXT_ADV) || !defined(CONFIG_BT_LL_SOFTDEVICE)
#error "BLE transport requires extended advertising with the SoftDevice Controller."
#endif

#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

#if DEVICE_ROLE != ROLE_CLICKER
#if !defined(CONFIG_BT_CENTRAL) || !defined(CONFIG_BT_PERIPHERAL) || !defined(CONFIG_BT_GATT_CLIENT)
#error "Anchor/gateway mesh data transport requires BLE central, peripheral, and GATT client support."
#endif
#endif

#ifndef DEVICE_ID
#define DEVICE_ID 0x1111222233334444ull
#endif

#ifndef GATEWAY_ID
#define GATEWAY_ID 0x9999888877776666ull
#endif

#define CLICK_BUTTON_NODE DT_ALIAS(click_button)
#define STATUS0_RED_NODE DT_ALIAS(status0_red)
#define STATUS0_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS0_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS1_RED_NODE DT_ALIAS(status1_red)
#define STATUS1_GREEN_NODE DT_ALIAS(status1_green)
#define STATUS1_BLUE_NODE DT_ALIAS(status1_blue)
#define USB_CONSOLE_NODE DT_CHOSEN(zephyr_console)

#if DT_NODE_HAS_STATUS(CLICK_BUTTON_NODE, okay)
static const struct gpio_dt_spec click_button = GPIO_DT_SPEC_GET(CLICK_BUTTON_NODE, gpios);
static struct gpio_callback click_button_cb;
static struct k_work click_button_work;
static struct k_work_delayable self_test_arm_timeout_work;
#define HAS_CLICK_BUTTON 1
#else
#define HAS_CLICK_BUTTON 0
#endif

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
static const struct gpio_dt_spec status0_red = GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
static const struct gpio_dt_spec status0_green = GPIO_DT_SPEC_GET(STATUS0_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
static const struct gpio_dt_spec status0_blue = GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
static const struct gpio_dt_spec status1_red = GPIO_DT_SPEC_GET(STATUS1_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
static const struct gpio_dt_spec status1_green = GPIO_DT_SPEC_GET(STATUS1_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
static const struct gpio_dt_spec status1_blue = GPIO_DT_SPEC_GET(STATUS1_BLUE_NODE, gpios);
#endif

#if DT_NODE_HAS_STATUS(USB_CONSOLE_NODE, okay)
static const struct device *serial_console = DEVICE_DT_GET(USB_CONSOLE_NODE);
#define HAS_SERIAL_CONSOLE 1
#else
#define HAS_SERIAL_CONSOLE 0
#endif

static struct button_fsm button_fsm;
static uint32_t next_event_seq;
static bool ble_ready;
static bool anchor_scan_active;
static bool gateway_mesh_scan_active;
static bool clicker_ready_scan_active;
static bool ble_adv_active;
static bool ble_ext_adv_connectable;
static bool uwb_rf_active;
static struct k_work anchor_ble_work;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable report_tx_work;
static struct k_work_delayable gateway_serial_rx_work;
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_spinlock anchor_ble_lock;
static struct k_spinlock clicker_ready_lock;
static bool anchor_uwb_busy;
static bool anchor_admission_open;
static bool anchor_service_active;
static struct mesh_relay mesh_runtime;
static struct bt_le_ext_adv *ble_ext_adv;

#define MAX_READY_ANCHORS 8u
#define MAX_SUCCESSFUL_ANCHORS 16u
#define WAKE_ADV_MS 330u
#define WAKE_ADV_UPDATE_MS 20u
#define READY_SCAN_MS 200u
#define ANCHOR_READY_ADV_MS 180u
#define ANCHOR_UWB_WAIT_MS 500u
#define NO_ANCHOR_RETRY_DELAY_MS 700u
#define MAX_WAKE_ATTEMPTS 6u
#define MIN_UNIQUE_RANGED_ANCHORS 4u
#define CLICK_ANCHOR_RANGE_WINDOW_MS 50u
#define DS_TWR_RETRY_BACKOFF_MIN_MS 4u
#define DS_TWR_RETRY_BACKOFF_MAX_MS 10u
#define UWB_QUIET_TIME_MS 30u
#define MAX_POLITENESS_WAIT_MS 500u
#define CLICK_REPORT_BUILD_GUARD_MS 20u
#define ANCHOR_SERVICE_QUEUE_DEPTH 8u
#define SELF_TEST_UWB_TIMEOUT_MS 150u
#define CLICK_UWB_TIMEOUT_MS 150u
#define CLICK_REPORT_READY_DEADLINE_MS 15000u
#define ANCHOR_SCAN_INTERVAL_300MS 480u
#define ANCHOR_SCAN_WINDOW_30MS 48u
#define BLE_ADV_INTERVAL_20MS 32u
#define MESH_DISCOVERY_ADV_TX_MS 250u
#define MESH_ACK_REPLY_DELAY_MS 0u
#define GATEWAY_SERIAL_POLL_MS 10u
#define GATEWAY_SERIAL_MAX_BYTES_PER_POLL 64u
#define MESH_RX_QUEUE_DEPTH 8
#define REPORT_TX_QUEUE_DEPTH 16
#define REPORT_TX_RETRY_DELAY_MS 1000u
#define MESH_CONN_NEIGHBOR_SLOTS 8u
#define MESH_CONN_TIMEOUT_MS 900u
#define MESH_CONN_MTU_TIMEOUT_MS 500u
#define MESH_CONN_INTERVAL_MIN 80u
#define MESH_CONN_INTERVAL_MAX 120u
#define MESH_CONN_LATENCY 0u
#define MESH_CONN_SUPERVISION_TIMEOUT 400u
#define BLE_SCAN_OPTIONS BT_LE_SCAN_OPT_NONE
#define BLE_ADV_OPTIONS (BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_NO_2M | \
                         BT_LE_ADV_OPT_USE_TX_POWER)
#define BLE_CONN_ADV_OPTIONS (BLE_ADV_OPTIONS | BT_LE_ADV_OPT_CONNECTABLE)
#define BLE_ADV_INTERVAL_MIN BLE_ADV_INTERVAL_20MS
#define BLE_ADV_INTERVAL_MAX BLE_ADV_INTERVAL_20MS

struct ready_anchor {
    struct ble_discovery_ready ready;
    int8_t clicker_rssi;
    int16_t rssi_score;
    uint32_t received_ms;
};

struct anchor_service_slot {
    struct ble_discovery_req request;
    int8_t rssi;
    int64_t ready_scan_start_ms;
    int64_t ready_scan_end_ms;
};

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint64_t previous_hop_id;
    uint8_t link_quality;
};

struct mesh_neighbor {
    uint64_t node_id;
    bt_addr_le_t addr;
    struct bt_conn *conn;
    struct k_sem connected_sem;
    uint32_t last_seen_ms;
    bool valid;
    bool connecting;
    bool connected;
};

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);

static struct anchor_service_slot anchor_service_slots[ANCHOR_SERVICE_QUEUE_DEPTH];
static struct mesh_neighbor mesh_neighbors[MESH_CONN_NEIGHBOR_SLOTS];
static struct k_mutex mesh_neighbor_lock;
static uint8_t anchor_service_count;
static struct ready_anchor clicker_ready_anchors[MAX_READY_ANCHORS];
static uint8_t clicker_ready_count;
static uint8_t clicker_ready_expected_flags;
static uint32_t clicker_ready_expected_event_seq;
static uint8_t clicker_ready_expected_attempt_index;
static int64_t anchor_service_window_start_ms;
static int64_t anchor_service_window_end_ms;
static uint8_t gateway_serial_rx_frame[SERIAL_FRAME_MAX_LEN];
static size_t gateway_serial_rx_len;
static bool gateway_serial_rx_overflow;
static uint16_t gateway_command_seq;
static struct gateway_command_pending gateway_command_pending_state;
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static struct k_sem mesh_mtu_sem;
static struct bt_gatt_exchange_params mesh_mtu_exchange_params;
static uint8_t mesh_mtu_exchange_err;

static int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
static int mesh_request_route(uint64_t target_id, const char *reason);
static void mesh_clear_route_waiting_tx(const struct proto_packet *packet);
static void report_tx_schedule(uint32_t delay_ms);
static int queue_anchor_report(const struct mesh_outbound *outbound);
static bool mesh_queue_from_frame(const uint8_t *frame, size_t frame_len, uint8_t link_quality);
static ssize_t mesh_conn_write(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               const void *buf,
                               uint16_t len,
                               uint16_t offset,
                               uint8_t flags);

static struct bt_uuid_128 mesh_conn_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7a62b590, 0x52f0, 0x45b9, 0xa40a, 0x334156b26800));
static struct bt_uuid_128 mesh_conn_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7a62b590, 0x52f0, 0x45b9, 0xa40a, 0x334156b26801));

BT_GATT_SERVICE_DEFINE(mesh_conn_svc,
    BT_GATT_PRIMARY_SERVICE(&mesh_conn_service_uuid),
    BT_GATT_CHARACTERISTIC(&mesh_conn_rx_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL,
                           mesh_conn_write,
                           NULL),
);

static const struct bt_le_adv_param ble_adv_param =
    BT_LE_ADV_PARAM_INIT(BLE_ADV_OPTIONS,
                         BLE_ADV_INTERVAL_MIN,
                         BLE_ADV_INTERVAL_MAX,
                         NULL);

static const struct bt_le_adv_param ble_conn_adv_param =
    BT_LE_ADV_PARAM_INIT(BLE_CONN_ADV_OPTIONS,
                         BLE_ADV_INTERVAL_MIN,
                         BLE_ADV_INTERVAL_MAX,
                         NULL);

static const struct bt_le_scan_param anchor_low_duty_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BLE_SCAN_OPTIONS,
    .interval = ANCHOR_SCAN_INTERVAL_300MS,
    .window = ANCHOR_SCAN_WINDOW_30MS,
    .timeout = 0u,
};

static const struct bt_le_scan_param anchor_full_duty_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BLE_SCAN_OPTIONS,
    .interval = 16u,
    .window = 16u,
    .timeout = 0u,
};

static const struct bt_le_scan_param gateway_mesh_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BLE_SCAN_OPTIONS,
    .interval = 16u,
    .window = 16u,
    .timeout = 0u,
};

static const struct bt_le_scan_param clicker_ready_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BLE_SCAN_OPTIONS,
    .interval = 16u,
    .window = 16u,
    .timeout = 0u,
};

static const char *role_name(void)
{
    switch (DEVICE_ROLE) {
    case ROLE_CLICKER:
        return "clicker";
    case ROLE_ANCHOR:
        return "anchor";
    case ROLE_GATEWAY:
        return "gateway";
    default:
        return "unknown";
    }
}

static uint16_t mesh_conn_rx_value_handle(void)
{
    return bt_gatt_attr_get_handle(&mesh_conn_svc.attrs[2]);
}

static bool mesh_id_is_unicast(uint64_t node_id)
{
    return node_id != MESH_BROADCAST_ID;
}

static ssize_t mesh_conn_write(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               const void *buf,
                               uint16_t len,
                               uint16_t offset,
                               uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (buf == NULL || len == 0u || len > MESH_BLE_MAX_FRAME_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (!mesh_queue_from_frame(buf, len, 100u)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return len;
}

static struct mesh_neighbor *mesh_neighbor_find_by_id_locked(uint64_t node_id)
{
    for (uint8_t i = 0u; i < MESH_CONN_NEIGHBOR_SLOTS; i++) {
        if (mesh_neighbors[i].valid && mesh_neighbors[i].node_id == node_id) {
            return &mesh_neighbors[i];
        }
    }
    return NULL;
}

static struct mesh_neighbor *mesh_neighbor_find_by_addr_locked(const bt_addr_le_t *addr)
{
    for (uint8_t i = 0u; i < MESH_CONN_NEIGHBOR_SLOTS; i++) {
        if (mesh_neighbors[i].valid && bt_addr_le_cmp(&mesh_neighbors[i].addr, addr) == 0) {
            return &mesh_neighbors[i];
        }
    }
    return NULL;
}

static struct mesh_neighbor *mesh_neighbor_alloc_locked(void)
{
    struct mesh_neighbor *oldest = &mesh_neighbors[0];

    for (uint8_t i = 0u; i < MESH_CONN_NEIGHBOR_SLOTS; i++) {
        if (!mesh_neighbors[i].valid) {
            return &mesh_neighbors[i];
        }
        if (mesh_neighbors[i].last_seen_ms < oldest->last_seen_ms &&
            mesh_neighbors[i].conn == NULL) {
            oldest = &mesh_neighbors[i];
        }
    }
    if (oldest->conn != NULL) {
        return NULL;
    }
    return oldest;
}

static void mesh_note_peer_addr(uint64_t node_id, const bt_addr_le_t *addr)
{
    struct mesh_neighbor *neighbor;

    if (!mesh_id_is_unicast(node_id) || node_id == DEVICE_ID || addr == NULL) {
        return;
    }

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_id_locked(node_id);
    if (neighbor == NULL) {
        neighbor = mesh_neighbor_alloc_locked();
    }
    if (neighbor != NULL) {
        bool same_addr = neighbor->valid && bt_addr_le_cmp(&neighbor->addr, addr) == 0;

        if (!same_addr && neighbor->conn == NULL) {
            bt_addr_le_copy(&neighbor->addr, addr);
        } else if (!neighbor->valid) {
            bt_addr_le_copy(&neighbor->addr, addr);
        }
        neighbor->node_id = node_id;
        neighbor->last_seen_ms = k_uptime_get_32();
        neighbor->valid = true;
    }
    k_mutex_unlock(&mesh_neighbor_lock);
}

static void mesh_mtu_exchange_cb(struct bt_conn *conn,
                                 uint8_t err,
                                 struct bt_gatt_exchange_params *params)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    mesh_mtu_exchange_err = err;
    k_sem_give(&mesh_mtu_sem);
}

static int mesh_exchange_mtu(struct bt_conn *conn)
{
    int ret;

    if (conn == NULL) {
        return -EINVAL;
    }
    if (bt_gatt_get_mtu(conn) >= MESH_BLE_MAX_FRAME_LEN + 3u) {
        return 0;
    }

    k_sem_reset(&mesh_mtu_sem);
    mesh_mtu_exchange_err = 0u;
    mesh_mtu_exchange_params.func = mesh_mtu_exchange_cb;
    ret = bt_gatt_exchange_mtu(conn, &mesh_mtu_exchange_params);
    if (ret == -EALREADY) {
        return 0;
    }
    if (ret < 0) {
        return ret;
    }
    ret = k_sem_take(&mesh_mtu_sem, K_MSEC(MESH_CONN_MTU_TIMEOUT_MS));
    if (ret < 0) {
        return ret;
    }
    return mesh_mtu_exchange_err == 0u ? 0 : -EIO;
}

static void mesh_conn_connected(struct bt_conn *conn, uint8_t err)
{
    const bt_addr_le_t *addr = bt_conn_get_dst(conn);
    struct mesh_neighbor *neighbor;
    struct bt_le_conn_param param = {
        .interval_min = MESH_CONN_INTERVAL_MIN,
        .interval_max = MESH_CONN_INTERVAL_MAX,
        .latency = MESH_CONN_LATENCY,
        .timeout = MESH_CONN_SUPERVISION_TIMEOUT,
    };

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_addr_locked(addr);
    if (neighbor != NULL) {
        neighbor->connecting = false;
        if (err == 0u) {
            if (neighbor->conn == NULL) {
                neighbor->conn = bt_conn_ref(conn);
            }
            neighbor->connected = true;
            neighbor->last_seen_ms = k_uptime_get_32();
        } else {
            if (neighbor->conn != NULL) {
                bt_conn_unref(neighbor->conn);
                neighbor->conn = NULL;
            }
            neighbor->connected = false;
        }
        k_sem_give(&neighbor->connected_sem);
    }
    k_mutex_unlock(&mesh_neighbor_lock);

    if (err != 0u) {
        LOG_WRN("mesh BLE connection failed: err=%u", err);
        return;
    }

    (void)bt_conn_le_param_update(conn, &param);
    LOG_INF("mesh BLE connection established; role scan remains active");
}

static void mesh_conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct mesh_neighbor *neighbor;

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_addr_locked(bt_conn_get_dst(conn));
    if (neighbor != NULL && neighbor->conn == conn) {
        bt_conn_unref(neighbor->conn);
        neighbor->conn = NULL;
        neighbor->connected = false;
        neighbor->connecting = false;
    }
    k_mutex_unlock(&mesh_neighbor_lock);

    LOG_INF("mesh BLE connection disconnected: reason=%u; role scan remains active", reason);
}

BT_CONN_CB_DEFINE(mesh_conn_callbacks) = {
    .connected = mesh_conn_connected,
    .disconnected = mesh_conn_disconnected,
};

static void mesh_neighbors_init(void)
{
    k_mutex_init(&mesh_neighbor_lock);
    k_sem_init(&mesh_mtu_sem, 0u, 1u);
    for (uint8_t i = 0u; i < MESH_CONN_NEIGHBOR_SLOTS; i++) {
        k_sem_init(&mesh_neighbors[i].connected_sem, 0u, 1u);
    }
}

static int mesh_scan_create_params(struct bt_conn_le_create_param *param)
{
    if (param == NULL) {
        return -EINVAL;
    }

    memset(param, 0, sizeof(*param));
    param->options = BT_CONN_LE_OPT_NONE;
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        param->interval = 16u;
        param->window = 16u;
    } else {
        param->interval = ANCHOR_SCAN_INTERVAL_300MS;
        param->window = ANCHOR_SCAN_WINDOW_30MS;
    }
    return 0;
}

static int mesh_neighbor_connection(uint64_t node_id, struct bt_conn **conn_out)
{
    struct bt_conn_le_create_param create_param;
    struct bt_le_conn_param conn_param = {
        .interval_min = MESH_CONN_INTERVAL_MIN,
        .interval_max = MESH_CONN_INTERVAL_MAX,
        .latency = MESH_CONN_LATENCY,
        .timeout = MESH_CONN_SUPERVISION_TIMEOUT,
    };
    struct mesh_neighbor *neighbor;
    bt_addr_le_t addr;
    struct bt_conn *conn = NULL;
    bool connecting;
    int ret;

    if (conn_out == NULL || !mesh_id_is_unicast(node_id) || node_id == DEVICE_ID) {
        return -EINVAL;
    }
    *conn_out = NULL;

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_id_locked(node_id);
    if (neighbor == NULL || !neighbor->valid) {
        k_mutex_unlock(&mesh_neighbor_lock);
        return -EHOSTUNREACH;
    }
    if (neighbor->connected && neighbor->conn != NULL) {
        *conn_out = bt_conn_ref(neighbor->conn);
        k_mutex_unlock(&mesh_neighbor_lock);
        return 0;
    }
    if (neighbor->connecting) {
        connecting = true;
    } else {
        connecting = false;
        neighbor->connecting = true;
        neighbor->connected = false;
        k_sem_reset(&neighbor->connected_sem);
    }
    bt_addr_le_copy(&addr, &neighbor->addr);
    k_mutex_unlock(&mesh_neighbor_lock);

    if (!connecting) {
        ret = mesh_scan_create_params(&create_param);
        if (ret < 0) {
            return ret;
        }
        ret = bt_conn_le_create(&addr, &create_param, &conn_param, &conn);
        if (ret < 0) {
            k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
            neighbor = mesh_neighbor_find_by_id_locked(node_id);
            if (neighbor != NULL) {
                neighbor->connecting = false;
                neighbor->connected = false;
                k_sem_give(&neighbor->connected_sem);
            }
            k_mutex_unlock(&mesh_neighbor_lock);
            return ret;
        }
        k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
        neighbor = mesh_neighbor_find_by_id_locked(node_id);
        if (neighbor != NULL) {
            neighbor->conn = conn;
        } else if (conn != NULL) {
            bt_conn_unref(conn);
        }
        k_mutex_unlock(&mesh_neighbor_lock);
    }

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_id_locked(node_id);
    k_mutex_unlock(&mesh_neighbor_lock);
    if (neighbor == NULL ||
        k_sem_take(&neighbor->connected_sem, K_MSEC(MESH_CONN_TIMEOUT_MS)) < 0) {
        return -ETIMEDOUT;
    }

    k_mutex_lock(&mesh_neighbor_lock, K_FOREVER);
    neighbor = mesh_neighbor_find_by_id_locked(node_id);
    if (neighbor != NULL && neighbor->connected && neighbor->conn != NULL) {
        *conn_out = bt_conn_ref(neighbor->conn);
        ret = 0;
    } else {
        ret = -ENOTCONN;
    }
    k_mutex_unlock(&mesh_neighbor_lock);
    return ret;
}

static int configure_output(const struct gpio_dt_spec *gpio)
{
    if (!gpio_is_ready_dt(gpio)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
}

static void set_output(const struct gpio_dt_spec *gpio, bool enabled)
{
    if (gpio_is_ready_dt(gpio)) {
        (void)gpio_pin_set_dt(gpio, enabled ? 1 : 0);
    }
}

static void status_leds_set(bool red, bool green, bool blue)
{
#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    set_output(&status0_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    set_output(&status0_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    set_output(&status0_blue, blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    set_output(&status1_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    set_output(&status1_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    set_output(&status1_blue, blue);
#endif
}

static void status_apply(const struct status_inputs *inputs)
{
    struct status_indication indication;

    if (status_select(inputs, &indication) != PROTO_OK) {
        return;
    }

    switch (indication.pattern) {
    case STATUS_PATTERN_BLUE_PULSE:
    case STATUS_PATTERN_BLUE_CHASE:
        status_leds_set(false, false, true);
        break;
    case STATUS_PATTERN_GREEN_SOLID:
    case STATUS_PATTERN_GREEN_SLOW_BLINK:
        status_leds_set(false, true, false);
        break;
    case STATUS_PATTERN_AMBER_BLINK_ONCE:
    case STATUS_PATTERN_AMBER_SLOW_BLINK:
        status_leds_set(true, true, false);
        break;
    case STATUS_PATTERN_RED_BLINK_CODE:
        status_leds_set(true, false, false);
        break;
    case STATUS_PATTERN_OFF:
    default:
        status_leds_set(false, false, false);
        break;
    }

    LOG_INF("status pattern=%d red_blinks=%u repeat=%u duration_ms=%u",
            indication.pattern, indication.red_blink_count,
            indication.repeat_count, indication.duration_ms);
}

static int status_leds_init(void)
{
    int ret = 0;

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    ret |= configure_output(&status0_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    ret |= configure_output(&status0_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    ret |= configure_output(&status0_blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    ret |= configure_output(&status1_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    ret |= configure_output(&status1_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    ret |= configure_output(&status1_blue);
#endif

    status_leds_set(false, false, false);
    return ret;
}

static int ble_runtime_init(void)
{
    int ret;

    if (ble_ready) {
        return 0;
    }

    ret = bt_enable(NULL);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    ble_ready = true;
    LOG_INF("BLE runtime ready");
    return 0;
}

static void ble_runtime_shutdown(void)
{
    int ret;

    if (!ble_ready) {
        return;
    }

    if (ble_ext_adv != NULL) {
        (void)bt_le_ext_adv_stop(ble_ext_adv);
        ble_adv_active = false;
        ret = bt_le_ext_adv_delete(ble_ext_adv);
        if (ret < 0) {
            LOG_WRN("BLE advertising set delete failed during shutdown: %d", ret);
        }
        ble_ext_adv = NULL;
        ble_ext_adv_connectable = false;
    }

    ret = bt_disable();
    if (ret < 0) {
        LOG_WRN("BLE runtime shutdown not supported/failed: %d", ret);
        return;
    }

    ble_ready = false;
    anchor_scan_active = false;
    gateway_mesh_scan_active = false;
    clicker_ready_scan_active = false;
    LOG_INF("BLE runtime shut down for clicker idle");
}

static bool ble_rf_active(void)
{
    return ble_adv_active ||
           anchor_scan_active ||
           gateway_mesh_scan_active ||
           clicker_ready_scan_active;
}

static int radio_guard_ble_start(const char *reason)
{
    if (uwb_rf_active) {
        LOG_ERR("blocked BLE RF while UWB active: %s", reason);
        return -EBUSY;
    }
    return 0;
}

static int radio_guard_uwb_start(const char *reason)
{
    if (ble_rf_active()) {
        LOG_ERR("blocked UWB while BLE RF active: %s", reason);
        return -EBUSY;
    }
    if (uwb_rf_active) {
        LOG_ERR("blocked nested UWB operation: %s", reason);
        return -EBUSY;
    }

    uwb_rf_active = true;
    return 0;
}

static void radio_guard_uwb_stop(void)
{
    uwb_rf_active = false;
}

static int debug_serial_init(void)
{
#if HAS_SERIAL_CONSOLE
    if (!device_is_ready(serial_console)) {
        return -ENODEV;
    }
#endif
#if defined(CONFIG_USB_DEVICE_STACK)
    int ret = usb_enable(NULL);

    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
#endif
    return 0;
}

static bool debug_serial_dtr_ready(void)
{
#if HAS_SERIAL_CONSOLE && defined(CONFIG_UART_LINE_CTRL)
    uint32_t dtr = 0u;

    if (uart_line_ctrl_get(serial_console, UART_LINE_CTRL_DTR, &dtr) == 0) {
        return dtr != 0u;
    }
#endif
    return true;
}

static int gateway_emit_serial_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
#if HAS_SERIAL_CONSOLE
    struct proto_packet frame_packet;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        payload_len > UINT8_MAX) {
        return -EINVAL;
    }
    if (!device_is_ready(serial_console)) {
        return -ENODEV;
    }
    if (!debug_serial_dtr_ready()) {
        return -EAGAIN;
    }

    frame_packet = *packet;
    frame_packet.payload_len = (uint8_t)payload_len;
    ret = serial_frame_encode_packet(&frame_packet,
                                     payload,
                                     frame,
                                     sizeof(frame),
                                     &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    uart_poll_out(serial_console, SERIAL_FRAME_DELIMITER);
    for (size_t i = 0u; i < frame_len; i++) {
        uart_poll_out(serial_console, frame[i]);
    }

    LOG_INF("gateway USB COBS frame emitted: msg=0x%02x src=0x%016llx seq=%u payload_len=%u frame_len=%u",
            frame_packet.msg_type,
            (unsigned long long)frame_packet.src_id,
            frame_packet.seq,
            (unsigned int)payload_len,
            (unsigned int)frame_len);
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENODEV;
#endif
}

static int mesh_errno_from_proto(int ret)
{
    switch (ret) {
    case PROTO_OK:
        return 0;
    case PROTO_ERR_MALFORMED:
        return -EBUSY;
    case PROTO_ERR_NOT_FOUND:
    case PROTO_ERR_STALE:
        return -EHOSTUNREACH;
    case PROTO_ERR_NO_SPACE:
        return -ENOSPC;
    default:
        return -EINVAL;
    }
}

static uint16_t gateway_next_command_seq(void)
{
    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    return gateway_command_seq;
}

static int append_anchor_status_tlvs(uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
    int ret;

    ret = tlv_append_u8(payload, payload_cap, payload_len, TLV_DEVICE_ROLE, (uint8_t)DEVICE_ROLE);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len, TLV_UPTIME_MS, k_uptime_get_32());
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len, TLV_STATUS_BITS, 0u);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_append_status_tlvs(&mesh_runtime, payload, payload_cap, payload_len);
}

static int anchor_send_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    bool diagnostic;
    int ret;

    if (command == NULL) {
        return -EINVAL;
    }

    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (status == COMMAND_OK && command_id == CMD_GET_STATUS) {
        ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }

    diagnostic = (command->flags & FLAG_DIAGNOSTIC) != 0u;
    ret = mesh_init_command_result(&outbound.packet,
                                   DEVICE_ID,
                                   GATEWAY_ID,
                                   command->session_id,
                                   command->seq,
                                   (uint8_t)payload_len,
                                   diagnostic);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint8_t)payload_len;

    return mesh_start_tracked_tx(&outbound, "command-result");
}

static void gateway_emit_serial_command_result(const struct proto_packet *command,
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

    ret = gateway_emit_serial_packet(&result, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway USB command failure result not emitted: %d", ret);
    }
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

static void gateway_clear_pending_command_result(const struct proto_packet *command)
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
    mesh_clear_route_waiting_tx(&command);
    gateway_emit_serial_command_result(&command, command_id, COMMAND_TIMEOUT, 0u);
}

static void gateway_note_command_result(const struct proto_packet *packet)
{
    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !gateway_command_pending_complete_result(&gateway_command_pending_state, packet)) {
        return;
    }

    (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
    LOG_INF("gateway command result received: src=0x%016llx session=%u seq=%u",
            (unsigned long long)packet->src_id,
            packet->session_id,
            packet->seq);
}

static int gateway_begin_command_result_wait(const struct proto_packet *command,
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

static void anchor_handle_local_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND ||
        packet->dst_id != DEVICE_ID) {
        return;
    }

    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(-ret);
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        status = COMMAND_UNSUPPORTED_COMMAND;
        reason = 1u;
    }

    ret = anchor_send_command_result(packet, command_id, status, reason);
    if (ret < 0) {
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        return;
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
}

static int gateway_route_serial_packet(struct proto_packet *packet,
                                       uint8_t *payload,
                                       size_t payload_len)
{
    struct mesh_outbound outbound = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    ret = gateway_command_prepare_outbound(packet,
                                           payload,
                                           payload_len,
                                           DEVICE_ID,
                                           k_uptime_get_32(),
                                           packet != NULL && packet->seq == 0u ?
                                           gateway_next_command_seq() : 0u,
                                           &outbound,
                                           &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected USB command: %d", ret);
        gateway_emit_serial_command_result(packet,
                                           CMD_VENDOR_BASE,
                                           ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_begin_command_result_wait(&outbound.packet, command_id);
    if (ret < 0) {
        LOG_WRN("gateway command result tracker busy: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        gateway_emit_serial_command_result(&outbound.packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = mesh_start_tracked_tx(&outbound, "usb-command");
    if (ret < 0) {
        LOG_WRN("gateway USB command route failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id);
            return 0;
        }
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_serial_command_result(&outbound.packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway USB command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
            (unsigned int)command_id,
            (unsigned long long)outbound.packet.dst_id,
            outbound.packet.session_id,
            outbound.packet.seq,
            outbound.packet.ttl);
    return 0;
}

static void gateway_handle_serial_frame(const uint8_t *frame, size_t frame_len)
{
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway USB COBS frame decode failed: %d", ret);
        return;
    }

    ret = gateway_route_serial_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway USB packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
}

static void gateway_serial_rx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

#if HAS_SERIAL_CONSOLE
    if (device_is_ready(serial_console) && debug_serial_dtr_ready()) {
        unsigned char byte;
        uint16_t read_count = 0u;

        while (read_count < GATEWAY_SERIAL_MAX_BYTES_PER_POLL &&
               uart_poll_in(serial_console, &byte) == 0) {
            read_count++;

            if (gateway_serial_rx_overflow) {
                if (byte == SERIAL_FRAME_DELIMITER) {
                    gateway_serial_rx_overflow = false;
                    gateway_serial_rx_len = 0u;
                    LOG_WRN("gateway USB RX frame dropped after overflow");
                }
                continue;
            }

            if (gateway_serial_rx_len >= sizeof(gateway_serial_rx_frame)) {
                gateway_serial_rx_overflow = true;
                gateway_serial_rx_len = 0u;
                continue;
            }

            gateway_serial_rx_frame[gateway_serial_rx_len] = byte;
            gateway_serial_rx_len++;
            if (byte == SERIAL_FRAME_DELIMITER) {
                if (gateway_serial_rx_len > 1u) {
                    gateway_handle_serial_frame(gateway_serial_rx_frame, gateway_serial_rx_len);
                }
                gateway_serial_rx_len = 0u;
            }
        }
    }
#endif

    (void)k_work_reschedule(&gateway_serial_rx_work, K_MSEC(GATEWAY_SERIAL_POLL_MS));
}

static int ble_start_manufacturer_advertising(const uint8_t *payload,
                                              size_t payload_len,
                                              bool connectable)
{
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, payload, payload_len),
    };
    const struct bt_le_adv_param *param = connectable ? &ble_conn_adv_param : &ble_adv_param;
    int ret;

    if (payload == NULL || payload_len == 0u) {
        return -EINVAL;
    }

    ret = radio_guard_ble_start("advertising");
    if (ret < 0) {
        return ret;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    if (ble_ext_adv != NULL && ble_ext_adv_connectable != connectable) {
        (void)bt_le_ext_adv_stop(ble_ext_adv);
        ble_adv_active = false;
        ret = bt_le_ext_adv_delete(ble_ext_adv);
        if (ret < 0) {
            return ret;
        }
        ble_ext_adv = NULL;
        ble_ext_adv_connectable = false;
    }

    if (ble_ext_adv == NULL) {
        ret = bt_le_ext_adv_create(param, NULL, &ble_ext_adv);
        if (ret < 0) {
            return ret;
        }
        ble_ext_adv_connectable = connectable;
    } else {
        (void)bt_le_ext_adv_stop(ble_ext_adv);
        ble_adv_active = false;
    }

    ret = bt_le_ext_adv_set_data(ble_ext_adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (ret < 0) {
        return ret;
    }
    ret = bt_le_ext_adv_start(ble_ext_adv, BT_LE_EXT_ADV_START_DEFAULT);
    if (ret == 0) {
        ble_adv_active = true;
    }
    return ret;
}

static int ble_stop_advertising(void)
{
    int ret;

    if (ble_ext_adv == NULL) {
        return 0;
    }

    ret = bt_le_ext_adv_stop(ble_ext_adv);
    if (ret == 0 || ret == -EALREADY) {
        ble_adv_active = false;
        return 0;
    }
    return ret;
}

static int ble_advertise_manufacturer_payload(const uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t duration_ms,
                                              bool connectable)
{
    int ret;

    ret = ble_start_manufacturer_advertising(payload, payload_len, connectable);
    if (ret < 0) {
        return ret;
    }
    k_msleep(duration_ms);

    return ble_stop_advertising();
}

static int ble_advertise_mesh_payload(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t duration_ms,
                                      bool connectable)
{
    return ble_advertise_manufacturer_payload(payload, payload_len, duration_ms, connectable);
}

static uint16_t local_uwb_short_addr(void)
{
    uint16_t short_addr = (uint16_t)(DEVICE_ID & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

static uint8_t mesh_quality_from_rssi(int8_t rssi)
{
    if (rssi <= -100) {
        return 1u;
    }
    if (rssi >= -40) {
        return 100u;
    }
    return (uint8_t)((int)rssi + 100);
}

struct anchor_scan_parse_context {
    bool found;
    struct ble_discovery_req request;
};

struct clicker_ready_parse_context {
    bool found;
    struct ble_discovery_ready ready;
};

struct mesh_frame_parse_context {
    bool found;
    uint64_t previous_hop_id;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

struct anchor_scan_combined_context {
    struct mesh_frame_parse_context mesh;
    struct anchor_scan_parse_context discovery;
};

static bool parse_ready_ad(struct bt_data *data, void *user_data)
{
    struct clicker_ready_parse_context *context = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }

    if (ble_discovery_ready_decode(data->data, data->data_len,
                                        &context->ready) == PROTO_OK) {
        context->found = true;
        return false;
    }

    return true;
}

static bool parse_mesh_ad(struct bt_data *data, void *user_data)
{
    struct mesh_frame_parse_context *context = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }

    if (mesh_ble_frame_decode(data->data,
                              data->data_len,
                              DEVICE_ID,
                              &context->previous_hop_id,
                              &context->packet,
                              context->payload,
                              sizeof(context->payload),
                              &context->payload_len) == PROTO_OK) {
        context->found = true;
        return false;
    }

    return true;
}

static bool parse_anchor_scan_ad(struct bt_data *data, void *user_data)
{
    struct anchor_scan_combined_context *context = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }

    if (ble_discovery_req_decode(data->data,
                                 data->data_len,
                                 &context->discovery.request) == PROTO_OK) {
        context->discovery.found = true;
        return false;
    }

    if (mesh_ble_frame_decode(data->data,
                              data->data_len,
                              DEVICE_ID,
                              &context->mesh.previous_hop_id,
                              &context->mesh.packet,
                              context->mesh.payload,
                              sizeof(context->mesh.payload),
                              &context->mesh.payload_len) == PROTO_OK) {
        context->mesh.found = true;
        return false;
    }

    return true;
}

static int anchor_start_ble_scan(void);
static int gateway_start_mesh_scan(void);
static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
static bool mesh_queue_parsed(const struct mesh_frame_parse_context *context,
                              const bt_addr_le_t *addr,
                              int8_t rssi);
static bool mesh_queue_from_scan(struct net_buf_simple *buf,
                                 const bt_addr_le_t *addr,
                                 int8_t rssi);
static void anchor_scan_cb(const bt_addr_le_t *addr,
                           int8_t rssi,
                           uint8_t adv_type,
                           struct net_buf_simple *buf);
static int build_range_report(uint64_t clicker_id,
                              uint32_t event_seq,
                              const struct dwm3000_range_result *range_result);
static int build_range_report_samples(uint64_t clicker_id,
                                      uint32_t event_seq,
                                      const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      uint16_t sample_count);

static int16_t ready_rssi_score(const struct ble_discovery_ready *ready,
                                int8_t clicker_rssi)
{
    ARG_UNUSED(ready);
    return (int16_t)clicker_rssi;
}

static bool ready_anchor_precedes(const struct ready_anchor *left,
                                  const struct ready_anchor *right)
{
    if (left->rssi_score != right->rssi_score) {
        return left->rssi_score > right->rssi_score;
    }
    return left->ready.anchor_id < right->ready.anchor_id;
}

static void sort_ready_anchors_by_rssi(void)
{
    for (uint8_t i = 1u; i < clicker_ready_count; i++) {
        struct ready_anchor candidate = clicker_ready_anchors[i];
        uint8_t j = i;

        while (j > 0u && ready_anchor_precedes(&candidate, &clicker_ready_anchors[j - 1u])) {
            clicker_ready_anchors[j] = clicker_ready_anchors[j - 1u];
            j--;
        }
        clicker_ready_anchors[j] = candidate;
    }
}

static void clicker_ready_collection_reset(uint8_t expected_flags,
                                           uint32_t expected_event_seq,
                                           uint8_t expected_attempt_index)
{
    k_spinlock_key_t key;

    key = k_spin_lock(&clicker_ready_lock);
    clicker_ready_count = 0u;
    clicker_ready_expected_flags = expected_flags;
    clicker_ready_expected_event_seq = expected_event_seq;
    clicker_ready_expected_attempt_index = expected_attempt_index;
    memset(clicker_ready_anchors, 0, sizeof(clicker_ready_anchors));
    k_spin_unlock(&clicker_ready_lock, key);
}

static uint8_t clicker_ready_snapshot(struct ready_anchor *anchors,
                                      uint8_t anchor_cap)
{
    k_spinlock_key_t key;
    uint8_t found_count;

    if (anchors == NULL || anchor_cap == 0u) {
        return 0u;
    }

    key = k_spin_lock(&clicker_ready_lock);
    found_count = MIN(clicker_ready_count, anchor_cap);
    for (uint8_t i = 0u; i < found_count; i++) {
        anchors[i] = clicker_ready_anchors[i];
    }
    k_spin_unlock(&clicker_ready_lock, key);

    return found_count;
}

static bool ready_anchor_store_locked(const struct ble_discovery_ready *ready,
                                      int8_t clicker_rssi,
                                      uint32_t received_ms)
{
    struct ready_anchor candidate = {
        .ready = *ready,
        .clicker_rssi = clicker_rssi,
        .rssi_score = ready_rssi_score(ready, clicker_rssi),
        .received_ms = received_ms,
    };

    for (uint8_t i = 0u; i < clicker_ready_count; i++) {
        if (clicker_ready_anchors[i].ready.anchor_id != ready->anchor_id) {
            continue;
        }
        if (candidate.rssi_score > clicker_ready_anchors[i].rssi_score) {
            clicker_ready_anchors[i] = candidate;
            sort_ready_anchors_by_rssi();
            return true;
        }
        return false;
    }

    if (clicker_ready_count < MAX_READY_ANCHORS) {
        clicker_ready_anchors[clicker_ready_count] = candidate;
        clicker_ready_count++;
        sort_ready_anchors_by_rssi();
        return true;
    }

    if (ready_anchor_precedes(&candidate, &clicker_ready_anchors[MAX_READY_ANCHORS - 1u])) {
        clicker_ready_anchors[MAX_READY_ANCHORS - 1u] = candidate;
        sort_ready_anchors_by_rssi();
        return true;
    }

    return false;
}

static bool service_windows_overlap(int64_t left_start_ms,
                                    int64_t left_end_ms,
                                    int64_t right_start_ms,
                                    int64_t right_end_ms)
{
    return left_start_ms < right_end_ms && right_start_ms < left_end_ms;
}

static bool anchor_service_add_locked(const struct ble_discovery_req *request,
                                      int8_t rssi,
                                      int64_t received_ms,
                                      bool *start_work)
{
    int64_t ready_scan_start_ms;
    int64_t ready_scan_end_ms;

    if (request == NULL || start_work == NULL) {
        return false;
    }
    if (anchor_uwb_busy || (anchor_service_active && !anchor_admission_open)) {
        return false;
    }

    ready_scan_start_ms = received_ms + request->ready_scan_starts_in_ms;
    ready_scan_end_ms = ready_scan_start_ms + request->ready_scan_duration_ms;
    if (anchor_service_count > 0u &&
        !service_windows_overlap(ready_scan_start_ms,
                                 ready_scan_end_ms,
                                 anchor_service_window_start_ms,
                                 anchor_service_window_end_ms)) {
        return false;
    }

    for (uint8_t i = 0u; i < anchor_service_count; i++) {
        if (anchor_service_slots[i].request.clicker_id == request->clicker_id &&
            anchor_service_slots[i].request.event_seq == request->event_seq &&
            anchor_service_slots[i].request.attempt_index == request->attempt_index) {
            anchor_service_slots[i].rssi = rssi;
            if (ready_scan_start_ms > anchor_service_slots[i].ready_scan_start_ms) {
                anchor_service_slots[i].ready_scan_start_ms = ready_scan_start_ms;
                anchor_service_slots[i].ready_scan_end_ms = ready_scan_end_ms;
                anchor_service_slots[i].request = *request;
            }
            return true;
        }
    }

    if (anchor_service_count >= ANCHOR_SERVICE_QUEUE_DEPTH) {
        return false;
    }

    anchor_service_slots[anchor_service_count].request = *request;
    anchor_service_slots[anchor_service_count].rssi = rssi;
    anchor_service_slots[anchor_service_count].ready_scan_start_ms = ready_scan_start_ms;
    anchor_service_slots[anchor_service_count].ready_scan_end_ms = ready_scan_end_ms;
    anchor_service_count++;
    if (!anchor_service_active) {
        anchor_service_active = true;
        anchor_admission_open = true;
        anchor_service_window_start_ms = ready_scan_start_ms;
        anchor_service_window_end_ms = ready_scan_end_ms;
        *start_work = true;
    }
    return true;
}

static uint16_t delay_ms_to_u16(int64_t delay_ms)
{
    if (delay_ms <= 0) {
        return 0u;
    }
    if (delay_ms > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)delay_ms;
}

static bool anchor_service_slot_precedes(const struct anchor_service_slot *left,
                                         const struct anchor_service_slot *right)
{
    if (left->request.priority_id != right->request.priority_id) {
        return left->request.priority_id < right->request.priority_id;
    }
    if (left->request.clicker_id != right->request.clicker_id) {
        return left->request.clicker_id < right->request.clicker_id;
    }
    if (left->request.event_seq != right->request.event_seq) {
        return left->request.event_seq < right->request.event_seq;
    }
    return left->request.attempt_index < right->request.attempt_index;
}

static void select_anchor_service_slot(struct anchor_service_slot *slots,
                                       uint8_t slot_count)
{
    uint8_t selected = 0u;

    for (uint8_t i = 1u; i < slot_count; i++) {
        if (anchor_service_slot_precedes(&slots[i], &slots[selected])) {
            selected = i;
        }
    }

    if (selected != 0u) {
        struct anchor_service_slot chosen = slots[selected];

        slots[selected] = slots[0];
        slots[0] = chosen;
    }
}

static int advertise_ready_to_range(const struct anchor_service_slot *slot)
{
    struct ble_discovery_ready ready = {
        .anchor_id = DEVICE_ID,
        .target_clicker_id = slot->request.clicker_id,
        .priority_id_seen = slot->request.priority_id,
        .target_event_seq = slot->request.event_seq,
        .uwb_short_addr = local_uwb_short_addr(),
        .flags = slot->request.flags,
        .attempt_index = slot->request.attempt_index,
        .rssi_hint = slot->rssi,
        .status = BLE_READY_STATUS_ADMITTED,
    };
    uint8_t payload[BLE_DISCOVERY_READY_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = ble_discovery_ready_encode(&ready, payload, sizeof(payload), &payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    LOG_INF("anchor READY advert: clicker=0x%016llx event_seq=%u attempt=%u ready_ms=%u priority=0x%016llx",
            (unsigned long long)ready.target_clicker_id,
            ready.target_event_seq,
            ready.attempt_index,
            ANCHOR_READY_ADV_MS,
            (unsigned long long)ready.priority_id_seen);
    return ble_advertise_manufacturer_payload(payload, payload_len, ANCHOR_READY_ADV_MS, false);
}

struct anchor_range_window_report {
    struct dwm3000_range_result result;
    int32_t distance_samples_mm[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t distance_sum_mm;
    uint32_t quality_sum;
    uint16_t sample_count;
    bool have_result;
    bool rsl_sampled;
};

static int32_t average_i32_nearest(int64_t sum, uint16_t count)
{
    int64_t half;

    if (count == 0u) {
        return 0;
    }

    half = count / 2;
    return (int32_t)(sum >= 0 ? (sum + half) / count : (sum - half) / count);
}

static void anchor_range_window_record(struct anchor_range_window_report *report,
                                       const struct dwm3000_range_result *result)
{
    int8_t sampled_rsl = 0;

    if (report == NULL || result == NULL) {
        return;
    }

    if (report->rsl_sampled) {
        sampled_rsl = report->result.rsl_dbm;
    }

    if (!report->have_result || result->status == RANGE_OK) {
        report->result = *result;
        report->have_result = true;
    }
    if (result->rsl_sampled && !report->rsl_sampled) {
        sampled_rsl = result->rsl_dbm;
        report->rsl_sampled = true;
    }
    if (report->rsl_sampled) {
        report->result.rsl_dbm = sampled_rsl;
        report->result.rsl_sampled = true;
    }

    if (result->status != RANGE_OK ||
        report->sample_count >= RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return;
    }

    report->distance_samples_mm[report->sample_count] = result->distance_mm;
    report->distance_sum_mm += result->distance_mm;
    report->quality_sum += result->quality;
    report->sample_count++;
}

static void anchor_range_window_finalize(struct anchor_range_window_report *report)
{
    if (report == NULL || !report->have_result || report->sample_count == 0u) {
        return;
    }

    report->result.distance_mm = average_i32_nearest(report->distance_sum_mm,
                                                     report->sample_count);
    report->result.quality = (uint8_t)((report->quality_sum +
                                        (report->sample_count / 2u)) /
                                       report->sample_count);
    report->result.status = RANGE_OK;
}

static void build_slot_report_if_click(const struct anchor_service_slot *slot,
                                       const struct anchor_range_window_report *report)
{
    int ret;

    if (!ble_flags_count_as_click(slot->request.flags)) {
        return;
    }
    if (report == NULL || !report->have_result) {
        return;
    }

    ret = build_range_report_samples(slot->request.clicker_id,
                                     slot->request.event_seq,
                                     &report->result,
                                     report->distance_samples_mm,
                                     report->sample_count);
    if (ret < 0) {
        LOG_WRN("failed to build anchor range report: %d", ret);
    }
}

static void run_anchor_uwb_window(const struct anchor_service_slot *slot)
{
    struct dwm3000_range_request expected = {
        .initiator_id = slot->request.clicker_id,
        .responder_id = DEVICE_ID,
        .responder_short_addr = local_uwb_short_addr(),
        .session_id = slot->request.event_seq,
        .seq = 0u,
        .flags = slot->request.flags,
        .timeout_ms = ANCHOR_UWB_WAIT_MS,
    };
    struct dwm3000_range_result range_result;
    struct anchor_range_window_report window_report = {0};
    int64_t deadline_ms = k_uptime_get() + ANCHOR_UWB_WAIT_MS;
    bool saw_poll = false;
    int ret = -ETIMEDOUT;

    LOG_INF("anchor UWB responder window start: clicker=0x%016llx event_seq=%u attempt=%u window_ms=%u",
            (unsigned long long)slot->request.clicker_id,
            slot->request.event_seq,
            slot->request.attempt_index,
            ANCHOR_UWB_WAIT_MS);

    while (true) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();

        if (remaining_ms <= 0) {
            break;
        }
        expected.timeout_ms = (uint32_t)remaining_ms;
        expected.capture_rsl = !window_report.rsl_sampled;
        ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                     &expected,
                                                     expected.timeout_ms,
                                                     &range_result);
        if (ret == -EAGAIN) {
            continue;
        }
        if (ret == -ETIMEDOUT) {
            break;
        }

        saw_poll = true;
        if (range_result.initiator_id != 0u && range_result.responder_id != 0u) {
            anchor_range_window_record(&window_report, &range_result);
        }
        if (ret < 0 || range_result.status != RANGE_OK) {
            LOG_WRN("anchor UWB exchange failed after poll: clicker=0x%016llx event_seq=%u seq=%u ret=%d status=%u quality=%u rsl=%d dBm",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    range_result.seq,
                    ret,
                    range_result.status,
                    range_result.quality,
                    range_result.rsl_dbm);
        } else {
            LOG_INF("anchor UWB exchange sample complete: clicker=0x%016llx event_seq=%u seq=%u distance_mm=%d quality=%u rsl=%d dBm samples=%u/%u",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality,
                    range_result.rsl_dbm,
                    window_report.sample_count,
                    RANGE_REPORT_MAX_DISTANCE_SAMPLES);
        }
    }

    anchor_range_window_finalize(&window_report);
    build_slot_report_if_click(slot, &window_report);
    if (window_report.sample_count > 0u) {
        LOG_INF("anchor UWB window report ready: clicker=0x%016llx event_seq=%u samples=%u aggregate_distance_mm=%d aggregate_quality=%u rsl=%d dBm",
                (unsigned long long)window_report.result.initiator_id,
                window_report.result.session_id,
                window_report.sample_count,
                window_report.result.distance_mm,
                window_report.result.quality,
                window_report.result.rsl_dbm);
    }

    if (!saw_poll) {
        LOG_WRN("anchor UWB responder window timed out without selected clicker poll: clicker=0x%016llx event_seq=%u ret=%d",
                (unsigned long long)slot->request.clicker_id,
                slot->request.event_seq,
                ret);
    }
}

static void mesh_preempt_for_click_event(void)
{
    struct mesh_outbound pending_report = {0};
    bool requeue_report = false;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        if (mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
            mesh_runtime.pending.packet.src_id == DEVICE_ID) {
            pending_report.packet = mesh_runtime.pending.packet;
            pending_report.payload_len = mesh_runtime.pending.payload_len;
            if (pending_report.payload_len > 0u) {
                memcpy(pending_report.payload,
                       mesh_runtime.pending.payload,
                       pending_report.payload_len);
            }
            requeue_report = true;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        LOG_INF("anchor click discovery preempted active mesh TX");
    }

    k_msgq_purge(&mesh_rx_msgq);
    if (requeue_report) {
        if (k_msgq_put(&report_tx_msgq, &pending_report, K_NO_WAIT) != 0) {
            LOG_WRN("preempted click report could not be requeued");
        }
    }
}

static void anchor_discovery_work_handler(struct k_work *work)
{
    struct anchor_service_slot slots[ANCHOR_SERVICE_QUEUE_DEPTH];
    k_spinlock_key_t key;
    uint8_t slot_count;
    bool uwb_started = false;
    int ret;

    ARG_UNUSED(work);

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_WRN("anchor DWM3000 wake during arbitration failed: %d", ret);
    }

    if (anchor_scan_active) {
        ret = bt_le_scan_stop();
        if (ret < 0 && ret != -EALREADY) {
            LOG_WRN("failed to stop low-duty anchor BLE scan: %d", ret);
        }
        anchor_scan_active = false;
    }

    ret = bt_le_scan_start(&anchor_full_duty_scan_param, anchor_scan_cb);
    if (ret < 0 && ret != -EALREADY) {
        LOG_WRN("failed to start anchor arbitration scan: %d", ret);
    } else {
        anchor_scan_active = true;
        LOG_INF("anchor arbitration scan active until selected READY window");
    }

    while (true) {
        int64_t scan_until_ms;
        int64_t now_ms = k_uptime_get();

        key = k_spin_lock(&anchor_ble_lock);
        scan_until_ms = anchor_service_window_start_ms;
        k_spin_unlock(&anchor_ble_lock, key);

        if (now_ms >= scan_until_ms) {
            break;
        }
        k_msleep((uint32_t)MIN(10, scan_until_ms - now_ms));
    }

    key = k_spin_lock(&anchor_ble_lock);
    anchor_admission_open = false;
    slot_count = anchor_service_count;
    memcpy(slots, anchor_service_slots, sizeof(slots));
    k_spin_unlock(&anchor_ble_lock, key);

    if (anchor_scan_active) {
        ret = bt_le_scan_stop();
        if (ret < 0 && ret != -EALREADY) {
            LOG_WRN("failed to stop anchor BLE scan: %d", ret);
        }
        anchor_scan_active = false;
    }

    if (slot_count == 0u) {
        LOG_WRN("anchor arbitration ended with empty queue");
        goto restart_scan;
    }

    select_anchor_service_slot(slots, slot_count);
    LOG_INF("anchor selected clicker for READY: selected=0x%016llx event_seq=%u attempt=%u queued=%u priority=0x%016llx",
            (unsigned long long)slots[0].request.clicker_id,
            slots[0].request.event_seq,
            slots[0].request.attempt_index,
            slot_count,
            (unsigned long long)slots[0].request.priority_id);

    (void)ble_stop_advertising();
    if (slots[0].ready_scan_start_ms > k_uptime_get()) {
        k_msleep((uint32_t)(slots[0].ready_scan_start_ms - k_uptime_get()));
    }

    ret = advertise_ready_to_range(&slots[0]);
    if (ret < 0) {
        LOG_WRN("anchor READY advertisement failed: %d", ret);
    }
    (void)ble_stop_advertising();

    ret = radio_guard_uwb_start("anchor READY UWB responder window");
    if (ret < 0) {
        goto restart_scan;
    }
    uwb_started = true;
    key = k_spin_lock(&anchor_ble_lock);
    anchor_uwb_busy = true;
    k_spin_unlock(&anchor_ble_lock, key);

    LOG_INF("anchor UWB responder service start: window_ms=%u BLE RF is off",
            ANCHOR_UWB_WAIT_MS);
    run_anchor_uwb_window(&slots[0]);

    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    uwb_started = false;
    LOG_INF("anchor UWB responder service closed; DWM3000 standby requested");

restart_scan:
    if (uwb_started) {
        (void)dwm3000_driver_standby();
        radio_guard_uwb_stop();
    }
    key = k_spin_lock(&anchor_ble_lock);
    anchor_uwb_busy = false;
    anchor_admission_open = false;
    anchor_service_active = false;
    anchor_service_count = 0u;
    anchor_service_window_start_ms = 0;
    anchor_service_window_end_ms = 0;
    k_spin_unlock(&anchor_ble_lock, key);

    ret = anchor_start_ble_scan();
    if (ret < 0) {
        LOG_ERR("failed to restart low-duty anchor BLE scan: %d", ret);
    }
    report_tx_schedule(0u);
}

static void anchor_scan_cb(const bt_addr_le_t *addr,
                           int8_t rssi,
                           uint8_t adv_type,
                           struct net_buf_simple *buf)
{
    struct anchor_scan_combined_context context = {0};
    k_spinlock_key_t key;
    int64_t received_ms;
    bool submit = false;

    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    bt_data_parse(buf, parse_anchor_scan_ad, &context);
    if (context.mesh.found) {
        (void)mesh_queue_parsed(&context.mesh, addr, rssi);
        return;
    }
    if (!context.discovery.found) {
        return;
    }

    mesh_preempt_for_click_event();
    received_ms = k_uptime_get();
    key = k_spin_lock(&anchor_ble_lock);
    if (anchor_service_add_locked(&context.discovery.request, rssi, received_ms, &submit)) {
        LOG_INF("anchor queued wake request: clicker=0x%016llx event_seq=%u attempt=%u flags=0x%02x priority=0x%016llx ready_in_ms=%u ready_scan_ms=%u rssi=%d depth=%u/%u",
                (unsigned long long)context.discovery.request.clicker_id,
                context.discovery.request.event_seq,
                context.discovery.request.attempt_index,
                context.discovery.request.flags,
                (unsigned long long)context.discovery.request.priority_id,
                context.discovery.request.ready_scan_starts_in_ms,
                context.discovery.request.ready_scan_duration_ms,
                rssi,
                anchor_service_count,
                ANCHOR_SERVICE_QUEUE_DEPTH);
    }
    k_spin_unlock(&anchor_ble_lock, key);

    if (submit) {
        (void)k_work_submit(&anchor_ble_work);
    } else if (!anchor_admission_open) {
        LOG_DBG("anchor ignored wake request while busy/pending: clicker=0x%016llx event_seq=%u attempt=%u",
                (unsigned long long)context.discovery.request.clicker_id,
                context.discovery.request.event_seq,
                context.discovery.request.attempt_index);
    }
}

static void clicker_ready_scan_cb(const bt_addr_le_t *addr,
                                  int8_t rssi,
                                  uint8_t adv_type,
                                  struct net_buf_simple *buf)
{
    struct clicker_ready_parse_context context = {0};
    k_spinlock_key_t key;
    bool stored = false;
    uint8_t ready_count = 0u;
    int16_t score = 0;
    uint32_t received_ms = k_uptime_get_32();

    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    bt_data_parse(buf, parse_ready_ad, &context);
    if (!context.found) {
        return;
    }

    key = k_spin_lock(&clicker_ready_lock);
    if (context.ready.status == BLE_READY_STATUS_ADMITTED &&
        context.ready.flags == clicker_ready_expected_flags &&
        context.ready.target_clicker_id == DEVICE_ID &&
        context.ready.target_event_seq == clicker_ready_expected_event_seq &&
        context.ready.attempt_index == clicker_ready_expected_attempt_index) {
        score = ready_rssi_score(&context.ready, rssi);
        stored = ready_anchor_store_locked(&context.ready, rssi, received_ms);
        ready_count = clicker_ready_count;
    }
    k_spin_unlock(&clicker_ready_lock, key);

    if (stored) {
        LOG_INF("READY accepted: anchor=0x%016llx uwb=0x%04x event_seq=%u attempt=%u priority_seen=0x%016llx anchor_rssi=%d clicker_rssi=%d score=%d count=%u/%u",
                (unsigned long long)context.ready.anchor_id,
                context.ready.uwb_short_addr,
                context.ready.target_event_seq,
                context.ready.attempt_index,
                (unsigned long long)context.ready.priority_id_seen,
                context.ready.rssi_hint,
                rssi,
                score,
                ready_count,
                MAX_READY_ANCHORS);
    } else if (context.ready.flags != clicker_ready_expected_flags ||
               context.ready.target_clicker_id != DEVICE_ID ||
               context.ready.target_event_seq != clicker_ready_expected_event_seq ||
               context.ready.attempt_index != clicker_ready_expected_attempt_index) {
        LOG_DBG("READY ignored: anchor=0x%016llx target=0x%016llx event_seq=%u attempt=%u flags=0x%02x expected_event=%u expected_attempt=%u expected_flags=0x%02x status=%u",
                (unsigned long long)context.ready.anchor_id,
                (unsigned long long)context.ready.target_clicker_id,
                context.ready.target_event_seq,
                context.ready.attempt_index,
                context.ready.flags,
                clicker_ready_expected_event_seq,
                clicker_ready_expected_attempt_index,
                clicker_ready_expected_flags,
                context.ready.status);
    }
}

static int anchor_start_ble_scan(void)
{
    int ret;

    ret = radio_guard_ble_start("anchor scan");
    if (ret < 0) {
        return ret;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    ret = bt_le_scan_start(&anchor_low_duty_scan_param, anchor_scan_cb);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    anchor_scan_active = true;
    LOG_INF("anchor low-duty BLE scan active: interval=%u units window=%u units; DWM3000 idle until discovery",
            anchor_low_duty_scan_param.interval,
            anchor_low_duty_scan_param.window);
    return 0;
}

static void gateway_mesh_scan_cb(const bt_addr_le_t *addr,
                                 int8_t rssi,
                                 uint8_t adv_type,
                                 struct net_buf_simple *buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    (void)mesh_queue_from_scan(buf, addr, rssi);
}

static int gateway_start_mesh_scan(void)
{
    int ret;

    ret = radio_guard_ble_start("gateway scan");
    if (ret < 0) {
        return ret;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    ret = bt_le_scan_start(&gateway_mesh_scan_param, gateway_mesh_scan_cb);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    gateway_mesh_scan_active = true;
    LOG_INF("gateway mesh BLE scan active: interval=%u units window=%u units",
            gateway_mesh_scan_param.interval,
            gateway_mesh_scan_param.window);
    return 0;
}

static int clicker_scan_for_ready_list(struct ready_anchor *anchors,
                                       uint8_t anchor_cap,
                                       uint8_t expected_flags,
                                       uint32_t expected_event_seq,
                                       uint8_t expected_attempt_index,
                                       uint32_t duration_ms)
{
    uint8_t found_count;
    int ret;

    if (anchors == NULL || anchor_cap == 0u) {
        return -EINVAL;
    }

    ret = radio_guard_ble_start("clicker READY scan");
    if (ret < 0) {
        return ret;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    LOG_INF("clicker scanning for READY advertisements: duration_ms=%u max=%u event_seq=%u attempt=%u expected_flags=0x%02x",
            duration_ms,
            MAX_READY_ANCHORS,
            expected_event_seq,
            expected_attempt_index,
            expected_flags);

    ret = bt_le_scan_start(&clicker_ready_scan_param, clicker_ready_scan_cb);
    if (ret == -EALREADY) {
        (void)bt_le_scan_stop();
        clicker_ready_scan_active = false;
        ret = bt_le_scan_start(&clicker_ready_scan_param, clicker_ready_scan_cb);
    }
    if (ret < 0) {
        return ret;
    }
    clicker_ready_scan_active = true;

    k_msleep(duration_ms);

    ret = bt_le_scan_stop();
    clicker_ready_scan_active = false;
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    found_count = clicker_ready_snapshot(anchors, anchor_cap);

    if (found_count == 0u) {
        return -ETIMEDOUT;
    }

    LOG_INF("clicker READY scan complete: ready_anchors=%u sorted_by_rssi=1", found_count);
    for (uint8_t i = 0u; i < found_count; i++) {
        LOG_INF("READY order[%u]: id=0x%016llx uwb=0x%04x attempt=%u anchor_rssi=%d clicker_rssi=%d score=%d",
                i,
                (unsigned long long)anchors[i].ready.anchor_id,
                anchors[i].ready.uwb_short_addr,
                anchors[i].ready.attempt_index,
                anchors[i].ready.rssi_hint,
                anchors[i].clicker_rssi,
                anchors[i].rssi_score);
    }

    return (int)found_count;
}

static int advertise_discovery_request(const struct ble_discovery_req *request,
                                       uint32_t duration_ms)
{
    uint8_t payload[BLE_DISCOVERY_REQ_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = ble_discovery_req_encode(request, payload, sizeof(payload), &payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    LOG_INF("clicker BLE wake advertisement: clicker=0x%016llx event_seq=%u attempt=%u flags=0x%02x duration_ms=%u ready_scan_starts_in_ms=%u ready_scan_ms=%u min_anchors=%u priority=0x%016llx",
            (unsigned long long)request->clicker_id,
            request->event_seq,
            request->attempt_index,
            request->flags,
            duration_ms,
            request->ready_scan_starts_in_ms,
            request->ready_scan_duration_ms,
            request->min_anchor_count,
            (unsigned long long)request->priority_id);
    return ble_advertise_manufacturer_payload(payload, payload_len, duration_ms, false);
}

static int advertise_wake_window(const struct ble_discovery_req *request,
                                 uint32_t window_ms)
{
    int64_t close_ms;
    int ret;

    if (request == NULL || window_ms == 0u) {
        return -EINVAL;
    }

    close_ms = k_uptime_get() + window_ms;
    while (k_uptime_get() < close_ms) {
        struct ble_discovery_req advertised = *request;
        int64_t remaining_ms = close_ms - k_uptime_get();
        uint32_t slice_ms;

        if (remaining_ms <= 0) {
            break;
        }

        advertised.ready_scan_starts_in_ms = delay_ms_to_u16(remaining_ms);
        slice_ms = MIN(WAKE_ADV_UPDATE_MS, (uint32_t)remaining_ms);
        ret = advertise_discovery_request(&advertised, slice_ms);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int clicker_collect_ready_attempt(const struct ble_discovery_req *request,
                                         struct ready_anchor *anchors,
                                         uint8_t anchor_cap)
{
    int ret;

    if (request == NULL || anchors == NULL || anchor_cap == 0u) {
        return -EINVAL;
    }

    clicker_ready_collection_reset(request->flags,
                                   request->event_seq,
                                   request->attempt_index);

    ret = advertise_wake_window(request, WAKE_ADV_MS);
    if (ret < 0) {
        return ret;
    }

    return clicker_scan_for_ready_list(anchors,
                                       anchor_cap,
                                       request->flags,
                                       request->event_seq,
                                       request->attempt_index,
                                       READY_SCAN_MS);
}

static void mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_relay_tx_active(&mesh_runtime)) {
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        return;
    }

    deadline = mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ?
               mesh_runtime.pending.gateway_ack_deadline_ms :
               mesh_runtime.pending.hop_ack_deadline_ms;
    delay_ms = deadline > now ? deadline - now : 1u;
    (void)k_work_reschedule(&mesh_tx_timeout_work, K_MSEC(delay_ms));
}

static void mesh_stop_role_scan(void)
{
    if (DEVICE_ROLE == ROLE_ANCHOR && anchor_scan_active) {
        (void)bt_le_scan_stop();
        anchor_scan_active = false;
    } else if (DEVICE_ROLE == ROLE_GATEWAY && gateway_mesh_scan_active) {
        (void)bt_le_scan_stop();
        gateway_mesh_scan_active = false;
    }
}

static void mesh_restart_role_scan(void)
{
    int ret;

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy && !anchor_service_active) {
        ret = anchor_start_ble_scan();
        if (ret < 0) {
            LOG_WRN("mesh failed to restart anchor scan: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = gateway_start_mesh_scan();
        if (ret < 0) {
            LOG_WRN("mesh failed to restart gateway scan: %d", ret);
        }
    }
}

static bool mesh_outbound_uses_advertising(const struct mesh_outbound *out)
{
    if (out == NULL) {
        return false;
    }

    return out->packet.msg_type == MSG_ROUTE_REQ ||
           out->packet.msg_type == MSG_ROUTE_REPLY ||
           out->packet.msg_type == MSG_ROUTE_ADV;
}

static int mesh_send_connected_payload(const struct mesh_outbound *out,
                                       const uint8_t *frame,
                                       size_t frame_len,
                                       const char *reason)
{
    struct bt_conn *conn = NULL;
    uint16_t mtu;
    uint16_t handle;
    int ret;

    ret = mesh_neighbor_connection(out->next_hop_id, &conn);
    if (ret < 0) {
        return ret;
    }

    ret = mesh_exchange_mtu(conn);
    if (ret < 0) {
        LOG_WRN("mesh MTU exchange failed for %s: %d", reason, ret);
    }

    mtu = bt_gatt_get_mtu(conn);
    if (mtu <= 3u || frame_len > (size_t)(mtu - 3u)) {
        bt_conn_unref(conn);
        LOG_WRN("mesh connection frame too large for ATT MTU: frame_len=%u mtu=%u",
                (unsigned int)frame_len,
                mtu);
        return -EMSGSIZE;
    }

    handle = mesh_conn_rx_value_handle();
    ret = bt_gatt_write_without_response(conn, handle, frame, (uint16_t)frame_len, false);
    bt_conn_unref(conn);
    return ret;
}

static int mesh_send_advertisement_payload(const struct mesh_outbound *out,
                                           const uint8_t *frame,
                                           size_t frame_len,
                                           const char *reason)
{
    int ret;

    mesh_stop_role_scan();
    ret = ble_advertise_mesh_payload(frame, frame_len, MESH_DISCOVERY_ADV_TX_MS, true);
    mesh_restart_role_scan();
    if (ret < 0) {
        LOG_WRN("mesh BLE discovery advertisement failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                out->packet.msg_type,
                (unsigned long long)out->next_hop_id,
                (unsigned int)frame_len,
                ret);
    }
    return ret;
}

static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int ret;

    ret = mesh_ble_frame_encode(DEVICE_ID, out, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }

    if (mesh_outbound_uses_advertising(out)) {
        ret = mesh_send_advertisement_payload(out, frame, frame_len, reason);
    } else {
        ret = mesh_send_connected_payload(out, frame, frame_len, reason);
    }
    if (ret < 0) {
        LOG_WRN("mesh BLE TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                out->packet.msg_type,
                (unsigned long long)out->next_hop_id,
                (unsigned int)frame_len,
                ret);
        return ret;
    }

    LOG_INF("mesh BLE TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u frame_len=%u",
            reason,
            out->packet.msg_type,
            (unsigned long long)out->packet.src_id,
            (unsigned long long)out->packet.dst_id,
            (unsigned long long)out->next_hop_id,
            out->packet.seq,
            out->packet.ttl,
            (unsigned int)frame_len);
    return 0;
}

static int mesh_request_route(uint64_t target_id, const char *reason)
{
    struct mesh_outbound route_req;
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    ret = mesh_relay_build_route_request(&mesh_runtime,
                                         target_id,
                                         &route_req,
                                         k_uptime_get_32());
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("mesh route discovery request: target=0x%016llx reason=%s",
            (unsigned long long)target_id,
            reason);
    return mesh_send_outbound(&route_req, "route-request");
}

static void mesh_store_route_waiting_tx(const struct mesh_outbound *out)
{
    if (out == NULL || DEVICE_ROLE != ROLE_GATEWAY || out->packet.msg_type != MSG_COMMAND) {
        return;
    }

    mesh_route_waiting_tx = *out;
    mesh_route_waiting_tx_valid = true;
}

static void mesh_clear_route_waiting_tx(const struct proto_packet *packet)
{
    if (!mesh_route_waiting_tx_valid || packet == NULL) {
        return;
    }
    if (mesh_route_waiting_tx.packet.dst_id == packet->dst_id &&
        mesh_route_waiting_tx.packet.session_id == packet->session_id &&
        mesh_route_waiting_tx.packet.seq == packet->seq) {
        mesh_route_waiting_tx_valid = false;
    }
}

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound pending;
    int ret;

    if (!mesh_route_waiting_tx_valid || mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    pending = mesh_route_waiting_tx;
    ret = mesh_start_tracked_tx(&pending, "route-discovered-command");
    if (ret == 0) {
        mesh_route_waiting_tx_valid = false;
    } else if (ret == -EHOSTUNREACH) {
        (void)mesh_request_route(pending.packet.dst_id, "route-waiting-command");
    }
}

static int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    struct mesh_outbound tx;
    int ret;

    ret = mesh_relay_start_tx(&mesh_runtime,
                              &out->packet,
                              out->payload,
                              out->payload_len,
                              k_uptime_get_32(),
                              &tx);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh could not start tracked TX for %s: %d", reason, ret);
        if (ret == PROTO_ERR_NOT_FOUND) {
            mesh_store_route_waiting_tx(out);
            (void)mesh_request_route(out->packet.dst_id, reason);
        }
        return mesh_errno_from_proto(ret);
    }
    ret = mesh_send_outbound(&tx, reason);
    if (ret < 0) {
        mesh_relay_cancel_tx(&mesh_runtime);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            mesh_store_route_waiting_tx(out);
            (void)mesh_request_route(out->packet.dst_id, reason);
        }
        return ret;
    }
    mesh_schedule_tx_timeout();
    return 0;
}

static bool anchor_uwb_window_active(void)
{
    k_spinlock_key_t key;
    bool busy;

    key = k_spin_lock(&anchor_ble_lock);
    busy = anchor_uwb_busy || anchor_service_active;
    k_spin_unlock(&anchor_ble_lock, key);
    return busy;
}

static void report_tx_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        (void)k_work_reschedule(&report_tx_work, K_MSEC(delay_ms));
    }
}

static void report_tx_work_handler(struct k_work *work)
{
    struct mesh_outbound outbound;
    struct mesh_outbound dropped;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (anchor_uwb_window_active() || mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    ret = k_msgq_peek(&report_tx_msgq, &outbound);
    if (ret != 0) {
        return;
    }

    ret = mesh_start_tracked_tx(&outbound, "queued-click-report");
    if (ret == 0) {
        (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
        return;
    }

    if (ret == -EHOSTUNREACH || ret == -EBUSY) {
        LOG_WRN("queued click report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
    LOG_WRN("queued click report dropped after permanent TX error: ret=%d", ret);
}

static int queue_anchor_report(const struct mesh_outbound *outbound)
{
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }

    ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
    if (ret != 0) {
        LOG_WRN("anchor report queue full; click report dropped");
        return -ENOSPC;
    }

    LOG_INF("anchor queued click report: queue_depth=%u",
            k_msgq_num_used_get(&report_tx_msgq));
    if (!anchor_uwb_window_active()) {
        report_tx_schedule(0u);
    }
    return 0;
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result)
{
    if (result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) {
        if (MESH_ACK_REPLY_DELAY_MS > 0u) {
            k_msleep(MESH_ACK_REPLY_DELAY_MS);
        }
        (void)mesh_send_outbound(&result->hop_ack, "hop-ack");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        (void)mesh_start_tracked_tx(&result->gateway_ack, "gateway-ack");
    }
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        (void)mesh_start_tracked_tx(&result->forward, "forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_STATUS) {
        (void)mesh_start_tracked_tx(&result->route_status, "route-status");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_ADV) {
        (void)mesh_send_outbound(&result->route_adv, "route-adv");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_send_outbound(&result->route_request, "route-request-forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        (void)mesh_send_outbound(&result->route_reply, "route-reply");
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        (void)mesh_send_outbound(&result->retransmit, "retransmit");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        LOG_WRN("mesh route discovery needed after delivery failure");
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        LOG_INF("mesh reactive route ready");
        mesh_try_route_waiting_tx();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_CONFIRMED) {
        LOG_INF("mesh pending TX hop acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        LOG_INF("mesh pending TX gateway acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (DEVICE_ROLE == ROLE_GATEWAY && !mesh_relay_tx_active(&mesh_runtime)) {
        mesh_try_route_waiting_tx();
    }
}

static void mesh_rx_work_handler(struct k_work *work)
{
    struct mesh_rx_pending pending;
    struct mesh_relay_result result;
    int ret;

    ARG_UNUSED(work);

    while (k_msgq_get(&mesh_rx_msgq, &pending, K_NO_WAIT) == 0) {
        ret = mesh_relay_handle_rx(&mesh_runtime,
                                   &pending.packet,
                                   pending.payload,
                                   pending.payload_len,
                                   pending.previous_hop_id,
                                   pending.link_quality,
                                   k_uptime_get_32(),
                                   &result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            continue;
        }

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id,
                (unsigned long long)pending.previous_hop_id,
                result.actions,
                result.status);
        if (result.status == PROTO_ERR_NOT_FOUND &&
            pending.packet.dst_id != DEVICE_ID &&
            pending.packet.dst_id != MESH_BROADCAST_ID) {
            (void)mesh_request_route(pending.packet.dst_id, "rx-forward-miss");
        }
        mesh_handle_result_actions(&result);
        if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY) {
            if (pending.packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending.packet);
            }
            ret = gateway_emit_serial_packet(&pending.packet,
                                             pending.payload,
                                             pending.payload_len);
            if (ret < 0) {
                LOG_WRN("gateway USB COBS frame not emitted: %d", ret);
            }
        } else if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                   DEVICE_ROLE == ROLE_ANCHOR) {
            anchor_handle_local_command(&pending.packet, pending.payload, pending.payload_len);
        }
    }
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result result;
    struct proto_packet pending_packet = {0};
    uint8_t pending_payload[PACKET_MAX_PAYLOAD_LEN];
    size_t pending_payload_len = 0u;
    bool pending_gateway_command = false;
    struct mesh_outbound pending_report = {0};
    bool pending_anchor_report = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE == ROLE_ANCHOR &&
        mesh_relay_tx_active(&mesh_runtime) &&
        mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
        mesh_runtime.pending.packet.src_id == DEVICE_ID) {
        pending_report.packet = mesh_runtime.pending.packet;
        pending_report.payload_len = mesh_runtime.pending.payload_len;
        if (pending_report.payload_len > 0u) {
            memcpy(pending_report.payload,
                   mesh_runtime.pending.payload,
                   pending_report.payload_len);
        }
        pending_anchor_report = true;
    }

    if (DEVICE_ROLE == ROLE_GATEWAY &&
        mesh_relay_tx_active(&mesh_runtime) &&
        mesh_runtime.pending.packet.msg_type == MSG_COMMAND &&
        mesh_runtime.pending.packet.src_id == DEVICE_ID) {
        pending_packet = mesh_runtime.pending.packet;
        pending_payload_len = mesh_runtime.pending.payload_len;
        if (pending_payload_len > 0u) {
            memcpy(pending_payload, mesh_runtime.pending.payload, pending_payload_len);
        }
        pending_gateway_command = true;
    }

    if (mesh_relay_tick(&mesh_runtime, k_uptime_get_32(), &result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(&result);

    if (pending_gateway_command &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        struct mesh_outbound waiting = {
            .packet = pending_packet,
            .payload_len = (uint8_t)pending_payload_len,
        };

        if (pending_payload_len > 0u) {
            memcpy(waiting.payload, pending_payload, pending_payload_len);
        }
        mesh_store_route_waiting_tx(&waiting);
        (void)mesh_request_route(pending_packet.dst_id, "gateway-command-timeout");
    }

    if (pending_anchor_report &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        LOG_WRN("requeueing click report after mesh route loss");
        (void)queue_anchor_report(&pending_report);
    }
}

static bool mesh_queue_parsed(const struct mesh_frame_parse_context *context,
                              const bt_addr_le_t *addr,
                              int8_t rssi)
{
    struct mesh_rx_pending pending = {0};
    int ret;

    if (context == NULL || !context->found || context->payload_len > UINT8_MAX) {
        return false;
    }

    pending.packet = context->packet;
    if (context->payload_len > 0u) {
        memcpy(pending.payload, context->payload, context->payload_len);
    }
    pending.payload_len = (uint8_t)context->payload_len;
    pending.previous_hop_id = context->previous_hop_id;
    pending.link_quality = mesh_quality_from_rssi(rssi);
    mesh_note_peer_addr(context->previous_hop_id, addr);

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("mesh RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return true;
    }

    (void)k_work_submit(&mesh_rx_work);
    return true;
}

static bool mesh_queue_from_frame(const uint8_t *frame, size_t frame_len, uint8_t link_quality)
{
    struct mesh_frame_parse_context context = {0};
    struct mesh_rx_pending pending = {0};
    int ret;

    if (frame == NULL || frame_len == 0u) {
        return false;
    }
    if (mesh_ble_frame_decode(frame,
                              frame_len,
                              DEVICE_ID,
                              &context.previous_hop_id,
                              &context.packet,
                              context.payload,
                              sizeof(context.payload),
                              &context.payload_len) != PROTO_OK ||
        context.payload_len > UINT8_MAX) {
        return false;
    }

    pending.packet = context.packet;
    if (context.payload_len > 0u) {
        memcpy(pending.payload, context.payload, context.payload_len);
    }
    pending.payload_len = (uint8_t)context.payload_len;
    pending.previous_hop_id = context.previous_hop_id;
    pending.link_quality = link_quality;

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("mesh connection RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return false;
    }

    (void)k_work_submit(&mesh_rx_work);
    return true;
}

static bool mesh_queue_from_scan(struct net_buf_simple *buf,
                                 const bt_addr_le_t *addr,
                                 int8_t rssi)
{
    struct mesh_frame_parse_context context = {0};

    bt_data_parse(buf, parse_mesh_ad, &context);
    return mesh_queue_parsed(&context, addr, rssi);
}

static int build_range_report_samples(uint64_t clicker_id,
                                      uint32_t event_seq,
                                      const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      uint16_t sample_count)
{
    struct range_report_fields fields;
    struct proto_packet packet;
    uint8_t payload[MESH_BLE_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    uint16_t sample_index = 0u;
    uint16_t packet_index = 0u;
    bool fragmented;
    int ret;

    if (range_result == NULL ||
        clicker_id == 0u ||
        event_seq == 0u ||
        range_result->responder_id == 0u ||
        (sample_count > 0u && distance_samples_mm == NULL) ||
        sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return -EINVAL;
    }
    fragmented = sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;

    do {
        size_t payload_len = 0u;
        size_t encoded_len = 0u;
        uint16_t chunk_count = 0u;
        uint16_t packet_seq;

        if (sample_count > 0u) {
            uint16_t chunk_cap = fragmented ?
                                 RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT :
                                 RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;

            chunk_count = MIN(chunk_cap, sample_count - sample_index);
        }

        fields.clicker_id = clicker_id;
        fields.anchor_id = range_result->responder_id;
        fields.event_seq = event_seq;
        fields.timestamp_ms = k_uptime_get_32();
        fields.distance_mm = range_result->distance_mm;
        fields.quality = range_result->quality;
        fields.rsl_dbm = range_result->rsl_dbm;
        fields.range_status = range_result->status;
        fields.distance_samples_mm = chunk_count > 0u ? &distance_samples_mm[sample_index] : NULL;
        fields.sample_index = sample_index;
        fields.sample_count = sample_count;
        fields.distance_sample_count = chunk_count;
        fields.omit_rsl = packet_index != 0u;

        ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        packet_seq = (uint16_t)((range_result->seq == 0u ?
                                 (uint16_t)event_seq :
                                 range_result->seq) + packet_index);
        ret = report_init_click_packet(&packet,
                                       range_result->responder_id,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       (uint8_t)payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        LOG_INF("click report ready: clicker=0x%016llx event_seq=%u anchor=0x%016llx distance_mm=%d samples=%u chunk_index=%u chunk_samples=%u quality=%u rsl_included=%u packet_len=%u",
                (unsigned long long)clicker_id,
                event_seq,
                (unsigned long long)range_result->responder_id,
                range_result->distance_mm,
                sample_count,
                sample_index,
                chunk_count,
                range_result->quality,
                packet_index == 0u ? 1u : 0u,
                (unsigned int)encoded_len);

        if (DEVICE_ROLE == ROLE_ANCHOR) {
            struct mesh_outbound outbound = {
                .packet = packet,
                .payload_len = (uint8_t)payload_len,
            };

            memcpy(outbound.payload, payload, payload_len);
            ret = queue_anchor_report(&outbound);
            if (ret < 0) {
                LOG_WRN("click report could not be queued for mesh TX: %d", ret);
                return ret;
            }
        }

        sample_index += chunk_count;
        packet_index++;
    } while (sample_index < sample_count);

    return 0;
}

static int build_range_report(uint64_t clicker_id,
                              uint32_t event_seq,
                              const struct dwm3000_range_result *range_result)
{
    int32_t distance_sample;
    const int32_t *distance_samples = NULL;
    uint16_t sample_count = 0u;

    if (range_result != NULL && range_result->status == RANGE_OK) {
        distance_sample = range_result->distance_mm;
        distance_samples = &distance_sample;
        sample_count = 1u;
    }

    return build_range_report_samples(clicker_id,
                                      event_seq,
                                      range_result,
                                      distance_samples,
                                      sample_count);
}

static bool anchor_id_seen(const uint64_t *anchors, uint8_t anchor_count, uint64_t anchor_id)
{
    for (uint8_t i = 0u; i < anchor_count; i++) {
        if (anchors[i] == anchor_id) {
            return true;
        }
    }
    return false;
}

static uint32_t ds_twr_retry_backoff_ms(uint64_t anchor_id, uint8_t seq)
{
    uint32_t jitter = k_cycle_get_32() ^
                      (uint32_t)anchor_id ^
                      (uint32_t)(anchor_id >> 32) ^
                      (uint32_t)seq;
    uint32_t span = DS_TWR_RETRY_BACKOFF_MAX_MS - DS_TWR_RETRY_BACKOFF_MIN_MS + 1u;

    return DS_TWR_RETRY_BACKOFF_MIN_MS + (jitter % span);
}

static int clicker_politeness_phase(void)
{
    int64_t deadline_ms = k_uptime_get() + MAX_POLITENESS_WAIT_MS;
    uint32_t quiet_ms = 0u;
    int ret;

    ret = radio_guard_uwb_start("clicker politeness sniff");
    if (ret < 0) {
        return ret;
    }

    while (quiet_ms < UWB_QUIET_TIME_MS && k_uptime_get() < deadline_ms) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        uint32_t listen_ms;
        bool activity_detected = false;

        if (remaining_ms <= 0) {
            break;
        }
        listen_ms = MIN(UWB_QUIET_TIME_MS - quiet_ms, (uint32_t)remaining_ms);
        if (listen_ms == 0u) {
            break;
        }

        ret = dwm3000_driver_listen_activity(listen_ms, &activity_detected);
        if (ret < 0) {
            (void)dwm3000_driver_standby();
            radio_guard_uwb_stop();
            return ret;
        }

        if (activity_detected) {
            quiet_ms = 0u;
        } else {
            quiet_ms += listen_ms;
        }
    }

    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    LOG_INF("clicker politeness phase complete: quiet_ms=%u max_wait_ms=%u",
            quiet_ms,
            MAX_POLITENESS_WAIT_MS);
    return 0;
}

static int range_ready_anchor_for_window(const struct ble_discovery_req *request,
                                         const struct ready_anchor *anchor,
                                         int64_t click_deadline_ms,
                                         uint8_t *next_seq,
                                         uint8_t *attempted_count,
                                         bool *anchor_succeeded)
{
    int64_t anchor_deadline_ms;
    int last_ret = -ETIMEDOUT;

    if (request == NULL || anchor == NULL || next_seq == NULL ||
        attempted_count == NULL || anchor_succeeded == NULL) {
        return -EINVAL;
    }

    *anchor_succeeded = false;
    anchor_deadline_ms = MIN(k_uptime_get() + CLICK_ANCHOR_RANGE_WINDOW_MS,
                             click_deadline_ms - CLICK_REPORT_BUILD_GUARD_MS);

    while (k_uptime_get() < anchor_deadline_ms) {
        struct dwm3000_range_request range_request = {
            .initiator_id = DEVICE_ID,
            .responder_id = anchor->ready.anchor_id,
            .responder_short_addr = anchor->ready.uwb_short_addr,
            .session_id = request->event_seq,
            .seq = *next_seq,
            .flags = request->flags,
        };
        struct dwm3000_range_result range_result;
        int64_t remaining_ms = anchor_deadline_ms - k_uptime_get();
        int ret;

        if (remaining_ms <= 0) {
            break;
        }
        if (range_request.seq == 0u) {
            range_request.seq = 1u;
            *next_seq = 1u;
        }
        range_request.timeout_ms = MIN(CLICK_UWB_TIMEOUT_MS, (uint32_t)remaining_ms);

        ret = radio_guard_uwb_start("clicker READY UWB range");
        if (ret < 0) {
            return ret;
        }
        (*attempted_count)++;
        LOG_INF("normal click DS-TWR start: anchor=0x%016llx seq=%u timeout_ms=%u anchor_window_remaining_ms=%d BLE RF is off",
                (unsigned long long)anchor->ready.anchor_id,
                range_request.seq,
                range_request.timeout_ms,
                (int)(anchor_deadline_ms - k_uptime_get()));

        ret = dwm3000_driver_range_initiator(&range_request, &range_result);
        (void)dwm3000_driver_standby();
        radio_guard_uwb_stop();
        (*next_seq)++;
        if (*next_seq == 0u) {
            *next_seq = 1u;
        }

        if (ret == 0 && range_result.status == RANGE_OK) {
            int report_ret;

            report_ret = build_range_report(DEVICE_ID, request->event_seq, &range_result);
            if (report_ret < 0) {
                LOG_WRN("normal click report build failed: anchor=0x%016llx ret=%d",
                        (unsigned long long)range_result.responder_id,
                        report_ret);
                last_ret = report_ret;
            } else {
                *anchor_succeeded = true;
                last_ret = 0;
            }
            continue;
        }

        LOG_WRN("normal click DS-TWR failed: anchor=0x%016llx ret=%d range_status=%u",
                (unsigned long long)anchor->ready.anchor_id,
                ret,
                range_result.status);
        last_ret = ret < 0 ? ret : -EIO;

        remaining_ms = anchor_deadline_ms - k_uptime_get();
        if (remaining_ms > (int64_t)DS_TWR_RETRY_BACKOFF_MIN_MS) {
            uint32_t backoff_ms = ds_twr_retry_backoff_ms(anchor->ready.anchor_id,
                                                          range_request.seq);

            backoff_ms = MIN(backoff_ms, (uint32_t)remaining_ms);
            k_msleep(backoff_ms);
        }
    }

    return *anchor_succeeded ? 0 : last_ret;
}

static int run_normal_click(void)
{
    uint32_t event_seq = ++next_event_seq;
    uint64_t successful_anchors[MAX_SUCCESSFUL_ANCHORS] = {0};
    uint8_t success_count = 0u;
    uint8_t attempted_count = 0u;
    uint8_t next_seq = 1u;
    uint16_t total_ready_count = 0u;
    int64_t click_deadline_ms;
    int last_ret = -ETIMEDOUT;
    int ret;

    BUILD_ASSERT(MIN_UNIQUE_RANGED_ANCHORS <= MAX_SUCCESSFUL_ANCHORS,
                 "successful anchor result storage must cover the success threshold");

    LOG_INF("normal click started: event_seq=%u wake_ms=%u ready_scan_ms=%u max_attempts=%u min_unique_anchors=%u",
            event_seq,
            WAKE_ADV_MS,
            READY_SCAN_MS,
            MAX_WAKE_ATTEMPTS,
            MIN_UNIQUE_RANGED_ANCHORS);

    click_deadline_ms = k_uptime_get() + CLICK_REPORT_READY_DEADLINE_MS;
    ret = clicker_politeness_phase();
    if (ret < 0) {
        LOG_WRN("normal click politeness sniff failed: %d", ret);
        return ret;
    }

    for (uint8_t attempt = 1u; attempt <= MAX_WAKE_ATTEMPTS; attempt++) {
        struct ble_discovery_req request = {
            .clicker_id = DEVICE_ID,
            .priority_id = DEVICE_ID,
            .event_seq = event_seq,
            .ready_scan_starts_in_ms = WAKE_ADV_MS,
            .ready_scan_duration_ms = READY_SCAN_MS,
            .flags = ble_flags_for_click(),
            .attempt_index = attempt,
            .min_anchor_count = MIN_UNIQUE_RANGED_ANCHORS,
        };
        struct ready_anchor anchors[MAX_READY_ANCHORS];
        int ready_count;

        if (k_uptime_get() + WAKE_ADV_MS + READY_SCAN_MS >= click_deadline_ms) {
            break;
        }

        ready_count = clicker_collect_ready_attempt(&request, anchors, ARRAY_SIZE(anchors));
        if (ready_count == -ETIMEDOUT) {
            last_ret = ready_count;
            LOG_WRN("normal click attempt found no READY anchors: event_seq=%u attempt=%u",
                    event_seq,
                    attempt);
            if (attempt < MAX_WAKE_ATTEMPTS) {
                int64_t retry_delay_ms = MIN((int64_t)NO_ANCHOR_RETRY_DELAY_MS,
                                             click_deadline_ms - k_uptime_get());

                if (retry_delay_ms > 0) {
                    k_msleep((uint32_t)retry_delay_ms);
                }
            }
            continue;
        }
        if (ready_count < 0) {
            return ready_count;
        }

        total_ready_count += (uint16_t)ready_count;
        LOG_INF("normal click attempt received READY anchors: event_seq=%u attempt=%u ready=%d unique_success=%u/%u",
                event_seq,
                attempt,
                ready_count,
                success_count,
                MIN_UNIQUE_RANGED_ANCHORS);

        for (uint8_t i = 0u; i < (uint8_t)ready_count; i++) {
            bool anchor_succeeded = false;

            if (anchor_id_seen(successful_anchors, success_count, anchors[i].ready.anchor_id)) {
                continue;
            }
            if (k_uptime_get() >= click_deadline_ms - CLICK_REPORT_BUILD_GUARD_MS) {
                break;
            }

            ret = range_ready_anchor_for_window(&request,
                                                &anchors[i],
                                                click_deadline_ms,
                                                &next_seq,
                                                &attempted_count,
                                                &anchor_succeeded);
            if (anchor_succeeded) {
                successful_anchors[success_count] = anchors[i].ready.anchor_id;
                success_count++;
                LOG_INF("normal click unique anchor ranged: anchor=0x%016llx success=%u/%u",
                        (unsigned long long)anchors[i].ready.anchor_id,
                        success_count,
                        MIN_UNIQUE_RANGED_ANCHORS);
                if (success_count >= MIN_UNIQUE_RANGED_ANCHORS) {
                    LOG_INF("normal click completed: event_seq=%u ready_seen=%u attempted_ranges=%u successful_unique_ranges=%u",
                            event_seq,
                            total_ready_count,
                            attempted_count,
                            success_count);
                    return 0;
                }
            } else if (ret < 0) {
                last_ret = ret;
            }
        }
    }

    LOG_WRN("normal click failed: event_seq=%u ready_seen=%u attempted_ranges=%u successful_unique_ranges=%u required=%u",
            event_seq,
            total_ready_count,
            attempted_count,
            success_count,
            MIN_UNIQUE_RANGED_ANCHORS);
    return last_ret < 0 ? last_ret : -EIO;
}

static enum self_test_failure run_self_test(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = DEVICE_ID,
        .priority_id = DEVICE_ID,
        .event_seq = ++next_event_seq,
        .ready_scan_starts_in_ms = WAKE_ADV_MS,
        .ready_scan_duration_ms = READY_SCAN_MS,
        .flags = ble_flags_for_diagnostic(),
        .attempt_index = 1u,
        .min_anchor_count = 1u,
    };
    struct dwm3000_range_request range_request;
    struct dwm3000_range_result range_result;
    struct ready_anchor anchors[MAX_READY_ANCHORS];
    int ready_count;
    uint32_t dev_id;
    int ret;

    LOG_INF("self-test started: event_seq=%u", request.event_seq);

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 port init failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 wake failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 reset failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 decadriver DEV_ID probe failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 fast SPI config failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    LOG_INF("self-test DWM3000 decadriver DEV_ID=0x%08x; fast SPI config checked at %u Hz",
            dev_id,
            (unsigned int)dwm3000_port_current_spi_hz());
    (void)dwm3000_driver_standby();

    ready_count = clicker_collect_ready_attempt(&request, anchors, ARRAY_SIZE(anchors));
    if (ready_count < 0) {
        LOG_WRN("self-test did not receive diagnostic READY over BLE: %d", ready_count);
        return ready_count == -ETIMEDOUT ? SELF_TEST_FAILURE_NO_ANCHOR :
                                           SELF_TEST_FAILURE_BLE;
    }

    LOG_INF("self-test using first READY anchor: anchor=0x%016llx score=%d discovered=%d",
            (unsigned long long)anchors[0].ready.anchor_id,
            anchors[0].rssi_score,
            ready_count);

    range_request.initiator_id = DEVICE_ID;
    range_request.responder_id = anchors[0].ready.anchor_id;
    range_request.responder_short_addr = anchors[0].ready.uwb_short_addr;
    range_request.session_id = request.event_seq;
    range_request.seq = 1u;
    range_request.flags = request.flags;
    range_request.timeout_ms = SELF_TEST_UWB_TIMEOUT_MS;

    ret = radio_guard_uwb_start("self-test READY UWB range");
    if (ret < 0) {
        return SELF_TEST_FAILURE_UWB;
    }
    ret = dwm3000_driver_range_initiator(&range_request, &range_result);
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ret == 0 && range_result.status == RANGE_OK) {
        LOG_INF("self-test UWB dud range passed: anchor=0x%016llx distance_mm=%d quality=%u rsl=%d dBm",
                (unsigned long long)range_result.responder_id,
                range_result.distance_mm,
                range_result.quality,
                range_result.rsl_dbm);
        return SELF_TEST_FAILURE_NONE;
    }

    LOG_WRN("self-test UWB dud range failed: ret=%d range_status=%u", ret, range_result.status);
    if (ret == -ETIMEDOUT || range_result.status == RANGE_RX_TIMEOUT) {
        return SELF_TEST_FAILURE_NO_ANCHOR;
    }
    return SELF_TEST_FAILURE_UWB;
}

static void handle_button_action(enum button_action action)
{
    struct status_inputs status = {0};
    enum self_test_failure failure;
    int ret;

    switch (action) {
    case BUTTON_ACTION_NORMAL_CLICK:
        ret = run_normal_click();
        if (DEVICE_ROLE == ROLE_CLICKER) {
            ble_runtime_shutdown();
        }
        status.click_accepted = ret == 0;
        status_apply(&status);
        if (ret == 0) {
            LOG_INF("normal click MVP completed");
        } else {
            LOG_WRN("normal click MVP failed: %d", ret);
        }
        break;
    case BUTTON_ACTION_SELF_TEST_ARMED:
        status.self_test_armed = true;
        status_apply(&status);
#if HAS_CLICK_BUTTON
        (void)k_work_reschedule(&self_test_arm_timeout_work, K_MSEC(SELF_TEST_ARM_WINDOW_MS + 1u));
#endif
        LOG_INF("self-test armed");
        break;
    case BUTTON_ACTION_SELF_TEST_START:
#if HAS_CLICK_BUTTON
        (void)k_work_cancel_delayable(&self_test_arm_timeout_work);
#endif
        status.self_test_running = true;
        status_apply(&status);
        failure = run_self_test();
        if (DEVICE_ROLE == ROLE_CLICKER) {
            ble_runtime_shutdown();
        }
        status.self_test_running = false;
        status.failure = failure;
        status.self_test_passed = failure == SELF_TEST_FAILURE_NONE;
        status_apply(&status);
        break;
    case BUTTON_ACTION_SELF_TEST_CANCELLED:
        status_apply(&status);
        LOG_INF("self-test arm cancelled");
        break;
    case BUTTON_ACTION_NONE:
    default:
        break;
    }
}

#if HAS_CLICK_BUTTON
static void click_button_work_handler(struct k_work *work)
{
    enum button_action action;
    enum button_signal signal;
    int value;

    ARG_UNUSED(work);

    value = gpio_pin_get_dt(&click_button);
    if (value < 0) {
        LOG_ERR("failed to read click button: %d", value);
        return;
    }

    signal = value != 0 ? BUTTON_SIGNAL_PRESS : BUTTON_SIGNAL_RELEASE;
    if (button_fsm_handle(&button_fsm, signal, k_uptime_get_32(), &action) != PROTO_OK) {
        LOG_ERR("button FSM rejected signal");
        return;
    }
    handle_button_action(action);
}

static void click_button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    (void)k_work_submit(&click_button_work);
}

static void self_test_arm_timeout_handler(struct k_work *work)
{
    enum button_action action;

    ARG_UNUSED(work);

    if (button_fsm_handle(&button_fsm, BUTTON_SIGNAL_TICK,
                               k_uptime_get_32(), &action) == PROTO_OK) {
        handle_button_action(action);
    }
}

static int click_button_init(void)
{
    int ret;

    if (!gpio_is_ready_dt(&click_button)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&click_button, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&click_button, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        return ret;
    }

    k_work_init(&click_button_work, click_button_work_handler);
    k_work_init_delayable(&self_test_arm_timeout_work, self_test_arm_timeout_handler);
    gpio_init_callback(&click_button_cb, click_button_isr, BIT(click_button.pin));
    return gpio_add_callback(click_button.port, &click_button_cb);
}
#else
static int click_button_init(void)
{
    return -ENODEV;
}
#endif

int main(void)
{
    int ret;

    ret = debug_serial_init();
    if (ret < 0) {
        printk("USB serial debug init failed: %d\n", ret);
    } else {
        printk("USB serial debug active; firmware booting\n");
    }

    button_fsm_init(&button_fsm);
    mesh_neighbors_init();
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
    LOG_INF("UWB/BLE firmware starting as %s", role_name());
    LOG_INF("runtime config: device_id=0x%016llx gateway_id=0x%016llx max_ready=%u wake_ms=%u ready_scan_ms=%u max_attempts=%u min_unique_anchors=%u anchor_scan_interval=%u anchor_scan_window=%u anchor_uwb_wait_ms=%u",
            (unsigned long long)DEVICE_ID,
            (unsigned long long)GATEWAY_ID,
            MAX_READY_ANCHORS,
            WAKE_ADV_MS,
            READY_SCAN_MS,
            MAX_WAKE_ATTEMPTS,
            MIN_UNIQUE_RANGED_ANCHORS,
            ANCHOR_SCAN_INTERVAL_300MS,
            ANCHOR_SCAN_WINDOW_30MS,
            ANCHOR_UWB_WAIT_MS);

    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
    }

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_WRN("DWM3000 low-power pin park unavailable: %d", ret);
    } else {
        LOG_INF("DWM3000 wake pin parked inactive; radio init waits for BLE-triggered UWB");
    }

    if (DEVICE_ROLE == ROLE_CLICKER) {
        ret = click_button_init();
        if (ret < 0) {
            LOG_WRN("click button unavailable: %d", ret);
        }
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        mesh_relay_init(&mesh_runtime,
                        MESH_RELAY_ROLE_ANCHOR,
                        DEVICE_ID,
                        GATEWAY_ID,
                        1u);
        k_work_init(&anchor_ble_work, anchor_discovery_work_handler);
        ret = anchor_start_ble_scan();
        if (ret < 0) {
            LOG_ERR("anchor BLE scan unavailable: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        mesh_relay_init(&mesh_runtime,
                        MESH_RELAY_ROLE_GATEWAY,
                        DEVICE_ID,
                        GATEWAY_ID,
                        1u);
        k_work_init_delayable(&gateway_serial_rx_work, gateway_serial_rx_work_handler);
        ret = gateway_start_mesh_scan();
        if (ret < 0) {
            LOG_ERR("gateway mesh scan unavailable: %d", ret);
        }
        (void)k_work_schedule(&gateway_serial_rx_work, K_MSEC(GATEWAY_SERIAL_POLL_MS));
        LOG_INF("gateway reactive mesh root active; USB COBS packet input/output active");
    }

    return 0;
}
