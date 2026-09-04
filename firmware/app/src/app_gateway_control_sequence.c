#include "app_gateway_control_sequence.h"

#include <zephyr/kernel.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_IMEC_DURABLE_STATE) || \
    defined(APP_GATEWAY_CONTROL_SEQUENCE_TESTING)
#define APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE 1
#else
#define APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE 0
#endif

struct gateway_control_sequence_block {
    uint32_t next;
    uint32_t remaining;
};

static K_MUTEX_DEFINE(gateway_control_sequence_mutex);
static struct gateway_control_sequence_block gateway_control_sequence_active;
static struct gateway_control_sequence_block gateway_control_sequence_standby;
static struct k_work_delayable gateway_control_sequence_maintenance_work;
static uint32_t gateway_control_sequence_last_reservation_ms;
static int gateway_control_sequence_refill_error;
static bool gateway_control_sequence_ready;
static bool gateway_control_sequence_last_reservation_valid;
static bool gateway_control_sequence_refill_in_flight;
static bool gateway_control_sequence_maintenance_scheduled;

#define GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD \
    (APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE / 2u)

_Static_assert(APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE >
                   APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR,
               "gateway control block must leave a protected runway");
_Static_assert(GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD >
                   APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR,
               "gateway control prefetch must precede the protected runway");
_Static_assert(APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE <
                   UINT32_C(0x80000000),
               "gateway control block must stay inside RFC 1982 half range");
_Static_assert(APP_DURABLE_STATE_COMMAND_MAX_BLOCK_RESERVATIONS >=
                   APP_GATEWAY_CONTROL_SEQUENCE_MIN_DAILY_LIFETIME_DAYS,
               "one gateway control block per day must last at least 44 years");
_Static_assert(APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS <
                   INT32_MAX,
               "gateway control refill interval must be wrap safe");

static uint32_t gateway_control_sequence_increment(uint32_t sequence)
{
    return sequence == UINT32_MAX ? 1u : sequence + 1u;
}

static bool gateway_control_sequence_elapsed(uint32_t now_ms,
                                             uint32_t then_ms,
                                             uint32_t interval_ms)
{
    return (uint32_t)(now_ms - then_ms) >= interval_ms;
}

static uint32_t gateway_control_sequence_wait_until(uint32_t now_ms,
                                                    uint32_t then_ms,
                                                    uint32_t interval_ms)
{
    uint32_t elapsed_ms = now_ms - then_ms;

    return elapsed_ms >= interval_ms ? 0u : interval_ms - elapsed_ms;
}

static uint32_t gateway_control_sequence_runway_locked(void)
{
    return gateway_control_sequence_active.remaining +
           gateway_control_sequence_standby.remaining;
}

static bool gateway_control_sequence_available_locked(uint32_t requested_ids)
{
    uint32_t runway;

    if (!gateway_control_sequence_ready || requested_ids == 0u ||
        requested_ids > UINT32_MAX -
                            APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR) {
        return false;
    }
    runway = gateway_control_sequence_runway_locked();
    return runway >= requested_ids +
                          APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR;
}

static int gateway_control_sequence_schedule_maintenance_locked(
    k_timeout_t delay)
{
    int ret;

    if (gateway_control_sequence_maintenance_scheduled) {
        return 0;
    }
    ret = k_work_reschedule(&gateway_control_sequence_maintenance_work, delay);
    if (ret < 0) {
        return ret;
    }
    gateway_control_sequence_maintenance_scheduled = true;
    return 0;
}

static bool gateway_control_sequence_maintenance_needed_locked(void)
{
    return gateway_control_sequence_ready &&
           !gateway_control_sequence_refill_in_flight &&
           gateway_control_sequence_standby.remaining == 0u &&
           gateway_control_sequence_active.remaining <=
               GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD;
}

static int gateway_control_sequence_ensure_maintenance_locked(
    k_timeout_t delay)
{
    int ret;

    if (!gateway_control_sequence_maintenance_needed_locked()) {
        return 0;
    }
    ret = gateway_control_sequence_schedule_maintenance_locked(delay);
    if (ret < 0) {
        /*
         * A failed work submission owns nothing.  Remember the failure, but
         * leave maintenance_scheduled clear so the next sequence/admission
         * API call can recreate the missing refill owner.
         */
        gateway_control_sequence_refill_error = ret;
    }
    return ret;
}

