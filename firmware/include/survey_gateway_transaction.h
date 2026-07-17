#ifndef SURVEY_GATEWAY_TRANSACTION_H
#define SURVEY_GATEWAY_TRANSACTION_H

#include "node_transaction.h"
#include "survey.h"

#include <stdbool.h>
#include <stdint.h>

#define SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK 0x01u
#define SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK 0x02u
#define SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT 4u
#define SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS 3000u

enum survey_gateway_transaction_result {
    SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK = 0,
    SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_STALE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_LATE,
    SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT,
};

enum survey_gateway_drive_action {
    SURVEY_GATEWAY_DRIVE_NONE = 0,
    SURVEY_GATEWAY_DRIVE_POLL_CLEANUP,
    SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY,
    SURVEY_GATEWAY_DRIVE_RUN_NOW,
};

struct survey_gateway_drive_state {
    bool survey_active;
    bool auto_running;
    bool auto_waiting;
    bool pair_observation_active;
    bool cleanup_pending;
    bool boundary_pending;
    bool response_ack_settle_pending;
};

struct survey_gateway_response_ack_settle {
    uint64_t deadline_ms;
    bool active;
};

struct survey_gateway_transaction_recent {
    struct node_transaction_key key;
    uint32_t request_fingerprint;
    uint32_t result_fingerprint;
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
    uint8_t prepared_mask;
    uint8_t possible_prepare_mask;
    uint8_t cleanup_mask;
    uint8_t recent_next;
    bool pair_loaded;
    bool abandoning;
    bool conflict;
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
    uint32_t request_fingerprint,
    uint32_t client_token,
    uint32_t delivery_handle,
    uint64_t absolute_deadline_ms,
    uint64_t now_ms);
int survey_gateway_transaction_note_delivery_terminal(
    struct survey_gateway_transaction *context,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action);
int survey_gateway_transaction_reconcile_result(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint32_t request_fingerprint,
    uint32_t result_fingerprint,
    uint32_t result_token,
    enum command_status status,
    uint64_t now_ms,
    enum survey_gateway_transaction_result *result,
    enum node_transaction_action *action);
bool survey_gateway_transaction_service(
    struct survey_gateway_transaction *context,
    uint64_t now_ms,
    enum node_transaction_action *action);
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
enum survey_gateway_drive_action survey_gateway_drive_action(
    const struct survey_gateway_drive_state *state);
void survey_gateway_response_ack_settle_init(
    struct survey_gateway_response_ack_settle *state);
void survey_gateway_response_ack_settle_note_result(
    struct survey_gateway_response_ack_settle *state,
    uint64_t now_ms);
bool survey_gateway_response_ack_settle_pending(
    struct survey_gateway_response_ack_settle *state,
    uint64_t now_ms);
bool survey_gateway_transaction_request_fingerprint(
    const struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint32_t *request_fingerprint);
int survey_gateway_transaction_note_cleanup_started(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask);
int survey_gateway_transaction_note_cleanup_complete(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask,
    uint64_t now_ms);
void survey_gateway_transaction_pair_complete(
    struct survey_gateway_transaction *context,
    bool success,
    uint64_t now_ms);

#endif
