#ifndef APP_MESH_PERSISTENCE_H
#define APP_MESH_PERSISTENCE_H

#include "gateway_command.h"
#include "gateway_membership.h"
#include "app_mesh_local_delivery.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION 1u
#define APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION 1u
#define APP_MESH_GATEWAY_CLICK_JOURNAL_VERSION 1u
#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAGIC UINT32_C(0x47434A31)
#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAX_PAYLOAD_LEN \
    PACKET_EXT_MAX_PAYLOAD_LEN

enum app_mesh_click_handoff_phase {
    APP_MESH_CLICK_HANDOFF_STAGED = 1,
    APP_MESH_CLICK_HANDOFF_COMMITTED,
};

struct app_mesh_click_handoff_snapshot {
    uint16_t version;
    enum app_mesh_click_handoff_phase phase;
    struct mesh_relay_outbox_snapshot outbox;
    bool valid;
};

struct app_mesh_persistence_health {
    uint32_t total_failures;
    uint16_t consecutive_failures;
    int last_error;
    bool ready;
};

#define APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION 2u
struct app_mesh_discovery_assignment_snapshot {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    bool provisioned;
    bool valid;
};

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

/*
 * The gateway click journal keeps the commit marker separate from the exact
 * payload bytes.  The payload is written first and this compact metadata is
 * written last, so a reset can expose only an orphan payload, never a
 * committed marker whose payload was not durable.
 *
 * The two NVS keys used by this journal are anchor-only in their normal
 * APIs.  They are deliberately overlaid for ROLE_GATEWAY so the journal does
 * not add another live key or grow the gateway's static RAM budget.
 */
struct app_mesh_gateway_click_journal_metadata {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct proto_packet packet;
    uint16_t payload_len;
    uint16_t payload_crc;
    uint32_t received_at_ms;
    uint32_t checksum;
    uint8_t valid;
};

int app_mesh_persistence_init(void);
int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms);
int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms);
int app_mesh_persistence_clear_outbox(void);
/* Returns 1 when a valid deferred outbox copy exists, 0 when absent. */
int app_mesh_persistence_deferred_outbox_present(void);
int app_mesh_persistence_restore_deferred_outbox(struct mesh_relay *relay,
                                                 uint32_t now_ms);
int app_mesh_persistence_save_deferred_outbox(struct mesh_relay *relay,
                                              uint32_t now_ms);
int app_mesh_persistence_clear_deferred_outbox(void);
int app_mesh_persistence_complete_deferred_outbox(struct mesh_relay *relay,
                                                  uint32_t now_ms);
int app_mesh_persistence_clear_deferred_outbox_if_matches(
    const struct proto_packet *packet);
int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_persistence_clear_local_delivery(void);
int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms);
int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms);
int app_mesh_persistence_rollback_click_handoff(void);
int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms);
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
int app_mesh_persistence_rollback_gateway_collection(
    struct gateway_collection_state *collection);
int app_mesh_persistence_clear_gateway_collection(void);
int app_mesh_persistence_save_gateway_eack_custody(
    const struct gateway_collection_eack_custody_snapshot *snapshot);
int app_mesh_persistence_restore_gateway_eack_custody(
    struct gateway_collection_eack_custody_snapshot *snapshot);
void app_mesh_persistence_clear_gateway_eack_custody(void);
int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster);
int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster);
int app_mesh_persistence_clear_gateway_membership(void);
int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot);
int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot);
void app_mesh_persistence_clear_discovery_assignment(void);
/* Returns 1 for the exact pending semantic packet, 0 when the journal is
 * empty, and -EBUSY when another click owns the journal.  Identity includes
 * the message type, flags, endpoints, session/sequence, payload length, and
 * durable payload CRC; relay-local TTL and message age are deliberately
 * excluded because retries may rewrite them.  Persistence/I/O failures are
 * returned unchanged so callers can fail closed. */
int app_mesh_persistence_gateway_click_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_mesh_persistence_save_gateway_click_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms);
/* Returns 1 when a valid packet/payload was restored, 0 when absent. */
int app_mesh_persistence_restore_gateway_click_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms);
int app_mesh_persistence_clear_gateway_click_journal(void);
int app_mesh_persistence_clear_gateway_click_journal_if_matches(
    const struct proto_packet *packet);
void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health);

#if defined(CONFIG_ZTEST)
void app_mesh_persistence_test_reset_faults(void);
void app_mesh_persistence_test_reset_deferred_presence(void);
void app_mesh_persistence_test_set_deferred_busy(bool busy);
void app_mesh_persistence_test_fail_deferred_read(int error, uint8_t count);
void app_mesh_persistence_test_fail_deferred_write(int error, uint8_t count);
void app_mesh_persistence_test_fail_deferred_delete(int error, uint8_t count);
void app_mesh_persistence_test_fail_outbox_write(int error, uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_write(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_write(int error,
                                                                 uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_read(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_read(int error,
                                                               uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_delete(int error,
                                                         uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_delete(int error,
                                                                  uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_delete(int error,
                                                                 uint8_t count);
int app_mesh_persistence_test_delete_gateway_click_payload(void);
int app_mesh_persistence_test_write_gateway_click_payload(const void *payload,
                                                          size_t payload_len);
int app_mesh_persistence_test_write_deferred_outbox_snapshot(
    const void *snapshot,
    size_t snapshot_len);
#endif

#endif
