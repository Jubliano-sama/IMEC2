"""Small targeted actions on the current connection's enumerated anchors."""

from __future__ import annotations

from dataclasses import dataclass, field
import time

from .command_telemetry import GatewayCommandEvent, is_enumeration_count_mismatch
from .protocol import (
    CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY, COMMAND_STATUS_NAMES,
    MSG_COMMAND_RESULT, TLV_ANCHOR_ID, TLV_BATTERY_MV, TLV_COMMAND_ID,
    TLV_COMMAND_STATUS, TLV_DISCOVERY_ASSIGNMENT_EPOCH, TLV_DURATION_MS,
    TLV_HOP_COUNT, TLV_NODE_BOOT_COUNTER, TLV_TIMESTAMP_MS,
    CommandFrame, Packet, _build_command_frame, append_tlv,
)

ANCHOR_ACTION_COMMANDS = frozenset((CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY))
# Mirrors app_anchor_actions.h. These are safety deadlines, not a claim that
# every action takes this long. Early correlated replies complete immediately.
ANCHOR_ACTION_PER_HOP_MS = 12_000
ANCHOR_ACTION_HOST_GUARD_MS = 5_000


def anchor_action_timeout_s(hop_count: int) -> float:
    if type(hop_count) is not int or not 1 <= hop_count <= 8:
        raise ValueError("Anchor hop depth must be known from enumeration (1..8).")
    return (2 * hop_count * ANCHOR_ACTION_PER_HOP_MS + ANCHOR_ACTION_HOST_GUARD_MS) / 1000


@dataclass(frozen=True)
class EnumeratedAnchor:
    node_id: int
    slot: int
    hop_count: int


@dataclass(frozen=True)
class AnchorActionRequest:
    anchor: EnumeratedAnchor
    command_id: int
    epoch: int
    session_id: int
    sequence: int


@dataclass(frozen=True)
class AnchorActionReply:
    status: int
    text: str
    battery_mv: int | None = None
    boot_counter: int | None = None
    sampled_at_ms: int | None = None
    age_ms: int | None = None
    received_at: float = field(default_factory=time.monotonic)
    stale_assignment: bool = False


