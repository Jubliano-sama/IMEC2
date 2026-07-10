"""Strict live BLE acceptance check for an IMEC mesh gateway."""

from __future__ import annotations

import argparse
import asyncio

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

from .protocol import (
    GATEWAY_IDENTITY_UUID,
    LOG_TX_UUID,
    PACKET_TX_UUID,
    SERVICE_UUID,
    decode_gateway_identity,
)


async def find_gateway() -> BLEDevice:
    discovered = await BleakScanner.discover(timeout=5.0, return_adv=True)
    entries = discovered.values() if isinstance(discovered, dict) else discovered
    for entry in entries:
        device, advertisement = entry if isinstance(entry, tuple) else (entry, None)
        name = (
            getattr(advertisement, "local_name", None)
            or getattr(device, "name", None)
            or ""
        )
        service_uuids = {
            str(uuid).lower()
            for uuid in (getattr(advertisement, "service_uuids", None) or [])
        }
        if SERVICE_UUID in service_uuids or "imec" in name.lower():
            return device
    raise RuntimeError("no advertising IMEC gateway found")


async def run(address: str | None, settle_s: float) -> None:
    target = address or await find_gateway()
    packet_notifications = 0
    log_notifications = 0

    def on_packet(_sender: object, _data: bytearray) -> None:
        nonlocal packet_notifications
        packet_notifications += 1

    def on_log(_sender: object, _data: bytearray) -> None:
        nonlocal log_notifications
        log_notifications += 1

    async with BleakClient(target, timeout=12.0) as client:
        service = client.services.get_service(SERVICE_UUID)
        if service is None:
            raise RuntimeError("gateway service is missing")
        identity = decode_gateway_identity(
            bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID))
        )
        await client.start_notify(PACKET_TX_UUID, on_packet)
        await client.start_notify(LOG_TX_UUID, on_log)
        await asyncio.sleep(settle_s)
        if not client.is_connected:
            raise RuntimeError("gateway disconnected during BLE settle interval")

    print(
        f"BLE acceptance passed: gateway=0x{identity:016x} "
        f"packet_notifications={packet_notifications} "
        f"log_notifications={log_notifications}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("address", nargs="?", help="BLE address; scans when omitted")
    parser.add_argument("--settle", type=float, default=1.0)
    args = parser.parse_args()
    asyncio.run(run(args.address, args.settle))


if __name__ == "__main__":
    main()
