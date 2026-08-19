"""Operational Tk views for geometry, click, and mesh diagnostics."""

from __future__ import annotations

import math
from functools import partial
import tkinter as tk
from tkinter import ttk
from typing import Callable, Iterable

from .anchor_geometry import AnchorLayoutResult
from .diagnostic_models import (
    ClickDiagnosticState, CommandTimelineModel, SurveyGeometryModel, TopologyComparison,
    COLLISION_WINDOW_MS, WAKE_COLLISION, WAKE_LATE, WAKE_NORMAL, anchor_label,
    command_run_status, command_step_sentence,
)
from .command_telemetry import GatewayCommandEvent
from .command_telemetry import GATEWAY_COMMAND_KIND_NAMES, GATEWAY_COMMAND_REASON_NAMES, GATEWAY_COMMAND_STAGE_NAMES


ACCENT = "#126b5b"
AMBER = "#a56200"
ERROR = "#a72b2b"
MUTED = "#667079"
BLUE = "#315c9b"


class AnchorGeometryView(ttk.Frame):
    def __init__(
        self,
        parent: tk.Misc,
        *,
        solve: Callable[[], None],
        transform: Callable[[str], None],
    ) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=4)
        self.rowconfigure(2, weight=2)
        bar = ttk.Frame(self, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.columnconfigure(5, weight=1)
        self.solver_var = tk.StringVar(value="Visibility branching tuned")
        ttk.Combobox(
            bar, textvariable=self.solver_var,
            values=("Visibility branching tuned", "Spring energy"), state="readonly", width=25,
        ).grid(row=0, column=0, padx=(0, 6))
        self.solve_button = ttk.Button(bar, text="Solve", style="Primary.TButton", command=solve)
        self.solve_button.grid(row=0, column=1, padx=(0, 12))
        self.status_var = tk.StringVar(value="Waiting for successful survey pair records")
        for control_column, (button_text, action) in enumerate((("Mirror", "mirror"), ("-90°", "left"), ("+90°", "right")), 2):
            ttk.Button(bar, text=button_text, style="Tool.TButton", command=partial(transform, action)).grid(row=0, column=control_column, padx=(3, 0))
        ttk.Label(bar, textvariable=self.status_var, style="PanelMuted.TLabel").grid(row=1, column=0, columnspan=6, sticky="w", pady=(4, 0))

        self.canvas = tk.Canvas(self, background="#ffffff", highlightthickness=1, highlightbackground="#d5dbdd")
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self.redraw())
        self.model: SurveyGeometryModel | None = None
        self.result: AnchorLayoutResult | None = None

        pair_frame = ttk.Frame(self, style="Panel.TFrame")
        pair_frame.grid(row=2, column=0, sticky="nsew", pady=(6, 0))
        pair_frame.columnconfigure(0, weight=1)
        pair_frame.rowconfigure(0, weight=1)
        self.pair_tree = ttk.Treeview(
            pair_frame, columns=("a", "b", "distance", "state", "source"), show="headings", height=6,
        )
        for pair_column, title, width in (
            ("a", "Anchor A", 155), ("b", "Anchor B", 155), ("distance", "Distance", 90),
            ("state", "Constraint", 95), ("source", "Source", 130),
        ):
            self.pair_tree.heading(pair_column, text=title)
            self.pair_tree.column(pair_column, width=width, minwidth=70, stretch=pair_column == "source")
        self.pair_tree.grid(row=0, column=0, sticky="nsew")
        pair_scroll = ttk.Scrollbar(pair_frame, orient="vertical", command=self.pair_tree.yview)
        pair_scroll.grid(row=0, column=1, sticky="ns")
        self.pair_tree.configure(yscrollcommand=pair_scroll.set)

    def show_model(self, model: SurveyGeometryModel, result: AnchorLayoutResult | None = None) -> None:
        self.model = model
        if result is not None:
            self.result = result
        elif not model.positions_m:
            self.result = None
            _ready, reason = model.solve_readiness()
            self.status_var.set(reason)
        self.pair_tree.delete(*self.pair_tree.get_children())
        all_pairs = sorted(set(model.pairs) | model.failures)
        for pair in all_pairs:
            known = model.pairs.get(pair)
            state = "known" if known else "missing" if pair in model.missing_pairs else "failed"
            self.pair_tree.insert("", "end", values=(pair[0], pair[1], f"{known.distance_m:.3f} m" if known else "-", state, known.source if known else "survey"))
        self.redraw()

    def redraw(self) -> None:
        self.canvas.delete("all")
        positions = self.model.positions_m if self.model else {}
        if not positions:
            self.canvas.create_text(max(self.canvas.winfo_width(), 400) / 2, max(self.canvas.winfo_height(), 260) / 2, text="No solved geometry", fill=MUTED)
            return
        project = _projector(positions.values(), self.canvas.winfo_width(), self.canvas.winfo_height())
        if self.model:
            for pair, constraint in self.model.pairs.items():
                if pair[0] in positions and pair[1] in positions:
                    self.canvas.create_line(*project(*positions[pair[0]]), *project(*positions[pair[1]]), fill="#aab5b9", width=1)
                    mid = tuple((a + b) / 2 for a, b in zip(project(*positions[pair[0]]), project(*positions[pair[1]])))
                    self.canvas.create_text(*mid, text=f"{constraint.distance_m:.2f}", fill=MUTED)
        for anchor_id, point in sorted(positions.items()):
            x, y = project(*point)
            self.canvas.create_oval(x - 7, y - 7, x + 7, y + 7, fill=ACCENT, outline="")
            self.canvas.create_text(x + 10, y - 8, text=anchor_id, anchor="sw", fill="#20262b")


