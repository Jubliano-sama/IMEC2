#include "report.h"

#include "mesh_relay.h"
#include "uwb.h"

#include <assert.h>
#include <string.h>

static void fill_sample_metadata(uint8_t *round_indices,
                                 uint64_t *sequence_start_timestamps_ms,
                                 uint16_t count,
                                 uint8_t first_round)
{
    for (uint16_t i = 0u; i < count; i++) {
        round_indices[i] = (uint8_t)(first_round + i);
        sequence_start_timestamps_ms[i] = UINT64_C(1000000) +
                                          ((uint64_t)(first_round + i) * 50u);
    }
}

static void test_range_transport_sequence_is_unique_across_retry_attempts(void)
{
    bool seen[UINT16_MAX + 1u] = {false};
    uint16_t previous_attempt_last = 0u;
    uint16_t packet_seq = 0u;

    assert(RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS ==
           RANGE_REPORT_MAX_PACKET_FRAGMENTS +
           RANGE_REPORT_MAX_CIR_FRAGMENTS);
    for (uint16_t attempt = 1u; attempt <= UINT8_MAX; attempt++) {
        for (uint16_t fragment = 0u;
             fragment < RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS;
             fragment++) {
            assert(report_range_transport_seq((uint8_t)attempt,
                                              fragment,
                                              &packet_seq) == PROTO_OK);
            assert(packet_seq != 0u);
            assert(!seen[packet_seq]);
            seen[packet_seq] = true;
            if (fragment == 0u && attempt > 1u) {
                assert(packet_seq == previous_attempt_last + 1u);
            }
        }
        previous_attempt_last = packet_seq;
    }

    assert(report_range_transport_seq(0u, 0u, &packet_seq) ==
           PROTO_ERR_MALFORMED);
    assert(report_range_transport_seq(1u,
                                      RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS,
                                      &packet_seq) == PROTO_ERR_MALFORMED);
    assert(report_range_transport_seq(1u, 0u, NULL) == PROTO_ERR_ARG);
}

