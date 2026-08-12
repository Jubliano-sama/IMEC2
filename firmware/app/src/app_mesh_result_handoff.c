#include "app_mesh_result_handoff.h"

#include <errno.h>
#include <string.h>

static void status_init(struct app_mesh_result_handoff_status *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

void app_mesh_result_handoff_after_forward(
    const struct mesh_relay_result *result,
    bool forward_sent,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status)
{
    status_init(status);
    if (result == NULL || !forward_sent ||
        (result->actions & MESH_RELAY_ACTION_FORWARD) == 0u) {
        return;
    }

    if (result->forward.packet.msg_type == MSG_RESULT_BUNDLE &&
        ops != NULL && ops->note_result_bundle_forwarded != NULL) {
        ops->note_result_bundle_forwarded(&result->forward, ops->ctx);
        if (status != NULL) {
            status->result_bundle_forward_noted = true;
        }
    }

}

void app_mesh_result_handoff_result_grant(
    const struct mesh_relay_result *result,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status)
{
    int ret;

    status_init(status);
    if (result == NULL ||
        (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT) == 0u) {
        return;
    }

    if (ops != NULL && ops->send_result_grant != NULL) {
        ret = ops->send_result_grant(&result->result_grant, ops->ctx);
    } else {
        ret = -ENOSYS;
    }

    if (status != NULL) {
        status->send_ret = ret;
        if (ret == 0) {
            status->result_grant_sent = true;
        }
    }
    if (ret == 0 && ops != NULL && ops->note_tx_sent != NULL) {
        ops->note_tx_sent(&result->result_grant, ops->ctx);
    }
}

void app_mesh_result_handoff_prepare_hop_ack(
    const struct mesh_relay_result *result,
    bool forward_sent,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status)
{
    bool custody_accepted;

    (void)ops;

    status_init(status);
    if (result == NULL ||
        (result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) == 0u) {
        return;
    }

    custody_accepted =
        (result->actions & MESH_RELAY_ACTION_CUSTODY_ACCEPTED) != 0u;
    if (status != NULL) {
        status->hop_ack_allowed = forward_sent || custody_accepted;
    }
}
