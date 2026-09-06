"""Protocol failures must retire all host owners without starving UI deadlines."""
import queue
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace
from tools.gateway_gui.tests.test_command_orchestration import terminal

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.command_orchestration import GatewayCommandDispatch, GatewayCommandOrchestrator, GatewayCommandPlan
from tools.gateway_gui.command_telemetry import GatewayCommandRequestTracker
from tools.gateway_gui.survey_runtime import SurveyCommandOwner, SurveyOperationModel
from tools.gateway_gui.protocol import CMD_ASSIGN_DISCOVERY_SLOTS, CMD_FORCE_REDISCOVERY, CMD_READ_ANCHOR_BATTERY, CMD_IDENTIFY_ANCHOR, CMD_REBOOT, MSG_GATEWAY_COMMAND_EVENT


def gui_owner(command_id=CMD_ASSIGN_DISCOVERY_SLOTS, *, survey=False, phase='target'):
    gui=GatewayGui.__new__(GatewayGui)
    gui.command_request_tracker=GatewayCommandRequestTracker()
    gui.command_orchestrator=GatewayCommandOrchestrator(gui.command_request_tracker)
    request=GatewayCommandDispatch(1 if command_id==CMD_ASSIGN_DISCOVERY_SLOTS else 3,
        command_id,10,1,b'command','command',1.,'running')
    gui.command_orchestrator.begin(GatewayCommandPlan(request,None),now=0.)
    gui.command_orchestrator.phase=phase
    gui.survey_command_owner=SurveyCommandOwner()
    gui.survey_model=SurveyOperationModel()
    if survey:gui.survey_model.begin()
    gui._survey_chain_pending=survey
    gui._survey_phase='enumerating' if survey else 'idle'
    gui._survey_auto_all=survey
    gui._survey_generation=None
    gui._survey_deferred_dispatch=None
    gui._survey_pending_dispatch=None
    gui._survey_event_buffer=[]
    gui.status_text=Mock()
    gui.root=Mock()
    gui.root.winfo_exists.return_value=True
    gui.events=queue.Queue()
    for name in ('_show_error','_refresh_survey_view','_update_command_state',
                 '_clear_scheduled_phase_estimate','_update_scheduled_phase_progress',
                 '_reconcile_stalled_survey'):
        setattr(gui,name,Mock())
    return gui


class CommandRecoveryTests(unittest.TestCase):
    def test_enumeration_timeout_retires_survey_chain_and_model(self):
        for phase in ('preflight','target','target_wait'):
            gui=gui_owner(survey=True,phase=phase)
            gui._expire_gateway_command()
            self.assertFalse(gui.command_orchestrator.active)
            self.assertIsNone(gui.command_request_tracker.pending)
            self.assertEqual(gui._survey_phase,'idle',phase)
            self.assertFalse(gui._survey_chain_pending,phase)
            self.assertFalse(gui.survey_model.active,phase)
            self.assertEqual(gui.survey_model.phase,'failed')
            self.assertFalse(gui._survey_auto_all)

    def test_every_gateway_command_expires_despite_continuous_unrelated_events(self):
        for command in (CMD_ASSIGN_DISCOVERY_SLOTS,CMD_FORCE_REDISCOVERY,
                        CMD_READ_ANCHOR_BATTERY,CMD_IDENTIFY_ANCHOR,CMD_REBOOT):
            gui=gui_owner(command)
            for _ in range(130):gui.events.put({'kind':'noise'})
            gui._handle_event=lambda event:gui.events.put({'kind':'new-noise'})
            for _ in range(4):gui._drain_events()
            self.assertFalse(gui.events.empty())
            self.assertFalse(gui.command_orchestrator.active,command)

    def test_bad_event_cannot_skip_expiry_or_remaining_events(self):
        gui=gui_owner()
        gui.events.put({'kind':'bad'})
        gui.events.put({'kind':'valid'})
        seen=[]
        def handle(event):
            seen.append(event['kind'])
            if event['kind']=='bad':raise ValueError('malformed injected event')
        gui._handle_event=handle
        gui._drain_events()
        self.assertEqual(seen,['bad','valid'])
        self.assertFalse(gui.command_orchestrator.active)
        gui._show_error.assert_called()
        gui.root.after.assert_called()

    def test_late_preflight_receipt_cannot_dispatch_an_expired_target(self):
        gui=gui_owner(phase='target_wait',survey=True)
        transition=gui.command_orchestrator.release_waiting_target(now=2.)
        self.assertTrue(transition.completed)
        self.assertEqual(transition.outcome,'timeout')
        self.assertIsNone(transition.dispatch)
        gui._apply_gateway_command_transition(transition)
        self.assertFalse(gui.survey_model.active)

    def test_late_enumeration_terminal_cannot_start_a_survey(self):
        gui=gui_owner(survey=True)
        event=terminal(gui.command_orchestrator.current)
        gui.command_timeline_model=Mock(enumerated_anchors={})
        gui.mesh_diagnostics_view=Mock()
        gui.topology_model=Mock()
        gui.topology_model.observe.return_value=None
        gui._start_survey_neighbors=Mock()
        with patch('tools.gateway_gui.diagnostics_integration.decode_gateway_command_event', return_value=event):
            gui._observe_diagnostic_packet(SimpleNamespace(msg_type=MSG_GATEWAY_COMMAND_EVENT,payload=b''),received_at=2.)
        gui._start_survey_neighbors.assert_not_called()
        self.assertFalse(gui.survey_model.active)
        self.assertEqual(gui._survey_phase,'idle')

    def test_timely_reply_beyond_first_tick_keeps_original_receive_time(self):
        gui=gui_owner(CMD_REBOOT)
        for _ in range(65):gui.events.put({'kind':'noise'})
        gui.events.put({'kind':'reply','received_at':.5})
        def handle(event):
            if event['kind']=='reply':
                gui._apply_gateway_command_transition(gui.command_orchestrator.observe_command_result(
                    command_id=CMD_REBOOT,host_session_id=10,host_sequence=1,
                    command_status=0,received_at=event['received_at']))
        gui._handle_event=handle
        gui._drain_events()
        self.assertTrue(gui.command_orchestrator.active)
        gui._drain_events()
        self.assertEqual(gui.command_request_tracker.last_outcome,'complete')
