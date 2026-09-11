from dataclasses import replace
import unittest
from unittest.mock import Mock, patch

from tools.gateway_gui.protocol import (
    CMD_SURVEY_CANCEL, CMD_SURVEY_GET_STATUS, CMD_SURVEY_PLAN, CMD_SURVEY_START,
    DecodeError, SURVEY_EVENT_BATCH_COMPLETE, SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED, SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_SIGNALS,
    SURVEY_EVENT_STARTED, SURVEY_EVENT_TERMINAL,
    SURVEY_TERMINAL_ABORTED, SURVEY_TERMINAL_BUSY, SURVEY_TERMINAL_COMPLETE,
    SURVEY_TERMINAL_PARTIAL, SurveyEvent, SurveyPlanPair, SurveyRangeResult,
    decode_survey_event,
)
from tools.gateway_gui.survey_runtime import (
    StaleSurveyEvent, SurveyCommandOwner, SurveyOperationModel, SurveyStateError,
)
from tools.gateway_gui.tests.test_survey_runtime import (
    assignment, enumerate_three, neighbor_event, plan_event,
)
from tools.gateway_gui.tests.test_survey_app_integration import (
    GATEWAY_ID, FakeVariable, dispatch, gui_model, neighbor_packet, result_packet,
)


def packet(event):
    payload = bytearray(72)
    payload[0:4] = bytes((2, event.kind, event.status, event.batch_index))
    payload[4:8] = event.generation.to_bytes(4, "little")
    payload[8:10] = event.partial_reasons.to_bytes(2, "little")
    payload[10:14] = bytes((len(event.range_results), len(event.plan_pairs), event.wave_count, 0))
    payload[14:56] = event.assignment.encode()
    payload[56:60] = event.host_session_id.to_bytes(4, "little")
    payload[60:62] = event.host_sequence.to_bytes(2, "little")
    payload[64] = int(event.final_batch)
    for pair in event.plan_pairs:
        payload.extend((pair.initiator_slot, pair.responder_slot, pair.wave_index))
    for result in event.range_results:
        payload.extend((result.pair_index, result.success_count, result.responder_slot, 0))
        payload.extend(result.median_mm.to_bytes(4, "little", signed=True))
    return replace(neighbor_packet(), payload=bytes(payload))


def started(session=10, sequence=1):
    return SurveyEvent(SURVEY_EVENT_STARTED, 0, 9, assignment(), 0,
                       host_session_id=session, host_sequence=sequence)


def live_gui():
    gui = gui_model()
    enumerate_three(gui.survey_model)
    gui.root = Mock()
    gui.connected = True
    gui.gateway_id = GATEWAY_ID
    gui.host_id_text = FakeVariable("0x42")
    gui._survey_gateway_id = GATEWAY_ID
    gui._survey_generation = None
    gui._survey_assignment = None
    gui._survey_pairs = ()
    gui._survey_auto_all = False
    for name in ("_append_log", "_clear_scheduled_phase_estimate",
                 "_set_scheduled_phase_estimate", "_finish_scheduled_phase_estimate",
                 "_schedule_survey_geometry_solve", "_commit_buffered_survey_packet"):
        setattr(gui, name, Mock())
    gui.survey_model.note_command_dispatched(CMD_SURVEY_START, now=0., session_id=10, sequence=1)
    gui.survey_command_owner.begin(CMD_SURVEY_START, 10, 1, "START", now=0., timeout_s=1.)
    return gui


