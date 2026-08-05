#include "node_transaction.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define PROPERTY_ITERATIONS 25000u

static uint32_t test_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static struct node_transaction_key test_key(uint16_t transaction_id)
{
    return (struct node_transaction_key) {
        .requester_id = UINT64_C(0x1111222233334444),
        .responder_id = UINT64_C(0xaaaabbbbccccdddd),
        .session_id = UINT32_C(0x10203040),
        .transaction_id = transaction_id,
        .operation_id = UINT16_C(0x0101),
    };
}

static void test_digest_u32(
    uint32_t value,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    uint8_t encoded[sizeof(value)];

    proto_put_u32_le(encoded, value);
    assert(node_transaction_digest_bytes(encoded, sizeof(encoded), digest));
}

static void different_digest(
    const uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint8_t changed[SEMANTIC_DIGEST_SHA256_LEN])
{
    memcpy(changed, digest, SEMANTIC_DIGEST_SHA256_LEN);
    changed[SEMANTIC_DIGEST_SHA256_LEN - 1u] ^= 1u;
}

static struct node_transaction_spec test_spec(uint16_t transaction_id,
                                              uint64_t deadline_ms,
                                              bool cleanup_required)
{
    struct node_transaction_spec spec = {0};

    spec.key = test_key(transaction_id);
    test_digest_u32(UINT32_C(0xaabbccdd) ^ transaction_id,
                    spec.request_digest);
    spec.client_token = UINT32_C(0x87650000) | transaction_id;
    spec.absolute_deadline_ms = deadline_ms;
    spec.cleanup_required = cleanup_required;
    return spec;
}

static struct node_comm_terminal_event terminal_event(
    const struct node_transaction *transaction,
    enum node_comm_terminal_reason reason,
    uint8_t attempts_started)
{
    return (struct node_comm_terminal_event) {
        .handle = transaction->request_delivery_handle,
        .client_token = transaction->spec.client_token,
        .reason = reason,
        .attempts_started = attempts_started,
    };
}

static int reconcile_result_u32(
    struct node_transaction *transaction,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t result_value,
    uint32_t result_token,
    uint64_t now_ms,
    enum node_transaction_result_disposition *disposition,
    enum node_transaction_action *action)
{
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];

    test_digest_u32(result_value, result_digest);
    return node_transaction_reconcile_result(transaction,
                                             key,
                                             request_digest,
                                             result_digest,
                                             result_token,
                                             now_ms,
                                             disposition,
                                             action);
}

static int responder_receive_u32(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint32_t request_value,
    uint64_t expires_at_ms,
    uint64_t now_ms,
    enum node_transaction_receive_disposition *disposition,
    uint8_t cached_result_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t *cached_result_token)
{
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];

    test_digest_u32(request_value, request_digest);
    return node_transaction_responder_receive(responder,
                                              key,
                                              request_digest,
                                              expires_at_ms,
                                              now_ms,
                                              disposition,
                                              cached_result_digest,
                                              cached_result_token);
}

static int responder_commit_u32(
    struct node_transaction_responder *responder,
    const struct node_transaction_key *key,
    uint32_t request_value,
    uint32_t result_value,
    uint32_t result_token)
{
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];

    test_digest_u32(request_value, request_digest);
    test_digest_u32(result_value, result_digest);
    return node_transaction_responder_commit(responder,
                                             key,
                                             request_digest,
                                             result_digest,
                                             result_token);
}

static void assert_key_equal(const struct node_transaction_key *left,
                             const struct node_transaction_key *right)
{
    assert(node_transaction_key_equal(left, right));
    assert(left->requester_id == right->requester_id);
    assert(left->responder_id == right->responder_id);
    assert(left->session_id == right->session_id);
    assert(left->transaction_id == right->transaction_id);
    assert(left->operation_id == right->operation_id);
}

