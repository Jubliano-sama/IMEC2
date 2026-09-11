#include "survey_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct survey_identity identity(void)
{
    struct survey_identity value = {
        .generation = 9u,
        .assignment = {
            .assignment_epoch = 7u,
            .table_command_seq = 8u,
            .slot_span = 50u,
            .max_hop_count = 5u,
        },
    };

    memset(value.assignment.table_commitment.bytes, 0xa5,
           sizeof(value.assignment.table_commitment.bytes));
    return value;
}

static struct survey_plan maximum_plan(void)
{
    struct survey_plan plan = {
        .identity = identity(),
        .execution_start_delay_ms = 3000u,
        .self_stop_delay_ms = 1200000u,
        .pair_count = SURVEY_MAX_PAIRS,
        .wave_count = SURVEY_MAX_PAIRS,
        .batch_index = SURVEY_MAX_BATCHES - 1u,
        .final_batch = true,
    };

    for (uint8_t i = 0u; i < plan.pair_count; i++) {
        plan.pairs[i] = (struct survey_plan_pair) {
            .initiator_slot = (uint8_t)(i % SURVEY_MAX_ANCHORS),
            .responder_slot = (uint8_t)((i + 1u) % SURVEY_MAX_ANCHORS),
            .wave_index = i,
        };
    }
    assert(survey_plan_commitment(&plan, plan.commitment));
    return plan;
}

static void test_chunked_plan_control(void)
{
    struct survey_control control = {
        .phase = SURVEY_PHASE_PLAN,
        .identity = identity(),
        .plan = maximum_plan(),
        .plan_present = true,
    };
    struct survey_control decoded;
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN] = {0};
    size_t payload_len = 0u;
    size_t plan_tlv_count = 0u;
    size_t offset = 0u;

    assert(survey_control_append_tlvs(payload, sizeof(payload), &payload_len,
                                      &control) == PROTO_OK);
    while (offset < payload_len) {
        uint8_t type = payload[offset];
        uint8_t len = payload[offset + 1u];

        if (type == TLV_SURVEY_PLAN) {
            plan_tlv_count++;
        }
        offset += 2u + len;
    }
    assert(plan_tlv_count == 2u);
    assert(survey_control_extract_tlvs(payload, payload_len,
                                       &decoded) == PROTO_OK);
    assert(decoded.plan_present);
    assert(decoded.plan.pair_count == SURVEY_MAX_PAIRS);
    assert(survey_identity_equal(&decoded.identity, &control.identity));

    /* A reordered/corrupted chunk no longer matches the whole-plan hash. */
    payload[payload_len - 1u] ^= 1u;
    assert(survey_control_extract_tlvs(payload, payload_len,
                                       &decoded) == PROTO_ERR_MALFORMED);
}