class ClickDiagnosticsView(ttk.Frame):
    def __init__(self, parent: tk.Misc) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)
        bar = ttk.Frame(self, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.columnconfigure(1, weight=1)
        self.identity_var = tk.StringVar(value="No click event")
        self.fit_var = tk.StringVar(value="Waiting for solved geometry")
        self.wake_var = tk.StringVar(value="[?] Detection attempt unavailable")
        ttk.Label(bar, textvariable=self.identity_var, style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(bar, textvariable=self.fit_var, style="PanelMuted.TLabel").grid(row=1, column=0, columnspan=2, sticky="w", pady=(3, 0))
        ttk.Label(bar, textvariable=self.wake_var, wraplength=650, justify="left").grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(3, 0)
        )
        self.canvas = tk.Canvas(self, background="#ffffff", highlightthickness=1, highlightbackground="#d5dbdd")
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self.redraw())
        self.diagnostic_state: ClickDiagnosticState | None = None
        self.positions: dict[str, tuple[float, float]] = {}

    def show(self, state: ClickDiagnosticState, positions: dict[str, tuple[float, float]]) -> None:
        self.diagnostic_state = state
        self.positions = dict(positions)
        if state.identity:
            self.identity_var.set(f"Session {state.identity[0]}  •  Event {state.identity[1]}  •  Clicker {anchor_label(state.identity[2])}")
        else:
            self.identity_var.set("No click event")
        if state.result:
            maximum = max((abs(value) for value in state.result.range_residuals_m.values()), default=0.0)
            self.fit_var.set(f"x {state.result.x_m:.3f} m   y {state.result.y_m:.3f} m   RMSE {state.result.rmse_m:.3f} m   Max {maximum:.3f} m")
        else:
            self.fit_var.set(f"{state.status.replace('_', ' ').title()}  •  {state.message}")
        wake = state.wake
        if wake:
            attempt = "-" if wake.attempt is None else str(wake.attempt)
            nearby = ", ".join(wake.nearby_click_ids) or "-"
            timestamp = "-" if wake.event_time_ms is None else f"{wake.event_time_ms:.1f} ms"
            self.wake_var.set(
                f"[{wake.marker}] {wake.classification.replace('_', ' ').title()}   Attempt {attempt}   "
                f"Event {timestamp}   Nearby {nearby}   Window {COLLISION_WINDOW_MS} ms   {wake.reason}"
            )
        self.redraw()

    def redraw(self) -> None:
        self.canvas.delete("all")
        if not self.positions:
            self.canvas.create_text(max(self.canvas.winfo_width(), 400) / 2, max(self.canvas.winfo_height(), 260) / 2, text="No solved geometry", fill=MUTED)
            return
        points = list(self.positions.values())
        if self.diagnostic_state and self.diagnostic_state.result:
            points.append((self.diagnostic_state.result.x_m, self.diagnostic_state.result.y_m))
        project = _projector(points, self.canvas.winfo_width(), self.canvas.winfo_height())
        state = self.diagnostic_state
        if state:
            for anchor_id, radius in state.ranges_m.items():
                if anchor_id not in self.positions:
                    continue
                x, y = project(*self.positions[anchor_id])
                scale = project.scale
                self.canvas.create_oval(x - radius * scale, y - radius * scale, x + radius * scale, y + radius * scale, outline="#7aa7a0", dash=(4, 4))
        for anchor_id, point in sorted(self.positions.items()):
            x, y = project(*point)
            self.canvas.create_oval(x - 6, y - 6, x + 6, y + 6, fill=BLUE, outline="")
            self.canvas.create_text(x + 9, y - 7, text=anchor_id, anchor="sw", fill="#20262b")
        if state and state.result:
            x, y = project(state.result.x_m, state.result.y_m)
            classification = state.wake.classification if state.wake else "unknown"
            color, marker = {WAKE_NORMAL: (ACCENT, "OK"), WAKE_LATE: (ERROR, "!"), WAKE_COLLISION: (AMBER, "C")}.get(classification, (MUTED, "?"))
            self.canvas.create_oval(x - 11, y - 11, x + 11, y + 11, fill=color, outline="#20262b")
            self.canvas.create_text(x, y, text=marker, fill="#ffffff", font=("TkDefaultFont", 8, "bold"))


