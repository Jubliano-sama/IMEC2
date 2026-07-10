#ifndef SURVEY_H
#define SURVEY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_MIN_SAMPLE_COUNT 1u
#define SURVEY_MAX_SAMPLE_COUNT 1000u
#define SURVEY_DEFAULT_TTL 4u
#define SURVEY_REACHABILITY_ENTRY_LEN 10u
#define SURVEY_GATEWAY_MAX_REPORTS 50u
#define SURVEY_GATEWAY_MAX_PEERS_PER_REPORT 8u
#define SURVEY_GATEWAY_MAX_PAIRS 128u
#define SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR 4u
#define SURVEY_DISCOVERY_MAX_SLOT_COUNT 50u
#define SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT 2u
#define SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT 8u
#define SURVEY_DISCOVERY_MIN_SLOT_MS 10u
#define SURVEY_DISCOVERY_MAX_SLOT_MS 1000u
#define SURVEY_DISCOVERY_MAX_START_DELAY_MS 60000u

struct survey_reachability_entry {
    uint64_t peer_id;
    int8_t rssi_dbm;
    uint8_t quality;
};

struct survey_pair {
    uint32_t survey_id;
    uint64_t initiator_id;
    uint64_t responder_id;
    uint16_t sample_count;
};

struct survey_sample {
    struct survey_pair pair;
    uint16_t sample_index;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status range_status;
};

struct survey_reachability_report {
    uint64_t anchor_id;
    const struct survey_reachability_entry *entries;
    size_t entry_count;
};

struct survey_discovery_config {
    uint32_t survey_id;
    uint32_t start_delay_ms;
    uint16_t slot_ms;
    uint8_t slot_count;
};

struct survey_ml_anchor_pair_request {
    uint8_t discovery_slot_count;
};

struct survey_discovery_timing {
    uint32_t wait_ms;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool pending;
    bool active;
    bool expired;
};

struct survey_gateway_report_slot {
    uint64_t anchor_id;
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    size_t entry_count;
    bool valid;
};

struct survey_gateway_context {
    uint32_t survey_id;
    uint16_t sample_count;
    struct survey_gateway_report_slot reports[SURVEY_GATEWAY_MAX_REPORTS];
    struct survey_pair pairs[SURVEY_GATEWAY_MAX_PAIRS];
    size_t report_count;
    size_t pair_count;
    size_t next_pair_index;
    bool pairs_planned;
};

enum survey_gateway_auto_stage {
    SURVEY_GATEWAY_AUTO_IDLE = 0,
    SURVEY_GATEWAY_AUTO_LOAD_PAIR,
    SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
    SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
    SURVEY_GATEWAY_AUTO_START_RESPONDER,
    SURVEY_GATEWAY_AUTO_START_INITIATOR,
};

struct survey_gateway_auto_context {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    bool running;
    bool waiting;
};

struct survey_gateway_auto_action {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    enum command_id command_id;
    uint64_t target_id;
    bool complete;
};

bool survey_sample_count_valid(uint16_t sample_count);
int survey_pair_validate(const struct survey_pair *pair);
int survey_sample_validate(const struct survey_sample *sample);
uint64_t survey_sample_nonce(const struct survey_pair *pair, uint16_t sample_index);
int survey_reachability_entry_validate(const struct survey_reachability_entry *entry);
int survey_discovery_config_validate(const struct survey_discovery_config *config);
uint32_t survey_discovery_duration_ms(const struct survey_discovery_config *config);
int survey_discovery_timing_from_age(const struct survey_discovery_config *config,
                                     uint32_t message_age_ms,
                                     struct survey_discovery_timing *timing);
int survey_discovery_report_delay_ms(const struct survey_discovery_config *config,
                                     uint8_t anchor_slot,
                                     uint32_t report_slot_ms,
                                     uint32_t *delay_ms);
int survey_gateway_begin(struct survey_gateway_context *context,
                         uint32_t survey_id,
                         uint16_t sample_count);
int survey_gateway_note_reach_report(struct survey_gateway_context *context,
                                     uint32_t survey_id,
                                     uint64_t anchor_id,
                                     const struct survey_reachability_entry *entries,
                                     size_t entry_count);
int survey_gateway_plan_pairs(struct survey_gateway_context *context);
int survey_gateway_next_pair(struct survey_gateway_context *context,
                             struct survey_pair *pair);
