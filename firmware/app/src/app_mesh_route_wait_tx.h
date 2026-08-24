#ifndef APP_MESH_ROUTE_WAIT_TX_H
#define APP_MESH_ROUTE_WAIT_TX_H

#include "app_mesh_direct_gateway_retry.h"
#include "protocol.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_route_wait_tx_action {
    APP_MESH_ROUTE_WAIT_TX_ACTION_NONE = 0,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY,
    APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE,
    APP_MESH_ROUTE_WAIT_TX_ACTION_DROP,
    APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID,
};

enum app_mesh_route_wait_tx_owner {
    APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC = 0,
    APP_MESH_ROUTE_WAIT_TX_OWNER_RETAINED_LOCAL,
    APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK,
};

bool app_mesh_route_wait_tx_may_store(
    enum app_mesh_route_wait_tx_owner owner);

/* Match one clear request to the exact route-wait owner and full immutable
 * packet commitment. The transport owner supplies semantic digests that
 * commit the full payload; TTL and message age are retry-local fields. */
bool app_mesh_route_wait_tx_clear_matches(
    enum app_mesh_route_wait_tx_owner active_owner,
    const struct proto_packet *active_packet,
    const uint8_t active_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum app_mesh_route_wait_tx_owner expected_owner,
    const struct proto_packet *expected_packet,
    const uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN]);

struct app_mesh_route_wait_tx_state {
    bool outbound_ready;
    int tx_ret;
    bool route_request_attempted;
    int route_request_ret;
    uint32_t channel9_retry_delay_ms;
    uint32_t busy_retry_delay_ms;
};

struct app_mesh_route_wait_tx_decision {
    enum app_mesh_route_wait_tx_action action;
    const char *reason;
    uint32_t delay_ms;
};

void app_mesh_route_wait_tx_decide(
    const struct app_mesh_route_wait_tx_state *state,
    struct app_mesh_route_wait_tx_decision *decision);

#endif
