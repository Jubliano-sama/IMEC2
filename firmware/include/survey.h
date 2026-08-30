#ifndef SURVEY_H
#define SURVEY_H

#include "discovery_assignment.h"
#include "mesh_capacity.h"
#include "semantic_digest.h"
#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_PROTOCOL_VERSION 1u
#define SURVEY_MAX_ANCHORS MESH_CONNECTED_MAX_ANCHORS
#define SURVEY_MAX_DEGREE 4u
#define SURVEY_MAX_PAIRS \
    ((SURVEY_MAX_ANCHORS * SURVEY_MAX_DEGREE) / 2u)
#define SURVEY_NEIGHBOR_BITMAP_BYTES \
    ((SURVEY_MAX_ANCHORS + 7u) / 8u)
#define SURVEY_NEIGHBOR_RECORD_WIRE_LEN \
    (1u + SURVEY_NEIGHBOR_BITMAP_BYTES)
#define SURVEY_PAIR_REQUEST_WIRE_LEN 2u
#define SURVEY_PLAN_PAIR_WIRE_LEN 3u
#define SURVEY_RANGE_RESULT_WIRE_LEN 8u
#define SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN \
    (sizeof(uint32_t) + sizeof(uint32_t) + \
     sizeof(struct discovery_assignment_table_commitment) + 2u)
#define SURVEY_PLAN_HEADER_WIRE_LEN \
    (1u + sizeof(uint32_t) + SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN + \
     sizeof(uint32_t) + sizeof(uint32_t) + 2u + \
     SEMANTIC_DIGEST_SHA256_LEN)
#define SURVEY_PLAN_MAX_WIRE_LEN \
    (SURVEY_PLAN_HEADER_WIRE_LEN + \
     (SURVEY_MAX_PAIRS * SURVEY_PLAN_PAIR_WIRE_LEN))
#define SURVEY_GRAPH_MAX_WIRE_LEN \
    (SURVEY_MAX_ANCHORS * SURVEY_NEIGHBOR_RECORD_WIRE_LEN)
#define SURVEY_RESULTS_MAX_WIRE_LEN \
    (SURVEY_MAX_PAIRS * SURVEY_RANGE_RESULT_WIRE_LEN)

#define SURVEY_NEIGHBOR_SLOT_MS 1000u
#define SURVEY_NEIGHBOR_QUIET_MARGIN_MS 100u
#define SURVEY_NEIGHBOR_BEACON_COUNT 5u
#define SURVEY_NEIGHBOR_BEACON_SPACING_MS 160u
#define SURVEY_RANGE_WAVE_MS 600u
#define SURVEY_RESPONDER_HEAD_START_MS 100u
#define SURVEY_RANGE_ATTEMPT_COUNT 5u
#define SURVEY_RANGE_ATTEMPT_SPACING_MS 80u
#define SURVEY_RESULT_PREPARE_MS 40u
#define SURVEY_RADIO_GUARD_MS 60u
#define SURVEY_CONTROL_ORIGIN_BUDGET_MS \
    NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS
#define SURVEY_CONTROL_PROPAGATION_MARGIN_MS \
    DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS
#define SURVEY_CONTROL_PER_HOP_BUDGET_MS \
    DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS
#define SURVEY_CONTROL_REDUNDANCY_MS \
    DISCOVERY_ASSIGNMENT_CONTROL_LISTENER_REDUNDANCY_MS
#define SURVEY_EXTRA_DRAIN_STRIDES 2u
#define SURVEY_INITIAL_SELF_EXPIRY_MS 180000u
#define SURVEY_HOST_PLAN_TIMEOUT_MS 60000u
#define SURVEY_ENUMERATION_HANDOFF_GUARD_MS 5000u
#define SURVEY_ENUMERATION_HANDOFF_HOLD_MS \
    (SURVEY_HOST_PLAN_TIMEOUT_MS + SURVEY_ENUMERATION_HANDOFF_GUARD_MS)
