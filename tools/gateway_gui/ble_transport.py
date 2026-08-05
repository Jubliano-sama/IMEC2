"""Background asyncio/Bleak transport for the Tk gateway GUI."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import threading
import time
from typing import Any, Callable, Coroutine

from .protocol import (
    GATEWAY_IDENTITY_UUID,
    GatewayReceiveBuffer,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    SERVICE_UUID,
    decode_gateway_identity,
)

try:
    from bleak import BleakClient, BleakScanner
except Exception as exc:  # pragma: no cover - depends on desktop installation.
    BleakClient = None  # type: ignore[assignment,misc]
    BleakScanner = None  # type: ignore[assignment,misc]
    BLEAK_IMPORT_ERROR: Exception | None = exc
else:
    BLEAK_IMPORT_ERROR = None


EventSink = Callable[[dict[str, Any]], None]


@dataclass(frozen=True)
class BleDeviceInfo:
    address: str
    name: str
    rssi: int | None
    service_present: bool

    @property
    def display(self) -> str:
        signal = "? dBm" if self.rssi is None else f"{self.rssi} dBm"
        marker = "service" if self.service_present else "name match"
        return f"{self.name or '<unnamed>'} | {self.address} | {signal} | {marker}"


class BleTransport:
    """Owns one Bleak client on a dedicated asyncio event-loop thread."""

    def __init__(self, event_sink: EventSink) -> None:
        self._event_sink = event_sink
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_loop, name="gateway-ble", daemon=True)
        self._thread.start()
        self._client: Any = None
        self._connecting_client: Any = None
        self._connection_generation = 0
        self._decoder = GatewayReceiveBuffer()

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()
        pending = asyncio.all_tasks(self._loop)
        for task in pending:
            task.cancel()
        if pending:
            self._loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
        self._loop.close()

    def _emit(self, kind: str, **fields: Any) -> None:
        # Capture arrival on the BLE worker before Tk scheduling can delay
        # consumption across a command deadline.
        self._event_sink(
            {"kind": kind, **fields, "received_at": time.monotonic()}
        )

    def _submit(self, coroutine: Coroutine[Any, Any, Any], operation: str) -> None:
        if not self._thread.is_alive():
            self._emit("transport_error", message=f"Cannot {operation}: BLE worker is stopped")
            coroutine.close()
            return
        future = asyncio.run_coroutine_threadsafe(coroutine, self._loop)

        def done_callback(done: Any) -> None:
            try:
                done.result()
            except asyncio.CancelledError:
                return
            except Exception as exc:  # Defensive boundary for unexpected Bleak failures.
                self._emit("transport_error", message=f"{operation} failed: {type(exc).__name__}: {exc}")

        future.add_done_callback(done_callback)

    def scan(self, timeout_s: float = 5.0) -> None:
        self._submit(self._scan(timeout_s), "BLE scan")

    async def _scan(self, timeout_s: float) -> None:
        if BLEAK_IMPORT_ERROR is not None or BleakScanner is None:
            raise RuntimeError(f"Bleak is unavailable: {BLEAK_IMPORT_ERROR}")
        self._emit("scan_state", active=True)
        try:
            discovered = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
            devices: list[BleDeviceInfo] = []
            entries = discovered.values() if isinstance(discovered, dict) else discovered
            for entry in entries:
                if isinstance(entry, tuple) and len(entry) == 2:
                    device, advertisement = entry
                else:
                    device = entry
                    advertisement = None
                name = (
                    getattr(advertisement, "local_name", None)
                    or getattr(device, "name", None)
                    or ""
                )
                service_uuids = {
                    str(uuid).lower()
                    for uuid in (getattr(advertisement, "service_uuids", None) or [])
                }
                service_present = SERVICE_UUID in service_uuids
                if not service_present and "imec" not in name.lower():
                    continue
                rssi = getattr(advertisement, "rssi", None)
                if rssi is None:
                    rssi = getattr(device, "rssi", None)
                devices.append(
                    BleDeviceInfo(
                        address=str(getattr(device, "address", "")),
                        name=name,
                        rssi=rssi,
                        service_present=service_present,
                    )
                )
            devices.sort(
                key=lambda item: (
                    not item.service_present,
                    "gateway" not in item.name.lower(),
                    -(item.rssi if item.rssi is not None else -999),
                    item.name,
                )
            )
            self._emit("scan_result", devices=devices)
        except Exception as exc:
            self._emit("transport_error", message=f"BLE scan failed: {type(exc).__name__}: {exc}")
        finally:
            self._emit("scan_state", active=False)

    def connect(self, target: str, timeout_s: float = 12.0) -> None:
        self._submit(self._connect(target.strip(), timeout_s), "BLE connect")

    async def _connect(self, target: str, timeout_s: float) -> None:
        if BLEAK_IMPORT_ERROR is not None or BleakClient is None:
            raise RuntimeError(f"Bleak is unavailable: {BLEAK_IMPORT_ERROR}")
        if not target:
            raise ValueError("device address is empty")
        if self._connecting_client is not None:
            raise RuntimeError("a gateway connection attempt is already in progress")
        if self._client is not None and self._client.is_connected:
            raise RuntimeError("a gateway is already connected")

        self._connection_generation += 1
        generation = self._connection_generation
        decoder = GatewayReceiveBuffer()
        self._emit("connection_state", state="connecting", target=target)
        client = BleakClient(
            target,
            timeout=timeout_s,
            disconnected_callback=lambda disconnected: self._on_disconnected(
                disconnected, generation
            ),
        )
        self._connecting_client = client
        try:
            await client.connect()
            if not self._connection_is_current(client, generation):
                await self._disconnect_client_quietly(client)
                return
            services = client.services
            required = (SERVICE_UUID, PACKET_TX_UUID, PACKET_RX_UUID, GATEWAY_IDENTITY_UUID)
            missing = [uuid for uuid in required if services.get_characteristic(uuid) is None and uuid != SERVICE_UUID]
            if services.get_service(SERVICE_UUID) is None:
                missing.insert(0, SERVICE_UUID)
            if missing:
                raise RuntimeError("connected device lacks required IMEC GATT UUIDs: " + ", ".join(missing))

            gateway_id = decode_gateway_identity(bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID)))
            if not self._connection_is_current(client, generation):
                await self._disconnect_client_quietly(client)
                return
            self._emit("gateway_identity", target=target, gateway_id=gateway_id)

            def packet_notification(sender: Any, data: bytearray) -> None:
                self._on_packet_notification(
                    client, generation, decoder, sender, data
                )

            await client.start_notify(PACKET_TX_UUID, packet_notification)
            if not self._connection_is_current(client, generation):
                await self._disconnect_client_quietly(client)
                return
            self._client = client
            self._connecting_client = None
            self._decoder = decoder
            self._emit("connection_state", state="connected", target=target, gateway_id=gateway_id)
        except Exception:
            if not self._connection_is_current(client, generation):
                await self._disconnect_client_quietly(client)
                return
            self._connecting_client = None
            self._connection_generation += 1
            await self._disconnect_client_quietly(client)
            self._emit("connection_state", state="disconnected", target=target)
            raise

    def _connection_is_current(self, client: Any, generation: int) -> bool:
        return (
            generation == self._connection_generation
            and (
                self._client is client
                or self._connecting_client is client
            )
        )

    @staticmethod
    async def _disconnect_client_quietly(client: Any) -> None:
        try:
            if client.is_connected:
                await client.disconnect()
        except Exception:
            pass

    def _on_packet_notification(
        self,
        client: Any,
        generation: int,
        decoder: GatewayReceiveBuffer,
        _sender: Any,
        data: bytearray,
    ) -> None:
        if (
            generation != self._connection_generation
            or self._client is not client
        ):
            return
        result = decoder.feed(bytes(data))
        for error in result.errors:
            self._emit("transport_error", message=error)
        for packet in result.packets:
            self._emit("packet", packet=packet)

    def _on_disconnected(self, client: Any, generation: int) -> None:
        if not self._connection_is_current(client, generation):
            return
        self._client = None
        self._connecting_client = None
        self._connection_generation += 1
        self._decoder = GatewayReceiveBuffer()
        self._emit("connection_state", state="disconnected", target="")
        self._emit(
            "transport_error", message="Gateway disconnected unexpectedly"
        )

    def disconnect(self) -> None:
        self._submit(self._disconnect(), "BLE disconnect")

    async def _disconnect(self) -> None:
        client = (
            self._client
            if self._client is not None
            else self._connecting_client
        )
        self._connection_generation += 1
        self._client = None
        self._connecting_client = None
        self._decoder = GatewayReceiveBuffer()
        if client is None:
            self._emit("connection_state", state="disconnected", target="")
            return
        self._emit("connection_state", state="disconnecting", target="")
        try:
            await self._disconnect_client_quietly(client)
        finally:
            self._emit("connection_state", state="disconnected", target="")

    def send_frame(self, frame: bytes, label: str) -> None:
        self._submit(self._send_frame(frame, label), f"send {label}")

    async def _send_frame(self, frame: bytes, label: str) -> None:
        client = self._client
        if client is None or not client.is_connected:
            raise RuntimeError("gateway is not connected")
        characteristic = client.services.get_characteristic(PACKET_RX_UUID)
        if characteristic is None:
            raise RuntimeError("packet RX characteristic disappeared")
        chunk_size = int(getattr(characteristic, "max_write_without_response_size", 20) or 20)
        chunk_size = max(1, min(chunk_size, 244))
        chunks = 0
        for offset in range(0, len(frame), chunk_size):
            await client.write_gatt_char(
                characteristic,
                frame[offset:offset + chunk_size],
                response=False,
            )
            chunks += 1
        self._emit(
            "tx_written",
            label=label,
            byte_count=len(frame),
            chunks=chunks,
            raw=frame,
        )

    def shutdown(self) -> None:
        if not self._thread.is_alive():
            return
        try:
            future = asyncio.run_coroutine_threadsafe(self._disconnect(), self._loop)
            future.result(timeout=2.0)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=2.0)
