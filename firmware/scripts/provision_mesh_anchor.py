#!/usr/bin/env python3
"""Send mesh provisioning commands and inspect raw gateway BLE events."""

from __future__ import annotations

import argparse
import asyncio
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
import pathlib
import re
import secrets
import sys

from bleak import BleakClient, BleakScanner


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.gateway_gui.protocol import (  # noqa: E402
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    DecodeError,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    GATEWAY_COMMAND_BUDGET_MIN_MS,
    GATEWAY_IDENTITY_UUID,
    MESH_NETWORK_MAX_HOPS,
    MSG_ANCHOR_HEARTBEAT,
    GatewayReceiveBuffer,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_SURVEY_DISCOVERY_REPORT,
    MSG_SURVEY_PAIR_RESULT,
    Packet,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
    SERVICE_UUID,
    SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
    SURVEY_GATEWAY_OPERATION_MAX_BUDGET_MS,
    SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    TLV_COMMAND_ID,
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_gateway_host_receipt,
    build_here_i_am_command,
    build_survey_abort_command,
    decode_gateway_identity,
    is_gateway_assignment_publisher_event,
    validate_gateway_local_command_result_packet,
    validate_survey_discovery_report,
)
from tools.gateway_gui.delivery_dedup import (  # noqa: E402
    GatewayPacketDeduplicator,
    PacketDisposition,
    is_host_delivery_packet,
)
from tools.gateway_gui.operation_policy import (  # noqa: E402
    DISCOVERY_DEFAULT_ROUND_COUNT,
    DISCOVERY_DEFAULT_SLOT_MS,
    ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
    AssignmentOperationPolicy,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    assignment_required_budget_ms,
    discovery_required_budget_ms,
    discovery_required_start_delay_ms,
)
from tools.gateway_gui.command_telemetry import (  # noqa: E402
    GatewayCommandEvent,
    decode_gateway_command_event,
)
from tools.gateway_gui.command_orchestration import (  # noqa: E402
    GATEWAY_COMMAND_COMPLETION_GUARD_S,
    GatewayAssignmentReplayBarrier,
    GatewayAssignmentReplayReceipt,
    gateway_command_requires_preflight,
)
from tools.gateway_gui.anchor_geometry import solve_anchor_layout  # noqa: E402
from tools.gateway_gui.diagnostic_models import SurveyGeometryModel  # noqa: E402
from tools.gateway_gui.protocol import COMMAND_STATUS_NAMES  # noqa: E402


GATEWAY_COMMAND_KIND_ANCHOR_SURVEY = 2
GATEWAY_COMMAND_KIND_ANCHOR_ENUMERATION = 1
GATEWAY_COMMAND_KIND_ROUTE_REFRESH = 3
GATEWAY_COMMAND_STAGE_FLOOD_ATTEMPT = 4
GATEWAY_COMMAND_STAGE_BACKOFF = 5
GATEWAY_COMMAND_STAGE_ANCHOR_REPORT = 6
GATEWAY_COMMAND_STAGE_ENUMERATION_COMPLETE = 7
GATEWAY_COMMAND_STAGE_SCHEDULE_READY = 8
GATEWAY_COMMAND_STAGE_PAIR_START = 9
GATEWAY_COMMAND_STAGE_PAIR_SUCCESS = 10
GATEWAY_COMMAND_STAGE_PAIR_FAILURE = 11
GATEWAY_COMMAND_STAGE_TERMINAL = 12
CMD_SURVEY_REACHABILITY = 0x0100
CMD_SURVEY_PREPARE_PAIR = 0x0101
CMD_SURVEY_START_PAIR = 0x0102
CMD_SURVEY_ABORT = 0x0103
CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104
CMD_FORCE_REDISCOVERY = 0x000C
DISCOVERY_SLOT_UNAVAILABLE = 0xFF
DISCOVERY_SLOT_COUNT = 50
ROUTE_REFRESH_MAX_LOCAL_ATTEMPTS = 9
SURVEY_TERMINAL_DRAIN_QUIET_DEFAULT_S = 5.0
SURVEY_TERMINAL_DRAIN_MAX_S = 135.0
BLE_RECONNECT_ATTEMPTS_DEFAULT = 6
BLE_RECONNECT_DELAY_DEFAULT_S = 0.5


def _assignment_operation_policy(
    expected_anchor_count: int,
    command_budget_ms: int | None,
    deepest_hop: int = 0,
    *,
    ram_only_iteration: bool = False,
) -> OperationPolicyProfile:
    """Mirror the desktop GUI's explicit assignment policy on the wire."""
    effective_budget_ms = (
        assignment_required_budget_ms(
            ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            expected_anchor_count,
            deepest_hop=deepest_hop,
        )
        if command_budget_ms is None
        else command_budget_ms
    )
    return OperationPolicyProfile(
        assignment=AssignmentOperationPolicy(
            expected_anchor_count=expected_anchor_count,
            operation_budget_ms=effective_budget_ms,
            response_spread_ms=ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            deepest_hop=deepest_hop,
            ram_only_iteration=ram_only_iteration,
        )
    )


def _survey_operation_policy(
    *,
    expected_anchor_count: int,
    discovery_slot_count: int,
    report_grace_ms: int,
    deepest_hop: int = 0,
) -> OperationPolicyProfile:
    """Bind headless survey arguments to the policy firmware validates."""
    start_delay_ms = discovery_required_start_delay_ms(deepest_hop)
    discovery_budget_ms = discovery_required_budget_ms(
        start_delay_ms,
        DISCOVERY_DEFAULT_SLOT_MS,
        discovery_slot_count,
        DISCOVERY_DEFAULT_ROUND_COUNT,
        report_grace_ms,
        deepest_hop,
    )
    return OperationPolicyProfile(
        assignment=AssignmentOperationPolicy(
            expected_anchor_count=expected_anchor_count,
            deepest_hop=deepest_hop,
        ),
        discovery=DiscoveryOperationPolicy(
            start_delay_ms=start_delay_ms,
            slot_ms=DISCOVERY_DEFAULT_SLOT_MS,
            slot_count=discovery_slot_count,
            round_count=DISCOVERY_DEFAULT_ROUND_COUNT,
            report_grace_ms=report_grace_ms,
            operation_budget_ms=discovery_budget_ms,
            deepest_hop=deepest_hop,
        ),
    )


def _is_ble_address(value: str) -> bool:
    return re.fullmatch(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}", value) is not None


def _parse_expected_anchor_hop(value: str) -> tuple[int, int]:
    try:
        node_text, hop_text = value.split("=", 1)
        node_id = int(node_text, 0)
        hop_count = int(hop_text, 0)
    except (ValueError, TypeError) as exc:
        raise argparse.ArgumentTypeError(
            "expected NODE_ID=HOPS"
        ) from exc
    if not 1 <= node_id <= 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError("node ID must be a nonzero uint64")
    if not 1 <= hop_count <= MESH_NETWORK_MAX_HOPS:
        raise argparse.ArgumentTypeError(
            f"hop count must be in 1..{MESH_NETWORK_MAX_HOPS}"
        )
    return node_id, hop_count


async def _resolve_gateway_target(name_or_address: str, timeout_s: float) -> object:
    """Resolve a friendly gateway name once and hand Bleak its device object."""
    if _is_ble_address(name_or_address):
        return name_or_address

    discovered = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
    entries = discovered.values() if isinstance(discovered, dict) else discovered
    named_without_service = False
    for entry in entries:
        if isinstance(entry, tuple) and len(entry) == 2:
            device, advertisement = entry
        else:
            device = entry
            advertisement = None
        address = str(getattr(device, "address", ""))
        name = (
            getattr(advertisement, "local_name", None)
            or getattr(device, "name", None)
            or ""
        )
        if name_or_address.casefold() not in (address.casefold(), name.casefold()):
            continue
        service_uuids = {
            str(uuid).lower()
            for uuid in (getattr(advertisement, "service_uuids", None) or [])
        }
        if SERVICE_UUID in service_uuids:
            return device
        named_without_service = True

    if named_without_service:
        raise RuntimeError(
            f"gateway {name_or_address!r} was found but did not advertise the IMEC service"
        )
    raise RuntimeError(f"gateway {name_or_address!r} was not found during BLE scan")


def _survey_custody_record_retained(packet: Packet) -> bool:
    """Retain the closed survey set even when it predates this host command."""
    if packet.msg_type == MSG_ANCHOR_HEARTBEAT:
        # Heartbeats are diagnostic topology records. The receive buffer has
        # already checked their stream framing/CRC, and retaining the exact
        # packet here lets this provisioning host release a stale BLE head.
        return True
    if packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT:
        validate_survey_discovery_report(packet)
        return True
    if packet.msg_type == MSG_SURVEY_PAIR_RESULT:
        return True
    if packet.msg_type != MSG_COMMAND_RESULT:
        return False
    command_ids = tuple(
        tlv
        for tlv in packet.tlvs
        if tlv.type_id == TLV_COMMAND_ID
    )
    if len(command_ids) != 1 or command_ids[0].decode_error is not None:
        return False
    return command_ids[0].decoded in (
        CMD_FORCE_REDISCOVERY,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        CMD_SURVEY_REACHABILITY,
        CMD_SURVEY_PREPARE_PAIR,
        CMD_SURVEY_START_PAIR,
        CMD_SURVEY_ABORT,
    )


