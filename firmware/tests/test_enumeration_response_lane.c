#include "enumeration_response_lane.h"
#include "mesh.h"
#include "protocol.h"

#include <assert.h>
#include <stdint.h>

_Static_assert(MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS <
                   ENUMERATION_RESPONSE_DEPTH_MS,
               "one bounded CLAIM relay hop must fit inside one depth band");

static void test_fixed_schedule_is_shallowest_first_with_forwarding_tails(void)
{
    struct enumeration_response_timing timing = {0};
    const uint64_t start_ms = 1000u;

    assert(ENUMERATION_RESPONSE_LANE_MS == 10000u);
    assert(!enumeration_response_timing_at(start_ms, start_ms - 1u, &timing));
    assert(enumeration_response_ms_until_lane(start_ms, start_ms - 1u) == 1u);
    assert(enumeration_response_timing_at(start_ms, start_ms, &timing));
    assert(timing.depth == 1u && timing.round == 0u &&
           timing.round_offset_ms == 0u);
    assert(enumeration_response_timing_at(
        start_ms, start_ms + ENUMERATION_RESPONSE_DEPTH_MS, &timing));
    assert(timing.depth == 2u && timing.round == 0u &&
           timing.round_offset_ms == 0u);
    assert(enumeration_response_timing_at(
        start_ms, start_ms + ENUMERATION_RESPONSE_LANE_MS - 1u, &timing));
    assert(timing.depth == 5u && timing.round == 19u &&
           timing.round_offset_ms == 124u);
    assert(!enumeration_response_timing_at(
        start_ms, start_ms + ENUMERATION_RESPONSE_LANE_MS, &timing));
    assert(enumeration_response_lane_complete(
        start_ms, start_ms + ENUMERATION_RESPONSE_LANE_MS));
}

static void test_perceived_depth_expands_by_exactly_one_next_hop_band(void)
{
    struct enumeration_response_timing timing = {0};
    const uint64_t start_ms = 2000u;

    assert(enumeration_response_duration_ms(1u) == 1500u);
    assert(enumeration_response_duration_ms(2u) == 3250u);
    assert(enumeration_response_duration_ms(3u) == 5250u);
    assert(enumeration_response_duration_ms(UWB_ENUM_MAX_HOPS) == 10000u);
    assert(enumeration_response_depth_duration_ms(1u) == 1500u);
    assert(enumeration_response_depth_duration_ms(2u) == 1750u);
    assert(enumeration_response_depth_duration_ms(3u) == 2000u);
    assert(enumeration_response_duration_ms(0u) == 0u);
    assert(enumeration_response_duration_ms(UWB_ENUM_MAX_HOPS + 1u) == 0u);

    /* One-hop knowledge closes after H1 unless the gateway opens H2. */
    assert(enumeration_response_timing_at_depth(
        start_ms, start_ms + ENUMERATION_RESPONSE_DEPTH_MS - 1u, 1u,
        &timing));
    assert(timing.depth == 1u);
    assert(!enumeration_response_timing_at_depth(
        start_ms, start_ms + ENUMERATION_RESPONSE_DEPTH_MS, 1u, &timing));

    /* Expanding the same origin once opens exactly the adjacent H2 band. */
    assert(enumeration_response_timing_at_depth(
        start_ms, start_ms + ENUMERATION_RESPONSE_DEPTH_MS, 2u, &timing));
    assert(timing.depth == 2u && timing.round == 0u &&
           timing.round_offset_ms == 0u);
    assert(enumeration_response_timing_at_depth(
        start_ms, start_ms + enumeration_response_duration_ms(2u) - 1u, 2u,
        &timing));
    assert(timing.depth == 2u && timing.round == 13u &&
           timing.round_offset_ms == 124u);
    assert(!enumeration_response_timing_at_depth(
        start_ms, start_ms + enumeration_response_duration_ms(2u), 2u,
        &timing));
    assert(enumeration_response_lane_complete_depth(
        start_ms, start_ms + enumeration_response_duration_ms(2u), 2u));
}

