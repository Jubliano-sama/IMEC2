#include "app_mesh_radio_owner_policy.h"

#include <errno.h>
#include <string.h>

static bool radio_client_valid(enum app_mesh_radio_client client)
{
    return client > APP_MESH_RADIO_CLIENT_NONE &&
           client < APP_MESH_RADIO_CLIENT_COUNT;
}

static bool abort_kind_valid(enum app_mesh_radio_abort_kind kind)
{
    return kind > APP_MESH_RADIO_ABORT_NONE &&
           kind < APP_MESH_RADIO_ABORT_KIND_COUNT;
}

static bool owner_lease_equal(
    const struct app_mesh_radio_owner_lease *lhs,
    const struct app_mesh_radio_owner_lease *rhs)
{
    return lhs->generation == rhs->generation && lhs->client == rhs->client;
}

static bool handoff_lease_equal(
    const struct app_mesh_radio_owner_handoff_lease *lhs,
    const struct app_mesh_radio_owner_handoff_lease *rhs)
{
    return lhs->generation == rhs->generation &&
           lhs->identity == rhs->identity;
}

static bool generation_in_use(
    const struct app_mesh_radio_owner_policy *policy,
    uint32_t generation)
{
    if (generation == 0u) {
        return true;
    }
    if (policy->owner_phase != APP_MESH_RADIO_OWNER_IDLE &&
        policy->owner.generation == generation) {
        return true;
    }
    if (policy->pause_active && policy->pause.generation == generation) {
        return true;
    }
    if (policy->handoff_phase != APP_MESH_RADIO_HANDOFF_IDLE &&
        policy->handoff.generation == generation) {
        return true;
    }
    for (size_t i = 0u; i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
        if (policy->aborts[i].lease.token == generation) {
            return true;
        }
    }
    return false;
}

static uint32_t allocate_generation(
    struct app_mesh_radio_owner_policy *policy)
{
    do {
        policy->next_generation++;
        if (policy->next_generation == 0u) {
            policy->next_generation++;
        }
    } while (generation_in_use(policy, policy->next_generation));

    return policy->next_generation;
}

static void clear_owner(struct app_mesh_radio_owner_policy *policy)
{
    policy->owner_phase = APP_MESH_RADIO_OWNER_IDLE;
    memset(&policy->owner, 0, sizeof(policy->owner));
}

static void clear_handoff(struct app_mesh_radio_owner_policy *policy)
{
    policy->handoff_phase = APP_MESH_RADIO_HANDOFF_IDLE;
    memset(&policy->handoff, 0, sizeof(policy->handoff));
}

void app_mesh_radio_owner_policy_reset(
    struct app_mesh_radio_owner_policy *policy)
{
    uint32_t next_generation;

    if (policy == NULL) {
        return;
    }

    next_generation = policy->next_generation;
    memset(policy, 0, sizeof(*policy));
    policy->next_generation = next_generation;
}

int app_mesh_radio_owner_policy_try_claim(
    struct app_mesh_radio_owner_policy *policy,
    enum app_mesh_radio_client client,
    struct app_mesh_radio_owner_lease *lease_out)
{
    if (policy == NULL || lease_out == NULL || !radio_client_valid(client)) {
        return -EINVAL;
    }
    if (policy->pause_active ||
        policy->owner_phase != APP_MESH_RADIO_OWNER_IDLE) {
        return -EBUSY;
    }

    policy->owner = (struct app_mesh_radio_owner_lease) {
        .generation = allocate_generation(policy),
        .client = client,
    };
    policy->owner_phase = APP_MESH_RADIO_OWNER_ACTIVE;
    *lease_out = policy->owner;
    return 0;
}

int app_mesh_radio_owner_policy_release_begin(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || !radio_client_valid(lease->client)) {
        return -EINVAL;
    }
    if (policy->owner_phase == APP_MESH_RADIO_OWNER_IDLE ||
        !owner_lease_equal(&policy->owner, lease)) {
        return -ESTALE;
    }
    if (policy->owner_phase == APP_MESH_RADIO_OWNER_RELEASING) {
        return -EALREADY;
    }

    policy->owner_phase = APP_MESH_RADIO_OWNER_RELEASING;
    return 0;
}

