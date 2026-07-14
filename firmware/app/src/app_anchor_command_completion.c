#include "app_anchor_command_completion.h"

#include "app_node_comm.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

LOG_MODULE_REGISTER(app_anchor_command_completion, LOG_LEVEL_INF);

#define COMMAND_COMPLETION_POLL_MS 5u

struct command_completion_slot {
    uint32_t delivery_handle;
    enum command_id command_id;
    uint8_t actions;
    bool active;
};

static struct app_anchor_command_completion_ops completion_ops;
static struct command_completion_slot
    completion_slots[APP_NODE_COMM_MAX_DELIVERIES];
static struct k_work_delayable completion_work;
static struct k_spinlock completion_lock;
static bool completion_initialized;

static void completion_work_handler(struct k_work *work)
{
    bool pending = false;

    ARG_UNUSED(work);
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        struct node_comm_terminal_event event;
        struct command_completion_slot snapshot;
        k_spinlock_key_t key = k_spin_lock(&completion_lock);

        snapshot = completion_slots[i];
        k_spin_unlock(&completion_lock, key);
        if (!snapshot.active) {
            continue;
        }
        if (!app_node_comm_take_delivery_event_for(snapshot.delivery_handle,
                                                   &event)) {
            pending = true;
            continue;
        }

        key = k_spin_lock(&completion_lock);
        if (completion_slots[i].active &&
            completion_slots[i].delivery_handle == snapshot.delivery_handle) {
            memset(&completion_slots[i], 0, sizeof(completion_slots[i]));
        }
        k_spin_unlock(&completion_lock, key);

        if (event.reason != NODE_COMM_TERMINAL_DELIVERED) {
            LOG_WRN("command result delivery failed: cmd=0x%04x handle=%u reason=%u attempts=%u",
                    (unsigned int)snapshot.command_id,
                    snapshot.delivery_handle,
                    (unsigned int)event.reason,
                    event.attempts_started);
            continue;
        }
        if ((snapshot.actions &
             APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY) != 0u) {
            completion_ops.force_rediscovery();
        }
        if ((snapshot.actions & APP_ANCHOR_COMMAND_COMPLETION_REBOOT) != 0u) {
            completion_ops.schedule_reboot();
        }
        LOG_INF("command result gateway-confirmed: cmd=0x%04x handle=%u attempts=%u",
                (unsigned int)snapshot.command_id,
                snapshot.delivery_handle,
                event.attempts_started);
    }

    if (pending) {
        (void)k_work_reschedule(&completion_work,
                                K_MSEC(COMMAND_COMPLETION_POLL_MS));
    }
}

int app_anchor_command_completion_init(
    const struct app_anchor_command_completion_ops *ops)
{
    if (ops == NULL || ops->force_rediscovery == NULL ||
        ops->schedule_reboot == NULL) {
        return -EINVAL;
    }
    completion_ops = *ops;
    memset(completion_slots, 0, sizeof(completion_slots));
    k_work_init_delayable(&completion_work, completion_work_handler);
    completion_initialized = true;
    return 0;
}

int app_anchor_command_completion_watch(
    uint32_t delivery_handle,
    enum command_id command_id,
    uint8_t actions)
{
    struct command_completion_slot *free_slot = NULL;
    k_spinlock_key_t key;

    if (!completion_initialized || delivery_handle == 0u || actions == 0u) {
        return -EINVAL;
    }
    key = k_spin_lock(&completion_lock);
    for (size_t i = 0u; i < APP_NODE_COMM_MAX_DELIVERIES; i++) {
        if (completion_slots[i].active &&
            completion_slots[i].delivery_handle == delivery_handle) {
            completion_slots[i].actions |= actions;
            k_spin_unlock(&completion_lock, key);
            (void)k_work_reschedule(&completion_work, K_NO_WAIT);
            return 0;
        }
        if (!completion_slots[i].active && free_slot == NULL) {
            free_slot = &completion_slots[i];
        }
    }
    if (free_slot == NULL) {
        k_spin_unlock(&completion_lock, key);
        return -ENOSPC;
    }
    *free_slot = (struct command_completion_slot) {
        .delivery_handle = delivery_handle,
        .command_id = command_id,
        .actions = actions,
        .active = true,
    };
    k_spin_unlock(&completion_lock, key);
    (void)k_work_reschedule(&completion_work, K_NO_WAIT);
    return 0;
}
