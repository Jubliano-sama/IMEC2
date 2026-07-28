#include "app_mesh_radio_owner.h"

#include "app_mesh_radio_handoff.h"
#include "app_mesh_radio_owner_policy.h"

#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_radio_owner, LOG_LEVEL_DBG);

static struct app_mesh_radio_owner_policy radio_policy;
static struct k_spinlock radio_lock;
static struct app_mesh_radio_owner_platform_ops platform_ops;
static const char *active_reason;
static uint64_t active_since_ms;

static struct app_mesh_radio_owner_gateway_ops gateway_ops;
static struct app_mesh_radio_owner_abort_lease gateway_abort;
static struct app_mesh_radio_owner_abort_lease scheduled_control_abort;
static struct app_mesh_radio_owner_abort_lease inline_control_abort;

static bool lease_identity_matches(
    const struct app_mesh_radio_owner_handoff_lease *lease,
    const struct k_work_delayable *work)
{
    return lease != NULL && work != NULL &&
           lease->identity == (uintptr_t)work;
}

static void sync_receive_abort_locked(void)
{
    if (app_mesh_radio_owner_policy_abort_pending(&radio_policy)) {
        platform_ops.request_receive_abort();
    } else {
        platform_ops.clear_receive_abort();
    }
}

static void abort_release_locked(
    struct app_mesh_radio_owner_abort_lease *lease)
{
    if (lease->token != 0u) {
        (void)app_mesh_radio_owner_policy_abort_release(
            &radio_policy, lease);
    }
    sync_receive_abort_locked();
    memset(lease, 0, sizeof(*lease));
}

static void gateway_abort_release_locked(void)
{
    abort_release_locked(&gateway_abort);
}

