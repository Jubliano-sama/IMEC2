#ifndef REPORT_H
#define REPORT_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REPORT_DEFAULT_TTL 4u
#define RANGE_REPORT_MAX_DISTANCE_SAMPLES 96u
#define RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET 13u
#define RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT 13u
#define RANGE_REPORT_MAX_DIAGNOSTIC_BYTES_SINGLE_PACKET 48u
#define RANGE_REPORT_CIR_WINDOW_RAW_BYTES 1152u
#define RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES 881u
#define RANGE_REPORT_CIR_PACKET_METADATA_BYTES 60u
#define RANGE_REPORT_CIR_PACKET_CHUNK_TLV_COUNT \
    ((RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES + UINT8_MAX - 1u) / UINT8_MAX)
#define RANGE_REPORT_CIR_REMAINDER_RAW_BYTES \
    (RANGE_REPORT_CIR_WINDOW_RAW_BYTES - RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES)
#define RANGE_REPORT_CIR_REMAINDER_CHUNK_TLV_COUNT \
    ((RANGE_REPORT_CIR_REMAINDER_RAW_BYTES + UINT8_MAX - 1u) / UINT8_MAX)
#define RANGE_REPORT_CIR_REMAINDER_PAYLOAD_BYTES \
    (RANGE_REPORT_CIR_PACKET_METADATA_BYTES + RANGE_REPORT_CIR_REMAINDER_RAW_BYTES + \
     (2u * RANGE_REPORT_CIR_REMAINDER_CHUNK_TLV_COUNT))

enum range_diagnostic_status_flag {
    RANGE_DIAG_CLICKER_PRESENT = 1u << 0,
    RANGE_DIAG_CLICKER_MISSING = 1u << 1,
    RANGE_DIAG_ANCHOR_PRESENT = 1u << 2,
    RANGE_DIAG_ANCHOR_MISSING = 1u << 3,
    RANGE_DIAG_TRUNCATED = 1u << 4,
    RANGE_DIAG_CAPTURE_FAILED = 1u << 5,
    RANGE_DIAG_CHANNEL9_DELIVERED = 1u << 6,
};

struct range_report_diagnostics {
    uint32_t status_flags;
    uint32_t burst_id;
    uint16_t exchange_stride_us;
    uint16_t burst_duration_ms;
    uint32_t click_latency_ms;
    uint32_t uwb_awake_time_us;
    uint32_t diag_bytes_captured;
    uint32_t diag_bytes_transmitted;
    uint32_t diag_bytes_truncated;
    uint32_t diag_frames_dropped;
    uint16_t report_fragment_count;
    uint32_t channel9_report_latency_ms;
    uint32_t gateway_ack_latency_ms;
    uint8_t phy_config_id;
    int16_t clock_offset_raw;
    int16_t clicker_clock_offset_raw;
    int32_t carrier_integrator;
    const uint8_t *clicker_diag;
    uint8_t clicker_diag_len;
    const uint8_t *anchor_diag;
    uint8_t anchor_diag_len;
    bool click_latency_present;
    bool channel9_report_latency_present;
    bool gateway_ack_latency_present;
    bool clock_offset_present;
    bool clicker_clock_offset_present;
    bool carrier_integrator_present;
};

struct range_report_fields {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint32_t event_seq;
    uint64_t timestamp_ms;
    int32_t distance_mm;
    uint8_t quality;
    int8_t rsl_dbm;
    const uint8_t *cir_sample;
    enum range_status range_status;
    const int32_t *distance_samples_mm;
    const uint8_t *range_round_indices;
    const uint64_t *sequence_start_timestamps_ms;
    uint16_t sample_index;
    uint16_t sample_count;
    uint16_t distance_sample_count;
    bool omit_rsl;
    bool omit_cir;
    const struct range_report_diagnostics *diagnostics;
};

struct range_report_cir_fragment {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint32_t event_seq;
    uint64_t timestamp_ms;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t byte_offset;
    uint16_t total_bytes;
    uint16_t first_path_index;
    uint16_t start_index;
    const uint8_t *chunk;
    uint16_t chunk_len;
};

struct self_test_report_fields {
    uint64_t clicker_id;
    uint32_t event_seq;
    uint8_t failure_code;
    uint16_t battery_mv;
};

struct anchor_heartbeat_fields {
    uint8_t device_role;
    uint16_t battery_mv;
    uint32_t status_bits;
    uint32_t uptime_ms;
    uint64_t timestamp_ms;
};

int report_append_range_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct range_report_fields *fields);
int report_append_cir_fragment_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct range_report_cir_fragment *fragment);
int report_append_self_test_tlvs(uint8_t *payload,
                                      size_t payload_cap,
                                      size_t *offset,
                                      const struct self_test_report_fields *fields);
int report_append_anchor_heartbeat_tlvs(uint8_t *payload,
                                        size_t payload_cap,
                                        size_t *offset,
                                        const struct anchor_heartbeat_fields *fields);
int report_init_range_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t report_flags,
                                  uint16_t payload_len);
int report_init_click_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len);
int report_init_self_test_packet(struct proto_packet *packet,
                                      uint64_t clicker_id,
                                      uint64_t gateway_id,
                                      uint32_t session_id,
                                      uint16_t seq,
                                      uint8_t payload_len);
int report_init_anchor_heartbeat_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t session_id,
                                        uint16_t seq,
                                        uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
