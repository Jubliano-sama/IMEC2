#!/usr/bin/env python3
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "app" / "src" / "app_watchdog.c").read_text()
MAIN = (ROOT / "app" / "src" / "main.c").read_text()
ANCHOR_RADIO = (ROOT / "app" / "src" / "app_anchor_radio.inc").read_text()
MESH_RX = (ROOT / "app" / "src" / "app_mesh_report_rx.inc").read_text()
MESH_TRANSPORT = (
    ROOT / "app" / "src" / "app_mesh_report_transport.inc"
).read_text()
MESH_DELIVERY = (
    ROOT / "app" / "src" / "app_mesh_report_delivery.inc"
).read_text()
MESH_DIRECT_GATEWAY = (
    ROOT / "app" / "src" / "app_mesh_report_direct_gateway.inc"
).read_text()
SURVEY_DISCOVERY = (
    ROOT / "app" / "src" / "app_anchor_survey_discovery.c"
).read_text()
SURVEY_RUNTIME = (
    ROOT / "app" / "src" / "app_anchor_survey_runtime.c"
).read_text()
SURVEY_HEADER = (ROOT / "include" / "survey.h").read_text()
WATCHDOG_HEADER = (ROOT / "app" / "src" / "app_watchdog.h").read_text()
KCONFIG = (ROOT / "app" / "Kconfig").read_text()
CMAKE = (ROOT / "app" / "CMakeLists.txt").read_text()
BYPASS_CONF = (ROOT / "app" / "conf" / "watchdog-bypass.conf").read_text()


