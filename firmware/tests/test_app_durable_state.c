#include "app_durable_state.h"
#include "app_click_event_sequence.h"
#include "app_gateway_control_sequence.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_DEVICE_A UINT64_C(0x1020304050607080)
#define TEST_DEVICE_B UINT64_C(0x1020304050607081)
#define TEST_GATEWAY_ID UINT64_C(0x8877665544332211)
#define TEST_SLOT_COUNT 4u

#define TEST_SCHEMA_OFFSET 4u
#define TEST_TYPE_OFFSET 8u
#define TEST_ROLE_OFFSET 12u
#define TEST_CRC_OFFSET 26u
#define TEST_PAYLOAD_OFFSET APP_DURABLE_STATE_RECORD_HEADER_SIZE
#define TEST_HIGH_WATER_OFFSET \
    (APP_DURABLE_STATE_RECORD_HEADER_SIZE + sizeof(uint64_t))

#define TEST_PREFETCH_RETRY_1H_MS \
    (INT64_C(60) * INT64_C(60) * INT64_C(1000))
#define TEST_PREFETCH_RETRY_2H_MS (INT64_C(2) * TEST_PREFETCH_RETRY_1H_MS)
#define TEST_PREFETCH_RETRY_4H_MS (INT64_C(4) * TEST_PREFETCH_RETRY_1H_MS)
#define TEST_PREFETCH_RETRY_8H_MS (INT64_C(8) * TEST_PREFETCH_RETRY_1H_MS)

static int64_t test_uptime_ms;

int64_t k_uptime_get(void)
{
    return test_uptime_ms;
}

struct fake_slot {
    uint16_t id;
    uint8_t data[APP_DURABLE_STATE_MAX_RECORD_SIZE];
    size_t len;
    bool present;
};

struct fake_store {
    struct fake_slot slots[TEST_SLOT_COUNT];
    unsigned int mount_calls;
    unsigned int read_calls;
    unsigned int write_calls;
    int mount_error;
    int next_read_error;
    int next_write_error;
    bool leave_old_bytes_on_write;
    bool fail_read_after_write;
};

