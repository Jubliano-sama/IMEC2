#include "gateway_collection_journal.h"

#include "mesh.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TEST_GATEWAY_ID UINT64_C(0x9988776655443322)
#define TEST_NODE_BASE UINT64_C(0x1100000000000000)

struct fake_record {
    uint8_t data[GATEWAY_COLLECTION_JOURNAL_RECORD_MAX_SIZE];
    size_t len;
    uint32_t write_count;
    bool present;
};

struct fake_store {
    struct fake_record bases[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT];
    struct fake_record controls[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT];
    struct fake_record rosters[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT]
                              [GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT];
    struct fake_record results[GATEWAY_COLLECTION_JOURNAL_BANK_COUNT]
                              [GATEWAY_COLLECTION_RESULT_CACHE_SIZE];
    uint64_t bytes_written;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t fail_read_call;
    uint32_t fail_write_call;
    uint32_t tear_write_call;
};

static struct fake_store store;
static struct fake_store baseline_store;

static struct fake_record *fake_record_for(
    struct fake_store *fake,
    struct gateway_collection_journal_key key)
{
    switch (key.kind) {
    case GATEWAY_COLLECTION_JOURNAL_RECORD_BASE:
        return key.bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT && key.index == 0u ?
               &fake->bases[key.bank] : NULL;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_CONTROL:
        return key.bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT && key.index == 0u ?
               &fake->controls[key.bank] : NULL;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_ROSTER:
        return key.bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT &&
               key.index < GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT ?
               &fake->rosters[key.bank][key.index] : NULL;
    case GATEWAY_COLLECTION_JOURNAL_RECORD_RESULT:
        return key.bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT &&
               key.index < GATEWAY_COLLECTION_RESULT_CACHE_SIZE ?
               &fake->results[key.bank][key.index] : NULL;
    default:
        return NULL;
    }
}

static int fake_read(void *ctx,
                     struct gateway_collection_journal_key key,
                     void *data,
                     size_t data_cap,
                     size_t *stored_len)
{
    struct fake_store *fake = ctx;
    struct fake_record *record = fake_record_for(fake, key);
    size_t copy_len;

    if (record == NULL || stored_len == NULL) {
        return -EINVAL;
    }
    fake->read_calls++;
    if (fake->fail_read_call == fake->read_calls) {
        return -EIO;
    }
    if (!record->present) {
        return -ENOENT;
    }
    copy_len = record->len < data_cap ? record->len : data_cap;
    if (copy_len != 0u) {
        memcpy(data, record->data, copy_len);
    }
    *stored_len = record->len;
    return 0;
}

static int fake_write(void *ctx,
                      struct gateway_collection_journal_key key,
                      const void *data,
                      size_t data_len)
{
    struct fake_store *fake = ctx;
    struct fake_record *record = fake_record_for(fake, key);

    if (record == NULL || data == NULL || data_len > sizeof(record->data)) {
        return -EINVAL;
    }
    fake->write_calls++;
    if (fake->tear_write_call == fake->write_calls) {
        record->len = data_len / 2u;
        memcpy(record->data, data, record->len);
        record->present = true;
        record->write_count++;
        fake->bytes_written += record->len;
        return -EIO;
    }
    if (fake->fail_write_call == fake->write_calls) {
        return -EIO;
    }

    memcpy(record->data, data, data_len);
    record->len = data_len;
    record->present = true;
    record->write_count++;
    fake->bytes_written += data_len;
    return 0;
}

static const struct gateway_collection_journal_io fake_io = {
    .ctx = &store,
    .read = fake_read,
    .write = fake_write,
};

static uint64_t test_node_id(size_t index)
{
    return TEST_NODE_BASE + index + 1u;
}