def function_body(source: str, name: str) -> str:
    definition = re.search(
        rf"(?m)^[A-Za-z_][^;\n]*\b{re.escape(name)}\s*\([^;]*?\)\s*\n?\{{",
        source,
    )
    if definition is None:
        raise AssertionError(f"missing function {name}")
    brace = source.index("{", definition.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


def braced_statement_end(source: str, statement_start: int) -> int:
    brace = source.index("{", statement_start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    raise AssertionError("unterminated braced statement")


def uint_macro(source: str, name: str) -> int:
    match = re.search(
        rf"(?m)^#define\s+{re.escape(name)}\s+([0-9]+)u\s*$",
        source,
    )
    if match is None:
        raise AssertionError(f"missing simple unsigned macro {name}")
    return int(match.group(1))


class WatchdogAdoptionSourceTests(unittest.TestCase):
    def require(self, needle: str) -> None:
        if needle not in SOURCE:
            self.fail(f"app_watchdog.c is missing required invariant: {needle}")

    def test_production_watchdog_is_a_last_resort_not_an_operation_timer(self) -> None:
        hardware_ms = uint_macro(
            WATCHDOG_HEADER, "APP_WATCHDOG_HARDWARE_TIMEOUT_MS"
        )
        lease_ms = uint_macro(
            WATCHDOG_HEADER, "APP_WATCHDOG_PROGRESS_LEASE_MS"
        )
        startup_ms = uint_macro(
            WATCHDOG_HEADER, "APP_WATCHDOG_STARTUP_GRACE_MS"
        )

        self.assertEqual(hardware_ms, 24 * 60 * 60 * 1000)
        self.assertEqual(lease_ms, 12 * 60 * 60 * 1000)
        self.assertEqual(startup_ms, 60 * 60 * 1000)
        self.assertLess(lease_ms, hardware_ms)

    def test_running_hardware_is_detected_before_zephyr_setup(self) -> None:
        detect = SOURCE.find("nrf_wdt_started_check(NRF_WDT0)")
        install = SOURCE.find("wdt_install_timeout(watchdog_device, &timeout)")
        self.assertGreaterEqual(detect, 0)
        self.assertGreater(install, detect)

    def test_all_enabled_reload_requests_are_enumerated_and_fed(self) -> None:
        self.require("index < WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS")
        self.require("nrf_wdt_reload_request_enable_check")
        self.require("nrf_wdt_reload_request_set")
        self.require("feed_inherited_watchdog(true)")

    def test_stale_and_explicit_stop_gate_every_feed_path(self) -> None:
        handler = SOURCE[SOURCE.index("static void watchdog_timer_handler") :]
        stale_return = handler.find(
            "if (system_stale || radio_stale || clicker_action_stale)"
        )
        inherited_feed = handler.find("feed_inherited_watchdog(true)")
        zephyr_feed = handler.find("wdt_feed(watchdog_device")
        self.assertGreater(stale_return, 0)
        self.assertGreater(inherited_feed, stale_return)
        self.assertGreater(zephyr_feed, stale_return)
        self.require("atomic_set(&feeding_stopped, 1)")

    def test_active_clicker_action_has_a_generation_bound_lease(self) -> None:
        handler = SOURCE[SOURCE.index("static void watchdog_timer_handler") :]
        snapshot_begin = handler.find(
            "atomic_get(&clicker_action_active_generation)"
        )
        policy = handler.find("app_watchdog_action_lease_stale(", snapshot_begin)
        stale_gate = handler.find(
            "if (system_stale || radio_stale || clicker_action_stale)",
            policy,
        )
        feed = handler.find("feed_inherited_watchdog(true)", stale_gate)
        self.assertGreaterEqual(snapshot_begin, 0)
        self.assertGreater(policy, snapshot_begin)
        self.assertGreater(stale_gate, policy)
        self.assertGreater(feed, stale_gate)
        self.require("app_watchdog_clicker_action_begin")
        self.require("app_watchdog_note_clicker_action_progress")
        self.require("app_watchdog_clicker_action_end")

    def test_watchdog_init_failure_never_enters_role_runtime(self) -> None:
        init = MAIN.find("ret = app_watchdog_init();")
        failure = MAIN.find("if (ret < 0)", init)
        fail_closed = MAIN.find("watchdog_init_fail_closed(ret)", failure)
        first_runtime_init = MAIN.find("battery_adc_divider_disable()", fail_closed)
        self.assertGreaterEqual(init, 0)
        self.assertGreater(failure, init)
        self.assertGreater(fail_closed, failure)
        self.assertGreater(first_runtime_init, fail_closed)

        helper_start = MAIN.find("static void watchdog_init_fail_closed")
        helper_end = MAIN.find("\n}\n", helper_start)
        helper = MAIN[helper_start : helper_end + 3]
        stop = helper.find("app_watchdog_stop_feeding()")
        delay = helper.find("k_msleep(APP_WATCHDOG_INIT_RETRY_DELAY_MS)", stop)
        reboot = helper.find("sys_reboot(SYS_REBOOT_COLD)", delay)
        idle = helper.find("k_cpu_idle()", reboot)
        self.assertGreaterEqual(stop, 0)
        self.assertGreater(delay, stop)
        self.assertGreater(reboot, delay)
        self.assertGreater(idle, reboot)

    def test_required_runtime_startup_failures_reboot_fail_closed(self) -> None:
        helper_start = MAIN.find("static void runtime_start_fail_closed")
        helper_end = MAIN.find("\n}\n", helper_start)
        helper = MAIN[helper_start : helper_end + 3]
        stop = helper.find("app_watchdog_stop_feeding()")
        delay = helper.find("k_msleep(APP_WATCHDOG_INIT_RETRY_DELAY_MS)", stop)
        reboot = helper.find("sys_reboot(SYS_REBOOT_COLD)", delay)
        idle = helper.find("k_cpu_idle()", reboot)
        self.assertGreaterEqual(stop, 0)
        self.assertGreater(delay, stop)
        self.assertGreater(reboot, delay)
        self.assertGreater(idle, reboot)

        required_failures = (
            "node communication initialization",
            "anchor role startup",
            "anchor UWB mesh RX startup",
            "gateway role startup",
            "gateway UWB mesh RX startup",
        )
        for phase in required_failures:
            self.assertRegex(
                MAIN,
                rf'runtime_start_fail_closed\s*\(\s*"{re.escape(phase)}"',
                f"{phase} must not leave the watchdog feeding a partial role",
            )

    def test_boot_evidence_distinguishes_fresh_inherited_and_invalid(self) -> None:
        for mode in ("fresh", "inherited", "invalid"):
            self.require(f"DBG_WATCHDOG_BOOT mode={mode}")
        self.require("immediate=1")
        self.require("rr=0x%02x")

    def test_opt_in_bench_bypass_never_arms_fresh_watchdog(self) -> None:
        init = function_body(SOURCE, "app_watchdog_init")
        bypass = init.index("IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)")
        bypass_end = braced_statement_end(init, bypass)
        setup = init.index("wdt_setup(watchdog_device", bypass)
        bypass_body = init[bypass:bypass_end]

        self.assertIn("WATCHDOG_ADOPTION_INHERITED", bypass_body)
        self.assertIn("feed_inherited_watchdog(true)", bypass_body)
        self.assertIn("return 0;", bypass_body)
        self.assertNotIn("wdt_install_timeout", bypass_body)
        self.assertIn("DBG_WATCHDOG_BOOT mode=bypass", bypass_body)
        self.assertLess(bypass_end, setup)

        stop = function_body(SOURCE, "app_watchdog_stop_feeding")
        self.assertLess(
            stop.index("IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)"),
            stop.index("atomic_set(&feeding_stopped, 1)"),
        )
        self.assertIn("DBG_WATCHDOG_BYPASS_STOP_IGNORED", stop)
        self.assertIn("config IMEC_WATCHDOG_BYPASS", KCONFIG)
        self.assertIn("CONFIG_IMEC_WATCHDOG_BYPASS=y", BYPASS_CONF)
        self.assertIn("IMEC_WATCHDOG_BYPASS_BUILD", CMAKE)
        bypass_gate = CMAKE[
            CMAKE.index("if(IMEC_WATCHDOG_BYPASS_BUILD)") :
            CMAKE.index("find_package(Zephyr", CMAKE.index("if(IMEC_WATCHDOG_BYPASS_BUILD)"))
        ]
        self.assertIn(
            'NOT IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop"',
            bypass_gate,
        )
        self.assertNotIn("mesh_transmitter_forcedhop", bypass_gate)

    def test_reset_cause_is_captured_before_hardware_latches_are_cleared(self) -> None:
        capture = SOURCE.find("hwinfo_get_reset_cause(&watchdog_health.reset_cause)")
        clear = SOURCE.find("hwinfo_clear_reset_cause()")
        marker = SOURCE.find("DBG_WATCHDOG_BOOT mode=invalid")
        self.assertGreaterEqual(capture, 0)
        self.assertGreater(clear, capture)
        self.assertGreater(marker, clear)

    def test_anchor_scan_progress_requires_a_functional_bounded_receive(self) -> None:
        handler = function_body(ANCHOR_RADIO, "anchor_uwb_scan_work_handler")
        progress = "app_watchdog_note_radio_progress();"
        acquire = handler.index(
            "ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,"
        )
        failure = handler.index("if (ret < 0)", acquire)
        failure_end = braced_statement_end(handler, failure)
        configure = handler.index(
            "ret = dwm3000_driver_configure_wake_mode();", failure_end
        )
        attempted = handler.index("rx_attempted = true;", configure)
        receive = handler.index(
            "ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(",
            attempted,
        )
        low_power = handler.index(
            "low_power_ret = anchor_enter_low_power(", receive
        )
        release = handler.index(
            "release_ret = radio_guard_uwb_release_finish(&radio_lease,",
            low_power,
        )
        release_failure = handler.index("if (release_ret < 0)", release)
        feed = handler.index(progress, release)

        self.assertEqual(
            handler.count(progress),
            1,
            "anchor scan must expose only one functional-progress feed",
        )
        self.assertNotIn(
            progress,
            handler[:failure_end],
            "a deferred scan that never acquired the radio renewed its owner's lease",
        )
        self.assertNotIn(
            progress,
            handler[failure_end:receive],
            "guard acquisition or radio configuration renewed the lease before RX",
        )
        self.assertLess(
            configure,
            attempted,
            "the RX-attempt marker must be reachable only after configure succeeds",
        )
        self.assertLess(
            attempted,
            receive,
            "the RX-attempt marker must describe the bounded receive call",
        )
        self.assertLess(low_power, release)
        self.assertLess(release_failure, feed)
        self.assertLess(release, feed)
        self.assertRegex(
            handler[release:],
            r"if\s*\(\s*low_power_ret\s*==\s*0\s*&&\s*rx_attempted\s*&&\s*"
            r"\(\s*ret\s*==\s*0\s*\|\|\s*ret\s*==\s*-ETIMEDOUT\s*\|\|\s*"
            r"app_anchor_rx_failure_detected_preamble\s*\(\s*rx_failure\s*\)"
            r"\s*\)\s*\)\s*\{\s*app_watchdog_note_radio_progress\s*\(\s*\)"
            r"\s*;\s*\}",
            "only a completed frame, quiet timeout, or typed RF decode outcome "
            "after successful radio release may renew anchor progress",
        )

    def test_mesh_rx_progress_requires_functional_receive_and_release(self) -> None:
        handler = function_body(MESH_RX, "mesh_uwb_rx_work_handler")
        progress = "app_watchdog_note_radio_progress();"
        typed_outcome = (
            r"functional_rx_outcome\s*=\s*"
            r"(?:functional_rx_outcome\s*\|\|\s*)?"
            r"rx_ret\s*==\s*0\s*\|\|\s*"
            r"rx_ret\s*==\s*-ETIMEDOUT\s*\|\|\s*"
            r"rx_ret\s*==\s*-ECANCELED\s*\|\|\s*"
            r"app_mesh_rx_policy_gateway_ch9_rx_error_recoverable\s*"
            r"\(\s*rx_ret\s*,\s*rx_failure\s*\)\s*;"
        )

        gateway_acquire = handler.index(
            'ret = mesh_rx_radio_claim("mesh gateway continuous channel9 RX",'
        )
        gateway_failure = handler.index("if (ret < 0)", gateway_acquire)
        gateway_failure_end = braced_statement_end(handler, gateway_failure)
        scheduled_acquire = handler.index(
            "ret = mesh_rx_radio_claim(channel9_event ?", gateway_failure_end
        )
        continuous_begin = handler.rindex(
            "bool functional_rx_outcome = false;", 0, gateway_acquire
        )
        continuous = handler[continuous_begin:scheduled_acquire]
        gateway_configure = continuous.index(
            "ret = dwm3000_driver_configure_mesh_payload_mode();"
        )
        gateway_receive = continuous.index(
            "rx_ret = dwm3000_driver_receive_frame_continuous(",
            gateway_configure,
        )
        gateway_stop = continuous.index(
            "release_ret = mesh_rx_radio_finish(&radio_lease, parking_ret);",
            gateway_receive,
        )
        gateway_feed = continuous.index(progress, gateway_stop)

        self.assertEqual(
            continuous.count(progress),
            1,
            "one continuous-loop iteration must have no unconditional feed",
        )
        self.assertNotIn(
            progress,
            continuous[:gateway_receive],
            "gateway guard acquisition or configure renewed progress before RX",
        )
        self.assertEqual(
            continuous.count("functional_rx_outcome ="),
            2,
            "continuous RX must initialize false and change eligibility only "
            "from its driver result",
        )
        self.assertRegex(
            continuous[gateway_receive:gateway_stop],
            typed_outcome,
            "continuous RX must keep frame, quiet-timeout, cancellation, and "
            "typed RF decode outcomes eligible without accepting an untyped "
            "device/SPI error",
        )
        self.assertRegex(
            continuous[gateway_stop:],
            r"(?s)if\s*\(\s*release_ret\s*<\s*0\s*\).*?"
            r"else\s+if\s*\(\s*functional_rx_outcome\s*\)\s*\{\s*"
            r"app_watchdog_note_radio_progress\s*\(\s*\)\s*;\s*\}",
            "a failed idle/standby release or nonfunctional RX must not feed "
            "the gateway lease",
        )
        self.assertLess(gateway_stop, gateway_feed)

        scheduled = handler[scheduled_acquire:]
        scheduled_failure = handler.index("if (ret < 0)", scheduled_acquire)
        scheduled_failure_end = braced_statement_end(handler, scheduled_failure)
        scheduled_configure = handler.index(
            "ret = channel9_event ?", scheduled_failure_end
        )
        scheduled_first_receive = handler.index(
            "dwm3000_driver_receive_frame_continuous_timed(",
            scheduled_configure,
        )
        scheduled_stop = handler.index(
            "radio_release_ret = mesh_rx_radio_finish(&radio_lease, radio_release_ret);",
            scheduled_first_receive,
        )
        scheduled_feed = handler.index(progress, scheduled_stop)

        self.assertEqual(
            scheduled.count(progress),
            1,
            "scheduled/channel-5 RX must expose only one post-operation feed",
        )
        self.assertNotIn(
            progress,
            handler[scheduled_failure_end:scheduled_first_receive],
            "scheduled guard acquisition or configure renewed progress before RX",
        )
        self.assertEqual(
            len(re.findall(typed_outcome, scheduled)),
            3,
            "each typed scheduled/continuous receive path must classify RF "
            "outcomes without treating untyped device/SPI errors as progress",
        )
        self.assertRegex(
            scheduled,
            r"(?s)int\s+rx_ret\s*=\s*dwm3000_driver_receive_frame\s*\(.*?"
            r"functional_rx_outcome\s*=\s*rx_ret\s*==\s*0\s*\|\|\s*"
            r"rx_ret\s*==\s*-ETIMEDOUT\s*;",
            "the bounded receive path without typed failure output must keep "
            "ordinary no-frame timeout eligible",
        )
        self.assertRegex(
            handler[scheduled_stop:],
            r"(?s)if\s*\(\s*radio_release_ret\s*<\s*0\s*\).*?"
            r"else\s+if\s*\(\s*functional_rx_outcome\s*\)\s*\{\s*"
            r"app_watchdog_note_radio_progress\s*\(\s*\)\s*;\s*\}",
            "scheduled RX may feed only after successful radio parking and a "
            "functional bounded receive",
        )
        self.assertLess(scheduled_stop, scheduled_feed)

    def test_preconfigured_ch9_send_defers_progress_to_release_owner(self) -> None:
        progress = "app_watchdog_note_radio_progress();"
        preconfigured = function_body(
            MESH_TRANSPORT,
            "mesh_send_outbound_preconfigured_ch9_locked_until",
        )
        preconfigured_send = preconfigured.index(
            "ret = dwm3000_driver_send_frame_tracked_until("
        )
        preconfigured_failure = preconfigured.index(
            "if (ret < 0)", preconfigured_send
        )
        preconfigured_failure_end = braced_statement_end(
            preconfigured, preconfigured_failure
        )
        success_return = preconfigured.index(
            "return 0;", preconfigured_failure_end
        )
        self.assertGreater(success_return, preconfigured_failure_end)
        self.assertNotIn(
            progress,
            preconfigured,
            "a preconfigured send cannot prove radio progress before its "
            "caller parks the shared slot",
        )

    def test_general_tx_progress_requires_successful_radio_release(self) -> None:
        progress = "app_watchdog_note_radio_progress();"
        released = function_body(
            MESH_TRANSPORT,
            "mesh_send_outbound_with_release_on_channel_until",
        )
        released_send = released.index(
            "ret = dwm3000_driver_send_frame_tracked_until("
        )
        idle_release = released.index(
            "parking_ret = mesh_radio_idle_with_bounded_recovery(",
            released_send,
        )
        mesh_release = released.index(
            "parking_ret = mesh_release_radio_after_mesh_turn(true, reason);",
            idle_release,
        )
        standby_release = released.index(
            "parking_ret = mesh_radio_standby_with_bounded_recovery(",
            mesh_release,
        )
        released_stop = released.index(
            "release_ret = radio_guard_uwb_release_finish(&radio_lease, parking_ret);",
            standby_release,
        )
        release_failure = released.index(
            "if (release_ret < 0)", released_stop
        )
        release_failure_end = braced_statement_end(released, release_failure)
        release_failure_branch = released[
            release_failure:release_failure_end
        ]
        released_failure = released.index("if (ret < 0)", release_failure_end)
        released_failure_end = braced_statement_end(released, released_failure)
        released_progress = released.index(progress, release_failure_end)

        self.assertEqual(
            released.count(progress),
            1,
            "one general TX operation must expose only one functional-progress feed",
        )
        self.assertLess(idle_release, released_stop)
        self.assertLess(mesh_release, released_stop)
        self.assertLess(standby_release, released_stop)
        self.assertLess(released_stop, release_failure)
        self.assertLess(release_failure_end, released_failure)
        self.assertIn(
            "goto out_unlock;",
            released[released_failure:released_failure_end],
            "a failed TX must leave before the progress feed",
        )
        self.assertIn("ret = release_ret;", release_failure_branch)
        self.assertIn(
            "goto out_unlock;",
            release_failure_branch,
            "a permanent idle/standby failure must leave before the progress feed",
        )
        self.assertGreater(released_progress, release_failure_end)

    def test_ch9_release_policy_has_transactional_default_release(self) -> None:
        released = function_body(
            MESH_TRANSPORT,
            "mesh_send_outbound_with_release_on_channel_until",
        )
        release_default = released.index("int release_ret = 0;")
        ch9_release = released.index(
            "if (ret == 0 && radio_channel == UWB_CHANNEL_MESH_PAYLOAD)"
        )
        idle_policy = released.index(
            "if (release_policy == MESH_RADIO_RELEASE_IDLE)",
            ch9_release,
        )
        finish = released.index(
            "release_ret = radio_guard_uwb_release_finish(&radio_lease, parking_ret);",
            idle_policy,
        )
        release_failure = released.index("if (release_ret < 0)", finish)
        ch9_release_body = released[ch9_release:release_failure]

        self.assertLess(release_default, ch9_release)
        self.assertLess(ch9_release, idle_policy)
        self.assertLess(idle_policy, finish)
        self.assertLess(finish, release_failure)
        self.assertIn(
            "parking_ret = mesh_radio_idle_with_bounded_recovery(",
            ch9_release_body,
        )
        self.assertIn(
            "parking_ret = mesh_release_radio_after_mesh_turn(true, reason);",
            ch9_release_body,
        )

    def test_ch9_batch_aggregates_progress_until_successful_slot_release(
        self,
    ) -> None:
        progress = "app_watchdog_note_radio_progress();"
        slot_end = function_body(MESH_TRANSPORT, "mesh_ch9_slot_tx_end")
        snapshot = slot_end.index(
            "functional_tx_completed = ctx->functional_tx_completed;"
        )
        release = slot_end.index(
            "parking_ret = mesh_release_radio_after_mesh_turn(true, \"ch9-slot-tx\");",
            snapshot,
        )
        radio_stop = slot_end.index(
            "release_ret = radio_guard_uwb_release_finish(&ctx->radio_lease,",
            release,
        )
        release_failure = slot_end.index("if (release_ret < 0)", radio_stop)
        release_failure_end = braced_statement_end(slot_end, release_failure)
        clear = slot_end.index(
            "ctx->functional_tx_completed = false;", release_failure_end
        )
        functional_gate = slot_end.index(
            "if (functional_tx_completed)", release_failure_end
        )
        feed = slot_end.index(progress, functional_gate)

        self.assertEqual(
            slot_end.count(progress),
            1,
            "one batch slot may renew the radio lease at most once",
        )
        self.assertLess(snapshot, release)
        self.assertLess(release, radio_stop)
        self.assertLess(radio_stop, release_failure)
        self.assertLess(release_failure_end, clear)
        self.assertIn(
            "return release_ret;",
            slot_end[release_failure:release_failure_end],
            "a failed slot release must return without renewing progress",
        )
        self.assertLess(release_failure_end, functional_gate)
        self.assertLess(functional_gate, feed)

        batch = function_body(
            MESH_DELIVERY, "mesh_try_send_report_tx_ch9_batch"
        )
        batch_send = batch.index(
            "ret = mesh_send_outbound_preconfigured_ch9_locked("
        )
        batch_failure = batch.index("if (ret < 0)", batch_send)
        batch_failure_end = braced_statement_end(batch, batch_failure)
        batch_mark = batch.index(
            "slot_tx.functional_tx_completed = true;", batch_failure_end
        )
        batch_end = batch.index(
            "release_ret = mesh_ch9_slot_tx_end(&slot_tx);", batch_mark
        )
        self.assertLess(batch_failure_end, batch_mark)
        self.assertLess(batch_mark, batch_end)
        self.assertNotIn(
            progress,
            batch,
            "batch callers must aggregate progress for slot release instead "
            "of feeding once per frame",
        )

        direct_batch = function_body(
            MESH_DIRECT_GATEWAY,
            "mesh_try_send_report_tx_ch9_direct_gateway_batch",
        )
        direct_send = direct_batch.index(
            "ret = mesh_send_outbound_preconfigured_ch9_locked("
        )
        direct_failure = direct_batch.index("if (ret < 0)", direct_send)
        direct_failure_end = braced_statement_end(
            direct_batch, direct_failure
        )
        direct_mark = direct_batch.index(
            "slot_tx.functional_tx_completed = true;", direct_failure_end
        )
        ack_gate = direct_batch.index(
            "if (!ack_debug.functional_rx_outcome)", direct_mark
        )
        ack_clear = direct_batch.index(
            "slot_tx.functional_tx_completed = false;", ack_gate
        )
        direct_end = direct_batch.index(
            "release_ret = mesh_ch9_slot_tx_end(&slot_tx);", ack_clear
        )
        self.assertLess(direct_failure_end, direct_mark)
        self.assertLess(direct_mark, ack_gate)
        self.assertLess(ack_gate, ack_clear)
        self.assertLess(ack_clear, direct_end)
        self.assertNotIn(
            progress,
            direct_batch,
            "direct batches must leave their one possible feed to slot release",
        )

    def test_direct_gateway_ack_hard_errors_cannot_feed_progress(self) -> None:
        progress = "app_watchdog_note_radio_progress();"
        wait = function_body(
            MESH_DIRECT_GATEWAY,
            "mesh_wait_for_direct_gateway_ack_configured",
        )
        receive = wait.index(
            "rx_ret = dwm3000_driver_receive_frame_detailed_quiet("
        )
        timeout = wait.index("if (rx_ret == -ETIMEDOUT)", receive)
        timeout_end = braced_statement_end(wait, timeout)
        timeout_branch = wait[timeout:timeout_end]
        hard_error = wait.index("if (rx_ret < 0)", timeout_end)
        hard_error_end = braced_statement_end(wait, hard_error)
        hard_error_branch = wait[hard_error:hard_error_end]
        frame = wait.index(
            "debug->saw_frame = true;", hard_error_end
        )

        self.assertIn(
            "debug->functional_rx_outcome = true;",
            timeout_branch,
            "a real bounded quiet timeout must count as functional RX progress",
        )
        self.assertRegex(
            hard_error_branch,
            r"debug->functional_rx_outcome\s*=\s*"
            r"rx_failure\s*!=\s*DWM3000_RX_FAILURE_NONE\s*;",
            "an immediate untyped SPI/device failure must remain ineligible",
        )
        self.assertIn("result_ret = rx_ret;", hard_error_branch)
        self.assertIn("break;", hard_error_branch)
        self.assertIn(
            "debug->functional_rx_outcome = true;",
            wait[frame:],
            "a completed frame receive must count as functional RX progress",
        )
        self.assertTrue(
            wait.rstrip().endswith("return result_ret;\n}"),
            "the ACK wait must preserve its classified result instead of "
            "rewriting a hard error as timeout",
        )

        single = function_body(
            MESH_DIRECT_GATEWAY,
            "mesh_send_direct_gateway_payload_and_wait_ack",
        )
        single_release_failure = single.index("if (release_ret < 0)")
        single_release_failure_end = braced_statement_end(
            single, single_release_failure
        )
        single_feed = single.index(progress, single_release_failure_end)
        self.assertEqual(single.count(progress), 1)
        self.assertRegex(
            single[single_release_failure_end:],
            r"if\s*\(\s*functional_tx_completed\s*&&\s*"
            r"\(\s*!gateway_ack_required\s*\|\|\s*"
            r"ack_debug\.functional_rx_outcome\s*\)\s*\)\s*\{\s*"
            r"app_watchdog_note_radio_progress\s*\(\s*\)\s*;\s*\}",
            "ACK-required direct sends may feed only after functional ACK RX "
            "and successful release",
        )
        self.assertGreater(single_feed, single_release_failure_end)

        probe = function_body(
            MESH_DIRECT_GATEWAY,
            "mesh_send_direct_gateway_probe_and_wait",
        )
        probe_release_failure = probe.index("if (release_ret < 0)")
        probe_release_failure_end = braced_statement_end(
            probe, probe_release_failure
        )
        probe_feed = probe.index(progress, probe_release_failure_end)
        self.assertEqual(probe.count(progress), 1)
        self.assertRegex(
            probe[probe_release_failure_end:],
            r"if\s*\(\s*functional_tx_completed\s*&&\s*"
            r"\(\s*!ack_wait_ran\s*\|\|\s*"
            r"ack_debug\.functional_rx_outcome\s*\)\s*\)\s*\{\s*"
            r"app_watchdog_note_radio_progress\s*\(\s*\)\s*;\s*\}",
            "direct probes may feed only after functional ACK RX and "
            "successful release",
        )
        self.assertGreater(probe_feed, probe_release_failure_end)

    def test_max_survey_discovery_fits_relaxed_progress_lease(self) -> None:
        max_slots = uint_macro(
            SURVEY_HEADER, "SURVEY_DISCOVERY_MAX_SLOT_COUNT"
        )
        max_slot_ms = uint_macro(
            SURVEY_HEADER, "SURVEY_DISCOVERY_MAX_SLOT_MS"
        )
        max_rounds = uint_macro(
            SURVEY_HEADER, "SURVEY_DISCOVERY_MAX_ROUND_COUNT"
        )
        lease_ms = uint_macro(
            WATCHDOG_HEADER, "APP_WATCHDOG_PROGRESS_LEASE_MS"
        )

        self.assertLess(
            max_slots * max_slot_ms * max_rounds,
            lease_ms,
            "a valid maximum discovery must not be reset by the watchdog",
        )
        self.assertLess(
            max_slots * max_slot_ms,
            lease_ms,
            "one bounded opportunity must fit within the radio-progress lease",
        )

    def test_survey_discovery_progress_requires_operation_and_release(
        self,
    ) -> None:
        progress = "app_watchdog_note_radio_progress();"
        runtime = function_body(SURVEY_RUNTIME, "survey_work_handler")
        discovery_start = runtime.index("if (run_discovery) {")
        discovery_end = braced_statement_end(runtime, discovery_start)
        discovery = runtime[discovery_start:discovery_end]
        acquire = discovery.index(
            "ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,"
        )
        failure = discovery.index("if (ret < 0)", acquire)
        failure_end = braced_statement_end(discovery, failure)
        operation = discovery.index(
            "ret = app_anchor_survey_discovery_run(", failure_end
        )
        low_power = discovery.index(
            "low_power_ret = runtime_ops.enter_low_power(", operation
        )
        release = discovery.index(
            "release_ret = survey_radio_release(&radio_lease, low_power_ret);",
            low_power,
        )
        release_failure = discovery.index("if (release_ret < 0)", release)
        feed = discovery.index(progress, release)

        self.assertEqual(
            discovery.count(progress),
            1,
            "one discovery run must expose only one release-gated owner feed",
        )
        self.assertNotIn(
            progress,
            discovery[:failure_end],
            "a deferred discovery contender refreshed the active owner's lease",
        )
        self.assertNotIn(
            progress,
            discovery[failure_end:operation],
            "guard acquisition renewed progress before discovery did radio work",
        )
        self.assertLess(operation, low_power)
        self.assertLess(low_power, release)
        self.assertLess(release_failure, feed)
        self.assertLess(release, feed)
        self.assertRegex(
            discovery[release:],
            r"if\s*\(\s*functional_radio_outcome\s*&&\s*"
            r"ret\s*>=\s*0\s*&&\s*low_power_ret\s*>=\s*0\s*\)\s*\{\s*"
            r"app_watchdog_note_radio_progress\s*\(\s*\)\s*;\s*\}",
            "discovery may renew its owner only after a functional operation "
            "and successful idle/standby release",
        )

    def test_each_bounded_survey_receive_refreshes_discovery_progress(self) -> None:
        progress = "app_watchdog_note_radio_progress();"
        receive = function_body(
            SURVEY_DISCOVERY, "receive_survey_probes_until"
        )
        bounded_receive = receive.index(
            "ret = dwm3000_driver_receive_frame_continuous("
        )
        receive_result = receive.index(
            "if (ret == -ETIMEDOUT)", bounded_receive
        )
        self.assertIn(
            progress,
            receive[bounded_receive:receive_result],
            "a completed bounded discovery receive did not renew the owner's lease",
        )

    def test_survey_receive_chunks_stay_inside_the_radio_progress_lease(self) -> None:
        slice_definition = re.search(
            r"(?m)^#define\s+SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS\s+\\\n"
            r"\s*\(APP_WATCHDOG_PROGRESS_LEASE_MS\s*/\s*([0-9]+)u\)\s*$",
            SURVEY_DISCOVERY,
        )
        lease_ms = uint_macro(
            WATCHDOG_HEADER, "APP_WATCHDOG_PROGRESS_LEASE_MS"
        )
        self.assertIsNotNone(
            slice_definition,
            "survey discovery must derive a bounded receive slice from the watchdog lease",
        )
        divisor = int(slice_definition.group(1))
        chunk_ms = lease_ms // divisor
        receive = function_body(
            SURVEY_DISCOVERY, "receive_survey_probes_until"
        )
        remaining = receive.index("uint32_t remaining_ms =")
        empty = receive.index("if (remaining_ms == 0u)", remaining)
        cap = re.search(
            r"receive_ms\s*=\s*MIN\s*\(\s*remaining_ms\s*,\s*"
            r"SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS\s*\)\s*;",
            receive[empty:],
        )
        self.assertIsNotNone(
            cap,
            "each continuous receive must cap its timeout before entering the driver",
        )
        call = receive.index(
            "ret = dwm3000_driver_receive_frame_continuous(", empty
        )
        self.assertLess(empty + cap.start(), call)
        self.assertRegex(
            receive[call:],
            r"dwm3000_driver_receive_frame_continuous\s*\(\s*receive_ms\s*,",
            "the capped timeout must be the value passed to continuous receive",
        )
        self.assertGreater(divisor, 1)
        self.assertGreater(chunk_ms, 0)
        self.assertLess(
            chunk_ms,
            lease_ms,
            "a quiet continuous receive must return before its radio lease expires",
        )

    def test_survey_receive_chunk_timeout_preserves_the_overall_deadline(self) -> None:
        receive = function_body(
            SURVEY_DISCOVERY, "receive_survey_probes_until"
        )
        timeout = receive.index("if (ret == -ETIMEDOUT)")
        timeout_end = braced_statement_end(receive, timeout)
        timeout_branch = receive[timeout:timeout_end]

        self.assertRegex(
            timeout_branch,
            r"if\s*\(\s*receive_ms\s*<\s*remaining_ms\s*\)\s*\{\s*"
            r"continue\s*;\s*\}",
            "a chunk timeout must re-enter the loop until the overall deadline",
        )


if __name__ == "__main__":
    unittest.main()
