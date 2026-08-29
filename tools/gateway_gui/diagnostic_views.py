"""Operational Tk views for geometry, click, and mesh diagnostics."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import Callable, Iterable

from .diagnostic_models import (
    ClickDiagnosticState, CommandTimelineModel, TopologyComparison,
    COLLISION_WINDOW_MS, WAKE_COLLISION, WAKE_LATE, WAKE_NORMAL, anchor_label,
    command_run_status, command_step_sentence,
)
from .command_telemetry import GatewayCommandEvent
from .command_telemetry import GATEWAY_COMMAND_KIND_NAMES, GATEWAY_COMMAND_REASON_NAMES, GATEWAY_COMMAND_STAGE_NAMES
from .survey_view import (
    HELD_TRANSLATION_INTERVAL_MS,
    LayoutRegistration,
    LayoutRegistrationControls,
    held_translation_delta,
)


ACCENT = "#126b5b"
AMBER = "#a56200"
ERROR = "#a72b2b"
MUTED = "#667079"
BLUE = "#315c9b"


class ClickDiagnosticsView(ttk.Frame):
    def __init__(
        self,
        parent: tk.Misc,
        *,
        on_translate: Callable[[float, float], None] = lambda _x, _y: None,
        on_scale: Callable[[float], None] = lambda _factor: None,
        on_reset: Callable[[], None] = lambda: None,
    ) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self._on_translate = on_translate
        self._on_scale = on_scale
        self._on_reset = on_reset
        self._fullscreen_window: tk.Toplevel | None = None
        self._fullscreen_canvas: tk.Canvas | None = None
        self._fullscreen_registration_controls: LayoutRegistrationControls | None = None
        self._held_move_keys: set[str] = set()
        self._held_move_after_id: str | None = None
        self._registration_scale = 1.0
        self._registration_translate_x_m = 0.0
        self._registration_translate_y_m = 0.0
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)
        bar = ttk.Frame(self, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.columnconfigure(1, weight=1)
        self.identity_var = tk.StringVar(value="No click event")
        self.fit_var = tk.StringVar(value="Waiting for solved geometry")
        self.wake_var = tk.StringVar(value="[?] Detection attempt unavailable")
        ttk.Label(bar, textvariable=self.identity_var, style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Button(
            bar,
            text="Fullscreen",
            command=self._open_fullscreen,
        ).grid(row=0, column=2, sticky="e")
        ttk.Label(bar, textvariable=self.fit_var, style="PanelMuted.TLabel").grid(row=1, column=0, columnspan=3, sticky="w", pady=(3, 0))
        ttk.Label(bar, textvariable=self.wake_var, wraplength=650, justify="left").grid(
            row=2, column=0, columnspan=3, sticky="w", pady=(3, 0)
        )
        self.registration_controls = LayoutRegistrationControls(
            self,
            on_translate=on_translate,
            on_scale=on_scale,
            on_reset=on_reset,
        )
        self.registration_controls.grid(row=1, column=0, sticky="ew", pady=(0, 6))
        self.canvas = tk.Canvas(self, background="#ffffff", highlightthickness=1, highlightbackground="#d5dbdd")
        self.canvas.grid(row=2, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self.redraw())
        self.diagnostic_state: ClickDiagnosticState | None = None
        self.positions: dict[str, tuple[float, float]] = {}
        self.reference_positions: dict[str, tuple[float, float]] = {}
        self.connections: frozenset[tuple[str, str]] = frozenset()

    def show(self, state: ClickDiagnosticState, positions: dict[str, tuple[float, float]]) -> None:
        self.diagnostic_state = state
        self.positions = dict(positions)
        self.registration_controls.set_enabled(bool(positions))
        if self._fullscreen_registration_controls is not None:
            self._fullscreen_registration_controls.set_enabled(bool(positions))
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

    def show_registration(self, registration: LayoutRegistration) -> None:
        self.reference_positions = dict(registration.reference_positions_m)
        self._registration_scale = registration.scale
        self._registration_translate_x_m = registration.translate_x_m
        self._registration_translate_y_m = registration.translate_y_m
        self.registration_controls.show_registration(
            registration.scale,
            registration.translate_x_m,
            registration.translate_y_m,
        )
        if self._fullscreen_registration_controls is not None:
            self._fullscreen_registration_controls.show_registration(
                registration.scale,
                registration.translate_x_m,
                registration.translate_y_m,
            )
        self.redraw()

    def show_connections(self, connections: frozenset[tuple[str, str]]) -> None:
        self.connections = connections
        self.redraw()

    def redraw(self) -> None:
        self._draw_click_graph(self.canvas)
        window = self._fullscreen_window
        canvas = self._fullscreen_canvas
        if (
            window is not None
            and canvas is not None
            and window.winfo_exists()
        ):
            self._draw_click_graph(canvas)

    def _draw_click_graph(self, canvas: tk.Canvas) -> None:
        canvas.delete("all")
        if not self.positions:
            canvas.create_text(max(canvas.winfo_width(), 400) / 2, max(canvas.winfo_height(), 260) / 2, text="No solved geometry", fill=MUTED)
            return
        points = list((self.reference_positions or self.positions).values())
        points.append((0.0, 0.0))
        project = _projector(points, canvas.winfo_width(), canvas.winfo_height())
        origin_x, origin_y = project(0.0, 0.0)
        canvas.create_line(
            0,
            origin_y,
            canvas.winfo_width(),
            origin_y,
            fill="#e3e7e9",
            dash=(3, 4),
        )
        canvas.create_line(
            origin_x,
            0,
            origin_x,
            canvas.winfo_height(),
            fill="#e3e7e9",
            dash=(3, 4),
        )
        state = self.diagnostic_state
        for anchor_a, anchor_b in sorted(self.connections):
            if anchor_a not in self.positions or anchor_b not in self.positions:
                continue
            ax, ay = project(*self.positions[anchor_a])
            bx, by = project(*self.positions[anchor_b])
            canvas.create_line(
                ax,
                ay,
                bx,
                by,
                fill="#c5ced1",
                width=1,
                dash=(2, 5),
            )
        if state:
            for anchor_id, radius in state.ranges_m.items():
                if anchor_id not in self.positions:
                    continue
                x, y = project(*self.positions[anchor_id])
                scale = project.scale
                canvas.create_oval(x - radius * scale, y - radius * scale, x + radius * scale, y + radius * scale, outline="#7aa7a0", dash=(4, 4))
        for anchor_id, point in sorted(self.positions.items()):
            x, y = project(*point)
            canvas.create_oval(x - 6, y - 6, x + 6, y + 6, fill=BLUE, outline="")
            canvas.create_text(x + 9, y - 7, text=anchor_id, anchor="sw", fill="#20262b")
        if state and state.result:
            x, y = project(state.result.x_m, state.result.y_m)
            classification = state.wake.classification if state.wake else "unknown"
            color, marker = {WAKE_NORMAL: (ACCENT, "OK"), WAKE_LATE: (ERROR, "!"), WAKE_COLLISION: (AMBER, "C")}.get(classification, (MUTED, "?"))
            canvas.create_oval(x - 11, y - 11, x + 11, y + 11, fill=color, outline="#20262b")
            canvas.create_text(x, y, text=marker, fill="#ffffff", font=("TkDefaultFont", 8, "bold"))

    def _open_fullscreen(self) -> None:
        window = self._fullscreen_window
        if window is not None and window.winfo_exists():
            window.lift()
            window.focus_force()
            return
        window = tk.Toplevel(self)
        self._fullscreen_window = window
        window.title("IMEC2 Click Location — Fullscreen")
        window.configure(background="#ffffff")
        window.columnconfigure(0, weight=1)
        window.rowconfigure(2, weight=1)

        header = ttk.Frame(window, style="Panel.TFrame", padding=(8, 6))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        ttk.Label(
            header,
            textvariable=self.identity_var,
            style="Section.TLabel",
        ).grid(row=0, column=0, sticky="w")
        ttk.Label(
            header,
            textvariable=self.fit_var,
            style="PanelMuted.TLabel",
        ).grid(row=1, column=0, sticky="w", pady=(2, 0))
        ttk.Label(
            header,
            text="Hold WASD to move · Esc or F11 exits",
            style="PanelMuted.TLabel",
        ).grid(row=0, column=1, rowspan=2, padx=(8, 8))
        ttk.Button(
            header,
            text="Exit fullscreen",
            command=self._close_fullscreen,
        ).grid(row=0, column=2, rowspan=2)

        self._fullscreen_registration_controls = LayoutRegistrationControls(
            window,
            on_translate=self._on_translate,
            on_scale=self._on_scale,
            on_reset=self._on_reset,
        )
        self._fullscreen_registration_controls.grid(
            row=1,
            column=0,
            sticky="ew",
            padx=8,
            pady=(2, 4),
        )
        self._fullscreen_registration_controls.set_enabled(bool(self.positions))
        self._fullscreen_registration_controls.show_registration(
            self._registration_scale,
            self._registration_translate_x_m,
            self._registration_translate_y_m,
        )
        canvas = tk.Canvas(window, background="#ffffff", highlightthickness=0)
        self._fullscreen_canvas = canvas
        canvas.grid(row=2, column=0, sticky="nsew")
        canvas.bind("<Configure>", lambda _event: self.redraw())
        window.bind("<KeyPress>", self._fullscreen_key_pressed)
        window.bind("<KeyRelease>", self._fullscreen_key_released)
        window.bind("<Escape>", lambda _event: self._close_fullscreen())
        window.bind("<F11>", lambda _event: self._close_fullscreen())
        window.protocol("WM_DELETE_WINDOW", self._close_fullscreen)
        try:
            window.attributes("-fullscreen", True)
        except tk.TclError:
            window.state("zoomed")
        window.focus_force()
        self.redraw()

    def _fullscreen_key_pressed(self, event: tk.Event[tk.Misc]) -> str | None:
        key = event.keysym.lower()
        if key not in {"w", "a", "s", "d"}:
            return None
        self._held_move_keys.add(key)
        if self._held_move_after_id is None:
            self._run_held_movement()
        return "break"

    def _fullscreen_key_released(self, event: tk.Event[tk.Misc]) -> str | None:
        key = event.keysym.lower()
        if key not in {"w", "a", "s", "d"}:
            return None
        self._held_move_keys.discard(key)
        return "break"

    def _run_held_movement(self) -> None:
        self._held_move_after_id = None
        window = self._fullscreen_window
        if window is None or not window.winfo_exists():
            self._held_move_keys.clear()
            return
        delta_x_m, delta_y_m = held_translation_delta(self._held_move_keys)
        if delta_x_m or delta_y_m:
            self._on_translate(delta_x_m, delta_y_m)
        if self._held_move_keys:
            self._held_move_after_id = window.after(
                HELD_TRANSLATION_INTERVAL_MS,
                self._run_held_movement,
            )

    def _close_fullscreen(self) -> None:
        window = self._fullscreen_window
        after_id = self._held_move_after_id
        self._held_move_after_id = None
        self._held_move_keys.clear()
        if window is not None and after_id is not None and window.winfo_exists():
            window.after_cancel(after_id)
        self._fullscreen_window = None
        self._fullscreen_canvas = None
        self._fullscreen_registration_controls = None
        if window is not None and window.winfo_exists():
            window.destroy()


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
                detail.hop_count if detail is not None and detail.hop_count != 0 else "-",
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
