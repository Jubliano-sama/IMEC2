"""Exercise the real lock controls and their shared solver/display frame."""

from dataclasses import replace
from itertools import combinations
import math
from types import SimpleNamespace
import tkinter as tk
import unittest
from unittest.mock import Mock, patch

from tools.gateway_gui.anchor_geometry import AnchorPairDistance, evaluate_anchor_layout
from tools.gateway_gui.anchor_geometry_nlos import NLOS_ONE_SIDED_ALGORITHM
from tools.gateway_gui.diagnostic_models import refine_geometry
from tools.gateway_gui.survey_runtime import SurveyOperationModel, anchor_label
from tools.gateway_gui.survey_view import RADIO_INTERVAL_SOLVERS, SurveyGeometryView, _canvas_projection


class SurveyLockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.root = tk.Tk()
            cls.root.withdraw()
        except tk.TclError as exc:
            raise unittest.SkipTest(f"Tk display unavailable: {exc}") from exc

    @classmethod
    def tearDownClass(cls):
        cls.root.destroy()

    def setUp(self):
        self.changed = Mock()
        self.edited = Mock()
        self.view = SurveyGeometryView(
            self.root, on_positions_changed=self.changed, on_layout_edited=self.edited,
        )
        self.model = SurveyOperationModel()
        self.model.slot_to_anchor = {i: i + 1 for i in range(4)}
        self.positions = dict(zip(
            (anchor_label(i + 1) for i in range(4)),
            ((0.0, 0.0), (4.0, 0.0), (0.0, 3.0), (3.0, 4.0)),
        ))
        self.pairs = tuple(
            AnchorPairDistance(a, b, math.dist(self.positions[a], self.positions[b]))
            for a, b in combinations(self.positions, 2)
        )
        self.model._solve_base_pairs = {
            (pair.anchor_a_id, pair.anchor_b_id): pair for pair in self.pairs
        }
        self.model.layout = evaluate_anchor_layout(self.pairs, self.positions)
        self.view.show_model(self.model)
        self.anchor = anchor_label(1)

    def tearDown(self):
        self.view._close_fullscreen()
        self.view.destroy()

    def anchor_event(self, key):
        view = self.view
        projection = _canvas_projection(
            (*view.registration.reference_positions_m.values(), (0.0, 0.0)),
            max(view.canvas.winfo_width(), 160), max(view.canvas.winfo_height(), 80),
        )
        x, y = projection.project(*view.registration.positions_m[key])
        return SimpleNamespace(x=x, y=y)

    def test_solver_selection_restores_independent_weight_settings(self):
        view = self.view
        self.assertIn(NLOS_ONE_SIDED_ALGORITHM, view.solver_combo.cget("values"))
        self.assertIn(NLOS_ONE_SIDED_ALGORITHM, RADIO_INTERVAL_SOLVERS)
        original = view.solver_var.get()
        self.assertEqual(view.distance_weight_power, 0)
        view.distance_weight_power_var.set("2")
        view.solver_var.set(NLOS_ONE_SIDED_ALGORITHM)
        self.assertEqual(view.distance_weight_power, 1)
        view.distance_weight_power_var.set("0.5")
        view.solver_var.set(original)
        self.assertEqual(view.distance_weight_power, 2)
        view.solver_var.set(NLOS_ONE_SIDED_ALGORITHM)
        self.assertEqual(view.distance_weight_power, 0.5)

    def test_fit_details_exposes_solver_warnings_in_both_views(self):
        self.model.layout = replace(self.model.layout, warnings=(
            "Near-fit ranges constrain 4/5 shape dimensions; result depends on relaxed ranges or radio bounds.",
        ))
        self.view.show_model(self.model)
        self.view._open_fullscreen()
        with patch("tools.gateway_gui.survey_view.messagebox.showinfo") as show:
            for button in self.view._fit_details_buttons:
                button.invoke()
                self.assertIn("4/5 shape dimensions", show.call_args.args[1])
        self.view._close_fullscreen()
        self.assertEqual(len(self.view._fit_details_buttons), 1)

    def test_select_lock_block_drag_then_unlock_and_drag(self):
        view = self.view
        event = self.anchor_event(self.anchor)
        view._anchor_drag_started(view.canvas, event)
        view._anchor_drag_finished(view.canvas, event)
        view._anchor_lock_buttons[0].invoke()
        self.assertEqual(view.locked_positions_m, {self.anchor: self.positions[self.anchor]})
        self.assertEqual(view._anchor_lock_buttons[0].cget("text"), "Unlock selected")
        self.assertTrue(any(
            "[locked]" in view.canvas.itemcget(item, "text")
            for item in view.canvas.find_all() if view.canvas.type(item) == "text"
        ))
        moved = SimpleNamespace(x=event.x + 20, y=event.y + 10)
        view._anchor_drag_started(view.canvas, event)
        view._anchor_dragged(view.canvas, moved)
        view._anchor_drag_finished(view.canvas, moved)
        self.assertEqual(view.registration.positions_m, self.positions)
        self.edited.assert_not_called()

        view._anchor_lock_buttons[0].invoke()
        view._anchor_drag_started(view.canvas, event)
        view._anchor_drag_finished(view.canvas, moved)
        self.assertNotEqual(view.registration.positions_m[self.anchor], self.positions[self.anchor])
        self.edited.assert_called_once()

    def test_locks_follow_frame_transforms_and_stay_put_through_refinement(self):
        view = self.view
        for key in list(self.positions)[:3]:
            view._select_anchor(key)
            view._toggle_anchor_lock()
        view._rotate(37)
        view._mirror()
        view.nudge_scale(1.5)
        view.nudge_translation(2.0, -1.0)
        before = view.registration
        fixed = view.locked_positions_m
        for key in fixed:
            self.assertEqual(fixed[key], before.reference_positions_m[key])
        self.changed.assert_called_with(before)

        self.model.layout = refine_geometry(
            self.pairs, before.reference_positions_m, fixed_positions_m=fixed,
        )
        view.show_model(self.model)
        after = view.registration
        for key in fixed:
            self.assertEqual(after.positions_m[key], before.positions_m[key])
        self.assertEqual((after.scale, after.translate_x_m, after.translate_y_m), (1.5, 2.0, -1.0))

        # Fresh survey telemetry can temporarily clear the layout; locks remain
        # bound to hardware identities until explicitly unlocked.
        solved = self.model.layout
        self.model.layout = None
        self.model.run_serial += 1
        view.show_model(self.model)
        self.assertEqual(view.locked_positions_m, fixed)
        self.model.layout = replace(solved)
        view.show_model(self.model)
        for key in fixed:
            self.assertEqual(view.registration.positions_m[key], before.positions_m[key])

        view.reset_transform()
        self.assertEqual(view.registration.scale, 1.0)
        self.assertEqual(view.locked_positions_m, fixed)
        view._unlock_all_anchors()
        self.assertFalse(view.locked_positions_m)

    def test_pending_solve_freezes_lock_edits_and_frame_controls_in_both_views(self):
        view = self.view
        view._select_anchor(self.anchor)
        view._toggle_anchor_lock()
        view._open_fullscreen()
        view._select_anchor(self.anchor)
        self.assertEqual(len(view._anchor_lock_buttons), 2)
        for button in view._anchor_lock_buttons:
            self.assertEqual(button.cget("text"), "Unlock selected")
        before = view.registration
        fixed = view.locked_positions_m
        view.set_geometry_job_pending(True)
        for button in (*view._anchor_lock_buttons, *view._anchor_unlock_all_buttons, view.mirror_button):
            self.assertEqual(str(button.cget("state")), "disabled")
        view._toggle_anchor_lock()
        view._unlock_all_anchors()
        view._rotate(90)
        view._mirror()
        view.nudge_scale(2)
        view.nudge_translation(1, 1)
        view.reset_transform()
        self.assertEqual(view.registration, before)
        self.assertEqual(view.locked_positions_m, fixed)
        view.set_geometry_job_pending(False)
        view._anchor_lock_buttons[1].invoke()
        self.assertFalse(view.locked_positions_m)
        self.assertEqual(view._anchor_lock_buttons[0].cget("text"), "Lock selected")
        view._close_fullscreen()
        self.assertEqual(len(view._anchor_lock_buttons), 1)
