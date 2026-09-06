#include "survey_response_lane.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* This links the unmodified production core. The Python parent owns timeouts. */
static void begin(struct survey_response_lane *lane,
                  enum survey_response_kind kind)
{
    assert(survey_response_lane_begin(lane, 9u, 7u, 0x22u, 0x11u,
                                     kind, 1u, 2u, 1000u) == PROTO_OK);
}

static void add(struct survey_response_lane *lane,
                const struct survey_response_record *record)
{
    bool added = false;

    assert(survey_response_lane_add_record(lane, record, &added) == PROTO_OK);
    assert(added);
}

static struct survey_response_record range_record(uint8_t pair,
                                                  int32_t distance)
{
    const struct survey_range_result result = {
        .median_mm = distance,
        .pair_index = pair,
        .success_count = 3u,
        .responder_slot = (uint8_t)(pair % 30u),
    };
    struct survey_response_record record = {0};

    assert(survey_range_result_encode(&result, record.bytes) ==
           sizeof(record.bytes));
    return record;
}

static void neighbors(struct survey_response_lane *lane,
                      uint8_t first_slot, uint8_t node_count)
{
    begin(lane, SURVEY_RESPONSE_NEIGHBORS);
    for (uint8_t i = 0u; i < node_count; ++i) {
        uint8_t slot = (uint8_t)(first_slot + i);
        struct survey_neighbor_report report = {.own_slot = slot};
        struct survey_response_record record = {0};
        uint8_t levels[SURVEY_MAX_ANCHORS] = {0};

        for (uint8_t peer = first_slot;
             peer < (uint8_t)(first_slot + node_count); ++peer) {
            if (peer != slot) {
                assert(survey_neighbor_bitmap_set(report.heard_bitmap, peer));
            }
            if (peer < slot) {
                levels[peer] = 8u;
            }
        }
        assert(survey_neighbor_report_encode(&report, record.bytes) ==
               sizeof(record.bytes));
        add(lane, &record);
        for (uint8_t chunk = 0u;
             chunk < survey_signal_record_count_for_slot(slot); ++chunk) {
            struct survey_signal_record signal;

            assert(survey_signal_record_encode(slot, chunk, levels, &signal) ==
                   sizeof(signal.bytes));
            memcpy(record.bytes, signal.bytes, sizeof(record.bytes));
            add(lane, &record);
        }
    }
}

static int prepare(const char *name)
{
    struct survey_response_lane lane;
    uint32_t seed;
    int ret;

    if (strcmp(name, "compact30") == 0) {
        neighbors(&lane, 0u, 30u);
        assert(lane.record_count == 75u);
        seed = 265u;
    } else if (strcmp(name, "sparse30") == 0) {
        neighbors(&lane, 20u, 30u);
        assert(lane.record_count == 118u);
        seed = 8u;
    } else if (strcmp(name, "full50") == 0) {
        neighbors(&lane, 0u, 50u);
        assert(lane.record_count == 162u);
        seed = 0u;
    } else if (strcmp(name, "pairs100") == 0 ||
               strcmp(name, "pairs100_baseline") == 0) {
        begin(&lane, SURVEY_RESPONSE_RANGES);
        for (uint8_t pair = 0u; pair < 100u; ++pair) {
            struct survey_response_record record = range_record(pair, 1000 + pair);

            add(&lane, &record);
        }
        seed = strcmp(name, "pairs100_baseline") == 0 ? 456u : 265u;
    } else {
        return 2;
    }
    printf("case=%s records=%u bundles=%u seed=%u prepare_enter\n",
           name, (unsigned)lane.record_count,
           (unsigned)survey_response_lane_bundle_count(&lane), (unsigned)seed);
    fflush(stdout);
    ret = survey_response_lane_prepare_round(&lane, 0u, seed);
    printf("prepare_return=%d offsets=", ret);
    for (uint8_t seq = 0u; seq < survey_response_lane_bundle_count(&lane); ++seq) {
        uint8_t offset = survey_response_lane_round_offset_ms(&lane, seq);

        printf("%s%u", seq == 0u ? "" : ",", (unsigned)offset);
        if (ret == PROTO_OK && offset != SURVEY_RESPONSE_NO_OFFSET) {
            assert(offset < ENUMERATION_RESPONSE_TX_WINDOW_MS -
                            ENUMERATION_RESPONSE_TX_LATE_GUARD_MS);
            for (uint8_t prior = 0u; prior < seq; ++prior) {
                uint8_t old = survey_response_lane_round_offset_ms(&lane, prior);
                unsigned distance = offset > old ? offset - old : old - offset;

                if (old != SURVEY_RESPONSE_NO_OFFSET) {
                    assert(distance >= ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS);
                }
            }
        }
    }
    puts("");
    /* Any explicit return settles the bounded-completion contract. */
    return 0;
}

