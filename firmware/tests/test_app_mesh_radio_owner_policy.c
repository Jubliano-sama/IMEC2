#include "app_mesh_radio_owner_policy.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static void assert_owner_lease_equal(
    const struct app_mesh_radio_owner_lease *lhs,
    const struct app_mesh_radio_owner_lease *rhs)
{
    assert(lhs->generation == rhs->generation);
    assert(lhs->client == rhs->client);
}

static void assert_handoff_lease_equal(
    const struct app_mesh_radio_owner_handoff_lease *lhs,
    const struct app_mesh_radio_owner_handoff_lease *rhs)
{
    assert(lhs->generation == rhs->generation);
    assert(lhs->identity == rhs->identity);
}

static void release_owner(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_lease *lease)
{
    assert(app_mesh_radio_owner_policy_release_begin(policy, lease) == 0);
    assert(app_mesh_radio_owner_policy_release_complete(policy, lease) == 0);
}

static void grant_handoff(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease)
{
    assert(app_mesh_radio_owner_policy_handoff_begin(policy, lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               policy, lease, true) == 0);
}

static void test_claim_supports_every_runtime_client(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    uint32_t previous_generation = 0u;

    app_mesh_radio_owner_policy_reset(&policy);
    for (enum app_mesh_radio_client client =
             APP_MESH_RADIO_CLIENT_CLICKER;
         client < APP_MESH_RADIO_CLIENT_COUNT;
         client++) {
        struct app_mesh_radio_owner_lease lease = {0};
        struct app_mesh_radio_owner_lease snapshot = {0};

        assert(app_mesh_radio_owner_policy_try_claim(
                   &policy, client, &lease) == 0);
        assert(lease.generation != 0u);
        assert(lease.generation != previous_generation);
        assert(lease.client == client);
        assert(app_mesh_radio_owner_policy_busy(&policy));
        assert(app_mesh_radio_owner_policy_phase(&policy) ==
               APP_MESH_RADIO_OWNER_ACTIVE);
        assert(app_mesh_radio_owner_policy_claim_snapshot(
            &policy, &snapshot));
        assert_owner_lease_equal(&snapshot, &lease);

        release_owner(&policy, &lease);
        assert(!app_mesh_radio_owner_policy_busy(&policy));
        assert(app_mesh_radio_owner_policy_phase(&policy) ==
               APP_MESH_RADIO_OWNER_IDLE);
        assert(!app_mesh_radio_owner_policy_claim_snapshot(
            &policy, &snapshot));
        previous_generation = lease.generation;
    }
}

static void test_claim_and_two_phase_release_reject_wrong_owners(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_lease first = {0};
    struct app_mesh_radio_owner_lease competing = {0};
    struct app_mesh_radio_owner_lease wrong = {0};

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_MESH_RX, &first) == 0);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_MESH_TX, &competing) ==
           -EBUSY);

    wrong = first;
    wrong.client = APP_MESH_RADIO_CLIENT_MESH_TX;
    assert(app_mesh_radio_owner_policy_release_begin(&policy, &wrong) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, &wrong) ==
           -ESTALE);

    wrong = first;
    wrong.generation++;
    assert(wrong.generation != 0u);
    assert(app_mesh_radio_owner_policy_release_begin(&policy, &wrong) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, &first) ==
           -EINVAL);

    assert(app_mesh_radio_owner_policy_release_begin(&policy, &first) == 0);
    assert(app_mesh_radio_owner_policy_busy(&policy));
    assert(app_mesh_radio_owner_policy_phase(&policy) ==
           APP_MESH_RADIO_OWNER_RELEASING);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_CLICKER, &competing) ==
           -EBUSY);
    assert(app_mesh_radio_owner_policy_release_begin(&policy, &first) ==
           -EALREADY);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, &wrong) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, &first) ==
           0);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, &first) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_release_begin(&policy, &first) ==
           -ESTALE);

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_CLICKER, &competing) == 0);
    assert(competing.generation != first.generation);
    assert(app_mesh_radio_owner_policy_release_begin(&policy, &first) ==
           -ESTALE);
    release_owner(&policy, &competing);
}

