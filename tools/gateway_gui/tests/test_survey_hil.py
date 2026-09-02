from types import SimpleNamespace

from tools.gateway_gui.protocol import CMD_SURVEY_PLAN, CMD_SURVEY_START
from tools.gateway_gui.survey_hil import (
    SurveyHilEvidence,
    _disconnect_injection_due,
)


def test_disconnect_injection_waits_for_full_requested_delay() -> None:
    assert not _disconnect_injection_due(
        delay_s=None, accepted_at=100.0, now=999.0
    )
    assert not _disconnect_injection_due(
        delay_s=1.0, accepted_at=None, now=999.0
    )
    assert not _disconnect_injection_due(
        delay_s=1.0, accepted_at=100.0, now=100.999
    )
    assert _disconnect_injection_due(
        delay_s=1.0, accepted_at=100.0, now=101.0
    )


def test_one_pair_batches_require_every_plan_and_intermediate_completion() -> None:
    evidence = SurveyHilEvidence(
        command_status={CMD_SURVEY_START: 0, CMD_SURVEY_PLAN: 0},
        plan_command_successes=3,
        neighbor_reports=3,
        planned_pairs_by_batch={0: 1, 1: 1, 2: 1},
        completed_batches={0, 1},
        terminal_results=1,
        terminal_usable=1,
        terminal_seen=True,
        signal_measurements=3,
    )
    results = {
        index: SimpleNamespace(usable=True, success_count=5)
        for index in range(3)
    }

    assert evidence.qualifies(
        expected_anchors=3,
        expected_pairs=3,
        expected_samples=5,
        batch_pairs=1,
        gui_pairs=((0, 1), (0, 2), (1, 2)),
        gui_results=results,
        gui_error="",
    )

    evidence.completed_batches.remove(1)
    assert not evidence.qualifies(
        expected_anchors=3,
        expected_pairs=3,
        expected_samples=5,
        batch_pairs=1,
        gui_pairs=((0, 1), (0, 2), (1, 2)),
        gui_results=results,
        gui_error="",
    )
