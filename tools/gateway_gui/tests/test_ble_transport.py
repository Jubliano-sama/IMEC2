from __future__ import annotations

import asyncio
import codecs
import unittest
from typing import Any
from unittest.mock import patch

from tools.gateway_gui import ble_transport
from tools.gateway_gui.ble_transport import BleTransport
from tools.gateway_gui.protocol import (
    GATEWAY_IDENTITY_UUID,
    LOG_TX_UUID,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    SERVICE_UUID,
    GatewayReceiveBuffer,
)


class FakeServices:
    def __init__(self) -> None:
        self.characteristics = {
            PACKET_TX_UUID,
            PACKET_RX_UUID,
            LOG_TX_UUID,
            GATEWAY_IDENTITY_UUID,
        }

    def get_service(self, uuid: str) -> object | None:
        return object() if uuid == SERVICE_UUID else None

    def get_characteristic(self, uuid: str) -> object | None:
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
        self.__class__.instances.append(self)

    async def connect(self) -> None:
        self.is_connected = True

    async def disconnect(self) -> None:
        self.is_connected = False

    async def read_gatt_char(self, uuid: str) -> bytes:
        self.read_uuids.append(uuid)
        return self.identity

    async def start_notify(self, uuid: str, _callback: Any) -> None:
        self.notify_uuids.append(uuid)


def transport_model(events: list[dict[str, Any]]) -> BleTransport:
    transport = BleTransport.__new__(BleTransport)
    transport._event_sink = events.append
    transport._client = None
    transport._decoder = GatewayReceiveBuffer()
    transport._log_decoder = codecs.getincrementaldecoder("utf-8")("replace")
    transport._log_buffer = ""
    transport._intentional_disconnect = False
    return transport


class BleTransportIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        FakeBleakClient.instances.clear()
        FakeBleakClient.identity = 0xAABBCCDDEEFF0011.to_bytes(8, "little")

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
        self.assertEqual(client.notify_uuids, [PACKET_TX_UUID, LOG_TX_UUID])
        self.assertEqual([event["kind"] for event in events], ["connection_state", "gateway_identity", "connection_state"])
        self.assertEqual(events[1]["gateway_id"], 0xAABBCCDDEEFF0011)
        self.assertEqual(events[2]["gateway_id"], 0xAABBCCDDEEFF0011)

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

        transport._on_disconnected(old_client)

        self.assertIs(transport._client, current_client)
        self.assertEqual(events, [])


if __name__ == "__main__":
    unittest.main()
