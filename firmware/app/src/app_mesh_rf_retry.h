#ifndef APP_MESH_RF_RETRY_H
#define APP_MESH_RF_RETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum app_mesh_rf_retry_operation {
    APP_MESH_RF_RETRY_OPERATION_NONE = 0,
    APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE,
    APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_CONTROL,
    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY,
    APP_MESH_RF_RETRY_OPERATION_RETRANSMIT,
    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK,
    APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY,
    APP_MESH_RF_RETRY_OPERATION_CONTROL_FLOOD,
    APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE,
    APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT,
    APP_MESH_RF_RETRY_OPERATION_COLLECTION_EACK,
};

enum app_mesh_rf_retry_policy {
    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA = 0,
    APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
    APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
};

struct app_mesh_rf_retry_key {
    uint64_t source_id;
    uint64_t destination_id;
    uint32_t session_id;
    uint32_t sequence;
    uint8_t message_type;
    uint8_t operation;
};

struct app_mesh_rf_retry_state {
    struct app_mesh_rf_retry_key key;
    uint16_t retry_round;
    bool active;
};

struct app_mesh_rf_retry_bank {
    struct app_mesh_rf_retry_state *states;
    size_t state_count;
    size_t replacement_cursor;
};

uint32_t app_mesh_rf_retry_next_delay_ms(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    uint32_t attempt_entropy);
void app_mesh_rf_retry_note_success(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key);
void app_mesh_rf_retry_forget(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key);
void app_mesh_rf_retry_reset(struct app_mesh_rf_retry_state *state);
uint32_t app_mesh_rf_retry_bank_next_delay_ms(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    uint32_t attempt_entropy);
void app_mesh_rf_retry_bank_note_success(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key);
void app_mesh_rf_retry_bank_forget(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key);
void app_mesh_rf_retry_bank_reset(struct app_mesh_rf_retry_bank *bank);

#endif
