"""Bounded in-process idempotence for gateway host delivery.

The gateway may deliver one reliable record again after a BLE reconnect or a
short-lived retry.  This cache keeps those at-least-once replays from applying
the same GUI/model mutation twice while this GUI process remains alive.  It is
deliberately RAM-only, bounded, and scoped to the gateway identity read from
GATT; a process restart starts a new host session.
"""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from enum import Enum

from .protocol import (
    FLAG_DIAGNOSTIC,
    FLAG_GATEWAY_ACK_REQUIRED,
    MSG_ANCHOR_HEARTBEAT,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_MESH_DATA,
    MSG_RESULT_BUNDLE,
    MSG_SELF_TEST_REPORT,
    MSG_SURVEY_DISCOVERY_REPORT,
    MSG_SURVEY_PAIR_RESULT,
    Packet,
)


DEFAULT_MAX_ENTRIES = 1024
DEFAULT_MAX_PAYLOAD_BYTES = 1024 * 1024

# These are the packet classes that the gateway treats as reliable host
# output. The ACK flag remains part of admission for packets originating in
# the mesh; gateway-local command events are already terminal host-visible
# records and intentionally have no gateway-ACK flag on the wire.
GATEWAY_ACK_REQUIRED_MESSAGE_TYPES = frozenset(
    {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_ANCHOR_HEARTBEAT,
        MSG_COMMAND_RESULT,
        MSG_RESULT_BUNDLE,
        MSG_SURVEY_DISCOVERY_REPORT,
        MSG_SURVEY_PAIR_RESULT,
    }
)


@dataclass(frozen=True)
class PacketIdentity:
    """Stable identity fields for one gateway host-delivery record.

    Transport age, ATT framing, and the optional original packet bytes are not
    identity: a replay after reconnect can legitimately carry different age or
    framing metadata.  Header flags are compared as part of the exact record
    fingerprint so a same-identity flag mutation is reported as a conflict.
    ``gateway_id`` prevents records from different gateways in one GUI process
    from sharing a cache entry.
    """

    msg_type: int
    src_id: int
    dst_id: int
    session_id: int
    seq: int
    gateway_id: int | None = None


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


