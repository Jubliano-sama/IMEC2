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

int app_operation_policy_apply_payload(const uint8_t *payload,
                                       size_t payload_len,
                                       uint8_t required_mask,
                                       struct operation_policy_set *accepted)
{
    struct operation_policy_set parsed;
    int ret;

    ret = operation_policy_set_from_tlvs(payload, payload_len, &parsed);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ret = app_operation_policy_install(&parsed, required_mask);
    if (ret < 0) {
        return ret;
    }
    if (accepted != NULL) {
        app_operation_policy_snapshot(accepted);
    }
    return 0;
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
