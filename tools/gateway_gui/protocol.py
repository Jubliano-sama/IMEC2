"""Host-side IMEC gateway packet framing and decoding.

The constants in this module mirror the current firmware protocol. Keeping the
decoder independent of Zephyr lets the GUI and its tests run on a desktop.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import math
from typing import Any, Callable, Iterable

from .operation_policy import (
    OperationPolicyProfile,
    decode_operation_policy_value,
)
from .command_telemetry import (
    CommandTelemetryDecodeError,
    GatewayCommandEvent,
    decode_gateway_command_event,
)


SERVICE_UUID = "494d4543-0001-4757-8000-000000000001"
PACKET_TX_UUID = "494d4543-0001-4757-8000-000000000002"
PACKET_RX_UUID = "494d4543-0001-4757-8000-000000000003"
GATEWAY_IDENTITY_UUID = "494d4543-0001-4757-8000-000000000005"

PROTO_MAGIC = 0xC1
PROTO_VERSION = 0x01
PACKET_HEADER_LEN = 32
PACKET_EXT_HEADER_LEN = 34
PACKET_CRC_LEN = 2
PACKET_EXT_LENGTH_SENTINEL = 0xFF
PACKET_EXT_MAX_PAYLOAD_LEN = 958

GATEWAY_STREAM_MAGIC = 0x5747
GATEWAY_STREAM_MAGIC_BYTES = GATEWAY_STREAM_MAGIC.to_bytes(2, "little")
GATEWAY_STREAM_VERSION = 1
GATEWAY_STREAM_RECORD_HEADER_LEN = 40
GATEWAY_STREAM_RECORD_MAX_LEN = GATEWAY_STREAM_RECORD_HEADER_LEN + PACKET_EXT_MAX_PAYLOAD_LEN
GATEWAY_STREAM_RECORD_PACKET = 1
GATEWAY_STREAM_FLAG_TRUNCATED = 0x01

MESH_BROADCAST_ID = 0
MESH_NETWORK_MAX_HOPS = 8
DEFAULT_HOST_ID = 0xA1C1BEEFC0DE0001
GATEWAY_COMMAND_BUDGET_MIN_MS = 1000
GATEWAY_COMMAND_BUDGET_MAX_MS = 3_600_000
DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS = 1_800_000
ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS = 120000
MSG_CLICK_REPORT = 0x20
MSG_SELF_TEST_REPORT = 0x21
MSG_ANCHOR_HEARTBEAT = 0x22
MSG_MESH_DATA = 0x30
MSG_COMMAND = 0x40
MSG_COMMAND_RESULT = 0x41
MSG_RESULT_BUNDLE = 0x44
MSG_GATEWAY_COMMAND_EVENT = 0x56
MSG_GATEWAY_HOST_RECEIPT = 0x57
MSG_SURVEY_EVENT = 0x58

GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN = 56
GATEWAY_HOST_RECEIPT_TLV_LEN = 2 + GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN

# Click-report semantic flags and bounds mirror firmware/include/protocol.h and
# firmware/include/report.h.  Envelope decoding intentionally remains
# independent; the semantic gate below is applied only at the receive-buffer
# boundary where a malformed click must not reach the GUI.
FLAG_GATEWAY_ACK_REQUIRED = 0x04
FLAG_DIAGNOSTIC = 0x10
FLAG_COUNT_AS_CLICK = 0x20
FLAG_ERROR = 0x40

GATEWAY_COMMAND_EVENT_FLAG_TERMINAL = 0x01
GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT = 0x02
GATEWAY_COMMAND_EVENT_FLAG_REPLAY = 0x04
GATEWAY_COMMAND_EVENT_FLAG_DUPLICATE = 0x08
GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED = 6
GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE = 7
GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY = 8
GATEWAY_COMMAND_EVENT_STAGE_COMPLETE = 12
GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE = 0xFF
GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES = 50
RANGE_REPORT_MAX_DISTANCE_SAMPLES = 96
DETECTION_SOURCE_UWB_WAKE_CLAIM = 1
_CLICK_REPORT_IDENTITY_FNV_OFFSET = 2166136261
_CLICK_REPORT_IDENTITY_FNV_PRIME = 16777619


def click_report_session_id(clicker_id: int, event_seq: int) -> int:
    """Return the firmware-compatible transport session for one click.

    Semantic event numbers are local to a clicker. The transport projection
    includes the full clicker ID so equal counters from different clickers
    normally separate in reliable-delivery deduplication. The semantic digest
    makes a rare 32-bit projection collision fail closed.
    """

    if (
        isinstance(clicker_id, bool)
        or not isinstance(clicker_id, int)
        or not 1 <= clicker_id <= 0xFFFFFFFFFFFFFFFF
        or isinstance(event_seq, bool)
        or not isinstance(event_seq, int)
        or not 1 <= event_seq <= 0xFFFFFFFF
    ):
        return 0
    identity = clicker_id.to_bytes(8, "little") + event_seq.to_bytes(4, "little")
    value = _CLICK_REPORT_IDENTITY_FNV_OFFSET
    for byte in identity:
        value ^= byte
        value = (value * _CLICK_REPORT_IDENTITY_FNV_PRIME) & 0xFFFFFFFF
    return value or 1

CMD_REBOOT = 0x0004
CMD_CLEAR_ROUTE = 0x0007
CMD_FORCE_REDISCOVERY = 0x000C
CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104
CMD_SURVEY_START = 0x0105
CMD_SURVEY_PLAN = 0x0106
CMD_SURVEY_CANCEL = 0x0107
CMD_SURVEY_GET_STATUS = 0x0108
TLV_EVENT_SEQ = 0x06
TLV_BATTERY_MV = 0x02
TLV_ERROR_CODE = 0x04
TLV_TIMESTAMP_MS = 0x07
TLV_ANCHOR_ID = 0x0A
TLV_CLICKER_ID = 0x0B
TLV_PEER_ID_LIST = 0x16
TLV_DISTANCE_MM = 0x0C
TLV_QUALITY = 0x0D
TLV_SAMPLE_INDEX = 0x0E
TLV_SAMPLE_COUNT = 0x0F
TLV_COMMAND_ID = 0x10
TLV_COMMAND_STATUS = 0x11
TLV_EXPECTED_NODE_COUNT = 0x78
TLV_DURATION_MS = 0x1A
TLV_REASON = 0x1E
TLV_RANGE_STATUS = 0x21
TLV_UWB_RSL_DBM = 0x24
TLV_DISTANCE_SAMPLES_MM = 0x25
TLV_UWB_CIR_SAMPLE = 0x26
TLV_RANGE_ROUND_INDICES = 0x28
TLV_SEQUENCE_START_TIMESTAMPS_MS = 0x29
TLV_DIAG_STATUS_FLAGS = 0x33
TLV_BURST_ID = 0x34
TLV_EXCHANGE_STRIDE_US = 0x35
TLV_BURST_DURATION_MS = 0x36
TLV_CLICK_LATENCY_MS = 0x37
TLV_UWB_AWAKE_TIME_US = 0x38
TLV_DIAG_BYTES_CAPTURED = 0x39
TLV_DIAG_BYTES_TRANSMITTED = 0x3A
TLV_DIAG_BYTES_TRUNCATED = 0x3B
TLV_DIAG_FRAMES_DROPPED = 0x3C
TLV_REPORT_FRAGMENT_COUNT = 0x3D
TLV_CHANNEL9_REPORT_LATENCY_MS = 0x3E
TLV_GATEWAY_ACK_LATENCY_MS = 0x3F
TLV_CLICKER_DIAG_BYTES = 0x40
TLV_ANCHOR_DIAG_BYTES = 0x41
TLV_PHY_CONFIG_ID = 0x42
TLV_MESH_CH9_REPORT_LATENCY_MS = 0x49
TLV_DISCOVERY_SLOT_COUNT = 0x4C
TLV_UWB_CLOCK_OFFSET_RAW = 0x4D
TLV_UWB_CARRIER_INTEGRATOR = 0x4E
TLV_UWB_CIR_FULL_CHUNK = 0x4F
TLV_UWB_CIR_BYTE_OFFSET = 0x50
TLV_UWB_CIR_TOTAL_BYTES = 0x51
TLV_UWB_CIR_FIRST_PATH_INDEX = 0x52
TLV_UWB_RAW_TIMESTAMPS = 0x53
TLV_UWB_RX_DIAG_BYTES = 0x54
TLV_ATTEMPT_INDEX = 0xA9
TLV_DETECTION_SOURCE = 0xAA
TLV_COMMAND_BUDGET_MS = 0xAB
TLV_OPERATION_POLICY = 0xAE
TLV_GATEWAY_ROUTE_ADV_MODE = 0xAF
GATEWAY_ROUTE_ADV_MODE_ENUMERATION_PREARM = 1
GATEWAY_ROUTE_ADV_MODE_ENUMERATION_SURVEY_PREARM = 2
TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION = 0xB1
TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT = 0xB2
TLV_MESH_ACK_SEMANTIC_IDENTITY = 0xB8
TLV_GATEWAY_HOST_RECEIPT_IDENTITY = 0xBB
TLV_MESH_EVENT_PHASE_SHIFT_MS = 0xBC
TLV_DIAG_FRAGMENT_INDEX = 0x55
TLV_DIAG_FRAGMENT_COUNT = 0x56
TLV_DIAG_SOURCE = 0x57
TLV_UWB_CIR_START_INDEX = 0x58
TLV_NODE_BOOT_COUNTER = 0x6E
TLV_DISCOVERY_ASSIGNMENT_PHASE = 0xA4
TLV_DISCOVERY_ASSIGNMENT_EPOCH = 0xA5
TLV_DISCOVERY_ASSIGNMENT_HASH = 0xA6
TLV_DISCOVERY_ASSIGNMENT_TABLE = 0xA7
TLV_CLICKER_CLOCK_OFFSET_RAW = 0xA8
TLV_SURVEY_PHASE = 0xC0
TLV_SURVEY_GENERATION = 0xC1
TLV_SURVEY_ASSIGNMENT_IDENTITY = 0xC2
TLV_SURVEY_START_DELAY_MS = 0xC3
TLV_SURVEY_SELF_STOP_DELAY_MS = 0xC4
TLV_SURVEY_PLAN_COMMITMENT = 0xC5
TLV_SURVEY_PLAN = 0xC6
TLV_SURVEY_GRAPH = 0xC7
TLV_SURVEY_RESULTS = 0xC8
TLV_SURVEY_STATUS = 0xC9
TLV_SURVEY_PARTIAL_REASONS = 0xCA
TLV_SURVEY_SKIPPED_PLAN = 0xCB

SURVEY_PROTOCOL_VERSION = 1
SURVEY_MAX_ANCHORS = 50
SURVEY_MAX_DEGREE = 4
SURVEY_MAX_PAIRS = 100
SURVEY_NEIGHBOR_BITMAP_BYTES = 7
SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN = 42
SURVEY_EVENT_HEADER_WIRE_LEN = 72
SURVEY_NEIGHBOR_RECORD_WIRE_LEN = 8
SURVEY_PLAN_PAIR_WIRE_LEN = 3
SURVEY_RANGE_RESULT_WIRE_LEN = 8
SURVEY_NO_MEDIAN_MM = -(1 << 31)

SURVEY_EVENT_NEIGHBOR_GRAPH = 1
SURVEY_EVENT_PLAN_ACCEPTED = 2
SURVEY_EVENT_RANGE_PROGRESS = 3
SURVEY_EVENT_TERMINAL = 4

SURVEY_TERMINAL_COMPLETE = 0
SURVEY_TERMINAL_PARTIAL = 1
SURVEY_TERMINAL_ABORTED = 2
SURVEY_TERMINAL_ENUMERATION_FAILED = 3
SURVEY_TERMINAL_BUSY = 4


MESSAGE_NAMES = {
    0x08: "UWB_WAKE_CLAIM",
    0x09: "UWB_DISCOVER",
    0x0A: "UWB_DISCOVERY_REPLY",
    0x0B: "UWB_RANGE_SCHEDULE",
    0x0C: "UWB_MESH",
    0x0D: "UWB_RANGE_RELEASE",
    0x10: "UWB_POLL",
    0x11: "UWB_RESP",
    0x12: "UWB_FINAL",
    0x13: "UWB_REPORT",
    0x14: "UWB_CLICKER_DIAG",
    0x16: "UWB_ANCHOR_DIAG",
    0x17: "UWB_ANCHOR_DIAG_FRAGMENT",
    MSG_CLICK_REPORT: "CLICK_REPORT",
    MSG_SELF_TEST_REPORT: "SELF_TEST_REPORT",
    MSG_ANCHOR_HEARTBEAT: "ANCHOR_HEARTBEAT",
    MSG_MESH_DATA: "MESH_DATA",
    0x31: "MESH_HOP_ACK",
    0x32: "GATEWAY_ACK",
    0x35: "ROUTE_REQ",
    0x36: "ROUTE_REPLY",
    0x37: "MESH_EVENT_PROPOSE",
    0x38: "MESH_EVENT_ACCEPT",
    0x39: "MESH_EVENT_UPDATE",
    0x3A: "MESH_EVENT_END",
    0x3B: "ROUTE_REPLY_ACK",
    0x3C: "GATEWAY_ROUTE_ADV",
    0x3D: "RELAY_BUSY",
    0x3E: "RESULT_BUSY",
    0x3F: "GATEWAY_ROUTE_REQ",
    MSG_COMMAND: "COMMAND",
    MSG_COMMAND_RESULT: "COMMAND_RESULT",
    MSG_GATEWAY_COMMAND_EVENT: "GATEWAY_COMMAND_EVENT",
    0x42: "RESULT_OFFER",
    0x43: "RESULT_GRANT",
    MSG_RESULT_BUNDLE: "RESULT_BUNDLE",
    0x45: "GATEWAY_COLLECTION_EACK",
    MSG_GATEWAY_HOST_RECEIPT: "GATEWAY_HOST_RECEIPT",
    MSG_SURVEY_EVENT: "SURVEY_EVENT",
    0x7F: "ERROR",
}

SHARED_MESSAGE_TYPES = {
    MSG_CLICK_REPORT,
    MSG_SELF_TEST_REPORT,
    MSG_ANCHOR_HEARTBEAT,
    MSG_MESH_DATA,
    0x31,
    0x32,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3A,
    0x3B,
    0x3C,
    0x3D,
    0x3E,
    0x3F,
    MSG_COMMAND,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    0x42,
    0x43,
    MSG_RESULT_BUNDLE,
    0x45,
    MSG_GATEWAY_HOST_RECEIPT,
    MSG_SURVEY_EVENT,
    0x7F,
}

# These identifiers are valid in the shared serial/COBS envelope but have no
# UWB lane. A host receipt is deliberately a serial-only acknowledgement of
# GUI RAM ownership, while the two older gateway message types remain
# telemetry/control compatibility types.
HOST_ONLY_MESSAGE_TYPES = {
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_GATEWAY_HOST_RECEIPT,
    MSG_SURVEY_EVENT,
    0x7F,
}
RF_SHARED_MESSAGE_TYPES = SHARED_MESSAGE_TYPES - HOST_ONLY_MESSAGE_TYPES
# A command event is host-only and remains invalid on UWB, but it is a
# receiptable gateway-stream record after the GUI commits its semantic model.
HOST_RECEIPTABLE_MESSAGE_TYPES = RF_SHARED_MESSAGE_TYPES | {
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_SURVEY_EVENT,
}

COMMAND_NAMES = {
    0x0001: "PING",
    0x0002: "GET_STATUS",
    0x0003: "SET_LED_PATTERN",
    0x0004: "REBOOT",
    0x0005: "SET_ROLE",
    0x0006: "SET_ROUTE",
    0x0007: "CLEAR_ROUTE",
    0x0008: "SET_SCAN_DUTY",
    0x0009: "START_HEARTBEAT",
    0x000A: "STOP_HEARTBEAT",
    CMD_FORCE_REDISCOVERY: "FORCE_REDISCOVERY",
    CMD_ASSIGN_DISCOVERY_SLOTS: "ASSIGN_DISCOVERY_SLOTS",
    CMD_SURVEY_START: "SURVEY_START",
    CMD_SURVEY_PLAN: "SURVEY_PLAN",
    CMD_SURVEY_CANCEL: "SURVEY_CANCEL",
    CMD_SURVEY_GET_STATUS: "SURVEY_GET_STATUS",
    0x8000: "ML_START_COLLECTION",
    0x8001: "ML_START_FAST_RANGING",
    0x8003: "ML_START_LIVE_TRACKING",
    0x8004: "ML_LIVE_TRACKING_HEARTBEAT",
    0x8005: "ML_STOP_LIVE_TRACKING",
}

COMMAND_STATUS_NAMES = {
    0: "OK",
    1: "UNSUPPORTED_COMMAND",
    2: "MALFORMED_PAYLOAD",
    3: "BUSY",
    4: "DENIED",
    5: "TIMEOUT",
    6: "RADIO_ERROR",
    7: "INVALID_STATE",
    8: "INTERNAL_ERROR",
}

RANGE_STATUS_NAMES = {
    0: "OK",
    1: "RX_TIMEOUT",
    2: "RX_ERROR",
    3: "BAD_FRAME",
    4: "WRONG_TARGET",
    5: "STS_QUALITY_FAIL",
    6: "DELAYED_TX_MISSED",
    7: "INTERNAL_ERROR",
    8: "TIMING_INVALID",
}

PACKET_FLAG_NAMES = {
    0x02: "ROUTE_SETUP",
    0x04: "GATEWAY_ACK_REQUIRED",
    0x08: "GATEWAY_ACK",
    0x10: "DIAGNOSTIC",
    0x20: "COUNT_AS_CLICK",
    0x40: "ERROR",
    0x80: "RANGE_ONLY",
}

STREAM_CLASS_NAMES = {
    0: "UNKNOWN",
    1: "CLICK",
    2: "RESULT",
    4: "DIAGNOSTIC",
    5: "STATUS",
}

DIAGNOSTIC_STATUS_NAMES = {
    0x01: "CLICKER_PRESENT",
    0x02: "CLICKER_MISSING",
    0x04: "ANCHOR_PRESENT",
    0x08: "ANCHOR_MISSING",
    0x10: "TRUNCATED",
    0x20: "CAPTURE_FAILED",
    0x40: "CHANNEL9_DELIVERED",
}

DISCOVERY_ASSIGNMENT_PHASE_NAMES = {
    1: "CLAIM",
    2: "TABLE",
    3: "ACK",
}


class DecodeError(ValueError):
    """A transport frame or packet violated the firmware framing contract."""


def _operation_policy(raw: bytes) -> dict[str, Any]:
    try:
        return decode_operation_policy_value(raw)
    except ValueError as exc:
        raise DecodeError(str(exc)) from exc


Decoder = Callable[[bytes], Any]


def _scalar(width: int, *, signed: bool = False) -> Decoder:
    def decode(raw: bytes) -> int:
        if len(raw) != width:
            raise DecodeError(f"expected {width} bytes, got {len(raw)}")
        return int.from_bytes(raw, "little", signed=signed)

    return decode


def _array(width: int, *, signed: bool = False) -> Decoder:
    def decode(raw: bytes) -> list[int]:
        if len(raw) % width != 0:
            raise DecodeError(f"length {len(raw)} is not divisible by {width}")
        return [
            int.from_bytes(raw[offset:offset + width], "little", signed=signed)
            for offset in range(0, len(raw), width)
        ]

    return decode


def _exact_bytes(width: int) -> Decoder:
    def decode(raw: bytes) -> bytes:
        if len(raw) != width:
            raise DecodeError(f"expected {width} bytes, got {len(raw)}")
        return raw

    return decode


def _mesh_ack_semantic_identity(raw: bytes) -> dict[str, Any]:
    if len(raw) != 38:
        raise DecodeError(f"expected 38 bytes, got {len(raw)}")
    session_id = int.from_bytes(raw[:4], "little")
    sequence = int.from_bytes(raw[4:6], "little")
    if session_id == 0 or sequence == 0:
        raise DecodeError("session and sequence must both be nonzero")
    return {
        "session_id": session_id,
        "sequence": sequence,
        "sha256": raw[6:],
    }


def _raw_timestamps(raw: bytes) -> dict[str, int]:
    names = (
        "poll_tx_ts_32",
        "poll_rx_ts_32",
        "resp_tx_ts_32",
        "resp_rx_ts_32",
        "final_tx_ts_32",
        "final_rx_ts_32",
    )
    if len(raw) != len(names) * 4:
        raise DecodeError(f"expected 24 bytes, got {len(raw)}")
    return {
        name: int.from_bytes(raw[index * 4:index * 4 + 4], "little")
        for index, name in enumerate(names)
    }


def _discovery_assignment_table(raw: bytes) -> list[dict[str, int]]:
    entry_len = 17
    if not raw or len(raw) % entry_len != 0:
        raise DecodeError(f"expected a non-empty multiple of {entry_len} bytes, got {len(raw)}")
    return [
        {
            "anchor_id": int.from_bytes(raw[offset:offset + 8], "little"),
            "hash": int.from_bytes(raw[offset + 8:offset + 16], "little"),
            "slot": raw[offset + 16],
        }
        for offset in range(0, len(raw), entry_len)
    ]


@dataclass(frozen=True)
class TlvSpec:
    name: str
    decoder: Decoder | None = None


TLV_SPECS: dict[int, TlvSpec] = {
    0x01: TlvSpec("DEVICE_ROLE", _scalar(1)),
    0x02: TlvSpec("BATTERY_MV", _scalar(2)),
    0x03: TlvSpec("STATUS_BITS", _scalar(4)),
    0x04: TlvSpec("ERROR_CODE", _scalar(2)),
    0x05: TlvSpec("ERROR_DETAIL"),
    TLV_EVENT_SEQ: TlvSpec("EVENT_SEQ", _scalar(4)),
    TLV_TIMESTAMP_MS: TlvSpec("TIMESTAMP_MS", _scalar(8)),
    0x08: TlvSpec("RSSI_DBM", _scalar(1, signed=True)),
    0x09: TlvSpec("UWB_SHORT_ADDR", _scalar(2)),
    TLV_ANCHOR_ID: TlvSpec("ANCHOR_ID", _scalar(8)),
    TLV_CLICKER_ID: TlvSpec("CLICKER_ID", _scalar(8)),
    TLV_DISTANCE_MM: TlvSpec("DISTANCE_MM", _scalar(4, signed=True)),
    TLV_QUALITY: TlvSpec("QUALITY", _scalar(1)),
    TLV_SAMPLE_INDEX: TlvSpec("SAMPLE_INDEX", _scalar(2)),
    TLV_SAMPLE_COUNT: TlvSpec("SAMPLE_COUNT", _scalar(2)),
    TLV_COMMAND_ID: TlvSpec("COMMAND_ID", _scalar(2)),
    TLV_COMMAND_STATUS: TlvSpec("COMMAND_STATUS", _scalar(2)),
    0x12: TlvSpec("REQUESTED_MSG_SEQ", _scalar(2)),
    0x13: TlvSpec("NEXT_HOP_ID", _scalar(8)),
    0x14: TlvSpec("GATEWAY_ID", _scalar(8)),
    TLV_PEER_ID_LIST: TlvSpec("PEER_ID_LIST", _array(8)),
    0x18: TlvSpec("RANGE_FLAGS", _scalar(1)),
    0x19: TlvSpec("LED_PATTERN_ID", _scalar(1)),
    TLV_DURATION_MS: TlvSpec("DURATION_MS", _scalar(4)),
    TLV_COMMAND_BUDGET_MS: TlvSpec("COMMAND_BUDGET_MS", _scalar(4)),
    0x1B: TlvSpec("RETRY_COUNT", _scalar(1)),
    0x1C: TlvSpec("FW_VERSION"),
    0x1D: TlvSpec("UPTIME_MS", _scalar(4)),
    TLV_REASON: TlvSpec("REASON", _scalar(1)),
    0x1F: TlvSpec("INITIATOR_ID", _scalar(8)),
    0x20: TlvSpec("RESPONDER_ID", _scalar(8)),
    TLV_RANGE_STATUS: TlvSpec("RANGE_STATUS", _scalar(1)),
    0x22: TlvSpec("ROUTE_EPOCH", _scalar(4)),
    0x23: TlvSpec("HOP_COUNT", _scalar(1)),
    TLV_UWB_RSL_DBM: TlvSpec("UWB_RSL_DBM", _scalar(1, signed=True)),
    TLV_DISTANCE_SAMPLES_MM: TlvSpec("DISTANCE_SAMPLES_MM", _array(4, signed=True)),
    TLV_UWB_CIR_SAMPLE: TlvSpec("UWB_CIR_SAMPLE"),
    TLV_RANGE_ROUND_INDICES: TlvSpec("RANGE_ROUND_INDICES", _array(1)),
    TLV_SEQUENCE_START_TIMESTAMPS_MS: TlvSpec("SEQUENCE_START_TIMESTAMPS_MS", _array(8)),
    0x2A: TlvSpec("MESH_CHANNEL", _scalar(1)),
    0x2B: TlvSpec("MESH_EVENT_INTERVAL_MS", _scalar(4)),
    0x2C: TlvSpec("MESH_EVENT_WINDOW_MS", _scalar(2)),
    0x2D: TlvSpec("MESH_NEXT_EVENT_TIME_MS", _scalar(4)),
    0x2E: TlvSpec("MESH_EVENT_COUNTER", _scalar(4)),
    0x2F: TlvSpec("MESH_EVENT_GUARD_MS", _scalar(2)),
    0x30: TlvSpec("MESH_CLOCK_SKEW_PPM", _scalar(2, signed=True)),
    0x31: TlvSpec("MESH_MAX_MISSED_EVENTS", _scalar(1)),
    0x32: TlvSpec("MESH_SUPERVISION_TIMEOUT_MS", _scalar(4)),
    TLV_MESH_EVENT_PHASE_SHIFT_MS: TlvSpec(
        "MESH_EVENT_PHASE_SHIFT_MS", _scalar(2)
    ),
    TLV_DIAG_STATUS_FLAGS: TlvSpec("DIAG_STATUS_FLAGS", _scalar(4)),
    TLV_BURST_ID: TlvSpec("BURST_ID", _scalar(4)),
    TLV_EXCHANGE_STRIDE_US: TlvSpec("EXCHANGE_STRIDE_US", _scalar(2)),
    TLV_BURST_DURATION_MS: TlvSpec("BURST_DURATION_MS", _scalar(2)),
    TLV_CLICK_LATENCY_MS: TlvSpec("CLICK_LATENCY_MS", _scalar(4)),
    TLV_UWB_AWAKE_TIME_US: TlvSpec("UWB_AWAKE_TIME_US", _scalar(4)),
    TLV_DIAG_BYTES_CAPTURED: TlvSpec("DIAG_BYTES_CAPTURED", _scalar(4)),
    TLV_DIAG_BYTES_TRANSMITTED: TlvSpec("DIAG_BYTES_TRANSMITTED", _scalar(4)),
    TLV_DIAG_BYTES_TRUNCATED: TlvSpec("DIAG_BYTES_TRUNCATED", _scalar(4)),
    TLV_DIAG_FRAMES_DROPPED: TlvSpec("DIAG_FRAMES_DROPPED", _scalar(4)),
    TLV_REPORT_FRAGMENT_COUNT: TlvSpec("REPORT_FRAGMENT_COUNT", _scalar(2)),
    TLV_CHANNEL9_REPORT_LATENCY_MS: TlvSpec("CHANNEL9_REPORT_LATENCY_MS", _scalar(4)),
    TLV_GATEWAY_ACK_LATENCY_MS: TlvSpec("GATEWAY_ACK_LATENCY_MS", _scalar(4)),
    TLV_CLICKER_DIAG_BYTES: TlvSpec("CLICKER_DIAG_BYTES"),
    TLV_ANCHOR_DIAG_BYTES: TlvSpec("ANCHOR_DIAG_BYTES"),
    TLV_PHY_CONFIG_ID: TlvSpec("PHY_CONFIG_ID", _scalar(1)),
    0x43: TlvSpec("MESH_CHANNEL_SWITCHES", _scalar(4)),
    0x44: TlvSpec("MESH_PLL_READY_FAILURES", _scalar(4)),
    0x45: TlvSpec("MESH_LATE_CHANNEL5_RETURNS", _scalar(4)),
    0x46: TlvSpec("MESH_DEFERRALS", _scalar(4)),
    0x47: TlvSpec("MESH_CH9_EVENT_MISSES", _scalar(4)),
    0x48: TlvSpec("MESH_CHANNEL5_PREEMPTIONS", _scalar(4)),
    TLV_MESH_CH9_REPORT_LATENCY_MS: TlvSpec("MESH_CH9_REPORT_LATENCY_MS", _scalar(4)),
    0x4A: TlvSpec("DISCOVERY_START_DELAY_MS", _scalar(4)),
    0x4B: TlvSpec("DISCOVERY_SLOT_MS", _scalar(2)),
    TLV_DISCOVERY_SLOT_COUNT: TlvSpec("DISCOVERY_SLOT_COUNT", _scalar(1)),
    TLV_UWB_CLOCK_OFFSET_RAW: TlvSpec("UWB_CLOCK_OFFSET_RAW", _scalar(2, signed=True)),
    TLV_UWB_CARRIER_INTEGRATOR: TlvSpec("UWB_CARRIER_INTEGRATOR", _scalar(4, signed=True)),
    TLV_UWB_CIR_FULL_CHUNK: TlvSpec("UWB_CIR_FULL_CHUNK"),
    TLV_UWB_CIR_BYTE_OFFSET: TlvSpec("UWB_CIR_BYTE_OFFSET", _scalar(2)),
    TLV_UWB_CIR_TOTAL_BYTES: TlvSpec("UWB_CIR_TOTAL_BYTES", _scalar(2)),
    TLV_UWB_CIR_FIRST_PATH_INDEX: TlvSpec("UWB_CIR_FIRST_PATH_INDEX", _scalar(2)),
    TLV_UWB_RAW_TIMESTAMPS: TlvSpec("UWB_RAW_TIMESTAMPS", _raw_timestamps),
    TLV_UWB_RX_DIAG_BYTES: TlvSpec("UWB_RX_DIAG_BYTES"),
    TLV_DIAG_FRAGMENT_INDEX: TlvSpec("DIAG_FRAGMENT_INDEX", _scalar(2)),
    TLV_DIAG_FRAGMENT_COUNT: TlvSpec("DIAG_FRAGMENT_COUNT", _scalar(2)),
    TLV_DIAG_SOURCE: TlvSpec("DIAG_SOURCE", _scalar(1)),
    TLV_UWB_CIR_START_INDEX: TlvSpec("UWB_CIR_START_INDEX", _scalar(2)),
    TLV_NODE_BOOT_COUNTER: TlvSpec("NODE_BOOT_COUNTER", _scalar(4)),
    TLV_DISCOVERY_ASSIGNMENT_PHASE: TlvSpec("DISCOVERY_ASSIGNMENT_PHASE", _scalar(1)),
    TLV_DISCOVERY_ASSIGNMENT_EPOCH: TlvSpec("DISCOVERY_ASSIGNMENT_EPOCH", _scalar(4)),
    TLV_DISCOVERY_ASSIGNMENT_HASH: TlvSpec("DISCOVERY_ASSIGNMENT_HASH", _scalar(8)),
    TLV_EXPECTED_NODE_COUNT: TlvSpec("EXPECTED_NODE_COUNT", _scalar(2)),
    TLV_DISCOVERY_ASSIGNMENT_TABLE: TlvSpec(
        "DISCOVERY_ASSIGNMENT_TABLE", _discovery_assignment_table
    ),
    TLV_CLICKER_CLOCK_OFFSET_RAW: TlvSpec("CLICKER_CLOCK_OFFSET_RAW", _scalar(2, signed=True)),
    TLV_ATTEMPT_INDEX: TlvSpec("ATTEMPT_INDEX", _scalar(1)),
    TLV_DETECTION_SOURCE: TlvSpec("DETECTION_SOURCE", _scalar(1)),
    TLV_OPERATION_POLICY: TlvSpec("OPERATION_POLICY", _operation_policy),
    TLV_GATEWAY_ROUTE_ADV_MODE: TlvSpec("GATEWAY_ROUTE_ADV_MODE", _scalar(1)),
    TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION: TlvSpec(
        "DISCOVERY_ASSIGNMENT_SCHEME_VERSION", _scalar(1)
    ),
    TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT: TlvSpec(
        "DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT", _exact_bytes(32)
    ),
    TLV_MESH_ACK_SEMANTIC_IDENTITY: TlvSpec(
        "MESH_ACK_SEMANTIC_IDENTITY", _mesh_ack_semantic_identity
    ),
    TLV_GATEWAY_HOST_RECEIPT_IDENTITY: TlvSpec(
        "GATEWAY_HOST_RECEIPT_IDENTITY",
        _exact_bytes(GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN),
    ),
    TLV_SURVEY_PHASE: TlvSpec("SURVEY_PHASE", _scalar(1)),
    TLV_SURVEY_GENERATION: TlvSpec("SURVEY_GENERATION", _scalar(4)),
    TLV_SURVEY_ASSIGNMENT_IDENTITY: TlvSpec(
        "SURVEY_ASSIGNMENT_IDENTITY",
        _exact_bytes(SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN),
    ),
    TLV_SURVEY_START_DELAY_MS: TlvSpec("SURVEY_START_DELAY_MS", _scalar(4)),
    TLV_SURVEY_SELF_STOP_DELAY_MS: TlvSpec(
        "SURVEY_SELF_STOP_DELAY_MS", _scalar(4)
    ),
    TLV_SURVEY_PLAN_COMMITMENT: TlvSpec(
        "SURVEY_PLAN_COMMITMENT", _exact_bytes(32)
    ),
    TLV_SURVEY_PLAN: TlvSpec("SURVEY_PLAN"),
    TLV_SURVEY_GRAPH: TlvSpec("SURVEY_GRAPH"),
    TLV_SURVEY_RESULTS: TlvSpec("SURVEY_RESULTS"),
    TLV_SURVEY_STATUS: TlvSpec("SURVEY_STATUS", _scalar(1)),
    TLV_SURVEY_PARTIAL_REASONS: TlvSpec(
        "SURVEY_PARTIAL_REASONS", _scalar(2)
    ),
    TLV_SURVEY_SKIPPED_PLAN: TlvSpec("SURVEY_SKIPPED_PLAN"),
}

# Keep every currently assigned protocol TLV named even where the GUI does not
# claim a semantic decoder for its value shape.
_REMAINING_TLV_NAMES = {
    0x59: "MESH_TEST_PACKET_ID",
    0x5A: "MESH_TEST_ATTEMPT",
    0x5B: "MESH_TEST_DROP_COUNT",
    0x5C: "MESH_TEST_ORIGIN_ID",
    0x5D: "MESH_TEST_TARGET_ID",
    0x5E: "MESH_TEST_FLAGS",
    0x5F: "MESH_ACK_SEQ_LIST",
    0x60: "MESH_ACK_PACKET_ID_LIST",
    0x61: "MESH_TEST_PADDING",
    0x62: "MESH_ACK_SESSION_LIST",
    0x63: "FLOOD_EPOCH_ID",
    0x64: "FLOOD_PROFILE_VERSION",
    0x65: "SLOT_SEED",
    0x66: "ACCUMULATED_COST",
    0x67: "PATH_QUALITY_MIN",
    0x68: "RELAY_CAPACITY_STATE",
    0x69: "QUEUE_FREE_HINT",
    0x6A: "CHANNEL9_BUSY_HINT",
    0x6B: "GATEWAY_EPOCH",
    0x6C: "COMMAND_SEQ",
    0x6D: "COLLECTION_EPOCH_ID",
    0x6E: "NODE_BOOT_COUNTER",
    0x6F: "REPLY_NONCE",
    0x70: "METRIC_CRC",
    0x71: "GATEWAY_ROUTE_SEQ",
    0x72: "RETRY_AFTER_MS",
    0x73: "REQUESTED_MSG_SESSION_ID",
    0x74: "ALTERNATE_PARENT_ID",
    0x75: "COMMAND_SCOPE",
    0x76: "COMMAND_RESPONSE_MODE",
    0x77: "MEMBERSHIP_EPOCH",
    0x78: "EXPECTED_NODE_COUNT",
    0x79: "EXECUTE_DELAY_MS",
    0x7A: "COMMAND_EXPIRY_S",
    0x7B: "COLLECTION_SLOT_SEED",
    0x7C: "RESULT_SEQ",
    0x7D: "PAYLOAD_LEN",
    0x7E: "PAYLOAD_CRC",
    0x7F: "CAPACITY_VALIDITY_INTERVAL_MS",
    0x80: "NODE_ID",
    0x81: "PRIORITY",
    0x82: "GRANTED_CHANNEL",
    0x83: "MAX_BYTES",
    0x84: "EVENT_OFFSET_HINT",
    0x85: "BUNDLE_ID",
    0x86: "RECORD_COUNT",
    0x87: "BUNDLE_CRC",
    0x88: "EACK_FORMAT",
    0x89: "RECEIVED_COUNT",
    0x8A: "RETRY_ROUND",
    0x8B: "NEXT_RETRY_SPREAD_MS",
    0x8C: "COLLECTION_OPEN",
    0x8D: "RESULT_RECORD",
    0x8E: "MESH_DUPLICATE_COUNT",
    0x8F: "COLLECTION_PENDING_COUNT",
    0x90: "PARENT_HOLDDOWN_COUNT",
    0x91: "ROUTE_DISCOVERY_ATTEMPTS",
    0x92: "OUTBOX_DELIVERY_STATE",
    0x93: "FLOOD_SUPPRESSION_COUNT",
    0x94: "ROUTE_REPLY_RETRY_COUNT",
    0x95: "BUSY_RESPONSE_COUNT",
    0x96: "EXPECTED_NODE_ID",
    0x97: "MESH_TEST_PACKET_AGE_MS",
    0x98: "MESH_TEST_SELECTED_PARENT_ID",
    0x99: "MESH_TEST_CH9_TIMING_STATE",
    0x9A: "MESH_TEST_PAYLOAD_CRC",
    0x9B: "ROUTE_REQUEST_FLAGS",
    0x9C: "ROUTE_REPLY_RX_DELAY_MS",
    0x9D: "MESH_LOST_PACKET_COUNT",
    0x9E: "FLOOD_RANDOM_BACKOFF_MAX_MS",
    0x9F: "FLOOD_RANDOM_BACKOFF_SLOT_MS",
    0xA0: "FLOOD_RETRY_COUNT",
    0xA1: "FLOOD_PACKET_AGE_MS",
    0xA2: "MESH_CH9_BATCH_ID",
    0xA3: "MESH_CH9_BATCH_FLAGS",
}
for _type_id, _name in _REMAINING_TLV_NAMES.items():
    TLV_SPECS.setdefault(_type_id, TlvSpec(_name))


ID_TLVS = {
    TLV_ANCHOR_ID,
    TLV_CLICKER_ID,
    0x13,
    0x14,
    0x1F,
    0x20,
    0x5C,
    0x5D,
    0x74,
    0x80,
    0x96,
    0x98,
}


@dataclass(frozen=True)
class TlvValue:
    type_id: int
    name: str
    raw: bytes
    decoded: Any = None
    decode_error: str | None = None
    known: bool = True
    truncated: bool = False

    @property
    def display(self) -> str:
        if self.truncated:
            return f"truncated ({self.decode_error or 'partial TLV value'})"
        if self.decode_error:
            return f"invalid ({self.decode_error})"
        if isinstance(self.decoded, bytes) or self.decoded is None:
            return self.raw.hex(" ")
        if self.type_id in ID_TLVS and isinstance(self.decoded, int):
            return format_device_id(self.decoded)
        if self.type_id == TLV_COMMAND_ID and isinstance(self.decoded, int):
            return f"0x{self.decoded:04x} ({COMMAND_NAMES.get(self.decoded, 'UNKNOWN')})"
        if self.type_id == TLV_COMMAND_STATUS and isinstance(self.decoded, int):
            return f"{self.decoded} ({COMMAND_STATUS_NAMES.get(self.decoded, 'UNKNOWN')})"
        if self.type_id == TLV_RANGE_STATUS and isinstance(self.decoded, int):
            return f"{self.decoded} ({RANGE_STATUS_NAMES.get(self.decoded, 'UNKNOWN')})"
        if self.type_id == TLV_DIAG_STATUS_FLAGS and isinstance(self.decoded, int):
            names = bit_names(self.decoded, DIAGNOSTIC_STATUS_NAMES)
            return f"0x{self.decoded:08x}" + (f" ({', '.join(names)})" if names else "")
        if self.type_id == TLV_DISCOVERY_ASSIGNMENT_PHASE and isinstance(self.decoded, int):
            return (
                f"{self.decoded} "
                f"({DISCOVERY_ASSIGNMENT_PHASE_NAMES.get(self.decoded, 'UNKNOWN')})"
            )
        if self.type_id == TLV_DISCOVERY_ASSIGNMENT_HASH and isinstance(self.decoded, int):
            return f"0x{self.decoded:016x}"
        if self.type_id == TLV_DISCOVERY_ASSIGNMENT_TABLE and isinstance(self.decoded, list):
            entries: list[str] = []
            for entry in self.decoded:
                if isinstance(entry, dict):
                    entries.append(
                        f"anchor={format_device_id(entry.get('anchor_id', 0))} "
                        f"hash=0x{entry.get('hash', 0):016x} slot={entry.get('slot', 0)}"
                    )
            return "; ".join(entries)
        if isinstance(self.decoded, list):
            return "[" + ", ".join(str(value) for value in self.decoded) + "]"
        if isinstance(self.decoded, dict):
            return ", ".join(f"{key}={value}" for key, value in self.decoded.items())
        return str(self.decoded)


@dataclass(frozen=True)
class Packet:
    transport: str
    raw_transport: bytes
    raw_packet: bytes | None
    msg_type: int
    flags: int
    src_id: int
    dst_id: int
    session_id: int
    seq: int
    ttl: int | None
    age_ms: int
    age_kind: str
    payload: bytes
    tlvs: tuple[TlvValue, ...]
    stream_class: int | None = None
    stream_priority: int | None = None
    stream_flags: int = 0

    @property
    def message_name(self) -> str:
        return MESSAGE_NAMES.get(self.msg_type, f"UNKNOWN_0x{self.msg_type:02x}")

    @property
    def flag_names(self) -> list[str]:
        return bit_names(self.flags, PACKET_FLAG_NAMES)

    @property
    def is_click_report(self) -> bool:
        return self.msg_type == MSG_CLICK_REPORT

    def first_tlv(self, type_id: int) -> TlvValue | None:
        return next((tlv for tlv in self.tlvs if tlv.type_id == type_id), None)

    def value(self, type_id: int, default: Any = None) -> Any:
        tlv = self.first_tlv(type_id)
        if tlv is None or tlv.decode_error:
            return default
        return tlv.decoded

    def raw_value(self, type_id: int) -> bytes | None:
        tlv = self.first_tlv(type_id)
        return None if tlv is None else tlv.raw


@dataclass(frozen=True)
class GatewayHostReceiptIdentity:
    """Identity committed by a GUI receipt for one gateway stream record."""

    original_msg_type: int
    original_flags: int
    src_id: int
    dst_id: int
    session_id: int
    seq: int
    stream_record_digest: bytes


@dataclass(frozen=True)
class GatewayHostReceiptFrame:
    """Serialized host receipt and its decoded metadata."""

    frame: bytes
    packet: Packet
    identity: GatewayHostReceiptIdentity


@dataclass(frozen=True)
class ClickSample:
    sample_index: int
    distance_mm: int | None
    round_index: int | None
    timestamp_ms: int | None


@dataclass(frozen=True)
class FeedResult:
    packets: tuple[Packet, ...] = ()
    errors: tuple[str, ...] = ()


@dataclass(frozen=True)
class CommandFrame:
    label: str
    command_id: int
    frame: bytes
    packet: Packet


@dataclass(frozen=True)
class SurveyAssignmentIdentity:
    assignment_epoch: int
    table_command_sequence: int
    table_commitment: bytes
    slot_span: int
    max_hop_count: int

    def __post_init__(self) -> None:
        if not 0 < self.assignment_epoch <= 0xFFFFFFFF:
            raise ValueError("survey assignment epoch must be nonzero uint32")
        if not 0 < self.table_command_sequence <= 0xFFFFFFFF:
            raise ValueError("survey table command sequence must be nonzero uint32")
        if len(self.table_commitment) != 32 or not any(self.table_commitment):
            raise ValueError("survey table commitment must be a nonzero SHA-256 digest")
        if not 1 <= self.slot_span <= SURVEY_MAX_ANCHORS:
            raise ValueError("survey slot span must be in 1..50")
        if not 1 <= self.max_hop_count <= 8:
            raise ValueError("survey max hop count must be in 1..8")

    def encode(self) -> bytes:
        return b"".join(
            (
                self.assignment_epoch.to_bytes(4, "little"),
                self.table_command_sequence.to_bytes(4, "little"),
                self.table_commitment,
                bytes((self.slot_span, self.max_hop_count)),
            )
        )

    @classmethod
    def decode(cls, raw: bytes) -> "SurveyAssignmentIdentity":
        if len(raw) != SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN:
            raise DecodeError(
                "survey assignment identity must be exactly "
                f"{SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN} bytes"
            )
        try:
            return cls(
                assignment_epoch=int.from_bytes(raw[0:4], "little"),
                table_command_sequence=int.from_bytes(raw[4:8], "little"),
                table_commitment=raw[8:40],
                slot_span=raw[40],
                max_hop_count=raw[41],
            )
        except ValueError as exc:
            raise DecodeError(str(exc)) from exc


@dataclass(frozen=True)
class SurveyNeighborReport:
    own_slot: int
    heard_slots: frozenset[int]


@dataclass(frozen=True)
class SurveyPlanPair:
    initiator_slot: int
    responder_slot: int
    wave_index: int


@dataclass(frozen=True)
class SurveySkippedPair:
    input_index: int
    first_slot: int
    second_slot: int
    reason: int


@dataclass(frozen=True)
class SurveyRangeResult:
    pair_index: int
    success_count: int
    responder_slot: int
    median_mm: int | None

    @property
    def usable(self) -> bool:
        return self.success_count >= 3 and self.median_mm is not None


@dataclass(frozen=True)
class SurveyEvent:
    kind: int
    status: int
    generation: int
    assignment: SurveyAssignmentIdentity
    partial_reasons: int
    occupied_slots: frozenset[int] = frozenset()
    neighbor_reports: tuple[SurveyNeighborReport, ...] = ()
    plan_pairs: tuple[SurveyPlanPair, ...] = ()
    wave_count: int = 0
    skipped_pairs: tuple[SurveySkippedPair, ...] = ()
    range_results: tuple[SurveyRangeResult, ...] = ()


def _slots_from_bitmap(raw: bytes) -> frozenset[int]:
    return frozenset(
        slot
        for slot in range(SURVEY_MAX_ANCHORS)
        if raw[slot // 8] & (1 << (slot % 8))
    )


def decode_survey_event(packet_or_payload: Packet | bytes) -> SurveyEvent:
    payload = (
        packet_or_payload.payload
        if isinstance(packet_or_payload, Packet)
        else packet_or_payload
    )
    if isinstance(packet_or_payload, Packet) and packet_or_payload.msg_type != MSG_SURVEY_EVENT:
        raise DecodeError("survey event decoder requires MSG_SURVEY_EVENT")
    if len(payload) < SURVEY_EVENT_HEADER_WIRE_LEN:
        raise DecodeError("survey event is shorter than its fixed header")
    if payload[0] != SURVEY_PROTOCOL_VERSION:
        raise DecodeError(f"unsupported survey event version {payload[0]}")
    kind = payload[1]
    status = payload[2]
    graph_count = payload[3]
    generation = int.from_bytes(payload[4:8], "little")
    partial_reasons = int.from_bytes(payload[8:10], "little")
    result_count = payload[10]
    pair_count = payload[11]
    wave_count = payload[12]
    skipped_count = payload[13]
    assignment = SurveyAssignmentIdentity.decode(payload[14:56])
    occupied_mask = int.from_bytes(payload[56:64], "little")
    received_mask = int.from_bytes(payload[64:72], "little")
    if generation == 0 or status > 4 or kind not in (1, 2, 3, 4):
        raise DecodeError("survey event has an invalid identity, status, or kind")
    offset = SURVEY_EVENT_HEADER_WIRE_LEN
    reports: list[SurveyNeighborReport] = []
    plan_pairs: list[SurveyPlanPair] = []
    skipped: list[SurveySkippedPair] = []
    results: list[SurveyRangeResult] = []
    if kind == 1:
        if pair_count or result_count or skipped_count:
            raise DecodeError("neighbor graph event carries unrelated records")
        expected = SURVEY_EVENT_HEADER_WIRE_LEN + graph_count * SURVEY_NEIGHBOR_RECORD_WIRE_LEN
        if len(payload) != expected or graph_count != received_mask.bit_count():
            raise DecodeError("neighbor graph count or length mismatch")
        seen: set[int] = set()
        for _ in range(graph_count):
            raw = payload[offset:offset + SURVEY_NEIGHBOR_RECORD_WIRE_LEN]
            own_slot = raw[0]
            if own_slot >= SURVEY_MAX_ANCHORS or own_slot in seen:
                raise DecodeError("neighbor graph has an invalid or duplicate owner slot")
            heard = _slots_from_bitmap(raw[1:])
            if own_slot in heard or not (received_mask & (1 << own_slot)):
                raise DecodeError("neighbor report violates its owner bitmap")
            reports.append(SurveyNeighborReport(own_slot, heard))
            seen.add(own_slot)
            offset += SURVEY_NEIGHBOR_RECORD_WIRE_LEN
    elif kind == 2:
        if graph_count or result_count or pair_count > SURVEY_MAX_PAIRS or skipped_count > SURVEY_MAX_PAIRS:
            raise DecodeError("plan event has invalid record counts")
        expected = SURVEY_EVENT_HEADER_WIRE_LEN + pair_count * SURVEY_PLAN_PAIR_WIRE_LEN + skipped_count * 4
        if len(payload) != expected:
            raise DecodeError("plan event length mismatch")
        for _ in range(pair_count):
            initiator, responder, wave = payload[offset:offset + 3]
            if initiator >= 50 or responder >= 50 or initiator == responder or wave >= wave_count:
                raise DecodeError("plan event carries an invalid accepted pair")
            plan_pairs.append(SurveyPlanPair(initiator, responder, wave))
            offset += 3
        for _ in range(skipped_count):
            input_index, first, second, reason = payload[offset:offset + 4]
            if input_index >= SURVEY_MAX_PAIRS or reason not in range(1, 8):
                raise DecodeError("plan event carries an invalid skipped pair")
            skipped.append(SurveySkippedPair(input_index, first, second, reason))
            offset += 4
    else:
        if graph_count or pair_count or skipped_count or result_count > SURVEY_MAX_PAIRS:
            raise DecodeError("range event has invalid record counts")
        expected = SURVEY_EVENT_HEADER_WIRE_LEN + result_count * SURVEY_RANGE_RESULT_WIRE_LEN
        if len(payload) != expected:
            raise DecodeError("range event length mismatch")
        seen_pairs: set[int] = set()
        for _ in range(result_count):
            pair_index = payload[offset]
            successes = payload[offset + 1]
            responder = payload[offset + 2]
            reserved = payload[offset + 3]
            median_raw = int.from_bytes(payload[offset + 4:offset + 8], "little", signed=True)
            if (
                pair_index >= SURVEY_MAX_PAIRS
                or pair_index in seen_pairs
                or successes > 5
                or responder >= SURVEY_MAX_ANCHORS
                or reserved
                or (successes == 0) != (median_raw == SURVEY_NO_MEDIAN_MM)
                or (successes and median_raw < 0)
            ):
                raise DecodeError("range event carries an invalid result")
            results.append(
                SurveyRangeResult(
                    pair_index,
                    successes,
                    responder,
                    None if median_raw == SURVEY_NO_MEDIAN_MM else median_raw,
                )
            )
            seen_pairs.add(pair_index)
            offset += SURVEY_RANGE_RESULT_WIRE_LEN
    return SurveyEvent(
        kind=kind,
        status=status,
        generation=generation,
        assignment=assignment,
        partial_reasons=partial_reasons,
        occupied_slots=frozenset(
            slot for slot in range(SURVEY_MAX_ANCHORS) if occupied_mask & (1 << slot)
        ),
        neighbor_reports=tuple(reports),
        plan_pairs=tuple(plan_pairs),
        wave_count=wave_count,
        skipped_pairs=tuple(skipped),
        range_results=tuple(results),
    )


def _mutual_survey_edges(
    event: SurveyEvent,
    excluded_pairs: Iterable[tuple[int, int]] = (),
) -> set[tuple[int, int]]:
    if event.kind != 1:
        raise ValueError("pair selection requires a neighbor graph event")
    heard = {report.own_slot: report.heard_slots for report in event.neighbor_reports}
    excluded = {
        (min(first, second), max(first, second))
        for first, second in excluded_pairs
        if first != second
    }
    return {
        (first, second)
        for first in sorted(event.occupied_slots)
        for second in sorted(event.occupied_slots)
        if first < second
        and second in heard.get(first, frozenset())
        and first in heard.get(second, frozenset())
        and (first, second) not in excluded
    }


def select_degree_balanced_survey_pairs(
    event: SurveyEvent,
    *,
    degree_cap: int = SURVEY_MAX_DEGREE,
    excluded_pairs: Iterable[tuple[int, int]] = (),
) -> tuple[tuple[int, int], ...]:
    """Retained legacy deterministic degree-balanced selector."""

    if not 1 <= degree_cap <= SURVEY_MAX_DEGREE:
        raise ValueError("survey degree cap must be in 1..4")
    candidates = _mutual_survey_edges(event, excluded_pairs)
    degree = {slot: 0 for slot in event.occupied_slots}
    selected: list[tuple[int, int]] = []
    while candidates:
        eligible = [
            edge
            for edge in candidates
            if degree[edge[0]] < degree_cap and degree[edge[1]] < degree_cap
        ]
        if not eligible:
            break
        edge = min(
            eligible,
            key=lambda pair: (
                max(degree[pair[0]], degree[pair[1]]),
                degree[pair[0]] + degree[pair[1]],
                pair[0],
                pair[1],
            ),
        )
        selected.append(edge)
        degree[edge[0]] += 1
        degree[edge[1]] += 1
        candidates.remove(edge)
    return tuple(selected)


_RIGIDITY_PRIME = 2_147_483_647


def _generic_coordinate(value: int) -> tuple[int, int]:
    """Return deterministic pseudo-random coordinates in a prime field."""

    def mix(seed: int) -> int:
        seed = (seed + 0x9E3779B9) & 0xFFFFFFFF
        seed = ((seed ^ (seed >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
        seed = ((seed ^ (seed >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
        return (seed ^ (seed >> 16)) % _RIGIDITY_PRIME

    return mix(2 * value + 1), mix(2 * value + 2)


def _rigidity_row(
    edge: tuple[int, int],
    slot_index: dict[int, int],
) -> list[int]:
    first, second = edge
    first_x, first_y = _generic_coordinate(first)
    second_x, second_y = _generic_coordinate(second)
    delta_x = (first_x - second_x) % _RIGIDITY_PRIME
    delta_y = (first_y - second_y) % _RIGIDITY_PRIME
    row = [0] * (2 * len(slot_index))
    first_index = 2 * slot_index[first]
    second_index = 2 * slot_index[second]
    row[first_index] = delta_x
    row[first_index + 1] = delta_y
    row[second_index] = (-delta_x) % _RIGIDITY_PRIME
    row[second_index + 1] = (-delta_y) % _RIGIDITY_PRIME
    return row


def _basis_with_row(
    basis: dict[int, list[int]],
    row: list[int],
) -> tuple[dict[int, list[int]], bool]:
    reduced = list(row)
    for pivot in sorted(basis):
        factor = reduced[pivot]
        if factor == 0:
            continue
        source = basis[pivot]
        reduced = [
            (value - factor * source_value) % _RIGIDITY_PRIME
            for value, source_value in zip(reduced, source)
        ]
    new_pivot = next(
        (index for index, value in enumerate(reduced) if value),
        None,
    )
    if new_pivot is None:
        return basis, False
    pivot = new_pivot
    inverse = pow(reduced[pivot], -1, _RIGIDITY_PRIME)
    normalized = [(value * inverse) % _RIGIDITY_PRIME for value in reduced]
    updated: dict[int, list[int]] = {}
    for existing_pivot, existing_row in basis.items():
        factor = existing_row[pivot]
        if factor == 0:
            updated[existing_pivot] = existing_row
            continue
        updated[existing_pivot] = [
            (value - factor * new_value) % _RIGIDITY_PRIME
            for value, new_value in zip(existing_row, normalized)
        ]
    updated[pivot] = normalized
    return updated, True


def survey_pair_rigidity_rank(
    pairs: Iterable[tuple[int, int]],
    occupied_slots: Iterable[int],
) -> int:
    """Return deterministic generic 2D rigidity-matroid rank."""

    slots = sorted(set(occupied_slots))
    slot_index = {slot: index for index, slot in enumerate(slots)}
    basis: dict[int, list[int]] = {}
    for edge in pairs:
        first, second = min(edge), max(edge)
        if first == second or first not in slot_index or second not in slot_index:
            raise ValueError("rigidity pair must name two occupied slots")
        basis, _added = _basis_with_row(
            basis,
            _rigidity_row((first, second), slot_index),
        )
    return len(basis)


def _selected_neighbors(
    selected: Iterable[tuple[int, int]],
    slots: Iterable[int],
) -> dict[int, set[int]]:
    neighbors: dict[int, set[int]] = {slot: set() for slot in slots}
    for first, second in selected:
        neighbors[first].add(second)
        neighbors[second].add(first)
    return neighbors


def _shortest_selected_path(
    first: int,
    second: int,
    neighbors: dict[int, set[int]],
) -> int:
    frontier = [(first, 0)]
    seen = {first}
    while frontier:
        current, depth = frontier.pop(0)
        if current == second:
            return depth
        for neighbor in sorted(neighbors[current] - seen):
            seen.add(neighbor)
            frontier.append((neighbor, depth + 1))
    return len(neighbors) + 1


def select_rigidity_aware_survey_pairs(
    event: SurveyEvent,
    *,
    degree_cap: int = SURVEY_MAX_DEGREE,
    excluded_pairs: Iterable[tuple[int, int]] = (),
) -> tuple[tuple[int, int], ...]:
    """Select a connectivity- and rigidity-first degree-capped mutual-edge plan."""

    if not 1 <= degree_cap <= SURVEY_MAX_DEGREE:
        raise ValueError("survey degree cap must be in 1..4")
    slots = sorted(event.occupied_slots)
    candidates = _mutual_survey_edges(event, excluded_pairs)
    candidate_degree = {
        slot: sum(slot in edge for edge in candidates)
        for slot in slots
    }
    degree = {slot: 0 for slot in slots}
    selected: list[tuple[int, int]] = []
    remaining = set(candidates)
    parent = {slot: slot for slot in slots}

    def find(slot: int) -> int:
        root = slot
        while parent[root] != root:
            root = parent[root]
        while parent[slot] != slot:
            next_slot = parent[slot]
            parent[slot] = root
            slot = next_slot
        return root

    def union(first: int, second: int) -> None:
        first_root = find(first)
        second_root = find(second)
        if first_root != second_root:
            parent[second_root] = first_root

    while True:
        eligible = [
            edge
            for edge in remaining
            if find(edge[0]) != find(edge[1])
            and degree[edge[0]] < degree_cap
            and degree[edge[1]] < degree_cap
        ]
        if not eligible:
            break
        edge = min(
            eligible,
            key=lambda item: (
                min(candidate_degree[item[0]], candidate_degree[item[1]]),
                max(degree[item[0]], degree[item[1]]),
                degree[item[0]] + degree[item[1]],
                item,
            ),
        )
        selected.append(edge)
        remaining.remove(edge)
        degree[edge[0]] += 1
        degree[edge[1]] += 1
        union(*edge)

    slot_index = {slot: index for index, slot in enumerate(slots)}
    basis: dict[int, list[int]] = {}
    for edge in selected:
        basis, _added = _basis_with_row(basis, _rigidity_row(edge, slot_index))
    target_rank = max(0, 2 * len(slots) - 3)
    while len(basis) < target_rank:
        neighbors = _selected_neighbors(selected, slots)
        options: list[tuple[tuple[object, ...], tuple[int, int], dict[int, list[int]]]] = []
        for edge in remaining:
            first, second = edge
            if degree[first] >= degree_cap or degree[second] >= degree_cap:
                continue
            candidate_basis, increases_rank = _basis_with_row(
                basis,
                _rigidity_row(edge, slot_index),
            )
            if not increases_rank:
                continue
            common_neighbors = len(neighbors[first] & neighbors[second])
            needs_degree = int(degree[first] < 2) + int(degree[second] < 2)
            score: tuple[object, ...] = (
                -needs_degree,
                -common_neighbors,
                max(degree[first], degree[second]),
                degree[first] + degree[second],
                min(candidate_degree[first], candidate_degree[second]),
                edge,
            )
            options.append((score, edge, candidate_basis))
        if not options:
            break
        _score, edge, basis = min(options, key=lambda item: item[0])
        selected.append(edge)
        remaining.remove(edge)
        degree[edge[0]] += 1
        degree[edge[1]] += 1

    while True:
        neighbors = _selected_neighbors(selected, slots)
        eligible = [
            edge
            for edge in remaining
            if degree[edge[0]] < degree_cap and degree[edge[1]] < degree_cap
        ]
        if not eligible:
            break
        edge = min(
            eligible,
            key=lambda item: (
                -_shortest_selected_path(item[0], item[1], neighbors),
                max(degree[item[0]], degree[item[1]]),
                degree[item[0]] + degree[item[1]],
                item,
            ),
        )
        selected.append(edge)
        remaining.remove(edge)
        degree[edge[0]] += 1
        degree[edge[1]] += 1
    return tuple(selected)


def select_closest_survey_pairs(
    event: SurveyEvent,
    positions_m_by_slot: dict[int, tuple[float, float]],
    *,
    degree_cap: int = SURVEY_MAX_DEGREE,
) -> tuple[tuple[int, int], ...]:
    """Select a short, connected, rigidity-aware pass from solved positions."""

    if not 1 <= degree_cap <= SURVEY_MAX_DEGREE:
        raise ValueError("survey degree cap must be in 1..4")
    slots = sorted(event.occupied_slots)
    if set(positions_m_by_slot) != set(slots):
        raise ValueError("closest-pair seed must cover every occupied survey slot")
    if not all(
        math.isfinite(value)
        for position in positions_m_by_slot.values()
        for value in position
    ):
        raise ValueError("closest-pair seed coordinates must be finite")
    candidates = _mutual_survey_edges(event)
    distances = {
        edge: math.dist(
            positions_m_by_slot[edge[0]],
            positions_m_by_slot[edge[1]],
        )
        for edge in candidates
    }
    neighbor_rank: dict[int, dict[int, int]] = {slot: {} for slot in slots}
    for slot in slots:
        neighbors = sorted(
            (
                (distances[edge], edge[1] if edge[0] == slot else edge[0])
                for edge in candidates
                if slot in edge
            ),
            key=lambda item: (item[0], item[1]),
        )
        neighbor_rank[slot] = {
            neighbor: rank
            for rank, (_distance, neighbor) in enumerate(neighbors)
        }

    def proximity_score(edge: tuple[int, int]) -> tuple[object, ...]:
        first, second = edge
        first_rank = neighbor_rank[first][second]
        second_rank = neighbor_rank[second][first]
        return (
            max(first_rank, second_rank),
            first_rank + second_rank,
            distances[edge],
            edge,
        )

    degree = {slot: 0 for slot in slots}
    selected: list[tuple[int, int]] = []
    remaining = set(candidates)
    parent = {slot: slot for slot in slots}

    def find(slot: int) -> int:
        root = slot
        while parent[root] != root:
            root = parent[root]
        while parent[slot] != slot:
            next_slot = parent[slot]
            parent[slot] = root
            slot = next_slot
        return root

    def union(first: int, second: int) -> None:
        first_root = find(first)
        second_root = find(second)
        if first_root != second_root:
            parent[second_root] = first_root

    for edge in sorted(remaining, key=proximity_score):
        first, second = edge
        if (
            find(first) == find(second)
            or degree[first] >= degree_cap
            or degree[second] >= degree_cap
        ):
            continue
        selected.append(edge)
        remaining.remove(edge)
        degree[first] += 1
        degree[second] += 1
        union(first, second)

    slot_index = {slot: index for index, slot in enumerate(slots)}
    basis: dict[int, list[int]] = {}
    for edge in selected:
        basis, _added = _basis_with_row(basis, _rigidity_row(edge, slot_index))
    target_rank = max(0, 2 * len(slots) - 3)
    for edge in sorted(remaining, key=proximity_score):
        if len(basis) >= target_rank:
            break
        first, second = edge
        if degree[first] >= degree_cap or degree[second] >= degree_cap:
            continue
        candidate_basis, increases_rank = _basis_with_row(
            basis,
            _rigidity_row(edge, slot_index),
        )
        if not increases_rank:
            continue
        selected.append(edge)
        remaining.remove(edge)
        degree[first] += 1
        degree[second] += 1
        basis = candidate_basis

    for edge in sorted(remaining, key=proximity_score):
        first, second = edge
        if degree[first] >= degree_cap or degree[second] >= degree_cap:
            continue
        selected.append(edge)
        degree[first] += 1
        degree[second] += 1
    return tuple(selected)


def select_survey_pairs(
    event: SurveyEvent,
    *,
    degree_cap: int = SURVEY_MAX_DEGREE,
    strategy: str = "rigidity",
    excluded_pairs: Iterable[tuple[int, int]] = (),
) -> tuple[tuple[int, int], ...]:
    if strategy == "rigidity":
        return select_rigidity_aware_survey_pairs(
            event,
            degree_cap=degree_cap,
            excluded_pairs=excluded_pairs,
        )
    if strategy == "degree-balanced":
        return select_degree_balanced_survey_pairs(
            event,
            degree_cap=degree_cap,
            excluded_pairs=excluded_pairs,
        )
    raise ValueError(f"unknown survey pair strategy: {strategy}")


def bit_names(value: int, names: dict[int, str]) -> list[str]:
    return [name for bit, name in names.items() if value & bit]


def format_device_id(value: int) -> str:
    return f"0x{value:016x}"


def decode_gateway_identity(raw: bytes) -> int:
    if len(raw) != 8:
        raise DecodeError(f"gateway identity must be exactly 8 bytes, received {len(raw)}")
    gateway_id = int.from_bytes(raw, "little")
    if gateway_id == 0:
        raise DecodeError("gateway identity must be non-zero")
    return gateway_id


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


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
            continue
        out.append(byte)
        code += 1
        if code == 0xFF:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
    out[code_index] = code
    return bytes(out)


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


def _truncated_tlv(type_id: int, raw: bytes, warning: str) -> TlvValue:
    spec = TLV_SPECS.get(type_id)
    return TlvValue(
        type_id=type_id,
        name=spec.name if spec is not None else f"UNKNOWN_0x{type_id:02x}",
        raw=raw,
        decoded=raw,
        decode_error=warning,
        known=spec is not None,
        truncated=True,
    )


def parse_tlvs(payload: bytes, *, allow_truncated_tail: bool = False) -> tuple[TlvValue, ...]:
    values: list[TlvValue] = []
    index = 0
    while index < len(payload):
        if len(payload) - index < 2:
            if allow_truncated_tail:
                values.append(
                    _truncated_tlv(
                        payload[index],
                        b"",
                        f"stream payload ended after TLV type at offset {index}; declared length unavailable",
                    )
                )
                break
            raise DecodeError(f"trailing partial TLV header at payload offset {index}")
        type_id = payload[index]
        value_len = payload[index + 1]
        index += 2
        if len(payload) - index < value_len:
            if allow_truncated_tail:
                raw = payload[index:]
                values.append(
                    _truncated_tlv(
                        type_id,
                        raw,
                        f"stream payload ended inside value at offset {index - 2}: "
                        f"declared {value_len} bytes, received {len(raw)}",
                    )
                )
                break
            raise DecodeError(
                f"TLV 0x{type_id:02x} length {value_len} overruns payload at offset {index - 2}"
            )
        raw = payload[index:index + value_len]
        index += value_len
        spec = TLV_SPECS.get(type_id)
        if spec is None:
            values.append(TlvValue(type_id, f"UNKNOWN_0x{type_id:02x}", raw, raw, known=False))
            continue
        if spec.decoder is None:
            values.append(TlvValue(type_id, spec.name, raw, raw))
            continue
        try:
            decoded = spec.decoder(raw)
        except DecodeError as exc:
            values.append(TlvValue(type_id, spec.name, raw, decode_error=str(exc)))
        else:
            values.append(TlvValue(type_id, spec.name, raw, decoded))
    return tuple(values)


_CLICK_SINGLETON_TLVS = {
    TLV_CLICKER_ID,
    TLV_ANCHOR_ID,
    TLV_PEER_ID_LIST,
    TLV_EVENT_SEQ,
    TLV_TIMESTAMP_MS,
    TLV_DISTANCE_MM,
    TLV_QUALITY,
    TLV_RANGE_STATUS,
    TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
    TLV_DISTANCE_SAMPLES_MM,
    TLV_RANGE_ROUND_INDICES,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
    TLV_ATTEMPT_INDEX,
    TLV_DETECTION_SOURCE,
    TLV_BURST_ID,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_START_INDEX,
}

_CLICK_COMMON_TLVS = {
    TLV_CLICKER_ID,
    TLV_ANCHOR_ID,
    TLV_EVENT_SEQ,
    TLV_TIMESTAMP_MS,
}

_CLICK_RANGE_TLVS = {
    TLV_DISTANCE_MM,
    TLV_QUALITY,
    TLV_RANGE_STATUS,
}

_CLICK_SAMPLE_TLVS = {
    TLV_SAMPLE_COUNT,
    TLV_DISTANCE_SAMPLES_MM,
    TLV_RANGE_ROUND_INDICES,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
}

_CLICK_DETECTION_TLVS = {
    TLV_ATTEMPT_INDEX,
    TLV_DETECTION_SOURCE,
}

_CLICK_CIR_FRAGMENT_TLVS = {
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_START_INDEX,
}


def _click_tlv_values(tlvs: tuple[TlvValue, ...], type_id: int) -> list[TlvValue]:
    return [tlv for tlv in tlvs if tlv.type_id == type_id]


def validate_click_payload(packet: Packet, payload: bytes | None = None) -> None:
    """Apply the firmware's semantic ``MSG_CLICK_REPORT`` admission rules.

    Envelope parsers deliberately do not call this function so callers that
    inspect raw packets retain access to malformed payloads.  The gateway
    receive buffer invokes it before emitting a click packet.  ``payload`` is
    optional for native-test parity with ``report_validate_click_payload``;
    when supplied, its TLVs are decoded independently from ``packet.tlvs``.
    """

    if packet.msg_type != MSG_CLICK_REPORT:
        raise DecodeError(
            f"click payload validator requires MSG_CLICK_REPORT, got 0x{packet.msg_type:02x}"
        )

    payload_bytes = packet.payload if payload is None else payload
    if not payload_bytes:
        raise DecodeError("malformed click report: payload is empty")
    tlvs = packet.tlvs if payload is None else parse_tlvs(payload_bytes)
    if any(tlv.truncated for tlv in tlvs):
        raise DecodeError("malformed click report: payload contains a truncated TLV")

    mode_flags = packet.flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)
    allowed_flags = (
        FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC
    )
    if packet.flags & ~allowed_flags:
        raise DecodeError(
            "malformed click report: unrelated protocol flags are not allowed"
        )
    if (packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0:
        raise DecodeError("malformed click report: gateway ACK is required")
    if mode_flags == 0:
        raise DecodeError("malformed click report: exactly one click/diagnostic mode is required")
    if mode_flags == (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC):
        raise DecodeError(
            "malformed click report: click and diagnostic modes are mutually exclusive"
        )

    for type_id in _CLICK_SINGLETON_TLVS:
        occurrences = _click_tlv_values(tlvs, type_id)
        if len(occurrences) > 1:
            name = TLV_SPECS.get(type_id, TlvSpec(f"TLV_0x{type_id:02x}")).name
            raise DecodeError(f"malformed click report: duplicate {name} TLV")

    def required(type_id: int, width: int, name: str) -> TlvValue:
        values = _click_tlv_values(tlvs, type_id)
        if not values:
            raise DecodeError(f"malformed click report: missing required {name} TLV")
        value = values[0]
        if len(value.raw) != width:
            raise DecodeError(
                f"malformed click report: {name} TLV must be {width} bytes, got {len(value.raw)}"
            )
        return value

    def present(type_id: int, name: str) -> TlvValue:
        values = _click_tlv_values(tlvs, type_id)
        if not values:
            raise DecodeError(f"malformed click report: missing required {name} TLV")
        return values[0]

    clicker_tlv = required(TLV_CLICKER_ID, 8, "CLICKER_ID")
    anchor_tlv = required(TLV_ANCHOR_ID, 8, "ANCHOR_ID")
    event_tlv = required(TLV_EVENT_SEQ, 4, "EVENT_SEQ")
    required(TLV_TIMESTAMP_MS, 8, "TIMESTAMP_MS")
    clicker_id = int.from_bytes(clicker_tlv.raw, "little")
    anchor_id = int.from_bytes(anchor_tlv.raw, "little")
    event_seq = int.from_bytes(event_tlv.raw, "little")
    if clicker_id == 0 or anchor_id == 0 or clicker_id == anchor_id:
        raise DecodeError("malformed click report: clicker/anchor IDs must be nonzero and distinct")
    if anchor_id != packet.src_id:
        raise DecodeError("malformed click report: ANCHOR_ID must equal packet source")
    if event_seq == 0 or click_report_session_id(clicker_id, event_seq) != packet.session_id:
        raise DecodeError(
            "malformed click report: packet session must bind CLICKER_ID and EVENT_SEQ"
        )

    participant_values = _click_tlv_values(tlvs, TLV_PEER_ID_LIST)
    if mode_flags == FLAG_COUNT_AS_CLICK:
        participant_tlv = present(TLV_PEER_ID_LIST, "PEER_ID_LIST")
        if (
            len(participant_tlv.raw) < 2 * 8
            or len(participant_tlv.raw) > 4 * 8
            or len(participant_tlv.raw) % 8 != 0
        ):
            raise DecodeError(
                "malformed click report: PEER_ID_LIST must contain 2 through 4 anchor IDs"
            )
        participant_ids = [
            int.from_bytes(participant_tlv.raw[offset:offset + 8], "little")
            for offset in range(0, len(participant_tlv.raw), 8)
        ]
        if any(anchor == 0 for anchor in participant_ids) or any(
            right <= left
            for left, right in zip(participant_ids, participant_ids[1:])
        ):
            raise DecodeError(
                "malformed click report: PEER_ID_LIST must be nonzero, unique, and sorted"
            )
        if anchor_id not in participant_ids:
            raise DecodeError(
                "malformed click report: PEER_ID_LIST must include the reporting anchor"
            )
    elif participant_values:
        raise DecodeError(
            "malformed click report: diagnostic reports must not carry PEER_ID_LIST"
        )

    detection_present = {
        type_id: bool(_click_tlv_values(tlvs, type_id))
        for type_id in _CLICK_DETECTION_TLVS
    }
    if any(detection_present.values()) and not all(detection_present.values()):
        raise DecodeError("malformed click report: detection attempt/source pair is incomplete")
    if detection_present[TLV_ATTEMPT_INDEX]:
        attempt = required(TLV_ATTEMPT_INDEX, 1, "ATTEMPT_INDEX")
        source = required(TLV_DETECTION_SOURCE, 1, "DETECTION_SOURCE")
        if attempt.raw[0] == 0:
            raise DecodeError("malformed click report: ATTEMPT_INDEX must be nonzero")
        if source.raw[0] != DETECTION_SOURCE_UWB_WAKE_CLAIM:
            raise DecodeError("malformed click report: unsupported DETECTION_SOURCE")

    cir_chunks = _click_tlv_values(tlvs, TLV_UWB_CIR_FULL_CHUNK)
    for chunk in cir_chunks:
        if not chunk.raw:
            raise DecodeError("malformed click report: UWB_CIR_FULL_CHUNK must not be empty")
    cir_chunk_bytes = sum(len(chunk.raw) for chunk in cir_chunks)
    cir_metadata_present = any(
        _click_tlv_values(tlvs, type_id) for type_id in _CLICK_CIR_FRAGMENT_TLVS
    )
    if cir_chunks or cir_metadata_present:
        if mode_flags != FLAG_DIAGNOSTIC:
            raise DecodeError("malformed click report: CIR fragments require diagnostic mode")
        fragment_index = int.from_bytes(
            required(TLV_DIAG_FRAGMENT_INDEX, 2, "DIAG_FRAGMENT_INDEX").raw, "little"
        )
        fragment_count = int.from_bytes(
            required(TLV_DIAG_FRAGMENT_COUNT, 2, "DIAG_FRAGMENT_COUNT").raw, "little"
        )
        byte_offset = int.from_bytes(
            required(TLV_UWB_CIR_BYTE_OFFSET, 2, "UWB_CIR_BYTE_OFFSET").raw, "little"
        )
        total_bytes = int.from_bytes(
            required(TLV_UWB_CIR_TOTAL_BYTES, 2, "UWB_CIR_TOTAL_BYTES").raw, "little"
        )
        required(TLV_UWB_CIR_FIRST_PATH_INDEX, 2, "UWB_CIR_FIRST_PATH_INDEX")
        required(TLV_UWB_CIR_START_INDEX, 2, "UWB_CIR_START_INDEX")
        if (
            fragment_count == 0
            or fragment_index >= fragment_count
            or total_bytes == 0
            or byte_offset >= total_bytes
            or cir_chunk_bytes == 0
            or cir_chunk_bytes > total_bytes - byte_offset
        ):
            raise DecodeError("malformed click report: invalid CIR fragment bounds")
        return

    for type_id, width, name in (
        (TLV_DISTANCE_MM, 4, "DISTANCE_MM"),
        (TLV_QUALITY, 1, "QUALITY"),
        (TLV_RANGE_STATUS, 1, "RANGE_STATUS"),
    ):
        required(type_id, width, name)
    quality = required(TLV_QUALITY, 1, "QUALITY").raw[0]
    if quality > 100:
        raise DecodeError("malformed click report: QUALITY must be at most 100")
    range_status = required(TLV_RANGE_STATUS, 1, "RANGE_STATUS").raw[0]
    if range_status > 8 or range_status == 5:
        raise DecodeError("malformed click report: invalid RANGE_STATUS")

    sample_present = any(_click_tlv_values(tlvs, type_id) for type_id in _CLICK_SAMPLE_TLVS)
    sample_index_present = bool(_click_tlv_values(tlvs, TLV_SAMPLE_INDEX))
    if sample_present or sample_index_present:
        sample_count = int.from_bytes(
            required(TLV_SAMPLE_COUNT, 2, "SAMPLE_COUNT").raw, "little"
        )
        sample_values = present(TLV_DISTANCE_SAMPLES_MM, "DISTANCE_SAMPLES_MM")
        round_values = present(TLV_RANGE_ROUND_INDICES, "RANGE_ROUND_INDICES")
        timestamp_values = present(
            TLV_SEQUENCE_START_TIMESTAMPS_MS, "SEQUENCE_START_TIMESTAMPS_MS"
        )
        distance_sample_count = len(sample_values.raw) // 4
        round_index_count = len(round_values.raw)
        timestamp_count = len(timestamp_values.raw) // 8
        sample_index = (
            int.from_bytes(required(TLV_SAMPLE_INDEX, 2, "SAMPLE_INDEX").raw, "little")
            if sample_index_present
            else 0
        )
        if (
            sample_count == 0
            or sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES
            or len(sample_values.raw) == 0
            or len(sample_values.raw) % 4 != 0
            or len(round_values.raw) == 0
            or len(timestamp_values.raw) == 0
            or len(timestamp_values.raw) % 8 != 0
            or distance_sample_count != round_index_count
            or distance_sample_count != timestamp_count
            or sample_index >= sample_count
            or distance_sample_count > sample_count - sample_index
        ):
            raise DecodeError("malformed click report: invalid sample alignment or bounds")

    if mode_flags == FLAG_COUNT_AS_CLICK:
        if not _CLICK_SAMPLE_TLVS.issubset(
            {tlv.type_id for tlv in tlvs}
        ):
            raise DecodeError("malformed click report: count-as-click requires sample fields")
        burst = required(TLV_BURST_ID, 4, "BURST_ID")
        if int.from_bytes(burst.raw, "little") == 0:
            raise DecodeError("malformed click report: BURST_ID must be nonzero")


# Name the entry point after the packet type as well, so callers that do not
# use the firmware's payload-oriented naming can reuse the same admission gate.
validate_click_report = validate_click_payload


def parse_shared_packet_bytes(
    raw: bytes,
    *,
    transport: str = "shared-packet",
    raw_transport: bytes | None = None,
) -> Packet:
    if len(raw) < PACKET_HEADER_LEN + PACKET_CRC_LEN:
        raise DecodeError(f"short shared packet: {len(raw)} bytes")
    if raw[0] != PROTO_MAGIC or raw[1] != PROTO_VERSION:
        raise DecodeError(
            f"bad shared packet magic/version: 0x{raw[0]:02x}/0x{raw[1]:02x}"
        )
    if raw[2] not in SHARED_MESSAGE_TYPES:
        raise DecodeError(f"unsupported shared message type 0x{raw[2]:02x}")

    extended = raw[27] == PACKET_EXT_LENGTH_SENTINEL
    if extended:
        if len(raw) < PACKET_EXT_HEADER_LEN + PACKET_CRC_LEN:
            raise DecodeError(f"short extended shared packet: {len(raw)} bytes")
        payload_len = int.from_bytes(raw[28:30], "little")
        if payload_len < PACKET_EXT_LENGTH_SENTINEL:
            raise DecodeError("extended packet uses a non-extended payload length")
        header_len = PACKET_EXT_HEADER_LEN
        age_ms = int.from_bytes(raw[30:34], "little")
    else:
        payload_len = raw[27]
        header_len = PACKET_HEADER_LEN
        age_ms = int.from_bytes(raw[28:32], "little")
    if payload_len > PACKET_EXT_MAX_PAYLOAD_LEN:
        raise DecodeError(f"payload length {payload_len} exceeds protocol maximum")
    expected_len = header_len + payload_len + PACKET_CRC_LEN
    if len(raw) != expected_len:
        raise DecodeError(f"bad shared packet length: got {len(raw)}, expected {expected_len}")

    expected_crc = int.from_bytes(raw[-2:], "little")
    actual_crc = crc16_ccitt_false(raw[:-2])
    if expected_crc != actual_crc:
        raise DecodeError(
            f"bad shared packet CRC: encoded 0x{expected_crc:04x}, calculated 0x{actual_crc:04x}"
        )
    payload = raw[header_len:header_len + payload_len]
    return Packet(
        transport=transport,
        raw_transport=raw if raw_transport is None else raw_transport,
        raw_packet=raw,
        msg_type=raw[2],
        flags=raw[3],
        src_id=int.from_bytes(raw[4:12], "little"),
        dst_id=int.from_bytes(raw[12:20], "little"),
        session_id=int.from_bytes(raw[20:24], "little"),
        seq=int.from_bytes(raw[24:26], "little"),
        ttl=raw[26],
        age_ms=age_ms,
        age_kind="message_age_ms",
        payload=payload,
        tlvs=() if raw[2] in (MSG_GATEWAY_COMMAND_EVENT, MSG_SURVEY_EVENT) else parse_tlvs(payload),
    )


def parse_cobs_packet(frame: bytes) -> Packet:
    if not frame:
        raise DecodeError("empty COBS frame")
    encoded = frame[:-1] if frame.endswith(b"\x00") else frame
    if not encoded:
        raise DecodeError("empty COBS payload")
    return parse_shared_packet_bytes(
        cobs_decode(encoded),
        transport="cobs-shared-packet",
        raw_transport=frame,
    )


def parse_stream_record(record: bytes) -> Packet:
    if len(record) < GATEWAY_STREAM_RECORD_HEADER_LEN:
        raise DecodeError(f"short gateway stream record: {len(record)} bytes")
    if int.from_bytes(record[0:2], "little") != GATEWAY_STREAM_MAGIC:
        raise DecodeError("bad gateway stream magic")
    if record[2] != GATEWAY_STREAM_VERSION:
        raise DecodeError(f"unsupported gateway stream version {record[2]}")
    if record[3] != GATEWAY_STREAM_RECORD_HEADER_LEN:
        raise DecodeError(f"bad gateway stream header length {record[3]}")
    if record[4] != GATEWAY_STREAM_RECORD_PACKET:
        raise DecodeError(f"unsupported gateway stream record type {record[4]}")
    payload_len = int.from_bytes(record[36:38], "little")
    expected_len = record[3] + payload_len
    if expected_len > GATEWAY_STREAM_RECORD_MAX_LEN:
        raise DecodeError(f"gateway stream record length {expected_len} exceeds maximum")
    if len(record) != expected_len:
        raise DecodeError(f"bad gateway stream length: got {len(record)}, expected {expected_len}")
    payload = record[record[3]:]
    expected_crc = int.from_bytes(record[38:40], "little")
    actual_crc = crc16_ccitt_false(payload)
    if expected_crc != actual_crc:
        raise DecodeError(
            f"bad gateway stream payload CRC: encoded 0x{expected_crc:04x}, calculated 0x{actual_crc:04x}"
        )
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=record,
        raw_packet=None,
        msg_type=record[8],
        flags=record[9],
        src_id=int.from_bytes(record[16:24], "little"),
        dst_id=int.from_bytes(record[24:32], "little"),
        session_id=int.from_bytes(record[12:16], "little"),
        seq=int.from_bytes(record[10:12], "little"),
        ttl=None,
        age_ms=int.from_bytes(record[32:36], "little"),
        age_kind="gateway_queue_age_ms",
        payload=payload,
        tlvs=() if record[8] in (MSG_GATEWAY_COMMAND_EVENT, MSG_SURVEY_EVENT) else parse_tlvs(
            payload, allow_truncated_tail=bool(record[7] & GATEWAY_STREAM_FLAG_TRUNCATED),
        ),
        stream_class=record[5],
        stream_priority=record[6],
        stream_flags=record[7],
    )


def is_gateway_assignment_publisher_event(event: GatewayCommandEvent) -> bool:
    """Return whether an event is one durable assignment-publication item.

    Only this narrow producer domain advances the assignment publication
    barrier. Other retained command events still require an exact host receipt,
    but their stream identity does not represent assignment publication.
    """

    if (
        event.command_kind != 1
        or event.command_id != CMD_ASSIGN_DISCOVERY_SLOTS
        or event.route_epoch == 0
        or event.correlation_id == 0
        or event.gateway_sequence == 0
        or event.host_session_id == 0
        or event.host_sequence == 0
        or event.event_sequence == 0
        or event.correlation_id != event.host_session_id
        or event.flags
        & ~(GATEWAY_COMMAND_EVENT_FLAG_TERMINAL | GATEWAY_COMMAND_EVENT_FLAG_REPLAY)
    ):
        return False

    terminal = bool(event.flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL)
    if event.stage == GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED:
        return (
            not terminal
            and event.anchor_id != 0
            and event.discovery_slot < GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES
            and event.progress_count != 0
            and event.total_count != 0
            and event.success_count + event.failure_count == 1
        )
    if event.stage in (
        GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE,
        GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY,
    ):
        return (
            not terminal
            and event.anchor_id == 0
            and event.pair_initiator_id == 0
            and event.pair_responder_id == 0
            and event.discovery_slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE
        )
    return (
        event.stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE
        and terminal
        and event.anchor_id == 0
        and event.pair_initiator_id == 0
        and event.pair_responder_id == 0
        and event.discovery_slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE
    )


def validate_gateway_command_event_packet(packet: Packet) -> GatewayCommandEvent:
    """Validate one command event before it reaches GUI state.

    All command observability is self-addressed and event-sequence bound.
    Retained generic events and assignment publisher items both carry the
    ACK-required outer marker; publisher classification is a separate concern
    used only by the assignment replay barrier.
    """

    if packet.msg_type != MSG_GATEWAY_COMMAND_EVENT:
        raise DecodeError(
            "gateway command-event validator requires MSG_GATEWAY_COMMAND_EVENT"
        )
    if packet.transport != "gateway-stream-v1":
        raise DecodeError("gateway command events require gateway stream transport")
    if packet.flags not in (0, FLAG_GATEWAY_ACK_REQUIRED) or packet.stream_flags != 0:
        raise DecodeError(
            "gateway command event requires zero or ACK-required envelope flags"
        )
    if (
        packet.src_id == 0
        or packet.dst_id == 0
        or packet.src_id != packet.dst_id
        or packet.session_id == 0
        or packet.seq == 0
    ):
        raise DecodeError(
            "gateway command event requires nonzero self-addressed stream identity"
        )
    try:
        event = decode_gateway_command_event(
            packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES)
        )
    except CommandTelemetryDecodeError as exc:
        raise DecodeError(f"malformed gateway command event: {exc}") from exc

    if (
        event.command_id == 0
        or event.event_sequence == 0
        or event.event_sequence != packet.session_id
        or (event.event_sequence & 0xFFFF) != packet.seq
        or bool(event.flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL)
        != (event.stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE)
    ):
        raise DecodeError("gateway command event identity/stage binding is invalid")
    return event


def validate_gateway_local_command_result_packet(packet: Packet) -> None:
    """Validate the exact self-addressed result built by the gateway.

    Mesh command results retain distinct endpoints and are validated by their
    mesh owner. This local result is a short BLE-only acknowledgement of the
    GUI command and must be canonical before the GUI can receipt it.
    """

    if (
        packet.msg_type != MSG_COMMAND_RESULT
        or packet.transport != "gateway-stream-v1"
        or packet.stream_flags != 0
        or packet.src_id == 0
        or packet.src_id != packet.dst_id
        or packet.session_id == 0
        or packet.seq == 0
        or packet.flags & FLAG_GATEWAY_ACK_REQUIRED == 0
        or packet.flags
        & ~(FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC | FLAG_ERROR)
        or len(packet.payload) != 11
    ):
        raise DecodeError("gateway local command result envelope is invalid")
    payload = packet.payload
    if (
        payload[0:2] != bytes((TLV_COMMAND_ID, 2))
        or payload[4:6] != bytes((TLV_COMMAND_STATUS, 2))
        or payload[8:10] != bytes((TLV_REASON, 1))
    ):
        raise DecodeError("gateway local command result TLVs are not canonical")
    command_id = int.from_bytes(payload[2:4], "little")
    command_status = int.from_bytes(payload[6:8], "little")
    if command_id == 0 or command_status not in COMMAND_STATUS_NAMES:
        raise DecodeError("gateway local command result has invalid command or status")
    if (command_status == 0) != (packet.flags & FLAG_ERROR == 0):
        raise DecodeError("gateway local command result error flag disagrees with status")


def validate_self_test_report_packet(packet: Packet) -> None:
    """Validate the exact clicker self-test record before host acceptance."""

    if (
        packet.msg_type != MSG_SELF_TEST_REPORT
        or packet.flags != (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC)
        or packet.src_id == 0
        or packet.dst_id == 0
        or packet.src_id == packet.dst_id
        or packet.session_id == 0
        or packet.seq == 0
        or (packet.ttl is not None and packet.ttl == 0)
    ):
        raise DecodeError("self-test report envelope is invalid")

    expected = {
        TLV_CLICKER_ID: 8,
        TLV_EVENT_SEQ: 4,
        TLV_ERROR_CODE: 2,
        TLV_BATTERY_MV: 2,
    }
    values: dict[int, TlvValue] = {}
    for tlv in packet.tlvs:
        width = expected.get(tlv.type_id)
        if width is None or tlv.type_id in values or len(tlv.raw) != width:
            raise DecodeError("self-test report TLVs are not canonical")
        values[tlv.type_id] = tlv
    if values.keys() != expected.keys():
        raise DecodeError("self-test report TLVs are incomplete")

    clicker_id = int.from_bytes(values[TLV_CLICKER_ID].raw, "little")
    event_seq = int.from_bytes(values[TLV_EVENT_SEQ].raw, "little")
    failure_code = int.from_bytes(values[TLV_ERROR_CODE].raw, "little")
    expected_seq = event_seq & 0xFFFF
    if expected_seq == 0:
        expected_seq = 1
    if (
        clicker_id != packet.src_id
        or event_seq != packet.session_id
        or expected_seq != packet.seq
        or failure_code > 6
    ):
        raise DecodeError("self-test report identity or result is invalid")


class GatewayReceiveBuffer:
    """Reassembles packet notifications split at arbitrary ATT boundaries."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def reset(self) -> None:
        self._buffer.clear()

    @staticmethod
    def _append_packet(packets: list[Packet], packet: Packet) -> None:
        # Keep parse_shared_packet_bytes/parse_stream_record useful for raw
        # inspection, but make the live BLE receive boundary fail closed for
        # reliable records whose host receipt commits semantic acceptance.
        if packet.msg_type == MSG_CLICK_REPORT:
            validate_click_payload(packet)
        elif packet.msg_type == MSG_SELF_TEST_REPORT:
            validate_self_test_report_packet(packet)
        elif packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
            validate_gateway_command_event_packet(packet)
        elif (
            packet.msg_type == MSG_COMMAND_RESULT
            and packet.src_id == packet.dst_id
        ):
            validate_gateway_local_command_result_packet(packet)
        packets.append(packet)

    def _find_stream_record(self) -> tuple[int, int | None] | None:
        """Find a plausible stream-v1 header in the buffered notification data.

        Gateway stream records have no delimiter, so a dropped ATT prefix can
        leave the buffer in the middle of a record and make a later retry look
        like one oversized legacy COBS frame.  Scan for a fixed, versioned
        header and require a matching payload CRC before treating a complete
        candidate as a record.  A structurally valid but incomplete candidate
        is returned with ``None`` as its length so the caller can retain it for
        the next notification.
        """

        search_from = 0
        partial: tuple[int, int | None] | None = None
        while True:
            offset = self._buffer.find(GATEWAY_STREAM_MAGIC_BYTES, search_from)
            if offset < 0:
                return partial
            search_from = offset + 1
            remaining = len(self._buffer) - offset

            # Validate fields as they become available.  This keeps a random
            # occurrence of ``GW`` inside a legacy COBS frame from pinning the
            # decoder, while still retaining a split retry header.
            if remaining >= 3 and self._buffer[offset + 2] != GATEWAY_STREAM_VERSION:
                continue
            if remaining >= 4 and self._buffer[offset + 3] != GATEWAY_STREAM_RECORD_HEADER_LEN:
                continue
            if remaining >= 5 and self._buffer[offset + 4] != GATEWAY_STREAM_RECORD_PACKET:
                continue
            if remaining >= 6 and self._buffer[offset + 5] > 6:
                continue
            if remaining < GATEWAY_STREAM_RECORD_HEADER_LEN:
                if partial is None:
                    partial = (offset, None)
                continue

            payload_len = int.from_bytes(
                self._buffer[offset + 36:offset + 38], "little"
            )
            record_len = GATEWAY_STREAM_RECORD_HEADER_LEN + payload_len
            if (
                self._buffer[offset + 8] not in SHARED_MESSAGE_TYPES
                or record_len > GATEWAY_STREAM_RECORD_MAX_LEN
            ):
                continue
            if len(self._buffer) - offset < record_len:
                if partial is None:
                    partial = (offset, record_len)
                continue

            payload_start = offset + GATEWAY_STREAM_RECORD_HEADER_LEN
            payload_end = offset + record_len
            expected_crc = int.from_bytes(
                self._buffer[offset + 38:offset + 40], "little"
            )
            actual_crc = crc16_ccitt_false(
                bytes(self._buffer[payload_start:payload_end])
            )
            if expected_crc != actual_crc:
                continue
            return offset, record_len

    def _consume_stream_record(
        self,
        candidate: tuple[int, int | None],
        packets: list[Packet],
        errors: list[str],
    ) -> bool:
        """Consume one stream candidate, returning whether more work is ready."""

        offset, record_len = candidate
        if offset:
            # The bytes before a validated retry are an incomplete/dropped
            # notification suffix.  They cannot be parsed independently, so
            # discard only that prefix and retain the exact record boundary.
            del self._buffer[:offset]
        if record_len is None or len(self._buffer) < record_len:
            return False
        record = bytes(self._buffer[:record_len])
        del self._buffer[:record_len]
        try:
            self._append_packet(packets, parse_stream_record(record))
        except DecodeError as exc:
            errors.append(f"gateway stream record decode failed: {exc}")
        return True

    def feed(self, data: bytes) -> FeedResult:
        self._buffer.extend(data)
        packets: list[Packet] = []
        errors: list[str] = []

        while self._buffer:
            stream_candidate = self._find_stream_record()
            if stream_candidate is not None:
                if not self._consume_stream_record(stream_candidate, packets, errors):
                    break
                continue

            stream_prefix = bytes(self._buffer[:2]) == GATEWAY_STREAM_MAGIC_BYTES
            possible_stream_prefix = (
                len(self._buffer) == 1
                and self._buffer[0] == GATEWAY_STREAM_MAGIC_BYTES[0]
            )
            if possible_stream_prefix:
                break
            if stream_prefix:
                delimiter = self._buffer.find(0)
                if delimiter < 0:
                    errors.append(
                        "invalid gateway stream header without a COBS delimiter; buffer reset"
                    )
                    self._buffer.clear()
                    break
                frame = bytes(self._buffer[:delimiter + 1])
                del self._buffer[:delimiter + 1]
                try:
                    self._append_packet(packets, parse_cobs_packet(frame))
                except DecodeError as exc:
                    errors.append(f"packet notification decode failed: {exc}")
                continue

            delimiter = self._buffer.find(0)
            if delimiter < 0:
                if len(self._buffer) > PACKET_EXT_MAX_PAYLOAD_LEN + PACKET_EXT_HEADER_LEN + 16:
                    errors.append("unterminated packet notification exceeded maximum frame size; buffer reset")
                    self._buffer.clear()
                break
            frame = bytes(self._buffer[:delimiter + 1])
            try:
                packet = parse_cobs_packet(frame)
            except DecodeError as exc:
                # A dropped-prefix stream record can contain zero bytes, which
                # look like COBS delimiters until the gateway retries its full
                # GW/v1 record.  Discard each invalid delimited prefix: the
                # stream scanner above will still find a later complete,
                # CRC-valid GW record, while a corrupt legacy COBS frame can no
                # longer pin every valid legacy frame that follows it.
                del self._buffer[:delimiter + 1]
                errors.append(f"COBS packet notification decode failed: {exc}")
                continue
            del self._buffer[:delimiter + 1]
            try:
                self._append_packet(packets, packet)
            except DecodeError as exc:
                errors.append(f"COBS packet notification decode failed: {exc}")

        return FeedResult(tuple(packets), tuple(errors))