static void test_pause_is_token_idempotent_and_blocks_new_claims(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_lease owner = {0};
    struct app_mesh_radio_owner_lease refused = {0};
    struct app_mesh_radio_owner_pause_lease pause = {0};
    struct app_mesh_radio_owner_pause_lease duplicate;
    struct app_mesh_radio_owner_pause_lease competitor = {0};
    struct app_mesh_radio_owner_pause_lease stale;
    struct app_mesh_radio_owner_pause_lease renewed = {0};

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_ANCHOR_SCAN, &owner) == 0);
    assert(app_mesh_radio_owner_policy_pause(&policy, &pause) == 0);
    assert(pause.generation != 0u);
    assert(app_mesh_radio_owner_policy_paused(&policy));

    duplicate = pause;
    assert(app_mesh_radio_owner_policy_pause(&policy, &duplicate) == 0);
    assert(duplicate.generation == pause.generation);
    assert(app_mesh_radio_owner_policy_pause(&policy, &competitor) ==
           -EBUSY);

    stale = pause;
    stale.generation++;
    assert(stale.generation != 0u);
    assert(app_mesh_radio_owner_policy_pause(&policy, &stale) == -ESTALE);
    assert(app_mesh_radio_owner_policy_resume(&policy, &stale) == -ESTALE);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_SURVEY, &refused) == -EBUSY);

    release_owner(&policy, &owner);
    assert(app_mesh_radio_owner_policy_paused(&policy));
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_SURVEY, &refused) == -EBUSY);
    assert(app_mesh_radio_owner_policy_resume(&policy, &pause) == 0);
    assert(!app_mesh_radio_owner_policy_paused(&policy));
    assert(app_mesh_radio_owner_policy_resume(&policy, &pause) == -ESTALE);

    assert(app_mesh_radio_owner_policy_pause(&policy, &renewed) == 0);
    assert(renewed.generation != pause.generation);
    assert(app_mesh_radio_owner_policy_resume(&policy, &pause) == -ESTALE);
    assert(app_mesh_radio_owner_policy_paused(&policy));
    assert(app_mesh_radio_owner_policy_pause(&policy, &pause) == -ESTALE);
    assert(app_mesh_radio_owner_policy_resume(&policy, &renewed) == 0);
}

static void test_handoff_owns_wait_schedule_and_grant_phases(void)
{
    static int first_work;
    static int competing_work;
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_handoff_lease handoff = {0};
    struct app_mesh_radio_owner_handoff_lease duplicate = {0};
    struct app_mesh_radio_owner_handoff_lease snapshot = {0};
    struct app_mesh_radio_owner_handoff_lease competing = {0};
    struct app_mesh_radio_owner_handoff_lease stale;
    uintptr_t first_identity = (uintptr_t)&first_work;
    uintptr_t competing_identity = (uintptr_t)&competing_work;

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, first_identity, &handoff) == 0);
    assert(handoff.generation != 0u);
    assert(handoff.identity == first_identity);
    assert(app_mesh_radio_owner_policy_handoff_waiting(&policy));
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY);
    assert(app_mesh_radio_owner_policy_handoff_snapshot(
        &policy, &snapshot));
    assert_handoff_lease_equal(&snapshot, &handoff);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, first_identity, &duplicate) == 0);
    assert_handoff_lease_equal(&duplicate, &handoff);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, competing_identity, &competing) == -EBUSY);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &handoff) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &handoff, true) == -EINVAL);

    assert(app_mesh_radio_owner_policy_handoff_begin(&policy, &handoff) == 0);
    assert(!app_mesh_radio_owner_policy_handoff_waiting(&policy));
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_SCHEDULING);
    assert(app_mesh_radio_owner_policy_handoff_begin(&policy, &handoff) ==
           -EALREADY);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, first_identity, &duplicate) == 0);
    assert_handoff_lease_equal(&duplicate, &handoff);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, competing_identity, &competing) == -EBUSY);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &handoff) == -EINVAL);

    stale = handoff;
    stale.generation++;
    assert(stale.generation != 0u);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &stale, true) == -ESTALE);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &handoff, true) == 0);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_GRANTED);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, first_identity, &duplicate) == 0);
    assert_handoff_lease_equal(&duplicate, &handoff);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, competing_identity, &competing) == -EBUSY);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &handoff, true) == -EALREADY);

    stale = handoff;
    stale.identity = competing_identity;
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &stale) == -ESTALE);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_GRANTED);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &handoff) == 0);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_IDLE);
    assert(!app_mesh_radio_owner_policy_handoff_snapshot(
        &policy, &snapshot));
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &handoff) == -ESTALE);
}

