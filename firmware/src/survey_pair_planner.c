#include "survey.h"

struct survey_pair_candidate {
    size_t peer_index;
    int8_t rssi_dbm;
    uint8_t quality;
    bool mutual;
};

struct survey_pair_vertices {
    const struct survey_reachability_report *reports;
    const struct survey_gateway_context *gateway_context;
    uint64_t anchor_ids[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t report_indices[SURVEY_GATEWAY_MAX_REPORTS];
};

struct survey_connect_candidate {
    struct survey_pair_candidate pair;
    size_t report_index;
    bool valid;
};

struct survey_pair_plan_output {
    struct survey_pair *full_pairs;
    struct survey_gateway_context *gateway_context;
    size_t pair_cap;
    bool *topology_complete;
};

struct survey_pair_cardinality_move {
    struct survey_pair_candidate added_first;
    struct survey_pair_candidate added_second;
    struct survey_pair_candidate removed;
    size_t remove_index;
    size_t remove_first;
    size_t remove_second;
    size_t add_first_peer;
    size_t add_second_peer;
    uint8_t added_peer_max_degree;
    uint8_t added_peer_degree_sum;
    bool valid;
};

struct survey_pair_connectivity_move {
    struct survey_pair_candidate added;
    struct survey_pair_candidate removed;
    size_t add_first;
    size_t add_second;
    size_t remove_index;
    size_t remove_first;
    size_t remove_second;
    uint8_t added_peer_max_degree;
    uint8_t added_peer_degree_sum;
    bool valid;
};

struct survey_pair_cardinality_budget {
    uint32_t candidate_evaluations;
    uint32_t candidate_limit;
    uint32_t *component_candidate_evaluations;
    uint32_t component_candidate_limit;
    uint32_t *total_candidate_evaluations;
    bool exhausted;
};

#define SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS \
    SURVEY_GATEWAY_MAX_PAIRS
#define SURVEY_PAIR_CARDINALITY_TRAIL_VERTEX_CAP \
    ((2u * SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS) + 2u)

struct survey_pair_cardinality_trail {
    uint8_t vertices[SURVEY_PAIR_CARDINALITY_TRAIL_VERTEX_CAP];
    uint8_t next_peer[SURVEY_PAIR_CARDINALITY_TRAIL_VERTEX_CAP];
    int8_t degree_delta[SURVEY_GATEWAY_MAX_REPORTS];
};

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS >=
               (SURVEY_GATEWAY_MAX_REPORTS * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u,
               "survey pair storage must hold the bounded topology");
_Static_assert(SURVEY_GATEWAY_MAX_REPORTS < UINT8_MAX,
               "survey report index must leave one synthetic sentinel");
#define SURVEY_PAIR_QUALITY_EXCHANGE_MAX SURVEY_GATEWAY_MAX_REPORTS
_Static_assert(SURVEY_PAIR_QUALITY_EXCHANGE_MAX <= 50u,
               "survey pair quality repair must stay deterministically bounded");
#define SURVEY_PAIR_CARDINALITY_AUGMENT_MAX SURVEY_GATEWAY_MAX_PAIRS
#define SURVEY_PAIR_CARDINALITY_CANDIDATE_EVAL_MAX \
    UINT32_C(50000)
/*
 * Each augmentation search is locally bounded by its phase envelope and
 * resets only after applying one +1 move. The containing candidate component
 * retains a 10000-evaluation allocation per vertex across every move and
 * phase. Since components partition at most 50 vertices, the 500000 global
 * guard cannot let an earlier component consume a later component's share.
 */
#define SURVEY_PAIR_CARDINALITY_COMPONENT_EVALS_PER_VERTEX \
    UINT32_C(10000)
#define SURVEY_PAIR_CARDINALITY_TOTAL_CANDIDATE_EVAL_MAX \
    (SURVEY_PAIR_CARDINALITY_COMPONENT_EVALS_PER_VERTEX * \
     SURVEY_GATEWAY_MAX_REPORTS)
_Static_assert(SURVEY_PAIR_CARDINALITY_AUGMENT_MAX == 150u,
               "survey cardinality iteration cap must be reviewed with K50 capacity");
_Static_assert(SURVEY_PAIR_CARDINALITY_CANDIDATE_EVAL_MAX == UINT32_C(50000),
               "survey cardinality per-component phase cap must remain explicit");
_Static_assert(SURVEY_GATEWAY_MAX_REPORTS == 50u &&
               SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR == 6u,
               "survey cardinality total-work envelope must be rederived");
_Static_assert(SURVEY_PAIR_CARDINALITY_TOTAL_CANDIDATE_EVAL_MAX ==
                   UINT32_C(500000),
               "survey cardinality total embedded work cap must remain explicit");
_Static_assert(SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS == 150u,
               "survey alternating-trail depth needs a fresh stack review");
_Static_assert(SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS < UINT8_MAX,
               "survey trail removal indices must fit one byte");
_Static_assert(SURVEY_GATEWAY_MAX_REPORTS <= INT8_MAX,
               "survey trail degree deltas must fit one signed byte");

static uint8_t survey_gateway_compact_entry_node_index(
    const struct survey_gateway_compact_reachability_entry *entry)
{
    return entry->peer_index & SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK;
}

static bool survey_report_find_peer(
    const struct survey_pair_vertices *vertices,
    size_t vertex_index,
    uint64_t peer_id,
    struct survey_reachability_entry *best)
{
    const uint8_t report_index = vertices->report_indices[vertex_index];
    bool found = false;

    if (best == NULL || report_index == UINT8_MAX) {
        return false;
    }
    if (vertices->gateway_context == NULL) {
        const struct survey_reachability_report *report =
            &vertices->reports[report_index];

        for (size_t i = 0u; i < report->entry_count; i++) {
            const struct survey_reachability_entry *entry =
                &report->entries[i];

            if (entry->peer_id == peer_id &&
                (!found || entry->quality > best->quality ||
                 (entry->quality == best->quality &&
                  entry->rssi_dbm > best->rssi_dbm))) {
                *best = *entry;
                found = true;
            }
        }
        return found;
    }

    const struct survey_gateway_context *context =
        vertices->gateway_context;
    const struct survey_gateway_report_slot *slot =
        &context->reports[report_index];
    const size_t entry_count = slot->metadata & 0x0fu;

