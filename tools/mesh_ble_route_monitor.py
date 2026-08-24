#!/usr/bin/env python3
"""Monitor synthetic mesh-routing packets from the gateway BLE stream."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import dataclasses
import json
import os
from pathlib import Path
import sys
import time
from typing import Callable

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.gateway_gui.delivery_dedup import is_host_delivery_packet  # noqa: E402
from tools.gateway_gui.protocol import (  # noqa: E402
    GATEWAY_IDENTITY_UUID,
    PACKET_RX_UUID,
    build_gateway_host_receipt,
    decode_gateway_identity,
    parse_stream_record as parse_gateway_stream_record,
    validate_self_test_report_packet,
)

try:
    from bleak.backends.bluezdbus import defs as bluez_defs
    from bleak.backends.bluezdbus.manager import get_global_bluez_manager
except Exception:  # pragma: no cover - host tool fallback for non-BlueZ systems.
    bluez_defs = None
    get_global_bluez_manager = None


# Duplicated from firmware/app/src/app_gateway_ble.c. Keep these local so this
# host-side test tool does not import or require the firmware build tree.
SERVICE_UUID = "494d4543-0001-4757-8000-000000000001"
PACKET_TX_UUID = "494d4543-0001-4757-8000-000000000002"

# Duplicated from firmware/include/protocol.h and firmware/src/protocol.c.
PROTO_MAGIC = 0xC1
PROTO_VERSION = 0x01
PACKET_HEADER_LEN = 32
PACKET_CRC_LEN = 2
MESH_DEFAULT_TTL = 4
GATEWAY_STREAM_MAGIC = 0x5747
GATEWAY_STREAM_VERSION = 1
GATEWAY_STREAM_RECORD_HEADER_LEN = 40
GATEWAY_STREAM_RECORD_PACKET = 1

MSG_NAMES = {
    0x20: "CLICK_REPORT",
    0x21: "SELF_TEST_REPORT",
    0x22: "ANCHOR_HEARTBEAT",
    0x30: "MESH_DATA",
    0x32: "GATEWAY_ACK",
    0x35: "ROUTE_REQ",
    0x36: "ROUTE_REPLY",
    0x37: "MESH_EVENT_PROPOSE",
    0x38: "MESH_EVENT_ACCEPT",
    0x39: "MESH_EVENT_UPDATE",
    0x3A: "MESH_EVENT_END",
    0x40: "COMMAND",
    0x41: "COMMAND_RESULT",
    0x7F: "ERROR",
}


@dataclasses.dataclass(frozen=True)
class TlvSpec:
    name: str
    decoder: Callable[[bytes], object]


@dataclasses.dataclass(frozen=True)
class ResolvedDevice:
    address: str
    name: str = ""
    source: str = "unknown"
    connected: bool = False
    ble_device: BLEDevice | None = None

    @property
    def client_target(self) -> BLEDevice | str:
        return self.ble_device if self.ble_device is not None else self.address


def _uint(width: int) -> Callable[[bytes], int | None]:
    return lambda raw: None if len(raw) < width else int.from_bytes(raw[:width], "little")


def _sint(width: int) -> Callable[[bytes], int | None]:
    return lambda raw: None if len(raw) < width else int.from_bytes(raw[:width], "little", signed=True)


def _hex(raw: bytes) -> str:
    return raw.hex()


def _id64(raw: bytes) -> str | None:
    value = _uint(8)(raw)
    return None if value is None else f"0x{value:016x}"


# Parser table for normal protocol TLVs plus provisional mesh-test TLVs. If
# firmware assigns different synthetic-test IDs near protocol.h, update this
# table only; the rest of the monitor uses semantic names.
TLV_SPECS = {
    0x01: TlvSpec("device_role", _uint(1)),
    0x06: TlvSpec("event_seq", _uint(4)),
    0x07: TlvSpec("timestamp_ms", _uint(8)),
    0x0A: TlvSpec("anchor_id", _id64),
    0x0B: TlvSpec("clicker_id", _id64),
    0x10: TlvSpec("command_id", _uint(2)),
    0x11: TlvSpec("command_status", _uint(2)),
    0x12: TlvSpec("requested_msg_seq", _uint(2)),
    0x13: TlvSpec("next_hop_id", _id64),
    0x14: TlvSpec("gateway_id", _id64),
    0x1B: TlvSpec("retry_count", _uint(1)),
    0x1D: TlvSpec("uptime_ms", _uint(4)),
    0x1E: TlvSpec("reason", _uint(1)),
    0x22: TlvSpec("route_epoch", _uint(4)),
    0x23: TlvSpec("hop_count", _uint(1)),
    0x2A: TlvSpec("mesh_channel", _uint(1)),
    0x2E: TlvSpec("mesh_event_counter", _uint(4)),
    0x33: TlvSpec("diag_status_flags", _uint(4)),
    0x3C: TlvSpec("diag_frames_dropped", _uint(4)),
    0x46: TlvSpec("mesh_deferrals", _uint(4)),
    0x47: TlvSpec("mesh_ch9_event_misses", _uint(4)),
    # Provisional synthetic mesh-test TLVs, intentionally local to this tool.
    0x59: TlvSpec("mesh_test_packet_id", _uint(4)),
    0x5A: TlvSpec("mesh_test_attempt", _uint(2)),
    0x5B: TlvSpec("mesh_test_drop_count", _uint(4)),
    0x5C: TlvSpec("mesh_test_origin_id", _id64),
    0x5D: TlvSpec("mesh_test_target_id", _id64),
    0x5E: TlvSpec("mesh_test_flags", _uint(4)),
    0x97: TlvSpec("mesh_test_packet_age_ms", _uint(4)),
    0x98: TlvSpec("mesh_test_selected_parent_id", _id64),
    0x99: TlvSpec("mesh_test_ch9_timing_state", _uint(1)),
    0x9A: TlvSpec("mesh_test_payload_crc", _uint(2)),
}

SYNTHETIC_TLV_NAMES = {
    "mesh_test_packet_id",
    "mesh_test_attempt",
    "mesh_test_drop_count",
    "mesh_test_origin_id",
    "mesh_test_target_id",
    "mesh_test_flags",
    "mesh_test_payload_crc",
}


class DecodeError(ValueError):
    pass


@dataclasses.dataclass
class ProtoPacket:
    msg_type: int
    flags: int
    src_id: int
    dst_id: int
    session_id: int
    seq: int
    ttl: int
    message_age_ms: int
    payload: bytes
    tlvs: dict[str, object]
    raw_tlvs: dict[int, bytes]


@dataclasses.dataclass
class MonitorStats:
    packets_seen: int = 0
    synthetic_seen: int = 0
    decode_errors: int = 0
    capture_errors: int = 0
    gap_events: int = 0
    missing_packets: int = 0
    sequence_resets: int = 0
    max_hop_count: int | None = None
    max_attempt: int | None = None
    last_packet_ids: dict[tuple[int, str], int] = dataclasses.field(
        default_factory=dict)
    last_drop_counts: dict[int, int] = dataclasses.field(default_factory=dict)


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        if code == 0:
            raise DecodeError("zero byte inside COBS payload")
        index += 1
        end = index + code - 1
        if end > len(data):
            raise DecodeError("COBS block overruns frame")
        out.extend(data[index:end])
        index = end
        if code != 0xFF and index < len(data):
            out.append(0)
    return bytes(out)


def parse_tlvs(payload: bytes) -> tuple[dict[str, object], dict[int, bytes]]:
    decoded: dict[str, object] = {}
    raw_tlvs: dict[int, bytes] = {}
    index = 0
    while index + 2 <= len(payload):
        tlv_type = payload[index]
        tlv_len = payload[index + 1]
        index += 2
        if index + tlv_len > len(payload):
            decoded.setdefault("_errors", [])
            decoded["_errors"].append(f"tlv 0x{tlv_type:02x} overruns payload")
            break
        value = payload[index:index + tlv_len]
        index += tlv_len
        raw_tlvs[tlv_type] = value
        spec = TLV_SPECS.get(tlv_type)
        if spec is None:
            decoded[f"tlv_0x{tlv_type:02x}"] = value.hex()
        else:
            decoded[spec.name] = spec.decoder(value)
    if index != len(payload):
        decoded.setdefault("_errors", [])
        decoded["_errors"].append("trailing partial tlv header")
    return decoded, raw_tlvs


def parse_cobs_packet(frame: bytes) -> ProtoPacket:
    encoded = frame[:-1] if frame.endswith(b"\x00") else frame
    raw = cobs_decode(encoded)
    if len(raw) < PACKET_HEADER_LEN + PACKET_CRC_LEN:
        raise DecodeError(f"short decoded packet: {len(raw)} bytes")
    payload_len = raw[27]
    expected_len = PACKET_HEADER_LEN + payload_len + PACKET_CRC_LEN
    if len(raw) != expected_len:
        raise DecodeError(f"bad decoded length: got {len(raw)}, expected {expected_len}")
    if raw[0] != PROTO_MAGIC or raw[1] != PROTO_VERSION:
        raise DecodeError("bad packet magic/version")
    expected_crc = int.from_bytes(raw[-2:], "little")
    actual_crc = crc16_ccitt_false(raw[:-2])
    if expected_crc != actual_crc:
        raise DecodeError(f"bad crc: expected 0x{expected_crc:04x}, actual 0x{actual_crc:04x}")
    payload = raw[PACKET_HEADER_LEN:PACKET_HEADER_LEN + payload_len]
    tlvs, raw_tlvs = parse_tlvs(payload)
    return ProtoPacket(
        msg_type=raw[2],
        flags=raw[3],
        src_id=int.from_bytes(raw[4:12], "little"),
        dst_id=int.from_bytes(raw[12:20], "little"),
        session_id=int.from_bytes(raw[20:24], "little"),
        seq=int.from_bytes(raw[24:26], "little"),
        ttl=raw[26],
        message_age_ms=int.from_bytes(raw[28:32], "little"),
        payload=payload,
        tlvs=tlvs,
        raw_tlvs=raw_tlvs,
    )


def parse_stream_record(record: bytes) -> ProtoPacket:
    if len(record) < GATEWAY_STREAM_RECORD_HEADER_LEN:
        raise DecodeError(f"short stream record: {len(record)} bytes")
    magic = int.from_bytes(record[0:2], "little")
    version = record[2]
    header_len = record[3]
    record_type = record[4]
    if magic != GATEWAY_STREAM_MAGIC:
        raise DecodeError(f"bad stream magic 0x{magic:04x}")
    if version != GATEWAY_STREAM_VERSION:
        raise DecodeError(f"bad stream version {version}")
    if header_len != GATEWAY_STREAM_RECORD_HEADER_LEN:
        raise DecodeError(f"bad stream header length {header_len}")
    if record_type != GATEWAY_STREAM_RECORD_PACKET:
        raise DecodeError(f"bad stream record type {record_type}")
    payload_len = int.from_bytes(record[36:38], "little")
    expected_len = header_len + payload_len
    if len(record) != expected_len:
        raise DecodeError(f"bad stream length: got {len(record)}, expected {expected_len}")
    payload = record[header_len:expected_len]
    expected_crc = int.from_bytes(record[38:40], "little")
    actual_crc = crc16_ccitt_false(payload)
    if expected_crc != actual_crc:
        raise DecodeError(f"bad stream payload crc: expected 0x{expected_crc:04x}, actual 0x{actual_crc:04x}")
    tlvs, raw_tlvs = parse_tlvs(payload)
    return ProtoPacket(
        msg_type=record[8],
        flags=record[9],
        src_id=int.from_bytes(record[16:24], "little"),
        dst_id=int.from_bytes(record[24:32], "little"),
        session_id=int.from_bytes(record[12:16], "little"),
        seq=int.from_bytes(record[10:12], "little"),
        ttl=MESH_DEFAULT_TTL,
        message_age_ms=int.from_bytes(record[32:36], "little"),
        payload=payload,
        tlvs=tlvs,
        raw_tlvs=raw_tlvs,
    )


def next_stream_record_len(buffer: bytearray) -> int | None:
    if len(buffer) < GATEWAY_STREAM_RECORD_HEADER_LEN:
        return None
    if int.from_bytes(buffer[0:2], "little") != GATEWAY_STREAM_MAGIC:
        return None
    header_len = buffer[3]
    if header_len < GATEWAY_STREAM_RECORD_HEADER_LEN:
        raise DecodeError(f"bad stream header length {header_len}")
    if len(buffer) < header_len:
        return None
    payload_len = int.from_bytes(buffer[36:38], "little")
    return header_len + payload_len


def packet_sequence(packet: ProtoPacket) -> tuple[str, int]:
    for key in ("mesh_test_packet_id", "mesh_event_counter", "event_seq",
                "requested_msg_seq"):
        value = packet.tlvs.get(key)
        if isinstance(value, int):
            return key, value
    return f"msg_0x{packet.msg_type:02x}_seq", packet.seq


def packet_attempt(packet: ProtoPacket) -> int | None:
    for key in ("mesh_test_attempt", "retry_count"):
        value = packet.tlvs.get(key)
        if isinstance(value, int):
            return value
    return None


def packet_drop_count(packet: ProtoPacket) -> int | None:
    for key in ("mesh_test_drop_count", "diag_frames_dropped", "mesh_ch9_event_misses"):
        value = packet.tlvs.get(key)
        if isinstance(value, int):
            return value
    return None


def packet_hop_count(packet: ProtoPacket) -> int | None:
    value = packet.tlvs.get("hop_count")
    if isinstance(value, int):
        return value
    if "mesh_test_packet_id" in packet.tlvs and packet.ttl <= MESH_DEFAULT_TTL:
        return (MESH_DEFAULT_TTL - packet.ttl) + 1
    return None


def is_synthetic_mesh_test(packet: ProtoPacket, include_all_mesh_data: bool) -> bool:
    if SYNTHETIC_TLV_NAMES.intersection(packet.tlvs):
        return True
    if include_all_mesh_data and packet.msg_type == 0x30:
        return True
    return False


def format_id64(value: int) -> str:
    return f"0x{value:016x}"


def format_packet_line(packet: ProtoPacket, stats: MonitorStats, now_s: float) -> str:
    sequence_class, pid = packet_sequence(packet)
    sequence_key = (packet.src_id, sequence_class)
    previous_id = stats.last_packet_ids.get(sequence_key)
    attempt = packet_attempt(packet)
    drop_count = packet_drop_count(packet)
    hop_count = packet_hop_count(packet)

    gap_text = "gap=0"
    advance_high_watermark = True
    if previous_id is not None and pid == 1 and previous_id > 1:
        stats.sequence_resets += 1
        gap_text = f"gap=stream-reset prev={previous_id}"
    elif previous_id is not None and pid > previous_id + 1:
        missing = pid - previous_id - 1
        stats.gap_events += 1
        stats.missing_packets += missing
        gap_text = f"gap={missing} missing={previous_id + 1}..{pid - 1}"
    elif previous_id is not None and pid <= previous_id:
        gap_text = f"gap=out-of-order prev={previous_id}"
        advance_high_watermark = False
    if advance_high_watermark:
        stats.last_packet_ids[sequence_key] = pid

    if drop_count is not None:
        stats.last_drop_counts[packet.src_id] = drop_count
    if hop_count is not None:
        stats.max_hop_count = hop_count if stats.max_hop_count is None else max(stats.max_hop_count, hop_count)
    if attempt is not None:
        stats.max_attempt = attempt if stats.max_attempt is None else max(stats.max_attempt, attempt)

    fields = [
        f"t={now_s:8.3f}s",
        f"stream={sequence_class}",
        f"id={pid}",
        gap_text,
        f"attempt={attempt if attempt is not None else '-'}",
        f"drops={drop_count if drop_count is not None else '-'}",
        f"hop={hop_count if hop_count is not None else '-'}",
        f"msg={MSG_NAMES.get(packet.msg_type, f'0x{packet.msg_type:02x}')}",
        f"seq={packet.seq}",
        f"ttl={packet.ttl}",
        f"age_ms={packet.message_age_ms}",
        f"src={format_id64(packet.src_id)}",
        f"dst={format_id64(packet.dst_id)}",
    ]
    next_hop = packet.tlvs.get("next_hop_id")
    if isinstance(next_hop, str):
        fields.append(f"next={next_hop}")
    selected_parent = packet.tlvs.get("mesh_test_selected_parent_id")
    if isinstance(selected_parent, str):
        fields.append(f"parent={selected_parent}")
    ch9_state = packet.tlvs.get("mesh_test_ch9_timing_state")
    if isinstance(ch9_state, int):
        fields.append(f"ch9_state={ch9_state}")
    payload_crc = packet.tlvs.get("mesh_test_payload_crc")
    if isinstance(payload_crc, int):
        fields.append(f"payload_crc=0x{payload_crc:04x}")
    return " ".join(fields)


def packet_json_record(packet: ProtoPacket,
                       now_s: float,
                       observed_unix_ns: int | None = None) -> dict[str, object]:
    sequence_class, sequence_id = packet_sequence(packet)

    record: dict[str, object] = {
        "observed_elapsed_s": round(now_s, 6),
        "sequence_class": sequence_class,
        "packet_id": sequence_id,
        "attempt": packet_attempt(packet),
        "drop_count": packet_drop_count(packet),
        "hop_count": packet_hop_count(packet),
        "msg_type": packet.msg_type,
        "msg_name": MSG_NAMES.get(packet.msg_type),
        "src_id": format_id64(packet.src_id),
        "dst_id": format_id64(packet.dst_id),
        "session_id": packet.session_id,
        "seq": packet.seq,
        "ttl": packet.ttl,
        "message_age_ms": packet.message_age_ms,
        "tlvs": packet.tlvs,
    }
    if observed_unix_ns is not None:
        record["observed_unix_ns"] = observed_unix_ns
    return record


def append_jsonl_record(fd: int, record: dict[str, object], sync: bool) -> None:
    pending = (json.dumps(record, sort_keys=True) + "\n").encode("utf-8")

    while pending:
        written = os.write(fd, pending)
        if written <= 0:
            raise OSError("JSONL append made no progress")
        pending = pending[written:]
    if sync:
        os.fsync(fd)


def device_matches(name_or_address: str, address: str, name: str) -> bool:
    lowered = name_or_address.lower()
    return name_or_address in {address, name} or lowered in {address.lower(), name.lower()}


async def bluez_known_devices() -> list[ResolvedDevice]:
    if get_global_bluez_manager is None or bluez_defs is None:
        return []
    try:
        manager = await get_global_bluez_manager()
    except Exception:
        return []

    devices: list[ResolvedDevice] = []
    for path, ifaces in getattr(manager, "_properties", {}).items():
        props = ifaces.get(bluez_defs.DEVICE_INTERFACE)
        if not props:
            continue
        address = props.get("Address")
        if not address:
            continue
        name = props.get("Name") or props.get("Alias") or ""
        connected = bool(props.get("Connected"))
        ble_device = BLEDevice(address, name or None, {"path": path, "props": props})
        devices.append(ResolvedDevice(
            address=address,
            name=name,
            source="bluez-connected" if connected else "bluez-cache",
            connected=connected,
            ble_device=ble_device,
        ))
    return devices


async def bluetoothctl_devices(filter_arg: str | None = None) -> list[ResolvedDevice]:
    command = ["bluetoothctl", "devices"]
    if filter_arg:
        command.append(filter_arg)
    try:
        proc = await asyncio.create_subprocess_exec(
            *command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
    except FileNotFoundError:
        return []
    stdout, _ = await proc.communicate()
    devices: list[ResolvedDevice] = []
    for raw_line in stdout.decode("utf-8", errors="replace").splitlines():
        parts = raw_line.strip().split(maxsplit=2)
        if len(parts) >= 2 and parts[0] == "Device":
            devices.append(ResolvedDevice(
                parts[1],
                parts[2] if len(parts) > 2 else "",
                source=f"bluetoothctl-{filter_arg.lower()}" if filter_arg else "bluetoothctl-cache",
                connected=filter_arg == "Connected",
            ))
    return devices


def cached_device_has_service(device: ResolvedDevice) -> bool | None:
    if device.ble_device is None:
        return None
    props = device.ble_device.details.get("props", {})
    uuids = {uuid.lower() for uuid in props.get("UUIDs", [])}
    return SERVICE_UUID in uuids


async def find_cached_device(name_or_address: str, require_service: bool, only_connected: bool) -> ResolvedDevice | None:
    for device in await bluez_known_devices():
        if only_connected and not device.connected:
            continue
        if not device_matches(name_or_address, device.address, device.name):
            continue
        has_service = cached_device_has_service(device)
        if require_service and has_service is False:
            raise RuntimeError(f"{name_or_address!r} found in BlueZ cache but has no IMEC gateway service")
        return device

    for filter_arg in ("Connected", None):
        for device in await bluetoothctl_devices(filter_arg):
            if only_connected and not device.connected:
                continue
            if device_matches(name_or_address, device.address, device.name):
                return device
    return None


async def find_device(name_or_address: str, timeout_s: float, require_service: bool) -> ResolvedDevice:
    connected = await find_cached_device(name_or_address, require_service, only_connected=True)
    if connected is not None:
        return connected

    devices = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
    for _, (device, adv) in devices.items():
        name = adv.local_name or device.name or ""
        uuids = {uuid.lower() for uuid in (adv.service_uuids or [])}
        if device_matches(name_or_address, device.address, name):
            if require_service and SERVICE_UUID not in uuids:
                raise RuntimeError(f"{name_or_address!r} found but did not advertise IMEC gateway service")
            return ResolvedDevice(device.address, name, source="scan", connected=False, ble_device=device)
    cached = await find_cached_device(name_or_address, require_service, only_connected=False)
    if cached is not None:
        return cached
    seen = sorted(
        format_scan_device(device, adv)
        for _, (device, adv) in devices.items()
    )
    raise RuntimeError(
        f"device {name_or_address!r} not found; seen: {', '.join(seen) or '<none>'}. "
        "Run with --list-devices and pass the address if the OS Bluetooth UI has a cached name."
    )


def format_scan_device(device: object, adv: object) -> str:
    name = adv.local_name or device.name or "<unnamed>"
    uuids = ",".join(sorted(adv.service_uuids or [])) or "-"
    rssi = getattr(adv, "rssi", None)
    rssi_text = "-" if rssi is None else str(rssi)
    return f"{device.address} name={name!r} rssi={rssi_text} services={uuids}"


async def list_devices(timeout_s: float) -> int:
    devices = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
    for _, (device, adv) in sorted(devices.items(), key=lambda item: item[1][0].address):
        print(format_scan_device(device, adv), flush=True)
    if not devices:
        print("<none>", flush=True)
    return 0


async def start_notify_bounded(
    client: BleakClient,
    char_uuid: str,
    callback: Callable[[object, bytearray], None],
    timeout_s: float,
    label: str,
) -> None:
    try:
        await asyncio.wait_for(client.start_notify(char_uuid, callback), timeout=timeout_s)
    except asyncio.TimeoutError as exc:
        raise RuntimeError(f"{label} notify setup timed out after {timeout_s:.1f}s") from exc


async def disconnect_bounded(client: BleakClient, timeout_s: float) -> None:
    await asyncio.wait_for(client.disconnect(), timeout=timeout_s)


async def run(args: argparse.Namespace) -> int:
    if args.list_devices:
        return await list_devices(args.scan_timeout)

    gateway = await find_device(args.gateway, args.scan_timeout, require_service=not args.no_service_filter)
    gateway_client: BleakClient | None = None
    jsonl_fd: int | None = None
    gateway_preconnected = gateway.connected
    stats = MonitorStats()
    start_s = time.monotonic()
    start_unix_ns = time.time_ns()
    done = asyncio.Event()
    rx_buffer = bytearray()
    receipt_tasks: set[asyncio.Task[None]] = set()
    receipt_in_flight: set[object] = set()
    receipt_write_lock = asyncio.Lock()
    gateway_id = 0
    packet_rx_characteristic: object | None = None
    receipt_chunk_size = 20
    if args.jsonl_file:
        jsonl_fd = os.open(args.jsonl_file,
                           os.O_APPEND | os.O_CREAT | os.O_WRONLY,
                           0o644)

    def schedule_host_receipt(record: bytes) -> None:
        if gateway_client is None or packet_rx_characteristic is None:
            return
        try:
            delivery_packet = parse_gateway_stream_record(record)
            if not is_host_delivery_packet(delivery_packet):
                return
            if delivery_packet.msg_type == 0x21:
                validate_self_test_report_packet(delivery_packet)
            receipt = build_gateway_host_receipt(
                delivery_packet,
                host_id=args.host_id,
                gateway_id=gateway_id,
            )
        except (TypeError, ValueError) as exc:
            stats.capture_errors += 1
            print(f"receipt_error {exc}", file=sys.stderr, flush=True)
            return
        if receipt.identity in receipt_in_flight:
            return
        receipt_in_flight.add(receipt.identity)

        async def write_receipt() -> None:
            try:
                async with receipt_write_lock:
                    for offset in range(0, len(receipt.frame), receipt_chunk_size):
                        await gateway_client.write_gatt_char(
                            packet_rx_characteristic,
                            receipt.frame[offset:offset + receipt_chunk_size],
                            response=False,
                        )
                if args.verbose:
                    print(
                        "host_receipt "
                        f"msg=0x{receipt.identity.original_msg_type:02x} "
                        f"src={format_id64(receipt.identity.src_id)} "
                        f"session={receipt.identity.session_id} "
                        f"seq={receipt.identity.seq}",
                        flush=True,
                    )
            except Exception as exc:
                stats.capture_errors += 1
                print(
                    f"receipt_error {type(exc).__name__}: {exc}",
                    file=sys.stderr,
                    flush=True,
                )
            finally:
                receipt_in_flight.discard(receipt.identity)

        task = asyncio.create_task(write_receipt())
        receipt_tasks.add(task)
        task.add_done_callback(receipt_tasks.discard)

    def on_packet(_sender: object, data: bytearray) -> None:
        rx_buffer.extend(bytes(data))
        while rx_buffer:
            try:
                stream_len = next_stream_record_len(rx_buffer)
            except DecodeError as exc:
                stats.decode_errors += 1
                if args.verbose:
                    print(f"decode_error {exc}", file=sys.stderr, flush=True)
                del rx_buffer[:1]
                continue

            if stream_len is not None:
                if len(rx_buffer) < stream_len:
                    break
                frame = bytes(rx_buffer[:stream_len])
                del rx_buffer[:stream_len]
                parser = parse_stream_record
            else:
                if (len(rx_buffer) < 2 and rx_buffer[0] == (GATEWAY_STREAM_MAGIC & 0xFF)) or \
                   (len(rx_buffer) >= 2 and
                    rx_buffer[0] == (GATEWAY_STREAM_MAGIC & 0xFF) and
                    rx_buffer[1] == (GATEWAY_STREAM_MAGIC >> 8)):
                    break
                if 0 not in rx_buffer:
                    break
                frame_end = rx_buffer.index(0)
                frame = bytes(rx_buffer[:frame_end + 1])
                del rx_buffer[:frame_end + 1]
                parser = parse_cobs_packet

            try:
                packet = parser(frame)
            except DecodeError as exc:
                stats.decode_errors += 1
                if args.verbose:
                    print(f"decode_error {exc}", file=sys.stderr, flush=True)
                continue

            stats.packets_seen += 1
            if not is_synthetic_mesh_test(packet, args.include_all_mesh_data):
                if args.verbose:
                    print(
                        f"packet msg={MSG_NAMES.get(packet.msg_type, f'0x{packet.msg_type:02x}')} "
                        f"seq={packet.seq} src={format_id64(packet.src_id)}",
                        flush=True,
                    )
                if parser is parse_stream_record:
                    schedule_host_receipt(frame)
                continue

            stats.synthetic_seen += 1
            observed_s = time.monotonic() - start_s
            print(format_packet_line(packet, stats, observed_s), flush=True)
            record = packet_json_record(
                packet,
                observed_s,
                start_unix_ns + int(observed_s * 1_000_000_000))
            if args.jsonl:
                print(json.dumps(record, sort_keys=True), flush=True)
            receipt_allowed = True
            if jsonl_fd is not None:
                try:
                    append_jsonl_record(jsonl_fd, record, args.jsonl_fsync)
                except OSError as exc:
                    stats.capture_errors += 1
                    print(f"capture_error {exc}", file=sys.stderr, flush=True)
                    receipt_allowed = False
            if receipt_allowed and parser is parse_stream_record:
                schedule_host_receipt(frame)
            if args.count and stats.synthetic_seen >= args.count:
                done.set()

    try:
        gateway_client = BleakClient(gateway.client_target, timeout=args.connect_timeout)
        print(
            f"resolved gateway={args.gateway} address={gateway.address} "
            f"source={gateway.source} preconnected={int(gateway.connected)}",
            flush=True,
        )
        await asyncio.wait_for(
            gateway_client.connect(timeout=args.connect_timeout, dangerous_use_bleak_cache=True),
            timeout=args.connect_timeout + 1.0,
        )
        print(f"attached gateway={args.gateway} address={gateway.address}", flush=True)
        gateway_id = decode_gateway_identity(
            bytes(await gateway_client.read_gatt_char(GATEWAY_IDENTITY_UUID))
        )
        packet_rx_characteristic = gateway_client.services.get_characteristic(
            PACKET_RX_UUID
        )
        if packet_rx_characteristic is None:
            raise RuntimeError("gateway packet RX characteristic is unavailable")
        receipt_chunk_size = max(
            1,
            min(
                int(packet_rx_characteristic.max_write_without_response_size or 20),
                244,
            ),
        )
        await start_notify_bounded(gateway_client, PACKET_TX_UUID, on_packet, args.connect_timeout, "gateway packet")

        if args.duration_s is None and args.count is None:
            while True:
                await asyncio.sleep(3600)
        elif args.duration_s is None:
            await done.wait()
        else:
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(done.wait(), timeout=args.duration_s)
        while receipt_tasks:
            await asyncio.gather(*tuple(receipt_tasks))
    finally:
        if gateway_client is not None:
            with contextlib.suppress(Exception):
                if gateway_preconnected:
                    backend = getattr(gateway_client, "_backend", None)
                    cleanup = getattr(backend, "_cleanup_all", None)
                    if callable(cleanup):
                        cleanup()
                else:
                    await disconnect_bounded(gateway_client, args.connect_timeout)
        if jsonl_fd is not None:
            os.close(jsonl_fd)

    print(
        "summary "
        f"packets={stats.packets_seen} synthetic={stats.synthetic_seen} "
        f"gap_events={stats.gap_events} missing={stats.missing_packets} "
        f"streams={len(stats.last_packet_ids)} resets={stats.sequence_resets} "
        f"drop_sources={len(stats.last_drop_counts)} "
        f"max_attempt={stats.max_attempt if stats.max_attempt is not None else '-'} "
        f"max_hop={stats.max_hop_count if stats.max_hop_count is not None else '-'} "
        f"decode_errors={stats.decode_errors} capture_errors={stats.capture_errors}",
        flush=True,
    )
    return 1 if stats.capture_errors != 0 else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Monitor IMEC mesh-routing test packets from the gateway BLE stream.",
    )
    parser.add_argument("--gateway", default="IMEC Mesh Test Gateway",
                        help="gateway BLE name or address")
    parser.add_argument("--host-id", type=lambda value: int(value, 0), default=1,
                        help="non-zero host ID used for exact gateway custody receipts")
    parser.add_argument("--scan-timeout", type=float, default=6.0,
                        help="BLE scan timeout per lookup")
    parser.add_argument("--connect-timeout", type=float, default=8.0,
                        help="BLE GATT attach/connect timeout")
    parser.add_argument("--list-devices", action="store_true",
                        help="scan once, print address/name/RSSI/service UUIDs, and exit")
    parser.add_argument("--duration-s", type=float,
                        help="stop after this many seconds")
    parser.add_argument("--count", type=int,
                        help="stop after this many synthetic packets")
    parser.add_argument("--include-all-mesh-data", action="store_true",
                        help="treat every MSG_MESH_DATA packet as a synthetic test packet")
    parser.add_argument("--no-service-filter", action="store_true",
                        help="do not require the gateway advertisement to include the IMEC service UUID")
    parser.add_argument("--jsonl", action="store_true",
                        help="also emit one JSON object per synthetic packet")
    parser.add_argument("--jsonl-file",
                        help="append one JSON object per packet to this durable capture file")
    parser.add_argument("--jsonl-fsync", action="store_true",
                        help="fsync every JSONL record (slower, but survives host power loss)")
    parser.add_argument("--verbose", action="store_true",
                        help="print non-synthetic packet notices and decode errors")
    args = parser.parse_args()
    if args.duration_s is not None and args.duration_s <= 0:
        parser.error("--duration-s must be positive")
    if args.count is not None and args.count <= 0:
        parser.error("--count must be positive")
    if args.connect_timeout <= 0:
        parser.error("--connect-timeout must be positive")
    if args.jsonl_fsync and not args.jsonl_file:
        parser.error("--jsonl-fsync requires --jsonl-file")
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