def encode_shared_packet(
    *,
    msg_type: int,
    flags: int,
    src_id: int,
    dst_id: int,
    session_id: int,
    seq: int,
    ttl: int,
    payload: bytes,
    message_age_ms: int = 0,
) -> bytes:
    if msg_type not in SHARED_MESSAGE_TYPES:
        raise ValueError(f"unsupported shared message type 0x{msg_type:02x}")
    if len(payload) > PACKET_EXT_MAX_PAYLOAD_LEN:
        raise ValueError("payload exceeds protocol maximum")
    for name, value, maximum in (
        ("flags", flags, 0xFF),
        ("src_id", src_id, 0xFFFFFFFFFFFFFFFF),
        ("dst_id", dst_id, 0xFFFFFFFFFFFFFFFF),
        ("session_id", session_id, 0xFFFFFFFF),
        ("seq", seq, 0xFFFF),
        ("ttl", ttl, 0xFF),
        ("message_age_ms", message_age_ms, 0xFFFFFFFF),
    ):
        if not 0 <= value <= maximum:
            raise ValueError(f"{name} is outside its encoded range")

    extended = len(payload) >= PACKET_EXT_LENGTH_SENTINEL
    raw = bytearray([PROTO_MAGIC, PROTO_VERSION, msg_type, flags])
    raw.extend(src_id.to_bytes(8, "little"))
    raw.extend(dst_id.to_bytes(8, "little"))
    raw.extend(session_id.to_bytes(4, "little"))
    raw.extend(seq.to_bytes(2, "little"))
    raw.append(ttl)
    if extended:
        raw.append(PACKET_EXT_LENGTH_SENTINEL)
        raw.extend(len(payload).to_bytes(2, "little"))
        raw.extend(message_age_ms.to_bytes(4, "little"))
    else:
        raw.append(len(payload))
        raw.extend(message_age_ms.to_bytes(4, "little"))
    raw.extend(payload)
    raw.extend(crc16_ccitt_false(raw).to_bytes(2, "little"))
    return bytes(raw)


