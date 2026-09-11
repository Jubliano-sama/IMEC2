#!/usr/bin/env python3
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CLICKER = (ROOT / "app/src/app_clicker.c").read_text(encoding="utf-8")
MAIN = (ROOT / "app/src/main.c").read_text(encoding="utf-8")
RTT_CONTROL = (
    ROOT / "app/src/app_clicker_rtt_control.c"
).read_text(encoding="utf-8")
RTT_BENCH_CONFIG = (
    ROOT / "app/conf/mesh-clicker-rtt-bench.conf"
).read_text(encoding="utf-8")
RTT_CONTROL_CONFIG = (
    ROOT / "app/conf/mesh-clicker-rtt-control.conf"
).read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", CLICKER, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = CLICKER.index("{", match.start())
    depth = 0
    for index in range(brace, len(CLICKER)):
        depth += CLICKER[index] == "{"
        depth -= CLICKER[index] == "}"
        if depth == 0:
            return CLICKER[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


def source_function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


# RTT is an extra supervised input on the normal clicker, never a replacement
# for the physical GPIO path. RTT overlays enlarge only the debug buffer; startup
# must arm the button before polling RTT, and RTT must inject through the same
# gesture machine and bounded action FIFO as a real press/release pair.  This
# matched bench also opts into the explicit two-anchor click wire contract.
rtt_bench_options = {
    line.strip()
    for line in RTT_BENCH_CONFIG.splitlines()
    if line.strip() and not line.lstrip().startswith("#")
}
assert rtt_bench_options == {
    "CONFIG_IMEC_CLICKER_RTT_CONTROL=y",
    "CONFIG_IMEC_TWO_ANCHOR_CLICK_BENCH=y",
    "CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192",
}
rtt_control_options = {
    line.strip()
    for line in RTT_CONTROL_CONFIG.splitlines()
    if line.strip() and not line.lstrip().startswith("#")
}
assert rtt_control_options == {
    "CONFIG_IMEC_CLICKER_RTT_CONTROL=y",
    "CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192",
}
main = source_function_body(MAIN, "main")
button_start = main.index("app_clicker_button_init()")
rtt_start = main.index("app_clicker_rtt_control_start()", button_start)
assert button_start < rtt_start
rtt_dispatch = source_function_body(
    RTT_CONTROL, "app_clicker_rtt_dispatch_line"
)
assert "app_clicker_inject_button_gesture(gesture)" in rtt_dispatch
gesture_inject = function_body("app_clicker_inject_button_gesture")
assert "click_button_gesture_handle(BUTTON_SIGNAL_PRESS" in gesture_inject
assert "click_button_gesture_handle(BUTTON_SIGNAL_RELEASE" in gesture_inject
assert gesture_inject.count("app_clicker_submit_button_action(action)") == 2
for physical_wrapper in (
    "click_button_handle_signal_at(",
    "click_button_handle_signal(",
):
    assert physical_wrapper not in gesture_inject, (
        "RTT gesture injection must not pass through the physical-button "
        "provenance wrapper"
    )

physical_signal = function_body("click_button_handle_signal_at")
gesture = physical_signal.index("click_button_gesture_handle(")
trace = physical_signal.index(
    'status_debug_printf("DBG_CLICKER_BUTTON signal=%u source=%s at=%u '
    'action=%u ret=%d"',
    gesture,
)
reject = physical_signal.index("if (ret != PROTO_OK)", trace)
assert gesture < trace < reject, (
    "every physical signal outcome must be traced before success or rejection"
)
for trace_argument in (
    "(unsigned int)signal",
    'source == NULL ? "unknown" : source',
    "signal_at_ms",
    "(unsigned int)action",
    "ret",
):
    assert trace_argument in physical_signal[trace:reject], (
        f"physical-button provenance trace is missing {trace_argument}"
    )


poweroff = function_body("clicker_systemoff_now")
guard = poweroff.index("if (!click_button_systemoff_wake_armed)")
reset = poweroff.index("click_button_recovery_reset", guard)
systemoff = poweroff.index("sys_poweroff()")
assert guard < reset < systemoff, (
    "system-off must fail closed unless the physical-low wake source was armed"
)

arm = function_body("click_button_try_arm_systemoff_wake")
clear = arm.index("click_button_systemoff_wake_armed = false")
level_arm = arm.index("GPIO_INT_LEVEL_LOW")
arm_check = arm.index("if (ret < 0)", level_arm)
commit = arm.index("click_button_systemoff_wake_armed = true")
assert clear < level_arm < arm_check < commit, (
    "wake admission may commit only after the GPIO driver accepts level-low"
)
assert arm.count("click_button_pressed()") >= 2, (
    "system-off admission must prove a stable released button before arming"
)

enter = function_body("app_clicker_enter_systemoff_idle")
attempt = enter.index("ret = click_button_try_arm_systemoff_wake()")
success = enter.index("if (ret == 0)", attempt)
ready = enter.index("BUTTON_WAKE_RECOVERY_POWER_READY", success)
poweroff_call = enter.index("clicker_systemoff_now()", ready)
retry = enter.index("ret == -EBUSY ? BUTTON_WAKE_OBSERVATION_WAITING", poweroff_call)
assert attempt < success < ready < poweroff_call < retry
assert "entering system-off without wake arm" not in enter
assert "GPIO_DISCONNECTED" not in enter

release_poll = function_body("click_button_release_work_handler")
read_failure = release_poll.index("if (pressed < 0)")
retry_release = release_poll.index(
    "click_button_retry_release_poll(pressed)", read_failure
)
assert read_failure < retry_release
poll_rearm_failure = release_poll.index(
    'click_button_schedule_rearm_recovery(\n'
    '            ret, "release_poll_rearm")'
)
assert retry_release < poll_rearm_failure
release_signal = release_poll.index("BUTTON_SIGNAL_RELEASE")
finish_cycle = release_poll.index(
    "click_button_finish_press_cycle()", release_signal
)
release_rearm = release_poll.index(
    "click_button_arm_idle_interrupt()", finish_cycle
)
assert release_signal < finish_cycle < release_rearm, (
    "release polling must retire the exact press cycle before rearming a new "
    "edge owner"
)

irq = function_body("click_button_work_handler")
pending = irq.index("click_button_press_is_pending()")
latched_disable = irq.index("GPIO_INT_DISABLE", pending)
claim = irq.index("click_button_claim_press", latched_disable)
latched_press = irq.index(
    "click_button_handle_signal_at(BUTTON_SIGNAL_PRESS", claim
)
release_owner = irq.index(
    "click_button_release_work", latched_press
)
later_level_sample = irq.index("pressed = click_button_pressed()", release_owner)
assert (
    pending
    < latched_disable
    < claim
    < latched_press
    < release_owner
    < later_level_sample
), (
    "an ISR-owned press must reach the FSM with its captured timestamp before "
    "a delayed worker samples the later GPIO level"
)
irq_read_failure = irq.index("if (pressed < 0)")
irq_read_recovery = irq.index(
    'click_button_schedule_rearm_recovery(\n'
    '            pressed, "irq_read")',
    irq_read_failure,
)
irq_disable_failure = irq.index(
    'click_button_schedule_rearm_recovery(\n'
    '                ret, "irq_press_disable")',
    irq_read_recovery,
)
irq_rearm_failure = irq.index(
    'click_button_schedule_rearm_recovery(\n'
    '            ret, "release_irq_rearm")'
)
assert (
    irq_read_failure
    < irq_read_recovery
    < irq_disable_failure
    < irq_rearm_failure
)

button_isr = function_body("click_button_isr")
isr_latch = button_isr.index("click_button_latch_press(k_uptime_get_32())")
isr_submit = button_isr.index("k_work_submit(&click_button_work)", isr_latch)
isr_fail_closed = button_isr.index(
    "app_watchdog_stop_feeding()", isr_submit
)
assert isr_latch < isr_submit < isr_fail_closed, (
    "the ISR must own and timestamp the press before deferring work, and a "
    "lost worker submission must stop watchdog feeds"
)

rearm = function_body("click_button_rearm_work_handler")
assert "click_button_schedule_rearm_recovery" in rearm
assert "BUTTON_SIGNAL_PRESS" in rearm
assert "click_button_release_work" in rearm
assert "BUTTON_WAKE_OBSERVATION_ARMED" in rearm
assert rearm.index("click_button_press_is_pending()") < rearm.index(
    "pressed = click_button_pressed()"
), "recovery must service an ISR-owned press before sampling a later level"

idle_arm = function_body("click_button_arm_idle_interrupt")
callback = idle_arm.index("click_button_ensure_callback_registered()")
interrupt_arm = idle_arm.index("GPIO_INT_EDGE_TO_ACTIVE")
post_arm_sample = idle_arm.index("pressed = click_button_pressed()", interrupt_arm)
submit = idle_arm.index("k_work_submit(&click_button_work)", post_arm_sample)
assert callback < interrupt_arm < post_arm_sample < submit, (
    "a recovered edge interrupt is useless unless its callback is registered"
)
post_arm_latch = idle_arm.index(
    "click_button_press_at_ms = k_uptime_get_32()", post_arm_sample
)
assert post_arm_sample < post_arm_latch < submit, (
    "the post-arm race closure must retain the observed press before work "
    "submission"
)
assert "GPIO_INT_DISABLE" in idle_arm[post_arm_sample:], (
    "post-arm sampling failures must not leave an unmonitored interrupt armed"
)
edge_lock = idle_arm.index("k_spin_lock(&click_button_edge_lock)")
choose_owner = idle_arm.index("release_owner = click_button_press_cycle_active")
edge_unlock = idle_arm.index("k_spin_unlock(&click_button_edge_lock")
assert edge_lock < choose_owner < interrupt_arm < post_arm_latch < edge_unlock < submit
assert "GPIO_INT_EDGE_TO_INACTIVE" in idle_arm
assert "CLICK_BUTTON_RELEASE_POLL_MS" not in release_poll, (
    "a healthy held button must wait for its release edge without periodic polling"
)
assert "click_button_arm_idle_interrupt()" in release_poll
assert "click_button_release_irq_armed" in button_isr
assert button_isr.index("k_work_reschedule(&click_button_release_work") < isr_latch

systemoff_capture = function_body("clicker_capture_systemoff_button_action")
disable = systemoff_capture.index("GPIO_INT_DISABLE")
disable_failure = systemoff_capture.index("if (ret < 0)", disable)
press_signal = systemoff_capture.index("BUTTON_SIGNAL_PRESS", disable_failure)
assert disable < disable_failure < press_signal, (
    "system-off wake capture must not consume a press while IRQ disable failed"
)

button_init = function_body("app_clicker_button_init")
init_failure = button_init.index("if (ret < 0)")
recovery = button_init.index(
    'click_button_schedule_rearm_recovery(\n'
    '            ret, "button_init")',
    init_failure,
)
assert init_failure < recovery
assert "gpio_remove_callback" not in button_init
close_race_submit = button_init.rindex("k_work_submit(&click_button_work)")
close_race_failure = button_init.index("if (ret < 0)", close_race_submit)
close_race_recovery = button_init.index(
    '"button_init_close_race"', close_race_failure
)
assert close_race_submit < close_race_failure < close_race_recovery, (
    "the startup race-closure work item must retain recovery ownership when "
    "the system queue rejects its submission"
)

action_worker = function_body("clicker_action_work_handler")
assert action_worker.count("app_clicker_handle_button_action(action)") == 1, (
    "one queued button action must execute exactly once"
)
take = action_worker.index("button_action_handoff_take")
handle = action_worker.index("app_clicker_handle_button_action(action)", take)
idle = action_worker.index("app_clicker_enter_idle()", handle)
release = action_worker.index("button_action_handoff_release_if_empty", idle)
inactive = action_worker.index("atomic_set(&clicker_action_active, 0)", release)
assert take < handle < idle < release < inactive, (
    "the worker must retain ownership through action execution and idle re-arm"
)
watchdog_progress_begin = action_worker.index(
    "app_watchdog_note_clicker_action_progress", take
)
watchdog_progress_end = action_worker.index(
    "app_watchdog_note_clicker_action_progress",
    watchdog_progress_begin + 1,
)
watchdog_release = action_worker.index(
    "app_watchdog_clicker_action_end", release
)
assert (
    take
    < watchdog_progress_begin
    < handle
    < watchdog_progress_end
    < release
    < watchdog_release
    < inactive
), "action queue ownership must carry an exact watchdog generation"

action_handler = function_body("app_clicker_handle_button_action")
assert "app_clicker_enter_idle()" not in action_handler, (
    "action handlers must not re-arm idle before the serialized owner drains"
)

action_submit = function_body("app_clicker_submit_button_action")
assert "button_action_handoff_submit" in action_submit
assert "clicker_action_submit_or_recover" in action_submit
handoff_start = action_submit.index("BUTTON_ACTION_HANDOFF_START_OWNER")
watchdog_begin = action_submit.index(
    "app_watchdog_clicker_action_begin()", handoff_start
)
work_submit = action_submit.index(
    "clicker_action_submit_or_recover", watchdog_begin
)
assert handoff_start < watchdog_begin < work_submit, (
    "the watchdog lease must start when FIFO ownership is admitted, before "
    "the action worker can stall in the queue"
)
assert "action_drop reason=busy" not in CLICKER
assert "ignored while previous action" not in CLICKER

submit_recovery = function_body("clicker_action_submit_or_recover")
failure = submit_recovery.index("if (ret >= 0)")
retain_retry = submit_recovery.index(
    "k_work_reschedule(&clicker_action_submit_retry_work", failure
)
reset = submit_recovery.index(
    'click_button_recovery_reset("action_submit_retry_schedule"', retain_retry
)
assert failure < retain_retry < reset, (
    "a failed work submission must retain ownership and retry or reset"
)

self_test_timeout = function_body("app_clicker_arm_self_test_timeout")
assert "click_button_reschedule_or_reset" in self_test_timeout
assert '"self_test_arm_timeout"' in self_test_timeout

# Run the actual GPIO/work handlers with the real button FSM and action FIFO.
# Hooks move the pin inside IRQ arming and fault GPIO calls; the fake scheduler
# exposes whether an otherwise healthy held button still schedules polling.
native = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include "app_clicker_event_runtime.h"
#include "button_action_handoff.h"
#include "button_wake_recovery.h"
#define GPIO_INPUT 1
#define GPIO_INT_DISABLE 0
#define GPIO_INT_EDGE_TO_ACTIVE 1
#define GPIO_INT_EDGE_TO_INACTIVE 2
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
#define ARG_UNUSED(x) (void)(x)
typedef int atomic_t;
static int atomic_get(atomic_t *a) { return *a; }
static void atomic_set(atomic_t *a, int value) { *a = value; }
struct device { int unused; };
struct gpio_callback { int unused; };
struct gpio_dt_spec { void *port; unsigned pin; };
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static int spin_depth;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *s)
{ (void)s; assert(spin_depth == 0); return ++spin_depth; }
static void k_spin_unlock(struct k_spinlock *s, k_spinlock_key_t key)
{ (void)s; assert(key == spin_depth && spin_depth == 1); spin_depth--; }
struct k_work { bool queued; };
struct k_work_delayable { struct k_work work; uint32_t due; bool scheduled; };
static struct k_work click_button_work;
static struct k_work_delayable click_button_release_work, click_button_rearm_work;
static struct gpio_dt_spec click_button;
static struct gpio_callback click_button_cb;
static struct k_spinlock click_button_edge_lock;
static bool click_button_press_pending, click_button_press_cycle_active;
static uint32_t click_button_press_at_ms;
static atomic_t click_button_release_irq_armed, clicker_action_active;
static bool click_button_callback_initialized = true, click_button_callback_registered;
static struct button_wake_recovery click_button_release_recovery, click_button_rearm_recovery;
static struct app_clicker_event_runtime clicker_event_runtime;
static struct button_action_handoff handoff;
static uint32_t now_ms;
static unsigned schedules, gpio_arm_failures, gpio_read_failures;
static int pin_level, irq_mode, arm_hook;
static bool stopped;
static jmp_buf recovery_target;
static uint32_t k_uptime_get_32(void) { return now_ms; }
static void log_sink(const char *fmt, ...) { (void)fmt; }
#define LOG_ERR(...) log_sink(__VA_ARGS__)
#define LOG_WRN(...) log_sink(__VA_ARGS__)
#define status_debug_printf(...) log_sink(__VA_ARGS__)
static int gpio_is_ready_dt(const void *p) { (void)p; return 1; }
static int gpio_pin_configure_dt(const void *p, int flags)
{ (void)p; (void)flags; return 0; }
static int gpio_add_callback(void *p, struct gpio_callback *cb)
{ (void)p; (void)cb; return 0; }
static int gpio_pin_get_raw(void *p, unsigned pin)
{
    (void)p; (void)pin;
    if (gpio_read_failures) { gpio_read_failures--; return -EIO; }
    return pin_level;
}
static int gpio_pin_interrupt_configure(void *p, unsigned pin, int flags)
{
    (void)p; (void)pin;
    if (flags != GPIO_INT_DISABLE && gpio_arm_failures) {
        gpio_arm_failures--; return -EIO;
    }
    irq_mode = flags;
    if ((arm_hook == 1 && flags == GPIO_INT_DISABLE) ||
        (arm_hook == 2 && flags == GPIO_INT_EDGE_TO_INACTIVE)) {
        pin_level = 1; arm_hook = 0;
    } else if (arm_hook == 3 && flags == GPIO_INT_EDGE_TO_ACTIVE) {
        pin_level = 0; arm_hook = 0;
    }
    return 0;
}
static int k_work_submit(struct k_work *work)
{ assert(spin_depth == 0); work->queued = true; return 1; }
static int k_work_reschedule(struct k_work_delayable *work, unsigned delay)
{
    assert(spin_depth == 0); work->due = now_ms + delay;
    work->scheduled = true; schedules++; return 1;
}
static int k_work_cancel_delayable(struct k_work_delayable *work)
{ work->scheduled = false; return 0; }
static bool clicker_systemon_retained_idle_enabled(void) { return true; }
static void app_watchdog_stop_feeding(void) { stopped = true; }
static void click_button_recovery_reset(const char *reason, int ret)
{ (void)reason; assert(ret < 0); stopped = true; longjmp(recovery_target, 1); }
static void app_clicker_submit_button_action(enum button_action action)
{
    if (action != BUTTON_ACTION_NONE) {
        assert(button_action_handoff_submit(&handoff, action) != BUTTON_ACTION_HANDOFF_FULL);
        clicker_action_active = 1;
    }
}
'''
config = (ROOT / "app/src/app_config.h").read_text(encoding="utf-8")
for source, names in (
    (CLICKER, ("CLICK_BUTTON_RECOVERY_MAX_FAILURES", "CLICK_BUTTON_RECOVERY_RETRY_MS")),
    (config, ("CLICK_BUTTON_RELEASE_STABLE_MS",)),
):
    for name in names:
        native += next(line for line in source.splitlines()
                       if line.startswith(f"#define {name} ")) + "\n"
for name in (
    "click_button_latch_press", "click_button_press_is_pending",
    "click_button_press_cycle_is_active", "click_button_claim_press",
    "click_button_finish_press_cycle", "click_button_pressed", "click_button_clear_latch",
    "click_button_gesture_handle", "click_button_configure_input",
    "click_button_ensure_callback_registered", "click_button_arm_idle_interrupt",
    "click_button_handle_signal_at", "click_button_handle_signal",
    "click_button_reschedule_or_reset", "click_button_schedule_rearm_recovery",
    "click_button_retry_release_poll", "click_button_release_work_handler",
    "click_button_rearm_work_handler", "click_button_work_handler", "click_button_isr",
):
    prefix = re.search(rf"(?m)^(static \w+ )(?={name}\()", CLICKER)
    assert prefix is not None, f"missing return type for {name}"
    native += prefix.group(1) + function_body(name) + "\n"

native += r'''
static void run_due(void)
{
    for (unsigned iteration = 0; iteration < 32u; iteration++) {
        if (click_button_work.queued) {
            click_button_work.queued = false;
            click_button_work_handler(&click_button_work);
        } else if (click_button_release_work.scheduled &&
                   (int32_t)(now_ms - click_button_release_work.due) >= 0) {
            click_button_release_work.scheduled = false;
            click_button_release_work_handler(&click_button_release_work.work);
        } else if (click_button_rearm_work.scheduled &&
                   (int32_t)(now_ms - click_button_rearm_work.due) >= 0) {
            click_button_rearm_work.scheduled = false;
            click_button_rearm_work_handler(&click_button_rearm_work.work);
        } else { return; }
    }
    assert(!"unbounded immediate gesture work");
}
static void edge(int level)
{
    int previous = pin_level;
    pin_level = level;
    if ((irq_mode == GPIO_INT_EDGE_TO_ACTIVE && previous == 1 && level == 0) ||
        (irq_mode == GPIO_INT_EDGE_TO_INACTIVE && previous == 0 && level == 1)) {
        click_button_isr(NULL, NULL, 1u);
    }
}
static void reset_fixture(void)
{
    now_ms = 100u; pin_level = 1; irq_mode = GPIO_INT_DISABLE; arm_hook = 0;
    gpio_arm_failures = gpio_read_failures = schedules = 0u; stopped = false;
    click_button_work.queued = click_button_release_work.scheduled = false;
    click_button_rearm_work.scheduled = false;
    click_button_press_pending = click_button_press_cycle_active = false;
    click_button_release_irq_armed = clicker_action_active = 0;
    click_button_press_at_ms = 0u;
    app_clicker_event_runtime_init(&clicker_event_runtime);
    button_action_handoff_init(&handoff);
    button_wake_recovery_init(&click_button_release_recovery, CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    button_wake_recovery_init(&click_button_rearm_recovery, CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    assert(click_button_arm_idle_interrupt() == 0);
}
static void begin_press(void)
{
    edge(0); run_due(); now_ms += CLICK_BUTTON_RELEASE_STABLE_MS; run_due();
    assert(click_button_press_cycle_active && irq_mode == GPIO_INT_EDGE_TO_INACTIVE);
    assert(!click_button_release_work.scheduled);
}
static void finish_press(void) { now_ms += 60u; edge(1); run_due(); }
int main(void)
{
    enum button_action action;
    reset_fixture(); begin_press();
    unsigned before = schedules;
    now_ms += 3600000u; run_due();
    assert(schedules == before); /* a held button adds no periodic work */
    finish_press();
    assert(handoff.count == 1u);
    assert(button_action_handoff_take(&handoff, &action) && action == BUTTON_ACTION_SELF_TEST_ARMED);
    begin_press(); finish_press();
    assert(button_action_handoff_take(&handoff, &action) && action == BUTTON_ACTION_SELF_TEST_START);

    /* Release before/while arming is retained by the post-arm level sample. */
    for (int phase = 1; phase <= 2; phase++) {
        reset_fixture(); edge(0); run_due();
        now_ms += CLICK_BUTTON_RELEASE_STABLE_MS; arm_hook = phase; run_due();
        assert(!click_button_press_cycle_active && handoff.count == 1u);
        assert(irq_mode == GPIO_INT_EDGE_TO_ACTIVE);
        begin_press(); finish_press();
        assert(handoff.count == 2u); /* two gestures remain in the action FIFO */
    }
    /* A press inside idle re-arm owns its timestamp before delayed work. */
    reset_fixture(); arm_hook = 3;
    assert(click_button_arm_idle_interrupt() == 0);
    assert(click_button_press_pending && click_button_press_at_ms == now_ms);
    run_due(); now_ms += 100u; run_due(); finish_press();
    assert(handoff.count == 1u);

    /* An old action's idle transition must preserve a newer release owner. */
    reset_fixture(); begin_press(); finish_press(); begin_press();
    assert(click_button_arm_idle_interrupt() == 0);
    assert(irq_mode == GPIO_INT_EDGE_TO_INACTIVE && !click_button_release_work.scheduled);
    finish_press(); assert(handoff.count == 2u);

    /* A bounced release that is already held again does not complete twice. */
    reset_fixture(); begin_press(); edge(1); edge(0); run_due();
    assert(click_button_press_cycle_active && handoff.count == 0u);
    assert(!click_button_release_work.scheduled);
    finish_press(); run_due(); assert(handoff.count == 1u);

    /* Read and arm errors own bounded work; a recovered owner accepts next clicks. */
    for (int read_fault = 0; read_fault < 2; read_fault++) {
        reset_fixture(); edge(0); run_due(); now_ms += CLICK_BUTTON_RELEASE_STABLE_MS;
        if (read_fault) { gpio_read_failures = 1u; }
        else { gpio_arm_failures = 1u; }
        run_due(); assert(click_button_release_work.scheduled && !stopped);
        now_ms += CLICK_BUTTON_RECOVERY_RETRY_MS; run_due();
        assert(!click_button_release_work.scheduled && irq_mode == GPIO_INT_EDGE_TO_INACTIVE);
        finish_press(); begin_press(); finish_press(); assert(handoff.count == 2u);
    }
    reset_fixture(); edge(0); run_due(); gpio_arm_failures = 100u;
    now_ms += CLICK_BUTTON_RELEASE_STABLE_MS;
    if (setjmp(recovery_target) == 0) {
        for (;;) { run_due(); now_ms += CLICK_BUTTON_RECOVERY_RETRY_MS; }
    }
    assert(stopped && click_button_release_recovery.consecutive_failures ==
                      CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="imec-button-release-") as directory:
    source_path = Path(directory) / "button_release.c"
    source_path.write_text(native, encoding="utf-8")
    binary = Path(directory) / "button_release"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
        "-I", str(ROOT / "app/src"), str(source_path),
        str(ROOT / "app/src/app_clicker_event_runtime.c"),
        str(ROOT / "src/firmware_state_click.c"),
        str(ROOT / "src/firmware_events.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)

print("clicker button wake source invariants passed")