static void test_small_controls_reject_plan_and_foreign_phase_fields(void)
{
    struct survey_control control = {
        .phase = SURVEY_PHASE_ABORT,
        .identity = identity(),
    };
    struct survey_control decoded;
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN] = {0};
    size_t base_len = 0u;

    assert(survey_control_append_tlvs(payload, sizeof(payload), &base_len,
                                      &control) == PROTO_OK);
    assert(survey_control_extract_tlvs(payload, base_len, &decoded) == PROTO_OK);
    assert(decoded.phase == SURVEY_PHASE_ABORT && !decoded.plan_present);
    /* A malformed small control must fail its phase envelope before entering
     * the PLAN decoder, including a zero-length PLAN and repeated chunks. */
    for (uint16_t length = 0u; length <= SURVEY_TLV_CHUNK_MAX_LEN; length++) {
        uint8_t bytes[SURVEY_TLV_CHUNK_MAX_LEN] = {0};
        size_t payload_len = base_len;

        assert(tlv_append_bytes(payload, sizeof(payload), &payload_len,
                                TLV_SURVEY_PLAN, bytes, (uint8_t)length) == PROTO_OK);
        assert(survey_control_extract_tlvs(payload, payload_len, &decoded) ==
               PROTO_ERR_MALFORMED);
        assert(tlv_append_bytes(payload, sizeof(payload), &payload_len,
                                TLV_SURVEY_PLAN, bytes, (uint8_t)length) == PROTO_OK);
        assert(survey_control_extract_tlvs(payload, payload_len, &decoded) ==
               PROTO_ERR_MALFORMED);
    }
    for (unsigned which = 0u; which < 2u; which++) {
        size_t payload_len = base_len;

        assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                              which == 0u ? TLV_SURVEY_START_DELAY_MS :
                                            TLV_SURVEY_SELF_STOP_DELAY_MS,
                              1000u) == PROTO_OK);
        assert(survey_control_extract_tlvs(payload, payload_len, &decoded) ==
               PROTO_ERR_MALFORMED);
    }
    control.phase = SURVEY_PHASE_NEIGHBOR_START;
    control.start_delay_present = true;
    control.self_stop_delay_present = true;
    control.start_delay_ms = 3000u;
    control.self_stop_delay_ms = 10000u;
    base_len = 0u;
    assert(survey_control_append_tlvs(payload, sizeof(payload), &base_len,
                                      &control) == PROTO_OK);
    assert(survey_control_extract_tlvs(payload, base_len, &decoded) == PROTO_OK);
    assert(decoded.start_delay_ms == 3000u && decoded.self_stop_delay_ms == 10000u);
    assert(tlv_append_bytes(payload, sizeof(payload), &base_len,
                            TLV_SURVEY_PLAN, NULL, 0u) == PROTO_OK);
    assert(survey_control_extract_tlvs(payload, base_len, &decoded) ==
           PROTO_ERR_MALFORMED);
}

static void test_host_plan_and_event_round_trip(void)
{
    struct survey_host_plan_request request = {
        .identity = identity(),
        .pair_count = SURVEY_MAX_PAIRS,
        .batch_index = 3u,
        .final_batch = true,
    };
    struct survey_host_plan_request decoded_request;
    struct survey_event event = {
        .kind = SURVEY_EVENT_NEIGHBOR_GRAPH,
        .status = SURVEY_TERMINAL_PARTIAL,
        .identity = identity(),
        .partial_reasons = SURVEY_PARTIAL_ASYMMETRIC_NEIGHBOR,
    };
    struct survey_event decoded_event;
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN] = {0};
    uint8_t event_wire[SURVEY_EVENT_MAX_WIRE_LEN] = {0};
    size_t payload_len = 0u;
    size_t event_len;

    for (uint8_t i = 0u; i < request.pair_count; i++) {
        request.pairs[i].first_slot = (uint8_t)(i % 49u);
        request.pairs[i].second_slot =
            (uint8_t)((i % 49u) + 1u);
    }
    assert(survey_host_plan_request_append_tlvs(
        payload, sizeof(payload), &payload_len, &request) == PROTO_OK);
    assert(survey_host_plan_request_extract_tlvs(
        payload, payload_len, &decoded_request) == PROTO_OK);
    assert(decoded_request.pair_count == SURVEY_MAX_PAIRS);
    assert(decoded_request.batch_index == request.batch_index);
    assert(decoded_request.final_batch);
    assert(decoded_request.pairs[99].first_slot == request.pairs[99].first_slot);

    event.graph.occupied_slot_mask = UINT64_C(0x7);
    for (uint8_t slot = 0u; slot < 3u; slot++) {
        struct survey_neighbor_report report = {.own_slot = slot};

        assert(survey_neighbor_bitmap_set(report.heard_bitmap,
                                          (uint8_t)((slot + 1u) % 3u)));
        assert(survey_graph_note_report(&event.graph, &report) == PROTO_OK);
    }
    event_len = survey_event_encode(&event, event_wire, sizeof(event_wire));
    assert(event_len == SURVEY_EVENT_HEADER_WIRE_LEN +
                        3u * SURVEY_NEIGHBOR_RECORD_WIRE_LEN);
    assert(survey_event_decode(event_wire, event_len,
                               &decoded_event) == PROTO_OK);
    assert(decoded_event.graph.received_report_mask == UINT64_C(0x7));
    assert(decoded_event.partial_reasons ==
           SURVEY_PARTIAL_ASYMMETRIC_NEIGHBOR);
}

