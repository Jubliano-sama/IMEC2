#include "app_gateway_survey_round.h"
#include "survey_pair_lease.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void context_init(struct survey_gateway_context *context,
                         size_t pair_count,
                         uint16_t sample_count)
{
    assert(pair_count <= SURVEY_GATEWAY_MAX_REPORTS / 2u);
    assert(survey_gateway_begin(
               context, UINT32_C(0x27182818), sample_count) == PROTO_OK);

    for (size_t i = 0u; i < pair_count; i++) {
        const uint64_t initiator_id = UINT64_C(0x1000) + 2u * i;
        const uint64_t responder_id = initiator_id + 1u;
        const uint8_t initiator_index = (uint8_t)(2u * i);
        const uint8_t responder_index = (uint8_t)(2u * i + 1u);
        const uint8_t reverse_hop_count =
            pair_count <= 4u ? (uint8_t)(1u + (2u * i)) : 1u;

        context->node_ids[initiator_index] = initiator_id;
        context->node_ids[responder_index] = responder_id;
        context->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_index = initiator_index,
            .responder_index = responder_index,
        };
        context->reports[initiator_index] =
            (struct survey_gateway_report_slot) {
            .reverse_next_hop_index = initiator_index,
            .reverse_hop_count = reverse_hop_count,
            .metadata = (uint8_t)(COMMAND_OK << 4u),
        };
        context->reports[responder_index] =
            (struct survey_gateway_report_slot) {
                .reverse_next_hop_index = responder_index,
                .reverse_hop_count = reverse_hop_count,
                .metadata = (uint8_t)(COMMAND_OK << 4u),
            };
        for (uint8_t part = 0u; part < 3u; part++) {
            context->reports[initiator_index].entries[part].peer_index =
                (uint8_t)(((initiator_index >> (2u * part)) & 0x03u)
                          << 6u);
            context->reports[responder_index].entries[part].peer_index =
                (uint8_t)(((responder_index >> (2u * part)) & 0x03u)
                          << 6u);
        }
    }
    context->node_count = (uint8_t)(2u * pair_count);
    context->report_count = (uint8_t)(2u * pair_count);
    context->pair_count = (uint8_t)pair_count;
    context->pairs_planned = true;
}

static void dispatch_current_batch(struct app_gateway_survey_round *round)
{
    const enum survey_gateway_auto_stage stages[] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };

    size_t stage_index = 0u;
    size_t previous_lane = SIZE_MAX;

    while (!app_gateway_survey_round_go_needed(round)) {
        struct app_gateway_survey_round_control control;

        assert(app_gateway_survey_round_current_control(round, &control) ==
               PROTO_OK);
        if (control.lane_index != previous_lane) {
            previous_lane = control.lane_index;
            stage_index = 0u;
        }
        assert(control.stage == stages[stage_index]);
        assert(app_gateway_survey_round_note_control_success(
                   round,
                   control.command_id,
                   control.target_id,
                   control.pair.operation_generation == 0u ?
                       control.pair.survey_id :
                       survey_operation_session_id(
                           control.pair.operation_generation)) == PROTO_OK);
        stage_index++;
    }
    assert(app_gateway_survey_round_go_needed(round));
}

static void test_round_commitment(
    uint16_t round_id,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    memset(commitment, 0x5au, SEMANTIC_DIGEST_SHA256_LEN);
    proto_put_u16_le(commitment, round_id);
}

