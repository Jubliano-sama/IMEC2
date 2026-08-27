#!/usr/bin/env python3
"""Regression tests for the non-resetting clicker RTT HIL attachment gate."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/test_clicker_hil.py"


def load_harness():
    spec = importlib.util.spec_from_file_location("test_clicker_hil_harness", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def time(self) -> float:
        self.now += 0.05
        return self.now

    def sleep(self, duration_s: float) -> None:
        self.now += duration_s


class ClickerHilContractTest(unittest.TestCase):
    @staticmethod
    def ble_process():
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        process.stdout.fileno.return_value = 74
        return process

    def test_rtt_capture_attaches_without_resetting_the_provisioned_target(self):
        harness = load_harness()
        process = mock.Mock()

        with (
            mock.patch.object(harness.pty, "openpty", return_value=(41, 42)),
            mock.patch.object(harness.os, "close"),
            mock.patch.object(harness.subprocess, "Popen", return_value=process) as popen,
        ):
            returned_process, master_fd = harness.start_rtt_capture(
                "clicker", "TEST-PROBE"
            )

        self.assertIs(returned_process, process)
        self.assertEqual(master_fd, 41)
        command = popen.call_args.args[0]
        self.assertEqual(command[1], "rtt")
        self.assertIn("TEST-PROBE", command)
        connect_mode = command.index("-M")
        self.assertEqual(command[connect_mode + 1], "attach")
        self.assertNotIn(
            "pre-reset",
            command,
            "click qualification must not reset away volatile routes",
        )

    def run_with_clicker_output(self, chunks: list[bytes]):
        harness = load_harness()
        process = mock.Mock()
        process.wait.return_value = 0
        ble_process = self.ble_process()
        clock = FakeClock()
        pending = list(chunks)

        def readable_fds(read_list, _write_list, _error_list, _timeout):
            return ([read_list[0]] if pending else [], [], [])

        def read_chunk(_fd, _capacity):
            return pending.pop(0)

        failure = None
        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(harness, "LOGS_DIR", Path(temp_dir)),
                mock.patch.object(harness, "PROBES", {"clicker": "TEST-PROBE"}),
                mock.patch.object(
                    harness,
                    "start_rtt_capture",
                    return_value=(process, 73),
                ),
                mock.patch.object(
                    harness,
                    "start_ble_receipt_consumer",
                    return_value=ble_process,
                ),
                mock.patch.object(harness, "wait_for_ble_receipt_consumer"),
                mock.patch.object(harness.select, "select", side_effect=readable_fds),
                mock.patch.object(harness.os, "read", side_effect=read_chunk),
                mock.patch.object(harness.os, "write", return_value=6) as write,
                mock.patch.object(harness.os, "close"),
                mock.patch.object(harness.time, "time", side_effect=clock.time),
                mock.patch.object(harness.time, "sleep", side_effect=clock.sleep),
            ):
                try:
                    harness.run_click_test(
                        "direct", duration_s=0.2, pre_click_delay_s=0.0
                    )
                except RuntimeError as error:
                    failure = error

        return write, failure

    def test_firmware_ready_record_without_host_down_channel_does_not_inject(self):
        write, failure = self.run_with_clicker_output(
            [b"DBG_CLICKER_RTT ready=1 commands=CLICK,LONG\r\n"]
        )

        write.assert_not_called()
        self.assertIsNotNone(failure)

    def test_down_channel_attachment_allows_injection_without_ready_replay(self):
        write, failure = self.run_with_clicker_output(
            [
                b'Reading from up channel 0 ("Terminal")\r\n',
                b'Writing to down channel 0 ("Terminal")\r\n',
            ]
        )

        writes = write.call_args_list
        self.assertTrue(writes, "the armed clicker must receive a command")
        self.assertTrue(
            all(call.args[0] == 73 for call in writes),
            "every command character must use the clicker RTT PTY",
        )
        self.assertTrue(
            all(len(call.args[1]) == 1 for call in writes),
            "pyOCD's one-character stdin reader requires one PTY write per byte",
        )
        self.assertEqual(
            b"".join(call.args[1] for call in writes),
            b"\nCLICK\n",
            "the paced flush must clear a contaminated line before CLICK",
        )
        self.assertIsNone(failure)

    def test_counted_click_series_uses_the_requested_cadence(self):
        harness = load_harness()
        process = mock.Mock()
        process.wait.return_value = 0
        ble_process = self.ble_process()
        clock = FakeClock()
        pending = [
            b'Reading from up channel 0 ("Terminal")\r\n'
            b'Writing to down channel 0 ("Terminal")\r\n'
        ]

        def readable_fds(read_list, _write_list, _error_list, _timeout):
            return ([read_list[0]] if pending else [], [], [])

        def read_chunk(_fd, _capacity):
            return pending.pop(0)

        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(harness, "LOGS_DIR", Path(temp_dir)),
                mock.patch.object(harness, "PROBES", {"clicker": "TEST-PROBE"}),
                mock.patch.object(
                    harness,
                    "start_rtt_capture",
                    return_value=(process, 73),
                ),
                mock.patch.object(
                    harness,
                    "start_ble_receipt_consumer",
                    return_value=ble_process,
                ),
                mock.patch.object(harness, "wait_for_ble_receipt_consumer"),
                mock.patch.object(harness.select, "select", side_effect=readable_fds),
                mock.patch.object(harness.os, "read", side_effect=read_chunk),
                mock.patch.object(harness.os, "write", return_value=1) as write,
                mock.patch.object(harness.os, "close"),
                mock.patch.object(harness.time, "time", side_effect=clock.time),
                mock.patch.object(harness.time, "sleep", side_effect=clock.sleep),
            ):
                harness.run_click_test(
                    "direct",
                    duration_s=3.0,
                    pre_click_delay_s=0.0,
                    click_count=3,
                    click_interval_s=0.1,
                )

        self.assertEqual(
            b"".join(call.args[1] for call in write.call_args_list),
            b"\nCLICK\n\nCLICK\n\nCLICK\n",
        )

    def test_explicit_gesture_sequence_can_start_self_test(self):
        harness = load_harness()
        process = mock.Mock()
        process.wait.return_value = 0
        ble_process = self.ble_process()
        clock = FakeClock()
        pending = [
            b'Reading from up channel 0 ("Terminal")\r\n'
            b'Writing to down channel 0 ("Terminal")\r\n'
        ]

        def readable_fds(read_list, _write_list, _error_list, _timeout):
            return ([read_list[0]] if pending else [], [], [])

        def read_chunk(_fd, _capacity):
            return pending.pop(0)

        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(harness, "LOGS_DIR", Path(temp_dir)),
                mock.patch.object(harness, "PROBES", {"clicker": "TEST-PROBE"}),
                mock.patch.object(
                    harness,
                    "start_rtt_capture",
                    return_value=(process, 73),
                ),
                mock.patch.object(
                    harness,
                    "start_ble_receipt_consumer",
                    return_value=ble_process,
                ),
                mock.patch.object(harness, "wait_for_ble_receipt_consumer"),
                mock.patch.object(harness.select, "select", side_effect=readable_fds),
                mock.patch.object(harness.os, "read", side_effect=read_chunk),
                mock.patch.object(harness.os, "write", return_value=1) as write,
                mock.patch.object(harness.os, "close"),
                mock.patch.object(harness.time, "time", side_effect=clock.time),
                mock.patch.object(harness.time, "sleep", side_effect=clock.sleep),
            ):
                harness.run_click_test(
                    "forced",
                    duration_s=3.0,
                    pre_click_delay_s=0.0,
                    click_interval_s=0.5,
                    command_sequence=("LONG", "CLICK"),
                )

        self.assertEqual(
            b"".join(call.args[1] for call in write.call_args_list),
            b"\nLONG\n\nCLICK\n",
        )

    def test_ble_receipt_consumer_uses_the_repository_monitor(self):
        harness = load_harness()
        process = self.ble_process()

        with mock.patch.object(
            harness.subprocess, "Popen", return_value=process
        ) as popen:
            returned = harness.start_ble_receipt_consumer(
                "TEST GATEWAY",
                duration_s=123.0,
                connect_timeout_s=7.0,
            )

        self.assertIs(returned, process)
        command = popen.call_args.args[0]
        self.assertIn("provision_mesh_anchor.py", command[1])
        self.assertEqual(command[command.index("--gateway") + 1], "TEST GATEWAY")
        self.assertEqual(command[command.index("--command") + 1], "monitor")
        self.assertEqual(command[command.index("--duration") + 1], "123.0")
        self.assertEqual(
            popen.call_args.kwargs["env"]["PYTHONUNBUFFERED"],
            "1",
        )

    def test_ble_connection_marker_can_span_output_chunks(self):
        harness = load_harness()
        process = self.ble_process()
        output: list[bytes] = []
        chunks = [
            b"BLE_CONNECTED gateway_id=0x1234 comm",
            b"and=monitor\n",
        ]

        def read_chunk(_process, destination, _timeout_s):
            data = chunks.pop(0)
            destination.append(data)
            return data

        with mock.patch.object(
            harness,
            "read_process_output",
            side_effect=read_chunk,
        ):
            harness.wait_for_ble_receipt_consumer(
                process,
                output,
                timeout_s=1.0,
            )

        self.assertEqual(
            b"".join(output),
            b"BLE_CONNECTED gateway_id=0x1234 command=monitor\n",
        )

    def test_ble_monitor_shutdown_uses_sigint_for_async_disconnect(self):
        harness = load_harness()
        process = self.ble_process()

        harness.stop_process(process)

        process.send_signal.assert_called_once_with(harness.signal.SIGINT)
        process.wait.assert_called_once_with(
            timeout=harness.BLE_CONSUMER_SHUTDOWN_TIMEOUT_S
        )
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_ble_monitor_shutdown_has_bounded_terminate_fallback(self):
        harness = load_harness()
        process = self.ble_process()
        process.wait.side_effect = [
            harness.subprocess.TimeoutExpired("monitor", 5.0),
            0,
        ]

        harness.stop_process(process)

        process.send_signal.assert_called_once_with(harness.signal.SIGINT)
        process.terminate.assert_called_once_with()
        process.kill.assert_not_called()
        self.assertEqual(process.wait.call_count, 2)

    def test_missing_ble_consumer_blocks_every_rtt_click(self):
        harness = load_harness()
        ble_process = self.ble_process()

        with tempfile.TemporaryDirectory() as temp_dir:
            with (
                mock.patch.object(harness, "LOGS_DIR", Path(temp_dir)),
                mock.patch.object(
                    harness,
                    "start_ble_receipt_consumer",
                    return_value=ble_process,
                ),
                mock.patch.object(
                    harness,
                    "wait_for_ble_receipt_consumer",
                    side_effect=RuntimeError("BLE unavailable"),
                ),
                mock.patch.object(harness, "start_rtt_capture") as start_rtt,
                mock.patch.object(harness, "stop_process") as stop_process,
                mock.patch.object(
                    harness, "read_process_output", return_value=b""
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, "BLE unavailable"):
                    harness.run_click_test(
                        "forced",
                        duration_s=0.1,
                        pre_click_delay_s=0.0,
                    )

        start_rtt.assert_not_called()
        stop_process.assert_called_once_with(ble_process)


if __name__ == "__main__":
    unittest.main()
