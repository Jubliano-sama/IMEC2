#ifndef APP_DISCOVERY_ASSIGNMENT_POLICY_H
#define APP_DISCOVERY_ASSIGNMENT_POLICY_H

#include "discovery_assignment.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum app_discovery_assignment_provisioning_state {
    APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED = 0,
    APP_DISCOVERY_ASSIGNMENT_PROVISIONED = 1,
};

enum app_discovery_assignment_claim_decision {
    APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND = 0,
    APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE,
    APP_DISCOVERY_ASSIGNMENT_CLAIM_INVALID,
};

enum app_discovery_assignment_table_decision {
    APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY = 0,
    APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY,
    APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM,
    APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE,
    APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID,
};

enum app_discovery_assignment_work_request {
    APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT = 0,
    APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING,
    APP_DISCOVERY_ASSIGNMENT_WORK_WAIT_STALE,
    APP_DISCOVERY_ASSIGNMENT_WORK_INVALID,
};

enum app_discovery_assignment_terminal_phase {
    APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM = 0,
    APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
};

struct app_discovery_assignment_work_guard {
    uint32_t pending_generation;
};

struct app_discovery_assignment_policy {
    uint32_t committed_epoch;
    uint32_t committed_table_seq;
    struct discovery_assignment_table_commitment committed_table_commitment;
    uint32_t joining_epoch;
    uint32_t joining_table_seq;
    struct discovery_assignment_table_commitment joining_table_commitment;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint8_t retired_epoch_count;
    bool claim_observed;
    bool provisioned;
    bool ordered_epoch_valid;
};

static inline void app_discovery_assignment_policy_init(
    struct app_discovery_assignment_policy *policy,
    bool persisted_assignment_valid,
    bool persisted_ordered_epoch_valid,
    bool persisted_provisioned,
    uint32_t persisted_epoch,
    uint32_t persisted_table_seq,
    const struct discovery_assignment_table_commitment *persisted_table_commitment)
{
    if (policy == NULL) {
        return;
    }

    policy->committed_epoch = persisted_assignment_valid ? persisted_epoch : 0u;
    policy->committed_table_seq =
        persisted_assignment_valid ? persisted_table_seq : 0u;
    memset(&policy->committed_table_commitment,
           0,
           sizeof(policy->committed_table_commitment));
    if (persisted_assignment_valid && persisted_table_commitment != NULL) {
        policy->committed_table_commitment = *persisted_table_commitment;
    }
    policy->joining_epoch = 0u;
    policy->joining_table_seq = 0u;
    memset(&policy->joining_table_commitment,
           0,
           sizeof(policy->joining_table_commitment));
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        policy->retired_epochs[i] = 0u;
    }
    policy->retired_epoch_count = 0u;
    policy->claim_observed = false;
    policy->provisioned = persisted_assignment_valid && persisted_provisioned &&
                          persisted_epoch != 0u && persisted_table_seq != 0u &&
                          persisted_table_commitment != NULL;
    policy->ordered_epoch_valid =
        persisted_assignment_valid && persisted_ordered_epoch_valid &&
        persisted_epoch != 0u;
}

static inline bool app_discovery_assignment_policy_epoch_admissible(
    const struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u) {
        return false;
    }
    if (!policy->ordered_epoch_valid) {
        /*
         * A migrated snapshot has no comparable baseline, so its first
         * new-scheme epoch may have any nonzero value.  Once that candidate
         * is observed, bind the in-flight exchange to it until it commits.
         */
        return policy->joining_epoch == 0u ||
               epoch == policy->joining_epoch ||
               discovery_assignment_epoch_strictly_newer(
                   epoch, policy->joining_epoch);
    }
    if (policy->joining_epoch != 0u &&
        epoch != policy->joining_epoch &&
        !discovery_assignment_epoch_strictly_newer(
            epoch, policy->joining_epoch)) {
        return false;
    }
    if (policy->committed_epoch != 0u &&
        epoch != policy->committed_epoch &&
        !discovery_assignment_epoch_strictly_newer(
            epoch, policy->committed_epoch)) {
        return false;
    }
    return true;
}

static inline bool app_discovery_assignment_policy_epoch_retired(
    const struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < policy->retired_epoch_count; i++) {
        if (policy->retired_epochs[i] == epoch) {
            return true;
        }
    }
    return false;
}

