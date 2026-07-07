#ifndef APP_MESH_ROUTE_REPLY_ACK_H
#define APP_MESH_ROUTE_REPLY_ACK_H

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_route_reply_ack_attempt_action {
    APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS = 0,
    APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY,
    APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_FAILED,
};

struct app_mesh_route_reply_ack_attempt_state {
    uint8_t attempt;
    uint8_t max_retries;
    int send_ret;
    bool listen_attempted;
    int listen_ret;
};

struct app_mesh_route_reply_ack_attempt_result {
    enum app_mesh_route_reply_ack_attempt_action action;
    bool note_retry;
    int return_ret;
};

struct app_mesh_route_reply_ack_backup_state {
    int primary_ret;
    bool backup_valid;
    uint64_t primary_next_hop_id;
    uint64_t backup_next_hop_id;
};

struct app_mesh_route_reply_ack_backup_result {
    bool try_backup;
    bool note_retry;
    uint64_t backup_next_hop_id;
    int return_ret;
    const char *clear_reason;
};

void app_mesh_route_reply_ack_decide_attempt(
    const struct app_mesh_route_reply_ack_attempt_state *state,
    struct app_mesh_route_reply_ack_attempt_result *result);

void app_mesh_route_reply_ack_decide_backup(
    const struct app_mesh_route_reply_ack_backup_state *state,
    struct app_mesh_route_reply_ack_backup_result *result);

uint32_t app_mesh_route_reply_ack_deadline_after_preemption(
    uint32_t preempted_at_ms,
    uint32_t timeout_ms,
    uint32_t latest_deadline_ms);

#endif
