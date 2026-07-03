#ifndef APP_MESH_TX_HANDOFF_GATE_H
#define APP_MESH_TX_HANDOFF_GATE_H

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_tx_handoff_work {
    APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING = 0,
    APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE = 1,
};

enum app_mesh_tx_handoff_reason {
    APP_MESH_TX_HANDOFF_REASON_NONE = 0,
    APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY = 1,
    APP_MESH_TX_HANDOFF_REASON_RX_CONTROL = 2,
};

struct app_mesh_tx_handoff_state {
    bool queued_gateway_tx_pending;
    bool route_reply_handoff_active;
    bool rx_control_handoff_active;
    enum app_mesh_tx_handoff_work work;
    uint32_t retry_delay_ms;
};

struct app_mesh_tx_handoff_ops {
    int (*schedule_retry)(enum app_mesh_tx_handoff_work work,
                          enum app_mesh_tx_handoff_reason reason,
                          uint32_t delay_ms,
                          void *ctx);
    void *ctx;
};

struct app_mesh_tx_handoff_result {
    bool yield;
    bool keep_queued_tx;
    bool retry_scheduled;
    enum app_mesh_tx_handoff_work work;
    enum app_mesh_tx_handoff_reason reason;
    uint32_t retry_delay_ms;
    int schedule_ret;
};

bool app_mesh_tx_handoff_gate_yield(
    const struct app_mesh_tx_handoff_state *state,
    const struct app_mesh_tx_handoff_ops *ops,
    struct app_mesh_tx_handoff_result *result);

#endif