def encode_cobs_packet(**packet_fields: Any) -> bytes:
    return cobs_encode(encode_shared_packet(**packet_fields)) + b"\x00"


def append_tlv(payload: bytearray, type_id: int, value: bytes) -> None:
    if not 0 <= type_id <= 0xFF:
        raise ValueError("TLV type is outside uint8 range")
    if len(value) > 0xFF:
        raise ValueError("TLV value exceeds uint8 length")
    payload.extend((type_id, len(value)))
    payload.extend(value)


def _validate_gateway_host_receipt_identity(
    identity: GatewayHostReceiptIdentity,
    *,
    error_type: type[Exception] = ValueError,
) -> None:
    if identity.original_msg_type not in HOST_RECEIPTABLE_MESSAGE_TYPES:
        raise error_type(
            f"host receipt original message type 0x{identity.original_msg_type:02x} "
            "is not a receiptable gateway-stream record"
        )
    for name, value, maximum in (
        ("original_msg_type", identity.original_msg_type, 0xFF),
        ("original_flags", identity.original_flags, 0xFF),
        ("src_id", identity.src_id, 0xFFFFFFFFFFFFFFFF),
        ("dst_id", identity.dst_id, 0xFFFFFFFFFFFFFFFF),
        ("session_id", identity.session_id, 0xFFFFFFFF),
        ("seq", identity.seq, 0xFFFF),
    ):
        if not isinstance(value, int) or not 0 <= value <= maximum:
            raise error_type(f"{name} is outside its encoded range")
    if identity.src_id == 0 or identity.dst_id == 0:
        raise error_type("host receipt source and destination IDs must be non-zero")
    if identity.original_msg_type == MSG_GATEWAY_COMMAND_EVENT:
        if identity.src_id != identity.dst_id:
            raise error_type(
                "gateway command-event host receipt must be self-addressed"
            )
        if identity.original_flags != FLAG_GATEWAY_ACK_REQUIRED:
            raise error_type(
                "gateway command-event host receipt requires ACK-required flags"
            )
    elif identity.original_msg_type == MSG_COMMAND_RESULT:
        if (identity.original_flags & FLAG_GATEWAY_ACK_REQUIRED) == 0:
            raise error_type(
                "command-result host receipt requires ACK-required flags"
            )
        if identity.src_id == identity.dst_id:
            if identity.original_flags & ~(
                FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC | FLAG_ERROR
            ):
                raise error_type(
                    "gateway-local command-result host receipt has invalid flags"
                )
    elif identity.original_msg_type == MSG_SURVEY_EVENT:
        if identity.src_id != identity.dst_id:
            raise error_type(
                "gateway survey-event host receipt must be self-addressed"
            )
        if identity.original_flags != FLAG_GATEWAY_ACK_REQUIRED:
            raise error_type(
                "gateway survey-event host receipt requires ACK-required flags"
            )
    elif identity.src_id == identity.dst_id:
        raise error_type("host receipt source and destination IDs must differ")
    if identity.session_id == 0 or identity.seq == 0:
        raise error_type("host receipt session and sequence must be non-zero")
    if not isinstance(identity.stream_record_digest, bytes) or len(
        identity.stream_record_digest
    ) != GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN - 24:
        raise error_type("host receipt stream digest must be exactly 32 bytes")


