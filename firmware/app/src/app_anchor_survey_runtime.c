#include "app_anchor_survey_runtime.h"

#include "app_anchor_survey_discovery.h"
#include "app_anchor_survey_result_delivery.h"
#include "app_board.h"
#include "app_config.h"
#include "app_durable_state.h"
#include "app_node_comm.h"
#include "app_operation_policy.h"
#include "app_state.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "status.h"
#include "survey_anchor_deadline.h"
#include "survey_pair_lease.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

static struct app_anchor_survey_runtime_ops runtime_ops;
static bool runtime_initialized;
static bool runtime_started;
static bool runtime_work_queue_ready;
static uint16_t survey_sequence;
static struct k_spinlock survey_lock;
static struct survey_pair_lease pair_lease;
static struct survey_discovery_config discovery_config;
static uint32_t discovery_start_ms;
static bool pair_start_pending;
static uint32_t pair_start_delivery_handle;
static uint32_t pair_start_failed_abandon_handle;
static bool survey_running;
static bool discovery_pending;
static bool discovery_generation_active;
static bool discovery_report_stage_pending;
static atomic_t abort_requested;
static struct k_work_delayable survey_work;
static struct k_work_delayable pair_lease_work;
static struct k_work_delayable pair_start_kick_work;
static uint64_t pair_cleanup_generation;
static struct survey_anchor_deadline_registry survey_deadlines;
static struct survey_pair_control_id pair_start_kick_id;
static uint32_t pair_start_kick_delivery_handle;
static bool pair_start_kick_active;
static uint64_t survey_generation_high_watermark;
static uint64_t survey_generation_active;
static bool survey_generation_restored;
K_MUTEX_DEFINE(survey_generation_admission_mutex);

#define SURVEY_START_DELIVERY_POLL_MS 5u
#define SURVEY_START_BIND_RETRY_MS 10u
#define SURVEY_NON_RF_SERVICE_POLL_MS REPORT_TX_RETRY_DELAY_MS

struct survey_rf_retry_state {
    uint32_t survey_id;
    uint32_t opportunity;
    uint16_t retry_round;
    bool valid;
};

static struct survey_rf_retry_state discovery_rf_retry;
static struct survey_rf_retry_state pair_rf_retry;

static void pair_start_kick_clear_locked(void);
static void pair_lease_work_handler(struct k_work *work);

/* The survey worker owns its exact radio lease until the DWM3000 is parked. */
static int survey_radio_release(struct radio_guard_uwb_lease *lease,
                                int parking_ret)
{
    int ret;

    ret = radio_guard_uwb_release_begin(lease);
    if (ret < 0) {
        return ret;
    }
    return radio_guard_uwb_release_finish(lease, parking_ret);
}

static bool pair_queueable(const struct survey_pair *pair)
{
    return pair != NULL &&
           pair->sample_count <= SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT &&
           pair->sample_count <= REPORT_TX_QUEUE_DEPTH;
}

static bool discovery_config_matches_locked(
    const struct survey_discovery_config *config)
{
    return config != NULL &&
           discovery_generation_active &&
           discovery_config.operation_generation ==
               config->operation_generation &&
           discovery_config.survey_id == config->survey_id;
}

static int schedule_physical(k_timeout_t delay)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    if (runtime_work_queue_ready && runtime_ops.work_queue != NULL) {
        return k_work_reschedule_for_queue(runtime_ops.work_queue,
                                           &survey_work,
                                           delay);
    }
    return -ENODEV;
#else
    ARG_UNUSED(delay);
    return -ENOTSUP;
#endif
}

static int schedule_result_checked(int ret, const char *owner)
{
    if (ret < 0) {
        LOG_ERR("anchor survey work scheduling failed: owner=%s ret=%d",
                owner == NULL ? "unknown" : owner,
                ret);
        /*
         * The caller has already published protocol or bounded-RAM custody.
         * A rejected sole work owner must not be hidden by unrelated healthy
         * system-workqueue watchdog progress.
         */
        app_watchdog_stop_feeding();
    }
    return ret;
}

/* Caller holds survey_lock. */
static int deadline_rearm_locked(uint32_t now_ms)
{
    uint32_t delay_ms;

    if (!survey_anchor_deadline_next(&survey_deadlines,
                                     now_ms,
                                     &delay_ms)) {
        return 0;
    }
    return schedule_physical(K_MSEC(delay_ms));
}

/* Caller holds survey_lock. */
static int deadline_schedule_locked(
    enum survey_anchor_deadline_owner deadline_owner,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t delay_ms)
{
    int ret = survey_anchor_deadline_schedule_after(&survey_deadlines,
                                                    deadline_owner,
                                                    generation,
                                                    now_ms,
                                                    delay_ms);

    if (ret < 0) {
        return ret;
    }
    return deadline_rearm_locked(now_ms);
}

static int deadline_schedule_raw(
    enum survey_anchor_deadline_owner deadline_owner,
    uint64_t generation,
    uint32_t delay_ms)
{
    const uint32_t now_ms = k_uptime_get_32();
    k_spinlock_key_t key = k_spin_lock(&survey_lock);
    int ret = deadline_schedule_locked(deadline_owner,
                                       generation,
                                       now_ms,
                                       delay_ms);

    k_spin_unlock(&survey_lock, key);
    return ret;
}

static int deadline_schedule(
    enum survey_anchor_deadline_owner deadline_owner,
    uint64_t generation,
    uint32_t delay_ms,
    const char *owner)
{
    return schedule_result_checked(
        deadline_schedule_raw(deadline_owner, generation, delay_ms),
        owner);
}

static void deadline_cancel(enum survey_anchor_deadline_owner owner,
                            uint64_t generation,
                            const char *reason)
{
    const uint32_t now_ms = k_uptime_get_32();
    k_spinlock_key_t key = k_spin_lock(&survey_lock);
    int ret;

    (void)survey_anchor_deadline_cancel(&survey_deadlines,
                                        owner,
                                        generation);
    ret = deadline_rearm_locked(now_ms);
    k_spin_unlock(&survey_lock, key);
    (void)schedule_result_checked(ret, reason);
}

static void deadline_cancel_pair(uint64_t generation, const char *reason)
{
    const uint32_t now_ms = k_uptime_get_32();
    k_spinlock_key_t key;
    int ret;

    if (generation == 0u) {
        return;
    }
    key = k_spin_lock(&survey_lock);
    (void)survey_anchor_deadline_cancel(
        &survey_deadlines, SURVEY_ANCHOR_DEADLINE_OPERATION, generation);
    (void)survey_anchor_deadline_cancel(
        &survey_deadlines, SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY, generation);
    (void)survey_anchor_deadline_cancel(
        &survey_deadlines,
        SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
        generation);
    ret = deadline_rearm_locked(now_ms);
    k_spin_unlock(&survey_lock, key);
    (void)schedule_result_checked(ret, reason);
}

static int pair_cleanup_schedule_exact(uint64_t generation,
                                       uint32_t delay_ms)
{
    bool current;
    k_spinlock_key_t key;
    int ret;

    if (generation == 0u || k_is_in_isr()) {
        return -EINVAL;
    }
    key = k_spin_lock(&survey_lock);
    current = pair_lease.pair.operation_generation == generation &&
              (pair_lease.phase == SURVEY_PAIR_LEASE_PREPARED ||
               pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING);
    if (current) {
        pair_cleanup_generation = generation;
    }
    k_spin_unlock(&survey_lock, key);
    if (!current) {
        return -ESTALE;
    }
    key = k_spin_lock(&survey_lock);
    if (pair_cleanup_generation != generation) {
        k_spin_unlock(&survey_lock, key);
        return -ESTALE;
    }
    ret = k_work_reschedule(&pair_lease_work, K_MSEC(delay_ms));
    if (ret < 0) {
        if (pair_cleanup_generation == generation) {
            pair_cleanup_generation = 0u;
        }
    }
    k_spin_unlock(&survey_lock, key);
    return ret;
}

static void pair_cleanup_cancel_exact(uint64_t generation)
{
    bool current = false;
    k_spinlock_key_t key;

    if (generation == 0u || k_is_in_isr()) {
        return;
    }
    key = k_spin_lock(&survey_lock);
    if (pair_cleanup_generation == generation) {
        pair_cleanup_generation = 0u;
        current = true;
    }
    if (current) {
        (void)k_work_cancel_delayable(&pair_lease_work);
    }
    k_spin_unlock(&survey_lock, key);
}

static struct survey_anchor_deadline_events deadline_take_due(void)
{
    struct survey_anchor_deadline_events events;
    const uint32_t now_ms = k_uptime_get_32();
    k_spinlock_key_t key = k_spin_lock(&survey_lock);
    int ret;

    survey_anchor_deadline_take_due(&survey_deadlines, now_ms, &events);
    ret = deadline_rearm_locked(now_ms);
    k_spin_unlock(&survey_lock, key);
    (void)schedule_result_checked(ret, "deadline-rearm");
    return events;
}

static int schedule_pair_unless_discovery_pending(
                                                  enum survey_anchor_deadline_owner deadline_owner,
                                                  uint64_t generation,
                                                  uint32_t delay_ms,
                                                  const char *owner)
{
    k_spinlock_key_t key = k_spin_lock(&survey_lock);
    const uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (discovery_pending) {
        k_spin_unlock(&survey_lock, key);
        return -ECANCELED;
    }
    ret = deadline_schedule_locked(deadline_owner,
                                   generation,
                                   now_ms,
                                   delay_ms);
    k_spin_unlock(&survey_lock, key);
    return schedule_result_checked(ret, owner);
}

static int schedule_discovery_if_current(
    const struct survey_discovery_config *config,
    enum survey_anchor_deadline_owner deadline_owner,
    uint32_t delay_ms,
    const char *owner)
{
    k_spinlock_key_t key = k_spin_lock(&survey_lock);
    const uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (!discovery_config_matches_locked(config)) {
        k_spin_unlock(&survey_lock, key);
        return -ECANCELED;
    }
    ret = deadline_schedule_locked(deadline_owner,
                                   config->operation_generation,
                                   now_ms,
                                   delay_ms);
    k_spin_unlock(&survey_lock, key);
    return schedule_result_checked(ret, owner);
}

static void survey_rf_retry_reset(struct survey_rf_retry_state *state)
{
    if (state != NULL) {
        *state = (struct survey_rf_retry_state) {0};
    }
}

static int survey_rf_retry_delay_ms(struct survey_rf_retry_state *state,
                                    uint32_t survey_id,
                                    uint32_t opportunity,
                                    uint32_t absolute_deadline_ms,
                                    uint32_t *delay_ms_out)
{
    uint32_t remaining_ms;
    uint32_t now_ms;
    int ret;

    if (state == NULL || survey_id == 0u || delay_ms_out == NULL) {
        return -EINVAL;
    }
    now_ms = k_uptime_get_32();
    if (uptime_deadline_reached(now_ms, absolute_deadline_ms)) {
        return -ETIMEDOUT;
    }
    if (!state->valid || state->survey_id != survey_id ||
        state->opportunity != opportunity) {
        *state = (struct survey_rf_retry_state) {
            .survey_id = survey_id,
            .opportunity = opportunity,
            .valid = true,
        };
    }
    if (state->retry_round < UINT16_MAX) {
        state->retry_round++;
    }
    ret = app_node_comm_retry_identity_backoff_ms(
        DEVICE_ID,
        survey_id,
        opportunity,
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        state->retry_round,
        delay_ms_out);
    if (ret < 0) {
        return ret;
    }
    remaining_ms = uptime_ms_until_deadline(now_ms, absolute_deadline_ms);
    if (*delay_ms_out > remaining_ms) {
        *delay_ms_out = remaining_ms;
    }
    status_debug_printf(
        "DBG_SURVEY_RF_DEFER survey=%u opportunity=%u round=%u delay_ms=%u deadline_ms=%u\n",
        survey_id,
        opportunity,
        state->retry_round,
        *delay_ms_out,
        absolute_deadline_ms);
    return 0;
}

