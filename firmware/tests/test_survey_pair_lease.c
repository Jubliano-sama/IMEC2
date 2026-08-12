#include "survey_pair_lease.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define PREPARE_LEASE_MS 180000u

_Static_assert(PREPARE_LEASE_MS <= INT32_MAX,
               "the prepared lease must fit wrap-safe signed time ordering");

static struct survey_pair pair_with(uint32_t survey_id, uint64_t suffix)
{
    const struct survey_pair pair = {
        .initiator_id = 0x1111000000000000ull + suffix,
        .responder_id = 0x2222000000000000ull + suffix,
        .survey_id = survey_id,
        .sample_count = 4u,
    };

    return pair;
}

static struct survey_pair operation_pair_with(uint64_t operation_generation,
                                              uint32_t survey_id,
                                              uint64_t suffix)
{
    struct survey_pair pair = pair_with(survey_id, suffix);

    pair.operation_generation = operation_generation;
    return pair;
}

static void assert_pair_equal(const struct survey_pair *left,
                              const struct survey_pair *right)
{
    assert(left->operation_generation == right->operation_generation);
    assert(left->survey_id == right->survey_id);
    assert(left->initiator_id == right->initiator_id);
    assert(left->responder_id == right->responder_id);
    assert(left->sample_count == right->sample_count);
}

static struct survey_pair_control_id control_id(uint32_t session_id,
                                                uint16_t command_seq)
{
    const struct survey_pair_control_id id = {
        .session_id = session_id,
        .command_seq = command_seq,
    };

    return id;
}