def encode_gateway_host_receipt_identity(
    identity: GatewayHostReceiptIdentity,
) -> bytes:
    _validate_gateway_host_receipt_identity(identity)
    return bytes((identity.original_msg_type, identity.original_flags)) + (
        identity.src_id.to_bytes(8, "little")
        + identity.dst_id.to_bytes(8, "little")
        + identity.session_id.to_bytes(4, "little")
        + identity.seq.to_bytes(2, "little")
        + identity.stream_record_digest
    )


def decode_gateway_host_receipt_identity(value: bytes) -> GatewayHostReceiptIdentity:
    if len(value) != GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN:
        raise DecodeError(
            f"host receipt identity must be exactly "
            f"{GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN} bytes"
        )
    identity = GatewayHostReceiptIdentity(
        original_msg_type=value[0],
        original_flags=value[1],
        src_id=int.from_bytes(value[2:10], "little"),
        dst_id=int.from_bytes(value[10:18], "little"),
        session_id=int.from_bytes(value[18:22], "little"),
        seq=int.from_bytes(value[22:24], "little"),
        stream_record_digest=value[24:56],
    )
    _validate_gateway_host_receipt_identity(identity, error_type=DecodeError)
    return identity


def parse_gateway_host_receipt(packet: Packet) -> GatewayHostReceiptIdentity:
    if packet.msg_type != MSG_GATEWAY_HOST_RECEIPT:
        raise DecodeError("packet is not a gateway host receipt")
    if (
        packet.flags != 0
        or packet.src_id == 0
        or packet.dst_id == 0
        or packet.src_id == packet.dst_id
        or packet.session_id == 0
        or packet.seq == 0
        or packet.ttl != 1
    ):
        raise DecodeError("invalid gateway host receipt envelope")
    if len(packet.payload) != GATEWAY_HOST_RECEIPT_TLV_LEN:
        raise DecodeError("gateway host receipt payload has the wrong length")
    if (
        packet.payload[0] != TLV_GATEWAY_HOST_RECEIPT_IDENTITY
        or packet.payload[1] != GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN
    ):
        raise DecodeError("gateway host receipt must contain one identity TLV")
    return decode_gateway_host_receipt_identity(packet.payload[2:])


