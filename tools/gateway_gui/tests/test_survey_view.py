from __future__ import annotations

import math
import unittest

from tools.gateway_gui.survey_view import (
    CanvasProjection,
    held_translation_delta,
    inverse_transform_point,
    node_error_color,
    node_mean_absolute_errors,
    parse_nearest_anchor_count,
    parse_neighbor_interval_m,
    parse_neighbor_max_m,
    transform_layout,
)
from tools.gateway_gui.anchor_geometry import AnchorPairDistance


class SurveyLayoutTransformTests(unittest.TestCase):
    def test_canvas_projection_and_registration_inverse_round_trip(self) -> None:
        projection = CanvasProjection(
            min_x=-2.0,
            min_y=-3.0,
            scale=40.0,
            offset_x=25.0,
            offset_y=30.0,
            height=700.0,
        )
        displayed = (12.5, -7.25)
        canvas_point = projection.project(*displayed)
        recovered_displayed = projection.unproject(*canvas_point)
        self.assertAlmostEqual(recovered_displayed[0], displayed[0])
        self.assertAlmostEqual(recovered_displayed[1], displayed[1])

        solver_point = inverse_transform_point(
            displayed,
            scale=2.5,
            translate_x_m=5.0,
            translate_y_m=-1.0,
        )
        self.assertEqual(solver_point, (3.0, -2.5))

    def test_nearest_anchor_count_uses_zero_for_all(self) -> None:
        self.assertEqual(parse_nearest_anchor_count("0"), 0)
        self.assertEqual(parse_nearest_anchor_count("4"), 4)
        for invalid in ("", "1.5", "-1", "four"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parse_nearest_anchor_count(invalid)

    def test_neighbor_maximum_parser_accepts_metres_at_or_above_floor(self) -> None:
        self.assertEqual(parse_neighbor_max_m("7"), 7.0)
        self.assertEqual(parse_neighbor_max_m("18.5"), 18.5)
        for invalid in ("", "six", "6.99", "inf", "nan"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parse_neighbor_max_m(invalid)

    def test_radio_interval_parser_accepts_adjustable_minimum(self) -> None:
        self.assertEqual(parse_neighbor_interval_m("5.5", "18"), (5.5, 18.0))
        for minimum, maximum in (
            ("0", "15"),
            ("-1", "15"),
            ("16", "15"),
            ("nan", "15"),
            ("7", "inf"),
        ):
            with self.subTest(minimum=minimum, maximum=maximum), self.assertRaises(
                ValueError
            ):
                parse_neighbor_interval_m(minimum, maximum)

    def test_held_wasd_supports_continuous_and_diagonal_movement(self) -> None:
        self.assertEqual(held_translation_delta({"w", "d"}), (0.1, 0.1))
        self.assertEqual(held_translation_delta({"a", "s"}), (-0.1, -0.1))
        self.assertEqual(held_translation_delta({"a", "d"}), (0.0, 0.0))

    def test_node_error_colors_use_a_fixed_one_metre_scale(self) -> None:
        self.assertEqual(node_error_color(0.0), "#2e9d50")
        self.assertEqual(node_error_color(0.5), "#e2b93b")
        self.assertEqual(node_error_color(1.0), "#c83b3b")
        self.assertEqual(node_error_color(1.5), "#c83b3b")

    def test_node_errors_average_absolute_residuals_across_connections(self) -> None:
        pairs = (
            AnchorPairDistance("A", "B", 1.0),
            AnchorPairDistance("A", "C", 1.0),
            AnchorPairDistance("B", "C", 1.0),
            AnchorPairDistance("C", "D", 1.0),
        )

        errors = node_mean_absolute_errors(
            pairs,
            {"A-B": -0.25, "C-A": 0.75, "B-C": 1.5},
        )

        self.assertEqual(errors, {"A": 0.5, "B": 0.875, "C": 1.125})

    def test_scale_and_translation_register_solved_coordinates(self) -> None:
        transformed = transform_layout(
            {"A": (1.0, -2.0), "B": (3.5, 4.0)},
            scale=2.0,
            translate_x_m=10.0,
            translate_y_m=-5.0,
        )

        self.assertEqual(transformed["A"], (12.0, -9.0))
        self.assertEqual(transformed["B"], (17.0, 3.0))
        self.assertEqual(
            transformed["B"][0] - transformed["A"][0],
            2.0 * (3.5 - 1.0),
        )
        self.assertEqual(
            transformed["B"][1] - transformed["A"][1],
            2.0 * (4.0 - -2.0),
        )

    def test_transform_rejects_nonpositive_and_nonfinite_values(self) -> None:
        positions = {"A": (0.0, 0.0)}
        invalid = (
            {"scale": 0.0, "translate_x_m": 0.0, "translate_y_m": 0.0},
            {"scale": -1.0, "translate_x_m": 0.0, "translate_y_m": 0.0},
            {"scale": 1.0, "translate_x_m": math.inf, "translate_y_m": 0.0},
        )
        for values in invalid:
            with self.subTest(values=values), self.assertRaises(ValueError):
                transform_layout(positions, **values)


if __name__ == "__main__":
    unittest.main()
