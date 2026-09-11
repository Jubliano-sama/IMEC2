/* Compile the production monitor and action checkpoints against a virtual
 * clock/workqueue. The generated include contains app_watchdog.c unchanged. */
#include "app_watchdog.h"
#include "watchdog_adoption.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROLE_ANCHOR 1
#define ROLE_CLICKER 2
static int test_role;
static bool test_bypass;
#define DEVICE_ROLE test_role
#define CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR 1
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define CONFIG_IMEC_WATCHDOG_BYPASS test_bypass
#define IS_ENABLED(value) (value)
#define ARG_UNUSED(value) (void)(value)
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#define LOG_MODULE_REGISTER(...) _Static_assert(1, "logging stub")
#define LOG_LEVEL_INF 0
#define K_MSEC(value) ((uint32_t)(value))
#define K_NO_WAIT 0u

static void log_sink(const char *fmt, ...)
{
    (void)fmt;
}
#define LOG_ERR(...) log_sink(__VA_ARGS__)
#define LOG_WRN(...) log_sink(__VA_ARGS__)
#define LOG_INF(...) log_sink(__VA_ARGS__)
#define status_debug_printf(...) log_sink(__VA_ARGS__)

typedef int32_t atomic_t;
typedef int32_t atomic_val_t;
static atomic_val_t atomic_get(const atomic_t *value) { return *value; }
static atomic_val_t atomic_set(atomic_t *value, atomic_val_t next)
{
    atomic_val_t previous = *value;
    *value = next;
    return previous;
}
static void atomic_clear(atomic_t *value) { *value = 0; }
static bool atomic_cas(atomic_t *value, atomic_val_t old, atomic_val_t next)
{
    if (*value != old) {
        return false;
    }
    *value = next;
    return true;
}
static atomic_val_t atomic_inc(atomic_t *value)
{
    return atomic_set(value, (atomic_val_t)((uint32_t)*value + 1u));
}

struct device { int unused; };
static const struct device fake_device;
#define DT_NODELABEL(value) 0
#define DEVICE_DT_GET_OR_NULL(value) (&fake_device)
static bool device_is_ready(const struct device *dev) { return dev != NULL; }

static uint64_t clock_ms;
static uint64_t last_feed_ms;
static unsigned feed_count;
static unsigned timer_starts;
static unsigned periodic_work_requests;
static bool hardware_running;
static uint8_t hardware_reload_mask;
static int schedule_result;
static uint32_t test_reset_cause;
static int reset_cause_read_error;
static int reset_cause_clear_error;
static unsigned reset_cause_reads;
static unsigned reset_cause_clears;
static unsigned reboot_count;
static uint64_t last_reboot_ms;

static int64_t k_uptime_get(void) { return (int64_t)clock_ms; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)clock_ms; }

struct k_work { int unused; };
struct k_work_delayable {
    struct k_work work;
    void (*handler)(struct k_work *);
    bool pending;
    uint64_t due_ms;
};
struct k_timer {
    void (*handler)(struct k_timer *);
    bool pending;
    uint64_t due_ms;
    uint32_t period_ms;
    unsigned starts;
};
#define K_TIMER_DEFINE(name, callback, stop) \
    static struct k_timer name = { .handler = callback }
static void k_work_init_delayable(struct k_work_delayable *work,
                                  void (*handler)(struct k_work *))
{
    memset(work, 0, sizeof(*work));
    work->handler = handler;
}
static int k_work_reschedule(struct k_work_delayable *work, uint32_t delay_ms)
{
    assert(work->handler != NULL);
    if (schedule_result < 0) {
        return schedule_result;
    }
    work->pending = true;
    work->due_ms = clock_ms + delay_ms;
    periodic_work_requests += delay_ms != 0u;
    return 1;
}
static void k_timer_init(struct k_timer *timer,
                         void (*handler)(struct k_timer *), void *stop)
{
    (void)stop;
    memset(timer, 0, sizeof(*timer));
    timer->handler = handler;
}
static void k_timer_start(struct k_timer *timer, uint32_t delay, uint32_t period)
{
    assert(timer->handler != NULL);
    timer->pending = true;
    timer->due_ms = clock_ms + delay;
    timer->period_ms = period;
    timer->starts++;
    timer_starts++;
}

