#include "app_clicker_ble_courtesy.h"

#include "app_config.h"
#include "app_high_debug_log.h"
#include "uwb_ble_courtesy.h"

#if defined(CONFIG_BT) && DEVICE_ROLE == ROLE_CLICKER
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/net_buf.h>
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
#include <bluetooth/hci_vs_sdc.h>
#endif
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_DECLARE(app_clicker, LOG_LEVEL_DBG);

#define BLE_COURTESY_STOP_RETRY_COUNT 3u
#define BLE_COURTESY_STOP_RETRY_DELAY_MS 5u

#if defined(CONFIG_BT) && DEVICE_ROLE == ROLE_CLICKER
static bool ble_courtesy_init_attempted;
static bool ble_courtesy_available;
static bool ble_courtesy_adv_active;
static bool ble_courtesy_scan_active;
static struct k_spinlock ble_courtesy_lock;
static uint32_t ble_courtesy_higher_wait_ms;
static uint8_t ble_courtesy_adv_data[UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN];
static struct uwb_ble_courtesy_frame ble_courtesy_local;

static int clicker_ble_courtesy_set_scan_channel(void)
{
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
    const sdc_hci_cmd_vs_scan_channel_map_set_t params = {
        .channel_map = {0xffu, 0xffu, 0xffu, 0xffu, 0x3fu},
    };

    return hci_vs_sdc_scan_channel_map_set(&params);
#else
    return -ENOTSUP;
#endif
}

static void clicker_ble_courtesy_note_higher_peer(uint32_t wait_ms)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    if (wait_ms > ble_courtesy_higher_wait_ms) {
        ble_courtesy_higher_wait_ms = wait_ms;
    }
    k_spin_unlock(&ble_courtesy_lock, key);
}

static void clicker_ble_courtesy_clear_higher_peer(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    ble_courtesy_higher_wait_ms = 0u;
    k_spin_unlock(&ble_courtesy_lock, key);
}

static bool clicker_ble_courtesy_parse_ad(struct bt_data *data, void *user_data)
{
    struct uwb_ble_courtesy_frame peer;
    int cmp;

    ARG_UNUSED(user_data);

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }
    if (uwb_ble_courtesy_decode(data->data, data->data_len, &peer) != PROTO_OK) {
        return true;
    }
    if (peer.network_id != NETWORK_ID || peer.clicker_id == ble_courtesy_local.clicker_id) {
        return false;
    }

    cmp = uwb_claim_precedence_compare(peer.attempt_index,
                                       peer.priority_id,
                                       peer.clicker_id,
                                       peer.click_event_id,
                                       ble_courtesy_local.attempt_index,
                                       ble_courtesy_local.priority_id,
                                       ble_courtesy_local.clicker_id,
                                       ble_courtesy_local.click_event_id);
    if (cmp > 0) {
        uint32_t wait_ms = uwb_ble_courtesy_duration_ms(peer.defer_duration_units);

        clicker_ble_courtesy_note_higher_peer(wait_ms);
        LOG_INF("BLE courtesy saw higher-precedence clicker: peer=%llx event=%u attempt=%u priority=%llx wait_ms=%u",
                (unsigned long long)peer.clicker_id,
                peer.click_event_id,
                peer.attempt_index,
                (unsigned long long)peer.priority_id,
                wait_ms);
    }
    return false;
}

static void clicker_ble_courtesy_scan_cb(const bt_addr_le_t *addr,
                                         int8_t rssi,
                                         uint8_t adv_type,
                                         struct net_buf_simple *buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    if (!ble_courtesy_scan_active) {
        return;
    }
    bt_data_parse(buf, clicker_ble_courtesy_parse_ad, NULL);
}

static int clicker_ble_courtesy_init_once(void)
{
    int disable_ret;
    int ret;

    if (ble_courtesy_available) {
        return 0;
    }
    if (ble_courtesy_init_attempted) {
        return -ENOTSUP;
    }

    ble_courtesy_init_attempted = true;
    ret = bt_enable(NULL);
    if (ret != 0 && ret != -EALREADY) {
        LOG_WRN("BLE courtesy disabled: bt_enable failed: %d", ret);
        ble_courtesy_init_attempted = false;
        return ret;
    }

    ret = clicker_ble_courtesy_set_scan_channel();
    if (ret != 0) {
        LOG_WRN("BLE courtesy disabled: scan channel 37 map failed: %d", ret);
        disable_ret = bt_disable();
        if (disable_ret == 0 || disable_ret == -EALREADY) {
            ble_courtesy_init_attempted = false;
        } else {
            LOG_WRN("BLE courtesy initialization rollback failed: %d",
                    disable_ret);
        }
        return ret;
    }

    ble_courtesy_available = true;
    LOG_INF("BLE courtesy enabled on advertising/scanning channel 37");
    return 0;
}