static void test_put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void test_put_u64(uint8_t *dst, uint64_t value)
{
    for (uint8_t i = 0u; i < sizeof(value); i++) {
        dst[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t test_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = UINT16_C(0xffff);

    for (size_t i = 0u; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            crc = (crc & UINT16_C(0x8000)) != 0u
                      ? (uint16_t)((crc << 1) ^ UINT16_C(0x1021))
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void test_slot_set_high_water(struct fake_slot *slot,
                                     uint64_t high_water)
{
    uint16_t crc;

    assert(slot != NULL);
    assert(slot->len == APP_DURABLE_STATE_COUNTER_RECORD_SIZE);
    test_put_u64(&slot->data[TEST_HIGH_WATER_OFFSET], high_water);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], 0u);
    crc = test_crc16(slot->data, slot->len);
    test_put_u16(&slot->data[TEST_CRC_OFFSET], crc);
}

static struct fake_slot *fake_find_slot(struct fake_store *store,
                                        uint16_t id,
                                        bool allocate)
{
    struct fake_slot *free_slot = NULL;

    for (size_t i = 0u; i < TEST_SLOT_COUNT; i++) {
        if (store->slots[i].present && store->slots[i].id == id) {
            return &store->slots[i];
        }
        if (!store->slots[i].present && free_slot == NULL) {
            free_slot = &store->slots[i];
        }
    }
    if (!allocate || free_slot == NULL) {
        return NULL;
    }
    free_slot->id = id;
    return free_slot;
}

static int fake_mount(void *context)
{
    struct fake_store *store = context;

    store->mount_calls++;
    return store->mount_error;
}

static ssize_t fake_read(void *context,
                         uint16_t id,
                         void *data,
                         size_t len)
{
    struct fake_store *store = context;
    struct fake_slot *slot;

    store->read_calls++;
    if (store->next_read_error != 0) {
        int error = store->next_read_error;

        store->next_read_error = 0;
        return error;
    }
    slot = fake_find_slot(store, id, false);
    if (slot == NULL) {
        return -ENOENT;
    }
    memcpy(data, slot->data, len < slot->len ? len : slot->len);
    return (ssize_t)slot->len;
}

static ssize_t fake_write(void *context,
                          uint16_t id,
                          const void *data,
                          size_t len)
{
    struct fake_store *store = context;
    struct fake_slot *slot;

    store->write_calls++;
    if (store->next_write_error != 0) {
        int error = store->next_write_error;

        store->next_write_error = 0;
        return error;
    }
    slot = fake_find_slot(store, id, true);
    assert(slot != NULL);
    if (store->leave_old_bytes_on_write) {
        store->leave_old_bytes_on_write = false;
        if (store->fail_read_after_write) {
            store->next_read_error = -ETIMEDOUT;
            store->fail_read_after_write = false;
        }
        return (ssize_t)len;
    }
    if (slot->present && slot->len == len &&
        memcmp(slot->data, data, len) == 0) {
        return 0;
    }
    assert(len <= sizeof(slot->data));
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->present = true;
    if (store->fail_read_after_write) {
        store->next_read_error = -ETIMEDOUT;
        store->fail_read_after_write = false;
    }
    return (ssize_t)len;
}

static int fake_erase(void *context, uint16_t id)
{
    struct fake_store *store = context;
    struct fake_slot *slot = fake_find_slot(store, id, false);

    if (slot == NULL) {
        return -ENOENT;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}

static void install_backend(struct fake_store *store,
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
}

static void install_store(struct fake_store *store,
                          enum app_durable_state_role role,
                          uint64_t device_id)
{
    uint32_t boot_incarnation = 0u;

    install_backend(store, role);
    assert(app_durable_state_init(device_id) == 0);
    assert(app_durable_state_ready());
    assert(app_durable_state_begin_boot() == 0);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation != 0u);
    assert((uint16_t)boot_incarnation != 0u);
}

static struct fake_slot *present_slot_for_type(
    struct fake_store *store,
    enum app_durable_state_record_type type)
{
    for (size_t i = 0u; i < TEST_SLOT_COUNT; i++) {
        struct fake_slot *slot = &store->slots[i];

        if (!slot->present || slot->len <= TEST_TYPE_OFFSET + 1u) {
            continue;
        }
        if (((uint16_t)slot->data[TEST_TYPE_OFFSET] |
             ((uint16_t)slot->data[TEST_TYPE_OFFSET + 1u] << 8)) ==
            (uint16_t)type) {
            return slot;
        }
    }
    assert(false);
    return NULL;
}

static void test_boot_checkpoint_is_deferred_and_idempotent(void)
{
    struct fake_store store = {0};
    struct app_durable_state_reservation reservation = {0};
    uint32_t boot_incarnation = UINT32_MAX;
    unsigned int writes;

    install_backend(&store, APP_DURABLE_STATE_ROLE_CLICKER);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_ready());
    assert(store.mount_calls == 1u);
    assert(store.write_calls == 0u);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == -EACCES);
    assert(boot_incarnation == 0u);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == -EACCES);
    assert(store.write_calls == 0u);

    assert(app_durable_state_begin_boot() == 0);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 2u);
    writes = store.write_calls;
    assert(writes == 1u);
    assert(app_durable_state_begin_boot() == 0);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 2u);
    assert(store.mount_calls == 1u);
    assert(store.write_calls == writes);
}

static void test_missing_first_install_and_idempotent_write(void)
{
    struct fake_store store = {0};
    struct fake_store command_store = {0};
    struct app_durable_state_reservation reservation = {0};
    uint64_t restored = UINT64_MAX;
    uint32_t boot_incarnation = 0u;
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    assert(store.mount_calls == 1u);
    assert(app_durable_state_restore_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE, 0u, &restored) == 0);
    assert(restored == 0u);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == 0);
    assert(reservation.first ==
           APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR + 1u);
    assert(reservation.reserved_through ==
           APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
               APP_DURABLE_STATE_CLICK_BLOCK_SIZE);
    assert(app_durable_state_restore_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE, 0u, &restored) == 1);
    assert(restored == reservation.reserved_through);

    writes = store.write_calls;
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               reservation.reserved_through) == 0);
    assert(store.write_calls == writes + 1u);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(store.mount_calls == 1u);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 2u);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(store.write_calls == writes);
    assert(app_durable_state_init(TEST_DEVICE_B) == -EACCES);

    install_store(&command_store,
                  APP_DURABLE_STATE_ROLE_GATEWAY,
                  TEST_DEVICE_A);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &reservation) == 0);
    assert(reservation.first ==
           APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR + 1u);
    assert(reservation.reserved_through ==
           APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR +
               APP_DURABLE_STATE_COMMAND_BLOCK_SIZE);
}

