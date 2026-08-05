#include "app_mesh_event_retry.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool request_valid(
    const struct app_mesh_event_request_identity *request)
{
    return request != NULL && request->source_id != 0u &&
           request->session_id != 0u && request->sequence != 0u &&
           request->message_type != 0u;
}

static bool request_key_equal(
    const struct app_mesh_event_request_identity *lhs,
    const struct app_mesh_event_request_identity *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->source_id == rhs->source_id &&
           lhs->session_id == rhs->session_id &&
           lhs->sequence == rhs->sequence &&
           lhs->message_type == rhs->message_type;
}

bool app_mesh_event_payload_digest(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return semantic_digest_sha256(payload, payload_len, digest);
}

bool app_mesh_event_request_payload_equal(
    const struct app_mesh_event_request_identity *lhs,
    const struct app_mesh_event_request_identity *rhs)
{
    return lhs != NULL && rhs != NULL &&
           semantic_digest_equal(lhs->payload_digest,
                                 rhs->payload_digest,
                                 sizeof(lhs->payload_digest));
}

bool app_mesh_event_accept_timing_compatible(
    const struct mesh_event_timing *accepted,
    const struct mesh_event_timing *proposed)
{
    if (accepted == NULL || proposed == NULL) {
        return false;
    }

    return accepted->mesh_channel == proposed->mesh_channel &&
           accepted->event_interval_ms == proposed->event_interval_ms &&
           accepted->event_window_ms == proposed->event_window_ms &&
           accepted->event_counter == proposed->event_counter &&
           accepted->guard_ms == proposed->guard_ms &&
           accepted->peer_clock_skew_estimate_ppm ==
               proposed->peer_clock_skew_estimate_ppm &&
           accepted->max_missed_events == proposed->max_missed_events &&
           accepted->supervision_timeout_ms ==
               proposed->supervision_timeout_ms;
}

enum app_mesh_event_request_match app_mesh_event_retry_match(
    const struct app_mesh_event_retry_state *state,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request)
{
    if (state == NULL || !state->active) {
        return APP_MESH_EVENT_REQUEST_NEW;
    }
    if (state->peer_id != peer_id ||
        !request_key_equal(&state->request, request)) {
        return APP_MESH_EVENT_REQUEST_BUSY;
    }
    if (!app_mesh_event_request_payload_equal(&state->request, request)) {
        return APP_MESH_EVENT_REQUEST_CONFLICT;
    }
    return APP_MESH_EVENT_REQUEST_DUPLICATE;
}

enum app_mesh_event_accept_correlation app_mesh_event_accept_classify(
    const struct app_mesh_event_retry_state *proposal,
    uint64_t response_source_id,
    uint64_t response_destination_id,
    uint64_t response_previous_hop_id,
    uint32_t response_session_id,
    uint16_t response_sequence,
    bool timing_compatible)
{
    if (proposal == NULL || !proposal->active || !timing_compatible ||
        response_source_id == 0u || response_destination_id == 0u ||
        response_previous_hop_id != proposal->peer_id ||
        response_source_id != proposal->peer_id ||
        response_destination_id != proposal->request.source_id) {
        return APP_MESH_EVENT_ACCEPT_REJECT;
    }
    if (response_session_id == proposal->request.session_id &&
        response_sequence == proposal->request.sequence) {
        return APP_MESH_EVENT_ACCEPT_EXACT;
    }
    if (response_session_id == 0u || response_sequence == 0u) {
        return APP_MESH_EVENT_ACCEPT_REJECT;
    }

    /*
     * The stable connection release gave ACCEPT its own fresh packet identity.
     * Active-peer and timing-shape correlation keeps that wire behavior
     * interoperable without admitting an unrelated peer or negotiation.
     */
    return APP_MESH_EVENT_ACCEPT_LEGACY;
}

int app_mesh_event_retry_begin(
    struct app_mesh_event_retry_state *state,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    const struct app_mesh_rf_retry_key *retry_key,
    uint32_t now_ms,
    uint32_t deadline_ms,
    uint32_t event_interval_ms,
    uint16_t phase_slop_ms)
{
    if (state == NULL || peer_id == 0u || !request_valid(request) ||
        retry_key == NULL || retry_key->source_id == 0u ||
        retry_key->operation == APP_MESH_RF_RETRY_OPERATION_NONE ||
        event_interval_ms == 0u ||
        (phase_slop_ms != 0u && phase_slop_ms >= event_interval_ms / 2u) ||
        deadline_reached(now_ms, deadline_ms)) {
        return -EINVAL;
    }
    if (state->active) {
        return -EBUSY;
    }

    memset(state, 0, sizeof(*state));
    state->request = *request;
    state->retry_key = *retry_key;
    state->peer_id = peer_id;
    state->deadline_ms = deadline_ms;
    state->phase_anchor_ms = now_ms;
    state->event_interval_ms = event_interval_ms;
    state->phase_slop_ms = phase_slop_ms;
    state->active = true;
    return 0;
}