#define SYS_REBOOT_COLD 0
static void sys_reboot(int type)
{
    assert(type == SYS_REBOOT_COLD);
    reboot_count++;
    last_reboot_ms = clock_ms;
}

typedef uint8_t nrf_wdt_rr_register_t;
#define NRF_WDT0 (&fake_device)
static bool nrf_wdt_started_check(const struct device *dev)
{
    (void)dev;
    return hardware_running;
}
static bool nrf_wdt_reload_request_enable_check(const struct device *dev,
                                                nrf_wdt_rr_register_t rr)
{
    (void)dev;
    return (hardware_reload_mask & (1u << rr)) != 0u;
}
static void nrf_wdt_reload_request_set(const struct device *dev,
                                      nrf_wdt_rr_register_t rr)
{
    (void)dev;
    (void)rr;
    feed_count++;
    last_feed_ms = clock_ms;
}
static uint32_t nrf_wdt_reload_value_get(const struct device *dev)
{
    (void)dev;
    return (uint32_t)((uint64_t)APP_WATCHDOG_HARDWARE_TIMEOUT_MS *
                      32768u / 1000u) - 1u;
}
/* Values from this checkout's Zephyr hwinfo.h. Nordic POR/BOR returns zero. */
#define RESET_PIN (UINT32_C(1) << 0)
#define RESET_SOFTWARE (UINT32_C(1) << 1)
#define RESET_BROWNOUT (UINT32_C(1) << 2)
#define RESET_POR (UINT32_C(1) << 3)
#define RESET_WATCHDOG (UINT32_C(1) << 4)
#define RESET_DEBUG (UINT32_C(1) << 5)
#define RESET_LOW_POWER_WAKE (UINT32_C(1) << 7)
#define RESET_CPU_LOCKUP (UINT32_C(1) << 8)
static int hwinfo_get_reset_cause(uint32_t *cause)
{
    reset_cause_reads++;
    if (reset_cause_read_error == 0) {
        *cause = test_reset_cause;
    }
    return reset_cause_read_error;
}
static int hwinfo_clear_reset_cause(void)
{
    reset_cause_clears++;
    if (reset_cause_clear_error == 0) {
        test_reset_cause = 0u;
    }
    return reset_cause_clear_error;
}

#define WDT_FLAG_RESET_SOC 1
#define WDT_OPT_PAUSE_HALTED_BY_DBG 1
struct wdt_timeout_cfg {
    struct { uint32_t min, max; } window;
    void *callback;
    unsigned flags;
};
static int wdt_install_timeout(const struct device *dev,
                               const struct wdt_timeout_cfg *cfg)
{
    (void)dev;
    assert(cfg->window.max == APP_WATCHDOG_HARDWARE_TIMEOUT_MS);
    return 0;
}
static int wdt_setup(const struct device *dev, unsigned options)
{
    (void)dev;
    (void)options;
    hardware_running = true;
    hardware_reload_mask = 1u;
    last_feed_ms = clock_ms;
    return 0;
}
static int wdt_feed(const struct device *dev, int channel)
{
    assert(dev == &fake_device && channel == 0);
    feed_count++;
    last_feed_ms = clock_ms;
    return 0;
}

#include "watchdog_runtime_production.inc"

