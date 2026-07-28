#include "app_clicker_radio_power.h"

#include "app_mesh_radio_client.h"
#include "app_radio_low_power_policy.h"
#include "dwm3000_driver.h"
#include "dwm3000_port.h"

#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_clicker_radio_power, LOG_LEVEL_DBG);

static uint32_t low_power_transition_failures;

int app_clicker_radio_self_test_preflight(void)
{
    struct app_mesh_radio_owner_lease radio_lease = {0};
    uint32_t dev_id;
    int release_ret;
    int ret;

    ret = app_mesh_radio_owner_try_claim(
        APP_MESH_RADIO_CLIENT_CLICKER,
        "clicker self-test DWM3000 preflight",
        &radio_lease);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 ownership failed: %d", ret);
        return ret;
    }
    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 port init failed: %d", ret);
        goto out;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 wake failed: %d", ret);
        goto out;
    }
    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 reset failed: %d", ret);
        goto out;
    }
    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 DEV_ID probe failed: %d", ret);
        goto out;
    }
    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 fast SPI config failed: %d", ret);
        goto out;
    }
    LOG_INF("self-test DWM3000 DEV_ID=0x%08x; fast SPI checked at %u Hz",
            dev_id, (unsigned int)dwm3000_port_current_spi_hz());
    ret = dwm3000_driver_standby();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 standby failed: %d", ret);
    }

out:
    release_ret = app_mesh_radio_owner_release(&radio_lease);
    return ret < 0 ? ret : release_ret;
}

int app_clicker_radio_finish_scheduled_burst(
    struct app_mesh_radio_owner_lease *radio_lease)
{
    int release_ret;
    int ret;

    if (radio_lease == NULL || radio_lease->generation == 0u ||
        radio_lease->client != APP_MESH_RADIO_CLIENT_CLICKER) {
        return -EINVAL;
    }
    ret = dwm3000_driver_standby();
    release_ret = app_mesh_radio_owner_release(radio_lease);
    if (ret < 0) {
        LOG_ERR("DWM3000 standby after scheduled burst failed: %d", ret);
    }
    return ret < 0 ? ret : release_ret;
}

static int retained_standby_transition(void)
{
    int ret = dwm3000_driver_configure_wake_mode();

    return ret < 0 ? ret : dwm3000_driver_standby();
}

static int retained_standby_owned(void)
{
    struct app_radio_low_power_policy policy;
    enum app_radio_low_power_action action;
    int first_ret;
    int recovery_ret;
    int retry_ret = 0;

    app_radio_low_power_policy_init(&policy, APP_RADIO_LOW_POWER_STANDBY);
    first_ret = retained_standby_transition();
    action = app_radio_low_power_policy_note_transition(&policy, first_ret);
    if (action == APP_RADIO_LOW_POWER_COMPLETE) {
        return 0;
    }
    recovery_ret = dwm3000_driver_force_recovery();
    action = app_radio_low_power_policy_note_recovery(&policy, recovery_ret);
    if (action == APP_RADIO_LOW_POWER_RETRY) {
        retry_ret = retained_standby_transition();
        action = app_radio_low_power_policy_note_transition(&policy, retry_ret);
        if (action == APP_RADIO_LOW_POWER_COMPLETE) {
            LOG_WRN("DWM3000 retained standby recovered: first_ret=%d",
                    first_ret);
            return 0;
        }
    }
    if (low_power_transition_failures != UINT32_MAX) {
        low_power_transition_failures++;
    }
    LOG_ERR("DWM3000 retained standby failed after recovery: first=%d recovery=%d retry=%d failures=%u",
            first_ret, recovery_ret, retry_ret,
            low_power_transition_failures);
    return recovery_ret < 0 ? recovery_ret : retry_ret;
}

int app_clicker_radio_enter_retained_standby(void)
{
    struct app_mesh_radio_owner_lease radio_lease = {0};
    int release_ret;
    int ret = app_mesh_radio_owner_try_claim(
        APP_MESH_RADIO_CLIENT_CLICKER,
        "clicker retained standby",
        &radio_lease);

    if (ret < 0) {
        return ret;
    }
    ret = retained_standby_owned();
    release_ret = app_mesh_radio_owner_release(&radio_lease);
    return ret < 0 ? ret : release_ret;
}

int app_clicker_radio_prepare_systemoff(
    struct app_mesh_radio_owner_lease *terminal_lease_out)
{
    uint32_t dev_id;
    int park_ret;
    int ret;

    if (terminal_lease_out == NULL) {
        return -EINVAL;
    }
    ret = app_mesh_radio_owner_try_claim(
        APP_MESH_RADIO_CLIENT_CLICKER,
        "clicker terminal system-off",
        terminal_lease_out);
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_WRN("DWM3000 port init before system-off failed: %d", ret);
        goto park;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_WRN("DWM3000 wake before system-off failed: %d", ret);
        goto park;
    }
    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_WRN("DWM3000 reset before system-off failed: %d", ret);
        goto park;
    }
    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_WRN("DWM3000 probe before system-off failed: %d", ret);
        goto park;
    }
    ret = dwm3000_driver_configure_default();
    if (ret < 0) {
        LOG_WRN("DWM3000 config before system-off failed: %d", ret);
        goto park;
    }
    ret = dwm3000_driver_standby();
    if (ret < 0) {
        LOG_WRN("DWM3000 standby before system-off failed: %d", ret);
    }

park:
    park_ret = dwm3000_port_prepare_systemoff();
    if (park_ret < 0) {
        LOG_WRN("DWM3000 pin park before system-off failed: %d", park_ret);
    }
    return ret < 0 ? ret : park_ret;
}
