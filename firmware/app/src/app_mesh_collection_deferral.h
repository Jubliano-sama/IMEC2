#ifndef APP_MESH_COLLECTION_DEFERRAL_H
#define APP_MESH_COLLECTION_DEFERRAL_H

#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_collection_deferral_ops {
    int (*save_outbox)(struct mesh_relay *relay, uint32_t now_ms, void *ctx);
    int (*schedule_retry)(void *ctx);
    void *ctx;
};

struct app_mesh_collection_deferral_result {
    bool deferred;
    bool outbox_saved;
    bool retry_scheduled;
    int save_ret;
    int schedule_ret;
};

bool app_mesh_collection_defer_active_result(
    struct mesh_relay *relay,
    uint32_t now_ms,
    const struct app_mesh_collection_deferral_ops *ops,
    struct app_mesh_collection_deferral_result *result);

#endif
