#ifndef APP_MESH_RESULT_HANDOFF_H
#define APP_MESH_RESULT_HANDOFF_H

#include "mesh_relay.h"

#include <stdbool.h>

struct app_mesh_result_handoff_ops {
    void (*note_result_bundle_forwarded)(const struct mesh_outbound *out,
                                         void *ctx);
    int (*send_result_grant)(const struct mesh_outbound *out, void *ctx);
    void (*note_tx_sent)(const struct mesh_outbound *out, void *ctx);
    void *ctx;
};

struct app_mesh_result_handoff_status {
    bool result_bundle_forward_noted;
    bool result_grant_sent;
    bool hop_ack_allowed;
    int send_ret;
};

void app_mesh_result_handoff_after_forward(
    const struct mesh_relay_result *result,
    bool forward_sent,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status);

void app_mesh_result_handoff_result_grant(
    const struct mesh_relay_result *result,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status);

void app_mesh_result_handoff_prepare_hop_ack(
    const struct mesh_relay_result *result,
    bool forward_sent,
    const struct app_mesh_result_handoff_ops *ops,
    struct app_mesh_result_handoff_status *status);

#endif
