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
    uint32_t (*random_u32)(void *ctx);
    int (*send)(const struct mesh_outbound *out, void *ctx);
    uint32_t absolute_deadline_ms;
    bool absolute_deadline_valid;
    void *ctx;
};

struct app_mesh_flood_result {
    uint8_t sent_count;
    uint8_t busy_skip_count;
    uint8_t deferred_count;
    uint32_t first_due_ms;
    uint32_t last_due_ms;
};

struct app_mesh_flood_progress {
    struct app_mesh_flood_result result;
    uint32_t due_ms;
    uint32_t age_origin_ms;
    uint32_t absolute_deadline_ms;
    uint8_t next_opportunity;
    uint8_t backoff_index;
    bool initialized;
    bool complete;
    bool absolute_deadline_valid;
};

uint8_t app_mesh_flood_repeat_limit(void);
uint32_t app_mesh_flood_backoff_ms(uint8_t retry_index, uint32_t random_value);
int app_mesh_flood_send_bounded(const struct mesh_outbound *out,
                                const struct app_mesh_flood_ops *ops,
                                struct app_mesh_flood_result *result);
int app_mesh_flood_send_opportunity(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result);
int app_mesh_flood_send_bounded_resume(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result);
void app_mesh_flood_progress_rebase(struct app_mesh_flood_progress *progress,
                                    uint32_t paused_ms);

#endif
