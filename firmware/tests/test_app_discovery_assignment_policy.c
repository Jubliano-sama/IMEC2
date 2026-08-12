#include "app_discovery_assignment_policy.h"
#include "app_radio_low_power_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPOCH_1 UINT32_C(101)
#define EPOCH_2 UINT32_C(202)
#define EPOCH_3 UINT32_C(303)
#define EPOCH_4 UINT32_C(404)
#define TABLE_SEQ_1 UINT32_C(1001)
#define TABLE_SEQ_2 UINT32_C(1002)
#define TABLE_SEQ_3 UINT32_C(1003)
#define TABLE_SEQ_4 UINT32_C(1004)
static const struct discovery_assignment_table_commitment table_commitment_1 = {
    .bytes = {0x11u},
};
static const struct discovery_assignment_table_commitment table_commitment_2 = {
    .bytes = {0x22u},
};
static const struct discovery_assignment_table_commitment table_commitment_3 = {
    .bytes = {0x33u},
};
static const struct discovery_assignment_table_commitment table_commitment_4 = {
    .bytes = {0x44u},
};

#define TABLE_COMMITMENT_1 (&table_commitment_1)
#define TABLE_COMMITMENT_2 (&table_commitment_2)
#define TABLE_COMMITMENT_3 (&table_commitment_3)
#define TABLE_COMMITMENT_4 (&table_commitment_4)

static struct discovery_assignment_table_commitment
test_table_commitment(uint32_t value)
{
    struct discovery_assignment_table_commitment commitment = {0};

    commitment.bytes[0] = (uint8_t)value;
    commitment.bytes[1] = (uint8_t)(value >> 8u);
    commitment.bytes[2] = (uint8_t)(value >> 16u);
    commitment.bytes[3] = (uint8_t)(value >> 24u);
    return commitment;
}

static void test_response_custody_requires_exact_operation_identity(void)
{
    assert(app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        TABLE_SEQ_1,
        NULL,
        TABLE_SEQ_1,
        NULL));
    assert(!app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        TABLE_SEQ_1,
        NULL,
        TABLE_SEQ_2,
        NULL));
    assert(!app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        0u,
        NULL,
        0u,
        NULL));

    assert(app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1));
    assert(!app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_2));
    assert(!app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1,
        TABLE_SEQ_2,
        TABLE_COMMITMENT_1));
    assert(!app_discovery_assignment_response_identity_matches(
        DISCOVERY_ASSIGNMENT_PHASE_TABLE,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1));
}

static void advance_from_epoch_1_to_epoch_2(
    struct app_discovery_assignment_policy *policy)
{
    app_discovery_assignment_policy_init(
        policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(policy->committed_epoch == EPOCH_1);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(app_discovery_assignment_policy_note_claim(policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
}

static void test_rebooted_assignment_late_claims_new_epoch(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_PROVISIONED);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);
    assert(!policy.claim_observed);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(policy.committed_epoch == EPOCH_2);
}

static void test_out_of_order_old_table_cannot_interrupt_join(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
}

static void test_committed_claim_cannot_erase_newer_join(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(policy.joining_epoch == EPOCH_2);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(policy.joining_epoch == EPOCH_2);
    assert(policy.claim_observed);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
}

static void test_retired_epoch_cannot_roll_back_committed_assignment(void)
{
    struct app_discovery_assignment_policy policy;

    advance_from_epoch_1_to_epoch_2(&policy);
    assert(policy.retired_epoch_count == 1u);
    assert(policy.retired_epochs[0] == EPOCH_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_2);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_3) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_3, TABLE_SEQ_3, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
}

static void test_erased_nvs_production_anchor_is_unprovisioned(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, false, false, false, 0u, 0u, NULL);
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(strcmp(app_discovery_assignment_provisioning_state_name(
                      app_discovery_assignment_policy_provisioning_state(&policy)),
                  "UNPROVISIONED") == 0);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(policy.committed_epoch == 0u);
    assert(!app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
}

static void test_authoritative_omission_is_unprovisioned_until_reassigned(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(policy.retired_epoch_count == 1u);
    assert(policy.retired_epochs[0] == EPOCH_1);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_2);
    assert(policy.committed_table_seq == TABLE_SEQ_2);
    assert(discovery_assignment_table_commitment_equal(
        &policy.committed_table_commitment, TABLE_COMMITMENT_2));
}

