#include "survey.h"

#include "enumeration_response_lane.h"
#include "protocol.h"

#include <limits.h>
#include <string.h>

_Static_assert(SURVEY_MAX_ANCHORS == 50u,
               "survey protocol is qualified for fifty anchors");
_Static_assert(SURVEY_MAX_PAIRS == 100u,
               "degree four permits at most one hundred pairs");
_Static_assert(SURVEY_NEIGHBOR_RECORD_WIRE_LEN == 8u,
               "neighbor presence record must remain compact");
_Static_assert(SURVEY_PLAN_MAX_WIRE_LEN <= PACKET_EXT_MAX_PAYLOAD_LEN,
               "a maximum survey plan must fit one extended packet");
_Static_assert(SURVEY_GRAPH_MAX_WIRE_LEN <= PACKET_EXT_MAX_PAYLOAD_LEN,
               "a maximum neighbor graph must fit one extended packet");
_Static_assert(SURVEY_RESULTS_MAX_WIRE_LEN <= PACKET_EXT_MAX_PAYLOAD_LEN,
               "all survey results must fit one extended packet");
_Static_assert(
    SURVEY_RESPONDER_HEAD_START_MS +
        ((SURVEY_RANGE_ATTEMPT_COUNT - 1u) *
         SURVEY_RANGE_ATTEMPT_SPACING_MS) < SURVEY_RANGE_WAVE_MS,
    "all scheduled range attempts must start inside their wave");
_Static_assert(
    SURVEY_CONTROL_ORIGIN_BUDGET_MS +
        SURVEY_CONTROL_ACTIVATION_BUDGET_MS +
        SURVEY_CONTROL_PROPAGATION_MARGIN_MS +
        (UWB_ENUM_MAX_HOPS * SURVEY_CONTROL_PER_HOP_BUDGET_MS) +
        SURVEY_CONTROL_REDUNDANCY_MS +
        SURVEY_RADIO_GUARD_MS +
        (SURVEY_MAX_ANCHORS * SURVEY_NEIGHBOR_SLOT_MS) +
        SURVEY_RESULT_PREPARE_MS + ENUMERATION_RESPONSE_LANE_MS <
            SURVEY_INITIAL_SELF_EXPIRY_MS,
    "worst-case control and neighbor-result timing must fit self-expiry");
_Static_assert(SURVEY_CONTROL_PER_HOP_BUDGET_MS == 1100u,
               "survey START requires wake plus the compact relay bound");

static bool slot_valid(uint8_t slot)
{
    return slot < SURVEY_MAX_ANCHORS;
}

bool survey_assignment_identity_valid(
    const struct survey_assignment_identity *identity)
{
    uint8_t nonzero = 0u;

    if (identity == NULL || identity->assignment_epoch == 0u ||
        identity->table_command_seq == 0u || identity->slot_span == 0u ||
        identity->slot_span > SURVEY_MAX_ANCHORS ||
        identity->max_hop_count == 0u ||
        identity->max_hop_count > UWB_ENUM_MAX_HOPS) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(identity->table_commitment.bytes); i++) {
        nonzero |= identity->table_commitment.bytes[i];
    }
    return nonzero != 0u;
}

