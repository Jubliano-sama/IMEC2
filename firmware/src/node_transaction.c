#include "node_transaction.h"

#include <errno.h>
#include <string.h>

#define NODE_TRANSACTION_FINGERPRINT_OFFSET UINT32_C(2166136261)
#define NODE_TRANSACTION_FINGERPRINT_PRIME UINT32_C(16777619)

uint32_t node_transaction_fingerprint_bytes(uint32_t seed,
                                            const uint8_t *bytes,
                                            size_t length)
{
    uint32_t fingerprint = seed == 0u ?
                           NODE_TRANSACTION_FINGERPRINT_OFFSET : seed;

    if (bytes == NULL && length != 0u) {
        return 0u;
    }
    for (size_t i = 0u; i < length; i++) {
        fingerprint ^= bytes[i];
        fingerprint *= NODE_TRANSACTION_FINGERPRINT_PRIME;
        if (fingerprint == 0u) {
            fingerprint = NODE_TRANSACTION_FINGERPRINT_OFFSET;
        }
    }
    return fingerprint;
}

static bool node_transaction_key_valid(const struct node_transaction_key *key)
{
    return key != NULL &&
           key->requester_id != 0u &&
           key->responder_id != 0u &&
           key->requester_id != key->responder_id &&
           key->session_id != 0u &&
           key->transaction_id != 0u &&
           key->operation_id != 0u;
}

bool node_transaction_key_equal(const struct node_transaction_key *left,
                                const struct node_transaction_key *right)
{
    return left != NULL && right != NULL &&
           left->requester_id == right->requester_id &&
           left->responder_id == right->responder_id &&
           left->session_id == right->session_id &&
           left->transaction_id == right->transaction_id &&
           left->operation_id == right->operation_id;
}

static enum node_transaction_action node_transaction_state_action(
    const struct node_transaction *transaction)
{
    if (transaction == NULL) {
        return NODE_TRANSACTION_ACTION_NONE;
    }
    switch (transaction->state) {
    case NODE_TRANSACTION_ACTIVE:
        return NODE_TRANSACTION_ACTION_WAIT_RESULT;
    case NODE_TRANSACTION_ABANDONING:
        return NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED;
    case NODE_TRANSACTION_SUCCEEDED:
        return NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS;
    case NODE_TRANSACTION_ABANDONED:
        return NODE_TRANSACTION_ACTION_TERMINAL_ABANDON;
    case NODE_TRANSACTION_EMPTY:
    default:
        return NODE_TRANSACTION_ACTION_NONE;
    }
}

static void node_transaction_set_action(
    const struct node_transaction *transaction,
    enum node_transaction_action *action)
{
    if (action != NULL) {
        *action = node_transaction_state_action(transaction);
    }
}

static void node_transaction_abandon(
    struct node_transaction *transaction,
    enum node_transaction_abandon_reason reason)
{
    transaction->abandon_reason = reason;
    if (transaction->spec.cleanup_required &&
        transaction->remote_side_effect_possible) {
        transaction->state = NODE_TRANSACTION_ABANDONING;
    } else {
        transaction->state = NODE_TRANSACTION_ABANDONED;
    }
}

void node_transaction_init(struct node_transaction *transaction)
{
    if (transaction != NULL) {
        memset(transaction, 0, sizeof(*transaction));
    }
}

int node_transaction_begin(struct node_transaction *transaction,
                           const struct node_transaction_spec *spec,
                           uint32_t delivery_handle,
                           uint64_t now_ms)
{
    if (transaction == NULL || spec == NULL || delivery_handle == 0u ||
        !node_transaction_key_valid(&spec->key)) {
        return -EINVAL;
    }
    if (transaction->state != NODE_TRANSACTION_EMPTY) {
        return -EBUSY;
    }
    if (spec->absolute_deadline_ms <= now_ms) {
        return -ETIMEDOUT;
    }

    memset(transaction, 0, sizeof(*transaction));
    transaction->spec = *spec;
    transaction->request_delivery_handle = delivery_handle;
    transaction->state = NODE_TRANSACTION_ACTIVE;
    /* Until the delivery service proves that RF never started, cleanup wins. */
    transaction->remote_side_effect_possible = spec->cleanup_required;
    return 0;
}