static inline bool app_discovery_assignment_response_identity_matches(
    enum discovery_assignment_phase phase,
    uint32_t pending_command_seq,
    const struct discovery_assignment_table_commitment *pending_commitment,
    uint32_t incoming_command_seq,
    const struct discovery_assignment_table_commitment *incoming_commitment)
{
    if (pending_command_seq == 0u ||
        pending_command_seq != incoming_command_seq) {
        return false;
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        return true;
    }
    return phase == DISCOVERY_ASSIGNMENT_PHASE_ACK &&
           pending_commitment != NULL &&
           incoming_commitment != NULL &&
           discovery_assignment_table_commitment_equal(
               pending_commitment, incoming_commitment);
}

static inline bool app_discovery_assignment_retired_epochs_valid(
    uint32_t committed_epoch,
    const uint32_t *retired_epochs,
    uint8_t retired_epoch_count,
    bool require_canonical_tail)
{
    if (retired_epoch_count > DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP ||
        (retired_epochs == NULL &&
         (retired_epoch_count != 0u || require_canonical_tail))) {
        return false;
    }
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        if (i >= retired_epoch_count) {
            if (require_canonical_tail && retired_epochs[i] != 0u) {
                return false;
            }
            continue;
        }
        if (retired_epochs[i] == 0u ||
            retired_epochs[i] == committed_epoch) {
            return false;
        }
        for (size_t j = 0u; j < i; j++) {
            if (retired_epochs[i] == retired_epochs[j]) {
                return false;
            }
        }
    }
    return true;
}

static inline void app_discovery_assignment_policy_retire_epoch(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    size_t move_count;

    if (policy == NULL || epoch == 0u ||
        epoch == policy->committed_epoch ||
        app_discovery_assignment_policy_epoch_retired(policy, epoch)) {
        return;
    }
    move_count = policy->retired_epoch_count;
    if (move_count >= DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP) {
        move_count = DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP - 1u;
    }
    for (size_t i = move_count; i > 0u; i--) {
        policy->retired_epochs[i] = policy->retired_epochs[i - 1u];
    }
    policy->retired_epochs[0] = epoch;
    if (policy->retired_epoch_count <
        DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP) {
        policy->retired_epoch_count++;
    }
}

static inline bool app_discovery_assignment_policy_restore_retired_epochs(
    struct app_discovery_assignment_policy *policy,
    const uint32_t *retired_epochs,
    uint8_t retired_epoch_count)
{
    if (policy == NULL ||
        !app_discovery_assignment_retired_epochs_valid(
            policy->committed_epoch,
            retired_epochs,
            retired_epoch_count,
            false)) {
        return false;
    }
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        policy->retired_epochs[i] =
            i < retired_epoch_count ? retired_epochs[i] : 0u;
    }
    policy->retired_epoch_count = retired_epoch_count;
    return true;
}

static inline bool app_discovery_assignment_policy_export_retired_epochs(
    const struct app_discovery_assignment_policy *policy,
    uint32_t *retired_epochs,
    uint8_t *retired_epoch_count)
{
    if (policy == NULL || retired_epochs == NULL ||
        retired_epoch_count == NULL) {
        return false;
    }
    memcpy(retired_epochs,
           policy->retired_epochs,
           sizeof(policy->retired_epochs));
    *retired_epoch_count = policy->retired_epoch_count;
    return true;
}

static inline bool app_discovery_assignment_policy_restore_pending(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    if (policy == NULL || epoch == 0u || table_seq == 0u ||
        table_commitment == NULL ||
        (policy->committed_epoch != 0u &&
         epoch == policy->committed_epoch)) {
        return false;
    }
    policy->joining_epoch = epoch;
    policy->joining_table_seq = table_seq;
    policy->joining_table_commitment = *table_commitment;
    /*
     * A durable pending TABLE could only have been written after its CLAIM
     * was admitted. Restore that causal fact so exact ACK delivery can
     * promote without requiring another TABLE after reset.
     */
    policy->claim_observed = true;
    policy->ordered_epoch_valid = true;
    return true;
}

