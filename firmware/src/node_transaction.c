#include "node_transaction.h"
#include "protocol.h"

#include <errno.h>
#include <string.h>

bool node_transaction_digest_bytes(
    const uint8_t *bytes,
    size_t length,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return semantic_digest_sha256(bytes, length, digest);
}

bool node_transaction_digest_packet(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct semantic_digest_sha256_context digest_context;
    uint8_t header[PACKET_EXT_HEADER_LEN] = {0};
    uint8_t encoded_crc[PACKET_CRC_LEN];
    size_t header_len;
    uint16_t crc;

    if (packet == NULL || digest == NULL ||
        packet->payload_len != payload_len ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN ||
        (payload_len != 0u && payload == NULL)) {
        return false;
    }

    header[0] = PROTO_MAGIC;
    header[1] = PROTO_VERSION;
    header[2] = packet->msg_type;
    header[3] = packet->flags;
    proto_put_u64_le(&header[4], packet->src_id);
    proto_put_u64_le(&header[12], packet->dst_id);
    proto_put_u32_le(&header[20], packet->session_id);
    proto_put_u16_le(&header[24], packet->seq);
    header[26] = packet->ttl;
    if (payload_len < PACKET_EXT_LENGTH_SENTINEL) {
        header[27] = (uint8_t)payload_len;
        proto_put_u32_le(&header[28], packet->message_age_ms);
    } else {
        header[27] = PACKET_EXT_LENGTH_SENTINEL;
        proto_put_u16_le(&header[28], (uint16_t)payload_len);
        proto_put_u32_le(&header[30], packet->message_age_ms);
    }
    header_len = proto_packet_header_len((uint16_t)payload_len);

    crc = proto_crc16_ccitt_false_update(
        UINT16_C(0xFFFF), header, header_len);
    crc = proto_crc16_ccitt_false_update(crc, payload, payload_len);
    proto_put_u16_le(encoded_crc, crc);
    return semantic_digest_sha256_init(&digest_context) &&
           semantic_digest_sha256_update(&digest_context,
                                         header,
                                         header_len) &&
           semantic_digest_sha256_update(&digest_context,
                                         payload,
                                         payload_len) &&
           semantic_digest_sha256_update(&digest_context,
                                         encoded_crc,
                                         sizeof(encoded_crc)) &&
           semantic_digest_sha256_final(&digest_context, digest);
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
    bool zero_rf_authoritative;

    if (transaction == NULL || event == NULL ||
        (int)event->reason < (int)NODE_COMM_TERMINAL_DELIVERED ||
        event->reason > NODE_COMM_TERMINAL_CANCELLED) {
        return -EINVAL;
    }
    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        event->handle != transaction->request_delivery_handle ||
        event->client_token != transaction->spec.client_token) {
        return -ESTALE;
    }
    if (event->reason == NODE_COMM_TERMINAL_DELIVERED &&
        event->attempts_started == 0u) {
        return -EPROTO;
    }

    zero_rf_authoritative =
        event->attempts_started == 0u &&
        event->reason != NODE_COMM_TERMINAL_DELIVERED &&
        transaction->request_attempts_started == 0u &&
        transaction->state != NODE_TRANSACTION_SUCCEEDED;
    if (event->attempts_started > transaction->request_attempts_started) {
        transaction->request_attempts_started = event->attempts_started;
    }
    transaction->request_delivery_terminal = true;
    if (zero_rf_authoritative) {
        transaction->remote_side_effect_possible = false;
    } else {
        transaction->remote_side_effect_possible = true;
    }

    (void)node_transaction_service(transaction, now_ms, action);
    if (transaction->state == NODE_TRANSACTION_ABANDONING &&
        zero_rf_authoritative) {
        /*
         * Deadline/cancel may have conservatively entered ABANDONING before
         * the delivery owner supplied its terminal proof. Zero RF attempts
         * remove the only possible remote side effect, so no cleanup remains.
         */
        transaction->state = NODE_TRANSACTION_ABANDONED;
    }
    if (transaction->state != NODE_TRANSACTION_ACTIVE) {
        node_transaction_set_action(transaction, action);
        return 0;
    }
    if (event->reason == NODE_COMM_TERMINAL_DELIVERED) {
        node_transaction_set_action(transaction, action);
        return 0;
    }

    /*
     * Once RF has started, a failed request-delivery owner cannot prove that
     * the responder did not execute the request. Keep the semantic
     * transaction alive until its later result deadline so an exact response
     * can resolve that ambiguity. Explicit cancellation still starts cleanup
     * immediately; zero-RF failures are handled above.
     */
    if (!zero_rf_authoritative &&
        event->reason != NODE_COMM_TERMINAL_CANCELLED) {
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

int node_transaction_note_request_redrive(
    struct node_transaction *transaction,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    if (transaction == NULL || event == NULL || action == NULL ||
        transaction->state != NODE_TRANSACTION_ACTIVE ||
        transaction->request_delivery_terminal ||
        event->handle != transaction->request_delivery_handle ||
        event->client_token != transaction->spec.client_token ||
        event->reason != NODE_COMM_TERMINAL_DELIVERED ||
        event->attempts_started == 0u ||
        now_ms >= transaction->spec.absolute_deadline_ms) {
        return -ESTALE;
    }
    if (event->attempts_started > transaction->request_attempts_started) {
        transaction->request_attempts_started = event->attempts_started;
    }
    transaction->remote_side_effect_possible = true;
    node_transaction_set_action(transaction, action);
    return 0;
}

int node_transaction_reconcile_result(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_token,
    uint64_t now_ms,
    enum node_transaction_result_disposition *disposition,
    enum node_transaction_action *action)
{
    if (transaction == NULL || key == NULL || request_digest == NULL ||
        result_digest == NULL || disposition == NULL || action == NULL) {
        return -EINVAL;
    }
    *action = NODE_TRANSACTION_ACTION_NONE;
    if (transaction->state == NODE_TRANSACTION_EMPTY ||
        !node_transaction_key_equal(&transaction->spec.key, key)) {
        *disposition = NODE_TRANSACTION_RESULT_STALE;
        return 0;
    }
    (void)node_transaction_service(transaction, now_ms, action);
    if (!semantic_digest_equal(request_digest,
                               transaction->spec.request_digest,
                               SEMANTIC_DIGEST_SHA256_LEN)) {
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
        if (semantic_digest_equal(transaction->accepted_result_digest,
                                  result_digest,
                                  SEMANTIC_DIGEST_SHA256_LEN) &&
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

    memcpy(transaction->accepted_result_digest,
           result_digest,
           sizeof(transaction->accepted_result_digest));
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
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint64_t expires_at_ms,
    uint64_t now_ms,
    enum node_transaction_receive_disposition *disposition,
    uint8_t cached_result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t *cached_result_token)
{
    struct node_transaction_response_record *record;
    uint8_t request_digest_copy[SEMANTIC_DIGEST_SHA256_LEN];

    if (responder == NULL || !node_transaction_key_valid(key) ||
        request_digest == NULL || disposition == NULL) {
        return -EINVAL;
    }
    /*
     * The optional replay output may alias caller-owned request storage. Copy
     * the authority before clearing outputs so aliasing cannot turn an exact
     * request into a conflict or a different cache record.
     */
    memcpy(request_digest_copy, request_digest, sizeof(request_digest_copy));
    if (cached_result_digest != NULL) {
        memset(cached_result_digest, 0, SEMANTIC_DIGEST_SHA256_LEN);
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
        if (!semantic_digest_equal(record->request_digest,
                                   request_digest_copy,
                                   SEMANTIC_DIGEST_SHA256_LEN)) {
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
            if (cached_result_digest != NULL) {
                memcpy(cached_result_digest,
                       record->result_digest,
                       SEMANTIC_DIGEST_SHA256_LEN);
            }
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
    memcpy(record->request_digest,
           request_digest_copy,
           sizeof(record->request_digest));
    record->expires_at_ms = expires_at_ms;
    record->state = NODE_TRANSACTION_RESPONSE_EXECUTING;
    *disposition = NODE_TRANSACTION_RECEIVE_EXECUTE;
    return 0;
}

int node_transaction_responder_commit(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_token)
{
    struct node_transaction_response_record *record;

    if (responder == NULL || !node_transaction_key_valid(key) ||
        request_digest == NULL || result_digest == NULL) {
        return -EINVAL;
    }
    record = responder_find(responder, key);
    if (record == NULL) {
        return -ENOENT;
    }
    if (!semantic_digest_equal(record->request_digest,
                               request_digest,
                               SEMANTIC_DIGEST_SHA256_LEN)) {
        return -EPROTO;
    }
    if (record->state == NODE_TRANSACTION_RESPONSE_ABANDONED) {
        return -ESTALE;
    }
    if (record->state == NODE_TRANSACTION_RESPONSE_COMMITTED) {
        return semantic_digest_equal(record->result_digest,
                                     result_digest,
                                     SEMANTIC_DIGEST_SHA256_LEN) &&
               record->result_token == result_token ? 0 : -EEXIST;
    }
    if (record->state != NODE_TRANSACTION_RESPONSE_EXECUTING) {
        return -EPROTO;
    }

    memcpy(record->result_digest,
           result_digest,
           sizeof(record->result_digest));
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
    memset(record->result_digest, 0, sizeof(record->result_digest));
    record->result_token = 0u;
    record->state = NODE_TRANSACTION_RESPONSE_ABANDONED;
    return 0;
}
