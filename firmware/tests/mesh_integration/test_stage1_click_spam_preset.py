#!/usr/bin/env python3
"""Source/config guards for the non-production Stage 1 click-session source."""

from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
APP_CMAKE = REPO_ROOT / "firmware" / "app" / "CMakeLists.txt"
APP_KCONFIG = REPO_ROOT / "firmware" / "app" / "Kconfig"
APP_CONF = REPO_ROOT / "firmware" / "app" / "conf" / "tag-click-spam.conf"
MAIN_SOURCE = REPO_ROOT / "firmware" / "app" / "src" / "main.c"
CLICKER_SOURCE = REPO_ROOT / "firmware" / "app" / "src" / "app_clicker.c"
CLICKER_HEADER = REPO_ROOT / "firmware" / "app" / "src" / "app_clicker.h"


class Stage1ClickSpamPresetTests(unittest.TestCase):
    def test_preset_is_bench_only_and_role_bound(self) -> None:
        source = APP_CMAKE.read_text(encoding="utf-8")

        self.assertGreaterEqual(source.count("tag_stage1_click_spam"), 2)
        branch = source.split(
            'elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage1_click_spam")', 1
        )[1].split("elseif", 1)[0]
        self.assertIn('set(FIRMWARE_ROLE "clicker"', branch)
        self.assertIn("set(IMEC_BENCH_STAGE 1)", branch)
        self.assertIn('set(IMEC_PRESET_ROLE_CONF "role-tag.conf")', branch)
        self.assertIn('set(IMEC_PRESET_EXTRA_CONF "tag-click-spam.conf")', branch)
        self.assertNotIn("IMEC_DEPLOYABLE_MESH_PRESET ON", branch)
        self.assertNotIn('IMEC_BUILD_PRESET STREQUAL "mesh_clicker"', branch)

    def test_kconfig_and_fragment_select_autonomous_diagnostic_sessions(self) -> None:
        kconfig = APP_KCONFIG.read_text(encoding="utf-8")
        conf = APP_CONF.read_text(encoding="utf-8")

        self.assertIn("config IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS", kconfig)
        self.assertIn(
            "depends on IMEC_HIGH_DEBUG && IMEC_BENCH_STAGE = 1 && IMEC_ROLE_TAG",
            kconfig,
        )
        self.assertIn("depends on IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE", kconfig)
        self.assertIn("CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS=y", conf)
        self.assertIn("CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE=n", conf)
        self.assertIn("CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE=n", conf)

    def test_main_bypasses_button_and_runs_the_session_source(self) -> None:
        main = MAIN_SOURCE.read_text(encoding="utf-8")
        header = CLICKER_HEADER.read_text(encoding="utf-8")
        clicker = CLICKER_SOURCE.read_text(encoding="utf-8")
        runner = clicker.split(
            "void app_clicker_run_continuous_click_sessions", 1
        )[1].split("int app_clicker_discover_uwb_anchors", 1)[0]

        self.assertIn("app_clicker_start_continuous_click_sessions", main)
        self.assertIn("physical_button=disabled", main)
        self.assertIn("app_clicker_run_continuous_click_sessions", header)
        self.assertIn("app_clicker_start_continuous_click_sessions", header)
        self.assertIn("app_clicker_submit_work(&stage1_click_spam_work)", clicker)
        self.assertIn("CLICKER_ACTION_WORKQUEUE_STACK_SIZE == 8192u", clicker)
        self.assertNotIn(
            "app_clicker_run_continuous_click_sessions(&clicker_session_spam_config)",
            main,
        )
        self.assertIn("app_clicker_run_normal_click();", runner)
        self.assertNotIn("app_clicker_run_uwb_diagnostic_click", runner)
        self.assertGreaterEqual(runner.count('high_debug_log_event("STAGE1_CLICK_SPAM"'), 2)
        self.assertIn("MIN(requested_delay_ms, STAGE1_CLICK_SPAM_MAX_DELAY_MS)", runner)


if __name__ == "__main__":
    unittest.main()