class SurveyAcceptanceRecoveryTests(unittest.TestCase):
    def test_status_replay_recovers_lost_start_acceptance_and_buffered_graph(self):
        gui = live_gui()
        gui._expire_survey_command(now=2.)
        gui.error_text = FakeVariable(
            "Survey command result was lost; checking gateway state before releasing the run"
        )
        self.assertFalse(gui._observe_survey_event_packet(neighbor_packet(), received_at=2.))
        gui.survey_command_owner.begin(CMD_SURVEY_GET_STATUS, 20, 2, "status", now=2., timeout_s=60.)
        with patch("tools.gateway_gui.app.time.monotonic", return_value=3.):
            self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=3.))
        self.assertEqual(gui._survey_generation, 9)
        self.assertTrue(gui.survey_model.start_accepted)
        self.assertEqual(len(gui.survey_model.neighbor_reports), 3)
        self.assertEqual(gui._survey_event_buffer, [])
        self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_GET_STATUS)
        self.assertIsNone(gui.survey_command_owner.uncertain)
        self.assertEqual(gui._survey_phase, "planning")
        self.assertEqual(gui.error_text.get(), "")
        self.assertIsNone(gui.survey_model.error)
        gui._commit_buffered_survey_packet.assert_called_once()

    def test_started_exposes_generation_for_abort_before_neighbor_graph(self):
        gui = live_gui()
        gui._next_identity = Mock(return_value=(11, 2))
        self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=.5))
        self.assertEqual(gui._survey_generation, 9)
        self.assertFalse(gui.survey_model.neighbor_reports)
        gui._cancel_survey()
        self.assertEqual(gui._survey_phase, "aborting")
        self.assertEqual(gui._dispatch_gateway_command.call_count, 1)

    def test_late_exact_acceptance_preserves_inflight_status_owner(self):
        gui = live_gui()
        gui._expire_survey_command(now=2.)
        gui.survey_command_owner.begin(CMD_SURVEY_GET_STATUS, 20, 2, "status", now=2., timeout_s=60.)
        gui._observe_survey_command_result(result_packet(CMD_SURVEY_START, 10, 2, 0), received_at=3.)
        self.assertFalse(gui.survey_model.start_accepted)
        gui._observe_survey_command_result(result_packet(CMD_SURVEY_START, 10, 1, 0), received_at=3.)
        self.assertTrue(gui.survey_model.start_accepted)
        self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_GET_STATUS)

    def test_wrong_started_identity_and_duplicate_cannot_change_lifecycle(self):
        gui = live_gui()
        self.assertTrue(gui._observe_survey_event_packet(packet(started(sequence=2)), received_at=.5))
        self.assertIsNone(gui.survey_model.generation)
        self.assertFalse(gui.survey_model.start_accepted)
        self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=.5))
        gui.survey_model.phase = "aborting"
        self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=.6))
        self.assertEqual(gui.survey_model.phase, "aborting")

    def test_disconnect_retains_exact_uncertain_control_until_reset(self):
        owner = SurveyCommandOwner()
        owner.begin(CMD_SURVEY_PLAN, 10, 1, "plan", timeout_s=1.)
        owner.disconnect()
        self.assertTrue(owner.observe_result(CMD_SURVEY_PLAN, 10, 1, 0).recovered)
        owner.begin(CMD_SURVEY_START, 11, 2, "start", timeout_s=1.)
        owner.disconnect()
        owner.reset()
        self.assertFalse(owner.observe_result(CMD_SURVEY_START, 11, 2, 0).matched)

    def test_lost_plan_acceptance_replays_before_buffered_terminal(self):
        gui = live_gui()
        gui.survey_model.note_command_accepted(CMD_SURVEY_START)
        gui.survey_model.observe_survey_event(neighbor_event())
        gui.survey_command_owner.reset()
        gui.survey_model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
        gui.survey_model.note_command_dispatched(CMD_SURVEY_PLAN, session_id=11, sequence=2)
        gui.survey_command_owner.begin(CMD_SURVEY_PLAN, 11, 2, "plan", now=0., timeout_s=1.)
        gui._expire_survey_command(now=2.)
        terminal = SurveyEvent(SURVEY_EVENT_TERMINAL, SURVEY_TERMINAL_COMPLETE, 9, assignment(), 0,
            range_results=(SurveyRangeResult(0, 5, 1, 3000), SurveyRangeResult(1, 5, 2, 4000), SurveyRangeResult(2, 5, 2, 5000)))
        self.assertFalse(gui._observe_survey_event_packet(packet(terminal), received_at=3.))
        self.assertTrue(gui._observe_survey_event_packet(packet(replace(plan_event(), host_session_id=11, host_sequence=2)), received_at=3.))
        self.assertFalse(gui.survey_model.active)
        self.assertEqual(gui._survey_phase, "idle")
        self.assertEqual(len(gui.survey_model.results), 3)

    def test_reserved_acceptance_and_range_bytes_fail_closed(self):
        valid = packet(started())
        self.assertEqual(decode_survey_event(valid).host_session_id, 10)
        for offset in (62, 63, 65, 71):
            payload = bytearray(valid.payload); payload[offset] = 1
            with self.assertRaises(DecodeError):
                decode_survey_event(bytes(payload))
        for event in (started(), replace(plan_event(), host_session_id=11, host_sequence=2)):
            with self.assertRaises(DecodeError):
                decode_survey_event(packet(replace(event, status=SURVEY_TERMINAL_ABORTED)))
        payload = bytearray(valid.payload); payload[1] = SURVEY_EVENT_RANGE_PROGRESS
        with self.assertRaises(DecodeError):
            decode_survey_event(bytes(payload))

    def test_plan_status_replay_merges_partial_without_reappending_or_changing_phase(self):
        gui = live_gui()
        model = gui.survey_model
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event())
        model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
        model.note_command_dispatched(CMD_SURVEY_PLAN, session_id=11, sequence=2)
        accepted = replace(plan_event(), host_session_id=11, host_sequence=2)
        model.observe_survey_event(accepted)
        model.phase = "aborting"
        model._applied_events.clear()  # Replay correctness cannot depend on history retention.
        replay = replace(accepted, status=SURVEY_TERMINAL_PARTIAL, partial_reasons=16)
        self.assertFalse(model.observe_survey_event(replay))
        self.assertEqual(len(model.plan_pairs), 3)
        self.assertEqual(model.partial_reasons, 16)
        self.assertEqual(model.phase, "aborting")
        with self.assertRaises(SurveyStateError):
            model.observe_survey_event(replace(replay, wave_count=3))