bool node_transaction_service(struct node_transaction *transaction,
                              uint64_t now_ms,
                              enum node_transaction_action *action)
{
    bool expired = false;

    if (transaction != NULL &&
        transaction->state == NODE_TRANSACTION_ACTIVE &&
        now_ms >= transaction->spec.absolute_deadline_ms) {
        node_transaction_abandon(transaction,
                                 NODE_TRANSACTION_ABANDON_DEADLINE);
        expired = true;
    }
    node_transaction_set_action(transaction, action);
    return expired;
}

int node_transaction_note_request_terminal(
    struct node_transaction *transaction,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    if (transaction == NULL || event == NULL ||
        event->reason > NODE_COMM_TERMINAL_CANCELLED) {
        return -EINVAL;
    }
    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        event->handle != transaction->request_delivery_handle ||
        event->client_token != transaction->spec.client_token) {
        return -ESTALE;
    }

    if (event->attempts_started > transaction->request_attempts_started) {
        transaction->request_attempts_started = event->attempts_started;
    }
    transaction->request_delivery_terminal = true;
    if (event->attempts_started == 0u &&
        event->reason != NODE_COMM_TERMINAL_DELIVERED) {
        transaction->remote_side_effect_possible = false;
    } else {
        transaction->remote_side_effect_possible = true;
    }

    (void)node_transaction_service(transaction, now_ms, action);
    if (transaction->state != NODE_TRANSACTION_ACTIVE) {
        node_transaction_set_action(transaction, action);
        return 0;
    }
    if (event->reason == NODE_COMM_TERMINAL_DELIVERED) {
        node_transaction_set_action(transaction, action);
        return 0;
    }

    node_transaction_abandon(
        transaction,
        event->reason == NODE_COMM_TERMINAL_CANCELLED ?
            NODE_TRANSACTION_ABANDON_CANCELLED :
            NODE_TRANSACTION_ABANDON_DELIVERY_FAILED);
    node_transaction_set_action(transaction, action);
    return 0;
}

int node_transaction_reconcile_result(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    uint32_t request_fingerprint,
    uint32_t result_fingerprint,
    uint32_t result_token,
    uint64_t now_ms,
    enum node_transaction_result_disposition *disposition,
    enum node_transaction_action *action)
{
    if (transaction == NULL || key == NULL || disposition == NULL ||
        action == NULL) {
        return -EINVAL;
    }
    *action = NODE_TRANSACTION_ACTION_NONE;
    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        !node_transaction_key_equal(&transaction->spec.key, key)) {
        *disposition = NODE_TRANSACTION_RESULT_STALE;
        return 0;
    }
    (void)node_transaction_service(transaction, now_ms, action);
    if (request_fingerprint != transaction->spec.request_fingerprint) {
        *disposition = NODE_TRANSACTION_RESULT_CONFLICT;
        node_transaction_set_action(transaction, action);
        return 0;
    }

    if (transaction->state == NODE_TRANSACTION_ABANDONING ||
        transaction->state == NODE_TRANSACTION_ABANDONED) {
        *disposition = NODE_TRANSACTION_RESULT_LATE_AFTER_ABANDON;
        node_transaction_set_action(transaction, action);
        return 0;
    }
    if (transaction->state == NODE_TRANSACTION_SUCCEEDED) {
        if (transaction->accepted_result_fingerprint == result_fingerprint &&
            transaction->result_token == result_token) {
            *disposition = NODE_TRANSACTION_RESULT_DUPLICATE;
        } else {
            *disposition = NODE_TRANSACTION_RESULT_CONFLICT;
        }
        node_transaction_set_action(transaction, action);
        return 0;
    }
    if (transaction->state != NODE_TRANSACTION_ACTIVE) {
        *disposition = NODE_TRANSACTION_RESULT_STALE;
        node_transaction_set_action(transaction, action);
        return 0;
    }

    transaction->accepted_result_fingerprint = result_fingerprint;
    transaction->result_token = result_token;
    transaction->remote_side_effect_possible = true;
    transaction->state = NODE_TRANSACTION_SUCCEEDED;
    *disposition = NODE_TRANSACTION_RESULT_ACCEPTED;
    node_transaction_set_action(transaction, action);
    return 0;
}

