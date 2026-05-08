#include "report.h"

#include "mesh_ble.h"

#include <assert.h>
#include <string.h>

static void test_click_report_packet_counts_as_click(void)
{
    const int32_t distance_samples[] = {4550, 4567, 4580};
    const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0x01u, 0x02u, 0x03u, 0xF1u, 0xF2u, 0xF3u};
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
        .sample_count = 3u,
    };
    uint8_t payload[160];
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
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == fields.timestamp_ms);

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
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

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

    assert(tlv_find(payload, payload_len, TLV_ERROR_CODE, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.failure_code);
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

static void test_max_single_packet_range_samples_fit_one_mesh_frame(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET];
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
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET,
    };
    uint8_t payload[MESH_BLE_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};
    struct mesh_outbound outbound = {
        .next_hop_id = 0x9999888877776666ull,
    };
    uint8_t frame[MESH_BLE_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET; i++) {
        distance_samples[i] = 4500 + (int32_t)i;
    }

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= MESH_BLE_MAX_PAYLOAD_LEN);
    assert(report_init_click_packet(&packet,
                                    fields.anchor_id,
                                    outbound.next_hop_id,
                                    0x12345678u,
                                    1u,
                                    (uint8_t)payload_len) == PROTO_OK);

    outbound.packet = packet;
    outbound.payload_len = (uint8_t)payload_len;
    memcpy(outbound.payload, payload, payload_len);

    assert(mesh_ble_frame_encode(fields.anchor_id,
                                 &outbound,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    assert(frame_len <= MESH_BLE_MAX_FRAME_LEN);
}

static void test_first_fragmented_range_chunk_has_single_diagnostics(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
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
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES,
        .distance_sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
    };
    uint8_t payload[MESH_BLE_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT; i++) {
        distance_samples[i] = 4600 + (int32_t)i;
    }

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= MESH_BLE_MAX_PAYLOAD_LEN);

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
}

static void test_later_fragmented_range_chunk_omits_single_diagnostics(void)
{
    int32_t distance_samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT];
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
        .sample_index = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
        .sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES,
        .distance_sample_count = RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT,
        .omit_rsl = true,
        .omit_cir = true,
    };
    uint8_t payload[MESH_BLE_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    for (uint16_t i = 0u; i < RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT; i++) {
        distance_samples[i] = 4600 + (int32_t)i;
    }

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(payload_len <= MESH_BLE_MAX_PAYLOAD_LEN);

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
}

int main(void)
{
    test_click_report_packet_counts_as_click();
    test_self_test_report_is_diagnostic_not_click();
    test_rejects_bad_range_fields();
    test_rejects_missing_sample_data();
    test_max_single_packet_range_samples_fit_one_mesh_frame();
    test_first_fragmented_range_chunk_has_single_diagnostics();
    test_later_fragmented_range_chunk_omits_single_diagnostics();
    return 0;
}