static void test_restored_pending_candidate_retains_committed_assignment(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true,
        EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_restore_pending(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(
        &policy));
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(policy.committed_epoch == EPOCH_2);
    assert(policy.retired_epoch_count == 1u);
    assert(policy.retired_epochs[0] == EPOCH_1);
}

static void test_first_assignment_pending_remains_unprovisioned(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, false, false, false, 0u, 0u, NULL);
    assert(app_discovery_assignment_policy_restore_pending(
        &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1));
    assert(policy.ordered_epoch_valid);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(
        &policy));
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
}

static void test_multi_generation_replay_history_survives_reset(void)
{
    struct app_discovery_assignment_policy policy;
    struct app_discovery_assignment_policy restored;
    uint32_t persisted_retired[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP] = {0};
    uint8_t persisted_retired_count = 0u;

    advance_from_epoch_1_to_epoch_2(&policy);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_3) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_3, TABLE_SEQ_3, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_3, TABLE_SEQ_3, TABLE_COMMITMENT_3));
    assert(policy.retired_epoch_count == 2u);
    assert(policy.retired_epochs[0] == EPOCH_2);
    assert(policy.retired_epochs[1] == EPOCH_1);

    assert(app_discovery_assignment_policy_project_retired_epochs(
        &policy, EPOCH_4, persisted_retired, &persisted_retired_count));
    assert(persisted_retired_count == 3u);
    assert(persisted_retired[0] == EPOCH_3);
    assert(persisted_retired[1] == EPOCH_2);
    assert(persisted_retired[2] == EPOCH_1);

    app_discovery_assignment_policy_init(
        &restored, true, true, true, EPOCH_4, TABLE_SEQ_4, TABLE_COMMITMENT_4);
    assert(app_discovery_assignment_policy_restore_retired_epochs(
        &restored, persisted_retired, persisted_retired_count));
    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_3) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);

    /* The current exact generation remains replayable so a lost ACK recovers. */
    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_4) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_4, TABLE_SEQ_4, TABLE_COMMITMENT_4) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_commit(
        &restored, EPOCH_4, TABLE_SEQ_4, TABLE_COMMITMENT_4));

    persisted_retired[2] = EPOCH_2;
    assert(!app_discovery_assignment_policy_restore_retired_epochs(
        &restored, persisted_retired, persisted_retired_count));
}

