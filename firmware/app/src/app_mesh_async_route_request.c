#include "app_mesh_async_route_request.h"

#include <string.h>

static bool c5_authorization_equal(
    const struct app_mesh_c5_tx_authorization_token *left,
    const struct app_mesh_c5_tx_authorization_token *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->kind == right->kind && left->peer_id == right->peer_id &&
           left->pending_session_id == right->pending_session_id &&
           left->pending_seq == right->pending_seq &&
           left->pending_msg_type == right->pending_msg_type &&
           left->retained_ack_session_id == right->retained_ack_session_id &&
           left->retained_ack_seq == right->retained_ack_seq &&
           left->retained_ack_valid == right->retained_ack_valid &&
           memcmp(left->pending_digest,
                  right->pending_digest,
                  sizeof(left->pending_digest)) == 0 &&
           memcmp(left->retained_ack_digest,
                  right->retained_ack_digest,
                  sizeof(left->retained_ack_digest)) == 0;
}

static bool transfer_identity_valid(
    const struct app_mesh_async_route_transfer_identity *transfer,
    uint64_t target_id)
{
    return transfer != NULL &&
           (transfer->owner_kind == APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT ||
            transfer->owner_kind == APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING ||
            transfer->owner_kind == APP_MESH_ASYNC_ROUTE_TRANSFER_NODE_COMM) &&
           transfer->target_id == target_id &&
           transfer->owner_generation != 0u &&
           transfer->packet_seq != 0u;
}

static bool transfer_identity_equal(
    const struct app_mesh_async_route_transfer_identity *left,
    const struct app_mesh_async_route_transfer_identity *right)
{
    return left != NULL && right != NULL &&
           left->target_id == right->target_id &&
           left->owner_generation == right->owner_generation &&
           left->packet_seq == right->packet_seq &&
           left->msg_type == right->msg_type &&
           left->owner_kind == right->owner_kind;
}

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
    const struct app_mesh_async_route_transfer_identity *transfer,
    const struct app_mesh_c5_tx_authorization_token *c5_authorization)
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
    if (c5_authorization != NULL && c5_authorization->valid &&
        c5_authorization->peer_id != target_id) {
        return false;
    }
    if (transfer != NULL && !transfer_identity_valid(transfer, target_id)) {
        return false;
    }

    if (request->pending && request->c5_authorization.valid) {
        if (target_id == request->target_id &&
            c5_authorization_equal(&request->c5_authorization,
                                   c5_authorization)) {
            return true;
        }
        return false;
    }
    if (request->pending &&
        request->transfer.owner_kind != APP_MESH_ASYNC_ROUTE_TRANSFER_NONE) {
        if (target_id == request->target_id &&
            transfer_identity_valid(transfer, target_id) &&
            transfer_identity_equal(&request->transfer, transfer)) {
            return true;
        }
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
    if (transfer != NULL) {
        request->transfer = *transfer;
    }
    memset(&request->c5_authorization, 0,
           sizeof(request->c5_authorization));
    if (c5_authorization != NULL && c5_authorization->valid &&
        c5_authorization->peer_id == target_id) {
        request->c5_authorization = *c5_authorization;
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
    attempt->c5_authorization = request->c5_authorization;
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
    uint8_t owner_kind,
    bool owner_valid,
    uint64_t owner_target_id,
    uint32_t owner_generation,
    uint16_t owner_packet_seq,
    uint8_t owner_msg_type)
{
    return attempt != NULL &&
           attempt->transfer.owner_kind !=
               APP_MESH_ASYNC_ROUTE_TRANSFER_NONE &&
           attempt->transfer.owner_kind == owner_kind && owner_valid &&
           owner_target_id == attempt->target_id &&
           owner_target_id == attempt->transfer.target_id &&
           owner_generation ==
               attempt->transfer.owner_generation &&
           owner_packet_seq == attempt->transfer.packet_seq &&
           owner_msg_type == attempt->transfer.msg_type;
}
