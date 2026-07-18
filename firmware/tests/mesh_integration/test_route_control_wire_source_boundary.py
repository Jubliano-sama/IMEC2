#!/usr/bin/env python3

from pathlib import Path
import unittest

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
APP_SRC = ROOT / "app" / "src"


class RouteControlWireSourceBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = read_composed_source(APP_SRC / "app_mesh_report.c")

    def test_mandatory_ancestry_selects_standalone_route_request(self):
        self.assertIn(
            "BUILD_ASSERT(MESH_ROUTE_DISCOVERY_MIN_PAYLOAD_LEN >\n"
            "             MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES,",
            self.report,
        )
        self.assertIn(
            "embedded_route_req->payload_len <= "
            "MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES",
            self.report,
        )

    def test_failed_embedded_suffix_keeps_post_wake_fallback(self):
        self.assertIn("bool embedded_route_candidate = false;", self.report)
        self.assertIn(
            "embedded_route_frame = mesh_queue_embedded_route_request(",
            self.report,
        )
        self.assertNotIn(
            "(void)mesh_queue_embedded_route_request(",
            self.report,
        )
        queue_index = self.report.index(
            "embedded_route_frame = mesh_queue_embedded_route_request("
        )
        contact_index = self.report.index(
            "mesh_c5_contact_accept(", queue_index
        )
        self.assertLess(queue_index, contact_index)
        self.assertIn("if (!embedded_route_frame) {", self.report)
        self.assertIn("mesh_listen_for_route_reply(", self.report)


if __name__ == "__main__":
    unittest.main()
