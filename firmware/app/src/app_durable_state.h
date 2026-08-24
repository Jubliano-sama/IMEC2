#ifndef APP_DURABLE_STATE_H
#define APP_DURABLE_STATE_H

#include "app_gateway_command_observability.h"
#include "discovery_assignment.h"
#include "gateway_membership.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This module is the only application owner of storage_partition and NVS.
 * It deliberately exposes typed, infrequently updated checkpoints rather
 * than raw NVS access. Packet queues, retry state, ACK history, and other hot
 * runtime state do not belong here.
 */
#define APP_DURABLE_STATE_ENVELOPE_VERSION 1u
#define APP_DURABLE_STATE_RECORD_VERSION 1u
#define APP_DURABLE_STATE_RECORD_HEADER_SIZE 32u
#define APP_DURABLE_STATE_COUNTER_PAYLOAD_SIZE 16u
#define APP_DURABLE_STATE_COUNTER_RECORD_SIZE \
    (APP_DURABLE_STATE_RECORD_HEADER_SIZE + \
     APP_DURABLE_STATE_COUNTER_PAYLOAD_SIZE)
#define APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE 168u
#define APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_RECORD_SIZE \
    (APP_DURABLE_STATE_RECORD_HEADER_SIZE + \
     APP_DURABLE_STATE_ANCHOR_ASSIGNMENT_PAYLOAD_SIZE)
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_GATEWAY_ID_SIZE \
    sizeof(uint64_t)
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_IDENTITY_SIZE 16u
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RETIRE_PROOF_SIZE \
    sizeof(uint64_t)
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_PAYLOAD_SIZE \
    (APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_GATEWAY_ID_SIZE + \
     APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_IDENTITY_SIZE + \
     GATEWAY_MEMBERSHIP_SNAPSHOT_WIRE_SIZE + \
     APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RETIRE_PROOF_SIZE)
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE \
    (APP_DURABLE_STATE_RECORD_HEADER_SIZE + \
     APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_PAYLOAD_SIZE)
#define APP_DURABLE_STATE_MAX_RECORD_SIZE \
    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_RECORD_SIZE

#define APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR \
    UINT64_C(0x01000000)
/*
 * Version-1 click high-water records are block-aligned. A deployed 256-value
 * record is not a safe seed for this 65536-value format: back it up and erase
 * or explicitly migrate it before upgrading rather than rounding a partially
 * consumed block and risking identity reuse.
 */
#define APP_DURABLE_STATE_CLICK_BLOCK_SIZE UINT32_C(65536)
#define APP_DURABLE_STATE_CLICK_MAX_BLOCK_RESERVATIONS \
    ((UINT32_MAX - APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR) / \
     APP_DURABLE_STATE_CLICK_BLOCK_SIZE)
#define APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR \
    UINT64_C(0x01000000)
/*
 * Gateway control identifiers deliberately do not wrap.  The command record
 * high-water is aligned to this block, so a deployed 256-value record that is
 * not already a whole 262144-value boundary fails closed rather than being
 * rounded.  First deployment of this format must explicitly initialize the
 * storage record; an accidentally aligned old high-water is safe because the
 * new owner only skips values that were already reserved.  The final partial
 * UINT32 range is intentionally stranded and then fails with -EOVERFLOW.
 */
#define APP_DURABLE_STATE_COMMAND_BLOCK_SIZE UINT32_C(262144)
#define APP_DURABLE_STATE_COMMAND_MAX_BLOCK_RESERVATIONS \
    ((UINT32_MAX - APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR) / \
     APP_DURABLE_STATE_COMMAND_BLOCK_SIZE)

enum app_durable_state_role {
    APP_DURABLE_STATE_ROLE_CLICKER = 1,
    APP_DURABLE_STATE_ROLE_ANCHOR = 2,
    APP_DURABLE_STATE_ROLE_GATEWAY = 3,
};

/*
 * The NVS IDs behind these types retain the proven pre-RAM-migration key
 * assignments for click and gateway-command high-water records.
 * Their common envelope intentionally rejects older unbound schemas instead
 * of treating them as an empty first install.
 */
enum app_durable_state_record_type {
    APP_DURABLE_STATE_BOOT_INCARNATION = 1,
    APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE = 2,
    APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE = 3,
    APP_DURABLE_STATE_ANCHOR_ASSIGNMENT = 5,
    APP_DURABLE_STATE_GATEWAY_ASSIGNMENT = 6,
};

