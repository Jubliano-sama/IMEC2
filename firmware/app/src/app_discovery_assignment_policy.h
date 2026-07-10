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

#endif
