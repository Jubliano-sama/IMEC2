#include "survey.h"

struct survey_pair_candidate {
    size_t peer_index;
    int8_t rssi_dbm;
    uint8_t quality;
    bool mutual;
};

struct survey_connect_candidate {
    struct survey_pair_candidate pair;
    size_t report_index;
    bool valid;
};

struct survey_pair_plan_output {
    struct survey_pair *full_pairs;
    struct survey_gateway_pair_entry *gateway_pairs;
    size_t pair_cap;
};

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS >=
               (SURVEY_GATEWAY_MAX_REPORTS * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u,
               "survey pair storage must hold the bounded topology");

static const struct survey_reachability_entry *survey_report_find_peer(
    const struct survey_reachability_report *report,
    uint64_t peer_id)
{
    for (size_t i = 0u; i < report->entry_count; i++) {
        if (report->entries[i].peer_id == peer_id) {
            return &report->entries[i];
        }
    }
    return NULL;
}

static bool survey_candidate_precedes(const struct survey_pair_candidate *left,
                                      const struct survey_pair_candidate *right,
                                      const struct survey_reachability_report *const *ordered)
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
    return ordered[left->peer_index]->anchor_id <
           ordered[right->peer_index]->anchor_id;
}

static bool survey_pair_candidate_build(
    const struct survey_reachability_report *const *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_candidate *candidate)
{
    const struct survey_reachability_entry *forward =
        survey_report_find_peer(ordered[report_index],
                                ordered[peer_index]->anchor_id);
    const struct survey_reachability_entry *reverse =
        survey_report_find_peer(ordered[peer_index],
                                ordered[report_index]->anchor_id);

    if (candidate == NULL || (forward == NULL && reverse == NULL)) {
        return false;
    }
    candidate->peer_index = peer_index;
    candidate->mutual = forward != NULL && reverse != NULL;
    if (candidate->mutual) {
        candidate->quality = forward->quality < reverse->quality ?
                             forward->quality : reverse->quality;
        candidate->rssi_dbm = forward->rssi_dbm < reverse->rssi_dbm ?
                              forward->rssi_dbm : reverse->rssi_dbm;
    } else if (forward != NULL) {
        candidate->quality = forward->quality;
        candidate->rssi_dbm = forward->rssi_dbm;
    } else {
        candidate->quality = reverse->quality;
        candidate->rssi_dbm = reverse->rssi_dbm;
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
    const struct survey_reachability_report *const *ordered)
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
    return ordered[left->report_index]->anchor_id <
           ordered[right->report_index]->anchor_id;
}

static int survey_append_planned_pair(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_reachability_report *const *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    const uint64_t initiator_id = ordered[report_index]->anchor_id;
    const uint64_t responder_id = ordered[peer_index]->anchor_id;

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
        output->gateway_pairs[*count] = (struct survey_gateway_pair_entry) {
            .initiator_id = initiator_id,
            .responder_id = responder_id,
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
        const uint64_t initiator_id = output->full_pairs != NULL ?
            output->full_pairs[i].initiator_id :
            output->gateway_pairs[i].initiator_id;
        const uint64_t responder_id = output->full_pairs != NULL ?
            output->full_pairs[i].responder_id :
            output->gateway_pairs[i].responder_id;

        if (initiator_id == first_id && responder_id == second_id) {
            return true;
        }
    }
    return false;
}