def build_gateway_host_receipt(
    packet: Packet,
    *,
    host_id: int,
    gateway_id: int,
) -> GatewayHostReceiptFrame:
    """Build a serial receipt after the GUI has accepted a stream record.

    The digest covers ``packet.raw_transport`` byte-for-byte, including the
    gateway-stream header and payload CRC.  Mirroring the source session and
    sequence in the receipt envelope makes a retry produce the same COBS
    frame, while the fixed identity TLV remains the gateway's authority.
    """
    if packet.transport != "gateway-stream-v1" or not packet.raw_transport:
        raise ValueError("host receipts require one parsed gateway stream record")
    if not isinstance(host_id, int) or not 0 < host_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("host ID must be a non-zero uint64")
    if not isinstance(gateway_id, int) or not 0 < gateway_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("gateway ID must be a non-zero uint64")
    if host_id == gateway_id:
        raise ValueError("host and gateway IDs must differ")
    if packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
        validate_gateway_command_event_packet(packet)
    elif (
        packet.msg_type == MSG_COMMAND_RESULT
        and packet.src_id == packet.dst_id
    ):
        validate_gateway_local_command_result_packet(packet)

    identity = GatewayHostReceiptIdentity(
        original_msg_type=packet.msg_type,
        original_flags=packet.flags,
        src_id=packet.src_id,
        dst_id=packet.dst_id,
        session_id=packet.session_id,
        seq=packet.seq,
        stream_record_digest=hashlib.sha256(packet.raw_transport).digest(),
    )
    identity_value = encode_gateway_host_receipt_identity(identity)
    payload = bytearray()
    append_tlv(payload, TLV_GATEWAY_HOST_RECEIPT_IDENTITY, identity_value)
    frame = encode_cobs_packet(
        msg_type=MSG_GATEWAY_HOST_RECEIPT,
        flags=0,
        src_id=host_id,
        dst_id=gateway_id,
        session_id=packet.session_id,
        seq=packet.seq,
        ttl=1,
        payload=bytes(payload),
    )
    return GatewayHostReceiptFrame(
        frame=frame,
        packet=parse_cobs_packet(frame),
        identity=identity,
    )


