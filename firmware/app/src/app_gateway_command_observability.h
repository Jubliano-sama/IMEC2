#ifndef APP_GATEWAY_COMMAND_OBSERVABILITY_H
#define APP_GATEWAY_COMMAND_OBSERVABILITY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GATEWAY_COMMAND_EVENT_SCHEMA_VERSION 1u
#define GATEWAY_COMMAND_EVENT_WIRE_LEN 78u
#define GATEWAY_COMMAND_EVENT_MAX_TRACKED 3u
#define GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH 2u
#define GATEWAY_COMMAND_OBSERVABILITY_RAM_BUDGET_BYTES 576u
#define GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE UINT8_MAX

enum gateway_command_event_kind {
    GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION = 1,
    GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY = 2,
    GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH = 3,
};

enum gateway_command_event_stage {
    GATEWAY_COMMAND_EVENT_STAGE_ACCEPTED = 1,
    GATEWAY_COMMAND_EVENT_STAGE_QUEUED = 2,
    GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING = 3,
    GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT = 4,
    GATEWAY_COMMAND_EVENT_STAGE_BACKOFF = 5,
    GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED = 6,
    GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE = 7,
    GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY = 8,
    GATEWAY_COMMAND_EVENT_STAGE_PAIR_START = 9,
    GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS = 10,
    GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE = 11,
    GATEWAY_COMMAND_EVENT_STAGE_COMPLETE = 12,
};

enum gateway_command_event_reason {
    GATEWAY_COMMAND_EVENT_REASON_NONE = 0,
    GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST = 1,
    GATEWAY_COMMAND_EVENT_REASON_BUSY = 2,
    GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS = 3,
    GATEWAY_COMMAND_EVENT_REASON_CAPACITY = 4,
    GATEWAY_COMMAND_EVENT_REASON_RADIO = 5,
    GATEWAY_COMMAND_EVENT_REASON_TIMEOUT = 6,
    GATEWAY_COMMAND_EVENT_REASON_MALFORMED_RESPONSE = 7,
    GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE = 8,
    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED = 9,
    GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE = 10,
    GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED = 11,
    GATEWAY_COMMAND_EVENT_REASON_ABORTED = 12,
    GATEWAY_COMMAND_EVENT_REASON_INTERNAL = 13,
    GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION = 14,
};

enum gateway_command_event_flag {
    GATEWAY_COMMAND_EVENT_FLAG_TERMINAL = 1u << 0,
    GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT = 1u << 1,
    GATEWAY_COMMAND_EVENT_FLAG_REPLAY = 1u << 2,
    GATEWAY_COMMAND_EVENT_FLAG_DUPLICATE = 1u << 3,
};

enum gateway_command_event_reason gateway_command_survey_failure_reason_merge(
    enum gateway_command_event_reason current,
    enum gateway_command_event_reason candidate);
bool gateway_command_survey_sample_admission(
    uint16_t sample_count,
    enum command_status *status,
    enum gateway_command_event_reason *reason);
void gateway_command_survey_terminal_outcome(
    size_t report_count,
    uint16_t failure_count,
    enum gateway_command_event_reason failure_reason,
    enum command_status *status,
    enum gateway_command_event_reason *reason);

struct gateway_command_event {
    uint8_t schema_version;
    uint8_t record_len;
    enum gateway_command_event_kind kind;
    enum gateway_command_event_stage stage;
    uint8_t flags;
    uint8_t attempt;
    enum command_status status;
    enum gateway_command_event_reason reason;
    enum command_id command_id;
    uint16_t gateway_epoch;
    uint32_t correlation_id;
    uint32_t gateway_sequence;
    uint32_t host_session_id;
    uint16_t host_seq;
    uint32_t event_seq;
    uint64_t anchor_id;
    uint64_t pair_initiator_id;
    uint64_t pair_responder_id;
    uint64_t previous_hop_id;
    uint16_t progress_count;
    uint16_t total_count;
    uint16_t success_count;
    uint16_t failure_count;
    uint16_t duplicate_count;
    uint16_t lost_event_count;
    uint8_t hop_count;
    uint8_t slot;
};

struct gateway_command_event_snapshot {
    struct gateway_command_event event;
    bool valid;
};

struct gateway_command_event_terminal {
    struct gateway_command_event event;
    bool valid;
};

struct gateway_command_observability_state {
    struct gateway_command_event_snapshot snapshots[GATEWAY_COMMAND_EVENT_MAX_TRACKED];
    struct gateway_command_event_terminal
        terminals[GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH];
    uint32_t next_event_seq;
    uint16_t lost_event_count;
};

void gateway_command_observability_init(
    struct gateway_command_observability_state *state);
int gateway_command_event_encode(const struct gateway_command_event *event,
                                 uint8_t *out,
                                 size_t out_cap,
                                 size_t *written);
int gateway_command_event_decode(const uint8_t *data,
                                 size_t data_len,
                                 struct gateway_command_event *event);
int gateway_command_observability_prepare(
    struct gateway_command_observability_state *state,
    struct gateway_command_event *event,
    bool terminal);
void gateway_command_observability_note_enqueue(
    struct gateway_command_observability_state *state,
    uint32_t event_seq,
    int enqueue_result);
bool gateway_command_observability_pending_terminal(
    struct gateway_command_observability_state *state,
    struct gateway_command_event *event);
bool gateway_command_observability_reconnect_snapshot(
    const struct gateway_command_observability_state *state,
    enum gateway_command_event_kind kind,
    struct gateway_command_event *event);
void gateway_command_observability_mark_sent(
    struct gateway_command_observability_state *state,
    uint32_t event_seq);

#endif