bool survey_assignment_identity_equal(
    const struct survey_assignment_identity *left,
    const struct survey_assignment_identity *right)
{
    return survey_assignment_identity_valid(left) &&
           survey_assignment_identity_valid(right) &&
           left->assignment_epoch == right->assignment_epoch &&
           left->table_command_seq == right->table_command_seq &&
           left->slot_span == right->slot_span &&
           left->max_hop_count == right->max_hop_count &&
           semantic_digest_equal(left->table_commitment.bytes,
                                 right->table_commitment.bytes,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}

bool survey_identity_equal(const struct survey_identity *left,
                           const struct survey_identity *right)
{
    return left != NULL && right != NULL && left->generation != 0u &&
           left->generation == right->generation &&
           survey_assignment_identity_equal(&left->assignment,
                                            &right->assignment);
}

bool survey_neighbor_bitmap_get(const uint8_t *bitmap, uint8_t slot)
{
    return bitmap != NULL && slot_valid(slot) &&
           (bitmap[slot / 8u] & (uint8_t)(1u << (slot % 8u))) != 0u;
}

bool survey_neighbor_bitmap_set(uint8_t *bitmap, uint8_t slot)
{
    if (bitmap == NULL || !slot_valid(slot)) {
        return false;
    }
    bitmap[slot / 8u] |= (uint8_t)(1u << (slot % 8u));
    return true;
}

bool survey_graph_observed(const struct survey_graph *graph,
                           uint8_t observer_slot,
                           uint8_t observed_slot)
{
    uint64_t observer_bit;

    if (graph == NULL || !slot_valid(observer_slot) ||
        !slot_valid(observed_slot)) {
        return false;
    }
    observer_bit = UINT64_C(1) << observer_slot;
    return (graph->occupied_slot_mask & observer_bit) != 0u &&
           (graph->received_report_mask & observer_bit) != 0u &&
           survey_neighbor_bitmap_get(
               graph->reports[observer_slot].heard_bitmap, observed_slot);
}

bool survey_graph_mutual(const struct survey_graph *graph,
                         uint8_t first_slot,
                         uint8_t second_slot)
{
    return first_slot != second_slot &&
           survey_graph_observed(graph, first_slot, second_slot) &&
           survey_graph_observed(graph, second_slot, first_slot);
}

bool survey_graph_interferes(const struct survey_graph *graph,
                             uint8_t first_slot,
                             uint8_t second_slot)
{
    return first_slot == second_slot ||
           survey_graph_observed(graph, first_slot, second_slot) ||
           survey_graph_observed(graph, second_slot, first_slot);
}

int survey_graph_note_report(struct survey_graph *graph,
                             const struct survey_neighbor_report *report)
{
    uint64_t bit;

    if (graph == NULL || report == NULL || !slot_valid(report->own_slot)) {
        return PROTO_ERR_ARG;
    }
    bit = UINT64_C(1) << report->own_slot;
    if ((graph->occupied_slot_mask & bit) == 0u ||
        survey_neighbor_bitmap_get(report->heard_bitmap, report->own_slot)) {
        return PROTO_ERR_MALFORMED;
    }
    if ((graph->received_report_mask & bit) != 0u) {
        return memcmp(&graph->reports[report->own_slot], report,
                      sizeof(*report)) == 0 ? PROTO_OK : PROTO_ERR_STALE;
    }
    graph->reports[report->own_slot] = *report;
    graph->received_report_mask |= bit;
    return PROTO_OK;
}

uint32_t survey_neighbor_sequence_duration_ms(uint8_t slot_span)
{
    return slot_span == 0u || slot_span > SURVEY_MAX_ANCHORS ? 0u :
           (uint32_t)slot_span * SURVEY_NEIGHBOR_SLOT_MS;
}

uint8_t survey_slot_span_include(uint8_t occupied_slot_span,
                                 uint8_t assigned_slot)
{
    uint8_t required_span;

    if (occupied_slot_span > SURVEY_MAX_ANCHORS ||
        assigned_slot >= SURVEY_MAX_ANCHORS) {
        return 0u;
    }
    required_span = (uint8_t)(assigned_slot + 1u);
    return required_span > occupied_slot_span ?
        required_span : occupied_slot_span;
}

uint32_t survey_control_delivery_delay_ms(uint8_t max_hop_count)
{
    if (max_hop_count == 0u || max_hop_count > UWB_ENUM_MAX_HOPS) {
        return 0u;
    }
    /* The origin is submitted asynchronously, so keep its complete custody
     * budget plus the activation train of a wave admitted at that deadline.
     * The shared START/PLAN barrier retains START's per-hop activation bound;
     * PLAN itself is wake-free once START owns continuous Channel-5 RX. */
    return SURVEY_CONTROL_ORIGIN_BUDGET_MS +
           SURVEY_CONTROL_ACTIVATION_BUDGET_MS +
           SURVEY_CONTROL_PROPAGATION_MARGIN_MS +
           (uint32_t)max_hop_count *
               SURVEY_CONTROL_PER_HOP_BUDGET_MS +
           SURVEY_CONTROL_REDUNDANCY_MS +
           SURVEY_RADIO_GUARD_MS;
}

uint32_t survey_result_lane_duration_ms(uint8_t max_hop_count)
{
    return enumeration_response_duration_ms(max_hop_count);
}

uint32_t survey_wave_stride_ms(uint8_t max_hop_count)
{
    uint32_t lane_ms = survey_result_lane_duration_ms(max_hop_count);

    return lane_ms == 0u ? 0u : SURVEY_RANGE_WAVE_MS +
           SURVEY_RESULT_PREPARE_MS + lane_ms + SURVEY_RADIO_GUARD_MS;
}

uint32_t survey_execution_duration_ms(uint8_t wave_count,
                                      uint8_t max_hop_count)
{
    uint32_t stride_ms = survey_wave_stride_ms(max_hop_count);
    uint32_t stride_count = (uint32_t)wave_count +
                            SURVEY_EXTRA_DRAIN_STRIDES;

    if (stride_ms == 0u || stride_count > UINT32_MAX / stride_ms) {
        return 0u;
    }
    return stride_count * stride_ms;
}

bool survey_plan_fits_hard_cap(uint32_t elapsed_before_execution_ms,
                               uint8_t wave_count,
                               uint8_t max_hop_count)
{
    uint32_t execution_ms =
        survey_execution_duration_ms(wave_count, max_hop_count);

    return execution_ms != 0u &&
           elapsed_before_execution_ms <= SURVEY_HARD_CAP_MS &&
           execution_ms <= SURVEY_HARD_CAP_MS - elapsed_before_execution_ms;
}

uint32_t survey_neighbor_beacon_offset_ms(uint8_t beacon_index)
{
    if (beacon_index >= SURVEY_NEIGHBOR_BEACON_COUNT) {
        return UINT32_MAX;
    }
    return SURVEY_NEIGHBOR_QUIET_MARGIN_MS +
           (uint32_t)beacon_index * SURVEY_NEIGHBOR_BEACON_SPACING_MS;
}

uint32_t survey_range_attempt_offset_ms(uint8_t attempt_index)
{
    if (attempt_index >= SURVEY_RANGE_ATTEMPT_COUNT) {
        return UINT32_MAX;
    }
    return SURVEY_RESPONDER_HEAD_START_MS +
           (uint32_t)attempt_index * SURVEY_RANGE_ATTEMPT_SPACING_MS;
}

static bool pair_same(const struct survey_pair_request *left,
                      const struct survey_pair_request *right)
{
    return (left->first_slot == right->first_slot &&
            left->second_slot == right->second_slot) ||
           (left->first_slot == right->second_slot &&
            left->second_slot == right->first_slot);
}

static bool wave_pair_compatible(const struct survey_graph *graph,
                                 const struct survey_plan_pair *left,
                                 const struct survey_plan_pair *right)
{
    const uint8_t left_slots[2] = {
        left->initiator_slot, left->responder_slot,
    };
    const uint8_t right_slots[2] = {
        right->initiator_slot, right->responder_slot,
    };

    for (size_t i = 0u; i < 2u; i++) {
        for (size_t j = 0u; j < 2u; j++) {
            if (survey_graph_interferes(graph,
                                        left_slots[i], right_slots[j])) {
                return false;
            }
        }
    }
    return true;
}

static bool plan_shape_valid(const struct survey_plan *plan)
{
    uint8_t max_wave = 0u;

    if (plan == NULL || plan->identity.generation == 0u ||
        !survey_assignment_identity_valid(&plan->identity.assignment) ||
        plan->pair_count > SURVEY_MAX_PAIRS ||
        plan->execution_start_delay_ms == 0u ||
        plan->self_stop_delay_ms <= plan->execution_start_delay_ms) {
        return false;
    }
    if (plan->pair_count == 0u) {
        return plan->wave_count == 0u;
    }
    if (plan->wave_count == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < plan->pair_count; i++) {
        const struct survey_plan_pair *pair = &plan->pairs[i];

        if (!slot_valid(pair->initiator_slot) ||
            !slot_valid(pair->responder_slot) ||
            pair->initiator_slot == pair->responder_slot ||
            pair->wave_index >= plan->wave_count) {
            return false;
        }
        if (pair->wave_index > max_wave) {
            max_wave = pair->wave_index;
        }
    }
    return (uint8_t)(max_wave + 1u) == plan->wave_count;
}

bool survey_plan_commitment(const struct survey_plan *plan,
                            uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct semantic_digest_sha256_context context;
    uint8_t identity[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN];
    uint8_t fixed[15u];

    if (!plan_shape_valid(plan) || commitment == NULL ||
        survey_assignment_identity_encode(&plan->identity.assignment,
                                          identity) != sizeof(identity) ||
        !semantic_digest_sha256_init(&context)) {
        return false;
    }
    proto_put_u32_le(&fixed[0], plan->identity.generation);
    proto_put_u32_le(&fixed[4], plan->execution_start_delay_ms);
    proto_put_u32_le(&fixed[8], plan->self_stop_delay_ms);
    fixed[12] = plan->pair_count;
    fixed[13] = plan->wave_count;
    fixed[14] = SURVEY_PROTOCOL_VERSION;
    if (!semantic_digest_sha256_update(&context, fixed, sizeof(fixed)) ||
        !semantic_digest_sha256_update(&context, identity, sizeof(identity))) {
        return false;
    }
    for (uint8_t i = 0u; i < plan->pair_count; i++) {
        const uint8_t pair[SURVEY_PLAN_PAIR_WIRE_LEN] = {
            plan->pairs[i].initiator_slot,
            plan->pairs[i].responder_slot,
            plan->pairs[i].wave_index,
        };

        if (!semantic_digest_sha256_update(&context, pair, sizeof(pair))) {
            return false;
        }
    }
    return semantic_digest_sha256_final(&context, commitment);
}

bool survey_plan_commitment_valid(const struct survey_plan *plan)
{
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];

    return survey_plan_commitment(plan, commitment) &&
           semantic_digest_equal(commitment, plan->commitment,
                                 sizeof(commitment));
}

