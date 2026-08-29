"""Tk view for one live enumeration, survey, and relative geometry run."""

from __future__ import annotations

import math
import tkinter as tk
from collections.abc import Callable
from dataclasses import dataclass
from tkinter import ttk
from typing import Iterable

from .anchor_geometry import (
    AnchorPairDistance,
    AnchorLayoutResult,
    MANUALLY_EDITED_LAYOUT_ALGORITHM,
    mirror_layout,
    rotate_layout,
)
from .anchor_geometry_connectivity import (
    CONNECTIVITY_INTERVAL_ALGORITHM,
    CONNECTIVITY_SEEDS,
    DEFAULT_NEIGHBOR_MAX_M,
    DEFAULT_NONNEIGHBOR_MIN_M,
)
from .anchor_geometry_visibility import (
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
)
from .anchor_geometry_seeds import SEED_CURRENT
from .diagnostic_models import anchor_label
from .survey_runtime import SurveyOperationModel


INK = "#20262b"
MUTED = "#667079"
ACCENT = "#126b5b"
AMBER = "#a56200"
ERROR = "#a72b2b"
PANEL_BG = "#ffffff"
TRANSLATION_STEP_M = 0.25
SCALE_STEP = 1.05
HELD_TRANSLATION_STEP_M = 0.10
HELD_TRANSLATION_INTERVAL_MS = 50
SOLVER_CHOICES = (
    CONNECTIVITY_INTERVAL_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
    "Spring energy",
)
RADIO_INTERVAL_SOLVERS = frozenset((
    CONNECTIVITY_INTERVAL_ALGORITHM,
    VISIBILITY_BRANCHING_TUNED_ALGORITHM,
    VISIBILITY_BRANCHING_NEIGHBOR_AWARE_ALGORITHM,
))


@dataclass(frozen=True)
class LayoutRegistration:
    reference_positions_m: dict[str, tuple[float, float]]
    positions_m: dict[str, tuple[float, float]]
    scale: float
    translate_x_m: float
    translate_y_m: float


@dataclass(frozen=True)
class CanvasProjection:
    min_x: float
    min_y: float
    scale: float
    offset_x: float
    offset_y: float
    height: float

    def project(self, x_m: float, y_m: float) -> tuple[float, float]:
        return (
            self.offset_x + (x_m - self.min_x) * self.scale,
            self.height - self.offset_y - (y_m - self.min_y) * self.scale,
        )

    def unproject(self, x_px: float, y_px: float) -> tuple[float, float]:
        return (
            self.min_x + (x_px - self.offset_x) / self.scale,
            self.min_y
            + (self.height - self.offset_y - y_px) / self.scale,
        )


class LayoutRegistrationControls(ttk.Frame):
    """Button-only controls shared by geometry and click-location views."""

    def __init__(
        self,
        parent: tk.Misc,
        *,
        on_translate: Callable[[float, float], None],
        on_scale: Callable[[float], None],
        on_reset: Callable[[], None],
    ) -> None:
        super().__init__(parent, style="Panel.TFrame")
        ttk.Label(self, text="Move frame").grid(row=0, column=0, padx=(0, 3))
        specs: tuple[tuple[str, Callable[[], None]], ...] = (
            ("← X", lambda: on_translate(-TRANSLATION_STEP_M, 0.0)),
            ("X →", lambda: on_translate(TRANSLATION_STEP_M, 0.0)),
            ("Y ↓", lambda: on_translate(0.0, -TRANSLATION_STEP_M)),
            ("Y ↑", lambda: on_translate(0.0, TRANSLATION_STEP_M)),
            ("Scale −", lambda: on_scale(1.0 / SCALE_STEP)),
            ("Scale +", lambda: on_scale(SCALE_STEP)),
            ("Reset", on_reset),
        )
        self.buttons: list[ttk.Button] = []
        for column, (label, command) in enumerate(specs, 1):
            button = ttk.Button(
                self,
                text=label,
                command=command,
                state="disabled",
            )
            button.grid(row=0, column=column, padx=(3, 0))
            self.buttons.append(button)
        self.status_var = tk.StringVar(
            value="Frame: scale 1.000, offset (0.000, 0.000) m"
        )
        ttk.Label(
            self,
            textvariable=self.status_var,
            style="PanelMuted.TLabel",
        ).grid(row=1, column=0, columnspan=8, sticky="w", pady=(2, 0))

    def set_enabled(self, enabled: bool) -> None:
        for button in self.buttons:
            button.configure(state="normal" if enabled else "disabled")

    def show_registration(
        self,
        scale: float,
        translate_x_m: float,
        translate_y_m: float,
    ) -> None:
        self.status_var.set(
            f"Frame: scale {scale:.3f}, "
            f"offset ({translate_x_m:.3f}, {translate_y_m:.3f}) m"
        )


