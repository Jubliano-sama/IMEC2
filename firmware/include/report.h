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

struct range_report_fields {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint32_t event_seq;
    uint64_t timestamp_ms;
    uint32_t time_sync_age_ms;
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
    uint32_t time_sync_age_ms;
};

int report_append_range_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct range_report_fields *fields);
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
                                  uint8_t payload_len);
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
