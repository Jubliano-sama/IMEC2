#!/usr/bin/env python3
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "app" / "src" / "app_watchdog.c").read_text()


class WatchdogAdoptionSourceTests(unittest.TestCase):
    def require(self, needle: str) -> None:
        if needle not in SOURCE:
            self.fail(f"app_watchdog.c is missing required invariant: {needle}")

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
        stale_return = handler.find("if (system_stale || radio_stale)")
        inherited_feed = handler.find("feed_inherited_watchdog(true)")
        zephyr_feed = handler.find("wdt_feed(watchdog_device")
        self.assertGreater(stale_return, 0)
        self.assertGreater(inherited_feed, stale_return)
        self.assertGreater(zephyr_feed, stale_return)
        self.require("atomic_set(&feeding_stopped, 1)")

    def test_boot_evidence_distinguishes_fresh_inherited_and_invalid(self) -> None:
        for mode in ("fresh", "inherited", "invalid"):
            self.require(f"DBG_WATCHDOG_BOOT mode={mode}")
        self.require("immediate=1")
        self.require("rr=0x%02x")

    def test_reset_cause_is_captured_before_hardware_latches_are_cleared(self) -> None:
        capture = SOURCE.find("hwinfo_get_reset_cause(&watchdog_health.reset_cause)")
        clear = SOURCE.find("hwinfo_clear_reset_cause()")
        marker = SOURCE.find("DBG_WATCHDOG_BOOT mode=invalid")
        self.assertGreaterEqual(capture, 0)
        self.assertGreater(clear, capture)
        self.assertGreater(marker, clear)


if __name__ == "__main__":
    unittest.main()
