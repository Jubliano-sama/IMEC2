#include "app_mesh_persistence.h"
#include "app_click_event_sequence.h"
#include "app_discovery_assignment_policy.h"
#include "app_gateway_collection_receipts.h"
#include "app_gateway_terminal_receipts.h"
#include "app_nvs_storage.h"
#include "discovery_assignment.h"
#include "gateway_collection_journal.h"

#include "app_config.h"
#include "protocol.h"

#include <zephyr/sys/util.h>

bool app_mesh_persistence_gateway_host_journal_supports(
    const struct proto_packet *packet)
{
    if (packet == NULL ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    case MSG_MESH_DATA:
        return (packet->flags & FLAG_DIAGNOSTIC) != 0u;
    default:
        return false;
    }
}

#if (DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY) && \
    defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_persistence, LOG_LEVEL_INF);

#define APP_MESH_NVS_OUTBOX_ID APP_NVS_ID_MESH_OUTBOX
#define APP_MESH_NVS_COLLECTION_RESULT_ID 0x0102u
#define APP_MESH_NVS_CHILD_CUSTODY_ID 0x0103u
#define APP_MESH_NVS_GATEWAY_COLLECTION_ID 0x0104u
#define APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID 0x0105u
#define APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID 0x0106u
#define APP_MESH_NVS_CLICK_HANDOFF_ID APP_NVS_ID_MESH_CLICK_HANDOFF
#define APP_MESH_NVS_LOCAL_DELIVERY_ID 0x0108u
#define APP_MESH_NVS_GATEWAY_EACK_CUSTODY_ID 0x0109u
#define APP_MESH_NVS_DEFERRED_OUTBOX_ID 0x010Du
#define APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID 0x010Eu
#define APP_MESH_NVS_GATEWAY_COMMAND_SEQUENCE_ID 0x010Fu
#define APP_MESH_NVS_ANCHOR_COMMAND_REPLAY_ID 0x01A0u
#define APP_MESH_NVS_SURVEY_GENERATION_ID 0x01A1u
#define APP_MESH_NVS_SURVEY_PAIR_RESULT_BASE_ID 0x01A2u
#define APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID 0x01B0u
#define APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID 0x01B1u
#define APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_BASE_ID 0x01C0u
#define APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_BASE_ID 0x0202u
#define APP_MESH_COLLECTION_RESULT_RECORD_MAGIC UINT32_C(0x41435232)
#define APP_MESH_COLLECTION_RESULT_RECORD_VERSION 1u
#define APP_MESH_NVS_GATEWAY_COLLECTION_BASE_1_ID 0x010Au
#define APP_MESH_NVS_GATEWAY_COLLECTION_CONTROL_0_ID 0x010Bu
#define APP_MESH_NVS_GATEWAY_COLLECTION_CONTROL_1_ID 0x010Cu
#define APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_0_BASE_ID 0x0110u
#define APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_1_BASE_ID 0x0118u
#define APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_0_BASE_ID 0x0120u
#define APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_1_BASE_ID 0x0160u
#define APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID APP_MESH_NVS_COLLECTION_RESULT_ID
#define APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID APP_MESH_NVS_LOCAL_DELIVERY_ID

struct app_mesh_collection_result_record {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint16_t reserved;
    struct app_mesh_collection_result_snapshot snapshot;
};
#define APP_MESH_NVS_SECTOR_SIZE APP_NVS_STORAGE_SECTOR_SIZE
#if DEVICE_ROLE == ROLE_GATEWAY
#ifdef CONFIG_NVS_DATA_CRC
#define APP_MESH_NVS_REQUIRED_SECTOR_COUNT 7u
#else
#define APP_MESH_NVS_REQUIRED_SECTOR_COUNT 6u
#endif
#else
#define APP_MESH_NVS_REQUIRED_SECTOR_COUNT 5u
#endif
#define APP_MESH_NVS_CAPACITY_ALIGNMENT 4u
#define APP_MESH_NVS_ATE_SIZE 8u
#ifdef CONFIG_NVS_DATA_CRC
#define APP_MESH_NVS_DATA_CRC_SIZE 4u
#else
#define APP_MESH_NVS_DATA_CRC_SIZE 0u
#endif
#define APP_MESH_NVS_ENTRY_BYTES(data_size) \
    (ROUND_UP((data_size) + APP_MESH_NVS_DATA_CRC_SIZE, \
              APP_MESH_NVS_CAPACITY_ALIGNMENT) + \
     ROUND_UP(APP_MESH_NVS_ATE_SIZE, APP_MESH_NVS_CAPACITY_ALIGNMENT))
#if DEVICE_ROLE == ROLE_GATEWAY
#define APP_MESH_NVS_GATEWAY_JOURNAL_LIVE_BYTES \
    (2u * APP_MESH_NVS_ENTRY_BYTES( \
              GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE) + \
     2u * APP_MESH_NVS_ENTRY_BYTES( \
              GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE) + \
     2u * GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT * \
              APP_MESH_NVS_ENTRY_BYTES( \
                  GATEWAY_COLLECTION_JOURNAL_ROSTER_RECORD_MAX_SIZE) + \
     GATEWAY_COLLECTION_JOURNAL_BANK_COUNT * \
              GATEWAY_COLLECTION_RESULT_CACHE_SIZE * \
              APP_MESH_NVS_ENTRY_BYTES( \
                  GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE))
#define APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_LIVE_BYTES \
    (APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES * \
     APP_MESH_NVS_ENTRY_BYTES( \
         APP_GATEWAY_COLLECTION_RECEIPT_RECORD_SIZE))
#define APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_LIVE_BYTES \
    (APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY * \
     APP_MESH_NVS_ENTRY_BYTES( \
         APP_GATEWAY_TERMINAL_RECEIPT_RECORD_SIZE))
#define APP_MESH_NVS_SURVEY_PAIR_RESULT_LIVE_BYTES 0u
#define APP_MESH_NVS_ROLE_OUTBOX_LIVE_BYTES 0u
#define APP_MESH_NVS_ROLE_CHILD_CUSTODY_LIVE_BYTES 0u
#define APP_MESH_NVS_ROLE_DEFERRED_OUTBOX_LIVE_BYTES 0u
#define APP_MESH_NVS_ROLE_COLLECTION_RESULT_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct app_mesh_gateway_click_journal_metadata))
#define APP_MESH_NVS_ROLE_CLICK_HANDOFF_LIVE_BYTES 0u
#define APP_MESH_NVS_ROLE_LOCAL_DELIVERY_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN)
#define APP_MESH_NVS_ROLE_CONTROL_LIVE_BYTES \
    (APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct gateway_collection_eack_custody_snapshot)) + \
     APP_MESH_NVS_ENTRY_BYTES(sizeof(struct gateway_membership_snapshot)) + \
     APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct app_mesh_gateway_assignment_epoch_snapshot)) + \
     APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct app_mesh_gateway_command_sequence_snapshot)))
#else
#define APP_MESH_NVS_GATEWAY_JOURNAL_LIVE_BYTES 0u
#define APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_LIVE_BYTES 0u
#define APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_LIVE_BYTES 0u
#define APP_MESH_NVS_SURVEY_PAIR_RESULT_LIVE_BYTES \
    (APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS * \
     APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct app_mesh_local_delivery_snapshot)))
#define APP_MESH_NVS_ROLE_COLLECTION_RESULT_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct app_mesh_collection_result_record))
#define APP_MESH_NVS_ROLE_CLICK_HANDOFF_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct app_mesh_click_handoff_snapshot))
#define APP_MESH_NVS_ROLE_OUTBOX_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES(sizeof(struct mesh_relay_outbox_snapshot))
#define APP_MESH_NVS_ROLE_CHILD_CUSTODY_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct mesh_relay_child_custody_snapshot))
#define APP_MESH_NVS_ROLE_DEFERRED_OUTBOX_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES(sizeof(struct mesh_relay_outbox_snapshot))
#define APP_MESH_NVS_ROLE_LOCAL_DELIVERY_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct app_mesh_local_delivery_snapshot))
#define APP_MESH_NVS_ROLE_CONTROL_LIVE_BYTES \
    (APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct app_mesh_discovery_assignment_snapshot)) + \
     APP_MESH_NVS_ENTRY_BYTES( \
         sizeof(struct app_mesh_anchor_command_replay_snapshot)))
#endif
#define APP_MESH_NVS_SURVEY_GENERATION_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES( \
        sizeof(struct app_mesh_survey_generation_snapshot))
#define APP_MESH_NVS_ROUTE_STATE_LIVE_BYTES \
    APP_MESH_NVS_ENTRY_BYTES(APP_NVS_ROUTE_STATE_RECORD_SIZE)
#define APP_MESH_NVS_OTHER_LIVE_BYTES \
    (APP_MESH_NVS_ROLE_OUTBOX_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_COLLECTION_RESULT_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_CHILD_CUSTODY_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_CLICK_HANDOFF_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_DEFERRED_OUTBOX_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_LOCAL_DELIVERY_LIVE_BYTES + \
     APP_MESH_NVS_ROLE_CONTROL_LIVE_BYTES + \
     APP_MESH_NVS_SURVEY_GENERATION_LIVE_BYTES + \
     APP_MESH_NVS_ROUTE_STATE_LIVE_BYTES + \
     APP_MESH_NVS_SURVEY_PAIR_RESULT_LIVE_BYTES)
#define APP_MESH_NVS_ANCHOR_RANGE_JOURNAL_LIVE_BYTES \
    (APP_MESH_NVS_ENTRY_BYTES(ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN) + \
     RANGE_REPORT_MAX_PACKET_FRAGMENTS * \
         APP_MESH_NVS_ENTRY_BYTES( \
             ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN))

struct app_mesh_discovery_assignment_snapshot_v2 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
};
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot_v2) == 40u,
             "discovery assignment v2 migration layout changed");

/*
 * Schema 7 was the final anchor-assignment record that treated the 32-bit
 * FNV table fingerprint as authority.  Keep it byte-exact only so a valid
 * installed record can be recognized and retired; its proof is never
 * promoted into the SHA-256 schema.
 */
struct app_mesh_discovery_assignment_snapshot_v7 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint16_t table_packet_seq;
    uint16_t response_spread_ms;
    uint64_t local_id;
    uint64_t gateway_id;
    uint32_t pending_epoch;
    uint32_t pending_table_command_seq;
    uint32_t pending_table_fingerprint;
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
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint32_t magic;
    uint16_t size;
    uint16_t checksum;
};
BUILD_ASSERT(
    sizeof(struct app_mesh_discovery_assignment_snapshot_v7) == 128u &&
    offsetof(struct app_mesh_discovery_assignment_snapshot_v7,
             table_packet_seq) == 12u &&
    offsetof(struct app_mesh_discovery_assignment_snapshot_v7,
             pending_epoch) == 32u &&
    offsetof(struct app_mesh_discovery_assignment_snapshot_v7,
             retired_epochs) == 56u &&
    offsetof(struct app_mesh_discovery_assignment_snapshot_v7, magic) == 120u,
    "discovery assignment v7 migration layout changed");

struct gateway_membership_snapshot_v1 {
    uint8_t version;
    bool valid;
    uint16_t membership_epoch;
    uint16_t node_count;
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
};
BUILD_ASSERT(sizeof(struct gateway_membership_snapshot_v1) == 408u,
             "gateway membership v1 migration layout changed");

/*
 * Schema 2 used a dense roster and added the durable assignment proof.  Keep
 * this byte-exact definition so installed gateways can migrate without
 * discarding their roster or weakening late-ACK admission after an update.
 */
struct gateway_membership_snapshot_v2 {
    uint8_t version;
    uint8_t valid;
    uint16_t membership_epoch;
    uint16_t node_count;
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint32_t assignment_epoch;
    uint32_t assignment_table_seq;
    uint32_t assignment_table_fingerprint;
    uint32_t magic;
    uint16_t checksum;
    uint8_t assignment_proof_valid;
    uint8_t reserved;
};
BUILD_ASSERT(sizeof(struct gateway_membership_snapshot_v2) == 432u &&
             offsetof(struct gateway_membership_snapshot_v2, node_ids) == 8u &&
             offsetof(struct gateway_membership_snapshot_v2,
                      assignment_epoch) == 408u &&
             offsetof(struct gateway_membership_snapshot_v2, checksum) == 424u,
             "gateway membership v2 migration layout changed");
#define GATEWAY_MEMBERSHIP_SNAPSHOT_V2_MAGIC UINT32_C(0x474D5332)

/*
 * Schema 3 added sparse slots and durable host-publication debt while still
 * authorizing assignment membership with a 32-bit FNV fingerprint.  The
 * roster may be migrated, but the proof and publication debt must be dropped
 * because neither can be rebound to the new collision-resistant commitment.
 */
struct gateway_membership_snapshot_v3 {
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    struct gateway_membership_publication publication;
    uint32_t assignment_epoch;
    uint32_t assignment_table_seq;
    uint32_t assignment_table_fingerprint;
    uint32_t magic;
    uint16_t membership_epoch;
    uint16_t checksum;
    uint8_t version;
    uint8_t node_count;
    uint8_t slot_span;
    uint8_t valid;
    uint8_t assignment_proof_valid;
    uint8_t reserved[3];
};
#define GATEWAY_MEMBERSHIP_SNAPSHOT_V3_MAGIC UINT32_C(0x474D5333)

static int gateway_membership_restore_v1(
    const struct gateway_membership_snapshot_v1 *snapshot,
    struct gateway_membership_roster *roster)
{
    int ret;

    if (snapshot == NULL || roster == NULL ||
        snapshot->version != 1u || !snapshot->valid ||
        snapshot->membership_epoch == 0u ||
        snapshot->node_count == 0u ||
        snapshot->node_count > GATEWAY_MEMBERSHIP_MAX_NODES) {
        return -EINVAL;
    }
    ret = gateway_membership_set_roster_preserve_order(
        roster,
        snapshot->membership_epoch,
        snapshot->node_ids,
        snapshot->node_count);
    return ret == PROTO_OK ? 0 : -EINVAL;
}

static uint16_t gateway_membership_snapshot_v2_checksum(
    const struct gateway_membership_snapshot_v2 *snapshot)
{
    struct gateway_membership_snapshot_v2 copy;

    if (snapshot == NULL) {
        return 0u;
    }
    copy = *snapshot;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static int gateway_membership_restore_v2(
    const struct gateway_membership_snapshot_v2 *snapshot,
    struct gateway_membership_roster *roster)
{
    int ret;

    if (snapshot == NULL || roster == NULL) {
        return -EINVAL;
    }
    if (snapshot->version != 2u ||
        snapshot->valid != 1u ||
        snapshot->membership_epoch == 0u ||
        snapshot->node_count == 0u ||
        snapshot->node_count > GATEWAY_MEMBERSHIP_MAX_NODES ||
        snapshot->magic != GATEWAY_MEMBERSHIP_SNAPSHOT_V2_MAGIC ||
        snapshot->reserved != 0u ||
        snapshot->assignment_proof_valid > 1u ||
        snapshot->checksum !=
            gateway_membership_snapshot_v2_checksum(snapshot)) {
        return -EINVAL;
    }
    if ((snapshot->assignment_proof_valid != 0u &&
         (snapshot->assignment_epoch == 0u ||
          snapshot->assignment_table_seq == 0u ||
          snapshot->assignment_table_fingerprint == 0u)) ||
        (snapshot->assignment_proof_valid == 0u &&
         (snapshot->assignment_epoch != 0u ||
          snapshot->assignment_table_seq != 0u ||
          snapshot->assignment_table_fingerprint != 0u))) {
        return -EINVAL;
    }
    for (size_t i = snapshot->node_count;
         i < GATEWAY_MEMBERSHIP_MAX_NODES;
         i++) {
        if (snapshot->node_ids[i] != 0u) {
            return -EINVAL;
        }
    }

    ret = gateway_membership_set_roster_preserve_order(
        roster,
        snapshot->membership_epoch,
        snapshot->node_ids,
        snapshot->node_count);
    return ret == PROTO_OK ? 0 : -EINVAL;
}

BUILD_ASSERT(sizeof(struct gateway_membership_snapshot_v3) == 904u &&
             offsetof(struct gateway_membership_snapshot_v3,
                      assignment_epoch) == 872u &&
             offsetof(struct gateway_membership_snapshot_v3, checksum) == 890u,
             "gateway membership v3 migration layout changed");

union gateway_membership_stored_snapshot {
    struct gateway_membership_snapshot current;
    struct gateway_membership_snapshot_v3 legacy_v3;
    uint8_t raw[sizeof(struct gateway_membership_snapshot_v3)];
};
BUILD_ASSERT(
    sizeof(struct gateway_membership_snapshot) <=
        sizeof(struct gateway_membership_snapshot_v3),
    "gateway membership read buffer must cover the largest installed schema");

static uint16_t gateway_membership_snapshot_v3_checksum(
    const struct gateway_membership_snapshot_v3 *snapshot)
{
    static const uint8_t zero_checksum[sizeof(snapshot->checksum)];
    const uint8_t *bytes = (const uint8_t *)snapshot;
    const size_t checksum_offset =
        offsetof(struct gateway_membership_snapshot_v3, checksum);
    uint16_t crc;

    if (snapshot == NULL) {
        return 0u;
    }
    crc = proto_crc16_ccitt_false_update(UINT16_C(0xFFFF),
                                         bytes,
                                         checksum_offset);
    crc = proto_crc16_ccitt_false_update(crc,
                                         zero_checksum,
                                         sizeof(zero_checksum));
    return proto_crc16_ccitt_false_update(
        crc,
        bytes + checksum_offset + sizeof(snapshot->checksum),
        sizeof(*snapshot) - checksum_offset - sizeof(snapshot->checksum));
}

static int gateway_membership_restore_v3(
    const struct gateway_membership_snapshot_v3 *snapshot,
    struct gateway_membership_roster *roster)
{
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint8_t slots[GATEWAY_MEMBERSHIP_MAX_NODES];
    size_t node_count = 0u;
    int ret;

    if (snapshot == NULL || roster == NULL ||
        snapshot->version != 3u ||
        snapshot->magic != GATEWAY_MEMBERSHIP_SNAPSHOT_V3_MAGIC ||
        snapshot->valid != 1u ||
        snapshot->assignment_proof_valid > 1u ||
        snapshot->reserved[0] != 0u ||
        snapshot->reserved[1] != 0u ||
        snapshot->reserved[2] != 0u ||
        snapshot->membership_epoch == 0u ||
        snapshot->node_count == 0u ||
        snapshot->node_count > GATEWAY_MEMBERSHIP_MAX_NODES ||
        snapshot->slot_span == 0u ||
        snapshot->slot_span > GATEWAY_MEMBERSHIP_MAX_NODES ||
        snapshot->node_count > snapshot->slot_span ||
        snapshot->checksum !=
            gateway_membership_snapshot_v3_checksum(snapshot)) {
        return -EINVAL;
    }
    if ((snapshot->assignment_proof_valid != 0u &&
         (snapshot->assignment_epoch == 0u ||
          snapshot->assignment_table_seq == 0u ||
          snapshot->assignment_table_fingerprint == 0u)) ||
        (snapshot->assignment_proof_valid == 0u &&
         (snapshot->assignment_epoch != 0u ||
          snapshot->assignment_table_seq != 0u ||
          snapshot->assignment_table_fingerprint != 0u))) {
        return -EINVAL;
    }
    if (snapshot->publication.publish_pending > 1u ||
        (snapshot->publication.publish_pending != 0u &&
         snapshot->assignment_proof_valid == 0u)) {
        return -EINVAL;
    }

    for (size_t slot = 0u; slot < GATEWAY_MEMBERSHIP_MAX_NODES; slot++) {
        uint64_t node_id = snapshot->node_ids[slot];

        if (slot >= snapshot->slot_span) {
            if (node_id != 0u) {
                return -EINVAL;
            }
            continue;
        }
        if (node_id == 0u) {
            continue;
        }
        node_ids[node_count] = node_id;
        slots[node_count] = (uint8_t)slot;
        node_count++;
    }
    if (node_count != snapshot->node_count ||
        snapshot->node_ids[snapshot->slot_span - 1u] == 0u) {
        return -EINVAL;
    }

    ret = gateway_membership_set_roster_explicit_slots(
        roster,
        snapshot->membership_epoch,
        node_ids,
        slots,
        node_count);
    return ret == PROTO_OK ? 0 : -EINVAL;
}

struct app_mesh_discovery_assignment_snapshot_v3 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
};
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot_v3) == 104u,
             "discovery assignment v3 migration size changed");
BUILD_ASSERT(offsetof(struct app_mesh_discovery_assignment_snapshot_v3,
                      retired_epochs) == 40u,
    "discovery assignment v3 migration offsets changed");

struct app_mesh_discovery_assignment_snapshot_v4 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
};
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot_v4) == 104u,
             "discovery assignment v4 migration size changed");
BUILD_ASSERT(offsetof(struct app_mesh_discovery_assignment_snapshot_v4,
                      ordered_epoch_valid) == 38u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v4,
                      retired_epochs) == 40u,
             "discovery assignment v4 migration offsets changed");

struct app_mesh_discovery_assignment_snapshot_v5 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint32_t magic;
    uint16_t size;
    uint16_t checksum;
};
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot_v5) == 112u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v5,
                      retired_epochs) == 40u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v5,
                      magic) == 104u,
             "discovery assignment v5 migration layout changed");

struct app_mesh_discovery_assignment_snapshot_v6 {
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t table_fingerprint;
    uint16_t table_packet_seq;
    uint16_t response_spread_ms;
    uint64_t local_id;
    uint64_t gateway_id;
    uint8_t version;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t valid;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint8_t ack_pending;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint32_t magic;
    uint16_t size;
    uint16_t checksum;
};
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot_v6) == 112u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v6,
                      table_packet_seq) == 12u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v6,
                      ack_pending) == 39u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot_v6,
                      magic) == 104u,
             "discovery assignment v6 migration layout changed");
BUILD_ASSERT(sizeof(struct app_mesh_discovery_assignment_snapshot) == 184u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      table_commitment) == 16u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      pending_table_commitment) == 48u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      epoch) == 80u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      retired_epochs) == 96u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      magic) == 160u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      checksum) == 170u &&
             offsetof(struct app_mesh_discovery_assignment_snapshot,
                      ack_retry_round) == 183u,
             "discovery assignment v8 storage layout changed");

#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V5_MAGIC UINT32_C(0x44415335)
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V6_MAGIC UINT32_C(0x44415336)
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V7_MAGIC UINT32_C(0x44415337)
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V5_CHECKSUM_BYTES 105u
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V6_CHECKSUM_BYTES 110u
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_V7_CHECKSUM_BYTES 126u
#define DISCOVERY_ASSIGNMENT_SNAPSHOT_CHECKSUM_BYTES 182u

static bool discovery_assignment_commitment_is_zero(
    const struct discovery_assignment_table_commitment *commitment)
{
    uint8_t combined = 0u;

    if (commitment == NULL) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(commitment->bytes); i++) {
        combined |= commitment->bytes[i];
    }
    return combined == 0u;
}

static bool discovery_assignment_snapshot_history_valid(
    const struct app_mesh_discovery_assignment_snapshot *snapshot,
    bool require_ordered_epoch)
{
    uint32_t freshness_epoch;

    if (snapshot == NULL) {
        return false;
    }
    freshness_epoch = snapshot->pending_valid != 0u ?
                      snapshot->pending_epoch : snapshot->epoch;
    if (!app_discovery_assignment_retired_epochs_valid(
            freshness_epoch,
            snapshot->retired_epochs,
            snapshot->retired_epoch_count,
            true)) {
        return false;
    }
    if (!require_ordered_epoch) {
        return true;
    }
    for (size_t i = 0u; i < snapshot->retired_epoch_count; i++) {
        if (!discovery_assignment_epoch_strictly_newer(
                freshness_epoch, snapshot->retired_epochs[i]) ||
            (i != 0u &&
             !discovery_assignment_epoch_strictly_newer(
                 snapshot->retired_epochs[i - 1u],
                 snapshot->retired_epochs[i]))) {
            return false;
        }
    }
    return true;
}

static uint16_t discovery_assignment_snapshot_checksum(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    uint8_t encoded[DISCOVERY_ASSIGNMENT_SNAPSHOT_CHECKSUM_BYTES];
    size_t offset = 0u;

    if (snapshot == NULL) {
        return 0u;
    }
    proto_put_u32_le(&encoded[offset], snapshot->magic);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->size);
    offset += sizeof(uint16_t);
    encoded[offset++] = snapshot->version;
    proto_put_u32_le(&encoded[offset], snapshot->epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_command_seq);
    offset += sizeof(uint32_t);
    memcpy(&encoded[offset],
           snapshot->table_commitment.bytes,
           sizeof(snapshot->table_commitment.bytes));
    offset += sizeof(snapshot->table_commitment.bytes);
    proto_put_u16_le(&encoded[offset], snapshot->table_packet_seq);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&encoded[offset], snapshot->response_spread_ms);
    offset += sizeof(uint16_t);
    proto_put_u64_le(&encoded[offset], snapshot->local_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&encoded[offset], snapshot->gateway_id);
    offset += sizeof(uint64_t);
    proto_put_u32_le(&encoded[offset], snapshot->pending_epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->pending_table_command_seq);
    offset += sizeof(uint32_t);
    memcpy(&encoded[offset],
           snapshot->pending_table_commitment.bytes,
           sizeof(snapshot->pending_table_commitment.bytes));
    offset += sizeof(snapshot->pending_table_commitment.bytes);
    encoded[offset++] = snapshot->slot;
    encoded[offset++] = snapshot->slot_count;
    encoded[offset++] = snapshot->provisioned;
    encoded[offset++] = snapshot->valid;
    encoded[offset++] = snapshot->retired_epoch_count;
    encoded[offset++] = snapshot->ordered_epoch_valid;
    encoded[offset++] = snapshot->ack_pending;
    encoded[offset++] = snapshot->pending_slot;
    encoded[offset++] = snapshot->pending_slot_count;
    encoded[offset++] = snapshot->pending_valid;
    encoded[offset++] = snapshot->ack_retry_round;
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        proto_put_u32_le(&encoded[offset], snapshot->retired_epochs[i]);
        offset += sizeof(uint32_t);
    }
    if (offset != sizeof(encoded)) {
        return 0u;
    }
    return proto_crc16_ccitt_false(encoded, sizeof(encoded));
}

