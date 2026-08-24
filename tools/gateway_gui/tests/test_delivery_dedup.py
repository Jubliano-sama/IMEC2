from __future__ import annotations

from dataclasses import replace
import unittest

from tools.gateway_gui.delivery_dedup import (
    GatewayPacketDeduplicator,
    PacketDisposition,
)
from tools.gateway_gui.protocol import (
    FLAG_DIAGNOSTIC,
    FLAG_GATEWAY_ACK_REQUIRED,
    GATEWAY_COMMAND_EVENT_FLAG_DUPLICATE,
    GATEWAY_COMMAND_EVENT_FLAG_REPLAY,
    GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT,
    MSG_ANCHOR_HEARTBEAT,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_MESH_DATA,
    MSG_RESULT_BUNDLE,
    MSG_SELF_TEST_REPORT,
    Packet,
    click_report_session_id,
    parse_stream_record,
    parse_tlvs,
)
from tools.gateway_gui.tests.test_protocol import (
    gateway_assignment_event_payload,
    stream_record,
)


def click_packet(
    payload: bytes = b"click",
    *,
    src_id: int = 0x1111,
    dst_id: int = 0x2222,
    session_id: int = 7,
    seq: int = 3,
    flags: int = 0x24,
    age_ms: int = 0,
):
    return Packet(
        transport="test",
        raw_transport=b"",
        raw_packet=None,
        msg_type=MSG_CLICK_REPORT,
        flags=flags,
        src_id=src_id,
        dst_id=dst_id,
        session_id=session_id,
        seq=seq,
        ttl=4,
        age_ms=age_ms,
        age_kind="test",
        payload=payload,
        tlvs=(),
    )


def host_packet(
    msg_type: int,
    payload: bytes = b"host-record",
    *,
    flags: int = FLAG_GATEWAY_ACK_REQUIRED,
    src_id: int = 0x1111,
    dst_id: int = 0x2222,
    session_id: int = 7,
    seq: int = 3,
):
    return replace(
        click_packet(
            payload=payload,
            src_id=src_id,
            dst_id=dst_id,
            session_id=session_id,
            seq=seq,
            flags=flags,
        ),
        msg_type=msg_type,
    )


def self_test_packet(
    *,
    clicker_id: int = 0x1111,
    gateway_id: int = 0x2222,
    event_seq: int = 7,
    failure: int = 0,
) -> Packet:
    payload = b"".join(
        (
            bytes((0x0B, 8)) + clicker_id.to_bytes(8, "little"),
            bytes((0x06, 4)) + event_seq.to_bytes(4, "little"),
            bytes((0x04, 2)) + failure.to_bytes(2, "little"),
            bytes((0x02, 2)) + (3000).to_bytes(2, "little"),
        )
    )
    seq = event_seq & 0xFFFF or 1
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=b"",
        raw_packet=None,
        msg_type=MSG_SELF_TEST_REPORT,
        flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        src_id=clicker_id,
        dst_id=gateway_id,
        session_id=event_seq,
        seq=seq,
        ttl=None,
        age_ms=0,
        age_kind="gateway_queue_age_ms",
        payload=payload,
        tlvs=parse_tlvs(payload),
    )


def command_event_packet(
    *,
    event_sequence: int = 0x10203040,
    stage: int = 6,
    anchor_id: int = 0x5555666677778888,
    discovery_slot: int = 4,
    attempt: int = 0,
    replay: bool = False,
    event_flags: int | None = None,
    previous_hop_id: int = 0,
    lost_event_count: int = 0,
    hop_count: int = 0,
    status: int = 0,
    progress_count: int = 1,
    total_count: int = 1,
    success_count: int = 1,
    failure_count: int = 0,
    packet_flags: int = FLAG_GATEWAY_ACK_REQUIRED,
) -> Packet:
    gateway_id = 0x9999AAAABBBBCCCC
    payload = bytearray(
        gateway_assignment_event_payload(
            event_sequence=event_sequence,
            stage=stage,
            anchor_id=anchor_id,
            discovery_slot=discovery_slot,
            progress_count=progress_count,
            total_count=total_count,
            success_count=success_count,
            failure_count=failure_count,
        )
    )
    payload[4] = (
        event_flags
        if event_flags is not None
        else (GATEWAY_COMMAND_EVENT_FLAG_REPLAY if replay else 0)
    )
    payload[5] = attempt
    payload[6] = status
    payload[56:64] = previous_hop_id.to_bytes(8, "little")
    payload[74:76] = lost_event_count.to_bytes(2, "little")
    payload[76] = hop_count
    return parse_stream_record(
        stream_record(
            bytes(payload),
            msg_type=MSG_GATEWAY_COMMAND_EVENT,
            packet_flags=packet_flags,
            packet_src_id=gateway_id,
            packet_dst_id=gateway_id,
            packet_session_id=event_sequence,
            packet_seq=event_sequence & 0xFFFF,
        )
    )