def is_host_delivery_packet(packet: Packet) -> bool:
    """Return whether ``packet`` can replay a host-side semantic mutation.

    Mesh records enter this cache only when the protocol explicitly marks them
    as gateway-ACK-required.  Diagnostic mesh data must carry both the ACK and
    diagnostic flags.  Gateway command events are generated locally for the
    GUI and are reliable terminal telemetry even though their envelope flags
    are zero.  Best-effort status/diagnostic traffic therefore remains visible
    on every arrival and consumes no cache budget.
    """

    if packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
        return True
    if packet.msg_type in GATEWAY_ACK_REQUIRED_MESSAGE_TYPES:
        return (packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0
    if packet.msg_type == MSG_MESH_DATA:
        return (
            packet.flags & (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC)
        ) == (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC)
    return False


class GatewayPacketDeduplicator:
    """Bounded exact-duplicate suppression for one GUI host session.

    Entries use LRU eviction and are retained across BLE disconnect/reconnect
    events while the active gateway identity is unchanged.  Switching to a
    different gateway clears the old scope before accepting new records; no
    disk or firmware persistence is involved.
    """

    def __init__(
        self,
        *,
        gateway_id: int | None = None,
        max_entries: int = DEFAULT_MAX_ENTRIES,
        max_payload_bytes: int = DEFAULT_MAX_PAYLOAD_BYTES,
    ) -> None:
        if max_entries < 0:
            raise ValueError("max_entries must be non-negative")
        if max_payload_bytes < 0:
            raise ValueError("max_payload_bytes must be non-negative")
        self.max_entries = max_entries
        self.max_payload_bytes = max_payload_bytes
        self._gateway_id = self._validate_gateway_id(gateway_id)
        self._entries: OrderedDict[PacketIdentity, _CachedPacket] = OrderedDict()
        self._cached_payload_bytes = 0

    @staticmethod
    def _validate_gateway_id(gateway_id: int | None) -> int | None:
        if gateway_id is None:
            return None
        if (
            isinstance(gateway_id, bool)
            or not isinstance(gateway_id, int)
            or not 1 <= gateway_id <= 0xFFFFFFFFFFFFFFFF
        ):
            raise ValueError("gateway_id must be a non-zero 64-bit integer")
        return gateway_id

    @property
    def gateway_id(self) -> int | None:
        return self._gateway_id

    def set_gateway_id(self, gateway_id: int | None) -> None:
        """Select the active gateway, clearing entries when its scope changes."""

        gateway_id = self._validate_gateway_id(gateway_id)
        if gateway_id == self._gateway_id:
            return
        self._gateway_id = gateway_id
        self.clear()

    @staticmethod
    def _identity(
        packet: Packet,
        gateway_id: int | None = None,
    ) -> PacketIdentity:
        return PacketIdentity(
            msg_type=packet.msg_type,
            src_id=packet.src_id,
            dst_id=packet.dst_id,
            session_id=packet.session_id,
            seq=packet.seq,
            gateway_id=gateway_id,
        )

    @property
    def size(self) -> int:
        return len(self._entries)

    @property
    def cached_payload_bytes(self) -> int:
        return self._cached_payload_bytes

    def clear(self) -> None:
        """Forget in-process delivery history without touching disk or firmware."""

        self._entries.clear()
        self._cached_payload_bytes = 0

    def observe(
        self, packet: Packet, *, commit: bool = True
    ) -> PacketDeliveryDecision:
        """Classify one packet without hiding same-identity mutations.

        Best-effort packets are always ``NEW`` and are never cached.  A
        same-identity reliable packet with changed flags or payload is a
        ``CONFLICT``; its canonical entry remains authoritative for future
        exact retries.  ``commit=False`` stages a new reliable decision for a
        caller that must complete its semantic/model mutation before making the
        RAM entry authoritative.
        """

        if not is_host_delivery_packet(packet):
            return PacketDeliveryDecision(PacketDisposition.NEW, None, False)

        identity = self._identity(packet, self._gateway_id)
        candidate = _CachedPacket(packet.flags, bytes(packet.payload))
        previous = self._entries.get(identity)
        if previous is not None:
            self._entries.move_to_end(identity)
            if previous == candidate:
                return PacketDeliveryDecision(PacketDisposition.DUPLICATE, identity, True)
            return PacketDeliveryDecision(PacketDisposition.CONFLICT, identity, True)

        if self.max_entries == 0 or len(candidate.payload) > self.max_payload_bytes:
            return PacketDeliveryDecision(PacketDisposition.NEW, identity, False)

        if commit:
            self._commit_candidate(identity, candidate)
        return PacketDeliveryDecision(PacketDisposition.NEW, identity, True)

    def commit(self, packet: Packet, decision: PacketDeliveryDecision) -> bool:
        """Make a staged NEW decision authoritative after semantic apply.

        Returns ``False`` when the gateway scope changed between ``observe``
        and ``commit``.  In that case no receipt may be sent because the
        decision no longer belongs to the active in-process RAM scope.
        """
        if (
            decision.disposition is not PacketDisposition.NEW
            or not decision.cached
            or not is_host_delivery_packet(packet)
        ):
            return True
        identity = self._identity(packet, self._gateway_id)
        if decision.identity != identity:
            return False
        candidate = _CachedPacket(packet.flags, bytes(packet.payload))
        previous = self._entries.get(identity)
        if previous is not None:
            return previous == candidate
        self._commit_candidate(identity, candidate)
        return True

    def _commit_candidate(
        self, identity: PacketIdentity, candidate: _CachedPacket
    ) -> None:
        self._evict_for(candidate)
        self._entries[identity] = candidate
        self._cached_payload_bytes += len(candidate.payload)

    def _evict_for(self, candidate: _CachedPacket) -> None:
        while self._entries and (
            len(self._entries) >= self.max_entries
            or self._cached_payload_bytes + len(candidate.payload) > self.max_payload_bytes
        ):
            _, evicted = self._entries.popitem(last=False)
            self._cached_payload_bytes -= len(evicted.payload)