static void test_mount_and_ambiguous_write_fail_closed(void)
{
    struct fake_store store = {.mount_error = -EIO};
    struct fake_store interrupted_store = {0};
    struct app_durable_state_reservation reservation = {
        .first = UINT64_MAX,
        .reserved_through = UINT64_MAX,
    };
    uint32_t boot_incarnation = UINT32_MAX;

    install_backend(&store, APP_DURABLE_STATE_ROLE_CLICKER);
    assert(app_durable_state_init(TEST_DEVICE_A) == -EIO);
    assert(!app_durable_state_ready());
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == -EACCES);
    assert(boot_incarnation == 0u);

    memset(&store, 0, sizeof(store));
    store.leave_old_bytes_on_write = true;
    install_backend(&store, APP_DURABLE_STATE_ROLE_CLICKER);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_ready());
    assert(app_durable_state_begin_boot() == -EIO);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == -EACCES);
    assert(app_durable_state_begin_boot() == 0);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 2u);

    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR) == 0);
    store.leave_old_bytes_on_write = true;
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == -EIO);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);

    store.fail_read_after_write = true;
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == -ETIMEDOUT);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);

    interrupted_store.fail_read_after_write = true;
    install_backend(&interrupted_store, APP_DURABLE_STATE_ROLE_CLICKER);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_begin_boot() == -ETIMEDOUT);
    assert(app_durable_state_ready());
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_begin_boot() == 0);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 3u);
}

static void test_zero_wrap_and_exhaustion_boundaries(void)
{
    struct fake_store boot_store = {0};
    struct fake_store command_store = {0};
    struct fake_store click_store = {0};
    struct app_durable_state_reservation reservation = {0};
    uint32_t boot_incarnation = 0u;
    const uint32_t final_click_high_water =
        UINT32_MAX - (UINT32_MAX % APP_DURABLE_STATE_CLICK_BLOCK_SIZE);
    const uint32_t previous_click_high_water =
        final_click_high_water - APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
    const uint32_t final_command_high_water =
        UINT32_MAX -
        (UINT32_MAX % APP_DURABLE_STATE_COMMAND_BLOCK_SIZE);

    install_store(&boot_store,
                  APP_DURABLE_STATE_ROLE_ANCHOR,
                  TEST_DEVICE_A);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == 2u);
    reservation.first = UINT64_MAX;
    reservation.reserved_through = UINT64_MAX;
    assert(app_durable_state_reserve(APP_DURABLE_STATE_BOOT_INCARNATION,
                                     0u,
                                     &reservation) == -ENOTSUP);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_BOOT_INCARNATION, 0u, UINT16_MAX) == 0);
    install_store(&boot_store,
                  APP_DURABLE_STATE_ROLE_ANCHOR,
                  TEST_DEVICE_A);
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == 0);
    assert(boot_incarnation == UINT32_C(0x00010001));
    assert((uint16_t)boot_incarnation != 0u);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_BOOT_INCARNATION, 0u, UINT32_MAX) == 0);
    install_backend(&boot_store, APP_DURABLE_STATE_ROLE_ANCHOR);
    assert(app_durable_state_init(TEST_DEVICE_A) == 0);
    assert(app_durable_state_begin_boot() == -EOVERFLOW);
    assert(app_durable_state_ready());
    assert(app_durable_state_boot_incarnation(&boot_incarnation) == -EACCES);
    assert(boot_incarnation == 0u);

    install_store(&command_store,
                  APP_DURABLE_STATE_ROLE_GATEWAY,
                  TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               final_command_high_water) == 0);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &reservation) == -EOVERFLOW);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);

    install_store(&click_store,
                  APP_DURABLE_STATE_ROLE_CLICKER,
                  TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               previous_click_high_water) == 0);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == 0);
    assert(reservation.reserved_through == final_click_high_water);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               &reservation) == -EOVERFLOW);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE, 0u, 0u) == -EILSEQ);

}