static inline bool app_discovery_assignment_policy_project_retired_epochs(
    const struct app_discovery_assignment_policy *policy,
    uint32_t next_epoch,
    uint32_t *retired_epochs,
    uint8_t *retired_epoch_count)
{
    struct app_discovery_assignment_policy projected;

    if (policy == NULL || next_epoch == 0u || retired_epochs == NULL ||
        retired_epoch_count == NULL) {
        return false;
    }
    if (!policy->ordered_epoch_valid ||
        (policy->committed_epoch != 0u &&
         next_epoch != policy->committed_epoch &&
         !discovery_assignment_epoch_strictly_newer(
             next_epoch, policy->committed_epoch))) {
        /*
         * A received enumeration is authoritative even when its correlation
         * epoch is numerically lower than the last committed operation. Such
         * a rebase makes the old ordering history incomparable, so persist an
         * empty history beside the pending TABLE and let this CLAIM establish
         * the new baseline.
         */
        memset(retired_epochs,
               0,
               DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP *
                   sizeof(retired_epochs[0]));
        *retired_epoch_count = 0u;
        return true;
    }
    projected = *policy;
    if (projected.committed_epoch != 0u &&
        projected.committed_epoch != next_epoch) {
        uint32_t previous_epoch = projected.committed_epoch;

        projected.committed_epoch = 0u;
        app_discovery_assignment_policy_retire_epoch(&projected,
                                                     previous_epoch);
    }
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        retired_epochs[i] =
            i < projected.retired_epoch_count ?
                projected.retired_epochs[i] : 0u;
    }
    *retired_epoch_count = projected.retired_epoch_count;
    return true;
}

static inline enum app_discovery_assignment_provisioning_state
app_discovery_assignment_policy_provisioning_state(
    const struct app_discovery_assignment_policy *policy)
{
    return policy != NULL && policy->provisioned ?
           APP_DISCOVERY_ASSIGNMENT_PROVISIONED :
           APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED;
}

static inline const char *app_discovery_assignment_provisioning_state_name(
    enum app_discovery_assignment_provisioning_state state)
{
    return state == APP_DISCOVERY_ASSIGNMENT_PROVISIONED ?
           "PROVISIONED" : "UNPROVISIONED";
}

static inline bool app_discovery_assignment_policy_normal_click_reply_allowed(
    const struct app_discovery_assignment_policy *policy)
{
    return app_discovery_assignment_policy_provisioning_state(policy) ==
           APP_DISCOVERY_ASSIGNMENT_PROVISIONED;
}

static inline enum app_discovery_assignment_claim_decision
app_discovery_assignment_policy_note_claim(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u) {
        return APP_DISCOVERY_ASSIGNMENT_CLAIM_INVALID;
    }
    if (policy->joining_epoch != epoch) {
        policy->joining_table_seq = 0u;
        memset(&policy->joining_table_commitment,
               0,
               sizeof(policy->joining_table_commitment));
    }
    policy->joining_epoch = epoch;
    policy->claim_observed = true;
    return APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND;
}

static inline enum app_discovery_assignment_table_decision
app_discovery_assignment_policy_note_table(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    bool claim_owns_epoch;

    if (policy == NULL || epoch == 0u || table_seq == 0u ||
        table_commitment == NULL) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID;
    }
    claim_owns_epoch = policy->joining_epoch == epoch &&
                       policy->claim_observed;
    /*
     * CLAIM establishes the authoritative active enumeration identity. Epoch
     * ordering is deliberately not an admission rule: a valid lower epoch
     * from a restarted gateway must still enumerate retained anchors. A TABLE
     * for another epoch cannot replace a CLAIM already in progress.
     */
    if (policy->joining_epoch != 0u &&
        policy->joining_epoch != epoch) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
    }
    if (!claim_owns_epoch &&
        ((policy->ordered_epoch_valid &&
          app_discovery_assignment_policy_epoch_retired(policy, epoch)) ||
         !app_discovery_assignment_policy_epoch_admissible(policy, epoch))) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
    }
    /*
     * Ordered operation epochs make a committed epoch immutable.  Gateway
     * retransmissions reuse the same table command sequence and fingerprint,
     * so any other generation under that epoch is stale, not an expansion.
     * Migrated unordered snapshots intentionally bypass this until their first
     * new-scheme epoch commits.
     */
    if (policy->ordered_epoch_valid &&
        epoch == policy->committed_epoch &&
        policy->committed_table_seq != 0u) {
        return table_seq == policy->committed_table_seq &&
                       discovery_assignment_table_commitment_equal(
                           table_commitment,
                           &policy->committed_table_commitment) ?
               APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY :
               APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
    }
    if (policy->joining_epoch != 0u) {
        if (policy->joining_table_seq != 0u) {
            if (table_seq == policy->joining_table_seq) {
                if (!discovery_assignment_table_commitment_equal(
                        table_commitment,
                        &policy->joining_table_commitment)) {
                    return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
                }
                if (!policy->claim_observed) {
                    return APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM;
                }
                return epoch == policy->committed_epoch &&
                               table_seq == policy->committed_table_seq &&
                               discovery_assignment_table_commitment_equal(
                                   table_commitment,
                                   &policy->committed_table_commitment) ?
                       APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY :
                       APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY;
            }
            return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
        }
        policy->joining_table_seq = table_seq;
        policy->joining_table_commitment = *table_commitment;
        return policy->claim_observed ?
               APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY :
               APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM;
    }
    policy->joining_epoch = epoch;
    policy->joining_table_seq = table_seq;
    policy->joining_table_commitment = *table_commitment;
    policy->claim_observed = false;
    return APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM;
}