static void test_digest_is_deterministic(void)
{
    static const uint8_t bytes[] = {
        0x10u, 0x00u, 0xffu, 0x42u, 0x81u, 0x7eu, 0x5au,
    };
    uint8_t one_shot[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t repeated[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t shorter[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t empty[SEMANTIC_DIGEST_SHA256_LEN];

    assert(node_transaction_digest_bytes(NULL, 0u, empty));
    assert(!node_transaction_digest_bytes(NULL, 1u, one_shot));
    assert(!node_transaction_digest_bytes(bytes, sizeof(bytes), NULL));
    assert(node_transaction_digest_bytes(bytes, sizeof(bytes), one_shot));
    assert(node_transaction_digest_bytes(bytes, sizeof(bytes), repeated));
    assert(node_transaction_digest_bytes(bytes, sizeof(bytes) - 1u, shorter));
    assert(semantic_digest_equal(one_shot, repeated, sizeof(one_shot)));
    assert(!semantic_digest_equal(one_shot, shorter, sizeof(one_shot)));
    assert(!semantic_digest_equal(one_shot, empty, sizeof(one_shot)));
}

static void test_packet_digest_matches_canonical_encoding(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = UINT64_C(0x1111222233334444),
        .dst_id = UINT64_C(0xaaaabbbbccccdddd),
        .session_id = UINT32_C(0x12345678),
        .seq = UINT16_C(0x3344),
        .ttl = 4u,
        .payload_len = 0u,
        .message_age_ms = UINT32_C(0x55667788),
    };
    uint8_t payload[PACKET_EXT_LENGTH_SENTINEL + 1u];
    uint8_t encoded[PACKET_EXT_HEADER_LEN + sizeof(payload) + PACKET_CRC_LEN];
    const size_t payload_lengths[] = {
        0u,
        PACKET_EXT_LENGTH_SENTINEL - 1u,
        PACKET_EXT_LENGTH_SENTINEL,
        sizeof(payload),
    };

    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i ^ (i >> 3));
    }
    for (size_t i = 0u;
         i < sizeof(payload_lengths) / sizeof(payload_lengths[0]);
         i++) {
        size_t encoded_len = 0u;
        size_t payload_len = payload_lengths[i];
        uint8_t expected[SEMANTIC_DIGEST_SHA256_LEN];
        uint8_t actual[SEMANTIC_DIGEST_SHA256_LEN];

        packet.payload_len = (uint16_t)payload_len;
        assert(proto_packet_encode(&packet,
                                   payload_len == 0u ? NULL : payload,
                                   encoded,
                                   sizeof(encoded),
                                   &encoded_len) == PROTO_OK);
        assert(node_transaction_digest_bytes(encoded, encoded_len, expected));
        assert(node_transaction_digest_packet(
            &packet,
            payload_len == 0u ? NULL : payload,
            payload_len,
            actual));
        assert(semantic_digest_equal(expected, actual, sizeof(expected)));
    }

    packet.payload_len = 1u;
    {
        uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

        assert(!node_transaction_digest_packet(
            &packet, NULL, 1u, digest));
        assert(!node_transaction_digest_packet(
            &packet, payload, 0u, digest));
        assert(!node_transaction_digest_packet(
            NULL, payload, 1u, digest));
        assert(!node_transaction_digest_packet(
            &packet, payload, 1u, NULL));
    }
}

static void test_begin_requires_stable_valid_identity_and_future_deadline(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(1u, 1000u, true);

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 7u, 1000u) ==
           -ETIMEDOUT);
    assert(transaction.state == NODE_TRANSACTION_EMPTY);
    assert(node_transaction_begin(&transaction, &spec, 7u, 999u) == 0);
    assert(transaction.state == NODE_TRANSACTION_ACTIVE);
    assert_key_equal(&transaction.spec.key, &spec.key);
    assert(node_transaction_begin(&transaction, &spec, 8u, 999u) == -EBUSY);
    assert(node_transaction_retire(&transaction) == -EBUSY);
}

static void test_matching_result_survives_transport_retries(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(2u, 1000u, true);
    struct node_comm_terminal_event delivered;
    enum node_transaction_result_disposition disposition;
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 22u, 100u) == 0);
    delivered = terminal_event(&transaction, NODE_COMM_TERMINAL_DELIVERED, 4u);
    assert(node_transaction_note_request_terminal(
               &transaction, &delivered, 900u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_WAIT_RESULT);
    assert(transaction.state == NODE_TRANSACTION_ACTIVE);
    assert(transaction.request_attempts_started == 4u);

    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               UINT32_C(0x12345678), 9u, 999u,
               &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_ACCEPTED);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
    assert(transaction.state == NODE_TRANSACTION_SUCCEEDED);
    assert(node_transaction_terminal(&transaction));
}

