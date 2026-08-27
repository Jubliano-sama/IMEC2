"""Tk view for one live enumeration, survey, and relative geometry run."""

from __future__ import annotations

import math
import tkinter as tk
from tkinter import ttk
from typing import Iterable

from .anchor_geometry import mirror_layout, rotate_layout
from .diagnostic_models import anchor_label
from .survey_runtime import SurveyOperationModel


INK = "#20262b"
MUTED = "#667079"
ACCENT = "#126b5b"
AMBER = "#a56200"
ERROR = "#a72b2b"
PANEL_BG = "#ffffff"


class SurveyGeometryView(ttk.Frame):
    """Render live command steps, pair results, and a solved 2D layout."""

    def __init__(self, parent: tk.Misc) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self.model: SurveyOperationModel | None = None
        self._layout_revision = -1
        self._display_positions: dict[str, tuple[float, float]] = {}

        self.columnconfigure(0, weight=2)
        self.columnconfigure(1, weight=3)
        self.rowconfigure(1, weight=3)
        self.rowconfigure(2, weight=2)

        summary = ttk.Frame(self, style="Panel.TFrame")
        summary.grid(
            row=0, column=0, columnspan=2, sticky="ew", pady=(0, 6)
        )
        summary.columnconfigure(0, weight=1)
        self.headline_var = tk.StringVar(
            value="Run a survey to enumerate anchors, range pairs, and solve geometry."
        )
        ttk.Label(
            summary,
            textvariable=self.headline_var,
            style="Section.TLabel",
            wraplength=900,
            justify="left",
        ).grid(row=0, column=0, sticky="w")
        self.identity_var = tk.StringVar(value="No active survey generation")
        ttk.Label(
            summary, textvariable=self.identity_var, style="PanelMuted.TLabel"
        ).grid(row=1, column=0, sticky="w", pady=(2, 0))
        self.progress = ttk.Progressbar(
            summary, orient="horizontal", mode="determinate", maximum=100.0
        )
        self.progress.grid(row=2, column=0, sticky="ew", pady=(5, 0))

        steps_frame = ttk.Frame(self, style="Panel.TFrame")
        steps_frame.grid(
            row=1, column=0, sticky="nsew", padx=(0, 6), pady=(0, 6)
        )
        steps_frame.columnconfigure(0, weight=1)
        steps_frame.rowconfigure(0, weight=1)
        columns = ("step", "state", "progress")
        self.steps = ttk.Treeview(
            steps_frame, columns=columns, show="headings", height=6
        )
        for column, title, width in (
            ("step", "Operation step", 170),
            ("state", "State", 75),
            ("progress", "Progress", 70),
        ):
            self.steps.heading(column, text=title)
            self.steps.column(
                column,
                width=width,
                minwidth=60,
                stretch=column == "step",
            )
        self.steps.tag_configure("done", foreground=ACCENT)
        self.steps.tag_configure("warning", foreground=AMBER)
        self.steps.tag_configure("failed", foreground=ERROR)
        self.steps.tag_configure("running", background="#e4f2ee")
        self.steps.grid(row=0, column=0, sticky="nsew")

        geometry = ttk.Frame(self, style="Panel.TFrame")
        geometry.grid(row=1, column=1, sticky="nsew", pady=(0, 6))
        geometry.columnconfigure(0, weight=1)
        geometry.rowconfigure(1, weight=1)
        toolbar = ttk.Frame(geometry, style="Panel.TFrame")
        toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        toolbar.columnconfigure(0, weight=1)
        self.geometry_var = tk.StringVar(value="Relative 2D geometry is waiting for survey ranges.")
        ttk.Label(
            toolbar,
            textvariable=self.geometry_var,
            style="PanelMuted.TLabel",
        ).grid(row=0, column=0, sticky="w")
        self.mirror_button = ttk.Button(
            toolbar, text="Mirror", command=self._mirror, state="disabled"
        )
        self.mirror_button.grid(row=0, column=1, padx=(4, 0))
        self.left_button = ttk.Button(
            toolbar, text="-90°", command=lambda: self._rotate(-90.0), state="disabled"
        )
        self.left_button.grid(row=0, column=2, padx=(4, 0))
        self.right_button = ttk.Button(
            toolbar, text="+90°", command=lambda: self._rotate(90.0), state="disabled"
        )
        self.right_button.grid(row=0, column=3, padx=(4, 0))
        self.canvas = tk.Canvas(
            geometry,
            background=PANEL_BG,
            highlightthickness=1,
            highlightbackground="#d5dbdd",
            height=300,
        )
        self.canvas.grid(row=1, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self._redraw())

        pairs_frame = ttk.Frame(self, style="Panel.TFrame")
        pairs_frame.grid(
            row=2, column=0, columnspan=2, sticky="nsew"
        )
        pairs_frame.columnconfigure(0, weight=1)
        pairs_frame.rowconfigure(0, weight=1)
        pair_columns = (
            "pair",
            "initiator",
            "responder",
            "wave",
            "samples",
            "distance",
            "state",
        )
        self.pairs = ttk.Treeview(
            pairs_frame, columns=pair_columns, show="headings", height=5
        )
        for column, title, width in (
            ("pair", "Pair", 55),
            ("initiator", "Initiator anchor", 190),
            ("responder", "Responder anchor", 190),
            ("wave", "Wave", 60),
            ("samples", "Successful samples", 125),
            ("distance", "Median distance", 125),
            ("state", "Result", 110),
        ):
            self.pairs.heading(column, text=title)
            self.pairs.column(
                column,
                width=width,
                minwidth=50,
                stretch=column in ("initiator", "responder"),
            )
        scrollbar = ttk.Scrollbar(
            pairs_frame, orient="vertical", command=self.pairs.yview
        )
        self.pairs.configure(yscrollcommand=scrollbar.set)
        self.pairs.tag_configure("usable", foreground=ACCENT)
        self.pairs.tag_configure("unusable", foreground=AMBER)
        self.pairs.grid(row=0, column=0, sticky="nsew")
        scrollbar.grid(row=0, column=1, sticky="ns")

    def show_model(self, model: SurveyOperationModel) -> None:
        self.model = model
        self.headline_var.set(model.headline)
        self.progress.configure(value=model.progress_percent)
        if model.generation is None:
            self.identity_var.set(
                f"Fresh enumeration: {len(model.slot_to_anchor)} anchor slot(s) mapped"
                if model.phase != "idle"
                else "No active survey generation"
            )
        else:
            assignment = model.assignment
            assert assignment is not None
            self.identity_var.set(
                f"Generation {model.generation} · assignment epoch "
                f"{assignment.assignment_epoch} · {len(model.slot_to_anchor)} anchors · "
                f"max hop {assignment.max_hop_count}"
            )

        self.steps.delete(*self.steps.get_children())
        for step in model.steps.values():
            progress = (
                f"{step.current}/{step.total}"
                if step.total > 0
                else "—"
            )
            self.steps.insert(
                "",
                "end",
                iid=f"survey-step-{step.key}",
                values=(step.title, step.state.title(), progress),
                tags=(step.state,),
            )

        if model.layout is not None and model.layout_revision != self._layout_revision:
            self._layout_revision = model.layout_revision
            self._display_positions = dict(model.layout.positions_m)
        elif model.layout is None:
            self._layout_revision = -1
            self._display_positions = {}
        has_layout = bool(self._display_positions)
        for button in (self.mirror_button, self.left_button, self.right_button):
            button.configure(state="normal" if has_layout else "disabled")
        if model.layout is not None:
            warnings = (
                f" · {len(model.layout.warnings)} warning(s)"
                if model.layout.warnings
                else ""
            )
            self.geometry_var.set(
                f"Relative 2D fit · RMSE {model.layout.rmse_m:.3f} m · "
                f"max residual {model.layout.max_residual_m:.3f} m{warnings}"
            )
        elif model.geometry_solve_pending:
            self.geometry_var.set(
                f"Solving relative 2D geometry from {len(model.geometry_pairs)} ranges..."
            )
        else:
            self.geometry_var.set(model.geometry_requirement)
        self._show_pairs(model)
        self._redraw()

    def _show_pairs(self, model: SurveyOperationModel) -> None:
        self.pairs.delete(*self.pairs.get_children())
        for pair_index, pair in enumerate(model.plan_pairs):
            initiator = model.slot_to_anchor.get(pair.initiator_slot)
            responder = model.slot_to_anchor.get(pair.responder_slot)
            result = model.results.get(pair_index)
            if result is None:
                samples = "—"
                distance = "—"
                state = "Pending"
                tag = ""
            else:
                samples = f"{result.success_count}/5"
                distance = (
                    f"{result.median_mm / 1000.0:.3f} m"
                    if result.median_mm is not None
                    else "No median"
                )
                state = "Usable" if result.usable else "Insufficient"
                tag = "usable" if result.usable else "unusable"
            self.pairs.insert(
                "",
                "end",
                values=(
                    pair_index,
                    anchor_label(initiator) if initiator is not None else f"slot {pair.initiator_slot}",
                    anchor_label(responder) if responder is not None else f"slot {pair.responder_slot}",
                    pair.wave_index,
                    samples,
                    distance,
                    state,
                ),
                tags=(tag,) if tag else (),
            )

    def _rotate(self, degrees: float) -> None:
        if not self._display_positions:
            return
        self._display_positions = rotate_layout(self._display_positions, degrees)
        self._redraw()

    def _mirror(self) -> None:
        if not self._display_positions:
            return
        self._display_positions = mirror_layout(self._display_positions, "x")
        self._redraw()

    def _redraw(self) -> None:
        canvas = self.canvas
        canvas.delete("all")
        model = self.model
        width = max(canvas.winfo_width(), 520)
        height = max(canvas.winfo_height(), 260)
        if model is None or not model.slot_to_anchor:
            canvas.create_text(
                width / 2,
                height / 2,
                text="Waiting for a fresh anchor enumeration",
                fill=MUTED,
            )
            return

        positions = self._display_positions or self._fallback_positions(model)
        if not positions:
            return
        project = _projector(positions.values(), width, height)
        for pair_index, pair in enumerate(model.plan_pairs):
            anchor_a = model.slot_to_anchor.get(pair.initiator_slot)
            anchor_b = model.slot_to_anchor.get(pair.responder_slot)
            if anchor_a is None or anchor_b is None:
                continue
            label_a = anchor_label(anchor_a)
            label_b = anchor_label(anchor_b)
            if label_a not in positions or label_b not in positions:
                continue
            ax, ay = project(*positions[label_a])
            bx, by = project(*positions[label_b])
            result = model.results.get(pair_index)
            usable = result is not None and result.usable
            color = ACCENT if usable else AMBER if result is not None else "#aeb7bb"
            dash = () if usable else (5, 4)
            canvas.create_line(ax, ay, bx, by, fill=color, width=2, dash=dash)
            edge_label = (
                f"{result.median_mm / 1000.0:.2f} m"
                if usable and result is not None and result.median_mm is not None
                else f"P{pair_index}"
            )
            canvas.create_text(
                (ax + bx) / 2,
                (ay + by) / 2 - 8,
                text=edge_label,
                fill=color,
                font=("TkDefaultFont", 8),
            )

        slot_by_anchor = {
            anchor_label(anchor_id): slot
            for slot, anchor_id in model.slot_to_anchor.items()
        }
        for label, position in positions.items():
            x, y = project(*position)
            slot = slot_by_anchor.get(label)
            canvas.create_oval(
                x - 8,
                y - 8,
                x + 8,
                y + 8,
                fill=ACCENT,
                outline="#083d34",
            )
            short = label[-4:]
            canvas.create_text(
                x,
                y - 13,
                text=f"S{slot} · …{short}" if slot is not None else f"…{short}",
                fill=INK,
                anchor="s",
                font=("TkDefaultFont", 9, "bold"),
            )
        if not self._display_positions:
            canvas.create_text(
                12,
                height - 10,
                text="Topology preview; coordinates appear once enough usable pair ranges arrive.",
                fill=MUTED,
                anchor="sw",
                font=("TkDefaultFont", 8),
            )

    @staticmethod
    def _fallback_positions(
        model: SurveyOperationModel,
    ) -> dict[str, tuple[float, float]]:
        slots = sorted(model.slot_to_anchor)
        count = len(slots)
        if count == 1:
            slot = slots[0]
            return {anchor_label(model.slot_to_anchor[slot]): (0.0, 0.0)}
        return {
            anchor_label(model.slot_to_anchor[slot]): (
                math.cos(2.0 * math.pi * index / count),
                math.sin(2.0 * math.pi * index / count),
            )
            for index, slot in enumerate(slots)
        }


def _projector(
    points: Iterable[tuple[float, float]], width: int, height: int
):
    values = list(points)
    min_x = min(point[0] for point in values)
    max_x = max(point[0] for point in values)
    min_y = min(point[1] for point in values)
    max_y = max(point[1] for point in values)
    if max_x - min_x < 1e-9:
        min_x -= 0.5
        max_x += 0.5
    if max_y - min_y < 1e-9:
        min_y -= 0.5
        max_y += 0.5
    span_x = max(max_x - min_x, 1.0)
    span_y = max(max_y - min_y, 1.0)
    scale = min((width - 100) / span_x, (height - 80) / span_y)
    offset_x = (width - span_x * scale) / 2.0
    offset_y = (height - span_y * scale) / 2.0

    def project(x: float, y: float) -> tuple[float, float]:
        return (
            offset_x + (x - min_x) * scale,
            height - offset_y - (y - min_y) * scale,
        )

    return project