static int survey_connect_report_graph(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_reachability_report *const *ordered,
    size_t report_count,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    size_t parents[SURVEY_GATEWAY_MAX_REPORTS];

    if (report_count > 1u && output->pair_cap < report_count - 1u) {
        return PROTO_ERR_NO_SPACE;
    }
    for (size_t i = 0u; i < report_count; i++) {
        parents[i] = i;
    }
    for (size_t edge = 0u; edge + 1u < report_count; edge++) {
        struct survey_connect_candidate best = {0};

        for (size_t i = 0u; i < report_count; i++) {
            if (degree[i] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            for (size_t j = i + 1u; j < report_count; j++) {
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
            return PROTO_ERR_NOT_FOUND;
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

static int survey_plan_pairs_from_reachability_into(
    uint32_t survey_id,
    const struct survey_reachability_report *reports,
    size_t report_count,
    uint16_t sample_count,
    struct survey_pair_plan_output *output,
    size_t *pair_count)
{
    const struct survey_reachability_report *ordered[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t degree[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    size_t count = 0u;

    if (reports == NULL || output == NULL || pair_count == NULL ||
        (output->full_pairs == NULL) == (output->gateway_pairs == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (survey_id == 0u || !survey_sample_count_valid(sample_count)) {
        return PROTO_ERR_MALFORMED;
    }
    if (report_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }

    *pair_count = 0u;
    for (size_t i = 0u; i < report_count; i++) {
        const struct survey_reachability_report *report = &reports[i];

        if (report->anchor_id == 0u ||
            (report->entries == NULL && report->entry_count != 0u)) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < report->entry_count; j++) {
            const struct survey_reachability_entry *entry = &report->entries[j];
            int ret;

            ret = survey_reachability_entry_validate(entry);
            if (ret != PROTO_OK) {
                return ret;
            }
            if (entry->peer_id == report->anchor_id) {
                return PROTO_ERR_MALFORMED;
            }
        }
        ordered[i] = report;
    }

    for (size_t i = 1u; i < report_count; i++) {
        const struct survey_reachability_report *value = ordered[i];
        size_t j = i;

        while (j > 0u && ordered[j - 1u]->anchor_id > value->anchor_id) {
            ordered[j] = ordered[j - 1u];
            j--;
        }
        ordered[j] = value;
    }

    int ret = survey_connect_report_graph(survey_id, sample_count, ordered,
                                          report_count, output,
                                          &count, degree);
    if (ret != PROTO_OK) {
        return ret;
    }

    for (size_t i = 0u; i < report_count; i++) {
        struct survey_pair_candidate candidates[SURVEY_GATEWAY_MAX_REPORTS];
        size_t candidate_count = 0u;

        for (size_t j = i + 1u; j < report_count; j++) {
            struct survey_pair_candidate candidate;

            if (!survey_pair_candidate_build(ordered, i, j, &candidate)) {
                continue;
            }

            size_t insert = candidate_count;
            while (insert > 0u &&
                   survey_candidate_precedes(&candidate,
                                             &candidates[insert - 1u],
                                             ordered)) {
                candidates[insert] = candidates[insert - 1u];
                insert--;
            }
            candidates[insert] = candidate;
            candidate_count++;
        }

        for (size_t j = 0u; j < candidate_count &&
             degree[i] < SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR; j++) {
            const size_t peer_index = candidates[j].peer_index;

            if (degree[peer_index] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            if (survey_pair_already_planned(output, count,
                                            ordered[i]->anchor_id,
                                            ordered[peer_index]->anchor_id)) {
                continue;
            }
            ret = survey_append_planned_pair(
                survey_id, sample_count, ordered, i, peer_index, output,
                &count, degree);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
    }

    *pair_count = count;
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
                                                    sample_count,
                                                    &output,
                                                    pair_count);
}

static int survey_plan_pairs_into_gateway_context(
    struct survey_gateway_context *context,
    const struct survey_reachability_report *reports,
    size_t report_count)
{
    struct survey_pair_plan_output output = {
        .gateway_pairs = context->pairs,
        .pair_cap = SURVEY_GATEWAY_MAX_PAIRS,
    };

    return survey_plan_pairs_from_reachability_into(context->survey_id,
                                                    reports,
                                                    report_count,
                                                    context->sample_count,
                                                    &output,
                                                    &context->pair_count);
}

static const struct survey_gateway_report_slot *
survey_gateway_round_report_for_anchor(
    const struct survey_gateway_context *context,
    uint64_t anchor_id);

static void survey_gateway_orient_pair_for_control(
    const struct survey_gateway_context *context,
    struct survey_gateway_pair_entry *pair)
{
    const struct survey_gateway_report_slot *initiator;
    const struct survey_gateway_report_slot *responder;
    uint64_t swap_id;

    if (context == NULL || pair == NULL) {
        return;
    }
    initiator = survey_gateway_round_report_for_anchor(
        context, pair->initiator_id);
    responder = survey_gateway_round_report_for_anchor(
        context, pair->responder_id);
    if (initiator == NULL || responder == NULL ||
        !initiator->reverse_hint_valid || !responder->reverse_hint_valid ||
        initiator->reverse_hop_count == 0u ||
        initiator->reverse_hop_count > SURVEY_DEFAULT_TTL ||
        responder->reverse_hop_count == 0u ||
        responder->reverse_hop_count > SURVEY_DEFAULT_TTL ||
        initiator->reverse_hop_count <= responder->reverse_hop_count) {
        return;
    }

    swap_id = pair->initiator_id;
    pair->initiator_id = pair->responder_id;
    pair->responder_id = swap_id;
}

int survey_gateway_plan_pairs(struct survey_gateway_context *context)
{
    struct survey_reachability_report reports[SURVEY_GATEWAY_MAX_REPORTS];
    size_t report_count = 0u;
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u) {
        return PROTO_ERR_STALE;
    }

    for (size_t i = 0u; i < context->report_count; i++) {
        const struct survey_gateway_report_slot *slot = &context->reports[i];

        if (!slot->valid) {
            continue;
        }
        reports[report_count].anchor_id = slot->anchor_id;
        reports[report_count].entries = slot->entries;
        reports[report_count].entry_count = slot->entry_count;
        report_count++;
    }

    ret = survey_plan_pairs_into_gateway_context(context,
                                                 reports,
                                                 report_count);
    if (ret != PROTO_OK) {
        context->pairs_planned = false;
        context->pair_count = 0u;
        context->next_pair_index = 0u;
        return ret;
    }
    for (size_t i = 0u; i < context->pair_count; i++) {
        survey_gateway_orient_pair_for_control(context, &context->pairs[i]);
    }

    context->pairs_planned = true;
    context->next_pair_index = 0u;
    return PROTO_OK;
}

static const struct survey_gateway_report_slot *
survey_gateway_round_report_for_anchor(
    const struct survey_gateway_context *context,
    uint64_t anchor_id)
{
    for (size_t i = 0u; i < context->report_count; i++) {
        const struct survey_gateway_report_slot *slot = &context->reports[i];

        if (slot->valid && slot->anchor_id == anchor_id) {
            return slot;
        }
    }
    return NULL;
}

static bool survey_gateway_round_report_has_peer(
    const struct survey_gateway_report_slot *slot,
    uint64_t peer_id)
{
    for (size_t i = 0u; i < slot->entry_count; i++) {
        if (slot->entries[i].peer_id == peer_id) {
            return true;
        }
    }
    return false;
}

static bool survey_gateway_round_reports_share_peer(
    const struct survey_gateway_report_slot *first,
    const struct survey_gateway_report_slot *second)
{
    for (size_t i = 0u; i < first->entry_count; i++) {
        for (size_t j = 0u; j < second->entry_count; j++) {
            if (first->entries[i].peer_id == second->entries[j].peer_id) {
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

    if (!first->reverse_hint_valid || !second->reverse_hint_valid) {
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
    uint64_t first_id,
    const struct survey_gateway_report_slot *second,
    uint64_t second_id)
{
    if (first->reverse_hint_valid &&
        first->reverse_next_hop_id != 0u &&
        first->reverse_next_hop_id == second_id) {
        return true;
    }
    if (second->reverse_hint_valid &&
        second->reverse_next_hop_id != 0u &&
        second->reverse_next_hop_id == first_id) {
        return true;
    }
    return first->reverse_hint_valid && second->reverse_hint_valid &&
           first->reverse_next_hop_id != 0u &&
           first->reverse_next_hop_id == second->reverse_next_hop_id;
}

static bool survey_gateway_round_anchors_conflict(
    const struct survey_gateway_context *context,
    uint64_t first_id,
    uint64_t second_id)
{
    const struct survey_gateway_report_slot *first =
        survey_gateway_round_report_for_anchor(context, first_id);
    const struct survey_gateway_report_slot *second =
        survey_gateway_round_report_for_anchor(context, second_id);

    if (first == NULL || second == NULL) {
        return true;
    }
    if (survey_gateway_round_report_has_peer(first, second_id) ||
        survey_gateway_round_report_has_peer(second, first_id)) {
        return true;
    }
    if (first->entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT &&
        second->entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
        return false;
    }
    return !survey_gateway_round_hops_prove_separation(first, second);
}

static bool survey_gateway_round_pairs_conflict(
    const struct survey_gateway_context *context,
    const struct survey_gateway_pair_entry *first,
    const struct survey_gateway_pair_entry *second)
{
    const uint64_t first_ids[] = {
        first->initiator_id,
        first->responder_id,
    };
    const uint64_t second_ids[] = {
        second->initiator_id,
        second->responder_id,
    };

    for (size_t i = 0u; i < 2u; i++) {
        for (size_t j = 0u; j < 2u; j++) {
            const struct survey_gateway_report_slot *first_report =
                survey_gateway_round_report_for_anchor(context, first_ids[i]);
            const struct survey_gateway_report_slot *second_report =
                survey_gateway_round_report_for_anchor(context, second_ids[j]);

            if (first_ids[i] == second_ids[j] ||
                (first_report != NULL && second_report != NULL &&
                 survey_gateway_round_reverse_paths_conflict(
                     first_report,
                     first_ids[i],
                     second_report,
                     second_ids[j])) ||
                survey_gateway_round_anchors_conflict(
                    context, first_ids[i], second_ids[j]) ||
                first_report == NULL || second_report == NULL ||
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
    uint8_t round_total = 0u;

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

    for (size_t i = 0u; i < context->pair_count; i++) {
        const struct survey_gateway_pair_entry *pair = &context->pairs[i];

        if (pair->initiator_id == 0u || pair->responder_id == 0u ||
            pair->initiator_id == pair->responder_id) {
            return PROTO_ERR_MALFORMED;
        }
        if (survey_gateway_round_report_for_anchor(
                context, pair->initiator_id) == NULL ||
            survey_gateway_round_report_for_anchor(
                context, pair->responder_id) == NULL) {
            return PROTO_ERR_NOT_FOUND;
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
