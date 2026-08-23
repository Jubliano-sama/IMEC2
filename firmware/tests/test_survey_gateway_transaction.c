#include "survey_gateway_transaction.h"
#include "gateway_command.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define INITIATOR_ID UINT64_C(0x1111222233334444)
#define RESPONDER_ID UINT64_C(0x5555666677778888)
#define TEST_SCHEDULER_SLOT_COUNT 2u

struct test_scheduler {
    uint32_t handles[TEST_SCHEDULER_SLOT_COUNT];
    size_t max_occupied;
};

struct test_due_arm {
    uint32_t delays[12];
    size_t call_count;
};

static int due_arm(void *context, uint32_t delay_ms)
{
    struct test_due_arm *arm = context;

    assert(arm != NULL);
    assert(arm->call_count < sizeof(arm->delays) / sizeof(arm->delays[0]));
    arm->delays[arm->call_count++] = delay_ms;
    return 0;
}

static size_t scheduler_occupied(const struct test_scheduler *scheduler)
{
    size_t occupied = 0u;

    for (size_t i = 0u; i < TEST_SCHEDULER_SLOT_COUNT; i++) {
        if (scheduler->handles[i] != 0u) {
            occupied++;
        }
    }
    return occupied;
}

static bool scheduler_submit(struct test_scheduler *scheduler,
                             uint32_t handle)
{
    for (size_t i = 0u; i < TEST_SCHEDULER_SLOT_COUNT; i++) {
        if (scheduler->handles[i] == 0u) {
            scheduler->handles[i] = handle;
            size_t occupied = scheduler_occupied(scheduler);

            if (occupied > scheduler->max_occupied) {
                scheduler->max_occupied = occupied;
            }
            return true;
        }
    }
    return false;
}

static bool scheduler_cancel_take(struct test_scheduler *scheduler,
                                  uint32_t handle,
                                  struct node_comm_terminal_event *event)
{
    for (size_t i = 0u; i < TEST_SCHEDULER_SLOT_COUNT; i++) {
        if (scheduler->handles[i] == handle) {
            scheduler->handles[i] = 0u;
            *event = (struct node_comm_terminal_event) {
                .handle = handle,
                .client_token = handle + 100u,
                .reason = NODE_COMM_TERMINAL_CANCELLED,
                .attempts_started = 1u,
            };
            return true;
        }
    }
    return false;
}

static struct survey_pair test_pair(void)
{
    return (struct survey_pair) {
        .survey_id = 77u,
        .initiator_id = INITIATOR_ID,
        .responder_id = RESPONDER_ID,
        .sample_count = 4u,
    };
}

static struct node_transaction_key test_key(enum command_id command_id,
                                            uint64_t target_id,
                                            uint16_t sequence)
{
    return (struct node_transaction_key) {
        .requester_id = GATEWAY_ID,
        .responder_id = target_id,
        .session_id = 77u,
        .transaction_id = sequence,
        .operation_id = (uint16_t)command_id,
    };
}

static void test_digest_u32(
    uint32_t value,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    uint8_t encoded[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8u),
        (uint8_t)(value >> 16u),
        (uint8_t)(value >> 24u),
    };

    assert(node_transaction_digest_bytes(encoded, sizeof(encoded), digest));
}

static void begin_phase(struct survey_gateway_transaction *context,
                        enum command_id command_id,
                        uint64_t target_id,
                        uint16_t sequence,
                        uint32_t request_value,
                        uint32_t delivery_handle,
                        uint64_t deadline_ms)
{
    struct node_transaction_key key =
        test_key(command_id, target_id, sequence);
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];

    test_digest_u32(request_value, request_digest);
    assert(survey_gateway_transaction_begin(context,
                                            &key,
                                            command_id,
                                            request_digest,
                                            delivery_handle + 100u,
                                            delivery_handle,
                                            deadline_ms,
                                            10u) == 0);
}

static enum survey_gateway_transaction_result note_result(
    struct survey_gateway_transaction *context,
    enum command_id command_id,
    uint64_t target_id,
    uint16_t sequence,
    uint32_t request_value,
    uint32_t result_value,
    uint32_t result_token,
    enum command_status status,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    struct node_transaction_key key =
        test_key(command_id, target_id, sequence);
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result result;

    test_digest_u32(request_value, request_digest);
    test_digest_u32(result_value, result_digest);
    assert(survey_gateway_transaction_reconcile_result(
               context, &key, request_digest, result_digest,
               result_token, status, now_ms, &result, action) == 0);
    return result;
}

