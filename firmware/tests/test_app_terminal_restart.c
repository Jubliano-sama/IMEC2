#include "app_watchdog.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned atomic_t;
struct k_timer { void (*handler)(struct k_timer *); uint64_t due; bool armed; };
#define ARG_UNUSED(v) (void)(v)
#define K_MSEC(v) (v)
#define K_NO_WAIT 0u
#define SYS_REBOOT_COLD 1
#define K_TIMER_DEFINE(name, handler, stop) static struct k_timer name = {handler, 0u, false}
static uint64_t now;
static unsigned starts, reboots;
/* The integrated watchdog fixture tests how real reset-cause admission sets
 * this BSS decision. Here it is the timer ISR's only boot-policy input. */
static bool terminal_restart_boot_backoff = true;
static int64_t k_uptime_get(void) { return (int64_t)now; }
static bool atomic_cas(atomic_t *value, unsigned expected, unsigned desired)
{
    if (*value != expected) return false;
    *value = desired;
    return true;
}
static void k_timer_start(struct k_timer *timer, unsigned delay, unsigned period)
{
    assert(period == 0u);
    timer->due = now + delay;
    timer->armed = true;
    starts++;
}
static void sys_reboot(int type)
{
    assert(type == SYS_REBOOT_COLD);
    reboots++;
}
#include "terminal_restart_production.inc"
/* The fake clock dispatches only the timer ISR; no workqueue symbols or
 * work dispatch exist in this fixture. */
static void tick_at(uint64_t uptime)
{
    now = uptime;
    if (terminal_restart_timer.armed && now >= terminal_restart_timer.due) {
        terminal_restart_timer.armed = false;
        terminal_restart_timer.handler(&terminal_restart_timer);
    }
}
static void exercise_owned_timer(bool backoff, uint64_t fault_uptime,
                                 uint64_t expected_deadline)
{
    terminal_restart_scheduled = 0u;
    terminal_restart_timer.armed = false;
    terminal_restart_boot_backoff = backoff;
    starts = reboots = 0u;
    now = fault_uptime;
    app_watchdog_schedule_terminal_restart();
    const uint64_t deadline = terminal_restart_timer.due;
    assert(deadline == expected_deadline);
    const uint64_t repeated_requests[] = {
        fault_uptime, fault_uptime + 1u,
        fault_uptime + (deadline - fault_uptime) / 2u, deadline - 1u,
    };
    for (unsigned i = 0u;
         i < sizeof(repeated_requests) / sizeof(repeated_requests[0]); i++) {
        tick_at(repeated_requests[i]);
        app_watchdog_schedule_terminal_restart();
        assert(terminal_restart_timer.due == deadline);
        assert(reboots == 0u);
    }
    assert(starts == 1u);
    /* Even a repeated request one tick before expiry cannot postpone it. */
    app_watchdog_schedule_terminal_restart();
    tick_at(deadline);
    assert(now == deadline && reboots == 1u);
    assert(!terminal_restart_timer.armed);
    /* The fake reboot returns; a real cold reboot would end this boot. */
    app_watchdog_schedule_terminal_restart();
    tick_at(deadline + 1u);
    assert(starts == 1u && reboots == 1u);
}

int main(void)
{
    /* Conservatively back off before initialization, then exercise the
     * admitted fresh boot and a multi-month-clock automatic boot separately. */
    exercise_owned_timer(true, 100u, 1800000u);
    exercise_owned_timer(false, 100u, 1100u);
    exercise_owned_timer(true, UINT64_C(49) * 86400000u,
                         UINT64_C(49) * 86400000u + 1000u);
    return 0;
}
