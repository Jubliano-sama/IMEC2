#include "gateway_collection_journal.h"

#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define JOURNAL_BASE_MAGIC 0x424A4347u
#define JOURNAL_CONTROL_MAGIC 0x434A4347u
#define JOURNAL_ROSTER_MAGIC 0x524A4347u
#define JOURNAL_RESULT_MAGIC 0x444A4347u

#define JOURNAL_BASE_FLAG_ACTIVE 0x01u
#define JOURNAL_CONTROL_FLAG_OPEN 0x01u
#define JOURNAL_CONTROL_FLAG_EACK_PENDING 0x02u

_Static_assert(GATEWAY_COLLECTION_RESULT_CACHE_SIZE > 0u &&
               GATEWAY_COLLECTION_RESULT_CACHE_SIZE < 64u,
               "journal committed-slot mask requires one to 63 result slots");
_Static_assert(GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT <= UINT8_MAX,
               "journal roster chunk indices must fit one byte");

struct journal_base {
    uint64_t generation;
    uint64_t gateway_id;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint16_t gateway_epoch;
    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t expected_node_id_count;
    uint16_t roster_crc;
    bool active;
};

struct journal_control {
    uint64_t generation;
    uint64_t committed_slots;
    uint32_t next_retry_spread_ms;
    uint16_t eack_sequence;
    uint8_t retry_round;
    bool collection_open;
    bool eack_pending;
};

static uint16_t crc16_update(uint16_t crc, uint8_t value)
{
    crc ^= (uint16_t)value << 8;
    for (uint8_t bit = 0u; bit < 8u; bit++) {
        crc = (crc & 0x8000u) != 0u ?
              (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t roster_crc(const uint64_t *node_ids, size_t node_count)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0u; i < node_count; i++) {
        uint64_t node_id = node_ids[i];

        for (uint8_t byte = 0u; byte < sizeof(node_id); byte++) {
            crc = crc16_update(crc, (uint8_t)(node_id >> (8u * byte)));
        }
    }
    return crc;
}

static void put_u16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1u] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *buffer, size_t offset, uint32_t value)
{
    for (uint8_t i = 0u; i < sizeof(value); i++) {
        buffer[offset + i] = (uint8_t)(value >> (8u * i));
    }
}

