from __future__ import annotations

from dataclasses import replace
import unittest
import queue
from typing import cast
from unittest.mock import Mock, patch

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.anchor_geometry_connectivity import (
    CONNECTIVITY_INTERVAL_ALGORITHM,
)
from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.command_orchestration import GatewayCommandDispatch
from tools.gateway_gui.delivery_dedup import GatewayPacketDeduplicator
from tools.gateway_gui.protocol import (
    CMD_SURVEY_GET_STATUS,
    CMD_SURVEY_CANCEL,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    FLAG_GATEWAY_ACK_REQUIRED,
    MSG_COMMAND_RESULT,
    MSG_SURVEY_EVENT,
    SURVEY_EVENT_HEADER_WIRE_LEN,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    Packet,
    SurveyAssignmentIdentity,
    append_tlv,
    encode_cobs_packet,
    parse_cobs_packet,
)
from tools.gateway_gui.survey_runtime import SurveyCommandOwner, SurveyOperationModel
from tools.gateway_gui.survey_runtime import SURVEY_PASS_ADDITIONAL_MERGE


GATEWAY_ID = 0x1111222233334444
HOST_ID = 0xAAAABBBBCCCCDDDD


class FakeVariable:
    def __init__(self, value: str = "") -> None:
        self.value = value

    def get(self) -> str:
        return self.value

    def set(self, value: str) -> None:
        self.value = value


class FakeTree:
    def __init__(self) -> None:
        self.rows: list[str] = []

    def insert(self, *_args: object, iid: str, **_kwargs: object) -> None:
        self.rows.append(iid)

    def get_children(self) -> tuple[str, ...]:
        return tuple(self.rows)

    def see(self, _iid: str) -> None:
        pass

    def delete(self, iid: str) -> None:
        self.rows.remove(iid)


def dispatch(command_id: int, session_id: int, sequence: int) -> GatewayCommandDispatch:
    return GatewayCommandDispatch(
        command_kind=2,
        command_id=command_id,
        session_id=session_id,
        sequence=sequence,
        frame=b"survey-command",
        label=f"survey-{command_id}",
        timeout_s=10.0,
        status_text="Survey command sent",
    )


def result_packet(
    command_id: int,
    session_id: int,
    sequence: int,
    status: int,
):
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, command_id.to_bytes(2, "little"))
    append_tlv(payload, TLV_COMMAND_STATUS, status.to_bytes(2, "little"))
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


def neighbor_packet() -> Packet:
    assignment = SurveyAssignmentIdentity(71, 81, bytes((0x5A,)) * 32, 3, 3)
    payload = bytearray(SURVEY_EVENT_HEADER_WIRE_LEN)
    payload[0] = 2
    payload[1] = 1
    payload[2] = 1
    payload[3] = 3
    payload[4:8] = (9).to_bytes(4, "little")
    payload[14:56] = assignment.encode()
    payload[56:64] = (0b111).to_bytes(8, "little")
    payload[64:72] = (0b111).to_bytes(8, "little")
    for own_slot, heard_mask in ((0, 0b110), (1, 0b101), (2, 0b011)):
        payload.extend((own_slot, heard_mask, 0, 0, 0, 0, 0, 0))
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=b"survey-event",
        raw_packet=None,
        msg_type=MSG_SURVEY_EVENT,
        flags=FLAG_GATEWAY_ACK_REQUIRED,
        src_id=GATEWAY_ID,
        dst_id=GATEWAY_ID,
        session_id=9,
        seq=1,
        ttl=None,
        age_ms=0,
        age_kind="gateway_queue_age_ms",
        payload=bytes(payload),
        tlvs=(),
    )


def plan_packet() -> Packet:
    assignment = SurveyAssignmentIdentity(71, 81, bytes((0x5A,)) * 32, 3, 3)
    payload = bytearray(SURVEY_EVENT_HEADER_WIRE_LEN)
    payload[0] = 2
    payload[1] = 2
    payload[2] = 1
    payload[4:8] = (9).to_bytes(4, "little")
    payload[11] = 3
    payload[12] = 2
    payload[14:56] = assignment.encode()
    payload.extend((0, 1, 0, 0, 2, 0, 1, 2, 1))
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=b"survey-plan-event",
        raw_packet=None,
        msg_type=MSG_SURVEY_EVENT,
        flags=FLAG_GATEWAY_ACK_REQUIRED,
        src_id=GATEWAY_ID,
        dst_id=GATEWAY_ID,
        session_id=9,
        seq=2,
        ttl=None,
        age_ms=0,
        age_kind="gateway_queue_age_ms",
        payload=bytes(payload),
        tlvs=(),
    )


