#include "app_durable_state.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define TEST_DEVICE_ID UINT64_C(0x1020304050607080)
#define TEST_GATEWAY_ID UINT64_C(0x9999888877776666)

struct fake_record {
    uint16_t id;
    uint8_t record[APP_DURABLE_STATE_MAX_RECORD_SIZE];
    size_t record_len;
    bool present;
};

struct fake_store {
    struct fake_record records[4];
    unsigned int mount_calls;
    unsigned int write_calls;
    uint16_t last_write_id;
    int read_error;
    int write_error;
};

static struct fake_record *find_record(struct fake_store *store,
                                       uint16_t id,
                                       bool allocate)
{
    struct fake_record *empty = NULL;

    for (size_t index = 0u;
         index < sizeof(store->records) / sizeof(store->records[0]);
         index++) {
        if (store->records[index].present &&
            store->records[index].id == id) {
            return &store->records[index];
        }
        if (!store->records[index].present && empty == NULL) {
            empty = &store->records[index];
        }
    }
    return allocate ? empty : NULL;
}

static struct fake_record *last_written_record(struct fake_store *store)
{
    struct fake_record *record = find_record(
        store, store->last_write_id, false);

    assert(record != NULL);
    return record;
}

static int fake_mount(void *context)
{
    struct fake_store *store = context;

    assert(store != NULL);
    store->mount_calls++;
    return 0;
}

static ssize_t fake_read(void *context,
                         uint16_t id,
                         void *data,
                         size_t len)
{
    struct fake_store *store = context;
    struct fake_record *record;

    assert(store != NULL);
    assert(data != NULL);
    if (store->read_error < 0) {
        return store->read_error;
    }
    record = find_record(store, id, false);
    if (record == NULL) {
        return -ENOENT;
    }
    if (len < record->record_len) {
        return -ENOSPC;
    }
    memcpy(data, record->record, record->record_len);
    return (ssize_t)record->record_len;
}

static ssize_t fake_write(void *context,
                          uint16_t id,
                          const void *data,
                          size_t len)
{
    struct fake_store *store = context;
    struct fake_record *record;

    assert(store != NULL);
    assert(data != NULL);
    if (store->write_error < 0) {
        return store->write_error;
    }
    record = find_record(store, id, true);
    if (record == NULL || len > sizeof(record->record)) {
        return -ENOSPC;
    }
    memcpy(record->record, data, len);
    record->id = id;
    record->record_len = len;
    record->present = true;
    store->last_write_id = id;
    store->write_calls++;
    return (ssize_t)len;
}

static int fake_erase(void *context, uint16_t id)
{
    struct fake_store *store = context;
    struct fake_record *record = find_record(store, id, false);

    if (record == NULL) {
        return -ENOENT;
    }
    memset(record, 0, sizeof(*record));
    return 0;
}

static void install_store(struct fake_store *store,
                          enum app_durable_state_role role)
{
    const struct app_durable_state_test_backend backend = {
        .context = store,
        .mount = fake_mount,
        .read = fake_read,
        .write = fake_write,
        .erase = fake_erase,
    };

    app_durable_state_test_reset();
    assert(app_durable_state_test_install_backend(&backend, role) == 0);
    assert(app_durable_state_init(TEST_DEVICE_ID) == 0);
    assert(app_durable_state_begin_boot() == 0);
}

static uint64_t gateway_reserve(struct fake_store *store)
{
    struct app_durable_state_reservation reservation = {0};

    install_store(store, APP_DURABLE_STATE_ROLE_GATEWAY);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               &reservation) == 0);
    assert(reservation.first == reservation.reserved_through);
    assert(reservation.first != 0u);
    assert((uint32_t)reservation.first != 0u);
    return reservation.first;
}

static void test_gateway_reboot_never_reuses_a_survey_generation(void)
{
    struct fake_store gateway_store = {0};
    uint64_t first_generation;
    uint64_t second_generation;

    first_generation = gateway_reserve(&gateway_store);
    assert(gateway_store.write_calls == 2u);

    /* Reset the sole owner while retaining its NVS image. */
    second_generation = gateway_reserve(&gateway_store);
    assert(second_generation > first_generation);
    assert(gateway_store.write_calls == 4u);
}

static void test_anchor_reboot_restores_exact_replay_and_rejects_rollback(void)
{
    struct fake_store gateway_store = {0};
    struct fake_store anchor_store = {0};
    uint64_t first_generation;
    uint64_t second_generation;
    uint64_t restored_generation = 0u;
    unsigned int writes;

    first_generation = gateway_reserve(&gateway_store);
    second_generation = gateway_reserve(&gateway_store);
    assert(second_generation > first_generation);

    install_store(&anchor_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               first_generation) == 0);
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               second_generation) == 0);

    /* Anchor-only reboot: restore before processing gateway traffic. */
    install_store(&anchor_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_restore_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               &restored_generation) == 1);
    assert(restored_generation == second_generation);

    writes = anchor_store.write_calls;
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               second_generation) == 0);
    assert(anchor_store.write_calls == writes);
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               first_generation) == -ESTALE);
    assert(anchor_store.write_calls == writes);
}

static void test_corruption_and_io_failure_are_not_first_install(void)
{
    struct fake_store gateway_store = {0};
    struct fake_store anchor_store = {0};
    struct fake_store failed_store = {0};
    struct app_durable_state_reservation reservation = {
        .first = UINT64_MAX,
        .reserved_through = UINT64_MAX,
    };
    uint64_t generation;
    uint64_t restored_generation = UINT64_MAX;
    unsigned int writes;

    generation = gateway_reserve(&gateway_store);
    last_written_record(&gateway_store)->record[0] ^= 1u;
    install_store(&gateway_store, APP_DURABLE_STATE_ROLE_GATEWAY);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               &reservation) == -EPROTO);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);

    install_store(&anchor_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               generation) == 0);
    last_written_record(&anchor_store)->record[
        APP_DURABLE_STATE_RECORD_HEADER_SIZE + sizeof(uint64_t)] ^= 1u;
    install_store(&anchor_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    writes = anchor_store.write_calls;
    assert(app_durable_state_restore_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               &restored_generation) == -EBADMSG);
    assert(restored_generation == 0u);
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               generation + 1u) == -EBADMSG);
    assert(anchor_store.write_calls == writes);

    install_store(&failed_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    writes = failed_store.write_calls;
    failed_store.write_error = -EIO;
    assert(app_durable_state_advance_high_water(
               APP_DURABLE_STATE_SURVEY_GENERATION,
               TEST_GATEWAY_ID,
               generation) == -EIO);
    assert(failed_store.write_calls == writes);
}

int main(void)
{
    test_gateway_reboot_never_reuses_a_survey_generation();
    test_anchor_reboot_restores_exact_replay_and_rejects_rollback();
    test_corruption_and_io_failure_are_not_first_install();
    return 0;
}
