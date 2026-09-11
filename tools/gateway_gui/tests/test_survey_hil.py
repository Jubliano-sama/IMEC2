from dataclasses import replace
from types import SimpleNamespace
from unittest.mock import Mock, patch

import pytest

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.protocol import (
    CMD_READ_ANCHOR_BATTERY,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    FLAG_GATEWAY_ACK_REQUIRED,
    MSG_COMMAND_RESULT,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_REASON,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
)
from tools.gateway_gui.survey_hil import (
    SurveyHilEvidence,
    SurveyHilGui,
    _disconnect_injection_due,
    _survey_event_is_current,
)
from tools.gateway_gui.survey_runtime import SurveyCommandOwner


def result_packet(command_id: int, session_id: int, sequence: int, status: int):
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, command_id.to_bytes(2, "little"))
    append_tlv(payload, TLV_COMMAND_STATUS, status.to_bytes(2, "little"))
    append_tlv(payload, TLV_REASON, b"\x00")
    return parse_cobs_packet(
        encode_cobs_packet(
            msg_type=MSG_COMMAND_RESULT,
            flags=0,
            src_id=0x11,
            dst_id=0x22,
            session_id=session_id,
            seq=sequence,
            ttl=1,
            payload=bytes(payload),
        )
    )


def accept_start(evidence: SurveyHilEvidence, *, generation: int = 9) -> None:
    evidence.note_dispatch(CMD_SURVEY_START, session_id=100, sequence=3)
    assert evidence.observe_command_result(
        CMD_SURVEY_START, session_id=100, sequence=3, status=0
    )
    assert evidence.note_started(generation, session_id=100, sequence=3)


def test_raw_result_does_not_bypass_production_acceptance() -> None:
    gui = SurveyHilGui.__new__(SurveyHilGui)
    gui.hil_evidence = SurveyHilEvidence()
    gui.gateway_id = 0x11
    gui.host_id_text = Mock(get=Mock(return_value="0x22"))
    stale = result_packet(CMD_SURVEY_START, 99, 3, 0)

    with patch.object(GatewayGui, "_add_packet") as production:
        gui._add_packet(stale)

    production.assert_called_once_with(stale, received_at=None)
    assert gui.hil_evidence.command_status == {}


@pytest.mark.parametrize(
    "changes,accepted",
    [
        ({}, True),
        ({"src_id": 0x33, "dst_id": 0x33}, False),
        ({"dst_id": 0x22}, False),
        ({"transport": "shared-cobs"}, False),
        ({"flags": 0}, False),
        ({"stream_flags": 1}, False),
        ({"session_id": 99}, False),
        ({"seq": 2}, False),
    ],
)
def test_generic_command_evidence_requires_canonical_gateway_and_current_identity(
    changes: dict[str, object], accepted: bool,
) -> None:
    gui = SurveyHilGui.__new__(SurveyHilGui)
    gui.hil_evidence = SurveyHilEvidence()
    gui.gateway_id = 0x11
    gui.hil_evidence.note_dispatch(
        CMD_READ_ANCHOR_BATTERY, session_id=100, sequence=3
    )
    packet = replace(
        result_packet(CMD_READ_ANCHOR_BATTERY, 100, 3, 0),
        transport="gateway-stream-v1",
        flags=FLAG_GATEWAY_ACK_REQUIRED,
        dst_id=0x11,
    )
    packet = replace(packet, **changes)
    with patch.object(GatewayGui, "_add_packet") as production:
        gui._add_packet(packet)
    production.assert_called_once_with(packet, received_at=None)
    assert gui.hil_evidence.command_status == (
        {CMD_READ_ANCHOR_BATTERY: 0} if accepted else {}
    )


@pytest.mark.parametrize("session_id,sequence", [(99, 3), (100, 2), (99, 2)])
def test_stale_start_result_never_binds_to_current_dispatch(
    session_id: int, sequence: int
) -> None:
    evidence = SurveyHilEvidence()
    assert not evidence.observe_command_result(
        CMD_SURVEY_START, session_id=session_id, sequence=sequence, status=0
    )
    evidence.note_dispatch(CMD_SURVEY_START, session_id=100, sequence=3)
    assert not evidence.observe_command_result(
        CMD_SURVEY_START, session_id=session_id, sequence=sequence, status=0
    )
    assert evidence.command_status == {}
    assert not evidence.start_accepted_for(9)


