#include "app_gateway_survey_round.h"
#include "mesh.h"
#include "survey_gateway_transaction.h"
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
    const enum app_gateway_survey_control_stage stages[] = {
        APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR,
        APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER,
        APP_GATEWAY_SURVEY_CONTROL_START_RESPONDER,
        APP_GATEWAY_SURVEY_CONTROL_START_INITIATOR,
    };

    size_t stage_index = 0u;
    size_t previous_lane = SIZE_MAX;

    while (round->phase == APP_GATEWAY_SURVEY_ROUND_DISPATCHING) {
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
    assert(round->phase == APP_GATEWAY_SURVEY_ROUND_OBSERVING);
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
                   APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR);
        }
        dispatch_current_batch(&round);
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
               &accepted_new) == PROTO_ERR_MALFORMED);
    assert(app_gateway_survey_round_lane(&round, 1u)->usable_result_mask ==
           0u);
    assert(app_gateway_survey_round_note_sample(
               &round,
               failed_pair.responder_id,
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

static void test_control_failure_skips_only_one_lane(void)
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

static void test_partial_start_cleans_both_leases_before_rerun_commitment(void)
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
    assert(survey_pair_lease_start_round_bound_at(
               &initiator_lease, &pair, old_round_id, old_commitment,
               &start_id, 11u, 12u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &responder_lease, &pair, old_round_id, old_commitment,
               &start_id, 11u, 12u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&initiator_lease, &start_id));

    /*
     * Only the initiator's START result reaches gateway confirmation. It runs
     * and retires while the responder keeps the old START in custody.
     */
    assert(survey_pair_lease_ready_snapshot(&initiator_lease, NULL));
    assert(survey_pair_lease_mark_running_at(
               &initiator_lease, 12u, NULL, NULL));
    assert(survey_pair_lease_finish(&initiator_lease));
    assert(initiator_lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(responder_lease.phase == SURVEY_PAIR_LEASE_START_PENDING);

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

static void test_incarnation_tracker_is_bounded_and_rfc1982_ordered(void)
{
    struct app_gateway_survey_incarnation_tracker tracker;
    uint32_t previous = UINT32_MAX;

    app_gateway_survey_incarnation_tracker_init(&tracker);
    assert(tracker.count == 0u);
    for (uint64_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        previous = UINT32_MAX;
        assert(app_gateway_survey_incarnation_tracker_note(
                   &tracker,
                   UINT64_C(0x8000000000000000) + i + 1u,
                   (uint32_t)i + 1u,
                   &previous) == 0);
        assert(previous == 0u);
    }
    assert(tracker.count == SURVEY_GATEWAY_MAX_REPORTS);

    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker,
               UINT64_C(0x8000000000000001),
               1u,
               &previous) == 0);
    assert(previous == 1u);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker,
               UINT64_C(0x8000000000000001),
               UINT32_MAX,
               &previous) == -ESTALE);
    assert(previous == 1u);
    assert(tracker.boot_incarnations[0] == 1u);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker,
               UINT64_C(0x8000000000000001),
               UINT32_C(0x80000001),
               &previous) == -ESTALE);
    assert(previous == 1u);
    assert(tracker.boot_incarnations[0] == 1u);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker,
               UINT64_C(0x9000000000000001),
               1u,
               &previous) == -ENOSPC);
    assert(tracker.count == SURVEY_GATEWAY_MAX_REPORTS);

    app_gateway_survey_incarnation_tracker_init(&tracker);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, UINT64_C(0x1234), UINT32_MAX, &previous) == 0);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, UINT64_C(0x1234), 1u, &previous) == 1);
    assert(previous == UINT32_MAX);
    assert(tracker.boot_incarnations[0] == 1u);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, UINT64_C(0x1234), 0u, &previous) == -EINVAL);
    assert(tracker.boot_incarnations[0] == 1u);
}