static void boot_with_reset(int role, bool inherited, bool bypass,
                           uint32_t cause, int read_error, int clear_error)
{
    test_role = role;
    test_bypass = bypass;
    clock_ms = 0u;
    last_feed_ms = 0u;
    feed_count = timer_starts = periodic_work_requests = 0u;
    hardware_running = inherited;
    hardware_reload_mask = inherited ? 1u : 0u;
    schedule_result = 0;
    test_reset_cause = cause;
    reset_cause_read_error = read_error;
    reset_cause_clear_error = clear_error;
    reset_cause_reads = reset_cause_clears = 0u;
    reboot_count = 0u;
    last_reboot_ms = 0u;
    /* Model a CPU reset: the scheduled flag and timer are ordinary BSS/static
     * kernel objects. Reinitializing the watchdog is not a reset operation. */
    atomic_clear(&terminal_restart_scheduled);
    terminal_restart_boot_backoff = true;
    terminal_restart_timer = (struct k_timer) {
        .handler = terminal_restart_handler,
    };
    memset(&watchdog_timer, 0, sizeof(watchdog_timer));
    memset(&system_progress_work, 0, sizeof(system_progress_work));
    assert(app_watchdog_init() == 0);
    assert(reset_cause_reads == 1u && reset_cause_clears == 1u);
}

static void boot_role(int role, bool inherited, bool bypass)
{
    boot_with_reset(role, inherited, bypass, 0u, 0, 0);
}

static void run_system_work(void)
{
    assert(system_progress_work.pending);
    assert(system_progress_work.due_ms <= clock_ms);
    system_progress_work.pending = false;
    system_progress_work.handler(&system_progress_work.work);
}

static void test_startup_grace_does_not_recur(void)
{
    const unsigned days[] = {25u, 35u, 49u, 50u};
    for (unsigned i = 0u; i < sizeof(days) / sizeof(days[0]); i++) {
        boot_role(ROLE_ANCHOR, false, false);
        clock_ms = APP_WATCHDOG_STARTUP_GRACE_MS - 1u;
        atomic_set(&system_progress_ms,
                   (atomic_val_t)((uint32_t)clock_ms -
                                 APP_WATCHDOG_PROGRESS_LEASE_MS - 1u));
        watchdog_timer_handler(NULL);
        assert(feed_count == 1u);
        clock_ms = (uint64_t)days[i] * 24u * 60u * 60u * 1000u;
        uint32_t stale_ms = (uint32_t)clock_ms -
                            APP_WATCHDOG_PROGRESS_LEASE_MS - 1u;
        atomic_set(&system_progress_ms, (atomic_val_t)stale_ms);
        atomic_set(&radio_progress_ms, (atomic_val_t)stale_ms);
        unsigned before = feed_count;
        watchdog_timer_handler(NULL);
        assert(feed_count == before);
        atomic_set(&system_progress_ms, (atomic_val_t)(uint32_t)clock_ms);
        watchdog_timer_handler(NULL);
        assert(feed_count == before); /* The radio lease is independently stale. */
        app_watchdog_note_radio_progress();
        watchdog_timer_handler(NULL);
        assert(feed_count == before + 1u);
    }
}

static void test_sustained_actions_without_battery_pulses(bool inherited)
{
    boot_role(ROLE_CLICKER, inherited, false);
    uint32_t generation = app_watchdog_clicker_action_begin();
    assert(generation != 0u);
    for (clock_ms = 1000u;
         clock_ms <= (uint64_t)APP_WATCHDOG_HARDWARE_TIMEOUT_MS * 2u;
         clock_ms += 4000u) {
        assert(clock_ms - last_feed_ms < APP_WATCHDOG_HARDWARE_TIMEOUT_MS);
        assert(app_watchdog_note_clicker_action_progress(generation));
        unsigned before = feed_count;
        assert(app_watchdog_clicker_action_checkpoint(generation));
        assert(feed_count == before); /* The action queue cannot feed directly. */
        run_system_work();
        assert(feed_count == before + 1u);
        assert(!system_progress_work.pending);
    }
    assert(app_watchdog_clicker_action_end(generation));
    assert(timer_starts == 0u && periodic_work_requests == 0u);
    assert(!app_watchdog_clicker_action_checkpoint(generation));
    assert(!system_progress_work.pending);
}