int app_mesh_event_retry_resume_backoff(
    struct app_mesh_event_retry_state *state,
    uint16_t retry_round)
{
    if (state == NULL || !state->active) {
        return -EINVAL;
    }

    state->retry.key = state->retry_key;
    state->retry.retry_round = retry_round;
    state->retry.active = true;
    return 0;
}

bool app_mesh_event_retry_note_failure(
    struct app_mesh_event_retry_state *state,
    enum app_mesh_rf_retry_policy policy,
    uint32_t now_ms,
    uint32_t attempt_entropy,
    bool rf_started,
    uint32_t *delay_ms)
{
    uint32_t raw_delay_ms;
    uint32_t due_ms;
    uint32_t phase_offset_ms;

    if (delay_ms != NULL) {
        *delay_ms = 0u;
    }
    if (state == NULL || !state->active ||
        deadline_reached(now_ms, state->deadline_ms)) {
        if (state != NULL && state->active) {
            state->retry_due_ms = 0u;
            state->retry_due_armed = false;
        }
        return false;
    }

    if (rf_started) {
        if (state->rf_attempts != UINT16_MAX) {
            state->rf_attempts++;
        }
    } else if (state->pre_rf_deferrals != UINT16_MAX) {
        state->pre_rf_deferrals++;
    }

    raw_delay_ms = app_mesh_rf_retry_next_delay_ms(&state->retry,
                                                    &state->retry_key,
                                                    policy,
                                                    attempt_entropy);
    if (raw_delay_ms == 0u) {
        state->retry_due_ms = 0u;
        state->retry_due_armed = false;
        return false;
    }
    due_ms = now_ms + raw_delay_ms;
    if (state->phase_slop_ms != 0u) {
        uint32_t spread_ms = (2u * state->phase_slop_ms) + 1u;
        uint32_t selected_ms = attempt_entropy % spread_ms;
        uint32_t desired_phase_ms;

        if (selected_ms <= state->phase_slop_ms) {
            desired_phase_ms = selected_ms;
        } else {
            desired_phase_ms = state->event_interval_ms -
                               (selected_ms - state->phase_slop_ms);
        }
        phase_offset_ms = (due_ms - state->phase_anchor_ms) %
                          state->event_interval_ms;
        due_ms += (desired_phase_ms + state->event_interval_ms -
                   phase_offset_ms) % state->event_interval_ms;
    }
    if (deadline_reached(due_ms, state->deadline_ms)) {
        state->retry_due_ms = 0u;
        state->retry_due_armed = false;
        return false;
    }

    state->retry_due_ms = due_ms;
    state->retry_due_armed = true;
    if (delay_ms != NULL) {
        *delay_ms = due_ms - now_ms;
    }
    return true;
}

void app_mesh_event_retry_note_send_success(
    struct app_mesh_event_retry_state *state)
{
    if (state == NULL || !state->active) {
        return;
    }
    state->response_sent = true;
    state->retry_due_ms = 0u;
    state->retry_due_armed = false;
    app_mesh_rf_retry_reset(&state->retry);
}

bool app_mesh_event_retry_claim_timing_install(
    struct app_mesh_event_retry_state *state)
{
    if (state == NULL || !state->active || state->timing_installed) {
        return false;
    }
    state->timing_installed = true;
    return true;
}

bool app_mesh_event_retry_due(const struct app_mesh_event_retry_state *state,
                              uint32_t now_ms)
{
    return state != NULL && state->active && state->retry_due_armed &&
           !deadline_reached(now_ms, state->deadline_ms) &&
           deadline_reached(now_ms, state->retry_due_ms);
}

bool app_mesh_event_retry_expired(
    const struct app_mesh_event_retry_state *state,
    uint32_t now_ms)
{
    return state != NULL && state->active &&
           deadline_reached(now_ms, state->deadline_ms);
}

void app_mesh_event_retry_clear(struct app_mesh_event_retry_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

enum app_mesh_event_request_match app_mesh_event_completion_match(
    const struct app_mesh_event_completion *completion,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms)
{
    if (completion == NULL || !completion->valid ||
        deadline_reached(now_ms, completion->expires_at_ms) ||
        completion->peer_id != peer_id ||
        !request_key_equal(&completion->request, request)) {
        return APP_MESH_EVENT_REQUEST_NEW;
    }
    if (!app_mesh_event_request_payload_equal(&completion->request, request)) {
        return APP_MESH_EVENT_REQUEST_CONFLICT;
    }
    return APP_MESH_EVENT_REQUEST_DUPLICATE;
}

int app_mesh_event_completion_store(
    struct app_mesh_event_completion *completion,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint32_t expires_at_ms)
{
    if (completion == NULL || peer_id == 0u || !request_valid(request) ||
        deadline_reached(now_ms, expires_at_ms)) {
        return -EINVAL;
    }

    memset(completion, 0, sizeof(*completion));
    completion->request = *request;
    completion->peer_id = peer_id;
    completion->expires_at_ms = expires_at_ms;
    completion->valid = true;
    return 0;
}