static uint32_t survey_discovery_radio_deadline_ms(
    const struct survey_discovery_config *config,
    uint32_t start_ms)
{
    return start_ms + survey_discovery_duration_ms(config);
}

static uint32_t survey_discovery_radio_opportunity(void)
{
    return (uint32_t)MSG_SURVEY_DISCOVERY_START << 16;
}

static uint32_t survey_pair_radio_opportunity(
    const struct survey_pair_control_id *control_id)
{
    return ((uint32_t)MSG_COMMAND << 16) | control_id->command_seq;
}

static bool schedule_pair_rf_retry(
    const struct survey_pair_control_id *control_id,
    uint64_t operation_generation,
    uint32_t absolute_deadline_ms,
    const char *reason)
{
    uint32_t retry_delay_ms = 0u;
    uint32_t delivery_handle = 0u;
    bool cancel_start_kick = false;
    bool still_current;
    k_spinlock_key_t key;
    int ret;

    ret = survey_rf_retry_delay_ms(
        &pair_rf_retry,
        control_id->session_id,
        survey_pair_radio_opportunity(control_id),
        absolute_deadline_ms,
        &retry_delay_ms);
    if (ret == 0) {
        ret = schedule_pair_unless_discovery_pending(
            SURVEY_ANCHOR_DEADLINE_OPERATION,
            operation_generation,
            retry_delay_ms,
            reason == NULL ? "pair-rf-retry" : reason);
        return ret >= 0;
    }

    key = k_spin_lock(&survey_lock);
    still_current = pair_start_pending && pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == control_id->session_id &&
        pair_lease.start_id.command_seq == control_id->command_seq;
    if (still_current) {
        delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        (void)survey_pair_lease_abort(&pair_lease);
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (still_current) {
        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_OPERATION,
                        operation_generation,
                        "pair-rf-retry-expired-operation");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                        operation_generation,
                        "pair-rf-retry-expired-phase");
        pair_cleanup_cancel_exact(operation_generation);
        (void)app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "pair-rf-retry-expired");
    }
    survey_rf_retry_reset(&pair_rf_retry);
    LOG_ERR("survey pair RF retry terminated: survey=%u seq=%u ret=%d reason=%s",
            control_id->session_id,
            control_id->command_seq,
            ret,
            reason == NULL ? "radio-deadline" : reason);
    return false;
}

int app_anchor_survey_runtime_abandon_pair_start_delivery(
    uint32_t delivery_handle,
    const char *reason)
{
    uint32_t retained_handle;
    k_spinlock_key_t key;
    int ret;

    if (delivery_handle == 0u) {
        return 0;
    }
    ret = app_node_comm_abandon_delivery(delivery_handle);
    status_debug_printf(
        "DBG_SURVEY_PAIR_START_ABANDON handle=%u ret=%d reason=%s\n",
        delivery_handle,
        ret,
        reason == NULL ? "pair-state-release" : reason);
    if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
        /*
         * The caller may already have detached this handle while replacing its
         * lease identity. Retain exact orphan custody and reject further
         * survey-generation work until watchdog recovery; otherwise the live
         * COMMAND_OK delivery could outlast all state that can identify it.
         */
        key = k_spin_lock(&survey_lock);
        if (pair_start_failed_abandon_handle == 0u ||
            pair_start_failed_abandon_handle == delivery_handle) {
            pair_start_failed_abandon_handle = delivery_handle;
        }
        retained_handle = pair_start_failed_abandon_handle;
        k_spin_unlock(&survey_lock, key);
        LOG_ERR("survey pair start delivery abandon failed: handle=%u ret=%d reason=%s retained=%u",
                delivery_handle,
                ret,
                reason == NULL ? "pair-state-release" : reason,
                retained_handle);
        app_watchdog_stop_feeding();
        return ret;
    }
    key = k_spin_lock(&survey_lock);
    if (pair_start_failed_abandon_handle == delivery_handle) {
        pair_start_failed_abandon_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    return 0;
}

/* Caller holds survey_lock. */
static bool pair_start_kick_matches_locked(
    const struct survey_pair_control_id *control_id,
    uint32_t delivery_handle)
{
    return control_id != NULL && pair_start_kick_active &&
           pair_start_kick_delivery_handle == delivery_handle &&
           pair_start_kick_id.session_id == control_id->session_id &&
           pair_start_kick_id.command_seq == control_id->command_seq &&
           pair_start_pending &&
           pair_start_delivery_handle == delivery_handle &&
           pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
           pair_lease.start_id_valid &&
           pair_lease.start_id.session_id == control_id->session_id &&
           pair_lease.start_id.command_seq == control_id->command_seq;
}

/* Caller holds survey_lock. */
static void pair_start_kick_clear_locked(void)
{
    pair_start_kick_active = false;
    pair_start_kick_delivery_handle = 0u;
    pair_start_kick_id = (struct survey_pair_control_id) {0};
}

static void pair_start_kick_work_handler(struct k_work *work)
{
    struct survey_pair_control_id control_id = {0};
    uint32_t delivery_handle = 0u;
    uint32_t deadline_ms = 0u;
    uint32_t retry_delay_ms;
    uint64_t operation_generation = 0u;
    bool expired = false;
    bool current = false;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);

    key = k_spin_lock(&survey_lock);
    if (pair_start_kick_active) {
        control_id = pair_start_kick_id;
        delivery_handle = pair_start_kick_delivery_handle;
        current = pair_start_kick_matches_locked(&control_id,
                                                 delivery_handle);
        if (current) {
            deadline_ms = pair_lease.prepared_deadline_ms;
            operation_generation = pair_lease.pair.operation_generation;
            expired = uptime_deadline_reached(k_uptime_get_32(),
                                              deadline_ms);
            if (expired) {
                (void)survey_pair_lease_abort(&pair_lease);
                pair_start_pending = false;
                pair_start_delivery_handle = 0u;
            }
        }
        if (!current || expired) {
            pair_start_kick_clear_locked();
        }
    }
    k_spin_unlock(&survey_lock, key);

    if (!current) {
        return;
    }
    if (expired) {
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                        operation_generation,
                        "pair-start-bind-expired-phase");
        pair_cleanup_cancel_exact(operation_generation);
        app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "pair-start-bind-deadline");
        LOG_ERR("survey pair START bind recovery expired: session=%u seq=%u handle=%u",
                control_id.session_id,
                control_id.command_seq,
                delivery_handle);
        return;
    }

    ret = deadline_schedule_raw(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                                operation_generation,
                                0u);
    if (ret >= 0) {
        key = k_spin_lock(&survey_lock);
        if (pair_start_kick_active &&
            pair_start_kick_delivery_handle == delivery_handle &&
            pair_start_kick_id.session_id == control_id.session_id &&
            pair_start_kick_id.command_seq == control_id.command_seq) {
            pair_start_kick_clear_locked();
        }
        k_spin_unlock(&survey_lock, key);
        status_debug_printf(
            "DBG_SURVEY_PAIR_START_BIND_RECOVERED session=%u seq=%u handle=%u\n",
            control_id.session_id,
            control_id.command_seq,
            delivery_handle);
        return;
    }

    retry_delay_ms = uptime_ms_until_deadline(k_uptime_get_32(), deadline_ms);
    if (retry_delay_ms > SURVEY_START_BIND_RETRY_MS) {
        retry_delay_ms = SURVEY_START_BIND_RETRY_MS;
    }
    ret = k_work_reschedule(&pair_start_kick_work,
                            K_MSEC(retry_delay_ms));
    if (ret < 0) {
        LOG_ERR("survey pair START bind recovery owner lost: session=%u seq=%u handle=%u ret=%d",
                control_id.session_id,
                control_id.command_seq,
                delivery_handle,
                ret);
        app_watchdog_stop_feeding();
    }
}

static int schedule_result_delivery_ms(uint32_t delay_ms)
{
    return deadline_schedule(SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
                             0u,
                             delay_ms,
                             "result-delivery");
}

int app_anchor_survey_runtime_schedule_discovery_custody_ms(
    uint32_t delay_ms)
{
    uint64_t generation;
    int ret;

    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    ret = k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    generation = survey_generation_active;
    k_mutex_unlock(&survey_generation_admission_mutex);
    return deadline_schedule(SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
                             generation,
                             delay_ms,
                             "discovery-custody");
}

uint16_t app_anchor_survey_runtime_next_sequence(void)
{
    uint16_t sequence;
    k_spinlock_key_t key = k_spin_lock(&survey_lock);

    sequence = survey_discovery_sequence_next(&survey_sequence);
    k_spin_unlock(&survey_lock, key);
    return sequence;
}

bool app_anchor_survey_runtime_discovery_is_pending(void)
{
    k_spinlock_key_t key;
    bool pending;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    key = k_spin_lock(&survey_lock);
    pending = discovery_pending;
    k_spin_unlock(&survey_lock, key);
    return pending;
}

bool app_anchor_survey_runtime_radio_active(void)
{
    k_spinlock_key_t key;
    bool active;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    key = k_spin_lock(&survey_lock);
    active = survey_running;
    k_spin_unlock(&survey_lock, key);
    return active;
}

bool app_anchor_survey_runtime_abort_requested(void)
{
    return atomic_get(&abort_requested) != 0;
}

bool app_anchor_survey_runtime_operation_generation_active(
    uint64_t operation_generation)
{
    bool active = false;

    if (operation_generation == 0u || k_is_in_isr() ||
        k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER) < 0) {
        return false;
    }
    active = survey_generation_restored &&
             survey_generation_active == operation_generation;
    k_mutex_unlock(&survey_generation_admission_mutex);
    return active;
}

/*
 * Caller holds survey_generation_admission_mutex.  Keeping persistence,
 * supersession, and the active-generation cursor under one mutex lets the
 * discovery admission path install its successor config before another
 * generation can interleave.
 */
