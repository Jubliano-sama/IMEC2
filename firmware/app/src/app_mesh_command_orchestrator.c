#include "app_mesh_command_orchestrator.h"

#include <errno.h>
#include <string.h>

void app_mesh_command_orchestrator_reset(
    struct app_mesh_command_orchestrator *orchestrator)
{
    if (orchestrator != NULL) {
        memset(orchestrator, 0, sizeof(*orchestrator));
    }
}

int app_mesh_command_orchestrator_activate(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct app_gateway_command_ingress_item *item)
{
    if (orchestrator == NULL || item == NULL || item->packet.msg_type != MSG_COMMAND) {
        return -EINVAL;
    }
    orchestrator->admitted = *item;
    orchestrator->command_admitted = true;
    orchestrator->safe_boundary_observed = false;
    return 0;
}

void app_mesh_command_orchestrator_clear_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator)
{
    if (orchestrator != NULL) {
        orchestrator->safe_boundary_observed = false;
    }
}

void app_mesh_command_orchestrator_mark_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator)
{
    if (orchestrator != NULL) {
        orchestrator->safe_boundary_observed = true;
    }
}

int app_mesh_command_orchestrator_gateway_ingress(
    const struct app_gateway_command_ingress_ops *ops,
    const uint8_t *frame,
    size_t frame_len,
    struct app_gateway_command_ingress_item *decoded,
    bool *command_handled)
{
    if (decoded == NULL || command_handled == NULL) {
        return -EINVAL;
    }
    /*
     * BLE RX is a higher-priority system-workqueue producer. Decode only into
     * caller-owned scratch; the ingress callbacks copy accepted commands into
     * the route-owned queue. It must never mutate the active route dispatch.
     */
    memset(decoded, 0, sizeof(*decoded));
    return app_gateway_command_ingress_handle_frame(ops,
                                                     frame,
                                                     frame_len,
                                                     decoded,
                                                     command_handled);
}

int app_mesh_command_orchestrator_decide(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_decision *decision,
    bool *state_changed)
{
    if (orchestrator == NULL) {
        return -EINVAL;
    }
    return app_mesh_coordinator_runtime_decide(capture,
                                               &orchestrator->coordinator,
                                               decision,
                                               state_changed);
}

int app_mesh_command_orchestrator_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator,
    bool ds_twr_active,
    app_mesh_command_safe_boundary_fn schedule_command,
    void *ctx)
{
    int ret;

    if (orchestrator == NULL || schedule_command == NULL) {
        return -EINVAL;
    }
    if (ds_twr_active) {
        return -EAGAIN;
    }
    ret = schedule_command(ctx);
    if (ret == 0) {
        orchestrator->safe_boundary_observed = true;
    }
    return ret;
}

int app_mesh_command_orchestrator_prepare_flood(
    struct app_mesh_command_orchestrator *orchestrator,
    uint64_t gateway_id,
    uint32_t now_ms,
    uint16_t fallback_seq)
{
    if (orchestrator == NULL || !orchestrator->command_admitted) {
        return -EINVAL;
    }
    return app_mesh_gateway_command_flow_prepare(
        &orchestrator->admitted.packet,
        orchestrator->admitted.payload,
        orchestrator->admitted.payload_len,
        gateway_id,
        now_ms,
        fallback_seq,
        &orchestrator->gateway_flow);
}

int app_mesh_command_orchestrator_send_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result)
{
    if (orchestrator == NULL || ops == NULL || result == NULL ||
        !orchestrator->safe_boundary_observed) {
        return -EINVAL;
    }
    return app_mesh_command_orchestrator_serialize_flood(
        &orchestrator->gateway_flow.outbound, ops, result);
}

int app_mesh_command_orchestrator_serialize_flood(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result)
{
    return app_mesh_flood_send_bounded(out, ops, result);
}

int app_mesh_command_orchestrator_anchor_receive(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint32_t now_ms,
    enum command_id *command_id,
    struct gateway_command_options *options,
    bool *broadcast,
    bool *expired,
    bool *duplicate)
{
    int ret;

    if (orchestrator == NULL) {
        return -EINVAL;
    }
    ret = app_mesh_gateway_command_flow_anchor_receive(
        &orchestrator->anchor,
        packet,
        payload,
        payload_len,
        gateway_id,
        now_ms,
        command_id,
        options,
        broadcast,
        expired,
        duplicate);
    return ret;
}

void app_mesh_command_orchestrator_anchor_commit(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    uint32_t now_ms)
{
    if (orchestrator != NULL) {
        app_mesh_gateway_command_flow_anchor_remember(&orchestrator->anchor,
                                                       packet,
                                                       options,
                                                       now_ms);
    }
}

int app_mesh_command_orchestrator_anchor_result(
    const struct proto_packet *command,
    uint64_t source_id,
    uint64_t gateway_id,
    bool diagnostic,
    struct mesh_outbound *result)
{
    return app_mesh_gateway_command_flow_init_result(result,
                                                     command,
                                                     source_id,
                                                     gateway_id,
                                                     diagnostic);
}

int app_mesh_command_orchestrator_gateway_deliver(
    const struct proto_packet *pending_command,
    enum command_id pending_command_id,
    const struct proto_packet *result_packet,
    const uint8_t *payload,
    size_t payload_len,
    app_mesh_command_gateway_delivery_fn deliver,
    void *ctx)
{
    enum command_status status;
    uint8_t reason;
    int ret;

    if (!app_mesh_gateway_command_flow_result_matches(pending_command,
                                                       result_packet) ||
        deliver == NULL) {
        return -ENOENT;
    }
    ret = app_mesh_gateway_command_flow_decode_result(pending_command_id,
                                                       payload,
                                                       payload_len,
                                                       &status,
                                                       &reason);
    if (ret != 0) {
        return ret;
    }
    return deliver(ctx, pending_command, pending_command_id, status, reason);
}