static void test_ordered_epoch_rejects_stale_generation_after_history_eviction(void)
{
    struct app_discovery_assignment_policy policy;
    struct app_discovery_assignment_policy restored;
    const uint32_t first_epoch = UINT32_C(5000);
    uint32_t current_epoch;
    uint32_t current_table_seq;
    struct discovery_assignment_table_commitment current_commitment;
    struct discovery_assignment_table_commitment initial_commitment =
        test_table_commitment(UINT32_C(0x70000000));
    struct discovery_assignment_table_commitment stale_commitment =
        test_table_commitment(UINT32_C(0x70000000));

    app_discovery_assignment_policy_init(
        &policy,
        true,
        true,
        true,
        first_epoch,
        UINT32_C(7000),
        &initial_commitment);
    for (uint32_t generation = 1u;
         generation <= DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP + 1u;
         generation++) {
        uint32_t epoch = first_epoch + generation;
        uint32_t table_seq = UINT32_C(7000) + generation;
        struct discovery_assignment_table_commitment commitment =
            test_table_commitment(UINT32_C(0x70000000) + generation);

        assert(app_discovery_assignment_policy_note_claim(&policy, epoch) ==
               APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
        assert(app_discovery_assignment_policy_note_table(
                   &policy, epoch, table_seq, &commitment) ==
               APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
        assert(app_discovery_assignment_policy_commit(
            &policy, epoch, table_seq, &commitment));
    }

    assert(policy.retired_epoch_count ==
           DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP);
    for (uint32_t index = 0u;
         index < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         index++) {
        assert(policy.retired_epochs[index] ==
               first_epoch + DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP - index);
    }
    for (uint32_t generation = 1u;
         generation <= DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         generation++) {
        assert(app_discovery_assignment_policy_note_claim(
                   &policy, first_epoch + generation) ==
               APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    }
    assert(!app_discovery_assignment_policy_epoch_retired(
        &policy, first_epoch));
    assert(app_discovery_assignment_policy_note_claim(
               &policy, first_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               first_epoch,
               UINT32_C(7000),
               &stale_commitment) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);

    current_epoch = policy.committed_epoch;
    current_table_seq = policy.committed_table_seq;
    current_commitment = policy.committed_table_commitment;
    app_discovery_assignment_policy_init(
        &restored,
        true,
        true,
        true,
        current_epoch,
        current_table_seq,
        &current_commitment);
    assert(app_discovery_assignment_policy_note_claim(
               &restored, first_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &restored,
               first_epoch,
               UINT32_C(7000),
               &stale_commitment) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
}

static void test_migrated_snapshot_accepts_one_new_scheme_epoch_then_orders(void)
{
    struct app_discovery_assignment_policy policy;
    const uint32_t legacy_random_epoch = UINT32_C(0xf0000000);
    const uint32_t first_ordered_epoch = UINT32_C(7);

    app_discovery_assignment_policy_init(
        &policy,
        true,
        false,
        true,
        legacy_random_epoch,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1);
    assert(!policy.ordered_epoch_valid);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, first_ordered_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               first_ordered_epoch,
               TABLE_SEQ_2,
               TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy,
        first_ordered_epoch,
        TABLE_SEQ_2,
        TABLE_COMMITMENT_2));
    assert(policy.ordered_epoch_valid);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, legacy_random_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
}

static void test_migration_candidate_uses_order_without_legacy_baseline(void)
{
    struct app_discovery_assignment_policy policy;
    const uint32_t delayed_epoch = UINT32_C(6);
    const uint32_t live_epoch = UINT32_C(7);

    app_discovery_assignment_policy_init(
        &policy,
        true,
        false,
        true,
        UINT32_C(0xf0000000),
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, delayed_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, live_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(policy.joining_epoch == live_epoch);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, delayed_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               delayed_epoch,
               TABLE_SEQ_1,
               TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               live_epoch,
               TABLE_SEQ_2,
               TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);

    app_discovery_assignment_policy_init(
        &policy,
        true,
        false,
        true,
        UINT32_C(0xf0000000),
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, live_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, delayed_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(policy.joining_epoch == live_epoch);
}

static void test_migration_ignores_random_retired_epoch_collision(void)
{
    struct app_discovery_assignment_policy policy;
    uint32_t legacy_retired[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP] = {0};
    uint32_t projected_retired[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint8_t projected_retired_count = UINT8_MAX;
    const uint32_t first_ordered_epoch = UINT32_C(7);

    memset(projected_retired, 0xa5, sizeof(projected_retired));
    legacy_retired[0] = first_ordered_epoch;
    app_discovery_assignment_policy_init(
        &policy,
        true,
        false,
        true,
        UINT32_C(0xf0000000),
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_restore_retired_epochs(
        &policy, legacy_retired, 1u));
    assert(app_discovery_assignment_policy_note_claim(
               &policy, first_ordered_epoch) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               first_ordered_epoch,
               TABLE_SEQ_2,
               TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_project_retired_epochs(
        &policy,
        first_ordered_epoch,
        projected_retired,
        &projected_retired_count));
    assert(projected_retired_count == 0u);
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        assert(projected_retired[i] == 0u);
    }
    assert(app_discovery_assignment_policy_commit(
        &policy,
        first_ordered_epoch,
        TABLE_SEQ_2,
        TABLE_COMMITMENT_2));
    assert(policy.ordered_epoch_valid);
    assert(policy.retired_epoch_count == 0u);
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        assert(policy.retired_epochs[i] == 0u);
    }
}

static void test_ordered_epoch_wrap_and_exact_replay(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy,
        true,
        true,
        true,
        UINT32_MAX,
        TABLE_SEQ_1,
        TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(
               &policy, UINT32_MAX) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy,
               UINT32_MAX,
               TABLE_SEQ_1,
               TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_commit(
        &policy, UINT32_MAX, TABLE_SEQ_1, TABLE_COMMITMENT_1));

    assert(app_discovery_assignment_policy_note_claim(&policy, 1u) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, 1u, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, 1u, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(app_discovery_assignment_policy_note_claim(
               &policy, UINT32_MAX) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
}

static void test_replay_history_validation_is_fail_closed_and_canonical(void)
{
    struct app_discovery_assignment_policy policy;
    uint32_t history[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP] = {0};
    uint32_t projected[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint8_t projected_count = 0u;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_4, TABLE_SEQ_4, TABLE_COMMITMENT_4);

    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4,
        history,
        DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP + 1u,
        true));
    assert(app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, NULL, 0u, false));
    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, NULL, 0u, true));
    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, history, 1u, true));
    history[0] = EPOCH_4;
    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, history, 1u, true));
    history[0] = EPOCH_3;
    history[1] = EPOCH_3;
    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, history, 2u, true));

    history[1] = EPOCH_2;
    history[2] = EPOCH_1;
    assert(!app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, history, 2u, true));
    assert(app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, history, 2u, false));
    assert(app_discovery_assignment_policy_restore_retired_epochs(
        &policy, history, 2u));
    assert(policy.retired_epochs[2] == 0u);

    memset(projected, 0xa5, sizeof(projected));
    assert(app_discovery_assignment_policy_project_retired_epochs(
        &policy, EPOCH_4, projected, &projected_count));
    assert(projected_count == 2u);
    assert(app_discovery_assignment_retired_epochs_valid(
        EPOCH_4, projected, projected_count, true));
}

