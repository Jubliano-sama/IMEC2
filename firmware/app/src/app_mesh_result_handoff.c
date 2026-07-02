#include "app_mesh_result_handoff.h"

#include <errno.h>
#include <string.h>

static void status_init(struct app_mesh_result_handoff_status *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
        status->child_custody_ready = true;
    }
}

static void status_note_save(struct app_mesh_result_handoff_status *status,
                             int ret)
{
    if (status == NULL) {
        return;
    }

    status->save_ret = ret;
    if (ret == 0) {
        status->child_custody_saved = true;
        status->child_custody_ready = true;
    } else {
        status->child_custody_save_failed = true;
        status->child_custody_ready = false;
    }
}

static int save_child_custody_if_needed(
    bool anchor_role,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status)
{
    int ret = 0;

    if (anchor_role && ops != NULL && ops->save_child_custody != NULL) {
        ret = ops->save_child_custody(ops->ctx);
    }
    status_note_save(status, ret);
    return ret;
}

void app_mesh_result_handoff_after_forward(
    const struct mesh_relay_result *result,
    bool forward_sent,
    bool anchor_role,
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

    (void)save_child_custody_if_needed(anchor_role, ops, status);
}

void app_mesh_result_handoff_result_grant(
    const struct mesh_relay_result *result,
    bool anchor_role,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status)
{
    int ret;

    status_init(status);
    if (result == NULL ||
        (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT) == 0u) {
        return;
    }

    ret = save_child_custody_if_needed(anchor_role, ops, status);
    if (ret != 0) {
        if (status != NULL) {
            status->result_grant_suppressed = true;
        }
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