static void test_gateway_blocks_skip_reboots(void)
{
    struct fake_store store = {0};
    struct app_durable_state_reservation first = {0};
    struct app_durable_state_reservation second = {0};

    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &first) == 0);
    assert(first.first ==
           APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR + 1u);

    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &second) == 0);
    assert(second.first == first.reserved_through + 1u);
    assert(second.reserved_through ==
           first.reserved_through +
               APP_DURABLE_STATE_COMMAND_BLOCK_SIZE);
}

static void test_legacy_256_gateway_command_record_requires_explicit_migration(
    void)
{
    struct fake_store store = {0};
    struct fake_slot *slot;
    struct app_durable_state_reservation reservation = {0};
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR +
                   APP_DURABLE_STATE_COMMAND_BLOCK_SIZE) == 0);
    slot = present_slot_for_type(
        &store, APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE);
    assert(slot != NULL);
    test_slot_set_high_water(
        slot, APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR + UINT64_C(256));
    writes = store.write_calls;

    assert(app_durable_state_restore_high_water(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &reservation.reserved_through) == -EILSEQ);
    assert(app_durable_state_reserve(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               &reservation) == -EILSEQ);
    assert(reservation.first == 0u);
    assert(reservation.reserved_through == 0u);
    assert(store.write_calls == writes);
}

static uint32_t gateway_control_first_sequence(void)
{
    return (uint32_t)APP_DURABLE_STATE_COMMAND_FIRST_INSTALL_FLOOR + 1u;
}

static void consume_gateway_control_sequences(uint32_t count,
                                              uint32_t *expected_sequence)
{
    uint32_t sequence = 0u;

    assert(expected_sequence != NULL);
    for (uint32_t index = 0u; index < count; index++) {
        assert(app_gateway_control_sequence_next(&sequence) == 0);
        assert(sequence == *expected_sequence);
        (*expected_sequence)++;
    }
}

static void test_gateway_control_sequence_interleaves_without_hot_writes(void)
{
    struct fake_store store = {0};
    uint32_t sequence = 0u;
    unsigned int writes;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    app_gateway_control_sequence_test_reset();
    writes = store.write_calls;
    assert(app_gateway_control_sequence_init() == 0);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    assert(app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET));
    assert(app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_FORCED_ROUTE_REFRESH_BUDGET));
    assert(app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_GENERIC_FLOOD_BUDGET));
    assert(!app_gateway_control_sequence_admission_available(0u));
    assert(!app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE -
            APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR + 1u));

    /* Ordinary control and route refresh share exactly one monotonic owner. */
    assert(app_gateway_control_sequence_next(&sequence) == 0);
    assert(sequence == gateway_control_first_sequence());
    assert(app_gateway_control_sequence_next(&sequence) == 0);
    assert(sequence == gateway_control_first_sequence() + 1u);
    assert(app_gateway_control_sequence_next(&sequence) == 0);
    assert(sequence == gateway_control_first_sequence() + 2u);
    assert(store.write_calls == writes);
}

static void test_gateway_control_sequence_receiptable_skip_is_ram_only(void)
{
    struct fake_store store = {0};
    uint32_t sequence = 0u;
    unsigned int writes;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    app_gateway_control_sequence_test_reset();
    assert(app_gateway_control_sequence_init() == 0);
    writes = store.write_calls;

    /*
     * The initial command block begins at 0x01000001. Consume through the
     * valid 0x0100ffff identity, then the receiptable API must burn
     * 0x01010000 and expose 0x01010001 without taking another NVS block.
     */
    for (uint32_t count = 0u; count < UINT16_MAX; count++) {
        assert(app_gateway_control_sequence_next(&sequence) == 0);
    }
    assert(sequence == UINT32_C(0x0100ffff));
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_next_receiptable(&sequence) == 0);
    assert(sequence == UINT32_C(0x01010001));
    assert(store.write_calls == writes);
}

