from __future__ import annotations

import unittest

from tools.gateway_gui.protocol import (
    CMD_SURVEY_CANCEL,
    CMD_SURVEY_GET_STATUS,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    FLAG_GATEWAY_ACK_REQUIRED,
    MSG_SURVEY_EVENT,
    SURVEY_EVENT_HEADER_WIRE_LEN,
    SURVEY_EVENT_SIGNALS,
    SURVEY_SIGNAL_EVENT_HEADER_WIRE_LEN,
    SURVEY_PROTOCOL_VERSION,
    TLV_COMMAND_ID,
    TLV_SURVEY_ASSIGNMENT_IDENTITY,
    TLV_SURVEY_GENERATION,
    TLV_SURVEY_PLAN,
    DecodeError,
    Packet,
    SurveyAssignmentIdentity,
    SurveyEvent,
    SurveyNeighborReport,
    build_survey_cancel_command,
    build_survey_get_status_command,
    build_survey_plan_command,
    build_survey_start_command,
    decode_survey_event,
    select_closest_survey_pairs,
    select_degree_balanced_survey_pairs,
    select_survey_pairs,
    survey_pair_rigidity_rank,
)
from tools.gateway_gui.delivery_dedup import (
    GatewayPacketDeduplicator,
    PacketDisposition,
    is_host_delivery_packet,
)


GATEWAY_ID = 0x1111222233334444
HOST_ID = 0xAAAABBBBCCCCDDDD


def assignment() -> SurveyAssignmentIdentity:
    return SurveyAssignmentIdentity(7, 8, bytes((0x5A,)) * 32, 8, 3)


def event_payload(
    *,
    kind: int,
    body: bytes = b"",
    graph_count: int = 0,
    result_count: int = 0,
    pair_count: int = 0,
    wave_count: int = 0,
    skipped_count: int = 0,
    occupied_mask: int = 0,
    received_mask: int = 0,
) -> bytes:
    raw = bytearray(SURVEY_EVENT_HEADER_WIRE_LEN)
    raw[0] = SURVEY_PROTOCOL_VERSION
    raw[1] = kind
    raw[2] = 1
    raw[3] = graph_count
    raw[4:8] = (9).to_bytes(4, "little")
    raw[8:10] = (2).to_bytes(2, "little")
    raw[10] = result_count
    raw[11] = pair_count
    raw[12] = wave_count
    raw[13] = skipped_count
    raw[14:56] = assignment().encode()
    raw[56:64] = occupied_mask.to_bytes(8, "little")
    raw[64:72] = received_mask.to_bytes(8, "little")
    raw.extend(body)
    return bytes(raw)


def packet(
    payload: bytes, *, flags: int = 0, session_id: int = 9, seq: int = 1
) -> Packet:
    return Packet(
        transport="gateway-stream-v1",
        raw_transport=b"x",
        raw_packet=None,
        msg_type=MSG_SURVEY_EVENT,
        flags=flags,
        src_id=GATEWAY_ID,
        dst_id=GATEWAY_ID,
        session_id=session_id,
        seq=seq,
        ttl=None,
        age_ms=0,
        age_kind="gateway_queue_age_ms",
        payload=payload,
        tlvs=(),
    )


def complete_neighbor_event(count: int = 6) -> SurveyEvent:
    slots = frozenset(range(count))
    return SurveyEvent(
        kind=1,
        status=1,
        generation=9,
        assignment=assignment(),
        partial_reasons=0,
        occupied_slots=slots,
        neighbor_reports=tuple(
            SurveyNeighborReport(slot, slots - {slot})
            for slot in sorted(slots)
        ),
    )


