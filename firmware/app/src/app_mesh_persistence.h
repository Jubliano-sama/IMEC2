#ifndef APP_MESH_PERSISTENCE_H
#define APP_MESH_PERSISTENCE_H

#include "anchor_range_fragment_policy.h"
#include "anchor_range_journal.h"
#include "gateway_command.h"
#include "gateway_membership.h"
#include "app_mesh_local_delivery.h"
#include "discovery_assignment.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION 1u
#define APP_MESH_CLICK_HANDOFF_SNAPSHOT_VERSION 1u
#define APP_MESH_GATEWAY_HOST_JOURNAL_VERSION 3u
#define APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC UINT32_C(0x47434A31)
#define APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED 1u
#define APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED 2u
#define APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW 3u
#define APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED 4u
#define APP_MESH_GATEWAY_HOST_JOURNAL_SOURCE_CONFIRMED UINT16_C(0x0001)
#define APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN \
    PACKET_EXT_MAX_PAYLOAD_LEN
/* Compatibility names preserve the installed click-journal schema and the
 * focused fault-test surface while the owner is generalized. */
#define APP_MESH_GATEWAY_CLICK_JOURNAL_VERSION \
    APP_MESH_GATEWAY_HOST_JOURNAL_VERSION
#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAGIC \
    APP_MESH_GATEWAY_HOST_JOURNAL_MAGIC
#define APP_MESH_GATEWAY_CLICK_JOURNAL_MAX_PAYLOAD_LEN \
    APP_MESH_GATEWAY_HOST_JOURNAL_MAX_PAYLOAD_LEN

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

#define APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION 8u
#define APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_MAGIC \
    UINT32_C(0x44415338)
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
    uint32_t magic;
    /* Exact pending TABLE ACK envelope. */
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

#define APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_VERSION 1u
#define APP_MESH_GATEWAY_ASSIGNMENT_EPOCH_SNAPSHOT_MAGIC \
    UINT32_C(0x47414531)
struct app_mesh_gateway_assignment_epoch_snapshot {
    uint32_t magic;
    uint32_t epoch;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint8_t valid;
    uint8_t reserved;
};

#define APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_VERSION 1u
#define APP_MESH_GATEWAY_COMMAND_SEQUENCE_SNAPSHOT_MAGIC \
    UINT32_C(0x47435331)
struct app_mesh_gateway_command_sequence_snapshot {
    uint32_t magic;
    uint32_t reserved_through;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint8_t valid;
    uint8_t reserved;
};

#define APP_MESH_SURVEY_GENERATION_SNAPSHOT_VERSION 1u
#define APP_MESH_SURVEY_GENERATION_SNAPSHOT_MAGIC \
    UINT32_C(0x53564731)
#define APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS 4u
struct app_mesh_survey_generation_snapshot {
    uint32_t magic;
    uint64_t local_id;
    uint64_t gateway_id;
    uint64_t generation;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint8_t role;
    uint8_t valid;
    uint16_t reserved;
};

#define APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_VERSION 1u
#define APP_MESH_ANCHOR_COMMAND_REPLAY_SNAPSHOT_MAGIC \
    UINT32_C(0x41435231)
struct app_mesh_anchor_command_replay_snapshot {
    uint32_t magic;
    uint64_t local_id;
    uint64_t gateway_id;
    struct gateway_command_rx_duplicate_cache replay;
    uint16_t version;
    uint16_t size;
    uint16_t checksum;
    uint8_t valid;
    uint8_t reserved;
};

_Static_assert(offsetof(struct app_mesh_anchor_command_replay_snapshot,
                        replay) == 24u &&
               offsetof(struct app_mesh_anchor_command_replay_snapshot,
                        version) == 40u &&
               sizeof(struct app_mesh_anchor_command_replay_snapshot) == 48u,
               "anchor command replay snapshot schema-1 layout changed");

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
 * The gateway host-output journal keeps the commit marker separate from the
 * exact payload bytes.  The payload is written first and this compact
 * metadata is written last, so a reset can expose only an orphan payload,
 * never a committed marker whose payload was not durable.
 *
 * The two NVS keys used by this journal are anchor-only in their normal
 * APIs.  They are deliberately overlaid for ROLE_GATEWAY so the journal does
 * not add another live key or grow the gateway's static RAM budget.
 *
 * Schema 2 commits the exact payload bytes with SHA-256. Installed schema-1
 * records are migrated from their retained raw payload before they can take
 * part in an identity decision.
 */