static void test_handoff_failure_cancel_and_stale_worker_paths(void)
{
    static int work;
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_handoff_lease wait_lease = {0};
    struct app_mesh_radio_owner_handoff_lease scheduling_lease = {0};
    struct app_mesh_radio_owner_handoff_lease granted_lease = {0};
    struct app_mesh_radio_owner_handoff_lease failed_lease = {0};
    struct app_mesh_radio_owner_handoff_lease renewed = {0};
    uintptr_t identity = (uintptr_t)&work;

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &wait_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &wait_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &wait_lease) == -ESTALE);

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &scheduling_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_begin(
               &policy, &scheduling_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &scheduling_lease) == 0);

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &granted_lease) == 0);
    grant_handoff(&policy, &granted_lease);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &granted_lease) == 0);

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &failed_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_begin(
               &policy, &failed_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &failed_lease, false) == 0);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &renewed) == -EAGAIN);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &failed_lease) == -EBUSY);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, &failed_lease, false) == -EALREADY);
    assert(app_mesh_radio_owner_policy_handoff_failure_complete(
               &policy, &failed_lease) == 0);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_IDLE);
    assert(app_mesh_radio_owner_policy_handoff_failure_complete(
               &policy, &failed_lease) == -ESTALE);

    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, identity, &renewed) == 0);
    assert(renewed.generation != granted_lease.generation);
    grant_handoff(&policy, &renewed);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &granted_lease) == -ESTALE);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &failed_lease) == -ESTALE);
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_GRANTED);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, &renewed) == 0);
}

static void test_abort_leases_are_independent_and_exact(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_abort_lease first = {0};
    struct app_mesh_radio_owner_abort_lease duplicate;
    struct app_mesh_radio_owner_abort_lease second = {0};
    struct app_mesh_radio_owner_abort_lease wrong;
    struct app_mesh_radio_owner_abort_lease renewed = {0};

    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, &first) == 0);
    assert(first.token != 0u);
    assert(first.kind == APP_MESH_RADIO_ABORT_HOST_COMMAND);
    assert(app_mesh_radio_owner_policy_abort_pending(&policy));
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 1u);

    duplicate = first;
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, &duplicate) == 0);
    assert(duplicate.token == first.token);
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 1u);

    wrong = first;
    wrong.kind = APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY;
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &wrong) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY, &first) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 1u);

    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, &second) == 0);
    assert(second.token != first.token);
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 2u);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &first) == 0);
    assert(app_mesh_radio_owner_policy_abort_pending(&policy));
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 1u);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &first) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, &first) ==
           -ESTALE);

    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, &renewed) == 0);
    assert(renewed.token != first.token);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &first) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 2u);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &second) == 0);
    assert(app_mesh_radio_owner_policy_abort_pending(&policy));
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &renewed) == 0);
    assert(!app_mesh_radio_owner_policy_abort_pending(&policy));
}

