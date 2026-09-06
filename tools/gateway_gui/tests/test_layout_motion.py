import math
import unittest
from unittest.mock import patch

from tools.gateway_gui.anchor_geometry import rotate_layout
from tools.gateway_gui.diagnostic_models import ClickLocationModel
from tools.gateway_gui.layout_motion import rigid_motion, rotation_safe_bounds
from tools.gateway_gui.tests.test_diagnostic_models import click


class LayoutMotionTests(unittest.TestCase):
    def test_rotation_does_not_change_view_bounds(self):
        points = {"a": (0., 0.), "b": (9., 0.), "c": (1., 2.)}
        before = rotation_safe_bounds(points)
        after = rotation_safe_bounds(rotate_layout(points, 37.5))
        for a, b in zip(before, after):
            self.assertAlmostEqual(a[0], b[0])
            self.assertAlmostEqual(a[1], b[1])

    def test_nonrigid_edits_do_not_use_fast_path(self):
        points = {"a": (0., 0.), "b": (5., 0.), "c": (0., 4.)}
        self.assertIsNone(rigid_motion(points, {**points, "c": (0., 4.1)}))
        self.assertIsNone(rigid_motion(points, {key: (2*x, 2*y) for key, (x,y) in points.items()}))

    def test_retained_clicks_rotate_without_rerunning_solver(self):
        points = {f"0x{i:016x}": p for i, p in enumerate(((0., 0.), (5., 0.), (0., 4.)), 1)}
        model = ClickLocationModel()
        model.set_geometry(points, 1)
        target = (2., 1.5)
        for i, point in enumerate(points.values(), 1):
            model.observe(click(i, math.dist(target, point), sequence=i))
        original = model.state.result
        transformed = {key: (-y+10, x-3) for key, (x, y) in points.items()}
        with patch('tools.gateway_gui.diagnostic_models.solve_position', side_effect=AssertionError("unnecessary solve")):
            result = model.set_geometry(transformed, 1).result
        self.assertAlmostEqual(result.x_m, -original.y_m+10)
        self.assertAlmostEqual(result.y_m, original.x_m-3)
        self.assertEqual(result.rmse_m, original.rmse_m)
        self.assertEqual(result.range_residuals_m, original.range_residuals_m)
        for reading in result.processed_readings:
            self.assertEqual((reading.x_m, reading.y_m), transformed[reading.anchor_id])

    def test_individual_anchor_edit_recalculates_click(self):
        from tools.gateway_gui.localization import solve_position
        points = {f"0x{i:016x}": p for i, p in enumerate(((0., 0.), (5., 0.), (0., 4.)), 1)}
        model = ClickLocationModel()
        model.set_geometry(points, 1)
        for i, point in enumerate(points.values(), 1):
            model.observe(click(i, math.dist((2., 1.5), point), sequence=i))
        previous = model.state.result
        changed = {**points, "0x0000000000000003": (1., 4.)}
        with patch('tools.gateway_gui.diagnostic_models.solve_position', wraps=solve_position) as solve:
            result = model.set_geometry(changed, 1).result
        solve.assert_called_once()
        self.assertNotEqual((result.x_m, result.y_m), (previous.x_m, previous.y_m))
