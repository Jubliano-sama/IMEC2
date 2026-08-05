#include "app_mesh_route_reply_ack.h"

#include <errno.h>
#include <string.h>

static void attempt_result_init(struct app_mesh_route_reply_ack_attempt_result *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->action = APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_FAILED;
        result->return_ret = -EINVAL;
    }
}

static void backup_result_init(struct app_mesh_route_reply_ack_backup_result *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->return_ret = -EINVAL;
        result->clear_reason = "route-reply-failed";
    }
}

static bool id_is_unicast(uint64_t node_id)
{
    return node_id != 0u && node_id != UINT64_MAX;
}

static bool deadline_after(uint32_t candidate_ms, uint32_t reference_ms)
{
    return (int32_t)(candidate_ms - reference_ms) > 0;
}

void app_mesh_route_reply_ack_decide_attempt(
    const struct app_mesh_route_reply_ack_attempt_state *state,
    struct app_mesh_route_reply_ack_attempt_result *result)
{
    int ret;

    attempt_result_init(result);
    if (state == NULL || result == NULL) {
        return;
    }

    ret = state->send_ret;
    if (ret == 0 && state->listen_attempted) {
        ret = state->listen_ret;
    }

    if (ret == 0 && state->listen_attempted) {
        result->action = APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS;
        result->return_ret = 0;
        return;
    }

    result->return_ret = ret;
    if (state->attempt < state->max_retries) {
        result->action = APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY;
        result->note_retry = true;
        return;
    }

    result->action = APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_FAILED;
}

void app_mesh_route_reply_ack_decide_backup(
    const struct app_mesh_route_reply_ack_backup_state *state,
    struct app_mesh_route_reply_ack_backup_result *result)
{
    backup_result_init(result);
    if (state == NULL || result == NULL) {
        return;
    }

    result->return_ret = state->primary_ret;
    if (!state->backup_valid ||
        !id_is_unicast(state->backup_next_hop_id) ||
        state->backup_next_hop_id == state->primary_next_hop_id) {
        result->clear_reason = "route-reply-failed";
        return;
    }

    result->try_backup = true;
    result->note_retry = true;
    result->backup_next_hop_id = state->backup_next_hop_id;
}

bool app_mesh_route_reply_upstream_ack_allowed(
    bool downstream_handoff_required,
    bool downstream_handoff_acked)
{
    return !downstream_handoff_required || downstream_handoff_acked;
}

uint32_t app_mesh_route_reply_ack_deadline_after_preemption(
    uint32_t preempted_at_ms,
    uint32_t timeout_ms,
    uint32_t latest_deadline_ms,
    bool latest_deadline_valid)
{
    uint32_t deadline_ms = preempted_at_ms + (timeout_ms == 0u ? 1u : timeout_ms);

    if (latest_deadline_valid &&
        deadline_after(deadline_ms, latest_deadline_ms)) {
        return latest_deadline_ms;
    }
    return deadline_ms;
}
