#!/usr/bin/env python3
"""Keep survey lifecycle ownership in the real round/transaction service."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app"
ROUND_GLUE = APP / "src/app_anchor_gateway_survey_round.inc"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index]
    raise AssertionError(f"unterminated function {name}")


class GatewaySurveyEventRuntimeRemovalTests(unittest.TestCase):
    def test_production_has_no_survey_mirror_runtime(self) -> None:
        production_files = [
            *APP.rglob("*.c"),
            *APP.rglob("*.h"),
            *APP.rglob("*.inc"),
            APP / "CMakeLists.txt",
            ROOT / "CMakeLists.txt",
        ]

        for source_file in production_files:
            source = source_file.read_text(encoding="utf-8")
            self.assertNotIn("app_gateway_survey_event_runtime", source)
            self.assertNotIn("gateway_survey_state_event", source)

        self.assertFalse(
            (APP / "src/app_gateway_survey_event_runtime.c").exists()
        )
        self.assertFalse(
            (APP / "src/app_gateway_survey_event_runtime.h").exists()
        )

    def test_production_has_no_automatic_survey_mirror_api(self) -> None:
        for source_file in (APP / "src").iterdir():
            if source_file.suffix not in {".c", ".h", ".inc"}:
                continue
            source = source_file.read_text(encoding="utf-8")
            for retired in (
                "gateway_survey_auto",
                "survey_gateway_auto_next_action",
                "survey_gateway_auto_",
            ):
                self.assertNotIn(
                    retired,
                    source,
                    f"{source_file.name} retains retired survey mirror {retired}",
                )

    def test_round_owner_starts_before_any_progress_is_published(self) -> None:
        start = function_body(
            ROUND_GLUE.read_text(encoding="utf-8"),
            "gateway_survey_round_start",
        )
        empty = start.index("if (gateway_survey_context.pair_count == 0u)")
        begin = start.index("app_gateway_survey_round_begin(")
        begin_failure = start.index("if (ret != PROTO_OK)", begin)
        failure_return = start.index("return ret;", begin_failure)
        publication = start.index("GATEWAY_SURVEY_VERBOSE_LOG", failure_return)

        self.assertLess(empty, begin)
        self.assertLess(begin, begin_failure)
        self.assertLess(begin_failure, failure_return)
        self.assertLess(failure_return, publication)
        self.assertEqual(start.count("app_gateway_survey_round_begin("), 1)
        self.assertNotIn("gateway_survey_round_sync_auto", start)
        self.assertNotIn("FW_EVENT_PAIR_AVAILABLE", start)
        self.assertNotIn("FW_EVENT_NO_PAIR_AVAILABLE", start)


if __name__ == "__main__":
    unittest.main()