int node_transaction_cancel(struct node_transaction *transaction,
                            uint64_t now_ms,
                            enum node_transaction_action *action)
{
    if (transaction == NULL || action == NULL) {
        return -EINVAL;
    }
    (void)node_transaction_service(transaction, now_ms, action);
    if (transaction->state == NODE_TRANSACTION_EMPTY) {
        return -ENOENT;
    }
    if (transaction->state == NODE_TRANSACTION_SUCCEEDED ||
        transaction->state == NODE_TRANSACTION_ABANDONED) {
        return -EALREADY;
    }
    if (transaction->state == NODE_TRANSACTION_ACTIVE) {
        node_transaction_abandon(transaction,
                                 NODE_TRANSACTION_ABANDON_CANCELLED);
    }
    node_transaction_set_action(transaction, action);
    return 0;
}

int node_transaction_cleanup_complete(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    (void)now_ms;
    if (transaction == NULL || key == NULL || action == NULL) {
        return -EINVAL;
    }
    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        !node_transaction_key_equal(&transaction->spec.key, key)) {
        return -ESTALE;
    }
    if (transaction->state != NODE_TRANSACTION_ABANDONING) {
        return -EALREADY;
    }

    transaction->state = NODE_TRANSACTION_ABANDONED;
    node_transaction_set_action(transaction, action);
    return 0;
}

bool node_transaction_terminal(const struct node_transaction *transaction)
{
    return transaction != NULL &&
           (transaction->state == NODE_TRANSACTION_SUCCEEDED ||
            transaction->state == NODE_TRANSACTION_ABANDONED);
}

int node_transaction_retire(struct node_transaction *transaction)
{
    if (transaction == NULL) {
        return -EINVAL;
    }
    if (transaction->state == NODE_TRANSACTION_EMPTY) {
        return -EALREADY;
    }
    if (!node_transaction_terminal(transaction)) {
        return -EBUSY;
    }
    memset(transaction, 0, sizeof(*transaction));
    return 0;
}

static struct node_transaction_response_record *responder_find(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key)
{
    for (size_t i = 0u; i < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; i++) {
        if (responder->records[i].state != NODE_TRANSACTION_RESPONSE_EMPTY &&
            node_transaction_key_equal(&responder->records[i].key, key)) {
            return &responder->records[i];
        }
    }
    return NULL;
}

static struct node_transaction_response_record *responder_find_empty(
    struct node_transaction_responder *responder)
{
    for (size_t i = 0u; i < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; i++) {
        if (responder->records[i].state == NODE_TRANSACTION_RESPONSE_EMPTY) {
            return &responder->records[i];
        }
    }
    return NULL;
}

void node_transaction_responder_init(
    struct node_transaction_responder *responder)
{
    if (responder != NULL) {
        memset(responder, 0, sizeof(*responder));
    }
}

size_t node_transaction_responder_service(
    struct node_transaction_responder *responder,
    uint64_t now_ms)
{
    size_t expired = 0u;

    if (responder == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; i++) {
        struct node_transaction_response_record *record =
            &responder->records[i];

        if (record->state != NODE_TRANSACTION_RESPONSE_EMPTY &&
            now_ms >= record->expires_at_ms) {
            memset(record, 0, sizeof(*record));
            expired++;
        }
    }
    return expired;
}

