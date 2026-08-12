from __future__ import annotations

import asyncio
import unittest
from typing import Any
from unittest.mock import patch

from tools.gateway_gui import ble_transport
from tools.gateway_gui.ble_transport import BleTransport
from tools.gateway_gui.protocol import (
    GATEWAY_IDENTITY_UUID,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    SERVICE_UUID,
    GatewayReceiveBuffer,
)
from tools.gateway_gui.tests.test_protocol import click_payload, stream_record


class FakeServices:
    def __init__(self) -> None:
        self.characteristics = {
            PACKET_TX_UUID,
            PACKET_RX_UUID,
            GATEWAY_IDENTITY_UUID,
        }
        self.packet_rx = type(
            "PacketRxCharacteristic",
            (),
            {"max_write_without_response_size": 4},
        )()

    def get_service(self, uuid: str) -> object | None:
        return object() if uuid == SERVICE_UUID else None

    def get_characteristic(self, uuid: str) -> object | None:
        if uuid == PACKET_RX_UUID:
            return self.packet_rx
        return object() if uuid in self.characteristics else None


class FakeBleakClient:
    identity = 0xAABBCCDDEEFF0011.to_bytes(8, "little")
    instances: list[FakeBleakClient] = []

    def __init__(
        self,
        target: str,
        *,
        timeout: float,
        disconnected_callback: Any,
    ) -> None:
        self.target = target
        self.timeout = timeout
        self.disconnected_callback = disconnected_callback
        self.services = FakeServices()
        self.is_connected = False
        self.read_uuids: list[str] = []
        self.notify_uuids: list[str] = []
        self.notify_callback: Any = None
        self.writes: list[tuple[bytes, bool]] = []
        self.__class__.instances.append(self)

    async def connect(self) -> None:
        self.is_connected = True

    async def disconnect(self) -> None:
        self.is_connected = False

    async def read_gatt_char(self, uuid: str) -> bytes:
        self.read_uuids.append(uuid)
        return self.identity

    async def start_notify(self, uuid: str, callback: Any) -> None:
        self.notify_uuids.append(uuid)
        self.notify_callback = callback

    async def write_gatt_char(
        self, _characteristic: Any, data: bytes, *, response: bool
    ) -> None:
        self.writes.append((bytes(data), response))


class BlockingBleakClient(FakeBleakClient):
    connect_started: asyncio.Event
    connect_release: asyncio.Event

    async def connect(self) -> None:
        self.connect_started.set()
        await self.connect_release.wait()
        self.is_connected = True


class YieldingBleakClient(FakeBleakClient):
    async def write_gatt_char(
        self, characteristic: Any, data: bytes, *, response: bool
    ) -> None:
        await super().write_gatt_char(
            characteristic, data, response=response
        )
        await asyncio.sleep(0)


class ImmediateNotifyBleakClient(FakeBleakClient):
    notification_payload = b""

    async def start_notify(self, uuid: str, callback: Any) -> None:
        await super().start_notify(uuid, callback)
        if self.notification_payload:
            # Bleak implementations may deliver an ATT notification inline
            # before start_notify returns and before the client is published.
            callback(self, bytearray(self.notification_payload))


def transport_model(events: list[dict[str, Any]]) -> BleTransport:
    transport = BleTransport.__new__(BleTransport)
    transport._event_sink = events.append
    transport._client = None
    transport._connecting_client = None
    transport._connection_generation = 0
    transport._decoder = GatewayReceiveBuffer()
    transport._write_lock = None
    return transport


class BleTransportIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        FakeBleakClient.instances.clear()
        FakeBleakClient.identity = 0xAABBCCDDEEFF0011.to_bytes(8, "little")
        ImmediateNotifyBleakClient.notification_payload = b""

    def test_connect_reads_identity_before_subscribing_and_reporting_connected(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", FakeBleakClient),
        ):
            asyncio.run(transport._connect("AA:BB:CC:DD:EE:FF", 12.0))

        client = FakeBleakClient.instances[-1]
        self.assertEqual(client.read_uuids, [GATEWAY_IDENTITY_UUID])
        self.assertEqual(client.notify_uuids, [PACKET_TX_UUID])
        self.assertEqual([event["kind"] for event in events], ["connection_state", "gateway_identity", "connection_state"])
        self.assertEqual(events[1]["gateway_id"], 0xAABBCCDDEEFF0011)
        self.assertEqual(events[2]["gateway_id"], 0xAABBCCDDEEFF0011)

    def test_immediate_subscription_notification_is_bound_to_connecting_generation(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)
        ImmediateNotifyBleakClient.notification_payload = stream_record(click_payload())

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", ImmediateNotifyBleakClient),
        ):
            asyncio.run(transport._connect("AA:BB:CC:DD:EE:FF", 12.0))

        self.assertEqual(
            [event["kind"] for event in events],
            ["connection_state", "gateway_identity", "packet", "connection_state"],
        )
        self.assertEqual(events[2]["packet"].transport, "gateway-stream-v1")
        ImmediateNotifyBleakClient.notification_payload = b""

    def test_connect_rejects_invalid_identity_without_exposing_connected_state(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)
        FakeBleakClient.identity = b"\x01" * 7

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", FakeBleakClient),
            self.assertRaisesRegex(ValueError, "exactly 8 bytes"),
        ):
            asyncio.run(transport._connect("AA:BB:CC:DD:EE:FF", 12.0))

        self.assertIsNone(transport._client)
        self.assertFalse(FakeBleakClient.instances[-1].is_connected)
        self.assertEqual([event["state"] for event in events if event["kind"] == "connection_state"], ["connecting", "disconnected"])
        self.assertNotIn("gateway_identity", [event["kind"] for event in events])

    def test_stale_disconnect_callback_does_not_clear_reconnected_client(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)
        old_client = object()
        current_client = object()
        transport._client = current_client
        transport._connection_generation = 2

        transport._on_disconnected(old_client, 1)

        self.assertIs(transport._client, current_client)
        self.assertEqual(events, [])

    def test_concurrent_connect_is_rejected_before_second_client_starts(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)

        async def exercise() -> None:
            BlockingBleakClient.connect_started = asyncio.Event()
            BlockingBleakClient.connect_release = asyncio.Event()
            first = asyncio.create_task(transport._connect("first", 12.0))
            await BlockingBleakClient.connect_started.wait()
            with self.assertRaisesRegex(RuntimeError, "already in progress"):
                await transport._connect("second", 12.0)
            BlockingBleakClient.connect_release.set()
            await first

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", BlockingBleakClient),
        ):
            asyncio.run(exercise())

        self.assertEqual(len(BlockingBleakClient.instances), 1)
        self.assertEqual(transport._client.target, "first")
        self.assertEqual(
            [
                event["state"]
                for event in events
                if event["kind"] == "connection_state"
            ],
            ["connecting", "connected"],
        )

    def test_stale_notification_is_ignored_after_reconnect(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)

        async def exercise() -> tuple[Any, Any]:
            await transport._connect("first", 12.0)
            first = FakeBleakClient.instances[-1]
            await transport._disconnect()
            await transport._connect("second", 12.0)
            second = FakeBleakClient.instances[-1]
            return first.notify_callback, second.notify_callback

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", FakeBleakClient),
        ):
            stale_callback, current_callback = asyncio.run(exercise())

        events.clear()
        stale_callback(None, bytearray(b"\x00"))
        self.assertEqual(events, [])

        current_callback(None, bytearray(b"\x00"))
        self.assertEqual([event["kind"] for event in events], ["transport_error"])

    def test_worker_events_capture_immutable_receive_time(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)

        with patch.object(ble_transport.time, "monotonic", return_value=12.5):
            transport._emit("packet", packet=object())

        self.assertEqual(events[0]["received_at"], 12.5)

    def test_host_receipt_frame_is_written_unchanged_in_att_chunks(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)
        client = FakeBleakClient(
            "AA:BB:CC:DD:EE:FF",
            timeout=12.0,
            disconnected_callback=lambda _client: None,
        )
        client.is_connected = True
        transport._client = client
        frame = b"\x02receipt\x00"

        asyncio.run(transport._send_frame(frame, "gateway host receipt"))

        self.assertEqual(
            client.writes,
            [
                (b"\x02rec", False),
                (b"eipt", False),
                (b"\x00", False),
            ],
        )
        self.assertEqual(
            [event["kind"] for event in events],
            ["tx_written"],
        )
        self.assertEqual(events[0]["label"], "gateway host receipt")
        self.assertEqual(events[0]["raw"], frame)
        self.assertEqual(events[0]["byte_count"], len(frame))

    def test_concurrent_frames_keep_all_att_chunks_contiguous(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)
        client = YieldingBleakClient(
            "AA:BB:CC:DD:EE:FF",
            timeout=12.0,
            disconnected_callback=lambda _client: None,
        )
        client.is_connected = True
        transport._client = client
        first = b"AAAABBBBCCCC"
        second = b"111122223333"

        async def exercise() -> None:
            await asyncio.gather(
                transport._send_frame(first, "first"),
                transport._send_frame(second, "second"),
            )

        asyncio.run(exercise())

        self.assertEqual(
            [chunk for chunk, _response in client.writes],
            [b"AAAA", b"BBBB", b"CCCC", b"1111", b"2222", b"3333"],
        )
        self.assertEqual(
            [event["label"] for event in events if event["kind"] == "tx_written"],
            ["first", "second"],
        )

    def test_queued_frame_rejects_reconnect_before_writer_lock(self) -> None:
        events: list[dict[str, Any]] = []
        transport = transport_model(events)

        async def exercise() -> tuple[FakeBleakClient, FakeBleakClient]:
            await transport._connect("first", 12.0)
            first = FakeBleakClient.instances[-1]
            transport._write_lock = asyncio.Lock()
            await transport._write_lock.acquire()
            queued = asyncio.create_task(transport._send_frame(b"queued", "queued"))
            # Let _send_frame capture the first client/generation and block on
            # the lock before the connection is replaced.
            await asyncio.sleep(0)

            await transport._disconnect()
            await transport._connect("second", 12.0)
            second = FakeBleakClient.instances[-1]
            transport._write_lock.release()
            with self.assertRaisesRegex(RuntimeError, "connection changed"):
                await queued
            return first, second

        with (
            patch.object(ble_transport, "BLEAK_IMPORT_ERROR", None),
            patch.object(ble_transport, "BleakClient", FakeBleakClient),
        ):
            first, second = asyncio.run(exercise())

        self.assertEqual(first.writes, [])
        self.assertEqual(second.writes, [])


if __name__ == "__main__":
    unittest.main()
