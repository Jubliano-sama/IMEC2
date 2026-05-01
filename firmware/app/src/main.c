#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "discovery.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "status.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
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

#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
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

static struct button_fsm button_fsm;
static uint32_t next_event_seq;
static bool ble_ready;
static bool anchor_scan_active;
static bool gateway_mesh_scan_active;
static struct k_work anchor_ble_work;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable gateway_route_adv_work;
static struct k_spinlock anchor_ble_lock;
static struct k_spinlock clicker_ready_lock;
static struct k_spinlock mesh_rx_lock;
static struct ble_discovery_req anchor_pending_request;
static int8_t anchor_pending_rssi;
static bool anchor_pending_valid;
static bool anchor_uwb_busy;
static struct mesh_relay mesh_runtime;
#if defined(CONFIG_BT_EXT_ADV)
static struct bt_le_ext_adv *mesh_ext_adv;
#endif

#define MAX_READY_ANCHORS 8u
#define BLE_DISCOVERY_ADV_MS 120u
#define BLE_READY_SCAN_MS 150u
#define ANCHOR_UWB_WINDOW_MS 400u
#define SELF_TEST_UWB_TIMEOUT_MS 150u
#define CLICK_UWB_TIMEOUT_MS 150u
#define ANCHOR_SCAN_INTERVAL_100MS 160u
#define ANCHOR_SCAN_WINDOW_10MS 16u
#define ANCHOR_ERROR_BACKOFF_MS 5u
#define MESH_BLE_FRAME_MAGIC 0xB7u
#define MESH_BLE_FRAME_VERSION 0x01u
#define MESH_BLE_FRAME_HEADER_LEN 20u
#define MESH_ADV_TX_MS 30u
#define GATEWAY_ROUTE_ADV_INTERVAL_MS 2000u

struct ready_anchor {
    struct ble_discovery_ready ready;
    int8_t clicker_rssi;
    int16_t rssi_score;
};

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint64_t previous_hop_id;
    uint8_t link_quality;
    bool valid;
};

static struct ready_anchor clicker_ready_anchors[MAX_READY_ANCHORS];
static uint8_t clicker_ready_count;
static uint8_t clicker_ready_expected_flags;
static struct mesh_rx_pending mesh_rx_pending;

static const struct bt_le_scan_param anchor_low_duty_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = ANCHOR_SCAN_INTERVAL_100MS,
    .window = ANCHOR_SCAN_WINDOW_10MS,
    .timeout = 0u,
    .interval_coded = 0u,
    .window_coded = 0u,
};

static const struct bt_le_scan_param gateway_mesh_scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = 16u,
    .window = 16u,
    .timeout = 0u,
    .interval_coded = 0u,
    .window_coded = 0u,
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

static int debug_serial_init(void)
{
#if defined(CONFIG_USB_DEVICE_STACK)
    int ret = usb_enable(NULL);

    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
#endif
    return 0;
}

static int ble_start_manufacturer_advertising(const uint8_t *payload, size_t payload_len)
{
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, payload, payload_len),
    };
    int ret;

    if (payload == NULL || payload_len == 0u) {
        return -EINVAL;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    ret = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
    if (ret == -EALREADY) {
        (void)bt_le_adv_stop();
        ret = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
    }

    return ret;
}

