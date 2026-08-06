#include "app_click_event_sequence.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_click_event_sequence, LOG_LEVEL_INF);

static K_MUTEX_DEFINE(click_event_sequence_lock);
static uint32_t click_event_next;
static bool click_event_sequence_ready;

int app_click_event_sequence_init(void)
{
    k_mutex_lock(&click_event_sequence_lock, K_FOREVER);
    if (click_event_sequence_ready) {
        k_mutex_unlock(&click_event_sequence_lock);
        return 0;
    }
    click_event_next = 1u;
    click_event_sequence_ready = true;
    k_mutex_unlock(&click_event_sequence_lock);
    return 0;
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
    if (click_event_next == 0u) {
        ret = -EOVERFLOW;
        goto out;
    }

    *event_seq = click_event_next;
    click_event_next++;

out:
    k_mutex_unlock(&click_event_sequence_lock);
    return ret;
}