struct app_mesh_gateway_click_journal_metadata {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct proto_packet packet;
    uint16_t payload_len;
    uint16_t payload_crc;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t received_at_ms;
    uint32_t checksum;
    uint8_t valid;
    /* A nonzero mask selects newly accepted records from an immutable result
     * bundle for host delivery; zero preserves the raw payload. */
    uint8_t host_projection_mask;
    uint16_t state_flags;
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
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
/*
 * The anchor range journal stores exact protocol wire packets under
 * per-fragment keys and writes the canonical control record last. A caller
 * must not make any staged fragment visible to the report worker until
 * commit succeeds.
 */
int app_mesh_persistence_prepare_anchor_range_journal(
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    uint64_t anchor_id,
    uint64_t gateway_id,
    struct anchor_range_journal_control *control);
int app_mesh_persistence_save_anchor_range_fragment(
    struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    const struct mesh_outbound *outbound,
    enum anchor_range_fragment_persistence_observation *observation);
int app_mesh_persistence_commit_anchor_range_journal(
    const struct anchor_range_journal_control *control);
/* Returns 1 when a committed journal exists, 0 when absent. */
int app_mesh_persistence_restore_anchor_range_journal(
    struct anchor_range_journal_control *control);
int app_mesh_persistence_restore_anchor_range_fragment(
    const struct anchor_range_journal_control *control,
    uint8_t fragment_index,
    struct mesh_outbound *outbound);
int app_mesh_persistence_clear_anchor_range_journal(
    const struct anchor_range_journal_control *control);
int app_mesh_persistence_save_local_delivery(
    const struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_persistence_restore_local_delivery(
    struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_persistence_clear_local_delivery(void);
int app_mesh_persistence_save_survey_pair_result_delivery(
    uint8_t slot,
    const struct app_mesh_local_delivery_snapshot *snapshot);
/* Returns 1 when present, 0 when absent, or a negative errno. */
int app_mesh_persistence_restore_survey_pair_result_delivery(
    uint8_t slot,
    struct app_mesh_local_delivery_snapshot *snapshot);
int app_mesh_persistence_clear_survey_pair_result_delivery(uint8_t slot);
int app_mesh_persistence_stage_click_handoff(struct mesh_relay *relay,
                                             uint32_t now_ms);
int app_mesh_persistence_commit_click_handoff(struct mesh_relay *relay,
                                              uint32_t now_ms);
int app_mesh_persistence_rollback_click_handoff(void);
int app_mesh_persistence_complete_click_handoff(struct mesh_relay *relay,
                                                uint32_t now_ms);
/*
 * Retire any retained producer-side handoff that is proven to be the exact
 * original carried by a terminal ACK_CONFIRM. A different producer record is
 * left intact; an identity collision with a different digest fails closed.
 */
int app_mesh_persistence_complete_confirmed_producer(
    const struct proto_packet *original_packet,
    const uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN]);
/*
 * Retire the same retained producer handoff when the exact outbound reaches a
 * terminal transport outcome instead of an ACK_CONFIRM. The payload is
 * required so an identity wrap or collision cannot release different bytes.
 */
int app_mesh_persistence_complete_terminal_producer(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_mesh_persistence_restore_child_custody(struct mesh_relay *relay,
                                               uint32_t now_ms);
int app_mesh_persistence_save_child_custody(struct mesh_relay *relay,
                                            uint32_t now_ms);
int app_mesh_persistence_clear_child_custody(void);
int app_mesh_persistence_save_collection_result(
    const struct app_mesh_collection_result_snapshot *snapshot);
int app_mesh_persistence_restore_collection_result(
    struct app_mesh_collection_result_snapshot *snapshot);
int app_mesh_persistence_clear_collection_result(void);
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
int app_mesh_persistence_clear_gateway_eack_custody(void);
int app_mesh_persistence_save_gateway_membership(
    const struct gateway_membership_roster *roster);
int app_mesh_persistence_save_gateway_assignment_membership(
    const struct gateway_membership_roster *roster,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication);
int app_mesh_persistence_restore_gateway_membership(
    struct gateway_membership_roster *roster,
    bool *publication_pending);
/*
 * Returns 1 with the exact pending host publication, 0 when the durable
 * membership has no publication debt, and a negative errno on storage or
 * validation failure.
 */
int app_mesh_persistence_restore_gateway_assignment_publication(
    struct gateway_membership_publication *publication,
    uint32_t *assignment_epoch,
    uint32_t *table_seq,
    struct discovery_assignment_table_commitment *table_commitment);
/*
 * Clear publication debt only when the durable record still belongs to the
 * exact host-visible operation.  The roster and assignment proof remain.
 */
int app_mesh_persistence_complete_gateway_assignment_publication(
    uint32_t assignment_epoch,
    uint16_t event_gateway_epoch,
    uint32_t host_session_id,
    uint16_t host_seq);
/* Returns 1 only when the exact durable assignment roster contains node_id,
 * 0 when no such proof exists, and a negative errno on storage/corruption. */
int app_mesh_persistence_gateway_assignment_proves(
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id);
int app_mesh_persistence_clear_gateway_membership(void);
int app_mesh_persistence_save_discovery_assignment(
    const struct app_mesh_discovery_assignment_snapshot *snapshot);
int app_mesh_persistence_restore_discovery_assignment(
    struct app_mesh_discovery_assignment_snapshot *snapshot);
int app_mesh_persistence_clear_discovery_assignment_checked(void);
void app_mesh_persistence_clear_discovery_assignment(void);
int app_mesh_persistence_save_gateway_assignment_epoch(uint32_t epoch);
int app_mesh_persistence_restore_gateway_assignment_epoch(uint32_t *epoch);
/*
 * Read and validate the durable membership record without changing or
 * deleting it. Returns 1 with its assignment-proof epoch, 0 when the record
 * is absent or valid without a proof, and a negative errno on read or
 * validation failure.
 */
int app_mesh_persistence_restore_gateway_assignment_baseline(
    uint32_t *assignment_epoch);
/*
 * Reconcile the standalone reservation cursor with the validated membership
 * proof. A proof that is strictly newer repairs the cursor durably before
 * success; a newer cursor is preserved. Half-range ambiguity fails closed.
 */
int app_mesh_persistence_reconcile_gateway_assignment_epoch(uint32_t *epoch);
/*
 * Atomically reserve a non-zero serial-number block.  The persisted cursor
 * moves to the end of the block before the caller may emit its first value,
 * so reset can skip unused values but can never reuse an emitted identity.
 */
int app_mesh_persistence_reserve_gateway_command_sequences(
    uint32_t count,
    uint32_t *first_sequence);
/*
 * Reserve one never-reused survey operation generation. The record advances
 * before success is returned, and values whose low 32 bits are zero are
 * skipped because the packet session projection must remain nonzero.
 */
int app_mesh_persistence_reserve_gateway_survey_generation(
    uint64_t gateway_id,
    uint64_t *generation);
/*
 * Anchor high-water persistence is identity-bound. Restore returns 1 when a
 * valid record exists and 0 when absent. Advance is idempotent for the exact
 * current generation and rejects rollback with -ESTALE.
 */
int app_mesh_persistence_restore_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t *generation);
int app_mesh_persistence_advance_anchor_survey_generation(
    uint64_t local_id,
    uint64_t gateway_id,
    uint64_t generation);
int app_mesh_persistence_save_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    const struct gateway_command_rx_duplicate_cache *replay);
/* Returns 1 when a valid replay window was restored, 0 when absent. */
int app_mesh_persistence_restore_anchor_command_replay(
    uint64_t local_id,
    uint64_t gateway_id,
    struct gateway_command_rx_duplicate_cache *replay);
/* Returns APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED or
 * APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED for the exact packet, 0 when the
 * journal is empty, and -EBUSY when another host record owns the journal.
 * Identity includes the message type, flags, endpoints, session/sequence,
 * payload length, and durable payload CRC; relay-local TTL and message age
 * are deliberately excluded because retries may rewrite them.
 * Persistence/I/O failures are returned unchanged so callers can fail
 * closed. */
bool app_mesh_persistence_gateway_host_journal_supports(
    const struct proto_packet *packet);
int app_mesh_persistence_gateway_host_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_mesh_persistence_gateway_host_journal_matches_with_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *host_projection_mask);
/* Writes exact raw host custody in PREPARED phase. PREPARED is neither
 * host-visible nor ACK-eligible until semantic commit is durable. */