static void test_committed_epoch_allows_exact_table_replay_only(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.committed_table_seq == TABLE_SEQ_1);
    assert(discovery_assignment_table_commitment_equal(
        &policy.committed_table_commitment, TABLE_COMMITMENT_1));
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_3, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1));
}

static void test_joining_table_is_immutable_and_commitment_bound(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, false, false, false, 0u, 0u, NULL);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_3, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1));
}

static void test_committed_generation_rejects_same_seq_conflicts(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_COMMITMENT_2));
}

static void test_unassigned_generation_survives_reboot_and_blocks_replay(void)
{
    struct app_discovery_assignment_policy policy;
    struct app_discovery_assignment_policy restored;

    app_discovery_assignment_policy_init(
        &policy, true, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));

    app_discovery_assignment_policy_init(
        &restored, true, true, false, policy.committed_epoch,
        policy.committed_table_seq, &policy.committed_table_commitment);
    assert(app_discovery_assignment_policy_provisioning_state(&restored) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(restored.committed_epoch == EPOCH_2);
    assert(restored.committed_table_seq == TABLE_SEQ_2);
    assert(discovery_assignment_table_commitment_equal(
        &restored.committed_table_commitment, TABLE_COMMITMENT_2));

    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_1, TABLE_SEQ_1, TABLE_COMMITMENT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(restored.committed_table_seq == TABLE_SEQ_2);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(
        &restored));

    /* An exact retransmission preserves the tombstone idempotently. */
    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &restored, EPOCH_2, TABLE_SEQ_2, TABLE_COMMITMENT_2));

    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_3) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_3, TABLE_SEQ_3, TABLE_COMMITMENT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &restored, EPOCH_3, TABLE_SEQ_3, TABLE_COMMITMENT_3));
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(
        &restored));
}

static void test_connected_anchor_low_power_policy_uses_idle(void)
{
    assert(app_radio_low_power_mode_for_connection(true) ==
           APP_RADIO_LOW_POWER_IDLE);
    assert(app_radio_low_power_mode_for_connection(false) ==
           APP_RADIO_LOW_POWER_STANDBY);
}

static void test_gateway_operation_deadline_is_64_bit_and_terminal(void)
{
    assert(!app_discovery_assignment_operation_expired(999u, 1000u));
    assert(app_discovery_assignment_operation_expired(1000u, 1000u));
    assert(app_discovery_assignment_operation_expired(1001u, 1000u));
    assert(!app_discovery_assignment_operation_expired(
        UINT64_C(0x100000002), UINT64_C(0x100000003)));
    assert(app_discovery_assignment_operation_expired(
        UINT64_C(0x100000003), UINT64_C(0x100000003)));
}

static void test_explicit_budget_scales_complete_claim_rounds(void)
{
    assert(app_discovery_assignment_claim_round_limit(true, 1u, 4u) == 1u);
    assert(app_discovery_assignment_claim_round_limit(true, 3u, 4u) == 3u);
    assert(app_discovery_assignment_claim_round_limit(true, 8u, 4u) == 4u);
    assert(app_discovery_assignment_claim_round_limit(false, 1u, 4u) == 4u);
    assert(app_discovery_assignment_claim_round_limit(true, 0u, 4u) == 0u);
    assert(app_discovery_assignment_claim_round_limit(true, 1u, 0u) == 0u);
}

