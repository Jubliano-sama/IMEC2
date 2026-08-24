"""Minimal GatewayGui wiring for focused diagnostic models and views."""

from __future__ import annotations

from pathlib import Path
import time
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Any, Callable, cast

from .diagnostic_models import (
    ClickLocationModel, CommandTimelineModel, PendingWakeAttemptAdapter,
    TopologyBaselineModel, WakeEvidence, WakeTrainMonitor, anchor_label,
)
from .diagnostic_views import ClickDiagnosticsView, MeshDiagnosticsView
from .command_telemetry import (
    CommandTelemetryDecodeError, GatewayCommandRequestTracker,
    decode_gateway_command_event,
)
from .command_orchestration import (
    GatewayAssignmentReplayBarrier,
    GatewayCommandOrchestrator,
)
from .protocol import (
    COMMAND_STATUS_NAMES, MSG_CLICK_REPORT, MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT, Packet, TLV_ANCHOR_ID, TLV_CLICKER_ID,
    TLV_COMMAND_ID, TLV_COMMAND_STATUS, TLV_EVENT_SEQ,
)


class GatewayDiagnosticsMixin:
    root: Any
    events: Any
    packet_tree: Any
    packet_by_iid: dict[str, Packet]
    _show_error: Callable[[str], None]
    _packet_summary: Callable[[Packet], str]
    _update_command_state: Callable[[], None]
    _apply_gateway_command_transition: Callable[[Any], None]
    status_text: Any
    def _initialize_gateway_diagnostics(self) -> None:
        self.click_location_model = ClickLocationModel()
        self.wake_monitor = WakeTrainMonitor()
        self.wake_attempt_adapter = PendingWakeAttemptAdapter()
        self.command_timeline_model = CommandTimelineModel()
        self.command_request_tracker = GatewayCommandRequestTracker()
        self.command_orchestrator = GatewayCommandOrchestrator(
            self.command_request_tracker
        )
        self.assignment_replay_barrier = GatewayAssignmentReplayBarrier()
        self.topology_model = TopologyBaselineModel(
            Path.home() / ".config" / "imec2-gateway-gui" / "anchor-baseline.json"
        )
        self._wake_by_packet_key: dict[tuple[object, ...], Any] = {}
        self._wake_row_iids: dict[tuple[object, ...], str] = {}

    def _build_gateway_diagnostic_tabs(self, notebook: ttk.Notebook) -> None:
        self.activity_notebook = notebook
        notebook.bind("<<NotebookTabChanged>>", self._diagnostic_tab_changed, add=True)
        click_tab = ttk.Frame(notebook, style="Panel.TFrame")
        mesh_tab = ttk.Frame(notebook, style="Panel.TFrame")
        notebook.add(click_tab, text="Click Location")
        notebook.add(mesh_tab, text="Mesh Commands")
        self.click_location_tab = click_tab
        self.mesh_commands_tab = mesh_tab
        self.click_diagnostics_view = ClickDiagnosticsView(click_tab)
        self.click_diagnostics_view.pack(fill="both", expand=True)
        self.mesh_diagnostics_view = MeshDiagnosticsView(mesh_tab, accept_baseline=self._accept_topology_baseline)
        self.mesh_diagnostics_view.pack(fill="both", expand=True)
        if self.topology_model.load_error:
            self.mesh_diagnostics_view.topology_var.set(f"[?] Baseline load failed: {self.topology_model.load_error}")
        self.click_diagnostics_view.show(self.click_location_model.state, {})

    def _diagnostic_tab_changed(self, _event: tk.Event[Any]) -> None:
        if not hasattr(self, "click_location_tab"):
            return
        selected = self.activity_notebook.nametowidget(self.activity_notebook.select())
        if selected not in (self.click_location_tab, self.mesh_commands_tab):
            return
        split = cast(ttk.Panedwindow, self.activity_notebook.master)
        self.root.after_idle(lambda: split.sashpos(0, max(360, int(split.winfo_height() * 0.68))))

    def _observe_diagnostic_packet(
        self, packet: Packet, *, received_at: float | None = None
    ) -> None:
        if packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
            try:
                event = decode_gateway_command_event(packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES))
            except CommandTelemetryDecodeError as exc:
                self._show_error(f"Malformed gateway command telemetry: {exc}")
                return
            self.command_timeline_model.observe(event)
            observe_progress = getattr(self, "_observe_operation_progress", None)
            if callable(observe_progress):
                observe_progress(event)
            self._apply_gateway_command_transition(
                self.command_orchestrator.observe_event(
                    event,
                    received_at=received_at,
                    target_dispatch_allowed=not (
                        self.command_orchestrator.plan is not None
                        and self.assignment_replay_barrier.blocks(
                            self.command_orchestrator.plan.target.command_id
                        )
                    ),
                )
            )
            self.mesh_diagnostics_view.show_timeline(self.command_timeline_model)
            comparison = self.topology_model.observe(event)
            anchors = self.command_timeline_model.enumerated_anchors.get(
                event.correlation_key, {})
            if comparison is not None or anchors or event.command_kind == 1:
                self.mesh_diagnostics_view.show_topology(comparison, anchors)
            return
        if packet.msg_type == MSG_COMMAND_RESULT:
            command_id = packet.value(TLV_COMMAND_ID)
            status = packet.value(TLV_COMMAND_STATUS)
            if isinstance(command_id, int) and isinstance(status, int):
                self._apply_gateway_command_transition(
                    self.command_orchestrator.observe_command_result(
                        command_id=command_id,
                        host_session_id=packet.session_id,
                        host_sequence=packet.seq,
                        command_status=status,
                        received_at=received_at,
                    )
                )
        if packet.msg_type != MSG_CLICK_REPORT:
            return
        wake = self._wake_evidence(packet)
        diagnostic = None
        for key, update in self.wake_monitor.observe(wake):
            self._wake_by_packet_key[key] = update
            self._refresh_wake_row(key)
            if key == wake.key:
                diagnostic = update
        state = self.click_location_model.observe(packet, diagnostic)
        if state is not None:
            self.click_diagnostics_view.show(state, self.click_location_model.positions_m)

    def _expire_gateway_command(self) -> None:
        transition = self.command_orchestrator.expire()
        if transition.matched:
            self._apply_gateway_command_transition(transition)
            if transition.phase != "preflight":
                self.status_text.set(
                    "Gateway command timed out; controls are available again"
                )

    def _wake_evidence(self, packet: Packet) -> WakeEvidence:
        event_seq = packet.value(TLV_EVENT_SEQ)
        clicker = packet.value(TLV_CLICKER_ID)
        anchor = packet.value(TLV_ANCHOR_ID)
        identity_complete = isinstance(event_seq, int) and isinstance(clicker, int)
        click_key = (packet.session_id, event_seq, clicker) if identity_complete else None
        event_time = time.monotonic() * 1000.0 - packet.age_ms if packet.age_ms >= 0 else None
        key = (packet.session_id, event_seq, clicker, anchor, packet.seq)
        click_id = f"{anchor_label(clicker)}/event-{event_seq}" if isinstance(clicker, int) else "unknown"
        return WakeEvidence(
            key, packet.session_id, click_key, click_id,
            self.wake_attempt_adapter.attempt(packet), event_time,
        )

    def _configure_diagnostic_packet_tags(self) -> None:
        self.packet_tree.tag_configure("wake_normal", background="#e4f2ee")
        self.packet_tree.tag_configure("wake_late", background="#fbe9e8")
        self.packet_tree.tag_configure("wake_collision", background="#fff0d6")
        self.packet_tree.tag_configure("wake_unknown", background="#edf0f2")

    def _diagnostic_packet_tags(self, packet: Packet) -> tuple[str, ...]:
        diagnostic = self._wake_by_packet_key.get(self._wake_evidence(packet).key)
        if diagnostic is None:
            return ()
        suffix = {"unexplained_late": "late", "collision_explained": "collision"}.get(
            diagnostic.classification, diagnostic.classification
        )
        return (f"wake_{suffix}",)

    def _diagnostic_packet_label(self, packet: Packet) -> str:
        diagnostic = self._wake_by_packet_key.get(self._wake_evidence(packet).key)
        return f"[{diagnostic.marker}]" if diagnostic is not None else "[?]"

    def _register_diagnostic_packet_row(self, packet: Packet, iid: str) -> None:
        if packet.msg_type == MSG_CLICK_REPORT:
            self._wake_row_iids[self._wake_evidence(packet).key] = iid

    def _forget_diagnostic_packet_row(self, packet: Packet) -> None:
        if packet.msg_type == MSG_CLICK_REPORT:
            self._wake_row_iids.pop(self._wake_evidence(packet).key, None)

    def _refresh_wake_row(self, key: tuple[object, ...]) -> None:
        iid = self._wake_row_iids.get(key)
        if not iid or not hasattr(self, "packet_tree") or not self.packet_tree.exists(iid):
            return
        packet = self.packet_by_iid.get(iid)
        if packet is None:
            return
        values = list(self.packet_tree.item(iid, "values"))
        values[5] = self._packet_summary(packet)
        self.packet_tree.item(iid, values=values, tags=self._diagnostic_packet_tags(packet))

    def _handle_diagnostic_event(self, event: dict[str, Any]) -> bool:
        return False

    def _accept_topology_baseline(self) -> None:
        try:
            self.topology_model.accept_latest()
        except ValueError as exc:
            messagebox.showwarning("Baseline unchanged", str(exc), parent=self.root)
            return
        assert self.topology_model.latest is not None
        key = self.topology_model.current_key
        anchors = self.command_timeline_model.enumerated_anchors.get(key, {}) if key else {}
        self.mesh_diagnostics_view.show_topology(self.topology_model.latest, anchors)