#define SURVEY_HARD_CAP_MS 1800000u
#define SURVEY_MIN_SUCCESSFUL_RANGES 3u
#define SURVEY_NO_MEDIAN_MM INT32_MIN

enum survey_phase {
    SURVEY_PHASE_NEIGHBOR_START = 1,
    SURVEY_PHASE_PLAN = 2,
    SURVEY_PHASE_ABORT = 3,
};

enum survey_terminal_status {
    SURVEY_TERMINAL_COMPLETE = 0,
    SURVEY_TERMINAL_PARTIAL = 1,
    SURVEY_TERMINAL_ABORTED = 2,
    SURVEY_TERMINAL_ENUMERATION_FAILED = 3,
    SURVEY_TERMINAL_BUSY = 4,
};

enum survey_partial_reason {
    SURVEY_PARTIAL_NONE = 0,
    SURVEY_PARTIAL_MISSING_NEIGHBOR_REPORT = 1u << 0,
    SURVEY_PARTIAL_ASYMMETRIC_NEIGHBOR = 1u << 1,
    SURVEY_PARTIAL_SKIPPED_PLAN_ENTRY = 1u << 2,
    SURVEY_PARTIAL_INSUFFICIENT_RANGE = 1u << 3,
    SURVEY_PARTIAL_MISSING_RANGE_RESULT = 1u << 4,
    SURVEY_PARTIAL_NO_EXECUTABLE_PAIRS = 1u << 5,
};

enum survey_plan_skip_reason {
    SURVEY_PLAN_ACCEPTED = 0,
    SURVEY_PLAN_SKIP_UNKNOWN_SLOT = 1,
    SURVEY_PLAN_SKIP_SELF_PAIR = 2,
    SURVEY_PLAN_SKIP_NOT_MUTUAL = 3,
    SURVEY_PLAN_SKIP_DUPLICATE = 4,
    SURVEY_PLAN_SKIP_DEGREE_CAP = 5,
    SURVEY_PLAN_SKIP_CAPACITY = 6,
    SURVEY_PLAN_SKIP_MISSING_HOP = 7,
};

enum survey_range_result_status {
    SURVEY_RANGE_RESULT_MISSING = 0,
    SURVEY_RANGE_RESULT_INSUFFICIENT = 1,
    SURVEY_RANGE_RESULT_USABLE = 2,
};

struct survey_assignment_identity {
    uint32_t assignment_epoch;
    uint32_t table_command_seq;
    struct discovery_assignment_table_commitment table_commitment;
    uint8_t slot_span;
    uint8_t max_hop_count;
};

struct survey_identity {
    uint32_t generation;
    struct survey_assignment_identity assignment;
};

struct survey_neighbor_report {
    uint8_t own_slot;
    uint8_t heard_bitmap[SURVEY_NEIGHBOR_BITMAP_BYTES];
};

struct survey_pair_request {
    uint8_t first_slot;
    uint8_t second_slot;
};

struct survey_plan_pair {
    uint8_t initiator_slot;
    uint8_t responder_slot;
    uint8_t wave_index;
};

struct survey_plan_skip {
    struct survey_pair_request request;
    enum survey_plan_skip_reason reason;
    uint8_t input_index;
};

struct survey_range_result {
    int32_t median_mm;
    uint8_t pair_index;
    uint8_t success_count;
    uint8_t responder_slot;
    uint8_t reserved;
};

struct survey_plan {
    struct survey_identity identity;
    uint32_t execution_start_delay_ms;
    uint32_t self_stop_delay_ms;
    uint8_t pair_count;
    uint8_t wave_count;
    struct survey_plan_pair pairs[SURVEY_MAX_PAIRS];
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
};

struct survey_graph {
    struct survey_neighbor_report reports[SURVEY_MAX_ANCHORS];
    uint64_t occupied_slot_mask;
    uint64_t received_report_mask;
};