static void test_table_retry_budget_preserves_every_ack_window(void)
{
    assert(app_discovery_assignment_table_windows_remaining(1u, 4u) == 4u);
    assert(app_discovery_assignment_table_windows_remaining(2u, 4u) == 3u);
    assert(app_discovery_assignment_table_windows_remaining(3u, 4u) == 2u);
    assert(app_discovery_assignment_table_windows_remaining(4u, 4u) == 1u);
    assert(app_discovery_assignment_table_windows_remaining(0u, 4u) == 0u);
    assert(app_discovery_assignment_table_windows_remaining(5u, 4u) == 0u);

    assert(app_discovery_assignment_table_round_after_delivery(
               0u, 4u, false) == 1u);
    assert(app_discovery_assignment_table_round_after_delivery(
               1u, 4u, true) == 1u);
    assert(app_discovery_assignment_table_round_after_delivery(
               1u, 0u, false) == 1u);
    assert(app_discovery_assignment_table_windows_remaining(
               app_discovery_assignment_table_round_after_delivery(
                   1u, 4u, true),
               1u) == 1u);

    assert(app_discovery_assignment_table_retry_backoff_required(
        true, 3u, 1u, 4u));
    assert(app_discovery_assignment_table_retry_backoff_required(
        true, 1u, 3u, 4u));
    assert(!app_discovery_assignment_table_retry_backoff_required(
        true, 1u, 4u, 4u));
    assert(!app_discovery_assignment_table_retry_backoff_required(
        true, 0u, 1u, 4u));
    assert(!app_discovery_assignment_table_retry_backoff_required(
        false, 3u, 1u, 4u));
}

static void test_stale_publish_work_cannot_run_in_next_assignment(void)
{
    struct app_discovery_assignment_work_guard guard;

    app_discovery_assignment_work_guard_init(&guard);
    assert(app_discovery_assignment_work_guard_request(&guard, 1u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT);
    assert(app_discovery_assignment_work_guard_request(&guard, 1u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING);

    /* Assignment 2 starts while assignment 1 still waits for a radio boundary. */
    assert(app_discovery_assignment_work_guard_request(&guard, 2u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_WAIT_STALE);
    assert(!app_discovery_assignment_work_guard_begin(&guard, 2u));

    assert(app_discovery_assignment_work_guard_request(&guard, 2u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT);
    assert(app_discovery_assignment_work_guard_begin(&guard, 2u));
    assert(guard.pending_generation == 0u);
}

static void test_failed_publish_submission_releases_generation(void)
{
    struct app_discovery_assignment_work_guard guard;

    app_discovery_assignment_work_guard_init(&guard);
    assert(app_discovery_assignment_work_guard_request(&guard, 7u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT);
    app_discovery_assignment_work_guard_note_submit_result(&guard, 7u, -1);
    assert(app_discovery_assignment_work_guard_request(&guard, 8u) ==
           APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT);
    app_discovery_assignment_work_guard_note_submit_result(&guard, 7u, -1);
    assert(app_discovery_assignment_work_guard_begin(&guard, 8u));
}

static void test_real_rf_attempt_preserves_original_response_horizon(void)
{
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 2u, 2u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 1u, 2u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        0u, 2u, 2u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        0u, 0u, 0u, 1u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 2u, 2u, 0u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 2u, 2u, 1u, true, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 2u, 2u, 1u, false, true));

    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        2u, 2u, 0u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        2u, 2u, 1u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        2u, 2u, 2u, 1u, false, false));
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        0u, 0u, 0u, 1u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        2u, 2u, 0u, 1u, false, true));
}

static void test_collection_depth_stays_conservative_until_roster_is_known(void)
{
    /*
     * Zero means "unknown/max-depth" to the collection-window calculator.
     * Seeing one direct response cannot prove that an undiscovered deeper
     * responder does not exist.
     */
    assert(app_discovery_assignment_collection_hop_count(
               0u, 1u, 1u) == 0u);
    assert(app_discovery_assignment_collection_hop_count(
               5u, 1u, 1u) == 0u);
    assert(app_discovery_assignment_collection_hop_count(
               5u, 4u, 3u) == 0u);

    /* Once the expected roster is complete, observed depth may shorten it. */
    assert(app_discovery_assignment_collection_hop_count(
               5u, 5u, 3u) == 3u);
    assert(app_discovery_assignment_collection_hop_count(
               5u, 6u, 2u) == 2u);
    assert(app_discovery_assignment_collection_hop_count(
               5u, 5u, 0u) == 0u);
}

