#!/usr/bin/env python3
"""Send mesh provisioning commands and inspect raw gateway BLE events."""

from __future__ import annotations

import argparse
import asyncio
import pathlib
import sys
import time

from bleak import BleakClient


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.gateway_gui.protocol import (  # noqa: E402
    GATEWAY_IDENTITY_UUID,
    GatewayReceiveBuffer,
    MSG_GATEWAY_COMMAND_EVENT,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
    decode_gateway_identity,
)
from tools.gateway_gui.command_telemetry import decode_gateway_command_event  # noqa: E402
from tools.gateway_gui.protocol import COMMAND_STATUS_NAMES  # noqa: E402


async def run(args: argparse.Namespace) -> None:
    decoder = GatewayReceiveBuffer()
    received = 0

    def on_notify(_sender: object, data: bytearray) -> None:
        nonlocal received
        raw = bytes(data)
        print(f"BLE_CHUNK len={len(raw)} hex={raw.hex()}", flush=True)
        result = decoder.feed(raw)
        for error in result.errors:
            print(f"BLE_DECODE_ERROR {error}", flush=True)
        for packet in result.packets:
            received += 1
            print(
                f"BLE_PACKET type=0x{packet.msg_type:02x} src=0x{packet.src_id:016x} "
                f"dst=0x{packet.dst_id:016x} session={packet.session_id} "
                f"seq={packet.seq} payload={packet.payload.hex()}",
                flush=True,
            )
            if packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
                event = decode_gateway_command_event(
                    packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES)
                )
                print(f"GATEWAY_COMMAND_EVENT {event}", flush=True)

    async with BleakClient(args.gateway, timeout=args.connect_timeout) as client:
        gateway_id = decode_gateway_identity(
            bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID))
        )
        await client.start_notify(PACKET_TX_UUID, on_notify)
        characteristic = client.services.get_characteristic(PACKET_RX_UUID)
        if characteristic is None:
            raise RuntimeError("gateway packet RX characteristic is unavailable")
        chunk_size = max(
            1,
            min(int(characteristic.max_write_without_response_size or 20), 244),
        )
        if args.command == "monitor":
            print(
                f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command=monitor",
                flush=True,
            )
        else:
            for index in range(args.repeat):
                identity = int(time.time_ns() & 0xFFFFFFFF) or 1
                command_args = {
                    "host_id": args.host_id,
                    "gateway_id": gateway_id,
                    "session_id": identity,
                    "seq": identity & 0xFFFF,
                }
                if args.command == "here-i-am":
                    command = build_here_i_am_command(**command_args)
                elif args.command == "assign-slots":
                    command = build_assign_discovery_slots_command(**command_args)
                else:
                    survey_id = args.survey_id or identity
                    command = build_anchor_discovery_command(
                        **command_args,
                        survey_id=survey_id,
                        duration_ms=args.survey_duration_ms,
                        discovery_slot_count=args.discovery_slots,
                        sample_count=args.samples,
                    )
                print(
                    f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command={args.command} "
                    f"index={index + 1}/{args.repeat} session={identity} "
                    f"frame={command.frame.hex()}",
                    flush=True,
                )
                for offset in range(0, len(command.frame), chunk_size):
                    await client.write_gatt_char(
                        characteristic,
                        command.frame[offset : offset + chunk_size],
                        response=False,
                    )
                if index + 1 < args.repeat:
                    await asyncio.sleep(args.interval)
        await asyncio.sleep(args.duration)
        print(f"BLE_COMPLETE packets={received}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", required=True)
    parser.add_argument(
        "--command",
        choices=("here-i-am", "assign-slots", "survey", "monitor"),
        required=True,
    )
    parser.add_argument("--host-id", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--connect-timeout", type=float, default=12.0)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0.05)
    parser.add_argument("--survey-id", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--survey-duration-ms", type=int, default=1000)
    parser.add_argument("--discovery-slots", type=int, default=6)
    parser.add_argument("--samples", type=int, default=1)
    args = parser.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
