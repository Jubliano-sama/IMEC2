#include "app_gateway_survey_observability.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct app_gateway_survey_observability_state) <=
               APP_GATEWAY_SURVEY_OBSERVABILITY_RAM_BUDGET_BYTES,
               "survey observability state exceeds compact RAM budget");

static bool inputs_valid(
    const struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops)
{
    return state != NULL && ops != NULL && ops->emit_if_available != NULL;
}

static struct gateway_command_event progress_event(
    const struct gateway_command_event *base,
    enum gateway_command_event_stage stage,
    uint16_t duplicate_count)
{
    struct gateway_command_event event = *base;

    event.schema_version = 0u;
    event.record_len = 0u;
    event.stage = stage;
    event.flags = 0u;
    event.attempt = 0u;
    event.status = COMMAND_OK;
    event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    event.event_seq = 0u;
    event.anchor_id = 0u;
    event.pair_initiator_id = 0u;
    event.pair_responder_id = 0u;
    event.previous_hop_id = 0u;
    event.progress_count = 0u;
    event.total_count = 0u;
    event.success_count = 0u;
    event.failure_count = 0u;
    event.duplicate_count = duplicate_count;
    event.lost_event_count = 0u;
    event.hop_count = 0u;
    event.slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
    return event;
}

static int emit_progress(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops,
    struct gateway_command_event *event)
{
    uint32_t retained_event_seq;
    int ret;

    retained_event_seq = state->pending_event_seq;
    event->event_seq = retained_event_seq;
    ret = ops->emit_if_available(event, false, ops->ctx);
    if (event->event_seq != 0u && retained_event_seq != 0u &&
        event->event_seq != retained_event_seq) {
        return -EIO;
    }
    if (ret < 0) {
        if (event->event_seq != 0u) {
            state->pending_event_seq = event->event_seq;
        }
        return ret;
    }
    if (event->event_seq == 0u) {
        return -EIO;
    }
    state->pending_event_seq = 0u;
    return ret;
}

void app_gateway_survey_observability_reset(
    struct app_gateway_survey_observability_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int app_gateway_survey_observability_emit_collection_next(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops,
    const struct survey_gateway_context *survey,
    const struct gateway_command_event *base_event,
    uint16_t duplicate_count)
{
    struct gateway_command_event event;
    int ret;

    if (!inputs_valid(state, ops) || survey == NULL || base_event == NULL) {
        return -EINVAL;
    }
    if (!state->collection_complete) {
        if (state->report_cursor < survey->report_count) {
            struct survey_gateway_reverse_hint reverse_hint;
            enum command_status report_status;
            uint64_t anchor_id;
            size_t entry_count;

            ret = survey_gateway_report_info_at(
                survey,
                state->report_cursor,
                &anchor_id,
                &entry_count,
                &report_status);
            if (ret != PROTO_OK) {
                return -EBADMSG;
            }
            ret = survey_gateway_reverse_hint_for_target(
                survey, anchor_id, &reverse_hint);
            if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
                return -EBADMSG;
            }

            event = progress_event(
                base_event, GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
                duplicate_count);
            event.anchor_id = anchor_id;
            event.previous_hop_id =
                ret == PROTO_OK ? reverse_hint.next_hop_id : 0u;
            event.progress_count = (uint16_t)(state->report_cursor + 1u);
            event.total_count = (uint16_t)survey->report_count;
            ret = emit_progress(state, ops, &event);
            if (ret < 0) {
                return ret;
            }
            state->report_cursor++;
            return 0;
        }
        event = progress_event(
            base_event, GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE,
            duplicate_count);
        event.progress_count = (uint16_t)survey->report_count;
        event.total_count = event.progress_count;
        ret = emit_progress(state, ops, &event);
        if (ret < 0) {
            return ret;
        }
        state->collection_complete = true;
        return 0;
    }
    if (!state->schedule_complete) {
        event = progress_event(
            base_event, GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY,
            duplicate_count);
        event.total_count = (uint16_t)survey->pair_count;
        ret = emit_progress(state, ops, &event);
        if (ret < 0) {
            return ret;
        }
        state->schedule_complete = true;
        return 1;
    }
    return 1;
}

int app_gateway_survey_observability_submit_boundary(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops,
    const struct gateway_command_event *event)
{
    int ret;

    if (!inputs_valid(state, ops) || event == NULL) {
        return -EINVAL;
    }
    if (state->boundary_pending) {
        return -EBUSY;
    }
    state->boundary_event = *event;
    ret = ops->emit_if_available(&state->boundary_event, false, ops->ctx);
    state->boundary_pending = ret < 0;
    return ret;
}

int app_gateway_survey_observability_flush_boundary(
    struct app_gateway_survey_observability_state *state,
    const struct app_gateway_survey_observability_ops *ops)
{
    int ret;

    if (!inputs_valid(state, ops)) {
        return -EINVAL;
    }
    if (!state->boundary_pending) {
        return 1;
    }
    ret = ops->emit_if_available(&state->boundary_event, false, ops->ctx);
    if (ret < 0) {
        return ret;
    }
    state->boundary_pending = false;
    return 1;
}
