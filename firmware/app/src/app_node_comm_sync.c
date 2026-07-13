#include "app_node_comm_sync.h"

#include <zephyr/kernel.h>

#include <errno.h>

K_MUTEX_DEFINE(node_comm_state_lock);

int app_node_comm_sync_lock(void)
{
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    return k_mutex_lock(&node_comm_state_lock, K_FOREVER);
}

void app_node_comm_sync_unlock(void)
{
    k_mutex_unlock(&node_comm_state_lock);
}