def append_operation_policy_tlvs(
    payload: bytearray, values: tuple[bytes, ...]
) -> None:
    for value in values:
        append_tlv(payload, TLV_OPERATION_POLICY, value)


def _build_command_frame(
    *,
    label: str,
    command_id: int,
    host_id: int,
    dst_id: int,
    session_id: int,
    seq: int,
    payload: bytes,
) -> CommandFrame:
    frame = encode_cobs_packet(
        msg_type=MSG_COMMAND,
        flags=0,
        src_id=host_id,
        dst_id=dst_id,
        session_id=session_id,
        seq=seq,
        ttl=1,
        payload=payload,
    )
    return CommandFrame(label, command_id, frame, parse_cobs_packet(frame))


def build_here_i_am_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    operation_policy: OperationPolicyProfile | None = None,
    enumeration_follows: bool = False,
    survey_follows: bool = False,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    if survey_follows and not enumeration_follows:
        raise ValueError("survey prearm requires an enumeration")
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_FORCE_REDISCOVERY.to_bytes(2, "little"))
    if enumeration_follows:
        append_tlv(
            payload,
            TLV_GATEWAY_ROUTE_ADV_MODE,
            bytes((
                GATEWAY_ROUTE_ADV_MODE_ENUMERATION_SURVEY_PREARM
                if survey_follows
                else GATEWAY_ROUTE_ADV_MODE_ENUMERATION_PREARM,
            )),
        )
    if operation_policy is not None:
        append_operation_policy_tlvs(payload, operation_policy.encoded_values())
    return _build_command_frame(
        label=(
            "Survey Enumeration Here I Am"
            if survey_follows
            else "Enumeration Here I Am"
            if enumeration_follows
            else "Here I Am route refresh"
        ),
        command_id=CMD_FORCE_REDISCOVERY,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )

def build_reboot_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_REBOOT.to_bytes(2, "little"))
    return _build_command_frame(
        label="Reboot gateway board",
        command_id=CMD_REBOOT,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_assign_discovery_slots_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    operation_policy: OperationPolicyProfile | None = None,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS.to_bytes(2, "little"))
    if operation_policy is not None:
        append_operation_policy_tlvs(
            payload,
            (operation_policy.assignment.encode_value(),),
        )
    return _build_command_frame(
        label="Assign discovery slots",
        command_id=CMD_ASSIGN_DISCOVERY_SLOTS,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def _validate_survey_command_envelope(
    host_id: int, gateway_id: int, session_id: int, seq: int
) -> None:
    if not 0 < host_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("host ID must be a non-zero uint64")
    if not 0 < gateway_id <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("gateway ID must be a non-zero uint64")
    if host_id == gateway_id:
        raise ValueError("gateway ID must differ from host ID")
    if not 0 < session_id <= 0xFFFFFFFF or not 0 < seq <= 0xFFFF:
        raise ValueError("survey command session and sequence must be nonzero")


def build_survey_start_command(
    *, host_id: int, gateway_id: int, session_id: int, seq: int
) -> CommandFrame:
    _validate_survey_command_envelope(host_id, gateway_id, session_id, seq)
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_START.to_bytes(2, "little"))
    return _build_command_frame(
        label="Start neighbor survey",
        command_id=CMD_SURVEY_START,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_survey_plan_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    generation: int,
    assignment: SurveyAssignmentIdentity,
    pairs: tuple[tuple[int, int], ...],
) -> CommandFrame:
    _validate_survey_command_envelope(host_id, gateway_id, session_id, seq)
    if not 0 < generation <= 0xFFFFFFFF:
        raise ValueError("survey generation must be a nonzero uint32")
    if len(pairs) > SURVEY_MAX_PAIRS:
        raise ValueError("survey plan exceeds the 100-pair cap")
    raw_pairs = bytearray()
    for first, second in pairs:
        if not 0 <= first < SURVEY_MAX_ANCHORS or not 0 <= second < SURVEY_MAX_ANCHORS:
            raise ValueError("survey pair slots must be in 0..49")
        raw_pairs.extend((first, second))
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_PLAN.to_bytes(2, "little"))
    append_tlv(payload, TLV_SURVEY_GENERATION, generation.to_bytes(4, "little"))
    append_tlv(payload, TLV_SURVEY_ASSIGNMENT_IDENTITY, assignment.encode())
    if raw_pairs:
        append_tlv(payload, TLV_SURVEY_PLAN, bytes(raw_pairs))
    return _build_command_frame(
        label="Submit survey ranging plan",
        command_id=CMD_SURVEY_PLAN,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_survey_cancel_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    generation: int,
) -> CommandFrame:
    _validate_survey_command_envelope(host_id, gateway_id, session_id, seq)
    if not 0 < generation <= 0xFFFFFFFF:
        raise ValueError("survey generation must be a nonzero uint32")
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_CANCEL.to_bytes(2, "little"))
    append_tlv(payload, TLV_SURVEY_GENERATION, generation.to_bytes(4, "little"))
    return _build_command_frame(
        label="Cancel survey",
        command_id=CMD_SURVEY_CANCEL,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_survey_get_status_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    generation: int | None = None,
) -> CommandFrame:
    _validate_survey_command_envelope(host_id, gateway_id, session_id, seq)
    if generation is not None and not 0 < generation <= 0xFFFFFFFF:
        raise ValueError("survey generation must be a nonzero uint32")
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_GET_STATUS.to_bytes(2, "little"))
    if generation is not None:
        append_tlv(payload, TLV_SURVEY_GENERATION, generation.to_bytes(4, "little"))
    return _build_command_frame(
        label="Retrieve survey status",
        command_id=CMD_SURVEY_GET_STATUS,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def click_samples(packet: Packet) -> tuple[list[ClickSample], list[str]]:
    distances = packet.value(TLV_DISTANCE_SAMPLES_MM, [])
    rounds = packet.value(TLV_RANGE_ROUND_INDICES, [])
    timestamps = packet.value(TLV_SEQUENCE_START_TIMESTAMPS_MS, [])
    if not isinstance(distances, list):
        distances = []
    if not isinstance(rounds, list):
        rounds = []
    if not isinstance(timestamps, list):
        timestamps = []

    warnings: list[str] = []
    lengths = {
        "distance samples": len(distances),
        "round indices": len(rounds),
        "timestamps": len(timestamps),
    }
    nonzero_lengths = {length for length in lengths.values() if length}
    if len(nonzero_lengths) > 1:
        warnings.append(
            "unaligned sample arrays: " + ", ".join(f"{name}={length}" for name, length in lengths.items())
        )
    row_count = max(lengths.values(), default=0)
    base_index = packet.value(TLV_SAMPLE_INDEX, 0)
    if not isinstance(base_index, int):
        base_index = 0
    rows = [
        ClickSample(
            sample_index=base_index + index,
            distance_mm=distances[index] if index < len(distances) else None,
            round_index=rounds[index] if index < len(rounds) else None,
            timestamp_ms=timestamps[index] if index < len(timestamps) else None,
        )
        for index in range(row_count)
    ]
    total = packet.value(TLV_SAMPLE_COUNT)
    if isinstance(total, int) and base_index + row_count > total:
        warnings.append(
            f"sample chunk ends at {base_index + row_count}, beyond declared SAMPLE_COUNT {total}"
        )
    return rows, warnings


def decode_cir_sample(raw: bytes | None) -> dict[str, int | float | str] | None:
    if raw is None:
        return None
    if len(raw) != 6:
        return {"error": f"expected one 6-byte complex sample, got {len(raw)} bytes"}
    real = int.from_bytes(raw[0:3], "little", signed=True)
    imaginary = int.from_bytes(raw[3:6], "little", signed=True)
    return {
        "raw": raw.hex(" "),
        "real_signed24": real,
        "imag_signed24": imaginary,
        "magnitude": math.hypot(real, imaginary),
    }


def hex_dump(data: bytes, width: int = 16) -> str:
    lines: list[str] = []
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        hex_bytes = " ".join(f"{byte:02x}" for byte in chunk)
        ascii_bytes = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in chunk)
        lines.append(f"{offset:04x}  {hex_bytes:<{width * 3 - 1}}  {ascii_bytes}")
    return "\n".join(lines) if lines else "<empty>"