static void test_stalled_or_replaced_action_cannot_feed(void)
{
    const unsigned days[] = {25u, 35u, 49u, 50u};
    for (unsigned i = 0u; i < sizeof(days) / sizeof(days[0]); i++) {
        boot_role(ROLE_CLICKER, false, false);
        clock_ms = (uint64_t)days[i] * 86400000u;
        uint32_t generation = app_watchdog_clicker_action_begin();
        assert(app_watchdog_clicker_action_checkpoint(generation));
        /* The system queue stalls after admission; the callback must recheck
         * the action when it eventually runs, rather than trust admission. */
        clock_ms += APP_WATCHDOG_PROGRESS_LEASE_MS + 1u;
        assert(!app_watchdog_clicker_action_checkpoint(generation));
        run_system_work();
        assert(feed_count == 0u);
        app_watchdog_clicker_idle_checkpoint();
        assert(feed_count == 0u); /* A healthy battery worker cannot mask it. */
        assert(app_watchdog_clicker_action_end(generation));
        uint32_t replacement = app_watchdog_clicker_action_begin();
        assert(replacement != generation);
        assert(!app_watchdog_clicker_action_checkpoint(generation));
        assert(!system_progress_work.pending);
        assert(app_watchdog_clicker_action_checkpoint(replacement));
        app_watchdog_stop_feeding();
        run_system_work();
        assert(feed_count == 0u);
    }
}

static void test_checkpoint_schedule_failure_and_bypass(void)
{
    boot_role(ROLE_CLICKER, false, false);
    uint32_t generation = app_watchdog_clicker_action_begin();
    schedule_result = -EBUSY;
    assert(!app_watchdog_clicker_action_checkpoint(generation));
    assert(!system_progress_work.pending && feed_count == 0u);

    boot_role(ROLE_CLICKER, true, true);
    generation = app_watchdog_clicker_action_begin();
    unsigned before = feed_count;
    assert(app_watchdog_clicker_action_checkpoint(generation));
    assert(feed_count == before + 1u);
    assert(!system_progress_work.pending && timer_starts == 0u);
}

static void dispatch_terminal_timer_at(uint64_t uptime_ms)
{
    clock_ms = uptime_ms;
    if (terminal_restart_timer.pending &&
        terminal_restart_timer.due_ms <= clock_ms) {
        terminal_restart_timer.pending = false;
        terminal_restart_timer.handler(&terminal_restart_timer);
    }
}

static void test_terminal_restart_before_watchdog_init(void)
{
    /* No reset cause has been read: exercise the actual conservative BSS
     * initializer before any test calls the production initialization API. */
    assert(terminal_restart_boot_backoff);
    clock_ms = 100u;
    app_watchdog_schedule_terminal_restart();
    assert(terminal_restart_timer.due_ms == 1800000u);
    assert(terminal_restart_timer.starts == 1u && feed_count == 0u);
    dispatch_terminal_timer_at(1800000u);
    assert(reboot_count == 1u);
}

