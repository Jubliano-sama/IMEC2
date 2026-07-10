"""Capture the gateway's firmware log stream over BLE."""

from __future__ import annotations

import argparse
import asyncio
import codecs
import re

from bleak import BleakClient

from .ble_smoke import find_gateway
from .protocol import GatewayReceiveBuffer, LOG_TX_UUID, PACKET_TX_UUID


async def run(
    address: str | None,
    duration_s: float,
    pattern: str | None,
    packets_only: bool,
) -> None:
    target = address or await find_gateway()
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    packet_decoder = GatewayReceiveBuffer()
    buffered = ""
    matcher = re.compile(pattern) if pattern else None

    def emit(line: str) -> None:
        if matcher is None or matcher.search(line):
            print(line, flush=True)

    def on_log(_sender: object, data: bytearray) -> None:
        nonlocal buffered

        buffered += decoder.decode(bytes(data))
        while "\n" in buffered:
            line, buffered = buffered.split("\n", 1)
            emit(line.rstrip("\r"))

    def on_packet(_sender: object, data: bytearray) -> None:
        result = packet_decoder.feed(bytes(data))

        for error in result.errors:
            print(f"DECODE_ERROR {error}", flush=True)
        for packet in result.packets:
            print(
                f"PACKET msg={packet.message_name} type=0x{packet.msg_type:02x} "
                f"src=0x{packet.src_id:016x} dst=0x{packet.dst_id:016x} "
                f"session={packet.session_id} seq={packet.seq} "
                f"payload={len(packet.payload)}",
                flush=True,
            )

    async with BleakClient(target, timeout=12.0) as client:
        characteristic = PACKET_TX_UUID if packets_only else LOG_TX_UUID
        callback = on_packet if packets_only else on_log
        await client.start_notify(characteristic, callback)
        await asyncio.sleep(duration_s)
        if not client.is_connected:
            raise RuntimeError("gateway disconnected during BLE capture")

    if not packets_only:
        buffered += decoder.decode(b"", final=True)
        if buffered:
            emit(buffered.rstrip("\r"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("address", nargs="?", help="BLE address; scans when omitted")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--match", help="Only print log lines matching this regex")
    parser.add_argument(
        "--packets-only",
        action="store_true",
        help="Capture decoded packet notifications without enabling firmware logs",
    )
    args = parser.parse_args()
    asyncio.run(run(args.address, args.duration, args.match, args.packets_only))


if __name__ == "__main__":
    main()