static int merge(const char *name)
{
    struct survey_response_lane lane;
    struct survey_response_lane before;
    struct survey_response_record first = range_record(0u, 1000);
    struct enumeration_response_timing timing = {.depth = 1u, .round = 0u};
    struct survey_response_bundle sent;
    struct survey_response_bundle incoming = {
        .network_id = 9u, .generation = 7u,
        .sender_id = 0x33u, .parent_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES, .sequence = 0u,
        .record_count = 2u,
    };
    const struct survey_response_hop_ack ack = {
        .network_id = 9u, .generation = 7u,
        .parent_id = 0x11u, .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES, .sequence = 0u,
    };
    uint8_t wire[UWB_SURVEY_BUNDLE_MAX_LEN];
    size_t wire_len = 0u;
    struct survey_response_bundle decoded;
    bool added = false;
    bool conflict = strcmp(name, "merge_conflict") == 0;
    int ret;

    begin(&lane, SURVEY_RESPONSE_RANGES);
    add(&lane, &first);
    assert(survey_response_lane_prepare_round(&lane, 0u, 1u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing, &sent) == PROTO_OK);
    assert(survey_response_lane_note_ack(&lane, &ack));
    assert(survey_response_lane_all_acked(&lane));
    before = lane;
    incoming.records[0] = range_record(1u, 1001);
    incoming.records[1] = conflict ? range_record(0u, 2000) : first;
    /* Both records are valid on wire; the rejection is a custody conflict. */
    assert(uwb_encode_survey_bundle(&incoming, wire, sizeof(wire), &wire_len) == PROTO_OK);
    assert(uwb_decode_survey_bundle(wire, wire_len, &decoded) == PROTO_OK);
    ret = survey_response_lane_merge_bundle(&lane, &decoded, &added);
    printf("case=%s merge_return=%d added=%d records=%u->%u "
           "acked=%u->%u attempted=%u->%u prepared=%u->%u all_acked=%d\n",
           name, ret, added, (unsigned)before.record_count, (unsigned)lane.record_count,
           (unsigned)before.acked_mask, (unsigned)lane.acked_mask,
           (unsigned)before.attempted_mask, (unsigned)lane.attempted_mask,
           (unsigned)before.prepared_round, (unsigned)lane.prepared_round,
           survey_response_lane_all_acked(&lane));
    if (!conflict) {
        assert(ret == PROTO_OK && added && lane.record_count == 2u);
        assert(lane.acked_mask == 0u && lane.attempted_mask == 0u);
        return 0;
    }
    assert(ret == PROTO_ERR_STALE);
    if (memcmp(&lane, &before, sizeof(lane)) != 0) {
        /* Retrying the valid prefix alone must not inherit the old ACK. */
        incoming.record_count = 1u;
        added = false;
        ret = survey_response_lane_merge_bundle(&lane, &incoming, &added);
        printf("prefix_retry_return=%d added=%d all_acked=%d\n",
               ret, added, survey_response_lane_all_acked(&lane));
        assert(survey_response_lane_prepare_round(&lane, 1u, 1u) == PROTO_OK);
        printf("next_round_bundle0_offset=%u\n",
               (unsigned)survey_response_lane_round_offset_ms(&lane, 0u));
        puts("FAIL: rejected bundle changed custody; new record retained old upstream ACK");
        return 1;
    }
    puts("PASS: rejected bundle left all custody and scheduling state unchanged");
    return 0;
}

static int replay_ack(const char *name)
{
    struct survey_response_lane lane;
    struct survey_response_record record = range_record(0u, 1000);
    struct survey_response_bundle old_bundle;
    struct survey_response_bundle new_bundle;
    struct enumeration_response_timing timing = {.depth = 1u, .round = 0u};
    const struct survey_response_hop_ack prior_ack = {
        .network_id = 9u, .generation = 7u,
        .parent_id = 0x11u, .child_id = 0x22u,
        .kind = SURVEY_RESPONSE_RANGES, .sequence = 0u,
    };
    struct survey_response_hop_ack decoded_ack;
    uint8_t wire[UWB_SURVEY_HOP_ACK_LEN];
    size_t wire_len = 0u;
    bool same_generation = strcmp(name, "replay_ack_same_generation") == 0;
    bool accepted;

    begin(&lane, SURVEY_RESPONSE_RANGES);
    add(&lane, &record);
    assert(survey_response_lane_prepare_round(&lane, 0u, 1u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing, &old_bundle) == PROTO_OK);
    assert(uwb_encode_survey_hop_ack(&prior_ack, wire, sizeof(wire), &wire_len) == PROTO_OK);
    assert(uwb_decode_survey_hop_ack(wire, wire_len, &decoded_ack) == PROTO_OK);
    assert(survey_response_lane_note_ack(&lane, &decoded_ack));

    /* Later ranging batch: reset lane state, keep operation identity, change data. */
    assert(survey_response_lane_begin(&lane, 9u, same_generation ? 7u : 8u,
                                     0x22u, 0x11u, SURVEY_RESPONSE_RANGES,
                                     1u, 2u, 2000u) == PROTO_OK);
    record = range_record(0u, 2000);
    add(&lane, &record);
    assert(survey_response_lane_prepare_round(&lane, 0u, 1u) == PROTO_OK);
    timing.round_offset_ms = survey_response_lane_round_offset_ms(&lane, 0u);
    assert(survey_response_lane_bundle_for_offset(&lane, &timing, &new_bundle) == PROTO_OK);
    assert(memcmp(old_bundle.records, new_bundle.records,
                  sizeof(old_bundle.records[0])) != 0);
    accepted = survey_response_lane_note_ack(&lane, &decoded_ack);
    printf("case=%s old_generation=%u new_generation=%u payload_changed=1 "
           "prior_ack_accepted=%d all_acked=%d\n", name,
           (unsigned)old_bundle.generation, (unsigned)new_bundle.generation,
           accepted, survey_response_lane_all_acked(&lane));
    if (accepted) {
        puts("FAIL: old lane ACK retired changed data in a later lane");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "merge_conflict") == 0 ||
        strcmp(argv[1], "merge_baseline") == 0) {
        return merge(argv[1]);
    }
    if (strcmp(argv[1], "replay_ack_same_generation") == 0 ||
        strcmp(argv[1], "replay_ack_new_generation") == 0) {
        return replay_ack(argv[1]);
    }
    return prepare(argv[1]);
}