static void test_asymmetric_anchor_reset_cleans_old_generation_only(void)
{
    const uint64_t anchor_id = UINT64_C(0x1000);
    const uint64_t operation_generation = UINT64_C(0x100000001);
    struct app_gateway_survey_incarnation_tracker tracker;
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round successor_before;
    struct app_gateway_survey_round_control control;
    struct survey_pair cleanup_pair;
    uint32_t previous = 0u;
    uint8_t cleanup_mask = 0u;
    size_t cleanup_lane = SIZE_MAX;

    app_gateway_survey_incarnation_tracker_init(&tracker);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, anchor_id, 41u, &previous) == 0);
    context_init(&context, 1u, 2u);
    context.operation_generation = operation_generation;
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 0u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(control.target_id == anchor_id);
    assert(app_gateway_survey_round_note_control_success(
               &round,
               control.command_id,
               control.target_id,
               survey_operation_session_id(operation_generation)) ==
           PROTO_OK);

    /* A newer startup heartbeat terminates G and retains its exact PREPARE. */
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, anchor_id, 42u, &previous) == 1);
    assert(previous == 41u);
    assert(app_gateway_survey_round_begin_termination(
               &round, NULL, 0u, NULL) == PROTO_OK);
    assert(app_gateway_survey_round_next_termination_cleanup(
               &round,
               &cleanup_lane,
               &cleanup_pair,
               &cleanup_mask) == PROTO_OK);
    assert(cleanup_lane == 0u);
    assert(cleanup_pair.operation_generation == operation_generation);
    assert(cleanup_pair.initiator_id == anchor_id);
    assert(cleanup_mask == SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK);
    assert(app_gateway_survey_round_note_termination_cleanup_complete(
               &round, cleanup_lane, cleanup_mask) == PROTO_OK);
    assert(app_gateway_survey_round_next_termination_cleanup(
               &round,
               &cleanup_lane,
               &cleanup_pair,
               &cleanup_mask) == PROTO_ERR_NOT_FOUND);

    /* Delayed boot-41 evidence cannot terminate or mutate successor G+1. */
    context.operation_generation = operation_generation + 1u;
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 0u) ==
           PROTO_OK);
    successor_before = round;
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, anchor_id, 41u, &previous) == -ESTALE);
    assert(previous == 42u);
    assert(memcmp(&round, &successor_before, sizeof(round)) == 0);
    assert(app_gateway_survey_incarnation_tracker_note(
               &tracker, anchor_id, 42u, &previous) == 0);
    assert(memcmp(&round, &successor_before, sizeof(round)) == 0);
}

static void test_begin_failure_leaves_real_round_retryable(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;

    context_init(&context, 1u, 1u);
    memset(&round, 0xa5, sizeof(round));

    /* Invalid capacity must not leave a started owner behind. */
    assert(app_gateway_survey_round_begin(&round, &context, 0u, 0u) ==
           PROTO_ERR_MALFORMED);
    assert(round.phase == APP_GATEWAY_SURVEY_ROUND_INACTIVE);
    assert(!round.runtime.active);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_ERR_STALE);

    /* The same frozen plan can start normally once its capacity is valid. */
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 0u) ==
           PROTO_OK);
    assert(round.phase == APP_GATEWAY_SURVEY_ROUND_DISPATCHING);
    assert(round.runtime.active);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(control.stage == APP_GATEWAY_SURVEY_CONTROL_PREPARE_INITIATOR);
}

static void test_control_failure_requires_operation_session_id(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;
    const uint64_t operation_generation = UINT64_C(0x00000001a5a55a5a);
    size_t lane_index = SIZE_MAX;

    context_init(&context, 1u, 1u);
    context.operation_generation = operation_generation;
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 0u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(survey_operation_session_id(operation_generation) !=
           control.pair.survey_id);

    /* A visible survey ID is not a nonzero-generation control session. */
    assert(app_gateway_survey_round_note_control_failure(
               &round,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &lane_index) == PROTO_ERR_NOT_FOUND);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);

    assert(app_gateway_survey_round_note_control_failure(
               &round,
               control.command_id,
               control.target_id,
               survey_operation_session_id(operation_generation),
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &lane_index) == PROTO_OK);
    assert(lane_index == 0u);
}