int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms)
{
    const struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BLE_COURTESY_SCAN_INTERVAL_UNITS,
        .window = BLE_COURTESY_SCAN_WINDOW_UNITS,
        .timeout = 0u,
        .interval_coded = 0u,
        .window_coded = 0u,
    };
    const struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0u,
        .secondary_max_skip = 0u,
        .options = BT_LE_ADV_OPT_USE_IDENTITY |
                   BT_LE_ADV_OPT_DISABLE_CHAN_38 |
                   BT_LE_ADV_OPT_DISABLE_CHAN_39,
        .interval_min = BLE_COURTESY_ADV_INTERVAL_MIN_UNITS,
        .interval_max = BLE_COURTESY_ADV_INTERVAL_MAX_UNITS,
        .peer = NULL,
    };
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                ble_courtesy_adv_data,
                sizeof(ble_courtesy_adv_data)),
    };
    size_t written = 0u;
    int ret;

    ret = clicker_ble_courtesy_init_once();
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=init ret=%d", ret);
#endif
    if (ret < 0) {
        return ret;
    }

    ble_courtesy_local.network_id = NETWORK_ID;
    ble_courtesy_local.clicker_id = DEVICE_ID;
    ble_courtesy_local.click_event_id = event_seq;
    ble_courtesy_local.attempt_index = attempt_index;
    ble_courtesy_local.priority_id = priority_id;
    ble_courtesy_local.defer_duration_units =
        uwb_ble_courtesy_duration_units_from_ms(peer_finish_ms);
    ret = uwb_ble_courtesy_encode(&ble_courtesy_local,
                                  ble_courtesy_adv_data,
                                  sizeof(ble_courtesy_adv_data),
                                  &written);
    if (ret != PROTO_OK || written != sizeof(ble_courtesy_adv_data)) {
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        high_debug_log_event("BLE_TEST",
                             "phase=encode ret=%d written=%u expected=%u duration_units=%u",
                             ret,
                             (unsigned int)written,
                             (unsigned int)sizeof(ble_courtesy_adv_data),
                             ble_courtesy_local.defer_duration_units);
#endif
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST",
                         "phase=encode ret=0 written=%u duration_units=%u",
                         (unsigned int)written,
                         ble_courtesy_local.defer_duration_units);
#endif

    clicker_ble_courtesy_clear_higher_peer();
    /* Legacy scan and advertising share the random-address state. Start the
     * identity advertiser before identity scanning so Zephyr accepts both.
     */
    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=adv_start ret=%d", ret);
#endif
    if (ret != 0) {
        LOG_WRN("BLE courtesy advertising start failed: %d", ret);
        return ret;
    }
    ble_courtesy_adv_active = true;

    ble_courtesy_scan_active = true;
    ret = bt_le_scan_start(&scan_param, clicker_ble_courtesy_scan_cb);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=scan_start ret=%d", ret);
#endif
    if (ret != 0) {
        int stop_ret;

        LOG_WRN("BLE courtesy scan start failed: %d", ret);
        ble_courtesy_scan_active = false;
        stop_ret = bt_le_adv_stop();
        if (stop_ret == 0 || stop_ret == -EALREADY) {
            ble_courtesy_adv_active = false;
        } else {
            LOG_WRN("BLE courtesy advertising rollback failed: %d", stop_ret);
        }
        return ret;
    }
    return 0;
}

uint32_t app_clicker_ble_courtesy_higher_wait_ms(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);
    uint32_t wait_ms = ble_courtesy_higher_wait_ms;

    k_spin_unlock(&ble_courtesy_lock, key);
    return wait_ms;
}

static int clicker_ble_courtesy_stop_advertising(void)
{
    int ret = 0;

    for (uint8_t attempt = 0u;
         ble_courtesy_adv_active && attempt < BLE_COURTESY_STOP_RETRY_COUNT;
         attempt++) {
        ret = bt_le_adv_stop();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_adv_active = false;
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    if (ble_courtesy_adv_active) {
        LOG_WRN("BLE courtesy advertising stop failed after %u attempts: %d",
                BLE_COURTESY_STOP_RETRY_COUNT,
                ret);
    }
    return ret;
}

static int clicker_ble_courtesy_stop_scanning(void)
{
    int ret = 0;

    for (uint8_t attempt = 0u;
         ble_courtesy_scan_active && attempt < BLE_COURTESY_STOP_RETRY_COUNT;
         attempt++) {
        ret = bt_le_scan_stop();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_scan_active = false;
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    if (ble_courtesy_scan_active) {
        LOG_WRN("BLE courtesy scan stop failed after %u attempts: %d",
                BLE_COURTESY_STOP_RETRY_COUNT,
                ret);
    }
    return ret;
}

void app_clicker_ble_courtesy_stop(void)
{
    (void)clicker_ble_courtesy_stop_advertising();
    (void)clicker_ble_courtesy_stop_scanning();
}

int app_clicker_ble_courtesy_low_power_stop(void)
{
    int ret = 0;

    app_clicker_ble_courtesy_stop();
    if (!ble_courtesy_init_attempted) {
        return 0;
    }

    for (uint8_t attempt = 0u; attempt < BLE_COURTESY_STOP_RETRY_COUNT; attempt++) {
        ret = bt_disable();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_init_attempted = false;
            ble_courtesy_available = false;
            ble_courtesy_adv_active = false;
            ble_courtesy_scan_active = false;
            clicker_ble_courtesy_clear_higher_peer();
#if defined(CONFIG_IMEC_HIGH_DEBUG)
            high_debug_log_event("BLE_TEST",
                                 "phase=low_power_stop ret=0 attempts=%u",
                                 (unsigned int)(attempt + 1u));
#endif
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    LOG_WRN("BLE courtesy disable before retained idle failed after %u attempts: %d",
            BLE_COURTESY_STOP_RETRY_COUNT,
            ret);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST",
                         "phase=low_power_stop ret=%d attempts=%u",
                         ret,
                         BLE_COURTESY_STOP_RETRY_COUNT);
#endif
    return ret;
}
#else
int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms)
{
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(priority_id);
    ARG_UNUSED(peer_finish_ms);

    return -ENOTSUP;
}

uint32_t app_clicker_ble_courtesy_higher_wait_ms(void)
{
    return 0u;
}

void app_clicker_ble_courtesy_stop(void)
{
}

int BLE_CONNECTIVITY_TEST_UNUSED app_clicker_ble_courtesy_low_power_stop(void)
{
    return 0;
}
#endif
