import subprocess
import sys
import tempfile
from dataclasses import replace
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.gateway_gui.command_telemetry import GatewayCommandEvent, decode_gateway_command_event
from tools.gateway_gui.diagnostic_models import CommandTimelineModel, TopologyBaselineModel, command_run_status
from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    OperationPolicyProfile,
)
from tools.gateway_gui.protocol import (
    COMMAND_STATUS_NAMES,
    DecodeError,
    FLAG_COUNT_AS_CLICK,
    FLAG_DIAGNOSTIC,
    FLAG_GATEWAY_ACK_REQUIRED,
    GatewayReceiveBuffer,
    MSG_CLICK_REPORT,
    TLV_ANCHOR_ID,
    TLV_BURST_ID,
    TLV_CLICKER_ID,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_DISTANCE_MM,
    TLV_DISTANCE_SAMPLES_MM,
    TLV_EVENT_SEQ,
    TLV_QUALITY,
    TLV_RANGE_ROUND_INDICES,
    TLV_RANGE_STATUS,
    TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
    TLV_TIMESTAMP_MS,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_OPERATION_POLICY,
    append_tlv,
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
    encode_cobs_packet,
    parse_cobs_packet,
    validate_click_payload,
)

HOST = 0x484F5354
GATEWAY = 0x9999888877776666
ANCHOR = 0xA7DDDD61D5DD19B3


def firmware_parse(oracle: str, frame: bytes) -> str:
    return subprocess.run((oracle, frame.hex()), check=True, text=True,
                          stdout=subprocess.PIPE).stdout.strip()


CLICKER = 0x1111222233334444
CLICK_ANCHOR = 0x5555666677778888
CLICK_GATEWAY = 0x9999AAAABBBBCCCC
CLICK_SESSION = 0x11223344


def _tlv(type_id: int, value: bytes) -> bytes:
    encoded = bytearray()
    append_tlv(encoded, type_id, value)
    return bytes(encoded)


def _replace_tlv(payload: bytes, type_id: int, value: bytes, *, occurrence: int = 0) -> bytes:
    encoded = bytearray()
    offset = 0
    seen = 0
    replaced = False
    while offset < len(payload):
        current_type = payload[offset]
        value_len = payload[offset + 1]
        raw = payload[offset + 2:offset + 2 + value_len]
        if current_type == type_id and seen == occurrence:
            append_tlv(encoded, type_id, value)
            replaced = True
        else:
            append_tlv(encoded, current_type, raw)
        if current_type == type_id:
            seen += 1
        offset += 2 + value_len
    if not replaced:
        raise AssertionError(f"missing TLV 0x{type_id:02x}")
    return bytes(encoded)


def _remove_tlv(payload: bytes, type_id: int, *, occurrence: int = 0) -> bytes:
    encoded = bytearray()
    offset = 0
    seen = 0
    removed = False
    while offset < len(payload):
        current_type = payload[offset]
        value_len = payload[offset + 1]
        raw = payload[offset + 2:offset + 2 + value_len]
        if current_type == type_id and seen == occurrence:
            removed = True
        else:
            append_tlv(encoded, current_type, raw)
        if current_type == type_id:
            seen += 1
        offset += 2 + value_len
    if not removed:
        raise AssertionError(f"missing TLV 0x{type_id:02x}")
    return bytes(encoded)


def _common_click_payload() -> bytearray:
    payload = bytearray()
    append_tlv(payload, TLV_CLICKER_ID, CLICKER.to_bytes(8, "little"))
    append_tlv(payload, TLV_ANCHOR_ID, CLICK_ANCHOR.to_bytes(8, "little"))
    append_tlv(payload, TLV_EVENT_SEQ, CLICK_SESSION.to_bytes(4, "little"))
    append_tlv(payload, TLV_TIMESTAMP_MS, (1_234_567).to_bytes(8, "little"))
    return payload