static void test_control_result_requires_exact_ack_confirm_before_successor(void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct app_gateway_survey_round_control control;
    struct survey_gateway_transaction transaction = {0};
    struct node_transaction_key result_key;
    struct node_comm_terminal_event cancelled_delivery;
    struct proto_packet result_packet = {0};
    struct app_gateway_survey_round_ack_confirm confirm = {0};
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    const uint8_t request_payload[] = { 0x23u, 0x01u };
    const uint8_t result_payload[] = { 0x42u, 0x00u, 0x5au };
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action action;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    const uint64_t operation_generation = UINT64_C(0x00000001a5a55a5a);
    const uint32_t control_started_at_ms = 10u;
    const uint64_t control_deadline_ms = 1000u;

    context_init(&context, 1u, 1u);
    context.operation_generation = operation_generation;
    assert(app_gateway_survey_round_begin(&round, &context, 1u, 0u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);

    result_key = (struct node_transaction_key) {
        .requester_id = UINT64_C(0xabcdef0123456789),
        .responder_id = control.target_id,
        .session_id = survey_operation_session_id(operation_generation),
        .transaction_id = 0x42u,
        .operation_id = (uint16_t)control.command_id,
    };
    result_packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = result_key.responder_id,
        .dst_id = result_key.requester_id,
        .session_id = result_key.session_id,
        .seq = result_key.transaction_id,
        .payload_len = sizeof(result_payload),
    };
    assert(node_transaction_digest_bytes(request_payload,
                                         sizeof(request_payload),
                                         request_digest));
    assert(mesh_packet_semantic_digest(&result_packet,
                                       result_payload,
                                       sizeof(result_payload),
                                       result_digest));
    survey_gateway_transaction_init(&transaction);
    assert(survey_gateway_transaction_load_pair(&transaction, &control.pair) ==
           PROTO_OK);
    assert(survey_gateway_transaction_begin(&transaction,
                                            &result_key,
                                            control.command_id,
                                            request_digest,
                                            0x91u,
                                            0x45u,
                                            control_deadline_ms,
                                            control_started_at_ms) == PROTO_OK);
    assert(survey_gateway_transaction_reconcile_result(
               &transaction,
               &result_key,
               request_digest,
               result_digest,
               result_key.transaction_id,
               COMMAND_OK,
               20u,
               &transaction_result,
               &action) == PROTO_OK);
    assert(transaction_result ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);

    /* Match the production cancel/take retirement of the accepted request. */
    cancelled_delivery = (struct node_comm_terminal_event) {
        .handle = 0x45u,
        .client_token = 0x91u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = 1u,
    };
    assert(survey_gateway_transaction_note_delivery_terminal(
               &transaction, &cancelled_delivery, 21u, &action) == PROTO_OK);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
    assert(transaction.active.request_delivery_terminal);

    assert(app_gateway_survey_round_capture_control_result(
               &round,
               control.command_id,
               control.target_id,
               &transaction.active,
               transaction.active_started_at_ms,
               COMMAND_OK) == PROTO_OK);
    assert(round.control_confirmation.started_at_ms ==
           control_started_at_ms);
    assert(round.control_confirmation.deadline_ms == control_deadline_ms);

    /* An idempotent capture must bind both ends of the exact interval. */
    assert(app_gateway_survey_round_capture_control_result(
               &round,
               control.command_id,
               control.target_id,
               &transaction.active,
               control_started_at_ms,
               COMMAND_OK) == PROTO_OK);
    assert(app_gateway_survey_round_capture_control_result(
               &round,
               control.command_id,
               control.target_id,
               &transaction.active,
               control_started_at_ms + 1u,
               COMMAND_OK) == PROTO_ERR_BUSY);
    transaction.active.spec.absolute_deadline_ms = control_deadline_ms + 1u;
    assert(app_gateway_survey_round_capture_control_result(
               &round,
               control.command_id,
               control.target_id,
               &transaction.active,
               control_started_at_ms,
               COMMAND_OK) == PROTO_ERR_BUSY);
    transaction.active.spec.absolute_deadline_ms = control_deadline_ms;

    assert(survey_gateway_transaction_phase_complete(&transaction) ==
           PROTO_OK);
    assert(transaction.active.state == NODE_TRANSACTION_EMPTY);
    assert(app_gateway_survey_round_control_confirmation_pending(&round));
    assert(app_gateway_survey_round_control_confirmation_deadline(&round) ==
           control_deadline_ms);
    assert(!app_gateway_survey_round_control_confirmation_expired(
        &round, control_deadline_ms - 1u));
    assert(app_gateway_survey_round_control_confirmation_expired(
        &round, control_deadline_ms));
    assert(node_transaction_key_equal(
        &round.control_confirmation.result_key, &result_key));
    assert(memcmp(round.control_confirmation.semantic_digest,
                  result_digest,
                  sizeof(result_digest)) == 0);

    /* Retiring the request does not authorize the next control yet. */
    assert(app_gateway_survey_round_note_control_success(
               &round,
               control.command_id,
               control.target_id,
               result_key.session_id) == PROTO_ERR_BUSY);

    confirm = (struct app_gateway_survey_round_ack_confirm) {
        .source_id = result_packet.src_id,
        .destination_id = result_packet.dst_id,
        .first_received_at_ms = 999u,
        .msg_type = MSG_COMMAND_RESULT,
        .session_id = result_key.session_id,
        .seq = result_key.transaction_id,
    };
    memset(confirm.semantic_digest, 0xa5u, sizeof(confirm.semantic_digest));
    assert(app_gateway_survey_round_note_control_ack_confirm(
               &round, &confirm) == PROTO_ERR_MALFORMED);
    assert(app_gateway_survey_round_control_confirmation_pending(&round));

    memcpy(confirm.semantic_digest,
           result_digest,
           sizeof(confirm.semantic_digest));
    assert(app_gateway_survey_round_note_control_ack_confirm(
               &round, &confirm) == PROTO_OK);
    assert(!app_gateway_survey_round_control_confirmation_pending(&round));
    assert(round.control_confirmation.confirmed_at_ms == 999u);
    assert(app_gateway_survey_round_control_confirmation_received_in_interval(
        &round, 10u, 1000u));

    /* A later duplicate cannot replace the first timely physical receipt. */
    confirm.first_received_at_ms = 1001u;
    assert(app_gateway_survey_round_note_control_ack_confirm(
               &round, &confirm) == PROTO_OK);
    assert(round.control_confirmation.confirmed_at_ms == 999u);
    assert(app_gateway_survey_round_control_confirmation_ready(
               &round, &control, &status) == PROTO_OK);
    assert(status == COMMAND_OK);
    assert(control.pair.operation_generation == operation_generation);

    assert(app_gateway_survey_round_note_control_success(
               &round,
               control.command_id,
               control.target_id,
               result_key.session_id) == PROTO_OK);
    app_gateway_survey_round_clear_control_confirmation(&round);
    assert(app_gateway_survey_round_current_control(&round, &control) ==
           PROTO_OK);
    assert(control.stage == APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER);

    /* A delayed replay cannot advance the same generation a second time. */
    assert(app_gateway_survey_round_note_control_ack_confirm(
               &round, &confirm) == PROTO_ERR_NOT_FOUND);
}