@pytest.mark.parametrize("event_first", [False, True])
def test_current_start_acceptance_requires_both_sources_in_either_order(
    event_first: bool,
) -> None:
    evidence = SurveyHilEvidence()
    evidence.note_dispatch(CMD_SURVEY_START, session_id=100, sequence=3)
    result = lambda: evidence.observe_command_result(
        CMD_SURVEY_START, session_id=100, sequence=3, status=0
    )
    started = lambda: evidence.note_started(9, session_id=100, sequence=3)
    first, second = (started, result) if event_first else (result, started)

    assert first()
    assert not evidence.start_accepted_for(9)
    assert second()
    assert evidence.start_accepted_for(9)
    assert not evidence.start_accepted_for(None)
    assert not evidence.start_accepted_for(0)
    assert not evidence.start_accepted_for(10)


@pytest.mark.parametrize(
    "generation,session_id,sequence", [(0, 100, 3), (9, 99, 3), (9, 100, 2)]
)
def test_started_rejects_invalid_generation_or_wrong_dispatch_identity(
    generation: int, session_id: int, sequence: int
) -> None:
    evidence = SurveyHilEvidence()
    assert not evidence.note_started(9, session_id=100, sequence=3)
    evidence.note_dispatch(CMD_SURVEY_START, session_id=100, sequence=3)
    assert evidence.observe_command_result(
        CMD_SURVEY_START, session_id=100, sequence=3, status=0
    )
    assert not evidence.note_started(
        generation, session_id=session_id, sequence=sequence
    )
    assert not evidence.start_accepted_for(9)


def test_new_start_dispatch_invalidates_previous_acceptance() -> None:
    evidence = SurveyHilEvidence()
    accept_start(evidence)
    assert evidence.start_accepted_for(9)
    evidence.note_dispatch(CMD_SURVEY_START, session_id=101, sequence=4)

    assert not evidence.start_accepted_for(9)
    assert not evidence.start_accepted_for(10)
    assert not evidence.observe_command_result(
        CMD_SURVEY_START, session_id=100, sequence=3, status=0
    )
    assert not evidence.note_started(9, session_id=100, sequence=3)
    assert evidence.observe_command_result(
        CMD_SURVEY_START, session_id=101, sequence=4, status=0
    )
    assert evidence.note_started(10, session_id=101, sequence=4)
    assert evidence.start_accepted_for(10)


@pytest.mark.parametrize("first_status,second_status", [(0, 5), (5, 0)])
@pytest.mark.parametrize("command_id", [CMD_SURVEY_START, CMD_SURVEY_PLAN])
def test_first_correlated_result_cannot_be_replaced_by_conflicting_replay(
    command_id: int, first_status: int, second_status: int
) -> None:
    evidence = SurveyHilEvidence()
    evidence.note_dispatch(command_id, session_id=100, sequence=3)
    assert evidence.observe_command_result(
        command_id, session_id=100, sequence=3, status=first_status
    )
    assert not evidence.observe_command_result(
        command_id, session_id=100, sequence=3, status=second_status
    )
    assert evidence.command_status[command_id] == first_status
    assert evidence.plan_command_successes == int(
        command_id == CMD_SURVEY_PLAN and first_status == 0
    )


def test_plan_acceptance_counts_distinct_dispatches_once_each() -> None:
    evidence = SurveyHilEvidence()
    accept_start(evidence)
    for sequence in (4, 5, 6):
        evidence.note_dispatch(CMD_SURVEY_PLAN, session_id=100, sequence=sequence)
        assert evidence.observe_command_result(
            CMD_SURVEY_PLAN, session_id=100, sequence=sequence, status=0
        )
        for replay in range(4, sequence + 1):
            assert not evidence.observe_command_result(
                CMD_SURVEY_PLAN, session_id=100, sequence=replay, status=0
            )
        assert evidence.plan_command_successes == sequence - 3

    # Repeating the same dispatch must not reopen a completed identity.
    evidence.note_dispatch(CMD_SURVEY_PLAN, session_id=100, sequence=6)
    assert not evidence.observe_command_result(
        CMD_SURVEY_PLAN, session_id=100, sequence=6, status=0
    )
    assert evidence.plan_command_successes == 3