static void complete_delivery(struct survey_gateway_transaction *context,
                              uint32_t delivery_handle,
                              uint8_t attempts_started)
{
    struct node_comm_terminal_event event = {
        .handle = delivery_handle,
        .client_token = delivery_handle + 100u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = attempts_started,
    };
    enum node_transaction_action action;

    assert(survey_gateway_transaction_note_delivery_terminal(
               context, &event, 100u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
}

static void complete_abandon_delivery(
    struct survey_gateway_transaction *context,
    uint32_t delivery_handle,
    uint8_t attempts_started)
{
    struct node_comm_terminal_event event = {
        .handle = delivery_handle,
        .client_token = delivery_handle + 100u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = attempts_started,
    };
    enum node_transaction_action action;

    assert(survey_gateway_transaction_note_delivery_terminal(
               context, &event, 100u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
}

static void test_four_phase_success_tracks_prepared_peers(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                11u, 101u, 1u, 1000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       11u, 101u, 201u, 1u, COMMAND_OK, 100u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(context.prepared_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
    assert(survey_gateway_transaction_phase_complete(&context) == -EBUSY);
    complete_delivery(&context, 1u, 1u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                12u, 102u, 2u, 1100u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                       12u, 102u, 202u, 2u, COMMAND_OK, 200u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(context.prepared_mask ==
           (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK));
    complete_delivery(&context, 2u, 2u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);

    begin_phase(&context, CMD_SURVEY_START_PAIR, RESPONDER_ID,
                13u, 103u, 3u, 1200u);
    assert(note_result(&context, CMD_SURVEY_START_PAIR, RESPONDER_ID,
                       13u, 103u, 203u, 3u, COMMAND_OK, 300u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    complete_delivery(&context, 3u, 4u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);

    begin_phase(&context, CMD_SURVEY_START_PAIR, INITIATOR_ID,
                14u, 104u, 4u, 1300u);
    assert(note_result(&context, CMD_SURVEY_START_PAIR, INITIATOR_ID,
                       14u, 104u, 204u, 4u, COMMAND_OK, 400u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    complete_delivery(&context, 4u, 1u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);
    survey_gateway_transaction_pair_complete(&context, true, 401u);
    assert(!context.pair_loaded);
    assert(context.prepared_mask == 0u);
    assert(context.cleanup_mask == 0u);
}

static void test_prepare_failure_cleans_every_possible_peer(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                21u, 301u, 11u, 1000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       21u, 301u, 401u, 11u, COMMAND_OK, 100u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    complete_delivery(&context, 11u, 1u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                22u, 302u, 12u, 1100u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                       22u, 302u, 402u, 12u, COMMAND_BUSY, 200u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(survey_gateway_transaction_cleanup_mask(&context) ==
           (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK));

    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK,
               300u) == -EINPROGRESS);
    assert(survey_gateway_transaction_cleanup_mask(&context) ==
           (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK));
    complete_delivery(&context, 12u, 1u);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK,
               300u) == 0);
    assert(context.abandoning);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK,
               301u) == 0);
    assert(!context.abandoning);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
}

static void test_deadline_late_ok_cannot_advance(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct node_transaction_key key =
        test_key(CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID, 31u);
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result result;
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    test_digest_u32(501u, request_digest);
    assert(survey_gateway_transaction_begin(&context, &key,
                                            CMD_SURVEY_PREPARE_PAIR,
                                            request_digest, 601u, 21u,
                                            1000u, 10u) == 0);
    assert(survey_gateway_transaction_service(&context, 1000u, &action));
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(context.abandoning);
    test_digest_u32(701u, result_digest);
    assert(survey_gateway_transaction_reconcile_result(
               &context, &key, request_digest, result_digest,
               31u, COMMAND_OK, 1001u,
               &result, &action) == 0);
    assert(result == SURVEY_GATEWAY_TRANSACTION_RESULT_LATE);
    assert(context.prepared_mask == 0u);
    assert(context.cleanup_mask ==
           SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
}

static void test_zero_rf_delivery_failure_needs_no_abort(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct node_comm_terminal_event event = {
        .handle = 41u,
        .client_token = 141u,
        .reason = NODE_COMM_TERMINAL_PERMANENT_FAILURE,
        .attempts_started = 0u,
    };
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                41u, 801u, 41u, 1000u);
    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &event, 100u, &action) == 0);
    assert(context.cleanup_mask == 0u);
    assert(context.abandoning);
    assert(context.active.state == NODE_TRANSACTION_ABANDONED);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, 0u, 101u) == 0);
    assert(!context.abandoning);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
}

static void test_deadline_then_zero_rf_terminal_retires_possible_prepare(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct survey_pair next_pair = test_pair();
    struct node_comm_terminal_event event = {
        .handle = 42u,
        .client_token = 142u,
        .reason = NODE_COMM_TERMINAL_PERMANENT_FAILURE,
        .attempts_started = 0u,
    };
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                42u, 802u, 42u, 1000u);
    assert(survey_gateway_transaction_service(&context, 1000u, &action));
    assert(context.active.state == NODE_TRANSACTION_ABANDONING);
    assert(context.cleanup_mask ==
           SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
    assert(context.possible_prepare_mask ==
           SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);

    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &event, 1001u, &action) == 0);
    assert(context.active.state == NODE_TRANSACTION_ABANDONED);
    assert(!context.active.remote_side_effect_possible);
    assert(context.possible_prepare_mask == 0u);
    assert(context.cleanup_mask == 0u);
    assert(context.pair_loaded);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, 0u, 1002u) == 0);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
    assert(!context.abandoning);
    assert(context.pair_loaded);

    /*
     * Empty cleanup debt retires the request, not its pair identity. The
     * orchestration owner may release that identity only after it observes
     * the transaction's EMPTY state, then the next pair is admissible.
     */
    survey_gateway_transaction_pair_complete(&context, true, 1003u);
    assert(!context.pair_loaded);
    next_pair.survey_id++;
    assert(survey_gateway_transaction_load_pair(&context, &next_pair) == 0);
}

