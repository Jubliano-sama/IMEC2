#include "survey_protocol.h"

#include <string.h>

_Static_assert(SURVEY_EVENT_MAX_WIRE_LEN <= PACKET_EXT_MAX_PAYLOAD_LEN,
               "one retained host event must carry every range result");
_Static_assert(SURVEY_PLAN_MAX_WIRE_LEN > SURVEY_TLV_CHUNK_MAX_LEN,
               "maximum plans exercise ordered repeated TLVs");
_Static_assert(SURVEY_PLAN_MAX_WIRE_LEN <= 2u * SURVEY_TLV_CHUNK_MAX_LEN,
               "one plan must need at most two TLV chunks");

static bool phase_valid(enum survey_phase phase)
{
    return phase >= SURVEY_PHASE_NEIGHBOR_START &&
           phase <= SURVEY_PHASE_ABORT;
}

static int find_unique(const uint8_t *payload,
                       size_t payload_len,
                       uint8_t type,
                       const uint8_t **value,
                       uint8_t *value_len,
                       bool required)
{
    int ret = tlv_find_unique(payload, payload_len, type, value, value_len);

    if (ret == PROTO_ERR_NOT_FOUND && !required) {
        *value = NULL;
        *value_len = 0u;
        return PROTO_OK;
    }
    return ret;
}