static void test_terminal_reset_cause_admission_and_stopped_feeds(void)
{
    const struct {
        uint32_t cause;
        int read_error;
        int clear_error;
        bool fresh;
    } cases[] = {
        {0u, 0, 0, true}, {RESET_PIN, 0, 0, true},
        {RESET_SOFTWARE, 0, 0, false}, {RESET_WATCHDOG, 0, 0, false},
        {RESET_CPU_LOCKUP, 0, 0, false}, {RESET_DEBUG, 0, 0, false},
        {RESET_LOW_POWER_WAKE, 0, 0, false}, {UINT32_C(1) << 31, 0, 0, false},
        {RESET_PIN | RESET_SOFTWARE, 0, 0, false},
        {RESET_PIN | RESET_WATCHDOG, 0, 0, false},
        {RESET_PIN | (UINT32_C(1) << 31), 0, 0, false},
        /* nRF reports POR/BOR as zero; other platforms' explicit bits cannot
         * be silently interpreted as that verified Nordic fresh-reset case. */
        {RESET_POR, 0, 0, false}, {RESET_BROWNOUT, 0, 0, false},
        {0u, -EIO, 0, false}, {RESET_PIN, -ENOTSUP, 0, false},
        {0u, 0, -EIO, false}, {RESET_PIN, 0, -EIO, false},
        {0u, -EIO, -EIO, false},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        boot_with_reset(ROLE_CLICKER, true, false, cases[i].cause,
                        cases[i].read_error, cases[i].clear_error);
        assert(!terminal_restart_timer.pending);
        assert(!watchdog_timer.pending);
        assert(timer_starts == 0u && periodic_work_requests == 0u);
        assert(!system_progress_work.pending);
        const unsigned feeds_before_fault = feed_count;
        clock_ms = 200u;
        app_watchdog_stop_feeding();
        app_watchdog_schedule_terminal_restart();
        const uint64_t expected = cases[i].fresh ? 1200u : 1800000u;
        assert(terminal_restart_timer.due_ms == expected);
        assert(terminal_restart_timer.period_ms == 0u);
        assert(terminal_restart_timer.starts == 1u && timer_starts == 1u);
        assert(feed_count == feeds_before_fault);
        /* A stalled workqueue cannot prevent the timer ISR, and reporting
         * apparent radio/action progress cannot resume feeds after the fault. */
        clock_ms = expected - 1u;
        app_watchdog_note_radio_progress();
        uint32_t generation = app_watchdog_clicker_action_begin();
        assert(app_watchdog_note_clicker_action_progress(generation));
        assert(!app_watchdog_clicker_action_checkpoint(generation));
        app_watchdog_clicker_idle_checkpoint();
        watchdog_timer_handler(NULL);
        app_watchdog_schedule_terminal_restart();
        assert(feed_count == feeds_before_fault);
        assert(terminal_restart_timer.starts == 1u);
        assert(terminal_restart_timer.due_ms == expected);
        dispatch_terminal_timer_at(expected - 1u);
        assert(reboot_count == 0u);
        dispatch_terminal_timer_at(expected);
        assert(reboot_count == 1u && last_reboot_ms == expected);
        assert(!terminal_restart_timer.pending);
        /* The stub reset returns. Repeated fault requests still cannot create
         * a second owner in that same boot. */
        app_watchdog_schedule_terminal_restart();
        assert(terminal_restart_timer.starts == 1u);
        assert(!terminal_restart_timer.pending && feed_count == feeds_before_fault);
        assert(periodic_work_requests == 0u);
    }
}

static void test_terminal_fault_time_boundaries_and_long_uptime(void)
{
    const uint64_t uptimes[] = {
        0u, 1u, 1798999u, 1799000u, 1799001u, 1799999u, 1800000u, 1800001u,
        (uint64_t)INT32_MAX - 1u, (uint64_t)INT32_MAX + 1u,
        (uint64_t)UINT32_MAX - 1u, (uint64_t)UINT32_MAX + 1u,
        UINT64_C(25) * 86400000u, UINT64_C(49) * 86400000u,
    };
    for (unsigned i = 0u; i < sizeof(uptimes) / sizeof(uptimes[0]); i++) {
        boot_with_reset(ROLE_ANCHOR, true, false, RESET_SOFTWARE, 0, 0);
        clock_ms = uptimes[i];
        /* This test jumps time to examine deadline arithmetic. These explicit
         * healthy leases stand for work before the fault, not a real soak. */
        atomic_set(&system_progress_ms, (atomic_val_t)(uint32_t)clock_ms);
        app_watchdog_note_radio_progress();
        watchdog_timer_handler(NULL);
        const unsigned feeds_before_fault = feed_count;
        app_watchdog_stop_feeding();
        app_watchdog_schedule_terminal_restart();
        const uint64_t expected = uptimes[i] + 1000u < 1800000u ?
                                   1800000u : uptimes[i] + 1000u;
        assert(terminal_restart_timer.due_ms == expected);
        assert(expected - uptimes[i] < APP_WATCHDOG_HARDWARE_TIMEOUT_MS);
        clock_ms = expected - 1u;
        app_watchdog_schedule_terminal_restart();
        watchdog_timer_handler(NULL);
        assert(feed_count == feeds_before_fault);
        assert(terminal_restart_timer.due_ms == expected);
        assert(terminal_restart_timer.starts == 1u);
        dispatch_terminal_timer_at(expected);
        assert(reboot_count == 1u && last_reboot_ms == expected);
    }
}