static void test_gateway_control_sequence_prefetch_is_daily_and_admission_floor_is_protected(
    void)
{
    struct fake_store store = {0};
    uint32_t expected = gateway_control_first_sequence();
    uint32_t sequence = UINT32_MAX;
    unsigned int writes;
    const uint32_t half_block =
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE / 2u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    app_gateway_control_sequence_test_reset();
    assert(app_gateway_control_sequence_init() == 0);
    writes = store.write_calls;
    consume_gateway_control_sequences(half_block, &expected);
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS - 1u) ==
           -EAGAIN);
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS) == 0);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    /* Proven standby capacity counts with active capacity for new admission. */
    assert(app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE + half_block -
            APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR));
    assert(!app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE + half_block -
            APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR + 1u));

    /* Existing accepted work can spend the floor; only new admission stops. */
    consume_gateway_control_sequences(half_block, &expected);
    consume_gateway_control_sequences(
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE -
            APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR,
        &expected);
    assert(!app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_GENERIC_FLOOD_BUDGET));
    assert(app_gateway_control_sequence_next(&sequence) == 0);
    assert(sequence == expected);
    expected++;
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               UINT32_C(2) *
                   APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS -
                   1u) == -EAGAIN);
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               UINT32_C(2) *
                   APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS) == 0);
    assert(store.write_calls == writes + 1u);
}

static void test_gateway_control_sequence_refill_failure_is_rate_limited(void)
{
    struct fake_store store = {0};
    uint32_t expected = gateway_control_first_sequence();
    uint32_t sequence = UINT32_MAX;
    unsigned int writes;
    const uint32_t half_block =
        APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE / 2u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    app_gateway_control_sequence_test_reset();
    assert(app_gateway_control_sequence_init() == 0);
    consume_gateway_control_sequences(half_block, &expected);
    writes = store.write_calls;

    store.next_write_error = -EIO;
    assert(app_gateway_control_sequence_test_maintain_at(
               APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS) == -EIO);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;
    consume_gateway_control_sequences(
        half_block - APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR,
        &expected);
    assert(!app_gateway_control_sequence_admission_available(
        APP_GATEWAY_CONTROL_SEQUENCE_GENERIC_FLOOD_BUDGET));
    assert(app_gateway_control_sequence_next(&sequence) == 0);
    assert(sequence == expected);
    expected++;
    consume_gateway_control_sequences(
        APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR - 1u, &expected);
    assert(app_gateway_control_sequence_next(&sequence) == -EAGAIN);
    assert(sequence == 0u);
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS + 1u) ==
           -EAGAIN);
    assert(store.write_calls == writes);
    assert(app_gateway_control_sequence_test_maintain_at(
               UINT32_C(2) *
                   APP_GATEWAY_CONTROL_SEQUENCE_REFILL_INTERVAL_MS) == 0);
    assert(store.write_calls == writes + 1u);
}

static void test_gateway_control_sequence_reboot_skips_ram_blocks(void)
{
    struct fake_store store = {0};
    uint32_t first = 0u;
    uint32_t second = 0u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    app_gateway_control_sequence_test_reset();
    assert(app_gateway_control_sequence_init() == 0);
    assert(app_gateway_control_sequence_next(&first) == 0);

    app_gateway_control_sequence_test_reset();
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_gateway_control_sequence_init() == 0);
    assert(app_gateway_control_sequence_next(&second) == 0);
    assert(second == first + APP_GATEWAY_CONTROL_SEQUENCE_BLOCK_SIZE);
}

static void test_gateway_control_sequence_final_partial_block_fails_closed(void)
{
    struct fake_store store = {0};
    const uint32_t final_high_water =
        UINT32_MAX -
        (UINT32_MAX % APP_DURABLE_STATE_COMMAND_BLOCK_SIZE);

    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE,
               0u,
               final_high_water) == 0);
    app_gateway_control_sequence_test_reset();
    assert(app_gateway_control_sequence_init() == -EOVERFLOW);
}

static void test_boot_incarnation_advances_across_one_sided_reboots(void)
{
    struct fake_store store = {0};
    uint32_t first_boot = 0u;
    uint32_t second_boot = 0u;
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_boot_incarnation(&first_boot) == 0);
    writes = store.write_calls;
    for (uint32_t read = 0u; read < 16u; read++) {
        uint32_t repeated = 0u;

        assert(app_durable_state_boot_incarnation(&repeated) == 0);
        assert(repeated == first_boot);
        assert(store.write_calls == writes);
    }

    /* Only this gateway restarts; every warm peer can order the new epoch. */
    install_store(&store, APP_DURABLE_STATE_ROLE_GATEWAY, TEST_DEVICE_A);
    assert(app_durable_state_boot_incarnation(&second_boot) == 0);
    assert(second_boot == first_boot + 1u);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;
    assert(app_durable_state_boot_incarnation(&second_boot) == 0);
    assert(store.write_calls == writes);
}

