#include "app_mesh_route_wait_tx.h"

#include <errno.h>
#include <string.h>

static void decision_init(struct app_mesh_route_wait_tx_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
}

bool app_mesh_route_wait_tx_may_store(
    enum app_mesh_route_wait_tx_owner owner)
{
    return owner == APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
}

bool app_mesh_route_wait_tx_clear_matches(
    enum app_mesh_route_wait_tx_owner active_owner,
    const struct proto_packet *active_packet,
    const uint8_t active_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum app_mesh_route_wait_tx_owner expected_owner,
    const struct proto_packet *expected_packet,
    const uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return active_packet != NULL && expected_packet != NULL &&
           active_digest != NULL && expected_digest != NULL &&
           active_owner == expected_owner &&
           active_packet->msg_type == expected_packet->msg_type &&
           active_packet->flags == expected_packet->flags &&
           active_packet->src_id == expected_packet->src_id &&
           active_packet->dst_id == expected_packet->dst_id &&
           active_packet->session_id == expected_packet->session_id &&
           active_packet->seq == expected_packet->seq &&
           active_packet->payload_len == expected_packet->payload_len &&
           semantic_digest_equal(active_digest,
                                 expected_digest,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}

void app_mesh_route_wait_tx_decide(
    const struct app_mesh_route_wait_tx_state *state,
    struct app_mesh_route_wait_tx_decision *decision)
{
    decision_init(decision);
    if (decision == NULL || state == NULL) {
        return;
    }

    if (!state->outbound_ready) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY;
        decision->reason = "route-waiting-not-ready";
        return;
    }

    if (state->tx_ret == 0) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID;
        return;
    }

    if (state->tx_ret == -EHOSTUNREACH) {
        if (!state->route_request_attempted) {
            decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE;
            decision->reason = "route-waiting-packet";
            return;
        }
        if (state->route_request_ret == -ETIMEDOUT) {
            decision->action =
                APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY;
            decision->reason = "route-waiting-exhausted";
        }
        return;
    }

    if (state->tx_ret == -ETIMEDOUT) {
        decision->action =
            APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY;
        decision->reason = "route-waiting-stale";
        return;
    }

    if (state->tx_ret == -EBUSY) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY;
        decision->reason = "route-waiting-channel9-event";
        decision->delay_ms = state->channel9_retry_delay_ms;
        return;
    }

    decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY;
    decision->reason = "route-waiting-busy";
    decision->delay_ms = state->busy_retry_delay_ms;
}
