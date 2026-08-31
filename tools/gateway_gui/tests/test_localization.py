import math
import unittest

from tools.gateway_gui.localization import LocalizationReading, solve_position


class LocalizationAlgorithmPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        target = (2.0, 1.5)
        anchors = (("A", 0.0, 0.0), ("B", 5.0, 0.0), ("C", 0.0, 4.0))
        self.readings = [
            LocalizationReading(name, x, y, math.dist(target, (x, y)))
            for name, x, y in anchors
        ]

    def test_better_height_agnostic_candidate_is_rejected_inside_hull(self) -> None:
        result = solve_position(self.readings)

        self.assertEqual(result.algorithm, "radical_axis")
        self.assertAlmostEqual(result.x_m, result.seed_x_m)
        self.assertIn("inside the anchor convex hull", " ".join(result.warnings))

    def test_better_height_agnostic_candidate_is_used_outside_hull(self) -> None:
        anchors = (("A", 0.0, 0.0), ("B", 5.0, 0.0), ("C", 5.0, 4.0), ("D", 0.0, 4.0))
        ranges = (4.7495788529, 1.0915225251, 8.4268585833, 3.6877586968)
        readings = [
            LocalizationReading(name, x, y, distance)
            for (name, x, y), distance in zip(anchors, ranges)
        ]
        result = solve_position(readings)

        self.assertEqual(result.algorithm, "height_agnostic_range_ls")
        self.assertLess(result.y_m, 0.0)
        self.assertLess(result.rmse_m, 4.0)


if __name__ == "__main__":
    unittest.main()