def transform_layout(
    positions: dict[str, tuple[float, float]],
    *,
    scale: float,
    translate_x_m: float,
    translate_y_m: float,
) -> dict[str, tuple[float, float]]:
    """Apply one host-side registration transform to solved coordinates."""

    values = (scale, translate_x_m, translate_y_m)
    if not all(math.isfinite(value) for value in values):
        raise ValueError("layout transform values must be finite")
    if scale <= 0.0:
        raise ValueError("layout scale must be greater than zero")
    return {
        anchor_id: (
            scale * x_m + translate_x_m,
            scale * y_m + translate_y_m,
        )
        for anchor_id, (x_m, y_m) in positions.items()
    }


def inverse_transform_point(
    position_m: tuple[float, float],
    *,
    scale: float,
    translate_x_m: float,
    translate_y_m: float,
) -> tuple[float, float]:
    """Map one displayed point back into the editable solver frame."""

    if not math.isfinite(scale) or scale <= 0.0:
        raise ValueError("layout scale must be finite and greater than zero")
    x_m, y_m = position_m
    if not all(
        math.isfinite(value)
        for value in (x_m, y_m, translate_x_m, translate_y_m)
    ):
        raise ValueError("layout transform values must be finite")
    return (
        (x_m - translate_x_m) / scale,
        (y_m - translate_y_m) / scale,
    )


def node_error_color(mean_abs_error_m: float) -> str:
    """Map a node's mean absolute connection error onto a fixed 0-1 m scale."""

    ratio = min(max(mean_abs_error_m, 0.0), 1.0)
    green = (46, 157, 80)
    yellow = (226, 185, 59)
    red = (200, 59, 59)
    if ratio <= 0.5:
        amount = ratio * 2.0
        start, end = green, yellow
    else:
        amount = (ratio - 0.5) * 2.0
        start, end = yellow, red
    rgb = tuple(round(left + (right - left) * amount) for left, right in zip(start, end))
    return "#" + "".join(f"{component:02x}" for component in rgb)


def node_mean_absolute_errors(
    pairs: Iterable[AnchorPairDistance],
    residuals_m: dict[str, float],
) -> dict[str, float]:
    """Return each node's mean absolute residual across measured connections."""

    errors_by_node: dict[str, list[float]] = {}
    for pair in pairs:
        residual = residuals_m.get(f"{pair.anchor_a_id}-{pair.anchor_b_id}")
        if residual is None:
            residual = residuals_m.get(f"{pair.anchor_b_id}-{pair.anchor_a_id}")
        if residual is None or not math.isfinite(residual):
            continue
        abs_error = abs(residual)
        errors_by_node.setdefault(pair.anchor_a_id, []).append(abs_error)
        errors_by_node.setdefault(pair.anchor_b_id, []).append(abs_error)
    return {
        node_id: sum(errors) / len(errors)
        for node_id, errors in errors_by_node.items()
    }


def held_translation_delta(keys: Iterable[str]) -> tuple[float, float]:
    pressed = set(keys)
    return (
        HELD_TRANSLATION_STEP_M * (int("d" in pressed) - int("a" in pressed)),
        HELD_TRANSLATION_STEP_M * (int("w" in pressed) - int("s" in pressed)),
    )