struct test_control_confirmation_fixture {
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    struct survey_gateway_transaction transaction;
    struct app_gateway_survey_round_control control;
    struct app_gateway_survey_round_ack_confirm confirm;
};

static void control_confirmation_fixture_init(
    struct test_control_confirmation_fixture *fixture,
    uint32_t started_at_ms,
    uint64_t deadline_ms)
{
    const uint64_t operation_generation = UINT64_C(0x0000000277112233);
    const uint8_t request_payload[] = { 0x19u, 0x27u };
    const uint8_t result_payload[] = { 0x31u, 0x41u };
    struct node_transaction_key result_key;
    struct node_comm_terminal_event terminal;
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action action;

    memset(fixture, 0, sizeof(*fixture));
    context_init(&fixture->context, 1u, 1u);
    fixture->context.operation_generation = operation_generation;
    assert(app_gateway_survey_round_begin(
               &fixture->round, &fixture->context, 1u, 0u) == PROTO_OK);
    assert(app_gateway_survey_round_current_control(
               &fixture->round, &fixture->control) == PROTO_OK);

    result_key = (struct node_transaction_key) {
        .requester_id = UINT64_C(0xabcdef0123456789),
        .responder_id = fixture->control.target_id,
        .session_id = survey_operation_session_id(operation_generation),
        .transaction_id = 0x52u,
        .operation_id = (uint16_t)fixture->control.command_id,
    };
    assert(node_transaction_digest_bytes(request_payload,
                                         sizeof(request_payload),
                                         request_digest));
    assert(node_transaction_digest_bytes(result_payload,
                                         sizeof(result_payload),
                                         result_digest));
    survey_gateway_transaction_init(&fixture->transaction);
    assert(survey_gateway_transaction_load_pair(
               &fixture->transaction, &fixture->control.pair) == PROTO_OK);
    assert(survey_gateway_transaction_begin(&fixture->transaction,
                                            &result_key,
                                            fixture->control.command_id,
                                            request_digest,
                                            0xa1u,
                                            0x55u,
                                            deadline_ms,
                                            started_at_ms) == PROTO_OK);
    assert(survey_gateway_transaction_reconcile_result(
               &fixture->transaction,
               &result_key,
               request_digest,
               result_digest,
               result_key.transaction_id,
               COMMAND_OK,
               (uint64_t)started_at_ms + 1u,
               &transaction_result,
               &action) == PROTO_OK);
    assert(transaction_result ==
           SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);

    terminal = (struct node_comm_terminal_event) {
        .handle = 0x55u,
        .client_token = 0xa1u,
        .reason = NODE_COMM_TERMINAL_CANCELLED,
        .attempts_started = 1u,
    };
    assert(survey_gateway_transaction_note_delivery_terminal(
               &fixture->transaction,
               &terminal,
               (uint64_t)started_at_ms + 2u,
               &action) == PROTO_OK);
    assert(action == NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS);
    assert(app_gateway_survey_round_capture_control_result(
               &fixture->round,
               fixture->control.command_id,
               fixture->control.target_id,
               &fixture->transaction.active,
               started_at_ms,
               COMMAND_OK) == PROTO_OK);
    assert(survey_gateway_transaction_phase_complete(
               &fixture->transaction) == PROTO_OK);

    fixture->confirm = (struct app_gateway_survey_round_ack_confirm) {
        .source_id = result_key.responder_id,
        .destination_id = result_key.requester_id,
        .session_id = result_key.session_id,
        .seq = result_key.transaction_id,
        .msg_type = MSG_COMMAND_RESULT,
    };
    memcpy(fixture->confirm.semantic_digest,
           result_digest,
           sizeof(fixture->confirm.semantic_digest));
}