static bool discovery_assignment_snapshot_logical_valid(
    const struct app_mesh_discovery_assignment_snapshot *snapshot,
    bool require_ordered_epoch)
{
    bool finalized_identity_present;
    bool pending_identity_present;

    if (snapshot == NULL) {
        return false;
    }
    finalized_identity_present =
        snapshot->epoch != 0u &&
        snapshot->table_command_seq != 0u;
    pending_identity_present =
        snapshot->pending_epoch != 0u &&
        snapshot->pending_table_command_seq != 0u;

    return snapshot != NULL &&
           snapshot->valid == 1u &&
           snapshot->provisioned <= 1u &&
           snapshot->ack_pending <= 1u &&
           snapshot->pending_valid <= 1u &&
           snapshot->ordered_epoch_valid <= 1u &&
           (!require_ordered_epoch ||
            snapshot->ordered_epoch_valid == 1u) &&
           snapshot->local_id != 0u &&
           snapshot->gateway_id != 0u &&
           (finalized_identity_present ||
            snapshot->pending_valid != 0u) &&
           ((snapshot->epoch == 0u &&
             snapshot->table_command_seq == 0u &&
             discovery_assignment_commitment_is_zero(
                 &snapshot->table_commitment)) ||
            finalized_identity_present) &&
           ((snapshot->provisioned != 0u &&
             finalized_identity_present &&
             snapshot->slot_count != 0u &&
             snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
             snapshot->slot < snapshot->slot_count) ||
            (snapshot->provisioned == 0u &&
             ((!finalized_identity_present &&
               snapshot->slot == 0u &&
               snapshot->slot_count == 0u) ||
              (finalized_identity_present &&
               snapshot->slot == 0u &&
               snapshot->slot_count != 0u &&
               snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT)))) &&
           ((snapshot->pending_valid != 0u &&
             pending_identity_present &&
             snapshot->pending_slot_count != 0u &&
             snapshot->pending_slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
             (!finalized_identity_present ||
              discovery_assignment_epoch_strictly_newer(
                  snapshot->pending_epoch, snapshot->epoch))) ||
            (snapshot->pending_valid == 0u &&
             !pending_identity_present &&
             snapshot->pending_epoch == 0u &&
             snapshot->pending_table_command_seq == 0u &&
             discovery_assignment_commitment_is_zero(
                 &snapshot->pending_table_commitment) &&
             snapshot->pending_slot == 0u &&
             snapshot->pending_slot_count == 0u)) &&
           (snapshot->ack_pending == 0u ||
            (snapshot->pending_valid != 0u &&
             snapshot->pending_slot < snapshot->pending_slot_count &&
             snapshot->response_spread_ms >=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS &&
             snapshot->response_spread_ms <=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS &&
             snapshot->table_packet_seq != 0u)) &&
           (snapshot->ack_pending != 0u ||
            (snapshot->response_spread_ms == 0u &&
             snapshot->table_packet_seq == 0u &&
             snapshot->ack_retry_round == 0u &&
             (snapshot->pending_valid != 0u ||
              (snapshot->pending_slot == 0u &&
               snapshot->pending_slot_count == 0u)))) &&
           (snapshot->pending_valid == 0u ||
            snapshot->ack_pending != 0u ||
            snapshot->pending_slot == 0u) &&
           discovery_assignment_snapshot_history_valid(
               snapshot, require_ordered_epoch);
}

static void discovery_assignment_snapshot_finalize(
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    snapshot->magic = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_MAGIC;
    snapshot->version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION;
    snapshot->size = sizeof(*snapshot);
    snapshot->checksum = 0u;
    snapshot->checksum =
        discovery_assignment_snapshot_checksum(snapshot);
}

static void discovery_assignment_snapshot_copy_logical(
    struct app_mesh_discovery_assignment_snapshot *destination,
    const struct app_mesh_discovery_assignment_snapshot *source)
{
    memset(destination, 0, sizeof(*destination));
    destination->epoch = source->epoch;
    destination->table_command_seq = source->table_command_seq;
    destination->table_commitment = source->table_commitment;
    destination->table_packet_seq = source->table_packet_seq;
    destination->response_spread_ms = source->response_spread_ms;
    destination->local_id = source->local_id;
    destination->gateway_id = source->gateway_id;
    destination->pending_epoch = source->pending_epoch;
    destination->pending_table_command_seq =
        source->pending_table_command_seq;
    destination->pending_table_commitment =
        source->pending_table_commitment;
    destination->slot = source->slot;
    destination->slot_count = source->slot_count;
    destination->provisioned = source->provisioned;
    destination->valid = source->valid;
    destination->retired_epoch_count = source->retired_epoch_count;
    destination->ordered_epoch_valid = source->ordered_epoch_valid;
    destination->ack_pending = source->ack_pending;
    destination->pending_slot = source->pending_slot;
    destination->pending_slot_count = source->pending_slot_count;
    destination->pending_valid = source->pending_valid;
    destination->ack_retry_round = source->ack_retry_round;
    memcpy(destination->retired_epochs,
           source->retired_epochs,
           sizeof(destination->retired_epochs));
}

static bool discovery_assignment_snapshot_import_legacy(
    struct app_mesh_discovery_assignment_snapshot *snapshot,
    uint32_t epoch,
    uint32_t table_command_seq,
    uint32_t table_fingerprint,
    uint64_t local_id,
    uint64_t gateway_id,
    uint8_t slot,
    uint8_t slot_count,
    uint8_t provisioned,
    uint8_t valid,
    const uint32_t *retired_epochs,
    uint8_t retired_epoch_count)
{
    if (snapshot == NULL ||
        epoch == 0u ||
        table_command_seq == 0u ||
        table_fingerprint == 0u ||
        local_id == 0u ||
        gateway_id == 0u ||
        provisioned > 1u ||
        valid != 1u ||
        slot_count == 0u ||
        slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        (provisioned != 0u && slot >= slot_count) ||
        retired_epoch_count > DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP ||
        (retired_epoch_count != 0u && retired_epochs == NULL) ||
        !app_discovery_assignment_retired_epochs_valid(
            epoch,
            retired_epochs,
            retired_epoch_count,
            retired_epochs != NULL)) {
        return false;
    }
    /*
     * A 32-bit fingerprint cannot be upgraded into the schema-2 TABLE
     * commitment.  Validation here distinguishes a recognized old record
     * from random bytes; the caller then retires it and requires enumeration.
     */
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

static bool discovery_assignment_snapshot_v8_valid(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->magic ==
               APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_MAGIC &&
           snapshot->version ==
               APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           discovery_assignment_snapshot_logical_valid(snapshot, true) &&
           snapshot->checksum ==
               discovery_assignment_snapshot_checksum(snapshot);
}

static uint16_t discovery_assignment_snapshot_v7_checksum(
    const struct app_mesh_discovery_assignment_snapshot_v7 *snapshot)
{
    uint8_t encoded[DISCOVERY_ASSIGNMENT_SNAPSHOT_V7_CHECKSUM_BYTES];
    size_t offset = 0u;

    if (snapshot == NULL) {
        return 0u;
    }
    proto_put_u32_le(&encoded[offset], snapshot->magic);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->size);
    offset += sizeof(uint16_t);
    encoded[offset++] = snapshot->version;
    proto_put_u32_le(&encoded[offset], snapshot->epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_command_seq);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_fingerprint);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->table_packet_seq);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&encoded[offset], snapshot->response_spread_ms);
    offset += sizeof(uint16_t);
    proto_put_u64_le(&encoded[offset], snapshot->local_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&encoded[offset], snapshot->gateway_id);
    offset += sizeof(uint64_t);
    proto_put_u32_le(&encoded[offset], snapshot->pending_epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset],
                     snapshot->pending_table_command_seq);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset],
                     snapshot->pending_table_fingerprint);
    offset += sizeof(uint32_t);
    encoded[offset++] = snapshot->slot;
    encoded[offset++] = snapshot->slot_count;
    encoded[offset++] = snapshot->provisioned;
    encoded[offset++] = snapshot->valid;
    encoded[offset++] = snapshot->retired_epoch_count;
    encoded[offset++] = snapshot->ordered_epoch_valid;
    encoded[offset++] = snapshot->ack_pending;
    encoded[offset++] = snapshot->pending_slot;
    encoded[offset++] = snapshot->pending_slot_count;
    encoded[offset++] = snapshot->pending_valid;
    encoded[offset++] = snapshot->ack_retry_round;
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        proto_put_u32_le(&encoded[offset], snapshot->retired_epochs[i]);
        offset += sizeof(uint32_t);
    }
    if (offset != sizeof(encoded)) {
        return 0u;
    }
    return proto_crc16_ccitt_false(encoded, sizeof(encoded));
}

static bool discovery_assignment_snapshot_v7_valid(
    const struct app_mesh_discovery_assignment_snapshot_v7 *snapshot)
{
    bool finalized_identity_present;
    bool pending_identity_present;
    uint32_t freshness_epoch;

    if (snapshot == NULL) {
        return false;
    }
    finalized_identity_present =
        snapshot->epoch != 0u &&
        snapshot->table_command_seq != 0u &&
        snapshot->table_fingerprint != 0u;
    pending_identity_present =
        snapshot->pending_epoch != 0u &&
        snapshot->pending_table_command_seq != 0u &&
        snapshot->pending_table_fingerprint != 0u;
    freshness_epoch = snapshot->pending_valid != 0u ?
                      snapshot->pending_epoch : snapshot->epoch;

    return snapshot->magic == DISCOVERY_ASSIGNMENT_SNAPSHOT_V7_MAGIC &&
           snapshot->version == 7u &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->valid == 1u &&
           snapshot->provisioned <= 1u &&
           snapshot->ack_pending <= 1u &&
           snapshot->pending_valid <= 1u &&
           snapshot->ordered_epoch_valid == 1u &&
           snapshot->local_id != 0u &&
           snapshot->gateway_id != 0u &&
           (finalized_identity_present || snapshot->pending_valid != 0u) &&
           ((snapshot->epoch == 0u &&
             snapshot->table_command_seq == 0u &&
             snapshot->table_fingerprint == 0u) ||
            finalized_identity_present) &&
           ((snapshot->provisioned != 0u &&
             finalized_identity_present &&
             snapshot->slot_count != 0u &&
             snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
             snapshot->slot < snapshot->slot_count) ||
            (snapshot->provisioned == 0u &&
             ((!finalized_identity_present &&
               snapshot->slot == 0u &&
               snapshot->slot_count == 0u) ||
              (finalized_identity_present &&
               snapshot->slot == 0u &&
               snapshot->slot_count != 0u &&
               snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT)))) &&
           ((snapshot->pending_valid != 0u &&
             pending_identity_present &&
             snapshot->pending_slot_count != 0u &&
             snapshot->pending_slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
             (!finalized_identity_present ||
              discovery_assignment_epoch_strictly_newer(
                  snapshot->pending_epoch, snapshot->epoch))) ||
            (snapshot->pending_valid == 0u &&
             snapshot->pending_epoch == 0u &&
             snapshot->pending_table_command_seq == 0u &&
             snapshot->pending_table_fingerprint == 0u &&
             snapshot->pending_slot == 0u &&
             snapshot->pending_slot_count == 0u)) &&
           (snapshot->ack_pending == 0u ||
            (snapshot->pending_valid != 0u &&
             snapshot->pending_slot < snapshot->pending_slot_count &&
             snapshot->response_spread_ms >=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS &&
             snapshot->response_spread_ms <=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS &&
             snapshot->table_packet_seq != 0u)) &&
           (snapshot->ack_pending != 0u ||
            (snapshot->response_spread_ms == 0u &&
             snapshot->table_packet_seq == 0u &&
             snapshot->ack_retry_round == 0u &&
             (snapshot->pending_valid != 0u ||
              (snapshot->pending_slot == 0u &&
               snapshot->pending_slot_count == 0u)))) &&
           (snapshot->pending_valid == 0u ||
            snapshot->ack_pending != 0u ||
            snapshot->pending_slot == 0u) &&
           app_discovery_assignment_retired_epochs_valid(
               freshness_epoch,
               snapshot->retired_epochs,
               snapshot->retired_epoch_count,
               true) &&
           snapshot->checksum ==
               discovery_assignment_snapshot_v7_checksum(snapshot);
}

static uint16_t discovery_assignment_snapshot_v6_checksum(
    const struct app_mesh_discovery_assignment_snapshot_v6 *snapshot)
{
    uint8_t encoded[DISCOVERY_ASSIGNMENT_SNAPSHOT_V6_CHECKSUM_BYTES];
    size_t offset = 0u;

    if (snapshot == NULL) {
        return 0u;
    }
    proto_put_u32_le(&encoded[offset], snapshot->magic);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->size);
    offset += sizeof(uint16_t);
    encoded[offset++] = snapshot->version;
    proto_put_u32_le(&encoded[offset], snapshot->epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_command_seq);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_fingerprint);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->table_packet_seq);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&encoded[offset], snapshot->response_spread_ms);
    offset += sizeof(uint16_t);
    proto_put_u64_le(&encoded[offset], snapshot->local_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&encoded[offset], snapshot->gateway_id);
    offset += sizeof(uint64_t);
    encoded[offset++] = snapshot->slot;
    encoded[offset++] = snapshot->slot_count;
    encoded[offset++] = snapshot->provisioned;
    encoded[offset++] = snapshot->valid;
    encoded[offset++] = snapshot->retired_epoch_count;
    encoded[offset++] = snapshot->ordered_epoch_valid;
    encoded[offset++] = snapshot->ack_pending;
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        proto_put_u32_le(&encoded[offset], snapshot->retired_epochs[i]);
        offset += sizeof(uint32_t);
    }
    if (offset != sizeof(encoded)) {
        return 0u;
    }
    return proto_crc16_ccitt_false(encoded, sizeof(encoded));
}

static bool discovery_assignment_snapshot_v6_valid(
    const struct app_mesh_discovery_assignment_snapshot_v6 *snapshot)
{
    return snapshot != NULL &&
           snapshot->magic == DISCOVERY_ASSIGNMENT_SNAPSHOT_V6_MAGIC &&
           snapshot->version == 6u &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->valid == 1u &&
           snapshot->provisioned <= 1u &&
           snapshot->ack_pending <= 1u &&
           !(snapshot->provisioned != 0u &&
             snapshot->ack_pending != 0u) &&
           snapshot->ordered_epoch_valid == 1u &&
           snapshot->epoch != 0u &&
           snapshot->table_command_seq != 0u &&
           snapshot->table_fingerprint != 0u &&
           snapshot->local_id != 0u &&
           snapshot->gateway_id != 0u &&
           snapshot->slot_count != 0u &&
           snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
           snapshot->slot < snapshot->slot_count &&
           (snapshot->provisioned != 0u ||
            snapshot->ack_pending != 0u ||
            snapshot->slot == 0u) &&
           (snapshot->ack_pending == 0u ||
            (snapshot->response_spread_ms >=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS &&
             snapshot->response_spread_ms <=
                 DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS &&
             snapshot->table_packet_seq != 0u)) &&
           (snapshot->ack_pending != 0u ||
            (snapshot->response_spread_ms == 0u &&
             snapshot->table_packet_seq == 0u)) &&
           app_discovery_assignment_retired_epochs_valid(
               snapshot->epoch,
               snapshot->retired_epochs,
               snapshot->retired_epoch_count,
               true) &&
           snapshot->checksum ==
               discovery_assignment_snapshot_v6_checksum(snapshot);
}

static bool discovery_assignment_snapshot_import_v6(
    struct app_mesh_discovery_assignment_snapshot *snapshot,
    const struct app_mesh_discovery_assignment_snapshot_v6 *legacy)
{
    if (snapshot == NULL || !discovery_assignment_snapshot_v6_valid(legacy)) {
        return false;
    }
    /*
     * The old ACK envelope is bound only by a 32-bit fingerprint.  Retire it
     * instead of synthesizing a stronger proof and require a new TABLE.
     */
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

static uint16_t discovery_assignment_snapshot_v5_checksum(
    const struct app_mesh_discovery_assignment_snapshot_v5 *snapshot)
{
    uint8_t encoded[DISCOVERY_ASSIGNMENT_SNAPSHOT_V5_CHECKSUM_BYTES];
    size_t offset = 0u;

    if (snapshot == NULL) {
        return 0u;
    }
    proto_put_u32_le(&encoded[offset], snapshot->magic);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&encoded[offset], snapshot->size);
    offset += sizeof(uint16_t);
    encoded[offset++] = snapshot->version;
    proto_put_u32_le(&encoded[offset], snapshot->epoch);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_command_seq);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&encoded[offset], snapshot->table_fingerprint);
    offset += sizeof(uint32_t);
    proto_put_u64_le(&encoded[offset], snapshot->local_id);
    offset += sizeof(uint64_t);
    proto_put_u64_le(&encoded[offset], snapshot->gateway_id);
    offset += sizeof(uint64_t);
    encoded[offset++] = snapshot->slot;
    encoded[offset++] = snapshot->slot_count;
    encoded[offset++] = snapshot->provisioned;
    encoded[offset++] = snapshot->valid;
    encoded[offset++] = snapshot->retired_epoch_count;
    encoded[offset++] = snapshot->ordered_epoch_valid;
    for (size_t i = 0u;
         i < DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP;
         i++) {
        proto_put_u32_le(&encoded[offset], snapshot->retired_epochs[i]);
        offset += sizeof(uint32_t);
    }
    if (offset != sizeof(encoded)) {
        return 0u;
    }
    return proto_crc16_ccitt_false(encoded, sizeof(encoded));
}

static bool discovery_assignment_snapshot_v5_valid(
    const struct app_mesh_discovery_assignment_snapshot_v5 *snapshot)
{
    return snapshot != NULL &&
           snapshot->magic == DISCOVERY_ASSIGNMENT_SNAPSHOT_V5_MAGIC &&
           snapshot->version == 5u &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->valid == 1u &&
           snapshot->provisioned <= 1u &&
           snapshot->ordered_epoch_valid == 1u &&
           snapshot->epoch != 0u &&
           snapshot->table_command_seq != 0u &&
           snapshot->table_fingerprint != 0u &&
           snapshot->local_id != 0u &&
           snapshot->gateway_id != 0u &&
           snapshot->slot_count != 0u &&
           snapshot->slot_count <= UWB_DISCOVERY_SLOT_COUNT &&
           snapshot->slot < snapshot->slot_count &&
           (snapshot->provisioned != 0u || snapshot->slot == 0u) &&
           app_discovery_assignment_retired_epochs_valid(
               snapshot->epoch,
               snapshot->retired_epochs,
               snapshot->retired_epoch_count,
               true) &&
           snapshot->checksum ==
               discovery_assignment_snapshot_v5_checksum(snapshot);
}

static bool discovery_assignment_snapshot_import_v5(
    struct app_mesh_discovery_assignment_snapshot *snapshot,
    const struct app_mesh_discovery_assignment_snapshot_v5 *legacy)
{
    if (snapshot == NULL || !discovery_assignment_snapshot_v5_valid(legacy)) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

static uint16_t gateway_assignment_epoch_snapshot_checksum(
    const struct app_mesh_gateway_assignment_epoch_snapshot *snapshot)
{
    struct app_mesh_gateway_assignment_epoch_snapshot copy;

    if (snapshot == NULL) {
        return 0u;
    }
    copy = *snapshot;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static bool gateway_assignment_epoch_snapshot_valid(
    const struct app_mesh_gateway_assignment_epoch_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->magic ==
               APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_MAGIC &&
           snapshot->version ==
               APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->valid == 1u &&
           snapshot->reserved == 0u &&
           snapshot->epoch != 0u &&
           snapshot->checksum ==
               gateway_assignment_epoch_snapshot_checksum(snapshot);
}

static uint16_t gateway_command_sequence_snapshot_checksum(
    const struct app_mesh_gateway_command_sequence_snapshot *snapshot)
{
    struct app_mesh_gateway_command_sequence_snapshot copy;

    if (snapshot == NULL) {
        return 0u;
    }
    copy = *snapshot;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static bool gateway_command_sequence_snapshot_valid(
    const struct app_mesh_gateway_command_sequence_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->magic ==
               APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_MAGIC &&
           snapshot->version ==
               APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->valid == 1u &&
           snapshot->reserved == 0u &&
           snapshot->reserved_through != 0u &&
           snapshot->checksum ==
               gateway_command_sequence_snapshot_checksum(snapshot);
}

static uint32_t gateway_command_sequence_advance(uint32_t sequence,
                                                 uint32_t count)
{
    uint64_t normalized;

    if (sequence == 0u) {
        return count;
    }
    normalized = (uint64_t)(sequence - 1u) + count;
    return (uint32_t)(normalized % UINT32_MAX) + 1u;
}

static uint16_t survey_generation_snapshot_checksum(
    const struct app_mesh_survey_generation_snapshot *snapshot)
{
    struct app_mesh_survey_generation_snapshot copy;

    if (snapshot == NULL) {
        return 0u;
    }
    copy = *snapshot;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static bool survey_generation_snapshot_valid(
    const struct app_mesh_survey_generation_snapshot *snapshot,
    uint8_t role,
    uint64_t local_id,
    uint64_t gateway_id)
{
    return snapshot != NULL &&
           snapshot->magic == APP_MESH_SURVEY_GENERATION_SNAPSHOT_MAGIC &&
           snapshot->version ==
               APP_MESH_SURVEY_GENERATION_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->local_id == local_id &&
           snapshot->gateway_id == gateway_id &&
           snapshot->generation != 0u &&
           (uint32_t)snapshot->generation != 0u &&
           snapshot->role == role &&
           snapshot->valid == 1u &&
           snapshot->reserved == 0u &&
           snapshot->checksum ==
               survey_generation_snapshot_checksum(snapshot);
}

static uint16_t anchor_command_replay_snapshot_checksum(
    const struct app_mesh_anchor_command_replay_snapshot *snapshot)
{
    struct app_mesh_anchor_command_replay_snapshot copy;

    if (snapshot == NULL) {
        return 0u;
    }
    copy = *snapshot;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false((const uint8_t *)&copy, sizeof(copy));
}

static bool anchor_command_replay_snapshot_valid(
    const struct app_mesh_anchor_command_replay_snapshot *snapshot,
    uint64_t local_id,
    uint64_t gateway_id)
{
    return snapshot != NULL &&
           snapshot->magic == APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_MAGIC &&
           snapshot->version ==
               APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->local_id == local_id &&
           snapshot->gateway_id == gateway_id &&
           snapshot->replay.initialized &&
           snapshot->replay.newest_command_seq != 0u &&
           snapshot->replay.committed != 0u &&
           snapshot->valid == 1u &&
           snapshot->reserved == 0u &&
           snapshot->checksum ==
               anchor_command_replay_snapshot_checksum(snapshot);
}
#define APP_MESH_NVS_MINIMUM_USABLE_BYTES \
    ((APP_MESH_NVS_REQUIRED_SECTOR_COUNT - 1u) * \
     (APP_MESH_NVS_SECTOR_SIZE - \
      2u * ROUND_UP(APP_MESH_NVS_ATE_SIZE, \
                    APP_MESH_NVS_CAPACITY_ALIGNMENT)))

BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(storage_partition), okay),
             "mesh persistence requires a storage_partition");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(storage_partition)) >=
             (APP_MESH_NVS_REQUIRED_SECTOR_COUNT * APP_MESH_NVS_SECTOR_SIZE),
             "mesh persistence storage_partition lacks the role's required NVS sectors");
BUILD_ASSERT(DT_PROP(DT_GPARENT(DT_NODELABEL(storage_partition)),
                     write_block_size) <= APP_MESH_NVS_CAPACITY_ALIGNMENT,
             "mesh persistence capacity model supports flash alignment up to four bytes");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_JOURNAL_LIVE_BYTES +
             APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_LIVE_BYTES +
             APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_LIVE_BYTES +
             APP_MESH_NVS_OTHER_LIVE_BYTES <=
             APP_MESH_NVS_MINIMUM_USABLE_BYTES,
             "gateway NVS sectors must fit journals plus all-source durable receipts");
BUILD_ASSERT(APP_MESH_NVS_ANCHOR_RANGE_JOURNAL_LIVE_BYTES +
             APP_MESH_NVS_OTHER_LIVE_BYTES <=
             APP_MESH_NVS_MINIMUM_USABLE_BYTES,
             "anchor NVS sectors must fit the worst-case live key set");
