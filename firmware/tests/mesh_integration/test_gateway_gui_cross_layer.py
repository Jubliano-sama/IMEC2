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
    TLV_OPERATION_POLICY,
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
)

HOST = 0x484F5354
GATEWAY = 0x9999888877776666
ANCHOR = 0xA7DDDD61D5DD19B3


def firmware_parse(oracle: str, frame: bytes) -> str:
    return subprocess.run((oracle, frame.hex()), check=True, text=True,
                          stdout=subprocess.PIPE).stdout.strip()


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
        command_budget_ms=profile.discovery.operation_budget_ms,
        operation_policy=profile,
    )
    expected_survey_payload = (
        "100200011504070000001a04fa0000000f0201004c0106"
        "ab04c0270900" + policy_suffix
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