int app_mesh_radio_owner_policy_release_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || !radio_client_valid(lease->client)) {
        return -EINVAL;
    }
    if (policy->owner_phase == APP_MESH_RADIO_OWNER_IDLE ||
        !owner_lease_equal(&policy->owner, lease)) {
        return -ESTALE;
    }
    if (policy->owner_phase != APP_MESH_RADIO_OWNER_RELEASING) {
        return -EINVAL;
    }

    clear_owner(policy);
    return 0;
}

bool app_mesh_radio_owner_policy_busy(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL &&
           policy->owner_phase != APP_MESH_RADIO_OWNER_IDLE;
}

enum app_mesh_radio_owner_phase app_mesh_radio_owner_policy_phase(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy == NULL ? APP_MESH_RADIO_OWNER_IDLE : policy->owner_phase;
}

bool app_mesh_radio_owner_policy_claim_snapshot(
    const struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_lease *lease_out)
{
    if (policy == NULL || lease_out == NULL ||
        policy->owner_phase == APP_MESH_RADIO_OWNER_IDLE) {
        return false;
    }

    *lease_out = policy->owner;
    return true;
}

int app_mesh_radio_owner_policy_pause(
    struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_pause_lease *lease_in_out)
{
    if (policy == NULL || lease_in_out == NULL) {
        return -EINVAL;
    }
    if (policy->pause_active) {
        if (lease_in_out->generation == policy->pause.generation) {
            *lease_in_out = policy->pause;
            return 0;
        }
        return lease_in_out->generation == 0u ? -EBUSY : -ESTALE;
    }
    if (lease_in_out->generation != 0u) {
        return -ESTALE;
    }

    policy->pause.generation = allocate_generation(policy);
    policy->pause_active = true;
    *lease_in_out = policy->pause;
    return 0;
}

int app_mesh_radio_owner_policy_resume(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_pause_lease *lease)
{
    if (policy == NULL || lease == NULL || lease->generation == 0u) {
        return -EINVAL;
    }
    if (!policy->pause_active ||
        lease->generation != policy->pause.generation) {
        return -ESTALE;
    }

    policy->pause_active = false;
    memset(&policy->pause, 0, sizeof(policy->pause));
    return 0;
}

bool app_mesh_radio_owner_policy_paused(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL && policy->pause_active;
}

int app_mesh_radio_owner_policy_handoff_request(
    struct app_mesh_radio_owner_policy *policy,
    uintptr_t identity,
    struct app_mesh_radio_owner_handoff_lease *lease_out)
{
    if (policy == NULL || identity == (uintptr_t)0 || lease_out == NULL) {
        return -EINVAL;
    }
    if (policy->handoff_phase != APP_MESH_RADIO_HANDOFF_IDLE) {
        if (policy->handoff_phase ==
            APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK) {
            return -EAGAIN;
        }
        if (policy->handoff.identity != identity) {
            return -EBUSY;
        }
        *lease_out = policy->handoff;
        return 0;
    }

    policy->handoff = (struct app_mesh_radio_owner_handoff_lease) {
        .generation = allocate_generation(policy),
        .identity = identity,
    };
    policy->handoff_phase = APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY;
    *lease_out = policy->handoff;
    return 0;
}

int app_mesh_radio_owner_policy_handoff_begin(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || lease->identity == (uintptr_t)0) {
        return -EINVAL;
    }
    if (policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE ||
        !handoff_lease_equal(&policy->handoff, lease)) {
        return -ESTALE;
    }
    if (policy->handoff_phase !=
        APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY) {
        return -EALREADY;
    }

    policy->handoff_phase = APP_MESH_RADIO_HANDOFF_SCHEDULING;
    return 0;
}