static void test_claim_start_tlv_remains_valid_after_countdown_becomes_negative(void)
{
    uint8_t payload[sizeof(uint32_t) + 2u];
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;
    uint64_t start_ms = 0u;
    int64_t starts_in_ms = 0;
    const uint64_t now_ms = 10000u;
    const uint32_t late_age_ms =
        ENUMERATION_RESPONSE_START_DELAY_MS + 250u;
    uint32_t advertised_start_delay_ms;

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DISCOVERY_START_DELAY_MS,
                          ENUMERATION_RESPONSE_START_DELAY_MS) == PROTO_OK);
    assert(tlv_find_unique(payload,
                           payload_len,
                           TLV_DISCOVERY_START_DELAY_MS,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    advertised_start_delay_ms = proto_get_u32_le(value);
    assert(advertised_start_delay_ms == ENUMERATION_RESPONSE_START_DELAY_MS);

    assert(enumeration_response_claim_start(now_ms,
                                            late_age_ms,
                                            advertised_start_delay_ms,
                                            &start_ms,
                                            &starts_in_ms));
    assert(starts_in_ms == -250);
    assert(start_ms == now_ms - 250u);
    assert(!enumeration_response_lane_complete_depth(start_ms, now_ms, 1u));

    /* A packet cannot predate the receiver's own uptime domain. */
    assert(!enumeration_response_claim_start(now_ms,
                                             (uint32_t)now_ms + 1u,
                                             advertised_start_delay_ms,
                                             &start_ms,
                                             &starts_in_ms));
}

static void test_claim_lane_timing_crosses_uptime32_wrap_without_truncation(void)
{
    struct enumeration_response_timing timing = {0};
    const uint64_t origin_ms = (uint64_t)UINT32_MAX - 100u;
    const uint32_t packet_age_ms =
        ENUMERATION_RESPONSE_START_DELAY_MS + 250u;
    const uint64_t now_ms = origin_ms + packet_age_ms;
    const uint64_t expected_start_ms =
        origin_ms + ENUMERATION_RESPONSE_START_DELAY_MS;
    const uint64_t expected_h2_deadline_ms =
        expected_start_ms + enumeration_response_duration_ms(2u);
    uint64_t start_ms = 0u;
    int64_t starts_in_ms = 0;

    assert(origin_ms < UINT32_MAX);
    assert(now_ms > UINT32_MAX);
    assert(expected_start_ms > UINT32_MAX);
    assert(enumeration_response_claim_start(
        now_ms,
        packet_age_ms,
        ENUMERATION_RESPONSE_START_DELAY_MS,
        &start_ms,
        &starts_in_ms));
    assert(start_ms == expected_start_ms);
    assert(starts_in_ms == -250);
    assert(enumeration_response_timing_at_depth(
        start_ms, now_ms, 2u, &timing));
    assert(timing.depth == 1u);
    assert(enumeration_response_timing_at_depth(
        start_ms, expected_h2_deadline_ms - 1u, 2u, &timing));
    assert(timing.depth == 2u);
    assert(!enumeration_response_timing_at_depth(
        start_ms, expected_h2_deadline_ms, 2u, &timing));
    assert(enumeration_response_lane_complete_depth(
        start_ms, expected_h2_deadline_ms, 2u));
}

static void test_bounded_claim_relay_reaches_every_depth_before_its_band_closes(void)
{
    const uint64_t claim_origin_ms = 100000u;
    const uint64_t shared_start_ms =
        claim_origin_ms + ENUMERATION_RESPONSE_START_DELAY_MS;
    const uint32_t latest_direct_age_ms =
        ENUMERATION_RESPONSE_START_DELAY_MS +
        ENUMERATION_RESPONSE_DEPTH_MS - 1u;

    assert(MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS ==
           (MESH_ENUMERATION_RELAY_SLOT_COUNT - 1u) *
               MESH_ENUMERATION_RELAY_SLOT_MS);

    for (uint8_t hop_count = 1u;
         hop_count <= UWB_ENUM_MAX_HOPS;
         hop_count++) {
        uint32_t relay_elapsed_ms =
            (uint32_t)(hop_count - 1u) *
                MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS;
        uint32_t packet_age_ms = latest_direct_age_ms + relay_elapsed_ms;
        uint64_t now_ms = claim_origin_ms + latest_direct_age_ms +
                          relay_elapsed_ms;
        uint64_t start_ms = 0u;
        uint64_t band_close_ms;
        int64_t starts_in_ms = 0;

        /* Start with the latest valid H1 receipt, then charge every additional
         * hop the largest possible first-copy enumeration relay slot. */
        assert(enumeration_response_claim_start(
            now_ms,
            packet_age_ms,
            ENUMERATION_RESPONSE_START_DELAY_MS,
            &start_ms,
            &starts_in_ms));
        assert(starts_in_ms < 0);
        assert(start_ms == shared_start_ms);
        band_close_ms = start_ms +
            enumeration_response_duration_ms(hop_count);
        assert(now_ms < band_close_ms);
        assert(!enumeration_response_lane_complete_depth(
            start_ms, now_ms, hop_count));
    }
}

static void test_parent_forwards_new_child_records_in_a_later_depth_band(void)
{
    struct enumeration_response_lane lane;
    struct uwb_enumeration_bundle_frame child = {
        .network_id = 0x494D4543u,
        .epoch = 9u,
        .sender_id = 0x200u,
        .parent_id = 0x100u,
        .sequence = 0u,
        .record_count = 1u,
        .records = {{
            .anchor_id = 0x200u,
            .hop_count = 2u,
        }},
    };
    struct uwb_enumeration_bundle_frame outbound;
    struct uwb_enumeration_hop_ack_frame ack = {
        .network_id = 0x494D4543u,
        .epoch = 9u,
        .parent_id = 0x010u,
        .child_id = 0x100u,
        .sequence = 0u,
    };
    struct enumeration_response_timing timing = {
        .depth = 1u,
        .round = 0u,
        .round_offset_ms = 0u,
    };
    bool added = false;

    assert(enumeration_response_lane_begin(&lane,
                                           child.network_id,
                                           child.epoch,
                                           0x100u,
                                           0x010u,
                                           1u,
                                           1000u) == PROTO_OK);

    /* H1 publishes its local record and transfers that aggregate upstream. */
    assert(enumeration_response_lane_prepare_round(&lane, 0u, 0u) == PROTO_OK);
    timing.round_offset_ms =
        enumeration_response_lane_round_offset_ms(&lane, 0u);
    assert(timing.round_offset_ms != ENUMERATION_RESPONSE_NO_OFFSET);
    assert(enumeration_response_lane_bundle_for_offset(
               &lane, &timing, &outbound) == PROTO_OK);
    assert(outbound.record_count == 1u);
    assert(outbound.records[0].anchor_id == 0x100u);
    assert(enumeration_response_lane_note_ack(&lane, &ack));
    assert(enumeration_response_lane_all_acked(&lane));

    /* H2 then arrives; the parent must reopen and forward the new aggregate. */
    assert(enumeration_response_lane_merge_bundle(
               &lane, &child, &added) == PROTO_OK);
    assert(added);
    assert(lane.record_count == 2u);
    assert(!enumeration_response_lane_all_acked(&lane));
    timing.depth = 2u;
    timing.round = ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH;
    assert(enumeration_response_lane_prepare_round(
               &lane, timing.round, 1u) == PROTO_OK);
    timing.round_offset_ms =
        enumeration_response_lane_round_offset_ms(&lane, 0u);
    assert(timing.round_offset_ms != ENUMERATION_RESPONSE_NO_OFFSET);
    assert(enumeration_response_lane_bundle_for_offset(
               &lane, &timing, &outbound) == PROTO_OK);
    assert(outbound.record_count == 2u);
    assert(outbound.records[0].anchor_id == 0x100u);
    assert(outbound.records[1].anchor_id == 0x200u);
}

static void test_last_child_round_leaves_parent_forward_and_retry_margin(void)
{
    struct enumeration_response_lane parent;
    struct uwb_enumeration_bundle_frame child = {
        .network_id = 0x494D4543u,
        .epoch = 11u,
        .sender_id = 0x200u,
        .parent_id = 0x100u,
        .sequence = 0u,
        .record_count = 1u,
        .records = {{
            .anchor_id = 0x200u,
            .hop_count = 2u,
        }},
    };
    struct uwb_enumeration_bundle_frame outbound;
    struct enumeration_response_timing timing = {
        .depth = 2u,
        .round = ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH - 1u,
        .round_offset_ms = 0u,
    };
    const uint64_t start_ms = 1000u;
    uint8_t first_offset_ms;
    uint8_t retry_offset_ms;
    bool added = false;

    assert(ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP == 2u);
    assert(enumeration_response_lane_begin(&parent,
                                           child.network_id,
                                           child.epoch,
                                           0x100u,
                                           0x010u,
                                           1u,
                                           start_ms) == PROTO_OK);

    /* The deepest child may use its final source round, while its parent is
     * held silent until the two dedicated forwarding rounds that follow. */
    assert(enumeration_response_lane_tx_round_allowed(
        2u, 2u, ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH - 1u));
    assert(!enumeration_response_lane_tx_round_allowed(
        2u, 2u, ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH));
    assert(!enumeration_response_lane_tx_round_allowed(
        2u, 1u, ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH - 1u));
    assert(enumeration_response_lane_merge_bundle(
               &parent, &child, &added) == PROTO_OK);
    assert(added);

    timing.round = ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH;
    assert(enumeration_response_lane_tx_round_allowed(
        timing.depth, 1u, timing.round));
    assert(enumeration_response_lane_prepare_round(
               &parent, timing.round, 3u) == PROTO_OK);
    first_offset_ms =
        enumeration_response_lane_round_offset_ms(&parent, 0u);
    timing.round_offset_ms = first_offset_ms;
    assert(enumeration_response_lane_bundle_for_offset(
               &parent, &timing, &outbound) == PROTO_OK);

    /* With no ACK, the second forwarding round owns a fresh random retry and
     * still ends strictly before the gateway's depth-two cutoff. */
    timing.round++;
    assert(enumeration_response_lane_tx_round_allowed(
        timing.depth, 1u, timing.round));
    assert(enumeration_response_lane_prepare_round(
               &parent, timing.round, 9u) == PROTO_OK);
    retry_offset_ms =
        enumeration_response_lane_round_offset_ms(&parent, 0u);
    assert(retry_offset_ms != first_offset_ms);
    timing.round_offset_ms = retry_offset_ms;
    assert(enumeration_response_lane_bundle_for_offset(
               &parent, &timing, &outbound) == PROTO_OK);
    assert(!enumeration_response_lane_tx_round_allowed(
        2u, 1u, (uint8_t)(timing.round + 1u)));
    assert(enumeration_response_timing_at_depth(
        start_ms,
        start_ms + enumeration_response_duration_ms(2u) - 1u,
        2u,
        &timing));
    assert(timing.depth == 2u && timing.round == 13u &&
           timing.round_offset_ms == 124u);
    assert(!enumeration_response_timing_at_depth(
        start_ms,
        start_ms + enumeration_response_duration_ms(2u),
        2u,
        &timing));
}

static void test_each_depth_orders_two_retry_rounds_per_parent(void)
{
    for (uint8_t response_depth = 1u;
         response_depth <= UWB_ENUM_MAX_HOPS;
         response_depth++) {
        uint8_t rounds_in_depth = (uint8_t)(
            ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
            (response_depth - 1u) *
                ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP);

        assert(enumeration_response_depth_duration_ms(response_depth) ==
               (uint32_t)rounds_in_depth * ENUMERATION_RESPONSE_ROUND_MS);
        for (uint8_t local_hop_count = 1u;
             local_hop_count <= response_depth;
             local_hop_count++) {
            uint8_t allowed_count = 0u;
            uint8_t first_allowed = UINT8_MAX;
            uint8_t last_allowed = UINT8_MAX;

            for (uint8_t round = 0u; round < rounds_in_depth; round++) {
                if (!enumeration_response_lane_tx_round_allowed(
                        response_depth, local_hop_count, round)) {
                    continue;
                }
                if (allowed_count == 0u) {
                    first_allowed = round;
                }
                last_allowed = round;
                allowed_count++;
            }
            if (local_hop_count == response_depth) {
                assert(first_allowed == 0u);
                assert(last_allowed ==
                       ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH - 1u);
                assert(allowed_count ==
                       ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH);
            } else {
                uint8_t expected_first = (uint8_t)(
                    ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
                    (response_depth - local_hop_count - 1u) *
                        ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP);

                assert(first_allowed == expected_first);
                assert(last_allowed == expected_first +
                       ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP - 1u);
                assert(allowed_count ==
                       ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP);
            }
        }
    }
}

static void test_bundle_custody_and_uniform_retry_offsets(void)
{
    struct enumeration_response_lane lane;
    struct uwb_enumeration_bundle_frame child = {
        .network_id = 0x494D4543u,
        .epoch = 7u,
        .sender_id = 0x300u,
        .parent_id = 0x200u,
        .sequence = 0u,
        .record_count = 10u,
    };
    struct uwb_enumeration_bundle_frame outbound;
    struct uwb_enumeration_hop_ack_frame ack = {
        .network_id = 0x494D4543u,
        .epoch = 7u,
        .parent_id = 0x100u,
        .child_id = 0x200u,
        .sequence = 0u,
    };
    struct enumeration_response_timing timing = {
        .depth = 2u,
        .round = 0u,
        .round_offset_ms = 0u,
    };
    uint8_t first_offset_ms;
    uint8_t second_offset_ms;
    bool added = false;

    assert(enumeration_response_lane_begin(&lane,
                                           child.network_id,
                                           child.epoch,
                                           0x200u,
                                           0x100u,
                                           2u,
                                           1000u) == PROTO_OK);
    for (uint8_t i = 0u; i < child.record_count; i++) {
        child.records[i].anchor_id = 0x300u + i;
        child.records[i].hop_count = 3u;
    }
    assert(enumeration_response_lane_merge_bundle(&lane,
                                                  &child,
                                                  &added) == PROTO_OK);
    assert(added);
    assert(lane.record_count == 11u);
    assert(enumeration_response_lane_bundle_count(&lane) == 2u);
    assert(enumeration_response_lane_merge_bundle(&lane,
                                                  &child,
                                                  &added) == PROTO_OK);
    assert(!added);

    assert(enumeration_response_lane_prepare_round(&lane, 0u, 3u) == PROTO_OK);
    first_offset_ms =
        enumeration_response_lane_round_offset_ms(&lane, 0u);
    second_offset_ms =
        enumeration_response_lane_round_offset_ms(&lane, 1u);
    assert(first_offset_ms != ENUMERATION_RESPONSE_NO_OFFSET);
    assert(second_offset_ms != ENUMERATION_RESPONSE_NO_OFFSET);
    assert(first_offset_ms > second_offset_ms ?
               first_offset_ms - second_offset_ms >=
                   ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS :
               second_offset_ms - first_offset_ms >=
                   ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
    timing.round_offset_ms = first_offset_ms;
    assert(enumeration_response_lane_bundle_for_offset(&lane,
                                                     &timing,
                                                     &outbound) == PROTO_OK);
    assert(outbound.record_count == 10u);
    assert(outbound.records[0].anchor_id == 0x200u);
    assert(enumeration_response_lane_note_ack(&lane, &ack));

    timing.round = 1u;
    assert(enumeration_response_lane_prepare_round(&lane, 1u, 9u) == PROTO_OK);
    assert(enumeration_response_lane_round_offset_ms(&lane, 0u) ==
           ENUMERATION_RESPONSE_NO_OFFSET);
    assert(enumeration_response_lane_round_offset_ms(&lane, 1u) == 9u);
    ack.sequence = 1u;
    assert(enumeration_response_lane_note_ack(&lane, &ack));
    assert(enumeration_response_lane_all_acked(&lane));
}

static void test_single_bundle_uses_every_millisecond_uniformly(void)
{
    for (uint32_t seed = 0u;
         seed < ENUMERATION_RESPONSE_TX_WINDOW_MS;
         seed++) {
        struct enumeration_response_lane lane;
        struct enumeration_response_timing timing = {
            .depth = 1u,
            .round = 0u,
            .round_offset_ms = 0u,
        };

        assert(enumeration_response_lane_begin(&lane,
                                               0x494D4543u,
                                               seed + 1u,
                                               0x200u,
                                               0x100u,
                                               1u,
                                               1000u) == PROTO_OK);
        assert(enumeration_response_lane_prepare_round(
                   &lane, 0u, seed) == PROTO_OK);
        assert(enumeration_response_lane_round_offset_ms(&lane, 0u) ==
               seed);
        if (seed > 0u) {
            assert(enumeration_response_lane_next_offset_ms(
                       &lane, &timing) == seed);
        }
    }
}

static void test_ack_reserve_bounds_every_source_offset_without_widening_round(void)
{
    assert(ENUMERATION_RESPONSE_ROUND_MS == 125u);
    assert(ENUMERATION_RESPONSE_TX_ACK_RESERVE_MS == 50u);
    assert(ENUMERATION_RESPONSE_TX_WINDOW_MS == 75u);
    assert(ENUMERATION_RESPONSE_TX_WINDOW_MS +
               ENUMERATION_RESPONSE_TX_ACK_RESERVE_MS ==
           ENUMERATION_RESPONSE_ROUND_MS);
    assert(ENUMERATION_RESPONSE_TX_LATE_GUARD_MS <
           ENUMERATION_RESPONSE_TX_ACK_RESERVE_MS);

    for (uint32_t seed = 0u; seed < 4096u; seed++) {
        struct enumeration_response_lane lane;
        uint8_t bundle_count;

        assert(enumeration_response_lane_begin(&lane,
                                               0x494D4543u,
                                               seed + 1u,
                                               0x200u,
                                               0x100u,
                                               1u,
                                               1000u) == PROTO_OK);
        /* Exercise every aggregate offset in the largest supported roster. */
        lane.record_count = MESH_CONNECTED_MAX_ANCHORS;
        bundle_count = enumeration_response_lane_bundle_count(&lane);
        assert(bundle_count == ENUMERATION_RESPONSE_MAX_BUNDLES);
        assert(enumeration_response_lane_prepare_round(
                   &lane, 0u, seed) == PROTO_OK);

        for (uint8_t sequence = 0u; sequence < bundle_count; sequence++) {
            uint8_t offset = enumeration_response_lane_round_offset_ms(
                &lane, sequence);

            assert(offset != ENUMERATION_RESPONSE_NO_OFFSET);
            assert(offset < ENUMERATION_RESPONSE_TX_WINDOW_MS);
            for (uint8_t previous = 0u; previous < sequence; previous++) {
                uint8_t previous_offset =
                    enumeration_response_lane_round_offset_ms(
                        &lane, previous);
                uint8_t distance = offset > previous_offset ?
                    (uint8_t)(offset - previous_offset) :
                    (uint8_t)(previous_offset - offset);

                assert(distance >=
                       ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
            }
        }
    }
}

int main(void)
{
    test_fixed_schedule_is_shallowest_first_with_forwarding_tails();
    test_perceived_depth_expands_by_exactly_one_next_hop_band();
    test_claim_start_tlv_remains_valid_after_countdown_becomes_negative();
    test_claim_lane_timing_crosses_uptime32_wrap_without_truncation();
    test_bounded_claim_relay_reaches_every_depth_before_its_band_closes();
    test_parent_forwards_new_child_records_in_a_later_depth_band();
    test_last_child_round_leaves_parent_forward_and_retry_margin();
    test_each_depth_orders_two_retry_rounds_per_parent();
    test_bundle_custody_and_uniform_retry_offsets();
    test_single_bundle_uses_every_millisecond_uniformly();
    test_ack_reserve_bounds_every_source_offset_without_widening_round();
    return 0;
}
