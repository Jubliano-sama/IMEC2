#include "app_mesh_async_route_request.h"

#include <string.h>

static bool attempt_matches(
    const struct app_mesh_async_route_request *request,
    const struct app_mesh_async_route_attempt *attempt)
{
    return request != NULL && attempt != NULL && request->pending &&
           request->generation == attempt->generation &&
           request->target_id == attempt->target_id;
}

void app_mesh_async_route_request_init(
    struct app_mesh_async_route_request *request)
{
    if (request != NULL) {
        memset(request, 0, sizeof(*request));
    }
}

bool app_mesh_async_route_request_submit(
    struct app_mesh_async_route_request *request,
    uint64_t target_id,
    const char *reason,
    uint32_t now_ms,
    const struct app_mesh_async_route_transfer_identity *transfer)
{
    const char *selected_reason = reason == NULL ? "async-route" : reason;
    size_t reason_len;
    uint32_t generation;

    if (request == NULL || target_id == 0u) {
        return false;
    }
    reason_len = strlen(selected_reason);
    if (reason_len >= sizeof(request->reason)) {
        return false;
    }

    generation = request->generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    request->target_id = target_id;
    request->generation = generation;
    request->retry_at_ms = now_ms;
    memset(&request->transfer, 0, sizeof(request->transfer));
    if (transfer != NULL && transfer->valid &&
        transfer->target_id == target_id &&
        transfer->owner_generation != 0u &&
        transfer->packet_seq != 0u) {
        request->transfer = *transfer;
    }
    memcpy(request->reason, selected_reason, reason_len + 1u);
    request->retry_at_valid = true;
    request->pending = true;
    return true;
}

bool app_mesh_async_route_request_snapshot(
    const struct app_mesh_async_route_request *request,
    struct app_mesh_async_route_attempt *attempt)
{
    if (request == NULL || attempt == NULL || !request->pending ||
        request->target_id == 0u || request->generation == 0u ||
        request->reason[0] == '\0') {
        return false;
    }

    attempt->target_id = request->target_id;
    attempt->generation = request->generation;
    attempt->transfer = request->transfer;
    memcpy(attempt->reason, request->reason, sizeof(attempt->reason));
    attempt->reason[sizeof(attempt->reason) - 1u] = '\0';
    return true;
}

bool app_mesh_async_route_request_complete(
    struct app_mesh_async_route_request *request,
    const struct app_mesh_async_route_attempt *attempt)
{
    uint32_t generation;

    if (!attempt_matches(request, attempt)) {
        return false;
    }

    generation = request->generation;
    memset(request, 0, sizeof(*request));
    request->generation = generation;
    return true;
}

bool app_mesh_async_route_request_defer(
    struct app_mesh_async_route_request *request,
    const struct app_mesh_async_route_attempt *attempt,
    uint32_t now_ms,
    uint32_t delay_ms)
{
    if (!attempt_matches(request, attempt)) {
        return false;
    }

    request->retry_at_ms = now_ms + delay_ms;
    request->retry_at_valid = true;
    return true;
}

bool app_mesh_async_route_request_retry_delay_ms(
    const struct app_mesh_async_route_request *request,
    uint32_t now_ms,
    uint32_t *delay_ms)
{
    if (request == NULL || delay_ms == NULL || !request->pending) {
        return false;
    }
    if (!request->retry_at_valid ||
        (int32_t)(now_ms - request->retry_at_ms) >= 0) {
        *delay_ms = 0u;
    } else {
        *delay_ms = request->retry_at_ms - now_ms;
    }
    return true;
}

bool app_mesh_async_route_request_transfer_matches(
    const struct app_mesh_async_route_attempt *attempt,
    bool route_waiting_valid,
    uint64_t route_waiting_target_id,
    uint32_t route_waiting_owner_generation,
    uint16_t route_waiting_packet_seq,
    uint8_t route_waiting_msg_type)
{
    return attempt != NULL && attempt->transfer.valid &&
           route_waiting_valid &&
           route_waiting_target_id == attempt->target_id &&
           route_waiting_target_id == attempt->transfer.target_id &&
           route_waiting_owner_generation ==
               attempt->transfer.owner_generation &&
           route_waiting_packet_seq == attempt->transfer.packet_seq &&
           route_waiting_msg_type == attempt->transfer.msg_type;
}