static void test_low_power_failure_recovers_and_retries_once(void)
{
    struct app_radio_low_power_policy policy;

    app_radio_low_power_policy_init(&policy, APP_RADIO_LOW_POWER_STANDBY);
    assert(app_radio_low_power_policy_note_transition(&policy, -1) ==
           APP_RADIO_LOW_POWER_RECOVER);
    assert(app_radio_low_power_policy_note_recovery(&policy, 0) ==
           APP_RADIO_LOW_POWER_RETRY);
    assert(app_radio_low_power_policy_note_transition(&policy, -2) ==
           APP_RADIO_LOW_POWER_FAIL);
    assert(policy.transition_attempts == 2u);
    assert(app_radio_low_power_policy_note_transition(&policy, 0) ==
           APP_RADIO_LOW_POWER_FAIL);
}

static void test_low_power_recovery_retry_can_complete(void)
{
    struct app_radio_low_power_policy policy;

    app_radio_low_power_policy_init(&policy, APP_RADIO_LOW_POWER_IDLE);
    assert(app_radio_low_power_policy_note_transition(&policy, -1) ==
           APP_RADIO_LOW_POWER_RECOVER);
    assert(app_radio_low_power_policy_note_recovery(&policy, 0) ==
           APP_RADIO_LOW_POWER_RETRY);
    assert(app_radio_low_power_policy_note_transition(&policy, 0) ==
           APP_RADIO_LOW_POWER_COMPLETE);
    assert(policy.transition_attempts == 2u);
}

static void test_low_power_recovery_failure_does_not_retry(void)
{
    struct app_radio_low_power_policy policy;

    app_radio_low_power_policy_init(&policy, APP_RADIO_LOW_POWER_IDLE);
    assert(app_radio_low_power_policy_note_transition(&policy, -1) ==
           APP_RADIO_LOW_POWER_RECOVER);
    assert(app_radio_low_power_policy_note_recovery(&policy, -2) ==
           APP_RADIO_LOW_POWER_FAIL);
    assert(policy.transition_attempts == 1u);
}

int main(void)
{
    test_response_custody_requires_exact_operation_identity();
    test_rebooted_assignment_late_claims_new_epoch();
    test_out_of_order_old_table_cannot_interrupt_join();
    test_committed_claim_cannot_erase_newer_join();
    test_retired_epoch_cannot_roll_back_committed_assignment();
    test_erased_nvs_production_anchor_is_unprovisioned();
    test_authoritative_omission_is_unprovisioned_until_reassigned();
    test_restored_pending_candidate_retains_committed_assignment();
    test_first_assignment_pending_remains_unprovisioned();
    test_multi_generation_replay_history_survives_reset();
    test_ordered_epoch_rejects_stale_generation_after_history_eviction();
    test_migrated_snapshot_accepts_one_new_scheme_epoch_then_orders();
    test_migration_candidate_uses_order_without_legacy_baseline();
    test_migration_ignores_random_retired_epoch_collision();
    test_ordered_epoch_wrap_and_exact_replay();
    test_replay_history_validation_is_fail_closed_and_canonical();
    test_committed_epoch_allows_exact_table_replay_only();
    test_joining_table_is_immutable_and_commitment_bound();
    test_committed_generation_rejects_same_seq_conflicts();
    test_unassigned_generation_survives_reboot_and_blocks_replay();
    test_connected_anchor_low_power_policy_uses_idle();
    test_gateway_operation_deadline_is_64_bit_and_terminal();
    test_explicit_budget_scales_complete_claim_rounds();
    test_table_retry_budget_preserves_every_ack_window();
    test_stale_publish_work_cannot_run_in_next_assignment();
    test_failed_publish_submission_releases_generation();
    test_real_rf_attempt_preserves_original_response_horizon();
    test_collection_depth_stays_conservative_until_roster_is_known();
    test_low_power_failure_recovers_and_retries_once();
    test_low_power_recovery_retry_can_complete();
    test_low_power_recovery_failure_does_not_retry();
    puts("app discovery assignment policy tests passed");
    return 0;
}
