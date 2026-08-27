import unittest
from types import SimpleNamespace

from tools.gateway_gui.protocol import CMD_SURVEY_PLAN, CMD_SURVEY_START
from tools.gateway_gui.survey_hil import (
    SurveyHilEvidence,
    _configure_expected_anchors,
)


class SurveyHilEvidenceTests(unittest.TestCase):
    def test_expected_anchor_count_configures_production_gui_input(self) -> None:
        field = SimpleNamespace(value="3")
        field.set = lambda value: setattr(field, "value", value)
        gui = SimpleNamespace(assignment_expected_anchors_text=field)

        _configure_expected_anchors(gui, 4)  # type: ignore[arg-type]

        self.assertEqual(field.value, "4")

    def _complete_evidence(self) -> SurveyHilEvidence:
        return SurveyHilEvidence(
            command_status={CMD_SURVEY_START: 0, CMD_SURVEY_PLAN: 0},
            neighbor_reports=3,
            planned_pairs=3,
            terminal_results=3,
            terminal_usable=3,
            terminal_seen=True,
        )

    def test_exact_complete_survey_qualifies(self) -> None:
        results = {
            index: SimpleNamespace(usable=True, success_count=5)
            for index in range(3)
        }
        self.assertTrue(
            self._complete_evidence().qualifies(
                expected_anchors=3,
                expected_pairs=3,
                expected_samples=5,
                gui_pairs=((0, 1), (0, 2), (1, 2)),
                gui_results=results,
                gui_error="",
            )
        )

    def test_partial_or_short_survey_fails(self) -> None:
        results = {
            index: SimpleNamespace(usable=True, success_count=5)
            for index in range(3)
        }
        partial = self._complete_evidence()
        partial.partial_reasons = 1
        self.assertFalse(
            partial.qualifies(
                expected_anchors=3,
                expected_pairs=3,
                expected_samples=5,
                gui_pairs=((0, 1), (0, 2), (1, 2)),
                gui_results=results,
                gui_error="",
            )
        )
        short = self._complete_evidence()
        results[2] = SimpleNamespace(usable=True, success_count=4)
        self.assertFalse(
            short.qualifies(
                expected_anchors=3,
                expected_pairs=3,
                expected_samples=5,
                gui_pairs=((0, 1), (0, 2), (1, 2)),
                gui_results=results,
                gui_error="",
            )
        )


if __name__ == "__main__":
    unittest.main()
