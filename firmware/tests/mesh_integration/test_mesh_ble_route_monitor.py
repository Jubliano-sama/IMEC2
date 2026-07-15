#!/usr/bin/env python3

import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


try:
    import bleak  # noqa: F401
except ImportError:
    bleak_module = types.ModuleType("bleak")
    bleak_backends = types.ModuleType("bleak.backends")
    bleak_device = types.ModuleType("bleak.backends.device")

    class BleStub:
        pass

    bleak_module.BleakClient = BleStub
    bleak_module.BleakScanner = BleStub
    bleak_device.BLEDevice = BleStub
    sys.modules["bleak"] = bleak_module
    sys.modules["bleak.backends"] = bleak_backends
    sys.modules["bleak.backends.device"] = bleak_device


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "tools" / "mesh_ble_route_monitor.py"
SPEC = importlib.util.spec_from_file_location("mesh_ble_route_monitor", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
monitor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = monitor
SPEC.loader.exec_module(monitor)


def synthetic_packet(source_id: int, packet_id: int, *, event_seq: int | None = None):
    tlvs = {"mesh_test_packet_id": packet_id}
    if event_seq is not None:
        tlvs = {"event_seq": event_seq}
    return monitor.ProtoPacket(
        msg_type=0x30,
        flags=0,
        src_id=source_id,
        dst_id=0x9000,
        session_id=packet_id + 100,
        seq=packet_id & 0xFFFF,
        ttl=4,
        message_age_ms=0,
        payload=b"",
        tlvs=tlvs,
        raw_tlvs={},
    )


def synthetic_stream_record(source_id: int, packet_id: int) -> bytes:
    payload = bytes((0x59, 4)) + packet_id.to_bytes(4, "little")
    record = bytearray(monitor.GATEWAY_STREAM_RECORD_HEADER_LEN + len(payload))
    record[0:2] = monitor.GATEWAY_STREAM_MAGIC.to_bytes(2, "little")
    record[2] = monitor.GATEWAY_STREAM_VERSION
    record[3] = monitor.GATEWAY_STREAM_RECORD_HEADER_LEN
    record[4] = monitor.GATEWAY_STREAM_RECORD_PACKET
    record[8] = 0x30
    record[10:12] = (packet_id & 0xFFFF).to_bytes(2, "little")
    record[12:16] = (packet_id + 100).to_bytes(4, "little")
    record[16:24] = source_id.to_bytes(8, "little")
    record[24:32] = (0x9000).to_bytes(8, "little")
    record[36:38] = len(payload).to_bytes(2, "little")
    record[38:40] = monitor.crc16_ccitt_false(payload).to_bytes(2, "little")
    record[monitor.GATEWAY_STREAM_RECORD_HEADER_LEN:] = payload
    return bytes(record)


class SequenceTrackingTests(unittest.TestCase):
    def test_interleaved_sources_do_not_create_false_gaps(self):
        stats = monitor.MonitorStats()

        monitor.format_packet_line(synthetic_packet(0xA1, 1), stats, 0.0)
        monitor.format_packet_line(synthetic_packet(0xB2, 100), stats, 0.1)
        monitor.format_packet_line(synthetic_packet(0xA1, 2), stats, 0.2)
        monitor.format_packet_line(synthetic_packet(0xB2, 101), stats, 0.3)

        self.assertEqual(stats.gap_events, 0)
        self.assertEqual(stats.missing_packets, 0)
        self.assertEqual(len(stats.last_packet_ids), 2)

    def test_gap_and_reset_are_scoped_to_one_source_and_event_class(self):
        stats = monitor.MonitorStats()

        monitor.format_packet_line(synthetic_packet(0xA1, 4), stats, 0.0)
        line = monitor.format_packet_line(synthetic_packet(0xA1, 7), stats, 0.1)
        monitor.format_packet_line(
            synthetic_packet(0xA1, 0, event_seq=50), stats, 0.2)
        reset_line = monitor.format_packet_line(
            synthetic_packet(0xA1, 1), stats, 0.3)

        self.assertIn("gap=2", line)
        self.assertEqual(stats.gap_events, 1)
        self.assertEqual(stats.missing_packets, 2)
        self.assertIn("gap=stream-reset", reset_line)
        self.assertEqual(stats.sequence_resets, 1)
        self.assertEqual(len(stats.last_packet_ids), 2)

    def test_out_of_order_packet_does_not_move_high_watermark_backward(self):
        stats = monitor.MonitorStats()

        monitor.format_packet_line(synthetic_packet(0xA1, 10), stats, 0.0)
        late_line = monitor.format_packet_line(
            synthetic_packet(0xA1, 9), stats, 0.1)
        next_line = monitor.format_packet_line(
            synthetic_packet(0xA1, 11), stats, 0.2)

        self.assertIn("gap=out-of-order prev=10", late_line)
        self.assertIn("gap=0", next_line)
        self.assertEqual(stats.gap_events, 0)
        self.assertEqual(stats.missing_packets, 0)
        self.assertEqual(
            stats.last_packet_ids[(0xA1, "mesh_test_packet_id")], 11)


class DurableCaptureTests(unittest.TestCase):
    def test_jsonl_append_keeps_complete_independent_records(self):
        packet_a = synthetic_packet(0xA1, 1)
        packet_b = synthetic_packet(0xB2, 9)

        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "mesh.jsonl"
            fd = os.open(path, os.O_APPEND | os.O_CREAT | os.O_WRONLY, 0o600)
            try:
                monitor.append_jsonl_record(
                    fd,
                    monitor.packet_json_record(packet_a, 0.25, 1_000_000_000),
                    False)
                monitor.append_jsonl_record(
                    fd, monitor.packet_json_record(packet_b, 0.50), True)
            finally:
                os.close(fd)

            records = [json.loads(line) for line in path.read_text().splitlines()]

        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["src_id"], "0x00000000000000a1")
        self.assertEqual(records[1]["src_id"], "0x00000000000000b2")
        self.assertEqual(records[0]["sequence_class"], "mesh_test_packet_id")
        self.assertEqual(records[0]["observed_unix_ns"], 1_000_000_000)

    def test_removed_ble_log_characteristic_is_not_advertised(self):
        source = MODULE_PATH.read_text()

        self.assertNotIn("LOG_TX_UUID", source)
        self.assertNotIn("--log-device", source)
        self.assertNotIn("--gateway-logs", source)


