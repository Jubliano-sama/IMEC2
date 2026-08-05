#include "button_wake_recovery.h"

#include <assert.h>

static void test_transient_failures_recover_before_power_admission(void)
{
    struct button_wake_recovery recovery;

    button_wake_recovery_init(&recovery, 4u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RETRY);
    assert(recovery.consecutive_failures == 1u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_ARMED) ==
           BUTTON_WAKE_RECOVERY_POWER_READY);
    assert(recovery.consecutive_failures == 0u);
}

static void test_held_button_wait_does_not_consume_failure_budget(void)
{
    struct button_wake_recovery recovery;

    button_wake_recovery_init(&recovery, 3u);
    for (unsigned int i = 0u; i < 1000u; i++) {
        assert(button_wake_recovery_note(
                   &recovery, BUTTON_WAKE_OBSERVATION_WAITING) ==
               BUTTON_WAKE_RECOVERY_RETRY);
    }
    assert(recovery.consecutive_failures == 0u);
}

static void test_consecutive_failures_force_reset_at_exact_bound(void)
{
    struct button_wake_recovery recovery;

    button_wake_recovery_init(&recovery, 3u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RETRY);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RETRY);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RESET);
    assert(recovery.consecutive_failures == 3u);
}

static void test_progress_breaks_a_failure_streak(void)
{
    struct button_wake_recovery recovery;

    button_wake_recovery_init(&recovery, 2u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RETRY);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_WAITING) ==
           BUTTON_WAKE_RECOVERY_RETRY);
    assert(recovery.consecutive_failures == 0u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_FAILURE) ==
           BUTTON_WAKE_RECOVERY_RETRY);
}

static void test_invalid_policy_fails_closed(void)
{
    struct button_wake_recovery recovery;

    button_wake_recovery_init(&recovery, 0u);
    assert(button_wake_recovery_note(
               &recovery, BUTTON_WAKE_OBSERVATION_ARMED) ==
           BUTTON_WAKE_RECOVERY_RESET);
    assert(button_wake_recovery_note(
               NULL, BUTTON_WAKE_OBSERVATION_ARMED) ==
           BUTTON_WAKE_RECOVERY_RESET);
}

int main(void)
{
    test_transient_failures_recover_before_power_admission();
    test_held_button_wait_does_not_consume_failure_budget();
    test_consecutive_failures_force_reset_at_exact_bound();
    test_progress_breaks_a_failure_streak();
    test_invalid_policy_fails_closed();
    return 0;
}
