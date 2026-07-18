#ifndef APP_NODE_COMM_GATEWAY_ROUTE_REFRESH_H
#define APP_NODE_COMM_GATEWAY_ROUTE_REFRESH_H

#include "app_mesh_flood.h"
#include "app_node_comm.h"
#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

struct k_work_delayable;

typedef int (*app_node_comm_route_refresh_build_fn)(
    void *ctx,
    uint32_t sequence,
    uint32_t now_ms,
    struct mesh_gateway_route_adv_snapshot *snapshot,
    struct mesh_outbound *out);
typedef int (*app_node_comm_route_refresh_wake_fn)(void *ctx,
                                                   const char *reason);
typedef int (*app_node_comm_route_refresh_schedule_fn)(
    void *ctx, struct k_work_delayable *work, uint32_t delay_ms);
typedef void (*app_node_comm_route_refresh_event_fn)(
    void *ctx, const struct app_node_comm_route_refresh_event *event);

struct app_node_comm_gateway_route_refresh_config {
    bool gateway_role;
    uint32_t wake_train_ms;
    bool (*allowed)(void *ctx);
    bool (*policy_running)(void *ctx);
    bool (*response_priority_active)(uint32_t now_ms, void *ctx);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*random_u32)(void *ctx);
    void (*sleep_until_ms)(uint32_t due_ms, void *ctx);
    bool (*defer_active)(void *ctx);
    bool (*c5_quiet)(uint32_t sniff_ms, void *ctx);
    int (*send)(const struct mesh_outbound *out, void *ctx);
    app_node_comm_route_refresh_build_fn build;
    app_node_comm_route_refresh_wake_fn send_wake;
    void (*note_sent)(const struct mesh_outbound *out,
                      uint32_t now_ms,
                      void *ctx);
    void (*stop_role_scan)(void *ctx);
    void (*restart_role_scan)(void *ctx);
    void (*clear_response_priority)(void *ctx);
    app_node_comm_route_refresh_schedule_fn schedule;
    app_node_comm_route_refresh_event_fn observe;
    void *ctx;
};

void app_node_comm_gateway_route_refresh_init(
    const struct app_node_comm_gateway_route_refresh_config *config,
    uint32_t sequence_seed);
int app_node_comm_gateway_route_refresh_request(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation);
int app_node_comm_gateway_route_refresh_request_bounded(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation,
    uint32_t timeout_ms);
bool app_node_comm_gateway_route_refresh_pending_wait_ms(
    uint32_t now_ms, uint32_t *wait_ms);
bool app_node_comm_gateway_route_refresh_due(uint32_t now_ms);
uint32_t app_node_comm_gateway_route_refresh_due_ms(void);
bool app_node_comm_gateway_route_refresh_response_priority_due(uint32_t now_ms);
uint32_t app_node_comm_gateway_route_refresh_response_priority_wait_ms(
    uint32_t now_ms);
int app_node_comm_gateway_route_refresh_schedule_now(void);
void app_node_comm_gateway_route_refresh_response_priority_clear(void);
void app_node_comm_gateway_route_refresh_pause(uint32_t now_ms);
void app_node_comm_gateway_route_refresh_resume(uint32_t now_ms);

#endif