static void test_duplicate_and_conflicting_history_fail_closed(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                51u, 901u, 51u, 5000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       51u, 901u, 1001u, 61u, COMMAND_OK, 100u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    complete_delivery(&context, 51u, 1u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                52u, 902u, 52u, 5000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       51u, 901u, 1001u, 61u, COMMAND_OK, 200u,
                       &action) == SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE);
    assert(!context.abandoning);

    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       51u, 901u, 1002u, 61u, COMMAND_OK, 201u,
                       &action) == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT);
    assert(context.abandoning);
    assert(context.conflict);
    assert(context.cleanup_mask ==
           (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK));
    assert(context.active.state == NODE_TRANSACTION_ABANDONING);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context,
               SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
               SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK,
               202u) == -EINPROGRESS);
    complete_abandon_delivery(&context, 52u, 1u);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context,
               SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
               SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK,
               202u) == 0);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
    survey_gateway_transaction_pair_complete(&context, true, 202u);
    assert(!context.pair_loaded);
    pair.survey_id = 78u;
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    {
        struct node_transaction_key next =
            test_key(CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID, 53u);
        uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];

        next.session_id = 78u;
        test_digest_u32(903u, request_digest);
        assert(survey_gateway_transaction_begin(
                   &context, &next, CMD_SURVEY_PREPARE_PAIR,
                   request_digest, 153u, 53u, 6000u, 203u) == 0);
    }
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