static int survey_generation_admit_locked(uint64_t generation, bool *advanced)
{
    uint32_t superseded_delivery_handle = 0u;
    uint64_t superseded_generation = 0u;
    size_t retiring_pair_results = 0u;
    bool retiring_discovery_report = false;
    bool cancel_start_kick = false;
    int ret;

    if (generation == 0u || (uint32_t)generation == 0u ||
        advanced == NULL || !survey_generation_restored) {
        return -EINVAL;
    }
    {
        k_spinlock_key_t key = k_spin_lock(&survey_lock);
        bool abandon_failed = pair_start_failed_abandon_handle != 0u;

        k_spin_unlock(&survey_lock, key);
        if (abandon_failed) {
            return -EIO;
        }
    }
    *advanced = false;
    if (generation < survey_generation_high_watermark) {
        return -ESTALE;
    }
    if (generation > survey_generation_high_watermark) {
        /*
         * Publish no successor state until its exact gateway-scoped high
         * water is verified on flash.  A failed or ambiguous write leaves
         * the previous active operation untouched and retryable.
         */
        ret = app_durable_state_advance_high_water(
            APP_DURABLE_STATE_SURVEY_GENERATION,
            GATEWAY_ID,
            generation);
        if (ret < 0) {
            status_debug_printf(
                "DBG_SURVEY_GENERATION_ADVANCE_FAILED generation=%llu ret=%d\n",
                (unsigned long long)generation,
                ret);
            return ret;
        }
        survey_generation_high_watermark = generation;
    }
    /*
     * Equality with a restored high water is the exact replay needed after
     * an anchor-only reboot.  It does not write flash again, but it may
     * reconstruct the operation's RAM owner from the gateway's redrive.
     */
    if (survey_generation_active != 0u &&
        survey_generation_active != generation) {
        k_spinlock_key_t key = k_spin_lock(&survey_lock);

        superseded_generation = survey_generation_active;
        atomic_set(&abort_requested, 1);
        if (pair_lease.phase != SURVEY_PAIR_LEASE_IDLE) {
            (void)survey_pair_lease_abort(&pair_lease);
        }
        superseded_delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
        discovery_pending = false;
        discovery_generation_active = false;
        discovery_report_stage_pending = false;
        survey_generation_active = 0u;
        k_spin_unlock(&survey_lock, key);

        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_OPERATION,
                        superseded_generation,
                        "survey-generation-operation");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                        superseded_generation,
                        "survey-generation-phase");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
                        superseded_generation,
                        "survey-generation-custody");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
                        superseded_generation,
                        "survey-generation-pair-admission");
        pair_cleanup_cancel_exact(superseded_generation);
        ret = app_anchor_survey_runtime_abandon_pair_start_delivery(
            superseded_delivery_handle,
            "survey-generation-superseded");
        if (ret < 0) {
            return ret;
        }
        survey_rf_retry_reset(&pair_rf_retry);
        survey_rf_retry_reset(&discovery_rf_retry);
    }
    if (survey_generation_active != generation) {
        survey_generation_active = generation;
        *advanced = true;
    }
    /*
     * The new generation is durable and the predecessor producer is aborted
     * before any old source record is released.  Each delivery module then
     * abandons its exact communication handle before clearing RAM custody.
     * Returning busy keeps the successor control out of executable state
     * until that explicit repair boundary has drained.
     */
    ret = app_anchor_survey_result_delivery_supersede_before(
        generation, &retiring_pair_results);
    if (ret < 0 && ret != -EINPROGRESS) {
        return ret;
    }
    ret = app_anchor_survey_discovery_supersede_before(
        generation, &retiring_discovery_report);
    if (ret < 0 && ret != -EINPROGRESS) {
        return ret;
    }
    if (retiring_pair_results != 0u || retiring_discovery_report) {
        status_debug_printf(
            "DBG_SURVEY_GENERATION_SUPERSEDE_WAIT generation=%llu pair_results=%u discovery=%u\n",
            (unsigned long long)generation,
            (unsigned int)retiring_pair_results,
            retiring_discovery_report ? 1u : 0u);
        return -EBUSY;
    }
    return 0;
}

enum app_anchor_survey_discovery_admission
app_anchor_survey_runtime_admit_discovery(
    const struct survey_discovery_config *config)
{
    enum app_anchor_survey_discovery_admission admission;
    bool advanced = false;
    int custody_status;
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || config == NULL ||
        config->survey_id == 0u ||
        survey_operation_session_id(config->operation_generation) == 0u ||
        !survey_generation_restored || k_is_in_isr()) {
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    ret = k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER);
    if (ret < 0) {
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    ret = survey_generation_admit_locked(
        config->operation_generation, &advanced);
    if (ret < 0) {
        k_mutex_unlock(&survey_generation_admission_mutex);
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    if (app_anchor_survey_result_delivery_occupied_count() > 0u) {
        (void)app_anchor_survey_result_delivery_service();
        k_mutex_unlock(&survey_generation_admission_mutex);
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    custody_status =
        app_anchor_survey_discovery_report_custody_status(
            config->operation_generation);
    if (custody_status == -EALREADY) {
        k_mutex_unlock(&survey_generation_admission_mutex);
        return APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE;
    }
    if (custody_status < 0) {
        k_mutex_unlock(&survey_generation_admission_mutex);
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }

    key = k_spin_lock(&survey_lock);
    if (advanced || (!survey_running && !discovery_generation_active)) {
        discovery_generation_active = true;
        discovery_config = (struct survey_discovery_config) {
            .operation_generation = config->operation_generation,
            .survey_id = config->survey_id,
        };
        admission = APP_ANCHOR_SURVEY_DISCOVERY_ACCEPTED;
    } else if (discovery_config.operation_generation ==
                   config->operation_generation &&
               discovery_config.survey_id == config->survey_id) {
        admission = APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE;
    } else {
        admission = APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    k_spin_unlock(&survey_lock, key);
    k_mutex_unlock(&survey_generation_admission_mutex);
    return admission;
}

int app_anchor_survey_runtime_queue_discovery(
    const struct survey_discovery_config *config,
    uint32_t start_ms,
    uint32_t delay_ms)
{
    bool committed = false;
    bool rolled_back = false;
    uint32_t survey_id;
    k_spinlock_key_t key;
    int restart_ret;
    int ret;

    if (config == NULL || config->survey_id == 0u ||
        !runtime_initialized) {
        return -EINVAL;
    }
    survey_id = config->survey_id;

    key = k_spin_lock(&survey_lock);
    if (discovery_generation_active &&
        discovery_config.operation_generation ==
            config->operation_generation &&
        discovery_config.survey_id == survey_id) {
        discovery_config = *config;
        discovery_start_ms = start_ms;
        discovery_pending = true;
        committed = true;
    }
    k_spin_unlock(&survey_lock, key);
    if (!committed) {
        return -ESTALE;
    }

    ret = deadline_schedule_raw(SURVEY_ANCHOR_DEADLINE_OPERATION,
                                config->operation_generation,
                                delay_ms);
    if (ret >= 0) {
        return 0;
    }

    /*
     * Admission and pending-state publication are useful only when a work
     * owner exists.  If the queue rejects that owner, roll back the exact
     * generation under the same lock.  A concurrent worker that already took
     * pending ownership wins instead, so its generation is left intact.
     */
    key = k_spin_lock(&survey_lock);
    if (discovery_generation_active && discovery_pending &&
        discovery_config.operation_generation ==
            config->operation_generation &&
        discovery_config.survey_id == survey_id) {
        discovery_config = (struct survey_discovery_config) {0};
        discovery_start_ms = 0u;
        discovery_pending = false;
        discovery_generation_active = false;
        rolled_back = true;
    }
    k_spin_unlock(&survey_lock, key);
    if (!rolled_back) {
        return 0;
    }

    deadline_cancel(SURVEY_ANCHOR_DEADLINE_OPERATION,
                    config->operation_generation,
                    "discovery-schedule-rollback");
    survey_rf_retry_reset(&discovery_rf_retry);
    app_node_comm_restart_role_scan();
    restart_ret = runtime_ops.start_uwb_scan();
    status_debug_printf(
        "DBG_SURVEY_DISCOVERY_SCHEDULE_REJECT survey=%u ret=%d scan_ret=%d\n",
        survey_id,
        ret,
        restart_ret);
    if (restart_ret < 0) {
        LOG_WRN("survey discovery scheduling rollback scan restart failed: survey=%u schedule_ret=%d scan_ret=%d",
                survey_id,
                ret,
                restart_ret);
    }
    return ret;
}

void app_anchor_survey_runtime_handle_pair_prepare(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_operation_policy_candidate policy_candidate;
    struct survey_pair pair = {0};
    struct survey_pair_control_id control_id;
    struct survey_pair_lease preflight_lease;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_pair_lease_decision decision;
    enum survey_pair_lease_decision preflight_decision;
    enum command_status status = COMMAND_OK;
    uint32_t lease_remaining_ms = 0u;
    uint32_t prepare_now_ms = 0u;
    uint16_t round_id = SURVEY_LEGACY_ROUND_ID;
    uint8_t reason = 0u;
    bool lease_invariant_ok = true;
    bool lease_rolled_back = false;
    bool generation_advanced = false;
    int schedule_ret = 0;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_PREPARE ||
        packet->flags != FLAG_DIAGNOSTIC ||
        packet->dst_id != DEVICE_ID ||
        packet->src_id != GATEWAY_ID) {
        return;
    }

    ret = app_operation_policy_prepare_payload(payload,
                                               payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_PAIR_MASK,
                                               &policy_candidate);
    if (ret < 0) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = EBADMSG;
    }
    if (ret == 0) {
        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(payload, payload_len, &round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_extract_tlv(
            payload, payload_len, round_commitment);
    }
    if (ret != PROTO_OK || pair.operation_generation == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        packet->session_id !=
            survey_operation_session_id(pair.operation_generation)) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
    } else if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        status = COMMAND_DENIED;
        reason = 2u;
    } else if (!pair_queueable(&pair)) {
        status = COMMAND_DENIED;
        reason = 4u;
    } else {
        control_id = (struct survey_pair_control_id) {
            .session_id = packet->session_id,
            .command_seq = packet->seq,
        };
        prepare_now_ms = k_uptime_get_32();
        ret = k_mutex_lock(&survey_generation_admission_mutex, K_FOREVER);
        if (ret < 0) {
            status = COMMAND_INTERNAL_ERROR;
            reason = (uint8_t)(-ret);
        } else {
            k_spinlock_key_t key = k_spin_lock(&survey_lock);

            preflight_lease = pair_lease;
            lease_invariant_ok = survey_pair_lease_invariant(
                &preflight_lease);
            k_spin_unlock(&survey_lock, key);
            preflight_decision = survey_pair_lease_prepare_round_bound(
                &preflight_lease,
                &pair,
                round_id,
                round_commitment,
                &control_id,
                prepare_now_ms,
                SURVEY_PAIR_PREPARED_LEASE_MS);
            if (!lease_invariant_ok) {
                status = COMMAND_INTERNAL_ERROR;
                reason = EINVAL;
                app_watchdog_stop_feeding();
            } else if (
                preflight_decision ==
                SURVEY_PAIR_LEASE_INVALID_ARGUMENT) {
                /*
                 * Validate the complete decoded lease transition before a
                 * newer generation can become durable or abort live RF work.
                 * The full-round digest is opaque at one endpoint, but its
                 * exact value remains bound across PREPARE and START.
                 */
                status = COMMAND_MALFORMED_PAYLOAD;
                reason = EINVAL;
            } else if ((ret = survey_generation_admit_locked(
                            pair.operation_generation,
                            &generation_advanced)) < 0) {
                status = ret == -ESTALE ? COMMAND_INVALID_STATE :
                         ret == -EBUSY ? COMMAND_BUSY :
                         COMMAND_INTERNAL_ERROR;
                reason = ret == -ESTALE ? 6u :
                         ret == -EBUSY ? 3u : (uint8_t)(-ret);
            } else {
                key = k_spin_lock(&survey_lock);
                decision = survey_pair_lease_prepare_round_bound(
                    &pair_lease,
                    &pair,
                    round_id,
                    round_commitment,
                    &control_id,
                    prepare_now_ms,
                    SURVEY_PAIR_PREPARED_LEASE_MS);
                if (decision == SURVEY_PAIR_LEASE_BUSY) {
                    status = COMMAND_BUSY;
                    reason = 3u;
                } else if (
                    decision == SURVEY_PAIR_LEASE_ACCEPTED ||
                    decision == SURVEY_PAIR_LEASE_DUPLICATE ||
                    decision == SURVEY_PAIR_LEASE_SUPERSEDED) {
                    lease_remaining_ms =
                        survey_pair_lease_remaining_ms(
                            &pair_lease, k_uptime_get_32());
                } else {
                    status = COMMAND_INVALID_STATE;
                    reason = decision == SURVEY_PAIR_LEASE_EXPIRED ?
                             5u : 4u;
                }
                k_spin_unlock(&survey_lock, key);
            }

            if (lease_remaining_ms > 0u) {
                schedule_ret = pair_cleanup_schedule_exact(
                    pair.operation_generation, lease_remaining_ms);
                if (schedule_ret < 0) {
                    key = k_spin_lock(&survey_lock);

                    if (pair_lease.phase ==
                            SURVEY_PAIR_LEASE_PREPARED &&
                        pair_lease.prepare_id_valid &&
                        pair_lease.prepare_id.session_id ==
                            control_id.session_id &&
                        pair_lease.prepare_id.command_seq ==
                            control_id.command_seq) {
                        lease_rolled_back =
                            survey_pair_lease_abort(&pair_lease);
                    }
                    k_spin_unlock(&survey_lock, key);
                    if (lease_rolled_back) {
                        pair_cleanup_cancel_exact(
                            pair.operation_generation);
                    }
                    status = COMMAND_INTERNAL_ERROR;
                    reason = (uint8_t)(-schedule_ret);
                    LOG_ERR("survey pair lease scheduling failed: survey=%u round=%u seq=%u ret=%d rollback=%u",
                            packet->session_id,
                            round_id,
                            packet->seq,
                            schedule_ret,
                            lease_rolled_back ? 1u : 0u);
                    if (!lease_rolled_back) {
                        app_watchdog_stop_feeding();
                    }
                }
            }
            k_mutex_unlock(&survey_generation_admission_mutex);
        }
    }

    if (status == COMMAND_OK) {
        app_operation_policy_commit_prepared(&policy_candidate);
    }

    ret = runtime_ops.send_command_result(packet,
                                          CMD_SURVEY_PREPARE_PAIR,
                                          status,
                                          reason,
                                          NULL,
                                          0u);
    status_debug_printf(
        "DBG_SURVEY_PAIR_PREPARE_RX survey=%u round=%u seq=%u initiator=0x%016llx "
        "responder=0x%016llx samples=%u status=%u reason=%u result_ret=%d\n",
        packet->session_id,
        round_id,
        packet->seq,
        (unsigned long long)pair.initiator_id,
        (unsigned long long)pair.responder_id,
        pair.sample_count,
        status,
        reason,
        ret);
    if (ret < 0) {
        LOG_WRN("survey pair prepare result TX failed: status=%u ret=%d",
                status,
                ret);
        return;
    }

    LOG_INF("survey pair prepare handled: survey=%u responder=0x%016llx samples=%u status=%u reason=%u",
            pair.survey_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            status,
            reason);
}