static int radio_try_claim(
    enum app_mesh_radio_client client,
    const char *reason,
    struct app_mesh_radio_owner_lease *lease_out,
    bool rx_scan)
{
    struct app_mesh_radio_owner_lease active = {0};
    const char *blocked_reason = NULL;
    uint64_t blocked_since_ms = 0u;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    k_spinlock_key_t key;
    int ret;

    if (reason == NULL || lease_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    if (app_mesh_radio_owner_policy_paused(&radio_policy)) {
        ret = -ESHUTDOWN;
    } else if (app_mesh_radio_owner_policy_abort_pending(&radio_policy)) {
        ret = -ECANCELED;
    } else if (rx_scan &&
               !app_mesh_radio_owner_policy_rx_scan_try_begin(
                   &radio_policy)) {
        ret = -ECANCELED;
    } else {
        ret = app_mesh_radio_owner_policy_try_claim(
            &radio_policy, client, lease_out);
        if (ret < 0 && rx_scan) {
            app_mesh_radio_owner_policy_rx_scan_end(&radio_policy);
        }
    }
    if (ret == 0) {
        active_reason = reason;
        active_since_ms = now_ms;
    } else if (app_mesh_radio_owner_policy_claim_snapshot(
                   &radio_policy, &active)) {
        blocked_reason = active_reason;
        blocked_since_ms = active_since_ms;
    }
    k_spin_unlock(&radio_lock, key);

    if (ret == -EBUSY && active.generation != 0u) {
        LOG_ERR("blocked nested UWB operation: %s owner=%s client=%u age_ms=%llu",
                reason,
                blocked_reason == NULL ? "unknown" : blocked_reason,
                (unsigned int)active.client,
                (unsigned long long)(now_ms - blocked_since_ms));
    }
    if (ret < 0) {
        return ret;
    }

    if (platform_ops.enter_uwb_quiet != NULL) {
        platform_ops.enter_uwb_quiet(reason);
    }
    return 0;
}

int app_mesh_radio_owner_try_claim(
    enum app_mesh_radio_client client,
    const char *reason,
    struct app_mesh_radio_owner_lease *lease_out)
{
    return radio_try_claim(client, reason, lease_out, false);
}

int app_mesh_radio_owner_rx_scan_try_claim(
    const char *reason,
    struct app_mesh_radio_owner_lease *lease_out)
{
    return radio_try_claim(
        APP_MESH_RADIO_CLIENT_MESH_RX, reason, lease_out, true);
}

static int radio_release(
    struct app_mesh_radio_owner_lease *lease,
    bool rx_scan)
{
    k_spinlock_key_t key;
    int ret;

    if (lease == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    ret = app_mesh_radio_owner_policy_release_begin(&radio_policy, lease);
    k_spin_unlock(&radio_lock, key);
    if (ret < 0) {
        LOG_ERR("rejected stale UWB release: generation=%u client=%u ret=%d",
                lease->generation,
                (unsigned int)lease->client,
                ret);
        return ret;
    }

    if (platform_ops.exit_uwb_quiet != NULL) {
        platform_ops.exit_uwb_quiet("app_mesh_radio_owner");
    }

    key = k_spin_lock(&radio_lock);
    ret = app_mesh_radio_owner_policy_release_complete(&radio_policy, lease);
    if (ret == 0) {
        if (rx_scan) {
            app_mesh_radio_owner_policy_rx_scan_end(&radio_policy);
        }
        active_reason = NULL;
        active_since_ms = 0u;
        memset(lease, 0, sizeof(*lease));
    }
    k_spin_unlock(&radio_lock, key);
    if (ret < 0) {
        LOG_ERR("UWB release completion lost ownership: ret=%d", ret);
    }
    return ret;
}

int app_mesh_radio_owner_release(
    struct app_mesh_radio_owner_lease *lease)
{
    return radio_release(lease, false);
}

int app_mesh_radio_owner_rx_scan_release(
    struct app_mesh_radio_owner_lease *lease)
{
    if (lease == NULL ||
        lease->client != APP_MESH_RADIO_CLIENT_MESH_RX) {
        return -EINVAL;
    }
    return radio_release(lease, true);
}

bool app_mesh_radio_owner_busy(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool busy = app_mesh_radio_owner_policy_busy(&radio_policy);

    k_spin_unlock(&radio_lock, key);
    return busy;
}

int app_mesh_radio_owner_pause(
    struct app_mesh_radio_owner_pause_lease *lease_in_out)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    int ret = app_mesh_radio_owner_policy_pause(&radio_policy, lease_in_out);

    k_spin_unlock(&radio_lock, key);
    return ret;
}

int app_mesh_radio_owner_resume(
    struct app_mesh_radio_owner_pause_lease *lease_in_out)
{
    k_spinlock_key_t key;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&radio_lock);
    ret = app_mesh_radio_owner_policy_resume(&radio_policy, lease_in_out);
    if (ret == 0) {
        memset(lease_in_out, 0, sizeof(*lease_in_out));
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

bool app_mesh_radio_owner_paused(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool paused = app_mesh_radio_owner_policy_paused(&radio_policy);

    k_spin_unlock(&radio_lock, key);
    return paused;
}

static int abort_request_locked(
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease)
{
    int ret = app_mesh_radio_owner_policy_abort_request(
        &radio_policy, kind, lease);

    if (ret == 0) {
        /*
         * The driver observes this as a level until the last exact logical
         * lease reaches its safe boundary. Reassert on an idempotent request
         * as a defensive repair if platform state was disturbed.
         */
        platform_ops.request_receive_abort();
    }
    return ret;
}

int app_mesh_radio_owner_abort_request(
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease_in_out)
{
    k_spinlock_key_t key;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&radio_lock);
    ret = abort_request_locked(kind, lease_in_out);
    k_spin_unlock(&radio_lock, key);
    return ret;
}

int app_mesh_radio_owner_abort_release(
    struct app_mesh_radio_owner_abort_lease *lease_in_out)
{
    k_spinlock_key_t key;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&radio_lock);
    if (lease_in_out->token == 0u) {
        k_spin_unlock(&radio_lock, key);
        return 0;
    }
    ret = app_mesh_radio_owner_policy_abort_release(
        &radio_policy, lease_in_out);
    if (ret == 0) {
        sync_receive_abort_locked();
        memset(lease_in_out, 0, sizeof(*lease_in_out));
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

bool app_mesh_radio_owner_abort_pending(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool pending = app_mesh_radio_owner_policy_abort_pending(&radio_policy);

    k_spin_unlock(&radio_lock, key);
    return pending;
}

bool app_mesh_radio_owner_rx_scan_rearm_allowed(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool allowed = app_mesh_radio_owner_policy_rx_scan_rearm_allowed(
        &radio_policy);

    k_spin_unlock(&radio_lock, key);
    return allowed;
}

int app_mesh_radio_owner_scheduled_control_begin(
    bool *wait_for_scan_boundary)
{
    bool abort_scan = false;
    k_spinlock_key_t key;
    int ret = 0;

    if (wait_for_scan_boundary == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&radio_lock);
    if (!app_mesh_radio_owner_policy_rx_scheduled_control_request(
            &radio_policy, &abort_scan)) {
        ret = -EINVAL;
    } else if (abort_scan) {
        ret = abort_request_locked(
            APP_MESH_RADIO_ABORT_SCHEDULED_DELIVERY,
            &scheduled_control_abort);
        if (ret < 0) {
            (void)app_mesh_radio_owner_policy_rx_scheduled_control_end(
                &radio_policy);
        }
    }
    k_spin_unlock(&radio_lock, key);
    *wait_for_scan_boundary = ret == 0 && abort_scan;
    return ret;
}

bool app_mesh_radio_owner_scheduled_control_pending(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool pending =
        app_mesh_radio_owner_policy_rx_scheduled_control_pending(
            &radio_policy);

    k_spin_unlock(&radio_lock, key);
    return pending;
}

bool app_mesh_radio_owner_scheduled_control_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool ready = app_mesh_radio_owner_policy_rx_scheduled_control_ready(
        &radio_policy);

    if (ready) {
        abort_release_locked(&scheduled_control_abort);
    }
    k_spin_unlock(&radio_lock, key);
    return ready;
}

bool app_mesh_radio_owner_scheduled_control_end(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool was_pending =
        app_mesh_radio_owner_policy_rx_scheduled_control_end(&radio_policy);

    abort_release_locked(&scheduled_control_abort);
    k_spin_unlock(&radio_lock, key);
    return was_pending;
}

int app_mesh_radio_owner_inline_control_begin(bool *abort_scan)
{
    k_spinlock_key_t key;
    int ret = 0;

    if (abort_scan == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&radio_lock);
    if (!app_mesh_radio_owner_policy_rx_inline_control_begin(
            &radio_policy, abort_scan)) {
        ret = -EBUSY;
    } else if (*abort_scan) {
        ret = abort_request_locked(
            APP_MESH_RADIO_ABORT_INLINE_CONTROL,
            &inline_control_abort);
        if (ret < 0) {
            app_mesh_radio_owner_policy_rx_inline_control_end(
                &radio_policy);
        }
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

bool app_mesh_radio_owner_inline_control_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);
    bool ready = app_mesh_radio_owner_policy_rx_inline_control_ready(
        &radio_policy);

    if (ready) {
        abort_release_locked(&inline_control_abort);
    }
    k_spin_unlock(&radio_lock, key);
    return ready;
}

void app_mesh_radio_owner_inline_control_end(void)
{
    k_spinlock_key_t key = k_spin_lock(&radio_lock);

    app_mesh_radio_owner_policy_rx_inline_control_end(&radio_policy);
    abort_release_locked(&inline_control_abort);
    k_spin_unlock(&radio_lock, key);
}

int app_mesh_radio_owner_gateway_command_submit(
    const struct app_mesh_radio_owner_gateway_ops *ops,
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_out)
{
    enum app_mesh_radio_handoff_phase previous_phase;
    k_spinlock_key_t key;
    int ret;

    if (ops == NULL || !ops->gateway_role || work == NULL ||
        lease_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    previous_phase =
        app_mesh_radio_owner_policy_handoff_phase(&radio_policy);
    ret = app_mesh_radio_owner_policy_handoff_request(
        &radio_policy, (uintptr_t)work, lease_out);
    if (ret == 0 && previous_phase == APP_MESH_RADIO_HANDOFF_IDLE) {
        ret = app_mesh_radio_owner_policy_abort_request(
            &radio_policy,
            APP_MESH_RADIO_ABORT_HOST_COMMAND,
            &gateway_abort);
        if (ret == 0) {
            gateway_ops = *ops;
            platform_ops.request_receive_abort();
        } else {
            (void)app_mesh_radio_owner_policy_handoff_cancel(
                &radio_policy, lease_out);
            memset(lease_out, 0, sizeof(*lease_out));
        }
    } else if (ret == 0) {
        gateway_ops.schedule_failure_token =
            ops->schedule_failure_token;
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

int app_mesh_radio_owner_gateway_safe_boundary(
    struct app_mesh_radio_owner_handoff_lease *lease_in_out)
{
    struct app_mesh_radio_owner_handoff_lease scheduled_lease;
    struct k_work_delayable *work;
    struct k_work_q *queue;
    app_mesh_radio_owner_schedule_failure_fn failure_handler;
    void *failure_ctx;
    uint32_t failure_token;
    k_spinlock_key_t key;
    int completion_ret;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    if (lease_in_out->generation == 0u) {
        k_spin_unlock(&radio_lock, key);
        return 0;
    }
    ret = app_mesh_radio_owner_policy_handoff_begin(
        &radio_policy, lease_in_out);
    if (ret == -EALREADY) {
        k_spin_unlock(&radio_lock, key);
        return 0;
    }
    if (ret < 0) {
        k_spin_unlock(&radio_lock, key);
        return ret;
    }
    scheduled_lease = *lease_in_out;
    failure_handler = gateway_ops.schedule_failure;
    failure_ctx = gateway_ops.schedule_failure_ctx;
    failure_token = gateway_ops.schedule_failure_token;
    ret = app_mesh_radio_owner_policy_handoff_schedule_complete(
        &radio_policy, &scheduled_lease, true);
    if (ret == 0) {
        work = (struct k_work_delayable *)scheduled_lease.identity;
        queue = gateway_ops.priority_work_queue;
        ret = queue == NULL ?
            k_work_reschedule(work, K_NO_WAIT) :
            k_work_reschedule_for_queue(queue, work, K_NO_WAIT);
    }
    if (ret < 0) {
        completion_ret =
            app_mesh_radio_owner_policy_handoff_schedule_complete(
                &radio_policy, &scheduled_lease, false);
        if (completion_ret < 0) {
            LOG_ERR("failed to serialize radio scheduling failure: %d",
                    completion_ret);
        }
        gateway_abort_release_locked();
        memset(lease_in_out, 0, sizeof(*lease_in_out));
    }
    k_spin_unlock(&radio_lock, key);

    if (ret < 0 && failure_handler != NULL) {
        failure_handler(failure_ctx, ret, failure_token);
    }
    if (ret < 0) {
        key = k_spin_lock(&radio_lock);
        completion_ret =
            app_mesh_radio_owner_policy_handoff_failure_complete(
                &radio_policy, &scheduled_lease);
        if (completion_ret == 0) {
            memset(&gateway_ops, 0, sizeof(gateway_ops));
        }
        k_spin_unlock(&radio_lock, key);
        if (completion_ret < 0) {
            LOG_ERR("failed to complete radio scheduling failure: %d",
                    completion_ret);
        }
    }
    return ret;
}

int app_mesh_radio_owner_gateway_command_begin(
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_in_out)
{
    k_spinlock_key_t key;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    if (lease_in_out->generation == 0u) {
        k_spin_unlock(&radio_lock, key);
        return -ENOENT;
    }
    if (!lease_identity_matches(lease_in_out, work)) {
        k_spin_unlock(&radio_lock, key);
        return -ESTALE;
    }
    ret = app_mesh_radio_owner_policy_handoff_take_grant(
        &radio_policy, lease_in_out);
    if (ret == 0) {
        gateway_abort_release_locked();
        memset(lease_in_out, 0, sizeof(*lease_in_out));
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

int app_mesh_radio_owner_gateway_command_cancel(
    struct k_work_delayable *work,
    struct app_mesh_radio_owner_handoff_lease *lease_in_out)
{
    k_spinlock_key_t key;
    int ret;

    if (lease_in_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    if (lease_in_out->generation == 0u) {
        k_spin_unlock(&radio_lock, key);
        return 0;
    }
    if (!lease_identity_matches(lease_in_out, work)) {
        k_spin_unlock(&radio_lock, key);
        return -ESTALE;
    }
    ret = app_mesh_radio_owner_policy_handoff_cancel(
        &radio_policy, lease_in_out);
    if (ret == 0) {
        gateway_abort_release_locked();
        memset(lease_in_out, 0, sizeof(*lease_in_out));
    }
    k_spin_unlock(&radio_lock, key);
    return ret;
}

int app_mesh_radio_owner_init(
    const struct app_mesh_radio_owner_platform_ops *ops)
{
    k_spinlock_key_t key;

    if (ops == NULL || ops->request_receive_abort == NULL ||
        ops->clear_receive_abort == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&radio_lock);
    app_mesh_radio_owner_policy_reset(&radio_policy);
    platform_ops = *ops;
    active_reason = NULL;
    active_since_ms = 0u;
    memset(&gateway_ops, 0, sizeof(gateway_ops));
    memset(&gateway_abort, 0, sizeof(gateway_abort));
    memset(&scheduled_control_abort, 0,
           sizeof(scheduled_control_abort));
    memset(&inline_control_abort, 0, sizeof(inline_control_abort));
    platform_ops.clear_receive_abort();
    k_spin_unlock(&radio_lock, key);
    return 0;
}
