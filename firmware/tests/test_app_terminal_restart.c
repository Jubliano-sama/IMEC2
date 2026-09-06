#include "app_watchdog.h"
#include <assert.h>
#include <stddef.h>

typedef unsigned atomic_t;
struct k_timer { void (*handler)(struct k_timer *); unsigned due; bool armed; };
#define ARG_UNUSED(v) (void)(v)
#define K_MSEC(v) (v)
#define K_NO_WAIT 0u
#define SYS_REBOOT_COLD 1
#define K_TIMER_DEFINE(name, handler, stop) static struct k_timer name = {handler, 0u, false}
static unsigned now, starts, reboots;
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
static void tick(void)
{
    now++;
    if (terminal_restart_timer.armed && now == terminal_restart_timer.due) {
        terminal_restart_timer.armed = false;
        terminal_restart_timer.handler(&terminal_restart_timer);
    }
}
int main(void)
{
    now = 100u;
    app_watchdog_schedule_terminal_restart();
    const unsigned deadline = terminal_restart_timer.due;
    assert(deadline == now + APP_WATCHDOG_TERMINAL_RESTART_DELAY_MS);
    while (now + 1u < deadline) {
        tick();
        app_watchdog_schedule_terminal_restart();
        assert(terminal_restart_timer.due == deadline);
        assert(reboots == 0u);
    }
    assert(starts == 1u);
    /* Even a repeated request one tick before expiry cannot postpone it. */
    app_watchdog_schedule_terminal_restart();
    tick();
    assert(now == deadline && reboots == 1u);
    assert(!terminal_restart_timer.armed);
    /* The fake reboot returns; a real cold reboot would end this boot. */
    app_watchdog_schedule_terminal_restart();
    tick();
    assert(starts == 1u && reboots == 1u);
    return 0;
}