int app_mesh_radio_owner_policy_handoff_schedule_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease,
    bool success)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || lease->identity == (uintptr_t)0) {
        return -EINVAL;
    }
    if (policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE ||
        !handoff_lease_equal(&policy->handoff, lease)) {
        return -ESTALE;
    }
    if (policy->handoff_phase ==
        APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY) {
        return -EINVAL;
    }
    if (success &&
        policy->handoff_phase == APP_MESH_RADIO_HANDOFF_GRANTED) {
        return -EALREADY;
    }
    if (!success &&
        policy->handoff_phase ==
            APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK) {
        return -EALREADY;
    }

    if (success) {
        policy->handoff_phase = APP_MESH_RADIO_HANDOFF_GRANTED;
    } else {
        policy->handoff_phase = APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK;
    }
    return 0;
}

int app_mesh_radio_owner_policy_handoff_failure_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || lease->identity == (uintptr_t)0) {
        return -EINVAL;
    }
    if (policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE ||
        !handoff_lease_equal(&policy->handoff, lease)) {
        return -ESTALE;
    }
    if (policy->handoff_phase !=
        APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK) {
        return -EINVAL;
    }

    clear_handoff(policy);
    return 0;
}

int app_mesh_radio_owner_policy_handoff_take_grant(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || lease->identity == (uintptr_t)0) {
        return -EINVAL;
    }
    if (policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE ||
        !handoff_lease_equal(&policy->handoff, lease)) {
        return -ESTALE;
    }
    if (policy->handoff_phase != APP_MESH_RADIO_HANDOFF_GRANTED) {
        return -EINVAL;
    }

    clear_handoff(policy);
    return 0;
}

int app_mesh_radio_owner_policy_handoff_cancel(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease)
{
    if (policy == NULL || lease == NULL ||
        lease->generation == 0u || lease->identity == (uintptr_t)0) {
        return -EINVAL;
    }
    if (policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE ||
        !handoff_lease_equal(&policy->handoff, lease)) {
        return -ESTALE;
    }
    if (policy->handoff_phase ==
        APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK) {
        return -EBUSY;
    }

    clear_handoff(policy);
    return 0;
}

bool app_mesh_radio_owner_policy_handoff_waiting(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL &&
           policy->handoff_phase ==
               APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY;
}

enum app_mesh_radio_handoff_phase app_mesh_radio_owner_policy_handoff_phase(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy == NULL ? APP_MESH_RADIO_HANDOFF_IDLE :
                            policy->handoff_phase;
}

bool app_mesh_radio_owner_policy_handoff_snapshot(
    const struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_handoff_lease *lease_out)
{
    if (policy == NULL || lease_out == NULL ||
        policy->handoff_phase == APP_MESH_RADIO_HANDOFF_IDLE) {
        return false;
    }

    *lease_out = policy->handoff;
    return true;
}

int app_mesh_radio_owner_policy_abort_request(
    struct app_mesh_radio_owner_policy *policy,
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease_in_out)
{
    struct app_mesh_radio_owner_abort_slot *free_slot = NULL;

    if (policy == NULL || lease_in_out == NULL ||
        !abort_kind_valid(kind)) {
        return -EINVAL;
    }

    if (lease_in_out->token != 0u) {
        if (lease_in_out->kind != kind) {
            return -ESTALE;
        }
        for (size_t i = 0u; i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
            const struct app_mesh_radio_owner_abort_slot *slot =
                &policy->aborts[i];

            if (slot->lease.token == lease_in_out->token &&
                slot->lease.kind == kind) {
                *lease_in_out = slot->lease;
                return 0;
            }
        }
        return -ESTALE;
    }
    if (lease_in_out->kind != APP_MESH_RADIO_ABORT_NONE &&
        lease_in_out->kind != kind) {
        return -EINVAL;
    }

    for (size_t i = 0u; i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
        if (policy->aborts[i].lease.token == 0u) {
            free_slot = &policy->aborts[i];
            break;
        }
    }
    if (free_slot == NULL) {
        return -ENOSPC;
    }

    free_slot->lease = (struct app_mesh_radio_owner_abort_lease) {
        .token = allocate_generation(policy),
        .kind = kind,
    };
    *lease_in_out = free_slot->lease;
    return 0;
}