static void test_click_active_and_standby_blocks_keep_next_flash_free(void)
{
    struct fake_store store = {0};
    uint32_t event_seq = 0u;
    unsigned int startup_writes;
    unsigned int writes;
    const uint32_t half_block = APP_DURABLE_STATE_CLICK_BLOCK_SIZE / 2u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    app_click_event_sequence_test_reset();
    startup_writes = store.write_calls;
    assert(app_click_event_sequence_init() == 0);
    writes = store.write_calls;
    assert(writes == startup_writes + 1u);
    assert(app_click_event_sequence_init() == 0);
    assert(store.write_calls == writes);

    for (uint32_t index = 0u;
         index < half_block;
         index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(event_seq ==
                   (uint32_t)APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
                   1u + index);
        assert(store.write_calls == writes);
    }
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    for (uint32_t index = half_block;
         index < APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
         index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(event_seq ==
               (uint32_t)APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
                   1u + index);
        assert(store.write_calls == writes);
    }
    assert(app_click_event_sequence_next(&event_seq) == 0);
    assert(event_seq ==
           (uint32_t)APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
               APP_DURABLE_STATE_CLICK_BLOCK_SIZE + 1u);
    assert(store.write_calls == writes);
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);

    /* A reset abandons both RAM blocks and reserves a fresh durable block. */
    app_click_event_sequence_test_reset();
    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    assert(app_click_event_sequence_init() == 0);
    assert(app_click_event_sequence_next(&event_seq) == 0);
    assert(event_seq ==
           (uint32_t)APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
               (2u * APP_DURABLE_STATE_CLICK_BLOCK_SIZE) + 1u);
}

static void test_click_prefetch_retries_without_admission_writes(void)
{
    struct fake_store store = {0};
    uint32_t event_seq = UINT32_MAX;
    unsigned int writes;
    const uint32_t half_block = APP_DURABLE_STATE_CLICK_BLOCK_SIZE / 2u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    app_click_event_sequence_test_reset();
    assert(app_click_event_sequence_init() == 0);
    writes = store.write_calls;
    for (uint32_t index = 0u; index < half_block; index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(store.write_calls == writes);
    }

    store.next_write_error = -EIO;
    assert(app_click_event_sequence_maintain() == -EIO);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;
    for (uint32_t index = half_block;
         index < APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
         index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(store.write_calls == writes);
    }
    event_seq = UINT32_MAX;
    assert(app_click_event_sequence_next(&event_seq) == -EAGAIN);
    assert(event_seq == 0u);
    assert(store.write_calls == writes);

    test_uptime_ms = TEST_PREFETCH_RETRY_1H_MS - 1;
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);
    test_uptime_ms = TEST_PREFETCH_RETRY_1H_MS;
    store.next_write_error = -EIO;
    assert(app_click_event_sequence_maintain() == -EIO);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    test_uptime_ms = TEST_PREFETCH_RETRY_1H_MS +
                     TEST_PREFETCH_RETRY_2H_MS - 1;
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);
    test_uptime_ms++;
    store.next_write_error = -EIO;
    assert(app_click_event_sequence_maintain() == -EIO);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    test_uptime_ms += TEST_PREFETCH_RETRY_4H_MS - 1;
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);
    test_uptime_ms++;
    store.next_write_error = -EIO;
    assert(app_click_event_sequence_maintain() == -EIO);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    test_uptime_ms += TEST_PREFETCH_RETRY_8H_MS - 1;
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);
    test_uptime_ms++;
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes + 1u);
    writes = store.write_calls;

    assert(app_click_event_sequence_next(&event_seq) == 0);
    assert(event_seq ==
           (uint32_t)APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
               APP_DURABLE_STATE_CLICK_BLOCK_SIZE + 1u);
    assert(store.write_calls == writes);
}