int survey_gateway_auto_begin(struct survey_gateway_auto_context *context);
int survey_gateway_auto_next_action(struct survey_gateway_auto_context *auto_context,
                                    struct survey_gateway_context *gateway_context,
                                    struct survey_gateway_auto_action *action);
int survey_gateway_auto_mark_waiting(struct survey_gateway_auto_context *context);
bool survey_gateway_auto_command_matches(const struct survey_gateway_auto_context *context,
                                         enum command_id command_id,
                                         uint64_t target_id,
                                         uint32_t survey_id);
int survey_gateway_auto_retry_pending(struct survey_gateway_auto_context *context,
                                      enum command_id command_id,
                                      uint64_t target_id,
                                      uint32_t survey_id);
int survey_gateway_auto_note_result(struct survey_gateway_auto_context *context,
                                    enum command_id command_id,
                                    uint64_t target_id,
                                    uint32_t survey_id,
                                    enum command_status status,
                                    bool *pair_launched,
                                    bool *pair_skipped);
int survey_extract_reach_request_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t *survey_id,
                                      uint32_t *duration_ms);
int survey_extract_reach_report_tlvs(const uint8_t *payload,
                                     size_t payload_len,
                                     uint32_t *survey_id,
                                     uint64_t *anchor_id,
                                     struct survey_reachability_entry *entries,
                                     size_t entry_cap,
                                     size_t *entry_count);
int survey_extract_discovery_start_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct survey_discovery_config *config);
int survey_extract_discovery_slot_count_tlv(const uint8_t *payload,
                                            size_t payload_len,
                                            uint8_t default_slot_count,
                                            uint8_t *slot_count);
int survey_extract_ml_anchor_pair_request_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t default_slot_count,
    struct survey_ml_anchor_pair_request *request);
int survey_extract_pair_tlvs(const uint8_t *payload,
                             size_t payload_len,
                             struct survey_pair *pair);
int survey_plan_pairs_from_reachability(uint32_t survey_id,
                                        const struct survey_reachability_report *reports,
                                        size_t report_count,
                                        uint16_t sample_count,
                                        struct survey_pair *pairs,
                                        size_t pair_cap,
                                        size_t *pair_count);
int survey_append_reach_request_tlvs(uint8_t *payload,
                                          size_t payload_cap,
                                          size_t *offset,
                                          uint32_t survey_id,
                                          uint32_t duration_ms);
int survey_append_discovery_start_tlvs(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *offset,
                                       const struct survey_discovery_config *config);
int survey_append_reachability_entry_tlv(uint8_t *payload,
                                              size_t payload_cap,
                                              size_t *offset,
                                              const struct survey_reachability_entry *entry);
int survey_append_reach_report_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         uint32_t survey_id,
                                         uint64_t anchor_id,
                                         const struct survey_reachability_entry *entries,
                                         size_t entry_count);
int survey_append_pair_tlvs(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *offset,
                                 const struct survey_pair *pair);
int survey_append_sample_tlvs(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct survey_sample *sample);
int survey_init_result_packet(struct proto_packet *packet,
                                   const struct survey_sample *sample,
                                   uint64_t gateway_id,
                                   uint16_t seq,
                                   uint8_t payload_len);
int survey_init_result_packet_from_reporter(struct proto_packet *packet,
                                            const struct survey_sample *sample,
                                            uint64_t reporter_id,
                                            uint64_t gateway_id,
                                            uint16_t seq,
                                            uint8_t payload_len);
int survey_init_reach_request_packet(struct proto_packet *packet,
                                     uint64_t gateway_id,
                                     uint32_t survey_id,
                                     uint16_t seq,
                                     uint8_t payload_len);
int survey_init_reach_report_packet(struct proto_packet *packet,
                                         uint64_t anchor_id,
                                         uint64_t gateway_id,
                                         uint32_t survey_id,
                                         uint16_t seq,
                                         uint8_t payload_len);
int survey_init_discovery_start_packet(struct proto_packet *packet,
                                       uint64_t gateway_id,
                                       const struct survey_discovery_config *config,
                                       uint16_t seq,
                                       uint8_t payload_len);
int survey_init_discovery_report_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t survey_id,
                                        uint16_t seq,
                                        uint8_t payload_len);
int survey_init_pair_prepare_packet(struct proto_packet *packet,
                                    const struct survey_pair *pair,
                                    uint64_t gateway_id,
                                    uint16_t seq,
                                    uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