static void test_recent_history_rejects_deliberate_fnv_result_collision(void)
{
    static const uint8_t colliding_a[] = {
        0xb1u, 0xd7u, 0x7au, 0x70u, 0x16u, 0xc0u,
        0x1au, 0xacu, 0xacu, 0x64u, 0x24u, 0x8du,
    };
    static const uint8_t colliding_b[] = {
        0x2eu, 0x00u, 0x72u, 0xf2u, 0xfcu, 0x53u,
        0x58u, 0x7fu, 0x6du, 0xf1u, 0x12u, 0xa9u,
    };
    struct survey_gateway_transaction context;
    struct survey_gateway_transaction request_context;
    struct survey_pair pair = test_pair();
    struct node_transaction_key old_key =
        test_key(CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID, 54u);
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t recovered_request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest_a[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest_b[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result result;
    enum node_transaction_action action;

    assert(legacy_fnv1a32(colliding_a, sizeof(colliding_a)) ==
           UINT32_C(0x1cbf45d8));
    assert(legacy_fnv1a32(colliding_b, sizeof(colliding_b)) ==
           UINT32_C(0x1cbf45d8));
    test_digest_u32(904u, request_digest);
    assert(node_transaction_digest_bytes(
        colliding_a, sizeof(colliding_a), result_digest_a));
    assert(node_transaction_digest_bytes(
        colliding_b, sizeof(colliding_b), result_digest_b));
    assert(!semantic_digest_equal(result_digest_a,
                                  result_digest_b,
                                  sizeof(result_digest_a)));

    /*
     * The same retired-FNV collision in request position must be a conflict
     * before the active survey transaction can advance.
     */
    survey_gateway_transaction_init(&request_context);
    assert(survey_gateway_transaction_load_pair(&request_context, &pair) == 0);
    assert(survey_gateway_transaction_begin(
               &request_context, &old_key, CMD_SURVEY_PREPARE_PAIR,
               result_digest_a, 154u, 54u, 5000u, 10u) == 0);
    assert(survey_gateway_transaction_reconcile_result(
               &request_context, &old_key, result_digest_b, result_digest_a,
               71u, COMMAND_OK, 100u, &result, &action) == 0);
    assert(result == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT);
    assert(request_context.conflict);

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    assert(survey_gateway_transaction_begin(
               &context, &old_key, CMD_SURVEY_PREPARE_PAIR,
               request_digest, 154u, 54u, 5000u, 10u) == 0);
    assert(survey_gateway_transaction_reconcile_result(
               &context, &old_key, request_digest, result_digest_a,
               71u, COMMAND_OK, 100u, &result, &action) == 0);
    assert(result == SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    complete_delivery(&context, 54u, 1u);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);
    assert(survey_gateway_transaction_request_digest(
        &context, &old_key, recovered_request_digest));
    assert(semantic_digest_equal(request_digest,
                                 recovered_request_digest,
                                 sizeof(request_digest)));

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                55u, 905u, 55u, 5000u);
    assert(survey_gateway_transaction_reconcile_result(
               &context, &old_key, request_digest, result_digest_b,
               71u, COMMAND_OK, 200u, &result, &action) == 0);
    assert(result == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT);
    assert(context.conflict);
    assert(context.abandoning);
}

static void test_incomplete_pair_cleans_both_started_peers(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    context.prepared_mask = SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
                            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    survey_gateway_transaction_pair_complete(&context, false, 100u);
    assert(context.abandoning);
    assert(context.cleanup_mask ==
           (SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK));
}

static void test_early_results_release_exact_handle_with_two_slots(void)
{
    static const enum command_id commands[] = {
        CMD_SURVEY_PREPARE_PAIR,
        CMD_SURVEY_PREPARE_PAIR,
        CMD_SURVEY_START_PAIR,
        CMD_SURVEY_START_PAIR,
    };
    static const uint64_t targets[] = {
        INITIATOR_ID,
        RESPONDER_ID,
        RESPONDER_ID,
        INITIATOR_ID,
    };
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct test_scheduler scheduler = {
        /* An unrelated request permanently occupies half the capacity. */
        .handles = { UINT32_C(0xffff0001), 0u },
        .max_occupied = 1u,
    };
    enum node_transaction_action action;
    size_t completed = 0u;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    for (size_t i = 0u; i < 4u; i++) {
        uint32_t handle = (uint32_t)(70u + i);
        uint16_t sequence = (uint16_t)(170u + i);
        uint32_t fingerprint = (uint32_t)(270u + i);
        struct node_comm_terminal_event event;

        assert(scheduler_submit(&scheduler, handle));
        begin_phase(&context, commands[i], targets[i], sequence,
                    fingerprint, handle, 5000u);
        assert(note_result(&context, commands[i], targets[i], sequence,
                           fingerprint, (uint32_t)(370u + i),
                           (uint32_t)(470u + i), COMMAND_OK,
                           (uint64_t)(100u + i), &action) ==
               SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
        assert(survey_gateway_transaction_phase_complete(&context) == -EBUSY);
        assert(scheduler_cancel_take(&scheduler, handle, &event));
        assert(survey_gateway_transaction_note_delivery_terminal(
                   &context, &event, (uint64_t)(200u + i), &action) == 0);
        assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
        assert(survey_gateway_transaction_phase_complete(&context) == 0);
        completed++;
    }
    assert(completed == 4u);
    assert(scheduler.max_occupied == TEST_SCHEDULER_SLOT_COUNT);
    assert(scheduler_occupied(&scheduler) == 1u);
    survey_gateway_transaction_pair_complete(&context, true, 300u);
    assert(!context.pair_loaded);
}

static void test_consecutive_early_abandons_do_not_leak_two_slots(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct test_scheduler scheduler = {
        .handles = { UINT32_C(0xffff0001), 0u },
        .max_occupied = 1u,
    };
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    for (size_t cycle = 0u; cycle < 2u; cycle++) {
        uint32_t handle = (uint32_t)(90u + cycle);
        struct node_comm_terminal_event event;
        uint8_t cleanup_mask;

        assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
        assert(scheduler_submit(&scheduler, handle));
        begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                    (uint16_t)(190u + cycle),
                    (uint32_t)(290u + cycle), handle, 5000u);
        assert(scheduler_cancel_take(&scheduler, handle, &event));
        assert(survey_gateway_transaction_note_delivery_terminal(
                   &context, &event, (uint64_t)(100u + cycle), &action) == 0);
        assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
        assert(survey_gateway_transaction_cleanup_pending(&context));
        survey_gateway_transaction_require_cleanup(
            &context, false, (uint64_t)(110u + cycle));
        cleanup_mask = survey_gateway_transaction_cleanup_mask(&context);
        assert(cleanup_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
        assert(survey_gateway_transaction_note_cleanup_complete(
                   &context, cleanup_mask,
                   (uint64_t)(120u + cycle)) == 0);
        survey_gateway_transaction_pair_complete(
            &context, true, (uint64_t)(130u + cycle));
        assert(!survey_gateway_transaction_cleanup_pending(&context));
        assert(scheduler_occupied(&scheduler) == 1u);
        assert(context.active.state == NODE_TRANSACTION_EMPTY);
    }
    assert(scheduler.max_occupied == TEST_SCHEDULER_SLOT_COUNT);
}

static void test_conflict_and_deadline_abandonment_free_scheduler_slot(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct test_scheduler scheduler = {
        .handles = { UINT32_C(0xffff0001), 0u },
        .max_occupied = 1u,
    };
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    for (size_t cycle = 0u; cycle < 2u; cycle++) {
        uint32_t handle = (uint32_t)(110u + cycle);
        uint16_t sequence = (uint16_t)(210u + cycle);
        uint32_t fingerprint = (uint32_t)(310u + cycle);
        struct node_comm_terminal_event event;
        uint8_t cleanup_mask;

        assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
        assert(scheduler_submit(&scheduler, handle));
        begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                    sequence, fingerprint, handle, 1000u);
        if (cycle == 0u) {
            assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR,
                               INITIATOR_ID, sequence,
                               fingerprint + 1u, 410u, 510u,
                               COMMAND_OK, 100u, &action) ==
                   SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT);
        } else {
            assert(survey_gateway_transaction_service(
                &context, 1000u, &action));
        }
        assert(context.active.state == NODE_TRANSACTION_ABANDONING);
        assert(!context.active.request_delivery_terminal);
        assert(scheduler_cancel_take(&scheduler, handle, &event));
        assert(survey_gateway_transaction_note_delivery_terminal(
                   &context, &event, (uint64_t)(1010u + cycle),
                   &action) == 0);
        assert(context.active.request_delivery_terminal);
        assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
        cleanup_mask = survey_gateway_transaction_cleanup_mask(&context);
        assert(cleanup_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
        assert(survey_gateway_transaction_note_cleanup_complete(
                   &context, cleanup_mask,
                   (uint64_t)(1020u + cycle)) == 0);
        survey_gateway_transaction_pair_complete(
            &context, true, (uint64_t)(1030u + cycle));
        assert(scheduler_occupied(&scheduler) == 1u);
    }
    assert(scheduler.max_occupied == TEST_SCHEDULER_SLOT_COUNT);
}