static void test_click_report_packet_counts_as_click(void)
{
    const int32_t distance_samples[] = {4550, 4567, 4580};
    const uint8_t round_indices[] = {0u, 1u, 2u};
    const uint64_t sequence_start_timestamps_ms[] = {
        UINT64_C(1234567890123),
        UINT64_C(1234567890173),
        UINT64_C(1234567890223),
    };
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0x01u, 0x02u, 0x03u, 0xF1u, 0xF2u, 0xF3u};
    const struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT | RANGE_DIAG_ANCHOR_PRESENT,
        .burst_id = 0x01020304u,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .burst_duration_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS,
    };
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 1234567890123ull,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 3u,
        .diagnostics = &diagnostics,
    };
    uint8_t payload[256];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(report_init_click_packet(&packet,
                                         fields.anchor_id,
                                         0x9999888877776666ull,
                                         0x12345678u,
                                         1u,
                                         (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_CLICK_REPORT);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) != 0u);
    assert((packet.flags & FLAG_DIAGNOSTIC) == 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);

    assert(tlv_find(payload, payload_len, TLV_CLICKER_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == fields.clicker_id);

    assert(tlv_find(payload, payload_len, TLV_TIMESTAMP_MS, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == fields.timestamp_ms);

    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == RANGE_OK);

    assert(tlv_find(payload, payload_len, TLV_UWB_RSL_DBM, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert((int8_t)value[0] == fields.rsl_dbm);

    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_SAMPLE, &value, &value_len) == PROTO_OK);
    assert(value_len == UWB_CIR_SAMPLE_LEN);
    assert(memcmp(value, cir_sample, UWB_CIR_SAMPLE_LEN) == 0);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == 3u);

    assert(tlv_find(payload, payload_len, TLV_DISTANCE_SAMPLES_MM, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(distance_samples));
    assert((int32_t)proto_get_u32_le(&value[0]) == distance_samples[0]);
    assert((int32_t)proto_get_u32_le(&value[4]) == distance_samples[1]);
    assert((int32_t)proto_get_u32_le(&value[8]) == distance_samples[2]);

    assert(tlv_find(payload, payload_len, TLV_RANGE_ROUND_INDICES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(round_indices));
    assert(memcmp(value, round_indices, sizeof(round_indices)) == 0);

    assert(tlv_find(payload, payload_len, TLV_SEQUENCE_START_TIMESTAMPS_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(sequence_start_timestamps_ms));
    assert(proto_get_u64_le(&value[0]) == sequence_start_timestamps_ms[0]);
    assert(proto_get_u64_le(&value[8]) == sequence_start_timestamps_ms[1]);
    assert(proto_get_u64_le(&value[16]) == sequence_start_timestamps_ms[2]);

    assert(tlv_find(payload, payload_len, TLV_BURST_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.burst_id);
    assert(tlv_find(payload, payload_len, TLV_CLICK_LATENCY_MS, &value, &value_len) ==
           PROTO_ERR_NOT_FOUND);
    assert(tlv_find(payload, payload_len, TLV_CHANNEL9_REPORT_LATENCY_MS,
                    &value, &value_len) == PROTO_ERR_NOT_FOUND);
    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ACK_LATENCY_MS,
                    &value, &value_len) == PROTO_ERR_NOT_FOUND);
    assert(tlv_find(payload, payload_len, TLV_ANCHOR_DIAG_BYTES, &value, &value_len) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_diagnostic_range_packet_is_not_click(void)
{
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 125u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 90u,
        .range_status = RANGE_OK,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(report_init_range_packet(&packet,
                                    fields.anchor_id,
                                    0x9999888877776666ull,
                                    0x12345678u,
                                    3u,
                                    FLAG_DIAGNOSTIC,
                                    (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_CLICK_REPORT);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(report_init_range_packet(&packet,
                                    fields.anchor_id,
                                    0x9999888877776666ull,
                                    0x12345678u,
                                    0u,
                                    FLAG_DIAGNOSTIC,
                                    (uint8_t)payload_len) == PROTO_ERR_MALFORMED);
    assert(report_init_range_packet(&packet,
                                    fields.anchor_id,
                                    0x9999888877776666ull,
                                    0x12345678u,
                                    3u,
                                    FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK,
                                    (uint8_t)payload_len) == PROTO_ERR_MALFORMED);
}

static void test_timing_invalid_range_report_is_preserved(void)
{
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 126u,
        .timestamp_ms = 987655u,
        .distance_mm = 0,
        .quality = 0u,
        .range_status = RANGE_TIMING_INVALID,
        .omit_rsl = true,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == RANGE_TIMING_INVALID);
}

static void test_range_report_combines_clicker_and_anchor_diagnostics(void)
{
    const uint8_t clicker_diag[] = {0x10u, 0x11u, 0x12u, 0x13u};
    const uint8_t anchor_diag[] = {0xA0u, 0xA1u, 0xA2u};
    const struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT |
                        RANGE_DIAG_ANCHOR_PRESENT |
                        RANGE_DIAG_TRUNCATED |
                        RANGE_DIAG_CHANNEL9_DELIVERED,
        .burst_id = 0x12345678u,
        .exchange_stride_us = 7000u,
        .burst_duration_ms = 200u,
        .click_latency_ms = 640u,
        .uwb_awake_time_us = 22100u,
        .diag_bytes_captured = 19u,
        .diag_bytes_transmitted = sizeof(clicker_diag) + sizeof(anchor_diag),
        .diag_bytes_truncated = 12u,
        .diag_frames_dropped = 1u,
        .report_fragment_count = 3u,
        .channel9_report_latency_ms = 42u,
        .gateway_ack_latency_ms = 18u,
        .phy_config_id = 5u,
        .clicker_diag = clicker_diag,
        .clicker_diag_len = sizeof(clicker_diag),
        .anchor_diag = anchor_diag,
        .anchor_diag_len = sizeof(anchor_diag),
        .click_latency_present = true,
        .channel9_report_latency_present = true,
        .gateway_ack_latency_present = true,
    };
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 126u,
        .timestamp_ms = 987655u,
        .distance_mm = 4567,
        .quality = 90u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .diagnostics = &diagnostics,
    };
    uint8_t payload[220];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_DIAG_STATUS_FLAGS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.status_flags);
    assert(tlv_find(payload, payload_len, TLV_BURST_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.burst_id);
    assert(tlv_find(payload, payload_len, TLV_EXCHANGE_STRIDE_US, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == diagnostics.exchange_stride_us);
    assert(tlv_find(payload, payload_len, TLV_BURST_DURATION_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == diagnostics.burst_duration_ms);
    assert(tlv_find(payload, payload_len, TLV_CLICK_LATENCY_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.click_latency_ms);
    assert(tlv_find(payload, payload_len, TLV_UWB_AWAKE_TIME_US, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.uwb_awake_time_us);
    assert(tlv_find(payload, payload_len, TLV_DIAG_BYTES_CAPTURED, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.diag_bytes_captured);
    assert(tlv_find(payload, payload_len, TLV_DIAG_BYTES_TRANSMITTED, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.diag_bytes_transmitted);
    assert(tlv_find(payload, payload_len, TLV_DIAG_BYTES_TRUNCATED, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.diag_bytes_truncated);
    assert(tlv_find(payload, payload_len, TLV_DIAG_FRAMES_DROPPED, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.diag_frames_dropped);
    assert(tlv_find(payload, payload_len, TLV_REPORT_FRAGMENT_COUNT, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == diagnostics.report_fragment_count);
    assert(tlv_find(payload, payload_len, TLV_CHANNEL9_REPORT_LATENCY_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.channel9_report_latency_ms);
    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ACK_LATENCY_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == diagnostics.gateway_ack_latency_ms);
    assert(tlv_find(payload, payload_len, TLV_PHY_CONFIG_ID, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == diagnostics.phy_config_id);
    assert(tlv_find(payload, payload_len, TLV_CLICKER_DIAG_BYTES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(clicker_diag));
    assert(memcmp(value, clicker_diag, sizeof(clicker_diag)) == 0);
    assert(tlv_find(payload, payload_len, TLV_ANCHOR_DIAG_BYTES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(anchor_diag));
    assert(memcmp(value, anchor_diag, sizeof(anchor_diag)) == 0);
}

static void test_ml_single_sample_diagnostics_fit_and_encode_offsets(void)
{
    const int32_t distance_sample = 4389;
    const uint8_t round_index = 7u;
    const uint64_t sample_timestamp_ms = UINT64_C(1234567890456);
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {
        0x31u, 0x32u, 0x33u, 0x91u, 0x92u, 0x93u,
    };
    const struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT | RANGE_DIAG_ANCHOR_PRESENT,
        .burst_id = 0xA1B2C3D4u,
        .exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
        .burst_duration_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS,
        .diag_bytes_captured = sizeof(cir_sample),
        .diag_bytes_transmitted = sizeof(cir_sample),
        .report_fragment_count = 1u,
        .phy_config_id = UWB_CHANNEL_WAKE_CONTACT,
        .clock_offset_raw = -321,
        .clicker_clock_offset_raw = 456,
        .carrier_integrator = -12345678,
        .clicker_diag = cir_sample,
        .clicker_diag_len = sizeof(cir_sample),
        .clock_offset_present = true,
        .clicker_clock_offset_present = true,
        .carrier_integrator_present = true,
    };
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 127u,
        .timestamp_ms = sample_timestamp_ms,
        .distance_mm = distance_sample,
        .quality = 88u,
        .rsl_dbm = -72,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = &distance_sample,
        .range_round_indices = &round_index,
        .sequence_start_timestamps_ms = &sample_timestamp_ms,
        .sample_index = 7u,
        .sample_count = 32u,
        .distance_sample_count = 1u,
        .diagnostics = &diagnostics,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_OK);
    assert(payload_len <= PACKET_MAX_PAYLOAD_LEN);
    assert(report_init_range_packet(&packet,
                                    fields.clicker_id,
                                    0x9999888877776666ull,
                                    fields.event_seq,
                                    12u,
                                    FLAG_DIAGNOSTIC,
                                    (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_CLICK_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.sample_count);
    assert(tlv_find(payload, payload_len, TLV_SAMPLE_INDEX, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.sample_index);
    assert(tlv_find(payload, payload_len, TLV_DISTANCE_SAMPLES_MM, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(distance_sample));
    assert((int32_t)proto_get_u32_le(value) == distance_sample);
    assert(tlv_find(payload, payload_len, TLV_UWB_RSL_DBM, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert((int8_t)value[0] == fields.rsl_dbm);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_SAMPLE, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(cir_sample));
    assert(memcmp(value, cir_sample, sizeof(cir_sample)) == 0);
    assert(tlv_find(payload, payload_len, TLV_UWB_CLOCK_OFFSET_RAW, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 2u);
    assert((int16_t)proto_get_u16_le(value) == diagnostics.clock_offset_raw);
    assert(tlv_find(payload, payload_len, TLV_CLICKER_CLOCK_OFFSET_RAW,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert((int16_t)proto_get_u16_le(value) ==
           diagnostics.clicker_clock_offset_raw);
    assert(tlv_find(payload, payload_len, TLV_UWB_CARRIER_INTEGRATOR, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == 4u);
    assert((int32_t)proto_get_u32_le(value) == diagnostics.carrier_integrator);
    assert(tlv_find(payload, payload_len, TLV_CLICKER_DIAG_BYTES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == sizeof(cir_sample));
    assert(memcmp(value, cir_sample, sizeof(cir_sample)) == 0);
}

static void test_rejects_unbounded_diagnostic_bytes(void)
{
    uint8_t oversized[RANGE_REPORT_MAX_DIAGNOSTIC_BYTES_SINGLE_PACKET + 1u] = {0};
    const struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT,
        .clicker_diag = oversized,
        .clicker_diag_len = sizeof(oversized),
    };
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 126u,
        .timestamp_ms = 987655u,
        .distance_mm = 4567,
        .quality = 90u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .diagnostics = &diagnostics,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_ERR_NO_SPACE);
}

static void test_full_diagnostic_first_fragment_stays_inside_packet_payload(void)
{
    int32_t distance_samples[5u];
    uint8_t round_indices[5u];
    uint64_t sequence_start_timestamps_ms[5u];
    uint8_t clicker_diag[RANGE_REPORT_MAX_DIAGNOSTIC_BYTES_SINGLE_PACKET] = {0};
    const uint8_t anchor_diag[UWB_CIR_SAMPLE_LEN] = {
        0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u,
    };
    const struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT | RANGE_DIAG_ANCHOR_PRESENT,
        .burst_id = 0x12345678u,
        .exchange_stride_us = 7000u,
        .burst_duration_ms = 200u,
        .click_latency_ms = 20u,
        .uwb_awake_time_us = 30000u,
        .diag_bytes_captured = sizeof(clicker_diag) + sizeof(anchor_diag),
        .diag_bytes_transmitted = sizeof(clicker_diag) + sizeof(anchor_diag),
        .report_fragment_count = 32u,
        .phy_config_id = UWB_CHANNEL_WAKE_CONTACT,
        .clicker_diag = clicker_diag,
        .clicker_diag_len = sizeof(clicker_diag),
        .anchor_diag = anchor_diag,
        .anchor_diag_len = sizeof(anchor_diag),
        .click_latency_present = true,
    };
    struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .cir_sample = anchor_diag,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES,
        .distance_sample_count = 4u,
        .diagnostics = &diagnostics,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    for (uint16_t i = 0u; i < 5u; i++) {
        distance_samples[i] = 4500 + (int32_t)i;
        clicker_diag[i] = (uint8_t)i;
    }
    fill_sample_metadata(round_indices, sequence_start_timestamps_ms, 5u, 0u);

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_OK);
    assert(payload_len <= PACKET_MAX_PAYLOAD_LEN);

    fields.distance_sample_count = 5u;
    payload_len = 0u;
    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_ERR_NO_SPACE);
}

static void test_self_test_report_is_diagnostic_not_click(void)
{
    const struct self_test_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .event_seq = 124u,
        .failure_code = 5u,
        .battery_mv = 3710u,
    };
    uint8_t payload[64];
    uint8_t decoded_payload[64];
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t payload_len = 0u;
    size_t decoded_payload_len = 0u;
    size_t frame_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};
    struct proto_packet decoded_packet = {0};
    uint64_t previous_hop_id = 0u;

    assert(report_append_self_test_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                             fields.clicker_id,
                                             0x9999888877776666ull,
                                             0x12345678u,
                                             2u,
                                             (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_SELF_TEST_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        0x9999888877776666ull,
                                        0x12345678u,
                                        0u,
                                        (uint8_t)payload_len) == PROTO_ERR_MALFORMED);

    assert(tlv_find(payload, payload_len, TLV_CLICKER_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == fields.clicker_id);

    assert(tlv_find(payload, payload_len, TLV_EVENT_SEQ, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == fields.event_seq);

    assert(tlv_find(payload, payload_len, TLV_ERROR_CODE, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.failure_code);

    assert(tlv_find(payload, payload_len, TLV_BATTERY_MV, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.battery_mv);

    assert(uwb_mesh_frame_encode(0x494D4543u,
                                 fields.clicker_id,
                                 packet.dst_id,
                                 &packet,
                                 payload,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    assert(uwb_mesh_frame_decode(frame,
                                 frame_len,
                                 0x494D4543u,
                                 packet.dst_id,
                                 &previous_hop_id,
                                 &decoded_packet,
                                 decoded_payload,
                                 sizeof(decoded_payload),
                                 &decoded_payload_len) == PROTO_OK);
    assert(previous_hop_id == fields.clicker_id);
    assert(decoded_packet.msg_type == MSG_SELF_TEST_REPORT);
    assert((decoded_packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert(decoded_payload_len == payload_len);
    assert(memcmp(decoded_payload, payload, payload_len) == 0);
}

static void test_anchor_heartbeat_report_requires_gateway_ack(void)
{
    const struct anchor_heartbeat_fields fields = {
        .device_role = ROLE_ANCHOR,
        .battery_mv = 0u,
        .status_bits = STATUS_BIT_UWB_SCAN_ACTIVE |
                       STATUS_BIT_UWB_TIMING_REJECTION,
        .uptime_ms = 123456u,
        .timestamp_ms = 9876543210ull,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_anchor_heartbeat_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &fields) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_DEVICE_ROLE, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == ROLE_ANCHOR);
    assert(tlv_find(payload, payload_len, TLV_BATTERY_MV, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.battery_mv);
    assert(tlv_find(payload, payload_len, TLV_STATUS_BITS, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == fields.status_bits);
    assert(tlv_find(payload, payload_len, TLV_UPTIME_MS, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == fields.uptime_ms);
    assert(tlv_find(payload, payload_len, TLV_TIMESTAMP_MS, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == fields.timestamp_ms);
    assert(report_init_anchor_heartbeat_packet(&packet,
                                               0x5555666677778888ull,
                                               0x9999888877776666ull,
                                               0x12345678u,
                                               7u,
                                               (uint8_t)payload_len) == PROTO_OK);
    assert(packet.msg_type == MSG_ANCHOR_HEARTBEAT);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert((packet.flags & FLAG_DIAGNOSTIC) == 0u);
}

static void test_rejects_bad_range_fields(void)
{
    struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 101u,
        .range_status = RANGE_OK,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_ERR_MALFORMED);
    fields.quality = 100u;
    fields.range_status = RANGE_STS_QUALITY_FAIL;
    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
           PROTO_ERR_MALFORMED);
}

static void test_rejects_missing_sample_data(void)
{
    struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 100u,
        .range_status = RANGE_OK,
        .sample_count = 1u,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_ERR_MALFORMED);
}

static void test_max_single_packet_range_samples_fit_one_uwb_mesh_frame(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET];
    uint8_t round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET];
    uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET];
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0x10u, 0x11u, 0x12u, 0x20u, 0x21u, 0x22u};
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};
    struct mesh_outbound outbound = {
        .next_hop_id = 0x9999888877776666ull,
    };
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET; i++) {
        distance_samples[i] = 4500 + (int32_t)i;
    }
    fill_sample_metadata(round_indices,
                         sequence_start_timestamps_ms,
                         RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET,
                         0u);

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= PACKET_MAX_PAYLOAD_LEN);
    assert(report_init_click_packet(&packet,
                                    fields.anchor_id,
                                    outbound.next_hop_id,
                                    0x12345678u,
                                    1u,
                                    (uint8_t)payload_len) == PROTO_OK);

    outbound.packet = packet;
    outbound.payload_len = (uint8_t)payload_len;
    memcpy(outbound.payload, payload, payload_len);

    assert(uwb_mesh_frame_encode(0x494D4543u,
                                 fields.anchor_id,
                                 outbound.next_hop_id,
                                 &outbound.packet,
                                 outbound.payload,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    assert(frame_len <= UWB_MESH_MAX_FRAME_LEN);
}

static void test_first_fragmented_range_chunk_has_single_diagnostics(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    uint8_t round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0x30u, 0x31u, 0x32u, 0x40u, 0x41u, 0x42u};
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES,
        .distance_sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT; i++) {
        distance_samples[i] = 4600 + (int32_t)i;
    }
    fill_sample_metadata(round_indices,
                         sequence_start_timestamps_ms,
                         RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
                         0u);

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= PACKET_MAX_PAYLOAD_LEN);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_INDEX, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == 0u);

    assert(tlv_find(payload, payload_len, TLV_UWB_RSL_DBM, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert((int8_t)value[0] == fields.rsl_dbm);

    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_SAMPLE, &value, &value_len) == PROTO_OK);
    assert(value_len == UWB_CIR_SAMPLE_LEN);
    assert(memcmp(value, cir_sample, UWB_CIR_SAMPLE_LEN) == 0);

    assert(tlv_find(payload, payload_len, TLV_DISTANCE_SAMPLES_MM, &value, &value_len) == PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT * sizeof(int32_t));
    assert(tlv_find(payload, payload_len, TLV_RANGE_ROUND_INDICES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT);
    assert(value[0] == 0u);
    assert(value[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT - 1u] ==
           RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT - 1u);
    assert(tlv_find(payload, payload_len, TLV_SEQUENCE_START_TIMESTAMPS_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT * sizeof(uint64_t));
    assert(proto_get_u64_le(&value[0]) == UINT64_C(1000000));
    assert(proto_get_u64_le(&value[(RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT - 1u) *
                                   sizeof(uint64_t)]) ==
           UINT64_C(1000000) + ((RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT - 1u) * 50u));
}

static void test_later_fragmented_range_chunk_omits_single_diagnostics(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    uint8_t round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0x50u, 0x51u, 0x52u, 0x60u, 0x61u, 0x62u};
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_index = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES,
        .distance_sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
        .omit_rsl = true,
        .omit_cir = true,
    };
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT; i++) {
        distance_samples[i] = 4600 + (int32_t)i;
    }
    fill_sample_metadata(round_indices,
                         sequence_start_timestamps_ms,
                         RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
                         RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT);

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= PACKET_MAX_PAYLOAD_LEN);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == RANGE_REPORT_MAX_DISTANCE_SAMPLES);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_INDEX, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT);

    assert(tlv_find(payload, payload_len, TLV_UWB_RSL_DBM, &value, &value_len) == PROTO_ERR_NOT_FOUND);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_SAMPLE, &value, &value_len) == PROTO_ERR_NOT_FOUND);

    assert(tlv_find(payload, payload_len, TLV_DISTANCE_SAMPLES_MM, &value, &value_len) == PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT * sizeof(int32_t));
    assert(tlv_find(payload, payload_len, TLV_RANGE_ROUND_INDICES, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT);
    assert(value[0] == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT);
    assert(tlv_find(payload, payload_len, TLV_SEQUENCE_START_TIMESTAMPS_MS, &value, &value_len) ==
           PROTO_OK);
    assert(value_len == RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT * sizeof(uint64_t));
}

static void test_partial_cir_fragment_fits_and_encodes_reassembly_metadata(void)
{
    uint8_t chunk[RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES];
    uint8_t oversized_chunk[RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES + 1u];
    struct range_report_cir_fragment fragment = {
        .clicker_id = UINT64_C(0x1111222233334444),
        .anchor_id = UINT64_C(0x5555666677778888),
        .event_seq = 123u,
        .timestamp_ms = UINT64_C(987654),
        .fragment_index = 0u,
        .fragment_count = 2u,
        .byte_offset = 0u,
        .total_bytes = 1152u,
        .first_path_index = 713u,
        .start_index = 649u,
        .chunk = chunk,
        .chunk_len = sizeof(chunk),
    };
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;
    size_t copied = 0u;
    size_t offset = 0u;
    unsigned int chunk_tlv_count = 0u;

    for (size_t i = 0u; i < sizeof(chunk); i++) {
        chunk[i] = (uint8_t)i;
    }

    assert(report_append_cir_fragment_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           &fragment) == PROTO_OK);
    assert(payload_len == RANGE_REPORT_CIR_PACKET_METADATA_BYTES + sizeof(chunk) +
                          (2u * RANGE_REPORT_CIR_PACKET_CHUNK_TLV_COUNT));
    assert(payload_len + MESH_CH9_BATCH_METADATA_TLV_BYTES ==
           PACKET_EXT_MAX_PAYLOAD_LEN);
    assert(tlv_find(payload, payload_len, TLV_DIAG_FRAGMENT_INDEX,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    assert(proto_get_u16_le(value) == fragment.fragment_index);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_BYTE_OFFSET,
                    &value, &value_len) == PROTO_OK);
    assert(proto_get_u16_le(value) == fragment.byte_offset);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_TOTAL_BYTES,
                    &value, &value_len) == PROTO_OK);
    assert(proto_get_u16_le(value) == fragment.total_bytes);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_FIRST_PATH_INDEX,
                    &value, &value_len) == PROTO_OK);
    assert(proto_get_u16_le(value) == fragment.first_path_index);
    assert(tlv_find(payload, payload_len, TLV_UWB_CIR_START_INDEX,
                    &value, &value_len) == PROTO_OK);
    assert(proto_get_u16_le(value) == fragment.start_index);
    while (offset < payload_len) {
        uint8_t type = payload[offset];
        uint8_t len = payload[offset + 1u];

        offset += 2u;
        assert(payload_len - offset >= len);
        if (type == TLV_UWB_CIR_FULL_CHUNK) {
            assert(sizeof(chunk) - copied >= len);
            assert(memcmp(&payload[offset], &chunk[copied], len) == 0);
            copied += len;
            chunk_tlv_count++;
        }
        offset += len;
    }
    assert(copied == sizeof(chunk));
    assert(chunk_tlv_count == RANGE_REPORT_CIR_PACKET_CHUNK_TLV_COUNT);

    fragment.fragment_index = 1u;
    fragment.byte_offset = sizeof(chunk);
    fragment.chunk_len = RANGE_REPORT_CIR_REMAINDER_RAW_BYTES;
    payload_len = 0u;
    assert(report_append_cir_fragment_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           &fragment) == PROTO_OK);
    assert(payload_len == RANGE_REPORT_CIR_REMAINDER_PAYLOAD_BYTES);

    payload_len = 0u;
    assert(report_append_cir_fragment_tlvs(payload,
                                           64u,
                                           &payload_len,
                                           &fragment) == PROTO_ERR_NO_SPACE);

    fragment.chunk = oversized_chunk;
    fragment.chunk_len = sizeof(oversized_chunk);
    payload_len = 0u;
    assert(report_append_cir_fragment_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           &fragment) == PROTO_ERR_ARG);
}

static void test_click_report_adds_optional_detection_attempt_metadata(void)
{
    struct range_report_fields fields = {
        .clicker_id = UINT64_C(0x1111),
        .anchor_id = UINT64_C(0x2222),
        .event_seq = 7u,
        .timestamp_ms = 8u,
        .distance_mm = 1234,
        .quality = 90u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .omit_cir = true,
        .attempt_index = 3u,
        .detection_source = DETECTION_SOURCE_UWB_WAKE_CLAIM,
        .detection_attempt_present = true,
    };
    uint8_t payload[128];
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len,
                                    &fields) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_ATTEMPT_INDEX,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == 1u && value[0] == 3u);
    assert(tlv_find(payload, payload_len, TLV_DETECTION_SOURCE,
                    &value, &value_len) == PROTO_OK);
    assert(value_len == 1u && value[0] == DETECTION_SOURCE_UWB_WAKE_CLAIM);

    fields.detection_attempt_present = false;
    payload_len = 0u;
    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len,
                                    &fields) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_ATTEMPT_INDEX,
                    &value, &value_len) == PROTO_ERR_NOT_FOUND);
}