BUILD_ASSERT(sizeof(struct mesh_relay_outbox_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh outbox snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_collection_result_record) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh collection result snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct mesh_relay_child_custody_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh child custody snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_click_handoff_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "mesh click handoff snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_local_delivery_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "local delivery journal must fit comfortably in one NVS sector");
BUILD_ASSERT(GATEWAY_COLLECTION_JOURNAL_RECORD_MAX_SIZE <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "every gateway collection journal record must fit comfortably in NVS");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_0_BASE_ID +
             GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT <=
             APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_1_BASE_ID,
             "gateway collection roster bank zero IDs must not overlap bank one");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID <
             APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_0_BASE_ID,
             "gateway assignment epoch ID must not overlap collection records");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_1_BASE_ID +
             GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT <=
             APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_0_BASE_ID,
             "gateway collection roster IDs must not overlap result IDs");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_0_BASE_ID +
             GATEWAY_COLLECTION_RESULT_CACHE_SIZE <=
             APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_1_BASE_ID,
             "gateway collection result bank zero IDs must not overlap bank one");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_1_BASE_ID +
             GATEWAY_COLLECTION_RESULT_CACHE_SIZE - 1u <= UINT16_MAX,
             "gateway collection result record IDs must fit the NVS key space");
BUILD_ASSERT(sizeof(struct gateway_collection_eack_custody_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway EACK custody must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct gateway_membership_snapshot) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway membership snapshot must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_gateway_click_journal_metadata) <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway host journal metadata must fit comfortably in one NVS sector");
BUILD_ASSERT(sizeof(struct app_mesh_survey_generation_snapshot) == 48u &&
             offsetof(struct app_mesh_survey_generation_snapshot,
                      local_id) == 8u &&
             offsetof(struct app_mesh_survey_generation_snapshot,
                      generation) == 24u &&
             offsetof(struct app_mesh_survey_generation_snapshot,
                      checksum) == 36u,
             "survey generation snapshot schema-1 layout changed");
BUILD_ASSERT(APP_MESH_NVS_ANCHOR_COMMAND_REPLAY_ID <
             APP_MESH_NVS_SURVEY_GENERATION_ID &&
             APP_MESH_NVS_SURVEY_GENERATION_ID <
                 APP_MESH_NVS_SURVEY_PAIR_RESULT_BASE_ID &&
             APP_MESH_NVS_SURVEY_PAIR_RESULT_BASE_ID +
                     APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS <=
             APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID,
             "survey generation/result keys must not overlap persistent journals");
BUILD_ASSERT(sizeof(struct app_mesh_gateway_click_journal_metadata) == 128u &&
             offsetof(struct app_mesh_gateway_click_journal_metadata,
                      payload_digest) == 52u &&
             offsetof(struct app_mesh_gateway_click_journal_metadata,
                      packet_digest) == 84u &&
             offsetof(struct app_mesh_gateway_click_journal_metadata, valid) == 124u &&
             offsetof(struct app_mesh_gateway_click_journal_metadata,
                      host_projection_mask) == 125u &&
             offsetof(struct app_mesh_gateway_click_journal_metadata,
                      state_flags) == 126u,
             "gateway host journal schema-3 layout changed");
BUILD_ASSERT(APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "gateway host journal payload must fit comfortably in one NVS sector");
BUILD_ASSERT(ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "anchor range fragment journal record must fit in one NVS sector");
BUILD_ASSERT(ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN <
             (APP_MESH_NVS_SECTOR_SIZE / 2u),
             "anchor range journal control must fit in one NVS sector");
BUILD_ASSERT(APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID +
             RANGE_REPORT_MAX_PACKET_FRAGMENTS - 1u <= UINT16_MAX,
             "anchor range fragment journal IDs must fit the NVS key space");
BUILD_ASSERT(APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID +
             RANGE_REPORT_MAX_PACKET_FRAGMENTS <=
             APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_BASE_ID,
             "anchor range fragment IDs must not overlap gateway receipts");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_BASE_ID +
             APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES <=
             APP_NVS_ID_CLICK_EVENT_SEQUENCE,
             "gateway receipt IDs must not overlap click sequence storage");
BUILD_ASSERT(APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_BASE_ID >
                 APP_NVS_ID_CLICK_EVENT_SEQUENCE &&
             APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_BASE_ID +
                     APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY - 1u <=
                 UINT16_MAX,
             "gateway terminal receipt IDs must be disjoint and fit NVS");
BUILD_ASSERT(APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN ==
             PACKET_EXT_MAX_PAYLOAD_LEN,
             "gateway host journal must retain the extended payload bound");
BUILD_ASSERT(
    APP_MESH_NVS_ENTRY_BYTES(
        sizeof(struct app_mesh_collection_result_record)) >=
    APP_MESH_NVS_ENTRY_BYTES(
        sizeof(struct app_mesh_gateway_click_journal_metadata)),
    "gateway host metadata overlay must fit the capacity-model entry");
BUILD_ASSERT(
    APP_MESH_NVS_ENTRY_BYTES(
        sizeof(struct app_mesh_local_delivery_snapshot)) >=
    APP_MESH_NVS_ENTRY_BYTES(
        APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN),
    "gateway host payload overlay must fit the capacity-model entry");

#define mesh_nvs (*app_nvs_storage_fs())
static atomic_t mesh_nvs_ready = ATOMIC_INIT(0);
K_MUTEX_DEFINE(mesh_nvs_init_mutex);
K_MUTEX_DEFINE(gateway_control_sequence_mutex);
K_MUTEX_DEFINE(survey_generation_mutex);
static struct k_spinlock mesh_persistence_health_lock;
/*
 * The deferred slot is read during the role's startup restore and whenever
 * custody changes.  Keep that result here so scheduler callbacks can answer
 * without mounting NVS (or copying a full outbox snapshot) from a workqueue.
 * -1 means the mounted store has not been observed yet.
 */
static atomic_t deferred_outbox_presence = ATOMIC_INIT(-1);
/*
 * NVS does not provide a transaction spanning the deferred read/decision/
 * write/delete sequence.  A try-lock keeps that sequence serialized without
 * blocking a radio or system-workqueue thread on flash I/O.  Callers treat
 * -EBUSY as a retryable custody failure and leave the deferred owner intact.
 */
static atomic_t deferred_outbox_busy = ATOMIC_INIT(0);
static uint8_t mesh_nvs_init_retry_round;
static uint32_t mesh_nvs_init_retry_at_ms;
static bool mesh_nvs_init_retry_armed;
static struct app_mesh_persistence_health mesh_persistence_health;
static struct gateway_collection_journal_cursor gateway_collection_journal_cursor;

#if defined(CONFIG_ZTEST)
/*
 * The native NVS fixture uses these bounded faults to exercise the same
 * read/write/delete paths as the application.  They are deliberately scoped
 * to the deferred slot (or the active outbox promotion write) so a test
 * cannot accidentally hide an unrelated journal failure.
 */
static int deferred_test_read_error;
static uint8_t deferred_test_read_failures;
static int deferred_test_write_error;
static uint8_t deferred_test_write_failures;
static int deferred_test_delete_error;
static uint8_t deferred_test_delete_failures;
static int outbox_test_write_error;
static uint8_t outbox_test_write_failures;
static int outbox_test_delete_error;
static uint8_t outbox_test_delete_failures;
static int gateway_eack_custody_delete_error;
static uint8_t gateway_eack_custody_delete_failures;
static int gateway_click_payload_write_error;
static uint8_t gateway_click_payload_write_failures;
static int gateway_click_metadata_write_error;
static uint8_t gateway_click_metadata_write_failures;
static int gateway_click_metadata_read_error;
static uint8_t gateway_click_metadata_read_failures;
static int gateway_click_metadata_verify_error;
static uint8_t gateway_click_metadata_verify_failures;
static int gateway_click_payload_read_error;
static uint8_t gateway_click_payload_read_failures;
static int gateway_click_delete_error;
static uint8_t gateway_click_delete_failures;
static int gateway_click_metadata_delete_error;
static uint8_t gateway_click_metadata_delete_failures;
static int gateway_click_payload_delete_error;
static uint8_t gateway_click_payload_delete_failures;
static int gateway_assignment_epoch_write_error;
static uint8_t gateway_assignment_epoch_write_failures;
static int gateway_membership_write_error;
static uint8_t gateway_membership_write_failures;
static int discovery_assignment_read_error;
static uint8_t discovery_assignment_read_failures;
static int discovery_assignment_delete_error;
static uint8_t discovery_assignment_delete_failures;
static int collection_result_delete_error;
static uint8_t collection_result_delete_failures;
static int child_custody_delete_error;
static uint8_t child_custody_delete_failures;
static int anchor_range_fragment_write_error;
static uint8_t anchor_range_fragment_write_failures;
static int anchor_range_control_write_error;
static uint8_t anchor_range_control_write_failures;
static int anchor_range_fragment_read_error;
static uint8_t anchor_range_fragment_read_failures;
static int anchor_range_control_readback_error;
static uint8_t anchor_range_control_readback_failures;
static int anchor_range_control_delete_error;
static uint8_t anchor_range_control_delete_failures;
static int gateway_collection_receipt_read_error;
static uint8_t gateway_collection_receipt_read_failures;
static int gateway_collection_receipt_write_error;
static uint8_t gateway_collection_receipt_write_failures;
static int gateway_terminal_receipt_read_error;
static uint8_t gateway_terminal_receipt_read_failures;
static int gateway_terminal_receipt_write_error;
static uint8_t gateway_terminal_receipt_write_failures;
static int gateway_terminal_receipt_delete_error;
static uint8_t gateway_terminal_receipt_delete_failures;

static bool deferred_test_consume_fault(int *error, uint8_t *remaining)
{
    if (error == NULL || remaining == NULL || *remaining == 0u) {
        return false;
    }
    (*remaining)--;
    return true;
}

static bool gateway_click_test_consume_fault(int *error, uint8_t *remaining)
{
    if (error == NULL || remaining == NULL || *remaining == 0u) {
        return false;
    }
    (*remaining)--;
    return true;
}
#endif

static void mesh_persistence_note_failure(int ret)
{
    k_spinlock_key_t key =
        k_spin_lock(&mesh_persistence_health_lock);

    if (mesh_persistence_health.total_failures < UINT32_MAX) {
        mesh_persistence_health.total_failures++;
    }
    if (mesh_persistence_health.consecutive_failures < UINT16_MAX) {
        mesh_persistence_health.consecutive_failures++;
    }
    mesh_persistence_health.last_error = ret;
    mesh_persistence_health.ready = atomic_get(&mesh_nvs_ready) != 0;
    k_spin_unlock(&mesh_persistence_health_lock, key);
}

static void mesh_persistence_note_success(void)
{
    k_spinlock_key_t key =
        k_spin_lock(&mesh_persistence_health_lock);

    mesh_persistence_health.consecutive_failures = 0u;
    mesh_persistence_health.last_error = 0;
    mesh_persistence_health.ready = atomic_get(&mesh_nvs_ready) != 0;
    k_spin_unlock(&mesh_persistence_health_lock, key);
}

static int mesh_persistence_init_failed(int ret)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        mesh_nvs_init_retry_round,
        sys_rand32_get());

    atomic_set(&mesh_nvs_ready, 0);
    atomic_set(&deferred_outbox_presence, -1);
    if (mesh_nvs_init_retry_round < UINT8_MAX) {
        mesh_nvs_init_retry_round++;
    }
    mesh_nvs_init_retry_at_ms = k_uptime_get_32() + delay_ms;
    mesh_nvs_init_retry_armed = true;
    mesh_persistence_note_failure(ret);
    LOG_WRN("mesh persistence recovery scheduled: ret=%d round=%u delay_ms=%u",
            ret,
            mesh_nvs_init_retry_round,
            delay_ms);
    return ret;
}

void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health)
{
    if (health != NULL) {
        k_spinlock_key_t key =
            k_spin_lock(&mesh_persistence_health_lock);

        *health = mesh_persistence_health;
        health->ready = atomic_get(&mesh_nvs_ready) != 0;
        k_spin_unlock(&mesh_persistence_health_lock, key);
    }
}

int app_mesh_persistence_init(void)
{
    int ret;

    if (atomic_get(&mesh_nvs_ready) != 0) {
        return 0;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    ret = k_mutex_lock(&mesh_nvs_init_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    if (atomic_get(&mesh_nvs_ready) != 0) {
        ret = 0;
        goto out;
    }
    if (mesh_nvs_init_retry_armed &&
        (int32_t)(k_uptime_get_32() - mesh_nvs_init_retry_at_ms) < 0) {
        struct app_mesh_persistence_health health;

        app_mesh_persistence_get_health(&health);
        ret = health.last_error == 0 ? -EAGAIN : health.last_error;
        goto out;
    }

    ret = app_nvs_storage_init();
    if (ret < 0) {
        LOG_WRN("mesh persistence shared NVS init failed: %d", ret);
        ret = mesh_persistence_init_failed(ret);
        goto out;
    }

    atomic_set(&mesh_nvs_ready, 1);
    atomic_set(&deferred_outbox_presence, -1);
    mesh_nvs_init_retry_round = 0u;
    mesh_nvs_init_retry_at_ms = 0u;
    mesh_nvs_init_retry_armed = false;
    mesh_persistence_note_success();
    LOG_INF("mesh persistence mounted: offset=0x%08x sectors=%u sector_size=%u",
            (unsigned int)mesh_nvs.offset,
            (unsigned int)mesh_nvs.sector_count,
            (unsigned int)mesh_nvs.sector_size);
    ret = 0;
out:
    k_mutex_unlock(&mesh_nvs_init_mutex);
    return ret;
}

static bool mesh_persistence_ready(void)
{
    return app_mesh_persistence_init() == 0;
}

static bool deferred_outbox_try_lock(void)
{
    return atomic_cas(&deferred_outbox_busy, 0, 1);
}

static void deferred_outbox_unlock(void)
{
    atomic_set(&deferred_outbox_busy, 0);
}

static bool mesh_persistence_nvs_write_succeeded(ssize_t written,
                                                 size_t expected_len)
{
    /*
     * nvs_write() returns zero when the latest entry already contains the
     * requested bytes (and for an already-applied delete).  That is a durable,
     * idempotent success, which is essential when retrying after an ambiguous
     * completion.
     */
    return written == 0 ||
           (written > 0 && (size_t)written == expected_len);
}

static int mesh_persistence_write(uint16_t id,
                                  const void *data,
                                  size_t len,
                                  const char *label)
{
    ssize_t written;
    int ret;

#if defined(CONFIG_ZTEST)
    if (id == APP_MESH_NVS_DEFERRED_OUTBOX_ID &&
        deferred_test_consume_fault(&deferred_test_write_error,
                                    &deferred_test_write_failures)) {
        ret = deferred_test_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    if (id == APP_MESH_NVS_OUTBOX_ID &&
        deferred_test_consume_fault(&outbox_test_write_error,
                                    &outbox_test_write_failures)) {
        ret = outbox_test_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    if (id == APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID &&
        deferred_test_consume_fault(
            &gateway_assignment_epoch_write_error,
            &gateway_assignment_epoch_write_failures)) {
        ret = gateway_assignment_epoch_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    if (id == APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID &&
        deferred_test_consume_fault(
            &gateway_membership_write_error,
            &gateway_membership_write_failures)) {
        ret = gateway_membership_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    if (id >= APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID &&
        id < APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID +
                 RANGE_REPORT_MAX_PACKET_FRAGMENTS &&
        deferred_test_consume_fault(
            &anchor_range_fragment_write_error,
            &anchor_range_fragment_write_failures)) {
        ret = anchor_range_fragment_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    if (id == APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID &&
        deferred_test_consume_fault(
            &anchor_range_control_write_error,
            &anchor_range_control_write_failures)) {
        ret = anchor_range_control_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
#endif

    written = nvs_write(&mesh_nvs, id, data, len);

    if (written < 0) {
        ret = (int)written;
    } else if (!mesh_persistence_nvs_write_succeeded(written, len)) {
        ret = -EIO;
    } else {
        mesh_persistence_note_success();
        if (id == APP_MESH_NVS_DEFERRED_OUTBOX_ID) {
            atomic_set(&deferred_outbox_presence, 1);
        }
        return 0;
    }
    mesh_persistence_note_failure(ret);
    LOG_WRN("%s write failed: ret=%d written=%d expected=%u",
            label == NULL ? "mesh persistence" : label,
            ret,
            (int)written,
            (unsigned int)len);
    return ret;
}

static uint16_t gateway_collection_receipt_nvs_id(uint8_t slot)
{
    return (uint16_t)(APP_MESH_NVS_GATEWAY_COLLECTION_RECEIPT_BASE_ID +
                      slot);
}

int app_mesh_persistence_read_gateway_collection_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len)
{
    ssize_t read_len;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES ||
        data == NULL || data_cap == 0u || stored_len == NULL) {
        return -EINVAL;
    }
    *stored_len = 0u;
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_collection_receipt_read_error,
            &gateway_collection_receipt_read_failures)) {
        mesh_persistence_note_failure(gateway_collection_receipt_read_error);
        return gateway_collection_receipt_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        gateway_collection_receipt_nvs_id(slot),
                        data,
                        data_cap);
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    *stored_len = (size_t)read_len;
    mesh_persistence_note_success();
    return 1;
}

int app_mesh_persistence_write_gateway_collection_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES ||
        data == NULL ||
        data_len != APP_GATEWAY_COLLECTION_RECEIPT_RECORD_SIZE) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_collection_receipt_write_error,
            &gateway_collection_receipt_write_failures)) {
        mesh_persistence_note_failure(gateway_collection_receipt_write_error);
        return gateway_collection_receipt_write_error;
    }
#endif
    return mesh_persistence_write(
        gateway_collection_receipt_nvs_id(slot),
        data,
        data_len,
        "gateway collection receipt");
}

int app_mesh_persistence_delete_gateway_collection_receipt(uint8_t slot)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    ret = nvs_delete(&mesh_nvs, gateway_collection_receipt_nvs_id(slot));
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

static uint16_t gateway_terminal_receipt_nvs_id(uint8_t slot)
{
    return (uint16_t)(APP_MESH_NVS_GATEWAY_TERMINAL_RECEIPT_BASE_ID +
                      slot);
}

int app_mesh_persistence_read_gateway_terminal_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len)
{
    ssize_t read_len;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY ||
        data == NULL || data_cap == 0u || stored_len == NULL) {
        return -EINVAL;
    }
    *stored_len = 0u;
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_terminal_receipt_read_error,
            &gateway_terminal_receipt_read_failures)) {
        mesh_persistence_note_failure(gateway_terminal_receipt_read_error);
        return gateway_terminal_receipt_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        gateway_terminal_receipt_nvs_id(slot),
                        data,
                        data_cap);
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    *stored_len = (size_t)read_len;
    mesh_persistence_note_success();
    return 1;
}

int app_mesh_persistence_write_gateway_terminal_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY ||
        data == NULL ||
        data_len != APP_GATEWAY_TERMINAL_RECEIPT_RECORD_SIZE) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_terminal_receipt_write_error,
            &gateway_terminal_receipt_write_failures)) {
        mesh_persistence_note_failure(gateway_terminal_receipt_write_error);
        return gateway_terminal_receipt_write_error;
    }
#endif
    return mesh_persistence_write(
        gateway_terminal_receipt_nvs_id(slot),
        data,
        data_len,
        "gateway terminal receipt");
}

int app_mesh_persistence_delete_gateway_terminal_receipt(uint8_t slot)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_terminal_receipt_delete_error,
            &gateway_terminal_receipt_delete_failures)) {
        mesh_persistence_note_failure(gateway_terminal_receipt_delete_error);
        return gateway_terminal_receipt_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, gateway_terminal_receipt_nvs_id(slot));
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

static uint16_t anchor_range_fragment_nvs_id(uint8_t fragment_index)
{
    return (uint16_t)(APP_MESH_NVS_ANCHOR_RANGE_FRAGMENT_BASE_ID +
                      fragment_index);
}

static bool anchor_range_control_base_valid(
    const struct anchor_range_journal_control *control)
{
    return control != NULL &&
           control->generation != 0u &&
           control->clicker_id != 0u &&
           control->event_seq != 0u &&
           control->anchor_id != 0u &&
           control->gateway_id != 0u &&
           control->fragment_count <= RANGE_REPORT_MAX_PACKET_FRAGMENTS;
}

static bool anchor_range_controls_equal(
    const struct anchor_range_journal_control *left,
    const struct anchor_range_journal_control *right)
{
    if (left == NULL || right == NULL ||
        left->clicker_id != right->clicker_id ||
        left->anchor_id != right->anchor_id ||
        left->gateway_id != right->gateway_id ||
        left->event_seq != right->event_seq ||
        left->generation != right->generation ||
        left->fragment_count != right->fragment_count ||
        left->attempt_index != right->attempt_index) {
        return false;
    }
    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        if (left->fragments[i].seq != right->fragments[i].seq ||
            left->fragments[i].wire_len != right->fragments[i].wire_len ||
            left->fragments[i].wire_crc != right->fragments[i].wire_crc) {
            return false;
        }
    }
    return true;
}

static int anchor_range_read_control_locked(
    struct anchor_range_journal_control *control)
{
    uint8_t record[ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN];
    ssize_t read_len;
    int ret;

    if (control == NULL) {
        return -EINVAL;
    }
    memset(control, 0, sizeof(*control));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID,
                        record,
                        sizeof(record));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    ret = anchor_range_journal_decode_control(
        record, (size_t)read_len, control);
    if (ret < 0) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    return 1;
}