static bool survey_range_outcome_is_functional(
    int ret,
    enum range_status status)
{
    if (ret == 0 || ret == -ETIMEDOUT) {
        return true;
    }

    return status == RANGE_BAD_FRAME ||
           status == RANGE_WRONG_TARGET ||
           status == RANGE_STS_QUALITY_FAIL ||
           status == RANGE_TIMING_INVALID;
}

static int run_pair_initiator(const struct survey_pair *pair,
                              bool *functional_radio_outcome)
{
    const uint32_t operation_session_id =
        survey_operation_session_id(pair->operation_generation);
    int last_ret = 0;

    if (operation_session_id == 0u) {
        return -EINVAL;
    }

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count &&
         !app_anchor_survey_runtime_abort_requested();
         sample_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result result = {0};
        int ret = -ETIMEDOUT;

        request.initiator_id = pair->initiator_id;
        request.responder_id = pair->responder_id;
        request.network_id = NETWORK_ID;
        request.session_nonce = survey_sample_nonce(pair, sample_index);
        request.responder_short_addr =
            uwb_session_short_addr_from_id(pair->responder_id);
        request.session_id = operation_session_id;
        request.seq = survey_sample_seq(sample_index);
        request.flags = FLAG_DIAGNOSTIC;
        request.timeout_ms = SURVEY_PAIR_INITIATOR_TIMEOUT_MS;
        /*
         * The responder report carries the link RSL. Reading optional RX
         * diagnostics here would run between RESP reception and the delayed
         * FINAL, where the clicker path deliberately performs no extra SPI.
         */
        request.capture_rsl = false;
        result.status = RANGE_RX_TIMEOUT;

        LOG_INF("survey DS-TWR initiator sample start: survey=%u responder=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->responder_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                request.seq);
        ret = dwm3000_driver_range_initiator(&request, &result);
        if (functional_radio_outcome != NULL &&
            survey_range_outcome_is_functional(ret, result.status)) {
            *functional_radio_outcome = true;
        }
        if (result.initiator_id == 0u) {
            result.initiator_id = pair->initiator_id;
        }
        if (result.responder_id == 0u) {
            result.responder_id = pair->responder_id;
        }
        result.session_id = operation_session_id;
        result.seq = request.seq;
        result.flags = FLAG_DIAGNOSTIC;
        if (result.status == RANGE_OK && ret < 0) {
            result.status = RANGE_INTERNAL_ERROR;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR initiator sample complete: survey=%u responder=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            LOG_WRN("survey DS-TWR initiator sample failed: survey=%u responder=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        if (sample_index + 1u < pair->sample_count) {
            k_msleep(SURVEY_PAIR_SAMPLE_GAP_MS);
        }
    }

    return last_ret;
}

static int run_pair_responder(const struct survey_pair *pair,
                              uint16_t round_id,
                              const uint8_t round_commitment[
                                  SEMANTIC_DIGEST_SHA256_LEN],
                              struct app_node_comm_reservation_lease
                                  *delivery_reservation_leases,
                              size_t delivery_reservation_count,
                              bool *functional_radio_outcome)
{
    const uint32_t operation_session_id =
        survey_operation_session_id(pair->operation_generation);
    int last_ret = 0;

    if (operation_session_id == 0u || round_commitment == NULL ||
        delivery_reservation_leases == NULL ||
        delivery_reservation_count < pair->sample_count) {
        return -EINVAL;
    }

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count &&
         !app_anchor_survey_runtime_abort_requested();
         sample_index++) {
        struct dwm3000_range_request expected = {0};
        struct dwm3000_range_result result = {0};
        int64_t deadline_ms;
        int ret = -ETIMEDOUT;

        expected.initiator_id = pair->initiator_id;
        expected.responder_id = pair->responder_id;
        expected.network_id = NETWORK_ID;
        expected.session_nonce = survey_sample_nonce(pair, sample_index);
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = operation_session_id;
        expected.seq = survey_sample_seq(sample_index);
        expected.flags = FLAG_DIAGNOSTIC;
        expected.capture_rsl = sample_index == 0u;
        result.status = RANGE_RX_TIMEOUT;

        deadline_ms = k_uptime_get() + SURVEY_PAIR_RESPONDER_WINDOW_MS;
        LOG_INF("survey DS-TWR responder listen: survey=%u initiator=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->initiator_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                expected.seq);
        while (k_uptime_get() < deadline_ms &&
               !app_anchor_survey_runtime_abort_requested()) {
            uint32_t remaining_ms =
                (uint32_t)MAX(1, deadline_ms - k_uptime_get());

            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         remaining_ms,
                                                         &result);
            if (ret == -EAGAIN) {
                continue;
            }
            break;
        }
        if (functional_radio_outcome != NULL &&
            survey_range_outcome_is_functional(ret, result.status)) {
            *functional_radio_outcome = true;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR responder sample complete: survey=%u initiator=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            if (result.status == RANGE_OK || !range_status_valid(result.status)) {
                result.status = ret == -ETIMEDOUT ?
                                RANGE_RX_TIMEOUT : RANGE_INTERNAL_ERROR;
            }
            LOG_WRN("survey DS-TWR responder sample failed: survey=%u initiator=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        ret = runtime_ops.queue_sample_result(pair,
                                              round_id,
                                              sample_index,
                                              DEVICE_ID,
                                              &delivery_reservation_leases[
                                                  sample_index],
                                              round_commitment,
                                              &result);
        if (ret < 0) {
            LOG_ERR("survey responder sample result custody failed; stopping pair: survey=%u sample=%u ret=%d",
                    pair->survey_id, sample_index, ret);
            return ret;
        }
        memset(&delivery_reservation_leases[sample_index], 0,
               sizeof(delivery_reservation_leases[sample_index]));
    }

    return last_ret;
}

static bool pair_start_delivery_ready(void)
{
    struct survey_pair_control_id control_id = {0};
    uint32_t delivery_handle;
    uint32_t release_remaining_ms = 0u;
    uint64_t operation_generation = 0u;
    bool as_responder = false;
    bool start_released = false;
    bool ready = false;
    k_spinlock_key_t key;

    key = k_spin_lock(&survey_lock);
    if (!pair_start_pending) {
        k_spin_unlock(&survey_lock, key);
        return false;
    }
    if (survey_pair_lease_ready_snapshot(&pair_lease, NULL)) {
        operation_generation = pair_lease.pair.operation_generation;
        as_responder = pair_lease.pair.responder_id == DEVICE_ID;
        release_remaining_ms =
            survey_pair_lease_execution_remaining_for_role_ms(
                &pair_lease, k_uptime_get_32(), as_responder);
        k_spin_unlock(&survey_lock, key);
        if (release_remaining_ms != 0u) {
            status_debug_printf(
                "DBG_SURVEY_PAIR_RELEASE_WAIT remaining=%u\n",
                release_remaining_ms);
            (void)deadline_schedule(
                SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                operation_generation,
                release_remaining_ms,
                "pair-start-release");
            return false;
        }
        status_debug_printf("DBG_SURVEY_PAIR_RELEASE_DUE\n");
        return true;
    }
    delivery_handle = pair_start_delivery_handle;
    if (pair_lease.start_id_valid) {
        control_id = pair_lease.start_id;
    }
    if (delivery_handle != 0u &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == control_id.session_id &&
        pair_lease.start_id.command_seq == control_id.command_seq) {
        /*
         * Local acceptance and the shared execute timestamp are sufficient to
         * arm DS-TWR. The status packet keeps its own reliable custody, but a
         * delayed ACK_CONFIRM return path must not suppress an already accepted
         * radio action at either endpoint.
         */
        start_released = survey_pair_lease_release_start(&pair_lease,
                                                         &control_id);
        if (start_released) {
            pair_start_kick_clear_locked();
            operation_generation = pair_lease.pair.operation_generation;
            as_responder = pair_lease.pair.responder_id == DEVICE_ID;
            release_remaining_ms =
                survey_pair_lease_execution_remaining_for_role_ms(
                    &pair_lease, k_uptime_get_32(), as_responder);
            ready = release_remaining_ms == 0u &&
                    survey_pair_lease_ready_snapshot(&pair_lease, NULL);
        }
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);

    if (!start_released) {
        LOG_WRN("survey START local release was stale: session=%u seq=%u handle=%u",
                control_id.session_id,
                control_id.command_seq,
                delivery_handle);
        return false;
    }
    if (release_remaining_ms != 0u) {
        (void)deadline_schedule(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                                operation_generation,
                                release_remaining_ms,
                                "pair-start-release");
    }
    status_debug_printf(
        "DBG_SURVEY_PAIR_START_ARM session=%u seq=%u handle=%u status_owner=auto ready=%u remaining=%u\n",
        control_id.session_id,
        control_id.command_seq,
        delivery_handle,
        ready ? 1u : 0u,
        release_remaining_ms);
    return ready;
}

static void finish_discovery_without_radio(
    const struct survey_discovery_config *config,
    uint32_t start_ms,
    int run_ret,
    const char *reason)
{
    bool current;
    bool successor_pending;
    k_spinlock_key_t key;
    int report_ret;

    key = k_spin_lock(&survey_lock);
    current = discovery_config_matches_locked(config) &&
              !app_anchor_survey_runtime_abort_requested();
    if (!current) {
        survey_running = false;
        if (discovery_config_matches_locked(config)) {
            discovery_report_stage_pending = false;
            discovery_generation_active = false;
        }
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        survey_rf_retry_reset(&discovery_rf_retry);
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
            (void)runtime_ops.start_uwb_scan();
        }
        status_debug_printf(
            "DBG_SURVEY_DISCOVERY_NO_RADIO_RETIRED survey=%u run_ret=%d reason=%s successor=%u\n",
            config->survey_id,
            run_ret,
            reason == NULL ? "superseded" : reason,
            successor_pending ? 1u : 0u);
        return;
    }
    k_spin_unlock(&survey_lock, key);

    report_ret = app_anchor_survey_discovery_stage_empty_report(config,
                                                                 start_ms);
    status_debug_printf(
        "DBG_SURVEY_DISCOVERY_NO_RADIO survey=%u run_ret=%d report_ret=%d reason=%s\n",
        config->survey_id,
        run_ret,
        report_ret,
        reason == NULL ? "radio-deadline" : reason);
    runtime_ops.report_schedule(0u);
    survey_rf_retry_reset(&discovery_rf_retry);
    key = k_spin_lock(&survey_lock);
    survey_running = false;
    current = discovery_config_matches_locked(config);
    if (current) {
        discovery_report_stage_pending = report_ret < 0;
        discovery_generation_active = report_ret < 0;
    }
    successor_pending = discovery_pending;
    k_spin_unlock(&survey_lock, key);
    if (!successor_pending) {
        app_node_comm_restart_role_scan();
        (void)runtime_ops.start_uwb_scan();
    }
    if (current && report_ret < 0) {
        (void)schedule_discovery_if_current(
            config,
            SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
            SURVEY_NON_RF_SERVICE_POLL_MS,
            "discovery-report-stage");
    }
}