    for (size_t i = 0u; i < entry_count; i++) {
        const struct survey_gateway_compact_reachability_entry *entry =
            &slot->entries[i];
        const uint8_t peer_index =
            survey_gateway_compact_entry_node_index(entry);

        if (peer_index < context->node_count &&
            context->node_ids[peer_index] == peer_id &&
            (!found || entry->quality > best->quality ||
             (entry->quality == best->quality &&
              entry->rssi_dbm > best->rssi_dbm))) {
            *best = (struct survey_reachability_entry) {
                .peer_id = context->node_ids[peer_index],
                .rssi_dbm = entry->rssi_dbm,
                .quality = entry->quality,
            };
            found = true;
        }
    }
    return found;
}

static bool survey_candidate_precedes(const struct survey_pair_candidate *left,
                                      const struct survey_pair_candidate *right,
                                      const struct survey_pair_vertices *ordered)
{
    if (left->mutual != right->mutual) {
        return left->mutual;
    }
    if (left->quality != right->quality) {
        return left->quality > right->quality;
    }
    if (left->rssi_dbm != right->rssi_dbm) {
        return left->rssi_dbm > right->rssi_dbm;
    }
    return ordered->anchor_ids[left->peer_index] <
           ordered->anchor_ids[right->peer_index];
}

static bool survey_pair_candidate_build(
    const struct survey_pair_vertices *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_candidate *candidate)
{
    struct survey_reachability_entry forward_entry;
    struct survey_reachability_entry reverse_entry;
    const bool forward = survey_report_find_peer(
        ordered, report_index, ordered->anchor_ids[peer_index],
        &forward_entry);
    const bool reverse = survey_report_find_peer(
        ordered, peer_index, ordered->anchor_ids[report_index],
        &reverse_entry);

    if (candidate == NULL || (!forward && !reverse)) {
        return false;
    }
    candidate->peer_index = peer_index;
    candidate->mutual = forward && reverse;
    if (candidate->mutual) {
        candidate->quality = forward_entry.quality < reverse_entry.quality ?
                             forward_entry.quality : reverse_entry.quality;
        candidate->rssi_dbm =
            forward_entry.rssi_dbm < reverse_entry.rssi_dbm ?
            forward_entry.rssi_dbm : reverse_entry.rssi_dbm;
    } else if (forward) {
        candidate->quality = forward_entry.quality;
        candidate->rssi_dbm = forward_entry.rssi_dbm;
    } else {
        candidate->quality = reverse_entry.quality;
        candidate->rssi_dbm = reverse_entry.rssi_dbm;
    }
    return true;
}

static size_t survey_component_root(size_t *parents, size_t index)
{
    size_t root = index;

    while (parents[root] != root) {
        root = parents[root];
    }
    while (parents[index] != index) {
        size_t next = parents[index];

        parents[index] = root;
        index = next;
    }
    return root;
}

static bool survey_connect_candidate_precedes(
    const struct survey_connect_candidate *left,
    const struct survey_connect_candidate *right,
    const uint8_t *degree,
    const struct survey_pair_vertices *ordered)
{
    const uint8_t left_max = degree[left->report_index] >
        degree[left->pair.peer_index] ? degree[left->report_index] :
                                       degree[left->pair.peer_index];
    const uint8_t right_max = degree[right->report_index] >
        degree[right->pair.peer_index] ? degree[right->report_index] :
                                        degree[right->pair.peer_index];
    const uint8_t left_sum = degree[left->report_index] +
        degree[left->pair.peer_index];
    const uint8_t right_sum = degree[right->report_index] +
        degree[right->pair.peer_index];

    if (!right->valid) {
        return true;
    }
    if (left_max != right_max) {
        return left_max < right_max;
    }
    if (left_sum != right_sum) {
        return left_sum < right_sum;
    }
    if (survey_candidate_precedes(&left->pair, &right->pair, ordered)) {
        return true;
    }
    if (survey_candidate_precedes(&right->pair, &left->pair, ordered)) {
        return false;
    }
    return ordered->anchor_ids[left->report_index] <
           ordered->anchor_ids[right->report_index];
}

static bool survey_fill_candidate_precedes(
    const struct survey_connect_candidate *left,
    const struct survey_connect_candidate *right,
    const uint8_t *degree,
    const struct survey_pair_vertices *ordered)
{
    const uint8_t left_max = degree[left->report_index] >
        degree[left->pair.peer_index] ? degree[left->report_index] :
                                       degree[left->pair.peer_index];
    const uint8_t right_max = degree[right->report_index] >
        degree[right->pair.peer_index] ? degree[right->report_index] :
                                        degree[right->pair.peer_index];
    const uint8_t left_sum = degree[left->report_index] +
        degree[left->pair.peer_index];
    const uint8_t right_sum = degree[right->report_index] +
        degree[right->pair.peer_index];

    if (!right->valid) {
        return true;
    }
    if (left->pair.mutual != right->pair.mutual) {
        return left->pair.mutual;
    }
    if (left->pair.quality != right->pair.quality) {
        return left->pair.quality > right->pair.quality;
    }
    if (left->pair.rssi_dbm != right->pair.rssi_dbm) {
        return left->pair.rssi_dbm > right->pair.rssi_dbm;
    }
    if (left_max != right_max) {
        return left_max < right_max;
    }
    if (left_sum != right_sum) {
        return left_sum < right_sum;
    }
    if (ordered->anchor_ids[left->report_index] !=
        ordered->anchor_ids[right->report_index]) {
        return ordered->anchor_ids[left->report_index] <
               ordered->anchor_ids[right->report_index];
    }
    return ordered->anchor_ids[left->pair.peer_index] <
           ordered->anchor_ids[right->pair.peer_index];
}

static int survey_append_planned_pair(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_pair_vertices *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    const uint64_t initiator_id = ordered->anchor_ids[report_index];
    const uint64_t responder_id = ordered->anchor_ids[peer_index];

    if (*count >= output->pair_cap) {
        return PROTO_ERR_NO_SPACE;
    }
    if (output->full_pairs != NULL) {
        output->full_pairs[*count] = (struct survey_pair) {
            .survey_id = survey_id,
            .initiator_id = initiator_id,
            .responder_id = responder_id,
            .sample_count = sample_count,
        };
    } else {
        uint8_t initiator_index = UINT8_MAX;
        uint8_t responder_index = UINT8_MAX;

        for (uint8_t i = 0u;
             i < output->gateway_context->node_count;
             i++) {
            if (output->gateway_context->node_ids[i] == initiator_id) {
                initiator_index = i;
            }
            if (output->gateway_context->node_ids[i] == responder_id) {
                responder_index = i;
            }
        }
        if (initiator_index == UINT8_MAX ||
            responder_index == UINT8_MAX ||
            initiator_index == responder_index) {
            return PROTO_ERR_MALFORMED;
        }
        output->gateway_context->pairs[*count] =
            (struct survey_gateway_pair_entry) {
                .initiator_index = initiator_index,
                .responder_index = responder_index,
            };
    }
    degree[report_index]++;
    degree[peer_index]++;
    (*count)++;
    return PROTO_OK;
}

static bool survey_pair_already_planned(
    const struct survey_pair_plan_output *output,
    size_t count,
    uint64_t first_id,
    uint64_t second_id)
{
    for (size_t i = 0u; i < count; i++) {
        uint64_t initiator_id;
        uint64_t responder_id;

        if (output->full_pairs != NULL) {
            initiator_id = output->full_pairs[i].initiator_id;
            responder_id = output->full_pairs[i].responder_id;
        } else {
            const struct survey_gateway_pair_entry *entry =
                &output->gateway_context->pairs[i];

            if (entry->initiator_index >=
                    output->gateway_context->node_count ||
                entry->responder_index >=
                    output->gateway_context->node_count) {
                return false;
            }
            initiator_id =
                output->gateway_context->node_ids[entry->initiator_index];
            responder_id =
                output->gateway_context->node_ids[entry->responder_index];
        }

        if ((initiator_id == first_id && responder_id == second_id) ||
            (initiator_id == second_id && responder_id == first_id)) {
            return true;
        }
    }
    return false;
}

static void survey_planned_pair_ids(
    const struct survey_pair_plan_output *output,
    size_t pair_index,
    uint64_t *first_id,
    uint64_t *second_id)
{
    if (output->full_pairs != NULL) {
        *first_id = output->full_pairs[pair_index].initiator_id;
        *second_id = output->full_pairs[pair_index].responder_id;
    } else {
        const struct survey_gateway_pair_entry *entry =
            &output->gateway_context->pairs[pair_index];

        *first_id =
            output->gateway_context->node_ids[entry->initiator_index];
        *second_id =
            output->gateway_context->node_ids[entry->responder_index];
    }
}

static void survey_planned_pair_replace(
    struct survey_pair_plan_output *output,
    size_t pair_index,
    uint64_t first_id,
    uint64_t second_id)
{
    const uint64_t initiator_id = first_id < second_id ? first_id : second_id;
    const uint64_t responder_id = first_id < second_id ? second_id : first_id;

    if (output->full_pairs != NULL) {
        output->full_pairs[pair_index].initiator_id = initiator_id;
        output->full_pairs[pair_index].responder_id = responder_id;
    } else {
        uint8_t initiator_index = UINT8_MAX;
        uint8_t responder_index = UINT8_MAX;

        for (uint8_t i = 0u;
             i < output->gateway_context->node_count;
             i++) {
            if (output->gateway_context->node_ids[i] == initiator_id) {
                initiator_index = i;
            }
            if (output->gateway_context->node_ids[i] == responder_id) {
                responder_index = i;
            }
        }
        output->gateway_context->pairs[pair_index] =
            (struct survey_gateway_pair_entry) {
                .initiator_index = initiator_index,
                .responder_index = responder_index,
            };
    }
}

static size_t survey_vertex_index(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    uint64_t anchor_id);

static bool survey_planned_topology_complete(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    const struct survey_pair_plan_output *output,
    size_t pair_count)
{
    size_t parents[SURVEY_GATEWAY_MAX_REPORTS];
    bool selected[SURVEY_GATEWAY_MAX_REPORTS] = {false};

    if (vertices == NULL || output == NULL ||
        vertex_count < 2u || pair_count == 0u) {
        return false;
    }
    for (size_t i = 0u; i < vertex_count; i++) {
        parents[i] = i;
    }
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;
        size_t first_root;
        size_t second_root;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);
        if (first >= vertex_count || second >= vertex_count) {
            return false;
        }
        selected[first] = true;
        selected[second] = true;
        first_root = survey_component_root(parents, first);
        second_root = survey_component_root(parents, second);
        parents[second_root] = first_root;
    }

    {
        size_t root = survey_component_root(parents, 0u);

        for (size_t i = 0u; i < vertex_count; i++) {
            if (!selected[i] ||
                survey_component_root(parents, i) != root) {
                return false;
            }
        }
    }
    return true;
}

