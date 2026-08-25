#ifndef SURVEY_RESPONSE_LANE_H
#define SURVEY_RESPONSE_LANE_H

#include "enumeration_response_lane.h"
#include "survey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_RESPONSE_RECORD_WIRE_LEN 8u
#define SURVEY_RESPONSE_RECORDS_PER_BUNDLE 20u
#define SURVEY_RESPONSE_MAX_RECORDS SURVEY_MAX_PAIRS
#define SURVEY_RESPONSE_MAX_BUNDLES \
    ((SURVEY_RESPONSE_MAX_RECORDS + SURVEY_RESPONSE_RECORDS_PER_BUNDLE - 1u) / \
     SURVEY_RESPONSE_RECORDS_PER_BUNDLE)
#define SURVEY_RESPONSE_NO_OFFSET ENUMERATION_RESPONSE_NO_OFFSET
#define UWB_SURVEY_PRESENCE_LEN 22u
#define UWB_SURVEY_BUNDLE_BASE_LEN 32u
#define UWB_SURVEY_BUNDLE_MAX_LEN \
    (UWB_SURVEY_BUNDLE_BASE_LEN + \
     (SURVEY_RESPONSE_RECORDS_PER_BUNDLE * \
      SURVEY_RESPONSE_RECORD_WIRE_LEN))
#define UWB_SURVEY_HOP_ACK_LEN 31u

enum survey_response_kind {
    SURVEY_RESPONSE_NEIGHBORS = 1,
    SURVEY_RESPONSE_RANGES = 2,
};

struct survey_response_record {
    uint8_t bytes[SURVEY_RESPONSE_RECORD_WIRE_LEN];
};

struct survey_response_bundle {
    uint32_t network_id;
    uint32_t generation;
    uint64_t sender_id;
    uint64_t parent_id;
    enum survey_response_kind kind;
    uint8_t sequence;
    uint8_t record_count;
    struct survey_response_record records[SURVEY_RESPONSE_RECORDS_PER_BUNDLE];
};

struct survey_response_hop_ack {
    uint32_t network_id;
    uint32_t generation;
    uint64_t parent_id;
    uint64_t child_id;
    enum survey_response_kind kind;
    uint8_t sequence;
};

struct survey_presence_frame {
    uint32_t network_id;
    uint32_t generation;
    uint64_t sender_id;
    uint8_t sender_slot;
};

struct survey_response_lane {
    struct survey_response_record records[SURVEY_RESPONSE_MAX_RECORDS];
    uint64_t local_id;
    uint64_t parent_id;
    uint64_t start_ms;
    uint32_t network_id;
    uint32_t generation;
    uint16_t acked_mask;
    uint16_t attempted_mask;
    uint8_t round_offsets_ms[SURVEY_RESPONSE_MAX_BUNDLES];
    enum survey_response_kind kind;
    uint8_t hop_count;
    uint8_t max_hop_count;
    uint8_t record_count;
    uint8_t prepared_round;
    bool active;
};

int survey_response_lane_begin(
    struct survey_response_lane *lane,
    uint32_t network_id,
    uint32_t generation,
    uint64_t local_id,
    uint64_t parent_id,
    enum survey_response_kind kind,
    uint8_t hop_count,
    uint8_t max_hop_count,
    uint64_t start_ms);
void survey_response_lane_stop(struct survey_response_lane *lane);
int survey_response_lane_add_record(
    struct survey_response_lane *lane,
    const struct survey_response_record *record,
    bool *added);
int survey_response_lane_merge_bundle(
    struct survey_response_lane *lane,
    const struct survey_response_bundle *bundle,
    bool *added_records);
int survey_response_lane_prepare_round(
    struct survey_response_lane *lane,
    uint8_t round,
    uint32_t random_value);
int survey_response_lane_bundle_for_offset(
    struct survey_response_lane *lane,
    const struct enumeration_response_timing *timing,
    struct survey_response_bundle *bundle);
bool survey_response_lane_note_ack(
    struct survey_response_lane *lane,
    const struct survey_response_hop_ack *ack);
bool survey_response_lane_all_acked(const struct survey_response_lane *lane);
uint8_t survey_response_lane_bundle_count(
    const struct survey_response_lane *lane);
uint8_t survey_response_lane_round_offset_ms(
    const struct survey_response_lane *lane,
    uint8_t sequence);
uint8_t survey_response_lane_next_offset_ms(
    const struct survey_response_lane *lane,
    const struct enumeration_response_timing *timing);
const struct survey_response_record *survey_response_lane_record(
    const struct survey_response_lane *lane,
    uint8_t index);

int uwb_encode_survey_presence(const struct survey_presence_frame *frame,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written);
int uwb_decode_survey_presence(const uint8_t *data,
                               size_t data_len,
                               struct survey_presence_frame *frame);
size_t uwb_survey_bundle_encoded_len(uint8_t record_count);
int uwb_encode_survey_bundle(const struct survey_response_bundle *bundle,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written);
int uwb_decode_survey_bundle(const uint8_t *data,
                             size_t data_len,
                             struct survey_response_bundle *bundle);
int uwb_encode_survey_hop_ack(const struct survey_response_hop_ack *ack,
                              uint8_t *out,
                              size_t out_cap,
                              size_t *written);
int uwb_decode_survey_hop_ack(const uint8_t *data,
                              size_t data_len,
                              struct survey_response_hop_ack *ack);

#ifdef __cplusplus
}
#endif

#endif