static void test_cleanup_cannot_retire_before_request_delivery_terminal(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct node_comm_terminal_event event = {
        .handle = 130u,
        .client_token = 230u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = 1u,
    };
    enum node_transaction_action action;
    uint8_t cleanup_mask;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                230u, 330u, 130u, 5000u);

    survey_gateway_transaction_require_cleanup(&context, false, 100u);
    cleanup_mask = survey_gateway_transaction_cleanup_mask(&context);
    assert(cleanup_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
    assert(context.active.state == NODE_TRANSACTION_ABANDONING);
    assert(!context.active.request_delivery_terminal);

    /* Cleanup RF can finish while cancellation is still inside the backend. */
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, cleanup_mask, 101u) == -EINPROGRESS);
    assert(survey_gateway_transaction_cleanup_mask(&context) == cleanup_mask);
    assert(context.active.state == NODE_TRANSACTION_ABANDONING);
    assert(context.abandoning);

    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &event, 102u, &action) == 0);
    assert(context.active.request_delivery_terminal);
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, cleanup_mask, 103u) == 0);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
    assert(!context.abandoning);
    assert(survey_gateway_transaction_cleanup_mask(&context) == 0u);
}

static void test_duplicate_cannot_replace_accepted_result_before_terminal(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    uint8_t expected_result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                240u, 340u, 140u, 5000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       240u, 340u, 440u, 540u, COMMAND_OK, 100u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(context.active.state == NODE_TRANSACTION_SUCCEEDED);
    assert(!context.active.request_delivery_terminal);
    test_digest_u32(440u, expected_result_digest);
    assert(semantic_digest_equal(context.active.accepted_result_digest,
                                 expected_result_digest,
                                 sizeof(expected_result_digest)));
    assert(context.active.result_token == 540u);

    /* The retry can arrive while cancel/take is still inside the backend. */
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       240u, 340u, 440u, 540u, COMMAND_OK, 101u,
                       &action) == SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE);
    assert(context.active.state == NODE_TRANSACTION_SUCCEEDED);
    assert(!context.active.request_delivery_terminal);
    assert(semantic_digest_equal(context.active.accepted_result_digest,
                                 expected_result_digest,
                                 sizeof(expected_result_digest)));
    assert(context.active.result_token == 540u);
    assert(context.prepared_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
}

static void test_close_intent_survives_cancel_take_poll_and_blocks_redrive(void)
{
    struct survey_gateway_transaction context;
    struct survey_gateway_transaction before_redrive;
    struct survey_pair pair = test_pair();
    struct node_comm_terminal_event delivered = {
        .handle = 180u,
        .client_token = 280u,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };
    struct node_comm_terminal_event cancelled = {
        .handle = 180u,
        .client_token = 280u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = 1u,
    };
    enum node_transaction_action action =
        NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS;
    uint8_t cleanup_mask;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                280u, 380u, 180u, 5000u);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE);
    assert(survey_gateway_transaction_request_close(
               &context, SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT) == 0);

    /*
     * cancel/take may report -EAGAIN for several worker polls.  No terminal
     * evidence has crossed this boundary, so the close request must remain
     * the sole disposition owner and an ordinary delivery redrive is stale.
     */
    before_redrive = context;
    assert(survey_gateway_transaction_note_delivery_redrive(
               &context, &delivered, 100u, &action) == -ECANCELED);
    assert(memcmp(&context, &before_redrive, sizeof(context)) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
    assert(survey_gateway_transaction_request_close(
               &context, SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT) == 0);
    assert(survey_gateway_transaction_request_close(
               &context, SURVEY_GATEWAY_TRANSACTION_CLOSE_RADIO) ==
           -EALREADY);
    assert(!survey_gateway_transaction_service(&context, 101u, &action));
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT);
    assert(!context.active.request_delivery_terminal);

    /* Exact cancel/take evidence is required before the owner may clear it. */
    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &cancelled, 102u, &action) == 0);
    assert(context.active.request_delivery_terminal);
    assert(context.active.state == NODE_TRANSACTION_ABANDONING);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_TIMEOUT);
    cleanup_mask = survey_gateway_transaction_cleanup_mask(&context);
    assert(cleanup_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);

    survey_gateway_transaction_clear_close_request(&context);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context, cleanup_mask, 103u) == 0);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
}

static void test_close_intent_clears_on_exact_phase_completion_or_reset(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    struct node_comm_terminal_event terminal = {
        .handle = 181u,
        .client_token = 281u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = 1u,
    };
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                281u, 381u, 181u, 5000u);
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       281u, 381u, 481u, 581u, COMMAND_OK, 100u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(survey_gateway_transaction_request_close(
               &context, SURVEY_GATEWAY_TRANSACTION_CLOSE_INTERNAL) == 0);
    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &terminal, 101u, &action) == 0);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_INTERNAL);
    assert(survey_gateway_transaction_phase_complete(&context) == 0);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE);

    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, RESPONDER_ID,
                282u, 382u, 182u, 5000u);
    assert(survey_gateway_transaction_request_close(
               &context, SURVEY_GATEWAY_TRANSACTION_CLOSE_RADIO) == 0);
    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_close_requested(&context) ==
           SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
    assert(!context.pair_loaded);
}