int survey_build_plan(const struct survey_identity *identity,
                      const struct survey_graph *graph,
                      const uint8_t hop_counts[SURVEY_MAX_ANCHORS],
                      const struct survey_pair_request *requests,
                      size_t request_count,
                      uint32_t execution_start_delay_ms,
                      struct survey_plan_build_result *result)
{
    uint8_t degree[SURVEY_MAX_ANCHORS] = {0};

    if (identity == NULL || identity->generation == 0u || graph == NULL ||
        hop_counts == NULL || result == NULL ||
        (requests == NULL && request_count != 0u) ||
        request_count > SURVEY_MAX_PAIRS || execution_start_delay_ms == 0u ||
        !survey_assignment_identity_valid(&identity->assignment)) {
        return PROTO_ERR_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->plan.identity = *identity;
    result->plan.execution_start_delay_ms = execution_start_delay_ms;

    for (size_t input = 0u; input < request_count; input++) {
        const struct survey_pair_request *request = &requests[input];
        enum survey_plan_skip_reason reason = SURVEY_PLAN_ACCEPTED;
        struct survey_plan_pair accepted = {0};
        uint8_t first = request->first_slot;
        uint8_t second = request->second_slot;
        uint8_t wave;

        if (!slot_valid(first) || !slot_valid(second) ||
            (graph->occupied_slot_mask & (UINT64_C(1) << first)) == 0u ||
            (graph->occupied_slot_mask & (UINT64_C(1) << second)) == 0u) {
            reason = SURVEY_PLAN_SKIP_UNKNOWN_SLOT;
        } else if (first == second) {
            reason = SURVEY_PLAN_SKIP_SELF_PAIR;
        } else if (!survey_graph_mutual(graph, first, second)) {
            reason = SURVEY_PLAN_SKIP_NOT_MUTUAL;
        } else if (hop_counts[first] == 0u ||
                   hop_counts[second] == 0u ||
                   hop_counts[first] > identity->assignment.max_hop_count ||
                   hop_counts[second] > identity->assignment.max_hop_count) {
            reason = SURVEY_PLAN_SKIP_MISSING_HOP;
        } else {
            for (size_t prior = 0u; prior < input; prior++) {
                if (pair_same(request, &requests[prior])) {
                    reason = SURVEY_PLAN_SKIP_DUPLICATE;
                    break;
                }
            }
        }
        if (reason == SURVEY_PLAN_ACCEPTED &&
            (degree[first] >= SURVEY_MAX_DEGREE ||
             degree[second] >= SURVEY_MAX_DEGREE)) {
            reason = SURVEY_PLAN_SKIP_DEGREE_CAP;
        }
        if (reason == SURVEY_PLAN_ACCEPTED &&
            result->plan.pair_count >= SURVEY_MAX_PAIRS) {
            reason = SURVEY_PLAN_SKIP_CAPACITY;
        }
        if (reason != SURVEY_PLAN_ACCEPTED) {
            struct survey_plan_skip *skip =
                &result->skipped[result->skipped_count++];

            skip->request = *request;
            skip->reason = reason;
            skip->input_index = (uint8_t)input;
            continue;
        }

        if (hop_counts[first] < hop_counts[second] ||
            (hop_counts[first] == hop_counts[second] && first < second)) {
            accepted.responder_slot = first;
            accepted.initiator_slot = second;
        } else {
            accepted.responder_slot = second;
            accepted.initiator_slot = first;
        }

        for (wave = 0u; wave < result->plan.wave_count; wave++) {
            bool compatible = true;

            for (uint8_t prior = 0u;
                 prior < result->plan.pair_count; prior++) {
                if (result->plan.pairs[prior].wave_index == wave &&
                    !wave_pair_compatible(graph,
                                          &accepted,
                                          &result->plan.pairs[prior])) {
                    compatible = false;
                    break;
                }
            }
            if (compatible) {
                break;
            }
        }
        accepted.wave_index = wave;
        if (wave == result->plan.wave_count) {
            result->plan.wave_count++;
        }
        result->plan.pairs[result->plan.pair_count++] = accepted;
        degree[first]++;
        degree[second]++;
    }

    result->plan.self_stop_delay_ms = execution_start_delay_ms +
        survey_execution_duration_ms(result->plan.wave_count,
                                     identity->assignment.max_hop_count);
    if (result->plan.self_stop_delay_ms <= execution_start_delay_ms ||
        result->plan.self_stop_delay_ms > SURVEY_HARD_CAP_MS ||
        !survey_plan_commitment(&result->plan, result->plan.commitment)) {
        memset(result, 0, sizeof(*result));
        return PROTO_ERR_NO_SPACE;
    }
    return PROTO_OK;
}

enum survey_range_result_status survey_range_status(
    const struct survey_range_result *result)
{
    if (result == NULL || result->success_count == 0u ||
        result->median_mm == SURVEY_NO_MEDIAN_MM) {
        return SURVEY_RANGE_RESULT_MISSING;
    }
    return result->success_count >= SURVEY_MIN_SUCCESSFUL_RANGES ?
        SURVEY_RANGE_RESULT_USABLE : SURVEY_RANGE_RESULT_INSUFFICIENT;
}

static void sort_samples(int32_t *samples, size_t count)
{
    for (size_t i = 1u; i < count; i++) {
        int32_t value = samples[i];
        size_t j = i;

        while (j > 0u && samples[j - 1u] > value) {
            samples[j] = samples[j - 1u];
            j--;
        }
        samples[j] = value;
    }
}

int survey_range_result_from_samples(uint8_t pair_index,
                                     uint8_t responder_slot,
                                     const int32_t *samples_mm,
                                     size_t sample_count,
                                     struct survey_range_result *result)
{
    int32_t sorted[SURVEY_RANGE_ATTEMPT_COUNT];