class SurveyCleanupStatusTests(unittest.TestCase):
    def cancelling_gui(self, *, plan_state):
        gui = live_gui()
        self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=.5))
        model = gui.survey_model
        if plan_state != "not_requested":
            model.observe_survey_event(neighbor_event())
            model.set_requested_pairs(((0, 1), (0, 2), (1, 2)))
            model.note_command_dispatched(CMD_SURVEY_PLAN, session_id=11, sequence=2)
            if plan_state == "accepted":
                self.assertTrue(gui._observe_survey_event_packet(packet(replace(
                    plan_event(), host_session_id=11, host_sequence=2, final_batch=True,
                )), received_at=1.))
        model.note_command_dispatched(CMD_SURVEY_CANCEL, session_id=12, sequence=3)
        model.note_command_accepted(CMD_SURVEY_CANCEL)
        gui._survey_phase = "aborting"
        gui.survey_command_owner.reset()
        gui.survey_command_owner.begin(
            CMD_SURVEY_GET_STATUS, 13, 4, "status", now=2., timeout_s=60.,
        )
        return gui

    def test_cleanup_status_keeps_owner_until_exact_aborted_terminal(self):
        # Firmware can finish collecting results before its conservative remote
        # self-stop deadline. GET_STATUS exposes progress during that interval,
        # including a final batch with every result already present.
        complete_results = (
            SurveyRangeResult(0, 5, 1, 3000),
            SurveyRangeResult(1, 5, 2, 4000),
            SurveyRangeResult(2, 5, 2, 5000),
        )
        cases = (("not_requested", ()), ("accepted", ()), ("accepted", complete_results))
        for plan_state, results in cases:
            for partial_reasons in (0, 16):
                with self.subTest(plan_state=plan_state, results=len(results), partial=partial_reasons):
                    gui = self.cancelling_gui(plan_state=plan_state)
                    model = gui.survey_model
                    pending_status = gui.survey_command_owner.pending
                    identity = (model.run_serial, model.generation, model.assignment,
                                model.start_command_identity, model.plan_command_identity)
                    snapshot = SurveyEvent(
                        SURVEY_EVENT_RANGE_PROGRESS,
                        SURVEY_TERMINAL_PARTIAL if partial_reasons else SURVEY_TERMINAL_COMPLETE,
                        9, assignment(), partial_reasons,
                        range_results=results, final_batch=plan_state == "accepted",
                    )
                    self.assertEqual(decode_survey_event(packet(snapshot)), snapshot)
                    for received_at in (3., 4.):
                        self.assertTrue(gui._observe_survey_event_packet(
                            packet(snapshot), received_at=received_at,
                        ))
                        self.assertTrue(model.active)
                        self.assertEqual(model.phase, "aborting")
                        self.assertEqual(gui._survey_phase, "aborting")
                        self.assertIsNone(model.terminal_status)
                        self.assertIs(gui.survey_command_owner.pending, pending_status)
                        self.assertEqual(identity, (
                            model.run_serial, model.generation, model.assignment,
                            model.start_command_identity, model.plan_command_identity,
                        ))
                    self.assertEqual(tuple(model.results.values()), results)
                    gui._observe_survey_command_result(result_packet(
                        CMD_SURVEY_GET_STATUS, 13, 4, 0,
                    ), received_at=5.)
                    self.assertTrue(model.active)
                    self.assertIsNone(gui.survey_command_owner.pending)
                    terminal = replace(snapshot, kind=SURVEY_EVENT_TERMINAL,
                                       status=SURVEY_TERMINAL_ABORTED)
                    self.assertTrue(gui._observe_survey_event_packet(
                        packet(replace(terminal, generation=10)), received_at=6.,
                    ))
                    self.assertTrue(model.active)
                    self.assertTrue(gui._observe_survey_event_packet(
                        packet(terminal), received_at=7.,
                    ))
                    self.assertFalse(model.active)
                    self.assertEqual(model.terminal_status, SURVEY_TERMINAL_ABORTED)
                    self.assertEqual(gui._survey_phase, "idle")
                    self.assertEqual(gui._survey_event_buffer, [])

    def test_cleanup_status_waiting_for_plan_acceptance_cannot_release_owner(self):
        gui = self.cancelling_gui(plan_state="pending")
        model = gui.survey_model
        pending_status = gui.survey_command_owner.pending
        snapshot = SurveyEvent(
            SURVEY_EVENT_RANGE_PROGRESS, SURVEY_TERMINAL_PARTIAL,
            9, assignment(), 16, final_batch=True,
        )
        self.assertEqual(decode_survey_event(packet(snapshot)), snapshot)
        for received_at in (3., 4.):
            self.assertFalse(gui._observe_survey_event_packet(
                packet(snapshot), received_at=received_at,
            ))
            self.assertTrue(model.active)
            self.assertFalse(model.plan_accepted)
            self.assertEqual(model.phase, "aborting")
            self.assertEqual(gui._survey_phase, "aborting")
            self.assertIsNone(model.terminal_status)
            self.assertIs(gui.survey_command_owner.pending, pending_status)
            self.assertEqual(len(gui._survey_event_buffer), 1)
        # No PLAN was accepted, so terminal cleanup must need no pair schedule.
        terminal = replace(snapshot, kind=SURVEY_EVENT_TERMINAL,
                           status=SURVEY_TERMINAL_ABORTED)
        self.assertTrue(gui._observe_survey_event_packet(packet(terminal), received_at=5.))
        self.assertFalse(model.active)
        self.assertEqual(model.terminal_status, SURVEY_TERMINAL_ABORTED)
        self.assertEqual(gui._survey_phase, "idle")
        self.assertIsNone(gui.survey_command_owner.pending)
        self.assertEqual(gui._survey_event_buffer, [])