static void start_collection(struct gateway_collection_state *collection,
                             size_t node_count,
                             uint32_t command_seq,
                             uint32_t collection_epoch_id)
{
    uint64_t roster[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP];

    assert(node_count != 0u &&
           node_count <= GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP);
    for (size_t i = 0u; i < node_count; i++) {
        roster[i] = test_node_id(i);
    }
    assert(gateway_collection_start(collection,
                                    TEST_GATEWAY_ID,
                                    7u,
                                    command_seq,
                                    collection_epoch_id,
                                    9u,
                                    (uint16_t)node_count,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_set_expected_roster(collection,
                                                  roster,
                                                  node_count) == PROTO_OK);
}

static void add_direct_result(struct gateway_collection_state *collection,
                              size_t slot)
{
    struct gateway_collection_result_entry *entry;

    assert(slot < collection->expected_node_id_count);
    assert(!collection->results[slot].valid);
    entry = &collection->results[slot];
    entry->id.node_id = collection->expected_node_ids[slot];
    entry->id.node_boot_counter = (uint32_t)(100u + slot);
    entry->id.result_seq = (uint16_t)(slot + 1u);
    entry->previous_hop_id = entry->id.node_id;
    memset(entry->payload_digest,
           (int)(0x40u + (uint8_t)slot),
           sizeof(entry->payload_digest));
    entry->payload_len = (uint16_t)(60u + slot);
    entry->valid = true;
    collection->received_count++;
    collection->collection_open =
        collection->received_count < collection->expected_count;
    collection->eack_pending = true;
    assert(gateway_collection_state_validate(collection) == PROTO_OK);
}

static void make_result_payload(uint8_t *payload,
                                size_t payload_cap,
                                size_t *payload_len,
                                const struct command_result_id *id,
                                uint32_t collection_epoch_id)
{
    *payload_len = 0u;
    assert(gateway_command_append_collection_result_identity(payload,
                                                            payload_cap,
                                                            payload_len,
                                                            id,
                                                            collection_epoch_id) == PROTO_OK);
    assert(mesh_append_command_result(payload,
                                      payload_cap,
                                      payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
}

static void make_crc16_colliding_result_payloads(
    uint8_t *first,
    size_t first_cap,
    size_t *first_len,
    uint8_t *second,
    size_t second_cap,
    size_t *second_len,
    const struct command_result_id *id,
    uint32_t collection_epoch_id)
{
    make_result_payload(first,
                        first_cap,
                        first_len,
                        id,
                        collection_epoch_id);
    assert(tlv_append_u16(first,
                          first_cap,
                          first_len,
                          TLV_FW_VERSION,
                          UINT16_C(0x3037)) == PROTO_OK);

    *second_len = 0u;
    assert(gateway_command_append_collection_result_identity(
               second,
               second_cap,
               second_len,
               id,
               collection_epoch_id) == PROTO_OK);
    assert(mesh_append_command_result(second,
                                      second_cap,
                                      second_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      1u) == PROTO_OK);
    assert(tlv_append_u16(second,
                          second_cap,
                          second_len,
                          TLV_FW_VERSION,
                          0u) == PROTO_OK);
    assert(*first_len == *second_len);
    assert(memcmp(first, second, *first_len) != 0);
    assert(proto_crc16_ccitt_false(first, *first_len) ==
           proto_crc16_ccitt_false(second, *second_len));
}

static struct proto_packet make_result_packet(const struct command_result_id *id,
                                              size_t payload_len)
{
    return (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = id->node_id,
        .dst_id = id->gateway_id,
        .session_id = id->command_seq,
        .seq = id->result_seq,
        .ttl = 1u,
        .payload_len = (uint16_t)payload_len,
    };
}

static uint32_t total_base_writes(const struct fake_store *fake)
{
    uint32_t count = 0u;

    for (size_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
        count += fake->bases[bank].write_count;
    }
    return count;
}

static uint32_t total_roster_writes(const struct fake_store *fake)
{
    uint32_t count = 0u;

    for (size_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
        for (size_t chunk = 0u;
             chunk < GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_COUNT;
             chunk++) {
            count += fake->rosters[bank][chunk].write_count;
        }
    }
    return count;
}

static uint32_t total_control_writes(const struct fake_store *fake)
{
    uint32_t count = 0u;

    for (size_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
        count += fake->controls[bank].write_count;
    }
    return count;
}

static uint32_t total_result_writes(const struct fake_store *fake)
{
    uint32_t count = 0u;

    for (size_t bank = 0u; bank < GATEWAY_COLLECTION_JOURNAL_BANK_COUNT; bank++) {
        for (size_t slot = 0u;
             slot < GATEWAY_COLLECTION_RESULT_CACHE_SIZE;
             slot++) {
            count += fake->results[bank][slot].write_count;
        }
    }
    return count;
}

static void test_scale_and_byte_accounting(size_t node_count)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_journal_stats stats = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    uint64_t initial_bytes;
    size_t expected_roster_chunks =
        (node_count + GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP - 1u) /
        GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP;

    memset(&store, 0, sizeof(store));
    start_collection(&collection,
                     node_count,
                     (uint32_t)(1000u + node_count),
                     (uint32_t)(3000u + node_count));
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           &stats) == PROTO_OK);
    initial_bytes = stats.bytes_committed;
    assert(total_base_writes(&store) == 1u);
    assert(total_roster_writes(&store) == expected_roster_chunks);
    assert(total_control_writes(&store) == 1u);
    assert(total_result_writes(&store) == 0u);

    for (size_t slot = 0u; slot < node_count; slot++) {
        uint64_t before = stats.bytes_committed;

        add_direct_result(&collection, slot);
        assert(gateway_collection_journal_save(&fake_io,
                                               &cursor,
                                               &collection,
                                               &stats) == PROTO_OK);
        assert(stats.bytes_committed - before ==
               GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE +
               GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE);
    }

    assert(total_base_writes(&store) == 1u);
    assert(total_roster_writes(&store) == expected_roster_chunks);
    assert(total_control_writes(&store) == node_count + 1u);
    assert(total_result_writes(&store) == node_count);
    assert(stats.bytes_committed == initial_bytes +
           node_count * (GATEWAY_COLLECTION_JOURNAL_RESULT_RECORD_SIZE +
                         GATEWAY_COLLECTION_JOURNAL_CONTROL_RECORD_SIZE));
    assert(stats.bytes_committed == store.bytes_written);
    assert(stats.bytes_committed * 16u < node_count *
           (uint64_t)GATEWAY_COLLECTION_STATE_SIZE);

    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_OK);
    assert(reboot_cursor.active);
    assert(restored.received_count == node_count);
    assert(!restored.collection_open);
    assert(restored.eack_pending);
    assert(gateway_collection_state_validate(&restored) == PROTO_OK);
}

