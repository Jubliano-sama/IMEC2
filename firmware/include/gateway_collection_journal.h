#ifndef GATEWAY_COLLECTION_JOURNAL_H
#define GATEWAY_COLLECTION_JOURNAL_H

#include "gateway_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_COLLECTION_JOURNAL_VERSION 1u
#define GATEWAY_COLLECTION_JOURNAL_BANK_COUNT 2u
#define GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP 10u
#define GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT \
    ((GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP + \
      GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP - 1u) / \
     GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP)

#define GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE 46u
#define GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE 34u
#define GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE 46u
#define GATEWAY_COLLECTION_JOURNAL_ROSTER_RECORD_MAX_SIZE 102u
#define GATEWAY_COLLECTION_JOURNAL_RECORD_MAX_SIZE \
    GATEWAY_COLLECTION_JOURNAL_ROSTER_RECORD_MAX_SIZE

enum gateway_collection_journal_record_kind {
    GATEWAY_COLLECTION_JOURNAL_RECORD_BASE = 1,
    GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL,
    GATEWAY_COLLECTION_JOURNAL_RECORD_ROSTER,
    GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT,
};

struct gateway_collection_journal_key {
    enum gateway_collection_journal_record_kind kind;
    uint8_t bank;
    uint8_t index;
};

struct gateway_collection_journal_io {
    void *ctx;
    int (*read)(void *ctx,
                struct gateway_collection_journal_key key,
                void *data,
                size_t data_cap,
                size_t *stored_len);
    int (*write)(void *ctx,
                 struct gateway_collection_journal_key key,
                 const void *data,
                 size_t data_len);
};

struct gateway_collection_journal_cursor {
    uint64_t generation;
    uint64_t committed_slots;
    uint64_t gateway_id;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint32_t next_retry_spread_ms;
    uint16_t gateway_epoch;
    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t expected_node_id_count;
    uint16_t roster_crc;
    uint16_t eack_sequence;
    uint8_t retry_round;
    bool collection_open;
    bool eack_pending;
    bool active;
    bool loaded;
};

struct gateway_collection_journal_stats {
    uint64_t bytes_attempted;
    uint64_t bytes_committed;
    uint32_t write_attempts;
    uint32_t writes_committed;
    uint32_t records_ignored;
    uint32_t results_replayed;
};

/*
 * A loaded cursor is the commit boundary for one collection generation.
 * While saving that generation, callers may only append invalid-to-valid
 * result slots and update control fields. Entries selected by
 * cursor->committed_slots must remain byte-for-byte stable; the compact cursor
 * deliberately does not duplicate all result identities in RAM to recheck
 * that invariant.
 */
int gateway_collection_journal_save(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    const struct gateway_collection_state *collection,
    struct gateway_collection_journal_stats *stats);

int gateway_collection_journal_restore(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_state *collection,
    struct gateway_collection_journal_stats *stats);

int gateway_collection_journal_clear(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_journal_stats *stats);

int gateway_collection_journal_rollback_uncommitted(
    const struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_state *collection);

#ifdef __cplusplus
}
#endif

#endif