static void test_abort_pool_keeps_pending_until_final_cause_releases(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_abort_lease
        leases[APP_MESH_RADIO_ABORT_LEASE_CAPACITY] = {0};
    struct app_mesh_radio_owner_abort_lease overflow = {0};
    const enum app_mesh_radio_abort_kind kinds[] = {
        APP_MESH_RADIO_ABORT_HOST_COMMAND,
        APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY,
        APP_MESH_RADIO_ABORT_INLINE_CONTROL,
        APP_MESH_RADIO_ABORT_TRANSPORT_PAUSE,
        APP_MESH_RADIO_ABORT_SURVEY_ABORT,
    };

    for (size_t i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        assert(app_mesh_radio_owner_policy_abort_request(
                   &policy, kinds[i], &leases[i]) == 0);
    }
    for (size_t i = sizeof(kinds) / sizeof(kinds[0]);
         i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY;
         i++) {
        assert(app_mesh_radio_owner_policy_abort_request(
                   &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND,
                   &leases[i]) == 0);
    }

    assert(app_mesh_radio_owner_policy_abort_count(&policy) ==
           APP_MESH_RADIO_ABORT_LEASE_CAPACITY);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_SURVEY_ABORT, &overflow) ==
           -ENOSPC);
    assert(overflow.token == 0u);

    for (size_t i = 0u; i + 1u < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
        assert(app_mesh_radio_owner_policy_abort_release(
                   &policy, &leases[i]) == 0);
        assert(app_mesh_radio_owner_policy_abort_pending(&policy));
        assert(app_mesh_radio_owner_policy_abort_count(&policy) ==
               APP_MESH_RADIO_ABORT_LEASE_CAPACITY - i - 1u);
    }
    assert(app_mesh_radio_owner_policy_abort_release(
               &policy,
               &leases[APP_MESH_RADIO_ABORT_LEASE_CAPACITY - 1u]) == 0);
    assert(!app_mesh_radio_owner_policy_abort_pending(&policy));
    assert(app_mesh_radio_owner_policy_abort_count(&policy) == 0u);
}

static void test_shared_generation_allocator_skips_zero_and_live_tokens(void)
{
    static int work;
    struct app_mesh_radio_owner_policy policy = {
        .next_generation = UINT32_MAX,
    };
    struct app_mesh_radio_owner_lease owner = {0};
    struct app_mesh_radio_owner_pause_lease pause = {0};
    struct app_mesh_radio_owner_handoff_lease handoff = {0};
    struct app_mesh_radio_owner_abort_lease abort = {0};

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_CLICKER, &owner) == 0);
    assert(owner.generation == 1u);

    policy.next_generation = UINT32_MAX;
    assert(app_mesh_radio_owner_policy_pause(&policy, &pause) == 0);
    assert(pause.generation == 2u);
    policy.next_generation = UINT32_MAX;
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, (uintptr_t)&work, &handoff) == 0);
    assert(handoff.generation == 3u);
    policy.next_generation = UINT32_MAX;
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_INLINE_CONTROL, &abort) == 0);
    assert(abort.token == 4u);

    assert(owner.generation != pause.generation);
    assert(owner.generation != handoff.generation);
    assert(owner.generation != abort.token);
    assert(pause.generation != handoff.generation);
    assert(pause.generation != abort.token);
    assert(handoff.generation != abort.token);

    release_owner(&policy, &owner);
    assert(app_mesh_radio_owner_policy_resume(&policy, &pause) == 0);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &handoff) == 0);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &abort) == 0);
}

