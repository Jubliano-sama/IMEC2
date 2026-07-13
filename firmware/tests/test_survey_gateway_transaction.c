#include "survey_gateway_transaction.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define INITIATOR_ID UINT64_C(0x1111222233334444)
#define RESPONDER_ID UINT64_C(0x5555666677778888)
#define TEST_SCHEDULER_SLOT_COUNT 2u

struct test_scheduler {
    uint32_t handles[TEST_SCHEDULER_SLOT_COUNT];
    size_t max_occupied;
};

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

static void begin_phase(struct survey_gateway_transaction *context,
                        enum command_id command_id,
                        uint64_t target_id,
                        uint16_t sequence,
                        uint32_t request_fingerprint,
                        uint32_t delivery_handle,
                        uint64_t deadline_ms)
{
    struct node_transaction_key key =
        test_key(command_id, target_id, sequence);

    assert(survey_gateway_transaction_begin(context,
                                            &key,
                                            command_id,
                                            request_fingerprint,
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
    uint32_t request_fingerprint,
    uint32_t result_fingerprint,
    uint32_t result_token,
    enum command_status status,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    struct node_transaction_key key =
        test_key(command_id, target_id, sequence);
    enum survey_gateway_transaction_result result;

    assert(survey_gateway_transaction_reconcile_result(
               context, &key, request_fingerprint, result_fingerprint,
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
    enum survey_gateway_transaction_result result;
    enum node_transaction_action action;

    survey_gateway_transaction_init(&context);
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    assert(survey_gateway_transaction_begin(&context, &key,
                                            CMD_SURVEY_PREPARE_PAIR,
                                            501u, 601u, 21u,
                                            1000u, 10u) == 0);
    assert(survey_gateway_transaction_service(&context, 1000u, &action));
    assert(action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED);
    assert(context.abandoning);
    assert(survey_gateway_transaction_reconcile_result(
               &context, &key, 501u, 701u, 31u, COMMAND_OK, 1001u,
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
               202u) == 0);
    assert(context.active.state == NODE_TRANSACTION_EMPTY);
    pair.survey_id = 78u;
    assert(survey_gateway_transaction_load_pair(&context, &pair) == 0);
    {
        struct node_transaction_key next =
            test_key(CMD_SURVEY_PREPARE_PAIR, INITIATOR_ID, 53u);

        next.session_id = 78u;
        assert(survey_gateway_transaction_begin(
                   &context, &next, CMD_SURVEY_PREPARE_PAIR,
                   903u, 153u, 53u, 6000u, 203u) == 0);
    }
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

int main(void)
{
    test_four_phase_success_tracks_prepared_peers();
    test_prepare_failure_cleans_every_possible_peer();
    test_deadline_late_ok_cannot_advance();
    test_zero_rf_delivery_failure_needs_no_abort();
    test_duplicate_and_conflicting_history_fail_closed();
    test_incomplete_pair_cleans_both_started_peers();
    test_early_results_release_exact_handle_with_two_slots();
    test_consecutive_early_abandons_do_not_leak_two_slots();
    test_conflict_and_deadline_abandonment_free_scheduler_slot();
    puts("survey gateway transaction tests passed");
    return 0;
}