int app_mesh_persistence_prepare_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms);
int app_mesh_persistence_prepare_gateway_host_journal_projection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms,
    uint8_t host_projection_mask);
/* Idempotently promotes the exact PREPARED owner to COMMITTED. */
int app_mesh_persistence_commit_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/*
 * Terminally retain raw host output after reset when volatile orchestration
 * cannot be replayed. This is intentionally distinct from semantic commit.
 */
int app_mesh_persistence_recover_raw_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/* Compatibility helper for callers without a semantic boundary. New gateway
 * ingress uses the explicit prepare/commit API above. */
int app_mesh_persistence_save_gateway_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms);
/* Returns the durable journal phase when a valid packet/payload was restored,
 * or 0 when absent. */
int app_mesh_persistence_restore_gateway_host_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms);
/*
 * Read a valid terminal marker without requiring its payload. This is only
 * for reconciling a torn terminal clear after ordinary restore has proven the
 * payload unreadable. PREPARED and COMMITTED markers remain fail-closed.
 */
int app_mesh_persistence_restore_gateway_host_terminal_marker(
    struct proto_packet *packet,
    uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool *source_confirmed);
int app_mesh_persistence_restore_gateway_host_journal_projection(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms,
    uint8_t *host_projection_mask);
int app_mesh_persistence_clear_gateway_host_journal(void);
int app_mesh_persistence_clear_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/*
 * Durably records that the exact terminal journal owner has completed its BLE
 * notification.  The raw payload remains available for post-notification
 * receipt materialization, but reset recovery must never expose it to the
 * host again.
 */