    if (result == NULL || pair_index >= SURVEY_MAX_PAIRS ||
        !slot_valid(responder_slot) ||
        sample_count > SURVEY_RANGE_ATTEMPT_COUNT ||
        (samples_mm == NULL && sample_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->pair_index = pair_index;
    result->responder_slot = responder_slot;
    result->median_mm = SURVEY_NO_MEDIAN_MM;
    for (size_t i = 0u; i < sample_count; i++) {
        if (samples_mm[i] < 0) {
            return PROTO_ERR_MALFORMED;
        }
        sorted[i] = samples_mm[i];
    }
    result->success_count = (uint8_t)sample_count;
    if (sample_count == 0u) {
        return PROTO_OK;
    }
    sort_samples(sorted, sample_count);
    if ((sample_count & 1u) != 0u) {
        result->median_mm = sorted[sample_count / 2u];
    } else {
        int64_t sum = (int64_t)sorted[(sample_count / 2u) - 1u] +
                      sorted[sample_count / 2u];

        result->median_mm = (int32_t)((sum + 1) / 2);
    }
    return PROTO_OK;
}

size_t survey_assignment_identity_encode(
    const struct survey_assignment_identity *identity,
    uint8_t out[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN])
{
    if (!survey_assignment_identity_valid(identity) || out == NULL) {
        return 0u;
    }
    proto_put_u32_le(&out[0], identity->assignment_epoch);
    proto_put_u32_le(&out[4], identity->table_command_seq);
    memcpy(&out[8], identity->table_commitment.bytes,
           SEMANTIC_DIGEST_SHA256_LEN);
    out[40] = identity->slot_span;
    out[41] = identity->max_hop_count;
    return SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN;
}

int survey_assignment_identity_decode(
    const uint8_t *data,
    size_t data_len,
    struct survey_assignment_identity *identity)
{
    if (data == NULL || identity == NULL ||
        data_len != SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    memset(identity, 0, sizeof(*identity));
    identity->assignment_epoch = proto_get_u32_le(&data[0]);
    identity->table_command_seq = proto_get_u32_le(&data[4]);
    memcpy(identity->table_commitment.bytes, &data[8],
           SEMANTIC_DIGEST_SHA256_LEN);
    identity->slot_span = data[40];
    identity->max_hop_count = data[41];
    return survey_assignment_identity_valid(identity) ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

size_t survey_neighbor_report_encode(
    const struct survey_neighbor_report *report,
    uint8_t out[SURVEY_NEIGHBOR_RECORD_WIRE_LEN])
{
    if (report == NULL || out == NULL || !slot_valid(report->own_slot) ||
        survey_neighbor_bitmap_get(report->heard_bitmap, report->own_slot)) {
        return 0u;
    }
    out[0] = report->own_slot;
    memcpy(&out[1], report->heard_bitmap, SURVEY_NEIGHBOR_BITMAP_BYTES);
    return SURVEY_NEIGHBOR_RECORD_WIRE_LEN;
}

int survey_neighbor_report_decode(const uint8_t *data,
                                  size_t data_len,
                                  struct survey_neighbor_report *report)
{
    if (data == NULL || report == NULL ||
        data_len != SURVEY_NEIGHBOR_RECORD_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    report->own_slot = data[0];
    memcpy(report->heard_bitmap, &data[1], SURVEY_NEIGHBOR_BITMAP_BYTES);
    return slot_valid(report->own_slot) &&
           !survey_neighbor_bitmap_get(report->heard_bitmap,
                                       report->own_slot) ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

size_t survey_range_result_encode(
    const struct survey_range_result *result,
    uint8_t out[SURVEY_RANGE_RESULT_WIRE_LEN])
{
    if (result == NULL || out == NULL ||
        result->pair_index >= SURVEY_MAX_PAIRS ||
        result->success_count > SURVEY_RANGE_ATTEMPT_COUNT ||
        !slot_valid(result->responder_slot) || result->reserved != 0u ||
        (result->success_count == 0u) !=
            (result->median_mm == SURVEY_NO_MEDIAN_MM) ||
        (result->success_count != 0u && result->median_mm < 0)) {
        return 0u;
    }
    out[0] = result->pair_index;
    out[1] = result->success_count;
    out[2] = result->responder_slot;
    out[3] = 0u;
    proto_put_u32_le(&out[4], (uint32_t)result->median_mm);
    return SURVEY_RANGE_RESULT_WIRE_LEN;
}

int survey_range_result_decode(const uint8_t *data,
                               size_t data_len,
                               struct survey_range_result *result)
{
    if (data == NULL || result == NULL ||
        data_len != SURVEY_RANGE_RESULT_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    *result = (struct survey_range_result) {
        .pair_index = data[0],
        .success_count = data[1],
        .responder_slot = data[2],
        .reserved = data[3],
        .median_mm = (int32_t)proto_get_u32_le(&data[4]),
    };
    return survey_range_result_encode(result,
                                      (uint8_t[SURVEY_RANGE_RESULT_WIRE_LEN]){0}) ==
            SURVEY_RANGE_RESULT_WIRE_LEN ? PROTO_OK : PROTO_ERR_MALFORMED;
}

size_t survey_plan_encode(const struct survey_plan *plan,
                          uint8_t *out,
                          size_t out_cap)
{
    size_t required;
    size_t offset = 0u;

    if (!survey_plan_commitment_valid(plan) || out == NULL) {
        return 0u;
    }
    required = SURVEY_PLAN_HEADER_WIRE_LEN +
               (size_t)plan->pair_count * SURVEY_PLAN_PAIR_WIRE_LEN;
    if (out_cap < required) {
        return 0u;
    }
    out[offset++] = SURVEY_PROTOCOL_VERSION;
    proto_put_u32_le(&out[offset], plan->identity.generation);
    offset += sizeof(uint32_t);
    offset += survey_assignment_identity_encode(&plan->identity.assignment,
                                                &out[offset]);
    proto_put_u32_le(&out[offset], plan->execution_start_delay_ms);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&out[offset], plan->self_stop_delay_ms);
    offset += sizeof(uint32_t);
    out[offset++] = plan->pair_count;
    out[offset++] = plan->wave_count;
    memcpy(&out[offset], plan->commitment, sizeof(plan->commitment));
    offset += sizeof(plan->commitment);
    for (uint8_t i = 0u; i < plan->pair_count; i++) {
        out[offset++] = plan->pairs[i].initiator_slot;
        out[offset++] = plan->pairs[i].responder_slot;
        out[offset++] = plan->pairs[i].wave_index;
    }
    return offset;
}

int survey_plan_decode(const uint8_t *data,
                       size_t data_len,
                       struct survey_plan *plan)
{
    size_t offset = 0u;
    size_t expected;
    int ret;

    if (data == NULL || plan == NULL || data_len < SURVEY_PLAN_HEADER_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    memset(plan, 0, sizeof(*plan));
    if (data[offset++] != SURVEY_PROTOCOL_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    plan->identity.generation = proto_get_u32_le(&data[offset]);
    offset += sizeof(uint32_t);
    ret = survey_assignment_identity_decode(&data[offset],
                                            SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN,
                                            &plan->identity.assignment);
    if (ret != PROTO_OK) {
        return ret;
    }
    offset += SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN;
    plan->execution_start_delay_ms = proto_get_u32_le(&data[offset]);
    offset += sizeof(uint32_t);
    plan->self_stop_delay_ms = proto_get_u32_le(&data[offset]);
    offset += sizeof(uint32_t);
    plan->pair_count = data[offset++];
    plan->wave_count = data[offset++];
    if (plan->pair_count > SURVEY_MAX_PAIRS) {
        return PROTO_ERR_MALFORMED;
    }
    expected = SURVEY_PLAN_HEADER_WIRE_LEN +
               (size_t)plan->pair_count * SURVEY_PLAN_PAIR_WIRE_LEN;
    if (data_len != expected) {
        return PROTO_ERR_BAD_LENGTH;
    }
    memcpy(plan->commitment, &data[offset], sizeof(plan->commitment));
    offset += sizeof(plan->commitment);
    for (uint8_t i = 0u; i < plan->pair_count; i++) {
        plan->pairs[i].initiator_slot = data[offset++];
        plan->pairs[i].responder_slot = data[offset++];
        plan->pairs[i].wave_index = data[offset++];
    }
    return survey_plan_commitment_valid(plan) ? PROTO_OK : PROTO_ERR_MALFORMED;
}