int app_mesh_radio_owner_policy_abort_release(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_abort_lease *lease)
{
    if (policy == NULL || lease == NULL || lease->token == 0u ||
        !abort_kind_valid(lease->kind)) {
        return -EINVAL;
    }

    for (size_t i = 0u; i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
        struct app_mesh_radio_owner_abort_slot *slot = &policy->aborts[i];

        if (slot->lease.token != lease->token) {
            continue;
        }
        if (slot->lease.kind != lease->kind) {
            return -ESTALE;
        }

        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    return -ESTALE;
}

bool app_mesh_radio_owner_policy_abort_pending(
    const struct app_mesh_radio_owner_policy *policy)
{
    return app_mesh_radio_owner_policy_abort_count(policy) != 0u;
}

size_t app_mesh_radio_owner_policy_abort_count(
    const struct app_mesh_radio_owner_policy *policy)
{
    size_t count = 0u;

    if (policy == NULL) {
        return 0u;
    }

    for (size_t i = 0u; i < APP_MESH_RADIO_ABORT_LEASE_CAPACITY; i++) {
        if (policy->aborts[i].lease.token != 0u) {
            count++;
        }
    }
    return count;
}

bool app_mesh_radio_owner_policy_rx_scheduled_control_request(
    struct app_mesh_radio_owner_policy *policy,
    bool *abort_scan)
{
    if (policy == NULL || abort_scan == NULL) {
        return false;
    }

    policy->rx_scheduled_control_pending = true;
    *abort_scan = policy->rx_scan_active;
    return true;
}

bool app_mesh_radio_owner_policy_rx_scheduled_control_pending(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL && policy->rx_scheduled_control_pending;
}

bool app_mesh_radio_owner_policy_rx_scheduled_control_ready(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL && policy->rx_scheduled_control_pending &&
           !policy->rx_scan_active;
}

bool app_mesh_radio_owner_policy_rx_scheduled_control_end(
    struct app_mesh_radio_owner_policy *policy)
{
    bool was_pending;

    if (policy == NULL) {
        return false;
    }
    was_pending = policy->rx_scheduled_control_pending;
    policy->rx_scheduled_control_pending = false;
    return was_pending;
}

bool app_mesh_radio_owner_policy_rx_inline_control_begin(
    struct app_mesh_radio_owner_policy *policy,
    bool *abort_scan)
{
    if (policy == NULL || abort_scan == NULL ||
        policy->rx_inline_control_active) {
        return false;
    }

    policy->rx_inline_control_active = true;
    *abort_scan = policy->rx_scan_active;
    return true;
}

bool app_mesh_radio_owner_policy_rx_inline_control_ready(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL && policy->rx_inline_control_active &&
           !policy->rx_scan_active;
}

void app_mesh_radio_owner_policy_rx_inline_control_end(
    struct app_mesh_radio_owner_policy *policy)
{
    if (policy != NULL) {
        policy->rx_inline_control_active = false;
    }
}

bool app_mesh_radio_owner_policy_rx_scan_try_begin(
    struct app_mesh_radio_owner_policy *policy)
{
    if (policy == NULL || policy->rx_scheduled_control_pending ||
        policy->rx_inline_control_active || policy->rx_scan_active) {
        return false;
    }

    policy->rx_scan_active = true;
    return true;
}

void app_mesh_radio_owner_policy_rx_scan_end(
    struct app_mesh_radio_owner_policy *policy)
{
    if (policy != NULL) {
        policy->rx_scan_active = false;
    }
}

bool app_mesh_radio_owner_policy_rx_scan_rearm_allowed(
    const struct app_mesh_radio_owner_policy *policy)
{
    return policy != NULL && !policy->rx_scheduled_control_pending &&
           !policy->rx_inline_control_active;
}