static void test_click_payload_semantic_validation(void)
{
    const int32_t samples[] = {1234};
    const uint8_t rounds[] = {0u};
    const uint64_t starts[] = {UINT64_C(1000)};
    struct range_report_fields fields = {
        .clicker_id = UINT64_C(0x1111),
        .anchor_id = UINT64_C(0x2222),
        .event_seq = 7u,
        .timestamp_ms = 8u,
        .distance_mm = 1234,
        .quality = 90u,
        .range_status = RANGE_OK,
        .distance_samples_mm = samples,
        .range_round_indices = rounds,
        .sequence_start_timestamps_ms = starts,
        .sample_count = 1u,
        .burst_id = 9u,
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
        .attempt_index = 3u,
        .detection_source = DETECTION_SOURCE_UWB_WAKE_CLAIM,
        .detection_attempt_present = true,
    };
    struct proto_packet packet;
    uint8_t payload[160];
    uint8_t mutated[160];
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t payload_len = 0u;
    size_t mutated_len;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len,
                                    &fields) == PROTO_OK);
    assert(report_init_click_packet(&packet, fields.anchor_id,
                                    UINT64_C(0x9999), fields.event_seq, 1u,
                                    (uint8_t)payload_len) == PROTO_OK);
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_OK);

    packet.flags |= FLAG_ROUTE_SETUP;
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);
    packet.flags &= (uint8_t)~FLAG_ROUTE_SETUP;

    memcpy(mutated, payload, payload_len);
    assert(tlv_find(mutated, payload_len, TLV_ANCHOR_ID,
                    &value, &value_len) == PROTO_OK);
    proto_put_u64_le((uint8_t *)value, UINT64_C(0x3333));
    assert(report_validate_click_payload(&packet, mutated, payload_len) ==
           PROTO_ERR_MALFORMED);

    memcpy(mutated, payload, payload_len);
    assert(tlv_find(mutated, payload_len, TLV_EVENT_SEQ,
                    &value, &value_len) == PROTO_OK);
    proto_put_u32_le((uint8_t *)value, fields.event_seq + 1u);
    assert(report_validate_click_payload(&packet, mutated, payload_len) ==
           PROTO_ERR_MALFORMED);

    memcpy(mutated, payload, payload_len);
    assert(tlv_find(mutated, payload_len, TLV_RANGE_STATUS,
                    &value, &value_len) == PROTO_OK);
    mutated[(size_t)(value - mutated) - PROTO_TLV_HEADER_LEN] = 0xfeu;
    assert(report_validate_click_payload(&packet, mutated, payload_len) ==
           PROTO_ERR_MALFORMED);

    memcpy(mutated, payload, payload_len);
    mutated_len = payload_len;
    assert(tlv_append_u8(mutated, sizeof(mutated), &mutated_len,
                         TLV_QUALITY, 90u) == PROTO_OK);
    packet.payload_len = (uint16_t)mutated_len;
    assert(report_validate_click_payload(&packet, mutated, mutated_len) ==
           PROTO_ERR_MALFORMED);
    packet.payload_len = (uint16_t)payload_len;

    memcpy(mutated, payload, payload_len);
    assert(tlv_find(mutated, payload_len, TLV_DETECTION_SOURCE,
                    &value, &value_len) == PROTO_OK);
    mutated[(size_t)(value - mutated) - PROTO_TLV_HEADER_LEN] = 0xfeu;
    assert(report_validate_click_payload(&packet, mutated, payload_len) ==
           PROTO_ERR_MALFORMED);

    fields.burst_id_present = false;
    payload_len = 0u;
    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len,
                                    &fields) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);

    fields.burst_id_present = true;
    fields.sample_count = 0u;
    fields.distance_samples_mm = NULL;
    fields.range_round_indices = NULL;
    fields.sequence_start_timestamps_ms = NULL;
    payload_len = 0u;
    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len,
                                    &fields) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);
}

