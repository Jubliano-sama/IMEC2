#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Match the production route/click ordering from app_config.h without
 * importing its board-only configuration into native_sim. */
#define HARNESS_ROUTE_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(0)
#define HARNESS_CLICK_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(1)
#define HARNESS_WORKQUEUE_STACK_SIZE 4096u
#define HARNESS_PHYSICAL_HANDOFF_WINDOW_MS 250u

BUILD_ASSERT(HARNESS_ROUTE_WORKQUEUE_PRIORITY <
                 HARNESS_CLICK_WORKQUEUE_PRIORITY,
             "the route owner must preempt the click owner");
BUILD_ASSERT(HARNESS_PHYSICAL_HANDOFF_WINDOW_MS >
                 UWB_DISCOVERY_RX_GUARD_MS,
             "the harness needs a nonempty production bridge window");

enum harness_request_state {
    HARNESS_REQUEST_IDLE = 0,
    HARNESS_REQUEST_PREPARING,
    HARNESS_REQUEST_QUEUED,
    HARNESS_REQUEST_RUNNING,
    HARNESS_REQUEST_COMPLETE,
};

static struct k_work_q route_work_q;
static struct k_work_q click_work_q;
K_THREAD_STACK_DEFINE(route_work_q_stack, HARNESS_WORKQUEUE_STACK_SIZE);
K_THREAD_STACK_DEFINE(click_work_q_stack, HARNESS_WORKQUEUE_STACK_SIZE);

static struct k_work timing_negotiation_work;
static struct k_work click_request_work;
static struct k_work click_preempt_work;
static struct k_spinlock request_lock;
static enum harness_request_state request_state;
static uint32_t physical_deadline_ms;
static uint32_t bridge_deadline_ms;
static uint32_t boundary_observed_ms;
static uint32_t preempt_admitted_ms;
static uint32_t request_generation;
static int click_result;
static bool negotiation_cancelled;
static bool route_wake_token;
static bool watchdog_fail_stop;
static bool force_post_apply_deadline;
static uint8_t route_owned_apply_calls;

K_SEM_DEFINE(negotiation_entered, 0, 1);
K_SEM_DEFINE(request_published, 0, 1);
K_SEM_DEFINE(preempt_completed, 0, 1);
K_SEM_DEFINE(click_completed, 0, 1);

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/* Model the production route-owned transaction, including its second
 * deadline check after the nonblocking custody callbacks have returned. */
static int run_route_owned(uint32_t deadline_ms)
{
    if (deadline_reached(k_uptime_get_32(), deadline_ms)) {
        return -ETIMEDOUT;
    }

    route_owned_apply_calls++;
    if (force_post_apply_deadline ||
        deadline_reached(k_uptime_get_32(), deadline_ms)) {
        watchdog_fail_stop = true;
        return -ETIMEDOUT;
    }
    return 0;
}

static uint32_t publish_request(void)
{
    k_spinlock_key_t key = k_spin_lock(&request_lock);

    zassert_equal(request_state, HARNESS_REQUEST_IDLE);
    request_generation++;
    if (request_generation == 0u) {
        request_generation = 1u;
    }
    request_state = HARNESS_REQUEST_PREPARING;
    k_spin_unlock(&request_lock, key);

    route_wake_token = false;
    key = k_spin_lock(&request_lock);
    request_state = HARNESS_REQUEST_QUEUED;
    k_spin_unlock(&request_lock, key);
    route_wake_token = true;
    return request_generation;
}

/* The fallback submission is speculative. A rejected kick can either roll
 * back the still-queued request or lose the race to its in-band route owner;
 * neither case is an accepted-state ownership loss. */
static int classify_route_kick_result(uint32_t generation, int submit_ret)
{
    k_spinlock_key_t key;

    if (submit_ret >= 0) {
        return 0;
    }

    key = k_spin_lock(&request_lock);
    if (request_generation == generation &&
        request_state == HARNESS_REQUEST_QUEUED) {
        request_state = HARNESS_REQUEST_PREPARING;
        k_spin_unlock(&request_lock, key);
        route_wake_token = false;
        key = k_spin_lock(&request_lock);
        if (request_generation == generation &&
            request_state == HARNESS_REQUEST_PREPARING) {
            request_state = HARNESS_REQUEST_IDLE;
        }
        k_spin_unlock(&request_lock, key);
        return submit_ret;
    }
    if (request_generation != generation ||
        (request_state != HARNESS_REQUEST_RUNNING &&
         request_state != HARNESS_REQUEST_COMPLETE)) {
        k_spin_unlock(&request_lock, key);
        return -ESTALE;
    }
    k_spin_unlock(&request_lock, key);
    return 0;
}

