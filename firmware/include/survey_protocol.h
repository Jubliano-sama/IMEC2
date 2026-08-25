#ifndef SURVEY_PROTOCOL_H
#define SURVEY_PROTOCOL_H

#include "protocol.h"
#include "survey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_TLV_CHUNK_MAX_LEN UINT8_MAX
#define SURVEY_EVENT_HEADER_WIRE_LEN 72u
#define SURVEY_EVENT_SKIP_WIRE_LEN 4u
#define SURVEY_EVENT_MAX_WIRE_LEN \
    (SURVEY_EVENT_HEADER_WIRE_LEN + SURVEY_RESULTS_MAX_WIRE_LEN)

enum survey_event_kind {
    SURVEY_EVENT_NEIGHBOR_GRAPH = 1,
    SURVEY_EVENT_PLAN_ACCEPTED = 2,
    SURVEY_EVENT_RANGE_PROGRESS = 3,
    SURVEY_EVENT_TERMINAL = 4,
};

struct survey_control {
    enum survey_phase phase;
    struct survey_identity identity;
    uint32_t start_delay_ms;
    uint32_t self_stop_delay_ms;
    struct survey_plan plan;
    bool plan_present;
    bool start_delay_present;
    bool self_stop_delay_present;
};

struct survey_host_plan_request {
    struct survey_identity identity;
    struct survey_pair_request pairs[SURVEY_MAX_PAIRS];
    uint8_t pair_count;
};

struct survey_event {
    enum survey_event_kind kind;
    enum survey_terminal_status status;
    struct survey_identity identity;
    struct survey_graph graph;
    struct survey_plan plan;
    struct survey_range_result results[SURVEY_MAX_PAIRS];
    struct survey_plan_skip skipped[SURVEY_MAX_PAIRS];
    uint16_t partial_reasons;
    uint8_t result_count;
    uint8_t skipped_count;
};

int survey_control_append_tlvs(uint8_t *payload,
                               size_t payload_cap,
                               size_t *payload_len,
                               const struct survey_control *control);
int survey_control_extract_tlvs(const uint8_t *payload,
                                size_t payload_len,
                                struct survey_control *control);
int survey_host_plan_request_append_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    const struct survey_host_plan_request *request);
int survey_host_plan_request_extract_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    struct survey_host_plan_request *request);

size_t survey_event_encode(const struct survey_event *event,
                           uint8_t *out,
                           size_t out_cap);
int survey_event_decode(const uint8_t *data,
                        size_t data_len,
                        struct survey_event *event);

#ifdef __cplusplus
}
#endif

#endif