static void test_maximum_25_sparse_pairs_serialize_and_complete(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    bool complete = false;

    context_init(&context, 25u, 2u);
    assert(app_gateway_survey_round_begin(
               &round,
               &context,
               SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
               1u) == PROTO_OK);
    assert(round.planned_round_count == 25u);
    for (size_t pair_index = 0u; pair_index < 25u; pair_index++) {
        struct app_gateway_survey_round_control control;

        assert(app_gateway_survey_round_lane_count(&round) == 1u);
        assert(app_gateway_survey_round_current_control(&round, &control) ==
               PROTO_OK);
        if (pair_index == 0u) {
            assert(app_gateway_survey_round_note_control_success(
                       &round,
                       control.command_id,
                       control.target_id + 1u,
                       control.pair.survey_id) == PROTO_ERR_NOT_FOUND);
            assert(app_gateway_survey_round_current_control(
                       &round, &control) == PROTO_OK);
            assert(control.stage ==
                   SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);
        }
        dispatch_current_batch(&round);
        assert(survey_pair_round_lane_armed(
            app_gateway_survey_round_lane(&round, 0u)));
        assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
               PROTO_OK);
        assert(app_gateway_survey_round_lane(&round, 0u)->state ==
               SURVEY_PAIR_ROUND_LANE_OBSERVING);
        assert(app_gateway_survey_round_finalize_lane(
                   &round,
                   0u,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
        assert(app_gateway_survey_round_batch_complete(&round));
        assert(app_gateway_survey_round_advance_batch(
                   &round, &complete) == PROTO_OK);
        assert(complete == (pair_index == 24u));
    }
    assert(complete);
    assert(app_gateway_survey_round_complete(&round));
    assert(round.runtime.completed_success_count == 25u);
}

static void test_one_lane_failure_rerun_does_not_disturb_peer(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    const struct survey_pair failed_pair = {
        .initiator_id = UINT64_C(0x1002),
        .responder_id = UINT64_C(0x1003),
        .survey_id = UINT32_C(0x27182818),
        .sample_count = 2u,
    };
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    bool complete = true;

    context_init(&context, 2u, 2u);
    assert(app_gateway_survey_round_begin(&round,
                                           &context,
                                           2u,
                                           1u) == PROTO_OK);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);

    sample = (struct survey_sample) {
        .pair = failed_pair,
        .round_id = round.runtime.batch_sequence,
        .sample_index = 0u,
        .distance_mm = 900,
        .quality = 80u,
        .range_status = RANGE_OK,
    };
    assert(app_gateway_survey_round_note_sample(
               &round,
               failed_pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 1u);
    assert(accepted_new);
    assert(app_gateway_survey_round_lane(&round, 0u)->usable_result_mask ==
           0u);

    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) == PROTO_OK);
    assert(round.runtime.completed_success_count == 1u);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(!app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));

    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(!complete);
    assert(app_gateway_survey_round_lane_count(&round) == 1u);
    assert(app_gateway_survey_round_lane(&round, 0u)->pair.initiator_id ==
           failed_pair.initiator_id);
    assert(app_gateway_survey_round_lane(&round, 0u)->reruns_started == 1u);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_FAIL) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(complete);
    assert(round.runtime.completed_success_count == 1u);
    assert(round.runtime.completed_failure_count == 1u);
}