static int anchor_range_read_fragment_locked(
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    struct mesh_outbound *outbound)
{
    struct anchor_range_journal_fragment_identity identity = {0};
    struct proto_packet packet = {0};
    uint8_t record[ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN];
    uint8_t reencoded[PACKET_MAX_LEN];
    const uint8_t *wire = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0u;
    size_t reencoded_len = 0u;
    size_t wire_len = 0u;
    ssize_t read_len;
    int ret;

    if (!anchor_range_control_base_valid(control) ||
        fragment_index >= control->fragment_count) {
        return -EINVAL;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &anchor_range_fragment_read_error,
            &anchor_range_fragment_read_failures)) {
        mesh_persistence_note_failure(anchor_range_fragment_read_error);
        return anchor_range_fragment_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        anchor_range_fragment_nvs_id(fragment_index),
                        record,
                        sizeof(record));
    if (read_len == -ENOENT) {
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    ret = anchor_range_journal_decode_fragment(
        record,
        (size_t)read_len,
        control->generation,
        fragment_index,
        &wire,
        &wire_len,
        &identity);
    if (ret < 0 ||
        identity.seq != control->fragments[fragment_index].seq ||
        identity.wire_len != control->fragments[fragment_index].wire_len ||
        identity.wire_crc != control->fragments[fragment_index].wire_crc) {
        ret = ret < 0 ? ret : -EBADMSG;
        mesh_persistence_note_failure(ret);
        return ret;
    }
    ret = proto_packet_decode(
        wire, wire_len, &packet, &payload, &payload_len);
    if (ret != PROTO_OK ||
        packet.msg_type != MSG_CLICK_REPORT ||
        (packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        packet.src_id != control->anchor_id ||
        packet.dst_id != control->gateway_id ||
        packet.session_id != control->event_seq ||
        packet.seq != identity.seq ||
        packet.payload_len != payload_len ||
        payload_len > UWB_MESH_MAX_PAYLOAD_LEN) {
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    ret = proto_packet_encode(&packet,
                              payload,
                              reencoded,
                              sizeof(reencoded),
                              &reencoded_len);
    if (ret != PROTO_OK || reencoded_len != wire_len ||
        memcmp(reencoded, wire, wire_len) != 0) {
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }

    if (outbound != NULL) {
        memset(outbound, 0, sizeof(*outbound));
        outbound->packet = packet;
        memcpy(outbound->payload, payload, payload_len);
        outbound->payload_len = (uint16_t)payload_len;
    }
    return 0;
}

int app_mesh_persistence_prepare_anchor_range_journal(
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    uint64_t anchor_id,
    uint64_t gateway_id,
    struct anchor_range_journal_control *control)
{
    struct anchor_range_journal_control existing;
    uint32_t generation;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    if (clicker_id == 0u || event_seq == 0u || anchor_id == 0u ||
        gateway_id == 0u || control == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = anchor_range_read_control_locked(&existing);
    deferred_outbox_unlock();
    if (ret != 0) {
        return ret > 0 ? -EBUSY : ret;
    }

    generation = sys_rand32_get();
    if (generation == 0u) {
        generation = event_seq ^ (uint32_t)anchor_id ^
                     (uint32_t)(anchor_id >> 32u);
        if (generation == 0u) {
            generation = 1u;
        }
    }
    memset(control, 0, sizeof(*control));
    control->clicker_id = clicker_id;
    control->event_seq = event_seq;
    control->attempt_index = attempt_index;
    control->anchor_id = anchor_id;
    control->gateway_id = gateway_id;
    control->generation = generation;
    return 0;
}

int app_mesh_persistence_save_anchor_range_fragment(
    struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    const struct mesh_outbound *outbound,
    enum anchor_range_fragment_persistence_observation *observation)
{
    struct anchor_range_journal_control existing;
    struct anchor_range_journal_fragment_identity identity = {0};
    struct anchor_range_journal_fragment_identity restored_identity = {0};
    uint8_t record[ANCHOR_RANGE_JOURNAL_FRAGMENT_RECORD_MAX_LEN];
    uint8_t wire[PACKET_MAX_LEN];
    const uint8_t *restored_wire = NULL;
    size_t record_len = 0u;
    size_t restored_wire_len = 0u;
    size_t wire_len = 0u;
    ssize_t read_len;
    int ret;

    if (observation == NULL) {
        return -EINVAL;
    }
    *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    if (!anchor_range_control_base_valid(control) ||
        outbound == NULL ||
        fragment_index != control->fragment_count ||
        fragment_index >= RANGE_REPORT_MAX_PACKET_FRAGMENTS ||
        outbound->packet.msg_type != MSG_CLICK_REPORT ||
        (outbound->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        outbound->packet.src_id != control->anchor_id ||
        outbound->packet.dst_id != control->gateway_id ||
        outbound->packet.session_id != control->event_seq ||
        outbound->packet.payload_len != outbound->payload_len ||
        outbound->payload_len > PACKET_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }
    ret = proto_packet_encode(&outbound->packet,
                              outbound->payload,
                              wire,
                              sizeof(wire),
                              &wire_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = anchor_range_journal_encode_fragment(
        control->generation,
        fragment_index,
        wire,
        wire_len,
        record,
        sizeof(record),
        &record_len,
        &identity);
    if (ret < 0) {
        return ret;
    }
    if (!mesh_persistence_ready()) {
        *observation =
            ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE;
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        *observation =
            ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE;
        return -EBUSY;
    }
    ret = anchor_range_read_control_locked(&existing);
    if (ret > 0) {
        /*
         * The existing marker has exactly control->fragment_count committed
         * fragments, so it can never confirm the new fragment at that same
         * index. Treat even an equal base record as stale instead of falsely
         * publishing an unpersisted append.
         */
        ret = -ESTALE;
        goto out;
    }
    if (ret < 0) {
        if (ret == -EAGAIN || ret == -EBUSY || ret == -EINTR ||
            ret == -ETIMEDOUT) {
            *observation =
                ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE;
        }
        goto out;
    }
    ret = mesh_persistence_write(
        anchor_range_fragment_nvs_id(fragment_index),
        record,
        record_len,
        "anchor range journal fragment");
    if (ret < 0) {
        *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS;
        goto out;
    }
    memset(record, 0, sizeof(record));
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &anchor_range_fragment_read_error,
            &anchor_range_fragment_read_failures)) {
        ret = anchor_range_fragment_read_error;
        *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS;
        mesh_persistence_note_failure(ret);
        goto out;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        anchor_range_fragment_nvs_id(fragment_index),
                        record,
                        sizeof(record));
    if (read_len < 0) {
        ret = (int)read_len;
        *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS;
        mesh_persistence_note_failure(ret);
        goto out;
    }
    ret = anchor_range_journal_decode_fragment(
        record,
        (size_t)read_len,
        control->generation,
        fragment_index,
        &restored_wire,
        &restored_wire_len,
        &restored_identity);
    if (ret < 0 ||
        restored_wire_len != wire_len ||
        memcmp(restored_wire, wire, wire_len) != 0 ||
        restored_identity.seq != identity.seq ||
        restored_identity.wire_len != identity.wire_len ||
        restored_identity.wire_crc != identity.wire_crc) {
        ret = ret < 0 ? ret : -EIO;
        *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS;
        mesh_persistence_note_failure(ret);
        goto out;
    }
    control->fragments[fragment_index] = identity;
    control->fragment_count++;
    *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED;
    ret = 0;

out:
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_commit_anchor_range_journal(
    const struct anchor_range_journal_control *control)
{
    struct anchor_range_journal_control existing;
    struct anchor_range_journal_control restored;
    uint8_t record[ANCHOR_RANGE_JOURNAL_CONTROL_RECORD_LEN];
    size_t record_len = 0u;
    ssize_t read_len;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    ret = anchor_range_journal_encode_control(
        control, record, sizeof(record), &record_len);
    if (ret < 0) {
        return ret;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = anchor_range_read_control_locked(&existing);
    if (ret > 0) {
        /*
         * The control marker may have reached NVS even when its readback
         * failed or reset interrupted the caller.  Retrying the exact commit
         * is success; a different owner must remain blocked.
         */
        ret = anchor_range_controls_equal(control, &existing) ? 0 : -EBUSY;
        goto out;
    }
    if (ret < 0) {
        goto out;
    }
    for (uint8_t i = 0u; i < control->fragment_count; i++) {
        ret = anchor_range_read_fragment_locked(control, i, NULL);
        if (ret < 0) {
            goto out;
        }
    }
    ret = mesh_persistence_write(APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID,
                                 record,
                                 record_len,
                                 "anchor range journal commit");
    if (ret < 0) {
        goto out;
    }
    memset(record, 0, sizeof(record));
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &anchor_range_control_readback_error,
            &anchor_range_control_readback_failures)) {
        ret = anchor_range_control_readback_error;
        mesh_persistence_note_failure(ret);
        goto out;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID,
                        record,
                        sizeof(record));
    if (read_len < 0) {
        ret = (int)read_len;
        mesh_persistence_note_failure(ret);
        goto out;
    }
    ret = anchor_range_journal_decode_control(
        record, (size_t)read_len, &restored);
    if (ret < 0 || !anchor_range_controls_equal(control, &restored)) {
        ret = ret < 0 ? ret : -EIO;
        mesh_persistence_note_failure(ret);
        goto out;
    }
    ret = 0;

out:
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_restore_anchor_range_journal(
    struct anchor_range_journal_control *control)
{
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    if (control == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = anchor_range_read_control_locked(control);
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_restore_anchor_range_fragment(
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    struct mesh_outbound *outbound)
{
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = anchor_range_read_fragment_locked(
        control, fragment_index, outbound);
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_clear_anchor_range_journal(
    const struct anchor_range_journal_control *control)
{
    struct anchor_range_journal_control existing;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -ENOTSUP;
    }
    if (!anchor_range_control_base_valid(control)) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = anchor_range_read_control_locked(&existing);
    if (ret == 0) {
        goto out;
    }
    if (ret < 0) {
        goto out;
    }
    if (!anchor_range_controls_equal(control, &existing)) {
        ret = -ESTALE;
        goto out;
    }

    /*
     * Delete the commit marker first. A reset after this point can expose
     * only orphan fragment records, which restore deliberately ignores.
     */
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &anchor_range_control_delete_error,
            &anchor_range_control_delete_failures)) {
        ret = anchor_range_control_delete_error;
        mesh_persistence_note_failure(ret);
        goto out;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_ANCHOR_RANGE_CONTROL_ID);
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        goto out;
    }
    for (uint8_t i = 0u; i < existing.fragment_count; i++) {
        int delete_ret = nvs_delete(
            &mesh_nvs, anchor_range_fragment_nvs_id(i));

        if (delete_ret < 0 && delete_ret != -ENOENT) {
            mesh_persistence_note_failure(delete_ret);
            LOG_WRN("anchor range journal orphan cleanup failed: fragment=%u ret=%d",
                    i, delete_ret);
        }
    }
    mesh_persistence_note_success();
    ret = 0;

out:
    deferred_outbox_unlock();
    return ret;
}

static bool gateway_click_journal_try_lock(void)
{
    /* Share the existing NVS transaction gate; NVS does not support
     * concurrent read/write/delete sequences, and adding a second atomic
     * would consume precious gateway static RAM for no stronger guarantee. */
    return deferred_outbox_try_lock();
}

static void gateway_click_journal_unlock(void)
{
    deferred_outbox_unlock();
}

struct gateway_click_journal_metadata_v1 {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct proto_packet packet;
    uint16_t payload_len;
    uint16_t payload_crc;
    uint32_t received_at_ms;
    uint32_t checksum;
    uint8_t valid;
    uint8_t host_projection_mask;
    uint16_t reserved;
};

struct gateway_click_journal_metadata_v2 {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct proto_packet packet;
    uint16_t payload_len;
    uint16_t payload_crc;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t received_at_ms;
    uint32_t checksum;
    uint8_t valid;
    uint8_t host_projection_mask;
    uint16_t reserved;
};

BUILD_ASSERT(sizeof(struct gateway_click_journal_metadata_v1) == 64u &&
             offsetof(struct gateway_click_journal_metadata_v1, valid) == 60u,
             "gateway host journal schema-1 migration layout changed");
BUILD_ASSERT(sizeof(struct gateway_click_journal_metadata_v2) == 96u &&
             offsetof(struct gateway_click_journal_metadata_v2, valid) == 92u,
             "gateway host journal schema-2 migration layout changed");

static bool gateway_click_packet_matches(const struct proto_packet *left,
                                         const struct proto_packet *right)
{
    /* A relay may decrement TTL and accumulate message age before the same
     * report reaches the gateway again.  Those transport fields are not part
     * of semantic identity; every other header field, including all semantic
     * flags, remains immutable. Callers also compare the full payload
     * commitment before treating the packet as an exact retry. */
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->payload_len == right->payload_len;
}

static bool gateway_click_payload_digest(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return payload_len <= APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN &&
           semantic_digest_sha256(payload, payload_len, digest);
}

static bool gateway_click_payload_matches(
    const struct app_mesh_gateway_click_journal_metadata *metadata,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN];

    return metadata != NULL &&
           metadata->payload_len == payload_len &&
           gateway_click_payload_digest(payload, payload_len, digest) &&
           semantic_digest_equal(metadata->payload_digest,
                                 digest,
                                 sizeof(digest)) &&
           mesh_packet_semantic_digest(&metadata->packet,
                                       payload,
                                       payload_len,
                                       packet_digest) &&
           semantic_digest_equal(metadata->packet_digest,
                                 packet_digest,
                                 sizeof(packet_digest));
}

static uint32_t gateway_click_metadata_checksum(
    const struct app_mesh_gateway_click_journal_metadata *metadata)
{
    struct app_mesh_gateway_click_journal_metadata copy;

    if (metadata == NULL) {
        return 0u;
    }
    copy = *metadata;
    copy.checksum = 0u;
    return (uint32_t)proto_crc16_ccitt_false((const uint8_t *)&copy,
                                              sizeof(copy));
}

static uint32_t gateway_click_metadata_v1_checksum(
    const struct gateway_click_journal_metadata_v1 *metadata)
{
    struct gateway_click_journal_metadata_v1 copy;

    if (metadata == NULL) {
        return 0u;
    }
    copy = *metadata;
    copy.checksum = 0u;
    return (uint32_t)proto_crc16_ccitt_false((const uint8_t *)&copy,
                                              sizeof(copy));
}

static uint32_t gateway_click_metadata_v2_checksum(
    const struct gateway_click_journal_metadata_v2 *metadata)
{
    struct gateway_click_journal_metadata_v2 copy;

    if (metadata == NULL) {
        return 0u;
    }
    copy = *metadata;
    copy.checksum = 0u;
    return (uint32_t)proto_crc16_ccitt_false((const uint8_t *)&copy,
                                              sizeof(copy));
}

static bool gateway_click_metadata_phase_valid(uint8_t phase)
{
    return phase == APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED ||
           phase == APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED ||
           phase == APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW ||
           phase == APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED;
}

static bool gateway_click_metadata_valid(
    const struct app_mesh_gateway_click_journal_metadata *metadata)
{
    return metadata != NULL &&
           metadata->magic == APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC &&
           metadata->version == APP_MESH_GATEWAY_HOST_JOURNAL_VERSION &&
           metadata->size == sizeof(*metadata) &&
           gateway_click_metadata_phase_valid(metadata->valid) &&
           app_mesh_persistence_gateway_host_journal_supports(
               &metadata->packet) &&
           metadata->payload_len != 0u &&
           metadata->payload_len <=
               APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN &&
           metadata->packet.payload_len == metadata->payload_len &&
           (metadata->state_flags &
            (uint16_t)~APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED) ==
               0u &&
           (metadata->host_projection_mask == 0u ||
            metadata->packet.msg_type == MSG_RESULT_BUNDLE) &&
           metadata->checksum == gateway_click_metadata_checksum(metadata);
}

static bool gateway_click_metadata_v1_valid(
    const struct gateway_click_journal_metadata_v1 *metadata)
{
    return metadata != NULL &&
           metadata->magic == APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC &&
           metadata->version == 1u &&
           metadata->size == sizeof(*metadata) &&
           gateway_click_metadata_phase_valid(metadata->valid) &&
           app_mesh_persistence_gateway_host_journal_supports(
               &metadata->packet) &&
           metadata->payload_len != 0u &&
           metadata->payload_len <=
               APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN &&
           metadata->packet.payload_len == metadata->payload_len &&
           metadata->reserved == 0u &&
           (metadata->host_projection_mask == 0u ||
            metadata->packet.msg_type == MSG_RESULT_BUNDLE) &&
           metadata->checksum == gateway_click_metadata_v1_checksum(metadata);
}

static bool gateway_click_metadata_v2_valid(
    const struct gateway_click_journal_metadata_v2 *metadata)
{
    return metadata != NULL &&
           metadata->magic == APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC &&
           metadata->version == 2u &&
           metadata->size == sizeof(*metadata) &&
           gateway_click_metadata_phase_valid(metadata->valid) &&
           app_mesh_persistence_gateway_host_journal_supports(
               &metadata->packet) &&
           metadata->payload_len != 0u &&
           metadata->payload_len <=
               APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN &&
           metadata->packet.payload_len == metadata->payload_len &&
           metadata->reserved == 0u &&
           (metadata->host_projection_mask == 0u ||
            metadata->packet.msg_type == MSG_RESULT_BUNDLE) &&
           metadata->checksum ==
               gateway_click_metadata_v2_checksum(metadata);
}

static int gateway_click_write(uint16_t id,
                               const void *data,
                               size_t len,
                               bool metadata);
static int gateway_click_read_payload(uint8_t *payload,
                                      size_t payload_cap,
                                      size_t expected_len);

static int gateway_click_read_metadata(
    struct app_mesh_gateway_click_journal_metadata *metadata)
{
    union {
        struct app_mesh_gateway_click_journal_metadata current;
        struct gateway_click_journal_metadata_v2 v2;
        struct gateway_click_journal_metadata_v1 legacy;
    } stored;
    ssize_t read_len;

    if (metadata == NULL) {
        return -EINVAL;
    }
    memset(metadata, 0, sizeof(*metadata));
    memset(&stored, 0, sizeof(stored));
#if defined(CONFIG_ZTEST)
    if (gateway_click_test_consume_fault(&gateway_click_metadata_read_error,
                                         &gateway_click_metadata_read_failures)) {
        mesh_persistence_note_failure(gateway_click_metadata_read_error);
        return gateway_click_metadata_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                        &stored,
                        sizeof(stored));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len == sizeof(stored.current) &&
        gateway_click_metadata_valid(&stored.current)) {
        *metadata = stored.current;
        return 1;
    }
    if ((size_t)read_len == sizeof(stored.v2) &&
        gateway_click_metadata_v2_valid(&stored.v2)) {
        uint8_t payload[APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN];
        uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
        int ret = gateway_click_read_payload(payload,
                                             sizeof(payload),
                                             stored.v2.payload_len);

        if (ret < 0 ||
            proto_crc16_ccitt_false(payload, stored.v2.payload_len) !=
                stored.v2.payload_crc ||
            !gateway_click_payload_digest(payload,
                                          stored.v2.payload_len,
                                          payload_digest) ||
            !semantic_digest_equal(stored.v2.payload_digest,
                                   payload_digest,
                                   sizeof(payload_digest))) {
            mesh_persistence_note_failure(ret < 0 ? ret : -EBADMSG);
            return ret < 0 ? ret : -EBADMSG;
        }
        memset(metadata, 0, sizeof(*metadata));
        metadata->magic = stored.v2.magic;
        metadata->version = APP_MESH_GATEWAY_HOST_JOURNAL_VERSION;
        metadata->size = sizeof(*metadata);
        metadata->packet = stored.v2.packet;
        metadata->payload_len = stored.v2.payload_len;
        metadata->payload_crc = stored.v2.payload_crc;
        memcpy(metadata->payload_digest,
               stored.v2.payload_digest,
               sizeof(metadata->payload_digest));
        if (!mesh_packet_semantic_digest(&metadata->packet,
                                         payload,
                                         metadata->payload_len,
                                         metadata->packet_digest)) {
            return -EBADMSG;
        }
        metadata->received_at_ms = stored.v2.received_at_ms;
        metadata->valid = stored.v2.valid;
        metadata->host_projection_mask =
            stored.v2.host_projection_mask;
        metadata->checksum = gateway_click_metadata_checksum(metadata);
        ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                                  metadata,
                                  sizeof(*metadata),
                                  true);
        if (ret < 0) {
            return ret;
        }
        return 1;
    }
    if ((size_t)read_len == sizeof(stored.legacy) &&
        gateway_click_metadata_v1_valid(&stored.legacy)) {
        uint8_t payload[APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN];
        int ret = gateway_click_read_payload(payload,
                                             sizeof(payload),
                                             stored.legacy.payload_len);

        if (ret < 0 ||
            proto_crc16_ccitt_false(payload, stored.legacy.payload_len) !=
                stored.legacy.payload_crc) {
            mesh_persistence_note_failure(ret < 0 ? ret : -EBADMSG);
            return ret < 0 ? ret : -EBADMSG;
        }
        memset(metadata, 0, sizeof(*metadata));
        metadata->magic = stored.legacy.magic;
        metadata->version = APP_MESH_GATEWAY_HOST_JOURNAL_VERSION;
        metadata->size = sizeof(*metadata);
        metadata->packet = stored.legacy.packet;
        metadata->payload_len = stored.legacy.payload_len;
        metadata->payload_crc = stored.legacy.payload_crc;
        metadata->received_at_ms = stored.legacy.received_at_ms;
        metadata->valid = stored.legacy.valid;
        metadata->host_projection_mask =
            stored.legacy.host_projection_mask;
        if (!gateway_click_payload_digest(payload,
                                          stored.legacy.payload_len,
                                          metadata->payload_digest) ||
            !mesh_packet_semantic_digest(&metadata->packet,
                                         payload,
                                         metadata->payload_len,
                                         metadata->packet_digest)) {
            return -EBADMSG;
        }
        metadata->checksum = gateway_click_metadata_checksum(metadata);
        ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                                  metadata,
                                  sizeof(*metadata),
                                  true);
        if (ret < 0) {
            return ret;
        }
        return 1;
    }
    {
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
}

static int gateway_click_verify_metadata(
    const struct app_mesh_gateway_click_journal_metadata *expected)
{
    struct app_mesh_gateway_click_journal_metadata actual;
    int ret;

    if (expected == NULL) {
        return -EINVAL;
    }
#if defined(CONFIG_ZTEST)
    if (gateway_click_test_consume_fault(
            &gateway_click_metadata_verify_error,
            &gateway_click_metadata_verify_failures)) {
        mesh_persistence_note_failure(gateway_click_metadata_verify_error);
        return gateway_click_metadata_verify_error;
    }
#endif
    ret = gateway_click_read_metadata(&actual);
    if (ret != 1) {
        return ret == 0 ? -EIO : ret;
    }
    if (memcmp(&actual, expected, sizeof(actual)) != 0) {
        mesh_persistence_note_failure(-EIO);
        return -EIO;
    }
    return 0;
}

static int gateway_click_write(uint16_t id,
                               const void *data,
                               size_t len,
                               bool metadata)
{
    ssize_t written;
    int ret;

#if defined(CONFIG_ZTEST)
    if (metadata) {
        if (gateway_click_test_consume_fault(
                &gateway_click_metadata_write_error,
                &gateway_click_metadata_write_failures)) {
            ret = gateway_click_metadata_write_error;
            mesh_persistence_note_failure(ret);
            return ret;
        }
    } else if (gateway_click_test_consume_fault(
                   &gateway_click_payload_write_error,
                   &gateway_click_payload_write_failures)) {
        ret = gateway_click_payload_write_error;
        mesh_persistence_note_failure(ret);
        return ret;
    }
#endif

    written = nvs_write(&mesh_nvs, id, data, len);
    if (written < 0) {
        ret = (int)written;
    } else if (!mesh_persistence_nvs_write_succeeded(written, len)) {
        ret = -EIO;
    } else {
        mesh_persistence_note_success();
        return 0;
    }
    mesh_persistence_note_failure(ret);
    return ret;
}

static int gateway_click_read_payload(uint8_t *payload,
                                      size_t payload_cap,
                                      size_t expected_len)
{
    ssize_t read_len;

    if (payload == NULL || expected_len == 0u ||
        expected_len > payload_cap) {
        return -EINVAL;
    }
#if defined(CONFIG_ZTEST)
    if (gateway_click_test_consume_fault(&gateway_click_payload_read_error,
                                         &gateway_click_payload_read_failures)) {
        mesh_persistence_note_failure(gateway_click_payload_read_error);
        return gateway_click_payload_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID,
                        payload,
                        payload_cap);
    if (read_len == -ENOENT) {
        /* A committed marker with no payload is permanently malformed, not a
         * transient flash read.  The caller retains the marker and keeps host
         * admission closed because it may be the only accepted custody
         * evidence left after the gateway ACK. */
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != expected_len) {
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    return (int)read_len;
}

static int gateway_click_delete(uint16_t id, bool metadata)
{
    int ret;

#if defined(CONFIG_ZTEST)
    if (metadata &&
        gateway_click_test_consume_fault(&gateway_click_metadata_delete_error,
                                         &gateway_click_metadata_delete_failures)) {
        mesh_persistence_note_failure(gateway_click_metadata_delete_error);
        return gateway_click_metadata_delete_error;
    }
    if (!metadata &&
        gateway_click_test_consume_fault(&gateway_click_payload_delete_error,
                                         &gateway_click_payload_delete_failures)) {
        mesh_persistence_note_failure(gateway_click_payload_delete_error);
        return gateway_click_payload_delete_error;
    }
    if (gateway_click_test_consume_fault(&gateway_click_delete_error,
                                         &gateway_click_delete_failures)) {
        mesh_persistence_note_failure(gateway_click_delete_error);
        return gateway_click_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, id);
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    ARG_UNUSED(metadata);
    return 0;
}

/* Delete the payload before its metadata commit marker.  If either delete is
 * interrupted, the marker still blocks replacement admission; a later retry
 * can remove an orphan payload or a marker with a missing payload without
 * touching a newer click. */
static int gateway_click_clear_locked(void)
{
    int ret;

    ret = gateway_click_delete(APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID, false);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_click_delete(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID, true);
    if (ret < 0) {
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

static int gateway_collection_journal_nvs_id(
    struct gateway_collection_journal_key key,
    uint16_t *id)
{
    if (id == NULL) {
        return -EINVAL;
    }

    switch (key.kind) {
    case GATEWAY_COLLECTION_JOURNAL_RECORD_BASE:
        if (key.bank >= GATEWAY_COLLECTION_JOURNAL_BANK_COUNT || key.index != 0u) {
            return -EINVAL;
        }
        *id = key.bank == 0u ? APP_MESH_NVS_GATEWAY_COLLECTION_ID :
                              APP_MESH_NVS_GATEWAY_COLLECTION_BASE_1_ID;
        return 0;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL:
        if (key.bank >= GATEWAY_COLLECTION_JOURNAL_BANK_COUNT || key.index != 0u) {
            return -EINVAL;
        }
        *id = key.bank == 0u ? APP_MESH_NVS_GATEWAY_COLLECTION_CONTROL_0_ID :
                              APP_MESH_NVS_GATEWAY_COLLECTION_CONTROL_1_ID;
        return 0;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_ROSTER:
        if (key.bank >= GATEWAY_COLLECTION_JOURNAL_BANK_COUNT ||
            key.index >= GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT) {
            return -EINVAL;
        }
        *id = (uint16_t)((key.bank == 0u ?
                         APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_0_BASE_ID :
                         APP_MESH_NVS_GATEWAY_COLLECTION_ROSTER_1_BASE_ID) +
                        key.index);
        return 0;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT:
        if (key.bank >= GATEWAY_COLLECTION_JOURNAL_BANK_COUNT ||
            key.index >= GATEWAY_COLLECTION_RESULT_CACHE_SIZE) {
            return -EINVAL;
        }
        *id = (uint16_t)((key.bank == 0u ?
                         APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_0_BASE_ID :
                         APP_MESH_NVS_GATEWAY_COLLECTION_RESULT_1_BASE_ID) +
                        key.index);
        return 0;
    default:
        return -EINVAL;
    }
}

static int gateway_collection_journal_nvs_read(
    void *ctx,
    struct gateway_collection_journal_key key,
    void *data,
    size_t data_cap,
    size_t *stored_len)
{
    ssize_t read_len;
    uint16_t id;
    int ret;

    ARG_UNUSED(ctx);
    if (stored_len == NULL || (data == NULL && data_cap != 0u)) {
        return -EINVAL;
    }
    ret = gateway_collection_journal_nvs_id(key, &id);
    if (ret < 0) {
        return ret;
    }
    read_len = nvs_read(&mesh_nvs, id, data, data_cap);
    if (read_len < 0) {
        return (int)read_len;
    }
    *stored_len = (size_t)read_len;
    return 0;
}

static int gateway_collection_journal_nvs_write(
    void *ctx,
    struct gateway_collection_journal_key key,
    const void *data,
    size_t data_len)
{
    uint16_t id;
    int ret;

    ARG_UNUSED(ctx);
    ret = gateway_collection_journal_nvs_id(key, &id);
    if (ret < 0) {
        return ret;
    }
    return mesh_persistence_write(id,
                                  data,
                                  data_len,
                                  "gateway collection journal record");
}

static const struct gateway_collection_journal_io gateway_collection_journal_io = {
    .read = gateway_collection_journal_nvs_read,
    .write = gateway_collection_journal_nvs_write,
};

static bool click_handoff_snapshot_valid(
    const struct app_mesh_click_handoff_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->valid &&
           snapshot->version == APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION &&
           (snapshot->phase == APP_MESH_CLICK_HANDOFF_STAGED ||
            snapshot->phase == APP_MESH_CLICK_HANDOFF_COMMITTED) &&
           snapshot->outbox.valid;
}

static bool outbox_snapshots_match(const struct mesh_relay_outbox_snapshot *left,
                                   const struct mesh_relay_outbox_snapshot *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->role == right->role && left->local_id == right->local_id &&
           left->gateway_id == right->gateway_id &&
           left->pending.packet.msg_type == right->pending.packet.msg_type &&
           left->pending.packet.src_id == right->pending.packet.src_id &&
           left->pending.packet.dst_id == right->pending.packet.dst_id &&
           left->pending.packet.session_id == right->pending.packet.session_id &&
           left->pending.packet.seq == right->pending.packet.seq &&
           left->pending.payload_len == right->pending.payload_len &&
           memcmp(left->pending.payload,
                  right->pending.payload,
                  left->pending.payload_len) == 0;
}

/*
 * A deferred slot owns one packet, not one wall-clock serialization.  The
 * age, absolute retry deadlines, and snapshot timestamp can legitimately
 * change between a failed delete and the retry.  Every other custody field
 * must remain identical before an occupied slot is treated as an idempotent
 * save; a different packet or delivery phase remains busy.
 */
static bool deferred_outbox_snapshots_match(
    const struct mesh_relay_outbox_snapshot *left,
    const struct mesh_relay_outbox_snapshot *right)
{
    if (!outbox_snapshots_match(left, right)) {
        return false;
    }
    return left->version == right->version &&
           left->record.valid == right->record.valid &&
           left->record.packet_id == right->record.packet_id &&
           left->record.gateway_id == right->record.gateway_id &&
           left->record.session_id == right->record.session_id &&
           left->record.seq == right->record.seq &&
           left->record.packet_class == right->record.packet_class &&
           left->record.payload_len == right->record.payload_len &&
           left->record.payload_crc == right->record.payload_crc &&
           left->record.delivery_state == right->record.delivery_state &&
           left->record.gateway_acked == right->record.gateway_acked &&
           left->record.retry_round == right->record.retry_round &&
           left->record.expiry_s == right->record.expiry_s &&
           left->pending.state == right->pending.state &&
           left->pending.packet.flags == right->pending.packet.flags &&
           left->pending.packet.ttl == right->pending.packet.ttl &&
           left->pending.radio_channel == right->pending.radio_channel &&
           left->pending.next_hop_id == right->pending.next_hop_id &&
           left->pending.result_offer_active == right->pending.result_offer_active &&
           left->pending.gateway_ack_forward_pending ==
               right->pending.gateway_ack_forward_pending &&
           left->pending.busy_retry_round == right->pending.busy_retry_round;
}

static int verify_outbox_snapshot(uint16_t id,
                                  const struct mesh_relay_outbox_snapshot *expected)
{
    struct mesh_relay_outbox_snapshot stored;
    ssize_t read_len;

    if (expected == NULL) {
        return -EINVAL;
    }
    memset(&stored, 0, sizeof(stored));
    read_len = nvs_read(&mesh_nvs, id, &stored, sizeof(stored));
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(stored) ||
        memcmp(&stored, expected, sizeof(stored)) != 0) {
        mesh_persistence_note_failure(-EIO);
        return -EIO;
    }
    mesh_persistence_note_success();
    return 0;
}

static bool deferred_outbox_packet_matches(
    const struct mesh_relay_outbox_snapshot *snapshot,
    const struct proto_packet *packet)
{
    if (snapshot == NULL || packet == NULL || !snapshot->valid) {
        return false;
    }
    return snapshot->pending.packet.msg_type == packet->msg_type &&
           snapshot->pending.packet.flags == packet->flags &&
           snapshot->pending.packet.src_id == packet->src_id &&
           snapshot->pending.packet.dst_id == packet->dst_id &&
           snapshot->pending.packet.session_id == packet->session_id &&
           snapshot->pending.packet.seq == packet->seq &&
           snapshot->pending.packet.payload_len == packet->payload_len &&
           snapshot->pending.payload_len == packet->payload_len;
}

static int read_deferred_outbox(
    struct mesh_relay_outbox_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&deferred_test_read_error,
                                   &deferred_test_read_failures)) {
        mesh_persistence_note_failure(deferred_test_read_error);
        return deferred_test_read_error;
    }
#endif
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_DEFERRED_OUTBOX_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        atomic_set(&deferred_outbox_presence, 0);
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !snapshot->valid ||
        snapshot->version != MESH_RELAY_OUTBOX_SNAPSHOT_VERSION) {
        /* The key exists, even though its payload is unusable.  The caller
         * decides whether it is safe to retire that custody record. */
        atomic_set(&deferred_outbox_presence, 1);
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    atomic_set(&deferred_outbox_presence, 1);
    return 1;
}

