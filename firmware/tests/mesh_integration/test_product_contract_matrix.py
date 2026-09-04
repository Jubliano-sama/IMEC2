import subprocess
import sys
from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    OperationPolicyProfile,
)
from tools.gateway_gui.protocol import (
    TLV_OPERATION_POLICY,
    build_assign_discovery_slots_command,
    parse_cobs_packet,
)


def _run(oracle: str, *arguments: str) -> str:
    return subprocess.run(
        (oracle, *arguments),
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()


def _policy_tlvs(payload: bytes) -> bytes:
    selected = bytearray()
    offset = 0
    while offset < len(payload):
        if offset + 2 > len(payload):
            raise AssertionError("truncated command TLV")
        type_id = payload[offset]
        value_len = payload[offset + 1]
        end = offset + 2 + value_len
        if end > len(payload):
            raise AssertionError("truncated command TLV value")
        if type_id == TLV_OPERATION_POLICY:
            selected.extend(payload[offset:end])
        offset = end
    if not selected:
        raise AssertionError("GUI command omitted operation-policy TLVs")
    return bytes(selected)


def _fields(line: str) -> dict[str, int]:
    return {
        key: int(value)
        for key, value in (field.split("=", 1) for field in line.split())
    }


def exercise_assignment_policy(oracle: str) -> None:
    for ram_only in (False, True):
        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(
                expected_anchor_count=3,
                ram_only_iteration=ram_only,
            )
        )
        command = build_assign_discovery_slots_command(
            host_id=0x484F5354,
            gateway_id=0x9999888877776666,
            session_id=0x12345679,
            seq=18,
            operation_policy=profile,
        )
        packet = parse_cobs_packet(command.frame)
        firmware = _fields(
            _run(oracle, "--policy", _policy_tlvs(packet.payload).hex())
        )
        assert firmware == {
            "expected": 3,
            "assignment_budget": profile.assignment.operation_budget_ms,
            "assignment_spread": profile.assignment.response_spread_ms,
            "ram_only": int(ram_only),
        }


def exercise_clean_slate_depth_order(oracle: str) -> None:
    ordered = _run(oracle, "--order").split(",")
    parsed = [(int(item.split(":")[0]), int(item.split(":")[1])) for item in ordered]
    assert [hop for _anchor, hop in parsed] == [0, 0, 1, 2]
    assert len({anchor for anchor, _hop in parsed}) == 4


def exercise_gateway_control_priority(oracle: str) -> None:
    fields = _fields(_run(oracle, "--control"))
    assert fields == {
        "enumeration": 1,
        "here_i_am": 1,
        "unrelated": 0,
    }


def exercise_production_owner_source_invariants() -> None:
    assignment = (
        REPO / "firmware/app/src/app_anchor_gateway_control.inc"
    ).read_text(encoding="utf-8")
    start = assignment.index("static int gateway_start_discovery_assignment(")
    first_claim_round = assignment.index(
        "gateway_discovery_assignment_open_claim_round_locked()", start
    )
    clear = assignment.index(
        "gateway_clear_registered_membership_roster();", start
    )
    assert start < clear < first_claim_round
    assert "prior_anchor_ids" not in assignment

    reject_excess = assignment.index(
        'DBG_ENUM_REJECT reason=expected-count-closed'
    )
    wake_closed = assignment.index(
        'gateway_discovery_assignment_wake_now(\n'
        '                    "expected-count-closed")'
    )
    publish = assignment.index(
        "gateway_discovery_assignment_publish_table();", wake_closed
    )
    assert reject_excess < wake_closed < publish
    assert "GATEWAY_ASSIGNMENT_RETURN(-ENOSPC)" in assignment[
        reject_excess:reject_excess + 300
    ]
    assert (
        "gateway_discovery_assignment_current_claim_count_locked() == 0u"
        in assignment
    )
    assert (
        "current_claim_count_locked() < gateway_discovery_assignment_state.expected_claim_count"
        not in re.sub(r"\s+", " ", assignment[reject_excess:publish])
    )

    route_control = (
        REPO / "firmware/app/src/app_mesh_report_route_control.inc"
    ).read_text(encoding="utf-8")
    route_start = route_control.index("static int mesh_listen_for_route_reply(")
    route_listener = route_control[route_start:]
    retained_click = route_listener.index("DBG_C5_CONTROL_CLICK_RETAIN")
    classify = route_listener.index(
        "app_mesh_c5_gateway_operation_outranks_unaccepted_click(",
        retained_click,
    )
    queue = route_listener.index("mesh_queue_from_frame_at_internal(", classify)
    retained = route_listener.index(
        "gateway_operation_retained = true", queue
    )
    handoff = route_listener.index(
        "mesh_handoff_anchor_click_claim(", retained
    )
    resume = route_listener.index(
        "DBG_C5_GATEWAY_OPERATION_QUEUED_AFTER_CLICK", handoff
    )
    assert retained_click < classify < queue < retained < handoff < resume
    assert "gateway-operation-before-click" not in route_listener


def main() -> None:
    oracle = sys.argv[1]
    exercise_assignment_policy(oracle)
    exercise_clean_slate_depth_order(oracle)
    exercise_gateway_control_priority(oracle)
    exercise_production_owner_source_invariants()
    print("product contract matrix passed")


if __name__ == "__main__":
    main()