static void test_control_failure_skips_only_one_lane_before_go(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;
    size_t failed_lane = SIZE_MAX;

    context_init(&context, 2u, 2u);
    assert(app_gateway_survey_round_begin(&round, &context, 2u, 1u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(app_gateway_survey_round_note_control_failure(
               &round,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &failed_lane) == PROTO_OK);
    assert(failed_lane == 0u);
    assert(round.runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_go_needed(&round));
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    assert(round.runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    assert(round.runtime.lanes[1].state ==
           SURVEY_PAIR_ROUND_LANE_OBSERVING);
}

static void test_cleanup_completed_batch_advance_stress(void)
{
    for (uint32_t cycle = 0u; cycle < 512u; cycle++) {
        struct survey_gateway_context context;
        struct app_gateway_survey_round round;
        struct app_gateway_survey_round_control control;
        size_t failed_lane = SIZE_MAX;
        bool complete = true;

        context_init(&context, 1u, 1u);
        assert(app_gateway_survey_round_begin(&round, &context, 1u, 1u) ==
               PROTO_OK);
        assert(app_gateway_survey_round_current_control(&round, &control) ==
               PROTO_OK);
        assert(app_gateway_survey_round_note_control_failure(
                   &round,
                   control.command_id,
                   control.target_id,
                   control.pair.survey_id,
                   SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
                   SURVEY_PAIR_ROUND_CLEANUP_RETRY,
                   &failed_lane) == PROTO_OK);
        assert(failed_lane == 0u);
        assert(app_gateway_survey_round_note_cleanup_complete(
                   &round,
                   failed_lane,
                   SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
        assert(!app_gateway_survey_round_batch_complete(&round));
        assert(app_gateway_survey_round_note_cleanup_complete(
                   &round,
                   failed_lane,
                   SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
        assert(app_gateway_survey_round_batch_complete(&round));
        assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
               PROTO_OK);
        assert(!complete);
        assert(round.phase == APP_GATEWAY_SURVEY_ROUND_DISPATCHING);
        assert(app_gateway_survey_round_lane_count(&round) == 1u);
        assert(app_gateway_survey_round_lane(&round, 0u)->reruns_started ==
               1u);
        assert(app_gateway_survey_round_current_control(&round, &control) ==
               PROTO_OK);
        assert(app_gateway_survey_round_note_control_failure(
                   &round,
                   control.command_id,
                   control.target_id,
                   control.pair.survey_id,
                   SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
                   SURVEY_PAIR_ROUND_CLEANUP_RETRY,
                   &failed_lane) == PROTO_OK);
        assert(app_gateway_survey_round_note_cleanup_complete(
                   &round,
                   failed_lane,
                   SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
        assert(app_gateway_survey_round_note_cleanup_complete(
                   &round,
                   failed_lane,
                   SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
        assert(app_gateway_survey_round_batch_complete(&round));
        assert(app_gateway_survey_round_lane(&round, 0u)->state ==
               SURVEY_PAIR_ROUND_LANE_FAILED);
        assert(round.runtime.completed_success_count == 0u);
        assert(round.runtime.completed_failure_count == 1u);
        assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
               PROTO_OK);
        assert(complete);
    }
}

static void test_partial_go_cleans_both_leases_before_rerun_commitment(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct survey_pair_lease initiator_lease;
    struct survey_pair_lease responder_lease;
    struct survey_pair pair;
    struct survey_pair next_pair;
    struct survey_pair_control_id prepare_id;
    struct survey_pair_control_id start_id;
    struct survey_pair_control_id next_prepare_id;
    uint8_t old_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t next_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t old_round_id;
    uint16_t next_round_id;
    bool complete = true;

    context_init(&context, 1u, 1u);
    context.operation_generation = UINT64_C(0x0000000127182818);
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 1u) ==
           PROTO_OK);
    dispatch_current_batch(&round);
    old_round_id = round.runtime.batch_sequence;
    pair = app_gateway_survey_round_lane(&round, 0u)->pair;
    test_round_commitment(old_round_id, old_commitment);
    prepare_id = (struct survey_pair_control_id) {
        .session_id = survey_operation_session_id(
            pair.operation_generation),
        .command_seq = 100u,
    };
    start_id = (struct survey_pair_control_id) {
        .session_id = prepare_id.session_id,
        .command_seq = 101u,
    };

    survey_pair_lease_reset(&initiator_lease);
    survey_pair_lease_reset(&responder_lease);
    assert(survey_pair_lease_prepare_round_bound(
               &initiator_lease, &pair, old_round_id, old_commitment,
               &prepare_id, 10u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_prepare_round_bound(
               &responder_lease, &pair, old_round_id, old_commitment,
               &prepare_id, 10u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound(
               &initiator_lease, &pair, old_round_id, old_commitment,
               &start_id, 11u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound(
               &responder_lease, &pair, old_round_id, old_commitment,
               &start_id, 11u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&initiator_lease, &start_id));
    assert(survey_pair_lease_release_start(&responder_lease, &start_id));

    /*
     * Only the initiator receives GO. It runs, times out, and retires while
     * the responder keeps the old commitment in START_PENDING.
     */
    assert(survey_pair_lease_go_until_bound(
               &initiator_lease,
               pair.operation_generation,
               pair.survey_id,
               old_round_id,
               old_commitment,
               12u,
               1012u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_ready_snapshot(&initiator_lease, NULL));
    assert(survey_pair_lease_mark_running_at(
               &initiator_lease, 12u, NULL, NULL));
    assert(survey_pair_lease_finish(&initiator_lease));
    assert(initiator_lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(responder_lease.phase == SURVEY_PAIR_LEASE_START_PENDING);

    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(round.runtime.lanes[0].state ==
           SURVEY_PAIR_ROUND_LANE_CLEANUP);
    assert(round.runtime.lanes[0].cleanup_mask ==
           SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK);

    next_round_id = (uint16_t)(old_round_id + 1u);
    assert(next_round_id != SURVEY_LEGACY_ROUND_ID);
    test_round_commitment(next_round_id, next_commitment);
    next_prepare_id = (struct survey_pair_control_id) {
        .session_id = prepare_id.session_id,
        .command_seq = 200u,
    };
    assert(survey_pair_lease_prepare_round_bound(
               &responder_lease, &pair, next_round_id, next_commitment,
               &next_prepare_id, 20u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
           SURVEY_PAIR_LEASE_BUSY);

    /* One cleanup result cannot release the batch or its old commitment. */
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(!app_gateway_survey_round_batch_complete(&round));
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_ERR_BUSY);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) ==
           PROTO_ERR_MALFORMED);
    assert(!app_gateway_survey_round_batch_complete(&round));
    assert(responder_lease.phase == SURVEY_PAIR_LEASE_START_PENDING);

    assert(survey_pair_lease_abort_matching_round_bound(
        &responder_lease,
        &pair,
        prepare_id.session_id,
        old_round_id,
        old_commitment));
    assert(responder_lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(app_gateway_survey_round_note_cleanup_complete(
               &round,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));

    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(!complete);
    assert(round.runtime.batch_sequence != old_round_id);
    assert(round.runtime.batch_sequence == next_round_id);
    next_pair = app_gateway_survey_round_lane(&round, 0u)->pair;
    assert(next_pair.operation_generation == pair.operation_generation);
    assert(survey_pair_lease_prepare_round_bound(
               &initiator_lease, &next_pair, next_round_id, next_commitment,
               &next_prepare_id, 30u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_prepare_round_bound(
               &responder_lease, &next_pair, next_round_id, next_commitment,
               &next_prepare_id, 30u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
}

static void termination_round_init(struct app_gateway_survey_round *round)
{
    memset(round, 0, sizeof(*round));
    round->phase = APP_GATEWAY_SURVEY_ROUND_OBSERVING;
    round->runtime.active = true;
    round->runtime.lane_count = SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES;
    round->runtime.pending_rerun_count =
        SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES;
    for (size_t i = 0u; i < round->runtime.lane_count; i++) {
        struct survey_pair_round_lane *lane = &round->runtime.lanes[i];

        lane->pair = (struct survey_pair) {
            .operation_generation = UINT64_C(0x100000000) + i,
            .survey_id = UINT32_C(0x27182818),
            .initiator_id = UINT64_C(0x1000) + 2u * i,
            .responder_id = UINT64_C(0x1001) + 2u * i,
            .sample_count = 2u,
        };
        lane->prepared_mask =
            (i % 3u) == 0u ?
                SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK :
            (i % 3u) == 1u ?
                SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK : 0u;
        lane->started_mask =
            (i % 3u) == 0u ?
                SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK : 0u;
        lane->state = SURVEY_PAIR_ROUND_LANE_OBSERVING;
    }
}

static void test_termination_retains_all_25_lane_cleanup_masks(void)
{
    struct app_gateway_survey_round round;
    struct survey_pair pair;
    uint8_t cleanup_mask;
    size_t active_lane = SIZE_MAX;
    size_t lane_index;
    size_t cleanup_lane_count = 0u;

    termination_round_init(&round);
    /*
     * Lane 2 has no confirmed PREPARE, but its in-flight PREPARE may have
     * reached the responder. Lane 4 was already in cleanup when the global
     * abort arrived and must retain that outstanding endpoint as well.
     */
    round.runtime.lanes[4].state = SURVEY_PAIR_ROUND_LANE_CLEANUP;
    round.runtime.lanes[4].cleanup_mask =
        SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK;
    pair = round.runtime.lanes[2].pair;
    assert(app_gateway_survey_round_begin_termination(
               &round,
               &pair,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK,
               &active_lane) == PROTO_OK);
    assert(active_lane == 2u);
    assert(app_gateway_survey_round_terminating(&round));
    assert(round.runtime.pending_rerun_count == 0u);
    assert(round.runtime.lanes[2].cleanup_mask ==
           SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK);
    assert(round.runtime.lanes[4].cleanup_mask ==
           (SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK |
            SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK));

    while (app_gateway_survey_round_next_termination_cleanup(
               &round, &lane_index, &pair, &cleanup_mask) == PROTO_OK) {
        const struct survey_pair_round_lane *lane =
            app_gateway_survey_round_lane(&round, lane_index);

        assert(lane != NULL);
        assert(memcmp(&pair, &lane->pair, sizeof(pair)) == 0);
        cleanup_lane_count++;
        if ((cleanup_mask &
             SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) != 0u) {
            assert(app_gateway_survey_round_note_termination_cleanup_complete(
                       &round,
                       lane_index,
                       SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) ==
                   PROTO_OK);
        }
        if ((cleanup_mask &
             SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) != 0u) {
            assert(app_gateway_survey_round_note_termination_cleanup_complete(
                       &round,
                       lane_index,
                       SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) ==
                   PROTO_OK);
        }
    }
    assert(cleanup_lane_count == 18u);
    assert(app_gateway_survey_round_next_termination_cleanup(
               &round, &lane_index, &pair, &cleanup_mask) ==
           PROTO_ERR_NOT_FOUND);
    for (size_t i = 0u; i < round.runtime.lane_count; i++) {
        assert(round.runtime.lanes[i].cleanup_mask == 0u);
    }
}

static void test_termination_rejects_unowned_active_pair_atomically(void)
{
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round before;
    struct survey_pair pair;
    size_t lane_index = SIZE_MAX;

    termination_round_init(&round);
    before = round;
    pair = round.runtime.lanes[7].pair;
    pair.operation_generation++;
    assert(app_gateway_survey_round_begin_termination(
               &round,
               &pair,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               &lane_index) == PROTO_ERR_NOT_FOUND);
    assert(memcmp(&round, &before, sizeof(round)) == 0);
    assert(lane_index == SIZE_MAX);
}

static void test_go_submission_retry_policy_covers_transient_admission(void)
{
    for (int error = -4096; error <= 4096; error++) {
        const bool expected =
            error == -EAGAIN || error == -EBUSY ||
            error == -ENOSPC || error == -ESHUTDOWN;

        assert(app_gateway_survey_round_go_submit_retryable(error) ==
               expected);
    }
}

static void test_go_terminal_retry_requires_zero_rf_attempts(void)
{
    for (enum node_comm_terminal_reason reason =
             NODE_COMM_TERMINAL_DELIVERED;
         reason <= NODE_COMM_TERMINAL_CANCELLED;
         reason++) {
        const bool retryable_reason =
            reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ||
            reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED ||
            reason == NODE_COMM_TERMINAL_CANCELLED;

        assert(app_gateway_survey_round_go_terminal_retryable(reason, 0u) ==
               retryable_reason);
        assert(!app_gateway_survey_round_go_terminal_retryable(reason, 1u));
        assert(!app_gateway_survey_round_go_terminal_retryable(
            reason, UINT8_MAX));
    }
    assert(!app_gateway_survey_round_go_terminal_retryable(
        (enum node_comm_terminal_reason)-1, 0u));
    assert(!app_gateway_survey_round_go_terminal_retryable(
        (enum node_comm_terminal_reason)(NODE_COMM_TERMINAL_CANCELLED + 1),
        0u));
}

static void test_sample_conflict_is_rejected_without_round_mutation(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round before;
    struct survey_sample sample;
    const struct survey_pair *pair;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    bool duplicate = true;

    context_init(&context, 1u, 4u);
    context.survey_id = UINT32_C(0x12345678);
    context.node_ids[0] = UINT64_C(0x0102030405060708);
    context.node_ids[1] = UINT64_C(0x1112131415161718);
    assert(app_gateway_survey_round_begin(
               &round, &context, 1u, 0u) == PROTO_OK);
    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_mark_observing_after_go(&round) ==
           PROTO_OK);
    round.runtime.batch_sequence = 7u;
    pair = &app_gateway_survey_round_lane(&round, 0u)->pair;
    sample = (struct survey_sample) {
        .pair = *pair,
        .round_id = round.runtime.batch_sequence,
        .sample_index = 2u,
        .distance_mm = 65637,
        .quality = 58u,
        .range_status = RANGE_DELAYED_TX_MISSED,
    };

    before = round;
    assert(app_gateway_survey_round_preflight_sample(
               &round,
               SURVEY_PAIR_ROUND_LANE_OBSERVING,
               pair->initiator_id,
               &sample,
               &lane_index,
               &duplicate) == PROTO_OK);
    assert(lane_index == 0u);
    assert(!duplicate);
    assert(memcmp(&round, &before, sizeof(round)) == 0);

    assert(app_gateway_survey_round_note_sample(
               &round,
               pair->initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(accepted_new);
    before = round;
    assert(app_gateway_survey_round_preflight_sample(
               &round,
               SURVEY_PAIR_ROUND_LANE_OBSERVING,
               pair->initiator_id,
               &sample,
               &lane_index,
               &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(memcmp(&round, &before, sizeof(round)) == 0);

    /*
     * These valid observations collide at 0x08765b68 under the former 32-bit
     * FNV identity and must remain a conflict, not an idempotent duplicate.
     */
    sample.distance_mm = 948493;
    sample.quality = 24u;
    sample.range_status = RANGE_OK;
    assert(app_gateway_survey_round_preflight_sample(
               &round,
               SURVEY_PAIR_ROUND_LANE_OBSERVING,
               pair->initiator_id,
               &sample,
               &lane_index,
               &duplicate) == PROTO_ERR_MALFORMED);
    assert(memcmp(&round, &before, sizeof(round)) == 0);
    assert(app_gateway_survey_round_note_sample(
               &round,
               pair->initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_ERR_MALFORMED);
    assert(memcmp(&round, &before, sizeof(round)) == 0);
}

int main(void)
{
    test_go_submission_retry_policy_covers_transient_admission();
    test_go_terminal_retry_requires_zero_rf_attempts();
    test_maximum_25_sparse_pairs_serialize_and_complete();
    test_one_lane_failure_rerun_does_not_disturb_peer();
    test_control_failure_skips_only_one_lane_before_go();
    test_cleanup_completed_batch_advance_stress();
    test_partial_go_cleans_both_leases_before_rerun_commitment();
    test_termination_retains_all_25_lane_cleanup_masks();
    test_termination_rejects_unowned_active_pair_atomically();
    test_sample_conflict_is_rejected_without_round_mutation();
    puts("app gateway survey round tests passed");
    return 0;
}
