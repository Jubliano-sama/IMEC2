#include "survey.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct survey_identity identity(void)
{
    struct survey_identity value = {
        .generation = 7u,
        .assignment = {
            .assignment_epoch = 11u,
            .table_command_seq = 13u,
            .slot_span = 8u,
            .max_hop_count = 3u,
        },
    };

    memset(value.assignment.table_commitment.bytes, 0x5au,
           sizeof(value.assignment.table_commitment.bytes));
    return value;
}

static void connect(struct survey_graph *graph, uint8_t first, uint8_t second)
{
    assert(survey_neighbor_bitmap_set(
        graph->reports[first].heard_bitmap, second));
    assert(survey_neighbor_bitmap_set(
        graph->reports[second].heard_bitmap, first));
}

static void test_timing_contract(void)
{
    assert(node_comm_bounded_control_apply_budget_ms(0u) == 0u);
    assert(node_comm_bounded_control_apply_budget_ms(1u) == 20000u);
    assert(node_comm_bounded_control_apply_budget_ms(5u) == 60000u);
    assert(survey_control_delivery_delay_ms(0u) == 0u);
    assert(survey_control_delivery_delay_ms(1u) == 2750u);
    assert(survey_control_delivery_delay_ms(2u) == 3290u);
    assert(survey_control_delivery_delay_ms(3u) == 3830u);
    assert(survey_control_delivery_delay_ms(4u) == 4370u);
    assert(survey_control_delivery_delay_ms(5u) == 4910u);
    assert(survey_control_delivery_delay_ms(6u) == 5450u);
    assert(survey_control_delivery_delay_ms(7u) == 5990u);
    assert(survey_control_delivery_delay_ms(8u) == 6530u);
    assert(survey_slot_span_include(0u, 0u) == 1u);
    assert(survey_slot_span_include(1u, 2u) == 3u);
    assert(survey_slot_span_include(3u, 1u) == 3u);
    assert(survey_slot_span_include(3u, SURVEY_MAX_ANCHORS) == 0u);
    assert(survey_neighbor_sequence_duration_ms(50u) == 50000u);
    assert(survey_neighbor_sequence_duration_ms(0u) == 0u);
    assert(survey_neighbor_beacon_offset_ms(0u) == 100u);
    assert(survey_neighbor_beacon_offset_ms(4u) == 740u);
    assert(survey_neighbor_beacon_offset_ms(5u) == UINT32_MAX);
    assert(survey_range_attempt_offset_ms(0u) == 100u);
    assert(survey_range_attempt_offset_ms(4u) == 420u);
    assert(survey_wave_stride_ms(1u) == 2200u);
    assert(survey_wave_stride_ms(3u) == 5950u);
    assert(survey_wave_stride_ms(5u) == 10700u);
    assert(survey_execution_duration_ms(2u, 3u) == 23800u);
    assert(survey_plan_fits_hard_cap(60000u, 100u, 5u));
    assert(survey_plan_fits_hard_cap(708600u, 100u, 5u));
    assert(!survey_plan_fits_hard_cap(708601u, 100u, 5u));
}

static void test_gateway_cleanup_timing_contract(void)
{
    const uint8_t max_hop_count = 3u;
    const uint8_t slot_span = 3u;
    const uint8_t wave_count = 3u;
    const uint64_t start_origin_ms = 0u;
    const uint64_t initial_self_stop_ms =
        start_origin_ms + SURVEY_INITIAL_SELF_EXPIRY_MS;
    const uint64_t wait_plan_timeout_ms = start_origin_ms +
        survey_control_delivery_delay_ms(max_hop_count) +
        survey_neighbor_sequence_duration_ms(slot_span) +
        SURVEY_RESULT_PREPARE_MS +
        survey_result_lane_duration_ms(max_hop_count) +
        SURVEY_HOST_PLAN_TIMEOUT_MS;
    const uint64_t abort_release_ms = wait_plan_timeout_ms +
        survey_control_delivery_delay_ms(max_hop_count);
    const uint64_t plan_origin_ms = wait_plan_timeout_ms -
        SURVEY_HOST_PLAN_TIMEOUT_MS;
    const uint64_t execution_start_ms = plan_origin_ms +
        survey_control_delivery_delay_ms(max_hop_count);
    const uint64_t plan_self_stop_ms = execution_start_ms +
        survey_execution_duration_ms(wave_count, max_hop_count);
    const uint64_t final_result_lane_edge_ms = execution_start_ms +
        ((uint64_t)wave_count + SURVEY_EXTRA_DRAIN_STRIDES - 1u) *
            survey_wave_stride_ms(max_hop_count) +
        SURVEY_RANGE_WAVE_MS + SURVEY_RESULT_PREPARE_MS +
        survey_result_lane_duration_ms(max_hop_count);

    assert(wait_plan_timeout_ms == 72120u);
    assert(abort_release_ms == 75950u);
    assert(final_result_lane_edge_ms == 45640u);
    assert(plan_self_stop_ms == 45700u);

    /* A queued ABORT has no all-anchor delivery quorum. Its flood deadline
     * lets receivers stop early, but cleanup remains owned until self-stop. */
    assert(abort_release_ms - wait_plan_timeout_ms ==
           survey_control_delivery_delay_ms(max_hop_count));
    assert(wait_plan_timeout_ms < abort_release_ms);
    assert(abort_release_ms < initial_self_stop_ms);
    assert(initial_self_stop_ms == 180000u);

    /* Normal collection cannot release the gateway 60 ms before the
     * anchors' exact planned self-stop. */
    assert(final_result_lane_edge_ms < plan_self_stop_ms);
    assert(plan_self_stop_ms - final_result_lane_edge_ms ==
           SURVEY_RADIO_GUARD_MS);
}