static size_t survey_vertex_index(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    uint64_t anchor_id)
{
    for (size_t i = 0u; i < vertex_count; i++) {
        if (vertices->anchor_ids[i] == anchor_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int survey_candidate_strength_compare(
    const struct survey_pair_candidate *left,
    const struct survey_pair_candidate *right)
{
    if (left->mutual != right->mutual) {
        return left->mutual ? 1 : -1;
    }
    if (left->quality != right->quality) {
        return left->quality > right->quality ? 1 : -1;
    }
    if (left->rssi_dbm != right->rssi_dbm) {
        return left->rssi_dbm > right->rssi_dbm ? 1 : -1;
    }
    return 0;
}

static void survey_candidate_strength_sort_pair(
    struct survey_pair_candidate *first,
    struct survey_pair_candidate *second)
{
    if (survey_candidate_strength_compare(second, first) > 0) {
        const struct survey_pair_candidate swap = *first;

        *first = *second;
        *second = swap;
    }
}

static bool survey_candidate_pair_is_stronger(
    struct survey_pair_candidate added_first,
    struct survey_pair_candidate added_second,
    struct survey_pair_candidate removed_first,
    struct survey_pair_candidate removed_second)
{
    int comparison;

    survey_candidate_strength_sort_pair(&added_first, &added_second);
    survey_candidate_strength_sort_pair(&removed_first, &removed_second);
    comparison = survey_candidate_strength_compare(&added_first,
                                                   &removed_first);
    if (comparison != 0) {
        return comparison > 0;
    }
    return survey_candidate_strength_compare(&added_second,
                                             &removed_second) > 0;
}

static void survey_component_union(size_t *parents,
                                   size_t first,
                                   size_t second)
{
    const size_t first_root = survey_component_root(parents, first);
    const size_t second_root = survey_component_root(parents, second);

    if (first_root != second_root) {
        parents[second_root] = first_root;
    }
}

static bool survey_pair_cardinality_selected(
    uint8_t selected[SURVEY_GATEWAY_MAX_REPORTS]
                    [SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR],
    uint8_t *selected_count,
    size_t first,
    size_t second)
{
    for (size_t i = 0u; i < selected_count[first]; i++) {
        if (selected[first][i] == second) {
            return true;
        }
    }
    return false;
}

static void survey_pair_cardinality_select(
    uint8_t selected[SURVEY_GATEWAY_MAX_REPORTS]
                    [SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR],
    uint8_t *selected_count,
    size_t first,
    size_t second)
{
    selected[first][selected_count[first]++] = (uint8_t)second;
    selected[second][selected_count[second]++] = (uint8_t)first;
}

static bool survey_pair_cardinality_take_candidate(
    struct survey_pair_cardinality_budget *budget)
{
    if (budget->candidate_evaluations >= budget->candidate_limit ||
        *budget->component_candidate_evaluations >=
            budget->component_candidate_limit ||
        *budget->total_candidate_evaluations >=
            SURVEY_PAIR_CARDINALITY_TOTAL_CANDIDATE_EVAL_MAX) {
        budget->exhausted = true;
        return false;
    }
    budget->candidate_evaluations++;
    (*budget->component_candidate_evaluations)++;
    (*budget->total_candidate_evaluations)++;
    return true;
}

static uint8_t survey_pair_cardinality_component_root(
    uint8_t *parents,
    size_t index)
{
    uint8_t root = (uint8_t)index;

    while (parents[root] != root) {
        root = parents[root];
    }
    while (parents[index] != index) {
        const uint8_t next = parents[index];

        parents[index] = root;
        index = next;
    }
    return root;
}

static void survey_pair_cardinality_component_union(
    uint8_t *parents,
    size_t first,
    size_t second)
{
    const uint8_t first_root =
        survey_pair_cardinality_component_root(parents, first);
    const uint8_t second_root =
        survey_pair_cardinality_component_root(parents, second);

    if (first_root != second_root) {
        parents[second_root] = first_root;
    }
}

static void survey_pair_candidate_components(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    uint8_t *components)
{
    for (size_t i = 0u; i < vertex_count; i++) {
        components[i] = (uint8_t)i;
    }
    for (size_t first = 0u; first < vertex_count; first++) {
        for (size_t second = first + 1u;
             second < vertex_count;
             second++) {
            struct survey_pair_candidate candidate;

            if (survey_pair_candidate_build(vertices, first, second,
                                            &candidate)) {
                survey_pair_cardinality_component_union(
                    components, first, second);
            }
        }
    }
    for (size_t i = 0u; i < vertex_count; i++) {
        components[i] =
            survey_pair_cardinality_component_root(components, i);
    }
}

static uint32_t survey_pair_cardinality_phase_limit(
    size_t component_size,
    bool trail_search)
{
    uint64_t edge_limit =
        (component_size * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u;
    const uint64_t complete_graph_edge_count =
        (component_size * (component_size - 1u)) / 2u;
    uint64_t evaluation_limit;

    if (edge_limit > complete_graph_edge_count) {
        edge_limit = complete_graph_edge_count;
    }
    if (trail_search) {
        evaluation_limit = 1u;
        for (size_t i = 0u; i < 6u; i++) {
            evaluation_limit *= component_size;
        }
    } else {
        evaluation_limit =
            edge_limit * component_size * (component_size + 1u);
    }
    if (evaluation_limit >
        SURVEY_PAIR_CARDINALITY_CANDIDATE_EVAL_MAX) {
        evaluation_limit =
            SURVEY_PAIR_CARDINALITY_CANDIDATE_EVAL_MAX;
    }
    return (uint32_t)evaluation_limit;
}

static bool survey_pair_cardinality_move_precedes(
    const struct survey_pair_cardinality_move *left,
    const struct survey_pair_cardinality_move *right)
{
    struct survey_pair_candidate left_added_first = left->added_first;
    struct survey_pair_candidate left_added_second = left->added_second;
    struct survey_pair_candidate right_added_first = right->added_first;
    struct survey_pair_candidate right_added_second = right->added_second;
    int comparison;

    if (!right->valid) {
        return true;
    }
    if (left->added_peer_max_degree != right->added_peer_max_degree) {
        return left->added_peer_max_degree <
               right->added_peer_max_degree;
    }
    if (left->added_peer_degree_sum != right->added_peer_degree_sum) {
        return left->added_peer_degree_sum <
               right->added_peer_degree_sum;
    }

    survey_candidate_strength_sort_pair(&left_added_first,
                                        &left_added_second);
    survey_candidate_strength_sort_pair(&right_added_first,
                                        &right_added_second);
    comparison = survey_candidate_strength_compare(&left_added_first,
                                                   &right_added_first);
    if (comparison != 0) {
        return comparison > 0;
    }
    comparison = survey_candidate_strength_compare(&left_added_second,
                                                   &right_added_second);
    if (comparison != 0) {
        return comparison > 0;
    }
    comparison = survey_candidate_strength_compare(&left->removed,
                                                   &right->removed);
    if (comparison != 0) {
        return comparison < 0;
    }
    if (left->remove_first != right->remove_first) {
        return left->remove_first < right->remove_first;
    }
    if (left->remove_second != right->remove_second) {
        return left->remove_second < right->remove_second;
    }
    if (left->add_first_peer != right->add_first_peer) {
        return left->add_first_peer < right->add_first_peer;
    }
    if (left->add_second_peer != right->add_second_peer) {
        return left->add_second_peer < right->add_second_peer;
    }
    return left->remove_index < right->remove_index;
}

static bool survey_pair_cardinality_move_reconnects(
    uint8_t *parents,
    size_t remove_first,
    size_t remove_second,
    size_t add_first_peer,
    size_t add_second_peer)
{
    const uint8_t first_root =
        survey_pair_cardinality_component_root(parents, remove_first);
    const uint8_t second_root =
        survey_pair_cardinality_component_root(parents, remove_second);
    const uint8_t first_peer_root =
        survey_pair_cardinality_component_root(parents, add_first_peer);
    const uint8_t second_peer_root =
        survey_pair_cardinality_component_root(parents, add_second_peer);

    return first_root == second_root ||
           first_peer_root == second_root ||
           second_peer_root == first_root ||
           first_peer_root == second_peer_root;
}

static bool survey_pair_cardinality_same_edge(size_t first_left,
                                              size_t first_right,
                                              size_t second_left,
                                              size_t second_right)
{
    return (first_left == second_left && first_right == second_right) ||
           (first_left == second_right && first_right == second_left);
}

static int survey_augment_pair_cardinality_once(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    const uint8_t *candidate_components,
    uint8_t component_root,
    struct survey_pair_plan_output *output,
    size_t *pair_count,
    uint8_t *degree,
    struct survey_pair_cardinality_budget *budget,
    bool *augmented)
{
    uint8_t selected[SURVEY_GATEWAY_MAX_REPORTS]
                    [SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR] = {{0}};
    uint8_t selected_count[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    struct survey_pair_cardinality_move best = {0};

    *augmented = false;
    if (*pair_count >= output->pair_cap ||
        *pair_count >= SURVEY_GATEWAY_MAX_PAIRS ||
        budget->exhausted) {
        return PROTO_OK;
    }

    for (size_t i = 0u; i < *pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);
        if (first == SIZE_MAX || second == SIZE_MAX) {
            return PROTO_ERR_MALFORMED;
        }
        if (candidate_components[first] !=
            candidate_components[second]) {
            return PROTO_ERR_MALFORMED;
        }
        if (selected_count[first] >=
                SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
            selected_count[second] >=
                SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return PROTO_ERR_MALFORMED;
        }
        survey_pair_cardinality_select(selected, selected_count,
                                       first, second);
    }

    for (size_t remove = 0u; remove < *pair_count; remove++) {
        uint64_t remove_first_id;
        uint64_t remove_second_id;
        size_t remove_first;
        size_t remove_second;
        struct survey_pair_candidate removed;
        uint8_t parents[SURVEY_GATEWAY_MAX_REPORTS];

        survey_planned_pair_ids(output, remove, &remove_first_id,
                                &remove_second_id);
        remove_first = survey_vertex_index(
            vertices, vertex_count, remove_first_id);
        remove_second = survey_vertex_index(
            vertices, vertex_count, remove_second_id);
        if (remove_first == SIZE_MAX || remove_second == SIZE_MAX) {
            return PROTO_ERR_MALFORMED;
        }
        if (!survey_pair_candidate_build(vertices, remove_first,
                                         remove_second, &removed)) {
            return PROTO_ERR_MALFORMED;
        }
        if (candidate_components[remove_first] != component_root) {
            continue;
        }
        for (size_t i = 0u; i < vertex_count; i++) {
            parents[i] = (uint8_t)i;
        }
        for (size_t first = 0u; first < vertex_count; first++) {
            for (size_t i = 0u; i < selected_count[first]; i++) {
                const size_t second = selected[first][i];

                if (first < second &&
                    !survey_pair_cardinality_same_edge(
                        first, second, remove_first, remove_second)) {
                    survey_pair_cardinality_component_union(
                        parents, first, second);
                }
            }
        }

        for (size_t first_peer = 0u;
             first_peer < vertex_count;
             first_peer++) {
            struct survey_pair_candidate added_first;

            if (candidate_components[first_peer] != component_root) {
                continue;
            }
            if (!survey_pair_cardinality_take_candidate(budget)) {
                goto apply_best;
            }
            if (degree[first_peer] >=
                    SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                survey_pair_cardinality_selected(selected, selected_count,
                                                 remove_first,
                                                 first_peer) ||
                !survey_pair_candidate_build(vertices, remove_first,
                                             first_peer, &added_first)) {
                continue;
            }
            for (size_t second_peer = 0u;
                 second_peer < vertex_count;
                 second_peer++) {
                struct survey_pair_candidate added_second;
                struct survey_pair_cardinality_move candidate;
                uint8_t first_peer_after;
                uint8_t second_peer_after;

                if (candidate_components[second_peer] != component_root) {
                    continue;
                }
                if (!survey_pair_cardinality_take_candidate(budget)) {
                    goto apply_best;
                }
                if (degree[second_peer] >=
                        SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                    survey_pair_cardinality_selected(
                        selected, selected_count,
                        remove_second, second_peer) ||
                    !survey_pair_candidate_build(
                        vertices, remove_second, second_peer,
                        &added_second)) {
                    continue;
                }
                first_peer_after =
                    (uint8_t)(degree[first_peer] + 1u +
                              (first_peer == second_peer ? 1u : 0u));
                second_peer_after =
                    (uint8_t)(degree[second_peer] + 1u +
                              (first_peer == second_peer ? 1u : 0u));
                if (first_peer_after >
                        SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                    second_peer_after >
                        SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                    continue;
                }
                if (!survey_pair_cardinality_move_reconnects(
                        parents, remove_first, remove_second,
                        first_peer, second_peer)) {
                    continue;
                }
                candidate = (struct survey_pair_cardinality_move) {
                    .added_first = added_first,
                    .added_second = added_second,
                    .removed = removed,
                    .remove_index = remove,
                    .remove_first = remove_first,
                    .remove_second = remove_second,
                    .add_first_peer = first_peer,
                    .add_second_peer = second_peer,
                    .added_peer_max_degree =
                        first_peer_after > second_peer_after ?
                        first_peer_after : second_peer_after,
                    .added_peer_degree_sum =
                        first_peer == second_peer ?
                        first_peer_after :
                        (uint8_t)(first_peer_after + second_peer_after),
                    .valid = true,
                };
                if (survey_pair_cardinality_move_precedes(&candidate,
                                                          &best)) {
                    best = candidate;
                }
            }
        }
    }

apply_best:
    if (!best.valid) {
        return PROTO_OK;
    }

    degree[best.remove_first]--;
    degree[best.remove_second]--;
    size_t append_first = best.remove_second < best.add_second_peer ?
                          best.remove_second : best.add_second_peer;
    size_t append_second = best.remove_second < best.add_second_peer ?
                           best.add_second_peer : best.remove_second;
    int ret = survey_append_planned_pair(
        survey_id, sample_count, vertices, append_first, append_second,
        output, pair_count, degree);

    if (ret != PROTO_OK) {
        degree[best.remove_first]++;
        degree[best.remove_second]++;
        return ret;
    }
    survey_planned_pair_replace(
        output, best.remove_index, vertices->anchor_ids[best.remove_first],
        vertices->anchor_ids[best.add_first_peer]);
    degree[best.remove_first]++;
    degree[best.add_first_peer]++;
    *augmented = true;
    return PROTO_OK;
}

static bool survey_pair_cardinality_trail_contains_edge(
    const struct survey_pair_cardinality_trail *trail,
    size_t edge_count,
    bool added,
    size_t first,
    size_t second)
{
    for (size_t i = 0u; i < edge_count; i++) {
        const size_t edge_index = (2u * i) + (added ? 0u : 1u);

        if (survey_pair_cardinality_same_edge(
                trail->vertices[edge_index],
                trail->vertices[edge_index + 1u],
                first, second)) {
            return true;
        }
    }
    return false;
}

static bool survey_pair_cardinality_trail_degree_valid(
    const struct survey_pair_cardinality_trail *trail,
    const uint8_t *degree,
    size_t vertex_count)
{
    for (size_t i = 0u; i < vertex_count; i++) {
        const int final_degree =
            (int)degree[i] + (int)trail->degree_delta[i];

        if (final_degree < 0 ||
            final_degree >
                (int)SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return false;
        }
    }
    return true;
}

static bool survey_pair_cardinality_trail_preserves_components(
    const struct survey_pair_plan_output *output,
    size_t pair_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    const struct survey_pair_cardinality_trail *trail,
    size_t removed_count)
{
    uint8_t before[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t after[SURVEY_GATEWAY_MAX_REPORTS];

    for (size_t i = 0u; i < vertex_count; i++) {
        before[i] = (uint8_t)i;
        after[i] = (uint8_t)i;
    }
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);

        if (first == SIZE_MAX || second == SIZE_MAX) {
            return false;
        }
        survey_pair_cardinality_component_union(before, first, second);
        if (!survey_pair_cardinality_trail_contains_edge(
                trail, removed_count, false, first, second)) {
            survey_pair_cardinality_component_union(after, first, second);
        }
    }
    for (size_t i = 0u; i <= removed_count; i++) {
        const size_t edge_index = 2u * i;

        survey_pair_cardinality_component_union(
            after, trail->vertices[edge_index],
            trail->vertices[edge_index + 1u]);
    }
    for (size_t first = 0u; first < vertex_count; first++) {
        for (size_t second = first + 1u;
             second < vertex_count;
             second++) {
            if (survey_pair_cardinality_component_root(before, first) ==
                    survey_pair_cardinality_component_root(before, second) &&
                survey_pair_cardinality_component_root(after, first) !=
                    survey_pair_cardinality_component_root(after, second)) {
                return false;
            }
        }
    }
    return true;
}

static size_t survey_pair_cardinality_planned_index(
    const struct survey_pair_plan_output *output,
    size_t pair_count,
    const struct survey_pair_vertices *vertices,
    size_t first,
    size_t second)
{
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        if ((first_id == vertices->anchor_ids[first] &&
             second_id == vertices->anchor_ids[second]) ||
            (first_id == vertices->anchor_ids[second] &&
             second_id == vertices->anchor_ids[first])) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int survey_pair_cardinality_apply_trail(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    struct survey_pair_plan_output *output,
    size_t *pair_count,
    uint8_t *degree,
    const struct survey_pair_cardinality_trail *trail,
    size_t removed_count,
    bool *augmented)
{
    uint8_t remove_indices[SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS];
    uint8_t final_degree[SURVEY_GATEWAY_MAX_REPORTS];
    const size_t append_edge_index = 2u * removed_count;
    const size_t append_first =
        trail->vertices[append_edge_index] <
                trail->vertices[append_edge_index + 1u] ?
            trail->vertices[append_edge_index] :
            trail->vertices[append_edge_index + 1u];
    const size_t append_second =
        trail->vertices[append_edge_index] <
                trail->vertices[append_edge_index + 1u] ?
            trail->vertices[append_edge_index + 1u] :
            trail->vertices[append_edge_index];
    int ret;

    for (size_t i = 0u; i < removed_count; i++) {
        const size_t edge_index = (2u * i) + 1u;
        const size_t remove_index =
            survey_pair_cardinality_planned_index(
                output, *pair_count, vertices,
                trail->vertices[edge_index],
                trail->vertices[edge_index + 1u]);

        if (remove_index == SIZE_MAX) {
            return PROTO_ERR_MALFORMED;
        }
        remove_indices[i] = (uint8_t)remove_index;
    }
    for (size_t i = 0u; i < vertex_count; i++) {
        const int updated =
            (int)degree[i] + (int)trail->degree_delta[i];

        if (updated < 0 ||
            updated > (int)SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return PROTO_ERR_MALFORMED;
        }
        final_degree[i] = (uint8_t)updated;
    }
    ret = survey_append_planned_pair(
        survey_id, sample_count, vertices, append_first, append_second,
        output, pair_count, degree);
    if (ret != PROTO_OK) {
        return ret;
    }
    for (size_t i = 0u; i < removed_count; i++) {
        const size_t add_edge_index = 2u * i;

        survey_planned_pair_replace(
            output, remove_indices[i],
            vertices->anchor_ids[trail->vertices[add_edge_index]],
            vertices->anchor_ids[trail->vertices[add_edge_index + 1u]]);
    }
    for (size_t i = 0u; i < vertex_count; i++) {
        degree[i] = final_degree[i];
    }
    *augmented = true;
    return PROTO_OK;
}

static void survey_pair_cardinality_trail_undo_edge(
    struct survey_pair_cardinality_trail *trail,
    size_t edge_index)
{
    const size_t first = trail->vertices[edge_index];
    const size_t second = trail->vertices[edge_index + 1u];

    if ((edge_index & 1u) == 0u) {
        trail->degree_delta[first]--;
        trail->degree_delta[second]--;
    } else {
        trail->degree_delta[first]++;
        trail->degree_delta[second]++;
    }
}

static int survey_augment_pair_cardinality_trail_once(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    const uint8_t *candidate_components,
    uint8_t component_root,
    struct survey_pair_plan_output *output,
    size_t *pair_count,
    uint8_t *degree,
    struct survey_pair_cardinality_budget *budget,
    bool *augmented)
{
    uint8_t selected[SURVEY_GATEWAY_MAX_REPORTS]
                    [SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR] = {{0}};
    uint8_t selected_count[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    size_t component_selected_count = 0u;

    *augmented = false;
    if (*pair_count >= output->pair_cap ||
        *pair_count >= SURVEY_GATEWAY_MAX_PAIRS ||
        budget->exhausted) {
        return PROTO_OK;
    }
    for (size_t i = 0u; i < *pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);
        if (first == SIZE_MAX || second == SIZE_MAX ||
            candidate_components[first] !=
                candidate_components[second]) {
            return PROTO_ERR_MALFORMED;
        }
        if (selected_count[first] >=
                SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
            selected_count[second] >=
                SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return PROTO_ERR_MALFORMED;
        }
        survey_pair_cardinality_select(selected, selected_count,
                                       first, second);
        if (candidate_components[first] == component_root) {
            component_selected_count++;
        }
    }
    if (component_selected_count < 2u) {
        return PROTO_OK;
    }
    if (component_selected_count >
        SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS) {
        component_selected_count =
            SURVEY_PAIR_CARDINALITY_TRAIL_MAX_REMOVALS;
    }

    for (size_t start = 0u; start < vertex_count; start++) {
        struct survey_pair_cardinality_trail trail = {0};
        const size_t maximum_edge_count =
            (2u * component_selected_count) + 1u;
        size_t depth = 0u;

        if (candidate_components[start] != component_root ||
            degree[start] >=
                SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            continue;
        }
        trail.vertices[0] = (uint8_t)start;
        for (;;) {
            const size_t current = trail.vertices[depth];

            if (depth >= maximum_edge_count ||
                trail.next_peer[depth] >= vertex_count) {
                if (depth == 0u) {
                    break;
                }
                depth--;
                survey_pair_cardinality_trail_undo_edge(&trail, depth);
                continue;
            }

            const size_t next = trail.next_peer[depth]++;
            struct survey_pair_candidate candidate;

            if (next == current ||
                candidate_components[next] != component_root) {
                continue;
            }
            if (!survey_pair_cardinality_take_candidate(budget)) {
                return PROTO_OK;
            }
            if ((depth & 1u) == 0u) {
                const size_t added_index = depth / 2u;

                if (survey_pair_cardinality_selected(
                        selected, selected_count, current, next) ||
                    survey_pair_cardinality_trail_contains_edge(
                        &trail, added_index, true, current, next) ||
                    !survey_pair_candidate_build(
                        vertices, current, next, &candidate)) {
                    continue;
                }
                trail.degree_delta[current]++;
                trail.degree_delta[next]++;
            } else {
                const size_t removed_index = depth / 2u;

                if (!survey_pair_cardinality_selected(
                        selected, selected_count, current, next) ||
                    survey_pair_cardinality_trail_contains_edge(
                        &trail, removed_index, false, current, next)) {
                    continue;
                }
                trail.degree_delta[current]--;
                trail.degree_delta[next]--;
            }
            trail.vertices[depth + 1u] = (uint8_t)next;
            depth++;
            trail.next_peer[depth] = 0u;

            if ((depth & 1u) != 0u) {
                const size_t removed_count = depth / 2u;

                if (removed_count >= 2u &&
                    survey_pair_cardinality_trail_degree_valid(
                        &trail, degree, vertex_count) &&
                    survey_pair_cardinality_trail_preserves_components(
                        output, *pair_count, vertices, vertex_count,
                        &trail, removed_count)) {
                    return survey_pair_cardinality_apply_trail(
                        survey_id, sample_count, vertices, vertex_count,
                        output, pair_count, degree, &trail,
                        removed_count, augmented);
                }
            }
        }
    }
    return PROTO_OK;
}

static bool survey_pair_connectivity_move_precedes(
    const struct survey_pair_connectivity_move *left,
    const struct survey_pair_connectivity_move *right)
{
    int comparison;

    if (!right->valid) {
        return true;
    }
    if (left->added_peer_max_degree != right->added_peer_max_degree) {
        return left->added_peer_max_degree <
               right->added_peer_max_degree;
    }
    if (left->added_peer_degree_sum != right->added_peer_degree_sum) {
        return left->added_peer_degree_sum <
               right->added_peer_degree_sum;
    }
    comparison = survey_candidate_strength_compare(&left->added,
                                                   &right->added);
    if (comparison != 0) {
        return comparison > 0;
    }
    comparison = survey_candidate_strength_compare(&left->removed,
                                                   &right->removed);
    if (comparison != 0) {
        return comparison < 0;
    }
    if (left->add_first != right->add_first) {
        return left->add_first < right->add_first;
    }
    if (left->add_second != right->add_second) {
        return left->add_second < right->add_second;
    }
    if (left->remove_first != right->remove_first) {
        return left->remove_first < right->remove_first;
    }
    if (left->remove_second != right->remove_second) {
        return left->remove_second < right->remove_second;
    }
    return left->remove_index < right->remove_index;
}

static bool survey_pair_connectivity_degree_valid(
    const uint8_t *degree,
    size_t vertex_count,
    size_t add_first,
    size_t add_second,
    size_t remove_first,
    size_t remove_second,
    uint8_t *added_peer_max_degree,
    uint8_t *added_peer_degree_sum)
{
    uint8_t add_first_degree = 0u;
    uint8_t add_second_degree = 0u;

    for (size_t i = 0u; i < vertex_count; i++) {
        int updated = degree[i];

        if (i == add_first) {
            updated++;
        }
        if (i == add_second) {
            updated++;
        }
        if (i == remove_first) {
            updated--;
        }
        if (i == remove_second) {
            updated--;
        }
        if (updated < 0 ||
            updated > (int)SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
            return false;
        }
        if (i == add_first) {
            add_first_degree = (uint8_t)updated;
        }
        if (i == add_second) {
            add_second_degree = (uint8_t)updated;
        }
    }
    *added_peer_max_degree =
        add_first_degree > add_second_degree ?
        add_first_degree : add_second_degree;
    *added_peer_degree_sum =
        (uint8_t)(add_first_degree + add_second_degree);
    return true;
}

static bool survey_pair_connectivity_removal_preserves_component(
    const struct survey_pair_plan_output *output,
    size_t pair_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    size_t remove_index,
    size_t remove_first,
    size_t remove_second)
{
    uint8_t parents[SURVEY_GATEWAY_MAX_REPORTS];

    for (size_t i = 0u; i < vertex_count; i++) {
        parents[i] = (uint8_t)i;
    }
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;

        if (i == remove_index) {
            continue;
        }
        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);
        if (first == SIZE_MAX || second == SIZE_MAX) {
            return false;
        }
        survey_pair_cardinality_component_union(parents, first, second);
    }
    return survey_pair_cardinality_component_root(parents, remove_first) ==
           survey_pair_cardinality_component_root(parents, remove_second);
}

/*
 * Greedy fill can spend an endpoint's last degree on an internal cycle and
 * leave a candidate bridge unavailable. Replace one non-bridge selected edge
 * with that bridge: cardinality stays fixed, every existing selected component
 * stays connected, and two selected components merge. Repeating is bounded by
 * the vertex count because each successful move removes one component.
 */
static int survey_reconnect_pair_components_once(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    const uint8_t *candidate_components,
    struct survey_pair_plan_output *output,
    size_t pair_count,
    uint8_t *degree,
    bool *reconnected)
{
    uint8_t parents[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t pair_first[SURVEY_GATEWAY_MAX_PAIRS];
    uint8_t pair_second[SURVEY_GATEWAY_MAX_PAIRS];
    uint8_t removal_preserves[(SURVEY_GATEWAY_MAX_PAIRS + 7u) / 8u] = {0};
    struct survey_pair_connectivity_move best = {0};
    bool component_split = false;

    *reconnected = false;
    if (pair_count > SURVEY_GATEWAY_MAX_PAIRS) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t i = 0u; i < vertex_count; i++) {
        parents[i] = (uint8_t)i;
    }
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first;
        size_t second;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first = survey_vertex_index(vertices, vertex_count, first_id);
        second = survey_vertex_index(vertices, vertex_count, second_id);
        if (first == SIZE_MAX || second == SIZE_MAX) {
            return PROTO_ERR_MALFORMED;
        }
        pair_first[i] = (uint8_t)first;
        pair_second[i] = (uint8_t)second;
        survey_pair_cardinality_component_union(parents, first, second);
    }
    for (size_t first = 0u;
         first < vertex_count && !component_split;
         first++) {
        for (size_t second = 0u; second < first; second++) {
            if (candidate_components[first] ==
                    candidate_components[second] &&
                survey_pair_cardinality_component_root(parents, first) !=
                    survey_pair_cardinality_component_root(
                        parents, second)) {
                component_split = true;
                break;
            }
        }
    }
    if (!component_split) {
        return PROTO_OK;
    }
    for (size_t remove = 0u; remove < pair_count; remove++) {
        if (survey_pair_connectivity_removal_preserves_component(
                output, pair_count, vertices, vertex_count, remove,
                pair_first[remove], pair_second[remove])) {
            removal_preserves[remove / 8u] |=
                (uint8_t)(1u << (remove % 8u));
        }
    }

    for (size_t add_first = 0u;
         add_first < vertex_count;
         add_first++) {
        for (size_t add_second = add_first + 1u;
             add_second < vertex_count;
             add_second++) {
            struct survey_pair_candidate added;

            if (candidate_components[add_first] !=
                    candidate_components[add_second] ||
                survey_pair_cardinality_component_root(parents, add_first) ==
                    survey_pair_cardinality_component_root(
                        parents, add_second) ||
                !survey_pair_candidate_build(
                    vertices, add_first, add_second, &added)) {
                continue;
            }
            for (size_t remove = 0u; remove < pair_count; remove++) {
                const size_t remove_first = pair_first[remove];
                const size_t remove_second = pair_second[remove];
                struct survey_pair_candidate removed;
                struct survey_pair_connectivity_move candidate = {
                    .added = added,
                    .add_first = add_first,
                    .add_second = add_second,
                    .remove_index = remove,
                    .valid = true,
                };

                candidate.remove_first = remove_first;
                candidate.remove_second = remove_second;
                if ((removal_preserves[remove / 8u] &
                     (uint8_t)(1u << (remove % 8u))) == 0u ||
                    !survey_pair_connectivity_degree_valid(
                        degree, vertex_count, add_first, add_second,
                        remove_first, remove_second,
                        &candidate.added_peer_max_degree,
                        &candidate.added_peer_degree_sum) ||
                    !survey_pair_candidate_build(
                        vertices, remove_first, remove_second,
                        &removed)) {
                    continue;
                }
                candidate.removed = removed;
                if (survey_pair_connectivity_move_precedes(
                        &candidate, &best)) {
                    best = candidate;
                }
            }
        }
    }
    if (!best.valid) {
        return PROTO_OK;
    }

    degree[best.remove_first]--;
    degree[best.remove_second]--;
    degree[best.add_first]++;
    degree[best.add_second]++;
    survey_planned_pair_replace(
        output, best.remove_index, vertices->anchor_ids[best.add_first],
        vertices->anchor_ids[best.add_second]);
    *reconnected = true;
    return PROTO_OK;
}

static bool survey_quality_exchange_preserves_components(
    const struct survey_pair_plan_output *output,
    size_t pair_count,
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    size_t remove_first,
    size_t remove_second,
    size_t add_first_left,
    size_t add_first_right,
    size_t add_second_left,
    size_t add_second_right)
{
    size_t before[SURVEY_GATEWAY_MAX_REPORTS];
    size_t after[SURVEY_GATEWAY_MAX_REPORTS];

    for (size_t i = 0u; i < vertex_count; i++) {
        before[i] = i;
        after[i] = i;
    }
    for (size_t i = 0u; i < pair_count; i++) {
        uint64_t first_id;
        uint64_t second_id;
        size_t first_index;
        size_t second_index;

        survey_planned_pair_ids(output, i, &first_id, &second_id);
        first_index = survey_vertex_index(vertices, vertex_count, first_id);
        second_index = survey_vertex_index(vertices, vertex_count, second_id);
        if (first_index == SIZE_MAX || second_index == SIZE_MAX) {
            return false;
        }
        survey_component_union(before, first_index, second_index);
        if (i != remove_first && i != remove_second) {
            survey_component_union(after, first_index, second_index);
        }
    }
    survey_component_union(after, add_first_left, add_first_right);
    survey_component_union(after, add_second_left, add_second_right);

    for (size_t i = 0u; i < vertex_count; i++) {
        for (size_t j = i + 1u; j < vertex_count; j++) {
            if (survey_component_root(before, i) ==
                    survey_component_root(before, j) &&
                survey_component_root(after, i) !=
                    survey_component_root(after, j)) {
                return false;
            }
        }
    }
    return true;
}

static bool survey_improve_pair_quality_once(
    const struct survey_pair_vertices *vertices,
    size_t vertex_count,
    struct survey_pair_plan_output *output,
    size_t pair_count)
{
    for (size_t first = 0u; first < vertex_count; first++) {
        for (size_t second = first + 1u; second < vertex_count; second++) {
            struct survey_pair_candidate added_first;

            if (survey_pair_already_planned(
                    output, pair_count, vertices->anchor_ids[first],
                    vertices->anchor_ids[second]) ||
                !survey_pair_candidate_build(vertices, first, second,
                                             &added_first)) {
                continue;
            }
            for (size_t remove_first = 0u;
                 remove_first < pair_count;
                 remove_first++) {
                uint64_t remove_first_left_id;
                uint64_t remove_first_right_id;
                size_t remove_first_left;
                size_t remove_first_right;
                size_t first_other;

                survey_planned_pair_ids(
                    output, remove_first, &remove_first_left_id,
                    &remove_first_right_id);
                remove_first_left = survey_vertex_index(
                    vertices, vertex_count, remove_first_left_id);
                remove_first_right = survey_vertex_index(
                    vertices, vertex_count, remove_first_right_id);
                if (remove_first_left == SIZE_MAX ||
                    remove_first_right == SIZE_MAX) {
                    return false;
                }
                if (remove_first_left == first) {
                    first_other = remove_first_right;
                } else if (remove_first_right == first) {
                    first_other = remove_first_left;
                } else {
                    continue;
                }

                for (size_t remove_second = 0u;
                     remove_second < pair_count;
                     remove_second++) {
                    struct survey_pair_candidate added_second;
                    struct survey_pair_candidate removed_first;
                    struct survey_pair_candidate removed_second;
                    uint64_t remove_second_left_id;
                    uint64_t remove_second_right_id;
                    size_t remove_second_left;
                    size_t remove_second_right;
                    size_t second_other;
                    size_t add_second_left;
                    size_t add_second_right;

                    if (remove_second == remove_first) {
                        continue;
                    }
                    survey_planned_pair_ids(
                        output, remove_second, &remove_second_left_id,
                        &remove_second_right_id);
                    remove_second_left = survey_vertex_index(
                        vertices, vertex_count, remove_second_left_id);
                    remove_second_right = survey_vertex_index(
                        vertices, vertex_count, remove_second_right_id);
                    if (remove_second_left == SIZE_MAX ||
                        remove_second_right == SIZE_MAX) {
                        return false;
                    }
                    if (remove_second_left == second) {
                        second_other = remove_second_right;
                    } else if (remove_second_right == second) {
                        second_other = remove_second_left;
                    } else {
                        continue;
                    }
                    if (first_other == second_other) {
                        continue;
                    }
                    add_second_left = first_other < second_other ?
                                      first_other : second_other;
                    add_second_right = first_other < second_other ?
                                       second_other : first_other;
                    if (survey_pair_already_planned(
                            output, pair_count,
                            vertices->anchor_ids[add_second_left],
                            vertices->anchor_ids[add_second_right]) ||
                        !survey_pair_candidate_build(
                            vertices, add_second_left, add_second_right,
                            &added_second) ||
                        !survey_pair_candidate_build(
                            vertices, remove_first_left,
                            remove_first_right, &removed_first) ||
                        !survey_pair_candidate_build(
                            vertices, remove_second_left,
                            remove_second_right, &removed_second) ||
                        !survey_candidate_pair_is_stronger(
                            added_first, added_second,
                            removed_first, removed_second) ||
                        !survey_quality_exchange_preserves_components(
                            output, pair_count, vertices, vertex_count,
                            remove_first, remove_second, first, second,
                            add_second_left, add_second_right)) {
                        continue;
                    }
                    survey_planned_pair_replace(
                        output, remove_first, vertices->anchor_ids[first],
                        vertices->anchor_ids[second]);
                    survey_planned_pair_replace(
                        output, remove_second,
                        vertices->anchor_ids[add_second_left],
                        vertices->anchor_ids[add_second_right]);
                    return true;
                }
            }
        }
    }
    return false;
}

static int survey_connect_report_forest(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_pair_vertices *ordered,
    size_t vertex_count,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    size_t parents[SURVEY_GATEWAY_MAX_REPORTS];

    for (size_t i = 0u; i < vertex_count; i++) {
        parents[i] = i;
    }
    for (size_t edge = 0u; edge + 1u < vertex_count; edge++) {
        struct survey_connect_candidate best = {0};

        for (size_t i = 0u; i < vertex_count; i++) {
            if (degree[i] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            for (size_t j = i + 1u; j < vertex_count; j++) {
                struct survey_connect_candidate candidate = {
                    .report_index = i,
                    .valid = true,
                };

                if (degree[j] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                    survey_component_root(parents, i) ==
                        survey_component_root(parents, j) ||
                    !survey_pair_candidate_build(ordered, i, j,
                                                 &candidate.pair)) {
                    continue;
                }
                if (survey_connect_candidate_precedes(&candidate, &best,
                                                      degree, ordered)) {
                    best = candidate;
                }
            }
        }
        if (!best.valid) {
            break;
        }
        int ret = survey_append_planned_pair(
            survey_id, sample_count, ordered, best.report_index,
            best.pair.peer_index, output, count, degree);
        if (ret != PROTO_OK) {
            return ret;
        }
        size_t first_root = survey_component_root(parents, best.report_index);
        size_t second_root = survey_component_root(parents,
                                                   best.pair.peer_index);

        parents[second_root] = first_root;
    }
    return PROTO_OK;
}

static bool survey_vertex_contains(const struct survey_pair_vertices *vertices,
                                   size_t vertex_count,
                                   uint64_t anchor_id)
{
    for (size_t i = 0u; i < vertex_count; i++) {
        if (vertices->anchor_ids[i] == anchor_id) {
            return true;
        }
    }
    return false;
}

static void survey_vertices_sort(struct survey_pair_vertices *vertices,
                                 size_t vertex_count)
{
    for (size_t i = 1u; i < vertex_count; i++) {
        const uint64_t anchor_id = vertices->anchor_ids[i];
        const uint8_t report_index = vertices->report_indices[i];
        size_t insert = i;

        while (insert > 0u &&
               vertices->anchor_ids[insert - 1u] > anchor_id) {
            vertices->anchor_ids[insert] =
                vertices->anchor_ids[insert - 1u];
            vertices->report_indices[insert] =
                vertices->report_indices[insert - 1u];
            insert--;
        }
        vertices->anchor_ids[insert] = anchor_id;
        vertices->report_indices[insert] = report_index;
    }
}

static int survey_gateway_vertices_build(
    const struct survey_gateway_context *context,
    struct survey_pair_vertices *vertices,
    size_t *vertex_count)
{
    int ret = survey_gateway_context_validate(context);

    if (ret != PROTO_OK) {
        return ret;
    }
    for (size_t node_index = 0u;
         node_index < context->node_count;
         node_index++) {
        const struct survey_gateway_report_slot *slot =
            &context->reports[node_index];

        if (slot->metadata == UINT8_MAX) {
            continue;
        }
        vertices->anchor_ids[*vertex_count] =
            context->node_ids[node_index];
        vertices->report_indices[*vertex_count] = (uint8_t)node_index;
        (*vertex_count)++;
    }

    for (size_t node_index = 0u;
         node_index < context->node_count;
         node_index++) {
        const struct survey_gateway_report_slot *slot =
            &context->reports[node_index];
        const size_t entry_count = slot->metadata & 0x0fu;

        if (slot->metadata == UINT8_MAX) {
            continue;
        }
        for (size_t i = 0u; i < entry_count; i++) {
            const uint64_t peer_id =
                context->node_ids[
                    survey_gateway_compact_entry_node_index(
                        &slot->entries[i])];

            if (survey_vertex_contains(vertices, *vertex_count, peer_id)) {
                continue;
            }
            if (*vertex_count >= SURVEY_GATEWAY_MAX_REPORTS) {
                return PROTO_ERR_NO_SPACE;
            }
            vertices->anchor_ids[*vertex_count] = peer_id;
            vertices->report_indices[*vertex_count] = UINT8_MAX;
            (*vertex_count)++;
        }
    }
    return PROTO_OK;
}

static int survey_plan_pairs_from_reachability_into(
    uint32_t survey_id,
    const struct survey_reachability_report *reports,
    size_t report_count,
    const struct survey_gateway_context *gateway_context,
    uint16_t sample_count,
    struct survey_pair_plan_output *output,
    size_t *pair_count)
{
    struct survey_pair_vertices ordered = {
        .reports = reports,
        .gateway_context = gateway_context,
    };
    uint8_t candidate_components[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    uint8_t degree[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    uint32_t cardinality_candidate_evaluations = 0u;
    size_t vertex_count = 0u;
    size_t count = 0u;

    if (output == NULL || pair_count == NULL ||
        (reports == NULL) == (gateway_context == NULL) ||
        (output->full_pairs == NULL) ==
            (output->gateway_context == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (survey_id == 0u || !survey_sample_count_valid(sample_count)) {
        return PROTO_ERR_MALFORMED;
    }
    if (reports != NULL && report_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }

    *pair_count = 0u;
    if (output->topology_complete != NULL) {
        *output->topology_complete = false;
    }
    if (gateway_context != NULL) {
        int ret = survey_gateway_vertices_build(
            gateway_context, &ordered, &vertex_count);

        if (ret != PROTO_OK) {
            return ret;
        }
    } else {
        for (size_t i = 0u; i < report_count; i++) {
            const struct survey_reachability_report *report = &reports[i];

            if (report->anchor_id == 0u ||
                (report->entries == NULL && report->entry_count != 0u)) {
                return PROTO_ERR_MALFORMED;
            }
            if (survey_vertex_contains(&ordered, vertex_count,
                                       report->anchor_id)) {
                return PROTO_ERR_MALFORMED;
            }
            for (size_t j = 0u; j < report->entry_count; j++) {
                const struct survey_reachability_entry *entry =
                    &report->entries[j];
                int ret;

                ret = survey_reachability_entry_validate(entry);
                if (ret != PROTO_OK) {
                    return ret;
                }
                if (entry->peer_id == report->anchor_id) {
                    return PROTO_ERR_MALFORMED;
                }
            }
            ordered.anchor_ids[vertex_count] = report->anchor_id;
            ordered.report_indices[vertex_count] = (uint8_t)i;
            vertex_count++;
        }

        for (size_t i = 0u; i < report_count; i++) {
            const struct survey_reachability_report *report = &reports[i];

            for (size_t j = 0u; j < report->entry_count; j++) {
                const uint64_t peer_id = report->entries[j].peer_id;

                if (survey_vertex_contains(&ordered, vertex_count, peer_id)) {
                    continue;
                }
                if (vertex_count >= SURVEY_GATEWAY_MAX_REPORTS) {
                    return PROTO_ERR_NO_SPACE;
                }
                ordered.anchor_ids[vertex_count] = peer_id;
                ordered.report_indices[vertex_count] = UINT8_MAX;
                vertex_count++;
            }
        }
    }
    survey_vertices_sort(&ordered, vertex_count);
    survey_pair_candidate_components(
        &ordered, vertex_count, candidate_components);

    int ret = survey_connect_report_forest(survey_id, sample_count, &ordered,
                                           vertex_count, output,
                                           &count, degree);
    if (ret != PROTO_OK) {
        return ret;
    }

    for (;;) {
        struct survey_connect_candidate best = {0};

        for (size_t i = 0u; i < vertex_count; i++) {
            if (degree[i] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            for (size_t j = i + 1u; j < vertex_count; j++) {
                struct survey_connect_candidate candidate = {
                    .report_index = i,
                    .valid = true,
                };

                if (degree[j] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                    survey_pair_already_planned(
                        output, count, ordered.anchor_ids[i],
                        ordered.anchor_ids[j]) ||
                    !survey_pair_candidate_build(&ordered, i, j,
                                                 &candidate.pair)) {
                    continue;
                }
                if (survey_fill_candidate_precedes(
                        &candidate, &best, degree, &ordered)) {
                    best = candidate;
                }
            }
        }
        if (!best.valid) {
            break;
        }
        ret = survey_append_planned_pair(
            survey_id, sample_count, &ordered, best.report_index,
            best.pair.peer_index, output, &count, degree);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    size_t cardinality_limit =
        (vertex_count * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u;

    if (cardinality_limit > output->pair_cap) {
        cardinality_limit = output->pair_cap;
    }
    for (size_t component = 0u;
         component < vertex_count && count < cardinality_limit;
         component++) {
        size_t component_size = 0u;
        uint32_t component_candidate_evaluations = 0u;
        struct survey_pair_cardinality_budget one_for_two_budget = {
            .component_candidate_evaluations =
                &component_candidate_evaluations,
            .total_candidate_evaluations =
                &cardinality_candidate_evaluations,
        };
        struct survey_pair_cardinality_budget trail_budget = {
            .component_candidate_evaluations =
                &component_candidate_evaluations,
            .total_candidate_evaluations =
                &cardinality_candidate_evaluations,
        };

        for (size_t i = 0u; i < vertex_count; i++) {
            if (candidate_components[i] == component) {
                component_size++;
            }
        }
        if (component_size < 2u) {
            continue;
        }
        one_for_two_budget.component_candidate_limit =
            (uint32_t)component_size *
            SURVEY_PAIR_CARDINALITY_COMPONENT_EVALS_PER_VERTEX;
        trail_budget.component_candidate_limit =
            one_for_two_budget.component_candidate_limit;
        one_for_two_budget.candidate_limit =
            survey_pair_cardinality_phase_limit(component_size, false);
        trail_budget.candidate_limit =
            survey_pair_cardinality_phase_limit(component_size, true);

        for (size_t augmentation = 0u;
             augmentation < SURVEY_PAIR_CARDINALITY_AUGMENT_MAX &&
             count < cardinality_limit;
             augmentation++) {
            bool augmented;

            ret = survey_augment_pair_cardinality_once(
                survey_id, sample_count, &ordered, vertex_count,
                candidate_components, (uint8_t)component, output,
                &count, degree, &one_for_two_budget, &augmented);
            if (ret != PROTO_OK) {
                return ret;
            }
            if (!augmented) {
                ret = survey_augment_pair_cardinality_trail_once(
                    survey_id, sample_count, &ordered, vertex_count,
                    candidate_components, (uint8_t)component, output,
                    &count, degree, &trail_budget, &augmented);
                if (ret != PROTO_OK) {
                    return ret;
                }
            }
            if (!augmented) {
                break;
            }
            one_for_two_budget.candidate_evaluations = 0u;
            one_for_two_budget.exhausted = false;
            trail_budget.candidate_evaluations = 0u;
            trail_budget.exhausted = false;
        }
    }
    for (size_t repair = 0u;
         repair < SURVEY_GATEWAY_MAX_REPORTS;
         repair++) {
        bool reconnected;

        ret = survey_reconnect_pair_components_once(
            &ordered, vertex_count, candidate_components, output, count,
            degree, &reconnected);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (!reconnected) {
            break;
        }
    }
    for (size_t repair = 0u;
         repair < SURVEY_PAIR_QUALITY_EXCHANGE_MAX;
         repair++) {
        if (!survey_improve_pair_quality_once(&ordered, vertex_count,
                                              output, count)) {
            break;
        }
    }

    *pair_count = count;
    if (output->topology_complete != NULL) {
        *output->topology_complete =
            survey_planned_topology_complete(
                &ordered, vertex_count, output, count);
    }
    return PROTO_OK;
}

int survey_plan_pairs_from_reachability(uint32_t survey_id,
                                        const struct survey_reachability_report *reports,
                                        size_t report_count,
                                        uint16_t sample_count,
                                        struct survey_pair *pairs,
                                        size_t pair_cap,
                                        size_t *pair_count)
{
    struct survey_pair_plan_output output = {
        .full_pairs = pairs,
        .pair_cap = pair_cap,
    };

    return survey_plan_pairs_from_reachability_into(survey_id,
                                                    reports,
                                                    report_count,
                                                    NULL,
                                                    sample_count,
                                                    &output,
                                                    pair_count);
}

static int survey_plan_pairs_into_gateway_context(
    struct survey_gateway_context *context)
{
    size_t pair_count = 0u;
    struct survey_pair_plan_output output = {
        .gateway_context = context,
        .pair_cap = SURVEY_GATEWAY_MAX_PAIRS,
        .topology_complete = &context->topology_complete,
    };
    int ret;

    ret = survey_plan_pairs_from_reachability_into(
        context->survey_id,
        NULL,
        0u,
        context,
        context->sample_count,
        &output,
        &pair_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (pair_count > SURVEY_GATEWAY_MAX_PAIRS) {
        return PROTO_ERR_NO_SPACE;
    }
    context->pair_count = (uint8_t)pair_count;
    return PROTO_OK;
}

int survey_gateway_plan_pairs(struct survey_gateway_context *context)
{
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u) {
        return PROTO_ERR_STALE;
    }

    ret = survey_plan_pairs_into_gateway_context(context);
    if (ret != PROTO_OK) {
        context->pairs_planned = false;
        context->topology_complete = false;
        context->pair_count = 0u;
        return ret;
    }
    context->pairs_planned = true;
    return PROTO_OK;
}

static bool survey_gateway_round_report_has_peer(
    const struct survey_gateway_report_slot *slot,
    uint8_t peer_index)
{
    const size_t entry_count = slot->metadata & 0x0fu;

    for (size_t i = 0u; i < entry_count; i++) {
        if (survey_gateway_compact_entry_node_index(
                &slot->entries[i]) == peer_index) {
            return true;
        }
    }
    return false;
}

static bool survey_gateway_round_reports_share_peer(
    const struct survey_gateway_report_slot *first,
    const struct survey_gateway_report_slot *second)
{
    const size_t first_count = first->metadata & 0x0fu;
    const size_t second_count = second->metadata & 0x0fu;

    for (size_t i = 0u; i < first_count; i++) {
        for (size_t j = 0u; j < second_count; j++) {
            if (survey_gateway_compact_entry_node_index(
                    &first->entries[i]) ==
                survey_gateway_compact_entry_node_index(
                    &second->entries[j])) {
                return true;
            }
        }
    }
    return false;
}

static bool survey_gateway_round_hops_prove_separation(
    const struct survey_gateway_report_slot *first,
    const struct survey_gateway_report_slot *second)
{
    uint8_t first_hops;
    uint8_t second_hops;

    if (first->reverse_next_hop_index == UINT8_MAX ||
        second->reverse_next_hop_index == UINT8_MAX) {
        return false;
    }
    first_hops = first->reverse_hop_count;
    second_hops = second->reverse_hop_count;
    if (first_hops == 0u || first_hops > SURVEY_DEFAULT_TTL ||
        second_hops == 0u || second_hops > SURVEY_DEFAULT_TTL) {
        return false;
    }
    return first_hops > second_hops ?
        (uint8_t)(first_hops - second_hops) >= 2u :
        (uint8_t)(second_hops - first_hops) >= 2u;
}

static bool survey_gateway_round_reverse_paths_conflict(
    const struct survey_gateway_report_slot *first,
    uint8_t first_index,
    const struct survey_gateway_report_slot *second,
    uint8_t second_index)
{
    if (first->reverse_next_hop_index != UINT8_MAX &&
        first->reverse_next_hop_index == second_index) {
        return true;
    }
    if (second->reverse_next_hop_index != UINT8_MAX &&
        second->reverse_next_hop_index == first_index) {
        return true;
    }
    return first->reverse_next_hop_index != UINT8_MAX &&
           second->reverse_next_hop_index != UINT8_MAX &&
           first->reverse_next_hop_index ==
               second->reverse_next_hop_index;
}

static bool survey_gateway_round_anchors_conflict(
    const struct survey_gateway_context *context,
    uint8_t first_index,
    uint8_t second_index)
{
    const struct survey_gateway_report_slot *first;
    const struct survey_gateway_report_slot *second;

    if (first_index >= context->node_count ||
        second_index >= context->node_count) {
        return true;
    }
    first = &context->reports[first_index];
    second = &context->reports[second_index];
    if (first->metadata == UINT8_MAX || second->metadata == UINT8_MAX) {
        return true;
    }
    if (survey_gateway_round_report_has_peer(first, second_index) ||
        survey_gateway_round_report_has_peer(second, first_index)) {
        return true;
    }
    return !survey_gateway_round_hops_prove_separation(first, second);
}

static bool survey_gateway_round_pairs_conflict(
    const struct survey_gateway_context *context,
    const struct survey_gateway_pair_entry *first,
    const struct survey_gateway_pair_entry *second)
{
    const uint8_t first_indices[] = {
        first->initiator_index,
        first->responder_index,
    };
    const uint8_t second_indices[] = {
        second->initiator_index,
        second->responder_index,
    };

    for (size_t i = 0u; i < 2u; i++) {
        for (size_t j = 0u; j < 2u; j++) {
            const struct survey_gateway_report_slot *first_report =
                &context->reports[first_indices[i]];
            const struct survey_gateway_report_slot *second_report =
                &context->reports[second_indices[j]];

            if (first_indices[i] == second_indices[j] ||
                survey_gateway_round_reverse_paths_conflict(
                    first_report,
                    first_indices[i],
                    second_report,
                    second_indices[j]) ||
                survey_gateway_round_anchors_conflict(
                    context, first_indices[i], second_indices[j]) ||
                survey_gateway_round_reports_share_peer(
                    first_report, second_report)) {
                return true;
            }
        }
    }
    return false;
}

int survey_gateway_plan_pair_rounds(
    const struct survey_gateway_context *context,
    struct survey_pair_round_metadata *metadata,
    size_t metadata_cap,
    size_t *round_count)
{
    struct survey_pair_vertices validated = {
        .gateway_context = context,
    };
    size_t vertex_count = 0u;
    uint8_t round_total = 0u;
    int ret;

    if (context == NULL || round_count == NULL ||
        (metadata == NULL && context->pair_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    *round_count = 0u;
    if (!context->pairs_planned) {
        return PROTO_ERR_STALE;
    }
    if (context->pair_count > SURVEY_GATEWAY_MAX_PAIRS ||
        metadata_cap < context->pair_count) {
        return PROTO_ERR_NO_SPACE;
    }
    ret = survey_gateway_vertices_build(
        context, &validated, &vertex_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        const struct survey_gateway_pair_entry *pair = &context->pairs[i];

        if (pair->initiator_index >= context->node_count ||
            pair->responder_index >= context->node_count ||
            pair->initiator_index == pair->responder_index) {
            return PROTO_ERR_MALFORMED;
        }
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        uint8_t conflicting_rounds[(SURVEY_GATEWAY_MAX_PAIRS + 7u) / 8u] = {0};
        uint8_t selected_round = 0u;
        uint8_t position = 0u;

        for (size_t j = 0u; j < i; j++) {
            if (survey_gateway_round_pairs_conflict(
                    context, &context->pairs[i], &context->pairs[j])) {
                const uint8_t round = metadata[j].round_index;

                conflicting_rounds[round / 8u] |=
                    (uint8_t)(1u << (round % 8u));
            }
        }
        while (selected_round < round_total &&
               (conflicting_rounds[selected_round / 8u] &
                (uint8_t)(1u << (selected_round % 8u))) != 0u) {
            selected_round++;
        }
        if (selected_round == round_total) {
            round_total++;
        }
        for (size_t j = 0u; j < i; j++) {
            if (metadata[j].round_index == selected_round) {
                position++;
            }
        }
        metadata[i] = (struct survey_pair_round_metadata) {
            .round_index = selected_round,
            .pair_index_in_round = position,
        };
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        uint8_t pairs_in_round = 0u;

        for (size_t j = 0u; j < context->pair_count; j++) {
            if (metadata[j].round_index == metadata[i].round_index) {
                pairs_in_round++;
            }
        }
        metadata[i].pair_count_in_round = pairs_in_round;
    }

    *round_count = round_total;
    return PROTO_OK;
}