class MeshDiagnosticsView(ttk.Frame):
    RUN_COLUMNS = ("Started", "Command", "Status", "Anchors / Pairs", "Attempts", "Result")
    ANCHOR_COLUMNS = ("Anchor ID", "Hop to gateway", "Discovery slot", "Reply status", "Last seen", "Baseline comparison")

    def __init__(self, parent: tk.Misc, *, accept_baseline: Callable[[], None]) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=3)
        self.rowconfigure(1, weight=2)
        commands = ttk.Frame(self, style="Panel.TFrame")
        commands.grid(row=0, column=0, sticky="nsew")
        commands.columnconfigure(0, weight=1)
        commands.rowconfigure(1, weight=1)
        self.timeline_status_var = tk.StringVar(value="No command has been run yet.")
        ttk.Label(commands, textvariable=self.timeline_status_var, style="PanelMuted.TLabel").grid(row=0, column=0, sticky="w", pady=(0, 5))
        columns = ("started", "command", "status", "work", "attempts", "result")
        self.timeline = ttk.Treeview(commands, columns=columns, show="headings", height=6)
        for column, title, width in zip(columns, self.RUN_COLUMNS, (80, 150, 150, 125, 70, 345)):
            self.timeline.heading(column, text=title)
            self.timeline.column(column, width=width, minwidth=60, stretch=column == "result")
        self.timeline.grid(row=1, column=0, sticky="nsew")
        self.timeline.bind("<<TreeviewSelect>>", self._run_selected)
        self._run_events: dict[str, tuple[GatewayCommandEvent, ...]] = {}
        self.step_var = tk.StringVar(value="Select a command run to see its chronological steps.")
        ttk.Label(commands, textvariable=self.step_var, style="PanelMuted.TLabel", wraplength=900, justify="left").grid(row=2, column=0, sticky="ew", pady=(5, 0))

        topology = ttk.Frame(self, style="Panel.TFrame")
        topology.grid(row=1, column=0, sticky="nsew", pady=(7, 0))
        topology.columnconfigure(0, weight=1)
        topology.rowconfigure(2, weight=1)
        bar = ttk.Frame(topology, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 3))
        bar.columnconfigure(0, weight=1)
        self.topology_var = tk.StringVar(value="No complete anchor enumeration is available.")
        ttk.Label(bar, textvariable=self.topology_var, style="Section.TLabel").grid(row=0, column=0, sticky="w")
        self.accept_baseline_button = ttk.Button(bar, text="Accept baseline", style="Tool.TButton", command=accept_baseline, state="disabled")
        self.accept_baseline_button.grid(row=0, column=1, sticky="e")
        self.baseline_reason_var = tk.StringVar(value="Run anchor enumeration and wait for its terminal result.")
        ttk.Label(topology, textvariable=self.baseline_reason_var, style="PanelMuted.TLabel", wraplength=900, justify="left").grid(row=1, column=0, sticky="ew", pady=(0, 4))
        anchor_columns = ("anchor", "hop", "slot", "reply", "seen", "comparison")
        self.topology = ttk.Treeview(topology, columns=anchor_columns, show="headings", height=5)
        for column, title, width in zip(anchor_columns, self.ANCHOR_COLUMNS, (185, 105, 105, 120, 90, 150)):
            self.topology.heading(column, text=title)
            self.topology.column(column, width=width, minwidth=75, stretch=column in ("anchor", "comparison"))
        self.topology.grid(row=2, column=0, sticky="nsew")

    def show_timeline(self, model: CommandTimelineModel) -> None:
        self.timeline.delete(*self.timeline.get_children())
        self._run_events.clear()
        runs = model.runs()
        for index, (_key, events) in enumerate(runs):
            status, result = command_run_status(events)
            terminal = next((event for event in reversed(events) if event.terminal), events[-1])
            attempts = max((event.attempt for event in events), default=0)
            work = terminal.total_count or terminal.progress_count
            iid = f"run-{index}"
            self._run_events[iid] = events
            self.timeline.insert("", "end", iid=iid, values=(
                f"Event {events[0].event_sequence}", GATEWAY_COMMAND_KIND_NAMES[events[0].command_kind], status,
                work if work else "-", attempts if attempts else "Not started", result,
            ))
        if runs:
            status, result = command_run_status(runs[-1][1])
            self.timeline_status_var.set(f"{status}: {result}")

    def _run_selected(self, _event: tk.Event[tk.Misc]) -> None:
        selection = self.timeline.selection()
        if selection:
            events = self._run_events.get(selection[0], ())
            self.step_var.set("   ".join(
                f"{index + 1}. {command_step_sentence(event)}" for index, event in enumerate(events)
            ))

    def show_topology(self, result: TopologyComparison | None,
                      anchors: dict[int, GatewayCommandEvent] | None = None) -> None:
        anchors_dict = anchors or {}
        if result is None:
            if anchors_dict:
                summary = f"Enumerating anchors: {len(anchors_dict)} anchor(s) discovered..."
            else:
                summary = "No complete anchor enumeration is available."
            eligibility_reason = "Enumeration in progress; waiting for terminal result..."
            complete = False
            expected: set[int] = set()
            actual: set[int] = set(anchors_dict.keys())
        else:
            complete = result.complete
            if not result.complete:
                summary = result.eligibility_reason
            elif result.status == "no_baseline":
                summary = f"Enumeration complete: {len(result.actual)} anchor(s) found. No baseline has been accepted yet."
            elif result.status == "exact":
                summary = f"Topology unchanged: all {len(result.actual)} expected anchor(s) replied."
            else:
                summary = f"Topology changed: {len(result.added)} new anchor(s), {len(result.missing)} expected anchor(s) missing."
            eligibility_reason = result.eligibility_reason
            expected = set(result.expected)
            actual = set(result.actual) | set(anchors_dict.keys())

        self.topology_var.set(summary)
        self.baseline_reason_var.set(eligibility_reason)
        self.accept_baseline_button.configure(state="normal" if complete else "disabled")
        self.topology.delete(*self.topology.get_children())
        for anchor_id in sorted(expected | actual | set(anchors_dict.keys())):
            if result is None or not result.complete:
                comparison = "Discovered" if anchor_id in anchors_dict else "Pending"
            elif anchor_id in expected and anchor_id in actual:
                comparison = "Unchanged"
            elif anchor_id in expected:
                comparison = "Missing"
            else:
                comparison = "Added"
            detail = anchors_dict.get(anchor_id)
            self.topology.insert("", "end", values=(
                anchor_label(anchor_id),
                detail.hop_count if detail is not None and detail.hop_count != 0 else ("1" if detail is not None else "-"),
                detail.discovery_slot if detail is not None and detail.discovery_slot != 255 else "Pending",
                "Assigned" if detail is not None and detail.discovery_slot != 255 else "Replied" if anchor_id in actual else "No reply",
                f"Event {detail.event_sequence}" if detail is not None else "Current run",
                comparison,
            ))

def _projector(points: Iterable[tuple[float, float]], width: int, height: int):
    values = list(points)
    min_x, max_x = min(point[0] for point in values), max(point[0] for point in values)
    min_y, max_y = min(point[1] for point in values), max(point[1] for point in values)
    scale = min((max(width, 300) - 80) / max(max_x - min_x, 1.0), (max(height, 220) - 70) / max(max_y - min_y, 1.0))
    def project(x: float, y: float) -> tuple[float, float]:
        return 40 + (x - min_x) * scale, max(height, 220) - 35 - (y - min_y) * scale
    project.scale = scale  # type: ignore[attr-defined]
    return project
