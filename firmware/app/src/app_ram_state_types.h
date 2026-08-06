#ifndef APP_RAM_STATE_TYPES_H
#define APP_RAM_STATE_TYPES_H

#include "discovery_assignment.h"
#include "gateway_command.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

/* RAM-only replacement for the deleted durable host-journal.  These phase
 * values track in-session host-output ownership and are never written to flash. */
#define APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED 1u
#define APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED 2u
#define APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW 3u
#define APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED 4u
#define APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED UINT16_C(0x0001)

#define APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION 8u

struct app_mesh_discovery_assignment_snapshot {
    uint64_t local_id;
    uint64_t gateway_id;
    struct discovery_assignment_table_commitment table_commitment;
    struct discovery_assignment_table_commitment pending_table_commitment;
    /* Last finalized assignment. This remains live while a newer ACK waits. */
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t pending_epoch;
    uint32_t pending_table_command_seq;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint16_t table_packet_seq;
    uint16_t response_spread_ms;
    uint16_t size;
    uint16_t checksum;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint8_t ack_pending;
    uint8_t pending_slot;
    uint8_t pending_slot_count;
    uint8_t pending_valid;
    uint8_t ack_retry_round;
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

#endif
