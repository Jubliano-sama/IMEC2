import subprocess
import sys
from itertools import combinations
from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from tools.gateway_gui.anchor_geometry import AnchorPairDistance
from tools.gateway_gui.diagnostic_models import (
    SurveyGeometryModel,
    classify_survey_topology,
)
from tools.gateway_gui.operation_policy import (
    AssignmentOperationPolicy,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    PairOperationPolicy,
    discovery_required_start_delay_ms,
)
from tools.gateway_gui.protocol import (
    TLV_OPERATION_POLICY,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
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


def exercise_topology_policy_vectors(oracle: str) -> None:
    vectors = (
        ("DDD", {1: 0, 2: 0, 3: 0}, {}, "Direct", "green", 1),
        ("F1DD", {1: 1, 2: 0, 3: 0}, {1: 2}, "F1", "green", 1),
        ("F1F1D", {1: 1, 2: 1, 3: 0}, {1: 3, 2: 3}, "F1F1D", "amber", 1),
        ("F2F1D", {1: 2, 2: 1, 3: 0}, {1: 2, 2: 3}, "F2F1D", "amber", 2),
    )
    for name, hops, parents, expected_topology, expected_level, deepest_hop in vectors:
        classification = classify_survey_topology(
            hops_by_anchor=hops, parent_by_anchor=parents
        )
        assert classification.topology == expected_topology, name
        assert classification.level == expected_level, name

        profile = OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(expected_anchor_count=3),
            discovery=DiscoveryOperationPolicy(
                start_delay_ms=discovery_required_start_delay_ms(deepest_hop),
                slot_count=3,
                deepest_hop=deepest_hop,
            ),
            pair=PairOperationPolicy(max_parallel_pairs=1),
        )
        command = build_here_i_am_command(
            host_id=0x484F5354,
            gateway_id=0x9999888877776666,
            session_id=0x12345678,
            seq=17,
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
            "discovery_start": profile.discovery.start_delay_ms,
            "discovery_slots": 3,
            "discovery_budget": profile.discovery.operation_budget_ms,
            "pair_reruns": profile.pair.max_reruns,
            "pair_parallel": 1,
            "ram_only": 0,
        }, name

    bench_profile = OperationPolicyProfile(
        assignment=AssignmentOperationPolicy(
            expected_anchor_count=3, ram_only_iteration=True
        ),
        pair=PairOperationPolicy(max_parallel_pairs=1),
    )
    bench_command = build_assign_discovery_slots_command(
        host_id=0x484F5354,
        gateway_id=0x9999888877776666,
        session_id=0x12345679,
        seq=18,
        command_budget_ms=bench_profile.assignment.operation_budget_ms,
        expected_anchor_count=3,
        operation_policy=bench_profile,
    )
    bench_packet = parse_cobs_packet(bench_command.frame)
    bench_firmware = _fields(
        _run(oracle, "--policy", _policy_tlvs(bench_packet.payload).hex())
    )
    assert bench_firmware["ram_only"] == 1
    assert bench_firmware["pair_parallel"] == 1

    try:
        PairOperationPolicy(max_parallel_pairs=2)
    except ValueError:
        pass
    else:
        raise AssertionError("host policy accepted concurrent survey pairs")

    invalid_values = bytearray().join(bench_profile.encoded_values())
    pair_family = bytes((1, 3, 0, bench_profile.pair.max_reruns, 1))
    pair_offset = invalid_values.find(pair_family)
    assert pair_offset >= 0
    invalid_values[pair_offset + 4] = 2
    invalid_tlvs = bytearray()
    offset = 0
    for value_len in (11, 19, 5):
        invalid_tlvs.extend((TLV_OPERATION_POLICY, value_len))
        invalid_tlvs.extend(invalid_values[offset:offset + value_len])
        offset += value_len
    rejected = subprocess.run(
        (oracle, "--policy", invalid_tlvs.hex()),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
    )
    assert rejected.returncode != 0, rejected.stdout


def exercise_clean_slate_depth_order(oracle: str) -> None:
    ordered = _run(oracle, "--order").split(",")
    parsed = [(int(item.split(":")[0]), int(item.split(":")[1])) for item in ordered]
    assert [hop for _anchor, hop in parsed] == [0, 0, 1, 2]
    assert len({anchor for anchor, _hop in parsed}) == 4


def exercise_partial_solver_gate() -> None:
    anchors = tuple(f"A{index}" for index in range(6))
    planned = {tuple(sorted(pair)) for pair in combinations(anchors, 2)}
    failed = tuple(sorted((anchors[0], anchors[1])))
    successful = planned - {failed}
    model = SurveyGeometryModel()
    model.expected_opportunities = len(planned)
    model.planned_opportunities = set(planned)
    model.observed_opportunities = set(planned)
    model.successful_opportunities = set(successful)
    model.failed_opportunities = {failed}
    model.failures = {failed}
    model.pairs = {
        pair: AnchorPairDistance(pair[0], pair[1], 1.0)
        for pair in successful
    }
    model.terminal_seen = True
    model.terminal_complete = True
    model.terminal_success_count = len(successful)
    model.terminal_failure_count = 1
    model._command_identity = (0x12345678, 17)

    ready, reason = model.solve_readiness()
    assert ready, reason
    assert "ready to solve" in reason


def exercise_gateway_control_priority(oracle: str) -> None:
    fields = _fields(_run(oracle, "--control"))
    assert fields == {
        "enumeration": 1,
        "survey": 1,
        "pair": 1,
        "abort": 1,
        "here_i_am": 1,
        "unrelated": 0,
    }


def exercise_strict_pair_custody(oracle: str) -> None:
    fields = _fields(_run(oracle, "--pair-custody"))
    assert fields["first"] == 0
    assert fields["replacement"] < 0
    assert fields["retained"] == 1


def exercise_production_owner_source_invariants() -> None:
    assignment = (
        REPO / "firmware/app/src/app_anchor_gateway_control.inc"
    ).read_text(encoding="utf-8")
    normalized_assignment = re.sub(r"\s+", " ", assignment)
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
        not in normalized_assignment
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
    wins = route_listener.index(
        "if (click_captured && gateway_operation_retained)", queue
    )
    handoff = route_listener.index("mesh_handoff_anchor_click_claim(", wins)
    assert retained_click < classify < queue < wins < handoff
    assert "mesh_handoff_anchor_click_claim(" not in route_listener[wins:handoff]


def main() -> None:
    oracle = sys.argv[1]
    exercise_topology_policy_vectors(oracle)
    exercise_clean_slate_depth_order(oracle)
    exercise_gateway_control_priority(oracle)
    exercise_strict_pair_custody(oracle)
    exercise_partial_solver_gate()
    exercise_production_owner_source_invariants()
    print("product contract matrix passed")


if __name__ == "__main__":
    main()