static void test_result_power_cut_is_atomic(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_state collection;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2001u, 4001u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    baseline_store = store;

    for (uint32_t cut = 1u; cut <= 2u; cut++) {
        struct gateway_collection_journal_cursor attempt_cursor = cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state attempted = collection;
        struct gateway_collection_state restored;

        store = baseline_store;
        store.fail_write_call = store.write_calls + cut;
        add_direct_result(&attempted, 0u);
        assert(gateway_collection_journal_save(&fake_io,
                                               &attempt_cursor,
                                               &attempted,
                                               NULL) == -EIO);
        assert(attempt_cursor.committed_slots == 0u);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(reboot_cursor.active);
        assert(restored.received_count == 0u);
        assert(restored.collection_open);
    }

    store = baseline_store;
    store.tear_write_call = store.write_calls + 1u;
    {
        struct gateway_collection_journal_cursor attempt_cursor = cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state attempted = collection;
        struct gateway_collection_state restored;

        add_direct_result(&attempted, 0u);
        assert(gateway_collection_journal_save(&fake_io,
                                               &attempt_cursor,
                                               &attempted,
                                               NULL) == -EIO);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(restored.received_count == 0u);
        add_direct_result(&restored, 0u);
        store.tear_write_call = 0u;
        assert(gateway_collection_journal_save(&fake_io,
                                               &reboot_cursor,
                                               &restored,
                                               NULL) == PROTO_OK);
    }
}