static void test_cleanup_deadline_is_frozen_and_retirement_admits_next_pair(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    uint64_t cleanup_deadline_ms;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    context.prepared_mask =
        SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
        SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;

    survey_gateway_transaction_pair_complete(&context, false, 100u);
    cleanup_deadline_ms =
        survey_gateway_transaction_cleanup_deadline(&context);
    assert(cleanup_deadline_ms ==
           100u + SURVEY_GATEWAY_TRANSACTION_CLEANUP_TIMEOUT_MS);

    /* Route/admission retries cannot restart the cleanup horizon. */
    survey_gateway_transaction_require_cleanup(&context, true, 500u);
    assert(survey_gateway_transaction_cleanup_deadline(&context) ==
           cleanup_deadline_ms);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context,
               SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK,
               cleanup_deadline_ms) == 0);
    assert(survey_gateway_transaction_note_cleanup_complete(
               &context,
               SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK,
               cleanup_deadline_ms) == 0);
    assert(survey_gateway_transaction_cleanup_deadline(&context) == 0u);
    assert(!survey_gateway_transaction_cleanup_pending(&context));

    survey_gateway_transaction_pair_complete(
        &context, true, cleanup_deadline_ms);
    assert(!context.pair_loaded);
    pair.survey_id++;
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
}

static void test_remote_lease_expiry_is_terminal_cleanup_authority(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair pair = test_pair();
    uint64_t safe_release_ms =
        100u + SURVEY_PAIR_PREPARED_LEASE_MS;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    context.prepared_mask =
        SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
        SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    survey_gateway_transaction_pair_complete(&context, false, 100u);

    assert(survey_gateway_transaction_note_cleanup_lease_expired(
               &context,
               SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK,
               safe_release_ms) == 0);
    assert(survey_gateway_transaction_cleanup_mask(&context) ==
           SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK);
    assert(survey_gateway_transaction_note_cleanup_lease_expired(
               &context,
               SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK,
               safe_release_ms) == 0);
    assert(!survey_gateway_transaction_cleanup_pending(&context));
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
}

static void test_d_minus_one_result_decides_before_failed_delivery_terminal(void)
{
    struct survey_gateway_transaction context;
    struct gateway_command_result_validation_leases leases = {0};
    struct survey_pair pair = test_pair();
    struct node_comm_terminal_event terminal = {
        .handle = 160u,
        .client_token = 260u,
        .reason = NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
        .attempts_started = 3u,
    };
    enum node_transaction_action action;
    uint32_t token = 0u;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    begin_phase(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                260u, 360u, 160u, 1000u);

    /* Work sees the ready failure first, but D-1 RX custody blocks consume. */
    assert(gateway_command_result_validation_arm(
               &leases, 999u, 1200u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_check_interval(
               &leases, 10u, 1000u, 1000u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED);
    assert(context.active.state == NODE_TRANSACTION_ACTIVE);
    assert(!context.active.request_delivery_terminal);

    assert(gateway_command_result_validation_complete(
        &leases, token, 999u));
    assert(note_result(&context, CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID,
                       260u, 360u, 460u, 560u, COMMAND_OK, 999u,
                       &action) ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(gateway_command_result_validation_release(&leases, token));
    assert(gateway_command_result_validation_check_interval(
               &leases, 10u, 1000u, 1001u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR);

    assert(survey_gateway_transaction_note_delivery_terminal(
               &context, &terminal, 1001u, &action) == 0);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
    assert(context.active.state == NODE_TRANSACTION_SUCCEEDED);
    assert(context.prepared_mask ==
           SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
}

static void test_completed_validation_uses_full_processing_hold_across_wrap(void)
{
    struct gateway_command_result_validation_leases leases = {0};
    const uint32_t armed_at_ms = UINT32_MAX - 20u;
    const uint32_t initial_expires_at_ms = armed_at_ms + 30u;
    const uint32_t received_at_ms = initial_expires_at_ms - 1u;
    const uint32_t interval_deadline_ms = received_at_ms + 12u;
    const uint32_t completed_expires_at_ms =
        received_at_ms + GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS;
    uint32_t token = 0u;

    /* The RX lease is deliberately short and wraps through zero.  Once RF
     * completion transfers it to semantic processing, that old deadline must
     * not expire valid custody while a host receipt is still outstanding. */
    assert(gateway_command_result_validation_arm(
               &leases,
               armed_at_ms,
               initial_expires_at_ms,
               &token) == PROTO_OK);
    assert(gateway_command_result_validation_complete(
        &leases, token, received_at_ms));
    assert(gateway_command_result_validation_check_interval(
               &leases,
               armed_at_ms,
               interval_deadline_ms,
               initial_expires_at_ms) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED);
    assert(gateway_command_result_validation_check_interval(
               &leases,
               armed_at_ms,
               interval_deadline_ms,
               completed_expires_at_ms - 1u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED);
    assert(gateway_command_result_validation_check_interval(
               &leases,
               armed_at_ms,
               interval_deadline_ms,
               completed_expires_at_ms) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED);
}

static void test_pair_plan_default_and_explicit_maximum_budgets(void)
{
    assert(SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS == 12000u);
    assert(SURVEY_GATEWAY_TRANSACTION_CLEANUP_TIMEOUT_MS == 540000u);
    assert(SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS == 1800000u);

    /* The 30-minute safety boundary covers the complete 150-pair store once. */
    assert(survey_gateway_transaction_pair_plan_fits_minimum_budget(
        150u,
        SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS));
    assert(!survey_gateway_transaction_pair_plan_fits_minimum_budget(
        150u,
        SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS - 1u));

    /* The explicit one-hour command maximum retains the larger-fleet option. */
    assert(survey_gateway_transaction_pair_plan_fits_minimum_budget(
        300u,
        GATEWAY_COMMAND_BUDGET_MAX_MS));
    assert(!survey_gateway_transaction_pair_plan_fits_minimum_budget(
        301u,
        GATEWAY_COMMAND_BUDGET_MAX_MS));
}

static void test_due_registry_retains_earliest_observation_due(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = { 0 };
    uint32_t delay_ms;
    const uint32_t now_ms = 100u;

    survey_gateway_due_registry_init(&registry);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms, 2u, due_arm, &arm) == 0);
    assert(arm.call_count == 1u);
    assert(arm.delays[0] == 2u);

    /* A routine 50 ms poll must not replace the retained 2 ms observation. */
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
               now_ms, 50u, due_arm, &arm) == 0);
    assert(arm.call_count == 2u);
    assert(arm.delays[1] == 2u);
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 2u);

    survey_gateway_due_registry_cancel(
        &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION);
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 50u);
    assert(survey_gateway_due_registry_rearm(
               &registry, now_ms, due_arm, &arm) == 0);
    assert(arm.call_count == 3u);
    assert(arm.delays[2] == 50u);

    /* Consuming the re-added observation exposes the same later poll. */
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms, 2u, due_arm, &arm) == 0);
    survey_gateway_due_registry_consume_due(&registry, now_ms + 2u);
    assert(survey_gateway_due_registry_next(
        &registry, now_ms + 2u, &delay_ms));
    assert(delay_ms == 48u);
}