/* Model the production WAKE_CLAIM frame boundary: it observes only a live
 * queued request. It does not execute click custody while the timing owner
 * still holds its radio lease and scratch mutex. */
static bool timing_negotiation_click_cancel_boundary(void)
{
    k_spinlock_key_t key = k_spin_lock(&request_lock);
    bool requested = request_state == HARNESS_REQUEST_QUEUED &&
                     !deadline_reached(k_uptime_get_32(), bridge_deadline_ms);

    if (requested) {
        boundary_observed_ms = k_uptime_get_32();
    }
    k_spin_unlock(&request_lock, key);
    return requested;
}

static bool service_queued_route_owned(void)
{
    k_spinlock_key_t key = k_spin_lock(&request_lock);

    if (request_state != HARNESS_REQUEST_QUEUED) {
        k_spin_unlock(&request_lock, key);
        return false;
    }
    if (deadline_reached(k_uptime_get_32(), bridge_deadline_ms)) {
        request_state = HARNESS_REQUEST_COMPLETE;
        click_result = -ETIMEDOUT;
    } else {
        request_state = HARNESS_REQUEST_RUNNING;
        preempt_admitted_ms = k_uptime_get_32();
        click_result = run_route_owned(bridge_deadline_ms);
        request_state = HARNESS_REQUEST_COMPLETE;
    }
    k_spin_unlock(&request_lock, key);
    route_wake_token = false;
    k_sem_give(&preempt_completed);
    return true;
}

static void timing_negotiation_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    k_sem_give(&negotiation_entered);

    /* A synchronous timing negotiation owns the sole route worker. Without
     * this in-band boundary, the preemption work queued behind it cannot run
     * until the click's bridge deadline has already passed. */
    for (;;) {
        if (timing_negotiation_click_cancel_boundary()) {
            negotiation_cancelled = true;
            /* Production reaches the same service seam only after the
             * synchronous sender has unwound its radio/scratch ownership. */
            zassert_true(service_queued_route_owned());
            return;
        }
        if (physical_deadline_ms != 0u &&
            deadline_reached(k_uptime_get_32(), physical_deadline_ms)) {
            return;
        }
        k_msleep(1u);
    }
}

static void click_preempt_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)service_queued_route_owned();
}

static void click_request_handler(struct k_work *work)
{
    uint32_t now_ms;
    uint32_t wait_ms;
    int ret;

    ARG_UNUSED(work);
    now_ms = k_uptime_get_32();
    physical_deadline_ms = now_ms + HARNESS_PHYSICAL_HANDOFF_WINDOW_MS;
    bridge_deadline_ms = physical_deadline_ms - UWB_DISCOVERY_RX_GUARD_MS;

    (void)publish_request();
    k_sem_give(&request_published);

    ret = k_work_submit_to_queue(&route_work_q, &click_preempt_work);
    zassert_equal(ret, 1, "click preemption was not queued behind negotiation");

    now_ms = k_uptime_get_32();
    wait_ms = deadline_reached(now_ms, bridge_deadline_ms) ?
                  0u : bridge_deadline_ms - now_ms;
    if (k_sem_take(&preempt_completed, K_MSEC(wait_ms)) != 0) {
        click_result = -ETIMEDOUT;
    }
    k_sem_give(&click_completed);
}

static void fixture_reset(void)
{
    request_state = HARNESS_REQUEST_IDLE;
    physical_deadline_ms = 0u;
    bridge_deadline_ms = 0u;
    boundary_observed_ms = 0u;
    preempt_admitted_ms = 0u;
    request_generation = 0u;
    click_result = -EINPROGRESS;
    negotiation_cancelled = false;
    route_wake_token = false;
    watchdog_fail_stop = false;
    force_post_apply_deadline = false;
    route_owned_apply_calls = 0u;
    k_sem_reset(&negotiation_entered);
    k_sem_reset(&request_published);
    k_sem_reset(&preempt_completed);
    k_sem_reset(&click_completed);
    k_work_init(&timing_negotiation_work, timing_negotiation_handler);
    k_work_init(&click_request_work, click_request_handler);
    k_work_init(&click_preempt_work, click_preempt_handler);
}