/* Caller must hold deferred_outbox_busy. */
static int clear_deferred_outbox_locked(void)
{
    int ret;

#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&deferred_test_delete_error,
                                   &deferred_test_delete_failures)) {
        mesh_persistence_note_failure(deferred_test_delete_error);
        return deferred_test_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_DEFERRED_OUTBOX_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh deferred outbox clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    /* Publish custody only after the delete has completed successfully. */
    atomic_set(&deferred_outbox_presence, 0);
    mesh_persistence_note_success();
    return 0;
}

static int read_click_handoff(struct app_mesh_click_handoff_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_CLICK_HANDOFF_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        mesh_persistence_note_failure((int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !click_handoff_snapshot_valid(snapshot)) {
        mesh_persistence_note_failure(-EINVAL);
        return -EINVAL;
    }
    return 1;
}

static int clear_click_handoff(void)
{
    int ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_CLICK_HANDOFF_ID);

    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_outbox(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }

#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&outbox_test_delete_error,
                                    &outbox_test_delete_failures)) {
        mesh_persistence_note_failure(outbox_test_delete_error);
        return outbox_test_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_OUTBOX_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted outbox clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_deferred_outbox(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }

    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = clear_deferred_outbox_locked();
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_deferred_outbox_present(void)
{
    atomic_val_t presence;

    if (atomic_get(&mesh_nvs_ready) == 0) {
        struct app_mesh_persistence_health health;

        app_mesh_persistence_get_health(&health);
        return health.last_error == 0 ? -EAGAIN : health.last_error;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    presence = atomic_get(&deferred_outbox_presence);
    deferred_outbox_unlock();

    return presence < 0 ? -EAGAIN : (presence != 0 ? 1 : 0);
}

int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (snapshot == NULL ||
        !app_mesh_local_delivery_snapshot_valid(snapshot)) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    return mesh_persistence_write(APP_MESH_NVS_LOCAL_DELIVERY_ID,
                                  snapshot, sizeof(*snapshot),
                                  "local delivery journal");
}

int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    ssize_t read_len;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    read_len = nvs_read(&mesh_nvs, APP_MESH_NVS_LOCAL_DELIVERY_ID,
                        snapshot, sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !app_mesh_local_delivery_snapshot_valid(snapshot)) {
        memset(snapshot, 0, sizeof(*snapshot));
        return -EBADMSG;
    }
    return 1;
}

int app_mesh_persistence_clear_local_delivery(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_LOCAL_DELIVERY_ID);
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

static uint16_t survey_pair_result_delivery_nvs_id(uint8_t slot)
{
    return (uint16_t)(APP_MESH_NVS_SURVEY_PAIR_RESULT_BASE_ID + slot);
}

int app_mesh_persistence_save_survey_pair_result_delivery(
    uint8_t slot,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (slot >= APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS ||
        snapshot == NULL ||
        !app_mesh_local_delivery_snapshot_valid(snapshot) ||
        snapshot->outbound.packet.msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    return mesh_persistence_write(
        survey_pair_result_delivery_nvs_id(slot),
        snapshot,
        sizeof(*snapshot),
        "survey pair result delivery");
}

int app_mesh_persistence_restore_survey_pair_result_delivery(
    uint8_t slot,
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    ssize_t read_len;

    if (slot >= APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS ||
        snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    read_len = nvs_read(
        &mesh_nvs,
        survey_pair_result_delivery_nvs_id(slot),
        snapshot,
        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !app_mesh_local_delivery_snapshot_valid(snapshot) ||
        snapshot->outbound.packet.msg_type != MSG_SURVEY_PAIR_RESULT) {
        memset(snapshot, 0, sizeof(*snapshot));
        return -EBADMSG;
    }
    return 1;
}

int app_mesh_persistence_clear_survey_pair_result_delivery(uint8_t slot)
{
    int ret;

    if (slot >= APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = nvs_delete(
        &mesh_nvs, survey_pair_result_delivery_nvs_id(slot));
    if (ret < 0 && ret != -ENOENT) {
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

static int gateway_click_journal_validate_input(const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        !app_mesh_persistence_gateway_host_journal_supports(packet) ||
        payload_len == 0u ||
        payload_len > APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }
    return 0;
}

int app_mesh_persistence_gateway_host_journal_matches_with_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *host_projection_mask)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    if (host_projection_mask != NULL) {
        *host_projection_mask = 0u;
    }
    ret = gateway_click_journal_validate_input(packet, payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    ret = gateway_click_read_metadata(&metadata);
    if (ret == 0) {
        gateway_click_journal_unlock();
        return 0;
    }
    if (ret < 0) {
        /*
         * A malformed marker may be the sole remaining proof of a record that
         * was already gateway-ACKed.  Preserve it and fail admission closed;
         * only the explicit repair/clear API may discard corrupt evidence.
         */
        gateway_click_journal_unlock();
        return ret;
    }

    ret = gateway_click_packet_matches(&metadata.packet, packet) &&
          gateway_click_payload_matches(&metadata, payload, payload_len) ?
              metadata.valid : -EBUSY;
    if (ret > 0 && host_projection_mask != NULL) {
        *host_projection_mask = metadata.host_projection_mask;
    }
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_gateway_host_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return app_mesh_persistence_gateway_host_journal_matches_with_projection(
        packet, payload, payload_len, NULL);
}

int app_mesh_persistence_prepare_gateway_host_journal_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms,
    uint8_t host_projection_mask)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t payload_crc;
    int ret;

    ret = gateway_click_journal_validate_input(packet, payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    if (host_projection_mask != 0u &&
        packet->msg_type != MSG_RESULT_BUNDLE) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    payload_crc = proto_crc16_ccitt_false(payload, payload_len);
    if (!gateway_click_payload_digest(payload,
                                      payload_len,
                                      payload_digest)) {
        gateway_click_journal_unlock();
        return -EINVAL;
    }
    ret = gateway_click_read_metadata(&metadata);
    if (ret == 1) {
        if (gateway_click_packet_matches(&metadata.packet, packet) &&
            gateway_click_payload_matches(&metadata, payload, payload_len) &&
            metadata.host_projection_mask == host_projection_mask) {
            /* Re-write the exact payload on an idempotent retry.  A reset or
             * torn clear may have removed the payload while its marker delete
             * was still pending; returning success without repairing it would
             * leave a valid marker pointing at a missing record. */
            ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID,
                                      payload,
                                      payload_len,
                                      false);
            gateway_click_journal_unlock();
            return ret;
        }
        gateway_click_journal_unlock();
        return -EBUSY;
    }
    if (ret < 0) {
        /*
         * Never replace an unreadable marker in place.  It may represent an
         * accepted host record whose upstream sender has already retired
         * custody.
         */
        gateway_click_journal_unlock();
        return ret;
    }

    /* Payload bytes are durable before the PREPARED metadata marker. */
    ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID,
                              payload,
                              payload_len,
                              false);
    if (ret < 0) {
        gateway_click_journal_unlock();
        return ret;
    }

    memset(&metadata, 0, sizeof(metadata));
    metadata.magic = APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC;
    metadata.version = APP_MESH_GATEWAY_HOST_JOURNAL_VERSION;
    metadata.size = sizeof(metadata);
    metadata.packet = *packet;
    metadata.payload_len = (uint16_t)payload_len;
    metadata.payload_crc = payload_crc;
    memcpy(metadata.payload_digest,
           payload_digest,
           sizeof(metadata.payload_digest));
    if (!mesh_packet_semantic_digest(packet,
                                     payload,
                                     payload_len,
                                     metadata.packet_digest)) {
        gateway_click_journal_unlock();
        return -EINVAL;
    }
    metadata.received_at_ms = received_at_ms;
    metadata.valid = APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED;
    metadata.host_projection_mask = host_projection_mask;
    metadata.state_flags = 0u;
    metadata.checksum = gateway_click_metadata_checksum(&metadata);
    ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                              &metadata,
                              sizeof(metadata),
                              true);
    if (ret == 0) {
        ret = gateway_click_verify_metadata(&metadata);
    }
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_prepare_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    return app_mesh_persistence_prepare_gateway_host_journal_projection(
        packet, payload, payload_len, received_at_ms, 0u);
}

static int gateway_host_journal_promote(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t terminal_phase)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    ret = gateway_click_journal_validate_input(packet, payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    if (terminal_phase != APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED &&
        terminal_phase != APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    ret = gateway_click_read_metadata(&metadata);
    if (ret <= 0) {
        ret = ret == 0 ? -ENOENT : ret;
        goto out;
    }
    if (!gateway_click_packet_matches(&metadata.packet, packet) ||
        !gateway_click_payload_matches(&metadata, payload, payload_len)) {
        ret = -ESTALE;
        goto out;
    }
    if (metadata.valid == terminal_phase) {
        ret = 0;
        goto out;
    }
    if (metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED) {
        ret = -EBADMSG;
        goto out;
    }

    metadata.valid = terminal_phase;
    metadata.checksum = gateway_click_metadata_checksum(&metadata);
    ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                              &metadata,
                              sizeof(metadata),
                              true);
    if (ret == 0) {
        ret = gateway_click_verify_metadata(&metadata);
    }

out:
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_commit_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return gateway_host_journal_promote(
        packet,
        payload,
        payload_len,
        APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED);
}

int app_mesh_persistence_recover_raw_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return gateway_host_journal_promote(
        packet,
        payload,
        payload_len,
        APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW);
}

int app_mesh_persistence_save_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    int ret = app_mesh_persistence_prepare_gateway_host_journal(
        packet, payload, payload_len, received_at_ms);

    if (ret < 0) {
        return ret;
    }
    return app_mesh_persistence_commit_gateway_host_journal(
        packet, payload, payload_len);
}

int app_mesh_persistence_restore_gateway_host_journal_projection(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms,
    uint8_t *host_projection_mask)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int read_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || payload == NULL || payload_len == NULL ||
        received_at_ms == NULL || host_projection_mask == NULL) {
        return -EINVAL;
    }
    memset(packet, 0, sizeof(*packet));
    *payload_len = 0u;
    *received_at_ms = 0u;
    *host_projection_mask = 0u;
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    read_ret = gateway_click_read_metadata(&metadata);
    if (read_ret == 0) {
        gateway_click_journal_unlock();
        return 0;
    }
    if (read_ret < 0) {
        /* Corrupt sole-custody evidence remains present and blocks admission. */
        gateway_click_journal_unlock();
        return read_ret;
    }
    if (metadata.payload_len > payload_cap) {
        gateway_click_journal_unlock();
        return -EMSGSIZE;
    }

    ret = gateway_click_read_payload(payload,
                                     payload_cap,
                                     metadata.payload_len);
    if (ret < 0) {
        /* Preserve the marker when its exact payload cannot be recovered. */
        gateway_click_journal_unlock();
        return ret;
    }
    if (proto_crc16_ccitt_false(payload, metadata.payload_len) !=
            metadata.payload_crc ||
        !gateway_click_payload_matches(&metadata,
                                       payload,
                                       metadata.payload_len)) {
        ret = -EBADMSG;
        mesh_persistence_note_failure(ret);
        gateway_click_journal_unlock();
        return ret;
    }

    *packet = metadata.packet;
    *payload_len = metadata.payload_len;
    *received_at_ms = metadata.received_at_ms;
    *host_projection_mask = metadata.host_projection_mask;
    gateway_click_journal_unlock();
    return metadata.valid;
}

int app_mesh_persistence_restore_gateway_host_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms)
{
    uint8_t host_projection_mask = 0u;

    return app_mesh_persistence_restore_gateway_host_journal_projection(
        packet,
        payload,
        payload_cap,
        payload_len,
        received_at_ms,
        &host_projection_mask);
}

int app_mesh_persistence_restore_gateway_host_terminal_marker(
    struct proto_packet *packet,
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool *source_confirmed)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || packet_digest == NULL ||
        source_confirmed == NULL) {
        return -EINVAL;
    }
    memset(packet, 0, sizeof(*packet));
    memset(packet_digest, 0, SEMANTIC_DIGEST_SHA256_LEN);
    *source_confirmed = false;
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    ret = gateway_click_read_metadata(&metadata);
    if (ret == 1) {
        if (metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
            ret = -EBADMSG;
        } else {
            *packet = metadata.packet;
            memcpy(packet_digest,
                   metadata.packet_digest,
                   SEMANTIC_DIGEST_SHA256_LEN);
            *source_confirmed =
                (metadata.state_flags &
                 APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED) != 0u;
        }
    }
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_clear_gateway_host_journal(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }
    ret = gateway_click_clear_locked();
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_clear_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        !gateway_click_payload_digest(payload,
                                      payload_len,
                                      payload_digest)) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }
    ret = gateway_click_read_metadata(&metadata);
    if (ret == 0) {
        /* Also retire any orphan payload left by an earlier torn commit. */
        ret = gateway_click_clear_locked();
    } else if (ret == 1 &&
               gateway_click_packet_matches(&metadata.packet, packet) &&
               semantic_digest_equal(metadata.payload_digest,
                                     payload_digest,
                                     sizeof(payload_digest))) {
        ret = gateway_click_clear_locked();
    } else if (ret == 1) {
        ret = -ESTALE;
    }
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
    const struct proto_packet *packet,
    const uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || payload_digest == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    ret = gateway_click_read_metadata(&metadata);
    if (ret <= 0) {
        ret = ret == 0 ? -ENOENT : ret;
        goto out;
    }
    if (!gateway_click_packet_matches(&metadata.packet, packet) ||
        !semantic_digest_equal(metadata.payload_digest,
                               payload_digest,
                               SEMANTIC_DIGEST_SHA256_LEN)) {
        ret = -ESTALE;
        goto out;
    }
    if (metadata.valid == APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
        ret = 0;
        goto out;
    }
    if (metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED &&
        metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
        ret = -EBADMSG;
        goto out;
    }

    metadata.valid = APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED;
    metadata.checksum = gateway_click_metadata_checksum(&metadata);
    ret = gateway_click_write(APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
                              &metadata,
                              sizeof(metadata),
                              true);
    if (ret == 0) {
        ret = gateway_click_verify_metadata(&metadata);
    }
    if (ret == 0 &&
        (metadata.state_flags &
         APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED) != 0u) {
        /*
         * Both independent durable phases are now proven. Delete only after
         * NOTIFIED readback so a reset can never turn an early source confirm
         * into lost host data.
         */
        ret = gateway_click_clear_locked();
        if (ret == 0) {
            ret = 1;
        }
    }

out:
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_confirm_gateway_host_journal(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    struct proto_packet acknowledged_packet;
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = mesh_gateway_ack_confirm_identity_packet(
        confirm_packet,
        confirm_payload,
        confirm_payload_len,
        &acknowledged_packet,
        packet_digest);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }

    ret = gateway_click_read_metadata(&metadata);
    if (ret == 0) {
        /* A valid late duplicate cannot mutate host state; ACK it
         * idempotently after an earlier exact clear and reset. */
        goto out;
    }
    if (ret < 0) {
        goto out;
    }
    if (!gateway_click_packet_matches(&metadata.packet,
                                      &acknowledged_packet) ||
        !semantic_digest_equal(metadata.packet_digest,
                               packet_digest,
                               sizeof(packet_digest))) {
        /*
         * The occupied singleton belongs to a different report. A stale,
         * structurally valid confirm is still terminal for its source, but
         * must never clear or rewrite the current owner.
         */
        ret = 0;
        goto out;
    }
    if (metadata.valid == APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED) {
        ret = -EBADMSG;
        goto out;
    }
    if ((metadata.state_flags &
         APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED) == 0u) {
        metadata.state_flags |=
            APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED;
        metadata.checksum = gateway_click_metadata_checksum(&metadata);
        ret = gateway_click_write(
            APP_MESH_NVS_GATEWAY_CLICK_METADATA_ID,
            &metadata,
            sizeof(metadata),
            true);
        if (ret == 0) {
            ret = gateway_click_verify_metadata(&metadata);
        }
        if (ret < 0) {
            goto out;
        }
    }
    if (metadata.valid == APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
        ret = gateway_click_clear_locked();
        if (ret < 0) {
            goto out;
        }
        ret = 2;
        goto out;
    }
    ret = 1;

out:
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_finalize_gateway_host_journal_if_complete(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    ret = gateway_click_journal_validate_input(packet, payload, payload_len);
    if (ret < 0) {
        return ret;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }
    ret = gateway_click_read_metadata(&metadata);
    if (ret == 0) {
        goto out;
    }
    if (ret < 0) {
        goto out;
    }
    if (!gateway_click_packet_matches(&metadata.packet, packet) ||
        !gateway_click_payload_matches(&metadata, payload, payload_len)) {
        ret = -ESTALE;
        goto out;
    }
    if (metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED ||
        (metadata.state_flags &
         APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED) == 0u) {
        ret = 0;
        goto out;
    }
    ret = gateway_click_clear_locked();
    if (ret == 0) {
        ret = 1;
    }

out:
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_retire_notified_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct app_mesh_gateway_click_journal_metadata metadata;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (packet == NULL || packet_digest == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    if (!gateway_click_journal_try_lock()) {
        return -EBUSY;
    }
    ret = gateway_click_read_metadata(&metadata);
    if (ret == 0) {
        goto out;
    }
    if (ret < 0) {
        goto out;
    }
    if (!gateway_click_packet_matches(&metadata.packet, packet) ||
        !semantic_digest_equal(metadata.packet_digest,
                               packet_digest,
                               SEMANTIC_DIGEST_SHA256_LEN)) {
        ret = -ESTALE;
        goto out;
    }
    if (metadata.valid != APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
        ret = -EAGAIN;
        goto out;
    }
    ret = gateway_click_clear_locked();
    if (ret == 0) {
        ret = 1;
    }

out:
    gateway_click_journal_unlock();
    return ret;
}

int app_mesh_persistence_gateway_click_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return app_mesh_persistence_gateway_host_journal_matches(
        packet, payload, payload_len);
}

int app_mesh_persistence_save_gateway_click_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    return app_mesh_persistence_save_gateway_host_journal(
        packet, payload, payload_len, received_at_ms);
}

int app_mesh_persistence_restore_gateway_click_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms)
{
    return app_mesh_persistence_restore_gateway_host_journal(
        packet,
        payload,
        payload_cap,
        payload_len,
        received_at_ms);
}

int app_mesh_persistence_clear_gateway_click_journal(void)
{
    return app_mesh_persistence_clear_gateway_host_journal();
}

int app_mesh_persistence_clear_gateway_click_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return app_mesh_persistence_clear_gateway_host_journal_if_matches(
        packet, payload, payload_len);
}

int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot = {
        .version = APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION,
        .phase = APP_MESH_CLICK_HANDOFF_STAGED,
        .valid = true,
    };
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot.outbox);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NOT_FOUND ? -ENOENT : -EINVAL;
    }
    return mesh_persistence_write(APP_MESH_NVS_CLICK_HANDOFF_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh click handoff stage");
}

int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot;
    struct mesh_relay_outbox_snapshot active;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = read_click_handoff(&snapshot);
    if (ret <= 0) {
        return ret == 0 ? -ENOENT : ret;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &active);
    if (ret != PROTO_OK || !outbox_snapshots_match(&snapshot.outbox, &active)) {
        return -ESTALE;
    }
    snapshot.phase = APP_MESH_CLICK_HANDOFF_COMMITTED;
    return mesh_persistence_write(APP_MESH_NVS_CLICK_HANDOFF_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh click handoff commit");
}

int app_mesh_persistence_rollback_click_handoff(void)
{
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    return clear_click_handoff();
}

int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    struct app_mesh_click_handoff_snapshot snapshot;
    struct mesh_relay_outbox_snapshot active;
    int ret;

    if (relay == NULL || !mesh_relay_tx_active(relay)) {
        return 0;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }
    ret = read_click_handoff(&snapshot);
    if (ret <= 0) {
        return ret < 0 ? ret : 0;
    }
    if (snapshot.phase != APP_MESH_CLICK_HANDOFF_COMMITTED) {
        return 0;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &active);
    if (ret != PROTO_OK || !outbox_snapshots_match(&snapshot.outbox, &active)) {
        return 0;
    }
    return clear_click_handoff();
}

int app_mesh_persistence_complete_confirmed_producer(
    const struct proto_packet *original_packet,
    const uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct app_mesh_click_handoff_snapshot handoff;
    const struct mesh_pending_tx *pending;
    uint8_t handoff_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (original_packet == NULL || original_digest == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -EAGAIN;
    }

    ret = read_click_handoff(&handoff);
    if (ret <= 0) {
        return ret < 0 ? ret : 0;
    }
    pending = &handoff.outbox.pending;
    if (pending->packet.msg_type != original_packet->msg_type ||
        pending->packet.flags != original_packet->flags ||
        pending->packet.src_id != original_packet->src_id ||
        pending->packet.dst_id != original_packet->dst_id ||
        pending->packet.session_id != original_packet->session_id ||
        pending->packet.seq != original_packet->seq ||
        pending->packet.payload_len != original_packet->payload_len ||
        pending->payload_len != original_packet->payload_len) {
        return 0;
    }
    if (!mesh_packet_semantic_digest(&pending->packet,
                                     pending->payload,
                                     pending->payload_len,
                                     handoff_digest) ||
        !semantic_digest_equal(handoff_digest,
                               original_digest,
                               sizeof(handoff_digest))) {
        return -EBADMSG;
    }
    return clear_click_handoff();
}

