#include "app_watchdog.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_idle_clicker_never_requires_an_action_lease(void)
{
    assert(!app_watchdog_action_lease_stale(
        0u, 0u, 0u, UINT32_MAX, 0u, APP_WATCHDOG_PROGRESS_LEASE_MS));
}

static void test_active_action_requires_progress_from_exact_generation(void)
{
    assert(app_watchdog_action_lease_stale(
        17u, 17u, 16u, 100u, 100u, APP_WATCHDOG_PROGRESS_LEASE_MS));
    assert(!app_watchdog_action_lease_stale(
        17u, 17u, 17u, 100u, 100u, APP_WATCHDOG_PROGRESS_LEASE_MS));
}

static void test_stalled_action_expires_even_if_other_work_is_healthy(void)
{
    const uint32_t progress_ms = 1000u;
    const uint32_t deadline_ms =
        progress_ms + APP_WATCHDOG_PROGRESS_LEASE_MS;

    assert(!app_watchdog_action_lease_stale(
        9u, 9u, 9u, deadline_ms, progress_ms,
        APP_WATCHDOG_PROGRESS_LEASE_MS));
    assert(app_watchdog_action_lease_stale(
        9u, 9u, 9u, deadline_ms + 1u, progress_ms,
        APP_WATCHDOG_PROGRESS_LEASE_MS));
}

static void test_generation_transition_is_retried_without_false_expiry(void)
{
    assert(!app_watchdog_action_lease_stale(
        23u, 24u, 23u, UINT32_MAX, 0u, 1u));
}

static void test_action_age_is_wrap_safe(void)
{
    const uint32_t progress_ms = UINT32_MAX - 4u;

    assert(!app_watchdog_action_lease_stale(
        3u, 3u, 3u, 5u, progress_ms, 10u));
    assert(app_watchdog_action_lease_stale(
        3u, 3u, 3u, 5u, progress_ms, 9u));
}

int main(void)
{
    test_idle_clicker_never_requires_an_action_lease();
    test_active_action_requires_progress_from_exact_generation();
    test_stalled_action_expires_even_if_other_work_is_healthy();
    test_generation_transition_is_retried_without_false_expiry();
    test_action_age_is_wrap_safe();
    puts("app watchdog action lease tests passed");
    return 0;
}