class MonitorExitStatusTests(unittest.IsolatedAsyncioTestCase):
    async def test_list_devices_empty_scan_succeeds(self):
        output = io.StringIO()

        with mock.patch.object(
                monitor.BleakScanner,
                "discover",
                new=mock.AsyncMock(return_value={}),
                create=True), contextlib.redirect_stdout(output):
            status = await monitor.list_devices(0.01)

        self.assertEqual(status, 0)
        self.assertEqual(output.getvalue().strip(), "<none>")

    async def test_capture_error_makes_run_fail(self):
        record = synthetic_stream_record(0xA1, 1)

        class FakeClient:
            def __init__(self, _target, timeout):
                self.timeout = timeout

            async def connect(self, **_kwargs):
                return True

            async def start_notify(self, _uuid, callback):
                callback(None, bytearray(record))

            async def disconnect(self):
                return None

        args = types.SimpleNamespace(
            list_devices=False,
            scan_timeout=0.01,
            gateway="gateway-test",
            no_service_filter=False,
            jsonl_file=None,
            connect_timeout=0.1,
            verbose=False,
            include_all_mesh_data=False,
            jsonl_fsync=False,
            count=1,
            duration_s=None,
            jsonl=False,
        )
        gateway = monitor.ResolvedDevice(address="00:11:22:33:44:55")

        with tempfile.TemporaryDirectory() as temp_dir:
            args.jsonl_file = str(Path(temp_dir) / "capture.jsonl")
            with mock.patch.object(
                    monitor,
                    "find_device",
                    new=mock.AsyncMock(return_value=gateway)), \
                    mock.patch.object(monitor, "BleakClient", FakeClient), \
                    mock.patch.object(
                        monitor,
                        "append_jsonl_record",
                        side_effect=OSError("simulated disk failure")), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                status = await monitor.run(args)

        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
