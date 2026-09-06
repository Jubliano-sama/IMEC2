#include "survey_response_lane.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct survey_response_record neighbor_record(uint8_t slot,
                                                      uint8_t heard)
{
    struct survey_neighbor_report report = {.own_slot = slot};
    struct survey_response_record record = {0};

    assert(survey_neighbor_bitmap_set(report.heard_bitmap, heard));
    assert(survey_neighbor_report_encode(&report, record.bytes) ==
           sizeof(record.bytes));
    return record;
}

static struct survey_response_record range_record(uint8_t pair,
                                                   uint8_t responder,
                                                   int32_t distance)
{
    struct survey_range_result result = {
        .median_mm = distance,
        .pair_index = pair,
        .success_count = 3u,
        .responder_slot = responder,
    };
    struct survey_response_record record = {0};

    assert(survey_range_result_encode(&result, record.bytes) ==
           sizeof(record.bytes));
    return record;
}

static struct survey_response_record signal_record(uint8_t owner,
                                                    uint8_t chunk)
{
    uint8_t levels[SURVEY_MAX_ANCHORS] = {0};
    struct survey_signal_record signal;
    struct survey_response_record record = {0};

    for (uint8_t target = 0u; target < owner; target++) {
        levels[target] = (uint8_t)(1u + target % 15u);
    }
    assert(survey_signal_record_encode(owner, chunk, levels, &signal) ==
           sizeof(signal.bytes));
    memcpy(record.bytes, signal.bytes, sizeof(signal.bytes));
    return record;
}