static int append_chunked(uint8_t *payload,
                          size_t payload_cap,
                          size_t *payload_len,
                          uint8_t type,
                          const uint8_t *data,
                          size_t data_len)
{
    size_t offset = 0u;

    if (payload == NULL || payload_len == NULL ||
        (data == NULL && data_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    while (offset < data_len) {
        uint8_t chunk_len = (uint8_t)(data_len - offset >
                SURVEY_TLV_CHUNK_MAX_LEN ? SURVEY_TLV_CHUNK_MAX_LEN :
                data_len - offset);
        int ret = tlv_append_bytes(payload, payload_cap, payload_len, type,
                                   &data[offset], chunk_len);

        if (ret != PROTO_OK) {
            return ret;
        }
        offset += chunk_len;
    }
    return PROTO_OK;
}

static int extract_chunks(const uint8_t *payload,
                          size_t payload_len,
                          uint8_t type,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *out_len)
{
    size_t offset = 0u;
    size_t written = 0u;

    if ((payload == NULL && payload_len != 0u) || out == NULL ||
        out_len == NULL) {
        return PROTO_ERR_ARG;
    }
    while (offset < payload_len) {
        uint8_t item_type;
        uint8_t item_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        item_type = payload[offset++];
        item_len = payload[offset++];
        if ((size_t)item_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        if (item_type == type) {
            if ((size_t)item_len > out_cap - written) {
                return PROTO_ERR_NO_SPACE;
            }
            memcpy(&out[written], &payload[offset], item_len);
            written += item_len;
        }
        offset += item_len;
    }
    *out_len = written;
    return written == 0u ? PROTO_ERR_NOT_FOUND : PROTO_OK;
}

int survey_control_append_tlvs(uint8_t *payload,
                               size_t payload_cap,
                               size_t *payload_len,
                               const struct survey_control *control)
{
    uint8_t assignment[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN];
    uint8_t encoded_plan[SURVEY_PLAN_MAX_WIRE_LEN];
    size_t encoded_plan_len = 0u;
    int ret;

    if (payload == NULL || payload_len == NULL || control == NULL ||
        !phase_valid(control->phase) || control->identity.generation == 0u ||
        survey_assignment_identity_encode(&control->identity.assignment,
                                          assignment) != sizeof(assignment)) {
        return PROTO_ERR_ARG;
    }
    if (control->phase == SURVEY_PHASE_NEIGHBOR_START &&
        (!control->start_delay_present ||
         !control->self_stop_delay_present || control->plan_present)) {
        return PROTO_ERR_MALFORMED;
    }
    if (control->phase == SURVEY_PHASE_PLAN &&
        (!control->plan_present || control->start_delay_present ||
         control->self_stop_delay_present)) {
        return PROTO_ERR_MALFORMED;
    }
    if (control->phase == SURVEY_PHASE_ABORT &&
        (control->plan_present || control->start_delay_present ||
         control->self_stop_delay_present)) {
        return PROTO_ERR_MALFORMED;
    }
    if (control->plan_present) {
        encoded_plan_len = survey_plan_encode(&control->plan,
                                              encoded_plan,
                                              sizeof(encoded_plan));
        if (encoded_plan_len == 0u ||
            !survey_identity_equal(&control->identity,
                                   &control->plan.identity)) {
            return PROTO_ERR_MALFORMED;
        }
    }
    ret = tlv_append_u8(payload, payload_cap, payload_len,
                        TLV_SURVEY_PHASE, (uint8_t)control->phase);
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(payload, payload_cap, payload_len,
                             TLV_SURVEY_GENERATION,
                             control->identity.generation);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_SURVEY_ASSIGNMENT_IDENTITY,
                               assignment, sizeof(assignment));
    }
    if (ret == PROTO_OK && control->start_delay_present) {
        ret = tlv_append_u32(payload, payload_cap, payload_len,
                             TLV_SURVEY_START_DELAY_MS,
                             control->start_delay_ms);
    }
    if (ret == PROTO_OK && control->self_stop_delay_present) {
        ret = tlv_append_u32(payload, payload_cap, payload_len,
                             TLV_SURVEY_SELF_STOP_DELAY_MS,
                             control->self_stop_delay_ms);
    }
    if (ret == PROTO_OK && control->plan_present) {
        ret = append_chunked(payload, payload_cap, payload_len,
                             TLV_SURVEY_PLAN,
                             encoded_plan, encoded_plan_len);
    }
    return ret;
}

int survey_control_extract_tlvs(const uint8_t *payload,
                                size_t payload_len,
                                struct survey_control *control)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t encoded_plan[SURVEY_PLAN_MAX_WIRE_LEN];
    size_t encoded_plan_len = 0u;
    int ret;

    if ((payload == NULL && payload_len != 0u) || control == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(control, 0, sizeof(*control));
    ret = find_unique(payload, payload_len, TLV_SURVEY_PHASE,
                      &value, &value_len, true);
    if (ret != PROTO_OK || value_len != 1u ||
        !phase_valid((enum survey_phase)value[0])) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    control->phase = (enum survey_phase)value[0];
    ret = find_unique(payload, payload_len, TLV_SURVEY_GENERATION,
                      &value, &value_len, true);
    if (ret != PROTO_OK || value_len != sizeof(uint32_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    control->identity.generation = proto_get_u32_le(value);
    ret = find_unique(payload, payload_len, TLV_SURVEY_ASSIGNMENT_IDENTITY,
                      &value, &value_len, true);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_assignment_identity_decode(value, value_len,
                                             &control->identity.assignment);
    if (ret != PROTO_OK || control->identity.generation == 0u) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    ret = find_unique(payload, payload_len, TLV_SURVEY_START_DELAY_MS,
                      &value, &value_len, false);
    if (ret != PROTO_OK || (value != NULL && value_len != sizeof(uint32_t))) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    if (value != NULL) {
        control->start_delay_ms = proto_get_u32_le(value);
        control->start_delay_present = true;
    }
    ret = find_unique(payload, payload_len, TLV_SURVEY_SELF_STOP_DELAY_MS,
                      &value, &value_len, false);
    if (ret != PROTO_OK || (value != NULL && value_len != sizeof(uint32_t))) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    if (value != NULL) {
        control->self_stop_delay_ms = proto_get_u32_le(value);
        control->self_stop_delay_present = true;
    }
    ret = extract_chunks(payload, payload_len, TLV_SURVEY_PLAN,
                         encoded_plan, sizeof(encoded_plan),
                         &encoded_plan_len);
    if (ret == PROTO_OK) {
        ret = survey_plan_decode(encoded_plan, encoded_plan_len,
                                 &control->plan);
        if (ret != PROTO_OK ||
            !survey_identity_equal(&control->identity,
                                   &control->plan.identity)) {
            return ret == PROTO_OK ? PROTO_ERR_STALE : ret;
        }
        control->plan_present = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    {
        uint8_t scratch[PACKET_EXT_MAX_PAYLOAD_LEN];
        size_t scratch_len = 0u;
        struct survey_control canonical = *control;

        memset(scratch, 0, sizeof(scratch));
        if (survey_control_append_tlvs(scratch, sizeof(scratch),
                                       &scratch_len, &canonical) != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

int survey_host_plan_request_append_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    const struct survey_host_plan_request *request)
{
    uint8_t assignment[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN];
    uint8_t pairs[SURVEY_MAX_PAIRS * SURVEY_PAIR_REQUEST_WIRE_LEN];
    int ret;

    if (payload == NULL || payload_len == NULL || request == NULL ||
        request->identity.generation == 0u ||
        request->pair_count > SURVEY_MAX_PAIRS ||
        survey_assignment_identity_encode(&request->identity.assignment,
                                          assignment) != sizeof(assignment)) {
        return PROTO_ERR_ARG;
    }
    for (uint8_t i = 0u; i < request->pair_count; i++) {
        pairs[i * 2u] = request->pairs[i].first_slot;
        pairs[i * 2u + 1u] = request->pairs[i].second_slot;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_SURVEY_GENERATION,
                         request->identity.generation);
    if (ret == PROTO_OK) {
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_SURVEY_ASSIGNMENT_IDENTITY,
                               assignment, sizeof(assignment));
    }
    if (ret == PROTO_OK && request->pair_count != 0u) {
        ret = append_chunked(payload, payload_cap, payload_len,
                             TLV_SURVEY_PLAN, pairs,
                             (size_t)request->pair_count * 2u);
    }
    return ret;
}

int survey_host_plan_request_extract_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct survey_host_plan_request *request)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t pairs[SURVEY_MAX_PAIRS * SURVEY_PAIR_REQUEST_WIRE_LEN];
    size_t pairs_len = 0u;
    int ret;

    if ((payload == NULL && payload_len != 0u) || request == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(request, 0, sizeof(*request));
    ret = find_unique(payload, payload_len, TLV_SURVEY_GENERATION,
                      &value, &value_len, true);
    if (ret != PROTO_OK || value_len != sizeof(uint32_t)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    request->identity.generation = proto_get_u32_le(value);
    ret = find_unique(payload, payload_len, TLV_SURVEY_ASSIGNMENT_IDENTITY,
                      &value, &value_len, true);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_assignment_identity_decode(value, value_len,
                                             &request->identity.assignment);
    if (ret != PROTO_OK || request->identity.generation == 0u) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    ret = extract_chunks(payload, payload_len, TLV_SURVEY_PLAN,
                         pairs, sizeof(pairs), &pairs_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK || (pairs_len % 2u) != 0u) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    request->pair_count = (uint8_t)(pairs_len / 2u);
    for (uint8_t i = 0u; i < request->pair_count; i++) {
        request->pairs[i].first_slot = pairs[i * 2u];
        request->pairs[i].second_slot = pairs[i * 2u + 1u];
    }
    return PROTO_OK;
}

static bool event_shape_valid(const struct survey_event *event)
{
    if (event == NULL || event->identity.generation == 0u ||
        !survey_assignment_identity_valid(&event->identity.assignment) ||
        event->kind < SURVEY_EVENT_NEIGHBOR_GRAPH ||
        event->kind > SURVEY_EVENT_TERMINAL ||
        event->status > SURVEY_TERMINAL_BUSY ||
        event->result_count > SURVEY_MAX_PAIRS ||
        event->skipped_count > SURVEY_MAX_PAIRS) {
        return false;
    }
    switch (event->kind) {
    case SURVEY_EVENT_NEIGHBOR_GRAPH:
        return event->result_count == 0u && event->skipped_count == 0u;
    case SURVEY_EVENT_PLAN_ACCEPTED:
        return event->result_count == 0u &&
               event->plan.pair_count <= SURVEY_MAX_PAIRS &&
               survey_identity_equal(&event->identity,
                                     &event->plan.identity) &&
               survey_plan_commitment_valid(&event->plan);
    case SURVEY_EVENT_RANGE_PROGRESS:
    case SURVEY_EVENT_TERMINAL:
        return event->skipped_count == 0u;
    default:
        return false;
    }
}

size_t survey_event_encode(const struct survey_event *event,
                           uint8_t *out,
                           size_t out_cap)
{
    uint8_t assignment[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN];
    uint8_t graph_count = 0u;
    size_t body_len = 0u;
    size_t offset = SURVEY_EVENT_HEADER_WIRE_LEN;

    if (!event_shape_valid(event) || out == NULL ||
        survey_assignment_identity_encode(&event->identity.assignment,
                                          assignment) != sizeof(assignment)) {
        return 0u;
    }
    if (event->kind == SURVEY_EVENT_NEIGHBOR_GRAPH) {
        graph_count = (uint8_t)__builtin_popcountll(
            event->graph.received_report_mask);
        body_len = (size_t)graph_count * SURVEY_NEIGHBOR_RECORD_WIRE_LEN;
    } else if (event->kind == SURVEY_EVENT_PLAN_ACCEPTED) {
        body_len = (size_t)event->plan.pair_count *
                       SURVEY_PLAN_PAIR_WIRE_LEN +
                   (size_t)event->skipped_count *
                       SURVEY_EVENT_SKIP_WIRE_LEN;
    } else {
        body_len = (size_t)event->result_count *
                   SURVEY_RANGE_RESULT_WIRE_LEN;
    }
    if (out_cap < SURVEY_EVENT_HEADER_WIRE_LEN + body_len) {
        return 0u;
    }
    memset(out, 0, SURVEY_EVENT_HEADER_WIRE_LEN + body_len);
    out[0] = SURVEY_PROTOCOL_VERSION;
    out[1] = (uint8_t)event->kind;
    out[2] = (uint8_t)event->status;
    out[3] = graph_count;
    proto_put_u32_le(&out[4], event->identity.generation);
    proto_put_u16_le(&out[8], event->partial_reasons);
    out[10] = event->result_count;
    out[11] = event->kind == SURVEY_EVENT_PLAN_ACCEPTED ?
        event->plan.pair_count : 0u;
    out[12] = event->kind == SURVEY_EVENT_PLAN_ACCEPTED ?
        event->plan.wave_count : 0u;
    out[13] = event->skipped_count;
    memcpy(&out[14], assignment, sizeof(assignment));
    proto_put_u64_le(&out[56], event->graph.occupied_slot_mask);
    proto_put_u64_le(&out[64], event->graph.received_report_mask);

    if (event->kind == SURVEY_EVENT_NEIGHBOR_GRAPH) {
        for (uint8_t slot = 0u; slot < SURVEY_MAX_ANCHORS; slot++) {
            if ((event->graph.received_report_mask &
                 (UINT64_C(1) << slot)) == 0u) {
                continue;
            }
            if (survey_neighbor_report_encode(&event->graph.reports[slot],
                                              &out[offset]) == 0u) {
                return 0u;
            }
            offset += SURVEY_NEIGHBOR_RECORD_WIRE_LEN;
        }
    } else if (event->kind == SURVEY_EVENT_PLAN_ACCEPTED) {
        for (uint8_t i = 0u; i < event->plan.pair_count; i++) {
            out[offset++] = event->plan.pairs[i].initiator_slot;
            out[offset++] = event->plan.pairs[i].responder_slot;
            out[offset++] = event->plan.pairs[i].wave_index;
        }
        for (uint8_t i = 0u; i < event->skipped_count; i++) {
            out[offset++] = event->skipped[i].input_index;
            out[offset++] = event->skipped[i].request.first_slot;
            out[offset++] = event->skipped[i].request.second_slot;
            out[offset++] = (uint8_t)event->skipped[i].reason;
        }
    } else {
        for (uint8_t i = 0u; i < event->result_count; i++) {
            if (survey_range_result_encode(&event->results[i],
                                           &out[offset]) == 0u) {
                return 0u;
            }
            offset += SURVEY_RANGE_RESULT_WIRE_LEN;
        }
    }
    return offset;
}

int survey_event_decode(const uint8_t *data,
                        size_t data_len,
                        struct survey_event *event)
{
    uint8_t graph_count;
    uint8_t pair_count;
    uint64_t encoded_received_report_mask;
    size_t expected_len;
    size_t offset = SURVEY_EVENT_HEADER_WIRE_LEN;
    int ret;

    if (data == NULL || event == NULL ||
        data_len < SURVEY_EVENT_HEADER_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    if (data[0] != SURVEY_PROTOCOL_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    memset(event, 0, sizeof(*event));
    event->kind = (enum survey_event_kind)data[1];
    event->status = (enum survey_terminal_status)data[2];
    graph_count = data[3];
    event->identity.generation = proto_get_u32_le(&data[4]);
    event->partial_reasons = proto_get_u16_le(&data[8]);
    event->result_count = data[10];
    pair_count = data[11];
    event->plan.wave_count = data[12];
    event->skipped_count = data[13];
    ret = survey_assignment_identity_decode(
        &data[14], SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN,
        &event->identity.assignment);
    if (ret != PROTO_OK) {
        return ret;
    }
    event->graph.occupied_slot_mask = proto_get_u64_le(&data[56]);
    encoded_received_report_mask = proto_get_u64_le(&data[64]);
    if (event->kind == SURVEY_EVENT_NEIGHBOR_GRAPH) {
        if (graph_count > SURVEY_MAX_ANCHORS || pair_count != 0u ||
            event->result_count != 0u || event->skipped_count != 0u ||
            graph_count != __builtin_popcountll(
                encoded_received_report_mask)) {
            return PROTO_ERR_MALFORMED;
        }
        expected_len = SURVEY_EVENT_HEADER_WIRE_LEN +
                       (size_t)graph_count *
                           SURVEY_NEIGHBOR_RECORD_WIRE_LEN;
        if (data_len != expected_len) {
            return PROTO_ERR_BAD_LENGTH;
        }
        for (uint8_t i = 0u; i < graph_count; i++) {
            struct survey_neighbor_report report;

            ret = survey_neighbor_report_decode(
                &data[offset], SURVEY_NEIGHBOR_RECORD_WIRE_LEN, &report);
            if (ret != PROTO_OK ||
                survey_graph_note_report(&event->graph, &report) != PROTO_OK) {
                return PROTO_ERR_MALFORMED;
            }
            offset += SURVEY_NEIGHBOR_RECORD_WIRE_LEN;
        }
        if (event->graph.received_report_mask !=
            encoded_received_report_mask) {
            return PROTO_ERR_MALFORMED;
        }
    } else if (event->kind == SURVEY_EVENT_PLAN_ACCEPTED) {
        if (pair_count > SURVEY_MAX_PAIRS ||
            event->skipped_count > SURVEY_MAX_PAIRS || graph_count != 0u ||
            event->result_count != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        expected_len = SURVEY_EVENT_HEADER_WIRE_LEN +
                       (size_t)pair_count * SURVEY_PLAN_PAIR_WIRE_LEN +
                       (size_t)event->skipped_count *
                           SURVEY_EVENT_SKIP_WIRE_LEN;
        if (data_len != expected_len) {
            return PROTO_ERR_BAD_LENGTH;
        }
        event->plan.identity = event->identity;
        event->plan.pair_count = pair_count;
        for (uint8_t i = 0u; i < pair_count; i++) {
            event->plan.pairs[i].initiator_slot = data[offset++];
            event->plan.pairs[i].responder_slot = data[offset++];
            event->plan.pairs[i].wave_index = data[offset++];
        }
        for (uint8_t i = 0u; i < event->skipped_count; i++) {
            event->skipped[i].input_index = data[offset++];
            event->skipped[i].request.first_slot = data[offset++];
            event->skipped[i].request.second_slot = data[offset++];
            event->skipped[i].reason =
                (enum survey_plan_skip_reason)data[offset++];
        }
        /* Host projection omits timing/commitment; it is descriptive only. */
        return event->kind == SURVEY_EVENT_PLAN_ACCEPTED ?
            PROTO_OK : PROTO_ERR_MALFORMED;
    } else if (event->kind == SURVEY_EVENT_RANGE_PROGRESS ||
               event->kind == SURVEY_EVENT_TERMINAL) {
        if (event->result_count > SURVEY_MAX_PAIRS || graph_count != 0u ||
            pair_count != 0u || event->skipped_count != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        expected_len = SURVEY_EVENT_HEADER_WIRE_LEN +
                       (size_t)event->result_count *
                           SURVEY_RANGE_RESULT_WIRE_LEN;
        if (data_len != expected_len) {
            return PROTO_ERR_BAD_LENGTH;
        }
        for (uint8_t i = 0u; i < event->result_count; i++) {
            ret = survey_range_result_decode(&data[offset],
                                             SURVEY_RANGE_RESULT_WIRE_LEN,
                                             &event->results[i]);
            if (ret != PROTO_OK) {
                return ret;
            }
            offset += SURVEY_RANGE_RESULT_WIRE_LEN;
        }
    } else {
        return PROTO_ERR_MALFORMED;
    }
    return event_shape_valid(event) ? PROTO_OK : PROTO_ERR_MALFORMED;
}