static void test_transient_restore_failure_requires_reload(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2051u, 4051u);
    add_direct_result(&collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);

    store.fail_read_call = store.read_calls + 1u;
    memset(&restored, 0xA5, sizeof(restored));
    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == -EIO);
    assert(!reboot_cursor.loaded);
    assert(restored.gateway_id == 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &reboot_cursor,
                                           &collection,
                                           NULL) == PROTO_ERR_ARG);

    store.fail_read_call = 0u;
    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_OK);
    assert(reboot_cursor.loaded);
    assert(reboot_cursor.active);
    assert(restored.command_seq == collection.command_seq);
    assert(restored.received_count == 1u);
}

static void test_new_generation_power_cut_keeps_old_collection(void)
{
    struct gateway_collection_journal_cursor old_cursor = {.loaded = true};
    struct gateway_collection_state old_collection;
    struct gateway_collection_state new_collection;
    uint32_t staged_write_count;

    memset(&store, 0, sizeof(store));
    start_collection(&old_collection, 12u, 2101u, 4101u);
    add_direct_result(&old_collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &old_cursor,
                                           &old_collection,
                                           NULL) == PROTO_OK);
    baseline_store = store;
    start_collection(&new_collection, 32u, 2102u, 4102u);

    store = baseline_store;
    {
        struct gateway_collection_journal_cursor count_cursor = old_cursor;
        uint32_t before = store.write_calls;

        assert(gateway_collection_journal_save(&fake_io,
                                               &count_cursor,
                                               &new_collection,
                                               NULL) == PROTO_OK);
        staged_write_count = store.write_calls - before;
    }
    assert(staged_write_count == 6u);

    for (uint32_t cut = 1u; cut <= staged_write_count; cut++) {
        struct gateway_collection_journal_cursor attempt_cursor = old_cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state restored;

        store = baseline_store;
        store.fail_write_call = store.write_calls + cut;
        assert(gateway_collection_journal_save(&fake_io,
                                               &attempt_cursor,
                                               &new_collection,
                                               NULL) == -EIO);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(reboot_cursor.active);
        assert(restored.command_seq == old_collection.command_seq);
        assert(restored.collection_epoch_id == old_collection.collection_epoch_id);
        assert(restored.received_count == 1u);
    }

    store = baseline_store;
    store.tear_write_call = store.write_calls + staged_write_count;
    {
        struct gateway_collection_journal_cursor attempt_cursor = old_cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state restored;

        assert(gateway_collection_journal_save(&fake_io,
                                               &attempt_cursor,
                                               &new_collection,
                                               NULL) == -EIO);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(restored.command_seq == old_collection.command_seq);
        assert(restored.received_count == 1u);
    }

    store = baseline_store;
    {
        struct gateway_collection_journal_cursor committed_cursor = old_cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state restored;

        assert(gateway_collection_journal_save(&fake_io,
                                               &committed_cursor,
                                               &new_collection,
                                               NULL) == PROTO_OK);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(restored.command_seq == new_collection.command_seq);
        assert(restored.collection_epoch_id == new_collection.collection_epoch_id);
        assert(restored.received_count == 0u);
        assert(restored.collection_open);
        assert(store.results[old_cursor.generation & 1u][0].present);
        assert(!store.results[committed_cursor.generation & 1u][0].present);
    }
}