static void test_click_payload_semantic_validation_accepts_cir_fragment(void)
{
    const uint8_t cir[] = {1u, 2u, 3u, 4u};
    const struct range_report_cir_fragment fragment = {
        .clicker_id = UINT64_C(0x1111),
        .anchor_id = UINT64_C(0x2222),
        .event_seq = 7u,
        .timestamp_ms = 8u,
        .fragment_index = 0u,
        .fragment_count = 1u,
        .byte_offset = 0u,
        .total_bytes = sizeof(cir),
        .first_path_index = 2u,
        .start_index = 3u,
        .chunk = cir,
        .chunk_len = sizeof(cir),
    };
    struct proto_packet packet;
    uint8_t payload[128];
    size_t payload_len = 0u;

    assert(report_append_cir_fragment_tlvs(payload, sizeof(payload),
                                           &payload_len, &fragment) ==
           PROTO_OK);
    assert(report_init_range_packet(&packet, fragment.anchor_id,
                                    UINT64_C(0x9999), fragment.event_seq, 1u,
                                    FLAG_DIAGNOSTIC,
                                    (uint16_t)payload_len) == PROTO_OK);
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_OK);

    packet.flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK |
                   FLAG_GATEWAY_ACK_REQUIRED;
    assert(report_validate_click_payload(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_range_transport_sequence_is_unique_across_retry_attempts();
    test_click_report_packet_counts_as_click();
    test_diagnostic_range_packet_is_not_click();
    test_timing_invalid_range_report_is_preserved();
    test_range_report_combines_clicker_and_anchor_diagnostics();
    test_ml_single_sample_diagnostics_fit_and_encode_offsets();
    test_rejects_unbounded_diagnostic_bytes();
    test_full_diagnostic_first_fragment_stays_inside_packet_payload();
    test_self_test_report_is_diagnostic_not_click();
    test_anchor_heartbeat_report_requires_gateway_ack();
    test_rejects_bad_range_fields();
    test_rejects_missing_sample_data();
    test_max_single_packet_range_samples_fit_one_uwb_mesh_frame();
    test_first_fragmented_range_chunk_has_single_diagnostics();
    test_later_fragmented_range_chunk_omits_single_diagnostics();
    test_partial_cir_fragment_fits_and_encodes_reassembly_metadata();
    test_click_report_adds_optional_detection_attempt_metadata();
    test_click_payload_semantic_validation();
    test_click_payload_semantic_validation_accepts_cir_fragment();
    return 0;
}