struct app_durable_state_reservation {
    uint64_t first;
    uint64_t reserved_through;
};

/*
 * Semantic assignment state. The durable codec writes every field explicitly;
 * this in-memory layout is never copied to flash and its tail padding is not
 * part of the storage ABI. Local device identity lives in the common envelope,
 * gateway identity is the required scope passed to the typed API, and retry
 * rounds deliberately remain RAM-only.
 */
struct app_durable_state_anchor_assignment {
    struct discovery_assignment_table_commitment table_commitment;
    struct discovery_assignment_table_commitment pending_table_commitment;
    uint32_t epoch;
    uint32_t table_command_seq;
    uint32_t pending_epoch;
    uint32_t pending_table_command_seq;
    uint32_t retired_epochs[DISCOVERY_ASSIGNMENT_RETIRED_EPOCH_CAP];
    uint16_t table_packet_seq;
    uint16_t response_spread_ms;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t provisioned;
    uint8_t retired_epoch_count;
    uint8_t ordered_epoch_valid;
    uint8_t ack_pending;
    uint8_t pending_slot;
    uint8_t pending_slot_count;
    /* Compact TABLE timing lane; zero count decodes legacy pending records. */
    uint8_t pending_response_lane;
    uint8_t pending_response_lane_count;
    uint8_t pending_valid;
};

_Static_assert(sizeof(struct app_durable_state_anchor_assignment) == 160u,
               "assignment semantic state layout changed");

/*
 * The command-event sequence and BLE record digest are transport details: a
 * reset replay deliberately receives a fresh stream record identity.  This
 * compact operation identity is the stable semantic identity that binds the
 * immutable membership commit to the terminal host receipt that may retire it.
 */
struct app_durable_state_gateway_assignment_identity {
    uint32_t correlation_id;
    uint32_t gateway_sequence;
    uint32_t host_session_id;
    uint16_t gateway_epoch;
    uint16_t host_seq;
};

_Static_assert(sizeof(struct app_durable_state_gateway_assignment_identity) ==
                   APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_IDENTITY_SIZE,
               "gateway assignment identity layout changed");

/*
 * Capability returned only after this module has verified the exact record it
 * wrote or restored.  Its representation is deliberately opaque to callers:
 * never initialize, copy from storage, or synthesize one locally.
 */
#define APP_DURABLE_STATE_RECEIPT_OPAQUE_WORDS 2u
struct app_durable_receipt {
    uint64_t opaque[APP_DURABLE_STATE_RECEIPT_OPAQUE_WORDS];
};

_Static_assert(sizeof(struct app_durable_receipt) == 16u,
               "durable receipt ABI changed");

/*
 * A write completed but its required readback was unavailable, so the caller
 * must retain its exact prepared semantic commit and retry it for adoption.
 * This is deliberately distinct from a successful save: no receipt is
 * returned and publication/activation remains forbidden.
 */
#define APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_SAVE_ADOPT_REQUIRED 1

/*
 * Bind and mount the sole store owner for this compiled role and physical
 * device. The device identity must be nonzero. Rebinding a mounted owner fails
 * closed. Initialization deliberately does not consume a boot incarnation, so
 * fallible hardware/application setup before role admission cannot turn into a
 * rapid NVS-write reboot loop.
 */
int app_durable_state_init(uint64_t device_id);
/* True once the store is mounted and bound; begin_boot may still be pending. */
bool app_durable_state_ready(void);

/*
 * Reserve and read back the one incarnation for this boot. Call this exactly
 * at the role-admission boundary, after fallible startup. Repeated calls are
 * idempotent and RAM-only once an incarnation has been cached.
 */
int app_durable_state_begin_boot(void);

/*
 * Return the incarnation cached by begin_boot(). Calls before that boundary
 * fail closed. Consumers may read it but cannot advance the durable record.
 */
int app_durable_state_boot_incarnation(uint32_t *incarnation);

/*
 * Reserve the configured block for a typed counter before any value is
 * exposed. Missing storage is a first install; corrupt, stale-schema, wrong
 * role, wrong-device, and wrong-scope records are errors, never "missing".
 *
 * scope_id is zero for boot/click/command counters. Scoped state binds a
 * record to the nonzero gateway identity as well as the local device.
 * Boot incarnation is reserved exactly once by app_durable_state_begin_boot()
 * and is not available through this generic API. It is a 32-bit route-epoch seed:
 * first install reserves two rather than reproducing the legacy value one,
 * later boots skip every low-16-zero value, and exhaustion fails closed at
 * UINT32_MAX. Reaching that bound would require billions of device boots and
 * must not be hidden by caller-side remapping.
 */
