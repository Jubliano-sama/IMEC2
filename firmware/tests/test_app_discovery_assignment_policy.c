#include "app_anchor_assignment_command.h"
#include "app_anchor_assignment_policy.h"
#include "app_anchor_host_command_policy.h"
#include "app_anchor_radio_policy.h"
#include "app_discovery_assignment_policy.h"
#include "app_radio_low_power_policy.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include "operation_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <errno.h>
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
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
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
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
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
           APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY);
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

static void test_semantic_quorum_overrides_only_redundant_tail_failure(void)
{
    assert(app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 2u, 2u, 1u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        2u, 1u, 2u, 1u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
        0u, 2u, 2u, 1u, false, false));
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
    assert(!app_discovery_assignment_semantic_terminal_success(
        APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
        2u, 2u, 2u, 1u, false, false));
    assert(!app_discovery_assignment_semantic_terminal_success(
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

static void test_anchor_assignment_state_arithmetic(void)
{
    assert(app_anchor_assignment_normalize_epoch(0u) == 1u);
    assert(app_anchor_assignment_normalize_epoch(42u) == 42u);
    assert(app_anchor_assignment_next_nonzero(0u) == 1u);
    assert(app_anchor_assignment_next_nonzero(41u) == 42u);
    assert(app_anchor_assignment_next_nonzero(UINT32_MAX) == 1u);

    assert(app_anchor_assignment_missing_ack_count(0u, 0u) == 0u);
    assert(app_anchor_assignment_missing_ack_count(4u, UINT64_C(0x5)) == 2u);
    assert(app_anchor_assignment_missing_ack_count(4u, UINT64_MAX) == 0u);
    assert(app_anchor_assignment_missing_ack_count(
               64u, UINT64_MAX ^ (UINT64_C(1) << 63)) == 1u);

    assert(!app_anchor_assignment_claims_complete(0u, 3u));
    assert(!app_anchor_assignment_claims_complete(3u, 2u));
    assert(app_anchor_assignment_claims_complete(3u, 3u));
    assert(app_anchor_assignment_claims_complete(3u, 4u));
}

static void test_anchor_assignment_settle_remaining_is_bounded(void)
{
    assert(app_anchor_assignment_settle_remaining_ms(false, 100u, 200u) ==
           0u);
    assert(app_anchor_assignment_settle_remaining_ms(true, 100u, 0u) == 0u);
    assert(app_anchor_assignment_settle_remaining_ms(true, 200u, 200u) ==
           0u);
    assert(app_anchor_assignment_settle_remaining_ms(true, 100u, 250u) ==
           150u);
    assert(app_anchor_assignment_settle_remaining_ms(
               true, 0u, UINT64_MAX) == UINT32_MAX);
}

static void test_anchor_assignment_collects_only_acked_claims(void)
{
    const uint64_t anchor_ids[] = {11u, 22u, 33u, 44u};
    struct discovery_assignment_entry entries[4] = {0};
    uint64_t committed_anchor_ids[4] = {0};
    size_t count;

    count = app_anchor_assignment_collect_committed(
        anchor_ids,
        4u,
        UINT64_C(0xa),
        entries,
        committed_anchor_ids,
        4u);
    assert(count == 2u);
    assert(committed_anchor_ids[0] == 22u);
    assert(entries[0].anchor_id == 22u);
    assert(entries[0].hash == discovery_assignment_hash(22u));
    assert(entries[0].slot == 1u);
    assert(committed_anchor_ids[1] == 44u);
    assert(entries[1].anchor_id == 44u);
    assert(entries[1].hash == discovery_assignment_hash(44u));
    assert(entries[1].slot == 3u);

    memset(entries, 0, sizeof(entries));
    memset(committed_anchor_ids, 0, sizeof(committed_anchor_ids));
    count = app_anchor_assignment_collect_committed(
        anchor_ids,
        4u,
        UINT64_MAX,
        entries,
        committed_anchor_ids,
        1u);
    assert(count == 1u);
    assert(committed_anchor_ids[0] == 11u);
    assert(entries[0].slot == 0u);
    assert(app_anchor_assignment_collect_committed(
               NULL, 4u, UINT64_MAX, entries, committed_anchor_ids, 4u) ==
           0u);
}

static void test_anchor_assignment_command_encoding(void)
{
    const struct app_anchor_assignment_command_params params = {
        .phase = DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        .epoch = 77u,
        .command_seq = 88u,
        .operation_budget_ms = 90000u,
        .response_spread_ms = 1500u,
        .source_id = 99u,
        .expected_anchor_count = 4u,
        .command_expiry_s = 120u,
        .ttl = 7u,
        .radio_channel = 5u,
    };
    struct operation_policy_set policy;
    struct mesh_outbound outbound;
    enum discovery_assignment_phase phase;
    enum command_id command_id;
    uint32_t epoch;

    assert(app_anchor_assignment_command_prepare(&outbound, &params) ==
           PROTO_OK);
    assert(gateway_command_extract_id(
               outbound.payload, outbound.payload_len, &command_id) ==
           PROTO_OK);
    assert(command_id == CMD_ASSIGN_DISCOVERY_SLOTS);
    assert(discovery_assignment_extract_control_tlvs(
               outbound.payload, outbound.payload_len, &phase, &epoch) ==
           PROTO_OK);
    assert(phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM);
    assert(epoch == 77u);
    assert(operation_policy_set_from_tlvs(
               outbound.payload, outbound.payload_len, &policy) == PROTO_OK);
    assert(policy.assignment_present);
    assert(policy.assignment.expected_anchor_count == 4u);
    assert(policy.assignment.operation_budget_ms == 90000u);
    assert(policy.assignment.response_spread_ms == 1500u);
    assert(outbound.packet.msg_type == MSG_COMMAND);
    assert(outbound.packet.src_id == 99u);
    assert(outbound.packet.dst_id == MESH_BROADCAST_ID);
    assert(outbound.packet.session_id == 88u);
    assert(outbound.packet.seq == 0u);
    assert(outbound.packet.ttl == 7u);
    assert(outbound.packet.payload_len == outbound.payload_len);
    assert(outbound.next_hop_id == MESH_BROADCAST_ID);
    assert(outbound.radio_channel == 5u);
    assert(app_anchor_assignment_command_prepare(NULL, &params) ==
           PROTO_ERR_ARG);
}

static void test_host_failure_cutoff_is_wrap_safe(void)
{
    assert(app_anchor_host_command_within_failure_cutoff(4u, 4u));
    assert(app_anchor_host_command_within_failure_cutoff(3u, 4u));
    assert(!app_anchor_host_command_within_failure_cutoff(5u, 4u));
    assert(app_anchor_host_command_within_failure_cutoff(UINT32_MAX, 1u));
    assert(!app_anchor_host_command_within_failure_cutoff(1u, UINT32_MAX));
    assert(!app_anchor_host_command_within_failure_cutoff(0u, 4u));
    assert(!app_anchor_host_command_within_failure_cutoff(4u, 0u));
}

static void test_host_handoff_contention_retains_queued_admissions(void)
{
    const uint32_t failed_cutoff = 7u;
    const uint32_t newer_admission = 8u;
    const uint32_t second_admission = 9u;

    /* The failure callback observed an empty queue before this admission. */
    assert(!app_anchor_host_command_within_failure_cutoff(
        newer_admission, failed_cutoff));
    assert(!app_anchor_host_command_within_failure_cutoff(
        second_admission, failed_cutoff));

    /* WAIT and GRANTED owner contention retain both depth-two admissions. */
    assert(app_anchor_host_command_submit_needs_retry(-EAGAIN));
    assert(app_anchor_host_command_submit_needs_retry(-EBUSY));
    assert(app_anchor_host_command_submit_needs_retry(-ENOSPC));
    assert(app_anchor_host_command_submit_needs_retry(-EINVAL));
    assert(app_anchor_host_command_submit_needs_retry(-ESTALE));
    assert(app_anchor_host_command_submit_needs_retry(-EIO));
    assert(!app_anchor_host_command_submit_needs_retry(0));
    assert(!app_anchor_host_command_submit_is_contract_failure(-EAGAIN));
    assert(!app_anchor_host_command_submit_is_contract_failure(-EBUSY));
    assert(!app_anchor_host_command_submit_is_contract_failure(-ENOSPC));
    assert(app_anchor_host_command_submit_is_contract_failure(-EINVAL));
    assert(app_anchor_host_command_submit_is_contract_failure(-ESTALE));
    assert(app_anchor_host_command_submit_is_contract_failure(-EIO));
    assert(app_anchor_host_command_ingress_result(-EAGAIN) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(-EBUSY) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(-ENOSPC) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(-EINVAL) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(-ESTALE) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(-EIO) == -EAGAIN);
    assert(app_anchor_host_command_ingress_result(0) == 0);
}

static void test_anchor_radio_policy(void)
{
    struct uwb_anchor_pair_schedule_frame schedule = {
        .network_id = 10u,
        .clicker_id = 20u,
        .survey_id = 30u,
        .attempt_index = 2u,
        .nonce = 40u,
        .flags = FLAG_DIAGNOSTIC,
    };
    struct uwb_anchor_epoch epoch = {
        .active = true,
        .network_id = 10u,
        .clicker_id = 20u,
        .click_event_id = 30u,
        .attempt_index = 2u,
        .nonce = 40u,
        .flags = FLAG_DIAGNOSTIC,
    };

    assert(app_anchor_radio_blocked_retry_ms(true, 7u, 100u) == 7u);
    assert(app_anchor_radio_blocked_retry_ms(false, 7u, 100u) == 100u);
    assert(app_anchor_radio_blocked_retry_ms(true, 0u, 100u) == 0u);
    assert(app_anchor_radio_blocked_retry_ms(false, 7u, 0u) == 0u);
    assert(app_anchor_radio_pair_schedule_matches_epoch(&schedule, &epoch));
    epoch.attempt_index++;
    assert(!app_anchor_radio_pair_schedule_matches_epoch(&schedule, &epoch));
    assert(!app_anchor_radio_pair_schedule_matches_epoch(NULL, &epoch));
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
    test_gateway_operation_deadline_is_64_bit_and_terminal();
    test_explicit_budget_scales_complete_claim_rounds();
    test_table_retry_budget_preserves_every_ack_window();
    test_stale_publish_work_cannot_run_in_next_assignment();
    test_failed_publish_submission_releases_generation();
    test_semantic_quorum_overrides_only_redundant_tail_failure();
    test_collection_depth_stays_conservative_until_roster_is_known();
    test_low_power_failure_recovers_and_retries_once();
    test_low_power_recovery_retry_can_complete();
    test_low_power_recovery_failure_does_not_retry();
    test_anchor_assignment_state_arithmetic();
    test_anchor_assignment_settle_remaining_is_bounded();
    test_anchor_assignment_collects_only_acked_claims();
    test_anchor_assignment_command_encoding();
    test_host_failure_cutoff_is_wrap_safe();
    test_host_handoff_contention_retains_queued_admissions();
    test_anchor_radio_policy();
    puts("app discovery assignment policy tests passed");
    return 0;
}