static void test_lane_custody_and_ack(void)
{
    struct survey_response_lane child;
    struct survey_response_lane parent;
    struct survey_response_bundle bundle;
    struct survey_response_hop_ack ack;
    struct enumeration_response_timing timing;
    bool added;

    assert(survey_response_lane_begin(&child, 9u, 7u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_NEIGHBORS,
                                       0u, 2u, 3u, 1000u) == PROTO_OK);
    assert(survey_response_lane_begin(&parent, 9u, 7u, 0x11u, 0x99u,
                                      SURVEY_RESPONSE_NEIGHBORS,
                                       0u, 1u, 3u, 1000u) == PROTO_OK);
    {
        struct survey_response_record record = neighbor_record(2u, 1u);
        assert(survey_response_lane_add_record(&child, &record,
                                               &added) == PROTO_OK);
        assert(added);
    }
    timing = (struct enumeration_response_timing) {
        .depth = 2u,
        .round = 0u,
        .round_offset_ms = 0u,
    };
    assert(survey_response_lane_prepare_round(&child, 0u, 123u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&child, 0u);
    assert(survey_response_lane_bundle_for_offset(&child, &timing,
                                                  &bundle) == PROTO_OK);
    assert(survey_response_lane_merge_bundle(&parent, &bundle,
                                             &added) == PROTO_OK);
    assert(added);
    ack = (struct survey_response_hop_ack) {
        .network_id = 9u,
        .generation = 7u,
        .parent_id = 0x11u,
        .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_NEIGHBORS,
        .sequence = 0u,
    };
    assert(survey_response_bundle_make_ack(&bundle, &ack) == PROTO_OK);
    assert(survey_response_lane_note_ack(&child, &ack));
    assert(survey_response_lane_all_acked(&child));
    assert(parent.record_count == 1u);
    assert(!survey_response_lane_note_ack(&parent, &ack));
}

static void test_max_result_bundles(void)
{
    struct survey_response_lane lane;
    bool added;

    assert(survey_response_lane_begin(&lane, 9u, 8u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_RANGES,
                                       0u, 2u, 5u, 1000u) == PROTO_OK);
    for (uint8_t pair = 0u; pair < SURVEY_MAX_PAIRS; pair++) {
        struct survey_response_record record =
            range_record(pair, (uint8_t)(pair % SURVEY_MAX_ANCHORS),
                         1000 + pair);
        assert(survey_response_lane_add_record(&lane, &record,
                                               &added) == PROTO_OK);
        assert(added);
    }
    assert(lane.record_count == SURVEY_MAX_PAIRS);
    assert(survey_response_lane_bundle_count(&lane) == 5u);
    assert(survey_response_lane_prepare_round(&lane, 0u, 456u) == PROTO_OK);
    for (uint8_t sequence = 0u; sequence < 5u; sequence++) {
        uint8_t offset = survey_response_lane_round_offset_ms(&lane, sequence);
        assert(offset != SURVEY_RESPONSE_NO_OFFSET);
        for (uint8_t prior = 0u; prior < sequence; prior++) {
            uint8_t prior_offset =
                survey_response_lane_round_offset_ms(&lane, prior);
            uint8_t distance = offset > prior_offset ?
                (uint8_t)(offset - prior_offset) :
                (uint8_t)(prior_offset - offset);
            assert(distance >= ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
        }
    }
}

static void fill_neighbor_lane(struct survey_response_lane *lane,
                               uint8_t first_slot, uint8_t count)
{
    bool added;

    assert(survey_response_lane_begin(lane, 9u, 8u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_NEIGHBORS,
                                       0u, 2u, 5u, 1000u) == PROTO_OK);
    for (uint8_t slot = first_slot; slot < first_slot + count; slot++) {
        struct survey_response_record record = neighbor_record(
            slot, (uint8_t)((slot + 1u) % SURVEY_MAX_ANCHORS));

        assert(survey_response_lane_add_record(lane, &record,
                                               &added) == PROTO_OK);
        assert(added);
        for (uint8_t chunk = 0u;
             chunk < survey_signal_record_count_for_slot(slot); chunk++) {
            record = signal_record(slot, chunk);
            assert(survey_response_lane_add_record(lane, &record,
                                                   &added) == PROTO_OK);
            assert(added);
        }
    }
}

static void test_max_neighbor_and_signal_bundles(void)
{
    struct survey_response_lane lane;

    fill_neighbor_lane(&lane, 0u, SURVEY_MAX_ANCHORS);
    assert(lane.record_count == SURVEY_RESPONSE_MAX_RECORDS);
    assert(lane.record_count == 162u);
    assert(survey_response_lane_bundle_count(&lane) == 9u);
}

static uint16_t assert_round_schedule(const struct survey_response_lane *lane)
{
    uint8_t bundle_count = survey_response_lane_bundle_count(lane);
    uint8_t scheduled = 0u;
    uint8_t pending = 0u;
    uint16_t scheduled_mask = 0u;

    for (uint8_t sequence = 0u; sequence < SURVEY_RESPONSE_MAX_BUNDLES;
         sequence++) {
        uint8_t offset = survey_response_lane_round_offset_ms(lane, sequence);
        bool waiting = sequence < bundle_count &&
            (lane->acked_mask & (UINT16_C(1) << sequence)) == 0u;

        pending += waiting ? 1u : 0u;
        if (offset == SURVEY_RESPONSE_NO_OFFSET) {
            continue;
        }
        assert(waiting);
        assert(offset < ENUMERATION_RESPONSE_TX_WINDOW_MS -
                        ENUMERATION_RESPONSE_TX_LATE_GUARD_MS);
        for (uint8_t prior = 0u; prior < sequence; prior++) {
            uint8_t old = survey_response_lane_round_offset_ms(lane, prior);
            uint8_t distance = offset > old ? (uint8_t)(offset - old) :
                                              (uint8_t)(old - offset);

            if (old != SURVEY_RESPONSE_NO_OFFSET) {
                assert(distance >= ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
            }
        }
        scheduled++;
        scheduled_mask |= (uint16_t)(UINT16_C(1) << sequence);
    }
    /* Existing 0..70 ms starts and 10 ms spacing can hold eight bundles. */
    assert(scheduled == (pending < 8u ? pending : 8u));
    return scheduled_mask;
}

static void assert_two_round_opportunities(struct survey_response_lane *lane,
                                           uint8_t round, uint32_t seed)
{
    uint8_t count = survey_response_lane_bundle_count(lane);
    uint16_t expected = (uint16_t)(((UINT16_C(1) << count) - 1u) &
                                   (uint16_t)~lane->acked_mask);
    uint16_t scheduled;

    assert(survey_response_lane_prepare_round(lane, round, seed) == PROTO_OK);
    assert(lane->attempted_mask == 0u);
    scheduled = assert_round_schedule(lane);
    assert(survey_response_lane_prepare_round(lane, (uint8_t)(round + 1u),
                                               seed ^ UINT32_MAX) == PROTO_OK);
    scheduled |= assert_round_schedule(lane);
    assert(scheduled == expected);
}

static void test_all_counts_seeds_and_partial_ack_schedules(void)
{
    struct survey_response_lane full;
    struct survey_response_lane lane;

    fill_neighbor_lane(&full, 0u, SURVEY_MAX_ANCHORS);
    /* Every record count, including partial bundles, gets every small seed
     * plus UINT32_MAX. Vary the first round across its entire legal domain. */
    for (unsigned count = 0u; count <= SURVEY_RESPONSE_MAX_RECORDS; count++) {
        for (uint32_t sample = 0u; sample <= 1024u; sample++) {
            uint32_t seed = sample == 1024u ? UINT32_MAX : sample;
            uint8_t round = (uint8_t)(sample %
                (ENUMERATION_RESPONSE_MAX_ROUNDS_PER_DEPTH - 1u));

            lane = full;
            lane.record_count = (uint8_t)count;
            assert_two_round_opportunities(&lane, round, seed);
        }
    }
    /* ACKed bundles cannot consume capacity, regardless of which subset
     * was delivered; every remaining bundle must still get an opportunity. */
    for (uint16_t acked = 0u;
         acked < (UINT16_C(1) << SURVEY_RESPONSE_MAX_BUNDLES); acked++) {
        lane = full;
        lane.acked_mask = acked;
        assert_two_round_opportunities(&lane, 0u, 265u);
    }

    fill_neighbor_lane(&lane, 20u, 30u);
    assert(lane.record_count == 118u);
    assert_two_round_opportunities(&lane, 0u, 8u);
}

static void test_deferred_bundle_custody_and_same_round_idempotence(void)
{
    struct survey_response_lane lane;
    struct survey_response_hop_ack ack = {
        .network_id = 9u, .generation = 8u,
        .parent_id = 0x11u, .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_NEIGHBORS,
    };
    struct enumeration_response_timing timing = {.depth = 2u};
    uint8_t before[sizeof(lane)];

    fill_neighbor_lane(&lane, 0u, SURVEY_MAX_ANCHORS);
    for (uint8_t round = 0u; round < 2u; round++) {
        assert(survey_response_lane_prepare_round(&lane, round, 0u) == PROTO_OK);
        timing.round = round;
        for (uint8_t seq = 0u; seq < SURVEY_RESPONSE_MAX_BUNDLES; seq++) {
            struct survey_response_bundle bundle;
            uint8_t offset = survey_response_lane_round_offset_ms(&lane, seq);

            ack.sequence = seq;
            if (offset == SURVEY_RESPONSE_NO_OFFSET) {
                assert(!survey_response_lane_note_ack(&lane, &ack));
                continue;
            }
            timing.round_offset_ms = offset;
            assert(survey_response_lane_bundle_for_offset(&lane, &timing,
                                                           &bundle) == PROTO_OK);
            assert(bundle.sequence == seq);
            assert(survey_response_bundle_make_ack(&bundle, &ack) == PROTO_OK);
            assert(survey_response_lane_note_ack(&lane, &ack));
        }
        memcpy(before, &lane, sizeof(before));
        assert(survey_response_lane_prepare_round(&lane, round,
                                                   UINT32_MAX) == PROTO_OK);
        assert(memcmp(before, &lane, sizeof(before)) == 0);
        assert(survey_response_lane_all_acked(&lane) == (round == 1u));
    }
}

static void fill_acked_range_lane(struct survey_response_lane *lane,
                                  uint8_t count)
{
    struct survey_response_hop_ack ack = {
        .network_id = 9u, .generation = 7u,
        .parent_id = 0x11u, .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES,
    };
    struct enumeration_response_timing timing = {.depth = 2u, .round = 0u};

    assert(survey_response_lane_begin(lane, 9u, 7u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_RANGES,
                                       0u, 2u, 3u, 1000u) == PROTO_OK);
    for (uint8_t pair = 0u; pair < count; pair++) {
        struct survey_response_record record = range_record(pair, 1u, 1000 + pair);

        assert(survey_response_lane_add_record(lane, &record, NULL) == PROTO_OK);
    }
    assert(survey_response_lane_prepare_round(lane, 0u, 265u) == PROTO_OK);
    for (uint8_t seq = 0u; seq < survey_response_lane_bundle_count(lane); seq++) {
        struct survey_response_bundle bundle;

        timing.round_offset_ms = survey_response_lane_round_offset_ms(lane, seq);
        assert(survey_response_lane_bundle_for_offset(lane, &timing,
                                                       &bundle) == PROTO_OK);
        assert(survey_response_bundle_make_ack(&bundle, &ack) == PROTO_OK);
        assert(survey_response_lane_note_ack(lane, &ack));
    }
    assert(survey_response_lane_all_acked(lane));
}

static struct survey_response_bundle incoming_ranges(void)
{
    return (struct survey_response_bundle) {
        .network_id = 9u, .generation = 7u,
        .sender_id = 0x33u, .parent_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES,
    };
}

static void test_rejected_merge_preserves_all_custody(void)
{
    struct survey_response_lane original;

    fill_acked_range_lane(&original, 21u);
    for (uint8_t prefix = 1u; prefix < SURVEY_RESPONSE_RECORDS_PER_BUNDLE; prefix++) {
        for (uint8_t failure = 0u; failure < 3u; failure++) {
            struct survey_response_lane lane = original;
            struct survey_response_bundle bundle = incoming_ranges();
            uint8_t before[sizeof(lane)];
            bool added = true;
            int expected = failure == 2u ? PROTO_ERR_MALFORMED : PROTO_ERR_STALE;

            bundle.record_count = (uint8_t)(prefix + 1u);
            for (uint8_t i = 0u; i < prefix; i++) {
                bundle.records[i] = range_record((uint8_t)(21u + i), 1u,
                                                 1021 + i);
            }
            bundle.records[prefix] = range_record(failure == 1u ? 21u : 0u,
                                                   1u, 9999);
            if (failure == 2u) {
                bundle.records[prefix].bytes[0] = SURVEY_MAX_PAIRS;
            }
            memcpy(before, &lane, sizeof(before));
            assert(survey_response_lane_merge_bundle(&lane, &bundle,
                                                       &added) == expected);
            assert(memcmp(before, &lane, sizeof(before)) == 0);
            assert(added); /* Failure does not publish a partial output. */
            assert(survey_response_lane_merge_bundle(&lane, &bundle,
                                                       NULL) == expected);
            assert(memcmp(before, &lane, sizeof(before)) == 0);

            bundle.record_count = prefix;
            assert(survey_response_lane_merge_bundle(&lane, &bundle,
                                                       &added) == PROTO_OK);
            assert(added && lane.record_count == 21u + prefix);
            assert(lane.acked_mask == 1u && lane.attempted_mask == 1u);
            assert(!survey_response_lane_all_acked(&lane));
        }
    }
}

static void test_merge_invalidates_only_changed_bundles(void)
{
    for (uint8_t count = 1u; count < SURVEY_MAX_PAIRS; count++) {
        struct survey_response_lane lane;
        struct survey_response_bundle bundle = incoming_ranges();
        uint8_t before[sizeof(lane)];
        uint16_t preserved = (uint16_t)((UINT16_C(1) <<
            (count / SURVEY_RESPONSE_RECORDS_PER_BUNDLE)) - 1u);
        bool added = false;

        fill_acked_range_lane(&lane, count);
        bundle.record_count = 3u;
        bundle.records[0] = range_record(count, 1u, 1000 + count);
        bundle.records[1] = bundle.records[0];
        bundle.records[2] = lane.records[0];
        assert(survey_response_lane_merge_bundle(&lane, &bundle,
                                                   &added) == PROTO_OK);
        assert(added && lane.record_count == count + 1u);
        assert(lane.acked_mask == preserved && lane.attempted_mask == preserved);
        assert(lane.prepared_round == UINT8_MAX);
        assert(!survey_response_lane_all_acked(&lane));

        memcpy(before, &lane, sizeof(before));
        assert(survey_response_lane_merge_bundle(&lane, &bundle,
                                                   &added) == PROTO_OK);
        assert(!added && memcmp(before, &lane, sizeof(before)) == 0);
    }
}

static void test_raw_codecs_and_generation_binding(void)
{
    struct survey_presence_frame presence = {
        .network_id = 9u,
        .generation = 77u,
        .sender_id = 0x1234u,
        .sender_slot = 3u,
    };
    struct survey_presence_frame decoded_presence;
    struct survey_response_bundle bundle = {
        .network_id = 9u,
        .generation = 77u,
        .sender_id = 0x22u,
        .parent_id = 0x11u,
        .kind = SURVEY_RESPONSE_RANGES,
        .sequence = 1u,
        .record_count = 1u,
    };
    struct survey_response_bundle decoded_bundle;
    struct survey_response_hop_ack ack = {
        .network_id = 9u,
        .generation = 77u,
        .parent_id = 0x11u,
        .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES,
        .sequence = 1u,
    };
    struct survey_response_hop_ack decoded_ack;
    uint8_t encoded[UWB_SURVEY_BUNDLE_MAX_LEN];
    size_t encoded_len;

    assert(uwb_encode_survey_presence(&presence, encoded, sizeof(encoded),
                                      &encoded_len) == PROTO_OK);
    assert(encoded_len == UWB_SURVEY_PRESENCE_LEN);
    assert(uwb_decode_survey_presence(encoded, encoded_len,
                                      &decoded_presence) == PROTO_OK);
    assert(decoded_presence.generation == 77u);

    bundle.records[0] = range_record(9u, 4u, 2222);
    assert(uwb_encode_survey_bundle(&bundle, encoded, sizeof(encoded),
                                    &encoded_len) == PROTO_OK);
    assert(uwb_decode_survey_bundle(encoded, encoded_len,
                                    &decoded_bundle) == PROTO_OK);
    assert(decoded_bundle.generation == 77u);
    encoded[7] ^= 1u;
    assert(uwb_decode_survey_bundle(encoded, encoded_len,
                                    &decoded_bundle) == PROTO_ERR_BAD_CRC);

    assert(uwb_encode_survey_hop_ack(&ack, encoded, sizeof(encoded),
                                     &encoded_len) == PROTO_OK);
    assert(encoded_len == UWB_SURVEY_HOP_ACK_LEN);
    assert(uwb_decode_survey_hop_ack(encoded, encoded_len,
                                    &decoded_ack) == PROTO_OK);
    assert(decoded_ack.kind == SURVEY_RESPONSE_RANGES);
}

static void test_stale_ack_cannot_cross_batch_or_bundle_contents(void)
{
    struct survey_response_lane lane;
    struct enumeration_response_timing timing = {.depth = 1u, .round = 0u};
    struct survey_response_bundle old_bundle;
    struct survey_response_bundle current_bundle;
    struct survey_response_hop_ack old_ack;
    struct survey_response_record first = range_record(0u, 1u, 1000);
    struct survey_response_record second = range_record(1u, 1u, 1001);

    assert(survey_response_lane_begin(&lane, 9u, 77u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_RANGES,
                                      0u, 1u, 1u, 1000u) == PROTO_OK);
    assert(survey_response_lane_add_record(&lane, &first, NULL) == PROTO_OK);
    assert(survey_response_lane_prepare_round(&lane, 0u, 1u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing,
                                                  &old_bundle) == PROTO_OK);
    assert(survey_response_bundle_make_ack(&old_bundle, &old_ack) == PROTO_OK);

    /* A next batch can reuse sequence zero, but never its previous ACK. */
    assert(survey_response_lane_begin(&lane, 9u, 77u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_RANGES,
                                      1u, 1u, 1u, 1000u) == PROTO_OK);
    assert(survey_response_lane_add_record(&lane, &first, NULL) == PROTO_OK);
    assert(survey_response_lane_prepare_round(&lane, 0u, 2u) == PROTO_OK);
    assert(!survey_response_lane_note_ack(&lane, &old_ack));
    assert(lane.acked_mask == 0u);

    /* The same batch and sequence also require the exact immutable records. */
    assert(survey_response_lane_begin(&lane, 9u, 77u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_RANGES,
                                      1u, 1u, 1u, 1000u) == PROTO_OK);
    assert(survey_response_lane_add_record(&lane, &first, NULL) == PROTO_OK);
    assert(survey_response_lane_prepare_round(&lane, 0u, 3u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing,
                                                  &old_bundle) == PROTO_OK);
    assert(survey_response_bundle_make_ack(&old_bundle, &old_ack) == PROTO_OK);
    assert(survey_response_lane_add_record(&lane, &second, NULL) == PROTO_OK);
    assert(survey_response_lane_prepare_round(&lane, 1u, 4u) == PROTO_OK);
    timing.round = 1u;
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing,
                                                  &current_bundle) == PROTO_OK);
    assert(!survey_response_lane_note_ack(&lane, &old_ack));
    assert(lane.acked_mask == 0u);
    assert(survey_response_bundle_make_ack(&current_bundle, &old_ack) == PROTO_OK);
    assert(survey_response_lane_note_ack(&lane, &old_ack));
}

int main(void)
{
    test_lane_custody_and_ack();
    test_max_result_bundles();
    test_max_neighbor_and_signal_bundles();
    test_all_counts_seeds_and_partial_ack_schedules();
    test_deferred_bundle_custody_and_same_round_idempotence();
    test_rejected_merge_preserves_all_custody();
    test_merge_invalidates_only_changed_bundles();
    test_raw_codecs_and_generation_binding();
    test_stale_ack_cannot_cross_batch_or_bundle_contents();
    puts("survey response lane tests passed");
    return 0;
}