int app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
    const struct proto_packet *packet,
    const uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN]);
/*
 * Record a validated ACK-confirm independently from host notification.
 * Returns two after both durable phases were present and the journal cleared,
 * one after SOURCE_CONFIRMED became durable while host notification is still
 * pending, zero for a safe idempotent late/stale duplicate, and a negative
 * errno when storage or the matching phase is uncertain.
 */
int app_mesh_persistence_confirm_gateway_host_journal(
    const struct proto_packet *confirm_packet,
    const uint8_t *confirm_payload,
    size_t confirm_payload_len);
/* Return one after clearing a matching journal whose NOTIFIED and
 * SOURCE_CONFIRMED phases are both durable, zero while either phase is
 * absent, and a negative errno when storage or identity is uncertain. */
int app_mesh_persistence_finalize_gateway_host_journal_if_complete(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/* Bounded liveness escape for a HOST_NOTIFIED record whose source never
 * confirms. The caller owns the grace timer; identity mismatch is fail-safe
 * and cannot clear a newer singleton owner. */
int app_mesh_persistence_retire_notified_gateway_host_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN]);
/* Compatibility wrappers for the original click-only API. */
int app_mesh_persistence_gateway_click_journal_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_mesh_persistence_save_gateway_click_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms);
int app_mesh_persistence_restore_gateway_click_journal(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint32_t *received_at_ms);
int app_mesh_persistence_clear_gateway_click_journal(void);
int app_mesh_persistence_clear_gateway_click_journal_if_matches(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
/*
 * Narrow raw storage seam for the gateway's NVS-only per-node collection
 * receipt module. Slots are fixed and bounded; callers outside that module
 * must not assign receipt record IDs directly.
 *
 * Read returns 1 when present, 0 when absent, or a negative errno. A present
 * record may be larger than data_cap; stored_len always reports NVS's exact
 * record length so the receipt decoder can reject truncation.
 */
int app_mesh_persistence_read_gateway_collection_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len);
int app_mesh_persistence_write_gateway_collection_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len);
int app_mesh_persistence_delete_gateway_collection_receipt(uint8_t slot);
/*
 * Narrow raw storage seam for the gateway's fixed per-source host-terminal
 * receipt ledger. The receipt module owns validation and replacement policy.
 */
int app_mesh_persistence_read_gateway_terminal_receipt(
    uint8_t slot,
    void *data,
    size_t data_cap,
    size_t *stored_len);
int app_mesh_persistence_write_gateway_terminal_receipt(
    uint8_t slot,
    const void *data,
    size_t data_len);