int app_mesh_persistence_complete_terminal_producer(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (packet == NULL ||
        (payload_len > 0u && payload == NULL) ||
        packet->payload_len != payload_len ||
        !mesh_packet_semantic_digest(packet,
                                     payload,
                                     payload_len,
                                     semantic_digest)) {
        return -EINVAL;
    }
    return app_mesh_persistence_complete_confirmed_producer(
        packet, semantic_digest);
}

int app_mesh_persistence_clear_collection_result(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&collection_result_delete_error,
                                    &collection_result_delete_failures)) {
        mesh_persistence_note_failure(collection_result_delete_error);
        return collection_result_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_COLLECTION_RESULT_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted collection result clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_child_custody(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&child_custody_delete_error,
                                    &child_custody_delete_failures)) {
        mesh_persistence_note_failure(child_custody_delete_error);
        return child_custody_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_CHILD_CUSTODY_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("mesh persisted child custody clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_gateway_collection(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = gateway_collection_journal_clear(&gateway_collection_journal_io,
                                           &gateway_collection_journal_cursor,
                                           NULL);
    if (ret < 0) {
        LOG_WRN("gateway collection journal tombstone failed: %d", ret);
    }
    return ret;
}

int app_mesh_persistence_clear_gateway_eack_custody(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(
            &gateway_eack_custody_delete_error,
            &gateway_eack_custody_delete_failures)) {
        mesh_persistence_note_failure(gateway_eack_custody_delete_error);
        return gateway_eack_custody_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_GATEWAY_EACK_CUSTODY_ID);
    if (ret < 0 && ret != -ENOENT) {
        LOG_WRN("gateway EACK custody clear failed: %d", ret);
        mesh_persistence_note_failure(ret);
        return ret;
    }
    mesh_persistence_note_success();
    return 0;
}

int app_mesh_persistence_clear_gateway_membership(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID);
    if (ret == -ENOENT) {
        return 0;
    }
    if (ret < 0) {
        LOG_WRN("gateway membership snapshot clear failed: %d", ret);
    }
    return ret;
}

int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return app_mesh_persistence_clear_outbox();
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh outbox snapshot export failed: %d", ret);
        return -EINVAL;
    }

    ret = mesh_persistence_write(APP_MESH_NVS_OUTBOX_ID,
                                 &snapshot,
                                 sizeof(snapshot),
                                 "mesh outbox snapshot");
    if (ret < 0) {
        return ret;
    }
    return verify_outbox_snapshot(APP_MESH_NVS_OUTBOX_ID, &snapshot);
}

int app_mesh_persistence_save_deferred_outbox(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    struct mesh_relay_outbox_snapshot existing;
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        /* An existing deferred owner is still authoritative even when the
         * active runtime has already been cancelled.  Do not clear it merely
         * because this caller no longer has a RAM snapshot to export. */
        if (!deferred_outbox_try_lock()) {
            return -EBUSY;
        }
        ret = read_deferred_outbox(&existing);
        if (ret > 0) {
            deferred_outbox_unlock();
            return -EBUSY;
        }
        if (ret < 0) {
            deferred_outbox_unlock();
            return ret;
        }
        ret = clear_deferred_outbox_locked();
        deferred_outbox_unlock();
        return ret;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh deferred outbox snapshot export failed: %d", ret);
        return -EINVAL;
    }

    /* The deferred slot is a single custody owner.  A retry after a failed
     * delete may observe the exact same record; acknowledge that save without
     * rewriting it.  A different valid owner remains busy. */
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    ret = read_deferred_outbox(&existing);
    if (ret > 0) {
        ret = deferred_outbox_snapshots_match(&existing, &snapshot) ?
              0 : -EBUSY;
        deferred_outbox_unlock();
        return ret;
    }
    if (ret == -EBADMSG) {
        /* Only a positively malformed/incompatible record may be retired. */
        ret = clear_deferred_outbox_locked();
        if (ret < 0) {
            deferred_outbox_unlock();
            return ret;
        }
    } else if (ret < 0) {
        /* A read I/O error is transient; retain the sole custody copy. */
        deferred_outbox_unlock();
        return ret;
    }

    ret = mesh_persistence_write(APP_MESH_NVS_DEFERRED_OUTBOX_ID,
                                 &snapshot,
                                 sizeof(snapshot),
                                 "mesh deferred outbox snapshot");
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    struct mesh_relay_child_custody_snapshot snapshot;
    int ret;

    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = mesh_relay_export_child_custody_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return app_mesh_persistence_clear_child_custody();
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh child custody snapshot export failed: %d", ret);
        return -EINVAL;
    }

    return mesh_persistence_write(APP_MESH_NVS_CHILD_CUSTODY_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh child custody snapshot");
}

int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    struct app_mesh_click_handoff_snapshot handoff;
    ssize_t read_len;
    int handoff_ret;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    handoff_ret = read_click_handoff(&handoff);
    if (handoff_ret < 0) {
        LOG_WRN("mesh click handoff journal read failed: %d", handoff_ret);
        return handoff_ret;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_OUTBOX_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        if (handoff_ret > 0) {
            ret = mesh_relay_restore_outbox_snapshot(relay, &handoff.outbox, now_ms);
            if (ret != PROTO_OK) {
                LOG_WRN("mesh staged click handoff fallback restore rejected: %d", ret);
                return -EINVAL;
            }
            LOG_INF("mesh staged click handoff restored as reset fallback");
        }
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("mesh outbox snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot)) {
        LOG_WRN("mesh outbox snapshot has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(snapshot));
        /*
         * This key may be the only durable owner of an accepted packet.
         * Preserve corrupt bytes so every boot remains fail-closed and an
         * explicit repair path can inspect or retire them.
         */
        return -EINVAL;
    }

    /*
     * Validate every installed primary snapshot before applying handoff
     * precedence.  A committed original handoff normally supersedes an older
     * primary copy, but it must never hide a corrupt sole-custody record.
     * Restore/cancel is the public validation path and changes only the relay
     * pending/outbox fields.
     */
    ret = mesh_relay_restore_outbox_snapshot(relay, &snapshot, now_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh outbox snapshot restore rejected: %d", ret);
        return -EINVAL;
    }

    /*
     * The gateway-ACK confirmation is the durable successor of the original
     * producer record.  Keep the producer handoff until the exact terminal
     * ACK proves that confirmation; restoring the older original here would
     * reopen already-confirmed work after every reset.
     */
    if (snapshot.pending.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM) {
        LOG_INF("mesh gateway ACK confirmation restored");
        return 0;
    }

    if (handoff_ret > 0 &&
        handoff.phase == APP_MESH_CLICK_HANDOFF_COMMITTED) {
        mesh_relay_cancel_tx(relay);
        ret = mesh_relay_restore_outbox_snapshot(relay,
                                                 &handoff.outbox,
                                                 now_ms);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh committed click handoff restore rejected: %d", ret);
            return -EINVAL;
        }
        /* The committed journal is authoritative until the same outbox is resaved. */
        ret = app_mesh_persistence_clear_outbox();
        if (ret < 0) {
            LOG_WRN("mesh obsolete outbox clear deferred after handoff restore: %d", ret);
        }
        LOG_INF("mesh committed click handoff restored");
        return 0;
    }

    LOG_INF("mesh outbox snapshot restored");
    if (handoff_ret > 0) {
        ret = clear_click_handoff();
        if (ret < 0) {
            LOG_WRN("mesh staged click handoff cleanup deferred: %d", ret);
        }
    }
    return 0;
}

int app_mesh_persistence_gateway_assignment_proves(
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id)
{
    union gateway_membership_stored_snapshot stored = {0};
    struct gateway_membership_roster validation_roster;
    ssize_t read_len;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (assignment_epoch == 0u || table_seq == 0u ||
        table_commitment == NULL || node_id == 0u) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT ||
        (size_t)read_len ==
            sizeof(struct gateway_membership_snapshot_v1)) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v2)) {
        const struct gateway_membership_snapshot_v2 *legacy =
            (const struct gateway_membership_snapshot_v2 *)stored.raw;

        if (gateway_membership_restore_v2(
                legacy, &validation_roster) < 0) {
            return -EINVAL;
        }
        /* A legacy 32-bit proof can never authorize a schema-2 ACK. */
        return 0;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v3)) {
        const struct gateway_membership_snapshot_v3 *legacy =
            &stored.legacy_v3;

        if (gateway_membership_restore_v3(
                legacy, &validation_roster) < 0) {
            return -EINVAL;
        }
        /* A legacy 32-bit proof can never authorize a schema-2 ACK. */
        return 0;
    }
    if ((size_t)read_len != sizeof(stored.current) ||
        gateway_membership_restore_snapshot(
            &validation_roster, &stored.current) != PROTO_OK) {
        return -EINVAL;
    }
    return gateway_membership_snapshot_proves_assignment(
               &stored.current,
               assignment_epoch,
               table_seq,
               table_commitment,
               node_id) ?
           1 : 0;
}

int app_mesh_persistence_restore_deferred_outbox(struct mesh_relay *relay,
                                                 uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot snapshot;
    int read_ret;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    if (mesh_relay_tx_active(relay)) {
        return -EBUSY;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    read_ret = read_deferred_outbox(&snapshot);
    if (read_ret == 0) {
        deferred_outbox_unlock();
        return 0;
    }
    if (read_ret < 0) {
        /*
         * A read or validation failure is not permission to discard the only
         * packet owner. Keep the key and cached presence asserted so every
         * startup retries or fails closed until explicit repair.
         */
        deferred_outbox_unlock();
        return read_ret;
    }

    ret = mesh_relay_restore_outbox_snapshot(relay, &snapshot, now_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh deferred outbox restore rejected: %d", ret);
        deferred_outbox_unlock();
        return -EINVAL;
    }

    /* Establish the normal active journal before retiring the handoff copy. */
    ret = mesh_persistence_write(APP_MESH_NVS_OUTBOX_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "mesh deferred outbox promotion");
    if (ret < 0) {
        LOG_WRN("mesh deferred outbox promotion failed: %d", ret);
        deferred_outbox_unlock();
        return ret;
    }
    ret = verify_outbox_snapshot(APP_MESH_NVS_OUTBOX_ID, &snapshot);
    if (ret < 0) {
        LOG_WRN("mesh deferred outbox promotion verification failed: %d", ret);
        deferred_outbox_unlock();
        return ret;
    }
    ret = clear_deferred_outbox_locked();
    if (ret < 0) {
        LOG_WRN("mesh deferred outbox cleanup deferred: %d", ret);
        deferred_outbox_unlock();
        return ret;
    }
    deferred_outbox_unlock();
    LOG_INF("mesh deferred outbox restored");
    return 0;
}

int app_mesh_persistence_complete_deferred_outbox(struct mesh_relay *relay,
                                                  uint32_t now_ms)
{
    struct mesh_relay_outbox_snapshot deferred;
    struct mesh_relay_outbox_snapshot active;
    int read_ret;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    read_ret = read_deferred_outbox(&deferred);
    if (read_ret <= 0) {
        deferred_outbox_unlock();
        return read_ret < 0 ? read_ret : 0;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &active);
    if (ret != PROTO_OK || !outbox_snapshots_match(&deferred, &active)) {
        deferred_outbox_unlock();
        return 0;
    }
    ret = clear_deferred_outbox_locked();
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_clear_deferred_outbox_if_matches(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct mesh_relay_outbox_snapshot deferred;
    uint8_t stored_digest[SEMANTIC_DIGEST_SHA256_LEN];
    int read_ret;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }
    read_ret = read_deferred_outbox(&deferred);
    if (read_ret <= 0) {
        deferred_outbox_unlock();
        return read_ret < 0 ? read_ret : 0;
    }
    if (!deferred_outbox_packet_matches(&deferred, packet)) {
        deferred_outbox_unlock();
        return 0;
    }
    if (!mesh_packet_semantic_digest(&deferred.pending.packet,
                                     deferred.pending.payload,
                                     deferred.pending.payload_len,
                                     stored_digest) ||
        !semantic_digest_equal(stored_digest,
                               semantic_digest,
                               sizeof(stored_digest))) {
        deferred_outbox_unlock();
        return -EBADMSG;
    }
    read_ret = clear_deferred_outbox_locked();
    deferred_outbox_unlock();
    return read_ret;
}

int app_mesh_persistence_restore_child_custody(struct mesh_relay *relay,
                                               uint32_t now_ms)
{
    struct mesh_relay_child_custody_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_CHILD_CUSTODY_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("mesh child custody snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot)) {
        LOG_WRN("mesh child custody snapshot has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(snapshot));
        return -EINVAL;
    }

    ret = mesh_relay_restore_child_custody_snapshot(relay, &snapshot, now_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh child custody snapshot restore rejected: %d", ret);
        return -EINVAL;
    }

    LOG_INF("mesh child custody snapshot restored");
    return 0;
}

static uint16_t collection_result_record_checksum(
    const struct app_mesh_collection_result_record *record)
{
    struct app_mesh_collection_result_record copy;

    if (record == NULL) {
        return 0u;
    }
    copy = *record;
    copy.checksum = 0u;
    return proto_crc16_ccitt_false(
        (const uint8_t *)&copy, sizeof(copy));
}

static bool collection_result_record_valid(
    const struct app_mesh_collection_result_record *record)
{
    return record != NULL &&
           record->magic == APP_MESH_COLLECTION_RESULT_RECORD_MAGIC &&
           record->version == APP_MESH_COLLECTION_RESULT_RECORD_VERSION &&
           record->size == sizeof(*record) &&
           record->checksum == collection_result_record_checksum(record) &&
           record->snapshot.version ==
               APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION &&
           record->snapshot.valid;
}

int app_mesh_persistence_save_collection_result(
    const struct app_mesh_collection_result_snapshot *snapshot)
{
    struct app_mesh_collection_result_record record;
    int ret;

    if (snapshot == NULL || !snapshot->valid ||
        snapshot->version != APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    memset(&record, 0, sizeof(record));
    record.magic = APP_MESH_COLLECTION_RESULT_RECORD_MAGIC;
    record.version = APP_MESH_COLLECTION_RESULT_RECORD_VERSION;
    record.size = sizeof(record);
    record.snapshot = *snapshot;
    record.checksum = collection_result_record_checksum(&record);
    return mesh_persistence_write(APP_MESH_NVS_COLLECTION_RESULT_ID,
                                  &record,
                                  sizeof(record),
                                  "mesh collection result snapshot");
}

int app_mesh_persistence_save_gateway_collection(
    const struct gateway_collection_state *collection)
{
    int ret;

    if (gateway_collection_state_validate(collection) != PROTO_OK) {
        LOG_WRN("gateway collection state validation failed before save");
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = gateway_collection_journal_save(&gateway_collection_journal_io,
                                          &gateway_collection_journal_cursor,
                                          collection,
                                          NULL);
    if (ret < 0) {
        LOG_WRN("gateway collection journal save failed: %d", ret);
        return ret;
    }
    return 0;
}

int app_mesh_persistence_save_gateway_eack_custody(
    const struct gateway_collection_eack_custody_snapshot *snapshot)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (gateway_collection_eack_custody_validate(snapshot) != PROTO_OK) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_EACK_CUSTODY_ID,
                                  snapshot,
                                  sizeof(*snapshot),
                                  "gateway EACK custody");
}

int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster)
{
    struct gateway_membership_snapshot snapshot;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    ret = gateway_membership_export_snapshot(roster, &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway membership snapshot export failed: %d", ret);
        return -EINVAL;
    }

    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "gateway membership snapshot");
}

int app_mesh_persistence_save_gateway_assignment_membership(
    const struct gateway_membership_roster *roster,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication)
{
    struct gateway_membership_snapshot snapshot;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = gateway_membership_export_assignment_snapshot(
        roster,
        assignment_epoch,
        table_seq,
        table_commitment,
        publication,
        &snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway assignment membership proof export failed: %d",
                ret);
        return -EINVAL;
    }
    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "gateway assignment membership proof");
}

int app_mesh_persistence_restore_collection_result(
    struct app_mesh_collection_result_snapshot *snapshot)
{
    union {
        struct app_mesh_collection_result_record record;
        struct app_mesh_collection_result_snapshot legacy;
        uint8_t raw[sizeof(struct app_mesh_collection_result_record)];
    } stored;
    ssize_t read_len;
    int ret;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    memset(&stored, 0, sizeof(stored));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_COLLECTION_RESULT_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("mesh collection result snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len ==
        sizeof(struct app_mesh_collection_result_record)) {
        if (!collection_result_record_valid(&stored.record)) {
            LOG_WRN("mesh collection result record rejected: size=%d version=%u",
                    (int)read_len,
                    stored.record.version);
            return -EINVAL;
        }
        *snapshot = stored.record.snapshot;
        return 0;
    }
    if ((size_t)read_len == sizeof(*snapshot) &&
        stored.legacy.version ==
            APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION &&
        stored.legacy.valid) {
        /*
         * Installed anchors may carry the original raw schema. Migrate it
         * immediately into the checksummed wrapper before exposing the
         * result/action owner to runtime code.
         */
        *snapshot = stored.legacy;
        ret = app_mesh_persistence_save_collection_result(snapshot);
        if (ret < 0) {
            memset(snapshot, 0, sizeof(*snapshot));
            return ret;
        }
        return 0;
    }
    {
        uint16_t version = 0u;
        bool valid = false;

        if ((size_t)read_len >= sizeof(*snapshot)) {
            version = stored.legacy.version;
            valid = stored.legacy.valid;
        }
        LOG_WRN("mesh collection result snapshot rejected: size=%d version=%u valid=%u",
                (int)read_len,
                version,
                valid ? 1u : 0u);
        /*
         * This record can own a delivered command result and a pending reboot
         * or rediscovery. Never turn corruption into apparent absence: keep
         * the bytes in place and fail closed so a repair tool can inspect
         * them and a later transient-free read cannot repeat the command.
         */
        return -EINVAL;
    }
}

int app_mesh_persistence_restore_gateway_collection(
    struct gateway_collection_state *collection)
{
    struct gateway_collection_journal_stats stats = {0};
    ssize_t read_len;
    int ret;

    if (collection == NULL) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    ret = gateway_collection_journal_restore(&gateway_collection_journal_io,
                                             &gateway_collection_journal_cursor,
                                             collection,
                                             &stats);
    if (ret < 0) {
        LOG_WRN("gateway collection journal restore failed: %d", ret);
        return ret;
    }
    if (!gateway_collection_journal_cursor.active &&
        gateway_collection_journal_cursor.generation == 0u) {
        /*
         * Schema migration: the retired format stored the complete 2048-byte
         * live state at the bank-0 key. Read it directly into the existing
         * singleton, then rewrite it through the compact journal. No second
         * collection-sized stack or static buffer is needed.
         */
        read_len = nvs_read(&mesh_nvs,
                            APP_MESH_NVS_GATEWAY_COLLECTION_ID,
                            collection,
                            sizeof(*collection));
        if (read_len == -ENOENT) {
            gateway_collection_clear(collection);
        } else if (read_len < 0) {
            /*
             * The retired monolithic key may still be the only durable owner
             * of an accepted collection. A transient NVS read failure is not
             * absence: preserve the caller's restore gate and retry rather
             * than zeroing RAM and reopening command/result admission.
             */
            LOG_WRN("legacy gateway collection read failed closed: %d",
                    (int)read_len);
            return (int)read_len;
        } else if ((size_t)read_len == sizeof(*collection) &&
                   gateway_collection_state_validate(collection) ==
                       PROTO_OK) {
            ret = gateway_collection_journal_save(
                &gateway_collection_journal_io,
                &gateway_collection_journal_cursor,
                collection,
                NULL);
            if (ret < 0) {
                LOG_WRN("legacy gateway collection restored but journal migration deferred: %d",
                        ret);
                gateway_collection_clear(collection);
                return ret;
            } else {
                LOG_INF("legacy gateway collection migrated to compact journal");
                /*
                 * Generation one commits to bank one. Remove the retired
                 * bank-zero payload so an older firmware image cannot
                 * resurrect it after a later downgrade.
                 */
                ret = nvs_delete(&mesh_nvs,
                                 APP_MESH_NVS_GATEWAY_COLLECTION_ID);
                if (ret < 0 && ret != -ENOENT) {
                    LOG_WRN("legacy gateway collection cleanup failed: %d", ret);
                }
            }
        } else {
            /*
             * A present but malformed legacy record is possible accepted
             * custody, not an empty store. Keep its bytes for repair and fail
             * closed so a reboot cannot turn corruption into a fresh round.
             */
            LOG_WRN("legacy gateway collection record malformed: len=%d",
                    (int)read_len);
            return -EBADMSG;
        }
    }
    if (!gateway_collection_journal_cursor.active) {
        if (stats.records_ignored != 0u) {
            LOG_WRN("gateway collection journal ignored %u stale or torn records",
                    stats.records_ignored);
        }
        return 0;
    }

    LOG_INF("gateway collection state restored: command_seq=%u collection=%u received=%u expected=%u open=%u",
            collection->command_seq,
            collection->collection_epoch_id,
            collection->received_count,
            collection->expected_count,
            collection->collection_open ? 1u : 0u);
    return 0;
}

int app_mesh_persistence_rollback_gateway_collection(
    struct gateway_collection_state *collection)
{
    int ret = gateway_collection_journal_rollback_uncommitted(
        &gateway_collection_journal_cursor,
        collection);

    if (ret != PROTO_OK) {
        LOG_ERR("gateway collection journal RAM rollback failed: %d", ret);
        return -EINVAL;
    }
    return 0;
}

int app_mesh_persistence_restore_gateway_eack_custody(
    struct gateway_collection_eack_custody_snapshot *snapshot)
{
    ssize_t read_len;
    int ret;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_EACK_CUSTODY_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        mesh_persistence_note_success();
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("gateway EACK custody read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot)) {
        LOG_WRN("gateway EACK custody has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(*snapshot));
        /*
         * This key is the exact owner for a potentially transmitted CLOSED
         * EACK.  Corruption cannot be interpreted as absence: deleting it
         * would let collection recovery generate a different EACK and could
         * release relay custody without proving which receipt was durable.
         * Retain the bytes for explicit repair and keep persistence health
         * unhealthy until a later read succeeds.
         */
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }

    ret = gateway_collection_eack_custody_validate(snapshot);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway EACK custody restore rejected: %d", ret);
        memset(snapshot, 0, sizeof(*snapshot));
        mesh_persistence_note_failure(-EBADMSG);
        return -EBADMSG;
    }
    mesh_persistence_note_success();
    return 0;
}

static int gateway_membership_migrate_roster_only(
    const struct gateway_membership_roster *roster,
    uint8_t legacy_version)
{
    struct gateway_membership_snapshot migrated;
    int ret = gateway_membership_export_snapshot(roster, &migrated);

    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = mesh_persistence_write(APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                                 &migrated,
                                 sizeof(migrated),
                                 "gateway legacy membership migration");
    if (ret < 0) {
        LOG_ERR("gateway membership v%u roster-only migration failed: %d",
                legacy_version,
                ret);
        return ret;
    }
    LOG_WRN("gateway membership v%u migrated without 32-bit assignment proof or publication debt",
            legacy_version);
    return 0;
}

int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster,
    bool *publication_pending)
{
    union gateway_membership_stored_snapshot stored;
    struct gateway_membership_publication publication;
    ssize_t read_len;
    bool has_publication = false;
    int ret;

    if (roster == NULL) {
        return -EINVAL;
    }
    if (publication_pending != NULL) {
        *publication_pending = false;
    }
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        gateway_membership_clear(roster);
        return -ENOTSUP;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    gateway_membership_clear(roster);
    memset(&stored, 0, sizeof(stored));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        LOG_WRN("gateway membership snapshot read failed: %d", (int)read_len);
        return (int)read_len;
    }
    if ((size_t)read_len == sizeof(struct gateway_membership_snapshot_v1)) {
        const struct gateway_membership_snapshot_v1 *legacy =
            (const struct gateway_membership_snapshot_v1 *)stored.raw;

        if (gateway_membership_restore_v1(legacy, roster) < 0) {
            LOG_WRN("gateway membership v1 snapshot restore rejected");
            gateway_membership_clear(roster);
            return -EINVAL;
        }
        LOG_WRN("gateway membership v1 restored without assignment proof: epoch=%u nodes=%u",
                roster->membership_epoch,
                roster->node_count);
        ret = gateway_membership_migrate_roster_only(roster, 1u);
        if (ret < 0) {
            gateway_membership_clear(roster);
        }
        return ret;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v2)) {
        const struct gateway_membership_snapshot_v2 *legacy =
            (const struct gateway_membership_snapshot_v2 *)stored.raw;

        ret = gateway_membership_restore_v2(legacy, roster);
        if (ret < 0) {
            LOG_WRN("gateway membership v2 snapshot restore rejected");
            gateway_membership_clear(roster);
            return ret;
        }
        ret = gateway_membership_migrate_roster_only(roster, 2u);
        if (ret < 0) {
            gateway_membership_clear(roster);
        }
        return ret;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v3)) {
        const struct gateway_membership_snapshot_v3 *legacy =
            &stored.legacy_v3;

        ret = gateway_membership_restore_v3(legacy, roster);
        if (ret < 0) {
            LOG_WRN("gateway membership v3 snapshot restore rejected");
            gateway_membership_clear(roster);
            return ret;
        }
        ret = gateway_membership_migrate_roster_only(roster, 3u);
        if (ret < 0) {
            gateway_membership_clear(roster);
        }
        return ret;
    }
    if ((size_t)read_len != sizeof(stored.current)) {
        LOG_WRN("gateway membership snapshot has wrong size: %d/%u",
                (int)read_len,
                (unsigned int)sizeof(stored.current));
        return -EINVAL;
    }

    ret = gateway_membership_restore_snapshot(roster, &stored.current);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway membership snapshot restore rejected: %d", ret);
        return -EINVAL;
    }
    ret = gateway_membership_snapshot_get_publication(
        &stored.current, &publication);
    if (ret == PROTO_OK) {
        has_publication = true;
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        gateway_membership_clear(roster);
        return -EINVAL;
    }
    if (publication_pending != NULL) {
        *publication_pending = has_publication;
    }

    LOG_INF("gateway membership snapshot restored: epoch=%u nodes=%u publication_pending=%u",
            roster->membership_epoch,
            roster->node_count,
            has_publication);
    return 0;
}

