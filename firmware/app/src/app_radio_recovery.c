#include "app_radio_recovery.h"

#include "app_watchdog.h"
#include "dwm3000_driver.h"

#include <stddef.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_radio_recovery, LOG_LEVEL_DBG);

enum radio_recovery_target {
    RADIO_RECOVERY_TARGET_IDLE = 0,
    RADIO_RECOVERY_TARGET_STANDBY = 1,
};

static int radio_recovery_transition(enum radio_recovery_target target)
{
    return target == RADIO_RECOVERY_TARGET_IDLE ?
           dwm3000_driver_idle() :
           dwm3000_driver_standby();
}

static const char *radio_recovery_target_name(enum radio_recovery_target target)
{
    return target == RADIO_RECOVERY_TARGET_IDLE ? "idle" : "standby";
}

static int radio_transition_with_bounded_recovery(
    enum radio_recovery_target target,
    const char *reason)
{
    int first_ret;
    int recovery_ret;
    int retry_ret;

    first_ret = radio_recovery_transition(target);
    if (first_ret >= 0) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    if (recovery_ret >= 0) {
        retry_ret = radio_recovery_transition(target);
        if (retry_ret >= 0) {
            LOG_WRN("radio %s recovered: reason=%s first_ret=%d",
                    radio_recovery_target_name(target),
                    reason == NULL ? "unspecified" : reason,
                    first_ret);
            return 0;
        }
    } else {
        retry_ret = recovery_ret;
    }

    LOG_ERR("radio %s failed after bounded recovery: reason=%s first_ret=%d recovery_ret=%d retry_ret=%d",
            radio_recovery_target_name(target),
            reason == NULL ? "unspecified" : reason,
            first_ret,
            recovery_ret,
            retry_ret);
    app_watchdog_stop_feeding();
    return recovery_ret < 0 ? recovery_ret : retry_ret;
}

int app_radio_idle_with_bounded_recovery(const char *reason)
{
    return radio_transition_with_bounded_recovery(
        RADIO_RECOVERY_TARGET_IDLE,
        reason);
}

int app_radio_standby_with_bounded_recovery(const char *reason)
{
    return radio_transition_with_bounded_recovery(
        RADIO_RECOVERY_TARGET_STANDBY,
        reason);
}