def _failed_qualification_command_result(
    packet: Packet,
    qualification: object | None,
) -> str | None:
    """Return an exact active-command failure without waiting for telemetry."""
    if (
        packet.msg_type != MSG_COMMAND_RESULT
        or qualification is None
        or packet.session_id != getattr(qualification, "host_session_id", None)
        or packet.seq != getattr(qualification, "host_sequence", None)
    ):
        return None
    validate_gateway_local_command_result_packet(packet)
    command_id = int.from_bytes(packet.payload[2:4], "little")
    command_status = int.from_bytes(packet.payload[6:8], "little")
    if command_status == 0:
        return None
    return (
        "gateway command failed before lifecycle terminal "
        f"(command={command_id} status={command_status} "
        f"reason={packet.payload[10]})"
    )


def _same_event(left: GatewayCommandEvent, right: GatewayCommandEvent) -> bool:
    """Treat replay/snapshot flag additions as the same retained event."""
    replay_flags = 0x02 | 0x04
    return (
        left == right
        or (
            left.flags & ~replay_flags == right.flags & ~replay_flags
            and left.__class__(
                **{
                    **left.__dict__,
                    "flags": left.flags & ~replay_flags,
                }
            )
            == right.__class__(
                **{
                    **right.__dict__,
                    "flags": right.flags & ~replay_flags,
                }
            )
        )
    )


def _accept_event_sequence(
    event: GatewayCommandEvent,
    seen: dict[int, GatewayCommandEvent],
    errors: list[str],
) -> bool:
    prior = seen.get(event.event_sequence)
    if prior is None:
        seen[event.event_sequence] = event
        return True
    if not _same_event(prior, event):
        message = f"event sequence {event.event_sequence} changed across replay"
        if message not in errors:
            errors.append(message)
    return False


def _observe_lost_event_counter(
    event: GatewayCommandEvent,
    baseline: int | None,
    errors: list[str],
    label: str,
) -> int:
    """Require no cumulative observability loss during this qualification."""
    if baseline is None:
        return event.lost_event_count
    if event.lost_event_count != baseline:
        message = (
            f"{label} lost events counter changed during qualification "
            f"({baseline}->{event.lost_event_count})"
        )
        if message not in errors:
            errors.append(message)
    return baseline


def _qualification_timeout_s(
    requested_timeout_s: float,
    effective_command_budget_ms: int,
) -> float:
    """Keep the host alive through the firmware deadline plus delivery guard."""
    return max(
        requested_timeout_s,
        effective_command_budget_ms / 1000.0
        + GATEWAY_COMMAND_COMPLETION_GUARD_S,
    )


@dataclass
class RouteRefreshQualification:
    host_session_id: int
    host_sequence: int
    correlation_id: int
    flood_attempts: set[int] = field(default_factory=set)
    retries: int = 0
    terminal_event: GatewayCommandEvent | None = None
    seen_events: dict[int, GatewayCommandEvent] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    lost_event_baseline: int | None = None

    def _matches(self, event: GatewayCommandEvent) -> bool:
        return (
            event.command_kind == GATEWAY_COMMAND_KIND_ROUTE_REFRESH
            and event.correlation_id == self.correlation_id
            and event.host_session_id == self.host_session_id
            and event.host_sequence == self.host_sequence
        )

    def _error(self, message: str) -> None:
        if message not in self.errors:
            self.errors.append(message)

    def observe(self, event: GatewayCommandEvent) -> bool:
        if not self._matches(event):
            return False
        if not _accept_event_sequence(event, self.seen_events, self.errors):
            return self.terminal_event is not None
        self.lost_event_baseline = _observe_lost_event_counter(
            event,
            self.lost_event_baseline,
            self.errors,
            "route-refresh",
        )
        if event.command_id != CMD_FORCE_REDISCOVERY:
            self._error("route-refresh event has the wrong command")
        if (
            self.terminal_event is not None
            and event.stage != GATEWAY_COMMAND_STAGE_TERMINAL
        ):
            self._error("route-refresh progress arrived after its terminal event")
            return True
        if event.stage == GATEWAY_COMMAND_STAGE_FLOOD_ATTEMPT:
            if (
                event.attempt < 1
                or event.attempt > ROUTE_REFRESH_MAX_LOCAL_ATTEMPTS
                or event.command_status != 0
                or event.reason != 0
            ):
                self._error("route-refresh flood-attempt event is invalid")
            else:
                self.flood_attempts.add(event.attempt)
        elif event.stage == GATEWAY_COMMAND_STAGE_BACKOFF:
            if event.attempt < 1 or event.attempt >= ROUTE_REFRESH_MAX_LOCAL_ATTEMPTS:
                self._error("route-refresh backoff exceeds the local attempt budget")
            self.retries += 1
        elif event.stage == GATEWAY_COMMAND_STAGE_TERMINAL:
            if not event.terminal:
                self._error("route-refresh terminal event is missing its terminal flag")
            if not self.flood_attempts:
                self._error("route-refresh terminal preceded every completed local flood")
            self.terminal_event = event
            return True
        return False

    def validate(self) -> None:
        terminal = self.terminal_event
        if not self.flood_attempts:
            self._error("no completed local route-refresh flood attempt was observed")
        if terminal is None:
            self._error("matching route-refresh terminal event was not received")
        elif (
            terminal.command_status != 0
            or terminal.reason != 0
        ):
            self._error(
                "route-refresh terminal is not a lossless local flood success "
                f"(status={terminal.command_status} reason={terminal.reason} "
                f"lost={terminal.lost_event_count})"
            )
        if self.errors:
            raise RuntimeError(
                "Here-I-Am local flood qualification failed: " + "; ".join(self.errors)
            )


