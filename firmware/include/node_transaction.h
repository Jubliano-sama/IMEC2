#ifndef NODE_TRANSACTION_H
#define NODE_TRANSACTION_H

#include "node_comm.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NODE_TRANSACTION_RESPONDER_MAX_RECORDS 8u

/*
 * Full SHA-256 commitments over canonical serialized bytes. These commitments
 * are equality authority for request/result reconciliation; callers must never
 * truncate them to a transport token or a bounded fingerprint.
 */
bool node_transaction_digest_bytes(
    const uint8_t *bytes,
    size_t length,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);
struct proto_packet;
bool node_transaction_digest_packet(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN]);

enum node_transaction_state {
    NODE_TRANSACTION_EMPTY = 0,
    NODE_TRANSACTION_ACTIVE,
    NODE_TRANSACTION_ABANDONING,
    NODE_TRANSACTION_SUCCEEDED,
    NODE_TRANSACTION_ABANDONED,
};

enum node_transaction_abandon_reason {
    NODE_TRANSACTION_ABANDON_DELIVERY_FAILED = 0,
    NODE_TRANSACTION_ABANDON_DEADLINE,
    NODE_TRANSACTION_ABANDON_CANCELLED,
};

enum node_transaction_result_disposition {
    NODE_TRANSACTION_RESULT_ACCEPTED = 0,
    NODE_TRANSACTION_RESULT_DUPLICATE,
    NODE_TRANSACTION_RESULT_STALE,
    NODE_TRANSACTION_RESULT_LATE_AFTER_ABANDON,
    NODE_TRANSACTION_RESULT_CONFLICT,
};

enum node_transaction_action {
    NODE_TRANSACTION_ACTION_NONE = 0,
    NODE_TRANSACTION_ACTION_WAIT_RESULT,
    NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED,
    NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS,
    NODE_TRANSACTION_ACTION_TERMINAL_ABANDON,
};

struct node_transaction_key {
    uint64_t requester_id;
    uint64_t responder_id;
    uint32_t session_id;
    uint16_t transaction_id;
    uint16_t operation_id;
};

struct node_transaction_spec {
    struct node_transaction_key key;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t client_token;
    uint64_t absolute_deadline_ms;
    bool cleanup_required;
};

struct node_transaction {
    struct node_transaction_spec spec;
    uint8_t accepted_result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t request_delivery_handle;
    uint32_t result_token;
    enum node_transaction_state state;
    enum node_transaction_abandon_reason abandon_reason;
    uint8_t request_attempts_started;
    bool request_delivery_terminal;
    bool remote_side_effect_possible;
};

bool node_transaction_key_equal(const struct node_transaction_key *left,
                                const struct node_transaction_key *right);
void node_transaction_init(struct node_transaction *transaction);
int node_transaction_begin(struct node_transaction *transaction,
                           const struct node_transaction_spec *spec,
                           uint32_t delivery_handle,
                           uint64_t now_ms);
int node_transaction_note_request_terminal(
    struct node_transaction *transaction,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action);
int node_transaction_note_request_redrive(
    struct node_transaction *transaction,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action);
int node_transaction_reconcile_result(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_token,
    uint64_t now_ms,
    enum node_transaction_result_disposition *disposition,
    enum node_transaction_action *action);
int node_transaction_cancel(struct node_transaction *transaction,
                            uint64_t now_ms,
                            enum node_transaction_action *action);
bool node_transaction_service(struct node_transaction *transaction,
                              uint64_t now_ms,
                              enum node_transaction_action *action);
int node_transaction_cleanup_complete(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    uint64_t now_ms,
    enum node_transaction_action *action);
int node_transaction_retire(struct node_transaction *transaction);
bool node_transaction_terminal(const struct node_transaction *transaction);

enum node_transaction_response_state {
    NODE_TRANSACTION_RESPONSE_EMPTY = 0,
    NODE_TRANSACTION_RESPONSE_EXECUTING,
    NODE_TRANSACTION_RESPONSE_COMMITTED,
    NODE_TRANSACTION_RESPONSE_ABANDONED,
};

enum node_transaction_receive_disposition {
    NODE_TRANSACTION_RECEIVE_EXECUTE = 0,
    NODE_TRANSACTION_RECEIVE_REPLAY,
    NODE_TRANSACTION_RECEIVE_COALESCE,
    NODE_TRANSACTION_RECEIVE_EXPIRED,
    NODE_TRANSACTION_RECEIVE_ABANDONED,
    NODE_TRANSACTION_RECEIVE_CONFLICT,
    NODE_TRANSACTION_RECEIVE_FULL,
};

struct node_transaction_response_record {
    struct node_transaction_key key;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t result_token;
    uint64_t expires_at_ms;
    enum node_transaction_response_state state;
};

struct node_transaction_responder {
    struct node_transaction_response_record
        records[NODE_TRANSACTION_RESPONDER_MAX_RECORDS];
};

void node_transaction_responder_init(
    struct node_transaction_responder *responder);
int node_transaction_responder_receive(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint64_t expires_at_ms,
    uint64_t now_ms,
    enum node_transaction_receive_disposition *disposition,
    uint8_t cached_result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t *cached_result_token);
int node_transaction_responder_commit(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_token);
int node_transaction_responder_abandon(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint64_t expires_at_ms,
    uint64_t now_ms);
size_t node_transaction_responder_service(
    struct node_transaction_responder *responder,
    uint64_t now_ms);

#endif