static void survey_work_handler(struct k_work *work)
{
    struct survey_anchor_deadline_events deadline_events;
    struct survey_pair pair;
    struct survey_pair_control_id pair_control_id = {0};
    struct survey_discovery_config pending_discovery = {0};
    uint32_t pending_discovery_start_ms = 0u;
    uint32_t pair_deadline_ms = 0u;
    uint32_t expired_pair_delivery_handle = 0u;
    uint16_t pair_round_id = SURVEY_LEGACY_ROUND_ID;
    struct app_node_comm_reservation_lease delivery_reservation_leases[
        SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT] = {0};
    bool as_responder;
    bool discovery_take_pending = false;
    bool functional_radio_outcome = false;
    bool report_retained = false;
    bool run_discovery = false;
    bool retry_report_stage = false;
    bool successor_pending = false;
    bool cancel_pair_start_kick = false;
    bool pair_lease_expired = false;
    bool pair_due = false;
    bool discovery_custody_due;
    bool pair_admission_due;
    bool result_due;
    int64_t uwb_window_start_ms;
    struct radio_guard_uwb_lease radio_lease = {0};
    k_spinlock_key_t key;
    int low_power_ret;
    int release_ret;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    deadline_events = deadline_take_due();
    if (deadline_events.due_mask == 0u) {
        return;
    }
    result_due = (deadline_events.due_mask &
                  (UINT8_C(1) <<
                   SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY)) != 0u;
    discovery_custody_due =
        (deadline_events.due_mask &
         (UINT8_C(1) <<
          SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY)) != 0u;
    pair_admission_due =
        (deadline_events.due_mask &
         (UINT8_C(1) << SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION)) != 0u;
    if (result_due || pair_admission_due) {
        ret = app_anchor_survey_result_delivery_service();
        if (ret < 0 && ret != -EAGAIN && ret != -ENOSPC &&
            ret != -ETIMEDOUT) {
            LOG_ERR("survey pair result delivery service failed: %d", ret);
        }
    }
    if (discovery_custody_due) {
        (void)app_anchor_survey_discovery_retry_report();
    }
    key = k_spin_lock(&survey_lock);
    if (discovery_report_stage_pending &&
        survey_anchor_deadline_event_matches(
            &deadline_events,
            SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
            discovery_config.operation_generation)) {
        pending_discovery = discovery_config;
        retry_report_stage = true;
    }
    if (discovery_pending &&
        survey_anchor_deadline_event_matches(
            &deadline_events,
            SURVEY_ANCHOR_DEADLINE_OPERATION,
            discovery_config.operation_generation)) {
        pending_discovery = discovery_config;
        pending_discovery_start_ms = discovery_start_ms;
        discovery_take_pending = true;
    }
    if (pair_lease.pair.operation_generation != 0u) {
        const uint64_t pair_generation =
            pair_lease.pair.operation_generation;

        pair_due = survey_anchor_deadline_event_matches(
                       &deadline_events,
                       SURVEY_ANCHOR_DEADLINE_OPERATION,
                       pair_generation) ||
                   survey_anchor_deadline_event_matches(
                       &deadline_events,
                       SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                       pair_generation) ||
                   survey_anchor_deadline_event_matches(
                       &deadline_events,
                       SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
                       pair_generation);
    }
    k_spin_unlock(&survey_lock, key);

    if (retry_report_stage) {
        (void)app_anchor_survey_discovery_retry_report();
        if (app_anchor_survey_discovery_report_staged(
                survey_operation_session_id(
                    pending_discovery.operation_generation))) {
            key = k_spin_lock(&survey_lock);
            if (discovery_report_stage_pending &&
                discovery_config.operation_generation ==
                    pending_discovery.operation_generation &&
                discovery_config.survey_id == pending_discovery.survey_id) {
                discovery_report_stage_pending = false;
                discovery_generation_active = false;
            }
            k_spin_unlock(&survey_lock, key);
            runtime_ops.report_schedule(0u);
        } else {
            key = k_spin_lock(&survey_lock);
            retry_report_stage =
                discovery_report_stage_pending &&
                discovery_config.operation_generation ==
                    pending_discovery.operation_generation &&
                discovery_config.survey_id == pending_discovery.survey_id;
            k_spin_unlock(&survey_lock, key);
            if (retry_report_stage) {
                (void)schedule_discovery_if_current(
                    &pending_discovery,
                    SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
                    SURVEY_NON_RF_SERVICE_POLL_MS,
                    "discovery-report-stage-retry");
            }
        }
        return;
    }

    if (discovery_take_pending) {
        ret = app_anchor_survey_discovery_report_custody_status(
            pending_discovery.operation_generation);
        if (ret == -EALREADY) {
            bool retired = false;

            key = k_spin_lock(&survey_lock);
            if (discovery_pending &&
                discovery_config.operation_generation ==
                    pending_discovery.operation_generation &&
                discovery_config.survey_id ==
                    pending_discovery.survey_id) {
                discovery_pending = false;
                discovery_report_stage_pending = false;
                discovery_generation_active = false;
                retired = true;
            }
            k_spin_unlock(&survey_lock, key);
            if (retired) {
                runtime_ops.report_schedule(0u);
                app_node_comm_restart_role_scan();
                (void)runtime_ops.start_uwb_scan();
                status_debug_printf(
                    "DBG_SURVEY_DISCOVERY_ALREADY_RETAINED survey=%u generation=%llu\n",
                    pending_discovery.survey_id,
                    (unsigned long long)
                        pending_discovery.operation_generation);
            }
            return;
        }
        if (ret < 0) {
            bool retired = false;

            key = k_spin_lock(&survey_lock);
            if (discovery_pending &&
                discovery_config.operation_generation ==
                    pending_discovery.operation_generation &&
                discovery_config.survey_id == pending_discovery.survey_id) {
                discovery_pending = false;
                discovery_report_stage_pending = false;
                discovery_generation_active = false;
                retired = true;
            }
            k_spin_unlock(&survey_lock, key);
            if (retired) {
                runtime_ops.report_schedule(0u);
                app_node_comm_restart_role_scan();
                (void)runtime_ops.start_uwb_scan();
                status_debug_printf(
                    "DBG_SURVEY_DISCOVERY_REPORT_CUSTODY_BLOCKED survey=%u generation=%llu ret=%d\n",
                    pending_discovery.survey_id,
                    (unsigned long long)
                        pending_discovery.operation_generation,
                    ret);
            }
            return;
        }
        key = k_spin_lock(&survey_lock);
        if (discovery_pending &&
            discovery_config.operation_generation ==
                pending_discovery.operation_generation &&
            discovery_config.survey_id == pending_discovery.survey_id) {
            discovery_pending = false;
            survey_running = true;
            atomic_set(&abort_requested, 0);
            run_discovery = true;
        }
        k_spin_unlock(&survey_lock, key);
        if (!run_discovery) {
            return;
        }
    }

    if (run_discovery) {
        uint32_t discovery_deadline_ms = survey_discovery_radio_deadline_ms(
            &pending_discovery, pending_discovery_start_ms);
        uint32_t retry_delay_ms = 0u;

        if (uptime_deadline_reached(k_uptime_get_32(),
                                    discovery_deadline_ms)) {
            finish_discovery_without_radio(&pending_discovery,
                                           pending_discovery_start_ms,
                                           -ETIMEDOUT,
                                           "radio-deadline");
            return;
        }
        app_node_comm_stop_role_scan();
        ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,
                                    "survey discovery",
                                    &radio_lease);
        if (ret < 0) {
            bool retry_current = false;
            int retry_ret;

            if (radio_guard_uwb_poisoned()) {
                LOG_ERR("survey discovery rearm suppressed after radio parking failure: %d",
                        radio_guard_uwb_poison_error());
                return;
            }
            successor_pending = false;
            retry_ret = survey_rf_retry_delay_ms(
                &discovery_rf_retry,
                pending_discovery.survey_id,
                survey_discovery_radio_opportunity(),
                discovery_deadline_ms,
                &retry_delay_ms);
            if (retry_ret < 0 ||
                app_anchor_survey_runtime_abort_requested()) {
                if (!app_anchor_survey_runtime_abort_requested()) {
                    finish_discovery_without_radio(
                        &pending_discovery,
                        pending_discovery_start_ms,
                        retry_ret,
                        "radio-deferral-terminal");
                } else {
                    survey_rf_retry_reset(&discovery_rf_retry);
                    key = k_spin_lock(&survey_lock);
                    survey_running = false;
                    if (discovery_config_matches_locked(
                            &pending_discovery)) {
                        discovery_report_stage_pending = false;
                        discovery_generation_active = false;
                    }
                    successor_pending = discovery_pending;
                    k_spin_unlock(&survey_lock, key);
                    if (!successor_pending) {
                        app_node_comm_restart_role_scan();
                        (void)runtime_ops.start_uwb_scan();
                    }
                }
                return;
            }
            key = k_spin_lock(&survey_lock);
            survey_running = false;
            if (discovery_config_matches_locked(&pending_discovery) &&
                !app_anchor_survey_runtime_abort_requested()) {
                discovery_pending = true;
                retry_current = true;
            }
            successor_pending = discovery_pending && !retry_current;
            k_spin_unlock(&survey_lock, key);
            if (!successor_pending) {
                app_node_comm_restart_role_scan();
            }
            if (retry_current) {
                (void)schedule_discovery_if_current(
                    &pending_discovery,
                    SURVEY_ANCHOR_DEADLINE_OPERATION,
                    retry_delay_ms,
                    "discovery-rf-retry");
            }
            return;
        }
        survey_rf_retry_reset(&discovery_rf_retry);

        runtime_ops.set_uwb_busy(true);
        uwb_window_start_ms = k_uptime_get();
        ret = app_anchor_survey_discovery_run(&pending_discovery,
                                              pending_discovery_start_ms,
                                              &functional_radio_outcome);
        report_retained = ret >= 0;
        if (ret < 0 &&
            !app_anchor_survey_runtime_abort_requested()) {
            int report_ret = app_anchor_survey_discovery_stage_empty_report(
                &pending_discovery, pending_discovery_start_ms);

            report_retained = report_ret == 0 ||
                app_anchor_survey_discovery_report_staged(
                    survey_operation_session_id(
                        pending_discovery.operation_generation));
            status_debug_printf(
                "DBG_SURVEY_DISCOVERY_FAILSAFE_REPORT survey=%u run_ret=%d report_ret=%d\n",
                pending_discovery.survey_id, ret, report_ret);
        }
        low_power_ret = runtime_ops.enter_low_power(
            app_radio_low_power_mode_for_connection(
                runtime_ops.connected_radio_active()),
            "survey-discovery-exit");
        release_ret = survey_radio_release(&radio_lease, low_power_ret);
        runtime_ops.note_uwb_awake_since(uwb_window_start_ms, 0u);
        if (release_ret < 0) {
            LOG_ERR("survey discovery radio release failed; retaining poisoned owner: %d",
                    release_ret);
            return;
        }
        if (ret >= 0 && low_power_ret < 0) {
            ret = low_power_ret;
        }
        runtime_ops.set_uwb_busy(false);
        if (functional_radio_outcome &&
            ret >= 0 &&
            low_power_ret >= 0) {
            app_watchdog_note_radio_progress();
        }
        survey_rf_retry_reset(&discovery_rf_retry);
        key = k_spin_lock(&survey_lock);
        survey_running = false;
        discovery_take_pending =
            discovery_config_matches_locked(&pending_discovery);
        if (discovery_take_pending) {
            discovery_report_stage_pending = ret < 0 && !report_retained;
            discovery_generation_active = discovery_report_stage_pending;
        }
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
            (void)runtime_ops.start_uwb_scan();
        }
        if (discovery_take_pending) {
            (void)app_anchor_survey_discovery_retry_report();
        }
        runtime_ops.report_schedule(0u);
        if (discovery_take_pending &&
            ret < 0 && !report_retained) {
            (void)schedule_discovery_if_current(
                &pending_discovery,
                SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
                SURVEY_NON_RF_SERVICE_POLL_MS,
                "discovery-failsafe-report-stage");
        }
        LOG_INF("survey discovery run finished: survey=%u ret=%d",
                pending_discovery.survey_id,
                ret);
        return;
    }

    if (!pair_due) {
        return;
    }

    if (!pair_start_delivery_ready()) {
        return;
    }

    key = k_spin_lock(&survey_lock);
    if (survey_pair_lease_expire(&pair_lease, k_uptime_get_32())) {
        pair_lease_expired = true;
        expired_pair_delivery_handle = pair_start_delivery_handle;
        cancel_pair_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    if (!pair_start_pending || !pair_lease.start_id_valid ||
        !survey_pair_lease_ready_snapshot(&pair_lease, &pair)) {
        if (pair_lease.phase != SURVEY_PAIR_LEASE_START_PENDING ||
            !pair_lease.start_id_valid) {
            pair_start_pending = false;
            pair_start_delivery_handle = 0u;
        }
        k_spin_unlock(&survey_lock, key);
        if (pair_lease_expired) {
            if (cancel_pair_start_kick) {
                (void)k_work_cancel_delayable(&pair_start_kick_work);
            }
            (void)app_anchor_survey_runtime_abandon_pair_start_delivery(
                expired_pair_delivery_handle,
                "pair-worker-lease-expired");
        }
        survey_rf_retry_reset(&pair_rf_retry);
        return;
    }
    pair_control_id = pair_lease.start_id;
    pair_deadline_ms = pair_lease.prepared_deadline_ms;
    /*
     * The private UWB worker serializes RF runs. Clear a predecessor's abort
     * latch only after this exact successor pair owns the worker; ingress-side
     * PREPARE/START must leave cancellation asserted while old RF is exiting.
     */
    atomic_set(&abort_requested, 0);
    k_spin_unlock(&survey_lock, key);

    if (app_anchor_survey_runtime_discovery_is_pending()) {
        status_debug_printf(
            "DBG_SURVEY_PAIR_RF_BLOCK stage=discovery-pending\n");
        return;
    }
    if (anchor_uwb_window_active() ||
        runtime_ops.relay_tx_active()) {
        status_debug_printf(
            "DBG_SURVEY_PAIR_RF_BLOCK stage=radio-owner click=%u relay=%u\n",
            anchor_uwb_window_active() ? 1u : 0u,
            runtime_ops.relay_tx_active() ? 1u : 0u);
        (void)schedule_pair_rf_retry(&pair_control_id,
                                     pair.operation_generation,
                                     pair_deadline_ms,
                                     "radio-owner-busy");
        return;
    }

    as_responder = pair.responder_id == DEVICE_ID;
    if (as_responder &&
        app_anchor_survey_result_delivery_occupied_count() > 0u) {
        status_debug_printf(
            "DBG_SURVEY_PAIR_RF_BLOCK stage=result-custody occupied=%u\n",
            (unsigned int)
                app_anchor_survey_result_delivery_occupied_count());
        (void)app_anchor_survey_result_delivery_service();
        (void)schedule_pair_unless_discovery_pending(
            SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
            pair.operation_generation,
            SURVEY_NON_RF_SERVICE_POLL_MS,
            "pair-result-custody");
        return;
    }

    if (as_responder) {
        ret = app_node_comm_reserve_durable_reliable_uplinks(
            pair.operation_generation,
            pair.sample_count,
            delivery_reservation_leases,
            ARRAY_SIZE(delivery_reservation_leases));
        if (ret < 0) {
            status_debug_printf(
                "DBG_SURVEY_PAIR_RF_BLOCK stage=delivery-reserve ret=%d count=%u\n",
                ret,
                pair.sample_count);
            (void)app_anchor_survey_result_delivery_service();
            (void)schedule_pair_unless_discovery_pending(
                SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
                pair.operation_generation,
                SURVEY_NON_RF_SERVICE_POLL_MS,
                "pair-result-admission");
            return;
        }
    }

    app_node_comm_stop_role_scan();
    ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,
                                "survey pair DS-TWR",
                                &radio_lease);
    if (ret < 0) {
        if (radio_guard_uwb_poisoned()) {
            LOG_ERR("survey pair rearm suppressed after radio parking failure: %d",
                    radio_guard_uwb_poison_error());
            (void)app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "radio-guard-poisoned");
            return;
        }
        status_debug_printf(
            "DBG_SURVEY_PAIR_RF_BLOCK stage=radio-guard ret=%d\n",
            ret);
        bool reschedule;
        bool successor_pending;
        int cancel_ret;

        key = k_spin_lock(&survey_lock);
        reschedule = pair_start_pending &&
                     !app_anchor_survey_runtime_abort_requested() &&
                     survey_pair_lease_ready_snapshot(&pair_lease, NULL);
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
        }
        cancel_ret =
            app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "radio-guard");
        if (reschedule && cancel_ret == 0) {
            (void)schedule_pair_rf_retry(&pair_control_id,
                                         pair.operation_generation,
                                         pair_deadline_ms,
                                         "radio-guard-busy");
        }
        return;
    }
    survey_rf_retry_reset(&pair_rf_retry);

    expired_pair_delivery_handle = 0u;
    cancel_pair_start_kick = false;
    pair_lease_expired = false;
    key = k_spin_lock(&survey_lock);
    if (survey_pair_lease_expire(&pair_lease, k_uptime_get_32())) {
        pair_lease_expired = true;
        expired_pair_delivery_handle = pair_start_delivery_handle;
        cancel_pair_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    if (!pair_start_pending ||
        !survey_pair_lease_mark_running_for_role_at(&pair_lease,
                                                    k_uptime_get_32(),
                                                    as_responder,
                                                    &pair,
                                                    &pair_round_id)) {
        if (pair_lease.phase != SURVEY_PAIR_LEASE_START_PENDING ||
            !pair_lease.start_id_valid) {
            pair_start_pending = false;
            pair_start_delivery_handle = 0u;
        }
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        if (pair_lease_expired) {
            if (cancel_pair_start_kick) {
                (void)k_work_cancel_delayable(&pair_start_kick_work);
            }
            (void)app_anchor_survey_runtime_abandon_pair_start_delivery(
                expired_pair_delivery_handle,
                "pair-worker-admission-lease-expired");
        }
        release_ret = survey_radio_release(&radio_lease, 0);
        if (release_ret < 0) {
            (void)app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "lease-race-radio-release");
            LOG_ERR("survey pair lease-race radio release failed: %d",
                    release_ret);
            return;
        }
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
        }
        (void)app_anchor_survey_result_delivery_cancel_reservations(
            delivery_reservation_leases,
            ARRAY_SIZE(delivery_reservation_leases),
            "lease-race");
        return;
    }
    if (!pair_lease.round_commitment_valid) {
        survey_running = false;
        (void)survey_pair_lease_finish(&pair_lease);
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        release_ret = survey_radio_release(&radio_lease, 0);
        if (release_ret < 0) {
            (void)app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "pair-commitment-missing-radio-release");
            LOG_ERR("survey pair commitment-missing radio release failed: %d",
                    release_ret);
            return;
        }
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
        }
        (void)app_anchor_survey_result_delivery_cancel_reservations(
            delivery_reservation_leases,
            ARRAY_SIZE(delivery_reservation_leases),
            "pair-commitment-missing");
        app_watchdog_stop_feeding();
        return;
    }
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    survey_running = true;
    if (as_responder != (pair.responder_id == DEVICE_ID)) {
        survey_running = false;
        (void)survey_pair_lease_finish(&pair_lease);
        successor_pending = discovery_pending;
        k_spin_unlock(&survey_lock, key);
        release_ret = survey_radio_release(&radio_lease, 0);
        if (release_ret < 0) {
            (void)app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "pair-role-race-radio-release");
            LOG_ERR("survey pair role-race radio release failed: %d",
                    release_ret);
            return;
        }
        if (!successor_pending) {
            app_node_comm_restart_role_scan();
        }
        (void)app_anchor_survey_result_delivery_cancel_reservations(
            delivery_reservation_leases,
            ARRAY_SIZE(delivery_reservation_leases),
            "pair-role-race");
        app_watchdog_stop_feeding();
        return;
    }
    k_spin_unlock(&survey_lock, key);
    status_debug_printf(
        "DBG_SURVEY_PAIR_RF_START survey=%u round=%u role=%s samples=%u\n",
        pair.survey_id,
        pair_round_id,
        as_responder ? "responder" : "initiator",
        pair.sample_count);
    pair_cleanup_cancel_exact(pair.operation_generation);
    deadline_cancel_pair(pair.operation_generation, "pair-rf-start");

    runtime_ops.set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    if (as_responder) {
        ret = run_pair_responder(&pair,
                                 pair_round_id,
                                 pair_lease.round_commitment,
                                 delivery_reservation_leases,
                                 ARRAY_SIZE(delivery_reservation_leases),
                                 &functional_radio_outcome);
    } else {
        ret = run_pair_initiator(&pair, &functional_radio_outcome);
    }
    {
        int cancel_ret =
            app_anchor_survey_result_delivery_cancel_reservations(
                delivery_reservation_leases,
                ARRAY_SIZE(delivery_reservation_leases),
                "pair-exit");

        if (ret >= 0 && cancel_ret < 0) {
            ret = cancel_ret;
        }
    }
    low_power_ret = runtime_ops.enter_low_power(
        app_radio_low_power_mode_for_connection(
            runtime_ops.connected_radio_active()),
        "survey-pair-exit");
    release_ret = survey_radio_release(&radio_lease, low_power_ret);
    runtime_ops.note_uwb_awake_since(uwb_window_start_ms, 0u);
    if (release_ret < 0) {
        LOG_ERR("survey pair radio release failed; retaining poisoned owner: %d",
                release_ret);
        return;
    }
    if (ret >= 0 && low_power_ret < 0) {
        ret = low_power_ret;
    }
    runtime_ops.set_uwb_busy(false);
    if (functional_radio_outcome && low_power_ret >= 0) {
        app_watchdog_note_radio_progress();
    }
    runtime_ops.report_schedule(0u);
    key = k_spin_lock(&survey_lock);
    survey_running = false;
    successor_pending = discovery_pending;
    k_spin_unlock(&survey_lock, key);
    app_anchor_survey_result_delivery_producer_finished(
        &pair,
        survey_operation_session_id(pair.operation_generation),
        pair_round_id,
        pair_lease.round_commitment);
    key = k_spin_lock(&survey_lock);
    (void)survey_pair_lease_finish(&pair_lease);
    k_spin_unlock(&survey_lock, key);
    if (!successor_pending) {
        app_node_comm_restart_role_scan();
    }

    LOG_INF("survey pair run finished: survey=%u role=%s ret=%d aborted=%u",
            pair.survey_id,
            as_responder ? "responder" : "initiator",
            ret,
            app_anchor_survey_runtime_abort_requested() ? 1u : 0u);
}

