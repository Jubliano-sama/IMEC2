"""Host-side IMEC gateway packet framing and decoding.

The constants in this module mirror the current firmware protocol. Keeping the
decoder independent of Zephyr lets the GUI and its tests run on a desktop.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import math
from typing import Any, Callable

from .operation_policy import (
    ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
    DISCOVERY_DEFAULT_ROUND_COUNT,
    DISCOVERY_DEFAULT_SLOT_MS,
    DISCOVERY_DEFAULT_START_DELAY_MS,
    OperationPolicyProfile,
    assignment_required_budget_ms,
    decode_operation_policy_value,
    discovery_required_budget_ms,
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
DEFAULT_HOST_ID = 0xA1C1BEEFC0DE0001
GATEWAY_COMMAND_BUDGET_MIN_MS = 1000
GATEWAY_COMMAND_BUDGET_MAX_MS = 3_600_000
DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS = 751204
ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS = 120000
SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS = 3_600_000
SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT = 5

MSG_CLICK_REPORT = 0x20
MSG_SELF_TEST_REPORT = 0x21
MSG_ANCHOR_HEARTBEAT = 0x22
MSG_MESH_DATA = 0x30
MSG_COMMAND = 0x40
MSG_COMMAND_RESULT = 0x41
MSG_RESULT_BUNDLE = 0x44
MSG_SURVEY_PAIR_RESULT = 0x53
MSG_SURVEY_DISCOVERY_REPORT = 0x55
MSG_GATEWAY_COMMAND_EVENT = 0x56
MSG_GATEWAY_HOST_RECEIPT = 0x57

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
SURVEY_GATEWAY_MAX_PEERS_PER_REPORT = 12
SURVEY_REACHABILITY_ENTRY_LEN = 10

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

CMD_FORCE_REDISCOVERY = 0x000C
CMD_SURVEY_REACHABILITY = 0x0100
CMD_SURVEY_ABORT = 0x0103
CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104
CMD_SURVEY_GO_RETIRED_ID = 0x0105

TLV_EVENT_SEQ = 0x06
TLV_BATTERY_MV = 0x02
TLV_ERROR_CODE = 0x04
TLV_TIMESTAMP_MS = 0x07
TLV_ANCHOR_ID = 0x0A
TLV_CLICKER_ID = 0x0B
TLV_DISTANCE_MM = 0x0C
TLV_QUALITY = 0x0D
TLV_SAMPLE_INDEX = 0x0E
TLV_SAMPLE_COUNT = 0x0F
TLV_COMMAND_ID = 0x10
TLV_COMMAND_STATUS = 0x11
TLV_EXPECTED_NODE_COUNT = 0x78
TLV_SURVEY_ID = 0x15
TLV_REACHABILITY_ENTRY = 0x17
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
TLV_SURVEY_ROUND_ID = 0xAF
TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION = 0xB1
TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT = 0xB2
TLV_SURVEY_OPERATION_GENERATION = 0xB6
TLV_SURVEY_ROUND_COMMITMENT = 0xB7
TLV_MESH_ACK_SEMANTIC_IDENTITY = 0xB8
TLV_GATEWAY_HOST_RECEIPT_IDENTITY = 0xBB
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
    0x15: "UWB_SURVEY_DISCOVERY_PROBE",
    0x16: "UWB_ANCHOR_DIAG",
    0x17: "UWB_ANCHOR_DIAG_FRAGMENT",
    0x18: "UWB_ANCHOR_PAIR_SCHEDULE",
    0x19: "UWB_ANCHOR_PAIR_RESULT",
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
    0x50: "SURVEY_REACH_REQ",
    0x51: "SURVEY_REACH_REPORT",
    0x52: "SURVEY_PAIR_PREPARE",
    MSG_SURVEY_PAIR_RESULT: "SURVEY_PAIR_RESULT",
    0x54: "SURVEY_DISCOVERY_START",
    MSG_SURVEY_DISCOVERY_REPORT: "SURVEY_DISCOVERY_REPORT",
    MSG_GATEWAY_HOST_RECEIPT: "GATEWAY_HOST_RECEIPT",
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
    0x50,
    0x51,
    0x52,
    MSG_SURVEY_PAIR_RESULT,
    0x54,
    MSG_SURVEY_DISCOVERY_REPORT,
    MSG_GATEWAY_HOST_RECEIPT,
    0x7F,
}

# These identifiers are valid in the shared serial/COBS envelope but have no
# UWB lane. A host receipt is deliberately a serial-only acknowledgement of
# GUI RAM ownership, while the two older gateway message types remain
# telemetry/control compatibility types.
HOST_ONLY_MESSAGE_TYPES = {
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_GATEWAY_HOST_RECEIPT,
    0x7F,
}
RF_SHARED_MESSAGE_TYPES = SHARED_MESSAGE_TYPES - HOST_ONLY_MESSAGE_TYPES
# A command event is host-only and remains invalid on UWB, but it is a
# receiptable gateway-stream record after the GUI commits its semantic model.
HOST_RECEIPTABLE_MESSAGE_TYPES = RF_SHARED_MESSAGE_TYPES | {
    MSG_GATEWAY_COMMAND_EVENT,
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
    CMD_SURVEY_REACHABILITY: "SURVEY_REACHABILITY",
    0x0101: "SURVEY_PREPARE_PAIR",
    0x0102: "SURVEY_START_PAIR",
    0x0103: "SURVEY_ABORT",
    CMD_ASSIGN_DISCOVERY_SLOTS: "ASSIGN_DISCOVERY_SLOTS",
    CMD_SURVEY_GO_RETIRED_ID: "RETIRED_SURVEY_GO",
    0x8000: "ML_START_COLLECTION",
    0x8001: "ML_START_FAST_RANGING",
    0x8002: "ML_START_ANCHOR_PAIR_SURVEY",
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
    3: "SURVEY",
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
    TLV_SURVEY_ID: TlvSpec("SURVEY_ID", _scalar(4)),
    0x16: TlvSpec("PEER_ID_LIST", _array(8)),
    TLV_REACHABILITY_ENTRY: TlvSpec("REACHABILITY_ENTRY"),
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
    TLV_SURVEY_ROUND_ID: TlvSpec("SURVEY_ROUND_ID", _scalar(2)),
    TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION: TlvSpec(
        "DISCOVERY_ASSIGNMENT_SCHEME_VERSION", _scalar(1)
    ),
    TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT: TlvSpec(
        "DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT", _exact_bytes(32)
    ),
    TLV_SURVEY_OPERATION_GENERATION: TlvSpec(
        "SURVEY_OPERATION_GENERATION", _scalar(8)
    ),
    TLV_SURVEY_ROUND_COMMITMENT: TlvSpec(
        "SURVEY_ROUND_COMMITMENT", _exact_bytes(32)
    ),
    TLV_MESH_ACK_SEMANTIC_IDENTITY: TlvSpec(
        "MESH_ACK_SEMANTIC_IDENTITY", _mesh_ack_semantic_identity
    ),
    TLV_GATEWAY_HOST_RECEIPT_IDENTITY: TlvSpec(
        "GATEWAY_HOST_RECEIPT_IDENTITY",
        _exact_bytes(GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN),
    ),
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


def validate_survey_discovery_report(
    packet: Packet, payload: bytes | None = None
) -> None:
    """Validate one reliable discovery report before host acceptance.

    The report's transport replay domain is the anchor boot incarnation in
    ``packet.session_id``.  The survey operation generation remains an
    independent mandatory payload value; equating the two would recreate the
    asymmetric-reboot collision this identity split prevents.
    """

    if packet.msg_type != MSG_SURVEY_DISCOVERY_REPORT:
        raise DecodeError(
            "survey discovery validator requires "
            f"MSG_SURVEY_DISCOVERY_REPORT, got 0x{packet.msg_type:02x}"
        )

    payload_bytes = packet.payload if payload is None else payload
    if not payload_bytes:
        raise DecodeError("malformed survey discovery report: payload is empty")
    tlvs = packet.tlvs if payload is None else parse_tlvs(payload_bytes)
    if packet.stream_flags & GATEWAY_STREAM_FLAG_TRUNCATED or any(
        tlv.truncated for tlv in tlvs
    ):
        raise DecodeError(
            "malformed survey discovery report: truncated custody records "
            "cannot be accepted"
        )
    if packet.flags != (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC):
        raise DecodeError(
            "malformed survey discovery report: exact gateway-ACK and "
            "diagnostic flags are required"
        )
    if (
        packet.src_id == 0
        or packet.dst_id == 0
        or packet.src_id == packet.dst_id
        or packet.session_id == 0
        or packet.seq == 0
    ):
        raise DecodeError(
            "malformed survey discovery report: envelope identity must be "
            "nonzero with distinct endpoints"
        )

    def values(type_id: int) -> list[TlvValue]:
        return [tlv for tlv in tlvs if tlv.type_id == type_id]

    required_specs = (
        (TLV_SURVEY_ID, 4, "SURVEY_ID"),
        (TLV_ANCHOR_ID, 8, "ANCHOR_ID"),
        (TLV_SURVEY_OPERATION_GENERATION, 8, "SURVEY_OPERATION_GENERATION"),
        (TLV_NODE_BOOT_COUNTER, 4, "NODE_BOOT_COUNTER"),
        (TLV_COMMAND_STATUS, 2, "COMMAND_STATUS"),
    )
    required_values: dict[int, TlvValue] = {}
    for type_id, width, name in required_specs:
        occurrences = values(type_id)
        if len(occurrences) != 1:
            qualifier = "missing" if not occurrences else "duplicate"
            raise DecodeError(
                f"malformed survey discovery report: {qualifier} required "
                f"{name} TLV"
            )
        value = occurrences[0]
        if len(value.raw) != width:
            raise DecodeError(
                f"malformed survey discovery report: {name} TLV must be "
                f"{width} bytes, got {len(value.raw)}"
            )
        required_values[type_id] = value

    entries = values(TLV_REACHABILITY_ENTRY)
    if len(entries) > SURVEY_GATEWAY_MAX_PEERS_PER_REPORT:
        raise DecodeError(
            "malformed survey discovery report: too many reachability entries"
        )
    expected_types = (
        [TLV_SURVEY_ID, TLV_ANCHOR_ID]
        + [TLV_REACHABILITY_ENTRY] * len(entries)
        + [
            TLV_SURVEY_OPERATION_GENERATION,
            TLV_NODE_BOOT_COUNTER,
            TLV_COMMAND_STATUS,
        ]
    )
    if [tlv.type_id for tlv in tlvs] != expected_types:
        raise DecodeError(
            "malformed survey discovery report: TLVs are not in canonical "
            "producer order or contain an unsupported type"
        )

    survey_id = int.from_bytes(required_values[TLV_SURVEY_ID].raw, "little")
    anchor_id = int.from_bytes(required_values[TLV_ANCHOR_ID].raw, "little")
    operation_generation = int.from_bytes(
        required_values[TLV_SURVEY_OPERATION_GENERATION].raw, "little"
    )
    boot_incarnation = int.from_bytes(
        required_values[TLV_NODE_BOOT_COUNTER].raw, "little"
    )
    command_status = int.from_bytes(
        required_values[TLV_COMMAND_STATUS].raw, "little"
    )
    if survey_id == 0:
        raise DecodeError(
            "malformed survey discovery report: SURVEY_ID must be nonzero"
        )
    if anchor_id == 0 or anchor_id != packet.src_id:
        raise DecodeError(
            "malformed survey discovery report: ANCHOR_ID must be nonzero "
            "and equal packet source"
        )
    if operation_generation == 0 or operation_generation & 0xFFFFFFFF == 0:
        raise DecodeError(
            "malformed survey discovery report: operation generation must "
            "have a nonzero transport projection"
        )
    if boot_incarnation == 0 or packet.session_id != boot_incarnation:
        raise DecodeError(
            "malformed survey discovery report: packet session must equal "
            "the nonzero node boot counter"
        )
    if command_status not in COMMAND_STATUS_NAMES:
        raise DecodeError(
            "malformed survey discovery report: command status is invalid"
        )

    peer_ids: set[int] = set()
    for entry in entries:
        if len(entry.raw) != SURVEY_REACHABILITY_ENTRY_LEN:
            raise DecodeError(
                "malformed survey discovery report: reachability entry must "
                f"be {SURVEY_REACHABILITY_ENTRY_LEN} bytes"
            )
        peer_id = int.from_bytes(entry.raw[:8], "little")
        quality = entry.raw[9]
        if (
            peer_id == 0
            or peer_id == anchor_id
            or peer_id == packet.dst_id
            or peer_id in peer_ids
            or quality > 100
        ):
            raise DecodeError(
                "malformed survey discovery report: reachability endpoints "
                "must be unique, nonzero, distinct from anchor/gateway, and "
                "have quality at most 100"
            )
        peer_ids.add(peer_id)


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
        tlvs=parse_tlvs(payload),
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
        tlvs=() if record[8] == MSG_GATEWAY_COMMAND_EVENT else parse_tlvs(
            payload, allow_truncated_tail=bool(record[7] & GATEWAY_STREAM_FLAG_TRUNCATED),
        ),
        stream_class=record[5],
        stream_priority=record[6],
        stream_flags=record[7],
    )


def is_gateway_assignment_publisher_event(event: GatewayCommandEvent) -> bool:
    """Return whether an event is one durable assignment-publication item.

    Only this narrow producer domain carries an outer ACK-required envelope,
    therefore only it can cross the GUI's receipt/durable-replay boundary.
    Live command progress deliberately remains ordinary telemetry even when
    it shares an enumeration stage number with a published mapping.
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

    All command observability is self-addressed and event-sequence bound, but
    only publisher items carry the ACK-required outer marker. Generic queued,
    progress, and early-terminal telemetry is valid with zero route/assignment
    fields and remains best effort, so it cannot block a host custody head.
    """

    if packet.msg_type != MSG_GATEWAY_COMMAND_EVENT:
        raise DecodeError(
            "gateway command-event validator requires MSG_GATEWAY_COMMAND_EVENT"
        )
    if packet.transport != "gateway-stream-v1":
        raise DecodeError("gateway command events require gateway stream transport")
    if packet.flags not in (0, FLAG_GATEWAY_ACK_REQUIRED) or packet.stream_flags != 0:
        raise DecodeError(
            "gateway command event requires zero or publisher ACK-required envelope flags"
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
    if packet.flags == FLAG_GATEWAY_ACK_REQUIRED and not is_gateway_assignment_publisher_event(event):
        raise DecodeError(
            "ACK-required gateway command event is not a durable assignment publisher item"
        )
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
        elif packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT:
            validate_survey_discovery_report(packet)
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


def build_anchor_discovery_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    survey_id: int,
    duration_ms: int,
    discovery_slot_count: int = 6,
    sample_count: int = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    expected_anchor_count: int | None = None,
    command_budget_ms: int | None = None,
    operation_policy: OperationPolicyProfile | None = None,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    if not 1 <= survey_id <= 0xFFFFFFFF:
        raise ValueError("survey ID must be in 1..0xffffffff")
    if not 1 <= duration_ms <= 0xFFFFFFFF:
        raise ValueError("duration must be in 1..0xffffffff ms")
    if not 1 <= discovery_slot_count <= 50:
        raise ValueError("discovery slot count must be in 1..50")
    if sample_count != SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT:
        raise ValueError(
            "sample count must be exactly "
            f"{SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT}"
        )
    if expected_anchor_count is not None and not (
        1 <= expected_anchor_count <= 50
    ):
        raise ValueError("expected anchor count must be in 1..50")
    if command_budget_ms is not None and not (
        GATEWAY_COMMAND_BUDGET_MIN_MS
        <= command_budget_ms
        <= GATEWAY_COMMAND_BUDGET_MAX_MS
    ):
        raise ValueError(
            f"command budget must be in {GATEWAY_COMMAND_BUDGET_MIN_MS}.."
            f"{GATEWAY_COMMAND_BUDGET_MAX_MS} ms"
        )
    if command_budget_ms is not None:
        required_budget_ms = discovery_required_budget_ms(
            DISCOVERY_DEFAULT_START_DELAY_MS,
            DISCOVERY_DEFAULT_SLOT_MS,
            discovery_slot_count,
            DISCOVERY_DEFAULT_ROUND_COUNT,
            duration_ms,
        )
        if command_budget_ms < required_budget_ms:
            raise ValueError(
                "command budget must cover the selected survey discovery "
                f"policy: minimum {required_budget_ms} ms"
            )
    if operation_policy is not None:
        discovery = operation_policy.discovery
        if duration_ms != discovery.report_grace_ms:
            raise ValueError(
                "legacy duration must equal operation-policy report grace"
            )
        if discovery_slot_count != discovery.slot_count:
            raise ValueError(
                "legacy discovery slot count must equal operation policy"
            )

    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_REACHABILITY.to_bytes(2, "little"))
    append_tlv(payload, TLV_SURVEY_ID, survey_id.to_bytes(4, "little"))
    append_tlv(payload, TLV_DURATION_MS, duration_ms.to_bytes(4, "little"))
    append_tlv(payload, TLV_SAMPLE_COUNT, sample_count.to_bytes(2, "little"))
    append_tlv(payload, TLV_DISCOVERY_SLOT_COUNT, bytes((discovery_slot_count,)))
    if expected_anchor_count is not None:
        append_tlv(
            payload,
            TLV_EXPECTED_NODE_COUNT,
            expected_anchor_count.to_bytes(2, "little"),
        )
    if command_budget_ms is not None:
        append_tlv(payload, TLV_COMMAND_BUDGET_MS, command_budget_ms.to_bytes(4, "little"))
    if operation_policy is not None:
        append_operation_policy_tlvs(
            payload,
            (
                operation_policy.discovery.encode_value(),
                operation_policy.pair.encode_value(),
            ),
        )
    return _build_command_frame(
        label="Anchor survey discovery",
        command_id=CMD_SURVEY_REACHABILITY,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_survey_abort_command(
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
    append_tlv(payload, TLV_COMMAND_ID, CMD_SURVEY_ABORT.to_bytes(2, "little"))
    return _build_command_frame(
        label="Abort active anchor survey",
        command_id=CMD_SURVEY_ABORT,
        host_id=host_id,
        dst_id=gateway_id,
        session_id=session_id,
        seq=seq,
        payload=bytes(payload),
    )


def build_here_i_am_command(
    *,
    host_id: int,
    gateway_id: int,
    session_id: int,
    seq: int,
    command_budget_ms: int | None = None,
    operation_policy: OperationPolicyProfile | None = None,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    if command_budget_ms is not None and not (
        GATEWAY_COMMAND_BUDGET_MIN_MS
        <= command_budget_ms
        <= GATEWAY_COMMAND_BUDGET_MAX_MS
    ):
        raise ValueError(
            f"command budget must be in {GATEWAY_COMMAND_BUDGET_MIN_MS}.."
            f"{GATEWAY_COMMAND_BUDGET_MAX_MS} ms"
        )
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_FORCE_REDISCOVERY.to_bytes(2, "little"))
    if command_budget_ms is not None:
        append_tlv(payload, TLV_COMMAND_BUDGET_MS, command_budget_ms.to_bytes(4, "little"))
    if operation_policy is not None:
        append_operation_policy_tlvs(payload, operation_policy.encoded_values())
    return _build_command_frame(
        label="Here I Am route refresh",
        command_id=CMD_FORCE_REDISCOVERY,
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
    command_budget_ms: int | None = None,
    expected_anchor_count: int | None = None,
    operation_policy: OperationPolicyProfile | None = None,
) -> CommandFrame:
    if host_id == 0:
        raise ValueError("host ID must be non-zero")
    if gateway_id == 0:
        raise ValueError("gateway ID must be non-zero")
    if gateway_id == host_id:
        raise ValueError("gateway ID must differ from host ID")
    if command_budget_ms is not None and not (
        GATEWAY_COMMAND_BUDGET_MIN_MS
        <= command_budget_ms
        <= GATEWAY_COMMAND_BUDGET_MAX_MS
    ):
        raise ValueError(
            "assignment command budget must be in "
            f"{GATEWAY_COMMAND_BUDGET_MIN_MS}.."
            f"{GATEWAY_COMMAND_BUDGET_MAX_MS} ms"
        )
    if expected_anchor_count is not None and not 1 <= expected_anchor_count <= 50:
        raise ValueError("expected anchor count must be in 1..50")
    if command_budget_ms is not None:
        response_spread_ms = (
            operation_policy.assignment.response_spread_ms
            if operation_policy is not None
            else ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS
        )
        required_budget_ms = assignment_required_budget_ms(response_spread_ms)
        if command_budget_ms < required_budget_ms:
            raise ValueError(
                "command budget must cover the selected assignment policy: "
                f"minimum {required_budget_ms} ms"
            )
    if operation_policy is not None:
        assignment = operation_policy.assignment
        policy_expected = assignment.expected_anchor_count
        if expected_anchor_count != (policy_expected or None):
            raise ValueError(
                "legacy expected anchor count must equal assignment operation policy"
            )
        if command_budget_ms != assignment.operation_budget_ms:
            raise ValueError(
                "legacy command budget must equal assignment operation policy"
            )
    payload = bytearray()
    append_tlv(payload, TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS.to_bytes(2, "little"))
    if expected_anchor_count is not None:
        append_tlv(
            payload,
            TLV_EXPECTED_NODE_COUNT,
            expected_anchor_count.to_bytes(2, "little"),
        )
    if command_budget_ms is not None:
        append_tlv(payload, TLV_COMMAND_BUDGET_MS, command_budget_ms.to_bytes(4, "little"))
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