@pytest.mark.parametrize("recovered", [False, True])
def test_production_matched_transition_records_acceptance_once(
    recovered: bool,
) -> None:
    gui = SurveyHilGui.__new__(SurveyHilGui)
    gui.hil_evidence = SurveyHilEvidence()
    dispatch = SimpleNamespace(command_id=CMD_SURVEY_PLAN, session_id=100, sequence=4)
    with patch.object(GatewayGui, "_dispatch_gateway_command") as production_dispatch:
        gui._dispatch_gateway_command(dispatch)
    production_dispatch.assert_called_once_with(dispatch)

    owner = SurveyCommandOwner()
    assert owner.begin(CMD_SURVEY_PLAN, 100, 4, "PLAN", timeout_s=5, now=0)
    if recovered:
        owner.expire(now=6)
    transition = owner.observe_result(CMD_SURVEY_PLAN, 100, 4, 0)
    assert transition.matched
    assert transition.recovered == recovered
    with patch.object(GatewayGui, "_apply_survey_command_result") as production_apply:
        gui._apply_survey_command_result(transition, acceptance_applied=recovered)
        gui._apply_survey_command_result(transition, acceptance_applied=recovered)
    assert production_apply.call_count == 2
    assert gui.hil_evidence.plan_command_successes == 1
    assert gui.hil_evidence.command_status[CMD_SURVEY_PLAN] == 0


def test_unmatched_production_transition_cannot_enter_evidence() -> None:
    gui = SurveyHilGui.__new__(SurveyHilGui)
    gui.hil_evidence = SurveyHilEvidence()
    gui.hil_evidence.note_dispatch(CMD_SURVEY_START, session_id=100, sequence=3)
    owner = SurveyCommandOwner()
    assert owner.begin(CMD_SURVEY_START, 100, 3, "START", timeout_s=5, now=0)
    transition = owner.observe_result(CMD_SURVEY_START, 99, 3, 0)
    assert not transition.matched
    with patch.object(GatewayGui, "_apply_survey_command_result"):
        gui._apply_survey_command_result(transition)
    assert gui.hil_evidence.command_status == {}


def disconnect_gui() -> SurveyHilGui:
    gui = SurveyHilGui.__new__(SurveyHilGui)
    gui.hil_evidence = SurveyHilEvidence()
    accept_start(gui.hil_evidence)
    gui._survey_generation = 9
    gui.survey_model = SimpleNamespace(
        active=True,
        start_accepted=True,
        generation=9,
        start_command_identity=(100, 3),
        plan_command_identity=None,
    )
    return gui


@pytest.mark.parametrize(
    "field,value",
    [
        ("active", False),
        ("start_accepted", False),
        ("generation", None),
        ("generation", 10),
        ("start_command_identity", (99, 3)),
        ("start_command_identity", (100, 2)),
        ("plan_command_identity", (100, 4)),
    ],
)
def test_disconnect_requires_the_current_active_model_and_start_identity(
    field: str, value: object,
) -> None:
    gui = disconnect_gui()
    assert gui._disconnect_start_is_current()
    setattr(gui.survey_model, field, value)
    assert not gui._disconnect_start_is_current()


def test_disconnect_rejects_null_gui_generation_and_already_dispatched_plan() -> None:
    gui = disconnect_gui()
    gui._survey_generation = None
    assert not gui._disconnect_start_is_current()
    gui._survey_generation = 9
    assert gui._disconnect_start_is_current()
    gui.hil_evidence.note_dispatch(CMD_SURVEY_PLAN, session_id=100, sequence=4)
    assert not gui._disconnect_start_is_current()


def test_disconnect_suppresses_plan_but_normal_survey_still_submits() -> None:
    gui = disconnect_gui()
    event = object()
    with patch.object(GatewayGui, "_submit_survey_plan") as production:
        gui.hil_disconnect_after_start = True
        gui._submit_survey_plan(event)
        production.assert_not_called()
        gui.hil_disconnect_after_start = False
        gui._submit_survey_plan(event)
        production.assert_called_once_with(event)


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


def test_hil_evidence_accepts_only_the_production_gui_generation() -> None:
    assert not _survey_event_is_current(
        event_generation=1, current_generation=None
    )
    assert not _survey_event_is_current(
        event_generation=1, current_generation=2
    )
    assert _survey_event_is_current(
        event_generation=2, current_generation=2
    )


def test_one_pair_batches_require_every_plan_and_intermediate_completion() -> None:
    evidence = SurveyHilEvidence(
        neighbor_reports=3,
        planned_pairs_by_batch={0: 1, 1: 1, 2: 1},
        completed_batches={0, 1},
        terminal_results=1,
        terminal_usable=1,
        terminal_seen=True,
        signal_measurements=3,
    )
    accept_start(evidence)
    for sequence in (4, 5, 6):
        evidence.note_dispatch(CMD_SURVEY_PLAN, session_id=100, sequence=sequence)
        assert evidence.observe_command_result(
            CMD_SURVEY_PLAN, session_id=100, sequence=sequence, status=0
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