class AnchorActions:
    def __init__(self) -> None:
        self.gateway_id: int | None = None
        self.epoch = 0
        self.anchors: dict[int, EnumeratedAnchor] = {}
        self.pending: AnchorActionRequest | None = None
        self.replies: dict[int, AnchorActionReply] = {}
        self.batteries: dict[int, AnchorActionReply] = {}
        self.battery_queue: list[int] | None = None
        self.battery_outcomes: dict[int, str] = {}

    def reset(self) -> None:
        self.gateway_id = None
        self.epoch = 0
        self.anchors.clear()
        self.pending = None
        self.replies.clear()
        self.batteries.clear()
        self.battery_queue = None
        self.battery_outcomes.clear()

    def remember_enumeration(self, gateway_id: int, terminal: GatewayCommandEvent,
                             details: dict[int, GatewayCommandEvent]) -> None:
        count_mismatch = is_enumeration_count_mismatch(terminal)
        if (not terminal.terminal or terminal.command_kind != 1 or terminal.flags & 4
            or terminal.command_status != 0 or not terminal.gateway_sequence
            or not details or len(details) != terminal.success_count
            or ((terminal.reason or terminal.failure_count) and not count_mismatch)):
            raise ValueError("A successful enumeration is required before anchor commands.")
        anchors = {}
        for node_id, detail in details.items():
            if (detail.correlation_key != terminal.correlation_key
                or detail.gateway_sequence != terminal.gateway_sequence
                or detail.anchor_id != node_id or not 0 < node_id < 0xFFFFFFFFFFFFFFFF
                or not 0 <= detail.discovery_slot < 50):
                raise ValueError("Enumeration contains an invalid anchor identity or slot.")
            anchor_action_timeout_s(detail.hop_count)
            anchors[node_id] = EnumeratedAnchor(node_id, detail.discovery_slot, detail.hop_count)
        if len({anchor.slot for anchor in anchors.values()}) != len(anchors):
            raise ValueError("Enumeration contains conflicting anchor slots.")
        self.gateway_id, self.epoch = gateway_id, terminal.gateway_sequence
        self.anchors = anchors
        self.replies.clear()
        self.batteries.clear()
        self.battery_queue = None
        self.battery_outcomes.clear()

    @property
    def battery_batch_active(self) -> bool:
        return self.battery_queue is not None

    def begin_battery_batch(self) -> None:
        if not self.gateway_id or not self.epoch or not self.anchors:
            raise ValueError("Enumerate anchors before reading batteries.")
        if self.pending is not None or self.battery_batch_active:
            raise ValueError("An anchor command is already active.")
        self.battery_queue = [a.node_id for a in sorted(self.anchors.values(), key=lambda a: a.slot)]
        self.battery_outcomes.clear()

    def finish_battery_target(self, anchor_id: int, outcome: str) -> None:
        if not self.battery_queue or self.battery_queue[0] != anchor_id:
            return
        self.battery_outcomes[anchor_id] = outcome
        self.battery_queue.pop(0)
        if not self.battery_queue:
            self.battery_queue = None

    def cancel_battery_batch(self) -> None:
        for node_id in self.battery_queue or ():
            self.battery_outcomes[node_id] = "cancelled"
        self.battery_queue = None

    def battery_text(self, anchor_id: int, *, now: float | None = None) -> str:
        reply = self.batteries.get(anchor_id)
        if reply is None or reply.battery_mv is None:
            return "Battery: not read yet"
        elapsed = max(0.0, (time.monotonic() if now is None else now) - reply.received_at)
        freshness = (f"received {elapsed:.0f} s ago" if reply.age_ms is None else
                     f"sample age {reply.age_ms / 1000 + elapsed:.0f} s")
        return f"Battery: {reply.battery_mv / 1000:.3f} V · {freshness}"

    def prepare(self, *, gateway_id: int, host_id: int, anchor_id: int,
                command_id: int, session_id: int, sequence: int) -> CommandFrame:
        if self.pending is not None:
            raise ValueError("An anchor command is already active.")
        if gateway_id != self.gateway_id or not self.epoch or anchor_id not in self.anchors:
            raise ValueError("Enumerate anchors on this connection before using these commands.")
        if command_id not in ANCHOR_ACTION_COMMANDS:
            raise ValueError("Unknown anchor action.")
        if self.battery_batch_active and (command_id != CMD_READ_ANCHOR_BATTERY or
                                          not self.battery_queue or self.battery_queue[0] != anchor_id):
            raise ValueError("Wait until all batteries have been read.")
        anchor = self.anchors[anchor_id]
        payload = bytearray()
        for tag, value, width in ((TLV_COMMAND_ID, command_id, 2),
                                  (TLV_DISCOVERY_ASSIGNMENT_EPOCH, self.epoch, 4),
                                  (TLV_HOP_COUNT, anchor.hop_count, 1)):
            append_tlv(payload, tag, value.to_bytes(width, "little"))
        frame = _build_command_frame(label="Identify anchor" if command_id == CMD_IDENTIFY_ANCHOR
                                     else "Read anchor battery", command_id=command_id,
            host_id=host_id, dst_id=anchor_id, session_id=session_id, seq=sequence,
            payload=bytes(payload))
        self.pending = AnchorActionRequest(anchor, command_id, self.epoch, session_id, sequence)
        return frame

    def observe(self, packet: Packet) -> AnchorActionReply | None:
        request = self.pending
        if (request is None or packet.msg_type != MSG_COMMAND_RESULT
            or packet.session_id != request.session_id or packet.seq != request.sequence
            or packet.src_id not in (self.gateway_id, request.anchor.node_id)):
            return None

        def scalar(tag: int) -> int:
            values = [tlv for tlv in packet.tlvs if tlv.type_id == tag]
            if (len(values) != 1 or values[0].decode_error or values[0].truncated
                or type(values[0].decoded) is not int):
                raise ValueError("Anchor reply has a missing, duplicate or malformed field.")
            return values[0].decoded

        if scalar(TLV_COMMAND_ID) != request.command_id:
            return None
        status = scalar(TLV_COMMAND_STATUS)
        if status not in COMMAND_STATUS_NAMES:
            raise ValueError("Anchor reply has an unknown command status.")
        if status != 0:
            message = f"Command failed: {COMMAND_STATUS_NAMES[status]}"
            reported_epoch = packet.value(TLV_DISCOVERY_ASSIGNMENT_EPOCH)
            stale_assignment = (status == 7 and packet.src_id == request.anchor.node_id
                                and isinstance(reported_epoch, int) and reported_epoch != request.epoch)
            if stale_assignment:
                message += "; anchor assignment changed, enumerate again before retrying"
            reply = AnchorActionReply(status, message, stale_assignment=stale_assignment)
        else:
            if packet.src_id != request.anchor.node_id:
                return None  # Gateway admission cannot stand in for the anchor's result.
            scalar(TLV_DISCOVERY_ASSIGNMENT_EPOCH)  # Observed state may change across reset.
            if scalar(TLV_ANCHOR_ID) != request.anchor.node_id:
                raise ValueError("Anchor reply does not match the selected enumerated anchor.")
            boot, sampled = scalar(TLV_NODE_BOOT_COUNTER), scalar(TLV_TIMESTAMP_MS)
            if not boot:
                raise ValueError("Anchor reply has no boot identity.")
            # Stream v1 reports gateway queue age, not sample age.
            age = packet.age_ms if packet.age_kind == "message_age_ms" else None
            if request.command_id == CMD_READ_ANCHOR_BATTERY:
                mv = scalar(TLV_BATTERY_MV)
                freshness = f"sample age {age / 1000:.1f} s" if age is not None else "just received"
                reply = AnchorActionReply(status, f"Battery: {mv / 1000:.3f} V · {freshness}",
                                          mv, boot, sampled, age)
            else:
                if scalar(TLV_DURATION_MS) != 10_000:
                    raise ValueError("Anchor did not accept a ten-second identification.")
                reply = AnchorActionReply(status, "10-second RGB identification accepted",
                                          boot_counter=boot, sampled_at_ms=sampled, age_ms=age)
        if reply.battery_mv is not None:
            self.batteries[request.anchor.node_id] = reply
        self.replies[request.anchor.node_id] = reply
        return reply