static void test_reset_invalidates_every_live_lease(void)
{
    static int work;
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_lease owner = {0};
    struct app_mesh_radio_owner_lease next_owner = {0};
    struct app_mesh_radio_owner_pause_lease pause = {0};
    struct app_mesh_radio_owner_handoff_lease handoff = {0};
    struct app_mesh_radio_owner_abort_lease abort = {0};
    uint32_t cursor;

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_HIGH_DEBUG, &owner) == 0);
    assert(app_mesh_radio_owner_policy_pause(&policy, &pause) == 0);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, (uintptr_t)&work, &handoff) == 0);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_TRANSPORT_PAUSE, &abort) == 0);
    cursor = policy.next_generation;

    app_mesh_radio_owner_policy_reset(&policy);
    assert(policy.next_generation == cursor);
    assert(!app_mesh_radio_owner_policy_busy(&policy));
    assert(!app_mesh_radio_owner_policy_paused(&policy));
    assert(app_mesh_radio_owner_policy_handoff_phase(&policy) ==
           APP_MESH_RADIO_HANDOFF_IDLE);
    assert(!app_mesh_radio_owner_policy_abort_pending(&policy));

    assert(app_mesh_radio_owner_policy_release_begin(&policy, &owner) ==
           -ESTALE);
    assert(app_mesh_radio_owner_policy_resume(&policy, &pause) == -ESTALE);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, &handoff) == -ESTALE);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, &abort) ==
           -ESTALE);

    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_HIGH_DEBUG, &next_owner) == 0);
    assert(next_owner.generation != owner.generation);
    release_owner(&policy, &next_owner);
}

static void test_invalid_and_null_arguments_fail_closed(void)
{
    static int work;
    struct app_mesh_radio_owner_policy policy = {0};
    struct app_mesh_radio_owner_lease owner = {0};
    struct app_mesh_radio_owner_pause_lease pause = {0};
    struct app_mesh_radio_owner_handoff_lease handoff = {0};
    struct app_mesh_radio_owner_abort_lease abort = {0};

    app_mesh_radio_owner_policy_reset(NULL);
    assert(!app_mesh_radio_owner_policy_busy(NULL));
    assert(app_mesh_radio_owner_policy_phase(NULL) ==
           APP_MESH_RADIO_OWNER_IDLE);
    assert(!app_mesh_radio_owner_policy_claim_snapshot(NULL, &owner));
    assert(!app_mesh_radio_owner_policy_claim_snapshot(&policy, NULL));
    assert(!app_mesh_radio_owner_policy_paused(NULL));
    assert(!app_mesh_radio_owner_policy_handoff_waiting(NULL));
    assert(app_mesh_radio_owner_policy_handoff_phase(NULL) ==
           APP_MESH_RADIO_HANDOFF_IDLE);
    assert(!app_mesh_radio_owner_policy_handoff_snapshot(NULL, &handoff));
    assert(!app_mesh_radio_owner_policy_handoff_snapshot(&policy, NULL));
    assert(!app_mesh_radio_owner_policy_abort_pending(NULL));
    assert(app_mesh_radio_owner_policy_abort_count(NULL) == 0u);

    assert(app_mesh_radio_owner_policy_try_claim(
               NULL, APP_MESH_RADIO_CLIENT_CLICKER, &owner) == -EINVAL);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_NONE, &owner) == -EINVAL);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_COUNT, &owner) == -EINVAL);
    assert(app_mesh_radio_owner_policy_try_claim(
               &policy, APP_MESH_RADIO_CLIENT_CLICKER, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_release_begin(&policy, NULL) ==
           -EINVAL);
    assert(app_mesh_radio_owner_policy_release_complete(&policy, NULL) ==
           -EINVAL);

    assert(app_mesh_radio_owner_policy_pause(NULL, &pause) == -EINVAL);
    assert(app_mesh_radio_owner_policy_pause(&policy, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_resume(&policy, NULL) == -EINVAL);

    assert(app_mesh_radio_owner_policy_handoff_request(
               NULL, (uintptr_t)&work, &handoff) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, (uintptr_t)0, &handoff) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_request(
               &policy, (uintptr_t)&work, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_begin(&policy, NULL) ==
           -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_schedule_complete(
               &policy, NULL, false) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_failure_complete(
               &policy, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_take_grant(
               &policy, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_handoff_cancel(
               &policy, NULL) == -EINVAL);

    assert(app_mesh_radio_owner_policy_abort_request(
               NULL, APP_MESH_RADIO_ABORT_HOST_COMMAND, &abort) == -EINVAL);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_NONE, &abort) == -EINVAL);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_KIND_COUNT, &abort) == -EINVAL);
    assert(app_mesh_radio_owner_policy_abort_request(
               &policy, APP_MESH_RADIO_ABORT_HOST_COMMAND, NULL) == -EINVAL);
    assert(app_mesh_radio_owner_policy_abort_release(&policy, NULL) ==
           -EINVAL);
}