int app_mesh_persistence_restore_gateway_assignment_publication(
    struct gateway_membership_publication *publication,
    uint32_t *assignment_epoch,
    uint32_t *table_seq,
    struct discovery_assignment_table_commitment *table_commitment)
{
    union gateway_membership_stored_snapshot stored;
    ssize_t read_len;
    int ret;

    if (publication == NULL || assignment_epoch == NULL ||
        table_seq == NULL || table_commitment == NULL) {
        return -EINVAL;
    }
    memset(publication, 0, sizeof(*publication));
    *assignment_epoch = 0u;
    *table_seq = 0u;
    memset(table_commitment, 0, sizeof(*table_commitment));
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    memset(&stored, 0, sizeof(stored));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT ||
        (size_t)read_len ==
            sizeof(struct gateway_membership_snapshot_v1) ||
        (size_t)read_len ==
            sizeof(struct gateway_membership_snapshot_v2) ||
        (size_t)read_len ==
            sizeof(struct gateway_membership_snapshot_v3)) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(stored.current)) {
        return -EINVAL;
    }
    ret = gateway_membership_snapshot_get_publication(
        &stored.current, publication);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK) {
        memset(publication, 0, sizeof(*publication));
        return -EINVAL;
    }

    *assignment_epoch = stored.current.assignment_epoch;
    *table_seq = stored.current.assignment_table_seq;
    *table_commitment = stored.current.assignment_table_commitment;
    return 1;
}

int app_mesh_persistence_complete_gateway_assignment_publication(
    uint32_t assignment_epoch,
    uint16_t event_gateway_epoch,
    uint32_t host_session_id,
    uint16_t host_seq)
{
    union gateway_membership_stored_snapshot stored;
    struct gateway_membership_publication publication;
    struct gateway_membership_roster roster;
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t table_seq;
    ssize_t read_len;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (assignment_epoch == 0u) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    memset(&stored, 0, sizeof(stored));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT) {
        return -ENOENT;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(stored.current) ||
        gateway_membership_restore_snapshot(&roster, &stored.current) !=
            PROTO_OK ||
        stored.current.assignment_proof_valid == 0u ||
        stored.current.assignment_epoch != assignment_epoch) {
        return -ESTALE;
    }

    ret = gateway_membership_snapshot_get_publication(
        &stored.current, &publication);
    if (ret == PROTO_ERR_NOT_FOUND) {
        /* Retrying after the matching completion write is idempotent. */
        return 0;
    }
    if (ret != PROTO_OK ||
        publication.event_gateway_epoch != event_gateway_epoch ||
        publication.host_command.session_id != host_session_id ||
        publication.host_command.seq != host_seq) {
        return -ESTALE;
    }

    /*
     * Reuse the already validated 896-byte read buffer for the completed
     * snapshot.  The exporter clears its output first, so retain the two proof
     * fields that must survive that in-place rewrite.
     */
    table_seq = stored.current.assignment_table_seq;
    table_commitment = stored.current.assignment_table_commitment;
    ret = gateway_membership_export_assignment_snapshot(
        &roster,
        assignment_epoch,
        table_seq,
        &table_commitment,
        NULL,
        &stored.current);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    return mesh_persistence_write(
        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
        &stored.current,
        sizeof(stored.current),
        "gateway assignment publication completion");
}

int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    struct app_mesh_discovery_assignment_snapshot stored;
    int ret;

    if (snapshot == NULL ||
        snapshot->version != APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION ||
        !discovery_assignment_snapshot_logical_valid(snapshot, true)) {
        return -EINVAL;
    }
    discovery_assignment_snapshot_copy_logical(&stored, snapshot);
    discovery_assignment_snapshot_finalize(&stored);
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    return mesh_persistence_write(APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID,
                                  &stored,
                                  sizeof(stored),
                                  "discovery assignment snapshot");
}

static int discovery_assignment_retire_legacy_snapshot(
    struct app_mesh_discovery_assignment_snapshot *snapshot,
    uint8_t legacy_version)
{
    int ret = app_mesh_persistence_clear_discovery_assignment_checked();

    if (ret < 0) {
        LOG_ERR("legacy discovery assignment v%u retirement failed: %d",
                legacy_version,
                ret);
        return ret;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    LOG_WRN("legacy discovery assignment v%u retired; fresh schema-2 TABLE required",
            legacy_version);
    return 0;
}

int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    uint8_t stored[sizeof(*snapshot)] = {0};
    ssize_t read_len;
    int ret;

    if (snapshot == NULL) {
        return -EINVAL;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&discovery_assignment_read_error,
                                    &discovery_assignment_read_failures)) {
        read_len = discovery_assignment_read_error;
    } else
#endif
    {
        read_len = nvs_read(&mesh_nvs,
                            APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID,
                            stored,
                            sizeof(stored));
    }
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }

    if ((size_t)read_len ==
            sizeof(struct app_mesh_discovery_assignment_snapshot_v2)) {
        struct app_mesh_discovery_assignment_snapshot_v2 legacy;

        memcpy(&legacy, stored, sizeof(legacy));
        if (legacy.version != 2u ||
            !discovery_assignment_snapshot_import_legacy(
                snapshot,
                legacy.epoch,
                legacy.table_command_seq,
                legacy.table_fingerprint,
                legacy.local_id,
                legacy.gateway_id,
                legacy.slot,
                legacy.slot_count,
                legacy.provisioned,
                legacy.valid,
                NULL,
                0u)) {
            memset(snapshot, 0, sizeof(*snapshot));
            return -EINVAL;
        }
        return discovery_assignment_retire_legacy_snapshot(snapshot, 2u);
    }

    if ((size_t)read_len ==
            sizeof(struct app_mesh_discovery_assignment_snapshot_v3)) {
        struct app_mesh_discovery_assignment_snapshot_v3 legacy_v3;

        memcpy(&legacy_v3, stored, sizeof(legacy_v3));
        if (legacy_v3.version == 3u) {
            if (!discovery_assignment_snapshot_import_legacy(
                    snapshot,
                    legacy_v3.epoch,
                    legacy_v3.table_command_seq,
                    legacy_v3.table_fingerprint,
                    legacy_v3.local_id,
                    legacy_v3.gateway_id,
                    legacy_v3.slot,
                    legacy_v3.slot_count,
                    legacy_v3.provisioned,
                    legacy_v3.valid,
                    legacy_v3.retired_epochs,
                    legacy_v3.retired_epoch_count)) {
                memset(snapshot, 0, sizeof(*snapshot));
                return -EINVAL;
            }
            return discovery_assignment_retire_legacy_snapshot(snapshot, 3u);
        }
        if (legacy_v3.version == 4u) {
            struct app_mesh_discovery_assignment_snapshot_v4 legacy_v4;

            memcpy(&legacy_v4, stored, sizeof(legacy_v4));
            if (legacy_v4.ordered_epoch_valid > 1u ||
                !discovery_assignment_snapshot_import_legacy(
                    snapshot,
                    legacy_v4.epoch,
                    legacy_v4.table_command_seq,
                    legacy_v4.table_fingerprint,
                    legacy_v4.local_id,
                    legacy_v4.gateway_id,
                    legacy_v4.slot,
                    legacy_v4.slot_count,
                    legacy_v4.provisioned,
                    legacy_v4.valid,
                    legacy_v4.retired_epochs,
                    legacy_v4.retired_epoch_count)) {
                memset(snapshot, 0, sizeof(*snapshot));
                return -EINVAL;
            }
            return discovery_assignment_retire_legacy_snapshot(snapshot, 4u);
        }
        return -EINVAL;
    }

    if ((size_t)read_len == sizeof(*snapshot)) {
        memcpy(snapshot, stored, sizeof(*snapshot));
        if (discovery_assignment_snapshot_v8_valid(snapshot)) {
            return 0;
        }
    }
    if ((size_t)read_len ==
            sizeof(struct app_mesh_discovery_assignment_snapshot_v7)) {
        struct app_mesh_discovery_assignment_snapshot_v7 legacy_v7;

        memcpy(&legacy_v7, stored, sizeof(legacy_v7));
        if (discovery_assignment_snapshot_v7_valid(&legacy_v7)) {
            return discovery_assignment_retire_legacy_snapshot(snapshot, 7u);
        }
    }
    if ((size_t)read_len ==
            sizeof(struct app_mesh_discovery_assignment_snapshot_v6)) {
        struct app_mesh_discovery_assignment_snapshot_v6 legacy_v6;
        struct app_mesh_discovery_assignment_snapshot_v5 legacy_v5;

        memcpy(&legacy_v6, stored, sizeof(legacy_v6));
        if (discovery_assignment_snapshot_import_v6(snapshot, &legacy_v6)) {
            return discovery_assignment_retire_legacy_snapshot(snapshot, 6u);
        }
        memcpy(&legacy_v5, stored, sizeof(legacy_v5));
        if (discovery_assignment_snapshot_import_v5(snapshot, &legacy_v5)) {
            return discovery_assignment_retire_legacy_snapshot(snapshot, 5u);
        }
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return -EINVAL;
}

int app_mesh_persistence_clear_discovery_assignment_checked(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
#if defined(CONFIG_ZTEST)
    if (deferred_test_consume_fault(&discovery_assignment_delete_error,
                                    &discovery_assignment_delete_failures)) {
        mesh_persistence_note_failure(discovery_assignment_delete_error);
        return discovery_assignment_delete_error;
    }
#endif
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID);
    if (ret == -ENOENT) {
        return 0;
    }
    if (ret < 0) {
        mesh_persistence_note_failure(ret);
    }
    return ret;
}

void app_mesh_persistence_clear_discovery_assignment(void)
{
    (void)app_mesh_persistence_clear_discovery_assignment_checked();
}

int app_mesh_persistence_save_gateway_assignment_epoch(uint32_t epoch)
{
    struct app_mesh_gateway_assignment_epoch_snapshot snapshot = {
        .magic = APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_MAGIC,
        .epoch = epoch,
        .version = APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .valid = 1u,
    };
    int ret;

    if (epoch == 0u) {
        return -EINVAL;
    }
    snapshot.checksum =
        gateway_assignment_epoch_snapshot_checksum(&snapshot);
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    return mesh_persistence_write(APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "gateway assignment epoch");
}

int app_mesh_persistence_restore_gateway_assignment_epoch(uint32_t *epoch)
{
    struct app_mesh_gateway_assignment_epoch_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (epoch == NULL) {
        return -EINVAL;
    }
    *epoch = 0u;
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot) ||
        !gateway_assignment_epoch_snapshot_valid(&snapshot)) {
        return -EINVAL;
    }
    *epoch = snapshot.epoch;
    return 0;
}

int app_mesh_persistence_restore_gateway_assignment_baseline(
    uint32_t *assignment_epoch)
{
    union gateway_membership_stored_snapshot stored;
    struct gateway_membership_roster restored;
    ssize_t read_len;
    int ret;

    if (assignment_epoch == NULL) {
        return -EINVAL;
    }
    *assignment_epoch = 0u;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }

    memset(&stored, 0, sizeof(stored));
    gateway_membership_clear(&restored);
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        stored.raw,
                        sizeof(stored.raw));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v1)) {
        ret = gateway_membership_restore_v1(
            (const struct gateway_membership_snapshot_v1 *)stored.raw,
            &restored);
        return ret < 0 ? ret : 0;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v2)) {
        const struct gateway_membership_snapshot_v2 *legacy =
            (const struct gateway_membership_snapshot_v2 *)stored.raw;

        ret = gateway_membership_restore_v2(legacy, &restored);
        if (ret < 0) {
            return ret;
        }
        return 0;
    }
    if ((size_t)read_len ==
        sizeof(struct gateway_membership_snapshot_v3)) {
        ret = gateway_membership_restore_v3(
            &stored.legacy_v3,
            &restored);
        return ret < 0 ? ret : 0;
    }
    if ((size_t)read_len != sizeof(stored.current) ||
        gateway_membership_restore_snapshot(&restored, &stored.current) !=
            PROTO_OK) {
        return -EINVAL;
    }
    if (stored.current.assignment_proof_valid == 0u) {
        return 0;
    }

    *assignment_epoch = stored.current.assignment_epoch;
    return 1;
}

int app_mesh_persistence_reconcile_gateway_assignment_epoch(uint32_t *epoch)
{
    uint32_t cursor_epoch = 0u;
    uint32_t proof_epoch = 0u;
    uint32_t resolved_epoch = 0u;
    bool repair_required = false;
    int ret;

    if (epoch == NULL) {
        return -EINVAL;
    }
    *epoch = 0u;
    ret = app_mesh_persistence_restore_gateway_assignment_epoch(
        &cursor_epoch);
    if (ret < 0) {
        return ret;
    }
    ret = app_mesh_persistence_restore_gateway_assignment_baseline(
        &proof_epoch);
    if (ret < 0) {
        return ret;
    }
    ret = discovery_assignment_reconcile_epoch_baseline(
        cursor_epoch,
        proof_epoch,
        &resolved_epoch,
        &repair_required);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_STALE ? -ESTALE : -EINVAL;
    }
    if (repair_required) {
        ret = app_mesh_persistence_save_gateway_assignment_epoch(
            resolved_epoch);
        if (ret < 0) {
            return ret;
        }
    }

    *epoch = resolved_epoch;
    return 0;
}

int app_mesh_persistence_reserve_gateway_command_sequences(
    uint32_t count,
    uint32_t *first_sequence)
{
    struct app_mesh_gateway_command_sequence_snapshot snapshot = {0};
    uint32_t previous = 0u;
    ssize_t read_len;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || count == 0u ||
        count >= UINT32_C(0x80000000) || first_sequence == NULL ||
        k_is_in_isr()) {
        return -EINVAL;
    }
    *first_sequence = 0u;
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = k_mutex_lock(&gateway_control_sequence_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_COMMAND_SEQUENCE_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        memset(&snapshot, 0, sizeof(snapshot));
    } else if (read_len < 0) {
        ret = (int)read_len;
        goto out;
    } else if ((size_t)read_len != sizeof(snapshot) ||
               !gateway_command_sequence_snapshot_valid(&snapshot)) {
        ret = -EINVAL;
        goto out;
    } else {
        previous = snapshot.reserved_through;
    }

    snapshot = (struct app_mesh_gateway_command_sequence_snapshot) {
        .magic = APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_MAGIC,
        .reserved_through =
            gateway_command_sequence_advance(previous, count),
        .version = APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .valid = 1u,
    };
    snapshot.checksum =
        gateway_command_sequence_snapshot_checksum(&snapshot);
    ret = mesh_persistence_write(
        APP_MESH_NVS_GATEWAY_COMMAND_SEQUENCE_ID,
        &snapshot,
        sizeof(snapshot),
        "gateway command sequence");
    if (ret < 0) {
        goto out;
    }
    *first_sequence = previous == UINT32_MAX ? 1u : previous + 1u;
out:
    k_mutex_unlock(&gateway_control_sequence_mutex);
    return ret;
}

static int survey_generation_snapshot_read(
    uint8_t role,
    uint64_t local_id,
    uint64_t gateway_id,
    struct app_mesh_survey_generation_snapshot *snapshot)
{
    ssize_t read_len;

    memset(snapshot, 0, sizeof(*snapshot));
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_SURVEY_GENERATION_ID,
                        snapshot,
                        sizeof(*snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(*snapshot) ||
        !survey_generation_snapshot_valid(snapshot,
                                          role,
                                          local_id,
                                          gateway_id)) {
        return -EINVAL;
    }
    return 1;
}

static int survey_generation_snapshot_write(
    uint8_t role,
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t generation)
{
    struct app_mesh_survey_generation_snapshot snapshot = {
        .magic = APP_MESH_SURVEY_GENERATION_SNAPSHOT_MAGIC,
        .local_id = local_id,
        .gateway_id = gateway_id,
        .generation = generation,
        .version = APP_MESH_SURVEY_GENERATION_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .role = role,
        .valid = 1u,
    };

    snapshot.checksum = survey_generation_snapshot_checksum(&snapshot);
    return mesh_persistence_write(APP_MESH_NVS_SURVEY_GENERATION_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "survey generation");
}

int app_mesh_persistence_reserve_gateway_survey_generation(
    uint64_t gateway_id,
    uint64_t *generation)
{
    struct app_mesh_survey_generation_snapshot snapshot;
    uint64_t next_generation;
    int found;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || gateway_id == 0u ||
        generation == NULL || k_is_in_isr()) {
        return -EINVAL;
    }
    *generation = 0u;
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = k_mutex_lock(&survey_generation_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    found = survey_generation_snapshot_read(ROLE_GATEWAY,
                                            gateway_id,
                                            gateway_id,
                                            &snapshot);
    if (found < 0) {
        ret = found;
        goto out;
    }
    if (found == 0) {
        next_generation = 1u;
    } else {
        if (snapshot.generation == UINT64_MAX) {
            ret = -EOVERFLOW;
            goto out;
        }
        next_generation = snapshot.generation + 1u;
        if ((uint32_t)next_generation == 0u) {
            next_generation++;
        }
    }
    ret = survey_generation_snapshot_write(ROLE_GATEWAY,
                                           gateway_id,
                                           gateway_id,
                                           next_generation);
    if (ret == 0) {
        *generation = next_generation;
    }
out:
    k_mutex_unlock(&survey_generation_mutex);
    return ret;
}

int app_mesh_persistence_restore_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t *generation)
{
    struct app_mesh_survey_generation_snapshot snapshot;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || local_id == 0u ||
        gateway_id == 0u || generation == NULL || k_is_in_isr()) {
        return -EINVAL;
    }
    *generation = 0u;
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = k_mutex_lock(&survey_generation_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    ret = survey_generation_snapshot_read(ROLE_ANCHOR,
                                          local_id,
                                          gateway_id,
                                          &snapshot);
    if (ret == 1) {
        *generation = snapshot.generation;
    }
    k_mutex_unlock(&survey_generation_mutex);
    return ret;
}

int app_mesh_persistence_advance_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t generation)
{
    struct app_mesh_survey_generation_snapshot snapshot;
    int found;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || local_id == 0u ||
        gateway_id == 0u || generation == 0u ||
        (uint32_t)generation == 0u || k_is_in_isr()) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    ret = k_mutex_lock(&survey_generation_mutex, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    found = survey_generation_snapshot_read(ROLE_ANCHOR,
                                            local_id,
                                            gateway_id,
                                            &snapshot);
    if (found < 0) {
        ret = found;
        goto out;
    }
    if (found == 1 && generation < snapshot.generation) {
        ret = -ESTALE;
        goto out;
    }
    if (found == 1 && generation == snapshot.generation) {
        ret = 0;
        goto out;
    }
    ret = survey_generation_snapshot_write(ROLE_ANCHOR,
                                           local_id,
                                           gateway_id,
                                           generation);
out:
    k_mutex_unlock(&survey_generation_mutex);
    return ret;
}

int app_mesh_persistence_save_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    const struct gateway_command_rx_duplicate_cache *replay)
{
    struct app_mesh_anchor_command_replay_snapshot snapshot = {
        .magic = APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_MAGIC,
        .local_id = local_id,
        .gateway_id = gateway_id,
        .version = APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .valid = 1u,
    };
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || local_id == 0u || gateway_id == 0u ||
        replay == NULL || !replay->initialized ||
        replay->newest_command_seq == 0u || replay->committed == 0u) {
        return -EINVAL;
    }
    snapshot.replay = *replay;
    snapshot.checksum = anchor_command_replay_snapshot_checksum(&snapshot);
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    return mesh_persistence_write(APP_MESH_NVS_ANCHOR_COMMAND_REPLAY_ID,
                                  &snapshot,
                                  sizeof(snapshot),
                                  "anchor command replay");
}

int app_mesh_persistence_restore_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    struct gateway_command_rx_duplicate_cache *replay)
{
    struct app_mesh_anchor_command_replay_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || local_id == 0u || gateway_id == 0u ||
        replay == NULL) {
        return -EINVAL;
    }
    memset(replay, 0, sizeof(*replay));
    ret = app_mesh_persistence_init();
    if (ret < 0) {
        return ret;
    }
    read_len = nvs_read(&mesh_nvs,
                        APP_MESH_NVS_ANCHOR_COMMAND_REPLAY_ID,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot) ||
        !anchor_command_replay_snapshot_valid(
            &snapshot, local_id, gateway_id)) {
        return -EINVAL;
    }
    *replay = snapshot.replay;
    /*
     * Early schema-1 records used committed as an out-of-order bitset.  Its
     * byte layout remains valid, but every nonzero legacy value now denotes
     * one strict committed high watermark so a hole below it fails closed.
     */
    replay->committed = GATEWAY_COMMAND_RX_PERSISTED_MARKER;
    return 1;
}

#if defined(CONFIG_ZTEST)
void app_mesh_persistence_test_reset_faults(void)
{
    deferred_test_read_error = 0;
    deferred_test_read_failures = 0u;
    deferred_test_write_error = 0;
    deferred_test_write_failures = 0u;
    deferred_test_delete_error = 0;
    deferred_test_delete_failures = 0u;
    outbox_test_write_error = 0;
    outbox_test_write_failures = 0u;
    outbox_test_delete_error = 0;
    outbox_test_delete_failures = 0u;
    gateway_eack_custody_delete_error = 0;
    gateway_eack_custody_delete_failures = 0u;
    gateway_click_payload_write_error = 0;
    gateway_click_payload_write_failures = 0u;
    gateway_click_metadata_write_error = 0;
    gateway_click_metadata_write_failures = 0u;
    gateway_click_metadata_read_error = 0;
    gateway_click_metadata_read_failures = 0u;
    gateway_click_metadata_verify_error = 0;
    gateway_click_metadata_verify_failures = 0u;
    gateway_click_payload_read_error = 0;
    gateway_click_payload_read_failures = 0u;
    gateway_click_delete_error = 0;
    gateway_click_delete_failures = 0u;
    gateway_click_metadata_delete_error = 0;
    gateway_click_metadata_delete_failures = 0u;
    gateway_click_payload_delete_error = 0;
    gateway_click_payload_delete_failures = 0u;
    gateway_assignment_epoch_write_error = 0;
    gateway_assignment_epoch_write_failures = 0u;
    gateway_membership_write_error = 0;
    gateway_membership_write_failures = 0u;
    discovery_assignment_read_error = 0;
    discovery_assignment_read_failures = 0u;
    discovery_assignment_delete_error = 0;
    discovery_assignment_delete_failures = 0u;
    collection_result_delete_error = 0;
    collection_result_delete_failures = 0u;
    child_custody_delete_error = 0;
    child_custody_delete_failures = 0u;
    anchor_range_fragment_write_error = 0;
    anchor_range_fragment_write_failures = 0u;
    anchor_range_control_write_error = 0;
    anchor_range_control_write_failures = 0u;
    anchor_range_fragment_read_error = 0;
    anchor_range_fragment_read_failures = 0u;
    anchor_range_control_readback_error = 0;
    anchor_range_control_readback_failures = 0u;
    anchor_range_control_delete_error = 0;
    anchor_range_control_delete_failures = 0u;
    gateway_collection_receipt_read_error = 0;
    gateway_collection_receipt_read_failures = 0u;
    gateway_collection_receipt_write_error = 0;
    gateway_collection_receipt_write_failures = 0u;
    gateway_terminal_receipt_read_error = 0;
    gateway_terminal_receipt_read_failures = 0u;
    gateway_terminal_receipt_write_error = 0;
    gateway_terminal_receipt_write_failures = 0u;
    gateway_terminal_receipt_delete_error = 0;
    gateway_terminal_receipt_delete_failures = 0u;
}

void app_mesh_persistence_test_reset_deferred_presence(void)
{
    atomic_set(&deferred_outbox_presence, -1);
}

void app_mesh_persistence_test_set_deferred_busy(bool busy)
{
    atomic_set(&deferred_outbox_busy, busy ? 1 : 0);
}

void app_mesh_persistence_test_fail_deferred_read(int error, uint8_t count)
{
    deferred_test_read_error = error;
    deferred_test_read_failures = count;
}

void app_mesh_persistence_test_fail_deferred_write(int error, uint8_t count)
{
    deferred_test_write_error = error;
    deferred_test_write_failures = count;
}

void app_mesh_persistence_test_fail_deferred_delete(int error, uint8_t count)
{
    deferred_test_delete_error = error;
    deferred_test_delete_failures = count;
}

void app_mesh_persistence_test_fail_outbox_write(int error, uint8_t count)
{
    outbox_test_write_error = error;
    outbox_test_write_failures = count;
}