static void test_graph_and_plan(void)
{
    struct survey_identity id = identity();
    struct survey_graph graph = {0};
    const struct survey_pair_request requests[] = {
        {0u, 1u}, {5u, 6u}, {0u, 4u}, {1u, 0u},
        {5u, 5u}, {0u, 7u}, {0u, 2u},
    };
    uint8_t hops[SURVEY_MAX_ANCHORS] = {0};
    struct survey_plan_build_result built;
    uint8_t encoded[SURVEY_PLAN_MAX_WIRE_LEN];
    struct survey_plan decoded;
    size_t encoded_len;

    graph.occupied_slot_mask = UINT64_C(0xff);
    graph.received_report_mask = UINT64_C(0xff);
    for (uint8_t slot = 0u; slot < 8u; slot++) {
        graph.reports[slot].own_slot = slot;
        hops[slot] = (uint8_t)(1u + (slot % 3u));
    }
    connect(&graph, 0u, 1u);
    connect(&graph, 5u, 6u);
    connect(&graph, 0u, 4u);
    connect(&graph, 0u, 2u);
    /* A directed-only observation is retained but cannot become a pair. */
    assert(survey_neighbor_bitmap_set(graph.reports[0].heard_bitmap, 7u));

    assert(survey_build_plan(&id, &graph, hops, requests,
                             sizeof(requests) / sizeof(requests[0]),
                             2500u, 0u, true, &built) == PROTO_OK);
    assert(built.plan.pair_count == 4u);
    assert(built.skipped_count == 3u);
    assert(built.skipped[0].reason == SURVEY_PLAN_SKIP_DUPLICATE);
    assert(built.skipped[1].reason == SURVEY_PLAN_SKIP_SELF_PAIR);
    assert(built.skipped[2].reason == SURVEY_PLAN_SKIP_NOT_MUTUAL);
    /* Shallower hop responds; ties use the lower stable slot. */
    assert(built.plan.pairs[0].responder_slot == 0u);
    assert(built.plan.pairs[1].responder_slot == 6u);
    /* Pair 0-1 and 5-6 share a wave; 0-4 and 0-2 cannot. */
    assert(built.plan.pairs[0].wave_index == 0u);
    assert(built.plan.pairs[1].wave_index == 0u);
    assert(built.plan.pairs[2].wave_index == 1u);
    assert(built.plan.pairs[3].wave_index == 2u);
    assert(built.plan.wave_count == 3u);
    assert(survey_plan_commitment_valid(&built.plan));

    encoded_len = survey_plan_encode(&built.plan, encoded, sizeof(encoded));
    assert(encoded_len == SURVEY_PLAN_HEADER_WIRE_LEN +
                          4u * SURVEY_PLAN_PAIR_WIRE_LEN);
    assert(survey_plan_decode(encoded, encoded_len, &decoded) == PROTO_OK);
    assert(decoded.pair_count == built.plan.pair_count);
    assert(memcmp(decoded.commitment, built.plan.commitment,
                  sizeof(decoded.commitment)) == 0);
    encoded[encoded_len - 1u] ^= 1u;
    assert(survey_plan_decode(encoded, encoded_len, &decoded) ==
           PROTO_ERR_MALFORMED);
}