static void test_final_sample_immediate_due_preempts_old_observation(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = {0};
    uint32_t delay_ms;
    const uint32_t now_ms = 1000u;

    survey_gateway_due_registry_init(&registry);

    /* Model the live failure boundary: the observation owner already has its
     * long phase deadline and a second owner is also registered when the last
     * pair sample asks to drive the round immediately. */
    assert(survey_gateway_due_registry_schedule_after(
               &registry,
               SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms,
               261000u,
               due_arm,
               &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry,
               SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
               now_ms,
               1000u,
               due_arm,
               &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry,
               SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms,
               0u,
               due_arm,
               &arm) == 0);
    assert(arm.call_count == 3u);
    assert(arm.delays[0] == 261000u);
    assert(arm.delays[1] == 1000u);
    assert(arm.delays[2] == 0u);
    assert(survey_gateway_due_registry_next(
        &registry, now_ms, &delay_ms));
    assert(delay_ms == 0u);

    /* The immediate callback consumes only the final-sample owner. The later
     * boundary stays registered and is physically rearmed afterwards. */
    assert(survey_gateway_due_registry_consume_due(&registry, now_ms));
    assert(survey_gateway_due_registry_next(
        &registry, now_ms, &delay_ms));
    assert(delay_ms == 1000u);
    assert(survey_gateway_due_registry_rearm(
               &registry, now_ms, due_arm, &arm) == 0);
    assert(arm.call_count == 4u);
    assert(arm.delays[3] == 1000u);
}

static void test_due_registry_same_owner_only_moves_earlier(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = { 0 };
    uint32_t delay_ms;
    const uint32_t now_ms = 100u;

    survey_gateway_due_registry_init(&registry);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 2u, due_arm, &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 50u, due_arm, &arm) == 0);
    assert(arm.delays[0] == 2u);
    assert(arm.delays[1] == 2u);
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 2u);

    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 1u, due_arm, &arm) == 0);
    assert(arm.delays[2] == 1u);

    /* A due wake stays urgent until its owner explicitly consumes/cancels it. */
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms + 1u, 50u, due_arm, &arm) == 0);
    assert(arm.delays[3] == 0u);
    survey_gateway_due_registry_cancel(
        &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms + 1u, 50u, due_arm, &arm) == 0);
    assert(arm.delays[4] == 50u);
}

static void test_due_registry_stale_callback_cannot_consume_a_later_owner(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = {0};
    uint32_t delay_ms;
    const uint32_t now_ms = 100u;

    survey_gateway_due_registry_init(&registry);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms, 2u, due_arm, &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
               now_ms, 50u, due_arm, &arm) == 0);

    assert(!survey_gateway_due_registry_consume_due(&registry, now_ms));
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 2u);

    survey_gateway_due_registry_cancel(
        &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION);
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 50u);

    /* The already-armed 2 ms callback is now stale, not the 50 ms owner. */
    assert(!survey_gateway_due_registry_consume_due(&registry, now_ms + 2u));
    assert(survey_gateway_due_registry_next(
        &registry, now_ms + 2u, &delay_ms));
    assert(delay_ms == 48u);
    assert(!survey_gateway_due_registry_consume_due(&registry, now_ms + 49u));
    assert(survey_gateway_due_registry_next(
        &registry, now_ms + 49u, &delay_ms));
    assert(delay_ms == 1u);

    assert(survey_gateway_due_registry_consume_due(&registry, now_ms + 50u));
    assert(!survey_gateway_due_registry_next(
        &registry, now_ms + 50u, &delay_ms));
}