@dataclass
class AssignmentQualification:
    host_session_id: int
    host_sequence: int
    correlation_id: int
    expected_anchors: int
    require_hop_evidence: bool = False
    expected_direct_anchors: int | None = None
    expected_multihop_anchors: int | None = None
    expected_anchor_hops: dict[int, int] = field(default_factory=dict)
    anchors: set[int] = field(default_factory=set)
    assigned_slots: dict[int, int] = field(default_factory=dict)
    confirmed_slots: dict[int, int] = field(default_factory=dict)
    hop_paths: dict[int, tuple[int, int]] = field(default_factory=dict)
    hop_evidence_ranks: dict[int, int] = field(default_factory=dict)
    flood_attempts: set[int] = field(default_factory=set)
    table_attempts: set[int] = field(default_factory=set)
    collection_complete: bool = False
    terminal_event: GatewayCommandEvent | None = None
    retries: int = 0
    seen_events: dict[int, GatewayCommandEvent] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    lost_event_baseline: int | None = None

    def _matches(self, event: GatewayCommandEvent) -> bool:
        return (
            event.command_kind == GATEWAY_COMMAND_KIND_ANCHOR_ENUMERATION
            and event.correlation_id == self.correlation_id
            and event.host_session_id == self.host_session_id
            and event.host_sequence == self.host_sequence
        )

    def _error(self, message: str) -> None:
        if message not in self.errors:
            self.errors.append(message)

    def observe(self, event: GatewayCommandEvent) -> bool:
        if not self._matches(event):
            return False
        if not _accept_event_sequence(event, self.seen_events, self.errors):
            return self.terminal_event is not None
        self.lost_event_baseline = _observe_lost_event_counter(
            event,
            self.lost_event_baseline,
            self.errors,
            "assignment",
        )
        if event.command_id != CMD_ASSIGN_DISCOVERY_SLOTS:
            self._error("assignment event has the wrong command")
        if (
            self.terminal_event is not None
            and event.stage != GATEWAY_COMMAND_STAGE_TERMINAL
        ):
            self._error("assignment progress arrived after its terminal event")
            return True
        if event.stage == GATEWAY_COMMAND_STAGE_FLOOD_ATTEMPT:
            if event.attempt < 1 or event.command_status != 0 or event.reason != 0:
                self._error("assignment flood-attempt event is invalid")
            else:
                self.flood_attempts.add(event.attempt)
        elif event.stage == GATEWAY_COMMAND_STAGE_BACKOFF:
            self.retries += 1
        elif event.stage == GATEWAY_COMMAND_STAGE_ANCHOR_REPORT:
            if event.anchor_id == 0 or event.command_status != 0 or event.reason != 0:
                self._error("assignment anchor event is invalid")
            else:
                self.anchors.add(event.anchor_id)
                if event.discovery_slot != DISCOVERY_SLOT_UNAVAILABLE:
                    prior = self.assigned_slots.get(event.anchor_id)
                    if prior is not None and prior != event.discovery_slot:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} changed assigned slot"
                        )
                    self.assigned_slots[event.anchor_id] = event.discovery_slot
                if event.hop_count == 0:
                    if event.previous_hop_id != 0:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} has previous hop "
                            "without hop count"
                        )
                else:
                    path = (event.hop_count, event.previous_hop_id)
                    prior_path = self.hop_paths.get(event.anchor_id)
                    mapping_depth_only = (
                        event.discovery_slot != DISCOVERY_SLOT_UNAVAILABLE
                        and event.previous_hop_id == 0
                    )
                    evidence_rank = 2 if mapping_depth_only else 1
                    prior_rank = self.hop_evidence_ranks.get(event.anchor_id, 0)
                    if not mapping_depth_only and event.previous_hop_id == 0:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} has hop count "
                            "without previous hop"
                        )
                    elif evidence_rank > prior_rank:
                        self.hop_paths[event.anchor_id] = (
                            event.hop_count,
                            prior_path[1]
                            if mapping_depth_only and prior_path is not None
                            else event.previous_hop_id,
                        )
                        self.hop_evidence_ranks[event.anchor_id] = evidence_rank
                    elif evidence_rank == prior_rank and prior_path is not None and (
                        prior_path[0] != event.hop_count
                        or (
                            prior_path[1] != 0
                            and prior_path[1] != event.previous_hop_id
                        )
                    ):
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} changed hop evidence"
                        )
        elif event.stage == GATEWAY_COMMAND_STAGE_ENUMERATION_COMPLETE:
            if event.command_status != 0 or event.reason != 0:
                self._error("assignment collection-complete event is not successful")
            if event.progress_count != self.expected_anchors:
                self._error("assignment collection-complete count is not exact")
            self.collection_complete = True
        elif event.stage == GATEWAY_COMMAND_STAGE_SCHEDULE_READY:
            if event.anchor_id != 0:
                if (
                    event.command_status != 0
                    or event.reason != 0
                    or event.hop_count == 0
                    or event.hop_count > MESH_NETWORK_MAX_HOPS
                    or event.discovery_slot >= DISCOVERY_SLOT_COUNT
                    or event.progress_count < 1
                    or event.progress_count > self.expected_anchors
                    or event.total_count != self.expected_anchors
                ):
                    self._error("assignment table-confirmation event is invalid")
                else:
                    prior = self.confirmed_slots.get(event.anchor_id)
                    if prior is not None and prior != event.discovery_slot:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} changed confirmed slot"
                        )
                    self.confirmed_slots[event.anchor_id] = event.discovery_slot
                    # The TABLE confirmation is the newest operation-bound
                    # route-depth proof.  CLAIM's depth is the anchor's route
                    # estimate while previous_hop_id describes the packet's
                    # physical ingress, so those two fields need not describe
                    # the same path.  The reliable mapping batch was prepared
                    # before TABLE responses and can legitimately carry the
                    # earlier depth; do not let it overwrite this proof when
                    # host delivery later drains that batch.
                    prior_path = self.hop_paths.get(event.anchor_id)
                    self.hop_paths[event.anchor_id] = (
                        event.hop_count,
                        0 if prior_path is None else prior_path[1],
                    )
                    self.hop_evidence_ranks[event.anchor_id] = 3
            elif (
                event.command_status != 0
                or event.reason != 0
                or event.progress_count != self.expected_anchors
                or event.total_count != self.expected_anchors
                or event.attempt < 1
            ):
                self._error("assignment table-publication event is not exact")
            else:
                self.table_attempts.add(event.attempt)
        elif event.stage == GATEWAY_COMMAND_STAGE_TERMINAL:
            if not event.terminal:
                self._error("assignment terminal event is missing its terminal flag")
            if (
                len(self.anchors) != self.expected_anchors
                or len(self.assigned_slots) != self.expected_anchors
                or not self.collection_complete
                or not self.table_attempts
            ):
                self._error("assignment terminal preceded required lifecycle progress")
            self.terminal_event = event
            return True
        return False

    def validate(self) -> None:
        terminal = self.terminal_event
        if len(self.anchors) != self.expected_anchors:
            self._error(
                f"expected {self.expected_anchors} unique anchor claims, got {len(self.anchors)}"
            )
        if len(self.assigned_slots) != self.expected_anchors:
            self._error(
                f"expected {self.expected_anchors} published slot mappings, "
                f"got {len(self.assigned_slots)}"
            )
        assigned_slot_values = tuple(self.assigned_slots.values())
        if (
            len(set(assigned_slot_values)) != len(assigned_slot_values)
            or any(
                slot < 0 or slot >= DISCOVERY_SLOT_COUNT
                for slot in assigned_slot_values
            )
        ):
            self._error("published discovery slots are not unique and valid")
        for anchor_id, confirmed_slot in self.confirmed_slots.items():
            assigned_slot = self.assigned_slots.get(anchor_id)
            if assigned_slot is None:
                self._error(
                    f"confirmed anchor 0x{anchor_id:016x} is absent from publication"
                )
            elif assigned_slot != confirmed_slot:
                self._error(
                    f"anchor 0x{anchor_id:016x} confirmed slot {confirmed_slot}, "
                    f"published slot {assigned_slot}"
                )
        if not self.collection_complete:
            self._error("assignment collection-complete event was not received")
        if not self.table_attempts:
            self._error("successful assignment table publication was not observed")
        if self.require_hop_evidence and len(self.hop_paths) != self.expected_anchors:
            self._error(
                f"expected hop-path evidence for {self.expected_anchors} anchors, "
                f"got {len(self.hop_paths)}"
            )
        for anchor_id, (hop_count, previous_hop_id) in self.hop_paths.items():
            if (
                hop_count > 1
                and previous_hop_id != 0
                and previous_hop_id not in self.anchors
            ):
                self._error(
                    f"anchor 0x{anchor_id:016x} has multihop predecessor "
                    f"0x{previous_hop_id:016x} outside the qualified roster"
                )
        for anchor_id, expected_hop_count in self.expected_anchor_hops.items():
            actual = self.hop_paths.get(anchor_id)
            if actual is None:
                self._error(
                    f"expected anchor 0x{anchor_id:016x} hop evidence is missing"
                )
            elif actual[0] != expected_hop_count:
                self._error(
                    f"anchor 0x{anchor_id:016x} expected hop count "
                    f"{expected_hop_count}, got {actual[0]}"
                )
        direct_count = sum(1 for hop, _previous in self.hop_paths.values() if hop == 1)
        multihop_count = sum(1 for hop, _previous in self.hop_paths.values() if hop > 1)
        if (
            self.expected_direct_anchors is not None
            and direct_count != self.expected_direct_anchors
        ):
            self._error(
                f"expected {self.expected_direct_anchors} direct anchors, got {direct_count}"
            )
        if (
            self.expected_multihop_anchors is not None
            and multihop_count != self.expected_multihop_anchors
        ):
            self._error(
                f"expected {self.expected_multihop_anchors} multihop anchors, "
                f"got {multihop_count}"
            )
        if terminal is None:
            self._error("matching assignment terminal event was not received")
        elif (
            terminal.command_status != 0
            or terminal.reason != 0
            or terminal.progress_count != self.expected_anchors
            or terminal.total_count != self.expected_anchors
            or terminal.success_count != self.expected_anchors
            or terminal.failure_count != 0
        ):
            self._error(
                "terminal assignment counters do not prove every table ACK completed "
                f"(status={terminal.command_status} reason={terminal.reason} "
                f"total={terminal.total_count} success={terminal.success_count} "
                f"failure={terminal.failure_count} lost={terminal.lost_event_count})"
            )
        if self.errors:
            raise RuntimeError("assignment qualification failed: " + "; ".join(self.errors))

    @property
    def direct_count(self) -> int:
        return sum(1 for hop, _previous in self.hop_paths.values() if hop == 1)

    @property
    def multihop_count(self) -> int:
        return sum(1 for hop, _previous in self.hop_paths.values() if hop > 1)


def _pair(initiator_id: int, responder_id: int) -> tuple[int, int] | None:
    if initiator_id == 0 or responder_id == 0 or initiator_id == responder_id:
        return None
    return tuple(sorted((initiator_id, responder_id)))


