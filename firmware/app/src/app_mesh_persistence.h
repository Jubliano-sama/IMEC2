#ifndef APP_MESH_PERSISTENCE_H
#define APP_MESH_PERSISTENCE_H

#include "gateway_command.h"
#include "gateway_membership.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION 1u

struct app_mesh_collection_result_snapshot {
    uint16_t version;
    uint64_t local_id;
    uint64_t gateway_id;
    struct proto_packet command;
    struct command_result_id result_id;
    uint32_t collection_epoch_id;
    uint32_t delay_ms;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool force_rediscovery_after_result;
    bool reboot_after_result;
    bool valid;
};

int app_mesh_persistence_init(void);
int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms);
int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms);
void app_mesh_persistence_clear_outbox(void);
int app_mesh_persistence_restore_child_custody(struct mesh_relay *relay,
                                               uint32_t now_ms);
int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms);
void app_mesh_persistence_clear_child_custody(void);
int app_mesh_persistence_save_collection_result(
    const struct app_mesh_collection_result_snapshot *snapshot);
int app_mesh_persistence_restore_collection_result(
    struct app_mesh_collection_result_snapshot *snapshot);
void app_mesh_persistence_clear_collection_result(void);
int app_mesh_persistence_save_gateway_collection(
    const struct gateway_collection_state *collection);
int app_mesh_persistence_restore_gateway_collection(
    struct gateway_collection_state *collection);
void app_mesh_persistence_clear_gateway_collection(void);
int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster);
int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster);
void app_mesh_persistence_clear_gateway_membership(void);

#endif