int app_anchor_survey_runtime_start_pair_from_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason)
{
    struct gateway_command_options command_options = {0};
    struct app_operation_policy_candidate policy_candidate;
    struct survey_pair pair = {0};
    struct survey_pair_control_id control_id;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    enum survey_pair_lease_decision decision;
    enum survey_pair_lease_phase phase_before;
    enum survey_pair_lease_phase phase_after;
    bool as_responder;
    bool cancel_start_kick = false;
    bool commitment_match;
    bool pair_match;
    bool transition_accepted;
    uint32_t superseded_delivery_handle = 0u;
    uint32_t active_commitment_tag = 0u;
    uint32_t received_commitment_tag = 0u;
    uint32_t prepared_deadline_before;
    uint32_t prepared_session_before;
    uint16_t prepared_seq_before;
    uint16_t round_before;
    uint32_t execution_deadline_ms;
    uint32_t execution_remaining_ms;
    uint32_t now_ms;
    uint16_t round_id = SURVEY_LEGACY_ROUND_ID;
    k_spinlock_key_t key;
    int ret;

    if (packet == NULL || payload == NULL || status == NULL || reason == NULL ||
        packet->msg_type != MSG_COMMAND || packet->src_id != GATEWAY_ID ||
        packet->dst_id != DEVICE_ID) {
        return -EINVAL;
    }

    ret = app_operation_policy_prepare_payload(payload,
                                               payload_len,
                                               0u,
                                               APP_OPERATION_POLICY_PAIR_MASK,
                                               &policy_candidate);
    if (ret == 0) {
        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(payload, payload_len, &round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_commitment_extract_tlv(
            payload, payload_len, round_commitment);
    }
    if (ret == PROTO_OK) {
        ret = gateway_command_extract_options(payload,
                                              payload_len,
                                              &command_options);
    }
    if (ret != PROTO_OK || pair.operation_generation == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        command_options.execute_delay_ms !=
            SURVEY_ROUND_START_EXECUTE_DELAY_MS ||
        packet->message_age_ms >= command_options.execute_delay_ms ||
        packet->session_id !=
            survey_operation_session_id(pair.operation_generation)) {
        *status = COMMAND_MALFORMED_PAYLOAD;
        *reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
        return -EINVAL;
    }
    execution_remaining_ms = command_options.execute_delay_ms -
                             packet->message_age_ms;
    if (execution_remaining_ms > (uint32_t)INT32_MAX) {
        *status = COMMAND_MALFORMED_PAYLOAD;
        *reason = 1u;
        return -EINVAL;
    }
    if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        *status = COMMAND_DENIED;
        *reason = 2u;
        return -EINVAL;
    }
    if (!pair_queueable(&pair)) {
        *status = COMMAND_DENIED;
        *reason = 4u;
        return -EINVAL;
    }
    if (anchor_uwb_window_active()) {
        *status = COMMAND_BUSY;
        *reason = 3u;
        return -EBUSY;
    }
    now_ms = k_uptime_get_32();
    execution_deadline_ms = now_ms + execution_remaining_ms;
    key = k_spin_lock(&survey_lock);
    control_id = (struct survey_pair_control_id) {
        .session_id = packet->session_id,
        .command_seq = packet->seq,
    };
    phase_before = pair_lease.phase;
    round_before = pair_lease.round_id;
    prepared_deadline_before = pair_lease.prepared_deadline_ms;
    prepared_session_before = pair_lease.prepare_id.session_id;
    prepared_seq_before = pair_lease.prepare_id.command_seq;
    pair_match = pair_lease.pair.operation_generation ==
                     pair.operation_generation &&
                 pair_lease.pair.survey_id == pair.survey_id &&
                 pair_lease.pair.initiator_id == pair.initiator_id &&
                 pair_lease.pair.responder_id == pair.responder_id &&
                 pair_lease.pair.sample_count == pair.sample_count;
    commitment_match = pair_lease.round_commitment_valid &&
        memcmp(pair_lease.round_commitment,
               round_commitment,
               sizeof(pair_lease.round_commitment)) == 0;
    memcpy(&active_commitment_tag,
           pair_lease.round_commitment,
           sizeof(active_commitment_tag));
    memcpy(&received_commitment_tag,
           round_commitment,
           sizeof(received_commitment_tag));
    decision = survey_pair_lease_start_round_bound_at(
        &pair_lease,
        &pair,
        round_id,
        round_commitment,
        &control_id,
        now_ms,
        execution_deadline_ms);
    transition_accepted =
        decision == SURVEY_PAIR_LEASE_ACCEPTED ||
        decision == SURVEY_PAIR_LEASE_DUPLICATE ||
        decision == SURVEY_PAIR_LEASE_SUPERSEDED;
    if (decision == SURVEY_PAIR_LEASE_ACCEPTED ||
        decision == SURVEY_PAIR_LEASE_SUPERSEDED) {
        /*
         * start_round() has already replaced the lease identity while this
         * lock is held. Detach the old result custody in the same critical
         * section so its terminal event cannot be mistaken for the new START.
         */
        superseded_delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = true;
        pair_start_delivery_handle = 0u;
    }
    phase_after = pair_lease.phase;
    k_spin_unlock(&survey_lock, key);
    status_debug_printf(
        "DBG_SURVEY_PAIR_START_DECISION session=%u seq=%u decision=%u phase=%u>%u round=%u/%u pair=%u commit=%u tag=%08x/%08x prep=%u:%u deadline=%u now=%u age=%u remaining=%u\n",
        packet->session_id,
        packet->seq,
        (unsigned int)decision,
        (unsigned int)phase_before,
        (unsigned int)phase_after,
        round_before,
        round_id,
        pair_match ? 1u : 0u,
        commitment_match ? 1u : 0u,
        active_commitment_tag,
        received_commitment_tag,
        prepared_session_before,
        prepared_seq_before,
        prepared_deadline_before,
        now_ms,
        packet->message_age_ms,
        execution_remaining_ms);
    if (decision == SURVEY_PAIR_LEASE_BUSY) {
        *status = COMMAND_BUSY;
        *reason = 3u;
        return -EBUSY;
    }
    if (!transition_accepted) {
        *status = COMMAND_INVALID_STATE;
        *reason = decision == SURVEY_PAIR_LEASE_EXPIRED ? 5u : 4u;
        return -EINVAL;
    }

    as_responder = pair.responder_id == DEVICE_ID;
    if (cancel_start_kick) {
        (void)k_work_cancel_delayable(&pair_start_kick_work);
    }
    ret = app_anchor_survey_runtime_abandon_pair_start_delivery(
        superseded_delivery_handle,
        decision == SURVEY_PAIR_LEASE_SUPERSEDED ?
            "superseded-start" : "accepted-new-start");
    if (ret < 0) {
        key = k_spin_lock(&survey_lock);
        if (pair_start_pending &&
            pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
            pair_lease.start_id_valid &&
            pair_lease.start_id.session_id == control_id.session_id &&
            pair_lease.start_id.command_seq == control_id.command_seq) {
            (void)survey_pair_lease_abort(&pair_lease);
            pair_start_pending = false;
            pair_start_delivery_handle = 0u;
        }
        k_spin_unlock(&survey_lock, key);
        *status = COMMAND_INTERNAL_ERROR;
        *reason = (uint8_t)(-ret);
        return ret;
    }
    app_operation_policy_commit_prepared(&policy_candidate);
    *status = COMMAND_OK;
    *reason = 0u;
    LOG_INF("survey pair start %s: survey=%u round=%u initiator=0x%016llx responder=0x%016llx samples=%u local_role=%s",
            decision == SURVEY_PAIR_LEASE_ACCEPTED ? "accepted" :
            decision == SURVEY_PAIR_LEASE_SUPERSEDED ? "superseded" :
                                                       "duplicate",
            pair.survey_id,
            round_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            as_responder ? "responder" : "initiator");
    return 0;
}