static void test_ack_confirm_deadline_controls_round_advance(void)
{
    static struct test_control_confirmation_fixture fixture;
    const uint32_t started_at_ms = 100u;
    const uint64_t deadline_ms = 1000u;
    const uint64_t received_at_ms[] = {
        deadline_ms - 1u,
        deadline_ms,
        deadline_ms + 1u,
    };

    for (size_t i = 0u;
         i < sizeof(received_at_ms) / sizeof(received_at_ms[0]);
         i++) {
        struct app_gateway_survey_round_control ready_control;
        struct app_gateway_survey_round_control still_current;
        enum command_status status = COMMAND_INTERNAL_ERROR;

        control_confirmation_fixture_init(&fixture,
                                          started_at_ms,
                                          deadline_ms);
        assert(app_gateway_survey_round_control_confirmation_pending(
            &fixture.round));
        assert(app_gateway_survey_round_control_confirmation_deadline(
                   &fixture.round) == deadline_ms);
        assert(!app_gateway_survey_round_control_confirmation_expired(
            &fixture.round, deadline_ms - 1u));
        assert(app_gateway_survey_round_control_confirmation_expired(
            &fixture.round, deadline_ms));

        fixture.confirm.first_received_at_ms = received_at_ms[i];
        assert(app_gateway_survey_round_note_control_ack_confirm(
                   &fixture.round, &fixture.confirm) == PROTO_OK);
        assert(!app_gateway_survey_round_control_confirmation_pending(
            &fixture.round));

        if (received_at_ms[i] < deadline_ms) {
            assert(app_gateway_survey_round_control_confirmation_ready(
                       &fixture.round,
                       &ready_control,
                       &status) == PROTO_OK);
            assert(status == COMMAND_OK);
            assert(app_gateway_survey_round_note_control_success(
                       &fixture.round,
                       ready_control.command_id,
                       ready_control.target_id,
                       ready_control.pair.operation_generation == 0u ?
                           ready_control.pair.survey_id :
                           survey_operation_session_id(
                               ready_control.pair.operation_generation)) ==
                   PROTO_OK);
            assert(app_gateway_survey_round_current_control(
                       &fixture.round, &still_current) == PROTO_OK);
            assert(still_current.stage ==
                   APP_GATEWAY_SURVEY_CONTROL_PREPARE_RESPONDER);
        } else {
            assert(app_gateway_survey_round_control_confirmation_ready(
                       &fixture.round,
                       &ready_control,
                       &status) == -ETIMEDOUT);
            assert(app_gateway_survey_round_current_control(
                       &fixture.round, &still_current) == PROTO_OK);
            assert(still_current.stage == fixture.control.stage);
            assert(still_current.target_id == fixture.control.target_id);
        }
    }
}

