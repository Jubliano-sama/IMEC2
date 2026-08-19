"""Exact decoder and model for gateway command observability schema v1."""

from __future__ import annotations

from dataclasses import dataclass
import time


class CommandTelemetryDecodeError(ValueError):
    pass


@dataclass(frozen=True)
class GatewayCommandEvent:
    command_kind: int
    stage: int
    flags: int
    attempt: int
    command_status: int
    reason: int
    command_id: int
    route_epoch: int
    correlation_id: int
    gateway_sequence: int
    host_session_id: int
    host_sequence: int
    event_sequence: int
    anchor_id: int
    pair_initiator_id: int
    pair_responder_id: int
    previous_hop_id: int
    progress_count: int
    total_count: int
    success_count: int
    failure_count: int
    duplicate_count: int
    lost_event_count: int
    hop_count: int
    discovery_slot: int

    @property
    def correlation_key(self) -> tuple[int, int, int, int]:
        return self.command_kind, self.correlation_id, self.host_session_id, self.host_sequence

    @property
    def terminal(self) -> bool:
        return bool(self.flags & 0x01) and self.stage == 12


GATEWAY_COMMAND_KIND_NAMES = {1: "Anchor enumeration", 2: "Anchor survey", 3: "Route refresh"}
GATEWAY_COMMAND_STAGE_NAMES = {
    1: "Accepted", 2: "Queued", 3: "Dispatching", 4: "Flood attempt",
    5: "Retry / backoff", 6: "Anchor enumerated", 7: "Enumeration complete",
    8: "Schedule ready", 9: "Pair started", 10: "Pair succeeded",
    11: "Pair failed", 12: "Terminal",
}
GATEWAY_COMMAND_REASON_NAMES = {
    0: "None", 1: "Invalid request", 2: "Busy", 3: "No anchors", 4: "Capacity",
    5: "Radio", 6: "Timeout", 7: "Malformed response", 8: "Route unavailable",
    9: "Retry exhausted", 10: "Pair incomplete", 11: "Pair range failed",
    12: "Aborted", 13: "Internal", 14: "Survey radio preparation failed",
}


@dataclass(frozen=True)
class PendingGatewayCommand:
    command_kind: int
    host_session_id: int
    host_sequence: int
    started_at: float
    timeout_s: float


class GatewayCommandRequestTracker:
    """Own one host command until typed terminal, result, timeout, or disconnect."""

    def __init__(self, *, timeout_s: float = 100.0) -> None:
        self.timeout_s = timeout_s
        self.pending: PendingGatewayCommand | None = None
        self.last_outcome = "idle"
    def reset(self) -> None:
        self.pending = None
        self.last_outcome = "idle"

    def begin(self, command_kind: int, host_session_id: int, host_sequence: int,
              *, now: float | None = None,
              timeout_s: float | None = None) -> bool:
        self.expire(now=now)
        if self.pending is not None:
            self.last_outcome = "busy"
            return False
        effective_timeout_s = self.timeout_s if timeout_s is None else timeout_s
        if effective_timeout_s <= 0:
            raise ValueError("command timeout must be positive")
        self.pending = PendingGatewayCommand(
            command_kind, host_session_id, host_sequence,
            time.monotonic() if now is None else now,
            effective_timeout_s,
        )
        self.last_outcome = "pending"
        return True

    def observe_event(self, event: GatewayCommandEvent) -> bool:
        pending = self.pending
        if pending is None or not event.terminal:
            return False
        if (event.command_kind, event.host_session_id, event.host_sequence) != (
            pending.command_kind, pending.host_session_id, pending.host_sequence
        ):
            return False
        self.pending = None
        self.last_outcome = "complete" if event.command_status == 0 and event.reason == 0 else "failed"
        return True

    def observe_command_result(self, host_session_id: int, host_sequence: int,
                               command_status: int) -> bool:
        pending = self.pending
        if pending is None or (host_session_id, host_sequence) != (
            pending.host_session_id, pending.host_sequence
        ):
            return False
        self.pending = None
        self.last_outcome = "complete" if command_status == 0 else "failed"
        return True

    def expire(self, *, now: float | None = None) -> bool:
        if self.pending is None:
            return False
        current = time.monotonic() if now is None else now
        if current - self.pending.started_at < self.pending.timeout_s:
            return False
        self.pending = None
        self.last_outcome = "timeout"
        return True

    def disconnect(self) -> None:
        self.pending = None
        self.last_outcome = "disconnected"


def decode_gateway_command_event(raw: bytes, *, valid_statuses: set[int]) -> GatewayCommandEvent:
    if len(raw) != 78 or raw[0] != 1 or raw[1] != 78:
        raise CommandTelemetryDecodeError("gateway command event requires schema 1 and exactly 78 bytes")
    if raw[2] not in GATEWAY_COMMAND_KIND_NAMES or raw[3] not in GATEWAY_COMMAND_STAGE_NAMES:
        raise CommandTelemetryDecodeError("gateway command event has unknown kind or stage")
    if raw[4] & ~0x0F or raw[6] not in valid_statuses or raw[7] not in GATEWAY_COMMAND_REASON_NAMES:
        raise CommandTelemetryDecodeError("gateway command event has unknown flags, status, or reason")
    if raw[26:28] != b"\x00\x00":
        raise CommandTelemetryDecodeError("gateway command event reserved field is nonzero")
    u16 = lambda offset: int.from_bytes(raw[offset:offset + 2], "little")
    u32 = lambda offset: int.from_bytes(raw[offset:offset + 4], "little")
    u64 = lambda offset: int.from_bytes(raw[offset:offset + 8], "little")
    return GatewayCommandEvent(
        raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], u16(8), u16(10),
        u32(12), u32(16), u32(20), u16(24), u32(28), u64(32), u64(40),
        u64(48), u64(56), u16(64), u16(66), u16(68), u16(70), u16(72),
        u16(74), raw[76], raw[77],
    )
