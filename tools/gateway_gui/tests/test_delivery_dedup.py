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
    click_report_session_id,
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
            host_packet(MSG_SELF_TEST_REPORT, seq=4),
            host_packet(MSG_ANCHOR_HEARTBEAT, seq=5),
            host_packet(MSG_COMMAND_RESULT, seq=6),
            host_packet(MSG_RESULT_BUNDLE, seq=7),
            host_packet(MSG_SURVEY_DISCOVERY_REPORT, seq=8),
            host_packet(MSG_SURVEY_PAIR_RESULT, seq=9),
            host_packet(
                MSG_MESH_DATA,
                flags=FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
                seq=10,
            ),
            host_packet(MSG_GATEWAY_COMMAND_EVENT, flags=0, seq=11),
        )

        for packet in packets:
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.NEW)
        for packet in packets:
            self.assertEqual(cache.observe(packet).disposition, PacketDisposition.DUPLICATE)
        self.assertEqual(cache.size, len(packets))

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
