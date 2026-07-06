#ifndef APP_MESH_COORDINATOR_H
#define APP_MESH_COORDINATOR_H

#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

enum app_mesh_coordinator_state {
    APP_MESH_COORDINATOR_IDLE = 0,
    APP_MESH_COORDINATOR_CLICK,
    APP_MESH_COORDINATOR_SURVEY,
    APP_MESH_COORDINATOR_MESH_RX,
    APP_MESH_COORDINATOR_MESH_TX,
    APP_MESH_COORDINATOR_GATEWAY_RX,
};

struct app_mesh_coordinator_inputs {
    bool click_priority;
    bool survey_pending;
    bool rx_queue_pending;
    bool relay_tx_active;
    bool route_waiting_tx_active;
    bool ch9_ack_wait_active;
    bool report_queue_pending;
    bool gateway_continuous_ch9;
};

struct app_mesh_coordinator_decision {
    enum app_mesh_coordinator_state state;
    bool mesh_work_allowed;
    bool route_wait_allowed;
    bool report_tx_allowed;
    bool uwb_rx_allowed;
    const char *reason;
};

struct app_mesh_paused_delivery_state {
    uint32_t lost_count;
};

struct app_mesh_paused_delivery_store_result {
    bool replaced_existing;
    uint32_t lost_count;
};

struct app_mesh_paused_delivery_attach_result {
    bool tlv_attached;
    bool tlv_updated;
    bool lost_count_pending;
    uint32_t lost_count;
    int ret;
};

void app_mesh_coordinator_decide(
    const struct app_mesh_coordinator_inputs *inputs,
    struct app_mesh_coordinator_decision *decision);
const char *app_mesh_coordinator_state_name(enum app_mesh_coordinator_state state);
void app_mesh_paused_delivery_reset(struct app_mesh_paused_delivery_state *state);
void app_mesh_paused_delivery_note_store(
    struct app_mesh_paused_delivery_state *state,
    bool existing_valid,
    const struct mesh_outbound *existing,
    const struct mesh_outbound *replacement,
    struct app_mesh_paused_delivery_store_result *result);
void app_mesh_paused_delivery_note_drop(
    struct app_mesh_paused_delivery_state *state,
    struct app_mesh_paused_delivery_store_result *result);
int app_mesh_paused_delivery_attach_loss(
    const struct app_mesh_paused_delivery_state *state,
    struct mesh_outbound *out,
    struct app_mesh_paused_delivery_attach_result *result);
void app_mesh_paused_delivery_note_sent(
    struct app_mesh_paused_delivery_state *state,
    const struct mesh_outbound *sent);
uint32_t app_mesh_paused_delivery_lost_count(
    const struct app_mesh_paused_delivery_state *state);

#endif