static void put_u64(uint8_t *buffer, size_t offset, uint64_t value)
{
    for (uint8_t i = 0u; i < sizeof(value); i++) {
        buffer[offset + i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t get_u16(const uint8_t *buffer, size_t offset)
{
    return (uint16_t)buffer[offset] |
           ((uint16_t)buffer[offset + 1u] << 8);
}

static uint32_t get_u32(const uint8_t *buffer, size_t offset)
{
    uint32_t value = 0u;

    for (uint8_t i = 0u; i < sizeof(value); i++) {
        value |= (uint32_t)buffer[offset + i] << (8u * i);
    }
    return value;
}

static uint64_t get_u64(const uint8_t *buffer, size_t offset)
{
    uint64_t value = 0u;

    for (uint8_t i = 0u; i < sizeof(value); i++) {
        value |= (uint64_t)buffer[offset + i] << (8u * i);
    }
    return value;
}

static void finish_record(uint8_t *buffer, size_t record_len)
{
    put_u16(buffer,
            record_len - sizeof(uint16_t),
            proto_crc16_ccitt_false(buffer, record_len - sizeof(uint16_t)));
}

static bool record_crc_valid(const uint8_t *buffer, size_t record_len)
{
    return get_u16(buffer, record_len - sizeof(uint16_t)) ==
           proto_crc16_ccitt_false(buffer, record_len - sizeof(uint16_t));
}

static size_t encode_base(uint8_t *buffer,
                          const struct gateway_collection_state *collection,
                          uint64_t generation,
                          bool active)
{
    memset(buffer, 0, GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE);
    put_u32(buffer, 0u, JOURNAL_BASE_MAGIC);
    buffer[4] = GATEWAY_COLLECTION_JOURNAL_VERSION;
    buffer[5] = active ? JOURNAL_BASE_FLAG_ACTIVE : 0u;
    put_u16(buffer, 6u, GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE);
    put_u64(buffer, 8u, generation);
    if (active) {
        put_u64(buffer, 16u, collection->gateway_id);
        put_u32(buffer, 24u, collection->command_seq);
        put_u32(buffer, 28u, collection->collection_epoch_id);
        put_u16(buffer, 32u, collection->gateway_epoch);
        put_u16(buffer, 34u, collection->membership_epoch);
        put_u16(buffer, 36u, collection->expected_count);
        put_u16(buffer, 38u, collection->expected_node_id_count);
        put_u16(buffer,
                40u,
                roster_crc(collection->expected_node_ids,
                           collection->expected_node_id_count));
    }
    finish_record(buffer, GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE);
    return GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE;
}

static int decode_base(const uint8_t *buffer,
                       size_t buffer_len,
                       struct journal_base *base)
{
    uint8_t flags;

    if (buffer == NULL || base == NULL ||
        buffer_len != GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE ||
        get_u32(buffer, 0u) != JOURNAL_BASE_MAGIC ||
        buffer[4] != GATEWAY_COLLECTION_JOURNAL_VERSION ||
        get_u16(buffer, 6u) != buffer_len ||
        !record_crc_valid(buffer, buffer_len)) {
        return PROTO_ERR_MALFORMED;
    }
    flags = buffer[5];
    if ((flags & (uint8_t)~JOURNAL_BASE_FLAG_ACTIVE) != 0u ||
        get_u64(buffer, 8u) == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(base, 0, sizeof(*base));
    base->generation = get_u64(buffer, 8u);
    base->active = (flags & JOURNAL_BASE_FLAG_ACTIVE) != 0u;
    if (!base->active) {
        return PROTO_OK;
    }

    base->gateway_id = get_u64(buffer, 16u);
    base->command_seq = get_u32(buffer, 24u);
    base->collection_epoch_id = get_u32(buffer, 28u);
    base->gateway_epoch = get_u16(buffer, 32u);
    base->membership_epoch = get_u16(buffer, 34u);
    base->expected_count = get_u16(buffer, 36u);
    base->expected_node_id_count = get_u16(buffer, 38u);
    base->roster_crc = get_u16(buffer, 40u);
    if (base->gateway_id == 0u || base->command_seq == 0u ||
        base->collection_epoch_id == 0u || base->membership_epoch == 0u ||
        base->expected_count == 0u ||
        base->expected_count > GATEWAY_COLLECTION_RESULT_CACHE_SIZE ||
        base->expected_node_id_count > GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP ||
        (base->expected_node_id_count != 0u &&
         base->expected_node_id_count != base->expected_count)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static size_t encode_roster(uint8_t *buffer,
                            const struct gateway_collection_state *collection,
                            uint64_t generation,
                            uint8_t chunk_index)
{
    size_t start = (size_t)chunk_index *
                   GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP;
    size_t remaining = collection->expected_node_id_count - start;
    uint16_t count = (uint16_t)(remaining >
        GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP ?
        GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP : remaining);
    size_t record_len = 22u + ((size_t)count * sizeof(uint64_t));

    memset(buffer, 0, record_len);
    put_u32(buffer, 0u, JOURNAL_ROSTER_MAGIC);
    buffer[4] = GATEWAY_COLLECTION_JOURNAL_VERSION;
    buffer[5] = chunk_index;
    put_u16(buffer, 6u, (uint16_t)record_len);
    put_u64(buffer, 8u, generation);
    put_u16(buffer, 16u, (uint16_t)start);
    put_u16(buffer, 18u, count);
    for (uint16_t i = 0u; i < count; i++) {
        put_u64(buffer, 20u + ((size_t)i * sizeof(uint64_t)),
                collection->expected_node_ids[start + i]);
    }
    finish_record(buffer, record_len);
    return record_len;
}

static int decode_roster(const uint8_t *buffer,
                         size_t buffer_len,
                         uint64_t generation,
                         uint8_t chunk_index,
                         struct gateway_collection_state *collection)
{
    uint16_t start;
    uint16_t count;
    size_t expected_len;

    if (buffer == NULL || collection == NULL || buffer_len < 22u ||
        buffer_len > GATEWAY_COLLECTION_JOURNAL_ROSTER_RECORD_MAX_SIZE ||
        get_u32(buffer, 0u) != JOURNAL_ROSTER_MAGIC ||
        buffer[4] != GATEWAY_COLLECTION_JOURNAL_VERSION ||
        buffer[5] != chunk_index || get_u16(buffer, 6u) != buffer_len ||
        get_u64(buffer, 8u) != generation ||
        !record_crc_valid(buffer, buffer_len)) {
        return PROTO_ERR_MALFORMED;
    }
    start = get_u16(buffer, 16u);
    count = get_u16(buffer, 18u);
    expected_len = 22u + ((size_t)count * sizeof(uint64_t));
    if (start != (uint16_t)((size_t)chunk_index *
                            GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP) ||
        count == 0u || count > GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP ||
        expected_len != buffer_len ||
        (size_t)start + count > collection->expected_node_id_count) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint16_t i = 0u; i < count; i++) {
        collection->expected_node_ids[start + i] =
            get_u64(buffer, 20u + ((size_t)i * sizeof(uint64_t)));
    }
    return PROTO_OK;
}

static size_t encode_control(uint8_t *buffer,
                             const struct gateway_collection_state *collection,
                             uint64_t generation,
                             uint64_t committed_slots)
{
    uint8_t flags = 0u;

    memset(buffer, 0, GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE);
    put_u32(buffer, 0u, JOURNAL_CONTROL_MAGIC);
    buffer[4] = GATEWAY_COLLECTION_JOURNAL_VERSION;
    if (collection->collection_open) {
        flags |= JOURNAL_CONTROL_FLAG_OPEN;
    }
    if (collection->eack_pending) {
        flags |= JOURNAL_CONTROL_FLAG_EACK_PENDING;
    }
    buffer[5] = flags;
    put_u16(buffer, 6u, GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE);
    put_u64(buffer, 8u, generation);
    put_u64(buffer, 16u, committed_slots);
    buffer[24] = collection->retry_round;
    put_u16(buffer, 26u, collection->eack_sequence);
    put_u32(buffer, 28u, collection->next_retry_spread_ms);
    finish_record(buffer, GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE);
    return GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE;
}

static int decode_control(const uint8_t *buffer,
                          size_t buffer_len,
                          uint64_t generation,
                          struct journal_control *control)
{
    const uint64_t valid_mask =
        (UINT64_C(1) << GATEWAY_COLLECTION_RESULT_CACHE_SIZE) - 1u;
    uint8_t flags;

    if (buffer == NULL || control == NULL ||
        buffer_len != GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE ||
        get_u32(buffer, 0u) != JOURNAL_CONTROL_MAGIC ||
        buffer[4] != GATEWAY_COLLECTION_JOURNAL_VERSION ||
        get_u16(buffer, 6u) != buffer_len ||
        get_u64(buffer, 8u) != generation ||
        !record_crc_valid(buffer, buffer_len)) {
        return PROTO_ERR_MALFORMED;
    }
    flags = buffer[5];
    if ((flags & (uint8_t)~(JOURNAL_CONTROL_FLAG_OPEN |
                            JOURNAL_CONTROL_FLAG_EACK_PENDING)) != 0u ||
        (get_u64(buffer, 16u) & ~valid_mask) != 0u ||
        get_u16(buffer, 26u) == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(control, 0, sizeof(*control));
    control->generation = generation;
    control->committed_slots = get_u64(buffer, 16u);
    control->retry_round = buffer[24];
    control->eack_sequence = get_u16(buffer, 26u);
    control->next_retry_spread_ms = get_u32(buffer, 28u);
    control->collection_open = (flags & JOURNAL_CONTROL_FLAG_OPEN) != 0u;
    control->eack_pending =
        (flags & JOURNAL_CONTROL_FLAG_EACK_PENDING) != 0u;
    return PROTO_OK;
}

static size_t encode_result(uint8_t *buffer,
                            const struct gateway_collection_result_entry *entry,
                            uint64_t generation,
                            uint8_t slot)
{
    memset(buffer, 0, GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE);
    put_u32(buffer, 0u, JOURNAL_RESULT_MAGIC);
    buffer[4] = GATEWAY_COLLECTION_JOURNAL_VERSION;
    buffer[5] = slot;
    put_u16(buffer, 6u, GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE);
    put_u64(buffer, 8u, generation);
    put_u64(buffer, 16u, entry->id.node_id);
    put_u64(buffer, 24u, entry->previous_hop_id);
    put_u32(buffer, 32u, entry->id.node_boot_counter);
    put_u16(buffer, 36u, entry->id.result_seq);
    put_u16(buffer, 38u, entry->payload_crc);
    put_u16(buffer, 40u, entry->payload_len);
    finish_record(buffer, GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE);
    return GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE;
}

static int decode_result(const uint8_t *buffer,
                         size_t buffer_len,
                         uint64_t generation,
                         uint8_t slot,
                         struct gateway_collection_result_entry *entry)
{
    if (buffer == NULL || entry == NULL ||
        buffer_len != GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE ||
        get_u32(buffer, 0u) != JOURNAL_RESULT_MAGIC ||
        buffer[4] != GATEWAY_COLLECTION_JOURNAL_VERSION ||
        buffer[5] != slot || get_u16(buffer, 6u) != buffer_len ||
        get_u64(buffer, 8u) != generation ||
        !record_crc_valid(buffer, buffer_len)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(entry, 0, sizeof(*entry));
    entry->id.node_id = get_u64(buffer, 16u);
    entry->previous_hop_id = get_u64(buffer, 24u);
    entry->id.node_boot_counter = get_u32(buffer, 32u);
    entry->id.result_seq = get_u16(buffer, 36u);
    entry->payload_crc = get_u16(buffer, 38u);
    entry->payload_len = get_u16(buffer, 40u);
    entry->valid = true;
    return entry->id.node_id == 0u || entry->payload_len == 0u ?
           PROTO_ERR_MALFORMED : PROTO_OK;
}

static int journal_read(const struct gateway_collection_journal_io *io,
                        struct gateway_collection_journal_key key,
                        uint8_t *buffer,
                        size_t buffer_cap,
                        size_t *stored_len)
{
    if (io == NULL || io->read == NULL || stored_len == NULL) {
        return PROTO_ERR_ARG;
    }
    *stored_len = 0u;
    return io->read(io->ctx, key, buffer, buffer_cap, stored_len);
}

static int journal_write(const struct gateway_collection_journal_io *io,
                         struct gateway_collection_journal_key key,
                         const uint8_t *buffer,
                         size_t buffer_len,
                         struct gateway_collection_journal_stats *stats)
{
    int ret;

    if (stats != NULL) {
        stats->write_attempts++;
        stats->bytes_attempted += buffer_len;
    }
    ret = io->write(io->ctx, key, buffer, buffer_len);
    if (ret == 0 && stats != NULL) {
        stats->writes_committed++;
        stats->bytes_committed += buffer_len;
    }
    return ret;
}

static uint64_t collection_result_mask(
    const struct gateway_collection_state *collection)
{
    uint64_t mask = 0u;

    for (uint8_t slot = 0u; slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; slot++) {
        if (collection->results[slot].valid) {
            mask |= UINT64_C(1) << slot;
        }
    }
    return mask;
}

static uint16_t mask_count(uint64_t mask)
{
    uint16_t count = 0u;

    while (mask != 0u) {
        count += (uint16_t)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

static bool cursor_matches_collection(
    const struct gateway_collection_journal_cursor *cursor,
    const struct gateway_collection_state *collection)
{
    return cursor->active &&
           cursor->gateway_id == collection->gateway_id &&
           cursor->gateway_epoch == collection->gateway_epoch &&
           cursor->command_seq == collection->command_seq &&
           cursor->collection_epoch_id == collection->collection_epoch_id &&
           cursor->membership_epoch == collection->membership_epoch &&
           cursor->expected_count == collection->expected_count &&
           cursor->expected_node_id_count == collection->expected_node_id_count &&
           cursor->roster_crc == roster_crc(collection->expected_node_ids,
                                            collection->expected_node_id_count);
}

static void cursor_from_state(struct gateway_collection_journal_cursor *cursor,
                              const struct gateway_collection_state *collection,
                              uint64_t generation,
                              uint64_t committed_slots)
{
    memset(cursor, 0, sizeof(*cursor));
    cursor->generation = generation;
    cursor->committed_slots = committed_slots;
    cursor->gateway_id = collection->gateway_id;
    cursor->gateway_epoch = collection->gateway_epoch;
    cursor->command_seq = collection->command_seq;
    cursor->collection_epoch_id = collection->collection_epoch_id;
    cursor->membership_epoch = collection->membership_epoch;
    cursor->expected_count = collection->expected_count;
    cursor->expected_node_id_count = collection->expected_node_id_count;
    cursor->roster_crc = roster_crc(collection->expected_node_ids,
                                    collection->expected_node_id_count);
    cursor->retry_round = collection->retry_round;
    cursor->eack_sequence = collection->eack_sequence;
    cursor->next_retry_spread_ms = collection->next_retry_spread_ms;
    cursor->collection_open = collection->collection_open;
    cursor->eack_pending = collection->eack_pending;
    cursor->active = true;
    cursor->loaded = true;
}

static bool control_matches_cursor(
    const struct gateway_collection_journal_cursor *cursor,
    const struct gateway_collection_state *collection,
    uint64_t committed_slots)
{
    return cursor->committed_slots == committed_slots &&
           cursor->retry_round == collection->retry_round &&
           cursor->eack_sequence == collection->eack_sequence &&
           cursor->next_retry_spread_ms == collection->next_retry_spread_ms &&
           cursor->collection_open == collection->collection_open &&
           cursor->eack_pending == collection->eack_pending;
}

static int read_base_bank(const struct gateway_collection_journal_io *io,
                          uint8_t bank,
                          struct journal_base *base,
                          struct gateway_collection_journal_stats *stats)
{
    uint8_t buffer[GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE];
    size_t stored_len = 0u;
    int ret = journal_read(io,
                           (struct gateway_collection_journal_key) {
                               .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_BASE,
                               .bank = bank,
                           },
                           buffer,
                           sizeof(buffer),
                           &stored_len);

    if (ret == -ENOENT) {
        return ret;
    }
    if (ret < 0) {
        return ret;
    }
    ret = decode_base(buffer, stored_len, base);
    if (ret != PROTO_OK && stats != NULL) {
        stats->records_ignored++;
    }
    return ret;
}

static int restore_active_base(
    const struct gateway_collection_journal_io *io,
    const struct journal_base *base,
    struct gateway_collection_state *collection,
    struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_journal_stats *stats)
{
    uint8_t buffer[GATEWAY_COLLECTION_JOURNAL_RECORD_MAX_SIZE];
    struct journal_control control;
    uint8_t bank = (uint8_t)(base->generation & 1u);
    size_t stored_len = 0u;
    size_t roster_chunks;
    int ret;

    gateway_collection_clear(collection);
    collection->gateway_id = base->gateway_id;
    collection->gateway_epoch = base->gateway_epoch;
    collection->command_seq = base->command_seq;
    collection->collection_epoch_id = base->collection_epoch_id;
    collection->membership_epoch = base->membership_epoch;
    collection->expected_count = base->expected_count;
    collection->expected_node_id_count = base->expected_node_id_count;
    collection->persistence_version =
        GATEWAY_COLLECTION_STATE_PERSISTENCE_VERSION;
    collection->persistence_valid = true;

    roster_chunks = (collection->expected_node_id_count +
                     GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP - 1u) /
                    GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP;
    for (uint8_t chunk = 0u; chunk < roster_chunks; chunk++) {
        ret = journal_read(io,
                           (struct gateway_collection_journal_key) {
                               .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_ROSTER,
                               .bank = bank,
                               .index = chunk,
                           },
                           buffer,
                           sizeof(buffer),
                           &stored_len);
        if (ret < 0 || decode_roster(buffer,
                                     stored_len,
                                     base->generation,
                                     chunk,
                                     collection) != PROTO_OK) {
            if (ret >= 0 && stats != NULL) {
                stats->records_ignored++;
            }
            gateway_collection_clear(collection);
            return ret < 0 && ret != -ENOENT ? ret : PROTO_ERR_MALFORMED;
        }
    }
    if (roster_crc(collection->expected_node_ids,
                   collection->expected_node_id_count) != base->roster_crc) {
        if (stats != NULL) {
            stats->records_ignored++;
        }
        gateway_collection_clear(collection);
        return PROTO_ERR_MALFORMED;
    }

    ret = journal_read(io,
                       (struct gateway_collection_journal_key) {
                           .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL,
                           .bank = bank,
                       },
                       buffer,
                       sizeof(buffer),
                       &stored_len);
    if (ret < 0 || decode_control(buffer,
                                  stored_len,
                                  base->generation,
                                  &control) != PROTO_OK) {
        if (ret >= 0 && stats != NULL) {
            stats->records_ignored++;
        }
        gateway_collection_clear(collection);
        return ret < 0 && ret != -ENOENT ? ret : PROTO_ERR_MALFORMED;
    }

    for (uint8_t slot = 0u; slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; slot++) {
        if ((control.committed_slots & (UINT64_C(1) << slot)) == 0u) {
            continue;
        }
        ret = journal_read(io,
                           (struct gateway_collection_journal_key) {
                               .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT,
                               .bank = bank,
                               .index = slot,
                           },
                           buffer,
                           sizeof(buffer),
                           &stored_len);
        if (ret < 0 || decode_result(buffer,
                                     stored_len,
                                     base->generation,
                                     slot,
                                     &collection->results[slot]) != PROTO_OK) {
            if (ret >= 0 && stats != NULL) {
                stats->records_ignored++;
            }
            gateway_collection_clear(collection);
            return ret < 0 && ret != -ENOENT ? ret : PROTO_ERR_MALFORMED;
        }
        if (stats != NULL) {
            stats->results_replayed++;
        }
    }

    collection->received_count = mask_count(control.committed_slots);
    collection->retry_round = control.retry_round;
    collection->eack_sequence = control.eack_sequence;
    collection->next_retry_spread_ms = control.next_retry_spread_ms;
    collection->collection_open = control.collection_open;
    collection->eack_pending = control.eack_pending;
    ret = gateway_collection_state_validate(collection);
    if (ret != PROTO_OK) {
        gateway_collection_clear(collection);
        return ret;
    }
    cursor_from_state(cursor,
                      collection,
                      base->generation,
                      control.committed_slots);
    return PROTO_OK;
}

int gateway_collection_journal_save(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    const struct gateway_collection_state *collection,
    struct gateway_collection_journal_stats *stats)
{
    uint8_t buffer[GATEWAY_COLLECTION_JOURNAL_RECORD_MAX_SIZE];
    uint64_t committed_slots;
    uint64_t generation;
    uint64_t new_slots;
    uint8_t bank;
    bool same_collection;
    int ret;

    if (io == NULL || io->write == NULL || cursor == NULL ||
        collection == NULL || !cursor->loaded ||
        gateway_collection_state_validate(collection) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }

    committed_slots = collection_result_mask(collection);
    same_collection = cursor_matches_collection(cursor, collection);
    if (cursor->active && !same_collection && committed_slots != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (same_collection &&
        (cursor->committed_slots & ~committed_slots) != 0u) {
        return PROTO_ERR_MALFORMED;
    }

    if (same_collection) {
        generation = cursor->generation;
        bank = (uint8_t)(generation & 1u);
        new_slots = committed_slots & ~cursor->committed_slots;
        for (uint8_t slot = 0u;
             slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE;
             slot++) {
            size_t record_len;

            if ((new_slots & (UINT64_C(1) << slot)) == 0u) {
                continue;
            }
            record_len = encode_result(buffer,
                                       &collection->results[slot],
                                       generation,
                                       slot);
            ret = journal_write(io,
                                (struct gateway_collection_journal_key) {
                                    .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT,
                                    .bank = bank,
                                    .index = slot,
                                },
                                buffer,
                                record_len,
                                stats);
            if (ret < 0) {
                return ret;
            }
        }
        if (!control_matches_cursor(cursor, collection, committed_slots)) {
            size_t record_len = encode_control(buffer,
                                               collection,
                                               generation,
                                               committed_slots);

            ret = journal_write(io,
                                (struct gateway_collection_journal_key) {
                                    .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL,
                                    .bank = bank,
                                },
                                buffer,
                                record_len,
                                stats);
            if (ret < 0) {
                return ret;
            }
        }
        cursor_from_state(cursor, collection, generation, committed_slots);
        return PROTO_OK;
    }

    if (cursor->generation == UINT64_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    generation = cursor->generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    bank = (uint8_t)(generation & 1u);

    for (uint8_t chunk = 0u;
         (size_t)chunk * GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP <
             collection->expected_node_id_count;
         chunk++) {
        size_t record_len = encode_roster(buffer,
                                          collection,
                                          generation,
                                          chunk);

        ret = journal_write(io,
                            (struct gateway_collection_journal_key) {
                                .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_ROSTER,
                                .bank = bank,
                                .index = chunk,
                            },
                            buffer,
                            record_len,
                            stats);
        if (ret < 0) {
            return ret;
        }
    }
    for (uint8_t slot = 0u; slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; slot++) {
        size_t record_len;

        if ((committed_slots & (UINT64_C(1) << slot)) == 0u) {
            continue;
        }
        record_len = encode_result(buffer,
                                   &collection->results[slot],
                                   generation,
                                   slot);
        ret = journal_write(io,
                            (struct gateway_collection_journal_key) {
                                .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT,
                                .bank = bank,
                                .index = slot,
                            },
                            buffer,
                            record_len,
                            stats);
        if (ret < 0) {
            return ret;
        }
    }
    ret = journal_write(io,
                        (struct gateway_collection_journal_key) {
                            .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL,
                            .bank = bank,
                        },
                        buffer,
                        encode_control(buffer,
                                       collection,
                                       generation,
                                       committed_slots),
                        stats);
    if (ret < 0) {
        return ret;
    }
    ret = journal_write(io,
                        (struct gateway_collection_journal_key) {
                            .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_BASE,
                            .bank = bank,
                        },
                        buffer,
                        encode_base(buffer, collection, generation, true),
                        stats);
    if (ret < 0) {
        return ret;
    }

    cursor_from_state(cursor, collection, generation, committed_slots);
    return PROTO_OK;
}

int gateway_collection_journal_restore(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_state *collection,
    struct gateway_collection_journal_stats *stats)
{
    struct journal_base bases[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT];
    bool valid[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT] = {false};
    int first_error = 0;

    if (io == NULL || io->read == NULL || cursor == NULL || collection == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(cursor, 0, sizeof(*cursor));
    gateway_collection_clear(collection);

    for (uint8_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
        int ret = read_base_bank(io, bank, &bases[bank], stats);

        if (ret == PROTO_OK) {
            valid[bank] = true;
        } else if (ret != -ENOENT && ret != PROTO_ERR_MALFORMED && first_error == 0) {
            first_error = ret;
        }
    }
    if (first_error != 0) {
        return first_error;
    }
    if (!valid[0] && !valid[1]) {
        cursor->loaded = true;
        return PROTO_OK;
    }

    for (uint8_t attempt = 0u; attempt < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; attempt++) {
        int selected = -1;

        for (uint8_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
            if (valid[bank] &&
                (selected < 0 ||
                 bases[bank].generation > bases[(uint8_t)selected].generation)) {
                selected = bank;
            }
        }
        if (selected < 0) {
            break;
        }
        valid[(uint8_t)selected] = false;
        if (!bases[(uint8_t)selected].active) {
            cursor->generation = bases[(uint8_t)selected].generation;
            cursor->loaded = true;
            return PROTO_OK;
        }
        {
            int ret = restore_active_base(io,
                                          &bases[(uint8_t)selected],
                                          collection,
                                          cursor,
                                          stats);
            if (ret == PROTO_OK) {
                return PROTO_OK;
            }
            if (ret != PROTO_ERR_MALFORMED && ret != -ENOENT) {
                gateway_collection_clear(collection);
                memset(cursor, 0, sizeof(*cursor));
                return ret;
            }
            if (stats != NULL) {
                stats->records_ignored++;
            }
        }
    }

    gateway_collection_clear(collection);
    memset(cursor, 0, sizeof(*cursor));
    cursor->loaded = true;
    return PROTO_OK;
}

int gateway_collection_journal_clear(
    const struct gateway_collection_journal_io *io,
    struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_journal_stats *stats)
{
    uint8_t buffer[GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE];
    uint64_t generation;
    uint8_t bank;
    int ret;

    if (io == NULL || io->write == NULL || cursor == NULL || !cursor->loaded) {
        return PROTO_ERR_ARG;
    }
    if (!cursor->active) {
        return PROTO_OK;
    }
    if (cursor->generation == UINT64_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    generation = cursor->generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    bank = (uint8_t)(generation & 1u);
    ret = journal_write(io,
                        (struct gateway_collection_journal_key) {
                            .kind = GATEWAY_COLLECTION_JOURNAL_RECORD_BASE,
                            .bank = bank,
                        },
                        buffer,
                        encode_base(buffer, NULL, generation, false),
                        stats);
    if (ret < 0) {
        return ret;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->generation = generation;
    cursor->loaded = true;
    return PROTO_OK;
}

int gateway_collection_journal_rollback_uncommitted(
    const struct gateway_collection_journal_cursor *cursor,
    struct gateway_collection_state *collection)
{
    if (cursor == NULL || collection == NULL || !cursor->loaded ||
        !cursor_matches_collection(cursor, collection)) {
        return PROTO_ERR_ARG;
    }

    for (uint8_t slot = 0u; slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE; slot++) {
        if ((cursor->committed_slots & (UINT64_C(1) << slot)) == 0u) {
            memset(&collection->results[slot], 0, sizeof(collection->results[slot]));
        }
    }
    collection->received_count = mask_count(cursor->committed_slots);
    collection->retry_round = cursor->retry_round;
    collection->eack_sequence = cursor->eack_sequence;
    collection->next_retry_spread_ms = cursor->next_retry_spread_ms;
    collection->collection_open = cursor->collection_open;
    collection->eack_pending = cursor->eack_pending;
    return gateway_collection_state_validate(collection);
}