static void test_full_degree_and_missing_report(void)
{
    struct survey_identity id = identity();
    struct survey_graph graph = {0};
    struct survey_pair_request requests[SURVEY_MAX_DEGREE];
    uint8_t hops[SURVEY_MAX_ANCHORS] = {0};
    struct survey_plan_build_result built;

    graph.occupied_slot_mask = (UINT64_C(1) << SURVEY_MAX_ANCHORS) - 1u;
    graph.received_report_mask = graph.occupied_slot_mask;
    for (uint8_t slot = 0u; slot < SURVEY_MAX_ANCHORS; slot++) {
        graph.reports[slot].own_slot = slot;
        hops[slot] = 1u;
    }
    for (uint8_t i = 0u; i < SURVEY_MAX_DEGREE; i++) {
        requests[i] = (struct survey_pair_request){0u, (uint8_t)(i + 1u)};
        connect(&graph, 0u, (uint8_t)(i + 1u));
    }
    assert(survey_build_plan(&id, &graph, hops, requests,
                             SURVEY_MAX_DEGREE, 1000u, 0u, true,
                             &built) == PROTO_OK);
    assert(built.plan.pair_count == SURVEY_MAX_DEGREE);
    assert(built.skipped_count == 0u);

    graph.received_report_mask &= ~UINT64_C(1);
    assert(!survey_graph_mutual(&graph, 0u, 1u));
}

static void test_median_and_record_codecs(void)
{
    const int32_t odd[] = {1200, 1000, 1100, 900, 1300};
    const int32_t even[] = {1000, 1001};
    struct survey_range_result result;
    struct survey_range_result decoded;
    uint8_t encoded[SURVEY_RANGE_RESULT_WIRE_LEN];

    assert(survey_range_result_from_samples(2u, 4u, odd, 5u,
                                            &result) == PROTO_OK);
    assert(result.median_mm == 1100);
    assert(survey_range_status(&result) == SURVEY_RANGE_RESULT_USABLE);
    assert(survey_range_result_encode(&result, encoded) == sizeof(encoded));
    assert(survey_range_result_decode(encoded, sizeof(encoded),
                                      &decoded) == PROTO_OK);
    assert(decoded.median_mm == 1100);

    assert(survey_range_result_from_samples(3u, 5u, even, 2u,
                                            &result) == PROTO_OK);
    assert(result.median_mm == 1001);
    assert(survey_range_status(&result) ==
           SURVEY_RANGE_RESULT_INSUFFICIENT);
    assert(survey_range_result_from_samples(4u, 6u, NULL, 0u,
                                            &result) == PROTO_OK);
    assert(result.median_mm == SURVEY_NO_MEDIAN_MM);
    assert(survey_range_status(&result) == SURVEY_RANGE_RESULT_MISSING);
}

static void test_compact_signal_records(void)
{
    uint8_t levels[SURVEY_MAX_ANCHORS] = {0};
    bool seen[UINT8_MAX + 1u] = {false};
    uint16_t total = 0u;

    assert(survey_rsl_quantize_dbm(0) == 0u);
    assert(survey_rsl_quantize_dbm(-120) == 1u);
    assert(survey_rsl_quantize_dbm(-105) == 1u);
    assert(survey_rsl_quantize_dbm(-100) == 2u);
    assert(survey_rsl_quantize_dbm(-35) == 15u);
    assert(survey_rsl_quantize_dbm(-20) == 15u);
    assert(survey_rsl_level_dbm(1u) == -105);
    assert(survey_rsl_level_dbm(15u) == -35);
    assert(survey_rsl_level_dbm(0u) == 0);

    for (uint8_t slot = 0u; slot < SURVEY_MAX_ANCHORS; slot++) {
        uint8_t chunks = survey_signal_record_count_for_slot(slot);

        for (uint8_t target = 0u; target < slot; target++) {
            levels[target] = (uint8_t)(1u + target % 15u);
        }
        for (uint8_t chunk = 0u; chunk < chunks; chunk++) {
            struct survey_signal_record record;
            uint8_t owner;
            uint8_t base;
            uint8_t decoded[SURVEY_SIGNAL_LEVELS_PER_RECORD];

            assert(survey_signal_record_encode(slot, chunk, levels,
                                               &record) ==
                   SURVEY_SIGNAL_RECORD_WIRE_LEN);
            assert(!seen[record.bytes[0]]);
            seen[record.bytes[0]] = true;
            assert(survey_signal_record_decode(&record, &owner, &base,
                                               decoded) == PROTO_OK);
            assert(owner == slot);
            assert(base == chunk * SURVEY_SIGNAL_LEVELS_PER_RECORD);
            total++;
        }
    }
    assert(total == SURVEY_MAX_SIGNAL_RECORDS);
    assert(total == 112u);
}

int main(void)
{
    test_timing_contract();
    test_gateway_cleanup_timing_contract();
    test_graph_and_plan();
    test_full_degree_and_missing_report();
    test_median_and_record_codecs();
    test_compact_signal_records();
    puts("survey tests passed");
    return 0;
}