static void test_new_generation_corruption_preserves_old_results(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state old_collection;
    struct gateway_collection_state new_collection;
    struct gateway_collection_state restored;
    uint8_t new_bank;

    memset(&store, 0, sizeof(store));
    start_collection(&old_collection, 12u, 2121u, 4121u);
    add_direct_result(&old_collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &old_collection,
                                           NULL) == PROTO_OK);

    start_collection(&new_collection, 12u, 2122u, 4122u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &new_collection,
                                           NULL) == PROTO_OK);
    add_direct_result(&new_collection, 0u);
    memset(new_collection.results[0].payload_digest,
           0x5Au,
           sizeof(new_collection.results[0].payload_digest));
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &new_collection,
                                           NULL) == PROTO_OK);

    new_bank = (uint8_t)(cursor.generation & 1u);
    assert(store.controls[new_bank].present);
    store.controls[new_bank].data[0] ^= 0x80u;

    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_OK);
    assert(reboot_cursor.active);
    assert(restored.command_seq == old_collection.command_seq);
    assert(restored.collection_epoch_id == old_collection.collection_epoch_id);
    assert(restored.received_count == 1u);
    assert(semantic_digest_equal(restored.results[0].payload_digest,
                                 old_collection.results[0].payload_digest,
                                 sizeof(restored.results[0].payload_digest)));
}

static void test_only_committed_base_corruption_fails_closed(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    uint8_t active_bank;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2131u, 4131u);
    add_direct_result(&collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    active_bank = (uint8_t)(cursor.generation & 1u);
    assert(store.bases[active_bank].present);
    assert(store.bases[active_bank].len ==
           GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE);
    store.bases[active_bank].data[0] ^= 0x80u;

    memset(&restored, 0xA5, sizeof(restored));
    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_ERR_MALFORMED);
    assert(!reboot_cursor.loaded);
    assert(!reboot_cursor.active);
    assert(restored.gateway_id == 0u);
}

static void test_only_committed_child_corruption_fails_closed(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    uint8_t active_bank;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2132u, 4132u);
    add_direct_result(&collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    active_bank = (uint8_t)(cursor.generation & 1u);
    assert(store.controls[active_bank].present);
    store.controls[active_bank].data[0] ^= 0x80u;

    memset(&restored, 0xA5, sizeof(restored));
    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_ERR_MALFORMED);
    assert(!reboot_cursor.loaded);
    assert(!reboot_cursor.active);
    assert(restored.gateway_id == 0u);
}

static void test_short_uncommitted_initial_base_is_ignored(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    uint32_t roster_chunks;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2133u, 4133u);
    roster_chunks =
        (collection.expected_node_id_count +
         GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP - 1u) /
        GATEWAY_COLLECTION_JOURNAL_ROSTER_CHUNK_CAP;
    store.tear_write_call = roster_chunks + 2u;
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == -EIO);
    assert(store.bases[1].present);
    assert(store.bases[1].len <
           GATEWAY_COLLECTION_JOURNAL_BASE_RECORD_SIZE);

    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_OK);
    assert(reboot_cursor.loaded);
    assert(!reboot_cursor.active);
    assert(reboot_cursor.generation == 0u);
    assert(restored.gateway_id == 0u);
}

static void test_clear_tombstone_is_atomic(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_state collection;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2151u, 4151u);
    add_direct_result(&collection, 0u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    baseline_store = store;

    for (uint8_t failure_mode = 0u; failure_mode < 2u; failure_mode++) {
        struct gateway_collection_journal_cursor attempt_cursor = cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state restored;
        bool torn = failure_mode != 0u;

        store = baseline_store;
        if (torn) {
            store.tear_write_call = store.write_calls + 1u;
        } else {
            store.fail_write_call = store.write_calls + 1u;
        }
        assert(gateway_collection_journal_clear(&fake_io,
                                                &attempt_cursor,
                                                NULL) == -EIO);
        assert(attempt_cursor.active);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(reboot_cursor.active);
        assert(restored.command_seq == collection.command_seq);
        assert(restored.received_count == 1u);
    }

    store = baseline_store;
    {
        struct gateway_collection_journal_cursor cleared_cursor = cursor;
        struct gateway_collection_journal_cursor reboot_cursor = {0};
        struct gateway_collection_state restored;
        uint32_t writes_before_repeat;

        assert(gateway_collection_journal_clear(&fake_io,
                                                &cleared_cursor,
                                                NULL) == PROTO_OK);
        assert(!cleared_cursor.active);
        assert(cleared_cursor.generation == cursor.generation + 1u);
        assert(gateway_collection_journal_restore(&fake_io,
                                                  &reboot_cursor,
                                                  &restored,
                                                  NULL) == PROTO_OK);
        assert(!reboot_cursor.active);
        assert(reboot_cursor.generation == cleared_cursor.generation);
        assert(restored.gateway_id == 0u);

        writes_before_repeat = store.write_calls;
        assert(gateway_collection_journal_clear(&fake_io,
                                                &reboot_cursor,
                                                NULL) == PROTO_OK);
        assert(store.write_calls == writes_before_repeat);
    }
}