static void test_operation_generation_and_commitment_isolate_stale_traffic(void)
{
    const uint64_t old_generation = UINT64_C(0x0000000112345678);
    const uint64_t new_generation = old_generation + 1u;
    const struct survey_pair old_pair =
        operation_pair_with(old_generation, 170u, 1u);
    const struct survey_pair new_pair =
        operation_pair_with(new_generation, 170u, 1u);
    const struct survey_pair_control_id old_prepare =
        control_id(survey_operation_session_id(old_generation), 100u);
    const struct survey_pair_control_id old_start =
        control_id(survey_operation_session_id(old_generation), 101u);
    const struct survey_pair_control_id new_prepare =
        control_id(survey_operation_session_id(new_generation), 1u);
    const struct survey_pair_control_id new_start =
        control_id(survey_operation_session_id(new_generation), 2u);
    const uint8_t old_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x11u};
    const uint8_t new_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x22u};
    const uint8_t wrong_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x33u};
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round_bound(
               &lease, &old_pair, 7u, old_commitment, &old_prepare,
               100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &old_pair, 7u, old_commitment, &old_start,
               101u, 1000u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &old_start));
    assert(survey_pair_lease_ready_snapshot(&lease, NULL));
    assert(survey_pair_lease_execution_remaining_ms(&lease, 102u) == 898u);

    /*
     * A gateway incarnation change supersedes an armed old round even when
     * the host reuses its survey ID and the new command sequence restarts.
     */
    assert(survey_pair_lease_prepare_round_bound(
               &lease, &new_pair, 8u, new_commitment, &new_prepare,
               103u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_SUPERSEDED);
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert_pair_equal(&lease.pair, &new_pair);
    assert(!lease.start_released);
    assert(!lease.start_execution_armed);

    assert(survey_pair_lease_start_round_bound(
               &lease, &old_pair, 7u, old_commitment, &old_start,
               104u) == SURVEY_PAIR_LEASE_STALE);
    assert(!survey_pair_lease_abort_matching_round_bound(
        &lease,
        &old_pair,
        survey_operation_session_id(old_generation),
        7u,
        old_commitment));

    assert(survey_pair_lease_start_round_bound(
               &lease, &new_pair, 8u, wrong_commitment, &new_start,
               105u) == SURVEY_PAIR_LEASE_INVALID_STATE);
    assert(survey_pair_lease_start_round_bound(
               &lease, &new_pair, 8u, new_commitment, &new_start,
               105u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(!survey_pair_lease_abort_matching_round_bound(
        &lease,
        &new_pair,
        survey_operation_session_id(new_generation),
        8u,
        wrong_commitment));
    assert(survey_pair_lease_abort_matching_round_bound(
        &lease,
        &new_pair,
        survey_operation_session_id(new_generation),
        8u,
        new_commitment));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_operation_generation_rejects_legacy_round_zero(void)
{
    const uint64_t generation = UINT64_C(0x0000000212345678);
    const struct survey_pair pair =
        operation_pair_with(generation, 180u, 1u);
    const struct survey_pair_control_id prepare =
        control_id(survey_operation_session_id(generation), 10u);
    const uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0xa5u};
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round_bound(
               &lease,
               &pair,
               SURVEY_LEGACY_ROUND_ID,
               commitment,
               &prepare,
               100u,
               PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_INVALID_ARGUMENT);
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_new_generation_cannot_overwrite_running_radio_owner(void)
{
    const uint64_t generation = UINT64_C(0x0000000312345678);
    const struct survey_pair pair =
        operation_pair_with(generation, 190u, 1u);
    const struct survey_pair next_pair =
        operation_pair_with(generation + 1u, 190u, 1u);
    const struct survey_pair_control_id prepare =
        control_id(survey_operation_session_id(generation), 10u);
    const struct survey_pair_control_id start =
        control_id(survey_operation_session_id(generation), 11u);
    const struct survey_pair_control_id next_prepare =
        control_id(survey_operation_session_id(generation + 1u), 1u);
    const uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x44u};
    const uint8_t next_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x55u};
    const uint16_t round_id = 9u;
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round_bound(
               &lease, &pair, round_id, commitment,
               &prepare, 100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound(
               &lease, &pair, round_id, commitment,
               &start, 101u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_mark_running(&lease, NULL, NULL));
    assert(survey_pair_lease_prepare_round_bound(
               &lease, &next_pair, round_id + 1u,
               next_commitment, &next_prepare, 102u,
               PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert_pair_equal(&lease.pair, &pair);
    assert(survey_pair_lease_invariant(&lease));
}

static void prepare_ok(struct survey_pair_lease *lease,
                       const struct survey_pair *pair,
                       uint16_t seq,
                       uint32_t now_ms)
{
    const struct survey_pair_control_id id = control_id(pair->survey_id, seq);

    assert(survey_pair_lease_prepare(lease, pair, &id, now_ms,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_invariant(lease));
}

static void start_ok(struct survey_pair_lease *lease,
                     const struct survey_pair *pair,
                     uint16_t seq,
                     uint32_t now_ms)
{
    const struct survey_pair_control_id id = control_id(pair->survey_id, seq);

    assert(survey_pair_lease_start(lease, pair, &id, now_ms) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_invariant(lease));
}

static void release_ok(struct survey_pair_lease *lease,
                       const struct survey_pair *pair,
                       uint16_t seq)
{
    const struct survey_pair_control_id id = control_id(pair->survey_id, seq);

    assert(survey_pair_lease_release_start(lease, &id));
    assert(survey_pair_lease_ready_snapshot(lease, NULL));
    assert(survey_pair_lease_invariant(lease));
}

static void test_reset_clears_every_phase(void)
{
    const struct survey_pair pair = pair_with(10u, 1u);
    struct survey_pair_lease lease;

    memset(&lease, 0xa5, sizeof(lease));
    survey_pair_lease_reset(&lease);
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));

    prepare_ok(&lease, &pair, 10u, 100u);
    survey_pair_lease_reset(&lease);
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!lease.last_accepted_id_valid);
    assert(survey_pair_lease_invariant(&lease));

    prepare_ok(&lease, &pair, 10u, 100u);
    start_ok(&lease, &pair, 11u, 101u);
    release_ok(&lease, &pair, 11u);
    assert(survey_pair_lease_mark_running(&lease, NULL, NULL));
    assert(survey_pair_lease_abort(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_ABORTING);
    survey_pair_lease_reset(&lease);
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_prepare_duplicate_supersede_and_ordering(void)
{
    const struct survey_pair first = pair_with(20u, 1u);
    const struct survey_pair second = pair_with(20u, 2u);
    const struct survey_pair other_survey = pair_with(21u, 1u);
    const struct survey_pair_control_id duplicate = control_id(20u, 100u);
    const struct survey_pair_control_id retry = control_id(20u, 101u);
    const struct survey_pair_control_id stale = control_id(20u, 99u);
    const struct survey_pair_control_id next_pair = control_id(20u, 102u);
    const struct survey_pair_control_id other_session = control_id(21u, 1u);
    uint32_t original_deadline;
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare(&lease, &first, &duplicate, 1000u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    original_deadline = lease.prepared_deadline_ms;
    assert(survey_pair_lease_prepare(&lease, &first, &duplicate, 2000u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(lease.prepared_deadline_ms == original_deadline);
    assert(survey_pair_lease_prepare(&lease, &first, &stale, 2000u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(survey_pair_lease_prepare(&lease, &first, &retry, 2000u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(lease.prepared_deadline_ms == 2000u + PREPARE_LEASE_MS);
    assert(lease.prepare_id.command_seq == retry.command_seq);

    assert(survey_pair_lease_prepare(&lease, &other_survey, &other_session, 2001u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert(survey_pair_lease_prepare(&lease, &second, &next_pair, 2001u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_SUPERSEDED);
    assert_pair_equal(&lease.pair, &second);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_start_is_exact_ordered_transition(void)
{
    const struct survey_pair pair = pair_with(30u, 1u);
    const struct survey_pair other = pair_with(30u, 2u);
    const struct survey_pair_control_id same_as_prepare = control_id(30u, 7u);
    const struct survey_pair_control_id start = control_id(30u, 8u);
    const struct survey_pair_control_id retry = control_id(30u, 9u);
    const struct survey_pair_control_id newer = control_id(30u, 10u);
    struct survey_pair snapshot = {0};
    struct survey_pair_lease lease = {0};

    prepare_ok(&lease, &pair, 7u, 100u);
    assert(survey_pair_lease_start(&lease, &pair, &same_as_prepare, 101u) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(survey_pair_lease_start(&lease, &other, &start, 101u) ==
           SURVEY_PAIR_LEASE_INVALID_STATE);
    assert(survey_pair_lease_start(&lease, &pair, &start, 101u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(lease.phase == SURVEY_PAIR_LEASE_START_PENDING);
    assert(survey_pair_lease_remaining_ms(&lease, 101u) ==
           PREPARE_LEASE_MS - 1u);
    assert(survey_pair_lease_pending_snapshot(&lease, &snapshot));
    assert_pair_equal(&snapshot, &pair);

    assert(survey_pair_lease_start(&lease, &pair, &start, 102u) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_ready_snapshot(&lease, NULL));
    assert(survey_pair_lease_start(&lease, &pair, &retry, 102u) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(lease.start_id.command_seq == start.command_seq);
    assert(survey_pair_lease_pending_snapshot(&lease, NULL));
    assert(survey_pair_lease_ready_snapshot(&lease, &snapshot));
    assert(survey_pair_lease_mark_running(&lease, &snapshot, NULL));
    assert_pair_equal(&snapshot, &pair);
    assert(lease.phase == SURVEY_PAIR_LEASE_RUNNING);
    assert(survey_pair_lease_start(&lease, &pair, &start, 103u) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(survey_pair_lease_start(&lease, &pair, &retry, 103u) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert(survey_pair_lease_start(&lease, &pair, &newer, 103u) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert(lease.start_id.command_seq == start.command_seq);
    assert(survey_pair_lease_finish(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(lease.last_accepted_id_valid);
    assert(survey_pair_lease_prepare(&lease, &pair, &same_as_prepare, 104u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(survey_pair_lease_start(&lease, &pair, &retry, 104u) ==
           SURVEY_PAIR_LEASE_INVALID_STATE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_start_pending_expires_if_radio_never_runs(void)
{
    const struct survey_pair pair = pair_with(31u, 1u);
    const struct survey_pair_control_id start = control_id(31u, 2u);
    const struct survey_pair_control_id wrong = control_id(31u, 3u);
    struct survey_pair_lease lease = {0};
    const uint32_t deadline_ms = 100u + PREPARE_LEASE_MS;

    prepare_ok(&lease, &pair, 1u, 100u);
    assert(survey_pair_lease_start(&lease, &pair, &start, 101u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(!survey_pair_lease_mark_running(&lease, NULL, NULL));
    assert(!survey_pair_lease_release_start(&lease, &wrong));
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_ready_snapshot(&lease, NULL));
    assert(!survey_pair_lease_expire(&lease, deadline_ms - 1u));
    assert(survey_pair_lease_pending_snapshot(&lease, NULL));
    assert(survey_pair_lease_expire(&lease, deadline_ms));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!survey_pair_lease_pending_snapshot(&lease, NULL));
    assert(lease.last_accepted_id_valid);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_prepare_duplicate_does_not_rewind_start(void)
{
    const struct survey_pair pair = pair_with(40u, 1u);
    const struct survey_pair other = pair_with(40u, 2u);
    const struct survey_pair_control_id prepare = control_id(40u, 20u);
    const struct survey_pair_control_id newer_prepare = control_id(40u, 22u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare(&lease, &pair, &prepare, 100u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    start_ok(&lease, &pair, 21u, 101u);
    assert(survey_pair_lease_prepare(&lease, &pair, &prepare, 102u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(lease.phase == SURVEY_PAIR_LEASE_START_PENDING);
    assert(survey_pair_lease_prepare(&lease, &pair, &newer_prepare, 102u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(survey_pair_lease_prepare(&lease, &other, &newer_prepare, 102u,
                                     PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert(lease.phase == SURVEY_PAIR_LEASE_START_PENDING);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_abort_is_bounded_and_idempotent(void)
{
    const struct survey_pair pair = pair_with(50u, 1u);
    struct survey_pair_lease lease = {0};

    assert(!survey_pair_lease_abort(&lease));
    prepare_ok(&lease, &pair, 1u, 100u);
    assert(survey_pair_lease_abort(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!survey_pair_lease_abort(&lease));

    prepare_ok(&lease, &pair, 2u, 200u);
    start_ok(&lease, &pair, 3u, 201u);
    assert(survey_pair_lease_abort(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);

    prepare_ok(&lease, &pair, 4u, 300u);
    start_ok(&lease, &pair, 5u, 301u);
    release_ok(&lease, &pair, 5u);
    assert(survey_pair_lease_mark_running(&lease, NULL, NULL));
    assert(survey_pair_lease_abort(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_ABORTING);
    assert(!survey_pair_lease_abort(&lease));
    assert(!survey_pair_lease_mark_running(&lease, NULL, NULL));
    assert(survey_pair_lease_finish(&lease));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_wrap_safe_expiry_and_gateway_skip(void)
{
    const struct survey_pair pair = pair_with(60u, 1u);
    const struct survey_pair_control_id id = control_id(60u, 1u);
    struct survey_pair_lease lease = {0};
    uint32_t now_ms = UINT32_MAX - 50u;

    assert(survey_pair_lease_prepare(&lease, &pair, &id, now_ms, 100u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(lease.prepared_deadline_ms == 49u);
    assert(survey_pair_lease_remaining_ms(&lease, UINT32_MAX - 1u) == 51u);
    assert(!survey_pair_lease_expire(&lease, 48u));
    assert(survey_pair_lease_remaining_ms(&lease, 48u) == 1u);
    assert(survey_pair_lease_expire(&lease, 49u));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!survey_pair_lease_expire(&lease, 50u));
    assert(survey_pair_lease_invariant(&lease));

    survey_pair_lease_reset(&lease);
    now_ms = UINT32_MAX - 99u;
    assert(survey_pair_lease_prepare(&lease, &pair, &id, now_ms, 100u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(lease.prepared_deadline_ms == 0u);
    {
        const struct survey_pair_control_id start = control_id(60u, 2u);

        assert(survey_pair_lease_start(&lease, &pair, &start, now_ms + 1u) ==
               SURVEY_PAIR_LEASE_ACCEPTED);
    }
    assert(survey_pair_lease_invariant(&lease));
    assert(survey_pair_lease_remaining_ms(&lease, UINT32_MAX) == 1u);
    assert(survey_pair_lease_expire(&lease, 0u));
    assert(survey_pair_lease_invariant(&lease));

    survey_pair_lease_reset(&lease);
    prepare_ok(&lease, &pair, 2u, 5000u);
    assert(survey_pair_lease_remaining_ms(&lease, 5000u) ==
           PREPARE_LEASE_MS);
    assert(!survey_pair_lease_expire(&lease,
                                     5000u + PREPARE_LEASE_MS - 1u));
    assert(survey_pair_lease_expire(&lease,
                                    5000u + PREPARE_LEASE_MS));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_argument_and_sequence_wrap_validation(void)
{
    const struct survey_pair pair = pair_with(70u, 1u);
    const struct survey_pair_control_id wrap_prepare = control_id(70u, 0xffffu);
    const struct survey_pair_control_id wrap_retry = control_id(70u, 1u);
    const struct survey_pair_control_id ambiguous = control_id(70u, 0x8001u);
    struct survey_pair_lease lease = {0};
    struct survey_pair invalid_pair = pair;

    assert(survey_pair_lease_prepare(NULL, &pair, &wrap_prepare, 0u, 1u) ==
           SURVEY_PAIR_LEASE_INVALID_ARGUMENT);
    assert(survey_pair_lease_prepare(&lease, &pair, &wrap_prepare, 0u, 0u) ==
           SURVEY_PAIR_LEASE_INVALID_ARGUMENT);
    assert(survey_pair_lease_prepare(&lease, &pair, &wrap_prepare, 0u,
                                     (uint32_t)INT32_MAX + 1u) ==
           SURVEY_PAIR_LEASE_INVALID_ARGUMENT);
    invalid_pair.sample_count = 0u;
    assert(survey_pair_lease_prepare(&lease, &invalid_pair, &wrap_prepare, 0u,
                                     1u) ==
           SURVEY_PAIR_LEASE_INVALID_ARGUMENT);

    assert(survey_pair_lease_prepare(&lease, &pair, &wrap_prepare, 0u, 100u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_prepare(&lease, &pair, &wrap_retry, 1u, 100u) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(lease.prepare_id.command_seq == 1u);
    assert(survey_pair_lease_prepare(&lease, &pair, &ambiguous, 2u, 100u) ==
           SURVEY_PAIR_LEASE_STALE);
    assert(survey_pair_lease_invariant(&lease));
}

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static void run_random_state_sequence(uint32_t seed)
{
    struct survey_pair_lease lease = {0};
    struct survey_pair pair = pair_with(0x70000000u | (seed & 0xffffu), 1u);
    uint16_t seq = 1u;
    uint32_t now_ms = UINT32_MAX - (seed & 0xfffu);
    uint32_t random = seed;

    for (size_t i = 0u; i < 20000u; i++) {
        struct survey_pair_control_id id;
        uint32_t choice = next_random(&random);

        now_ms += next_random(&random) & 0x3ffu;
        if ((choice & 7u) == 0u) {
            seq = (uint16_t)(seq + 1u);
            if (seq == 0u) {
                seq = 1u;
            }
        }
        id = control_id(pair.survey_id, seq);

        switch (choice % 10u) {
        case 0u:
            (void)survey_pair_lease_prepare(&lease, &pair, &id, now_ms,
                                            PREPARE_LEASE_MS);
            break;
        case 1u:
            (void)survey_pair_lease_start(&lease, &pair, &id, now_ms);
            break;
        case 2u:
            (void)survey_pair_lease_release_start(&lease, &id);
            break;
        case 3u:
            (void)survey_pair_lease_mark_running(&lease, NULL, NULL);
            break;
        case 4u:
            (void)survey_pair_lease_finish(&lease);
            break;
        case 5u:
            (void)survey_pair_lease_abort(&lease);
            break;
        case 6u:
            (void)survey_pair_lease_expire(&lease, now_ms);
            break;
        case 7u:
            (void)survey_pair_lease_remaining_ms(&lease, now_ms);
            break;
        case 8u:
            if (lease.phase == SURVEY_PAIR_LEASE_IDLE) {
                pair = pair_with(pair.survey_id + 1u,
                                 (uint64_t)(choice & 0xffu) + 1u);
                seq = 1u;
            }
            break;
        default:
            survey_pair_lease_reset(&lease);
            break;
        }
        assert(survey_pair_lease_invariant(&lease));
    }
}

static void test_randomized_state_sequence_invariants(void)
{
    static const uint32_t seeds[] = {
        1u,
        0x12345678u,
        0x89abcdefu,
        0xffffffffu,
    };

    for (size_t i = 0u; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        run_random_state_sequence(seeds[i]);
    }
}

static void test_targeted_abort_cannot_cancel_a_new_pair(void)
{
    struct survey_pair_lease lease = {0};
    struct survey_pair old_pair = pair_with(100u, 1u);
    struct survey_pair new_pair = pair_with(100u, 2u);
    struct survey_pair_control_id prepare = control_id(100u, 10u);

    assert(survey_pair_lease_prepare(&lease, &new_pair, &prepare,
                                     100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(!survey_pair_lease_abort_matching(&lease, &old_pair,
                                             old_pair.survey_id));
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert(!survey_pair_lease_abort_matching(&lease, &new_pair, 99u));
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert(survey_pair_lease_abort_matching(&lease, &new_pair,
                                            new_pair.survey_id));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!survey_pair_lease_abort_matching(&lease, &new_pair,
                                             new_pair.survey_id));
}

static void test_round_bound_abort_cannot_cancel_a_new_generation(void)
{
    const struct survey_pair pair = pair_with(105u, 1u);
    const struct survey_pair_control_id prepare = control_id(105u, 10u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease,
                                           &pair,
                                           8u,
                                           &prepare,
                                           100u,
                                           PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(!survey_pair_lease_abort_matching_round(
        &lease, &pair, pair.survey_id, 7u));
    assert(!survey_pair_lease_abort_matching_round(
        &lease, &pair, pair.survey_id, SURVEY_LEGACY_ROUND_ID));
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert(lease.round_id == 8u);
    assert_pair_equal(&lease.pair, &pair);
    assert(survey_pair_lease_invariant(&lease));

    assert(survey_pair_lease_abort_matching_round(
        &lease, &pair, pair.survey_id, 8u));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(!survey_pair_lease_abort_matching_round(
        &lease, &pair, pair.survey_id, 8u));
    assert(survey_pair_lease_invariant(&lease));
}

static void test_round_mismatch_cannot_retarget_active_lease(void)
{
    const struct survey_pair pair = pair_with(120u, 1u);
    const struct survey_pair other = pair_with(120u, 2u);
    const struct survey_pair_control_id prepare = control_id(120u, 20u);
    const struct survey_pair_control_id start = control_id(120u, 21u);
    const struct survey_pair_control_id next = control_id(120u, 22u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease,
                                           &pair,
                                           UINT16_MAX,
                                           &prepare,
                                           UINT32_MAX - 50u,
                                           100u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round(&lease,
                                         &pair,
                                         UINT16_MAX - 1u,
                                         &start,
                                         UINT32_MAX - 49u) ==
           SURVEY_PAIR_LEASE_INVALID_STATE);
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert(lease.round_id == UINT16_MAX);
    assert(survey_pair_lease_start_round(&lease,
                                         &other,
                                         UINT16_MAX,
                                         &start,
                                         UINT32_MAX - 49u) ==
           SURVEY_PAIR_LEASE_INVALID_STATE);
    assert(survey_pair_lease_start_round(&lease,
                                         &pair,
                                         UINT16_MAX,
                                         &start,
                                         UINT32_MAX - 49u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_prepare_round(&lease,
                                           &other,
                                           UINT16_MAX - 1u,
                                           &next,
                                           UINT32_MAX - 48u,
                                           100u) ==
           SURVEY_PAIR_LEASE_BUSY);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_start_execution_deadline_is_closed_and_does_not_slide(void)
{
    const struct survey_pair pair = pair_with(140u, 1u);
    const struct survey_pair_control_id prepare = control_id(140u, 40u);
    const struct survey_pair_control_id start = control_id(140u, 41u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease, &pair, 12u, &prepare,
                                           100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 12u, NULL, &start, 101u, 300u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 12u, NULL, &start, 102u, 400u) ==
           SURVEY_PAIR_LEASE_DUPLICATE);
    assert(lease.start_execution_deadline_ms == 300u);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_execution_remaining_ms(&lease, 299u) == 1u);
    assert(!survey_pair_lease_mark_running_at(&lease, 299u, NULL, NULL));
    assert(survey_pair_lease_mark_running_at(&lease, 300u, NULL, NULL));
    assert(survey_pair_lease_finish(&lease));
    assert(survey_pair_lease_invariant(&lease));
}

static void test_start_execution_deadline_zero_is_valid_at_wrap(void)
{
    const struct survey_pair pair = pair_with(150u, 1u);
    const struct survey_pair_control_id prepare = control_id(150u, 50u);
    const struct survey_pair_control_id start = control_id(150u, 51u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease, &pair, 13u, &prepare,
                                           UINT32_MAX - 100u, 200u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 13u, NULL, &start,
               UINT32_MAX - 2u, 0u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(lease.start_execution_armed);
    assert(lease.start_execution_deadline_ms == 0u);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_execution_remaining_ms(
               &lease, UINT32_MAX) == 1u);
    assert(!survey_pair_lease_mark_running_at(
        &lease, UINT32_MAX, NULL, NULL));
    assert(survey_pair_lease_mark_running_at(&lease, 0u, NULL, NULL));
    assert(survey_pair_lease_finish(&lease));
    assert(survey_pair_lease_invariant(&lease));
}

static void test_start_execution_lateness_is_bounded(void)
{
    const struct survey_pair pair = pair_with(160u, 1u);
    const struct survey_pair_control_id prepare = control_id(160u, 60u);
    const struct survey_pair_control_id start = control_id(160u, 61u);
    const struct survey_pair_control_id prepare_wrap = control_id(160u, 62u);
    const struct survey_pair_control_id start_wrap = control_id(160u, 63u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease, &pair, 14u, &prepare,
                                           100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 14u, NULL, &start, 101u, 300u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_mark_running_at(
        &lease,
        300u + SURVEY_PAIR_START_SKEW_MARGIN_MS - 1u,
        NULL,
        NULL));
    assert(survey_pair_lease_finish(&lease));

    assert(survey_pair_lease_prepare_round(&lease, &pair, 14u,
                                           &prepare_wrap,
                                           UINT32_MAX - 1100u,
                                           PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 14u, NULL, &start_wrap,
               UINT32_MAX - 1000u,
               UINT32_MAX - 500u) == SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &start_wrap));
    assert(!survey_pair_lease_mark_running_at(
        &lease,
        (UINT32_MAX - 500u) + SURVEY_PAIR_START_SKEW_MARGIN_MS,
        NULL,
        NULL));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
    assert(survey_pair_lease_invariant(&lease));
}

static void test_responder_arms_before_the_shared_initiator_deadline(void)
{
    const struct survey_pair pair = pair_with(170u, 1u);
    const struct survey_pair_control_id prepare = control_id(170u, 70u);
    const struct survey_pair_control_id start = control_id(170u, 71u);
    const struct survey_pair_control_id prepare_wrap = control_id(170u, 72u);
    const struct survey_pair_control_id start_wrap = control_id(170u, 73u);
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round(&lease, &pair, 15u, &prepare,
                                           100u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 15u, NULL, &start, 101u, 1300u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &start));
    assert(survey_pair_lease_execution_remaining_for_role_ms(
               &lease, 299u, true) == 1u);
    assert(survey_pair_lease_execution_remaining_for_role_ms(
               &lease, 299u, false) == 1001u);
    assert(!survey_pair_lease_mark_running_for_role_at(
        &lease, 299u, true, NULL, NULL));
    assert(survey_pair_lease_mark_running_for_role_at(
        &lease, 300u, true, NULL, NULL));
    assert(survey_pair_lease_finish(&lease));

    assert(survey_pair_lease_prepare_round(
               &lease, &pair, 15u, &prepare_wrap,
               UINT32_MAX - 1200u, PREPARE_LEASE_MS) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_start_round_bound_at(
               &lease, &pair, 15u, NULL, &start_wrap,
               UINT32_MAX - 1100u, 500u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(survey_pair_lease_release_start(&lease, &start_wrap));
    assert(survey_pair_lease_execution_remaining_for_role_ms(
               &lease, UINT32_MAX - 500u, true) == 1u);
    assert(!survey_pair_lease_mark_running_for_role_at(
        &lease, UINT32_MAX - 500u, true, NULL, NULL));
    assert(survey_pair_lease_mark_running_for_role_at(
        &lease, UINT32_MAX - 499u, true, NULL, NULL));
    assert(survey_pair_lease_finish(&lease));
    assert(survey_pair_lease_invariant(&lease));
}

int main(void)
{
    test_reset_clears_every_phase();
    test_prepare_duplicate_supersede_and_ordering();
    test_start_is_exact_ordered_transition();
    test_start_pending_expires_if_radio_never_runs();
    test_prepare_duplicate_does_not_rewind_start();
    test_abort_is_bounded_and_idempotent();
    test_wrap_safe_expiry_and_gateway_skip();
    test_argument_and_sequence_wrap_validation();
    test_targeted_abort_cannot_cancel_a_new_pair();
    test_round_bound_abort_cannot_cancel_a_new_generation();
    test_round_mismatch_cannot_retarget_active_lease();
    test_start_execution_deadline_is_closed_and_does_not_slide();
    test_start_execution_deadline_zero_is_valid_at_wrap();
    test_start_execution_lateness_is_bounded();
    test_responder_arms_before_the_shared_initiator_deadline();
    test_operation_generation_and_commitment_isolate_stale_traffic();
    test_operation_generation_rejects_legacy_round_zero();
    test_new_generation_cannot_overwrite_running_radio_owner();
    test_randomized_state_sequence_invariants();
    return 0;
}