int app_mesh_persistence_delete_gateway_terminal_receipt(uint8_t slot);
void app_mesh_persistence_get_health(struct app_mesh_persistence_health *health);

#if defined(CONFIG_ZTEST)
void app_mesh_persistence_test_reset_faults(void);
void app_mesh_persistence_test_reset_deferred_presence(void);
void app_mesh_persistence_test_set_deferred_busy(bool busy);
void app_mesh_persistence_test_fail_deferred_read(int error, uint8_t count);
void app_mesh_persistence_test_fail_deferred_write(int error, uint8_t count);
void app_mesh_persistence_test_fail_deferred_delete(int error, uint8_t count);
void app_mesh_persistence_test_fail_outbox_write(int error, uint8_t count);
void app_mesh_persistence_test_fail_outbox_delete(int error, uint8_t count);
void app_mesh_persistence_test_fail_gateway_eack_custody_delete(
    int error,
    uint8_t count);
void app_mesh_persistence_test_fail_gateway_membership_write(int error,
                                                              uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_write(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_write(int error,
                                                                 uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_read(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_verify(int error,
                                                                  uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_read(int error,
                                                               uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_delete(int error,
                                                         uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_metadata_delete(int error,
                                                                  uint8_t count);
void app_mesh_persistence_test_fail_gateway_click_payload_delete(int error,
                                                                 uint8_t count);
void app_mesh_persistence_test_fail_gateway_assignment_epoch_write(int error,
                                                                   uint8_t count);
void app_mesh_persistence_test_fail_discovery_assignment_read(int error,
                                                              uint8_t count);
void app_mesh_persistence_test_fail_discovery_assignment_delete(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_collection_result_delete(int error,
                                                              uint8_t count);
void app_mesh_persistence_test_fail_child_custody_delete(int error,
                                                         uint8_t count);
void app_mesh_persistence_test_fail_anchor_range_fragment_write(int error,
                                                                uint8_t count);
void app_mesh_persistence_test_fail_anchor_range_control_write(int error,
                                                               uint8_t count);
void app_mesh_persistence_test_fail_anchor_range_fragment_read(int error,
                                                               uint8_t count);
void app_mesh_persistence_test_fail_anchor_range_control_readback(int error,
                                                                 uint8_t count);
void app_mesh_persistence_test_fail_anchor_range_control_delete(int error,
                                                               uint8_t count);
void app_mesh_persistence_test_fail_gateway_collection_receipt_read(
    int error,
    uint8_t count);
void app_mesh_persistence_test_fail_gateway_collection_receipt_write(
    int error,
    uint8_t count);
void app_mesh_persistence_test_fail_gateway_terminal_receipt_read(
    int error,
    uint8_t count);
void app_mesh_persistence_test_fail_gateway_terminal_receipt_write(
    int error,
    uint8_t count);
void app_mesh_persistence_test_fail_gateway_terminal_receipt_delete(
    int error,
    uint8_t count);
int app_mesh_persistence_test_write_assignment_snapshot(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_write_gateway_assignment_epoch_snapshot(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_delete_gateway_assignment_epoch(void);
int app_mesh_persistence_test_delete_gateway_click_payload(void);
int app_mesh_persistence_test_write_gateway_click_payload(const void *payload,
                                                          size_t payload_len);
int app_mesh_persistence_test_write_collection_result_raw(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_write_outbox_raw(const void *snapshot,
                                               size_t snapshot_len);
int app_mesh_persistence_test_write_child_custody_raw(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_write_gateway_eack_custody_raw(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_write_deferred_outbox_snapshot(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_write_gateway_collection_receipt_raw(
    uint8_t slot,
    const void *data,
    size_t data_len);
int app_mesh_persistence_test_delete_gateway_collection_receipt(uint8_t slot);
int app_mesh_persistence_test_write_gateway_terminal_receipt_raw(
    uint8_t slot,
    const void *data,
    size_t data_len);
int app_mesh_persistence_test_delete_gateway_terminal_receipt(uint8_t slot);
int app_mesh_persistence_test_write_survey_generation_snapshot(
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_delete_survey_generation_snapshot(void);
int app_mesh_persistence_test_write_survey_pair_result_delivery_raw(
    uint8_t slot,
    const void *snapshot,
    size_t snapshot_len);
int app_mesh_persistence_test_delete_survey_pair_result_delivery(uint8_t slot);
#endif

#endif