static void test_observation_origin_keeps_a_wrapped_zero_start(void)
{
    struct survey_gateway_observation_origin origin;

    survey_gateway_observation_origin_reset(&origin);
    assert(!origin.valid);
    assert(origin.started_at_ms == 0u);
    assert(survey_gateway_observation_origin_freeze(&origin, 0u));
    assert(origin.valid);
    assert(origin.started_at_ms == 0u);
    assert(!survey_gateway_observation_origin_freeze(&origin, 17u));
    assert(origin.valid);
    assert(origin.started_at_ms == 0u);
}

static void test_due_registry_keeps_order_across_uptime_wrap(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = { 0 };
    uint32_t delay_ms;
    const uint32_t now_ms = UINT32_MAX - 1u;

    survey_gateway_due_registry_init(&registry);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_ROUND_OBSERVATION,
               now_ms, 2u, due_arm, &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_BOUNDARY_POLL,
               now_ms, 50u, due_arm, &arm) == 0);
    assert(arm.delays[0] == 2u);
    assert(arm.delays[1] == 2u);

    survey_gateway_due_registry_consume_due(&registry, 0u);
    assert(survey_gateway_due_registry_next(&registry, 0u, &delay_ms));
    assert(delay_ms == 48u);
}

static void test_same_owner_due_and_receive_interval_cross_uptime_wrap(void)
{
    struct survey_gateway_due_registry registry;
    struct test_due_arm arm = { 0 };
    uint32_t delay_ms;
    const uint32_t now_ms = UINT32_MAX - 5u;
    const uint32_t deadline_ms = 3u;

    survey_gateway_due_registry_init(&registry);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 50u, due_arm, &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 2u, due_arm, &arm) == 0);
    assert(survey_gateway_due_registry_schedule_after(
               &registry, SURVEY_GATEWAY_DUE_CONTROL_DELIVERY,
               now_ms, 10u, due_arm, &arm) == 0);
    assert(arm.delays[0] == 50u);
    assert(arm.delays[1] == 2u);
    assert(arm.delays[2] == 2u);
    assert(survey_gateway_due_registry_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 2u);

    /* Closed-open [start, D): D-1 is accepted; D and D+1 are rejected. */
    assert(survey_gateway_receive_in_interval(
        (UINT64_C(1) << 32u) | (deadline_ms - 1u),
        now_ms, deadline_ms));
    assert(!survey_gateway_receive_in_interval(
        (UINT64_C(1) << 32u) | deadline_ms,
        now_ms, deadline_ms));
    assert(!survey_gateway_receive_in_interval(
        (UINT64_C(1) << 32u) | (deadline_ms + 1u),
        now_ms, deadline_ms));
    assert(!survey_gateway_receive_in_interval(
        now_ms - 1u, now_ms, deadline_ms));
}

static void test_loaded_pair_cannot_be_replaced_before_result_custody_retires(void)
{
    struct survey_gateway_transaction context;
    struct survey_pair first = test_pair();
    struct survey_pair replacement = first;

    replacement.initiator_id++;
    replacement.responder_id++;
    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &first) == 0);
    assert(context.pair_loaded);
    assert(survey_gateway_transaction_load_pair(&context, &replacement) ==
           -EBUSY);
    assert(context.pair.initiator_id == first.initiator_id);
    assert(context.pair.responder_id == first.responder_id);

    survey_gateway_transaction_pair_complete(&context, true, 100u);
    assert(!context.pair_loaded);
    assert(survey_gateway_transaction_load_pair(&context, &replacement) == 0);
}

int main(void)
{
    test_four_phase_success_tracks_prepared_peers();
    test_prepare_failure_cleans_every_possible_peer();
    test_deadline_late_ok_cannot_advance();
    test_zero_rf_delivery_failure_needs_no_abort();
    test_deadline_then_zero_rf_terminal_retires_possible_prepare();
    test_duplicate_and_conflicting_history_fail_closed();
    test_recent_history_rejects_deliberate_fnv_result_collision();
    test_incomplete_pair_cleans_both_started_peers();
    test_early_results_release_exact_handle_with_two_slots();
    test_consecutive_early_abandons_do_not_leak_two_slots();
    test_conflict_and_deadline_abandonment_free_scheduler_slot();
    test_cleanup_cannot_retire_before_request_delivery_terminal();
    test_duplicate_cannot_replace_accepted_result_before_terminal();
    test_close_intent_survives_cancel_take_poll_and_blocks_redrive();
    test_close_intent_clears_on_exact_phase_completion_or_reset();
    test_cleanup_deadline_is_frozen_and_retirement_admits_next_pair();
    test_remote_lease_expiry_is_terminal_cleanup_authority();
    test_d_minus_one_result_decides_before_failed_delivery_terminal();
    test_completed_validation_uses_full_processing_hold_across_wrap();
    test_pair_plan_default_and_explicit_maximum_budgets();
    test_due_registry_retains_earliest_observation_due();
    test_final_sample_immediate_due_preempts_old_observation();
    test_due_registry_same_owner_only_moves_earlier();
    test_due_registry_stale_callback_cannot_consume_a_later_owner();
    test_observation_origin_keeps_a_wrapped_zero_start();
    test_due_registry_keeps_order_across_uptime_wrap();
    test_same_owner_due_and_receive_interval_cross_uptime_wrap();
    test_loaded_pair_cannot_be_replaced_before_result_custody_retires();
    puts("survey gateway transaction tests passed");
    return 0;
}