#if APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE
static int gateway_control_sequence_reserve_block(
    struct gateway_control_sequence_block *block)
{
    struct app_durable_state_reservation reservation = {0};
    uint64_t expected_through;
    int ret;

    if (block == NULL) {
        return -EINVAL;
    }
    ret = app_durable_state_reserve(
        APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE, 0u, &reservation);
    if (ret < 0) {
        return ret;
    }
    if (reservation.first == 0u || reservation.first > UINT32_MAX ||
        reservation.reserved_through == 0u ||
        reservation.reserved_through > UINT32_MAX ||
        reservation.reserved_through < reservation.first) {
        return -EILSEQ;
    }
    expected_through = reservation.first +
                       APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE - 1u;
    if (expected_through != reservation.reserved_through) {
        return -EILSEQ;
    }
    block->next = (uint32_t)reservation.first;
    block->remaining = APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE;
    return 0;
}
#endif

static int gateway_control_sequence_maintain_at(uint32_t now_ms)
{
#if APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE
    struct gateway_control_sequence_block block = {0};
    uint32_t retry_delay_ms = 0u;
    int ret;
#endif

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    gateway_control_sequence_maintenance_scheduled = false;
    if (!gateway_control_sequence_ready) {
        k_mutex_unlock(&gateway_control_sequence_mutex);
        return -EACCES;
    }
#if !APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return 0;
#else
    if (gateway_control_sequence_standby.remaining != 0u ||
        gateway_control_sequence_active.remaining >
            GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD) {
        k_mutex_unlock(&gateway_control_sequence_mutex);
        return 0;
    }
    if (gateway_control_sequence_refill_in_flight) {
        k_mutex_unlock(&gateway_control_sequence_mutex);
        return -EBUSY;
    }
    if (gateway_control_sequence_last_reservation_valid &&
        !gateway_control_sequence_elapsed(
            now_ms,
            gateway_control_sequence_last_reservation_ms,
            APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS)) {
        retry_delay_ms = gateway_control_sequence_wait_until(
            now_ms,
            gateway_control_sequence_last_reservation_ms,
            APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS);
        ret = gateway_control_sequence_ensure_maintenance_locked(
            K_MSEC(retry_delay_ms));
        k_mutex_unlock(&gateway_control_sequence_mutex);
        return ret < 0 ? ret : -EAGAIN;
    }

    /* Rate-limit attempts too, so a failing NVS backend cannot become hot. */
    gateway_control_sequence_refill_in_flight = true;
    gateway_control_sequence_last_reservation_ms = now_ms;
    gateway_control_sequence_last_reservation_valid = true;
    k_mutex_unlock(&gateway_control_sequence_mutex);

    ret = gateway_control_sequence_reserve_block(&block);

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    gateway_control_sequence_refill_in_flight = false;
    if (ret == 0 && gateway_control_sequence_ready &&
        gateway_control_sequence_standby.remaining == 0u) {
        gateway_control_sequence_standby = block;
        gateway_control_sequence_refill_error = 0;
    } else if (ret < 0) {
        int schedule_ret;

        gateway_control_sequence_refill_error = ret;
        schedule_ret = gateway_control_sequence_ensure_maintenance_locked(
            K_MSEC(APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS));
        if (schedule_ret < 0) {
            ret = schedule_ret;
        }
    } else {
        ret = -ECANCELED;
    }
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return ret;
#endif
}

static void gateway_control_sequence_maintenance_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)gateway_control_sequence_maintain_at(k_uptime_get_32());
}

int app_gateway_control_sequence_init(void)
{
    int ret = 0;

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    if (gateway_control_sequence_ready) {
        k_mutex_unlock(&gateway_control_sequence_mutex);
        return 0;
    }
    k_work_init_delayable(&gateway_control_sequence_maintenance_work,
                          gateway_control_sequence_maintenance_handler);
#if APP_GATEWAY_CONTROL_SEQUENCE_USES_DURABLE_STATE
    ret = gateway_control_sequence_reserve_block(
        &gateway_control_sequence_active);
    if (ret == 0) {
        gateway_control_sequence_last_reservation_ms = k_uptime_get_32();
        gateway_control_sequence_last_reservation_valid = true;
    }
#else
    gateway_control_sequence_active = (struct gateway_control_sequence_block) {
        .next = 1u,
        .remaining = UINT32_MAX,
    };
#endif
    if (ret == 0) {
        gateway_control_sequence_standby =
            (struct gateway_control_sequence_block){0};
        gateway_control_sequence_refill_error = 0;
        gateway_control_sequence_refill_in_flight = false;
        gateway_control_sequence_maintenance_scheduled = false;
        gateway_control_sequence_ready = true;
    }
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return ret;
}

