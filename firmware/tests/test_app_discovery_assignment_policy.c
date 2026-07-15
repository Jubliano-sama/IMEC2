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
#define TABLE_SEQ_1 UINT32_C(1001)
#define TABLE_SEQ_2 UINT32_C(1002)
#define TABLE_SEQ_3 UINT32_C(1003)
#define TABLE_FINGERPRINT_1 UINT32_C(0x11111111)
#define TABLE_FINGERPRINT_2 UINT32_C(0x22222222)
#define TABLE_FINGERPRINT_3 UINT32_C(0x33333333)

static void advance_from_epoch_1_to_epoch_2(
    struct app_discovery_assignment_policy *policy)
{
    app_discovery_assignment_policy_init(
        policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(policy->committed_epoch == EPOCH_1);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(app_discovery_assignment_policy_note_claim(policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
}

static void test_rebooted_assignment_late_claims_new_epoch(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_PROVISIONED);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);
    assert(!policy.claim_observed);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
    assert(policy.committed_epoch == EPOCH_2);
}

static void test_out_of_order_old_table_cannot_interrupt_join(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.joining_epoch == EPOCH_2);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
}

static void test_retired_epoch_cannot_roll_back_committed_assignment(void)
{
    struct app_discovery_assignment_policy policy;

    advance_from_epoch_1_to_epoch_2(&policy);
    assert(policy.retired_epoch == EPOCH_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_2);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_3) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_3, TABLE_SEQ_3, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
}

static void test_erased_nvs_production_anchor_is_unprovisioned(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, false, false, 0u, 0u, 0u);
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(strcmp(app_discovery_assignment_provisioning_state_name(
                      app_discovery_assignment_policy_provisioning_state(&policy)),
                  "UNPROVISIONED") == 0);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(policy.committed_epoch == 0u);
    assert(!app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
}

static void test_authoritative_omission_is_unprovisioned_until_reassigned(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_2) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &policy, EPOCH_2, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
    assert(app_discovery_assignment_policy_provisioning_state(&policy) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(&policy));
    assert(policy.retired_epoch == EPOCH_1);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_2);
    assert(policy.committed_table_seq == TABLE_SEQ_2);
    assert(policy.committed_table_fingerprint == TABLE_FINGERPRINT_2);
}

static void test_same_epoch_table_generations_never_roll_back(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2));

    /* A repeated claim must not erase the committed generation watermark. */
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(policy.committed_epoch == EPOCH_1);
    assert(policy.committed_table_seq == TABLE_SEQ_2);
    assert(policy.committed_table_fingerprint == TABLE_FINGERPRINT_2);
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(&policy));

    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3));
}

static void test_joining_generation_is_monotonic_and_fingerprint_bound(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, false, false, 0u, 0u, 0u);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3));
}

static void test_committed_generation_rejects_same_seq_conflicts(void)
{
    struct app_discovery_assignment_policy policy;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);

    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2));
}

static void test_unassigned_generation_survives_reboot_and_blocks_replay(void)
{
    struct app_discovery_assignment_policy policy;
    struct app_discovery_assignment_policy restored;

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &policy, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2));

    app_discovery_assignment_policy_init(
        &restored, true, false, policy.committed_epoch,
        policy.committed_table_seq, policy.committed_table_fingerprint);
    assert(app_discovery_assignment_policy_provisioning_state(&restored) ==
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED);
    assert(restored.committed_epoch == EPOCH_1);
    assert(restored.committed_table_seq == TABLE_SEQ_2);
    assert(restored.committed_table_fingerprint == TABLE_FINGERPRINT_2);

    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_1, TABLE_SEQ_1, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(restored.committed_table_seq == TABLE_SEQ_2);
    assert(!app_discovery_assignment_policy_normal_click_reply_allowed(
        &restored));

    /* An exact retransmission preserves the tombstone idempotently. */
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_note_unassigned(
        &restored, EPOCH_1, TABLE_SEQ_2, TABLE_FINGERPRINT_2));

    assert(app_discovery_assignment_policy_note_claim(&restored, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &restored, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &restored, EPOCH_1, TABLE_SEQ_3, TABLE_FINGERPRINT_3));
    assert(app_discovery_assignment_policy_normal_click_reply_allowed(
        &restored));
}

static void test_table_generation_order_is_wrap_safe(void)
{
    struct app_discovery_assignment_policy policy;

    assert(app_discovery_assignment_table_seq_newer(1u, UINT32_MAX));
    assert(!app_discovery_assignment_table_seq_newer(UINT32_MAX, 1u));
    assert(!app_discovery_assignment_table_seq_newer(
        UINT32_C(0x80000001), 1u));
    assert(!app_discovery_assignment_table_seq_newer(1u, 1u));

    app_discovery_assignment_policy_init(
        &policy, true, true, EPOCH_1, UINT32_MAX, TABLE_FINGERPRINT_1);
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, 1u, TABLE_FINGERPRINT_2) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(app_discovery_assignment_policy_commit(
        &policy, EPOCH_1, 1u, TABLE_FINGERPRINT_2));
    assert(app_discovery_assignment_policy_note_claim(&policy, EPOCH_1) ==
           APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND);
    assert(app_discovery_assignment_policy_note_table(
               &policy, EPOCH_1, UINT32_MAX, TABLE_FINGERPRINT_1) ==
           APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
}

static void test_connected_anchor_low_power_policy_uses_idle(void)
{
    assert(app_radio_low_power_mode_for_connection(true) ==
           APP_RADIO_LOW_POWER_IDLE);
    assert(app_radio_low_power_mode_for_connection(false) ==
           APP_RADIO_LOW_POWER_STANDBY);
}

static void test_gateway_operation_deadline_is_wrap_safe_and_terminal(void)
{
    assert(!app_discovery_assignment_operation_expired(999u, 1000u));
    assert(app_discovery_assignment_operation_expired(1000u, 1000u));
    assert(app_discovery_assignment_operation_expired(1001u, 1000u));
    assert(!app_discovery_assignment_operation_expired(UINT32_MAX - 2u, 3u));
    assert(app_discovery_assignment_operation_expired(3u, 3u));
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
    test_rebooted_assignment_late_claims_new_epoch();
    test_out_of_order_old_table_cannot_interrupt_join();
    test_retired_epoch_cannot_roll_back_committed_assignment();
    test_erased_nvs_production_anchor_is_unprovisioned();
    test_authoritative_omission_is_unprovisioned_until_reassigned();
    test_same_epoch_table_generations_never_roll_back();
    test_joining_generation_is_monotonic_and_fingerprint_bound();
    test_committed_generation_rejects_same_seq_conflicts();
    test_unassigned_generation_survives_reboot_and_blocks_replay();
    test_table_generation_order_is_wrap_safe();
    test_connected_anchor_low_power_policy_uses_idle();
    test_gateway_operation_deadline_is_wrap_safe_and_terminal();
    test_explicit_budget_scales_complete_claim_rounds();
    test_table_retry_budget_preserves_every_ack_window();
    test_stale_publish_work_cannot_run_in_next_assignment();
    test_failed_publish_submission_releases_generation();
    test_low_power_failure_recovers_and_retries_once();
    test_low_power_recovery_retry_can_complete();
    test_low_power_recovery_failure_does_not_retry();
    puts("app discovery assignment policy tests passed");
    return 0;
}
