#include "app_radio_recovery.h"

#include "app_board.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"

#include <stddef.h>

#include <zephyr/kernel.h>
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

    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
        status_debug_printf(
            "DBG_RADIO_RECOVERY step=first_begin target=%s reason=%s tid=%p\n",
            radio_recovery_target_name(target),
            reason == NULL ? "unspecified" : reason,
            k_current_get());
    }
    first_ret = radio_recovery_transition(target);
    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
        status_debug_printf(
            "DBG_RADIO_RECOVERY step=first_end target=%s reason=%s ret=%d\n",
            radio_recovery_target_name(target),
            reason == NULL ? "unspecified" : reason,
            first_ret);
    }
    if (first_ret >= 0) {
        return 0;
    }

    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
        status_debug_printf(
            "DBG_RADIO_RECOVERY step=force_begin target=%s reason=%s first_ret=%d\n",
            radio_recovery_target_name(target),
            reason == NULL ? "unspecified" : reason,
            first_ret);
    }
    recovery_ret = dwm3000_driver_force_recovery();
    if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
        status_debug_printf(
            "DBG_RADIO_RECOVERY step=force_end target=%s reason=%s ret=%d\n",
            radio_recovery_target_name(target),
            reason == NULL ? "unspecified" : reason,
            recovery_ret);
    }
    if (recovery_ret >= 0) {
        if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
            status_debug_printf(
                "DBG_RADIO_RECOVERY step=retry_begin target=%s reason=%s\n",
                radio_recovery_target_name(target),
                reason == NULL ? "unspecified" : reason);
        }
        retry_ret = radio_recovery_transition(target);
        if (IS_ENABLED(CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE)) {
            status_debug_printf(
                "DBG_RADIO_RECOVERY step=retry_end target=%s reason=%s ret=%d\n",
                radio_recovery_target_name(target),
                reason == NULL ? "unspecified" : reason,
                retry_ret);
        }
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