class SurveyCommandTests(unittest.TestCase):
    def test_compact_signal_event_decodes_four_bit_rsl(self) -> None:
        raw = bytearray(SURVEY_SIGNAL_EVENT_HEADER_WIRE_LEN)
        raw[0] = SURVEY_PROTOCOL_VERSION
        raw[1] = SURVEY_EVENT_SIGNALS
        raw[2] = 0
        raw[3] = 2
        raw[4:8] = (9).to_bytes(4, "little")
        raw[8:50] = assignment().encode()
        raw.extend(bytes((50, 0x07, 0, 0, 0, 0, 0, 0)))
        raw.extend(bytes((51, 0x5A, 0, 0, 0, 0, 0, 0)))

        event = decode_survey_event(bytes(raw))

        self.assertEqual(
            tuple(
                (item.observer_slot, item.target_slot, item.level, item.rsl_dbm)
                for item in event.signal_measurements
            ),
            ((1, 0, 7, -75), (2, 0, 10, -60), (2, 1, 5, -85)),
        )

    def test_assignment_identity_covers_every_mesh_hop(self) -> None:
        depth_eight = SurveyAssignmentIdentity(
            7, 8, bytes((0x5A,)) * 32, 8, 8
        )

        self.assertEqual(
            SurveyAssignmentIdentity.decode(depth_eight.encode()),
            depth_eight,
        )
        with self.assertRaisesRegex(ValueError, "1..8"):
            SurveyAssignmentIdentity(7, 8, bytes((0x5A,)) * 32, 8, 9)

    def test_start_cancel_and_get_status_are_gateway_local_commands(self) -> None:
        start = build_survey_start_command(
            host_id=HOST_ID, gateway_id=GATEWAY_ID, session_id=1, seq=1
        )
        cancel = build_survey_cancel_command(
            host_id=HOST_ID,
            gateway_id=GATEWAY_ID,
            session_id=2,
            seq=2,
            generation=9,
        )
        status = build_survey_get_status_command(
            host_id=HOST_ID,
            gateway_id=GATEWAY_ID,
            session_id=3,
            seq=3,
            generation=9,
        )
        self.assertEqual(start.packet.value(TLV_COMMAND_ID), CMD_SURVEY_START)
        self.assertEqual(cancel.packet.value(TLV_COMMAND_ID), CMD_SURVEY_CANCEL)
        self.assertEqual(status.packet.value(TLV_COMMAND_ID), CMD_SURVEY_GET_STATUS)
        self.assertEqual(cancel.packet.value(TLV_SURVEY_GENERATION), 9)

    def test_plan_binds_generation_assignment_and_gui_order(self) -> None:
        command = build_survey_plan_command(
            host_id=HOST_ID,
            gateway_id=GATEWAY_ID,
            session_id=4,
            seq=4,
            generation=9,
            assignment=assignment(),
            pairs=((5, 6), (0, 1), (2, 3)),
        )
        self.assertEqual(command.command_id, CMD_SURVEY_PLAN)
        self.assertEqual(command.packet.value(TLV_SURVEY_GENERATION), 9)
        self.assertEqual(
            command.packet.raw_value(TLV_SURVEY_ASSIGNMENT_IDENTITY),
            assignment().encode(),
        )
        self.assertEqual(command.packet.raw_value(TLV_SURVEY_PLAN), bytes((5, 6, 0, 1, 2, 3)))

    def test_empty_followup_plan_is_a_valid_explicit_plan(self) -> None:
        command = build_survey_plan_command(
            host_id=HOST_ID,
            gateway_id=GATEWAY_ID,
            session_id=5,
            seq=5,
            generation=9,
            assignment=assignment(),
            pairs=(),
        )

        self.assertEqual(command.command_id, CMD_SURVEY_PLAN)
        self.assertIsNone(command.packet.raw_value(TLV_SURVEY_PLAN))


