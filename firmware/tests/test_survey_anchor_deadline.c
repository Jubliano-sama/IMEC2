#include "survey_anchor_deadline.h"
#include "survey_pair_lease.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static void test_phase_safety_survives_later_result_delivery(void)
{
    struct survey_anchor_deadline_registry registry;
    struct survey_anchor_deadline_events events;
    uint32_t delay_ms = UINT32_MAX;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
               UINT64_C(42), 100u, 2u) == 0);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
               0u, 100u, 50u) == 0);
    assert(survey_anchor_deadline_next(&registry, 100u, &delay_ms));
    assert(delay_ms == 2u);

    survey_anchor_deadline_take_due(&registry, 102u, &events);
    assert(survey_anchor_deadline_event_matches(
        &events, SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY, UINT64_C(42)));
    assert(!survey_anchor_deadline_event_matches(
        &events, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY, 0u));
    assert(survey_anchor_deadline_next(&registry, 102u, &delay_ms));
    assert(delay_ms == 48u);
}

static void test_zero_and_wrap_are_valid_deadlines(void)
{
    struct survey_anchor_deadline_registry registry;
    struct survey_anchor_deadline_events events;
    uint32_t delay_ms = UINT32_MAX;
    const uint32_t now_ms = UINT32_MAX - 1u;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
               UINT64_C(7), now_ms, 2u) == 0);
    assert(registry.due_ms[SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY] == 0u);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
               0u, now_ms, 50u) == 0);
    assert(survey_anchor_deadline_next(&registry, now_ms, &delay_ms));
    assert(delay_ms == 2u);

    survey_anchor_deadline_take_due(&registry, 0u, &events);
    assert(survey_anchor_deadline_event_matches(
        &events, SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY, UINT64_C(7)));
    assert(survey_anchor_deadline_next(&registry, 0u, &delay_ms));
    assert(delay_ms == 48u);
}

static void test_cancel_one_owner_retains_another(void)
{
    struct survey_anchor_deadline_registry registry;
    uint32_t delay_ms = UINT32_MAX;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
               0u, 10u, 5u) == 0);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
               UINT64_C(9), 10u, 20u) == 0);
    assert(survey_anchor_deadline_cancel(
        &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY, 0u));
    assert(survey_anchor_deadline_next(&registry, 10u, &delay_ms));
    assert(delay_ms == 20u);
}

static void test_stale_generation_cannot_replace_or_cancel_successor(void)
{
    struct survey_anchor_deadline_registry registry;
    uint32_t delay_ms = UINT32_MAX;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_OPERATION,
               UINT64_C(100), 50u, 30u) == 0);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_OPERATION,
               UINT64_C(101), 50u, 10u) == 0);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_OPERATION,
               UINT64_C(100), 50u, 1u) == -ESTALE);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_OPERATION,
               0u, 50u, 1u) == -ESTALE);
    assert(!survey_anchor_deadline_cancel(
        &registry, SURVEY_ANCHOR_DEADLINE_OPERATION, UINT64_C(100)));
    assert(survey_anchor_deadline_next(&registry, 50u, &delay_ms));
    assert(delay_ms == 10u);
}

static void test_same_owner_retains_earliest_level_trigger(void)
{
    struct survey_anchor_deadline_registry registry;
    uint32_t delay_ms = UINT32_MAX;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
               0u, 100u, 5u) == 0);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
               0u, 100u, 50u) == 0);
    assert(survey_anchor_deadline_next(&registry, 100u, &delay_ms));
    assert(delay_ms == 5u);
}

static void test_duration_outside_wrap_horizon_is_rejected(void)
{
    struct survey_anchor_deadline_registry registry;

    survey_anchor_deadline_registry_init(&registry);
    assert(survey_anchor_deadline_schedule_after(
               &registry, SURVEY_ANCHOR_DEADLINE_OPERATION,
               UINT64_C(1), 0u, (uint32_t)INT32_MAX + 1u) == -EINVAL);
}

static bool fire_exact_cleanup(struct survey_pair_lease *lease,
                               uint64_t callback_generation,
                               uint32_t now_ms)
{
    return lease->pair.operation_generation == callback_generation &&
           survey_pair_lease_expire(lease, now_ms);
}

static void test_cleanup_executor_is_independent_and_generation_exact(void)
{
    const uint64_t old_generation = UINT64_C(0x100000001);
    const uint64_t new_generation = old_generation + 1u;
    const struct survey_pair_control_id old_control = {
        .session_id = 1u,
        .command_seq = 1u,
    };
    const struct survey_pair_control_id new_control = {
        .session_id = 2u,
        .command_seq = 1u,
    };
    const uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0x5au};
    struct survey_pair pair = {
        .operation_generation = old_generation,
        .survey_id = 9u,
        .initiator_id = UINT64_C(0x11),
        .responder_id = UINT64_C(0x22),
        .sample_count = 1u,
    };
    struct survey_pair_lease lease = {0};

    assert(survey_pair_lease_prepare_round_bound(
               &lease, &pair, 1u, commitment, &old_control, 100u, 5u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    /* The private RF queue may remain blocked; the cleanup executor still fires. */
    assert(fire_exact_cleanup(&lease, old_generation, 106u));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);

    pair.operation_generation = new_generation;
    assert(survey_pair_lease_prepare_round_bound(
               &lease, &pair, 2u, commitment, &new_control, 200u, 5u) ==
           SURVEY_PAIR_LEASE_ACCEPTED);
    assert(!fire_exact_cleanup(&lease, old_generation, 206u));
    assert(lease.phase == SURVEY_PAIR_LEASE_PREPARED);
    assert(fire_exact_cleanup(&lease, new_generation, 206u));
    assert(lease.phase == SURVEY_PAIR_LEASE_IDLE);
}

int main(void)
{
    test_phase_safety_survives_later_result_delivery();
    test_zero_and_wrap_are_valid_deadlines();
    test_cancel_one_owner_retains_another();
    test_stale_generation_cannot_replace_or_cancel_successor();
    test_same_owner_retains_earliest_level_trigger();
    test_duration_outside_wrap_horizon_is_rejected();
    test_cleanup_executor_is_independent_and_generation_exact();
    puts("survey anchor deadline tests passed");
    return 0;
}
