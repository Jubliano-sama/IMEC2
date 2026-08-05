#include "app_operation_policy.h"

#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>

static struct operation_policy_set active_policy;
static bool active_policy_initialized;
K_MUTEX_DEFINE(active_policy_mutex);

static uint8_t operation_policy_present_mask(
    const struct operation_policy_set *set)
{
    uint8_t mask = 0u;

    if (set->assignment_present) {
        mask |= APP_OPERATION_POLICY_ASSIGNMENT_MASK;
    }
    if (set->discovery_present) {
        mask |= APP_OPERATION_POLICY_DISCOVERY_MASK;
    }
    if (set->pair_present) {
        mask |= APP_OPERATION_POLICY_PAIR_MASK;
    }
    return mask;
}

static int operation_policy_validate_set(
    const struct operation_policy_set *set)
{
    struct operation_policy policy;

    if (set == NULL) {
        return -EINVAL;
    }
    if (set->assignment_present) {
        policy = (struct operation_policy) {
            .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
            .value.assignment = set->assignment,
        };
        if (operation_policy_validate(&policy) != PROTO_OK) {
            return -EBADMSG;
        }
    }
    if (set->discovery_present) {
        policy = (struct operation_policy) {
            .family = OPERATION_POLICY_FAMILY_SURVEY_DISCOVERY,
            .value.discovery = set->discovery,
        };
        if (operation_policy_validate(&policy) != PROTO_OK) {
            return -EBADMSG;
        }
    }
    if (set->pair_present) {
        policy = (struct operation_policy) {
            .family = OPERATION_POLICY_FAMILY_SURVEY_PAIR,
            .value.pair = set->pair,
        };
        if (operation_policy_validate(&policy) != PROTO_OK) {
            return -EBADMSG;
        }
    }
    return 0;
}

static void operation_policy_ensure_initialized_locked(void)
{
    if (active_policy_initialized) {
        return;
    }
    operation_policy_set_defaults(&active_policy);
    active_policy.assignment_present = true;
    active_policy.discovery_present = true;
    active_policy.pair_present = true;
    active_policy_initialized = true;
}

void app_operation_policy_reset_defaults(void)
{
    k_mutex_lock(&active_policy_mutex, K_FOREVER);
    active_policy_initialized = false;
    operation_policy_ensure_initialized_locked();
    k_mutex_unlock(&active_policy_mutex);
}

int app_operation_policy_install(const struct operation_policy_set *set,
                                 uint8_t required_mask)
{
    uint8_t present_mask;

    if (set == NULL || (required_mask & ~APP_OPERATION_POLICY_ALL_MASK) != 0u) {
        return -EINVAL;
    }
    if (operation_policy_validate_set(set) < 0) {
        return -EBADMSG;
    }
    present_mask = operation_policy_present_mask(set);
    if ((present_mask & required_mask) != required_mask) {
        return -EBADMSG;
    }

    k_mutex_lock(&active_policy_mutex, K_FOREVER);
    operation_policy_ensure_initialized_locked();
    if (set->assignment_present) {
        active_policy.assignment = set->assignment;
    }
    if (set->discovery_present) {
        active_policy.discovery = set->discovery;
    }
    if (set->pair_present) {
        active_policy.pair = set->pair;
    }
    k_mutex_unlock(&active_policy_mutex);
    return 0;
}

static int operation_policy_parse_payload(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t required_mask,
    uint8_t allowed_mask,
    struct operation_policy_set *parsed)
{
    uint8_t present_mask;
    int ret;

    if (parsed == NULL ||
        (required_mask & ~APP_OPERATION_POLICY_ALL_MASK) != 0u ||
        (allowed_mask & ~APP_OPERATION_POLICY_ALL_MASK) != 0u ||
        (required_mask & ~allowed_mask) != 0u) {
        return -EINVAL;
    }
    ret = operation_policy_set_from_tlvs(payload, payload_len, parsed);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    present_mask = operation_policy_present_mask(parsed);
    if ((present_mask & required_mask) != required_mask ||
        (present_mask & ~allowed_mask) != 0u) {
        return -EBADMSG;
    }
    return operation_policy_validate_set(parsed);
}

static int operation_policy_resolve(
    const struct operation_policy_set *updates,
    struct operation_policy_set *resolved)
{
    int ret;

    if (updates == NULL || resolved == NULL) {
        return -EINVAL;
    }
    ret = operation_policy_validate_set(updates);
    if (ret < 0) {
        return ret;
    }

    app_operation_policy_snapshot(resolved);
    if (updates->assignment_present) {
        resolved->assignment = updates->assignment;
    }
    if (updates->discovery_present) {
        resolved->discovery = updates->discovery;
    }
    if (updates->pair_present) {
        resolved->pair = updates->pair;
    }
    return 0;
}

int app_operation_policy_prepare_payload(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t required_mask,
    uint8_t allowed_mask,
    struct app_operation_policy_candidate *candidate)
{
    int ret;

    if (candidate == NULL) {
        return -EINVAL;
    }
    memset(candidate, 0, sizeof(*candidate));
    ret = operation_policy_parse_payload(payload,
                                         payload_len,
                                         required_mask,
                                         allowed_mask,
                                         &candidate->updates);
    if (ret < 0) {
        return ret;
    }
    return operation_policy_resolve(&candidate->updates,
                                    &candidate->resolved);
}

void app_operation_policy_commit_prepared(
    const struct app_operation_policy_candidate *candidate)
{
    if (candidate == NULL) {
        return;
    }

    /*
     * prepare_payload() has already parsed and validated every supplied
     * family. Keep this commit infallible so a caller can publish the policy
     * only after its command-specific state transition succeeds.
     */
    k_mutex_lock(&active_policy_mutex, K_FOREVER);
    operation_policy_ensure_initialized_locked();
    if (candidate->updates.assignment_present) {
        active_policy.assignment = candidate->updates.assignment;
    }
    if (candidate->updates.discovery_present) {
        active_policy.discovery = candidate->updates.discovery;
    }
    if (candidate->updates.pair_present) {
        active_policy.pair = candidate->updates.pair;
    }
    k_mutex_unlock(&active_policy_mutex);
}

void app_operation_policy_snapshot(struct operation_policy_set *set)
{
    if (set == NULL) {
        return;
    }
    k_mutex_lock(&active_policy_mutex, K_FOREVER);
    operation_policy_ensure_initialized_locked();
    *set = active_policy;
    k_mutex_unlock(&active_policy_mutex);
}
