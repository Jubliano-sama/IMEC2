#!/usr/bin/env python3
"""Keep production battery LEDs separate from bench RF activity lights."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
APP_CMAKE = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
BOARD = (ROOT / "app" / "src" / "app_board.c").read_text(encoding="utf-8")
CLICKER = (ROOT / "app" / "src" / "app_clicker.c").read_text(encoding="utf-8")
INDICATOR = (ROOT / "app" / "src" / "app_battery_indicator.c").read_text(
    encoding="utf-8"
)
WATCHDOG = (ROOT / "app" / "src" / "app_watchdog.c").read_text(
    encoding="utf-8"
)


def function_body(source: str, name: str) -> str:
    marker = f"{name}("
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


assert (
    'if(IMEC_DEPLOYABLE_MESH_PRESET)\n'
    '        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"\n'
    '            "CONFIG_IMEC_PRODUCTION_STATUS_LEDS=y\\n")'
) in APP_CMAKE, "only deployable mesh presets may select production LED policy"

assert (
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR\n'
    '       IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
) in APP_CMAKE, "only exact production battery roles may select periodic pulses"
battery_selector_start = APP_CMAKE.index(
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR\n'
    '       IMEC_BUILD_PRESET STREQUAL "mesh_clicker")'
)
battery_selector_end = APP_CMAKE.index(
    'if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")',
    battery_selector_start,
)
battery_selector = APP_CMAKE[battery_selector_start:battery_selector_end]
assert "mesh_anchor_forcedhop" not in battery_selector
assert "CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR=y" in battery_selector
assert (
    "target_sources_ifdef(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR app PRIVATE"
    in APP_CMAKE
), "battery worker state must not link into gateway or test presets"

activity_gate = function_body(BOARD, "mesh_route_activity_leds_enabled")
assert "!IS_ENABLED(CONFIG_IMEC_PRODUCTION_STATUS_LEDS)" in activity_gate

power_indicator = function_body(BOARD, "status_power_indicator_set")
assert "production_anchor_battery_indicator_enabled()" in power_indicator
assert "enabled = false;" in power_indicator

assert "#define BATTERY_INDICATOR_LED_ON_MS 50u" in INDICATOR
assert "#define BATTERY_INDICATOR_ANCHOR_PERIOD_MS 5000u" in INDICATOR
assert "#define BATTERY_INDICATOR_CLICKER_PERIOD_MS 5000u" in INDICATOR
assert "band == BATTERY_STATUS_LOW" in INDICATOR
assert "band == BATTERY_STATUS_HIGH" in INDICATOR
assert "band == BATTERY_STATUS_MIDDLE" in INDICATOR
assert "battery_status_clicker_band(battery_mv)" in INDICATOR

indicator_handler = function_body(INDICATOR, "battery_indicator_work_handler")
assert indicator_handler.index("if (battery_indicator_led_on)") < \
    indicator_handler.index("app_watchdog_clicker_idle_checkpoint()") < \
    indicator_handler.index("battery_sample_lithium_mv"), \
    "the clicker watchdog must share the existing LED-on wake, even if ADC sampling fails"
assert (
    "BATTERY_INDICATOR_CLICKER_PERIOD_MS <\n"
    "                 APP_WATCHDOG_HARDWARE_TIMEOUT_MS"
) in INDICATOR, "the coalesced battery/watchdog interval needs a build-time bound"

coalesced = function_body(WATCHDOG, "clicker_idle_watchdog_coalesced")
assert "DEVICE_ROLE == ROLE_CLICKER" in coalesced
assert "CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR" in coalesced
monitor_start = function_body(WATCHDOG, "start_watchdog_health_monitor")
assert monitor_start.index("clicker_idle_watchdog_coalesced()") < \
    monitor_start.index("k_work_init_delayable"), \
    "production clicker must skip the one-second work and timer owners"
checkpoint = function_body(WATCHDOG, "app_watchdog_clicker_idle_checkpoint")
assert checkpoint.index("atomic_set(&system_progress_ms") < \
    checkpoint.index("watchdog_timer_handler(NULL)"), \
    "the shared battery wake must refresh progress before feeding"

action = function_body(CLICKER, "app_clicker_handle_button_action")
assert action.index("app_battery_indicator_suspend()") < action.index(
    "clicker_connect_status_leds_for_action()"
), "click/self-test feedback must take LED ownership before reconnecting pins"

retained_idle = function_body(CLICKER, "clicker_enter_systemon_retained_idle")
assert retained_idle.index("status_leds_disconnect()") < retained_idle.index(
    "app_battery_indicator_resume()"
), "the low-battery timer starts only after retained-idle pin parking"

systemoff_idle = function_body(CLICKER, "app_clicker_enter_systemoff_idle")
assert "app_battery_indicator_suspend()" in systemoff_idle

# Execute the production sampling and indicator functions against faulting
# GPIO/ADC calls. The seam keeps the real cleanup order and retry/reset owner;
# only Zephyr and the peripheral calls are replaced.
native = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <setjmp.h>
#include "battery_status.h"
#define DT_NODE_HAS_STATUS(...) 1
#define HAS_BATTERY_ADC 1
#define GPIO_OUTPUT_HIGH 1
#define GPIO_OUTPUT_LOW 0
#define GPIO_INPUT 2
#define GPIO_DISCONNECTED 3
#define CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR 1
#define IS_ENABLED(x) (x)
#define K_FOREVER -1
#define K_MSEC(n) (n)
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define SYS_REBOOT_COLD 0
#define LOG_PANIC() ((void)0)
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define LOG_ERR(...) log_sink(__VA_ARGS__)
#define LOG_WRN(...) log_sink(__VA_ARGS__)
struct gpio_dt_spec { void *port; unsigned pin; };
static const struct gpio_dt_spec battery_adc_enable = {0, 7u};
static const struct gpio_dt_spec battery_pg = {0, 15u};
struct adc_sequence { void *buffer; size_t buffer_size; };
static int battery_adc;
struct k_work { int unused; };
struct k_work_delayable { int unused; };
static struct k_work_delayable battery_indicator_work;
static int battery_indicator_mutex;
static int battery_adc_mutex;
static bool battery_indicator_initialized, battery_indicator_suspended;
static bool battery_indicator_led_on, battery_indicator_sample_failure_reported;
static bool divider_on, stopped;
static uint16_t battery_indicator_cached_mv;
static uint64_t battery_indicator_sampled_ms;
static bool battery_indicator_refresh_due, battery_indicator_usb_present;
static uint32_t battery_indicator_pulse_ms, battery_indicator_cycle_ms;
static int pg_result, pg_config_error, pg_cleanup_error;
static unsigned pg_checks, pg_disconnects, pg_reads, samples;
static int16_t sampled_adc_mv;
static bool led0[3], led1[3];
static unsigned disconnects;
static int enable_config_error, enable_set_error, adc_error_stage;
static unsigned disable_failures, disable_attempts, checkpoints;
static unsigned schedule_count, scheduled_ms;
static uint64_t now_ms;
static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static jmp_buf reboot_target;
static int gpio_is_ready_dt(const void *p) { (void)p; return 1; }
static int gpio_pin_configure(void *p, unsigned pin, int level)
{
    (void)p;
    if (pin == battery_pg.pin) {
        assert(level == GPIO_DISCONNECTED);
        pg_disconnects++;
        return pg_cleanup_error;
    }
    if (level == GPIO_OUTPUT_LOW) {
        assert(battery_adc_mutex == 1);
        /* A failed configure is allowed to have modified the pin. */
        divider_on = true;
        return enable_config_error;
    }
    disable_attempts++;
    if (disable_failures != 0u) { disable_failures--; return -EIO; }
    divider_on = false;
    return 0;
}
static int gpio_pin_configure_dt(const struct gpio_dt_spec *p, int flags)
{ assert(p == &battery_pg && flags == GPIO_INPUT); pg_checks++; return pg_config_error; }
static int gpio_pin_get_dt(const struct gpio_dt_spec *p)
{ assert(p == &battery_pg); pg_reads++; return pg_result; }
static int gpio_pin_set_raw(void *p, unsigned pin, int level)
{
    (void)p; (void)pin;
    if (level == 0 && enable_set_error) { return enable_set_error; }
    divider_on = level == 0;
    return 0;
}
static void k_busy_wait(unsigned us) { (void)us; }
static void k_msleep(unsigned ms)
{
    if (stopped) { assert(checkpoints == (DEVICE_ROLE == ROLE_CLICKER)); }
    now_ms += ms;
}
static int adc_is_ready_dt(const void *p) { (void)p; return 1; }
static int adc_channel_setup_dt(const void *p)
{ (void)p; assert(battery_adc_mutex == 1); return adc_error_stage == 1 ? -EINVAL : 0; }
static int adc_sequence_init_dt(const void *p, struct adc_sequence *s)
{ (void)p; (void)s; return adc_error_stage == 2 ? -EINVAL : 0; }
static int adc_read_dt(const void *p, struct adc_sequence *s)
{
    (void)p; assert(battery_adc_mutex == 1); samples++; *(int16_t *)s->buffer = sampled_adc_mv;
    return adc_error_stage == 3 ? -EINVAL : 0;
}
static int adc_raw_to_millivolts_dt(const void *p, int32_t *mv)
{ (void)p; (void)mv; return adc_error_stage == 4 ? -EINVAL : 0; }
static void k_mutex_lock(int *m, int timeout) { (void)timeout; (*m)++; }
static void k_mutex_unlock(int *m) { assert(*m > 0); (*m)--; }
static int k_work_reschedule(struct k_work_delayable *w, unsigned ms)
{ (void)w; assert(!stopped); schedule_count++; scheduled_ms = ms; return 0; }
static int k_work_cancel_delayable(struct k_work_delayable *w) { (void)w; return 0; }
static void status_led0_set(bool r, bool g, bool b)
{ led0[0] = r; led0[1] = g; led0[2] = b; }
static void status_led1_set(bool r, bool g, bool b)
{ led1[0] = r; led1[1] = g; led1[2] = b; }
static void status_leds_disconnect(void) { disconnects++; }
static int status_leds_connect(void) { return 0; }
static void app_watchdog_clicker_idle_checkpoint(void)
{ assert(!stopped); checkpoints++; }
static void app_watchdog_stop_feeding(void) { stopped = true; }
static void sys_reboot(int mode)
{ assert(mode == SYS_REBOOT_COLD && stopped); longjmp(reboot_target, 1); }
static void k_cpu_idle(void) { abort(); }
'''

for source, names in (
    (BOARD, ("BATTERY_ADC_DISABLE_RETRY_COUNT", "BATTERY_ADC_DISABLE_RETRY_DELAY_US")),
    (INDICATOR, ("BATTERY_INDICATOR_LED_ON_MS", "BATTERY_INDICATOR_ANCHOR_PERIOD_MS",
                 "BATTERY_INDICATOR_CLICKER_PERIOD_MS",
                 "BATTERY_INDICATOR_CLICKER_LED_ON_MS", "BATTERY_INDICATOR_CLICKER_RED_ON_MS",
                 "BATTERY_INDICATOR_CLICKER_SAMPLE_MS", "BATTERY_INDICATOR_USB_PERIOD_MS",
                 "BATTERY_INDICATOR_USB_LED_ON_MS", "BATTERY_INDICATOR_USB_SAMPLE_MS",
                 "BATTERY_INDICATOR_RECOVERY_REBOOT_DELAY_MS")),
):
    for name in names:
        native += next(line for line in source.splitlines()
                       if line.startswith(f"#define {name} ")) + "\n"

for source, signature in (
    (BOARD, "int battery_usb_power_present(void)"),
    (BOARD, "static int battery_adc_divider_disable_locked(void)"),
    (BOARD, "int battery_adc_divider_disable(void)"),
    (BOARD, "static int battery_adc_finish(int primary_ret)"),
    (BOARD, "static int battery_adc_divider_enable(void)"),
    (BOARD, "static int battery_sample_lithium_mv_locked(uint16_t *battery_mv)"),
    (BOARD, "int battery_sample_lithium_mv(uint16_t *battery_mv)"),
    (INDICATOR, "static bool battery_indicator_role_enabled(void)"),
    (INDICATOR, "static uint32_t battery_indicator_period_ms(void)"),
    (INDICATOR, "static void battery_indicator_led_off(void)"),
    (INDICATOR, "static void battery_indicator_recovery_reset(int sample_ret, int cleanup_ret)"),
    (INDICATOR, "static bool battery_indicator_schedule(uint32_t delay_ms)"),
    (INDICATOR, "static void battery_indicator_work_handler(struct k_work *work)"),
    (INDICATOR, "void app_battery_indicator_resume(void)"),
    (INDICATOR, "void app_battery_indicator_suspend(void)"),
):
    name = signature.split("(")[0].split()[-1]
    native += signature + "\n" + function_body(source, name) + "\n"

native += r'''
static void reset_fixture(void)
{
    battery_indicator_initialized = true;
    battery_indicator_suspended = false;
    battery_indicator_led_on = battery_indicator_sample_failure_reported = false;
    divider_on = stopped = false;
    enable_config_error = enable_set_error = adc_error_stage = 0;
    disable_failures = disable_attempts = checkpoints = 0u;
    schedule_count = scheduled_ms = now_ms = 0u;
    battery_indicator_cached_mv = 0u;
    battery_indicator_sampled_ms = 0u;
    battery_indicator_refresh_due = true;
    battery_indicator_usb_present = false;
    battery_indicator_pulse_ms = battery_indicator_cycle_ms = 0u;
    pg_result = pg_config_error = pg_cleanup_error = 0;
    pg_checks = pg_disconnects = pg_reads = samples = disconnects = 0u;
    sampled_adc_mv = 1800;
    status_led0_set(false, false, false);
    status_led1_set(false, false, false);
}
static void pulse_off(unsigned width, unsigned period)
{
    assert(battery_indicator_led_on && scheduled_ms == width);
    unsigned old_pg = pg_checks, old_samples = samples, old_checkpoints = checkpoints;
    now_ms += width;
    battery_indicator_work_handler(NULL);
    assert(!battery_indicator_led_on && scheduled_ms == period - width);
    assert(!led0[0] && !led0[1] && !led0[2]);
    assert(!led1[0] && !led1[1] && !led1[2]);
    assert(pg_checks == old_pg && samples == old_samples && checkpoints == old_checkpoints);
}
static void test_timing_and_cache(void)
{
    reset_fixture();
    now_ms = (uint64_t)UINT32_MAX + 10000u;
    battery_indicator_suspended = true;
    app_battery_indicator_resume();
    assert(scheduled_ms == 5000u && battery_indicator_refresh_due);
    now_ms += 5000u;
    battery_indicator_work_handler(NULL);
    assert(samples == 1u && !divider_on && led0[2]);
    uint64_t sampled_at = now_ms;
    pulse_off(DEVICE_ROLE == ROLE_CLICKER ? 25u : 50u, 5000u);
    unsigned schedules = schedule_count;
    app_battery_indicator_resume(); /* Already resumed: no resample or timer reset. */
    assert(schedule_count == schedules && !battery_indicator_refresh_due);
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == (DEVICE_ROLE == ROLE_CLICKER ? 1u : 2u));
    assert(checkpoints == (DEVICE_ROLE == ROLE_CLICKER ? 2u : 0u));
    assert(pg_checks == (DEVICE_ROLE == ROLE_CLICKER ? 2u : 0u));
    pulse_off(DEVICE_ROLE == ROLE_CLICKER ? 25u : 50u, 5000u);
    if (DEVICE_ROLE == ROLE_CLICKER) {
        now_ms = sampled_at + 3600000u - 1u;
        battery_indicator_work_handler(NULL);
        assert(samples == 1u);
        pulse_off(25u, 5000u);
        now_ms += scheduled_ms;
        battery_indicator_work_handler(NULL);
        assert(samples == 2u && battery_indicator_sampled_ms > UINT32_MAX);
        pulse_off(25u, 5000u);
    }
    if (DEVICE_ROLE == ROLE_CLICKER) {
        uint64_t previous_sample = battery_indicator_sampled_ms;
        unsigned old_checkpoints = checkpoints;
        now_ms = previous_sample + 3600000u;
        adc_error_stage = 3;
        battery_indicator_work_handler(NULL);
        assert(!battery_indicator_led_on && !divider_on && !stopped);
        assert(battery_indicator_cached_mv == 3600u);
        assert(battery_indicator_sampled_ms == previous_sample);
        assert(checkpoints == old_checkpoints + 1u && scheduled_ms == 5000u);
        adc_error_stage = 0;
        now_ms += scheduled_ms;
        battery_indicator_work_handler(NULL);
        assert(battery_indicator_sampled_ms > previous_sample);
        pulse_off(25u, 5000u);
    }
    unsigned before = samples;
    app_battery_indicator_suspend();
    app_battery_indicator_resume();
    assert(battery_indicator_refresh_due && scheduled_ms == 5000u);
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == before + 1u);
    pulse_off(DEVICE_ROLE == ROLE_CLICKER ? 25u : 50u, 5000u);
}
static void test_usb_and_thresholds(void)
{
    if (DEVICE_ROLE != ROLE_CLICKER) return;
    reset_fixture();
    battery_indicator_work_handler(NULL);
    pulse_off(25u, 5000u);
    pg_result = 1;
    sampled_adc_mv = 2090;
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == 2u && battery_indicator_usb_present);
    assert(led0[1] && led1[1]);
    uint64_t sampled_at = now_ms;
    pulse_off(100u, 1000u);
    now_ms = sampled_at + 30000u - 1u;
    battery_indicator_work_handler(NULL);
    assert(samples == 2u);
    pulse_off(100u, 1000u);
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == 3u);
    pulse_off(100u, 1000u);
    pg_result = 0;
    sampled_adc_mv = 1650;
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == 4u && !battery_indicator_usb_present);
    assert(led0[0] && !led1[1]);
    pulse_off(15u, 5000u);
    /* PG failures never declare USB and keep sampling retryable. */
    pg_result = -EIO;
    now_ms += scheduled_ms;
    battery_indicator_work_handler(NULL);
    assert(samples == 5u && !battery_indicator_usb_present && battery_indicator_refresh_due);
    pulse_off(15u, 5000u);
    static const uint16_t voltages[] = {3398u,3400u,3800u,4000u,4002u,4150u,4152u};
    for (unsigned usb = 0u; usb < 2u; usb++) {
        for (unsigned i = 0u; i < sizeof(voltages)/sizeof(voltages[0]); i++) {
            reset_fixture(); pg_result = (int)usb;
            sampled_adc_mv = (int16_t)(voltages[i] / 2u);
            battery_indicator_work_handler(NULL);
            bool low = voltages[i] < 3400u;
            bool high = voltages[i] > (usb ? 4000u : 3800u);
            assert(led0[0] == low && led0[1] == high && led0[2] == (!low && !high));
            assert(led1[1] == (usb && voltages[i] > 4150u));
            pulse_off(usb ? 100u : (low ? 15u : 25u), usb ? 1000u : 5000u);
            assert(disconnects == 1u && pg_checks == 1u && pg_disconnects == 1u);
        }
    }
    reset_fixture(); pg_result = 1; sampled_adc_mv = 2090;
    battery_indicator_work_handler(NULL);
    assert(led1[1]);
    app_battery_indicator_suspend();
    assert(!led0[1] && !led1[1] && !battery_indicator_led_on && disconnects == 1u);
}
static void test_pg_cleanup(void)
{
    for (unsigned stage = 0u; stage < 5u; stage++) {
        reset_fixture();
        if (stage == 1u) pg_result = 1;
        if (stage == 2u) pg_result = -EINVAL;
        if (stage == 3u) pg_config_error = -EAGAIN;
        if (stage == 4u) { pg_config_error = -EAGAIN; pg_cleanup_error = -EIO; }
        int expected = pg_cleanup_error ? pg_cleanup_error :
            (pg_config_error ? pg_config_error : pg_result);
        assert(battery_usb_power_present() == expected);
        assert(pg_checks == 1u && pg_disconnects == 1u);
        assert(pg_reads == (pg_config_error ? 0u : 1u));
    }
}
int main(void)
{
    uint16_t mv;
    test_timing_and_cache();
    test_usb_and_thresholds();
    test_pg_cleanup();
    /* Every fallible sample step preserves output and releases the divider. */
    for (unsigned stage = 0u; stage < 6u; stage++) {
        reset_fixture(); mv = 123u;
        if (stage == 0u) { enable_config_error = -EAGAIN; }
        else if (stage == 1u) { enable_set_error = -EAGAIN; }
        else { adc_error_stage = (int)stage - 1; }
        assert(battery_sample_lithium_mv(&mv) < 0);
        assert(!divider_on && mv == 123u && disable_attempts >= 1u);
    }
    reset_fixture(); disable_failures = 2u;
    assert(battery_sample_lithium_mv(&mv) == 0 && mv == 3600u);
    assert(disable_attempts == 3u && !divider_on);
    reset_fixture(); adc_error_stage = 3; disable_failures = 3u;
    assert(battery_sample_lithium_mv(&mv) == -EIO); /* cleanup overrides ADC */
    assert(divider_on);
    /* One full off retry may fail, but the indicator's bounded retry recovers. */
    reset_fixture(); disable_failures = 3u;
    battery_indicator_work_handler(NULL);
    assert(!divider_on && !stopped && disable_attempts == 4u);
    assert(schedule_count == 1u && scheduled_ms == battery_indicator_period_ms());
    /* An ordinary ADC failure still retries, then the next sample/LED works. */
    reset_fixture(); adc_error_stage = 3;
    battery_indicator_work_handler(NULL);
    assert(!divider_on && !stopped && !battery_indicator_led_on);
    assert(scheduled_ms == battery_indicator_period_ms());
    adc_error_stage = 0;
    battery_indicator_work_handler(NULL);
    assert(!divider_on && battery_indicator_led_on);
    assert(scheduled_ms == (DEVICE_ROLE == ROLE_ANCHOR ? 50u : 25u));
    battery_indicator_work_handler(NULL);
    assert(!battery_indicator_led_on && !stopped);
    /* Repeated cleanup failure resets despite the worker/ADC error context. */
    for (unsigned partial_enable = 0; partial_enable < 2u; partial_enable++) {
        reset_fixture(); disable_failures = 100u;
        if (partial_enable) { enable_set_error = -EAGAIN; }
        else { adc_error_stage = 3; }
        if (setjmp(reboot_target) == 0) {
            battery_indicator_work_handler(NULL);
            assert(!"unproven divider off returned to periodic idle");
        }
        assert(stopped && divider_on && battery_indicator_suspended);
        assert(disable_attempts == 6u && schedule_count == 0u);
        assert(now_ms == BATTERY_INDICATOR_RECOVERY_REBOOT_DELAY_MS +
                         (partial_enable ? 0u : 6u));
        assert(checkpoints == (DEVICE_ROLE == ROLE_CLICKER));
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="imec-battery-recovery-") as directory:
    source_path = Path(directory) / "battery_recovery.c"
    source_path.write_text(native, encoding="utf-8")
    for role in (1, 2):
        binary = Path(directory) / f"battery_recovery_{role}"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", f"-DDEVICE_ROLE={role}",
            "-I", str(ROOT / "include"), str(source_path),
            str(ROOT / "src/battery_status.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)

print("production battery indicator source invariants passed")
