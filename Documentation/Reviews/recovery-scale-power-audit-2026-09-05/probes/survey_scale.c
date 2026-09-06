/* Execute the current plan builder with sparse and full-batch requests. */
#define main existing_survey_test_main
#include "../../../../firmware/tests/test_survey.c"
#undef main

static void probe(unsigned nodes, unsigned depth, bool full_batch)
{
    struct survey_identity id = identity();
    struct survey_graph graph = {0};
    struct survey_pair_request requests[SURVEY_MAX_PAIRS];
    struct survey_plan_build_result result;
    uint8_t hops[SURVEY_MAX_ANCHORS] = {0};
    unsigned count = 0;
    id.assignment.slot_span = nodes;
    id.assignment.max_hop_count = depth;
    graph.occupied_slot_mask = (UINT64_C(1) << nodes) - 1;
    graph.received_report_mask = graph.occupied_slot_mask;
    for (unsigned i = 0; i < nodes; ++i) {
        graph.reports[i].own_slot = i;
        hops[i] = depth;
        for (unsigned j = i + 1; j < nodes; ++j) connect(&graph, i, j);
        for (unsigned distance = 1; !full_batch && distance <= 2; ++distance) {
            requests[count++] = (struct survey_pair_request) {
                i, (i + distance) % nodes,
            };
        }
    }
    if (full_batch) {
        for (unsigned i = 0; i < nodes && count < SURVEY_MAX_PAIRS; ++i)
            for (unsigned j = i + 1; j < nodes && count < SURVEY_MAX_PAIRS; ++j)
                requests[count++] = (struct survey_pair_request) {i, j};
    }
    int ret = survey_build_plan(&id, &graph, hops, requests, count,
        survey_control_delivery_delay_ms(depth), 0, true, &result);
    printf("nodes=%u depth=%u requests=%u ret=%d pairs=%u waves=%u "
           "stride_ms=%u stop_ms=%u\n", nodes, depth, count, ret,
           result.plan.pair_count, result.plan.wave_count,
           survey_wave_stride_ms(depth), result.plan.self_stop_delay_ms);
    if (!full_batch) assert(ret == 0 && result.plan.pair_count == 60);
    if (full_batch && depth == 8) assert(ret == PROTO_ERR_NO_SPACE);
}

int main(void)
{
    probe(30, 1, false);
    probe(30, 3, false);
    probe(30, 8, false);
    probe(30, 3, true);
    probe(30, 8, true);
    probe(50, 8, true);
    return 0;
}
