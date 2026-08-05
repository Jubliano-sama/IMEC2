#ifndef BUTTON_WAKE_RECOVERY_H
#define BUTTON_WAKE_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

enum button_wake_observation {
    BUTTON_WAKE_OBSERVATION_WAITING = 0,
    BUTTON_WAKE_OBSERVATION_FAILURE,
    BUTTON_WAKE_OBSERVATION_ARMED,
};

enum button_wake_recovery_action {
    BUTTON_WAKE_RECOVERY_RETRY = 0,
    BUTTON_WAKE_RECOVERY_POWER_READY,
    BUTTON_WAKE_RECOVERY_RESET,
};

struct button_wake_recovery {
    uint8_t consecutive_failures;
    uint8_t max_failures;
};

static inline void button_wake_recovery_init(
    struct button_wake_recovery *recovery,
    uint8_t max_failures)
{
    if (recovery == NULL) {
        return;
    }

    recovery->consecutive_failures = 0u;
    recovery->max_failures = max_failures;
}

static inline enum button_wake_recovery_action
button_wake_recovery_note(
    struct button_wake_recovery *recovery,
    enum button_wake_observation observation)
{
    if (recovery == NULL || recovery->max_failures == 0u) {
        return BUTTON_WAKE_RECOVERY_RESET;
    }

    if (observation == BUTTON_WAKE_OBSERVATION_ARMED) {
        recovery->consecutive_failures = 0u;
        return BUTTON_WAKE_RECOVERY_POWER_READY;
    }
    if (observation == BUTTON_WAKE_OBSERVATION_WAITING) {
        recovery->consecutive_failures = 0u;
        return BUTTON_WAKE_RECOVERY_RETRY;
    }
    if (observation != BUTTON_WAKE_OBSERVATION_FAILURE) {
        return BUTTON_WAKE_RECOVERY_RESET;
    }

    if (recovery->consecutive_failures < UINT8_MAX) {
        recovery->consecutive_failures++;
    }
    return recovery->consecutive_failures >= recovery->max_failures ?
           BUTTON_WAKE_RECOVERY_RESET : BUTTON_WAKE_RECOVERY_RETRY;
}

#endif