static void test_all_results_fit_one_event(void)
{
    struct survey_event event = {
        .kind = SURVEY_EVENT_TERMINAL,
        .status = SURVEY_TERMINAL_PARTIAL,
        .identity = identity(),
        .partial_reasons = SURVEY_PARTIAL_INSUFFICIENT_RANGE,
        .result_count = SURVEY_MAX_PAIRS,
    };
    struct survey_event decoded;
    uint8_t wire[SURVEY_EVENT_MAX_WIRE_LEN];
    size_t wire_len;

    for (uint8_t i = 0u; i < event.result_count; i++) {
        event.records.results[i] = (struct survey_range_result) {
            .median_mm = 1000 + i,
            .pair_index = i,
            .success_count = (uint8_t)(1u + (i % 5u)),
            .responder_slot = (uint8_t)(i % SURVEY_MAX_ANCHORS),
        };
    }
    wire_len = survey_event_encode(&event, wire, sizeof(wire));
    assert(wire_len == SURVEY_EVENT_HEADER_WIRE_LEN +
                       SURVEY_MAX_PAIRS * SURVEY_RANGE_RESULT_WIRE_LEN);
    assert(wire_len <= PACKET_EXT_MAX_PAYLOAD_LEN);
    assert(survey_event_decode(wire, wire_len, &decoded) == PROTO_OK);
    assert(decoded.result_count == SURVEY_MAX_PAIRS);
    assert(decoded.records.results[99].pair_index == 99u);
}

static void test_acceptance_event_identity_and_closed_header(void)
{
    const enum survey_event_kind kinds[] = {
        SURVEY_EVENT_STARTED, SURVEY_EVENT_PLAN_ACCEPTED,
    };
    for (size_t kind = 0u; kind < sizeof(kinds) / sizeof(kinds[0]); kind++) {
        struct survey_event event = {
            .kind = kinds[kind], .identity = identity(),
            .status = SURVEY_TERMINAL_COMPLETE,
            .host_session_id = UINT32_C(0xfedcba98), .host_sequence = UINT16_MAX,
        };
        struct survey_event decoded;
        uint8_t wire[SURVEY_EVENT_MAX_WIRE_LEN];
        if (event.kind == SURVEY_EVENT_PLAN_ACCEPTED) {
            event.plan = maximum_plan();
            event.batch_index = event.plan.batch_index;
            event.final_batch = event.plan.final_batch;
        }
        /* Host identity shares header storage with graph masks. Stale graph
         * fields must not enter acceptance wire data or the decoded graph. */
        event.graph.occupied_slot_mask = UINT64_MAX;
        event.graph.received_report_mask = UINT64_MAX;
        size_t length = survey_event_encode(&event, wire, sizeof(wire));
        assert(length == SURVEY_EVENT_HEADER_WIRE_LEN +
            (event.kind == SURVEY_EVENT_STARTED ? 0u :
                SURVEY_MAX_PAIRS * SURVEY_PLAN_PAIR_WIRE_LEN));
        assert(proto_get_u32_le(&wire[56]) == event.host_session_id);
        assert(proto_get_u16_le(&wire[60]) == event.host_sequence);
        assert(survey_event_decode(wire, length, &decoded) == PROTO_OK);
        assert(decoded.host_session_id == event.host_session_id);
        assert(decoded.host_sequence == event.host_sequence);
        assert(decoded.graph.occupied_slot_mask == 0u);
        assert(decoded.graph.received_report_mask == 0u);
        assert(survey_identity_equal(&decoded.identity, &event.identity));

        for (size_t offset = 62u; offset < SURVEY_EVENT_HEADER_WIRE_LEN; offset++) {
            if (offset == 64u) continue; /* The final-batch flag is defined. */
            assert(wire[offset] == 0u);
            wire[offset] = 1u;
            assert(survey_event_decode(wire, length, &decoded) == PROTO_ERR_MALFORMED);
            wire[offset] = 0u;
        }
        for (unsigned zero = 0u; zero < 2u; zero++) {
            struct survey_event missing = event;
            uint8_t candidate[SURVEY_EVENT_MAX_WIRE_LEN];
            if (zero == 0u) {
                missing.host_session_id = 0u;
                proto_put_u32_le(&wire[56], 0u);
            } else {
                missing.host_sequence = 0u;
                proto_put_u16_le(&wire[60], 0u);
            }
            assert(survey_event_encode(&missing, candidate, sizeof(candidate)) == 0u);
            assert(survey_event_decode(wire, length, &decoded) == PROTO_ERR_MALFORMED);
            assert(survey_event_encode(&event, wire, sizeof(wire)) == length);
        }
        for (int status = -1; status <= SURVEY_TERMINAL_BUSY; status++) {
            event.status = (enum survey_terminal_status)status;
            bool allowed = status >= 0 &&
                (event.kind == SURVEY_EVENT_STARTED ? status == 0 : status <= 1);
            uint8_t candidate[SURVEY_EVENT_MAX_WIRE_LEN];
            assert((survey_event_encode(&event, candidate, sizeof(candidate)) != 0u) == allowed);
            wire[2] = (uint8_t)status;
            assert((survey_event_decode(wire, length, &decoded) == PROTO_OK) == allowed);
        }
        if (event.kind == SURVEY_EVENT_STARTED) {
            wire[2] = SURVEY_TERMINAL_COMPLETE;
            wire[64] = 1u;
            assert(survey_event_decode(wire, length, &decoded) == PROTO_ERR_MALFORMED);
            wire[64] = 0u;
            wire[length] = 0u;
            assert(survey_event_decode(wire, length + 1u, &decoded) != PROTO_OK);
        }
    }
}

