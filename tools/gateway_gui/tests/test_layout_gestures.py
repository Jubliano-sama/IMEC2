"""Real Tk gesture dispatch, including modifier release and existing bindings."""
import tkinter as tk
import unittest
from unittest.mock import Mock

from tools.gateway_gui.layout_gestures import bind_layout_gestures
from tools.gateway_gui.survey_view import LayoutRegistrationControls


class LayoutGestureTests(unittest.TestCase):
    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(str(exc))
        self.canvas = tk.Canvas(self.root, width=400, height=300)
        self.canvas.pack()
        self.root.update()
        self.translate, self.scale, self.rotate = Mock(), Mock(), Mock()
        self.enabled = True
        bind_layout_gestures(
            self.canvas, on_translate=self.translate, on_scale=self.scale,
            on_rotate=self.rotate, pixels_per_metre=lambda: 100,
            enabled=lambda: self.enabled,
        )

    def tearDown(self):
        if hasattr(self, "root"):
            self.root.destroy()

    def test_rotation_is_continuous_and_control_is_fine(self):
        self.canvas.event_generate("<ButtonPress-3>", x=100, y=100)
        self.canvas.event_generate("<B3-Motion>", x=107, y=100)
        self.rotate.assert_called_with(-3.5)
        self.canvas.event_generate("<B3-Motion>", x=114, y=100, state=0x404)
        self.assertAlmostEqual(self.rotate.call_args.args[0], -0.35)
        self.canvas.event_generate("<ButtonRelease-3>", x=114, y=100)
        self.assertIsNone(self.canvas.grab_current())

    def test_shift_release_during_move_does_not_leave_a_grab(self):
        self.canvas.event_generate("<Shift-ButtonPress-1>", x=100, y=100)
        self.canvas.event_generate("<B1-Motion>", x=125, y=80)
        self.translate.assert_called_with(0.25, 0.2)
        self.canvas.event_generate("<ButtonRelease-1>", x=125, y=80)
        self.assertIsNone(self.canvas.grab_current())

    def test_wheel_reverses_and_disabled_gestures_do_nothing(self):
        self.canvas.event_generate("<Button-4>")
        up = self.scale.call_args.args[0]
        self.canvas.event_generate("<Button-5>")
        self.assertAlmostEqual(up * self.scale.call_args.args[0], 1)
        self.enabled = False
        self.scale.reset_mock()
        self.canvas.event_generate("<Button-4>")
        self.canvas.event_generate("<ButtonPress-3>", x=100, y=100)
        self.canvas.event_generate("<B3-Motion>", x=110, y=100)
        self.scale.assert_not_called()
        self.rotate.assert_not_called()

    def test_exact_angle_and_invalid_input(self):
        controls = LayoutRegistrationControls(
            self.root, on_translate=self.translate, on_scale=self.scale,
            on_rotate=self.rotate, on_reset=Mock(),
        )
        controls.set_enabled(True)
        controls.rotation_var.set("12.75")
        controls.buttons[-1].invoke()
        self.rotate.assert_called_once_with(12.75)
        controls.rotation_var.set("nan")
        controls.buttons[-1].invoke()
        self.assertEqual(self.rotate.call_count, 1)
        self.assertIn("finite", controls.status_var.get())

    def test_left_button_tools_take_precedence_over_anchor_selection(self):
        canvas = tk.Canvas(self.root, width=200, height=100)
        canvas.pack()
        self.root.update()
        selected = Mock()
        canvas.bind("<ButtonPress-1>", selected)
        bind_layout_gestures(
            canvas, on_translate=self.translate, on_scale=self.scale,
            on_rotate=self.rotate, pixels_per_metre=lambda: 100,
            enabled=lambda: True, tool=lambda: "rotate",
        )
        canvas.event_generate("<ButtonPress-1>", x=50, y=50)
        canvas.event_generate("<B1-Motion>", x=60, y=50)
        canvas.event_generate("<ButtonRelease-1>", x=60, y=50)
        selected.assert_not_called()
        self.assertAlmostEqual(sum(call.args[0] for call in self.rotate.call_args_list), -5)

    def test_click_anchor_drag_and_lock(self):
        from tools.gateway_gui.diagnostic_views import ClickDiagnosticsView
        moved = Mock()
        locked = set()
        view = ClickDiagnosticsView(
            self.root, on_anchor_moved=moved, anchor_locked=lambda anchor: anchor in locked,
            on_anchor_lock=lambda anchor: locked.add(anchor),
        )
        view.pack(fill="both", expand=True)
        view.positions = {"a": (0., 0.), "b": (5., 0.), "c": (0., 4.)}
        view.reference_positions = dict(view.positions)
        self.root.update()
        project = view._projection_for_canvas(view.canvas)
        x, y = project(*view.positions["a"])
        view.canvas.event_generate("<ButtonPress-1>", x=round(x), y=round(y))
        view.canvas.event_generate("<B1-Motion>", x=round(x)+20, y=round(y)-10)
        view.canvas.event_generate("<ButtonRelease-1>", x=round(x)+20, y=round(y)-10)
        moved.assert_called_once()
        self.assertEqual(moved.call_args.args[0], "a")
        self.assertAlmostEqual(moved.call_args.args[1][0], 20/project.scale)
        view._toggle_selected_lock()
        self.assertIn("a", locked)
        moved.reset_mock()
        x, y = project(*view.positions["a"])
        view.canvas.event_generate("<ButtonPress-1>", x=round(x), y=round(y))
        view.canvas.event_generate("<B1-Motion>", x=round(x)+20, y=round(y)-10)
        view.canvas.event_generate("<ButtonRelease-1>", x=round(x)+20, y=round(y)-10)
        moved.assert_not_called()
        view.destroy()
