"""Host-owned sequencing for user-triggered gateway commands."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Literal

from .command_telemetry import GatewayCommandEvent, GatewayCommandRequestTracker
from .protocol import CMD_FORCE_REDISCOVERY, CMD_REBOOT, CMD_CLEAR_ROUTE


# These commands must remain immediate when they are exposed by the GUI.  They
# either are the preflight itself or exist to stop/maintain an active operation.
CMD_STOP_HEARTBEAT = 0x000A
CMD_ML_LIVE_TRACKING_HEARTBEAT = 0x8004
CMD_ML_STOP_LIVE_TRACKING = 0x8005
PREFLIGHT_EXEMPT_COMMAND_IDS = frozenset(
    {
        CMD_REBOOT,
        CMD_CLEAR_ROUTE,
        CMD_STOP_HEARTBEAT,
        CMD_FORCE_REDISCOVERY,
        CMD_ML_LIVE_TRACKING_HEARTBEAT,
        CMD_ML_STOP_LIVE_TRACKING,
    }
)
ROUTE_REFRESH_DEFAULT_BUDGET_MS = 120_000
# The firmware may publish its terminal event exactly at the advertised
# operation deadline.  Keep every host command owner alive long enough for
# that event to traverse the retained BLE stream and for its receipt to be
# written back.  The headless qualification client uses this same boundary.
GATEWAY_COMMAND_COMPLETION_GUARD_S = 5.0

CommandPhase = Literal["preflight", "target_wait", "target"]


@dataclass(frozen=True)
class GatewayAssignmentReplayReceipt:
    """Exact reliable assignment event whose terminal receipt releases the barrier."""

    correlation_key: tuple[int, int, int, int]
    gateway_sequence: int
    event_sequence: int
    terminal: bool


class GatewayAssignmentReplayBarrier:
    """Keep a successor command behind unfinished assignment publication.

    Publication can resume after either a cold boot or a same-boot BLE client
    loss. Only the cold-boot form carries the REPLAY flag, so the assignment
    command kind is the stable boundary shared by both forms. Every ordinary
    command waits because the retained mapping/ACK owner is shared; explicitly
    immediate maintenance and abort commands remain exempt.
    """

    ASSIGNMENT_COMMAND_KIND = 1

    def __init__(self) -> None:
        self._active: set[tuple[int, int, int, int]] = set()
        self._terminal_by_key: dict[
            tuple[int, int, int, int], GatewayAssignmentReplayReceipt
        ] = {}

    @property
    def active(self) -> bool:
        return bool(self._active)

    def blocks(self, command_id: int) -> bool:
        return self.active and gateway_command_requires_preflight(command_id)

    def observe(
        self, event: GatewayCommandEvent
    ) -> GatewayAssignmentReplayReceipt | None:
        if event.command_kind != self.ASSIGNMENT_COMMAND_KIND:
            return None
        key = event.correlation_key
        self._active.add(key)
        token = GatewayAssignmentReplayReceipt(
            correlation_key=key,
            gateway_sequence=event.gateway_sequence,
            event_sequence=event.event_sequence,
            terminal=event.terminal,
        )
        if token.terminal:
            self._terminal_by_key[key] = token
        return token

    def receipt_written(self, token: GatewayAssignmentReplayReceipt) -> bool:
        if not token.terminal:
            return False
        if self._terminal_by_key.get(token.correlation_key) != token:
            return False
        self._terminal_by_key.pop(token.correlation_key, None)
        self._active.discard(token.correlation_key)
        return True

    def reset(self) -> None:
        self._active.clear()
        self._terminal_by_key.clear()


def gateway_command_requires_preflight(command_id: int) -> bool:
    """Default every user command to preflight unless immediacy is required."""

    return command_id not in PREFLIGHT_EXEMPT_COMMAND_IDS


@dataclass(frozen=True)
class GatewayCommandDispatch:
    command_kind: int
    command_id: int
    session_id: int
    sequence: int
    frame: bytes
    label: str
    timeout_s: float
    status_text: str
    on_dispatch: Callable[[], None] | None = None

    def __post_init__(self) -> None:
        if self.command_kind <= 0:
            raise ValueError("command kind must be positive")
        if not 0 <= self.command_id <= 0xFFFF:
            raise ValueError("command ID must fit uint16")
        if not 1 <= self.session_id <= 0xFFFFFFFF:
            raise ValueError("command session ID must be non-zero uint32")
        if not 1 <= self.sequence <= 0xFFFF:
            raise ValueError("command sequence must be non-zero uint16")
        if not self.frame:
            raise ValueError("command frame must not be empty")
        if self.timeout_s <= 0.0:
            raise ValueError("command timeout must be positive")


@dataclass(frozen=True)
class GatewayCommandPlan:
    target: GatewayCommandDispatch
    preflight: GatewayCommandDispatch | None

    @classmethod
    def user_triggered(
        cls,
        target: GatewayCommandDispatch,
        *,
        preflight: GatewayCommandDispatch | None = None,
    ) -> "GatewayCommandPlan":
        required = gateway_command_requires_preflight(target.command_id)
        if required and preflight is None:
            raise ValueError("user-triggered command requires a Here-I-Am preflight")
        if not required and preflight is not None:
            raise ValueError("immediate control command must not be preflighted")
        if preflight is not None:
            if preflight.command_id != CMD_FORCE_REDISCOVERY:
                raise ValueError("preflight must be a Here-I-Am command")
            if (
                preflight.session_id,
                preflight.sequence,
            ) == (target.session_id, target.sequence):
                raise ValueError("preflight and target identities must differ")
        return cls(target=target, preflight=preflight)


@dataclass(frozen=True)
class GatewayCommandTransition:
    matched: bool = False
    dispatch: GatewayCommandDispatch | None = None
    completed: bool = False
    outcome: str | None = None
    phase: CommandPhase | None = None


class GatewayCommandOrchestrator:
    """Own one GUI operation across its route preflight and target command."""

    def __init__(self, tracker: GatewayCommandRequestTracker) -> None:
        self.tracker = tracker
        self.plan: GatewayCommandPlan | None = None
        self.phase: CommandPhase | None = None
        self.current: GatewayCommandDispatch | None = None

    @property
    def active(self) -> bool:
        return self.plan is not None

    def begin(self, plan: GatewayCommandPlan, *, now: float | None = None) -> GatewayCommandDispatch | None:
        if self.plan is not None:
            self.tracker.last_outcome = "busy"
            return None
        first = plan.preflight or plan.target
        phase: CommandPhase = "preflight" if plan.preflight is not None else "target"
        if not self.tracker.begin(
            first.command_kind,
            first.session_id,
            first.sequence,
            now=now,
            timeout_s=first.timeout_s,
        ):
            return None
        self.plan = plan
        self.phase = phase
        self.current = first
        return first

    def observe_event(
        self,
        event: GatewayCommandEvent,
        *,
        now: float | None = None,
        received_at: float | None = None,
        target_dispatch_allowed: bool = True,
    ) -> GatewayCommandTransition:
        boundary_time = received_at if received_at is not None else now
        if boundary_time is not None:
            expired = self.expire(now=boundary_time)
            if expired.matched:
                return expired
        current = self.current
        if current is None or not event.terminal:
            return GatewayCommandTransition()
        if (
            event.command_kind != current.command_kind
            or event.command_id != current.command_id
            or event.correlation_id != current.session_id
            or event.host_session_id != current.session_id
            or event.host_sequence != current.sequence
        ):
            return GatewayCommandTransition()
        if not self.tracker.observe_event(event):
            return GatewayCommandTransition()
        return self._advance(
            self.tracker.last_outcome,
            now=now,
            target_dispatch_allowed=target_dispatch_allowed,
        )

    def observe_command_result(
        self,
        *,
        command_id: int,
        host_session_id: int,
        host_sequence: int,
        command_status: int,
        now: float | None = None,
        received_at: float | None = None,
    ) -> GatewayCommandTransition:
        boundary_time = received_at if received_at is not None else now
        if boundary_time is not None:
            expired = self.expire(now=boundary_time)
            if expired.matched:
                return expired
        current = self.current
        # Successful command results can precede the typed lifecycle terminal,
        # so only a negative result may terminate an operation here.
        if current is None or command_status == 0:
            return GatewayCommandTransition()
        if (
            command_id != current.command_id
            or host_session_id != current.session_id
            or host_sequence != current.sequence
        ):
            return GatewayCommandTransition()
        if not self.tracker.observe_command_result(
            host_session_id, host_sequence, command_status
        ):
            return GatewayCommandTransition()
        return self._advance(self.tracker.last_outcome, now=now)

    def release_waiting_target(
        self, *, now: float | None = None
    ) -> GatewayCommandTransition:
        if self.phase != "target_wait" or self.plan is None:
            return GatewayCommandTransition()
        target = self.plan.target
        self.phase = "target"
        self.current = target
        return GatewayCommandTransition(
            matched=True,
            dispatch=target,
            phase="target_wait",
        )

    def expire(self, *, now: float | None = None) -> GatewayCommandTransition:
        if not self.tracker.expire(now=now):
            return GatewayCommandTransition()
        phase = self.phase
        self._clear()
        return GatewayCommandTransition(
            matched=True, completed=True, outcome="timeout", phase=phase
        )

    def disconnect(self) -> GatewayCommandTransition:
        had_active = self.active or self.tracker.pending is not None
        phase = self.phase
        self.tracker.disconnect()
        self._clear()
        return GatewayCommandTransition(
            matched=had_active,
            completed=had_active,
            outcome="disconnected" if had_active else None,
            phase=phase,
        )

    def _advance(
        self,
        outcome: str,
        *,
        now: float | None,
        target_dispatch_allowed: bool = True,
    ) -> GatewayCommandTransition:
        plan = self.plan
        phase = self.phase
        if plan is None:
            self._clear()
            return GatewayCommandTransition(
                matched=True, completed=True, outcome=outcome, phase=phase
            )
        if self.phase == "preflight" and outcome == "complete":
            target = plan.target
            if not self.tracker.begin(
                target.command_kind,
                target.session_id,
                target.sequence,
                now=now,
                timeout_s=target.timeout_s,
            ):
                self._clear()
                return GatewayCommandTransition(
                    matched=True, completed=True, outcome="failed", phase=phase
                )
            if not target_dispatch_allowed:
                self.phase = "target_wait"
                self.current = target
                return GatewayCommandTransition(
                    matched=True,
                    phase="preflight",
                )
            self.phase = "target"
            self.current = target
            return GatewayCommandTransition(
                matched=True, dispatch=target, phase=phase
            )
        self._clear()
        return GatewayCommandTransition(
            matched=True, completed=True, outcome=outcome, phase=phase
        )

    def reset(self) -> None:
        self._clear()
        self.tracker.reset()

    def _clear(self) -> None:
        self.plan = None
        self.phase = None
        self.current = None