def _parse_radio_distance(value: str, label: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ValueError(f"{label} must be a number in metres.") from exc
    if not math.isfinite(parsed):
        raise ValueError(f"{label} must be finite.")
    return parsed


def parse_neighbor_interval_m(
    minimum_value: str,
    maximum_value: str,
) -> tuple[float, float]:
    """Parse and validate the radio non-neighbor/neighbor boundaries."""

    minimum = _parse_radio_distance(minimum_value, "Radio minimum")
    maximum = _parse_radio_distance(maximum_value, "Neighbor maximum")
    if minimum <= 0.0:
        raise ValueError("Radio minimum must be greater than zero.")
    if maximum < minimum:
        raise ValueError("Radio interval must satisfy 0 < minimum <= maximum.")
    return minimum, maximum


def parse_neighbor_max_m(value: str) -> float:
    """Compatibility parser using the default lower radio boundary."""

    return parse_neighbor_interval_m(
        f"{DEFAULT_NONNEIGHBOR_MIN_M:g}", value
    )[1]


def parse_nearest_anchor_count(value: str) -> int:
    """Parse the per-anchor nearest measured-range filter; zero means all."""

    try:
        parsed = int(value.strip())
    except ValueError as exc:
        raise ValueError(
            "Closest ranges per anchor must be a whole number; use 0 for all."
        ) from exc
    if parsed < 0:
        raise ValueError("Closest ranges per anchor must be zero or greater.")
    return parsed


class SurveyGeometryView(ttk.Frame):
    """Render live command steps, pair results, and a solved 2D layout."""

    def __init__(
        self,
        parent: tk.Misc,
        *,
        on_positions_changed: Callable[[LayoutRegistration], None] | None = None,
        on_layout_edited: Callable[[dict[str, tuple[float, float]]], None]
        | None = None,
        on_refine_requested: Callable[[], None] | None = None,
        on_solve_requested: Callable[
            [
                str,
                str,
                float,
                float,
                int,
                dict[str, tuple[float, float]] | None,
            ],
            None,
        ]
        | None = None,
    ) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self.model: SurveyOperationModel | None = None
        self._on_positions_changed = on_positions_changed
        self._on_layout_edited = on_layout_edited
        self._on_refine_requested = on_refine_requested
        self._on_solve_requested = on_solve_requested
        self._layout_revision = -1
        self._layout_object: object | None = None
        self._oriented_positions: dict[str, tuple[float, float]] = {}
        self._display_positions: dict[str, tuple[float, float]] = {}
        self._uniform_scale = 1.0
        self._translate_x_m = 0.0
        self._translate_y_m = 0.0
        self._geometry_job_pending = False
        self._geometry_job_label = ""
        self.neighbor_min_var = tk.StringVar(
            value=f"{DEFAULT_NONNEIGHBOR_MIN_M:g}"
        )
        self.neighbor_max_var = tk.StringVar(value=f"{DEFAULT_NEIGHBOR_MAX_M:g}")
        self.nearest_anchor_count_var = tk.StringVar(value="0")
        self._fullscreen_window: tk.Toplevel | None = None
        self._fullscreen_canvas: tk.Canvas | None = None
        self._fullscreen_registration_controls: LayoutRegistrationControls | None = None
        self._fullscreen_layout_buttons: list[ttk.Button] = []
        self._fullscreen_solve_button: ttk.Button | None = None
        self._fullscreen_refine_button: ttk.Button | None = None
        self._fullscreen_resolve_dragged_button: ttk.Button | None = None
        self._held_move_keys: set[str] = set()
        self._held_move_after_id: str | None = None
        self._drag_anchor_id: str | None = None
        self._drag_canvas: tk.Canvas | None = None
        self._drag_projection: CanvasProjection | None = None
        self._drag_start_px: tuple[float, float] | None = None
        self._drag_moved = False
        self._manual_layout_dirty = False

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
        geometry.rowconfigure(3, weight=1)
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
        self.fullscreen_button = ttk.Button(
            toolbar,
            text="Fullscreen",
            command=self._open_fullscreen,
            state="disabled",
        )
        self.fullscreen_button.grid(row=0, column=4, padx=(8, 0))
        solver_bar = ttk.Frame(geometry, style="Panel.TFrame")
        solver_bar.grid(row=1, column=0, sticky="ew", pady=(0, 4))
        ttk.Label(solver_bar, text="Solver").grid(row=0, column=0, padx=(0, 3))
        self.solver_var = tk.StringVar(value=CONNECTIVITY_INTERVAL_ALGORITHM)
        self.solver_combo = ttk.Combobox(
            solver_bar,
            textvariable=self.solver_var,
            values=SOLVER_CHOICES,
            state="readonly",
            width=40,
        )
        self.solver_combo.grid(row=0, column=1, padx=(0, 8))
        ttk.Label(solver_bar, text="Seed").grid(row=0, column=2, padx=(0, 3))
        self.seed_var = tk.StringVar(value=CONNECTIVITY_SEEDS[0])
        self.seed_combo = ttk.Combobox(
            solver_bar,
            textvariable=self.seed_var,
            values=CONNECTIVITY_SEEDS,
            state="readonly",
            width=24,
        )
        self.seed_combo.grid(row=0, column=3, padx=(0, 8))
        ttk.Label(solver_bar, text="Radio min (m)").grid(
            row=1,
            column=0,
            pady=(4, 0),
            padx=(0, 3),
        )
        self.neighbor_min_spinbox = ttk.Spinbox(
            solver_bar,
            textvariable=self.neighbor_min_var,
            from_=0.1,
            to=100.0,
            increment=0.5,
            width=6,
        )
        self.neighbor_min_spinbox.grid(
            row=1, column=1, padx=(0, 8), pady=(4, 0)
        )
        ttk.Label(solver_bar, text="Neighbor max (m)").grid(
            row=1,
            column=2,
            pady=(4, 0),
            padx=(0, 3),
        )
        self.neighbor_max_spinbox = ttk.Spinbox(
            solver_bar,
            textvariable=self.neighbor_max_var,
            from_=0.1,
            to=100.0,
            increment=0.5,
            width=6,
        )
        self.neighbor_max_spinbox.grid(
            row=1, column=3, padx=(0, 8), pady=(4, 0)
        )
        self.solve_button = ttk.Button(
            solver_bar,
            text="Solve / re-solve",
            command=self._request_solve,
            state="disabled",
        )
        self.solve_button.grid(row=1, column=4, pady=(4, 0))
        self.refine_button = ttk.Button(
            solver_bar,
            text="Refine measured distances only",
            command=self._request_refinement,
            state="disabled",
        )
        self.refine_button.grid(
            row=1, column=5, padx=(4, 0), pady=(4, 0)
        )
        ttk.Label(solver_bar, text="Closest ranges / anchor (0=all)").grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(4, 0)
        )
        self.nearest_anchor_count_spinbox = ttk.Spinbox(
            solver_bar,
            textvariable=self.nearest_anchor_count_var,
            from_=0,
            to=255,
            increment=1,
            width=6,
        )
        self.nearest_anchor_count_spinbox.grid(
            row=2, column=2, sticky="w", pady=(4, 0)
        )
        self.resolve_dragged_button = ttk.Button(
            solver_bar,
            text="Re-solve dragged",
            command=self._request_dragged_solve,
            state="disabled",
        )
        self.resolve_dragged_button.grid(
            row=2, column=4, sticky="w", pady=(4, 0)
        )

        self.registration_controls = LayoutRegistrationControls(
            geometry,
            on_translate=self.nudge_translation,
            on_scale=self.nudge_scale,
            on_reset=self.reset_transform,
        )
        self.registration_controls.grid(row=2, column=0, sticky="ew", pady=(0, 4))
        self.canvas = tk.Canvas(
            geometry,
            background=PANEL_BG,
            highlightthickness=1,
            highlightbackground="#d5dbdd",
            height=300,
        )
        self.canvas.grid(row=3, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self._redraw())
        self._bind_anchor_dragging(self.canvas)

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

        if model.layout is not None and model.layout is not self._layout_object:
            self._layout_revision = model.layout_revision
            self._layout_object = model.layout
            self._oriented_positions = dict(model.layout.positions_m)
            self._manual_layout_dirty = (
                model.layout.algorithm == MANUALLY_EDITED_LAYOUT_ALGORITHM
                or model.layout.algorithm.startswith(
                    f"{MANUALLY_EDITED_LAYOUT_ALGORITHM};"
                )
            )
            self.reset_transform(notify=False)
        elif model.layout is None:
            self._layout_revision = -1
            self._layout_object = None
            self._oriented_positions = {}
            self._display_positions = {}
            self._manual_layout_dirty = False
        has_layout = bool(self._display_positions)
        self._sync_control_states(has_layout)
        self.fullscreen_button.configure(
            state="normal" if model.slot_to_anchor else "disabled"
        )
        if self._geometry_job_pending:
            self.geometry_var.set(
                self._geometry_job_label or "Solving relative 2D geometry..."
            )
        elif model.layout is not None:
            warnings = (
                f" · {len(model.layout.warnings)} warning(s)"
                if model.layout.warnings
                else ""
            )
            self.geometry_var.set(
                f"{model.layout.algorithm} · RMSE {model.layout.rmse_m:.3f} m · "
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
        if not self._oriented_positions:
            return
        self._oriented_positions = rotate_layout(self._oriented_positions, degrees)
        self._apply_transform()

    def _mirror(self) -> None:
        if not self._oriented_positions:
            return
        self._oriented_positions = mirror_layout(self._oriented_positions, "x")
        self._apply_transform()

    def _apply_transform(self) -> None:
        if not self._oriented_positions:
            return
        transformed = transform_layout(
            self._oriented_positions,
            scale=self._uniform_scale,
            translate_x_m=self._translate_x_m,
            translate_y_m=self._translate_y_m,
        )
        self._display_positions = transformed
        self._show_registration()
        self._redraw()

    def accept_manual_layout(self, layout: AnchorLayoutResult) -> None:
        """Synchronize a model-accepted drag without resetting frame controls."""

        self._layout_object = layout
        if self.model is not None:
            self._layout_revision = self.model.layout_revision
        self._oriented_positions = dict(layout.positions_m)
        self._manual_layout_dirty = True
        self._display_positions = transform_layout(
            self._oriented_positions,
            scale=self._uniform_scale,
            translate_x_m=self._translate_x_m,
            translate_y_m=self._translate_y_m,
        )
        self._sync_control_states()
        self._redraw()

    def _bind_anchor_dragging(self, canvas: tk.Canvas) -> None:
        canvas.bind(
            "<ButtonPress-1>",
            lambda event, source=canvas: self._anchor_drag_started(
                source, event
            ),
        )
        canvas.bind(
            "<B1-Motion>",
            lambda event, source=canvas: self._anchor_dragged(source, event),
        )
        canvas.bind(
            "<ButtonRelease-1>",
            lambda event, source=canvas: self._anchor_drag_finished(
                source, event
            ),
        )

    def _anchor_drag_started(
        self,
        canvas: tk.Canvas,
        event: tk.Event[tk.Misc],
    ) -> str | None:
        model = self.model
        if (
            model is None
            or model.layout is None
            or not self._display_positions
            or self._geometry_job_pending
        ):
            return None
        width = max(canvas.winfo_width(), 520)
        height = max(canvas.winfo_height(), 260)
        reference = self._oriented_positions or self._display_positions
        projection = _canvas_projection(
            (*reference.values(), (0.0, 0.0)),
            width,
            height,
        )
        closest: tuple[float, str] | None = None
        for anchor_id, position in self._display_positions.items():
            x_px, y_px = projection.project(*position)
            distance = math.hypot(event.x - x_px, event.y - y_px)
            if closest is None or distance < closest[0]:
                closest = (distance, anchor_id)
        if closest is None or closest[0] > 14.0:
            return None
        self._drag_anchor_id = closest[1]
        self._drag_canvas = canvas
        self._drag_projection = projection
        self._drag_start_px = (float(event.x), float(event.y))
        self._drag_moved = False
        canvas.configure(cursor="fleur")
        canvas.grab_set()
        return "break"

    def _anchor_dragged(
        self,
        canvas: tk.Canvas,
        event: tk.Event[tk.Misc],
    ) -> str | None:
        anchor_id = self._drag_anchor_id
        projection = self._drag_projection
        if (
            anchor_id is None
            or projection is None
            or canvas is not self._drag_canvas
        ):
            return None
        if not self._drag_moved and self._drag_start_px is not None:
            if math.dist(
                self._drag_start_px,
                (float(event.x), float(event.y)),
            ) < 2.0:
                return "break"
        displayed = projection.unproject(float(event.x), float(event.y))
        self._oriented_positions[anchor_id] = inverse_transform_point(
            displayed,
            scale=self._uniform_scale,
            translate_x_m=self._translate_x_m,
            translate_y_m=self._translate_y_m,
        )
        self._display_positions = transform_layout(
            self._oriented_positions,
            scale=self._uniform_scale,
            translate_x_m=self._translate_x_m,
            translate_y_m=self._translate_y_m,
        )
        self._drag_moved = True
        self._redraw()
        return "break"

    def _anchor_drag_finished(
        self,
        canvas: tk.Canvas,
        event: tk.Event[tk.Misc],
    ) -> str | None:
        if canvas is not self._drag_canvas or self._drag_anchor_id is None:
            return None
        self._anchor_dragged(canvas, event)
        anchor_id = self._drag_anchor_id
        moved = self._drag_moved
        try:
            canvas.grab_release()
        except tk.TclError:
            pass
        canvas.configure(cursor="")
        self._drag_anchor_id = None
        self._drag_canvas = None
        self._drag_projection = None
        self._drag_start_px = None
        self._drag_moved = False
        if not moved:
            self._redraw()
            return "break"
        self._manual_layout_dirty = True
        if self._on_layout_edited is not None:
            self._on_layout_edited(dict(self._oriented_positions))
        if self._on_positions_changed is not None:
            self._on_positions_changed(self.registration)
        self.geometry_var.set(
            f"Dragged {anchor_id}; kept in GUI RAM. "
            "Use Re-solve dragged to optimize from this layout."
        )
        self._sync_control_states()
        self._redraw()
        return "break"

    @property
    def registration(self) -> LayoutRegistration:
        return LayoutRegistration(
            reference_positions_m=dict(self._oriented_positions),
            positions_m=dict(self._display_positions),
            scale=self._uniform_scale,
            translate_x_m=self._translate_x_m,
            translate_y_m=self._translate_y_m,
        )

    def nudge_translation(self, delta_x_m: float, delta_y_m: float) -> None:
        self._translate_x_m += delta_x_m
        self._translate_y_m += delta_y_m
        self._apply_transform()

    def nudge_scale(self, factor: float) -> None:
        if not math.isfinite(factor) or factor <= 0.0:
            return
        self._uniform_scale *= factor
        self._apply_transform()

    def reset_transform(self, *, notify: bool = True) -> None:
        if not self._oriented_positions:
            return
        model = self.model
        if model is not None and model.layout is not None:
            self._oriented_positions = dict(model.layout.positions_m)
        self._translate_x_m = 0.0
        self._translate_y_m = 0.0
        self._uniform_scale = 1.0
        self._display_positions = dict(self._oriented_positions)
        self._show_registration()
        self._redraw()
        if notify and self._on_positions_changed is not None:
            self._on_positions_changed(self.registration)

    def set_geometry_job_pending(self, pending: bool, label: str = "") -> None:
        self._geometry_job_pending = pending
        self._geometry_job_label = label
        if self.model is not None:
            self.show_model(self.model)

    def _request_refinement(self) -> None:
        if self._on_refine_requested is not None:
            self._on_refine_requested()

    def _request_solve(
        self,
        *,
        seed_override: str | None = None,
        current_positions_override: dict[str, tuple[float, float]] | None = None,
    ) -> None:
        solver = self.solver_var.get()
        neighbor_min_m = DEFAULT_NONNEIGHBOR_MIN_M
        neighbor_max_m = DEFAULT_NEIGHBOR_MAX_M
        if solver in RADIO_INTERVAL_SOLVERS:
            try:
                neighbor_min_m, neighbor_max_m = self.neighbor_interval_m
            except ValueError as exc:
                self.geometry_var.set(str(exc))
                return
        try:
            nearest_per_anchor = self.nearest_anchor_count
        except ValueError as exc:
            self.geometry_var.set(str(exc))
            return
        if self._on_solve_requested is not None:
            self._on_solve_requested(
                solver,
                seed_override or self.seed_var.get(),
                neighbor_min_m,
                neighbor_max_m,
                nearest_per_anchor,
                current_positions_override,
            )

    def _request_dragged_solve(self) -> None:
        if not self._manual_layout_dirty or not self._oriented_positions:
            return
        self.seed_var.set(SEED_CURRENT)
        self._request_solve(
            seed_override=SEED_CURRENT,
            current_positions_override=dict(self._oriented_positions),
        )

    @property
    def neighbor_interval_m(self) -> tuple[float, float]:
        return parse_neighbor_interval_m(
            self.neighbor_min_var.get(),
            self.neighbor_max_var.get(),
        )

    @property
    def neighbor_max_m(self) -> float:
        return self.neighbor_interval_m[1]

    @property
    def nearest_anchor_count(self) -> int:
        return parse_nearest_anchor_count(
            self.nearest_anchor_count_var.get()
        )

    def _show_registration(self) -> None:
        controls = (
            self.registration_controls,
            self._fullscreen_registration_controls,
        )
        for control in controls:
            if control is not None and control.winfo_exists():
                control.show_registration(
                    self._uniform_scale,
                    self._translate_x_m,
                    self._translate_y_m,
                )

    def _sync_control_states(self, has_layout: bool | None = None) -> None:
        if has_layout is None:
            has_layout = bool(self._display_positions)
        model = self.model
        can_solve = bool(
            model is not None
            and model.geometry_solve_ready
            and not self._geometry_job_pending
        )
        state = "normal" if has_layout else "disabled"
        for button in (
            self.mirror_button,
            self.left_button,
            self.right_button,
            *self._fullscreen_layout_buttons,
        ):
            button.configure(state=state)
        self.registration_controls.set_enabled(has_layout)
        controls = self._fullscreen_registration_controls
        if controls is not None and controls.winfo_exists():
            controls.set_enabled(has_layout)
        self.solve_button.configure(state="normal" if can_solve else "disabled")
        self.resolve_dragged_button.configure(
            state=(
                "normal"
                if can_solve and self._manual_layout_dirty
                else "disabled"
            )
        )
        solve_text = (
            "Solving..." if self._geometry_job_pending else "Solve / re-solve"
        )
        self.solve_button.configure(text=solve_text)
        refine_text = "Refine measured distances only"
        self.refine_button.configure(
            state="normal" if has_layout and can_solve else "disabled",
            text=refine_text,
        )
        if self._fullscreen_solve_button is not None:
            self._fullscreen_solve_button.configure(
                state="normal" if can_solve else "disabled",
                text=solve_text,
            )
        if self._fullscreen_refine_button is not None:
            self._fullscreen_refine_button.configure(
                state="normal" if has_layout and can_solve else "disabled",
                text=refine_text,
            )
        if self._fullscreen_resolve_dragged_button is not None:
            self._fullscreen_resolve_dragged_button.configure(
                state=(
                    "normal"
                    if can_solve and self._manual_layout_dirty
                    else "disabled"
                )
            )

    def _open_fullscreen(self) -> None:
        window = self._fullscreen_window
        if window is not None and window.winfo_exists():
            window.lift()
            window.focus_force()
            return
        window = tk.Toplevel(self)
        self._fullscreen_window = window
        window.title("IMEC2 Survey Geometry — Fullscreen")
        window.configure(background=PANEL_BG)
        window.columnconfigure(0, weight=1)
        window.rowconfigure(3, weight=1)
        header = ttk.Frame(window, style="Panel.TFrame", padding=(8, 6))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        ttk.Label(
            header,
            textvariable=self.geometry_var,
            style="Section.TLabel",
        ).grid(row=0, column=0, sticky="w")
        ttk.Label(
            header,
            text="Hold WASD to move · Esc or F11 exits",
            style="PanelMuted.TLabel",
        ).grid(row=0, column=1, padx=(8, 8))
        ttk.Button(
            header,
            text="Exit fullscreen",
            command=self._close_fullscreen,
        ).grid(row=0, column=2)

        solver_bar = ttk.Frame(window, style="Panel.TFrame", padding=(8, 2))
        solver_bar.grid(row=1, column=0, sticky="ew")
        ttk.Label(solver_bar, text="Solver").grid(row=0, column=0, padx=(0, 3))
        ttk.Combobox(
            solver_bar,
            textvariable=self.solver_var,
            values=SOLVER_CHOICES,
            state="readonly",
            width=40,
        ).grid(row=0, column=1, padx=(0, 8))
        ttk.Label(solver_bar, text="Seed").grid(row=0, column=2, padx=(0, 3))
        ttk.Combobox(
            solver_bar,
            textvariable=self.seed_var,
            values=CONNECTIVITY_SEEDS,
            state="readonly",
            width=24,
        ).grid(row=0, column=3, padx=(0, 8))
        ttk.Label(solver_bar, text="Radio min (m)").grid(
            row=0,
            column=4,
            padx=(0, 3),
        )
        ttk.Spinbox(
            solver_bar,
            textvariable=self.neighbor_min_var,
            from_=0.1,
            to=100.0,
            increment=0.5,
            width=6,
        ).grid(row=0, column=5, padx=(0, 8))
        ttk.Label(solver_bar, text="Neighbor max (m)").grid(
            row=0,
            column=6,
            padx=(0, 3),
        )
        ttk.Spinbox(
            solver_bar,
            textvariable=self.neighbor_max_var,
            from_=0.1,
            to=100.0,
            increment=0.5,
            width=6,
        ).grid(row=0, column=7, padx=(0, 8))
        ttk.Label(solver_bar, text="Closest ranges / anchor (0=all)").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(4, 0)
        )
        ttk.Spinbox(
            solver_bar,
            textvariable=self.nearest_anchor_count_var,
            from_=0,
            to=255,
            increment=1,
            width=6,
        ).grid(row=1, column=2, sticky="w", pady=(4, 0))
        self._fullscreen_solve_button = ttk.Button(
            solver_bar,
            text="Solve / re-solve",
            command=self._request_solve,
        )
        self._fullscreen_solve_button.grid(row=0, column=8)
        self._fullscreen_refine_button = ttk.Button(
            solver_bar,
            text="Refine measured distances only",
            command=self._request_refinement,
        )
        self._fullscreen_refine_button.grid(row=0, column=9, padx=(4, 8))
        self._fullscreen_resolve_dragged_button = ttk.Button(
            solver_bar,
            text="Re-solve dragged",
            command=self._request_dragged_solve,
        )
        self._fullscreen_resolve_dragged_button.grid(
            row=1, column=4, sticky="w", pady=(4, 0)
        )
        for column, (label, command) in enumerate(
            (
                ("Mirror", self._mirror),
                ("-90°", lambda: self._rotate(-90.0)),
                ("+90°", lambda: self._rotate(90.0)),
            ),
            start=10,
        ):
            button = ttk.Button(solver_bar, text=label, command=command)
            button.grid(row=0, column=column, padx=(4, 0))
            self._fullscreen_layout_buttons.append(button)

        self._fullscreen_registration_controls = LayoutRegistrationControls(
            window,
            on_translate=self.nudge_translation,
            on_scale=self.nudge_scale,
            on_reset=self.reset_transform,
        )
        self._fullscreen_registration_controls.grid(
            row=2,
            column=0,
            sticky="ew",
            padx=8,
            pady=(2, 4),
        )
        canvas = tk.Canvas(
            window,
            background=PANEL_BG,
            highlightthickness=0,
        )
        self._fullscreen_canvas = canvas
        canvas.grid(row=3, column=0, sticky="nsew")
        canvas.bind("<Configure>", lambda _event: self._redraw())
        self._bind_anchor_dragging(canvas)
        window.bind("<KeyPress>", self._fullscreen_key_pressed)
        window.bind("<KeyRelease>", self._fullscreen_key_released)
        window.bind("<Escape>", lambda _event: self._close_fullscreen())
        window.bind("<F11>", lambda _event: self._close_fullscreen())
        window.protocol("WM_DELETE_WINDOW", self._close_fullscreen)
        try:
            window.attributes("-fullscreen", True)
        except tk.TclError:
            window.state("zoomed")
        self._show_registration()
        self._sync_control_states()
        window.focus_force()
        self._redraw()

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
            self.nudge_translation(delta_x_m, delta_y_m)
        if self._held_move_keys:
            self._held_move_after_id = window.after(
                HELD_TRANSLATION_INTERVAL_MS,
                self._run_held_movement,
            )

    def _close_fullscreen(self) -> None:
        window = self._fullscreen_window
        canvas = self._fullscreen_canvas
        after_id = self._held_move_after_id
        self._held_move_after_id = None
        self._held_move_keys.clear()
        if window is not None and after_id is not None and window.winfo_exists():
            window.after_cancel(after_id)
        self._fullscreen_window = None
        self._fullscreen_canvas = None
        self._fullscreen_registration_controls = None
        self._fullscreen_layout_buttons = []
        self._fullscreen_solve_button = None
        self._fullscreen_refine_button = None
        self._fullscreen_resolve_dragged_button = None
        if canvas is not None and self._drag_canvas is canvas:
            try:
                canvas.grab_release()
            except tk.TclError:
                pass
            self._drag_anchor_id = None
            self._drag_canvas = None
            self._drag_projection = None
            self._drag_start_px = None
            self._drag_moved = False
        if window is not None and window.winfo_exists():
            window.destroy()

    def _redraw(self) -> None:
        self._draw_geometry(self.canvas)
        window = self._fullscreen_window
        canvas = self._fullscreen_canvas
        if (
            window is not None
            and canvas is not None
            and window.winfo_exists()
            and canvas.winfo_exists()
        ):
            self._draw_geometry(canvas)

    def _draw_geometry(self, canvas: tk.Canvas) -> None:
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
        reference = self._oriented_positions or positions
        projection = (
            self._drag_projection
            if canvas is self._drag_canvas and self._drag_projection is not None
            else _canvas_projection(
                (*reference.values(), (0.0, 0.0)),
                width,
                height,
            )
        )
        project = projection.project
        origin_x, origin_y = project(0.0, 0.0)
        canvas.create_line(0, origin_y, width, origin_y, fill="#e3e7e9", dash=(3, 4))
        canvas.create_line(origin_x, 0, origin_x, height, fill="#e3e7e9", dash=(3, 4))
        canvas.create_text(
            origin_x + 4,
            origin_y - 4,
            text="(0, 0)",
            fill=MUTED,
            anchor="sw",
            font=("TkDefaultFont", 8),
        )
        node_errors = (
            node_mean_absolute_errors(
                model.geometry_pairs,
                model.layout.residuals_m,
            )
            if model.layout is not None
            else {}
        )
        measured_keys: set[tuple[str, str]] = set()
        for pair in model.geometry_pairs:
            label_a = pair.anchor_a_id
            label_b = pair.anchor_b_id
            if label_a not in positions or label_b not in positions:
                continue
            measured_keys.add(tuple(sorted((label_a, label_b))))
            ax, ay = project(*positions[label_a])
            bx, by = project(*positions[label_b])
            canvas.create_line(ax, ay, bx, by, fill=MUTED, width=2)
            canvas.create_text(
                (ax + bx) / 2,
                (ay + by) / 2 - 8,
                text=f"{pair.distance_m:.2f} m",
                fill=MUTED,
                font=("TkDefaultFont", 8),
            )

        for pair_index, pair in enumerate(model.plan_pairs):
            anchor_a = model.slot_to_anchor.get(pair.initiator_slot)
            anchor_b = model.slot_to_anchor.get(pair.responder_slot)
            if anchor_a is None or anchor_b is None:
                continue
            label_a = anchor_label(anchor_a)
            label_b = anchor_label(anchor_b)
            if label_a not in positions or label_b not in positions:
                continue
            if tuple(sorted((label_a, label_b))) in measured_keys:
                continue
            ax, ay = project(*positions[label_a])
            bx, by = project(*positions[label_b])
            result = model.results.get(pair_index)
            color = AMBER if result is not None else "#aeb7bb"
            canvas.create_line(
                ax, ay, bx, by, fill=color, width=2, dash=(5, 4)
            )
            canvas.create_text(
                (ax + bx) / 2,
                (ay + by) / 2 - 8,
                text=f"P{pair_index}",
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
            dragging = label == self._drag_anchor_id
            node_error = node_errors.get(label)
            node_color = (
                node_error_color(node_error) if node_error is not None else ACCENT
            )
            canvas.create_oval(
                x - 8,
                y - 8,
                x + 8,
                y + 8,
                fill=node_color,
                outline=AMBER if dragging else "#083d34",
                width=2 if dragging else 1,
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
            canvas.create_text(
                x,
                y + 13,
                text=f"({position[0]:.2f}, {position[1]:.2f}) m",
                fill=MUTED,
                anchor="n",
                font=("TkDefaultFont", 8),
            )
        if node_errors:
            canvas.create_text(
                width - 10,
                height - 10,
                text=(
                    "Node fit: mean absolute connection error; "
                    "green = 0 m, red = 1 m or more"
                ),
                fill=MUTED,
                anchor="se",
                font=("TkDefaultFont", 8),
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
        else:
            canvas.create_text(
                12,
                10,
                text=(
                    "Drag an anchor to keep a manual edit; "
                    "Re-solve dragged optimizes from it"
                ),
                fill=MUTED,
                anchor="nw",
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


def _canvas_projection(
    points: Iterable[tuple[float, float]], width: int, height: int
) -> CanvasProjection:
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

    return CanvasProjection(
        min_x=min_x,
        min_y=min_y,
        scale=scale,
        offset_x=offset_x,
        offset_y=offset_y,
        height=float(height),
    )


def _projector(
    points: Iterable[tuple[float, float]], width: int, height: int
):
    """Compatibility projection callable used by older view tests/callers."""

    return _canvas_projection(points, width, height).project
