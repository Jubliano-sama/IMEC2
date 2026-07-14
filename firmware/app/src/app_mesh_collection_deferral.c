#include "app_mesh_collection_deferral.h"

#include <string.h>

bool app_mesh_collection_defer_active_result(
    struct mesh_relay *relay,
    uint32_t now_ms,
    uint32_t random_value,
    const struct app_mesh_collection_deferral_ops *ops,
    struct app_mesh_collection_deferral_result *result)
{
    struct app_mesh_collection_deferral_result local_result;

    memset(&local_result, 0, sizeof(local_result));
    if (!mesh_relay_defer_tx(relay, now_ms, random_value)) {
        if (result != NULL) {
            *result = local_result;
        }
        return false;
    }

    local_result.deferred = true;
    if (ops != NULL && ops->save_outbox != NULL) {
        local_result.save_ret = ops->save_outbox(relay, now_ms, ops->ctx);
        local_result.outbox_saved = local_result.save_ret == 0;
    }
    if (ops != NULL && ops->schedule_retry != NULL) {
        local_result.schedule_ret = ops->schedule_retry(ops->ctx);
        local_result.retry_scheduled = local_result.schedule_ret >= 0;
    }

    if (result != NULL) {
        *result = local_result;
    }
    return true;
}