static void test_exact_deadline_rejects_result_and_requires_cleanup(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(3u, 1000u, true);
    uint8_t conflicting_request[SEMANTIC_DIGEST_SHA256_LEN];
    enum node_transaction_result_disposition disposition;
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 33u, 100u) == 0);
    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               UINT32_C(0x11111111), 10u, 1000u,
               &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_LATE_AFTER_ABANDON);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    assert(transaction.abandon_reason == NODE_TRANSACTION_ABANDON_DEADLINE);
    assert(!node_transaction_terminal(&transaction));

    different_digest(spec.request_digest, conflicting_request);
    assert(reconcile_result_u32(
               &transaction, &spec.key, conflicting_request,
               UINT32_C(0x22222222), 11u, 1001u,
               &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_CONFLICT);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);

    assert(node_transaction_cleanup_complete(
               &transaction, &spec.key, 1001u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON);
    assert(node_transaction_terminal(&transaction));
    assert(node_transaction_retire(&transaction) == 0);
    assert(transaction.state == NODE_TRANSACTION_EMPTY);
}

static void test_duplicate_conflict_and_stale_results_are_inert(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(4u, 2000u, false);
    struct node_transaction_key wrong_key = spec.key;
    uint8_t conflicting_request[SEMANTIC_DIGEST_SHA256_LEN];
    enum node_transaction_result_disposition disposition;
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 44u, 100u) == 0);
    wrong_key.transaction_id++;
    assert(reconcile_result_u32(
               &transaction, &wrong_key, spec.request_digest,
               100u, 20u, 200u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_STALE);
    assert(transaction.state == NODE_TRANSACTION_ACTIVE);
    different_digest(spec.request_digest, conflicting_request);
    assert(reconcile_result_u32(
               &transaction, &spec.key, conflicting_request,
               100u, 20u, 200u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_CONFLICT);
    assert(transaction.state == NODE_TRANSACTION_ACTIVE);

    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               100u, 20u, 200u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_ACCEPTED);
    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               100u, 20u, 2500u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_DUPLICATE);
    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               101u, 20u, 2500u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_CONFLICT);
    assert(reconcile_result_u32(
               &transaction, &spec.key, spec.request_digest,
               100u, 21u, 2500u, &disposition, &action) == 0);
    assert(disposition == NODE_TRANSACTION_RESULT_CONFLICT);
    assert(transaction.state == NODE_TRANSACTION_SUCCEEDED);
}

static void test_terminal_delivery_failure_uses_attempt_evidence(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(5u, 5000u, true);
    struct node_comm_terminal_event event;
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 55u, 0u) == 0);
    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED, 3u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 100u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 56u, 0u) == 0);
    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_PERMANENT_FAILURE, 0u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 100u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONED);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON);
}

static void test_late_zero_attempt_proof_retires_conservative_cleanup(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(55u, 1000u, true);
    struct node_comm_terminal_event event;
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 550u, 0u) == 0);
    assert(node_transaction_service(&transaction, 1000u, &action));
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_PERMANENT_FAILURE,
                           0u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 1001u, &action) == 0);
    assert(transaction.request_delivery_terminal);
    assert(!transaction.remote_side_effect_possible);
    assert(transaction.state == NODE_TRANSACTION_ABANDONED);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON);

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 551u, 0u) == 0);
    assert(node_transaction_cancel(&transaction, 10u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_CANCELLED,
                           0u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 11u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONED);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON);
}

static void test_terminal_evidence_cannot_downgrade_known_rf(void)
{
    struct node_transaction transaction;
    struct node_transaction before;
    struct node_transaction_spec spec = test_spec(56u, 5000u, true);
    struct node_comm_terminal_event event;
    enum node_transaction_action action =
        NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 560u, 0u) == 0);
    event = terminal_event(&transaction, NODE_COMM_TERMINAL_DELIVERED, 0u);
    before = transaction;
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 1u, &action) == -EPROTO);
    assert(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);

    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
                           3u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 2u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    assert(transaction.remote_side_effect_possible);
    event = terminal_event(&transaction,
                           NODE_COMM_TERMINAL_CANCELLED,
                           0u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 3u, &action) == 0);
    assert(transaction.state == NODE_TRANSACTION_ABANDONING);
    assert(transaction.remote_side_effect_possible);
    assert(transaction.request_attempts_started == 3u);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
}