static void test_click_final_block_exhausts_without_identity_reuse(void)
{
    struct fake_store store = {0};
    const uint32_t final_high_water =
        UINT32_MAX - (UINT32_MAX % APP_DURABLE_STATE_CLICK_BLOCK_SIZE);
    const uint32_t previous_high_water =
        final_high_water - APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
    uint32_t event_seq = 0u;
    unsigned int writes;
    const uint32_t half_block = APP_DURABLE_STATE_CLICK_BLOCK_SIZE / 2u;

    test_uptime_ms = 0;
    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               previous_high_water) == 0);
    app_click_event_sequence_test_reset();
    assert(app_click_event_sequence_init() == 0);
    writes = store.write_calls;
    for (uint32_t index = 0u;
         index < half_block;
         index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(event_seq ==
               previous_high_water + 1u + index);
        assert(store.write_calls == writes);
    }
    assert(app_click_event_sequence_maintain() == -EOVERFLOW);
    assert(store.write_calls == writes);
    assert(app_click_event_sequence_maintain() == 0);
    assert(store.write_calls == writes);
    for (uint32_t index = half_block;
         index < APP_DURABLE_STATE_CLICK_BLOCK_SIZE;
         index++) {
        assert(app_click_event_sequence_next(&event_seq) == 0);
        assert(event_seq == previous_high_water + 1u + index);
        assert(store.write_calls == writes);
    }
    assert(event_seq == final_high_water);
    event_seq = UINT32_MAX;
    assert(app_click_event_sequence_next(&event_seq) == -EOVERFLOW);
    assert(event_seq == 0u);
    assert(store.write_calls == writes);
}

static void test_legacy_256_aligned_click_record_requires_explicit_migration(void)
{
    struct fake_store store = {0};
    struct fake_slot *slot;
    uint32_t event_seq = UINT32_MAX;
    unsigned int writes;

    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR +
                   APP_DURABLE_STATE_CLICK_BLOCK_SIZE) == 0);
    slot = present_slot_for_type(
        &store, APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE);
    assert(slot->data[TEST_SCHEMA_OFFSET] ==
           APP_DURABLE_STATE_ENVELOPE_VERSION);
    test_slot_set_high_water(
        slot, APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR + UINT64_C(256));
    writes = store.write_calls;

    app_click_event_sequence_test_reset();
    assert(app_click_event_sequence_init() == -EILSEQ);
    assert(store.write_calls == writes);
    assert(app_click_event_sequence_next(&event_seq) == -EACCES);
    assert(event_seq == 0u);
}

static void test_legacy_click_record_fails_closed_until_repaired(void)
{
    struct fake_store store = {0};
    struct fake_slot *slot;
    uint32_t event_seq = UINT32_MAX;

    install_store(&store, APP_DURABLE_STATE_ROLE_CLICKER, TEST_DEVICE_A);
    assert(app_durable_state_test_seed_high_water(
               APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE,
               0u,
               APP_DURABLE_STATE_CLICK_FIRST_INSTALL_FLOOR) == 0);
    slot = present_slot_for_type(
        &store, APP_DURABLE_STATE_CLICK_EVENT_SEQUENCE);
    slot->len = 12u; /* Pre-migration click-event record size. */
    app_click_event_sequence_test_reset();
    assert(app_click_event_sequence_init() == -EILSEQ);
    assert(app_click_event_sequence_next(&event_seq) == -EACCES);
    assert(event_seq == 0u);
}

int main(void)
{
    test_boot_checkpoint_is_deferred_and_idempotent();
    test_missing_first_install_and_idempotent_write();
    test_mount_and_ambiguous_write_fail_closed();
    test_zero_wrap_and_exhaustion_boundaries();
    test_gateway_blocks_skip_reboots();
    test_legacy_256_gateway_command_record_requires_explicit_migration();
    test_gateway_control_sequence_interleaves_without_hot_writes();
    test_gateway_control_sequence_receiptable_skip_is_ram_only();
    test_gateway_control_sequence_prefetch_is_daily_and_admission_floor_is_protected();
    test_gateway_control_sequence_refill_failure_is_rate_limited();
    test_gateway_control_sequence_reboot_skips_ram_blocks();
    test_gateway_control_sequence_final_partial_block_fails_closed();
    test_boot_incarnation_advances_across_one_sided_reboots();
    test_click_active_and_standby_blocks_keep_next_flash_free();
    test_click_prefetch_retries_without_admission_writes();
    test_click_final_block_exhausts_without_identity_reuse();
    test_legacy_256_aligned_click_record_requires_explicit_migration();
    test_legacy_click_record_fails_closed_until_repaired();
    return 0;
}
