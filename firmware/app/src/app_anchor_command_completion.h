#ifndef APP_ANCHOR_COMMAND_COMPLETION_H
#define APP_ANCHOR_COMMAND_COMPLETION_H

#include "protocol.h"

#include <stdint.h>

enum app_anchor_command_completion_action {
    APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY = 1u << 0,
    APP_ANCHOR_COMMAND_COMPLETION_REBOOT = 1u << 1,
};

struct app_anchor_command_completion_ops {
    void (*force_rediscovery)(void);
    void (*schedule_reboot)(void);
};

int app_anchor_command_completion_init(
    const struct app_anchor_command_completion_ops *ops);
int app_anchor_command_completion_watch(
    uint32_t delivery_handle,
    enum command_id command_id,
    uint8_t actions);

#endif
