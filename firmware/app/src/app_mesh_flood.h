#ifndef APP_MESH_FLOOD_H
#define APP_MESH_FLOOD_H

#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_flood_ops {
    uint32_t (*now_ms)(void *ctx);
    void (*sleep_until_ms)(uint32_t due_ms, void *ctx);
    bool (*defer_active)(void *ctx);
    bool (*c5_quiet)(uint32_t sniff_ms, void *ctx);
    int (*send)(const struct mesh_outbound *out, void *ctx);
    void *ctx;
};

struct app_mesh_flood_result {
    uint8_t sent_count;
    uint8_t busy_skip_count;
    uint8_t deferred_count;
    uint32_t first_due_ms;
    uint32_t last_due_ms;
};

uint8_t app_mesh_flood_repeat_limit(void);
int app_mesh_flood_send_bounded(const struct mesh_outbound *out,
                                const struct app_mesh_flood_ops *ops,
                                struct app_mesh_flood_result *result);

#endif