static void test_terminal_rf_poison_boot_checkpoint_bound(void)
{
    const uint64_t horizon_ms = UINT64_C(48) * 60u * 60u * 1000u;
    uint64_t wall_ms = 0u;
    unsigned boot_checkpoints = 1u; /* The initial boot also writes its ID. */
    unsigned boot_checkpoints_in_first_35_seconds = 1u;
    unsigned automatic_restarts = 0u;
    for (;;) {
        boot_with_reset(ROLE_ANCHOR, automatic_restarts != 0u, false,
                        automatic_restarts == 0u ? 0u : RESET_SOFTWARE, 0, 0);
        clock_ms = automatic_restarts == 0u ? 1000u :
                   (uint64_t)(automatic_restarts % 4u) * 750u;
        unsigned feeds_before_fault = feed_count;
        app_watchdog_stop_feeding();
        app_watchdog_schedule_terminal_restart();
        const uint64_t boot_duration = terminal_restart_timer.due_ms;
        assert(boot_duration == (automatic_restarts == 0u ? 2000u : 1800000u));
        /* The production one-hour hardware watchdog cannot preempt this
         * cooldown; shorter inherited hardware configurations are not modeled. */
        assert(boot_duration - last_feed_ms < APP_WATCHDOG_HARDWARE_TIMEOUT_MS);
        if (wall_ms + boot_duration > horizon_ms) {
            break;
        }
        clock_ms = boot_duration - 1u;
        system_progress_work_handler(NULL);
        app_watchdog_note_radio_progress();
        watchdog_timer_handler(NULL);
        app_watchdog_schedule_terminal_restart();
        assert(feed_count == feeds_before_fault);
        assert(terminal_restart_timer.due_ms == boot_duration);
        dispatch_terminal_timer_at(boot_duration);
        assert(reboot_count == 1u);
        wall_ms += boot_duration;
        automatic_restarts++;
        boot_checkpoints++;
        if (wall_ms <= 35000u) {
            boot_checkpoints_in_first_35_seconds++;
        }
    }
    assert(boot_checkpoints_in_first_35_seconds == 2u);
    assert(automatic_restarts == 96u && boot_checkpoints == 97u);
    /* These are NVS boot-checkpoint opportunities for this RF-fault path,
     * not flash erase cycles or a claim about unrelated reset mechanisms. */
}

int main(void)
{
    test_terminal_restart_before_watchdog_init();
    test_startup_grace_does_not_recur();
    test_sustained_actions_without_battery_pulses(false);
    test_sustained_actions_without_battery_pulses(true);
    test_stalled_or_replaced_action_cannot_feed();
    test_checkpoint_schedule_failure_and_bypass();
    test_terminal_reset_cause_admission_and_stopped_feeds();
    test_terminal_fault_time_boundaries_and_long_uptime();
    test_terminal_rf_poison_boot_checkpoint_bound();
    puts("production watchdog virtual-time tests passed");
    return 0;
}