int app_anchor_survey_runtime_bind_pair_start_delivery(
    const struct proto_packet *command,
    uint32_t delivery_handle)
{
    struct survey_pair_control_id control_id = {0};
    bool bound = false;
    bool fallback_bound = false;
    bool rolled_back = false;
    uint64_t operation_generation = 0u;
    k_spinlock_key_t key;
    int fallback_ret;
    int ret;

    if (command == NULL || command->session_id == 0u ||
        command->seq == 0u || delivery_handle == 0u) {
        return -EINVAL;
    }

    key = k_spin_lock(&survey_lock);
    if (pair_start_pending &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == command->session_id &&
        pair_lease.start_id.command_seq == command->seq) {
        if (pair_start_delivery_handle == 0u ||
            pair_start_delivery_handle == delivery_handle) {
            pair_start_delivery_handle = delivery_handle;
            operation_generation = pair_lease.pair.operation_generation;
            bound = true;
        }
    }
    k_spin_unlock(&survey_lock, key);

    if (!bound) {
        return -ESTALE;
    }
    ret = deadline_schedule_raw(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                                operation_generation,
                                0u);
    if (ret >= 0) {
        return 0;
    }

    /*
     * Keep the system workqueue owner tiny: it republishes the exact
     * generation onto the private UWB queue and never executes radio work.
     * This closes the gap where a COMMAND_OK delivery was live while its only
     * private-queue wake edge had been rejected.
     */
    control_id = (struct survey_pair_control_id) {
        .session_id = command->session_id,
        .command_seq = command->seq,
    };
    key = k_spin_lock(&survey_lock);
    if (pair_start_pending &&
        pair_start_delivery_handle == delivery_handle &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == control_id.session_id &&
        pair_lease.start_id.command_seq == control_id.command_seq) {
        pair_start_kick_id = control_id;
        pair_start_kick_delivery_handle = delivery_handle;
        pair_start_kick_active = true;
        fallback_bound = true;
    }
    k_spin_unlock(&survey_lock, key);
    if (!fallback_bound) {
        return -ESTALE;
    }

    fallback_ret = k_work_reschedule(&pair_start_kick_work, K_NO_WAIT);
    if (fallback_ret >= 0) {
        LOG_WRN("survey pair START bind using bounded system-workqueue recovery: session=%u seq=%u handle=%u private_ret=%d",
                control_id.session_id,
                control_id.command_seq,
                delivery_handle,
                ret);
        return 0;
    }

    key = k_spin_lock(&survey_lock);
    if (pair_start_kick_matches_locked(&control_id, delivery_handle)) {
        rolled_back = survey_pair_lease_abort(&pair_lease);
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
        pair_start_kick_clear_locked();
    }
    k_spin_unlock(&survey_lock, key);
    if (!rolled_back) {
        app_watchdog_stop_feeding();
    }
    LOG_ERR("survey pair START bind has no live work owner: session=%u seq=%u handle=%u private_ret=%d fallback_ret=%d rollback=%u",
            control_id.session_id,
            control_id.command_seq,
            delivery_handle,
            ret,
            fallback_ret,
            rolled_back ? 1u : 0u);
    return fallback_ret;
}

