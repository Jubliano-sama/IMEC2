#ifndef SURVEY_GATEWAY_TRANSACTION_H
#define SURVEY_GATEWAY_TRANSACTION_H

#include "node_transaction.h"
#include "survey.h"
#include "survey_round_control.h"

#include <stdbool.h>
#include <stdint.h>

#define SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK 0x01u
#define SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK 0x02u
#define SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT 4u
#define SURVEY_GATEWAY_PAIR_CONTROL_PHASE_COUNT 4u
#define SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS \
    (SURVEY_GATEWAY_PAIR_CONTROL_PHASE_COUNT * \
     SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS)
#define SURVEY_GATEWAY_TRANSACTION_CLEANUP_TIMEOUT_MS \
    SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS

enum survey_gateway_transaction_result {
    SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK = 0,
    SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_STALE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_LATE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT,
};

enum survey_gateway_transaction_close_intent {
    SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE = 0,
    SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT,
    SURVEY_GATEWAY_TRANSACTION_CLOSE_RADIO,
    SURVEY_GATEWAY_TRANSACTION_CLOSE_INTERNAL,
};

enum survey_gateway_drive_action {
    SURVEY_GATEWAY_DRIVE_NONE = 0,
    SURVEY_GATEWAY_DRIVE_POLL_CLEANUP,
    SURVEY_GATEWAY_DRIVE_POLL_WAIT,
    SURVEY_GATEWAY_DRIVE_WAIT_CONFIRMATION,
    SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY,
    SURVEY_GATEWAY_DRIVE_RUN_NOW,
};

/*
 * One gateway survey worker services several independent deadlines.  Each
 * owner retains its absolute due time, so a later poll can never replace an
 * earlier observation or cleanup wake.
 */
enum survey_gateway_due_owner {
    SURVEY_GATEWAY_DUE_ROUND_OBSERVATION = 0,
    SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
    SURVEY_GATEWAY_DUE_CLEANUP,
    SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
    SURVEY_GATEWAY_DUE_OWNER_COUNT,
};

struct survey_gateway_due_registry {
    uint32_t due_ms[SURVEY_GATEWAY_DUE_OWNER_COUNT];
    uint8_t valid_mask;
};

typedef int (*survey_gateway_due_arm_fn)(void *context, uint32_t delay_ms);

/* Zero is a valid wrapped origin and is therefore separate from validity. */
struct survey_gateway_observation_origin {
    uint32_t started_at_ms;
    bool valid;
};

struct survey_gateway_drive_state {
    bool survey_active;
    bool control_inflight;
    bool control_confirmation_pending;
    bool round_observing;
    bool round_drive_ready;
    bool cleanup_pending;
    bool boundary_pending;
};

struct survey_gateway_transaction_recent {
    struct node_transaction_key key;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t result_token;
    uint64_t expires_at_ms;
    bool valid;
};

struct survey_gateway_transaction {
    struct node_transaction active;
    struct survey_gateway_transaction_recent
        recent[SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT];
    struct survey_pair pair;
    enum command_id active_command_id;
    uint64_t active_target_id;
    uint64_t cleanup_deadline_ms;
    uint32_t active_started_at_ms;
    uint8_t prepared_mask;
    uint8_t possible_prepare_mask;
    uint8_t cleanup_mask;
    uint8_t recent_next;
    bool pair_loaded;
    bool abandoning;
    bool conflict;
    enum survey_gateway_transaction_close_intent close_intent;
};

void survey_gateway_transaction_init(
    struct survey_gateway_transaction *context);
int survey_gateway_transaction_load_pair(
    struct survey_gateway_transaction *context,
    const struct survey_pair *pair);
int survey_gateway_transaction_begin(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    enum command_id command_id,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t client_token,
    uint32_t delivery_handle,
    uint64_t absolute_deadline_ms,
    uint64_t now_ms);
int survey_gateway_transaction_note_delivery_terminal(
    struct survey_gateway_transaction *context,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action);
int survey_gateway_transaction_note_delivery_redrive(
    struct survey_gateway_transaction *context,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action);
int survey_gateway_transaction_reconcile_result(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_token,
    enum command_status status,
    uint64_t now_ms,
    enum survey_gateway_transaction_result *result,
    enum node_transaction_action *action);
bool survey_gateway_transaction_service(
    struct survey_gateway_transaction *context,
    uint64_t now_ms,
    enum node_transaction_action *action);
int survey_gateway_transaction_request_close(
    struct survey_gateway_transaction *context,
    enum survey_gateway_transaction_close_intent intent);
enum survey_gateway_transaction_close_intent
survey_gateway_transaction_close_requested(
    const struct survey_gateway_transaction *context);
void survey_gateway_transaction_clear_close_request(
    struct survey_gateway_transaction *context);
int survey_gateway_transaction_phase_complete(
    struct survey_gateway_transaction *context);
void survey_gateway_transaction_require_cleanup(
    struct survey_gateway_transaction *context,
    bool include_both_peers,
    uint64_t now_ms);
uint8_t survey_gateway_transaction_cleanup_mask(
    const struct survey_gateway_transaction *context);
bool survey_gateway_transaction_cleanup_pending(
    const struct survey_gateway_transaction *context);
uint64_t survey_gateway_transaction_cleanup_deadline(
    const struct survey_gateway_transaction *context);
bool survey_gateway_transaction_pair_plan_fits_minimum_budget(
    size_t pair_count,
    uint32_t remaining_ms);
enum survey_gateway_drive_action survey_gateway_drive_action(
    const struct survey_gateway_drive_state *state);
void survey_gateway_due_registry_init(
    struct survey_gateway_due_registry *registry);
int survey_gateway_due_registry_schedule_after(
    struct survey_gateway_due_registry *registry,
    enum survey_gateway_due_owner owner,
    uint32_t now_ms,
    uint32_t delay_ms,
    survey_gateway_due_arm_fn arm,
    void *arm_context);
void survey_gateway_due_registry_cancel(
    struct survey_gateway_due_registry *registry,
    enum survey_gateway_due_owner owner);
void survey_gateway_due_registry_consume_due(
    struct survey_gateway_due_registry *registry,
    uint32_t now_ms);
bool survey_gateway_due_registry_next(
    const struct survey_gateway_due_registry *registry,
    uint32_t now_ms,
    uint32_t *delay_ms);
int survey_gateway_due_registry_rearm(
    const struct survey_gateway_due_registry *registry,
    uint32_t now_ms,
    survey_gateway_due_arm_fn arm,
    void *arm_context);
/*
 * Classify physical ingress against a wrap-safe closed-open operation window.
 * first_received_at_ms may be the expanded 64-bit uptime retained by the mesh
 * queue; only its low 32 bits participate because operation windows are
 * bounded below the signed 32-bit uptime horizon.
 */
bool survey_gateway_receive_in_interval(uint64_t first_received_at_ms,
                                        uint32_t started_at_ms,
                                        uint32_t deadline_ms);
void survey_gateway_observation_origin_reset(
    struct survey_gateway_observation_origin *origin);
bool survey_gateway_observation_origin_freeze(
    struct survey_gateway_observation_origin *origin,
    uint32_t started_at_ms);
bool survey_gateway_transaction_request_digest(
    const struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN]);
int survey_gateway_transaction_note_cleanup_started(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask);
int survey_gateway_transaction_note_cleanup_complete(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask,
    uint64_t now_ms);
int survey_gateway_transaction_note_cleanup_lease_expired(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask,
    uint64_t now_ms);
void survey_gateway_transaction_pair_complete(
    struct survey_gateway_transaction *context,
    bool success,
    uint64_t now_ms);

#endif