ZTEST(production_seam_click_preempt_priority,
      test_in_band_boundary_releases_fifo_route_owner_before_bridge_deadline)
{
    fixture_reset();

    k_work_queue_start(&route_work_q,
                       route_work_q_stack,
                       K_THREAD_STACK_SIZEOF(route_work_q_stack),
                       HARNESS_ROUTE_WORKQUEUE_PRIORITY,
                       NULL);
    k_work_queue_start(&click_work_q,
                       click_work_q_stack,
                       K_THREAD_STACK_SIZEOF(click_work_q_stack),
                       HARNESS_CLICK_WORKQUEUE_PRIORITY,
                       NULL);

    zassert_equal(k_work_submit_to_queue(&route_work_q,
                                         &timing_negotiation_work),
                  1);
    zassert_ok(k_sem_take(&negotiation_entered, K_SECONDS(2)),
               "timing negotiation did not occupy the route queue");
    zassert_equal(k_work_submit_to_queue(&click_work_q,
                                         &click_request_work),
                  1);
    zassert_ok(k_sem_take(&request_published, K_SECONDS(2)),
               "click owner did not publish its valid preemption request");
    zassert_ok(k_sem_take(&click_completed, K_SECONDS(2)),
               "click owner did not observe preemption completion");

    zassert_true(negotiation_cancelled,
                 "timing negotiation ignored the in-band click boundary");
    zassert_equal(click_result, 0,
                  "click preemption missed its existing bridge deadline");
    zassert_not_equal(boundary_observed_ms, 0u);
    zassert_not_equal(preempt_admitted_ms, 0u);
    zassert_true(!deadline_reached(boundary_observed_ms,
                                   bridge_deadline_ms),
                 "timing owner noticed the click after its bridge deadline");
    zassert_true(!deadline_reached(preempt_admitted_ms,
                                   bridge_deadline_ms),
                 "route owner admitted the click after its bridge deadline");
    zassert_equal(route_owned_apply_calls, 1u);
    zassert_false(watchdog_fail_stop);
}

ZTEST(production_seam_click_preempt_priority,
      test_rejected_speculative_kick_rolls_back_queued_generation)
{
    uint32_t generation;

    fixture_reset();
    generation = publish_request();

    zassert_equal(classify_route_kick_result(generation, -EBUSY), -EBUSY);
    zassert_equal(request_generation, generation);
    zassert_equal(request_state, HARNESS_REQUEST_IDLE);
    zassert_false(route_wake_token);
    zassert_false(watchdog_fail_stop);
    zassert_equal(route_owned_apply_calls, 0u);
}

ZTEST(production_seam_click_preempt_priority,
      test_redundant_rejected_kick_preserves_in_band_owner)
{
    uint32_t generation;
    k_spinlock_key_t key;

    fixture_reset();
    generation = publish_request();

    key = k_spin_lock(&request_lock);
    request_state = HARNESS_REQUEST_RUNNING;
    k_spin_unlock(&request_lock, key);
    zassert_ok(classify_route_kick_result(generation, -EBUSY));
    zassert_equal(request_state, HARNESS_REQUEST_RUNNING);
    zassert_false(watchdog_fail_stop);

    key = k_spin_lock(&request_lock);
    request_state = HARNESS_REQUEST_COMPLETE;
    k_spin_unlock(&request_lock, key);
    zassert_ok(classify_route_kick_result(generation, -ENODEV));
    zassert_equal(request_state, HARNESS_REQUEST_COMPLETE);
    zassert_false(watchdog_fail_stop);
}

ZTEST(production_seam_click_preempt_priority,
      test_direct_route_owner_post_apply_overrun_fails_closed)
{
    int ret;

    fixture_reset();
    force_post_apply_deadline = true;

    ret = run_route_owned(k_uptime_get_32() +
                          HARNESS_PHYSICAL_HANDOFF_WINDOW_MS);

    zassert_equal(ret, -ETIMEDOUT);
    zassert_equal(route_owned_apply_calls, 1u);
    zassert_true(watchdog_fail_stop);
}

ZTEST_SUITE(production_seam_click_preempt_priority,
            NULL, NULL, NULL, NULL, NULL);