static void test_ack_confirm_physical_deadline_is_closed_open_across_wrap(void)
{
    struct app_gateway_survey_round round = {0};
    const uint32_t started_at_ms = UINT32_MAX - 5u;
    const uint32_t deadline_ms = 3u;

    round.control_confirmation.valid = true;
    round.control_confirmation.confirmed = true;
    round.control_confirmation.confirmed_at_ms =
        (UINT64_C(1) << 32u) | (deadline_ms - 1u);
    assert(app_gateway_survey_round_control_confirmation_received_in_interval(
        &round, started_at_ms, deadline_ms));

    round.control_confirmation.confirmed_at_ms =
        (UINT64_C(1) << 32u) | deadline_ms;
    assert(!app_gateway_survey_round_control_confirmation_received_in_interval(
        &round, started_at_ms, deadline_ms));
    round.control_confirmation.confirmed_at_ms =
        (UINT64_C(1) << 32u) | (deadline_ms + 1u);
    assert(!app_gateway_survey_round_control_confirmation_received_in_interval(
        &round, started_at_ms, deadline_ms));
}

static void test_outcome_event_identity_survives_retry_and_blocks_batch_advance(
    void)
{
    struct survey_gateway_context context;
    struct app_gateway_survey_round round;
    uint32_t event_seq = UINT32_MAX;
    bool complete = false;

    context_init(&context, 2u, 1u);
    assert(app_gateway_survey_round_begin(&round, &context, 2u, 0u) ==
           PROTO_OK);
    assert(app_gateway_survey_round_lane_count(&round) == 2u);
    assert(app_gateway_survey_round_outcome_event_seed(
               &round, 0u, &event_seq) == PROTO_OK);
    assert(event_seq == 0u);

    /* A failed first enqueue consumed identity 41 for lane zero. */
    assert(app_gateway_survey_round_outcome_event_retain(
               &round, 0u, 41u) == PROTO_OK);
    event_seq = 0u;
    assert(app_gateway_survey_round_outcome_event_seed(
               &round, 0u, &event_seq) == PROTO_OK);
    assert(event_seq == 41u);

    /* No other lane or changed identity can replace the retained owner. */
    event_seq = 0u;
    assert(app_gateway_survey_round_outcome_event_seed(
               &round, 1u, &event_seq) == PROTO_ERR_BUSY);
    assert(app_gateway_survey_round_outcome_event_retain(
               &round, 0u, 42u) == PROTO_ERR_BUSY);
    assert(app_gateway_survey_round_outcome_event_complete(
               &round, 1u) == PROTO_ERR_BUSY);
    assert(round.outcome_event_pending);
    assert(round.outcome_event_lane_index == 0u);
    assert(round.outcome_event_seq == 41u);

    dispatch_current_batch(&round);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(app_gateway_survey_round_finalize_lane(
               &round,
               1u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(app_gateway_survey_round_batch_complete(&round));

    /* A next batch may not alias lane zero while its outcome is unaccepted. */
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_ERR_BUSY);
    assert(!complete);
    assert(app_gateway_survey_round_outcome_event_complete(
               &round, 0u) == PROTO_OK);
    assert(!round.outcome_event_pending);
    event_seq = UINT32_MAX;
    assert(app_gateway_survey_round_outcome_event_seed(
               &round, 0u, &event_seq) == PROTO_OK);
    assert(event_seq == 0u);
    assert(app_gateway_survey_round_advance_batch(&round, &complete) ==
           PROTO_OK);
    assert(complete);

    /* Starting a fresh operation clears every retry identity field. */
    assert(app_gateway_survey_round_begin(&round, &context, 2u, 0u) ==
           PROTO_OK);
    assert(!round.outcome_event_pending);
    assert(round.outcome_event_seq == 0u);
    event_seq = UINT32_MAX;
    assert(app_gateway_survey_round_outcome_event_seed(
               &round, 0u, &event_seq) == PROTO_OK);
    assert(event_seq == 0u);
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
               pair->responder_id,
               &sample,
               &lane_index,
               &duplicate) == PROTO_OK);
    assert(lane_index == 0u);
    assert(!duplicate);
    assert(memcmp(&round, &before, sizeof(round)) == 0);

    assert(app_gateway_survey_round_note_sample(
               &round,
               pair->responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(accepted_new);
    before = round;
    assert(app_gateway_survey_round_preflight_sample(
               &round,
               SURVEY_PAIR_ROUND_LANE_OBSERVING,
               pair->responder_id,
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
               pair->responder_id,
               &sample,
               &lane_index,
               &duplicate) == PROTO_ERR_MALFORMED);
    assert(memcmp(&round, &before, sizeof(round)) == 0);
    assert(app_gateway_survey_round_note_sample(
               &round,
               pair->responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_ERR_MALFORMED);
    assert(memcmp(&round, &before, sizeof(round)) == 0);
}

int main(void)
{
    test_maximum_25_sparse_pairs_serialize_and_complete();
    test_one_lane_failure_rerun_does_not_disturb_peer();
    test_control_failure_skips_only_one_lane();
    test_cleanup_completed_batch_advance_stress();
    test_partial_start_cleans_both_leases_before_rerun_commitment();
    test_termination_retains_all_25_lane_cleanup_masks();
    test_termination_rejects_unowned_active_pair_atomically();
    test_incarnation_tracker_is_bounded_and_rfc1982_ordered();
    test_asymmetric_anchor_reset_cleans_old_generation_only();
    test_begin_failure_leaves_real_round_retryable();
    test_control_failure_requires_operation_session_id();
    test_control_result_requires_exact_ack_confirm_before_successor();
    test_ack_confirm_deadline_controls_round_advance();
    test_ack_confirm_physical_deadline_is_closed_open_across_wrap();
    test_outcome_event_identity_survives_retry_and_blocks_batch_advance();
    test_sample_conflict_is_rejected_without_round_mutation();
    puts("app gateway survey round tests passed");
    return 0;
}
