#include "app_click_event_sequence.h"

#if defined(CONFIG_IMEC_DURABLE_STATE) || \
    defined(APP_CLICK_EVENT_SEQUENCE_TESTING)
#include "app_durable_state.h"
#define APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE 1
#else
#define APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE 0
#endif

#include <zephyr/kernel.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>

static K_MUTEX_DEFINE(click_event_sequence_lock);
struct click_event_sequence_block {
    uint32_t next;
    uint32_t remaining;
};

static struct click_event_sequence_block click_event_active_block;
static struct click_event_sequence_block click_event_standby_block;
static bool click_event_sequence_ready;

#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
#define CLICK_EVENT_SEQUENCE_PREFETCH_THRESHOLD \
    (APP_DURABLE_STATE_CLICK_BLOCK_SIZE / 2u)

static int click_event_prefetch_error;
static int64_t click_event_prefetch_retry_at_ms;
static uint32_t click_event_prefetch_retry_delay_ms =
    APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS;

static int click_event_sequence_reserve_block_locked(
    uint32_t *next,
    uint32_t *remaining)
{
    struct app_durable_state_reservation reservation = {0};
    uint64_t count;
    int ret;

    if (next == NULL || remaining == NULL) {
        return -EINVAL;
    }
    if (*remaining != 0u) {
        return 0;
    }
    ret = app_durable_state_reserve(
        APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE, 0u, &reservation);
    if (ret < 0) {
        return ret;
    }
    if (reservation.first == 0u ||
        reservation.first > UINT32_MAX ||
        reservation.reserved_through > UINT32_MAX ||
        reservation.reserved_through < reservation.first) {
        return -EILSEQ;
    }
    count = reservation.reserved_through - reservation.first + 1u;
    if (count != APP_DURABLE_STATE_CLICK_BLOCK_SIZE) {
        return -EILSEQ;
    }
    *next = (uint32_t)reservation.first;
    *remaining = APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
    return 0;
}

static void click_event_sequence_reset_prefetch_retry_locked(void)
{
    click_event_prefetch_error = 0;
    click_event_prefetch_retry_at_ms = 0;
    click_event_prefetch_retry_delay_ms =
        APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS;
}

static void click_event_sequence_note_prefetch_failure_locked(int ret,
                                                               int64_t now_ms)
{
    uint32_t delay_ms = click_event_prefetch_retry_delay_ms;

    click_event_prefetch_error = ret;
    if (ret == -EOVERFLOW) {
        /* This sequence namespace cannot advance again after active drains. */
        click_event_prefetch_retry_at_ms = 0;
        return;
    }
    if (now_ms > INT64_MAX - (int64_t)delay_ms) {
        click_event_prefetch_retry_at_ms = INT64_MAX;
    } else {
        click_event_prefetch_retry_at_ms = now_ms + (int64_t)delay_ms;
    }
    if (delay_ms < APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS) {
        delay_ms *= 2u;
        if (delay_ms > APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS) {
            delay_ms = APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS;
        }
        click_event_prefetch_retry_delay_ms = delay_ms;
    }
}
#endif

int app_click_event_sequence_init(void)
{
    int ret = 0;

    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (click_event_sequence_ready) {
        k_mutex_unlock(&click_event_sequence_lock);
        return 0;
    }
#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
    ret = click_event_sequence_reserve_block_locked(
        &click_event_active_block.next, &click_event_active_block.remaining);
    if (ret == 0) {
        click_event_sequence_reset_prefetch_retry_locked();
        click_event_sequence_ready = true;
    }
#else
    click_event_active_block.next = 1u;
    click_event_active_block.remaining = UINT32_MAX;
    click_event_sequence_ready = true;
#endif
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}

int app_click_event_sequence_next(uint32_t *event_seq)
{
    int ret = 0;

    if (event_seq == NULL) {
        return -EINVAL;
    }
    *event_seq = 0u;

    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (!click_event_sequence_ready) {
        ret = -EACCES;
        goto out;
    }
    if (click_event_active_block.remaining == 0u &&
        click_event_standby_block.remaining != 0u) {
        click_event_active_block = click_event_standby_block;
        click_event_standby_block =
            (struct click_event_sequence_block){0};
#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
        click_event_sequence_reset_prefetch_retry_locked();
#endif
    }
    if (click_event_active_block.remaining == 0u) {
#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
        ret = click_event_prefetch_error == -EOVERFLOW ? -EOVERFLOW :
              -EAGAIN;
#else
        ret = -EOVERFLOW;
#endif
        goto out;
    }
    if (click_event_active_block.next == 0u) {
        ret = -EILSEQ;
        goto out;
    }

    *event_seq = click_event_active_block.next;
    click_event_active_block.next++;
    click_event_active_block.remaining--;

out:
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}

int app_click_event_sequence_maintain(void)
{
    int ret = 0;

    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (!click_event_sequence_ready) {
        ret = -EACCES;
        goto out;
    }
#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
    if (click_event_standby_block.remaining != 0u ||
        click_event_active_block.remaining >
            CLICK_EVENT_SEQUENCE_PREFETCH_THRESHOLD) {
        goto out;
    }
    if (click_event_prefetch_error == -EOVERFLOW) {
        goto out;
    }
    if (click_event_prefetch_error != 0 &&
        k_uptime_get() < click_event_prefetch_retry_at_ms) {
        goto out;
    }
    ret = click_event_sequence_reserve_block_locked(
        &click_event_standby_block.next, &click_event_standby_block.remaining);
    if (ret < 0) {
        click_event_sequence_note_prefetch_failure_locked(ret,
                                                           k_uptime_get());
        goto out;
    }
    click_event_sequence_reset_prefetch_retry_locked();
#endif

out:
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}

#if defined(APP_CLICK_EVENT_SEQUENCE_TESTING)
void app_click_event_sequence_test_reset(void)
{
    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    click_event_active_block = (struct click_event_sequence_block){0};
    click_event_standby_block = (struct click_event_sequence_block){0};
#if APP_CLICK_EVENT_SEQUENCE_USES_DURABLE_STATE
    click_event_sequence_reset_prefetch_retry_locked();
#endif
    click_event_sequence_ready = false;
    k_mutex_unlock(&click_event_sequence_lock);
}
#endif