static int ble_start_extended_manufacturer_advertising(const uint8_t *payload, size_t payload_len)
{
#if defined(CONFIG_BT_EXT_ADV)
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, payload, payload_len),
    };
    int ret;

    if (payload == NULL || payload_len == 0u || payload_len > UINT8_MAX) {
        return -EINVAL;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    if (mesh_ext_adv == NULL) {
        ret = bt_le_ext_adv_create(BT_LE_EXT_ADV_NCONN, NULL, &mesh_ext_adv);
        if (ret < 0) {
            return ret;
        }
    } else {
        (void)bt_le_ext_adv_stop(mesh_ext_adv);
    }

    ret = bt_le_ext_adv_set_data(mesh_ext_adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (ret < 0) {
        return ret;
    }
    return bt_le_ext_adv_start(mesh_ext_adv, BT_LE_EXT_ADV_START_DEFAULT);
#else
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -EMSGSIZE;
#endif
}

static int ble_stop_advertising(void)
{
    int ret = bt_le_adv_stop();

    return ret == -EALREADY ? 0 : ret;
}

static int ble_advertise_manufacturer_payload(const uint8_t *payload,
                                              size_t payload_len,
                                              uint32_t duration_ms)
{
    int ret;

    ret = ble_start_manufacturer_advertising(payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    k_msleep(duration_ms);

    return ble_stop_advertising();
}

static int ble_advertise_mesh_payload(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t duration_ms)
{
    int ret;

    if (payload_len <= BT_GAP_ADV_MAX_ADV_DATA_LEN) {
        return ble_advertise_manufacturer_payload(payload, payload_len, duration_ms);
    }

    ret = ble_start_extended_manufacturer_advertising(payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    k_msleep(duration_ms);
#if defined(CONFIG_BT_EXT_ADV)
    ret = bt_le_ext_adv_stop(mesh_ext_adv);
    return ret == -EALREADY ? 0 : ret;
#else
    return 0;
#endif
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

static int mesh_frame_encode(const struct mesh_outbound *out,
                             uint8_t *frame,
                             size_t frame_cap,
                             size_t *frame_len)
{
    size_t packet_len = 0u;
    int ret;

    if (out == NULL || frame == NULL || frame_len == NULL ||
        frame_cap < MESH_BLE_FRAME_HEADER_LEN) {
        return -EINVAL;
    }

    frame[0] = MESH_BLE_FRAME_MAGIC;
    frame[1] = MESH_BLE_FRAME_VERSION;
    proto_put_u64_le(&frame[2], DEVICE_ID);
    proto_put_u64_le(&frame[10], out->next_hop_id);

    ret = proto_packet_encode(&out->packet,
                              out->payload,
                              &frame[MESH_BLE_FRAME_HEADER_LEN],
                              frame_cap - MESH_BLE_FRAME_HEADER_LEN,
                              &packet_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (packet_len > UINT16_MAX) {
        return -EMSGSIZE;
    }

    proto_put_u16_le(&frame[18], (uint16_t)packet_len);
    *frame_len = MESH_BLE_FRAME_HEADER_LEN + packet_len;
    return 0;
}

static int mesh_frame_decode(const uint8_t *frame,
                             size_t frame_len,
                             uint64_t *previous_hop_id,
                             struct proto_packet *packet,
                             uint8_t *payload,
                             uint8_t *payload_len)
{
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    uint64_t next_hop_id;
    uint16_t packet_len;
    int ret;

    if (frame == NULL || previous_hop_id == NULL || packet == NULL ||
        payload == NULL || payload_len == NULL ||
        frame_len < MESH_BLE_FRAME_HEADER_LEN ||
        frame[0] != MESH_BLE_FRAME_MAGIC ||
        frame[1] != MESH_BLE_FRAME_VERSION) {
        return -EINVAL;
    }

    *previous_hop_id = proto_get_u64_le(&frame[2]);
    next_hop_id = proto_get_u64_le(&frame[10]);
    packet_len = proto_get_u16_le(&frame[18]);
    if (*previous_hop_id == 0u ||
        (next_hop_id != MESH_BROADCAST_ID && next_hop_id != DEVICE_ID) ||
        packet_len != frame_len - MESH_BLE_FRAME_HEADER_LEN) {
        return -EINVAL;
    }

    ret = proto_packet_decode(&frame[MESH_BLE_FRAME_HEADER_LEN],
                              packet_len,
                              packet,
                              &decoded_payload,
                              &decoded_payload_len);
    if (ret != PROTO_OK || decoded_payload_len > PACKET_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }

    if (decoded_payload_len > 0u) {
        memcpy(payload, decoded_payload, decoded_payload_len);
    }
    *payload_len = (uint8_t)decoded_payload_len;
    return 0;
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
    uint8_t payload_len;
};

static bool parse_discovery_ad(struct bt_data *data, void *user_data)
{
    struct anchor_scan_parse_context *context = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }

    if (ble_discovery_req_decode(data->data, data->data_len,
                                      &context->request) == PROTO_OK) {
        context->found = true;
        return false;
    }

    return true;
}

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

    if (mesh_frame_decode(data->data,
                          data->data_len,
                          &context->previous_hop_id,
                          &context->packet,
                          context->payload,
                          &context->payload_len) == 0) {
        context->found = true;
        return false;
    }

    return true;
}

static int anchor_start_ble_scan(void);
static int gateway_start_mesh_scan(void);
static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
static bool mesh_queue_from_scan(struct net_buf_simple *buf, int8_t rssi);
static int build_range_report(uint64_t clicker_id,
                              uint32_t event_seq,
                              const struct dwm3000_range_result *range_result);

static int16_t ready_rssi_score(const struct ble_discovery_ready *ready,
                                int8_t clicker_rssi)
{
    return (int16_t)ready->rssi_hint + (int16_t)clicker_rssi;
}

static void sort_ready_anchors_by_rssi(void)
{
    for (uint8_t i = 1u; i < clicker_ready_count; i++) {
        struct ready_anchor candidate = clicker_ready_anchors[i];
        uint8_t j = i;

        while (j > 0u && candidate.rssi_score > clicker_ready_anchors[j - 1u].rssi_score) {
            clicker_ready_anchors[j] = clicker_ready_anchors[j - 1u];
            j--;
        }
        clicker_ready_anchors[j] = candidate;
    }
}

static bool ready_anchor_store_locked(const struct ble_discovery_ready *ready,
                                      int8_t clicker_rssi)
{
    struct ready_anchor candidate = {
        .ready = *ready,
        .clicker_rssi = clicker_rssi,
        .rssi_score = ready_rssi_score(ready, clicker_rssi),
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

    if (candidate.rssi_score > clicker_ready_anchors[MAX_READY_ANCHORS - 1u].rssi_score) {
        clicker_ready_anchors[MAX_READY_ANCHORS - 1u] = candidate;
        sort_ready_anchors_by_rssi();
        return true;
    }

    return false;
}

static void anchor_discovery_work_handler(struct k_work *work)
{
    struct ble_discovery_req request;
    struct ble_discovery_ready ready = {
        .anchor_id = DEVICE_ID,
        .uwb_short_addr = local_uwb_short_addr(),
        .flags = 0u,
        .rssi_hint = 0,
    };
    uint8_t payload[BLE_DISCOVERY_READY_LEN];
    size_t payload_len = 0u;
    k_spinlock_key_t key;
    struct dwm3000_range_result range_result;
    int64_t deadline_ms;
    uint8_t handled_ranges = 0u;
    int8_t rssi;
    int ret;

    ARG_UNUSED(work);

    key = k_spin_lock(&anchor_ble_lock);
    request = anchor_pending_request;
    rssi = anchor_pending_rssi;
    anchor_pending_valid = false;
    anchor_uwb_busy = true;
    k_spin_unlock(&anchor_ble_lock, key);

    if (anchor_scan_active) {
        ret = bt_le_scan_stop();
        if (ret < 0 && ret != -EALREADY) {
            LOG_WRN("failed to stop anchor BLE scan: %d", ret);
        }
        anchor_scan_active = false;
    }

    ready.flags = request.flags;
    ready.rssi_hint = rssi;
    if (ble_discovery_ready_encode(&ready, payload, sizeof(payload),
                                        &payload_len) == PROTO_OK) {
        ret = ble_start_manufacturer_advertising(payload, payload_len);
        if (ret < 0) {
            LOG_WRN("anchor READY advertisement failed: %d", ret);
        }
    }

    LOG_INF("anchor accepted BLE request: clicker=0x%016llx event_seq=%u flags=0x%02x request_rssi=%d",
            (unsigned long long)request.clicker_id,
            request.event_seq,
            request.flags,
            rssi);
    LOG_INF("anchor UWB responder window open for %u ms; READY remains advertised",
            ANCHOR_UWB_WINDOW_MS);

    deadline_ms = k_uptime_get() + ANCHOR_UWB_WINDOW_MS;
    while (true) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        uint32_t poll_timeout_ms;

        if (remaining_ms <= 0) {
            break;
        }
        poll_timeout_ms = (uint32_t)remaining_ms;

        ret = dwm3000_driver_responder_poll_once(DEVICE_ID,
                                                      poll_timeout_ms,
                                                      &range_result);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret == -EAGAIN) {
            LOG_DBG("anchor ignored UWB poll for another responder");
            continue;
        }
        if (ret < 0) {
            LOG_WRN("anchor responder exchange failed: %d", ret);
            k_msleep(ANCHOR_ERROR_BACKOFF_MS);
            continue;
        }

        handled_ranges++;
        LOG_INF("anchor responder exchange %u complete: clicker=0x%016llx event_seq=%u status=%u distance_mm=%d quality=%u",
                handled_ranges,
                (unsigned long long)request.clicker_id,
                request.event_seq,
                range_result.status,
                range_result.distance_mm,
                range_result.quality);

        if (ble_flags_count_as_click(request.flags)) {
            ret = build_range_report(request.clicker_id, request.event_seq, &range_result);
            if (ret < 0) {
                LOG_WRN("failed to build anchor-side click report: %d", ret);
            }
        } else {
            LOG_INF("diagnostic responder result was not counted as click");
        }
    }

    (void)dwm3000_driver_standby();
    (void)ble_stop_advertising();

    LOG_INF("anchor UWB responder window closed: handled_ranges=%u; DWM3000 standby requested",
            handled_ranges);

    key = k_spin_lock(&anchor_ble_lock);
    anchor_uwb_busy = false;
    k_spin_unlock(&anchor_ble_lock, key);

    ret = anchor_start_ble_scan();
    if (ret < 0) {
        LOG_ERR("failed to restart low-duty anchor BLE scan: %d", ret);
    }
}

static void anchor_scan_cb(const bt_addr_le_t *addr,
                           int8_t rssi,
                           uint8_t adv_type,
                           struct net_buf_simple *buf)
{
    struct anchor_scan_parse_context context = {0};
    k_spinlock_key_t key;
    bool submit = false;

    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    if (mesh_queue_from_scan(buf, rssi)) {
        return;
    }

    bt_data_parse(buf, parse_discovery_ad, &context);
    if (!context.found) {
        return;
    }

    key = k_spin_lock(&anchor_ble_lock);
    if (!anchor_uwb_busy && !anchor_pending_valid) {
        anchor_pending_request = context.request;
        anchor_pending_rssi = rssi;
        anchor_pending_valid = true;
        submit = true;
    }
    k_spin_unlock(&anchor_ble_lock, key);

    if (submit) {
        LOG_INF("anchor queued discovery request: clicker=0x%016llx event_seq=%u flags=0x%02x rssi=%d",
                (unsigned long long)context.request.clicker_id,
                context.request.event_seq,
                context.request.flags,
                rssi);
        (void)k_work_submit(&anchor_ble_work);
    } else {
        LOG_DBG("anchor ignored discovery while busy/pending: clicker=0x%016llx event_seq=%u",
                (unsigned long long)context.request.clicker_id,
                context.request.event_seq);
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

    ARG_UNUSED(addr);
    ARG_UNUSED(adv_type);

    bt_data_parse(buf, parse_ready_ad, &context);
    if (!context.found) {
        return;
    }

    key = k_spin_lock(&clicker_ready_lock);
    if (context.ready.flags == clicker_ready_expected_flags) {
        score = ready_rssi_score(&context.ready, rssi);
        stored = ready_anchor_store_locked(&context.ready, rssi);
        ready_count = clicker_ready_count;
    }
    k_spin_unlock(&clicker_ready_lock, key);

    if (stored) {
        LOG_INF("READY anchor accepted: id=0x%016llx uwb=0x%04x anchor_rssi=%d clicker_rssi=%d score=%d count=%u/%u",
                (unsigned long long)context.ready.anchor_id,
                context.ready.uwb_short_addr,
                context.ready.rssi_hint,
                rssi,
                score,
                ready_count,
                MAX_READY_ANCHORS);
    } else if (context.ready.flags != clicker_ready_expected_flags) {
        LOG_DBG("READY anchor ignored due to flags: id=0x%016llx flags=0x%02x expected=0x%02x",
                (unsigned long long)context.ready.anchor_id,
                context.ready.flags,
                clicker_ready_expected_flags);
    }
}

static int anchor_start_ble_scan(void)
{
    int ret;

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

    (void)mesh_queue_from_scan(buf, rssi);
}

static int gateway_start_mesh_scan(void)
{
    int ret;

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

static void gateway_route_adv_work_handler(struct k_work *work)
{
    struct mesh_outbound route_adv;

    ARG_UNUSED(work);

    if (mesh_relay_build_route_adv(&mesh_runtime, &route_adv, k_uptime_get_32()) == PROTO_OK) {
        (void)mesh_send_outbound(&route_adv, "gateway-route-adv");
    }
    (void)k_work_reschedule(&gateway_route_adv_work, K_MSEC(GATEWAY_ROUTE_ADV_INTERVAL_MS));
}

static int clicker_scan_for_ready_list(struct ready_anchor *anchors,
                                       uint8_t anchor_cap,
                                       uint8_t expected_flags,
                                       uint32_t duration_ms)
{
    k_spinlock_key_t key;
    uint8_t found_count;
    int ret;

    if (anchors == NULL || anchor_cap == 0u) {
        return -EINVAL;
    }

    ret = ble_runtime_init();
    if (ret < 0) {
        return ret;
    }

    key = k_spin_lock(&clicker_ready_lock);
    clicker_ready_count = 0u;
    clicker_ready_expected_flags = expected_flags;
    memset(clicker_ready_anchors, 0, sizeof(clicker_ready_anchors));
    k_spin_unlock(&clicker_ready_lock, key);

    LOG_INF("clicker scanning for READY anchors: duration_ms=%u max=%u expected_flags=0x%02x",
            duration_ms,
            MAX_READY_ANCHORS,
            expected_flags);

    ret = bt_le_scan_start(BT_LE_SCAN_PASSIVE, clicker_ready_scan_cb);
    if (ret == -EALREADY) {
        (void)bt_le_scan_stop();
        ret = bt_le_scan_start(BT_LE_SCAN_PASSIVE, clicker_ready_scan_cb);
    }
    if (ret < 0) {
        return ret;
    }

    k_msleep(duration_ms);

    ret = bt_le_scan_stop();
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    key = k_spin_lock(&clicker_ready_lock);
    found_count = MIN(clicker_ready_count, anchor_cap);
    for (uint8_t i = 0u; i < found_count; i++) {
        anchors[i] = clicker_ready_anchors[i];
    }
    k_spin_unlock(&clicker_ready_lock, key);

    if (found_count == 0u) {
        return -ETIMEDOUT;
    }

    LOG_INF("clicker READY scan complete: anchors=%u sorted_by_rssi=1", found_count);
    for (uint8_t i = 0u; i < found_count; i++) {
        LOG_INF("READY order[%u]: id=0x%016llx uwb=0x%04x anchor_rssi=%d clicker_rssi=%d score=%d",
                i,
                (unsigned long long)anchors[i].ready.anchor_id,
                anchors[i].ready.uwb_short_addr,
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

    LOG_INF("clicker BLE discovery advertisement: clicker=0x%016llx event_seq=%u flags=0x%02x duration_ms=%u",
            (unsigned long long)request->clicker_id,
            request->event_seq,
            request->flags,
            duration_ms);
    return ble_advertise_manufacturer_payload(payload, payload_len, duration_ms);
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

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy) {
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

static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    uint8_t frame[UINT8_MAX];
    size_t frame_len = 0u;
    int ret;

    ret = mesh_frame_encode(out, frame, sizeof(frame), &frame_len);
    if (ret < 0) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return ret;
    }

    mesh_stop_role_scan();
    ret = ble_advertise_mesh_payload(frame, frame_len, MESH_ADV_TX_MS);
    mesh_restart_role_scan();
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

static void mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
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
        return;
    }
    if (mesh_send_outbound(&tx, reason) == 0) {
        mesh_schedule_tx_timeout();
    }
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result)
{
    if (result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) {
        (void)mesh_send_outbound(&result->hop_ack, "hop-ack");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        (void)mesh_send_outbound(&result->gateway_ack, "gateway-ack");
    }
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        mesh_start_tracked_tx(&result->forward, "forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_STATUS) {
        mesh_start_tracked_tx(&result->route_status, "route-status");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_ADV) {
        (void)mesh_send_outbound(&result->route_adv, "route-adv");
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        (void)mesh_send_outbound(&result->retransmit, "retransmit");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        LOG_WRN("mesh route discovery needed after delivery failure");
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
}

static void mesh_rx_work_handler(struct k_work *work)
{
    struct mesh_rx_pending pending;
    struct mesh_relay_result result;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);

    key = k_spin_lock(&mesh_rx_lock);
    pending = mesh_rx_pending;
    mesh_rx_pending.valid = false;
    k_spin_unlock(&mesh_rx_lock, key);

    if (!pending.valid) {
        return;
    }

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
        return;
    }

    LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
            pending.packet.msg_type,
            (unsigned long long)pending.packet.src_id,
            (unsigned long long)pending.packet.dst_id,
            (unsigned long long)pending.previous_hop_id,
            result.actions,
            result.status);
    mesh_handle_result_actions(&result);
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result result;

    ARG_UNUSED(work);

    if (mesh_relay_tick(&mesh_runtime, k_uptime_get_32(), &result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(&result);
}

static bool mesh_queue_from_scan(struct net_buf_simple *buf, int8_t rssi)
{
    struct mesh_frame_parse_context context = {0};
    k_spinlock_key_t key;
    bool queued = false;

    bt_data_parse(buf, parse_mesh_ad, &context);
    if (!context.found) {
        return false;
    }

    key = k_spin_lock(&mesh_rx_lock);
    if (!mesh_rx_pending.valid) {
        mesh_rx_pending.packet = context.packet;
        if (context.payload_len > 0u) {
            memcpy(mesh_rx_pending.payload, context.payload, context.payload_len);
        }
        mesh_rx_pending.payload_len = context.payload_len;
        mesh_rx_pending.previous_hop_id = context.previous_hop_id;
        mesh_rx_pending.link_quality = mesh_quality_from_rssi(rssi);
        mesh_rx_pending.valid = true;
        queued = true;
    }
    k_spin_unlock(&mesh_rx_lock, key);

    if (queued) {
        (void)k_work_submit(&mesh_rx_work);
    } else {
        LOG_WRN("mesh RX dropped because work slot is occupied");
    }
    return true;
}

static int build_range_report(uint64_t clicker_id,
                              uint32_t event_seq,
                              const struct dwm3000_range_result *range_result)
{
    struct range_report_fields fields;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    size_t payload_len = 0u;
    size_t encoded_len = 0u;
    int ret;

    if (range_result == NULL || range_result->responder_id == 0u) {
        return -EINVAL;
    }

    fields.clicker_id = clicker_id;
    fields.anchor_id = range_result->responder_id;
    fields.event_seq = event_seq;
    fields.distance_mm = range_result->distance_mm;
    fields.quality = range_result->quality;
    fields.range_status = range_result->status;

    ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = report_init_click_packet(&packet,
                                        range_result->responder_id,
                                        GATEWAY_ID,
                                        event_seq,
                                        (uint16_t)event_seq,
                                        (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    LOG_INF("click report ready: clicker=0x%016llx event_seq=%u anchor=0x%016llx distance_mm=%d quality=%u packet_len=%u",
            (unsigned long long)clicker_id,
            event_seq,
            (unsigned long long)range_result->responder_id,
            range_result->distance_mm,
            range_result->quality,
            (unsigned int)encoded_len);

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        struct mesh_outbound outbound = {
            .packet = packet,
            .payload_len = (uint8_t)payload_len,
        };

        memcpy(outbound.payload, payload, payload_len);
        mesh_start_tracked_tx(&outbound, "click-report");
    }
    return 0;
}

static int run_normal_click(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = DEVICE_ID,
        .event_seq = ++next_event_seq,
        .flags = ble_flags_for_click(),
    };
    struct ready_anchor anchors[MAX_READY_ANCHORS];
    struct dwm3000_range_request range_request;
    struct dwm3000_range_result range_result;
    uint8_t success_count = 0u;
    int ready_count;
    int ret;

    LOG_INF("normal click started: event_seq=%u discovery_adv_ms=%u ready_scan_ms=%u max_anchors=%u",
            request.event_seq,
            BLE_DISCOVERY_ADV_MS,
            BLE_READY_SCAN_MS,
            MAX_READY_ANCHORS);

    ret = advertise_discovery_request(&request, BLE_DISCOVERY_ADV_MS);
    if (ret < 0) {
        LOG_ERR("normal click BLE discovery advertisement failed: %d", ret);
        return ret;
    }

    ready_count = clicker_scan_for_ready_list(anchors,
                                              ARRAY_SIZE(anchors),
                                              request.flags,
                                              BLE_READY_SCAN_MS);
    if (ready_count < 0) {
        LOG_WRN("normal click did not receive READY anchors over BLE: %d", ready_count);
        return ready_count;
    }

    LOG_INF("normal click ranging %d anchors in RSSI order", ready_count);
    for (uint8_t i = 0u; i < (uint8_t)ready_count; i++) {
        range_request.initiator_id = DEVICE_ID;
        range_request.responder_id = anchors[i].ready.anchor_id;
        range_request.session_id = request.event_seq;
        range_request.seq = i + 1u;
        range_request.flags = request.flags;
        range_request.timeout_ms = CLICK_UWB_TIMEOUT_MS;

        LOG_INF("normal click DS-TWR start: index=%u/%d anchor=0x%016llx uwb=0x%04x score=%d timeout_ms=%u",
                i + 1u,
                ready_count,
                (unsigned long long)anchors[i].ready.anchor_id,
                anchors[i].ready.uwb_short_addr,
                anchors[i].rssi_score,
                range_request.timeout_ms);

        ret = dwm3000_driver_range_initiator(&range_request, &range_result);
        if (ret < 0 || range_result.status != RANGE_OK) {
            LOG_WRN("normal click DS-TWR failed: anchor=0x%016llx ret=%d range_status=%u",
                    (unsigned long long)anchors[i].ready.anchor_id,
                    ret,
                    range_result.status);
            continue;
        }

        ret = build_range_report(DEVICE_ID, request.event_seq, &range_result);
        if (ret < 0) {
            LOG_WRN("normal click report build failed: anchor=0x%016llx ret=%d",
                    (unsigned long long)range_result.responder_id,
                    ret);
            continue;
        }
        success_count++;
    }

    (void)dwm3000_driver_standby();

    if (success_count == 0u) {
        LOG_WRN("normal click completed with no successful anchor ranges: discovered=%d",
                ready_count);
        return -EIO;
    }

    LOG_INF("normal click completed: event_seq=%u discovered=%d successful_ranges=%u",
            request.event_seq,
            ready_count,
            success_count);
    return 0;
}

static enum self_test_failure run_self_test(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = DEVICE_ID,
        .event_seq = ++next_event_seq,
        .flags = ble_flags_for_diagnostic(),
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

    ret = advertise_discovery_request(&request, BLE_DISCOVERY_ADV_MS);
    if (ret < 0) {
        LOG_ERR("self-test BLE diagnostic advertisement failed: %d", ret);
        return SELF_TEST_FAILURE_BLE;
    }

    ready_count = clicker_scan_for_ready_list(anchors,
                                              ARRAY_SIZE(anchors),
                                              request.flags,
                                              BLE_READY_SCAN_MS);
    if (ready_count < 0) {
        LOG_WRN("self-test did not receive diagnostic READY over BLE: %d", ready_count);
        return ready_count == -ETIMEDOUT ? SELF_TEST_FAILURE_NO_ANCHOR :
                                           SELF_TEST_FAILURE_BLE;
    }

    LOG_INF("self-test using strongest READY anchor: id=0x%016llx score=%d discovered=%d",
            (unsigned long long)anchors[0].ready.anchor_id,
            anchors[0].rssi_score,
            ready_count);

    range_request.initiator_id = DEVICE_ID;
    range_request.responder_id = anchors[0].ready.anchor_id;
    range_request.session_id = request.event_seq;
    range_request.seq = 1u;
    range_request.flags = request.flags;
    range_request.timeout_ms = SELF_TEST_UWB_TIMEOUT_MS;

    ret = dwm3000_driver_range_initiator(&range_request, &range_result);
    (void)dwm3000_driver_standby();
    if (ret == 0 && range_result.status == RANGE_OK) {
        LOG_INF("self-test UWB dud range passed: anchor=0x%016llx distance_mm=%d quality=%u",
                (unsigned long long)range_result.responder_id,
                range_result.distance_mm,
                range_result.quality);
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
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    LOG_INF("UWB/BLE firmware starting as %s", role_name());
    LOG_INF("runtime config: device_id=0x%016llx gateway_id=0x%016llx max_ready_anchors=%u ready_scan_ms=%u anchor_uwb_window_ms=%u",
            (unsigned long long)DEVICE_ID,
            (unsigned long long)GATEWAY_ID,
            MAX_READY_ANCHORS,
            BLE_READY_SCAN_MS,
            ANCHOR_UWB_WINDOW_MS);

    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
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
        k_work_init_delayable(&gateway_route_adv_work, gateway_route_adv_work_handler);
        ret = gateway_start_mesh_scan();
        if (ret < 0) {
            LOG_ERR("gateway mesh scan unavailable: %d", ret);
        }
        (void)k_work_schedule(&gateway_route_adv_work, K_MSEC(250u));
        LOG_INF("gateway mesh root active; USB COBS command dispatcher pending");
    }

    return 0;
}
