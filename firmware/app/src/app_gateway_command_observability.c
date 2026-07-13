#include "app_gateway_command_observability.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "survey.h"

_Static_assert(sizeof(struct gateway_command_observability_state) <=
               GATEWAY_COMMAND_OBSERVABILITY_RAM_BUDGET_BYTES,
               "gateway command observability state exceeds RAM budget");
_Static_assert(GATEWAY_COMMAND_EVENT_WIRE_LEN <= PACKET_MAX_PAYLOAD_LEN,
               "gateway command event must fit one compact protocol packet");

static bool kind_valid(enum gateway_command_event_kind kind)
{
    return kind >= GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION &&
           kind <= GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH;
}

static bool stage_valid(enum gateway_command_event_stage stage)
{
    return stage >= GATEWAY_COMMAND_EVENT_STAGE_ACCEPTED &&
           stage <= GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
}

static bool status_valid(enum command_status status)
{
    return status >= COMMAND_OK && status <= COMMAND_INTERNAL_ERROR;
}

static bool reason_valid(enum gateway_command_event_reason reason)
{
    return reason >= GATEWAY_COMMAND_EVENT_REASON_NONE &&
           reason <= GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION;
}

static uint8_t survey_failure_reason_priority(
    enum gateway_command_event_reason reason)
{
    switch (reason) {
    case GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE:
        return 1u;
    case GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED:
        return 2u;
    case GATEWAY_COMMAND_EVENT_REASON_RADIO:
        return 3u;
    case GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE:
        return 4u;
    case GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED:
        return 5u;
    case GATEWAY_COMMAND_EVENT_REASON_INTERNAL:
        return 6u;
    default:
        return 0u;
    }
}

enum gateway_command_event_reason gateway_command_survey_failure_reason_merge(
    enum gateway_command_event_reason current,
    enum gateway_command_event_reason candidate)
{
    return survey_failure_reason_priority(candidate) >
                   survey_failure_reason_priority(current) ?
               candidate : current;
}

bool gateway_command_survey_sample_admission(
    uint16_t sample_count,
    enum command_status *status,
    enum gateway_command_event_reason *reason)
{
    if (status == NULL || reason == NULL) {
        return false;
    }

    if (sample_count < SURVEY_MIN_SAMPLE_COUNT ||
        sample_count > SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        *status = COMMAND_DENIED;
        *reason = GATEWAY_COMMAND_EVENT_REASON_CAPACITY;
        return false;
    }

    *status = COMMAND_OK;
    *reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    return true;
}

void gateway_command_survey_terminal_outcome(
    size_t report_count,
    uint16_t failure_count,
    enum gateway_command_event_reason failure_reason,
    enum command_status *status,
    enum gateway_command_event_reason *reason)
{
    if (status == NULL || reason == NULL) {
        return;
    }

    *status = COMMAND_OK;
    *reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    if (report_count == 0u) {
        *status = COMMAND_TIMEOUT;
        *reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS;
    } else if (failure_count > 0u) {
        *status = COMMAND_INTERNAL_ERROR;
        *reason = survey_failure_reason_priority(failure_reason) > 0u ?
                      failure_reason :
                      GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED;
    }
}

static bool event_valid(const struct gateway_command_event *event)
{
    const uint8_t known_flags = GATEWAY_COMMAND_EVENT_FLAG_TERMINAL |
                                GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT |
                                GATEWAY_COMMAND_EVENT_FLAG_REPLAY |
                                GATEWAY_COMMAND_EVENT_FLAG_DUPLICATE;

    return event != NULL &&
           event->schema_version == GATEWAY_COMMAND_EVENT_SCHEMA_VERSION &&
           event->record_len == GATEWAY_COMMAND_EVENT_WIRE_LEN &&
           kind_valid(event->kind) && stage_valid(event->stage) &&
           status_valid(event->status) && reason_valid(event->reason) &&
           (event->flags & ~known_flags) == 0u;
}

static struct gateway_command_event_snapshot *snapshot_for_kind(
    struct gateway_command_observability_state *state,
    enum gateway_command_event_kind kind)
{
    if (state == NULL || !kind_valid(kind)) {
        return NULL;
    }
    return &state->snapshots[(size_t)kind - 1u];
}

static void put_u16(uint8_t *out, size_t *offset, uint16_t value)
{
    proto_put_u16_le(&out[*offset], value);
    *offset += sizeof(value);
}

static void put_u32(uint8_t *out, size_t *offset, uint32_t value)
{
    proto_put_u32_le(&out[*offset], value);
    *offset += sizeof(value);
}

static void put_u64(uint8_t *out, size_t *offset, uint64_t value)
{
    proto_put_u64_le(&out[*offset], value);
    *offset += sizeof(value);
}

