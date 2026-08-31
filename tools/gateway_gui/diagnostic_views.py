"""Operational Tk views for geometry, click, and mesh diagnostics."""

from __future__ import annotations

import math
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, ttk
from typing import Callable, Iterable

from PIL import Image, ImageEnhance, ImageTk, UnidentifiedImageError

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
from .theme import (
    ACCENT,
    AMBER,
    BORDER,
    CANVAS_BG,
    CONNECTION,
    ERROR,
    GRID,
    INK,
    MAGENTA,
    MUTED,
    PANEL_BG,
)


BLUE = "#2f8cff"


def parse_blueprint_dimensions_m(
    width_value: str,
    height_value: str,
) -> tuple[float, float]:
    """Parse an exact metric blueprint rectangle."""

    values: list[float] = []
    for label, raw in (
        ("Blueprint width", width_value),
        ("Blueprint height", height_value),
    ):
        try:
            value = float(raw)
        except ValueError as exc:
            raise ValueError(f"{label} must be a number in metres.") from exc
        if not math.isfinite(value) or value <= 0.0:
            raise ValueError(f"{label} must be finite and greater than zero.")
        values.append(value)
    return values[0], values[1]


class BlueprintControls(ttk.Frame):
    """Shared embedded/fullscreen controls for one RAM-only floor plan."""

    def __init__(
        self,
        parent: tk.Misc,
        *,
        width_var: tk.StringVar,
        height_var: tk.StringVar,
        status_var: tk.StringVar,
        on_load: Callable[[], None],
        on_apply: Callable[[], None],
        on_clear: Callable[[], None],
        on_mirror: Callable[[], None],
    ) -> None:
        super().__init__(parent, style="Panel.TFrame")
        self.columnconfigure(2, weight=1)
        ttk.Button(self, text="Load blueprint", command=on_load).grid(
            row=0, column=0, padx=(0, 4)
        )
        self.clear_button = ttk.Button(self, text="Clear", command=on_clear)
        self.clear_button.grid(row=0, column=1, padx=(0, 8))
        ttk.Label(
            self,
            textvariable=status_var,
            style="PanelMuted.TLabel",
        ).grid(row=0, column=2, columnspan=5, sticky="w")
        ttk.Label(self, text="Blueprint width (m)", style="Panel.TLabel").grid(
            row=1, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Entry(self, textvariable=width_var, width=8).grid(
            row=1, column=1, sticky="w", padx=(4, 10), pady=(4, 0)
        )
        ttk.Label(self, text="Height (m)", style="Panel.TLabel").grid(
            row=1, column=2, sticky="e", pady=(4, 0)
        )
        ttk.Entry(self, textvariable=height_var, width=8).grid(
            row=1, column=3, sticky="w", padx=(4, 4), pady=(4, 0)
        )
        self.apply_button = ttk.Button(
            self,
            text="Apply exact size",
            command=on_apply,
        )
        self.apply_button.grid(row=1, column=4, padx=(0, 8), pady=(4, 0))
        self.mirror_button = ttk.Button(
            self,
            text="Mirror anchors",
            command=on_mirror,
        )
        self.mirror_button.grid(row=1, column=5, pady=(4, 0))

    def set_enabled(self, *, has_blueprint: bool, has_positions: bool) -> None:
        self.clear_button.configure(state="normal" if has_blueprint else "disabled")
        self.apply_button.configure(state="normal" if has_blueprint else "disabled")
        self.mirror_button.configure(state="normal" if has_positions else "disabled")


class ClickDiagnosticsView(ttk.Frame):
    def __init__(
        self,
        parent: tk.Misc,
        *,
        on_translate: Callable[[float, float], None] = lambda _x, _y: None,
        on_scale: Callable[[float], None] = lambda _factor: None,
        on_reset: Callable[[], None] = lambda: None,
        on_mirror: Callable[[], None] = lambda: None,
        on_click_selected: Callable[[tuple[int, int, int]], None] = lambda _key: None,
        on_click_deleted: Callable[[tuple[int, int, int]], None] = lambda _key: None,
    ) -> None:
        super().__init__(parent, style="Panel.TFrame", padding=8)
        self._on_translate = on_translate
        self._on_scale = on_scale
        self._on_reset = on_reset
        self._on_mirror = on_mirror
        self._on_click_selected = on_click_selected
        self._on_click_deleted = on_click_deleted
        self._fullscreen_window: tk.Toplevel | None = None
        self._fullscreen_canvas: tk.Canvas | None = None
        self._fullscreen_registration_controls: LayoutRegistrationControls | None = None
        self._held_move_keys: set[str] = set()
        self._held_move_after_id: str | None = None
        self._registration_scale = 1.0
        self._registration_translate_x_m = 0.0
        self._registration_translate_y_m = 0.0
        self._selected_anchor_id: str | None = None
        self._blueprint_source: Image.Image | None = None
        self._blueprint_path: Path | None = None
        self._blueprint_width_m = 10.0
        self._blueprint_height_m = 10.0
        self._blueprint_drag_x_m = 0.0
        self._blueprint_drag_y_m = 0.0
        self._blueprint_drag_start: tuple[int, int, float, float, float] | None = None
        self._blueprint_render_cache: dict[tuple[int, int], Image.Image] = {}
        self._blueprint_photo_by_canvas: dict[int, ImageTk.PhotoImage] = {}
        self.blueprint_width_var = tk.StringVar(value="10.000")
        self.blueprint_height_var = tk.StringVar(value="10.000")
        self.blueprint_status_var = tk.StringVar(
            value="Load a raster blueprint, then enter its exact size in metres."
        )
        self.selection_var = tk.StringVar(
            value="Click an anchor to isolate its connections."
        )
        self._blueprint_controls: list[BlueprintControls] = []
        self.columnconfigure(0, weight=1)
        self.rowconfigure(3, weight=1)
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
        ttk.Label(bar, textvariable=self.wake_var, style="Panel.TLabel", wraplength=650, justify="left").grid(
            row=2, column=0, columnspan=3, sticky="w", pady=(3, 0)
        )
        ttk.Label(
            bar,
            textvariable=self.selection_var,
            style="PanelMuted.TLabel",
        ).grid(row=3, column=0, columnspan=3, sticky="w", pady=(3, 0))
        self.blueprint_controls = self._make_blueprint_controls(self)
        self.blueprint_controls.grid(row=1, column=0, sticky="ew", pady=(0, 6))
        self.registration_controls = LayoutRegistrationControls(
            self,
            on_translate=on_translate,
            on_scale=on_scale,
            on_reset=on_reset,
        )
        self.registration_controls.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        self.canvas = tk.Canvas(
            self,
            background=CANVAS_BG,
            highlightthickness=1,
            highlightbackground=BORDER,
        )
        self.canvas.grid(row=3, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", lambda _event: self.redraw())
        self.canvas.bind(
            "<ButtonPress-1>",
            lambda event, source=self.canvas: self._press_at(source, event),  # type: ignore[misc]
        )
        self.canvas.bind("<B1-Motion>", self._drag_blueprint)
        self.canvas.bind("<ButtonRelease-1>", self._end_blueprint_drag)
        self.canvas.bind("<Delete>", self._delete_selected_click)
        self.canvas.bind("<BackSpace>", self._delete_selected_click)
        self.diagnostic_state: ClickDiagnosticState | None = None
        self.click_states: tuple[ClickDiagnosticState, ...] = ()
        self.positions: dict[str, tuple[float, float]] = {}
        self.reference_positions: dict[str, tuple[float, float]] = {}
        self.connections: frozenset[tuple[str, str]] = frozenset()
        self._sync_blueprint_controls()

    def _make_blueprint_controls(self, parent: tk.Misc) -> BlueprintControls:
        controls = BlueprintControls(
            parent,
            width_var=self.blueprint_width_var,
            height_var=self.blueprint_height_var,
            status_var=self.blueprint_status_var,
            on_load=self._load_blueprint,
            on_apply=self._apply_blueprint_size,
            on_clear=self._clear_blueprint,
            on_mirror=self._on_mirror,
        )
        self._blueprint_controls.append(controls)
        return controls

    def _sync_blueprint_controls(self) -> None:
        for controls in self._blueprint_controls:
            if controls.winfo_exists():
                controls.set_enabled(
                    has_blueprint=self._blueprint_source is not None,
                    has_positions=bool(self.positions),
                )

    def show(
        self,
        state: ClickDiagnosticState,
        positions: dict[str, tuple[float, float]],
        click_states: Iterable[ClickDiagnosticState] = (),
    ) -> None:
        self.diagnostic_state = state
        self.click_states = tuple(click_states)
        self.positions = dict(positions)
        if self._selected_anchor_id not in self.positions:
            self._selected_anchor_id = None
            self.selection_var.set("Click an anchor to isolate its connections.")
        self.registration_controls.set_enabled(bool(positions))
        if self._fullscreen_registration_controls is not None:
            self._fullscreen_registration_controls.set_enabled(bool(positions))
        self._sync_blueprint_controls()
        if state.identity:
            self.identity_var.set(f"Session {state.identity[0]}  •  Event {state.identity[1]}  •  Clicker {anchor_label(state.identity[2])}")
        else:
            self.identity_var.set("No click event")
        if state.result:
            maximum = max((abs(value) for value in state.result.range_residuals_m.values()), default=0.0)
            algorithm = (
                "Height-agnostic LS (magenta)"
                if state.result.algorithm == "height_agnostic_range_ls"
                else "Radical axis (cyan)"
            )
            self.fit_var.set(f"{algorithm}   x {state.result.x_m:.3f} m   y {state.result.y_m:.3f} m   RMSE {state.result.rmse_m:.3f} m   Max {maximum:.3f} m")
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

    def _select_at(self, canvas: tk.Canvas, event: tk.Event[tk.Misc]) -> str | None:
        canvas.focus_set()
        project = self._projection_for_canvas(canvas)
        if project is not None:
            closest: tuple[float, tuple[int, int, int]] | None = None
            for click_state in self.click_states:
                if click_state.identity is None or click_state.result is None:
                    continue
                x, y = project(click_state.result.x_m, click_state.result.y_m)
                distance = math.hypot(float(event.x) - x, float(event.y) - y)
                if closest is None or distance < closest[0]:
                    closest = distance, click_state.identity
            if closest is not None and closest[0] <= 16.0:
                self._on_click_selected(closest[1])
                return "break"
        return self._select_anchor_at(canvas, event)

    def _press_at(self, canvas: tk.Canvas, event: tk.Event[tk.Misc]) -> str | None:
        selected = self._select_at(canvas, event)
        if selected == "break" or self._blueprint_source is None:
            self._blueprint_drag_start = None
            return selected
        self._blueprint_drag_start = (
            event.x,
            event.y,
            self._blueprint_drag_x_m,
            self._blueprint_drag_y_m,
            project.scale if (project := self._projection_for_canvas(canvas)) is not None else 1.0,
        )
        return "break"

    def _drag_blueprint(self, event: tk.Event[tk.Misc]) -> str | None:
        start = self._blueprint_drag_start
        if start is None or start[4] <= 0.0:
            return None
        self._blueprint_drag_x_m = start[2] + (event.x - start[0]) / start[4]
        self._blueprint_drag_y_m = start[3] - (event.y - start[1]) / start[4]
        self.redraw()
        return "break"

    def _end_blueprint_drag(self, _event: tk.Event[tk.Misc]) -> str | None:
        if self._blueprint_drag_start is None:
            return None
        self._blueprint_drag_start = None
        return "break"

    def _delete_selected_click(self, _event: tk.Event[tk.Misc]) -> str | None:
        state = self.diagnostic_state
        if state is None or state.identity is None:
            return None
        self._on_click_deleted(state.identity)
        return "break"

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

    def _load_blueprint(self) -> None:
        selected = filedialog.askopenfilename(
            parent=self.winfo_toplevel(),
            title="Load workplace blueprint",
            filetypes=(
                ("Blueprint images", "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff"),
                ("All files", "*"),
            ),
        )
        if not selected:
            return
        path = Path(selected)
        try:
            with Image.open(path) as image:
                image.load()
                source = image.convert("RGB")
        except (OSError, UnidentifiedImageError) as exc:
            self.blueprint_status_var.set(f"Could not load blueprint: {exc}")
            return
        source.thumbnail((4096, 4096), Image.Resampling.LANCZOS)
        source = ImageEnhance.Color(source).enhance(0.78)
        self._blueprint_source = ImageEnhance.Brightness(source).enhance(0.46)
        self._blueprint_path = path
        try:
            width_m, _height_m = parse_blueprint_dimensions_m(
                self.blueprint_width_var.get(),
                self.blueprint_height_var.get(),
            )
        except ValueError:
            width_m = 10.0
        self._blueprint_width_m = width_m
        self._blueprint_height_m = width_m * source.height / source.width
        self.blueprint_width_var.set(f"{self._blueprint_width_m:.3f}")
        self.blueprint_height_var.set(f"{self._blueprint_height_m:.3f}")
        self._blueprint_render_cache.clear()
        self.blueprint_status_var.set(
            f"{path.name} · aspect-ratio size applied; enter exact plan dimensions."
        )
        self._sync_blueprint_controls()
        self.redraw()

    def _apply_blueprint_size(self) -> None:
        if self._blueprint_source is None:
            return
        try:
            width_m, height_m = parse_blueprint_dimensions_m(
                self.blueprint_width_var.get(),
                self.blueprint_height_var.get(),
            )
        except ValueError as exc:
            self.blueprint_status_var.set(str(exc))
            return
        self._blueprint_width_m = width_m
        self._blueprint_height_m = height_m
        name = self._blueprint_path.name if self._blueprint_path is not None else "Blueprint"
        self.blueprint_status_var.set(
            f"{name} · exact footprint {width_m:.3f} × {height_m:.3f} m"
        )
        self.redraw()

    def _clear_blueprint(self) -> None:
        self._blueprint_source = None
        self._blueprint_path = None
        self._blueprint_render_cache.clear()
        self._blueprint_photo_by_canvas.clear()
        self.blueprint_status_var.set(
            "Load a raster blueprint, then enter its exact size in metres."
        )
        self._sync_blueprint_controls()
        self.redraw()

    def _projection_for_canvas(self, canvas: tk.Canvas):
        if not self.positions:
            return None
        points = list((self.reference_positions or self.positions).values())
        if self._blueprint_source is not None:
            blueprint_x = self._registration_translate_x_m + self._blueprint_drag_x_m
            blueprint_y = self._registration_translate_y_m + self._blueprint_drag_y_m
            points.extend(
                (
                    (blueprint_x, blueprint_y),
                    (blueprint_x + self._blueprint_width_m * self._registration_scale, blueprint_y),
                    (blueprint_x, blueprint_y + self._blueprint_height_m * self._registration_scale),
                    (blueprint_x + self._blueprint_width_m * self._registration_scale,
                     blueprint_y + self._blueprint_height_m * self._registration_scale),
                )
            )
        points.append((0.0, 0.0))
        return _projector(points, canvas.winfo_width(), canvas.winfo_height())

    def _select_anchor_at(
        self,
        canvas: tk.Canvas,
        event: tk.Event[tk.Misc],
    ) -> str | None:
        project = self._projection_for_canvas(canvas)
        if project is None:
            return None
        closest: tuple[float, str] | None = None
        for anchor_id, point in self.positions.items():
            x, y = project(*point)
            distance = math.hypot(float(event.x) - x, float(event.y) - y)
            if closest is None or distance < closest[0]:
                closest = distance, anchor_id
        self._selected_anchor_id = (
            closest[1] if closest is not None and closest[0] <= 14.0 else None
        )
        self.selection_var.set(
            f"…{self._selected_anchor_id[-4:]}: incident connections highlighted"
            if self._selected_anchor_id is not None
            else "Click an anchor to isolate its connections."
        )
        self.redraw()
        return "break" if self._selected_anchor_id is not None else None

    def _draw_blueprint(self, canvas: tk.Canvas, project: Callable[[float, float], tuple[float, float]]) -> None:
        source = self._blueprint_source
        if source is None:
            self._blueprint_photo_by_canvas.pop(id(canvas), None)
            return
        blueprint_x = self._registration_translate_x_m + self._blueprint_drag_x_m
        blueprint_y = self._registration_translate_y_m + self._blueprint_drag_y_m
        left, bottom = project(blueprint_x, blueprint_y)
        right, top = project(
            blueprint_x + self._blueprint_width_m * self._registration_scale,
            blueprint_y + self._blueprint_height_m * self._registration_scale,
        )
        x = min(left, right)
        y = min(top, bottom)
        width_px = max(1, round(abs(right - left)))
        height_px = max(1, round(abs(bottom - top)))
        cache_key = (width_px, height_px)
        rendered = self._blueprint_render_cache.get(cache_key)
        if rendered is None:
            rendered = source.resize(cache_key, Image.Resampling.BILINEAR)
            self._blueprint_render_cache[cache_key] = rendered
            while len(self._blueprint_render_cache) > 6:
                self._blueprint_render_cache.pop(next(iter(self._blueprint_render_cache)))
        photo = ImageTk.PhotoImage(rendered, master=canvas)
        self._blueprint_photo_by_canvas[id(canvas)] = photo
        canvas.create_image(x, y, image=photo, anchor="nw")
        canvas.create_rectangle(
            x,
            y,
            x + width_px,
            y + height_px,
            outline=BORDER,
            width=2,
        )
        canvas.create_text(
            x + 8,
            y + 8,
            text=(
                f"PLAN  {self._blueprint_width_m:.2f} × {self._blueprint_height_m:.2f} m"
                "  · drag background to align"
            ),
            fill=ACCENT,
            anchor="nw",
            font=("TkFixedFont", 8, "bold"),
        )

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
        project = self._projection_for_canvas(canvas)
        assert project is not None
        self._draw_blueprint(canvas, project)
        origin_x, origin_y = project(0.0, 0.0)
        canvas.create_line(
            0,
            origin_y,
            canvas.winfo_width(),
            origin_y,
            fill=GRID,
            dash=(3, 5),
        )
        canvas.create_line(
            origin_x,
            0,
            origin_x,
            canvas.winfo_height(),
            fill=GRID,
            dash=(3, 5),
        )
        state = self.diagnostic_state
        for anchor_a, anchor_b in sorted(self.connections):
            if anchor_a not in self.positions or anchor_b not in self.positions:
                continue
            ax, ay = project(*self.positions[anchor_a])
            bx, by = project(*self.positions[anchor_b])
            incident = (
                self._selected_anchor_id is not None
                and self._selected_anchor_id in (anchor_a, anchor_b)
            )
            canvas.create_line(
                ax,
                ay,
                bx,
                by,
                fill=ACCENT if incident else GRID if self._selected_anchor_id else CONNECTION,
                width=3 if incident else 1,
                dash=() if incident else (2, 5),
            )
        if state:
            for anchor_id, radius in state.ranges_m.items():
                if anchor_id not in self.positions:
                    continue
                x, y = project(*self.positions[anchor_id])
                scale = project.scale
                canvas.create_oval(x - radius * scale, y - radius * scale, x + radius * scale, y + radius * scale, outline=ACCENT, dash=(4, 4))
        for anchor_id, point in sorted(self.positions.items()):
            x, y = project(*point)
            selected = anchor_id == self._selected_anchor_id
            canvas.create_oval(
                x - 7,
                y - 7,
                x + 7,
                y + 7,
                fill=BLUE,
                outline=MAGENTA if selected else CANVAS_BG,
                width=3 if selected else 1,
            )
            canvas.create_text(
                x + 10,
                y - 8,
                text=f"…{anchor_id[-4:]}",
                anchor="sw",
                fill=INK,
            )
        for click_number, click_state in enumerate(self.click_states, 1):
            if click_state.result is None:
                continue
            x, y = project(click_state.result.x_m, click_state.result.y_m)
            selected = state is not None and click_state.identity == state.identity
            color = (
                MAGENTA
                if click_state.result.algorithm == "height_agnostic_range_ls"
                else ACCENT
            )
            canvas.create_oval(
                x - 12,
                y - 12,
                x + 12,
                y + 12,
                fill=color,
                outline=INK if selected else CANVAS_BG,
                width=3 if selected else 1,
            )
            canvas.create_text(
                x,
                y,
                text=str(click_number),
                fill=INK,
                font=("TkDefaultFont", 8, "bold"),
            )

    def _open_fullscreen(self) -> None:
        window = self._fullscreen_window
        if window is not None and window.winfo_exists():
            window.lift()
            window.focus_force()
            return
        window = tk.Toplevel(self)
        self._fullscreen_window = window
        window.title("IMEC2 Click Location — Fullscreen")
        window.configure(background=PANEL_BG)
        window.columnconfigure(0, weight=1)
        window.rowconfigure(3, weight=1)

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

        fullscreen_blueprint_controls = self._make_blueprint_controls(window)
        fullscreen_blueprint_controls.grid(
            row=1,
            column=0,
            sticky="ew",
            padx=8,
            pady=(2, 4),
        )
        self._fullscreen_registration_controls = LayoutRegistrationControls(
            window,
            on_translate=self._on_translate,
            on_scale=self._on_scale,
            on_reset=self._on_reset,
        )
        self._fullscreen_registration_controls.grid(
            row=2,
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
        self._sync_blueprint_controls()
        canvas = tk.Canvas(window, background=CANVAS_BG, highlightthickness=0)
        self._fullscreen_canvas = canvas
        canvas.grid(row=3, column=0, sticky="nsew")
        canvas.bind("<Configure>", lambda _event: self.redraw())
        canvas.bind(
            "<ButtonPress-1>",
            lambda event, source=canvas: self._press_at(source, event),  # type: ignore[misc]
        )
        canvas.bind("<B1-Motion>", self._drag_blueprint)
        canvas.bind("<ButtonRelease-1>", self._end_blueprint_drag)
        window.bind("<KeyPress>", self._fullscreen_key_pressed)
        window.bind("<KeyRelease>", self._fullscreen_key_released)
        window.bind("<Escape>", lambda _event: self._close_fullscreen())
        window.bind("<F11>", lambda _event: self._close_fullscreen())
        window.bind("<Delete>", self._delete_selected_click)
        window.bind("<BackSpace>", self._delete_selected_click)
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
        canvas = self._fullscreen_canvas
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
        if canvas is not None:
            self._blueprint_photo_by_canvas.pop(id(canvas), None)
        self._blueprint_controls = [
            controls for controls in self._blueprint_controls
            if controls.winfo_exists()
        ]


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
    render_width = max(width, 160)
    render_height = max(height, 80)
    span_x = max(max_x - min_x, 1.0)
    span_y = max(max_y - min_y, 1.0)
    horizontal_padding = min(80.0, max(24.0, render_width * 0.12))
    vertical_padding = min(70.0, max(24.0, render_height * 0.22))
    scale = max(
        min(
            (render_width - horizontal_padding) / span_x,
            (render_height - vertical_padding) / span_y,
        ),
        1e-6,
    )
    offset_x = (render_width - span_x * scale) / 2.0
    offset_y = (render_height - span_y * scale) / 2.0
    def project(x: float, y: float) -> tuple[float, float]:
        return (
            offset_x + (x - min_x) * scale,
            render_height - offset_y - (y - min_y) * scale,
        )
    project.scale = scale  # type: ignore[attr-defined]
    return project