def gui_model() -> GatewayGui:
    gui = GatewayGui.__new__(GatewayGui)
    gui.survey_model = SurveyOperationModel()
    gui.survey_model.begin(expected_anchor_count=3)
    gui.survey_command_owner = SurveyCommandOwner()
    gui._survey_pending_dispatch = None
    gui._survey_deferred_dispatch = None
    gui._survey_event_buffer = []
    gui._survey_chain_pending = False
    gui._survey_phase = "neighbors"
    gui.status_text = FakeVariable()  # type: ignore[assignment]
    gui.survey_max_degree_text = FakeVariable("4")  # type: ignore[assignment]
    gui._dispatch_gateway_command = Mock()  # type: ignore[method-assign]
    gui._refresh_survey_view = Mock()  # type: ignore[method-assign]
    gui._update_command_state = Mock()  # type: ignore[method-assign]
    gui._show_error = Mock()  # type: ignore[method-assign]
    return gui


class SurveyAppIntegrationTests(unittest.TestCase):
    def recovery_gui(self) -> GatewayGui:
        gui = gui_model()
        gui.connected = True
        gui.gateway_id = GATEWAY_ID
        gui._survey_gateway_id = GATEWAY_ID
        gui.host_id_text = FakeVariable(hex(HOST_ID))
        gui._next_identity = Mock(side_effect=((50, 1), (51, 2), (52, 3)))
        gui._survey_event_due_at = 0.0
        gui.survey_model.note_command_dispatched(CMD_SURVEY_START, now=0.0)
        gui._clear_scheduled_phase_estimate = Mock()
        return gui

    def test_next_survey_setup_does_not_recover_the_previous_run(self) -> None:
        gui = self.recovery_gui()
        gui.survey_model.begin(expected_anchor_count=3)
        gui._survey_chain_pending = True
        gui._survey_phase = "enumerating"
        # Simulate the expired timer left by a completed previous survey.
        gui._survey_event_due_at = 0.0
        gui._reconcile_stalled_survey()
        self.assertIsNone(gui.survey_command_owner.pending)
        gui._dispatch_gateway_command.assert_not_called()

    def test_old_survey_event_during_next_enumeration_is_retired_without_buffering(self) -> None:
        gui = self.recovery_gui()
        gui.survey_model.begin(expected_anchor_count=3)
        gui._survey_chain_pending = True
        gui._append_log = Mock()
        self.assertTrue(gui._observe_survey_event_packet(neighbor_packet()))
        self.assertEqual(gui._survey_event_buffer, [])
        self.assertIsNone(gui.survey_model.generation)
        self.assertEqual(gui.survey_model.phase, "routes")

    def test_valid_stale_event_after_failure_releases_transport_custody(self) -> None:
        gui = self.recovery_gui()
        gui.survey_model.fail("routes", "injected timeout")
        gui._append_log = Mock()
        self.assertTrue(gui._observe_survey_event_packet(neighbor_packet()))
        self.assertEqual(gui.survey_model.phase, "failed")
        self.assertIsNone(gui.survey_model.generation)
        self.assertEqual(gui._survey_event_buffer, [])

    def test_only_uncertain_start_and_plan_accept_exact_late_results(self) -> None:
        for command in (CMD_SURVEY_START, CMD_SURVEY_PLAN, CMD_SURVEY_CANCEL, CMD_SURVEY_GET_STATUS):
            gui = self.recovery_gui()
            gui.survey_command_owner.begin(command, 50, 1, "control", now=0., timeout_s=1.)
            gui._observe_survey_command_result(result_packet(command, 50, 1, 0), received_at=2.)
            self.assertIsNone(gui.survey_command_owner.pending)
            self.assertEqual(gui.survey_model.start_accepted, command == CMD_SURVEY_START)
            self.assertEqual(gui.survey_model.plan_accepted, command == CMD_SURVEY_PLAN)
            if command in (CMD_SURVEY_START, CMD_SURVEY_PLAN):
                self.assertIsNone(gui.survey_command_owner.uncertain)
                self.assertTrue(gui.survey_model.active)

    def test_lost_events_get_three_owned_status_attempts_then_unknown_failure(self) -> None:
        gui = self.recovery_gui()
        gui.survey_model.generation = 9
        for index in range(3):
            with patch("tools.gateway_gui.app.time.monotonic", return_value=index * 61.0):
                gui._reconcile_stalled_survey()
                pending = gui.survey_command_owner.pending
                self.assertEqual(pending.command_id, CMD_SURVEY_GET_STATUS)
                gui._reconcile_stalled_survey()
                self.assertIs(gui.survey_command_owner.pending, pending)
                gui._observe_survey_command_result(result_packet(
                    CMD_SURVEY_GET_STATUS, pending.session_id, pending.sequence, 0,
                ))
            self.assertTrue(gui.survey_model.active)
        with patch("tools.gateway_gui.app.time.monotonic", return_value=183.0):
            gui._reconcile_stalled_survey()
        self.assertFalse(gui.survey_model.active)
        self.assertIsNone(gui.survey_model.terminal_status)
        self.assertIn("remote outcome is unknown", gui.survey_model.error)
        self.assertEqual(gui._dispatch_gateway_command.call_count, 3)

    def test_status_result_after_terminal_never_overwrites_terminal_outcome(self) -> None:
        gui = self.recovery_gui()
        gui._reconcile_stalled_survey()
        pending = gui.survey_command_owner.pending
        gui.survey_model.active = False
        gui.survey_model.phase = "terminal"
        gui.survey_model.terminal_status = 1
        gui._survey_phase = "idle"
        gui._observe_survey_command_result(result_packet(
            CMD_SURVEY_GET_STATUS, pending.session_id, pending.sequence, 8,
        ))
        self.assertEqual(gui.survey_model.phase, "terminal")
        self.assertEqual(gui.survey_model.terminal_status, 1)
        self.assertIsNone(gui.survey_model.error)
        gui._show_error.assert_not_called()

    def test_status_result_preserves_recovered_long_ranging_schedule(self) -> None:
        gui = self.recovery_gui()
        gui._reconcile_stalled_survey()
        pending = gui.survey_command_owner.pending
        gui._survey_event_due_at = 1000.0
        with patch("tools.gateway_gui.app.time.monotonic", return_value=10.0):
            gui._observe_survey_command_result(result_packet(
                CMD_SURVEY_GET_STATUS, pending.session_id, pending.sequence, 0,
            ))
        self.assertEqual(gui._survey_event_due_at, 1000.0)

    def test_reconnect_to_other_gateway_cannot_query_old_survey_identity(self) -> None:
        gui = self.recovery_gui()
        gui.gateway_id += 1
        gui._reconcile_stalled_survey()
        self.assertFalse(gui.survey_model.active)
        gui._dispatch_gateway_command.assert_not_called()

    def test_oversized_plan_is_cancelled_before_dispatch_and_queue_is_retained(self) -> None:
        gui = gui_model()
        pairs = tuple((first, second) for first in range(20) for second in range(first + 1, 20))[:100]
        gui._survey_pair_batches = (pairs,)
        gui._survey_batch_cursor = 0
        gui._cancel_survey = Mock()
        gui._submit_survey_dispatch = Mock()
        assignment = SurveyAssignmentIdentity(71, 81, bytes((0x5A,)) * 32, 20, 8)
        gui._survey_generation = gui.survey_model.generation = 9
        gui._survey_assignment = gui.survey_model.assignment = assignment
        gui._survey_phase = gui.survey_model.phase = "planning"
        gui._submit_next_survey_batch(9, assignment)
        self.assertEqual(gui._survey_pair_batches, (pairs,))
        gui._cancel_survey.assert_called_once()
        gui._submit_survey_dispatch.assert_not_called()
        self.assertIn("pair queue is retained", gui._show_error.call_args.args[0])

    def test_event_drain_yields_with_backlog_and_does_not_expire_queued_packets(self) -> None:
        gui = gui_model()
        gui.root = Mock()
        gui.events = queue.Queue()
        for index in range(65):
            gui.events.put({"kind": "packet", "received_at": 1.0, "index": index})
        gui._handle_event = Mock()
        gui._expire_gateway_command = Mock()
        gui._expire_survey_command = Mock()
        gui._reconcile_stalled_survey = Mock()
        gui._update_scheduled_phase_progress = Mock()
        gui._drain_events()
        self.assertEqual(gui._handle_event.call_count, 64)
        gui._expire_gateway_command.assert_not_called()
        gui._expire_survey_command.assert_not_called()
        gui.root.after.assert_called_once()
        gui._drain_events()
        self.assertEqual(gui._handle_event.call_count, 65)
        gui._expire_gateway_command.assert_called_once()
        gui._expire_survey_command.assert_called_once()

    def test_event_callback_failure_is_reported_and_next_drain_is_scheduled(self) -> None:
        gui = gui_model()
        gui.root = Mock()
        gui.events = queue.Queue()
        gui.events.put({"kind": "packet"})
        gui._handle_event = Mock(side_effect=RuntimeError("semantic apply failed"))
        gui._expire_gateway_command = Mock()
        gui._expire_survey_command = Mock()
        gui._reconcile_stalled_survey = Mock()
        gui._update_scheduled_phase_progress = Mock()
        gui._drain_events()
        self.assertIn("semantic apply failed", gui._show_error.call_args.args[0])
        gui.root.report_callback_exception.assert_called_once()
        gui.root.after.assert_called_once_with(50, gui._drain_events)

    def test_operator_disabled_edge_is_removed_from_range_and_neighbor_inputs(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        pairs = (
            AnchorPairDistance("A", "B", 3.0),
            AnchorPairDistance("B", "C", 4.0),
        )
        gui.survey_model = Mock(
            geometry_pairs=pairs,
            neighbor_pairs=frozenset((("A", "B"), ("B", "C"))),
        )  # type: ignore[assignment]
        gui.survey_geometry_view = Mock(
            effective_geometry_pairs=(pairs[1],),
            disabled_edge_keys=frozenset((("A", "B"),)),
            edge_edit_counts=(1, 0),
        )  # type: ignore[assignment]

        effective, neighbors, counts = gui._effective_geometry_inputs()

        self.assertEqual(effective, (pairs[1],))
        self.assertEqual(neighbors, frozenset((("B", "C"),)))
        self.assertEqual(counts, (1, 0))

    def test_all_neighbor_action_starts_fresh_and_continues_with_merge(self) -> None:
        gui = GatewayGui.__new__(GatewayGui)
        gui._survey_auto_all = False
        gui._run_survey = Mock()  # type: ignore[method-assign]

        gui._run_all_neighbor_surveys()

        self.assertTrue(gui._survey_auto_all)
        cast(Mock, gui._run_survey).assert_called_once_with(
            "fresh",
            continue_all_neighbors=True,
        )

        cast(Mock, gui._run_survey).reset_mock()
        gui._survey_phase = "continuing-all-neighbors"
        gui.survey_model = Mock(phase="terminal")
        gui._continue_all_neighbor_surveys()
        cast(Mock, gui._run_survey).assert_called_once_with(
            SURVEY_PASS_ADDITIONAL_MERGE,
            continue_all_neighbors=True,
        )

    def test_geometry_worker_receives_selected_radio_interval(self) -> None:
        gui = gui_model()
        future = Mock()
        gui._geometry_executor = Mock()
        gui._geometry_executor.submit.return_value = future
        gui._geometry_future = None

        gui._submit_geometry_solve(
            CONNECTIVITY_INTERVAL_ALGORITHM,
            "Auto (best of all)",
            5.5,
            22.5,
            gui.survey_model.geometry_revision,
            gui.survey_model.run_serial,
        )

        call = gui._geometry_executor.submit.call_args
        self.assertEqual(call.kwargs["nonneighbor_min_m"], 5.5)
        self.assertEqual(call.kwargs["neighbor_max_m"], 22.5)
        future.add_done_callback.assert_called_once()
        self.assertIn("Solving with", gui.status_text.get())

    def test_geometry_worker_captures_distance_weight_power(self) -> None:
        gui = gui_model()
        gui._geometry_executor = Mock()
        gui.survey_geometry_view = Mock(distance_weight_power=2.0, locked_positions_m={})
        gui._effective_geometry_inputs = Mock(return_value=((), frozenset(), (0, 0)))

        gui._submit_geometry_solve(
            CONNECTIVITY_INTERVAL_ALGORITHM, "Auto (best of all)", 7, 15,
            gui.survey_model.geometry_revision, gui.survey_model.run_serial,
        )
        self.assertEqual(
            gui._geometry_executor.submit.call_args.kwargs["distance_weight_power"], 2.0,
        )

    def test_automatic_solve_reads_radio_interval_after_new_measurement_refresh(self) -> None:
        gui = gui_model()
        gui.survey_model = Mock(geometry_solve_pending=True, geometry_revision=1, run_serial=1)
        gui._geometry_future = None
        gui.survey_geometry_view = Mock(
            neighbor_interval_m=(7.0, 15.0), nearest_anchor_count=0,
        )
        gui.survey_geometry_view.solver_var.get.return_value = CONNECTIVITY_INTERVAL_ALGORITHM
        gui.survey_geometry_view.seed_var.get.return_value = "Auto (best of all)"
        def refresh():
            gui.survey_geometry_view.neighbor_interval_m = (7.0, 21.0)
        gui._refresh_survey_view = Mock(side_effect=refresh)
        gui._submit_geometry_solve = Mock()

        gui._schedule_survey_geometry_solve()

        self.assertEqual(gui._submit_geometry_solve.call_args.args[2:4], (7.0, 21.0))

    def test_solve_and_refinement_receive_locks_and_the_current_oriented_frame(self) -> None:
        gui = gui_model()
        gui._geometry_executor = Mock()
        gui._geometry_future = None
        current = {"A": (10.0, -4.0), "B": (10.0, 1.0)}
        fixed = {"B": current["B"]}
        gui.survey_model = Mock(
            layout=Mock(positions_m={"A": (0.0, 0.0), "B": (5.0, 0.0)}),
            geometry_pairs=(), geometry_solve_ready=True, geometry_revision=1, run_serial=1,
        )
        gui.survey_geometry_view = Mock(
            distance_weight_power=1.0, nearest_anchor_count=0,
            locked_positions_m=fixed, registration=Mock(reference_positions_m=current),
        )
        gui._effective_geometry_inputs = Mock(return_value=((), frozenset(), (0, 0)))
        gui._submit_geometry_solve(CONNECTIVITY_INTERVAL_ALGORITHM, "Current layout", 7, 15, 1, 1)
        solve_call = gui._geometry_executor.submit.call_args
        self.assertEqual(solve_call.kwargs["fixed_positions_m"], fixed)
        self.assertEqual(solve_call.kwargs["current_positions_m"], current)
        gui._geometry_future = None
        gui._request_distance_only_refinement()
        refine_call = gui._geometry_executor.submit.call_args
        self.assertEqual(refine_call.kwargs["fixed_positions_m"], fixed)
        self.assertEqual(refine_call.args[2], current)

    def test_survey_result_requires_exact_command_identity(self) -> None:
        gui = gui_model()
        start = dispatch(CMD_SURVEY_START, 100, 7)
        self.assertTrue(gui._submit_survey_dispatch(start))

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_START, 101, 7, 0)
        )
        self.assertIsNotNone(gui.survey_command_owner.pending)
        self.assertFalse(gui.survey_model.start_accepted)

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_START, 100, 7, 0)
        )
        self.assertIsNone(gui.survey_command_owner.pending)
        self.assertTrue(gui.survey_model.start_accepted)
        cast(Mock, gui._show_error).assert_not_called()

    def test_plan_waits_behind_start_result_without_bypassing_owner(self) -> None:
        gui = gui_model()
        start = dispatch(CMD_SURVEY_START, 100, 7)
        plan = dispatch(CMD_SURVEY_PLAN, 101, 8)
        gui._submit_survey_dispatch(start)
        self.assertTrue(gui._submit_survey_dispatch(plan))
        self.assertEqual(gui._survey_deferred_dispatch, plan)

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_START, 100, 7, 0)
        )

        self.assertIsNone(gui._survey_deferred_dispatch)
        self.assertIsNotNone(gui.survey_command_owner.pending)
        assert gui.survey_command_owner.pending is not None
        self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_PLAN)
        dispatch_mock = cast(Mock, gui._dispatch_gateway_command)
        self.assertEqual(dispatch_mock.call_args_list[0].args, (start,))
        self.assertEqual(dispatch_mock.call_args_list[1].args, (plan,))

    def test_rejected_start_ends_gui_run_and_surfaces_status(self) -> None:
        gui = gui_model()
        gui._submit_survey_dispatch(dispatch(CMD_SURVEY_START, 100, 7))

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_START, 100, 7, 8)
        )

        self.assertEqual(gui._survey_phase, "idle")
        self.assertFalse(gui.survey_model.active)
        self.assertIn("INTERNAL_ERROR", gui.survey_model.error or "")
        cast(Mock, gui._show_error).assert_called_once()

    def test_survey_command_timeout_keeps_run_for_bounded_reconciliation(self) -> None:
        gui = gui_model()
        gui.survey_command_owner.begin(
            CMD_SURVEY_PLAN,
            100,
            7,
            "survey plan",
            timeout_s=1.0,
            now=0.0,
        )
        gui._survey_pending_dispatch = dispatch(CMD_SURVEY_PLAN, 100, 7)

        with patch("tools.gateway_gui.survey_runtime.time.monotonic", return_value=1.0):
            gui._expire_survey_command()

        self.assertIsNone(gui.survey_command_owner.pending)
        self.assertTrue(gui.survey_model.active)
        self.assertEqual(gui.survey_model.phase, "recovering")
        self.assertEqual(gui._survey_phase, "recovering")

    def test_stale_geometry_completion_restarts_the_newest_pending_solve(self) -> None:
        gui = gui_model()
        gui._geometry_future = Mock()
        gui._geometry_resolve_pending = True
        gui._schedule_survey_geometry_solve = Mock()  # type: ignore[method-assign]

        handled = gui._handle_diagnostic_event(
            {
                "kind": "survey_geometry_solved",
                "run_serial": gui.survey_model.run_serial - 1,
                "revision": 1,
                "layout": object(),
            }
        )

        self.assertTrue(handled)
        self.assertFalse(gui._geometry_resolve_pending)
        cast(Mock, gui._schedule_survey_geometry_solve).assert_called_once()

    def test_same_revision_resolve_completion_is_applied_and_reported(self) -> None:
        gui = gui_model()
        layout = Mock(
            algorithm="Visibility branching neighbor-aware tuned",
            rmse_m=0.123,
            max_residual_m=0.456,
        )
        gui.survey_model = Mock(
            run_serial=8,
            geometry_revision=11,
            layout=Mock(),
        )  # type: ignore[assignment]
        gui.survey_model.apply_layout.return_value = True
        gui._geometry_future = Mock()
        gui._geometry_resolve_pending = False
        gui.survey_geometry_view = Mock()  # type: ignore[assignment]
        gui._apply_survey_geometry_positions = Mock()  # type: ignore[method-assign]
        gui.log_text = Mock()  # type: ignore[assignment]
        gui._append_log = Mock()  # type: ignore[method-assign]

        handled = gui._handle_diagnostic_event(
            {
                "kind": "survey_geometry_solved",
                "run_serial": 8,
                "revision": 11,
                "layout": layout,
            }
        )

        self.assertTrue(handled)
        gui.survey_model.apply_layout.assert_called_once_with(11, layout)
        gui._apply_survey_geometry_positions.assert_called_once_with(
            gui.survey_geometry_view.registration
        )
        self.assertIn("re-solve complete", gui.status_text.get())
        self.assertIn("0.123", gui.status_text.get())

    def test_discarded_solve_cannot_clear_a_newer_geometry_job(self) -> None:
        gui = gui_model()
        current_future = Mock()
        gui._geometry_future = current_future
        gui._geometry_job_serial = 4
        gui.survey_geometry_view = Mock()  # type: ignore[assignment]

        handled = gui._handle_diagnostic_event(
            {
                "kind": "survey_geometry_solved",
                "job_serial": 3,
                "run_serial": gui.survey_model.run_serial,
                "revision": gui.survey_model.geometry_revision,
                "layout": Mock(),
            }
        )

        self.assertTrue(handled)
        self.assertIs(gui._geometry_future, current_future)
        gui.survey_geometry_view.set_geometry_job_pending.assert_not_called()

    def test_early_reliable_events_wait_for_start_and_plan_acceptance(self) -> None:
        gui = gui_model()
        gui.gateway_id = GATEWAY_ID
        gui.host_id_text = FakeVariable(f"0x{HOST_ID:016x}")  # type: ignore[assignment]
        gui.delivery_dedup = GatewayPacketDeduplicator(gateway_id=GATEWAY_ID)
        gui.transport = Mock()  # type: ignore[assignment]
        gui.root = Mock()  # type: ignore[assignment]
        gui.packet_counter = 0
        gui.packet_by_iid = {}
        gui.packet_tree = FakeTree()  # type: ignore[assignment]
        gui.cir_reassembler = Mock()
        gui.cir_reassembler.ingest.return_value = None
        gui.cir_key_by_packet_id = {}
        gui.cir_errors_by_packet_id = {}
        gui._survey_generation = None
        gui._survey_assignment = None
        gui._survey_pairs = ()
        gui._survey_results = {}
        gui._append_log = Mock()  # type: ignore[method-assign]
        gui._packet_summary = Mock(return_value="survey")  # type: ignore[method-assign]
        gui._diagnostic_packet_tags = Mock(return_value=())  # type: ignore[method-assign]
        gui._register_diagnostic_packet_row = Mock()  # type: ignore[method-assign]
        gui._observe_diagnostic_packet = Mock()  # type: ignore[method-assign]
        gui._observe_gateway_id = Mock()  # type: ignore[method-assign]
        gui._schedule_survey_geometry_solve = Mock()  # type: ignore[method-assign]
        gui.survey_model.slot_to_anchor = {0: 0xA1, 1: 0xB2, 2: 0xC3}
        gui.survey_model.slot_hops = {0: 1, 1: 2, 2: 3}

        gui._submit_survey_dispatch(dispatch(CMD_SURVEY_START, 100, 7))
        packet = neighbor_packet()
        received_at = (gui.survey_model.start_dispatched_at or 0.0) + 1.0
        gui._add_packet(packet, received_at=received_at)
        gui._add_packet(packet, received_at=received_at)

        self.assertEqual(len(gui._survey_event_buffer), 1)
        self.assertEqual(gui.delivery_dedup.size, 0)
        gui.transport.send_frame.assert_not_called()

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_START, 100, 7, 0)
        )

        self.assertEqual(gui._survey_event_buffer, [])
        self.assertEqual(gui.survey_model.generation, 9)
        self.assertEqual(gui.delivery_dedup.size, 1)
        gui.transport.send_frame.assert_called_once()

        pairs = ((0, 1), (0, 2), (1, 2))
        gui.survey_model.set_requested_pairs(pairs)
        gui._submit_survey_dispatch(dispatch(CMD_SURVEY_PLAN, 101, 8))
        plan = replace(plan_packet(), age_ms=2_500)
        gui._add_packet(plan, received_at=received_at + 1.0)
        gui._add_packet(plan, received_at=received_at + 1.0)

        self.assertEqual(len(gui._survey_event_buffer), 1)
        self.assertEqual(gui.delivery_dedup.size, 1)
        self.assertEqual(gui.transport.send_frame.call_count, 1)

        gui._observe_survey_command_result(
            result_packet(CMD_SURVEY_PLAN, 101, 8, 0)
        )

        self.assertEqual(gui._survey_event_buffer, [])
        self.assertEqual(len(gui.survey_model.plan_pairs), 3)
        self.assertEqual(gui.delivery_dedup.size, 2)
        self.assertEqual(gui.transport.send_frame.call_count, 2)
        self.assertIsNotNone(gui._scheduled_phase_estimate)
        assert gui._scheduled_phase_estimate is not None
        self.assertEqual(gui._scheduled_phase_estimate.key, "ranging")
        self.assertEqual(
            gui._scheduled_phase_estimate.started_at,
            received_at + 1.0 - 2.5,
        )


if __name__ == "__main__":
    unittest.main()