static inline bool app_discovery_assignment_policy_commit(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    bool establishing_order;

    if (policy == NULL || epoch == 0u || table_seq == 0u ||
        table_commitment == NULL) {
        return false;
    }
    if (!((policy->joining_epoch == epoch && policy->claim_observed &&
           policy->joining_table_seq == table_seq &&
           discovery_assignment_table_commitment_equal(
               &policy->joining_table_commitment, table_commitment)) ||
          ((policy->joining_epoch == 0u || policy->joining_epoch == epoch) &&
           policy->committed_epoch == epoch &&
           policy->committed_table_seq == table_seq &&
           discovery_assignment_table_commitment_equal(
               &policy->committed_table_commitment, table_commitment)))) {
        return false;
    }

    establishing_order = !policy->ordered_epoch_valid ||
        (policy->committed_epoch != 0u &&
         policy->committed_epoch != epoch &&
         !discovery_assignment_epoch_strictly_newer(
             epoch, policy->committed_epoch));
    if (establishing_order) {
        memset(policy->retired_epochs, 0, sizeof(policy->retired_epochs));
        policy->retired_epoch_count = 0u;
        policy->committed_epoch = 0u;
    }
    if (policy->committed_epoch != 0u &&
        policy->committed_epoch != epoch) {
        uint32_t previous_epoch = policy->committed_epoch;

        policy->committed_epoch = 0u;
        app_discovery_assignment_policy_retire_epoch(policy,
                                                     previous_epoch);
    }
    policy->committed_epoch = epoch;
    policy->committed_table_seq = table_seq;
    policy->committed_table_commitment = *table_commitment;
    policy->joining_epoch = 0u;
    policy->joining_table_seq = 0u;
    memset(&policy->joining_table_commitment,
           0,
           sizeof(policy->joining_table_commitment));
    policy->claim_observed = false;
    policy->provisioned = true;
    policy->ordered_epoch_valid = true;
    return true;
}

static inline bool app_discovery_assignment_policy_note_unassigned(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    bool establishing_order;

    if (policy == NULL || epoch == 0u || table_seq == 0u ||
        table_commitment == NULL) {
        return false;
    }
    if (!((policy->joining_epoch == epoch && policy->claim_observed &&
           policy->joining_table_seq == table_seq &&
           discovery_assignment_table_commitment_equal(
               &policy->joining_table_commitment, table_commitment)) ||
          (!policy->provisioned &&
           (policy->joining_epoch == 0u || policy->joining_epoch == epoch) &&
           policy->committed_epoch == epoch &&
           policy->committed_table_seq == table_seq &&
           discovery_assignment_table_commitment_equal(
               &policy->committed_table_commitment, table_commitment)))) {
        return false;
    }

    establishing_order = !policy->ordered_epoch_valid ||
        (policy->committed_epoch != 0u &&
         policy->committed_epoch != epoch &&
         !discovery_assignment_epoch_strictly_newer(
             epoch, policy->committed_epoch));
    if (establishing_order) {
        memset(policy->retired_epochs, 0, sizeof(policy->retired_epochs));
        policy->retired_epoch_count = 0u;
        policy->committed_epoch = 0u;
    }
    if (policy->committed_epoch != 0u &&
        policy->committed_epoch != epoch) {
        uint32_t previous_epoch = policy->committed_epoch;

        policy->committed_epoch = 0u;
        app_discovery_assignment_policy_retire_epoch(policy,
                                                     previous_epoch);
    }
    policy->committed_epoch = epoch;
    policy->committed_table_seq = table_seq;
    policy->committed_table_commitment = *table_commitment;
    policy->joining_epoch = 0u;
    policy->joining_table_seq = 0u;
    memset(&policy->joining_table_commitment,
           0,
           sizeof(policy->joining_table_commitment));
    policy->claim_observed = false;
    policy->provisioned = false;
    policy->ordered_epoch_valid = true;
    return true;
}

