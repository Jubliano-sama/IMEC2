"""Minimal GatewayGui wiring for focused diagnostic models and views."""

from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path
import time
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Any, Callable, cast

from .diagnostic_models import (
    ClickLocationModel, CommandTimelineModel, PendingWakeAttemptAdapter,
    TopologyBaselineModel, WakeEvidence, WakeTrainMonitor, anchor_label,
    refine_geometry, select_nearest_anchor_ranges, solve_geometry,
)
from .anchor_geometry import (
    MANUALLY_EDITED_LAYOUT_ALGORITHM,
    evaluate_anchor_layout,
)
from .diagnostic_views import ClickDiagnosticsView, MeshDiagnosticsView
from .command_telemetry import (
    CommandTelemetryDecodeError, GatewayCommandRequestTracker,
    decode_gateway_command_event,
)
from .anchor_geometry_connectivity import (
    CONNECTIVITY_INTERVAL_ALGORITHM,
    DEFAULT_NEIGHBOR_MAX_M,
    DEFAULT_NONNEIGHBOR_MIN_M,
)
from .anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
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
from .survey_runtime import SurveyCommandOwner, SurveyOperationModel
from .survey_view import LayoutRegistration, SurveyGeometryView
from .theme import AMBER_BG, ERROR_BG, INK, PANEL_ALT_BG, SUCCESS_BG


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
        self.survey_model = SurveyOperationModel()
        self.survey_command_owner = SurveyCommandOwner()
        self._survey_event_buffer: list[tuple[Packet, float | None]] = []
        self._geometry_executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="imec-survey-geometry"
        )
        self._geometry_future: Future[Any] | None = None
        self._geometry_resolve_pending = False
        self._geometry_job_serial = 0
        self.topology_model = TopologyBaselineModel(
            Path.home() / ".config" / "imec2-gateway-gui" / "anchor-baseline.json"
        )
        self._wake_by_packet_key: dict[tuple[object, ...], Any] = {}
        self._wake_row_iids: dict[tuple[object, ...], str] = {}

    def _build_gateway_diagnostic_tabs(self, notebook: ttk.Notebook) -> None:
        self.activity_notebook = notebook
        notebook.bind("<<NotebookTabChanged>>", self._diagnostic_tab_changed, add=True)
        click_tab = ttk.Frame(notebook, style="Panel.TFrame")
        survey_tab = ttk.Frame(notebook, style="Panel.TFrame")
        mesh_tab = ttk.Frame(notebook, style="Panel.TFrame")
        notebook.add(click_tab, text="Click Location")
        notebook.add(survey_tab, text="Survey & Geometry")
        notebook.add(mesh_tab, text="Mesh Commands")
        self.click_location_tab = click_tab
        self.survey_geometry_tab = survey_tab
        self.mesh_commands_tab = mesh_tab
        self.click_diagnostics_view = ClickDiagnosticsView(
            click_tab,
            on_translate=self._nudge_layout_translation,
            on_scale=self._nudge_layout_scale,
            on_reset=self._reset_layout_registration,
            on_mirror=self._mirror_layout_frame,
            on_click_selected=self._select_click_location,
            on_click_deleted=self._delete_click_location,
        )
        self.click_diagnostics_view.pack(fill="both", expand=True)
        self.survey_geometry_view = SurveyGeometryView(
            survey_tab,
            on_positions_changed=self._apply_survey_geometry_positions,
            on_layout_edited=self._apply_manual_anchor_layout,
            on_refine_requested=self._request_distance_only_refinement,
            on_solve_requested=self._request_survey_geometry_solve,
        )
        self.survey_geometry_view.pack(fill="both", expand=True)
        self.mesh_diagnostics_view = MeshDiagnosticsView(mesh_tab, accept_baseline=self._accept_topology_baseline)
        self.mesh_diagnostics_view.pack(fill="both", expand=True)
        if self.topology_model.load_error:
            self.mesh_diagnostics_view.topology_var.set(f"[?] Baseline load failed: {self.topology_model.load_error}")
        self.click_diagnostics_view.show(
            self.click_location_model.state,
            {},
            self.click_location_model.event_states,
        )
        self.survey_geometry_view.show_model(self.survey_model)

    def _apply_survey_geometry_positions(
        self,
        registration: LayoutRegistration,
    ) -> None:
        """Install the user-registered survey frame into click localization."""

        generation = self.survey_model.generation
        if generation is None or not registration.positions_m:
            return
        state = self.click_location_model.set_geometry(
            registration.positions_m,
            generation,
            range_scale=registration.scale,
        )
        click_view = getattr(self, "click_diagnostics_view", None)
        if click_view is not None:
            click_view.show_registration(registration)
            click_view.show_connections(self.survey_model.neighbor_pairs)
            click_view.show(
                state,
                registration.positions_m,
                self.click_location_model.event_states,
            )

    def _select_click_location(self, key: tuple[int, int, int]) -> None:
        state = self.click_location_model.select(key)
        if state is not None:
            self.click_diagnostics_view.show(
                state,
                self.click_location_model.positions_m,
                self.click_location_model.event_states,
            )

    def _delete_click_location(self, key: tuple[int, int, int]) -> None:
        state = self.click_location_model.delete(key)
        self.click_diagnostics_view.show(
            state,
            self.click_location_model.positions_m,
            self.click_location_model.event_states,
        )

    def _nudge_layout_translation(self, delta_x_m: float, delta_y_m: float) -> None:
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.nudge_translation(delta_x_m, delta_y_m)

    def _apply_manual_anchor_layout(
        self,
        positions_m: dict[str, tuple[float, float]],
    ) -> None:
        """Keep one dragged layout as the current RAM-only geometry."""

        model = self.survey_model
        view = getattr(self, "survey_geometry_view", None)
        if view is None or not model.geometry_solve_ready:
            return
        try:
            layout = evaluate_anchor_layout(
                view.effective_geometry_pairs,
                positions_m,
                algorithm=MANUALLY_EDITED_LAYOUT_ALGORITHM,
            )
            applied = model.apply_layout(model.geometry_revision, layout)
        except ValueError as exc:
            self.status_text.set(f"Manual anchor edit failed: {exc}")
            return
        if not applied:
            return
        view.accept_manual_layout(layout)
        view.show_model(model)
        self.status_text.set(
            "Manual anchor layout retained in GUI RAM; re-solve remains optional"
        )

    def _nudge_layout_scale(self, factor: float) -> None:
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.nudge_scale(factor)

    def _reset_layout_registration(self) -> None:
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.reset_transform()

    def _mirror_layout_frame(self) -> None:
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.mirror_layout_frame()

    def _diagnostic_tab_changed(self, _event: tk.Event[Any]) -> None:
        if not hasattr(self, "click_location_tab"):
            return
        selected = self.activity_notebook.nametowidget(self.activity_notebook.select())
        if selected not in (
            self.click_location_tab,
            self.survey_geometry_tab,
            self.mesh_commands_tab,
        ):
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
            self.click_diagnostics_view.show(
                state,
                self.click_location_model.positions_m,
                self.click_location_model.event_states,
            )

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
        self.packet_tree.tag_configure(
            "wake_normal", background=SUCCESS_BG, foreground=INK
        )
        self.packet_tree.tag_configure(
            "wake_late", background=ERROR_BG, foreground=INK
        )
        self.packet_tree.tag_configure(
            "wake_collision", background=AMBER_BG, foreground=INK
        )
        self.packet_tree.tag_configure(
            "wake_unknown", background=PANEL_ALT_BG, foreground=INK
        )

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
        if event.get("kind") != "survey_geometry_solved":
            return False
        job_serial = event.get("job_serial")
        if (
            isinstance(job_serial, int)
            and job_serial != getattr(self, "_geometry_job_serial", 0)
        ):
            return True
        self._geometry_future = None
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.set_geometry_job_pending(False)
        revision = event.get("revision")
        run_serial = event.get("run_serial")
        if (
            not isinstance(revision, int)
            or run_serial != self.survey_model.run_serial
        ):
            if self._geometry_resolve_pending:
                self._geometry_resolve_pending = False
                self._schedule_survey_geometry_solve()
            return True
        error = event.get("error")
        applied = False
        if isinstance(error, str):
            if self.survey_model.layout is None:
                self.survey_model.geometry_failed(revision, error)
            else:
                self.status_text.set(error)
        else:
            layout = event.get("layout")
            if layout is not None:
                try:
                    applied = self.survey_model.apply_layout(revision, layout)
                except ValueError as exc:
                    self.survey_model.geometry_failed(revision, str(exc))
        self._refresh_survey_view()
        if applied and view is not None and layout is not None:
            self._apply_survey_geometry_positions(view.registration)
            self.status_text.set(
                f"Geometry re-solve complete: {layout.algorithm}; "
                f"RMSE {layout.rmse_m:.3f} m, max residual "
                f"{layout.max_residual_m:.3f} m."
            )
            append_log = getattr(self, "_append_log", None)
            if callable(append_log) and hasattr(self, "log_text"):
                append_log("info", self.status_text.get())
        if self._geometry_resolve_pending:
            self._geometry_resolve_pending = False
            self._schedule_survey_geometry_solve()
        return True

    def _refresh_survey_view(self) -> None:
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.show_model(self.survey_model)

    def _show_survey_tab(self) -> None:
        notebook = getattr(self, "activity_notebook", None)
        tab = getattr(self, "survey_geometry_tab", None)
        if notebook is not None and tab is not None:
            notebook.select(tab)

    def _schedule_survey_geometry_solve(self) -> None:
        model = self.survey_model
        if (
            not model.geometry_solve_pending
        ):
            self._refresh_survey_view()
            return
        future = self._geometry_future
        if future is not None and not future.done():
            self._geometry_resolve_pending = True
            return
        revision = model.geometry_revision
        run_serial = model.run_serial
        view = getattr(self, "survey_geometry_view", None)
        solver = (
            view.solver_var.get()
            if view is not None
            else CONNECTIVITY_INTERVAL_ALGORITHM
        )
        seed = view.seed_var.get() if view is not None else "Auto (best of all)"
        neighbor_min_m = DEFAULT_NONNEIGHBOR_MIN_M
        neighbor_max_m = DEFAULT_NEIGHBOR_MAX_M
        nearest_per_anchor = 0
        if view is not None:
            try:
                nearest_per_anchor = view.nearest_anchor_count
            except ValueError as exc:
                view.geometry_var.set(str(exc))
                self.status_text.set(str(exc))
                return
        if view is not None and solver in (
            CONNECTIVITY_INTERVAL_ALGORITHM,
            VISIBILITY_BRANCHING_TUNED_ALGORITHM,
            VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
        ):
            try:
                neighbor_min_m, neighbor_max_m = view.neighbor_interval_m
            except ValueError as exc:
                view.geometry_var.set(str(exc))
                self.status_text.set(str(exc))
                return
        self._submit_geometry_solve(
            solver,
            seed,
            neighbor_min_m,
            neighbor_max_m,
            revision,
            run_serial,
            nearest_per_anchor,
        )

    def _request_survey_geometry_solve(
        self,
        solver: str,
        seed: str,
        neighbor_min_m: float,
        neighbor_max_m: float,
        nearest_per_anchor: int,
        current_positions_override: dict[str, tuple[float, float]] | None,
    ) -> None:
        model = self.survey_model
        if not model.geometry_solve_ready:
            return
        future = self._geometry_future
        if future is not None and not future.done():
            return
        self._submit_geometry_solve(
            solver,
            seed,
            neighbor_min_m,
            neighbor_max_m,
            model.geometry_revision,
            model.run_serial,
            nearest_per_anchor,
            current_positions_override,
        )

    def _effective_geometry_inputs(
        self,
    ) -> tuple[
        tuple[Any, ...],
        frozenset[tuple[str, str]],
        tuple[int, int],
    ]:
        """Return the current RAM-only connection edits for one solve."""

        model = self.survey_model
        view = getattr(self, "survey_geometry_view", None)
        if view is None:
            return model.geometry_pairs, model.neighbor_pairs, (0, 0)
        return (
            view.effective_geometry_pairs,
            model.neighbor_pairs - view.disabled_edge_keys,
            view.edge_edit_counts,
        )

    def _submit_geometry_solve(
        self,
        solver: str,
        seed: str,
        neighbor_min_m: float,
        neighbor_max_m: float,
        revision: int,
        run_serial: int,
        nearest_per_anchor: int = 0,
        current_positions_override: dict[str, tuple[float, float]] | None = None,
    ) -> None:
        model = self.survey_model
        current = (
            dict(current_positions_override)
            if current_positions_override is not None
            else model.layout.positions_m
            if model.layout is not None
            else None
        )
        effective_pairs, neighbor_pairs, edge_edit_counts = (
            self._effective_geometry_inputs()
        )
        all_pair_count = len(model.geometry_pairs)
        solve_pairs = select_nearest_anchor_ranges(
            effective_pairs,
            nearest_per_anchor,
        )
        future = self._geometry_executor.submit(
            solve_geometry,
            solve_pairs,
            solver=solver,
            seed=seed,
            neighbor_pairs=neighbor_pairs,
            nonneighbor_pairs=model.nonneighbor_pairs,
            current_positions_m=current,
            nonneighbor_min_m=neighbor_min_m,
            neighbor_max_m=neighbor_max_m,
        )
        self._geometry_job_serial = getattr(self, "_geometry_job_serial", 0) + 1
        job_serial = self._geometry_job_serial
        self._geometry_future = future
        view = getattr(self, "survey_geometry_view", None)
        interval = (
            f" (radio interval {neighbor_min_m:g}-{neighbor_max_m:g} m)"
            if solver in (
                CONNECTIVITY_INTERVAL_ALGORITHM,
                VISIBILITY_BRANCHING_TUNED_ALGORITHM,
                VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
            )
            else ""
        )
        job_label = (
            f"Solving with {solver}{interval} from "
            f"{len(solve_pairs)}/{all_pair_count} ranges..."
        )
        if view is not None:
            view.set_geometry_job_pending(
                True,
                job_label,
            )
        self.status_text.set(job_label)
        append_log = getattr(self, "_append_log", None)
        if callable(append_log) and hasattr(self, "log_text"):
            append_log("info", job_label)

        def completed(item: Future[Any]) -> None:
            try:
                layout = item.result()
            except Exception as exc:  # Delivered on Tk's event queue.
                self.events.put(
                    {
                        "kind": "survey_geometry_solved",
                        "run_serial": run_serial,
                        "revision": revision,
                        "job_serial": job_serial,
                        "error": f"Geometry solve failed: {exc}",
                    }
                )
            else:
                annotations: list[str] = []
                if nearest_per_anchor > 0:
                    annotations.append(
                        f"closest {nearest_per_anchor}/anchor, "
                        f"{len(solve_pairs)}/{all_pair_count} ranges"
                    )
                disabled_count, adjusted_count = edge_edit_counts
                if disabled_count or adjusted_count:
                    annotations.append(
                        f"GUI edges: {disabled_count} disabled, "
                        f"{adjusted_count} adjusted"
                    )
                if annotations:
                    layout = replace(
                        layout,
                        algorithm=f"{layout.algorithm}; {'; '.join(annotations)}",
                    )
                self.events.put(
                    {
                        "kind": "survey_geometry_solved",
                        "run_serial": run_serial,
                        "revision": revision,
                        "job_serial": job_serial,
                        "layout": layout,
                    }
                )

        future.add_done_callback(completed)
        self._refresh_survey_view()

    def _request_distance_only_refinement(self) -> None:
        model = self.survey_model
        if model.layout is None or not model.geometry_solve_ready:
            return
        future = self._geometry_future
        if future is not None and not future.done():
            return
        revision = model.geometry_revision
        run_serial = model.run_serial
        view = getattr(self, "survey_geometry_view", None)
        try:
            nearest_per_anchor = (
                view.nearest_anchor_count if view is not None else 0
            )
        except ValueError as exc:
            if view is not None:
                view.geometry_var.set(str(exc))
            self.status_text.set(str(exc))
            return
        effective_pairs, _neighbor_pairs, edge_edit_counts = (
            self._effective_geometry_inputs()
        )
        refine_pairs = select_nearest_anchor_ranges(
            effective_pairs,
            nearest_per_anchor,
        )
        all_pair_count = len(model.geometry_pairs)
        future = self._geometry_executor.submit(
            refine_geometry,
            refine_pairs,
            model.layout.positions_m,
        )
        self._geometry_job_serial = getattr(self, "_geometry_job_serial", 0) + 1
        job_serial = self._geometry_job_serial
        self._geometry_future = future
        if view is not None:
            view.set_geometry_job_pending(
                True,
                "Refining measured distances only from "
                f"{len(refine_pairs)}/{all_pair_count} ranges...",
            )

        def completed(item: Future[Any]) -> None:
            try:
                layout = item.result()
            except Exception as exc:
                payload = {
                    "kind": "survey_geometry_solved",
                    "run_serial": run_serial,
                    "revision": revision,
                    "job_serial": job_serial,
                    "error": f"Distance-only refinement failed: {exc}",
                }
            else:
                annotations: list[str] = []
                if nearest_per_anchor > 0:
                    annotations.append(
                        f"closest {nearest_per_anchor}/anchor, "
                        f"{len(refine_pairs)}/{all_pair_count} ranges"
                    )
                disabled_count, adjusted_count = edge_edit_counts
                if disabled_count or adjusted_count:
                    annotations.append(
                        f"GUI edges: {disabled_count} disabled, "
                        f"{adjusted_count} adjusted"
                    )
                if annotations:
                    layout = replace(
                        layout,
                        algorithm=f"{layout.algorithm}; {'; '.join(annotations)}",
                    )
                payload = {
                    "kind": "survey_geometry_solved",
                    "run_serial": run_serial,
                    "revision": revision,
                    "job_serial": job_serial,
                    "layout": layout,
                }
            self.events.put(payload)

        future.add_done_callback(completed)

    def _discard_geometry_job(self) -> None:
        """Invalidate one host solve without letting its callback clobber a newer job."""

        self._geometry_job_serial = getattr(self, "_geometry_job_serial", 0) + 1
        future = getattr(self, "_geometry_future", None)
        self._geometry_future = None
        self._geometry_resolve_pending = False
        if future is not None and not future.done():
            future.cancel()
        view = getattr(self, "survey_geometry_view", None)
        if view is not None:
            view.set_geometry_job_pending(False)

    def _shutdown_gateway_diagnostics(self) -> None:
        executor = getattr(self, "_geometry_executor", None)
        if executor is not None:
            executor.shutdown(wait=False, cancel_futures=True)

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
