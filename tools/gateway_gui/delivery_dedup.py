"""Bounded host-session idempotence for gateway click-report delivery.

The gateway's durable click journal can replay a packet after it has already
completed a BLE notification but before its NVS record is cleared.  This cache
keeps that at-least-once replay from becoming a second GUI record while the
GUI process remains alive.  It deliberately does not persist across process
restarts and it never treats a changed payload as an exact duplicate.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from enum import Enum

from .protocol import MSG_CLICK_REPORT, Packet


DEFAULT_MAX_ENTRIES = 1024
DEFAULT_MAX_PAYLOAD_BYTES = 1024 * 1024


@dataclass(frozen=True)
class PacketIdentity:
    """The packet identity fields used by the firmware duplicate cache.

    Transport age, ATT framing, and the optional original packet bytes are not
    identity: a replay after reconnect can legitimately carry different age or
    framing metadata.  Header flags are compared as part of the exact record
    fingerprint so a same-identity flag mutation is reported as a conflict.
    """

    msg_type: int
    src_id: int
    dst_id: int
    session_id: int
    seq: int


class PacketDisposition(str, Enum):
    NEW = "new"
    DUPLICATE = "duplicate"
    CONFLICT = "conflict"


@dataclass(frozen=True)
class PacketDeliveryDecision:
    disposition: PacketDisposition
    identity: PacketIdentity | None
    cached: bool

    @property
    def is_new(self) -> bool:
        return self.disposition is PacketDisposition.NEW

    @property
    def is_duplicate(self) -> bool:
        return self.disposition is PacketDisposition.DUPLICATE

    @property
    def is_conflict(self) -> bool:
        return self.disposition is PacketDisposition.CONFLICT


@dataclass(frozen=True)
class _CachedPacket:
    flags: int
    payload: bytes


class GatewayClickDeduplicator:
    """Bounded exact-duplicate suppression for GUI-session click delivery.

    Only ``MSG_CLICK_REPORT`` packets are cached.  Command and telemetry
    packets retain their existing model-specific duplicate handling.  Entries
    use LRU eviction and are retained across BLE disconnect/reconnect events;
    the caller owns the cache for the lifetime of one GUI process.
    """

    def __init__(
        self,
        *,
        max_entries: int = DEFAULT_MAX_ENTRIES,
        max_payload_bytes: int = DEFAULT_MAX_PAYLOAD_BYTES,
    ) -> None:
        if max_entries < 0:
            raise ValueError("max_entries must be non-negative")
        if max_payload_bytes < 0:
            raise ValueError("max_payload_bytes must be non-negative")
        self.max_entries = max_entries
        self.max_payload_bytes = max_payload_bytes
        self._entries: OrderedDict[PacketIdentity, _CachedPacket] = OrderedDict()
        self._cached_payload_bytes = 0

    @staticmethod
    def _identity(packet: Packet) -> PacketIdentity:
        return PacketIdentity(
            msg_type=packet.msg_type,
            src_id=packet.src_id,
            dst_id=packet.dst_id,
            session_id=packet.session_id,
            seq=packet.seq,
        )

    @property
    def size(self) -> int:
        return len(self._entries)

    @property
    def cached_payload_bytes(self) -> int:
        return self._cached_payload_bytes

    def clear(self) -> None:
        """Forget session history; normally called only at GUI process teardown."""

        self._entries.clear()
        self._cached_payload_bytes = 0

    def observe(self, packet: Packet) -> PacketDeliveryDecision:
        """Classify one packet without hiding same-identity mutations.

        Non-click packets are intentionally outside this cache and are always
        returned as ``NEW``.  A same-identity click with changed flags or bytes
        is a ``CONFLICT`` and remains visible to the caller; its original
        canonical payload stays in the cache for future exact retries.
        """

        if packet.msg_type != MSG_CLICK_REPORT:
            return PacketDeliveryDecision(PacketDisposition.NEW, None, False)

        identity = self._identity(packet)
        candidate = _CachedPacket(packet.flags, bytes(packet.payload))
        previous = self._entries.get(identity)
        if previous is not None:
            self._entries.move_to_end(identity)
            if previous == candidate:
                return PacketDeliveryDecision(PacketDisposition.DUPLICATE, identity, True)
            return PacketDeliveryDecision(PacketDisposition.CONFLICT, identity, True)

        if self.max_entries == 0 or len(candidate.payload) > self.max_payload_bytes:
            return PacketDeliveryDecision(PacketDisposition.NEW, identity, False)

        self._evict_for(candidate)
        self._entries[identity] = candidate
        self._cached_payload_bytes += len(candidate.payload)
        return PacketDeliveryDecision(PacketDisposition.NEW, identity, True)

    def _evict_for(self, candidate: _CachedPacket) -> None:
        while self._entries and (
            len(self._entries) >= self.max_entries
            or self._cached_payload_bytes + len(candidate.payload) > self.max_payload_bytes
        ):
            _, evicted = self._entries.popitem(last=False)
            self._cached_payload_bytes -= len(evicted.payload)


# Short aliases keep the intent discoverable for callers that use generic
# packet-delivery terminology while retaining one implementation and policy.
PacketDeduplicator = GatewayClickDeduplicator
DeliveryDeduplicationDecision = PacketDeliveryDecision