def _normal_click_payload() -> bytes:
    payload = _common_click_payload()
    append_tlv(payload, TLV_DISTANCE_MM, (4_512).to_bytes(4, "little", signed=True))
    append_tlv(payload, TLV_QUALITY, b"\x5a")
    append_tlv(payload, TLV_RANGE_STATUS, b"\x00")
    append_tlv(payload, TLV_SAMPLE_COUNT, (5).to_bytes(2, "little"))
    append_tlv(payload, TLV_SAMPLE_INDEX, (2).to_bytes(2, "little"))
    append_tlv(
        payload,
        TLV_DISTANCE_SAMPLES_MM,
        b"".join(value.to_bytes(4, "little", signed=True) for value in (4_500, 4_510, 4_520)),
    )
    append_tlv(payload, TLV_RANGE_ROUND_INDICES, bytes((7, 8, 9)))
    append_tlv(
        payload,
        TLV_SEQUENCE_START_TIMESTAMPS_MS,
        b"".join(value.to_bytes(8, "little") for value in (10_000, 10_050, 10_100)),
    )
    append_tlv(payload, TLV_BURST_ID, (0xABCDEF01).to_bytes(4, "little"))
    return bytes(payload)


def _diagnostic_range_payload() -> bytes:
    payload = _common_click_payload()
    append_tlv(payload, TLV_DISTANCE_MM, (4_512).to_bytes(4, "little", signed=True))
    append_tlv(payload, TLV_QUALITY, b"\x5a")
    append_tlv(payload, TLV_RANGE_STATUS, b"\x00")
    return bytes(payload)


def _cir_payload(fragment_index: int, byte_offset: int, chunks: tuple[bytes, ...]) -> bytes:
    payload = _common_click_payload()
    append_tlv(payload, TLV_DIAG_FRAGMENT_INDEX, fragment_index.to_bytes(2, "little"))
    append_tlv(payload, TLV_DIAG_FRAGMENT_COUNT, (2).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_BYTE_OFFSET, byte_offset.to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_TOTAL_BYTES, (60).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_FIRST_PATH_INDEX, (17).to_bytes(2, "little"))
    append_tlv(payload, TLV_UWB_CIR_START_INDEX, (23).to_bytes(2, "little"))
    for chunk in chunks:
        append_tlv(payload, TLV_UWB_CIR_FULL_CHUNK, chunk)
    return bytes(payload)


def _click_frame(payload: bytes, *, flags: int = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
                 src_id: int = CLICK_ANCHOR, session_id: int = CLICK_SESSION) -> bytes:
    return encode_cobs_packet(
        msg_type=MSG_CLICK_REPORT,
        flags=flags,
        src_id=src_id,
        dst_id=CLICK_GATEWAY,
        session_id=session_id,
        seq=0x1234,
        ttl=4,
        payload=payload,
    )


