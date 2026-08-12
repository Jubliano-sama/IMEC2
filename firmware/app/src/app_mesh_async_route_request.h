#ifndef APP_MESH_ASYNC_ROUTE_REQUEST_H
#define APP_MESH_ASYNC_ROUTE_REQUEST_H

#include "app_mesh_c5_repair_authorization.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MESH_ASYNC_ROUTE_REASON_CAPACITY 80u

enum app_mesh_async_route_transfer_owner {
    APP_MESH_ASYNC_ROUTE_TRANSFER_NONE = 0,
    APP_MESH_ASYNC_ROUTE_TRANSFER_ROUTE_WAIT = 1,
    APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING = 2,
};

struct app_mesh_async_route_transfer_identity {
    uint64_t target_id;
    uint32_t owner_generation;
    uint16_t packet_seq;
    uint8_t msg_type;
    uint8_t owner_kind;
};

struct app_mesh_async_route_request {
    uint64_t target_id;
    uint32_t generation;
    uint32_t retry_at_ms;
    struct app_mesh_async_route_transfer_identity transfer;
    struct app_mesh_c5_tx_authorization_token c5_authorization;
    char reason[APP_MESH_ASYNC_ROUTE_REASON_CAPACITY];
    bool retry_at_valid;
    bool pending;
};

struct app_mesh_async_route_attempt {
    uint64_t target_id;
    uint32_t generation;
    struct app_mesh_async_route_transfer_identity transfer;
    struct app_mesh_c5_tx_authorization_token c5_authorization;
    char reason[APP_MESH_ASYNC_ROUTE_REASON_CAPACITY];
};

void app_mesh_async_route_request_init(
    struct app_mesh_async_route_request *request);

bool app_mesh_async_route_request_submit(
    struct app_mesh_async_route_request *request,
    uint64_t target_id,
    const char *reason,
    uint32_t now_ms,
    const struct app_mesh_async_route_transfer_identity *transfer,
    const struct app_mesh_c5_tx_authorization_token *c5_authorization);

bool app_mesh_async_route_request_snapshot(
    const struct app_mesh_async_route_request *request,
    struct app_mesh_async_route_attempt *attempt);

bool app_mesh_async_route_request_complete(
    struct app_mesh_async_route_request *request,
    const struct app_mesh_async_route_attempt *attempt);

bool app_mesh_async_route_request_defer(
    struct app_mesh_async_route_request *request,
    const struct app_mesh_async_route_attempt *attempt,
    uint32_t now_ms,
    uint32_t delay_ms);

bool app_mesh_async_route_request_retry_delay_ms(
    const struct app_mesh_async_route_request *request,
    uint32_t now_ms,
    uint32_t *delay_ms);

bool app_mesh_async_route_request_transfer_matches(
    const struct app_mesh_async_route_attempt *attempt,
    uint8_t owner_kind,
    bool owner_valid,
    uint64_t owner_target_id,
    uint32_t owner_generation,
    uint16_t owner_packet_seq,
    uint8_t owner_msg_type);

#endif