static void test_invalid_terminal_reason_is_failure_atomic(void)
{
    struct node_transaction transaction;
    struct node_transaction before;
    struct node_transaction_spec spec = test_spec(57u, 5000u, true);
    struct node_comm_terminal_event event;
    enum node_transaction_action action =
        NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 570u, 0u) == 0);
    before = transaction;

    event = terminal_event(
        &transaction, (enum node_comm_terminal_reason)-1, 1u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 1u, &action) == -EINVAL);
    assert(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);

    event = terminal_event(
        &transaction,
        (enum node_comm_terminal_reason)
            (NODE_COMM_TERMINAL_CANCELLED + 1),
        1u);
    assert(node_transaction_note_request_terminal(
               &transaction, &event, 1u, &action) == -EINVAL);
    assert(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
}

static void test_explicit_cancel_is_idempotent_while_cleaning(void)
{
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(6u, 5000u, true);
    enum node_transaction_action action;

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 66u, 0u) == 0);
    assert(node_transaction_cancel(&transaction, 10u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(node_transaction_cancel(&transaction, 11u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(node_transaction_cleanup_complete(
               &transaction, &spec.key, 12u, &action) == 0);
    assert(node_transaction_cancel(&transaction, 13u, &action) == -EALREADY);
}

static void test_responder_executes_coalesces_commits_and_replays(void)
{
    struct node_transaction_responder responder;
    struct node_transaction_key key = test_key(10u);
    enum node_transaction_receive_disposition disposition;
    uint8_t cached_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t cached_token = 0u;

    node_transaction_responder_init(&responder);
    assert(responder_receive_u32(
               &responder, &key, 100u, 1000u, 10u,
               &disposition, cached_digest, &cached_token) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_EXECUTE);
    assert(responder_receive_u32(
               &responder, &key, 100u, 1000u, 20u,
               &disposition, cached_digest, &cached_token) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_COALESCE);
    assert(responder_receive_u32(
               &responder, &key, 101u, 1000u, 20u,
               &disposition, cached_digest, &cached_token) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_CONFLICT);

    assert(responder_commit_u32(
               &responder, &key, 100u, 200u, 300u) == 0);
    assert(responder_commit_u32(
               &responder, &key, 100u, 200u, 300u) == 0);
    assert(responder_commit_u32(
               &responder, &key, 100u, 201u, 300u) == -EEXIST);
    assert(responder_receive_u32(
               &responder, &key, 100u, 990u, 30u,
               &disposition, cached_digest, &cached_token) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_REPLAY);
    test_digest_u32(200u, expected_digest);
    assert(semantic_digest_equal(cached_digest,
                                 expected_digest,
                                 sizeof(cached_digest)));
    assert(cached_token == 300u);

    /* Optional replay output is allowed to reuse the request buffer. */
    test_digest_u32(100u, cached_digest);
    assert(node_transaction_responder_receive(
               &responder, &key, cached_digest, 980u, 31u,
               &disposition, cached_digest, &cached_token) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_REPLAY);
    assert(semantic_digest_equal(cached_digest,
                                 expected_digest,
                                 sizeof(cached_digest)));
}

static void test_responder_abandon_tombstone_blocks_reordered_request(void)
{
    struct node_transaction_responder responder;
    struct node_transaction_key key = test_key(11u);
    enum node_transaction_receive_disposition disposition;

    node_transaction_responder_init(&responder);
    assert(node_transaction_responder_abandon(
               &responder, &key, 1000u, 10u) == 0);
    assert(responder_receive_u32(
               &responder, &key, 999u, 1000u, 20u,
               &disposition, NULL, NULL) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_ABANDONED);
    assert(node_transaction_responder_service(&responder, 999u) == 0u);
    assert(node_transaction_responder_service(&responder, 1000u) == 1u);
    assert(responder_receive_u32(
               &responder, &key, 999u, 1000u, 1000u,
               &disposition, NULL, NULL) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_EXPIRED);
}

static void test_responder_capacity_releases_at_exact_expiry(void)
{
    struct node_transaction_responder responder;
    enum node_transaction_receive_disposition disposition;

    node_transaction_responder_init(&responder);
    for (uint16_t i = 0u; i < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; i++) {
        struct node_transaction_key key = test_key((uint16_t)(20u + i));

        assert(responder_receive_u32(
                   &responder, &key, i, 100u, 0u,
                   &disposition, NULL, NULL) == 0);
        assert(disposition == NODE_TRANSACTION_RECEIVE_EXECUTE);
    }
    {
        struct node_transaction_key extra = test_key(40u);

        assert(responder_receive_u32(
                   &responder, &extra, 40u, 101u, 0u,
                   &disposition, NULL, NULL) == 0);
        assert(disposition == NODE_TRANSACTION_RECEIVE_FULL);
        assert(node_transaction_responder_service(&responder, 99u) == 0u);
        assert(node_transaction_responder_service(&responder, 100u) ==
               NODE_TRANSACTION_RESPONDER_MAX_RECORDS);
        assert(responder_receive_u32(
                   &responder, &extra, 40u, 101u, 100u,
                   &disposition, NULL, NULL) == 0);
        assert(disposition == NODE_TRANSACTION_RECEIVE_EXECUTE);
    }
}

static void test_responder_deadlines_do_not_wrap_near_u64_max(void)
{
    struct node_transaction_responder responder;
    struct node_transaction_key key = test_key(50u);
    enum node_transaction_receive_disposition disposition;
    const uint64_t expires_at_ms = UINT64_MAX - 1u;

    node_transaction_responder_init(&responder);
    assert(responder_receive_u32(
               &responder, &key, 50u, expires_at_ms, UINT64_MAX - 10u,
               &disposition, NULL, NULL) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_EXECUTE);
    assert(node_transaction_responder_service(
               &responder, UINT64_MAX - 2u) == 0u);
    assert(node_transaction_responder_service(
               &responder, UINT64_MAX - 1u) == 1u);
    assert(responder_receive_u32(
               &responder, &key, 50u, expires_at_ms, expires_at_ms,
               &disposition, NULL, NULL) == 0);
    assert(disposition == NODE_TRANSACTION_RECEIVE_EXPIRED);
}

static uint32_t legacy_fnv1a32(const uint8_t *bytes, size_t length)
{
    uint32_t fingerprint = UINT32_C(2166136261);

    for (size_t i = 0u; i < length; i++) {
        fingerprint ^= bytes[i];
        fingerprint *= UINT32_C(16777619);
    }
    return fingerprint;
}

static void test_deliberate_fnv_collision_is_conflict_everywhere(void)
{
    /*
     * Fixed FNV-1a collision found for F24:
     * b1d77a7016c01aacac64248d
     * 2e0072f2fc53587f6df112a9
     * Both map to 0x1cbf45d8 under the retired 32-bit authority.
     */
    static const uint8_t colliding_a[] = {
        0xb1u, 0xd7u, 0x7au, 0x70u, 0x16u, 0xc0u,
        0x1au, 0xacu, 0xacu, 0x64u, 0x24u, 0x8du,
    };
    static const uint8_t colliding_b[] = {
        0x2eu, 0x00u, 0x72u, 0xf2u, 0xfcu, 0x53u,
        0x58u, 0x7fu, 0x6du, 0xf1u, 0x12u, 0xa9u,
    };
    struct node_transaction transaction;
    struct node_transaction_spec spec = test_spec(52u, 5000u, false);
    struct node_transaction_responder responder;
    struct node_transaction_key responder_key = test_key(53u);
    enum node_transaction_result_disposition result_disposition;
    enum node_transaction_receive_disposition receive_disposition;
    enum node_transaction_action action;
    uint8_t digest_a[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t digest_b[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t ordinary_result[SEMANTIC_DIGEST_SHA256_LEN];

    assert(legacy_fnv1a32(colliding_a, sizeof(colliding_a)) ==
           UINT32_C(0x1cbf45d8));
    assert(legacy_fnv1a32(colliding_b, sizeof(colliding_b)) ==
           UINT32_C(0x1cbf45d8));
    assert(node_transaction_digest_bytes(
        colliding_a, sizeof(colliding_a), digest_a));
    assert(node_transaction_digest_bytes(
        colliding_b, sizeof(colliding_b), digest_b));
    assert(!semantic_digest_equal(digest_a, digest_b, sizeof(digest_a)));
    test_digest_u32(UINT32_C(0x11223344), ordinary_result);

    memcpy(spec.request_digest, digest_a, sizeof(spec.request_digest));
    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 52u, 0u) == 0);
    assert(node_transaction_reconcile_result(
               &transaction, &spec.key, digest_b, ordinary_result,
               1u, 10u, &result_disposition, &action) == 0);
    assert(result_disposition == NODE_TRANSACTION_RESULT_CONFLICT);
    assert(transaction.state == NODE_TRANSACTION_ACTIVE);

    node_transaction_init(&transaction);
    assert(node_transaction_begin(&transaction, &spec, 53u, 0u) == 0);
    assert(node_transaction_reconcile_result(
               &transaction, &spec.key, digest_a, digest_a,
               7u, 10u, &result_disposition, &action) == 0);
    assert(result_disposition == NODE_TRANSACTION_RESULT_ACCEPTED);
    assert(node_transaction_reconcile_result(
               &transaction, &spec.key, digest_a, digest_b,
               7u, 11u, &result_disposition, &action) == 0);
    assert(result_disposition == NODE_TRANSACTION_RESULT_CONFLICT);

    node_transaction_responder_init(&responder);
    assert(node_transaction_responder_receive(
               &responder, &responder_key, digest_a, 1000u, 0u,
               &receive_disposition, NULL, NULL) == 0);
    assert(receive_disposition == NODE_TRANSACTION_RECEIVE_EXECUTE);
    assert(node_transaction_responder_receive(
               &responder, &responder_key, digest_b, 1000u, 1u,
               &receive_disposition, NULL, NULL) == 0);
    assert(receive_disposition == NODE_TRANSACTION_RECEIVE_CONFLICT);
    assert(node_transaction_responder_commit(
               &responder, &responder_key, digest_a, digest_a, 9u) == 0);
    assert(node_transaction_responder_commit(
               &responder, &responder_key, digest_a, digest_b, 9u) ==
           -EEXIST);
}

static void assert_responder_invariants(
    const struct node_transaction_responder *responder,
    uint64_t now_ms)
{
    for (size_t i = 0u; i < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; i++) {
        const struct node_transaction_response_record *record =
            &responder->records[i];

        if (record->state == NODE_TRANSACTION_RESPONSE_EMPTY) {
            continue;
        }
        assert(record->state >= NODE_TRANSACTION_RESPONSE_EXECUTING);
        assert(record->state <= NODE_TRANSACTION_RESPONSE_ABANDONED);
        assert(record->expires_at_ms > now_ms);
        for (size_t j = i + 1u;
             j < NODE_TRANSACTION_RESPONDER_MAX_RECORDS; j++) {
            if (responder->records[j].state !=
                NODE_TRANSACTION_RESPONSE_EMPTY) {
                assert(!node_transaction_key_equal(
                    &record->key, &responder->records[j].key));
            }
        }
    }
}

static void test_randomized_transaction_properties(void)
{
    struct node_transaction_responder responder;
    uint32_t random_state = UINT32_C(0x4e4f4445);
    uint64_t responder_now = 0u;

    node_transaction_responder_init(&responder);
    for (uint32_t iteration = 0u; iteration < PROPERTY_ITERATIONS; iteration++) {
        struct node_transaction transaction;
        struct node_transaction_spec spec;
        struct node_transaction_key original_key;
        enum node_transaction_result_disposition result_disposition;
        enum node_transaction_receive_disposition receive_disposition;
        enum node_transaction_action action;
        uint64_t now_ms = test_random(&random_state) % 100000u;
        uint64_t deadline_ms = now_ms + 1u +
                               (test_random(&random_state) % 1000u);
        uint16_t transaction_id =
            (uint16_t)(1u + (iteration % UINT16_MAX));
        uint32_t choice = test_random(&random_state) % 5u;
        bool cleanup_required = (test_random(&random_state) & 1u) != 0u;

        node_transaction_init(&transaction);
        spec = test_spec(transaction_id, deadline_ms, cleanup_required);
        original_key = spec.key;
        assert(node_transaction_begin(
                   &transaction, &spec, iteration + 1u, now_ms) == 0);

        if (choice == 0u) {
            assert(reconcile_result_u32(
                       &transaction, &spec.key, spec.request_digest,
                       iteration, choice, deadline_ms - 1u,
                       &result_disposition, &action) == 0);
            assert(result_disposition == NODE_TRANSACTION_RESULT_ACCEPTED);
            assert(transaction.state == NODE_TRANSACTION_SUCCEEDED);
        } else if (choice == 1u) {
            assert(reconcile_result_u32(
                       &transaction, &spec.key, spec.request_digest,
                       iteration, choice, deadline_ms,
                       &result_disposition, &action) == 0);
            assert(result_disposition ==
                   NODE_TRANSACTION_RESULT_LATE_AFTER_ABANDON);
        } else if (choice == 2u) {
            struct node_comm_terminal_event event = terminal_event(
                &transaction, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
                (uint8_t)(test_random(&random_state) % 5u));

            assert(node_transaction_note_request_terminal(
                       &transaction, &event, now_ms, &action) == 0);
        } else if (choice == 3u) {
            struct node_transaction_key wrong_key = spec.key;

            wrong_key.operation_id++;
            assert(reconcile_result_u32(
                       &transaction, &wrong_key, spec.request_digest,
                       iteration, choice, deadline_ms - 1u,
                       &result_disposition, &action) == 0);
            assert(result_disposition == NODE_TRANSACTION_RESULT_STALE);
            assert(transaction.state == NODE_TRANSACTION_ACTIVE);
            assert(node_transaction_service(
                       &transaction, deadline_ms, &action));
        } else {
            assert(node_transaction_cancel(&transaction, now_ms, &action) == 0);
        }

        assert_key_equal(&transaction.spec.key, &original_key);
        if (transaction.state == NODE_TRANSACTION_ABANDONING) {
            assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
            assert(node_transaction_cleanup_complete(
                       &transaction, &spec.key, deadline_ms, &action) == 0);
        }
        assert(node_transaction_terminal(&transaction));
        assert(node_transaction_retire(&transaction) == 0);

        responder_now += test_random(&random_state) % 3u;
        (void)node_transaction_responder_service(&responder, responder_now);
        {
            struct node_transaction_key key = test_key(
                (uint16_t)(1u + (test_random(&random_state) % 32u)));
            uint64_t expiry = responder_now + 1u +
                              (test_random(&random_state) % 20u);
            uint32_t request_value = test_random(&random_state);

            assert(responder_receive_u32(
                       &responder, &key, request_value, expiry, responder_now,
                       &receive_disposition, NULL, NULL) == 0);
            if (receive_disposition == NODE_TRANSACTION_RECEIVE_EXECUTE) {
                if ((test_random(&random_state) & 1u) != 0u) {
                    assert(responder_commit_u32(
                               &responder, &key, request_value,
                               request_value ^ UINT32_C(0x5a5a5a5a),
                               request_value) == 0);
                } else {
                    assert(node_transaction_responder_abandon(
                               &responder, &key, expiry, responder_now) == 0);
                }
            }
        }
        assert_responder_invariants(&responder, responder_now);
    }
}

int main(void)
{
    test_digest_is_deterministic();
    test_packet_digest_matches_canonical_encoding();
    test_begin_requires_stable_valid_identity_and_future_deadline();
    test_matching_result_survives_transport_retries();
    test_exact_deadline_rejects_result_and_requires_cleanup();
    test_duplicate_conflict_and_stale_results_are_inert();
    test_terminal_delivery_failure_uses_attempt_evidence();
    test_late_zero_attempt_proof_retires_conservative_cleanup();
    test_terminal_evidence_cannot_downgrade_known_rf();
    test_invalid_terminal_reason_is_failure_atomic();
    test_explicit_cancel_is_idempotent_while_cleaning();
    test_responder_executes_coalesces_commits_and_replays();
    test_responder_abandon_tombstone_blocks_reordered_request();
    test_responder_capacity_releases_at_exact_expiry();
    test_responder_deadlines_do_not_wrap_near_u64_max();
    test_deliberate_fnv_collision_is_conflict_everywhere();
    test_randomized_transaction_properties();
    return 0;
}