def _firmware_click_admitted(oracle: str, frame: bytes) -> bool:
    result = subprocess.run(
        (oracle, "--validate-click", frame.hex()),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"firmware click oracle failed to decode a valid envelope: "
            f"rc={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    prefix = "click_validation="
    line = next((line for line in result.stdout.splitlines() if line.startswith(prefix)), None)
    if line is None:
        raise AssertionError(f"firmware click oracle omitted validation result: {result.stdout!r}")
    return int(line[len(prefix):]) == 0


def _python_click_admitted(frame: bytes) -> tuple[bool, bool]:
    try:
        packet = parse_cobs_packet(frame)
        validate_click_payload(packet)
        direct = True
    except DecodeError:
        direct = False
    received = GatewayReceiveBuffer().feed(frame)
    through_buffer = len(received.packets) == 1 and not received.errors
    if direct != through_buffer:
        raise AssertionError(
            f"GatewayReceiveBuffer disagrees with direct click validator: "
            f"direct={direct} packets={len(received.packets)} errors={received.errors!r}"
        )
    return direct, through_buffer


def exercise_click_validator_parity(oracle: str) -> None:
    """Run the live firmware validator and host admission gate on one corpus.

    Every case uses a freshly encoded COBS packet, so the C target executes the
    production ``report_validate_click_payload`` path rather than a source-text
    copy.  The receiver-buffer result is checked against the direct Python
    validator as an additional live-ingress invariant.
    """

    normal = _normal_click_payload()
    diagnostic = _diagnostic_range_payload()
    cir_first = _cir_payload(0, 0, (b"a" * 30,))
    cir_second = _cir_payload(1, 30, (b"b" * 30,))
    cases: list[tuple[str, bytes, int, int, int, bool]] = [
        ("normal", normal, FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, True),
        ("diagnostic range", diagnostic, FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, True),
        ("CIR fragment 0", cir_first, FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, True),
        ("CIR fragment 1", cir_second, FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, True),
        ("missing ACK", normal, FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("missing mode", normal, FLAG_GATEWAY_ACK_REQUIRED, CLICK_ANCHOR, CLICK_SESSION, False),
        ("both modes", normal, FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("missing clicker", _remove_tlv(normal, TLV_CLICKER_ID), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("zero clicker", _replace_tlv(normal, TLV_CLICKER_ID, (0).to_bytes(8, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("same IDs", _replace_tlv(normal, TLV_CLICKER_ID, CLICK_ANCHOR.to_bytes(8, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("anchor source mismatch", normal, FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR + 1, CLICK_SESSION, False),
        ("zero event", _replace_tlv(normal, TLV_EVENT_SEQ, (0).to_bytes(4, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("event/session mismatch", normal, FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION + 1, False),
        ("duplicate quality", normal + _tlv(TLV_QUALITY, b"\x5a"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("quality over 100", _replace_tlv(normal, TLV_QUALITY, b"\x65"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("STS quality fail", _replace_tlv(normal, TLV_RANGE_STATUS, b"\x05"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("unknown range status", _replace_tlv(normal, TLV_RANGE_STATUS, b"\x09"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("missing sample field", _remove_tlv(normal, TLV_RANGE_ROUND_INDICES), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("unaligned samples", _replace_tlv(normal, TLV_DISTANCE_SAMPLES_MM, b"\x01"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("zero sample count", _replace_tlv(normal, TLV_SAMPLE_COUNT, (0).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("sample count over max", _replace_tlv(normal, TLV_SAMPLE_COUNT, (97).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("sample index out of bounds", _replace_tlv(normal, TLV_SAMPLE_INDEX, (5).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("missing burst", _remove_tlv(normal, TLV_BURST_ID), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("zero burst", _replace_tlv(normal, TLV_BURST_ID, (0).to_bytes(4, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("partial detection pair", normal + _tlv(0xA9, b"\x01"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("bad detection source", normal + _tlv(0xA9, b"\x01") + _tlv(0xAA, b"\x02"), FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("missing diagnostic range", _remove_tlv(diagnostic, TLV_DISTANCE_MM), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR missing metadata", _remove_tlv(cir_first, TLV_DIAG_FRAGMENT_INDEX), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR index out of bounds", _replace_tlv(cir_first, TLV_DIAG_FRAGMENT_INDEX, (2).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR zero count", _replace_tlv(cir_first, TLV_DIAG_FRAGMENT_COUNT, (0).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR offset out of bounds", _replace_tlv(cir_first, TLV_UWB_CIR_BYTE_OFFSET, (60).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR chunk exceeds total", _replace_tlv(cir_first, TLV_UWB_CIR_TOTAL_BYTES, (20).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR empty chunk", _replace_tlv(cir_first, TLV_UWB_CIR_FULL_CHUNK, b""), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR in click mode", cir_first, FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK, CLICK_ANCHOR, CLICK_SESSION, False),
        ("CIR duplicate metadata", cir_first + _tlv(TLV_DIAG_FRAGMENT_COUNT, (2).to_bytes(2, "little")), FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC, CLICK_ANCHOR, CLICK_SESSION, False),
    ]
    for name, payload, flags, src_id, session_id, expected in cases:
        frame = _click_frame(payload, flags=flags, src_id=src_id, session_id=session_id)
        firmware = _firmware_click_admitted(oracle, frame)
        python_direct, python_buffer = _python_click_admitted(frame)
        if firmware != python_direct or firmware != python_buffer or firmware != expected:
            raise AssertionError(
                f"click validator parity failed for {name!r}: "
                f"firmware={firmware} python={python_direct}/{python_buffer} expected={expected}"
            )


def event(stage: int, sequence: int, *, slot: int = 255, terminal: bool = False,
          total: int = 0, success: int = 0, lost: int = 4) -> GatewayCommandEvent:
    raw = bytearray(78)
    raw[0:8] = bytes((1, 78, 1, stage, 0x01 if terminal else 0, 1, 0, 0))
    raw[8:10] = (0x0104).to_bytes(2, "little")
    raw[10:12] = (2).to_bytes(2, "little")
    raw[12:16] = (0x12345678).to_bytes(4, "little")
    raw[16:20] = (99).to_bytes(4, "little")
    raw[20:24] = (0x12345678).to_bytes(4, "little")
    raw[24:26] = (17).to_bytes(2, "little")
    raw[28:32] = sequence.to_bytes(4, "little")
    if stage == 6:
        raw[32:40] = ANCHOR.to_bytes(8, "little")
        raw[56:64] = ANCHOR.to_bytes(8, "little")
    raw[64:66] = (1 if stage == 6 else total).to_bytes(2, "little")
    raw[66:68] = total.to_bytes(2, "little")
    raw[68:70] = success.to_bytes(2, "little")
    raw[74:76] = lost.to_bytes(2, "little")
    raw[76] = 1 if stage == 6 else 0
    raw[77] = slot
    return decode_gateway_command_event(bytes(raw), valid_statuses=set(COMMAND_STATUS_NAMES))


def operation_policy_suffix(profile: OperationPolicyProfile) -> str:
    return "".join(
        bytes((TLV_OPERATION_POLICY, len(value))).hex() + value.hex()
        for value in profile.encoded_values()
    )


def main() -> None:
    oracle = sys.argv[1]
    exercise_click_validator_parity(oracle)
    enumeration = build_assign_discovery_slots_command(
        host_id=HOST, gateway_id=GATEWAY, session_id=0x12345678, seq=17)
    parsed = firmware_parse(oracle, enumeration.frame)
    assert "msg_type=64" in parsed and "command_id=260" in parsed
    assert "ttl=1" in parsed and "payload_len=4" in parsed and "payload=10020401" in parsed

    route = build_here_i_am_command(
        host_id=HOST, gateway_id=GATEWAY, session_id=0x12345679, seq=18)
    route_parsed = firmware_parse(oracle, route.frame)
    assert "command_id=12" in route_parsed and "payload=10020c00" in route_parsed

    profile = OperationPolicyProfile(
        assignment=AssignmentOperationPolicy(expected_anchor_count=5)
    )
    policy_suffix = operation_policy_suffix(profile)
    policy_enumeration = build_assign_discovery_slots_command(
        host_id=HOST,
        gateway_id=GATEWAY,
        session_id=0x1234567A,
        seq=19,
        command_budget_ms=profile.assignment.operation_budget_ms,
        expected_anchor_count=profile.assignment.expected_anchor_count,
        operation_policy=profile,
    )
    expected_enumeration_payload = (
        "1002040178020500ab04c9960300" + policy_suffix
    )
    policy_enumeration_parsed = firmware_parse(oracle, policy_enumeration.frame)
    assert "command_id=260" in policy_enumeration_parsed
    assert f"payload_len={len(policy_enumeration.packet.payload)}" in policy_enumeration_parsed
    assert f"payload={expected_enumeration_payload}" in policy_enumeration_parsed

    policy_route = build_here_i_am_command(
        host_id=HOST,
        gateway_id=GATEWAY,
        session_id=0x1234567B,
        seq=20,
        operation_policy=profile,
    )
    policy_route_parsed = firmware_parse(oracle, policy_route.frame)
    assert "command_id=12" in policy_route_parsed
    assert f"payload=10020c00{policy_suffix}" in policy_route_parsed

    policy_survey = build_anchor_discovery_command(
        host_id=HOST,
        gateway_id=GATEWAY,
        session_id=0x1234567C,
        seq=21,
        survey_id=7,
        duration_ms=profile.discovery.report_grace_ms,
        discovery_slot_count=profile.discovery.slot_count,
        sample_count=1,
        expected_anchor_count=5,
        command_budget_ms=profile.discovery.operation_budget_ms,
        operation_policy=profile,
    )
    expected_survey_payload = (
        "100200011504070000001a04fa0000000f0201004c0106"
        "78020500ab04c0270900" + policy_suffix
    )
    policy_survey_parsed = firmware_parse(oracle, policy_survey.frame)
    assert "command_id=256" in policy_survey_parsed
    assert f"payload={expected_survey_payload}" in policy_survey_parsed

    timeline = CommandTimelineModel()
    with tempfile.TemporaryDirectory() as temporary:
        topology = TopologyBaselineModel(Path(temporary) / "baseline.json")
        # Terminal can outrun normal-priority detail; the model must settle by event sequence/count.
        records = (event(1, 40), event(12, 43, terminal=True, total=1, success=1),
                   event(6, 41), event(6, 42, slot=0))
        for record in records:
            timeline.observe(record)
            comparison = topology.observe(record)
        assert comparison is not None and comparison.complete
        assert comparison.actual == (ANCHOR,)
        assert topology.accept_latest().anchor_ids == (ANCHOR,)
        assert command_run_status(timeline.runs()[0][1])[0] == "Succeeded"

        zero = replace(event(12, 50, terminal=True), correlation_id=0x12345679,
                       host_session_id=0x12345679)
        assert not topology.observe(zero).complete

    print("gateway GUI cross-layer scenarios passed")


if __name__ == "__main__":
    main()