static void test_rx_control_aborts_active_scan_and_blocks_rearm(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    bool abort_scan = false;

    assert(app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
    assert(app_mesh_radio_owner_policy_rx_inline_control_begin(
        &policy, &abort_scan));
    assert(abort_scan);
    assert(!app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
    assert(!app_mesh_radio_owner_policy_rx_inline_control_ready(&policy));

    app_mesh_radio_owner_policy_rx_scan_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_inline_control_ready(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));

    app_mesh_radio_owner_policy_rx_inline_control_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
    assert(app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
}

static void test_rx_control_wins_before_scan_acquires_radio(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    bool abort_scan = true;

    assert(app_mesh_radio_owner_policy_rx_inline_control_begin(
        &policy, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_radio_owner_policy_rx_inline_control_ready(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
    assert(!app_mesh_radio_owner_policy_rx_inline_control_begin(
        &policy, &abort_scan));
    app_mesh_radio_owner_policy_rx_inline_control_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
}

static void test_rx_scheduled_control_blocks_scan_until_delivery_ends(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    bool abort_scan = true;

    assert(app_mesh_radio_owner_policy_rx_scheduled_control_request(
        &policy, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_pending(
        &policy));
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_ready(
        &policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));

    assert(app_mesh_radio_owner_policy_rx_scheduled_control_end(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scheduled_control_pending(
        &policy));
    assert(app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
    assert(!app_mesh_radio_owner_policy_rx_scheduled_control_end(&policy));
}

static void test_rx_scheduled_control_aborts_scan_and_coalesces(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    bool abort_scan = false;

    assert(app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_request(
        &policy, &abort_scan));
    assert(abort_scan);
    assert(!app_mesh_radio_owner_policy_rx_scheduled_control_ready(
        &policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_try_begin(&policy));

    app_mesh_radio_owner_policy_rx_scan_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_ready(
        &policy));
    abort_scan = true;
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_request(
        &policy, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_pending(
        &policy));
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_end(&policy));
    assert(app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
}

static void test_rx_inline_control_can_precede_scheduled_control(void)
{
    struct app_mesh_radio_owner_policy policy = {0};
    bool abort_scan = true;

    assert(app_mesh_radio_owner_policy_rx_inline_control_begin(
        &policy, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_request(
        &policy, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_ready(
        &policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));

    app_mesh_radio_owner_policy_rx_inline_control_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_pending(
        &policy));
    assert(!app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
    assert(app_mesh_radio_owner_policy_rx_inline_control_begin(
        &policy, &abort_scan));
    app_mesh_radio_owner_policy_rx_inline_control_end(&policy);
    assert(app_mesh_radio_owner_policy_rx_scheduled_control_end(&policy));
    assert(app_mesh_radio_owner_policy_rx_scan_rearm_allowed(&policy));
}

int main(void)
{
    test_claim_supports_every_runtime_client();
    test_claim_and_two_phase_release_reject_wrong_owners();
    test_pause_is_token_idempotent_and_blocks_new_claims();
    test_handoff_owns_wait_schedule_and_grant_phases();
    test_handoff_failure_cancel_and_stale_worker_paths();
    test_abort_leases_are_independent_and_exact();
    test_abort_pool_keeps_pending_until_final_cause_releases();
    test_shared_generation_allocator_skips_zero_and_live_tokens();
    test_reset_invalidates_every_live_lease();
    test_invalid_and_null_arguments_fail_closed();
    test_rx_control_aborts_active_scan_and_blocks_rearm();
    test_rx_control_wins_before_scan_acquires_radio();
    test_rx_scheduled_control_blocks_scan_until_delivery_ends();
    test_rx_scheduled_control_aborts_scan_and_coalesces();
    test_rx_inline_control_can_precede_scheduled_control();
    return 0;
}
