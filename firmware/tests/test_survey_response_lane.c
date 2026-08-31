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
                                      2u, 3u, 1000u) == PROTO_OK);
    assert(survey_response_lane_begin(&parent, 9u, 7u, 0x11u, 0x99u,
                                      SURVEY_RESPONSE_NEIGHBORS,
                                      1u, 3u, 1000u) == PROTO_OK);
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
                                      2u, 5u, 1000u) == PROTO_OK);
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

static void test_max_neighbor_and_signal_bundles(void)
{
    struct survey_response_lane lane;
    bool added;

    assert(survey_response_lane_begin(&lane, 9u, 8u, 0x22u, 0x11u,
                                      SURVEY_RESPONSE_NEIGHBORS,
                                      2u, 5u, 1000u) == PROTO_OK);
    for (uint8_t slot = 0u; slot < SURVEY_MAX_ANCHORS; slot++) {
        struct survey_response_record record = neighbor_record(
            slot, (uint8_t)((slot + 1u) % SURVEY_MAX_ANCHORS));

        assert(survey_response_lane_add_record(&lane, &record,
                                               &added) == PROTO_OK);
        assert(added);
        for (uint8_t chunk = 0u;
             chunk < survey_signal_record_count_for_slot(slot); chunk++) {
            record = signal_record(slot, chunk);
            assert(survey_response_lane_add_record(&lane, &record,
                                                   &added) == PROTO_OK);
            assert(added);
        }
    }
    assert(lane.record_count == SURVEY_RESPONSE_MAX_RECORDS);
    assert(lane.record_count == 162u);
    assert(survey_response_lane_bundle_count(&lane) == 9u);
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

int main(void)
{
    test_lane_custody_and_ack();
    test_max_result_bundles();
    test_max_neighbor_and_signal_bundles();
    test_raw_codecs_and_generation_binding();
    puts("survey response lane tests passed");
    return 0;
}