static void test_all_signal_records_fit_one_event(void)
{
    struct survey_event event = {
        .kind = SURVEY_EVENT_SIGNALS,
        .status = SURVEY_TERMINAL_COMPLETE,
        .identity = identity(),
    };
    struct survey_event decoded;
    uint8_t levels[SURVEY_MAX_ANCHORS] = {0};
    uint8_t wire[SURVEY_EVENT_MAX_WIRE_LEN];
    size_t wire_len;

    for (uint8_t owner = 0u; owner < SURVEY_MAX_ANCHORS; owner++) {
        for (uint8_t target = 0u; target < owner; target++) {
            levels[target] = (uint8_t)(1u + target % 15u);
        }
        for (uint8_t chunk = 0u;
             chunk < survey_signal_record_count_for_slot(owner); chunk++) {
            assert(event.signal_count < SURVEY_MAX_SIGNAL_RECORDS);
            assert(survey_signal_record_encode(
                       owner, chunk, levels,
                       &event.records.signals[event.signal_count++]) ==
                   SURVEY_SIGNAL_RECORD_WIRE_LEN);
        }
    }
    assert(event.signal_count == SURVEY_MAX_SIGNAL_RECORDS);
    wire_len = survey_event_encode(&event, wire, sizeof(wire));
    assert(wire_len == SURVEY_EVENT_MAX_WIRE_LEN);
    assert(wire_len == 946u);
    assert(wire_len <= PACKET_EXT_MAX_PAYLOAD_LEN);
    assert(survey_event_decode(wire, wire_len, &decoded) == PROTO_OK);
    assert(decoded.signal_count == SURVEY_MAX_SIGNAL_RECORDS);
    assert(memcmp(decoded.records.signals, event.records.signals,
                  sizeof(event.records.signals)) == 0);
}

int main(void)
{
    test_chunked_plan_control();
    test_small_controls_reject_plan_and_foreign_phase_fields();
    test_host_plan_and_event_round_trip();
    test_all_results_fit_one_event();
    test_acceptance_event_identity_and_closed_header();
    test_all_signal_records_fit_one_event();
    puts("survey protocol tests passed");
    return 0;
}