class SurveyEventTests(unittest.TestCase):
    def test_reliable_event_is_validated_cached_and_deduplicated(self) -> None:
        event = packet(
            event_payload(kind=4),
            flags=FLAG_GATEWAY_ACK_REQUIRED,
        )
        cache = GatewayPacketDeduplicator(gateway_id=GATEWAY_ID)

        self.assertTrue(is_host_delivery_packet(event))
        self.assertEqual(cache.observe(event).disposition, PacketDisposition.NEW)
        self.assertEqual(
            cache.observe(event).disposition, PacketDisposition.DUPLICATE
        )
        stale_envelope = packet(
            event.payload,
            flags=FLAG_GATEWAY_ACK_REQUIRED,
            session_id=10,
        )
        self.assertEqual(
            cache.observe(stale_envelope).disposition,
            PacketDisposition.CONFLICT,
        )

    def test_neighbor_graph_decodes_and_rigidity_selector_is_deterministic(self) -> None:
        reports = {
            0: {1, 2, 3, 4, 5},
            1: {0, 2, 3},
            2: {0, 1, 4},
            3: {0, 1, 5},
            4: {0, 2, 5},
            5: {0, 3, 4},
        }
        body = bytearray()
        for own_slot, heard in reports.items():
            bitmap = bytearray(7)
            for slot in heard:
                bitmap[slot // 8] |= 1 << (slot % 8)
            body.extend((own_slot,))
            body.extend(bitmap)
        event = decode_survey_event(
            packet(
                event_payload(
                    kind=1,
                    body=bytes(body),
                    graph_count=6,
                    occupied_mask=0x3F,
                    received_mask=0x3F,
                )
            )
        )
        selected = select_survey_pairs(event)
        self.assertEqual(selected, select_survey_pairs(event))
        degree = {slot: 0 for slot in range(6)}
        for first, second in selected:
            self.assertIn(second, reports[first])
            self.assertIn(first, reports[second])
            degree[first] += 1
            degree[second] += 1
        self.assertLessEqual(max(degree.values()), 4)
        self.assertIn((0, 1), selected)

    def test_rigidity_selector_reaches_full_rank_where_degree_balance_does_not(self) -> None:
        reports = {
            0: {1, 2, 3},
            1: {0, 2, 3},
            2: {0, 1, 3, 5},
            3: {0, 1, 2, 4, 5},
            4: {3, 5},
            5: {2, 3, 4},
        }
        body = bytearray()
        for own_slot, heard in reports.items():
            bitmap = bytearray(7)
            for slot in heard:
                bitmap[slot // 8] |= 1 << (slot % 8)
            body.extend((own_slot,))
            body.extend(bitmap)
        event = decode_survey_event(
            event_payload(
                kind=1,
                body=bytes(body),
                graph_count=6,
                occupied_mask=0x3F,
                received_mask=0x3F,
            )
        )

        legacy = select_degree_balanced_survey_pairs(event)
        selected = select_survey_pairs(event)

        self.assertEqual(survey_pair_rigidity_rank(legacy, range(6)), 8)
        self.assertEqual(survey_pair_rigidity_rank(selected, range(6)), 9)
        self.assertEqual(selected, select_survey_pairs(event))
        selected_neighbors = {slot: set() for slot in range(6)}
        for first, second in selected:
            self.assertIn(second, reports[first])
            self.assertIn(first, reports[second])
            selected_neighbors[first].add(second)
            selected_neighbors[second].add(first)
        self.assertLessEqual(max(map(len, selected_neighbors.values())), 4)
        # The explicit walk avoids accepting a full-rank but disconnected plan.
        reached = {0}
        frontier = [0]
        while frontier:
            slot = frontier.pop()
            new_slots = selected_neighbors[slot] - reached
            reached.update(new_slots)
            frontier.extend(new_slots)
        self.assertEqual(reached, set(range(6)))

    def test_additional_selector_excludes_every_previously_attempted_edge(self) -> None:
        event = complete_neighbor_event()
        excluded = ((0, 1), (0, 2), (1, 2), (4, 5))

        selected = select_survey_pairs(event, excluded_pairs=excluded)

        self.assertTrue(selected)
        self.assertTrue(set(selected).isdisjoint(excluded))
        degree = {slot: 0 for slot in event.occupied_slots}
        for first, second in selected:
            degree[first] += 1
            degree[second] += 1
        self.assertLessEqual(max(degree.values()), 4)

    def test_repeated_ten_anchor_plans_exhaust_all_mutual_pairs(self) -> None:
        event = complete_neighbor_event(10)
        attempted: set[tuple[int, int]] = set()
        pass_sizes: list[int] = []

        while True:
            selected = select_survey_pairs(
                event,
                excluded_pairs=tuple(sorted(attempted)),
            )
            if not selected:
                break
            self.assertTrue(set(selected).isdisjoint(attempted))
            degree = {slot: 0 for slot in event.occupied_slots}
            for first, second in selected:
                degree[first] += 1
                degree[second] += 1
            self.assertLessEqual(max(degree.values()), 4)
            attempted.update(selected)
            pass_sizes.append(len(selected))

        self.assertEqual(len(attempted), 45)
        self.assertGreater(len(pass_sizes), 1)
        self.assertLessEqual(max(pass_sizes), 20)

    def test_closest_selector_is_deterministic_connected_and_degree_four(self) -> None:
        event = complete_neighbor_event()
        positions = {
            0: (0.0, 0.0),
            1: (1.0, 0.0),
            2: (2.0, 0.0),
            3: (0.0, 1.0),
            4: (1.0, 1.0),
            5: (2.0, 1.0),
        }

        selected = select_closest_survey_pairs(event, positions)

        self.assertEqual(selected, select_closest_survey_pairs(event, positions))
        self.assertEqual(survey_pair_rigidity_rank(selected, range(6)), 9)
        neighbors = {slot: set() for slot in event.occupied_slots}
        for first, second in selected:
            neighbors[first].add(second)
            neighbors[second].add(first)
        self.assertLessEqual(max(map(len, neighbors.values())), 4)
        reached = {0}
        frontier = [0]
        while frontier:
            new_slots = neighbors[frontier.pop()] - reached
            reached.update(new_slots)
            frontier.extend(new_slots)
        self.assertEqual(reached, set(event.occupied_slots))

    def test_one_way_neighbor_is_retained_but_never_selected(self) -> None:
        body = bytes((0, 0b10, 0, 0, 0, 0, 0, 0)) + bytes((1, 0, 0, 0, 0, 0, 0, 0))
        event = decode_survey_event(
            event_payload(
                kind=1,
                body=body,
                graph_count=2,
                occupied_mask=3,
                received_mask=3,
            )
        )
        self.assertEqual(select_survey_pairs(event), ())
        self.assertEqual(event.neighbor_reports[0].heard_slots, frozenset({1}))

    def test_range_result_threshold_and_zero_result_decode(self) -> None:
        usable = bytes((0, 3, 1, 0)) + (1234).to_bytes(4, "little", signed=True)
        zero = bytes((1, 0, 2, 0)) + (-(1 << 31)).to_bytes(4, "little", signed=True)
        event = decode_survey_event(
            event_payload(kind=4, body=usable + zero, result_count=2)
        )
        self.assertTrue(event.range_results[0].usable)
        self.assertFalse(event.range_results[1].usable)
        self.assertIsNone(event.range_results[1].median_mm)

    def test_corrupt_graph_owner_is_rejected(self) -> None:
        body = bytes((0, 0b1, 0, 0, 0, 0, 0, 0))
        with self.assertRaises(DecodeError):
            decode_survey_event(
                event_payload(
                    kind=1,
                    body=body,
                    graph_count=1,
                    occupied_mask=1,
                    received_mask=1,
                )
            )


if __name__ == "__main__":
    unittest.main()
