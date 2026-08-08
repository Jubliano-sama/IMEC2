#!/usr/bin/env python3
"""Send mesh provisioning commands and inspect raw gateway BLE events."""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, field
import pathlib
import secrets
import sys

from bleak import BleakClient


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.gateway_gui.protocol import (  # noqa: E402
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    GATEWAY_COMMAND_BUDGET_MIN_MS,
    GATEWAY_IDENTITY_UUID,
    GatewayReceiveBuffer,
    MSG_GATEWAY_COMMAND_EVENT,
    Packet,
    PACKET_RX_UUID,
    PACKET_TX_UUID,
    ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
    SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS,
    SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_here_i_am_command,
    build_survey_abort_command,
    decode_gateway_identity,
)
from tools.gateway_gui.command_telemetry import (  # noqa: E402
    GatewayCommandEvent,
    decode_gateway_command_event,
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
CMD_SURVEY_START_PAIR = 0x0102
CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104
CMD_FORCE_REDISCOVERY = 0x000C
DISCOVERY_SLOT_UNAVAILABLE = 0xFF
ROUTE_REFRESH_MAX_LOCAL_ATTEMPTS = 9
QUALIFICATION_TIMEOUT_GUARD_S = 5.0


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
        effective_command_budget_ms / 1000.0 + QUALIFICATION_TIMEOUT_GUARD_S,
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
    anchors: set[int] = field(default_factory=set)
    assigned_slots: dict[int, int] = field(default_factory=dict)
    hop_paths: dict[int, tuple[int, int]] = field(default_factory=dict)
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
                    if event.previous_hop_id == 0:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} has hop count "
                            "without previous hop"
                        )
                    elif event.hop_count == 1 and (
                        event.previous_hop_id != event.anchor_id
                    ):
                        self._error(
                            f"direct anchor 0x{event.anchor_id:016x} has "
                            "contradictory previous-hop evidence"
                        )
                    elif event.hop_count > 1 and (
                        event.previous_hop_id == event.anchor_id
                    ):
                        self._error(
                            f"multihop anchor 0x{event.anchor_id:016x} "
                            "claims itself as the previous hop"
                        )
                    if prior_path is not None and prior_path != path:
                        self._error(
                            f"anchor 0x{event.anchor_id:016x} changed hop evidence"
                        )
                    else:
                        self.hop_paths[event.anchor_id] = path
        elif event.stage == GATEWAY_COMMAND_STAGE_ENUMERATION_COMPLETE:
            if event.command_status != 0 or event.reason != 0:
                self._error("assignment collection-complete event is not successful")
            if event.progress_count != self.expected_anchors:
                self._error("assignment collection-complete count is not exact")
            self.collection_complete = True
        elif event.stage == GATEWAY_COMMAND_STAGE_SCHEDULE_READY:
            if (
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
        if set(self.assigned_slots.values()) != set(range(self.expected_anchors)):
            self._error("published discovery slots are not unique and contiguous")
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
            if hop_count > 1 and previous_hop_id not in self.anchors:
                self._error(
                    f"anchor 0x{anchor_id:016x} has multihop predecessor "
                    f"0x{previous_hop_id:016x} outside the qualified roster"
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

    def observe_packet(self, packet: Packet) -> None:
        self.geometry_model.observe_pair_packet(packet)

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


async def run(args: argparse.Namespace) -> Qualification | None:
    decoder = GatewayReceiveBuffer()
    command_budget_ms = getattr(args, "command_budget_ms", None)
    received = 0
    decode_errors: list[str] = []
    disconnect_errors: list[str] = []
    qualification: Qualification | None = None
    qualification_done = asyncio.Event()
    transport_failed = asyncio.Event()

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
            received += 1
            print(
                f"BLE_PACKET type=0x{packet.msg_type:02x} src=0x{packet.src_id:016x} "
                f"dst=0x{packet.dst_id:016x} session={packet.session_id} "
                f"seq={packet.seq} payload={packet.payload.hex()}",
                flush=True,
            )
            if isinstance(qualification, SurveyQualification):
                qualification.observe_packet(packet)
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
                print(f"GATEWAY_COMMAND_EVENT {event}", flush=True)
                if qualification is not None and qualification.observe(event):
                    qualification_done.set()

    def on_disconnect(_client: object) -> None:
        disconnect_errors.append("gateway disconnected during active command or monitoring")
        qualification_done.set()
        transport_failed.set()

    def raise_transport_errors(label: str) -> None:
        if decode_errors:
            raise RuntimeError(
                f"{label} failed: BLE decode errors: " + "; ".join(decode_errors)
            )
        if disconnect_errors:
            raise RuntimeError(f"{label} failed: " + "; ".join(disconnect_errors))

    async def await_transport_duration(timeout_s: float, label: str) -> None:
        try:
            await asyncio.wait_for(transport_failed.wait(), timeout=timeout_s)
        except asyncio.TimeoutError:
            return
        raise_transport_errors(label)

    async def await_qualification(
        current: Qualification,
        timeout_s: float,
        label: str,
    ) -> None:
        try:
            await asyncio.wait_for(qualification_done.wait(), timeout=timeout_s)
        except asyncio.TimeoutError as exc:
            raise RuntimeError(
                f"{label} qualification timed out after {timeout_s:.1f}s"
            ) from exc
        raise_transport_errors(f"{label} qualification")
        current.validate()

    async with BleakClient(
        args.gateway,
        timeout=args.connect_timeout,
        disconnected_callback=on_disconnect,
    ) as client:
        gateway_id = decode_gateway_identity(
            bytes(await client.read_gatt_char(GATEWAY_IDENTITY_UUID))
        )
        characteristic = client.services.get_characteristic(PACKET_RX_UUID)
        if characteristic is None:
            raise RuntimeError("gateway packet RX characteristic is unavailable")
        chunk_size = max(
            1,
            min(int(characteristic.max_write_without_response_size or 20), 244),
        )
        strict_mode = (
            args.require_survey_success
            or args.require_assignment_success
            or args.command == "qualify-reachability"
        )
        defer_notifications = strict_mode or args.notification_hold_s > 0.0
        notifications_enabled = False
        if not defer_notifications:
            await client.start_notify(PACKET_TX_UUID, on_notify)
            notifications_enabled = True

        async def enable_notifications() -> None:
            nonlocal defer_notifications, notifications_enabled
            if notifications_enabled:
                return
            print(
                "BLE_NOTIFICATIONS_HELD "
                f"seconds={args.notification_hold_s:.3f}",
                flush=True,
            )
            if args.notification_hold_s > 0.0:
                await asyncio.sleep(args.notification_hold_s)
            await client.start_notify(PACKET_TX_UUID, on_notify)
            notifications_enabled = True
            defer_notifications = False
            raise_transport_errors("BLE notification start")
            print("BLE_NOTIFICATIONS_ENABLED", flush=True)

        async def send_command(command_name: str, identity: int, command: object) -> None:
            print(
                f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command={command_name} "
                f"session={identity} frame={command.frame.hex()}",
                flush=True,
            )
            for offset in range(0, len(command.frame), chunk_size):
                await client.write_gatt_char(
                    characteristic,
                    command.frame[offset : offset + chunk_size],
                    response=True,
                )
                raise_transport_errors(command_name)

        if args.command == "monitor":
            print(
                f"BLE_CONNECTED gateway_id=0x{gateway_id:016x} command=monitor",
                flush=True,
            )
        elif args.command == "qualify-reachability":
            route_identity = _next_identity()
            route_args = {
                "host_id": args.host_id,
                "gateway_id": gateway_id,
                "session_id": route_identity,
                "seq": route_identity & 0xFFFF,
                "command_budget_ms": command_budget_ms,
            }
            qualification = RouteRefreshQualification(
                route_identity,
                route_identity & 0xFFFF,
                route_identity,
            )
            await send_command(
                "here-i-am",
                route_identity,
                build_here_i_am_command(**route_args),
            )
            await enable_notifications()
            await await_qualification(
                qualification,
                _qualification_timeout_s(
                    args.route_refresh_timeout,
                    command_budget_ms
                    or ROUTE_REFRESH_OPERATION_DEFAULT_BUDGET_MS,
                ),
                "Here-I-Am local flood",
            )
            print(
                "HERE_I_AM_LOCAL_FLOOD_OK "
                f"attempts={len(qualification.flood_attempts)} "
                f"retries={qualification.retries} "
                "delivery_claim=local-broadcast-only",
                flush=True,
            )

            assignment_identity = _next_identity(route_identity)
            assignment_args = {
                "host_id": args.host_id,
                "gateway_id": gateway_id,
                "session_id": assignment_identity,
                "seq": assignment_identity & 0xFFFF,
                "command_budget_ms": command_budget_ms,
                "expected_anchor_count": args.expected_anchors,
            }
            qualification_done.clear()
            qualification = AssignmentQualification(
                assignment_identity,
                assignment_identity & 0xFFFF,
                assignment_identity,
                args.expected_anchors,
                require_hop_evidence=True,
                expected_direct_anchors=args.expected_direct_anchors,
                expected_multihop_anchors=args.expected_multihop_anchors,
            )
            await send_command(
                "assign-slots",
                assignment_identity,
                build_assign_discovery_slots_command(**assignment_args),
            )
            await await_qualification(
                qualification,
                _qualification_timeout_s(
                    args.assignment_timeout,
                    command_budget_ms
                    or DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
                ),
                "assignment reachability",
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
                    command = build_assign_discovery_slots_command(
                        **command_args,
                        expected_anchor_count=args.expected_anchors,
                    )
                    if args.require_assignment_success:
                        qualification = AssignmentQualification(
                            identity,
                            identity & 0xFFFF,
                            identity,
                            args.expected_anchors,
                            expected_direct_anchors=args.expected_direct_anchors,
                            expected_multihop_anchors=args.expected_multihop_anchors,
                        )
                else:
                    if args.require_survey_success and args.survey_id:
                        raise RuntimeError(
                            "survey qualification requires an auto-generated "
                            "survey ID so retained results from an earlier "
                            "operation cannot satisfy the proof"
                        )
                    survey_id = args.survey_id or identity
                    command = build_anchor_discovery_command(
                        **command_args,
                        survey_id=survey_id,
                        duration_ms=args.survey_duration_ms,
                        discovery_slot_count=args.discovery_slots,
                        sample_count=args.samples,
                        expected_anchor_count=(
                            args.expected_anchors
                            if args.require_survey_success
                            else None
                        ),
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
                await send_command(args.command, identity, command)
                if defer_notifications:
                    await enable_notifications()
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
                    command_budget_ms
                    or DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
                ),
                "assignment",
            )
            print(
                "ASSIGNMENT_QUALIFICATION_OK "
                f"anchors={len(qualification.anchors)} "
                f"direct={qualification.direct_count} "
                f"multihop={qualification.multihop_count} "
                f"retries={qualification.retries}",
                flush=True,
            )
        elif args.command != "qualify-reachability":
            await await_transport_duration(args.duration, args.command)
        raise_transport_errors("BLE session")
        print(f"BLE_COMPLETE packets={received}", flush=True)
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
    parser.add_argument("--require-survey-success", action="store_true")
    parser.add_argument("--require-assignment-success", action="store_true")
    parser.add_argument("--expected-anchors", type=int, default=3)
    parser.add_argument("--expected-pairs", type=int, default=3)
    parser.add_argument("--expected-direct-anchors", type=int)
    parser.add_argument("--expected-multihop-anchors", type=int)
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
    if args.notification_hold_s < 0.0:
        parser.error("--notification-hold-s must be non-negative")
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
    if (
        args.require_assignment_success or args.command == "qualify-reachability"
    ) and args.repeat != 1:
        parser.error("assignment qualification requires --repeat 1")
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
        and args.command_budget_ms <
            DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS
    ):
        parser.error(
            "assignment --command-budget-ms must be at least "
            f"{DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS}"
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
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