int app_gateway_control_sequence_next(uint32_t *sequence)
{
    int ret = 0;

    if (sequence == NULL) {
        return -EINVAL;
    }
    *sequence = 0u;

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    if (!gateway_control_sequence_ready) {
        ret = -EACCES;
        goto out;
    }
    if (gateway_control_sequence_active.remaining == 0u &&
        gateway_control_sequence_standby.remaining != 0u) {
        gateway_control_sequence_active = gateway_control_sequence_standby;
        gateway_control_sequence_standby =
            (struct gateway_control_sequence_block){0};
    }
    if (gateway_control_sequence_active.remaining == 0u) {
        int schedule_ret = gateway_control_sequence_ensure_maintenance_locked(
            K_NO_WAIT);

        ret = gateway_control_sequence_refill_error == -EOVERFLOW ?
              -EOVERFLOW : (schedule_ret < 0 ? schedule_ret : -EAGAIN);
        goto out;
    }
    if (gateway_control_sequence_active.remaining == 0u ||
        gateway_control_sequence_active.next == 0u) {
        ret = -EILSEQ;
        goto out;
    }

    *sequence = gateway_control_sequence_active.next;
    gateway_control_sequence_active.next =
        gateway_control_sequence_increment(*sequence);
    gateway_control_sequence_active.remaining--;
    if (gateway_control_sequence_standby.remaining == 0u &&
        gateway_control_sequence_active.remaining <=
            GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD) {
        (void)gateway_control_sequence_ensure_maintenance_locked(K_NO_WAIT);
    }

out:
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return ret;
}

int app_gateway_control_sequence_next_receiptable(uint32_t *sequence)
{
    uint32_t candidate = 0u;
    int ret;

    if (sequence == NULL) {
        return -EINVAL;
    }
    *sequence = 0u;
    do {
        ret = app_gateway_control_sequence_next(&candidate);
        if (ret < 0) {
            return ret;
        }
    } while ((uint16_t)candidate == 0u);
    *sequence = candidate;
    return 0;
}

bool app_gateway_control_sequence_admission_available(uint32_t requested_ids)
{
    bool available;

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    available = gateway_control_sequence_available_locked(requested_ids);
    if (!available) {
        (void)gateway_control_sequence_ensure_maintenance_locked(K_NO_WAIT);
    }
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return available;
}

int app_gateway_control_sequence_maintain(void)
{
    return gateway_control_sequence_maintain_at(k_uptime_get_32());
}

#if defined(APP_GATEWAY_CONTROL_SEQUENCE_TESTING)
int app_gateway_control_sequence_test_maintain_at(uint32_t now_ms)
{
    return gateway_control_sequence_maintain_at(now_ms);
}

void app_gateway_control_sequence_test_reset(void)
{
    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    gateway_control_sequence_active =
        (struct gateway_control_sequence_block){0};
    gateway_control_sequence_standby =
        (struct gateway_control_sequence_block){0};
    gateway_control_sequence_last_reservation_ms = 0u;
    gateway_control_sequence_refill_error = 0;
    gateway_control_sequence_ready = false;
    gateway_control_sequence_last_reservation_valid = false;
    gateway_control_sequence_refill_in_flight = false;
    gateway_control_sequence_maintenance_scheduled = false;
    k_mutex_unlock(&gateway_control_sequence_mutex);
}

void app_gateway_control_sequence_test_set_schedule_result(int result)
{
    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    gateway_control_sequence_maintenance_work.reschedule_result = result;
    k_mutex_unlock(&gateway_control_sequence_mutex);
}

uint32_t app_gateway_control_sequence_test_schedule_calls(void)
{
    uint32_t calls;

    k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    calls = gateway_control_sequence_maintenance_work.reschedule_calls;
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return calls;
}
#endif