class SurveyAbortIntentTests(unittest.TestCase):
    pairs = ((0, 1), (0, 2), (1, 2))
    results = (SurveyRangeResult(0, 5, 1, 3000),
               SurveyRangeResult(1, 5, 2, 4000),
               SurveyRangeResult(2, 5, 2, 5000))

    def prepared_gui(self, *, graph=False, plan=False):
        gui = live_gui()
        with patch("tools.gateway_gui.app.time.monotonic", return_value=.5):
            self.assertTrue(gui._observe_survey_event_packet(packet(started()), received_at=.5))
        gui._survey_batch_cursor = 0
        gui._survey_batch_pair_limit = 100
        gui._survey_pair_batches = (self.pairs, ((0, 1),))
        if graph or plan:
            gui.survey_model.observe_survey_event(neighbor_event())
            gui._survey_phase = "planning"
        if plan:
            gui.survey_model.set_requested_pairs(self.pairs)
            gui.survey_model.note_command_dispatched(CMD_SURVEY_PLAN, session_id=11, sequence=2)
            self.assertTrue(gui._observe_survey_event_packet(packet(replace(
                plan_event(), host_session_id=11, host_sequence=2,
            )), received_at=1.))
        gui._next_identity = Mock(return_value=(12, 3))
        return gui

    def request_cancel(self, gui, *, accepted=False):
        with patch("tools.gateway_gui.app.time.monotonic", return_value=2.):
            gui._cancel_survey()
        self.assertTrue(gui.survey_model.active)
        self.assertEqual(gui.survey_model.phase, "aborting")
        self.assertEqual(gui._survey_phase, "aborting")
        if accepted:
            with patch("tools.gateway_gui.app.time.monotonic", return_value=2.1):
                gui._observe_survey_command_result(
                    result_packet(CMD_SURVEY_CANCEL, 12, 3, 0), received_at=2.1,
                )
        gui.root.after_idle.reset_mock()
        gui._next_identity.reset_mock()
        gui._dispatch_gateway_command.reset_mock()
        gui._set_scheduled_phase_estimate.reset_mock()
        gui._finish_scheduled_phase_estimate.reset_mock()

    def assert_aborting_without_new_work(self, gui):
        self.assertTrue(gui.survey_model.active)
        self.assertEqual(gui.survey_model.phase, "aborting")
        self.assertEqual(gui._survey_phase, "aborting")
        self.assertIsNone(gui.survey_model.terminal_status)
        gui.root.after_idle.assert_not_called()
        gui._next_identity.assert_not_called()
        gui._dispatch_gateway_command.assert_not_called()
        gui._set_scheduled_phase_estimate.assert_not_called()
        gui._finish_scheduled_phase_estimate.assert_not_called()

    def terminal(self, gui, *, received_at=8.):
        event = SurveyEvent(
            SURVEY_EVENT_TERMINAL, SURVEY_TERMINAL_ABORTED,
            9, assignment(), gui.survey_model.partial_reasons,
            batch_index=gui.survey_model.next_batch_index,
        )
        self.assertTrue(gui._observe_survey_event_packet(packet(event), received_at=received_at))
        self.assertFalse(gui.survey_model.active)
        self.assertEqual(gui._survey_phase, "idle")
        self.assertEqual(gui.survey_model.terminal_status, SURVEY_TERMINAL_ABORTED)
        self.assertIsNone(gui.survey_command_owner.pending)
        self.assertIsNone(gui._survey_deferred_dispatch)
        self.assertEqual(gui._survey_event_buffer, [])

    def test_every_accepted_nonterminal_retains_abort_intent_and_data(self):
        kinds = (SURVEY_EVENT_STARTED, SURVEY_EVENT_NEIGHBOR_GRAPH,
                 SURVEY_EVENT_SIGNALS, SURVEY_EVENT_PLAN_ACCEPTED,
                 SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_BATCH_COMPLETE)
        for kind in kinds:
            for accepted in (False, True):
                with self.subTest(kind=kind, cancel_accepted=accepted):
                    gui = self.prepared_gui(
                        graph=kind in (SURVEY_EVENT_SIGNALS, SURVEY_EVENT_PLAN_ACCEPTED),
                        plan=kind in (SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_BATCH_COMPLETE),
                    )
                    model = gui.survey_model
                    if kind == SURVEY_EVENT_STARTED:
                        # A replay must remain safe after bounded duplicate
                        # history has been reclaimed while the owner is live.
                        model._applied_events.clear()
                        incoming = packet(started())
                    elif kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
                        incoming = neighbor_packet()
                    elif kind == SURVEY_EVENT_SIGNALS:
                        raw = bytearray(50)
                        raw[:4] = bytes((2, SURVEY_EVENT_SIGNALS, 0, 1))
                        raw[4:8] = (9).to_bytes(4, "little")
                        raw[8:50] = assignment().encode()
                        raw.extend((50, 7, 0, 0, 0, 0, 0, 0))
                        incoming = replace(neighbor_packet(), payload=bytes(raw))
                    elif kind == SURVEY_EVENT_PLAN_ACCEPTED:
                        model.set_requested_pairs(self.pairs)
                        model.note_command_dispatched(CMD_SURVEY_PLAN, session_id=11, sequence=2)
                        incoming = packet(replace(plan_event(), host_session_id=11, host_sequence=2))
                    else:
                        incoming = packet(SurveyEvent(
                            kind, SURVEY_TERMINAL_COMPLETE, 9, assignment(), 0,
                            range_results=self.results,
                        ))
                    event = decode_survey_event(incoming)
                    self.request_cancel(gui, accepted=accepted)
                    pending = gui.survey_command_owner.pending
                    identity = (model.run_serial, model.generation, model.assignment)
                    for received_at in (3., 4.):
                        self.assertTrue(gui._observe_survey_event_packet(incoming, received_at=received_at))
                        self.assert_aborting_without_new_work(gui)
                        self.assertIs(gui.survey_command_owner.pending, pending)
                        self.assertEqual((model.run_serial, model.generation, model.assignment), identity)
                        self.assertEqual(gui._survey_batch_cursor, model.next_batch_index)
                    if kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
                        self.assertEqual(model.neighbor_reports, event.neighbor_reports)
                    elif kind == SURVEY_EVENT_SIGNALS:
                        self.assertEqual(tuple(model.signal_measurements.values()), event.signal_measurements)
                    elif kind == SURVEY_EVENT_PLAN_ACCEPTED:
                        self.assertEqual(model.plan_pairs, event.plan_pairs)
                        self.assertEqual(gui._survey_pairs, self.pairs)
                    elif kind in (SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_BATCH_COMPLETE):
                        self.assertEqual(tuple(model.results.values()), self.results)
                        self.assertEqual(tuple(gui._survey_results.values()), self.results)
                    retained_results = dict(model.results)
                    self.terminal(gui)
                    self.assertEqual(model.results, retained_results)

    def queue_cancel_behind(self, command, *, timeout_s=60.):
        gui = self.prepared_gui(graph=command == CMD_SURVEY_PLAN)
        session, sequence = (10, 1) if command == CMD_SURVEY_START else (11, 2)
        if command == CMD_SURVEY_PLAN:
            gui.survey_model.set_requested_pairs(self.pairs)
        # Preserve the known generation while a control receipt is unresolved;
        # this exercises the command-owner boundary independently of which
        # earlier notification first exposed that generation to the GUI.
        gui.survey_model.note_command_dispatched(
            command, now=1., session_id=session, sequence=sequence,
        )
        gui.survey_model._applied_events.clear()
        gui.survey_command_owner.begin(command, session, sequence, "control", now=1., timeout_s=timeout_s)
        self.request_cancel(gui)
        self.assertEqual(gui.survey_command_owner.pending.command_id, command)
        self.assertEqual(gui._survey_deferred_dispatch.command_id, CMD_SURVEY_CANCEL)
        return gui, session, sequence

    def test_queued_cancel_survives_exact_late_start_and_plan_acceptance(self):
        for command in (CMD_SURVEY_START, CMD_SURVEY_PLAN):
            for authoritative in (False, True):
                with self.subTest(command=command, authoritative=authoritative):
                    gui, session, sequence = self.queue_cancel_behind(command)
                    acceptance = started() if command == CMD_SURVEY_START else replace(
                        plan_event(), host_session_id=session, host_sequence=sequence,
                    )
                    with patch("tools.gateway_gui.app.time.monotonic", return_value=3.):
                        if authoritative:
                            self.assertTrue(gui._observe_survey_event_packet(packet(acceptance), received_at=3.))
                        else:
                            gui._observe_survey_command_result(
                                result_packet(command, session, sequence, 0), received_at=3.,
                            )
                    sent = gui._dispatch_gateway_command.call_args_list
                    self.assertEqual(len(sent), 1)
                    self.assertEqual(sent[0].args[0].command_id, CMD_SURVEY_CANCEL)
                    pending = gui.survey_command_owner.pending
                    self.assertEqual((pending.command_id, pending.session_id, pending.sequence),
                                     (CMD_SURVEY_CANCEL, 12, 3))
                    self.assertIsNone(gui._survey_deferred_dispatch)
                    gui._dispatch_gateway_command.reset_mock()
                    # The acceptance's other representation may arrive next.
                    # It cannot dispatch CANCEL twice or create fresh work.
                    with patch("tools.gateway_gui.app.time.monotonic", return_value=4.):
                        self.assertTrue(gui._observe_survey_event_packet(packet(acceptance), received_at=4.))
                        gui._observe_survey_command_result(
                            result_packet(command, session, sequence, 0), received_at=4.,
                        )
                    self.assert_aborting_without_new_work(gui)
                    self.assertIs(gui.survey_command_owner.pending, pending)
                    self.terminal(gui)

    def test_expired_start_or_plan_dispatches_queued_cancel_without_losing_intent(self):
        for command in (CMD_SURVEY_START, CMD_SURVEY_PLAN):
            with self.subTest(command=command):
                gui, session, sequence = self.queue_cancel_behind(command, timeout_s=1.)
                with patch("tools.gateway_gui.app.time.monotonic", return_value=3.):
                    gui._expire_survey_command(now=3.)
                self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_CANCEL)
                self.assertIsNone(gui._survey_deferred_dispatch)
                self.assertEqual(gui._dispatch_gateway_command.call_count, 1)
                self.assertEqual(gui._dispatch_gateway_command.call_args.args[0].command_id,
                                 CMD_SURVEY_CANCEL)
                uncertain = gui.survey_command_owner.uncertain
                self.assertEqual((uncertain.command_id, uncertain.session_id, uncertain.sequence),
                                 (command, session, sequence))
                gui._dispatch_gateway_command.reset_mock()
                self.assert_aborting_without_new_work(gui)
                # A late result still reconciles its original identity without
                # taking custody away from the now-transmitted CANCEL.
                with patch("tools.gateway_gui.app.time.monotonic", return_value=4.):
                    gui._observe_survey_command_result(
                        result_packet(command, session, sequence, 0), received_at=4.,
                    )
                self.assertIsNone(gui.survey_command_owner.uncertain)
                self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_CANCEL)
                self.assert_aborting_without_new_work(gui)
                self.terminal(gui)

    def test_cancel_supersedes_unsent_plan_without_releasing_current_status_owner(self):
        gui = self.prepared_gui(graph=True)
        gui.survey_command_owner.begin(CMD_SURVEY_GET_STATUS, 20, 4, "status", now=1., timeout_s=60.)
        status_owner = gui.survey_command_owner.pending
        unsent_plan = dispatch(CMD_SURVEY_PLAN, 21, 5)
        self.assertTrue(gui._submit_survey_dispatch(unsent_plan))
        self.assertIs(gui._survey_deferred_dispatch, unsent_plan)
        self.request_cancel(gui)
        self.assertIs(gui.survey_command_owner.pending, status_owner)
        self.assertEqual(gui._survey_deferred_dispatch.command_id, CMD_SURVEY_CANCEL)
        self.assert_aborting_without_new_work(gui)
        with patch("tools.gateway_gui.app.time.monotonic", return_value=3.):
            gui._observe_survey_command_result(
                result_packet(CMD_SURVEY_GET_STATUS, 20, 4, 0), received_at=3.,
            )
        sent = gui._dispatch_gateway_command.call_args_list
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0].args[0].command_id, CMD_SURVEY_CANCEL)
        gui._dispatch_gateway_command.reset_mock()
        self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_CANCEL)
        self.assertIsNone(gui._survey_deferred_dispatch)
        self.assert_aborting_without_new_work(gui)
        self.terminal(gui)

    def test_cancel_timeout_keeps_abort_owner_and_cannot_reopen_work(self):
        gui = self.prepared_gui(plan=True)
        self.request_cancel(gui)
        pending = gui.survey_command_owner.pending
        deadline = pending.started_at + pending.timeout_s
        with patch("tools.gateway_gui.app.time.monotonic", return_value=deadline + 1.):
            gui._expire_survey_command(now=deadline + 1.)
        self.assertIsNone(gui.survey_command_owner.pending)
        self.assert_aborting_without_new_work(gui)
        progress = packet(SurveyEvent(
            SURVEY_EVENT_RANGE_PROGRESS, SURVEY_TERMINAL_COMPLETE,
            9, assignment(), 0, range_results=self.results,
        ))
        self.assertTrue(gui._observe_survey_event_packet(progress, received_at=deadline + 2.))
        self.assert_aborting_without_new_work(gui)
        self.assertEqual(tuple(gui._survey_results.values()), self.results)
        self.terminal(gui, received_at=deadline + 3.)

    def pending_plan_gui(self):
        gui = self.prepared_gui(graph=True)
        self.assertTrue(gui.survey_model.start_accepted)
        self.assertEqual(gui.survey_model.generation, 9)
        self.assertEqual(gui.survey_model.assignment, assignment())
        gui.survey_model.set_requested_pairs(self.pairs)
        gui.survey_model.note_command_dispatched(CMD_SURVEY_PLAN, now=1., session_id=11, sequence=2)
        gui.survey_command_owner.begin(CMD_SURVEY_PLAN, 11, 2, "PLAN", now=1., timeout_s=60.)
        return gui

    def test_rejected_plan_preserves_start_owner_and_sends_exactly_one_cancel(self):
        # Production rejects PLAN on busy/stale/submit failure while its
        # accepted START and anchor leases remain active. A command rejection
        # therefore cannot be projected as survey completion.
        for status in (3, 7, 8):
            for already_queued in (False, True):
                with self.subTest(status=status, cancel_queued=already_queued):
                    gui = self.pending_plan_gui()
                    reports = gui.survey_model.neighbor_reports
                    if already_queued:
                        self.request_cancel(gui)
                    with patch("tools.gateway_gui.app.time.monotonic", return_value=3.):
                        gui._observe_survey_command_result(
                            result_packet(CMD_SURVEY_PLAN, 11, 2, status), received_at=3.,
                        )
                    self.assertTrue(gui.survey_model.start_accepted)
                    self.assertEqual(gui.survey_model.neighbor_reports, reports)
                    self.assertEqual(gui.survey_model.requested_pairs, self.pairs)
                    self.assertIn("rejected", gui.survey_model.error)
                    pending = gui.survey_command_owner.pending
                    self.assertEqual((pending.command_id, pending.session_id, pending.sequence),
                                     (CMD_SURVEY_CANCEL, 12, 3))
                    self.assertIsNone(gui._survey_deferred_dispatch)
                    self.assertEqual(gui._dispatch_gateway_command.call_count, 1)
                    self.assertEqual(gui._dispatch_gateway_command.call_args.args[0].command_id,
                                     CMD_SURVEY_CANCEL)
                    if already_queued:
                        gui._next_identity.assert_not_called()
                    else:
                        gui._next_identity.assert_called_once()
                    gui._dispatch_gateway_command.reset_mock()
                    gui._next_identity.reset_mock()
                    self.assert_aborting_without_new_work(gui)
                    # A duplicate rejection cannot allocate a second CANCEL.
                    gui._observe_survey_command_result(
                        result_packet(CMD_SURVEY_PLAN, 11, 2, status), received_at=4.,
                    )
                    self.assertIs(gui.survey_command_owner.pending, pending)
                    self.assert_aborting_without_new_work(gui)
                    self.terminal(gui)

    def test_late_uncertain_plan_rejection_preserves_inflight_status_or_cancel(self):
        for status in (3, 7, 8):
            for current in (CMD_SURVEY_GET_STATUS, CMD_SURVEY_CANCEL):
                with self.subTest(status=status, current=current):
                    gui = self.pending_plan_gui()
                    with patch("tools.gateway_gui.app.time.monotonic", return_value=62.):
                        gui._expire_survey_command(now=62.)
                    self.assertEqual(gui.survey_command_owner.uncertain.command_id, CMD_SURVEY_PLAN)
                    if current == CMD_SURVEY_GET_STATUS:
                        gui.survey_command_owner.begin(current, 20, 4, "status", now=63., timeout_s=60.)
                    else:
                        with patch("tools.gateway_gui.app.time.monotonic", return_value=63.):
                            gui._cancel_survey()
                    owner = gui.survey_command_owner.pending
                    gui._dispatch_gateway_command.reset_mock()
                    gui._next_identity.reset_mock()
                    with patch("tools.gateway_gui.app.time.monotonic", return_value=64.):
                        gui._observe_survey_command_result(
                            result_packet(CMD_SURVEY_PLAN, 11, 2, status), received_at=64.,
                        )
                    self.assertIsNone(gui.survey_command_owner.uncertain)
                    self.assertIs(gui.survey_command_owner.pending, owner)
                    self.assertTrue(gui.survey_model.active)
                    self.assertEqual(gui.survey_model.phase, "aborting")
                    self.assertEqual(gui._survey_phase, "aborting")
                    self.assertIsNone(gui.survey_model.terminal_status)
                    gui._dispatch_gateway_command.assert_not_called()
                    if current == CMD_SURVEY_GET_STATUS:
                        self.assertEqual(gui._survey_deferred_dispatch.command_id, CMD_SURVEY_CANCEL)
                        gui._next_identity.assert_called_once()
                        with patch("tools.gateway_gui.app.time.monotonic", return_value=65.):
                            gui._observe_survey_command_result(
                                result_packet(CMD_SURVEY_GET_STATUS, 20, 4, 0), received_at=65.,
                            )
                        self.assertEqual(gui._dispatch_gateway_command.call_count, 1)
                        self.assertEqual(gui._dispatch_gateway_command.call_args.args[0].command_id,
                                         CMD_SURVEY_CANCEL)
                    else:
                        self.assertIsNone(gui._survey_deferred_dispatch)
                        gui._next_identity.assert_not_called()
                    gui._dispatch_gateway_command.reset_mock()
                    gui._next_identity.reset_mock()
                    self.assertEqual(gui.survey_command_owner.pending.command_id, CMD_SURVEY_CANCEL)
                    self.assert_aborting_without_new_work(gui)
                    self.terminal(gui, received_at=66.)

    def test_rejected_start_without_remote_identity_still_fails_without_cancel(self):
        gui = live_gui()
        gui._next_identity = Mock()
        self.assertIsNone(gui.survey_model.generation)
        with patch("tools.gateway_gui.app.time.monotonic", return_value=.5):
            gui._observe_survey_command_result(
                result_packet(CMD_SURVEY_START, 10, 1, 3), received_at=.5,
            )
        self.assertFalse(gui.survey_model.active)
        self.assertEqual(gui._survey_phase, "idle")
        self.assertIsNone(gui.survey_command_owner.pending)
        self.assertIsNone(gui._survey_deferred_dispatch)
        gui._next_identity.assert_not_called()
        gui._dispatch_gateway_command.assert_not_called()

    def test_callbacks_queued_before_cancel_cannot_submit_work_after_late_data(self):
        for batch in (False, True):
            with self.subTest(batch=batch):
                gui = self.prepared_gui(plan=batch)
                if batch:
                    incoming = packet(SurveyEvent(
                        SURVEY_EVENT_BATCH_COMPLETE, SURVEY_TERMINAL_COMPLETE,
                        9, assignment(), 0, range_results=self.results,
                    ))
                else:
                    incoming = neighbor_packet()
                self.assertTrue(gui._observe_survey_event_packet(incoming, received_at=1.5))
                callbacks = [call.args[0] for call in gui.root.after_idle.call_args_list]
                self.assertEqual(len(callbacks), 1)
                self.request_cancel(gui)
                # A valid empty status event is enough to reproduce the old
                # display-phase overwrite before an already queued callback.
                progress = packet(SurveyEvent(
                    SURVEY_EVENT_RANGE_PROGRESS, SURVEY_TERMINAL_COMPLETE,
                    9, assignment(), 0, batch_index=gui.survey_model.next_batch_index,
                ))
                gui._observe_survey_event_packet(progress, received_at=3.)
                for callback in callbacks:
                    callback()
                self.assert_aborting_without_new_work(gui)
                self.terminal(gui)