static inline bool app_discovery_assignment_operation_expired(
    uint64_t now_ms,
    uint64_t deadline_ms)
{
    return now_ms >= deadline_ms;
}

static inline bool app_discovery_assignment_semantic_terminal_success(
    enum app_discovery_assignment_terminal_phase phase,
    uint16_t expected_claim_count,
    size_t accepted_claim_count,
    uint8_t missing_ack_count,
    uint8_t attempts_started,
    bool operation_expired,
    bool cancelled)
{
    if (attempts_started == 0u || operation_expired || cancelled) {
        return false;
    }
    /*
     * A real control-flood attempt can already have scheduled a response that
     * outlives the delivery handle. Preserve that attempt's original response
     * horizon even when no response has reached the gateway yet. The phase
     * timeout decides whether the frozen window ultimately produced enough
     * CLAIMs or ACKs.
     */
    (void)expected_claim_count;
    (void)accepted_claim_count;
    (void)missing_ack_count;
    return phase == APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM ||
           phase == APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE;
}

static inline uint8_t app_discovery_assignment_collection_hop_count(
    uint16_t expected_claim_count,
    size_t accepted_claim_count,
    uint8_t observed_max_hop_count)
{
    if (expected_claim_count == 0u ||
        accepted_claim_count < expected_claim_count) {
        return 0u;
    }
    return observed_max_hop_count;
}

static inline uint8_t app_discovery_assignment_claim_round_limit(
    bool explicit_budget,
    uint8_t budget_round_limit,
    uint8_t robust_round_limit)
{
    if (robust_round_limit == 0u ||
        (explicit_budget && budget_round_limit == 0u)) {
        return 0u;
    }
    if (!explicit_budget || budget_round_limit >= robust_round_limit) {
        return robust_round_limit;
    }
    return budget_round_limit;
}

static inline uint8_t app_discovery_assignment_table_windows_remaining(
    uint8_t current_round,
    uint8_t round_limit)
{
    if (current_round == 0u || current_round > round_limit) {
        return 0u;
    }
    return (uint8_t)(round_limit - current_round + 1u);
}

static inline uint8_t app_discovery_assignment_table_round_after_delivery(
    uint8_t current_round,
    uint8_t attempts_started,
    bool immutable_redrive)
{
    if (immutable_redrive || attempts_started == 0u ||
        current_round == UINT8_MAX) {
        return current_round;
    }
    return (uint8_t)(current_round + 1u);
}

static inline bool app_discovery_assignment_table_retry_backoff_required(
    bool ack_window_open,
    uint8_t missing_ack_count,
    uint8_t current_round,
    uint8_t round_limit)
{
    return ack_window_open && missing_ack_count != 0u &&
           current_round != 0u && current_round < round_limit;
}

static inline void app_discovery_assignment_work_guard_init(
    struct app_discovery_assignment_work_guard *guard)
{
    if (guard != NULL) {
        guard->pending_generation = 0u;
    }
}

static inline enum app_discovery_assignment_work_request
app_discovery_assignment_work_guard_request(
    struct app_discovery_assignment_work_guard *guard,
    uint32_t generation)
{
    if (guard == NULL || generation == 0u) {
        return APP_DISCOVERY_ASSIGNMENT_WORK_INVALID;
    }
    if (guard->pending_generation == 0u) {
        guard->pending_generation = generation;
        return APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT;
    }
    if (guard->pending_generation == generation) {
        return APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING;
    }
    return APP_DISCOVERY_ASSIGNMENT_WORK_WAIT_STALE;
}

static inline void app_discovery_assignment_work_guard_note_submit_result(
    struct app_discovery_assignment_work_guard *guard,
    uint32_t generation,
    int result)
{
    if (guard != NULL && result < 0 &&
        guard->pending_generation == generation) {
        guard->pending_generation = 0u;
    }
}

static inline bool app_discovery_assignment_work_guard_begin(
    struct app_discovery_assignment_work_guard *guard,
    uint32_t active_generation)
{
    uint32_t pending_generation;

    if (guard == NULL || active_generation == 0u) {
        return false;
    }
    pending_generation = guard->pending_generation;
    guard->pending_generation = 0u;
    return pending_generation == active_generation;
}

#endif