@dataclass
class SurveyQualification:
    host_session_id: int
    host_sequence: int
    correlation_id: int
    survey_id: int
    expected_anchors: int
    expected_pairs: int
    anchors: set[int] = field(default_factory=set)
    pair_starts: set[tuple[int, int]] = field(default_factory=set)
    pair_successes: set[tuple[int, int]] = field(default_factory=set)
    schedule_total: int | None = None
    terminal_event: GatewayCommandEvent | None = None
    retries: int = 0
    errors: list[str] = field(default_factory=list)
    seen_events: dict[int, GatewayCommandEvent] = field(default_factory=dict)
    lost_event_baseline: int | None = None
    geometry_model: SurveyGeometryModel = field(init=False)
    geometry_rmse_m: float | None = field(default=None, init=False)

    def __post_init__(self) -> None:
        self.geometry_model = SurveyGeometryModel()
        self.geometry_model.begin_survey(
            self.survey_id,
            host_session_id=self.host_session_id,
            host_sequence=self.host_sequence,
        )

    def _matches(self, event: GatewayCommandEvent) -> bool:
        return (
            event.command_kind == GATEWAY_COMMAND_KIND_ANCHOR_SURVEY
            and event.correlation_id == self.correlation_id
            and event.host_session_id == self.host_session_id
            and event.host_sequence == self.host_sequence
        )

    def _error(self, message: str) -> None:
        if message not in self.errors:
            self.errors.append(message)

    def observe(self, event: GatewayCommandEvent) -> bool:
        """Record one correlated event and return true at terminal."""
        if not self._matches(event):
            return False
        if not _accept_event_sequence(event, self.seen_events, self.errors):
            return self.terminal_event is not None
        self.geometry_model.observe_command_event(event)
        self.lost_event_baseline = _observe_lost_event_counter(
            event,
            self.lost_event_baseline,
            self.errors,
            "survey",
        )
        if event.stage == 5:
            self.retries += 1
        elif event.stage == GATEWAY_COMMAND_STAGE_ANCHOR_REPORT:
            if event.command_id != CMD_SURVEY_REACHABILITY or event.anchor_id == 0:
                self._error("anchor-report event has invalid command or anchor identity")
            elif event.command_status != 0 or event.reason != 0:
                self._error("anchor-report event is not successful")
            else:
                self.anchors.add(event.anchor_id)
        elif event.stage == GATEWAY_COMMAND_STAGE_SCHEDULE_READY:
            if event.command_id != CMD_SURVEY_REACHABILITY:
                self._error("schedule-ready event has the wrong command")
            if event.command_status != 0 or event.reason != 0:
                self._error("schedule-ready event is not successful")
            if self.schedule_total is not None and self.schedule_total != event.total_count:
                self._error("schedule-ready pair count changed")
            self.schedule_total = event.total_count
        elif event.stage in (
            GATEWAY_COMMAND_STAGE_PAIR_START,
            GATEWAY_COMMAND_STAGE_PAIR_SUCCESS,
            GATEWAY_COMMAND_STAGE_PAIR_FAILURE,
        ):
            pair = _pair(event.pair_initiator_id, event.pair_responder_id)
            if event.command_id != CMD_SURVEY_START_PAIR or pair is None:
                self._error(f"pair event stage {event.stage} has invalid identity")
            elif event.stage == GATEWAY_COMMAND_STAGE_PAIR_START:
                if event.command_status != 0 or event.reason != 0:
                    self._error("pair-start event is not successful")
                self.pair_starts.add(pair)
            elif event.stage == GATEWAY_COMMAND_STAGE_PAIR_SUCCESS:
                if pair not in self.pair_starts:
                    self._error("pair succeeded before its pair-start event")
                if event.command_status != 0 or event.reason != 0:
                    self._error("pair-success event is not successful")
                self.pair_successes.add(pair)
            else:
                self._error("survey emitted a pair-failure event")
        elif event.stage == GATEWAY_COMMAND_STAGE_TERMINAL:
            if event.command_id != CMD_SURVEY_REACHABILITY or not event.terminal:
                self._error("survey terminal event has invalid command or flags")
            self.terminal_event = event
            return True
        return False

    def observe_packet(self, packet: Packet) -> bool:
        return self.geometry_model.observe_pair_packet(packet) is not None

    def validate(self) -> None:
        terminal = self.terminal_event
        if len(self.anchors) != self.expected_anchors:
            self._error(
                f"expected {self.expected_anchors} unique anchors, got {len(self.anchors)}"
            )
        if self.schedule_total != self.expected_pairs:
            self._error(
                f"expected schedule of {self.expected_pairs} pairs, got {self.schedule_total}"
            )
        if len(self.pair_starts) != self.expected_pairs:
            self._error(
                f"expected {self.expected_pairs} pair starts, got {len(self.pair_starts)}"
            )
        if self.pair_successes != self.pair_starts:
            self._error("pair-success set does not exactly match the pair-start set")
        if len(self.geometry_model.pairs) != self.expected_pairs:
            self._error(
                f"expected {self.expected_pairs} usable GUI pair distances, got "
                f"{len(self.geometry_model.pairs)}"
            )
        expected_geometry_pairs = {
            tuple(sorted((f"0x{left:016x}", f"0x{right:016x}")))
            for left, right in self.pair_successes
        }
        if set(self.geometry_model.pairs) != expected_geometry_pairs:
            self._error(
                "usable GUI distance identities do not exactly match the "
                "gateway pair-success identities"
            )
        if terminal is None:
            self._error("matching survey terminal event was not received")
        elif (
            terminal.command_status != 0
            or terminal.reason != 0
            or terminal.total_count != self.expected_pairs
            or terminal.success_count != self.expected_pairs
            or terminal.failure_count != 0
        ):
            self._error(
                "terminal survey counters are not an exact lossless success "
                f"(status={terminal.command_status} reason={terminal.reason} "
                f"total={terminal.total_count} success={terminal.success_count} "
                f"failure={terminal.failure_count} lost={terminal.lost_event_count})"
            )
        ready, reason = self.geometry_model.solve_readiness()
        if not ready:
            self._error(f"GUI geometry is not solvable: {reason}")
        else:
            try:
                result = solve_anchor_layout(self.geometry_model.pairs.values())
            except (RuntimeError, ValueError) as exc:
                self._error(f"GUI geometry solver rejected live pair data: {exc}")
            else:
                self.geometry_rmse_m = result.rmse_m
                if len(result.positions_m) != self.expected_anchors:
                    self._error(
                        f"expected solved positions for {self.expected_anchors} anchors, "
                        f"got {len(result.positions_m)}"
                    )
        if self.errors:
            raise RuntimeError("survey qualification failed: " + "; ".join(self.errors))


_identity_state = secrets.randbits(32)


def _new_identity() -> int:
    """Allocate a fresh process-local command identity from a random start."""
    global _identity_state

    while True:
        _identity_state = (_identity_state + 1) & 0xFFFFFFFF
        if _identity_state != 0 and _identity_state & 0xFFFF:
            return _identity_state


def _next_identity(previous: int = 0) -> int:
    identity = _new_identity()
    while identity == previous:
        identity = _new_identity()
    return identity


Qualification = SurveyQualification | AssignmentQualification | RouteRefreshQualification


@asynccontextmanager
async def _ble_client_session(*args: object, **kwargs: object):
    """Ignore only BlueZ teardown EOF after the whole session completed.

    ``dbus-fast`` can observe EOF while Bleak's context manager disconnects a
    link whose peer has already closed. An EOF raised by connect, transfer, or
    the session body remains fatal; only the context-manager exit after a
    successful body is treated as completed transport teardown.
    """
    body_completed = False
    try:
        async with BleakClient(*args, **kwargs) as client:
            yield client
            body_completed = True
    except EOFError:
        if not body_completed:
            raise
        print("BLE_DISCONNECT_COMPLETE peer already closed D-Bus transport", flush=True)


