"""Host-owned sequencing for user-triggered gateway commands."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Literal

from .command_telemetry import GatewayCommandEvent, GatewayCommandRequestTracker
from .protocol import CMD_FORCE_REDISCOVERY


# These commands must remain immediate when they are exposed by the GUI.  They
# either are the preflight itself or exist to stop/maintain an active operation.
CMD_STOP_HEARTBEAT = 0x000A
CMD_SURVEY_ABORT = 0x0103
CMD_ML_LIVE_TRACKING_HEARTBEAT = 0x8004
CMD_ML_STOP_LIVE_TRACKING = 0x8005
PREFLIGHT_EXEMPT_COMMAND_IDS = frozenset(
    {
        CMD_STOP_HEARTBEAT,
        CMD_FORCE_REDISCOVERY,
        CMD_SURVEY_ABORT,
        CMD_ML_LIVE_TRACKING_HEARTBEAT,
        CMD_ML_STOP_LIVE_TRACKING,
    }
)
ROUTE_REFRESH_DEFAULT_BUDGET_MS = 120_000

CommandPhase = Literal["preflight", "target"]


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
        return self._advance(self.tracker.last_outcome, now=now)

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
        self, outcome: str, *, now: float | None
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
            self.phase = "target"
            self.current = target
            return GatewayCommandTransition(
                matched=True, dispatch=target, phase=phase
            )
        self._clear()
        return GatewayCommandTransition(
            matched=True, completed=True, outcome=outcome, phase=phase
        )

    def _clear(self) -> None:
        self.plan = None
        self.phase = None
        self.current = None