void app_mesh_persistence_test_fail_outbox_delete(int error, uint8_t count)
{
    outbox_test_delete_error = error;
    outbox_test_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_eack_custody_delete(
    int error,
    uint8_t count)
{
    gateway_eack_custody_delete_error = error;
    gateway_eack_custody_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_membership_write(int error,
                                                             uint8_t count)
{
    gateway_membership_write_error = error;
    gateway_membership_write_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_payload_write(int error,
                                                                uint8_t count)
{
    gateway_click_payload_write_error = error;
    gateway_click_payload_write_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_metadata_write(int error,
                                                                 uint8_t count)
{
    gateway_click_metadata_write_error = error;
    gateway_click_metadata_write_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_metadata_read(int error,
                                                                uint8_t count)
{
    gateway_click_metadata_read_error = error;
    gateway_click_metadata_read_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_metadata_verify(int error,
                                                                  uint8_t count)
{
    gateway_click_metadata_verify_error = error;
    gateway_click_metadata_verify_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_payload_read(int error,
                                                               uint8_t count)
{
    gateway_click_payload_read_error = error;
    gateway_click_payload_read_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_delete(int error,
                                                         uint8_t count)
{
    gateway_click_delete_error = error;
    gateway_click_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_metadata_delete(int error,
                                                                  uint8_t count)
{
    gateway_click_metadata_delete_error = error;
    gateway_click_metadata_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_click_payload_delete(int error,
                                                                 uint8_t count)
{
    gateway_click_payload_delete_error = error;
    gateway_click_payload_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_assignment_epoch_write(int error,
                                                                   uint8_t count)
{
    gateway_assignment_epoch_write_error = error;
    gateway_assignment_epoch_write_failures = count;
}

void app_mesh_persistence_test_fail_discovery_assignment_read(int error,
                                                              uint8_t count)
{
    discovery_assignment_read_error = error;
    discovery_assignment_read_failures = count;
}

void app_mesh_persistence_test_fail_discovery_assignment_delete(int error,
                                                                uint8_t count)
{
    discovery_assignment_delete_error = error;
    discovery_assignment_delete_failures = count;
}

void app_mesh_persistence_test_fail_collection_result_delete(int error,
                                                              uint8_t count)
{
    collection_result_delete_error = error;
    collection_result_delete_failures = count;
}

void app_mesh_persistence_test_fail_child_custody_delete(int error,
                                                         uint8_t count)
{
    child_custody_delete_error = error;
    child_custody_delete_failures = count;
}

void app_mesh_persistence_test_fail_anchor_range_fragment_write(int error,
                                                                uint8_t count)
{
    anchor_range_fragment_write_error = error;
    anchor_range_fragment_write_failures = count;
}

void app_mesh_persistence_test_fail_anchor_range_control_write(int error,
                                                               uint8_t count)
{
    anchor_range_control_write_error = error;
    anchor_range_control_write_failures = count;
}

void app_mesh_persistence_test_fail_anchor_range_fragment_read(int error,
                                                               uint8_t count)
{
    anchor_range_fragment_read_error = error;
    anchor_range_fragment_read_failures = count;
}

void app_mesh_persistence_test_fail_anchor_range_control_readback(int error,
                                                                 uint8_t count)
{
    anchor_range_control_readback_error = error;
    anchor_range_control_readback_failures = count;
}

void app_mesh_persistence_test_fail_anchor_range_control_delete(int error,
                                                               uint8_t count)
{
    anchor_range_control_delete_error = error;
    anchor_range_control_delete_failures = count;
}

void app_mesh_persistence_test_fail_gateway_collection_receipt_read(
    int error,
    uint8_t count)
{
    gateway_collection_receipt_read_error = error;
    gateway_collection_receipt_read_failures = count;
}

void app_mesh_persistence_test_fail_gateway_collection_receipt_write(
    int error,
    uint8_t count)
{
    gateway_collection_receipt_write_error = error;
    gateway_collection_receipt_write_failures = count;
}

void app_mesh_persistence_test_fail_gateway_terminal_receipt_read(
    int error,
    uint8_t count)
{
    gateway_terminal_receipt_read_error = error;
    gateway_terminal_receipt_read_failures = count;
}

void app_mesh_persistence_test_fail_gateway_terminal_receipt_write(
    int error,
    uint8_t count)
{
    gateway_terminal_receipt_write_error = error;
    gateway_terminal_receipt_write_failures = count;
}

void app_mesh_persistence_test_fail_gateway_terminal_receipt_delete(
    int error,
    uint8_t count)
{
    gateway_terminal_receipt_delete_error = error;
    gateway_terminal_receipt_delete_failures = count;
}

int app_mesh_persistence_test_write_assignment_snapshot(
    const void *snapshot,
    size_t snapshot_len)
{
    ssize_t written;

    if (snapshot == NULL && snapshot_len != 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_DISCOVERY_ASSIGNMENT_ID,
                        snapshot,
                        snapshot_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_write_gateway_assignment_epoch_snapshot(
    const void *snapshot,
    size_t snapshot_len)
{
    ssize_t written;

    if (snapshot == NULL && snapshot_len != 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID,
                        snapshot,
                        snapshot_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_delete_gateway_assignment_epoch(void)
{
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    return nvs_delete(&mesh_nvs,
                      APP_MESH_NVS_GATEWAY_ASSIGNMENT_EPOCH_ID);
}

int app_mesh_persistence_test_delete_gateway_click_payload(void)
{
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    return nvs_delete(&mesh_nvs, APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID);
}

int app_mesh_persistence_test_write_gateway_click_payload(const void *payload,
                                                          size_t payload_len)
{
    ssize_t written;

    if (payload == NULL && payload_len != 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_CLICK_PAYLOAD_ID,
                        payload,
                        payload_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, payload_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_write_collection_result_raw(
    const void *snapshot,
    size_t snapshot_len)
{
    ssize_t written;

    if (snapshot == NULL || snapshot_len == 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_COLLECTION_RESULT_ID,
                        snapshot,
                        snapshot_len);
    return mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : (written < 0 ? (int)written : -EIO);
}

static int app_mesh_persistence_test_write_raw(uint16_t id,
                                               const void *snapshot,
                                               size_t snapshot_len)
{
    ssize_t written;

    if (snapshot == NULL || snapshot_len == 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(&mesh_nvs, id, snapshot, snapshot_len);
    return mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : (written < 0 ? (int)written : -EIO);
}

int app_mesh_persistence_test_write_outbox_raw(const void *snapshot,
                                               size_t snapshot_len)
{
    return app_mesh_persistence_test_write_raw(
        APP_MESH_NVS_OUTBOX_ID, snapshot, snapshot_len);
}

int app_mesh_persistence_test_write_child_custody_raw(
    const void *snapshot,
    size_t snapshot_len)
{
    return app_mesh_persistence_test_write_raw(
        APP_MESH_NVS_CHILD_CUSTODY_ID, snapshot, snapshot_len);
}

int app_mesh_persistence_test_write_gateway_eack_custody_raw(
    const void *snapshot,
    size_t snapshot_len)
{
    return app_mesh_persistence_test_write_raw(
        APP_MESH_NVS_GATEWAY_EACK_CUSTODY_ID, snapshot, snapshot_len);
}

int app_mesh_persistence_test_write_deferred_outbox_snapshot(
    const void *snapshot,
    size_t snapshot_len)
{
    ssize_t written;
    int ret;

    if (snapshot == NULL && snapshot_len != 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    if (!deferred_outbox_try_lock()) {
        return -EBUSY;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_DEFERRED_OUTBOX_ID,
                        snapshot,
                        snapshot_len);
    if (written < 0) {
        deferred_outbox_unlock();
        return (int)written;
    }
    if (mesh_persistence_nvs_write_succeeded(written, snapshot_len)) {
        atomic_set(&deferred_outbox_presence, 1);
    }
    ret = mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : -EIO;
    deferred_outbox_unlock();
    return ret;
}

int app_mesh_persistence_test_write_gateway_membership_snapshot(
    const void *snapshot,
    size_t snapshot_len)
{
    ssize_t written;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (snapshot == NULL && snapshot_len != 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }

    written = nvs_write(&mesh_nvs,
                        APP_MESH_NVS_GATEWAY_MEMBERSHIP_ID,
                        snapshot,
                        snapshot_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, snapshot_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_write_gateway_collection_receipt_raw(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    ssize_t written;

    if (slot >= APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES ||
        data == NULL || data_len == 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(
        &mesh_nvs,
        gateway_collection_receipt_nvs_id(slot),
        data,
        data_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, data_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_delete_gateway_collection_receipt(uint8_t slot)
{
    int ret;

    if (slot >= APP_GATEWAY_COLLECTION_RECEIPT_MAX_NODES) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    ret = nvs_delete(&mesh_nvs, gateway_collection_receipt_nvs_id(slot));
    return ret == -ENOENT ? 0 : ret;
}

int app_mesh_persistence_test_write_gateway_terminal_receipt_raw(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    ssize_t written;

    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY ||
        data == NULL || data_len == 0u) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    written = nvs_write(
        &mesh_nvs,
        gateway_terminal_receipt_nvs_id(slot),
        data,
        data_len);
    if (written < 0) {
        return (int)written;
    }
    return mesh_persistence_nvs_write_succeeded(written, data_len) ?
        0 : -EIO;
}

int app_mesh_persistence_test_delete_gateway_terminal_receipt(uint8_t slot)
{
    int ret;

    if (slot >= APP_GATEWAY_TERMINAL_RECEIPT_CAPACITY) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    ret = nvs_delete(&mesh_nvs, gateway_terminal_receipt_nvs_id(slot));
    return ret == -ENOENT ? 0 : ret;
}

int app_mesh_persistence_test_write_survey_generation_snapshot(
    const void *snapshot,
    size_t snapshot_len)
{
    return app_mesh_persistence_test_write_raw(
        APP_MESH_NVS_SURVEY_GENERATION_ID, snapshot, snapshot_len);
}

int app_mesh_persistence_test_delete_survey_generation_snapshot(void)
{
    int ret;

    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    ret = nvs_delete(&mesh_nvs, APP_MESH_NVS_SURVEY_GENERATION_ID);
    return ret == -ENOENT ? 0 : ret;
}

int app_mesh_persistence_test_write_survey_pair_result_delivery_raw(
    uint8_t slot,
    const void *snapshot,
    size_t snapshot_len)
{
    if (slot >= APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS) {
        return -EINVAL;
    }
    return app_mesh_persistence_test_write_raw(
        survey_pair_result_delivery_nvs_id(slot),
        snapshot,
        snapshot_len);
}

int app_mesh_persistence_test_delete_survey_pair_result_delivery(uint8_t slot)
{
    int ret;

    if (slot >= APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS) {
        return -EINVAL;
    }
    if (!mesh_persistence_ready()) {
        return -ENODEV;
    }
    ret = nvs_delete(
        &mesh_nvs, survey_pair_result_delivery_nvs_id(slot));
    return ret == -ENOENT ? 0 : ret;
}
#endif

#else

#include <errno.h>
#include <string.h>

#if DEVICE_ROLE == ROLE_CLICKER && defined(CONFIG_NVS) && \
    defined(CONFIG_FLASH_MAP)
#define APP_MESH_CLICKER_OUTBOX_PERSISTENCE 1

static int clicker_outbox_write_verified(
    const struct mesh_relay_outbox_snapshot *snapshot)
{
    struct mesh_relay_outbox_snapshot readback;
    ssize_t io_len;

    io_len = nvs_write(app_nvs_storage_fs(),
                       APP_NVS_ID_MESH_OUTBOX,
                       snapshot,
                       sizeof(*snapshot));
    if (io_len < 0) {
        return (int)io_len;
    }
    if (io_len != 0 && (size_t)io_len != sizeof(*snapshot)) {
        return -EIO;
    }
    memset(&readback, 0, sizeof(readback));
    io_len = nvs_read(app_nvs_storage_fs(),
                      APP_NVS_ID_MESH_OUTBOX,
                      &readback,
                      sizeof(readback));
    if ((size_t)io_len != sizeof(readback) ||
        memcmp(&readback, snapshot, sizeof(readback)) != 0) {
        return io_len < 0 ? (int)io_len : -EIO;
    }
    return 0;
}
#endif

int app_mesh_persistence_read_gateway_collection_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len)
{
    ARG_UNUSED(slot);
    ARG_UNUSED(data);
    ARG_UNUSED(data_cap);
    if (stored_len != NULL) {
        *stored_len = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_write_gateway_collection_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    ARG_UNUSED(slot);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    return -ENOTSUP;
}

int app_mesh_persistence_delete_gateway_collection_receipt(uint8_t slot)
{
    ARG_UNUSED(slot);
    return -ENOTSUP;
}

int app_mesh_persistence_read_gateway_terminal_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len)
{
    ARG_UNUSED(slot);
    ARG_UNUSED(data);
    ARG_UNUSED(data_cap);
    if (stored_len != NULL) {
        *stored_len = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_write_gateway_terminal_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len)
{
    ARG_UNUSED(slot);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    return -ENOTSUP;
}

int app_mesh_persistence_delete_gateway_terminal_receipt(uint8_t slot)
{
    ARG_UNUSED(slot);
    return -ENOTSUP;
}

int app_mesh_persistence_init(void)
{
#if defined(APP_MESH_CLICKER_OUTBOX_PERSISTENCE)
    return app_nvs_storage_init();
#else
    return -ENOTSUP;
#endif
}

int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
#if defined(APP_MESH_CLICKER_OUTBOX_PERSISTENCE)
    struct mesh_relay_outbox_snapshot snapshot;
    ssize_t read_len;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    ret = app_nvs_storage_init();
    if (ret < 0) {
        return ret;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    read_len = nvs_read(app_nvs_storage_fs(),
                        APP_NVS_ID_MESH_OUTBOX,
                        &snapshot,
                        sizeof(snapshot));
    if (read_len == -ENOENT) {
        return 0;
    }
    if (read_len < 0) {
        return (int)read_len;
    }
    if ((size_t)read_len != sizeof(snapshot)) {
        return -EINVAL;
    }
    ret = mesh_relay_restore_outbox_snapshot(relay, &snapshot, now_ms);
    return ret == PROTO_OK ? 0 : -EINVAL;
#else
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
#endif
}

int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms)
{
#if defined(APP_MESH_CLICKER_OUTBOX_PERSISTENCE)
    struct mesh_relay_outbox_snapshot snapshot;
    int ret;

    if (relay == NULL) {
        return -EINVAL;
    }
    ret = app_nvs_storage_init();
    if (ret < 0) {
        return ret;
    }
    ret = mesh_relay_export_outbox_snapshot(relay, now_ms, &snapshot);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return app_mesh_persistence_clear_outbox();
    }
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    return clicker_outbox_write_verified(&snapshot);
#else
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
#endif
}

int app_mesh_persistence_clear_outbox(void)
{
#if defined(APP_MESH_CLICKER_OUTBOX_PERSISTENCE)
    uint8_t stale;
    ssize_t read_len;
    int ret;

    ret = app_nvs_storage_init();
    if (ret < 0) {
        return ret;
    }
    ret = nvs_delete(app_nvs_storage_fs(), APP_NVS_ID_MESH_OUTBOX);
    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }
    read_len = nvs_read(app_nvs_storage_fs(),
                        APP_NVS_ID_MESH_OUTBOX,
                        &stale,
                        sizeof(stale));
    return read_len == -ENOENT ? 0 :
           (read_len < 0 ? (int)read_len : -EIO);
#else
    return -ENOTSUP;
#endif
}

int app_mesh_persistence_deferred_outbox_present(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_restore_deferred_outbox(struct mesh_relay *relay,
                                                 uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_save_deferred_outbox(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_deferred_outbox(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_complete_deferred_outbox(struct mesh_relay *relay,
                                                  uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_deferred_outbox_if_matches(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    ARG_UNUSED(packet);
    ARG_UNUSED(semantic_digest);
    return -ENOTSUP;
}

int app_mesh_persistence_prepare_anchor_range_journal(
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    uint64_t anchor_id,
    uint64_t gateway_id,
    struct anchor_range_journal_control *control)
{
    ARG_UNUSED(clicker_id);
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(anchor_id);
    ARG_UNUSED(gateway_id);
    if (control != NULL) {
        memset(control, 0, sizeof(*control));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_save_anchor_range_fragment(
    struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    const struct mesh_outbound *outbound,
    enum anchor_range_fragment_persistence_observation *observation)
{
    ARG_UNUSED(control);
    ARG_UNUSED(fragment_index);
    ARG_UNUSED(outbound);
    if (observation != NULL) {
        *observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_commit_anchor_range_journal(
    const struct anchor_range_journal_control *control)
{
    ARG_UNUSED(control);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_anchor_range_journal(
    struct anchor_range_journal_control *control)
{
    if (control != NULL) {
        memset(control, 0, sizeof(*control));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_anchor_range_fragment(
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    struct mesh_outbound *outbound)
{
    ARG_UNUSED(control);
    ARG_UNUSED(fragment_index);
    if (outbound != NULL) {
        memset(outbound, 0, sizeof(*outbound));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_anchor_range_journal(
    const struct anchor_range_journal_control *control)
{
    ARG_UNUSED(control);
    return -ENOTSUP;
}

int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_local_delivery(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_survey_pair_result_delivery(
    uint8_t slot,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    ARG_UNUSED(slot);
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_survey_pair_result_delivery(
    uint8_t slot,
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    ARG_UNUSED(slot);
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_survey_pair_result_delivery(uint8_t slot)
{
    ARG_UNUSED(slot);
    return -ENOTSUP;
}

int app_mesh_persistence_gateway_host_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_gateway_host_journal_matches_with_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *host_projection_mask)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    if (host_projection_mask != NULL) {
        *host_projection_mask = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_prepare_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_prepare_gateway_host_journal_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms,
    uint8_t host_projection_mask)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(host_projection_mask);
    return -ENOTSUP;
}

int app_mesh_persistence_commit_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_recover_raw_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_save_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_host_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms)
{
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_cap);
    if (packet != NULL) {
        memset(packet, 0, sizeof(*packet));
    }
    if (payload_len != NULL) {
        *payload_len = 0u;
    }
    if (received_at_ms != NULL) {
        *received_at_ms = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_host_journal_projection(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms,
    uint8_t *host_projection_mask)
{
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_cap);
    if (packet != NULL) {
        memset(packet, 0, sizeof(*packet));
    }
    if (payload_len != NULL) {
        *payload_len = 0u;
    }
    if (received_at_ms != NULL) {
        *received_at_ms = 0u;
    }
    if (host_projection_mask != NULL) {
        *host_projection_mask = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_host_terminal_marker(
    struct proto_packet *packet,
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool *source_confirmed)
{
    if (packet != NULL) {
        memset(packet, 0, sizeof(*packet));
    }
    if (packet_digest != NULL) {
        memset(packet_digest, 0, SEMANTIC_DIGEST_SHA256_LEN);
    }
    if (source_confirmed != NULL) {
        *source_confirmed = false;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_gateway_host_journal(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_clear_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
    const struct proto_packet *packet,
    const uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload_digest);
    return -ENOTSUP;
}

int app_mesh_persistence_confirm_gateway_host_journal(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len)
{
    ARG_UNUSED(confirm_packet);
    ARG_UNUSED(confirm_payload);
    ARG_UNUSED(confirm_payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_finalize_gateway_host_journal_if_complete(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

int app_mesh_persistence_retire_notified_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    ARG_UNUSED(packet);
    ARG_UNUSED(packet_digest);
    return -ENOTSUP;
}

int app_mesh_persistence_gateway_click_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return app_mesh_persistence_gateway_host_journal_matches(
        packet, payload, payload_len);
}

int app_mesh_persistence_save_gateway_click_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    return app_mesh_persistence_save_gateway_host_journal(
        packet, payload, payload_len, received_at_ms);
}

int app_mesh_persistence_restore_gateway_click_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms)
{
    return app_mesh_persistence_restore_gateway_host_journal(
        packet,
        payload,
        payload_cap,
        payload_len,
        received_at_ms);
}

int app_mesh_persistence_clear_gateway_click_journal(void)
{
    return app_mesh_persistence_clear_gateway_host_journal();
}

int app_mesh_persistence_clear_gateway_click_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return app_mesh_persistence_clear_gateway_host_journal_if_matches(
        packet, payload, payload_len);
}

int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_rollback_click_handoff(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_complete_confirmed_producer(
    const struct proto_packet *original_packet,
    const uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    ARG_UNUSED(original_packet);
    ARG_UNUSED(original_digest);
    return 0;
}

int app_mesh_persistence_complete_terminal_producer(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return 0;
}

int app_mesh_persistence_restore_child_custody(struct mesh_relay *relay,
                                               uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms)
{
    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_child_custody(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_collection_result(
    const struct app_mesh_collection_result_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_collection_result(
    struct app_mesh_collection_result_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_collection_result(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_gateway_collection(
    const struct gateway_collection_state *collection)
{
    ARG_UNUSED(collection);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_collection(
    struct gateway_collection_state *collection)
{
    ARG_UNUSED(collection);
    return -ENOTSUP;
}

int app_mesh_persistence_rollback_gateway_collection(
    struct gateway_collection_state *collection)
{
    ARG_UNUSED(collection);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_gateway_collection(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_gateway_eack_custody(
    const struct gateway_collection_eack_custody_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_eack_custody(
    struct gateway_collection_eack_custody_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_gateway_eack_custody(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster)
{
    ARG_UNUSED(roster);
    return -ENOTSUP;
}

int app_mesh_persistence_save_gateway_assignment_membership(
    const struct gateway_membership_roster *roster,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication)
{
    ARG_UNUSED(roster);
    ARG_UNUSED(assignment_epoch);
    ARG_UNUSED(table_seq);
    ARG_UNUSED(table_commitment);
    ARG_UNUSED(publication);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster,
    bool *publication_pending)
{
    gateway_membership_clear(roster);
    if (publication_pending != NULL) {
        *publication_pending = false;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_assignment_publication(
    struct gateway_membership_publication *publication,
    uint32_t *assignment_epoch,
    uint32_t *table_seq,
    struct discovery_assignment_table_commitment *table_commitment)
{
    if (publication != NULL) {
        memset(publication, 0, sizeof(*publication));
    }
    if (assignment_epoch != NULL) {
        *assignment_epoch = 0u;
    }
    if (table_seq != NULL) {
        *table_seq = 0u;
    }
    if (table_commitment != NULL) {
        memset(table_commitment, 0, sizeof(*table_commitment));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_complete_gateway_assignment_publication(
    uint32_t assignment_epoch,
    uint16_t event_gateway_epoch,
    uint32_t host_session_id,
    uint16_t host_seq)
{
    ARG_UNUSED(assignment_epoch);
    ARG_UNUSED(event_gateway_epoch);
    ARG_UNUSED(host_session_id);
    ARG_UNUSED(host_seq);
    return -ENOTSUP;
}

int app_mesh_persistence_gateway_assignment_proves(
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id)
{
    ARG_UNUSED(assignment_epoch);
    ARG_UNUSED(table_seq);
    ARG_UNUSED(table_commitment);
    ARG_UNUSED(node_id);
    return -ENOTSUP;
}

int app_mesh_persistence_clear_gateway_membership(void)
{
    return -ENOTSUP;
}

int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    ARG_UNUSED(snapshot);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return -ENOTSUP;
}

int app_mesh_persistence_clear_discovery_assignment_checked(void)
{
    return -ENOTSUP;
}

void app_mesh_persistence_clear_discovery_assignment(void)
{
}

int app_mesh_persistence_save_gateway_assignment_epoch(uint32_t epoch)
{
    ARG_UNUSED(epoch);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_assignment_epoch(uint32_t *epoch)
{
    if (epoch != NULL) {
        *epoch = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_gateway_assignment_baseline(
    uint32_t *assignment_epoch)
{
    if (assignment_epoch != NULL) {
        *assignment_epoch = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_reconcile_gateway_assignment_epoch(uint32_t *epoch)
{
    if (epoch != NULL) {
        *epoch = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_reserve_gateway_command_sequences(
    uint32_t count,
    uint32_t *first_sequence)
{
    ARG_UNUSED(count);
    if (first_sequence != NULL) {
        *first_sequence = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_reserve_gateway_survey_generation(
    uint64_t gateway_id,
    uint64_t *generation)
{
    ARG_UNUSED(gateway_id);
    if (generation != NULL) {
        *generation = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_restore_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t *generation)
{
    ARG_UNUSED(local_id);
    ARG_UNUSED(gateway_id);
    if (generation != NULL) {
        *generation = 0u;
    }
    return -ENOTSUP;
}

int app_mesh_persistence_advance_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t generation)
{
    ARG_UNUSED(local_id);
    ARG_UNUSED(gateway_id);
    ARG_UNUSED(generation);
    return -ENOTSUP;
}

int app_mesh_persistence_save_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    const struct gateway_command_rx_duplicate_cache *replay)
{
    ARG_UNUSED(local_id);
    ARG_UNUSED(gateway_id);
    ARG_UNUSED(replay);
    return -ENOTSUP;
}

int app_mesh_persistence_restore_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    struct gateway_command_rx_duplicate_cache *replay)
{
    ARG_UNUSED(local_id);
    ARG_UNUSED(gateway_id);
    if (replay != NULL) {
        memset(replay, 0, sizeof(*replay));
    }
    return -ENOTSUP;
}

void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health)
{
    if (health != NULL) {
        memset(health, 0, sizeof(*health));
#if defined(APP_MESH_CLICKER_OUTBOX_PERSISTENCE)
        health->ready = app_nvs_storage_ready();
#endif
    }
}

#endif