class GatewayPacketDeduplicatorTests(unittest.TestCase):
    def test_clicker_identity_namespaces_same_anchor_event_and_fragment(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        event_seq = 7
        first_session = click_report_session_id(0xAAAABBBBCCCCDDDD, event_seq)
        second_session = click_report_session_id(0xAAAABBBBCCCCDDDE, event_seq)
        first = click_packet(
            payload=b"first-clicker",
            src_id=0x5555666677778888,
            session_id=first_session,
            seq=3,
        )
        second = click_packet(
            payload=b"second-clicker",
            src_id=first.src_id,
            session_id=second_session,
            seq=first.seq,
        )

        self.assertNotEqual(first_session, second_session)
        self.assertEqual(cache.observe(first).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(second).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(first).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.observe(second).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 2)

    def test_exact_replay_is_suppressed_even_when_transport_age_changes(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        original = click_packet(age_ms=10)
        replay = replace(original, transport="gateway-stream-v1", raw_transport=b"replayed", age_ms=900)

        first = cache.observe(original)
        second = cache.observe(replay)

        self.assertEqual(first.disposition, PacketDisposition.NEW)
        self.assertTrue(first.cached)
        self.assertEqual(second.disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 1)

    def test_reconnect_does_not_clear_session_history(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        packet = click_packet()

        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        # A Bleak reconnect recreates framing state, not the GUI process. A
        # replay from the newly connected stream remains an exact duplicate.
        reconnected = replace(
            packet,
            transport="gateway-stream-v1",
            raw_transport=b"new-connection-record",
            age_ms=125,
        )
        self.assertEqual(cache.observe(reconnected).disposition, PacketDisposition.DUPLICATE)

    def test_normal_click_and_both_extended_cir_fragments_are_idempotent(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=8, max_payload_bytes=4096)
        normal = click_packet(payload=b"normal", seq=10)
        cir_first = click_packet(payload=b"a" * 881, seq=11, flags=0x14)
        cir_second = click_packet(payload=b"b" * 271, seq=12, flags=0x14)

        for packet in (normal, cir_first, cir_second):
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        for packet in (normal, cir_first, cir_second):
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 3)

    def test_same_identity_mutation_is_visible_as_conflict_and_does_not_replace_canonical(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        original = click_packet(payload=b"canonical")
        mutated = click_packet(payload=b"mutated")

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(mutated).disposition, PacketDisposition.CONFLICT)
        self.assertEqual(cache.observe(original).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 1)

    def test_header_flag_mutation_is_not_hidden_as_an_exact_duplicate(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        original = click_packet(flags=0x24)
        mutated = click_packet(flags=0x04)

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(mutated).disposition, PacketDisposition.CONFLICT)

    def test_distinct_identity_is_a_new_record(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        original = click_packet()

        for changed in (
            click_packet(seq=4),
            click_packet(session_id=8),
            click_packet(src_id=0x3333),
            click_packet(dst_id=0x4444),
        ):
            self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
            self.assertEqual(cache.observe(changed).disposition, PacketDisposition.NEW)
            cache.clear()

    def test_non_click_packets_are_not_claimed_by_click_cache(self) -> None:
        packet = Packet(
            transport="test",
            raw_transport=b"",
            raw_packet=None,
            msg_type=MSG_MESH_DATA,
            flags=0,
            src_id=1,
            dst_id=2,
            session_id=7,
            seq=3,
            ttl=4,
            age_ms=0,
            age_kind="test",
            payload=b"mesh",
            tlvs=(),
        )
        cache = GatewayPacketDeduplicator(max_entries=1)

        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.size, 0)

    def test_entry_pressure_evicts_oldest_lru_record(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=2)
        first = click_packet(seq=1)
        second = click_packet(seq=2)
        third = click_packet(seq=3)

        cache.observe(first)
        cache.observe(second)
        # Touch the first entry so the second entry is the bounded eviction.
        self.assertEqual(cache.observe(first).disposition, PacketDisposition.DUPLICATE)
        cache.observe(third)

        self.assertEqual(cache.observe(first).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.observe(second).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.size, 2)

    def test_payload_pressure_evicts_until_exact_byte_budget_fits(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=8, max_payload_bytes=10)
        first = click_packet(payload=b"123456")
        second = click_packet(payload=b"abcd", seq=4)
        third = click_packet(payload=b"wxyz", seq=5)

        cache.observe(first)
        cache.observe(second)

        self.assertEqual(cache.cached_payload_bytes, 10)
        self.assertEqual(cache.observe(first).disposition, PacketDisposition.DUPLICATE)
        cache.observe(third)
        self.assertEqual(cache.cached_payload_bytes, 10)
        self.assertEqual(cache.observe(first).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.observe(second).disposition, PacketDisposition.NEW)

    def test_oversize_record_is_delivered_but_not_cached(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4, max_payload_bytes=3)
        packet = click_packet(payload=b"too-large")

        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertFalse(cache.observe(packet).cached)
        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.size, 0)

    def test_reliable_host_classes_are_idempotent(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0xAAA, max_entries=16)
        packets = (
            host_packet(MSG_CLICK_REPORT),
            self_test_packet(),
            host_packet(MSG_ANCHOR_HEARTBEAT, seq=5),
            host_packet(MSG_COMMAND_RESULT, seq=6),
            host_packet(MSG_RESULT_BUNDLE, seq=7),
            host_packet(
                MSG_MESH_DATA,
                flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                seq=10,
            ),
            command_event_packet(),
        )

        for packet in packets:
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        for packet in packets:
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, len(packets))

    def test_best_effort_heartbeat_remains_visible_without_cache_custody(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0xAAA, max_entries=16)
        heartbeat = host_packet(MSG_ANCHOR_HEARTBEAT, flags=0, seq=11)

        self.assertEqual(cache.observe(heartbeat).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(heartbeat).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.size, 0)

    def test_command_event_replay_uses_semantic_identity_not_stream_sequence(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=4)
        original = command_event_packet(event_sequence=0x10203040)
        replay = command_event_packet(
            event_sequence=0x50607080,
            attempt=0,
            replay=True,
            previous_hop_id=0x44,
            lost_event_count=9,
            hop_count=3,
        )

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(replay).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 1)

    def test_retained_generic_command_event_uses_exact_stream_identity(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=4)
        queued = command_event_packet(
            event_sequence=0x10203040,
            stage=1,
            anchor_id=0,
            discovery_slot=0xFF,
            progress_count=0,
            total_count=0,
            success_count=0,
            failure_count=0,
        )

        self.assertEqual(cache.observe(queued).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(queued).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, 1)

    def test_retained_generic_delivery_flags_are_transport_only(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=4)
        common = dict(
            event_sequence=0x10203040,
            stage=1,
            anchor_id=0,
            discovery_slot=0xFF,
            progress_count=0,
            total_count=0,
            success_count=0,
            failure_count=0,
        )
        original = command_event_packet(**common)
        replay = command_event_packet(
            **common,
            event_flags=(
                GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT
                | GATEWAY_COMMAND_EVENT_FLAG_REPLAY
                | GATEWAY_COMMAND_EVENT_FLAG_DUPLICATE
            ),
        )
        semantic_mutation = command_event_packet(**common, status=1)

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(replay).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(
            cache.observe(semantic_mutation).disposition,
            PacketDisposition.CONFLICT,
        )
        self.assertEqual(cache.size, 1)

    def test_full_assignment_publisher_replay_batch_is_semantically_idempotent(
        self,
    ) -> None:
        cache = GatewayPacketDeduplicator(
            gateway_id=0x9999AAAABBBBCCCC, max_entries=8
        )

        def publisher_packet(
            stage: int,
            event_sequence: int,
            *,
            replay: bool,
            anchor_id: int = 0,
            discovery_slot: int = 0xFF,
        ) -> Packet:
            terminal = stage == 12
            mapping = stage == 6
            payload = bytearray(
                gateway_assignment_event_payload(
                    event_sequence=event_sequence,
                    stage=stage,
                    flags=(0x01 if terminal else 0x00)
                    | (0x04 if replay else 0x00),
                    anchor_id=anchor_id,
                    discovery_slot=discovery_slot,
                    progress_count=1 if mapping else 2,
                    total_count=2,
                    success_count=1 if mapping else 2,
                    failure_count=0,
                )
            )
            payload[5] = 7 if stage in (8, 12) else 0
            return parse_stream_record(
                stream_record(
                    bytes(payload),
                    msg_type=MSG_GATEWAY_COMMAND_EVENT,
                    packet_flags=FLAG_GATEWAY_ACK_REQUIRED,
                    packet_src_id=0x9999AAAABBBBCCCC,
                    packet_dst_id=0x9999AAAABBBBCCCC,
                    packet_session_id=event_sequence,
                    packet_seq=event_sequence & 0xFFFF,
                )
            )

        live = (
            publisher_packet(
                6, 0x10203041,
                replay=False,
                anchor_id=0x5100000000000001,
                discovery_slot=0,
            ),
            publisher_packet(
                6, 0x10203042,
                replay=False,
                anchor_id=0x5100000000000002,
                discovery_slot=4,
            ),
            publisher_packet(7, 0x10203043, replay=False),
            publisher_packet(8, 0x10203044, replay=False),
            publisher_packet(12, 0x10203045, replay=False),
        )
        replay = (
            publisher_packet(
                6, 0x50607041,
                replay=True,
                anchor_id=0x5100000000000001,
                discovery_slot=0,
            ),
            publisher_packet(
                6, 0x50607042,
                replay=True,
                anchor_id=0x5100000000000002,
                discovery_slot=4,
            ),
            publisher_packet(7, 0x50607043, replay=True),
            publisher_packet(8, 0x50607044, replay=True),
            publisher_packet(12, 0x50607045, replay=True),
        )

        for packet in live:
            self.assertEqual(
                cache.observe(packet).disposition, PacketDisposition.NEW
            )
        for packet in replay:
            self.assertEqual(
                cache.observe(packet).disposition, PacketDisposition.DUPLICATE
            )
        self.assertEqual(cache.size, len(live))

    def test_command_event_stage_or_mapping_change_is_new_and_semantic_mutation_conflicts(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=8)
        original = command_event_packet()
        different_mapping = command_event_packet(
            event_sequence=0x20304050,
            anchor_id=0x5555666677779999,
            discovery_slot=5,
        )
        conflicting_status = command_event_packet(
            event_sequence=0x30405060,
            status=5,
        )

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(
            cache.observe(different_mapping).disposition, PacketDisposition.NEW
        )
        self.assertEqual(
            cache.observe(conflicting_status).disposition,
            PacketDisposition.CONFLICT,
        )
        self.assertEqual(cache.observe(original).disposition, PacketDisposition.DUPLICATE)

    def test_live_command_retry_progress_is_best_effort_and_not_cached(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=8)
        first_attempt = command_event_packet(
            event_sequence=0x10203040,
            stage=4,
            attempt=1,
            progress_count=1,
            total_count=4,
            success_count=0,
            failure_count=0,
            packet_flags=0,
        )
        second_attempt = command_event_packet(
            event_sequence=0x20304050,
            stage=4,
            attempt=2,
            progress_count=2,
            total_count=4,
            success_count=0,
            failure_count=0,
            packet_flags=0,
        )

        self.assertEqual(cache.observe(first_attempt).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(second_attempt).disposition, PacketDisposition.NEW)
        self.assertEqual(
            cache.observe(second_attempt).disposition,
            PacketDisposition.NEW,
        )
        self.assertEqual(cache.size, 0)

    def test_malformed_command_event_is_conflict_and_never_cached(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0x9999AAAABBBBCCCC, max_entries=4)
        malformed = host_packet(
            MSG_GATEWAY_COMMAND_EVENT,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            src_id=0x9999AAAABBBBCCCC,
            dst_id=0x9999AAAABBBBCCCC,
        )

        decision = cache.observe(malformed)
        self.assertEqual(decision.disposition, PacketDisposition.CONFLICT)
        self.assertIsNone(decision.identity)
        self.assertFalse(decision.cached)
        self.assertEqual(cache.size, 0)

    def test_best_effort_records_are_always_visible_and_uncached(self) -> None:
        cache = GatewayPacketDeduplicator(max_entries=4)
        best_effort = (
            host_packet(MSG_MESH_DATA, flags=0),
            host_packet(MSG_MESH_DATA, flags=FLAG_DIAGNOSTIC, seq=4),
        )

        for packet in best_effort:
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.size, 0)

    def test_gateway_scope_survives_reconnect_but_changes_between_gateways(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0xAAA, max_entries=4)
        packet = host_packet(MSG_COMMAND_RESULT)

        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        cache.set_gateway_id(0xAAA)  # A reconnect to the same gateway preserves RAM history.
        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.DUPLICATE)
        cache.set_gateway_id(0xBBB)
        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(packet).identity.gateway_id, 0xBBB)  # type: ignore[union-attr]

    def test_same_identity_mutation_conflicts_for_non_click_records(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0xAAA, max_entries=4)
        original = host_packet(MSG_COMMAND_RESULT, payload=b"canonical")
        mutated = replace(original, payload=b"mutated")

        self.assertEqual(cache.observe(original).disposition, PacketDisposition.NEW)
        self.assertEqual(cache.observe(mutated).disposition, PacketDisposition.CONFLICT)
        self.assertEqual(cache.observe(original).disposition, PacketDisposition.DUPLICATE)

    def test_staged_new_is_only_duplicate_after_explicit_commit(self) -> None:
        cache = GatewayPacketDeduplicator(gateway_id=0xAAA, max_entries=4)
        packet = host_packet(MSG_COMMAND_RESULT)

        staged = cache.observe(packet, commit=False)
        self.assertEqual(staged.disposition, PacketDisposition.NEW)
        self.assertTrue(staged.cached)
        self.assertEqual(cache.size, 0)

        self.assertTrue(cache.commit(packet, staged))
        self.assertEqual(cache.size, 1)
        self.assertEqual(cache.observe(packet).disposition, PacketDisposition.DUPLICATE)


if __name__ == "__main__":
    unittest.main()
