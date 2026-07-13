#ifndef APP_RADIO_LOW_POWER_POLICY_H
#define APP_RADIO_LOW_POWER_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum app_radio_low_power_mode {
    APP_RADIO_LOW_POWER_IDLE = 0,
    APP_RADIO_LOW_POWER_STANDBY,
};

enum app_radio_low_power_action {
    APP_RADIO_LOW_POWER_COMPLETE = 0,
    APP_RADIO_LOW_POWER_RECOVER,
    APP_RADIO_LOW_POWER_RETRY,
    APP_RADIO_LOW_POWER_FAIL,
};

struct app_radio_low_power_policy {
    enum app_radio_low_power_mode mode;
    uint8_t transition_attempts;
    bool recovery_attempted;
};

static inline enum app_radio_low_power_mode
app_radio_low_power_mode_for_connection(bool connected)
{
    return connected ? APP_RADIO_LOW_POWER_IDLE :
           APP_RADIO_LOW_POWER_STANDBY;
}

static inline const char *app_radio_low_power_mode_name(
    enum app_radio_low_power_mode mode)
{
    return mode == APP_RADIO_LOW_POWER_IDLE ? "idle" : "standby";
}

static inline void app_radio_low_power_policy_init(
    struct app_radio_low_power_policy *policy,
    enum app_radio_low_power_mode mode)
{
    if (policy == NULL) {
        return;
    }
    policy->mode = mode;
    policy->transition_attempts = 0u;
    policy->recovery_attempted = false;
}

static inline enum app_radio_low_power_action
app_radio_low_power_policy_note_transition(
    struct app_radio_low_power_policy *policy,
    int ret)
{
    if (policy == NULL || policy->transition_attempts >= 2u) {
        return APP_RADIO_LOW_POWER_FAIL;
    }

    policy->transition_attempts++;
    if (ret >= 0) {
        return APP_RADIO_LOW_POWER_COMPLETE;
    }
    return policy->transition_attempts == 1u && !policy->recovery_attempted ?
           APP_RADIO_LOW_POWER_RECOVER : APP_RADIO_LOW_POWER_FAIL;
}

static inline enum app_radio_low_power_action
app_radio_low_power_policy_note_recovery(
    struct app_radio_low_power_policy *policy,
    int ret)
{
    if (policy == NULL || policy->recovery_attempted ||
        policy->transition_attempts != 1u) {
        return APP_RADIO_LOW_POWER_FAIL;
    }

    policy->recovery_attempted = true;
    return ret >= 0 ? APP_RADIO_LOW_POWER_RETRY :
           APP_RADIO_LOW_POWER_FAIL;
}

#endif
