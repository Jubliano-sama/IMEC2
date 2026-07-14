#ifndef APP_DISCOVERY_ASSIGNMENT_POLICY_H
#define APP_DISCOVERY_ASSIGNMENT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

struct app_discovery_assignment_work_guard {
    uint32_t pending_generation;
};

struct app_discovery_assignment_policy {
    uint32_t committed_epoch;
    uint32_t joining_epoch;
    uint32_t retired_epoch;
    bool claim_observed;
    bool provisioned;
};

static inline void app_discovery_assignment_policy_init(
    struct app_discovery_assignment_policy *policy,
    bool persisted_assignment_valid,
    uint32_t persisted_epoch)
{
    if (policy == NULL) {
        return;
    }

    policy->committed_epoch = persisted_assignment_valid ? persisted_epoch : 0u;
    policy->joining_epoch = 0u;
    policy->retired_epoch = 0u;
    policy->claim_observed = false;
    policy->provisioned = persisted_assignment_valid && persisted_epoch != 0u;
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
    if (policy->retired_epoch != 0u && epoch == policy->retired_epoch) {
        return APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE;
    }
    if (policy->joining_epoch != 0u &&
        policy->joining_epoch != epoch &&
        policy->provisioned &&
        epoch == policy->committed_epoch) {
        return APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE;
    }

    policy->joining_epoch = epoch;
    policy->claim_observed = true;
    return APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND;
}

static inline enum app_discovery_assignment_table_decision
app_discovery_assignment_policy_note_table(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID;
    }
    if (policy->joining_epoch != 0u) {
        if (epoch != policy->joining_epoch) {
            return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
        }
        return policy->claim_observed ?
               APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY :
               APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM;
    }
    if (policy->provisioned && epoch == policy->committed_epoch) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY;
    }
    if (policy->retired_epoch != 0u && epoch == policy->retired_epoch) {
        return APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE;
    }

    policy->joining_epoch = epoch;
    policy->claim_observed = false;
    return APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM;
}

static inline bool app_discovery_assignment_policy_commit(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u ||
        (policy->joining_epoch != 0u &&
         (policy->joining_epoch != epoch || !policy->claim_observed)) ||
        (policy->joining_epoch == 0u &&
         (!policy->provisioned || policy->committed_epoch != epoch))) {
        return false;
    }

    if (policy->provisioned && policy->committed_epoch != epoch) {
        policy->retired_epoch = policy->committed_epoch;
    }
    if (policy->retired_epoch == epoch) {
        policy->retired_epoch = 0u;
    }
    policy->committed_epoch = epoch;
    policy->joining_epoch = 0u;
    policy->claim_observed = false;
    policy->provisioned = true;
    return true;
}

static inline void app_discovery_assignment_policy_note_unassigned(
    struct app_discovery_assignment_policy *policy,
    uint32_t epoch)
{
    if (policy == NULL || epoch == 0u) {
        return;
    }

    if (policy->provisioned && policy->committed_epoch != epoch) {
        policy->retired_epoch = policy->committed_epoch;
    }
    policy->committed_epoch = 0u;
    policy->joining_epoch = epoch;
    policy->claim_observed = true;
    policy->provisioned = false;
}

static inline bool app_discovery_assignment_operation_expired(
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
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
