#ifndef APP_MESH_COMMAND_ORCHESTRATOR_H
#define APP_MESH_COMMAND_ORCHESTRATOR_H

#include "app_gateway_command_ingress.h"
#include "app_mesh_coordinator_runtime.h"
#include "app_mesh_flood.h"
#include "app_mesh_gateway_command_flow.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * This is the production host seam for a gateway command.  It keeps the
 * command lifecycle assembled in one native-testable place; platform code only
 * owns the final work-queue, DWM and GATT boundaries.
 */
struct app_mesh_command_orchestrator {
    struct app_mesh_coordinator_runtime_state coordinator;
    struct app_mesh_gateway_command_anchor_state anchor;
    struct app_mesh_gateway_command_flow gateway_flow;
    struct app_gateway_command_ingress_item admitted;
    bool command_admitted;
    bool safe_boundary_observed;
};

typedef int (*app_mesh_command_safe_boundary_fn)(void *ctx);
typedef int (*app_mesh_command_gateway_delivery_fn)(
    void *ctx,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason);

void app_mesh_command_orchestrator_reset(
    struct app_mesh_command_orchestrator *orchestrator);
int app_mesh_command_orchestrator_activate(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct app_gateway_command_ingress_item *item);
void app_mesh_command_orchestrator_clear_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator);
void app_mesh_command_orchestrator_mark_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator);

int app_mesh_command_orchestrator_gateway_ingress(
    const struct app_gateway_command_ingress_ops *ops,
    const uint8_t *frame,
    size_t frame_len,
    struct app_gateway_command_ingress_item *decoded,
    bool *command_handled);

int app_mesh_command_orchestrator_decide(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_decision *decision,
    bool *state_changed);

int app_mesh_command_orchestrator_safe_boundary(
    struct app_mesh_command_orchestrator *orchestrator,
    bool ds_twr_active,
    app_mesh_command_safe_boundary_fn schedule_command,
    void *ctx);

int app_mesh_command_orchestrator_prepare_flood(
    struct app_mesh_command_orchestrator *orchestrator,
    uint64_t gateway_id,
    uint32_t now_ms,
    uint16_t fallback_seq);

int app_mesh_command_orchestrator_send_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result);

int app_mesh_command_orchestrator_serialize_flood(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result);

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
    bool *duplicate);
void app_mesh_command_orchestrator_anchor_commit(
    struct app_mesh_command_orchestrator *orchestrator,
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    uint32_t now_ms);

int app_mesh_command_orchestrator_anchor_result(
    const struct proto_packet *command,
    uint64_t source_id,
    uint64_t gateway_id,
    bool diagnostic,
    struct mesh_outbound *result);

int app_mesh_command_orchestrator_gateway_deliver(
    const struct proto_packet *pending_command,
    enum command_id pending_command_id,
    const struct proto_packet *result_packet,
    const uint8_t *payload,
    size_t payload_len,
    app_mesh_command_gateway_delivery_fn deliver,
    void *ctx);

#endif
