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
    assert(survey_control_delivery_delay_ms(1u) == 12750u);
    assert(survey_control_delivery_delay_ms(2u) == 13290u);
    assert(survey_control_delivery_delay_ms(3u) == 13830u);
    assert(survey_control_delivery_delay_ms(4u) == 14370u);
    assert(survey_control_delivery_delay_ms(5u) == 14910u);
    assert(survey_control_delivery_delay_ms(6u) == 15450u);
    assert(survey_control_delivery_delay_ms(7u) == 15990u);
    assert(survey_control_delivery_delay_ms(8u) == 16530u);
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
                             2500u, &built) == PROTO_OK);
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

static void test_degree_cap_and_missing_report(void)
{
    struct survey_identity id = identity();
    struct survey_graph graph = {0};
    struct survey_pair_request requests[6];
    uint8_t hops[SURVEY_MAX_ANCHORS] = {0};
    struct survey_plan_build_result built;

    graph.occupied_slot_mask = UINT64_C(0x7f);
    graph.received_report_mask = UINT64_C(0x7f);
    for (uint8_t slot = 0u; slot < 7u; slot++) {
        graph.reports[slot].own_slot = slot;
        hops[slot] = 1u;
    }
    for (uint8_t i = 0u; i < 6u; i++) {
        requests[i] = (struct survey_pair_request){0u, (uint8_t)(i + 1u)};
        connect(&graph, 0u, (uint8_t)(i + 1u));
    }
    assert(survey_build_plan(&id, &graph, hops, requests, 6u, 1000u,
                             &built) == PROTO_OK);
    assert(built.plan.pair_count == 4u);
    assert(built.skipped_count == 2u);
    assert(built.skipped[0].reason == SURVEY_PLAN_SKIP_DEGREE_CAP);
    assert(built.skipped[1].reason == SURVEY_PLAN_SKIP_DEGREE_CAP);

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

int main(void)
{
    test_timing_contract();
    test_graph_and_plan();
    test_degree_cap_and_missing_report();
    test_median_and_record_codecs();
    puts("survey tests passed");
    return 0;
}