int app_durable_state_reserve(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    struct app_durable_state_reservation *reservation);

/* Returns 1 when present, 0 only for a genuinely missing first install. */
int app_durable_state_restore_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t *reserved_through);

/*
 * Persist an exact monotonic high-water checkpoint. Exact replay is
 * idempotent and rollback is rejected. Reservation-owned counter types reject
 * this API.
 */
int app_durable_state_advance_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t candidate);

/*
 * Store, restore, or delete the anchor's sparse assignment transaction. Save
 * and delete require begin_boot() and perform an exact readback before success.
 * Restore returns 1 for a validated record, 0 only for a genuinely missing
 * first install, and a negative error for corrupt or inaccessible evidence.
 */
int app_durable_state_save_anchor_assignment(
    uint64_t gateway_id,
    const struct app_durable_state_anchor_assignment *assignment);
int app_durable_state_restore_anchor_assignment(
    uint64_t gateway_id,
    struct app_durable_state_anchor_assignment *assignment);
int app_durable_state_delete_anchor_assignment(uint64_t gateway_id);

/*
 * Gateway assignment publication is one immutable semantic commit.  Its
 * `publish_pending` membership field is the sole reset replay debt: it stores
 * sparse slots, TABLE proof, and host-visible publication input, but never a
 * BLE queue, retry round, event sequence, or host-receipt digest.  Save is
 * idempotent for the exact same commit and rejects a successor while debt is
 * retained. It returns SAVE_ADOPT_REQUIRED after a write/readback ambiguity;
 * callers must retry the exact candidate while retaining their prepared
 * publisher/operation, and must never activate it without a returned receipt.
 * Restore returns 1 for a validated commit, 0 for a genuine first install,
 * and a negative error for corrupt or inaccessible evidence.
 */
int app_durable_state_save_gateway_assignment_commit(
    uint64_t gateway_id,
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity,
    struct app_durable_receipt *receipt);
int app_durable_state_restore_gateway_assignment_commit(
    uint64_t gateway_id,
    struct gateway_membership_snapshot *snapshot,
    struct app_durable_state_gateway_assignment_identity *identity,
    bool *replay_debt,
    struct app_durable_receipt *receipt);
/*
 * Retire only after the BLE layer has accepted the exact stream-record GUI
 * receipt.  The terminal event and prior receipt must still match the
 * committed operation.  Retirement writes/readbacks the same sparse roster
 * with its publication debt removed; it never deletes the roster. The
 * retired record retains only an opaque predecessor-record proof so a
 * write/readback retry can require the exact pending receipt; it is neither
 * replay debt nor host transport state.
 */
int app_durable_state_retire_gateway_assignment_commit(
    uint64_t gateway_id,
    const struct app_durable_receipt *receipt,
    const struct gateway_command_event *terminal_event,
    struct app_durable_receipt *retired_receipt);
/*
 * Explicit decommission only after host receipt retirement; a pending
 * publication/replay-debt record is rejected. Regular receipt retirement must
 * never use it. The semantic identity lets an erase/readback retry validate
 * receipt binding after no NVS record remains.
 */
int app_durable_state_delete_gateway_assignment(
    uint64_t gateway_id,
    const struct app_durable_state_gateway_assignment_identity *identity,
    const struct app_durable_receipt *receipt);

#if defined(APP_DURABLE_STATE_TESTING)
#include <sys/types.h>

struct app_durable_state_test_backend {
    void *context;
    int (*mount)(void *context);
    ssize_t (*read)(void *context,
                    uint16_t id,
                    void *data,
                    size_t len);
    ssize_t (*write)(void *context,
                     uint16_t id,
                     const void *data,
                     size_t len);
    int (*erase)(void *context, uint16_t id);
};

/* Native-only injection. Production never exposes its nvs_fs or backend. */
int app_durable_state_test_install_backend(
    const struct app_durable_state_test_backend *backend,
    enum app_durable_state_role role);
void app_durable_state_test_reset(void);
int app_durable_state_test_seed_high_water(
    enum app_durable_state_record_type type,
    uint64_t scope_id,
    uint64_t reserved_through);
#endif

#endif