async def run(args: argparse.Namespace) -> Qualification | None:
    decoder = GatewayReceiveBuffer()
    delivery_dedup = GatewayPacketDeduplicator()
    write_lock = asyncio.Lock()
    reconnect_lock = asyncio.Lock()
    receipt_tasks: set[asyncio.Task[None]] = set()
    receipt_in_flight: set[object] = set()
    receipt_outstanding: set[object] = set()
    receipt_state_changed = asyncio.Event()
    survey_custody_activity = asyncio.Event()
    assignment_replay_barrier = GatewayAssignmentReplayBarrier()
    assignment_replay_changed = asyncio.Event()
    command_budget_ms = getattr(args, "command_budget_ms", None)
    assignment_command_budget_ms = command_budget_ms
    if (
        assignment_command_budget_ms is None
        and args.command in ("assign-slots", "qualify-reachability")
    ):
        assignment_command_budget_ms = assignment_required_budget_ms(
            ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            args.expected_anchors,
            deepest_hop=getattr(args, "deepest_hop", 0),
        )
    received = 0
    decode_errors: list[str] = []
    disconnect_errors: list[str] = []
    qualification: Qualification | None = None
    qualified_assignment_repeats = 0
    qualification_done = asyncio.Event()
    transport_failed = asyncio.Event()
    link_disconnected = asyncio.Event()
    connection_generation = 0
    disconnect_generation = 0
    command_write_active = False
    session_complete = False
    notifications_enabled = False
    defer_notifications = args.notification_hold_s > 0.0
    gateway_id = 0
    characteristic: object | None = None
    chunk_size = 20

    async def send_gateway_frame(
        client: BleakClient,
        characteristic: object,
        frame: bytes,
        *,
        chunk_size: int,
        expected_generation: int,
    ) -> None:
        """Keep every serial frame contiguous across asynchronous BLE writers."""
        async with write_lock:
            for offset in range(0, len(frame), chunk_size):
                if (
                    expected_generation != connection_generation
                    or link_disconnected.is_set()
                    or not bool(getattr(client, "is_connected", True))
                ):
                    raise ConnectionError(
                        "gateway connection changed while writing BLE frame"
                    )
                await client.write_gatt_char(
                    characteristic,
                    frame[offset : offset + chunk_size],
                    # These frames carry command or receipt custody.  A
                    # write command only proves that BlueZ queued the bytes;
                    # an ATT write request proves the gateway admitted this
                    # chunk and reports its bounded RX-queue backpressure.
                    response=True,
                )

    def schedule_host_receipt(
        packet: Packet,
        *,
        accepted: bool,
        assignment_replay: GatewayAssignmentReplayReceipt | None,
        client: BleakClient,
        characteristic: object,
        gateway_id: int,
        chunk_size: int,
        connection_generation: int,
    ) -> bool:
        """Release firmware custody only after this tool accepted an exact record."""
        if (
            not accepted
            or getattr(packet, "transport", None) != "gateway-stream-v1"
            or not is_host_delivery_packet(packet)
        ):
            return False
        try:
            receipt = build_gateway_host_receipt(
                packet,
                host_id=args.host_id,
                gateway_id=gateway_id,
            )
        except (DecodeError, TypeError, ValueError) as exc:
            decode_errors.append(f"host receipt construction failed: {exc}")
            qualification_done.set()
            transport_failed.set()
            return False
        if receipt.identity in receipt_in_flight:
            return True
        receipt_in_flight.add(receipt.identity)
        receipt_outstanding.add(receipt.identity)

        async def write_receipt() -> None:
            try:
                await send_gateway_frame(
                    client,
                    characteristic,
                    receipt.frame,
                    chunk_size=chunk_size,
                    expected_generation=connection_generation,
                )
                print(
                    "BLE_HOST_RECEIPT "
                    f"type=0x{receipt.identity.original_msg_type:02x} "
                    f"src=0x{receipt.identity.src_id:016x} "
                    f"session={receipt.identity.session_id} "
                    f"seq={receipt.identity.seq}",
                    flush=True,
                )
                if assignment_replay is not None:
                    assignment_replay_barrier.receipt_written(
                        assignment_replay
                    )
                receipt_outstanding.discard(receipt.identity)
            except Exception as exc:
                # A receipt is idempotent and remains in gateway custody until
                # accepted. Reconnect and wait for the gateway's exact replay;
                # never manufacture or resend the active command to recover it.
                print(
                    "BLE_HOST_RECEIPT_RETRY "
                    f"type=0x{receipt.identity.original_msg_type:02x} "
                    f"src=0x{receipt.identity.src_id:016x} "
                    f"session={receipt.identity.session_id} "
                    f"seq={receipt.identity.seq} "
                    f"error={type(exc).__name__}: {exc}",
                    flush=True,
                )
                link_disconnected.set()
                survey_custody_activity.set()
            finally:
                receipt_in_flight.discard(receipt.identity)
                receipt_state_changed.set()
                assignment_replay_changed.set()

        task = asyncio.create_task(write_receipt())
        receipt_tasks.add(task)
        task.add_done_callback(receipt_tasks.discard)
        return True

    def on_notify(_sender: object, data: bytearray) -> None:
        nonlocal received, qualification
        raw = bytes(data)
        print(f"BLE_CHUNK len={len(raw)} hex={raw.hex()}", flush=True)
        result = decoder.feed(raw)
        for error in result.errors:
            print(f"BLE_DECODE_ERROR {error}", flush=True)
            decode_errors.append(str(error))
            qualification_done.set()
            transport_failed.set()
        for packet in result.packets:
            accepted_for_receipt = False
            assignment_replay = None
            command_event = None
            survey_custody_record = False
            received += 1
            print(
                f"BLE_PACKET type=0x{packet.msg_type:02x} src=0x{packet.src_id:016x} "
                f"dst=0x{packet.dst_id:016x} session={packet.session_id} "
                f"seq={packet.seq} payload={packet.payload.hex()}",
                flush=True,
            )
            if isinstance(qualification, SurveyQualification):
                qualification.observe_packet(packet)
            try:
                survey_custody_record = _survey_custody_record_retained(packet)
            except DecodeError as exc:
                message = str(exc)
                print(f"BLE_DECODE_ERROR {message}", flush=True)
                decode_errors.append(message)
                qualification_done.set()
                transport_failed.set()
                continue
            if packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
                try:
                    event = decode_gateway_command_event(
                        packet.payload, valid_statuses=set(COMMAND_STATUS_NAMES)
                    )
                except ValueError as exc:
                    print(f"BLE_DECODE_ERROR {exc}", flush=True)
                    decode_errors.append(str(exc))
                    qualification_done.set()
                    transport_failed.set()
                    continue
                accepted_for_receipt = (
                    getattr(packet, "flags", 0) != 0
                    and is_host_delivery_packet(packet)
                )
                print(f"GATEWAY_COMMAND_EVENT {event}", flush=True)
                command_event = event
                if qualification is not None and qualification.observe(event):
                    qualification_done.set()
            elif packet.msg_type == MSG_COMMAND_RESULT:
                try:
                    message = _failed_qualification_command_result(
                        packet, qualification
                    )
                except DecodeError as exc:
                    message = str(exc)
                    print(f"BLE_DECODE_ERROR {message}", flush=True)
                    decode_errors.append(message)
                    qualification_done.set()
                    transport_failed.set()
                    continue
                if message is not None and qualification is not None:
                    if message not in qualification.errors:
                        qualification.errors.append(message)
                    qualification_done.set()
            if isinstance(packet, Packet):
                delivery = delivery_dedup.observe(packet, commit=False)
                if delivery.disposition in (
                    PacketDisposition.NEW,
                    PacketDisposition.DUPLICATE,
                ) and delivery.cached:
                    accepted_for_receipt = (
                        delivery.disposition is PacketDisposition.DUPLICATE
                        or delivery_dedup.commit(packet, delivery)
                    )
                elif delivery.disposition is PacketDisposition.CONFLICT:
                    accepted_for_receipt = False
                    print(
                        "BLE_HOST_CONFLICT "
                        f"type=0x{packet.msg_type:02x} "
                        f"src=0x{packet.src_id:016x} "
                        f"session={packet.session_id} seq={packet.seq}; "
                        "no receipt sent",
                        flush=True,
                    )
            if (
                accepted_for_receipt
                and command_event is not None
                and is_gateway_assignment_publisher_event(command_event)
            ):
                assignment_replay = assignment_replay_barrier.observe(
                    command_event
                )
            if schedule_host_receipt(
                packet,
                accepted=accepted_for_receipt,
                assignment_replay=assignment_replay,
                client=client,
                characteristic=characteristic,
                gateway_id=gateway_id,
                chunk_size=chunk_size,
                connection_generation=connection_generation,
            ):
                if survey_custody_record:
                    survey_custody_activity.set()

    def on_disconnect(_client: object) -> None:
        nonlocal disconnect_generation, notifications_enabled

        if session_complete:
            return
        disconnect_generation += 1
        notifications_enabled = False
        link_disconnected.set()
        survey_custody_activity.set()
        assignment_replay_changed.set()
        receipt_state_changed.set()
        if command_write_active:
            disconnect_errors.append(
                "gateway disconnected while writing the command; delivery is "
                "ambiguous and the command will not be resent"
            )
            qualification_done.set()
            transport_failed.set()

    def notification_callback(generation: int):
        """Ignore callbacks left behind by a previous BLE connection."""

        def callback(sender: object, data: bytearray) -> None:
            if generation != connection_generation:
                return
            on_notify(sender, data)

        return callback

    def raise_transport_errors(label: str) -> None:
        if decode_errors:
            raise RuntimeError(
                f"{label} failed: BLE decode errors: " + "; ".join(decode_errors)
            )
        if disconnect_errors:
            raise RuntimeError(f"{label} failed: " + "; ".join(disconnect_errors))

    async def reconnect_before(deadline: float, label: str) -> bool:
        """Restore one dropped link without changing the active command identity."""
        nonlocal characteristic, chunk_size, connection_generation, decoder
        nonlocal defer_notifications, notifications_enabled

        raise_transport_errors(label)
        if (
            not link_disconnected.is_set()
            and bool(getattr(client, "is_connected", True))
        ):
            return True
        async with reconnect_lock:
            raise_transport_errors(label)
            if (
                not link_disconnected.is_set()
                and bool(getattr(client, "is_connected", True))
            ):
                return True
            attempts = int(
                getattr(
                    args,
                    "reconnect_attempts",
                    BLE_RECONNECT_ATTEMPTS_DEFAULT,
                )
            )
            delay_s = float(
                getattr(
                    args,
                    "reconnect_delay_s",
                    BLE_RECONNECT_DELAY_DEFAULT_S,
                )
            )
            if attempts <= 0:
                disconnect_errors.append(
                    "gateway disconnected and BLE reconnect attempts are disabled"
                )
                transport_failed.set()
                raise_transport_errors(label)

            last_error = "gateway remained disconnected"
            for attempt in range(1, attempts + 1):
                remaining_s = deadline - asyncio.get_running_loop().time()
                if remaining_s <= 0.0:
                    return False
                if delay_s > 0.0:
                    if delay_s >= remaining_s:
                        try:
                            await asyncio.wait_for(
                                asyncio.sleep(delay_s),
                                timeout=remaining_s,
                            )
                        except asyncio.TimeoutError:
                            return False
                    else:
                        await asyncio.sleep(delay_s)

                async def reconnect_once() -> None:
                    nonlocal characteristic, chunk_size
                    nonlocal connection_generation, decoder
                    nonlocal defer_notifications, notifications_enabled

                    if bool(getattr(client, "is_connected", False)):
                        try:
                            await client.disconnect()
                        except Exception as exc:
                            # BlueZ commonly reports ATT 0x0e or D-Bus EOF
                            # while tearing down the same already-failed link.
                            # That stale-link cleanup is not a reconnect
                            # attempt; continue with a fresh connect and let
                            # identity plus notification restoration prove it.
                            print(
                                "BLE_RECONNECT_CLEANUP "
                                f"error={type(exc).__name__}: {exc}",
                                flush=True,
                            )
                    # Bleak can expose ``is_connected == False`` one loop turn
                    # before delivering the old link's disconnect callback.
                    # Drain that callback before snapshotting the generation so
                    # it cannot be mistaken for a failure of the new link.
                    await asyncio.sleep(0)
                    attempt_disconnect_generation = disconnect_generation
                    await client.connect()
                    candidate_gateway_id = decode_gateway_identity(
                        bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID))
                    )
                    if candidate_gateway_id != gateway_id:
                        raise RuntimeError(
                            "reconnected gateway identity mismatch: expected "
                            f"0x{gateway_id:016x}, got "
                            f"0x{candidate_gateway_id:016x}"
                        )
                    next_characteristic = client.services.get_characteristic(
                        PACKET_RX_UUID
                    )
                    if next_characteristic is None:
                        raise RuntimeError(
                            "gateway packet RX characteristic is unavailable after reconnect"
                        )
                    characteristic = next_characteristic
                    chunk_size = max(
                        1,
                        min(
                            int(
                                characteristic.max_write_without_response_size
                                or 20
                            ),
                            244,
                        ),
                    )
                    connection_generation += 1
                    decoder = GatewayReceiveBuffer()
                    await client.start_notify(
                        PACKET_TX_UUID,
                        notification_callback(connection_generation),
                    )
                    if (
                        disconnect_generation != attempt_disconnect_generation
                        or not bool(getattr(client, "is_connected", True))
                    ):
                        raise ConnectionError(
                            "gateway disconnected while restoring notifications"
                        )
                    notifications_enabled = True
                    defer_notifications = False
                    link_disconnected.clear()

                remaining_s = deadline - asyncio.get_running_loop().time()
                if remaining_s <= 0.0:
                    return False
                try:
                    await asyncio.wait_for(reconnect_once(), timeout=remaining_s)
                except asyncio.TimeoutError:
                    return False
                except Exception as exc:
                    last_error = f"{type(exc).__name__}: {exc}"
                    if "gateway identity mismatch" in str(exc):
                        disconnect_errors.append(str(exc))
                        transport_failed.set()
                        raise_transport_errors(label)
                    if bool(getattr(client, "is_connected", False)):
                        try:
                            await client.disconnect()
                        except Exception as cleanup_exc:
                            print(
                                "BLE_RECONNECT_CLEANUP "
                                f"error={type(cleanup_exc).__name__}: "
                                f"{cleanup_exc}",
                                flush=True,
                            )
                    # Drain the disconnect callback before another connection
                    # attempt reuses this Bleak client and its service cache.
                    await asyncio.sleep(0)
                    print(
                        "BLE_RECONNECT_RETRY "
                        f"attempt={attempt}/{attempts} error={last_error}",
                        flush=True,
                    )
                    link_disconnected.set()
                    continue
                print(
                    "BLE_RECONNECTED "
                    f"gateway_id=0x{gateway_id:016x} "
                    f"attempt={attempt}/{attempts}",
                    flush=True,
                )
                return True

            disconnect_errors.append(
                f"gateway BLE reconnect exhausted {attempts} attempts: {last_error}"
            )
            transport_failed.set()
            raise_transport_errors(label)
        return False

    async def wait_for_event_before(
        event: asyncio.Event,
        deadline: float,
        label: str,
    ) -> bool:
        """Wait through reconnects while retaining one absolute deadline."""
        while True:
            raise_transport_errors(label)
            if (
                link_disconnected.is_set()
                or not bool(getattr(client, "is_connected", True))
            ):
                link_disconnected.set()
                if not await reconnect_before(deadline, label):
                    return False
                continue
            if event.is_set():
                return True
            remaining_s = deadline - asyncio.get_running_loop().time()
            if remaining_s <= 0.0:
                return False
            event_wait = asyncio.create_task(event.wait())
            disconnect_wait = asyncio.create_task(link_disconnected.wait())
            failure_wait = asyncio.create_task(transport_failed.wait())
            waiters = (event_wait, disconnect_wait, failure_wait)
            try:
                done, _pending = await asyncio.wait(
                    waiters,
                    timeout=remaining_s,
                    return_when=asyncio.FIRST_COMPLETED,
                )
            finally:
                for waiter in waiters:
                    if not waiter.done():
                        waiter.cancel()
                await asyncio.gather(*waiters, return_exceptions=True)
            if not done:
                return False

    async def await_receipt_custody(deadline: float, label: str) -> None:
        """Require failed receipts to be replayed and accepted before exit."""
        while receipt_tasks or receipt_outstanding:
            receipt_tasks.difference_update(
                task for task in tuple(receipt_tasks) if task.done()
            )
            raise_transport_errors(label)
            if (
                link_disconnected.is_set()
                or not bool(getattr(client, "is_connected", True))
            ):
                link_disconnected.set()
                if not await reconnect_before(deadline, label):
                    break
                continue
            remaining_s = deadline - asyncio.get_running_loop().time()
            if remaining_s <= 0.0:
                break
            if receipt_tasks:
                tasks = tuple(receipt_tasks)
                try:
                    await asyncio.wait_for(
                        asyncio.gather(*tasks, return_exceptions=True),
                        timeout=remaining_s,
                    )
                except asyncio.TimeoutError:
                    break
                receipt_tasks.difference_update(tasks)
                continue
            receipt_state_changed.clear()
            if not receipt_outstanding:
                break
            if not await wait_for_event_before(
                receipt_state_changed,
                deadline,
                label,
            ):
                break
        if receipt_outstanding:
            raise RuntimeError(
                f"{label} failed: {len(receipt_outstanding)} retained gateway "
                "record receipt(s) were not replayed before the original deadline"
            )

    async def await_transport_duration(
        timeout_s: float,
        label: str,
        *,
        deadline: float | None = None,
    ) -> None:
        if deadline is None:
            deadline = asyncio.get_running_loop().time() + timeout_s
        await wait_for_event_before(transport_failed, deadline, label)
        raise_transport_errors(label)
        if link_disconnected.is_set():
            raise RuntimeError(
                f"{label} failed: gateway remained disconnected at the "
                "original monitoring deadline"
            )
        await await_receipt_custody(deadline, label)

    async def await_qualification(
        current: Qualification,
        timeout_s: float,
        label: str,
        *,
        deadline: float | None = None,
    ) -> None:
        loop = asyncio.get_running_loop()
        if deadline is None:
            deadline = loop.time() + timeout_s
        if not await wait_for_event_before(
            qualification_done,
            deadline,
            f"{label} qualification",
        ):
            raise RuntimeError(
                f"{label} qualification timed out after {timeout_s:.1f}s"
            )
        raise_transport_errors(f"{label} qualification")
        if isinstance(current, SurveyQualification):
            drain_deadline = deadline
            quiet_s = max(
                float(getattr(args, "survey_terminal_drain_quiet_s", 0.0)),
                float(args.notification_hold_s),
            )
            if quiet_s > 0.0:
                drain_deadline = min(
                    deadline,
                    loop.time() + SURVEY_TERMINAL_DRAIN_MAX_S,
                )

                # Legacy firmware can publish its terminal event before the
                # last reliable cleanup result reaches the BLE head. Keep the
                # exact-record receipt path alive until it has been quiet for
                # one configured interval, but retain an absolute bound so a
                # noisy peer cannot keep this command attached forever.
                survey_custody_activity.clear()
                while True:
                    if link_disconnected.is_set():
                        if not await reconnect_before(
                            drain_deadline,
                            f"{label} terminal drain",
                        ):
                            break
                        survey_custody_activity.clear()
                    remaining_s = drain_deadline - loop.time()
                    if remaining_s <= 0.0:
                        break
                    try:
                        await asyncio.wait_for(
                            survey_custody_activity.wait(),
                            timeout=min(quiet_s, remaining_s),
                        )
                    except asyncio.TimeoutError:
                        break
                    survey_custody_activity.clear()
            await await_receipt_custody(
                drain_deadline,
                f"{label} terminal drain",
            )
            raise_transport_errors(f"{label} terminal drain")
        else:
            await await_receipt_custody(
                deadline,
                f"{label} qualification receipt drain",
            )
        current.validate()

    async def await_assignment_replay_clear(timeout_s: float) -> None:
        if not assignment_replay_barrier.active:
            return
        print(
            "BLE_ASSIGNMENT_REPLAY_DRAIN waiting for exact terminal receipt",
            flush=True,
        )
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout_s
        while assignment_replay_barrier.active:
            assignment_replay_changed.clear()
            if not assignment_replay_barrier.active:
                break
            raise_transport_errors("assignment replay drain")
            if link_disconnected.is_set():
                if not await reconnect_before(
                    deadline,
                    "assignment replay drain",
                ):
                    raise RuntimeError(
                        "assignment replay terminal receipt timed out before "
                        "successor command"
                    )
                continue
            remaining_s = deadline - loop.time()
            if remaining_s <= 0.0:
                raise RuntimeError(
                    "assignment replay terminal receipt timed out before "
                    "successor command"
                )
            try:
                await asyncio.wait_for(
                    assignment_replay_changed.wait(),
                    timeout=remaining_s,
                )
            except asyncio.TimeoutError as exc:
                raise RuntimeError(
                    "assignment replay terminal receipt timed out before "
                    "successor command"
                ) from exc
        raise_transport_errors("assignment replay drain")
        print("BLE_ASSIGNMENT_REPLAY_DRAIN_COMPLETE", flush=True)

    gateway_target = await _resolve_gateway_target(
        args.gateway,
        args.connect_timeout,
    )
    async with _ble_client_session(
        gateway_target,
        timeout=args.connect_timeout,
        disconnected_callback=on_disconnect,
    ) as client:
        gateway_id = decode_gateway_identity(
            bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID))
        )
        delivery_dedup.set_gateway_id(gateway_id)
        connection_generation = 1
        characteristic = client.services.get_characteristic(PACKET_RX_UUID)
        if characteristic is None:
            raise RuntimeError("gateway packet RX characteristic is unavailable")
        chunk_size = max(
            1,
            min(int(characteristic.max_write_without_response_size or 20), 244),
        )
        if not defer_notifications:
            await client.start_notify(
                PACKET_TX_UUID,
                notification_callback(connection_generation),
            )
            notifications_enabled = True

        async def enable_notifications(deadline: float | None = None) -> None:
            nonlocal defer_notifications, notifications_enabled
            if notifications_enabled:
                return
            print(
                "BLE_NOTIFICATIONS_HELD "
                f"seconds={args.notification_hold_s:.3f}",
                flush=True,
            )
            if args.notification_hold_s > 0.0:
                if deadline is None:
                    await asyncio.sleep(args.notification_hold_s)
                else:
                    remaining_s = deadline - asyncio.get_running_loop().time()
                    if remaining_s <= 0.0:
                        raise RuntimeError(
                            "BLE notification hold exhausted the original "
                            "operation deadline"
                        )
                    try:
                        await asyncio.wait_for(
                            asyncio.sleep(args.notification_hold_s),
                            timeout=remaining_s,
                        )
                    except asyncio.TimeoutError as exc:
                        raise RuntimeError(
                            "BLE notification hold exhausted the original "
                            "operation deadline"
                        ) from exc
            reconnect_deadline = deadline
            if reconnect_deadline is None:
                reconnect_deadline = (
                    asyncio.get_running_loop().time() + args.connect_timeout
                )
            if (
                link_disconnected.is_set()
                or not bool(getattr(client, "is_connected", True))
            ):
                link_disconnected.set()
                if not await reconnect_before(
                    reconnect_deadline,
                    "BLE notification start",
                ):
                    raise RuntimeError(
                        "BLE notification reconnect exceeded its deadline"
                    )
                print("BLE_NOTIFICATIONS_ENABLED", flush=True)
                return
            await client.start_notify(
                PACKET_TX_UUID,
                notification_callback(connection_generation),
            )
            notifications_enabled = True
            defer_notifications = False
            if link_disconnected.is_set():
                if not await reconnect_before(
                    reconnect_deadline,
                    "BLE notification start",
                ):
                    raise RuntimeError(
                        "BLE notification reconnect exceeded its deadline"
                    )
            raise_transport_errors("BLE notification start")
            print("BLE_NOTIFICATIONS_ENABLED", flush=True)

        async def send_command(
            command_name: str,
            identity: int,
            command: object,
            *,
            timeout_s: float | None = None,
        ) -> float | None:
            nonlocal command_write_active

            if (
                link_disconnected.is_set()
                or not bool(getattr(client, "is_connected", True))
            ):
                link_disconnected.set()
                reconnect_deadline = (
                    asyncio.get_running_loop().time() + args.connect_timeout
                )
                if not await reconnect_before(reconnect_deadline, command_name):
                    raise RuntimeError(
                        f"{command_name} could not reconnect before command write"
                    )
            if characteristic is None:
                raise RuntimeError("gateway packet RX characteristic is unavailable")
            print(
                f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command={command_name} "
                f"session={identity} frame={command.frame.hex()}",
                flush=True,
            )
            command_generation = connection_generation
            command_write_active = True
            try:
                await send_gateway_frame(
                    client,
                    characteristic,
                    command.frame,
                    chunk_size=chunk_size,
                    expected_generation=command_generation,
                )
            except Exception as exc:
                if not disconnect_errors:
                    disconnect_errors.append(
                        "gateway command write failed; delivery is ambiguous "
                        "and the command will not be resent: "
                        f"{type(exc).__name__}: {exc}"
                    )
                transport_failed.set()
            finally:
                command_write_active = False
            raise_transport_errors(command_name)
            if timeout_s is None:
                return None
            return asyncio.get_running_loop().time() + timeout_s

        qualification_deadline: float | None = None

        if args.command == "monitor":
            print(
                f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command=monitor",
                flush=True,
            )
        elif args.command == "qualify-reachability":
            route_identity = _next_identity()
            operation_policy = _assignment_operation_policy(
                args.expected_anchors,
                assignment_command_budget_ms,
                deepest_hop=getattr(args, "deepest_hop", 0),
                ram_only_iteration=True,
            )
            route_args = {
                "host_id": args.host_id,
                "gateway_id": gateway_id,
                "session_id": route_identity,
                "seq": route_identity & 0xFFFF,
                "command_budget_ms": assignment_command_budget_ms,
                "operation_policy": operation_policy,
            }
            qualification = RouteRefreshQualification(
                route_identity,
                route_identity & 0xFFFF,
                route_identity,
            )
            await enable_notifications()
            route_timeout_s = _qualification_timeout_s(
                args.route_refresh_timeout,
                assignment_command_budget_ms
                or ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
            )
            qualification_deadline = await send_command(
                "here-i-am",
                route_identity,
                build_here_i_am_command(**route_args),
                timeout_s=route_timeout_s,
            )
            await await_qualification(
                qualification,
                route_timeout_s,
                "Here-I-Am local flood",
                deadline=qualification_deadline,
            )
            print(
                "HERE_I_AM_LOCAL_FLOOD_OK "
                f"attempts={len(qualification.flood_attempts)} "
                f"retries={qualification.retries} "
                "delivery_claim=local-broadcast-only",
                flush=True,
            )
            await await_assignment_replay_clear(args.assignment_timeout)

            assignment_identity = _next_identity(route_identity)
            assignment_policy = operation_policy
            assignment_args = {
                "host_id": args.host_id,
                "gateway_id": gateway_id,
                "session_id": assignment_identity,
                "seq": assignment_identity & 0xFFFF,
                "command_budget_ms": (
                    assignment_policy.assignment.operation_budget_ms
                ),
                "expected_anchor_count": args.expected_anchors,
                "operation_policy": assignment_policy,
            }
            qualification_done.clear()
            qualification = AssignmentQualification(
                assignment_identity,
                assignment_identity & 0xFFFF,
                assignment_identity,
                args.expected_anchors,
                require_hop_evidence=(
                    args.expected_direct_anchors is not None
                    or args.expected_multihop_anchors is not None
                    or bool(args.expected_anchor_hops)
                ),
                expected_direct_anchors=args.expected_direct_anchors,
                expected_multihop_anchors=args.expected_multihop_anchors,
                expected_anchor_hops=args.expected_anchor_hops,
            )
            assignment_timeout_s = _qualification_timeout_s(
                args.assignment_timeout,
                assignment_policy.assignment.operation_budget_ms,
            )
            qualification_deadline = await send_command(
                "assign-slots",
                assignment_identity,
                build_assign_discovery_slots_command(**assignment_args),
                timeout_s=assignment_timeout_s,
            )
            await await_qualification(
                qualification,
                assignment_timeout_s,
                "assignment reachability",
                deadline=qualification_deadline,
            )
            print(
                "HERE_I_AM_REACHABILITY_QUALIFICATION_OK "
                f"anchors={len(qualification.anchors)} "
                f"direct={qualification.direct_count} "
                f"multihop={qualification.multihop_count} "
                f"retries={qualification.retries}",
                flush=True,
            )
        else:
            for index in range(args.repeat):
                identity = _next_identity()
                command_args = {
                    "host_id": args.host_id,
                    "gateway_id": gateway_id,
                    "session_id": identity,
                    "seq": identity & 0xFFFF,
                    "command_budget_ms": command_budget_ms,
                }
                if args.command == "here-i-am":
                    command = build_here_i_am_command(**command_args)
                elif args.command == "abort-survey":
                    command_args.pop("command_budget_ms")
                    command = build_survey_abort_command(**command_args)
                elif args.command == "assign-slots":
                    assignment_policy = _assignment_operation_policy(
                        args.expected_anchors,
                        assignment_command_budget_ms,
                        deepest_hop=getattr(args, "deepest_hop", 0),
                        ram_only_iteration=True,
                    )
                    command_args["command_budget_ms"] = (
                        assignment_policy.assignment.operation_budget_ms
                    )
                    command = build_assign_discovery_slots_command(
                        **command_args,
                        expected_anchor_count=args.expected_anchors,
                        operation_policy=assignment_policy,
                    )
                    if args.require_assignment_success:
                        qualification_done.clear()
                        qualification = AssignmentQualification(
                            identity,
                            identity & 0xFFFF,
                            identity,
                            args.expected_anchors,
                            require_hop_evidence=(
                                args.expected_direct_anchors is not None
                                or args.expected_multihop_anchors is not None
                                or bool(args.expected_anchor_hops)
                            ),
                            expected_direct_anchors=args.expected_direct_anchors,
                            expected_multihop_anchors=args.expected_multihop_anchors,
                            expected_anchor_hops=args.expected_anchor_hops,
                        )
                else:
                    if args.require_survey_success and args.survey_id:
                        raise RuntimeError(
                            "survey qualification requires an auto-generated "
                            "survey ID so retained results from an earlier "
                            "operation cannot satisfy the proof"
                        )
                    survey_id = args.survey_id or identity
                    survey_policy = _survey_operation_policy(
                        expected_anchor_count=args.expected_anchors,
                        discovery_slot_count=args.discovery_slots,
                        report_grace_ms=args.survey_duration_ms,
                        deepest_hop=getattr(args, "deepest_hop", 0),
                    )
                    command = build_anchor_discovery_command(
                        **command_args,
                        survey_id=survey_id,
                        duration_ms=args.survey_duration_ms,
                        discovery_slot_count=args.discovery_slots,
                        sample_count=args.samples,
                        expected_anchor_count=args.expected_anchors,
                        operation_policy=survey_policy,
                    )
                    if args.require_survey_success:
                        qualification = SurveyQualification(
                            identity,
                            identity & 0xFFFF,
                            identity,
                            survey_id,
                            args.expected_anchors,
                            args.expected_pairs,
                        )
                if (
                    notifications_enabled
                    and gateway_command_requires_preflight(command.command_id)
                ):
                    # Give an already-retained publication one event-loop turn
                    # to enter the exact receipt barrier. A later publication
                    # race is still closed by firmware admission backpressure.
                    await asyncio.sleep(0)
                    await await_assignment_replay_clear(
                        args.assignment_timeout
                    )
                qualification_timeout_s = None
                if isinstance(qualification, SurveyQualification):
                    qualification_timeout_s = _qualification_timeout_s(
                        args.duration,
                        command_budget_ms
                        or SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
                    )
                elif isinstance(qualification, AssignmentQualification):
                    qualification_timeout_s = _qualification_timeout_s(
                        args.assignment_timeout,
                        assignment_command_budget_ms
                        or DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
                    )
                else:
                    qualification_timeout_s = args.duration
                qualification_deadline = await send_command(
                    args.command,
                    identity,
                    command,
                    timeout_s=qualification_timeout_s,
                )
                if defer_notifications:
                    await enable_notifications(qualification_deadline)
                if (
                    isinstance(qualification, AssignmentQualification)
                    and args.command == "assign-slots"
                    and args.require_assignment_success
                    and args.repeat > 1
                ):
                    await await_qualification(
                        qualification,
                        qualification_timeout_s,
                        f"assignment {index + 1}/{args.repeat}",
                        deadline=qualification_deadline,
                    )
                    print(
                        "ASSIGNMENT_QUALIFICATION_OK "
                        f"run={index + 1}/{args.repeat} "
                        f"anchors={len(qualification.anchors)} "
                        f"direct={qualification.direct_count} "
                        f"multihop={qualification.multihop_count} "
                        f"retries={qualification.retries}",
                        flush=True,
                    )
                    qualified_assignment_repeats += 1
                    qualification = None
                    qualification_deadline = None
                if index + 1 < args.repeat:
                    await asyncio.sleep(args.interval)
                    raise_transport_errors(args.command)
        if defer_notifications:
            await enable_notifications()
        if isinstance(qualification, SurveyQualification):
            await await_qualification(
                qualification,
                _qualification_timeout_s(
                    args.duration,
                    command_budget_ms
                    or SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
                ),
                "survey",
                deadline=qualification_deadline,
            )
            print(
                "SURVEY_QUALIFICATION_OK "
                f"survey={qualification.survey_id} "
                f"anchors={len(qualification.anchors)} "
                f"pairs={len(qualification.pair_successes)} "
                f"distances={len(qualification.geometry_model.pairs)} "
                f"geometry_rmse_m={qualification.geometry_rmse_m:.6f} "
                f"retries={qualification.retries}",
                flush=True,
            )
        elif (
            isinstance(qualification, AssignmentQualification)
            and args.command == "assign-slots"
        ):
            await await_qualification(
                qualification,
                _qualification_timeout_s(
                    args.assignment_timeout,
                    assignment_command_budget_ms
                    or DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
                ),
                "assignment",
                deadline=qualification_deadline,
            )
            print(
                "ASSIGNMENT_QUALIFICATION_OK "
                f"anchors={len(qualification.anchors)} "
                f"direct={qualification.direct_count} "
                f"multihop={qualification.multihop_count} "
                f"retries={qualification.retries}",
                flush=True,
            )
        elif (
            args.command != "qualify-reachability"
            and qualified_assignment_repeats == 0
        ):
            await await_transport_duration(
                args.duration,
                args.command,
                deadline=qualification_deadline,
            )
        while receipt_tasks:
            tasks = tuple(receipt_tasks)
            await asyncio.gather(*tasks)
            receipt_tasks.difference_update(tasks)
        raise_transport_errors("BLE session")
        print(f"BLE_COMPLETE packets={received}", flush=True)
        session_complete = True
    return qualification


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", required=True)
    parser.add_argument(
        "--command",
        choices=(
            "here-i-am",
            "assign-slots",
            "qualify-reachability",
            "survey",
            "abort-survey",
            "monitor",
        ),
        required=True,
    )
    parser.add_argument("--host-id", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument("--connect-timeout", type=float, default=12.0)
    parser.add_argument(
        "--reconnect-attempts",
        type=int,
        default=BLE_RECONNECT_ATTEMPTS_DEFAULT,
        help=(
            "bounded reconnect attempts after a completed command write; "
            "the command is never resent"
        ),
    )
    parser.add_argument(
        "--reconnect-delay-s",
        type=float,
        default=BLE_RECONNECT_DELAY_DEFAULT_S,
        help="delay before each bounded reconnect attempt",
    )
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0.05)
    parser.add_argument("--survey-id", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--survey-duration-ms", type=int, default=1000)
    parser.add_argument("--discovery-slots", type=int, default=6)
    parser.add_argument(
        "--samples",
        type=int,
        default=SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    )
    parser.add_argument("--notification-hold-s", type=float, default=0.0)
    parser.add_argument(
        "--survey-terminal-drain-quiet-s",
        type=float,
        default=SURVEY_TERMINAL_DRAIN_QUIET_DEFAULT_S,
        help=(
            "post-terminal quiet interval used to receipt delayed retained "
            "survey records before disconnect"
        ),
    )
    parser.add_argument("--require-survey-success", action="store_true")
    parser.add_argument("--require-assignment-success", action="store_true")
    parser.add_argument("--expected-anchors", type=int, default=3)
    parser.add_argument("--expected-pairs", type=int, default=3)
    parser.add_argument("--expected-direct-anchors", type=int)
    parser.add_argument("--expected-multihop-anchors", type=int)
    parser.add_argument(
        "--expected-anchor-hop",
        action="append",
        type=_parse_expected_anchor_hop,
        default=[],
        metavar="NODE_ID=HOPS",
        help=(
            "require an exact gateway hop count for one anchor; repeat once "
            "per expected anchor"
        ),
    )
    parser.add_argument(
        "--deepest-hop",
        type=int,
        default=0,
        help="Deepest expected hop count across the mesh (0 = automatic from expected anchors)",
    )
    parser.add_argument("--route-refresh-timeout", type=float, default=60.0)
    parser.add_argument("--assignment-timeout", type=float, default=240.0)
    parser.add_argument(
        "--command-budget-ms",
        type=int,
        help=(
            "firmware command deadline in "
            f"{GATEWAY_COMMAND_BUDGET_MIN_MS}..{GATEWAY_COMMAND_BUDGET_MAX_MS} ms; "
            "omitted keeps the protocol-specific robust default"
        ),
    )
    args = parser.parse_args()
    args.expected_anchor_hops = {}
    for node_id, hop_count in args.expected_anchor_hop:
        if node_id in args.expected_anchor_hops:
            parser.error(
                f"--expected-anchor-hop repeats node 0x{node_id:016x}"
            )
        args.expected_anchor_hops[node_id] = hop_count
    if args.notification_hold_s < 0.0:
        parser.error("--notification-hold-s must be non-negative")
    if args.reconnect_attempts < 0:
        parser.error("--reconnect-attempts must be non-negative")
    if args.reconnect_delay_s < 0.0:
        parser.error("--reconnect-delay-s must be non-negative")
    if args.survey_terminal_drain_quiet_s < 0.0:
        parser.error("--survey-terminal-drain-quiet-s must be non-negative")
    if args.require_survey_success and args.command != "survey":
        parser.error("--require-survey-success requires --command survey")
    if args.require_survey_success and args.repeat != 1:
        parser.error("survey qualification requires --repeat 1")
    if args.require_survey_success and args.survey_id != 0:
        parser.error(
            "survey qualification requires --survey-id 0 (fresh automatic identity)"
        )
    if args.require_survey_success and (
        args.expected_anchors < 2 or args.expected_pairs < 1
    ):
        parser.error("survey qualification requires at least two anchors and one pair")
    if args.require_assignment_success and args.command != "assign-slots":
        parser.error("--require-assignment-success requires --command assign-slots")
    if args.command == "qualify-reachability" and args.repeat != 1:
        parser.error("reachability qualification requires --repeat 1")
    if (
        args.require_assignment_success or args.command == "qualify-reachability"
    ) and not 1 <= args.expected_anchors <= 50:
        parser.error("assignment qualification requires 1..50 expected anchors")
    if args.route_refresh_timeout <= 0.0 or args.assignment_timeout <= 0.0:
        parser.error("qualification timeouts must be positive")
    if args.samples != SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT:
        parser.error(
            "--samples must be exactly "
            f"{SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT}"
        )
    if args.command_budget_ms is not None and not (
        GATEWAY_COMMAND_BUDGET_MIN_MS
        <= args.command_budget_ms
        <= GATEWAY_COMMAND_BUDGET_MAX_MS
    ):
        parser.error(
            "--command-budget-ms must be in "
            f"{GATEWAY_COMMAND_BUDGET_MIN_MS}..{GATEWAY_COMMAND_BUDGET_MAX_MS}"
        )
    if (
        args.command in ("assign-slots", "qualify-reachability")
        and args.command_budget_ms is not None
    ):
        required_assignment_budget_ms = assignment_required_budget_ms(
            ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
            args.expected_anchors,
            deepest_hop=args.deepest_hop,
        )
        if args.command_budget_ms < required_assignment_budget_ms:
            parser.error(
                "assignment --command-budget-ms must be at least "
                f"{required_assignment_budget_ms}"
            )
    if args.command == "survey" and args.command_budget_ms is not None:
        if args.command_budget_ms > SURVEY_GATEWAY_OPERATION_MAX_BUDGET_MS:
            parser.error(
                "survey --command-budget-ms must not exceed the 30-minute "
                f"safety limit ({SURVEY_GATEWAY_OPERATION_MAX_BUDGET_MS})"
            )
        required_survey_budget_ms = discovery_required_budget_ms(
            discovery_required_start_delay_ms(args.deepest_hop),
            DISCOVERY_DEFAULT_SLOT_MS,
            args.discovery_slots,
            DISCOVERY_DEFAULT_ROUND_COUNT,
            args.survey_duration_ms,
            args.deepest_hop,
        )
        if args.command_budget_ms < required_survey_budget_ms:
            parser.error(
                "survey --command-budget-ms must cover the selected discovery "
                f"policy: minimum {required_survey_budget_ms}"
            )
    for value, name in (
        (args.expected_direct_anchors, "--expected-direct-anchors"),
        (args.expected_multihop_anchors, "--expected-multihop-anchors"),
    ):
        if value is not None and value < 0:
            parser.error(f"{name} must be non-negative")
    if (
        args.expected_direct_anchors is not None
        and args.expected_multihop_anchors is not None
        and args.expected_direct_anchors + args.expected_multihop_anchors
        != args.expected_anchors
    ):
        parser.error("direct plus multihop anchors must equal --expected-anchors")
    if args.expected_anchor_hops and (
        args.command not in ("assign-slots", "qualify-reachability")
        or len(args.expected_anchor_hops) != args.expected_anchors
    ):
        parser.error(
            "--expected-anchor-hop requires assignment qualification and one "
            "entry per expected anchor"
        )
    if args.expected_anchor_hops:
        direct_hops = sum(
            hop_count == 1
            for hop_count in args.expected_anchor_hops.values()
        )
        if (
            args.expected_direct_anchors is not None
            and args.expected_direct_anchors != direct_hops
        ):
            parser.error(
                "--expected-direct-anchors contradicts --expected-anchor-hop"
            )
        if (
            args.expected_multihop_anchors is not None
            and args.expected_multihop_anchors !=
                len(args.expected_anchor_hops) - direct_hops
        ):
            parser.error(
                "--expected-multihop-anchors contradicts --expected-anchor-hop"
            )
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