static void test_replay_preserves_exact_duplicate_semantics(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_journal_cursor reboot_cursor = {0};
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    struct command_result_id id = {
        .gateway_id = TEST_GATEWAY_ID,
        .gateway_epoch = 7u,
        .command_seq = 2201u,
        .node_id = test_node_id(0u),
        .node_boot_counter = 55u,
        .result_seq = 3u,
    };
    uint8_t payload[96];
    uint8_t conflicting[96];
    size_t payload_len;
    struct proto_packet packet;
    bool duplicate = false;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, id.command_seq, 4201u);
    {
        size_t conflicting_len = 0u;

        make_crc16_colliding_result_payloads(
            payload,
            sizeof(payload),
            &payload_len,
            conflicting,
            sizeof(conflicting),
            &conflicting_len,
            &id,
            collection.collection_epoch_id);
        assert(conflicting_len == payload_len);
    }
    packet = make_result_packet(&id, payload_len);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &packet,
                                                     payload,
                                                     payload_len,
                                                     id.node_id,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    assert(gateway_collection_journal_restore(&fake_io,
                                              &reboot_cursor,
                                              &restored,
                                              NULL) == PROTO_OK);

    duplicate = false;
    assert(gateway_collection_record_result_from_hop(&restored,
                                                     &packet,
                                                     payload,
                                                     payload_len,
                                                     id.node_id,
                                                     &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(restored.received_count == 1u);

    duplicate = false;
    assert(gateway_collection_record_result_from_hop(&restored,
                                                     &packet,
                                                     conflicting,
                                                     payload_len,
                                                     id.node_id,
                                                     &duplicate) == PROTO_ERR_MALFORMED);
    assert(!duplicate);
    assert(restored.received_count == 1u);
}

static void test_in_ram_rollback_removes_uncommitted_results(void)
{
    struct gateway_collection_journal_cursor cursor = {.loaded = true};
    struct gateway_collection_state collection;

    memset(&store, 0, sizeof(store));
    start_collection(&collection, 12u, 2301u, 4301u);
    assert(gateway_collection_journal_save(&fake_io,
                                           &cursor,
                                           &collection,
                                           NULL) == PROTO_OK);
    add_direct_result(&collection, 0u);
    assert(gateway_collection_journal_rollback_uncommitted(&cursor,
                                                           &collection) == PROTO_OK);
    assert(collection.received_count == 0u);
    assert(collection.collection_open);
    assert(collection.eack_pending);
    assert(!collection.results[0].valid);
}

int main(void)
{
    test_scale_and_byte_accounting(12u);
    test_scale_and_byte_accounting(32u);
    test_scale_and_byte_accounting(50u);
    test_result_power_cut_is_atomic();
    test_transient_restore_failure_requires_reload();
    test_new_generation_power_cut_keeps_old_collection();
    test_new_generation_corruption_preserves_old_results();
    test_only_committed_base_corruption_fails_closed();
    test_only_committed_child_corruption_fails_closed();
    test_short_uncommitted_initial_base_is_ignored();
    test_clear_tombstone_is_atomic();
    test_replay_preserves_exact_duplicate_semantics();
    test_in_ram_rollback_removes_uncommitted_results();
    puts("gateway collection journal tests passed");
    return 0;
}