struct survey_plan_build_result {
    struct survey_plan plan;
    struct survey_plan_skip skipped[SURVEY_MAX_PAIRS];
    uint8_t skipped_count;
};

bool survey_assignment_identity_valid(
    const struct survey_assignment_identity *identity);
bool survey_assignment_identity_equal(
    const struct survey_assignment_identity *left,
    const struct survey_assignment_identity *right);
bool survey_identity_equal(const struct survey_identity *left,
                           const struct survey_identity *right);

bool survey_neighbor_bitmap_get(const uint8_t *bitmap, uint8_t slot);
bool survey_neighbor_bitmap_set(uint8_t *bitmap, uint8_t slot);
bool survey_graph_observed(const struct survey_graph *graph,
                           uint8_t observer_slot,
                           uint8_t observed_slot);
bool survey_graph_mutual(const struct survey_graph *graph,
                         uint8_t first_slot,
                         uint8_t second_slot);
bool survey_graph_interferes(const struct survey_graph *graph,
                             uint8_t first_slot,
                             uint8_t second_slot);
int survey_graph_note_report(struct survey_graph *graph,
                             const struct survey_neighbor_report *report);

uint32_t survey_neighbor_sequence_duration_ms(uint8_t slot_span);
uint8_t survey_slot_span_include(uint8_t occupied_slot_span,
                                 uint8_t assigned_slot);
uint32_t survey_control_delivery_delay_ms(uint8_t max_hop_count);
uint32_t survey_result_lane_duration_ms(uint8_t max_hop_count);
uint32_t survey_wave_stride_ms(uint8_t max_hop_count);
uint32_t survey_execution_duration_ms(uint8_t wave_count,
                                      uint8_t max_hop_count);
bool survey_plan_fits_hard_cap(uint32_t elapsed_before_execution_ms,
                               uint8_t wave_count,
                               uint8_t max_hop_count);
uint32_t survey_neighbor_beacon_offset_ms(uint8_t beacon_index);
uint32_t survey_range_attempt_offset_ms(uint8_t attempt_index);

int survey_build_plan(const struct survey_identity *identity,
                      const struct survey_graph *graph,
                      const uint8_t hop_counts[SURVEY_MAX_ANCHORS],
                      const struct survey_pair_request *requests,
                      size_t request_count,
                      uint32_t execution_start_delay_ms,
                      struct survey_plan_build_result *result);
bool survey_plan_commitment(const struct survey_plan *plan,
                            uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);
bool survey_plan_commitment_valid(const struct survey_plan *plan);

enum survey_range_result_status survey_range_status(
    const struct survey_range_result *result);
int survey_range_result_from_samples(uint8_t pair_index,
                                     uint8_t responder_slot,
                                     const int32_t *samples_mm,
                                     size_t sample_count,
                                     struct survey_range_result *result);

size_t survey_assignment_identity_encode(
    const struct survey_assignment_identity *identity,
    uint8_t out[SURVEY_ASSIGNMENT_IDENTITY_WIRE_LEN]);
int survey_assignment_identity_decode(
    const uint8_t *data,
    size_t data_len,
    struct survey_assignment_identity *identity);
size_t survey_neighbor_report_encode(
    const struct survey_neighbor_report *report,
    uint8_t out[SURVEY_NEIGHBOR_RECORD_WIRE_LEN]);
int survey_neighbor_report_decode(const uint8_t *data,
                                  size_t data_len,
                                  struct survey_neighbor_report *report);
size_t survey_range_result_encode(
    const struct survey_range_result *result,
    uint8_t out[SURVEY_RANGE_RESULT_WIRE_LEN]);
int survey_range_result_decode(const uint8_t *data,
                               size_t data_len,
                               struct survey_range_result *result);
size_t survey_plan_encode(const struct survey_plan *plan,
                          uint8_t *out,
                          size_t out_cap);
int survey_plan_decode(const uint8_t *data,
                       size_t data_len,
                       struct survey_plan *plan);

#ifdef __cplusplus
}
#endif

#endif