bool app_anchor_survey_runtime_cancel_pair_start(
    const struct proto_packet *command)
{
    bool cancelled = false;
    bool cancel_start_kick = false;
    uint32_t delivery_handle = 0u;
    uint64_t operation_generation = 0u;
    k_spinlock_key_t key;

    if (command == NULL) {
        return false;
    }
    key = k_spin_lock(&survey_lock);
    if (pair_start_pending &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == command->session_id &&
        pair_lease.start_id.command_seq == command->seq) {
        operation_generation = pair_lease.pair.operation_generation;
        cancelled = survey_pair_lease_abort(&pair_lease);
        delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (cancelled) {
        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        pair_cleanup_cancel_exact(operation_generation);
        deadline_cancel_pair(operation_generation, "cancel-pair-start");
        app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "cancel-pair-start");
    }
    return cancelled;
}

static void pair_lease_work_handler(struct k_work *work)
{
    struct survey_pair expired_pair = {0};
    enum survey_pair_lease_phase expired_phase = SURVEY_PAIR_LEASE_IDLE;
    uint32_t delivery_handle = 0u;
    uint32_t remaining_ms = 0u;
    uint64_t operation_generation = 0u;
    bool expired = false;
    bool cancel_start_kick = false;
    k_spinlock_key_t key;

    ARG_UNUSED(work);

    key = k_spin_lock(&survey_lock);
    operation_generation = pair_cleanup_generation;
    if (pair_lease.pair.operation_generation == operation_generation &&
        (pair_lease.phase == SURVEY_PAIR_LEASE_PREPARED ||
         pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING)) {
        expired_pair = pair_lease.pair;
        expired_phase = pair_lease.phase;
        expired = survey_pair_lease_expire(&pair_lease,
                                           k_uptime_get_32());
        if (expired) {
            pair_cleanup_generation = 0u;
            delivery_handle = pair_start_delivery_handle;
            cancel_start_kick = pair_start_kick_active;
            pair_start_kick_clear_locked();
            pair_start_pending = false;
            pair_start_delivery_handle = 0u;
        } else {
            remaining_ms = survey_pair_lease_remaining_ms(
                &pair_lease, k_uptime_get_32());
        }
    } else if (pair_cleanup_generation == operation_generation) {
        pair_cleanup_generation = 0u;
    }
    k_spin_unlock(&survey_lock, key);

    if (!expired && remaining_ms != 0u) {
        (void)pair_cleanup_schedule_exact(operation_generation,
                                          remaining_ms);
        return;
    }
    if (expired) {
        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_OPERATION,
                        operation_generation,
                        "pair-lease-expired-operation");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
                        operation_generation,
                        "pair-lease-expired-phase");
        deadline_cancel(SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
                        operation_generation,
                        "pair-lease-expired-custody");
        app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "pair-lease-expired");
        LOG_WRN("survey pair lease expired: phase=%u survey=%u initiator=0x%016llx responder=0x%016llx",
                (unsigned int)expired_phase,
                expired_pair.survey_id,
                (unsigned long long)expired_pair.initiator_id,
                (unsigned long long)expired_pair.responder_id);
    }
}

void app_anchor_survey_runtime_abort_pair(void)
{
    bool pair_active;
    bool cancel_start_kick;
    uint32_t delivery_handle;
    uint64_t operation_generation = 0u;
    k_spinlock_key_t key;

    key = k_spin_lock(&survey_lock);
    pair_active = pair_lease.phase != SURVEY_PAIR_LEASE_IDLE;
    if (pair_active) {
        operation_generation = pair_lease.pair.operation_generation;
        atomic_set(&abort_requested, 1);
        (void)survey_pair_lease_abort(&pair_lease);
    }
    delivery_handle = pair_start_delivery_handle;
    cancel_start_kick = pair_start_kick_active;
    pair_start_kick_clear_locked();
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    k_spin_unlock(&survey_lock, key);
    if (cancel_start_kick) {
        (void)k_work_cancel_delayable(&pair_start_kick_work);
    }
    pair_cleanup_cancel_exact(operation_generation);
    deadline_cancel_pair(operation_generation, "abort-pair");
    app_anchor_survey_runtime_abandon_pair_start_delivery(
        delivery_handle, "abort-pair");
    LOG_INF("survey pair state abort requested: active=%u",
            pair_active ? 1u : 0u);
}

bool app_anchor_survey_runtime_abort_pair_matching(
    const struct survey_pair *pair,
    uint32_t session_id)
{
    bool matched;
    bool cancel_start_kick = false;
    uint32_t delivery_handle = 0u;
    k_spinlock_key_t key;

    if (pair == NULL) {
        return false;
    }
    key = k_spin_lock(&survey_lock);
    matched = survey_pair_lease_abort_matching(&pair_lease,
                                               pair,
                                               session_id);
    if (matched) {
        atomic_set(&abort_requested, 1);
        delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (matched) {
        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        pair_cleanup_cancel_exact(pair->operation_generation);
        deadline_cancel_pair(pair->operation_generation,
                             "targeted-abort");
        app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "targeted-abort");
    }
    LOG_INF("survey pair targeted abort: matched=%u survey=%u initiator=0x%016llx responder=0x%016llx",
            matched ? 1u : 0u,
            pair->survey_id,
            (unsigned long long)pair->initiator_id,
            (unsigned long long)pair->responder_id);
    return matched;
}

int app_anchor_survey_runtime_abort_pair_matching_round(
    const struct survey_pair *pair,
    uint32_t session_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    bool matched;
    bool producer_active;
    bool cancel_start_kick = false;
    uint32_t delivery_handle = 0u;
    size_t retired_result_count = 0u;
    k_spinlock_key_t key;
    int ret;

    if (pair == NULL || pair->operation_generation == 0u ||
        round_commitment == NULL) {
        return -EINVAL;
    }
    key = k_spin_lock(&survey_lock);
    producer_active = survey_running;
    matched = survey_pair_lease_abort_matching_round_bound(
        &pair_lease,
        pair,
        session_id,
        round_id,
        round_commitment);
    if (matched) {
        atomic_set(&abort_requested, 1);
        delivery_handle = pair_start_delivery_handle;
        cancel_start_kick = pair_start_kick_active;
        pair_start_kick_clear_locked();
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    producer_active = producer_active && matched;
    if (matched) {
        if (cancel_start_kick) {
            (void)k_work_cancel_delayable(&pair_start_kick_work);
        }
        pair_cleanup_cancel_exact(pair->operation_generation);
        deadline_cancel_pair(pair->operation_generation,
                             "round-targeted-abort");
        app_anchor_survey_runtime_abandon_pair_start_delivery(
            delivery_handle, "round-targeted-abort");
    }
    ret = app_anchor_survey_result_delivery_abort_round(
        pair,
        session_id,
        round_id,
        round_commitment,
        producer_active,
        &retired_result_count);
    LOG_INF("survey pair round-targeted abort: matched=%u retired=%u survey=%u round=%u initiator=0x%016llx responder=0x%016llx ret=%d",
            matched ? 1u : 0u,
            (unsigned int)retired_result_count,
            pair->survey_id,
            round_id,
            (unsigned long long)pair->initiator_id,
            (unsigned long long)pair->responder_id,
            ret);
    return ret;
}

int app_anchor_survey_runtime_init(
    const struct app_anchor_survey_runtime_ops *ops)
{
    const struct app_anchor_survey_result_delivery_ops result_ops = {
        .schedule_work_ms = schedule_result_delivery_ms,
        .active_owner_matches_outbound =
            ops->active_owner_matches_outbound,
        .wake_active_outbox = ops->wake_active_outbox,
    };
    int ret;

    if (ops == NULL || ops->send_command_result == NULL ||
        ops->enter_low_power == NULL || ops->set_uwb_busy == NULL ||
        ops->note_uwb_awake_since == NULL || ops->start_uwb_scan == NULL ||
        ops->queue_sample_result == NULL ||
        ops->report_queue_used == NULL || ops->report_schedule == NULL ||
        ops->relay_tx_active == NULL ||
        ops->connected_radio_active == NULL ||
        ops->active_owner_matches_outbound == NULL ||
        ops->wake_active_outbox == NULL) {
        return -EINVAL;
    }
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    if (ops->work_queue == NULL) {
        return -EINVAL;
    }
#endif

    runtime_ops = *ops;
    runtime_started = false;
    runtime_work_queue_ready = false;
    ret = app_anchor_survey_result_delivery_init(&result_ops);
    if (ret < 0) {
        return ret;
    }
    runtime_initialized = true;
    return 0;
}

int app_anchor_survey_runtime_start(void)
{
    uint64_t restored_generation = 0u;
    int ret;

    if (!runtime_initialized) {
        return -EINVAL;
    }
    survey_generation_high_watermark = 0u;
    survey_generation_active = 0u;
    survey_generation_restored = false;
#if DEVICE_ROLE == ROLE_ANCHOR
    ret = app_durable_state_restore_high_water(
        APP_DURABLE_STATE_SURVEY_GENERATION,
        GATEWAY_ID,
        &restored_generation);
    if (ret < 0) {
        return ret;
    }
#else
    ARG_UNUSED(ret);
#endif
    survey_generation_high_watermark = restored_generation;
    /*
     * The checkpoint is not an active operation by itself. An exact gateway
     * redrive may reconstruct it idempotently; rollback below it remains
     * stale, and a newer generation must advance flash before publication.
     */
    survey_generation_restored = true;

    survey_anchor_deadline_registry_init(&survey_deadlines);
    k_work_init_delayable(&survey_work, survey_work_handler);
    survey_pair_lease_reset(&pair_lease);
    pair_cleanup_generation = 0u;
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    pair_start_kick_active = false;
    pair_start_kick_delivery_handle = 0u;
    pair_start_kick_id = (struct survey_pair_control_id) {0};
    discovery_pending = false;
    discovery_generation_active = false;
    discovery_report_stage_pending = false;
    survey_rf_retry_reset(&discovery_rf_retry);
    survey_rf_retry_reset(&pair_rf_retry);
    k_work_init_delayable(&pair_lease_work, pair_lease_work_handler);
    k_work_init_delayable(&pair_start_kick_work,
                          pair_start_kick_work_handler);
    runtime_started = true;
    return 0;
}

int app_anchor_survey_runtime_post_work_queue_start(void)
{
    if (!runtime_initialized || !runtime_started) {
        return -EACCES;
    }
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    if (runtime_ops.work_queue == NULL) {
        return -ENODEV;
    }
    /*
     * app_anchor_start_anchor_role() calls this only after
     * k_work_queue_start() has returned. Publish the queue-ready boundary
     * before the radio can produce any volatile result record.
     */
    runtime_work_queue_ready = true;
    return 0;
#else
    return 0;
#endif
}