class SurveyBatchIsolationTests(unittest.TestCase):
    def model(self):
        model = SurveyOperationModel(); model.begin(); enumerate_three(model)
        model.note_command_accepted(CMD_SURVEY_START)
        model.observe_survey_event(neighbor_event())
        model.set_requested_pairs(((0, 2),)); model.note_command_accepted(CMD_SURVEY_PLAN)
        first = replace(plan_event(), plan_pairs=(SurveyPlanPair(0, 2, 0),), wave_count=1)
        model.observe_survey_event(first)
        model.observe_survey_event(SurveyEvent(SURVEY_EVENT_BATCH_COMPLETE, SURVEY_TERMINAL_PARTIAL, 9, assignment(), 16))
        model.set_requested_pairs(((1, 2),)); model.note_command_accepted(CMD_SURVEY_PLAN)
        model.observe_survey_event(replace(first, plan_pairs=(SurveyPlanPair(1, 2, 0),), batch_index=1))
        return model

    def test_other_batch_rejected_before_results_or_partial_flags_mutate(self):
        for kind in (SURVEY_EVENT_RANGE_PROGRESS, SURVEY_EVENT_BATCH_COMPLETE, SURVEY_EVENT_TERMINAL):
            for batch, error in ((0, StaleSurveyEvent), (2, SurveyStateError)):
                model = self.model()
                event = SurveyEvent(kind, SURVEY_TERMINAL_COMPLETE, 9, assignment(), 32, batch_index=batch,
                    range_results=(SurveyRangeResult(0, 5, 2, 4000),))
                with self.assertRaises(error): model.observe_survey_event(event)
                self.assertEqual(model.results, {})
                self.assertEqual(model.partial_reasons, 16)
                self.assertTrue(model.active)

    def test_clean_last_batch_keeps_prior_partial_outcome_and_stops_auto_continue(self):
        gui = live_gui(); gui.survey_model = self.model(); gui._survey_auto_all = True
        gui._survey_pairs = ((0, 2), (1, 2))
        terminal = SurveyEvent(SURVEY_EVENT_TERMINAL, SURVEY_TERMINAL_COMPLETE, 9, assignment(), 0, batch_index=1,
            range_results=(SurveyRangeResult(0, 5, 2, 4000),))
        self.assertTrue(gui._observe_survey_event_packet(packet(terminal)))
        self.assertEqual(gui.survey_model.terminal_status, SURVEY_TERMINAL_PARTIAL)
        self.assertFalse(gui._survey_auto_all)
        self.assertIn("Survey partial", gui.status_text.get())
        self.assertIn("partial flags=0x0010", gui.status_text.get())

    def test_between_batch_abort_failure_and_timeout_need_no_unaccepted_plan(self):
        for status in (SURVEY_TERMINAL_ABORTED, SURVEY_TERMINAL_BUSY, SURVEY_TERMINAL_PARTIAL):
            gui = live_gui(); model = self.model(); gui.survey_model = model
            # Batch 1 has drained, and firmware waits for batch 2. The last
            # requested pairs remain retained for diagnostics.
            model.observe_survey_event(SurveyEvent(
                SURVEY_EVENT_BATCH_COMPLETE, SURVEY_TERMINAL_PARTIAL,
                9, assignment(), 16, batch_index=1,
            ))
            self.assertEqual(model.next_batch_index, 2)
            self.assertFalse(model.plan_accepted)
            self.assertTrue(model.requested_pairs)
            terminal = SurveyEvent(
                SURVEY_EVENT_TERMINAL, status, 9, assignment(), 16, batch_index=2,
            )
            gui.survey_command_owner.reset()
            gui.survey_command_owner.begin(CMD_SURVEY_PLAN, 12, 3, "plan", now=0., timeout_s=1.)
            self.assertTrue(gui._observe_survey_event_packet(packet(terminal)))
            self.assertFalse(model.active)
            self.assertEqual(model.terminal_status, status)
            self.assertEqual(gui._survey_phase, "idle")
            self.assertEqual(gui._survey_event_buffer, [])
            self.assertIsNone(gui.survey_command_owner.pending)
            gui._expire_survey_command(now=100.)
            self.assertEqual(model.phase, "terminal")

    def test_queued_batch_callback_cannot_run_after_terminal_or_into_a_new_run(self):
        for replacement in ("terminal", "new_identity", "reused_identity"):
            gui = live_gui(); gui.survey_model = self.model()
            gui._survey_batch_cursor = 1
            gui._survey_pair_batches = (((0, 2),), ((1, 2),), ((0, 1),))
            gui._next_identity = Mock(return_value=(100, 3))
            completed = SurveyEvent(
                SURVEY_EVENT_BATCH_COMPLETE, SURVEY_TERMINAL_PARTIAL,
                9, assignment(), 16, batch_index=1,
            )
            self.assertTrue(gui._observe_survey_event_packet(packet(completed)))
            callback = gui.root.after_idle.call_args.args[0]
            if replacement == "terminal":
                terminal = SurveyEvent(
                    SURVEY_EVENT_TERMINAL, SURVEY_TERMINAL_ABORTED,
                    9, assignment(), 16, batch_index=2,
                )
                self.assertTrue(gui._observe_survey_event_packet(packet(terminal)))
            else:
                gui.survey_model.begin()
                gui._survey_generation = gui.survey_model.generation = (
                    10 if replacement == "new_identity" else 9
                )
                gui._survey_assignment = gui.survey_model.assignment = assignment()
                gui._survey_phase = gui.survey_model.phase = "planning"
            before = (
                gui.survey_model.run_serial, gui.survey_model.phase,
                gui._survey_phase, gui._survey_batch_cursor,
                gui.survey_model.requested_pairs,
            )
            callback()
            self.assertEqual(before, (
                gui.survey_model.run_serial, gui.survey_model.phase,
                gui._survey_phase, gui._survey_batch_cursor,
                gui.survey_model.requested_pairs,
            ), replacement)
            gui._next_identity.assert_not_called()
            gui._dispatch_gateway_command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