int node_transaction_responder_receive(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint32_t request_fingerprint,
    uint64_t expires_at_ms,
    uint64_t now_ms,
    enum node_transaction_receive_disposition *disposition,
    uint32_t *cached_result_token)
{
    struct node_transaction_response_record *record;

    if (responder == NULL || !node_transaction_key_valid(key) ||
        disposition == NULL) {
        return -EINVAL;
    }
    if (cached_result_token != NULL) {
        *cached_result_token = 0u;
    }
    (void)node_transaction_responder_service(responder, now_ms);
    if (expires_at_ms <= now_ms) {
        *disposition = NODE_TRANSACTION_RECEIVE_EXPIRED;
        return 0;
    }

    record = responder_find(responder, key);
    if (record != NULL) {
        if (record->state == NODE_TRANSACTION_RESPONSE_ABANDONED) {
            *disposition = NODE_TRANSACTION_RECEIVE_ABANDONED;
            return 0;
        }
        if (record->request_fingerprint != request_fingerprint) {
            *disposition = NODE_TRANSACTION_RECEIVE_CONFLICT;
            return 0;
        }
        if (expires_at_ms < record->expires_at_ms) {
            record->expires_at_ms = expires_at_ms;
        }
        switch (record->state) {
        case NODE_TRANSACTION_RESPONSE_EXECUTING:
            *disposition = NODE_TRANSACTION_RECEIVE_COALESCE;
            return 0;
        case NODE_TRANSACTION_RESPONSE_COMMITTED:
            *disposition = NODE_TRANSACTION_RECEIVE_REPLAY;
            if (cached_result_token != NULL) {
                *cached_result_token = record->result_token;
            }
            return 0;
        case NODE_TRANSACTION_RESPONSE_EMPTY:
        case NODE_TRANSACTION_RESPONSE_ABANDONED:
        default:
            return -EPROTO;
        }
    }

    record = responder_find_empty(responder);
    if (record == NULL) {
        *disposition = NODE_TRANSACTION_RECEIVE_FULL;
        return 0;
    }
    memset(record, 0, sizeof(*record));
    record->key = *key;
    record->request_fingerprint = request_fingerprint;
    record->expires_at_ms = expires_at_ms;
    record->state = NODE_TRANSACTION_RESPONSE_EXECUTING;
    *disposition = NODE_TRANSACTION_RECEIVE_EXECUTE;
    return 0;
}

int node_transaction_responder_commit(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint32_t request_fingerprint,
    uint32_t result_fingerprint,
    uint32_t result_token)
{
    struct node_transaction_response_record *record;

    if (responder == NULL || !node_transaction_key_valid(key)) {
        return -EINVAL;
    }
    record = responder_find(responder, key);
    if (record == NULL) {
        return -ENOENT;
    }
    if (record->request_fingerprint != request_fingerprint) {
        return -EPROTO;
    }
    if (record->state == NODE_TRANSACTION_RESPONSE_ABANDONED) {
        return -ESTALE;
    }
    if (record->state == NODE_TRANSACTION_RESPONSE_COMMITTED) {
        return record->result_fingerprint == result_fingerprint &&
               record->result_token == result_token ? 0 : -EEXIST;
    }
    if (record->state != NODE_TRANSACTION_RESPONSE_EXECUTING) {
        return -EPROTO;
    }

    record->result_fingerprint = result_fingerprint;
    record->result_token = result_token;
    record->state = NODE_TRANSACTION_RESPONSE_COMMITTED;
    return 0;
}

int node_transaction_responder_abandon(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint64_t expires_at_ms,
    uint64_t now_ms)
{
    struct node_transaction_response_record *record;

    if (responder == NULL || !node_transaction_key_valid(key)) {
        return -EINVAL;
    }
    (void)node_transaction_responder_service(responder, now_ms);
    if (expires_at_ms <= now_ms) {
        return -ETIMEDOUT;
    }
    record = responder_find(responder, key);
    if (record == NULL) {
        record = responder_find_empty(responder);
        if (record == NULL) {
            return -ENOSPC;
        }
        memset(record, 0, sizeof(*record));
        record->key = *key;
    }
    if (expires_at_ms > record->expires_at_ms) {
        record->expires_at_ms = expires_at_ms;
    }
    record->result_fingerprint = 0u;
    record->result_token = 0u;
    record->state = NODE_TRANSACTION_RESPONSE_ABANDONED;
    return 0;
}