static uint16_t get_u16(const uint8_t *data, size_t *offset)
{
    uint16_t value = proto_get_u16_le(&data[*offset]);

    *offset += sizeof(value);
    return value;
}

static uint32_t get_u32(const uint8_t *data, size_t *offset)
{
    uint32_t value = proto_get_u32_le(&data[*offset]);

    *offset += sizeof(value);
    return value;
}

static uint64_t get_u64(const uint8_t *data, size_t *offset)
{
    uint64_t value = proto_get_u64_le(&data[*offset]);

    *offset += sizeof(value);
    return value;
}

void gateway_command_observability_init(
    struct gateway_command_observability_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int gateway_command_event_encode(const struct gateway_command_event *event,
                                 uint8_t *out,
                                 size_t out_cap,
                                 size_t *written)
{
    size_t offset = 0u;

    if (!event_valid(event) || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (out_cap < GATEWAY_COMMAND_EVENT_WIRE_LEN) {
        return -ENOSPC;
    }

    out[offset++] = event->schema_version;
    out[offset++] = event->record_len;
    out[offset++] = (uint8_t)event->kind;
    out[offset++] = (uint8_t)event->stage;
    out[offset++] = event->flags;
    out[offset++] = event->attempt;
    out[offset++] = (uint8_t)event->status;
    out[offset++] = (uint8_t)event->reason;
    put_u16(out, &offset, (uint16_t)event->command_id);
    put_u16(out, &offset, event->gateway_epoch);
    put_u32(out, &offset, event->correlation_id);
    put_u32(out, &offset, event->gateway_sequence);
    put_u32(out, &offset, event->host_session_id);
    put_u16(out, &offset, event->host_seq);
    put_u16(out, &offset, 0u);
    put_u32(out, &offset, event->event_seq);
    put_u64(out, &offset, event->anchor_id);
    put_u64(out, &offset, event->pair_initiator_id);
    put_u64(out, &offset, event->pair_responder_id);
    put_u64(out, &offset, event->previous_hop_id);
    put_u16(out, &offset, event->progress_count);
    put_u16(out, &offset, event->total_count);
    put_u16(out, &offset, event->success_count);
    put_u16(out, &offset, event->failure_count);
    put_u16(out, &offset, event->duplicate_count);
    put_u16(out, &offset, event->lost_event_count);
    out[offset++] = event->hop_count;
    out[offset++] = event->slot;
    if (offset != GATEWAY_COMMAND_EVENT_WIRE_LEN) {
        return -EINVAL;
    }
    *written = offset;
    return 0;
}

int gateway_command_event_decode(const uint8_t *data,
                                 size_t data_len,
                                 struct gateway_command_event *event)
{
    size_t offset = 0u;

    if (data == NULL || event == NULL || data_len < 2u) {
        return -EINVAL;
    }
    if (data[0] != GATEWAY_COMMAND_EVENT_SCHEMA_VERSION) {
        return -EPROTONOSUPPORT;
    }
    if (data[1] != GATEWAY_COMMAND_EVENT_WIRE_LEN || data_len != data[1]) {
        return -EMSGSIZE;
    }

    memset(event, 0, sizeof(*event));
    event->schema_version = data[offset++];
    event->record_len = data[offset++];
    event->kind = (enum gateway_command_event_kind)data[offset++];
    event->stage = (enum gateway_command_event_stage)data[offset++];
    event->flags = data[offset++];
    event->attempt = data[offset++];
    event->status = (enum command_status)data[offset++];
    event->reason = (enum gateway_command_event_reason)data[offset++];
    event->command_id = (enum command_id)get_u16(data, &offset);
    event->gateway_epoch = get_u16(data, &offset);
    event->correlation_id = get_u32(data, &offset);
    event->gateway_sequence = get_u32(data, &offset);
    event->host_session_id = get_u32(data, &offset);
    event->host_seq = get_u16(data, &offset);
    if (get_u16(data, &offset) != 0u) {
        return -EINVAL;
    }
    event->event_seq = get_u32(data, &offset);
    event->anchor_id = get_u64(data, &offset);
    event->pair_initiator_id = get_u64(data, &offset);
    event->pair_responder_id = get_u64(data, &offset);
    event->previous_hop_id = get_u64(data, &offset);
    event->progress_count = get_u16(data, &offset);
    event->total_count = get_u16(data, &offset);
    event->success_count = get_u16(data, &offset);
    event->failure_count = get_u16(data, &offset);
    event->duplicate_count = get_u16(data, &offset);
    event->lost_event_count = get_u16(data, &offset);
    event->hop_count = data[offset++];
    event->slot = data[offset++];
    return offset == data_len && event_valid(event) ? 0 : -EINVAL;
}

int gateway_command_observability_prepare(
    struct gateway_command_observability_state *state,
    struct gateway_command_event *event,
    bool terminal)
{
    struct gateway_command_event_snapshot *snapshot;

    if (state == NULL || event == NULL || !kind_valid(event->kind) ||
        !stage_valid(event->stage) || !status_valid(event->status) ||
        !reason_valid(event->reason)) {
        return -EINVAL;
    }

    state->next_event_seq++;
    if (state->next_event_seq == 0u) {
        state->next_event_seq = 1u;
    }
    event->schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION;
    event->record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN;
    event->event_seq = state->next_event_seq;
    event->lost_event_count = state->lost_event_count;
    event->flags &= (uint8_t)~GATEWAY_COMMAND_EVENT_FLAG_TERMINAL;
    if (terminal) {
        event->flags |= GATEWAY_COMMAND_EVENT_FLAG_TERMINAL |
                        GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT;
    }

    snapshot = snapshot_for_kind(state, event->kind);
    if (!terminal) {
        snapshot->event = *event;
        snapshot->valid = true;
        snapshot->enqueue_pending = true;
        return 0;
    }

    snapshot->valid = false;
    for (size_t i = 0u; i < GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH; i++) {
        if (!state->terminals[i].valid) {
            state->terminals[i].event = *event;
            state->terminals[i].valid = true;
            return 0;
        }
    }
    if (state->lost_event_count < UINT16_MAX) {
        state->lost_event_count++;
    }
    return -ENOSPC;
}

void gateway_command_observability_note_enqueue(
    struct gateway_command_observability_state *state,
    uint32_t event_seq,
    int enqueue_result)
{
    if (state == NULL || event_seq == 0u) {
        return;
    }

    for (size_t i = 0u; i < GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH; i++) {
        struct gateway_command_event_terminal *terminal = &state->terminals[i];

        if (!terminal->valid || terminal->event.event_seq != event_seq) {
            continue;
        }
        if (enqueue_result >= 0) {
            terminal->valid = false;
            return;
        }
        /* A transient transport refusal leaves terminal custody here. */
        return;
    }
    for (size_t i = 0u; i < GATEWAY_COMMAND_EVENT_MAX_TRACKED; i++) {
        struct gateway_command_event_snapshot *snapshot = &state->snapshots[i];

        if (!snapshot->valid || snapshot->event.event_seq != event_seq) {
            continue;
        }
        snapshot->enqueue_pending = enqueue_result < 0;
        return;
    }
}

bool gateway_command_observability_pending_terminal(
    struct gateway_command_observability_state *state,
    struct gateway_command_event *event)
{
    if (state == NULL || event == NULL) {
        return false;
    }
    for (size_t i = 0u; i < GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH; i++) {
        struct gateway_command_event_terminal *terminal = &state->terminals[i];

        if (terminal->valid) {
            *event = terminal->event;
            event->flags |= GATEWAY_COMMAND_EVENT_FLAG_REPLAY |
                            GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT;
            return true;
        }
    }
    return false;
}

bool gateway_command_observability_reconnect_snapshot(
    const struct gateway_command_observability_state *state,
    enum gateway_command_event_kind kind,
    struct gateway_command_event *event)
{
    const struct gateway_command_event_snapshot *snapshot;

    if (state == NULL || event == NULL || !kind_valid(kind)) {
        return false;
    }
    snapshot = &state->snapshots[(size_t)kind - 1u];
    if (!snapshot->valid) {
        return false;
    }
    *event = snapshot->event;
    event->flags |= GATEWAY_COMMAND_EVENT_FLAG_REPLAY |
                    GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT;
    return true;
}

bool gateway_command_observability_pending_snapshot(
    const struct gateway_command_observability_state *state,
    enum gateway_command_event_kind kind,
    struct gateway_command_event *event)
{
    const struct gateway_command_event_snapshot *snapshot;

    if (state == NULL || event == NULL || !kind_valid(kind)) {
        return false;
    }
    snapshot = &state->snapshots[(size_t)kind - 1u];
    if (!snapshot->valid || !snapshot->enqueue_pending) {
        return false;
    }
    *event = snapshot->event;
    event->flags |= GATEWAY_COMMAND_EVENT_FLAG_REPLAY |
                    GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT;
    return true;
}

void gateway_command_observability_mark_sent(
    struct gateway_command_observability_state *state,
    uint32_t event_seq)
{
    if (state == NULL || event_seq == 0u) {
        return;
    }
    for (size_t i = 0u; i < GATEWAY_COMMAND_EVENT_TERMINAL_BACKLOG_DEPTH; i++) {
        struct gateway_command_event_terminal *terminal = &state->terminals[i];

        if (!terminal->valid || terminal->event.event_seq != event_seq) {
            continue;
        }
        terminal->valid = false;
        return;
    }
}
