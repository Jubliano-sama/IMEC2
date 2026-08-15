#ifndef APP_MESH_EVENT_RETRY_H
#define APP_MESH_EVENT_RETRY_H

#include "app_mesh_rf_retry.h"
#include "mesh_event_timing.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_event_request_identity {
    uint64_t source_id;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t session_id;
    uint16_t sequence;
    uint8_t message_type;
};

enum app_mesh_event_request_match {
    APP_MESH_EVENT_REQUEST_NEW = 0,
    APP_MESH_EVENT_REQUEST_DUPLICATE,
    APP_MESH_EVENT_REQUEST_CONFLICT,
    APP_MESH_EVENT_REQUEST_BUSY,
};

enum app_mesh_event_accept_correlation {
    APP_MESH_EVENT_ACCEPT_REJECT = 0,
    APP_MESH_EVENT_ACCEPT_EXACT,
    APP_MESH_EVENT_ACCEPT_LEGACY,
};

struct app_mesh_event_retry_state {
    struct app_mesh_event_request_identity request;
    struct app_mesh_rf_retry_key retry_key;
    struct app_mesh_rf_retry_state retry;
    uint64_t peer_id;
    uint32_t deadline_ms;
    uint32_t retry_due_ms;
    uint32_t phase_anchor_ms;
    uint32_t event_interval_ms;
    uint16_t phase_slop_ms;
    uint16_t rf_attempts;
    uint16_t pre_rf_deferrals;
    bool active;
    bool response_sent;
    bool timing_installed;
    /* retry_due_ms may validly wrap to zero; this owns its armed state. */
    bool retry_due_armed;
};

struct app_mesh_event_completion {
    struct app_mesh_event_request_identity request;
    uint64_t peer_id;
    uint32_t expires_at_ms;
    bool valid;
};

bool app_mesh_event_payload_digest(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);
bool app_mesh_event_request_payload_equal(
    const struct app_mesh_event_request_identity *lhs,
    const struct app_mesh_event_request_identity *rhs);
bool app_mesh_event_accept_timing_compatible(
    const struct mesh_event_timing *accepted,
    const struct mesh_event_timing *proposed);

/* Apply one immutable counterproposal offset to a frozen PROPOSE cadence. */
bool app_mesh_event_timing_apply_phase_shift(
    struct mesh_event_timing *timing,
    uint16_t phase_shift_ms);
enum app_mesh_event_request_match app_mesh_event_retry_match(
    const struct app_mesh_event_retry_state *state,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request);
enum app_mesh_event_accept_correlation app_mesh_event_accept_classify(
    const struct app_mesh_event_retry_state *proposal,
    uint64_t response_source_id,
    uint64_t response_destination_id,
    uint64_t response_previous_hop_id,
    uint32_t response_session_id,
    uint16_t response_sequence,
    bool timing_compatible);
int app_mesh_event_retry_begin(
    struct app_mesh_event_retry_state *state,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    const struct app_mesh_rf_retry_key *retry_key,
    uint32_t now_ms,
    uint32_t deadline_ms,
    uint32_t event_interval_ms,
    uint16_t phase_slop_ms);
int app_mesh_event_retry_resume_backoff(
    struct app_mesh_event_retry_state *state,
    uint16_t retry_round);
bool app_mesh_event_retry_note_failure(
    struct app_mesh_event_retry_state *state,
    enum app_mesh_rf_retry_policy policy,
    uint32_t now_ms,
    uint32_t attempt_entropy,
    bool rf_started,
    uint32_t *delay_ms);
void app_mesh_event_retry_note_send_success(
    struct app_mesh_event_retry_state *state);
bool app_mesh_event_retry_claim_timing_install(
    struct app_mesh_event_retry_state *state);
bool app_mesh_event_retry_due(const struct app_mesh_event_retry_state *state,
                              uint32_t now_ms);
bool app_mesh_event_retry_expired(
    const struct app_mesh_event_retry_state *state,
    uint32_t now_ms);
void app_mesh_event_retry_clear(struct app_mesh_event_retry_state *state);
enum app_mesh_event_request_match app_mesh_event_completion_match(
    const struct app_mesh_event_completion *completion,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms);
int app_mesh_event_completion_store(
    struct app_mesh_event_completion *completion,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint32_t expires_at_ms);

#endif
